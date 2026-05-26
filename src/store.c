#include "store.h"
#include "log.h"

#include <cJSON.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// Kept well below BG_PATH_MAX so composed "<dir>/<id>.json" paths provably fit
// into BG_PATH_MAX buffers (silences -Wformat-truncation).
#define BG_DIR_MAX 2048
static char g_dir[BG_DIR_MAX];
static int  g_dir_ready = 0;

// ---------- paths ----------

static int resolve_dir(void) {
    const char *base = getenv("XDG_CACHE_HOME");
    int n;
    if (base && *base) {
        n = snprintf(g_dir, sizeof(g_dir), "%s/background", base);
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home) { LOG_ERR("store: HOME not set"); return -1; }
        n = snprintf(g_dir, sizeof(g_dir), "%s/.cache/background", home);
    }
    if (n < 0 || (size_t)n >= sizeof(g_dir)) return -1;
    return 0;
}

// mkdir -p for a single absolute path.
static int mkdir_p(const char *path) {
    char tmp[BG_PATH_MAX];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) return -1;
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0700) < 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0700) < 0 && errno != EEXIST) return -1;
    return 0;
}

int bg_store_init(void) {
    if (g_dir_ready) return 0;
    if (resolve_dir() < 0) return -1;
    if (mkdir_p(g_dir) < 0) {
        LOG_ERR("store: mkdir %s: %s", g_dir, strerror(errno));
        return -1;
    }
    g_dir_ready = 1;
    return 0;
}

const char *bg_store_dir(void) { return g_dir; }

// ---------- helpers ----------

static int write_file(const char *path, const void *data, size_t len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    size_t off = 0;
    const uint8_t *p = data;
    while (off < len) {
        ssize_t w = write(fd, p + off, len - off);
        if (w < 0) { if (errno == EINTR) continue; close(fd); return -1; }
        off += (size_t)w;
    }
    close(fd);
    return 0;
}

static char *read_file_str(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) { close(fd); return NULL; }
    for (;;) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); close(fd); return NULL; }
            buf = nb;
        }
        ssize_t r = read(fd, buf + len, cap - len - 1);
        if (r < 0) { if (errno == EINTR) continue; free(buf); close(fd); return NULL; }
        if (r == 0) break;
        len += (size_t)r;
    }
    buf[len] = '\0';
    close(fd);
    return buf;
}

static void make_id(char *out, size_t out_len) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char base[24];
    strftime(base, sizeof(base), "%Y%m%d-%H%M%S", &tm);
    // Dedup within the same second with a suffix.
    for (int i = 0; i < 1000; i++) {
        if (i == 0) snprintf(out, out_len, "%s", base);
        else        snprintf(out, out_len, "%s-%d", base, i);
        char probe[BG_PATH_MAX];
        snprintf(probe, sizeof(probe), "%s/%s.json", g_dir, out);
        if (access(probe, F_OK) != 0) return;
    }
}

// ---------- add ----------

int bg_store_add(const uint8_t *data, size_t len, const char *ext,
                 const char *prompt, const char *model,
                 const char *size, const char *quality,
                 char *out_id, size_t out_id_len) {
    if (bg_store_init() < 0) return -1;

    char id[BG_ID_MAX];
    make_id(id, sizeof(id));

    char img_path[BG_PATH_MAX], meta_path[BG_PATH_MAX];
    snprintf(img_path,  sizeof(img_path),  "%s/%s.%s", g_dir, id, ext ? ext : "png");
    snprintf(meta_path, sizeof(meta_path), "%s/%s.json", g_dir, id);

    if (write_file(img_path, data, len) < 0) {
        LOG_ERR("store: write %s: %s", img_path, strerror(errno));
        return -1;
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", id);
    cJSON_AddStringToObject(o, "file", img_path);
    if (prompt)  cJSON_AddStringToObject(o, "prompt",  prompt);
    if (model)   cJSON_AddStringToObject(o, "model",   model);
    if (size)    cJSON_AddStringToObject(o, "size",    size);
    if (quality) cJSON_AddStringToObject(o, "quality", quality);
    cJSON_AddNumberToObject(o, "ts", (double)time(NULL));
    char *js = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!js || write_file(meta_path, js, strlen(js)) < 0) {
        LOG_ERR("store: write %s failed", meta_path);
        free(js);
        unlink(img_path);
        return -1;
    }
    free(js);

    if (out_id) snprintf(out_id, out_id_len, "%s", id);
    LOG_INFO("store: saved %s", id);
    return 0;
}

// ---------- get ----------

static int parse_entry(const char *meta_path, bg_store_entry *e) {
    char *js = read_file_str(meta_path);
    if (!js) return -1;
    cJSON *o = cJSON_Parse(js);
    free(js);
    if (!o) return -1;

    memset(e, 0, sizeof(*e));
    const cJSON *v;
    if ((v = cJSON_GetObjectItem(o, "id")) && cJSON_IsString(v))
        snprintf(e->id, sizeof(e->id), "%s", v->valuestring);
    if ((v = cJSON_GetObjectItem(o, "file")) && cJSON_IsString(v))
        snprintf(e->path, sizeof(e->path), "%s", v->valuestring);
    if ((v = cJSON_GetObjectItem(o, "prompt"))  && cJSON_IsString(v)) e->prompt  = strdup(v->valuestring);
    if ((v = cJSON_GetObjectItem(o, "model"))   && cJSON_IsString(v)) e->model   = strdup(v->valuestring);
    if ((v = cJSON_GetObjectItem(o, "size"))    && cJSON_IsString(v)) e->size    = strdup(v->valuestring);
    if ((v = cJSON_GetObjectItem(o, "quality")) && cJSON_IsString(v)) e->quality = strdup(v->valuestring);
    if ((v = cJSON_GetObjectItem(o, "ts")) && cJSON_IsNumber(v)) e->ts = (long)v->valuedouble;
    cJSON_Delete(o);
    return 0;
}

int bg_store_get(const char *id, bg_store_entry *out) {
    if (bg_store_init() < 0) return -1;
    char meta_path[BG_PATH_MAX];
    snprintf(meta_path, sizeof(meta_path), "%s/%s.json", g_dir, id);
    return parse_entry(meta_path, out);
}

void bg_store_entry_clear(bg_store_entry *e) {
    if (!e) return;
    free(e->prompt); free(e->model); free(e->size); free(e->quality);
    e->prompt = e->model = e->size = e->quality = NULL;
}

void bg_store_free_list(bg_store_entry *list, size_t count) {
    if (!list) return;
    for (size_t i = 0; i < count; i++) bg_store_entry_clear(&list[i]);
    free(list);
}

// ---------- list ----------

static int cmp_ts_desc(const void *a, const void *b) {
    long ta = ((const bg_store_entry *)a)->ts;
    long tb = ((const bg_store_entry *)b)->ts;
    if (ta < tb) return 1;
    if (ta > tb) return -1;
    return 0;
}

int bg_store_list(bg_store_entry **out, size_t *count) {
    if (bg_store_init() < 0) return -1;
    DIR *d = opendir(g_dir);
    if (!d) { LOG_ERR("store: opendir %s: %s", g_dir, strerror(errno)); return -1; }

    size_t cap = 16, n = 0;
    bg_store_entry *arr = malloc(cap * sizeof(*arr));
    if (!arr) { closedir(d); return -1; }

    struct dirent *de;
    while ((de = readdir(d))) {
        size_t nlen = strlen(de->d_name);
        if (nlen < 6 || strcmp(de->d_name + nlen - 5, ".json") != 0) continue;
        char meta_path[BG_PATH_MAX];
        snprintf(meta_path, sizeof(meta_path), "%s/%s", g_dir, de->d_name);
        bg_store_entry e;
        if (parse_entry(meta_path, &e) != 0) continue;
        if (n == cap) {
            cap *= 2;
            bg_store_entry *na = realloc(arr, cap * sizeof(*arr));
            if (!na) { bg_store_free_list(arr, n); closedir(d); return -1; }
            arr = na;
        }
        arr[n++] = e;
    }
    closedir(d);

    qsort(arr, n, sizeof(*arr), cmp_ts_desc);
    *out = arr;
    *count = n;
    return 0;
}

// ---------- current ----------

int bg_store_set_current(const char *id) {
    if (bg_store_init() < 0) return -1;
    char p[BG_PATH_MAX];
    snprintf(p, sizeof(p), "%s/current", g_dir);
    return write_file(p, id, strlen(id));
}

int bg_store_prune(size_t keep, size_t *deleted) {
    if (bg_store_init() < 0) return -1;

    char cur[BG_ID_MAX] = {0};
    bg_store_get_current(cur, sizeof(cur));

    bg_store_entry *list = NULL;
    size_t n = 0;
    if (bg_store_list(&list, &n) != 0) return -1;

    size_t del = 0;
    for (size_t i = keep; i < n; i++) {
        if (cur[0] && strcmp(list[i].id, cur) == 0) continue; // never delete current
        char meta[BG_PATH_MAX];
        snprintf(meta, sizeof(meta), "%s/%s.json", g_dir, list[i].id);
        if (list[i].path[0]) unlink(list[i].path);
        unlink(meta);
        del++;
    }
    bg_store_free_list(list, n);
    if (deleted) *deleted = del;
    return 0;
}

int bg_store_get_current(char *out_id, size_t out_id_len) {
    if (bg_store_init() < 0) return -1;
    char p[BG_PATH_MAX];
    snprintf(p, sizeof(p), "%s/current", g_dir);
    char *s = read_file_str(p);
    if (!s) return -1;
    // strip trailing newline/space
    size_t l = strlen(s);
    while (l && (s[l-1] == '\n' || s[l-1] == ' ' || s[l-1] == '\r')) s[--l] = '\0';
    snprintf(out_id, out_id_len, "%s", s);
    free(s);
    return (out_id[0]) ? 0 : -1;
}
