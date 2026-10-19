/* File-output regression test for applog.c on top of rxi/log.c.
 *   cmake --build --preset release && out\Release\log_test.exe
 * Writes only into %TEMP%\yagp-log-test-<pid>, never into the user's profile. */
#include "applog.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int s_failures;
#define CHECK(cond, ...) do { if (!(cond)) { ++s_failures; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static wchar_t s_dir[MAX_PATH];
static int s_stops;

static void stop_cb(void *ctx)
{
    (void)ctx;
    ++s_stops;
}

static void make_temp_dir(void)
{
    wchar_t tmp[MAX_PATH];
    if (!GetTempPathW(ARRAYSIZE(tmp), tmp)) wcscpy_s(tmp, ARRAYSIZE(tmp), L".\\");
    _snwprintf_s(s_dir, ARRAYSIZE(s_dir), _TRUNCATE, L"%syagp-log-test-%lu", tmp,
                 (unsigned long)GetCurrentProcessId());
    RemoveDirectoryW(s_dir);
    if (CreateDirectoryW(s_dir, NULL)) return;
    /* Some sandboxes hide %TEMP% from spawned processes: fall back to the
     * current directory so the test still never touches the user profile. */
    _snwprintf_s(s_dir, ARRAYSIZE(s_dir), _TRUNCATE, L"yagp-log-test-%lu",
                 (unsigned long)GetCurrentProcessId());
    RemoveDirectoryW(s_dir);
    if (!CreateDirectoryW(s_dir, NULL)) {
        printf("cannot create a test directory (error %lu)\n", (unsigned long)GetLastError());
        exit(2);
    }
}

static void clear_dir(void)
{
    wchar_t pattern[MAX_PATH * 2];
    _snwprintf_s(pattern, ARRAYSIZE(pattern), _TRUNCATE, L"%s\\*", s_dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        wchar_t file[MAX_PATH * 2];
        _snwprintf_s(file, ARRAYSIZE(file), _TRUNCATE, L"%s\\%s", s_dir, fd.cFileName);
        DeleteFileW(file);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void remove_temp_dir(void)
{
    clear_dir();
    RemoveDirectoryW(s_dir);
}

static int collect_logs(wchar_t out[][MAX_PATH], int cap)
{
    wchar_t pattern[MAX_PATH * 2];
    _snwprintf_s(pattern, ARRAYSIZE(pattern), _TRUNCATE, L"%s\\yagp-*.log", s_dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (n < cap) {
            _snwprintf_s(out[n], MAX_PATH, _TRUNCATE, L"%s\\%s", s_dir, fd.cFileName);
            ++n;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

static char *read_file(const wchar_t *path, size_t *len)
{
    FILE *f = NULL;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return NULL;
    _fseeki64(f, 0, SEEK_END);
    long long size = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    char *buf = size >= 0 ? (char *)malloc((size_t)size + 1) : NULL;
    if (buf) {
        size_t got = fread(buf, 1, (size_t)size, f);
        buf[got] = 0;
        if (len) *len = got;
    }
    fclose(f);
    return buf;
}

static time_t make_time(int year, int mon, int day)
{
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = year - 1900;
    tm.tm_mon = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = 12;
    return mktime(&tm);
}

static void expected_log_path(wchar_t *out, size_t cap, time_t t)
{
    struct tm tm;
    localtime_s(&tm, &t);
    _snwprintf_s(out, cap, _TRUNCATE, L"%s\\yagp-%04d%02d%02d.log", s_dir, tm.tm_year + 1900,
                 tm.tm_mon + 1, tm.tm_mday);
}

static int count_lines(const char *text)
{
    int n = 0;
    for (const char *p = text; *p; ++p) {
        if (*p == '\n') ++n;
    }
    return n;
}

static size_t max_line_length(const char *text)
{
    size_t best = 0, cur = 0;
    for (const char *p = text;; ++p) {
        if (*p == '\n' || *p == 0) {
            if (cur > best) best = cur;
            cur = 0;
            if (*p == 0) break;
        } else {
            ++cur;
        }
    }
    return best;
}

/* The 4999-byte message is cut by the message buffer and marked with "...". */
static bool long_line_has_marker(const char *text)
{
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (n > 3000) return n >= 3 && strncmp(p + n - 3, "...", 3) == 0;
        if (!nl) break;
        p = nl + 1;
    }
    return false;
}

static void test_before_enable(void)
{
    log_info("before enable");
    wchar_t files[8][MAX_PATH];
    CHECK(collect_logs(files, 8) == 0, "a log file exists while logging is off");
    CHECK(applog_path()[0] == 0, "applog_path is not empty while off");
}

static void test_basic_file(void)
{
    clear_dir();
    time_t t0 = make_time(2026, 10, 19);
    applog_test_set_time(t0);
    DWORD err = 0;
    CHECK(applog_start(s_dir, &err), "start failed (error %lu)", (unsigned long)err);
    CHECK(applog_state() == APPLOG_ON, "state is not ON");
    wchar_t want[MAX_PATH * 2];
    expected_log_path(want, ARRAYSIZE(want), t0);
    CHECK(_wcsicmp(applog_path(), want) == 0, "unexpected path %ls", applog_path());

    log_info("first line");
    log_warn("second line");
    log_error("third line");
    log_info("multi\r\nline");
    char *longmsg = (char *)malloc(5000);
    if (longmsg) {
        memset(longmsg, 'x', 4999);
        longmsg[4999] = 0;
        log_info("%s", longmsg);
        free(longmsg);
    }

    /* Once off, nothing may reach the file, even if another line is logged. */
    size_t before_stop = 0;
    char *text = NULL;
    applog_stop();
    CHECK(applog_state() == APPLOG_OFF, "state is not OFF after stop");
    CHECK(applog_path()[0] == 0, "path not cleared after stop");

    text = read_file(want, &before_stop);
    CHECK(text != NULL, "cannot read the log file");
    if (text) {
        CHECK(strstr(text, "logging enabled:") != NULL, "missing logging enabled line");
        CHECK(strstr(text, "first line") && strstr(text, "second line") && strstr(text, "third line"),
              "missing a logged line");
        CHECK(strstr(text, "WARN  log_test.c") != NULL, "level/source formatting");
        CHECK(strstr(text, "multi\\r\\nline") != NULL, "CR/LF was not escaped");
        CHECK(strstr(text, "multi\r\nline") == NULL, "raw CR/LF reached the file");
        CHECK(strstr(text, "logging stopped") != NULL, "missing logging stopped line");
        CHECK(count_lines(text) == 7, "unexpected line count %d", count_lines(text));
        CHECK(max_line_length(text) <= 4095, "line too long: %llu", (unsigned long long)max_line_length(text));
        CHECK(long_line_has_marker(text), "long line lacks the truncation marker");
        free(text);
    }

    /* The same day appends instead of replacing. */
    size_t after_first = before_stop;
    CHECK(applog_start(s_dir, &err), "second start failed (error %lu)", (unsigned long)err);
    log_info("appended line");
    applog_stop();
    size_t after_append = 0;
    text = read_file(want, &after_append);
    CHECK(text != NULL, "cannot read the appended log file");
    if (text) {
        CHECK(strstr(text, "first line") != NULL, "earlier content was replaced");
        CHECK(strstr(text, "appended line") != NULL, "appended line missing");
        CHECK(after_append > after_first, "file did not grow");
        free(text);
    }

    /* Logging off: no file handle, no write. */
    size_t size_before = after_append;
    log_info("while off");
    size_t size_after = 0;
    text = read_file(want, &size_after);
    CHECK(text != NULL && size_after == size_before, "file changed while logging is off");
    free(text);
}

static void test_cap(void)
{
    clear_dir();
    applog_test_set_time(make_time(2026, 10, 20));
    s_stops = 0;
    DWORD err = 0;
    CHECK(applog_start(s_dir, &err), "cap start failed (error %lu)", (unsigned long)err);

    char line[160];
    memset(line, 'y', sizeof(line) - 1);
    line[sizeof(line) - 1] = 0;
    for (int i = 0; i < 200 && applog_state() == APPLOG_ON; ++i) log_info("%s", line);

    CHECK(applog_state() == APPLOG_OFF, "the session cap did not stop logging");
    CHECK(s_stops == 1, "stop callback called %d time(s)", s_stops);

    wchar_t files[8][MAX_PATH];
    CHECK(collect_logs(files, 8) == 1, "expected one capped log file");
    size_t len = 0;
    char *text = read_file(files[0], &len);
    CHECK(text != NULL, "cannot read the capped file");
    CHECK(len <= 8192 + 256, "file exceeds the cap: %llu", (unsigned long long)len);
    if (text) {
        CHECK(strstr(text, "session limit reached") != NULL, "missing cap notice");
        free(text);
    }

    size_t before = len;
    log_info("after cap");
    size_t after = 0;
    text = read_file(files[0], &after);
    CHECK(text != NULL && after == before, "logging continued after the cap");
    free(text);
}

static void test_date_rollover(void)
{
    clear_dir();
    time_t d1 = make_time(2026, 11, 1);
    time_t d2 = make_time(2026, 11, 2);
    applog_test_set_time(d1);
    DWORD err = 0;
    CHECK(applog_start(s_dir, &err), "rollover start failed (error %lu)", (unsigned long)err);
    log_info("day one");
    applog_test_set_time(d2);
    log_info("day two");
    applog_stop();

    wchar_t p1[MAX_PATH * 2], p2[MAX_PATH * 2];
    expected_log_path(p1, ARRAYSIZE(p1), d1);
    expected_log_path(p2, ARRAYSIZE(p2), d2);
    CHECK(GetFileAttributesW(p1) != INVALID_FILE_ATTRIBUTES, "no file for the first day");
    CHECK(GetFileAttributesW(p2) != INVALID_FILE_ATTRIBUTES, "no file for the second day");

    size_t len1 = 0;
    char *text = read_file(p1, &len1);
    CHECK(text != NULL, "cannot read the first day file");
    if (text) {
        CHECK(strstr(text, "day one") != NULL, "day one line missing");
        CHECK(strstr(text, "day two") == NULL, "day two line went into the old file");
        free(text);
    }
    size_t len2 = 0;
    text = read_file(p2, &len2);
    CHECK(text != NULL, "cannot read the second day file");
    if (text) {
        CHECK(strstr(text, "day two") != NULL, "day two line missing");
        free(text);
    }
}

typedef struct ThreadArg { int id; } ThreadArg;

static DWORD WINAPI log_thread(void *param)
{
    int id = (int)(intptr_t)param;
    for (int i = 0; i < 20; ++i) log_info("thread %d line %d", id, i);
    return 0;
}

static void test_threads(void)
{
    clear_dir();
    applog_test_set_time(make_time(2026, 12, 3));
    DWORD err = 0;
    CHECK(applog_start(s_dir, &err), "thread start failed (error %lu)", (unsigned long)err);

    enum { THREADS = 4 };
    HANDLE threads[THREADS];
    for (int i = 0; i < THREADS; ++i) {
        threads[i] = CreateThread(NULL, 0, log_thread, (void *)(intptr_t)i, 0, NULL);
        CHECK(threads[i] != NULL, "cannot create log thread %d", i);
    }
    for (int i = 0; i < THREADS; ++i) {
        if (threads[i]) {
            WaitForSingleObject(threads[i], 5000);
            CloseHandle(threads[i]);
        }
    }
    applog_stop();

    wchar_t files[8][MAX_PATH];
    CHECK(collect_logs(files, 8) == 1, "expected one thread-test log file");
    size_t len = 0;
    char *text = read_file(files[0], &len);
    CHECK(text != NULL, "cannot read the thread-test file");
    if (text) {
        CHECK(count_lines(text) == THREADS * 20 + 2, "unexpected line count %d", count_lines(text));
        for (int id = 0; id < THREADS; ++id) {
            for (int i = 0; i < 20; ++i) {
                char needle[64];
                _snprintf_s(needle, sizeof(needle), _TRUNCATE, "thread %d line %d", id, i);
                CHECK(strstr(text, needle) != NULL, "missing %s", needle);
            }
        }
        CHECK(max_line_length(text) <= 4095, "threaded line too long");
        free(text);
    }
}

static void test_console_output(void)
{
    clear_dir();
    wchar_t out_path[MAX_PATH * 2], err_path[MAX_PATH * 2];
    _snwprintf_s(out_path, ARRAYSIZE(out_path), _TRUNCATE, L"%s\\console-out.txt", s_dir);
    _snwprintf_s(err_path, ARRAYSIZE(err_path), _TRUNCATE, L"%s\\console-err.txt", s_dir);
    HANDLE out = CreateFileW(out_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE err = CreateFileW(err_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(out != INVALID_HANDLE_VALUE && err != INVALID_HANDLE_VALUE, "cannot create console test files");
    if (out == INVALID_HANDLE_VALUE || err == INVALID_HANDLE_VALUE) {
        if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
        if (err != INVALID_HANDLE_VALUE) CloseHandle(err);
        return;
    }

    HANDLE saved_out = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE saved_err = GetStdHandle(STD_ERROR_HANDLE);
    CHECK(SetStdHandle(STD_OUTPUT_HANDLE, out) != 0, "cannot set stdout");
    CHECK(SetStdHandle(STD_ERROR_HANDLE, err) != 0, "cannot set stderr");
    CHECK(applog_state() == APPLOG_OFF, "console test must not enable file logging");
    CHECK(applog_attach_console(), "cannot attach console to redirected handles");

    log_info("console info line");
    log_warn("console warn line");
    log_error("console error line");
    applog_detach_console();
    log_info("after detach line");

    SetStdHandle(STD_OUTPUT_HANDLE, saved_out);
    SetStdHandle(STD_ERROR_HANDLE, saved_err);
    CloseHandle(out);
    CloseHandle(err);

    size_t out_len = 0, err_len = 0;
    char *out_text = read_file(out_path, &out_len);
    char *err_text = read_file(err_path, &err_len);
    CHECK(out_text != NULL && err_text != NULL, "cannot read console test files");
    if (out_text && err_text) {
        CHECK(strstr(out_text, "console info line") != NULL, "INFO line missing from stdout");
        CHECK(strstr(out_text, "console warn line") == NULL, "WARN line leaked to stdout");
        CHECK(strstr(out_text, "after detach line") == NULL, "output continued after detach");
        CHECK(strstr(err_text, "console warn line") != NULL, "WARN line missing from stderr");
        CHECK(strstr(err_text, "console error line") != NULL, "ERROR line missing from stderr");
        CHECK(strstr(err_text, "console info line") == NULL, "INFO line leaked to stderr");
    }
    free(out_text);
    free(err_text);

    wchar_t files[8][MAX_PATH];
    CHECK(collect_logs(files, 8) == 0, "a log file was created without enabling logging");
}

int main(void)
{
    make_temp_dir();
    applog_init();
    applog_set_stop_callback(stop_cb, NULL);

    test_before_enable();
    test_basic_file();
    test_cap();
    test_date_rollover();
    test_threads();
    test_console_output();

    applog_shutdown();
    remove_temp_dir();
    if (s_failures) {
        printf("%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("all log tests passed\n");
    return 0;
}
