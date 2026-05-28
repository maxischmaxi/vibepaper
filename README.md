# vibepaper

A Wayland wallpaper daemon for `wlr-layer-shell` compositors (Hyprland, Sway,
river, …) that can **generate wallpapers on the fly from a text prompt — and
iteratively refine them** via the OpenAI image API — plus set static images or
solid colors, crossfade between them, keep a history you can restore from, and
run on any of the four layers so it can coexist with an existing wallpaper tool.

```sh
vibepaper generate "a moody cyberpunk alley at night, rain, neon"
```

---

## How it works

`vibepaper` is a single binary with two roles:

- **Daemon** (`vibepaper daemon`) — connects to the compositor, creates a
  `wlr-layer-shell` surface on every output, and renders the wallpaper into a
  shared-memory (`wl_shm`) buffer. It listens on a UNIX socket for commands.
- **Client** (every other invocation) — sends one JSON command to the daemon
  over that socket and prints the result. If no daemon is running, the client
  forks one automatically before sending.

```
  vibepaper generate "…"                   ┌──────────────── daemon ────────────────┐
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
        │◄──────────── {"ok":true} ──────────┤   save to history (~/.cache/vibepaper) │
        ▼                                    └─────────────────────────────────────────┘
```

Key design points:

- **Non-blocking generation.** The OpenAI request (10–30 s) runs on a worker
  thread; the compositor event loop stays responsive. Multiple `generate`
  requests are **queued** (FIFO) and processed one at a time.
- **Crossfades.** Source changes fade over `VIBEPAPER_FADE_MS` (default 400 ms),
  driven by frame callbacks. On outputs larger than 1080p the blend runs at
  half resolution (scaled up by `wp_viewporter`) to keep memory and CPU low; the
  final frame is committed at full resolution.
- **History / persistence.** Every generated image is saved to
  `~/.cache/vibepaper/<id>.png` with a `<id>.json` sidecar (prompt, size,
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
| `src/openai.c`  | OpenAI image generate + edit client (libcurl + cJSON + b64) |
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

This produces the `vibepaper` binary. `make clean` removes objects;
`make distclean` also removes the generated protocol code.

---

## Usage

```
vibepaper daemon                                  run the renderer + IPC server
vibepaper generate "prompt" [--size S] [--quality Q] [--model M] [--output NAME]
vibepaper refine   "prompt" [--from ID|INDEX|last] [--size S] [--quality Q] [--output NAME]
vibepaper --file PATH        [--output NAME]       set a local image
vibepaper --color RRGGBB     [--output NAME]       set a solid color
vibepaper list                                    past wallpapers, newest first
vibepaper restore ID|INDEX|last [--output NAME]    re-display a past wallpaper
vibepaper current                                 show the active wallpaper
vibepaper outputs                                 list connected outputs
vibepaper prune [--keep N]                         delete old history (default keep 20)
vibepaper --stop                                  shut the daemon down
vibepaper --help
```

You normally never start the daemon by hand — the first command that needs it
spawns one. To control its environment (fade, layer) start it explicitly, e.g.
`VIBEPAPER_LAYER=bottom vibepaper daemon`.

### Generating

```sh
export OPENAI_API_KEY=sk-…

vibepaper generate "snowy pine forest at golden hour, wide angle"
vibepaper generate "abstract liquid gradient, teal and magenta" --quality high
vibepaper generate "rolling fog over mountains" --size 3840x2160
```

`gpt-image-2` sizes: `auto`, `1024x1024`, `1536x1024` (default), `1024x1536`,
`2048x2048`, `2048x1152`, `3840x2160`, `2160x3840`.
Qualities: `low`, `medium` (default), `high`, `auto`.

### Refining (iterating on an image)

`refine` takes an existing image and a prompt describing what to change, sends
both to the OpenAI image-edit endpoint, and saves the result as a new wallpaper.
Without `--from` it edits the **currently displayed** wallpaper, so repeated
calls iterate like a chat session — each result becomes the new `current` that
the next `refine` builds on:

```sh
vibepaper generate "a serene mountain lake at dawn"   # start from a fresh image
vibepaper refine "add a wooden canoe on the water"    # edits the current one
vibepaper refine "make it sunset, warmer tones"       # keeps iterating
```

To branch off an older version instead of the current one, point `--from` at a
list index, an id, or `last`:

```sh
vibepaper refine "now add northern lights" --from 4
vibepaper refine "make the canoe red"      --from 20260528-091500
```

Notes:

- The edit goes to `gpt-image-2`, which preserves the input image at high
  fidelity automatically — small prompts make small changes.
- Output size defaults to the **source image's size** so the wallpaper format
  stays stable across iterations; override it per call with `--size`.
- Like `generate`, `refine` queues behind any in-flight job and shows the live
  progress spinner while the daemon works.
- `--output NAME` refines onto a single monitor only and does not change the
  global `current` (same rule as `generate`/`restore`).

### History

Every generated image is kept in `~/.cache/vibepaper`, so you can browse what
you made before and jump back to any of it without paying for a regeneration.

`vibepaper list` prints the history newest-first as a numbered table. The
leading `#` column is a 1-based index, and `*` marks the wallpaper that is
currently displayed:

```text
$ vibepaper list
  #  ID                SIZE       QUAL    PROMPT
*  1  20260528-091500   1536x1024  medium  snowy pine forest at golden hour, wide…
   2  20260527-220314   1536x1024  high    abstract liquid gradient, teal and mag…
   3  20260524-143000   3840x2160  medium  rolling fog over mountains
```

To switch the wallpaper to a past image, pass that number to `restore`:

```sh
vibepaper restore 3                  # switch to entry #3 from the list
vibepaper restore last               # switch to the most recent
vibepaper restore 20260524-143000    # switch by exact id
vibepaper current                    # show full metadata of the active one
vibepaper prune --keep 10            # keep the 10 newest, delete the rest
```

`restore N` always targets the entry that `list` numbered `N` — the ordering is
stable across calls, so the numbers don't shift between listing and restoring
(as long as you don't generate a new image in between, which pushes everything
down by one). `restore` without `--output` updates all outputs and becomes the
new "current"; with `--output NAME` it changes only that monitor.

The currently displayed wallpaper is never deleted by `prune`. `--keep 0`
removes everything except the current one.

### Per-output (multi-monitor)

```sh
vibepaper outputs                              # e.g. DP-1, HDMI-A-1
vibepaper generate "ocean" --output DP-1       # only that output
vibepaper --color 1e1e2e --output HDMI-A-1
```

A command without `--output` applies to all outputs and clears any per-output
overrides.

---

## Coexisting with hyprpaper (without disabling it)

Both `vibepaper` and hyprpaper draw a layer-shell surface. By default
`vibepaper` uses the **`background` layer** — the same one hyprpaper uses — so
running both there is ambiguous.

Instead, run `vibepaper` on the **`bottom` layer**. The layer stack is:

```
background  <  bottom  <  (your windows)  <  top  <  overlay
```

So a surface on `bottom` sits **above** hyprpaper's wallpaper but **below** every
window — exactly where a wallpaper belongs:

```sh
VIBEPAPER_LAYER=bottom vibepaper daemon          # then use vibepaper generate/restore/…
# or set it on the command that auto-spawns the daemon:
VIBEPAPER_LAYER=bottom vibepaper generate "…"
```

While `vibepaper` runs, it covers hyprpaper. Stop it and hyprpaper shows
through again — no killing, no config edits, no reboot:

```sh
vibepaper --stop
```

To make `vibepaper` your _only_ wallpaper tool instead, leave `VIBEPAPER_LAYER` at the
default (`background`) and replace your existing wallpaper autostart — see below.

---

## Autostart: replacing your wallpaper tool on Hyprland

To make `vibepaper` your permanent wallpaper daemon (instead of hyprpaper, swww,
swaybg, …), start it from your compositor's autostart and disable the old tool.
On daemon startup `vibepaper` automatically restores the last wallpaper you set
(stored in `~/.cache/vibepaper`), so your wallpaper survives reboots.

> **Put your API key in a file first.** A compositor-launched daemon won't see
> `OPENAI_API_KEY` from your shell — write it to `~/.config/vibepaper/api_key`
> (see [API key resolution](#api-key-resolution)) or `generate` will fail.

**1. Make the binary easy to launch.** Either install it onto your `PATH`:

```sh
make install            # installs to ~/.local/bin/vibepaper (PREFIX overridable)
```

…or just note its absolute path (e.g. `/home/you/code/vibepaper/vibepaper`) and
use that below. (`make install` assumes `~/.local/bin` is on your `PATH`.)

**2. Edit `~/.config/hypr/hyprland.conf`.** Comment out your current wallpaper
autostart and add `vibepaper`:

```diff
- exec-once = hyprpaper
+ exec-once = vibepaper daemon          # if installed on PATH
+ # exec-once = /home/you/code/vibepaper/vibepaper daemon   # or absolute path
```

If you used a different tool, comment out its line instead, e.g.
`exec-once = swww-daemon` or `exec-once = swaybg -i ~/wall.png`.

**3. Apply it.** Reboot, or for the current session:

```sh
pkill hyprpaper                      # stop the old tool now (swww kill / pkill swaybg)
vibepaper restore last              # bring your last wallpaper up immediately
hyprctl reload                       # reload config (picks up the autostart edit)
```

From then on, `vibepaper generate "…"`, `restore`, etc. control your wallpaper,
and the daemon comes back with your last wallpaper after every login.

To **revert** to your old tool: uncomment its `exec-once` line, remove the
`vibepaper daemon` line, `vibepaper --stop`, and reload.

---

## Configuration (environment variables)

Read by the **daemon** at startup (set them before the command that spawns it):

| Variable               | Default      | Meaning                                                             |
| ---------------------- | ------------ | ------------------------------------------------------------------- |
| `OPENAI_API_KEY`       | —            | API key for `generate` (see key resolution below).                  |
| `OPENAI_API_KEY_FILE`  | —            | Path to a file whose first line is the API key.                     |
| `VIBEPAPER_LAYER`      | `background` | Layer to render on: `background`, `bottom`, `top`, `overlay`.       |
| `VIBEPAPER_FADE_MS`    | `400`        | Crossfade duration in ms. `0` disables fades.                       |
| `XDG_CACHE_HOME`       | `~/.cache`   | History lives in `$XDG_CACHE_HOME/vibepaper`.                       |
| `XDG_RUNTIME_DIR`      | —            | Daemon socket lives here (`vibepaper.sock`); falls back to `/tmp`.  |

### API key resolution

For `generate`, the key is resolved in this order:

1. `OPENAI_API_KEY` environment variable
2. file named by `OPENAI_API_KEY_FILE`
3. `~/.config/vibepaper/api_key` (first line)

**This matters for autostart.** A daemon launched by your compositor at login
does *not* inherit your shell's environment, so `OPENAI_API_KEY` from `~/.zshrc`
won't be visible to it. Put the key in a file instead:

```sh
mkdir -p ~/.config/vibepaper
printf '%s\n' "sk-your-key" > ~/.config/vibepaper/api_key
chmod 600 ~/.config/vibepaper/api_key
```

---

## Notes & limitations

- One generation runs at a time; further `generate` calls queue (max 16) and the
  client blocks until its turn completes.
- Image generation is non-deterministic — there is no dedup cache. Use `restore`
  to re-display a past wallpaper for free instead of regenerating.
- During a crossfade on a 4K output the wallpaper is briefly soft (half-res
  blend) and snaps to full resolution at the end. Raise the `HALFRES_ABOVE`
  threshold in `src/wayland.c`, or set `VIBEPAPER_FADE_MS=0`, to avoid this.
- Requires a compositor implementing `wlr-layer-shell`. `wp_viewporter` is
  optional; without it, crossfades run at full resolution.
