/* Per-tick script cost with many overlays, modelled on the real host loop:
 * every tick, for each overlay, app_reschedule + app_tick ask
 * overlay_is_moving (script_has_update) and then run update(dt); every peer
 * query a script makes asks script_has_update for each other overlay too
 * (fill_peer -> overlay_is_moving). Scripts are bounce.lua with COLLIDE on,
 * which calls yagp.overlaps() every tick.
 *
 *   script_bench [overlays] [ticks] */
#include "monitors.h"
#include "script.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_W 64

typedef struct Win { double x, y; int w, h, id; bool flip; Script *s; } Win;
static Win s_w[MAX_W];
static int s_n;

static void get_rect(void *c, double *x, double *y, int *w, int *h)
{
    Win *f = (Win *)c;
    *x = f->x; *y = f->y; *w = f->w; *h = f->h;
}
static void set_pos(void *c, double x, double y) { Win *f = (Win *)c; f->x = x; f->y = y; }
static void set_flip(void *c, bool v) { ((Win *)c)->flip = v; }
static void set_rate(void *c, int hz) { (void)c; (void)hz; }
static bool get_mouse(void *c, ScriptMouse *m) { (void)c; memset(m, 0, sizeof(*m)); return true; }
static int get_peers(void *c, ScriptPeer *out, int cap)
{
    int n = 0;
    for (int i = 0; i < s_n && n < cap; ++i) {
        Win *f = &s_w[i];
        if (f == (Win *)c) continue;
        ScriptPeer *p = &out[n++];
        memset(p, 0, sizeof(*p));
        bool moving = script_has_update(f->s); /* As fill_peer -> overlay_is_moving. */
        p->id = f->id;
        p->x = p->hx = f->x;
        p->y = p->hy = f->y;
        p->w = p->hw = f->w;
        p->h = p->hh = f->h;
        p->scripted = f->s != NULL;
        p->vx = moving ? 1 : 0;
        p->name = "bench.gif";
    }
    return n;
}
static const ScriptHostOps OPS = {get_rect, set_pos, set_flip, set_rate, get_mouse, get_peers, NULL, NULL};

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 16, ticks = argc > 2 ? atoi(argv[2]) : 3000;
    if (n < 1 || n > MAX_W) return 2;
    Monitor m = {{0, 0, 3840, 2160}, {0, 0, 3840, 2120}, 96, true};
    monitors_set_fake(&m, 1);
    char path[512];
    snprintf(path, sizeof(path), "%s/bounce.lua", YAGP_SCRIPT_DIR);
    FILE *f = fopen(path, "rb");
    if (!f) return 3;
    static char src[1 << 16];
    size_t len = fread(src, 1, sizeof(src) - 1, f);
    fclose(f);
    wchar_t err[512];
    s_n = n;
    for (int i = 0; i < n; ++i) {
        s_w[i] = (Win){100.0 + (i % 8) * 400, 100.0 + (i / 8) * 250, 120, 90, i + 1, false, NULL};
        s_w[i].s = script_create(&OPS, &s_w[i], src, len, "bounce.lua", "bench.gif", i + 1, err, 512);
        if (!s_w[i].s) { printf("load: %ls\n", err); return 4; }
    }
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    long long checks = 0;
    for (int t = 0; t < ticks; ++t) {
        for (int i = 0; i < n; ++i) checks += script_has_update(s_w[i].s); /* app_reschedule */
        for (int i = 0; i < n; ++i) {
            if (!script_has_update(s_w[i].s)) continue;                    /* app_tick */
            if (!script_update(s_w[i].s, 1.0 / 60, err, 512)) { printf("update: %ls\n", err); return 5; }
        }
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    double us = (double)(t1.QuadPart - t0.QuadPart) * 1e6 / (double)fq.QuadPart / ticks;
    printf("%d overlays: %.1f us per tick (all scripts), %.2f us per script update\n", n, us, us / n);
    for (int i = 0; i < n; ++i) script_destroy(s_w[i].s);
    return checks > 0 ? 0 : 6;
}
