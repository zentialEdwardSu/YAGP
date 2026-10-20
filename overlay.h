#ifndef YAGP_OVERLAY_H
#define YAGP_OVERLAY_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

#include "clip.h"
#include "config.h"
#include "dx11_renderer.h"
#include "gif_stream.h"
#include "input_fsm.h"
#include "script.h"
#include "transform.h"

/* Per-overlay menu commands; the tray menu offsets them by index * OVERLAY_CMD_SPAN. */
enum {
    OVERLAY_CMD_PAUSE = 1,
    OVERLAY_CMD_REPLACE,
    OVERLAY_CMD_TOPMOST,
    OVERLAY_CMD_LOCK,
    OVERLAY_CMD_CLICKTHROUGH,
    OVERLAY_CMD_RECENTER,
    OVERLAY_CMD_REMOVE,
    OVERLAY_CMD_COLOR_KEY,        /* Prompt for hex colour + tolerance. */
    OVERLAY_CMD_COLOR_KEY_CORNER, /* Key the top-left pixel's colour. */
    OVERLAY_CMD_COLOR_KEY_OFF,
    OVERLAY_CMD_SCALE,            /* Slider dialog, APP_SCALE_MIN..MAX %. */
    OVERLAY_CMD_SCRIPT_NONE,
    OVERLAY_CMD_SCRIPT_PATROL,    /* Built-in presets, in OVERLAY_BUILTINS order. */
    OVERLAY_CMD_SCRIPT_WANDER,
    OVERLAY_CMD_SCRIPT_BOUNCE,
    OVERLAY_CMD_SCRIPT_FOLLOW,
    OVERLAY_CMD_SCRIPT_FLEE,
    OVERLAY_CMD_SCRIPT_LOOK,
    OVERLAY_CMD_SCRIPT_FILE,      /* Pick a .lua file in the helper process. */
    OVERLAY_CMD_SCRIPT_RELOAD,
    OVERLAY_CMD_SCRIPT_NEW_PATROL, /* New .lua file from a preset, OVERLAY_BUILTINS order. */
    OVERLAY_CMD_SCRIPT_NEW_WANDER,
    OVERLAY_CMD_SCRIPT_NEW_BOUNCE,
    OVERLAY_CMD_SCRIPT_NEW_FOLLOW,
    OVERLAY_CMD_SCRIPT_NEW_FLEE,
    OVERLAY_CMD_SCRIPT_NEW_LOOK,
    OVERLAY_CMD_HIDE,             /* Toggle: hide (keep in the tray) / show again. */
    OVERLAY_CMD_MASTER_NONE,
    OVERLAY_CMD_SPAN = 32
};

#define OVERLAY_MASTER_CMD_BASE 2000000u

#define OVERLAY_MAX_CLIPS 32

/* Playback of the clip on screen. Frame numbers are 0-based. */
typedef struct Playback {
    int clip;             /* Index into Overlay.clips. */
    int frame;            /* Frame on screen (decoded last). */
    int from, to;         /* Range; to = -1: up to the end (count unknown until the cache is ready). */
    int loops_left;       /* < 0 = forever. */
    double speed;
    bool held;            /* Finished: holding the last frame (clip_end delivered). */
    bool playing;         /* yagp.set_playing */
    GifSnapshot range_start; /* Canvas at `from`, for jumping back in a sub-range loop. */
    bool has_snapshot;
} Playback;

typedef struct Overlay {
    struct Overlay *next;
    HWND hwnd;
    RenderSurface *surface;
    wchar_t *path;        /* The main GIF (clips[0]); relative clip paths start from its folder. */
    Clip clips[OVERLAY_MAX_CLIPS];
    int clip_count;       /* clips[0] is always "main". */
    Playback play;
    int scale_pct;
    bool paused, topmost, locked, clickthrough;
    /* Window hidden and swap chain released: no animation, no script ticks, not
     * seen by other scripts. Shown again from the tray menu. */
    bool hidden;
    struct Group *group;  /* NULL = not in a group. */
    uint64_t uid;         /* Stable config identity. */
    struct Overlay *master; /* NULL = not a slave. */
    bool still_image;     /* PNG/JPG/JPEG: animation controls are unavailable. */
    bool key_enabled;     /* Colour key applied while compositing. */
    uint32_t key_rgb;     /* 0xRRGGBB */
    int key_tolerance;    /* Per-channel, 0-255. */
    bool failed;          /* Decoder error; keeps showing the last good frame. */
    bool is_static;       /* Single frame, single pass: nothing to schedule. */
    int64_t next_frame;   /* QPC tick at which the next frame is due. */
    /* Motion script (see script.h). script_spec is "" / "builtin:<name>" / a .lua path
     * and survives failures, so "reload" and the saved config keep it. */
    Script *script;
    wchar_t *script_spec;
    bool script_failed;
    unsigned subs;        /* script_subscriptions(script), cached. */
    bool dragging;        /* Inside a user move loop: the script is suspended. */
    bool scaling;         /* Scale dialog open: the script is suspended too. */
    bool flip_x;
    double pos_x, pos_y;  /* Layout box top-left (untransformed), sub-pixel. */
    int win_x, win_y;     /* Window's actual top-left, tracked from WM_WINDOWPOSCHANGED. */
    double vx, vy;        /* px/s over the last script tick, seen by other scripts. */
    int64_t next_tick, last_tick, tick_interval;
    int64_t next_timer;   /* QPC tick of the script's earliest timer; INT64_MAX = none. */
    int id;               /* yagp.id; unique for this run. */
    int hitbox_override[4];
    bool has_override;
    char *name_u8;        /* overlay_name() as UTF-8, for other scripts' peer tables. */
    /* Script transform (window pixels of the layout box). The window is the
     * bounding box of the transform (union of both ends while animating), at
     * aabb[] relative to the layout box. */
    XformAnim xf;
    bool xf_animating;
    int aabb[4];          /* l t r b relative to pos_x/pos_y. */
    double opacity;
    int64_t xf_end;       /* QPC tick at which the animation ends (transform_end). */
    double op_from, op_to, op_t0, op_dur; /* Opacity animation (seconds, app clock). */
    int op_easing;
    /* Input. */
    InputFsm input;
    bool draggable;       /* yagp.set_draggable */
    int cursor_shape;     /* ScriptCursor */
    int ct_override;      /* yagp.set_clickthrough: -1 none, else 0/1; overrides clickthrough, not saved. */
    bool tracking;        /* TrackMouseEvent armed. */
    bool sys_drag;        /* The current move loop was started by our default left drag. */
    int64_t hover_at;     /* Click-through hover check (QPC); INT64_MAX = none. */
    int64_t next_poll;    /* Polling fallback when the mouse hook is unavailable; INT64_MAX = off. */
    POINT poll_cursor;
    int poll_buttons;     /* Bits 1 << button held at the last poll. */
    bool peer_dirty;      /* Moved / resized since the last peer_moved round. */
} Overlay;

void overlay_cache_ready(void *job);

bool overlay_register_class(HINSTANCE instance);
/* use_position=false places the window centred on the primary work area. */
Overlay *overlay_create(const OverlayConfig *cfg, bool use_position, int cascade);
void overlay_destroy(Overlay *o);

bool overlay_is_animating(const Overlay *o);
/* Has a live motion script that should be ticked (update or a timer). */
bool overlay_is_moving(const Overlay *o);
/* Earliest QPC tick the overlay needs the UI thread (frame, tick, timer,
 * transform end); INT64_MAX = none. */
int64_t overlay_deadline(const Overlay *o);
/* Runs whatever is due at now: frame, update(dt), timers, transform_end.
 * Returns true if the overlay may have moved. */
bool overlay_service(Overlay *o, int64_t now, int64_t slack);
/* An overlay moved, appeared or vanished since the last overlay_update_contacts. */
bool overlay_peers_dirty(void);
/* spec: NULL/"" = none, "builtin:patrol|wander|bounce", or an absolute .lua path.
 * Load errors are reported with app_notify; the spec is kept either way. */
void overlay_set_script(Overlay *o, const wchar_t *spec);
/* Display layout changed (monitors.c already refreshed). */
void overlay_screens_changed(Overlay *o);
/* Compares every overlay's rect with the last call and delivers on_collide /
 * on_separate, and peer_moved for overlays that moved. Call once per tick. */
void overlay_update_contacts(void);
/* peer_added / peer_removed to every other script. */
void overlay_announce(Overlay *o, bool added);
/* Hide (keep loaded) or show again. Showing re-centres a GIF whose monitor
 * has gone away meanwhile. */
void overlay_set_hidden(Overlay *o, bool hidden);
/* Re-applies the effective visibility (member hidden OR group hidden). */
void overlay_refresh_visibility(Overlay *o);

bool overlay_has_slaves(const Overlay *o);
bool overlay_bind_master(Overlay *slave, Overlay *master, wchar_t *err, size_t cap);
void overlay_unbind_master(Overlay *slave, bool reload_script);
void overlay_master_removed(Overlay *master);
void overlay_master_command(UINT cmd);

/* Device-loss recovery: drop / rebuild the swap chain on g_app.device. */
void overlay_detach_surface(Overlay *o);
bool overlay_attach_surface(Overlay *o);

/* Global mouse hook plumbing (cursor subscriptions, click-through input). */
void overlay_refresh_hook(void);
void overlay_cursor_moved(void);
void overlay_hook_input(void);

/* Swap in another GIF, keeping position, scale and settings. */
void overlay_replace(Overlay *o, const wchar_t *path);
void overlay_build_menu(const Overlay *o, HMENU menu, UINT base);
void overlay_command(Overlay *o, UINT cmd);
void overlay_to_config(const Overlay *o, OverlayConfig *out);
const wchar_t *overlay_name(const Overlay *o);

#endif
