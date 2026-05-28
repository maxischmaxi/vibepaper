#ifndef BG_STORE_H
#define BG_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BG_ID_MAX   32
#define BG_PATH_MAX 4096

typedef struct {
    char  id[BG_ID_MAX];
    char *prompt;        // malloc'd, may be NULL
    char *model;
    char *size;
    char *quality;
    char *provider;      // backend provider name (NULL for pre-multi-provider entries)
    char *base_url;      // resolved endpoint base (NULL → provider/scheme default)
    char *scheme;        // wire scheme ("openai"/"gemini"/"stability"); NULL → openai
    char  path[BG_PATH_MAX]; // absolute path to the image file
    long  ts;            // unix timestamp
} bg_store_entry;

// Ensure the cache directory exists. Returns 0 / -1.
int bg_store_init(void);

// Absolute path to the cache directory (valid after bg_store_init).
const char *bg_store_dir(void);

// Persist image bytes + metadata. `ext` is the file extension without dot
// ("png", "jpg", "webp"). A timestamp-based id is written into out_id.
// Returns 0 / -1.
int bg_store_add(const uint8_t *data, size_t len, const char *ext,
                 const char *prompt, const char *model,
                 const char *size, const char *quality,
                 const char *provider, const char *base_url, const char *scheme,
                 char *out_id, size_t out_id_len);

// Look up a single entry by exact id. Returns 0 / -1.
int bg_store_get(const char *id, bg_store_entry *out);

// List entries newest-first. Caller frees with bg_store_free_list.
int  bg_store_list(bg_store_entry **out, size_t *count);
void bg_store_entry_clear(bg_store_entry *e);
void bg_store_free_list(bg_store_entry *list, size_t count);

// Record / read the id of the currently displayed wallpaper.
int bg_store_set_current(const char *id);
int bg_store_get_current(char *out_id, size_t out_id_len);

// Delete all but the newest `keep` entries (image + metadata). The currently
// displayed wallpaper is always protected. `keep == 0` removes everything
// except current. Writes the number of deleted entries to *deleted (if set).
int bg_store_prune(size_t keep, size_t *deleted);

#endif
