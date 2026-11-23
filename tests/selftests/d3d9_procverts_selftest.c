/*
 * d3d9_procverts_selftest.c - IDirect3DDevice9::ProcessVertices.
 *
 * Fixed-function path: stream 0 (FVF XYZ|DIFFUSE) is transformed by
 * World*View*Projection into a pre-transformed XYZRHW|DIFFUSE buffer. The
 * expected values are computed by hand from the public viewport formula
 * (X = (x/w*0.5+0.5)*Width, Y = (1-(y/w*0.5+0.5))*Height, Z = z/w, RHW = 1/w).
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
    "procverts-mock", mock_active, mock_enable, mock_init, mock_shutdown,
    mock_begin, mock_end, mock_clear, mock_present,
    mock_tex, mock_tex_destroy, mock_tex_upload,
    mock_buf, mock_buf_destroy, mock_buf_upload,
    mock_pipe, mock_pipe_destroy, mock_draw,
    mock_set_rt, mock_create_rt, mock_destroy_rt, mock_set_tex,
    NULL, NULL, mock_draw_indexed, mock_set_rs,
    mock_set_decl, mock_set_consts, mock_set_sampler
};

const recomp_dispatch_entry_t recomp_dispatch_table[1];
const uint32_t recomp_dispatch_count = 0;

#define FVF_XYZ_DIFFUSE      0x042u
#define FVF_XYZRHW_DIFFUSE   0x044u

static int near(float a, float b) { return a - b < 0.001f && b - a < 0.001f; }

static void identity(D3DMATRIX *m) {
    memset(m, 0, sizeof *m);
    for (int i = 0; i < 4; i++) m->m[i][i] = 1.0f;
}

static uint32_t make_vb(uint32_t dev, uint32_t size, uint32_t fvf) {
    uint32_t *p = (uint32_t *)hle_alloc(4);
    uint32_t a[7] = { dev, size, 0, fvf, D3DPOOL_MANAGED, A(p), 0 };
    ok("CreateVertexBuffer", call("IDirect3DDevice9::CreateVertexBuffer", 7, a) == D3D_OK && *p);
    return *p;
}

int main(void) {
    recomp_host_init();
    win32hle_register(win32hle_d3d9);
    const d3d9_gpu_ops_t *prev_gpu = hle_d3d9_gpu;
    hle_d3d9_set_gpu(&g_mock_gpu);

    uint32_t d3d9 = call("Direct3DCreate9", 1, (uint32_t[]){ 32 });
    D3DPRESENT_PARAMETERS *pp = (D3DPRESENT_PARAMETERS *)hle_alloc(sizeof(*pp));
    memset(pp, 0, sizeof(*pp));
    pp->BackBufferWidth = 320; pp->BackBufferHeight = 240;
    pp->BackBufferFormat = D3DFMT_A8R8G8B8; pp->BackBufferCount = 1;
    pp->Windowed = 1;
    uint32_t *pDev = (uint32_t *)hle_alloc(4);
    uint32_t a_dev[7] = { d3d9, 0, D3DDEVTYPE_HAL, 0x1001, 0x44, A(pp), A(pDev) };
    ok("CreateDevice", call("IDirect3D9::CreateDevice", 7, a_dev) == D3D_OK && *pDev);
    uint32_t dev = *pDev;
    d3d9_device_ctx_t *D = (d3d9_device_ctx_t *)OBJ_CTX(dev);
    identity(&D->transforms[256]); identity(&D->transforms[2]); identity(&D->transforms[3]);
    D->viewport.X = 0; D->viewport.Y = 0; D->viewport.Width = 320; D->viewport.Height = 240;
    D->viewport.MinZ = 0.0f; D->viewport.MaxZ = 1.0f;

    uint32_t svb = make_vb(dev, 3 * 16, FVF_XYZ_DIFFUSE);
    uint32_t dvb = make_vb(dev, 4 * 20, FVF_XYZRHW_DIFFUSE);
    d3d9_vb_ctx_t *S = (d3d9_vb_ctx_t *)OBJ_CTX(svb), *T = (d3d9_vb_ctx_t *)OBJ_CTX(dvb);
    struct { float x, y, z; uint32_t c; } in[3] = {
        { 0.0f, 0.0f, 0.5f, 0xFF112233u }, { 1.0f, 1.0f, 0.0f, 0xFF445566u }, { -1.0f, -1.0f, 1.0f, 0xFF778899u } };
    memcpy(S->data, in, sizeof in);

    ok("SetFVF", call("IDirect3DDevice9::SetFVF", 2, (uint32_t[]){ dev, FVF_XYZ_DIFFUSE }) == D3D_OK);
    uint32_t a_pv[7] = { dev, 0, 0, 3, dvb, 0, 0 };

    /* no stream bound */
    ok("no source stream -> INVALIDCALL", call("IDirect3DDevice9::ProcessVertices", 7, a_pv) == D3DERR_INVALIDCALL);
    ok("SetStreamSource", call("IDirect3DDevice9::SetStreamSource", 5, (uint32_t[]){ dev, 0, svb, 0, 16 }) == D3D_OK);

    memset(T->data, 0xAB, 4 * 20);
    ok("ProcessVertices hr", call("IDirect3DDevice9::ProcessVertices", 7, a_pv) == D3D_OK);
    struct { float x, y, z, rhw; uint32_t c; } out[4];
    memcpy(out, T->data, sizeof out);
    ok("v0 screen x", near(out[0].x, 160.0f));
    ok("v0 screen y", near(out[0].y, 120.0f));
    ok("v0 z", near(out[0].z, 0.5f));
    ok("v0 rhw", near(out[0].rhw, 1.0f));
    ok("v0 colour copied", out[0].c == 0xFF112233u);
    ok("v1 screen x/y", near(out[1].x, 320.0f) && near(out[1].y, 0.0f));
    ok("v1 colour copied", out[1].c == 0xFF445566u);
    ok("v2 screen x/y/z", near(out[2].x, 0.0f) && near(out[2].y, 240.0f) && near(out[2].z, 1.0f));
    ok("v2 colour copied", out[2].c == 0xFF778899u);
    ok("untouched vertex 3 keeps its bytes", ((uint8_t *)T->data)[60] == 0xAB);

    /* w != 1: scale w by 2 through the projection matrix (m[3][3] = 2) */
    D->transforms[3].m[3][3] = 2.0f;
    ok("ProcessVertices with w=2", call("IDirect3DDevice9::ProcessVertices", 7, a_pv) == D3D_OK);
    memcpy(out, T->data, sizeof out);
    ok("w=2: v1 rhw 0.5", near(out[1].rhw, 0.5f));
    ok("w=2: v1 screen x = (0.5*0.5+0.5)*320 = 240", near(out[1].x, 240.0f));
    identity(&D->transforms[3]);

    /* SrcStartIndex and DestIndex */
    uint32_t a_off[7] = { dev, 1, 2, 2, dvb, 0, 0 };
    ok("SrcStart 1, DestIndex 2, count 2", call("IDirect3DDevice9::ProcessVertices", 7, a_off) == D3D_OK);
    memcpy(out, T->data, sizeof out);
    ok("dest[2] is source vertex 1", near(out[2].x, 320.0f) && out[2].c == 0xFF445566u);
    ok("dest[3] is source vertex 2", near(out[3].x, 0.0f) && out[3].c == 0xFF778899u);

    /* edge cases */
    uint32_t a_zero[7] = { dev, 0, 0, 0, dvb, 0, 0 };
    ok("count 0 -> OK", call("IDirect3DDevice9::ProcessVertices", 7, a_zero) == D3D_OK);
    uint32_t a_src_over[7] = { dev, 2, 0, 2, dvb, 0, 0 };
    ok("source range past buffer -> INVALIDCALL", call("IDirect3DDevice9::ProcessVertices", 7, a_src_over) == D3DERR_INVALIDCALL);
    uint32_t a_dst_over[7] = { dev, 0, 2, 3, dvb, 0, 0 };
    ok("destination range past buffer -> INVALIDCALL", call("IDirect3DDevice9::ProcessVertices", 7, a_dst_over) == D3DERR_INVALIDCALL);
    uint32_t a_nodst[7] = { dev, 0, 0, 1, 0, 0, 0 };
    ok("NULL destination -> INVALIDCALL", call("IDirect3DDevice9::ProcessVertices", 7, a_nodst) == D3DERR_INVALIDCALL);
    ok("stride below vertex size -> INVALIDCALL",
       call("IDirect3DDevice9::SetStreamSource", 5, (uint32_t[]){ dev, 0, svb, 0, 8 }) == D3D_OK &&
       call("IDirect3DDevice9::ProcessVertices", 7, a_pv) == D3DERR_INVALIDCALL);
    call("IDirect3DDevice9::SetStreamSource", 5, (uint32_t[]){ dev, 0, svb, 0, 16 });
    D->vertex_shader = 1;
    ok("bound vertex shader is reported, not faked", call("IDirect3DDevice9::ProcessVertices", 7, a_pv) == D3DERR_NOTAVAILABLE);
    D->vertex_shader = 0;

    hle_d3d9_set_gpu(prev_gpu);
    if (!fails) printf("OK d3d9_procverts_selftest\n");
    return fails ? 1 : 0;
}
