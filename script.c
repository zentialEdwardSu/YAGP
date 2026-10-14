#include "script.h"
#include "contacts.h"
#include "i18n.h"
#include "monitors.h"

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

/* { id, name, x, y, w, h, hx, hy, hw, hh, cx, cy, vx, vy, scripted, paused } */
static void push_peer(lua_State *L, const ScriptPeer *p)
{
    lua_createtable(L, 0, 16);
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

/* print -> OutputDebugString ("[YAGP name] ..."), visible in DebugView. */
static int l_print(lua_State *L)
{
    Script *s = self(L);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    luaL_addstring(&b, "[YAGP ");
    luaL_addstring(&b, s->name);
    luaL_addstring(&b, "] ");
    int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        if (i > 1) luaL_addchar(&b, '\t');
        luaL_tolstring(L, i, NULL);
        luaL_addvalue(&b);
    }
    luaL_addchar(&b, '\n');
    luaL_pushresult(&b);
    wchar_t text[1024];
    to_wide(lua_tostring(L, -1), text, 1024);
    OutputDebugStringW(text);
    return 0;
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
    {NULL, NULL}
};

/* ---- calls into the script ------------------------------------------------ */

enum { CALL_PLAIN, CALL_DT, CALL_PEER, CALL_CHUNK };

typedef struct Call {
    Script *s;
    const char *fn;          /* Global to call (not for CALL_CHUNK). */
    int kind;
    double dt;               /* CALL_DT */
    const ScriptPeer *peer;  /* CALL_PEER */
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

/* Every entry from the host runs through here, under protected_call, so no
 * Lua error (script error, memory cap, budget) can escape to an unprotected
 * frame, where Lua would abort the process. Stack: [call, chunk?]. */
static int dispatch(lua_State *L)
{
    Call *c = (Call *)lua_touserdata(L, 1);
    if (c->kind == CALL_CHUNK) {
        lua_pushvalue(L, 2);
        lua_call(L, 0, 0);
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
    c->s->has_update = raw_global(L, "update") == LUA_TFUNCTION;
    return 0;
}

/* Calls the global fn (if it is a function) with the arguments kind implies. */
static bool call_global(Script *s, int kind, const char *fn, double dt, const ScriptPeer *peer,
                        wchar_t *err, size_t err_cap)
{
    Call c = {s, fn, kind, dt, peer};
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
    if (err && err_cap) err[0] = 0;
    Script *s = (Script *)calloc(1, sizeof(Script));
    if (!s) {
        to_wide(TXT_U8_OUT_OF_MEMORY, err, err_cap);
        return NULL;
    }
    s->ops = ops;
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
    Call run = {s, NULL, CALL_CHUNK, 0, NULL};
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
