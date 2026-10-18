#include "input_fsm.h"

#include <stdlib.h>
#include <string.h>

static void emit(InputOut *out, int ev)
{
    if (out->n < 4) out->ev[out->n++] = ev;
}

static void clear(InputOut *out)
{
    memset(out, 0, sizeof(*out));
}

void ifsm_reset(InputFsm *f)
{
    memset(f, 0, sizeof(*f));
}

void ifsm_cancel(InputFsm *f)
{
    f->button = 0;
    f->dragging = false;
    f->script_drag = false;
}

void ifsm_move(InputFsm *f, const InputMetrics *m, int x, int y, bool inside, unsigned ms, InputOut *out)
{
    clear(out);
    bool moved = x != f->last_x || y != f->last_y;
    int dx = x - f->last_x, dy = y - f->last_y;
    f->last_x = x;
    f->last_y = y;
    if (moved) {
        f->move_ms = ms;
        f->hovered = false;
    }
    /* While a button is held the press owns the cursor (capture): no
     * enter / leave until it is released. */
    if (!f->button) {
        if (inside && !f->inside) {
            f->inside = true;
            f->move_ms = ms;
            f->hovered = false;
            emit(out, IFSM_ENTER);
        } else if (!inside && f->inside) {
            f->inside = false;
            emit(out, IFSM_LEAVE);
            return;
        }
        if (inside && moved) emit(out, IFSM_MOVE);
        return;
    }
    if (!moved) return;
    out->button = f->button;
    if (!f->dragging) {
        if (abs(x - f->down_x) > m->drag_dx / 2 || abs(y - f->down_y) > m->drag_dy / 2) {
            f->dragging = true;
            f->click_valid = false; /* A drag never counts towards a double click. */
            out->dx = x - f->down_x;
            out->dy = y - f->down_y;
            emit(out, IFSM_DRAG_START);
        } else {
            emit(out, IFSM_MOVE);
        }
        return;
    }
    if (f->script_drag) {
        out->dx = dx;
        out->dy = dy;
        emit(out, IFSM_DRAG);
    }
}

void ifsm_button_down(InputFsm *f, const InputMetrics *m, int button, int x, int y, unsigned ms, InputOut *out)
{
    (void)m;
    clear(out);
    if (f->button) return; /* A second button during a press is ignored. */
    if (!f->inside) {
        f->inside = true;
        emit(out, IFSM_ENTER);
    }
    f->button = button;
    f->down_x = x;
    f->down_y = y;
    f->last_x = x;
    f->last_y = y;
    f->dragging = false;
    f->script_drag = false;
    (void)ms;
    out->button = button;
    emit(out, IFSM_DOWN);
}

void ifsm_button_up(InputFsm *f, const InputMetrics *m, int button, int x, int y, unsigned ms, InputOut *out)
{
    clear(out);
    if (!f->button || button != f->button) return;
    out->button = button;
    bool dragged = f->dragging, script_drag = f->script_drag;
    ifsm_cancel(f);
    emit(out, IFSM_UP);
    if (dragged) {
        if (script_drag) emit(out, IFSM_DRAG_END);
        return;
    }
    emit(out, IFSM_CLICK);
    bool dbl = f->click_valid && f->click_button == button && ms - f->click_ms <= m->dbl_ms &&
               abs(x - f->click_x) <= m->dbl_dx / 2 && abs(y - f->click_y) <= m->dbl_dy / 2;
    if (dbl) {
        emit(out, IFSM_DOUBLE_CLICK);
        f->click_valid = false; /* A third click starts over. */
        return;
    }
    f->click_valid = true;
    f->click_button = button;
    f->click_x = x;
    f->click_y = y;
    f->click_ms = ms;
}

void ifsm_leave(InputFsm *f, InputOut *out)
{
    clear(out);
    if (f->button) return; /* Captured: the press decides. */
    if (f->inside) {
        f->inside = false;
        emit(out, IFSM_LEAVE);
    }
}

void ifsm_idle(InputFsm *f, const InputMetrics *m, unsigned ms, InputOut *out)
{
    clear(out);
    if (!f->inside || f->button || f->hovered) return;
    if (ms - f->move_ms >= m->hover_ms) {
        f->hovered = true;
        emit(out, IFSM_HOVER);
    }
}

int ifsm_default_action(const InputFsm *f, const InputOut *out, int i, bool handled)
{
    (void)f;
    if (handled || i < 0 || i >= out->n) return IFSM_ACT_NONE;
    if (out->ev[i] == IFSM_DRAG_START && out->button == 1) return IFSM_ACT_SYSTEM_DRAG;
    if (out->ev[i] == IFSM_CLICK && out->button == 2) return IFSM_ACT_MENU;
    return IFSM_ACT_NONE;
}

void ifsm_take_drag(InputFsm *f)
{
    if (f->dragging) f->script_drag = true;
}
