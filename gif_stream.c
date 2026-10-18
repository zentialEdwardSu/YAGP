#include "gif_stream.h"

#include <stdlib.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define GIF_MAX_SIDE 4096
#define GIF_MAX_PIXELS (2048u * 2048u)
#define GIF_DEFAULT_DELAY_MS 100

/* Copies from the mapped file. If the file lives on a network share or
 * removable drive that goes away, touching the view raises
 * EXCEPTION_IN_PAGE_ERROR: report a short read instead of crashing, so giflib
 * sees a damaged GIF and playback stops on the last good frame. */
static size_t copy_from_view(void *dst, const uint8_t *src, size_t n)
{
#ifdef _MSC_VER
    __try {
        memcpy(dst, src, n);
    } __except (GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER
                                                               : EXCEPTION_CONTINUE_SEARCH) {
        return 0;
    }
#else
    memcpy(dst, src, n);
#endif
    return n;
}

static int gif_input(GifFileType *decoder, GifByteType *bytes, int n)
{
    GifStream *g = (GifStream *)decoder->UserData;
    size_t left = g->size - g->pos;
    size_t count = (size_t)n < left ? (size_t)n : left;
    count = copy_from_view(bytes, g->data + g->pos, count);
    g->pos += count;
    return (int)count;
}

static void box_union(int dst[4], const int add[4])
{
    if (add[0] >= add[2] || add[1] >= add[3]) return;
    if (dst[0] >= dst[2] || dst[1] >= dst[3]) {
        memcpy(dst, add, 4 * sizeof(int));
        return;
    }
    if (add[0] < dst[0]) dst[0] = add[0];
    if (add[1] < dst[1]) dst[1] = add[1];
    if (add[2] > dst[2]) dst[2] = add[2];
    if (add[3] > dst[3]) dst[3] = add[3];
}

/* Copies the canvas box b (l, t, r, b) from src to dst; both are out_w wide. */
static void copy_box(uint8_t *dst, const uint8_t *src, int out_w, const int b[4])
{
    if (b[0] >= b[2] || b[1] >= b[3]) return;
    size_t stride = (size_t)out_w * 4, run = (size_t)(b[2] - b[0]) * 4;
    for (int y = b[1]; y < b[3]; ++y) {
        size_t at = (size_t)y * stride + (size_t)b[0] * 4;
        memcpy(dst + at, src + at, run);
    }
}

static void close_decoder(GifStream *g)
{
    if (g->decoder) {
        int error_code = 0;
        DGifCloseFile(g->decoder, &error_code);
        g->decoder = NULL;
    }
}

static int start_decoder(GifStream *g)
{
    int error_code = 0;
    g->pos = 0;
    g->decoder = DGifOpen(g, gif_input, &error_code);
    if (!g->decoder) return -1;
    if (g->decoder->SWidth <= 0 || g->decoder->SHeight <= 0 ||
        g->decoder->SWidth > GIF_MAX_SIDE || g->decoder->SHeight > GIF_MAX_SIDE ||
        (size_t)g->decoder->SWidth * (size_t)g->decoder->SHeight > GIF_MAX_PIXELS) {
        close_decoder(g);
        return -1;
    }
    return 0;
}

static int map_file(GifStream *g, const wchar_t *path)
{
    LARGE_INTEGER size;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
    if (file == INVALID_HANDLE_VALUE) return -1;
    g->file = file;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 13 ||
        size.QuadPart > (LONGLONG)256 * 1024 * 1024) return -1;
    g->mapping = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!g->mapping) return -1;
    g->data = (const uint8_t *)MapViewOfFile(g->mapping, FILE_MAP_READ, 0, 0, 0);
    if (!g->data) return -1;
    g->size = (size_t)size.QuadPart;
    return 0;
}

/* First index whose map value is >= v (maps are non-decreasing). */
static int lower_bound(const int *map, int n, int v)
{
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (map[mid] < v) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* Nearest-centre sampling: floor((i + 0.5) * src / dst), in exact integer
 * arithmetic so a centre landing exactly on a pixel edge is deterministic. */
static void build_map(int *map, int dst, int src)
{
    for (int i = 0; i < dst; ++i) {
        int s = (int)(((int64_t)(2 * i + 1) * src) / (2 * (int64_t)dst));
        map[i] = s < src ? s : src - 1;
    }
}

int gif_stream_set_output_size(GifStream *g, int out_w, int out_h)
{
    if (out_w < 1) out_w = 1;
    if (out_h < 1) out_h = 1;
    if (out_w > g->width) out_w = g->width;
    if (out_h > g->height) out_h = g->height;
    if (g->bgra && out_w == g->out_w && out_h == g->out_h) return 0;
    uint8_t *bgra = (uint8_t *)calloc((size_t)out_w * (size_t)out_h, 4);
    int *xmap = (int *)malloc((size_t)out_w * sizeof(int));
    int *ymap = (int *)malloc((size_t)out_h * sizeof(int));
    int *yinv = (int *)malloc((size_t)g->height * sizeof(int));
    if (!bgra || !xmap || !ymap || !yinv) {
        free(bgra);
        free(xmap);
        free(ymap);
        free(yinv);
        return -1;
    }
    build_map(xmap, out_w, g->width);
    build_map(ymap, out_h, g->height);
    /* Shrinking only, so every source row feeds at most one canvas row. */
    for (int y = 0; y < g->height; ++y) yinv[y] = -1;
    for (int oy = 0; oy < out_h; ++oy) yinv[ymap[oy]] = oy;
    free(g->bgra);
    free(g->backup);
    free(g->xmap);
    free(g->ymap);
    free(g->yinv);
    g->bgra = bgra;
    g->backup = NULL;
    g->xmap = xmap;
    g->ymap = ymap;
    g->yinv = yinv;
    g->out_w = out_w;
    g->out_h = out_h;
    g->old_disposal = 0;
    g->canvas_fresh = 1;
    return 0;
}

void gif_stream_map_box(const GifStream *g, int l, int t, int r, int b, int out[4])
{
    out[0] = lower_bound(g->xmap, g->out_w, l);
    out[1] = lower_bound(g->ymap, g->out_h, t);
    out[2] = lower_bound(g->xmap, g->out_w, r);
    out[3] = lower_bound(g->ymap, g->out_h, b);
}

int gif_stream_open(GifStream *g, const wchar_t *path)
{
    memset(g, 0, sizeof(*g));
    if (map_file(g, path) < 0) goto error;
    if (start_decoder(g) < 0) goto error;
    g->width = g->decoder->SWidth;
    g->height = g->decoder->SHeight;
    if (gif_stream_set_output_size(g, g->width, g->height) != 0) goto error;
    g->scanline = (uint8_t *)malloc((size_t)g->width);
    if (!g->scanline) goto error;
    g->pending_transparent = -1;
    g->delay_ms = GIF_DEFAULT_DELAY_MS;
    return 0;
error:
    gif_stream_close(g);
    return -1;
}

void gif_stream_close(GifStream *g)
{
    close_decoder(g);
    if (g->data) UnmapViewOfFile(g->data);
    if (g->mapping) CloseHandle((HANDLE)g->mapping);
    if (g->file) CloseHandle((HANDLE)g->file);
    free(g->bgra);
    free(g->backup);
    free(g->scanline);
    free(g->xmap);
    free(g->ymap);
    free(g->yinv);
    memset(g, 0, sizeof(*g));
}

int gif_stream_restart(GifStream *g)
{
    close_decoder(g);
    if (start_decoder(g) < 0) return -1;
    if (g->width != g->decoder->SWidth || g->height != g->decoder->SHeight) {
        close_decoder(g);
        return -1;
    }
    memset(g->bgra, 0, (size_t)g->out_w * (size_t)g->out_h * 4);
    g->pending_delay_cs = 0;
    g->pending_disposal = 0;
    g->pending_transparent = -1;
    g->old_disposal = 0;
    g->canvas_fresh = 1;
    return 0;
}

/* Every step below is per pixel, so working on the sampled canvas gives the
 * same pixels as working full size and downscaling afterwards. */
static void apply_old_disposal(GifStream *g)
{
    if (g->old_disposal == 3 && g->backup) {
        /* The previous frame only drew inside old_box, so restoring that box
         * restores the whole canvas. */
        copy_box(g->bgra, g->backup, g->out_w, g->old_box);
    } else if (g->old_disposal == 2 && g->old_box[0] < g->old_box[2]) {
        for (int oy = g->old_box[1]; oy < g->old_box[3]; ++oy) {
            size_t pos = ((size_t)oy * (size_t)g->out_w + (size_t)g->old_box[0]) * 4;
            memset(g->bgra + pos, 0, (size_t)(g->old_box[2] - g->old_box[0]) * 4);
        }
    }
    g->old_disposal = 0;
}

/* Key per palette entry (<=256 per frame), not per pixel: compose_line stays a table lookup. */
static void build_key_mask(GifStream *g, const ColorMapObject *palette)
{
    memset(g->key_mask, 0, sizeof(g->key_mask));
    if (!g->key_enabled) return;
    int kr = (int)(g->key_rgb >> 16) & 0xFF, kg = (int)(g->key_rgb >> 8) & 0xFF, kb = (int)g->key_rgb & 0xFF;
    int n = palette->ColorCount < 256 ? palette->ColorCount : 256;
    for (int i = 0; i < n; ++i) {
        const GifColorType *c = &palette->Colors[i];
        g->key_mask[i] = abs(c->Red - kr) <= g->key_tolerance &&
                         abs(c->Green - kg) <= g->key_tolerance &&
                         abs(c->Blue - kb) <= g->key_tolerance;
    }
}

void gif_stream_set_color_key(GifStream *g, int enabled, uint32_t rgb, int tolerance)
{
    g->key_enabled = enabled != 0;
    g->key_rgb = rgb & 0xFFFFFFu;
    g->key_tolerance = tolerance < 0 ? 0 : tolerance > 255 ? 255 : tolerance;
}

static int hex_digit(wchar_t c)
{
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

int gif_parse_hex_color(const wchar_t *text, uint32_t *rgb)
{
    while (*text == L' ' || *text == L'\t') ++text;
    if (*text == L'#') ++text;
    else if (text[0] == L'0' && (text[1] == L'x' || text[1] == L'X')) text += 2;
    int digits[6], n = 0;
    for (; *text && *text != L' ' && *text != L'\t'; ++text) {
        int d = hex_digit(*text);
        if (d < 0 || n == 6) return -1;
        digits[n++] = d;
    }
    while (*text == L' ' || *text == L'\t') ++text;
    if (*text) return -1;
    if (n == 3) { /* #RGB -> #RRGGBB */
        *rgb = (uint32_t)(digits[0] * 17) << 16 | (uint32_t)(digits[1] * 17) << 8 | (uint32_t)(digits[2] * 17);
        return 0;
    }
    if (n != 6) return -1;
    uint32_t v = 0;
    for (int i = 0; i < 6; ++i) v = v << 4 | (uint32_t)digits[i];
    *rgb = v;
    return 0;
}

/* Composites source row src_row of the current frame (scanline holds its
 * pixels from column `left`) into the canvas row that samples it, if any. */
static void compose_line(GifStream *g, const ColorMapObject *palette, int left, int src_row)
{
    int oy = g->yinv[src_row];
    if (oy < 0) return; /* Row not sampled by a shrunken canvas. */
    uint8_t *dst = g->bgra + ((size_t)oy * (size_t)g->out_w + (size_t)g->img_ox0) * 4;
    for (int ox = g->img_ox0; ox < g->img_ox1; ++ox, dst += 4) {
        int index = g->scanline[g->xmap[ox] - left];
        if (index == g->pending_transparent) continue;
        if (index >= palette->ColorCount) continue; /* Damaged input; avoid OOB. */
        if (g->key_mask[index]) {
            /* Keyed colour becomes transparent (premultiplied: all zero). */
            memset(dst, 0, 4);
            continue;
        }
        const GifColorType *c = &palette->Colors[index];
        /* Opaque pixel: premultiplied and straight BGRA are identical. */
        dst[0] = c->Blue;
        dst[1] = c->Green;
        dst[2] = c->Red;
        dst[3] = 255;
    }
}

static int read_image(GifStream *g)
{
    GifFileType *d = g->decoder;
    if (DGifGetImageDesc(d) == GIF_ERROR) return -1;
    /* giflib appends every descriptor to SavedImages; drop it so memory stays flat. */
    GifFreeSavedImages(d);
    d->ImageCount = 0;

    int x = d->Image.Left, y = d->Image.Top;
    int w = d->Image.Width, h = d->Image.Height;
    if (x < 0 || y < 0 || w <= 0 || h <= 0 ||
        x > g->width - w || y > g->height - h) return -1;
    g->frame_rect[0] = x;
    g->frame_rect[1] = y;
    g->frame_rect[2] = x + w;
    g->frame_rect[3] = y + h;
    const ColorMapObject *palette = d->Image.ColorMap ? d->Image.ColorMap : d->SColorMap;
    if (!palette || !palette->Colors) return -1;
    build_key_mask(g, palette);
    int box[4];
    gif_stream_map_box(g, x, y, x + w, y + h, box);
    g->img_ox0 = box[0];
    g->img_ox1 = box[2];

    /* What this frame can change: its own rect, plus the rect the previous
     * frame's disposal clears or restores. */
    int changed[4];
    memcpy(changed, box, sizeof(changed));
    if (g->old_disposal == 2 || (g->old_disposal == 3 && g->backup)) box_union(changed, g->old_box);
    if (g->canvas_fresh) {
        changed[0] = changed[1] = 0;
        changed[2] = g->out_w;
        changed[3] = g->out_h;
    }

    apply_old_disposal(g);
    if (g->pending_disposal == 3) {
        if (!g->backup) {
            g->backup = (uint8_t *)malloc((size_t)g->out_w * (size_t)g->out_h * 4);
            if (!g->backup) return -1;
        }
        copy_box(g->backup, g->bgra, g->out_w, box); /* Only the rect this frame draws in. */
    }
    memcpy(g->changed, changed, sizeof(changed));
    g->canvas_fresh = 0;

    if (d->Image.Interlace) {
        static const int starts[4] = {0, 4, 2, 1};
        static const int strides[4] = {8, 8, 4, 2};
        for (int pass = 0; pass < 4; ++pass) {
            for (int row = starts[pass]; row < h; row += strides[pass]) {
                if (DGifGetLine(d, g->scanline, w) == GIF_ERROR) return -1;
                compose_line(g, palette, x, y + row);
            }
        }
    } else {
        for (int row = 0; row < h; ++row) {
            if (DGifGetLine(d, g->scanline, w) == GIF_ERROR) return -1;
            compose_line(g, palette, x, y + row);
        }
    }

    g->old_disposal = g->pending_disposal;
    memcpy(g->old_box, box, sizeof(box));
    /* Browsers treat 0/1 cs as "unspecified" and use 100 ms. */
    g->delay_ms = g->pending_delay_cs > 1 ? g->pending_delay_cs * 10 : GIF_DEFAULT_DELAY_MS;
    g->pending_delay_cs = 0;
    g->pending_disposal = 0;
    g->pending_transparent = -1;
    return 1;
}

int gif_stream_seek(GifStream *g, size_t offset)
{
    if (!g->decoder && start_decoder(g) < 0) return -1;
    /* giflib reads straight from gif_input with no read-ahead, and every frame
     * is consumed up to its block terminator, so between frames its only
     * state is our read position. */
    if (offset < 13 || offset >= g->size) return -1;
    g->pos = offset;
    memset(g->bgra, 0, (size_t)g->out_w * (size_t)g->out_h * 4);
    g->pending_delay_cs = 0;
    g->pending_disposal = 0;
    g->pending_transparent = -1;
    g->old_disposal = 0;
    g->canvas_fresh = 1;
    return 0;
}

int gif_stream_snapshot_save(const GifStream *g, GifSnapshot *s)
{
    size_t bytes = (size_t)g->out_w * (size_t)g->out_h * 4;
    memset(s, 0, sizeof(*s));
    s->bgra = (uint8_t *)malloc(bytes);
    if (!s->bgra) return -1;
    memcpy(s->bgra, g->bgra, bytes);
    if (g->old_disposal == 3 && g->backup) {
        s->backup = (uint8_t *)malloc(bytes);
        if (!s->backup) {
            gif_stream_snapshot_free(s);
            return -1;
        }
        memcpy(s->backup, g->backup, bytes);
    }
    s->pos = g->pos;
    s->out_w = g->out_w;
    s->out_h = g->out_h;
    s->old_disposal = s->backup || g->old_disposal != 3 ? g->old_disposal : 0;
    memcpy(s->old_box, g->old_box, sizeof(s->old_box));
    return 0;
}

int gif_stream_snapshot_restore(GifStream *g, const GifSnapshot *s)
{
    if (!s->bgra || s->out_w != g->out_w || s->out_h != g->out_h) return -1;
    if (!g->decoder && start_decoder(g) < 0) return -1;
    size_t bytes = (size_t)g->out_w * (size_t)g->out_h * 4;
    if (s->backup) {
        if (!g->backup && !(g->backup = (uint8_t *)malloc(bytes))) return -1;
        memcpy(g->backup, s->backup, bytes);
    }
    memcpy(g->bgra, s->bgra, bytes);
    g->pos = s->pos;
    g->pending_delay_cs = 0;
    g->pending_disposal = 0;
    g->pending_transparent = -1;
    g->old_disposal = s->old_disposal;
    memcpy(g->old_box, s->old_box, sizeof(g->old_box));
    g->canvas_fresh = 1; /* The whole canvas may differ from what was shown. */
    return 0;
}

void gif_stream_snapshot_free(GifSnapshot *s)
{
    free(s->bgra);
    free(s->backup);
    memset(s, 0, sizeof(*s));
}

int gif_stream_next(GifStream *g)
{
    GifRecordType type;
    if (!g->decoder) return -1;
    size_t start = g->pos; /* The frame's first record (usually its graphics control extension). */
    for (;;) {
        if (DGifGetRecordType(g->decoder, &type) == GIF_ERROR) return -1;
        if (type == TERMINATE_RECORD_TYPE) return 0;
        if (type == IMAGE_DESC_RECORD_TYPE) {
            g->frame_offset = start;
            g->frame_disposal = g->pending_disposal;
            g->frame_transparent = g->pending_transparent >= 0;
            return read_image(g);
        }
        if (type == EXTENSION_RECORD_TYPE) {
            int extension_code = 0;
            GifByteType *block = NULL;
            if (DGifGetExtension(g->decoder, &extension_code, &block) == GIF_ERROR) return -1;
            while (block) {
                if (extension_code == GRAPHICS_EXT_FUNC_CODE && block[0] >= 4) {
                    unsigned packed = block[1];
                    g->pending_disposal = (packed >> 2) & 7;
                    g->pending_delay_cs = (int)block[2] | ((int)block[3] << 8);
                    g->pending_transparent = (packed & 1) ? (int)block[4] : -1;
                }
                if (DGifGetExtensionNext(g->decoder, &block) == GIF_ERROR) return -1;
            }
        }
    }
}
