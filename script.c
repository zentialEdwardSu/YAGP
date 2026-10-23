#include "script.h"
#include "contacts.h"
#include "i18n.h"
#include "log.h"
#include "monitors.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

/* Sandbox limits. Everything runs on the UI thread, so a runaway script must
 * fail fast instead of freezing every overlay. */
#define SCRIPT_MEM_LIMIT (4u * 1024u * 1024u)
#define SCRIPT_HOOK_STEP 1000          /* Instructions between hook calls. */
#define SCRIPT_HOOK_BUDGET 10000       /* Hook calls per entry: ~10M instructions. */
#define SCRIPT_MAX_TIMERS 64
#define SCRIPT_MAX_HANDLERS 64         /* yagp.on subscriptions alive at once. */
/* yagp.log / print throttling, per script. */
#define SCRIPT_LOG_MAX 1024            /* Bytes kept from one log call. */
#define SCRIPT_LOG_PER_SEC 100         /* Lines accepted per 1 s window. */
#define SCRIPT_LOG_WINDOW 1.0
/* yagp.notify throttling. */
#define SCRIPT_NOTIFY_WINDOW 3.0

/* yagp.after / yagp.every. The function lives in the registry (ref). */
typedef struct Timer {
    double due, period;      /* period 0 = one-shot. */
    int id, ref;
} Timer;

struct Script {
    lua_State *L;
    const ScriptHostOps *ops;
    void *ctx;
    size_t mem_used;
    int hooks_left;
    ScriptMouse mouse;   /* Last successfully read cursor state. */
    int id;
    /* Whether the global table holds an update function, refreshed after every
     * call into Lua (the only time it can change). The host asks for it many
     * times per tick, once per overlay and peer. */
    bool has_update;
    char name[128];
    const ScriptHostExtOps *ext;   /* NULL: clip / transform / input functions are no-ops. */
    /* yagp.on handlers: count per event, plus the legacy globals (on_moved ...)
     * found after the last call. subs = the union, as reported to the host. */
    int handler_count[SEV_COUNT];
    int handlers_total;
    unsigned legacy, subs, reported;
    int next_handle;
    int depth;                     /* Handler lists being iterated (no compaction meanwhile). */
    int dead_handlers;             /* Removed entries still in the lists. */
    Timer timers[SCRIPT_MAX_TIMERS];
    int timer_count;
    /* yagp.log / print: fixed window plus a summary of what was dropped. */
    double log_window;
    int log_count;
    int log_dropped;
    /* yagp.notify: one balloon per window. */
    double notify_next;
    double notify_window;
    int notify_dropped;
};

/* Registry keys (addresses are unique and never collected). */
static char HANDLERS_KEY, EVTABLES_KEY;

static const char *const EVENT_NAMES[SEV_COUNT] = {
    "mouse_down", "mouse_up", "click", "double_click", "mouse_enter", "mouse_leave", "hover",
    "mouse_move", "wheel", "drag_start", "drag", "drag_end",
    "cursor", "peer_moved", "peer_added", "peer_removed", "collide", "separate", "moved",
    "screens_changed", "frame", "clip_end", "transform_end",
};

static void *limited_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    Script *s = (Script *)ud;
    if (!ptr) osize = 0; /* osize then encodes the object type, not a size. */
    if (nsize == 0) {
        free(ptr);
        s->mem_used -= osize;
        return NULL;
    }
    if (nsize > osize && s->mem_used + (nsize - osize) > SCRIPT_MEM_LIMIT) return NULL;
    void *p = realloc(ptr, nsize);
    if (p) s->mem_used = s->mem_used - osize + nsize;
    return p;
}

static Script *self(lua_State *L)
{
    return *(Script **)lua_getextraspace(L);
}

/* Once the budget is gone the hook fires on every instruction and errors each
 * time, so a pcall that swallows the error cannot keep the loop alive: the next
 * instruction in the enclosing frame errors again. protected_call restores the
 * normal step. Coroutines created by the script inherit the hook. */
static void budget_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    Script *s = self(L);
    if (--s->hooks_left >= 0) return;
    if (s->hooks_left == -1) lua_sethook(L, budget_hook, LUA_MASKCOUNT, 1);
    luaL_error(L, TXT_U8_SCRIPT_TIMEOUT, SCRIPT_HOOK_STEP * SCRIPT_HOOK_BUDGET);
}

static void to_wide(const char *utf8, wchar_t *out, size_t cap)
{
    if (!out || !cap) return;
    out[0] = 0;
    if (!utf8) return;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, (int)cap) > 0) return;
    /* Longer than out: convert in full, then cut at a character boundary (not
     * byte-wise, which would garble every non-ASCII message, e.g. zh_CN). */
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    wchar_t *full = n > 0 ? (wchar_t *)malloc((size_t)n * sizeof(wchar_t)) : NULL;
    if (full && MultiByteToWideChar(CP_UTF8, 0, utf8, -1, full, n) > 0) {
        size_t keep = cap - 1;
        if (keep > 0 && full[keep - 1] >= 0xD800 && full[keep - 1] <= 0xDBFF) --keep; /* Lone high surrogate. */
        memcpy(out, full, keep * sizeof(wchar_t));
        out[keep] = 0;
    }
    free(full);
}

/* Pops the error value. Never allocates inside Lua: this runs outside any
 * protected call, where a memory error would abort the process (lua_tostring
 * on a number would create a string). */
static void take_error(Script *s, wchar_t *err, size_t cap)
{
    lua_State *L = s->L;
    if (lua_type(L, -1) == LUA_TSTRING) {
        to_wide(lua_tostring(L, -1), err, cap);
    } else if (lua_type(L, -1) == LUA_TNUMBER) {
        char num[64];
        _snprintf_s(num, sizeof(num), _TRUNCATE, "%.14g", (double)lua_tonumber(L, -1));
        to_wide(num, err, cap);
    } else {
        to_wide(TXT_U8_NON_STRING_ERROR, err, cap);
    }
    lua_pop(L, 1);
}

/* Runs the function on top of the stack with nargs arguments under the budget. */
static bool protected_call(Script *s, int nargs, wchar_t *err, size_t cap)
{
    s->hooks_left = SCRIPT_HOOK_BUDGET;
    lua_sethook(s->L, budget_hook, LUA_MASKCOUNT, SCRIPT_HOOK_STEP);
    if (lua_pcall(s->L, nargs, 0, 0) != LUA_OK) {
        take_error(s, err, cap);
        return false;
    }
    return true;
}

/* ---- yagp library -------------------------------------------------------- */

static void host_rect(Script *s, double *x, double *y, int *w, int *h)
{
    *x = *y = 0;
    *w = *h = 1;
    s->ops->get_rect(s->ctx, x, y, w, h);
}

/* Hitbox offset inside the window (dx, dy) and size; the whole window if the
 * host has none. */
static void host_hitbox(Script *s, int *dx, int *dy, int *hw, int *hh)
{
    double x, y;
    int w, h;
    host_rect(s, &x, &y, &w, &h);
    *dx = *dy = 0;
    *hw = w;
    *hh = h;
    if (s->ops->get_hitbox) s->ops->get_hitbox(s->ctx, dx, dy, hw, hh);
    if (*hw < 1) *hw = 1;
    if (*hh < 1) *hh = 1;
}

static int l_pos(lua_State *L)
{
    double x, y;
    int w, h;
    host_rect(self(L), &x, &y, &w, &h);
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    return 2;
}

static int l_size(lua_State *L)
{
    double x, y;
    int w, h;
    host_rect(self(L), &x, &y, &w, &h);
    lua_pushinteger(L, w);
    lua_pushinteger(L, h);
    return 2;
}

/* Rejects NaN/inf and absurd values so SetWindowPos never sees garbage. */
static double check_coord(lua_State *L, int arg)
{
    double v = luaL_checknumber(L, arg);
    luaL_argcheck(L, v == v && v > -1e7 && v < 1e7, arg, TXT_U8_COORD_RANGE);
    return v;
}

/* Same rounding as host_set_pos, so a script's view of rects matches the screen. */
static LONG round_px(double v)
{
    return (LONG)(v < 0 ? v - 0.5 : v + 0.5);
}

static int l_set_pos(lua_State *L)
{
    Script *s = self(L);
    double x = check_coord(L, 1), y = check_coord(L, 2);
    s->ops->set_pos(s->ctx, x, y);
    return 0;
}

static int l_move(lua_State *L)
{
    Script *s = self(L);
    double dx = check_coord(L, 1), dy = check_coord(L, 2), x, y;
    int w, h;
    host_rect(s, &x, &y, &w, &h);
    s->ops->set_pos(s->ctx, x + dx, y + dy);
    return 0;
}

static void push_rect(lua_State *L, const RECT *r)
{
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, r->left);              lua_setfield(L, -2, "x");
    lua_pushinteger(L, r->top);               lua_setfield(L, -2, "y");
    lua_pushinteger(L, r->right - r->left);   lua_setfield(L, -2, "w");
    lua_pushinteger(L, r->bottom - r->top);   lua_setfield(L, -2, "h");
}

/* { x, y, w, h, work = {x, y, w, h}, dpi, primary, index } */
static void push_monitor(lua_State *L, int i)
{
    const Monitor *m = monitors_get(i);
    RECT empty = {0, 0, 0, 0};
    push_rect(L, m ? &m->bounds : &empty);
    push_rect(L, m ? &m->work : &empty);
    lua_setfield(L, -2, "work");
    lua_pushinteger(L, m ? (lua_Integer)m->dpi : 96);
    lua_setfield(L, -2, "dpi");
    lua_pushboolean(L, m && m->primary);
    lua_setfield(L, -2, "primary");
    lua_pushinteger(L, i + 1);
    lua_setfield(L, -2, "index");
}

static int l_monitors(lua_State *L)
{
    int n = monitors_count();
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; ++i) {
        push_monitor(L, i);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int l_monitor(lua_State *L)
{
    Script *s = self(L);
    double x, y;
    int w, h, dx, dy, hw, hh;
    host_rect(s, &x, &y, &w, &h);
    host_hitbox(s, &dx, &dy, &hw, &hh);
    POINT c = {(LONG)(x + dx + hw / 2.0), (LONG)(y + dy + hh / 2.0)};
    int i = monitors_index_at(c);
    push_monitor(L, i);
    lua_pushinteger(L, i + 1);
    return 2;
}

static int l_desktop(lua_State *L)
{
    RECT d = monitors_desktop();
    lua_pushinteger(L, d.left);
    lua_pushinteger(L, d.top);
    lua_pushinteger(L, d.right - d.left);
    lua_pushinteger(L, d.bottom - d.top);
    return 4;
}

/* Window at (x, y) -> its hitbox on the desktop, or the explicit w x h rect at
 * (x, y) when the caller passes a size (args 3 and 4). */
static RECT probe_rect(lua_State *L, Script *s, double x, double y, int size_arg)
{
    LONG l = round_px(x), t = round_px(y);
    RECT r;
    if (!lua_isnoneornil(L, size_arg)) {
        int w = (int)luaL_checkinteger(L, size_arg), h = (int)luaL_checkinteger(L, size_arg + 1);
        SetRect(&r, l, t, l + w, t + h);
        return r;
    }
    int dx, dy, hw, hh;
    host_hitbox(s, &dx, &dy, &hw, &hh);
    SetRect(&r, l + dx, t + dy, l + dx + hw, t + dy + hh);
    return r;
}

static int l_fits(lua_State *L)
{
    double x = check_coord(L, 1), y = check_coord(L, 2);
    RECT r = probe_rect(L, self(L), x, y, 3);
    lua_pushboolean(L, monitors_rect_fits(&r));
    return 1;
}

/* { id, name, x, y, w, h, hx, hy, hw, hh, cx, cy, vx, vy, scripted, paused,
 *   still, controlled, leader, leader_id } */
static void push_peer(lua_State *L, const ScriptPeer *p)
{
    lua_createtable(L, 0, 20);
    lua_pushinteger(L, p->id);                   lua_setfield(L, -2, "id");
    lua_pushstring(L, p->name ? p->name : "");   lua_setfield(L, -2, "name");
    lua_pushnumber(L, p->x);                     lua_setfield(L, -2, "x");
    lua_pushnumber(L, p->y);                     lua_setfield(L, -2, "y");
    lua_pushinteger(L, p->w);                    lua_setfield(L, -2, "w");
    lua_pushinteger(L, p->h);                    lua_setfield(L, -2, "h");
    lua_pushnumber(L, p->hx);                    lua_setfield(L, -2, "hx");
    lua_pushnumber(L, p->hy);                    lua_setfield(L, -2, "hy");
    lua_pushinteger(L, p->hw);                   lua_setfield(L, -2, "hw");
    lua_pushinteger(L, p->hh);                   lua_setfield(L, -2, "hh");
    lua_pushnumber(L, p->hx + p->hw / 2.0);      lua_setfield(L, -2, "cx");
    lua_pushnumber(L, p->hy + p->hh / 2.0);      lua_setfield(L, -2, "cy");
    lua_pushnumber(L, p->vx);                    lua_setfield(L, -2, "vx");
    lua_pushnumber(L, p->vy);                    lua_setfield(L, -2, "vy");
    lua_pushboolean(L, p->scripted);             lua_setfield(L, -2, "scripted");
    lua_pushboolean(L, p->paused);               lua_setfield(L, -2, "paused");
    lua_pushboolean(L, p->still);                 lua_setfield(L, -2, "still");
    lua_pushboolean(L, p->follower);            lua_setfield(L, -2, "follower");
    lua_pushboolean(L, p->leader);                lua_setfield(L, -2, "leader");
    lua_pushinteger(L, p->leader_id);             lua_setfield(L, -2, "leader_id");
}

static int read_peers(Script *s, ScriptPeer *out)
{
    if (!s->ops->get_peers) return 0;
    int n = s->ops->get_peers(s->ctx, out, SCRIPT_MAX_PEERS);
    return n < 0 ? 0 : n > SCRIPT_MAX_PEERS ? SCRIPT_MAX_PEERS : n;
}

static int l_others(lua_State *L)
{
    ScriptPeer peers[SCRIPT_MAX_PEERS];
    int n = read_peers(self(L), peers);
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; ++i) {
        push_peer(L, &peers[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int l_peer(lua_State *L)
{
    lua_Integer id = luaL_checkinteger(L, 1);
    ScriptPeer peers[SCRIPT_MAX_PEERS];
    int n = read_peers(self(L), peers);
    for (int i = 0; i < n; ++i) {
        if (peers[i].id == id) {
            push_peer(L, &peers[i]);
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

/* yagp.overlaps([x, y [, w, h]]): peers whose hitbox strictly overlaps this
 * GIF's hitbox (with the window at x, y; default: where it is now), or the
 * explicit w x h rect at x, y. */
static int l_overlaps(lua_State *L)
{
    Script *s = self(L);
    double x, y;
    int w, h;
    host_rect(s, &x, &y, &w, &h);
    if (!lua_isnoneornil(L, 1)) {
        x = check_coord(L, 1);
        y = check_coord(L, 2);
    }
    RECT r = probe_rect(L, s, x, y, 3);
    ScriptPeer peers[SCRIPT_MAX_PEERS];
    int n = read_peers(s, peers), k = 0;
    lua_createtable(L, 0, 0);
    for (int i = 0; i < n; ++i) {
        const ScriptPeer *p = &peers[i];
        LONG pl = round_px(p->hx), pt = round_px(p->hy);
        if (!contacts_overlap(r.left, r.top, r.right, r.bottom, pl, pt, pl + p->hw, pt + p->hh)) continue;
        push_peer(L, p);
        lua_rawseti(L, -2, ++k);
    }
    return 1;
}

static int l_monitor_at(lua_State *L)
{
    POINT p = {(LONG)check_coord(L, 1), (LONG)check_coord(L, 2)};
    int i = monitors_index_at(p);
    push_monitor(L, i);
    lua_pushinteger(L, i + 1);
    return 2;
}

static int l_center(lua_State *L)
{
    Script *s = self(L);
    double x, y;
    int w, h, dx, dy, hw, hh;
    host_rect(s, &x, &y, &w, &h);
    host_hitbox(s, &dx, &dy, &hw, &hh);
    lua_pushnumber(L, x + dx + hw / 2.0);
    lua_pushnumber(L, y + dy + hh / 2.0);
    return 2;
}

/* dx, dy, w, h: hitbox relative to yagp.pos(), display pixels. */
static int l_hitbox(lua_State *L)
{
    int dx, dy, hw, hh;
    host_hitbox(self(L), &dx, &dy, &hw, &hh);
    lua_pushinteger(L, dx);
    lua_pushinteger(L, dy);
    lua_pushinteger(L, hw);
    lua_pushinteger(L, hh);
    return 4;
}

/* set_hitbox(x, y, w, h) in GIF pixels, or set_hitbox(nil) for automatic. */
static int l_set_hitbox(lua_State *L)
{
    Script *s = self(L);
    if (!s->ops->set_hitbox) return 0;
    if (lua_isnoneornil(L, 1)) {
        s->ops->set_hitbox(s->ctx, NULL);
        return 0;
    }
    lua_Integer v[4];
    for (int i = 0; i < 4; ++i) {
        v[i] = luaL_checkinteger(L, i + 1);
        luaL_argcheck(L, v[i] >= 0 && v[i] <= 65535 && (i < 2 || v[i] > 0), i + 1, TXT_U8_COORD_RANGE);
    }
    int box[4] = {(int)v[0], (int)v[1], (int)v[2], (int)v[3]};
    s->ops->set_hitbox(s->ctx, box);
    return 0;
}

/* Refreshes s->mouse; returns whether this read succeeded. */
static bool read_mouse(Script *s)
{
    ScriptMouse m;
    if (!s->ops->get_mouse || !s->ops->get_mouse(s->ctx, &m)) return false;
    s->mouse = m;
    return true;
}

/* x, y, valid. Cheap: no table, for scripts that poll every tick. */
static int l_cursor(lua_State *L)
{
    Script *s = self(L);
    bool valid = read_mouse(s);
    lua_pushinteger(L, s->mouse.x);
    lua_pushinteger(L, s->mouse.y);
    lua_pushboolean(L, valid);
    return 3;
}

/* { x, y, left, right, middle, over, valid, monitor } */
static int l_mouse(lua_State *L)
{
    Script *s = self(L);
    bool valid = read_mouse(s);
    const ScriptMouse *m = &s->mouse;
    double x, y;
    int w, h, dx, dy, hw, hh;
    host_rect(s, &x, &y, &w, &h);
    host_hitbox(s, &dx, &dy, &hw, &hh);
    LONG l = round_px(x) + dx, t = round_px(y) + dy;
    bool over = m->x >= l && m->x < l + hw && m->y >= t && m->y < t + hh;
    POINT p = {m->x, m->y};
    lua_createtable(L, 0, 8);
    lua_pushinteger(L, m->x);      lua_setfield(L, -2, "x");
    lua_pushinteger(L, m->y);      lua_setfield(L, -2, "y");
    lua_pushboolean(L, m->left);   lua_setfield(L, -2, "left");
    lua_pushboolean(L, m->right);  lua_setfield(L, -2, "right");
    lua_pushboolean(L, m->middle); lua_setfield(L, -2, "middle");
    lua_pushboolean(L, over);      lua_setfield(L, -2, "over");
    lua_pushboolean(L, valid);     lua_setfield(L, -2, "valid");
    lua_pushinteger(L, monitors_index_at(p) + 1);
    lua_setfield(L, -2, "monitor");
    return 1;
}

static int l_time(lua_State *L)
{
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    lua_pushnumber(L, (double)t.QuadPart / (double)f.QuadPart);
    return 1;
}

static int l_set_flip(lua_State *L)
{
    Script *s = self(L);
    s->ops->set_flip(s->ctx, lua_toboolean(L, 1));
    return 0;
}

static int l_set_rate(lua_State *L)
{
    Script *s = self(L);
    lua_Integer hz = luaL_checkinteger(L, 1);
    if (hz < SCRIPT_RATE_MIN) hz = SCRIPT_RATE_MIN;
    if (hz > SCRIPT_RATE_MAX) hz = SCRIPT_RATE_MAX;
    s->ops->set_rate(s->ctx, (int)hz);
    return 0;
}

/* ---- yagp.log, print and rate limiting ------------------------------------
 * Messages are built into fixed buffers: no Lua allocation and no metamethods,
 * so a log call can never raise. Each script may write 100 lines per second;
 * the excess is dropped and summarised by the host timer service. */

static double script_now(Script *s);

static void script_tag(const Script *s, char *out, size_t cap)
{
    _snprintf_s(out, cap, _TRUNCATE, "script %s#%d", s->name, s->id);
}

static void append_text(char *out, size_t cap, size_t *used, const char *text, size_t len, bool *truncated)
{
    size_t room = *used < cap ? cap - 1 - *used : 0;
    if (len > room) {
        memcpy(out + *used, text, room);
        *used += room;
        *truncated = true;
    } else {
        memcpy(out + *used, text, len);
        *used += len;
    }
    out[*used] = 0;
}

/* tostring-like conversion without allocating or invoking __tostring. */
static void add_value(lua_State *L, int idx, char *out, size_t cap, size_t *used, bool *truncated)
{
    const char *text;
    size_t len;
    char num[64];
    switch (lua_type(L, idx)) {
    case LUA_TNIL:
        text = "nil";
        len = 3;
        break;
    case LUA_TBOOLEAN:
        text = lua_toboolean(L, idx) ? "true" : "false";
        len = strlen(text);
        break;
    case LUA_TNUMBER: {
        int n = _snprintf_s(num, sizeof(num), _TRUNCATE, "%.14g", (double)lua_tonumber(L, idx));
        text = num;
        len = n > 0 ? (size_t)n : 0;
        break;
    }
    case LUA_TSTRING:
        text = lua_tolstring(L, idx, &len);
        break;
    default:
        text = luaL_typename(L, idx);
        len = strlen(text);
        break;
    }
    append_text(out, cap, used, text, len, truncated);
}

/* Builds the message of a log.* call from the whole stack, tab separated. */
static void build_log_message(lua_State *L, char *out, size_t cap, bool *truncated)
{
    size_t used = 0;
    *truncated = false;
    out[0] = 0;
    /* Leave room for the marker: the message itself never fills the buffer. */
    size_t limit = cap > 4 ? cap - 3 : cap;
    int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        if (i > 1) append_text(out, limit, &used, "\t", 1, truncated);
        add_value(L, i, out, limit, &used, truncated);
    }
    if (*truncated && used + 3 < cap) {
        out[used++] = '.';
        out[used++] = '.';
        out[used++] = '.';
        out[used] = 0;
    } else if (*truncated) {
        out[cap - 1] = 0;
    }
}

/* One WARN line when a rate window ends with dropped lines. */
static void flush_log_summary(Script *s, double now)
{
    if (s->log_dropped <= 0 || now < s->log_window + SCRIPT_LOG_WINDOW) return;
    int dropped = s->log_dropped;
    s->log_dropped = 0;
    s->log_count = 0;
    s->log_window = now;
    char tag[160];
    script_tag(s, tag, sizeof(tag));
    log_log(LOG_WARN, tag, 0, "dropped %d log line(s)", dropped);
}

/* Accounts for one log call and refreshes the rate window. Kept separate from
 * the message construction so dropped lines cost nothing. */
static bool script_log_begin(Script *s)
{
    double now = script_now(s);
    if (now - s->log_window >= SCRIPT_LOG_WINDOW) {
        flush_log_summary(s, now);
        s->log_count = 0;
        s->log_window = now;
    }
    if (s->log_count >= SCRIPT_LOG_PER_SEC) {
        ++s->log_dropped;
        return false;
    }
    ++s->log_count;
    return true;
}

static void script_log_write(Script *s, int level, const char *msg)
{
    char tag[160];
    script_tag(s, tag, sizeof(tag));
    log_log(level, tag, 0, "%s", msg);
}

static void script_log(Script *s, int level, const char *msg)
{
    if (script_log_begin(s)) script_log_write(s, level, msg);
}

static int log_call(lua_State *L, int level)
{
    Script *s = self(L);
    if (!script_log_begin(s)) return 0;
    char msg[SCRIPT_LOG_MAX];
    bool truncated;
    build_log_message(L, msg, sizeof(msg), &truncated);
    script_log_write(s, level, msg);
    return 0;
}

static int l_log_trace(lua_State *L) { return log_call(L, LOG_TRACE); }
static int l_log_debug(lua_State *L) { return log_call(L, LOG_DEBUG); }
static int l_log_info(lua_State *L) { return log_call(L, LOG_INFO); }
static int l_log_warn(lua_State *L) { return log_call(L, LOG_WARN); }
static int l_log_error(lua_State *L) { return log_call(L, LOG_ERROR); }
static int l_log_fatal(lua_State *L) { return log_call(L, LOG_FATAL); }

/* print -> INFO log; keeps its old tostring/tab formatting (unlike log.*,
 * which never calls __tostring and never raises). Documented in README. */
static int l_print(lua_State *L)
{
    Script *s = self(L);
    int n = lua_gettop(L); /* Before luaL_buffinit pushes its buffer placeholder. */
    if (!script_log_begin(s)) return 0; /* Do not allocate/metamethod for a dropped line. */
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 1; i <= n; ++i) {
        if (i > 1) luaL_addchar(&b, '\t');
        luaL_tolstring(L, i, NULL);
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
    const char *text = lua_tostring(L, -1);
    char msg[SCRIPT_LOG_MAX];
    strncpy_s(msg, sizeof(msg), text ? text : "", _TRUNCATE);
    script_log_write(s, LOG_INFO, msg);
    return 0;
}

/* ---- subscriptions (yagp.on / off) and timers ------------------------------
 * Handlers live in registry tables: HANDLERS_KEY[ev + 1] is the list of entry
 * tables for ev, in subscription order; BYID_KEY[handle] is the entry. An
 * entry is {fn = f, id = h, ev = e, <options>, <filter state>}. The host only
 * calls into Lua for events whose bit is in script_subscriptions(), so an
 * unsubscribed event costs one bit test. */

static char BYID_KEY;

static double script_now(Script *s)
{
    if (s->ext && s->ext->now) return s->ext->now(s->ctx);
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return (double)t.QuadPart / (double)f.QuadPart;
}

/* Pushes registry[key], creating an empty table on first use. */
static void push_reg_table(lua_State *L, const void *key)
{
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, key) == LUA_TTABLE) return;
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, key);
}

/* Pushes the handler list of ev (created on first use). */
static void push_handler_list(lua_State *L, int ev)
{
    push_reg_table(L, &HANDLERS_KEY);
    if (lua_rawgeti(L, -1, ev + 1) != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_rawseti(L, -3, ev + 1);
    }
    lua_remove(L, -2);
}

static const char *const LEGACY_NAMES[] = {"on_moved", "on_screens_changed", "on_collide", "on_separate"};
static const int LEGACY_EVENTS[] = {SEV_MOVED, SEV_SCREENS_CHANGED, SEV_COLLIDE, SEV_SEPARATE};

static const char *legacy_name(int ev)
{
    switch (ev) {
    case SEV_MOVED: return "on_moved";
    case SEV_SCREENS_CHANGED: return "on_screens_changed";
    case SEV_COLLIDE: return "on_collide";
    case SEV_SEPARATE: return "on_separate";
    default: return NULL;
    }
}

/* Recomputes the subscription mask and tells the host when it changed. */
static void refresh_subs(Script *s)
{
    unsigned m = s->legacy;
    for (int e = 0; e < SEV_COUNT; ++e) {
        if (s->handler_count[e] > 0) m |= SEV_BIT(e);
    }
    s->subs = m;
    if (m != s->reported) {
        s->reported = m;
        if (s->ext && s->ext->subs_changed) s->ext->subs_changed(s->ctx, m);
    }
}

/* opts[name] as a number in [lo, hi], or def when absent. */
static double opt_number(lua_State *L, int opts, const char *name, double lo, double hi, double def)
{
    if (!opts) return def;
    lua_getfield(L, opts, name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return def;
    }
    int isnum = 0;
    double v = (double)lua_tonumberx(L, -1, &isnum);
    lua_pop(L, 1);
    if (!isnum || !(v >= lo && v <= hi)) luaL_error(L, TXT_U8_BAD_OPTION, name);
    return v;
}

/* opts[name] as a string (pushed), or nothing pushed and false. */
static bool opt_string(lua_State *L, int opts, const char *name)
{
    if (!opts) return false;
    int t = lua_getfield(L, opts, name);
    if (t == LUA_TNIL) {
        lua_pop(L, 1);
        return false;
    }
    if (t != LUA_TSTRING) luaL_error(L, TXT_U8_BAD_OPTION, name);
    return true;
}

static void set_num(lua_State *L, int t, const char *k, double v)
{
    t = lua_absindex(L, t);
    lua_pushnumber(L, v);
    lua_setfield(L, t, k);
}

static void set_int(lua_State *L, int t, const char *k, lua_Integer v)
{
    t = lua_absindex(L, t);
    lua_pushinteger(L, v);
    lua_setfield(L, t, k);
}

static void set_bool(lua_State *L, int t, const char *k, bool v)
{
    t = lua_absindex(L, t);
    lua_pushboolean(L, v);
    lua_setfield(L, t, k);
}

static double get_num(lua_State *L, int t, const char *k, double def)
{
    t = lua_absindex(L, t);
    lua_getfield(L, t, k);
    int isnum = 0;
    double v = (double)lua_tonumberx(L, -1, &isnum);
    lua_pop(L, 1);
    return isnum ? v : def;
}

static bool get_bool(lua_State *L, int t, const char *k)
{
    t = lua_absindex(L, t);
    lua_getfield(L, t, k);
    bool v = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return v;
}
/* Drops dead entries from every handler list. Only between dispatches (an
 * index shift during one would skip or repeat handlers). */
static void compact_handlers(lua_State *L, Script *s)
{
    if (!s->dead_handlers || s->depth > 0) return;
    push_reg_table(L, &HANDLERS_KEY);
    int lists = lua_gettop(L);
    for (int e = 0; e < SEV_COUNT; ++e) {
        if (lua_rawgeti(L, lists, e + 1) != LUA_TTABLE) {
            lua_pop(L, 1);
            continue;
        }
        int list = lua_gettop(L);
        lua_Integer n = (lua_Integer)lua_rawlen(L, list), w = 1;
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_rawgeti(L, list, i);
            if (get_bool(L, -1, "dead")) {
                lua_pop(L, 1);
                continue;
            }
            lua_rawseti(L, list, w++);
        }
        for (; w <= n; ++w) {
            lua_pushnil(L);
            lua_rawseti(L, list, w);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    s->dead_handlers = 0;
}

static int event_index(const char *name)
{
    for (int e = 0; e < SEV_COUNT; ++e) {
        if (strcmp(name, EVENT_NAMES[e]) == 0) return e;
    }
    return -1;
}

/* yagp.on(event, fn [, opts]) -> handle */
static int l_on(lua_State *L)
{
    Script *s = self(L);
    const char *name = luaL_checkstring(L, 1);
    int ev = event_index(name);
    if (ev < 0) return luaL_error(L, TXT_U8_UNKNOWN_EVENT, name);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    int opts = lua_isnoneornil(L, 3) ? 0 : 3;
    if (opts) luaL_checktype(L, 3, LUA_TTABLE);
    if (s->handlers_total >= SCRIPT_MAX_HANDLERS) return luaL_error(L, TXT_U8_TOO_MANY, SCRIPT_MAX_HANDLERS);
    compact_handlers(L, s);

    lua_createtable(L, 0, 8);
    int entry = lua_gettop(L);
    lua_pushvalue(L, 2);
    lua_setfield(L, entry, "fn");
    int id = ++s->next_handle;
    set_int(L, entry, "id", id);
    set_int(L, entry, "ev", ev);
    /* Options, validated here so a typo fails at the call, not silently later. */
    if (ev == SEV_CURSOR) {
        set_num(L, entry, "min_move", opt_number(L, opts, "min_move", 0, 1e6, 0));
        set_num(L, entry, "radius", opt_number(L, opts, "radius", 0, 1e6, 0));
        set_num(L, entry, "rate", opt_number(L, opts, "rate", 0, 1000, 0));
        set_num(L, entry, "lt", -1e9);
    } else if (ev == SEV_PEER_MOVED || ev == SEV_PEER_ADDED || ev == SEV_PEER_REMOVED) {
        set_num(L, entry, "peer", opt_number(L, opts, "id", 0, 2147483647.0, 0));
        set_num(L, entry, "radius", opt_number(L, opts, "radius", 0, 1e6, 0));
    } else if (ev == SEV_FRAME || ev == SEV_CLIP_END) {
        if (opt_string(L, opts, "clip")) lua_setfield(L, entry, "clip");
        if (ev == SEV_FRAME && opts && lua_getfield(L, opts, "frames") != LUA_TNIL) {
            /* {3, 7} (1-based) -> set {[2] = true, [6] = true} keyed 0-based. */
            if (!lua_istable(L, -1)) return luaL_error(L, TXT_U8_BAD_OPTION, "frames");
            lua_newtable(L);
            lua_Integer n = (lua_Integer)lua_rawlen(L, -2);
            for (lua_Integer i = 1; i <= n; ++i) {
                lua_rawgeti(L, -2, i);
                int isnum = 0;
                lua_Integer f = lua_tointegerx(L, -1, &isnum);
                lua_pop(L, 1);
                if (!isnum || f < 1) return luaL_error(L, TXT_U8_BAD_OPTION, "frames");
                lua_pushboolean(L, 1);
                lua_rawseti(L, -2, f - 1);
            }
            lua_setfield(L, entry, "frames");
            lua_pop(L, 1);
        } else if (ev == SEV_FRAME && opts) {
            lua_pop(L, 1);
        }
    }
    push_handler_list(L, ev);
    lua_pushvalue(L, entry);
    lua_rawseti(L, -2, (lua_Integer)lua_rawlen(L, -2) + 1);
    lua_pop(L, 1);
    push_reg_table(L, &BYID_KEY);
    lua_pushvalue(L, entry);
    lua_rawseti(L, -2, id);
    lua_pop(L, 1);
    s->handler_count[ev]++;
    s->handlers_total++;
    refresh_subs(s);
    lua_pushinteger(L, id);
    return 1;
}

/* yagp.off(handle): unknown or already removed handles are ignored. */
static int l_off(lua_State *L)
{
    Script *s = self(L);
    lua_Integer id = luaL_checkinteger(L, 1);
    push_reg_table(L, &BYID_KEY);
    if (lua_rawgeti(L, -1, id) != LUA_TTABLE) return 0;
    set_bool(L, -1, "dead", true);
    int ev = (int)get_num(L, -1, "ev", 0);
    lua_pop(L, 1);
    lua_pushnil(L);
    lua_rawseti(L, -2, id);
    if (ev >= 0 && ev < SEV_COUNT && s->handler_count[ev] > 0) s->handler_count[ev]--;
    s->handlers_total--;
    s->dead_handlers++;
    compact_handlers(L, s);
    refresh_subs(s);
    return 0;
}

/* ---- timers ---------------------------------------------------------------- */

static int add_timer(lua_State *L, bool repeat)
{
    Script *s = self(L);
    double sec = luaL_checknumber(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    double lo = repeat ? 1.0 / SCRIPT_RATE_MAX : 0.0;
    luaL_argcheck(L, sec == sec && sec < 1e7, 1, TXT_U8_COORD_RANGE);
    if (sec < lo) sec = lo;
    if (s->timer_count >= SCRIPT_MAX_TIMERS) return luaL_error(L, TXT_U8_TOO_MANY, SCRIPT_MAX_TIMERS);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    Timer *t = &s->timers[s->timer_count++];
    t->due = script_now(s) + sec;
    t->period = repeat ? sec : 0;
    t->ref = ref;
    t->id = ++s->next_handle;
    lua_pushinteger(L, t->id);
    return 1;
}

static int l_after(lua_State *L) { return add_timer(L, false); }
static int l_every(lua_State *L) { return add_timer(L, true); }

static void remove_timer(lua_State *L, Script *s, int i)
{
    luaL_unref(L, LUA_REGISTRYINDEX, s->timers[i].ref);
    s->timers[i] = s->timers[--s->timer_count];
}

/* yagp.cancel(handle) */
static int l_cancel(lua_State *L)
{
    Script *s = self(L);
    lua_Integer id = luaL_checkinteger(L, 1);
    for (int i = 0; i < s->timer_count; ++i) {
        if (s->timers[i].id == id) {
            remove_timer(L, s, i);
            break;
        }
    }
    return 0;
}
/* ---- clips, playback, composition, input behaviour --------------------------
 * Thin argument checks; the host does the work through ScriptHostExtOps. */

static const ScriptHostExtOps *ext(lua_State *L)
{
    return self(L)->ext;
}

/* One WARN line when a notify window ends with suppressed calls. */
static void flush_notify_summary(Script *s, double now)
{
    if (s->notify_dropped <= 0 || now < s->notify_window) return;
    int dropped = s->notify_dropped;
    s->notify_dropped = 0;
    char tag[160];
    script_tag(s, tag, sizeof(tag));
    log_log(LOG_WARN, tag, 0, "suppressed %d notification(s)", dropped);
}

/* yagp.notify([title,] text) -> true if shown, false if throttled/unavailable. */
static int l_notify(lua_State *L)
{
    Script *s = self(L);
    int n = lua_gettop(L);
    char title[256] = "YAGP";
    char text[1024] = "";
    bool truncated = false;
    size_t used = 0;

    if (n >= 2) {
        add_value(L, 1, title, sizeof(title), &used, &truncated);
        used = 0;
        truncated = false;
        add_value(L, 2, text, sizeof(text), &used, &truncated);
    } else if (n == 1) {
        add_value(L, 1, text, sizeof(text), &used, &truncated);
    }
    if (truncated && used + 3 < sizeof(text)) {
        memcpy(text + used, "...", 3);
        text[used + 3] = 0;
    }

    double now = script_now(s);
    flush_notify_summary(s, now);
    if (now < s->notify_next) {
        if (s->notify_dropped == 0) s->notify_window = s->notify_next;
        ++s->notify_dropped;
        lua_pushboolean(L, 0);
        return 1;
    }
    s->notify_next = now + SCRIPT_NOTIFY_WINDOW;
    bool shown = s->ext && s->ext->notify ? s->ext->notify(s->ctx, title, text) : false;
    if (shown) {
        char msg[SCRIPT_LOG_MAX + 160];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE, "notify: %s | %s", title, text);
        script_log(s, LOG_INFO, msg);
    }
    lua_pushboolean(L, shown ? 1 : 0);
    return 1;
}

static const char *check_clip_name(lua_State *L, int arg)
{
    size_t n;
    const char *name = luaL_checklstring(L, arg, &n);
    luaL_argcheck(L, n > 0 && n < SCRIPT_CLIP_NAME_MAX && strlen(name) == n, arg, TXT_U8_BAD_CLIP_NAME);
    return name;
}

/* Pushes nil, err; returns 2. */
static int fail_with(lua_State *L, const char *msg)
{
    lua_pushnil(L);
    lua_pushstring(L, msg && *msg ? msg : TXT_U8_UNAVAILABLE);
    return 2;
}

static bool static_media(Script *s)
{
    return s->ext && s->ext->is_static && s->ext->is_static(s->ctx);
}

/* yagp.clip(name, relpath) -> true | nil, err */
static int l_clip(lua_State *L)
{
    Script *s = self(L);
    if (static_media(s)) return fail_with(L, TXT_U8_STATIC_NO_ANIM);
    const char *name = check_clip_name(L, 1);
    const char *rel = luaL_checkstring(L, 2);
    luaL_argcheck(L, strcmp(name, "main") != 0, 1, TXT_U8_BAD_CLIP_NAME);
    char err[256] = "";
    if (!s->ext || !s->ext->load_clip) return fail_with(L, NULL);
    if (!s->ext->load_clip(s->ctx, name, rel, err, sizeof(err))) return fail_with(L, err);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_unload(lua_State *L)
{
    Script *s = self(L);
    if (static_media(s)) return fail_with(L, TXT_U8_STATIC_NO_ANIM);
    const char *name = check_clip_name(L, 1);
    if (strcmp(name, "main") != 0 && s->ext && s->ext->unload_clip) s->ext->unload_clip(s->ctx, name);
    return 0;
}

/* yagp.clip_info(name) -> {w, h, frames, duration, delays, ready} | nil */
static int l_clip_info(lua_State *L)
{
    Script *s = self(L);
    if (static_media(s)) {
        lua_pushnil(L);
        return 1;
    }
    const char *name = check_clip_name(L, 1);
    ScriptClipInfo ci;
    memset(&ci, 0, sizeof(ci));
    if (!s->ext || !s->ext->clip_info || !s->ext->clip_info(s->ctx, name, &ci)) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 6);
    int t = lua_gettop(L);
    set_int(L, t, "w", ci.w);
    set_int(L, t, "h", ci.h);
    set_bool(L, t, "ready", ci.frames > 0);
    if (ci.frames > 0) {
        set_int(L, t, "frames", ci.frames);
        set_num(L, t, "duration", ci.duration);
        if (ci.delays) {
            lua_createtable(L, ci.frames, 0);
            for (int i = 0; i < ci.frames; ++i) {
                lua_pushnumber(L, ci.delays[i] / 1000.0);
                lua_rawseti(L, -2, i + 1);
            }
            lua_setfield(L, t, "delays");
        }
    }
    return 1;
}

static int anchor_from(lua_State *L, int opts)
{
    if (!opt_string(L, opts, "anchor")) return SANCHOR_BOTTOM;
    static const char *const names[] = {"bottom", "center", "top_left", NULL};
    int i = luaL_checkoption(L, -1, NULL, names);
    lua_pop(L, 1);
    return i;
}

/* yagp.play(name [, {from, to, loop, speed, anchor, sync, restart}]) -> true | nil, err */
static int l_play(lua_State *L)
{
    Script *s = self(L);
    if (static_media(s)) return fail_with(L, TXT_U8_STATIC_NO_ANIM);
    const char *name = check_clip_name(L, 1);
    int opts = lua_isnoneornil(L, 2) ? 0 : 2;
    if (opts) luaL_checktype(L, 2, LUA_TTABLE);
    ScriptPlay p;
    p.from = (int)opt_number(L, opts, "from", 1, 65535, 0) - 1;
    p.to = (int)opt_number(L, opts, "to", 1, 65535, 0) - 1;
    if (p.from >= 0 && p.to >= 0) luaL_argcheck(L, p.from <= p.to, 2, TXT_U8_BAD_RANGE);
    p.loops = -1;
    if (opts) {
        int t = lua_getfield(L, opts, "loop");
        if (t == LUA_TBOOLEAN) p.loops = lua_toboolean(L, -1) ? -1 : 1;
        else if (t == LUA_TNUMBER) {
            lua_Integer n = lua_tointeger(L, -1);
            if (n < 1 || n > 1000000) return luaL_error(L, TXT_U8_BAD_OPTION, "loop");
            p.loops = (int)n;
        } else if (t != LUA_TNIL) return luaL_error(L, TXT_U8_BAD_OPTION, "loop");
        lua_pop(L, 1);
    }
    p.speed = opt_number(L, opts, "speed", 0.1, 10, 1);
    p.anchor = anchor_from(L, opts);
    p.sync = opts && get_bool(L, opts, "sync");
    p.restart = true;
    if (opts) {
        lua_getfield(L, opts, "restart");
        if (!lua_isnil(L, -1)) p.restart = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }
    char err[256] = "";
    if (!s->ext || !s->ext->play) return fail_with(L, NULL);
    if (!s->ext->play(s->ctx, name, &p, err, sizeof(err))) return fail_with(L, err);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_seek(lua_State *L)
{
    Script *s = self(L);
    if (static_media(s)) return fail_with(L, TXT_U8_STATIC_NO_ANIM);
    lua_Integer f = luaL_checkinteger(L, 1);
    luaL_argcheck(L, f >= 1 && f <= 65535, 1, TXT_U8_BAD_RANGE);
    lua_pushboolean(L, s->ext && s->ext->seek && s->ext->seek(s->ctx, (int)f - 1));
    return 1;
}

/* yagp.frame() -> clip name, frame (1-based), frames (nil until known) */
static int l_frame(lua_State *L)
{
    Script *s = self(L);
    if (static_media(s)) return fail_with(L, TXT_U8_STATIC_NO_ANIM);
    const char *name = "main";
    int frame = 0, frames = 0;
    if (s->ext && s->ext->get_frame) s->ext->get_frame(s->ctx, &name, &frame, &frames);
    lua_pushstring(L, name ? name : "main");
    lua_pushinteger(L, frame + 1);
    if (frames > 0) lua_pushinteger(L, frames);
    else lua_pushnil(L);
    return 3;
}

static int l_set_speed(lua_State *L)
{
    Script *s = self(L);
    if (static_media(s)) return fail_with(L, TXT_U8_STATIC_NO_ANIM);
    double v = luaL_checknumber(L, 1);
    luaL_argcheck(L, v >= 0.1 && v <= 10, 1, TXT_U8_BAD_RANGE);
    if (s->ext && s->ext->set_speed) s->ext->set_speed(s->ctx, v);
    return 0;
}

static int l_set_playing(lua_State *L)
{
    Script *s = self(L);
    if (static_media(s)) return fail_with(L, TXT_U8_STATIC_NO_ANIM);
    if (s->ext && s->ext->set_playing) s->ext->set_playing(s->ctx, lua_toboolean(L, 1));
    return 0;
}

static int easing_from(lua_State *L, int arg)
{
    static const char *const names[] = {"linear", "ease_in", "ease_out", "ease_in_out", NULL};
    return luaL_checkoption(L, arg, "ease_out", names);
}

static double check_duration(lua_State *L, int arg)
{
    double d = luaL_optnumber(L, arg, 0);
    luaL_argcheck(L, d >= 0 && d <= 60, arg, TXT_U8_BAD_RANGE);
    return d;
}

/* Matrix element in a sane range: scale-ish parts within +-8, translation +-4096. */
static void check_matrix(lua_State *L, const double m[6])
{
    for (int i = 0; i < 6; ++i) {
        double lim = i < 4 ? 8.0 : 4096.0;
        if (!(m[i] >= -lim && m[i] <= lim)) luaL_error(L, TXT_U8_BAD_MATRIX);
    }
    double det = m[0] * m[3] - m[1] * m[2];
    if (!(det > 0.0025 || det < -0.0025)) luaL_error(L, TXT_U8_BAD_MATRIX); /* Degenerate: no inverse. */
}

static void apply_matrix(lua_State *L, const double m[6], double duration, int easing)
{
    Script *s = self(L);
    check_matrix(L, m);
    if (s->ext && s->ext->set_matrix && !s->ext->set_matrix(s->ctx, m, duration, easing))
        luaL_error(L, TXT_U8_BAD_MATRIX);
}

/* yagp.set_matrix(m11, m12, m21, m22, dx, dy [, duration, easing]) */
static int l_set_matrix(lua_State *L)
{
    double m[6];
    for (int i = 0; i < 6; ++i) m[i] = luaL_checknumber(L, i + 1);
    apply_matrix(L, m, check_duration(L, 7), easing_from(L, 8));
    return 0;
}

/* yagp.set_transform{sx, sy, angle, kx, ky, px, py, tx, ty, duration, easing}
 * About the pivot (px, py) in GIF pixels (default: bottom centre): scale, then
 * skew (kx, ky in degrees), then rotate (angle in degrees, clockwise on
 * screen), then move by (tx, ty) display pixels. */
static int l_set_transform(lua_State *L)
{
    const double rad = 3.14159265358979323846 / 180.0;
    luaL_checktype(L, 1, LUA_TTABLE);
    Script *s = self(L);
    double sx = opt_number(L, 1, "sx", -8, 8, 1), sy = opt_number(L, 1, "sy", -8, 8, 1);
    double a = opt_number(L, 1, "angle", -3600, 3600, 0) * rad;
    double tkx = tan(opt_number(L, 1, "kx", -80, 80, 0) * rad), tky = tan(opt_number(L, 1, "ky", -80, 80, 0) * rad);
    double tx = opt_number(L, 1, "tx", -4096, 4096, 0), ty = opt_number(L, 1, "ty", -4096, 4096, 0);
    double x, y;
    int w, h;
    host_rect(s, &x, &y, &w, &h);
    /* GIF pixels -> window pixels: the window shows the current clip at its display size. */
    double gw = w, gh = h;
    const char *cur = NULL;
    int frame = 0, frames = 0;
    ScriptClipInfo ci;
    if (s->ext && s->ext->get_frame && s->ext->clip_info) {
        s->ext->get_frame(s->ctx, &cur, &frame, &frames);
        if (cur && s->ext->clip_info(s->ctx, cur, &ci) && ci.w > 0 && ci.h > 0) {
            gw = ci.w;
            gh = ci.h;
        }
    }
    double cx = opt_number(L, 1, "px", -65535, 65535, gw / 2) * w / gw;
    double cy = opt_number(L, 1, "py", -65535, 65535, gh) * h / gh;
    /* Row vectors (p' = p * M): linear part S * K * R, then T(-c) ... T(c + t). */
    double ca = cos(a), sa = sin(a);
    double k11 = sx, k12 = sx * tky, k21 = sy * tkx, k22 = sy;
    double m[6];
    m[0] = k11 * ca - k12 * sa;
    m[1] = k11 * sa + k12 * ca;
    m[2] = k21 * ca - k22 * sa;
    m[3] = k21 * sa + k22 * ca;
    m[4] = cx + tx - cx * m[0] - cy * m[2];
    m[5] = cy + ty - cx * m[1] - cy * m[3];
    double duration = opt_number(L, 1, "duration", 0, 60, 0);
    int easing = SEASE_OUT;
    if (opt_string(L, 1, "easing")) easing = easing_from(L, lua_gettop(L));
    apply_matrix(L, m, duration, easing);
    return 0;
}

static int l_reset_transform(lua_State *L)
{
    Script *s = self(L);
    static const double identity[6] = {1, 0, 0, 1, 0, 0};
    double d = check_duration(L, 1);
    if (s->ext && s->ext->set_matrix) s->ext->set_matrix(s->ctx, d > 0 ? identity : NULL, d, SEASE_OUT);
    return 0;
}

static int l_set_opacity(lua_State *L)
{
    Script *s = self(L);
    double a = luaL_checknumber(L, 1);
    luaL_argcheck(L, a >= 0 && a <= 1, 1, TXT_U8_BAD_RANGE);
    double d = check_duration(L, 2);
    int easing = easing_from(L, 3);
    if (s->ext && s->ext->set_opacity) s->ext->set_opacity(s->ctx, a, d, easing);
    return 0;
}

static int l_set_cursor(lua_State *L)
{
    Script *s = self(L);
    static const char *const names[] = {"move", "arrow", "hand", "none", NULL};
    int shape = luaL_checkoption(L, 1, NULL, names);
    if (s->ext && s->ext->set_cursor) s->ext->set_cursor(s->ctx, shape);
    return 0;
}

static int l_set_draggable(lua_State *L)
{
    Script *s = self(L);
    if (s->ext && s->ext->set_draggable) s->ext->set_draggable(s->ctx, lua_toboolean(L, 1));
    return 0;
}

static int l_set_clickthrough(lua_State *L)
{
    Script *s = self(L);
    if (s->ext && s->ext->set_clickthrough) s->ext->set_clickthrough(s->ctx, lua_toboolean(L, 1));
    return 0;
}

/* ---- chainable follower handles ---------------------------------------------- */

typedef struct FollowerHandle {
    int id;
} FollowerHandle;

#define FOLLOWER_MT "YAGP_FOLLOWER"
#define FOLLOWER_MAX 64

static const ScriptFollowerOps *follower_api(Script *s)
{
    return s && s->ext ? s->ext->followers : NULL;
}

static void *follower_target(lua_State *L, Script **out)
{
    FollowerHandle *h = (FollowerHandle *)luaL_checkudata(L, 1, FOLLOWER_MT);
    Script *s = self(L);
    *out = s;
    const ScriptFollowerOps *ops = follower_api(s);
    return ops && ops->resolve ? ops->resolve(s->ctx, h->id) : NULL;
}

static int follower_self(lua_State *L)
{
    lua_pushvalue(L, 1);
    return 1;
}

static int follower_nil(lua_State *L)
{
    lua_pushnil(L);
    return 1;
}

static bool follower_static_target(lua_State *L, Script *s, void *target)
{
    (void)L;
    const ScriptFollowerOps *ops = follower_api(s);
    return ops && ops->is_static && ops->is_static(target);
}

static void push_follower(lua_State *L, int id)
{
    FollowerHandle *h = (FollowerHandle *)lua_newuserdatauv(L, sizeof(*h), 0);
    h->id = id;
    luaL_setmetatable(L, FOLLOWER_MT);
}

static int l_follower(lua_State *L)
{
    Script *s = self(L);
    int id = (int)luaL_checkinteger(L, 1);
    const ScriptFollowerOps *ops = follower_api(s);
    if (!ops || !ops->resolve || !ops->resolve(s->ctx, id)) return follower_nil(L);
    push_follower(L, id);
    return 1;
}

static int l_followers(lua_State *L)
{
    Script *s = self(L);
    const ScriptFollowerOps *ops = follower_api(s);
    int ids[FOLLOWER_MAX];
    int n = ops && ops->list ? ops->list(s->ctx, ids, FOLLOWER_MAX) : 0;
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; ++i) {
        push_follower(L, ids[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int follower_pos(lua_State *L)
{
    Script *s;
    void *t = follower_target(L, &s);
    if (!t) return follower_nil(L);
    double x, y;
    int w, h;
    s->ops->get_rect(t, &x, &y, &w, &h);
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    return 2;
}

static int follower_size(lua_State *L)
{
    Script *s;
    void *t = follower_target(L, &s);
    if (!t) return follower_nil(L);
    double x, y;
    int w, h;
    s->ops->get_rect(t, &x, &y, &w, &h);
    lua_pushinteger(L, w);
    lua_pushinteger(L, h);
    return 2;
}

static int follower_center(lua_State *L)
{
    Script *s;
    void *t = follower_target(L, &s);
    if (!t) return follower_nil(L);
    double x, y;
    int w, h, dx, dy, hw, hh;
    s->ops->get_rect(t, &x, &y, &w, &h);
    s->ops->get_hitbox(t, &dx, &dy, &hw, &hh);
    lua_pushnumber(L, x + dx + hw / 2.0);
    lua_pushnumber(L, y + dy + hh / 2.0);
    return 2;
}

static int follower_hitbox(lua_State *L)
{
    Script *s;
    void *t = follower_target(L, &s);
    if (!t) return follower_nil(L);
    int dx, dy, w, h;
    s->ops->get_hitbox(t, &dx, &dy, &w, &h);
    lua_pushinteger(L, dx);
    lua_pushinteger(L, dy);
    lua_pushinteger(L, w);
    lua_pushinteger(L, h);
    return 4;
}

static int follower_set_pos(lua_State *L)
{
    Script *s;
    void *t = follower_target(L, &s);
    double x = check_coord(L, 2), y = check_coord(L, 3);
    if (t) s->ops->set_pos(t, x, y);
    return follower_self(L);
}

static int follower_move(lua_State *L)
{
    Script *s;
    void *t = follower_target(L, &s);
    double dx = check_coord(L, 2), dy = check_coord(L, 3);
    if (t) {
        double x, y;
        int w, h;
        s->ops->get_rect(t, &x, &y, &w, &h);
        s->ops->set_pos(t, x + dx, y + dy);
    }
    return follower_self(L);
}

static int follower_set_flip(lua_State *L)
{
    Script *s;
    void *t = follower_target(L, &s);
    if (t) s->ops->set_flip(t, lua_toboolean(L, 2));
    return follower_self(L);
}

static int follower_set_hitbox(lua_State *L)
{
    Script *s;
    void *t = follower_target(L, &s);
    if (t && s->ops->set_hitbox) {
        if (lua_isnoneornil(L, 2)) {
            s->ops->set_hitbox(t, NULL);
        } else {
            lua_Integer v[4];
            for (int i = 0; i < 4; ++i) v[i] = luaL_checkinteger(L, i + 2);
            int box[4] = {(int)v[0], (int)v[1], (int)v[2], (int)v[3]};
            s->ops->set_hitbox(t, box);
        }
    }
    return follower_self(L);
}

static int follower_name(lua_State *L)
{
    FollowerHandle *h = (FollowerHandle *)luaL_checkudata(L, 1, FOLLOWER_MT);
    Script *s = self(L);
    const ScriptFollowerOps *ops = follower_api(s);
    const char *name = ops && ops->name ? ops->name(s->ctx, h->id) : NULL;
    if (name) lua_pushstring(L, name);
    else lua_pushnil(L);
    return 1;
}

static int follower_id(lua_State *L)
{
    FollowerHandle *h = (FollowerHandle *)luaL_checkudata(L, 1, FOLLOWER_MT);
    lua_pushinteger(L, h->id);
    return 1;
}

static int follower_paused(lua_State *L)
{
    Script *s;
    void *t = follower_target(L, &s);
    const ScriptFollowerOps *ops = follower_api(s);
    if (t && ops && ops->get_paused) lua_pushboolean(L, ops->get_paused(t));
    else lua_pushnil(L);
    return 1;
}

static int follower_hidden(lua_State *L)
{
    Script *s;
    void *t = follower_target(L, &s);
    const ScriptFollowerOps *ops = follower_api(s);
    if (t && ops && ops->get_hidden) lua_pushboolean(L, ops->get_hidden(t));
    else lua_pushnil(L);
    return 1;
}

static int follower_hide(lua_State *L)
{
    Script *s;
    void *t = follower_target(L, &s);
    if (t && s->ext && s->ext->set_hidden) s->ext->set_hidden(t, true);
    return follower_self(L);
}

static int follower_show(lua_State *L)
{
    Script *s;
    void *t = follower_target(L, &s);
    if (t && s->ext && s->ext->set_hidden) s->ext->set_hidden(t, false);
    return follower_self(L);
}

static int follower_set_paused(lua_State *L)
{
    Script *s;
    void *t = follower_target(L, &s);
    if (!t) return follower_self(L);
    if (follower_static_target(L, s, t)) return fail_with(L, TXT_U8_STATIC_NO_ANIM);
    if (s->ext && s->ext->set_paused) s->ext->set_paused(t, lua_toboolean(L, 2));
    return follower_self(L);
}

static int follower_clip(lua_State *L)
{
    Script *s;
    void *t = follower_target(L, &s);
    if (!t) return follower_self(L);
    if (follower_static_target(L, s, t)) return fail_with(L, TXT_U8_STATIC_NO_ANIM);
    const char *name = check_clip_name(L, 2);
    const char *rel = luaL_checkstring(L, 3);
    if (!s->ext || !s->ext->load_clip) return fail_with(L, TXT_U8_UNAVAILABLE);
    char err[256] = "";
    if (!s->ext->load_clip(t, name, rel, err, sizeof(err))) return fail_with(L, err);
    return follower_self(L);
}

static int follower_unload(lua_State *L)
{
    Script *s;
    void *t = follower_target(L, &s);
    if (!t) return follower_self(L);
    if (follower_static_target(L, s, t)) return fail_with(L, TXT_U8_STATIC_NO_ANIM);
    const char *name = check_clip_name(L, 2);
    if (s->ext && s->ext->unload_clip) s->ext->unload_clip(t, name);
    return follower_self(L);
}

static int follower_play(lua_State *L)
{
    Script *s;
    void *t = follower_target(L, &s);
    if (!t) return follower_self(L);
    if (follower_static_target(L, s, t)) return fail_with(L, TXT_U8_STATIC_NO_ANIM);
    const char *name = check_clip_name(L, 2);
    int opts = lua_isnoneornil(L, 3) ? 0 : 3;
    if (opts) luaL_checktype(L, opts, LUA_TTABLE);
    ScriptPlay p;
    p.from = (int)opt_number(L, opts, "from", 1, 65535, 0) - 1;
    p.to = (int)opt_number(L, opts, "to", 1, 65535, 0) - 1;
    if (p.from >= 0 && p.to >= 0) luaL_argcheck(L, p.from <= p.to, opts, TXT_U8_BAD_RANGE);
    p.loops = -1;
    if (opts) {
        int k = lua_getfield(L, opts, "loop");
        if (k == LUA_TBOOLEAN) p.loops = lua_toboolean(L, -1) ? -1 : 1;
        else if (k == LUA_TNUMBER) {
            lua_Integer n = lua_tointeger(L, -1);
            if (n < 1 || n > 1000000) return luaL_error(L, TXT_U8_BAD_OPTION, "loop");
            p.loops = (int)n;
        } else if (k != LUA_TNIL) return luaL_error(L, TXT_U8_BAD_OPTION, "loop");
        lua_pop(L, 1);
    }
    p.speed = opt_number(L, opts, "speed", 0.1, 10, 1);
    p.anchor = anchor_from(L, opts);
    p.sync = opts && get_bool(L, opts, "sync");
    p.restart = true;
    if (opts) {
        lua_getfield(L, opts, "restart");
        if (!lua_isnil(L, -1)) p.restart = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }
    char err[256] = "";
    if (!s->ext || !s->ext->play) return fail_with(L, TXT_U8_UNAVAILABLE);
    if (!s->ext->play(t, name, &p, err, sizeof(err))) return fail_with(L, err);
    return follower_self(L);
}

static int follower_seek(lua_State *L)
{
    Script *s;
    void *t = follower_target(L, &s);
    if (!t) return follower_self(L);
    if (follower_static_target(L, s, t)) return fail_with(L, TXT_U8_STATIC_NO_ANIM);
    lua_Integer f = luaL_checkinteger(L, 2);
    luaL_argcheck(L, f >= 1 && f <= 65535, 2, TXT_U8_BAD_RANGE);
    if (!s->ext || !s->ext->seek || !s->ext->seek(t, (int)f - 1))
        return fail_with(L, TXT_U8_BAD_RANGE);
    return follower_self(L);
}

static int follower_frame(lua_State *L)
{
    Script *s;
    void *t = follower_target(L, &s);
    if (!t) return follower_nil(L);
    if (follower_static_target(L, s, t)) return fail_with(L, TXT_U8_STATIC_NO_ANIM);
    const char *name = "main";
    int frame = 0, frames = 0;
    if (s->ext && s->ext->get_frame) s->ext->get_frame(t, &name, &frame, &frames);
    lua_pushstring(L, name ? name : "main");
    lua_pushinteger(L, frame + 1);
    if (frames > 0) lua_pushinteger(L, frames);
    else lua_pushnil(L);
    return 3;
}

static int follower_clip_info(lua_State *L)
{
    Script *s;
    void *t = follower_target(L, &s);
    if (!t) return follower_nil(L);
    if (follower_static_target(L, s, t)) return follower_nil(L);
    const char *name = check_clip_name(L, 2);
    ScriptClipInfo ci;
    memset(&ci, 0, sizeof(ci));
    if (!s->ext || !s->ext->clip_info || !s->ext->clip_info(t, name, &ci)) return follower_nil(L);
    lua_createtable(L, 0, 6);
    int tab = lua_gettop(L);
    set_int(L, tab, "w", ci.w);
    set_int(L, tab, "h", ci.h);
    set_bool(L, tab, "ready", ci.frames > 0);
    if (ci.frames > 0) {
        set_int(L, tab, "frames", ci.frames);
        set_num(L, tab, "duration", ci.duration);
        if (ci.delays) {
            lua_createtable(L, ci.frames, 0);
            for (int i = 0; i < ci.frames; ++i) {
                lua_pushnumber(L, ci.delays[i] / 1000.0);
                lua_rawseti(L, -2, i + 1);
            }
            lua_setfield(L, tab, "delays");
        }
    }
    return 1;
}

static int follower_set_speed(lua_State *L)
{
    Script *s;
    void *t = follower_target(L, &s);
    if (!t) return follower_self(L);
    if (follower_static_target(L, s, t)) return fail_with(L, TXT_U8_STATIC_NO_ANIM);
    double v = luaL_checknumber(L, 2);
    luaL_argcheck(L, v >= 0.1 && v <= 10, 2, TXT_U8_BAD_RANGE);
    if (s->ext && s->ext->set_speed) s->ext->set_speed(t, v);
    return follower_self(L);
}

static int follower_set_playing(lua_State *L)
{
    Script *s;
    void *t = follower_target(L, &s);
    if (!t) return follower_self(L);
    if (follower_static_target(L, s, t)) return fail_with(L, TXT_U8_STATIC_NO_ANIM);
    if (s->ext && s->ext->set_playing) s->ext->set_playing(t, lua_toboolean(L, 2));
    return follower_self(L);
}

static int follower_set_matrix(lua_State *L)
{
    Script *s;
    void *t = follower_target(L, &s);
    if (!t) return follower_self(L);
    double m[6];
    for (int i = 0; i < 6; ++i) m[i] = luaL_checknumber(L, i + 2);
    check_matrix(L, m);
    double duration = check_duration(L, 8);
    int easing = easing_from(L, 9);
    if (!s->ext || !s->ext->set_matrix || !s->ext->set_matrix(t, m, duration, easing))
        luaL_error(L, TXT_U8_BAD_MATRIX);
    return follower_self(L);
}

static int follower_set_transform(lua_State *L)
{
    const double rad = 3.14159265358979323846 / 180.0;
    Script *s;
    void *t = follower_target(L, &s);
    luaL_checktype(L, 2, LUA_TTABLE);
    if (!t) return follower_self(L);
    double sx = opt_number(L, 2, "sx", -8, 8, 1), sy = opt_number(L, 2, "sy", -8, 8, 1);
    double a = opt_number(L, 2, "angle", -3600, 3600, 0) * rad;
    double tkx = tan(opt_number(L, 2, "kx", -80, 80, 0) * rad);
    double tky = tan(opt_number(L, 2, "ky", -80, 80, 0) * rad);
    double tx = opt_number(L, 2, "tx", -4096, 4096, 0), ty = opt_number(L, 2, "ty", -4096, 4096, 0);
    double x, y;
    int w, h;
    s->ops->get_rect(t, &x, &y, &w, &h);
    double gw = w, gh = h;
    const char *cur = NULL;
    int frame = 0, frames = 0;
    ScriptClipInfo ci;
    if (s->ext && s->ext->get_frame && s->ext->clip_info) {
        s->ext->get_frame(t, &cur, &frame, &frames);
        if (cur && s->ext->clip_info(t, cur, &ci) && ci.w > 0 && ci.h > 0) {
            gw = ci.w;
            gh = ci.h;
        }
    }
    double cx = opt_number(L, 2, "px", -65535, 65535, gw / 2) * w / gw;
    double cy = opt_number(L, 2, "py", -65535, 65535, gh) * h / gh;
    double ca = cos(a), sa = sin(a);
    double k11 = sx, k12 = sx * tky, k21 = sy * tkx, k22 = sy;
    double m[6];
    m[0] = k11 * ca - k12 * sa;
    m[1] = k11 * sa + k12 * ca;
    m[2] = k21 * ca - k22 * sa;
    m[3] = k21 * sa + k22 * ca;
    m[4] = cx + tx - cx * m[0] - cy * m[2];
    m[5] = cy + ty - cx * m[1] - cy * m[3];
    double duration = opt_number(L, 2, "duration", 0, 60, 0);
    int easing = SEASE_OUT;
    if (opt_string(L, 2, "easing")) easing = easing_from(L, lua_gettop(L));
    check_matrix(L, m);
    if (!s->ext || !s->ext->set_matrix || !s->ext->set_matrix(t, m, duration, easing))
        luaL_error(L, TXT_U8_BAD_MATRIX);
    return follower_self(L);
}

static int follower_reset_transform(lua_State *L)
{
    Script *s;
    void *t = follower_target(L, &s);
    if (!t) return follower_self(L);
    static const double identity[6] = {1, 0, 0, 1, 0, 0};
    double d = check_duration(L, 2);
    if (s->ext && s->ext->set_matrix) s->ext->set_matrix(t, d > 0 ? identity : NULL, d, SEASE_OUT);
    return follower_self(L);
}

static int follower_set_opacity(lua_State *L)
{
    Script *s;
    void *t = follower_target(L, &s);
    if (!t) return follower_self(L);
    double a = luaL_checknumber(L, 2);
    luaL_argcheck(L, a >= 0 && a <= 1, 2, TXT_U8_BAD_RANGE);
    double d = check_duration(L, 3);
    int easing = easing_from(L, 4);
    if (s->ext && s->ext->set_opacity) s->ext->set_opacity(t, a, d, easing);
    return follower_self(L);
}

static int follower_set_cursor(lua_State *L)
{
    Script *s;
    void *t = follower_target(L, &s);
    if (!t) return follower_self(L);
    static const char *const names[] = {"move", "arrow", "hand", "none", NULL};
    int shape = luaL_checkoption(L, 2, NULL, names);
    if (s->ext && s->ext->set_cursor) s->ext->set_cursor(t, shape);
    return follower_self(L);
}

static int follower_set_draggable(lua_State *L)
{
    Script *s;
    void *t = follower_target(L, &s);
    if (t && s->ext && s->ext->set_draggable) s->ext->set_draggable(t, lua_toboolean(L, 2));
    return follower_self(L);
}

static int follower_set_clickthrough(lua_State *L)
{
    Script *s;
    void *t = follower_target(L, &s);
    if (t && s->ext && s->ext->set_clickthrough) s->ext->set_clickthrough(t, lua_toboolean(L, 2));
    return follower_self(L);
}

static int follower_tostring(lua_State *L)
{
    FollowerHandle *h = (FollowerHandle *)luaL_checkudata(L, 1, FOLLOWER_MT);
    lua_pushfstring(L, "follower#%d", h->id);
    return 1;
}

static const luaL_Reg follower_methods[] = {
    {"pos", follower_pos}, {"size", follower_size}, {"center", follower_center}, {"hitbox", follower_hitbox},
    {"name", follower_name}, {"id", follower_id}, {"paused", follower_paused}, {"hidden", follower_hidden},
    {"set_pos", follower_set_pos}, {"move", follower_move}, {"set_flip", follower_set_flip},
    {"set_hitbox", follower_set_hitbox}, {"clip", follower_clip}, {"unload", follower_unload},
    {"play", follower_play}, {"seek", follower_seek}, {"frame", follower_frame},
    {"clip_info", follower_clip_info}, {"set_speed", follower_set_speed},
    {"set_playing", follower_set_playing}, {"set_paused", follower_set_paused},
    {"set_matrix", follower_set_matrix}, {"set_transform", follower_set_transform},
    {"reset_transform", follower_reset_transform}, {"set_opacity", follower_set_opacity},
    {"set_cursor", follower_set_cursor}, {"set_draggable", follower_set_draggable},
    {"set_clickthrough", follower_set_clickthrough}, {"hide", follower_hide}, {"show", follower_show},
    {NULL, NULL}
};

static int follower_index(lua_State *L)
{
    luaL_checkudata(L, 1, FOLLOWER_MT);
    const char *name = luaL_checkstring(L, 2);
    for (const luaL_Reg *r = follower_methods; r->name; ++r) {
        if (strcmp(r->name, name) == 0) {
            lua_pushcfunction(L, r->func);
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}


static const luaL_Reg yagp_lib[] = {
    {"pos", l_pos},         {"set_pos", l_set_pos}, {"move", l_move},
    {"size", l_size},       {"monitors", l_monitors}, {"monitor", l_monitor},
    {"monitor_at", l_monitor_at}, {"center", l_center},
    {"desktop", l_desktop}, {"fits", l_fits},       {"cursor", l_cursor},
    {"mouse", l_mouse},
    {"others", l_others},   {"peer", l_peer},       {"overlaps", l_overlaps},
    {"hitbox", l_hitbox},   {"set_hitbox", l_set_hitbox},
    {"time", l_time},       {"set_flip", l_set_flip}, {"set_rate", l_set_rate},
    {"on", l_on},           {"off", l_off},
    {"after", l_after},     {"every", l_every},     {"cancel", l_cancel},
    {"clip", l_clip},       {"unload", l_unload},   {"clip_info", l_clip_info},
    {"play", l_play},       {"seek", l_seek},       {"frame", l_frame},
    {"set_speed", l_set_speed}, {"set_playing", l_set_playing},
    {"set_matrix", l_set_matrix}, {"set_transform", l_set_transform},
    {"reset_transform", l_reset_transform}, {"set_opacity", l_set_opacity},
    {"set_cursor", l_set_cursor}, {"set_draggable", l_set_draggable},
    {"set_clickthrough", l_set_clickthrough}, {"notify", l_notify},
    {"follower", l_follower}, {"followers", l_followers},
    {NULL, NULL}
};

/* The global `log` table (documented as log.info/log.warn/...). */
static const luaL_Reg log_lib[] = {
    {"trace", l_log_trace}, {"debug", l_log_debug}, {"info", l_log_info},
    {"warn", l_log_warn},   {"error", l_log_error}, {"fatal", l_log_fatal},
    {NULL, NULL}
};

/* ---- calls into the script ------------------------------------------------ */

enum { CALL_PLAIN, CALL_DT, CALL_PEER, CALL_CHUNK, CALL_EVENT, CALL_TIMERS };

typedef struct Call {
    Script *s;
    const char *fn;          /* Global to call (CALL_PLAIN / DT / PEER). */
    int kind;
    double dt;               /* CALL_DT */
    const ScriptPeer *peer;  /* CALL_PEER; CALL_EVENT for peer events */
    /* CALL_EVENT */
    int ev;
    const ScriptInput *in;   /* Input events. */
    bool handled;            /* Out: a handler stopped the routed event. */
    int cx, cy;              /* cursor */
    bool cvalid;
    const char *clip;        /* frame / clip_end */
    int frame;
    double now;              /* CALL_TIMERS */
} Call;

/* Pushes the global `name` without metamethods. A strict-mode script
 * (setmetatable(_G, {__index = error})) must not have its missing on_moved
 * turned into an error, and the host never runs script code just to look a
 * handler up. May allocate the key string, so only call it under lua_pcall. */
static int raw_global(lua_State *L, const char *name)
{
    lua_pushglobaltable(L);
    lua_pushstring(L, name);
    int type = lua_rawget(L, -2);
    lua_remove(L, -2);
    return type;
}

/* ---- event dispatch -------------------------------------------------------- */

static const char *button_name(int b)
{
    switch (b) {
    case SBTN_LEFT: return "left";
    case SBTN_RIGHT: return "right";
    case SBTN_MIDDLE: return "middle";
    default: return NULL;
    }
}

/* The reused event table of ev (input and cursor events only: they can fire
 * at mouse rate, so they must not allocate). Pushed. */
static void push_event_table(lua_State *L, int ev)
{
    push_reg_table(L, &EVTABLES_KEY);
    if (lua_rawgeti(L, -1, ev + 1) != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_createtable(L, 0, 16);
        lua_pushstring(L, EVENT_NAMES[ev]);
        lua_setfield(L, -2, "type");
        lua_pushvalue(L, -1);
        lua_rawseti(L, -3, ev + 1);
    }
    lua_remove(L, -2);
}

static void fill_input(lua_State *L, int t, const ScriptInput *in)
{
    const char *b = button_name(in->button);
    if (b) lua_pushstring(L, b);
    else lua_pushnil(L);
    lua_setfield(L, t, "button");
    set_int(L, t, "x", in->x);
    set_int(L, t, "y", in->y);
    set_num(L, t, "gx", in->gx);
    set_num(L, t, "gy", in->gy);
    set_int(L, t, "dx", in->dx);
    set_int(L, t, "dy", in->dy);
    set_num(L, t, "delta", in->wheel);
    set_bool(L, t, "opaque", in->opaque);
    set_bool(L, t, "shift", in->shift);
    set_bool(L, t, "ctrl", in->ctrl);
    set_bool(L, t, "alt", in->alt);
    set_bool(L, t, "passthrough", in->passthrough);
    set_bool(L, t, "handled", false);
}

/* Centre of this overlay's hitbox on the desktop. */
static void self_center(Script *s, double *cx, double *cy)
{
    double x, y;
    int w, h, dx, dy, hw, hh;
    host_rect(s, &x, &y, &w, &h);
    host_hitbox(s, &dx, &dy, &hw, &hh);
    *cx = x + dx + hw / 2.0;
    *cy = y + dy + hh / 2.0;
}

/* Per-handler filter. Returns false to skip entry (on top of the stack). May
 * update the entry's filter state and the event table at arg. */
static bool accept(lua_State *L, Script *s, const Call *c, int arg)
{
    int e = lua_gettop(L);
    switch (c->ev) {
    case SEV_CURSOR: {
        double rate = get_num(L, e, "rate", 0), radius = get_num(L, e, "radius", 0);
        if (rate > 0) {
            double now = script_now(s);
            if (now - get_num(L, e, "lt", -1e9) < 1.0 / rate) return false;
            set_num(L, e, "lt", now);
        }
        if (radius > 0) {
            double cx, cy;
            self_center(s, &cx, &cy);
            double dx = c->cx - cx, dy = c->cy - cy;
            bool near_now = dx * dx + dy * dy <= radius * radius;
            lua_getfield(L, e, "near");
            bool known = !lua_isnil(L, -1), was = lua_toboolean(L, -1);
            lua_pop(L, 1);
            if (known && was == near_now) return false;
            if (!known && !near_now) { /* Starting outside: nothing to report yet. */
                set_bool(L, e, "near", false);
                return false;
            }
            set_bool(L, e, "near", near_now);
            set_bool(L, arg, "near", near_now);
            return true;
        }
        double min_move = get_num(L, e, "min_move", 0);
        if (min_move > 0) {
            double lx = get_num(L, e, "lx", -1e18), ly = get_num(L, e, "ly", -1e18);
            double dx = c->cx - lx, dy = c->cy - ly;
            if (dx * dx + dy * dy < min_move * min_move) return false;
            set_num(L, e, "lx", c->cx);
            set_num(L, e, "ly", c->cy);
        }
        lua_pushnil(L);
        lua_setfield(L, arg, "near");
        return true;
    }
    case SEV_PEER_MOVED: case SEV_PEER_ADDED: case SEV_PEER_REMOVED:
    case SEV_COLLIDE: case SEV_SEPARATE: {
        double want = get_num(L, e, "peer", 0);
        if (want > 0 && (int)want != c->peer->id) return false;
        double radius = get_num(L, e, "radius", 0);
        if (radius > 0 && c->ev != SEV_PEER_REMOVED) {
            double cx, cy;
            self_center(s, &cx, &cy);
            double dx = c->peer->hx + c->peer->hw / 2.0 - cx, dy = c->peer->hy + c->peer->hh / 2.0 - cy;
            if (dx * dx + dy * dy > radius * radius) return false;
        }
        return true;
    }
    case SEV_FRAME: case SEV_CLIP_END: {
        if (lua_getfield(L, e, "clip") == LUA_TSTRING && strcmp(lua_tostring(L, -1), c->clip) != 0) {
            lua_pop(L, 1);
            return false;
        }
        lua_pop(L, 1);
        if (c->ev == SEV_FRAME && lua_getfield(L, e, "frames") == LUA_TTABLE) {
            bool hit = lua_rawgeti(L, -1, c->frame) != LUA_TNIL;
            lua_pop(L, 2);
            return hit;
        }
        if (c->ev == SEV_FRAME) lua_pop(L, 1);
        return true;
    }
    default:
        return true;
    }
}

/* Pushes the handler arguments for c; returns their count. arg receives the
 * index of the event table (input / cursor), or 0. */
static int push_args(lua_State *L, Call *c, int *arg)
{
    *arg = 0;
    if (c->ev <= SEV_DRAG_END) {
        push_event_table(L, c->ev);
        fill_input(L, lua_gettop(L), c->in);
        *arg = lua_gettop(L);
        return 1;
    }
    switch (c->ev) {
    case SEV_CURSOR:
        push_event_table(L, c->ev);
        *arg = lua_gettop(L);
        set_int(L, *arg, "x", c->cx);
        set_int(L, *arg, "y", c->cy);
        set_bool(L, *arg, "valid", c->cvalid);
        return 1;
    case SEV_PEER_MOVED: case SEV_PEER_ADDED: case SEV_PEER_REMOVED: case SEV_COLLIDE: case SEV_SEPARATE:
        push_peer(L, c->peer);
        return 1;
    case SEV_FRAME:
        lua_pushinteger(L, c->frame + 1);
        lua_pushstring(L, c->clip);
        return 2;
    case SEV_CLIP_END:
        lua_pushstring(L, c->clip);
        return 1;
    default:
        return 0;
    }
}

static void dispatch_event(lua_State *L, Call *c)
{
    Script *s = c->s;
    int base = lua_gettop(L), arg = 0;
    int nargs = push_args(L, c, &arg); /* Built once, shared by every handler. */
    const char *legacy = legacy_name(c->ev);
    if (legacy) {
        if (raw_global(L, legacy) == LUA_TFUNCTION) {
            for (int i = 1; i <= nargs; ++i) lua_pushvalue(L, base + i);
            lua_call(L, nargs, 0);
        } else {
            lua_pop(L, 1);
        }
    }
    if (s->handler_count[c->ev] > 0) {
        push_handler_list(L, c->ev);
        int list = lua_gettop(L);
        lua_Integer n = (lua_Integer)lua_rawlen(L, list); /* Handlers added meanwhile wait for the next event. */
        s->depth++;
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_rawgeti(L, list, i);
            if (!lua_istable(L, -1) || get_bool(L, -1, "dead") || !accept(L, s, c, arg)) {
                lua_settop(L, list);
                continue;
            }
            lua_getfield(L, -1, "fn");
            for (int k = 1; k <= nargs; ++k) lua_pushvalue(L, base + k);
            lua_call(L, nargs, 1);
            bool routed = c->in && !c->in->passthrough;
            bool handled = routed && (lua_toboolean(L, -1) || (arg && get_bool(L, arg, "handled")));
            lua_settop(L, list);
            if (handled) {
                c->handled = true;
                break; /* Like a handled routed event: later handlers do not see it. */
            }
        }
        s->depth--;
    }
    lua_settop(L, base);
    compact_handlers(L, s);
}

/* Runs the timers that were due when the call started, earliest first. A
 * callback that keeps adding yagp.after(0, ...) runs again next time, not in
 * an endless loop; a repeating timer never bursts to catch up. */
static void dispatch_timers(lua_State *L, Call *c)
{
    Script *s = c->s;
    int ids[SCRIPT_MAX_TIMERS], n = 0;
    double dues[SCRIPT_MAX_TIMERS];
    for (int i = 0; i < s->timer_count; ++i) {
        if (s->timers[i].due > c->now) continue;
        int k = n++;
        while (k > 0 && dues[k - 1] > s->timers[i].due) {
            ids[k] = ids[k - 1];
            dues[k] = dues[k - 1];
            --k;
        }
        ids[k] = s->timers[i].id;
        dues[k] = s->timers[i].due;
    }
    for (int k = 0; k < n; ++k) {
        int i = 0;
        while (i < s->timer_count && s->timers[i].id != ids[k]) ++i;
        if (i == s->timer_count) continue; /* Cancelled by an earlier callback. */
        Timer *t = &s->timers[i];
        lua_rawgeti(L, LUA_REGISTRYINDEX, t->ref);
        if (t->period > 0) {
            t->due += t->period;
            if (t->due <= c->now) t->due = c->now + t->period;
        } else {
            remove_timer(L, s, i); /* Before the call: the callback may add timers. */
        }
        lua_call(L, 0, 0);
    }
}

/* Every entry from the host runs through here, under protected_call, so no
 * Lua error (script error, memory cap, budget) can escape to an unprotected
 * frame, where Lua would abort the process. Stack: [call, chunk?]. */
static int dispatch(lua_State *L)
{
    Call *c = (Call *)lua_touserdata(L, 1);
    if (c->kind == CALL_CHUNK) {
        lua_pushvalue(L, 2);
        lua_call(L, 0, 0);
    } else if (c->kind == CALL_EVENT) {
        dispatch_event(L, c);
    } else if (c->kind == CALL_TIMERS) {
        dispatch_timers(L, c);
    } else if (raw_global(L, c->fn) == LUA_TFUNCTION) {
        int nargs = 0;
        if (c->kind == CALL_DT) {
            lua_pushnumber(L, c->dt);
            nargs = 1;
        } else if (c->kind == CALL_PEER) {
            push_peer(L, c->peer);
            nargs = 1;
        }
        lua_call(L, nargs, 0);
    }
    lua_settop(L, 0);
    Script *s = c->s;
    s->has_update = raw_global(L, "update") == LUA_TFUNCTION;
    s->legacy = 0;
    for (int i = 0; i < (int)(sizeof(LEGACY_NAMES) / sizeof(LEGACY_NAMES[0])); ++i) {
        if (raw_global(L, LEGACY_NAMES[i]) == LUA_TFUNCTION) s->legacy |= SEV_BIT(LEGACY_EVENTS[i]);
    }
    lua_settop(L, 0);
    refresh_subs(s);
    return 0;
}

/* Calls the global fn (if it is a function) with the arguments kind implies. */
static bool call_global(Script *s, int kind, const char *fn, double dt, const ScriptPeer *peer,
                        wchar_t *err, size_t err_cap)
{
    Call c;
    memset(&c, 0, sizeof(c));
    c.s = s;
    c.fn = fn;
    c.kind = kind;
    c.dt = dt;
    c.peer = peer;
    lua_pushcfunction(s->L, dispatch);   /* A light C function: no allocation. */
    lua_pushlightuserdata(s->L, &c);
    return protected_call(s, 1, err, err_cap);
}

/* ---- state setup --------------------------------------------------------- */

/* setmetatable without finalizers. Lua runs __gc with debug hooks disabled
 * (during any collection, and in lua_close), so an endless finalizer would
 * freeze the UI thread for good. An object is only marked for finalization if
 * its metatable has __gc when setmetatable is called, so checking here is
 * enough; debug.setmetatable does not exist in this binary. */
static int l_setmetatable(lua_State *L)
{
    if (lua_type(L, 2) == LUA_TTABLE) {
        lua_pushliteral(L, "__gc");
        if (lua_rawget(L, 2) != LUA_TNIL) return luaL_error(L, TXT_U8_NO_GC);
        lua_pop(L, 1);
    }
    lua_pushvalue(L, lua_upvalueindex(1)); /* The base library's setmetatable. */
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, 1);
    return 1;
}

/* Only these libraries are compiled in at all (see CMakeLists.txt); io, os,
 * package and debug do not exist in this binary. */
static int open_sandbox(lua_State *L)
{
    Script *s = self(L);
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
    lua_settop(L, 0);

    static const char *const removed[] = {"dofile", "loadfile", "load", "require", "collectgarbage"};
    for (size_t i = 0; i < sizeof(removed) / sizeof(removed[0]); ++i) {
        lua_pushnil(L);
        lua_setglobal(L, removed[i]);
    }
    lua_getglobal(L, LUA_STRLIBNAME);
    lua_pushnil(L);
    lua_setfield(L, -2, "dump");
    lua_pop(L, 1);
    lua_pushcfunction(L, l_print);
    lua_setglobal(L, "print");
    luaL_newlib(L, log_lib);
    lua_setglobal(L, "log");
    luaL_newmetatable(L, FOLLOWER_MT);
    lua_pushcfunction(L, follower_index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, follower_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);
    lua_getglobal(L, "setmetatable");
    lua_pushcclosure(L, l_setmetatable, 1);
    lua_setglobal(L, "setmetatable");

    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    lua_getglobal(L, LUA_MATHLIBNAME);
    lua_getfield(L, -1, "randomseed");
    lua_pushinteger(L, (lua_Integer)t.QuadPart);
    lua_call(L, 1, 0);
    lua_pop(L, 1);

    luaL_newlib(L, yagp_lib);
    lua_pushstring(L, s->name);
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, s->id);
    lua_setfield(L, -2, "id");
    lua_setglobal(L, "yagp");
    return 0;
}

Script *script_create(const ScriptHostOps *ops, void *ctx, const char *chunk, size_t len,
                      const char *chunkname, const char *name, int id, wchar_t *err, size_t err_cap)
{
    return script_create_ex(ops, NULL, ctx, chunk, len, chunkname, name, id, err, err_cap);
}

Script *script_create_ex(const ScriptHostOps *ops, const ScriptHostExtOps *ext, void *ctx, const char *chunk,
                         size_t len, const char *chunkname, const char *name, int id, wchar_t *err,
                         size_t err_cap)
{
    if (err && err_cap) err[0] = 0;
    Script *s = (Script *)calloc(1, sizeof(Script));
    if (!s) {
        to_wide(TXT_U8_OUT_OF_MEMORY, err, err_cap);
        return NULL;
    }
    s->ops = ops;
    s->ext = ext;
    s->ctx = ctx;
    s->id = id;
    strncpy_s(s->name, sizeof(s->name), name ? name : "script", _TRUNCATE);
    s->L = lua_newstate(limited_alloc, s);
    if (!s->L) {
        to_wide(TXT_U8_LUA_STATE, err, err_cap);
        free(s);
        return NULL;
    }
    *(Script **)lua_getextraspace(s->L) = s;
    lua_sethook(s->L, budget_hook, LUA_MASKCOUNT, SCRIPT_HOOK_STEP);

    lua_pushcfunction(s->L, open_sandbox);
    if (!protected_call(s, 0, err, err_cap)) goto fail;

    char label[132];
    _snprintf_s(label, sizeof(label), _TRUNCATE, "=%s", chunkname ? chunkname : "script");
    if (luaL_loadbufferx(s->L, chunk, len, label, "t") != LUA_OK) {
        take_error(s, err, err_cap);
        goto fail;
    }
    /* [chunk] -> [dispatch, call, chunk]: inserting values does not allocate. */
    Call run;
    memset(&run, 0, sizeof(run));
    run.s = s;
    run.kind = CALL_CHUNK;
    lua_pushcfunction(s->L, dispatch);
    lua_insert(s->L, -2);
    lua_pushlightuserdata(s->L, &run);
    lua_insert(s->L, -2);
    if (!protected_call(s, 2, err, err_cap)) goto fail;
    return s;

fail:
    script_destroy(s);
    return NULL;
}

void script_destroy(Script *s)
{
    if (!s) return;
    if (s->L) lua_close(s->L); /* No script finalizers can exist: see l_setmetatable. */
    free(s);
}

bool script_has_update(Script *s)
{
    return s && s->has_update;
}

bool script_update(Script *s, double dt, wchar_t *err, size_t err_cap)
{
    if (!s->has_update) return true; /* Nothing to run: skip the call entirely. */
    return call_global(s, CALL_DT, "update", dt, NULL, err, err_cap);
}

bool script_event(Script *s, const char *fn, wchar_t *err, size_t err_cap)
{
    return call_global(s, CALL_PLAIN, fn, 0, NULL, err, err_cap);
}

bool script_event_peer(Script *s, const char *fn, const ScriptPeer *other, wchar_t *err, size_t err_cap)
{
    return call_global(s, CALL_PEER, fn, 0, other, err, err_cap);
}

unsigned script_subscriptions(Script *s)
{
    return s ? s->subs : 0;
}

/* Runs c (CALL_EVENT / CALL_TIMERS) under the budget. */
static bool run_call(Script *s, Call *c, wchar_t *err, size_t err_cap)
{
    lua_pushcfunction(s->L, dispatch);
    lua_pushlightuserdata(s->L, c);
    return protected_call(s, 1, err, err_cap);
}

static bool run_event(Script *s, Call *c, int ev, wchar_t *err, size_t err_cap)
{
    if (!(s->subs & SEV_BIT(ev))) return true; /* Not subscribed: no call into Lua at all. */
    c->s = s;
    c->kind = CALL_EVENT;
    c->ev = ev;
    return run_call(s, c, err, err_cap);
}

bool script_input(Script *s, ScriptEvent ev, const ScriptInput *in, bool *handled, wchar_t *err, size_t err_cap)
{
    Call c;
    memset(&c, 0, sizeof(c));
    c.in = in;
    if (handled) *handled = false;
    if ((unsigned)ev > SEV_DRAG_END) return true;
    bool ok = run_event(s, &c, ev, err, err_cap);
    if (handled) *handled = ok && c.handled && !in->passthrough;
    return ok;
}

bool script_cursor(Script *s, int x, int y, bool valid, wchar_t *err, size_t err_cap)
{
    Call c;
    memset(&c, 0, sizeof(c));
    c.cx = x;
    c.cy = y;
    c.cvalid = valid;
    return run_event(s, &c, SEV_CURSOR, err, err_cap);
}

bool script_peer_event(Script *s, ScriptEvent ev, const ScriptPeer *p, wchar_t *err, size_t err_cap)
{
    Call c;
    memset(&c, 0, sizeof(c));
    c.peer = p;
    if (ev != SEV_PEER_MOVED && ev != SEV_PEER_ADDED && ev != SEV_PEER_REMOVED && ev != SEV_COLLIDE &&
        ev != SEV_SEPARATE) return true;
    return run_event(s, &c, ev, err, err_cap);
}

bool script_clip_event(Script *s, ScriptEvent ev, const char *clip, int frame, wchar_t *err, size_t err_cap)
{
    Call c;
    memset(&c, 0, sizeof(c));
    c.clip = clip ? clip : "main";
    c.frame = frame;
    if (ev != SEV_FRAME && ev != SEV_CLIP_END) return true;
    return run_event(s, &c, ev, err, err_cap);
}

bool script_plain_event(Script *s, ScriptEvent ev, wchar_t *err, size_t err_cap)
{
    Call c;
    memset(&c, 0, sizeof(c));
    if (ev != SEV_MOVED && ev != SEV_SCREENS_CHANGED && ev != SEV_TRANSFORM_END) return true;
    return run_event(s, &c, ev, err, err_cap);
}

double script_next_timer(Script *s)
{
    double best = -1;
    if (!s) return -1;
    for (int i = 0; i < s->timer_count; ++i) {
        if (best < 0 || s->timers[i].due < best) best = s->timers[i].due;
    }
    /* Pending rate-limit summaries are serviced through the same timer path. */
    if (s->log_dropped > 0) {
        double t = s->log_window + SCRIPT_LOG_WINDOW;
        if (best < 0 || t < best) best = t;
    }
    if (s->notify_dropped > 0) {
        double t = s->notify_window;
        if (best < 0 || t < best) best = t;
    }
    return best;
}

bool script_run_timers(Script *s, double now, wchar_t *err, size_t err_cap)
{
    flush_log_summary(s, now);
    flush_notify_summary(s, now);
    double next = script_next_timer(s);
    if (next < 0 || next > now) return true;
    Call c;
    memset(&c, 0, sizeof(c));
    c.s = s;
    c.kind = CALL_TIMERS;
    c.now = now;
    return run_call(s, &c, err, err_cap);
}
