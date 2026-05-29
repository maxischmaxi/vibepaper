// KDE Plasma display backend (delegate). KWin implements no wlr-layer-shell, so
// we set the wallpaper the supported Plasma way: a small JavaScript snippet sent
// to org.kde.plasmashell's evaluateScript over D-Bus, which iterates the desktops
// and writes the Image/Color config. Plasma renders it and runs its own
// transition, so this backend owns no pixels, has no fd, and (for now) applies
// the same wallpaper to every desktop rather than per-output.
//
// No subprocess is spawned — the call goes through libdbus directly, keeping the
// project's "never shell out" posture. The only interpolated value is a file
// path (from our own cache) or a color; it is escaped for the JS string context.

#include "backend.h"
#include "display.h"
#include "log.h"
#include "store.h"   // BG_PATH_MAX

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dbus/dbus.h>

#ifdef BG_BACKEND_KDE

typedef struct {
    DBusConnection *conn;
} kde_state;

// Append `in` to a JS single-quoted string literal, escaping backslash/quote and
// dropping control characters. Defends evaluateScript against a path containing
// quotes or newlines (e.g. a hostile `--file` argument).
static void js_escape_append(char *out, size_t cap, size_t *len, const char *in) {
    for (const char *p = in; *p && *len + 2 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\\' || c == '\'') { out[(*len)++] = '\\'; out[(*len)++] = (char)c; }
        else if (c < 0x20)          { /* drop control chars */ }
        else                        { out[(*len)++] = (char)c; }
    }
    out[*len] = '\0';
}

static bool kde_eval(kde_state *s, const char *script) {
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *msg = dbus_message_new_method_call(
        "org.kde.plasmashell", "/PlasmaShell", "org.kde.PlasmaShell", "evaluateScript");
    if (!msg) { LOG_ERR("kde: out of memory building D-Bus message"); return false; }
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &script, DBUS_TYPE_INVALID);

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(s->conn, msg, 4000, &err);
    dbus_message_unref(msg);
    bool ok = true;
    if (dbus_error_is_set(&err)) {
        LOG_ERR("kde: evaluateScript failed: %s", err.message);
        dbus_error_free(&err);
        ok = false;
    }
    if (reply) dbus_message_unref(reply);
    return ok;
}

static void *kde_init(void) {
    kde_state *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    DBusError err;
    dbus_error_init(&err);
    s->conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!s->conn) {
        LOG_ERR("kde: cannot connect to D-Bus session bus: %s",
                dbus_error_is_set(&err) ? err.message : "?");
        if (dbus_error_is_set(&err)) dbus_error_free(&err);
        free(s);
        return NULL;
    }
    dbus_connection_set_exit_on_disconnect(s->conn, FALSE);
    return s;
}

static void kde_destroy(void *st) {
    kde_state *s = st;
    if (!s) return;
    if (s->conn) dbus_connection_unref(s->conn);  // shared bus connection: unref only
    free(s);
}

static bool kde_set_source(void *st, const bg_source *src) {
    kde_state *s = st;
    char script[BG_PATH_MAX + 512];

    if (src->kind == BG_SRC_COLOR) {
        unsigned v = src->color_rrggbb & 0xFFFFFFu;
        snprintf(script, sizeof(script),
            "var d = desktops();"
            "for (var i = 0; i < d.length; i++) {"
            "  d[i].wallpaperPlugin = 'org.kde.color';"
            "  d[i].currentConfigGroup = ['Wallpaper','org.kde.color','General'];"
            "  d[i].writeConfig('Color', '%u,%u,%u');"
            "}",
            (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
        return kde_eval(s, script);
    }

    if (src->kind == BG_SRC_IMAGE) {
        if (!src->file_path) {
            LOG_ERR("kde: image source has no on-disk path to hand to Plasma");
            return false;
        }
        size_t len = 0;
        char head[] =
            "var d = desktops();"
            "for (var i = 0; i < d.length; i++) {"
            "  d[i].wallpaperPlugin = 'org.kde.image';"
            "  d[i].currentConfigGroup = ['Wallpaper','org.kde.image','General'];"
            "  d[i].writeConfig('Image', 'file://";
        len = strlen(head);
        if (len >= sizeof(script)) return false;
        memcpy(script, head, len + 1);
        js_escape_append(script, sizeof(script) - 8, &len, src->file_path);
        const char *tail = "'); }";
        size_t tl = strlen(tail);
        if (len + tl + 1 >= sizeof(script)) return false;
        memcpy(script + len, tail, tl + 1);
        return kde_eval(s, script);
    }

    return true;  // BG_SRC_NONE
}

static bool kde_probe(void) {
    const char *xdg = getenv("XDG_CURRENT_DESKTOP");
    if (!xdg) return false;
    return strcasestr(xdg, "KDE") != NULL || strcasestr(xdg, "plasma") != NULL;
}

const bg_display_ops bg_backend_kde = {
    .name              = "kde",
    .caps              = 0,   // no per-output, no self-controlled crossfade
    .probe             = kde_probe,
    .init              = kde_init,
    .destroy           = kde_destroy,
    .set_fade_ms       = NULL,
    .set_source        = kde_set_source,
    .set_source_output = NULL,
    .output_names      = NULL,
    .fd                = NULL,
    .flush             = NULL,
    .dispatch          = NULL,
    .tick              = NULL,
    .timeout_ms        = NULL,
    .should_exit       = NULL,
};

#endif // BG_BACKEND_KDE
