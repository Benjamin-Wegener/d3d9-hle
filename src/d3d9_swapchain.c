/*
 * d3d9_swapchain.c - IDirect3DSwapChain9 implementation for win32hle.
 */
#include "d3d9_internal.h"

uint32_t create_swapchain_internal(d3d9_device_ctx_t *dev) {
    init_d3d9_vtables();
    d3d9_swapchain_ctx_t *ctx = (d3d9_swapchain_ctx_t *)calloc(1, sizeof(*ctx));
    ctx->device = dev ? dev->self : 0;
    uint32_t o = hle_com_new(g_vt_swapchain9, 16);
    ctx->self = o;
    SET_OBJ_CTX(o, ctx);
    T("GetSwapChain: created 0x%08X (device 0x%08X)\n", o, ctx->device);
    return o;
}

void sc9_Release(void) {
    uint32_t self = A32(0);
    uint32_t n = --MEM32(self + 4);
    if (!n) {
        d3d9_swapchain_ctx_t *ctx = (d3d9_swapchain_ctx_t *)OBJ_CTX(self);
        free(ctx);
        d3d9_free_obj(self);
    }
    RET(n, 1);
}

void sc9_Present(void) {
    /* (this, pSrcRect, pDstRect, hDestWindowOverride, pDirtyRegion, dwFlags) */
    if (hle_d3d9_gpu && hle_d3d9_gpu->present) hle_d3d9_gpu->present();
    RET(D3D_OK, 6);
}

void sc9_GetFrontBufferData(void) {
    /* (this, pDestSurface) */
    uint32_t self = A32(0);
    uint32_t pDest = A32(1);
    if (!pDest) {
        fprintf(stderr, "[d3d9] IDirect3DSwapChain9::GetFrontBufferData: pDestSurface is NULL\n");
        RET(D3DERR_INVALIDCALL, 2);
    }
    d3d9_swapchain_ctx_t *ctx = (d3d9_swapchain_ctx_t *)OBJ_CTX(self);
    d3d9_device_ctx_t *dev = ctx ? (d3d9_device_ctx_t *)OBJ_CTX(ctx->device) : NULL;
    d3d9_surface_ctx_t *dst_ctx = (d3d9_surface_ctx_t *)OBJ_CTX(pDest);
    if (!dst_ctx) {
        fprintf(stderr, "[d3d9] IDirect3DSwapChain9::GetFrontBufferData: invalid context\n");
        RET(D3DERR_INVALIDCALL, 2);
    }
    if (dst_ctx->desc.Pool != D3DPOOL_SYSTEMMEM) {
        fprintf(stderr, "[d3d9] IDirect3DSwapChain9::GetFrontBufferData: dest pool %u is not SYSTEMMEM\n", dst_ctx->desc.Pool);
        RET(D3DERR_INVALIDCALL, 2);
    }

    uint32_t w = dst_ctx->desc.Width;
    uint32_t h = dst_ctx->desc.Height;

    /* Ensure default swapchain has a backbuffer. */
    if (ctx && !ctx->backbuffer && dev) ctx->backbuffer = dev->render_target;
    if (ctx && !ctx->pp.BackBufferWidth && dev) ctx->pp = dev->pp;
    uint32_t src = (ctx && ctx->backbuffer) ? ctx->backbuffer : (dev ? dev->render_target : 0);
    d3d9_surface_ctx_t *rt_ctx = src ? (d3d9_surface_ctx_t *)OBJ_CTX(src) : NULL;
    void *rt_handle = rt_ctx ? rt_ctx->gpu_rt : NULL;

    if (hle_d3d9_gpu && hle_d3d9_gpu->read_render_target && dst_ctx->pixels) {
        hle_d3d9_gpu->read_render_target(rt_handle, 0, 0, w, h, dst_ctx->pixels, dst_ctx->pitch);
    } else if (rt_ctx && rt_ctx->pixels && dst_ctx->pixels) {
        for (uint32_t y = 0; y < h && y < rt_ctx->desc.Height; y++) {
            uint32_t copy_bytes = dst_ctx->pitch < rt_ctx->pitch ? dst_ctx->pitch : rt_ctx->pitch;
            memcpy(dst_ctx->pixels + y * dst_ctx->pitch, rt_ctx->pixels + y * rt_ctx->pitch, copy_bytes);
        }
    }
    RET(D3D_OK, 2);
}

void sc9_GetBackBuffer(void) {
    /* (this, iBackBuffer, Type, ppBackBuffer) */
    uint32_t ppBack = A32(3);
    if (!ppBack) RET(D3DERR_INVALIDCALL, 4);
    d3d9_swapchain_ctx_t *ctx = (d3d9_swapchain_ctx_t *)OBJ_CTX(A32(0));
    d3d9_device_ctx_t *dev = ctx ? (d3d9_device_ctx_t *)OBJ_CTX(ctx->device) : NULL;
    uint32_t bb = (ctx && ctx->backbuffer) ? ctx->backbuffer
                : (dev ? dev->render_target : 0);
    MEM32(ppBack) = bb;
    if (bb) MEM32(bb + 4)++;
    RET(D3D_OK, 4);
}

void sc9_GetRasterStatus(void) {
    uint32_t pRaster = A32(1);
    if (pRaster) {
        MEM32(pRaster) = 1;     /* InVBlank */
        MEM32(pRaster + 4) = 0; /* ScanLine */
    }
    RET(D3D_OK, 2);
}

void sc9_GetDisplayMode(void) {
    uint32_t pMode = A32(1);
    if (!pMode) RET(D3DERR_INVALIDCALL, 2);
    d3d9_swapchain_ctx_t *ctx = (d3d9_swapchain_ctx_t *)OBJ_CTX(A32(0));
    d3d9_device_ctx_t *dev = ctx ? (d3d9_device_ctx_t *)OBJ_CTX(ctx->device) : NULL;
    D3DPRESENT_PARAMETERS *pp = (ctx && ctx->pp.BackBufferWidth) ? &ctx->pp : (dev ? &dev->pp : NULL);
    D3DDISPLAYMODE *dm = (D3DDISPLAYMODE *)hle_ptr(pMode);
    dm->Width = (pp && pp->BackBufferWidth) ? pp->BackBufferWidth : D3D9_DEFAULT_WIDTH;
    dm->Height = (pp && pp->BackBufferHeight) ? pp->BackBufferHeight : D3D9_DEFAULT_HEIGHT;
    dm->RefreshRate = 60;
    dm->Format = (pp && pp->BackBufferFormat) ? pp->BackBufferFormat : D3DFMT_X8R8G8B8;
    RET(D3D_OK, 2);
}

void sc9_GetDevice(void) {
    uint32_t ppDev = A32(1);
    if (!ppDev) RET(D3DERR_INVALIDCALL, 2);
    d3d9_swapchain_ctx_t *ctx = (d3d9_swapchain_ctx_t *)OBJ_CTX(A32(0));
    MEM32(ppDev) = ctx ? ctx->device : 0;
    if (ctx && ctx->device) MEM32(ctx->device + 4)++;
    RET(D3D_OK, 2);
}

void sc9_GetPresentParameters(void) {
    uint32_t pParams = A32(1);
    if (!pParams) RET(D3DERR_INVALIDCALL, 2);
    d3d9_swapchain_ctx_t *ctx = (d3d9_swapchain_ctx_t *)OBJ_CTX(A32(0));
    d3d9_device_ctx_t *dev = ctx ? (d3d9_device_ctx_t *)OBJ_CTX(ctx->device) : NULL;
    if (ctx && ctx->pp.BackBufferWidth) memcpy(hle_ptr(pParams), &ctx->pp, sizeof(D3DPRESENT_PARAMETERS));
    else if (dev) memcpy(hle_ptr(pParams), &dev->pp, sizeof(D3DPRESENT_PARAMETERS));
    RET(D3D_OK, 2);
}
