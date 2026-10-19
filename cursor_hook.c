#include "cursor_hook.h"
#include "log.h"

#include <string.h>

#define QUEUE_CAP 64

static SRWLOCK s_lock = SRWLOCK_INIT;
static HANDLE s_thread;
static DWORD s_thread_id;
static HANDLE s_ready;            /* Set once the hook is installed (or failed). */
static volatile LONG s_installed;
static DWORD s_hook_error;        /* Set by the hook thread when installation fails. */
static HWND s_notify;
static volatile LONG s_want_cursor;
static volatile LONG s_cursor_pending, s_input_pending;
static volatile LONG s_pos_x, s_pos_y;

/* Under s_lock. */
static HookWatch s_watch[CURSOR_HOOK_MAX_WATCH];
static int s_watch_n;
static HookEvent s_queue[QUEUE_CAP];
static int s_q_head, s_q_count;
/* Hook-thread only. */
static int s_inside_id = -1;      /* Watched box under the cursor, or -1. */
static int s_captured_id = -1;    /* Box a button went down on, until it goes up. */
static int s_buttons;             /* Held buttons, as bits 1 << button. */

static int watch_at(int x, int y)
{
    for (int i = s_watch_n - 1; i >= 0; --i) { /* Later = created later = usually on top. */
        const RECT *r = &s_watch[i].box;
        if (x >= r->left && x < r->right && y >= r->top && y < r->bottom) return s_watch[i].id;
    }
    return -1;
}

static void push(const HookEvent *e)
{
    if (e->kind == HOOK_MOVE && s_q_count > 0) {
        HookEvent *last = &s_queue[(s_q_head + s_q_count - 1) % QUEUE_CAP];
        if (last->kind == HOOK_MOVE) { /* Coalesce: only the latest position matters. */
            *last = *e;
            return;
        }
    }
    if (s_q_count == QUEUE_CAP) { /* UI thread stalled: drop the oldest. */
        s_q_head = (s_q_head + 1) % QUEUE_CAP;
        --s_q_count;
    }
    s_queue[(s_q_head + s_q_count++) % QUEUE_CAP] = *e;
}

static void notify(volatile LONG *pending, UINT msg)
{
    if (InterlockedExchange(pending, 1) == 0 && !PostMessageW(s_notify, msg, 0, 0)) InterlockedExchange(pending, 0);
}

static int button_of(WPARAM msg, const MSLLHOOKSTRUCT *m, bool *down)
{
    *down = false;
    switch (msg) {
    case WM_LBUTTONDOWN: *down = true; return 1;
    case WM_LBUTTONUP: return 1;
    case WM_RBUTTONDOWN: *down = true; return 2;
    case WM_RBUTTONUP: return 2;
    case WM_MBUTTONDOWN: *down = true; return 3;
    case WM_MBUTTONUP: return 3;
    default: (void)m; return 0;
    }
}

static LRESULT CALLBACK hook_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION) {
        const MSLLHOOKSTRUCT *m = (const MSLLHOOKSTRUCT *)lp;
        int x = m->pt.x, y = m->pt.y;
        bool moved = x != s_pos_x || y != s_pos_y;
        InterlockedExchange(&s_pos_x, x);
        InterlockedExchange(&s_pos_y, y);
        if (wp == WM_MOUSEMOVE && moved && s_want_cursor) notify(&s_cursor_pending, WM_APP_CURSOR);

        bool queued = false;
        AcquireSRWLockExclusive(&s_lock);
        if (s_watch_n > 0) {
            HookEvent e;
            memset(&e, 0, sizeof(e));
            e.x = x;
            e.y = y;
            e.time = m->time;
            bool down;
            int b = button_of(wp, m, &down);
            int at = watch_at(x, y);
            if (b) {
                if (down) {
                    if (!s_buttons && at >= 0) s_captured_id = at;
                    s_buttons |= 1 << b;
                } else {
                    s_buttons &= ~(1 << b);
                }
                if (s_captured_id >= 0) {
                    e.kind = down ? HOOK_DOWN : HOOK_UP;
                    e.button = b;
                    push(&e);
                    queued = true;
                }
                if (!s_buttons) s_captured_id = -1;
            } else if (wp == WM_MOUSEWHEEL && at >= 0) {
                e.kind = HOOK_WHEEL;
                e.wheel = (short)HIWORD(m->mouseData);
                push(&e);
                queued = true;
            } else if (wp == WM_MOUSEMOVE && moved && (at >= 0 || at != s_inside_id || s_captured_id >= 0)) {
                /* Inside a box, crossing an edge, or dragging: the UI thread
                 * needs it. Elsewhere moves cost a rectangle test, nothing more. */
                e.kind = HOOK_MOVE;
                push(&e);
                queued = true;
            }
            s_inside_id = at;
        }
        ReleaseSRWLockExclusive(&s_lock);
        if (queued) notify(&s_input_pending, WM_APP_HOOK_INPUT);
    }
    return CallNextHookEx(NULL, code, wp, lp); /* Observe only: never swallow input. */
}

/* Do not call log_* from this thread: it only has a 64 KB stack and formatting
 * one line uses a few KB. Queue events for the UI thread instead. */
static DWORD WINAPI hook_thread(void *arg)
{
    (void)arg;
    /* Above normal: the system waits for every low-level hook on each mouse event. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    MSG msg;
    PeekMessageW(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE); /* Create the queue for WM_QUIT. */
    HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, hook_proc, GetModuleHandleW(NULL), 0);
    if (!hook) s_hook_error = GetLastError();
    InterlockedExchange(&s_installed, hook != NULL);
    SetEvent(s_ready);
    if (!hook) return 1;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    UnhookWindowsHookEx(hook);
    return 0;
}

static void stop(void)
{
    if (!s_thread) return;
    PostThreadMessageW(s_thread_id, WM_QUIT, 0, 0);
    WaitForSingleObject(s_thread, 2000);
    CloseHandle(s_thread);
    s_thread = NULL;
    s_thread_id = 0;
    InterlockedExchange(&s_installed, 0);
}

static bool start(void)
{
    if (s_thread) return s_installed != 0;
    if (!s_ready) s_ready = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!s_ready) {
        log_warn("cannot create cursor hook event (error %lu); using polling",
                 (unsigned long)GetLastError());
        return false;
    }
    POINT p;
    if (GetCursorPos(&p)) {
        InterlockedExchange(&s_pos_x, p.x);
        InterlockedExchange(&s_pos_y, p.y);
    }
    s_thread = CreateThread(NULL, 64 * 1024, hook_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, &s_thread_id);
    if (!s_thread) {
        log_warn("cannot start cursor hook thread (error %lu); using polling",
                 (unsigned long)GetLastError());
        return false;
    }
    WaitForSingleObject(s_ready, 2000);
    if (!s_installed) {
        WaitForSingleObject(s_thread, 2000);
        CloseHandle(s_thread);
        s_thread = NULL;
        log_warn("cursor hook unavailable (error %lu); using polling", (unsigned long)s_hook_error);
        return false;
    }
    log_debug("cursor hook installed");
    return true;
}

void cursor_hook_set_watch(const HookWatch *watch, int n)
{
    if (n > CURSOR_HOOK_MAX_WATCH) n = CURSOR_HOOK_MAX_WATCH;
    AcquireSRWLockExclusive(&s_lock);
    if (n > 0) memcpy(s_watch, watch, (size_t)n * sizeof(*watch));
    s_watch_n = n < 0 ? 0 : n;
    ReleaseSRWLockExclusive(&s_lock);
}

void cursor_hook_configure(HWND notify_hwnd, bool want_cursor, const HookWatch *watch, int n)
{
    s_notify = notify_hwnd;
    InterlockedExchange(&s_want_cursor, want_cursor);
    cursor_hook_set_watch(watch, n);
    if (want_cursor || n > 0) start();
    else stop();
}

bool cursor_hook_running(void)
{
    return s_thread && s_installed;
}

POINT cursor_hook_pos(void)
{
    InterlockedExchange(&s_cursor_pending, 0); /* Read: the next move may post again. */
    POINT p = {s_pos_x, s_pos_y};
    if (!cursor_hook_running()) GetCursorPos(&p);
    return p;
}

int cursor_hook_take(HookEvent *out, int cap)
{
    InterlockedExchange(&s_input_pending, 0);
    AcquireSRWLockExclusive(&s_lock);
    int n = 0;
    while (n < cap && s_q_count > 0) {
        out[n++] = s_queue[s_q_head];
        s_q_head = (s_q_head + 1) % QUEUE_CAP;
        --s_q_count;
    }
    ReleaseSRWLockExclusive(&s_lock);
    return n;
}

void cursor_hook_shutdown(void)
{
    stop();
    if (s_ready) CloseHandle(s_ready);
    s_ready = NULL;
}
