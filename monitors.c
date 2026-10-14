#include "monitors.h"

#include <shellscalingapi.h>
#include <string.h>

static Monitor s_list[MONITORS_MAX];
static int s_count;

static BOOL CALLBACK collect(HMONITOR mon, HDC dc, LPRECT rc, LPARAM lp)
{
    (void)dc; (void)rc; (void)lp;
    if (s_count >= MONITORS_MAX) return FALSE;
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return TRUE;
    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy))) dx = 96;
    Monitor *m = &s_list[s_count++];
    m->bounds = mi.rcMonitor;
    m->work = mi.rcWork;
    m->dpi = dx;
    m->primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    return TRUE;
}

void monitors_refresh(void)
{
    s_count = 0;
    EnumDisplayMonitors(NULL, NULL, collect, 0);
    if (s_count == 0) { /* Should not happen; keep scripts working with something sane. */
        Monitor *m = &s_list[s_count++];
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &m->work, 0);
        SetRect(&m->bounds, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
        m->dpi = 96;
        m->primary = true;
    }
}

int monitors_count(void)
{
    return s_count;
}

const Monitor *monitors_get(int i)
{
    return i >= 0 && i < s_count ? &s_list[i] : NULL;
}

static long long dist2_to_rect(POINT p, const RECT *r)
{
    long long dx = p.x < r->left ? r->left - p.x : p.x >= r->right ? p.x - r->right + 1 : 0;
    long long dy = p.y < r->top ? r->top - p.y : p.y >= r->bottom ? p.y - r->bottom + 1 : 0;
    return dx * dx + dy * dy;
}

int monitors_index_at(POINT p)
{
    int best = 0;
    long long best_d = -1;
    for (int i = 0; i < s_count; ++i) {
        long long d = dist2_to_rect(p, &s_list[i].bounds);
        if (best_d < 0 || d < best_d) { best = i; best_d = d; }
    }
    return best;
}

RECT monitors_desktop(void)
{
    RECT u = {0, 0, 0, 0};
    for (int i = 0; i < s_count; ++i) {
        if (i == 0) u = s_list[i].bounds;
        else UnionRect(&u, &u, &s_list[i].bounds);
    }
    return u;
}

static bool on_work_area(LONG x, LONG y)
{
    for (int i = 0; i < s_count; ++i) {
        const RECT *w = &s_list[i].work;
        if (x >= w->left && x < w->right && y >= w->top && y < w->bottom) return true;
    }
    return false;
}

bool monitors_rect_fits(const RECT *r)
{
    if (r->right <= r->left || r->bottom <= r->top) return false;
    LONG l = r->left, t = r->top, rr = r->right - 1, b = r->bottom - 1;
    LONG mx = l + (rr - l) / 2, my = t + (b - t) / 2;
    const LONG pts[8][2] = {{l, t}, {rr, t}, {l, b}, {rr, b}, {mx, t}, {mx, b}, {l, my}, {rr, my}};
    for (int i = 0; i < 8; ++i) {
        if (!on_work_area(pts[i][0], pts[i][1])) return false;
    }
    return true;
}

void monitors_set_fake(const Monitor *list, int count)
{
    if (count > MONITORS_MAX) count = MONITORS_MAX;
    if (count < 0) count = 0;
    memcpy(s_list, list, (size_t)count * sizeof(Monitor));
    s_count = count;
}
