// X11 display backend: paints the wallpaper into the root window's background
// pixmap (the convention feh/hsetroot/xwallpaper use) and advertises it via the
// _XROOTPMAP_ID / ESETROOT_PMAP_ID properties so a running compositor (picom)
// picks it up. One of the backends behind the bg_display interface (display.h);
// selected on a plain X11 server when no Wayland session is present.
//
// Multi-monitor: RandR enumerates the CRTCs; each output renders into its
// sub-rectangle of a single root-sized pixmap. The daemon owns the pixmap for
// its lifetime (no RetainPermanent / kill-client dance — that one-shot model is
// for tools that exit, whereas this is a long-running daemon).
//
// This v1 swaps the image instantly; there is no crossfade yet, so the upload
// path uses chunked xcb_put_image (the wallpaper changes only on user commands,
// not per frame). A timer-driven fade can be added later via the tick/timeout
// hooks (left NULL here) and XShm.

#include "backend.h"
#include "display.h"
#include "image.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

#include <xcb/xcb.h>
#include <xcb/randr.h>

#ifdef BG_BACKEND_X11

typedef struct {
    char    *name;
    int16_t  x, y;
    uint16_t w, h;
    bool      has_override;
    bg_source override;
} x11_output;

typedef struct {
    xcb_connection_t *conn;
    xcb_window_t      root;
    xcb_gcontext_t    gc;
    uint8_t           depth;
    uint16_t          root_w, root_h;

    xcb_atom_t atom_xrootpmap;   // _XROOTPMAP_ID
    xcb_atom_t atom_esetroot;    // ESETROOT_PMAP_ID

    uint8_t    randr_event_base; // 0 if RandR unavailable
    bool       have_randr;

    xcb_pixmap_t cur_pixmap;     // pixmap currently set as the root background

    bg_source    global;         // global source of truth
    x11_output  *outputs;
    size_t       n_outputs;

    bool closed;
} x11_state;

// ---------- source ownership ----------

static void src_clear(bg_source *s) {
    if (s->kind == BG_SRC_IMAGE) bg_image_free(&s->image);
    s->kind = BG_SRC_NONE;
    s->color_rrggbb = 0;
    s->image = (bg_image){0};
    s->file_path = NULL;
}

static bool src_copy(bg_source *dst, const bg_source *src) {
    src_clear(dst);
    dst->kind = src->kind;
    dst->color_rrggbb = src->color_rrggbb;
    if (src->kind == BG_SRC_IMAGE && src->image.pixels) {
        size_t n = (size_t)src->image.width * src->image.height * 4;
        uint8_t *p = malloc(n);
        if (!p) return false;
        memcpy(p, src->image.pixels, n);
        dst->image.pixels = p;
        dst->image.width  = src->image.width;
        dst->image.height = src->image.height;
    }
    return true;
}

// ---------- outputs ----------

static void free_outputs(x11_state *s) {
    for (size_t i = 0; i < s->n_outputs; i++) {
        free(s->outputs[i].name);
        src_clear(&s->outputs[i].override);
    }
    free(s->outputs);
    s->outputs = NULL;
    s->n_outputs = 0;
}

static void add_output(x11_state *s, const char *name, int16_t x, int16_t y,
                       uint16_t w, uint16_t h) {
    x11_output *na = realloc(s->outputs, (s->n_outputs + 1) * sizeof(*na));
    if (!na) return;
    s->outputs = na;
    x11_output *o = &s->outputs[s->n_outputs++];
    memset(o, 0, sizeof(*o));
    o->name = strdup(name ? name : "?");
    o->x = x; o->y = y; o->w = w; o->h = h;
}

static void refresh_root_size(x11_state *s) {
    xcb_get_geometry_reply_t *g =
        xcb_get_geometry_reply(s->conn, xcb_get_geometry(s->conn, s->root), NULL);
    if (g) { s->root_w = g->width; s->root_h = g->height; free(g); }
}

// (Re)build the output geometry list from RandR. Per-output overrides are reset;
// the global source is re-applied to every output by the caller's commit.
static void enumerate_outputs(x11_state *s) {
    free_outputs(s);
    refresh_root_size(s);

    if (s->have_randr) {
        xcb_randr_get_screen_resources_current_reply_t *res =
            xcb_randr_get_screen_resources_current_reply(
                s->conn, xcb_randr_get_screen_resources_current(s->conn, s->root), NULL);
        if (res) {
            xcb_randr_output_t *outs = xcb_randr_get_screen_resources_current_outputs(res);
            int n = xcb_randr_get_screen_resources_current_outputs_length(res);
            xcb_timestamp_t cfg = res->config_timestamp;
            for (int i = 0; i < n; i++) {
                xcb_randr_get_output_info_reply_t *oi = xcb_randr_get_output_info_reply(
                    s->conn, xcb_randr_get_output_info(s->conn, outs[i], cfg), NULL);
                if (!oi) continue;
                if (oi->crtc != 0 && oi->connection == XCB_RANDR_CONNECTION_CONNECTED) {
                    xcb_randr_get_crtc_info_reply_t *ci = xcb_randr_get_crtc_info_reply(
                        s->conn, xcb_randr_get_crtc_info(s->conn, oi->crtc, cfg), NULL);
                    if (ci && ci->width && ci->height) {
                        int len = xcb_randr_get_output_info_name_length(oi);
                        uint8_t *nm = xcb_randr_get_output_info_name(oi);
                        char namebuf[128];
                        int cl = (len < (int)sizeof(namebuf) - 1) ? len : (int)sizeof(namebuf) - 1;
                        memcpy(namebuf, nm, cl);
                        namebuf[cl] = '\0';
                        add_output(s, namebuf, ci->x, ci->y, ci->width, ci->height);
                    }
                    free(ci);
                }
                free(oi);
            }
            free(res);
        }
    }

    // Fallback: no RandR, or no connected CRTCs — treat the whole root as one
    // output so single-head / headless setups still work.
    if (s->n_outputs == 0)
        add_output(s, "X11", 0, 0, s->root_w, s->root_h);
}

static x11_output *find_output(x11_state *s, const char *name) {
    for (size_t i = 0; i < s->n_outputs; i++)
        if (s->outputs[i].name && strcmp(s->outputs[i].name, name) == 0) return &s->outputs[i];
    return NULL;
}

// ---------- rendering ----------

// Render a source into the rect (rx,ry,rw,rh) of the root-sized XRGB8888 buffer,
// clipped to the buffer bounds. Reuses the shared cover-fit / blit helpers, so
// the pixel format matches the wlr backend exactly.
static void render_into_rect(uint32_t *buf, int W, int H,
                             int rx, int ry, int rw, int rh, const bg_source *src) {
    if (rw <= 0 || rh <= 0) return;
    uint32_t *sub = malloc((size_t)rw * rh * 4);
    if (!sub) return;
    switch (src->kind) {
    case BG_SRC_COLOR:
        bg_image_fill_xrgb8888(src->color_rrggbb, sub, rw, rh);
        break;
    case BG_SRC_IMAGE: {
        bg_image scaled = {0};
        if (src->image.pixels && bg_image_cover(&src->image, rw, rh, &scaled)) {
            bg_image_blit_xrgb8888(&scaled, sub, rw, rh);
            bg_image_free(&scaled);
        } else {
            bg_image_fill_xrgb8888(0x202020, sub, rw, rh);
        }
        break;
    }
    case BG_SRC_NONE:
    default:
        bg_image_fill_xrgb8888(0x000000, sub, rw, rh);
        break;
    }
    for (int y = 0; y < rh; y++) {
        int dy = ry + y;
        if (dy < 0 || dy >= H) continue;
        int dx0 = rx, sx0 = 0, cols = rw;
        if (dx0 < 0) { sx0 = -dx0; cols -= sx0; dx0 = 0; }
        if (dx0 + cols > W) cols = W - dx0;
        if (cols <= 0) continue;
        memcpy(&buf[(size_t)dy * W + dx0], &sub[(size_t)y * rw + sx0], (size_t)cols * 4);
    }
    free(sub);
}

// Upload a full root-sized buffer to `pix`, chunked to stay under the server's
// maximum request length.
static void upload_pixmap(x11_state *s, xcb_pixmap_t pix, const uint32_t *buf, int W, int H) {
    uint32_t stride = (uint32_t)W * 4;
    uint64_t max_req_bytes = (uint64_t)xcb_get_maximum_request_length(s->conn) * 4;
    uint64_t budget = (max_req_bytes > 256) ? (max_req_bytes - 256) : stride;
    int rows_per = (int)(budget / stride);
    if (rows_per < 1) rows_per = 1;
    for (int y = 0; y < H; y += rows_per) {
        int rows = (y + rows_per <= H) ? rows_per : (H - y);
        xcb_put_image(s->conn, XCB_IMAGE_FORMAT_Z_PIXMAP, pix, s->gc,
                      (uint16_t)W, (uint16_t)rows, 0, (int16_t)y, 0, s->depth,
                      (uint32_t)rows * stride,
                      (const uint8_t *)(buf + (size_t)y * W));
    }
}

// Render every output's effective source into one root-sized pixmap and install
// it as the root background, then publish the pixmap id for compositors.
static bool x11_commit(x11_state *s) {
    int W = s->root_w, H = s->root_h;
    if (W <= 0 || H <= 0) return false;

    uint32_t *buf = malloc((size_t)W * H * 4);
    if (!buf) { LOG_ERR("x11: out of memory rendering %dx%d", W, H); return false; }
    bg_image_fill_xrgb8888(0x000000, buf, W, H);  // base, in case outputs leave gaps
    for (size_t i = 0; i < s->n_outputs; i++) {
        x11_output *o = &s->outputs[i];
        const bg_source *src = o->has_override ? &o->override : &s->global;
        render_into_rect(buf, W, H, o->x, o->y, o->w, o->h, src);
    }

    xcb_pixmap_t pix = xcb_generate_id(s->conn);
    xcb_create_pixmap(s->conn, s->depth, pix, s->root, (uint16_t)W, (uint16_t)H);
    upload_pixmap(s, pix, buf, W, H);
    free(buf);

    uint32_t bp = pix;
    xcb_change_window_attributes(s->conn, s->root, XCB_CW_BACK_PIXMAP, &bp);
    xcb_clear_area(s->conn, 0, s->root, 0, 0, 0, 0);
    xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, s->root, s->atom_xrootpmap,
                        XCB_ATOM_PIXMAP, 32, 1, &pix);
    xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, s->root, s->atom_esetroot,
                        XCB_ATOM_PIXMAP, 32, 1, &pix);
    xcb_flush(s->conn);

    if (s->cur_pixmap) xcb_free_pixmap(s->conn, s->cur_pixmap);
    s->cur_pixmap = pix;
    return true;
}

// ---------- ops ----------

static xcb_atom_t intern(xcb_connection_t *c, const char *name) {
    xcb_intern_atom_reply_t *r =
        xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, (uint16_t)strlen(name), name), NULL);
    xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
    free(r);
    return a;
}

static void x11_destroy(void *st);

static void *x11_init(void) {
    x11_state *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    int screen_num = 0;
    s->conn = xcb_connect(NULL, &screen_num);
    if (!s->conn || xcb_connection_has_error(s->conn)) {
        LOG_ERR("x11: xcb_connect failed (is DISPLAY set?)");
        x11_destroy(s);
        return NULL;
    }

    const xcb_setup_t *setup = xcb_get_setup(s->conn);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screen_num && it.rem; i++) xcb_screen_next(&it);
    xcb_screen_t *screen = it.data;
    if (!screen) {
        LOG_ERR("x11: no screen %d", screen_num);
        x11_destroy(s);
        return NULL;
    }
    s->root   = screen->root;
    s->depth  = screen->root_depth;
    s->root_w = screen->width_in_pixels;
    s->root_h = screen->height_in_pixels;

    s->gc = xcb_generate_id(s->conn);
    xcb_create_gc(s->conn, s->gc, s->root, 0, NULL);

    s->atom_xrootpmap = intern(s->conn, "_XROOTPMAP_ID");
    s->atom_esetroot  = intern(s->conn, "ESETROOT_PMAP_ID");

    const xcb_query_extension_reply_t *rext = xcb_get_extension_data(s->conn, &xcb_randr_id);
    if (rext && rext->present) {
        s->have_randr = true;
        s->randr_event_base = rext->first_event;
        free(xcb_randr_query_version_reply(
            s->conn, xcb_randr_query_version(s->conn, 1, 5), NULL));
        xcb_randr_select_input(s->conn, s->root, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);
    }

    enumerate_outputs(s);
    xcb_flush(s->conn);
    return s;
}

static void x11_destroy(void *st) {
    x11_state *s = st;
    if (!s) return;
    if (s->conn && !xcb_connection_has_error(s->conn)) {
        if (s->cur_pixmap) xcb_free_pixmap(s->conn, s->cur_pixmap);
        if (s->gc) xcb_free_gc(s->conn, s->gc);
        xcb_flush(s->conn);
    }
    free_outputs(s);
    src_clear(&s->global);
    if (s->conn) xcb_disconnect(s->conn);
    free(s);
}

static bool x11_set_source(void *st, const bg_source *src) {
    x11_state *s = st;
    if (!src_copy(&s->global, src)) { LOG_ERR("x11: set_source out of memory"); return false; }
    for (size_t i = 0; i < s->n_outputs; i++) {
        src_clear(&s->outputs[i].override);
        s->outputs[i].has_override = false;
    }
    return x11_commit(s);
}

static bool x11_set_source_output(void *st, const char *output, const bg_source *src) {
    x11_state *s = st;
    x11_output *o = find_output(s, output);
    if (!o) { LOG_ERR("x11: no output named '%s'", output); return false; }
    if (!src_copy(&o->override, src)) return false;
    o->has_override = true;
    return x11_commit(s);
}

static int x11_output_names(void *st, char ***names, size_t *count) {
    x11_state *s = st;
    char **arr = calloc(s->n_outputs ? s->n_outputs : 1, sizeof(char *));
    if (!arr) return -1;
    for (size_t i = 0; i < s->n_outputs; i++)
        arr[i] = strdup(s->outputs[i].name ? s->outputs[i].name : "?");
    *names = arr;
    *count = s->n_outputs;
    return 0;
}

static int x11_fd(void *st) {
    x11_state *s = st;
    return xcb_get_file_descriptor(s->conn);
}

static int x11_flush(void *st) {
    x11_state *s = st;
    return xcb_flush(s->conn) <= 0 ? -1 : 0;
}

static int x11_dispatch(void *st) {
    x11_state *s = st;
    bool reconf = false;
    xcb_generic_event_t *ev;
    while ((ev = xcb_poll_for_event(s->conn))) {
        uint8_t type = ev->response_type & 0x7f;
        if (s->have_randr && type == s->randr_event_base + XCB_RANDR_SCREEN_CHANGE_NOTIFY)
            reconf = true;
        free(ev);
    }
    if (xcb_connection_has_error(s->conn)) { s->closed = true; return -1; }
    if (reconf) {
        // Monitor layout changed: re-enumerate and repaint the global source
        // (per-output overrides are dropped — a rare hotplug edge case).
        enumerate_outputs(s);
        x11_commit(s);
    }
    return 0;
}

static bool x11_should_exit(void *st) {
    x11_state *s = st;
    return s->closed;
}

static bool x11_probe(void) {
    // Never run under a Wayland session: an X11 root pixmap drawn through
    // XWayland is invisible to the Wayland compositor's desktop background.
    if (getenv("WAYLAND_DISPLAY")) return false;
    const char *disp = getenv("DISPLAY");
    if (!disp || !*disp) return false;
    int screen;
    xcb_connection_t *c = xcb_connect(NULL, &screen);
    bool ok = c && !xcb_connection_has_error(c);
    if (c) xcb_disconnect(c);
    return ok;
}

const bg_display_ops bg_backend_x11 = {
    .name              = "x11",
    .caps              = BG_CAP_PER_OUTPUT,
    .probe             = x11_probe,
    .init              = x11_init,
    .destroy           = x11_destroy,
    .set_fade_ms       = NULL,
    .set_source        = x11_set_source,
    .set_source_output = x11_set_source_output,
    .output_names      = x11_output_names,
    .fd                = x11_fd,
    .flush             = x11_flush,
    .dispatch          = x11_dispatch,
    .tick              = NULL,
    .timeout_ms        = NULL,
    .should_exit       = x11_should_exit,
};

#endif // BG_BACKEND_X11
