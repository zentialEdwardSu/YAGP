#ifndef YAGP_CONTACTS_H
#define YAGP_CONTACTS_H

#include <stdbool.h>
#include <stdint.h>

/* Tracks which pairs of rectangles overlap, so a script hears about a contact
 * once when it starts (on_collide) and once when it ends (on_separate).
 * Pure logic, no windows: the host passes one box per overlay each update. */
typedef struct ContactBox {
    int id;                 /* Stable per overlay, non-negative. */
    int l, t, r, b;         /* [l, r) x [t, b) in physical pixels. */
} ContactBox;

typedef struct Contacts {
    uint64_t *pairs;        /* Sorted keys (lo id << 32 | hi id) overlapping last time. */
    int n, cap;
    uint64_t *spare;        /* Reused for the next update's set (swapped with pairs). */
    int spare_cap;
} Contacts;

/* begin = true: a and b started overlapping; false: they stopped (or one of
 * them is gone from boxes). Always a < b. */
typedef void (*ContactFn)(void *ctx, int a, int b, bool begin);

/* Overlap is strict: rectangles that only share an edge do not touch.
 * Calls fn for every change since the previous update: all ends first, then
 * all begins, each in ascending (a, b) order. */
void contacts_update(Contacts *c, const ContactBox *boxes, int n, ContactFn fn, void *ctx);
void contacts_free(Contacts *c);

/* Maps a hitbox in GIF pixels (box = l, t, r, b; empty = none) to the window
 * the GIF is shown in: out = dx, dy, w, h relative to the window's top-left,
 * in display pixels. Scaled with the left/top edge rounded down and the
 * right/bottom edge rounded up, so no visible pixel is cut off, then mirrored
 * when flip_x. An empty box (fully transparent GIF, or none known) yields the
 * whole window. */
void hitbox_in_window(const int box[4], int gif_w, int gif_h, int win_w, int win_h, bool flip_x,
                      int out[4]);

/* Shared with script.c so yagp.overlaps() and on_collide agree. */
static inline bool contacts_overlap(int al, int at, int ar, int ab, int bl, int bt, int br, int bb)
{
    return al < br && bl < ar && at < bb && bt < ab;
}

#endif
