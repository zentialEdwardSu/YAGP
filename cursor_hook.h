#ifndef YAGP_CURSOR_HOOK_H
#define YAGP_CURSOR_HOOK_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdbool.h>

/* Low-level mouse hook on a thread of its own, running only while someone
 * needs it: scripts subscribed to "cursor", or click-through overlays with
 * input handlers (they get no window messages). The hook callback never runs
 * Lua: it compares the cursor with a snapshot of watched rectangles and posts
 * at most one pending message of each kind to the tray window, so a busy UI
 * thread never slows down the system's mouse (and LowLevelHooksTimeout never
 * fires because of us). */

/* wParam / lParam of WM_APP_CURSOR: none; read cursor_hook_pos(). Coalesced:
 * at most one is pending. */
#define WM_APP_CURSOR (WM_APP + 4)
/* A button / wheel / move event on a watched rectangle, read with
 * cursor_hook_take(). wParam = number of queued events (hint). */
#define WM_APP_HOOK_INPUT (WM_APP + 5)

#define CURSOR_HOOK_MAX_WATCH 64

typedef struct HookWatch {
    int id;                   /* Overlay id. */
    RECT box;                 /* Hitbox on the desktop. */
} HookWatch;

typedef enum HookKind { HOOK_MOVE, HOOK_DOWN, HOOK_UP, HOOK_WHEEL } HookKind;
typedef struct HookEvent {
    int kind;                 /* HookKind */
    int button;               /* 1 left, 2 right, 3 middle (logical, swap applied by the OS). */
    int x, y;
    int wheel;                /* WHEEL_DELTA units. */
    DWORD time;               /* Event time (ms, GetTickCount clock). */
} HookEvent;

/* Starts / stops the hook thread. want_cursor: post WM_APP_CURSOR on moves. */
void cursor_hook_configure(HWND notify, bool want_cursor, const HookWatch *watch, int n);
/* Replaces the watched rectangles (cheap; call when overlays move). */
void cursor_hook_set_watch(const HookWatch *watch, int n);
bool cursor_hook_running(void);
/* Latest cursor position seen by the hook (or GetCursorPos when not running). */
POINT cursor_hook_pos(void);
/* Pops queued input events (up to cap). Returns the count. */
int cursor_hook_take(HookEvent *out, int cap);
void cursor_hook_shutdown(void);

#endif
