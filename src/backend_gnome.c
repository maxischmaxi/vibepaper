// GNOME display backend (delegate). GNOME's Mutter implements no wlr-layer-shell
// and there is no cross-compositor protocol for a third-party process to draw
// the desktop background, so we hand the wallpaper to GNOME the supported way:
// the org.gnome.desktop.background GSettings keys. GNOME renders it and runs its
// own fade, so this backend owns no pixels, has no fd, and cannot do per-output.
//
// No subprocess is spawned — settings are written through the GIO C API, which
// keeps the project's "never shell out" posture. Used on a GNOME session
// (X11 or Wayland) when the wlr backend isn't available.

#include "backend.h"
#include "display.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

#include <gio/gio.h>

#ifdef BG_BACKEND_GNOME

#define BG_SCHEMA "org.gnome.desktop.background"

typedef struct {
    GSettings       *bg;
    GSettingsSchema *schema;   // kept to probe key existence before writing
} gnome_state;

static void set_str_if_has_key(gnome_state *s, const char *key, const char *val) {
    if (g_settings_schema_has_key(s->schema, key))
        g_settings_set_string(s->bg, key, val);
}

static void *gnome_init(void) {
    gnome_state *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    s->schema = src ? g_settings_schema_source_lookup(src, BG_SCHEMA, TRUE) : NULL;
    if (!s->schema) {
        LOG_ERR("gnome: GSettings schema %s not installed", BG_SCHEMA);
        free(s);
        return NULL;
    }
    s->bg = g_settings_new(BG_SCHEMA);
    return s;
}

static void gnome_destroy(void *st) {
    gnome_state *s = st;
    if (!s) return;
    if (s->bg) g_object_unref(s->bg);
    if (s->schema) g_settings_schema_unref(s->schema);
    free(s);
}

static bool gnome_set_source(void *st, const bg_source *src) {
    gnome_state *s = st;

    if (src->kind == BG_SRC_COLOR) {
        char hex[8];
        snprintf(hex, sizeof(hex), "#%06X", src->color_rrggbb & 0xFFFFFFu);
        set_str_if_has_key(s, "primary-color", hex);
        set_str_if_has_key(s, "color-shading-type", "solid");
        set_str_if_has_key(s, "picture-options", "none");
        g_settings_sync();
        return true;
    }

    if (src->kind == BG_SRC_IMAGE) {
        if (!src->file_path) {
            LOG_ERR("gnome: image source has no on-disk path to hand to GNOME");
            return false;
        }
        GError *err = NULL;
        char *uri = g_filename_to_uri(src->file_path, NULL, &err);
        if (!uri) {
            LOG_ERR("gnome: cannot make URI for '%s': %s",
                    src->file_path, err ? err->message : "?");
            if (err) g_error_free(err);
            return false;
        }
        set_str_if_has_key(s, "picture-uri", uri);
        set_str_if_has_key(s, "picture-uri-dark", uri);   // newer GNOME (dark theme)
        set_str_if_has_key(s, "picture-options", "zoom");
        g_settings_sync();
        g_free(uri);
        return true;
    }

    return true;  // BG_SRC_NONE: nothing to do
}

static bool gnome_probe(void) {
    const char *xdg = getenv("XDG_CURRENT_DESKTOP");
    return xdg && strcasestr(xdg, "GNOME") != NULL;
}

const bg_display_ops bg_backend_gnome = {
    .name              = "gnome",
    .caps              = 0,   // no per-output, no self-controlled crossfade
    .probe             = gnome_probe,
    .init              = gnome_init,
    .destroy           = gnome_destroy,
    .set_fade_ms       = NULL,
    .set_source        = gnome_set_source,
    .set_source_output = NULL,
    .output_names      = NULL,
    .fd                = NULL,
    .flush             = NULL,
    .dispatch          = NULL,
    .tick              = NULL,
    .timeout_ms        = NULL,
    .should_exit       = NULL,
};

#endif // BG_BACKEND_GNOME
