#include "frame_cache.h"
#include "gif_stream.h"

#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CACHE_MAGIC 0x31434759u /* "YGC1" little-endian */
#define CACHE_VERSION 3 /* v2: mask removed; v3: opaque box */
#define MAX_CACHE_FRAMES 65535

/* On-disk layout (little-endian):
 *   CacheHeader
 *   uint16_t dirty[frame_count][4]
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

static bool cache_file(const wchar_t *dir, const wchar_t *path, wchar_t *out, size_t cap)
{
    return _snwprintf_s(out, cap, _TRUNCATE, L"%s\\%016llx.ygc", dir,
                        (unsigned long long)path_hash(path)) > 0;
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
        ok = out->dirty && read_exact(f, out->dirty, dsize);
        /* Reject trailing junk / truncated writes. */
        LARGE_INTEGER size, pos = {0};
        ok = ok && GetFileSizeEx(f, &size) && SetFilePointerEx(f, pos, &pos, FILE_CURRENT) &&
             size.QuadPart == pos.QuadPart;
        for (int i = 0; ok && i < h.frame_count; ++i) {
            const uint16_t *d = out->dirty[i];
            ok = d[2] <= h.width && d[3] <= h.height && (d[0] >= d[2] || (d[0] < d[2] && d[1] < d[3]));
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
    if (_snwprintf_s(tmp, MAX_PATH * 2, _TRUNCATE, L"%s.%lu.tmp", file, GetCurrentProcessId()) <= 0) return;
    SHCreateDirectoryExW(NULL, dir, NULL); /* Creates intermediate folders too. */
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    bool ok = write_exact(f, h, sizeof(*h)) &&
              write_exact(f, c->dirty, (size_t)c->frame_count * sizeof(*c->dirty));
    CloseHandle(f);
    /* A cache is disposable: no flush, just an atomic rename. */
    if (!ok || !MoveFileExW(tmp, file, MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp);
}

/* Bounding box of bytes that differ between two premultiplied BGRA canvases. */
static void diff_box(const uint8_t *a, const uint8_t *b, int w, int h, uint16_t box[4])
{
    int l = w, t = h, r = 0, btm = 0;
    size_t stride = (size_t)w * 4;
    for (int y = 0; y < h; ++y) {
        const uint32_t *ra = (const uint32_t *)(a + (size_t)y * stride);
        const uint32_t *rb = (const uint32_t *)(b + (size_t)y * stride);
        if (!memcmp(ra, rb, stride)) continue;
        int x0 = 0, x1 = w;
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

/* Grows [*l, *r) x [*t, *b) to cover every pixel of the canvas with alpha != 0. */
static void grow_opaque(const uint8_t *bgra, int w, int h, int *l, int *t, int *r, int *b)
{
    for (int y = 0; y < h; ++y) {
        const uint8_t *row = bgra + (size_t)y * (size_t)w * 4;
        int x0 = 0;
        while (x0 < w && !row[x0 * 4 + 3]) ++x0;
        if (x0 == w) continue;
        int x1 = w;
        while (!row[(x1 - 1) * 4 + 3]) --x1; /* Stops at x0 at the latest. */
        if (x0 < *l) *l = x0;
        if (x1 > *r) *r = x1;
        if (y < *t) *t = y;
        if (y + 1 > *b) *b = y + 1;
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
    bool ok = prev && first && out->dirty;
    int n = 0;
    int ol = g.width, ot = g.height, orr = 0, ob = 0;
    while (ok) {
        int status = gif_stream_next(&g);
        /* Matches playback: an error after at least one frame ends the loop. */
        if (status == 0 || (status < 0 && n > 0)) break;
        if (status < 0 || n == MAX_CACHE_FRAMES) { ok = false; break; }
        if (n == cap) {
            cap *= 2;
            uint16_t (*grown)[4] = (uint16_t (*)[4])realloc(out->dirty, (size_t)cap * sizeof(*out->dirty));
            if (!grown) { ok = false; break; }
            out->dirty = grown;
        }
        grow_opaque(g.bgra, g.width, g.height, &ol, &ot, &orr, &ob);
        if (n == 0) memcpy(first, g.bgra, canvas);
        else diff_box(prev, g.bgra, g.width, g.height, out->dirty[n]);
        memcpy(prev, g.bgra, canvas);
        ++n;
    }
    if (ok && n == 0) ok = false;
    /* Wrap: playback restarts from a cleared canvas, so going from the last
     * frame to frame 0 changes exactly prev -> first. */
    if (ok) diff_box(prev, first, g.width, g.height, out->dirty[0]);
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

bool frame_cache_get(const wchar_t *path, bool key_enabled, uint32_t key_rgb, int key_tolerance,
                     int width, int height, const wchar_t *dir, FrameCache *out)
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
                     cache_file(dir, path, file, MAX_PATH * 2);
    if (cacheable && load(file, &h, out)) return true;
    if (!build(path, key_enabled, key_rgb, key_tolerance, out)) return false;
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
