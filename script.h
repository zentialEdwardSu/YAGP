#ifndef YAGP_SCRIPT_H
#define YAGP_SCRIPT_H

#include <stdbool.h>
#include <stddef.h>
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
    const char *name;       /* UTF-8 GIF file name, owned by the host. */
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
