/* Lua host + preset regression test. Runs script.c against a fake window and a
 * fake monitor layout; exits non-zero on the first failed check.
 *   cmake --build build --target script_test && build\script_test.exe */
#include "contacts.h"
#include "i18n.h"
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

static double center_dist(const FakeWin *w)
{
    double dx = w->x + w->w / 2.0 - w->mouse.x, dy = w->y + w->h / 2.0 - w->mouse.y;
    return sqrt(dx * dx + dy * dy);
}

static int s_failures;
#define CHECK(cond, ...) do { if (!(cond)) { ++s_failures; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

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
        /* Mouse on the second monitor: follows across without entering the gap. */
        set_mouse(&w, 2600, 700);
        run(s, 1200, &w, ok_fits, NULL, "follow cross");
        CHECK(w.x >= 1920, "follow did not cross to monitor 2: (%g, %g)", w.x, w.y);
        /* Mouse in the gap above monitor 2: slides along the edge, stays on screen. */
        set_mouse(&w, 2500, 50);
        run(s, 1200, &w, ok_fits, NULL, "follow gap");
        /* Mouse off every screen edge: clamps, no error. */
        set_mouse(&w, -5000, -5000);
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
        set_mouse(&w, 950, 540); /* Right on top of it. */
        run(s, 120, &w, ok_in_rect, &work, "flee");
        CHECK(center_dist(&w) > 150, "flee did not get away: %g", center_dist(&w));
        /* Keep chasing it into the top-left corner. It may run back there after
         * each escape, but must never stay cornered for long. */
        int cornered = 0, longest = 0, escapes = 0;
        for (int i = 0; i < 600; ++i) {
            set_mouse(&w, (int)(w.x + w.w / 2.0) + 30, (int)(w.y + w.h / 2.0) + 30);
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
        set_mouse(&w, 1900, 1030);
        run(s, 60 * 60, &w, ok_in_rect, &work, "flee return");
        CHECK(fabs(w.x - 900) < 1 && fabs(w.y - 500) < 1, "flee did not return home: (%g, %g)", w.x, w.y);
        /* Unreadable cursor: stays still. */
        w.mouse_ok = false;
        double hx = w.x;
        run(s, 60, &w, ok_in_rect, &work, "flee no mouse");
        CHECK(w.x == hx, "flee moved without a cursor");
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

int main(void)
{
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
    if (s_failures) {
        printf("%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("all script tests passed\n");
    return 0;
}
