#include "overlay.h"
#include "group.h"
#include "app.h"
#include "applog.h"
#include "clip_path.h"
#include "contacts.h"
#include "cursor_hook.h"
#include "i18n.h"
#include "log.h"
#include "monitors.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OVERLAY_CLASS L"YAGP.Overlay"

#define DEFAULT_KEY_TOLERANCE 16
#define MAX_WINDOW_SIDE 4096
#define POLL_HZ 30            /* Cursor / click-through fallback when the hook is unavailable. */

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

static LONG round_px(double v)
{
    return (LONG)(v < 0 ? v - 0.5 : v + 0.5);
}

static double app_seconds(void)
{
    return (double)app_now() / (double)g_app.qpc_freq;
}

static Clip *cur_clip(const Overlay *o)
{
    return (Clip *)&o->clips[o->play.clip];
}

static GifStream *cur_gif(const Overlay *o)
{
    return o->clips[o->play.clip].gif;
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

/* The layout box: the current clip at the overlay's scale, untransformed.
 * yagp.pos / size / set_pos work with it. */
static SIZE scaled_size(const Overlay *o)
{
    return size_at(cur_gif(o), o->scale_pct);
}

const wchar_t *overlay_name(const Overlay *o)
{
    const wchar_t *slash = wcsrchr(o->path, L'\\');
    const wchar_t *fwd = wcsrchr(o->path, L'/');
    if (fwd > slash) slash = fwd;
    return slash ? slash + 1 : o->path;
}

static bool effective_clickthrough(const Overlay *o)
{
    return o->ct_override >= 0 ? o->ct_override != 0 : o->clickthrough;
}

static bool overlay_effective_hidden(const Overlay *o)
{
    return o->hidden || (o->group && o->group->hidden);
}

static bool script_live(const Overlay *o)
{
    return o->script && !o->leader && !overlay_effective_hidden(o) && !o->paused && !o->scaling;
}

bool overlay_is_animating(const Overlay *o)
{
    return o->surface && !overlay_effective_hidden(o) && !o->paused && !o->failed && !o->is_static &&
           o->play.playing && !o->play.held;
}

static ClipKey overlay_key(const Overlay *o)
{
    ClipKey k = {o->key_enabled, o->key_rgb, o->key_tolerance};
    return k;
}

static int find_clip(const Overlay *o, const char *name)
{
    for (int i = 0; i < o->clip_count; ++i) {
        if (strcmp(o->clips[i].name, name) == 0) return i;
    }
    return -1;
}

static void apply_ex_style(Overlay *o)
{
    LONG_PTR ex = GetWindowLongPtrW(o->hwnd, GWL_EXSTYLE);
    ex &= ~(LONG_PTR)(WS_EX_LAYERED | WS_EX_TRANSPARENT);
    bool ct = effective_clickthrough(o);
    /* Cross-process click-through needs both bits; LWA_ALPHA=255 leaves the
     * DirectComposition content untouched. */
    if (ct) ex |= WS_EX_LAYERED | WS_EX_TRANSPARENT;
    SetWindowLongPtrW(o->hwnd, GWL_EXSTYLE, ex);
    if (ct) SetLayeredWindowAttributes(o->hwnd, 0, 255, LWA_ALPHA);
    SetWindowPos(o->hwnd, o->topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    ifsm_reset(&o->input); /* Input now arrives by the other route. */
}

/* ---- geometry ---------------------------------------------------------------
 * pos_x/pos_y and scaled_size() are the layout box. The script transform maps
 * it (window pixels relative to the box's top-left) somewhere else; the window
 * is the bounding box of the result (of both ends while an animation runs),
 * at aabb[] relative to the layout box, and the visual's matrix is
 *   canvas -> layout box (scale, flip) -> script transform -> window. */

static void current_xform(const Overlay *o, Xform *out)
{
    if (o->xf_animating) xform_anim_eval(&o->xf, app_seconds(), out);
    else *out = o->xf.to;
}

static void compute_aabb(const Overlay *o, int out[4])
{
    SIZE s = scaled_size(o);
    if (!o->xf_animating && xform_is_identity(&o->xf.to)) {
        out[0] = out[1] = 0;
        out[2] = s.cx;
        out[3] = s.cy;
        return;
    }
    xform_aabb(&o->xf.to, s.cx, s.cy, out);
    if (o->xf_animating) {
        /* Elements interpolate linearly (along the easing), so every corner
         * stays on the segment between its two ends: the union bounds it all. */
        int a[4];
        xform_aabb(&o->xf.from, s.cx, s.cy, a);
        if (a[0] < out[0]) out[0] = a[0];
        if (a[1] < out[1]) out[1] = a[1];
        if (a[2] > out[2]) out[2] = a[2];
        if (a[3] > out[3]) out[3] = a[3];
    }
}

/* canvas -> layout box. */
static Xform base_xform(const Overlay *o)
{
    SIZE s = scaled_size(o);
    const GifStream *g = cur_gif(o);
    double kx = (double)s.cx / g->out_w, ky = (double)s.cy / g->out_h;
    Xform b = {{o->flip_x ? -kx : kx, 0, 0, ky, o->flip_x ? s.cx : 0, 0}};
    return b;
}

static void composite(const Overlay *o, const Xform *script, Xform *out)
{
    Xform b = base_xform(o), t = {{1, 0, 0, 1, -o->aabb[0], -o->aabb[1]}};
    xform_mul(&b, script, out);
    xform_mul(out, &t, out);
}

/* Hands the visual its matrix: the final one, or the rest of a running animation. */
static void push_matrix(Overlay *o)
{
    if (!o->surface) return;
    if (o->xf_animating) {
        double now = app_seconds();
        xform_anim_rebase(&o->xf, now);
        if (!xform_anim_done(&o->xf, now)) {
            Xform from, to;
            composite(o, &o->xf.from, &from);
            composite(o, &o->xf.to, &to);
            render_surface_set_matrix(o->surface, to.m, from.m, o->xf.duration, o->xf.c);
            return;
        }
    }
    Xform m;
    composite(o, &o->xf.to, &m);
    render_surface_set_matrix(o->surface, m.m, NULL, 0, NULL);
}

/* Moves / resizes the window to the layout box plus its transform bounds and
 * updates the visual. lean: only the position may have changed. */
static void place_window(Overlay *o)
{
    int ix = round_px(o->pos_x) + o->aabb[0], iy = round_px(o->pos_y) + o->aabb[1];
    int w = o->aabb[2] - o->aabb[0], h = o->aabb[3] - o->aabb[1];
    RECT r;
    GetWindowRect(o->hwnd, &r);
    if (r.left == ix && r.top == iy && r.right - r.left == w && r.bottom - r.top == h) return;
    SetWindowPos(o->hwnd, NULL, ix, iy, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    o->peer_dirty = true;
}

static void apply_geometry(Overlay *o)
{
    compute_aabb(o, o->aabb);
    place_window(o);
    push_matrix(o);
}

/* ---- surface and presenting --------------------------------------------- */

bool overlay_attach_surface(Overlay *o)
{
    if (o->surface || overlay_effective_hidden(o)) return true; /* A hidden GIF gets its swap chain when shown. */
    const GifStream *g = cur_gif(o);
    o->surface = render_surface_create(g_app.device, o->hwnd, g->out_w, g->out_h);
    if (!o->surface) return false;
    push_matrix(o);
    if (o->opacity < 1.0) render_surface_set_opacity(o->surface, o->opacity, 1.0, 0, NULL);
    return render_surface_present(o->surface, g->bgra);
}

void overlay_detach_surface(Overlay *o)
{
    render_surface_destroy(o->surface);
    o->surface = NULL;
}

static void present_full(Overlay *o)
{
    const GifStream *g = cur_gif(o);
    if (o->surface) render_surface_present(o->surface, g->bgra);
}

/* Present the frame just decoded. index is its 0-based frame number, which
 * selects its precomputed change box; frame 0's box is the loop wrap
 * (last -> first), so it is only valid after the last frame. */
static void present_frame(Overlay *o, int index, bool sequential)
{
    const Clip *c = cur_clip(o);
    const GifStream *g = c->gif;
    if (!o->surface) return;
    if (sequential && c->dirty && index < c->frame_count) {
        /* Boxes are in GIF pixels; a change no canvas pixel samples maps to an
         * empty box, and the present is skipped like for an identical frame. */
        const uint16_t *d = c->dirty[index];
        int box[4];
        gif_stream_map_box(g, d[0], d[1], d[2], d[3], box);
        RECT rc = {box[0], box[1], box[2], box[3]};
        if (box[0] >= box[2] || box[1] >= box[3]) SetRectEmpty(&rc);
        render_surface_present_dirty(o->surface, g->bgra, &rc);
    } else {
        render_surface_present(o->surface, g->bgra);
    }
}

/* ---- playback ------------------------------------------------------------- */

static int64_t delay_ticks(const Overlay *o)
{
    double ms = cur_gif(o)->delay_ms / o->play.speed;
    if (ms < 10) ms = 10; /* 10 ms floor, like every browser: no spin at speed 10. */
    return (int64_t)(ms * (double)g_app.qpc_freq / 1000.0);
}

static void free_snapshot(Playback *p)
{
    if (p->has_snapshot) gif_stream_snapshot_free(&p->range_start);
    p->has_snapshot = false;
}

/* Frame count if known (cache ready). */
static int frame_count(const Overlay *o)
{
    return cur_clip(o)->frame_count;
}

static int range_end(const Overlay *o)
{
    int n = frame_count(o);
    if (o->play.to >= 0) return n > 0 && o->play.to >= n ? n - 1 : o->play.to;
    return n > 0 ? n - 1 : -1;
}

/* Decodes frames until `target` (0-based) is on the canvas, from wherever is
 * cheapest: the current position if it is before target, else the nearest
 * keyframe from the index, else frame 0. Nothing is presented. */
static bool decode_to(Overlay *o, int target)
{
    Clip *c = cur_clip(o);
    GifStream *g = c->gif;
    int at = o->play.frame;
    int start = -1;
    if (c->frames && target < c->frame_count) {
        int k = target;
        while (k > 0 && !(c->frames[k].flags & FRAME_KEYFRAME)) --k;
        if (k > at || at > target) start = k;
    } else if (at > target) {
        start = 0;
    }
    if (start == 0 && !c->frames) {
        if (gif_stream_restart(g) != 0) return false;
        at = -1;
    } else if (start >= 0) {
        if (gif_stream_seek(g, c->frames[start].offset) != 0) return false;
        at = start - 1;
    }
    while (at < target) {
        if (gif_stream_next(g) != 1) {
            /* Past the real end (count unknown until the cache is ready): the
             * stream is at its end now, so decode again up to the last frame. */
            if (at < 0 || gif_stream_restart(g) != 0) return false;
            for (int i = 0; i <= at; ++i) {
                if (gif_stream_next(g) != 1) return false;
            }
            o->play.frame = at;
            return false;
        }
        ++at;
    }
    o->play.frame = at;
    return true;
}

static void deliver_clip_event(Overlay *o, ScriptEvent ev, int frame);

/* Next frame of the current clip, honouring range, loops and speed. */
static void advance(Overlay *o, int64_t now)
{
    Playback *p = &o->play;
    Clip *c = cur_clip(o);
    GifStream *g = c->gif;
    int end = range_end(o);
    int next = p->frame + 1;
    bool wrap = end >= 0 && p->frame >= end;
    bool sequential = true;
    if (!wrap) {
        int st = gif_stream_next(g);
        /* Many GIFs have truncated trailers; after a full pass treat errors as end-of-loop. */
        if (st == 1) {
            p->frame = next;
        } else if (st == 0 || p->frame > 0) {
            wrap = true;
        } else {
            o->failed = true;
            return;
        }
    }
    if (wrap) {
        if (p->frame == 0 && p->from <= 0 && p->loops_left < 0) {
            o->is_static = true; /* Single frame, looping: already on screen, nothing to schedule. */
            return;
        }
        if (p->loops_left > 0 && --p->loops_left == 0) {
            p->held = true; /* Hold the last frame. */
            deliver_clip_event(o, SEV_CLIP_END, p->frame);
            return;
        }
        int from = p->from > 0 ? p->from : 0;
        bool ok;
        if (from == 0) {
            ok = gif_stream_restart(g) == 0 && gif_stream_next(g) == 1;
            /* Frame 0's change box is the wrap from the last frame: only right
             * after a full pass to the real end. */
            sequential = c->frame_count > 0 && p->frame == c->frame_count - 1;
            p->frame = 0;
        } else if (p->has_snapshot) {
            ok = gif_stream_snapshot_restore(g, &p->range_start) == 0;
            p->frame = from;
            sequential = false;
        } else {
            ok = decode_to(o, from);
            sequential = false;
            if (ok && gif_stream_snapshot_save(g, &p->range_start) == 0) p->has_snapshot = true;
        }
        if (!ok) {
            o->failed = true;
            return;
        }
    }
    present_frame(o, p->frame, sequential);
    int64_t delay = delay_ticks(o);
    o->next_frame += delay; /* Accumulate to avoid drift... */
    if (o->next_frame <= now) o->next_frame = now + delay; /* ...but never burst to catch up. */
    /* Last: a handler may switch clips (yagp.play), which reschedules itself. */
    if (o->subs & SEV_BIT(SEV_FRAME)) deliver_clip_event(o, SEV_FRAME, p->frame);
}

static void reset_timing(Overlay *o)
{
    o->failed = false;
    o->is_static = o->still_image;
    o->next_frame = o->still_image ? INT64_MAX : app_now() + delay_ticks(o);
}

/* Shows clip index ci from frame `from` (0-based), resizing the layout box
 * around the anchor. */
static bool switch_clip(Overlay *o, int ci, int anchor)
{
    Playback *p = &o->play;
    if (ci == p->clip) return true;
    SIZE before = scaled_size(o);
    Clip *old = cur_clip(o), *c = &o->clips[ci];
    if (!clip_activate(c, o->scale_pct)) return false;
    if (o->surface && !render_surface_resize(o->surface, c->gif->out_w, c->gif->out_h)) {
        clip_park(c);
        return false;
    }
    free_snapshot(p);
    clip_park(old);
    p->clip = ci;
    p->frame = 0;
    SIZE after = scaled_size(o);
    if (anchor == SANCHOR_BOTTOM) {
        o->pos_x += (before.cx - after.cx) / 2.0;
        o->pos_y += before.cy - after.cy;
    } else if (anchor == SANCHOR_CENTER) {
        o->pos_x += (before.cx - after.cx) / 2.0;
        o->pos_y += (before.cy - after.cy) / 2.0;
    }
    apply_geometry(o);
    return true;
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
    {L"builtin:look", L"LOOK", "look.lua", TXT_PRESET_LOOK},
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

/* UTF-8 -> wide, truncated to cap-1 characters. False when unusable. */
static bool to_wide(const char *u8, wchar_t *out, size_t cap)
{
    if (!out || cap == 0) return false;
    out[0] = 0;
    if (!u8 || !*u8) return true;
    wchar_t tmp[1024];
    int n = MultiByteToWideChar(CP_UTF8, 0, u8, -1, tmp, ARRAYSIZE(tmp));
    if (n <= 0) return false;
    wcsncpy_s(out, cap, tmp, _TRUNCATE);
    return true;
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

/* Hitbox inside the layout box: out = dx, dy, w, h in display pixels (scale,
 * flip and the script transform applied; a transformed box is its bounding box). */
static void overlay_hitbox(const Overlay *o, int out[4])
{
    SIZE s = scaled_size(o);
    const Clip *c = cur_clip(o);
    hitbox_in_window(o->has_override ? o->hitbox_override : c->opaque, c->gif->width, c->gif->height,
                     s.cx, s.cy, o->flip_x, out);
    if (!o->xf_animating && xform_is_identity(&o->xf.to)) return;
    Xform x, shift;
    current_xform(o, &x);
    Xform at = {{1, 0, 0, 1, out[0], out[1]}};
    xform_mul(&at, &x, &shift);
    int bb[4];
    xform_aabb(&shift, out[2], out[3], bb);
    out[0] = bb[0];
    out[1] = bb[1];
    out[2] = bb[2] - bb[0];
    out[3] = bb[3] - bb[1];
}

/* Peer snapshot, rebuilt at most once per change: yagp.others() from N
 * scripts in one tick fills it once. */
static ScriptPeer s_peer_cache[SCRIPT_MAX_PEERS + 1];
static const Overlay *s_peer_owner[SCRIPT_MAX_PEERS + 1];
static int s_peer_n = -1; /* -1 = stale. */
static bool s_peers_dirty;

static void peers_changed(void)
{
    s_peer_n = -1;
    s_peers_dirty = true;
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
    if (x != o->pos_x || y != o->pos_y) {
        o->peer_dirty = true;
        peers_changed();
    }
    o->pos_x = x;
    o->pos_y = y;
    int ix = round_px(x) + o->aabb[0], iy = round_px(y) + o->aabb[1];
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
    if (o->flip_x == flip_x) return;
    o->flip_x = flip_x;
    push_matrix(o);
}

static void host_set_rate(void *ctx, int hz)
{
    Overlay *o = (Overlay *)ctx;
    o->tick_interval = g_app.qpc_freq / hz;
}

/* One cursor sample per millisecond, shared by every script: N overlays
 * polling yagp.mouse() cost one GetCursorPos, not N. GetAsyncKeyState reports
 * physical buttons, so a left-handed swap is undone to give the logical ones. */
static bool host_get_mouse(void *ctx, ScriptMouse *m)
{
    (void)ctx;
    static int64_t at;
    static bool sampled; /* No sentinel time: now - INT64_MIN would overflow. */
    static ScriptMouse cached;
    static bool cached_ok;
    int64_t now = app_now();
    if (!sampled || now - at >= g_app.qpc_freq / 1000) {
        sampled = true;
        at = now;
        POINT p;
        cached_ok = GetCursorPos(&p) != 0; /* Fails on the secure desktop. */
        if (cached_ok) {
            bool swap = GetSystemMetrics(SM_SWAPBUTTON) != 0;
            bool phys_left = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            bool phys_right = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
            cached.x = p.x;
            cached.y = p.y;
            cached.left = swap ? phys_right : phys_left;
            cached.right = swap ? phys_left : phys_right;
            cached.middle = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
        }
    }
    if (cached_ok) *m = cached;
    return cached_ok;
}

/* Layout box as other scripts see it. A window being dragged has not updated
 * pos_x/pos_y yet, so read it back from the window. */
static void peer_rect(const Overlay *o, double *x, double *y)
{
    if (o->dragging) {
        RECT r;
        GetWindowRect(o->hwnd, &r);
        *x = r.left - o->aabb[0];
        *y = r.top - o->aabb[1];
    } else {
        *x = o->pos_x;
        *y = o->pos_y;
    }
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

/* box = x, y, w, h in GIF pixels of the current clip, clipped to it; NULL = automatic. */
static void host_set_hitbox(void *ctx, const int *box)
{
    Overlay *o = (Overlay *)ctx;
    const GifStream *g = cur_gif(o);
    o->has_override = false;
    o->peer_dirty = true;
    peers_changed();
    if (!box) return;
    int l = box[0], t = box[1], r = box[0] + box[2], b = box[1] + box[3];
    if (r > g->width) r = g->width;
    if (b > g->height) b = g->height;
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
    p->still = o->still_image;
    p->follower = o->leader != NULL;
    p->leader = overlay_has_followers(o);
    p->leader_id = o->leader ? o->leader->id : 0;
    p->name = o->name_u8 ? o->name_u8 : "";
}

static int host_get_peers(void *ctx, ScriptPeer *out, int cap)
{
    if (s_peer_n < 0) {
        s_peer_n = 0;
        for (Overlay *o = g_app.overlays; o && s_peer_n < SCRIPT_MAX_PEERS + 1; o = o->next) {
            if (overlay_effective_hidden(o)) continue;
            s_peer_owner[s_peer_n] = o;
            fill_peer(o, &s_peer_cache[s_peer_n++]);
        }
    }
    int n = 0;
    for (int i = 0; i < s_peer_n && n < cap; ++i) {
        if (s_peer_owner[i] != (const Overlay *)ctx) out[n++] = s_peer_cache[i];
    }
    return n;
}

static const ScriptHostOps HOST_OPS = {host_get_rect, host_set_pos, host_set_flip, host_set_rate,
                                       host_get_mouse, host_get_peers, host_get_hitbox, host_set_hitbox};

static Overlay *find_overlay(int id);

static int follower_list(void *ctx, int *ids, int cap)
{
    Overlay *leader = (Overlay *)ctx;
    int n = 0;
    for (Overlay *o = g_app.overlays; o && n < cap; o = o->next) {
        if (o->leader == leader) ids[n++] = o->id;
    }
    return n;
}

static void *follower_resolve(void *ctx, int id)
{
    Overlay *o = find_overlay(id);
    return o && o->leader == (Overlay *)ctx ? o : NULL;
}

static const char *follower_name(void *ctx, int id)
{
    Overlay *o = (Overlay *)follower_resolve(ctx, id);
    return o ? (o->name_u8 ? o->name_u8 : "") : NULL;
}

static bool follower_hidden(void *target) { return ((Overlay *)target)->hidden; }
static bool follower_paused(void *target) { return ((Overlay *)target)->paused; }
static bool follower_static(void *target) { return ((Overlay *)target)->still_image; }

static const ScriptFollowerOps FOLLOWER_OPS = {follower_list, follower_resolve, follower_name,
                                         follower_hidden, follower_paused, follower_static};

/* ---- extended host ops: clips, playback, composition, input behaviour ---- */

static void reset_tick(Overlay *o);

static bool xhost_load_clip(void *ctx, const char *name, const char *rel, char *err, size_t cap)
{
    Overlay *o = (Overlay *)ctx;
    wchar_t path[MAX_PATH * 4];
    if (!clip_resolve_path(o->path, rel, path, MAX_PATH * 4)) {
        strncpy_s(err, cap, TXT_U8_BAD_CLIP_PATH, _TRUNCATE);
        return false;
    }
    int at = find_clip(o, name);
    if (at < 0 && o->clip_count >= OVERLAY_MAX_CLIPS) {
        _snprintf_s(err, cap, _TRUNCATE, TXT_U8_TOO_MANY_CLIPS, OVERLAY_MAX_CLIPS);
        return false;
    }
    Clip c;
    ClipKey key = overlay_key(o);
    if (!clip_open(&c, name, path, &key, o->scale_pct, true)) {
        strncpy_s(err, cap, TXT_U8_CLIP_OPEN, _TRUNCATE);
        return false;
    }
    if (c.gif->is_still) {
        clip_close(&c);
        strncpy_s(err, cap, TXT_U8_STATIC_NO_ANIM, _TRUNCATE);
        return false;
    }
    if (at < 0) {
        o->clips[o->clip_count++] = c;
        return true;
    }
    /* Replacing a clip: if it is on screen, move to the new one in place. */
    bool showing = at == o->play.clip;
    if (showing) {
        if (!clip_activate(&c, o->scale_pct) ||
            (o->surface && !render_surface_resize(o->surface, c.gif->out_w, c.gif->out_h))) {
            clip_close(&c);
            strncpy_s(err, cap, TXT_U8_CLIP_OPEN, _TRUNCATE);
            return false;
        }
        free_snapshot(&o->play);
    }
    clip_close(&o->clips[at]);
    o->clips[at] = c;
    if (showing) {
        o->play.frame = 0;
        apply_geometry(o);
        present_full(o);
        reset_timing(o);
    }
    return true;
}

static void xhost_unload_clip(void *ctx, const char *name)
{
    Overlay *o = (Overlay *)ctx;
    int at = find_clip(o, name);
    if (at <= 0) return; /* "main" stays. */
    if (at == o->play.clip) {
        switch_clip(o, 0, SANCHOR_BOTTOM);
        present_full(o);
        reset_timing(o);
    }
    clip_close(&o->clips[at]);
    memmove(&o->clips[at], &o->clips[at + 1], (size_t)(o->clip_count - at - 1) * sizeof(Clip));
    --o->clip_count;
    memset(&o->clips[o->clip_count], 0, sizeof(Clip));
    if (o->play.clip > at) --o->play.clip;
}

static bool xhost_clip_info(void *ctx, const char *name, ScriptClipInfo *out)
{
    Overlay *o = (Overlay *)ctx;
    int at = find_clip(o, name);
    if (at < 0) return false;
    const Clip *c = &o->clips[at];
    out->w = c->gif->width;
    out->h = c->gif->height;
    out->frames = c->frame_count;
    out->delays = c->delays;
    double total = 0;
    for (int i = 0; c->delays && i < c->frame_count; ++i) total += c->delays[i];
    out->duration = total / 1000.0;
    return true;
}

static bool xhost_play(void *ctx, const char *name, const ScriptPlay *sp, char *err, size_t cap)
{
    Overlay *o = (Overlay *)ctx;
    int ci = find_clip(o, name);
    if (ci < 0) {
        _snprintf_s(err, cap, _TRUNCATE, TXT_U8_NO_SUCH_CLIP, name);
        return false;
    }
    Playback *p = &o->play;
    /* sync: same fraction of the range on the new clip (direction sets). */
    double frac = -1;
    if (sp->sync) {
        int lo = p->from > 0 ? p->from : 0, hi = range_end(o);
        if (hi > lo) frac = (double)(p->frame - lo) / (double)(hi - lo + 1);
        else if (hi == lo) frac = 0;
    }
    bool same = ci == p->clip;
    if (same && !sp->restart) {
        free_snapshot(p);
        p->from = sp->from;
        p->to = sp->to;
        p->loops_left = sp->loops;
        p->speed = sp->speed;
        p->held = false;
        o->is_static = false;
        return true;
    }
    if (!switch_clip(o, ci, sp->anchor)) {
        strncpy_s(err, cap, TXT_U8_CLIP_OPEN, _TRUNCATE);
        return false;
    }
    free_snapshot(p);
    p->from = sp->from;
    p->to = sp->to;
    p->loops_left = sp->loops;
    p->speed = sp->speed;
    p->held = false;
    p->playing = true;
    int start = sp->from > 0 ? sp->from : 0;
    if (frac >= 0) {
        int hi = range_end(o);
        if (hi >= start) start += (int)(frac * (hi - start + 1));
    }
    /* switch_clip left the new clip on frame 0, decoded. */
    p->frame = 0;
    if (same) {
        if (gif_stream_restart(cur_gif(o)) != 0 || gif_stream_next(cur_gif(o)) != 1) {
            o->failed = true;
            return true;
        }
    }
    if (start > 0 && !decode_to(o, start)) {
        /* Past the real end (count unknown yet): stays on its last frame. */
    }
    if (p->from > 0 && p->frame == p->from && gif_stream_snapshot_save(cur_gif(o), &p->range_start) == 0)
        p->has_snapshot = true;
    present_full(o);
    reset_timing(o);
    app_reschedule();
    return true;
}

static bool xhost_seek(void *ctx, int frame)
{
    Overlay *o = (Overlay *)ctx;
    int n = frame_count(o);
    if (n > 0 && frame >= n) return false;
    if (!decode_to(o, frame)) return false;
    o->play.held = false;
    o->is_static = false;
    present_full(o);
    reset_timing(o);
    app_reschedule();
    return true;
}

static void xhost_get_frame(void *ctx, const char **name, int *frame, int *frames)
{
    Overlay *o = (Overlay *)ctx;
    *name = cur_clip(o)->name;
    *frame = o->play.frame;
    *frames = frame_count(o);
}

static void xhost_set_speed(void *ctx, double speed)
{
    Overlay *o = (Overlay *)ctx;
    o->play.speed = speed;
}

static void xhost_set_playing(void *ctx, bool playing)
{
    Overlay *o = (Overlay *)ctx;
    if (o->play.playing == playing) return;
    o->play.playing = playing;
    if (playing) {
        o->next_frame = app_now() + delay_ticks(o);
        app_reschedule();
    }
}

static bool xhost_set_matrix(void *ctx, const double *m, double duration, int easing)
{
    Overlay *o = (Overlay *)ctx;
    Xform to = m ? *(const Xform *)m : XFORM_IDENTITY, from;
    current_xform(o, &from);
    SIZE s = scaled_size(o);
    int bb[4];
    xform_aabb(&to, s.cx, s.cy, bb);
    if (bb[2] - bb[0] > MAX_WINDOW_SIDE || bb[3] - bb[1] > MAX_WINDOW_SIDE) return false;
    double now = app_seconds();
    xform_anim_start(&o->xf, &from, &to, now, duration, easing);
    o->xf_animating = duration > 0;
    o->xf_end = o->xf_animating ? app_now() + (int64_t)(duration * (double)g_app.qpc_freq) : INT64_MAX;
    apply_geometry(o);
    o->peer_dirty = true;
    peers_changed();
    if (o->xf_animating) app_reschedule();
    return true;
}

static double opacity_now(const Overlay *o)
{
    if (o->op_dur <= 0) return o->op_to;
    double u = (app_seconds() - o->op_t0) / o->op_dur;
    return o->op_from + (o->op_to - o->op_from) * xform_ease(o->op_easing, u);
}

static void xhost_set_opacity(void *ctx, double a, double duration, int easing)
{
    Overlay *o = (Overlay *)ctx;
    double from = opacity_now(o);
    o->op_from = from;
    o->op_to = a;
    o->op_t0 = app_seconds();
    o->op_dur = duration;
    o->op_easing = easing;
    o->opacity = a;
    double c[4];
    xform_ease_cubic(easing, c);
    render_surface_set_opacity(o->surface, a, from, duration, c);
}

static void xhost_set_cursor(void *ctx, int shape)
{
    ((Overlay *)ctx)->cursor_shape = shape;
}

static void xhost_set_draggable(void *ctx, bool draggable)
{
    ((Overlay *)ctx)->draggable = draggable;
}

static void xhost_set_clickthrough(void *ctx, bool ct)
{
    Overlay *o = (Overlay *)ctx;
    int want = ct ? 1 : 0;
    if (o->ct_override == want) return;
    bool before = effective_clickthrough(o);
    o->ct_override = want;
    if (effective_clickthrough(o) != before) {
        apply_ex_style(o);
        overlay_refresh_hook();
    }
}

static void xhost_subs_changed(void *ctx, unsigned mask)
{
    Overlay *o = (Overlay *)ctx;
    unsigned hook_bits = SEV_BIT(SEV_CURSOR) | SEV_INPUT_MASK;
    bool changed = (o->subs & hook_bits) != (mask & hook_bits);
    o->subs = mask;
    if (changed) overlay_refresh_hook(); /* Only bookkeeping: no call back into the script. */
}

static bool xhost_notify(void *ctx, const char *title, const char *text)
{
    (void)ctx;
    wchar_t wt[64], wm[256];
    if (!to_wide(title, wt, ARRAYSIZE(wt))) wt[0] = 0;
    if (!to_wide(text, wm, ARRAYSIZE(wm))) wm[0] = 0;
    return app_notify(wt, wm);
}

static double xhost_now(void *ctx)
{
    (void)ctx;
    return app_seconds();
}

static bool xhost_is_static(void *ctx)
{
    return ((Overlay *)ctx)->still_image;
}

static void xhost_set_hidden(void *ctx, bool hidden)
{
    overlay_set_hidden((Overlay *)ctx, hidden);
    app_changed();
}

static void xhost_set_paused(void *ctx, bool paused)
{
    Overlay *o = (Overlay *)ctx;
    if (o->still_image || o->paused == paused) return;
    o->paused = paused;
    if (!paused) o->next_frame = app_now();
    reset_tick(o);
    overlay_refresh_hook();
    app_changed();
}

static const ScriptHostExtOps HOST_EXT = {
    xhost_load_clip, xhost_unload_clip, xhost_clip_info, xhost_play, xhost_seek, xhost_get_frame,
    xhost_set_speed, xhost_set_playing, xhost_set_matrix, xhost_set_opacity, xhost_set_cursor,
    xhost_set_draggable, xhost_set_clickthrough, xhost_subs_changed, xhost_now, xhost_notify,
    xhost_is_static, xhost_set_hidden, xhost_set_paused, &FOLLOWER_OPS,
};

/* ---- script lifecycle and calls -------------------------------------------- */

static void sync_pos(Overlay *o)
{
    RECT r;
    GetWindowRect(o->hwnd, &r);
    o->pos_x = r.left - o->aabb[0];
    o->pos_y = r.top - o->aabb[1];
    o->win_x = r.left;
    o->win_y = r.top;
    o->peer_dirty = true;
    peers_changed();
}

/* Back to "main", no transform, default input behaviour: what a fresh script
 * (or no script) starts from. */
static void reset_script_state(Overlay *o)
{
    bool switched = o->play.clip != 0;
    for (int i = o->clip_count - 1; i >= 1; --i) {
        if (i == o->play.clip) switch_clip(o, 0, SANCHOR_BOTTOM);
        clip_close(&o->clips[i]);
    }
    if (o->clip_count > 1) o->clip_count = 1;
    if (o->play.clip != 0) switch_clip(o, 0, SANCHOR_BOTTOM);
    Playback *p = &o->play;
    free_snapshot(p);
    bool restart = switched || p->from > 0 || p->to >= 0 || p->held || p->speed != 1.0 || !p->playing;
    p->from = p->to = -1;
    p->loops_left = -1;
    p->speed = 1.0;
    p->held = false;
    p->playing = true;
    if (restart && !overlay_effective_hidden(o)) {
        GifStream *g = cur_gif(o);
        if (gif_stream_restart(g) == 0 && gif_stream_next(g) == 1) {
            p->frame = 0;
            present_full(o);
            reset_timing(o);
        }
    }
    bool had_xf = o->xf_animating || !xform_is_identity(&o->xf.to);
    xform_anim_start(&o->xf, &XFORM_IDENTITY, &XFORM_IDENTITY, 0, 0, SEASE_LINEAR);
    o->xf_animating = false;
    o->xf_end = INT64_MAX;
    if (had_xf) apply_geometry(o);
    if (o->opacity != 1.0 || o->op_dur > 0) {
        o->opacity = o->op_from = o->op_to = 1.0;
        o->op_dur = 0;
        render_surface_set_opacity(o->surface, 1.0, 1.0, 0, NULL);
    }
    o->draggable = true;
    o->cursor_shape = SCURSOR_MOVE;
    if (o->ct_override >= 0) {
        o->ct_override = -1;
        apply_ex_style(o);
    }
    o->has_override = false;
    if (o->flip_x) host_set_flip(o, false);
    o->next_timer = INT64_MAX;
}

/* Stops the script (window stays where it is) and tells the user why. */
static void script_fail(Overlay *o, const wchar_t *err)
{
    script_destroy(o->script);
    o->script = NULL;
    o->subs = 0;
    o->script_failed = true;
    o->next_timer = INT64_MAX;
    peers_changed();
    overlay_refresh_hook();
    wchar_t title[64];
    _snwprintf_s(title, 64, _TRUNCATE, TXT_SCRIPT_STOPPED_TITLE, overlay_name(o));
    char *u8 = to_utf8(err && *err ? err : TXT_UNKNOWN_ERROR);
    log_error("script #%d stopped: %s", o->id, u8 ? u8 : "unknown error");
    free(u8);
    if (app_notify(title, err && *err ? err : TXT_UNKNOWN_ERROR))
        log_info("GIF #%d: script error notification shown", o->id);
}

/* After any call into the script: its earliest timer may have changed. */
static void refresh_timer(Overlay *o)
{
    double t = o->script ? script_next_timer(o->script) : -1;
    o->next_timer = t < 0 ? INT64_MAX : (int64_t)(t * (double)g_app.qpc_freq);
}

static bool checked(Overlay *o, bool ok, const wchar_t *err)
{
    if (!ok) {
        script_fail(o, err);
        return false;
    }
    refresh_timer(o);
    return true;
}

static void deliver_plain(Overlay *o, ScriptEvent ev)
{
    wchar_t err[512];
    if (o->script && (o->subs & SEV_BIT(ev))) checked(o, script_plain_event(o->script, ev, err, 512), err);
}

static void deliver_clip_event(Overlay *o, ScriptEvent ev, int frame)
{
    wchar_t err[512];
    if (o->script && (o->subs & SEV_BIT(ev)))
        checked(o, script_clip_event(o->script, ev, cur_clip(o)->name, frame, err, 512), err);
}

/* The window was moved or resized by something other than the script. */
static void moved_externally(Overlay *o)
{
    sync_pos(o);
    deliver_plain(o, SEV_MOVED);
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
    o->subs = 0;
    o->script_failed = false;
    free(o->script_spec);
    o->script_spec = copy;
    if (o->leader) {
        /* A follower's own script stays dormant; only the leader may control it. */
        script_destroy(o->script);
        o->script = NULL;
        o->subs = 0;
        o->script_failed = false;
        o->next_timer = INT64_MAX;
        overlay_refresh_hook();
        return;
    }
    reset_script_state(o);
    o->tick_interval = g_app.qpc_freq / SCRIPT_RATE_DEFAULT;
    if (!*copy) {
        overlay_refresh_hook();
        return;
    }

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
    reset_tick(o);
    peers_changed();
    if (src) {
        o->script = script_create_ex(&HOST_OPS, &HOST_EXT, o, src, len, b ? b->chunk : chunk, name, o->id,
                                     err, 512);
    }
    free(src);
    free(chunk);
    free(name);
    if (!o->script) {
        reset_script_state(o); /* Undo whatever the failed top level did. */
        script_fail(o, err);
    } else {
        o->subs = script_subscriptions(o->script);
        refresh_timer(o);
        char u8[MAX_PATH * 4];
        log_info("script #%d loaded: %s", o->id, applog_u8(o->script_spec, u8, sizeof(u8)));
    }
    overlay_refresh_hook();
    app_reschedule();
}

bool overlay_has_followers(const Overlay *o)
{
    if (!o) return false;
    for (const Overlay *q = g_app.overlays; q; q = q->next) {
        if (q->leader == o) return true;
    }
    return false;
}

bool overlay_bind_leader(Overlay *follower, Overlay *leader, wchar_t *err, size_t cap)
{
    if (err && cap) err[0] = 0;
    if (!follower || !leader || follower == leader) {
        if (err) _snwprintf_s(err, cap, _TRUNCATE, TXT_LEADER_CYCLE);
        return false;
    }
    if (follower->leader == leader) return true;
    if (leader->leader) {
        if (err) _snwprintf_s(err, cap, _TRUNCATE, TXT_LEADER_CHAIN_ERROR);
        return false;
    }
    if (overlay_has_followers(follower)) {
        if (err) _snwprintf_s(err, cap, _TRUNCATE, TXT_LEADER_CHAIN_ERROR);
        return false;
    }
    for (Overlay *m = leader; m; m = m->leader) {
        if (m == follower) {
            if (err) _snwprintf_s(err, cap, _TRUNCATE, TXT_LEADER_CYCLE);
            return false;
        }
    }
    script_destroy(follower->script);
    follower->script = NULL;
    follower->subs = 0;
    follower->script_failed = false;
    follower->next_timer = INT64_MAX;
    follower->leader = leader;
    peers_changed();
    overlay_refresh_hook();
    app_changed();
    char u8[MAX_PATH * 4];
    log_info("GIF #%d is now controlled by #%d (%s)", follower->id, leader->id,
             applog_u8(overlay_name(leader), u8, sizeof(u8)));
    return true;
}

void overlay_unbind_leader(Overlay *follower, bool reload_script)
{
    if (!follower || !follower->leader) return;
    follower->leader = NULL;
    if (reload_script && follower->script_spec && *follower->script_spec)
        overlay_set_script(follower, follower->script_spec);
    peers_changed();
    overlay_refresh_hook();
    app_changed();
}

void overlay_leader_removed(Overlay *leader)
{
    if (!leader) return;
    bool changed = false;
    for (Overlay *o = g_app.overlays; o; o = o->next) {
        if (o->leader != leader) continue;
        o->leader = NULL;
        changed = true;
        if (!g_app.shutting_down && o->script_spec && *o->script_spec)
            overlay_set_script(o, o->script_spec);
    }
    if (changed) {
        peers_changed();
        overlay_refresh_hook();
        app_changed();
    }
}

bool overlay_is_moving(const Overlay *o)
{
    return script_live(o) && o->surface && !o->dragging && script_has_update(o->script);
}

int64_t overlay_deadline(const Overlay *o)
{
    int64_t t = INT64_MAX;
    if (overlay_is_animating(o)) t = o->next_frame;
    if (overlay_is_moving(o) && o->next_tick < t) t = o->next_tick;
    if (script_live(o) && !o->dragging && o->next_timer < t) t = o->next_timer;
    if (o->xf_animating && o->xf_end < t) t = o->xf_end;
    if (o->hover_at < t) t = o->hover_at;
    if (o->next_poll < t) t = o->next_poll;
    return t;
}

static void step_update(Overlay *o, int64_t now)
{
    double dt = (double)(now - o->last_tick) / (double)g_app.qpc_freq;
    if (dt < 0) dt = 0;
    if (dt > SCRIPT_MAX_DT) dt = SCRIPT_MAX_DT;
    o->last_tick = now;
    double x0 = o->pos_x, y0 = o->pos_y;
    wchar_t err[512];
    if (!checked(o, script_update(o->script, dt, err, 512), err)) return;
    o->vx = dt > 0 ? (o->pos_x - x0) / dt : 0;
    o->vy = dt > 0 ? (o->pos_y - y0) / dt : 0;
    o->next_tick += o->tick_interval;
    if (o->next_tick <= now) o->next_tick = now + o->tick_interval;
}

static void poll_input(Overlay *o, int64_t now);
static void hover_check(Overlay *o, int64_t now);

bool overlay_service(Overlay *o, int64_t now, int64_t slack)
{
    bool moved = false;
    if (overlay_is_animating(o) && o->next_frame - now <= slack) advance(o, now);
    if (overlay_is_moving(o) && o->next_tick - now <= slack) {
        step_update(o, now);
        moved = true;
    }
    if (script_live(o) && !o->dragging && o->next_timer - now <= slack) {
        wchar_t err[512];
        double t = (double)(now + slack) / (double)g_app.qpc_freq;
        if (checked(o, script_run_timers(o->script, t, err, 512), err)) moved = true;
    }
    if (o->xf_animating && o->xf_end - now <= slack) {
        /* Settle on the final bounds (smaller than the union used meanwhile). */
        o->xf_animating = false;
        o->xf_end = INT64_MAX;
        o->xf.from = o->xf.to;
        o->xf.duration = 0;
        apply_geometry(o);
        deliver_plain(o, SEV_TRANSFORM_END);
        moved = true;
    }
    if (o->hover_at - now <= slack) hover_check(o, now);
    if (o->next_poll - now <= slack) poll_input(o, now);
    return moved;
}

void overlay_screens_changed(Overlay *o)
{
    sync_pos(o);
    deliver_plain(o, SEV_SCREENS_CHANGED);
}

void overlay_refresh_visibility(Overlay *o)
{
    if (!o || !o->hwnd) return;
    bool hidden = overlay_effective_hidden(o);
    bool visible = IsWindowVisible(o->hwnd) != FALSE;
    if (hidden == !visible) return;
    if (hidden) {
        overlay_announce(o, false);
        ShowWindow(o->hwnd, SW_HIDE);
        overlay_detach_surface(o);
        /* Keep the decoders (size, frame cache, hitbox) but drop the canvases. */
        free_snapshot(&o->play);
        for (int i = 0; i < o->clip_count; ++i) clip_park(&o->clips[i]);
        ifsm_reset(&o->input);
        overlay_refresh_hook();
        return;
    }
    bool ok = clip_activate(cur_clip(o), o->scale_pct);
    o->play.frame = 0;
    o->play.held = false;
    /* Its monitor may have gone away while it was hidden. */
    RECT r;
    GetWindowRect(o->hwnd, &r);
    if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
        RECT work;
        SIZE s = scaled_size(o);
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        o->pos_x = work.left + (work.right - work.left - s.cx) / 2;
        o->pos_y = work.top + (work.bottom - work.top - s.cy) / 2;
    }
    apply_geometry(o);
    overlay_attach_surface(o);
    reset_timing(o);
    o->failed = !ok;
    reset_tick(o);
    ShowWindow(o->hwnd, SW_SHOWNOACTIVATE);
    overlay_announce(o, true);
    overlay_refresh_hook();
    moved_externally(o); /* The script resumes from wherever the window is now. */
}

void overlay_set_hidden(Overlay *o, bool hidden)
{
    if (o->hidden == hidden) return;
    char u8[MAX_PATH * 4];
    log_info("GIF #%d %s: %s", o->id, hidden ? "hidden" : "shown",
             applog_u8(overlay_name(o), u8, sizeof(u8)));
    o->hidden = hidden;
    overlay_refresh_visibility(o);
}

/* ---- contacts and peer events ---------------------------------------------- */

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
    ScriptEvent ev = begin ? SEV_COLLIDE : SEV_SEPARATE;
    if (!o || !o->script || !(o->subs & SEV_BIT(ev))) return;
    ScriptPeer p;
    memset(&p, 0, sizeof(p));
    p.id = other_id;
    p.name = "";
    if (other) fill_peer(other, &p);
    wchar_t err[512];
    checked(o, script_peer_event(o->script, ev, &p, err, 512), err);
}

static void on_contact(void *ctx, int a, int b, bool begin)
{
    (void)ctx;
    Overlay *oa = find_overlay(a), *ob = find_overlay(b);
    notify_contact(oa, ob, b, begin);
    notify_contact(ob, oa, a, begin);
}

/* peer_* to every other live script subscribed to ev. */
static void broadcast_peer(Overlay *about, ScriptEvent ev, const ScriptPeer *p)
{
    for (Overlay *q = g_app.overlays; q; q = q->next) {
        if (q == about || !script_live(q) || !(q->subs & SEV_BIT(ev))) continue;
        wchar_t err[512];
        checked(q, script_peer_event(q->script, ev, p, err, 512), err);
    }
}

void overlay_announce(Overlay *o, bool added)
{
    ScriptPeer p;
    memset(&p, 0, sizeof(p));
    p.name = "";
    p.id = o->id;
    if (added) fill_peer(o, &p);
    peers_changed();
    broadcast_peer(o, added ? SEV_PEER_ADDED : SEV_PEER_REMOVED, &p);
}

bool overlay_peers_dirty(void)
{
    return s_peers_dirty;
}

void overlay_update_contacts(void)
{
    ContactBox boxes[SCRIPT_MAX_PEERS + 1];
    int n = 0;
    for (Overlay *o = g_app.overlays; o && n < SCRIPT_MAX_PEERS + 1; o = o->next) {
        if (overlay_effective_hidden(o)) continue; /* Not on screen: touches nothing. */
        double x, y;
        int hb[4];
        peer_rect(o, &x, &y);
        overlay_hitbox(o, hb);
        ContactBox *bx = &boxes[n++];
        bx->id = o->id;
        bx->l = round_px(x) + hb[0];
        bx->t = round_px(y) + hb[1];
        bx->r = bx->l + hb[2];
        bx->b = bx->t + hb[3];
    }
    /* Callbacks only run scripts; none of them can add or remove an overlay. */
    contacts_update(&s_contacts, boxes, n, on_contact, NULL);

    /* peer_moved: one event per overlay that changed, only to subscribers. */
    if (!s_peers_dirty) return;
    s_peers_dirty = false;
    bool any_sub = false, watch_moved = false;
    for (Overlay *q = g_app.overlays; q; q = q->next) any_sub |= script_live(q) && (q->subs & SEV_BIT(SEV_PEER_MOVED));
    for (Overlay *o = g_app.overlays; o; o = o->next) {
        if (!o->peer_dirty) continue;
        o->peer_dirty = false;
        if (effective_clickthrough(o) && (o->subs & SEV_INPUT_MASK)) watch_moved = true;
        if (!any_sub || overlay_effective_hidden(o)) continue;
        ScriptPeer p;
        fill_peer(o, &p);
        broadcast_peer(o, SEV_PEER_MOVED, &p);
    }
    if (watch_moved) overlay_refresh_hook(); /* Click-through boxes for the mouse hook. */
}

/* ---- input -----------------------------------------------------------------
 * Window messages (normal overlays) and the low-level mouse hook
 * (click-through overlays) both feed the InputFsm, which produces script
 * events. Each event goes to the script first (yagp.on handlers; returning
 * true or setting e.handled stops it), then to the default behaviour: left
 * drag moves the GIF, right click opens the menu. Shift + right click always
 * opens the menu, whatever the script does. */

static InputMetrics input_metrics(void)
{
    InputMetrics m;
    m.drag_dx = GetSystemMetrics(SM_CXDRAG);
    m.drag_dy = GetSystemMetrics(SM_CYDRAG);
    m.dbl_dx = GetSystemMetrics(SM_CXDOUBLECLK);
    m.dbl_dy = GetSystemMetrics(SM_CYDOUBLECLK);
    m.dbl_ms = GetDoubleClickTime();
    UINT hover = 400;
    SystemParametersInfoW(SPI_GETMOUSEHOVERTIME, 0, &hover, 0);
    m.hover_ms = hover;
    return m;
}

/* Desktop point -> GIF pixels of the current clip, and whether that pixel is
 * visible (alpha from the canvas on screen). */
static bool map_to_gif(const Overlay *o, int x, int y, double *gx, double *gy)
{
    Xform s, inv;
    current_xform(o, &s);
    *gx = *gy = -1;
    if (!xform_invert(&s, &inv)) return false;
    double lx, ly; /* Layout box pixels. */
    xform_apply(&inv, x - o->pos_x, y - o->pos_y, &lx, &ly);
    SIZE sz = scaled_size(o);
    const GifStream *g = cur_gif(o);
    if (o->flip_x) lx = sz.cx - lx;
    *gx = lx * g->width / sz.cx;
    *gy = ly * g->height / sz.cy;
    if (lx < 0 || ly < 0 || lx >= sz.cx || ly >= sz.cy || cur_clip(o)->parked) return false;
    int cx = (int)(lx * g->out_w / sz.cx), cy = (int)(ly * g->out_h / sz.cy);
    if (cx < 0 || cy < 0 || cx >= g->out_w || cy >= g->out_h) return false;
    return g->bgra[((size_t)cy * (size_t)g->out_w + (size_t)cx) * 4 + 3] != 0;
}

static void fill_input(const Overlay *o, ScriptInput *in, int button, int x, int y, bool passthrough)
{
    memset(in, 0, sizeof(*in));
    in->button = button;
    in->x = x;
    in->y = y;
    in->opaque = map_to_gif(o, x, y, &in->gx, &in->gy);
    /* Hook events come from other processes' input, which our thread's key
     * state does not follow: read the global state for those. */
    SHORT (WINAPI *key)(int) = passthrough ? GetAsyncKeyState : GetKeyState;
    in->shift = (key(VK_SHIFT) & 0x8000) != 0;
    in->ctrl = (key(VK_CONTROL) & 0x8000) != 0;
    in->alt = (key(VK_MENU) & 0x8000) != 0;
    in->passthrough = passthrough;
}

static void show_context_menu(Overlay *o, int x, int y);

/* Delivers the FSM's events in order and runs the default actions. Returns
 * false if o was removed (menu command). */
static bool route(Overlay *o, const InputOut *out, int x, int y, bool passthrough)
{
    for (int i = 0; i < out->n; ++i) {
        int ev = out->ev[i];
        bool handled = false;
        if (o->script && script_live(o) && (o->subs & SEV_BIT(ev))) {
            ScriptInput in;
            fill_input(o, &in, ev == IFSM_ENTER || ev == IFSM_LEAVE || ev == IFSM_HOVER ? SBTN_NONE : out->button,
                       x, y, passthrough);
            in.dx = out->dx;
            in.dy = out->dy;
            wchar_t err[512];
            if (!checked(o, script_input(o->script, (ScriptEvent)ev, &in, &handled, err, 512), err)) handled = false;
        }
        if (passthrough) {
            /* Observed only: the click went to whatever is below. Drags are
             * reported in full (drag / drag_end), there is no system drag. */
            if (ev == IFSM_DRAG_START) ifsm_take_drag(&o->input);
            continue;
        }
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (ev == IFSM_CLICK && out->button == SBTN_RIGHT && shift) handled = false; /* Never lose the menu. */
        if (ev == IFSM_DRAG_START && handled) ifsm_take_drag(&o->input);
        int act = ifsm_default_action(&o->input, out, i, handled);
        if (act == IFSM_ACT_SYSTEM_DRAG) {
            if (o->locked || !o->draggable) continue;
            /* Hand the press over to the system move loop (WM_ENTERSIZEMOVE ...). */
            ifsm_cancel(&o->input);
            ReleaseCapture();
            o->sys_drag = true;
            SendMessageW(o->hwnd, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, 0);
            return true;
        }
        if (act == IFSM_ACT_MENU) {
            int id = o->id;
            show_context_menu(o, x, y);
            return find_overlay(id) != NULL;
        }
    }
    return true;
}

/* Wheel: one event, no FSM state and no default behaviour. */
static bool route_wheel(Overlay *o, int x, int y, double notches, bool passthrough)
{
    if (!o->script || !script_live(o) || !(o->subs & SEV_BIT(SEV_WHEEL))) return true;
    ScriptInput in;
    fill_input(o, &in, SBTN_NONE, x, y, passthrough);
    in.wheel = notches;
    bool handled;
    wchar_t err[512];
    checked(o, script_input(o->script, SEV_WHEEL, &in, &handled, err, 512), err);
    return true;
}

static void arm_tracking(Overlay *o)
{
    if (o->tracking) return;
    TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE | ((o->subs & SEV_BIT(SEV_HOVER)) ? TME_HOVER : 0), o->hwnd,
                         HOVER_DEFAULT};
    if (TrackMouseEvent(&t)) o->tracking = true;
}

/* Is the desktop point over o's visible area (hitbox)? */
static bool over_hitbox(const Overlay *o, int x, int y)
{
    int hb[4];
    overlay_hitbox(o, hb);
    int l = round_px(o->pos_x) + hb[0], t = round_px(o->pos_y) + hb[1];
    return x >= l && x < l + hb[2] && y >= t && y < t + hb[3];
}

static int button_of(UINT msg)
{
    switch (msg) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: return SBTN_LEFT;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK: return SBTN_RIGHT;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK: return SBTN_MIDDLE;
    default: return SBTN_NONE;
    }
}

/* Mouse messages of a normal overlay. Returns false if o was removed. */
static bool window_input(Overlay *o, UINT msg, WPARAM wp, LPARAM lp)
{
    POINT p = {(short)LOWORD(lp), (short)HIWORD(lp)};
    if (msg != WM_MOUSEWHEEL) ClientToScreen(o->hwnd, &p); /* Wheel positions are already on the screen. */
    InputMetrics m = input_metrics();
    unsigned ms = (unsigned)GetMessageTime();
    InputOut out;
    out.n = 0;
    switch (msg) {
    case WM_MOUSEMOVE:
        arm_tracking(o);
        ifsm_move(&o->input, &m, p.x, p.y, true, ms, &out);
        break;
    case WM_MOUSELEAVE:
        o->tracking = false;
        ifsm_leave(&o->input, &out);
        break;
    case WM_MOUSEHOVER:
        o->tracking = false;
        arm_tracking(o); /* Keep getting leave. */
        ifsm_idle(&o->input, &m, o->input.move_ms + m.hover_ms, &out);
        break;
    case WM_MOUSEWHEEL:
        return route_wheel(o, p.x, p.y, (double)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA, false);
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
    case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK:
        /* Double clicks are detected by the FSM, the same way as for the hook. */
        SetCapture(o->hwnd);
        ifsm_button_down(&o->input, &m, button_of(msg), p.x, p.y, ms, &out);
        break;
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
        ifsm_button_up(&o->input, &m, button_of(msg), p.x, p.y, ms, &out);
        if (!o->input.button && GetCapture() == o->hwnd) ReleaseCapture();
        int id = o->id;
        if (!route(o, &out, p.x, p.y, false) || !find_overlay(id)) return false;
        /* Released outside: the leave was swallowed by the capture. */
        RECT wr;
        GetWindowRect(o->hwnd, &wr);
        if (!o->input.button && !PtInRect(&wr, p)) {
            ifsm_leave(&o->input, &out);
            o->tracking = false;
            return route(o, &out, p.x, p.y, false);
        }
        return true;
    }
    case WM_CAPTURECHANGED:
        ifsm_cancel(&o->input);
        return true;
    }
    return route(o, &out, p.x, p.y, false);
}

/* ---- mouse hook: cursor subscriptions and click-through input ---------------
 * Installed only while needed. Click-through overlays with input handlers
 * get their events from it (observed, not intercepted); cursor subscribers
 * get "cursor". If the hook cannot be installed, the same is done by polling
 * at POLL_HZ (a very short click between two polls can be missed). */

static bool wants_hook_input(const Overlay *o)
{
    return script_live(o) && effective_clickthrough(o) && (o->subs & SEV_INPUT_MASK) && o->surface;
}

static bool wants_cursor(const Overlay *o)
{
    return script_live(o) && (o->subs & SEV_BIT(SEV_CURSOR));
}

static int build_watch(HookWatch *w)
{
    int n = 0;
    for (Overlay *o = g_app.overlays; o && n < CURSOR_HOOK_MAX_WATCH; o = o->next) {
        if (!wants_hook_input(o)) continue;
        int hb[4];
        overlay_hitbox(o, hb);
        w[n].id = o->id;
        w[n].box.left = round_px(o->pos_x) + hb[0];
        w[n].box.top = round_px(o->pos_y) + hb[1];
        w[n].box.right = w[n].box.left + hb[2];
        w[n].box.bottom = w[n].box.top + hb[3];
        ++n;
    }
    return n;
}

void overlay_refresh_hook(void)
{
    HookWatch w[CURSOR_HOOK_MAX_WATCH];
    int n = build_watch(w);
    bool cursor = false;
    for (Overlay *o = g_app.overlays; o; o = o->next) cursor |= wants_cursor(o);
    cursor_hook_configure(g_app.tray_hwnd, cursor, w, n);
    bool fallback = (cursor || n > 0) && !cursor_hook_running();
    for (Overlay *o = g_app.overlays; o; o = o->next) {
        bool poll = fallback && (wants_cursor(o) || wants_hook_input(o));
        if (poll && o->next_poll == INT64_MAX) {
            o->next_poll = app_now();
            GetCursorPos(&o->poll_cursor);
            o->poll_buttons = 0;
        } else if (!poll) {
            o->next_poll = INT64_MAX;
        }
        if (!wants_hook_input(o) && effective_clickthrough(o)) o->hover_at = INT64_MAX;
    }
    app_reschedule();
}

static void deliver_cursor(Overlay *o, int x, int y, bool valid)
{
    wchar_t err[512];
    if (wants_cursor(o)) checked(o, script_cursor(o->script, x, y, valid, err, 512), err);
}

void overlay_cursor_moved(void)
{
    POINT p = cursor_hook_pos();
    for (Overlay *o = g_app.overlays; o; o = o->next) deliver_cursor(o, p.x, p.y, true);
    app_reschedule();
}

/* Is o under the cursor, or only covered by something else at (x, y)?
 * Walks up the z-order from the topmost window there to o. */
static bool on_top_at(const Overlay *o, int x, int y)
{
    if (o->topmost) {
        /* Only another topmost window above it can cover it. */
        POINT p = {x, y};
        HWND top = WindowFromPoint(p);
        if (!top) return true;
        top = GetAncestor(top, GA_ROOT);
        if (top == o->hwnd || !(GetWindowLongPtrW(top, GWL_EXSTYLE) & WS_EX_TOPMOST)) return true;
    }
    POINT p = {x, y};
    HWND top = WindowFromPoint(p); /* Skips WS_EX_TRANSPARENT windows: never o itself. */
    if (!top) return true;
    top = GetAncestor(top, GA_ROOT);
    for (HWND h = top; h; h = GetWindow(h, GW_HWNDPREV)) {
        if (h == o->hwnd) return true; /* o is above the window under the cursor. */
    }
    return false;
}

/* One hook / poll event for a click-through overlay. */
static void passthrough_event(Overlay *o, const HookEvent *e, const InputMetrics *m)
{
    InputOut out;
    out.n = 0;
    bool inside = over_hitbox(o, e->x, e->y);
    switch (e->kind) {
    case HOOK_MOVE:
        ifsm_move(&o->input, m, e->x, e->y, inside && on_top_at(o, e->x, e->y), e->time, &out);
        if (o->input.inside && (o->subs & SEV_BIT(SEV_HOVER)) && !o->input.hovered)
            o->hover_at = app_now() + (int64_t)m->hover_ms * g_app.qpc_freq / 1000;
        else if (!o->input.inside)
            o->hover_at = INT64_MAX;
        break;
    case HOOK_DOWN: {
        double gx, gy;
        if (!inside || !map_to_gif(o, e->x, e->y, &gx, &gy) || !on_top_at(o, e->x, e->y)) return;
        ifsm_button_down(&o->input, m, e->button, e->x, e->y, e->time, &out);
        break;
    }
    case HOOK_UP:
        ifsm_button_up(&o->input, m, e->button, e->x, e->y, e->time, &out);
        break;
    case HOOK_WHEEL:
        if (inside && on_top_at(o, e->x, e->y)) route_wheel(o, e->x, e->y, (double)e->wheel / WHEEL_DELTA, true);
        return;
    }
    route(o, &out, e->x, e->y, true);
}

static void hover_check(Overlay *o, int64_t now)
{
    (void)now;
    o->hover_at = INT64_MAX;
    InputMetrics m = input_metrics();
    InputOut out;
    ifsm_idle(&o->input, &m, o->input.move_ms + m.hover_ms, &out);
    POINT p = cursor_hook_pos();
    route(o, &out, p.x, p.y, true);
}

void overlay_hook_input(void)
{
    HookEvent ev[64];
    int n = cursor_hook_take(ev, 64);
    InputMetrics m = input_metrics();
    for (int i = 0; i < n; ++i) {
        for (Overlay *o = g_app.overlays; o; o = o->next) {
            if (wants_hook_input(o)) passthrough_event(o, &ev[i], &m);
        }
    }
    app_reschedule();
}

/* Polling fallback (hook unavailable): cursor and button edges. */
static void poll_input(Overlay *o, int64_t now)
{
    o->next_poll = now + g_app.qpc_freq / POLL_HZ;
    POINT p;
    bool valid = GetCursorPos(&p) != 0;
    if (!valid) p = o->poll_cursor;
    bool moved = p.x != o->poll_cursor.x || p.y != o->poll_cursor.y;
    o->poll_cursor = p;
    int id = o->id;
    if (moved) deliver_cursor(o, p.x, p.y, valid);
    if (!find_overlay(id) || !wants_hook_input(o)) return;
    InputMetrics m = input_metrics();
    HookEvent e;
    memset(&e, 0, sizeof(e));
    e.x = p.x;
    e.y = p.y;
    e.time = GetTickCount();
    if (moved) {
        e.kind = HOOK_MOVE;
        passthrough_event(o, &e, &m);
    }
    bool swap = GetSystemMetrics(SM_SWAPBUTTON) != 0;
    int vk[4] = {0, swap ? VK_RBUTTON : VK_LBUTTON, swap ? VK_LBUTTON : VK_RBUTTON, VK_MBUTTON};
    for (int b = 1; b <= 3; ++b) {
        bool down = (GetAsyncKeyState(vk[b]) & 0x8000) != 0, was = (o->poll_buttons >> b) & 1;
        if (down == was) continue;
        o->poll_buttons ^= 1 << b;
        e.kind = down ? HOOK_DOWN : HOOK_UP;
        e.button = b;
        passthrough_event(o, &e, &m);
    }
}

/* ---- window ------------------------------------------------------------------ */

static void show_context_menu(Overlay *o, int x, int y)
{
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    overlay_build_menu(o, menu, 0);
    UINT cmd = app_track_menu(menu, x, y);
    DestroyMenu(menu);
    if (cmd) overlay_command(o, cmd); /* May free o. */
}

static HCURSOR cursor_for(const Overlay *o)
{
    if (o->cursor_shape == SCURSOR_NONE) return NULL;
    if (o->cursor_shape == SCURSOR_HAND) return LoadCursorW(NULL, IDC_HAND);
    if (o->cursor_shape == SCURSOR_ARROW || o->locked || !o->draggable) return LoadCursorW(NULL, IDC_ARROW);
    return LoadCursorW(NULL, IDC_SIZEALL);
}

static LRESULT CALLBACK overlay_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Overlay *o = (Overlay *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
        break;
    case WM_NCHITTEST:
        /* Everything is client area: we tell clicks from drags ourselves and
         * hand drags to the system move loop (route). */
        if (o) return HTCLIENT;
        break;
    case WM_SETCURSOR:
        if (o && LOWORD(lp) == HTCLIENT) {
            SetCursor(cursor_for(o));
            return TRUE;
        }
        break;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE: case WM_MOUSELEAVE: case WM_MOUSEHOVER: case WM_MOUSEWHEEL:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_CAPTURECHANGED:
        if (o) {
            window_input(o, msg, wp, lp); /* May free o. */
            app_reschedule();
            return 0;
        }
        break;
    case WM_CONTEXTMENU:
        return 0; /* Our own right-click handling opens the menu (or not). */
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
            o->sys_drag = false;
            ifsm_reset(&o->input);
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
            o->peer_dirty = true;
            peers_changed();
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
    wc.hCursor = NULL; /* WM_SETCURSOR picks it (yagp.set_cursor). */
    wc.lpszClassName = OVERLAY_CLASS;
    return RegisterClassExW(&wc) != 0;
}

static void center_on_primary(Overlay *o, int cascade)
{
    RECT work;
    SIZE s = scaled_size(o);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int offset = (cascade % 8) * 32;
    o->pos_x = work.left + (work.right - work.left - s.cx) / 2 + offset;
    o->pos_y = work.top + (work.bottom - work.top - s.cy) / 2 + offset;
    apply_geometry(o);
}

static void init_defaults(Overlay *o)
{
    o->play.from = o->play.to = -1;
    o->play.loops_left = -1;
    o->play.speed = 1.0;
    o->play.playing = true;
    xform_anim_start(&o->xf, &XFORM_IDENTITY, &XFORM_IDENTITY, 0, 0, SEASE_LINEAR);
    o->xf_end = INT64_MAX;
    o->opacity = o->op_from = o->op_to = 1.0;
    o->draggable = true;
    o->ct_override = -1;
    o->next_timer = INT64_MAX;
    o->hover_at = INT64_MAX;
    o->next_poll = INT64_MAX;
    ifsm_reset(&o->input);
}

Overlay *overlay_create(const OverlayConfig *cfg, bool use_position, int cascade)
{
    Overlay *o = (Overlay *)calloc(1, sizeof(Overlay));
    if (!o) {
        log_error("cannot allocate an overlay");
        return NULL;
    }
    const char *stage = "out of memory";
    init_defaults(o);
    o->id = g_app.next_overlay_id++;
    o->uid = cfg->uid ? cfg->uid : app_uid_alloc();
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
    if (!o->path) goto fail;
    o->name_u8 = to_utf8(overlay_name(o));
    ClipKey key = overlay_key(o);
    stage = "cannot open GIF";
    if (!clip_open(&o->clips[0], "main", o->path, &key, o->scale_pct, false)) goto fail;
    o->clip_count = 1;
    o->still_image = o->clips[0].gif->is_still;

    /* WS_EX_TOOLWINDOW without an owner keeps it off the taskbar and Alt+Tab.
     * WS_EX_NOREDIRECTIONBITMAP: DWM composes only our DirectComposition visual. */
    DWORD ex = WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE;
    if (o->topmost) ex |= WS_EX_TOPMOST;
    SIZE s = scaled_size(o);
    o->aabb[2] = s.cx;
    o->aabb[3] = s.cy;
    stage = "cannot create the overlay window";
    o->hwnd = CreateWindowExW(ex, OVERLAY_CLASS, overlay_name(o), WS_POPUP,
                              cfg->x, cfg->y, s.cx, s.cy, NULL, NULL, g_app.instance, o);
    if (!o->hwnd) goto fail;
    o->pos_x = cfg->x;
    o->pos_y = cfg->y;

    RECT r = {cfg->x, cfg->y, cfg->x + s.cx, cfg->y + s.cy};
    if (!use_position || !MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) center_on_primary(o, cascade);
    if (o->clickthrough) apply_ex_style(o);
    stage = "cannot create the swap chain";
    if (!overlay_attach_surface(o)) goto fail;
    reset_timing(o);
    sync_pos(o);
    if (o->hidden) clip_park(&o->clips[0]); /* Restored hidden: no canvas until shown. */
    else ShowWindow(o->hwnd, SW_SHOWNOACTIVATE);
    /* A custom script that has gone missing is dropped silently on restore. */
    if (cfg->script && *cfg->script) {
        if (cfg->defer_script) {
            o->script_spec = dup_wcs(cfg->script);
        } else if (find_builtin(cfg->script) || GetFileAttributesW(cfg->script) != INVALID_FILE_ATTRIBUTES) {
            overlay_set_script(o, cfg->script);
        } else {
            char spec_u8[MAX_PATH * 4];
            log_warn("skipping missing script for GIF #%d: %s", o->id,
                     applog_u8(cfg->script, spec_u8, sizeof(spec_u8)));
        }
    }
    {
        char u8[MAX_PATH * 4];
        log_info("created GIF #%d: %s (%dx%d, %d frame(s), scale %d%%)", o->id,
                 applog_u8(o->path, u8, sizeof(u8)), o->clips[0].gif->width, o->clips[0].gif->height,
                 o->clips[0].frame_count, o->scale_pct);
    }
    return o;

fail:
    {
        char u8[MAX_PATH * 4];
        log_error("cannot create overlay for %s: %s", applog_u8(cfg->path, u8, sizeof(u8)), stage);
    }
    overlay_destroy(o);
    return NULL;
}

void overlay_destroy(Overlay *o)
{
    if (!o) return;
    script_destroy(o->script);
    o->script = NULL;
    o->subs = 0;
    free(o->script_spec);
    overlay_detach_surface(o);
    if (o->hwnd) {
        SetWindowLongPtrW(o->hwnd, GWLP_USERDATA, 0);
        DestroyWindow(o->hwnd);
    }
    free_snapshot(&o->play);
    for (int i = 0; i < o->clip_count; ++i) clip_close(&o->clips[i]);
    free(o->path);
    free(o->name_u8);
    free(o);
    peers_changed();
}

void overlay_cache_ready(void *job)
{
    for (Overlay *o = g_app.overlays; o; o = o->next) {
        for (int i = 0; i < o->clip_count; ++i) {
            if (clip_cache_ready(&o->clips[i], job)) {
                if (i == o->play.clip) {
                    o->peer_dirty = true; /* The hitbox is known now. */
                    peers_changed();
                    o->is_static = false; /* A range end may be known now too. */
                }
                clip_job_done(job);
                return;
            }
        }
    }
    clip_job_done(job);
}

void overlay_replace(Overlay *o, const wchar_t *path)
{
    Clip c;
    ClipKey key = overlay_key(o);
    wchar_t *p = dup_wcs(path);
    char u8[MAX_PATH * 4];
    if (!p || !clip_open(&c, "main", path, &key, o->scale_pct, overlay_effective_hidden(o))) {
        free(p);
        log_error("cannot replace GIF: %s", applog_u8(path, u8, sizeof(u8)));
        app_error(TXT_ERR_OPEN_GIF, path);
        return;
    }
    /* Clips a script loaded belong to the old GIF's folder: back to a single clip. */
    for (int i = o->clip_count - 1; i >= 1; --i) clip_close(&o->clips[i]);
    o->clip_count = 1;
    if (o->surface && !render_surface_resize(o->surface, c.gif->out_w, c.gif->out_h)) {
        clip_close(&c);
        free(p);
        log_error("cannot resize swap chain for GIF #%d: %s", o->id, applog_u8(path, u8, sizeof(u8)));
        app_error(TXT_ERR_SWAPCHAIN, path);
        return;
    }
    POINT center = {round_px(o->pos_x) + scaled_size(o).cx / 2, round_px(o->pos_y) + scaled_size(o).cy / 2};
    free_snapshot(&o->play);
    clip_close(&o->clips[0]);
    o->clips[0] = c;
    o->still_image = c.gif->is_still;
    o->is_static = o->still_image;
    o->play.clip = 0;
    o->play.frame = 0;
    free(o->path);
    o->path = p;
    free(o->name_u8);
    o->name_u8 = to_utf8(overlay_name(o));
    o->has_override = false;
    SetWindowTextW(o->hwnd, overlay_name(o));
    SIZE s = scaled_size(o);
    o->pos_x = center.x - s.cx / 2;
    o->pos_y = center.y - s.cy / 2;
    apply_geometry(o);
    present_full(o);
    reset_timing(o);
    peers_changed();
    /* The script keeps running, but its clips are gone: reload it so it
     * starts over with the new GIF (its top level loads clips again). */
    if (o->script) {
        wchar_t *spec = dup_wcs(o->script_spec);
        if (spec) overlay_set_script(o, spec);
        free(spec);
    }
    log_info("replaced GIF #%d: %s", o->id, applog_u8(o->path, u8, sizeof(u8)));
    deliver_plain(o, SEV_MOVED); /* Size changed. */
}

/* Re-decode from frame 0 so the key also applies to already-composited pixels. */
static void apply_color_key(Overlay *o)
{
    ClipKey key = overlay_key(o);
    free_snapshot(&o->play);
    bool ok = true;
    for (int i = 0; i < o->clip_count; ++i) ok &= clip_rekey(&o->clips[i], &key, o->scale_pct);
    if (!ok) {
        o->failed = true;
        return;
    }
    o->play.frame = 0;
    o->play.held = false;
    present_full(o);
    reset_timing(o);
}

/* Re-decode from frame 0 with the canvas a scale of pct needs. */
static void apply_canvas_for(Overlay *o, int pct)
{
    if (overlay_effective_hidden(o)) return; /* Rebuilt when shown. */
    Clip *c = cur_clip(o);
    int w, h;
    clip_canvas_size(c->gif, pct, &w, &h);
    if (w == c->gif->out_w && h == c->gif->out_h) return;
    free_snapshot(&o->play);
    if (!clip_resize_canvas(c, pct) || (o->surface && !render_surface_resize(o->surface, w, h))) {
        o->failed = true;
        return;
    }
    o->play.frame = 0;
    push_matrix(o);
    present_full(o);
    reset_timing(o);
}

/* Colour of the main GIF's first top-left pixel, ignoring any current key. */
static bool corner_color(const Overlay *o, uint32_t *rgb)
{
    GifStream g;
    if (gif_stream_open(&g, o->path) != 0) return false;
    bool ok = gif_stream_next(&g) == 1;
    const uint8_t *px = g.bgra; /* Full-size canvas: B, G, R, A of pixel (0, 0). */
    bool opaque = ok && px[3] != 0;
    if (opaque) *rgb = (uint32_t)px[2] << 16 | (uint32_t)px[1] << 8 | px[0];
    gif_stream_close(&g);
    return opaque;
}

typedef struct ScalePreview {
    int id;       /* Not a pointer: the overlay can be removed while the dialog is open. */
    POINT anchor; /* Fixed centre: re-deriving it each step would drift by rounding. */
} ScalePreview;

static POINT layout_center(const Overlay *o)
{
    SIZE s = scaled_size(o);
    POINT c = {round_px(o->pos_x) + s.cx / 2, round_px(o->pos_y) + s.cy / 2};
    return c;
}

static void preview_scale(int pct, void *ctx)
{
    ScalePreview *sp = (ScalePreview *)ctx;
    Overlay *o = find_overlay(sp->id);
    if (!o) return;
    o->scale_pct = valid_scale(pct);
    SIZE s = scaled_size(o);
    o->pos_x = sp->anchor.x - s.cx / 2;
    o->pos_y = sp->anchor.y - s.cy / 2;
    apply_geometry(o);
}

void overlay_command(Overlay *o, UINT cmd)
{
    switch (cmd) {
    case OVERLAY_CMD_PAUSE:
        if (o->still_image) return;
        o->paused = !o->paused;
        if (!o->paused) {
            o->next_frame = app_now();
            reset_tick(o);
        }
        overlay_refresh_hook();
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
        o->ct_override = -1; /* The user's choice wins over a script's. */
        apply_ex_style(o);
        overlay_refresh_hook();
        if (o->clickthrough) {
            log_info("GIF #%d: click-through enabled", o->id);
            app_notify(TXT_CLICKTHROUGH_ON_TITLE, TXT_CLICKTHROUGH_ON_TEXT);
        }
        break;
    case OVERLAY_CMD_HIDE:
        overlay_set_hidden(o, !o->hidden);
        if (o->hidden) app_notify(TXT_HIDDEN_TITLE, TXT_HIDDEN_TEXT);
        break;
    case OVERLAY_CMD_LEADER_NONE:
        overlay_unbind_leader(o, true);
        break;
    case OVERLAY_CMD_RECENTER:
        center_on_primary(o, 0);
        moved_externally(o);
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
        ScalePreview sp = {o->id, layout_center(o)};
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
    case OVERLAY_CMD_SCRIPT_LOOK:
        overlay_set_script(o, OVERLAY_BUILTINS[cmd - OVERLAY_CMD_SCRIPT_PATROL].spec);
        break;
    case OVERLAY_CMD_SCRIPT_FILE:
        app_pick_files(PICK_SCRIPT, o);
        return;
    case OVERLAY_CMD_SCRIPT_NEW_PATROL:
    case OVERLAY_CMD_SCRIPT_NEW_WANDER:
    case OVERLAY_CMD_SCRIPT_NEW_BOUNCE:
    case OVERLAY_CMD_SCRIPT_NEW_FOLLOW:
    case OVERLAY_CMD_SCRIPT_NEW_FLEE:
    case OVERLAY_CMD_SCRIPT_NEW_LOOK: {
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
    peers_changed(); /* paused / scripted / size as other scripts see them. */
    app_changed();
}

#define LEADER_ID_SPAN 100000
void overlay_leader_command(UINT cmd)
{
    if (cmd < OVERLAY_LEADER_CMD_BASE) return;
    UINT packed = cmd - OVERLAY_LEADER_CMD_BASE;
    Overlay *follower = find_overlay((int)(packed / LEADER_ID_SPAN));
    Overlay *leader = find_overlay((int)(packed % LEADER_ID_SPAN));
    if (!follower || !leader) return;
    wchar_t err[256];
    if (!overlay_bind_leader(follower, leader, err, ARRAYSIZE(err)))
        app_notify(TXT_MENU_LEADER, err);
}

void overlay_build_menu(const Overlay *o, HMENU menu, UINT base)
{
    HMENU leader = CreatePopupMenu();
    if (leader) {
        AppendMenuW(leader, MF_STRING | (o->leader ? 0 : MF_CHECKED), base + OVERLAY_CMD_LEADER_NONE,
                    TXT_MENU_LEADER_NONE);
        if (overlay_has_followers(o)) {
            AppendMenuW(leader, MF_STRING | MF_GRAYED, 0, TXT_MENU_LEADER_CHAIN);
        } else {
            for (Overlay *cand = g_app.overlays; cand; cand = cand->next) {
                if (cand == o || cand->leader) continue;
                wchar_t label[MAX_PATH + 32];
                _snwprintf_s(label, ARRAYSIZE(label), _TRUNCATE, L"%s  #%d", overlay_name(cand), cand->id);
                AppendMenuW(leader, MF_STRING | (o->leader == cand ? MF_CHECKED : 0),
                            OVERLAY_LEADER_CMD_BASE + (UINT)o->id * LEADER_ID_SPAN + (UINT)cand->id, label);
            }
        }
        AppendMenuW(menu, MF_POPUP | (o->leader ? MF_CHECKED : 0), (UINT_PTR)leader, TXT_MENU_LEADER);
    }
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
    AppendMenuW(menu, MF_STRING | (o->failed || o->still_image ? MF_GRAYED : 0),
                base + OVERLAY_CMD_PAUSE, o->paused ? TXT_MENU_RESUME : TXT_MENU_PAUSE);
    AppendMenuW(menu, MF_STRING, base + OVERLAY_CMD_REPLACE, TXT_MENU_REPLACE);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    {
        wchar_t label[32];
        _snwprintf_s(label, 32, _TRUNCATE, TXT_MENU_SCALE, o->scale_pct);
        AppendMenuW(menu, MF_STRING | (overlay_effective_hidden(o) ? MF_GRAYED : 0),
                    base + OVERLAY_CMD_SCALE, label);
    }
    if (key) AppendMenuW(menu, MF_POPUP | (o->key_enabled ? MF_CHECKED : 0), (UINT_PTR)key, TXT_MENU_COLOR_KEY);
    if (o->leader) {
        if (motion) DestroyMenu(motion);
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, TXT_MENU_SCRIPT_FOLLOWER);
    } else if (motion) {
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
    double x, y;
    peer_rect(o, &x, &y); /* The layout box: a script transform is not saved. */
    out->path = o->path;
    out->x = round_px(x);
    out->y = round_px(y);
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
    out->uid = o->uid;
    out->leader_uid = o->leader ? o->leader->uid : 0;
    out->group_uid = o->group ? o->group->uid : 0;
    out->defer_script = false;
}
