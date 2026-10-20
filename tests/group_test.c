/* Group folder scanner regression test. The overlay/app functions are stubbed:
 * this target only exercises recursive pairing and ignores clip assets. */
#include "app.h"
#include "group.h"
#include "overlay.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>

App g_app;

uint64_t app_uid_alloc(void) { return ++g_app.next_uid; }
void app_changed(void) {}
void overlay_refresh_visibility(Overlay *o) { (void)o; }
void overlay_refresh_hook(void) {}
void overlay_announce(Overlay *o, bool added) { (void)o; (void)added; }
void app_remove_overlay(Overlay *o) { (void)o; }
Overlay *overlay_create(const OverlayConfig *cfg, bool use_position, int cascade)
{
    (void)cfg;
    (void)use_position;
    (void)cascade;
    return NULL;
}

static int s_failures;

static void touch(const wchar_t *path)
{
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
}

static void make_dir(const wchar_t *path)
{
    CreateDirectoryW(path, NULL);
}

static void cleanup(const wchar_t *dir)
{
    wchar_t p[MAX_PATH * 2];
    _snwprintf_s(p, ARRAYSIZE(p), _TRUNCATE, L"%s\\clip\\walk.gif", dir);
    DeleteFileW(p);
    _snwprintf_s(p, ARRAYSIZE(p), _TRUNCATE, L"%s\\clip", dir);
    RemoveDirectoryW(p);
    const wchar_t *names[] = {L"pet1.gif", L"pet1.lua", L"pet2.png", L"pet2.lua", L"orphan.gif"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        _snwprintf_s(p, ARRAYSIZE(p), _TRUNCATE, L"%s\\%s", dir, names[i]);
        DeleteFileW(p);
    }
    RemoveDirectoryW(dir);
}

int main(void)
{
    wchar_t dir[MAX_PATH * 2], path[MAX_PATH * 2];
    GetCurrentDirectoryW(ARRAYSIZE(dir), dir);
    wcscat_s(dir, ARRAYSIZE(dir), L"\\out\\group-scan-test");
    cleanup(dir);
    make_dir(dir);
    _snwprintf_s(path, ARRAYSIZE(path), _TRUNCATE, L"%s\\clip", dir);
    make_dir(path);
    const wchar_t *files[] = {L"pet1.gif", L"pet1.lua", L"pet2.png", L"pet2.lua", L"orphan.gif",
                              L"clip\\walk.gif"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        _snwprintf_s(path, ARRAYSIZE(path), _TRUNCATE, L"%s\\%s", dir, files[i]);
        touch(path);
    }

    GroupScan scan;
    wchar_t err[512];
    if (!group_scan_folder(dir, &scan, err, ARRAYSIZE(err))) {
        printf("FAIL scan: %ls\n", err);
        cleanup(dir);
        return 1;
    }
    if (scan.count != 2) {
        printf("FAIL count %d\n", scan.count);
        ++s_failures;
    }
    bool p1 = false, p2 = false;
    for (int i = 0; i < scan.count; ++i) {
        if (wcsstr(scan.items[i].media, L"pet1.gif") && wcsstr(scan.items[i].script, L"pet1.lua")) p1 = true;
        if (wcsstr(scan.items[i].media, L"pet2.png") && wcsstr(scan.items[i].script, L"pet2.lua")) p2 = true;
        if (wcsstr(scan.items[i].media, L"walk.gif") || wcsstr(scan.items[i].media, L"orphan.gif")) {
            printf("FAIL unpaired media imported: %ls\n", scan.items[i].media);
            ++s_failures;
        }
    }
    if (!p1 || !p2) {
        printf("FAIL missing pair p1=%d p2=%d\n", p1, p2);
        ++s_failures;
    }
    group_scan_free(&scan);
    cleanup(dir);

    /* Also check the checked-in group demo: two paired members, one ignored asset. */
    wchar_t demo[MAX_PATH * 2];
    GetCurrentDirectoryW(ARRAYSIZE(demo), demo);
    wcscat_s(demo, ARRAYSIZE(demo), L"\\example\\group");
    if (GetFileAttributesW(demo) != INVALID_FILE_ATTRIBUTES) {
        memset(&scan, 0, sizeof(scan));
        if (!group_scan_folder(demo, &scan, err, ARRAYSIZE(err))) {
            printf("FAIL demo scan: %ls\n", err);
            ++s_failures;
        } else {
            if (scan.count != 2) {
                printf("FAIL demo count %d\n", scan.count);
                ++s_failures;
            }
            for (int i = 0; i < scan.count; ++i) {
                if (wcsstr(scan.items[i].media, L"spin.gif")) {
                    printf("FAIL demo imported unpaired asset\n");
                    ++s_failures;
                }
            }
            group_scan_free(&scan);
        }
    }
    if (s_failures) return 1;
    printf("all group scan tests passed\n");
    return 0;
}
