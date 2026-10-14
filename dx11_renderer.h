#ifndef YAGP_DX11_RENDERER_H
#define YAGP_DX11_RENDERER_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The renderer is implemented in C++ because the Windows SDK's dcomp.h has no
 * C bindings. Both handles are opaque to C callers. */
typedef struct RenderDevice RenderDevice;
typedef struct RenderSurface RenderSurface;

/* One D3D11 device + DirectComposition device shared by every overlay. */
RenderDevice *render_device_create(void);
void render_device_destroy(RenderDevice *dev);
/* True once the GPU was removed/reset; every surface must then be recreated. */
bool render_device_lost(const RenderDevice *dev);

/* Composition swap chain bound to hwnd. hwnd must have WS_EX_NOREDIRECTIONBITMAP. */
RenderSurface *render_surface_create(RenderDevice *dev, HWND hwnd, int width, int height);
void render_surface_destroy(RenderSurface *s);
/* Upload a premultiplied BGRA frame straight into the back buffer and present it. */
bool render_surface_present(RenderSurface *s, const uint8_t *bgra);
/* Same, but only `dirty` changed since the previously presented frame. Only the
 * union of the last two dirty rects is uploaded (flip model alternates buffers).
 * An empty rect skips both the upload and the present. */
bool render_surface_present_dirty(RenderSurface *s, const uint8_t *bgra, const RECT *dirty);
/* Resize the swap chain when the GIF is replaced by one with other dimensions. */
bool render_surface_resize(RenderSurface *s, int width, int height);
/* Scale the visual on the DWM side (nearest-neighbour); buffers keep native size. */
bool render_surface_set_scale(RenderSurface *s, float scale);
/* Mirror the visual horizontally (kept across set_scale / resize). */
bool render_surface_set_flip(RenderSurface *s, bool flip_x);

#ifdef __cplusplus
}
#endif

#endif
