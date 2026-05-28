// Provider-agnostic image generation/edit client (libcurl + cJSON).
//
// One entry point — bg_imagegen() — dispatches on opts->scheme to build the
// HTTP request (URL, data-driven auth header, JSON or multipart body) and parse
// the response into raw image bytes. Three synchronous schemes:
//
//   openai     POST {base}/images/generations        JSON {model,prompt,n,size,quality}
//              POST {base}/images/edits  (refine)     multipart (image,model,prompt,…)
//              response: data[0].b64_json (decode) or data[0].url (download)
//   gemini     POST {base}/models/{model}:generateContent
//              JSON {contents:[{parts:[(inlineData?),{text}]}], generationConfig}
//              response: candidates[0].content.parts[].inlineData.data (base64)
//   stability  POST {base}/v2beta/stable-image/generate/core   multipart, Accept image/*
//              response: raw image bytes in the body
//
// Auth is data-driven (opts->auth_header_name + auth_value_prefix), so OpenAI-
// compatible providers (xAI, Together, Azure, …) need no code here.

#include "imagegen.h"
#include "log.h"

#include <cJSON.h>
#include <curl/curl.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------- growable buffer for libcurl writes ----------

typedef struct { char *data; size_t len, cap; } buf_t;

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

// ---------- base64 ----------

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
    unsigned int val = 0;   // unsigned: the running accumulator overflows int
    int bits = 0;           // (signed-shift UB) once enough groups pile up
    for (size_t i = 0; i < src_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        int8_t v = T[c];
        if (v < 0) { free(buf); return -1; }
        val = (val << 6) | (unsigned int)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; buf[out_pos++] = (uint8_t)((val >> bits) & 0xFF); }
    }
    *out = buf;
    *out_len = out_pos;
    return 0;
}

// Standard base64 (no line wrapping). malloc'd NUL-terminated string / NULL.
static char *b64_encode(const uint8_t *data, size_t len) {
    static const char A[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t olen = 4 * ((len + 2) / 3);
    char *out = malloc(olen + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = (uint32_t)data[i] << 16;
        if (i + 1 < len) n |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) n |= (uint32_t)data[i + 2];
        out[o++] = A[(n >> 18) & 63];
        out[o++] = A[(n >> 12) & 63];
        out[o++] = (i + 1 < len) ? A[(n >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? A[n & 63] : '=';
    }
    out[o] = '\0';
    return out;
}

// ---------- misc helpers ----------

static char *join_url(const char *base, const char *path) {
    size_t n = strlen(base) + strlen(path) + 1;
    char *u = malloc(n);
    if (!u) return NULL;
    snprintf(u, n, "%s%s", base, path);
    return u;
}

// Map a content-type / mime string to a file extension literal.
static const char *ext_from_ct(const char *ct) {
    if (!ct) return "png";
    if (strstr(ct, "jpeg") || strstr(ct, "jpg")) return "jpeg";
    if (strstr(ct, "webp")) return "webp";
    return "png";
}

static const char *ext_from_curl(CURL *curl) {
    char *ct = NULL;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);
    return ext_from_ct(ct);
}

static uint8_t *read_file_bytes(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, len = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) { fclose(f); return NULL; }
    for (;;) {
        if (len == cap) {
            cap *= 2;
            uint8_t *nb = realloc(buf, cap);
            if (!nb) { free(buf); fclose(f); return NULL; }
            buf = nb;
        }
        size_t r = fread(buf + len, 1, cap - len, f);
        len += r;
        if (r == 0) break;
    }
    fclose(f);
    *out_len = len;
    return buf;
}

// Append the data-driven auth header ("<name>: <prefix><key>").
static struct curl_slist *append_auth(struct curl_slist *h, const bg_gen_opts *o) {
    size_t n = strlen(o->auth_header_name) + 2 + strlen(o->auth_value_prefix)
             + strlen(o->api_key) + 1;
    char *line = malloc(n);
    if (!line) return h;
    snprintf(line, n, "%s: %s%s", o->auth_header_name, o->auth_value_prefix, o->api_key);
    struct curl_slist *r = curl_slist_append(h, line);
    free(line);
    return r;
}

// Run a configured request; fill *resp (caller frees resp->data) and *http.
// Returns true if the transfer completed (any HTTP status), false on transport
// error. Shared timeout/UA live here.
static bool perform_request(CURL *curl, const char *label, buf_t *resp, long *http) {
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buf_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 180L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "vibepaper/0.1");
    CURLcode rc = curl_easy_perform(curl);
    *http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http);
    if (rc != CURLE_OK) { LOG_ERR("%s: curl: %s", label, curl_easy_strerror(rc)); return false; }
    return true;
}

// GET an image URL into out. FOLLOWLOCATION stays OFF (security model: no
// redirects). Used for openai-scheme responses that return data[0].url.
static bool download_url(const char *url, bg_gen_result *out) {
    CURL *curl = curl_easy_init();
    if (!curl) { LOG_ERR("download: curl init failed"); return false; }
    buf_t resp = {0};
    long http = 0;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    bool ok = perform_request(curl, "download", &resp, &http);
    if (ok && http >= 400) { LOG_ERR("download: HTTP %ld", http); ok = false; }
    else if (ok && (!resp.data || resp.len == 0)) { LOG_ERR("download: empty body"); ok = false; }
    if (ok) {
        out->data = (uint8_t *)resp.data;
        out->len = resp.len;
        out->ext = ext_from_curl(curl);
        resp.data = NULL;  // ownership transferred to out
    }
    free(resp.data);
    curl_easy_cleanup(curl);
    return ok;
}

// ---------- openai scheme ----------

// Pure (testable) request-body builders return malloc'd JSON / NULL.
static char *build_openai_body(const bg_gen_opts *o, const char *prompt) {
    cJSON *body = cJSON_CreateObject();
    if (!body) return NULL;
    cJSON_AddStringToObject(body, "model", o->model);
    cJSON_AddStringToObject(body, "prompt", prompt);
    cJSON_AddNumberToObject(body, "n", 1);
    if (o->size)    cJSON_AddStringToObject(body, "size", o->size);
    if (o->quality) cJSON_AddStringToObject(body, "quality", o->quality);
    char *s = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    return s;
}

static bool openai_parse(const char *body, bg_gen_result *out) {
    cJSON *root = cJSON_Parse(body);
    if (!root) { LOG_ERR("openai: invalid JSON response"); return false; }
    bool ok = false;
    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *first = data ? cJSON_GetArrayItem(data, 0) : NULL;
    if (!first) {
        LOG_ERR("openai: response has no data[0]");
    } else {
        cJSON *b64 = cJSON_GetObjectItem(first, "b64_json");
        cJSON *url = cJSON_GetObjectItem(first, "url");
        if (cJSON_IsString(b64)) {
            uint8_t *bytes = NULL; size_t n = 0;
            if (b64_decode(b64->valuestring, &bytes, &n) == 0) {
                out->data = bytes; out->len = n; out->ext = "png"; ok = true;
            } else LOG_ERR("openai: base64 decode failed");
        } else if (cJSON_IsString(url)) {
            ok = download_url(url->valuestring, out);
        } else {
            LOG_ERR("openai: response has neither b64_json nor url");
        }
    }
    cJSON_Delete(root);
    return ok;
}

static bool openai_generate(const bg_gen_opts *o, const char *prompt, bg_gen_result *out) {
    char *bs = build_openai_body(o, prompt);
    if (!bs) { LOG_ERR("openai: json build failed"); return false; }
    char *url = join_url(o->base_url, "/images/generations");
    CURL *curl = url ? curl_easy_init() : NULL;
    if (!curl) { free(url); free(bs); LOG_ERR("openai: curl init failed"); return false; }

    struct curl_slist *h = NULL;
    h = append_auth(h, o);
    h = curl_slist_append(h, "Content-Type: application/json");
    h = curl_slist_append(h, "Accept: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(bs));

    buf_t resp = {0}; long http = 0;
    bool ok = perform_request(curl, "openai", &resp, &http);
    if (ok && http >= 400) {
        LOG_ERR("openai: HTTP %ld: %.*s", http,
                (int)(resp.len < 800 ? resp.len : 800), resp.data ? resp.data : "");
        ok = false;
    } else if (ok && !resp.data) { LOG_ERR("openai: empty response"); ok = false; }
    else if (ok) ok = openai_parse(resp.data, out);

    free(resp.data); curl_slist_free_all(h); curl_easy_cleanup(curl); free(url); free(bs);
    return ok;
}

static bool openai_edit(const bg_gen_opts *o, const char *prompt,
                        const char *src_path, bg_gen_result *out) {
    char *url = join_url(o->base_url, "/images/edits");
    CURL *curl = url ? curl_easy_init() : NULL;
    if (!curl) { free(url); LOG_ERR("openai: curl init failed"); return false; }

    curl_mime *mime = curl_mime_init(curl);
    curl_mimepart *part = curl_mime_addpart(mime);
    curl_mime_name(part, "image");
    if (curl_mime_filedata(part, src_path) != CURLE_OK) {
        LOG_ERR("openai: cannot attach source image %s", src_path);
        curl_mime_free(mime); curl_easy_cleanup(curl); free(url);
        return false;
    }
    curl_mime_type(part, "image/png");
    part = curl_mime_addpart(mime); curl_mime_name(part, "model");  curl_mime_data(part, o->model,  CURL_ZERO_TERMINATED);
    part = curl_mime_addpart(mime); curl_mime_name(part, "prompt"); curl_mime_data(part, prompt,    CURL_ZERO_TERMINATED);
    if (o->size)    { part = curl_mime_addpart(mime); curl_mime_name(part, "size");    curl_mime_data(part, o->size,    CURL_ZERO_TERMINATED); }
    if (o->quality) { part = curl_mime_addpart(mime); curl_mime_name(part, "quality"); curl_mime_data(part, o->quality, CURL_ZERO_TERMINATED); }
    part = curl_mime_addpart(mime); curl_mime_name(part, "n"); curl_mime_data(part, "1", CURL_ZERO_TERMINATED);

    struct curl_slist *h = NULL;
    h = append_auth(h, o);
    h = curl_slist_append(h, "Accept: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);

    buf_t resp = {0}; long http = 0;
    bool ok = perform_request(curl, "openai", &resp, &http);
    if (ok && http >= 400) {
        LOG_ERR("openai: HTTP %ld: %.*s", http,
                (int)(resp.len < 800 ? resp.len : 800), resp.data ? resp.data : "");
        ok = false;
    } else if (ok && !resp.data) { LOG_ERR("openai: empty response"); ok = false; }
    else if (ok) ok = openai_parse(resp.data, out);

    free(resp.data); curl_slist_free_all(h); curl_mime_free(mime); curl_easy_cleanup(curl); free(url);
    return ok;
}

// ---------- gemini scheme ----------

static char *build_gemini_body(const bg_gen_opts *o, const char *prompt,
                               const char *src_path) {
    cJSON *body = cJSON_CreateObject();
    if (!body) return NULL;
    cJSON *contents = cJSON_AddArrayToObject(body, "contents");
    cJSON *content = cJSON_CreateObject();
    cJSON_AddItemToArray(contents, content);
    cJSON *parts = cJSON_AddArrayToObject(content, "parts");

    if (src_path) {  // image-to-image / refine: prepend the source as inlineData
        size_t slen = 0;
        uint8_t *sbytes = read_file_bytes(src_path, &slen);
        if (!sbytes) { LOG_ERR("gemini: cannot read source image %s", src_path); cJSON_Delete(body); return NULL; }
        char *b64 = b64_encode(sbytes, slen);
        free(sbytes);
        if (!b64) { LOG_ERR("gemini: out of memory encoding source"); cJSON_Delete(body); return NULL; }
        cJSON *ip = cJSON_CreateObject();
        cJSON_AddItemToArray(parts, ip);
        cJSON *inl = cJSON_AddObjectToObject(ip, "inlineData");
        cJSON_AddStringToObject(inl, "mimeType", "image/png");
        cJSON_AddStringToObject(inl, "data", b64);
        free(b64);
    }
    cJSON *tp = cJSON_CreateObject();
    cJSON_AddItemToArray(parts, tp);
    cJSON_AddStringToObject(tp, "text", prompt);

    if (o->size && (!strcmp(o->size, "1K") || !strcmp(o->size, "2K") || !strcmp(o->size, "4K"))) {
        cJSON *gc = cJSON_AddObjectToObject(body, "generationConfig");
        cJSON_AddStringToObject(gc, "imageSize", o->size);
    }
    char *s = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    return s;
}

static bool gemini_parse(const char *body, bg_gen_result *out) {
    cJSON *root = cJSON_Parse(body);
    if (!root) { LOG_ERR("gemini: invalid JSON response"); return false; }
    bool ok = false;
    cJSON *cands = cJSON_GetObjectItem(root, "candidates");
    cJSON *c0 = cands ? cJSON_GetArrayItem(cands, 0) : NULL;
    cJSON *content = c0 ? cJSON_GetObjectItem(c0, "content") : NULL;
    cJSON *parts = content ? cJSON_GetObjectItem(content, "parts") : NULL;
    if (cJSON_IsArray(parts)) {
        cJSON *p;
        cJSON_ArrayForEach(p, parts) {
            cJSON *inl = cJSON_GetObjectItem(p, "inlineData");
            if (!inl) inl = cJSON_GetObjectItem(p, "inline_data");
            cJSON *d = inl ? cJSON_GetObjectItem(inl, "data") : NULL;
            if (cJSON_IsString(d)) {
                uint8_t *bytes = NULL; size_t n = 0;
                if (b64_decode(d->valuestring, &bytes, &n) == 0) {
                    cJSON *mt = cJSON_GetObjectItem(inl, "mimeType");
                    if (!mt) mt = cJSON_GetObjectItem(inl, "mime_type");
                    out->data = bytes; out->len = n;
                    out->ext = ext_from_ct(cJSON_IsString(mt) ? mt->valuestring : NULL);
                    ok = true;
                } else LOG_ERR("gemini: base64 decode failed");
                break;
            }
        }
    }
    if (!ok) LOG_ERR("gemini: response had no inline image data");
    cJSON_Delete(root);
    return ok;
}

static bool gemini_request(const bg_gen_opts *o, const char *prompt,
                           const char *src_path, bg_gen_result *out) {
    char *bs = build_gemini_body(o, prompt, src_path);
    if (!bs) return false;  // build logs on failure

    size_t pn = strlen("/models/") + strlen(o->model) + strlen(":generateContent") + 1;
    char *mpath = malloc(pn);
    char *url = NULL;
    if (mpath) {
        snprintf(mpath, pn, "/models/%s:generateContent", o->model);
        url = join_url(o->base_url, mpath);
    }
    CURL *curl = url ? curl_easy_init() : NULL;
    if (!curl) { free(mpath); free(url); free(bs); LOG_ERR("gemini: curl init failed"); return false; }

    struct curl_slist *h = NULL;
    h = append_auth(h, o);
    h = curl_slist_append(h, "Content-Type: application/json");
    h = curl_slist_append(h, "Accept: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(bs));

    buf_t resp = {0}; long http = 0;
    bool ok = perform_request(curl, "gemini", &resp, &http);
    if (ok && http >= 400) {
        LOG_ERR("gemini: HTTP %ld: %.*s", http,
                (int)(resp.len < 800 ? resp.len : 800), resp.data ? resp.data : "");
        ok = false;
    } else if (ok && !resp.data) { LOG_ERR("gemini: empty response"); ok = false; }
    else if (ok) ok = gemini_parse(resp.data, out);

    free(resp.data); curl_slist_free_all(h); curl_easy_cleanup(curl); free(url); free(mpath); free(bs);
    return ok;
}

// ---------- stability scheme ----------

// "16:9", "1:1", … — passed through as aspect_ratio. Anything else is ignored.
static bool looks_like_aspect(const char *s) {
    const char *colon = strchr(s, ':');
    if (!colon || colon == s || !colon[1]) return false;
    for (const char *p = s; *p; p++)
        if (*p != ':' && (*p < '0' || *p > '9')) return false;
    return true;
}

static bool stability_generate(const bg_gen_opts *o, const char *prompt, bg_gen_result *out) {
    char *url = join_url(o->base_url, "/v2beta/stable-image/generate/core");
    CURL *curl = url ? curl_easy_init() : NULL;
    if (!curl) { free(url); LOG_ERR("stability: curl init failed"); return false; }

    curl_mime *mime = curl_mime_init(curl);
    curl_mimepart *part;
    part = curl_mime_addpart(mime); curl_mime_name(part, "prompt");        curl_mime_data(part, prompt, CURL_ZERO_TERMINATED);
    part = curl_mime_addpart(mime); curl_mime_name(part, "output_format"); curl_mime_data(part, "png",  CURL_ZERO_TERMINATED);
    if (o->size && looks_like_aspect(o->size)) {
        part = curl_mime_addpart(mime); curl_mime_name(part, "aspect_ratio"); curl_mime_data(part, o->size, CURL_ZERO_TERMINATED);
    }

    struct curl_slist *h = NULL;
    h = append_auth(h, o);
    h = curl_slist_append(h, "Accept: image/*");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);

    buf_t resp = {0}; long http = 0;
    bool ok = perform_request(curl, "stability", &resp, &http);
    if (ok && http >= 400) {
        LOG_ERR("stability: HTTP %ld: %.*s", http,
                (int)(resp.len < 800 ? resp.len : 800), resp.data ? resp.data : "");
        ok = false;
    } else if (ok && (!resp.data || resp.len == 0)) { LOG_ERR("stability: empty response"); ok = false; }
    else if (ok) {
        out->data = (uint8_t *)resp.data;   // body IS the image
        out->len = resp.len;
        out->ext = ext_from_curl(curl);
        resp.data = NULL;                    // ownership transferred
    }

    free(resp.data); curl_slist_free_all(h); curl_mime_free(mime); curl_easy_cleanup(curl); free(url);
    return ok;
}

// ---------- public API ----------

void bg_imagegen_global_init(void)    { curl_global_init(CURL_GLOBAL_DEFAULT); }
void bg_imagegen_global_cleanup(void) { curl_global_cleanup(); }

bool bg_imagegen(const bg_gen_opts *o, const char *prompt,
                 const char *src_path, bg_gen_result *out) {
    out->data = NULL; out->len = 0; out->ext = "png";
    if (!o->api_key || !*o->api_key) {
        LOG_ERR("imagegen: no API key for provider '%s'", o->provider ? o->provider : "?");
        return false;
    }
    switch (o->scheme) {
    case BG_SCHEME_OPENAI:
        return src_path ? openai_edit(o, prompt, src_path, out)
                        : openai_generate(o, prompt, out);
    case BG_SCHEME_GEMINI:
        return gemini_request(o, prompt, src_path, out);
    case BG_SCHEME_STABILITY:
        if (src_path) { LOG_ERR("stability: refine/edit not supported (needs an inpaint mask)"); return false; }
        return stability_generate(o, prompt, out);
    }
    LOG_ERR("imagegen: unknown scheme");
    return false;
}

void bg_imagegen_free_result(bg_gen_result *res) {
    if (!res || !res->data) return;
    free(res->data);
    res->data = NULL;
    res->len = 0;
}
