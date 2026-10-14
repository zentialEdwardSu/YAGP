#ifndef YAGP_GIF_STREAM_H
#define YAGP_GIF_STREAM_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>
#include <gif_lib.h>

typedef struct GifStream {
    void *file;            /* HANDLE */
    void *mapping;         /* HANDLE */
    const uint8_t *data;   /* Read-only view of the whole file (shared page cache). */
    size_t size, pos;
    GifFileType *decoder;
    int width, height;     /* Logical screen size of the GIF. */
    /* Canvas size: width x height, or smaller (gif_stream_set_output_size). */
    int out_w, out_h;
    uint8_t *bgra;         /* out_w x out_h premultiplied BGRA; GIF alpha is only 0 or 255. */
    uint8_t *backup;       /* Disposal 3: canvas-sized, only the frame's rect is used. Lazy. */
    uint8_t *scanline;
    int *xmap;             /* out_w: source column sampled by each canvas column. */
    int *ymap;             /* out_h: source row sampled by each canvas row. */
    int *yinv;             /* height: canvas row fed by each source row, or -1. */
    int img_ox0, img_ox1;  /* Canvas columns covered by the frame being composited. */
    int pending_delay_cs;
    int pending_disposal;
    int pending_transparent;
    int old_disposal;
    int old_box[4];        /* Previous frame's rect in canvas coordinates. */
    /* Canvas box {l, t, r, b} that the last gif_stream_next may have changed
     * (the frame's rect plus the previous frame's disposal); every pixel
     * outside it is unchanged. The whole canvas after open/restart/resize. */
    int changed[4];
    int canvas_fresh;      /* Canvas was (re)created or cleared since the last frame. */
    int delay_ms;
    /* Colour key: palette entries within key_tolerance (per channel) of
     * key_rgb (0xRRGGBB) are composited as fully transparent. */
    int key_enabled;
    uint32_t key_rgb;
    int key_tolerance;
    uint8_t key_mask[256]; /* Per-frame: 1 = palette index is keyed out. */
} GifStream;

/* Map the file and allocate a full-size compositing canvas. 0 on success. */
int gif_stream_open(GifStream *g, const wchar_t *path);
/* Return 1 (frame), 0 (end of animation), -1 (invalid GIF). */
int gif_stream_next(GifStream *g);
/* Rewind to the first frame without reopening the file. */
int gif_stream_restart(GifStream *g);
void gif_stream_close(GifStream *g);
/* Applies from the next decoded frame; call gif_stream_restart to re-key the canvas. */
void gif_stream_set_color_key(GifStream *g, int enabled, uint32_t rgb, int tolerance);
/* Shrinks the canvas to out_w x out_h (clamped to 1..width/height). Each canvas
 * pixel is the source pixel nearest its centre, so the result is exactly a
 * nearest-neighbour downscale of the full-size canvas, at a fraction of the
 * memory. Call gif_stream_restart afterwards. 0 on success; on failure the
 * stream keeps its current canvas. */
int gif_stream_set_output_size(GifStream *g, int out_w, int out_h);
/* Maps a source-pixel box [l, r) x [t, b) to the canvas box {l, t, r, b} of the
 * pixels that sample it (empty when l >= r or t >= b). */
void gif_stream_map_box(const GifStream *g, int l, int t, int r, int b, int out[4]);
/* Parses "#RRGGBB", "RRGGBB" or "#RGB". Returns 0 on success. */
int gif_parse_hex_color(const wchar_t *text, uint32_t *rgb);

#endif
