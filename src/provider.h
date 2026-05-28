#ifndef BG_PROVIDER_H
#define BG_PROVIDER_H

#include "imagegen.h"

#include <stddef.h>

// A resolved provider: a built-in preset overlaid with config.json overrides,
// with the API key resolved. All strings are owned by the struct.
typedef struct {
    char     *name;
    bg_scheme scheme;
    char     *base_url;
    char     *model;
    char     *auth_header_name;
    char     *auth_value_prefix;
    char     *api_key;   // NULL if no key could be resolved (not an error here)
} bg_provider;

// Resolve a provider by request name (config providers.<name> → built-in preset
// → fully-custom), apply optional overrides (any may be NULL), and resolve the
// key. When req_name is NULL the default is config.default_provider →
// $VIBEPAPER_PROVIDER → "openai". override_scheme is "openai"|"gemini"|
// "stability". Succeeds even when no key resolves (api_key left NULL) so the
// caller can report a clear, provider-specific error. Returns 0/-1 (logs).
// Fills *out on success; free with bg_provider_clear.
int  bg_provider_resolve(const char *req_name,
                         const char *override_base_url,
                         const char *override_scheme,
                         const char *override_model,
                         bg_provider *out);

void bg_provider_clear(bg_provider *p);

// Names of all known providers (built-in presets ∪ config providers, deduped),
// for the `providers` subcommand. Caller frees the array and each string.
// Returns 0/-1.
int  bg_provider_list_names(char ***names, size_t *count);

// scheme <-> string. bg_scheme_from_string returns 0/-1.
int         bg_scheme_from_string(const char *s, bg_scheme *out);
const char *bg_scheme_to_string(bg_scheme s);

#endif
