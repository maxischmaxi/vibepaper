#ifndef BG_DISPLAY_H
#define BG_DISPLAY_H

// Display-backend abstraction. The daemon talks only to this interface; the
// concrete backend (wlr-layer-shell, X11 root pixmap, or a delegate that hands a
// static image to GNOME/KDE) is chosen at runtime by bg_display_init().
//
// Two backend kinds exist behind this interface:
//   * "live" backends (wlr, x11) own the pixels and render every frame;
//   * "delegate" backends (gnome, kde) hand a file/color to the desktop
//     environment, which renders and transitions it itself.
// The bg_display_caps() bitmask tells the daemon which features a backend has.

#include "image.h"

#include <stdbool.h>
#include <stddef.h>
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

    // Optional: absolute path to the encoded source file on disk, if one exists
    // (set by `--file`, `restore`, and `generate`/`refine`). NULL for solid
    // colors and for pixels with no backing file. "Live" backends ignore this
    // and render `image`/`color_rrggbb`; "delegate" backends prefer this path so
    // they can hand the original file straight to the desktop environment.
    const char *file_path;
} bg_source;

// Backend capability flags (bg_display_caps()).
enum {
    BG_CAP_PER_OUTPUT = 1u << 0,  // honors set_source_output / the --output flag
    BG_CAP_CROSSFADE  = 1u << 1,  // performs its own crossfade (informational)
    BG_CAP_LAYER      = 1u << 2,  // VIBEPAPER_LAYER is meaningful (wlr only)
};

typedef struct bg_display bg_display;

// Pick a backend for the current environment and initialize it. Selection order:
//   1. the VIBEPAPER_BACKEND env var (wlr|x11|gnome|kde), if set;
//   2. otherwise the first backend whose probe() succeeds, in priority order
//      wlr → gnome → kde → x11 (x11 never wins under a Wayland session).
// Returns NULL on failure (logs).
bg_display *bg_display_init(void);

void bg_display_destroy(bg_display *d);

// Capability bitmask of the selected backend (BG_CAP_*).
unsigned bg_display_caps(bg_display *d);

// Crossfade duration in milliseconds for subsequent source changes (live
// backends only; delegate backends ignore it). 0 = instant.
void bg_display_set_fade_ms(bg_display *d, unsigned ms);

// Replace the global source on every output and clear per-output overrides.
// The backend consumes `src` synchronously, so the caller may free it after.
bool bg_display_set_source(bg_display *d, const bg_source *src);

// Set a source override for a single output (by name, e.g. "DP-1"). Returns
// false if no output matches the name or the backend lacks BG_CAP_PER_OUTPUT.
bool bg_display_set_source_output(bg_display *d, const char *output_name,
                                  const bg_source *src);

// Fill `*names` with a malloc'd array of malloc'd output-name strings (caller
// frees each + the array). Returns 0 / -1. Delegate backends return 0 names.
int bg_display_output_names(bg_display *d, char ***names, size_t *count);

// ---- event-loop integration ----

// File descriptor to poll(), or -1 if the backend has none (delegate backends).
int bg_display_fd(bg_display *d);

// Flush pending requests to the server. Returns 0 / -1. No-op when unsupported.
int bg_display_flush(bg_display *d);

// Drain queued events (called when bg_display_fd() became readable). 0 / -1.
int bg_display_dispatch(bg_display *d);

// Called once per event-loop iteration so a timer-driven backend can advance an
// animation. No-op for backends that don't need it.
void bg_display_tick(bg_display *d);

// Desired maximum poll() timeout in ms (e.g. during a timer-driven fade), or -1
// when the backend has no pending deadline. The daemon clamps this.
int bg_display_timeout_ms(bg_display *d);

// True once the backend can no longer render (e.g. compositor closed our surface).
bool bg_display_should_exit(bg_display *d);

#endif
