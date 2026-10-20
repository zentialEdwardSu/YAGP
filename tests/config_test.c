#include "config.h"

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct Capture {
    OverlayConfig overlays[4];
    wchar_t paths[4][MAX_PATH];
    wchar_t scripts[4][MAX_PATH];
    GroupConfig groups[4];
    wchar_t names[4][128];
    wchar_t folders[4][MAX_PATH];
    int on, gn;
} Capture;

static void copy_w(wchar_t *dst, size_t cap, const wchar_t *src)
{
    wcsncpy_s(dst, cap, src ? src : L"", _TRUNCATE);
}

static void overlay_cb(const OverlayConfig *e, void *ctx)
{
    Capture *c = (Capture *)ctx;
    if (c->on >= 4) return;
    int i = c->on++;
    c->overlays[i] = *e;
    copy_w(c->paths[i], MAX_PATH, e->path);
    copy_w(c->scripts[i], MAX_PATH, e->script);
    c->overlays[i].path = c->paths[i];
    c->overlays[i].script = c->scripts[i];
}

static void group_cb(const GroupConfig *e, void *ctx)
{
    Capture *c = (Capture *)ctx;
    if (c->gn >= 4) return;
    int i = c->gn++;
    c->groups[i] = *e;
    copy_w(c->names[i], 128, e->name);
    copy_w(c->folders[i], MAX_PATH, e->folder);
    c->groups[i].name = c->names[i];
    c->groups[i].folder = c->folders[i];
}

static int s_fail;

static void check(bool ok, const char *what)
{
    if (!ok) {
        printf("FAIL %s\n", what);
        ++s_fail;
    }
}

int main(void)
{
    wchar_t root[MAX_PATH], cfgdir[MAX_PATH], cfgfile[MAX_PATH];
    GetCurrentDirectoryW(ARRAYSIZE(root), root);
    wcscat_s(root, ARRAYSIZE(root), L"\\out\\config-test");
    config_test_set_dir(root);
    _snwprintf_s(cfgdir, ARRAYSIZE(cfgdir), _TRUNCATE, L"%s\\YAGP", root);
    CreateDirectoryW(root, NULL);
    CreateDirectoryW(cfgdir, NULL);
    _snwprintf_s(cfgfile, ARRAYSIZE(cfgfile), _TRUNCATE, L"%s\\config.txt", cfgdir);

    const wchar_t *v3 = L"# YAGP config v3\n"
                        L"100\t200\t50\t1\t1\t0\t0\t0\t0\t16\tC:\\pics\\a.gif\tbuiltin:patrol\n";
    int n = WideCharToMultiByte(CP_UTF8, 0, v3, -1, NULL, 0, NULL, NULL);
    char *v3u = n > 0 ? (char *)malloc((size_t)n) : NULL;
    if (!v3u) return 1;
    WideCharToMultiByte(CP_UTF8, 0, v3, -1, v3u, n, NULL, NULL);
    HANDLE f = CreateFileW(cfgfile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        free(v3u);
        return 1;
    }
    DWORD wrote = 0;
    WriteFile(f, v3u, (DWORD)n - 1, &wrote, NULL);
    CloseHandle(f);
    free(v3u);

    Capture cap;
    memset(&cap, 0, sizeof(cap));
    config_load(overlay_cb, group_cb, &cap);
    check(cap.on == 1 && cap.gn == 0, "v3 load count");
    check(cap.overlays[0].x == 100 && cap.overlays[0].y == 200 && cap.overlays[0].scale_pct == 50,
          "v3 fields");
    check(cap.overlays[0].paused && !cap.overlays[0].hidden, "v3 flags");
    check(wcscmp(cap.overlays[0].script, L"builtin:patrol") == 0, "v3 script");
    check(cap.overlays[0].uid == 1, "v3 uid");

    GroupConfig groups[1];
    memset(groups, 0, sizeof(groups));
    groups[0].uid = 10;
    groups[0].name = L"demo";
    groups[0].folder = L"C:\\pics\\pets";
    groups[0].hidden = true;
    OverlayConfig overlays[2];
    memset(overlays, 0, sizeof(overlays));
    overlays[0].uid = 1;
    overlays[0].group_uid = 10;
    overlays[0].path = L"C:\\pics\\a.gif";
    overlays[0].script = L"a.lua";
    overlays[0].x = 1;
    overlays[0].y = 2;
    overlays[0].scale_pct = 100;
    overlays[1].uid = 2;
    overlays[1].master_uid = 1;
    overlays[1].path = L"C:\\pics\\b.png";
    overlays[1].hidden = true;
    overlays[1].key_enabled = true;
    overlays[1].key_rgb = 0x112233;
    overlays[1].key_tolerance = 7;
    ConfigSave save = {groups, 1, overlays, 2};
    check(config_save(&save), "v4 save");

    char head[256] = "";
    f = CreateFileW(cfgfile, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD got = 0;
        ReadFile(f, head, sizeof(head) - 1, &got, NULL);
        head[got] = 0;
        CloseHandle(f);
    }
    check(strstr(head, "config v4") != NULL && strstr(head, "g\t10") != NULL, "v4 header/group");

    memset(&cap, 0, sizeof(cap));
    config_load(overlay_cb, group_cb, &cap);
    check(cap.on == 2 && cap.gn == 1, "v4 load count");
    check(cap.groups[0].uid == 10 && cap.groups[0].hidden &&
              wcscmp(cap.groups[0].name, L"demo") == 0, "v4 group");
    check(cap.overlays[0].group_uid == 10 && wcscmp(cap.overlays[0].script, L"a.lua") == 0,
          "v4 group member");
    check(cap.overlays[1].master_uid == 1 && cap.overlays[1].hidden &&
              cap.overlays[1].key_enabled && cap.overlays[1].key_rgb == 0x112233 &&
              cap.overlays[1].key_tolerance == 7, "v4 overlay fields");

    config_delete_all();
    RemoveDirectoryW(root);
    if (s_fail) return 1;
    printf("all config tests passed\n");
    return 0;
}
