#include "transform.h"
#include "script.h"

#include <math.h>

const Xform XFORM_IDENTITY = {{1, 0, 0, 1, 0, 0}};

bool xform_is_identity(const Xform *x)
{
    for (int i = 0; i < 6; ++i) {
        if (fabs(x->m[i] - XFORM_IDENTITY.m[i]) > 1e-9) return false;
    }
    return true;
}

void xform_mul(const Xform *a, const Xform *b, Xform *out)
{
    const double *x = a->m, *y = b->m;
    Xform r;
    r.m[0] = x[0] * y[0] + x[1] * y[2];
    r.m[1] = x[0] * y[1] + x[1] * y[3];
    r.m[2] = x[2] * y[0] + x[3] * y[2];
    r.m[3] = x[2] * y[1] + x[3] * y[3];
    r.m[4] = x[4] * y[0] + x[5] * y[2] + y[4];
    r.m[5] = x[4] * y[1] + x[5] * y[3] + y[5];
    *out = r;
}

void xform_apply(const Xform *x, double px, double py, double *ox, double *oy)
{
    const double *m = x->m;
    *ox = px * m[0] + py * m[2] + m[4];
    *oy = px * m[1] + py * m[3] + m[5];
}

bool xform_invert(const Xform *x, Xform *out)
{
    const double *m = x->m;
    double det = m[0] * m[3] - m[1] * m[2];
    if (fabs(det) < 1e-12) return false;
    double a = m[3] / det, b = -m[1] / det, c = -m[2] / det, d = m[0] / det;
    out->m[0] = a;
    out->m[1] = b;
    out->m[2] = c;
    out->m[3] = d;
    out->m[4] = -(m[4] * a + m[5] * c);
    out->m[5] = -(m[4] * b + m[5] * d);
    return true;
}

void xform_aabb(const Xform *x, double w, double h, int out[4])
{
    const double cx[4] = {0, w, 0, w}, cy[4] = {0, 0, h, h};
    double l = 1e300, t = 1e300, r = -1e300, b = -1e300;
    for (int i = 0; i < 4; ++i) {
        double px, py;
        xform_apply(x, cx[i], cy[i], &px, &py);
        if (px < l) l = px;
        if (px > r) r = px;
        if (py < t) t = py;
        if (py > b) b = py;
    }
    /* A small epsilon so 2.0000000001 does not grow the window by a pixel. */
    out[0] = (int)floor(l + 1e-6);
    out[1] = (int)floor(t + 1e-6);
    out[2] = (int)ceil(r - 1e-6);
    out[3] = (int)ceil(b - 1e-6);
    if (out[2] <= out[0]) out[2] = out[0] + 1;
    if (out[3] <= out[1]) out[3] = out[1] + 1;
}

/* ease_in = u^2, ease_out = 2u - u^2, ease_in_out = 3u^2 - 2u^3 (smoothstep):
 * each a cubic, so DComp can run it exactly with one AddCubic segment. */
void xform_ease_cubic(int easing, double out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0;
    switch (easing) {
    case SEASE_IN: out[2] = 1; break;
    case SEASE_OUT: out[1] = 2; out[2] = -1; break;
    case SEASE_IN_OUT: out[2] = 3; out[3] = -2; break;
    default: out[1] = 1; break;
    }
}

double xform_ease(int easing, double u)
{
    if (u <= 0) return 0;
    if (u >= 1) return 1;
    double c[4];
    xform_ease_cubic(easing, c);
    return c[0] + u * (c[1] + u * (c[2] + u * c[3]));
}

void xform_anim_start(XformAnim *a, const Xform *from, const Xform *to, double t0, double duration, int easing)
{
    a->from = *from;
    a->to = *to;
    a->t0 = t0;
    a->duration = duration;
    xform_ease_cubic(easing, a->c);
}

static double cubic_at(const double c[4], double u)
{
    if (u <= 0) return c[0];
    if (u >= 1) return c[0] + c[1] + c[2] + c[3];
    return c[0] + u * (c[1] + u * (c[2] + u * c[3]));
}

void xform_anim_rebase(XformAnim *a, double now)
{
    if (a->duration <= 0) return;
    double u0 = (now - a->t0) / a->duration;
    if (u0 <= 0) return;
    if (u0 >= 1) {
        a->from = a->to;
        a->t0 = now;
        a->duration = 0;
        return;
    }
    /* g(v) = (f(u0 + v d) - f(u0)) / (1 - f(u0)), d = 1 - u0: still a cubic. */
    const double *c = a->c;
    double d = 1 - u0, f0 = cubic_at(c, u0), rest = cubic_at(c, 1) - f0;
    Xform cur;
    for (int i = 0; i < 6; ++i) cur.m[i] = a->from.m[i] + (a->to.m[i] - a->from.m[i]) * f0;
    a->from = cur;
    a->t0 = now;
    a->duration *= d;
    if (rest < 1e-9 && rest > -1e-9) {
        a->c[0] = 0; a->c[1] = 1; a->c[2] = 0; a->c[3] = 0; /* Already there: a flat line. */
        return;
    }
    double g1 = (c[1] + 2 * c[2] * u0 + 3 * c[3] * u0 * u0) * d / rest;
    double g2 = (c[2] + 3 * c[3] * u0) * d * d / rest;
    double g3 = c[3] * d * d * d / rest;
    a->c[0] = 0;
    a->c[1] = g1;
    a->c[2] = g2;
    a->c[3] = g3;
}

void xform_anim_eval(const XformAnim *a, double now, Xform *out)
{
    if (a->duration <= 0 || now >= a->t0 + a->duration) {
        *out = a->to;
        return;
    }
    double f = cubic_at(a->c, (now - a->t0) / a->duration);
    for (int i = 0; i < 6; ++i) out->m[i] = a->from.m[i] + (a->to.m[i] - a->from.m[i]) * f;
}

bool xform_anim_done(const XformAnim *a, double now)
{
    return a->duration <= 0 || now >= a->t0 + a->duration;
}
