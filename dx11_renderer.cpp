#include "dx11_renderer.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <new>

/* rxi/log.h has no extern "C" guard; the C++ standard headers above already
 * pulled in the system ones it includes. */
extern "C" {
#include "log.h"
}

template <typename T> static void release(T *&p)
{
    if (p) { p->Release(); p = nullptr; }
}

struct RenderDevice {
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    IDXGIFactory2 *factory = nullptr;
    IDCompositionDevice *composition = nullptr;
    bool lost = false;
};

struct RenderSurface {
    RenderDevice *dev = nullptr;
    IDXGISwapChain1 *swapchain = nullptr;
    /* With D3D11 flip-model swap chains buffer 0 always aliases the current
     * back buffer, so one cached pointer is enough. */
    ID3D11Texture2D *backbuffer = nullptr;
    IDCompositionTarget *target = nullptr;
    IDCompositionVisual *visual = nullptr;
    IDCompositionMatrixTransform *transform = nullptr; /* Created on first use. */
    IDCompositionEffectGroup *effect = nullptr;        /* Opacity; created on first use. */
    int width = 0, height = 0;
    /* Flip-sequential keeps buffer contents, and the back buffer holds the
     * frame from two presents ago. A partial update must therefore also cover
     * the previous dirty rect. full_pending counts presents that must still be
     * full, because fresh buffers have undefined content. */
    RECT prev_dirty = {};
    int full_pending = 2;
};

static bool is_device_loss(HRESULT hr)
{
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
           hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

static void note_failure(RenderDevice *dev, HRESULT hr)
{
    if (is_device_loss(hr) ||
        (dev->device && FAILED(dev->device->GetDeviceRemovedReason()))) {
        dev->lost = true;
    }
}

extern "C" RenderDevice *render_device_create(void)
{
    RenderDevice *dev = new (std::nothrow) RenderDevice();
    if (!dev) return nullptr;

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };
    /* BGRA support is required for DirectComposition interop. The device is
     * only ever used from the UI thread. */
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED;
    /* WARP first: we only copy CPU pixels into the back buffer and let DWM
     * compose/scale, so a GPU driver buys nothing. Loading one costs 20-60 MB
     * of private memory per process; WARP costs ~1 MB. */
    const D3D_DRIVER_TYPE drivers[] = {D3D_DRIVER_TYPE_WARP, D3D_DRIVER_TYPE_HARDWARE};
    IDXGIDevice *dxgi = nullptr;
    IDXGIAdapter *adapter = nullptr;

    HRESULT hr = E_FAIL;
    for (int i = 0; i < ARRAYSIZE(drivers) && FAILED(hr); ++i) {
        hr = D3D11CreateDevice(nullptr, drivers[i], nullptr, flags,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &dev->device, nullptr, &dev->context);
        if (hr == E_INVALIDARG) { /* Pre-11.1 runtime rejects the 11_1 level. */
            hr = D3D11CreateDevice(nullptr, drivers[i], nullptr, flags,
                levels + 1, ARRAYSIZE(levels) - 1, D3D11_SDK_VERSION,
                &dev->device, nullptr, &dev->context);
        }
        if (FAILED(hr) && drivers[i] == D3D_DRIVER_TYPE_WARP) {
            log_debug("D3D11 WARP device creation failed (hr=0x%08lX)", (unsigned long)hr);
        } else if (SUCCEEDED(hr)) {
            log_debug("using %s D3D11 device",
                      drivers[i] == D3D_DRIVER_TYPE_WARP ? "WARP" : "hardware");
        }
    }
    if (FAILED(hr)) {
        log_error("D3D11 device creation failed (hr=0x%08lX)", (unsigned long)hr);
        goto fail;
    }
    hr = dev->device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi);
    if (FAILED(hr)) {
        log_error("IDXGIDevice query failed (hr=0x%08lX)", (unsigned long)hr);
        goto fail;
    }
    /* Keep at most one queued frame per surface; we never render ahead. */
    {
        IDXGIDevice1 *dxgi1 = nullptr;
        if (SUCCEEDED(dxgi->QueryInterface(__uuidof(IDXGIDevice1), (void **)&dxgi1))) {
            dxgi1->SetMaximumFrameLatency(1);
            dxgi1->Release();
        }
    }
    hr = dxgi->GetAdapter(&adapter);
    if (FAILED(hr)) {
        log_error("DXGI adapter query failed (hr=0x%08lX)", (unsigned long)hr);
        goto fail;
    }
    hr = adapter->GetParent(__uuidof(IDXGIFactory2), (void **)&dev->factory);
    if (FAILED(hr)) {
        log_error("DXGI factory query failed (hr=0x%08lX)", (unsigned long)hr);
        goto fail;
    }
    hr = DCompositionCreateDevice(dxgi, __uuidof(IDCompositionDevice), (void **)&dev->composition);
    if (FAILED(hr)) {
        log_error("DirectComposition device creation failed (hr=0x%08lX)", (unsigned long)hr);
        goto fail;
    }

    release(adapter);
    release(dxgi);
    return dev;

fail:
    release(adapter);
    release(dxgi);
    render_device_destroy(dev);
    return nullptr;
}

extern "C" void render_device_destroy(RenderDevice *dev)
{
    if (!dev) return;
    if (dev->context) dev->context->ClearState();
    release(dev->composition);
    release(dev->factory);
    release(dev->context);
    release(dev->device);
    delete dev;
}

extern "C" bool render_device_lost(const RenderDevice *dev)
{
    return !dev || dev->lost;
}

static bool acquire_backbuffer(RenderSurface *s)
{
    HRESULT hr = s->swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&s->backbuffer);
    if (FAILED(hr)) { note_failure(s->dev, hr); return false; }
    return true;
}

extern "C" RenderSurface *render_surface_create(RenderDevice *dev, HWND hwnd, int width, int height)
{
    if (!dev || dev->lost || !hwnd || width <= 0 || height <= 0) return nullptr;
    RenderSurface *s = new (std::nothrow) RenderSurface();
    if (!s) return nullptr;
    s->dev = dev;
    s->width = width;
    s->height = height;

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = (UINT)width;
    desc.Height = (UINT)height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

    HRESULT hr = dev->factory->CreateSwapChainForComposition(dev->device, &desc, nullptr, &s->swapchain);
    if (FAILED(hr)) goto fail;
    if (!acquire_backbuffer(s)) goto fail;
    hr = dev->composition->CreateTargetForHwnd(hwnd, TRUE, &s->target);
    if (FAILED(hr)) goto fail;
    hr = dev->composition->CreateVisual(&s->visual);
    if (FAILED(hr)) goto fail;
    s->visual->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    hr = s->visual->SetContent(s->swapchain);
    if (FAILED(hr)) goto fail;
    hr = s->target->SetRoot(s->visual);
    if (FAILED(hr)) goto fail;
    hr = dev->composition->Commit();
    if (FAILED(hr)) goto fail;
    return s;

fail:
    note_failure(dev, hr);
    log_error("overlay surface creation failed (hr=0x%08lX)", (unsigned long)hr);
    render_surface_destroy(s);
    return nullptr;
}

extern "C" void render_surface_destroy(RenderSurface *s)
{
    if (!s) return;
    if (s->target) {
        s->target->SetRoot(nullptr);
        if (s->dev && s->dev->composition) s->dev->composition->Commit();
    }
    release(s->effect);
    release(s->transform);
    release(s->visual);
    release(s->target);
    release(s->backbuffer);
    release(s->swapchain);
    delete s;
}

extern "C" bool render_surface_present(RenderSurface *s, const uint8_t *bgra)
{
    if (!s || !bgra || !s->backbuffer || s->dev->lost) return false;
    /* No shader pass: the CPU canvas is already premultiplied BGRA. */
    s->dev->context->UpdateSubresource(s->backbuffer, 0, nullptr, bgra, (UINT)s->width * 4, 0);
    /* Sync interval 0: frame pacing comes from the GIF timer, not vblank. */
    HRESULT hr = s->swapchain->Present(0, 0);
    if (FAILED(hr)) { note_failure(s->dev, hr); return false; }
    if (s->full_pending > 0) --s->full_pending;
    /* Everything changed: the next partial upload must refresh the whole buffer. */
    s->prev_dirty = {0, 0, s->width, s->height};
    return true;
}

extern "C" bool render_surface_present_dirty(RenderSurface *s, const uint8_t *bgra, const RECT *dirty)
{
    if (!s || !bgra || !s->backbuffer || s->dev->lost) return false;
    if (!dirty || s->full_pending > 0) return render_surface_present(s, bgra);
    const RECT bounds = {0, 0, s->width, s->height};
    RECT d;
    IntersectRect(&d, dirty, &bounds);
    /* Identical frame: the front buffer already shows it. Buffers do not
     * rotate, so prev_dirty stays as it is. */
    if (IsRectEmpty(&d)) return true;

    /* Upload only what differs from the buffer being reused. Present1 with
     * dirty rects was measured to cost more than it saves under WARP, so the
     * present itself stays a plain full present. */
    RECT up;
    UnionRect(&up, &d, &s->prev_dirty);
    D3D11_BOX box = {(UINT)up.left, (UINT)up.top, 0, (UINT)up.right, (UINT)up.bottom, 1};
    const uint8_t *src = bgra + ((size_t)up.top * (size_t)s->width + (size_t)up.left) * 4;
    s->dev->context->UpdateSubresource(s->backbuffer, 0, &box, src, (UINT)s->width * 4, 0);
    HRESULT hr = s->swapchain->Present(0, 0);
    if (FAILED(hr)) { note_failure(s->dev, hr); return false; }
    s->prev_dirty = d;
    return true;
}

extern "C" bool render_surface_resize(RenderSurface *s, int width, int height)
{
    if (!s || width <= 0 || height <= 0 || s->dev->lost) return false;
    if (width == s->width && height == s->height) return true;
    release(s->backbuffer);
    s->dev->context->ClearState();
    HRESULT hr = s->swapchain->ResizeBuffers(0, (UINT)width, (UINT)height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) { note_failure(s->dev, hr); return false; }
    s->width = width;
    s->height = height;
    s->full_pending = 2;
    return acquire_backbuffer(s);
}

/* One cubic segment from a to b over duration, then holds b. */
static IDCompositionAnimation *make_animation(RenderDevice *dev, double a, double b, double duration,
                                              const double c[4])
{
    IDCompositionAnimation *anim = nullptr;
    if (FAILED(dev->composition->CreateAnimation(&anim))) return nullptr;
    double d = b - a, t1 = duration, t2 = t1 * t1, t3 = t2 * t1;
    HRESULT hr = anim->AddCubic(0.0, (float)(a + d * c[0]), (float)(d * c[1] / t1), (float)(d * c[2] / t2),
                                (float)(d * c[3] / t3));
    if (SUCCEEDED(hr)) hr = anim->End(duration, (float)b);
    if (FAILED(hr)) release(anim);
    return anim;
}

extern "C" bool render_surface_set_matrix(RenderSurface *s, const double to[6], const double *from,
                                          double duration, const double cubic[4])
{
    if (!s || s->dev->lost) return false;
    RenderDevice *dev = s->dev;
    HRESULT hr = S_OK;
    if (!s->transform) {
        hr = dev->composition->CreateMatrixTransform(&s->transform);
        if (SUCCEEDED(hr)) hr = s->visual->SetTransform(s->transform);
        if (FAILED(hr)) { release(s->transform); note_failure(dev, hr); return false; }
    }
    if (duration <= 0 || !from || !cubic) {
        D2D_MATRIX_3X2_F m = {};
        m._11 = (float)to[0]; m._12 = (float)to[1];
        m._21 = (float)to[2]; m._22 = (float)to[3];
        m._31 = (float)to[4]; m._32 = (float)to[5];
        hr = s->transform->SetMatrix(m); /* Also replaces any running animation. */
    } else {
        for (int i = 0; i < 6 && SUCCEEDED(hr); ++i) {
            IDCompositionAnimation *anim = make_animation(dev, from[i], to[i], duration, cubic);
            hr = anim ? s->transform->SetMatrixElement(i / 2, i % 2, anim) : E_OUTOFMEMORY;
            release(anim); /* The transform keeps its own reference. */
        }
    }
    if (SUCCEEDED(hr)) hr = dev->composition->Commit();
    if (FAILED(hr)) { note_failure(dev, hr); return false; }
    return true;
}

extern "C" bool render_surface_set_opacity(RenderSurface *s, double to, double from, double duration,
                                           const double cubic[4])
{
    if (!s || s->dev->lost) return false;
    RenderDevice *dev = s->dev;
    HRESULT hr = S_OK;
    if (!s->effect) {
        if (to >= 1.0 && duration <= 0) return true; /* Never needed one. */
        hr = dev->composition->CreateEffectGroup(&s->effect);
        if (SUCCEEDED(hr)) hr = s->visual->SetEffect(s->effect);
        if (FAILED(hr)) { release(s->effect); note_failure(dev, hr); return false; }
    }
    if (duration <= 0 || !cubic) {
        hr = s->effect->SetOpacity((float)to);
    } else {
        IDCompositionAnimation *anim = make_animation(dev, from, to, duration, cubic);
        hr = anim ? s->effect->SetOpacity(anim) : E_OUTOFMEMORY;
        release(anim);
    }
    if (SUCCEEDED(hr)) hr = dev->composition->Commit();
    if (FAILED(hr)) { note_failure(dev, hr); return false; }
    return true;
}
