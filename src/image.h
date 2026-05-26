#ifndef BG_IMAGE_H
#define BG_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// RGBA8 in memory: byte order R,G,B,A.
typedef struct {
    uint8_t *pixels;
    int width;
    int height;
} bg_image;

// Decode a PNG/JPEG/WebP file from disk into RGBA8.
bool bg_image_load_file(const char *path, bg_image *out);

// Decode from an in-memory buffer (e.g. directly from the OpenAI response).
bool bg_image_load_memory(const uint8_t *data, size_t len, bg_image *out);

void bg_image_free(bg_image *img);

// Cover-fit resize: scale source so it fully covers target_w x target_h,
// then center-crop. Output buffer is allocated and stored in `out`.
bool bg_image_cover(const bg_image *src, int target_w, int target_h, bg_image *out);

// Blit an RGBA8 image into a Wayland SHM buffer that expects WL_SHM_FORMAT_XRGB8888.
// Wayland XRGB8888 in little-endian memory means byte order B,G,R,X, so this swaps
// red and blue while copying.
void bg_image_blit_xrgb8888(const bg_image *src, uint32_t *dst, int dst_w, int dst_h);

// Fill a Wayland XRGB8888 buffer with a single 0xRRGGBB color.
void bg_image_fill_xrgb8888(uint32_t color_rrggbb, uint32_t *dst, int dst_w, int dst_h);

#endif
