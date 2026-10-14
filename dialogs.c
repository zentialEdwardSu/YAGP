/* Colour-key and scale prompts built from in-memory dialog templates (no .rc file). */
#include "app.h"
#include "gif_stream.h"
#include "i18n.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

enum { IDC_HEX = 100, IDC_TOL = 101 };

typedef struct KeyPrompt {
    uint32_t rgb;
    int tolerance;
} KeyPrompt;

static WORD *align_dword(WORD *p)
{
    while ((ULONG_PTR)p & 3) *p++ = 0;
    return p;
}

static WORD *put_str(WORD *p, const wchar_t *s)
{
    do { *p++ = (WORD)*s; } while (*s++);
    return p;
}

/* Either cls_atom (0x0080 button, 0x0081 edit, 0x0082 static) or cls_name is used. */
static WORD *put_item_ex(WORD *p, DWORD style, short x, short y, short cx, short cy,
                         WORD id, WORD cls_atom, const wchar_t *cls_name, const wchar_t *text)
{
    p = align_dword(p);
    DLGITEMTEMPLATE *it = (DLGITEMTEMPLATE *)p;
    it->style = style | WS_CHILD | WS_VISIBLE;
    it->dwExtendedStyle = 0;
    it->x = x;
    it->y = y;
    it->cx = cx;
    it->cy = cy;
    it->id = id;
    p = (WORD *)(it + 1);
    if (cls_name) {
        p = put_str(p, cls_name);
    } else {
        *p++ = 0xFFFF;
        *p++ = cls_atom;
    }
    p = put_str(p, text);
    *p++ = 0; /* No creation data. */
    return p;
}

static WORD *put_item(WORD *p, DWORD style, short x, short y, short cx, short cy,
                      WORD id, WORD cls, const wchar_t *text)
{
    return put_item_ex(p, style, x, y, cx, cy, id, cls, NULL, text);
}

static WORD *put_header(DLGTEMPLATE *tpl, WORD items, short cx, short cy, const wchar_t *title)
{
    tpl->style = DS_MODALFRAME | DS_SHELLFONT | DS_CENTER | DS_SETFOREGROUND |
                 WS_POPUP | WS_CAPTION | WS_SYSMENU;
    tpl->dwExtendedStyle = WS_EX_TOPMOST;
    tpl->cdit = items;
    tpl->cx = cx;
    tpl->cy = cy;
    WORD *p = (WORD *)(tpl + 1);
    *p++ = 0; /* No menu. */
    *p++ = 0; /* Default dialog class. */
    p = put_str(p, title);
    *p++ = 9;
    return put_str(p, L"MS Shell Dlg");
}

static INT_PTR CALLBACK key_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    KeyPrompt *kp = (KeyPrompt *)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg) {
    case WM_INITDIALOG: {
        wchar_t text[16];
        kp = (KeyPrompt *)lp;
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)kp);
        _snwprintf_s(text, 16, _TRUNCATE, L"#%06X", (unsigned)kp->rgb);
        SetDlgItemTextW(dlg, IDC_HEX, text);
        SetDlgItemInt(dlg, IDC_TOL, (UINT)kp->tolerance, FALSE);
        SendDlgItemMessageW(dlg, IDC_HEX, EM_LIMITTEXT, 10, 0);
        SendDlgItemMessageW(dlg, IDC_TOL, EM_LIMITTEXT, 3, 0);
        SendDlgItemMessageW(dlg, IDC_HEX, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(dlg, IDC_HEX));
        return FALSE; /* Focus set manually. */
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            wchar_t text[16];
            uint32_t rgb;
            BOOL ok = FALSE;
            GetDlgItemTextW(dlg, IDC_HEX, text, 16);
            if (gif_parse_hex_color(text, &rgb) != 0) {
                MessageBeep(MB_ICONWARNING);
                SetFocus(GetDlgItem(dlg, IDC_HEX));
                SendDlgItemMessageW(dlg, IDC_HEX, EM_SETSEL, 0, -1);
                return TRUE;
            }
            UINT tol = GetDlgItemInt(dlg, IDC_TOL, &ok, FALSE);
            if (!ok || tol > 255) {
                MessageBeep(MB_ICONWARNING);
                SetFocus(GetDlgItem(dlg, IDC_TOL));
                SendDlgItemMessageW(dlg, IDC_TOL, EM_SETSEL, 0, -1);
                return TRUE;
            }
            kp->rgb = rgb;
            kp->tolerance = (int)tol;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

bool app_prompt_color_key(uint32_t *rgb, int *tolerance)
{
    DWORD storage[512]; /* DWORD-aligned template buffer. */
    memset(storage, 0, sizeof(storage));
    DLGTEMPLATE *tpl = (DLGTEMPLATE *)storage;
    WORD *p = put_header(tpl, 6, 200, 80, TXT_DLG_KEY_TITLE);

    p = put_item(p, SS_LEFT, 7, 7, 186, 9, (WORD)-1, 0x0082,
                 TXT_DLG_KEY_COLOR);
    p = put_item(p, ES_LEFT | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 7, 18, 186, 14,
                 IDC_HEX, 0x0081, L"");
    p = put_item(p, SS_LEFT, 7, 40, 120, 9, (WORD)-1, 0x0082, TXT_DLG_KEY_TOLERANCE);
    p = put_item(p, ES_LEFT | ES_NUMBER | WS_BORDER | WS_TABSTOP, 130, 37, 63, 14,
                 IDC_TOL, 0x0081, L"");
    p = put_item(p, BS_DEFPUSHBUTTON | WS_TABSTOP, 88, 59, 50, 14, IDOK, 0x0080, TXT_OK);
    p = put_item(p, BS_PUSHBUTTON | WS_TABSTOP, 143, 59, 50, 14, IDCANCEL, 0x0080, TXT_CANCEL);

    KeyPrompt kp = {*rgb, *tolerance};
    app_modal_begin();
    INT_PTR r = DialogBoxIndirectParamW(g_app.instance, tpl, g_app.tray_hwnd, key_proc, (LPARAM)&kp);
    app_modal_end();
    if (r != IDOK) return false;
    *rgb = kp.rgb;
    *tolerance = kp.tolerance;
    return true;
}

/* ---- minimal slider control ------------------------------------------------
 * A self-drawn horizontal slider, so the app does not need the comctl32
 * Trackbar. Range is APP_SCALE_MIN..APP_SCALE_MAX. User input notifies the
 * parent with WM_HSCROLL (lParam = control HWND).
 * Keys: Left/Right +-1, PgUp/PgDn +-25, Home/End to the limits; wheel +-5. */

#define SLIDER_CLASS L"YAGP.Slider"
#define SLM_SETPOS (WM_USER + 1) /* wParam = position; does not notify */
#define SLM_GETPOS (WM_USER + 2)

typedef struct SliderState {
    int pos;
    bool dragging;
} SliderState;

static int slider_clamp(int v)
{
    return v < APP_SCALE_MIN ? APP_SCALE_MIN : v > APP_SCALE_MAX ? APP_SCALE_MAX : v;
}

/* The track spans the client width minus half a thumb on each side. */
static void slider_track(HWND hwnd, RECT *track, int *thumb_w)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    int tw = (rc.bottom - rc.top) / 2;
    if (tw < 6) tw = 6;
    *thumb_w = tw;
    *track = rc;
    track->left += tw / 2 + 1;
    track->right -= tw / 2 + 1;
}

static int slider_x_for(HWND hwnd, int pos)
{
    RECT t;
    int tw;
    slider_track(hwnd, &t, &tw);
    return t.left + MulDiv(pos - APP_SCALE_MIN, t.right - t.left, APP_SCALE_MAX - APP_SCALE_MIN);
}

static int slider_pos_for(HWND hwnd, int x)
{
    RECT t;
    int tw;
    slider_track(hwnd, &t, &tw);
    if (t.right <= t.left) return APP_SCALE_MIN;
    return slider_clamp(APP_SCALE_MIN + MulDiv(x - t.left, APP_SCALE_MAX - APP_SCALE_MIN, t.right - t.left));
}

static void slider_set(HWND hwnd, SliderState *st, int pos, bool notify)
{
    pos = slider_clamp(pos);
    if (pos == st->pos) return;
    st->pos = pos;
    InvalidateRect(hwnd, NULL, FALSE);
    if (notify) SendMessageW(GetParent(hwnd), WM_HSCROLL, 0, (LPARAM)hwnd);
}

static void slider_paint(HWND hwnd, const SliderState *st)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc, t;
    int tw;
    GetClientRect(hwnd, &rc);
    slider_track(hwnd, &t, &tw);
    FillRect(dc, &rc, GetSysColorBrush(COLOR_BTNFACE));

    int cy = (rc.top + rc.bottom) / 2 - 2;
    for (int v = 50; v < APP_SCALE_MAX; v += 50) { /* Ticks every 50%. */
        int x = slider_x_for(hwnd, v);
        RECT tick = {x, rc.bottom - 4, x + 1, rc.bottom - 1};
        FillRect(dc, &tick, GetSysColorBrush(COLOR_GRAYTEXT));
    }
    int x = slider_x_for(hwnd, st->pos);
    RECT groove = {t.left, cy - 2, t.right, cy + 2};
    RECT filled = {t.left, cy - 2, x, cy + 2};
    RECT thumb = {x - tw / 2, rc.top + 2, x - tw / 2 + tw, rc.bottom - 6};
    FillRect(dc, &groove, GetSysColorBrush(COLOR_BTNSHADOW));
    FillRect(dc, &filled, GetSysColorBrush(COLOR_HIGHLIGHT));
    FillRect(dc, &thumb, GetSysColorBrush(COLOR_HIGHLIGHT));
    if (GetFocus() == hwnd && !(SendMessageW(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
        DrawFocusRect(dc, &rc);
    }
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK slider_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    SliderState *st = (SliderState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!st && msg != WM_NCCREATE) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_NCCREATE:
        st = (SliderState *)calloc(1, sizeof(*st));
        if (!st) return FALSE;
        st->pos = APP_SCALE_MIN;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        break;
    case WM_NCDESTROY:
        free(st);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    case SLM_SETPOS:
        slider_set(hwnd, st, (int)wp, false);
        return 0;
    case SLM_GETPOS:
        return st->pos;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS; /* Tab, Enter and Esc stay with the dialog. */
    case WM_KEYDOWN:
        switch (wp) {
        case VK_LEFT: case VK_DOWN: slider_set(hwnd, st, st->pos - 1, true); break;
        case VK_RIGHT: case VK_UP: slider_set(hwnd, st, st->pos + 1, true); break;
        case VK_PRIOR: slider_set(hwnd, st, st->pos + 25, true); break;
        case VK_NEXT: slider_set(hwnd, st, st->pos - 25, true); break;
        case VK_HOME: slider_set(hwnd, st, APP_SCALE_MIN, true); break;
        case VK_END: slider_set(hwnd, st, APP_SCALE_MAX, true); break;
        }
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        SetCapture(hwnd);
        st->dragging = true;
        slider_set(hwnd, st, slider_pos_for(hwnd, (short)LOWORD(lp)), true);
        return 0;
    case WM_MOUSEMOVE:
        if (st->dragging) slider_set(hwnd, st, slider_pos_for(hwnd, (short)LOWORD(lp)), true);
        return 0;
    case WM_LBUTTONUP:
        st->dragging = false;
        if (GetCapture() == hwnd) ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        st->dragging = false;
        return 0;
    case WM_MOUSEWHEEL:
        slider_set(hwnd, st, st->pos + (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 5 : -5), true);
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_UPDATEUISTATE:
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        slider_paint(hwnd, st);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool register_slider_class(void)
{
    static bool registered;
    if (registered) return true;
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = slider_proc;
    wc.hInstance = g_app.instance;
    wc.hCursor = LoadCursorW(NULL, IDC_HAND);
    wc.lpszClassName = SLIDER_CLASS;
    registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return registered;
}

/* ---- scale prompt ---------------------------------------------------------- */

enum { IDC_SLIDER = 200, IDC_PCT = 201, IDC_RESET = 202 };

typedef struct ScalePrompt {
    int original, value;
    void (*preview)(int pct, void *ctx);
    void *ctx;
    bool syncing; /* Suppresses EN_CHANGE while the slider writes the edit box. */
} ScalePrompt;

static void scale_set(HWND dlg, ScalePrompt *sp, int pct, bool update_slider, bool update_edit)
{
    if (pct < APP_SCALE_MIN) pct = APP_SCALE_MIN;
    if (pct > APP_SCALE_MAX) pct = APP_SCALE_MAX;
    if (update_slider) SendDlgItemMessageW(dlg, IDC_SLIDER, SLM_SETPOS, (WPARAM)pct, 0);
    if (update_edit) {
        sp->syncing = true;
        SetDlgItemInt(dlg, IDC_PCT, (UINT)pct, FALSE);
        sp->syncing = false;
    }
    if (pct != sp->value) {
        sp->value = pct;
        sp->preview(pct, sp->ctx); /* Live preview on the overlay itself. */
    }
}

/* Place the dialog next to the cursor, kept inside that monitor's work area. */
static void place_near_cursor(HWND dlg)
{
    POINT pt;
    RECT r;
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetCursorPos(&pt);
    GetWindowRect(dlg, &r);
    if (!GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi)) return;
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = pt.x + 16, y = pt.y + 16;
    if (x + w > mi.rcWork.right) x = pt.x - 16 - w;
    if (y + h > mi.rcWork.bottom) y = pt.y - 16 - h;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top) y = mi.rcWork.top;
    SetWindowPos(dlg, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static INT_PTR CALLBACK scale_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    ScalePrompt *sp = (ScalePrompt *)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg) {
    case WM_INITDIALOG: {
        sp = (ScalePrompt *)lp;
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)sp);
        HWND slider = GetDlgItem(dlg, IDC_SLIDER);
        SendDlgItemMessageW(dlg, IDC_PCT, EM_LIMITTEXT, 3, 0);
        scale_set(dlg, sp, sp->value, true, true);
        place_near_cursor(dlg);
        SetFocus(slider);
        return FALSE; /* Focus set manually. */
    }
    case WM_HSCROLL:
        if ((HWND)lp == GetDlgItem(dlg, IDC_SLIDER)) {
            int pos = (int)SendMessageW((HWND)lp, SLM_GETPOS, 0, 0);
            scale_set(dlg, sp, pos, false, true);
            return TRUE;
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_PCT:
            if (HIWORD(wp) == EN_CHANGE && !sp->syncing) {
                BOOL ok = FALSE;
                UINT v = GetDlgItemInt(dlg, IDC_PCT, &ok, FALSE);
                /* Only follow complete in-range values; partial input like "1" while
                 * typing "150" would otherwise make the overlay jump around. */
                if (ok && v >= APP_SCALE_MIN && v <= APP_SCALE_MAX) scale_set(dlg, sp, (int)v, true, false);
            } else if (HIWORD(wp) == EN_KILLFOCUS) {
                scale_set(dlg, sp, sp->value, false, true); /* Normalise leftovers. */
            }
            return TRUE;
        case IDC_RESET:
            scale_set(dlg, sp, 100, true, true);
            return TRUE;
        case IDOK: {
            /* Enter in the edit box: take its value even if focus never left it. */
            BOOL ok = FALSE;
            UINT v = GetDlgItemInt(dlg, IDC_PCT, &ok, FALSE);
            if (ok) scale_set(dlg, sp, (int)v, true, true);
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

bool app_prompt_scale(int *pct, void (*preview)(int pct, void *ctx), void *ctx)
{
    if (!register_slider_class()) return false;

    DWORD storage[512];
    memset(storage, 0, sizeof(storage));
    DLGTEMPLATE *tpl = (DLGTEMPLATE *)storage;
    WORD *p = put_header(tpl, 7, 220, 84, TXT_DLG_SCALE_TITLE);

    p = put_item(p, SS_LEFT, 7, 7, 206, 9, (WORD)-1, 0x0082, TXT_DLG_SCALE_HINT);
    p = put_item_ex(p, WS_TABSTOP, 4, 18, 212, 20, IDC_SLIDER, 0, SLIDER_CLASS, L"");
    p = put_item(p, ES_RIGHT | ES_NUMBER | WS_BORDER | WS_TABSTOP, 7, 43, 34, 14,
                 IDC_PCT, 0x0081, L"");
    p = put_item(p, SS_LEFT, 44, 46, 100, 9, (WORD)-1, 0x0082, L"%   (5 – 250)");
    p = put_item(p, BS_PUSHBUTTON | WS_TABSTOP, 7, 64, 50, 14, IDC_RESET, 0x0080, L"100%");
    p = put_item(p, BS_DEFPUSHBUTTON | WS_TABSTOP, 108, 64, 50, 14, IDOK, 0x0080, TXT_OK);
    p = put_item(p, BS_PUSHBUTTON | WS_TABSTOP, 163, 64, 50, 14, IDCANCEL, 0x0080, TXT_CANCEL);

    ScalePrompt sp;
    memset(&sp, 0, sizeof(sp));
    sp.original = sp.value = *pct;
    sp.preview = preview;
    sp.ctx = ctx;
    app_modal_begin();
    INT_PTR r = DialogBoxIndirectParamW(g_app.instance, tpl, g_app.tray_hwnd, scale_proc, (LPARAM)&sp);
    app_modal_end();
    if (r != IDOK) {
        if (sp.value != sp.original) preview(sp.original, ctx); /* Revert the live preview. */
        return false;
    }
    *pct = sp.value;
    return true;
}
