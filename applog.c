#include "applog.h"
#include "log.h"

#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One line, including the prefix. Longer messages are escaped as far as they
 * fit and closed with "...", so a call can never produce two lines. */
#define APPLOG_LINE_MAX 4096
#define APPLOG_MSG_MAX  3072

/* Bytes written since logging was enabled, i.e. per session, not per file. */
#ifndef YAGP_APPLOG_LIMIT
#define YAGP_APPLOG_LIMIT (16u * 1024u * 1024u)
#endif

typedef struct AppLog {
    CRITICAL_SECTION cs;
    bool initialized;
    bool enabled;
    FILE *fp;
    wchar_t path[MAX_PATH * 2];
    wchar_t path_out[MAX_PATH * 2]; /* Snapshot returned by applog_path(). */
    wchar_t last_dir[MAX_PATH];
    int year, month, day;     /* Local date of the open file. */
    size_t written;           /* Since enable. */
    bool roll_warned;
    void (*stop_cb)(void *);
    void *stop_ctx;
    bool starting;            /* applog_start is writing its first line. */
    /* Terminal mirror (--verbose). Inherited handles are not owned. */
    bool console;
    bool console_attached;    /* This process called AttachConsole. */
    HANDLE con_out, con_err;
    DWORD con_out_type, con_err_type;
    bool con_out_owned, con_err_owned;
#ifdef YAGP_APPLOG_TEST
    time_t fake_now;
#endif
} AppLog;

static AppLog s;

static void now_tm(struct tm *out)
{
#ifdef YAGP_APPLOG_TEST
    time_t t = s.fake_now ? s.fake_now : time(NULL);
#else
    time_t t = time(NULL);
#endif
    if (localtime_s(out, &t) != 0) memset(out, 0, sizeof(*out));
}

const char *applog_u8(const wchar_t *w, char *buf, size_t cap)
{
    if (!buf || cap == 0) return "";
    buf[0] = 0;
    if (!w || !*w) return buf;
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)cap, NULL, NULL) > 0) return buf;
    /* Pathological input: keep an ASCII approximation rather than nothing. */
    size_t i = 0;
    for (; w[i] && i + 1 < cap; ++i) buf[i] = w[i] < 0x80 ? (char)w[i] : '?';
    buf[i] = 0;
    return buf;
}

/* __FILE__ may be an absolute build path; keep only the file name. */
static const char *base_name(const char *file)
{
    const char *a = strrchr(file, '\\');
    const char *b = strrchr(file, '/');
    if (b > a) a = b;
    return a ? a + 1 : file;
}

static int utf8_to_wide(const char *text, wchar_t *out, int cap)
{
    /* CP_UTF8 without MB_ERR_INVALID_CHARS only fails when out is too small;
     * retrying with CP_ACP cannot help and would just hide bugs. */
    return MultiByteToWideChar(CP_UTF8, 0, text, -1, out, cap);
}

static void debugger_mirror(const char *text)
{
    wchar_t wide[APPLOG_LINE_MAX];
    if (utf8_to_wide(text, wide, ARRAYSIZE(wide)) > 0) OutputDebugStringW(wide);
}

static bool handle_ok(HANDLE h)
{
    return h != NULL && h != INVALID_HANDLE_VALUE;
}

static bool write_all(HANDLE h, const char *data, size_t len)
{
    while (len > 0) {
        DWORD chunk = len > (size_t)0x7FFFFFFFu ? 0x7FFFFFFFu : (DWORD)len;
        DWORD wrote = 0;
        if (!WriteFile(h, data, chunk, &wrote, NULL) || wrote == 0) return false;
        data += wrote;
        len -= wrote;
    }
    return true;
}

/* Closes only the handles this layer opened; inherited stdout/stderr stay. */
static void console_disable_locked(void)
{
    if (s.con_out_owned && handle_ok(s.con_out)) CloseHandle(s.con_out);
    if (s.con_err_owned && handle_ok(s.con_err)) CloseHandle(s.con_err);
    s.con_out = s.con_err = NULL;
    s.con_out_type = s.con_err_type = FILE_TYPE_UNKNOWN;
    s.con_out_owned = s.con_err_owned = false;
    s.console = false;
}

/* Called by the sink with the log lock held. Never calls log_* again. */
static void console_write(int level, const char *line)
{
    if (!s.console) return;
    bool is_err = level >= LOG_WARN;
    HANDLE h = is_err ? s.con_err : s.con_out;
    DWORD type = is_err ? s.con_err_type : s.con_out_type;
    if (!handle_ok(h)) { /* Fall back to the other stream. */
        is_err = !is_err;
        h = is_err ? s.con_err : s.con_out;
        type = is_err ? s.con_err_type : s.con_out_type;
    }
    if (!handle_ok(h)) return;

    size_t len = strlen(line);
    bool ok;
    if (type == FILE_TYPE_CHAR) {
        wchar_t wide[APPLOG_LINE_MAX + 2];
        int n = utf8_to_wide(line, wide, APPLOG_LINE_MAX + 1);
        if (n <= 0) {
            ok = false;
        } else {
            wide[n - 1] = L'\r';
            wide[n] = L'\n';
            wide[n + 1] = 0;
            DWORD wrote = 0;
            ok = WriteConsoleW(h, wide, (DWORD)(n + 1), &wrote, NULL) && wrote == (DWORD)(n + 1);
        }
        if (!ok) { /* Maybe the handle is redirected after all. */
            ok = write_all(h, line, len) && write_all(h, "\n", 1);
        }
    } else {
        ok = write_all(h, line, len) && write_all(h, "\n", 1);
    }
    if (!ok) console_disable_locked();
}

/* Picks a usable handle for one standard stream. Existing redirected handles
 * win so that pipes and files work without a console. */
static void console_get_stream(DWORD which, bool has_console, HANDLE *out, DWORD *type, bool *owned)
{
    HANDLE std = GetStdHandle(which);
    DWORD t = handle_ok(std) ? GetFileType(std) : FILE_TYPE_UNKNOWN;
    if (handle_ok(std) && (t == FILE_TYPE_DISK || t == FILE_TYPE_PIPE)) {
        *out = std;
        *type = t;
        *owned = false;
        return;
    }
    if (has_console) {
        HANDLE h = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (handle_ok(h)) {
            *out = h;
            *type = GetFileType(h);
            *owned = true;
            return;
        }
        if (handle_ok(std) && t == FILE_TYPE_CHAR) {
            *out = std;
            *type = t;
            *owned = false;
            return;
        }
    }
    *out = NULL;
    *type = FILE_TYPE_UNKNOWN;
    *owned = false;
}

/* "<timestamp> <LEVEL> <file>[:<line>]: <message>" with CR/LF escaped. tm is
 * shared with the caller so the timestamp and the date rollover agree. */
static void format_line(log_Event *ev, char *out, size_t cap, const struct tm *tm)
{
    char stamp[32] = "";
    if (!strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", tm)) stamp[0] = 0;

    int level = ev->level;
    if (level < LOG_TRACE || level > LOG_FATAL) level = LOG_INFO;
    const char *file = base_name(ev->file ? ev->file : "?");

    int n;
    if (ev->line > 0) {
        n = _snprintf_s(out, cap, _TRUNCATE, "%s %-5s %s:%d: ", stamp, log_level_string(level), file, ev->line);
    } else {
        n = _snprintf_s(out, cap, _TRUNCATE, "%s %-5s %s: ", stamp, log_level_string(level), file);
    }
    if (n < 0) n = (int)strlen(out);
    size_t at = (size_t)n;
    if (at >= cap) {
        out[cap - 1] = 0;
        return;
    }

    char msg[APPLOG_MSG_MAX];
    int needed = vsnprintf(msg, sizeof(msg), ev->fmt, ev->ap);
    if (needed < 0) msg[0] = 0;
    bool truncated = needed < 0 || needed >= (int)sizeof(msg);

    for (const char *p = msg; *p; ++p) {
        const char *rep = *p == '\r' ? "\\r" : *p == '\n' ? "\\n" : NULL;
        size_t need = rep ? 2 : 1;
        if (at + need + 3 >= cap) { /* Room for "..." and the terminator. */
            truncated = true;
            break;
        }
        if (rep) {
            out[at++] = '\\';
            out[at++] = rep[1];
        } else {
            out[at++] = *p;
        }
    }
    if (truncated) {
        if (at + 3 < cap) {
            out[at++] = '.';
            out[at++] = '.';
            out[at++] = '.';
        } else {
            at = cap - 1; /* No room for the marker: keep the prefix intact. */
        }
    }
    out[at] = 0;
}

/* Timestamped, unescaped line for messages the applog itself generates while
 * the normal callback path must not be re-entered. */
static void write_raw_locked(const char *level, const char *text)
{
    if (!s.fp) return;
    struct tm tm;
    char stamp[32] = "";
    now_tm(&tm);
    if (!strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm)) stamp[0] = 0;
    fprintf(s.fp, "%s %-5s applog: %s\n", stamp, level, text);
    fflush(s.fp);
}

/* Flushes and closes the file. Called with cs held. */
static void close_file_locked(void)
{
    if (s.fp) {
        fflush(s.fp);
        fclose(s.fp);
        s.fp = NULL;
    }
    s.enabled = false;
    s.path[0] = 0;
}

/* Closes the file, turns logging off and tells the host. Called with cs held,
 * from the sink: it must not call log_* again. */
static void stop_locked(const char *reason)
{
    if (reason && s.fp) write_raw_locked("INFO", reason);
    close_file_locked();
    if (reason) {
        char note[256];
        _snprintf_s(note, sizeof(note), _TRUNCATE, "applog: %s", reason);
        debugger_mirror(note);
    }
    void (*cb)(void *) = s.stop_cb;
    void *ctx = s.stop_ctx;
    /* applog_start reports its own failure with a message box; it must not
     * also trigger the "logging stopped" balloon. */
    if (cb && !s.starting) cb(ctx);
}

static bool build_path(const wchar_t *dir, wchar_t *out, size_t cap, const struct tm *tm)
{
    size_t n = wcslen(dir);
    const wchar_t *sep = n && (dir[n - 1] == L'\\' || dir[n - 1] == L'/') ? L"" : L"\\";
    return _snwprintf_s(out, cap, _TRUNCATE, L"%s%s" L"yagp-%04d%02d%02d.log", dir, sep,
                        tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday) > 0;
}

/* Opens (creating if needed) the dated file for append. On success the old
 * file, if any, is closed. On failure the old file is left untouched. */
static bool open_for_date(const wchar_t *dir, const struct tm *tm, DWORD *error)
{
    wchar_t path[MAX_PATH * 2];
    if (!build_path(dir, path, ARRAYSIZE(path), tm)) {
        if (error) *error = ERROR_BUFFER_OVERFLOW;
        return false;
    }
    HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        if (error) *error = GetLastError();
        return false;
    }
    int fd = _open_osfhandle((intptr_t)h, _O_APPEND | _O_BINARY);
    if (fd < 0) {
        CloseHandle(h);
        if (error) *error = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
    FILE *fp = _fdopen(fd, "ab");
    if (!fp) {
        _close(fd);
        if (error) *error = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
    if (s.fp) fclose(s.fp);
    s.fp = fp;
    wcscpy_s(s.path, ARRAYSIZE(s.path), path);
    s.year = tm->tm_year;
    s.month = tm->tm_mon;
    s.day = tm->tm_mday;
    s.roll_warned = false;
    return true;
}

/* Called by rxi/log.c with its lock held (see applog_lock). */
static void applog_sink(log_Event *ev)
{
    /* With no file, no terminal and no debugger, formatting TRACE/DEBUG would
     * be pure overhead. INFO and above still reach OutputDebugString/DebugView. */
    if (!s.enabled && !s.console && !IsDebuggerPresent() && ev->level <= LOG_DEBUG) return;

    struct tm tm;
    now_tm(&tm);
    char line[APPLOG_LINE_MAX];
    format_line(ev, line, sizeof(line), &tm);
    debugger_mirror(line);
    console_write(ev->level, line);

    if (!s.enabled || !s.fp) return;

    if (tm.tm_year != s.year || tm.tm_mon != s.month || tm.tm_mday != s.day) {
        DWORD err = 0;
        if (!open_for_date(s.last_dir, &tm, &err)) {
            /* Remember the new day even though the file could not be opened:
             * otherwise every following line would retry CreateFileW. Keep
             * writing to the old file and warn only once. */
            if (!s.roll_warned) {
                write_raw_locked("WARN", "cannot open the new day's log file; continuing with the current file");
                s.roll_warned = true;
            }
            s.year = tm.tm_year;
            s.month = tm.tm_mon;
            s.day = tm.tm_mday;
        }
    }

    size_t len = strlen(line);
    if (s.written + len + 1 > (size_t)YAGP_APPLOG_LIMIT) {
        char reason[128];
        _snprintf_s(reason, sizeof(reason), _TRUNCATE, "logging stopped: %u MiB session limit reached",
                    (unsigned)(YAGP_APPLOG_LIMIT / (1024u * 1024u)));
        stop_locked(reason);
        return;
    }
    if (fwrite(line, 1, len, s.fp) != len || fputc('\n', s.fp) == EOF || fflush(s.fp) != 0) {
        stop_locked("cannot write the log file");
        return;
    }
    s.written += len + 1;
}

static void applog_lock(bool lock, void *udata)
{
    (void)udata;
    if (lock) EnterCriticalSection(&s.cs);
    else LeaveCriticalSection(&s.cs);
}

void applog_init(void)
{
    if (s.initialized) return;
    InitializeCriticalSection(&s.cs);
    s.initialized = true;
    log_set_lock(applog_lock, NULL);
    log_set_quiet(true);
    log_set_level(LOG_TRACE);
    log_add_callback(applog_sink, NULL, LOG_TRACE);
}

void applog_shutdown(void)
{
    if (!s.initialized) return;
    applog_stop();
    applog_detach_console();
    s.initialized = false; /* The callback stays registered; it stops writing. */
}

bool applog_attach_console(void)
{
    if (!s.initialized) return false;
    EnterCriticalSection(&s.cs);
    if (s.console) {
        LeaveCriticalSection(&s.cs);
        return true;
    }

    bool has_console = GetConsoleWindow() != NULL;
    bool attached = false;
    /* If both streams are already redirected to usable handles, there is no
     * reason to attach to the parent console at all. */
    HANDLE std_out = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE std_err = GetStdHandle(STD_ERROR_HANDLE);
    DWORD std_out_type = handle_ok(std_out) ? GetFileType(std_out) : FILE_TYPE_UNKNOWN;
    DWORD std_err_type = handle_ok(std_err) ? GetFileType(std_err) : FILE_TYPE_UNKNOWN;
    bool out_ready = handle_ok(std_out) &&
                     (std_out_type == FILE_TYPE_DISK || std_out_type == FILE_TYPE_PIPE);
    bool err_ready = handle_ok(std_err) &&
                     (std_err_type == FILE_TYPE_DISK || std_err_type == FILE_TYPE_PIPE);
    if ((!out_ready || !err_ready) && !has_console && AttachConsole(ATTACH_PARENT_PROCESS)) {
        has_console = true;
        attached = true;
        s.console_attached = true;
    }

    HANDLE out, err;
    DWORD out_type, err_type;
    bool out_owned, err_owned;
    console_get_stream(STD_OUTPUT_HANDLE, has_console, &out, &out_type, &out_owned);
    console_get_stream(STD_ERROR_HANDLE, has_console, &err, &err_type, &err_owned);
    if (!handle_ok(out) && !handle_ok(err)) {
        if (attached) {
            FreeConsole();
            s.console_attached = false;
        }
        LeaveCriticalSection(&s.cs);
        return false;
    }
    s.con_out = out;
    s.con_err = err;
    s.con_out_type = out_type;
    s.con_err_type = err_type;
    s.con_out_owned = out_owned;
    s.con_err_owned = err_owned;
    s.console = true;
    LeaveCriticalSection(&s.cs);
    return true;
}

void applog_detach_console(void)
{
    if (!s.initialized) return;
    EnterCriticalSection(&s.cs);
    console_disable_locked();
    if (s.console_attached) {
        FreeConsole();
        s.console_attached = false;
    }
    LeaveCriticalSection(&s.cs);
}

bool applog_start(const wchar_t *dir, DWORD *error)
{
    if (error) *error = 0;
    if (!s.initialized) {
        if (error) *error = ERROR_INVALID_STATE;
        return false;
    }
    if (!dir || !*dir) {
        if (error) *error = ERROR_INVALID_PARAMETER;
        return false;
    }
    DWORD attr = GetFileAttributesW(dir);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        if (error) *error = attr == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_DIRECTORY;
        return false;
    }

    EnterCriticalSection(&s.cs);
    if (s.enabled) {
        LeaveCriticalSection(&s.cs);
        return true;
    }
    struct tm tm;
    now_tm(&tm);
    DWORD err = 0;
    if (!open_for_date(dir, &tm, &err)) {
        if (error) *error = err ? err : ERROR_OPEN_FAILED;
        LeaveCriticalSection(&s.cs);
        return false;
    }
    s.enabled = true;
    s.written = 0;
    wcscpy_s(s.last_dir, ARRAYSIZE(s.last_dir), dir);
    s.starting = true;
    char u8[MAX_PATH * 8];
    log_info("logging enabled: %s", applog_u8(s.path, u8, sizeof(u8)));
    s.starting = false;
    bool ok = s.fp && fflush(s.fp) == 0 && !ferror(s.fp);
    if (!ok) {
        close_file_locked();
        if (error) *error = ERROR_WRITE_FAULT;
    }
    LeaveCriticalSection(&s.cs);
    return ok;
}

void applog_stop(void)
{
    if (!s.initialized) return;
    EnterCriticalSection(&s.cs);
    if (s.enabled) {
        /* Raw write: the normal sink could hit the session cap mid-stop and
         * turn a user-requested stop into an automatic one. */
        write_raw_locked("INFO", "logging stopped");
        close_file_locked();
    }
    LeaveCriticalSection(&s.cs);
}

void applog_set_stop_callback(void (*fn)(void *ctx), void *ctx)
{
    if (!s.initialized) return;
    EnterCriticalSection(&s.cs);
    s.stop_cb = fn;
    s.stop_ctx = ctx;
    LeaveCriticalSection(&s.cs);
}

AppLogState applog_state(void)
{
    if (!s.initialized) return APPLOG_OFF;
    EnterCriticalSection(&s.cs);
    bool on = s.enabled;
    LeaveCriticalSection(&s.cs);
    return on ? APPLOG_ON : APPLOG_OFF;
}

const wchar_t *applog_path(void)
{
    if (!s.initialized) return L"";
    EnterCriticalSection(&s.cs);
    bool on = s.enabled;
    if (on) wcscpy_s(s.path_out, ARRAYSIZE(s.path_out), s.path);
    LeaveCriticalSection(&s.cs);
    return on ? s.path_out : L"";
}

const wchar_t *applog_last_dir(void)
{
    return s.last_dir;
}

#ifdef YAGP_APPLOG_TEST
void applog_test_set_time(time_t t)
{
    EnterCriticalSection(&s.cs);
    s.fake_now = t;
    LeaveCriticalSection(&s.cs);
}
#endif
