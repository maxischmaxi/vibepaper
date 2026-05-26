// background — Wayland layer-shell wallpaper tool with OpenAI image generation.
//
// Usage:
//   background daemon                          # run the renderer + IPC server
//   background --color RRGGBB                  # solid color
//   background --file PATH                     # PNG / JPEG / WebP
//   background generate "prompt" [--size S] [--quality Q] [--model M]
//   background --stop                          # shut the daemon down
//   background --help
//
// The non-daemon commands talk to a running daemon over a UNIX socket. If no
// daemon is listening, one is fork+execed automatically before the request.

#include "daemon.h"
#include "ipc.h"
#include "log.h"
#include "store.h"

#include <cJSON.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static void usage(FILE *f) {
    fputs(
"Usage:\n"
"  background daemon\n"
"  background --color RRGGBB\n"
"  background --file PATH\n"
"  background generate \"prompt\" [--size SIZE] [--quality Q] [--model M] [--output NAME]\n"
"  background list                            # past wallpapers, newest first\n"
"  background restore ID|INDEX|last [--output NAME]\n"
"  background current                         # show the active wallpaper\n"
"  background outputs                         # list connected outputs\n"
"  background prune [--keep N]                # delete old history (default keep 20)\n"
"  background --stop\n"
"  background --help\n"
"\n"
"--color and --file also accept [--output NAME] to target a single output.\n"
"Without --output a command applies to all outputs.\n"
"\n"
"Sizes (gpt-image-2): auto, 1024x1024, 1536x1024 (default), 1024x1536,\n"
"                     2048x2048, 2048x1152, 3840x2160, 2160x3840\n"
"Qualities: low, medium (default), high, auto\n"
"Crossfade: set BG_FADE_MS (default 400, 0 = instant) before starting the daemon.\n"
"Layer:     set BG_LAYER=bottom to sit above hyprpaper without disabling it\n"
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
    return send_request(o);
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
    if (!strcmp(a1, "list"))                         return cmd_list();
    if (!strcmp(a1, "current"))                      return cmd_current();
    if (!strcmp(a1, "restore") && argc >= 3)         return cmd_restore(argc - 2, argv + 2);
    if (!strcmp(a1, "outputs"))                      return cmd_outputs();
    if (!strcmp(a1, "prune"))                        return cmd_prune(argc - 2, argv + 2);

    usage(stderr);
    return 2;
}
