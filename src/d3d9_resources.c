#include <stdio.h>
#include "d3d9_internal.h"
#include "d3d9_format.h"

/* =========================================================================
 * Resource Implementations: Surfaces, Buffers, Textures, Declarations
 * ========================================================================= */

/* Weak fallbacks for builds without the Metal backend (HLE selftests). */
__attribute__((weak)) void *hle_d3d9_metal_create_msaa_render_target(uint32_t w, uint32_t h, uint32_t samples) {
    (void)w; (void)h; (void)samples; return NULL;
}
__attribute__((weak)) void hle_d3d9_metal_destroy_msaa_render_target(void *rt) { (void)rt; }
__attribute__((weak)) void hle_d3d9_metal_set_msaa_render_target(void *rt) { (void)rt; }
__attribute__((weak)) void hle_d3d9_metal_resolve_msaa_render_target(void *rt) { (void)rt; }
__attribute__((weak)) void hle_d3d9_metal_set_srgb_write(int enabled) { (void)enabled; }

/* ---- IDirect3DSurface9 ---- */
void surf9_Release(void) {
    uint32_t self = A32(0);
    uint32_t n = --MEM32(self + 4);
    if (!n) {
        d3d9_surface_ctx_t *ctx = (d3d9_surface_ctx_t *)OBJ_CTX(self);
        if (ctx) {
            if (ctx->gpu_rt) {
                if (ctx->msaa_samples) hle_d3d9_metal_destroy_msaa_render_target(ctx->gpu_rt);
                else if (hle_d3d9_gpu && hle_d3d9_gpu->destroy_render_target)
                    hle_d3d9_gpu->destroy_render_target(ctx->gpu_rt);
            }
            if (ctx->pixels) hle_free(ctx->pixels);
            free(ctx);
        }
        d3d9_free_obj(self);
    }
    RET(n, 1);
}

void surf9_GetDesc(void) {
    uint32_t self = A32(0);
    uint32_t pDesc = A32(1);
    if (!pDesc) RET(D3DERR_INVALIDCALL, 2);
    d3d9_surface_ctx_t *ctx = (d3d9_surface_ctx_t *)OBJ_CTX(self);
    memcpy(hle_ptr(pDesc), &ctx->desc, sizeof(D3DSURFACE_DESC));
    RET(D3D_OK, 2);
}

void surf9_LockRect(void) {
    uint32_t self = A32(0);
    uint32_t pLocked = A32(1);
    uint32_t pRect = A32(2);
    (void)pRect;
    if (!pLocked) RET(D3DERR_INVALIDCALL, 4);
    d3d9_surface_ctx_t *ctx = (d3d9_surface_ctx_t *)OBJ_CTX(self);
    ctx->locked = 1;
    D3DLOCKED_RECT *lr = (D3DLOCKED_RECT *)hle_ptr(pLocked);
    lr->Pitch = (int32_t)ctx->pitch;
    lr->pBits = ctx->pixels_va;
    RET(D3D_OK, 4);
}

void surf9_UnlockRect(void) {
    uint32_t self = A32(0);
    d3d9_surface_ctx_t *ctx = (d3d9_surface_ctx_t *)OBJ_CTX(self);
    ctx->locked = 0;
    RET(D3D_OK, 1);
}

void surf9_GetDC(void) { RET(D3DERR_INVALIDCALL, 2); }
void surf9_ReleaseDC(void) { RET(D3D_OK, 2); }
void surf9_GetContainer(void) {
    uint32_t ppContainer = A32(2);
    if (ppContainer) MEM32(ppContainer) = 0;
    RET(D3D_OK, 3);
}

uint32_t format_bpp(uint32_t fmt) {
    switch (fmt) {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_D24S8:
    case D3DFMT_D24X8:
    case D3DFMT_D32:
    case D3DFMT_R32F:
    case D3DFMT_INDEX32:
        return 4;
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_D16:
    case D3DFMT_INDEX16:
    case D3DFMT_G16R16:
    case D3DFMT_R16F:
        return 2;
    case D3DFMT_A8:
    case D3DFMT_L8:
        return 1;
    case D3DFMT_A16B16G16R16:
    case D3DFMT_A16B16G16R16F:
    case D3DFMT_A32B32G32R32F:
        return 16;
    case D3DFMT_DXT1:
        return 8;   /* 8 bytes per 4x4 block */
    case D3DFMT_DXT2:
    case D3DFMT_DXT3:
    case D3DFMT_DXT4:
    case D3DFMT_DXT5:
        return 16;  /* 16 bytes per 4x4 block */
    default:
        return 4;
    }
}

uint32_t create_surface_internal(d3d9_device_ctx_t *dev, uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool) {
    init_d3d9_vtables();
    d3d9_surface_ctx_t *ctx = (d3d9_surface_ctx_t *)calloc(1, sizeof(*ctx));
    ctx->device = dev ? dev->self : 0;
    ctx->desc.Width = w;
    ctx->desc.Height = h;
    ctx->desc.Format = fmt;
    ctx->desc.Usage = usage;
    ctx->desc.Pool = pool;
    ctx->desc.Type = D3DRTYPE_SURFACE;
    ctx->desc.MultiSampleType = 0;
    ctx->desc.MultiSampleQuality = 0;

    uint32_t bpp = format_bpp(fmt);
    if (fmt == D3DFMT_DXT1) {
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
        ctx->pitch = bw * 8;
        uint32_t bytes = ctx->pitch * bh;
        ctx->pixels = (uint8_t *)hle_alloc(bytes ? bytes : 16);
        ctx->pixels_va = hle_va(ctx->pixels);
    } else if (fmt >= D3DFMT_DXT2 && fmt <= D3DFMT_DXT5) {
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
        ctx->pitch = bw * 16;
        uint32_t bytes = ctx->pitch * bh;
        ctx->pixels = (uint8_t *)hle_alloc(bytes ? bytes : 16);
        ctx->pixels_va = hle_va(ctx->pixels);
    } else {
        ctx->pitch = w * bpp;
        uint32_t bytes = ctx->pitch * h;
        ctx->pixels = (uint8_t *)hle_alloc(bytes ? bytes : 16);
        ctx->pixels_va = hle_va(ctx->pixels);
    }

    if ((usage & D3DUSAGE_RENDERTARGET) && hle_d3d9_gpu && hle_d3d9_gpu->create_render_target) {
        ctx->gpu_rt = hle_d3d9_gpu->create_render_target(w, h);
    }

    uint32_t o = hle_com_new(g_vt_surface9, 16);
    ctx->self = o;
    SET_OBJ_CTX(o, ctx);
    return o;
}

void dev9_CreateRenderTarget(void) {
    /* (this, Width, Height, Format, MultiSample, Quality, Lockable, ppSurface, pShared) */
    uint32_t self = A32(0);
    uint32_t w = A32(1), h = A32(2), fmt = A32(3);
    uint32_t ms = A32(4), quality = A32(5), pool = D3DPOOL_DEFAULT;
    uint32_t ppSurf = A32(7);
    if (!ppSurf) RET(D3DERR_INVALIDCALL, 9);
    if (quality != 0u || (ms != 0u && ms != 2u && ms != 4u)) {
        fprintf(stderr, "[d3d9] CreateRenderTarget: unsupported multisample %u/%u\n", ms, quality);
        MEM32(ppSurf) = 0;
        RET(D3DERR_INVALIDCALL, 9);
    }
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    uint32_t s = create_surface_internal(ctx, w, h, fmt, D3DUSAGE_RENDERTARGET, pool);
    if (s && ms) {
        d3d9_surface_ctx_t *sc = (d3d9_surface_ctx_t *)OBJ_CTX(s);
        void *msrt = hle_d3d9_metal_create_msaa_render_target(w, h, ms);
        if (!msrt) {
            fprintf(stderr, "[d3d9] CreateRenderTarget: MSAA unavailable\n");
            /* release the surface we just made */
            uint32_t rel[1] = { s };
            hle_call_guest(MEM32(MEM32(s)), 1, rel);
            MEM32(ppSurf) = 0;
            RET(D3DERR_NOTAVAILABLE, 9);
        }
        if (sc->gpu_rt && hle_d3d9_gpu && hle_d3d9_gpu->destroy_render_target)
            hle_d3d9_gpu->destroy_render_target(sc->gpu_rt);
        sc->gpu_rt = msrt;
        sc->msaa_samples = ms;
        sc->desc.MultiSampleType = ms;
    }
    MEM32(ppSurf) = s;
    RET(D3D_OK, 9);
}

void dev9_CreateDepthStencilSurface(void) {
    /* (this, Width, Height, Format, MultiSample, Quality, Discard, ppSurface, pShared) */
    uint32_t self = A32(0);
    uint32_t w = A32(1), h = A32(2), fmt = A32(3), pool = D3DPOOL_DEFAULT;
    uint32_t ppSurf = A32(7);
    if (!ppSurf) RET(D3DERR_INVALIDCALL, 9);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    uint32_t s = create_surface_internal(ctx, w, h, fmt, D3DUSAGE_DEPTHSTENCIL, pool);
    MEM32(ppSurf) = s;
    RET(D3D_OK, 9);
}

void dev9_CreateOffscreenPlainSurface(void) {
    /* (this, Width, Height, Format, Pool, ppSurface, pShared) */
    uint32_t self = A32(0);
    uint32_t w = A32(1), h = A32(2), fmt = A32(3), pool = A32(4);
    uint32_t ppSurf = A32(5);
    if (!ppSurf) RET(D3DERR_INVALIDCALL, 7);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    uint32_t s = create_surface_internal(ctx, w, h, fmt, 0, pool);
    MEM32(ppSurf) = s;
    RET(D3D_OK, 7);
}

/* ---- IDirect3DVertexBuffer9 ---- */
void vb9_Release(void) {
    uint32_t self = A32(0);
    uint32_t n = --MEM32(self + 4);
    if (!n) {
        d3d9_vb_ctx_t *ctx = (d3d9_vb_ctx_t *)OBJ_CTX(self);
        if (ctx) {
            if (ctx->gpu_buf && hle_d3d9_gpu && hle_d3d9_gpu->destroy_buffer) {
                hle_d3d9_gpu->destroy_buffer(ctx->gpu_buf);
            }
            if (ctx->data) hle_free(ctx->data);
            free(ctx);
        }
        d3d9_free_obj(self);
    }
    RET(n, 1);
}

void vb9_Lock(void) {
    uint32_t self = A32(0);
    uint32_t offset = A32(1);
    uint32_t size = A32(2);
    uint32_t ppbData = A32(3);
    uint32_t flags = A32(4);
    (void)size; (void)flags;
    if (!ppbData) RET(D3DERR_INVALIDCALL, 5);
    d3d9_vb_ctx_t *ctx = (d3d9_vb_ctx_t *)OBJ_CTX(self);
    ctx->locked = 1;
    MEM32(ppbData) = ctx->data_va + offset;
    RET(D3D_OK, 5);
}

void vb9_Unlock(void) {
    uint32_t self = A32(0);
    d3d9_vb_ctx_t *ctx = (d3d9_vb_ctx_t *)OBJ_CTX(self);
    ctx->locked = 0;
    if (ctx->gpu_buf && hle_d3d9_gpu && hle_d3d9_gpu->upload_buffer) {
        hle_d3d9_gpu->upload_buffer(ctx->gpu_buf, 0, ctx->data, ctx->desc.Size);
    }
    RET(D3D_OK, 1);
}

void vb9_GetDesc(void) {
    uint32_t self = A32(0);
    uint32_t pDesc = A32(1);
    if (!pDesc) RET(D3DERR_INVALIDCALL, 2);
    d3d9_vb_ctx_t *ctx = (d3d9_vb_ctx_t *)OBJ_CTX(self);
    memcpy(hle_ptr(pDesc), &ctx->desc, sizeof(D3DVERTEXBUFFER_DESC));
    RET(D3D_OK, 2);
}

void dev9_CreateVertexBuffer(void) {
    /* (this, Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle) */
    uint32_t self = A32(0);
    uint32_t length = A32(1);
    uint32_t usage = A32(2);
    uint32_t fvf = A32(3);
    uint32_t pool = A32(4);
    uint32_t ppVB = A32(5);
    if (!ppVB) RET(D3DERR_INVALIDCALL, 7);

    init_d3d9_vtables();
    d3d9_vb_ctx_t *ctx = (d3d9_vb_ctx_t *)calloc(1, sizeof(*ctx));
    ctx->device = self;
    ctx->desc.Format = D3DFMT_VERTEXDATA;
    ctx->desc.Type = D3DRTYPE_VERTEXBUFFER;
    ctx->desc.Usage = usage;
    ctx->desc.Pool = pool;
    ctx->desc.Size = length;
    ctx->desc.FVF = fvf;

    ctx->data = (uint8_t *)hle_alloc(length ? length : 16);
    ctx->data_va = hle_va(ctx->data);
    if (hle_d3d9_gpu && hle_d3d9_gpu->create_buffer) {
        ctx->gpu_buf = hle_d3d9_gpu->create_buffer(length, 0);
    }

    uint32_t o = hle_com_new(g_vt_vb9, 16);
    ctx->self = o;
    SET_OBJ_CTX(o, ctx);
    MEM32(ppVB) = o;
    RET(D3D_OK, 7);
}

/* ---- IDirect3DIndexBuffer9 ---- */
void ib9_Release(void) {
    uint32_t self = A32(0);
    uint32_t n = --MEM32(self + 4);
    if (!n) {
        d3d9_ib_ctx_t *ctx = (d3d9_ib_ctx_t *)OBJ_CTX(self);
        if (ctx) {
            if (ctx->gpu_buf && hle_d3d9_gpu && hle_d3d9_gpu->destroy_buffer) {
                hle_d3d9_gpu->destroy_buffer(ctx->gpu_buf);
            }
            if (ctx->data) hle_free(ctx->data);
            free(ctx);
        }
        d3d9_free_obj(self);
    }
    RET(n, 1);
}

void ib9_Lock(void) {
    uint32_t self = A32(0);
    uint32_t offset = A32(1);
    uint32_t size = A32(2);
    uint32_t ppbData = A32(3);
    uint32_t flags = A32(4);
    (void)size; (void)flags;
    if (!ppbData) RET(D3DERR_INVALIDCALL, 5);
    d3d9_ib_ctx_t *ctx = (d3d9_ib_ctx_t *)OBJ_CTX(self);
    ctx->locked = 1;
    MEM32(ppbData) = ctx->data_va + offset;
    RET(D3D_OK, 5);
}

void ib9_Unlock(void) {
    uint32_t self = A32(0);
    d3d9_ib_ctx_t *ctx = (d3d9_ib_ctx_t *)OBJ_CTX(self);
    ctx->locked = 0;
    if (ctx->gpu_buf && hle_d3d9_gpu && hle_d3d9_gpu->upload_buffer) {
        hle_d3d9_gpu->upload_buffer(ctx->gpu_buf, 0, ctx->data, ctx->desc.Size);
    }
    RET(D3D_OK, 1);
}

void ib9_GetDesc(void) {
    uint32_t self = A32(0);
    uint32_t pDesc = A32(1);
    if (!pDesc) RET(D3DERR_INVALIDCALL, 2);
    d3d9_ib_ctx_t *ctx = (d3d9_ib_ctx_t *)OBJ_CTX(self);
    memcpy(hle_ptr(pDesc), &ctx->desc, sizeof(D3DINDEXBUFFER_DESC));
    RET(D3D_OK, 2);
}

void dev9_CreateIndexBuffer(void) {
    /* (this, Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle) */
    uint32_t self = A32(0);
    uint32_t length = A32(1);
    uint32_t usage = A32(2);
    uint32_t format = A32(3);
    uint32_t pool = A32(4);
    uint32_t ppIB = A32(5);
    if (!ppIB) RET(D3DERR_INVALIDCALL, 7);

    init_d3d9_vtables();
    d3d9_ib_ctx_t *ctx = (d3d9_ib_ctx_t *)calloc(1, sizeof(*ctx));
    ctx->device = self;
    ctx->desc.Format = format;
    ctx->desc.Type = D3DRTYPE_INDEXBUFFER;
    ctx->desc.Usage = usage;
    ctx->desc.Pool = pool;
    ctx->desc.Size = length;

    ctx->data = (uint8_t *)hle_alloc(length ? length : 16);
    ctx->data_va = hle_va(ctx->data);
    if (hle_d3d9_gpu && hle_d3d9_gpu->create_buffer) {
        ctx->gpu_buf = hle_d3d9_gpu->create_buffer(length, 1);
    }

    uint32_t o = hle_com_new(g_vt_ib9, 16);
    ctx->self = o;
    SET_OBJ_CTX(o, ctx);
    MEM32(ppIB) = o;
    RET(D3D_OK, 7);
}

/* ---- IDirect3DTexture9 ---- */
void tex9_Release(void) {
    uint32_t self = A32(0);
    uint32_t n = --MEM32(self + 4);
    if (!n) {
        d3d9_texture_ctx_t *ctx = (d3d9_texture_ctx_t *)OBJ_CTX(self);
        if (ctx) {
            if (ctx->surfaces) {
                for (uint32_t i = 0; i < ctx->levels; i++) {
                    if (ctx->surfaces[i]) {
                        uint32_t s = ctx->surfaces[i];
                        uint32_t sn = --MEM32(s + 4);
                        if (!sn) {
                            d3d9_surface_ctx_t *sc = (d3d9_surface_ctx_t *)OBJ_CTX(s);
                            if (sc) { if (sc->pixels) hle_free(sc->pixels); free(sc); }
                            d3d9_free_obj(s);
                        }
                    }
                }
                free(ctx->surfaces);
            }
            if (ctx->gpu_tex && hle_d3d9_gpu && hle_d3d9_gpu->destroy_texture) {
                hle_d3d9_gpu->destroy_texture(ctx->gpu_tex);
            }
            free(ctx);
        }
        d3d9_free_obj(self);
    }
    RET(n, 1);
}

void tex9_GetLevelCount(void) {
    uint32_t self = A32(0);
    d3d9_texture_ctx_t *ctx = (d3d9_texture_ctx_t *)OBJ_CTX(self);
    RET(ctx->levels, 1);
}

void tex9_GetLevelDesc(void) {
    uint32_t self = A32(0);
    uint32_t level = A32(1);
    uint32_t pDesc = A32(2);
    if (!pDesc) RET(D3DERR_INVALIDCALL, 3);
    d3d9_texture_ctx_t *ctx = (d3d9_texture_ctx_t *)OBJ_CTX(self);
    if (level < ctx->levels && ctx->surfaces[level]) {
        d3d9_surface_ctx_t *sc = (d3d9_surface_ctx_t *)OBJ_CTX(ctx->surfaces[level]);
        memcpy(hle_ptr(pDesc), &sc->desc, sizeof(D3DSURFACE_DESC));
        RET(D3D_OK, 3);
    }
    RET(D3DERR_INVALIDCALL, 3);
}

void tex9_GetSurfaceLevel(void) {
    uint32_t self = A32(0);
    uint32_t level = A32(1);
    uint32_t ppSurf = A32(2);
    if (!ppSurf) RET(D3DERR_INVALIDCALL, 3);
    d3d9_texture_ctx_t *ctx = (d3d9_texture_ctx_t *)OBJ_CTX(self);
    if (level < ctx->levels && ctx->surfaces[level]) {
        MEM32(ppSurf) = ctx->surfaces[level];
        MEM32(ctx->surfaces[level] + 4)++;
        RET(D3D_OK, 3);
    }
    RET(D3DERR_INVALIDCALL, 3);
}

void tex9_LockRect(void) {
    uint32_t self = A32(0);
    uint32_t level = A32(1);
    uint32_t pLocked = A32(2);
    uint32_t pRect = A32(3);
    uint32_t flags = A32(4);
    d3d9_texture_ctx_t *ctx = (d3d9_texture_ctx_t *)OBJ_CTX(self);
    if (level < ctx->levels && ctx->surfaces[level]) {
        uint32_t a[5] = { ctx->surfaces[level], pLocked, pRect, flags, 0 };
        hle_call_guest(hle_resolve("IDirect3DSurface9::LockRect"), 4, a);
        RET(D3D_OK, 5);
    }
    RET(D3DERR_INVALIDCALL, 5);
}

static int d3d9_is_bc_format(uint32_t fmt) {
    return fmt == D3DFMT_DXT1 || fmt == D3DFMT_DXT2 || fmt == D3DFMT_DXT3 ||
           fmt == D3DFMT_DXT4 || fmt == D3DFMT_DXT5;
}

/* Surface and volume locks advance in whole 4x4 blocks for DXT formats, not
 * in pixels. The block sizes are the public D3DFMT definitions (8 bytes for
 * DXT1, 16 for DXT2..DXT5), the same rule create_surface_internal uses. */
static void volume_lock_pitch(uint32_t fmt, uint32_t w, uint32_t h,
                              uint32_t *row_pitch, uint32_t *slice_pitch) {
    if (d3d9_is_bc_format(fmt)) {
        uint32_t block = fmt == D3DFMT_DXT1 ? 8u : 16u;
        *row_pitch = ((w + 3) / 4) * block;
        *slice_pitch = *row_pitch * ((h + 3) / 4);
    } else {
        *row_pitch = w * format_bpp(fmt);
        *slice_pitch = *row_pitch * h;
    }
}

void tex9_UnlockRect(void) {
    uint32_t self = A32(0);
    uint32_t level = A32(1);
    d3d9_texture_ctx_t *ctx = (d3d9_texture_ctx_t *)OBJ_CTX(self);
    if (level < ctx->levels && ctx->surfaces[level]) {
        d3d9_surface_ctx_t *sc = (d3d9_surface_ctx_t *)OBJ_CTX(ctx->surfaces[level]);
        if (ctx->gpu_tex && sc && sc->pixels && hle_d3d9_gpu && hle_d3d9_gpu->upload_texture) {
            uint32_t w = sc->desc.Width ? sc->desc.Width : 1;
            uint32_t h = sc->desc.Height ? sc->desc.Height : 1;
            if (d3d9_is_bc_format(ctx->format)) {
                /* Upload compressed blocks directly; pitch is bytes per block row. */
                hle_d3d9_gpu->upload_texture(ctx->gpu_tex, 0, (int)level, sc->pixels, sc->pitch);
            } else {
                /* Metal texture is BGRA8 for uncompressed formats we support. */
                void *bgra = hle_alloc(w * 4u * h);
                if (bgra) {
                    if (d3d9_format_to_bgra8(ctx->format, sc->pixels, sc->pitch, w, h,
                                             bgra, w * 4u) == 0)
                        hle_d3d9_gpu->upload_texture(ctx->gpu_tex, 0, (int)level, bgra, w * 4u);
                    hle_free(bgra);
                }
            }
        }
        uint32_t a[2] = { ctx->surfaces[level], 0 };
        hle_call_guest(hle_resolve("IDirect3DSurface9::UnlockRect"), 1, a);
        RET(D3D_OK, 2);
    }
    RET(D3DERR_INVALIDCALL, 2);
}

void tex9_AddDirtyRect(void) {
    /* Public D3D9 docs: AddDirtyRect only records a region for a later
     * UpdateTexture; it has no immediate effect on the image. UpdateTexture
     * is still an honest error here, so OK does not claim a copy happened. */
    RET(D3D_OK, 2);
}

void dev9_CreateTexture(void) {
    /* (this, Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle) */
    uint32_t self = A32(0);
    uint32_t w = A32(1), h = A32(2), levels = A32(3), usage = A32(4), fmt = A32(5), pool = A32(6);
    uint32_t ppTex = A32(7);
    if (!ppTex) RET(D3DERR_INVALIDCALL, 9);

    init_d3d9_vtables();
    uint32_t maxLevels = 1, tw = w ? w : 1, th = h ? h : 1;
    while (tw > 1 || th > 1) { if (tw > 1) tw >>= 1; if (th > 1) th >>= 1; maxLevels++; }
    if (levels == 0 || levels > maxLevels) levels = maxLevels;
    d3d9_texture_ctx_t *ctx = (d3d9_texture_ctx_t *)calloc(1, sizeof(*ctx));
    ctx->device = self;
    ctx->width = w;
    ctx->height = h;
    ctx->levels = levels;
    ctx->usage = usage;
    ctx->format = fmt;
    ctx->pool = pool;
    ctx->surfaces = (uint32_t *)calloc(levels, sizeof(uint32_t));

    d3d9_device_ctx_t *dev = (d3d9_device_ctx_t *)OBJ_CTX(self);
    uint32_t mw = w, mh = h;
    for (uint32_t i = 0; i < levels; i++) {
        ctx->surfaces[i] = create_surface_internal(dev, mw ? mw : 1, mh ? mh : 1, fmt, usage, pool);
        if (ctx->surfaces[i]) {
            d3d9_surface_ctx_t *sc = (d3d9_surface_ctx_t *)OBJ_CTX(ctx->surfaces[i]);
            if (sc) {
                sc->parent_tex = ctx;
                sc->level = i;
            }
        }
        if (mw > 1) mw >>= 1;
        if (mh > 1) mh >>= 1;
    }
    if (hle_d3d9_gpu && hle_d3d9_gpu->create_texture) {
        ctx->gpu_tex = hle_d3d9_gpu->create_texture(w, h, fmt, 0, levels);
    }

    uint32_t o = hle_com_new(g_vt_tex9, 16);
    ctx->self = o;
    SET_OBJ_CTX(o, ctx);
    MEM32(ppTex) = o;
    RET(D3D_OK, 9);
}

/* ---- IDirect3DCubeTexture9 ---- */
void cubetex9_Release(void) {
    uint32_t self = A32(0);
    uint32_t n = --MEM32(self + 4);
    if (!n) {
        d3d9_cubetexture_ctx_t *ctx = (d3d9_cubetexture_ctx_t *)OBJ_CTX(self);
        if (ctx) {
            for (int f = 0; f < 6; f++) {
                for (uint32_t i = 0; i < ctx->levels; i++) {
                    uint32_t s = ctx->surfaces[f][i];
                    if (s) {
                        uint32_t sn = --MEM32(s + 4);
                        if (!sn) {
                            d3d9_surface_ctx_t *sc = (d3d9_surface_ctx_t *)OBJ_CTX(s);
                            if (sc) { if (sc->pixels) hle_free(sc->pixels); free(sc); }
                            d3d9_free_obj(s);
                        }
                    }
                }
            }
            free(ctx);
        }
        d3d9_free_obj(self);
    }
    RET(n, 1);
}

void cubetex9_GetLevelCount(void) {
    uint32_t self = A32(0);
    d3d9_cubetexture_ctx_t *ctx = (d3d9_cubetexture_ctx_t *)OBJ_CTX(self);
    RET(ctx->levels, 1);
}

void cubetex9_GetLevelDesc(void) {
    uint32_t self = A32(0);
    uint32_t level = A32(1);
    uint32_t pDesc = A32(2);
    if (!pDesc) RET(D3DERR_INVALIDCALL, 3);
    d3d9_cubetexture_ctx_t *ctx = (d3d9_cubetexture_ctx_t *)OBJ_CTX(self);
    if (level < ctx->levels && ctx->surfaces[0][level]) {
        d3d9_surface_ctx_t *sc = (d3d9_surface_ctx_t *)OBJ_CTX(ctx->surfaces[0][level]);
        memcpy(hle_ptr(pDesc), &sc->desc, sizeof(D3DSURFACE_DESC));
        RET(D3D_OK, 3);
    }
    RET(D3DERR_INVALIDCALL, 3);
}

void cubetex9_GetCubeMapSurface(void) {
    uint32_t self = A32(0);
    uint32_t face = A32(1);
    uint32_t level = A32(2);
    uint32_t ppSurf = A32(3);
    if (!ppSurf) RET(D3DERR_INVALIDCALL, 4);
    d3d9_cubetexture_ctx_t *ctx = (d3d9_cubetexture_ctx_t *)OBJ_CTX(self);
    if (face < 6 && level < ctx->levels && ctx->surfaces[face][level]) {
        MEM32(ppSurf) = ctx->surfaces[face][level];
        MEM32(ctx->surfaces[face][level] + 4)++;
        RET(D3D_OK, 4);
    }
    RET(D3DERR_INVALIDCALL, 4);
}

void cubetex9_LockRect(void) {
    uint32_t self = A32(0);
    uint32_t face = A32(1);
    uint32_t level = A32(2);
    uint32_t pLocked = A32(3);
    uint32_t pRect = A32(4);
    uint32_t flags = A32(5);
    d3d9_cubetexture_ctx_t *ctx = (d3d9_cubetexture_ctx_t *)OBJ_CTX(self);
    if (face < 6 && level < ctx->levels && ctx->surfaces[face][level]) {
        uint32_t a[5] = { ctx->surfaces[face][level], pLocked, pRect, flags, 0 };
        uint32_t hr = hle_call_guest(hle_resolve("IDirect3DSurface9::LockRect"), 4, a);
        RET(hr, 6);
    }
    RET(D3DERR_INVALIDCALL, 6);
}

void cubetex9_UnlockRect(void) {
    uint32_t self = A32(0);
    uint32_t face = A32(1);
    uint32_t level = A32(2);
    d3d9_cubetexture_ctx_t *ctx = (d3d9_cubetexture_ctx_t *)OBJ_CTX(self);
    if (face < 6 && level < ctx->levels && ctx->surfaces[face][level]) {
        d3d9_surface_ctx_t *sc = (d3d9_surface_ctx_t *)OBJ_CTX(ctx->surfaces[face][level]);
        if (ctx->gpu_tex && sc && sc->pixels && hle_d3d9_gpu && hle_d3d9_gpu->upload_texture) {
            uint32_t w = sc->desc.Width ? sc->desc.Width : 1;
            uint32_t h = sc->desc.Height ? sc->desc.Height : 1;
            if (d3d9_is_bc_format(ctx->format)) {
                hle_d3d9_gpu->upload_texture(ctx->gpu_tex, (int)face, (int)level, sc->pixels, sc->pitch);
            } else {
                void *bgra = hle_alloc(w * 4u * h);
                if (bgra) {
                    if (d3d9_format_to_bgra8(ctx->format, sc->pixels, sc->pitch, w, h,
                                             bgra, w * 4u) == 0)
                        hle_d3d9_gpu->upload_texture(ctx->gpu_tex, (int)face, (int)level, bgra, w * 4u);
                    hle_free(bgra);
                }
            }
        }
        uint32_t a[2] = { ctx->surfaces[face][level], 0 };
        uint32_t hr = hle_call_guest(hle_resolve("IDirect3DSurface9::UnlockRect"), 1, a);
        RET(hr, 3);
    }
    RET(D3DERR_INVALIDCALL, 3);
}

void cubetex9_AddDirtyRect(void) {
    /* Same documented reason as IDirect3DTexture9::AddDirtyRect: a hint for
     * the later UpdateTexture, no immediate image change. */
    RET(D3D_OK, 3);
}

void dev9_CreateCubeTexture(void) {
    /* (this, EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle) */
    uint32_t self = A32(0);
    uint32_t edge = A32(1), levels = A32(2), usage = A32(3), fmt = A32(4), pool = A32(5);
    uint32_t ppCube = A32(6);
    if (!ppCube) RET(D3DERR_INVALIDCALL, 8);

    init_d3d9_vtables();
    uint32_t maxLevels = 1, te = edge ? edge : 1;
    while (te > 1) { te >>= 1; maxLevels++; }
    if (levels == 0 || levels > maxLevels) levels = maxLevels;
    d3d9_cubetexture_ctx_t *ctx = (d3d9_cubetexture_ctx_t *)calloc(1, sizeof(*ctx));
    ctx->device = self;
    ctx->edge_length = edge;
    ctx->levels = levels;
    ctx->usage = usage;
    ctx->format = fmt;
    ctx->pool = pool;

    d3d9_device_ctx_t *dev = (d3d9_device_ctx_t *)OBJ_CTX(self);
    for (int f = 0; f < 6; f++) {
        uint32_t me = edge;
        for (uint32_t i = 0; i < levels; i++) {
            ctx->surfaces[f][i] = create_surface_internal(dev, me ? me : 1, me ? me : 1, fmt, usage, pool);
            if (ctx->surfaces[f][i]) {
                d3d9_surface_ctx_t *sc = (d3d9_surface_ctx_t *)OBJ_CTX(ctx->surfaces[f][i]);
                if (sc) { sc->parent_tex = ctx; sc->level = i; sc->face = (uint32_t)f; }
            }
            if (me > 1) me >>= 1;
        }
    }
    if (hle_d3d9_gpu && hle_d3d9_gpu->create_texture) {
        ctx->gpu_tex = hle_d3d9_gpu->create_texture(edge, edge, fmt, 1, levels);
    }

    uint32_t o = hle_com_new(g_vt_cubetex9, 16);
    ctx->self = o;
    SET_OBJ_CTX(o, ctx);
    MEM32(ppCube) = o;
    RET(D3D_OK, 8);
}

/* ---- IDirect3DVolumeTexture9 & IDirect3DVolume9 ---- */
static uint32_t create_volume_internal(d3d9_device_ctx_t *dev, uint32_t w, uint32_t h, uint32_t d, uint32_t fmt, uint32_t usage, uint32_t pool) {
    init_d3d9_vtables();
    d3d9_volume_ctx_t *ctx = (d3d9_volume_ctx_t *)calloc(1, sizeof(*ctx));
    ctx->device = dev ? dev->self : 0;
    ctx->desc.Width = w;
    ctx->desc.Height = h;
    ctx->desc.Depth = d;
    ctx->desc.Format = fmt;
    ctx->desc.Usage = usage;
    ctx->desc.Pool = pool;
    ctx->desc.Type = D3DRTYPE_VOLUME;

    uint32_t row_pitch, slice_pitch;
    volume_lock_pitch(fmt, w, h, &row_pitch, &slice_pitch);
    ctx->row_pitch = row_pitch;
    ctx->slice_pitch = slice_pitch;
    uint32_t total_bytes = ctx->slice_pitch * d;
    ctx->pixels = (uint8_t *)hle_alloc(total_bytes ? total_bytes : 16);
    ctx->pixels_va = hle_va(ctx->pixels);

    uint32_t o = hle_com_new(g_vt_volume9, 16);
    ctx->self = o;
    SET_OBJ_CTX(o, ctx);
    return o;
}

void vol9_Release(void) {
    uint32_t self = A32(0);
    uint32_t n = --MEM32(self + 4);
    if (!n) {
        d3d9_volume_ctx_t *ctx = (d3d9_volume_ctx_t *)OBJ_CTX(self);
        if (ctx) {
            if (ctx->pixels) hle_free(ctx->pixels);
            free(ctx);
        }
        d3d9_free_obj(self);
    }
    RET(n, 1);
}

void vol9_GetDesc(void) {
    uint32_t self = A32(0);
    uint32_t pDesc = A32(1);
    if (!pDesc) RET(D3DERR_INVALIDCALL, 2);
    d3d9_volume_ctx_t *ctx = (d3d9_volume_ctx_t *)OBJ_CTX(self);
    memcpy(hle_ptr(pDesc), &ctx->desc, sizeof(D3DVOLUME_DESC));
    RET(D3D_OK, 2);
}

void vol9_LockBox(void) {
    uint32_t self = A32(0);
    uint32_t pLocked = A32(1);
    uint32_t pBox = A32(2);
    uint32_t flags = A32(3);
    (void)flags;
    if (!pLocked) RET(D3DERR_INVALIDCALL, 4);
    d3d9_volume_ctx_t *ctx = (d3d9_volume_ctx_t *)OBJ_CTX(self);

    uint8_t *bits = ctx->pixels;
    if (pBox) {
        D3DBOX *b = (D3DBOX *)hle_ptr(pBox);
        /* Right/Bottom/Back are exclusive. Reject empty or out-of-range boxes
         * instead of handing the guest a pointer past its allocation. */
        if (b->Right <= b->Left || b->Bottom <= b->Top || b->Back <= b->Front ||
            b->Right > ctx->desc.Width || b->Bottom > ctx->desc.Height ||
            b->Back > ctx->desc.Depth) {
            RET(D3DERR_INVALIDCALL, 4);
        }
        if (d3d9_is_bc_format(ctx->desc.Format)) {
            uint32_t block = ctx->desc.Format == D3DFMT_DXT1 ? 8u : 16u;
            bits += b->Front * ctx->slice_pitch + (b->Top / 4) * ctx->row_pitch +
                    (b->Left / 4) * block;
        } else {
            uint32_t bpp = format_bpp(ctx->desc.Format);
            bits += b->Front * ctx->slice_pitch + b->Top * ctx->row_pitch + b->Left * bpp;
        }
    }
    ctx->locked = 1;
    D3DLOCKED_BOX *lb = (D3DLOCKED_BOX *)hle_ptr(pLocked);
    lb->RowPitch = (int32_t)ctx->row_pitch;
    lb->SlicePitch = (int32_t)ctx->slice_pitch;
    lb->pBits = hle_va(bits);
    RET(D3D_OK, 4);
}

void vol9_UnlockBox(void) {
    uint32_t self = A32(0);
    d3d9_volume_ctx_t *ctx = (d3d9_volume_ctx_t *)OBJ_CTX(self);
    ctx->locked = 0;
    RET(D3D_OK, 1);
}

void vol9_GetContainer(void) {
    uint32_t self = A32(0);
    uint32_t riid = A32(1);
    uint32_t ppContainer = A32(2);
    (void)riid;
    if (!ppContainer) RET(D3DERR_INVALIDCALL, 3);
    d3d9_volume_ctx_t *ctx = (d3d9_volume_ctx_t *)OBJ_CTX(self);
    if (ctx && ctx->parent_tex) {
        d3d9_volumetexture_ctx_t *parent = (d3d9_volumetexture_ctx_t *)ctx->parent_tex;
        MEM32(ppContainer) = parent->self;
        if (parent->self) MEM32(parent->self + 4)++;
        RET(D3D_OK, 3);
    }
    MEM32(ppContainer) = 0;
    RET(D3DERR_INVALIDCALL, 3);
}

void volumetex9_Release(void) {
    uint32_t self = A32(0);
    uint32_t n = --MEM32(self + 4);
    if (!n) {
        d3d9_volumetexture_ctx_t *ctx = (d3d9_volumetexture_ctx_t *)OBJ_CTX(self);
        if (ctx) {
            if (ctx->gpu_tex && hle_d3d9_gpu && hle_d3d9_gpu->destroy_texture) {
                hle_d3d9_gpu->destroy_texture(ctx->gpu_tex);
            }
            if (ctx->volumes) {
                for (uint32_t i = 0; i < ctx->levels; i++) {
                    uint32_t v = ctx->volumes[i];
                    if (v) {
                        uint32_t vn = --MEM32(v + 4);
                        if (!vn) {
                            d3d9_volume_ctx_t *vc = (d3d9_volume_ctx_t *)OBJ_CTX(v);
                            if (vc) {
                                if (vc->pixels) hle_free(vc->pixels);
                                free(vc);
                            }
                            d3d9_free_obj(v);
                        }
                    }
                }
                free(ctx->volumes);
            }
            free(ctx);
        }
        d3d9_free_obj(self);
    }
    RET(n, 1);
}

void volumetex9_GetLevelCount(void) {
    uint32_t self = A32(0);
    d3d9_volumetexture_ctx_t *ctx = (d3d9_volumetexture_ctx_t *)OBJ_CTX(self);
    RET(ctx->levels, 1);
}

void volumetex9_GetLevelDesc(void) {
    uint32_t self = A32(0);
    uint32_t level = A32(1);
    uint32_t pDesc = A32(2);
    if (!pDesc) RET(D3DERR_INVALIDCALL, 3);
    d3d9_volumetexture_ctx_t *ctx = (d3d9_volumetexture_ctx_t *)OBJ_CTX(self);
    if (level < ctx->levels && ctx->volumes[level]) {
        d3d9_volume_ctx_t *vc = (d3d9_volume_ctx_t *)OBJ_CTX(ctx->volumes[level]);
        memcpy(hle_ptr(pDesc), &vc->desc, sizeof(D3DVOLUME_DESC));
        RET(D3D_OK, 3);
    }
    RET(D3DERR_INVALIDCALL, 3);
}

void volumetex9_GetVolumeLevel(void) {
    uint32_t self = A32(0);
    uint32_t level = A32(1);
    uint32_t ppVolume = A32(2);
    if (!ppVolume) RET(D3DERR_INVALIDCALL, 3);
    d3d9_volumetexture_ctx_t *ctx = (d3d9_volumetexture_ctx_t *)OBJ_CTX(self);
    if (level < ctx->levels && ctx->volumes[level]) {
        MEM32(ppVolume) = ctx->volumes[level];
        MEM32(ctx->volumes[level] + 4)++;
        RET(D3D_OK, 3);
    }
    RET(D3DERR_INVALIDCALL, 3);
}

/* Generic stubs for SetPrivateData, GetPrivateData, FreePrivateData on resources */
void d3d9_stub_set_priv_data(void) {
    /* (this, refguid, pData, SizeOfData, Flags) */
    RET(D3DERR_INVALIDCALL, 5);
}

void d3d9_stub_get_priv_data(void) {
    /* (this, refguid, pData, pSizeOfData) */
    RET(D3DERR_NOTFOUND, 4);
}

void d3d9_stub_free_priv_data(void) {
    /* (this, refguid) */
    RET(D3DERR_NOTFOUND, 2);
}

/* Common resource helper stubs for volume/volumetexture */
void vol9_SetPriority(void) {
    /* (this, PriorityNew) -> returns previous priority (DWORD) */
    RET(0, 2);
}

void vol9_GetPriority(void) {
    /* (this) -> returns priority (DWORD) */
    RET(0, 1);
}

void vol9_PreLoad(void) {
    /* (this) -> void */
    RET(0, 1);
}

void vol9_GetType(void) {
    /* (this) -> D3DRESOURCETYPE */
    RET(D3DRTYPE_VOLUME, 1);
}

void volumetex9_LockBox(void) {
    uint32_t self = A32(0);
    uint32_t level = A32(1);
    uint32_t pLocked = A32(2);
    uint32_t pBox = A32(3);
    uint32_t flags = A32(4);
    if (!pLocked) {
        RET(D3DERR_INVALIDCALL, 5);
    }
    d3d9_volumetexture_ctx_t *ctx = (d3d9_volumetexture_ctx_t *)OBJ_CTX(self);
    if (level < ctx->levels && ctx->volumes[level]) {
        uint32_t a[5] = { ctx->volumes[level], pLocked, pBox, flags, 0 };
        uint32_t hr = hle_call_guest(hle_resolve("IDirect3DVolume9::LockBox"), 4, a);
        RET(hr, 5);
    }
    RET(D3DERR_INVALIDCALL, 5);
}

void volumetex9_UnlockBox(void) {
    uint32_t self = A32(0);
    uint32_t level = A32(1);
    d3d9_volumetexture_ctx_t *ctx = (d3d9_volumetexture_ctx_t *)OBJ_CTX(self);
    if (level < ctx->levels && ctx->volumes[level]) {
        d3d9_volume_ctx_t *vc = (d3d9_volume_ctx_t *)OBJ_CTX(ctx->volumes[level]);
        if (ctx->gpu_tex && vc && vc->pixels && hle_d3d9_gpu && hle_d3d9_gpu->upload_volume_texture) {
            hle_d3d9_gpu->upload_volume_texture(ctx->gpu_tex, (int)level, vc->pixels, vc->row_pitch, vc->slice_pitch);
        }
        uint32_t a[2] = { ctx->volumes[level], 0 };
        uint32_t hr = hle_call_guest(hle_resolve("IDirect3DVolume9::UnlockBox"), 1, a);
        RET(hr, 2);
    }
    RET(D3DERR_INVALIDCALL, 2);
}

void volumetex9_AddDirtyBox(void) {
    RET(D3D_OK, 2);
}

void dev9_CreateVolumeTexture(void) {
    /* (this, Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture, pSharedHandle) */
    uint32_t self = A32(0);
    uint32_t w = A32(1), h = A32(2), d = A32(3), levels = A32(4);
    uint32_t usage = A32(5), fmt = A32(6), pool = A32(7);
    uint32_t ppVolTex = A32(8);
    if (!ppVolTex) {
        fprintf(stderr, "[d3d9] IDirect3DDevice9::CreateVolumeTexture: ppVolumeTexture is NULL\n");
        RET(D3DERR_INVALIDCALL, 10);
    }

    init_d3d9_vtables();
    /* Levels=0 means "as many as the dimensions allow". A larger request is
     * clamped the same way CreateTexture/CreateCubeTexture clamp it and the
     * Metal backend clamps its mipmapLevelCount; otherwise GetVolumeLevel()
     * would hand out levels the GPU texture cannot hold. */
    uint32_t maxLevels = 1, cw = w ? w : 1, ch = h ? h : 1, cd = d ? d : 1;
    while (cw > 1 || ch > 1 || cd > 1) {
        if (cw > 1) cw >>= 1;
        if (ch > 1) ch >>= 1;
        if (cd > 1) cd >>= 1;
        maxLevels++;
    }
    if (levels == 0 || levels > maxLevels) levels = maxLevels;
    d3d9_volumetexture_ctx_t *ctx = (d3d9_volumetexture_ctx_t *)calloc(1, sizeof(*ctx));
    ctx->device = self;
    ctx->width = w;
    ctx->height = h;
    ctx->depth = d;
    ctx->levels = levels;
    ctx->usage = usage;
    ctx->format = fmt;
    ctx->pool = pool;
    ctx->volumes = (uint32_t *)calloc(levels, sizeof(uint32_t));

    d3d9_device_ctx_t *dev = (d3d9_device_ctx_t *)OBJ_CTX(self);
    uint32_t mw = w, mh = h, md = d;
    for (uint32_t i = 0; i < levels; i++) {
        ctx->volumes[i] = create_volume_internal(dev, mw ? mw : 1, mh ? mh : 1, md ? md : 1, fmt, usage, pool);
        if (ctx->volumes[i]) {
            d3d9_volume_ctx_t *vc = (d3d9_volume_ctx_t *)OBJ_CTX(ctx->volumes[i]);
            if (vc) {
                vc->parent_tex = ctx;
                vc->level = i;
            }
        }
        if (mw > 1) mw >>= 1;
        if (mh > 1) mh >>= 1;
        if (md > 1) md >>= 1;
    }

    if (hle_d3d9_gpu && hle_d3d9_gpu->create_volume_texture) {
        ctx->gpu_tex = hle_d3d9_gpu->create_volume_texture(w, h, d, levels, fmt);
    }

    uint32_t o = hle_com_new(g_vt_volumetex9, 16);
    ctx->self = o;
    SET_OBJ_CTX(o, ctx);
    MEM32(ppVolTex) = o;
    RET(D3D_OK, 10);
}

/* ---- IDirect3DVertexDeclaration9 ---- */
void decl9_Release(void) {
    uint32_t self = A32(0);
    uint32_t n = --MEM32(self + 4);
    if (!n) {
        d3d9_decl_ctx_t *ctx = (d3d9_decl_ctx_t *)OBJ_CTX(self);
        if (ctx) free(ctx);
        d3d9_free_obj(self);
    }
    RET(n, 1);
}

void decl9_GetDeclaration(void) {
    uint32_t self = A32(0);
    uint32_t pElements = A32(1);
    uint32_t pNumElements = A32(2);
    d3d9_decl_ctx_t *ctx = (d3d9_decl_ctx_t *)OBJ_CTX(self);
    if (pNumElements) MEM32(pNumElements) = ctx->num_elements;
    if (pElements) memcpy(hle_ptr(pElements), ctx->elements, ctx->num_elements * sizeof(D3DVERTEXELEMENT9));
    RET(D3D_OK, 3);
}

void dev9_CreateVertexDeclaration(void) {
    /* (this, pVertexElements, ppDecl) */
    uint32_t self = A32(0);
    uint32_t pElem = A32(1);
    uint32_t ppDecl = A32(2);
    if (!pElem || !ppDecl) RET(D3DERR_INVALIDCALL, 3);

    init_d3d9_vtables();
    d3d9_decl_ctx_t *ctx = (d3d9_decl_ctx_t *)calloc(1, sizeof(*ctx));
    ctx->device = self;

    const D3DVERTEXELEMENT9 *e = (const D3DVERTEXELEMENT9 *)hle_ptr(pElem);
    uint32_t count = 0;
    while (count < 63 && e[count].Stream != 0xFF) {
        ctx->elements[count] = e[count];
        count++;
    }
    ctx->elements[count] = e[count]; /* copy D3DDECL_END() */
    count++;
    ctx->num_elements = count;

    uint32_t o = hle_com_new(g_vt_decl9, 16);
    ctx->self = o;
    SET_OBJ_CTX(o, ctx);
    MEM32(ppDecl) = o;
    RET(D3D_OK, 3);
}

/* ---- IDirect3DVertexShader9 / IDirect3DPixelShader9 ---- */
void shader9_Release(void) {
    uint32_t self = A32(0);
    uint32_t n = --MEM32(self + 4);
    if (!n) {
        d3d9_shader_ctx_t *ctx = (d3d9_shader_ctx_t *)OBJ_CTX(self);
        if (ctx) {
            if (ctx->tokens) free(ctx->tokens);
            if (ctx->msl) free(ctx->msl);
            if (ctx->metal_pipe && hle_d3d9_gpu && hle_d3d9_gpu->destroy_pipeline) {
                hle_d3d9_gpu->destroy_pipeline(ctx->metal_pipe);
            }
            free(ctx);
        }
        d3d9_free_obj(self);
    }
    RET(n, 1);
}

void shader9_GetFunction(void) {
    uint32_t self = A32(0);
    uint32_t pData = A32(1);
    uint32_t pSize = A32(2);
    d3d9_shader_ctx_t *ctx = (d3d9_shader_ctx_t *)OBJ_CTX(self);
    if (pSize) MEM32(pSize) = ctx->byte_count;
    if (pData && ctx->tokens) memcpy(hle_ptr(pData), ctx->tokens, ctx->byte_count);
    RET(D3D_OK, 3);
}

void dev9_CreateVertexShader(void) {
    /* (this, pFunction, ppShader) */
    uint32_t self = A32(0);
    uint32_t pFunc = A32(1);
    uint32_t ppShader = A32(2);
    if (!pFunc || !ppShader) RET(D3DERR_INVALIDCALL, 3);

    init_d3d9_vtables();
    d3d9_shader_ctx_t *ctx = (d3d9_shader_ctx_t *)calloc(1, sizeof(*ctx));
    ctx->device = self;

    /* Count D3D9 shader tokens until 0x0000FFFF (D3DSIO_END) */
    const uint32_t *tokens = (const uint32_t *)hle_ptr(pFunc);
    uint32_t count = 0;
    while (tokens[count] != 0x0000FFFFu && count < 65536) count++;
    count++;    /* include END token */
    ctx->byte_count = count * 4;
    ctx->tokens = (uint32_t *)malloc(ctx->byte_count);
    memcpy(ctx->tokens, tokens, ctx->byte_count);

    /* Translate to MSL */
    ctx->msl = (char *)malloc(16384);
    char err[256];
    if (d3d9_translate_shader_to_msl(tokens, count, 0, ctx->msl, 16384, err, sizeof(err)) != 0) {
        T("VS translation note: %s\n", err);
    }

    uint32_t o = hle_com_new(g_vt_vs9, 16);
    ctx->self = o;
    SET_OBJ_CTX(o, ctx);
    MEM32(ppShader) = o;
    RET(D3D_OK, 3);
}

void dev9_CreatePixelShader(void) {
    /* (this, pFunction, ppShader) */
    uint32_t self = A32(0);
    uint32_t pFunc = A32(1);
    uint32_t ppShader = A32(2);
    if (!pFunc || !ppShader) RET(D3DERR_INVALIDCALL, 3);

    init_d3d9_vtables();
    d3d9_shader_ctx_t *ctx = (d3d9_shader_ctx_t *)calloc(1, sizeof(*ctx));
    ctx->device = self;

    const uint32_t *tokens = (const uint32_t *)hle_ptr(pFunc);
    uint32_t count = 0;
    while (tokens[count] != 0x0000FFFFu && count < 65536) count++;
    count++;
    ctx->byte_count = count * 4;
    ctx->tokens = (uint32_t *)malloc(ctx->byte_count);
    memcpy(ctx->tokens, tokens, ctx->byte_count);

    /* Translate to MSL */
    ctx->msl = (char *)malloc(16384);
    char err[256];
    if (d3d9_translate_shader_to_msl(tokens, count, 1, ctx->msl, 16384, err, sizeof(err)) != 0) {
        T("PS translation note: %s\n", err);
    }

    uint32_t o = hle_com_new(g_vt_ps9, 16);
    ctx->self = o;
    SET_OBJ_CTX(o, ctx);
    MEM32(ppShader) = o;
    RET(D3D_OK, 3);
}

/* ---- IDirect3DQuery9 ----
 * Slot order and signatures from Wine's public include/d3d9.h:
 *   QueryInterface, AddRef, Release, GetDevice, GetType,
 *   GetDataSize, Issue, GetData  (8 slots).
 * The two supported types (EVENT, OCCLUSION) are served by the GPU backend's
 * query callbacks. Every other public D3DQUERYTYPE is rejected up front with
 * D3DERR_NOTAVAILABLE and a log line -- never a fake success.
 */
static int query_backend_ready(void) {
    return hle_d3d9_gpu && hle_d3d9_gpu->query_create &&
           hle_d3d9_gpu->query_issue && hle_d3d9_gpu->query_get_data &&
           hle_d3d9_gpu->query_destroy;
}
static int query_type_supported(uint32_t type) {
    return type == D3DQUERYTYPE_EVENT || type == D3DQUERYTYPE_OCCLUSION;
}

void query9_Release(void) {
    uint32_t self = A32(0);
    uint32_t n = --MEM32(self + 4);
    if (!n) {
        d3d9_query_ctx_t *ctx = (d3d9_query_ctx_t *)OBJ_CTX(self);
        if (ctx) {
            if (ctx->metal_q && ctx->gpu && ctx->gpu->query_destroy)
                ctx->gpu->query_destroy(ctx->metal_q);
            free(ctx);
        }
        d3d9_free_obj(self);
    }
    RET(n, 1);
}

void query9_GetType(void) {
    uint32_t self = A32(0);
    d3d9_query_ctx_t *ctx = (d3d9_query_ctx_t *)OBJ_CTX(self);
    RET(ctx ? ctx->type : 0u, 1);
}

void query9_GetDataSize(void) {
    /* Public layout: event -> BOOL, occlusion -> DWORD, both 4 bytes. */
    d3d9_query_ctx_t *ctx = (d3d9_query_ctx_t *)OBJ_CTX(A32(0));
    uint32_t size = (ctx && query_type_supported(ctx->type)) ? 4u : 0u;
    RET(size, 1);
}

void query9_Issue(void) {
    uint32_t self = A32(0);
    uint32_t flags = A32(1);
    uint32_t hr = D3DERR_INVALIDCALL;
    d3d9_query_ctx_t *ctx = (d3d9_query_ctx_t *)OBJ_CTX(self);

    if (!ctx || !query_type_supported(ctx->type)) {
        /* invalid object / unsupported type */
    } else if (flags != D3DISSUE_BEGIN && flags != D3DISSUE_END) {
        fprintf(stderr, "[d3d9] IDirect3DQuery9::Issue: invalid flags 0x%X\n", flags);
    } else if (!ctx->metal_q || !query_backend_ready()) {
        fprintf(stderr, "[d3d9] IDirect3DQuery9::Issue: type %u unavailable (no GPU backend)\n",
                ctx->type);
        hr = D3DERR_NOTAVAILABLE;
    } else {
        ctx->issued = 1;
        hr = (hle_d3d9_gpu->query_issue(ctx->metal_q, flags) == 0) ? D3D_OK : D3DERR_INVALIDCALL;
    }
    RET(hr, 2);
}

void query9_GetData(void) {
    uint32_t self = A32(0);
    uint32_t pData = A32(1);
    uint32_t size = A32(2);
    uint32_t flags = A32(3);
    uint32_t hr = D3DERR_INVALIDCALL;
    d3d9_query_ctx_t *ctx = (d3d9_query_ctx_t *)OBJ_CTX(self);

    if (!ctx || !query_type_supported(ctx->type) || !pData || size < 4) {
        /* invalid call */
    } else if (!ctx->metal_q || !query_backend_ready()) {
        fprintf(stderr, "[d3d9] IDirect3DQuery9::GetData: type %u unavailable (no GPU backend)\n",
                ctx->type);
        hr = D3DERR_NOTAVAILABLE;
    } else {
        /* Backend returns 0 (S_OK), 1 (S_FALSE, not ready) or -1 (error). */
        int r = hle_d3d9_gpu->query_get_data(ctx->metal_q, hle_ptr(pData), size, flags);
        hr = (r < 0) ? D3DERR_INVALIDCALL : (uint32_t)r;
    }
    RET(hr, 4);
}

void dev9_CreateQuery(void) {
    /* (this, Type, ppQuery) -- public order from Wine include/d3d9.h. */
    uint32_t self = A32(0);
    uint32_t type = A32(1);
    uint32_t ppQuery = A32(2);
    uint32_t hr = D3DERR_INVALIDCALL;

    if (ppQuery) {
        if (!query_type_supported(type) || !query_backend_ready()) {
            fprintf(stderr, "[d3d9] IDirect3DDevice9::CreateQuery: type %u not available\n", type);
            hr = D3DERR_NOTAVAILABLE;
        } else {
            init_d3d9_vtables();
            d3d9_query_ctx_t *ctx = (d3d9_query_ctx_t *)calloc(1, sizeof(*ctx));
            if (ctx) {
                ctx->device = self;
                ctx->type = type;
                ctx->gpu = hle_d3d9_gpu;
                ctx->metal_q = hle_d3d9_gpu->query_create(type);
                if (!ctx->metal_q) {
                    fprintf(stderr, "[d3d9] CreateQuery: no backend handle for type %u\n", type);
                    free(ctx);
                    hr = D3DERR_OUTOFVIDEOMEMORY;
                } else {
                    uint32_t o = hle_com_new(g_vt_query9, 16);
                    ctx->self = o;
                    SET_OBJ_CTX(o, ctx);
                    MEM32(ppQuery) = o;
                    hr = D3D_OK;
                }
            } else {
                hr = D3DERR_OUTOFVIDEOMEMORY;
            }
        }
    }
    RET(hr, 3);
}

/* ---- IDirect3DBaseTexture9 LOD / auto-gen filter, per texture class ---- */
uint32_t d3d_tex_SetLOD(uint32_t, uint32_t, uint32_t, uint32_t);
uint32_t d3d_tex_GetLOD(uint32_t, uint32_t);
uint32_t d3d_tex_SetFilter(uint32_t, uint32_t, uint32_t);
uint32_t d3d_tex_GetFilter(uint32_t);
void d3d_tex_GenerateMips(uint32_t, uint32_t);
#define BASETEX_FNS(prefix, ctxt) \
void prefix##_SetLOD(void) { ctxt *c = (ctxt *)OBJ_CTX(A32(0)); RET(d3d_tex_SetLOD(A32(0), A32(1), c->levels, c->pool), 2); } \
void prefix##_GetLOD(void) { ctxt *c = (ctxt *)OBJ_CTX(A32(0)); RET(d3d_tex_GetLOD(A32(0), c->pool), 1); } \
void prefix##_SetAutoGenFilterType(void) { ctxt *c = (ctxt *)OBJ_CTX(A32(0)); RET(d3d_tex_SetFilter(A32(0), A32(1), c->usage), 2); } \
void prefix##_GetAutoGenFilterType(void) { RET(d3d_tex_GetFilter(A32(0)), 1); } \
void prefix##_GenerateMipSubLevels(void) { ctxt *c = (ctxt *)OBJ_CTX(A32(0)); d3d_tex_GenerateMips(c->usage, c->levels); RET(0, 1); }
BASETEX_FNS(tex9, d3d9_texture_ctx_t)
BASETEX_FNS(cubetex9, d3d9_cubetexture_ctx_t)
BASETEX_FNS(volumetex9, d3d9_volumetexture_ctx_t)
