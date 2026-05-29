// Display-backend dispatcher: picks a backend for the current environment and
// forwards the bg_display_* API to its ops table.

#include "backend.h"
#include "display.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

struct bg_display {
    const bg_display_ops *ops;
    void *state;
};

// Selection priority. wlr first (preferred wherever layer-shell exists), then
// the Wayland delegate backends, then X11 last — and x11's probe() refuses to
// run under a Wayland session, so it is only ever picked on a real X11 server.
#if !defined(BG_BACKEND_WLR) && !defined(BG_BACKEND_X11) && \
    !defined(BG_BACKEND_GNOME) && !defined(BG_BACKEND_KDE)
#error "no display backend selected — build with at least one (see Makefile BACKENDS)"
#endif

static const bg_display_ops *const REGISTRY[] = {
#ifdef BG_BACKEND_WLR
    &bg_backend_wlr,
#endif
#ifdef BG_BACKEND_GNOME
    &bg_backend_gnome,
#endif
#ifdef BG_BACKEND_KDE
    &bg_backend_kde,
#endif
#ifdef BG_BACKEND_X11
    &bg_backend_x11,
#endif
};
#define REGISTRY_N (sizeof(REGISTRY) / sizeof(REGISTRY[0]))

bg_display *bg_display_init(void) {
    const bg_display_ops *chosen = NULL;

    const char *force = getenv("VIBEPAPER_BACKEND");
    if (force && *force) {
        for (size_t i = 0; i < REGISTRY_N; i++)
            if (strcmp(REGISTRY[i]->name, force) == 0) { chosen = REGISTRY[i]; break; }
        if (!chosen) {
            LOG_ERR("VIBEPAPER_BACKEND='%s' is not a known/compiled backend", force);
            return NULL;
        }
    } else {
        for (size_t i = 0; i < REGISTRY_N; i++) {
            if (REGISTRY[i]->probe && REGISTRY[i]->probe()) { chosen = REGISTRY[i]; break; }
        }
        if (!chosen) {
            LOG_ERR("no usable display backend found (need a wlr-layer-shell "
                    "compositor, a GNOME/KDE Wayland session, or an X11 server)");
            return NULL;
        }
    }

    void *st = chosen->init();
    if (!st) {
        LOG_ERR("display backend '%s' failed to initialize", chosen->name);
        return NULL;
    }

    bg_display *d = calloc(1, sizeof(*d));
    if (!d) { chosen->destroy(st); return NULL; }
    d->ops = chosen;
    d->state = st;
    LOG_INFO("display backend: %s", chosen->name);
    return d;
}

void bg_display_destroy(bg_display *d) {
    if (!d) return;
    d->ops->destroy(d->state);
    free(d);
}

unsigned bg_display_caps(bg_display *d) { return d->ops->caps; }

void bg_display_set_fade_ms(bg_display *d, unsigned ms) {
    if (d->ops->set_fade_ms) d->ops->set_fade_ms(d->state, ms);
}

bool bg_display_set_source(bg_display *d, const bg_source *src) {
    return d->ops->set_source(d->state, src);
}

bool bg_display_set_source_output(bg_display *d, const char *output_name,
                                  const bg_source *src) {
    if (!d->ops->set_source_output) return false;
    return d->ops->set_source_output(d->state, output_name, src);
}

int bg_display_output_names(bg_display *d, char ***names, size_t *count) {
    if (!d->ops->output_names) { *names = NULL; *count = 0; return 0; }
    return d->ops->output_names(d->state, names, count);
}

int bg_display_fd(bg_display *d) {
    return d->ops->fd ? d->ops->fd(d->state) : -1;
}

int bg_display_flush(bg_display *d) {
    return d->ops->flush ? d->ops->flush(d->state) : 0;
}

int bg_display_dispatch(bg_display *d) {
    return d->ops->dispatch ? d->ops->dispatch(d->state) : 0;
}

void bg_display_tick(bg_display *d) {
    if (d->ops->tick) d->ops->tick(d->state);
}

int bg_display_timeout_ms(bg_display *d) {
    return d->ops->timeout_ms ? d->ops->timeout_ms(d->state) : -1;
}

bool bg_display_should_exit(bg_display *d) {
    return d->ops->should_exit ? d->ops->should_exit(d->state) : false;
}
