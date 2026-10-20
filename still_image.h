#ifndef YAGP_STILL_IMAGE_H
#define YAGP_STILL_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One PNG/JPEG/JPEG image: the source is kept so re-keying/resizing can
 * rebuild the premultiplied BGRA canvas without decoding the file again. */
typedef struct StillImage {
    int width, height;      /* Source dimensions. */
    int out_w, out_h;       /* Current canvas dimensions. */
    uint8_t *src_rgba;      /* width * height RGBA, source alpha. */
    uint8_t *bgra;          /* out_w * out_h premultiplied BGRA. */
    int opaque[4];          /* Source pixel box {l, t, r, b}; empty if fully transparent. */
    bool key_enabled;
    uint32_t key_rgb;
    int key_tolerance;
} StillImage;

/* Decodes PNG/JPEG from memory. 0 on success. */
int still_image_open_memory(StillImage *s, const uint8_t *data, size_t len);
void still_image_close(StillImage *s);

/* Applies a colour key and rebuilds opaque[]/bgra. */
void still_image_set_color_key(StillImage *s, bool enabled, uint32_t rgb, int tolerance);
/* Rebuilds bgra at out_w x out_h (clamped to the source). 0 on success. */
int still_image_set_output_size(StillImage *s, int out_w, int out_h);
/* Total source pixels; callers use this for budgeting. */
size_t still_image_pixels(const StillImage *s);

#endif
