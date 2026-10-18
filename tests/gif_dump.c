/* Decodes a GIF with gif_stream and writes every composited frame as raw BGRA,
 * followed by one extra loop to exercise gif_stream_restart.
 * Usage: gif_dump [--size W H] <in.gif> <out.bin> [#RRGGBB tolerance]
 * (--size decodes into a shrunken W x H canvas.)
 * Output: int32 width, height, frame_count, then per frame int32 delay_ms + w*h*4 bytes.
 *
 * Cache mode: gif_dump --cache <in.gif> <out.bin> <cache dir> [#RRGGBB tolerance]
 * Output: int32 width, height, frame_count, from_disk, then uint16 dirty[n][4],
 * then uint16 opaque[4], then uint32 {offset, delay_ms, flags}[n].
 *
 * Seek mode: gif_dump --seek <in.gif> <out.bin> <cache dir>
 * Same output as the plain mode, but frame i is decoded by seeking to the
 * last keyframe <= i and decoding forward from there (one pass per frame),
 * followed by one more pass that snapshots the canvas after every frame,
 * decodes on to the end, restores the snapshot and decodes the next frame. */
#include "frame_cache.h"
#include "gif_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int dump_cache(int argc, wchar_t **argv)
{
    if (argc != 5 && argc != 7) return 2;
    GifStream g;
    if (gif_stream_open(&g, argv[2]) != 0) return 3;
    int w = g.width, h = g.height;
    gif_stream_close(&g);
    uint32_t rgb = 0;
    int tol = 0, key = argc == 7;
    if (key) {
        if (gif_parse_hex_color(argv[5], &rgb) != 0) return 6;
        tol = _wtoi(argv[6]);
    }
    FrameCache c;
    if (!frame_cache_get(argv[2], key, rgb, tol, w, h, argv[4], &c)) return 5;
    FILE *out = _wfopen(argv[3], L"wb");
    if (!out) return 4;
    int header[4] = {c.width, c.height, c.frame_count, c.from_disk};
    fwrite(header, sizeof(int), 4, out);
    fwrite(c.dirty, sizeof(*c.dirty), (size_t)c.frame_count, out);
    fwrite(c.opaque, sizeof(c.opaque), 1, out);
    fwrite(c.frames, sizeof(*c.frames), (size_t)c.frame_count, out);
    fclose(out);
    frame_cache_free(&c);
    return 0;
}

static void put_frame(FILE *out, const GifStream *g)
{
    fwrite(&g->delay_ms, sizeof(int), 1, out);
    fwrite(g->bgra, 4, (size_t)g->out_w * (size_t)g->out_h, out);
}

static int dump_seek(int argc, wchar_t **argv)
{
    if (argc != 5) return 2;
    GifStream g;
    if (gif_stream_open(&g, argv[2]) != 0) return 3;
    FrameCache c;
    if (!frame_cache_get(argv[2], false, 0, 0, g.width, g.height, argv[4], &c)) return 5;
    FILE *out = _wfopen(argv[3], L"wb");
    if (!out) return 4;
    int n = c.frame_count, header[3] = {g.out_w, g.out_h, 2 * n};
    fwrite(header, sizeof(int), 3, out);
    int rc = 0;
    for (int i = 0; i < n && !rc; ++i) {
        int k = i;
        while (!(c.frames[k].flags & FRAME_KEYFRAME)) --k;
        if (gif_stream_seek(&g, c.frames[k].offset) != 0) rc = 8;
        for (int j = k; j <= i && !rc; ++j) {
            if (gif_stream_next(&g) != 1) rc = 9;
        }
        if (!rc) put_frame(out, &g);
    }
    for (int i = 0; i < n && !rc; ++i) {
        /* Frame i follows a restored snapshot of frame i - 1 (or a restart). */
        GifSnapshot snap;
        memset(&snap, 0, sizeof(snap));
        if (gif_stream_restart(&g) != 0) rc = 10;
        for (int j = 0; j < i && !rc; ++j) {
            if (gif_stream_next(&g) != 1) rc = 9;
        }
        if (!rc && i > 0) {
            if (gif_stream_snapshot_save(&g, &snap) != 0) rc = 11;
            while (!rc && gif_stream_next(&g) == 1) {} /* Wander off, then come back. */
            if (!rc && gif_stream_snapshot_restore(&g, &snap) != 0) rc = 12;
        }
        if (!rc && gif_stream_next(&g) != 1) rc = 9;
        if (!rc) put_frame(out, &g);
        gif_stream_snapshot_free(&snap);
    }
    fclose(out);
    frame_cache_free(&c);
    gif_stream_close(&g);
    return rc;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc >= 2 && wcscmp(argv[1], L"--cache") == 0) return dump_cache(argc, argv);
    if (argc >= 2 && wcscmp(argv[1], L"--seek") == 0) return dump_seek(argc, argv);
    /* --size W H: decode into a shrunken canvas (gif_stream_set_output_size). */
    int out_w = 0, out_h = 0;
    if (argc >= 4 && wcscmp(argv[1], L"--size") == 0) {
        out_w = _wtoi(argv[2]);
        out_h = _wtoi(argv[3]);
        argv += 3;
        argc -= 3;
    }
    if (argc != 3 && argc != 5) return 2;
    GifStream g;
    if (gif_stream_open(&g, argv[1]) != 0) return 3;
    if (out_w > 0 && gif_stream_set_output_size(&g, out_w, out_h) != 0) return 7;
    if (argc == 5) {
        uint32_t rgb;
        if (gif_parse_hex_color(argv[3], &rgb) != 0) return 6;
        gif_stream_set_color_key(&g, 1, rgb, _wtoi(argv[4]));
    }
    FILE *out = _wfopen(argv[2], L"wb");
    if (!out) return 4;
    int header[3] = {g.out_w, g.out_h, 0};
    fwrite(header, sizeof(int), 3, out);
    int frames = 0, status = 0;
    for (int loop = 0; loop < 2; ++loop) {
        while ((status = gif_stream_next(&g)) == 1) {
            fwrite(&g.delay_ms, sizeof(int), 1, out);
            fwrite(g.bgra, 4, (size_t)g.out_w * (size_t)g.out_h, out);
            ++frames;
        }
        if (status < 0) break;
        if (loop == 0 && gif_stream_restart(&g) != 0) { status = -1; break; }
    }
    header[2] = frames;
    fseek(out, 0, SEEK_SET);
    fwrite(header, sizeof(int), 3, out);
    fclose(out);
    gif_stream_close(&g);
    return status < 0 ? 5 : 0;
}
