/* Lua host + preset regression test. Runs script.c against a fake window and a
 * fake monitor layout; exits non-zero on the first failed check.
 *   cmake --build build --target script_test && build\script_test.exe */
#include "clip_path.h"
#include "contacts.h"
#include "input_fsm.h"
#include "transform.h"
#include "i18n.h"
#include "log.h"
#include "monitors.h"
#include "script.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct FakeWin {
    double x, y;
    int w, h;
    bool flip;
    int flips, rate;
    ScriptMouse mouse;
    bool mouse_ok;
    int id;
    const char *name;
    bool scripted;
    /* Hitbox in GIF pixels (l t r b; empty = whole window). The fake GIF is
     * gw x gh (0 = same as the window, i.e. 100% scale). */
    int box[4];
    int gw, gh;
    bool ovr;
    int ovr_box[4];
    bool still;
    bool hidden;
    bool paused;
} FakeWin;

/* Same mapping the real host uses (overlay_hitbox). out = dx, dy, w, h. */
static void fake_hitbox(const FakeWin *f, int out[4])
{
    hitbox_in_window(f->ovr ? f->ovr_box : f->box, f->gw ? f->gw : f->w, f->gh ? f->gh : f->h,
                     f->w, f->h, f->flip, out);
}

/* Every FakeWin a script can see as a peer (empty unless a test fills it). */
static FakeWin *s_world[8];
static int s_world_n;

static void world_set(FakeWin **wins, int n)
{
    for (int i = 0; i < n; ++i) s_world[i] = wins[i];
    s_world_n = n;
}

static void get_rect(void *ctx, double *x, double *y, int *w, int *h)
{
    FakeWin *f = (FakeWin *)ctx;
    *x = f->x; *y = f->y; *w = f->w; *h = f->h;
}
static void set_pos(void *ctx, double x, double y) { FakeWin *f = (FakeWin *)ctx; f->x = x; f->y = y; }
static void set_flip(void *ctx, bool v)
{
    FakeWin *f = (FakeWin *)ctx;
    if (f->flip != v) f->flips++;
    f->flip = v;
}
static void set_rate(void *ctx, int hz) { ((FakeWin *)ctx)->rate = hz; }
static bool get_mouse(void *ctx, ScriptMouse *m)
{
    FakeWin *f = (FakeWin *)ctx;
    if (!f->mouse_ok) return false;
    *m = f->mouse;
    return true;
}
static int get_peers(void *ctx, ScriptPeer *out, int cap)
{
    int n = 0;
    for (int i = 0; i < s_world_n && n < cap; ++i) {
        const FakeWin *f = s_world[i];
        if (f == (const FakeWin *)ctx) continue;
        ScriptPeer *p = &out[n++];
        memset(p, 0, sizeof(*p));
        p->id = f->id;
        p->x = f->x;
        p->y = f->y;
        p->w = f->w;
        p->h = f->h;
        p->scripted = f->scripted;
        p->name = f->name ? f->name : "peer.gif";
        int hb[4];
        fake_hitbox(f, hb);
        p->hx = f->x + hb[0];
        p->hy = f->y + hb[1];
        p->hw = hb[2];
        p->hh = hb[3];
    }
    return n;
}
static void get_hitbox(void *ctx, int *dx, int *dy, int *w, int *h)
{
    int hb[4];
    fake_hitbox((const FakeWin *)ctx, hb);
    *dx = hb[0];
    *dy = hb[1];
    *w = hb[2];
    *h = hb[3];
}
static void set_hitbox(void *ctx, const int *box)
{
    FakeWin *f = (FakeWin *)ctx;
    f->ovr = box != NULL;
    if (box) {
        f->ovr_box[0] = box[0];
        f->ovr_box[1] = box[1];
        f->ovr_box[2] = box[0] + box[2];
        f->ovr_box[3] = box[1] + box[3];
    }
}
static const ScriptHostOps OPS = {get_rect, set_pos, set_flip, set_rate, get_mouse, get_peers,
                                  get_hitbox, set_hitbox};

static void set_mouse(FakeWin *w, int x, int y)
{
    w->mouse.x = x;
    w->mouse.y = y;
    w->mouse_ok = true;
}

/* Moves the fake cursor and tells s, like the host's mouse hook does. */
static void move_mouse(Script *s, FakeWin *w, int x, int y)
{
    wchar_t err[512];
    set_mouse(w, x, y);
    if (s && !script_cursor(s, x, y, true, err, 512)) printf("     cursor event: %ls\n", err);
}

static double center_dist(const FakeWin *w)
{
    double dx = w->x + w->w / 2.0 - w->mouse.x, dy = w->y + w->h / 2.0 - w->mouse.y;
    return sqrt(dx * dx + dy * dy);
}

static int s_failures;
#define CHECK(cond, ...) do { if (!(cond)) { ++s_failures; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- captured log lines (yagp.log / print / rate-limit summaries) ---------- */

#define LOG_CAP 512
static struct {
    int level;
    char source[160];
    char text[1200];
} s_logs[LOG_CAP];
static int s_logs_n;

static void collect(log_Event *ev)
{
    if (s_logs_n >= LOG_CAP) return;
    s_logs[s_logs_n].level = ev->level;
    strncpy_s(s_logs[s_logs_n].source, sizeof(s_logs[0].source), ev->file ? ev->file : "", _TRUNCATE);
    vsnprintf(s_logs[s_logs_n].text, sizeof(s_logs[0].text), ev->fmt, ev->ap);
    ++s_logs_n;
}

static void logs_reset(void) { s_logs_n = 0; }

static int logs_count(int level)
{
    int n = 0;
    for (int i = 0; i < s_logs_n; ++i) {
        if (level < 0 || s_logs[i].level == level) ++n;
    }
    return n;
}

static bool logs_contain(int level, const char *needle)
{
    for (int i = 0; i < s_logs_n; ++i) {
        if ((level < 0 || s_logs[i].level == level) && strstr(s_logs[i].text, needle)) return true;
    }
    return false;
}

static bool logs_source_contains(const char *needle)
{
    for (int i = 0; i < s_logs_n; ++i) {
        if (strstr(s_logs[i].source, needle)) return true;
    }
    return false;
}

/* True if err contains TXT_U8_SCRIPT_TIMEOUT up to its first format specifier,
 * so the check works whatever YAGP_LANG the build uses. */
static bool has_timeout_text(const wchar_t *err)
{
    char prefix[256];
    strncpy_s(prefix, sizeof(prefix), TXT_U8_SCRIPT_TIMEOUT, _TRUNCATE);
    char *pct = strchr(prefix, '%');
    if (pct) *pct = 0;
    wchar_t wide[256];
    if (MultiByteToWideChar(CP_UTF8, 0, prefix, -1, wide, 256) <= 0) return false;
    return wide[0] && wcsstr(err, wide) != NULL;
}

static Script *load(FakeWin *w, const char *src, wchar_t *err)
{
    return script_create(&OPS, w, src, strlen(src), "test.lua", "test.gif", w->id, err, 512);
}

static char *read_file(const char *name, size_t *len)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", YAGP_SCRIPT_DIR, name);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    if (buf) buf[n] = 0;
    fclose(f);
    *len = (size_t)n;
    return buf;
}

/* Loads scripts/<name>, optionally replacing the first `find` with `replace`
 * (used to flip tunables such as ROAM). */
static Script *load_preset(FakeWin *w, const char *name, const char *find, const char *replace, wchar_t *err)
{
    size_t len;
    char *src = read_file(name, &len);
    if (!src) { swprintf(err, 512, L"cannot read %hs", name); return NULL; }
    char *text = src;
    if (find) {
        char *at = strstr(src, find);
        if (!at) { free(src); swprintf(err, 512, L"%hs: no '%hs'", name, find); return NULL; }
        size_t fl = strlen(find), rl = strlen(replace);
        text = (char *)malloc(len - fl + rl + 1);
        memcpy(text, src, (size_t)(at - src));
        memcpy(text + (at - src), replace, rl);
        strcpy(text + (at - src) + rl, at + fl);
        len = len - fl + rl;
    }
    Script *s = script_create(&OPS, w, text, len, name, "test.gif", w->id, err, 512);
    if (text != src) free(text);
    free(src);
    return s;
}

static Monitor mon(int x, int y, int w, int h, int taskbar, bool primary)
{
    Monitor m;
    SetRect(&m.bounds, x, y, x + w, y + h);
    SetRect(&m.work, x, y, x + w, y + h - taskbar);
    m.dpi = 96;
    m.primary = primary;
    return m;
}

static bool fits(const FakeWin *w)
{
    LONG l = (LONG)(w->x < 0 ? w->x - 0.5 : w->x + 0.5), t = (LONG)(w->y < 0 ? w->y - 0.5 : w->y + 0.5);
    RECT r = {l, t, l + w->w, t + w->h};
    return monitors_rect_fits(&r);
}

static bool inside(const FakeWin *w, const RECT *r)
{
    return w->x >= r->left - 0.5 && w->y >= r->top - 0.5 &&
           w->x + w->w <= r->right + 0.5 && w->y + w->h <= r->bottom + 0.5;
}

static void test_sandbox(void)
{
    FakeWin w = {100, 100, 50, 50, false, 0, 0};
    wchar_t err[512];
    Script *s = load(&w, "assert(io == nil and os == nil and require == nil and load == nil)\n"
                         "assert(loadfile == nil and dofile == nil and package == nil and debug == nil)\n"
                         "assert(collectgarbage == nil and string.dump == nil)\n"
                         "assert(type(yagp.pos) == 'function' and yagp.name == 'test.gif')\n", err);
    CHECK(s, "sandbox globals: %ls", err);
    script_destroy(s);

    /* Bytecode is rejected by mode "t". */
    s = script_create(&OPS, &w, "\x1bLua\x54\x00", 6, "bc", "test.gif", 0, err, 512);
    CHECK(!s && wcsstr(err, L"binary"), "bytecode accepted: %ls", err);
    script_destroy(s);

    /* An infinite loop is cut off, also when pcall tries to swallow the error. */
    s = load(&w, "function update() while true do end end", err);
    CHECK(s, "load loop: %ls", err);
    if (s) {
        CHECK(!script_update(s, 0.016, err, 512) && has_timeout_text(err), "loop not stopped: %ls", err);
        script_destroy(s);
    }
    s = load(&w, "function update() while true do pcall(function() while true do end end) end end", err);
    if (s) {
        CHECK(!script_update(s, 0.016, err, 512), "pcall loop not stopped");
        script_destroy(s);
    }

    /* Memory cap. */
    s = load(&w, "local t = {} for i = 1, 1e8 do t[i] = string.rep('x', 64) .. i end", err);
    CHECK(!s && wcsstr(err, L"memory"), "memory cap: %ls", err);
    script_destroy(s);

    /* Runtime errors carry the chunk name and line. */
    s = load(&w, "function update()\n  local x = nil\n  return x.y\nend", err);
    CHECK(s, "load err script: %ls", err);
    if (s) {
        CHECK(!script_update(s, 0.016, err, 512) && wcsstr(err, L"test.lua:3:"), "error line: %ls", err);
        script_destroy(s);
    }

    /* Syntax error at load. */
    s = load(&w, "function update(", err);
    CHECK(!s && wcsstr(err, L"test.lua:1:"), "syntax error: %ls", err);

    /* NaN coordinates are rejected. */
    s = load(&w, "yagp.set_pos(0/0, 0)", err);
    CHECK(!s, "NaN accepted");
    script_destroy(s);

    /* API values. */
    s = load(&w, "local x, y = yagp.pos() assert(x == 100 and y == 100)\n"
                 "local w, h = yagp.size() assert(w == 50 and h == 50)\n"
                 "yagp.move(5, -5) yagp.set_rate(500)\n"
                 "local ms = yagp.monitors() assert(#ms == 2 and ms[1].primary and ms[2].work.w == 1280)\n"
                 "local m, i = yagp.monitor() assert(i == 1 and m.index == 1)\n"
                 "local dx, dy, dw, dh = yagp.desktop() assert(dx == 0 and dw == 3200)\n"
                 "assert(yagp.fits(0, 0) and not yagp.fits(1900, 1050) and not yagp.fits(-10, 0))\n"
                 "assert(type(yagp.time()) == 'number')", err);
    CHECK(s, "api: %ls", err);
    CHECK(w.x == 105 && w.y == 95 && w.rate == SCRIPT_RATE_MAX, "move/rate: %g %g %d", w.x, w.y, w.rate);
    script_destroy(s);

    /* Mouse API: cursor/mouse/center/monitor_at, hover test, unreadable cursor. */
    w = (FakeWin){100, 100, 50, 50, false, 0, 0};
    set_mouse(&w, 120, 130);
    w.mouse.left = true;
    s = load(&w, "local x, y, ok = yagp.cursor() assert(x == 120 and y == 130 and ok)\n"
                 "local m = yagp.mouse() assert(m.over and m.left and not m.right and m.valid and m.monitor == 1)\n"
                 "local cx, cy = yagp.center() assert(cx == 125 and cy == 125)\n"
                 "local mm, i = yagp.monitor_at(2500, 600) assert(i == 2 and mm.index == 2)\n"
                 "function update() local x, y, ok = yagp.cursor()\n"
                 "  assert(not ok and x == 120 and y == 130 and yagp.mouse().over) end", err);
    CHECK(s, "mouse api: %ls", err);
    if (s) {
        w.mouse_ok = false; /* Lock screen: last known position, valid = false. */
        CHECK(script_update(s, 0.016, err, 512), "mouse invalid: %ls", err);
        script_destroy(s);
    }
    set_mouse(&w, 500, 500);
    s = load(&w, "assert(not yagp.mouse().over)", err);
    CHECK(s, "mouse not over: %ls", err);
    script_destroy(s);
}

static void run(Script *s, int ticks, FakeWin *w, bool (*ok)(const FakeWin *, const void *), const void *ctx,
                const char *what)
{
    wchar_t err[512];
    for (int i = 0; i < ticks; ++i) {
        if (!script_update(s, 1.0 / 60, err, 512)) {
            CHECK(false, "%s: update failed at %d: %ls", what, i, err);
            return;
        }
        if (!ok(w, ctx)) {
            CHECK(false, "%s: out of bounds at tick %d: (%g, %g)", what, i, w->x, w->y);
            return;
        }
    }
}

static bool ok_in_rect(const FakeWin *w, const void *ctx) { return inside(w, (const RECT *)ctx); }
static bool ok_fits(const FakeWin *w, const void *ctx) { (void)ctx; return fits(w); }

typedef struct PatrolBox { double lo, hi; RECT work; } PatrolBox;
static bool ok_patrol(const FakeWin *w, const void *ctx)
{
    const PatrolBox *b = (const PatrolBox *)ctx;
    return w->x >= b->lo - 0.5 && w->x <= b->hi + 0.5 && inside(w, &b->work);
}

static void test_presets(void)
{
    wchar_t err[512];
    const Monitor *primary = monitors_get(0);
    const RECT work = primary->work;

    /* bounce, default: stays on the primary work area. */
    FakeWin w = {800, 500, 120, 90, false, 0, 0};
    Script *s = load_preset(&w, "bounce.lua", NULL, NULL, err);
    CHECK(s, "bounce: %ls", err);
    if (s) {
        run(s, 100000, &w, ok_in_rect, &work, "bounce");
        CHECK(w.flips > 0, "bounce never turned around");
        script_destroy(s);
    }

    /* bounce, ROAM: never enters the gap, and reaches the second monitor. */
    w = (FakeWin){800, 500, 120, 90, false, 0, 0};
    s = load_preset(&w, "bounce.lua", "ROAM = false", "ROAM = true", err);
    CHECK(s, "bounce roam: %ls", err);
    if (s) {
        wchar_t e2[512];
        bool reached = false;
        for (int i = 0; i < 200000; ++i) {
            if (!script_update(s, 1.0 / 60, e2, 512)) { CHECK(false, "roam update: %ls", e2); break; }
            if (!fits(&w)) { CHECK(false, "roam left work areas at tick %d: (%g, %g)", i, w.x, w.y); break; }
            if (w.x >= 1920) reached = true;
        }
        CHECK(reached, "roam never reached the second monitor");
        script_destroy(s);
    }

    /* bounce from off-screen gets pulled back in. */
    w = (FakeWin){-500, 3000, 120, 90, false, 0, 0};
    s = load_preset(&w, "bounce.lua", NULL, NULL, err);
    if (s) {
        CHECK(fits(&w), "bounce did not clamp an off-screen start: (%g, %g)", w.x, w.y);
        script_destroy(s);
    }

    /* patrol: spans the full work-area width of its monitor, turns at both
     * edges, keeps its height. */
    w = (FakeWin){900, 400, 100, 100, false, 0, 0};
    s = load_preset(&w, "patrol.lua", NULL, NULL, err);
    CHECK(s, "patrol: %ls", err);
    if (s) {
        PatrolBox b = {0, 1920 - 100, work};
        double lo = 1e9, hi = -1e9;
        for (int i = 0; i < 20000; ++i) {
            if (!script_update(s, 1.0 / 60, err, 512)) { CHECK(false, "patrol update: %ls", err); break; }
            if (!ok_patrol(&w, &b)) { CHECK(false, "patrol out of bounds: (%g, %g)", w.x, w.y); break; }
            if (w.x < lo) lo = w.x;
            if (w.x > hi) hi = w.x;
        }
        CHECK(lo < 0.5 && hi > 1820 - 0.5, "patrol did not span the width: %g..%g", lo, hi);
        CHECK(w.flips >= 2, "patrol flips: %d", w.flips);
        CHECK(w.y == 400, "patrol moved vertically");
        script_destroy(s);
    }

    /* patrol on the second monitor: uses that monitor's width, not the primary's. */
    w = (FakeWin){2500, 600, 100, 100, false, 0, 0};
    s = load_preset(&w, "patrol.lua", NULL, NULL, err);
    if (s) {
        const RECT *w2 = &monitors_get(1)->work;
        PatrolBox b = {(double)w2->left, (double)w2->right - 100, *w2};
        double lo = 1e9, hi = -1e9;
        for (int i = 0; i < 20000; ++i) {
            if (!script_update(s, 1.0 / 60, err, 512)) { CHECK(false, "patrol2 update: %ls", err); break; }
            if (!ok_patrol(&w, &b)) { CHECK(false, "patrol2 out of bounds: (%g, %g)", w.x, w.y); break; }
            if (w.x < lo) lo = w.x;
            if (w.x > hi) hi = w.x;
        }
        CHECK(lo < w2->left + 0.5 && hi > w2->right - 100 - 0.5, "patrol2 span: %g..%g", lo, hi);
        script_destroy(s);
    }

    /* patrol started below the work area: pulled up above the taskbar. */
    w = (FakeWin){900, 1050, 100, 100, false, 0, 0};
    s = load_preset(&w, "patrol.lua", NULL, NULL, err);
    if (s) {
        CHECK(w.y == 1040 - 100, "patrol did not clamp y: %g", w.y);
        script_destroy(s);
    }

    /* wander: stays on the work area and actually moves. */
    w = (FakeWin){300, 300, 80, 80, false, 0, 0};
    s = load_preset(&w, "wander.lua", NULL, NULL, err);
    CHECK(s, "wander: %ls", err);
    if (s) {
        run(s, 100000, &w, ok_in_rect, &work, "wander");
        CHECK(w.x != 300 || w.y != 300, "wander did not move");
        script_destroy(s);
    }

    /* on_screens_changed: the second monitor vanishes while the GIF is on it. */
    w = (FakeWin){2500, 300, 80, 80, false, 0, 0};
    s = load_preset(&w, "wander.lua", NULL, NULL, err);
    if (s) {
        Monitor one[1] = {*primary};
        Monitor saved[2] = {*monitors_get(0), *monitors_get(1)};
        monitors_set_fake(one, 1);
        CHECK(script_event(s, "on_screens_changed", err, 512), "event: %ls", err);
        CHECK(inside(&w, &work), "not pulled back after unplug: (%g, %g)", w.x, w.y);
        monitors_set_fake(saved, 2);
        script_destroy(s);
    }

    /* follow: closes in on the mouse, stops near it, never leaves the work areas. */
    w = (FakeWin){100, 100, 80, 80, false, 0, 0};
    set_mouse(&w, 1500, 800);
    s = load_preset(&w, "follow.lua", NULL, NULL, err);
    CHECK(s, "follow: %ls", err);
    if (s) {
        run(s, 600, &w, ok_fits, NULL, "follow");
        CHECK(center_dist(&w) <= 61, "follow did not arrive: %g", center_dist(&w));
        double before_x = w.x;
        run(s, 60, &w, ok_fits, NULL, "follow idle");
        CHECK(fabs(w.x - before_x) < 0.01, "follow jitters inside STOP");
        CHECK(!script_has_update(s), "follow keeps ticking after arriving");
        /* Mouse on the second monitor: follows across without entering the gap. */
        move_mouse(s, &w, 2600, 700);
        run(s, 1200, &w, ok_fits, NULL, "follow cross");
        CHECK(w.x >= 1920, "follow did not cross to monitor 2: (%g, %g)", w.x, w.y);
        /* Mouse in the gap above monitor 2: slides along the edge, stays on screen. */
        move_mouse(s, &w, 2500, 50);
        run(s, 1200, &w, ok_fits, NULL, "follow gap");
        /* Mouse off every screen edge: clamps, no error. */
        move_mouse(s, &w, -5000, -5000);
        run(s, 1200, &w, ok_fits, NULL, "follow far");
        script_destroy(s);
    }

    /* flee: keeps away from a cursor that sits next to it, stays on the work area,
     * escapes from a corner, and returns home once the mouse leaves. */
    w = (FakeWin){900, 500, 80, 80, false, 0, 0};
    set_mouse(&w, 2000, 2000);
    s = load_preset(&w, "flee.lua", NULL, NULL, err);
    CHECK(s, "flee: %ls", err);
    if (s) {
        move_mouse(s, &w, 950, 540); /* Right on top of it. */
        run(s, 120, &w, ok_in_rect, &work, "flee");
        CHECK(center_dist(&w) > 150, "flee did not get away: %g", center_dist(&w));
        /* Keep chasing it into the top-left corner. It may run back there after
         * each escape, but must never stay cornered for long. */
        int cornered = 0, longest = 0, escapes = 0;
        for (int i = 0; i < 600; ++i) {
            move_mouse(s, &w, (int)(w.x + w.w / 2.0) + 30, (int)(w.y + w.h / 2.0) + 30);
            double px = w.x;
            if (!script_update(s, 1.0 / 60, err, 512)) { CHECK(false, "flee chase: %ls", err); break; }
            if (!inside(&w, &work)) { CHECK(false, "flee left the work area: (%g, %g)", w.x, w.y); break; }
            if (w.x - px > 500) ++escapes;
            cornered = (w.x < 1 && w.y < 1) ? cornered + 1 : 0;
            if (cornered > longest) longest = cornered;
        }
        CHECK(escapes > 0, "flee never escaped the corner");
        CHECK(longest <= 30, "flee stayed cornered for %d ticks", longest);
        /* Mouse gone: drifts back home (900, 500). */
        move_mouse(s, &w, 1900, 1030);
        run(s, 60 * 60, &w, ok_in_rect, &work, "flee return");
        CHECK(fabs(w.x - 900) < 1 && fabs(w.y - 500) < 1, "flee did not return home: (%g, %g)", w.x, w.y);
        /* Unreadable cursor: stays still. */
        w.mouse_ok = false;
        double hx = w.x;
        run(s, 60, &w, ok_in_rect, &work, "flee no mouse");
        CHECK(w.x == hx, "flee moved without a cursor");
        /* Settled at home with the mouse far away: no per-tick work at all. */
        CHECK(!script_has_update(s), "flee keeps ticking at home");
        w.mouse_ok = true;
        move_mouse(s, &w, 950, 540); /* Back on top of it: wakes up. */
        CHECK(script_has_update(s), "flee did not wake on cursor_near");
        script_destroy(s);
    }
}

/* ---- peers and contacts ---------------------------------------------------- */

typedef struct ContactLog { int a[16], b[16]; bool begin[16]; int n; } ContactLog;

static void log_contact(void *ctx, int a, int b, bool begin)
{
    ContactLog *l = (ContactLog *)ctx;
    if (l->n < 16) {
        l->a[l->n] = a;
        l->b[l->n] = b;
        l->begin[l->n] = begin;
        ++l->n;
    }
}

static ContactBox box(int id, int x, int y, int w, int h)
{
    ContactBox b = {id, x, y, x + w, y + h};
    return b;
}

static void test_contacts(void)
{
    Contacts c = {0};
    ContactLog log = {0};
    ContactBox b[3] = {box(1, 0, 0, 10, 10), box(2, 20, 0, 10, 10), box(3, 100, 100, 5, 5)};

    contacts_update(&c, b, 3, log_contact, &log);
    CHECK(log.n == 0, "contacts: apart but %d events", log.n);

    b[1] = box(2, 10, 0, 10, 10); /* Shares an edge only. */
    contacts_update(&c, b, 3, log_contact, &log);
    CHECK(log.n == 0, "contacts: edge counted as contact");

    b[1] = box(2, 9, 0, 10, 10);
    contacts_update(&c, b, 3, log_contact, &log);
    CHECK(log.n == 1 && log.begin[0] && log.a[0] == 1 && log.b[0] == 2, "contacts: begin %d", log.n);

    contacts_update(&c, b, 3, log_contact, &log);
    CHECK(log.n == 1, "contacts: repeated begin");

    b[1] = box(2, 30, 0, 10, 10);
    contacts_update(&c, b, 3, log_contact, &log);
    CHECK(log.n == 2 && !log.begin[1] && log.a[1] == 1 && log.b[1] == 2, "contacts: end");

    b[1] = box(2, 5, 0, 10, 10);
    contacts_update(&c, b, 3, log_contact, &log);
    CHECK(log.n == 3 && log.begin[2], "contacts: begin again");

    /* Id 2 disappears while touching: an end for (1, 2). */
    ContactBox gone[2] = {b[0], b[2]};
    contacts_update(&c, gone, 2, log_contact, &log);
    CHECK(log.n == 4 && !log.begin[3] && log.a[3] == 1 && log.b[3] == 2, "contacts: vanished id");

    /* Three-way pile-up: begins in ascending (a, b) order. */
    log.n = 0;
    ContactBox pile[3] = {box(7, 0, 0, 10, 10), box(5, 2, 2, 10, 10), box(6, 4, 4, 10, 10)};
    contacts_update(&c, pile, 3, log_contact, &log);
    CHECK(log.n == 3 && log.a[0] == 5 && log.b[0] == 6 && log.a[1] == 5 && log.b[1] == 7 &&
          log.a[2] == 6 && log.b[2] == 7, "contacts: order (%d events)", log.n);
    contacts_free(&c);
}

static void test_peer_api(void)
{
    wchar_t err[512];
    FakeWin me = {100, 100, 50, 50, false, 0, 0};
    FakeWin other = {140, 120, 30, 30, false, 0, 0};
    FakeWin far_away = {150, 100, 20, 20, false, 0, 0}; /* Touches me only along x = 150. */
    me.id = 1;
    other.id = 2;
    other.name = "ball.gif";
    other.scripted = true;
    far_away.id = 3;
    FakeWin *world[3] = {&me, &other, &far_away};
    world_set(world, 3);

    Script *s = load(&me,
        "assert(yagp.id == 1)\n"
        "local o = yagp.others() assert(#o == 2, 'others ' .. #o)\n"
        "assert(o[1].id == 2 and o[1].name == 'ball.gif' and o[1].scripted and not o[1].paused)\n"
        "assert(o[1].x == 140 and o[1].w == 30 and o[1].cx == 155 and o[1].cy == 135)\n"
        "assert(yagp.peer(3).id == 3 and yagp.peer(99) == nil)\n"
        "local hit = yagp.overlaps() assert(#hit == 1 and hit[1].id == 2, 'default rect')\n"
        "assert(#yagp.overlaps(0, 0) == 0, 'moved away')\n"
        "assert(#yagp.overlaps(150, 100, 20, 20) == 1, 'explicit rect')\n"
        "function on_collide(p) assert(p.id == 2 and p.name == 'ball.gif') hits = (hits or 0) + 1 end\n"
        "function check() assert(hits == 1, 'hits ' .. tostring(hits)) end\n", err);
    CHECK(s, "peer api: %ls", err);
    if (s) {
        ScriptPeer p = {.id = 2, .x = 140, .y = 120, .w = 30, .h = 30, .hx = 140, .hy = 120,
                        .hw = 30, .hh = 30, .scripted = true, .name = "ball.gif"};
        CHECK(script_event_peer(s, "on_collide", &p, err, 512), "on_collide: %ls", err);
        CHECK(script_event(s, "check", err, 512), "on_collide not delivered: %ls", err);
        CHECK(script_event_peer(s, "on_separate", &p, err, 512), "missing on_separate must be ok");
        script_destroy(s);
    }
    s = load(&me, "function on_collide(p) error('boom') end", err);
    if (s) {
        ScriptPeer p = {.id = 2, .w = 1, .h = 1, .hw = 1, .hh = 1, .name = ""};
        CHECK(!script_event_peer(s, "on_collide", &p, err, 512) && wcsstr(err, L"boom"), "error in on_collide");
        script_destroy(s);
    }
    world_set(NULL, 0);
}

static bool overlapping(const FakeWin *a, const FakeWin *b)
{
    int al = (int)lround(a->x), at = (int)lround(a->y), bl = (int)lround(b->x), bt = (int)lround(b->y);
    return contacts_overlap(al, at, al + a->w, at + a->h, bl, bt, bl + b->w, bt + b->h);
}

static bool near_to(const FakeWin *a, const FakeWin *b, int gap)
{
    FakeWin g = *a;
    g.x -= gap;
    g.y -= gap;
    g.w += 2 * gap;
    g.h += 2 * gap;
    return overlapping(&g, b);
}

/* Two bouncing GIFs and one static obstacle (no script) sharing one world. */
static void test_collisions(void)
{
    wchar_t err[512];
    const RECT work = monitors_get(0)->work;

    for (int collide = 1; collide >= 0; --collide) {
        FakeWin a = {150, 150, 100, 100, false, 0, 0}, b = {1500, 700, 100, 100, false, 0, 0};
        FakeWin wall = {760, 370, 400, 300, false, 0, 0};
        a.id = 1;
        b.id = 2;
        wall.id = 3;
        a.scripted = b.scripted = true;
        FakeWin *world[3] = {&a, &b, &wall};
        world_set(world, 3);
        /* Anchored at a line start: the header comment also mentions "COLLIDE = true". */
        const char *find = collide ? NULL : "\nCOLLIDE = true", *repl = collide ? NULL : "\nCOLLIDE = false";
        Script *sa = load_preset(&a, "bounce.lua", find, repl, err);
        CHECK(sa, "collide %d a: %ls", collide, err);
        Script *sb = load_preset(&b, "bounce.lua", find, repl, err);
        CHECK(sb, "collide %d b: %ls", collide, err);
        int overlaps = 0, near_wall = 0;
        for (int i = 0; sa && sb && i < 100000; ++i) {
            if (!script_update(sa, 1.0 / 60, err, 512) || !script_update(sb, 1.0 / 60, err, 512)) {
                CHECK(false, "collide %d update: %ls", collide, err);
                break;
            }
            if (!inside(&a, &work) || !inside(&b, &work)) {
                CHECK(false, "collide %d left the work area at %d", collide, i);
                break;
            }
            if (overlapping(&a, &b) || overlapping(&a, &wall) || overlapping(&b, &wall)) ++overlaps;
            if (near_to(&a, &wall, 3) || near_to(&b, &wall, 3)) ++near_wall;
        }
        if (collide) {
            CHECK(overlaps == 0, "COLLIDE: GIFs overlapped on %d ticks", overlaps);
            CHECK(near_wall > 0, "COLLIDE: never reached the obstacle");
        } else {
            CHECK(overlaps > 0, "COLLIDE = false: never passed through anything");
        }
        script_destroy(sa);
        script_destroy(sb);
    }

    /* Spawned on top of each other: they push apart and stay apart. */
    FakeWin a = {600, 400, 100, 100, false, 0, 0}, b = {600, 400, 100, 100, false, 0, 0};
    a.id = 1;
    b.id = 2;
    a.scripted = b.scripted = true;
    FakeWin *world[2] = {&a, &b};
    world_set(world, 2);
    Script *sa = load_preset(&a, "bounce.lua", NULL, NULL, err);
    Script *sb = load_preset(&b, "bounce.lua", NULL, NULL, err);
    if (sa && sb) {
        int separated_at = -1, overlaps_after = 0;
        for (int i = 0; i < 20000; ++i) {
            if (!script_update(sa, 1.0 / 60, err, 512) || !script_update(sb, 1.0 / 60, err, 512)) {
                CHECK(false, "stacked update: %ls", err);
                break;
            }
            bool ov = overlapping(&a, &b);
            if (!ov && separated_at < 0) separated_at = i;
            if (ov && separated_at >= 0) ++overlaps_after;
        }
        CHECK(separated_at >= 0 && separated_at < 120, "stacked GIFs separated at tick %d", separated_at);
        CHECK(overlaps_after == 0, "stacked GIFs overlapped again on %d ticks", overlaps_after);
    } else {
        CHECK(false, "stacked load: %ls", err);
    }
    script_destroy(sa);
    script_destroy(sb);
    world_set(NULL, 0);
}

/* ---- hitbox (visible area) --------------------------------------------------- */

static bool eq4(const int a[4], int b0, int b1, int b2, int b3)
{
    return a[0] == b0 && a[1] == b1 && a[2] == b2 && a[3] == b3;
}

static void test_hitbox_mapping(void)
{
    int out[4];
    const int centred[4] = {80, 80, 120, 120};
    hitbox_in_window(centred, 200, 200, 200, 200, false, out);
    CHECK(eq4(out, 80, 80, 40, 40), "hitbox 1:1: %d %d %d %d", out[0], out[1], out[2], out[3]);
    hitbox_in_window(centred, 200, 200, 50, 50, false, out); /* 25% */
    CHECK(eq4(out, 20, 20, 10, 10), "hitbox 25%%: %d %d %d %d", out[0], out[1], out[2], out[3]);
    const int odd[4] = {1, 1, 2, 2};
    hitbox_in_window(odd, 3, 3, 2, 2, false, out); /* Edges round outwards. */
    CHECK(eq4(out, 0, 0, 2, 2), "hitbox rounding: %d %d %d %d", out[0], out[1], out[2], out[3]);
    hitbox_in_window(centred, 200, 200, 400, 400, false, out); /* 200% */
    CHECK(eq4(out, 160, 160, 80, 80), "hitbox 200%%: %d %d %d %d", out[0], out[1], out[2], out[3]);
    const int left[4] = {10, 0, 30, 10};
    hitbox_in_window(left, 100, 10, 100, 10, true, out);
    CHECK(eq4(out, 70, 0, 20, 10), "hitbox flip: %d %d %d %d", out[0], out[1], out[2], out[3]);
    const int empty[4] = {0, 0, 0, 0};
    hitbox_in_window(empty, 100, 100, 37, 21, false, out);
    CHECK(eq4(out, 0, 0, 37, 21), "hitbox empty -> window: %d %d %d %d", out[0], out[1], out[2], out[3]);
    hitbox_in_window(NULL, 100, 100, 37, 21, true, out);
    CHECK(eq4(out, 0, 0, 37, 21), "hitbox NULL -> window");
}

/* 200x200 window whose visible area is the centred 40x40. */
static FakeWin sprite(double x, double y, int id)
{
    FakeWin f;
    memset(&f, 0, sizeof(f));
    f.x = x;
    f.y = y;
    f.w = f.h = 200;
    f.id = id;
    f.scripted = true;
    f.box[0] = f.box[1] = 80;
    f.box[2] = f.box[3] = 120;
    return f;
}

static RECT hit_rect(const FakeWin *f)
{
    int hb[4];
    fake_hitbox(f, hb);
    LONG l = (LONG)lround(f->x) + hb[0], t = (LONG)lround(f->y) + hb[1];
    RECT r = {l, t, l + hb[2], t + hb[3]};
    return r;
}

static bool hit_inside(const FakeWin *f, const RECT *work)
{
    RECT r = hit_rect(f);
    return r.left >= work->left && r.top >= work->top && r.right <= work->right && r.bottom <= work->bottom;
}

static void test_hitbox_api(void)
{
    wchar_t err[512];
    FakeWin me = sprite(100, 100, 1);
    FakeWin other = {240, 240, 100, 100, false, 0, 0}; /* No box: the whole window. */
    other.id = 2;
    other.name = "plain.gif";
    FakeWin *world[2] = {&me, &other};
    world_set(world, 2);
    set_mouse(&me, 110, 110); /* On the window, off the visible area. */

    Script *s = load(&me,
        "local dx, dy, w, h = yagp.hitbox() assert(dx == 80 and dy == 80 and w == 40 and h == 40, 'hitbox')\n"
        "local cx, cy = yagp.center() assert(cx == 200 and cy == 200, 'center ' .. cx)\n"
        "assert(yagp.fits(-80, -80), 'hitbox at 0,0 fits')\n"
        "assert(not yagp.fits(-81, 0), 'hitbox at -1 does not')\n"
        "assert(not yagp.fits(-80, -80, 200, 200), 'explicit size = raw rect')\n"
        "assert(yagp.fits(1920 - 120, 0), 'flush with the right edge')\n"
        "assert(#yagp.overlaps() == 0, 'windows overlap, hitboxes do not')\n"
        "assert(#yagp.overlaps(140, 140) == 1, 'hitbox moved onto the other GIF')\n"
        "assert(#yagp.overlaps(100, 100, 200, 200) == 1, 'explicit raw rect')\n"
        "local p = yagp.peer(2) assert(p.hx == 240 and p.hw == 100 and p.cx == 290 and p.w == 100)\n"
        "assert(not yagp.mouse().over, 'over transparent part')\n"
        "yagp.set_hitbox(0, 0, 200, 200) local a, b, c, d = yagp.hitbox()\n"
        "assert(a == 0 and b == 0 and c == 200 and d == 200, 'override')\n"
        "assert(yagp.mouse().over, 'over with override')\n"
        "yagp.set_hitbox(nil) assert(select(3, yagp.hitbox()) == 40, 'override cleared')\n", err);
    CHECK(s, "hitbox api: %ls", err);
    script_destroy(s);

    /* The other GIF sees me through my hitbox. */
    s = load(&other,
        "local p = yagp.peer(1)\n"
        "assert(p.x == 100 and p.w == 200 and p.hx == 180 and p.hy == 180 and p.hw == 40, 'peer hitbox')\n"
        "assert(p.cx == 200 and p.cy == 200, 'peer centre')\n", err);
    CHECK(s, "peer hitbox: %ls", err);
    script_destroy(s);

    s = load(&me, "yagp.set_hitbox(-1, 0, 10, 10)", err);
    CHECK(!s, "negative hitbox accepted");
    script_destroy(s);
    world_set(NULL, 0);
}

/* Presets keep the visible area, not the window, on screen: the transparent
 * margin may hang over the edge. */
static void test_hitbox_presets(void)
{
    wchar_t err[512];
    const RECT work = monitors_get(0)->work;
    static const char *const names[] = {"bounce.lua", "patrol.lua", "wander.lua"};
    for (int k = 0; k < 3; ++k) {
        FakeWin f = sprite(800, 400, 1);
        FakeWin *world[1] = {&f};
        world_set(world, 1);
        Script *s = load_preset(&f, names[k], NULL, NULL, err);
        CHECK(s, "%s: %ls", names[k], err);
        bool hung_over = false, touched = false;
        for (int i = 0; s && i < 100000; ++i) {
            if (!script_update(s, 1.0 / 60, err, 512)) { CHECK(false, "%s update: %ls", names[k], err); break; }
            if (!hit_inside(&f, &work)) {
                RECT r = hit_rect(&f);
                CHECK(false, "%s: visible area left the work area at %d: %ld %ld %ld %ld", names[k], i,
                      r.left, r.top, r.right, r.bottom);
                break;
            }
            RECT r = hit_rect(&f);
            if (f.x < work.left || f.y < work.top || f.x + f.w > work.right || f.y + f.h > work.bottom) hung_over = true;
            if (r.left <= work.left + 2 || r.right >= work.right - 2) touched = true;
        }
        /* Wander only reaches an edge when a random target happens to land there. */
        if (k < 2) CHECK(touched, "%s: visible area never reached a side edge", names[k]);
        CHECK(hung_over, "%s: window never hung over the edge (still clamping the window?)", names[k]);
        script_destroy(s);
    }

    /* Two such GIFs bouncing: windows overlap, visible areas never do. */
    FakeWin a = sprite(150, 150, 1), b = sprite(900, 500, 2);
    FakeWin *world[2] = {&a, &b};
    world_set(world, 2);
    Script *sa = load_preset(&a, "bounce.lua", NULL, NULL, err);
    Script *sb = load_preset(&b, "bounce.lua", NULL, NULL, err);
    int window_overlaps = 0, hit_overlaps = 0;
    for (int i = 0; sa && sb && i < 100000; ++i) {
        if (!script_update(sa, 1.0 / 60, err, 512) || !script_update(sb, 1.0 / 60, err, 512)) {
            CHECK(false, "hitbox bounce: %ls", err);
            break;
        }
        RECT ra = hit_rect(&a), rb = hit_rect(&b);
        if (contacts_overlap(ra.left, ra.top, ra.right, ra.bottom, rb.left, rb.top, rb.right, rb.bottom)) ++hit_overlaps;
        if (overlapping(&a, &b)) ++window_overlaps;
    }
    CHECK(sa && sb, "hitbox bounce load: %ls", err);
    CHECK(hit_overlaps == 0, "visible areas overlapped on %d ticks", hit_overlaps);
    CHECK(window_overlaps > 0, "windows never overlapped (collisions still use the window?)");
    script_destroy(sa);
    script_destroy(sb);
    world_set(NULL, 0);
}

/* ---- robustness: nothing a script does may abort the host ------------------- */

static double seconds_since(LARGE_INTEGER t0)
{
    LARGE_INTEGER t1, f;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    return (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
}

static void test_robustness(void)
{
    wchar_t err[512];
    FakeWin w = {100, 100, 50, 50, false, 0, 0};
    ScriptPeer p = {.id = 2, .w = 1, .h = 1, .hw = 1, .hh = 1, .name = "peer.gif"};

    /* Strict mode (_G.__index raises): missing handlers are skipped. This used to
     * raise outside any protected call, which aborts the process. */
    Script *s = load(&w, "setmetatable(_G, {__index = function(_, k) error('undefined global ' .. tostring(k), 2) end})\n", err);
    CHECK(s, "strict load: %ls", err);
    if (s) {
        CHECK(!script_has_update(s), "strict: has_update without update");
        CHECK(script_update(s, 0.016, err, 512), "strict update: %ls", err);
        CHECK(script_event(s, "on_moved", err, 512), "strict on_moved: %ls", err);
        CHECK(script_event(s, "on_screens_changed", err, 512), "strict on_screens_changed: %ls", err);
        CHECK(script_event_peer(s, "on_collide", &p, err, 512), "strict on_collide: %ls", err);
        script_destroy(s);
    }
    s = load(&w, "setmetatable(_G, {__index = function(_, k) error('undefined global ' .. k, 2) end})\n"
                 "function update(dt) rawset(_G, 'ticks', (rawget(_G, 'ticks') or 0) + 1) end\n"
                 "function on_collide(p) assert(p.id == 2) rawset(_G, 'hit', true) end\n"
                 "function check() assert(ticks == 3 and hit) end\n", err);
    CHECK(s, "strict with handlers: %ls", err);
    if (s) {
        CHECK(script_has_update(s), "strict: has_update with update");
        for (int i = 0; i < 3; ++i) CHECK(script_update(s, 0.016, err, 512), "strict update %d: %ls", i, err);
        CHECK(script_event_peer(s, "on_collide", &p, err, 512), "strict on_collide: %ls", err);
        CHECK(script_event(s, "check", err, 512), "strict handlers did not run: %ls", err);
        script_destroy(s);
    }

    /* The cached has_update follows a script defining and removing update. */
    s = load(&w, "function on_moved() update = function() end end\n"
                 "function on_screens_changed() update = nil end\n", err);
    CHECK(s, "has_update script: %ls", err);
    if (s) {
        CHECK(!script_has_update(s), "has_update before");
        CHECK(script_event(s, "on_moved", err, 512) && script_has_update(s), "has_update after define");
        CHECK(script_event(s, "on_screens_changed", err, 512) && !script_has_update(s), "has_update after nil");
        script_destroy(s);
    }

    /* At the memory cap every entry point fails or succeeds, but returns: even
     * creating the name string of a handler it never mentioned can fail. */
    s = load(&w, "function update() local t = {} for i = 1, 100 do t[i] = {i} end end\n"
                 "hog = {}\n"
                 "pcall(function() local i = 0 while true do i = i + 1 hog[i] = {i} end end)\n", err);
    CHECK(s, "memory hog load: %ls", err);
    if (s) {
        int failed = 0;
        for (int i = 0; i < 50; ++i) {
            char name[48];
            snprintf(name, sizeof(name), "on_never_interned_%d_xxxxxxxxxxxxxxxxxxxxxxxx", i);
            failed += !script_update(s, 0.016, err, 512);
            failed += !script_event(s, name, err, 512);
            failed += !script_event_peer(s, name, &p, err, 512);
        }
        printf("     memory cap: %d of 150 calls reported an error, none aborted\n", failed);
        script_destroy(s);
    }

    /* Non-string error values. */
    s = load(&w, "function update() error(42) end\nfunction on_moved() error({}) end\n", err);
    if (s) {
        CHECK(!script_update(s, 0.016, err, 512) && wcscmp(err, L"42") == 0, "number error: %ls", err);
        CHECK(!script_event(s, "on_moved", err, 512) && err[0], "table error empty");
        script_destroy(s);
    }

    /* Long non-ASCII messages are cut at a character boundary, not garbled. */
    s = load(&w, "function update() error(string.rep('\\u{4E2D}', 600), 0) end\n", err);
    if (s) {
        bool all = !script_update(s, 0.016, err, 512) && wcslen(err) == 511;
        for (size_t i = 0; all && i < 511; ++i) all = err[i] == 0x4E2D;
        CHECK(all, "long message: len %zu, first %04X", wcslen(err), (unsigned)err[0]);
        script_destroy(s);
    }

    /* Backtracking patterns run in C, where the instruction budget never fires:
     * the matcher's own step limit must stop them quickly. */
    static const char *const slow[] = {
        "return string.find(string.rep('a', 100000), '.-.-.-.-b')",
        "return string.find(string.rep('(', 30000), '%b()')",
        "return string.find(string.rep('a', 200000), '(.*)%1b')",
        "for _ in string.gmatch(string.rep('a', 5000), '.-b') do end",
        "return (string.gsub(string.rep('a', 20000), 'a.-b', ''))",
    };
    for (size_t i = 0; i < sizeof(slow) / sizeof(slow[0]); ++i) {
        char src[256];
        snprintf(src, sizeof(src), "function update() %s end", slow[i]);
        s = load(&w, src, err);
        if (!s) { CHECK(false, "pattern load %zu: %ls", i, err); continue; }
        LARGE_INTEGER t0;
        QueryPerformanceCounter(&t0);
        bool ok = script_update(s, 0.016, err, 512);
        double dt = seconds_since(t0);
        CHECK(!ok && wcsstr(err, L"pattern too complex"),
              "pattern %zu not stopped: %ls", i, ok ? L"(succeeded)" : err);
        CHECK(dt < 2.0, "pattern %zu took %.2f s", i, dt);
        printf("     pattern %zu stopped after %.0f ms\n", i, dt * 1000);
        script_destroy(s);
    }
    /* ... while ordinary heavy string work still passes. */
    s = load(&w, "local s = string.rep('ab ', 300000)\n"
                 "local r, n = s:gsub('%w+', 'x') assert(n == 300000)\n"
                 "local c = 0 for _ in s:gmatch('%a+') do c = c + 1 end assert(c == 300000)\n"
                 "assert(s:find('ab $') == #s - 2)\n"
                 "assert(('key = value'):match('^(%w+)%s*=%s*(%w+)$') == 'key')\n"
                 "assert(('[[x]]'):find('%b[]') == 1)\n", err);
    CHECK(s, "ordinary string work rejected: %ls", err);
    script_destroy(s);

    /* A budget overrun inside a coroutine leaves later calls their full budget
     * (about 6M instructions here, under the 10M limit). */
    s = load(&w, "co = coroutine.create(function() while true do pcall(function() while true do end end) coroutine.yield() end end)\n"
                 "function update() if co then coroutine.resume(co) co = nil\n"
                 "  else local x = 0 for i = 1, 3000000 do x = x + i end end end\n", err);
    CHECK(s, "coroutine budget load: %ls", err);
    if (s) {
        script_update(s, 0.016, err, 512); /* Times out inside the coroutine. */
        CHECK(script_update(s, 0.016, err, 512), "budget after a coroutine timeout: %ls", err);
        script_destroy(s);
    }

    /* Finalizers run with hooks disabled, so an endless __gc would hang every
     * collection (and lua_close): they are refused. Ordinary metatables work. */
    s = load(&w, "setmetatable({}, {__gc = function() while true do end end})\n", err);
    CHECK(!s && wcsstr(err, L"__gc"), "__gc metatable accepted: %ls", s ? L"(loaded)" : err);
    script_destroy(s);
    s = load(&w, "local mt = {__index = function() return 1 end}\n"
                 "local t = setmetatable({}, mt) assert(getmetatable(t) == mt and t.x == 1)\n"
                 "mt.__gc = function() while true do end end -- added later: never called by Lua\n"
                 "t = nil\n", err);
    CHECK(s, "plain setmetatable: %ls", err);
    script_destroy(s);
}

/* ---- yagp.on / off, routed input, timers, ext ops -------------------------- */

typedef struct FakeExt {
    unsigned subs;
    int subs_calls;
    double now;
    char loaded[8][32];
    int loaded_n;
    char playing[32];
    ScriptPlay last_play;
    int seek_to;
    double matrix[6];
    bool matrix_reset;
    double matrix_dur;
    int cursor, draggable, clickthrough;
    double opacity;
    char notify_title[128];
    char notify_text[1024];
    int notify_calls;
    bool notify_ok;
} FakeExt;
static FakeExt s_ext;

static bool x_load_clip(void *ctx, const char *name, const char *rel, char *err, size_t cap)
{
    (void)ctx;
    if (strstr(rel, "missing")) {
        snprintf(err, cap, "no such file: %s", rel);
        return false;
    }
    strncpy_s(s_ext.loaded[s_ext.loaded_n++ % 8], 32, name, _TRUNCATE);
    return true;
}
static void x_unload_clip(void *ctx, const char *name) { (void)ctx; (void)name; }
static const uint32_t s_delays[3] = {100, 200, 300};
static bool x_clip_info(void *ctx, const char *name, ScriptClipInfo *out)
{
    (void)ctx;
    if (strcmp(name, "nope") == 0) return false;
    out->w = 40;
    out->h = 20;
    out->frames = 3;
    out->duration = 0.6;
    out->delays = s_delays;
    return true;
}
static bool x_play(void *ctx, const char *name, const ScriptPlay *p, char *err, size_t cap)
{
    (void)ctx;
    if (strcmp(name, "nope") == 0) {
        snprintf(err, cap, "no clip named '%s'", name);
        return false;
    }
    strncpy_s(s_ext.playing, 32, name, _TRUNCATE);
    s_ext.last_play = *p;
    return true;
}
static bool x_seek(void *ctx, int frame) { (void)ctx; s_ext.seek_to = frame; return frame < 3; }
static void x_get_frame(void *ctx, const char **name, int *frame, int *frames)
{
    (void)ctx;
    *name = s_ext.playing[0] ? s_ext.playing : "main";
    *frame = 1;
    *frames = 3;
}
static void x_set_speed(void *ctx, double v) { (void)ctx; (void)v; }
static void x_set_playing(void *ctx, bool v) { (void)ctx; (void)v; }
static bool x_set_matrix(void *ctx, const double *m, double d, int e)
{
    (void)ctx; (void)e;
    s_ext.matrix_reset = m == NULL;
    if (m) memcpy(s_ext.matrix, m, sizeof(s_ext.matrix));
    s_ext.matrix_dur = d;
    return true;
}
static void x_set_opacity(void *ctx, double a, double d, int e) { (void)ctx; (void)d; (void)e; s_ext.opacity = a; }
static void x_set_cursor(void *ctx, int c) { (void)ctx; s_ext.cursor = c; }
static void x_set_draggable(void *ctx, bool v) { (void)ctx; s_ext.draggable = v; }
static void x_set_clickthrough(void *ctx, bool v) { (void)ctx; s_ext.clickthrough = v; }
static void x_subs_changed(void *ctx, unsigned m) { (void)ctx; s_ext.subs = m; s_ext.subs_calls++; }
static double x_now(void *ctx) { (void)ctx; return s_ext.now; }
static bool x_notify(void *ctx, const char *title, const char *text)
{
    (void)ctx;
    ++s_ext.notify_calls;
    strncpy_s(s_ext.notify_title, sizeof(s_ext.notify_title), title ? title : "", _TRUNCATE);
    strncpy_s(s_ext.notify_text, sizeof(s_ext.notify_text), text ? text : "", _TRUNCATE);
    return s_ext.notify_ok;
}

static const ScriptHostExtOps EXT = {x_load_clip, x_unload_clip, x_clip_info, x_play, x_seek, x_get_frame,
                                     x_set_speed, x_set_playing, x_set_matrix, x_set_opacity, x_set_cursor,
                                     x_set_draggable, x_set_clickthrough, x_subs_changed, x_now, x_notify,
                                     NULL, NULL, NULL, NULL};

/* A very small fake master/slave world for the chainable handle tests. */
static FakeWin *s_slave;

static int slave_list(void *ctx, int *ids, int cap)
{
    (void)ctx;
    if (!s_slave || cap < 1) return 0;
    ids[0] = s_slave->id;
    return 1;
}
static void *slave_resolve(void *ctx, int id)
{
    (void)ctx;
    return s_slave && s_slave->id == id ? s_slave : NULL;
}
static const char *slave_name(void *ctx, int id)
{
    (void)ctx;
    return s_slave && s_slave->id == id ? "slave" : NULL;
}
static bool slave_hidden(void *target) { return ((FakeWin *)target)->hidden; }
static bool slave_paused(void *target) { return ((FakeWin *)target)->paused; }
static bool slave_static(void *target) { return ((FakeWin *)target)->still; }

static const ScriptSlaveOps SLAVE_OPS = {slave_list, slave_resolve, slave_name,
                                         slave_hidden, slave_paused, slave_static};

static const ScriptHostExtOps EXT_SLAVES = {
    x_load_clip, x_unload_clip, x_clip_info, x_play, x_seek, x_get_frame,
    x_set_speed, x_set_playing, x_set_matrix, x_set_opacity, x_set_cursor,
    x_set_draggable, x_set_clickthrough, x_subs_changed, x_now, x_notify,
    NULL, NULL, NULL, &SLAVE_OPS};

static Script *load_ext_slave(FakeWin *w, const char *src, wchar_t *err)
{
    return script_create_ex(&OPS, &EXT_SLAVES, w, src, strlen(src), "test.lua", "test.gif", w->id, err, 512);
}

static Script *load_ext(FakeWin *w, const char *src, wchar_t *err)
{
    return script_create_ex(&OPS, &EXT, w, src, strlen(src), "test.lua", "test.gif", w->id, err, 512);
}

static ScriptInput input_at(int button, int x, int y)
{
    ScriptInput in;
    memset(&in, 0, sizeof(in));
    in.button = button;
    in.x = x;
    in.y = y;
    in.gx = x - 100;
    in.gy = y - 100;
    in.opaque = true;
    return in;
}

static bool run_check(Script *s, const char *fn)
{
    wchar_t err[512];
    bool ok = script_event(s, fn, err, 512);
    if (!ok) printf("     %s: %ls\n", fn, err);
    return ok;
}

static void test_events(void)
{
    wchar_t err[512];
    FakeWin w = {100, 100, 50, 50, false, 0, 0};
    memset(&s_ext, 0, sizeof(s_ext));
    bool handled;

    /* Subscription mask: yagp.on handlers plus legacy globals, reported to the host. */
    Script *s = load_ext(&w,
        "h = yagp.on('click', function() end)\n"
        "function on_moved() end\n"
        "function drop() yagp.off(h) yagp.off(h) yagp.off(12345) on_moved = nil end\n", err);
    CHECK(s, "subs load: %ls", err);
    if (s) {
        unsigned m = script_subscriptions(s);
        CHECK(m == (SEV_BIT(SEV_CLICK) | SEV_BIT(SEV_MOVED)), "subs mask %x", m);
        CHECK(s_ext.subs == m, "subs_changed: %x vs %x", s_ext.subs, m);
        CHECK(run_check(s, "drop") && script_subscriptions(s) == 0 && s_ext.subs == 0,
              "mask after off: %x", script_subscriptions(s));
        script_destroy(s);
    }

    /* Routing: returning true or setting e.handled stops later handlers and is
     * reported; a passthrough event runs them all and is never handled. The
     * event table carries the input. */
    s = load_ext(&w,
        "order = ''\n"
        "yagp.on('click', function(e) order = order .. 'a'\n"
        "  assert(e.type == 'click' and e.button == 'left' and e.x == 120 and e.gx == 20 and e.opaque)\n"
        "  if e.shift then e.handled = true end end)\n"
        "yagp.on('click', function(e) order = order .. 'b' assert(not e.handled) return e.ctrl end)\n"
        "yagp.on('click', function(e) order = order .. 'c' end)\n"
        "function check() assert(order == 'abcaababc', order) end\n", err);
    CHECK(s, "routing load: %ls", err);
    if (s) {
        ScriptInput in = input_at(SBTN_LEFT, 120, 130);
        bool h[4];
        script_input(s, SEV_CLICK, &in, &h[0], err, 512);
        in.shift = true;
        script_input(s, SEV_CLICK, &in, &h[1], err, 512);
        in.shift = false;
        in.ctrl = true;
        script_input(s, SEV_CLICK, &in, &h[2], err, 512);
        in.passthrough = true;
        script_input(s, SEV_CLICK, &in, &h[3], err, 512);
        CHECK(!h[0] && h[1] && h[2] && !h[3], "handled flags %d %d %d %d", h[0], h[1], h[2], h[3]);
        CHECK(run_check(s, "check"), "routing order");
        /* Not subscribed: nothing runs, not handled. */
        CHECK(script_input(s, SEV_WHEEL, &in, &handled, err, 512) && !handled, "wheel: %ls", err);
        script_destroy(s);
    }

    /* Subscribing and unsubscribing from inside a handler. */
    s = load_ext(&w,
        "n, m = 0, 0\n"
        "local h\n"
        "h = yagp.on('click', function() n = n + 1 yagp.off(h) yagp.on('click', function() m = m + 1 end) end)\n"
        "function check() assert(n == 1 and m == 2, n .. ' ' .. m) end\n", err);
    CHECK(s, "nested load: %ls", err);
    if (s) {
        ScriptInput in = input_at(SBTN_LEFT, 0, 0);
        for (int i = 0; i < 3; ++i) CHECK(script_input(s, SEV_CLICK, &in, NULL, err, 512), "nested: %ls", err);
        CHECK(run_check(s, "check"), "nested on/off");
        script_destroy(s);
    }

    /* A handler error fails the call with the chunk name and line. */
    s = load_ext(&w, "yagp.on('click', function()\n  error('boom')\nend)\n", err);
    if (s) {
        ScriptInput in = input_at(SBTN_LEFT, 0, 0);
        CHECK(!script_input(s, SEV_CLICK, &in, NULL, err, 512) && wcsstr(err, L"test.lua:2:"),
              "handler error: %ls", err);
        script_destroy(s);
    }

    /* Event tables are reused: no allocation per input event. */
    s = load_ext(&w,
        "last = nil same = true\n"
        "yagp.on('mouse_move', function(e) if last and last ~= e then same = false end last = e end)\n"
        "function check() assert(same) end\n", err);
    if (s) {
        ScriptInput in = input_at(SBTN_NONE, 0, 0);
        for (int i = 0; i < 100; ++i) script_input(s, SEV_MOUSE_MOVE, &in, NULL, err, 512);
        CHECK(run_check(s, "check"), "event table reused");
        script_destroy(s);
    }

    /* cursor: min_move, radius (near / far transitions from the hitbox centre)
     * and rate filters, per handler. */
    s_ext.now = 0;
    s = load_ext(&w,
        "all, moved, edges, slow = 0, 0, {}, 0\n"
        "yagp.on('cursor', function(e) all = all + 1 assert(e.valid) end)\n"
        "yagp.on('cursor', function() moved = moved + 1 end, {min_move = 50})\n"
        "yagp.on('cursor', function(e) edges[#edges + 1] = e.near end, {radius = 100})\n"
        "yagp.on('cursor', function() slow = slow + 1 end, {rate = 2})\n"
        "function check() assert(all == 6, 'all ' .. all) assert(moved == 4, 'moved ' .. moved)\n"
        "  assert(#edges == 2 and edges[1] == true and edges[2] == false, 'edges ' .. #edges)\n"
        "  assert(slow == 2, 'slow ' .. slow) end\n", err);
    CHECK(s, "cursor load: %ls", err);
    if (s) {
        CHECK(script_subscriptions(s) & SEV_BIT(SEV_CURSOR), "cursor bit");
        /* Window 100,100 50x50: centre 125,125. */
        static const int path[6][2] = {{1000, 1000}, {1010, 1000}, {1100, 1000}, {150, 150}, {160, 150}, {600, 600}};
        for (int i = 0; i < 6; ++i) {
            s_ext.now = i * 0.2;
            CHECK(script_cursor(s, path[i][0], path[i][1], true, err, 512), "cursor %d: %ls", i, err);
        }
        CHECK(run_check(s, "check"), "cursor filters");
        script_destroy(s);
    }

    /* Peer events: id and radius filters; legacy on_collide still runs. */
    s = load_ext(&w,
        "a, b, c, legacy = 0, 0, 0, 0\n"
        "yagp.on('peer_moved', function(p) a = a + 1 end)\n"
        "yagp.on('peer_moved', function(p) assert(p.id == 7) b = b + 1 end, {id = 7})\n"
        "yagp.on('peer_moved', function(p) c = c + 1 end, {radius = 100})\n"
        "function on_collide(p) legacy = legacy + 1 end\n"
        "yagp.on('collide', function(p) legacy = legacy + 10 end)\n"
        "function check() assert(a == 2 and b == 1 and c == 1 and legacy == 11, a .. b .. c .. legacy) end\n", err);
    CHECK(s, "peer load: %ls", err);
    if (s) {
        ScriptPeer pnear = {.id = 7, .x = 150, .y = 100, .w = 20, .h = 20, .hx = 150, .hy = 100, .hw = 20, .hh = 20, .name = ""};
        ScriptPeer pfar = {.id = 8, .x = 900, .y = 900, .w = 20, .h = 20, .hx = 900, .hy = 900, .hw = 20, .hh = 20, .name = ""};
        CHECK(script_peer_event(s, SEV_PEER_MOVED, &pnear, err, 512), "peer 1: %ls", err);
        CHECK(script_peer_event(s, SEV_PEER_MOVED, &pfar, err, 512), "peer 2: %ls", err);
        CHECK(script_peer_event(s, SEV_COLLIDE, &pnear, err, 512), "collide: %ls", err);
        CHECK(run_check(s, "check"), "peer filters");
        script_destroy(s);
    }

    /* Frame and clip_end: filtered by clip name and frame set (1-based in Lua). */
    s = load_ext(&w,
        "hits, ends = {}, {}\n"
        "yagp.on('frame', function(f, clip) hits[#hits + 1] = clip .. f end, {frames = {2, 3}, clip = 'walk'})\n"
        "yagp.on('clip_end', function(clip) ends[#ends + 1] = clip end)\n"
        "function check() assert(table.concat(hits, ',') == 'walk2,walk3', table.concat(hits, ','))\n"
        "  assert(#ends == 1 and ends[1] == 'sit') end\n", err);
    CHECK(s, "frame load: %ls", err);
    if (s) {
        for (int f = 0; f < 4; ++f) script_clip_event(s, SEV_FRAME, "walk", f, err, 512);
        script_clip_event(s, SEV_FRAME, "sit", 1, err, 512);
        script_clip_event(s, SEV_CLIP_END, "sit", 0, err, 512);
        CHECK(run_check(s, "check"), "frame filters");
        script_destroy(s);
    }

    /* Timers: due order, a 0-delay timer added by a callback waits for the
     * next run, repeating timers never burst, cancel. */
    s_ext.now = 10;
    s = load_ext(&w,
        "log = ''\n"
        "yagp.after(0.5, function() log = log .. 'b' end)\n"
        "yagp.after(0.2, function() log = log .. 'a' end)\n"
        "rep = yagp.every(1, function() log = log .. 'r' end)\n"
        "local x = yagp.after(0.3, function() log = log .. 'X' end) yagp.cancel(x)\n"
        "yagp.after(0.1, function() yagp.after(0, function() log = log .. 'z' end) end)\n"
        "function check1() assert(log == 'ab', log) end\n"
        "function check2() assert(log == 'abzrr', log) yagp.cancel(rep) end\n", err);
    CHECK(s, "timers load: %ls", err);
    if (s) {
        CHECK(fabs(script_next_timer(s) - 10.1) < 1e-9, "next timer %g", script_next_timer(s));
        s_ext.now = 10.6;
        CHECK(script_run_timers(s, 10.6, err, 512), "timers: %ls", err);
        CHECK(run_check(s, "check1"), "timer order");
        CHECK(script_run_timers(s, 10.6, err, 512), "timers 2: %ls", err);
        CHECK(script_run_timers(s, 15.0, err, 512), "timers 3: %ls", err);
        CHECK(script_run_timers(s, 15.5, err, 512), "timers 4: %ls", err);
        CHECK(script_run_timers(s, 16.0, err, 512), "timers 5: %ls", err);
        CHECK(run_check(s, "check2"), "timer repeat");
        CHECK(script_next_timer(s) < 0, "timers left: %g", script_next_timer(s));
        script_destroy(s);
    }
    /* A timer callback error is reported. */
    s_ext.now = 0;
    s = load_ext(&w, "yagp.after(0, function() error('tick') end)", err);
    if (s) {
        CHECK(!script_run_timers(s, 1, err, 512) && wcsstr(err, L"tick"), "timer error: %ls", err);
        script_destroy(s);
    }

    /* Clip / playback / composition / input wrappers reach the host. */
    memset(&s_ext, 0, sizeof(s_ext));
    s = load_ext(&w,
        "assert(yagp.clip('walk', 'walk.gif') == true)\n"
        "local ok, e = yagp.clip('gone', 'missing.gif') assert(ok == nil and e:find('missing'))\n"
        "local i = yagp.clip_info('walk') assert(i.w == 40 and i.frames == 3 and i.ready and i.delays[2] == 0.2)\n"
        "assert(yagp.clip_info('nope') == nil)\n"
        "assert(yagp.play('walk', {from = 2, to = 3, loop = 2, speed = 2, anchor = 'center', sync = true}))\n"
        "local ok2, e2 = yagp.play('nope') assert(not ok2 and e2:find('nope'))\n"
        "assert(yagp.seek(2) and not yagp.seek(9))\n"
        "local name, f, n = yagp.frame() assert(name == 'walk' and f == 2 and n == 3)\n"
        "yagp.set_transform{sx = 2, sy = 0.5}\n"
        "yagp.set_opacity(0.5, 0.2) yagp.set_cursor('hand') yagp.set_draggable(false) yagp.set_clickthrough(true)\n", err);
    CHECK(s, "ext api: %ls", err);
    if (s) {
        CHECK(strcmp(s_ext.playing, "walk") == 0 && s_ext.last_play.from == 1 && s_ext.last_play.to == 2 &&
              s_ext.last_play.loops == 2 && s_ext.last_play.speed == 2 && s_ext.last_play.anchor == SANCHOR_CENTER &&
              s_ext.last_play.sync && s_ext.last_play.restart, "play options");
        CHECK(s_ext.seek_to == 8, "seek %d", s_ext.seek_to);
        /* Scale about the bottom centre of a 50x50 window: x' = 2x - 25, y' = 0.5y + 25. */
        const double *m = s_ext.matrix;
        CHECK(fabs(m[0] - 2) < 1e-9 && fabs(m[3] - 0.5) < 1e-9 && fabs(m[1]) < 1e-9 && fabs(m[2]) < 1e-9 &&
              fabs(m[4] + 25) < 1e-9 && fabs(m[5] - 25) < 1e-9,
              "matrix %g %g %g %g %g %g", m[0], m[1], m[2], m[3], m[4], m[5]);
        CHECK(s_ext.opacity == 0.5 && s_ext.cursor == SCURSOR_HAND && !s_ext.draggable && s_ext.clickthrough,
              "input behaviour");
        script_destroy(s);
    }
    /* Without ext ops the functions exist and fail softly. */
    s = load(&w, "assert(yagp.clip('a', 'a.gif') == nil and yagp.play('main') == nil)\n"
                 "assert(yagp.frame() == 'main') yagp.set_opacity(1) yagp.set_cursor('arrow')\n", err);
    CHECK(s, "no ext: %ls", err);
    script_destroy(s);

    /* Bad arguments fail at the call. */
    static const char *const bad[] = {
        "yagp.on('clik', function() end)",
        "yagp.on('click', 42)",
        "yagp.on('cursor', function() end, {radius = -1})",
        "yagp.on('frame', function() end, {frames = {0}})",
        "for i = 1, 65 do yagp.on('click', function() end) end",
        "for i = 1, 65 do yagp.after(1, function() end) end",
        "yagp.play('main', {from = 3, to = 2})",
        "yagp.play('main', {loop = 0})",
        "yagp.play('main', {anchor = 'side'})",
        "yagp.set_matrix(1, 0, 0, 0, 0, 0)",
        "yagp.set_matrix(100, 0, 0, 1, 0, 0)",
        "yagp.set_transform{sx = 0}",
        "yagp.set_opacity(2)",
        "yagp.set_cursor('wait')",
        "yagp.clip('main', 'a.gif')",
        "yagp.clip(string.rep('x', 40), 'a.gif')",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        s = load_ext(&w, bad[i], err);
        CHECK(!s, "accepted: %s", bad[i]);
        script_destroy(s);
    }
}

static void test_clip_path(void)
{
    const wchar_t *base = L"C:\\pets\\cat\\main.gif";
    wchar_t out[512];
    CHECK(clip_resolve_path(base, "sit.gif", out, 512) && wcscmp(out, L"C:\\pets\\cat\\sit.gif") == 0, "plain: %ls", out);
    CHECK(clip_resolve_path(base, "dir/Look_3.GIF", out, 512) && wcscmp(out, L"C:\\pets\\cat\\dir\\Look_3.GIF") == 0,
          "subdir: %ls", out);
    CHECK(clip_resolve_path(base, "\xE5\x9D\x90.gif", out, 512), "utf-8 name");
    static const char *const bad[] = {
        "", "../x.gif", "a/../../x.gif", "..\\x.gif", "./x.gif", "C:\\x.gif", "C:x.gif", "/x.gif",
        "\\\\server\\share\\x.gif", "x.gif:stream", "x.gif.", "x.gif ", "a//x.gif", "a/", "x.png", "x",
        "con.gif", "sub/NUL.gif", "com1.gif", "a*.gif", "a?.gif", "\x01.gif", "\xFF.gif", ".gif",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        CHECK(!clip_resolve_path(base, bad[i], out, 512), "accepted path '%s' -> %ls", bad[i], out);
    }
    CHECK(!clip_resolve_path(L"nofolder.gif", "x.gif", out, 512), "base without folder");
    CHECK(!clip_resolve_path(base, "x.gif", out, 10), "tiny buffer");
}

/* ---- input state machine ---------------------------------------------------- */

static const InputMetrics FSM_M = {4, 4, 4, 4, 500, 400};

static bool out_is(const InputOut *o, int n, int e0, int e1, int e2)
{
    int want[3] = {e0, e1, e2};
    if (o->n != n) return false;
    for (int i = 0; i < n && i < 3; ++i) {
        if (o->ev[i] != want[i]) return false;
    }
    return true;
}

static void test_input_fsm(void)
{
    InputFsm f;
    InputOut o;
    ifsm_reset(&f);

    ifsm_move(&f, &FSM_M, 10, 10, true, 0, &o);
    CHECK(out_is(&o, 2, IFSM_ENTER, IFSM_MOVE, 0), "enter+move: n=%d", o.n);
    ifsm_idle(&f, &FSM_M, 300, &o);
    CHECK(o.n == 0, "hover too early");
    ifsm_idle(&f, &FSM_M, 400, &o);
    CHECK(out_is(&o, 1, IFSM_HOVER, 0, 0), "hover");
    ifsm_idle(&f, &FSM_M, 900, &o);
    CHECK(o.n == 0, "hover twice");

    /* Click, then a second one in time and place: double click. */
    ifsm_button_down(&f, &FSM_M, 1, 10, 10, 1000, &o);
    CHECK(out_is(&o, 1, IFSM_DOWN, 0, 0) && o.button == 1, "down");
    ifsm_move(&f, &FSM_M, 11, 10, true, 1010, &o); /* Inside the drag threshold. */
    CHECK(out_is(&o, 1, IFSM_MOVE, 0, 0), "small move while down");
    ifsm_button_up(&f, &FSM_M, 1, 11, 10, 1050, &o);
    CHECK(out_is(&o, 2, IFSM_UP, IFSM_CLICK, 0), "click");
    CHECK(ifsm_default_action(&f, &o, 1, false) == IFSM_ACT_NONE, "left click has no default");
    ifsm_button_down(&f, &FSM_M, 1, 11, 11, 1200, &o);
    ifsm_button_up(&f, &FSM_M, 1, 11, 11, 1250, &o);
    CHECK(out_is(&o, 3, IFSM_UP, IFSM_CLICK, IFSM_DOUBLE_CLICK), "double click n=%d", o.n);
    /* A third click starts over. */
    ifsm_button_down(&f, &FSM_M, 1, 11, 11, 1300, &o);
    ifsm_button_up(&f, &FSM_M, 1, 11, 11, 1350, &o);
    CHECK(out_is(&o, 2, IFSM_UP, IFSM_CLICK, 0), "third click");
    /* Too slow: no double. */
    ifsm_button_down(&f, &FSM_M, 1, 11, 11, 2400, &o);
    ifsm_button_up(&f, &FSM_M, 1, 11, 11, 2450, &o);
    CHECK(out_is(&o, 2, IFSM_UP, IFSM_CLICK, 0), "slow second click");

    /* Left drag: unhandled -> system drag; no click afterwards. */
    ifsm_button_down(&f, &FSM_M, 1, 10, 10, 5000, &o);
    ifsm_move(&f, &FSM_M, 20, 10, true, 5010, &o);
    CHECK(out_is(&o, 1, IFSM_DRAG_START, 0, 0) && o.dx == 10, "drag_start");
    CHECK(ifsm_default_action(&f, &o, 0, false) == IFSM_ACT_SYSTEM_DRAG, "system drag");
    CHECK(ifsm_default_action(&f, &o, 0, true) == IFSM_ACT_NONE, "handled drag");
    ifsm_cancel(&f); /* The system move loop took over the press. */
    ifsm_button_up(&f, &FSM_M, 1, 20, 10, 5100, &o);
    CHECK(o.n == 0, "up after system drag");

    /* Script-owned drag: drag with deltas, drag_end, no click. */
    ifsm_button_down(&f, &FSM_M, 1, 10, 10, 6000, &o);
    ifsm_move(&f, &FSM_M, 30, 10, true, 6010, &o);
    ifsm_take_drag(&f);
    ifsm_move(&f, &FSM_M, 33, 12, false, 6020, &o); /* Captured: outside is fine. */
    CHECK(out_is(&o, 1, IFSM_DRAG, 0, 0) && o.dx == 3 && o.dy == 2, "drag delta %d %d", o.dx, o.dy);
    ifsm_button_up(&f, &FSM_M, 1, 33, 12, 6100, &o);
    CHECK(out_is(&o, 2, IFSM_UP, IFSM_DRAG_END, 0), "drag_end");
    /* The cursor ended outside: the next move leaves. */
    ifsm_move(&f, &FSM_M, 300, 300, false, 6200, &o);
    CHECK(out_is(&o, 1, IFSM_LEAVE, 0, 0), "leave after drag");

    /* Right click: menu unless handled; a right drag has no default. */
    ifsm_button_down(&f, &FSM_M, 2, 10, 10, 7000, &o);
    CHECK(out_is(&o, 2, IFSM_ENTER, IFSM_DOWN, 0), "enter on press");
    ifsm_button_up(&f, &FSM_M, 2, 10, 10, 7050, &o);
    CHECK(ifsm_default_action(&f, &o, 1, false) == IFSM_ACT_MENU, "menu");
    CHECK(ifsm_default_action(&f, &o, 1, true) == IFSM_ACT_NONE, "menu handled");
    ifsm_button_down(&f, &FSM_M, 2, 10, 10, 8000, &o);
    ifsm_move(&f, &FSM_M, 40, 10, true, 8010, &o);
    CHECK(ifsm_default_action(&f, &o, 0, false) == IFSM_ACT_NONE, "right drag default");
    /* A second button during a press is ignored. */
    ifsm_button_down(&f, &FSM_M, 1, 40, 10, 8020, &o);
    CHECK(o.n == 0, "second button");
    ifsm_button_up(&f, &FSM_M, 1, 40, 10, 8030, &o);
    CHECK(o.n == 0, "other button up");
    ifsm_button_up(&f, &FSM_M, 2, 40, 10, 8040, &o);
    CHECK(out_is(&o, 1, IFSM_UP, 0, 0), "right drag ends without click");
    ifsm_leave(&f, &o);
    CHECK(out_is(&o, 1, IFSM_LEAVE, 0, 0), "explicit leave");
    ifsm_leave(&f, &o);
    CHECK(o.n == 0, "leave twice");
}

static void test_transform(void)
{
    Xform x = {{2, 0, 0, 0.5, -25, 25}}, inv;
    double px, py;
    xform_apply(&x, 25, 50, &px, &py);
    CHECK(fabs(px - 25) < 1e-9 && fabs(py - 50) < 1e-9, "pivot fixed: %g %g", px, py);
    CHECK(xform_invert(&x, &inv), "invert");
    xform_apply(&inv, px, py, &px, &py);
    CHECK(fabs(px - 25) < 1e-9 && fabs(py - 50) < 1e-9, "inverse: %g %g", px, py);
    int bb[4];
    xform_aabb(&x, 50, 50, bb);
    CHECK(bb[0] == -25 && bb[1] == 25 && bb[2] == 75 && bb[3] == 50, "aabb %d %d %d %d", bb[0], bb[1], bb[2], bb[3]);
    Xform rot = {{0, 1, -1, 0, 0, 0}}; /* 90 degrees */
    xform_aabb(&rot, 40, 20, bb);
    CHECK(bb[0] == -20 && bb[1] == 0 && bb[2] == 0 && bb[3] == 40, "rot aabb %d %d %d %d", bb[0], bb[1], bb[2], bb[3]);
    Xform flat = {{1, 0, 0, 0, 0, 0}};
    CHECK(!xform_invert(&flat, &inv), "degenerate invertible");
    for (int e = 0; e < 4; ++e) {
        CHECK(fabs(xform_ease(e, 0)) < 1e-12 && fabs(xform_ease(e, 1) - 1) < 1e-12, "ease %d ends", e);
        double prev = 0;
        for (int i = 1; i <= 100; ++i) {
            double v = xform_ease(e, i / 100.0);
            CHECK(v >= prev - 1e-12, "ease %d not monotonic", e);
            prev = v;
        }
    }
    XformAnim a;
    xform_anim_start(&a, &XFORM_IDENTITY, &x, 1.0, 2.0, SEASE_LINEAR);
    Xform mid;
    xform_anim_eval(&a, 2.0, &mid);
    CHECK(fabs(mid.m[0] - 1.5) < 1e-9 && fabs(mid.m[4] + 12.5) < 1e-9, "anim mid %g %g", mid.m[0], mid.m[4]);
    CHECK(!xform_anim_done(&a, 2.9) && xform_anim_done(&a, 3.0), "anim done");
    /* Rebasing mid-way continues along the same curve, for every easing. */
    for (int e = 0; e < 4; ++e) {
        XformAnim ref, rb;
        xform_anim_start(&ref, &XFORM_IDENTITY, &x, 1.0, 2.0, e);
        rb = ref;
        xform_anim_rebase(&rb, 1.6);
        double worst = 0;
        for (int i = 0; i <= 20; ++i) {
            double t = 1.6 + 1.4 * i / 20.0;
            Xform p1, p2;
            xform_anim_eval(&ref, t, &p1);
            xform_anim_eval(&rb, t, &p2);
            for (int k = 0; k < 6; ++k) worst = fmax(worst, fabs(p1.m[k] - p2.m[k]));
        }
        CHECK(worst < 1e-9 && fabs(rb.t0 + rb.duration - 3.0) < 1e-12, "rebase easing %d off by %g", e, worst);
        xform_anim_rebase(&rb, 2.3);
        Xform p1, p2;
        xform_anim_eval(&ref, 2.7, &p1);
        xform_anim_eval(&rb, 2.7, &p2);
        CHECK(fabs(p1.m[0] - p2.m[0]) < 1e-9, "double rebase easing %d", e);
    }
}

/* ---- yagp.log / print / yagp.notify --------------------------------------- */

static void test_lua_log(void)
{
    wchar_t err[512];
    FakeWin w = {100, 100, 50, 50, false, 0, 0};
    w.id = 4;
    memset(&s_ext, 0, sizeof(s_ext));
    s_ext.now = 1000.0;
    logs_reset();
    Script *s = load_ext(&w,
        "function run()\n"
        "  log.trace('t')\n"
        "  log.debug('d')\n"
        "  log.info('i', 42, true, nil, {})\n"
        "  log.warn('w')\n"
        "  log.error('e')\n"
        "  log.fatal('f')\n"
        "  print('printed', 7)\n"
        "  log.info(string.rep('x', 3000))\n"
        "  log.info(setmetatable({}, {__tostring = function() error('boom') end}))\n"
        "  log.info()\n"
        "end\n", err);
    CHECK(s, "log load: %ls", err);
    if (!s) return;
    CHECK(run_check(s, "run"), "log run");
    CHECK(logs_contain(LOG_TRACE, "t"), "log.trace");
    CHECK(logs_contain(LOG_DEBUG, "d"), "log.debug");
    CHECK(logs_contain(LOG_INFO, "i\t42\ttrue\tnil\ttable"), "log.info conversion");
    CHECK(logs_contain(LOG_WARN, "w"), "log.warn");
    CHECK(logs_contain(LOG_ERROR, "e"), "log.error");
    CHECK(logs_contain(LOG_FATAL, "f"), "log.fatal");
    CHECK(logs_contain(LOG_INFO, "printed\t7"), "print maps to INFO");
    CHECK(!logs_contain(LOG_INFO, "userdata:"), "print leaked the Lua buffer placeholder");
    bool capped = false;
    for (int i = 0; i < s_logs_n; ++i) {
        size_t n = strlen(s_logs[i].text);
        if (s_logs[i].level == LOG_INFO && n == 1023 && strcmp(s_logs[i].text + n - 3, "...") == 0)
            capped = true;
    }
    CHECK(capped, "long log message truncated with a marker");
    CHECK(logs_contain(LOG_INFO, "table"), "__tostring is not called");
    CHECK(logs_source_contains("test.gif#4"), "log source tag");
    script_destroy(s);
}

static void test_log_rate_limit(void)
{
    wchar_t err[512];
    FakeWin w = {0};
    w.id = 6;
    memset(&s_ext, 0, sizeof(s_ext));
    s_ext.now = 100.0;
    Script *s = load_ext(&w, "function burst() for i = 1, 150 do log.info('line', i) end end", err);
    CHECK(s, "burst load: %ls", err);
    if (!s) return;
    logs_reset();
    CHECK(run_check(s, "burst"), "burst run");
    CHECK(logs_count(LOG_INFO) == 100, "rate limit kept %d lines", logs_count(LOG_INFO));
    double next = script_next_timer(s);
    CHECK(next > 100.9 && next < 101.1, "summary timer armed at %.3f", next);
    s_ext.now = 101.0;
    CHECK(script_run_timers(s, 101.0, err, 512), "summary flush: %ls", err);
    CHECK(logs_contain(LOG_WARN, "dropped 50 log line(s)"), "drop summary line");
    script_destroy(s);
}

static void test_lua_notify(void)
{
    wchar_t err[512];
    FakeWin w = {0};
    w.id = 7;

    /* Default title, success and the INFO line. */
    memset(&s_ext, 0, sizeof(s_ext));
    s_ext.now = 10.0;
    s_ext.notify_ok = true;
    logs_reset();
    Script *s = load_ext(&w, "r = yagp.notify('hello')", err);
    CHECK(s, "notify load: %ls", err);
    if (s) {
        CHECK(s_ext.notify_calls == 1, "notify calls %d", s_ext.notify_calls);
        CHECK(strcmp(s_ext.notify_title, "YAGP") == 0, "default title '%s'", s_ext.notify_title);
        CHECK(strcmp(s_ext.notify_text, "hello") == 0, "notify text '%s'", s_ext.notify_text);
        CHECK(logs_contain(LOG_INFO, "notify: YAGP | hello"), "notify logged");
        script_destroy(s);
    }

    /* Explicit title; a table argument becomes its type name without __tostring. */
    memset(&s_ext, 0, sizeof(s_ext));
    s_ext.now = 10.0;
    s_ext.notify_ok = true;
    logs_reset();
    s = load_ext(&w,
        "r = yagp.notify('Title', setmetatable({}, {__tostring = function() error('boom') end}), 'x')\n",
        err);
    CHECK(s, "notify title load: %ls", err);
    if (s) {
        CHECK(strcmp(s_ext.notify_title, "Title") == 0, "explicit title '%s'", s_ext.notify_title);
        CHECK(strcmp(s_ext.notify_text, "table") == 0, "table argument '%s'", s_ext.notify_text);
        script_destroy(s);
    }

    /* Long text is bounded before it reaches the tray fields. */
    memset(&s_ext, 0, sizeof(s_ext));
    s_ext.now = 10.0;
    s_ext.notify_ok = true;
    s = load_ext(&w, "r = yagp.notify('a', string.rep('b', 4000))", err);
    CHECK(s, "notify long load: %ls", err);
    if (s) {
        CHECK(strlen(s_ext.notify_text) <= 1023, "notify text length %zu", strlen(s_ext.notify_text));
        CHECK(strlen(s_ext.notify_text) >= 1000, "notify text was not truncated");
        script_destroy(s);
    }

    /* One balloon per 3 s; the return value reports the suppression. */
    memset(&s_ext, 0, sizeof(s_ext));
    s_ext.now = 20.0;
    s_ext.notify_ok = true;
    logs_reset();
    s = load_ext(&w,
        "r1 = yagp.notify('first')\n"
        "r2 = yagp.notify('second')\n"
        "function check() assert(r1 == true and r2 == false, 'return values') end\n", err);
    CHECK(s, "notify throttle load: %ls", err);
    if (s) {
        CHECK(s_ext.notify_calls == 1, "throttled host calls %d", s_ext.notify_calls);
        CHECK(strcmp(s_ext.notify_text, "first") == 0, "first notification shown");
        CHECK(run_check(s, "check"), "notify return values");
        s_ext.now = 23.0;
        CHECK(script_run_timers(s, 23.0, err, 512), "notify summary flush: %ls", err);
        CHECK(logs_contain(LOG_WARN, "suppressed 1 notification(s)"), "notify summary line");
        script_destroy(s);
    }

    /* A host that cannot show balloons yields false and no INFO line. */
    memset(&s_ext, 0, sizeof(s_ext));
    s_ext.now = 100.0;
    s_ext.notify_ok = false;
    logs_reset();
    s = load_ext(&w,
        "r = yagp.notify('nope')\n"
        "function check() assert(r == false) end\n", err);
    CHECK(s, "notify rejected load: %ls", err);
    if (s) {
        CHECK(s_ext.notify_calls == 1, "rejected host calls %d", s_ext.notify_calls);
        CHECK(run_check(s, "check"), "notify returns false when rejected");
        CHECK(!logs_contain(LOG_INFO, "notify:"), "rejected notify is not logged as shown");
        script_destroy(s);
    }
}

static void test_log_no_ext(void)
{
    wchar_t err[512];
    FakeWin w = {100, 100, 50, 50, false, 0, 0};
    w.id = 9;
    logs_reset();
    const char *src = "function f() log.info('safe') assert(yagp.notify('x') == false) end";
    Script *s = script_create(&OPS, &w, src, strlen(src), "test.lua", "test.gif", w.id, err, 512);
    CHECK(s, "no-ext load: %ls", err);
    if (!s) return;
    CHECK(run_check(s, "f"), "no-ext run");
    CHECK(logs_contain(LOG_INFO, "safe"), "no-ext log");
    script_destroy(s);
}

static void test_slave_api(void)
{
    wchar_t err[512];
    FakeWin master = {0, 0, 50, 50, false, 0, 0};
    FakeWin slave = {200, 200, 40, 40, false, 0, 0};
    master.id = 1;
    slave.id = 7;
    s_slave = &slave;
    memset(&s_ext, 0, sizeof(s_ext));
    Script *s = load_ext_slave(&master,
        "local h = yagp.slave(7) assert(h)\n"
        "assert(#yagp.slaves() == 1 and yagp.slave(99) == nil)\n"
        "local r = h:set_pos(11, 22):set_flip(true):set_opacity(0.5, 0)\n"
        "assert(r == h)\n"
        "local x, y = h:pos() assert(x == 11 and y == 22)\n"
        "assert(h:id() == 7 and h:name() == 'slave')\n"
        "keep = h\n"
        "function dead() assert(yagp.slave(7) == nil)\n"
        "  assert(keep:set_pos(1, 2) == keep) assert(keep:pos() == nil) end\n", err);
    CHECK(s, "slave load: %ls", err);
    if (s) {
        CHECK(slave.x == 11 && slave.y == 22 && slave.flip, "slave chain setters");
        s_slave = NULL;
        CHECK(run_check(s, "dead"), "deleted slave handle");
        script_destroy(s);
    }

    FakeWin master2 = {0, 0, 50, 50, false, 0, 0};
    FakeWin image = {200, 200, 40, 40, false, 0, 0};
    master2.id = 2;
    image.id = 8;
    image.still = true;
    s_slave = &image;
    s = load_ext_slave(&master2,
        "local h = yagp.slave(8) assert(h)\n"
        "local ok, e = h:play('walk') assert(ok == nil and e)\n"
        "assert(h:set_pos(3, 4) == h)\n"
        "local x, y = h:pos() assert(x == 3 and y == 4)\n", err);
    CHECK(s, "static slave load: %ls", err);
    if (s) {
        CHECK(image.x == 3 && image.y == 4, "static slave movement");
        script_destroy(s);
    }
    s_slave = NULL;
}

int main(void)
{
    /* script.c logs through rxi/log.c; capture the lines and keep stderr quiet. */
    log_set_quiet(true);
    log_set_level(LOG_TRACE);
    log_add_callback(collect, NULL, LOG_TRACE);

    /* 1920x1080 primary (40 px taskbar) and a 1280x1024 monitor to its right,
     * offset 200 px down: the strip x >= 1920, y < 200 is a gap. */
    Monitor layout[2] = {mon(0, 0, 1920, 1080, 40, true), mon(1920, 200, 1280, 1024, 0, false)};
    monitors_set_fake(layout, 2);
    test_sandbox();
    test_presets();
    test_contacts();
    test_peer_api();
    test_collisions();
    test_hitbox_mapping();
    test_hitbox_api();
    test_hitbox_presets();
    test_robustness();
    test_events();
    test_clip_path();
    test_input_fsm();
    test_transform();
    test_lua_log();
    test_log_rate_limit();
    test_lua_notify();
    test_log_no_ext();
    test_slave_api();
    if (s_failures) {
        printf("%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("all script tests passed\n");
    return 0;
}
