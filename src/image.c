#include "image.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

bool bg_image_load_file(const char *path, bg_image *out) {
    int w, h, ch;
    uint8_t *p = stbi_load(path, &w, &h, &ch, 4);
    if (!p) {
        LOG_ERR("stbi_load(%s): %s", path, stbi_failure_reason());
        return false;
    }
    out->pixels = p;
    out->width = w;
    out->height = h;
    return true;
}

bool bg_image_load_memory(const uint8_t *data, size_t len, bg_image *out) {
    int w, h, ch;
    uint8_t *p = stbi_load_from_memory(data, (int)len, &w, &h, &ch, 4);
    if (!p) {
        LOG_ERR("stbi_load_from_memory: %s", stbi_failure_reason());
        return false;
    }
    out->pixels = p;
    out->width = w;
    out->height = h;
    return true;
}

void bg_image_free(bg_image *img) {
    if (!img || !img->pixels) return;
    stbi_image_free(img->pixels);
    img->pixels = NULL;
    img->width = img->height = 0;
}

bool bg_image_cover(const bg_image *src, int target_w, int target_h, bg_image *out) {
    if (!src || !src->pixels || target_w <= 0 || target_h <= 0) return false;

    double sx = (double)target_w / src->width;
    double sy = (double)target_h / src->height;
    double s  = sx > sy ? sx : sy;
    int scaled_w = (int)(src->width  * s + 0.5);
    int scaled_h = (int)(src->height * s + 0.5);
    if (scaled_w < target_w) scaled_w = target_w;
    if (scaled_h < target_h) scaled_h = target_h;

    uint8_t *scaled = malloc((size_t)scaled_w * scaled_h * 4);
    if (!scaled) return false;

    // stb_image_resize2: high-quality default (Mitchell/Catmull-Rom-ish).
    if (!stbir_resize_uint8_linear(src->pixels, src->width, src->height, 0,
                                   scaled, scaled_w, scaled_h, 0,
                                   STBIR_RGBA)) {
        LOG_ERR("stbir_resize_uint8_linear failed");
        free(scaled);
        return false;
    }

    uint8_t *cropped = malloc((size_t)target_w * target_h * 4);
    if (!cropped) { free(scaled); return false; }

    int off_x = (scaled_w - target_w) / 2;
    int off_y = (scaled_h - target_h) / 2;
    for (int y = 0; y < target_h; y++) {
        memcpy(cropped + (size_t)y * target_w * 4,
               scaled  + ((size_t)(off_y + y) * scaled_w + off_x) * 4,
               (size_t)target_w * 4);
    }
    free(scaled);

    out->pixels = cropped;
    out->width  = target_w;
    out->height = target_h;
    return true;
}

void bg_image_blit_xrgb8888(const bg_image *src, uint32_t *dst, int dst_w, int dst_h) {
    int w = src->width  < dst_w ? src->width  : dst_w;
    int h = src->height < dst_h ? src->height : dst_h;
    for (int y = 0; y < h; y++) {
        const uint8_t *s = src->pixels + (size_t)y * src->width * 4;
        uint32_t *d = dst + (size_t)y * dst_w;
        for (int x = 0; x < w; x++) {
            uint8_t r = s[0], g = s[1], b = s[2];
            d[x] = ((uint32_t)0xFF << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            s += 4;
        }
    }
}

void bg_image_fill_xrgb8888(uint32_t color_rrggbb, uint32_t *dst, int dst_w, int dst_h) {
    uint32_t v = 0xFF000000u | (color_rrggbb & 0x00FFFFFFu);
    size_t n = (size_t)dst_w * dst_h;
    for (size_t i = 0; i < n; i++) dst[i] = v;
}
