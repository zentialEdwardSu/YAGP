#ifndef YAGP_MONITORS_H
#define YAGP_MONITORS_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdbool.h>

/* Snapshot of the display layout in physical pixels (the process is
 * PerMonitorV2, so these match window coordinates directly). */
#define MONITORS_MAX 16

typedef struct Monitor {
    RECT bounds;
    RECT work;   /* bounds minus taskbar / docked app bars. */
    UINT dpi;
    bool primary;
} Monitor;

/* Re-enumerate the displays. Call on startup and on WM_DISPLAYCHANGE /
 * WM_SETTINGCHANGE(SPI_SETWORKAREA). */
void monitors_refresh(void);
int monitors_count(void);
const Monitor *monitors_get(int i);
/* Index of the monitor containing p, or the nearest one. */
int monitors_index_at(POINT p);
/* Bounding box of every monitor. */
RECT monitors_desktop(void);
/* True if r lies inside the union of work areas: every corner and edge midpoint
 * is on some work area. Gaps between monitors of different sizes count as out. */
bool monitors_rect_fits(const RECT *r);
/* Tests: replace the snapshot (count is clamped to MONITORS_MAX). */
void monitors_set_fake(const Monitor *list, int count);

#endif
