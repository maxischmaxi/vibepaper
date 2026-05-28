#ifndef BG_OPENAI_H
#define BG_OPENAI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BG_OPENAI_DEFAULT_MODEL   "gpt-image-2"
#define BG_OPENAI_DEFAULT_SIZE    "1536x1024"
#define BG_OPENAI_DEFAULT_QUALITY "medium"

typedef struct {
    const char *model;     // e.g. "gpt-image-2"; NULL → default
    const char *size;      // e.g. "1536x1024", "3840x2160", "auto"; NULL → "auto"
    const char *quality;   // "low" | "medium" | "high" | "auto"; NULL → "auto"
    const char *api_key;   // NULL → read OPENAI_API_KEY env
} bg_openai_opts;

// Result holds the decoded image bytes (PNG/JPEG/WebP as returned by the API).
// Caller must free with bg_openai_free_result.
typedef struct {
    uint8_t *data;
    size_t   len;
} bg_openai_result;

// Initialise / tear down the underlying HTTP library. Call once from the main
// thread before any concurrent generation, and once at shutdown.
void bg_openai_global_init(void);
void bg_openai_global_cleanup(void);

// Synchronous generation. Returns true on success, false on error (and logs).
// Safe to call from a worker thread once bg_openai_global_init has run.
bool bg_openai_generate(const char *prompt, const bg_openai_opts *opts,
                        bg_openai_result *out);

// Synchronous edit ("refine"): sends the image at `image_path` together with a
// prompt to the edits endpoint and returns a new image. `opts` and `out` work
// exactly as for bg_openai_generate; if opts->size is NULL the caller's intent
// is the API default. Returns true on success, false on error (and logs).
// Safe to call from a worker thread once bg_openai_global_init has run.
bool bg_openai_edit(const char *image_path, const char *prompt,
                    const bg_openai_opts *opts, bg_openai_result *out);

void bg_openai_free_result(bg_openai_result *res);

#endif
