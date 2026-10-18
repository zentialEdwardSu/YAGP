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
/* Sets the visual's transform (D2D row-vector matrix: x' = x*m[0] + y*m[2] +
 * m[4], y' = x*m[1] + y*m[3] + m[5], from swap-chain pixels to window
 * pixels; nearest-neighbour sampling). With duration > 0 DWM animates every
 * element from `from` to `to`, each following the cubic c[0] + c[1]u + c[2]u^2
 * + c[3]u^3 of u = elapsed / duration, starting now: no CPU work per frame. */
bool render_surface_set_matrix(RenderSurface *s, const double to[6], const double *from, double duration,
                               const double cubic[4]);
/* Opacity 0..1, animated like the matrix when duration > 0. */
bool render_surface_set_opacity(RenderSurface *s, double to, double from, double duration, const double cubic[4]);

#ifdef __cplusplus
}
#endif

#endif
