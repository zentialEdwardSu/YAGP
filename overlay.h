#ifndef YAGP_OVERLAY_H
#define YAGP_OVERLAY_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "dx11_renderer.h"
#include "gif_stream.h"
#include "script.h"

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
    OVERLAY_CMD_SCRIPT_FILE,      /* Pick a .lua file in the helper process. */
    OVERLAY_CMD_SCRIPT_RELOAD,
    OVERLAY_CMD_SCRIPT_NEW_PATROL, /* New .lua file from a preset, OVERLAY_BUILTINS order. */
    OVERLAY_CMD_SCRIPT_NEW_WANDER,
    OVERLAY_CMD_SCRIPT_NEW_BOUNCE,
    OVERLAY_CMD_SCRIPT_NEW_FOLLOW,
    OVERLAY_CMD_SCRIPT_NEW_FLEE,
    OVERLAY_CMD_HIDE,             /* Toggle: hide (keep in the tray) / show again. */
    OVERLAY_CMD_SPAN = 32
};

typedef struct Overlay {
    struct Overlay *next;
    HWND hwnd;
    GifStream *gif;
    RenderSurface *surface;
    wchar_t *path;
    int scale_pct;
    bool paused, topmost, locked, clickthrough;
    /* Window hidden and swap chain released: no animation, no script ticks, not
     * seen by other scripts. Shown again from the tray menu. */
    bool hidden;
    bool key_enabled;     /* Colour key applied while compositing. */
    uint32_t key_rgb;     /* 0xRRGGBB */
    int key_tolerance;    /* Per-channel, 0-255. */
    /* Precomputed per-frame change boxes (see frame_cache.h); NULL = full uploads. */
    uint16_t (*dirty)[4];
    int dirty_count;
    struct CacheJob *cache_job; /* Build in progress on the worker thread, or NULL. */
    bool failed;          /* Decoder error; keeps showing the last good frame. */
    bool is_static;       /* Single-frame GIF: nothing to schedule. */
    int frame_index;      /* Frames decoded since the last rewind. */
    int64_t next_frame;   /* QPC tick at which the next frame is due. */
    /* Motion script (see script.h). script_spec is "" / "builtin:<name>" / a .lua path
     * and survives failures, so "reload" and the saved config keep it. */
    Script *script;
    wchar_t *script_spec;
    bool script_failed;
    bool dragging;        /* Inside a user move loop: the script is suspended. */
    bool scaling;         /* Scale dialog open: the script is suspended too. */
    bool flip_x;
    double pos_x, pos_y;  /* Sub-pixel position the script works with. */
    int win_x, win_y;     /* Window's actual top-left, tracked from WM_WINDOWPOSCHANGED. */
    double vx, vy;        /* px/s over the last script tick, seen by other scripts. */
    int64_t next_tick, last_tick, tick_interval;
    int id;               /* yagp.id; unique for this run. */
    /* Hitbox: visible area from the frame cache (GIF pixels, l t r b; empty =
     * whole window), or a script's yagp.set_hitbox override (l t r b). */
    int opaque[4];
    int hitbox_override[4];
    bool has_override;
    char *name_u8;        /* overlay_name() as UTF-8, for other scripts' peer tables. */
} Overlay;

/* Posted to g_app.tray_hwnd by the frame-cache worker; lParam is the job,
 * which the handler must pass to overlay_cache_ready. */
#define WM_APP_CACHE_READY (WM_APP + 3)
void overlay_cache_ready(void *job);

bool overlay_register_class(HINSTANCE instance);
/* use_position=false places the window centred on the primary work area. */
Overlay *overlay_create(const OverlayConfig *cfg, bool use_position, int cascade);
void overlay_destroy(Overlay *o);

bool overlay_is_animating(const Overlay *o);
/* Has a live motion script that should be ticked (not paused, not dragged). */
bool overlay_is_moving(const Overlay *o);
/* Run the script's update(dt), then advance next_tick. */
void overlay_step(Overlay *o, int64_t now);
/* spec: NULL/"" = none, "builtin:patrol|wander|bounce", or an absolute .lua path.
 * Load errors are reported with app_notify; the spec is kept either way. */
void overlay_set_script(Overlay *o, const wchar_t *spec);
/* Display layout changed (monitors.c already refreshed). */
void overlay_screens_changed(Overlay *o);
/* Compares every overlay's rect with the last call and delivers on_collide /
 * on_separate to the scripts involved. Call once per tick after the steps. */
void overlay_update_contacts(void);
/* Hide (keep loaded) or show again. Showing re-centres a GIF whose monitor
 * has gone away meanwhile. */
void overlay_set_hidden(Overlay *o, bool hidden);
/* Decode and present the next frame, then advance next_frame. */
void overlay_advance(Overlay *o, int64_t now);

/* Device-loss recovery: drop / rebuild the swap chain on g_app.device. */
void overlay_detach_surface(Overlay *o);
bool overlay_attach_surface(Overlay *o);

/* Swap in another GIF, keeping position, scale and settings. */
void overlay_replace(Overlay *o, const wchar_t *path);
void overlay_build_menu(const Overlay *o, HMENU menu, UINT base);
void overlay_command(Overlay *o, UINT cmd);
void overlay_to_config(const Overlay *o, OverlayConfig *out);
const wchar_t *overlay_name(const Overlay *o);

#endif
