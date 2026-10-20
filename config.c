#include "config.h"
#include "log.h"

#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_HEADER "# YAGP config v4: group and overlay records; v1-v3 lines are also accepted\n"

#ifdef YAGP_CONFIG_TEST
static wchar_t s_test_dir[MAX_PATH];

void config_test_set_dir(const wchar_t *dir)
{
    wcsncpy_s(s_test_dir, ARRAYSIZE(s_test_dir), dir ? dir : L"", _TRUNCATE);
}
#endif

bool config_dir(wchar_t *out, size_t cap)
{
#ifdef YAGP_CONFIG_TEST
    if (s_test_dir[0]) {
        return _snwprintf_s(out, cap, _TRUNCATE, L"%s\\YAGP", s_test_dir) > 0;
    }
#endif
    PWSTR appdata = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &appdata))) return false;
    int n = _snwprintf_s(out, cap, _TRUNCATE, L"%s\\YAGP", appdata);
    CoTaskMemFree(appdata);
    return n > 0;
}

static bool config_file(wchar_t *out, size_t cap, const wchar_t *name)
{
    wchar_t dir[MAX_PATH];
    if (!config_dir(dir, MAX_PATH)) return false;
    return _snwprintf_s(out, cap, _TRUNCATE, L"%s\\%s", dir, name) > 0;
}

static bool parse_int(wchar_t **cursor, int *out)
{
    wchar_t *end = NULL;
    long v = wcstol(*cursor, &end, 10);
    if (end == *cursor || *end != L'\t') return false;
    *out = (int)v;
    *cursor = end + 1;
    return true;
}

static bool parse_u64(wchar_t **cursor, uint64_t *out)
{
    wchar_t *end = NULL;
    unsigned long long v = wcstoull(*cursor, &end, 10);
    if (end == *cursor || *end != L'\t') return false;
    *out = (uint64_t)v;
    *cursor = end + 1;
    return true;
}

static void split_script(wchar_t *cursor, OverlayConfig *e)
{
    e->script = L"";
    wchar_t *tab = wcschr(cursor, L'\t');
    if (tab) {
        *tab = 0;
        e->script = tab + 1;
    }
}

/* v4: o<TAB>uid<TAB>master<TAB>group<TAB>10 ints<TAB>path[<TAB>script] */
static bool parse_v4_overlay(wchar_t *line, ConfigEntryFn fn, void *ctx)
{
    wchar_t *cursor = line + 2;
    OverlayConfig e;
    memset(&e, 0, sizeof(e));
    if (!parse_u64(&cursor, &e.uid) || !parse_u64(&cursor, &e.master_uid) ||
        !parse_u64(&cursor, &e.group_uid)) return false;
    int v[10];
    for (int i = 0; i < 10; ++i) {
        if (!parse_int(&cursor, &v[i])) return false;
    }
    if (!*cursor) return false;
    e.path = cursor;
    split_script(cursor, &e);
    e.x = v[0];
    e.y = v[1];
    e.scale_pct = v[2];
    e.paused = (v[3] & 1) != 0;
    e.hidden = (v[3] & 2) != 0;
    e.topmost = v[4] != 0;
    e.locked = v[5] != 0;
    e.clickthrough = v[6] != 0;
    e.key_enabled = v[7] != 0;
    e.key_rgb = (uint32_t)v[8] & 0xFFFFFFu;
    e.key_tolerance = v[9];
    e.defer_script = false;
    fn(&e, ctx);
    return true;
}

/* v4: g<TAB>uid<TAB>hidden<TAB>folder<TAB>name */
static bool parse_v4_group(wchar_t *line, ConfigGroupFn fn, void *ctx)
{
    if (!fn) return true;
    wchar_t *cursor = line + 2;
    GroupConfig g;
    int hidden = 0;
    if (!parse_u64(&cursor, &g.uid) || !parse_int(&cursor, &hidden)) return false;
    wchar_t *tab = wcschr(cursor, L'\t');
    if (!tab || !tab[1]) return false;
    *tab = 0;
    g.folder = cursor;
    g.name = tab + 1;
    g.hidden = hidden != 0;
    fn(&g, ctx);
    return true;
}

static uint64_t s_legacy_uid;

/* v1 lines carry 7 numeric fields, v2/v3 lines 10. Paths are absolute
 * ("C:\..." or "\\server"), so they never parse as a number. v3 may append
 * "\t<script spec>"; Windows paths cannot contain a tab, so the first tab
 * after the numbers ends the GIF path. */
static bool parse_v3_line(wchar_t *line, ConfigEntryFn fn, void *ctx)
{
    int v[10] = {0};
    int n = 0;
    wchar_t *cursor = line;
    while (n < 10 && parse_int(&cursor, &v[n])) ++n;
    if ((n != 7 && n != 10) || !*cursor) return false;
    OverlayConfig e;
    memset(&e, 0, sizeof(e));
    e.uid = s_legacy_uid++;
    e.path = cursor;
    split_script(cursor, &e);
    e.x = v[0];
    e.y = v[1];
    e.scale_pct = v[2];
    e.paused = (v[3] & 1) != 0;
    e.hidden = (v[3] & 2) != 0;
    e.topmost = v[4] != 0;
    e.locked = v[5] != 0;
    e.clickthrough = v[6] != 0;
    e.key_enabled = v[7] != 0;
    e.key_rgb = (uint32_t)v[8] & 0xFFFFFFu;
    e.key_tolerance = v[9];
    e.defer_script = false;
    fn(&e, ctx);
    return true;
}

static void parse_line(wchar_t *line, ConfigEntryFn fn, ConfigGroupFn group_fn, void *ctx)
{
    if (*line == L'#' || *line == 0) return;
    if (line[0] == L'g' && line[1] == L'\t') {
        parse_v4_group(line, group_fn, ctx);
        return;
    }
    if (line[0] == L'o' && line[1] == L'\t') {
        parse_v4_overlay(line, fn, ctx);
        return;
    }
    parse_v3_line(line, fn, ctx);
}

void config_load(ConfigEntryFn fn, ConfigGroupFn group_fn, void *ctx)
{
    wchar_t path[MAX_PATH];
    if (!config_file(path, MAX_PATH, L"config.txt")) {
        log_error("cannot resolve the settings folder");
        return;
    }
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND)
            log_warn("cannot open config (error %lu)", (unsigned long)err);
        return;
    }
    LARGE_INTEGER size;
    char *utf8 = NULL;
    wchar_t *text = NULL;
    DWORD got = 0;
    if (!GetFileSizeEx(f, &size)) {
        log_warn("cannot stat config (error %lu)", (unsigned long)GetLastError());
        goto done;
    }
    if (size.QuadPart <= 0 || size.QuadPart > 4 * 1024 * 1024) {
        log_warn("config size out of range: %lld byte(s)", (long long)size.QuadPart);
        goto done;
    }
    utf8 = (char *)malloc((size_t)size.QuadPart);
    if (!utf8) {
        log_error("out of memory reading config");
        goto done;
    }
    if (!ReadFile(f, utf8, (DWORD)size.QuadPart, &got, NULL)) {
        log_warn("cannot read config (error %lu)", (unsigned long)GetLastError());
        goto done;
    }
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, (int)got, NULL, 0);
    if (wlen <= 0) {
        log_warn("config is not valid UTF-8");
        goto done;
    }
    text = (wchar_t *)malloc(((size_t)wlen + 1) * sizeof(wchar_t));
    if (!text) {
        log_error("out of memory reading config");
        goto done;
    }
    MultiByteToWideChar(CP_UTF8, 0, utf8, (int)got, text, wlen);
    text[wlen] = 0;

    s_legacy_uid = 1;
    for (wchar_t *line = text; line && *line;) {
        wchar_t *nl = wcschr(line, L'\n');
        if (nl) *nl = 0;
        size_t n = wcslen(line);
        if (n && line[n - 1] == L'\r') line[n - 1] = 0;
        parse_line(line, fn, group_fn, ctx);
        line = nl ? nl + 1 : NULL;
    }
done:
    free(text);
    free(utf8);
    CloseHandle(f);
}

static bool write_all(HANDLE f, const char *data, size_t n)
{
    DWORD wrote = 0;
    if (!WriteFile(f, data, (DWORD)n, &wrote, NULL)) return false;
    if (wrote != (DWORD)n) {
        SetLastError(ERROR_WRITE_FAULT); /* Short write with a successful API call. */
        return false;
    }
    return true;
}

bool config_save(const ConfigSave *save)
{
    wchar_t dir[MAX_PATH], tmp[MAX_PATH], dst[MAX_PATH];
    if (!save) return false;
    if (!config_dir(dir, MAX_PATH)) {
        log_error("cannot resolve the settings folder");
        return false;
    }
    if (!CreateDirectoryW(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        log_warn("cannot create settings folder (error %lu)", (unsigned long)GetLastError());
    if (!config_file(tmp, MAX_PATH, L"config.tmp") || !config_file(dst, MAX_PATH, L"config.txt")) {
        log_error("cannot build the config path");
        return false;
    }

    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        log_error("cannot open config for writing (error %lu)", (unsigned long)GetLastError());
        return false;
    }
    SetLastError(ERROR_SUCCESS);
    bool ok = write_all(f, CONFIG_HEADER, sizeof(CONFIG_HEADER) - 1);

    for (int i = 0; ok && i < save->group_count; ++i) {
        const GroupConfig *g = &save->groups[i];
        char head[128];
        int hn = _snprintf_s(head, sizeof(head), _TRUNCATE, "g\t%llu\t%d\t",
                             (unsigned long long)g->uid, g->hidden ? 1 : 0);
        ok = hn > 0 && write_all(f, head, (size_t)hn);
        int fn = WideCharToMultiByte(CP_UTF8, 0, g->folder, -1, NULL, 0, NULL, NULL);
        char *fp = fn > 0 ? (char *)malloc((size_t)fn) : NULL;
        int nn = WideCharToMultiByte(CP_UTF8, 0, g->name, -1, NULL, 0, NULL, NULL);
        char *np = nn > 0 ? (char *)malloc((size_t)nn) : NULL;
        ok = ok && fp && np &&
             WideCharToMultiByte(CP_UTF8, 0, g->folder, -1, fp, fn, NULL, NULL) > 0 &&
             WideCharToMultiByte(CP_UTF8, 0, g->name, -1, np, nn, NULL, NULL) > 0 &&
             write_all(f, fp, (size_t)fn - 1) && write_all(f, "\t", 1) &&
             write_all(f, np, (size_t)nn - 1) && write_all(f, "\n", 1);
        free(fp);
        free(np);
    }

    for (int i = 0; ok && i < save->overlay_count; ++i) {
        const OverlayConfig *e = &save->overlays[i];
        char head[256];
        int hn = _snprintf_s(head, sizeof(head), _TRUNCATE,
                             "o\t%llu\t%llu\t%llu\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%u\t%d\t",
                             (unsigned long long)e->uid, (unsigned long long)e->master_uid,
                             (unsigned long long)e->group_uid, e->x, e->y, e->scale_pct,
                             (e->paused ? 1 : 0) | (e->hidden ? 2 : 0), e->topmost, e->locked,
                             e->clickthrough, e->key_enabled, (unsigned)e->key_rgb, e->key_tolerance);
        int pn = WideCharToMultiByte(CP_UTF8, 0, e->path, -1, NULL, 0, NULL, NULL);
        char *p = pn > 0 ? (char *)malloc((size_t)pn) : NULL;
        ok = hn > 0 && p && WideCharToMultiByte(CP_UTF8, 0, e->path, -1, p, pn, NULL, NULL) > 0 &&
             write_all(f, head, (size_t)hn) && write_all(f, p, (size_t)pn - 1);
        free(p);
        if (ok && e->script && *e->script) {
            int sn = WideCharToMultiByte(CP_UTF8, 0, e->script, -1, NULL, 0, NULL, NULL);
            char *sp = sn > 0 ? (char *)malloc((size_t)sn) : NULL;
            ok = sp && WideCharToMultiByte(CP_UTF8, 0, e->script, -1, sp, sn, NULL, NULL) > 0 &&
                 write_all(f, "\t", 1) && write_all(f, sp, (size_t)sn - 1);
            free(sp);
        }
        ok = ok && write_all(f, "\n", 1);
    }
    ok = ok && FlushFileBuffers(f);
    DWORD write_error = ok ? 0 : GetLastError();
    if (!ok && write_error == ERROR_SUCCESS) write_error = ERROR_WRITE_FAULT;
    CloseHandle(f);
    if (!ok) {
        log_error("cannot write config (error %lu)", (unsigned long)write_error);
        DeleteFileW(tmp);
        return false;
    }
    if (!MoveFileExW(tmp, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        log_error("cannot replace config (error %lu)", (unsigned long)GetLastError());
        DeleteFileW(tmp);
        return false;
    }
    return true;
}

bool config_delete_all(void)
{
    wchar_t dir[MAX_PATH + 1];
    if (!config_dir(dir, MAX_PATH)) {
        log_error("cannot resolve the settings folder");
        return false;
    }
    if (GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES) return true;
    size_t n = wcslen(dir);
    dir[n + 1] = 0; /* SHFileOperation wants a double-NUL terminated list. */
    SHFILEOPSTRUCTW op;
    memset(&op, 0, sizeof(op));
    op.wFunc = FO_DELETE;
    op.pFrom = dir;
    op.fFlags = FOF_NO_UI;
    bool ok = SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
    if (!ok) log_error("cannot delete settings folder");
    return ok;
}
