#include "d3d9_internal.h"
#include "d3d9_ffp.h"
#include "d3d9_prim.h"
#include "d3d9_metal.h"

/* The Metal implementation lives in d3d9_metal.m, which the mock-GPU selftests
 * do not link. A weak fallback keeps those builds linkable; the real .m
 * definition overrides it in the game. */
__attribute__((weak)) uint64_t hle_d3d9_metal_working_set_bytes(void) { return 0; }
/* Mock-GPU selftests do not link d3d9_metal.m either; keep Present linkable. */
__attribute__((weak)) void hle_d3d9_metal_set_present_params(const int32_t src[4],
                                                             const int32_t dst[4],
                                                             uint32_t fullscreen)
{ (void)src; (void)dst; (void)fullscreen; }

/* D3DRENDERSTATETYPE / D3DBLEND / D3DCULL / D3DCMP values, public D3D9 enums. */
#define D3DRS_ZENABLE         7u
#define D3DRS_ZFUNC          23u
#define D3DRS_ZWRITEENABLE   14u
#define D3DRS_ALPHATESTENABLE 15u
#define D3DRS_ALPHAREF       24u
#define D3DRS_ALPHAFUNC      25u
#define D3DRS_FOGENABLE      28u
#define D3DRS_FOGCOLOR       34u
#define D3DRS_FOGTABLEMODE   35u
#define D3DRS_FOGSTART       36u
#define D3DRS_FOGEND         37u
#define D3DRS_FOGDENSITY     38u
#define D3DRS_SPECULARENABLE 29u
#define D3DRS_LIGHTING       137u
#define D3DRS_AMBIENT        139u
#define D3DRS_FOGVERTEXMODE  140u
#define D3DRS_COLORVERTEX    141u
#define D3DRS_LOCALVIEWER    142u
#define D3DRS_NORMALIZENORMALS 143u
#define D3DRS_DIFFUSEMATERIALSOURCE  145u
#define D3DRS_SPECULARMATERIALSOURCE 146u
#define D3DRS_AMBIENTMATERIALSOURCE  147u
#define D3DRS_EMISSIVEMATERIALSOURCE 148u
#define D3DRS_CULLMODE       22u
#define D3DRS_SRCBLEND       19u
#define D3DRS_DESTBLEND      20u
#define D3DRS_ALPHABLENDENABLE 27u
#define D3DRS_SCISSORTESTENABLE 174u
#define D3DBLEND_SRCALPHA     5u
#define D3DBLEND_INVSRCALPHA  6u
#define D3DCULL_NONE          0u
#define D3DCMP_LESSEQUAL      4u

/* Texture stage state indices */
#define D3DTSS_COLOROP        1u
#define D3DTSS_COLORARG1      2u
#define D3DTSS_COLORARG2      3u
#define D3DTSS_ALPHAOP        4u
#define D3DTSS_ALPHAARG1      5u
#define D3DTSS_ALPHAARG2      6u

/* Transforms */
#define D3DTS_VIEW            2u
#define D3DTS_PROJECTION      3u
#define D3DTS_WORLD           256u

/* Push the device's render states into the GPU backend before a draw. Doing
 * it here also covers states restored by a state block or changed since the
 * last SetRenderState. */
static void apply_render_state(const d3d9_device_ctx_t *ctx) {
    if (!hle_d3d9_gpu || !hle_d3d9_gpu->set_render_state) return;
    const uint32_t *rs = ctx->render_states;
    hle_d3d9_gpu->set_render_state(
        rs[D3DRS_ALPHABLENDENABLE],
        rs[D3DRS_SRCBLEND] ? rs[D3DRS_SRCBLEND] : D3DBLEND_SRCALPHA,
        rs[D3DRS_DESTBLEND] ? rs[D3DRS_DESTBLEND] : D3DBLEND_INVSRCALPHA,
        rs[D3DRS_ZENABLE], rs[D3DRS_ZWRITEENABLE],
        rs[D3DRS_ZFUNC] ? rs[D3DRS_ZFUNC] : D3DCMP_LESSEQUAL,
        rs[D3DRS_CULLMODE], rs[D3DRS_SCISSORTESTENABLE],
        (const int32_t *)&ctx->scissor_rect);
    if (hle_d3d9_gpu->set_clip_planes) {
        uint32_t mask = rs[D3DRS_CLIPPLANEENABLE] & ((1u << D3D9_MAX_CLIP_PLANES) - 1u);
        hle_d3d9_gpu->set_clip_planes((const float (*)[4])ctx->clip_planes, mask);
    }
}

/* Push the shader-facing state into the backend before a draw. Constants are
 * copied (not borrowed) so the backend can upload them to both Metal stages. */
static void apply_shader_resources(const d3d9_device_ctx_t *ctx) {
    if (!hle_d3d9_gpu) return;
    if (hle_d3d9_gpu->set_vertex_decl && ctx->vertex_decl) {
        d3d9_decl_ctx_t *d = (d3d9_decl_ctx_t *)OBJ_CTX(ctx->vertex_decl);
        if (d) hle_d3d9_gpu->set_vertex_decl(d->elements, d->num_elements);
    }
    if (hle_d3d9_gpu->set_shader_constants) {
        hle_d3d9_gpu->set_shader_constants(
            (const float *)ctx->vs_const_f, (const int32_t *)ctx->vs_const_i,
            (const uint32_t *)ctx->vs_const_b,
            (const float *)ctx->ps_const_f, (const int32_t *)ctx->ps_const_i,
            (const uint32_t *)ctx->ps_const_b);
    }
}

/* Return the Metal texture behind a texture or cube-texture COM pointer. */
static void *gpu_texture_of(uint32_t tex) {
    if (!tex) return NULL;
    if (MEM32(tex) == g_vt_cubetex9) {
        d3d9_cubetexture_ctx_t *cc = (d3d9_cubetexture_ctx_t *)OBJ_CTX(tex);
        return cc ? cc->gpu_tex : NULL;
    }
    d3d9_texture_ctx_t *tc = (d3d9_texture_ctx_t *)OBJ_CTX(tex);
    return tc ? tc->gpu_tex : NULL;
}

/* Re-synchronise the GPU backend with the device's current state. Setters
 * already update the backend eagerly, but a StateBlock::Apply changes device
 * state in bulk, so the same state has to be pushed again afterwards. */
void d3d9_device_push_backend(const d3d9_device_ctx_t *ctx) {
    if (!ctx || !hle_d3d9_gpu) return;
    apply_render_state(ctx);
    apply_shader_resources(ctx);
    if (hle_d3d9_gpu->set_render_target) {
        d3d9_surface_ctx_t *sc = ctx->render_target
            ? (d3d9_surface_ctx_t *)OBJ_CTX(ctx->render_target) : NULL;
        if (sc && sc->msaa_samples) hle_d3d9_metal_set_msaa_render_target(sc->gpu_rt);
        else { hle_d3d9_metal_set_msaa_render_target(NULL); hle_d3d9_gpu->set_render_target(sc ? sc->gpu_rt : NULL); }
    }
    if (hle_d3d9_gpu->set_texture) {
        hle_d3d9_gpu->set_texture(gpu_texture_of(ctx->textures[0]));
    }
    if (hle_d3d9_gpu->set_sampler) {
        for (uint32_t i = 0; i < MAX_SAMPLERS; i++) {
            hle_d3d9_gpu->set_sampler(i, gpu_texture_of(ctx->textures[i]));
        }
    }
}

/* =========================================================================
 * IDirect3DDevice9 Implementation
 * ========================================================================= */

void dev9_QueryInterface(void) {
    uint32_t self = A32(0);
    uint32_t ppv = A32(2);
    if (!ppv) RET(D3DERR_INVALIDCALL, 3);
    MEM32(ppv) = self;
    MEM32(self + 4)++;
    RET(D3D_OK, 3);
}

void dev9_Release(void) {
    uint32_t self = A32(0);
    uint32_t n = --MEM32(self + 4);
    if (!n) {
        d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
        if (ctx) {
            if (ctx->recording_state_block) sb9_abort_record(ctx);
            if (ctx->dynamic_ib && hle_d3d9_gpu && hle_d3d9_gpu->destroy_buffer) {
                hle_d3d9_gpu->destroy_buffer(ctx->dynamic_ib);
                ctx->dynamic_ib = NULL;
                ctx->dynamic_ib_size = 0;
            }
            free(ctx);
        }
        d3d9_free_obj(self);
    }
    RET(n, 1);
}

/* IDirect3DDevice9::TestCooperativeLevel: reports the cooperative-level
 * status of the device. The public D3D9 contract distinguishes the operational
 * state (D3D_OK), a lost device that cannot be restored right now
 * (D3DERR_DEVICELOST) and a lost device whose reset is pending
 * (D3DERR_DEVICENOTRESET, after a failed Reset asked the app to release the
 * D3DPOOL_DEFAULT resources and retry). */
void dev9_TestCooperativeLevel(void) {
    uint32_t self = A32(0);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    uint32_t hr = D3D_OK;
    if (!ctx) {
        hr = D3DERR_INVALIDCALL;
    } else {
        switch (ctx->device_state) {
            case D3D9_DEVICE_STATE_LOST:      hr = D3DERR_DEVICELOST; break;
            case D3D9_DEVICE_STATE_NOT_RESET: hr = D3DERR_DEVICENOTRESET; break;
            default:                           hr = D3D_OK; break;
        }
    }
    RET(hr, 1);
}
/* Public D3D9 docs: bytes of texture memory available to the device. The HLE
 * has no texture pool of its own: textures live in the guest heap and Metal's
 * working set is the only real limit. Report the Metal device's public
 * recommendedMaxWorkingSetSize, clamped to the DWORD return type, instead of
 * a fixed 1 GiB. A value of 0 means no Metal device was enabled. */
void dev9_GetAvailableTextureMem(void) {
    uint64_t bytes = hle_d3d9_metal_working_set_bytes();
    if (!bytes) {
        fprintf(stderr, "[d3d9] GetAvailableTextureMem: no Metal device\n");
        RET(0u, 1);
    }
    if (bytes > 0xFFFFFFFFull) bytes = 0xFFFFFFFFull;
    RET((uint32_t)bytes, 1);
}
void dev9_EvictManagedResources(void) { RET(D3D_OK, 1); }

/* This shim serves both IDirect3DDevice9::GetDirect3D and every resource's
 * GetDevice (d3d9.c maps both to it). The two cases are distinguished by the
 * object's vtable: only the device returns its parent IDirect3D9 object, all
 * resources return their owning device. */
void dev9_GetDirect3D(void) {
    uint32_t self = A32(0);
    uint32_t ppOut = A32(1);
    if (!ppOut) RET(D3DERR_INVALIDCALL, 2);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    if (!ctx) RET(D3DERR_INVALIDCALL, 2);

    uint32_t out = ctx->d3d9_obj;
    if (MEM32(self) != g_vt_device9) {
        /* Every resource context starts with { self, device, ... }. */
        struct { uint32_t self; uint32_t device; } *res = (void *)ctx;
        out = res->device;
        d3d9_device_ctx_t *dev = out ? (d3d9_device_ctx_t *)OBJ_CTX(out) : NULL;
        if (dev) out = dev->self;
    }
    MEM32(ppOut) = out;
    if (out) MEM32(out + 4)++;
    RET(D3D_OK, 2);
}

void dev9_GetDeviceCaps(void) {
    uint32_t pCaps = A32(1);
    if (!pCaps) RET(D3DERR_INVALIDCALL, 2);
    D3DCAPS9 *c = (D3DCAPS9 *)hle_ptr(pCaps);
    fill_device_caps(c);
    RET(D3D_OK, 2);
}

void dev9_GetDisplayMode(void) {
    uint32_t pMode = A32(2);
    if (!pMode) RET(D3DERR_INVALIDCALL, 3);
    uint32_t self = A32(0);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);

    D3DDISPLAYMODE *dm = (D3DDISPLAYMODE *)hle_ptr(pMode);
    dm->Width = ctx->pp.BackBufferWidth ? ctx->pp.BackBufferWidth : D3D9_DEFAULT_WIDTH;
    dm->Height = ctx->pp.BackBufferHeight ? ctx->pp.BackBufferHeight : D3D9_DEFAULT_HEIGHT;
    dm->RefreshRate = 60;
    dm->Format = ctx->pp.BackBufferFormat ? ctx->pp.BackBufferFormat : D3DFMT_X8R8G8B8;
    RET(D3D_OK, 3);
}

void dev9_GetCreationParameters(void) {
    uint32_t self = A32(0);
    uint32_t pParams = A32(1);
    if (!pParams) RET(D3DERR_INVALIDCALL, 2);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);

    /* D3DDEVICE_CREATION_PARAMETERS: AdapterOrdinal (4), DeviceType (4), hFocusWindow (4), BehaviorFlags (4) */
    MEM32(pParams) = ctx->adapter;
    MEM32(pParams + 4) = ctx->dev_type;
    MEM32(pParams + 8) = ctx->focus_window;
    MEM32(pParams + 12) = ctx->behavior_flags;
    RET(D3D_OK, 2);
}

/* Public D3D9 docs: SetCursorProperties(XHotSpot, YHotSpot, pCursorBitmap)
 * configures the device's hardware cursor image and returns HRESULT. This HLE
 * has no hardware cursor (the visible pointer is the host SDL cursor), so a
 * valid call cannot be honoured: report NOTAVAILABLE instead of a fake OK. */
void dev9_SetCursorProperties(void) {
    uint32_t pCursorBitmap = A32(3);
    if (!pCursorBitmap) {
        fprintf(stderr, "[d3d9] SetCursorProperties: NULL pCursorBitmap\n");
        RET(D3DERR_INVALIDCALL, 4);
    }
    fprintf(stderr, "[d3d9] SetCursorProperties: no hardware cursor in HLE\n");
    RET(D3DERR_NOTAVAILABLE, 4);
}
void dev9_SetCursorPosition(void) {
    /* Public D3D9 docs: void SetCursorPosition(UINT X, UINT Y, DWORD Flags).
     * The HLE has no device cursor, so this is a documented no-op; EAX is
     * undefined for void and the stdcall pop is 4 args including this. */
    RET(0, 4);
}
void dev9_ShowCursor(void) {
    /* Public D3D9 docs: BOOL ShowCursor(BOOL bShow) returns the PREVIOUS
     * visibility state. Track it per device instead of a constant 1. The host
     * SDL cursor is not changed: only the reported device state is honest. */
    uint32_t self = A32(0), show = A32(1);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    if (!ctx) RET(0, 2);
    uint32_t prev = ctx->cursor_visible;
    ctx->cursor_visible = show ? 1u : 0u;
    RET(prev, 2);
}
static uint32_t get_swapchain_ptr(d3d9_device_ctx_t *ctx, uint32_t i) {
    if (!ctx) return 0;
    if (i == 0) {
        if (!ctx->swapchain) ctx->swapchain = create_swapchain_internal(ctx);
        return ctx->swapchain;
    }
    if (i - 1 < ctx->num_extra) return ctx->extra_swapchains[i - 1];
    return 0;
}
static void ensure_swapchain_defaults(d3d9_device_ctx_t *ctx, uint32_t sc) {
    if (!ctx || !sc) return;
    d3d9_swapchain_ctx_t *sctx = (d3d9_swapchain_ctx_t *)OBJ_CTX(sc);
    if (!sctx) return;
    if (!sctx->backbuffer) {
        if (sc == ctx->swapchain) sctx->backbuffer = ctx->render_target;
    }
    if (!sctx->pp.BackBufferWidth) {
        if (sc == ctx->swapchain) sctx->pp = ctx->pp;
    }
}

void dev9_CreateAdditionalSwapChain(void) {
    /* (this, pPresentParameters, ppSwapChain) */
    uint32_t self = A32(0);
    uint32_t pParams = A32(1);
    uint32_t ppSwap = A32(2);
    if (!pParams || !ppSwap) RET(D3DERR_INVALIDCALL, 3);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    if (!ctx) RET(D3DERR_INVALIDCALL, 3);
    if (ctx->num_extra >= 15) RET(D3DERR_OUTOFVIDEOMEMORY, 3);

    D3DPRESENT_PARAMETERS pp;
    memcpy(&pp, hle_ptr(pParams), sizeof(D3DPRESENT_PARAMETERS));
    uint32_t sc = create_swapchain_internal(ctx);
    d3d9_swapchain_ctx_t *sctx = (d3d9_swapchain_ctx_t *)OBJ_CTX(sc);
    if (sctx) {
        sctx->pp = pp;
        uint32_t w = pp.BackBufferWidth ? pp.BackBufferWidth : D3D9_DEFAULT_WIDTH;
        uint32_t h = pp.BackBufferHeight ? pp.BackBufferHeight : D3D9_DEFAULT_HEIGHT;
        uint32_t fmt = pp.BackBufferFormat ? pp.BackBufferFormat : D3DFMT_A8R8G8B8;
        sctx->backbuffer = create_surface_internal(ctx, w, h, fmt, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT);
    }
    ctx->extra_swapchains[ctx->num_extra++] = sc;
    MEM32(ppSwap) = sc;
    MEM32(sc + 4)++;
    RET(D3D_OK, 3);
}

void dev9_GetSwapChain(void) {
    /* (this, iSwapChain, ppSwapChain) */
    uint32_t self = A32(0);
    uint32_t iSwap = A32(1);
    uint32_t ppSwap = A32(2);
    if (!ppSwap) RET(D3DERR_INVALIDCALL, 3);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    if (!ctx) RET(D3DERR_INVALIDCALL, 3);
    uint32_t total = 1 + ctx->num_extra;
    if (iSwap >= total) {
        MEM32(ppSwap) = 0;
        RET(D3DERR_INVALIDCALL, 3);
    }
    uint32_t sc = get_swapchain_ptr(ctx, iSwap);
    ensure_swapchain_defaults(ctx, sc);
    MEM32(ppSwap) = sc;
    if (sc) MEM32(sc + 4)++;
    RET(D3D_OK, 3);
}

void dev9_GetNumberOfSwapChains(void) {
    uint32_t self = A32(0);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    uint32_t n = ctx ? (1 + ctx->num_extra) : 1;
    RET(n, 1);
}

/* True while the device still holds video-memory (D3DPOOL_DEFAULT) resources
 * the application must release before a Reset can recover: the public D3D9
 * contract makes Reset fail with D3DERR_INVALIDCALL until all D3DPOOL_DEFAULT
 * resources are freed, while D3DPOOL_MANAGED and D3DPOOL_SYSTEMMEM resources
 * are not released by the application and do not block a reset. The
 * device-created implicit back buffer and depth stencil are owned by the
 * device and therefore do not count as application-held resources. */
static int dev9_default_pool_live(const d3d9_device_ctx_t *ctx) {
    if (ctx->depth_stencil && ctx->depth_stencil != ctx->implicit_ds) return 1;
    if (ctx->render_target && ctx->render_target != ctx->implicit_rt) return 1;
    for (uint32_t i = 0; i < MAX_SAMPLERS; i++) {
        uint32_t t = ctx->textures[i];
        if (!t) continue;
        d3d9_texture_ctx_t *tc = (d3d9_texture_ctx_t *)OBJ_CTX(t);
        if (tc && tc->pool == D3DPOOL_DEFAULT) return 1;
    }
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (!ctx->streams[i].vb) continue;
        d3d9_vb_ctx_t *vc = (d3d9_vb_ctx_t *)OBJ_CTX(ctx->streams[i].vb);
        if (vc && vc->desc.Pool == D3DPOOL_DEFAULT) return 1;
    }
    if (ctx->indices) {
        d3d9_ib_ctx_t *ic = (d3d9_ib_ctx_t *)OBJ_CTX(ctx->indices);
        if (ic && ic->desc.Pool == D3DPOOL_DEFAULT) return 1;
    }
    return 0;
}

void dev9_Reset(void) {
    uint32_t self = A32(0);
    uint32_t pParams = A32(1);
    if (!pParams) RET(D3DERR_INVALIDCALL, 2);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    if (!ctx) RET(D3DERR_INVALIDCALL, 2);

    /* A failed Reset puts the device into the lost state (D3DERR_DEVICELOST via
     * TestCooperativeLevel) unless it already sits in the not-reset state. While
     * default-pool resources are still held the Reset fails with
     * D3DERR_INVALIDCALL; managed/systemmem resources may stay allocated. */
    if (dev9_default_pool_live(ctx)) {
        if (ctx->device_state != D3D9_DEVICE_STATE_NOT_RESET) ctx->device_state = D3D9_DEVICE_STATE_LOST;
        RET(D3DERR_INVALIDCALL, 2);
    }

    memcpy(&ctx->pp, hle_ptr(pParams), sizeof(D3DPRESENT_PARAMETERS));

    /* D3D9 resets the viewport and scissor to the new backbuffer on Reset,
     * and the GPU backend must resize its render/depth targets to match. */
    ctx->viewport.X = 0;
    ctx->viewport.Y = 0;
    ctx->viewport.Width = ctx->pp.BackBufferWidth ? ctx->pp.BackBufferWidth : D3D9_DEFAULT_WIDTH;
    ctx->viewport.Height = ctx->pp.BackBufferHeight ? ctx->pp.BackBufferHeight : D3D9_DEFAULT_HEIGHT;
    ctx->scissor_rect.x1 = 0;
    ctx->scissor_rect.y1 = 0;
    ctx->scissor_rect.x2 = (int32_t)ctx->viewport.Width;
    ctx->scissor_rect.y2 = (int32_t)ctx->viewport.Height;

    /* D3D9 Reset clears transforms to identity for all supported slots. */
    for (int st = 0; st < MAX_TEXTURE_STAGES; st++)
        for (int ty = 0; ty < MAX_TSS; ty++)
            ctx->texture_stage_states[st][ty] = 0;
    for (int st = 0; st < MAX_TEXTURE_STAGES; st++)
        ctx->texture_stage_states[st][32] = 0xFFFFFFFFu;
    for (int t = 0; t < 512; t++) {
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                ctx->transforms[t].m[i][j] = (i == j) ? 1.0f : 0.0f;
    }
    /* ... and all render states to their defaults (D3DRS_DEFAULT). */
    memset(ctx->render_states, 0, sizeof(ctx->render_states));
    /* Re-apply the public non-zero defaults (same as CreateDevice). */
    ctx->render_states[D3DRS_COLORWRITEENABLE] = 0xFu;
    ctx->render_states[D3DRS_COLORWRITEENABLE1] = 0xFu;
    ctx->render_states[D3DRS_COLORWRITEENABLE2] = 0xFu;
    ctx->render_states[D3DRS_COLORWRITEENABLE3] = 0xFu;
    if (ctx->dynamic_ib && hle_d3d9_gpu && hle_d3d9_gpu->destroy_buffer) {
        hle_d3d9_gpu->destroy_buffer(ctx->dynamic_ib);
        ctx->dynamic_ib = NULL;
        ctx->dynamic_ib_size = 0;
    }

    if (hle_d3d9_gpu && hle_d3d9_gpu->init_device) {
        hle_d3d9_gpu->init_device(ctx->viewport.Width, ctx->viewport.Height);
        T("Reset: GPU init_device %ux%u\n", ctx->viewport.Width, ctx->viewport.Height);
    }
    /* init_device only resizes the backend-owned backbuffer. The backbuffer
     * surface keeps its own texture, so rebind it or Metal would draw into a
     * different target than GetBackBuffer reports after a Reset. */
    if (hle_d3d9_gpu && hle_d3d9_gpu->set_render_target) {
        d3d9_surface_ctx_t *sc = ctx->render_target ? (d3d9_surface_ctx_t *)OBJ_CTX(ctx->render_target) : NULL;
        if (sc && sc->msaa_samples) hle_d3d9_metal_set_msaa_render_target(sc->gpu_rt);
        else { hle_d3d9_metal_set_msaa_render_target(NULL); hle_d3d9_gpu->set_render_target(sc ? sc->gpu_rt : NULL); }
    }
    T("Reset -> %ux%u format=%u\n", ctx->pp.BackBufferWidth, ctx->pp.BackBufferHeight, ctx->pp.BackBufferFormat);
    ctx->device_state = D3D9_DEVICE_STATE_OK;
    RET(D3D_OK, 2);
}

void dev9_Present(void) {
    /* (this, pSrcRect, pDstRect, hDestWindowOverride, pDirtyRegion)
     * Public D3D9: pSrcRect/pDstRect are RECT {left,top,right,bottom}; NULL
     * means the whole backbuffer / the whole client area. */
    uint32_t pSrc = A32(1), pDst = A32(2), hOverride = A32(3);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(A32(0));
    if (ctx && ctx->device_state != D3D9_DEVICE_STATE_OK) {
        /* A lost or not-reset device fails presenting until it is reset back
         * to the operational state (public D3D9: Present -> D3DERR_DEVICELOST). */
        RET(D3DERR_DEVICELOST, 5);
    }
    {
        int32_t src[4] = { 0, 0, 0, 0 }, dst[4] = { 0, 0, 0, 0 };
        if (pSrc) memcpy(src, hle_ptr(pSrc), sizeof src);
        if (pDst) memcpy(dst, hle_ptr(pDst), sizeof dst);
        uint32_t fullscreen = ctx ? (ctx->pp.Windowed ? 0u : 1u) : 0u;
        hle_d3d9_metal_set_present_params(src, dst, fullscreen);
        (void)hOverride;
    }
    if (hle_d3d9_gpu && hle_d3d9_gpu->present) {
        hle_d3d9_gpu->present();
    }
    RET(D3D_OK, 5);
}

void dev9_GetBackBuffer(void) {
    /* (this, iSwapChain, iBackBuffer, Type, ppBackBuffer) */
    uint32_t self = A32(0);
    uint32_t iSwap = A32(1);
    uint32_t ppBack = A32(4);
    if (!ppBack) RET(D3DERR_INVALIDCALL, 5);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    if (!ctx) RET(D3DERR_INVALIDCALL, 5);
    uint32_t total = 1 + ctx->num_extra;
    if (iSwap >= total) {
        MEM32(ppBack) = 0;
        RET(D3DERR_INVALIDCALL, 5);
    }
    uint32_t sc = get_swapchain_ptr(ctx, iSwap);
    ensure_swapchain_defaults(ctx, sc);
    d3d9_swapchain_ctx_t *sctx = sc ? (d3d9_swapchain_ctx_t *)OBJ_CTX(sc) : NULL;
    uint32_t bb = (sctx && sctx->backbuffer) ? sctx->backbuffer
                : (iSwap == 0 ? ctx->render_target : 0);
    MEM32(ppBack) = bb;
    if (bb) MEM32(bb + 4)++;
    RET(D3D_OK, 5);
}

void dev9_GetRasterStatus(void) {
    uint32_t self = A32(0);
    uint32_t iSwap = A32(1);
    uint32_t pRaster = A32(2);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    uint32_t total = ctx ? (1 + ctx->num_extra) : 1;
    if (iSwap >= total) {
        RET(D3DERR_INVALIDCALL, 3);
    }
    if (pRaster) {
        MEM32(pRaster) = 1;     /* InVBlank = TRUE */
        MEM32(pRaster + 4) = 0; /* ScanLine = 0 */
    }
    RET(D3D_OK, 3);
}

void dev9_SetDialogBoxMode(void) {
    fprintf(stderr, "[d3d9] IDirect3DDevice9::SetDialogBoxMode: not implemented\n");
    RET(D3DERR_NOTAVAILABLE, 2);
}
void dev9_SetGammaRamp(void) {
    /* (this, iSwapChain, Flags, pRamp) - public d3d9.h. */
    uint32_t self = A32(0), swap = A32(1), flags = A32(2), pRamp = A32(3);
    d3d9_device_ctx_t *ctx0 = (d3d9_device_ctx_t *)OBJ_CTX(self);
    uint32_t total = ctx0 ? (1 + ctx0->num_extra) : 1;
    if (!pRamp || swap >= total) {
        fprintf(stderr, "[d3d9] SetGammaRamp: invalid ramp/swap chain (%u, %u)\n", swap, pRamp);
        RET(D3DERR_INVALIDCALL, 4);
    }
    /* D3DSGR_NO_CALIBRATION / D3DSGR_CALIBRATE are the only public flags.
     * No gamma calibrator exists here, so CALIBRATE transfers the supplied
     * ramp unchanged, exactly as the public docs specify for that case.
     * The ramp is applied by the GPU backend at Present; the process display
     * gamma is never touched. */
    if (flags > D3DSGR_CALIBRATE) {
        fprintf(stderr, "[d3d9] SetGammaRamp: unsupported flags 0x%x\n", flags);
        RET(D3DERR_INVALIDCALL, 4);
    }
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    uint8_t *ramp_dst = ctx->gamma_ramp;
    d3d9_swapchain_ctx_t *sctx = NULL;
    if (swap == 0) {
        memcpy(ctx->gamma_ramp, hle_ptr(pRamp), sizeof ctx->gamma_ramp);
    } else {
        uint32_t sc = get_swapchain_ptr(ctx, swap);
        ensure_swapchain_defaults(ctx, sc);
        sctx = sc ? (d3d9_swapchain_ctx_t *)OBJ_CTX(sc) : NULL;
        if (sctx) { memcpy(sctx->gamma_ramp, hle_ptr(pRamp), sizeof sctx->gamma_ramp); ramp_dst = sctx->gamma_ramp; }
    }
    /* Public D3DGAMMARAMP is planar: red[256], green[256], blue[256]. The
     * backend LUT is interleaved RGB per index, so deinterleave here. */
    uint16_t aligned[256][3], raw[768];
    memcpy(raw, ramp_dst, sizeof raw);
    for (unsigned i = 0; i < 256; i++) {
        aligned[i][0] = raw[i];         /* red */
        aligned[i][1] = raw[256 + i];   /* green */
        aligned[i][2] = raw[512 + i];   /* blue */
    }
    if (hle_d3d9_gpu && hle_d3d9_gpu->set_gamma_ramp)
        hle_d3d9_gpu->set_gamma_ramp(aligned);
    else {
        fprintf(stderr, "[d3d9] SetGammaRamp: no GPU backend, ramp cannot be applied\n");
        RET(D3DERR_NOTAVAILABLE, 4);
    }
    RET(D3D_OK, 4);
}
void dev9_GetGammaRamp(void) {
    /* (this, iSwapChain, pRamp) - public d3d9.h. */
    uint32_t self = A32(0), swap = A32(1), pRamp = A32(2);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    uint32_t total = ctx ? (1 + ctx->num_extra) : 1;
    if (!pRamp || swap >= total) {
        fprintf(stderr, "[d3d9] GetGammaRamp: invalid ramp/swap chain (%u, %u)\n", swap, pRamp);
        RET(D3DERR_INVALIDCALL, 3);
    }
    if (swap == 0) {
        memcpy(hle_ptr(pRamp), ctx->gamma_ramp, sizeof ctx->gamma_ramp);
    } else {
        uint32_t sc = get_swapchain_ptr(ctx, swap);
        ensure_swapchain_defaults(ctx, sc);
        d3d9_swapchain_ctx_t *sctx = sc ? (d3d9_swapchain_ctx_t *)OBJ_CTX(sc) : NULL;
        if (sctx) memcpy(hle_ptr(pRamp), sctx->gamma_ramp, sizeof sctx->gamma_ramp);
        else memset(hle_ptr(pRamp), 0, D3D9_GAMMARAMP_BYTES);
    }
    RET(D3D_OK, 3);
}

void dev9_CreateTexture(void);
void dev9_CreateVolumeTexture(void);
void dev9_CreateCubeTexture(void);
void dev9_CreateVertexBuffer(void);
void dev9_CreateIndexBuffer(void);
void dev9_CreateRenderTarget(void);
void dev9_CreateDepthStencilSurface(void);
void dev9_UpdateSurface(void) {
    /* (this, pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint) */
    uint32_t self = A32(0);
    uint32_t pSrc = A32(1);
    uint32_t pSrcRect = A32(2);
    uint32_t pDst = A32(3);
    uint32_t pDestPoint = A32(4);
    (void)self;
    if (!pSrc || !pDst) {
        fprintf(stderr, "[d3d9] IDirect3DDevice9::UpdateSurface: NULL source (%u) or dest (%u)\n", pSrc, pDst);
        RET(D3DERR_INVALIDCALL, 5);
    }
    d3d9_surface_ctx_t *sc = (d3d9_surface_ctx_t *)OBJ_CTX(pSrc);
    d3d9_surface_ctx_t *dc = (d3d9_surface_ctx_t *)OBJ_CTX(pDst);
    if (!sc || !dc) {
        fprintf(stderr, "[d3d9] IDirect3DDevice9::UpdateSurface: invalid surface context\n");
        RET(D3DERR_INVALIDCALL, 5);
    }
    if (sc->desc.Pool != D3DPOOL_SYSTEMMEM || dc->desc.Pool != D3DPOOL_DEFAULT) {
        fprintf(stderr, "[d3d9] IDirect3DDevice9::UpdateSurface: invalid pool (src %u must be SYSTEMMEM 2, dst %u must be DEFAULT 0)\n",
                sc->desc.Pool, dc->desc.Pool);
        RET(D3DERR_INVALIDCALL, 5);
    }
    if (sc->desc.Format != dc->desc.Format) {
        fprintf(stderr, "[d3d9] IDirect3DDevice9::UpdateSurface: format mismatch (%u != %u)\n",
                sc->desc.Format, dc->desc.Format);
        RET(D3DERR_INVALIDCALL, 5);
    }

    int32_t sx1 = 0, sy1 = 0, sx2 = sc->desc.Width, sy2 = sc->desc.Height;
    if (pSrcRect) {
        int32_t *r = (int32_t *)hle_ptr(pSrcRect);
        sx1 = r[0]; sy1 = r[1]; sx2 = r[2]; sy2 = r[3];
    }
    int32_t dx = 0, dy = 0;
    if (pDestPoint) {
        int32_t *pt = (int32_t *)hle_ptr(pDestPoint);
        dx = pt[0]; dy = pt[1];
    }

    if (sx1 < 0 || sy1 < 0 || sx2 > (int32_t)sc->desc.Width || sy2 > (int32_t)sc->desc.Height ||
        sx2 <= sx1 || sy2 <= sy1 ||
        dx < 0 || dy < 0 ||
        (dx + (sx2 - sx1)) > (int32_t)dc->desc.Width ||
        (dy + (sy2 - sy1)) > (int32_t)dc->desc.Height) {
        fprintf(stderr, "[d3d9] IDirect3DDevice9::UpdateSurface: rect/point out of bounds\n");
        RET(D3DERR_INVALIDCALL, 5);
    }

    uint32_t copy_w = sx2 - sx1;
    uint32_t copy_h = sy2 - sy1;
    uint32_t bpp = format_bpp(sc->desc.Format);

    /* If destination has a GPU render target, sync GPU pixels to CPU dc->pixels first */
    if (dc->gpu_rt && dc->pixels && hle_d3d9_gpu && hle_d3d9_gpu->read_render_target) {
        hle_d3d9_gpu->read_render_target(dc->gpu_rt, 0, 0, dc->desc.Width, dc->desc.Height, dc->pixels, dc->pitch);
    }

    if (sc->pixels && dc->pixels) {
        uint32_t row_bytes = copy_w * bpp;
        for (uint32_t y = 0; y < copy_h; y++) {
            uint8_t *src_row = sc->pixels + (sy1 + y) * sc->pitch + sx1 * bpp;
            uint8_t *dst_row = dc->pixels + (dy + y) * dc->pitch + dx * bpp;
            memcpy(dst_row, src_row, row_bytes);
        }
    }

    /* Upload to GPU if dest has GPU texture or GPU render target */
    if (dc->pixels && hle_d3d9_gpu) {
        if (dc->parent_tex && hle_d3d9_gpu->upload_texture) {
            d3d9_texture_ctx_t *pt = (d3d9_texture_ctx_t *)dc->parent_tex;
            if (pt->gpu_tex) {
                uint32_t w = dc->desc.Width ? dc->desc.Width : 1;
                uint32_t h = dc->desc.Height ? dc->desc.Height : 1;
                void *bgra = hle_alloc(w * 4u * h);
                if (!bgra) {
                    fprintf(stderr, "[d3d9] IDirect3DDevice9::UpdateSurface: out of memory allocating conversion buffer\n");
                    RET(D3DERR_OUTOFVIDEOMEMORY, 5);
                }
                if (d3d9_format_to_bgra8(dc->desc.Format, dc->pixels, dc->pitch, w, h, bgra, w * 4u) != 0) {
                    fprintf(stderr, "[d3d9] IDirect3DDevice9::UpdateSurface: format %u cannot be converted to BGRA8 for GPU upload\n", dc->desc.Format);
                    hle_free(bgra);
                    RET(D3DERR_INVALIDCALL, 5);
                }
                hle_d3d9_gpu->upload_texture(pt->gpu_tex, 0, dc->level, bgra, w * 4u);
                hle_free(bgra);
            }
        } else if (dc->gpu_rt && hle_d3d9_gpu->upload_texture) {
            uint32_t w = dc->desc.Width ? dc->desc.Width : 1;
            uint32_t h = dc->desc.Height ? dc->desc.Height : 1;
            void *bgra = hle_alloc(w * 4u * h);
            if (!bgra) {
                fprintf(stderr, "[d3d9] IDirect3DDevice9::UpdateSurface: out of memory allocating conversion buffer\n");
                RET(D3DERR_OUTOFVIDEOMEMORY, 5);
            }
            if (d3d9_format_to_bgra8(dc->desc.Format, dc->pixels, dc->pitch, w, h, bgra, w * 4u) != 0) {
                fprintf(stderr, "[d3d9] IDirect3DDevice9::UpdateSurface: format %u cannot be converted to BGRA8 for GPU upload\n", dc->desc.Format);
                hle_free(bgra);
                RET(D3DERR_INVALIDCALL, 5);
            }
            hle_d3d9_gpu->upload_texture(dc->gpu_rt, 0, 0, bgra, w * 4u);
            hle_free(bgra);
        }
    }

    RET(D3D_OK, 5);
}
/* UpdateTexture requires matching texture types. The three context structs
 * have different layouts, so casting a cube/volume object to the 2D context
 * would read unrelated fields and report a false OK. Public D3D9 docs:
 * UpdateTexture copies a source texture into a destination of the same type,
 * format and dimensions. */
enum d3d9_update_kind { UPD_NONE = 0, UPD_2D, UPD_CUBE, UPD_VOLUME };
static enum d3d9_update_kind update_kind(uint32_t o) {
    if (!o) return UPD_NONE;
    uint32_t vt = MEM32(o);
    if (vt == g_vt_tex9) return UPD_2D;
    if (vt == g_vt_cubetex9) return UPD_CUBE;
    if (vt == g_vt_volumetex9) return UPD_VOLUME;
    return UPD_NONE;
}

static int update_is_bc(uint32_t fmt) {
    return fmt == D3DFMT_DXT1 || fmt == D3DFMT_DXT2 || fmt == D3DFMT_DXT3 ||
           fmt == D3DFMT_DXT4 || fmt == D3DFMT_DXT5;
}

static void update_copy_rows(d3d9_surface_ctx_t *s, d3d9_surface_ctx_t *d) {
    if (!s->pixels || !d->pixels) return;
    uint32_t h = s->desc.Height < d->desc.Height ? s->desc.Height : d->desc.Height;
    uint32_t pitch = s->pitch < d->pitch ? s->pitch : d->pitch;
    for (uint32_t y = 0; y < h; y++) {
        memcpy(d->pixels + y * d->pitch, s->pixels + y * s->pitch, pitch);
    }
}

static void update_upload_surface(void *gpu_tex, uint32_t fmt, int face,
                                  int level, d3d9_surface_ctx_t *d) {
    if (!gpu_tex || !d->pixels || !hle_d3d9_gpu || !hle_d3d9_gpu->upload_texture) return;
    uint32_t w = d->desc.Width ? d->desc.Width : 1;
    uint32_t h = d->desc.Height ? d->desc.Height : 1;
    if (update_is_bc(fmt)) {
        hle_d3d9_gpu->upload_texture(gpu_tex, face, level, d->pixels, d->pitch);
    } else {
        void *bgra = hle_alloc(w * 4u * h);
        if (bgra) {
            if (d3d9_format_to_bgra8(fmt, d->pixels, d->pitch, w, h, bgra, w * 4u) == 0)
                hle_d3d9_gpu->upload_texture(gpu_tex, face, level, bgra, w * 4u);
            hle_free(bgra);
        }
    }
}

void dev9_UpdateTexture(void) {
    /* (this, pSourceTexture, pDestTexture) */
    uint32_t self = A32(0);
    uint32_t pSrc = A32(1);
    uint32_t pDst = A32(2);
    (void)self;
    if (!pSrc || !pDst) RET(D3DERR_INVALIDCALL, 3);
    enum d3d9_update_kind kind = update_kind(pSrc);
    if (kind == UPD_NONE || kind != update_kind(pDst)) {
        fprintf(stderr, "[d3d9] IDirect3DDevice9::UpdateTexture: source/dest type mismatch or unknown object\n");
        RET(D3DERR_INVALIDCALL, 3);
    }

    if (kind == UPD_2D) {
        d3d9_texture_ctx_t *src_ctx = (d3d9_texture_ctx_t *)OBJ_CTX(pSrc);
        d3d9_texture_ctx_t *dst_ctx = (d3d9_texture_ctx_t *)OBJ_CTX(pDst);
        if (!src_ctx || !dst_ctx || src_ctx->format != dst_ctx->format) RET(D3DERR_INVALIDCALL, 3);
        uint32_t copy_levels = src_ctx->levels < dst_ctx->levels ? src_ctx->levels : dst_ctx->levels;
        for (uint32_t i = 0; i < copy_levels; i++) {
            uint32_t a = src_ctx->surfaces ? src_ctx->surfaces[i] : 0;
            uint32_t b = dst_ctx->surfaces ? dst_ctx->surfaces[i] : 0;
            if (!a || !b) continue;
            d3d9_surface_ctx_t *sc = (d3d9_surface_ctx_t *)OBJ_CTX(a);
            d3d9_surface_ctx_t *dc = (d3d9_surface_ctx_t *)OBJ_CTX(b);
            if (!sc || !dc) continue;
            update_copy_rows(sc, dc);
            update_upload_surface(dst_ctx->gpu_tex, dst_ctx->format, 0, (int)i, dc);
        }
        RET(D3D_OK, 3);
    }

    if (kind == UPD_CUBE) {
        d3d9_cubetexture_ctx_t *src_ctx = (d3d9_cubetexture_ctx_t *)OBJ_CTX(pSrc);
        d3d9_cubetexture_ctx_t *dst_ctx = (d3d9_cubetexture_ctx_t *)OBJ_CTX(pDst);
        if (!src_ctx || !dst_ctx || src_ctx->format != dst_ctx->format ||
            src_ctx->edge_length != dst_ctx->edge_length) RET(D3DERR_INVALIDCALL, 3);
        uint32_t copy_levels = src_ctx->levels < dst_ctx->levels ? src_ctx->levels : dst_ctx->levels;
        for (int f = 0; f < 6; f++) {
            for (uint32_t i = 0; i < copy_levels; i++) {
                uint32_t a = src_ctx->surfaces[f][i], b = dst_ctx->surfaces[f][i];
                if (!a || !b) continue;
                d3d9_surface_ctx_t *sc = (d3d9_surface_ctx_t *)OBJ_CTX(a);
                d3d9_surface_ctx_t *dc = (d3d9_surface_ctx_t *)OBJ_CTX(b);
                if (!sc || !dc) continue;
                update_copy_rows(sc, dc);
                update_upload_surface(dst_ctx->gpu_tex, dst_ctx->format, f, (int)i, dc);
            }
        }
        RET(D3D_OK, 3);
    }

    /* Volume textures: copy each mip level slice by slice. */
    {
        d3d9_volumetexture_ctx_t *src_ctx = (d3d9_volumetexture_ctx_t *)OBJ_CTX(pSrc);
        d3d9_volumetexture_ctx_t *dst_ctx = (d3d9_volumetexture_ctx_t *)OBJ_CTX(pDst);
        if (!src_ctx || !dst_ctx || src_ctx->format != dst_ctx->format ||
            src_ctx->width != dst_ctx->width || src_ctx->height != dst_ctx->height ||
            src_ctx->depth != dst_ctx->depth) RET(D3DERR_INVALIDCALL, 3);
        uint32_t copy_levels = src_ctx->levels < dst_ctx->levels ? src_ctx->levels : dst_ctx->levels;
        for (uint32_t i = 0; i < copy_levels; i++) {
            uint32_t a = src_ctx->volumes ? src_ctx->volumes[i] : 0;
            uint32_t b = dst_ctx->volumes ? dst_ctx->volumes[i] : 0;
            if (!a || !b) continue;
            d3d9_volume_ctx_t *sv = (d3d9_volume_ctx_t *)OBJ_CTX(a);
            d3d9_volume_ctx_t *dv = (d3d9_volume_ctx_t *)OBJ_CTX(b);
            if (!sv || !dv || !sv->pixels || !dv->pixels) continue;
            uint32_t rows = sv->desc.Height < dv->desc.Height ? sv->desc.Height : dv->desc.Height;
            uint32_t slices = sv->desc.Depth < dv->desc.Depth ? sv->desc.Depth : dv->desc.Depth;
            uint32_t pitch = sv->row_pitch < dv->row_pitch ? sv->row_pitch : dv->row_pitch;
            for (uint32_t z = 0; z < slices; z++) {
                for (uint32_t y = 0; y < rows; y++) {
                    memcpy(dv->pixels + z * dv->slice_pitch + y * dv->row_pitch,
                           sv->pixels + z * sv->slice_pitch + y * sv->row_pitch, pitch);
                }
            }
            if (dst_ctx->gpu_tex && hle_d3d9_gpu && hle_d3d9_gpu->upload_volume_texture) {
                hle_d3d9_gpu->upload_volume_texture(dst_ctx->gpu_tex, (int)i, dv->pixels,
                                                     dv->row_pitch, dv->slice_pitch);
            }
        }
        RET(D3D_OK, 3);
    }
}

void dev9_GetRenderTargetData(void) {
    /* (this, pRenderTarget, pDestSurface) */
    uint32_t self = A32(0);
    uint32_t pRT = A32(1);
    uint32_t pDest = A32(2);
    (void)self;
    if (!pRT || !pDest) {
        RET(D3DERR_INVALIDCALL, 3);
    }
    d3d9_surface_ctx_t *rt_ctx = (d3d9_surface_ctx_t *)OBJ_CTX(pRT);
    d3d9_surface_ctx_t *dst_ctx = (d3d9_surface_ctx_t *)OBJ_CTX(pDest);
    if (!rt_ctx || !dst_ctx) {
        RET(D3DERR_INVALIDCALL, 3);
    }
    if (rt_ctx->desc.Width != dst_ctx->desc.Width ||
        rt_ctx->desc.Height != dst_ctx->desc.Height ||
        rt_ctx->desc.Format != dst_ctx->desc.Format) {
        RET(D3DERR_INVALIDCALL, 3);
    }

    uint32_t w = rt_ctx->desc.Width;
    uint32_t h = rt_ctx->desc.Height;

    /* If on GPU, read back pixels to destination CPU memory */
    if (hle_d3d9_gpu && hle_d3d9_gpu->read_render_target && dst_ctx->pixels) {
        void *rt_handle = rt_ctx->gpu_rt;
        hle_d3d9_gpu->read_render_target(rt_handle, 0, 0, w, h, dst_ctx->pixels, dst_ctx->pitch);
    } else if (rt_ctx->pixels && dst_ctx->pixels) {
        /* CPU fallback */
        for (uint32_t y = 0; y < h; y++) {
            memcpy(dst_ctx->pixels + y * dst_ctx->pitch, rt_ctx->pixels + y * rt_ctx->pitch, dst_ctx->pitch);
        }
    }
    RET(D3D_OK, 3);
}

void dev9_GetFrontBufferData(void) {
    /* (this, iSwapChain, pDestSurface) */
    uint32_t self = A32(0);
    uint32_t iSwapChain = A32(1);
    uint32_t pDest = A32(2);
    if (!pDest) {
        fprintf(stderr, "[d3d9] IDirect3DDevice9::GetFrontBufferData: pDestSurface is NULL\n");
        RET(D3DERR_INVALIDCALL, 3);
    }
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    uint32_t total = ctx ? (1 + ctx->num_extra) : 1;
    if (iSwapChain >= total) {
        fprintf(stderr, "[d3d9] IDirect3DDevice9::GetFrontBufferData: swapchain index %u >= %u\n", iSwapChain, total);
        RET(D3DERR_INVALIDCALL, 3);
    }
    d3d9_surface_ctx_t *dst_ctx = (d3d9_surface_ctx_t *)OBJ_CTX(pDest);
    if (!ctx || !dst_ctx) {
        fprintf(stderr, "[d3d9] IDirect3DDevice9::GetFrontBufferData: invalid context\n");
        RET(D3DERR_INVALIDCALL, 3);
    }
    if (dst_ctx->desc.Pool != D3DPOOL_SYSTEMMEM) {
        fprintf(stderr, "[d3d9] IDirect3DDevice9::GetFrontBufferData: dest pool %u is not SYSTEMMEM\n", dst_ctx->desc.Pool);
        RET(D3DERR_INVALIDCALL, 3);
    }

    uint32_t w = dst_ctx->desc.Width;
    uint32_t h = dst_ctx->desc.Height;

    /* Front buffer is represented by the swapchain's backbuffer render target */
    uint32_t sc = get_swapchain_ptr(ctx, iSwapChain);
    ensure_swapchain_defaults(ctx, sc);
    d3d9_swapchain_ctx_t *sctx = sc ? (d3d9_swapchain_ctx_t *)OBJ_CTX(sc) : NULL;
    uint32_t src = (sctx && sctx->backbuffer) ? sctx->backbuffer : (iSwapChain == 0 ? ctx->render_target : 0);
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
    RET(D3D_OK, 3);
}

void dev9_StretchRect(void) {
    /* (this, pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter) */
    uint32_t self = A32(0);
    uint32_t pSrc = A32(1);
    uint32_t pSrcRect = A32(2);
    uint32_t pDst = A32(3);
    uint32_t pDstRect = A32(4);
    uint32_t filter = A32(5);
    (void)self;
    (void)filter;
    if (!pSrc || !pDst) {
        RET(D3DERR_INVALIDCALL, 6);
    }
    d3d9_surface_ctx_t *sc = (d3d9_surface_ctx_t *)OBJ_CTX(pSrc);
    d3d9_surface_ctx_t *dc = (d3d9_surface_ctx_t *)OBJ_CTX(pDst);
    if (!sc || !dc) {
        RET(D3DERR_INVALIDCALL, 6);
    }
    if (sc->desc.Format != dc->desc.Format) {
        RET(D3DERR_INVALIDCALL, 6);
    }

    /* Source rect */
    int32_t sx1 = 0, sy1 = 0, sx2 = sc->desc.Width, sy2 = sc->desc.Height;
    if (pSrcRect) {
        int32_t *r = (int32_t *)hle_ptr(pSrcRect);
        sx1 = r[0]; sy1 = r[1]; sx2 = r[2]; sy2 = r[3];
    }
    /* Dest rect */
    int32_t dx1 = 0, dy1 = 0, dx2 = dc->desc.Width, dy2 = dc->desc.Height;
    if (pDstRect) {
        int32_t *r = (int32_t *)hle_ptr(pDstRect);
        dx1 = r[0]; dy1 = r[1]; dx2 = r[2]; dy2 = r[3];
    }

    if (sx1 < 0 || sy1 < 0 || sx2 > (int32_t)sc->desc.Width || sy2 > (int32_t)sc->desc.Height ||
        sx2 <= sx1 || sy2 <= sy1 ||
        dx1 < 0 || dy1 < 0 || dx2 > (int32_t)dc->desc.Width || dy2 > (int32_t)dc->desc.Height ||
        dx2 <= dx1 || dy2 <= dy1) {
        RET(D3DERR_INVALIDCALL, 6);
    }

    uint32_t sw = sx2 - sx1;
    uint32_t sh = sy2 - sy1;
    uint32_t dw = dx2 - dx1;
    uint32_t dh = dy2 - dy1;
    uint32_t bpp = format_bpp(sc->desc.Format);

    /* If source has a GPU render target and dest is CPU only, read GPU pixels first */
    if (sc->gpu_rt && sc->pixels && hle_d3d9_gpu && hle_d3d9_gpu->read_render_target) {
        hle_d3d9_gpu->read_render_target(sc->gpu_rt, 0, 0, sc->desc.Width, sc->desc.Height, sc->pixels, sc->pitch);
    }

    /* CPU blit / stretch */
    if (sc->pixels && dc->pixels) {
        if (sw == dw && sh == dh) {
            /* 1:1 copy */
            uint32_t row_bytes = sw * bpp;
            for (uint32_t y = 0; y < sh; y++) {
                uint8_t *src_row = sc->pixels + (sy1 + y) * sc->pitch + sx1 * bpp;
                uint8_t *dst_row = dc->pixels + (dy1 + y) * dc->pitch + dx1 * bpp;
                memcpy(dst_row, src_row, row_bytes);
            }
        } else {
            /* Nearest neighbor stretch */
            for (uint32_t y = 0; y < dh; y++) {
                uint32_t sy = sy1 + (y * sh) / dh;
                uint8_t *src_row = sc->pixels + sy * sc->pitch;
                uint8_t *dst_row = dc->pixels + (dy1 + y) * dc->pitch + dx1 * bpp;
                for (uint32_t x = 0; x < dw; x++) {
                    uint32_t sx = sx1 + (x * sw) / dw;
                    memcpy(dst_row + x * bpp, src_row + sx * bpp, bpp);
                }
            }
        }
    }

    /* GPU texture blit if both have GPU representations and 1:1 match */
    void *src_gpu = sc->gpu_rt ? sc->gpu_rt : (sc->parent_tex ? ((d3d9_texture_ctx_t *)sc->parent_tex)->gpu_tex : NULL);
    void *dst_gpu = dc->gpu_rt ? dc->gpu_rt : (dc->parent_tex ? ((d3d9_texture_ctx_t *)dc->parent_tex)->gpu_tex : NULL);

    if (src_gpu && dst_gpu && sw == dw && sh == dh &&
        hle_d3d9_gpu && hle_d3d9_gpu->copy_texture_rect) {
        hle_d3d9_gpu->copy_texture_rect(src_gpu, sx1, sy1, dst_gpu, dx1, dy1, sw, sh);
    } else if (dst_gpu && dc->pixels && hle_d3d9_gpu) {
        /* Upload modified CPU pixels to GPU texture/RT */
        if (dc->parent_tex && hle_d3d9_gpu->upload_texture) {
            d3d9_texture_ctx_t *pt = (d3d9_texture_ctx_t *)dc->parent_tex;
            hle_d3d9_gpu->upload_texture(pt->gpu_tex, 0, dc->level, dc->pixels, dc->pitch);
        } else if (dc->gpu_rt && hle_d3d9_gpu->upload_texture) {
            hle_d3d9_gpu->upload_texture(dc->gpu_rt, 0, 0, dc->pixels, dc->pitch);
        }
    }

    RET(D3D_OK, 6);
}
void dev9_ColorFill(void) {
    /* (this, pSurface, pRect, Color) */
    uint32_t self = A32(0);
    uint32_t pSurf = A32(1);
    uint32_t pRect = A32(2);
    uint32_t color = A32(3);
    (void)self;
    if (!pSurf) {
        fprintf(stderr, "[d3d9] IDirect3DDevice9::ColorFill: pSurface is NULL\n");
        RET(D3DERR_INVALIDCALL, 4);
    }
    d3d9_surface_ctx_t *sc = (d3d9_surface_ctx_t *)OBJ_CTX(pSurf);
    if (!sc) {
        fprintf(stderr, "[d3d9] IDirect3DDevice9::ColorFill: invalid surface context\n");
        RET(D3DERR_INVALIDCALL, 4);
    }
    if (sc->desc.Pool != D3DPOOL_DEFAULT) {
        fprintf(stderr, "[d3d9] IDirect3DDevice9::ColorFill: surface pool %u is not D3DPOOL_DEFAULT\n", sc->desc.Pool);
        RET(D3DERR_INVALIDCALL, 4);
    }

    int32_t x1 = 0, y1 = 0, x2 = sc->desc.Width, y2 = sc->desc.Height;
    if (pRect) {
        int32_t *r = (int32_t *)hle_ptr(pRect);
        x1 = r[0]; y1 = r[1]; x2 = r[2]; y2 = r[3];
    }
    if (x1 < 0 || y1 < 0 || x2 > (int32_t)sc->desc.Width || y2 > (int32_t)sc->desc.Height ||
        x2 <= x1 || y2 <= y1) {
        fprintf(stderr, "[d3d9] IDirect3DDevice9::ColorFill: rect out of bounds\n");
        RET(D3DERR_INVALIDCALL, 4);
    }

    uint32_t fill_w = x2 - x1;
    uint32_t fill_h = y2 - y1;
    uint32_t bpp = format_bpp(sc->desc.Format);

    /* If surface has a GPU render target, sync GPU pixels to CPU sc->pixels first */
    if (sc->gpu_rt && sc->pixels && hle_d3d9_gpu && hle_d3d9_gpu->read_render_target) {
        hle_d3d9_gpu->read_render_target(sc->gpu_rt, 0, 0, sc->desc.Width, sc->desc.Height, sc->pixels, sc->pitch);
    }

    /* Fill CPU pixels if present */
    if (sc->pixels) {
        for (uint32_t y = 0; y < fill_h; y++) {
            uint8_t *row = sc->pixels + (y1 + y) * sc->pitch + x1 * bpp;
            if (bpp == 4) {
                uint32_t *row32 = (uint32_t *)row;
                for (uint32_t x = 0; x < fill_w; x++) {
                    row32[x] = color;
                }
            } else if (bpp == 2) {
                uint16_t *row16 = (uint16_t *)row;
                /* Convert ARGB8 to RGB565 */
                uint16_t c16 = (uint16_t)((((color >> 16) & 0xF8) << 8) |
                                          (((color >> 8) & 0xFC) << 3) |
                                          ((color & 0xF8) >> 3));
                for (uint32_t x = 0; x < fill_w; x++) {
                    row16[x] = c16;
                }
            } else if (bpp == 1) {
                uint8_t c8 = (uint8_t)((color >> 8) & 0xFF);
                memset(row, c8, fill_w);
            }
        }
    }

    /* Upload to GPU if dest has GPU texture or GPU render target */
    if (sc->pixels && hle_d3d9_gpu) {
        if (sc->parent_tex && hle_d3d9_gpu->upload_texture) {
            d3d9_texture_ctx_t *pt = (d3d9_texture_ctx_t *)sc->parent_tex;
            if (pt->gpu_tex) {
                uint32_t w = sc->desc.Width ? sc->desc.Width : 1;
                uint32_t h = sc->desc.Height ? sc->desc.Height : 1;
                void *bgra = hle_alloc(w * 4u * h);
                if (!bgra) {
                    fprintf(stderr, "[d3d9] IDirect3DDevice9::ColorFill: out of memory allocating conversion buffer\n");
                    RET(D3DERR_OUTOFVIDEOMEMORY, 4);
                }
                if (d3d9_format_to_bgra8(sc->desc.Format, sc->pixels, sc->pitch, w, h, bgra, w * 4u) != 0) {
                    fprintf(stderr, "[d3d9] IDirect3DDevice9::ColorFill: format %u cannot be converted to BGRA8 for GPU upload\n", sc->desc.Format);
                    hle_free(bgra);
                    RET(D3DERR_INVALIDCALL, 4);
                }
                hle_d3d9_gpu->upload_texture(pt->gpu_tex, 0, sc->level, bgra, w * 4u);
                hle_free(bgra);
            }
        } else if (sc->gpu_rt && hle_d3d9_gpu->upload_texture) {
            uint32_t w = sc->desc.Width ? sc->desc.Width : 1;
            uint32_t h = sc->desc.Height ? sc->desc.Height : 1;
            void *bgra = hle_alloc(w * 4u * h);
            if (!bgra) {
                fprintf(stderr, "[d3d9] IDirect3DDevice9::ColorFill: out of memory allocating conversion buffer\n");
                RET(D3DERR_OUTOFVIDEOMEMORY, 4);
            }
            if (d3d9_format_to_bgra8(sc->desc.Format, sc->pixels, sc->pitch, w, h, bgra, w * 4u) != 0) {
                fprintf(stderr, "[d3d9] IDirect3DDevice9::ColorFill: format %u cannot be converted to BGRA8 for GPU upload\n", sc->desc.Format);
                hle_free(bgra);
                RET(D3DERR_INVALIDCALL, 4);
            }
            hle_d3d9_gpu->upload_texture(sc->gpu_rt, 0, 0, bgra, w * 4u);
            hle_free(bgra);
        }
    }

    RET(D3D_OK, 4);
}
void dev9_CreateOffscreenPlainSurface(void);

void dev9_SetRenderTarget(void) {
    uint32_t self = A32(0);
    uint32_t index = A32(1);
    uint32_t pSurface = A32(2);
    if (index >= 1) {
        fprintf(stderr, "[d3d9] IDirect3DDevice9::SetRenderTarget: index %u >= NumSimultaneousRTs (1)\n", index);
        RET(D3DERR_INVALIDCALL, 3);
    }
    if (!pSurface) {
        fprintf(stderr, "[d3d9] IDirect3DDevice9::SetRenderTarget: pSurface is NULL for index 0\n");
        RET(D3DERR_INVALIDCALL, 3);
    }
    d3d9_surface_ctx_t *sc = (d3d9_surface_ctx_t *)OBJ_CTX(pSurface);
    /* Public SetRenderTarget docs: a non-NULL but invalid render target is
     * D3DERR_INVALIDCALL. A valid target must be a D3DUSAGE_RENDERTARGET
     * surface with nonzero dimensions. With NumSimultaneousRTs == 1 (see
     * d3d9_caps.c) index 0 is the only bindable slot; the MRT rules about
     * equal size / bit depth therefore have no second surface to compare. */
    if (!sc || !(sc->desc.Usage & D3DUSAGE_RENDERTARGET) ||
        !sc->desc.Width || !sc->desc.Height) {
        fprintf(stderr, "[d3d9] IDirect3DDevice9::SetRenderTarget: invalid surface 0x%x\n", pSurface);
        RET(D3DERR_INVALIDCALL, 3);
    }
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    ctx->render_target = pSurface;
    if (hle_d3d9_gpu && hle_d3d9_gpu->set_render_target) {
        if (sc->msaa_samples) hle_d3d9_metal_set_msaa_render_target(sc->gpu_rt);
        else { hle_d3d9_metal_set_msaa_render_target(NULL); hle_d3d9_gpu->set_render_target(sc->gpu_rt); }
    }
    RET(D3D_OK, 3);
}

void dev9_GetRenderTarget(void) {
    uint32_t self = A32(0);
    uint32_t index = A32(1);
    uint32_t ppSurface = A32(2);
    if (!ppSurface) {
        fprintf(stderr, "[d3d9] IDirect3DDevice9::GetRenderTarget: ppRenderTarget is NULL\n");
        RET(D3DERR_INVALIDCALL, 3);
    }
    if (index >= 1) {
        fprintf(stderr, "[d3d9] IDirect3DDevice9::GetRenderTarget: index %u >= NumSimultaneousRTs (1)\n", index);
        RET(D3DERR_INVALIDCALL, 3);
    }
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    MEM32(ppSurface) = ctx->render_target;
    if (ctx->render_target) MEM32(ctx->render_target + 4)++;
    RET(D3D_OK, 3);
}

void dev9_SetDepthStencilSurface(void) {
    uint32_t self = A32(0);
    uint32_t pSurface = A32(1);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    ctx->depth_stencil = pSurface;
    RET(D3D_OK, 2);
}

void dev9_GetDepthStencilSurface(void) {
    uint32_t self = A32(0);
    uint32_t ppSurface = A32(1);
    if (!ppSurface) RET(D3DERR_INVALIDCALL, 2);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    MEM32(ppSurface) = ctx->depth_stencil;
    /* Public docs: D3DERR_NOTFOUND when the device has no depth-stencil
     * surface; a D3D_OK with NULL would be a false success. */
    if (!ctx->depth_stencil) RET(D3DERR_NOTFOUND, 2);
    MEM32(ctx->depth_stencil + 4)++;
    RET(D3D_OK, 2);
}

void dev9_BeginScene(void) {
    uint32_t self = A32(0);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    ctx->in_scene = 1;
    if (hle_d3d9_gpu && hle_d3d9_gpu->begin_scene) {
        hle_d3d9_gpu->begin_scene();
    }
    RET(D3D_OK, 1);
}

void dev9_EndScene(void) {
    uint32_t self = A32(0);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    ctx->in_scene = 0;
    if (hle_d3d9_gpu && hle_d3d9_gpu->end_scene) {
        hle_d3d9_gpu->end_scene();
    }
    RET(D3D_OK, 1);
}

void dev9_Clear(void) {
    /* (this, Count, pRects, Flags, Color, Z, Stencil) */
    uint32_t flags = A32(3);
    uint32_t color = A32(4);
    uint32_t z_raw = A32(5);
    uint32_t stencil = A32(6);
    float z;
    memcpy(&z, &z_raw, 4);

    if (hle_d3d9_gpu && hle_d3d9_gpu->clear) {
        hle_d3d9_gpu->clear(flags, color, z, stencil);
    }
    RET(D3D_OK, 7);
}

void dev9_SetTransform(void) {
    uint32_t self = A32(0);
    uint32_t state = A32(1);
    uint32_t pMatrix = A32(2);
    if (!pMatrix) RET(D3DERR_INVALIDCALL, 3);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    if (state < 512) {
        memcpy(&ctx->transforms[state], hle_ptr(pMatrix), sizeof(D3DMATRIX));
        sb9_mark(ctx, SBK_XFORM, state, 1);
    }
    RET(D3D_OK, 3);
}

void dev9_GetTransform(void) {
    uint32_t self = A32(0);
    uint32_t state = A32(1);
    uint32_t pMatrix = A32(2);
    if (!pMatrix) RET(D3DERR_INVALIDCALL, 3);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    if (state < 512) {
        /* Fresh devices zero-fill ctx; D3D9 defaults transforms to identity.
         * CreateDevice init is outside allowed files, so normalize on access. */
        float *m = (float *)&ctx->transforms[state];
        int all_zero = 1;
        for (int k = 0; k < 16; k++) { if (m[k] != 0.0f) { all_zero = 0; break; } }
        if (all_zero) {
            for (int i = 0; i < 4; i++)
                for (int j = 0; j < 4; j++)
                    ctx->transforms[state].m[i][j] = (i == j) ? 1.0f : 0.0f;
        }
        memcpy(hle_ptr(pMatrix), &ctx->transforms[state], sizeof(D3DMATRIX));
    }
    RET(D3D_OK, 3);
}

/* (this, State, pMatrix): the transform becomes current * pMatrix (row-vector
 * convention). A never-set transform is the identity, like GetTransform. */
void dev9_MultiplyTransform(void) {
    uint32_t self = A32(0), state = A32(1), pMatrix = A32(2);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    if (!pMatrix || state >= 512) RET(D3DERR_INVALIDCALL, 3);
    float *cur = (float *)&ctx->transforms[state];
    int all_zero = 1;
    for (int k = 0; k < 16; k++) if (cur[k] != 0.0f) { all_zero = 0; break; }
    if (all_zero) for (int k = 0; k < 4; k++) cur[k * 5] = 1.0f;
    float a[16], r[16];
    const float *b = (const float *)hle_ptr(pMatrix);
    memcpy(a, cur, sizeof a);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] +
                           a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
    memcpy(cur, r, sizeof r);
    sb9_mark(ctx, SBK_XFORM, state, 1);
    RET(D3D_OK, 3);
}

/* Public IDirect3DDevice9::SetViewport docs: D3DERR_INVALIDCALL if the
 * viewport "describes a region that cannot exist within the render target
 * surface". Use the currently bound render target (not just the presentation
 * parameters): SetRenderTarget may bind a differently sized offscreen surface.
 * The presentation parameters are only a fallback for a missing/invalid
 * surface context. */
static void d3d9_render_target_size(d3d9_device_ctx_t *ctx, uint32_t *w, uint32_t *h) {
    d3d9_surface_ctx_t *sc = ctx->render_target ?
        (d3d9_surface_ctx_t *)OBJ_CTX(ctx->render_target) : NULL;
    if (sc && sc->desc.Width && sc->desc.Height) {
        *w = sc->desc.Width;
        *h = sc->desc.Height;
        return;
    }
    *w = ctx->pp.BackBufferWidth ? ctx->pp.BackBufferWidth : D3D9_DEFAULT_WIDTH;
    *h = ctx->pp.BackBufferHeight ? ctx->pp.BackBufferHeight : D3D9_DEFAULT_HEIGHT;
}

void dev9_SetViewport(void) {
    uint32_t self = A32(0);
    uint32_t pVp = A32(1);
    if (!pVp) RET(D3DERR_INVALIDCALL, 2);
    D3DVIEWPORT9 *vp = (D3DVIEWPORT9 *)hle_ptr(pVp);
    if (vp->Width == 0 || vp->Height == 0 || vp->MinZ < 0.0f || vp->MinZ > 1.0f || vp->MaxZ < 0.0f || vp->MaxZ > 1.0f || vp->MinZ > vp->MaxZ) {
        fprintf(stderr, "[d3d9] SetViewport: invalid params (W=%u H=%u MinZ=%f MaxZ=%f)\n", vp->Width, vp->Height, vp->MinZ, vp->MaxZ);
        RET(D3DERR_INVALIDCALL, 2);
    }
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    uint32_t tw, th;
    d3d9_render_target_size(ctx, &tw, &th);
    /* Subtraction form avoids X+Width / Y+Height wrapping around DWORD. */
    if (vp->X > tw || vp->Width > tw - vp->X ||
        vp->Y > th || vp->Height > th - vp->Y) {
        fprintf(stderr, "[d3d9] SetViewport: region X=%u Y=%u W=%u H=%u outside render target %ux%u\n",
                vp->X, vp->Y, vp->Width, vp->Height, tw, th);
        RET(D3DERR_INVALIDCALL, 2);
    }
    memcpy(&ctx->viewport, vp, sizeof(D3DVIEWPORT9));
    sb9_mark(ctx, SBK_VIEWPORT, 0, 1);
    RET(D3D_OK, 2);
}

void dev9_GetViewport(void) {
    uint32_t self = A32(0);
    uint32_t pVp = A32(1);
    if (!pVp) RET(D3DERR_INVALIDCALL, 2);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    memcpy(hle_ptr(pVp), &ctx->viewport, sizeof(D3DVIEWPORT9));
    RET(D3D_OK, 2);
}

void dev9_SetMaterial(void) {
    uint32_t self = A32(0), pMat = A32(1);
    if (!pMat) RET(D3DERR_INVALIDCALL, 2);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    memcpy(ctx->material, hle_ptr(pMat), sizeof ctx->material);
    sb9_mark(ctx, SBK_MATERIAL, 0, 1);
    RET(D3D_OK, 2);
}
void dev9_GetMaterial(void) {
    uint32_t self = A32(0), pMat = A32(1);
    if (!pMat) RET(D3DERR_INVALIDCALL, 2);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    memcpy(hle_ptr(pMat), ctx->material, sizeof ctx->material);
    RET(D3D_OK, 2);
}
void dev9_SetLight(void) {
    uint32_t self = A32(0), idx = A32(1), pLight = A32(2);
    if (!pLight || idx >= D3D9_MAX_LIGHTS) RET(D3DERR_INVALIDCALL, 3);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    memcpy(ctx->lights[idx], hle_ptr(pLight), D3D9_LIGHT_BYTES);
    ctx->light_defined[idx] = 1;
    sb9_mark(ctx, SBK_LIGHT, idx, 1);
    RET(D3D_OK, 3);
}
void dev9_GetLight(void) {
    uint32_t self = A32(0), idx = A32(1), pLight = A32(2);
    if (!pLight || idx >= D3D9_MAX_LIGHTS) RET(D3DERR_INVALIDCALL, 3);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    if (!ctx->light_defined[idx]) RET(D3DERR_INVALIDCALL, 3);
    memcpy(hle_ptr(pLight), ctx->lights[idx], D3D9_LIGHT_BYTES);
    RET(D3D_OK, 3);
}
void dev9_LightEnable(void) {
    uint32_t self = A32(0), idx = A32(1);
    if (idx >= D3D9_MAX_LIGHTS) RET(D3DERR_INVALIDCALL, 3);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    ctx->light_enable[idx] = A32(2) ? 1u : 0u;
    sb9_mark(ctx, SBK_LIGHT_EN, idx, 1);
    RET(D3D_OK, 3);
}
void dev9_GetLightEnable(void) {
    uint32_t self = A32(0), idx = A32(1), pEnable = A32(2);
    if (idx >= D3D9_MAX_LIGHTS) RET(D3DERR_INVALIDCALL, 3);
    if (pEnable) MEM32(pEnable) = ((d3d9_device_ctx_t *)OBJ_CTX(self))->light_enable[idx];
    RET(D3D_OK, 3);
}
void dev9_SetClipPlane(void) {
    uint32_t self = A32(0), idx = A32(1), pPlane = A32(2);
    if (!pPlane || idx >= D3D9_MAX_CLIP_PLANES) RET(D3DERR_INVALIDCALL, 3);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    memcpy(ctx->clip_planes[idx], hle_ptr(pPlane), 4 * sizeof(float));
    sb9_mark(ctx, SBK_CLIP, idx, 1);
    RET(D3D_OK, 3);
}
void dev9_GetClipPlane(void) {
    uint32_t self = A32(0), idx = A32(1), pPlane = A32(2);
    if (!pPlane || idx >= D3D9_MAX_CLIP_PLANES) RET(D3DERR_INVALIDCALL, 3);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    memcpy(hle_ptr(pPlane), ctx->clip_planes[idx], 4 * sizeof(float));
    RET(D3D_OK, 3);
}

/* Public-enum validation for dev9_SetRenderState (public docs:
 * IDirect3DDevice9::SetRenderState returns D3DERR_INVALIDCALL when an
 * argument is invalid; values are the public D3D9 ones, Wine 11.0
 * include/d3d9types.h). */
static int dev9_rs_valid(uint32_t state, uint32_t value) {
    static const uint32_t cmp_funcs[] = {
        D3DCMP_NEVER, D3DCMP_LESS, D3DCMP_EQUAL, D3DCMP_LESSEQUAL,
        D3DCMP_GREATER, D3DCMP_NOTEQUAL, D3DCMP_GREATEREQUAL, D3DCMP_ALWAYS
    };
    static const uint32_t blends[] = {
        D3DBLEND_ZERO, D3DBLEND_ONE, D3DBLEND_SRCCOLOR, D3DBLEND_INVSRCCOLOR,
        D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLEND_DESTALPHA,
        D3DBLEND_INVDESTALPHA, D3DBLEND_DESTCOLOR, D3DBLEND_INVDESTCOLOR,
        D3DBLEND_SRCALPHASAT, D3DBLEND_BOTHSRCALPHA, D3DBLEND_BOTHINVSRCALPHA,
        D3DBLEND_BLENDFACTOR, D3DBLEND_INVBLENDFACTOR
    };
    static const uint32_t blend_ops[] = {
        D3DBLENDOP_ADD, D3DBLENDOP_SUBTRACT, D3DBLENDOP_REVSUBTRACT,
        D3DBLENDOP_MIN, D3DBLENDOP_MAX
    };
    static const uint32_t stencil_ops[] = {
        D3DSTENCILOP_KEEP, D3DSTENCILOP_ZERO, D3DSTENCILOP_REPLACE,
        D3DSTENCILOP_INCRSAT, D3DSTENCILOP_DECRSAT, D3DSTENCILOP_INVERT,
        D3DSTENCILOP_INCR, D3DSTENCILOP_DECR
    };
    const uint32_t *tab;
    uint32_t n, i;
    switch (state) {
        case D3DRS_SRCBLEND: case D3DRS_DESTBLEND:
        case D3DRS_SRCBLENDALPHA: case D3DRS_DESTBLENDALPHA:
        case D3DRS_BLENDOPALPHA:
            tab = blends; n = sizeof blends / sizeof blends[0]; break;
        case D3DRS_ZFUNC: case D3DRS_ALPHAFUNC:
        case D3DRS_STENCILFUNC: case D3DRS_CCW_STENCILFUNC:
            tab = cmp_funcs; n = sizeof cmp_funcs / sizeof cmp_funcs[0]; break;
        case D3DRS_BLENDOP:
            tab = blend_ops; n = sizeof blend_ops / sizeof blend_ops[0]; break;
        case D3DRS_STENCILFAIL: case D3DRS_STENCILZFAIL: case D3DRS_STENCILPASS:
        case D3DRS_CCW_STENCILFAIL: case D3DRS_CCW_STENCILZFAIL: case D3DRS_CCW_STENCILPASS:
            tab = stencil_ops; n = sizeof stencil_ops / sizeof stencil_ops[0]; break;
        case D3DRS_STENCILENABLE: case D3DRS_TWOSIDEDSTENCILMODE:
        case D3DRS_ALPHATESTENABLE: case D3DRS_SEPARATEALPHABLENDENABLE:
            return value <= 1;
        case D3DRS_COLORWRITEENABLE: case D3DRS_COLORWRITEENABLE1:
        case D3DRS_COLORWRITEENABLE2: case D3DRS_COLORWRITEENABLE3:
            return (value & ~0xFu) == 0;
        default:
            return 1;
    }
    for (i = 0; i < n; i++) if (tab[i] == value) return 1;
    return 0;
}

/* Draw-time honesty (public docs): the Metal backend cannot apply these
 * combinations; drawing fails with D3DERR_NOTAVAILABLE and a log instead of
 * a false OK. Alpha test is supported by the HLE's fixed-function pipeline. */
static int dev9_draw_blocked(const d3d9_device_ctx_t *ctx) {
    if (ctx->render_states[D3DRS_STENCILENABLE]) {
        fprintf(stderr, "[d3d9] draw: stencil is not supported by the Metal backend\n");
        return 1;
    }
    if (ctx->render_states[D3DRS_DEPTHBIAS]) {
        fprintf(stderr, "[d3d9] draw: D3DRS_DEPTHBIAS is not supported by the Metal backend\n");
        return 1;
    }
    if (ctx->render_states[D3DRS_SLOPESCALEDEPTHBIAS]) {
        fprintf(stderr, "[d3d9] draw: D3DRS_SLOPESCALEDEPTHBIAS is not supported by the Metal backend\n");
        return 1;
    }
    if (ctx->render_states[D3DRS_COLORWRITEENABLE] != 0xFu ||
        ctx->render_states[D3DRS_COLORWRITEENABLE1] != 0xFu ||
        ctx->render_states[D3DRS_COLORWRITEENABLE2] != 0xFu ||
        ctx->render_states[D3DRS_COLORWRITEENABLE3] != 0xFu) {
        fprintf(stderr, "[d3d9] draw: color write masks are not supported by the Metal backend\n");
        return 1;
    }
    if (ctx->render_states[D3DRS_SEPARATEALPHABLENDENABLE]) {
        /* Compare against the effective blend factors (same defaults the
         * backend applies): a separate alpha setting equal to them is not
         * a distinct request. */
        uint32_t src_eff = ctx->render_states[D3DRS_SRCBLEND] ? ctx->render_states[D3DRS_SRCBLEND] : D3DBLEND_SRCALPHA;
        uint32_t dst_eff = ctx->render_states[D3DRS_DESTBLEND] ? ctx->render_states[D3DRS_DESTBLEND] : D3DBLEND_INVSRCALPHA;
        if (ctx->render_states[D3DRS_SRCBLENDALPHA] != src_eff ||
            ctx->render_states[D3DRS_DESTBLENDALPHA] != dst_eff) {
            fprintf(stderr, "[d3d9] draw: separate alpha blend factors are not supported by the Metal backend\n");
            return 1;
        }
    }
    /* BLENDOP's public default is D3DBLENDOP_ADD; the HLE stores 0 for unset. */
    if (ctx->render_states[D3DRS_BLENDOP] &&
        ctx->render_states[D3DRS_BLENDOP] != D3DBLENDOP_ADD) {
        fprintf(stderr, "[d3d9] draw: D3DRS_BLENDOP != D3DBLENDOP_ADD is not supported by the Metal backend\n");
        return 1;
    }
    if (ctx->render_states[D3DRS_SEPARATEALPHABLENDENABLE] &&
        ctx->render_states[D3DRS_BLENDOPALPHA] &&
        ctx->render_states[D3DRS_BLENDOPALPHA] != D3DBLENDOP_ADD) {
        fprintf(stderr, "[d3d9] draw: D3DRS_BLENDOPALPHA is not supported by the Metal backend\n");
        return 1;
    }
    return 0;
}

void dev9_SetRenderState(void) {
    uint32_t self = A32(0);
    uint32_t state = A32(1);
    uint32_t val = A32(2);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    if (!dev9_rs_valid(state, val)) {
        fprintf(stderr, "[d3d9] SetRenderState(state %u, value 0x%x): invalid value for state\n", state, val);
        RET(D3DERR_INVALIDCALL, 3);
    }
    if (state == 4034u /* D3DRS_SRGBWRITEENABLE */) {
        ctx->srgb_write = val ? 1u : 0u;
        hle_d3d9_metal_set_srgb_write((int)ctx->srgb_write);
        RET(D3D_OK, 3);
    }
    if (state < MAX_RENDER_STATES) {
        ctx->render_states[state] = val;
        sb9_mark(ctx, SBK_RS, state, 1);
    }
    RET(D3D_OK, 3);
}

void dev9_GetRenderState(void) {
    uint32_t self = A32(0);
    uint32_t state = A32(1);
    uint32_t pVal = A32(2);
    if (!pVal) RET(D3DERR_INVALIDCALL, 3);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    if (state == 4034u) MEM32(pVal) = ctx->srgb_write;
    else if (state < MAX_RENDER_STATES) {
        MEM32(pVal) = ctx->render_states[state];
    } else {
        MEM32(pVal) = 0;
    }
    RET(D3D_OK, 3);
}
/* Wrap a filled block context in a COM object. */
static uint32_t sb_wrap(uint32_t self, d3d9_sb_ctx_t *ctx) {
    init_d3d9_vtables();
    ctx->device = self;
    uint32_t o = hle_com_new(g_vt_sb9, 16);
    ctx->self = o;
    SET_OBJ_CTX(o, ctx);
    return o;
}

/* CreateStateBlock: the type is validated as D3D9 does; a block cannot be
 * created while another one is being recorded. */
void dev9_CreateStateBlock(void) {
    uint32_t self = A32(0), type = A32(1), ppSB = A32(2);
    d3d9_device_ctx_t *dev = (d3d9_device_ctx_t *)OBJ_CTX(self);
    if (!ppSB || !dev || dev->recording_state_block ||
        (type != D3DSBT_ALL && type != D3DSBT_PIXELSTATE && type != D3DSBT_VERTEXSTATE))
        RET(D3DERR_INVALIDCALL, 3);
    d3d9_sb_ctx_t *ctx = (d3d9_sb_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) RET(0x8007000Eu, 3);  /* E_OUTOFMEMORY */
    ctx->device = self;
    sb9_select_type(ctx, type);
    sb9_capture_state(ctx, dev);
    MEM32(ppSB) = sb_wrap(self, ctx);
    RET(D3D_OK, 3);
}

void dev9_BeginStateBlock(void) {
    d3d9_device_ctx_t *dev = (d3d9_device_ctx_t *)OBJ_CTX(A32(0));
    if (!dev || dev->recording_state_block) RET(D3DERR_INVALIDCALL, 1);
    if (!sb9_begin_record(dev)) RET(0x8007000Eu, 1);  /* E_OUTOFMEMORY */
    RET(D3D_OK, 1);
}

void dev9_EndStateBlock(void) {
    uint32_t self = A32(0), ppSB = A32(1);
    d3d9_device_ctx_t *dev = (d3d9_device_ctx_t *)OBJ_CTX(self);
    if (!dev || !dev->recording_state_block) RET(D3DERR_INVALIDCALL, 2);
    if (!ppSB) {                       /* the recording ends, nothing is kept */
        sb9_abort_record(dev);
        d3d9_device_push_backend(dev);
        RET(D3DERR_INVALIDCALL, 2);
    }
    d3d9_sb_ctx_t *ctx = sb9_end_record(dev);
    if (!ctx) RET(D3DERR_INVALIDCALL, 2);
    MEM32(ppSB) = sb_wrap(self, ctx);
    RET(D3D_OK, 2);
}
void dev9_SetClipStatus(void) { RET(D3D_OK, 2); }
void dev9_GetClipStatus(void) { RET(D3D_OK, 2); }

void dev9_GetTexture(void) {
    uint32_t self = A32(0);
    uint32_t stage = A32(1);
    uint32_t ppTex = A32(2);
    if (!ppTex) RET(D3DERR_INVALIDCALL, 3);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    if (stage < MAX_SAMPLERS) {
        MEM32(ppTex) = ctx->textures[stage];
        if (ctx->textures[stage]) MEM32(ctx->textures[stage] + 4)++;
    } else {
        MEM32(ppTex) = 0;
    }
    RET(D3D_OK, 3);
}

void dev9_SetTexture(void) {
    uint32_t self = A32(0);
    uint32_t stage = A32(1);
    uint32_t pTex = A32(2);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    if (stage < MAX_SAMPLERS) {
        ctx->textures[stage] = pTex;
        sb9_mark(ctx, SBK_TEX, stage, 1);
        if (hle_d3d9_gpu && hle_d3d9_gpu->set_sampler) {
            hle_d3d9_gpu->set_sampler(stage, gpu_texture_of(pTex));
        }
        if (stage == 0 && hle_d3d9_gpu && hle_d3d9_gpu->set_texture) {
            hle_d3d9_gpu->set_texture(gpu_texture_of(pTex));
        }
    }
    RET(D3D_OK, 3);
}

void dev9_GetTextureStageState(void) {
    uint32_t self = A32(0);
    uint32_t stage = A32(1);
    uint32_t type = A32(2);
    uint32_t pVal = A32(3);
    if (!pVal) RET(D3DERR_INVALIDCALL, 4);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    if (stage < MAX_TEXTURE_STAGES && type < MAX_TSS) {
        MEM32(pVal) = ctx->texture_stage_states[stage][type];
    } else {
        MEM32(pVal) = 0;
    }
    RET(D3D_OK, 4);
}

void dev9_SetTextureStageState(void) {
    uint32_t self = A32(0);
    uint32_t stage = A32(1);
    uint32_t type = A32(2);
    uint32_t val = A32(3);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    if (stage < MAX_TEXTURE_STAGES && type < MAX_TSS) {
        ctx->texture_stage_states[stage][type] = val;
        sb9_mark(ctx, SBK_TSS, stage * 64 + type, 1);
    }
    RET(D3D_OK, 4);
}

void dev9_GetSamplerState(void) {
    uint32_t self = A32(0);
    uint32_t sampler = A32(1);
    uint32_t type = A32(2);
    uint32_t pVal = A32(3);
    if (!pVal) RET(D3DERR_INVALIDCALL, 4);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    if (sampler < MAX_SAMPLERS && type < MAX_SAMPLER_STATES) {
        MEM32(pVal) = ctx->sampler_states[sampler][type];
    } else {
        MEM32(pVal) = 0;
    }
    RET(D3D_OK, 4);
}

__attribute__((weak)) void hle_d3d9_metal_set_sampler_state(uint32_t stage, uint32_t type, uint32_t value) {
    (void)stage; (void)type; (void)value;
}

void dev9_SetSamplerState(void) {
    uint32_t self = A32(0);
    uint32_t sampler = A32(1);
    uint32_t type = A32(2);
    uint32_t val = A32(3);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    if (sampler < MAX_SAMPLERS && type < MAX_SAMPLER_STATES) {
        ctx->sampler_states[sampler][type] = val;
        sb9_mark(ctx, SBK_SAMP, sampler * 16 + type, 1);
    }
    hle_d3d9_metal_set_sampler_state(sampler, type, val);
    RET(D3D_OK, 4);
}

/* Public D3D9 docs: ValidateDevice(pNumPasses) asks the driver whether the
 * current texture-blend state can be done in hardware and how many passes it
 * needs. The HLE cannot validate texture-blend caps, so claiming "1 pass, OK"
 * would be a fake success. Reject with NOTAVAILABLE and leave the output
 * untouched; a NULL out-pointer is INVALIDCALL. */
void dev9_ValidateDevice(void) {
    uint32_t pPasses = A32(1);
    if (!pPasses) {
        fprintf(stderr, "[d3d9] ValidateDevice: NULL pNumPasses\n");
        RET(D3DERR_INVALIDCALL, 2);
    }
    fprintf(stderr, "[d3d9] ValidateDevice: hardware pass validation not available\n");
    RET(D3DERR_NOTAVAILABLE, 2);
}

/* Guest ABI: (this, PaletteNumber, PALETTEENTRY[256]). All 256 entries are
 * transferred; a palette number outside the D3D9 range is an invalid call. */
void dev9_SetPaletteEntries(void) {
    uint32_t self = A32(0), pal = A32(1), pEntries = A32(2);
    if (!pEntries || pal >= D3D9_MAX_PALETTES) RET(D3DERR_INVALIDCALL, 3);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    memcpy(ctx->palette[pal], hle_ptr(pEntries), D3D9_PALETTE_ENTRIES * 4);
    sb9_mark(ctx, SBK_PALETTE, pal, 1);
    RET(D3D_OK, 3);
}
void dev9_GetPaletteEntries(void) {
    uint32_t self = A32(0), pal = A32(1), pEntries = A32(2);
    if (!pEntries || pal >= D3D9_MAX_PALETTES) RET(D3DERR_INVALIDCALL, 3);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    memcpy(hle_ptr(pEntries), ctx->palette[pal], D3D9_PALETTE_ENTRIES * 4);
    RET(D3D_OK, 3);
}
void dev9_SetCurrentTexturePalette(void) {
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(A32(0));
    ctx->current_texture_palette = A32(1);
    sb9_mark(ctx, SBK_CUR_PALETTE, 0, 1);
    RET(D3D_OK, 2);
}
void dev9_GetCurrentTexturePalette(void) {
    uint32_t pPal = A32(1);
    if (pPal) MEM32(pPal) = ((d3d9_device_ctx_t *)OBJ_CTX(A32(0)))->current_texture_palette;
    RET(D3D_OK, 2);
}

void dev9_SetScissorRect(void) {
    uint32_t self = A32(0);
    uint32_t pRect = A32(1);
    if (!pRect) RET(D3DERR_INVALIDCALL, 2);
    D3DRECT *r = (D3DRECT *)hle_ptr(pRect);
    /* D3D9 rectangles have an inclusive upper-left and an exclusive lower-right
     * corner; left > right or top > bottom cannot describe a rendering area,
     * so the public method reports D3DERR_INVALIDCALL. An empty rect (equal
     * edges) is valid: it simply clips everything while the test is enabled. */
    if (r->x1 > r->x2 || r->y1 > r->y2) {
        fprintf(stderr, "[d3d9] SetScissorRect: inverted rect (%d,%d)-(%d,%d)\n",
                r->x1, r->y1, r->x2, r->y2);
        RET(D3DERR_INVALIDCALL, 2);
    }
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    memcpy(&ctx->scissor_rect, r, sizeof(D3DRECT));
    sb9_mark(ctx, SBK_SCISSOR, 0, 1);
    RET(D3D_OK, 2);
}

void dev9_GetScissorRect(void) {
    uint32_t self = A32(0);
    uint32_t pRect = A32(1);
    if (!pRect) RET(D3DERR_INVALIDCALL, 2);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    memcpy(hle_ptr(pRect), &ctx->scissor_rect, sizeof(D3DRECT));
    RET(D3D_OK, 2);
}

void dev9_SetSoftwareVertexProcessing(void) {
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(A32(0));
    ctx->software_vertex_processing = A32(1) ? 1u : 0u;
    sb9_mark(ctx, SBK_SVP, 0, 1);
    RET(D3D_OK, 2);
}
void dev9_GetSoftwareVertexProcessing(void) {
    RET(((d3d9_device_ctx_t *)OBJ_CTX(A32(0)))->software_vertex_processing, 1);
}
void dev9_SetNPatchMode(void) {
    /* FLOAT nSegments is passed by value on the guest stack, not by pointer. */
    uint32_t bits = A32(1);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(A32(0));
    memcpy(&ctx->n_patch_mode, &bits, sizeof ctx->n_patch_mode);
    sb9_mark(ctx, SBK_NPATCH, 0, 1);
    RET(D3D_OK, 2);
}
void dev9_GetNPatchMode(void) {
    uint32_t bits;
    memcpy(&bits, &((d3d9_device_ctx_t *)OBJ_CTX(A32(0)))->n_patch_mode, sizeof bits);
    RET(bits, 1);
}

static void mul_mat(const float *a, const float *b, float *out) {
    float r[16];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] +
                           a[i * 4 + 1] * b[1 * 4 + j] +
                           a[i * 4 + 2] * b[2 * 4 + j] +
                           a[i * 4 + 3] * b[3 * 4 + j];
        }
    }
    memcpy(out, r, sizeof(r));
}

/* ffp_light_payload_t / ffp_mat_payload_t / ffp_uniforms_payload_t are shared
 * with the Metal backend via d3d9_metal.h so both sides use the same sizeof.
 * The unified payload carries lighting (mat/lights/globalAmbient) and texture
 * stage transforms (view/proj/textureFactor/texTransform). */

static uint32_t fvf_stride(uint32_t fvf) {
    uint32_t s = 0;
    if (fvf & D3D9_FVF_XYZ) s += 12;
    else if (fvf & D3D9_FVF_XYZRHW) s += 16;
    if (fvf & D3D9_FVF_NORMAL) s += 12;
    if (fvf & D3D9_FVF_DIFFUSE) s += 4;
    if (fvf & D3D9_FVF_SPECULAR) s += 4;
    uint32_t tc = (fvf & D3D9_FVF_TEXCOUNT_MASK) >> D3D9_FVF_TEXCOUNT_SHIFT;
    s += tc * 8;
    return s ? s : 32;
}

static void *ensure_ffp_pipeline(d3d9_device_ctx_t *ctx, uint32_t *out_stride) {
    uint32_t fvf = ctx->fvf;
    if (!fvf) {
        if (out_stride) *out_stride = 0;
        return NULL;
    }
    if (out_stride) *out_stride = fvf_stride(fvf);

    /* Construct vertex elements to set vertex decl for FVF */
    D3DVERTEXELEMENT9 elems[16];
    uint32_t nelems = 0;
    uint16_t off = 0;
    if (fvf & D3D9_FVF_XYZ) {
        elems[nelems++] = (D3DVERTEXELEMENT9){ 0, off, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0 };
        off += 12;
    } else if (fvf & D3D9_FVF_XYZRHW) {
        elems[nelems++] = (D3DVERTEXELEMENT9){ 0, off, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_POSITIONT, 0 };
        off += 16;
    }
    if (fvf & D3D9_FVF_NORMAL) {
        elems[nelems++] = (D3DVERTEXELEMENT9){ 0, off, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_NORMAL, 0 };
        off += 12;
    }
    if (fvf & D3D9_FVF_DIFFUSE) {
        elems[nelems++] = (D3DVERTEXELEMENT9){ 0, off, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0 };
        off += 4;
    }
    if (fvf & D3D9_FVF_SPECULAR) {
        elems[nelems++] = (D3DVERTEXELEMENT9){ 0, off, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 1 };
        off += 4;
    }
    uint32_t tc = (fvf & D3D9_FVF_TEXCOUNT_MASK) >> D3D9_FVF_TEXCOUNT_SHIFT;
    for (uint32_t i = 0; i < tc && i < 8; i++) {
        elems[nelems++] = (D3DVERTEXELEMENT9){ 0, off, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, (uint8_t)i };
        off += 8;
    }
    elems[nelems] = (D3DVERTEXELEMENT9){ 0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0 };
    if (hle_d3d9_gpu && hle_d3d9_gpu->set_vertex_decl) {
        hle_d3d9_gpu->set_vertex_decl(elems, nelems);
    }

    /* Build d3d9_ffp_desc_t */
    d3d9_ffp_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.fvf = fvf;

    d3d9_ffp_stage_t stages[8];
    memset(stages, 0, sizeof(stages));
    desc.stage_count = tc > 0 ? (tc > 8 ? 8 : tc) : (ctx->textures[0] ? 1 : 0);
    /* D3DTSS constant indices */
    #define TSS_COLORARG0 26u
    #define TSS_ALPHAARG0 27u
    #define TSS_RESULTARG 28u
    #define TSS_CONSTANT 32u
    #define TSS_TEXCOORDINDEX 11u
    #define TSS_TEXTURETRANSFORMFLAGS 24u
    #define TSS_BUMPENVMAT00 7u
    #define TSS_BUMPENVMAT01 8u
    #define TSS_BUMPENVMAT10 9u
    #define TSS_BUMPENVMAT11 10u
    #define TSS_BUMPENVLSCALE 22u
    #define TSS_BUMPENVLOFFSET 23u
    for (uint32_t i = 0; i < desc.stage_count; i++) {
        uint32_t cop = ctx->texture_stage_states[i][D3DTSS_COLOROP];
        uint32_t ca1 = ctx->texture_stage_states[i][D3DTSS_COLORARG1];
        uint32_t ca2 = ctx->texture_stage_states[i][D3DTSS_COLORARG2];
        uint32_t aop = ctx->texture_stage_states[i][D3DTSS_ALPHAOP];
        uint32_t aa1 = ctx->texture_stage_states[i][D3DTSS_ALPHAARG1];
        uint32_t aa2 = ctx->texture_stage_states[i][D3DTSS_ALPHAARG2];
        uint32_t ca0 = ctx->texture_stage_states[i][TSS_COLORARG0];
        uint32_t aa0 = ctx->texture_stage_states[i][TSS_ALPHAARG0];
        uint32_t res = ctx->texture_stage_states[i][TSS_RESULTARG];
        uint32_t tci = ctx->texture_stage_states[i][TSS_TEXCOORDINDEX];
        uint32_t ttf = ctx->texture_stage_states[i][TSS_TEXTURETRANSFORMFLAGS];
        uint32_t cst = ctx->texture_stage_states[i][TSS_CONSTANT];
        stages[i].color_op = cop ? cop : (ctx->textures[i] ? D3D9_TEXOP_MODULATE : D3D9_TEXOP_DISABLE);
        stages[i].color_arg0 = ca0 & 0x3Fu;
        stages[i].color_arg1 = ca1 & 0x3Fu;
        stages[i].color_arg2 = ca2 & 0x3Fu;
        stages[i].alpha_op = aop ? aop : (ctx->textures[i] ? D3D9_TEXOP_SELECTARG1 : D3D9_TEXOP_DISABLE);
        stages[i].alpha_arg0 = aa0 & 0x3Fu;
        stages[i].alpha_arg1 = aa1 & 0x3Fu;
        stages[i].alpha_arg2 = aa2 & 0x3Fu;
        stages[i].result_arg = res ? res : D3D9_TA_CURRENT;
        stages[i].texcoord_index = tci;
        stages[i].texture_transform_flags = ttf;
        stages[i].constant = cst;
        /* bump params */
        uint32_t b00 = ctx->texture_stage_states[i][TSS_BUMPENVMAT00];
        uint32_t b01 = ctx->texture_stage_states[i][TSS_BUMPENVMAT01];
        uint32_t b10 = ctx->texture_stage_states[i][TSS_BUMPENVMAT10];
        uint32_t b11 = ctx->texture_stage_states[i][TSS_BUMPENVMAT11];
        memcpy(&stages[i].bumpenv_mat00, &b00, 4);
        memcpy(&stages[i].bumpenv_mat01, &b01, 4);
        memcpy(&stages[i].bumpenv_mat10, &b10, 4);
        memcpy(&stages[i].bumpenv_mat11, &b11, 4);
        uint32_t bls = ctx->texture_stage_states[i][TSS_BUMPENVLSCALE];
        uint32_t blo = ctx->texture_stage_states[i][TSS_BUMPENVLOFFSET];
        memcpy(&stages[i].bumpenv_lscale, &bls, 4);
        memcpy(&stages[i].bumpenv_loffset, &blo, 4);
    }
    desc.stages = stages;
    desc.texture_factor = ctx->render_states[60];
    /* world/view/proj */
    {
        float *w = (float *)&ctx->transforms[D3DTS_WORLD];
        float *v = (float *)&ctx->transforms[D3DTS_VIEW];
        float *p = (float *)&ctx->transforms[D3DTS_PROJECTION];
        /* identity fallback */
        int zw=1,zv=1,zp=1;
        for (int k=0;k<16;k++){ if(w[k]!=0)zw=0; if(v[k]!=0)zv=0; if(p[k]!=0)zp=0;}
        if (zw) { memset(desc.world_matrix,0,sizeof(desc.world_matrix)); desc.world_matrix[0]=desc.world_matrix[5]=desc.world_matrix[10]=desc.world_matrix[15]=1.0f; }
        else memcpy(desc.world_matrix, w, sizeof(desc.world_matrix));
        if (zv) { memset(desc.view_matrix,0,sizeof(desc.view_matrix)); desc.view_matrix[0]=desc.view_matrix[5]=desc.view_matrix[10]=desc.view_matrix[15]=1.0f; }
        else memcpy(desc.view_matrix, v, sizeof(desc.view_matrix));
        if (zp) { memset(desc.proj_matrix,0,sizeof(desc.proj_matrix)); desc.proj_matrix[0]=desc.proj_matrix[5]=desc.proj_matrix[10]=desc.proj_matrix[15]=1.0f; }
        else memcpy(desc.proj_matrix, p, sizeof(desc.proj_matrix));
    }
    for (uint32_t ti = 0; ti < 8; ti++) {
        float *t = (float *)&ctx->transforms[16 + ti];
        int z=1; for (int k=0;k<16;k++) if (t[k]!=0) z=0;
        if (z) { memset(desc.texture_transforms[ti],0,sizeof(desc.texture_transforms[ti])); desc.texture_transforms[ti][0]=desc.texture_transforms[ti][5]=desc.texture_transforms[ti][10]=desc.texture_transforms[ti][15]=1.0f; }
        else memcpy(desc.texture_transforms[ti], t, sizeof(desc.texture_transforms[ti]));
    }

    /* Fog */
    if (ctx->render_states[D3DRS_FOGENABLE]) {
        uint32_t fm = ctx->render_states[D3DRS_FOGTABLEMODE];
        if (!fm) fm = ctx->render_states[D3DRS_FOGVERTEXMODE];
        desc.fog_mode = fm ? fm : D3D9_FOG_LINEAR;
        uint32_t fcolor = ctx->render_states[D3DRS_FOGCOLOR];
        desc.fog_color = fcolor;
        uint32_t fstart = ctx->render_states[D3DRS_FOGSTART];
        uint32_t fend = ctx->render_states[D3DRS_FOGEND];
        uint32_t fdensity = ctx->render_states[D3DRS_FOGDENSITY];
        memcpy(&desc.fog_start, &fstart, 4);
        memcpy(&desc.fog_end, &fend, 4);
        memcpy(&desc.fog_density, &fdensity, 4);
    } else {
        desc.fog_mode = D3D9_FOG_NONE;
    }

    /* Alpha test */
    if (ctx->render_states[D3DRS_ALPHATESTENABLE]) {
        desc.alpha_test_enable = 1;
        uint32_t afunc = ctx->render_states[D3DRS_ALPHAFUNC];
        desc.alpha_func = afunc ? afunc : D3D9_CMP_ALWAYS;
        desc.alpha_ref = ctx->render_states[D3DRS_ALPHAREF] & 0xFFu;
    }

    /* Lighting — public D3D9 docs: XYZRHW (POSITIONT) vertices are
     * pre-transformed and lit vertices are not supported; FFP lighting
     * is forced off for XYZRHW regardless of D3DRS_LIGHTING. */
    desc.lighting = ctx->render_states[D3DRS_LIGHTING] ? 1u : 0u;
    if (fvf & D3D9_FVF_XYZRHW) desc.lighting = 0;
    desc.specular_enable = ctx->render_states[D3DRS_SPECULARENABLE] ? 1u : 0u;
    desc.local_viewer = ctx->render_states[D3DRS_LOCALVIEWER] ? 1u : 0u;
    desc.normalize_normals = ctx->render_states[D3DRS_NORMALIZENORMALS] ? 1u : 0u;
    desc.color_vertex = ctx->render_states[D3DRS_COLORVERTEX] ? 1u : 0u;
    desc.diff_mat_source = ctx->render_states[D3DRS_DIFFUSEMATERIALSOURCE];
    desc.spec_mat_source = ctx->render_states[D3DRS_SPECULARMATERIALSOURCE];
    desc.amb_mat_source = ctx->render_states[D3DRS_AMBIENTMATERIALSOURCE];
    desc.emiss_mat_source = ctx->render_states[D3DRS_EMISSIVEMATERIALSOURCE];

    /* Uniforms */
    ffp_uniforms_payload_t u;
    memset(&u, 0, sizeof(u));
    /* Global Ambient */
    uint32_t amb_c = ctx->render_states[D3DRS_AMBIENT];
    u.globalAmbient[0] = ((amb_c >> 16) & 0xFFu) / 255.0f;
    u.globalAmbient[1] = ((amb_c >> 8) & 0xFFu) / 255.0f;
    u.globalAmbient[2] = (amb_c & 0xFFu) / 255.0f;
    u.globalAmbient[3] = ((amb_c >> 24) & 0xFFu) / 255.0f;

    /* Material */
    const D3DMATERIAL9 *pMat = (const D3DMATERIAL9 *)ctx->material;
    memcpy(u.mat.diffuse, &pMat->Diffuse, 16);
    memcpy(u.mat.ambient, &pMat->Ambient, 16);
    memcpy(u.mat.specular, &pMat->Specular, 16);
    memcpy(u.mat.emissive, &pMat->Emissive, 16);
    u.mat.power_pad[0] = pMat->Power;

    /* Lights */
    for (uint32_t i = 0; i < 8; i++) {
        if (!ctx->light_enable[i] || !ctx->light_defined[i]) {
            u.lights[i].params2[3] = 0.0f; /* inactive */
            continue;
        }
        const D3DLIGHT9 *lt = (const D3DLIGHT9 *)ctx->lights[i];
        memcpy(u.lights[i].diffuse, &lt->Diffuse, 16);
        memcpy(u.lights[i].specular, &lt->Specular, 16);
        memcpy(u.lights[i].ambient, &lt->Ambient, 16);
        memcpy(u.lights[i].pos, &lt->Position, 12); u.lights[i].pos[3] = 1.0f;
        memcpy(u.lights[i].dir, &lt->Direction, 12); u.lights[i].dir[3] = 0.0f;
        u.lights[i].params1[0] = lt->Range;
        u.lights[i].params1[1] = lt->Falloff;
        u.lights[i].params1[2] = lt->Theta;
        u.lights[i].params1[3] = lt->Phi;
        u.lights[i].params2[0] = lt->Attenuation0;
        u.lights[i].params2[1] = lt->Attenuation1;
        u.lights[i].params2[2] = lt->Attenuation2;
        u.lights[i].params2[3] = (float)lt->Type;
    }
    /* Identity matrix by default */
    u.mvp[0] = u.mvp[5] = u.mvp[10] = u.mvp[15] = 1.0f;
    if (fvf & D3D9_FVF_XYZ) {
        /* MVP = Proj * View * World */
        const float *world = (const float *)&ctx->transforms[D3DTS_WORLD];
        const float *view = (const float *)&ctx->transforms[D3DTS_VIEW];
        const float *proj = (const float *)&ctx->transforms[D3DTS_PROJECTION];
        /* Check if identity */
        float w[16], v[16], p[16];
        memcpy(w, world, 64);
        memcpy(v, view, 64);
        memcpy(p, proj, 64);
        /* If all zeros, make identity */
        int zero_w = 1, zero_v = 1, zero_p = 1;
        for (int k = 0; k < 16; k++) {
            if (w[k] != 0.0f) zero_w = 0;
            if (v[k] != 0.0f) zero_v = 0;
            if (p[k] != 0.0f) zero_p = 0;
        }
        if (zero_w) w[0] = w[5] = w[10] = w[15] = 1.0f;
        if (zero_v) v[0] = v[5] = v[10] = v[15] = 1.0f;
        if (zero_p) p[0] = p[5] = p[10] = p[15] = 1.0f;
        memcpy(u.world, w, sizeof(u.world));
        memcpy(u.view, v, sizeof(u.view));
        memcpy(u.proj, p, sizeof(u.proj));

        float wv[16];
        mul_mat(w, v, wv);
        mul_mat(wv, p, u.mvp);
    } else {
        u.world[0] = u.world[5] = u.world[10] = u.world[15] = 1.0f;
        u.view[0] = u.view[5] = u.view[10] = u.view[15] = 1.0f;
        u.proj[0] = u.proj[5] = u.proj[10] = u.proj[15] = 1.0f;
    }
    {
        uint32_t tf = desc.texture_factor;
        u.textureFactor[0] = ((tf >> 16) & 0xFFu) / 255.0f;
        u.textureFactor[1] = ((tf >> 8) & 0xFFu) / 255.0f;
        u.textureFactor[2] = (tf & 0xFFu) / 255.0f;
        u.textureFactor[3] = ((tf >> 24) & 0xFFu) / 255.0f;
        for (uint32_t ti = 0; ti < 8; ti++) {
            memcpy(u.texTransform[ti], desc.texture_transforms[ti], sizeof(u.texTransform[ti]));
        }
    }
    uint32_t fc = desc.fog_color;
    u.fogColor[0] = ((fc >> 16) & 0xFFu) / 255.0f;
    u.fogColor[1] = ((fc >> 8) & 0xFFu) / 255.0f;
    u.fogColor[2] = (fc & 0xFFu) / 255.0f;
    u.fogColor[3] = ((fc >> 24) & 0xFFu) / 255.0f;
    u.fogStart = desc.fog_start;
    u.fogEnd = desc.fog_end;
    u.fogDensity = desc.fog_density;
    /* Viewport for XYZRHW screen-pixel→clip conversion in the shader. */
    u.viewportXYWH[0] = (float)ctx->viewport.X;
    u.viewportXYWH[1] = (float)ctx->viewport.Y;
    u.viewportXYWH[2] = (float)ctx->viewport.Width;
    u.viewportXYWH[3] = (float)ctx->viewport.Height;
    u.viewportZ[0] = ctx->viewport.MinZ;
    u.viewportZ[1] = ctx->viewport.MaxZ;
    u.viewportZ[2] = 0.0f;
    u.viewportZ[3] = 0.0f;

    if (hle_d3d9_gpu && hle_d3d9_gpu->set_ffp_uniforms) {
        hle_d3d9_gpu->set_ffp_uniforms(&u, sizeof(u));
    }

    char *msl = NULL; size_t len = 0; const char *err = NULL;
    int rc = d3d9_ffp_generate(&desc, &msl, &len, &err);
    if (rc != 0 || !msl) {
        return NULL;
    }
    void *pipe = NULL;
    if (hle_d3d9_gpu && hle_d3d9_gpu->create_pipeline) {
        pipe = hle_d3d9_gpu->create_pipeline(msl, msl);
    }
    free(msl);
    return pipe;
}

static void *ensure_dynamic_ib(d3d9_device_ctx_t *ctx, uint32_t size) {
    if (ctx->dynamic_ib && ctx->dynamic_ib_size >= size) {
        return ctx->dynamic_ib;
    }
    if (ctx->dynamic_ib && hle_d3d9_gpu && hle_d3d9_gpu->destroy_buffer) {
        hle_d3d9_gpu->destroy_buffer(ctx->dynamic_ib);
        ctx->dynamic_ib = NULL;
        ctx->dynamic_ib_size = 0;
    }
    uint32_t alloc_sz = size < 4096 ? 4096 : size;
    if (hle_d3d9_gpu && hle_d3d9_gpu->create_buffer) {
        ctx->dynamic_ib = hle_d3d9_gpu->create_buffer(alloc_sz, 1);
        if (ctx->dynamic_ib) ctx->dynamic_ib_size = alloc_sz;
    }
    return ctx->dynamic_ib;
}

void dev9_DrawPrimitive(void) {
    /* (this, PrimitiveType, StartVertex, PrimitiveCount) */
    uint32_t self = A32(0);
    uint32_t type = A32(1);
    uint32_t start = A32(2);
    uint32_t count = A32(3);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);

    apply_render_state(ctx);
    apply_shader_resources(ctx);

    if (dev9_draw_blocked(ctx)) RET(D3DERR_NOTAVAILABLE, 4);

    /* Public docs: non-indexed instancing is not hardware accelerated and is
     * not supported by this backend. Never report a false OK for it. */
    for (uint32_t i = 0; i < MAX_STREAMS; i++) {
        uint32_t f = ctx->streams[i].freq;
        if ((f & D3DSTREAMSOURCE_INDEXEDDATA) || (f & D3DSTREAMSOURCE_INSTANCEDATA)) {
            fprintf(stderr, "[d3d9] DrawPrimitive: non-indexed instancing is not supported\n");
            RET(D3DERR_INVALIDCALL, 4);
        }
    }

    if (hle_d3d9_gpu && hle_d3d9_gpu->draw) {
        void *vb = NULL;
        uint32_t stride = 32;
        if (ctx->streams[0].vb) {
            d3d9_vb_ctx_t *vbc = (d3d9_vb_ctx_t *)OBJ_CTX(ctx->streams[0].vb);
            if (vbc) vb = vbc->gpu_buf;
            if (ctx->streams[0].stride) stride = ctx->streams[0].stride;
        }

        int use_ffp = 0;
        void *pipe = NULL;
        if (ctx->vertex_shader && ctx->pixel_shader) {
            d3d9_shader_ctx_t *vs = (d3d9_shader_ctx_t *)OBJ_CTX(ctx->vertex_shader);
            d3d9_shader_ctx_t *ps = (d3d9_shader_ctx_t *)OBJ_CTX(ctx->pixel_shader);
            if (vs && ps && vs->msl && ps->msl && hle_d3d9_gpu->create_pipeline) {
                pipe = hle_d3d9_gpu->create_pipeline(vs->msl, ps->msl);
            }
            if (!pipe) {
                fprintf(stderr, "[d3d9] DrawPrimitive: shader pipeline unavailable\n");
                RET(D3DERR_INVALIDCALL, 4);
            }
        } else if (ctx->fvf) {
            use_ffp = 1;
            uint32_t ffp_stride = 0;
            pipe = ensure_ffp_pipeline(ctx, &ffp_stride);
            if (ffp_stride && !ctx->streams[0].stride) stride = ffp_stride;
            if (!pipe) {
                fprintf(stderr, "[d3d9] DrawPrimitive: FFP pipeline unavailable (fvf=0x%x)\n", ctx->fvf);
                RET(D3DERR_INVALIDCALL, 4);
            }
        }

        if (type == D3DPT_TRIANGLEFAN) {
            /* Expand TRIANGLEFAN to an index list; use 32-bit indices to avoid
             * truncation for large vertex ranges or 32-bit index sources. */
            uint32_t tris = d3d9_prim_triangle_count(type, count);
            uint32_t out_indices = 0;
            uint32_t idx_buf[3072];
            uint32_t *p_idx = idx_buf;
            if (tris * 3 > 3072) p_idx = (uint32_t *)malloc(tris * 3 * sizeof(uint32_t));
            if (p_idx && d3d9_prim_expand_nonindexed(type, start, count, p_idx, tris * 3, &out_indices) == 0 && out_indices > 0) {
                void *dyn_ib = ensure_dynamic_ib(ctx, out_indices * 4);
                if (dyn_ib && hle_d3d9_gpu->upload_buffer) {
                    hle_d3d9_gpu->upload_buffer(dyn_ib, 0, p_idx, out_indices * 4);
                    if (hle_d3d9_gpu->draw_indexed32) {
                        hle_d3d9_gpu->draw_indexed32(D3DPT_TRIANGLELIST, 0, 0, out_indices, pipe, vb, dyn_ib, stride);
                    } else if (hle_d3d9_gpu->draw_indexed) {
                        /* Fallback: 16-bit only when all indices fit. */
                        int fits16 = 1;
                        for (uint32_t k = 0; k < out_indices; k++) if (p_idx[k] > 0xFFFFu) { fits16 = 0; break; }
                        if (fits16) {
                            uint16_t *ib16 = (uint16_t *)malloc(out_indices * 2);
                            if (ib16) {
                                for (uint32_t k = 0; k < out_indices; k++) ib16[k] = (uint16_t)p_idx[k];
                                hle_d3d9_gpu->upload_buffer(dyn_ib, 0, ib16, out_indices * 2);
                                hle_d3d9_gpu->draw_indexed(D3DPT_TRIANGLELIST, 0, 0, out_indices, pipe, vb, dyn_ib, stride);
                                free(ib16);
                            }
                        } else {
                            fprintf(stderr, "[d3d9] DrawPrimitive: TRIANGLEFAN needs 32-bit indices\n");
                        }
                    }
                }
            }
            if (p_idx && p_idx != idx_buf) free(p_idx);
        } else {
            uint32_t vert_count = count;
            if (type == D3DPT_TRIANGLELIST) vert_count = count * 3;
            else if (type == D3DPT_TRIANGLESTRIP) vert_count = count + 2;
            else if (type == D3DPT_LINELIST) vert_count = count * 2;
            hle_d3d9_gpu->draw((int)type, start, vert_count, pipe, vb, stride);
        }
        (void)use_ffp;
    }
    RET(D3D_OK, 4);
}

void dev9_DrawIndexedPrimitive(void) {
    /* (this, PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount) */
    uint32_t self = A32(0), type = A32(1), base = A32(2), min_vtx = A32(3), num_vtx = A32(4), start = A32(5), count = A32(6);
    (void)min_vtx;
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    apply_render_state(ctx);
    apply_shader_resources(ctx);
    if (dev9_draw_blocked(ctx)) RET(D3DERR_NOTAVAILABLE, 7);

    void *vb = NULL, *ib = NULL;
    uint32_t stride = 32;
    if (ctx->streams[0].vb) {
        d3d9_vb_ctx_t *vbc = (d3d9_vb_ctx_t *)OBJ_CTX(ctx->streams[0].vb);
        if (vbc) vb = vbc->gpu_buf;
        if (ctx->streams[0].stride) stride = ctx->streams[0].stride;
    }
    d3d9_ib_ctx_t *ibc = NULL;
    if (ctx->indices) {
        ibc = (d3d9_ib_ctx_t *)OBJ_CTX(ctx->indices);
        if (ibc) ib = ibc->gpu_buf;
    }

    void *pipe = NULL;
    if (ctx->vertex_shader && ctx->pixel_shader) {
        d3d9_shader_ctx_t *vs = (d3d9_shader_ctx_t *)OBJ_CTX(ctx->vertex_shader);
        d3d9_shader_ctx_t *ps = (d3d9_shader_ctx_t *)OBJ_CTX(ctx->pixel_shader);
        if (vs && ps && vs->msl && ps->msl && hle_d3d9_gpu && hle_d3d9_gpu->create_pipeline) {
            pipe = hle_d3d9_gpu->create_pipeline(vs->msl, ps->msl);
        }
        if (!pipe) {
            fprintf(stderr, "[d3d9] DrawIndexedPrimitive: shader pipeline unavailable\n");
            RET(D3DERR_INVALIDCALL, 7);
        }
    } else if (ctx->fvf) {
        uint32_t ffp_stride = 0;
        pipe = ensure_ffp_pipeline(ctx, &ffp_stride);
        if (ffp_stride && !ctx->streams[0].stride) stride = ffp_stride;
        if (!pipe) {
            fprintf(stderr, "[d3d9] DrawIndexedPrimitive: FFP pipeline unavailable (fvf=0x%x)\n", ctx->fvf);
            RET(D3DERR_INVALIDCALL, 7);
        }
    }
    if (!ib || !ibc) RET(D3DERR_INVALIDCALL, 7);

    uint32_t idx_fmt = ibc->desc.Format;
    uint32_t idx_stride = (idx_fmt == D3DFMT_INDEX32) ? D3D9_PRIM_IDX32 : D3D9_PRIM_IDX16;

    /* Indexed instancing (public SetStreamSourceFreq + the indexed instancing
     * article): stream 0 carries INDEXEDDATA|numInstances, one stream > 0
     * carries INSTANCEDATA|1, and a vs_3_0 programmable pipeline is required.
     * Everything else is rejected with a log and the correct RET. */
    uint32_t inst_count = 0, inst_stream = 0xFFFFFFFFu, inst_stride = 0;
    void *inst_vb = NULL;
    int instanced = 0;
    for (uint32_t i = 0; i < MAX_STREAMS; i++) {
        uint32_t f = ctx->streams[i].freq;
        if (f & D3DSTREAMSOURCE_INDEXEDDATA) {
            inst_count = f & D3DSTREAMSOURCE_MASK;
            instanced = 1;
        } else if (f & D3DSTREAMSOURCE_INSTANCEDATA) {
            inst_stream = i;
            inst_stride = ctx->streams[i].stride;
            if (ctx->streams[i].vb) {
                d3d9_vb_ctx_t *ivbc = (d3d9_vb_ctx_t *)OBJ_CTX(ctx->streams[i].vb);
                if (ivbc) inst_vb = ivbc->gpu_buf;
            }
        }
    }
    if (instanced) {
        uint32_t vsmaj = 0;
        if (ctx->vertex_shader) {
            d3d9_shader_ctx_t *vs = (d3d9_shader_ctx_t *)OBJ_CTX(ctx->vertex_shader);
            if (vs && vs->tokens) vsmaj = (vs->tokens[0] >> 8u) & 0xFFu;
        }
        if (type == D3DPT_TRIANGLEFAN || !ctx->vertex_shader || !ctx->pixel_shader ||
            vsmaj < 3 || !inst_count || inst_stream == 0xFFFFFFFFu || !inst_vb) {
            fprintf(stderr, "[d3d9] DrawIndexedPrimitive: instancing needs vs_3_0, an instance stream, "
                            "an instance count and no TRIANGLEFAN\n");
            RET(D3DERR_INVALIDCALL, 7);
        }
        if (!hle_d3d9_gpu || !hle_d3d9_gpu->draw_indexed_instanced) {
            fprintf(stderr, "[d3d9] DrawIndexedPrimitive: backend has no instancing support\n");
            RET(D3DERR_NOTAVAILABLE, 7);
        }
        uint32_t index_count = (type == D3DPT_TRIANGLELIST) ? count * 3u :
                               (type == D3DPT_TRIANGLESTRIP) ? count + 2u : count;
        hle_d3d9_gpu->draw_indexed_instanced((int)type, base, start, index_count, inst_count,
                                             pipe, vb, ib, stride, idx_fmt == D3DFMT_INDEX32,
                                             inst_stream, inst_vb, inst_stride);
        RET(D3D_OK, 7);
    }

    if (type == D3DPT_TRIANGLEFAN) {
        /* Expand indexed TRIANGLEFAN to a 32-bit index list to avoid
         * truncating large resolved vertex indices. */
        uint32_t tris = d3d9_prim_triangle_count(type, count);
        uint32_t out_indices = 0;
        uint32_t idx_buf[3072];
        uint32_t *p_idx = idx_buf;
        if (tris * 3 > 3072) p_idx = (uint32_t *)malloc(tris * 3 * sizeof(uint32_t));
        if (p_idx && d3d9_prim_expand_indexed(type, ibc->data, idx_stride, start, count,
                                              base, num_vtx ? num_vtx + base : 0xFFFFFFFFu,
                                              p_idx, tris * 3, &out_indices) == 0 && out_indices > 0) {
            void *dyn_ib = ensure_dynamic_ib(ctx, out_indices * 4);
            if (dyn_ib && hle_d3d9_gpu->upload_buffer) {
                hle_d3d9_gpu->upload_buffer(dyn_ib, 0, p_idx, out_indices * 4);
                if (hle_d3d9_gpu->draw_indexed32) {
                    hle_d3d9_gpu->draw_indexed32(D3DPT_TRIANGLELIST, 0, 0, out_indices, pipe, vb, dyn_ib, stride);
                } else if (hle_d3d9_gpu->draw_indexed) {
                    int fits16 = 1;
                    for (uint32_t k = 0; k < out_indices; k++) if (p_idx[k] > 0xFFFFu) { fits16 = 0; break; }
                    if (fits16) {
                        uint16_t *ib16 = (uint16_t *)malloc(out_indices * 2);
                        if (ib16) {
                            for (uint32_t k = 0; k < out_indices; k++) ib16[k] = (uint16_t)p_idx[k];
                            hle_d3d9_gpu->upload_buffer(dyn_ib, 0, ib16, out_indices * 2);
                            hle_d3d9_gpu->draw_indexed(D3DPT_TRIANGLELIST, 0, 0, out_indices, pipe, vb, dyn_ib, stride);
                            free(ib16);
                        }
                    } else {
                        fprintf(stderr, "[d3d9] DrawIndexedPrimitive: TRIANGLEFAN needs 32-bit indices\n");
                    }
                }
            }
        }
        if (p_idx && p_idx != idx_buf) free(p_idx);
    } else {
        uint32_t index_count = count;
        if (type == D3DPT_TRIANGLELIST) index_count = count * 3;
        else if (type == D3DPT_TRIANGLESTRIP) index_count = count + 2;

        if (idx_fmt == D3DFMT_INDEX32) {
            if (hle_d3d9_gpu && hle_d3d9_gpu->draw_indexed32)
                hle_d3d9_gpu->draw_indexed32((int)type, base, start, index_count, pipe, vb, ib, stride);
            else
                fprintf(stderr, "[d3d9] DrawIndexedPrimitive: 32-bit indices not supported by backend\n");
        } else {
            if (hle_d3d9_gpu && hle_d3d9_gpu->draw_indexed)
                hle_d3d9_gpu->draw_indexed((int)type, base, start, index_count, pipe, vb, ib, stride);
        }
    }
    RET(D3D_OK, 7);
}

void dev9_DrawPrimitiveUP(void) {
    /* (this, PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride) */
    uint32_t self = A32(0);
    uint32_t type = A32(1);
    uint32_t prim_count = A32(2);
    uint32_t data_ptr = A32(3);
    uint32_t stride = A32(4);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);

    apply_render_state(ctx);
    apply_shader_resources(ctx);
    if (dev9_draw_blocked(ctx)) RET(D3DERR_NOTAVAILABLE, 5);
    if (!prim_count || !stride || !data_ptr) RET(D3D_OK, 5);

    uint32_t vert_count = d3d9_prim_vertex_count(type, prim_count);
    if (!vert_count) RET(D3DERR_INVALIDCALL, 5);

    uint32_t data_size = vert_count * stride;
    const uint8_t *verts = (const uint8_t *)hle_ptr(data_ptr);

    /* Create a temp GPU vertex buffer and upload the user-pointer data. */
    if (!hle_d3d9_gpu || !hle_d3d9_gpu->create_buffer || !hle_d3d9_gpu->upload_buffer) {
        fprintf(stderr, "[d3d9] DrawPrimitiveUP: GPU buffer API unavailable\n");
        RET(D3DERR_NOTAVAILABLE, 5);
    }
    void *gpu_vb = hle_d3d9_gpu->create_buffer(data_size, 0);
    if (!gpu_vb) {
        fprintf(stderr, "[d3d9] DrawPrimitiveUP: create_buffer failed\n");
        RET(D3DERR_OUTOFVIDEOMEMORY, 5);
    }
    hle_d3d9_gpu->upload_buffer(gpu_vb, 0, verts, data_size);

    /* Set up the pipeline (FFP or custom shader), same as DrawPrimitive. */
    void *pipe = NULL;
    uint32_t draw_stride = stride;
    if (ctx->vertex_shader && ctx->pixel_shader) {
        d3d9_shader_ctx_t *vs = (d3d9_shader_ctx_t *)OBJ_CTX(ctx->vertex_shader);
        d3d9_shader_ctx_t *ps = (d3d9_shader_ctx_t *)OBJ_CTX(ctx->pixel_shader);
        if (vs && ps && vs->msl && ps->msl && hle_d3d9_gpu->create_pipeline) {
            pipe = hle_d3d9_gpu->create_pipeline(vs->msl, ps->msl);
        }
        if (!pipe) {
            fprintf(stderr, "[d3d9] DrawPrimitiveUP: shader pipeline unavailable\n");
            hle_d3d9_gpu->destroy_buffer(gpu_vb);
            RET(D3DERR_INVALIDCALL, 5);
        }
    } else if (ctx->fvf) {
        uint32_t ffp_stride = 0;
        pipe = ensure_ffp_pipeline(ctx, &ffp_stride);
        if (ffp_stride && !draw_stride) draw_stride = ffp_stride;
        if (!pipe) {
            fprintf(stderr, "[d3d9] DrawPrimitiveUP: FFP pipeline unavailable (fvf=0x%x)\n", ctx->fvf);
            hle_d3d9_gpu->destroy_buffer(gpu_vb);
            RET(D3DERR_INVALIDCALL, 5);
        }
    }

    /* Draw. */
    if (type == D3DPT_TRIANGLEFAN) {
        uint32_t tris = d3d9_prim_triangle_count(type, prim_count);
        uint32_t out_indices = 0;
        uint32_t idx_buf[3072];
        uint32_t *p_idx = idx_buf;
        if (tris * 3 > 3072) p_idx = (uint32_t *)malloc(tris * 3 * sizeof(uint32_t));
        if (p_idx && d3d9_prim_expand_nonindexed(type, 0, vert_count, p_idx, tris * 3, &out_indices) == 0 && out_indices > 0) {
            void *dyn_ib = ensure_dynamic_ib(ctx, out_indices * 4);
            if (dyn_ib && hle_d3d9_gpu->upload_buffer) {
                hle_d3d9_gpu->upload_buffer(dyn_ib, 0, p_idx, out_indices * 4);
                if (hle_d3d9_gpu->draw_indexed32) {
                    hle_d3d9_gpu->draw_indexed32(D3DPT_TRIANGLELIST, 0, 0, out_indices, pipe, gpu_vb, dyn_ib, draw_stride);
                }
            }
        }
        if (p_idx && p_idx != idx_buf) free(p_idx);
    } else {
        if (hle_d3d9_gpu->draw) {
            hle_d3d9_gpu->draw((int)type, 0, vert_count, pipe, gpu_vb, draw_stride);
        }
    }

    hle_d3d9_gpu->destroy_buffer(gpu_vb);
    RET(D3D_OK, 5);
}

void dev9_DrawIndexedPrimitiveUP(void) {
    /* (this, PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride) */
    RET(D3D_OK, 9);
}

/* ProcessVertices: fixed-function vertex processing of stream 0 into a vertex
 * buffer (public IDirect3DDevice9::ProcessVertices). Position is transformed
 * by World * View * Projection; a POSITIONT/XYZRHW destination gets the
 * viewport-mapped (x, y, z, 1/w), a POSITION destination the clip-space
 * position. Every other destination element is copied from the source element
 * with the same usage, usage index and type, else zeroed. A bound vertex
 * shader is not emulated here: that is reported, never faked. */
#define PV_FVF_XYZ      0x002u
#define PV_FVF_XYZRHW   0x004u
#define PV_FVF_POSMASK  0x00Eu
#define PV_FVF_NORMAL   0x010u
#define PV_FVF_DIFFUSE  0x040u
#define PV_FVF_SPECULAR 0x080u
#define PV_FVF_TEXMASK  0xF00u

static uint32_t pv_type_size(uint32_t t) {
    static const uint8_t sz[8] = { 4, 8, 12, 16, 4, 4, 4, 8 };
    return t < 8 ? sz[t] : 0;
}

/* Builds the element list for an FVF (plain XYZ/XYZRHW layouts only).
 * Returns the stride, or 0 when the FVF is not supported. */
static uint32_t pv_fvf_layout(uint32_t fvf, D3DVERTEXELEMENT9 *e, uint32_t *n) {
    uint32_t pos = fvf & PV_FVF_POSMASK, off = 0, k = 0;
    if (fvf & ~(PV_FVF_POSMASK | PV_FVF_NORMAL | PV_FVF_DIFFUSE | PV_FVF_SPECULAR | PV_FVF_TEXMASK)) return 0;
    if (pos == PV_FVF_XYZ) {
        e[k++] = (D3DVERTEXELEMENT9){ 0, (uint16_t)off, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0 }; off += 12;
    } else if (pos == PV_FVF_XYZRHW) {
        e[k++] = (D3DVERTEXELEMENT9){ 0, (uint16_t)off, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_POSITIONT, 0 }; off += 16;
    } else return 0;
    if (fvf & PV_FVF_NORMAL) { e[k++] = (D3DVERTEXELEMENT9){ 0, (uint16_t)off, D3DDECLTYPE_FLOAT3, 0, 3, 0 }; off += 12; }
    if (fvf & PV_FVF_DIFFUSE) { e[k++] = (D3DVERTEXELEMENT9){ 0, (uint16_t)off, 4, 0, 10, 0 }; off += 4; }
    if (fvf & PV_FVF_SPECULAR) { e[k++] = (D3DVERTEXELEMENT9){ 0, (uint16_t)off, 4, 0, 10, 1 }; off += 4; }
    for (uint32_t t = 0; t < ((fvf & PV_FVF_TEXMASK) >> 8); t++) {
        e[k++] = (D3DVERTEXELEMENT9){ 0, (uint16_t)off, D3DDECLTYPE_FLOAT2, 0, 5, (uint8_t)t }; off += 8;
    }
    *n = k;
    return off;
}

static uint32_t pv_decl_layout(const d3d9_decl_ctx_t *d, D3DVERTEXELEMENT9 *e, uint32_t *n) {
    uint32_t k = 0, stride = 0;
    for (uint32_t i = 0; i < d->num_elements && k < 32; i++) {
        if (d->elements[i].Stream != 0) continue;
        uint32_t sz = pv_type_size(d->elements[i].Type);
        e[k++] = d->elements[i];
        if (d->elements[i].Offset + sz > stride) stride = d->elements[i].Offset + sz;
    }
    *n = k;
    return stride;
}

void dev9_ProcessVertices(void) {
    /* (this, SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags) */
    uint32_t self = A32(0), src_start = A32(1), dst_index = A32(2), count = A32(3);
    uint32_t pDest = A32(4), pDecl = A32(5);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    D3DVERTEXELEMENT9 se[32], de[32];
    uint32_t sn = 0, dn = 0, sstride, dstride;

    if (ctx->vertex_shader) {
        fprintf(stderr, "[d3d9] ProcessVertices: a bound vertex shader is not emulated\n");
        RET(D3DERR_NOTAVAILABLE, 7);
    }
    if (!ctx->streams[0].vb || !pDest) RET(D3DERR_INVALIDCALL, 7);
    d3d9_vb_ctx_t *svb = (d3d9_vb_ctx_t *)OBJ_CTX(ctx->streams[0].vb);
    d3d9_vb_ctx_t *dvb = (d3d9_vb_ctx_t *)OBJ_CTX(pDest);
    if (!svb || !dvb || !svb->data || !dvb->data) RET(D3DERR_INVALIDCALL, 7);

    if (ctx->vertex_decl) {
        d3d9_decl_ctx_t *dc = (d3d9_decl_ctx_t *)OBJ_CTX(ctx->vertex_decl);
        sstride = dc ? pv_decl_layout(dc, se, &sn) : 0;
    } else {
        sstride = pv_fvf_layout(ctx->fvf, se, &sn);
    }
    if (pDecl) {
        d3d9_decl_ctx_t *dc = (d3d9_decl_ctx_t *)OBJ_CTX(pDecl);
        dstride = dc ? pv_decl_layout(dc, de, &dn) : 0;
    } else {
        dstride = pv_fvf_layout(dvb->desc.FVF, de, &dn);
    }
    if (!sstride || !dstride) RET(D3DERR_INVALIDCALL, 7);
    if (ctx->streams[0].stride && ctx->streams[0].stride < sstride) RET(D3DERR_INVALIDCALL, 7);
    uint32_t step = ctx->streams[0].stride ? ctx->streams[0].stride : sstride;
    if (count == 0) RET(D3D_OK, 7);
    if ((uint64_t)ctx->streams[0].offset + (uint64_t)(src_start + count) * step > svb->desc.Size ||
        (uint64_t)(dst_index + count) * dstride > dvb->desc.Size) RET(D3DERR_INVALIDCALL, 7);

    const D3DVERTEXELEMENT9 *sp = NULL;
    for (uint32_t i = 0; i < sn; i++)
        if ((se[i].Usage == D3DDECLUSAGE_POSITION || se[i].Usage == D3DDECLUSAGE_POSITIONT) && se[i].UsageIndex == 0) { sp = &se[i]; break; }
    if (!sp || (sp->Type != D3DDECLTYPE_FLOAT3 && sp->Type != D3DDECLTYPE_FLOAT4)) RET(D3DERR_INVALIDCALL, 7);

    /* M = World * View * Projection (row vectors, as in D3D). */
    const D3DMATRIX *W = &ctx->transforms[256], *V = &ctx->transforms[2], *P = &ctx->transforms[3];
    float WV[4][4], M[4][4];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        float a = 0; for (int k = 0; k < 4; k++) a += W->m[i][k] * V->m[k][j];
        WV[i][j] = a;
    }
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        float a = 0; for (int k = 0; k < 4; k++) a += WV[i][k] * P->m[k][j];
        M[i][j] = a;
    }

    for (uint32_t v = 0; v < count; v++) {
        const uint8_t *src = svb->data + ctx->streams[0].offset + (uint64_t)(src_start + v) * step;
        uint8_t *dst = dvb->data + (uint64_t)(dst_index + v) * dstride;
        memset(dst, 0, dstride);
        float in[4] = { 0, 0, 0, 1 }, out[4];
        memcpy(in, src + sp->Offset, sp->Type == D3DDECLTYPE_FLOAT4 ? 12 : 12);
        if (sp->Type == D3DDECLTYPE_FLOAT4) memcpy(&in[3], src + sp->Offset + 12, 4);
        int pre = sp->Usage == D3DDECLUSAGE_POSITIONT;   /* already transformed */
        for (int j = 0; j < 4; j++)
            out[j] = pre ? in[j] : in[0] * M[0][j] + in[1] * M[1][j] + in[2] * M[2][j] + in[3] * M[3][j];
        for (uint32_t i = 0; i < dn; i++) {
            uint8_t *d = dst + de[i].Offset;
            uint32_t sz = pv_type_size(de[i].Type);
            if (de[i].Usage == D3DDECLUSAGE_POSITIONT || (de[i].Usage == D3DDECLUSAGE_POSITION && de[i].UsageIndex == 0)) {
                float r[4];
                if (de[i].Usage == D3DDECLUSAGE_POSITIONT && !pre) {
                    /* POSITION source → POSITIONT dest: clip → viewport. */
                    float rhw = out[3] != 0.0f ? 1.0f / out[3] : 1.0f;
                    r[0] = (float)ctx->viewport.X + (out[0] * rhw * 0.5f + 0.5f) * (float)ctx->viewport.Width;
                    r[1] = (float)ctx->viewport.Y + (1.0f - (out[1] * rhw * 0.5f + 0.5f)) * (float)ctx->viewport.Height;
                    r[2] = ctx->viewport.MinZ + out[2] * rhw * (ctx->viewport.MaxZ - ctx->viewport.MinZ);
                    r[3] = rhw;
                } else if (de[i].Usage == D3DDECLUSAGE_POSITION && pre) {
                    /* POSITIONT source → POSITION dest: viewport → clip. */
                    float w_clip = out[3] != 0.0f ? 1.0f / out[3] : 1.0f;
                    float z_range = ctx->viewport.MaxZ - ctx->viewport.MinZ;
                    float x_ndc = 2.0f * (out[0] - (float)ctx->viewport.X) / (float)ctx->viewport.Width - 1.0f;
                    float y_ndc = 1.0f - 2.0f * (out[1] - (float)ctx->viewport.Y) / (float)ctx->viewport.Height;
                    float z_ndc = z_range != 0.0f ? 2.0f * (out[2] - ctx->viewport.MinZ) / z_range - 1.0f : 2.0f * out[2] - 1.0f;
                    r[0] = x_ndc * w_clip;
                    r[1] = y_ndc * w_clip;
                    r[2] = z_ndc * w_clip;
                    r[3] = w_clip;
                } else {
                    memcpy(r, out, sizeof r);
                }
                memcpy(d, r, sz < 16 ? sz : 16);
                continue;
            }
            for (uint32_t k = 0; k < sn; k++)
                if (se[k].Usage == de[i].Usage && se[k].UsageIndex == de[i].UsageIndex && se[k].Type == de[i].Type) {
                    memcpy(d, src + se[k].Offset, sz);
                    break;
                }
        }
    }
    RET(D3D_OK, 7);
}
void dev9_CreateVertexDeclaration(void);

void dev9_SetVertexDeclaration(void) {
    uint32_t self = A32(0);
    uint32_t pDecl = A32(1);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    ctx->vertex_decl = pDecl;
    sb9_mark(ctx, SBK_DECL, 0, 1);
    RET(D3D_OK, 2);
}

void dev9_GetVertexDeclaration(void) {
    uint32_t self = A32(0);
    uint32_t ppDecl = A32(1);
    if (!ppDecl) RET(D3DERR_INVALIDCALL, 2);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    MEM32(ppDecl) = ctx->vertex_decl;
    if (ctx->vertex_decl) MEM32(ctx->vertex_decl + 4)++;
    RET(D3D_OK, 2);
}

void dev9_SetFVF(void) {
    uint32_t self = A32(0);
    uint32_t fvf = A32(1);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    ctx->fvf = fvf;
    sb9_mark(ctx, SBK_FVF, 0, 1);
    RET(D3D_OK, 2);
}

void dev9_GetFVF(void) {
    uint32_t self = A32(0);
    uint32_t pFvf = A32(1);
    if (!pFvf) RET(D3DERR_INVALIDCALL, 2);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    MEM32(pFvf) = ctx->fvf;
    RET(D3D_OK, 2);
}

void dev9_CreateVertexShader(void);

void dev9_SetVertexShader(void) {
    uint32_t self = A32(0);
    uint32_t pShader = A32(1);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    ctx->vertex_shader = pShader;
    sb9_mark(ctx, SBK_VS, 0, 1);
    RET(D3D_OK, 2);
}

void dev9_GetVertexShader(void) {
    uint32_t self = A32(0);
    uint32_t ppShader = A32(1);
    if (!ppShader) RET(D3DERR_INVALIDCALL, 2);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    MEM32(ppShader) = ctx->vertex_shader;
    if (ctx->vertex_shader) MEM32(ctx->vertex_shader + 4)++;
    RET(D3D_OK, 2);
}

void dev9_SetVertexShaderConstantF(void) {
    uint32_t self = A32(0);
    uint32_t start = A32(1);
    uint32_t pData = A32(2);
    uint32_t count = A32(3);
    if (!pData) RET(D3DERR_INVALIDCALL, 4);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    if (start < MAX_VS_CONST_F) {
        uint32_t n = count;
        if (start + n > MAX_VS_CONST_F) n = MAX_VS_CONST_F - start;
        memcpy(&ctx->vs_const_f[start], hle_ptr(pData), n * 16);
        sb9_mark(ctx, SBK_VSF, start, n);
    }
    RET(D3D_OK, 4);
}

void dev9_GetVertexShaderConstantF(void) {
    uint32_t self = A32(0);
    uint32_t start = A32(1);
    uint32_t pData = A32(2);
    uint32_t count = A32(3);
    if (!pData) RET(D3DERR_INVALIDCALL, 4);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    if (start < MAX_VS_CONST_F) {
        uint32_t n = count;
        if (start + n > MAX_VS_CONST_F) n = MAX_VS_CONST_F - start;
        memcpy(hle_ptr(pData), &ctx->vs_const_f[start], n * 16);
    }
    RET(D3D_OK, 4);
}

void dev9_SetVertexShaderConstantI(void) {
    uint32_t self = A32(0), start = A32(1), pData = A32(2), count = A32(3);
    if (!pData) RET(D3DERR_INVALIDCALL, 4);
    if (start >= D3D9_MAX_VS_CONST_I) RET(D3DERR_INVALIDCALL, 4);
    uint32_t n = count;
    if (n > D3D9_MAX_VS_CONST_I - start) n = D3D9_MAX_VS_CONST_I - start;
    memcpy(&((d3d9_device_ctx_t *)OBJ_CTX(self))->vs_const_i[start], hle_ptr(pData), n * 16);
    sb9_mark((d3d9_device_ctx_t *)OBJ_CTX(self), SBK_VSI, start, n);
    RET(D3D_OK, 4);
}
void dev9_GetVertexShaderConstantI(void) {
    uint32_t self = A32(0), start = A32(1), pData = A32(2), count = A32(3);
    if (!pData) RET(D3DERR_INVALIDCALL, 4);
    if (start >= D3D9_MAX_VS_CONST_I) RET(D3DERR_INVALIDCALL, 4);
    uint32_t n = count;
    if (n > D3D9_MAX_VS_CONST_I - start) n = D3D9_MAX_VS_CONST_I - start;
    memcpy(hle_ptr(pData), &((d3d9_device_ctx_t *)OBJ_CTX(self))->vs_const_i[start], n * 16);
    RET(D3D_OK, 4);
}
void dev9_SetVertexShaderConstantB(void) {
    uint32_t self = A32(0), start = A32(1), pData = A32(2), count = A32(3);
    if (!pData) RET(D3DERR_INVALIDCALL, 4);
    if (start >= D3D9_MAX_VS_CONST_B) RET(D3DERR_INVALIDCALL, 4);
    uint32_t n = count;
    if (n > D3D9_MAX_VS_CONST_B - start) n = D3D9_MAX_VS_CONST_B - start;
    memcpy(&((d3d9_device_ctx_t *)OBJ_CTX(self))->vs_const_b[start], hle_ptr(pData), n * 4);
    sb9_mark((d3d9_device_ctx_t *)OBJ_CTX(self), SBK_VSB, start, n);
    RET(D3D_OK, 4);
}
void dev9_GetVertexShaderConstantB(void) {
    uint32_t self = A32(0), start = A32(1), pData = A32(2), count = A32(3);
    if (!pData) RET(D3DERR_INVALIDCALL, 4);
    if (start >= D3D9_MAX_VS_CONST_B) RET(D3DERR_INVALIDCALL, 4);
    uint32_t n = count;
    if (n > D3D9_MAX_VS_CONST_B - start) n = D3D9_MAX_VS_CONST_B - start;
    memcpy(hle_ptr(pData), &((d3d9_device_ctx_t *)OBJ_CTX(self))->vs_const_b[start], n * 4);
    RET(D3D_OK, 4);
}

void dev9_SetStreamSource(void) {
    uint32_t self = A32(0);
    uint32_t stream = A32(1);
    uint32_t pData = A32(2);
    uint32_t offset = A32(3);
    uint32_t stride = A32(4);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    if (stream < MAX_STREAMS) {
        ctx->streams[stream].vb = pData;
        ctx->streams[stream].offset = offset;
        ctx->streams[stream].stride = stride;
        sb9_mark(ctx, SBK_STREAM, stream, 1);
    }
    RET(D3D_OK, 5);
}

void dev9_GetStreamSource(void) {
    uint32_t self = A32(0);
    uint32_t stream = A32(1);
    uint32_t ppData = A32(2);
    uint32_t pOffset = A32(3);
    uint32_t pStride = A32(4);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    if (stream < MAX_STREAMS) {
        if (ppData) {
            MEM32(ppData) = ctx->streams[stream].vb;
            if (ctx->streams[stream].vb) MEM32(ctx->streams[stream].vb + 4)++;
        }
        if (pOffset) MEM32(pOffset) = ctx->streams[stream].offset;
        if (pStride) MEM32(pStride) = ctx->streams[stream].stride;
    }
    RET(D3D_OK, 5);
}

void dev9_SetStreamSourceFreq(void) {
    /* (this, StreamNumber, Setting) - public d3d9.h / SetStreamSourceFreq.
     * High two bits select indexed / instanced data; low bits are the count
     * (INDEXEDDATA) or divisor (INSTANCEDATA). See public d3d9types.h:
     *   D3DSTREAMSOURCE_INDEXEDDATA  (1u << 30)
     *   D3DSTREAMSOURCE_INSTANCEDATA (2u << 30)
     * Plain values are frequency dividers and are stored unchanged. */
    uint32_t self = A32(0), stream = A32(1), setting = A32(2);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    if (!ctx || stream >= MAX_STREAMS) {
        fprintf(stderr, "[d3d9] SetStreamSourceFreq: invalid stream %u\n", stream);
        RET(D3DERR_INVALIDCALL, 3);
    }
    uint32_t kind = setting & 0xC0000000u;
    uint32_t low  = setting & D3DSTREAMSOURCE_MASK;
    if (kind == D3DSTREAMSOURCE_INDEXEDDATA) {
        /* Public docs: index data must be in stream 0 with a nonzero count. */
        if (stream != 0 || low == 0) {
            fprintf(stderr, "[d3d9] SetStreamSourceFreq: INDEXEDDATA requires stream 0 and count>0 (%u, %u)\n",
                    stream, low);
            RET(D3DERR_INVALIDCALL, 3);
        }
    } else if (kind == D3DSTREAMSOURCE_INSTANCEDATA) {
        /* Instance data lives in a stream > 0 with a divisor of 1. */
        if (stream == 0 || low != 1) {
            fprintf(stderr, "[d3d9] SetStreamSourceFreq: INSTANCEDATA requires stream>0 and divisor 1 (%u, %u)\n",
                    stream, low);
            RET(D3DERR_INVALIDCALL, 3);
        }
    } else if (kind != 0) {
        fprintf(stderr, "[d3d9] SetStreamSourceFreq: reserved frequency bits set (0x%x)\n", setting);
        RET(D3DERR_INVALIDCALL, 3);
    }
    ctx->streams[stream].freq = setting;
    sb9_mark(ctx, SBK_FREQ, stream, 1);
    RET(D3D_OK, 3);
}

void dev9_GetStreamSourceFreq(void) {
    uint32_t self = A32(0), stream = A32(1), pSetting = A32(2);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    if (pSetting) {
        MEM32(pSetting) = (ctx && stream < MAX_STREAMS) ? ctx->streams[stream].freq : 0;
    }
    RET(D3D_OK, 3);
}

void dev9_SetIndices(void) {
    uint32_t self = A32(0);
    uint32_t pIndices = A32(1);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    ctx->indices = pIndices;
    sb9_mark(ctx, SBK_INDICES, 0, 1);
    RET(D3D_OK, 2);
}

void dev9_GetIndices(void) {
    uint32_t self = A32(0);
    uint32_t ppIndices = A32(1);
    if (!ppIndices) RET(D3DERR_INVALIDCALL, 2);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    MEM32(ppIndices) = ctx->indices;
    if (ctx->indices) MEM32(ctx->indices + 4)++;
    RET(D3D_OK, 2);
}

void dev9_CreatePixelShader(void);

void dev9_SetPixelShader(void) {
    uint32_t self = A32(0);
    uint32_t pShader = A32(1);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    ctx->pixel_shader = pShader;
    sb9_mark(ctx, SBK_PS, 0, 1);
    RET(D3D_OK, 2);
}

void dev9_GetPixelShader(void) {
    uint32_t self = A32(0);
    uint32_t ppShader = A32(1);
    if (!ppShader) RET(D3DERR_INVALIDCALL, 2);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    MEM32(ppShader) = ctx->pixel_shader;
    if (ctx->pixel_shader) MEM32(ctx->pixel_shader + 4)++;
    RET(D3D_OK, 2);
}

void dev9_SetPixelShaderConstantF(void) {
    uint32_t self = A32(0);
    uint32_t start = A32(1);
    uint32_t pData = A32(2);
    uint32_t count = A32(3);
    if (!pData) RET(D3DERR_INVALIDCALL, 4);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    if (start < MAX_PS_CONST_F) {
        uint32_t n = count;
        if (start + n > MAX_PS_CONST_F) n = MAX_PS_CONST_F - start;
        memcpy(&ctx->ps_const_f[start], hle_ptr(pData), n * 16);
        sb9_mark(ctx, SBK_PSF, start, n);
    }
    RET(D3D_OK, 4);
}

void dev9_GetPixelShaderConstantF(void) {
    uint32_t self = A32(0);
    uint32_t start = A32(1);
    uint32_t pData = A32(2);
    uint32_t count = A32(3);
    if (!pData) RET(D3DERR_INVALIDCALL, 4);
    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)OBJ_CTX(self);
    if (start < MAX_PS_CONST_F) {
        uint32_t n = count;
        if (start + n > MAX_PS_CONST_F) n = MAX_PS_CONST_F - start;
        memcpy(hle_ptr(pData), &ctx->ps_const_f[start], n * 16);
    }
    RET(D3D_OK, 4);
}

void dev9_SetPixelShaderConstantI(void) {
    uint32_t self = A32(0), start = A32(1), pData = A32(2), count = A32(3);
    if (!pData) RET(D3DERR_INVALIDCALL, 4);
    if (start >= D3D9_MAX_PS_CONST_I) RET(D3DERR_INVALIDCALL, 4);
    uint32_t n = count;
    if (n > D3D9_MAX_PS_CONST_I - start) n = D3D9_MAX_PS_CONST_I - start;
    memcpy(&((d3d9_device_ctx_t *)OBJ_CTX(self))->ps_const_i[start], hle_ptr(pData), n * 16);
    sb9_mark((d3d9_device_ctx_t *)OBJ_CTX(self), SBK_PSI, start, n);
    RET(D3D_OK, 4);
}
void dev9_GetPixelShaderConstantI(void) {
    uint32_t self = A32(0), start = A32(1), pData = A32(2), count = A32(3);
    if (!pData) RET(D3DERR_INVALIDCALL, 4);
    if (start >= D3D9_MAX_PS_CONST_I) RET(D3DERR_INVALIDCALL, 4);
    uint32_t n = count;
    if (n > D3D9_MAX_PS_CONST_I - start) n = D3D9_MAX_PS_CONST_I - start;
    memcpy(hle_ptr(pData), &((d3d9_device_ctx_t *)OBJ_CTX(self))->ps_const_i[start], n * 16);
    RET(D3D_OK, 4);
}
void dev9_SetPixelShaderConstantB(void) {
    uint32_t self = A32(0), start = A32(1), pData = A32(2), count = A32(3);
    if (!pData) RET(D3DERR_INVALIDCALL, 4);
    if (start >= D3D9_MAX_PS_CONST_B) RET(D3DERR_INVALIDCALL, 4);
    uint32_t n = count;
    if (n > D3D9_MAX_PS_CONST_B - start) n = D3D9_MAX_PS_CONST_B - start;
    memcpy(&((d3d9_device_ctx_t *)OBJ_CTX(self))->ps_const_b[start], hle_ptr(pData), n * 4);
    sb9_mark((d3d9_device_ctx_t *)OBJ_CTX(self), SBK_PSB, start, n);
    RET(D3D_OK, 4);
}
void dev9_GetPixelShaderConstantB(void) {
    uint32_t self = A32(0), start = A32(1), pData = A32(2), count = A32(3);
    if (!pData) RET(D3DERR_INVALIDCALL, 4);
    if (start >= D3D9_MAX_PS_CONST_B) RET(D3DERR_INVALIDCALL, 4);
    uint32_t n = count;
    if (n > D3D9_MAX_PS_CONST_B - start) n = D3D9_MAX_PS_CONST_B - start;
    memcpy(hle_ptr(pData), &((d3d9_device_ctx_t *)OBJ_CTX(self))->ps_const_b[start], n * 4);
    RET(D3D_OK, 4);
}
void dev9_DrawRectPatch(void) {
    fprintf(stderr, "[d3d9] IDirect3DDevice9::DrawRectPatch: not implemented\n");
    RET(D3DERR_NOTAVAILABLE, 4);
}
void dev9_DrawTriPatch(void) {
    fprintf(stderr, "[d3d9] IDirect3DDevice9::DrawTriPatch: not implemented\n");
    RET(D3DERR_NOTAVAILABLE, 4);
}
void dev9_DeletePatch(void) {
    /* Public D3D9 docs: DeletePatch frees a high-order patch previously
     * cached by DrawRectPatch/DrawTriPatch. Both of those are unimplemented
     * here and return D3DERR_NOTAVAILABLE, so no patch is ever cached and
     * there is nothing to free; D3D_OK is the documented success result. */
    RET(D3D_OK, 2);
}
void dev9_CreateQuery(void);
