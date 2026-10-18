#ifndef YAGP_CLIP_H
#define YAGP_CLIP_H

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

#include "frame_cache.h"
#include "gif_stream.h"
#include "script.h"

/* One GIF an overlay can show: the main GIF ("main") or one a script loaded
 * with yagp.clip. Only the clip on screen has a canvas; the others are
 * "parked" (file mapped, decoder open, 1x1 canvas), so a clip costs almost
 * nothing until it plays. */
typedef struct ClipKey {
    bool enabled;
    uint32_t rgb;
    int tolerance;
} ClipKey;

typedef struct Clip {
    char name[SCRIPT_CLIP_NAME_MAX];
    wchar_t *path;
    GifStream *gif;
    /* From the frame cache (NULL / 0 until it is ready): per-frame change
     * boxes, the frame index and its delays (ms, for yagp.clip_info). */
    uint16_t (*dirty)[4];
    FrameInfo *frames;
    uint32_t *delays;
    int frame_count;
    int opaque[4];          /* Visible area in GIF pixels; empty = unknown. */
    struct CacheJob *job;   /* Build in progress on the worker thread. */
    bool parked;
} Clip;

/* Posted to the tray window by the worker; the handler passes lParam to
 * clip_cache_ready for every clip, then frees it with clip_job_done. */
#define WM_APP_CACHE_READY (WM_APP + 3)

/* Canvas size for a scale: the displayed size below 100%, native above. */
void clip_canvas_size(const GifStream *g, int pct, int *w, int *h);
/* Opens path and decodes frame 0 at the canvas size pct needs. parked =
 * keep only a 1x1 canvas. Starts loading the frame cache. */
bool clip_open(Clip *c, const char *name, const wchar_t *path, const ClipKey *key, int pct, bool parked);
void clip_close(Clip *c);
/* Park / unpark. Unparking restarts at frame 0 (decoded, not presented). */
void clip_park(Clip *c);
bool clip_activate(Clip *c, int pct);
/* New colour key: rebuild the cache; an active clip restarts at frame 0. */
bool clip_rekey(Clip *c, const ClipKey *key, int pct);
/* Canvas for a new scale (active clip). Restarts at frame 0. */
bool clip_resize_canvas(Clip *c, int pct);
/* Adopts job if it belongs to c. Returns true if it did. */
bool clip_cache_ready(Clip *c, void *job);
void clip_job_done(void *job);

#endif
