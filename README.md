# background

A Wayland wallpaper daemon for `wlr-layer-shell` compositors (Hyprland, Sway,
river, …) that can **generate wallpapers on the fly from a text prompt** via the
OpenAI image API — plus set static images or solid colors, crossfade between
them, keep a history you can restore from, and run on any of the four layers so
it can coexist with an existing wallpaper tool.

```sh
background generate "a moody cyberpunk alley at night, rain, neon"
```

---

## How it works

`background` is a single binary with two roles:

- **Daemon** (`background daemon`) — connects to the compositor, creates a
  `wlr-layer-shell` surface on every output, and renders the wallpaper into a
  shared-memory (`wl_shm`) buffer. It listens on a UNIX socket for commands.
- **Client** (every other invocation) — sends one JSON command to the daemon
  over that socket and prints the result. If no daemon is running, the client
  forks one automatically before sending.

```
  background generate "…"                   ┌──────────────── daemon ────────────────┐
        │                                    │                                         │
        │ 1. connect (spawn daemon if none)  │  poll() loop:                           │
        ├───────────── UNIX socket ─────────►│   • wl_display fd  (compositor events)  │
        │  {"cmd":"generate","prompt":"…"}   │   • listen fd      (new clients)        │
        │                                    │   • eventfd        (worker done)        │
        │                                    │                                         │
        │                                    │  generate → worker thread:              │
        │                                    │   OpenAI HTTP ─► PNG bytes              │
        │                                    │      │                                  │
        │                                    │   decode (stb) → cover-fit → crossfade  │
        │                                    │      │            onto layer surface    │
        │◄──────────── {"ok":true} ──────────┤   save to history (~/.cache/background) │
        ▼                                    └─────────────────────────────────────────┘
```

Key design points:

- **Non-blocking generation.** The OpenAI request (10–30 s) runs on a worker
  thread; the compositor event loop stays responsive. Multiple `generate`
  requests are **queued** (FIFO) and processed one at a time.
- **Crossfades.** Source changes fade over `BG_FADE_MS` (default 400 ms),
  driven by frame callbacks. On outputs larger than 1080p the blend runs at
  half resolution (scaled up by `wp_viewporter`) to keep memory and CPU low; the
  final frame is committed at full resolution.
- **History / persistence.** Every generated image is saved to
  `~/.cache/background/<id>.png` with a `<id>.json` sidecar (prompt, size,
  quality, model, timestamp). The last wallpaper is restored automatically when
  the daemon restarts.
- **Cover-fit.** Images are scaled to fully cover each output and center-cropped
  (like CSS `background-size: cover`).

### Source layout

| File            | Responsibility                                             |
| --------------- | ---------------------------------------------------------- |
| `src/main.c`    | CLI parsing, client requests, daemon auto-spawn            |
| `src/daemon.c`  | Event loop, command dispatch, generation queue + worker    |
| `src/wayland.c` | Layer-shell surfaces, SHM buffer pool, crossfade, viewport |
| `src/openai.c`  | OpenAI image API client (libcurl + cJSON + base64)         |
| `src/image.c`   | Decode / cover-fit / blit (stb)                            |
| `src/store.c`   | History store (save, list, restore, prune)                 |
| `src/ipc.c`     | UNIX-socket JSON line protocol                             |
| `protocols/`    | Vendored `wlr-layer-shell` XML + generated glue            |
| `third_party/`  | Vendored `stb_image` / `stb_image_resize2`                 |

---

## Build

Dependencies (Arch package names):

```sh
sudo pacman -S --needed wayland wayland-protocols cjson curl base-devel
```

`stb` and the `wlr-layer-shell` protocol are vendored in the repo, so no AUR
packages are needed. Then:

```sh
make
```

This produces the `background` binary. `make clean` removes objects;
`make distclean` also removes the generated protocol code.

---

## Usage

```
background daemon                                  run the renderer + IPC server
background generate "prompt" [--size S] [--quality Q] [--model M] [--output NAME]
background --file PATH        [--output NAME]       set a local image
background --color RRGGBB     [--output NAME]       set a solid color
background list                                    past wallpapers, newest first
background restore ID|INDEX|last [--output NAME]    re-display a past wallpaper
background current                                 show the active wallpaper
background outputs                                 list connected outputs
background prune [--keep N]                         delete old history (default keep 20)
background --stop                                  shut the daemon down
background --help
```

You normally never start the daemon by hand — the first command that needs it
spawns one. To control its environment (fade, layer) start it explicitly, e.g.
`BG_LAYER=bottom background daemon`.

### Generating

```sh
export OPENAI_API_KEY=sk-…

background generate "snowy pine forest at golden hour, wide angle"
background generate "abstract liquid gradient, teal and magenta" --quality high
background generate "rolling fog over mountains" --size 3840x2160
```

`gpt-image-2` sizes: `auto`, `1024x1024`, `1536x1024` (default), `1024x1536`,
`2048x2048`, `2048x1152`, `3840x2160`, `2160x3840`.
Qualities: `low`, `medium` (default), `high`, `auto`.

### History

```sh
background list            # → indexed table, '*' marks the active one
background restore 3       # restore by list index
background restore last    # restore the most recent
background restore 20260524-143000   # restore by id
background current         # show metadata of the active wallpaper
background prune --keep 10 # keep the 10 newest, delete the rest
```

The currently displayed wallpaper is never deleted by `prune`. `--keep 0`
removes everything except the current one.

### Per-output (multi-monitor)

```sh
background outputs                              # e.g. DP-1, HDMI-A-1
background generate "ocean" --output DP-1       # only that output
background --color 1e1e2e --output HDMI-A-1
```

A command without `--output` applies to all outputs and clears any per-output
overrides.

---

## Coexisting with hyprpaper (without disabling it)

Both `background` and hyprpaper draw a layer-shell surface. By default
`background` uses the **`background` layer** — the same one hyprpaper uses — so
running both there is ambiguous.

Instead, run `background` on the **`bottom` layer**. The layer stack is:

```
background  <  bottom  <  (your windows)  <  top  <  overlay
```

So a surface on `bottom` sits **above** hyprpaper's wallpaper but **below** every
window — exactly where a wallpaper belongs:

```sh
BG_LAYER=bottom background daemon          # then use background generate/restore/…
# or set it on the command that auto-spawns the daemon:
BG_LAYER=bottom background generate "…"
```

While `background` runs, it covers hyprpaper. Stop it and hyprpaper shows
through again — no killing, no config edits, no reboot:

```sh
background --stop
```

To make `background` your _only_ wallpaper tool instead, leave `BG_LAYER` at the
default (`background`) and replace your existing wallpaper autostart — see below.

---

## Autostart: replacing your wallpaper tool on Hyprland

To make `background` your permanent wallpaper daemon (instead of hyprpaper, swww,
swaybg, …), start it from your compositor's autostart and disable the old tool.
On daemon startup `background` automatically restores the last wallpaper you set
(stored in `~/.cache/background`), so your wallpaper survives reboots.

**1. Make the binary easy to launch.** Either install it onto your `PATH`:

```sh
make install            # installs to ~/.local/bin/background (PREFIX overridable)
```

…or just note its absolute path (e.g. `/home/you/code/background/background`) and
use that below. (`make install` assumes `~/.local/bin` is on your `PATH`.)

**2. Edit `~/.config/hypr/hyprland.conf`.** Comment out your current wallpaper
autostart and add `background`:

```diff
- exec-once = hyprpaper
+ exec-once = background daemon          # if installed on PATH
+ # exec-once = /home/you/code/background/background daemon   # or absolute path
```

If you used a different tool, comment out its line instead, e.g.
`exec-once = swww-daemon` or `exec-once = swaybg -i ~/wall.png`.

**3. Apply it.** Reboot, or for the current session:

```sh
pkill hyprpaper                      # stop the old tool now (swww kill / pkill swaybg)
background restore last              # bring your last wallpaper up immediately
hyprctl reload                       # reload config (picks up the autostart edit)
```

From then on, `background generate "…"`, `restore`, etc. control your wallpaper,
and the daemon comes back with your last wallpaper after every login.

To **revert** to your old tool: uncomment its `exec-once` line, remove the
`background daemon` line, `background --stop`, and reload.

---

## Configuration (environment variables)

Read by the **daemon** at startup (set them before the command that spawns it):

| Variable          | Default      | Meaning                                                             |
| ----------------- | ------------ | ------------------------------------------------------------------- |
| `OPENAI_API_KEY`  | —            | Required for `generate`.                                            |
| `BG_LAYER`        | `background` | Layer to render on: `background`, `bottom`, `top`, `overlay`.       |
| `BG_FADE_MS`      | `400`        | Crossfade duration in ms. `0` disables fades.                       |
| `XDG_CACHE_HOME`  | `~/.cache`   | History lives in `$XDG_CACHE_HOME/background`.                      |
| `XDG_RUNTIME_DIR` | —            | Daemon socket lives here (`background.sock`); falls back to `/tmp`. |

---

## Notes & limitations

- One generation runs at a time; further `generate` calls queue (max 16) and the
  client blocks until its turn completes.
- Image generation is non-deterministic — there is no dedup cache. Use `restore`
  to re-display a past wallpaper for free instead of regenerating.
- During a crossfade on a 4K output the wallpaper is briefly soft (half-res
  blend) and snaps to full resolution at the end. Raise the `HALFRES_ABOVE`
  threshold in `src/wayland.c`, or set `BG_FADE_MS=0`, to avoid this.
- Requires a compositor implementing `wlr-layer-shell`. `wp_viewporter` is
  optional; without it, crossfades run at full resolution.
