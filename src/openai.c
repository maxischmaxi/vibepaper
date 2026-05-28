// OpenAI image generation + edit client.
//
// Generate — POST https://api.openai.com/v1/images/generations
//   Authorization: Bearer $OPENAI_API_KEY
//   Content-Type: application/json
//   Body (gpt-image-2):
//     { "model":"gpt-image-2", "prompt":"…", "n":1,
//       "size":"1536x1024", "quality":"medium" }
//
// Edit ("refine") — POST https://api.openai.com/v1/images/edits
//   Authorization: Bearer $OPENAI_API_KEY
//   Content-Type: multipart/form-data   (set by libcurl, do NOT send it)
//   Parts: image=<source png>, prompt, model, size, quality, n.
//   gpt-image-2 keeps inputs at high fidelity automatically, so we omit
//   "input_fidelity" (the model rejects it).
//
// Both endpoints: gpt-image-* always return base64 and reject "response_format",
// so we don't send it. Response: { "created":…, "data":[ { "b64_json":"…" } ] }

#include "openai.h"
#include "log.h"

#include <cJSON.h>
#include <curl/curl.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define API_URL  "https://api.openai.com/v1/images/generations"
#define EDIT_URL "https://api.openai.com/v1/images/edits"

#define DEFAULT_MODEL    BG_OPENAI_DEFAULT_MODEL
#define DEFAULT_SIZE     BG_OPENAI_DEFAULT_SIZE
#define DEFAULT_QUALITY  BG_OPENAI_DEFAULT_QUALITY

// ---------- growable buffer for libcurl writes ----------

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} buf_t;

static size_t buf_write(void *ptr, size_t size, size_t nmemb, void *userdata) {
    buf_t *b = userdata;
    size_t add = size * nmemb;
    if (b->len + add + 1 > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 4096;
        while (ncap < b->len + add + 1) ncap *= 2;
        char *nd = realloc(b->data, ncap);
        if (!nd) return 0;
        b->data = nd;
        b->cap = ncap;
    }
    memcpy(b->data + b->len, ptr, add);
    b->len += add;
    b->data[b->len] = '\0';
    return add;
}

// ---------- base64 decoder ----------

static int b64_decode(const char *src, uint8_t **out, size_t *out_len) {
    static int8_t T[256];
    static int init = 0;
    if (!init) {
        for (int i = 0; i < 256; i++) T[i] = -1;
        const char *alpha =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i++) T[(unsigned char)alpha[i]] = (int8_t)i;
        init = 1;
    }
    size_t src_len = strlen(src);
    uint8_t *buf = malloc(src_len * 3 / 4 + 4);
    if (!buf) return -1;
    size_t out_pos = 0;
    int val = 0, bits = 0;
    for (size_t i = 0; i < src_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        int8_t v = T[c];
        if (v < 0) { free(buf); return -1; }
        val = (val << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            buf[out_pos++] = (uint8_t)((val >> bits) & 0xFF);
        }
    }
    *out = buf;
    *out_len = out_pos;
    return 0;
}

// ---------- API key resolution ----------

static char *read_first_line(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char buf[1024];
    char *r = fgets(buf, sizeof(buf), f);
    fclose(f);
    if (!r) return NULL;
    size_t n = strlen(buf);
    while (n && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' ' || buf[n-1] == '\t'))
        buf[--n] = '\0';
    return n ? strdup(buf) : NULL;
}

// Resolve the key from (in order): explicit option, OPENAI_API_KEY env,
// OPENAI_API_KEY_FILE, or ~/.config/vibepaper/api_key. The file fallbacks let
// the daemon work when launched without a shell environment (e.g. from a
// compositor autostart). Returns a malloc'd string, or NULL.
static char *resolve_api_key(const bg_openai_opts *opts) {
    if (opts && opts->api_key && *opts->api_key) return strdup(opts->api_key);
    const char *env = getenv("OPENAI_API_KEY");
    if (env && *env) return strdup(env);
    const char *kf = getenv("OPENAI_API_KEY_FILE");
    if (kf && *kf) { char *k = read_first_line(kf); if (k) return k; }

    char path[1024];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        snprintf(path, sizeof(path), "%s/vibepaper/api_key", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home) return NULL;
        snprintf(path, sizeof(path), "%s/.config/vibepaper/api_key", home);
    }
    return read_first_line(path);
}

// ---------- public API ----------

void bg_openai_global_init(void)    { curl_global_init(CURL_GLOBAL_DEFAULT); }
void bg_openai_global_cleanup(void) { curl_global_cleanup(); }

// Run a request whose URL/method/body are already configured on `curl`, then
// decode data[0].b64_json into `out`. Sets the shared write callback, timeout
// and user-agent itself. `what` only labels the success log line. Shared by
// generate and edit (their only difference is how the body is attached).
static bool perform_image_request(CURL *curl, const char *what,
                                  bg_openai_result *out) {
    buf_t resp = {0};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buf_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 180L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "vibepaper/0.1");

    bool ok = false;
    CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);

    if (rc != CURLE_OK) {
        LOG_ERR("openai: curl: %s", curl_easy_strerror(rc));
    } else if (http >= 400) {
        LOG_ERR("openai: HTTP %ld: %.*s", http,
                (int)(resp.len < 800 ? resp.len : 800), resp.data ? resp.data : "");
    } else if (!resp.data) {
        LOG_ERR("openai: empty response");
    } else {
        cJSON *root = cJSON_Parse(resp.data);
        if (!root) {
            LOG_ERR("openai: invalid JSON");
        } else {
            cJSON *data = cJSON_GetObjectItem(root, "data");
            cJSON *first = data ? cJSON_GetArrayItem(data, 0) : NULL;
            cJSON *b64 = first ? cJSON_GetObjectItem(first, "b64_json") : NULL;
            if (!cJSON_IsString(b64)) {
                LOG_ERR("openai: missing data[0].b64_json");
            } else {
                uint8_t *bytes = NULL;
                size_t   nbytes = 0;
                if (b64_decode(b64->valuestring, &bytes, &nbytes) != 0) {
                    LOG_ERR("openai: base64 decode failed");
                } else {
                    out->data = bytes;
                    out->len = nbytes;
                    ok = true;
                    LOG_INFO("openai: %s received %zu bytes", what, nbytes);
                }
            }
            cJSON_Delete(root);
        }
    }
    free(resp.data);
    return ok;
}

bool bg_openai_generate(const char *prompt, const bg_openai_opts *opts,
                        bg_openai_result *out) {
    const char *model   = (opts && opts->model)   ? opts->model   : DEFAULT_MODEL;
    const char *size    = (opts && opts->size)    ? opts->size    : DEFAULT_SIZE;
    const char *quality = (opts && opts->quality) ? opts->quality : DEFAULT_QUALITY;
    char *api_key = resolve_api_key(opts);
    if (!api_key) {
        LOG_ERR("openai: no API key — set OPENAI_API_KEY, OPENAI_API_KEY_FILE, "
                "or write the key to ~/.config/vibepaper/api_key");
        return false;
    }
    char auth[1100];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
    free(api_key);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", model);
    cJSON_AddStringToObject(body, "prompt", prompt);
    cJSON_AddNumberToObject(body, "n", 1);
    cJSON_AddStringToObject(body, "size", size);
    cJSON_AddStringToObject(body, "quality", quality);
    // Note: gpt-image-* models always return base64 and reject "response_format"
    // (unlike the older dall-e models), so we don't send it.
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_str) { LOG_ERR("openai: json print failed"); return false; }

    LOG_INFO("openai: generating (model=%s size=%s quality=%s)…", model, size, quality);

    CURL *curl = curl_easy_init();
    if (!curl) { free(body_str); LOG_ERR("openai: curl_easy_init failed"); return false; }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, API_URL);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body_str));

    bool ok = perform_image_request(curl, "generate", out);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body_str);
    return ok;
}

bool bg_openai_edit(const char *image_path, const char *prompt,
                    const bg_openai_opts *opts, bg_openai_result *out) {
    const char *model   = (opts && opts->model)   ? opts->model   : DEFAULT_MODEL;
    const char *size    = (opts && opts->size)    ? opts->size    : DEFAULT_SIZE;
    const char *quality = (opts && opts->quality) ? opts->quality : DEFAULT_QUALITY;
    char *api_key = resolve_api_key(opts);
    if (!api_key) {
        LOG_ERR("openai: no API key — set OPENAI_API_KEY, OPENAI_API_KEY_FILE, "
                "or write the key to ~/.config/vibepaper/api_key");
        return false;
    }
    char auth[1100];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
    free(api_key);

    LOG_INFO("openai: editing %s (model=%s size=%s quality=%s)…",
             image_path, model, size, quality);

    CURL *curl = curl_easy_init();
    if (!curl) { LOG_ERR("openai: curl_easy_init failed"); return false; }

    // multipart/form-data: the source image plus text fields. libcurl sets the
    // Content-Type (with boundary) for CURLOPT_MIMEPOST, so we must not send it.
    curl_mime *mime = curl_mime_init(curl);
    curl_mimepart *part = curl_mime_addpart(mime);
    curl_mime_name(part, "image");
    if (curl_mime_filedata(part, image_path) != CURLE_OK) {
        LOG_ERR("openai: cannot attach source image %s", image_path);
        curl_mime_free(mime);
        curl_easy_cleanup(curl);
        return false;
    }
    curl_mime_type(part, "image/png");

    part = curl_mime_addpart(mime); curl_mime_name(part, "model");   curl_mime_data(part, model,   CURL_ZERO_TERMINATED);
    part = curl_mime_addpart(mime); curl_mime_name(part, "prompt");  curl_mime_data(part, prompt,  CURL_ZERO_TERMINATED);
    part = curl_mime_addpart(mime); curl_mime_name(part, "size");    curl_mime_data(part, size,    CURL_ZERO_TERMINATED);
    part = curl_mime_addpart(mime); curl_mime_name(part, "quality"); curl_mime_data(part, quality, CURL_ZERO_TERMINATED);
    part = curl_mime_addpart(mime); curl_mime_name(part, "n");       curl_mime_data(part, "1",      CURL_ZERO_TERMINATED);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth);
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, EDIT_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);

    bool ok = perform_image_request(curl, "edit", out);

    curl_slist_free_all(headers);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);
    return ok;
}

void bg_openai_free_result(bg_openai_result *res) {
    if (!res || !res->data) return;
    free(res->data);
    res->data = NULL;
    res->len = 0;
}
