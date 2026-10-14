#include "app.h"
#include "config.h"
#include "frame_cache.h"
#include "i18n.h"
#include "monitors.h"
#include "overlay.h"

#include <commdlg.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#define TRAY_CLASS L"YAGP.Tray"
#define INSTANCE_MUTEX L"Local\\YAGP.SingleInstance"
#define COPYDATA_ADD_FILES 0x59414750u   /* 'YAGP': paths from a second instance */
#define COPYDATA_PICK_RESULT 0x5941504Bu /* 'YAPK': paths from the --pick helper */
#define WM_TRAY (WM_APP + 1)
#define TRAY_UID 1

enum { TIMER_MODAL_TICK = 1, TIMER_SAVE, TIMER_DEVICE_RETRY };
enum {
    TRAY_CMD_ADD = 1,
    TRAY_CMD_PAUSE_ALL,
    TRAY_CMD_RESUME_ALL,
    TRAY_CMD_CLEAR_CONFIG,
    TRAY_CMD_AUTOSTART,
    TRAY_CMD_EXIT,
    TRAY_CMD_OVERLAY_BASE = 1000
};

App g_app;
static int s_cascade;

/* ---- timing -------------------------------------------------------------- */

int64_t app_now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void app_reschedule(void)
{
    int64_t earliest = INT64_MAX;
    for (Overlay *o = g_app.overlays; o; o = o->next) {
        if (overlay_is_animating(o) && o->next_frame < earliest) earliest = o->next_frame;
        if (overlay_is_moving(o) && o->next_tick < earliest) earliest = o->next_tick;
    }
    if (earliest == INT64_MAX) {
        /* Nothing animating: the thread blocks until a message arrives. */
        CancelWaitableTimer(g_app.frame_timer);
        return;
    }
    int64_t wait = earliest - app_now();
    if (wait < 0) wait = 0;
    LARGE_INTEGER due;
    /* Negative = relative, in 100 ns units. -1 still fires immediately. */
    due.QuadPart = -(wait * 10000000 / g_app.qpc_freq) - 1;
    SetWaitableTimer(g_app.frame_timer, &due, 0, NULL, NULL, FALSE);
}

static void recover_device(void)
{
    for (Overlay *o = g_app.overlays; o; o = o->next) overlay_detach_surface(o);
    render_device_destroy(g_app.device);
    g_app.device = render_device_create();
    if (!g_app.device) {
        SetTimer(g_app.tray_hwnd, TIMER_DEVICE_RETRY, 2000, NULL);
        return;
    }
    KillTimer(g_app.tray_hwnd, TIMER_DEVICE_RETRY);
    for (Overlay *o = g_app.overlays; o; o = o->next) {
        overlay_attach_surface(o);
        o->next_frame = app_now();
    }
}

static void app_tick(void)
{
    if (!g_app.device || render_device_lost(g_app.device)) {
        recover_device();
        app_reschedule();
        return;
    }
    int64_t now = app_now();
    int64_t slack = g_app.qpc_freq / 1000;
    bool moved = false;
    for (Overlay *o = g_app.overlays; o; o = o->next) {
        if (overlay_is_animating(o) && o->next_frame - now <= slack) overlay_advance(o, now);
        if (overlay_is_moving(o) && o->next_tick - now <= slack) {
            overlay_step(o, now);
            moved = true;
        }
        if (o->dragging) moved = true;
    }
    /* Only when something moved, so an idle tray still does no work. */
    if (moved) overlay_update_contacts();
    if (render_device_lost(g_app.device)) recover_device();
    app_reschedule();
}

/* Menus, dialogs and window drags run nested message loops that bypass our
 * MsgWaitForMultipleObjects; a coarse USER timer keeps animations moving. */
void app_modal_begin(void)
{
    if (g_app.modal_depth++ == 0) SetTimer(g_app.tray_hwnd, TIMER_MODAL_TICK, USER_TIMER_MINIMUM, NULL);
}

void app_modal_end(void)
{
    if (g_app.modal_depth > 0 && --g_app.modal_depth == 0) KillTimer(g_app.tray_hwnd, TIMER_MODAL_TICK);
}

/* ---- persistence --------------------------------------------------------- */

static void save_now(void)
{
    KillTimer(g_app.tray_hwnd, TIMER_SAVE);
    if (!g_app.persist) return;
    int count = 0;
    for (Overlay *o = g_app.overlays; o; o = o->next) ++count;
    OverlayConfig *entries = count ? (OverlayConfig *)malloc((size_t)count * sizeof(OverlayConfig)) : NULL;
    if (count && !entries) return;
    int i = 0;
    for (Overlay *o = g_app.overlays; o; o = o->next) overlay_to_config(o, &entries[i++]);
    config_save(entries, count);
    free(entries);
}

void app_changed(void)
{
    app_reschedule();
    /* Debounce: drags and menu toggles can arrive in bursts. */
    if (g_app.persist) SetTimer(g_app.tray_hwnd, TIMER_SAVE, 1000, NULL);
}

/* ---- overlays ------------------------------------------------------------ */

static void link_overlay(Overlay *o)
{
    Overlay **tail = &g_app.overlays;
    while (*tail) tail = &(*tail)->next;
    *tail = o;
}

static void add_overlay_from_path(const wchar_t *path, void *ctx)
{
    (void)ctx;
    OverlayConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.path = path;
    cfg.scale_pct = 100;
    cfg.topmost = true;
    cfg.key_tolerance = 16;
    Overlay *o = overlay_create(&cfg, false, s_cascade++);
    if (!o) {
        app_error(TXT_ERR_OPEN_GIF_ADD, path);
        return;
    }
    link_overlay(o);
    app_changed();
}

static void restore_overlay(const OverlayConfig *cfg, void *ctx)
{
    (void)ctx;
    if (GetFileAttributesW(cfg->path) == INVALID_FILE_ATTRIBUTES) return; /* File moved away. */
    Overlay *o = overlay_create(cfg, true, s_cascade++);
    if (o) link_overlay(o);
}

void app_remove_overlay(Overlay *o)
{
    if (g_app.pick_target == o) { /* A pending "replace" for it becomes meaningless. */
        g_app.pick_target = NULL;
        g_app.pick_id = 0;
    }
    for (Overlay **p = &g_app.overlays; *p; p = &(*p)->next) {
        if (*p == o) {
            *p = o->next;
            overlay_destroy(o);
            break;
        }
    }
    app_changed();
}

static void destroy_all_overlays(void)
{
    while (g_app.overlays) {
        Overlay *o = g_app.overlays;
        g_app.overlays = o->next;
        overlay_destroy(o);
    }
}

/* ---- shell helpers ------------------------------------------------------- */

UINT app_track_menu(HMENU menu, int x, int y)
{
    /* Required so the menu closes when the user clicks elsewhere (KB135788). */
    SetForegroundWindow(g_app.tray_hwnd);
    app_modal_begin();
    UINT cmd = (UINT)TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                      x, y, g_app.tray_hwnd, NULL);
    app_modal_end();
    PostMessageW(g_app.tray_hwnd, WM_NULL, 0, 0);
    return cmd;
}

/* ---- file picker helper process -------------------------------------------
 * GetOpenFileName loads ~100 shell DLLs, third-party shell extensions and ~20
 * threads, and none of it is unloaded afterwards (measured: private memory
 * 3 MB -> 18 MB, working set 16 MB -> 72 MB). The dialog therefore runs in
 * "yagp.exe --pick <add|replace|script> <id>" (or "--pick newscript <id>
 * <RESOURCE> <file name>" for "new script from preset"), which sends the chosen
 * paths back with
 * WM_COPYDATA and exits. The tray process stays small and keeps animating
 * while the dialog is open. */

static void pick_close(void)
{
    if (g_app.pick_process) CloseHandle(g_app.pick_process);
    g_app.pick_process = NULL;
    g_app.pick_pid = 0;
}

/* Brings an already open picker to the front instead of opening a second one. */
static BOOL CALLBACK focus_pick_window(HWND hwnd, LPARAM pid)
{
    DWORD owner = 0;
    GetWindowThreadProcessId(hwnd, &owner);
    if (owner == (DWORD)pid && IsWindowVisible(hwnd)) {
        SetForegroundWindow(hwnd);
        return FALSE;
    }
    return TRUE;
}

static const wchar_t *pick_verb(PickKind kind)
{
    switch (kind) {
    case PICK_REPLACE: return L"replace";
    case PICK_SCRIPT: return L"script";
    case PICK_NEW_SCRIPT: return L"newscript";
    default: return L"add";
    }
}

/* extra: further quoted arguments for the helper, or NULL. */
static void launch_picker(PickKind kind, Overlay *target, const wchar_t *extra)
{
    if (g_app.pick_process && WaitForSingleObject(g_app.pick_process, 0) == WAIT_TIMEOUT) {
        if ((int)kind == g_app.pick_kind && target == g_app.pick_target) {
            AllowSetForegroundWindow(g_app.pick_pid);
            EnumWindows(focus_pick_window, (LPARAM)g_app.pick_pid);
            return;
        }
        TerminateProcess(g_app.pick_process, 0); /* A different request supersedes it. */
    }
    pick_close();

    wchar_t exe[MAX_PATH * 4], cmd[MAX_PATH * 4 + 256];
    DWORD n = GetModuleFileNameW(NULL, exe, ARRAYSIZE(exe));
    if (!n || n >= ARRAYSIZE(exe)) return;
    /* Unguessable-enough id so stale or forged results are ignored. */
    uint64_t id = (uint64_t)app_now() * 6364136223846793005ull ^ (GetTickCount64() << 21) ^
                  (uint64_t)GetCurrentProcessId();
    if (!id) id = 1;
    _snwprintf_s(cmd, ARRAYSIZE(cmd), _TRUNCATE, L"\"%s\" --pick %s %016llx%s%s", exe, pick_verb(kind),
                 (unsigned long long)id, extra ? L" " : L"", extra ? extra : L"");

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        app_error(TXT_ERR_PICKER, exe);
        return;
    }
    /* We are foreground (the user just clicked our menu), so we may pass that on. */
    AllowSetForegroundWindow(pi.dwProcessId);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    g_app.pick_process = pi.hProcess;
    g_app.pick_pid = pi.dwProcessId;
    g_app.pick_id = id;
    g_app.pick_kind = (int)kind;
    g_app.pick_target = target;
}

void app_pick_files(PickKind kind, Overlay *target)
{
    launch_picker(kind, target, NULL);
}

void app_new_script(Overlay *target, const wchar_t *resource, const wchar_t *file_name)
{
    wchar_t extra[160];
    _snwprintf_s(extra, ARRAYSIZE(extra), _TRUNCATE, L"%s \"%s\"", resource, file_name);
    launch_picker(PICK_NEW_SCRIPT, target, extra);
}

/* Payload: uint64 id, then NUL separated full paths, double-NUL terminated. */
static void handle_pick_result(const COPYDATASTRUCT *cds)
{
    if (cds->cbData < sizeof(uint64_t) + 2 * sizeof(wchar_t) || (cds->cbData - sizeof(uint64_t)) % 2) return;
    uint64_t id;
    memcpy(&id, cds->lpData, sizeof(id));
    if (!g_app.pick_id || id != g_app.pick_id) return;
    g_app.pick_id = 0; /* One result per pick. */
    const wchar_t *p = (const wchar_t *)((const uint8_t *)cds->lpData + sizeof(uint64_t));
    const wchar_t *end = p + (cds->cbData - sizeof(uint64_t)) / sizeof(wchar_t);
    if (end[-1] != 0) return;
    if (g_app.pick_kind == PICK_REPLACE) {
        if (g_app.pick_target && *p) overlay_replace(g_app.pick_target, p);
        return;
    }
    if (g_app.pick_kind == PICK_SCRIPT || g_app.pick_kind == PICK_NEW_SCRIPT) {
        if (g_app.pick_target && *p) {
            overlay_set_script(g_app.pick_target, p);
            app_changed();
        }
        return;
    }
    for (; p < end && *p; p += wcslen(p) + 1) add_overlay_from_path(p, NULL);
}

/* Sends "id + path\0\0" to the tray. */
static void send_pick_result(uint64_t id, const wchar_t *paths, size_t used)
{
    HWND tray = FindWindowW(TRAY_CLASS, NULL);
    if (!tray) return;
    size_t bytes = sizeof(uint64_t) + used * sizeof(wchar_t);
    uint8_t *payload = (uint8_t *)malloc(bytes);
    if (!payload) return;
    memcpy(payload, &id, sizeof(id));
    memcpy(payload + sizeof(id), paths, used * sizeof(wchar_t));
    COPYDATASTRUCT cds = {COPYDATA_PICK_RESULT, (DWORD)bytes, payload};
    DWORD_PTR result = 0;
    SendMessageTimeoutW(tray, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 5000, &result);
    free(payload);
}

/* Opens path for editing. Never the "open" verb: a .lua file may be associated
 * with an interpreter, which would run the script instead. */
static void edit_file(const wchar_t *path)
{
    if ((INT_PTR)ShellExecuteW(NULL, L"edit", path, NULL, NULL, SW_SHOWNORMAL) > 32) return;
    wchar_t args[MAX_PATH * 4 + 4];
    _snwprintf_s(args, ARRAYSIZE(args), _TRUNCATE, L"\"%s\"", path);
    ShellExecuteW(NULL, L"open", L"notepad.exe", args, NULL, SW_SHOWNORMAL);
}

/* "--pick newscript <id> <RESOURCE> <file name>": save dialog, write the
 * preset's source, open it in an editor, report the path. */
static int run_new_script(int argc, wchar_t **argv, uint64_t id)
{
    if (argc != 6) return 2;
    HRSRC res = FindResourceW(NULL, argv[4], MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL mem = res ? LoadResource(NULL, res) : NULL;
    const void *data = mem ? LockResource(mem) : NULL;
    DWORD size = res ? SizeofResource(NULL, res) : 0;
    if (!data || !size) return 1;

    wchar_t path[MAX_PATH * 4];
    wcsncpy_s(path, ARRAYSIZE(path), argv[5], _TRUNCATE);
    wchar_t docs[MAX_PATH] = L"";
    PWSTR known = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Documents, 0, NULL, &known))) {
        wcsncpy_s(docs, ARRAYSIZE(docs), known, _TRUNCATE);
        CoTaskMemFree(known);
    }
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = TXT_FILTER_LUA_ONLY;
    ofn.lpstrDefExt = L"lua";
    ofn.lpstrFile = path;
    ofn.nMaxFile = ARRAYSIZE(path);
    ofn.lpstrInitialDir = docs[0] ? docs : NULL;
    ofn.lpstrTitle = TXT_TITLE_NEW_SCRIPT;
    ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY | OFN_NOREADONLYRETURN;
    if (!GetSaveFileNameW(&ofn)) return 0; /* Cancelled. */

    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD wrote = 0;
    bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, data, size, &wrote, NULL) && wrote == size;
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (!ok) {
        DeleteFileW(path);
        MessageBoxW(NULL, path, TXT_ERR_WRITE_SCRIPT, MB_OK | MB_ICONWARNING);
        return 1;
    }
    edit_file(path);
    size_t used = wcslen(path) + 2; /* "path\0\0" */
    wchar_t *out = (wchar_t *)calloc(used, sizeof(wchar_t));
    if (!out) return 1;
    memcpy(out, path, (used - 2) * sizeof(wchar_t));
    send_pick_result(id, out, used);
    free(out);
    return 0;
}

/* Entry point of the helper process. Returns the process exit code. */
static int run_picker(int argc, wchar_t **argv)
{
    if (argc >= 4 && wcscmp(argv[2], L"newscript") == 0) {
        HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        int rc = run_new_script(argc, argv, wcstoull(argv[3], NULL, 16));
        if (SUCCEEDED(com)) CoUninitialize();
        return rc;
    }
    if (argc != 4) return 2;
    bool multi = wcscmp(argv[2], L"add") == 0;
    bool script = wcscmp(argv[2], L"script") == 0;
    uint64_t id = wcstoull(argv[3], NULL, 16);
    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    const DWORD cap = 32768;
    wchar_t *buf = (wchar_t *)calloc(cap, sizeof(wchar_t));
    wchar_t *out = (wchar_t *)calloc((size_t)cap * 2, sizeof(wchar_t));
    int rc = 1;
    if (!buf || !out) goto done;
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = script ? TXT_FILTER_LUA : TXT_FILTER_GIF;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = cap;
    ofn.lpstrTitle = multi ? TXT_TITLE_ADD_GIF : script ? TXT_TITLE_PICK_SCRIPT : TXT_TITLE_REPLACE_GIF;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY |
                (multi ? OFN_ALLOWMULTISELECT : 0);
    if (!GetOpenFileNameW(&ofn)) { rc = 0; goto done; } /* Cancelled. */

    /* Normalise both dialog result shapes into "full\0full\0\0". */
    size_t used = 0, out_cap = (size_t)cap * 2;
    const wchar_t *first = buf, *name = buf + wcslen(buf) + 1;
    if (!multi || !*name) {
        size_t len = wcslen(first);
        if (len + 2 > out_cap) goto done;
        memcpy(out, first, (len + 1) * sizeof(wchar_t));
        used = len + 1;
    } else {
        for (; *name; name += wcslen(name) + 1) {
            int len = _snwprintf_s(out + used, out_cap - used - 1, _TRUNCATE, L"%s\\%s", first, name);
            if (len <= 0) break;
            used += (size_t)len + 1;
        }
    }
    out[used++] = 0;

    send_pick_result(id, out, used);
    rc = 0;
done:
    free(buf);
    free(out);
    if (SUCCEEDED(com)) CoUninitialize();
    return rc;
}

static void tray_icon_data(NOTIFYICONDATAW *nid)
{
    memset(nid, 0, sizeof(*nid));
    nid->cbSize = sizeof(*nid);
    nid->hWnd = g_app.tray_hwnd;
    nid->uID = TRAY_UID;
}

void app_notify(const wchar_t *title, const wchar_t *text)
{
    NOTIFYICONDATAW nid;
    tray_icon_data(&nid);
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    wcsncpy_s(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), title, _TRUNCATE);
    wcsncpy_s(nid.szInfo, ARRAYSIZE(nid.szInfo), text, _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void app_error(const wchar_t *fmt, const wchar_t *arg)
{
    wchar_t text[1024];
    _snwprintf_s(text, ARRAYSIZE(text), _TRUNCATE, fmt, arg);
    app_modal_begin();
    MessageBoxW(g_app.tray_hwnd, text, L"YAGP", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    app_modal_end();
}

/* app.ico (resource 1, from specturebot.png) at the small-icon size, so the
 * tray picks the closest pre-scaled image instead of shrinking the 256 px one. */
static HICON make_tray_icon(void)
{
    int size = GetSystemMetrics(SM_CXSMICON);
    HICON icon = (HICON)LoadImageW(g_app.instance, MAKEINTRESOURCEW(1), IMAGE_ICON, size, size, LR_DEFAULTCOLOR);
    return icon ? icon : LoadIconW(NULL, IDI_APPLICATION);
}

static void tray_add_icon(void)
{
    NOTIFYICONDATAW nid;
    tray_icon_data(&nid);
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_app.tray_icon;
    wcsncpy_s(nid.szTip, ARRAYSIZE(nid.szTip), TXT_TRAY_TIP, _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

static void tray_remove_icon(void)
{
    NOTIFYICONDATAW nid;
    tray_icon_data(&nid);
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

/* ---- tray menu ----------------------------------------------------------- */

/* ---- autostart -------------------------------------------------------------
 * Per-user Run key: no elevation needed, and Task Manager's Startup tab can
 * disable it. The value holds just the quoted exe path, so the autostarted
 * instance starts exactly like a normal launch and restores the saved GIFs. */

#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE L"YAGP"

static bool autostart_command(wchar_t *out, DWORD cap)
{
    wchar_t exe[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(NULL, exe, ARRAYSIZE(exe));
    if (!n || n >= ARRAYSIZE(exe)) return false;
    return _snwprintf_s(out, cap, _TRUNCATE, L"\"%s\"", exe) > 0;
}

/* Enabled only if the entry points at this exe; a stale entry from a moved
 * copy shows as off, and turning it on rewrites it. */
static bool autostart_enabled(void)
{
    wchar_t want[MAX_PATH * 4 + 2], have[MAX_PATH * 4 + 2];
    DWORD bytes = sizeof(have);
    if (!autostart_command(want, ARRAYSIZE(want))) return false;
    if (RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE, RRF_RT_REG_SZ, NULL, have, &bytes) != ERROR_SUCCESS)
        return false;
    return _wcsicmp(want, have) == 0;
}

static void autostart_set(bool enable)
{
    LSTATUS rc;
    if (enable) {
        wchar_t cmd[MAX_PATH * 4 + 2];
        if (!autostart_command(cmd, ARRAYSIZE(cmd))) return;
        rc = RegSetKeyValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE, REG_SZ, cmd,
                             (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
    } else {
        rc = RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    if (rc != ERROR_SUCCESS) app_error(TXT_ERR_AUTOSTART, L"HKCU\\...\\Run");
}

/* ---- delete all data --------------------------------------------------------
 * Removes everything YAGP leaves on the machine: config, frame cache and the
 * autostart entry (whatever exe it points at). */
static void clear_config(void)
{
    wchar_t dir[MAX_PATH];
    wchar_t text[MAX_PATH * 2 + 384];
    if (!config_dir(dir, MAX_PATH)) return;
    _snwprintf_s(text, ARRAYSIZE(text), _TRUNCATE, TXT_CLEAR_CONFIRM, dir, g_app.cache_dir,
                 L"HKCU\\...\\Run\\" RUN_VALUE);
    app_modal_begin();
    int answer = MessageBoxW(g_app.tray_hwnd, text, L"YAGP", MB_OKCANCEL | MB_ICONQUESTION | MB_SETFOREGROUND);
    app_modal_end();
    if (answer != IDOK) return;
    KillTimer(g_app.tray_hwnd, TIMER_SAVE);
    g_app.cache_dir[0] = 0; /* Stop writing cache files for the rest of this run. */
    frame_cache_delete_all();
    LSTATUS run = RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE);
    bool run_ok = run == ERROR_SUCCESS || run == ERROR_FILE_NOT_FOUND;
    bool config_ok = config_delete_all();
    if (config_ok) g_app.persist = false;
    if (config_ok && run_ok) app_notify(TXT_CONFIG_DELETED, dir);
    else if (!config_ok) app_error(TXT_ERR_DELETE_CONFIG, dir);
    else app_error(TXT_ERR_DELETE_AUTOSTART, L"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\\" RUN_VALUE);
}

static void show_tray_menu(void)
{
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    bool any_paused = false, any_playing = false;
    AppendMenuW(menu, MF_STRING, TRAY_CMD_ADD, TXT_MENU_ADD_GIF);
    if (g_app.overlays) AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    UINT index = 0;
    for (Overlay *o = g_app.overlays; o; o = o->next, ++index) {
        HMENU sub = CreatePopupMenu();
        if (!sub) continue;
        overlay_build_menu(o, sub, TRAY_CMD_OVERLAY_BASE + index * OVERLAY_CMD_SPAN);
        wchar_t label[MAX_PATH + 32];
        bool moving = o->script_spec && *o->script_spec;
        _snwprintf_s(label, ARRAYSIZE(label), _TRUNCATE, L"%s%s%s%s%s", overlay_name(o),
                     o->hidden ? TXT_TAG_HIDDEN : L"",
                     o->paused ? TXT_TAG_PAUSED : L"", moving ? TXT_TAG_MOVING : L"",
                     o->clickthrough ? TXT_TAG_CLICKTHROUGH : L"");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, label);
        if (o->paused) any_paused = true; else any_playing = true;
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (any_playing ? 0 : MF_GRAYED), TRAY_CMD_PAUSE_ALL, TXT_MENU_PAUSE_ALL);
    AppendMenuW(menu, MF_STRING | (any_paused ? 0 : MF_GRAYED), TRAY_CMD_RESUME_ALL, TXT_MENU_RESUME_ALL);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (autostart_enabled() ? MF_CHECKED : 0), TRAY_CMD_AUTOSTART, TXT_MENU_AUTOSTART);
    AppendMenuW(menu, MF_STRING, TRAY_CMD_CLEAR_CONFIG, TXT_MENU_CLEAR_CONFIG);
    AppendMenuW(menu, MF_STRING, TRAY_CMD_EXIT, TXT_MENU_EXIT);

    POINT pt;
    GetCursorPos(&pt);
    UINT cmd = app_track_menu(menu, pt.x, pt.y);
    DestroyMenu(menu);

    if (cmd >= TRAY_CMD_OVERLAY_BASE) {
        UINT rel = cmd - TRAY_CMD_OVERLAY_BASE;
        UINT target = rel / OVERLAY_CMD_SPAN;
        Overlay *o = g_app.overlays;
        for (UINT i = 0; o && i < target; ++i) o = o->next;
        if (o) overlay_command(o, rel % OVERLAY_CMD_SPAN);
        return;
    }
    switch (cmd) {
    case TRAY_CMD_ADD:
        app_pick_files(PICK_ADD, NULL);
        break;
    case TRAY_CMD_PAUSE_ALL:
    case TRAY_CMD_RESUME_ALL:
        for (Overlay *o = g_app.overlays; o; o = o->next) {
            bool pause = cmd == TRAY_CMD_PAUSE_ALL;
            if (o->paused != pause) overlay_command(o, OVERLAY_CMD_PAUSE);
        }
        break;
    case TRAY_CMD_AUTOSTART:
        autostart_set(!autostart_enabled());
        break;
    case TRAY_CMD_CLEAR_CONFIG:
        clear_config();
        break;
    case TRAY_CMD_EXIT:
        PostQuitMessage(0); /* Tray window stays alive until the icon is removed. */
        break;
    }
}

/* ---- single instance ----------------------------------------------------- */

/* Payload: NUL separated absolute paths, double-NUL terminated. Empty = open picker. */
static void handle_copydata(const COPYDATASTRUCT *cds)
{
    if (cds->dwData == COPYDATA_PICK_RESULT) {
        handle_pick_result(cds);
        return;
    }
    if (cds->dwData != COPYDATA_ADD_FILES) return;
    size_t n = cds->cbData / sizeof(wchar_t);
    const wchar_t *p = (const wchar_t *)cds->lpData;
    if (n < 2 || !p || p[n - 1] != 0 || p[0] == 0) {
        app_pick_files(PICK_ADD, NULL);
        return;
    }
    while (p < (const wchar_t *)cds->lpData + n && *p) {
        add_overlay_from_path(p, NULL);
        p += wcslen(p) + 1;
    }
}

static wchar_t *pack_args(int argc, wchar_t **argv, DWORD *bytes)
{
    size_t total = 1;
    wchar_t full[MAX_PATH * 4];
    for (int i = 1; i < argc; ++i) {
        DWORD len = GetFullPathNameW(argv[i], ARRAYSIZE(full), full, NULL);
        if (len && len < ARRAYSIZE(full)) total += len + 1;
    }
    if (total < 2) total = 2;
    wchar_t *buf = (wchar_t *)calloc(total, sizeof(wchar_t));
    if (!buf) return NULL;
    wchar_t *w = buf;
    for (int i = 1; i < argc; ++i) {
        DWORD len = GetFullPathNameW(argv[i], ARRAYSIZE(full), full, NULL);
        if (len && len < ARRAYSIZE(full)) {
            memcpy(w, full, (len + 1) * sizeof(wchar_t));
            w += len + 1;
        }
    }
    *bytes = (DWORD)(total * sizeof(wchar_t));
    return buf;
}

static void forward_to_running_instance(int argc, wchar_t **argv)
{
    HWND target = NULL;
    for (int i = 0; i < 50 && !target; ++i) { /* First instance may still be starting. */
        target = FindWindowW(TRAY_CLASS, NULL);
        if (!target) Sleep(100);
    }
    if (!target) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(target, &pid);
    AllowSetForegroundWindow(pid);
    DWORD bytes = 0;
    wchar_t *payload = pack_args(argc, argv, &bytes);
    if (!payload) return;
    COPYDATASTRUCT cds = {COPYDATA_ADD_FILES, bytes, payload};
    DWORD_PTR result = 0;
    SendMessageTimeoutW(target, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 5000, &result);
    free(payload);
}

/* ---- tray window --------------------------------------------------------- */

static LRESULT CALLBACK tray_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TRAY:
        switch (LOWORD(lp)) {
        case WM_CONTEXTMENU:
        case NIN_SELECT:
        case NIN_KEYSELECT:
            show_tray_menu();
            break;
        }
        return 0;
    case WM_TIMER:
        if (wp == TIMER_MODAL_TICK) app_tick();
        else if (wp == TIMER_SAVE) save_now();
        else if (wp == TIMER_DEVICE_RETRY) { recover_device(); app_reschedule(); }
        return 0;
    case WM_COPYDATA:
        /* Run the work after SendMessage returns so the sender is not blocked by dialogs. */
        {
            const COPYDATASTRUCT *src = (const COPYDATASTRUCT *)lp;
            COPYDATASTRUCT *copy = (COPYDATASTRUCT *)malloc(sizeof(*copy) + src->cbData);
            if (!copy) return FALSE;
            *copy = *src;
            copy->lpData = copy + 1;
            if (src->cbData) memcpy(copy->lpData, src->lpData, src->cbData);
            PostMessageW(hwnd, WM_APP + 2, 0, (LPARAM)copy);
        }
        return TRUE;
    case WM_APP + 2:
        handle_copydata((const COPYDATASTRUCT *)lp);
        free((void *)lp);
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
    case WM_SETTINGCHANGE:
        if (msg == WM_SETTINGCHANGE && wp != SPI_SETWORKAREA) break;
        /* Monitors added/removed/rearranged or a taskbar moved: let scripts
         * pull their GIF back into view. */
        monitors_refresh();
        for (Overlay *o = g_app.overlays; o; o = o->next) overlay_screens_changed(o);
        app_reschedule();
        return 0;
    case WM_QUERYENDSESSION:
        save_now();
        return TRUE;
    case WM_ENDSESSION:
        if (wp) save_now();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    if (msg == g_app.msg_taskbar_created && msg != 0) {
        tray_add_icon(); /* Explorer restarted. */
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool create_tray_window(void)
{
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = tray_proc;
    wc.hInstance = g_app.instance;
    wc.lpszClassName = TRAY_CLASS;
    if (!RegisterClassExW(&wc)) return false;
    /* A real (hidden) top-level window rather than HWND_MESSAGE: it must be able
     * to take the foreground for TrackPopupMenu and receive TaskbarCreated. */
    g_app.tray_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, TRAY_CLASS, L"YAGP", WS_POPUP,
                                      0, 0, 0, 0, NULL, NULL, g_app.instance, NULL);
    return g_app.tray_hwnd != NULL;
}

/* ---- entry point --------------------------------------------------------- */

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE prev, PWSTR cmdline, int show)
{
    (void)prev; (void)cmdline; (void)show;
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 2 && wcscmp(argv[1], L"--pick") == 0) {
        int rc = run_picker(argc, argv); /* Before the single-instance check: it is a helper. */
        LocalFree(argv);
        return rc;
    }

    HANDLE mutex = CreateMutexW(NULL, FALSE, INSTANCE_MUTEX);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (argv) forward_to_running_instance(argc, argv);
        LocalFree(argv);
        CloseHandle(mutex);
        return 0;
    }

    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g_app.instance = instance;
    g_app.qpc_freq = freq.QuadPart;
    g_app.persist = true;
    if (!frame_cache_default_dir(g_app.cache_dir, MAX_PATH)) g_app.cache_dir[0] = 0;
    g_app.msg_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    g_app.frame_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!g_app.frame_timer) g_app.frame_timer = CreateWaitableTimerW(NULL, FALSE, NULL); /* Pre-1803 */

    int exit_code = 1;
    if (!g_app.frame_timer || !create_tray_window() || !overlay_register_class(instance)) {
        MessageBoxW(NULL, TXT_ERR_INIT_WINDOW, L"YAGP", MB_OK | MB_ICONERROR);
        goto done;
    }
    g_app.device = render_device_create();
    if (!g_app.device) {
        MessageBoxW(NULL, TXT_ERR_INIT_D3D, L"YAGP", MB_OK | MB_ICONERROR);
        goto done;
    }
    /* Let a second instance at a different integrity level forward files. */
    ChangeWindowMessageFilterEx(g_app.tray_hwnd, WM_COPYDATA, MSGFLT_ALLOW, NULL);

    monitors_refresh();
    g_app.tray_icon = make_tray_icon();
    tray_add_icon();
    config_load(restore_overlay, NULL);
    for (int i = 1; argv && i < argc; ++i) {
        wchar_t full[MAX_PATH * 4];
        DWORD len = GetFullPathNameW(argv[i], ARRAYSIZE(full), full, NULL);
        if (len && len < ARRAYSIZE(full)) add_overlay_from_path(full, NULL);
    }
    app_changed();

    for (;;) {
        HANDLE waits[2] = {g_app.frame_timer, g_app.pick_process};
        DWORD count = g_app.pick_process ? 2 : 1;
        DWORD r = MsgWaitForMultipleObjectsEx(count, waits, INFINITE, QS_ALLINPUT,
                                              MWMO_INPUTAVAILABLE);
        if (r == WAIT_OBJECT_0) app_tick();
        else if (count == 2 && r == WAIT_OBJECT_0 + 1) pick_close(); /* Helper exited. */
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                exit_code = (int)msg.wParam;
                goto shutdown;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

shutdown:
    if (g_app.pick_process) TerminateProcess(g_app.pick_process, 0);
    pick_close();
    save_now();
    destroy_all_overlays();
    tray_remove_icon();
done:
    destroy_all_overlays();
    render_device_destroy(g_app.device);
    if (g_app.tray_hwnd && IsWindow(g_app.tray_hwnd)) DestroyWindow(g_app.tray_hwnd);
    if (g_app.tray_icon) DestroyIcon(g_app.tray_icon);
    if (g_app.frame_timer) CloseHandle(g_app.frame_timer);
    if (SUCCEEDED(com)) CoUninitialize();
    LocalFree(argv);
    if (mutex) CloseHandle(mutex);
    return exit_code;
}
