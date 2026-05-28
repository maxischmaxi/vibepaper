// vibepaper — Wayland layer-shell wallpaper tool with OpenAI image generation.
//
// Usage:
//   vibepaper daemon                          # run the renderer + IPC server
//   vibepaper --color RRGGBB                  # solid color
//   vibepaper --file PATH                     # PNG / JPEG / WebP
//   vibepaper generate "prompt" [--size S] [--quality Q] [--model M]
//   vibepaper --stop                          # shut the daemon down
//   vibepaper --help
//
// The non-daemon commands talk to a running daemon over a UNIX socket. If no
// daemon is listening, one is fork+execed automatically before the request.

#include "daemon.h"
#include "ipc.h"
#include "log.h"
#include "store.h"

#include <cJSON.h>

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static void usage(FILE *f) {
    fputs(
"Usage:\n"
"  vibepaper daemon\n"
"  vibepaper --color RRGGBB\n"
"  vibepaper --file PATH\n"
"  vibepaper generate \"prompt\" [--size SIZE] [--quality Q] [--model M] [--output NAME]\n"
"  vibepaper refine \"prompt\" [--from ID|INDEX|last] [--size SIZE] [--quality Q] [--model M] [--output NAME]\n"
"  vibepaper list                            # past wallpapers, newest first\n"
"  vibepaper restore ID|INDEX|last [--output NAME]\n"
"  vibepaper current                         # show the active wallpaper\n"
"  vibepaper outputs                         # list connected outputs\n"
"  vibepaper prune [--keep N]                # delete old history (default keep 20)\n"
"  vibepaper --stop\n"
"  vibepaper --help\n"
"\n"
"--color and --file also accept [--output NAME] to target a single output.\n"
"Without --output a command applies to all outputs.\n"
"\n"
"Sizes (gpt-image-2): auto, 1024x1024, 1536x1024 (default), 1024x1536,\n"
"                     2048x2048, 2048x1152, 3840x2160, 2160x3840\n"
"Qualities: low, medium (default), high, auto\n"
"Crossfade: set VIBEPAPER_FADE_MS (default 400, 0 = instant) before starting the daemon.\n"
"Layer:     set VIBEPAPER_LAYER=bottom to sit above hyprpaper without disabling it\n"
"           (background|bottom|top|overlay; default background).\n",
        f);
}

// Return malloc'd absolute path of the running executable.
static char *self_exe_path(void) {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return NULL;
    buf[n] = '\0';
    return strdup(buf);
}

// Connect to the daemon; if it's not running, spawn one and retry.
static int connect_or_spawn(void) {
    int fd = bg_ipc_client_connect();
    if (fd >= 0) return fd;
    if (errno != ENOENT && errno != ECONNREFUSED) {
        LOG_ERR("ipc connect: %s", strerror(errno));
        return -1;
    }
    LOG_INFO("no daemon running — spawning one");
    char *self = self_exe_path();
    if (!self) { LOG_ERR("cannot resolve self exe"); return -1; }
    int rc = bg_daemon_spawn(self);
    free(self);
    if (rc < 0) return -1;
    return bg_ipc_client_connect();
}

// Send a JSON request, print/check the response. Returns process exit code.
static int send_request(cJSON *body) {
    char *s = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!s) { LOG_ERR("json serialise failed"); return 1; }

    int fd = connect_or_spawn();
    if (fd < 0) { free(s); return 1; }

    char *resp = NULL;
    int rc = bg_ipc_client_request(fd, s, &resp);
    close(fd);
    free(s);
    if (rc < 0) { LOG_ERR("ipc request failed"); free(resp); return 1; }

    int exit_code = 0;
    cJSON *root = resp ? cJSON_Parse(resp) : NULL;
    if (!root) {
        LOG_ERR("daemon returned non-JSON: %s", resp ? resp : "(empty)");
        exit_code = 1;
    } else {
        cJSON *ok = cJSON_GetObjectItem(root, "ok");
        if (!cJSON_IsTrue(ok)) {
            cJSON *err = cJSON_GetObjectItem(root, "error");
            LOG_ERR("daemon: %s",
                    cJSON_IsString(err) ? err->valuestring : "unknown error");
            exit_code = 1;
        }
        cJSON_Delete(root);
    }
    free(resp);
    return exit_code;
}

// ---------- spinner (for long-running generate) ----------

static const char *const SPIN_FRAMES[] = {
    "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏",
};
#define SPIN_N ((int)(sizeof(SPIN_FRAMES) / sizeof(SPIN_FRAMES[0])))

typedef struct {
    char            msg[160];
    int             frame;
    struct timespec start;
    bool            tty;
} spinner;

static int spinner_elapsed(const spinner *s) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int)(now.tv_sec - s->start.tv_sec);
}

static void spinner_init(spinner *s) {
    s->msg[0] = '\0';
    s->frame = 0;
    s->tty = isatty(STDERR_FILENO);
    clock_gettime(CLOCK_MONOTONIC, &s->start);
}

// Update the label. On a non-TTY this prints one plain line per stage.
static void spinner_set(spinner *s, const char *msg) {
    snprintf(s->msg, sizeof(s->msg), "%s", msg);
    if (!s->tty) fprintf(stderr, "%s\n", msg);
}

// Redraw the in-place animated line (TTY only).
static void spinner_tick(spinner *s) {
    if (!s->tty) return;
    fprintf(stderr, "\r\033[36m%s\033[0m %s \033[2m(%ds)\033[0m\033[K",
            SPIN_FRAMES[s->frame % SPIN_N], s->msg, spinner_elapsed(s));
    fflush(stderr);
    s->frame++;
}

static void spinner_clear(spinner *s) {
    if (s->tty) { fprintf(stderr, "\r\033[K"); fflush(stderr); }
}

static void spinner_done(spinner *s, const char *msg) {
    if (s->tty)
        fprintf(stderr, "\r\033[32m✓\033[0m %s \033[2m(%ds)\033[0m\033[K\n", msg, spinner_elapsed(s));
    else
        fprintf(stderr, "%s (%ds)\n", msg, spinner_elapsed(s));
}

// Like send_request, but renders a spinner with live status text while waiting
// for the daemon. The daemon streams {"progress":"…"} lines before the final
// {"ok":…}. Used for generate, which can take tens of seconds.
static int send_request_with_progress(cJSON *body) {
    char *s = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!s) { LOG_ERR("json serialise failed"); return 1; }

    spinner sp;
    spinner_init(&sp);
    spinner_set(&sp, "Connecting to wallpaper daemon");
    spinner_tick(&sp);

    int fd = bg_ipc_client_connect();
    if (fd < 0 && (errno == ENOENT || errno == ECONNREFUSED)) {
        spinner_set(&sp, "Starting wallpaper daemon");
        spinner_tick(&sp);
        char *self = self_exe_path();
        if (self) { bg_daemon_spawn(self); free(self); }
        fd = bg_ipc_client_connect();
    }
    if (fd < 0) {
        spinner_clear(&sp);
        LOG_ERR("ipc connect: %s", strerror(errno));
        free(s);
        return 1;
    }

    if (bg_ipc_write_line(fd, s) < 0) {
        spinner_clear(&sp);
        LOG_ERR("ipc write failed");
        free(s); close(fd);
        return 1;
    }
    free(s);
    spinner_set(&sp, "Sending prompt");
    spinner_tick(&sp);

    int  exit_code = 1;
    bool got_final = false;
    for (;;) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, sp.tty ? 100 : 1000);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) { spinner_tick(&sp); continue; }
        if (pfd.revents & POLLIN) {
            char *line = NULL;
            if (bg_ipc_read_line(fd, &line) < 0) break;   // EOF / error
            cJSON *o = cJSON_Parse(line);
            free(line);
            if (!o) continue;
            const cJSON *prog = cJSON_GetObjectItem(o, "progress");
            const cJSON *ok   = cJSON_GetObjectItem(o, "ok");
            if (cJSON_IsString(prog)) {
                spinner_set(&sp, prog->valuestring);
                spinner_tick(&sp);
            } else if (ok) {
                got_final = true;
                if (cJSON_IsTrue(ok)) {
                    exit_code = 0;
                } else {
                    const cJSON *err = cJSON_GetObjectItem(o, "error");
                    spinner_clear(&sp);
                    LOG_ERR("daemon: %s", cJSON_IsString(err) ? err->valuestring : "unknown error");
                }
                cJSON_Delete(o);
                break;
            }
            cJSON_Delete(o);
        }
        // Only treat HUP/ERR as terminal once no buffered data remains: the
        // daemon may close the fd in the same instant it writes the final
        // {"ok":…} line, so POLLIN and POLLHUP can arrive together. We read one
        // line per iteration, so keep draining while POLLIN is still set.
        if ((pfd.revents & (POLLHUP | POLLERR)) && !(pfd.revents & POLLIN)) break;
    }
    close(fd);

    if (!got_final) {
        spinner_clear(&sp);
        LOG_ERR("lost connection to daemon");
        return 1;
    }
    if (exit_code == 0) spinner_done(&sp, "Wallpaper updated");
    return exit_code;
}

// ---------- subcommands ----------

// Scan argv[start..] for "--output NAME"; returns NAME or NULL.
static const char *extract_output(int argc, char **argv, int start) {
    for (int i = start; i < argc; i++)
        if (!strcmp(argv[i], "--output") && i + 1 < argc) return argv[i + 1];
    return NULL;
}

static int cmd_color(int argc, char **argv) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "cmd", "color");
    cJSON_AddStringToObject(o, "value", argv[0]);
    const char *out = extract_output(argc, argv, 1);
    if (out) cJSON_AddStringToObject(o, "output", out);
    return send_request(o);
}

static int cmd_file(int argc, char **argv) {
    // Resolve to an absolute path so the daemon (with a different cwd) finds it.
    char abs[4096];
    const char *p = realpath(argv[0], abs) ? abs : argv[0];
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "cmd", "file");
    cJSON_AddStringToObject(o, "path", p);
    const char *out = extract_output(argc, argv, 1);
    if (out) cJSON_AddStringToObject(o, "output", out);
    return send_request(o);
}

static int cmd_stop(void) {
    int fd = bg_ipc_client_connect();
    if (fd < 0) {
        if (errno == ENOENT || errno == ECONNREFUSED) {
            LOG_INFO("no daemon running");
            return 0;
        }
        LOG_ERR("ipc connect: %s", strerror(errno));
        return 1;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "cmd", "stop");
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    char *resp = NULL;
    int rc = bg_ipc_client_request(fd, s, &resp);
    free(s); free(resp); close(fd);
    return rc < 0 ? 1 : 0;
}

static int cmd_generate(int argc, char **argv) {
    if (argc < 1) {
        LOG_ERR("generate: missing prompt");
        usage(stderr);
        return 2;
    }
    const char *prompt = argv[0];
    const char *size = NULL, *quality = NULL, *model = NULL, *output = NULL;
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--size")    && i + 1 < argc) size    = argv[++i];
        else if (!strcmp(argv[i], "--quality") && i + 1 < argc) quality = argv[++i];
        else if (!strcmp(argv[i], "--model")   && i + 1 < argc) model   = argv[++i];
        else if (!strcmp(argv[i], "--output")  && i + 1 < argc) output  = argv[++i];
        else { LOG_ERR("unknown flag: %s", argv[i]); return 2; }
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "cmd", "generate");
    cJSON_AddStringToObject(o, "prompt", prompt);
    if (size)    cJSON_AddStringToObject(o, "size",    size);
    if (quality) cJSON_AddStringToObject(o, "quality", quality);
    if (model)   cJSON_AddStringToObject(o, "model",   model);
    if (output)  cJSON_AddStringToObject(o, "output",  output);
    return send_request_with_progress(o);
}

// Refine (edit) an existing image with a prompt. Without --from the daemon edits
// the currently displayed wallpaper, so repeated `refine` calls iterate like a
// chat session. The result is saved as a new history entry and becomes current.
static int cmd_refine(int argc, char **argv) {
    if (argc < 1) {
        LOG_ERR("refine: missing prompt");
        usage(stderr);
        return 2;
    }
    const char *prompt = argv[0];
    const char *from = NULL, *size = NULL, *quality = NULL, *model = NULL, *output = NULL;
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--from")    && i + 1 < argc) from    = argv[++i];
        else if (!strcmp(argv[i], "--size")    && i + 1 < argc) size    = argv[++i];
        else if (!strcmp(argv[i], "--quality") && i + 1 < argc) quality = argv[++i];
        else if (!strcmp(argv[i], "--model")   && i + 1 < argc) model   = argv[++i];
        else if (!strcmp(argv[i], "--output")  && i + 1 < argc) output  = argv[++i];
        else { LOG_ERR("unknown flag: %s", argv[i]); return 2; }
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "cmd", "refine");
    cJSON_AddStringToObject(o, "prompt", prompt);
    if (from)    cJSON_AddStringToObject(o, "src",     from);
    if (size)    cJSON_AddStringToObject(o, "size",    size);
    if (quality) cJSON_AddStringToObject(o, "quality", quality);
    if (model)   cJSON_AddStringToObject(o, "model",   model);
    if (output)  cJSON_AddStringToObject(o, "output",  output);
    return send_request_with_progress(o);
}

static void print_prompt_trunc(const char *p, int max) {
    if (!p) { printf("-"); return; }
    int n = 0;
    for (const char *c = p; *c && n < max; c++, n++) {
        putchar(*c == '\n' ? ' ' : *c);
    }
    if (p[n]) printf("…");
}

static int cmd_list(void) {
    bg_store_entry *list = NULL;
    size_t n = 0;
    if (bg_store_list(&list, &n) != 0) return 1;
    if (n == 0) { printf("no wallpapers yet\n"); return 0; }

    char cur[BG_ID_MAX] = {0};
    bg_store_get_current(cur, sizeof(cur));

    printf("  #  ID                SIZE       QUAL    PROMPT\n");
    for (size_t i = 0; i < n; i++) {
        bg_store_entry *e = &list[i];
        char mark = (cur[0] && strcmp(cur, e->id) == 0) ? '*' : ' ';
        printf("%c%3zu  %-16s  %-9s  %-6s  ", mark, i + 1, e->id,
               e->size ? e->size : "-", e->quality ? e->quality : "-");
        print_prompt_trunc(e->prompt, 60);
        printf("\n");
    }
    bg_store_free_list(list, n);
    return 0;
}

static int cmd_current(void) {
    char cur[BG_ID_MAX];
    if (bg_store_get_current(cur, sizeof(cur)) != 0) {
        printf("no current wallpaper\n");
        return 0;
    }
    bg_store_entry e;
    if (bg_store_get(cur, &e) != 0) {
        printf("%s (metadata missing)\n", cur);
        return 0;
    }
    printf("id:      %s\n", e.id);
    printf("size:    %s\n", e.size ? e.size : "-");
    printf("quality: %s\n", e.quality ? e.quality : "-");
    printf("model:   %s\n", e.model ? e.model : "-");
    printf("prompt:  %s\n", e.prompt ? e.prompt : "-");
    printf("file:    %s\n", e.path);
    bg_store_entry_clear(&e);
    return 0;
}

static int cmd_restore(int argc, char **argv) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "cmd", "restore");
    cJSON_AddStringToObject(o, "id", argv[0]);
    const char *out = extract_output(argc, argv, 1);
    if (out) cJSON_AddStringToObject(o, "output", out);
    return send_request(o);
}

// Query the daemon for output names (does not spawn one).
static int cmd_outputs(void) {
    int fd = bg_ipc_client_connect();
    if (fd < 0) {
        LOG_INFO("no daemon running — start one to enumerate outputs");
        return 1;
    }
    char *resp = NULL;
    int rc = bg_ipc_client_request(fd, "{\"cmd\":\"outputs\"}", &resp);
    close(fd);
    if (rc < 0) { LOG_ERR("ipc request failed"); free(resp); return 1; }

    cJSON *root = resp ? cJSON_Parse(resp) : NULL;
    free(resp);
    if (!root) { LOG_ERR("bad response"); return 1; }
    cJSON *arr = cJSON_GetObjectItem(root, "outputs");
    if (cJSON_IsArray(arr)) {
        cJSON *it;
        cJSON_ArrayForEach(it, arr)
            if (cJSON_IsString(it)) printf("%s\n", it->valuestring);
    }
    cJSON_Delete(root);
    return 0;
}

static int cmd_prune(int argc, char **argv) {
    size_t keep = 20;
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], "--keep") && i + 1 < argc) keep = strtoul(argv[++i], NULL, 10);
    size_t del = 0;
    if (bg_store_prune(keep, &del) != 0) { LOG_ERR("prune failed"); return 1; }
    printf("pruned %zu, kept newest %zu (current protected)\n", del, keep);
    return 0;
}

// ---------- entry ----------

int main(int argc, char **argv) {
    if (argc < 2) { usage(stderr); return 2; }

    const char *a1 = argv[1];

    if (!strcmp(a1, "--help") || !strcmp(a1, "-h")) { usage(stdout); return 0; }
    if (!strcmp(a1, "daemon"))                       return bg_daemon_run();
    if (!strcmp(a1, "--stop"))                       return cmd_stop();
    if (!strcmp(a1, "--color")  && argc >= 3)        return cmd_color(argc - 2, argv + 2);
    if (!strcmp(a1, "--file")   && argc >= 3)        return cmd_file(argc - 2, argv + 2);
    if (!strcmp(a1, "generate"))                     return cmd_generate(argc - 2, argv + 2);
    if (!strcmp(a1, "refine"))                       return cmd_refine(argc - 2, argv + 2);
    if (!strcmp(a1, "list"))                         return cmd_list();
    if (!strcmp(a1, "current"))                      return cmd_current();
    if (!strcmp(a1, "restore") && argc >= 3)         return cmd_restore(argc - 2, argv + 2);
    if (!strcmp(a1, "outputs"))                      return cmd_outputs();
    if (!strcmp(a1, "prune"))                        return cmd_prune(argc - 2, argv + 2);

    usage(stderr);
    return 2;
}
