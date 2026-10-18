#include "frame_cache.h"
#include "gif_stream.h"

#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CACHE_MAGIC 0x31434759u /* "YGC1" little-endian */
#define CACHE_VERSION 4 /* v2: mask removed; v3: opaque box; v4: frame index */
#define MAX_CACHE_FRAMES 65535

/* On-disk layout (little-endian):
 *   CacheHeader
 *   uint16_t dirty[frame_count][4]
 *   FrameInfo frames[frame_count]
 * Every field that can change the result is part of the header and is
 * compared on load, so a stale file is simply rebuilt. */
#pragma pack(push, 1)
typedef struct CacheHeader {
    uint32_t magic;
    uint32_t version;
    uint64_t file_size;
    uint64_t file_mtime;    /* FILETIME of last write */
    uint32_t key_enabled;
    uint32_t key_rgb;
    int32_t key_tolerance;
    int32_t width, height;
    int32_t frame_count;
    uint16_t opaque[4];     /* Result, not a key: not compared on load. */
} CacheHeader;
#pragma pack(pop)

void frame_cache_free(FrameCache *c)
{
    free(c->dirty);
    free(c->frames);
    memset(c, 0, sizeof(*c));
}

static bool local_dir(wchar_t *out, size_t cap, const wchar_t *suffix)
{
    PWSTR base = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &base))) return false;
    int n = _snwprintf_s(out, cap, _TRUNCATE, L"%s\\YAGP%s", base, suffix);
    CoTaskMemFree(base);
    return n > 0;
}

bool frame_cache_default_dir(wchar_t *out, size_t cap)
{
    return local_dir(out, cap, L"\\cache");
}

bool frame_cache_delete_all(void)
{
    wchar_t dir[MAX_PATH + 1];
    if (!local_dir(dir, MAX_PATH, L"")) return false;
    if (GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES) return true;
    dir[wcslen(dir) + 1] = 0; /* SHFileOperation wants a double-NUL terminated list. */
    SHFILEOPSTRUCTW op;
    memset(&op, 0, sizeof(op));
    op.wFunc = FO_DELETE;
    op.pFrom = dir;
    op.fFlags = FOF_NO_UI;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

/* FNV-1a over the case-folded full path: a stable, short file name. */
static uint64_t path_hash(const wchar_t *path)
{
    uint64_t h = 1469598103934665603ull;
    for (; *path; ++path) {
        wchar_t c = *path;
        if (c == L'/') c = L'\\';
        c = (wchar_t)(uintptr_t)CharLowerW((LPWSTR)(uintptr_t)c);
        h = (h ^ (uint8_t)c) * 1099511628211ull;
        h = (h ^ (uint8_t)(c >> 8)) * 1099511628211ull;
    }
    return h;
}

/* One file per GIF path and colour key, so switching a key on and off (or
 * back to an earlier one) reloads instead of rebuilding. The header still
 * holds and checks the key; the name only has to be stable. */
static bool cache_file(const wchar_t *dir, const wchar_t *path, const CacheHeader *h, wchar_t *out, size_t cap)
{
    uint64_t hash = path_hash(path);
    const uint32_t key[3] = {h->key_enabled, h->key_rgb, (uint32_t)h->key_tolerance};
    const uint8_t *k = (const uint8_t *)key;
    for (size_t i = 0; i < sizeof(key); ++i) hash = (hash ^ k[i]) * 1099511628211ull;
    return _snwprintf_s(out, cap, _TRUNCATE, L"%s\\%016llx.ygc", dir, (unsigned long long)hash) > 0;
}

static bool file_identity(const wchar_t *path, uint64_t *size, uint64_t *mtime)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a)) return false;
    *size = (uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow;
    *mtime = (uint64_t)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
    return true;
}

static bool read_exact(HANDLE f, void *buf, size_t n)
{
    DWORD got = 0;
    return ReadFile(f, buf, (DWORD)n, &got, NULL) && got == (DWORD)n;
}

static bool write_exact(HANDLE f, const void *buf, size_t n)
{
    DWORD wrote = 0;
    return WriteFile(f, buf, (DWORD)n, &wrote, NULL) && wrote == (DWORD)n;
}

static bool load(const wchar_t *file, const CacheHeader *want, FrameCache *out)
{
    HANDLE f = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    CacheHeader h;
    bool ok = read_exact(f, &h, sizeof(h)) &&
              h.magic == want->magic && h.version == want->version &&
              h.file_size == want->file_size && h.file_mtime == want->file_mtime &&
              h.key_enabled == want->key_enabled && h.key_rgb == want->key_rgb &&
              h.key_tolerance == want->key_tolerance &&
              h.width == want->width && h.height == want->height &&
              h.frame_count > 0 && h.frame_count <= MAX_CACHE_FRAMES;
    if (ok) {
        size_t dsize = (size_t)h.frame_count * sizeof(*out->dirty);
        out->dirty = (uint16_t (*)[4])malloc(dsize);
        size_t fsize = (size_t)h.frame_count * sizeof(*out->frames);
        out->frames = (FrameInfo *)malloc(fsize);
        ok = out->dirty && out->frames && read_exact(f, out->dirty, dsize) && read_exact(f, out->frames, fsize);
        /* Reject trailing junk / truncated writes. */
        LARGE_INTEGER size, pos = {0};
        ok = ok && GetFileSizeEx(f, &size) && SetFilePointerEx(f, pos, &pos, FILE_CURRENT) &&
             size.QuadPart == pos.QuadPart;
        for (int i = 0; ok && i < h.frame_count; ++i) {
            const uint16_t *d = out->dirty[i];
            ok = d[2] <= h.width && d[3] <= h.height && (d[0] >= d[2] || (d[0] < d[2] && d[1] < d[3]));
        }
        /* Offsets must point into the file, in order: gif_stream_seek trusts them. */
        for (int i = 0; ok && i < h.frame_count; ++i) {
            const FrameInfo *fi = &out->frames[i];
            ok = fi->offset >= 13 && fi->offset < h.file_size &&
                 (i == 0 ? (fi->flags & FRAME_KEYFRAME) != 0 : fi->offset > out->frames[i - 1].offset);
        }
        const uint16_t *o = h.opaque;
        ok = ok && (o[0] >= o[2] || (o[2] <= h.width && o[3] <= h.height && o[1] < o[3]));
        if (ok) memcpy(out->opaque, h.opaque, sizeof(out->opaque));
    }
    CloseHandle(f);
    if (!ok) {
        frame_cache_free(out);
        return false;
    }
    out->width = h.width;
    out->height = h.height;
    out->frame_count = h.frame_count;
    out->from_disk = true;
    return true;
}

static void save(const wchar_t *dir, const wchar_t *file, const CacheHeader *h, const FrameCache *c)
{
    wchar_t tmp[MAX_PATH * 2];
    /* Process and thread in the name: builds may run on a worker thread. */
    if (_snwprintf_s(tmp, MAX_PATH * 2, _TRUNCATE, L"%s.%lu.%lu.tmp", file, GetCurrentProcessId(),
                     GetCurrentThreadId()) <= 0) return;
    SHCreateDirectoryExW(NULL, dir, NULL); /* Creates intermediate folders too. */
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    bool ok = write_exact(f, h, sizeof(*h)) &&
              write_exact(f, c->dirty, (size_t)c->frame_count * sizeof(*c->dirty)) &&
              write_exact(f, c->frames, (size_t)c->frame_count * sizeof(*c->frames));
    CloseHandle(f);
    /* A cache is disposable: no flush, just an atomic rename. */
    if (!ok || !MoveFileExW(tmp, file, MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp);
}

/* Region {l, t, r, b} of a w-wide canvas; scans below only look inside it,
 * because gif_stream reports which pixels a frame can have changed. */
typedef struct Region { int l, t, r, b; } Region;

/* Bounding box of pixels inside rg that differ between two premultiplied
 * BGRA canvases (pixels outside rg must be equal). */
static void diff_box(const uint8_t *a, const uint8_t *b, int w, Region rg, uint16_t box[4])
{
    int l = rg.r, t = rg.b, r = rg.l, btm = rg.t;
    size_t stride = (size_t)w * 4, run = (size_t)(rg.r - rg.l) * 4;
    for (int y = rg.t; y < rg.b; ++y) {
        const uint32_t *ra = (const uint32_t *)(a + (size_t)y * stride);
        const uint32_t *rb = (const uint32_t *)(b + (size_t)y * stride);
        if (!memcmp(ra + rg.l, rb + rg.l, run)) continue;
        int x0 = rg.l, x1 = rg.r;
        while (ra[x0] == rb[x0]) ++x0;          /* Row differs, so both stop in range. */
        while (ra[x1 - 1] == rb[x1 - 1]) --x1;
        if (x0 < l) l = x0;
        if (x1 > r) r = x1;
        if (y < t) t = y;
        btm = y + 1;
    }
    if (r <= l) { box[0] = box[1] = box[2] = box[3] = 0; return; }
    box[0] = (uint16_t)l;
    box[1] = (uint16_t)t;
    box[2] = (uint16_t)r;
    box[3] = (uint16_t)btm;
}

/* Grows [*l, *r) x [*t, *b) to cover every pixel in rg with alpha != 0. */
static void grow_opaque(const uint8_t *bgra, int w, Region rg, int *l, int *t, int *r, int *b)
{
    for (int y = rg.t; y < rg.b; ++y) {
        const uint8_t *row = bgra + (size_t)y * (size_t)w * 4;
        int x0 = rg.l;
        while (x0 < rg.r && !row[x0 * 4 + 3]) ++x0;
        if (x0 == rg.r) continue;
        int x1 = rg.r;
        while (!row[(x1 - 1) * 4 + 3]) --x1; /* Stops at x0 at the latest. */
        if (x0 < *l) *l = x0;
        if (x1 > *r) *r = x1;
        if (y < *t) *t = y;
        if (y + 1 > *b) *b = y + 1;
    }
}

static void copy_region(uint8_t *dst, const uint8_t *src, int w, Region rg)
{
    size_t stride = (size_t)w * 4, run = (size_t)(rg.r - rg.l) * 4;
    for (int y = rg.t; y < rg.b; ++y) {
        size_t at = (size_t)y * stride + (size_t)rg.l * 4;
        memcpy(dst + at, src + at, run);
    }
}

/* One full decode pass with a private stream, so the caller's playback
 * state is untouched. */
static bool build(const wchar_t *path, bool key_enabled, uint32_t key_rgb, int key_tolerance,
                  FrameCache *out)
{
    GifStream g;
    if (gif_stream_open(&g, path) != 0) return false;
    gif_stream_set_color_key(&g, key_enabled, key_rgb, key_tolerance);
    size_t canvas = (size_t)g.width * (size_t)g.height * 4;
    uint8_t *prev = (uint8_t *)malloc(canvas);
    uint8_t *first = (uint8_t *)malloc(canvas);
    int cap = 64;
    out->dirty = (uint16_t (*)[4])malloc((size_t)cap * sizeof(*out->dirty));
    out->frames = (FrameInfo *)malloc((size_t)cap * sizeof(*out->frames));
    bool ok = prev && first && out->dirty && out->frames;
    int n = 0;
    bool cleared = true; /* The canvas is blank before the next frame. */
    int ol = g.width, ot = g.height, orr = 0, ob = 0;
    const Region full = {0, 0, g.width, g.height};
    while (ok) {
        int status = gif_stream_next(&g);
        /* Matches playback: an error after at least one frame ends the loop. */
        if (status == 0 || (status < 0 && n > 0)) break;
        if (status < 0 || n == MAX_CACHE_FRAMES) { ok = false; break; }
        if (n == cap) {
            cap *= 2;
            uint16_t (*grown)[4] = (uint16_t (*)[4])realloc(out->dirty, (size_t)cap * sizeof(*out->dirty));
            if (grown) out->dirty = grown;
            FrameInfo *fgrown = grown ? (FrameInfo *)realloc(out->frames, (size_t)cap * sizeof(*out->frames)) : NULL;
            if (!fgrown) { ok = false; break; }
            out->frames = fgrown;
        }
        /* Keyframe: decoding can start here on a blank canvas and give the same
         * pixels (and the same state for the frames after it). True when the
         * canvas was blank anyway, or when this frame overwrites every pixel
         * and does not restore (disposal 3 would bring the unknown old canvas
         * back). */
        const int *fr = g.frame_rect;
        bool whole = fr[0] == 0 && fr[1] == 0 && fr[2] == g.width && fr[3] == g.height;
        FrameInfo *fi = &out->frames[n];
        fi->offset = (uint32_t)g.frame_offset;
        fi->delay_ms = (uint32_t)g.delay_ms;
        fi->flags = cleared || (whole && !g.frame_transparent && g.frame_disposal != 3) ? FRAME_KEYFRAME : 0;
        cleared = whole && g.frame_disposal == 2;
        if (n == 0) {
            grow_opaque(g.bgra, g.width, full, &ol, &ot, &orr, &ob);
            memcpy(first, g.bgra, canvas);
            memcpy(prev, g.bgra, canvas);
        } else {
            /* Outside g.changed this canvas equals prev (the previous frame),
             * so diffing, the opaque scan and the copy only need that box:
             * a small sprite on a big canvas costs its own size per frame. */
            Region rg = {g.changed[0], g.changed[1], g.changed[2], g.changed[3]};
            if (rg.l < rg.r && rg.t < rg.b) {
                diff_box(prev, g.bgra, g.width, rg, out->dirty[n]);
                grow_opaque(g.bgra, g.width, rg, &ol, &ot, &orr, &ob);
                copy_region(prev, g.bgra, g.width, rg);
            } else {
                memset(out->dirty[n], 0, sizeof(out->dirty[n]));
            }
        }
        ++n;
    }
    if (ok && n == 0) ok = false;
    /* Wrap: playback restarts from a cleared canvas, so going from the last
     * frame to frame 0 changes exactly prev -> first. */
    if (ok) diff_box(prev, first, g.width, full, out->dirty[0]);
    if (ok) {
        out->width = g.width;
        out->height = g.height;
        out->frame_count = n;
        out->from_disk = false;
        if (orr > ol) {
            out->opaque[0] = (uint16_t)ol;
            out->opaque[1] = (uint16_t)ot;
            out->opaque[2] = (uint16_t)orr;
            out->opaque[3] = (uint16_t)ob;
        }
    } else {
        frame_cache_free(out);
    }
    free(prev);
    free(first);
    gif_stream_close(&g);
    return ok;
}

static bool get(const wchar_t *path, bool key_enabled, uint32_t key_rgb, int key_tolerance,
                int width, int height, const wchar_t *dir, bool allow_build, FrameCache *out)
{
    memset(out, 0, sizeof(*out));
    CacheHeader h;
    memset(&h, 0, sizeof(h));
    h.magic = CACHE_MAGIC;
    h.version = CACHE_VERSION;
    h.key_enabled = key_enabled ? 1u : 0u;
    h.key_rgb = key_enabled ? key_rgb & 0xFFFFFFu : 0u;
    h.key_tolerance = key_enabled ? key_tolerance : 0;
    h.width = width;
    h.height = height;

    wchar_t file[MAX_PATH * 2];
    bool cacheable = dir && *dir && file_identity(path, &h.file_size, &h.file_mtime) &&
                     cache_file(dir, path, &h, file, MAX_PATH * 2);
    if (cacheable && load(file, &h, out)) return true;
    if (!allow_build || !build(path, key_enabled, key_rgb, key_tolerance, out)) return false;
    if (out->width != width || out->height != height) {
        frame_cache_free(out);
        return false;
    }
    if (cacheable) {
        h.frame_count = out->frame_count;
        memcpy(h.opaque, out->opaque, sizeof(h.opaque));
        save(dir, file, &h, out);
    }
    return true;
}

bool frame_cache_get(const wchar_t *path, bool key_enabled, uint32_t key_rgb, int key_tolerance,
                     int width, int height, const wchar_t *dir, FrameCache *out)
{
    return get(path, key_enabled, key_rgb, key_tolerance, width, height, dir, true, out);
}

bool frame_cache_load(const wchar_t *path, bool key_enabled, uint32_t key_rgb, int key_tolerance,
                      int width, int height, const wchar_t *dir, FrameCache *out)
{
    return get(path, key_enabled, key_rgb, key_tolerance, width, height, dir, false, out);
}
