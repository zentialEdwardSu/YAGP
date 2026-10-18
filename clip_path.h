#ifndef YAGP_CLIP_PATH_H
#define YAGP_CLIP_PATH_H

#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

/* Resolves a script's relative clip path (UTF-8, '/' or '\' separators)
 * against the folder of the overlay's main GIF. Only that folder and its
 * subfolders are reachable: absolute paths, drive letters, UNC paths, ".."
 * segments, ':' (alternate data streams), wildcards, control characters,
 * device names and segments Windows would silently trim (trailing dots or
 * spaces) are rejected, and the normalised result must still start with the
 * folder. The name must end in ".gif". Returns false on any of these. */
bool clip_resolve_path(const wchar_t *base_gif, const char *rel_utf8, wchar_t *out, size_t cap);

#endif
