#ifndef YAGP_INPUT_FSM_H
#define YAGP_INPUT_FSM_H

#include <stdbool.h>

/* Turns raw button / move / wheel / leave notifications for one overlay into
 * the events scripts see (mouse_down, click, drag_start, ...), the same way
 * for a normal window (driven by its messages) and a click-through one
 * (driven by the low-level mouse hook). Pure logic: no windows, no time
 * source; the caller passes times in milliseconds and the system's drag /
 * double-click metrics, and executes the actions returned. */

enum {
    IFSM_EV_NONE = -1,
    /* Same order as ScriptEvent SEV_MOUSE_DOWN..SEV_DRAG_END. */
    IFSM_DOWN = 0, IFSM_UP, IFSM_CLICK, IFSM_DOUBLE_CLICK, IFSM_ENTER, IFSM_LEAVE, IFSM_HOVER, IFSM_MOVE,
    IFSM_WHEEL, IFSM_DRAG_START, IFSM_DRAG, IFSM_DRAG_END
};

/* What the default behaviour should do after an event nobody handled. */
enum {
    IFSM_ACT_NONE = 0,
    IFSM_ACT_SYSTEM_DRAG,  /* Left drag_start unhandled: hand over to the system move loop. */
    IFSM_ACT_MENU,         /* Right click unhandled: open the context menu. */
};

typedef struct InputMetrics {
    int drag_dx, drag_dy;         /* SM_CXDRAG / SM_CYDRAG */
    int dbl_dx, dbl_dy;           /* SM_CXDOUBLECLK / SM_CYDOUBLECLK */
    unsigned dbl_ms;              /* GetDoubleClickTime */
    unsigned hover_ms;            /* SPI_GETMOUSEHOVERTIME */
} InputMetrics;

typedef struct InputFsm {
    bool inside;
    int button;                   /* Pressed button (1 left, 2 right, 3 middle), 0 = none. */
    int down_x, down_y;
    bool dragging;                /* drag_start delivered for this press. */
    bool script_drag;             /* ...and handled: drag / drag_end go to the script. */
    int last_x, last_y;
    unsigned move_ms;             /* Last movement, for hover. */
    bool hovered;                 /* hover delivered since the last movement / entry. */
    /* Double click: the previous click. */
    int click_button;
    int click_x, click_y;
    unsigned click_ms;
    bool click_valid;
} InputFsm;

/* Up to four events per input (e.g. enter + move, or up + click + double). */
typedef struct InputOut {
    int ev[4];
    int n;
    int button;                   /* For button events. */
    int dx, dy;                   /* For drag. */
} InputOut;

void ifsm_reset(InputFsm *f);
/* Cursor at (x, y), inside = over the overlay's visible area. */
void ifsm_move(InputFsm *f, const InputMetrics *m, int x, int y, bool inside, unsigned ms, InputOut *out);
void ifsm_button_down(InputFsm *f, const InputMetrics *m, int button, int x, int y, unsigned ms, InputOut *out);
void ifsm_button_up(InputFsm *f, const InputMetrics *m, int button, int x, int y, unsigned ms, InputOut *out);
/* The cursor left the window (or the hook saw it leave the area). */
void ifsm_leave(InputFsm *f, InputOut *out);
/* Polled while inside and still: hover after hover_ms without movement. */
void ifsm_idle(InputFsm *f, const InputMetrics *m, unsigned ms, InputOut *out);
/* The press ended elsewhere (capture lost, system drag took over). */
void ifsm_cancel(InputFsm *f);

/* After delivering out: what the default behaviour is for event index i
 * given whether a handler stopped it. */
int ifsm_default_action(const InputFsm *f, const InputOut *out, int i, bool handled);
/* Marks the drag as script-owned (drag_start was handled). */
void ifsm_take_drag(InputFsm *f);

#endif
