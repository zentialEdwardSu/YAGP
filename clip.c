#include "clip.h"
#include "app.h"

#include <stdlib.h>
#include <string.h>

static wchar_t *dup_wcs(const wchar_t *s)
{
    size_t n = (wcslen(s) + 1) * sizeof(wchar_t);
    wchar_t *d = (wchar_t *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

void clip_canvas_size(const GifStream *g, int pct, int *w, int *h)
{
    /* Below 100% the canvas is decoded at the displayed size (a 1000x1000 GIF
     * at 20% then keeps a 0.15 MB canvas instead of 3.9 MB). At 100% and
     * above it stays native and DWM scales it up. */
    if (pct >= 100) {
        *w = g->width;
        *h = g->height;
        return;
    }
    *w = MulDiv(g->width, pct, 100);
    *h = MulDiv(g->height, pct, 100);
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}

/* ---- frame cache, built off the UI thread ---------------------------------
 * Building decodes every frame once, which for a big GIF takes long enough to
 * freeze every overlay. An existing cache file is still read synchronously
 * (fast, and the hitbox is right from the first frame); a missing or stale one
 * is built by a single low-priority worker thread, one GIF at a time so memory
 * stays bounded. Until it arrives the clip plays with full uploads, its whole
 * window as hitbox and no seeking index, exactly as when no cache can be
 * written. */

typedef struct CacheJob {
    struct CacheJob *next;    /* Worker queue. */
    volatile LONG cancelled;  /* Superseded or clip gone: skip the build. */
    wchar_t *path;
    ClipKey key;
    int width, height;
    wchar_t dir[MAX_PATH];
    bool ok;
    FrameCache cache;
} CacheJob;

static SRWLOCK s_job_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE s_job_ready = CONDITION_VARIABLE_INIT;
static CacheJob *s_job_head, *s_job_tail;
static HANDLE s_worker;

void clip_job_done(void *job)
{
    CacheJob *j = (CacheJob *)job;
    if (!j) return;
    frame_cache_free(&j->cache);
    free(j->path);
    free(j);
}

static DWORD WINAPI cache_worker(void *arg)
{
    (void)arg;
    /* Lower CPU and I/O priority: animations on the UI thread come first. */
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    for (;;) {
        AcquireSRWLockExclusive(&s_job_lock);
        while (!s_job_head) SleepConditionVariableSRW(&s_job_ready, &s_job_lock, INFINITE, 0);
        CacheJob *j = s_job_head;
        s_job_head = j->next;
        if (!s_job_head) s_job_tail = NULL;
        ReleaseSRWLockExclusive(&s_job_lock);
        if (!j->cancelled) {
            j->ok = frame_cache_get(j->path, j->key.enabled, j->key.rgb, j->key.tolerance,
                                    j->width, j->height, j->dir, &j->cache);
        }
        /* From here the UI thread owns the job (clip_cache_ready). */
        if (!PostMessageW(g_app.tray_hwnd, WM_APP_CACHE_READY, 0, (LPARAM)j)) clip_job_done(j);
    }
}

static bool submit_job(CacheJob *j)
{
    if (!s_worker) {
        s_worker = CreateThread(NULL, 0, cache_worker, NULL, 0, NULL);
        if (!s_worker) return false;
    }
    AcquireSRWLockExclusive(&s_job_lock);
    if (s_job_tail) s_job_tail->next = j;
    else s_job_head = j;
    s_job_tail = j;
    ReleaseSRWLockExclusive(&s_job_lock);
    WakeConditionVariable(&s_job_ready);
    return true;
}

static void cancel_job(Clip *c)
{
    if (!c->job) return;
    InterlockedExchange(&c->job->cancelled, 1); /* Freed when its result arrives. */
    c->job = NULL;
}

static void drop_cache(Clip *c)
{
    free(c->dirty);
    free(c->frames);
    free(c->delays);
    c->dirty = NULL;
    c->frames = NULL;
    c->delays = NULL;
    c->frame_count = 0;
    memset(c->opaque, 0, sizeof(c->opaque)); /* Unknown: the whole window counts. */
}

static void adopt(Clip *c, FrameCache *fc)
{
    uint32_t *delays = (uint32_t *)malloc((size_t)fc->frame_count * sizeof(uint32_t));
    if (!delays) return; /* Keep playing without the cache. */
    for (int i = 0; i < fc->frame_count; ++i) delays[i] = fc->frames[i].delay_ms;
    c->dirty = fc->dirty; /* Ownership moves to the clip. */
    c->frames = fc->frames;
    c->delays = delays;
    c->frame_count = fc->frame_count;
    for (int i = 0; i < 4; ++i) c->opaque[i] = fc->opaque[i];
    fc->dirty = NULL;
    fc->frames = NULL;
}

/* Load (or have the worker build and store) the cache for c's key. */
static void load_cache(Clip *c, const ClipKey *k)
{
    FrameCache fc;
    drop_cache(c);
    cancel_job(c);
    if (frame_cache_load(c->path, k->enabled, k->rgb, k->tolerance, c->gif->width, c->gif->height,
                         g_app.cache_dir, &fc)) {
        adopt(c, &fc);
        frame_cache_free(&fc);
        return;
    }
    CacheJob *j = (CacheJob *)calloc(1, sizeof(CacheJob));
    if (j) {
        j->path = dup_wcs(c->path);
        j->key = *k;
        j->width = c->gif->width;
        j->height = c->gif->height;
        wcsncpy_s(j->dir, MAX_PATH, g_app.cache_dir, _TRUNCATE);
        if (j->path && submit_job(j)) {
            c->job = j;
            return;
        }
        clip_job_done(j);
    }
    /* No worker thread: build here. */
    if (frame_cache_get(c->path, k->enabled, k->rgb, k->tolerance, c->gif->width, c->gif->height,
                        g_app.cache_dir, &fc)) {
        adopt(c, &fc);
        frame_cache_free(&fc);
    }
}

bool clip_cache_ready(Clip *c, void *job)
{
    CacheJob *j = (CacheJob *)job;
    if (!c->gif || c->job != j) return false;
    c->job = NULL;
    if (j->ok && j->cache.width == c->gif->width && j->cache.height == c->gif->height) adopt(c, &j->cache);
    return true;
}

/* Restart at frame 0 with the canvas pct needs (1x1 when parking). */
static bool restart_at(Clip *c, int w, int h)
{
    gif_stream_set_output_size(c->gif, w, h); /* On failure it keeps its canvas. */
    return gif_stream_restart(c->gif) == 0 && gif_stream_next(c->gif) == 1;
}

bool clip_open(Clip *c, const char *name, const wchar_t *path, const ClipKey *key, int pct, bool parked)
{
    memset(c, 0, sizeof(*c));
    strncpy_s(c->name, sizeof(c->name), name, _TRUNCATE);
    c->path = dup_wcs(path);
    c->gif = (GifStream *)malloc(sizeof(GifStream));
    if (!c->path || !c->gif || gif_stream_open(c->gif, path) != 0) {
        free(c->gif);
        c->gif = NULL;
        clip_close(c);
        return false;
    }
    gif_stream_set_color_key(c->gif, key->enabled, key->rgb, key->tolerance);
    int w = 1, h = 1;
    if (!parked) clip_canvas_size(c->gif, pct, &w, &h);
    c->parked = parked;
    /* Decoding frame 0 also validates the file before anyone relies on it. */
    if (!restart_at(c, w, h)) {
        clip_close(c);
        return false;
    }
    load_cache(c, key);
    return true;
}

void clip_close(Clip *c)
{
    cancel_job(c);
    drop_cache(c);
    if (c->gif) {
        gif_stream_close(c->gif);
        free(c->gif);
    }
    free(c->path);
    memset(c, 0, sizeof(*c));
}

void clip_park(Clip *c)
{
    if (!c->gif || c->parked) return;
    gif_stream_set_output_size(c->gif, 1, 1);
    c->parked = true;
}

bool clip_activate(Clip *c, int pct)
{
    int w, h;
    clip_canvas_size(c->gif, pct, &w, &h);
    c->parked = false;
    return restart_at(c, w, h);
}

bool clip_rekey(Clip *c, const ClipKey *key, int pct)
{
    gif_stream_set_color_key(c->gif, key->enabled, key->rgb, key->tolerance);
    load_cache(c, key);
    if (c->parked) return true; /* Re-keyed from frame 0 when it next plays. */
    return clip_activate(c, pct);
}

bool clip_resize_canvas(Clip *c, int pct)
{
    if (c->parked) return true;
    return clip_activate(c, pct);
}
