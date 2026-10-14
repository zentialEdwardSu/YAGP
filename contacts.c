#include "contacts.h"

#include <stdlib.h>
#include <string.h>

static uint64_t pair_key(int a, int b)
{
    uint32_t lo = (uint32_t)(a < b ? a : b), hi = (uint32_t)(a < b ? b : a);
    return (uint64_t)lo << 32 | hi;
}

static int cmp_key(const void *pa, const void *pb)
{
    uint64_t a = *(const uint64_t *)pa, b = *(const uint64_t *)pb;
    return a < b ? -1 : a > b;
}

static void emit(ContactFn fn, void *ctx, uint64_t key, bool begin)
{
    if (fn) fn(ctx, (int)(key >> 32), (int)(uint32_t)key, begin);
}

void contacts_update(Contacts *c, const ContactBox *boxes, int n, ContactFn fn, void *ctx)
{
    /* All pairs: a handful of overlays, so O(n^2) is a few dozen comparisons.
     * Called every tick something moves: the new set goes into a spare buffer
     * that is swapped with the old one, so steady state allocates nothing. */
    int cap = n > 1 ? n * (n - 1) / 2 : 1;
    if (cap > c->spare_cap) {
        uint64_t *grown = (uint64_t *)realloc(c->spare, (size_t)cap * sizeof(uint64_t));
        if (!grown) return; /* Keep the previous state; try again next update. */
        c->spare = grown;
        c->spare_cap = cap;
    }
    uint64_t *now = c->spare;
    int m = 0;
    for (int i = 0; i < n; ++i) {
        const ContactBox *a = &boxes[i];
        for (int j = i + 1; j < n; ++j) {
            const ContactBox *b = &boxes[j];
            if (a->id == b->id) continue;
            if (contacts_overlap(a->l, a->t, a->r, a->b, b->l, b->t, b->r, b->b)) now[m++] = pair_key(a->id, b->id);
        }
    }
    qsort(now, (size_t)m, sizeof(uint64_t), cmp_key);

    /* Ends: in the old set, not in the new one (a vanished id lands here too). */
    for (int i = 0, j = 0; i < c->n; ++i) {
        while (j < m && now[j] < c->pairs[i]) ++j;
        if (j == m || now[j] != c->pairs[i]) emit(fn, ctx, c->pairs[i], false);
    }
    /* Begins: in the new set, not in the old one. */
    for (int j = 0, i = 0; j < m; ++j) {
        while (i < c->n && c->pairs[i] < now[j]) ++i;
        if (i == c->n || c->pairs[i] != now[j]) emit(fn, ctx, now[j], true);
    }

    uint64_t *old = c->pairs;
    int old_cap = c->cap;
    c->pairs = now;
    c->cap = c->spare_cap;
    c->n = m;
    c->spare = old;
    c->spare_cap = old_cap;
}

void hitbox_in_window(const int box[4], int gif_w, int gif_h, int win_w, int win_h, bool flip_x,
                      int out[4])
{
    if (win_w < 1) win_w = 1;
    if (win_h < 1) win_h = 1;
    if (!box || box[0] >= box[2] || box[1] >= box[3] || gif_w <= 0 || gif_h <= 0) {
        out[0] = out[1] = 0;
        out[2] = win_w;
        out[3] = win_h;
        return;
    }
    /* Box coordinates are non-negative, so integer division rounds down. */
    int l = (int)((int64_t)box[0] * win_w / gif_w);
    int t = (int)((int64_t)box[1] * win_h / gif_h);
    int r = (int)(((int64_t)box[2] * win_w + gif_w - 1) / gif_w);
    int b = (int)(((int64_t)box[3] * win_h + gif_h - 1) / gif_h);
    if (r > win_w) r = win_w;
    if (b > win_h) b = win_h;
    if (l >= win_w) l = win_w - 1;
    if (t >= win_h) t = win_h - 1;
    if (r <= l) r = l + 1;
    if (b <= t) b = t + 1;
    if (flip_x) {
        int nl = win_w - r;
        r = win_w - l;
        l = nl;
    }
    out[0] = l;
    out[1] = t;
    out[2] = r - l;
    out[3] = b - t;
}

void contacts_free(Contacts *c)
{
    free(c->pairs);
    free(c->spare);
    memset(c, 0, sizeof(*c));
}
