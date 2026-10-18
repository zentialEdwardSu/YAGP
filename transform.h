#ifndef YAGP_TRANSFORM_H
#define YAGP_TRANSFORM_H

#include <stdbool.h>

/* 2D affine transforms in the D2D row-vector convention:
 * (x, y) -> (x*m[0] + y*m[2] + m[4], x*m[1] + y*m[3] + m[5]).
 * Pure maths shared by the overlay (layout, hit tests) and the tests; the
 * renderer animates the same matrices on the DWM side. */
typedef struct Xform { double m[6]; } Xform;

/* A transition from `from` to `to` over duration seconds starting at t0:
 * value = from + (to - from) * (c[0] + c[1]u + c[2]u^2 + c[3]u^3), u = elapsed
 * / duration, every element alike. A cubic, because that is what
 * DirectComposition animates natively. */
typedef struct XformAnim {
    Xform from, to;
    double t0, duration;
    double c[4];
} XformAnim;

extern const Xform XFORM_IDENTITY;

bool xform_is_identity(const Xform *x);
/* out = a then b (p * a * b). out may alias a or b. */
void xform_mul(const Xform *a, const Xform *b, Xform *out);
void xform_apply(const Xform *x, double px, double py, double *ox, double *oy);
/* False if x has no inverse. */
bool xform_invert(const Xform *x, Xform *out);
/* Bounding box {l, t, r, b} of the w x h rectangle at the origin, mapped by x,
 * rounded outwards to whole pixels. */
void xform_aabb(const Xform *x, double w, double h, int out[4]);
/* Fraction of the way at u in [0, 1] for an easing curve. */
double xform_ease(int easing, double u);
/* Cubic polynomial coefficients {c0, c1, c2, c3} of the easing, in u: what
 * DirectComposition's AddCubic takes after scaling by the duration. */
void xform_ease_cubic(int easing, double out[4]);
void xform_anim_start(XformAnim *a, const Xform *from, const Xform *to, double t0, double duration, int easing);
/* Restarts a running animation at now from its current value, with the
 * remaining time and the rest of the same curve, so it can be handed to the
 * renderer again (e.g. after the base transform changed). */
void xform_anim_rebase(XformAnim *a, double now);
/* Value of the animation at time now (clamped to its ends). */
void xform_anim_eval(const XformAnim *a, double now, Xform *out);
/* True once now >= t0 + duration. */
bool xform_anim_done(const XformAnim *a, double now);

#endif
