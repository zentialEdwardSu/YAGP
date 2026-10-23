#ifndef YAGP_SCRIPT_H
#define YAGP_SCRIPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

/* One sandboxed Lua state driving one overlay. The host (overlay.c, or the
 * test harness) supplies the window through ScriptHostOps, so this module has
 * no dependency on Overlay. Coordinates are physical pixels, window top-left. */
typedef struct ScriptMouse {
    int x, y;                  /* Physical pixels, virtual-desktop coordinates. */
    bool left, right, middle;  /* Logical buttons (swap-buttons setting applied). */
} ScriptMouse;

/* Another overlay as seen by a script: read-only, refreshed on every query. */
typedef struct ScriptPeer {
    int id;                 /* Unique and stable for this run (not persisted). */
    double x, y;            /* Top-left, physical pixels. */
    int w, h;               /* Size including scale. */
    double vx, vy;          /* px/s over its last script tick; 0 without a script. */
    double hx, hy;          /* Hitbox (visible area) top-left on the desktop. */
    int hw, hh;
    bool scripted, paused;
    bool still;             /* PNG/JPG/JPEG, no animation. */
    bool follower;        /* Follower of another overlay. */
    bool leader;            /* Has at least one follower. */
    int leader_id;          /* 0 unless follower. */
    const char *name;       /* UTF-8 media file name, owned by the host. */
} ScriptPeer;

#define SCRIPT_MAX_PEERS 64

typedef struct ScriptHostOps {
    void (*get_rect)(void *ctx, double *x, double *y, int *w, int *h);
    void (*set_pos)(void *ctx, double x, double y);
    void (*set_flip)(void *ctx, bool flip_x);
    void (*set_rate)(void *ctx, int hz); /* Already clamped to SCRIPT_RATE_MIN..MAX. */
    /* False if the cursor cannot be read (e.g. secure desktop / lock screen);
     * scripts then see the last known position with valid = false. */
    bool (*get_mouse)(void *ctx, ScriptMouse *m);
    /* Every other overlay (not ctx itself), at most cap. Returns the count. */
    int (*get_peers)(void *ctx, ScriptPeer *out, int cap);
    /* Hitbox relative to the window's top-left, display pixels (scale and flip
     * applied). NULL = the whole window. */
    void (*get_hitbox)(void *ctx, int *dx, int *dy, int *w, int *h);
    /* Override in GIF pixels: box = x, y, w, h; NULL = back to automatic. */
    void (*set_hitbox)(void *ctx, const int *box);
} ScriptHostOps;

/* ---- events a script can subscribe to with yagp.on(name, fn [, opts]) ---- */
typedef enum ScriptEvent {
    /* Input on the overlay (routed: a handler returning true or setting
     * e.handled stops the default behaviour). */
    SEV_MOUSE_DOWN, SEV_MOUSE_UP, SEV_CLICK, SEV_DOUBLE_CLICK,
    SEV_MOUSE_ENTER, SEV_MOUSE_LEAVE, SEV_HOVER, SEV_MOUSE_MOVE, SEV_WHEEL,
    SEV_DRAG_START, SEV_DRAG, SEV_DRAG_END,
    /* Everything else. */
    SEV_CURSOR, SEV_PEER_MOVED, SEV_PEER_ADDED, SEV_PEER_REMOVED,
    SEV_COLLIDE, SEV_SEPARATE, SEV_MOVED, SEV_SCREENS_CHANGED,
    SEV_FRAME, SEV_CLIP_END, SEV_TRANSFORM_END,
    SEV_COUNT
} ScriptEvent;
#define SEV_BIT(e) (1u << (e))
#define SEV_INPUT_MASK (SEV_BIT(SEV_DRAG_END + 1) - 1u)

typedef enum ScriptButton { SBTN_NONE, SBTN_LEFT, SBTN_RIGHT, SBTN_MIDDLE } ScriptButton;

typedef struct ScriptInput {
    int button;            /* ScriptButton */
    int x, y;              /* Cursor, desktop physical pixels. */
    double gx, gy;         /* Same point in GIF pixels (transform, scale and flip undone). */
    int dx, dy;            /* drag: movement since the previous drag event. */
    double wheel;          /* wheel: notches, positive = away from the user. */
    bool opaque;           /* The pixel under the cursor is not transparent. */
    bool shift, ctrl, alt;
    bool passthrough;      /* Observed on a click-through overlay: cannot be handled. */
} ScriptInput;

/* ---- clips and playback (yagp.clip / play / seek ...) ---------------------- */
#define SCRIPT_CLIP_NAME_MAX 32
typedef struct ScriptClipInfo {
    int w, h;
    int frames;              /* 0 until the frame index is ready. */
    double duration;         /* Seconds per loop; 0 until ready. */
    const uint32_t *delays;  /* frames entries (ms), owned by the host; NULL until ready. */
} ScriptClipInfo;

typedef enum ScriptAnchor { SANCHOR_BOTTOM, SANCHOR_CENTER, SANCHOR_TOP_LEFT } ScriptAnchor;

typedef struct ScriptPlay {
    int from, to;          /* 0-based frame range, to inclusive; -1 = clip start / end. */
    int loops;             /* < 0 = forever, else plays this many times then holds the last frame. */
    double speed;          /* 0.1 .. 10 */
    int anchor;            /* ScriptAnchor, used when the size changes. */
    bool sync;             /* Keep the playback position (as a fraction of the range). */
    bool restart;          /* false: same clip already playing -> only update the options. */
} ScriptPlay;

/* Script transform, row-vector convention like D2D: a point (x, y) of the
 * untransformed window (display pixels, relative to its top-left) maps to
 * (x*m[0] + y*m[2] + m[4], x*m[1] + y*m[3] + m[5]). */
typedef enum ScriptEasing { SEASE_LINEAR, SEASE_IN, SEASE_OUT, SEASE_IN_OUT } ScriptEasing;
typedef enum ScriptCursor { SCURSOR_MOVE, SCURSOR_ARROW, SCURSOR_HAND, SCURSOR_NONE } ScriptCursor;

/* Direct followers of the script's overlay, plus target state used by the
 * chainable follower handles. The host returns an Overlay* as `target`. */
typedef struct ScriptFollowerOps {
    int (*list)(void *leader_ctx, int *ids, int cap);
    void *(*resolve)(void *leader_ctx, int id);
    const char *(*name)(void *leader_ctx, int id);
    bool (*get_hidden)(void *target);
    bool (*get_paused)(void *target);
    bool (*is_static)(void *target);
} ScriptFollowerOps;

typedef struct ScriptHostExtOps {
    /* Clips. Errors are UTF-8 messages for the script (returned as nil, err). */
    bool (*load_clip)(void *ctx, const char *name, const char *relpath, char *err, size_t cap);
    void (*unload_clip)(void *ctx, const char *name);
    bool (*clip_info)(void *ctx, const char *name, ScriptClipInfo *out);
    bool (*play)(void *ctx, const char *name, const ScriptPlay *p, char *err, size_t cap);
    bool (*seek)(void *ctx, int frame);  /* 0-based, in the current clip. */
    /* Current clip name (host-owned), 0-based frame, frame count (0 = unknown). */
    void (*get_frame)(void *ctx, const char **name, int *frame, int *frames);
    void (*set_speed)(void *ctx, double speed);
    void (*set_playing)(void *ctx, bool playing);
    /* Composition. m = NULL resets. duration in seconds, 0 = immediate. */
    bool (*set_matrix)(void *ctx, const double *m, double duration, int easing);
    void (*set_opacity)(void *ctx, double opacity, double duration, int easing);
    /* Input behaviour. */
    void (*set_cursor)(void *ctx, int shape);
    void (*set_draggable)(void *ctx, bool draggable);
    void (*set_clickthrough)(void *ctx, bool clickthrough);
    void (*set_locked)(void *ctx, bool locked);
    void (*set_topmost)(void *ctx, bool topmost);
    /* The set of subscribed events changed (SEV_BIT mask), e.g. to start or
     * stop a mouse hook. Called from inside Lua: must not call back into the script. */
    void (*subs_changed)(void *ctx, unsigned mask);
    /* Monotonic seconds for timers; NULL = QueryPerformanceCounter. */
    double (*now)(void *ctx);
    /* Shows a tray notification. title/text are UTF-8 and already bounded by
     * the script host. Returns false when the host cannot show one. NULL =
     * notifications are unavailable (e.g. script_create without ext). */
    bool (*notify)(void *ctx, const char *title, const char *text);
    /* Still image: animation controls are unavailable. */
    bool (*is_static)(void *ctx);
    void (*set_hidden)(void *ctx, bool hidden);
    void (*set_paused)(void *ctx, bool paused);
    const ScriptFollowerOps *followers;
} ScriptHostExtOps;

typedef struct Script Script;

#define SCRIPT_RATE_MIN 1
#define SCRIPT_RATE_MAX 144
#define SCRIPT_RATE_DEFAULT 60

/* Compiles and runs the chunk's top level (text only, bytecode is rejected).
 * chunkname (UTF-8, e.g. "bounce.lua") prefixes error messages; name (UTF-8,
 * the GIF's file name) becomes yagp.name and prefixes print output; id becomes
 * yagp.id. On failure returns NULL and writes a message to err. */
Script *script_create(const ScriptHostOps *ops, void *ctx, const char *chunk, size_t len,
                      const char *chunkname, const char *name, int id, wchar_t *err, size_t err_cap);
void script_destroy(Script *s);
/* Handlers (update, on_moved, ...) are raw fields of the global table: a
 * metatable on _G (e.g. a strict-mode __index) is never consulted, so a
 * missing handler is simply skipped. Every call into Lua is protected: no
 * script, however broken, can abort the process. */

/* script_create plus the extended host API (clips, transforms, input
 * behaviour). ext may be NULL: the corresponding yagp functions then exist but
 * do nothing or return nil. ext must outlive the script. */
Script *script_create_ex(const ScriptHostOps *ops, const ScriptHostExtOps *ext, void *ctx, const char *chunk,
                         size_t len, const char *chunkname, const char *name, int id, wchar_t *err,
                         size_t err_cap);

/* SEV_BIT mask of events with at least one handler, including the legacy
 * globals (on_moved, on_collide, ...). Cached; refreshed after every call. */
unsigned script_subscriptions(Script *s);
/* Calls every handler of ev with the input table. handled = some handler
 * returned true or set e.handled (ignored when in->passthrough). */
bool script_input(Script *s, ScriptEvent ev, const ScriptInput *in, bool *handled, wchar_t *err, size_t err_cap);
/* cursor moved (handlers filter by min_move / radius themselves). */
bool script_cursor(Script *s, int x, int y, bool valid, wchar_t *err, size_t err_cap);
/* peer_moved / peer_added / peer_removed (removed: only id is valid). */
bool script_peer_event(Script *s, ScriptEvent ev, const ScriptPeer *p, wchar_t *err, size_t err_cap);
/* frame (0-based frame just shown, clip name) and clip_end (clip name). */
bool script_clip_event(Script *s, ScriptEvent ev, const char *clip, int frame, wchar_t *err, size_t err_cap);
/* moved / screens_changed / transform_end: calls yagp.on handlers and the
 * legacy global (on_moved, on_screens_changed). */
bool script_plain_event(Script *s, ScriptEvent ev, wchar_t *err, size_t err_cap);

/* Timers (yagp.after / every). Seconds on the ext->now clock; < 0 = none. */
double script_next_timer(Script *s);
/* Runs every timer due at now. */
bool script_run_timers(Script *s, double now, wchar_t *err, size_t err_cap);

/* True if the script defines a global update(dt). Cached, so cheap. */
bool script_has_update(Script *s);
/* Calls update(dt). Returns false with a message on any error. */
bool script_update(Script *s, double dt, wchar_t *err, size_t err_cap);
/* Calls the global fn() if it exists ("on_moved", "on_screens_changed"). */
bool script_event(Script *s, const char *fn, wchar_t *err, size_t err_cap);
/* Calls the global fn(other) if it exists ("on_collide", "on_separate"), with
 * other as a peer table (same fields as yagp.others()). */
bool script_event_peer(Script *s, const char *fn, const ScriptPeer *other, wchar_t *err, size_t err_cap);

#endif
