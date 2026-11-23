/*
 * d3d9_selftest.c - Selftest for win32hle Direct3D 9 implementation.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "win32hle.h"
#include "host.h"
#include "d3d9_hle.h"
#include "d3d9_shader.h"

static int fails;
static void ok(const char *what, int cond) {
    if (!cond) {
        printf("FAIL %s\n", what);
        fails++;
    }
}

static uint32_t call(const char *name, int n, const uint32_t *a) {
    return hle_call_guest(hle_resolve(name), n, a);
}

#define A(p) hle_va(p)

/* --- mock GPU backend: records (re)initialization of the render targets --- */
static int g_gpu_init_calls, g_gpu_init_w, g_gpu_init_h;
static const d3d9_gpu_ops_t *g_gpu_prev;
static int mock_is_active(void) { return 1; }
static int mock_enable(void) { return 0; }
static int mock_init(uint32_t w, uint32_t h) {
    g_gpu_init_calls++; g_gpu_init_w = (int)w; g_gpu_init_h = (int)h; return 0;
}
static void mock_shutdown(void) {}
static int mock_begin(void) { return 0; }
static int mock_end(void) { return 0; }
static int mock_clear(uint32_t f, uint32_t c, float z, uint32_t s) {
    (void)f; (void)c; (void)z; (void)s; return 0;
}
static int mock_present(void) { return 0; }
static void *mock_tex(uint32_t w, uint32_t h, uint32_t f, int c, uint32_t levels) {
    (void)w; (void)h; (void)f; (void)c; (void)levels; return (void *)1;
}
static void mock_tex_destroy(void *p) { (void)p; }
static void mock_tex_upload(void *t, int f, int l, const void *b, uint32_t r) {
    (void)t; (void)f; (void)l; (void)b; (void)r;
}
static void *mock_buf(uint32_t s, int i) { (void)s; (void)i; return (void *)2; }
static void mock_buf_destroy(void *p) { (void)p; }
static void mock_buf_upload(void *b, uint32_t o, const void *d, uint32_t s) {
    (void)b; (void)o; (void)d; (void)s;
}
static void *mock_pipe(const char *v, const char *p) { (void)v; (void)p; return (void *)3; }
static void mock_pipe_destroy(void *p) { (void)p; }
static int mock_draw(int t, uint32_t s, uint32_t c, void *p, void *v, uint32_t st) {
    (void)t; (void)s; (void)c; (void)p; (void)v; (void)st; return 0;
}
static void mock_set_rt(void *rt) { (void)rt; }
static void *mock_create_rt(uint32_t w, uint32_t h) { (void)w; (void)h; return (void *)4; }
static void mock_destroy_rt(void *rt) { (void)rt; }
static void mock_set_tex(void *tex) { (void)tex; }

/* Mock query backend: deterministic values so the shim's relay, flag checks
 * and error mapping are observable. The real occlusion counting is covered by
 * tests/d3d9-programme/test_d3d9_query.c against the Metal backend. */
static int g_q_created, g_q_issues, g_q_reads, g_q_destroyed;
static void *mock_query_create(uint32_t type) {
    g_q_created++;
    return (void *)(uintptr_t)(0x1000u + type);      /* opaque token, never dereferenced */
}
static int mock_query_issue(void *q, uint32_t flags) {
    g_q_issues++;
    if (!q) return -1;
    return (flags == D3DISSUE_BEGIN || flags == D3DISSUE_END) ? 0 : -1;
}
static int mock_query_get_data(void *q, void *out, uint32_t size, uint32_t flags) {
    g_q_reads++;
    if (!q || !out || size < 4) return -1;
    *(uint32_t *)out = (flags & D3DGETDATA_FLUSH) ? 0x5A5A1234u : 0u;
    return (flags & D3DGETDATA_FLUSH) ? 0 : 1;       /* S_OK with FLUSH, S_FALSE without */
}
static void mock_query_destroy(void *q) { (void)q; g_q_destroyed++; }

/* State hooks added for the clip-plane / gamma-ramp / instancing work. */
static int g_gamma_calls, g_clip_calls, g_inst_draws;
static uint32_t g_last_clip_mask, g_last_scissor_enable;
static int32_t g_last_scissor[4];
static void mock_gamma(const uint16_t ramp[256][3]) { (void)ramp; g_gamma_calls++; }
static void mock_clip(const float planes[6][4], uint32_t mask) {
    (void)planes; g_clip_calls++; g_last_clip_mask = mask;
}
static void mock_set_render_state(uint32_t blend_enable, uint32_t src_blend, uint32_t dst_blend,
                                  uint32_t z_enable, uint32_t z_write, uint32_t z_func,
                                  uint32_t cull_mode, uint32_t scissor_enable, const int32_t scissor[4]) {
    (void)blend_enable; (void)src_blend; (void)dst_blend; (void)z_enable;
    (void)z_write; (void)z_func; (void)cull_mode;
    g_last_scissor_enable = scissor_enable;
    if (scissor) memcpy(g_last_scissor, scissor, sizeof g_last_scissor);
}
static void mock_divisor(uint32_t stream, uint32_t divisor) { (void)stream; (void)divisor; }
static int mock_draw_instanced(int t, uint32_t s, uint32_t c, uint32_t n, void *p, void *v, uint32_t st) {
    (void)t; (void)s; (void)c; (void)n; (void)p; (void)v; (void)st; g_inst_draws++; return 0;
}
static int mock_draw_indexed_instanced(int t, uint32_t b, uint32_t s, uint32_t c, uint32_t n,
                                       void *p, void *v, void *i, uint32_t st, int i32,
                                       uint32_t is, void *iv, uint32_t ist) {
    (void)t; (void)b; (void)s; (void)c; (void)n; (void)p; (void)v; (void)i; (void)st;
    (void)i32; (void)is; (void)iv; (void)ist; g_inst_draws++; return 0;
}

static const d3d9_gpu_ops_t g_mock_gpu = {
    "mock", mock_is_active, mock_enable, mock_init, mock_shutdown,
    mock_begin, mock_end, mock_clear, mock_present,
    mock_tex, mock_tex_destroy, mock_tex_upload,
    mock_buf, mock_buf_destroy, mock_buf_upload,
    mock_pipe, mock_pipe_destroy, mock_draw,
    mock_set_rt, mock_create_rt, mock_destroy_rt, mock_set_tex,
    .set_clip_planes = mock_clip,
    .set_render_state = mock_set_render_state,
    .set_gamma_ramp = mock_gamma,
    .set_stream_divisor = mock_divisor,
    .draw_instanced = mock_draw_instanced,
    .draw_indexed_instanced = mock_draw_indexed_instanced,
    .query_create = mock_query_create,
    .query_issue = mock_query_issue,
    .query_get_data = mock_query_get_data,
    .query_destroy = mock_query_destroy
};

const recomp_dispatch_entry_t recomp_dispatch_table[1];
const uint32_t recomp_dispatch_count = 0;

int main(void) {
    recomp_host_init();
    win32hle_register(win32hle_d3d9);

    /* 1. Direct3DCreate9 */
    uint32_t a_create[1] = { 32 };
    uint32_t d3d9 = call("Direct3DCreate9", 1, a_create);
    ok("Direct3DCreate9 returns object", d3d9 != 0);

    /* 2. GetAdapterCount */
    uint32_t a_count[1] = { d3d9 };
    uint32_t count = call("IDirect3D9::GetAdapterCount", 1, a_count);
    ok("GetAdapterCount == 1", count == 1);

    /* 3. GetAdapterIdentifier */
    D3DADAPTER_IDENTIFIER9 *ident = (D3DADAPTER_IDENTIFIER9 *)hle_alloc(sizeof(D3DADAPTER_IDENTIFIER9));
    uint32_t a_ident[4] = { d3d9, 0, 0, A(ident) };
    uint32_t hr = call("IDirect3D9::GetAdapterIdentifier", 4, a_ident);
    ok("GetAdapterIdentifier hr == 0", hr == D3D_OK);
    ok("GetAdapterIdentifier generic VendorId", ident->VendorId == 0x0000);
    ok("GetAdapterIdentifier generic Description", strstr(ident->Description, "D3D9 HLE") != NULL);
    ok("GetAdapterIdentifier generic Driver", strstr(ident->Driver, "d3d9hle.dll") != NULL);

    /* 4. GetDeviceCaps (Shader Model 3.0) */
    D3DCAPS9 *caps = (D3DCAPS9 *)hle_alloc(sizeof(D3DCAPS9));
    uint32_t a_caps[4] = { d3d9, 0, D3DDEVTYPE_HAL, A(caps) };
    hr = call("IDirect3D9::GetDeviceCaps", 4, a_caps);
    ok("GetDeviceCaps hr == 0", hr == D3D_OK);
    ok("VertexShaderVersion >= SM3.0", caps->VertexShaderVersion >= D3DVS_VERSION(3, 0));
    ok("PixelShaderVersion >= SM3.0", caps->PixelShaderVersion >= D3DPS_VERSION(3, 0));
    ok("DevCaps has HWTRANSFORMANDLIGHT", (caps->DevCaps & 0x00020000u) != 0);
    ok("MaxTextureWidth >= 4096", caps->MaxTextureWidth >= 4096);
    ok("NumSimultaneousRTs == 1 (only RT0 bound)", caps->NumSimultaneousRTs == 1);

    /* 5. Adapter modes & display modes */
    uint32_t a_modecount[3] = { d3d9, 0, D3DFMT_X8R8G8B8 };
    uint32_t mcount = call("IDirect3D9::GetAdapterModeCount", 3, a_modecount);
    ok("GetAdapterModeCount > 0", mcount > 0);

    D3DDISPLAYMODE *disp = (D3DDISPLAYMODE *)hle_alloc(sizeof(D3DDISPLAYMODE));
    uint32_t a_disp[3] = { d3d9, 0, A(disp) };
    hr = call("IDirect3D9::GetAdapterDisplayMode", 3, a_disp);
    ok("GetAdapterDisplayMode hr == 0", hr == D3D_OK);
    ok("GetAdapterDisplayMode Width > 0", disp->Width > 0 && disp->Height > 0);

    /* 6. CheckDeviceType, CheckDeviceFormat, MultiSample */
    uint32_t a_chktype[6] = { d3d9, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8, 1 };
    ok("CheckDeviceType D3D_OK", call("IDirect3D9::CheckDeviceType", 6, a_chktype) == D3D_OK);

    uint32_t a_chkfmt[7] = { d3d9, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, D3DFMT_A8R8G8B8 };
    ok("CheckDeviceFormat D3D_OK", call("IDirect3D9::CheckDeviceFormat", 7, a_chkfmt) == D3D_OK);

    uint32_t *quality = (uint32_t *)hle_alloc(4);
    uint32_t a_ms[7] = { d3d9, 0, D3DDEVTYPE_HAL, D3DFMT_A8R8G8B8, 1, 0, A(quality) };
    ok("CheckDeviceMultiSampleType D3D_OK", call("IDirect3D9::CheckDeviceMultiSampleType", 7, a_ms) == D3D_OK);

    /* 7. CreateDevice with typical parameters (0x44 behavior flags, 1280x720, A8R8G8B8, D24S8) */
    D3DPRESENT_PARAMETERS *pp = (D3DPRESENT_PARAMETERS *)hle_alloc(sizeof(D3DPRESENT_PARAMETERS));
    memset(pp, 0, sizeof(*pp));
    pp->BackBufferWidth = 1280;
    pp->BackBufferHeight = 720;
    pp->BackBufferFormat = D3DFMT_A8R8G8B8;
    pp->BackBufferCount = 1;
    pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp->Windowed = 1;
    pp->EnableAutoDepthStencil = 1;
    pp->AutoDepthStencilFormat = D3DFMT_D24S8;
    pp->Flags = 0;

    /* Paket 2: the GPU backend must be sized from the backbuffer here,
     * otherwise the Metal render target stays nil and nothing is drawn. */
    g_gpu_prev = hle_d3d9_gpu;
    hle_d3d9_set_gpu(&g_mock_gpu);

    uint32_t *pDevOut = (uint32_t *)hle_alloc(4);
    uint32_t a_create_dev[7] = {
        d3d9, 0, D3DDEVTYPE_HAL, 0x1001,
        0x44,   /* D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED */
        A(pp), A(pDevOut)
    };
    hr = call("IDirect3D9::CreateDevice", 7, a_create_dev);
    ok("CreateDevice hr == 0", hr == D3D_OK);
    uint32_t dev = *pDevOut;
    ok("CreateDevice returned non-null device", dev != 0);
    ok("CreateDevice initialized GPU backend", g_gpu_init_calls == 1);
    ok("CreateDevice GPU size == 1280x720", g_gpu_init_w == 1280 && g_gpu_init_h == 720);

    /* 8. Device state and parameters */
    D3DVIEWPORT9 *vp = (D3DVIEWPORT9 *)hle_alloc(sizeof(D3DVIEWPORT9));
    uint32_t a_vp[2] = { dev, A(vp) };
    hr = call("IDirect3DDevice9::GetViewport", 2, a_vp);
    ok("GetViewport hr == 0", hr == D3D_OK);
    ok("Viewport width == 1280", vp->Width == 1280);
    ok("Viewport height == 720", vp->Height == 720);

    /* GetSwapChain: applications keep the default swap chain and call
     * Present/GetBackBuffer on it. A stub that returns D3D_OK without writing
     * the out-pointer would leave that pointer null. */
    uint32_t a_nsc[1] = { dev };
    ok("GetNumberOfSwapChains == 1",
       call("IDirect3DDevice9::GetNumberOfSwapChains", 1, a_nsc) == 1);

    uint32_t swap = 0, *pSwap = (uint32_t *)hle_alloc(4);
    uint32_t a_gsc[3] = { dev, 0, A(pSwap) };
    hr = call("IDirect3DDevice9::GetSwapChain", 3, a_gsc);
    ok("GetSwapChain(0) hr == 0", hr == D3D_OK);
    ok("GetSwapChain(0) filled non-null out-pointer", *pSwap != 0);
    swap = *pSwap;

    /* Repeated calls must return the same default swap chain. */
    *pSwap = 0;
    call("IDirect3DDevice9::GetSwapChain", 3, a_gsc);
    ok("GetSwapChain(0) returns same pointer", *pSwap == swap);

    /* Non-default index must fail and leave a null pointer. */
    uint32_t a_gsc_bad[3] = { dev, 1, A(pSwap) };
    *pSwap = 0xDEADBEEFu;
    hr = call("IDirect3DDevice9::GetSwapChain", 3, a_gsc_bad);
    ok("GetSwapChain(1) fails", hr == D3DERR_INVALIDCALL);
    ok("GetSwapChain(1) returns null", *pSwap == 0);

    /* The swap chain methods an application calls every frame. */
    uint32_t a_sc_present[6] = { swap, 0, 0, 0, 0, 0 };
    ok("SwapChain::Present hr == 0",
       call("IDirect3DSwapChain9::Present", 6, a_sc_present) == D3D_OK);

    uint32_t *pScDev = (uint32_t *)hle_alloc(4);
    uint32_t a_sc_getdev[2] = { swap, A(pScDev) };
    ok("SwapChain::GetDevice hr == 0",
       call("IDirect3DSwapChain9::GetDevice", 2, a_sc_getdev) == D3D_OK);
    ok("SwapChain::GetDevice returns the device", *pScDev == dev);

    uint32_t *pScBb = (uint32_t *)hle_alloc(4);
    uint32_t a_sc_getbb[4] = { swap, 0, 0, A(pScBb) };
    ok("SwapChain::GetBackBuffer hr == 0",
       call("IDirect3DSwapChain9::GetBackBuffer", 4, a_sc_getbb) == D3D_OK);
    ok("SwapChain::GetBackBuffer returns a surface", *pScBb != 0);

    /* Reset resizes the GPU targets and resets the viewport to the new
     * backbuffer, then we restore the original parameters for later tests. */
    D3DPRESENT_PARAMETERS *pp2 = (D3DPRESENT_PARAMETERS *)hle_alloc(sizeof(D3DPRESENT_PARAMETERS));
    memcpy(pp2, pp, sizeof(*pp));
    pp2->BackBufferWidth = 800;
    pp2->BackBufferHeight = 600;
    uint32_t a_reset[2] = { dev, A(pp2) };
    ok("Reset hr == 0", call("IDirect3DDevice9::Reset", 2, a_reset) == D3D_OK);
    ok("Reset reinitialized GPU backend with 800x600",
       g_gpu_init_calls == 2 && g_gpu_init_w == 800 && g_gpu_init_h == 600);
    uint32_t a_vp2[2] = { dev, A(vp) };
    call("IDirect3DDevice9::GetViewport", 2, a_vp2);
    ok("Reset viewport == 800x600", vp->Width == 800 && vp->Height == 600);
    memcpy(pp2, pp, sizeof(*pp));
    call("IDirect3DDevice9::Reset", 2, a_reset);
    ok("Reset restored GPU backend to 1280x720",
       g_gpu_init_calls == 3 && g_gpu_init_w == 1280 && g_gpu_init_h == 720);
    hle_d3d9_set_gpu(g_gpu_prev);

    /* Device-loss / recovery contract (public D3D9): TestCooperativeLevel
     * reports the operational state, Present fails while the device is lost,
     * Reset takes new presentation parameters, fails with D3DERR_INVALIDCALL
     * while an application-held D3DPOOL_DEFAULT resource is alive and lets
     * D3DPOOL_MANAGED resources survive the reset. */
    ok("TestCooperativeLevel hr == D3D_OK on operational device",
       call("IDirect3DDevice9::TestCooperativeLevel", 1, (uint32_t[]){ dev }) == D3D_OK);
    {
        uint32_t a_reset_null[2] = { dev, 0 };
        ok("Reset with null presentation parameters == D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::Reset", 2, a_reset_null) == D3DERR_INVALIDCALL);
    }
    /* Live D3DPOOL_DEFAULT texture: Reset fails (device goes lost), Present
     * then fails with D3DERR_DEVICELOST until the reset recovers. */
    uint32_t *pDefTex = (uint32_t *)hle_alloc(4);
    uint32_t a_cdef[9] = { dev, 256, 256, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, A(pDefTex), 0 };
    ok("CreateTexture D3DPOOL_DEFAULT hr == 0", call("IDirect3DDevice9::CreateTexture", 9, a_cdef) == D3D_OK && *pDefTex != 0);
    uint32_t a_stdef[3] = { dev, 0, *pDefTex };
    call("IDirect3DDevice9::SetTexture", 3, a_stdef);
    ok("Reset with live D3DPOOL_DEFAULT texture == D3DERR_INVALIDCALL",
       call("IDirect3DDevice9::Reset", 2, a_reset) == D3DERR_INVALIDCALL);
    {
        uint32_t a_present_lost[5] = { dev, 0, 0, 0, 0 };
        ok("Present on lost device == D3DERR_DEVICELOST",
           call("IDirect3DDevice9::Present", 5, a_present_lost) == D3DERR_DEVICELOST);
    }
    call("IDirect3DTexture9::Release", 1, (uint32_t[]){ *pDefTex });
    /* The released default-pool binding stays stale in the device context, so
     * rebind a managed texture (which does not block a reset) and recover. */
    uint32_t *pManTex = (uint32_t *)hle_alloc(4);
    uint32_t a_cman[9] = { dev, 256, 256, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, A(pManTex), 0 };
    ok("CreateTexture D3DPOOL_MANAGED hr == 0", call("IDirect3DDevice9::CreateTexture", 9, a_cman) == D3D_OK && *pManTex != 0);
    uint32_t a_sman[3] = { dev, 0, *pManTex };
    call("IDirect3DDevice9::SetTexture", 3, a_sman);
    ok("Reset after releasing D3DPOOL_DEFAULT texture and rebinding managed == D3D_OK",
       call("IDirect3DDevice9::Reset", 2, a_reset) == D3D_OK);
    call("IDirect3DTexture9::Release", 1, (uint32_t[]){ *pManTex });

    /* Set & Get RenderState */
    uint32_t *rs_val = (uint32_t *)hle_alloc(4);
    uint32_t a_srs[3] = { dev, 7 /* D3DRS_ZENABLE */, 1 };
    call("IDirect3DDevice9::SetRenderState", 3, a_srs);
    uint32_t a_grs[3] = { dev, 7, A(rs_val) };
    call("IDirect3DDevice9::GetRenderState", 3, a_grs);
    ok("RenderState roundtrip", *rs_val == 1);

    /* Transforms */
    D3DMATRIX *mat = (D3DMATRIX *)hle_alloc(sizeof(D3DMATRIX));
    mat->m[0][0] = 2.0f; mat->m[1][1] = 3.0f; mat->m[2][2] = 4.0f; mat->m[3][3] = 1.0f;
    uint32_t a_st[3] = { dev, 2 /* D3DTS_PROJECTION */, A(mat) };
    call("IDirect3DDevice9::SetTransform", 3, a_st);
    D3DMATRIX *mat_out = (D3DMATRIX *)hle_alloc(sizeof(D3DMATRIX));
    uint32_t a_gt[3] = { dev, 2, A(mat_out) };
    call("IDirect3DDevice9::GetTransform", 3, a_gt);
    ok("Set/GetTransform matrix match", mat_out->m[0][0] == 2.0f && mat_out->m[1][1] == 3.0f);

    /* StateBlock capture & apply */
    uint32_t *pSB = (uint32_t *)hle_alloc(4);
    uint32_t a_csb[3] = { dev, 1 /* D3DSBT_ALL */, A(pSB) };
    hr = call("IDirect3DDevice9::CreateStateBlock", 3, a_csb);
    ok("CreateStateBlock hr == 0", hr == D3D_OK && *pSB != 0);

    /* Change renderstate, then apply captured state */
    uint32_t a_srs2[3] = { dev, 7, 0 };
    call("IDirect3DDevice9::SetRenderState", 3, a_srs2);
    call("IDirect3DDevice9::GetRenderState", 3, a_grs);
    ok("RenderState changed to 0", *rs_val == 0);

    uint32_t a_apply[1] = { *pSB };
    call("IDirect3DStateBlock9::Apply", 1, a_apply);
    call("IDirect3DDevice9::GetRenderState", 3, a_grs);
    ok("RenderState restored to 1 by StateBlock Apply", *rs_val == 1);

    uint32_t a_rel_sb[1] = { *pSB };
    call("IDirect3DStateBlock9::Release", 1, a_rel_sb);

    /* 9. Resources: VertexBuffer */
    uint32_t *pVB = (uint32_t *)hle_alloc(4);
    uint32_t a_cvb[7] = { dev, 1024, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, A(pVB), 0 };
    hr = call("IDirect3DDevice9::CreateVertexBuffer", 7, a_cvb);
    ok("CreateVertexBuffer hr == 0", hr == D3D_OK && *pVB != 0);

    uint32_t *pLockData = (uint32_t *)hle_alloc(4);
    uint32_t a_vblock[5] = { *pVB, 0, 1024, A(pLockData), 0 };
    hr = call("IDirect3DVertexBuffer9::Lock", 5, a_vblock);
    ok("VertexBuffer::Lock hr == 0", hr == D3D_OK && *pLockData != 0);
    memset(hle_ptr(*pLockData), 0xAA, 128);
    uint32_t a_vbunlock[1] = { *pVB };
    hr = call("IDirect3DVertexBuffer9::Unlock", 1, a_vbunlock);
    ok("VertexBuffer::Unlock hr == 0", hr == D3D_OK);

    uint32_t a_stream[5] = { dev, 0, *pVB, 0, 32 };
    call("IDirect3DDevice9::SetStreamSource", 5, a_stream);

    /* 10. Resources: IndexBuffer */
    uint32_t *pIB = (uint32_t *)hle_alloc(4);
    uint32_t a_cib[7] = { dev, 512, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, A(pIB), 0 };
    hr = call("IDirect3DDevice9::CreateIndexBuffer", 7, a_cib);
    ok("CreateIndexBuffer hr == 0", hr == D3D_OK && *pIB != 0);

    uint32_t a_indices[2] = { dev, *pIB };
    call("IDirect3DDevice9::SetIndices", 2, a_indices);

    /* 11. Resources: Texture 2D & Cube */
    uint32_t *pTex = (uint32_t *)hle_alloc(4);
    uint32_t a_ctex[9] = { dev, 256, 256, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, A(pTex), 0 };
    hr = call("IDirect3DDevice9::CreateTexture", 9, a_ctex);
    ok("CreateTexture hr == 0", hr == D3D_OK && *pTex != 0);

    D3DLOCKED_RECT *lr = (D3DLOCKED_RECT *)hle_alloc(sizeof(D3DLOCKED_RECT));
    uint32_t a_texlock[5] = { *pTex, 0, A(lr), 0, 0 };
    hr = call("IDirect3DTexture9::LockRect", 5, a_texlock);
    ok("Texture9::LockRect hr == 0", hr == D3D_OK && lr->pBits != 0);
    ok("Texture9 pitch == 256*4", lr->Pitch == 256 * 4);
    uint32_t a_texunlock[2] = { *pTex, 0 };
    call("IDirect3DTexture9::UnlockRect", 2, a_texunlock);

    uint32_t a_stex[3] = { dev, 0, *pTex };
    call("IDirect3DDevice9::SetTexture", 3, a_stex);

    /* Cube Texture */
    uint32_t *pCube = (uint32_t *)hle_alloc(4);
    uint32_t a_ccube[8] = { dev, 128, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, A(pCube), 0 };
    hr = call("IDirect3DDevice9::CreateCubeTexture", 8, a_ccube);
    ok("CreateCubeTexture hr == 0", hr == D3D_OK && *pCube != 0);
    uint32_t a_rel_cube[1] = { *pCube };
    call("IDirect3DCubeTexture9::Release", 1, a_rel_cube);

    /* Volume textures: boundary sizes, level clamping and block pitch.
     * Expected values are computed from the public D3D9 rules, not from the
     * implementation: Levels=0 means floor(log2(max(w,h,d)))+1 and a level
     * halves each dimension; DXT blocks are 4x4 pixels (8 or 16 bytes). */
    {
        uint32_t *pv = (uint32_t *)hle_alloc(4);
        D3DVOLUME_DESC *vd = (D3DVOLUME_DESC *)hle_alloc(sizeof(*vd));
        D3DLOCKED_BOX *lb = (D3DLOCKED_BOX *)hle_alloc(sizeof(*lb));
        uint32_t *pout = (uint32_t *)hle_alloc(4);

        hr = call("IDirect3DDevice9::CreateVolumeTexture", 10,
                  (uint32_t[]){ dev, 1, 1, 1, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, A(pv), 0 });
        ok("CreateVolumeTexture 1x1x1 levels=0", hr == D3D_OK && *pv != 0);
        ok("1x1x1 GetLevelCount == 1",
           call("IDirect3DVolumeTexture9::GetLevelCount", 1, (uint32_t[]){ *pv }) == 1);
        uint32_t rc0 = MEM32(*pv + 4);
        call("IDirect3DVolumeTexture9::GenerateMipSubLevels", 1, (uint32_t[]){ *pv });
        ok("GenerateMipSubLevels keeps refcount", MEM32(*pv + 4) == rc0);
        hr = call("IDirect3DVolumeTexture9::GetLevelDesc", 3, (uint32_t[]){ *pv, 0, A(vd) });
        ok("1x1x1 GetLevelDesc", hr == D3D_OK && vd->Width == 1 && vd->Height == 1 && vd->Depth == 1);
        *pout = 0xDEADBEEFu;
        ok("1x1x1 GetVolumeLevel(1) fails untouched",
           call("IDirect3DVolumeTexture9::GetVolumeLevel", 3,
                (uint32_t[]){ *pv, 1, A(pout) }) == D3DERR_INVALIDCALL && *pout == 0xDEADBEEFu);
        call("IDirect3DVolumeTexture9::Release", 1, (uint32_t[]){ *pv });

        /* Non-square 8x4x2: levels 8x4x2, 4x2x1, 2x1x1, 1x1x1. */
        hr = call("IDirect3DDevice9::CreateVolumeTexture", 10,
                  (uint32_t[]){ dev, 8, 4, 2, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, A(pv), 0 });
        ok("CreateVolumeTexture 8x4x2 levels=0", hr == D3D_OK && *pv != 0);
        ok("8x4x2 GetLevelCount == 4",
           call("IDirect3DVolumeTexture9::GetLevelCount", 1, (uint32_t[]){ *pv }) == 4);
        hr = call("IDirect3DVolumeTexture9::GetLevelDesc", 3, (uint32_t[]){ *pv, 2, A(vd) });
        ok("8x4x2 level 2 is 2x1x1", hr == D3D_OK && vd->Width == 2 && vd->Height == 1 && vd->Depth == 1);
        ok("8x4x2 GetLevelDesc(4) fails",
           call("IDirect3DVolumeTexture9::GetLevelDesc", 3,
                (uint32_t[]){ *pv, 4, A(vd) }) == D3DERR_INVALIDCALL);
        call("IDirect3DVolumeTexture9::Release", 1, (uint32_t[]){ *pv });

        /* More levels than the dimensions allow must be clamped, as in
         * CreateTexture/CreateCubeTexture and in the Metal descriptor. */
        hr = call("IDirect3DDevice9::CreateVolumeTexture", 10,
                  (uint32_t[]){ dev, 4, 4, 2, 100, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, A(pv), 0 });
        ok("CreateVolumeTexture levels=100 clamped", hr == D3D_OK && *pv != 0);
        ok("4x4x2 clamped GetLevelCount == 3",
           call("IDirect3DVolumeTexture9::GetLevelCount", 1, (uint32_t[]){ *pv }) == 3);
        hr = call("IDirect3DVolumeTexture9::GetLevelDesc", 3, (uint32_t[]){ *pv, 2, A(vd) });
        ok("4x4x2 clamped level 2 is 1x1x1", hr == D3D_OK && vd->Width == 1 && vd->Height == 1 && vd->Depth == 1);
        call("IDirect3DVolumeTexture9::Release", 1, (uint32_t[]){ *pv });

        /* DXT1: 8x8x2 -> row 2 blocks*8=16, slice 16*2 blocks=32.
         * DXT5: 8x8x2 -> row 2*16=32, slice 32*2=64. */
        hr = call("IDirect3DDevice9::CreateVolumeTexture", 10,
                  (uint32_t[]){ dev, 8, 8, 2, 1, 0, D3DFMT_DXT1, D3DPOOL_MANAGED, A(pv), 0 });
        ok("CreateVolumeTexture DXT1", hr == D3D_OK && *pv != 0);
        hr = call("IDirect3DVolumeTexture9::LockBox", 5, (uint32_t[]){ *pv, 0, A(lb), 0, 0 });
        ok("DXT1 LockBox", hr == D3D_OK && lb->pBits != 0);
        ok("DXT1 RowPitch == 16", lb->RowPitch == 16);
        ok("DXT1 SlicePitch == 32", lb->SlicePitch == 32);
        void *base = hle_ptr(lb->pBits);
        uint32_t a_ub[2] = { *pv, 0 };
        call("IDirect3DVolumeTexture9::UnlockBox", 2, a_ub);
        /* Left=4,Top=4 -> block (1,1); Front=1 -> second slice. */
        D3DBOX *box = (D3DBOX *)hle_alloc(sizeof(*box));
        box->Left = 4; box->Top = 4; box->Right = 8; box->Bottom = 8;
        box->Front = 1; box->Back = 2;
        hr = call("IDirect3DVolumeTexture9::LockBox", 5,
                  (uint32_t[]){ *pv, 0, A(lb), A(box), 0 });
        ok("DXT1 box LockBox", hr == D3D_OK);
        ok("DXT1 box offset == slice + row + block",
           hle_ptr(lb->pBits) == (uint8_t *)base + 32 + 16 + 8);
        call("IDirect3DVolumeTexture9::UnlockBox", 2, a_ub);
        D3DBOX *bad = (D3DBOX *)hle_alloc(sizeof(*bad));
        bad->Left = 0; bad->Top = 0; bad->Right = 0; bad->Bottom = 8;
        bad->Front = 0; bad->Back = 2;                   /* Right == Left: empty */
        ok("empty box rejected",
           call("IDirect3DVolumeTexture9::LockBox", 5,
                (uint32_t[]){ *pv, 0, A(lb), A(bad), 0 }) == D3DERR_INVALIDCALL);
        D3DBOX *oob = (D3DBOX *)hle_alloc(sizeof(*oob));
        oob->Left = 0; oob->Top = 0; oob->Right = 12; oob->Bottom = 8;
        oob->Front = 0; oob->Back = 2;                   /* Right > width */
        ok("out-of-range box rejected",
           call("IDirect3DVolumeTexture9::LockBox", 5,
                (uint32_t[]){ *pv, 0, A(lb), A(oob), 0 }) == D3DERR_INVALIDCALL);
        call("IDirect3DVolumeTexture9::Release", 1, (uint32_t[]){ *pv });

        hr = call("IDirect3DDevice9::CreateVolumeTexture", 10,
                  (uint32_t[]){ dev, 8, 8, 2, 1, 0, D3DFMT_DXT5, D3DPOOL_MANAGED, A(pv), 0 });
        ok("CreateVolumeTexture DXT5", hr == D3D_OK && *pv != 0);
        hr = call("IDirect3DVolumeTexture9::LockBox", 5, (uint32_t[]){ *pv, 0, A(lb), 0, 0 });
        ok("DXT5 LockBox", hr == D3D_OK);
        ok("DXT5 RowPitch == 32", lb->RowPitch == 32);
        ok("DXT5 SlicePitch == 64", lb->SlicePitch == 64);
        call("IDirect3DVolumeTexture9::UnlockBox", 2, (uint32_t[]){ *pv, 0 });
        call("IDirect3DVolumeTexture9::Release", 1, (uint32_t[]){ *pv });
    }

    /* UpdateTexture must copy volume and cube textures, not reinterpret them
     * as a 2D d3d9_texture_ctx_t. Both source and destination are filled and
     * read back with independent guest-side locks. */
    {
        uint32_t *pvs = (uint32_t *)hle_alloc(4), *pvd = (uint32_t *)hle_alloc(4);
        D3DLOCKED_BOX *lb = (D3DLOCKED_BOX *)hle_alloc(sizeof(*lb));
        uint32_t mkvol[10];
        mkvol[0] = dev; mkvol[1] = 4; mkvol[2] = 4; mkvol[3] = 2; mkvol[4] = 1;
        mkvol[5] = 0; mkvol[6] = D3DFMT_A8R8G8B8; mkvol[7] = D3DPOOL_MANAGED;
        mkvol[8] = A(pvs); mkvol[9] = 0;
        hr = call("IDirect3DDevice9::CreateVolumeTexture", 10, mkvol);
        ok("UpdateTexture source volume", hr == D3D_OK && *pvs);
        mkvol[8] = A(pvd);
        hr = call("IDirect3DDevice9::CreateVolumeTexture", 10, mkvol);
        ok("UpdateTexture dest volume", hr == D3D_OK && *pvd);
        hr = call("IDirect3DVolumeTexture9::LockBox", 5, (uint32_t[]){ *pvs, 0, A(lb), 0, 0 });
        ok("UpdateTexture lock source volume", hr == D3D_OK);
        for (uint32_t z = 0; z < 2; z++) {
            for (uint32_t y = 0; y < 4; y++) {
                uint32_t *row = (uint32_t *)((uint8_t *)hle_ptr(lb->pBits) +
                                             z * (uint32_t)lb->SlicePitch + y * (uint32_t)lb->RowPitch);
                for (uint32_t x = 0; x < 4; x++) row[x] = 0xFF000000u | (z << 16) | (y << 8) | x;
            }
        }
        call("IDirect3DVolumeTexture9::UnlockBox", 2, (uint32_t[]){ *pvs, 0 });
        hr = call("IDirect3DDevice9::UpdateTexture", 3, (uint32_t[]){ dev, *pvs, *pvd });
        ok("UpdateTexture volume->volume hr", hr == D3D_OK);
        hr = call("IDirect3DVolumeTexture9::LockBox", 5, (uint32_t[]){ *pvd, 0, A(lb), 0, 0 });
        ok("UpdateTexture lock dest volume", hr == D3D_OK);
        int vol_same = 1;
        for (uint32_t z = 0; z < 2 && vol_same; z++) {
            for (uint32_t y = 0; y < 4; y++) {
                uint32_t *row = (uint32_t *)((uint8_t *)hle_ptr(lb->pBits) +
                                             z * (uint32_t)lb->SlicePitch + y * (uint32_t)lb->RowPitch);
                for (uint32_t x = 0; x < 4; x++) {
                    if (row[x] != (0xFF000000u | (z << 16) | (y << 8) | x)) vol_same = 0;
                }
            }
        }
        ok("UpdateTexture copied every volume slice", vol_same);
        call("IDirect3DVolumeTexture9::UnlockBox", 2, (uint32_t[]){ *pvd, 0 });
        /* A 2D texture is not a volume: the shim must fail, not silently copy. */
        ok("UpdateTexture 2D->volume rejected",
           call("IDirect3DDevice9::UpdateTexture", 3, (uint32_t[]){ dev, *pTex, *pvd }) == D3DERR_INVALIDCALL);
        call("IDirect3DVolumeTexture9::Release", 1, (uint32_t[]){ *pvs });
        call("IDirect3DVolumeTexture9::Release", 1, (uint32_t[]){ *pvd });

        uint32_t *pcs = (uint32_t *)hle_alloc(4), *pcd = (uint32_t *)hle_alloc(4);
        D3DLOCKED_RECT *lr2 = (D3DLOCKED_RECT *)hle_alloc(sizeof(*lr2));
        uint32_t mkcube[8];
        mkcube[0] = dev; mkcube[1] = 4; mkcube[2] = 1; mkcube[3] = 0;
        mkcube[4] = D3DFMT_A8R8G8B8; mkcube[5] = D3DPOOL_MANAGED;
        mkcube[6] = A(pcs); mkcube[7] = 0;
        hr = call("IDirect3DDevice9::CreateCubeTexture", 8, mkcube);
        ok("UpdateTexture source cube", hr == D3D_OK && *pcs);
        mkcube[6] = A(pcd);
        hr = call("IDirect3DDevice9::CreateCubeTexture", 8, mkcube);
        ok("UpdateTexture dest cube", hr == D3D_OK && *pcd);
        hr = call("IDirect3DCubeTexture9::LockRect", 6,
                  (uint32_t[]){ *pcs, 2, 0, A(lr2), 0, 0 });
        ok("UpdateTexture lock cube face 2", hr == D3D_OK);
        for (uint32_t y = 0; y < 4; y++) {
            uint32_t *row = (uint32_t *)((uint8_t *)hle_ptr(lr2->pBits) + y * (uint32_t)lr2->Pitch);
            for (uint32_t x = 0; x < 4; x++) row[x] = 0xAABB0000u | (y << 8) | x;
        }
        call("IDirect3DCubeTexture9::UnlockRect", 3, (uint32_t[]){ *pcs, 2, 0 });
        hr = call("IDirect3DDevice9::UpdateTexture", 3, (uint32_t[]){ dev, *pcs, *pcd });
        ok("UpdateTexture cube->cube hr", hr == D3D_OK);
        hr = call("IDirect3DCubeTexture9::LockRect", 6,
                  (uint32_t[]){ *pcd, 2, 0, A(lr2), 0, 0 });
        ok("UpdateTexture lock dest cube face 2", hr == D3D_OK);
        int cube_same = 1;
        for (uint32_t y = 0; y < 4; y++) {
            uint32_t *row = (uint32_t *)((uint8_t *)hle_ptr(lr2->pBits) + y * (uint32_t)lr2->Pitch);
            for (uint32_t x = 0; x < 4; x++) {
                if (row[x] != (0xAABB0000u | (y << 8) | x)) cube_same = 0;
            }
        }
        ok("UpdateTexture copied the cube face", cube_same);
        call("IDirect3DCubeTexture9::UnlockRect", 3, (uint32_t[]){ *pcd, 2, 0 });
        /* Other faces must not be overwritten by a face-2 copy. */
        hr = call("IDirect3DCubeTexture9::LockRect", 6,
                  (uint32_t[]){ *pcd, 0, 0, A(lr2), 0, 0 });
        ok("UpdateTexture face 0 untouched", hr == D3D_OK &&
           *(uint32_t *)hle_ptr(lr2->pBits) != 0xAABB0000u);
        call("IDirect3DCubeTexture9::UnlockRect", 3, (uint32_t[]){ *pcd, 0, 0 });
        /* The cube lock relays the inner surface HRESULT: a NULL output
         * pointer must be reported, not silently accepted as D3D_OK. */
        ok("cube LockRect pLocked=0 fails",
           call("IDirect3DCubeTexture9::LockRect", 6,
                (uint32_t[]){ *pcd, 0, 0, 0, 0, 0 }) == D3DERR_INVALIDCALL);
        call("IDirect3DCubeTexture9::Release", 1, (uint32_t[]){ *pcs });
        call("IDirect3DCubeTexture9::Release", 1, (uint32_t[]){ *pcd });
    }

    /* 12. Vertex Declaration */
    D3DVERTEXELEMENT9 *elems = (D3DVERTEXELEMENT9 *)hle_alloc(sizeof(D3DVERTEXELEMENT9) * 3);
    elems[0].Stream = 0; elems[0].Offset = 0; elems[0].Type = D3DDECLTYPE_FLOAT3;
    elems[0].Method = 0; elems[0].Usage = D3DDECLUSAGE_POSITION; elems[0].UsageIndex = 0;
    elems[1].Stream = 0; elems[1].Offset = 12; elems[1].Type = D3DDECLTYPE_D3DCOLOR;
    elems[1].Method = 0; elems[1].Usage = D3DDECLUSAGE_COLOR; elems[1].UsageIndex = 0;
    /* D3DDECL_END() */
    elems[2].Stream = 0xFF; elems[2].Offset = 0; elems[2].Type = D3DDECLTYPE_UNUSED;
    elems[2].Method = 0; elems[2].Usage = 0; elems[2].UsageIndex = 0;

    uint32_t *pDecl = (uint32_t *)hle_alloc(4);
    uint32_t a_cdecl[3] = { dev, A(elems), A(pDecl) };
    hr = call("IDirect3DDevice9::CreateVertexDeclaration", 3, a_cdecl);
    ok("CreateVertexDeclaration hr == 0", hr == D3D_OK && *pDecl != 0);

    uint32_t a_sdecl[2] = { dev, *pDecl };
    call("IDirect3DDevice9::SetVertexDeclaration", 2, a_sdecl);

    /* 13. Vertex & Pixel Shaders. Operand tokens use the Microsoft D3D9
     * layout: type in bits 28..30 + 11..12, number in bits 0..10. */
#define D9REG(type, num) \
    ((((uint32_t)(type) & 7u) << 28u) | ((((uint32_t)(type) >> 3u) & 3u) << 11u) | \
     ((uint32_t)(num) & 0x7FFu))
#define D9DST(type, num, mask) (0x80000000u | D9REG(type, num) | ((uint32_t)(mask) << 16u))
#define D9SRC(type, num) (D9REG(type, num) | (0xE4u << 16u))  /* .xyzw */
#define D9_TEMP 0u
#define D9_INPUT 1u
#define D9_CONST 2u
#define D9_ADDR 3u
#define D9_TEXTURE 3u
#define D9_RASTOUT 4u
#define D9_COLOROUT 8u
#define D9_SAMPLER 10u
    uint32_t vs_bytecode[7] = {
        D3DVS_VERSION(3, 0),    /* vs_3_0 */
        0x0200001F,             /* dcl v0 */
        0u,                     /* usage token */
        D9REG(D9_INPUT, 0),
        0x02000001,             /* mov oPos, v0 */
        D9DST(D9_RASTOUT, 0, 0xFu),
        D9SRC(D9_INPUT, 0)
    };
    uint32_t *pVS = (uint32_t *)hle_alloc(4);
    uint32_t *vs_guest = (uint32_t *)hle_alloc(sizeof(vs_bytecode) + 4);
    memcpy(vs_guest, vs_bytecode, sizeof(vs_bytecode));
    vs_guest[7] = 0x0000FFFFu;  /* D3DSIO_END */

    uint32_t a_cvs[3] = { dev, A(vs_guest), A(pVS) };
    hr = call("IDirect3DDevice9::CreateVertexShader", 3, a_cvs);
    ok("CreateVertexShader hr == 0", hr == D3D_OK && *pVS != 0);
    uint32_t a_svs[2] = { dev, *pVS };
    call("IDirect3DDevice9::SetVertexShader", 2, a_svs);

    /* Test MSL Translation of VS directly */
    char msl_buf[16384];
    char err_buf[256];
    int trans_rc = d3d9_translate_shader_to_msl(vs_guest, 8, 0, msl_buf, sizeof(msl_buf), err_buf, sizeof(err_buf));
    ok("VS translate_to_msl rc == 0", trans_rc == 0);
    ok("MSL contains vs_main", strstr(msl_buf, "vs_main") != NULL);
    ok("MSL contains oPos", strstr(msl_buf, "oPos") != NULL);

    /* Pixel Shader */
    uint32_t ps_bytecode[8] = {
        D3DPS_VERSION(3, 0),    /* ps_3_0 */
        0x0200001F,             /* dcl_2d s0 */
        (2u << 27u),            /* D3DSTT_2D usage token */
        D9REG(D9_SAMPLER, 0),
        0x02000001,             /* mov oC0, c0 */
        D9DST(D9_COLOROUT, 0, 0xFu),
        D9SRC(D9_CONST, 0),
        0x0000FFFFu             /* D3DSIO_END */
    };
    uint32_t *pPS = (uint32_t *)hle_alloc(4);
    uint32_t *ps_guest = (uint32_t *)hle_alloc(sizeof(ps_bytecode));
    memcpy(ps_guest, ps_bytecode, sizeof(ps_bytecode));

    uint32_t a_cps[3] = { dev, A(ps_guest), A(pPS) };
    hr = call("IDirect3DDevice9::CreatePixelShader", 3, a_cps);
    ok("CreatePixelShader hr == 0", hr == D3D_OK && *pPS != 0);

    trans_rc = d3d9_translate_shader_to_msl(ps_guest, 8, 1, msl_buf, sizeof(msl_buf), err_buf, sizeof(err_buf));
    ok("PS translate_to_msl rc == 0", trans_rc == 0);
    ok("MSL contains ps_main", strstr(msl_buf, "ps_main") != NULL);
    ok("MSL contains oC[0]", strstr(msl_buf, "oC[0]") != NULL);

    /* Register type 3 is stage-dependent. In a pixel shader it is the float4
     * texture coordinate t#, whose t0 is the TEXCOORD0 input; in a vertex
     * shader it is the integer address register a0. */
    {
        uint32_t ps_t0[] = {
            D3DPS_VERSION(2, 0),
            0x0200001F, 0, D9REG(D9_TEXTURE, 0),        /* dcl t0 */
            0x02000001, D9DST(D9_TEMP, 0, 0xFu),
            D9SRC(D9_TEXTURE, 0),                       /* mov r0, t0 */
            0x0000FFFF
        };
        trans_rc = d3d9_translate_shader_to_msl(ps_t0, sizeof(ps_t0) / sizeof(ps_t0[0]), 1, msl_buf,
                                                sizeof(msl_buf), err_buf, sizeof(err_buf));
        ok("PS type 3 is t0 (rc)", trans_rc == 0);
        ok("PS t0 comes from TEXCOORD0",
           strstr(msl_buf, "s.t[0] = in.uv0;") != NULL &&
           strstr(msl_buf, "s.t[0]") != NULL);
    }
    {
        uint32_t vs_a0[] = {
            D3DVS_VERSION(3, 0),
            0x0200001F, 0, D9REG(D9_INPUT, 0),          /* dcl v0 */
            0x02000001, D9DST(D9_ADDR, 0, 0xFu),
            D9SRC(D9_INPUT, 0),                         /* mov a0, v0 */
            0x02000001, D9DST(D9_RASTOUT, 0, 0xFu),
            D9SRC(D9_CONST, 0),                         /* mov oPos, c0 */
            0x0000FFFF
        };
        trans_rc = d3d9_translate_shader_to_msl(vs_a0, sizeof(vs_a0) / sizeof(vs_a0[0]), 0, msl_buf,
                                                sizeof(msl_buf), err_buf, sizeof(err_buf));
        ok("VS type 3 is a0 (rc)", trans_rc == 0);
        ok("VS type 3 stays integer a0",
           strstr(msl_buf, "s.a0.x = int(") != NULL);
    }

    /* Shader constants */
    float consts[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
    float *consts_guest = (float *)hle_alloc(sizeof(consts));
    memcpy(consts_guest, consts, sizeof(consts));
    uint32_t a_scf[4] = { dev, 0, A(consts_guest), 1 };
    call("IDirect3DDevice9::SetVertexShaderConstantF", 4, a_scf);
    float *consts_out = (float *)hle_alloc(sizeof(consts));
    uint32_t a_gcf[4] = { dev, 0, A(consts_out), 1 };
    call("IDirect3DDevice9::GetVertexShaderConstantF", 4, a_gcf);
    ok("VertexShaderConstantF match", consts_out[0] == 1.0f && consts_out[3] == 4.0f);

    /* 14. Scene lifecycle & Clear */
    uint32_t a_clear[7] = { dev, 0, 0, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF003366, 0x3F800000 /* 1.0f */, 0 };
    ok("Clear hr == 0", call("IDirect3DDevice9::Clear", 7, a_clear) == D3D_OK);

    uint32_t a_bscene[1] = { dev };
    ok("BeginScene hr == 0", call("IDirect3DDevice9::BeginScene", 1, a_bscene) == D3D_OK);

    /* DrawPrimitive */
    uint32_t a_draw[4] = { dev, D3DPT_TRIANGLELIST, 0, 1 };
    {
        uint32_t hr14 = call("IDirect3DDevice9::DrawPrimitive", 4, a_draw);
        ok("DrawPrimitive hr == 0", hr14 == D3D_OK);
    }

    uint32_t a_escene[1] = { dev };
    ok("EndScene hr == 0", call("IDirect3DDevice9::EndScene", 1, a_escene) == D3D_OK);

    uint32_t a_present[5] = { dev, 0, 0, 0, 0 };
    ok("Present hr == 0", call("IDirect3DDevice9::Present", 5, a_present) == D3D_OK);

    /* 15. Query: public query types from Wine include/d3d9types.h.
     * Type 1 is not a public D3DQUERYTYPE: it must be rejected, not silently
     * accepted as the old fake implementation did. */
    hle_d3d9_set_gpu(&g_mock_gpu);   /* restored to the previous backend above */
    uint32_t *pQuery = (uint32_t *)hle_alloc(4);
    *pQuery = 0;
    uint32_t a_bad_query[3] = { dev, 1, A(pQuery) };
    ok("CreateQuery type 1 is D3DERR_NOTAVAILABLE",
       call("IDirect3DDevice9::CreateQuery", 3, a_bad_query) == D3DERR_NOTAVAILABLE);
    ok("rejected CreateQuery leaves output untouched", *pQuery == 0);

    uint32_t a_query[3] = { dev, D3DQUERYTYPE_EVENT, A(pQuery) };
    hr = call("IDirect3DDevice9::CreateQuery", 3, a_query);
    ok("CreateQuery(EVENT) hr == D3D_OK", hr == D3D_OK && *pQuery != 0);
    ok("query GetType == D3DQUERYTYPE_EVENT",
       call("IDirect3DQuery9::GetType", 1, (uint32_t[]){ *pQuery }) == D3DQUERYTYPE_EVENT);
    ok("query GetDataSize == 4",
       call("IDirect3DQuery9::GetDataSize", 1, (uint32_t[]){ *pQuery }) == 4);

    int issues0 = g_q_issues;
    ok("Issue invalid flags is D3DERR_INVALIDCALL",
       call("IDirect3DQuery9::Issue", 2, (uint32_t[]){ *pQuery, 0 }) == D3DERR_INVALIDCALL);
    ok("Issue invalid flags never reaches the backend", g_q_issues == issues0);
    uint32_t a_issue[2] = { *pQuery, D3DISSUE_END };
    ok("Issue(END) hr == D3D_OK", call("IDirect3DQuery9::Issue", 2, a_issue) == D3D_OK &&
                                   g_q_issues == issues0 + 1);

    uint32_t *qdata = (uint32_t *)hle_alloc(4);
    *qdata = 0xDEADBEEFu;
    uint32_t a_qdata[4] = { *pQuery, A(qdata), 4, 0 };
    ok("GetData without FLUSH is S_FALSE", call("IDirect3DQuery9::GetData", 4, a_qdata) == 1);
    ok("GetData without FLUSH writes 0 (backend value)", *qdata == 0u);
    a_qdata[3] = D3DGETDATA_FLUSH;
    ok("GetData with FLUSH is S_OK", call("IDirect3DQuery9::GetData", 4, a_qdata) == D3D_OK);
    ok("GetData with FLUSH relays the backend value, not a fake 1", *qdata == 0x5A5A1234u);
    uint32_t a_qsmall[4] = { *pQuery, A(qdata), 2, D3DGETDATA_FLUSH };
    ok("GetData size < 4 is D3DERR_INVALIDCALL",
       call("IDirect3DQuery9::GetData", 4, a_qsmall) == D3DERR_INVALIDCALL);

    int destroyed0 = g_q_destroyed;
    ok("query Release drops the last reference",
       call("IDirect3DQuery9::Release", 1, (uint32_t[]){ *pQuery }) == 0);
    ok("query Release destroyed the backend handle once", g_q_destroyed == destroyed0 + 1);

    /* 16. Parameter validation on copy, fill, frontbuffer, volume texture and rendertarget methods. */
    {
        uint32_t a_updsurf[5] = { dev, 0, 0, 0, 0 };
        ok("UpdateSurface invalid args returns D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::UpdateSurface", 5, a_updsurf) == D3DERR_INVALIDCALL);
        uint32_t a_cfill[4] = { dev, 0, 0, 0 };
        ok("ColorFill invalid args returns D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::ColorFill", 4, a_cfill) == D3DERR_INVALIDCALL);
        uint32_t a_updtex[3] = { dev, 0, 0 };
        ok("UpdateTexture invalid args returns D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::UpdateTexture", 3, a_updtex) == D3DERR_INVALIDCALL);
        uint32_t a_stretch[6] = { dev, 0, 0, 0, 0, 0 };
        ok("StretchRect invalid args returns D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::StretchRect", 6, a_stretch) == D3DERR_INVALIDCALL);
        uint32_t a_rtdata[3] = { dev, 0, 0 };
        ok("GetRenderTargetData invalid args returns D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::GetRenderTargetData", 3, a_rtdata) == D3DERR_INVALIDCALL);
        uint32_t a_frontbuf[3] = { dev, 0, 0 };
        ok("GetFrontBufferData invalid args returns D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::GetFrontBufferData", 3, a_frontbuf) == D3DERR_INVALIDCALL);
        uint32_t a_cvol[10] = { dev, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        ok("CreateVolumeTexture invalid args returns D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::CreateVolumeTexture", 10, a_cvol) == D3DERR_INVALIDCALL);
        uint32_t a_scfront[2] = { swap, 0 };
        ok("SwapChain::GetFrontBufferData invalid args returns D3DERR_INVALIDCALL",
           call("IDirect3DSwapChain9::GetFrontBufferData", 2, a_scfront) == D3DERR_INVALIDCALL);
        /* SetRenderTarget / GetRenderTarget bounds checking */
        uint32_t a_srt_null[3] = { dev, 0, 0 };
        ok("SetRenderTarget index 0 NULL returns D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::SetRenderTarget", 3, a_srt_null) == D3DERR_INVALIDCALL);
        uint32_t a_srt_oob[3] = { dev, 1, *pTex };
        ok("SetRenderTarget index 1 returns D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::SetRenderTarget", 3, a_srt_oob) == D3DERR_INVALIDCALL);
        uint32_t a_grt_null[3] = { dev, 0, 0 };
        ok("GetRenderTarget ppSurface NULL returns D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::GetRenderTarget", 3, a_grt_null) == D3DERR_INVALIDCALL);
        uint32_t *pRTOut = (uint32_t *)hle_alloc(4);
        uint32_t a_grt_oob[3] = { dev, 1, A(pRTOut) };
        ok("GetRenderTarget index 1 returns D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::GetRenderTarget", 3, a_grt_oob) == D3DERR_INVALIDCALL);
        /* Documented no-ops: AddDirtyRect is a hint for UpdateTexture;
         * DeletePatch has no cached patch because DrawRectPatch/DrawTriPatch are unimplemented. */
        uint32_t a_dirty[2] = { *pTex, 0 };
        ok("Texture::AddDirtyRect returns D3D_OK (hint only)",
           call("IDirect3DTexture9::AddDirtyRect", 2, a_dirty) == D3D_OK);
        uint32_t a_delpatch[2] = { dev, 1 };
        ok("DeletePatch returns D3D_OK (no cached patch)",
           call("IDirect3DDevice9::DeletePatch", 2, a_delpatch) == D3D_OK);
    }

    /* Honest answers for methods the HLE cannot really perform. Expected
     * values come from the public D3D9 contracts, not from the shims: a NULL
     * out pointer is INVALIDCALL, an unserviceable query is NOTAVAILABLE. */
    {
        uint32_t *pp = (uint32_t *)hle_alloc(4);
        *pp = 0xDEADBEEFu;
        ok("ValidateDevice NULL out -> INVALIDCALL",
           call("IDirect3DDevice9::ValidateDevice", 2,
                (uint32_t[]){ dev, 0 }) == D3DERR_INVALIDCALL);
        ok("ValidateDevice not available, out untouched",
           call("IDirect3DDevice9::ValidateDevice", 2,
                (uint32_t[]){ dev, A(pp) }) == D3DERR_NOTAVAILABLE && *pp == 0xDEADBEEFu);

        ok("SetCursorProperties NULL bitmap -> INVALIDCALL",
           call("IDirect3DDevice9::SetCursorProperties", 4,
                (uint32_t[]){ dev, 0, 0, 0 }) == D3DERR_INVALIDCALL);
        ok("SetCursorProperties no hardware cursor -> NOTAVAILABLE",
           call("IDirect3DDevice9::SetCursorProperties", 4,
                (uint32_t[]){ dev, 0, 0, *pTex }) == D3DERR_NOTAVAILABLE);

        uint32_t mem0 = call("IDirect3DDevice9::GetAvailableTextureMem", 1, (uint32_t[]){ dev });
        uint32_t mem1 = call("IDirect3DDevice9::GetAvailableTextureMem", 1, (uint32_t[]){ dev });
        /* With the mock GPU no Metal device exists, so 0 is the honest answer;
         * with the real backend this is the MTLDevice working-set size. */
        ok("GetAvailableTextureMem is stable", mem0 == mem1);
        ok("GetAvailableTextureMem is not the old constant 1 GiB", mem0 != 1024u * 1024u * 1024u);

        /* ShowCursor returns the previous state (public D3D9 contract). */
        ok("ShowCursor(FALSE) returns initial 1",
           call("IDirect3DDevice9::ShowCursor", 2, (uint32_t[]){ dev, 0 }) == 1u);
        ok("ShowCursor(FALSE) again returns 0",
           call("IDirect3DDevice9::ShowCursor", 2, (uint32_t[]){ dev, 0 }) == 0u);
        ok("ShowCursor(TRUE) returns previous 0",
           call("IDirect3DDevice9::ShowCursor", 2, (uint32_t[]){ dev, 1 }) == 0u);
        ok("ShowCursor(TRUE) again returns 1",
           call("IDirect3DDevice9::ShowCursor", 2, (uint32_t[]){ dev, 1 }) == 1u);

        /* D3DPERF_* are plain stdcall exports, no `this`. The public Wine
         * 11.0 d3d9.h signatures fix the argument counts and return types. */
        uint32_t *wname = (uint32_t *)hle_alloc(32);
        memset(wname, 0, 32);
        const char *ascii = "selftest";
        for (int i = 0; ascii[i]; i++) ((uint16_t *)wname)[i] = (uint16_t)ascii[i];
        /* Public docs (learn.microsoft.com D3DPERF_BeginEvent/EndEvent) and
         * Wine 11.0 d3d9_main.c: BeginEvent returns the zero-based level it
         * starts in (post-increment), EndEvent the level it ends in
         * (pre-decrement) -- negative on an unmatched end. */
        uint32_t tok1 = call("D3DPERF_BeginEvent", 2, (uint32_t[]){ 0x11223344u, A(wname) });
        uint32_t tok2 = call("D3DPERF_BeginEvent", 2, (uint32_t[]){ 0, A(wname) });
        ok("BeginEvent first level is 0", tok1 == 0u);
        ok("BeginEvent second level is 1", tok2 == 1u);
        ok("QueryRepeatFrame is FALSE",
           call("D3DPERF_QueryRepeatFrame", 0, (uint32_t[]){ 0 }) == 0u);
        ok("GetStatus is 0 (no profiler)",
           call("D3DPERF_GetStatus", 0, (uint32_t[]){ 0 }) == 0u);
        ok("EndEvent returns level 1",
           call("D3DPERF_EndEvent", 0, (uint32_t[]){ 0 }) == 1u);
        ok("EndEvent returns level 0",
           call("D3DPERF_EndEvent", 0, (uint32_t[]){ 0 }) == 0u);
        ok("EndEvent unmatched returns -1",
           call("D3DPERF_EndEvent", 0, (uint32_t[]){ 0 }) == 0xFFFFFFFFu);
        /* Restore the level for any later tests that use the counter. */
        call("D3DPERF_BeginEvent", 2, (uint32_t[]){ 0, A(wname) });
        /* void-returning markers: only the stdcall pop is observable here. */
        call("D3DPERF_SetMarker", 2, (uint32_t[]){ 0, A(wname) });
        call("D3DPERF_SetRegion", 2, (uint32_t[]){ 0, A(wname) });
        call("D3DPERF_SetOptions", 1, (uint32_t[]){ 1 });
        ok("void D3DPERF markers kept the machine alive", 1);
    }

    /* 17. Pure state: Set stores, Get returns the same bytes/value. */
    {
        uint32_t a_dev1[1] = { dev };
        void *ramp = hle_alloc(1536), *ramp_out = hle_alloc(1536);
        memset(ramp, 0x5a, 1536); memset(ramp_out, 0, 1536);
        uint32_t a_sramp[4] = { dev, 0, 0, A(ramp) };
        ok("SetGammaRamp == D3D_OK", call("IDirect3DDevice9::SetGammaRamp", 4, a_sramp) == D3D_OK);
        uint32_t a_gramp[3] = { dev, 0, A(ramp_out) };
        ok("GetGammaRamp == D3D_OK", call("IDirect3DDevice9::GetGammaRamp", 3, a_gramp) == D3D_OK);
        ok("gamma ramp round-trip", !memcmp(ramp, ramp_out, 1536));

        void *mat = hle_alloc(68), *mat_out = hle_alloc(68);
        memset(mat, 0x3c, 68); memset(mat_out, 0, 68);
        uint32_t a_smat[2] = { dev, A(mat) };
        ok("SetMaterial == D3D_OK", call("IDirect3DDevice9::SetMaterial", 2, a_smat) == D3D_OK);
        uint32_t a_gmat[2] = { dev, A(mat_out) };
        ok("GetMaterial == D3D_OK", call("IDirect3DDevice9::GetMaterial", 2, a_gmat) == D3D_OK);
        ok("material round-trip", !memcmp(mat, mat_out, 68));

        void *light = hle_alloc(104), *light_out = hle_alloc(104);
        memset(light, 0x11, 104); memset(light_out, 0, 104);
        /* GetLight on unassigned light index -> D3DERR_INVALIDCALL */
        ok("GetLight on unassigned light -> D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::GetLight", 3, (uint32_t[]){ dev, 5, A(light_out) }) == D3DERR_INVALIDCALL);
        /* Light index >= 8 (D3D9_MAX_LIGHTS) -> D3DERR_INVALIDCALL */
        ok("SetLight index 8 -> D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::SetLight", 3, (uint32_t[]){ dev, 8, A(light) }) == D3DERR_INVALIDCALL);
        ok("GetLight index 8 -> D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::GetLight", 3, (uint32_t[]){ dev, 8, A(light_out) }) == D3DERR_INVALIDCALL);
        ok("LightEnable index 8 -> D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::LightEnable", 3, (uint32_t[]){ dev, 8, 1 }) == D3DERR_INVALIDCALL);
        ok("GetLightEnable index 8 -> D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::GetLightEnable", 3, (uint32_t[]){ dev, 8, A(light_out) }) == D3DERR_INVALIDCALL);
        /* Valid SetLight/GetLight */
        uint32_t a_slight[3] = { dev, 2, A(light) };
        ok("SetLight == D3D_OK", call("IDirect3DDevice9::SetLight", 3, a_slight) == D3D_OK);
        uint32_t a_glight[3] = { dev, 2, A(light_out) };
        ok("GetLight == D3D_OK", call("IDirect3DDevice9::GetLight", 3, a_glight) == D3D_OK);
        ok("light round-trip", !memcmp(light, light_out, 104));

        uint32_t a_slen[3] = { dev, 3, 1 };
        ok("LightEnable == D3D_OK", call("IDirect3DDevice9::LightEnable", 3, a_slen) == D3D_OK);
        uint32_t *enabled = (uint32_t *)hle_alloc(4); *enabled = 0;
        uint32_t a_glen[3] = { dev, 3, A(enabled) };
        ok("GetLightEnable == D3D_OK", call("IDirect3DDevice9::GetLightEnable", 3, a_glen) == D3D_OK);
        ok("light enable round-trip", *enabled == 1);

        float *plane = (float *)hle_alloc(16), *plane_out = (float *)hle_alloc(16);
        plane[0] = 1.5f; plane[1] = -2.25f; plane[2] = 3.f; plane[3] = 4.5f;
        memset(plane_out, 0, 16);
        uint32_t a_sclip[3] = { dev, 1, A(plane) };
        ok("SetClipPlane == D3D_OK", call("IDirect3DDevice9::SetClipPlane", 3, a_sclip) == D3D_OK);
        uint32_t a_gclip[3] = { dev, 1, A(plane_out) };
        ok("GetClipPlane == D3D_OK", call("IDirect3DDevice9::GetClipPlane", 3, a_gclip) == D3D_OK);
        ok("clip plane round-trip", !memcmp(plane, plane_out, 16));

        uint8_t *entries = (uint8_t *)hle_alloc(1024), *entries_out = (uint8_t *)hle_alloc(1024);
        for (int i = 0; i < 1024; i++) entries[i] = (uint8_t)(i * 7 + 3);
        memset(entries_out, 0, 1024);
        uint32_t a_spal[3] = { dev, 2, A(entries) };
        ok("SetPaletteEntries == D3D_OK", call("IDirect3DDevice9::SetPaletteEntries", 3, a_spal) == D3D_OK);
        uint32_t a_gpal[3] = { dev, 2, A(entries_out) };
        ok("GetPaletteEntries == D3D_OK", call("IDirect3DDevice9::GetPaletteEntries", 3, a_gpal) == D3D_OK);
        ok("all 256 palette entries round-trip", !memcmp(entries, entries_out, 1024));
        uint32_t a_badpal[3] = { dev, 9, A(entries) };
        ok("SetPaletteEntries(palette >= 4) fails",
           call("IDirect3DDevice9::SetPaletteEntries", 3, a_badpal) == D3DERR_INVALIDCALL);

        uint32_t a_scp[2] = { dev, 42 };
        ok("SetCurrentTexturePalette == D3D_OK",
           call("IDirect3DDevice9::SetCurrentTexturePalette", 2, a_scp) == D3D_OK);
        uint32_t *curpal = (uint32_t *)hle_alloc(4); *curpal = 0;
        uint32_t a_gcp[2] = { dev, A(curpal) };
        ok("GetCurrentTexturePalette == D3D_OK",
           call("IDirect3DDevice9::GetCurrentTexturePalette", 2, a_gcp) == D3D_OK);
        ok("current texture palette round-trip", *curpal == 42);

        uint32_t a_ssvp[2] = { dev, 1 };
        ok("SetSoftwareVertexProcessing == D3D_OK",
           call("IDirect3DDevice9::SetSoftwareVertexProcessing", 2, a_ssvp) == D3D_OK);
        ok("GetSoftwareVertexProcessing == 1",
           call("IDirect3DDevice9::GetSoftwareVertexProcessing", 1, a_dev1) == 1);

        float np = 2.5f; uint32_t np_bits;
        memcpy(&np_bits, &np, 4);
        uint32_t a_snp[2] = { dev, np_bits };  /* FLOAT by value, not a pointer */
        ok("SetNPatchMode == D3D_OK", call("IDirect3DDevice9::SetNPatchMode", 2, a_snp) == D3D_OK);
        ok("NPatchMode round-trip (by-value FLOAT)",
           call("IDirect3DDevice9::GetNPatchMode", 1, a_dev1) == np_bits);

        int32_t *ci = (int32_t *)hle_alloc(16), *cio = (int32_t *)hle_alloc(16);
        for (int i = 0; i < 4; i++) ci[i] = 10 + i;
        memset(cio, 0, 16);
        uint32_t a_sci[4] = { dev, 0, A(ci), 1 };
        ok("SetVertexShaderConstantI == D3D_OK",
           call("IDirect3DDevice9::SetVertexShaderConstantI", 4, a_sci) == D3D_OK);
        uint32_t a_gci[4] = { dev, 0, A(cio), 1 };
        ok("GetVertexShaderConstantI == D3D_OK",
           call("IDirect3DDevice9::GetVertexShaderConstantI", 4, a_gci) == D3D_OK);
        ok("VS integer constant round-trip", !memcmp(ci, cio, 16));

        uint32_t *cb = (uint32_t *)hle_alloc(4), *cbo = (uint32_t *)hle_alloc(4);
        *cb = 1; *cbo = 0;
        uint32_t a_scb[4] = { dev, 0, A(cb), 1 };
        ok("SetVertexShaderConstantB == D3D_OK",
           call("IDirect3DDevice9::SetVertexShaderConstantB", 4, a_scb) == D3D_OK);
        uint32_t a_gcb[4] = { dev, 0, A(cbo), 1 };
        ok("GetVertexShaderConstantB == D3D_OK",
           call("IDirect3DDevice9::GetVertexShaderConstantB", 4, a_gcb) == D3D_OK);
        ok("VS bool constant round-trip", *cbo == 1);
        *cbo = 0;
        uint32_t a_scb15[4] = { dev, 15, A(cb), 1 }, a_gcb15[4] = { dev, 15, A(cbo), 1 };
        ok("SetVertexShaderConstantB(15) == D3D_OK",
           call("IDirect3DDevice9::SetVertexShaderConstantB", 4, a_scb15) == D3D_OK);
        ok("GetVertexShaderConstantB(15) == D3D_OK",
           call("IDirect3DDevice9::GetVertexShaderConstantB", 4, a_gcb15) == D3D_OK);
        ok("VS bool constant 15 round-trip", *cbo == 1);
        uint32_t a_scb16[4] = { dev, 16, A(cb), 1 };
        ok("VS bool constant past 16 registers fails",
           call("IDirect3DDevice9::SetVertexShaderConstantB", 4, a_scb16) == D3DERR_INVALIDCALL);
        uint32_t a_sci16[4] = { dev, 16, A(ci), 1 };
        ok("VS int constant past capacity fails",
           call("IDirect3DDevice9::SetVertexShaderConstantI", 4, a_sci16) == D3DERR_INVALIDCALL);

        int32_t *pi = (int32_t *)hle_alloc(16), *pio = (int32_t *)hle_alloc(16);
        for (int i = 0; i < 4; i++) pi[i] = 20 + i;
        memset(pio, 0, 16);
        uint32_t a_spci[4] = { dev, 0, A(pi), 1 };
        ok("SetPixelShaderConstantI == D3D_OK",
           call("IDirect3DDevice9::SetPixelShaderConstantI", 4, a_spci) == D3D_OK);
        uint32_t a_gpci[4] = { dev, 0, A(pio), 1 };
        ok("GetPixelShaderConstantI == D3D_OK",
           call("IDirect3DDevice9::GetPixelShaderConstantI", 4, a_gpci) == D3D_OK);
        ok("PS integer constant round-trip", !memcmp(pi, pio, 16));

        uint32_t *pb = (uint32_t *)hle_alloc(4), *pbo = (uint32_t *)hle_alloc(4);
        *pb = 1; *pbo = 0;
        uint32_t a_spcb[4] = { dev, 0, A(pb), 1 };
        ok("SetPixelShaderConstantB == D3D_OK",
           call("IDirect3DDevice9::SetPixelShaderConstantB", 4, a_spcb) == D3D_OK);
        uint32_t a_gpcb[4] = { dev, 0, A(pbo), 1 };
        ok("GetPixelShaderConstantB == D3D_OK",
           call("IDirect3DDevice9::GetPixelShaderConstantB", 4, a_gpcb) == D3D_OK);
        ok("PS bool constant round-trip", *pbo == 1);
        uint32_t a_spci16[4] = { dev, 16, A(pi), 1 }, a_spcb16[4] = { dev, 16, A(pb), 1 };
        ok("PS int constant past capacity fails",
           call("IDirect3DDevice9::SetPixelShaderConstantI", 4, a_spci16) == D3DERR_INVALIDCALL);
        ok("PS bool constant past capacity fails",
           call("IDirect3DDevice9::SetPixelShaderConstantB", 4, a_spcb16) == D3DERR_INVALIDCALL);
    }

    /* 18. Clip planes, scissor, viewport boundaries, render targets.
     * Public rules from the IDirect3DDevice9::SetViewport / SetScissorRect /
     * SetClipPlane / SetRenderTarget / GetDepthStencilSurface documentation
     * (learn.microsoft.com) and the D3D9 enums in d3d9.h / d3d9types.h. */
    {
        const uint32_t D3DRS_CLIPPLANEENABLE_ = 152u;
        const uint32_t D3DRS_SCISSORTESTENABLE_ = 174u;
        uint32_t *out = (uint32_t *)hle_alloc(4);
        *out = 0;
        uint32_t a_one[1] = { dev };

        /* ---- Viewport: zero size, z range and the render target edges ---- */
        D3DVIEWPORT9 *tvp = (D3DVIEWPORT9 *)hle_alloc(sizeof(*tvp));
        uint32_t a_svp[2] = { dev, A(tvp) }, a_gvp[2] = { dev, A(tvp) };
        ok("SetViewport(NULL) -> D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::SetViewport", 2, (uint32_t[]){ dev, 0 }) == D3DERR_INVALIDCALL);
        ok("GetViewport(NULL) -> D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::GetViewport", 2, (uint32_t[]){ dev, 0 }) == D3DERR_INVALIDCALL);

        memset(tvp, 0, sizeof(*tvp));
        tvp->Width = 1280; tvp->Height = 720; tvp->MaxZ = 1.0f;
        ok("full-target viewport accepted", call("IDirect3DDevice9::SetViewport", 2, a_svp) == D3D_OK);
        tvp->X = 1180; tvp->Y = 680; tvp->Width = 100; tvp->Height = 40;
        ok("viewport ending exactly at target edge accepted",
           call("IDirect3DDevice9::SetViewport", 2, a_svp) == D3D_OK);
        call("IDirect3DDevice9::GetViewport", 2, a_gvp);
        ok("viewport round-trip at target edge",
           tvp->X == 1180 && tvp->Y == 680 && tvp->Width == 100 && tvp->Height == 40);

        tvp->X = 1181;
        ok("viewport X+Width beyond target -> D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::SetViewport", 2, a_svp) == D3DERR_INVALIDCALL);
        tvp->X = 0; tvp->Y = 681; tvp->Width = 1280; tvp->Height = 40;
        ok("viewport Y+Height beyond target -> D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::SetViewport", 2, a_svp) == D3DERR_INVALIDCALL);
        tvp->Y = 0; tvp->X = 1280; tvp->Width = 1; tvp->Height = 1;
        ok("viewport origin outside target -> D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::SetViewport", 2, a_svp) == D3DERR_INVALIDCALL);
        tvp->X = 0xFFFFFFF0u; tvp->Width = 0x100u;
        ok("viewport X+Width overflow -> D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::SetViewport", 2, a_svp) == D3DERR_INVALIDCALL);
        tvp->X = 0; tvp->Width = 0; tvp->Height = 20;
        ok("viewport zero width -> D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::SetViewport", 2, a_svp) == D3DERR_INVALIDCALL);
        tvp->Width = 20; tvp->Height = 0;
        ok("viewport zero height -> D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::SetViewport", 2, a_svp) == D3DERR_INVALIDCALL);
        tvp->Height = 20; tvp->MinZ = -0.01f;
        ok("viewport MinZ < 0 -> D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::SetViewport", 2, a_svp) == D3DERR_INVALIDCALL);
        tvp->MinZ = 0.0f; tvp->MaxZ = 1.01f;
        ok("viewport MaxZ > 1 -> D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::SetViewport", 2, a_svp) == D3DERR_INVALIDCALL);
        tvp->MaxZ = 0.5f; tvp->MinZ = 0.75f;
        ok("viewport MinZ > MaxZ -> D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::SetViewport", 2, a_svp) == D3DERR_INVALIDCALL);
        /* A rejected call must not change the stored viewport. */
        call("IDirect3DDevice9::GetViewport", 2, a_gvp);
        ok("rejected viewport leaves state unchanged",
           tvp->X == 1180 && tvp->Y == 680 && tvp->Width == 100 && tvp->Height == 40 &&
           tvp->MinZ == 0.0f && tvp->MaxZ == 1.0f);

        /* ---- Clip planes: all six slots, invalid index, enable mask ---- */
        for (uint32_t i = 0; i < 6; i++) {
            float p[4] = { (float)i + 1.0f, (float)i + 2.0f, (float)i + 3.0f, (float)i + 4.0f };
            float *q = (float *)hle_alloc(16);
            float *gplane = (float *)hle_alloc(16);
            memset(q, 0, 16);
            memcpy(gplane, p, 16);
            ok("SetClipPlane slot accepted",
               call("IDirect3DDevice9::SetClipPlane", 3,
                    (uint32_t[]){ dev, i, A(gplane) }) == D3D_OK);
            ok("GetClipPlane slot accepted",
               call("IDirect3DDevice9::GetClipPlane", 3,
                    (uint32_t[]){ dev, i, A(q) }) == D3D_OK);
            ok("clip plane round-trip", !memcmp(p, q, 16));
        }
        float *gplane = (float *)hle_alloc(16);
        memset(gplane, 0, 16);
        ok("SetClipPlane(6) -> D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::SetClipPlane", 3,
                (uint32_t[]){ dev, 6, A(gplane) }) == D3DERR_INVALIDCALL);
        ok("GetClipPlane(6) -> D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::GetClipPlane", 3,
                (uint32_t[]){ dev, 6, A(gplane) }) == D3DERR_INVALIDCALL);
        ok("SetClipPlane(NULL) -> D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::SetClipPlane", 3,
                (uint32_t[]){ dev, 0, 0 }) == D3DERR_INVALIDCALL);
        ok("GetClipPlane(NULL) -> D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::GetClipPlane", 3,
                (uint32_t[]){ dev, 0, 0 }) == D3DERR_INVALIDCALL);

        uint32_t mask = 0x2Fu;  /* planes 0-3 and 5 */
        call("IDirect3DDevice9::SetRenderState", 3,
             (uint32_t[]){ dev, D3DRS_CLIPPLANEENABLE_, mask });
        *out = 0;
        call("IDirect3DDevice9::GetRenderState", 3,
             (uint32_t[]){ dev, D3DRS_CLIPPLANEENABLE_, A(out) });
        ok("clip-plane enable mask round-trip", *out == mask);
        int clip_before = g_clip_calls;
        call("IDirect3DDevice9::BeginScene", 1, a_one);
        call("IDirect3DDevice9::DrawPrimitive", 4,
             (uint32_t[]){ dev, D3DPT_TRIANGLELIST, 0, 1 });
        call("IDirect3DDevice9::EndScene", 1, a_one);
        ok("clip-plane enable mask reaches the backend",
           g_clip_calls == clip_before + 1 && g_last_clip_mask == mask);
        call("IDirect3DDevice9::SetRenderState", 3,
             (uint32_t[]){ dev, D3DRS_CLIPPLANEENABLE_, 0 });

        /* ---- Scissor rect: order, empty rect and null pointer ---- */
        D3DRECT *sr = (D3DRECT *)hle_alloc(sizeof(*sr));
        D3DRECT *sq = (D3DRECT *)hle_alloc(sizeof(*sq));
        sr->x1 = 8; sr->y1 = 12; sr->x2 = 40; sr->y2 = 60;
        ok("SetScissorRect(valid) -> D3D_OK",
           call("IDirect3DDevice9::SetScissorRect", 2,
                (uint32_t[]){ dev, A(sr) }) == D3D_OK);
        memset(sq, 0, sizeof(*sq));
        call("IDirect3DDevice9::GetScissorRect", 2, (uint32_t[]){ dev, A(sq) });
        ok("scissor round-trip", !memcmp(sr, sq, sizeof(*sr)));
        call("IDirect3DDevice9::SetRenderState", 3,
             (uint32_t[]){ dev, D3DRS_SCISSORTESTENABLE_, 1 });
        call("IDirect3DDevice9::BeginScene", 1, a_one);
        call("IDirect3DDevice9::DrawPrimitive", 4,
             (uint32_t[]){ dev, D3DPT_TRIANGLELIST, 0, 1 });
        call("IDirect3DDevice9::EndScene", 1, a_one);
        ok("scissor rect and enable reach the backend",
           g_last_scissor_enable == 1 && !memcmp(g_last_scissor, sr, sizeof(*sr)));
        D3DRECT *bad = (D3DRECT *)hle_alloc(sizeof(*bad));
        bad->x1 = 40; bad->y1 = 12; bad->x2 = 8; bad->y2 = 60;
        ok("inverted scissor -> D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::SetScissorRect", 2,
                (uint32_t[]){ dev, A(bad) }) == D3DERR_INVALIDCALL);
        memset(sq, 0, sizeof(*sq));
        call("IDirect3DDevice9::GetScissorRect", 2, (uint32_t[]){ dev, A(sq) });
        ok("rejected scissor leaves state unchanged", !memcmp(sr, sq, sizeof(*sr)));
        sr->x1 = 8; sr->x2 = 8; sr->y1 = 12; sr->y2 = 12;
        ok("empty scissor rect -> D3D_OK",
           call("IDirect3DDevice9::SetScissorRect", 2,
                (uint32_t[]){ dev, A(sr) }) == D3D_OK);
        ok("SetScissorRect(NULL) -> D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::SetScissorRect", 2,
                (uint32_t[]){ dev, 0 }) == D3DERR_INVALIDCALL);
        ok("GetScissorRect(NULL) -> D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::GetScissorRect", 2,
                (uint32_t[]){ dev, 0 }) == D3DERR_INVALIDCALL);
        call("IDirect3DDevice9::SetRenderState", 3,
             (uint32_t[]){ dev, D3DRS_SCISSORTESTENABLE_, 0 });
        call("IDirect3DDevice9::BeginScene", 1, a_one);
        call("IDirect3DDevice9::DrawPrimitive", 4,
             (uint32_t[]){ dev, D3DPT_TRIANGLELIST, 0, 1 });
        call("IDirect3DDevice9::EndScene", 1, a_one);
        ok("scissor disable reaches the backend", g_last_scissor_enable == 0);

        /* ---- Render targets: index 0 must be non-NULL and valid ---- */
        uint32_t *saved_rt = (uint32_t *)hle_alloc(4);
        uint32_t *new_rt = (uint32_t *)hle_alloc(4);
        uint32_t *plain = (uint32_t *)hle_alloc(4);
        *new_rt = 0; *plain = 0;
        call("IDirect3DDevice9::GetRenderTarget", 3,
             (uint32_t[]){ dev, 0, A(saved_rt) });
        ok("CreateRenderTarget(64x64) -> D3D_OK",
           call("IDirect3DDevice9::CreateRenderTarget", 9,
                (uint32_t[]){ dev, 64, 64, D3DFMT_A8R8G8B8, 0, 0, 0, A(new_rt), 0 }) == D3D_OK && *new_rt);
        ok("SetRenderTarget(valid RT) -> D3D_OK",
           call("IDirect3DDevice9::SetRenderTarget", 3,
                (uint32_t[]){ dev, 0, *new_rt }) == D3D_OK);
        /* The bounds check uses the newly bound 64x64 target, not the
         * presentation parameters (1280x720). */
        memset(tvp, 0, sizeof(*tvp));
        tvp->Width = 64; tvp->Height = 64; tvp->MaxZ = 1.0f;
        ok("viewport within the new target -> D3D_OK",
           call("IDirect3DDevice9::SetViewport", 2, a_svp) == D3D_OK);
        tvp->Width = 65;
        ok("viewport beyond the new target -> D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::SetViewport", 2, a_svp) == D3DERR_INVALIDCALL);
        tvp->Width = 64;
        *out = 0;
        call("IDirect3DDevice9::GetRenderTarget", 3,
             (uint32_t[]){ dev, 0, A(out) });
        ok("GetRenderTarget returns the bound surface", *out == *new_rt);
        call("IDirect3DSurface9::Release", 1, (uint32_t[]){ *out });
        ok("CreateOffscreenPlainSurface -> D3D_OK",
           call("IDirect3DDevice9::CreateOffscreenPlainSurface", 7,
                (uint32_t[]){ dev, 64, 64, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, A(plain), 0 }) == D3D_OK && *plain);
        ok("SetRenderTarget(non-RT surface) -> D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::SetRenderTarget", 3,
                (uint32_t[]){ dev, 0, *plain }) == D3DERR_INVALIDCALL);
        ok("SetRenderTarget(index 1) -> D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::SetRenderTarget", 3,
                (uint32_t[]){ dev, 1, *new_rt }) == D3DERR_INVALIDCALL);
        ok("SetRenderTarget(index 0, NULL) -> D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::SetRenderTarget", 3,
                (uint32_t[]){ dev, 0, 0 }) == D3DERR_INVALIDCALL);
        call("IDirect3DDevice9::SetRenderTarget", 3,
             (uint32_t[]){ dev, 0, *saved_rt });

        /* ---- Depth stencil: null out-pointer, AddRef, NULL surface ---- */
        uint32_t *saved_ds = (uint32_t *)hle_alloc(4);
        *saved_ds = 0;
        ok("GetDepthStencilSurface(NULL) -> D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::GetDepthStencilSurface", 2,
                (uint32_t[]){ dev, 0 }) == D3DERR_INVALIDCALL);
        ok("GetDepthStencilSurface -> D3D_OK with a surface",
           call("IDirect3DDevice9::GetDepthStencilSurface", 2,
                (uint32_t[]){ dev, A(saved_ds) }) == D3D_OK && *saved_ds != 0);
        uint32_t ds_refs = MEM32(*saved_ds + 4);
        *out = 0;
        call("IDirect3DDevice9::GetDepthStencilSurface", 2,
             (uint32_t[]){ dev, A(out) });
        ok("GetDepthStencilSurface AddRef'd the surface",
           *out == *saved_ds && MEM32(*saved_ds + 4) == ds_refs + 1);
        call("IDirect3DSurface9::Release", 1, (uint32_t[]){ *out });
        ok("releasing the queried surface restores the refcount",
           MEM32(*saved_ds + 4) == ds_refs);
        ok("SetDepthStencilSurface(NULL) -> D3D_OK",
           call("IDirect3DDevice9::SetDepthStencilSurface", 2,
                (uint32_t[]){ dev, 0 }) == D3D_OK);
        *out = 0xFFFFFFFFu;
        ok("no depth stencil -> D3DERR_NOTFOUND and NULL",
           call("IDirect3DDevice9::GetDepthStencilSurface", 2,
                (uint32_t[]){ dev, A(out) }) == D3DERR_NOTFOUND && *out == 0);
        call("IDirect3DDevice9::SetDepthStencilSurface", 2,
             (uint32_t[]){ dev, *saved_ds });
        call("IDirect3DSurface9::Release", 1, (uint32_t[]){ *saved_ds });
        call("IDirect3DSurface9::Release", 1, (uint32_t[]){ *new_rt });
        call("IDirect3DSurface9::Release", 1, (uint32_t[]){ *plain });
        call("IDirect3DSurface9::Release", 1, (uint32_t[]){ *saved_rt });
    }

    /* 19. Alpha test, stencil, depth bias, blend op, separate alpha, color write */
    {
        uint32_t a_draw[4] = { dev, D3DPT_TRIANGLELIST, 0, 1 };
        uint32_t *rs_out = (uint32_t *)hle_alloc(4);
        struct { uint32_t rs; uint32_t val; } rt[] = {
            { D3DRS_ALPHAFUNC, D3DCMP_LESS },
            { D3DRS_ALPHAREF, 128 },
            { D3DRS_STENCILFAIL, D3DSTENCILOP_REPLACE },
            { D3DRS_STENCILZFAIL, D3DSTENCILOP_INCRSAT },
            { D3DRS_STENCILPASS, D3DSTENCILOP_DECR },
            { D3DRS_STENCILFUNC, D3DCMP_EQUAL },
            { D3DRS_STENCILREF, 0xAB },
            { D3DRS_STENCILMASK, 0x3C },
            { D3DRS_STENCILWRITEMASK, 0x7F },
            { D3DRS_BLENDOP, D3DBLENDOP_MAX },
            { D3DRS_SEPARATEALPHABLENDENABLE, 1 },
            { D3DRS_SRCBLENDALPHA, D3DBLEND_INVSRCALPHA },
            { D3DRS_DESTBLENDALPHA, D3DBLEND_ONE },
            { D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD },
            { D3DRS_COLORWRITEENABLE, 0x3 },
            { D3DRS_COLORWRITEENABLE1, 0xC },
            { D3DRS_COLORWRITEENABLE2, 0x5 },
            { D3DRS_COLORWRITEENABLE3, 0xA },
        };
        for (size_t i = 0; i < sizeof rt / sizeof rt[0]; i++) {
            char msg[96];
            call("IDirect3DDevice9::SetRenderState", 3,
                 (uint32_t[]){ dev, rt[i].rs, rt[i].val });
            call("IDirect3DDevice9::GetRenderState", 3,
                 (uint32_t[]){ dev, rt[i].rs, A(rs_out) });
            snprintf(msg, sizeof msg, "Set/GetRenderState(%u) roundtrip", rt[i].rs);
            ok(msg, *rs_out == rt[i].val);
        }
        /* Restore supported defaults for the states tested above. */
        struct { uint32_t rs; uint32_t val; } rst[] = {
            { D3DRS_ALPHAFUNC, D3DCMP_ALWAYS },
            { D3DRS_ALPHAREF, 0 },
            { D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP },
            { D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP },
            { D3DRS_STENCILPASS, D3DSTENCILOP_KEEP },
            { D3DRS_STENCILFUNC, D3DCMP_ALWAYS },
            { D3DRS_STENCILREF, 0 },
            { D3DRS_STENCILMASK, 0xFF },
            { D3DRS_STENCILWRITEMASK, 0xFF },
            { D3DRS_BLENDOP, D3DBLENDOP_ADD },
            { D3DRS_SEPARATEALPHABLENDENABLE, 0 },
            { D3DRS_SRCBLENDALPHA, D3DBLEND_SRCALPHA },
            { D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA },
            { D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD },
            { D3DRS_COLORWRITEENABLE, 0xF },
            { D3DRS_COLORWRITEENABLE1, 0xF },
            { D3DRS_COLORWRITEENABLE2, 0xF },
            { D3DRS_COLORWRITEENABLE3, 0xF },
        };
        for (size_t i = 0; i < sizeof rst / sizeof rst[0]; i++)
            call("IDirect3DDevice9::SetRenderState", 3,
                 (uint32_t[]){ dev, rst[i].rs, rst[i].val });

        /* Float-valued states (DEPTHBIAS, SLOPESCALEDDEPTHBIAS): stored as
         * float bits, like the Depth Bias page ("cast the value to a DWORD"). */
        float fv = 2.5f, sv = -0.5f;
        uint32_t fb, sb;
        memcpy(&fb, &fv, 4); memcpy(&sb, &sv, 4);
        ok("SetRenderState(DEPTHBIAS) D3D_OK",
           call("IDirect3DDevice9::SetRenderState", 3,
                (uint32_t[]){ dev, D3DRS_DEPTHBIAS, fb }) == D3D_OK);
        call("IDirect3DDevice9::GetRenderState", 3,
             (uint32_t[]){ dev, D3DRS_DEPTHBIAS, A(rs_out) });
        ok("GetRenderState(DEPTHBIAS) == 2.5f bits", *rs_out == fb);
        ok("SetRenderState(SLOPESCALEDDEPTHBIAS) D3D_OK",
           call("IDirect3DDevice9::SetRenderState", 3,
                (uint32_t[]){ dev, D3DRS_SLOPESCALEDEPTHBIAS, sb }) == D3D_OK);
        call("IDirect3DDevice9::GetRenderState", 3,
             (uint32_t[]){ dev, D3DRS_SLOPESCALEDEPTHBIAS, A(rs_out) });
        ok("GetRenderState(SLOPESCALEDDEPTHBIAS) == -0.5f bits", *rs_out == sb);
        call("IDirect3DDevice9::SetRenderState", 3,
             (uint32_t[]){ dev, D3DRS_DEPTHBIAS, 0 });
        call("IDirect3DDevice9::SetRenderState", 3,
             (uint32_t[]){ dev, D3DRS_SLOPESCALEDEPTHBIAS, 0 });

        /* Invalid values: D3DERR_INVALIDCALL per the public docs. */
        struct { uint32_t rs; uint32_t val; } bad[] = {
            { D3DRS_SRCBLEND, 99 }, { D3DRS_DESTBLEND, 99 },
            { D3DRS_ZFUNC, 0 }, { D3DRS_ZFUNC, 9 },
            { D3DRS_ALPHAFUNC, 0 }, { D3DRS_ALPHAFUNC, 9 },
            { D3DRS_STENCILFUNC, 0 }, { D3DRS_CCW_STENCILFUNC, 99 },
            { D3DRS_BLENDOP, 42 },
            { D3DRS_STENCILFAIL, 9 }, { D3DRS_STENCILPASS, 0 },
            { D3DRS_CCW_STENCILZFAIL, 0 }, { D3DRS_CCW_STENCILPASS, 99 },
            { D3DRS_COLORWRITEENABLE, 0x10 }, { D3DRS_COLORWRITEENABLE1, 0x20 },
            { D3DRS_ALPHATESTENABLE, 2 },
        };
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            char msg[96];
            snprintf(msg, sizeof msg, "SetRenderState(%u, 0x%x) invalid",
                     bad[i].rs, bad[i].val);
            ok(msg, call("IDirect3DDevice9::SetRenderState", 3,
                         (uint32_t[]){ dev, bad[i].rs, bad[i].val }) == D3DERR_INVALIDCALL);
        }

        /* Honest draws: unsupported combinations report D3DERR_NOTAVAILABLE
         * (Metal backend cannot apply them); supported ones stay D3D_OK.
         * Every draw is preceded by a supported-state reset so the
         * expectation is deterministic. */
        ok("SetRenderState(STENCILENABLE,0) D3D_OK",
           call("IDirect3DDevice9::SetRenderState", 3,
                (uint32_t[]){ dev, D3DRS_STENCILENABLE, 0 }) == D3D_OK);
        ok("SetRenderState(DEPTHBIAS,0) D3D_OK",
           call("IDirect3DDevice9::SetRenderState", 3,
                (uint32_t[]){ dev, D3DRS_DEPTHBIAS, 0 }) == D3D_OK);
        ok("SetRenderState(SLOPESCALEDDEPTHBIAS,0) D3D_OK",
           call("IDirect3DDevice9::SetRenderState", 3,
                (uint32_t[]){ dev, D3DRS_SLOPESCALEDEPTHBIAS, 0 }) == D3D_OK);
        ok("SetRenderState(COLORWRITEENABLE,0xF) D3D_OK",
           call("IDirect3DDevice9::SetRenderState", 3,
                (uint32_t[]){ dev, D3DRS_COLORWRITEENABLE, 0xF }) == D3D_OK);
        ok("SetRenderState(COLORWRITEENABLE1,0xF) D3D_OK",
           call("IDirect3DDevice9::SetRenderState", 3,
                (uint32_t[]){ dev, D3DRS_COLORWRITEENABLE1, 0xF }) == D3D_OK);
        ok("SetRenderState(BLENDOP,D3DBLENDOP_ADD) D3D_OK",
           call("IDirect3DDevice9::SetRenderState", 3,
                (uint32_t[]){ dev, D3DRS_BLENDOP, D3DBLENDOP_ADD }) == D3D_OK);
        ok("SetRenderState(SEPARATEALPHABLENDENABLE,0) D3D_OK",
           call("IDirect3DDevice9::SetRenderState", 3,
                (uint32_t[]){ dev, D3DRS_SEPARATEALPHABLENDENABLE, 0 }) == D3D_OK);
        ok("DrawPrimitive == D3D_OK (supported state)",
           call("IDirect3DDevice9::DrawPrimitive", 4, a_draw) == D3D_OK);

        ok("SetRenderState(STENCILENABLE,1) D3D_OK",
           call("IDirect3DDevice9::SetRenderState", 3,
                (uint32_t[]){ dev, D3DRS_STENCILENABLE, 1 }) == D3D_OK);
        ok("DrawPrimitive with stencil == D3DERR_NOTAVAILABLE",
           call("IDirect3DDevice9::DrawPrimitive", 4, a_draw) == D3DERR_NOTAVAILABLE);
        ok("SetRenderState(STENCILENABLE,0) D3D_OK",
           call("IDirect3DDevice9::SetRenderState", 3,
                (uint32_t[]){ dev, D3DRS_STENCILENABLE, 0 }) == D3D_OK);

        ok("SetRenderState(DEPTHBIAS,float) D3D_OK",
           call("IDirect3DDevice9::SetRenderState", 3,
                (uint32_t[]){ dev, D3DRS_DEPTHBIAS, fb }) == D3D_OK);
        ok("DrawPrimitive with DEPTHBIAS != 0 == D3DERR_NOTAVAILABLE",
           call("IDirect3DDevice9::DrawPrimitive", 4, a_draw) == D3DERR_NOTAVAILABLE);
        ok("SetRenderState(DEPTHBIAS,0) D3D_OK",
           call("IDirect3DDevice9::SetRenderState", 3,
                (uint32_t[]){ dev, D3DRS_DEPTHBIAS, 0 }) == D3D_OK);

        ok("SetRenderState(SLOPESCALEDDEPTHBIAS,float) D3D_OK",
           call("IDirect3DDevice9::SetRenderState", 3,
                (uint32_t[]){ dev, D3DRS_SLOPESCALEDEPTHBIAS, sb }) == D3D_OK);
        ok("DrawPrimitive with SLOPESCALEDDEPTHBIAS != 0 == D3DERR_NOTAVAILABLE",
           call("IDirect3DDevice9::DrawPrimitive", 4, a_draw) == D3DERR_NOTAVAILABLE);
        ok("SetRenderState(SLOPESCALEDDEPTHBIAS,0) D3D_OK",
           call("IDirect3DDevice9::SetRenderState", 3,
                (uint32_t[]){ dev, D3DRS_SLOPESCALEDEPTHBIAS, 0 }) == D3D_OK);

        ok("SetRenderState(COLORWRITEENABLE,0) D3D_OK",
           call("IDirect3DDevice9::SetRenderState", 3,
                (uint32_t[]){ dev, D3DRS_COLORWRITEENABLE, 0 }) == D3D_OK);
        ok("DrawPrimitive with COLORWRITEENABLE == 0 == D3DERR_NOTAVAILABLE",
           call("IDirect3DDevice9::DrawPrimitive", 4, a_draw) == D3DERR_NOTAVAILABLE);
        ok("SetRenderState(COLORWRITEENABLE,0xF) D3D_OK",
           call("IDirect3DDevice9::SetRenderState", 3,
                (uint32_t[]){ dev, D3DRS_COLORWRITEENABLE, 0xF }) == D3D_OK);
        ok("SetRenderState(COLORWRITEENABLE1,0x8) D3D_OK",
           call("IDirect3DDevice9::SetRenderState", 3,
                (uint32_t[]){ dev, D3DRS_COLORWRITEENABLE1, 0x8 }) == D3D_OK);
        ok("DrawPrimitive with COLORWRITEENABLE1 == 0x8 == D3DERR_NOTAVAILABLE",
           call("IDirect3DDevice9::DrawPrimitive", 4, a_draw) == D3DERR_NOTAVAILABLE);
        ok("SetRenderState(COLORWRITEENABLE1,0xF) D3D_OK",
           call("IDirect3DDevice9::SetRenderState", 3,
                (uint32_t[]){ dev, D3DRS_COLORWRITEENABLE1, 0xF }) == D3D_OK);

        ok("SetRenderState(BLENDOP,SUBTRACT) D3D_OK",
           call("IDirect3DDevice9::SetRenderState", 3,
                (uint32_t[]){ dev, D3DRS_BLENDOP, D3DBLENDOP_SUBTRACT }) == D3D_OK);
        ok("DrawPrimitive with BLENDOP != ADD == D3DERR_NOTAVAILABLE",
           call("IDirect3DDevice9::DrawPrimitive", 4, a_draw) == D3DERR_NOTAVAILABLE);
        ok("SetRenderState(BLENDOP,ADD) D3D_OK",
           call("IDirect3DDevice9::SetRenderState", 3,
                (uint32_t[]){ dev, D3DRS_BLENDOP, D3DBLENDOP_ADD }) == D3D_OK);

        ok("SetRenderState(SEPARATEALPHABLENDENABLE,1) D3D_OK",
           call("IDirect3DDevice9::SetRenderState", 3,
                (uint32_t[]){ dev, D3DRS_SEPARATEALPHABLENDENABLE, 1 }) == D3D_OK);
        ok("SetRenderState(SRCBLENDALPHA,D3DBLEND_INVSRCALPHA) D3D_OK",
           call("IDirect3DDevice9::SetRenderState", 3,
                (uint32_t[]){ dev, D3DRS_SRCBLENDALPHA, D3DBLEND_INVSRCALPHA }) == D3D_OK);
        ok("DrawPrimitive with separate alpha != rgb == D3DERR_NOTAVAILABLE",
           call("IDirect3DDevice9::DrawPrimitive", 4, a_draw) == D3DERR_NOTAVAILABLE);
        ok("SetRenderState(SRCBLENDALPHA,D3DBLEND_SRCALPHA) D3D_OK",
           call("IDirect3DDevice9::SetRenderState", 3,
                (uint32_t[]){ dev, D3DRS_SRCBLENDALPHA, D3DBLEND_SRCALPHA }) == D3D_OK);
        ok("SetRenderState(DESTBLENDALPHA,D3DBLEND_INVSRCALPHA) D3D_OK",
           call("IDirect3DDevice9::SetRenderState", 3,
                (uint32_t[]){ dev, D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA }) == D3D_OK);
        ok("DrawPrimitive with separate alpha == rgb == D3D_OK",
           call("IDirect3DDevice9::DrawPrimitive", 4, a_draw) == D3D_OK);
        ok("SetRenderState(SEPARATEALPHABLENDENABLE,0) D3D_OK",
           call("IDirect3DDevice9::SetRenderState", 3,
                (uint32_t[]){ dev, D3DRS_SEPARATEALPHABLENDENABLE, 0 }) == D3D_OK);

        /* Alpha test is supported by the HLE's fixed-function pipeline. */
        ok("SetRenderState(ALPHATESTENABLE,1) D3D_OK",
           call("IDirect3DDevice9::SetRenderState", 3,
                (uint32_t[]){ dev, D3DRS_ALPHATESTENABLE, 1 }) == D3D_OK);
        ok("SetRenderState(ALPHAFUNC,D3DCMP_LESS) D3D_OK",
           call("IDirect3DDevice9::SetRenderState", 3,
                (uint32_t[]){ dev, D3DRS_ALPHAFUNC, D3DCMP_LESS }) == D3D_OK);
        ok("SetRenderState(ALPHAREF,128) D3D_OK",
           call("IDirect3DDevice9::SetRenderState", 3,
                (uint32_t[]){ dev, D3DRS_ALPHAREF, 128 }) == D3D_OK);
        ok("DrawPrimitive with alpha test == D3D_OK",
           call("IDirect3DDevice9::DrawPrimitive", 4, a_draw) == D3D_OK);
        ok("SetRenderState(ALPHATESTENABLE,0) D3D_OK",
           call("IDirect3DDevice9::SetRenderState", 3,
                (uint32_t[]){ dev, D3DRS_ALPHATESTENABLE, 0 }) == D3D_OK);
    }

    /* 20. Cleanup & Release */
    uint32_t a_rel_q[1] = { *pQuery };
    call("IDirect3DQuery9::Release", 1, a_rel_q);

    uint32_t a_rel_ps[1] = { *pPS };
    call("IDirect3DPixelShader9::Release", 1, a_rel_ps);

    uint32_t a_rel_vs[1] = { *pVS };
    call("IDirect3DVertexShader9::Release", 1, a_rel_vs);

    uint32_t a_rel_decl[1] = { *pDecl };
    call("IDirect3DVertexDeclaration9::Release", 1, a_rel_decl);

    uint32_t a_rel_tex[1] = { *pTex };
    call("IDirect3DTexture9::Release", 1, a_rel_tex);

    uint32_t a_rel_ib[1] = { *pIB };
    call("IDirect3DIndexBuffer9::Release", 1, a_rel_ib);

    uint32_t a_rel_vb[1] = { *pVB };
    call("IDirect3DVertexBuffer9::Release", 1, a_rel_vb);

    uint32_t a_rel_dev[1] = { dev };
    call("IDirect3DDevice9::Release", 1, a_rel_dev);

    uint32_t a_rel_d3d9[1] = { d3d9 };
    call("IDirect3D9::Release", 1, a_rel_d3d9);

    if (fails) {
        printf("d3d9_selftest: %d FAILS\n", fails);
        return 1;
    }
    printf("d3d9_selftest: all tests passed\n");
    return 0;
}
