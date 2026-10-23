#include "group.h"

#include <windows.h>
#include <wchar.h>

#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "applog.h"
#include "compose.h"
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

static void rollback_group(Group *g)
{
    Overlay **p = &g_app.overlays;
    while (*p) {
        Overlay *o = *p;
        if (o->group == g) {
            *p = o->next;
            overlay_destroy(o);
        } else {
            p = &o->next;
        }
    }
    group_destroy(g);
}

bool group_import_compose(const wchar_t *compose_path, wchar_t *err, size_t cap)
{
    if (err && cap) err[0] = 0;
    const wchar_t *file_base = wcsrchr(compose_path, L'\\');
    file_base = file_base ? file_base + 1 : compose_path;
    const wchar_t *ext = wcsrchr(file_base, L'.');
    if (!ext || _wcsicmp(ext, L".compose") != 0) {
        if (err) _snwprintf_s(err, cap, _TRUNCATE, TXT_GROUP_ERR_COMPOSE);
        return false;
    }

    ComposeDoc doc;
    if (!compose_parse_file(compose_path, &doc, err, cap)) return false;

    wchar_t root[GROUP_PATH_MAX];
    wcsncpy_s(root, ARRAYSIZE(root), doc.root, _TRUNCATE);
    normalize_folder(root);
    if (group_find_folder(root)) {
        if (err) _snwprintf_s(err, cap, _TRUNCATE, TXT_GROUP_ERR_EXISTS);
        compose_free(&doc);
        return false;
    }

    wchar_t base[128], unique[160];
    if (doc.name && *doc.name) wcsncpy_s(base, ARRAYSIZE(base), doc.name, _TRUNCATE);
    else leaf_name(root, base, ARRAYSIZE(base));
    unique_group_name(base, unique, ARRAYSIZE(unique));
    Group *g = group_create(unique, root);
    if (!g) {
        if (err) _snwprintf_s(err, cap, _TRUNCATE, TXT_GROUP_ERR_CREATE);
        compose_free(&doc);
        return false;
    }

    Overlay **created = (Overlay **)calloc((size_t)doc.count, sizeof(*created));
    if (!created) {
        if (err) _snwprintf_s(err, cap, _TRUNCATE, TXT_GROUP_ERR_CREATE);
        group_destroy(g);
        compose_free(&doc);
        return false;
    }

    bool ok = true;
    for (int i = 0; i < doc.count && ok; ++i) {
        ComposeMember *m = &doc.members[i];
        OverlayConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.path = m->media;
        cfg.script = m->script;
        cfg.x = m->x;
        cfg.y = m->y;
        cfg.scale_pct = m->scale_pct;
        cfg.paused = m->paused;
        cfg.topmost = m->topmost;
        cfg.locked = m->locked;
        cfg.clickthrough = m->clickthrough;
        cfg.hidden = m->hidden;
        cfg.key_enabled = m->key_enabled;
        cfg.key_rgb = m->key_rgb;
        cfg.key_tolerance = m->key_tolerance;
        cfg.defer_script = true;
        Overlay *o = overlay_create(&cfg, m->has_position, i);
        if (!o) {
            if (err) _snwprintf_s(err, cap, _TRUNCATE, TXT_GROUP_ERR_LOAD);
            ok = false;
            break;
        }
        Overlay **tail = &g_app.overlays;
        while (*tail) tail = &(*tail)->next;
        *tail = o;
        o->group = g;
        o->uid = app_uid_alloc();
        overlay_announce(o, true);
        created[i] = o;
    }

    for (int i = 0; i < doc.count && ok; ++i) {
        int leader = doc.members[i].leader_index;
        if (leader < 0) continue;
        if (!overlay_bind_leader(created[i], created[leader], err, cap)) {
            ok = false;
            break;
        }
    }

    if (ok) {
        for (int i = 0; i < doc.count; ++i) {
            Overlay *o = created[i];
            if (!o->leader && o->script_spec && *o->script_spec)
                overlay_set_script(o, o->script_spec);
        }
        char u8[MAX_PATH * 4];
        log_info("group imported from compose: %s (%d member(s))",
                 applog_u8(g->folder, u8, sizeof(u8)), doc.count);
        overlay_refresh_hook();
        app_changed();
    } else {
        rollback_group(g);
    }

    free(created);
    compose_free(&doc);
    return ok;
}

bool group_create_template(const wchar_t *path, wchar_t *err, size_t cap)
{
    if (err && cap) err[0] = 0;
    const wchar_t *fname = wcsrchr(path, L'\\');
    fname = fname ? fname + 1 : path;
    const wchar_t *ext = wcsrchr(fname, L'.');
    if (!ext || _wcsicmp(ext, L".compose") != 0) {
        if (err) _snwprintf_s(err, cap, _TRUNCATE, TXT_GROUP_ERR_COMPOSE);
        return false;
    }

    static const char TEMPLATE[] =
        "# YAGP 分组 compose 模板\n"
        "#\n"
        "# 1. 把这个文件放在分组素材所在目录；\n"
        "# 2. 把 media / script 改成实际文件名；路径相对本文件所在目录；\n"
        "# 3. 托盘 →“分组”→“导入分组…”，选择这个 .compose 文件。\n"
        "#\n"
        "# 只允许一层 leader/follower：有 leader 的成员不能再拥有 follower。\n"
        "# 未列在 members 中的文件不会导入。\n"
        "\n"
        "version: 1\n"
        "name: my-group\n"
        "\n"
        "members:\n"
        "  - media: pet.gif\n"
        "    script: pet.lua\n"
        "    # position: [200, 200]\n"
        "    # scale: 100\n"
        "    # paused: false        # 仅 GIF；静态图不能暂停\n"
        "    # hidden: false\n"
        "    # topmost: false\n"
        "    # locked: false\n"
        "    # clickthrough: false\n"
        "    # key:\n"
        "    #   enabled: false\n"
        "    #   color: '#00FF00'\n"
        "    #   tolerance: 16\n"
        "\n"
        "  # 需要 leader/follower 时取消下面的注释，并让 leader 指向上面的 media：\n"
        "  # - media: follower.png\n"
        "  #   script: follower.lua\n"
        "  #   leader: pet.gif\n";

    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        log_warn("cannot create compose template (error %lu)", (unsigned long)GetLastError());
        if (err) _snwprintf_s(err, cap, _TRUNCATE, TXT_GROUP_ERR_WRITE);
        return false;
    }
    DWORD len = (DWORD)strlen(TEMPLATE);
    DWORD written = 0;
    BOOL ok = WriteFile(h, TEMPLATE, len, &written, NULL);
    CloseHandle(h);
    if (!ok || written != len) {
        DeleteFileW(path);
        log_warn("cannot write compose template (error %lu)", (unsigned long)GetLastError());
        if (err) _snwprintf_s(err, cap, _TRUNCATE, TXT_GROUP_ERR_WRITE);
        return false;
    }
    return true;
}
