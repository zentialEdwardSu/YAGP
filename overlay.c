#include "overlay.h"
#include "app.h"
#include "contacts.h"
#include "frame_cache.h"
#include "i18n.h"
#include "monitors.h"

#include <stdlib.h>
#include <string.h>

#define OVERLAY_CLASS L"YAGP.Overlay"

#define DEFAULT_KEY_TOLERANCE 16

static wchar_t *dup_wcs(const wchar_t *s)
{
    size_t n = (wcslen(s) + 1) * sizeof(wchar_t);
    wchar_t *d = (wchar_t *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static int valid_scale(int pct)
{
    if (pct <= 0) return 100; /* Missing / corrupt config value. */
    if (pct < APP_SCALE_MIN) return APP_SCALE_MIN;
    if (pct > APP_SCALE_MAX) return APP_SCALE_MAX;
    return pct;
}

static SIZE size_at(const GifStream *g, int pct)
{
    SIZE s;
    s.cx = MulDiv(g->width, pct, 100);
    s.cy = MulDiv(g->height, pct, 100);
    if (s.cx < 1) s.cx = 1;
    if (s.cy < 1) s.cy = 1;
    return s;
}

static SIZE scaled_size(const Overlay *o)
{
    return size_at(o->gif, o->scale_pct);
}

/* Below 100% the canvas is decoded at the displayed size (a 1000x1000 GIF at
 * 20% then keeps a 0.15 MB canvas instead of 3.9 MB). At 100% and above it
 * stays native and DWM scales it up, as before. */
static SIZE canvas_size(const GifStream *g, int pct)
{
    if (pct >= 100) {
        SIZE s = {g->width, g->height};
        return s;
    }
    return size_at(g, pct);
}

/* DComp scale from the canvas to the window: 1 below 100%, except while the
 * scale slider previews a size the canvas was not decoded for. */
static float visual_scale(const Overlay *o)
{
    return (float)scaled_size(o).cx / (float)o->gif->out_w;
}

const wchar_t *overlay_name(const Overlay *o)
{
    const wchar_t *slash = wcsrchr(o->path, L'\\');
    const wchar_t *fwd = wcsrchr(o->path, L'/');
    if (fwd > slash) slash = fwd;
    return slash ? slash + 1 : o->path;
}

bool overlay_is_animating(const Overlay *o)
{
    return o->surface && !o->hidden && !o->paused && !o->failed && !o->is_static;
}

/* Opens path and decodes its first frame with o's colour key, at the canvas
 * size o's scale needs. Returns NULL on failure. */
static GifStream *open_first_frame(const Overlay *o, const wchar_t *path)
{
    GifStream *g = (GifStream *)malloc(sizeof(GifStream));
    if (!g) return NULL;
    if (gif_stream_open(g, path) != 0) {
        free(g);
        return NULL;
    }
    gif_stream_set_color_key(g, o->key_enabled, o->key_rgb, o->key_tolerance);
    SIZE c = canvas_size(g, o->scale_pct);
    gif_stream_set_output_size(g, c.cx, c.cy); /* On failure it simply stays full size. */
    if (gif_stream_next(g) != 1) {
        gif_stream_close(g);
        free(g);
        return NULL;
    }
    return g;
}

static void free_gif(GifStream *g)
{
    if (!g) return;
    gif_stream_close(g);
    free(g);
}

static void reset_timing(Overlay *o)
{
    o->failed = false;
    o->is_static = false;
    o->frame_index = 1;
    o->next_frame = app_now() + (int64_t)o->gif->delay_ms * g_app.qpc_freq / 1000;
}

static void apply_ex_style(Overlay *o)
{
    LONG_PTR ex = GetWindowLongPtrW(o->hwnd, GWL_EXSTYLE);
    ex &= ~(LONG_PTR)(WS_EX_LAYERED | WS_EX_TRANSPARENT);
    /* Cross-process click-through needs both bits; LWA_ALPHA=255 leaves the
     * DirectComposition content untouched. */
    if (o->clickthrough) ex |= WS_EX_LAYERED | WS_EX_TRANSPARENT;
    SetWindowLongPtrW(o->hwnd, GWL_EXSTYLE, ex);
    if (o->clickthrough) SetLayeredWindowAttributes(o->hwnd, 0, 255, LWA_ALPHA);
    SetWindowPos(o->hwnd, o->topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

static void dirty_reset(Overlay *o)
{
    free(o->dirty);
    o->dirty = NULL;
    o->dirty_count = 0;
}

/* ---- frame cache, built off the UI thread ---------------------------------
 * Building decodes every frame once, which for a big GIF takes long enough to
 * freeze every overlay. An existing cache file is still read synchronously
 * (fast, and the hitbox is right from the first frame); a missing or stale one
 * is built by a single low-priority worker thread, one GIF at a time so memory
 * stays bounded. Until it arrives the overlay plays with full uploads and its
 * whole window as hitbox, exactly as when no cache can be written. */

typedef struct CacheJob {
    struct CacheJob *next;    /* Worker queue. */
    volatile LONG cancelled;  /* Superseded or overlay gone: skip the build. */
    wchar_t *path;
    bool key_enabled;
    uint32_t key_rgb;
    int key_tolerance;
    int width, height;
    wchar_t dir[MAX_PATH];
    bool ok;
    FrameCache cache;
} CacheJob;

static SRWLOCK s_job_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE s_job_ready = CONDITION_VARIABLE_INIT;
static CacheJob *s_job_head, *s_job_tail;
static HANDLE s_worker;

static void job_free(CacheJob *j)
{
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
            j->ok = frame_cache_get(j->path, j->key_enabled, j->key_rgb, j->key_tolerance,
                                    j->width, j->height, j->dir, &j->cache);
        }
        /* From here the UI thread owns the job (overlay_cache_ready). */
        if (!PostMessageW(g_app.tray_hwnd, WM_APP_CACHE_READY, 0, (LPARAM)j)) job_free(j);
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

static void cancel_cache_job(Overlay *o)
{
    if (!o->cache_job) return;
    InterlockedExchange(&o->cache_job->cancelled, 1); /* Freed when its result arrives. */
    o->cache_job = NULL;
}

static void adopt_frame_cache(Overlay *o, FrameCache *c)
{
    o->dirty = c->dirty; /* Ownership moves to the overlay. */
    o->dirty_count = c->frame_count;
    for (int i = 0; i < 4; ++i) o->opaque[i] = c->opaque[i];
    c->dirty = NULL;
}

/* Load (or have the worker build and store) the per-frame dirty boxes. On
 * failure playback falls back to full-frame uploads. */
static void load_frame_cache(Overlay *o)
{
    FrameCache c;
    dirty_reset(o);
    memset(o->opaque, 0, sizeof(o->opaque)); /* Unknown: the whole window counts. */
    cancel_cache_job(o);
    if (frame_cache_load(o->path, o->key_enabled, o->key_rgb, o->key_tolerance,
                         o->gif->width, o->gif->height, g_app.cache_dir, &c)) {
        adopt_frame_cache(o, &c);
        return;
    }
    CacheJob *j = (CacheJob *)calloc(1, sizeof(CacheJob));
    if (j) {
        j->path = dup_wcs(o->path);
        j->key_enabled = o->key_enabled;
        j->key_rgb = o->key_rgb;
        j->key_tolerance = o->key_tolerance;
        j->width = o->gif->width;
        j->height = o->gif->height;
        wcsncpy_s(j->dir, MAX_PATH, g_app.cache_dir, _TRUNCATE);
        if (j->path && submit_job(j)) {
            o->cache_job = j;
            return;
        }
        job_free(j);
    }
    /* No worker thread: build here, as before. */
    if (frame_cache_get(o->path, o->key_enabled, o->key_rgb, o->key_tolerance,
                        o->gif->width, o->gif->height, g_app.cache_dir, &c)) {
        adopt_frame_cache(o, &c);
    }
}

void overlay_cache_ready(void *job)
{
    CacheJob *j = (CacheJob *)job;
    for (Overlay *o = g_app.overlays; o; o = o->next) {
        if (o->cache_job != j) continue;
        o->cache_job = NULL;
        if (j->ok && j->cache.width == o->gif->width && j->cache.height == o->gif->height) {
            adopt_frame_cache(o, &j->cache);
        }
        break;
    }
    job_free(j);
}

static POINT window_center(const Overlay *o)
{
    RECT r;
    GetWindowRect(o->hwnd, &r);
    POINT c = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    return c;
}

/* Resize the window around c and scale the visual to match. */
static void apply_size_at(Overlay *o, POINT c)
{
    SIZE s = scaled_size(o);
    SetWindowPos(o->hwnd, NULL, c.x - s.cx / 2, c.y - s.cy / 2, s.cx, s.cy,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    render_surface_set_scale(o->surface, visual_scale(o));
    o->pos_x = c.x - s.cx / 2;
    o->pos_y = c.y - s.cy / 2;
}

static void apply_size(Overlay *o)
{
    apply_size_at(o, window_center(o));
}

static void center_on_primary(Overlay *o, int cascade)
{
    RECT work;
    SIZE s = scaled_size(o);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int offset = (cascade % 8) * 32;
    int x = work.left + (work.right - work.left - s.cx) / 2 + offset;
    int y = work.top + (work.bottom - work.top - s.cy) / 2 + offset;
    SetWindowPos(o->hwnd, NULL, x, y, s.cx, s.cy, SWP_NOZORDER | SWP_NOACTIVATE);
    o->pos_x = x;
    o->pos_y = y;
}

bool overlay_attach_surface(Overlay *o)
{
    if (o->surface || o->hidden) return true; /* A hidden GIF gets its swap chain when shown. */
    o->surface = render_surface_create(g_app.device, o->hwnd, o->gif->out_w, o->gif->out_h);
    if (!o->surface) return false;
    render_surface_set_scale(o->surface, visual_scale(o));
    render_surface_set_flip(o->surface, o->flip_x);
    return render_surface_present(o->surface, o->gif->bgra);
}

void overlay_detach_surface(Overlay *o)
{
    render_surface_destroy(o->surface);
    o->surface = NULL;
}

/* ---- motion scripts ------------------------------------------------------ */

typedef struct Builtin {
    const wchar_t *spec;     /* As stored in the config. */
    const wchar_t *resource; /* RCDATA name in presets.rc. */
    const char *chunk;       /* Shown in error messages. */
    const wchar_t *label;
} Builtin;

/* Same order as OVERLAY_CMD_SCRIPT_PATROL.. */
static const Builtin OVERLAY_BUILTINS[] = {
    {L"builtin:patrol", L"PATROL", "patrol.lua", TXT_PRESET_PATROL},
    {L"builtin:wander", L"WANDER", "wander.lua", TXT_PRESET_WANDER},
    {L"builtin:bounce", L"BOUNCE", "bounce.lua", TXT_PRESET_BOUNCE},
    {L"builtin:follow", L"FOLLOW", "follow.lua", TXT_PRESET_FOLLOW},
    {L"builtin:flee", L"FLEE", "flee.lua", TXT_PRESET_FLEE},
};
#define BUILTIN_COUNT ((int)(sizeof(OVERLAY_BUILTINS) / sizeof(OVERLAY_BUILTINS[0])))
#define SCRIPT_MAX_BYTES (1024 * 1024)
#define SCRIPT_MAX_DT 0.25 /* Seconds; a long stall must not teleport the GIF. */

static const Builtin *find_builtin(const wchar_t *spec)
{
    for (int i = 0; spec && i < BUILTIN_COUNT; ++i) {
        if (wcscmp(spec, OVERLAY_BUILTINS[i].spec) == 0) return &OVERLAY_BUILTINS[i];
    }
    return NULL;
}

static const wchar_t *file_part(const wchar_t *path)
{
    const wchar_t *slash = wcsrchr(path, L'\\');
    const wchar_t *fwd = wcsrchr(path, L'/');
    if (fwd > slash) slash = fwd;
    return slash ? slash + 1 : path;
}

static char *to_utf8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *u = n > 0 ? (char *)malloc((size_t)n) : NULL;
    if (u && WideCharToMultiByte(CP_UTF8, 0, w, -1, u, n, NULL, NULL) <= 0) u[0] = 0;
    return u;
}

/* Reads a .lua file (UTF-8, optional BOM). Caller frees *out. */
static bool read_script_file(const wchar_t *path, char **out, size_t *len)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    DWORD got = 0;
    char *buf = NULL;
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart >= 0 && size.QuadPart <= SCRIPT_MAX_BYTES &&
              (buf = (char *)malloc((size_t)size.QuadPart + 1)) != NULL &&
              ReadFile(f, buf, (DWORD)size.QuadPart, &got, NULL) && got == (DWORD)size.QuadPart;
    CloseHandle(f);
    if (!ok) {
        free(buf);
        return false;
    }
    size_t skip = got >= 3 && memcmp(buf, "\xEF\xBB\xBF", 3) == 0 ? 3 : 0;
    memmove(buf, buf + skip, got - skip);
    *out = buf;
    *len = got - skip;
    return true;
}

static void host_get_rect(void *ctx, double *x, double *y, int *w, int *h)
{
    Overlay *o = (Overlay *)ctx;
    SIZE s = scaled_size(o);
    *x = o->pos_x;
    *y = o->pos_y;
    *w = s.cx;
    *h = s.cy;
}

static void host_set_pos(void *ctx, double x, double y)
{
    Overlay *o = (Overlay *)ctx;
    o->pos_x = x;
    o->pos_y = y;
    int ix = (int)(x < 0 ? x - 0.5 : x + 0.5), iy = (int)(y < 0 ? y - 0.5 : y + 0.5);
    /* Sub-pixel steps often round to where the window already is. win_x/win_y
     * follow every move (ours or not), so no GetWindowRect per tick. */
    if (o->win_x == ix && o->win_y == iy) return;
    /* No app_changed(): the position is read back when the config is saved.
     * Lean flags measured ~30% cheaper per move: we never handle
     * WM_WINDOWPOSCHANGING, and DirectComposition content needs no redraw/copy. */
    SetWindowPos(o->hwnd, NULL, ix, iy, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING |
                 SWP_NOREDRAW | SWP_NOCOPYBITS | SWP_NOOWNERZORDER);
}

static void host_set_flip(void *ctx, bool flip_x)
{
    Overlay *o = (Overlay *)ctx;
    o->flip_x = flip_x;
    render_surface_set_flip(o->surface, flip_x);
}

static void host_set_rate(void *ctx, int hz)
{
    Overlay *o = (Overlay *)ctx;
    o->tick_interval = g_app.qpc_freq / hz;
}

/* GetAsyncKeyState reports physical buttons, so undo a left-handed swap to give
 * scripts the logical ones. GetCursorPos fails on the secure desktop. */
static bool host_get_mouse(void *ctx, ScriptMouse *m)
{
    (void)ctx;
    POINT p;
    if (!GetCursorPos(&p)) return false;
    bool swap = GetSystemMetrics(SM_SWAPBUTTON) != 0;
    bool phys_left = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    bool phys_right = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    m->x = p.x;
    m->y = p.y;
    m->left = swap ? phys_right : phys_left;
    m->right = swap ? phys_left : phys_right;
    m->middle = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
    return true;
}

/* Rect as other scripts see it. A window being dragged has not updated
 * pos_x/pos_y yet, so read it back from the window. */
static void peer_rect(const Overlay *o, double *x, double *y)
{
    if (o->dragging) {
        RECT r;
        GetWindowRect(o->hwnd, &r);
        *x = r.left;
        *y = r.top;
    } else {
        *x = o->pos_x;
        *y = o->pos_y;
    }
}

/* Hitbox inside the window: out = dx, dy, w, h in display pixels. */
static void overlay_hitbox(const Overlay *o, int out[4])
{
    SIZE s = scaled_size(o);
    hitbox_in_window(o->has_override ? o->hitbox_override : o->opaque, o->gif->width, o->gif->height,
                     s.cx, s.cy, o->flip_x, out);
}

static void host_get_hitbox(void *ctx, int *dx, int *dy, int *w, int *h)
{
    int hb[4];
    overlay_hitbox((const Overlay *)ctx, hb);
    *dx = hb[0];
    *dy = hb[1];
    *w = hb[2];
    *h = hb[3];
}

/* box = x, y, w, h in GIF pixels, clipped to the GIF; NULL = automatic. */
static void host_set_hitbox(void *ctx, const int *box)
{
    Overlay *o = (Overlay *)ctx;
    o->has_override = false;
    if (!box) return;
    int l = box[0], t = box[1], r = box[0] + box[2], b = box[1] + box[3];
    if (r > o->gif->width) r = o->gif->width;
    if (b > o->gif->height) b = o->gif->height;
    if (l >= r || t >= b) return; /* Nothing of it is inside the GIF: keep automatic. */
    o->hitbox_override[0] = l;
    o->hitbox_override[1] = t;
    o->hitbox_override[2] = r;
    o->hitbox_override[3] = b;
    o->has_override = true;
}

static void fill_peer(const Overlay *o, ScriptPeer *p)
{
    SIZE s = scaled_size(o);
    int hb[4];
    overlay_hitbox(o, hb);
    p->id = o->id;
    peer_rect(o, &p->x, &p->y);
    p->w = s.cx;
    p->h = s.cy;
    p->hx = p->x + hb[0];
    p->hy = p->y + hb[1];
    p->hw = hb[2];
    p->hh = hb[3];
    bool moving = overlay_is_moving(o);
    p->vx = moving ? o->vx : 0;
    p->vy = moving ? o->vy : 0;
    p->scripted = o->script != NULL;
    p->paused = o->paused;
    p->name = o->name_u8 ? o->name_u8 : "";
}

static int host_get_peers(void *ctx, ScriptPeer *out, int cap)
{
    int n = 0;
    for (Overlay *o = g_app.overlays; o && n < cap; o = o->next) {
        if (o != (Overlay *)ctx && !o->hidden) fill_peer(o, &out[n++]);
    }
    return n;
}

static const ScriptHostOps HOST_OPS = {host_get_rect, host_set_pos, host_set_flip, host_set_rate,
                                       host_get_mouse, host_get_peers, host_get_hitbox, host_set_hitbox};

static void sync_pos(Overlay *o)
{
    RECT r;
    GetWindowRect(o->hwnd, &r);
    o->pos_x = r.left;
    o->pos_y = r.top;
    o->win_x = r.left;
    o->win_y = r.top;
}

/* Stops the script (window stays where it is) and tells the user why. */
static void script_fail(Overlay *o, const wchar_t *err)
{
    script_destroy(o->script);
    o->script = NULL;
    o->script_failed = true;
    wchar_t title[64];
    _snwprintf_s(title, 64, _TRUNCATE, TXT_SCRIPT_STOPPED_TITLE, overlay_name(o));
    app_notify(title, err && *err ? err : TXT_UNKNOWN_ERROR);
}

static void script_notify(Overlay *o, const char *fn)
{
    wchar_t err[512];
    if (o->script && !script_event(o->script, fn, err, 512)) script_fail(o, err);
}

/* The window was moved or resized by something other than the script. */
static void moved_externally(Overlay *o)
{
    sync_pos(o);
    script_notify(o, "on_moved");
}

static void reset_tick(Overlay *o)
{
    o->last_tick = app_now();
    o->next_tick = o->last_tick;
    o->vx = o->vy = 0;
}

void overlay_set_script(Overlay *o, const wchar_t *spec)
{
    if (!spec) spec = L"";
    wchar_t *copy = dup_wcs(spec);
    if (!copy) return;
    script_destroy(o->script);
    o->script = NULL;
    o->script_failed = false;
    o->has_override = false;
    free(o->script_spec);
    o->script_spec = copy;
    if (o->flip_x) host_set_flip(o, false);
    o->tick_interval = g_app.qpc_freq / SCRIPT_RATE_DEFAULT;
    if (!*copy) return;

    char *src = NULL, *chunk = NULL;
    size_t len = 0;
    wchar_t err[512] = TXT_ERR_BUILTIN_MISSING;
    const Builtin *b = find_builtin(copy);
    if (b) {
        HRSRC res = FindResourceW(NULL, b->resource, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
        HGLOBAL mem = res ? LoadResource(NULL, res) : NULL;
        const void *data = mem ? LockResource(mem) : NULL;
        len = res ? SizeofResource(NULL, res) : 0;
        if (data && (src = (char *)malloc(len + 1)) != NULL) memcpy(src, data, len);
    } else if (read_script_file(copy, &src, &len)) {
        chunk = to_utf8(file_part(copy));
    } else {
        _snwprintf_s(err, 512, _TRUNCATE, TXT_ERR_READ_SCRIPT, copy);
    }
    char *name = to_utf8(overlay_name(o));
    sync_pos(o);
    if (src) o->script = script_create(&HOST_OPS, o, src, len, b ? b->chunk : chunk, name, o->id, err, 512);
    free(src);
    free(chunk);
    free(name);
    if (!o->script) script_fail(o, err);
    reset_tick(o);
}

bool overlay_is_moving(const Overlay *o)
{
    return o->script && o->surface && !o->hidden && !o->paused && !o->dragging && !o->scaling &&
           script_has_update(o->script);
}

void overlay_step(Overlay *o, int64_t now)
{
    double dt = (double)(now - o->last_tick) / (double)g_app.qpc_freq;
    if (dt < 0) dt = 0;
    if (dt > SCRIPT_MAX_DT) dt = SCRIPT_MAX_DT;
    o->last_tick = now;
    double x0 = o->pos_x, y0 = o->pos_y;
    wchar_t err[512];
    if (!script_update(o->script, dt, err, 512)) {
        script_fail(o, err);
        return;
    }
    o->vx = dt > 0 ? (o->pos_x - x0) / dt : 0;
    o->vy = dt > 0 ? (o->pos_y - y0) / dt : 0;
    o->next_tick += o->tick_interval;
    if (o->next_tick <= now) o->next_tick = now + o->tick_interval;
}

void overlay_screens_changed(Overlay *o)
{
    sync_pos(o);
    script_notify(o, "on_screens_changed");
}

void overlay_set_hidden(Overlay *o, bool hidden)
{
    if (o->hidden == hidden) return;
    o->hidden = hidden;
    if (hidden) {
        ShowWindow(o->hwnd, SW_HIDE);
        overlay_detach_surface(o);
        /* Keep the decoder (size, frame cache, hitbox) but drop the canvas. */
        gif_stream_set_output_size(o->gif, 1, 1);
        return;
    }
    SIZE c = canvas_size(o->gif, o->scale_pct);
    bool ok = gif_stream_set_output_size(o->gif, c.cx, c.cy) == 0 && gif_stream_restart(o->gif) == 0 &&
              gif_stream_next(o->gif) == 1;
    /* Its monitor may have gone away while it was hidden. */
    RECT r;
    GetWindowRect(o->hwnd, &r);
    if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) center_on_primary(o, 0);
    overlay_attach_surface(o);
    reset_timing(o);
    o->failed = !ok;
    reset_tick(o);
    ShowWindow(o->hwnd, SW_SHOWNOACTIVATE);
    moved_externally(o); /* The script resumes from wherever the window is now. */
}

/* ---- contacts between overlays -------------------------------------------- */

static Contacts s_contacts;

static Overlay *find_overlay(int id)
{
    for (Overlay *o = g_app.overlays; o; o = o->next) {
        if (o->id == id) return o;
    }
    return NULL;
}

/* Tells o's script about other. other may be gone (removed since the last
 * update): the script then gets a peer with only its id. */
static void notify_contact(Overlay *o, Overlay *other, int other_id, bool begin)
{
    if (!o || !o->script) return;
    ScriptPeer p;
    memset(&p, 0, sizeof(p));
    p.id = other_id;
    p.name = "";
    if (other) fill_peer(other, &p);
    wchar_t err[512];
    if (!script_event_peer(o->script, begin ? "on_collide" : "on_separate", &p, err, 512)) script_fail(o, err);
}

static void on_contact(void *ctx, int a, int b, bool begin)
{
    (void)ctx;
    Overlay *oa = find_overlay(a), *ob = find_overlay(b);
    notify_contact(oa, ob, b, begin);
    notify_contact(ob, oa, a, begin);
}

void overlay_update_contacts(void)
{
    ContactBox boxes[SCRIPT_MAX_PEERS + 1];
    int n = 0;
    for (Overlay *o = g_app.overlays; o && n < SCRIPT_MAX_PEERS + 1; o = o->next) {
        if (o->hidden) continue; /* Not on screen: touches nothing. */
        double x, y;
        int hb[4];
        peer_rect(o, &x, &y);
        overlay_hitbox(o, hb);
        ContactBox *bx = &boxes[n++];
        bx->id = o->id;
        bx->l = (int)(x < 0 ? x - 0.5 : x + 0.5) + hb[0];
        bx->t = (int)(y < 0 ? y - 0.5 : y + 0.5) + hb[1];
        bx->r = bx->l + hb[2];
        bx->b = bx->t + hb[3];
    }
    /* Callbacks only run scripts; none of them can add or remove an overlay. */
    contacts_update(&s_contacts, boxes, n, on_contact, NULL);
}

static void show_context_menu(Overlay *o)
{
    DWORD pos = GetMessagePos();
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    overlay_build_menu(o, menu, 0);
    UINT cmd = app_track_menu(menu, (short)LOWORD(pos), (short)HIWORD(pos));
    DestroyMenu(menu);
    if (cmd) overlay_command(o, cmd); /* May free o. */
}

static LRESULT CALLBACK overlay_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Overlay *o = (Overlay *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
        break;
    case WM_NCHITTEST:
        /* HTCAPTION lets the system drag the window with no extra code. */
        if (o) return o->locked ? HTCLIENT : HTCAPTION;
        break;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCRBUTTONDOWN:
    case WM_RBUTTONDOWN:
        return 0;
    case WM_NCRBUTTONUP:
    case WM_RBUTTONUP:
        /* Handled here so DefWindowProc never opens the system menu. */
        if (o) show_context_menu(o);
        return 0;
    case WM_NCLBUTTONDBLCLK:
        return 0; /* No maximize on caption double-click. */
    case WM_SYSCOMMAND:
        switch (wp & 0xFFF0) {
        case SC_MAXIMIZE: case SC_MINIMIZE: case SC_SIZE: case SC_KEYMENU: case SC_MOUSEMENU:
            return 0;
        }
        break;
    case WM_ENTERSIZEMOVE:
        if (o) o->dragging = true;
        app_modal_begin();
        break;
    case WM_EXITSIZEMOVE:
        app_modal_end();
        if (o) {
            o->dragging = false;
            reset_tick(o);
            moved_externally(o);
        }
        app_changed();
        break;
    case WM_WINDOWPOSCHANGED: {
        const WINDOWPOS *wpos = (const WINDOWPOS *)lp;
        if (o && !(wpos->flags & SWP_NOMOVE)) {
            o->win_x = wpos->x;
            o->win_y = wpos->y;
        }
        break; /* DefWindowProc still sends WM_MOVE / WM_SIZE. */
    }
    case WM_DPICHANGED:
        return 0; /* GIF pixels stay 1:1 with device pixels; keep our size. */
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        ValidateRect(hwnd, NULL); /* Content comes from DirectComposition. */
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool overlay_register_class(HINSTANCE instance)
{
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = overlay_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(NULL, IDC_SIZEALL);
    wc.lpszClassName = OVERLAY_CLASS;
    return RegisterClassExW(&wc) != 0;
}

Overlay *overlay_create(const OverlayConfig *cfg, bool use_position, int cascade)
{
    Overlay *o = (Overlay *)calloc(1, sizeof(Overlay));
    if (!o) return NULL;
    o->id = g_app.next_overlay_id++;
    o->scale_pct = valid_scale(cfg->scale_pct);
    o->paused = cfg->paused;
    o->topmost = cfg->topmost;
    o->locked = cfg->locked;
    o->clickthrough = cfg->clickthrough;
    o->hidden = cfg->hidden;
    o->key_enabled = cfg->key_enabled;
    o->key_rgb = cfg->key_rgb & 0xFFFFFFu;
    o->key_tolerance = cfg->key_enabled ? cfg->key_tolerance : DEFAULT_KEY_TOLERANCE;
    if (o->key_tolerance < 0 || o->key_tolerance > 255) o->key_tolerance = DEFAULT_KEY_TOLERANCE;
    o->path = dup_wcs(cfg->path);
    o->name_u8 = o->path ? to_utf8(overlay_name(o)) : NULL;
    o->gif = o->path ? open_first_frame(o, o->path) : NULL;
    if (!o->gif) goto fail;

    /* WS_EX_TOOLWINDOW without an owner keeps it off the taskbar and Alt+Tab.
     * WS_EX_NOREDIRECTIONBITMAP: DWM composes only our DirectComposition visual. */
    DWORD ex = WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE;
    if (o->topmost) ex |= WS_EX_TOPMOST;
    SIZE s = scaled_size(o);
    o->hwnd = CreateWindowExW(ex, OVERLAY_CLASS, overlay_name(o), WS_POPUP,
                              cfg->x, cfg->y, s.cx, s.cy, NULL, NULL, g_app.instance, o);
    if (!o->hwnd) goto fail;

    RECT r = {cfg->x, cfg->y, cfg->x + s.cx, cfg->y + s.cy};
    if (!use_position || !MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) center_on_primary(o, cascade);
    if (o->clickthrough) apply_ex_style(o);
    load_frame_cache(o);
    if (!overlay_attach_surface(o)) goto fail;
    reset_timing(o);
    sync_pos(o);
    if (o->hidden) gif_stream_set_output_size(o->gif, 1, 1); /* Restored hidden: no canvas until shown. */
    else ShowWindow(o->hwnd, SW_SHOWNOACTIVATE);
    /* A custom script that has gone missing is dropped silently on restore. */
    if (cfg->script && *cfg->script &&
        (find_builtin(cfg->script) || GetFileAttributesW(cfg->script) != INVALID_FILE_ATTRIBUTES)) {
        overlay_set_script(o, cfg->script);
    }
    return o;

fail:
    overlay_destroy(o);
    return NULL;
}

void overlay_destroy(Overlay *o)
{
    if (!o) return;
    script_destroy(o->script);
    free(o->script_spec);
    overlay_detach_surface(o);
    if (o->hwnd) {
        SetWindowLongPtrW(o->hwnd, GWLP_USERDATA, 0);
        DestroyWindow(o->hwnd);
    }
    cancel_cache_job(o);
    free_gif(o->gif);
    dirty_reset(o);
    free(o->path);
    free(o->name_u8);
    free(o);
}

void overlay_advance(Overlay *o, int64_t now)
{
    int index = o->frame_index; /* 0-based index of the frame about to be decoded. */
    int status = gif_stream_next(o->gif);
    /* Many GIFs have truncated trailers; after a full pass treat errors as end-of-loop. */
    if (status == 0 || (status < 0 && o->frame_index > 1)) {
        if (o->frame_index <= 1) {
            o->is_static = true; /* Single frame: already on screen, stop scheduling. */
            return;
        }
        if (gif_stream_restart(o->gif) < 0) {
            o->failed = true;
            return;
        }
        o->frame_index = 0;
        index = 0;
        status = gif_stream_next(o->gif);
    }
    if (status != 1) {
        o->failed = true;
        return;
    }
    o->frame_index++;
    if (o->dirty && index < o->dirty_count) {
        /* Boxes are in GIF pixels; a change no canvas pixel samples maps to an
         * empty box, and the present is skipped like for an identical frame. */
        const uint16_t *d = o->dirty[index];
        int box[4];
        gif_stream_map_box(o->gif, d[0], d[1], d[2], d[3], box);
        RECT rc = {box[0], box[1], box[2], box[3]};
        if (box[0] >= box[2] || box[1] >= box[3]) SetRectEmpty(&rc);
        render_surface_present_dirty(o->surface, o->gif->bgra, &rc);
    } else {
        render_surface_present(o->surface, o->gif->bgra); /* Cache missing or out of sync. */
    }

    int64_t delay = (int64_t)o->gif->delay_ms * g_app.qpc_freq / 1000;
    o->next_frame += delay; /* Accumulate to avoid drift... */
    if (o->next_frame <= now) o->next_frame = now + delay; /* ...but never burst to catch up. */
}

void overlay_replace(Overlay *o, const wchar_t *path)
{
    GifStream *g = open_first_frame(o, path);
    wchar_t *p = dup_wcs(path);
    if (!g || !p) {
        free_gif(g);
        free(p);
        app_error(TXT_ERR_OPEN_GIF, path);
        return;
    }
    if (o->surface && !render_surface_resize(o->surface, g->out_w, g->out_h)) {
        free_gif(g);
        free(p);
        app_error(TXT_ERR_SWAPCHAIN, path);
        return;
    }
    free_gif(o->gif);
    free(o->path);
    o->gif = g;
    o->path = p;
    free(o->name_u8);
    o->name_u8 = to_utf8(overlay_name(o));
    o->has_override = false;
    SetWindowTextW(o->hwnd, overlay_name(o));
    load_frame_cache(o);
    apply_size(o);
    render_surface_present(o->surface, o->gif->bgra);
    reset_timing(o);
    if (o->hidden) gif_stream_set_output_size(o->gif, 1, 1);
    script_notify(o, "on_moved"); /* Size changed. */
}

/* Re-decode from frame 0 so the key also applies to already-composited pixels. */
static void apply_color_key(Overlay *o)
{
    gif_stream_set_color_key(o->gif, o->key_enabled, o->key_rgb, o->key_tolerance);
    if (gif_stream_restart(o->gif) < 0 || gif_stream_next(o->gif) != 1) {
        o->failed = true;
        return;
    }
    load_frame_cache(o);
    render_surface_present(o->surface, o->gif->bgra);
    reset_timing(o);
}

/* Re-decode from frame 0 with the canvas a scale of pct needs. */
static void apply_canvas_for(Overlay *o, int pct)
{
    if (o->hidden) return; /* Rebuilt when shown. */
    SIZE c = canvas_size(o->gif, pct);
    if (c.cx == o->gif->out_w && c.cy == o->gif->out_h) return;
    if (gif_stream_set_output_size(o->gif, c.cx, c.cy) != 0) return; /* Keep the old canvas. */
    if (gif_stream_restart(o->gif) < 0 || gif_stream_next(o->gif) != 1 ||
        (o->surface && !render_surface_resize(o->surface, c.cx, c.cy))) {
        o->failed = true;
        return;
    }
    render_surface_set_scale(o->surface, visual_scale(o));
    render_surface_present(o->surface, o->gif->bgra);
    reset_timing(o);
}

/* Colour of the first frame's top-left pixel, ignoring any current key. */
static bool corner_color(const Overlay *o, uint32_t *rgb)
{
    Overlay plain = *o;
    plain.key_enabled = false;
    plain.scale_pct = 100; /* Full-size canvas: pixel (0, 0) is the GIF's own corner. */
    GifStream *g = open_first_frame(&plain, o->path);
    if (!g) return false;
    const uint8_t *px = g->bgra; /* B, G, R, A */
    bool opaque = px[3] != 0;
    *rgb = (uint32_t)px[2] << 16 | (uint32_t)px[1] << 8 | px[0];
    free_gif(g);
    return opaque;
}

typedef struct ScalePreview {
    int id;       /* Not a pointer: the overlay can be removed while the dialog is open. */
    POINT anchor; /* Fixed centre: re-deriving it each step would drift by rounding. */
} ScalePreview;

static void preview_scale(int pct, void *ctx)
{
    ScalePreview *sp = (ScalePreview *)ctx;
    Overlay *o = find_overlay(sp->id);
    if (!o) return;
    o->scale_pct = valid_scale(pct);
    apply_size_at(o, sp->anchor);
}

void overlay_command(Overlay *o, UINT cmd)
{
    switch (cmd) {
    case OVERLAY_CMD_PAUSE:
        o->paused = !o->paused;
        if (!o->paused) {
            o->next_frame = app_now();
            reset_tick(o);
        }
        break;
    case OVERLAY_CMD_REPLACE:
        app_pick_files(PICK_REPLACE, o);
        break;
    case OVERLAY_CMD_TOPMOST:
        o->topmost = !o->topmost;
        apply_ex_style(o);
        break;
    case OVERLAY_CMD_LOCK:
        o->locked = !o->locked;
        break;
    case OVERLAY_CMD_CLICKTHROUGH:
        o->clickthrough = !o->clickthrough;
        apply_ex_style(o);
        if (o->clickthrough) {
            app_notify(TXT_CLICKTHROUGH_ON_TITLE, TXT_CLICKTHROUGH_ON_TEXT);
        }
        break;
    case OVERLAY_CMD_HIDE:
        overlay_set_hidden(o, !o->hidden);
        if (o->hidden) app_notify(TXT_HIDDEN_TITLE, TXT_HIDDEN_TEXT);
        break;
    case OVERLAY_CMD_RECENTER:
        center_on_primary(o, 0);
        script_notify(o, "on_moved");
        break;
    case OVERLAY_CMD_REMOVE:
        app_remove_overlay(o);
        return;
    case OVERLAY_CMD_COLOR_KEY: {
        uint32_t rgb = o->key_rgb;
        int tol = o->key_tolerance;
        int id = o->id;
        if (!o->key_enabled && !corner_color(o, &rgb)) rgb = 0x00FF00;
        bool ok = app_prompt_color_key(&rgb, &tol);
        o = find_overlay(id); /* Removed while the dialog was open? (nested message loop) */
        if (!ok || !o) return;
        o->key_enabled = true;
        o->key_rgb = rgb;
        o->key_tolerance = tol;
        apply_color_key(o);
        break;
    }
    case OVERLAY_CMD_COLOR_KEY_CORNER: {
        uint32_t rgb;
        if (!corner_color(o, &rgb)) {
            app_notify(TXT_NO_KEY_TITLE, TXT_NO_KEY_TEXT);
            return;
        }
        o->key_enabled = true;
        o->key_rgb = rgb;
        apply_color_key(o);
        break;
    }
    case OVERLAY_CMD_SCALE: {
        /* While the dialog is open:
         * - the script is suspended, so it cannot move the window away from the
         *   anchor the preview re-centres on at every step;
         * - the canvas is full size, so the preview shrinks the real image like
         *   the final result instead of stretching a small canvas (which makes
         *   nearest-neighbour columns double up and shimmer as the slider moves).
         * The scaled canvas is rebuilt once when the dialog closes. */
        ScalePreview sp = {o->id, window_center(o)};
        int pct = o->scale_pct;
        o->scaling = true;
        apply_canvas_for(o, 100);
        bool ok = app_prompt_scale(&pct, preview_scale, &sp);
        /* The dialog runs a nested message loop in which the overlay's own menu
         * and the tray menu stay usable: it may have been removed meanwhile. */
        o = find_overlay(sp.id);
        if (!o) return;
        if (ok) preview_scale(pct, &sp); /* On cancel the preview is already reverted. */
        apply_canvas_for(o, o->scale_pct);
        o->scaling = false;
        reset_tick(o);
        moved_externally(o); /* Picks up the final position and size. */
        if (!ok) return;
        break;
    }
    case OVERLAY_CMD_SCRIPT_NONE:
        overlay_set_script(o, NULL);
        break;
    case OVERLAY_CMD_SCRIPT_PATROL:
    case OVERLAY_CMD_SCRIPT_WANDER:
    case OVERLAY_CMD_SCRIPT_BOUNCE:
    case OVERLAY_CMD_SCRIPT_FOLLOW:
    case OVERLAY_CMD_SCRIPT_FLEE:
        overlay_set_script(o, OVERLAY_BUILTINS[cmd - OVERLAY_CMD_SCRIPT_PATROL].spec);
        break;
    case OVERLAY_CMD_SCRIPT_FILE:
        app_pick_files(PICK_SCRIPT, o);
        return;
    case OVERLAY_CMD_SCRIPT_NEW_PATROL:
    case OVERLAY_CMD_SCRIPT_NEW_WANDER:
    case OVERLAY_CMD_SCRIPT_NEW_BOUNCE:
    case OVERLAY_CMD_SCRIPT_NEW_FOLLOW:
    case OVERLAY_CMD_SCRIPT_NEW_FLEE: {
        const Builtin *b = &OVERLAY_BUILTINS[cmd - OVERLAY_CMD_SCRIPT_NEW_PATROL];
        wchar_t name[64];
        _snwprintf_s(name, 64, _TRUNCATE, L"my_%hs", b->chunk);
        app_new_script(o, b->resource, name);
        return; /* Bound when the helper reports the saved path. */
    }
    case OVERLAY_CMD_SCRIPT_RELOAD: {
        wchar_t *spec = o->script_spec ? dup_wcs(o->script_spec) : NULL;
        if (!spec) return;
        overlay_set_script(o, spec);
        free(spec);
        break;
    }
    case OVERLAY_CMD_COLOR_KEY_OFF:
        if (!o->key_enabled) return;
        o->key_enabled = false;
        apply_color_key(o);
        break;
    default:
        return;
    }
    app_changed();
}

void overlay_build_menu(const Overlay *o, HMENU menu, UINT base)
{
    HMENU key = CreatePopupMenu();
    if (key) {
        wchar_t label[64];
        if (o->key_enabled) {
            _snwprintf_s(label, 64, _TRUNCATE, TXT_MENU_KEY_CURRENT, (unsigned)o->key_rgb, o->key_tolerance);
            AppendMenuW(key, MF_STRING | MF_GRAYED, 0, label);
            AppendMenuW(key, MF_SEPARATOR, 0, NULL);
        }
        AppendMenuW(key, MF_STRING, base + OVERLAY_CMD_COLOR_KEY, TXT_MENU_KEY_HEX);
        AppendMenuW(key, MF_STRING, base + OVERLAY_CMD_COLOR_KEY_CORNER, TXT_MENU_KEY_CORNER);
        AppendMenuW(key, MF_STRING | (o->key_enabled ? 0 : MF_GRAYED), base + OVERLAY_CMD_COLOR_KEY_OFF, TXT_MENU_KEY_OFF);
    }
    HMENU motion = CreatePopupMenu();
    if (motion) {
        const wchar_t *spec = o->script_spec ? o->script_spec : L"";
        const Builtin *cur = find_builtin(spec);
        if (o->script_failed) {
            AppendMenuW(motion, MF_STRING | MF_GRAYED, 0, TXT_MENU_SCRIPT_FAILED);
            AppendMenuW(motion, MF_SEPARATOR, 0, NULL);
        }
        AppendMenuW(motion, MF_STRING | (*spec ? 0 : MF_CHECKED), base + OVERLAY_CMD_SCRIPT_NONE, TXT_MENU_SCRIPT_NONE);
        for (int i = 0; i < BUILTIN_COUNT; ++i) {
            AppendMenuW(motion, MF_STRING | (cur == &OVERLAY_BUILTINS[i] ? MF_CHECKED : 0),
                        base + OVERLAY_CMD_SCRIPT_PATROL + (UINT)i, OVERLAY_BUILTINS[i].label);
        }
        AppendMenuW(motion, MF_SEPARATOR, 0, NULL);
        if (*spec && !cur) AppendMenuW(motion, MF_STRING | MF_GRAYED | MF_CHECKED, 0, file_part(spec));
        AppendMenuW(motion, MF_STRING, base + OVERLAY_CMD_SCRIPT_FILE, TXT_MENU_SCRIPT_FILE);
        HMENU fresh = CreatePopupMenu(); /* Freed with its parent (DestroyMenu is recursive). */
        if (fresh) {
            for (int i = 0; i < BUILTIN_COUNT; ++i) {
                AppendMenuW(fresh, MF_STRING, base + OVERLAY_CMD_SCRIPT_NEW_PATROL + (UINT)i,
                            OVERLAY_BUILTINS[i].label);
            }
            AppendMenuW(motion, MF_POPUP, (UINT_PTR)fresh, TXT_MENU_SCRIPT_NEW);
        }
        AppendMenuW(motion, MF_STRING | (*spec ? 0 : MF_GRAYED), base + OVERLAY_CMD_SCRIPT_RELOAD, TXT_MENU_SCRIPT_RELOAD);
    }
    UINT chk;
    AppendMenuW(menu, MF_STRING | (o->failed || o->is_static ? MF_GRAYED : 0),
                base + OVERLAY_CMD_PAUSE, o->paused ? TXT_MENU_RESUME : TXT_MENU_PAUSE);
    AppendMenuW(menu, MF_STRING, base + OVERLAY_CMD_REPLACE, TXT_MENU_REPLACE);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    {
        wchar_t label[32];
        _snwprintf_s(label, 32, _TRUNCATE, TXT_MENU_SCALE, o->scale_pct);
        AppendMenuW(menu, MF_STRING | (o->hidden ? MF_GRAYED : 0), base + OVERLAY_CMD_SCALE, label);
    }
    if (key) AppendMenuW(menu, MF_POPUP | (o->key_enabled ? MF_CHECKED : 0), (UINT_PTR)key, TXT_MENU_COLOR_KEY);
    if (motion) {
        bool on = o->script_spec && *o->script_spec;
        AppendMenuW(menu, MF_POPUP | (on ? MF_CHECKED : 0), (UINT_PTR)motion, TXT_MENU_SCRIPT);
    }
    chk = o->topmost ? MF_CHECKED : 0;
    AppendMenuW(menu, MF_STRING | chk, base + OVERLAY_CMD_TOPMOST, TXT_MENU_TOPMOST);
    chk = o->locked ? MF_CHECKED : 0;
    AppendMenuW(menu, MF_STRING | chk, base + OVERLAY_CMD_LOCK, TXT_MENU_LOCK);
    chk = o->clickthrough ? MF_CHECKED : 0;
    AppendMenuW(menu, MF_STRING | chk, base + OVERLAY_CMD_CLICKTHROUGH, TXT_MENU_CLICKTHROUGH);
    AppendMenuW(menu, MF_STRING, base + OVERLAY_CMD_RECENTER, TXT_MENU_RECENTER);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, base + OVERLAY_CMD_HIDE, o->hidden ? TXT_MENU_SHOW : TXT_MENU_HIDE);
    AppendMenuW(menu, MF_STRING, base + OVERLAY_CMD_REMOVE, TXT_MENU_REMOVE);
}

void overlay_to_config(const Overlay *o, OverlayConfig *out)
{
    RECT r;
    GetWindowRect(o->hwnd, &r);
    out->path = o->path;
    out->x = r.left;
    out->y = r.top;
    out->scale_pct = o->scale_pct;
    out->paused = o->paused;
    out->topmost = o->topmost;
    out->locked = o->locked;
    out->clickthrough = o->clickthrough;
    out->hidden = o->hidden;
    out->key_enabled = o->key_enabled;
    out->key_rgb = o->key_rgb;
    out->key_tolerance = o->key_tolerance;
    out->script = o->script_spec ? o->script_spec : L"";
}
