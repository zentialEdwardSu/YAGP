#ifndef YAGP_APP_H
#define YAGP_APP_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

#include "dx11_renderer.h"

typedef struct Overlay Overlay;

/* Process-wide state. Everything runs on the single UI thread. */
typedef struct App {
    HINSTANCE instance;
    HWND tray_hwnd;          /* Hidden owner window for the tray icon and menus. */
    HICON tray_icon;
    UINT msg_taskbar_created;
    RenderDevice *device;
    Overlay *overlays;       /* Singly linked list, in creation order. */
    HANDLE frame_timer;      /* Waitable timer armed for the earliest due frame. */
    int64_t qpc_freq;
    int modal_depth;         /* >0 while a menu, dialog or drag runs its own loop. */
    bool persist;            /* false after the user deleted %APPDATA%\YAGP. */
    wchar_t cache_dir[MAX_PATH]; /* %LOCALAPPDATA%\YAGP\cache; empty = do not write. */
    /* The open-file dialog runs in a short-lived helper process (yagp.exe --pick)
     * so the shell DLLs and extensions it pulls in never enter this process. */
    HANDLE pick_process;     /* Running helper, or NULL. */
    DWORD pick_pid;
    uint64_t pick_id;        /* Matches the helper's result message; 0 once consumed. */
    int pick_kind;           /* PickKind */
    Overlay *pick_target;    /* Overlay to replace, or NULL. */
    int next_overlay_id;     /* Source of Overlay.id (yagp.id), never reused in a run. */
} App;

extern App g_app;

int64_t app_now(void);
/* State changed: re-arm the frame timer and schedule a config save. */
void app_changed(void);
void app_reschedule(void);
/* Unlinks and frees o. o must not be touched afterwards. */
void app_remove_overlay(Overlay *o);
/* Keep animations running while a nested modal loop (menu, drag, dialog) blocks ours. */
void app_modal_begin(void);
void app_modal_end(void);
/* Shows a popup menu owned by the tray window and returns the chosen command (0 = none). */
UINT app_track_menu(HMENU menu, int x, int y);
typedef enum PickKind { PICK_ADD, PICK_REPLACE, PICK_SCRIPT, PICK_NEW_SCRIPT } PickKind;
/* Opens the file dialog in a helper process and returns immediately. Results
 * arrive later: PICK_ADD adds every chosen file, PICK_REPLACE replaces target,
 * PICK_SCRIPT binds the chosen .lua file to target. */
void app_pick_files(PickKind kind, Overlay *target);
/* Save dialog in the helper process: writes the built-in preset `resource`
 * (RCDATA name) to the chosen file, opens it in an editor, and binds it to
 * target once the result arrives (PICK_NEW_SCRIPT). */
void app_new_script(Overlay *target, const wchar_t *resource, const wchar_t *file_name);
void app_notify(const wchar_t *title, const wchar_t *text);
/* Modal "hex colour + tolerance" prompt. Returns false on cancel. */
bool app_prompt_color_key(uint32_t *rgb, int *tolerance);

#define APP_SCALE_MIN 5
#define APP_SCALE_MAX 250
/* Modal slider (APP_SCALE_MIN..MAX %). preview is called on every change; on
 * cancel it is called once more with the original value. */
bool app_prompt_scale(int *pct, void (*preview)(int pct, void *ctx), void *ctx);
void app_error(const wchar_t *fmt, const wchar_t *arg);

#endif
