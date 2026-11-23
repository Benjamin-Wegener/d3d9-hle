/*
 * d3d9_stateblock_selftest.c - Selftest for IDirect3DStateBlock9.
 *
 * Covers CreateStateBlock type validation, ALL/PIXEL/VERTEX capture & apply,
 * Begin/EndStateBlock, held resource references, GetDevice, and the backend
 * refresh that must follow StateBlock::Apply.
 *
 * All mutations go through the real HLE dispatch (Set, Create, Apply, Capture,
 * Release). State is read back directly from the device context because the
 * D3D9 Get* methods AddRef their out-parameters, which would perturb the very
 * reference counts this test needs to check.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "host.h"
#include "d3d9_hle.h"
#include "d3d9_internal.h"

/* guest pointer to IID_IUnknown {00000000-0000-0000-C000-000000000046} */
static uint32_t iid_unk(void) {
    uint8_t *g = (uint8_t *)hle_alloc(16);
    memset(g, 0, 16); g[8] = 0xC0; g[15] = 0x46;
    return hle_va(g);
}

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
#define REF(o) MEM32((o) + 4)

static d3d9_device_ctx_t *dc(uint32_t dev) {
    return (d3d9_device_ctx_t *)OBJ_CTX(dev);
}

/* ---- mock backend: records state pushes caused by StateBlock::Apply ---- */
static int g_rs_pushes, g_blend, g_src, g_dst, g_scissor_en;
static int g_const_pushes;
static float g_vsf0, g_psf0;
static int g_rt_pushes;
static int g_sampler_pushes;
static void *g_sampler0;

static int mock_active(void) { return 1; }
static int mock_enable(void) { return 0; }
static int mock_init(uint32_t w, uint32_t h) { (void)w; (void)h; return 0; }
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
static int mock_draw_indexed(int t, uint32_t b, uint32_t s, uint32_t n,
                             void *p, void *v, void *i, uint32_t st) {
    (void)t; (void)b; (void)s; (void)n; (void)p; (void)v; (void)i; (void)st; return 0;
}
static void mock_set_rt(void *rt) { (void)rt; g_rt_pushes++; }
static void *mock_create_rt(uint32_t w, uint32_t h) { (void)w; (void)h; return (void *)4; }
static void mock_destroy_rt(void *rt) { (void)rt; }
static void mock_set_tex(void *tex) { (void)tex; }
static void mock_set_sampler(uint32_t stage, void *tex) {
    if (stage == 0) { g_sampler0 = tex; g_sampler_pushes++; }
}
static void mock_set_rs(uint32_t blend, uint32_t src, uint32_t dst, uint32_t ze,
                        uint32_t zw, uint32_t zf, uint32_t cm, uint32_t sc,
                        const int32_t rect[4]) {
    (void)ze; (void)zw; (void)zf; (void)cm; (void)rect;
    g_rs_pushes++; g_blend = blend; g_src = src; g_dst = dst; g_scissor_en = sc;
}
static void mock_set_decl(const D3DVERTEXELEMENT9 *e, uint32_t n) { (void)e; (void)n; }
static void mock_set_consts(const float *vsf, const int32_t *vsi, const uint32_t *vsb,
                            const float *psf, const int32_t *psi, const uint32_t *psb) {
    (void)vsi; (void)vsb; (void)psi; (void)psb;
    g_const_pushes++; g_vsf0 = vsf[0]; g_psf0 = psf[0];
}

static const d3d9_gpu_ops_t g_mock_gpu = {
    "stateblock-mock", mock_active, mock_enable, mock_init, mock_shutdown,
    mock_begin, mock_end, mock_clear, mock_present,
    mock_tex, mock_tex_destroy, mock_tex_upload,
    mock_buf, mock_buf_destroy, mock_buf_upload,
    mock_pipe, mock_pipe_destroy, mock_draw,
    mock_set_rt, mock_create_rt, mock_destroy_rt, mock_set_tex,
    NULL, NULL, mock_draw_indexed, mock_set_rs,
    mock_set_decl, mock_set_consts, mock_set_sampler
};

/* ---- tiny guest-memory backed setters ---- */
static void set_rs(uint32_t dev, uint32_t rs, uint32_t val) {
    uint32_t a[3] = { dev, rs, val };
    call("IDirect3DDevice9::SetRenderState", 3, a);
}
static void set_f(float *dst, float x) { memset(dst, 0, 16); *dst = x; }
static void set_vsf(uint32_t dev, uint32_t reg, float x) {
    float *d = (float *)hle_alloc(16); set_f(d, x);
    uint32_t a[4] = { dev, reg, A(d), 1 };
    call("IDirect3DDevice9::SetVertexShaderConstantF", 4, a);
}
static void set_psf(uint32_t dev, uint32_t reg, float x) {
    float *d = (float *)hle_alloc(16); set_f(d, x);
    uint32_t a[4] = { dev, reg, A(d), 1 };
    call("IDirect3DDevice9::SetPixelShaderConstantF", 4, a);
}
static void set_vsi(uint32_t dev, uint32_t reg, int32_t x) {
    int32_t *d = (int32_t *)hle_alloc(16); memset(d, 0, 16); *d = x;
    uint32_t a[4] = { dev, reg, A(d), 1 };
    call("IDirect3DDevice9::SetVertexShaderConstantI", 4, a);
}
static void set_vsb(uint32_t dev, uint32_t reg, uint32_t x) {
    uint32_t *d = (uint32_t *)hle_alloc(4); *d = x;
    uint32_t a[4] = { dev, reg, A(d), 1 };
    call("IDirect3DDevice9::SetVertexShaderConstantB", 4, a);
}
static void set_psb(uint32_t dev, uint32_t reg, uint32_t x) {
    uint32_t *d = (uint32_t *)hle_alloc(4); *d = x;
    uint32_t a[4] = { dev, reg, A(d), 1 };
    call("IDirect3DDevice9::SetPixelShaderConstantB", 4, a);
}
static void set_proj(uint32_t dev, float x) {
    D3DMATRIX *m = (D3DMATRIX *)hle_alloc(sizeof(*m));
    memset(m, 0, sizeof(*m)); m->m[0][0] = x;
    uint32_t a[3] = { dev, 2 /* D3DTS_PROJECTION */, A(m) };
    call("IDirect3DDevice9::SetTransform", 3, a);
}
static void set_vp(uint32_t dev, uint32_t w) {
    D3DVIEWPORT9 *v = (D3DVIEWPORT9 *)hle_alloc(sizeof(*v));
    memset(v, 0, sizeof(*v)); v->Width = w; v->Height = w + 1;
    uint32_t a[2] = { dev, A(v) };
    call("IDirect3DDevice9::SetViewport", 2, a);
}
static void set_scissor(uint32_t dev, int32_t x) {
    D3DRECT *r = (D3DRECT *)hle_alloc(sizeof(*r));
    memset(r, 0, sizeof(*r)); r->x1 = x; r->x2 = x + 10;
    uint32_t a[2] = { dev, A(r) };
    call("IDirect3DDevice9::SetScissorRect", 2, a);
}
static void set_tss(uint32_t dev, uint32_t type, uint32_t val) {
    uint32_t a[4] = { dev, 0, type, val };
    call("IDirect3DDevice9::SetTextureStageState", 4, a);
}
static void set_sampler(uint32_t dev, uint32_t type, uint32_t val) {
    uint32_t a[4] = { dev, 0, type, val };
    call("IDirect3DDevice9::SetSamplerState", 4, a);
}
static void set_tex(uint32_t dev, uint32_t tex, uint32_t stage) {
    uint32_t a[3] = { dev, stage, tex };
    call("IDirect3DDevice9::SetTexture", 3, a);
}
static void set_stream(uint32_t dev, uint32_t vb, uint32_t off, uint32_t stride) {
    uint32_t a[5] = { dev, 0, vb, off, stride };
    call("IDirect3DDevice9::SetStreamSource", 5, a);
}
static void set_freq(uint32_t dev, uint32_t f) {
    uint32_t a[3] = { dev, 0, f };
    call("IDirect3DDevice9::SetStreamSourceFreq", 3, a);
}
static void set_indices(uint32_t dev, uint32_t ib) {
    uint32_t a[2] = { dev, ib };
    call("IDirect3DDevice9::SetIndices", 2, a);
}
static void set_vs(uint32_t dev, uint32_t vs) {
    uint32_t a[2] = { dev, vs };
    call("IDirect3DDevice9::SetVertexShader", 2, a);
}
static void set_ps(uint32_t dev, uint32_t ps) {
    uint32_t a[2] = { dev, ps };
    call("IDirect3DDevice9::SetPixelShader", 2, a);
}
static void set_decl(uint32_t dev, uint32_t decl) {
    uint32_t a[2] = { dev, decl };
    call("IDirect3DDevice9::SetVertexDeclaration", 2, a);
}
static void set_fvf(uint32_t dev, uint32_t fvf) {
    uint32_t a[2] = { dev, fvf };
    call("IDirect3DDevice9::SetFVF", 2, a);
}
static void set_rt(uint32_t dev, uint32_t rt) {
    uint32_t a[3] = { dev, 0, rt };
    call("IDirect3DDevice9::SetRenderTarget", 3, a);
}
static uint32_t create_sb(uint32_t dev, uint32_t type) {
    uint32_t *out = (uint32_t *)hle_alloc(4); *out = 0;
    uint32_t a[3] = { dev, type, A(out) };
    call("IDirect3DDevice9::CreateStateBlock", 3, a);
    return *out;
}
static uint32_t apply_sb(uint32_t sb) {
    uint32_t a[1] = { sb };
    return call("IDirect3DStateBlock9::Apply", 1, a);
}
static uint32_t capture_sb(uint32_t sb) {
    uint32_t a[1] = { sb };
    return call("IDirect3DStateBlock9::Capture", 1, a);
}
static uint32_t release_sb(uint32_t sb) {
    uint32_t a[1] = { sb };
    return call("IDirect3DStateBlock9::Release", 1, a);
}

/* Enter a guest-vtable slot directly and measure how far the shim moved esp.
 * hle_call_guest() saves and restores esp, so it would hide a stack imbalance:
 * a correct stdcall shim leaves esp exactly where it was before the pushes
 * (delta == 0 once return address and arguments are popped). */
static uint32_t raw_slot(uint32_t fva, int argc, const uint32_t *a, uint32_t *delta, uint32_t *eax) {
    mach_enter();
    uint32_t esp0 = g_esp;
    for (int k = argc - 1; k >= 0; k--) PUSH32(g_esp, a[k]);
    PUSH32(g_esp, RECOMP_RETADDR);
    recomp_func_t f = recomp_lookup_import(fva);
    int called = 0;
    if (f) { called = 1; f(); }
    *delta = g_esp - esp0;
    if (eax) *eax = g_eax;
    g_esp = esp0;
    mach_leave();
    return (uint32_t)called;
}
/* Slot i of an object's vtable, entered directly; ok() checks balance. */
static void check_slot(const char *what, uint32_t obj, int slot, int argc, const uint32_t *a, uint32_t *eax) {
    uint32_t delta = 0xFFFFu;
    uint32_t called = raw_slot(MEM32(MEM32(obj) + 4u * (uint32_t)slot), argc, a, &delta, eax);
    char msg[128];
    snprintf(msg, sizeof msg, "%s: entered, esp balanced (delta %u)", what, (unsigned)delta);
    ok(msg, called && delta == 0u);
}

const recomp_dispatch_entry_t recomp_dispatch_table[1];
const uint32_t recomp_dispatch_count = 0;

int main(void) {
    recomp_host_init();
    win32hle_register(win32hle_d3d9);
    const d3d9_gpu_ops_t *prev_gpu = hle_d3d9_gpu;
    hle_d3d9_set_gpu(&g_mock_gpu);

    uint32_t d3d9 = call("Direct3DCreate9", 1, (uint32_t[]){ 32 });
    ok("Direct3DCreate9 returns object", d3d9 != 0);

    D3DPRESENT_PARAMETERS *pp = (D3DPRESENT_PARAMETERS *)hle_alloc(sizeof(*pp));
    memset(pp, 0, sizeof(*pp));
    pp->BackBufferWidth = 320; pp->BackBufferHeight = 240;
    pp->BackBufferFormat = D3DFMT_A8R8G8B8; pp->BackBufferCount = 1;
    pp->Windowed = 1;
    uint32_t *pDev = (uint32_t *)hle_alloc(4);
    uint32_t a_dev[7] = { d3d9, 0, D3DDEVTYPE_HAL, 0x1001, 0x44, A(pp), A(pDev) };
    ok("CreateDevice hr == 0",
       call("IDirect3D9::CreateDevice", 7, a_dev) == D3D_OK && *pDev != 0);
    uint32_t dev = *pDev;
    d3d9_device_ctx_t *D = dc(dev);

    /* ---- resources that state blocks can hold references to ---- */
    uint32_t *pVB = (uint32_t *)hle_alloc(4);
    uint32_t a_vb[7] = { dev, 256, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, A(pVB), 0 };
    ok("CreateVertexBuffer", call("IDirect3DDevice9::CreateVertexBuffer", 7, a_vb) == D3D_OK);
    uint32_t vb = *pVB;
    uint32_t *pIB = (uint32_t *)hle_alloc(4);
    uint32_t a_ib[7] = { dev, 128, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, A(pIB), 0 };
    ok("CreateIndexBuffer", call("IDirect3DDevice9::CreateIndexBuffer", 7, a_ib) == D3D_OK);
    uint32_t ib = *pIB;
    uint32_t *pTex = (uint32_t *)hle_alloc(4);
    uint32_t a_tex[9] = { dev, 64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, A(pTex), 0 };
    ok("CreateTexture", call("IDirect3DDevice9::CreateTexture", 9, a_tex) == D3D_OK);
    uint32_t tex = *pTex;

    D3DVERTEXELEMENT9 *elems = (D3DVERTEXELEMENT9 *)hle_alloc(2 * sizeof(*elems));
    memset(elems, 0, 2 * sizeof(*elems));
    elems[0].Stream = 0; elems[0].Offset = 0; elems[0].Type = D3DDECLTYPE_FLOAT3;
    elems[0].Usage = 0;
    elems[1].Stream = 0xFF; elems[1].Offset = 0; elems[1].Type = D3DDECLTYPE_UNUSED;
    uint32_t *pDecl = (uint32_t *)hle_alloc(4);
    uint32_t a_decl[3] = { dev, A(elems), A(pDecl) };
    ok("CreateVertexDeclaration",
       call("IDirect3DDevice9::CreateVertexDeclaration", 3, a_decl) == D3D_OK);
    uint32_t decl = *pDecl;

    uint32_t *vstok = (uint32_t *)hle_alloc(8);
    vstok[0] = D3DVS_VERSION(3, 0); vstok[1] = 0x0000FFFFu;
    uint32_t *pVS = (uint32_t *)hle_alloc(4);
    uint32_t a_vs[3] = { dev, A(vstok), A(pVS) };
    ok("CreateVertexShader", call("IDirect3DDevice9::CreateVertexShader", 3, a_vs) == D3D_OK);
    uint32_t vs = *pVS;
    uint32_t *pstok = (uint32_t *)hle_alloc(8);
    pstok[0] = D3DPS_VERSION(3, 0); pstok[1] = 0x0000FFFFu;
    uint32_t *pPS = (uint32_t *)hle_alloc(4);
    uint32_t a_ps[3] = { dev, A(pstok), A(pPS) };
    ok("CreatePixelShader", call("IDirect3DDevice9::CreatePixelShader", 3, a_ps) == D3D_OK);
    uint32_t ps = *pPS;

    /* The default backbuffer surface is already bound by the device. */
    uint32_t rt = D->render_target;
    ok("device has a default render target", rt != 0);

    /* ---- initial state to capture ---- */
    set_rs(dev, 27, 1);           /* D3DRS_ALPHABLENDENABLE (pixel) */
    set_rs(dev, 19, 5);           /* D3DRS_SRCBLEND (pixel) */
    set_rs(dev, 20, 6);           /* D3DRS_DESTBLEND (pixel) */
    set_rs(dev, 174, 1);          /* D3DRS_SCISSORTESTENABLE (pixel) */
    set_rs(dev, 137, 1);          /* D3DRS_LIGHTING (vertex) */
    set_vp(dev, 111); set_scissor(dev, 17);
    set_tss(dev, 0, 7); set_sampler(dev, 0, 2);
    set_proj(dev, 2.5f);
    set_fvf(dev, 0x100);
    set_vsf(dev, 0, 1.0f); set_vsi(dev, 0, 11); set_vsb(dev, 0, 1);
    set_psf(dev, 0, 5.0f); set_psb(dev, 0, 1);
    set_vs(dev, vs); set_ps(dev, ps); set_decl(dev, decl);
    set_tex(dev, tex, 0);
    set_stream(dev, vb, 8, 24); set_freq(dev, 4); set_indices(dev, ib);
    set_rt(dev, rt);

    /* ---- type validation ---- */
    uint32_t *pBad = (uint32_t *)hle_alloc(4); *pBad = 0xDEADBEEFu;
    uint32_t a_bad[3] = { dev, 0, A(pBad) };
    ok("CreateStateBlock(0) is invalid",
       call("IDirect3DDevice9::CreateStateBlock", 3, a_bad) == D3DERR_INVALIDCALL);
    ok("invalid type leaves out-pointer untouched", *pBad == 0xDEADBEEFu);
    a_bad[1] = 4;
    ok("CreateStateBlock(4) is invalid",
       call("IDirect3DDevice9::CreateStateBlock", 3, a_bad) == D3DERR_INVALIDCALL);

    /* ---- ALL block captures and applies every saved state ---- */
    uint32_t all = create_sb(dev, D3DSBT_ALL);
    ok("CreateStateBlock(ALL) returns a block", all != 0);
    ok("StateBlock::GetDevice returns the device", ({
        uint32_t *pGot = (uint32_t *)hle_alloc(4); *pGot = 0;
        uint32_t a[2] = { all, A(pGot) };
        call("IDirect3DStateBlock9::GetDevice", 2, a) == D3D_OK && *pGot == dev;
    }));
    ok("capture AddRefs every held resource",
        REF(vb) == 2 && REF(ib) == 2 && REF(tex) == 2 &&
        REF(vs) == 2 && REF(ps) == 2 && REF(decl) == 2);
    ok("render target is not state-block state: not held", REF(rt) == 1);

    /* Mutate everything the block saved. */
    set_rs(dev, 27, 0); set_rs(dev, 19, 2); set_rs(dev, 20, 3);
    set_rs(dev, 174, 0); set_rs(dev, 137, 0);
    set_vp(dev, 222); set_scissor(dev, 99);
    set_tss(dev, 0, 9); set_sampler(dev, 0, 1);
    set_proj(dev, 9.5f); set_fvf(dev, 0);
    set_vsf(dev, 0, 55.0f); set_vsi(dev, 0, 99); set_vsb(dev, 0, 0);
    set_psf(dev, 0, 66.0f); set_psb(dev, 0, 0);
    set_vs(dev, 0); set_ps(dev, 0); set_decl(dev, 0);
    set_tex(dev, 0, 0); set_stream(dev, 0, 0, 0); set_freq(dev, 0);
    set_indices(dev, 0); set_rt(dev, 0);

    ok("Apply(ALL) hr == 0", apply_sb(all) == D3D_OK);
    ok("render state restored (pixel and vertex classes)",
        D->render_states[27] == 1 && D->render_states[19] == 5 &&
        D->render_states[20] == 6 && D->render_states[174] == 1 &&
        D->render_states[137] == 1);
    ok("viewport/scissor restored", D->viewport.Width == 111 && D->scissor_rect.x1 == 17);
    ok("texture stage/sampler restored",
        D->texture_stage_states[0][0] == 7 && D->sampler_states[0][0] == 2);
    ok("transform/FVF restored",
        D->transforms[2].m[0][0] == 2.5f && D->fvf == 0x100);
    ok("VS F/I/B restored",
        D->vs_const_f[0][0] == 1.0f && D->vs_const_i[0][0] == 11 && D->vs_const_b[0] == 1);
    ok("PS F/B restored",
        D->ps_const_f[0][0] == 5.0f && D->ps_const_b[0] == 1);
    ok("shaders/declaration/texture restored",
        D->vertex_shader == vs && D->pixel_shader == ps &&
        D->vertex_decl == decl && D->textures[0] == tex);
    ok("stream vb/offset/stride/freq restored",
        D->streams[0].vb == vb && D->streams[0].offset == 8 &&
        D->streams[0].stride == 24 && D->streams[0].freq == 4);
    ok("indices restored", D->indices == ib);
    ok("Apply leaves the render target alone", D->render_target == rt && REF(rt) == 1);

    ok("Apply pushed render state to backend", g_rs_pushes > 0 && g_blend == 1 &&
        g_src == 5 && g_dst == 6 && g_scissor_en == 1);
    ok("Apply pushed shader constants to backend", g_const_pushes > 0 &&
        g_vsf0 == 1.0f && g_psf0 == 5.0f);
    ok("Apply pushed sampler/render target to backend",
        g_sampler_pushes > 0 && g_rt_pushes > 0);
    ok("apply does not change held refcounts",
        REF(vb) == 2 && REF(ib) == 2 && REF(tex) == 2);

    /* ---- recapture: old references are dropped, new ones held ---- */
    set_stream(dev, 0, 0, 0); set_indices(dev, 0); set_tex(dev, 0, 0);
    ok("Capture(ALL) hr == 0", capture_sb(all) == D3D_OK);
    ok("recapture releases old vb/ib/texture references",
        REF(vb) == 1 && REF(ib) == 1 && REF(tex) == 1);
    ok("recapture kept shader/decl references",
        REF(vs) == 2 && REF(ps) == 2 && REF(decl) == 2 && REF(rt) == 1);
    ok("Release(stateblock) returns to 0", release_sb(all) == 0);
    ok("releasing block drops remaining references",
        REF(vs) == 1 && REF(ps) == 1 && REF(decl) == 1 && REF(rt) == 1);

    /* ---- PIXELSTATE captures pixel but not vertex state ---- */
    set_vsf(dev, 0, 1.0f); set_psf(dev, 0, 2.0f);
    set_rs(dev, 27, 1); set_rs(dev, 137, 1);
    uint32_t pixel = create_sb(dev, D3DSBT_PIXELSTATE);
    ok("CreateStateBlock(PIXELSTATE) returns a block", pixel != 0);
    set_vsf(dev, 0, 10.0f); set_psf(dev, 0, 20.0f);
    set_rs(dev, 27, 0); set_rs(dev, 137, 0);
    ok("Apply(PIXELSTATE) hr == 0", apply_sb(pixel) == D3D_OK);
    ok("PIXEL block restored PS constant", D->ps_const_f[0][0] == 2.0f);
    ok("PIXEL block did not restore VS constant", D->vs_const_f[0][0] == 10.0f);
    ok("PIXEL block restored pixel render state", D->render_states[27] == 1);
    ok("PIXEL block did not restore vertex render state", D->render_states[137] == 0);
    release_sb(pixel);

    /* ---- VERTEXSTATE captures vertex but not pixel state ---- */
    set_vsf(dev, 0, 3.0f); set_psf(dev, 0, 4.0f);
    set_stream(dev, vb, 4, 20); set_freq(dev, 7); set_indices(dev, ib);
    uint32_t vertex = create_sb(dev, D3DSBT_VERTEXSTATE);
    ok("CreateStateBlock(VERTEXSTATE) returns a block", vertex != 0);
    ok("VERTEX block holds stream/index references", REF(vb) == 2 && REF(ib) == 2);
    set_vsf(dev, 0, 30.0f); set_psf(dev, 0, 40.0f);
    set_stream(dev, 0, 0, 0); set_freq(dev, 0); set_indices(dev, 0);
    ok("Apply(VERTEXSTATE) hr == 0", apply_sb(vertex) == D3D_OK);
    ok("VERTEX block restored VS constant", D->vs_const_f[0][0] == 3.0f);
    ok("VERTEX block did not restore PS constant", D->ps_const_f[0][0] == 40.0f);
    ok("VERTEX block restored stream/freq/indices",
        D->streams[0].vb == vb && D->streams[0].offset == 4 &&
        D->streams[0].stride == 20 && D->streams[0].freq == 7 &&
        D->indices == ib);
    release_sb(vertex);
    ok("releasing VERTEX block drops stream/index references",
        REF(vb) == 1 && REF(ib) == 1);

    /* ---- Begin/EndStateBlock ---- */
    set_rs(dev, 27, 0);
    ok("BeginStateBlock hr == 0",
       call("IDirect3DDevice9::BeginStateBlock", 1, (uint32_t[]){ dev }) == D3D_OK);
    ok("nested BeginStateBlock fails",
       call("IDirect3DDevice9::BeginStateBlock", 1, (uint32_t[]){ dev }) == D3DERR_INVALIDCALL);
    ok("CreateStateBlock while recording fails", create_sb(dev, D3DSBT_ALL) == 0);
    set_rs(dev, 27, 1);
    uint32_t *pRec = (uint32_t *)hle_alloc(4); *pRec = 0;
    uint32_t a_end[2] = { dev, A(pRec) };
    ok("EndStateBlock hr == 0",
       call("IDirect3DDevice9::EndStateBlock", 2, a_end) == D3D_OK && *pRec != 0);
    ok("EndStateBlock without Begin fails",
       call("IDirect3DDevice9::EndStateBlock", 2, a_end) == D3DERR_INVALIDCALL);
    ok("End puts the device back as it was before Begin", D->render_states[27] == 0);
    ok("Apply(recorded block) sets the recorded state", apply_sb(*pRec) == D3D_OK &&
        D->render_states[27] == 1);
    ok("Capture/Apply while recording are rejected", ({
        call("IDirect3DDevice9::BeginStateBlock", 1, (uint32_t[]){ dev }) == D3D_OK &&
        capture_sb(*pRec) == D3DERR_INVALIDCALL &&
        apply_sb(*pRec) == D3DERR_INVALIDCALL &&
        call("IDirect3DDevice9::EndStateBlock", 2, a_end) == D3D_OK;
    }));
    release_sb(*pRec);


    /* ================= new: COM contract of the touched interfaces ================= */
    {
        /* QueryInterface of IDirect3D9, IDirect3DDevice9, IDirect3DStateBlock9: exactly one
         * RET with 3 args (esp delta 0), refcount + 1, out = self. A nested call of
         * hle_com_AddRef() (own RET argc 1) would pop 8 bytes too many. */
        uint32_t qsb = create_sb(dev, D3DSBT_ALL);
        uint32_t objs[3] = { d3d9, dev, qsb };
        const char *nm[3] = { "IDirect3D9::QueryInterface", "IDirect3DDevice9::QueryInterface",
                              "IDirect3DStateBlock9::QueryInterface" };
        for (int i = 0; i < 3; i++) {
            uint32_t *pOut = (uint32_t *)hle_alloc(4); *pOut = 0;
            uint32_t ref0 = REF(objs[i]);
            uint32_t eax = 0xFFFFFFFFu;
            check_slot(nm[i], objs[i], 0, 3, (uint32_t[]){ objs[i], iid_unk(), A(pOut) }, &eax);
            char msg[128];
            snprintf(msg, sizeof msg, "%s: S_OK and out = self", nm[i]);
            ok(msg, eax == D3D_OK && *pOut == objs[i]);
            snprintf(msg, sizeof msg, "%s: refcount + 1", nm[i]);
            ok(msg, REF(objs[i]) == ref0 + 1);
            REF(objs[i]) = ref0;
            eax = 0;
            check_slot(nm[i], objs[i], 0, 3, (uint32_t[]){ objs[i], 0, 0 }, &eax);
            snprintf(msg, sizeof msg, "%s: NULL out-pointer is D3DERR_INVALIDCALL, refcount unchanged", nm[i]);
            ok(msg, eax == D3DERR_INVALIDCALL && REF(objs[i]) == ref0);
        }
        release_sb(qsb);
    }
    {
        /* Every IDirect3DStateBlock9 slot, entered directly through the guest vtable.
         * Order and signatures: public d3d9.h (IDirect3DStateBlock9 : IUnknown with
         * GetDevice, Capture, Apply; Wine include/d3d9.h gives the same order). */
        static const char *slot_name[6] = {
            "IDirect3DStateBlock9::QueryInterface", "IDirect3DStateBlock9::AddRef",
            "IDirect3DStateBlock9::Release", "IDirect3DStateBlock9::GetDevice",
            "IDirect3DStateBlock9::Capture", "IDirect3DStateBlock9::Apply" };
        static const int nargs[6] = { 3, 1, 1, 2, 1, 1 };   /* stdcall argc including this */
        uint32_t blk = create_sb(dev, D3DSBT_ALL);
        uint32_t vt = MEM32(blk);
        for (int i = 0; i < 6; i++) {
            char msg[96];
            snprintf(msg, sizeof msg, "StateBlock9 vtable slot %d is %s", i, slot_name[i]);
            ok(msg, MEM32(vt + 4u * (uint32_t)i) == hle_resolve(slot_name[i]));
        }
        ok("StateBlock9 vtable has exactly 6 slots (IUnknown 3 + GetDevice, Capture, Apply)",
           MEM32(vt + 4u * 6u) == 0);
        uint32_t *pOut = (uint32_t *)hle_alloc(4); *pOut = 0;
        uint32_t args[6][3] = { { blk, iid_unk(), A(pOut) }, { blk }, { blk }, { blk, A(pOut) }, { blk }, { blk } };
        /* QI: ref 2 (out = blk); AddRef: 3; Release: 2; the others: S_OK. */
        uint32_t want[6] = { D3D_OK, 3, 2, D3D_OK, D3D_OK, D3D_OK };
        for (int i = 0; i < 6; i++) {
            uint32_t eax = 0xFFFFFFFFu;
            check_slot(slot_name[i], blk, i, nargs[i], args[i], &eax);
            char msg[96];
            snprintf(msg, sizeof msg, "%s: result %u", slot_name[i], (unsigned)want[i]);
            ok(msg, eax == want[i]);
        }
        ok("after QI + AddRef + Release the block holds 2 references", REF(blk) == 2);
        REF(blk) = 1;
        release_sb(blk);
    }

    /* ================= new: state groups of the D3DSBT_* types ================= */
    {
        set_rs(dev, 14, 1); set_rs(dev, 15, 1); set_rs(dev, 23, 3);   /* ZWRITE, ALPHATEST, ZFUNC: pixel */
        set_rs(dev, 22, 2); set_rs(dev, 28, 1);                       /* CULLMODE, FOGENABLE: vertex */
        set_tss(dev, 1, 5);  set_tss(dev, 11, 6);                     /* COLOROP: pixel, TEXCOORDINDEX: vertex */
        set_sampler(dev, 1, 4); set_sampler(dev, 13, 8);              /* ADDRESSU: pixel, DMAPOFFSET: vertex */
        uint32_t pb = create_sb(dev, D3DSBT_PIXELSTATE);
        uint32_t vb_ = create_sb(dev, D3DSBT_VERTEXSTATE);
        set_rs(dev, 14, 0); set_rs(dev, 15, 0); set_rs(dev, 23, 0); set_rs(dev, 22, 0); set_rs(dev, 28, 0);
        set_tss(dev, 1, 0); set_tss(dev, 11, 0); set_sampler(dev, 1, 0); set_sampler(dev, 13, 0);
        apply_sb(pb);
        ok("PIXEL block restores ZWRITEENABLE/ALPHATESTENABLE/ZFUNC",
           D->render_states[14] == 1 && D->render_states[15] == 1 && D->render_states[23] == 3);
        ok("PIXEL block leaves CULLMODE/FOGENABLE alone", D->render_states[22] == 0 && D->render_states[28] == 0);
        ok("PIXEL block: COLOROP and sampler ADDRESSU restored, TEXCOORDINDEX and DMAPOFFSET not",
           D->texture_stage_states[0][1] == 5 && D->texture_stage_states[0][11] == 0 &&
           D->sampler_states[0][1] == 4 && D->sampler_states[0][13] == 0);
        set_rs(dev, 14, 0); set_rs(dev, 15, 0); set_rs(dev, 23, 0);
        apply_sb(vb_);
        ok("VERTEX block restores CULLMODE/FOGENABLE", D->render_states[22] == 2 && D->render_states[28] == 1);
        ok("VERTEX block leaves ZWRITEENABLE/ALPHATESTENABLE/ZFUNC alone",
           D->render_states[14] == 0 && D->render_states[15] == 0 && D->render_states[23] == 0);
        ok("VERTEX block: TEXCOORDINDEX and DMAPOFFSET restored",
           D->texture_stage_states[0][11] == 6 && D->sampler_states[0][13] == 8);
        release_sb(pb); release_sb(vb_);
    }
    {
        /* World/view/projection, material, light, clip plane, palette, viewport:
         * captured by ALL, the first four by VERTEX, none by PIXEL. */
        D3DMATRIX *w = (D3DMATRIX *)hle_alloc(sizeof(*w)); memset(w, 0, sizeof(*w)); w->m[0][0] = 4.0f;
        call("IDirect3DDevice9::SetTransform", 3, (uint32_t[]){ dev, 256 /* WORLD */, A(w) });
        uint8_t *mat = (uint8_t *)hle_alloc(D3D9_MATERIAL_BYTES); memset(mat, 0x5A, D3D9_MATERIAL_BYTES);
        call("IDirect3DDevice9::SetMaterial", 2, (uint32_t[]){ dev, A(mat) });
        uint8_t *lt = (uint8_t *)hle_alloc(D3D9_LIGHT_BYTES); memset(lt, 0x33, D3D9_LIGHT_BYTES);
        call("IDirect3DDevice9::SetLight", 3, (uint32_t[]){ dev, 2, A(lt) });
        call("IDirect3DDevice9::LightEnable", 3, (uint32_t[]){ dev, 2, 1 });
        float *cp = (float *)hle_alloc(16); cp[0] = 1; cp[1] = 2; cp[2] = 3; cp[3] = 4;
        call("IDirect3DDevice9::SetClipPlane", 3, (uint32_t[]){ dev, 1, A(cp) });
        uint8_t *pal = (uint8_t *)hle_alloc(D3D9_PALETTE_ENTRIES * 4); memset(pal, 0x77, D3D9_PALETTE_ENTRIES * 4);
        call("IDirect3DDevice9::SetPaletteEntries", 3, (uint32_t[]){ dev, 1, A(pal) });
        set_vp(dev, 133);
        uint32_t ball = create_sb(dev, D3DSBT_ALL);
        uint32_t bvtx = create_sb(dev, D3DSBT_VERTEXSTATE);
        uint32_t bpix = create_sb(dev, D3DSBT_PIXELSTATE);
        D3DMATRIX *z = (D3DMATRIX *)hle_alloc(sizeof(*z)); memset(z, 0, sizeof(*z)); z->m[0][0] = 9.0f;
        call("IDirect3DDevice9::SetTransform", 3, (uint32_t[]){ dev, 256, A(z) });
        memset(mat, 0, D3D9_MATERIAL_BYTES); call("IDirect3DDevice9::SetMaterial", 2, (uint32_t[]){ dev, A(mat) });
        memset(lt, 0, D3D9_LIGHT_BYTES); call("IDirect3DDevice9::SetLight", 3, (uint32_t[]){ dev, 2, A(lt) });
        call("IDirect3DDevice9::LightEnable", 3, (uint32_t[]){ dev, 2, 0 });
        memset(cp, 0, 16); call("IDirect3DDevice9::SetClipPlane", 3, (uint32_t[]){ dev, 1, A(cp) });
        memset(pal, 0, D3D9_PALETTE_ENTRIES * 4);
        call("IDirect3DDevice9::SetPaletteEntries", 3, (uint32_t[]){ dev, 1, A(pal) });
        set_vp(dev, 144);

        apply_sb(bpix);
        ok("PIXEL block does not touch world/material/light/clip/palette/viewport",
           D->transforms[256].m[0][0] == 9.0f && D->material[0] == 0 && D->lights[2][0] == 0 &&
           D->light_enable[2] == 0 && D->clip_planes[1][3] == 0 && D->palette[1][0] == 0 &&
           D->viewport.Width == 144);
        apply_sb(bvtx);
        ok("VERTEX block restores the WORLD transform (index 256)", D->transforms[256].m[0][0] == 4.0f);
        ok("VERTEX block restores material, light, light enable, clip plane",
           D->material[0] == 0x5A && D->material[D3D9_MATERIAL_BYTES - 1] == 0x5A &&
           D->lights[2][0] == 0x33 && D->lights[2][D3D9_LIGHT_BYTES - 1] == 0x33 &&
           D->light_enable[2] == 1 && D->clip_planes[1][2] == 3.0f);
        ok("VERTEX block leaves palette and viewport alone", D->palette[1][0] == 0 && D->viewport.Width == 144);
        call("IDirect3DDevice9::SetTransform", 3, (uint32_t[]){ dev, 256, A(z) });
        apply_sb(ball);
        ok("ALL block restores WORLD, palette and viewport",
           D->transforms[256].m[0][0] == 4.0f && D->palette[1][0] == 0x77 && D->palette[1][1023] == 0x77 &&
           D->viewport.Width == 133);
        release_sb(ball); release_sb(bvtx); release_sb(bpix);
    }
    {
        /* MultiplyTransform: current * m, row-vector convention; identity if never set. */
        D3DMATRIX *a = (D3DMATRIX *)hle_alloc(sizeof(*a)); memset(a, 0, sizeof(*a));
        a->m[0][0] = 2.0f; a->m[0][1] = 5.0f; a->m[1][1] = 1.0f; a->m[2][2] = 1.0f; a->m[3][3] = 1.0f;
        D3DMATRIX *b = (D3DMATRIX *)hle_alloc(sizeof(*b)); memset(b, 0, sizeof(*b));
        b->m[0][0] = 3.0f; b->m[1][0] = 7.0f; b->m[1][1] = 1.0f; b->m[2][2] = 1.0f; b->m[3][3] = 1.0f;
        call("IDirect3DDevice9::SetTransform", 3, (uint32_t[]){ dev, 258, A(a) });
        ok("MultiplyTransform hr == 0",
           call("IDirect3DDevice9::MultiplyTransform", 3, (uint32_t[]){ dev, 258, A(b) }) == D3D_OK);
        /* (a*b)[0][0] = 2*3 + 5*7 = 41, [0][1] = 2*0 + 5*1 = 5 */
        ok("MultiplyTransform = current * m", D->transforms[258].m[0][0] == 41.0f && D->transforms[258].m[0][1] == 5.0f);
        ok("MultiplyTransform on a never-set transform multiplies the identity",
           call("IDirect3DDevice9::MultiplyTransform", 3, (uint32_t[]){ dev, 300, A(b) }) == D3D_OK &&
           D->transforms[300].m[0][0] == 3.0f && D->transforms[300].m[1][0] == 7.0f);
        ok("MultiplyTransform rejects NULL and out-of-range state",
           call("IDirect3DDevice9::MultiplyTransform", 3, (uint32_t[]){ dev, 258, 0 }) == D3DERR_INVALIDCALL &&
           call("IDirect3DDevice9::MultiplyTransform", 3, (uint32_t[]){ dev, 512, A(b) }) == D3DERR_INVALIDCALL);
    }

    /* ================= new: Begin/EndStateBlock records exactly what was set ================= */
    {
        set_rs(dev, 27, 0); set_rs(dev, 19, 2); set_rs(dev, 20, 4);
        set_vsf(dev, 3, 1.0f); set_psf(dev, 2, 1.0f); set_fvf(dev, 0x42); set_tex(dev, 0, 1);
        uint32_t tex_ref0 = REF(tex);
        ok("BeginStateBlock", call("IDirect3DDevice9::BeginStateBlock", 1, (uint32_t[]){ dev }) == D3D_OK);
        set_rs(dev, 27, 1);                 /* recorded */
        set_rs(dev, 20, 4);                 /* recorded although the value does not change */
        set_vsf(dev, 3, 77.0f);             /* recorded */
        set_tex(dev, tex, 1);               /* recorded, resource */
        uint32_t *pR = (uint32_t *)hle_alloc(4); *pR = 0;
        ok("EndStateBlock returns the block",
           call("IDirect3DDevice9::EndStateBlock", 2, (uint32_t[]){ dev, A(pR) }) == D3D_OK && *pR != 0);
        uint32_t rec = *pR;
        ok("nothing set while recording stays applied to the device",
           D->render_states[27] == 0 && D->vs_const_f[3][0] == 1.0f && D->textures[1] == 0);
        ok("recorded block holds a reference to the recorded texture only", REF(tex) == tex_ref0 + 1);

        set_rs(dev, 27, 0); set_rs(dev, 19, 9); set_rs(dev, 20, 8);
        set_vsf(dev, 3, 5.0f); set_psf(dev, 2, 9.0f); set_fvf(dev, 0x99);
        ok("Apply(recorded) hr == 0", apply_sb(rec) == D3D_OK);
        ok("Apply sets the recorded states (also the unchanged-value one)",
           D->render_states[27] == 1 && D->render_states[20] == 4 &&
           D->vs_const_f[3][0] == 77.0f && D->textures[1] == tex);
        ok("Apply leaves every unrecorded state alone",
           D->render_states[19] == 9 && D->ps_const_f[2][0] == 9.0f && D->fvf == 0x99 &&
           D->vs_const_f[4][0] == 0.0f);

        /* Capture re-reads only the recorded states. */
        set_rs(dev, 27, 6); set_rs(dev, 19, 7);
        ok("Capture(recorded) hr == 0", capture_sb(rec) == D3D_OK);
        set_rs(dev, 27, 0); set_rs(dev, 19, 0);
        apply_sb(rec);
        ok("Capture updated the recorded state, not the others", D->render_states[27] == 6 && D->render_states[19] == 0);
        release_sb(rec);
        ok("releasing the recorded block drops its texture reference", REF(tex) == tex_ref0);
        set_tex(dev, 0, 1);

        /* An empty recording is a valid, empty block. */
        call("IDirect3DDevice9::BeginStateBlock", 1, (uint32_t[]){ dev });
        *pR = 0;
        ok("empty recording yields a block",
           call("IDirect3DDevice9::EndStateBlock", 2, (uint32_t[]){ dev, A(pR) }) == D3D_OK && *pR != 0);
        set_rs(dev, 27, 3);
        apply_sb(*pR);
        ok("applying an empty block changes nothing", D->render_states[27] == 3);
        release_sb(*pR);

        /* EndStateBlock with a NULL out-pointer ends the recording and is an error. */
        call("IDirect3DDevice9::BeginStateBlock", 1, (uint32_t[]){ dev });
        set_rs(dev, 27, 8);
        ok("EndStateBlock(NULL) is invalid",
           call("IDirect3DDevice9::EndStateBlock", 2, (uint32_t[]){ dev, 0 }) == D3DERR_INVALIDCALL);
        ok("... and the device is no longer recording and was restored",
           D->recording_state_block == 0 && D->render_states[27] == 3 &&
           call("IDirect3DDevice9::BeginStateBlock", 1, (uint32_t[]){ dev }) == D3D_OK);
        *pR = 0;
        call("IDirect3DDevice9::EndStateBlock", 2, (uint32_t[]){ dev, A(pR) });
        release_sb(*pR);
    }

    /* ---- cleanup (each resource is back to its single creation reference) ---- */
    uint32_t a_rel[1];
    a_rel[0] = vs; call("IDirect3DVertexShader9::Release", 1, a_rel);
    a_rel[0] = ps; call("IDirect3DPixelShader9::Release", 1, a_rel);
    a_rel[0] = decl; call("IDirect3DVertexDeclaration9::Release", 1, a_rel);
    a_rel[0] = tex; call("IDirect3DTexture9::Release", 1, a_rel);
    a_rel[0] = vb; call("IDirect3DVertexBuffer9::Release", 1, a_rel);
    a_rel[0] = ib; call("IDirect3DIndexBuffer9::Release", 1, a_rel);
    a_rel[0] = rt; call("IDirect3DSurface9::Release", 1, a_rel);
    hle_d3d9_set_gpu(prev_gpu);

    if (!fails) printf("OK d3d9_stateblock_selftest\n");
    return fails ? 1 : 0;
}
