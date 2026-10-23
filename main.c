#include "app.h"
#include "group.h"
#include "applog.h"
#include "config.h"
#include "cursor_hook.h"
#include "frame_cache.h"
#include "i18n.h"
#include "log.h"
#include "monitors.h"
#include "overlay.h"

#include <cderr.h>
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
/* Logging stopped by itself (session limit or a write error): show the balloon.
 * Posted by the applog stop callback, which may run on any thread. */
#define WM_APP_LOG_STOPPED (WM_APP + 6)
#define TRAY_UID 1

enum { TIMER_MODAL_TICK = 1, TIMER_SAVE, TIMER_DEVICE_RETRY };
enum {
    TRAY_CMD_ADD = 1,
    TRAY_CMD_PAUSE_ALL,
    TRAY_CMD_RESUME_ALL,
    TRAY_CMD_CLEAR_CONFIG,
    TRAY_CMD_AUTOSTART,
    TRAY_CMD_LOG,
    TRAY_CMD_EXIT,
    TRAY_CMD_GROUP_IMPORT = 20,
    TRAY_CMD_OVERLAY_BASE = 1000,
    TRAY_CMD_GROUP_BASE = 1000000,   /* + group index * 4 + action */
    TRAY_CMD_LEADER_BASE = 2000000   /* + leader overlay id */
};
#define GROUP_ACTION_HIDE 0
#define GROUP_ACTION_REMOVE 1
#define GROUP_MENU_SPAN 4

App g_app;
static int s_cascade;

/* ---- timing -------------------------------------------------------------- */

int64_t app_now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

uint64_t app_uid_alloc(void)
{
    if (!g_app.next_uid) g_app.next_uid = 1;
    return g_app.next_uid++;
}

void app_reschedule(void)
{
    int64_t earliest = INT64_MAX;
    for (Overlay *o = g_app.overlays; o; o = o->next) {
        int64_t t = overlay_deadline(o);
        if (t < earliest) earliest = t;
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
        log_error("device recreation failed; retrying in 2 s");
        SetTimer(g_app.tray_hwnd, TIMER_DEVICE_RETRY, 2000, NULL);
        return;
    }
    KillTimer(g_app.tray_hwnd, TIMER_DEVICE_RETRY);
    log_info("D3D11 device recreated");
    for (Overlay *o = g_app.overlays; o; o = o->next) {
        overlay_attach_surface(o);
        o->next_frame = app_now();
    }
}

static void app_tick(void)
{
    if (!g_app.device || render_device_lost(g_app.device)) {
        log_warn("device lost; recreating");
        recover_device();
        app_reschedule();
        return;
    }
    int64_t now = app_now();
    int64_t slack = g_app.qpc_freq / 1000;
    bool moved = false;
    for (Overlay *o = g_app.overlays; o; o = o->next) {
        if (overlay_service(o, now, slack)) moved = true;
        if (o->dragging) moved = true;
    }
    /* Only when something moved, so an idle tray still does no work. */
    if (moved || overlay_peers_dirty()) overlay_update_contacts();
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
    int group_count = 0, count = 0;
    for (Group *g = g_app.groups; g; g = g->next) ++group_count;
    for (Overlay *o = g_app.overlays; o; o = o->next) ++count;
    GroupConfig *groups = group_count ? (GroupConfig *)calloc((size_t)group_count, sizeof(*groups)) : NULL;
    OverlayConfig *entries = count ? (OverlayConfig *)malloc((size_t)count * sizeof(*entries)) : NULL;
    if ((group_count && !groups) || (count && !entries)) {
        free(groups);
        free(entries);
        return;
    }
    int gi = 0;
    for (Group *g = g_app.groups; g; g = g->next) {
        groups[gi].uid = g->uid;
        groups[gi].name = g->name;
        groups[gi].folder = g->folder;
        groups[gi].hidden = g->hidden;
        ++gi;
    }
    int i = 0;
    for (Overlay *o = g_app.overlays; o; o = o->next) overlay_to_config(o, &entries[i++]);
    ConfigSave data = {groups, group_count, entries, count};
    config_save(&data);
    free(groups);
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
    overlay_announce(o, true);
    /* Its script was started before it was linked, so the mouse hook could
     * not see its subscriptions yet. */
    overlay_refresh_hook();
    app_changed();
}

#define RESTORE_MAX 256
typedef struct RestoreItem {
    Overlay *o;
    uint64_t leader_uid;
} RestoreItem;
static RestoreItem s_restore[RESTORE_MAX];
static int s_restore_n;
static int s_restored;

static void bump_uid(uint64_t uid)
{
    if (uid >= g_app.next_uid) g_app.next_uid = uid + 1;
}

static void restore_group(const GroupConfig *cfg, void *ctx)
{
    (void)ctx;
    Group *g = group_create(cfg->name, cfg->folder);
    if (!g) return;
    if (cfg->uid) g->uid = cfg->uid;
    g->hidden = cfg->hidden;
    bump_uid(g->uid);
}

static void restore_overlay(const OverlayConfig *cfg, void *ctx)
{
    (void)ctx;
    if (s_restore_n >= RESTORE_MAX) return;
    if (GetFileAttributesW(cfg->path) == INVALID_FILE_ATTRIBUTES) {
        char u8[MAX_PATH * 4];
        log_warn("skipping missing GIF: %s", applog_u8(cfg->path, u8, sizeof(u8)));
        return;
    }
    OverlayConfig copy = *cfg;
    copy.defer_script = true;
    Overlay *o = overlay_create(&copy, true, s_cascade++);
    if (!o) return;
    if (cfg->uid) o->uid = cfg->uid;
    bump_uid(o->uid);
    if (cfg->group_uid) o->group = group_find_uid(cfg->group_uid);
    link_overlay(o);
    s_restore[s_restore_n].o = o;
    s_restore[s_restore_n].leader_uid = cfg->leader_uid;
    ++s_restore_n;
    ++s_restored;
}

static void finish_restore(void)
{
    for (int i = 0; i < s_restore_n; ++i) {
        Overlay *o = s_restore[i].o;
        if (!s_restore[i].leader_uid) continue;
        Overlay *leader = NULL;
        for (int j = 0; j < s_restore_n; ++j) {
            if (s_restore[j].o->uid == s_restore[i].leader_uid) {
                leader = s_restore[j].o;
                break;
            }
        }
        if (leader && leader != o && !leader->leader && !overlay_has_followers(o))
            o->leader = leader;
    }
    for (int i = 0; i < s_restore_n; ++i) {
        Overlay *o = s_restore[i].o;
        if (!o->leader && o->script_spec && *o->script_spec) overlay_set_script(o, o->script_spec);
        overlay_refresh_visibility(o);
    }
    Group *g = g_app.groups;
    while (g) {
        Group *next = g->next;
        if (group_is_empty(g)) group_destroy(g);
        g = next;
    }
    s_restore_n = 0;
    overlay_refresh_hook();
}

void app_remove_overlay(Overlay *o)
{
    if (g_app.pick_target == o) { /* A pending "replace" for it becomes meaningless. */
        g_app.pick_target = NULL;
        g_app.pick_id = 0;
    }
    Group *group = o->group;
    if (overlay_has_followers(o)) overlay_leader_removed(o);
    for (Overlay **p = &g_app.overlays; *p; p = &(*p)->next) {
        if (*p == o) {
            *p = o->next;
            o->next = NULL;
            if (!o->hidden && !(group && group->hidden)) overlay_announce(o, false);
            char u8[MAX_PATH * 4];
            log_info("removed GIF #%d: %s", o->id, applog_u8(overlay_name(o), u8, sizeof(u8)));
            overlay_destroy(o);
            break;
        }
    }
    group_after_member_removed(group);
    overlay_refresh_hook();
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
    if (g_app.pick_process) {
        CloseHandle(g_app.pick_process);
        log_debug("picker helper exited");
    }
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
    case PICK_LOG_DIR: return L"logdir";
    case PICK_GROUP: return L"group";
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
            log_debug("picker already open (pid %lu)", (unsigned long)g_app.pick_pid);
            return;
        }
        TerminateProcess(g_app.pick_process, 0); /* A different request supersedes it. */
    }
    pick_close();

    wchar_t exe[MAX_PATH * 4], cmd[MAX_PATH * 8 + 512];
    DWORD n = GetModuleFileNameW(NULL, exe, ARRAYSIZE(exe));
    if (!n || n >= ARRAYSIZE(exe)) {
        log_error("cannot resolve the executable path for the picker");
        return;
    }
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
        DWORD err = GetLastError();
        char u8[MAX_PATH * 4];
        log_error("cannot start file picker: %s (error %lu)", applog_u8(exe, u8, sizeof(u8)),
                  (unsigned long)err);
        app_error(TXT_ERR_PICKER, exe);
        return;
    }
    /* We are foreground (the user just clicked our menu), so we may pass that on. */
    AllowSetForegroundWindow(pi.dwProcessId);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    log_info("picker start: %s (pid %lu)", pick_verb(kind), (unsigned long)pi.dwProcessId);
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

/* Folder picker for "enable logging". The helper starts in the folder used
 * earlier in this run, or in Documents when this is the first time. */
static void app_pick_log_dir(void)
{
    const wchar_t *dir = applog_last_dir();
    if (dir && *dir) {
        wchar_t extra[MAX_PATH * 4 + 4];
        size_t n = wcslen(dir);
        /* A trailing backslash would escape the closing quote for the helper's
         * CommandLineToArgvW; root directories are the only paths like this. */
        bool trailing = n && (dir[n - 1] == L'\\' || dir[n - 1] == L'/');
        _snwprintf_s(extra, ARRAYSIZE(extra), _TRUNCATE, L"\"%s%s\"", dir, trailing ? L"\\" : L"");
        launch_picker(PICK_LOG_DIR, NULL, extra);
    } else {
        launch_picker(PICK_LOG_DIR, NULL, NULL);
    }
}

static void app_pick_group(void)
{
    launch_picker(PICK_GROUP, NULL, NULL);
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
    if (g_app.pick_kind == PICK_LOG_DIR) {
        if (!*p) {
            log_debug("picker returned no path");
            return;
        }
        if (applog_state() != APPLOG_OFF) {
            log_debug("log directory result ignored: logging is already on");
            return;
        }
        DWORD err = 0;
        if (!applog_start(p, &err)) {
            char u8[MAX_PATH * 4];
            log_error("cannot create log file in %s (error %lu)", applog_u8(p, u8, sizeof(u8)),
                      (unsigned long)err);
            app_error(TXT_ERR_LOG_OPEN, p);
        }
        return;
    }
    if (g_app.pick_kind == PICK_GROUP) {
        if (!*p) {
            log_debug("picker returned no group folder");
            return;
        }
        wchar_t err[512];
        if (!group_import_folder(p, err, ARRAYSIZE(err))) {
            log_warn("group import failed: %ls", err);
            app_notify(TXT_GROUP_IMPORT_TITLE, err);
        } else {
            app_notify(TXT_GROUP_IMPORT_TITLE, TXT_GROUP_IMPORT_DONE);
        }
        return;
    }
    if (g_app.pick_kind == PICK_REPLACE) {
        if (g_app.pick_target && *p) {
            char u8[MAX_PATH * 4];
            log_info("picker result: replace %s", applog_u8(p, u8, sizeof(u8)));
            overlay_replace(g_app.pick_target, p);
        }
        return;
    }
    if (g_app.pick_kind == PICK_SCRIPT || g_app.pick_kind == PICK_NEW_SCRIPT) {
        if (g_app.pick_target && *p) {
            char u8[MAX_PATH * 4];
            log_info("picker result: script %s", applog_u8(p, u8, sizeof(u8)));
            overlay_set_script(g_app.pick_target, p);
            app_changed();
        }
        return;
    }
    int count = 0;
    for (; p < end && *p; p += wcslen(p) + 1) {
        ++count;
        add_overlay_from_path(p, NULL);
    }
    log_info("picker result: add %d path(s)", count);
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

/* Initial folder for the log folder picker. */
static int CALLBACK browse_init(HWND hwnd, UINT msg, LPARAM lp, LPARAM data)
{
    (void)lp;
    if (msg == BFFM_INITIALIZED && data) SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, data);
    return 0;
}

static int run_pick_group(int argc, wchar_t **argv, uint64_t id)
{
    (void)argc;
    (void)argv;
    wchar_t initial[MAX_PATH * 2] = L"";
    PWSTR docs = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Documents, 0, NULL, &docs))) {
        wcsncpy_s(initial, ARRAYSIZE(initial), docs, _TRUNCATE);
        CoTaskMemFree(docs);
    }
    BROWSEINFOW bi;
    memset(&bi, 0, sizeof(bi));
    wchar_t display[MAX_PATH] = L"";
    bi.pszDisplayName = display;
    bi.lpszTitle = TXT_TITLE_PICK_GROUP;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
    bi.lpfn = browse_init;
    bi.lParam = (LPARAM)(initial[0] ? initial : NULL);
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return 0;
    wchar_t path[MAX_PATH * 2];
    bool ok = SHGetPathFromIDListW(pidl, path) != FALSE;
    CoTaskMemFree(pidl);
    if (!ok) return 1;
    size_t used = wcslen(path) + 2;
    wchar_t *out = (wchar_t *)calloc(used, sizeof(wchar_t));
    if (!out) return 1;
    memcpy(out, path, (used - 2) * sizeof(wchar_t));
    send_pick_result(id, out, used);
    free(out);
    return 0;
}

/* "--pick logdir <id> [<initial folder>]": choose where the log file goes. */
static int run_pick_log_dir(int argc, wchar_t **argv, uint64_t id)
{
    wchar_t initial[MAX_PATH * 2] = L"";
    if (argc > 4 && argv[4]) wcsncpy_s(initial, ARRAYSIZE(initial), argv[4], _TRUNCATE);
    if (!initial[0]) {
        PWSTR docs = NULL;
        if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Documents, 0, NULL, &docs))) {
            wcsncpy_s(initial, ARRAYSIZE(initial), docs, _TRUNCATE);
            CoTaskMemFree(docs);
        }
    }

    BROWSEINFOW bi;
    memset(&bi, 0, sizeof(bi));
    wchar_t display[MAX_PATH] = L"";
    bi.pszDisplayName = display;
    bi.lpszTitle = TXT_TITLE_PICK_LOG_DIR;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
    bi.lpfn = browse_init;
    bi.lParam = (LPARAM)(initial[0] ? initial : NULL);
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return 0; /* Cancelled. */
    wchar_t path[MAX_PATH * 2];
    bool ok = SHGetPathFromIDListW(pidl, path) != FALSE;
    CoTaskMemFree(pidl);
    if (!ok) return 1;

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
    if (argc >= 4 && wcscmp(argv[2], L"group") == 0) {
        HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        int rc = run_pick_group(argc, argv, wcstoull(argv[3], NULL, 16));
        if (SUCCEEDED(com)) CoUninitialize();
        return rc;
    }
    if (argc >= 4 && wcscmp(argv[2], L"logdir") == 0) {
        HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        int rc = run_pick_log_dir(argc, argv, wcstoull(argv[3], NULL, 16));
        if (SUCCEEDED(com)) CoUninitialize();
        return rc;
    }
    if (argc != 4) return 2;
    bool multi = wcscmp(argv[2], L"add") == 0;
    bool script = wcscmp(argv[2], L"script") == 0;
    uint64_t id = wcstoull(argv[3], NULL, 16);
    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    /* Multi-select returns "dir\0name\0name\0\0". 1M characters (2 MB, in this
     * short-lived helper) fits thousands of files; 32K could fail with
     * FNERR_BUFFERTOOSMALL after a few hundred, which looked like a cancel. */
    const DWORD cap = 1u << 20;
    wchar_t *buf = (wchar_t *)calloc(cap, sizeof(wchar_t));
    wchar_t *out = NULL;
    int rc = 1;
    if (!buf) goto done;
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = script ? TXT_FILTER_LUA : TXT_FILTER_MEDIA;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = cap;
    ofn.lpstrTitle = script ? TXT_TITLE_PICK_SCRIPT
                            : multi ? TXT_TITLE_ADD_MEDIA : TXT_TITLE_REPLACE_MEDIA;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY |
                (multi ? OFN_ALLOWMULTISELECT : 0);
    if (!GetOpenFileNameW(&ofn)) {
        if (CommDlgExtendedError() == FNERR_BUFFERTOOSMALL) MessageBoxW(NULL, TXT_ERR_TOO_MANY_FILES, L"YAGP", MB_OK | MB_ICONWARNING);
        else rc = 0; /* Cancelled. */
        goto done;
    }

    /* Normalise both dialog result shapes into "full\0full\0\0". Each name
     * grows by the directory, so size the output from the input instead of
     * guessing: nothing is truncated or dropped. */
    const wchar_t *first = buf, *name = buf + wcslen(buf) + 1;
    bool several = multi && *name;
    size_t dir_len = wcslen(first), need = 1; /* Final NUL. */
    size_t sep = dir_len && first[dir_len - 1] == L'\\' ? 0 : 1; /* "C:\" already ends in one. */
    if (!several) {
        need += dir_len + 1;
    } else {
        for (const wchar_t *n = name; *n; n += wcslen(n) + 1) need += dir_len + sep + wcslen(n) + 1;
    }
    out = (wchar_t *)malloc(need * sizeof(wchar_t));
    if (!out) goto done;
    size_t used = 0;
    if (!several) {
        memcpy(out, first, (dir_len + 1) * sizeof(wchar_t));
        used = dir_len + 1;
    } else {
        for (; *name; name += wcslen(name) + 1) {
            size_t len = wcslen(name);
            memcpy(out + used, first, dir_len * sizeof(wchar_t));
            if (sep) out[used + dir_len] = L'\\';
            memcpy(out + used + dir_len + sep, name, (len + 1) * sizeof(wchar_t));
            used += dir_len + sep + len + 1;
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

bool app_notify(const wchar_t *title, const wchar_t *text)
{
    NOTIFYICONDATAW nid;
    tray_icon_data(&nid);
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    wcsncpy_s(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), title, _TRUNCATE);
    wcsncpy_s(nid.szInfo, ARRAYSIZE(nid.szInfo), text, _TRUNCATE);
    return Shell_NotifyIconW(NIM_MODIFY, &nid) != FALSE;
}

void app_error(const wchar_t *fmt, const wchar_t *arg)
{
    wchar_t text[1024];
    _snwprintf_s(text, ARRAYSIZE(text), _TRUNCATE, fmt, arg);
    app_modal_begin();
    MessageBoxW(g_app.tray_hwnd, text, L"YAGP", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    app_modal_end();
}

/* applog can stop by itself (session limit or a write error) on any thread;
 * marshal the balloon to the UI thread. */
static void log_stopped_cb(void *ctx)
{
    (void)ctx;
    if (g_app.tray_hwnd) PostMessageW(g_app.tray_hwnd, WM_APP_LOG_STOPPED, 0, 0);
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
    BOOL added = Shell_NotifyIconW(NIM_ADD, &nid);
    if (added) {
        log_debug("tray icon added");
    } else {
        log_warn("tray icon add failed (error %lu)", (unsigned long)GetLastError());
    }
    nid.uVersion = NOTIFYICON_VERSION_4;
    if (!Shell_NotifyIconW(NIM_SETVERSION, &nid))
        log_warn("tray icon version failed (error %lu)", (unsigned long)GetLastError());
}

static void tray_remove_icon(void)
{
    NOTIFYICONDATAW nid;
    tray_icon_data(&nid);
    if (Shell_NotifyIconW(NIM_DELETE, &nid)) log_debug("tray icon removed");
    else log_warn("tray icon remove failed (error %lu)", (unsigned long)GetLastError());
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
        if (!autostart_command(cmd, ARRAYSIZE(cmd))) {
            log_error("cannot resolve the executable path for autostart");
            return;
        }
        rc = RegSetKeyValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE, REG_SZ, cmd,
                             (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
    } else {
        rc = RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    if (rc != ERROR_SUCCESS) {
        log_error("cannot update autostart (error %ld)", (long)rc);
        app_error(TXT_ERR_AUTOSTART, L"HKCU\\...\\Run");
    } else {
        log_info("autostart %s", enable ? "enabled" : "disabled");
    }
}

/* ---- delete all data --------------------------------------------------------
 * Removes everything YAGP leaves on the machine: config, frame cache and the
 * autostart entry (whatever exe it points at). */
static void clear_config(void)
{
    wchar_t dir[MAX_PATH];
    wchar_t text[MAX_PATH * 2 + 384];
    if (!config_dir(dir, MAX_PATH)) {
        log_error("cannot resolve the settings folder");
        return;
    }
    _snwprintf_s(text, ARRAYSIZE(text), _TRUNCATE, TXT_CLEAR_CONFIRM, dir, g_app.cache_dir,
                 L"HKCU\\...\\Run\\" RUN_VALUE);
    app_modal_begin();
    int answer = MessageBoxW(g_app.tray_hwnd, text, L"YAGP", MB_OKCANCEL | MB_ICONQUESTION | MB_SETFOREGROUND);
    app_modal_end();
    if (answer != IDOK) return;
    char u8[MAX_PATH * 4];
    log_info("deleting settings data: %s", applog_u8(dir, u8, sizeof(u8)));
    KillTimer(g_app.tray_hwnd, TIMER_SAVE);
    g_app.cache_dir[0] = 0; /* Stop writing cache files for the rest of this run. */
    frame_cache_delete_all();
    LSTATUS run = RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE);
    bool run_ok = run == ERROR_SUCCESS || run == ERROR_FILE_NOT_FOUND;
    bool config_ok = config_delete_all();
    if (config_ok) {
        g_app.persist = false;
        while (g_app.groups) group_destroy(g_app.groups);
        for (Overlay *o = g_app.overlays; o; o = o->next) {
            if (o->leader) {
                o->leader = NULL;
                if (o->script_spec && *o->script_spec) overlay_set_script(o, o->script_spec);
            }
        }
        g_app.next_uid = 1;
    }
    if (config_ok && run_ok) {
        log_info("settings data deleted: %s", applog_u8(dir, u8, sizeof(u8)));
        app_notify(TXT_CONFIG_DELETED, dir);
    } else if (!config_ok) {
        log_error("cannot delete settings data: %s", applog_u8(dir, u8, sizeof(u8)));
        app_error(TXT_ERR_DELETE_CONFIG, dir);
    } else {
        log_error("cannot delete autostart entry (error %ld)", (long)run);
        app_error(TXT_ERR_DELETE_AUTOSTART, L"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\\" RUN_VALUE);
    }
}

static void show_tray_menu(void)
{
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    bool any_paused = false, any_playing = false;
    AppendMenuW(menu, MF_STRING, TRAY_CMD_ADD, TXT_MENU_ADD_GIF);
    HMENU groups = CreatePopupMenu();
    if (groups) {
        AppendMenuW(groups, MF_STRING, TRAY_CMD_GROUP_IMPORT, TXT_MENU_GROUP_IMPORT);
        if (g_app.groups) AppendMenuW(groups, MF_SEPARATOR, 0, NULL);
        UINT gi = 0;
        for (Group *g = g_app.groups; g; g = g->next, ++gi) {
            HMENU gs = CreatePopupMenu();
            if (!gs) continue;
            UINT base = TRAY_CMD_GROUP_BASE + gi * GROUP_MENU_SPAN;
            AppendMenuW(gs, MF_STRING, base + GROUP_ACTION_HIDE,
                        g->hidden ? TXT_MENU_GROUP_SHOW : TXT_MENU_GROUP_HIDE);
            AppendMenuW(gs, MF_STRING, base + GROUP_ACTION_REMOVE, TXT_MENU_GROUP_REMOVE);
            wchar_t label[MAX_PATH + 32];
            _snwprintf_s(label, ARRAYSIZE(label), _TRUNCATE, L"%s (%d)", g->name, group_member_count(g));
            AppendMenuW(groups, MF_POPUP, (UINT_PTR)gs, label);
        }
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)groups, TXT_MENU_GROUPS);
    }
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
    AppLogState log = applog_state();
    AppendMenuW(menu, MF_STRING, TRAY_CMD_LOG,
                log == APPLOG_ON ? TXT_MENU_LOG_STOP : TXT_MENU_LOG_START);
    AppendMenuW(menu, MF_STRING, TRAY_CMD_CLEAR_CONFIG, TXT_MENU_CLEAR_CONFIG);
    AppendMenuW(menu, MF_STRING, TRAY_CMD_EXIT, TXT_MENU_EXIT);

    POINT pt;
    GetCursorPos(&pt);
    UINT cmd = app_track_menu(menu, pt.x, pt.y);
    DestroyMenu(menu);

    if (cmd >= OVERLAY_LEADER_CMD_BASE) {
        overlay_leader_command(cmd);
        return;
    }
    if (cmd >= TRAY_CMD_GROUP_BASE && cmd < TRAY_CMD_LEADER_BASE) {
        UINT rel = cmd - TRAY_CMD_GROUP_BASE;
        UINT target = rel / GROUP_MENU_SPAN;
        UINT action = rel % GROUP_MENU_SPAN;
        Group *g = g_app.groups;
        for (UINT i = 0; g && i < target; ++i) g = g->next;
        if (!g) return;
        if (action == GROUP_ACTION_HIDE) {
            group_set_hidden(g, !g->hidden);
        } else if (action == GROUP_ACTION_REMOVE) {
            if (MessageBoxW(g_app.tray_hwnd, TXT_GROUP_REMOVE_CONFIRM, L"YAGP",
                            MB_YESNO | MB_ICONQUESTION) == IDYES) {
                group_remove(g);
            }
        }
        return;
    }
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
    case TRAY_CMD_GROUP_IMPORT:
        app_pick_group();
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
    case TRAY_CMD_LOG:
        if (applog_state() == APPLOG_ON) applog_stop();
        else app_pick_log_dir();
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
    int paths = 0;
    while (p < (const wchar_t *)cds->lpData + n && *p) {
        ++paths;
        add_overlay_from_path(p, NULL);
        p += wcslen(p) + 1;
    }
    log_info("received %d path(s) from another instance", paths);
}

static wchar_t *pack_args(int argc, wchar_t **argv, DWORD *bytes, int *count)
{
    if (count) *count = 0;
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
    int n = 0;
    for (int i = 1; i < argc; ++i) {
        DWORD len = GetFullPathNameW(argv[i], ARRAYSIZE(full), full, NULL);
        if (len && len < ARRAYSIZE(full)) {
            memcpy(w, full, (len + 1) * sizeof(wchar_t));
            w += len + 1;
            ++n;
        }
    }
    if (count) *count = n;
    *bytes = (DWORD)(total * sizeof(wchar_t));
    return buf;
}

/* Returns the number of paths actually put into the message (0 when no running
 * instance accepted it). */
static int forward_to_running_instance(int argc, wchar_t **argv)
{
    HWND target = NULL;
    for (int i = 0; i < 50 && !target; ++i) { /* First instance may still be starting. */
        target = FindWindowW(TRAY_CLASS, NULL);
        if (!target) Sleep(100);
    }
    if (!target) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(target, &pid);
    AllowSetForegroundWindow(pid);
    DWORD bytes = 0;
    int count = 0;
    wchar_t *payload = pack_args(argc, argv, &bytes, &count);
    if (!payload) return 0;
    COPYDATASTRUCT cds = {COPYDATA_ADD_FILES, bytes, payload};
    DWORD_PTR result = 0;
    SendMessageTimeoutW(target, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 5000, &result);
    free(payload);
    return count;
}

static bool is_verbose_flag(const wchar_t *arg)
{
    return wcscmp(arg, L"--verbose") == 0 || wcscmp(arg, L"-v") == 0;
}

/* Removes --verbose/-v from argv[1..], keeping path order. The array has room
 * for the closing NULL because it came from CommandLineToArgvW. */
static bool strip_verbose_flags(int *argc, wchar_t **argv)
{
    if (!argc || !argv || *argc <= 1) return false;
    bool found = false;
    int n = 1;
    for (int i = 1; i < *argc; ++i) {
        if (is_verbose_flag(argv[i])) {
            found = true;
            continue;
        }
        argv[n++] = argv[i];
    }
    argv[n] = NULL;
    *argc = n;
    return found;
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
    case WM_APP_CACHE_READY:
        overlay_cache_ready((void *)lp);
        app_reschedule();
        return 0;
    case WM_APP_LOG_STOPPED:
        app_notify(TXT_LOG_STOPPED_TITLE, TXT_LOG_STOPPED_TEXT);
        log_info("logging stopped automatically");
        return 0;
    case WM_APP_CURSOR:
        overlay_cursor_moved();
        return 0;
    case WM_APP_HOOK_INPUT:
        overlay_hook_input();
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
    case WM_SETTINGCHANGE:
        if (msg == WM_SETTINGCHANGE && wp != SPI_SETWORKAREA) break;
        /* Monitors added/removed/rearranged or a taskbar moved: let scripts
         * pull their GIF back into view. */
        monitors_refresh();
        log_info("display change (0x%04X): %d monitor(s)", (unsigned)msg, monitors_count());
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

    bool verbose = strip_verbose_flags(&argc, argv);
    applog_init();
    if (verbose) {
        if (applog_attach_console()) log_info("verbose console output enabled");
        else log_warn("--verbose: no terminal console available");
    }

    HANDLE mutex = CreateMutexW(NULL, FALSE, INSTANCE_MUTEX);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        int forwarded = argv ? forward_to_running_instance(argc, argv) : 0;
        if (verbose) {
            if (forwarded > 0) log_info("forwarded %d path(s) to the running instance", forwarded);
            else log_warn("no path was forwarded to the running instance");
        }
        applog_shutdown();
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
    log_info("YAGP starting: build %s %s", __DATE__, __TIME__);
    {
        wchar_t exe[MAX_PATH * 4];
        DWORD n = GetModuleFileNameW(NULL, exe, ARRAYSIZE(exe));
        if (n && n < ARRAYSIZE(exe)) {
            char u8[MAX_PATH * 4];
            log_info("executable: %s", applog_u8(exe, u8, sizeof(u8)));
        }
    }
    if (!frame_cache_default_dir(g_app.cache_dir, MAX_PATH)) g_app.cache_dir[0] = 0;
    {
        char u8[MAX_PATH * 4];
        log_info("cache directory: %s", applog_u8(g_app.cache_dir, u8, sizeof(u8)));
    }
    g_app.msg_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    g_app.frame_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!g_app.frame_timer) g_app.frame_timer = CreateWaitableTimerW(NULL, FALSE, NULL); /* Pre-1803 */

    int exit_code = 1;
    if (!g_app.frame_timer || !create_tray_window() || !overlay_register_class(instance)) {
        log_error("window initialisation failed");
        MessageBoxW(NULL, TXT_ERR_INIT_WINDOW, L"YAGP", MB_OK | MB_ICONERROR);
        goto done;
    }
    applog_set_stop_callback(log_stopped_cb, NULL);
    g_app.device = render_device_create();
    if (!g_app.device) {
        log_error("D3D11 initialisation failed");
        MessageBoxW(NULL, TXT_ERR_INIT_D3D, L"YAGP", MB_OK | MB_ICONERROR);
        goto done;
    }
    /* Let a second instance at a different integrity level forward files. */
    ChangeWindowMessageFilterEx(g_app.tray_hwnd, WM_COPYDATA, MSGFLT_ALLOW, NULL);

    monitors_refresh();
    log_info("monitors: %d", monitors_count());
    for (int i = 0; i < monitors_count(); ++i) {
        const Monitor *m = monitors_get(i);
        log_info("monitor %d: %dx%d at (%ld,%ld), work %dx%d at (%ld,%ld)%s", i,
                 (int)(m->bounds.right - m->bounds.left), (int)(m->bounds.bottom - m->bounds.top),
                 (long)m->bounds.left, (long)m->bounds.top,
                 (int)(m->work.right - m->work.left), (int)(m->work.bottom - m->work.top),
                 (long)m->work.left, (long)m->work.top, m->primary ? " (primary)" : "");
    }
    g_app.tray_icon = make_tray_icon();
    tray_add_icon();
    config_load(restore_overlay, restore_group, NULL);
    finish_restore();
    log_info("restored %d GIF(s) from config", s_restored);
    overlay_refresh_hook(); /* Restored scripts' cursor / input subscriptions. */
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
    cursor_hook_shutdown();
    if (g_app.pick_process) TerminateProcess(g_app.pick_process, 0);
    pick_close();
    save_now();
    g_app.shutting_down = true;
    destroy_all_overlays();
    tray_remove_icon();
done:
    destroy_all_overlays();
    render_device_destroy(g_app.device);
    if (g_app.tray_hwnd && IsWindow(g_app.tray_hwnd)) DestroyWindow(g_app.tray_hwnd);
    if (g_app.tray_icon) DestroyIcon(g_app.tray_icon);
    if (g_app.frame_timer) CloseHandle(g_app.frame_timer);
    log_info("YAGP exiting (code %d)", exit_code);
    applog_shutdown();
    if (SUCCEEDED(com)) CoUninitialize();
    LocalFree(argv);
    if (mutex) CloseHandle(mutex);
    return exit_code;
}
