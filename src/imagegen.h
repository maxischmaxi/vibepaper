#ifndef BG_IMAGEGEN_H
#define BG_IMAGEGEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Default provider/model/size/quality. These preserve the historical OpenAI
// behaviour so an existing user (only OPENAI_API_KEY / api_key set) sees no
// change after the multi-provider refactor.
#define BG_DEFAULT_PROVIDER "openai"
#define BG_DEFAULT_MODEL    "gpt-image-2"
#define BG_DEFAULT_SIZE     "1536x1024"
#define BG_DEFAULT_QUALITY  "medium"

// Wire "scheme": how a request is built and the response parsed. Providers that
// share a scheme differ only by base_url/model/auth (data-driven), so adding an
// OpenAI-compatible provider needs no new code — only a preset or config entry.
typedef enum {
    BG_SCHEME_OPENAI = 0,   // {base}/images/generations  (+ /images/edits)
    BG_SCHEME_GEMINI,       // {base}/models/{model}:generateContent
    BG_SCHEME_STABILITY,    // {base}/v2beta/stable-image/generate/core
} bg_scheme;

// Fully-resolved request options. All string pointers are BORROWED — the caller
// keeps them alive for the duration of the bg_imagegen() call.
typedef struct {
    bg_scheme   scheme;
    const char *provider;            // label, e.g. "openai" (for logs)
    const char *base_url;            // no trailing slash
    const char *model;               // scheme-specific model id
    const char *size;                // pass-through per scheme; NULL → omit
    const char *quality;             // pass-through; NULL → omit
    const char *api_key;             // resolved key; must be non-NULL at call
    const char *auth_header_name;    // e.g. "Authorization" / "x-goog-api-key"
    const char *auth_value_prefix;   // e.g. "Bearer " / ""
} bg_gen_opts;

// Decoded image bytes plus the file extension implied by the response
// content-type ("png"/"jpeg"/"webp"); "png" when unknown. `ext` points at a
// string literal, so it must not be freed.
typedef struct {
    uint8_t    *data;
    size_t      len;
    const char *ext;
} bg_gen_result;

// Initialise / tear down the HTTP library. Call once on the main thread before
// any concurrent generation, and once at shutdown.
void bg_imagegen_global_init(void);
void bg_imagegen_global_cleanup(void);

// Synchronous generate (src_path == NULL) or refine/edit (src_path != NULL).
// Returns true on success (out filled; free with bg_imagegen_free_result),
// false on error (and logs). Safe on a worker thread after global_init.
bool bg_imagegen(const bg_gen_opts *opts, const char *prompt,
                 const char *src_path, bg_gen_result *out);

void bg_imagegen_free_result(bg_gen_result *res);

#endif
