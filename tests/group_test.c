/* Compose group import test. Overlay/app functions are stubbed; the target
 * exercises required compose parsing, attributes, deferred scripts and auto
 * leader/follower binding. */
#include "app.h"
#include "compose.h"
#include "group.h"
#include "overlay.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>

App g_app;
static int s_failures;

uint64_t app_uid_alloc(void) { return ++g_app.next_uid; }
void app_changed(void) {}
void overlay_refresh_visibility(Overlay *o) { (void)o; }
void overlay_refresh_hook(void) {}
void overlay_announce(Overlay *o, bool added) { (void)o; (void)added; }
void app_remove_overlay(Overlay *o) { (void)o; }
void overlay_set_script(Overlay *o, const wchar_t *spec) { (void)o; (void)spec; }
void overlay_destroy(Overlay *o)
{
    if (!o) return;
    free(o->script_spec);
    free(o->path);
    free(o);
}
Overlay *overlay_create(const OverlayConfig *cfg, bool use_position, int cascade)
{
    Overlay *o = (Overlay *)calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->path = _wcsdup(cfg->path ? cfg->path : L"");
    o->script_spec = _wcsdup(cfg->script ? cfg->script : L"");
    o->scale_pct = cfg->scale_pct;
    o->paused = cfg->paused;
    o->topmost = cfg->topmost;
    o->locked = cfg->locked;
    o->clickthrough = cfg->clickthrough;
    o->hidden = cfg->hidden;
    o->flip_x = use_position;
    o->win_x = cascade;
    o->key_enabled = cfg->key_enabled;
    o->key_rgb = cfg->key_rgb;
    o->key_tolerance = cfg->key_tolerance;
    return o;
}
bool overlay_bind_leader(Overlay *follower, Overlay *leader, wchar_t *err, size_t cap)
{
    (void)err;
    (void)cap;
    if (!follower || !leader || follower == leader || follower->leader) return false;
    follower->leader = leader;
    return true;
}

static void check(bool ok, const char *what)
{
    if (!ok) {
        printf("FAIL %s\n", what);
        ++s_failures;
    }
}

static void write_file(const wchar_t *path, const char *text)
{
    FILE *f = _wfopen(path, L"wb");
    if (f) {
        fwrite(text, 1, strlen(text), f);
        fclose(f);
    }
}

static void cleanup(const wchar_t *dir)
{
    const wchar_t *names[] = {L"leader.gif", L"leader.lua", L"follower.png", L"yagp.compose",
                              L"generated.compose", L"bad.txt", L"pet.gif", L"pet.lua"};
    wchar_t p[MAX_PATH * 2];
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        _snwprintf_s(p, ARRAYSIZE(p), _TRUNCATE, L"%s\\%s", dir, names[i]);
        DeleteFileW(p);
    }
    RemoveDirectoryW(dir);
}

int main(void)
{
    wchar_t dir[MAX_PATH * 2];
    GetCurrentDirectoryW(ARRAYSIZE(dir), dir);
    wcscat_s(dir, ARRAYSIZE(dir), L"\\out\\group-compose-test");
    cleanup(dir);
    CreateDirectoryW(dir, NULL);

    wchar_t p[MAX_PATH * 2];
    const wchar_t *names[] = {L"leader.gif", L"leader.lua", L"follower.png", L"bad.txt"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        _snwprintf_s(p, ARRAYSIZE(p), _TRUNCATE, L"%s\\%s", dir, names[i]);
        write_file(p, "dummy");
    }
    _snwprintf_s(p, ARRAYSIZE(p), _TRUNCATE, L"%s\\yagp.compose", dir);
    write_file(p,
               "version: 1\n"
               "name: compose-demo\n"
               "members:\n"
               "  - media: leader.gif\n"
               "    script: leader.lua\n"
               "    position: [10, 20]\n"
               "    scale: 125\n"
               "    topmost: true\n"
               "  - media: follower.png\n"
               "    leader: leader.gif\n"
               "    locked: true\n");

    wchar_t err[512];
    bool ok = group_import_compose(p, err, ARRAYSIZE(err));
    check(ok, "compose import succeeds");
    if (!ok) {
        printf("  error: %ls\n", err);
        cleanup(dir);
        return 1;
    }
    check(g_app.groups != NULL, "group created");
    check(g_app.overlays != NULL && g_app.overlays->next != NULL, "two overlays created");
    if (g_app.overlays && g_app.overlays->next) {
        Overlay *leader = g_app.overlays;
        Overlay *follower = leader->next;
        check(wcscmp(leader->path, L"leader.gif") != 0, "absolute leader path");
        check(leader->scale_pct == 125 && leader->topmost, "leader attributes");
        check(leader->flip_x && leader->win_x == 0, "position and cascade used");
        check(follower->leader == leader, "auto leader/follower binding");
        check(follower->locked, "follower attributes");
        check(follower->script_spec == NULL || !*follower->script_spec,
              "follower has no script spec");
    }

    /* Importing the same directory again must be rejected. */
    check(!group_import_compose(p, err, ARRAYSIZE(err)), "duplicate group rejected");

    wchar_t bad[MAX_PATH * 2];
    _snwprintf_s(bad, ARRAYSIZE(bad), _TRUNCATE, L"%s\\bad.txt", dir);
    check(!group_import_compose(bad, err, ARRAYSIZE(err)), "non-compose file rejected");

    wchar_t generated[MAX_PATH * 2];
    _snwprintf_s(generated, ARRAYSIZE(generated), _TRUNCATE, L"%s\\generated.compose", dir);
    check(group_create_template(generated, err, ARRAYSIZE(err)), "template created");
    FILE *tf = _wfopen(generated, L"rb");
    if (tf) {
        char body[512] = {0};
        size_t got = fread(body, 1, sizeof(body) - 1, tf);
        fclose(tf);
        body[got] = 0;
        check(strstr(body, "version: 1") != NULL, "template contains version");
        check(strstr(body, "members:") != NULL, "template contains members");
    } else {
        check(false, "template readable");
    }
    check(!group_create_template(bad, err, ARRAYSIZE(err)), "template extension rejected");

    wchar_t pet[MAX_PATH * 2];
    _snwprintf_s(pet, ARRAYSIZE(pet), _TRUNCATE, L"%s\\pet.gif", dir);
    write_file(pet, "GIF89a");
    _snwprintf_s(pet, ARRAYSIZE(pet), _TRUNCATE, L"%s\\pet.lua", dir);
    write_file(pet, "-- test\n");
    ComposeDoc tdoc;
    check(compose_parse_file(generated, &tdoc, err, ARRAYSIZE(err)), "template parses");
    if (tdoc.count == 1) check(wcscmp(tdoc.name, L"my-group") == 0, "template name");
    compose_free(&tdoc);

    while (g_app.overlays) {
        Overlay *next = g_app.overlays->next;
        overlay_destroy(g_app.overlays);
        g_app.overlays = next;
    }
    while (g_app.groups) {
        Group *next = g_app.groups->next;
        group_destroy(g_app.groups);
        g_app.groups = next;
    }
    cleanup(dir);

    if (s_failures) return 1;
    printf("all group compose tests passed\n");
    return 0;
}
