// Daemon: owns the Wayland session and an IPC server. Image generation runs on
// a worker thread so the compositor event loop stays responsive during the
// (slow) OpenAI call. The worker signals completion through an eventfd that the
// main poll loop watches; all Wayland calls stay on the main thread.

#include "daemon.h"
#include "image.h"
#include "ipc.h"
#include "log.h"
#include "openai.h"
#include "store.h"
#include "wayland.h"

#include <cJSON.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// ---------- in-flight generation job ----------

#define GEN_QUEUE_MAX 16

// A pending or running generation request (owns its string copies).
typedef struct gen_request {
    struct gen_request *next;
    int   client_fd;        // who to reply to (-1 if gone)
    char *prompt, *model, *size, *quality;
    char *output;           // target output name, NULL = all
    char *src_path;         // edit source image; NULL = plain generate
} gen_request;

typedef struct {
    bool      active;       // a worker is running / result pending
    pthread_t thread;
    gen_request req;        // the request currently being processed

    // result (filled by worker)
    bool             ok;
    bg_openai_result res;
    char             err[256];
} gen_job;

typedef struct {
    bg_wayland *w;
    int   listen_fd;
    int   gen_evfd;     // eventfd, worker → main loop completion signal
    gen_job job;
    gen_request *q_head, *q_tail;  // FIFO of pending requests
    size_t q_len;
    bool  running;
} daemon_ctx;

// ---------- signal handling ----------

static volatile sig_atomic_t g_signaled = 0;
static void on_signal(int sig) { (void)sig; g_signaled = 1; }

// ---------- response helpers ----------

static void reply_ok(int fd) {
    if (fd >= 0) bg_ipc_write_line(fd, "{\"ok\":true}");
}

static void reply_err(int fd, const char *msg) {
    if (fd < 0) return;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", 0);
    cJSON_AddStringToObject(o, "error", msg);
    char *s = cJSON_PrintUnformatted(o);
    bg_ipc_write_line(fd, s ? s : "{\"ok\":false}");
    free(s);
    cJSON_Delete(o);
}

// Intermediate status line sent before the final result. The client may render
// it as a spinner label; non-progress-aware callers only read the final line.
static void reply_progress(int fd, const char *msg) {
    if (fd < 0) return;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "progress", msg);
    char *s = cJSON_PrintUnformatted(o);
    if (s) bg_ipc_write_line(fd, s);
    free(s);
    cJSON_Delete(o);
}

// ---------- small helpers ----------

static int parse_hex_color(const char *s, uint32_t *out) {
    if (!s) return -1;
    if (*s == '#') s++;
    if (strlen(s) != 6) return -1;
    char *end;
    unsigned long v = strtoul(s, &end, 16);
    if (*end != '\0') return -1;
    *out = (uint32_t)v;
    return 0;
}

// Route a source to a single output (out != NULL) or to all outputs.
static bool apply_source(bg_wayland *w, const char *out, const bg_source *src) {
    return out ? bg_wayland_set_source_output(w, out, src)
               : bg_wayland_set_source(w, src);
}

static bool apply_image_path(bg_wayland *w, const char *out, const char *path) {
    bg_image img = {0};
    if (!bg_image_load_file(path, &img)) return false;
    bg_source src = { .kind = BG_SRC_IMAGE, .image = img };
    bool ok = apply_source(w, out, &src);
    bg_image_free(&img);
    return ok;
}

// Resolve a restore argument (exact id, "last", or 1-based index) to an entry.
static int resolve_entry(const char *arg, bg_store_entry *out) {
    if (!arg || !*arg) return -1;
    bool all_digits = true;
    for (const char *p = arg; *p; p++) if (*p < '0' || *p > '9') { all_digits = false; break; }

    if (strcmp(arg, "last") == 0 || all_digits) {
        bg_store_entry *list = NULL;
        size_t n = 0;
        if (bg_store_list(&list, &n) != 0 || n == 0) { bg_store_free_list(list, n); return -1; }
        size_t idx = 0;
        if (all_digits) {
            long v = strtol(arg, NULL, 10);
            if (v < 1 || (size_t)v > n) { bg_store_free_list(list, n); return -1; }
            idx = (size_t)v - 1;
        }
        *out = list[idx];
        list[idx].prompt = list[idx].model = list[idx].size = list[idx].quality = NULL;
        bg_store_free_list(list, n);
        return 0;
    }
    return bg_store_get(arg, out);
}

// ---------- async generation ----------

static void *gen_worker(void *arg) {
    daemon_ctx *ctx = arg;
    gen_job *j = &ctx->job;
    bg_openai_opts opts = { .model = j->req.model, .size = j->req.size, .quality = j->req.quality };
    if (j->req.src_path) {
        reply_progress(j->req.client_fd, "Refining image with OpenAI");
        j->ok = bg_openai_edit(j->req.src_path, j->req.prompt, &opts, &j->res);
    } else {
        reply_progress(j->req.client_fd, "Generating image with OpenAI");
        j->ok = bg_openai_generate(j->req.prompt, &opts, &j->res);
    }
    if (!j->ok) snprintf(j->err, sizeof(j->err), "openai request failed (see daemon log)");
    else reply_progress(j->req.client_fd, "Image received");

    uint64_t one = 1;
    ssize_t wr = write(ctx->gen_evfd, &one, sizeof(one)); // wake the main loop
    (void)wr;
    return NULL;
}

static void request_free_fields(gen_request *r) {
    free(r->prompt); free(r->model); free(r->size); free(r->quality);
    free(r->output); free(r->src_path);
    r->prompt = r->model = r->size = r->quality = r->output = r->src_path = NULL;
}

// Allocate a request with copies of the given fields. `src_path` non-NULL marks
// this as an edit ("refine") of that image rather than a fresh generation.
static gen_request *request_new(int client_fd, const char *prompt, const char *model,
                                const char *size, const char *quality, const char *output,
                                const char *src_path) {
    gen_request *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->client_fd = client_fd;
    r->prompt   = strdup(prompt);
    r->model    = model    ? strdup(model)    : NULL;
    r->size     = size     ? strdup(size)     : NULL;
    r->quality  = quality  ? strdup(quality)  : NULL;
    r->output   = output   ? strdup(output)   : NULL;
    r->src_path = src_path ? strdup(src_path) : NULL;
    return r;
}

static void job_clear(gen_job *j) {
    bg_openai_free_result(&j->res);
    request_free_fields(&j->req);
    memset(j, 0, sizeof(*j));
    j->req.client_fd = -1;
}

static bool queue_push(daemon_ctx *ctx, gen_request *r) {
    if (ctx->q_len >= GEN_QUEUE_MAX) return false;
    r->next = NULL;
    if (ctx->q_tail) ctx->q_tail->next = r; else ctx->q_head = r;
    ctx->q_tail = r;
    ctx->q_len++;
    return true;
}

static gen_request *queue_pop(daemon_ctx *ctx) {
    gen_request *r = ctx->q_head;
    if (!r) return NULL;
    ctx->q_head = r->next;
    if (!ctx->q_head) ctx->q_tail = NULL;
    ctx->q_len--;
    r->next = NULL;
    return r;
}

// Take ownership of `r` and run it on the worker thread. Returns true if the
// worker started (the job now owns r's client_fd). On failure it replies and
// closes the fd itself; either way the caller must not touch the fd.
static bool start_job(daemon_ctx *ctx, gen_request *r) {
    gen_job *j = &ctx->job;
    memset(&j->res, 0, sizeof(j->res));
    j->ok = false;
    j->err[0] = '\0';
    j->req = *r;            // transfer owned strings + client_fd
    j->req.next = NULL;
    j->active = true;
    free(r);                // struct only; strings now owned by j->req

    if (pthread_create(&j->thread, NULL, gen_worker, ctx) != 0) {
        LOG_ERR("pthread_create: %s", strerror(errno));
        reply_err(j->req.client_fd, "could not start worker");
        if (j->req.client_fd >= 0) close(j->req.client_fd);
        request_free_fields(&j->req);
        j->active = false;
        return false;
    }
    LOG_INFO("worker started (queue depth %zu)", ctx->q_len);
    return true;
}

// Called on the main thread when the worker signals completion. Processes the
// finished job, then starts the next queued request (if any).
static void gen_complete(daemon_ctx *ctx) {
    uint64_t v;
    ssize_t rd = read(ctx->gen_evfd, &v, sizeof(v));
    (void)rd;

    gen_job *j = &ctx->job;
    if (!j->active) return;
    pthread_join(j->thread, NULL); // also publishes the worker's writes
    int cfd = j->req.client_fd;

    if (!j->ok) {
        reply_err(cfd, j->err[0] ? j->err : "generation failed");
    } else {
        reply_progress(cfd, "Rendering wallpaper");
        char id[BG_ID_MAX] = {0};
        bg_store_add(j->res.data, j->res.len, "png", j->req.prompt,
                     j->req.model   ? j->req.model   : BG_OPENAI_DEFAULT_MODEL,
                     j->req.size    ? j->req.size    : BG_OPENAI_DEFAULT_SIZE,
                     j->req.quality ? j->req.quality : BG_OPENAI_DEFAULT_QUALITY,
                     id, sizeof(id));

        bg_image img = {0};
        if (!bg_image_load_memory(j->res.data, j->res.len, &img)) {
            reply_err(cfd, "openai: response not decodable as image");
        } else {
            LOG_INFO("generate: rendered image %dx%d", img.width, img.height);
            bg_source src = { .kind = BG_SRC_IMAGE, .image = img };
            bool ok = apply_source(ctx->w, j->req.output, &src);
            bg_image_free(&img);
            if (ok) {
                if (id[0] && !j->req.output) bg_store_set_current(id);
                reply_ok(cfd);
            } else {
                reply_err(cfd, "set_source failed");
            }
        }
    }
    if (cfd >= 0) close(cfd);
    job_clear(j);

    // Kick off the next queued request, if any.
    gen_request *next = queue_pop(ctx);
    if (next) start_job(ctx, next);
}

// ---------- command dispatch ----------

// Hand a freshly built request to the worker, or queue it if one is running.
// Sets *fd_taken when the request now owns client_fd (caller must not close it).
// Replies and frees on out-of-memory / queue-full.
static void submit_request(daemon_ctx *ctx, int client_fd, gen_request *r, bool *fd_taken) {
    if (!r) {
        reply_err(client_fd, "out of memory");
    } else if (!ctx->job.active) {
        start_job(ctx, r);          // owns/closes the fd in all cases
        *fd_taken = true;
    } else if (queue_push(ctx, r)) {
        char qmsg[64];
        snprintf(qmsg, sizeof(qmsg), "Queued (position %zu)", ctx->q_len);
        reply_progress(client_fd, qmsg);
        LOG_INFO("request queued (depth %zu)", ctx->q_len);
        *fd_taken = true;           // queue owns the fd until processed
    } else {
        reply_err(client_fd, "queue full");
        request_free_fields(r);
        free(r);
    }
}

// Returns true if client_fd was handed to an async job (caller must NOT close).
static bool handle_command(daemon_ctx *ctx, int client_fd, const char *line) {
    bg_wayland *w = ctx->w;
    cJSON *root = cJSON_Parse(line);
    if (!root) { reply_err(client_fd, "invalid json"); return false; }

    const cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (!cJSON_IsString(cmd)) { reply_err(client_fd, "missing cmd"); cJSON_Delete(root); return false; }

    const char *c = cmd->valuestring;
    bool fd_taken = false;

    // Optional target output; NULL → all outputs.
    const cJSON *outv = cJSON_GetObjectItem(root, "output");
    const char *out = cJSON_IsString(outv) ? outv->valuestring : NULL;

    if (strcmp(c, "stop") == 0) {
        reply_ok(client_fd);
        ctx->running = false;
    } else if (strcmp(c, "color") == 0) {
        const cJSON *v = cJSON_GetObjectItem(root, "value");
        uint32_t col;
        if (!cJSON_IsString(v) || parse_hex_color(v->valuestring, &col) < 0) {
            reply_err(client_fd, "color: invalid value (expected RRGGBB)");
        } else {
            bg_source src = { .kind = BG_SRC_COLOR, .color_rrggbb = col };
            if (!apply_source(w, out, &src)) reply_err(client_fd, "set_source failed (bad output?)");
            else reply_ok(client_fd);
        }
    } else if (strcmp(c, "file") == 0) {
        const cJSON *p = cJSON_GetObjectItem(root, "path");
        if (!cJSON_IsString(p)) {
            reply_err(client_fd, "file: missing path");
        } else if (!apply_image_path(w, out, p->valuestring)) {
            reply_err(client_fd, "file: load failed (or bad output)");
        } else {
            reply_ok(client_fd);
        }
    } else if (strcmp(c, "generate") == 0) {
        const cJSON *prompt  = cJSON_GetObjectItem(root, "prompt");
        const cJSON *size    = cJSON_GetObjectItem(root, "size");
        const cJSON *quality = cJSON_GetObjectItem(root, "quality");
        const cJSON *model   = cJSON_GetObjectItem(root, "model");
        if (!cJSON_IsString(prompt)) {
            reply_err(client_fd, "generate: missing prompt");
        } else {
            gen_request *r = request_new(client_fd, prompt->valuestring,
                                 cJSON_IsString(model)   ? model->valuestring   : NULL,
                                 cJSON_IsString(size)    ? size->valuestring    : NULL,
                                 cJSON_IsString(quality) ? quality->valuestring : NULL,
                                 out, NULL);
            submit_request(ctx, client_fd, r, &fd_taken);
        }
    } else if (strcmp(c, "refine") == 0) {
        const cJSON *prompt  = cJSON_GetObjectItem(root, "prompt");
        const cJSON *srcv    = cJSON_GetObjectItem(root, "src");
        const cJSON *size    = cJSON_GetObjectItem(root, "size");
        const cJSON *quality = cJSON_GetObjectItem(root, "quality");
        const cJSON *model   = cJSON_GetObjectItem(root, "model");
        if (!cJSON_IsString(prompt)) {
            reply_err(client_fd, "refine: missing prompt");
        } else {
            // Resolve the source image: explicit --from, else the current one.
            bg_store_entry e;
            int got;
            if (cJSON_IsString(srcv)) {
                got = resolve_entry(srcv->valuestring, &e);
            } else {
                char cur[BG_ID_MAX];
                got = (bg_store_get_current(cur, sizeof(cur)) == 0)
                        ? bg_store_get(cur, &e) : -1;
            }
            if (got != 0) {
                reply_err(client_fd, cJSON_IsString(srcv)
                              ? "refine: no such source entry"
                              : "refine: no current wallpaper to refine");
            } else {
                // Default the output size to the source's size so the wallpaper
                // format stays stable across refinements (unless overridden).
                const char *eff_size = cJSON_IsString(size) ? size->valuestring
                                     : (e.size ? e.size : NULL);
                gen_request *r = request_new(client_fd, prompt->valuestring,
                                     cJSON_IsString(model)   ? model->valuestring   : NULL,
                                     eff_size,
                                     cJSON_IsString(quality) ? quality->valuestring : NULL,
                                     out, e.path);
                submit_request(ctx, client_fd, r, &fd_taken);
                bg_store_entry_clear(&e);
            }
        }
    } else if (strcmp(c, "restore") == 0) {
        const cJSON *idv = cJSON_GetObjectItem(root, "id");
        if (!cJSON_IsString(idv)) {
            reply_err(client_fd, "restore: missing id");
        } else {
            bg_store_entry e;
            if (resolve_entry(idv->valuestring, &e) != 0) {
                reply_err(client_fd, "restore: no such entry");
            } else {
                LOG_INFO("restore: %s (%s)", e.id, e.path);
                if (apply_image_path(w, out, e.path)) {
                    if (!out) bg_store_set_current(e.id);
                    reply_ok(client_fd);
                } else {
                    reply_err(client_fd, "restore: image load failed (or bad output)");
                }
                bg_store_entry_clear(&e);
            }
        }
    } else if (strcmp(c, "outputs") == 0) {
        char **names = NULL;
        size_t n = 0;
        if (bg_wayland_output_names(w, &names, &n) != 0) {
            reply_err(client_fd, "outputs: query failed");
        } else {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "ok", 1);
            cJSON *arr = cJSON_AddArrayToObject(o, "outputs");
            for (size_t i = 0; i < n; i++) {
                cJSON_AddItemToArray(arr, cJSON_CreateString(names[i]));
                free(names[i]);
            }
            free(names);
            char *s = cJSON_PrintUnformatted(o);
            bg_ipc_write_line(client_fd, s ? s : "{\"ok\":false}");
            free(s);
            cJSON_Delete(o);
        }
    } else {
        reply_err(client_fd, "unknown cmd");
    }

    cJSON_Delete(root);
    return fd_taken;
}

// ---------- main loop ----------

int bg_daemon_run(void) {
    bg_openai_global_init();

    daemon_ctx ctx = {0};
    ctx.running = true;
    ctx.job.req.client_fd = -1;

    ctx.w = bg_wayland_init();
    if (!ctx.w) { bg_openai_global_cleanup(); return 1; }

    ctx.listen_fd = bg_ipc_server_listen();
    if (ctx.listen_fd < 0) { bg_wayland_destroy(ctx.w); bg_openai_global_cleanup(); return 1; }

    ctx.gen_evfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (ctx.gen_evfd < 0) {
        LOG_ERR("eventfd: %s", strerror(errno));
        close(ctx.listen_fd);
        bg_wayland_destroy(ctx.w);
        bg_openai_global_cleanup();
        return 1;
    }

    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    // Restore the last wallpaper so it survives a daemon restart.
    char cur_id[BG_ID_MAX];
    if (bg_store_get_current(cur_id, sizeof(cur_id)) == 0) {
        bg_store_entry e;
        if (bg_store_get(cur_id, &e) == 0) {
            LOG_INFO("restoring last wallpaper %s", e.id);
            apply_image_path(ctx.w, NULL, e.path);
            bg_store_entry_clear(&e);
        }
    }

    int client_fd = -1;

    while (ctx.running && !g_signaled && !bg_wayland_should_exit(ctx.w)) {
        bg_wayland_flush(ctx.w);

        struct pollfd fds[4];
        int nfds = 0;
        int idx_wl  = nfds; fds[nfds++] = (struct pollfd){ .fd = bg_wayland_fd(ctx.w), .events = POLLIN };
        int idx_lis = nfds; fds[nfds++] = (struct pollfd){ .fd = ctx.listen_fd,        .events = POLLIN };
        int idx_gen = nfds; fds[nfds++] = (struct pollfd){ .fd = ctx.gen_evfd,         .events = POLLIN };
        int idx_cli = -1;
        if (client_fd >= 0) { idx_cli = nfds; fds[nfds++] = (struct pollfd){ .fd = client_fd, .events = POLLIN }; }

        int pr = poll(fds, nfds, 1000);
        if (pr < 0) {
            if (errno == EINTR) continue;
            LOG_ERR("poll: %s", strerror(errno));
            break;
        }

        if (fds[idx_wl].revents & POLLIN) {
            if (bg_wayland_dispatch(ctx.w) < 0) { LOG_ERR("wayland dispatch failed"); break; }
        }
        if (fds[idx_gen].revents & POLLIN) {
            gen_complete(&ctx);
        }
        if (fds[idx_lis].revents & POLLIN) {
            int cn = accept4(ctx.listen_fd, NULL, NULL, SOCK_CLOEXEC);
            if (cn < 0) {
                LOG_WARN("accept: %s", strerror(errno));
            } else {
                if (client_fd >= 0) close(client_fd);
                client_fd = cn;
            }
        }
        if (idx_cli >= 0 && (fds[idx_cli].revents & (POLLIN | POLLHUP | POLLERR))) {
            char *cline = NULL;
            if (bg_ipc_read_line(client_fd, &cline) < 0) {
                close(client_fd);
                client_fd = -1;
            } else {
                bool taken = handle_command(&ctx, client_fd, cline);
                free(cline);
                if (!taken && client_fd >= 0) close(client_fd);
                client_fd = -1; // ownership either closed above or handed to job
            }
        }
    }

    // If a worker is still in flight, wait for it so we don't leak the thread.
    if (ctx.job.active) {
        LOG_INFO("waiting for in-flight generation before shutdown");
        pthread_join(ctx.job.thread, NULL);
        if (ctx.job.req.client_fd >= 0) close(ctx.job.req.client_fd);
        job_clear(&ctx.job);
    }
    // Reject anything still queued.
    for (gen_request *r; (r = queue_pop(&ctx)); ) {
        reply_err(r->client_fd, "daemon shutting down");
        if (r->client_fd >= 0) close(r->client_fd);
        request_free_fields(r);
        free(r);
    }

    if (client_fd >= 0) close(client_fd);
    close(ctx.gen_evfd);
    close(ctx.listen_fd);

    char path[PATH_MAX];
    if (bg_ipc_socket_path(path, sizeof(path)) == 0) unlink(path);

    bg_wayland_destroy(ctx.w);
    bg_openai_global_cleanup();
    LOG_INFO("daemon shutdown");
    return 0;
}

// ---------- spawn helper ----------

int bg_daemon_spawn(const char *self_exe_path) {
    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERR("fork: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        setsid();
        int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }
        execl(self_exe_path, self_exe_path, "daemon", (char *)NULL);
        _exit(127);
    }

    for (int i = 0; i < 50; i++) {
        struct timespec ts = { .tv_nsec = 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        int fd = bg_ipc_client_connect();
        if (fd >= 0) { close(fd); return 0; }
        int status;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            LOG_ERR("daemon exited during startup (status %d)", status);
            return -1;
        }
    }
    LOG_ERR("daemon spawn: timed out waiting for socket");
    return -1;
}
