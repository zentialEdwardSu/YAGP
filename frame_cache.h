#ifndef YAGP_FRAME_CACHE_H
#define YAGP_FRAME_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

/* Per-animation data computed by decoding every frame once:
 *  dirty[i] is the bounding box of pixels that differ between the canvas shown
 *  before frame i and after it. dirty[0] is the loop wrap (last -> first).
 * Stored in <dir>\<hash>.ygc and reused while the GIF file and colour key
 * are unchanged. */
/* Per-frame index, for seeking. offset is GifStream.frame_offset; a keyframe
 * is a frame whose canvas does not depend on any earlier frame, so decoding
 * can start there (gif_stream_seek). Frame 0 always is one. */
#define FRAME_KEYFRAME 1u
typedef struct FrameInfo {
    uint32_t offset;
    uint32_t delay_ms;      /* As played (0/1 cs already mapped to 100 ms). */
    uint32_t flags;         /* FRAME_* */
} FrameInfo;

typedef struct FrameCache {
    int width, height;
    int frame_count;
    uint16_t (*dirty)[4];   /* left, top, right, bottom; left >= right = unchanged. */
    FrameInfo *frames;      /* frame_count entries. */
    /* Bounding box of every pixel that is not transparent in any frame (after
     * the colour key): the GIF's visible area, used as its hitbox. GIF pixels;
     * left >= right = fully transparent. */
    uint16_t opaque[4];
    bool from_disk;         /* Loaded from an existing cache file (vs. just built). */
} FrameCache;

/* Loads the cache for path + colour key, or builds it and (if dir is non-empty)
 * writes it. width/height are the GIF's logical screen size, used as a sanity
 * check. Returns false if the GIF cannot be decoded. */
bool frame_cache_get(const wchar_t *path, bool key_enabled, uint32_t key_rgb, int key_tolerance,
                     int width, int height, const wchar_t *dir, FrameCache *out);
/* Same, but only reads an existing cache file (fast); false if there is none
 * or it is stale. For the UI thread, before handing the build to a worker. */
bool frame_cache_load(const wchar_t *path, bool key_enabled, uint32_t key_rgb, int key_tolerance,
                      int width, int height, const wchar_t *dir, FrameCache *out);
/* Both are thread-safe: they share no state, and may run concurrently with
 * playback of the same file (each opens its own view). */
void frame_cache_free(FrameCache *c);

/* %LOCALAPPDATA%\YAGP\cache */
bool frame_cache_default_dir(wchar_t *out, size_t cap);
/* Removes %LOCALAPPDATA%\YAGP (the cache and its parent). */
bool frame_cache_delete_all(void);

#endif
