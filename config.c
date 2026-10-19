#include "config.h"
#include "log.h"

#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_HEADER "# YAGP config v3: x\ty\tscale%\tflags(1=paused,2=hidden)\ttopmost\tlocked\tclickthrough\tkey_on\tkey_rgb\tkey_tol\tpath[\tscript]\n"

bool config_dir(wchar_t *out, size_t cap)
{
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

/* v1 lines carry 7 numeric fields, v2/v3 lines 10. Paths are absolute
 * ("C:\..." or "\\server"), so they never parse as a number. v3 may append
 * "\t<script spec>"; Windows paths cannot contain a tab, so the first tab
 * after the numbers ends the GIF path. */
static void parse_line(wchar_t *line, ConfigEntryFn fn, void *ctx)
{
    int v[10] = {0};
    int n = 0;
    wchar_t *cursor = line;
    if (*line == L'#' || *line == 0) return;
    while (n < 10 && parse_int(&cursor, &v[n])) ++n;
    if ((n != 7 && n != 10) || !*cursor) return;
    OverlayConfig e;
    e.path = cursor;
    e.script = L"";
    wchar_t *tab = wcschr(cursor, L'	');
    if (tab) {
        *tab = 0;
        e.script = tab + 1;
    }
    e.x = v[0];
    e.y = v[1];
    e.scale_pct = v[2];
    /* Field 4 is a bit set: 1 = paused, 2 = hidden. Older builds read any
     * non-zero value as "paused", so a hidden GIF shows up paused there. */
    e.paused = (v[3] & 1) != 0;
    e.hidden = (v[3] & 2) != 0;
    e.topmost = v[4] != 0;
    e.locked = v[5] != 0;
    e.clickthrough = v[6] != 0;
    e.key_enabled = v[7] != 0;
    e.key_rgb = (uint32_t)v[8] & 0xFFFFFFu;
    e.key_tolerance = v[9];
    fn(&e, ctx);
}

void config_load(ConfigEntryFn fn, void *ctx)
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

    for (wchar_t *line = text; line && *line;) {
        wchar_t *nl = wcschr(line, L'\n');
        if (nl) *nl = 0;
        size_t n = wcslen(line);
        if (n && line[n - 1] == L'\r') line[n - 1] = 0;
        parse_line(line, fn, ctx);
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

bool config_save(const OverlayConfig *entries, int count)
{
    wchar_t dir[MAX_PATH], tmp[MAX_PATH], dst[MAX_PATH];
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
    for (int i = 0; ok && i < count; ++i) {
        const OverlayConfig *e = &entries[i];
        char head[192];
        int hn = _snprintf_s(head, sizeof(head), _TRUNCATE, "%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%u\t%d\t",
                             e->x, e->y, e->scale_pct, (e->paused ? 1 : 0) | (e->hidden ? 2 : 0),
                             e->topmost, e->locked, e->clickthrough,
                             e->key_enabled, (unsigned)e->key_rgb, e->key_tolerance);
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
