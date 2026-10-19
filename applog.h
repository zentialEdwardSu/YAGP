#ifndef YAGP_APPLOG_H
#define YAGP_APPLOG_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include <wchar.h>

/* YAGP's logging policy on top of rxi/log.c: debugger mirror, an optional
 * terminal mirror and a log file the user enables from the tray. The file is
 * <dir>\yagp-YYYYMMDD.log, opened for append and switched when the local date
 * changes. Settings are per run. */

typedef enum AppLogState { APPLOG_OFF = 0, APPLOG_ON = 1 } AppLogState;

/* Installs the output callback, the lock and the level/quiet settings. Call
 * once, early, before other threads may log. */
void applog_init(void);
/* Closes the file and detaches the terminal. The critical section is
 * deliberately not deleted: the frame cache worker can still log until the
 * process exits. */
void applog_shutdown(void);

/* Mirrors subsequent log lines to the terminal that launched this process, or
 * to valid redirected stdout/stderr handles. Never allocates a console. Safe to
 * call repeatedly; returns true when terminal output is active. */
bool applog_attach_console(void);
/* Stops terminal output, closes handles this layer opened and detaches a
 * console it attached. Inherited stdout/stderr handles are left alone. */
void applog_detach_console(void);

/* Opens <dir>\yagp-YYYYMMDD.log for append and turns logging on. On failure
 * returns false, leaves the state OFF and stores the system error code. */
bool applog_start(const wchar_t *dir, DWORD *error);
/* Writes the stop line, flushes and closes. No-op when already off. */
void applog_stop(void);

AppLogState applog_state(void);
/* The current file, or L"" when logging is off. The pointer is valid until the
 * next call to applog_path (the string is snapshotted under the log lock). */
const wchar_t *applog_path(void);
/* Last folder logging was successfully enabled in this run (empty if none).
 * Used as the initial folder of the next picker; never persisted. */
const wchar_t *applog_last_dir(void);

/* Called when logging stops by itself (session limit or a write error). The
 * callback may run on any thread, so main only posts a message from it. */
void applog_set_stop_callback(void (*fn)(void *ctx), void *ctx);

/* UTF-8 view of a wide string in a caller-provided buffer. Never returns NULL;
 * w == NULL or an empty string yields "". */
const char *applog_u8(const wchar_t *w, char *buf, size_t cap);

#ifdef YAGP_APPLOG_TEST
/* 0 restores the real clock. Lets the file tests cross a date boundary. */
void applog_test_set_time(time_t t);
#endif

#endif
