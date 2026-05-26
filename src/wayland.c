// Wayland layer-shell client with memory-cheap crossfade transitions.
//
// The current source (image/color) is the single source of truth; we do not
// keep a full-resolution copy of the displayed pixels around. On a source
// change we re-render the previous source as the fade's "old" frame, so idle
// memory is just the committed buffer (held by the compositor) plus the small
// source image.
//
// During a fade on large outputs (>1080p) we blend at half resolution and let
// wp_viewporter scale the result up — 4x less memory and CPU per frame — then
// commit the final frame at full resolution for sharpness. A per-output 2-slot
// buffer pool avoids allocating an SHM buffer every frame; idle slots are freed.

#include "wayland.h"
#include "image.h"
#include "log.h"

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "viewporter-client-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <wayland-client.h>

#define POOL_SIZE       2
#define DEFAULT_FADE_MS 400
#define HALFRES_ABOVE   (1920L * 1080)   // blend at half-res for outputs larger than this

struct bg_output_state;

struct bg_wayland {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wp_viewporter *viewporter;

    struct bg_output_state *outputs;

    bg_source source;        // global source (source of truth)
    bool      closed;
    unsigned  fade_ms;
    enum zwlr_layer_shell_v1_layer layer;  // which layer to render on
};

typedef struct {
    struct wl_buffer *wl_buf;
    uint32_t *data;
    size_t    size;
    int       w, h;
    bool      busy;
    bool      is_pool;
} bg_buffer;

typedef struct bg_output_state {
    struct bg_output_state *next;
    bg_wayland *w;
    struct wl_output *output;
    uint32_t global_name;
    char    *name;

    bg_source source;        // per-output override
    bool      has_source;

    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wp_viewport *viewport;
    int32_t width, height;

    bg_buffer pool[POOL_SIZE];

    // crossfade animation
    bool      animating;
    bg_source anim_target;          // target source (for the final full-res frame)
    uint32_t *anim_old_half;        // blend inputs at anim_hw x anim_hh
    uint32_t *anim_new_half;
    int       anim_hw, anim_hh;
    uint32_t  anim_start_ms;
    bool      anim_have_start;
    struct wl_callback *frame_cb;
} bg_output_state;

// ---------- source ownership ----------

static void source_clear(bg_source *s) {
    if (s->kind == BG_SRC_IMAGE) bg_image_free(&s->image);
    s->kind = BG_SRC_NONE;
    s->color_rrggbb = 0;
    s->image = (bg_image){0};
}

static bool source_copy_from(bg_source *dst, const bg_source *src) {
    source_clear(dst);
    dst->kind = src->kind;
    if (src->kind == BG_SRC_COLOR) {
        dst->color_rrggbb = src->color_rrggbb;
        return true;
    }
    if (src->kind == BG_SRC_IMAGE && src->image.pixels) {
        size_t n = (size_t)src->image.width * src->image.height * 4;
        uint8_t *p = malloc(n);
        if (!p) return false;
        memcpy(p, src->image.pixels, n);
        dst->image.pixels = p;
        dst->image.width  = src->image.width;
        dst->image.height = src->image.height;
        return true;
    }
    return true;
}

// ---------- pixel rendering ----------

static uint32_t *render_source_px(const bg_source *src, int width, int height) {
    uint32_t *px = malloc((size_t)width * height * 4);
    if (!px) return NULL;
    switch (src->kind) {
    case BG_SRC_COLOR:
        bg_image_fill_xrgb8888(src->color_rrggbb, px, width, height);
        break;
    case BG_SRC_IMAGE: {
        bg_image scaled = {0};
        if (bg_image_cover(&src->image, width, height, &scaled)) {
            bg_image_blit_xrgb8888(&scaled, px, width, height);
            bg_image_free(&scaled);
        } else {
            bg_image_fill_xrgb8888(0x202020, px, width, height);
        }
        break;
    }
    case BG_SRC_NONE:
    default:
        bg_image_fill_xrgb8888(0x000000, px, width, height);
        break;
    }
    return px;
}

static inline uint32_t lerp_px(uint32_t a, uint32_t b, uint32_t t /* 0..256 */) {
    uint32_t inv = 256 - t;
    uint32_t ar = (a >> 16) & 0xff, ag = (a >> 8) & 0xff, ab = a & 0xff;
    uint32_t br = (b >> 16) & 0xff, bg = (b >> 8) & 0xff, bb = b & 0xff;
    uint32_t r  = (ar * inv + br * t) >> 8;
    uint32_t g  = (ag * inv + bg * t) >> 8;
    uint32_t bl = (ab * inv + bb * t) >> 8;
    return 0xff000000u | (r << 16) | (g << 8) | bl;
}

static const bg_source *effective_source(bg_output_state *o) {
    return o->has_source ? &o->source : &o->w->source;
}

// ---------- SHM buffer pool ----------

static int create_anon_fd(off_t size) {
    int fd = -1;
#ifdef SYS_memfd_create
    fd = (int)syscall(SYS_memfd_create, "bg-shm", 0);
#endif
    if (fd < 0) {
        char name[] = "/bg-shm-XXXXXX";
        for (int i = 0; i < 100; i++) {
            for (int j = (int)sizeof(name) - 7; j < (int)sizeof(name) - 1; j++)
                name[j] = 'A' + (rand() % 26);
            fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
            if (fd >= 0) { shm_unlink(name); break; }
            if (errno != EEXIST) break;
        }
        if (fd < 0) { LOG_ERR("shm_open: %s", strerror(errno)); return -1; }
    }
    if (ftruncate(fd, size) < 0) {
        LOG_ERR("ftruncate: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static void buffer_destroy(bg_buffer *b) {
    if (b->wl_buf) wl_buffer_destroy(b->wl_buf);
    if (b->data && b->data != MAP_FAILED) munmap(b->data, b->size);
    b->wl_buf = NULL;
    b->data = NULL;
    b->size = 0;
    b->w = b->h = 0;
    b->busy = false;
}

static void on_buf_release(void *data, struct wl_buffer *buf) {
    bg_buffer *b = data;
    b->busy = false;
    if (!b->is_pool) { buffer_destroy(b); free(b); }
}
static const struct wl_buffer_listener g_buf_listener = { .release = on_buf_release };

static bool buffer_alloc(bg_wayland *w, bg_buffer *b, int width, int height) {
    int stride = width * 4;
    size_t size = (size_t)stride * height;
    int fd = create_anon_fd((off_t)size);
    if (fd < 0) return false;
    void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { LOG_ERR("mmap: %s", strerror(errno)); close(fd); return false; }
    struct wl_shm_pool *pool = wl_shm_create_pool(w->shm, fd, (int32_t)size);
    b->wl_buf = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    b->data = map;
    b->size = size;
    b->w = width;
    b->h = height;
    b->busy = false;
    wl_buffer_add_listener(b->wl_buf, &g_buf_listener, b);
    return true;
}

static bg_buffer *buffer_acquire(bg_output_state *o, int width, int height) {
    for (int i = 0; i < POOL_SIZE; i++) {
        bg_buffer *b = &o->pool[i];
        if (b->busy) continue;
        if (b->wl_buf && (b->w != width || b->h != height)) buffer_destroy(b);
        if (!b->wl_buf) {
            b->is_pool = true;
            if (!buffer_alloc(o->w, b, width, height)) return NULL;
        }
        return b;
    }
    bg_buffer *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->is_pool = false;
    if (!buffer_alloc(o->w, b, width, height)) { free(b); return NULL; }
    return b;
}

static void free_idle_pool(bg_output_state *o) {
    for (int i = 0; i < POOL_SIZE; i++)
        if (!o->pool[i].busy) buffer_destroy(&o->pool[i]);
}

static void set_viewport_dest(bg_output_state *o) {
    if (o->viewport && o->width > 0 && o->height > 0)
        wp_viewport_set_destination(o->viewport, o->width, o->height);
}

static void commit_buffer(bg_output_state *o, bg_buffer *b) {
    b->busy = true;
    set_viewport_dest(o);
    wl_surface_attach(o->surface, b->wl_buf, 0, 0);
    wl_surface_damage_buffer(o->surface, 0, 0, b->w, b->h);
    wl_surface_commit(o->surface);
}

static void commit_px(bg_output_state *o, const uint32_t *px, int w, int h) {
    bg_buffer *b = buffer_acquire(o, w, h);
    if (!b) return;
    memcpy(b->data, px, (size_t)w * h * 4);
    commit_buffer(o, b);
}

// ---------- animation ----------

static void request_frame(bg_output_state *o);

// Half-res blend only on large outputs and only when viewporter can scale back.
static int fade_scale(bg_output_state *o) {
    if (o->viewport && (long)o->width * o->height > HALFRES_ABOVE) return 2;
    return 1;
}

static void anim_abort(bg_output_state *o) {
    if (!o->animating) return;
    free(o->anim_old_half); o->anim_old_half = NULL;
    free(o->anim_new_half); o->anim_new_half = NULL;
    source_clear(&o->anim_target);
    o->animating = false;
    o->anim_have_start = false;
    if (o->frame_cb) { wl_callback_destroy(o->frame_cb); o->frame_cb = NULL; }
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
    bg_output_state *o = data;
    wl_callback_destroy(cb);
    o->frame_cb = NULL;
    if (!o->animating || o->width <= 0 || o->height <= 0) return;

    if (!o->anim_have_start) { o->anim_start_ms = time; o->anim_have_start = true; }
    uint32_t elapsed = time - o->anim_start_ms;
    unsigned dur = o->w->fade_ms ? o->w->fade_ms : 1;
    uint32_t t = (elapsed >= dur) ? 256 : (elapsed * 256) / dur;

    if (t < 256) {
        bg_buffer *b = buffer_acquire(o, o->anim_hw, o->anim_hh);
        if (b) {
            size_t n = (size_t)o->anim_hw * o->anim_hh;
            uint32_t *dst = b->data;
            const uint32_t *a = o->anim_old_half, *c = o->anim_new_half;
            for (size_t i = 0; i < n; i++) dst[i] = lerp_px(a[i], c[i], t);
        }
        request_frame(o);              // schedule next before committing
        if (b) commit_buffer(o, b);
    } else {
        // Final frame at full resolution for sharpness.
        uint32_t *px = render_source_px(&o->anim_target, o->width, o->height);
        if (px) { commit_px(o, px, o->width, o->height); free(px); }
        free(o->anim_old_half); o->anim_old_half = NULL;
        free(o->anim_new_half); o->anim_new_half = NULL;
        source_clear(&o->anim_target);
        o->animating = false;
        o->anim_have_start = false;
        free_idle_pool(o);
    }
}
static const struct wl_callback_listener frame_listener = { .done = frame_done };

static void request_frame(bg_output_state *o) {
    o->frame_cb = wl_surface_frame(o->surface);
    wl_callback_add_listener(o->frame_cb, &frame_listener, o);
}

static void commit_instant(bg_output_state *o, const bg_source *src) {
    uint32_t *px = render_source_px(src, o->width, o->height);
    if (px) { commit_px(o, px, o->width, o->height); free(px); }
    free_idle_pool(o);
}

// Transition the output from old_src to new_src. old_src == NULL or NONE, or a
// zero fade duration, means an instant swap. Both source pointers are read
// synchronously here; the caller may free/mutate them afterwards.
static void render_transition(bg_output_state *o,
                              const bg_source *old_src, const bg_source *new_src) {
    if (o->width <= 0 || o->height <= 0) return;
    if (o->animating) anim_abort(o);

    bool fade = o->w->fade_ms > 0 && old_src && old_src->kind != BG_SRC_NONE;
    if (!fade) { commit_instant(o, new_src); return; }

    int sc = fade_scale(o);
    int hw = o->width / sc;  if (hw < 1) hw = 1;
    int hh = o->height / sc; if (hh < 1) hh = 1;

    o->anim_old_half = render_source_px(old_src, hw, hh);
    o->anim_new_half = render_source_px(new_src, hw, hh);
    if (!o->anim_old_half || !o->anim_new_half || !source_copy_from(&o->anim_target, new_src)) {
        free(o->anim_old_half); o->anim_old_half = NULL;
        free(o->anim_new_half); o->anim_new_half = NULL;
        source_clear(&o->anim_target);
        commit_instant(o, new_src);
        return;
    }
    o->anim_hw = hw;
    o->anim_hh = hh;
    o->animating = true;
    o->anim_have_start = false;
    request_frame(o);
    commit_px(o, o->anim_old_half, hw, hh);   // start frame; callback drives the rest
}

// ---------- layer surface ----------

static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *ls,
                                    uint32_t serial, uint32_t width, uint32_t height) {
    bg_output_state *o = data;
    zwlr_layer_surface_v1_ack_configure(ls, serial);

    bool size_changed = (o->width != (int32_t)width || o->height != (int32_t)height);
    o->width  = (int32_t)width;
    o->height = (int32_t)height;
    if (width == 0 || height == 0) return;

    if (size_changed) {
        anim_abort(o);
        for (int i = 0; i < POOL_SIZE; i++)
            if (!o->pool[i].busy) buffer_destroy(&o->pool[i]);
    }
    // A (re)configure renders the current source instantly.
    commit_instant(o, effective_source(o));
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
    bg_output_state *o = data;
    LOG_INFO("layer surface closed");
    zwlr_layer_surface_v1_destroy(ls);
    o->layer_surface = NULL;
    o->w->closed = true;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed    = layer_surface_closed,
};

static void output_setup_layer(bg_output_state *o) {
    o->surface = wl_compositor_create_surface(o->w->compositor);
    if (o->w->viewporter)
        o->viewport = wp_viewporter_get_viewport(o->w->viewporter, o->surface);

    o->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        o->w->layer_shell, o->surface, o->output,
        o->w->layer, "background");
    zwlr_layer_surface_v1_set_anchor(o->layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(o->layer_surface, -1);
    zwlr_layer_surface_v1_set_size(o->layer_surface, 0, 0);
    zwlr_layer_surface_v1_add_listener(o->layer_surface, &layer_surface_listener, o);
    wl_surface_commit(o->surface);
}

static void output_destroy(bg_output_state *o) {
    if (o->frame_cb) wl_callback_destroy(o->frame_cb);
    for (int i = 0; i < POOL_SIZE; i++) buffer_destroy(&o->pool[i]);
    free(o->anim_old_half);
    free(o->anim_new_half);
    source_clear(&o->anim_target);
    source_clear(&o->source);
    free(o->name);
    if (o->viewport)      wp_viewport_destroy(o->viewport);
    if (o->layer_surface) zwlr_layer_surface_v1_destroy(o->layer_surface);
    if (o->surface)       wl_surface_destroy(o->surface);
    if (o->output)        wl_output_destroy(o->output);
    free(o);
}

// ---------- wl_output (names) ----------

static void output_geometry(void *d, struct wl_output *o, int32_t x, int32_t y,
                            int32_t pw, int32_t ph, int32_t sub,
                            const char *make, const char *model, int32_t transform) {}
static void output_mode(void *d, struct wl_output *o, uint32_t f, int32_t w, int32_t h, int32_t r) {}
static void output_done(void *d, struct wl_output *o) {}
static void output_scale(void *d, struct wl_output *o, int32_t s) {}
static void output_description(void *d, struct wl_output *o, const char *desc) {}
static void output_name(void *data, struct wl_output *wo, const char *name) {
    bg_output_state *o = data;
    free(o->name);
    o->name = strdup(name);
}
static const struct wl_output_listener output_listener = {
    .geometry = output_geometry, .mode = output_mode, .done = output_done,
    .scale = output_scale, .name = output_name, .description = output_description,
};

// ---------- registry ----------

static void registry_global(void *data, struct wl_registry *reg,
                            uint32_t name, const char *iface, uint32_t version) {
    bg_wayland *w = data;
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        w->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, version < 4 ? version : 4);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        w->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
        w->layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, version < 4 ? version : 4);
    } else if (strcmp(iface, wp_viewporter_interface.name) == 0) {
        w->viewporter = wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
    } else if (strcmp(iface, wl_output_interface.name) == 0) {
        bg_output_state *o = calloc(1, sizeof(*o));
        o->w = w;
        o->global_name = name;
        uint32_t ver = version < 4 ? version : 4;
        o->output = wl_registry_bind(reg, name, &wl_output_interface, ver);
        char fallback[32];
        snprintf(fallback, sizeof(fallback), "output-%u", name);
        o->name = strdup(fallback);
        wl_output_add_listener(o->output, &output_listener, o);
        o->next = w->outputs;
        w->outputs = o;
    }
}

static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name) {
    (void)reg;
    bg_wayland *w = data;
    bg_output_state **p = &w->outputs;
    while (*p) {
        if ((*p)->global_name == name) {
            bg_output_state *gone = *p;
            *p = gone->next;
            output_destroy(gone);
            return;
        }
        p = &(*p)->next;
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

// ---------- public API ----------

bg_wayland *bg_wayland_init(void) {
    bg_wayland *w = calloc(1, sizeof(*w));
    if (!w) return NULL;

    const char *env = getenv("BG_FADE_MS");
    w->fade_ms = env ? (unsigned)strtoul(env, NULL, 10) : DEFAULT_FADE_MS;

    // Layer to render on. Use "bottom" to sit above another wallpaper daemon
    // (e.g. hyprpaper on the "background" layer) but below all windows.
    w->layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
    const char *lenv = getenv("BG_LAYER");
    if (lenv) {
        if      (!strcmp(lenv, "background")) w->layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
        else if (!strcmp(lenv, "bottom"))     w->layer = ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
        else if (!strcmp(lenv, "top"))        w->layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
        else if (!strcmp(lenv, "overlay"))    w->layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
        else LOG_WARN("unknown BG_LAYER '%s' — using 'background'", lenv);
    }

    w->display = wl_display_connect(NULL);
    if (!w->display) {
        LOG_ERR("wl_display_connect failed (is WAYLAND_DISPLAY set?)");
        free(w);
        return NULL;
    }
    w->registry = wl_display_get_registry(w->display);
    wl_registry_add_listener(w->registry, &registry_listener, w);
    wl_display_roundtrip(w->display);
    wl_display_roundtrip(w->display);

    if (!w->compositor || !w->shm || !w->layer_shell) {
        LOG_ERR("missing required globals (compositor=%p shm=%p layer_shell=%p)",
                (void *)w->compositor, (void *)w->shm, (void *)w->layer_shell);
        bg_wayland_destroy(w);
        return NULL;
    }
    if (!w->viewporter)
        LOG_WARN("wp_viewporter unavailable — crossfades run at full resolution");
    if (!w->outputs) {
        LOG_ERR("no wl_outputs available");
        bg_wayland_destroy(w);
        return NULL;
    }
    for (bg_output_state *o = w->outputs; o; o = o->next) output_setup_layer(o);
    wl_display_flush(w->display);
    return w;
}

void bg_wayland_destroy(bg_wayland *w) {
    if (!w) return;
    while (w->outputs) {
        bg_output_state *o = w->outputs;
        w->outputs = o->next;
        output_destroy(o);
    }
    source_clear(&w->source);
    if (w->viewporter)  wp_viewporter_destroy(w->viewporter);
    if (w->layer_shell) zwlr_layer_shell_v1_destroy(w->layer_shell);
    if (w->shm)         wl_shm_destroy(w->shm);
    if (w->compositor)  wl_compositor_destroy(w->compositor);
    if (w->registry)    wl_registry_destroy(w->registry);
    if (w->display)     wl_display_disconnect(w->display);
    free(w);
}

void bg_wayland_set_fade_ms(bg_wayland *w, unsigned ms) { w->fade_ms = ms; }

bool bg_wayland_set_source(bg_wayland *w, const bg_source *src) {
    // Start transitions first (they read the current source as "old"), then
    // commit the new source as the global one and drop per-output overrides.
    for (bg_output_state *o = w->outputs; o; o = o->next)
        render_transition(o, effective_source(o), src);

    if (!source_copy_from(&w->source, src)) {
        LOG_ERR("set_source: out of memory");
        return false;
    }
    for (bg_output_state *o = w->outputs; o; o = o->next)
        if (o->has_source) { source_clear(&o->source); o->has_source = false; }

    wl_display_flush(w->display);
    return true;
}

bool bg_wayland_set_source_output(bg_wayland *w, const char *output_name,
                                  const bg_source *src) {
    bg_output_state *target = NULL;
    for (bg_output_state *o = w->outputs; o; o = o->next)
        if (o->name && strcmp(o->name, output_name) == 0) { target = o; break; }
    if (!target) {
        LOG_ERR("set_source_output: no output named '%s'", output_name);
        return false;
    }
    render_transition(target, effective_source(target), src);
    if (!source_copy_from(&target->source, src)) return false;
    target->has_source = true;
    wl_display_flush(w->display);
    return true;
}

int bg_wayland_output_names(bg_wayland *w, char ***names, size_t *count) {
    size_t n = 0;
    for (bg_output_state *o = w->outputs; o; o = o->next) n++;
    char **arr = calloc(n ? n : 1, sizeof(char *));
    if (!arr) return -1;
    size_t i = 0;
    for (bg_output_state *o = w->outputs; o; o = o->next)
        arr[i++] = strdup(o->name ? o->name : "?");
    *names = arr;
    *count = n;
    return 0;
}

int  bg_wayland_fd(bg_wayland *w)          { return wl_display_get_fd(w->display); }
int  bg_wayland_flush(bg_wayland *w)       { return wl_display_flush(w->display); }
int  bg_wayland_dispatch(bg_wayland *w)    { return wl_display_dispatch(w->display); }
bool bg_wayland_should_exit(bg_wayland *w) { return w->closed; }
