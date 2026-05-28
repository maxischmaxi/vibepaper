// Provider resolution: maps a provider name (built-in preset or config entry)
// plus optional CLI/IPC overrides into a fully-resolved backend (scheme,
// base_url, model, auth header, API key). Config lives in a JSON file so the
// daemon — launched by the compositor without a shell environment — can still
// find keys and provider definitions. API keys are NEVER taken from the CLI.

#include "provider.h"
#include "log.h"

#include <cJSON.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------- built-in presets ----------

typedef struct {
    const char *name;
    bg_scheme   scheme;
    const char *base_url;        // no trailing slash
    const char *default_model;   // overridable; documented as "may drift"
    const char *auth_header;
    const char *auth_prefix;
    const char *env_var;         // conventional key env var
} preset;

static const preset PRESETS[] = {
    {"openai",    BG_SCHEME_OPENAI,    "https://api.openai.com/v1",                       "gpt-image-2",                            "Authorization",  "Bearer ", "OPENAI_API_KEY"},
    {"xai",       BG_SCHEME_OPENAI,    "https://api.x.ai/v1",                             "grok-2-image",                           "Authorization",  "Bearer ", "XAI_API_KEY"},
    {"together",  BG_SCHEME_OPENAI,    "https://api.together.xyz/v1",                     "black-forest-labs/FLUX.1-schnell-Free",  "Authorization",  "Bearer ", "TOGETHER_API_KEY"},
    {"gemini",    BG_SCHEME_GEMINI,    "https://generativelanguage.googleapis.com/v1beta","gemini-2.5-flash-image",                 "x-goog-api-key", "",        "GEMINI_API_KEY"},
    {"stability", BG_SCHEME_STABILITY, "https://api.stability.ai",                        "core",                                   "Authorization",  "Bearer ", "STABILITY_API_KEY"},
};
#define N_PRESETS ((size_t)(sizeof(PRESETS) / sizeof(PRESETS[0])))

static const preset *preset_find(const char *name) {
    for (size_t i = 0; i < N_PRESETS; i++)
        if (strcmp(PRESETS[i].name, name) == 0) return &PRESETS[i];
    return NULL;
}

// ---------- scheme <-> string ----------

int bg_scheme_from_string(const char *s, bg_scheme *out) {
    if (!s) return -1;
    if      (!strcmp(s, "openai"))    *out = BG_SCHEME_OPENAI;
    else if (!strcmp(s, "gemini"))    *out = BG_SCHEME_GEMINI;
    else if (!strcmp(s, "stability")) *out = BG_SCHEME_STABILITY;
    else return -1;
    return 0;
}

const char *bg_scheme_to_string(bg_scheme s) {
    switch (s) {
    case BG_SCHEME_OPENAI:    return "openai";
    case BG_SCHEME_GEMINI:    return "gemini";
    case BG_SCHEME_STABILITY: return "stability";
    }
    return "openai";
}

// Per-scheme fallback base_url + auth (used for fully-custom providers). The
// openai scheme has no canonical host, so a custom openai provider must supply
// base_url itself.
static void scheme_defaults(bg_scheme s, const char **base,
                            const char **ah, const char **ap) {
    switch (s) {
    case BG_SCHEME_GEMINI:
        *base = "https://generativelanguage.googleapis.com/v1beta";
        *ah = "x-goog-api-key"; *ap = ""; break;
    case BG_SCHEME_STABILITY:
        *base = "https://api.stability.ai";
        *ah = "Authorization"; *ap = "Bearer "; break;
    case BG_SCHEME_OPENAI:
    default:
        *base = NULL;
        *ah = "Authorization"; *ap = "Bearer "; break;
    }
}

// ---------- small file/path helpers ----------

static int config_dir(char *out, size_t n) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    int w;
    if (xdg && *xdg) w = snprintf(out, n, "%s/vibepaper", xdg);
    else {
        const char *home = getenv("HOME");
        if (!home || !*home) return -1;
        w = snprintf(out, n, "%s/.config/vibepaper", home);
    }
    return (w < 0 || (size_t)w >= n) ? -1 : 0;
}

// Expand a leading "~/" to $HOME. Always returns a malloc'd string (or NULL).
static char *expand_tilde(const char *path) {
    if (path[0] == '~' && path[1] == '/') {
        const char *home = getenv("HOME");
        if (home && *home) {
            size_t n = strlen(home) + strlen(path);  // +slash slack, -'~'
            char *r = malloc(n + 1);
            if (!r) return NULL;
            snprintf(r, n + 1, "%s%s", home, path + 1);
            return r;
        }
    }
    return strdup(path);
}

// First non-empty line of a file, trailing whitespace stripped. malloc'd / NULL.
static char *read_first_line(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char buf[4096];
    char *r = fgets(buf, sizeof(buf), f);
    fclose(f);
    if (!r) return NULL;
    size_t n = strlen(buf);
    while (n && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' ' || buf[n-1] == '\t'))
        buf[--n] = '\0';
    return n ? strdup(buf) : NULL;
}

// Whole file as a NUL-terminated string (capped at 1 MiB). malloc'd / NULL.
static char *read_whole_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) { fclose(f); return NULL; }
    for (;;) {
        if (len + 1 >= cap) {
            if (cap > 1024u * 1024u) { free(buf); fclose(f); return NULL; }
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); fclose(f); return NULL; }
            buf = nb;
        }
        size_t r = fread(buf + len, 1, cap - len - 1, f);
        len += r;
        if (r == 0) break;
    }
    fclose(f);
    buf[len] = '\0';
    return buf;
}

static char *strdup_trim_slash(const char *s) {
    size_t n = strlen(s);
    while (n > 0 && s[n-1] == '/') n--;
    char *r = malloc(n + 1);
    if (!r) return NULL;
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

// ---------- config (lazy singleton) ----------

// Parsed ~/.config/vibepaper/config.json, loaded once. Touched only on the
// worker thread (jobs are serialized) and on the CLI side by `providers`;
// never concurrently, so no lock is needed. Kept for process lifetime (a
// still-reachable singleton, not a leak).
static cJSON *g_config;
static bool   g_config_loaded;

static const cJSON *config_get(void) {
    if (g_config_loaded) return g_config;
    g_config_loaded = true;
    char dir[2048];
    if (config_dir(dir, sizeof(dir)) != 0) return NULL;
    char path[2200];
    snprintf(path, sizeof(path), "%s/config.json", dir);
    char *txt = read_whole_file(path);
    if (!txt) return NULL;
    g_config = cJSON_Parse(txt);
    free(txt);
    if (!g_config) LOG_WARN("config: %s is not valid JSON — ignoring", path);
    return g_config;
}

static const char *cfg_str(const cJSON *o, const char *key) {
    if (!o) return NULL;
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static const cJSON *config_provider(const cJSON *cfg, const char *name) {
    if (!cfg) return NULL;
    const cJSON *provs = cJSON_GetObjectItem(cfg, "providers");
    if (!cJSON_IsObject(provs)) return NULL;
    return cJSON_GetObjectItem(provs, name);
}

// ---------- key resolution ----------

// First hit wins. NEVER reads keys from the CLI/IPC. Returns malloc'd / NULL.
static char *resolve_key(const char *name, const cJSON *ce,
                         const char *env_var, bg_scheme scheme) {
    // 1. config: literal key, or key_file.
    const char *k = cfg_str(ce, "key");
    if (k && *k) return strdup(k);
    const char *kf = cfg_str(ce, "key_file");
    if (kf && *kf) {
        char *p = expand_tilde(kf);
        char *r = p ? read_first_line(p) : NULL;
        free(p);
        if (r) return r;
    }
    // 2. conventional environment variable.
    if (env_var && *env_var) {
        const char *e = getenv(env_var);
        if (e && *e) return strdup(e);
    }
    // 3. ~/.config/vibepaper/keys/<name>.
    char dir[2048];
    if (config_dir(dir, sizeof(dir)) == 0) {
        char path[2300];
        snprintf(path, sizeof(path), "%s/keys/%s", dir, name);
        char *r = read_first_line(path);
        if (r) return r;
    }
    // 4. back-compat for the default openai provider only.
    if (scheme == BG_SCHEME_OPENAI && strcmp(name, "openai") == 0) {
        const char *kfe = getenv("OPENAI_API_KEY_FILE");
        if (kfe && *kfe) { char *r = read_first_line(kfe); if (r) return r; }
        if (config_dir(dir, sizeof(dir)) == 0) {
            char path[2300];
            snprintf(path, sizeof(path), "%s/api_key", dir);
            char *r = read_first_line(path);
            if (r) return r;
        }
    }
    return NULL;
}

// ---------- public: resolve ----------

int bg_provider_resolve(const char *req_name, const char *ov_base,
                        const char *ov_scheme, const char *ov_model,
                        bg_provider *out) {
    memset(out, 0, sizeof(*out));
    const cJSON *cfg = config_get();

    // 1. effective name.
    const char *name = (req_name && *req_name) ? req_name : NULL;
    if (!name) name = cfg_str(cfg, "default_provider");
    if (!name || !*name) name = getenv("VIBEPAPER_PROVIDER");
    if (!name || !*name) name = BG_DEFAULT_PROVIDER;

    const preset    *ps = preset_find(name);
    const cJSON     *ce = config_provider(cfg, name);

    if (!ps && !ce && !(ov_scheme && *ov_scheme)) {
        LOG_ERR("provider: unknown provider '%s' — define it in config.json "
                "with a \"scheme\", or pass --scheme", name);
        return -1;
    }

    // 2. scheme: override > config > preset.
    bg_scheme scheme = BG_SCHEME_OPENAI;
    bool have_scheme = false;
    if (ov_scheme && *ov_scheme) {
        if (bg_scheme_from_string(ov_scheme, &scheme) != 0) {
            LOG_ERR("provider: unknown scheme '%s' (openai|gemini|stability)", ov_scheme);
            return -1;
        }
        have_scheme = true;
    } else {
        const char *cs = cfg_str(ce, "scheme");
        if (cs) {
            if (bg_scheme_from_string(cs, &scheme) != 0) {
                LOG_ERR("provider: '%s' has unknown scheme '%s'", name, cs);
                return -1;
            }
            have_scheme = true;
        }
    }
    if (!have_scheme && ps) { scheme = ps->scheme; have_scheme = true; }
    if (!have_scheme) {
        LOG_ERR("provider: '%s' has no scheme — set \"scheme\" in config.json "
                "or pass --scheme", name);
        return -1;
    }

    const char *def_base, *def_ah, *def_ap;
    scheme_defaults(scheme, &def_base, &def_ah, &def_ap);

    // 3. base_url: override > config > preset > scheme default.
    const char *base = (ov_base && *ov_base) ? ov_base : NULL;
    if (!base) base = cfg_str(ce, "base_url");
    if (!base && ps) base = ps->base_url;
    if (!base) base = def_base;
    if (!base || !*base) {
        LOG_ERR("provider: '%s' needs a base_url — set it in config.json or "
                "pass --base-url", name);
        return -1;
    }

    // 4. model: override > config > preset > default.
    const char *model = (ov_model && *ov_model) ? ov_model : NULL;
    if (!model) model = cfg_str(ce, "model");
    if (!model && ps) model = ps->default_model;
    if (!model) model = BG_DEFAULT_MODEL;

    // 5. auth header: config > preset > scheme default.
    const char *ah = cfg_str(ce, "auth_header_name");
    if (!ah && ps) ah = ps->auth_header;
    if (!ah) ah = def_ah;
    const char *ap = cfg_str(ce, "auth_value_prefix");
    if (!ap && ps) ap = ps->auth_prefix;
    if (!ap) ap = def_ap;

    // 6. key.
    const char *env_var = cfg_str(ce, "env_var");
    if (!env_var && ps) env_var = ps->env_var;
    char *key = resolve_key(name, ce, env_var, scheme);

    // 7. materialise (own every string).
    out->name             = strdup(name);
    out->scheme           = scheme;
    out->base_url         = strdup_trim_slash(base);
    out->model            = strdup(model);
    out->auth_header_name  = strdup(ah);
    out->auth_value_prefix = strdup(ap);
    out->api_key          = key;
    if (!out->name || !out->base_url || !out->model ||
        !out->auth_header_name || !out->auth_value_prefix) {
        LOG_ERR("provider: out of memory");
        bg_provider_clear(out);
        return -1;
    }
    return 0;
}

void bg_provider_clear(bg_provider *p) {
    if (!p) return;
    free(p->name);
    free(p->base_url);
    free(p->model);
    free(p->auth_header_name);
    free(p->auth_value_prefix);
    free(p->api_key);
    memset(p, 0, sizeof(*p));
}

// ---------- public: list ----------

static bool name_present(char **arr, size_t n, const char *name) {
    for (size_t i = 0; i < n; i++)
        if (strcmp(arr[i], name) == 0) return true;
    return false;
}

int bg_provider_list_names(char ***names, size_t *count) {
    size_t cap = 16, n = 0;
    char **arr = malloc(cap * sizeof(*arr));
    if (!arr) return -1;

    for (size_t i = 0; i < N_PRESETS; i++) {
        if (n == cap) {
            cap *= 2;
            char **na = realloc(arr, cap * sizeof(*arr));
            if (!na) goto oom;
            arr = na;
        }
        arr[n] = strdup(PRESETS[i].name);
        if (!arr[n]) goto oom;
        n++;
    }

    const cJSON *cfg = config_get();
    const cJSON *provs = cfg ? cJSON_GetObjectItem(cfg, "providers") : NULL;
    if (cJSON_IsObject(provs)) {
        const cJSON *it;
        cJSON_ArrayForEach(it, provs) {
            if (!it->string || name_present(arr, n, it->string)) continue;
            if (n == cap) {
                cap *= 2;
                char **na = realloc(arr, cap * sizeof(*arr));
                if (!na) goto oom;
                arr = na;
            }
            arr[n] = strdup(it->string);
            if (!arr[n]) goto oom;
            n++;
        }
    }

    *names = arr;
    *count = n;
    return 0;
oom:
    for (size_t i = 0; i < n; i++) free(arr[i]);
    free(arr);
    return -1;
}
