#include "group.h"

#include <windows.h>
#include <wchar.h>

#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "applog.h"
#include "config.h"
#include "i18n.h"
#include "log.h"
#include "overlay.h"

#define GROUP_MAX_MEMBERS 256
#define GROUP_MAX_DEPTH 32
#define GROUP_PATH_MAX (MAX_PATH * 4)

static wchar_t *dup_wcs(const wchar_t *s)
{
    if (!s) return NULL;
    size_t n = (wcslen(s) + 1) * sizeof(wchar_t);
    wchar_t *d = (wchar_t *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static void normalize_folder(wchar_t *s)
{
    size_t n = wcslen(s);
    while (n > 3 && (s[n - 1] == L'\\' || s[n - 1] == L'/')) s[--n] = 0;
}

static void make_absolute_folder(wchar_t *s, size_t cap)
{
    wchar_t full[GROUP_PATH_MAX];
    DWORD n = GetFullPathNameW(s, ARRAYSIZE(full), full, NULL);
    if (n > 0 && n < ARRAYSIZE(full)) wcsncpy_s(s, cap, full, _TRUNCATE);
    normalize_folder(s);
}

Group *group_create(const wchar_t *name, const wchar_t *folder)
{
    Group *g = (Group *)calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->uid = app_uid_alloc();
    g->name = dup_wcs(name && *name ? name : L"group");
    g->folder = (wchar_t *)calloc(GROUP_PATH_MAX + 1, sizeof(wchar_t));
    if (g->folder) wcsncpy_s(g->folder, GROUP_PATH_MAX + 1, folder ? folder : L"", _TRUNCATE);
    if (!g->name || !g->folder) {
        group_destroy(g);
        return NULL;
    }
    make_absolute_folder(g->folder, GROUP_PATH_MAX + 1);
    Group **tail = &g_app.groups;
    while (*tail) tail = &(*tail)->next;
    *tail = g;
    return g;
}

void group_destroy(Group *g)
{
    if (!g) return;
    for (Overlay *o = g_app.overlays; o; o = o->next) {
        if (o->group == g) o->group = NULL;
    }
    for (Group **p = &g_app.groups; *p; p = &(*p)->next) {
        if (*p == g) {
            *p = g->next;
            break;
        }
    }
    free(g->name);
    free(g->folder);
    free(g);
}

Group *group_find_uid(uint64_t uid)
{
    if (!uid) return NULL;
    for (Group *g = g_app.groups; g; g = g->next) {
        if (g->uid == uid) return g;
    }
    return NULL;
}

Group *group_find_folder(const wchar_t *folder)
{
    if (!folder) return NULL;
    wchar_t norm[GROUP_PATH_MAX];
    wcsncpy_s(norm, ARRAYSIZE(norm), folder, _TRUNCATE);
    make_absolute_folder(norm, ARRAYSIZE(norm));
    for (Group *g = g_app.groups; g; g = g->next) {
        if (_wcsicmp(g->folder, norm) == 0) return g;
    }
    return NULL;
}

int group_member_count(const Group *g)
{
    int n = 0;
    if (!g) return 0;
    for (const Overlay *o = g_app.overlays; o; o = o->next) {
        if (o->group == g) ++n;
    }
    return n;
}

bool group_is_empty(const Group *g)
{
    return group_member_count(g) == 0;
}

void group_set_hidden(Group *g, bool hidden)
{
    if (!g || g->hidden == hidden) return;
    g->hidden = hidden;
    for (Overlay *o = g_app.overlays; o; o = o->next) {
        if (o->group == g) overlay_refresh_visibility(o);
    }
    app_changed();
}

void group_after_member_removed(Group *g)
{
    if (!g || g->removing) return;
    if (group_is_empty(g)) {
        group_destroy(g);
        app_changed();
    }
}

void group_remove(Group *g)
{
    if (!g) return;
    g->removing = true;
    Overlay *o = g_app.overlays;
    while (o) {
        Overlay *next = o->next;
        if (o->group == g) app_remove_overlay(o);
        o = next;
    }
    group_destroy(g);
    app_changed();
}

static bool media_ext(const wchar_t *name, const wchar_t **ext)
{
    const wchar_t *dot = wcsrchr(name, L'.');
    if (!dot) return false;
    if (_wcsicmp(dot, L".gif") == 0 || _wcsicmp(dot, L".png") == 0 ||
        _wcsicmp(dot, L".jpg") == 0 || _wcsicmp(dot, L".jpeg") == 0) {
        *ext = dot;
        return true;
    }
    return false;
}

static void make_script_path(const wchar_t *media, const wchar_t *dir, wchar_t *out, size_t cap)
{
    const wchar_t *base = wcsrchr(media, L'\\');
    base = base ? base + 1 : media;
    const wchar_t *dot = wcsrchr(base, L'.');
    size_t stem = dot && dot > base ? (size_t)(dot - base) : wcslen(base);
    _snwprintf_s(out, cap, _TRUNCATE, L"%s\\%.*s.lua", dir, (int)stem, base);
}

static bool scan_add(GroupScan *scan, const wchar_t *media, const wchar_t *script)
{
    if (scan->count >= GROUP_MAX_MEMBERS) return false;
    GroupMember *items = (GroupMember *)realloc(scan->items, (size_t)(scan->count + 1) * sizeof(*items));
    if (!items) return false;
    scan->items = items;
    wchar_t *m = dup_wcs(media), *s = dup_wcs(script);
    if (!m || !s) {
        free(m);
        free(s);
        return false;
    }
    scan->items[scan->count].media = m;
    scan->items[scan->count].script = s;
    ++scan->count;
    return true;
}

static int member_cmp(const void *a, const void *b)
{
    const GroupMember *ma = (const GroupMember *)a;
    const GroupMember *mb = (const GroupMember *)b;
    return _wcsicmp(ma->media, mb->media);
}

static bool scan_dir(GroupScan *scan, const wchar_t *dir, int depth, wchar_t *err, size_t cap)
{
    if (depth > GROUP_MAX_DEPTH) {
        _snwprintf_s(err, cap, _TRUNCATE, TXT_GROUP_ERR_DEEP);
        return false;
    }
    wchar_t pattern[GROUP_PATH_MAX];
    if (_snwprintf_s(pattern, ARRAYSIZE(pattern), _TRUNCATE, L"%s\\*", dir) <= 0) {
        _snwprintf_s(err, cap, _TRUNCATE, TXT_GROUP_ERR_PATH);
        return false;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        _snwprintf_s(err, cap, _TRUNCATE, TXT_GROUP_ERR_SCAN);
        return false;
    }
    bool ok = true;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        wchar_t path[GROUP_PATH_MAX];
        if (_snwprintf_s(path, ARRAYSIZE(path), _TRUNCATE, L"%s\\%s", dir, fd.cFileName) <= 0) {
            _snwprintf_s(err, cap, _TRUNCATE, TXT_GROUP_ERR_PATH);
            ok = false;
            break;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            if (!scan_dir(scan, path, depth + 1, err, cap)) {
                ok = false;
                break;
            }
            continue;
        }
        const wchar_t *ext = NULL;
        if (!media_ext(fd.cFileName, &ext)) continue;
        wchar_t script[GROUP_PATH_MAX];
        make_script_path(fd.cFileName, dir, script, ARRAYSIZE(script));
        DWORD attr = GetFileAttributesW(script);
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (!scan_add(scan, path, script)) {
            _snwprintf_s(err, cap, _TRUNCATE, TXT_GROUP_ERR_MANY);
            ok = false;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

bool group_scan_folder(const wchar_t *folder, GroupScan *out, wchar_t *err, size_t cap)
{
    memset(out, 0, sizeof(*out));
    if (err && cap) err[0] = 0;
    if (!folder || !*folder) {
        if (err) _snwprintf_s(err, cap, _TRUNCATE, TXT_GROUP_ERR_NO_FOLDER);
        return false;
    }
    wchar_t dir[GROUP_PATH_MAX];
    wcsncpy_s(dir, ARRAYSIZE(dir), folder, _TRUNCATE);
    make_absolute_folder(dir, ARRAYSIZE(dir));
    if (!scan_dir(out, dir, 0, err, cap)) {
        group_scan_free(out);
        return false;
    }
    if (out->count == 0) {
        if (err) _snwprintf_s(err, cap, _TRUNCATE, TXT_GROUP_ERR_NO_PAIR);
        group_scan_free(out);
        return false;
    }
    qsort(out->items, (size_t)out->count, sizeof(*out->items), member_cmp);
    return true;
}

void group_scan_free(GroupScan *s)
{
    if (!s) return;
    for (int i = 0; i < s->count; ++i) {
        free(s->items[i].media);
        free(s->items[i].script);
    }
    free(s->items);
    memset(s, 0, sizeof(*s));
}

static void unique_group_name(const wchar_t *base, wchar_t *out, size_t cap)
{
    wcsncpy_s(out, cap, base, _TRUNCATE);
    for (int n = 2; n < 1000; ++n) {
        bool used = false;
        for (Group *g = g_app.groups; g; g = g->next) {
            if (_wcsicmp(g->name, out) == 0) {
                used = true;
                break;
            }
        }
        if (!used) return;
        _snwprintf_s(out, cap, _TRUNCATE, L"%s (%d)", base, n);
    }
}

static void leaf_name(const wchar_t *folder, wchar_t *out, size_t cap)
{
    wchar_t tmp[GROUP_PATH_MAX];
    wcsncpy_s(tmp, ARRAYSIZE(tmp), folder, _TRUNCATE);
    normalize_folder(tmp);
    const wchar_t *p = wcsrchr(tmp, L'\\');
    p = p ? p + 1 : tmp;
    if (!*p) p = L"group";
    wcsncpy_s(out, cap, p, _TRUNCATE);
}

bool group_import_folder(const wchar_t *folder, wchar_t *err, size_t cap)
{
    if (err && cap) err[0] = 0;
    if (group_find_folder(folder)) {
        if (err) _snwprintf_s(err, cap, _TRUNCATE, TXT_GROUP_ERR_EXISTS);
        return false;
    }
    GroupScan scan;
    if (!group_scan_folder(folder, &scan, err, cap)) return false;

    wchar_t name[128], unique[160];
    leaf_name(folder, name, ARRAYSIZE(name));
    unique_group_name(name, unique, ARRAYSIZE(unique));
    Group *g = group_create(unique, folder);
    if (!g) {
        group_scan_free(&scan);
        if (err) _snwprintf_s(err, cap, _TRUNCATE, TXT_GROUP_ERR_CREATE);
        return false;
    }

    int created = 0;
    for (int i = 0; i < scan.count; ++i) {
        OverlayConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.path = scan.items[i].media;
        cfg.script = scan.items[i].script;
        cfg.scale_pct = 100;
        cfg.key_tolerance = 16;
        Overlay *o = overlay_create(&cfg, false, created);
        if (!o) continue;
        Overlay **tail = &g_app.overlays;
        while (*tail) tail = &(*tail)->next;
        *tail = o;
        o->group = g;
        o->uid = app_uid_alloc();
        overlay_announce(o, true);
        ++created;
    }
    group_scan_free(&scan);
    if (created == 0) {
        group_destroy(g);
        if (err) _snwprintf_s(err, cap, _TRUNCATE, TXT_GROUP_ERR_LOAD);
        return false;
    }
    char u8[MAX_PATH * 4];
    log_info("group imported: %s (%d member(s))", applog_u8(g->folder, u8, sizeof(u8)), created);
    overlay_refresh_hook();
    app_changed();
    return true;
}
