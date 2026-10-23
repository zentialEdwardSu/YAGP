#include "compose.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>

static int failures;

static void check(bool ok, const char *what)
{
    if (!ok) {
        printf("FAIL %s\n", what);
        ++failures;
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

static void join(wchar_t *out, size_t cap, const wchar_t *dir, const wchar_t *name)
{
    _snwprintf_s(out, cap, _TRUNCATE, L"%ls\\%ls", dir, name);
}

int wmain(void)
{
    wchar_t dir[MAX_PATH];
    GetTempPathW(ARRAYSIZE(dir), dir);
    wcscat_s(dir, ARRAYSIZE(dir), L"yagp-compose-test");
    CreateDirectoryW(dir, NULL);

    wchar_t media[MAX_PATH], image[MAX_PATH], script[MAX_PATH], compose[MAX_PATH];
    join(media, ARRAYSIZE(media), dir, L"leader.gif");
    join(image, ARRAYSIZE(image), dir, L"follower.png");
    join(script, ARRAYSIZE(script), dir, L"leader.lua");
    join(compose, ARRAYSIZE(compose), dir, L"yagp.compose");
    write_file(media, "GIF89a");
    write_file(image, "\x89PNG");
    write_file(script, "-- test\n");

    write_file(compose,
               "version: 1\n"
               "name: sample\n"
               "members:\n"
               "  - media: leader.gif\n"
               "    script: leader.lua\n"
               "    position: [120, 140]\n"
               "    scale: 125\n"
               "    topmost: true\n"
               "    key:\n"
               "      enabled: true\n"
               "      color: '#112233'\n"
               "      tolerance: 7\n"
               "  - media: follower.png\n"
               "    leader: leader.gif\n");
    ComposeDoc doc;
    wchar_t err[512];
    check(compose_parse_file(compose, &doc, err, ARRAYSIZE(err)), "valid compose parses");
    if (doc.count == 2) {
        check(doc.members[0].has_position && doc.members[0].x == 120 && doc.members[0].y == 140,
              "position parsed");
        check(doc.members[0].scale_pct == 125, "scale parsed");
        check(doc.members[0].topmost, "topmost parsed");
        check(doc.members[0].key_enabled && doc.members[0].key_rgb == 0x112233,
              "key parsed");
        check(doc.members[1].leader_index == 0, "leader resolved");
        check(wcscmp(doc.name, L"sample") == 0, "name parsed");
    } else {
        check(false, "member count");
    }
    compose_free(&doc);

    /* The checked-in commented sample must be valid once its placeholder media
     * and scripts exist. */
    const wchar_t *sample_names[] = {L"leader.gif", L"follower.png", L"solo.gif",
                                     L"leader.lua", L"follower.lua", L"solo.lua"};
    for (size_t i = 0; i < sizeof(sample_names) / sizeof(sample_names[0]); ++i) {
        join(media, ARRAYSIZE(media), dir, sample_names[i]);
        write_file(media, wcsstr(sample_names[i], L".lua") ? "-- test\n" : "GIF89a");
    }
    wchar_t sample[MAX_PATH];
    GetCurrentDirectoryW(ARRAYSIZE(sample), sample);
    wcscat_s(sample, ARRAYSIZE(sample), L"\\example\\yagp.compose.sample");
    if (GetFileAttributesW(sample) != INVALID_FILE_ATTRIBUTES) {
        CopyFileW(sample, compose, FALSE);
        check(compose_parse_file(compose, &doc, err, ARRAYSIZE(err)), "sample compose parses");
        check(doc.count == 3 && doc.members[1].leader_index == 0,
              "sample leader/member count");
        compose_free(&doc);
    }

    write_file(compose,
               "version: 1\nmembers:\n  - media: follower.png\n    paused: true\n");
    check(!compose_parse_file(compose, &doc, err, ARRAYSIZE(err)), "still paused rejected");
    compose_free(&doc);

    write_file(compose,
               "version: 1\nmembers:\n  - media: ../escape.gif\n");
    check(!compose_parse_file(compose, &doc, err, ARRAYSIZE(err)), "path escape rejected");
    compose_free(&doc);

    write_file(compose,
               "version: 1\nmembers:\n  - media: leader.gif\n  - media: follower.png\n"
               "    leader: leader.gif\n  - media: leader.gif\n    leader: follower.png\n");
    check(!compose_parse_file(compose, &doc, err, ARRAYSIZE(err)), "duplicate/chain rejected");
    compose_free(&doc);

    DeleteFileW(compose);
    DeleteFileW(media);
    DeleteFileW(image);
    DeleteFileW(script);
    for (size_t i = 0; i < sizeof(sample_names) / sizeof(sample_names[0]); ++i) {
        join(media, ARRAYSIZE(media), dir, sample_names[i]);
        DeleteFileW(media);
    }
    RemoveDirectoryW(dir);
    if (failures) return 1;
    printf("all compose tests passed\n");
    return 0;
}
