#include "still_image.h"

#include <stdlib.h>
#include <string.h>

#include "stb_image.h"

#define STILL_MAX_SIDE 16384
#define STILL_MAX_PIXELS (64u * 1024u * 1024u)

static bool keyed(const StillImage *s, const uint8_t *px)
{
    if (!s->key_enabled) return false;
    int kr = (int)(s->key_rgb >> 16) & 0xFF;
    int kg = (int)(s->key_rgb >> 8) & 0xFF;
    int kb = (int)s->key_rgb & 0xFF;
    return abs((int)px[0] - kr) <= s->key_tolerance &&
           abs((int)px[1] - kg) <= s->key_tolerance &&
           abs((int)px[2] - kb) <= s->key_tolerance;
}

/* Nearest-centre sample, the same rule gif_stream_set_output_size uses. */
static int sample_index(int i, int dst_count, int src)
{
    int v = (int)(((int64_t)(2 * i + 1) * src) / (2 * (int64_t)dst_count));
    return v < src ? v : src - 1;
}

static void compute_opaque(StillImage *s)
{
    s->opaque[0] = s->opaque[1] = s->opaque[2] = s->opaque[3] = 0;
    for (int y = 0; y < s->height; ++y) {
        const uint8_t *row = s->src_rgba + (size_t)y * (size_t)s->width * 4;
        for (int x = 0; x < s->width; ++x) {
            const uint8_t *px = row + (size_t)x * 4;
            if (px[3] == 0 || keyed(s, px)) continue;
            if (s->opaque[0] >= s->opaque[2]) {
                s->opaque[0] = x;
                s->opaque[1] = y;
                s->opaque[2] = x + 1;
                s->opaque[3] = y + 1;
            } else {
                if (x < s->opaque[0]) s->opaque[0] = x;
                if (y < s->opaque[1]) s->opaque[1] = y;
                if (x + 1 > s->opaque[2]) s->opaque[2] = x + 1;
                if (y + 1 > s->opaque[3]) s->opaque[3] = y + 1;
            }
        }
    }
}

int still_image_set_output_size(StillImage *s, int out_w, int out_h)
{
    if (!s->src_rgba) return -1;
    if (out_w < 1) out_w = 1;
    if (out_h < 1) out_h = 1;
    if (out_w > s->width) out_w = s->width;
    if (out_h > s->height) out_h = s->height;
    if (s->bgra && out_w == s->out_w && out_h == s->out_h) return 0;

    uint8_t *dst = (uint8_t *)malloc((size_t)out_w * (size_t)out_h * 4);
    if (!dst) return -1;
    for (int y = 0; y < out_h; ++y) {
        int sy = sample_index(y, out_h, s->height);
        const uint8_t *row = s->src_rgba + (size_t)sy * (size_t)s->width * 4;
        uint8_t *drow = dst + (size_t)y * (size_t)out_w * 4;
        for (int x = 0; x < out_w; ++x) {
            const uint8_t *px = row + (size_t)sample_index(x, out_w, s->width) * 4;
            uint8_t *d = drow + (size_t)x * 4;
            if (px[3] == 0 || keyed(s, px)) {
                d[0] = d[1] = d[2] = d[3] = 0;
            } else {
                unsigned a = px[3];
                d[0] = (uint8_t)((unsigned)px[2] * a / 255u); /* BGRA. */
                d[1] = (uint8_t)((unsigned)px[1] * a / 255u);
                d[2] = (uint8_t)((unsigned)px[0] * a / 255u);
                d[3] = (uint8_t)a;
            }
        }
    }
    free(s->bgra);
    s->bgra = dst;
    s->out_w = out_w;
    s->out_h = out_h;
    return 0;
}

void still_image_set_color_key(StillImage *s, bool enabled, uint32_t rgb, int tolerance)
{
    s->key_enabled = enabled;
    s->key_rgb = rgb & 0xFFFFFFu;
    s->key_tolerance = tolerance < 0 ? 0 : tolerance > 255 ? 255 : tolerance;
    if (!s->src_rgba) return;
    compute_opaque(s);
    if (s->bgra) {
        int w = s->out_w, h = s->out_h;
        s->out_w = s->out_h = 0; /* Force a rebuild, including when dimensions are unchanged. */
        if (still_image_set_output_size(s, w, h) != 0) {
            free(s->bgra);
            s->bgra = NULL;
            s->out_w = s->out_h = 0;
        }
    }
}

int still_image_open_memory(StillImage *s, const uint8_t *data, size_t len)
{
    memset(s, 0, sizeof(*s));
    int w = 0, h = 0, comp = 0;
    if (!data || len == 0 || len > (size_t)512 * 1024 * 1024) return -1;
    if (!stbi_info_from_memory(data, (int)len, &w, &h, &comp)) return -1;
    if (w < 1 || h < 1 || w > STILL_MAX_SIDE || h > STILL_MAX_SIDE ||
        (size_t)w * (size_t)h > STILL_MAX_PIXELS) return -1;
    int src_comp = 0;
    stbi_uc *rgba = stbi_load_from_memory(data, (int)len, &w, &h, &src_comp, 4);
    if (!rgba) return -1;
    s->width = w;
    s->height = h;
    s->src_rgba = rgba;
    s->key_tolerance = 0;
    compute_opaque(s);
    return 0;
}

void still_image_close(StillImage *s)
{
    free(s->src_rgba);
    free(s->bgra);
    memset(s, 0, sizeof(*s));
}

size_t still_image_pixels(const StillImage *s)
{
    return s ? (size_t)s->width * (size_t)s->height : 0;
}
