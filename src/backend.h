#ifndef BG_BACKEND_H
#define BG_BACKEND_H

// Internal interface between bg_display (the dispatcher in display.c) and the
// concrete backends (backend_wlr.c, backend_x11.c, backend_gnome.c, …). Each
// backend exposes exactly one `const bg_display_ops` table. Application code
// includes display.h, not this header.

#include "display.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *name;          // "wlr", "x11", "gnome", "kde"
    unsigned    caps;          // BG_CAP_* bitmask

    // Cheap check whether this backend can run in the current environment. May
    // briefly connect and disconnect. No lasting side effects. Used for the
    // auto-selection probe (skipped when VIBEPAPER_BACKEND forces this backend).
    bool  (*probe)(void);

    // Allocate and initialize backend state, or return NULL on failure (logs).
    void *(*init)(void);
    void  (*destroy)(void *st);

    void  (*set_fade_ms)(void *st, unsigned ms);
    bool  (*set_source)(void *st, const bg_source *src);
    bool  (*set_source_output)(void *st, const char *output, const bg_source *src);
    int   (*output_names)(void *st, char ***names, size_t *count);

    // Event-loop integration. Any of these may be NULL; the dispatcher then
    // supplies a sane default (fd → -1, flush/dispatch → 0, tick → no-op,
    // timeout_ms → -1, should_exit → false).
    int   (*fd)(void *st);
    int   (*flush)(void *st);
    int   (*dispatch)(void *st);
    void  (*tick)(void *st);
    int   (*timeout_ms)(void *st);
    bool  (*should_exit)(void *st);
} bg_display_ops;

// Backends are compiled in conditionally; the Makefile defines BG_BACKEND_*
// for each one it builds.
#ifdef BG_BACKEND_WLR
extern const bg_display_ops bg_backend_wlr;
#endif
#ifdef BG_BACKEND_X11
extern const bg_display_ops bg_backend_x11;
#endif
#ifdef BG_BACKEND_GNOME
extern const bg_display_ops bg_backend_gnome;
#endif
#ifdef BG_BACKEND_KDE
extern const bg_display_ops bg_backend_kde;
#endif

#endif
