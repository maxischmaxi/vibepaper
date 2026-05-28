#ifndef BG_WAYLAND_H
#define BG_WAYLAND_H

#include "image.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BG_SRC_NONE,
    BG_SRC_COLOR,
    BG_SRC_IMAGE,
} bg_source_kind;

typedef struct {
    bg_source_kind kind;
    uint32_t color_rrggbb;   // when kind == BG_SRC_COLOR
    bg_image image;          // when kind == BG_SRC_IMAGE; pixels are RGBA8
} bg_source;

typedef struct bg_wayland bg_wayland;

// Connect to the compositor, bind globals, set up a layer-shell surface on
// every output. Returns NULL on failure (logs).
bg_wayland *bg_wayland_init(void);

void bg_wayland_destroy(bg_wayland *w);

// Crossfade duration in milliseconds for subsequent source changes. 0 = instant.
// Defaults to the VIBEPAPER_FADE_MS env var, or 400ms.
void bg_wayland_set_fade_ms(bg_wayland *w, unsigned ms);

// Replace the global source on every output and clear per-output overrides.
// The module deep-copies `src`, so the caller may free its own copy after.
bool bg_wayland_set_source(bg_wayland *w, const bg_source *src);

// Set a source override for a single output (by name, e.g. "DP-1"), leaving
// other outputs untouched. Returns false if no output matches the name.
bool bg_wayland_set_source_output(bg_wayland *w, const char *output_name,
                                  const bg_source *src);

// Fill `*names` with a malloc'd array of malloc'd output-name strings (caller
// frees each + the array). Returns 0 / -1.
int bg_wayland_output_names(bg_wayland *w, char ***names, size_t *count);

// File descriptor for poll() integration.
int bg_wayland_fd(bg_wayland *w);

// Flush pending requests to the compositor. Returns 0 / -1.
int bg_wayland_flush(bg_wayland *w);

// Drain queued events without blocking. Returns 0 / -1.
int bg_wayland_dispatch(bg_wayland *w);

// True once the compositor has closed our surface (e.g. on exit).
bool bg_wayland_should_exit(bg_wayland *w);

#endif
