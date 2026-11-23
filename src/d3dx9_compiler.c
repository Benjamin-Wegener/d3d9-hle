/*
 * d3dx9_compiler.c - D3DXCompileShader / CompileShaderFromFileA / PreprocessShader.
 *
 * Uses the optional vkd3d-shader bridge (d3dx9_hlsl.c). No vkd3d code or binary
 * is shipped. If the library is unavailable, returns D3DXERR_NOTAVAILABLE and logs.
 * ID3DXBuffer and a minimal ID3DXConstantTable are created as guest COM objects
 * with 32-bit guest pointers. All stdcall ABIs match public D3DX9 signatures.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32hle.h"
#include "d3dx9_buffer.h"
void d3d_QueryInterface(void);
#include "d3dx9_hlsl.h"
#include "d3dx9_fxtext.h"
#include "d3dx9_effect_hle.h"

#define D3D_OK                  0x00000000u
#define D3DERR_INVALIDCALL      0x8876086Cu
#define D3DXERR_NOTAVAILABLE    0x88760866u
#define E_FAIL                  0x80004005u
#define E_OUTOFMEMORY           0x8007000Eu

static uint32_t make_dxbuf(const void *data, size_t size) { return d3dx9_buffer_new(data, size); }

/* The constant table of compiled shader bytecode: the one ID3DXConstantTable
 * implementation (d3dx9_consttable.c). The table reads the shader's tokens for
 * its lifetime, so they get their own guest copy. */
uint32_t d3dx9_consttable_from_shader(uint32_t pf, uint32_t *hr);
static uint32_t make_ct_from_bytecode(const void *code, size_t size) {
    uint32_t hr;
    if (!code || size < 8) return 0;
    void *g = hle_alloc((uint32_t)size);
    if (!g) return 0;
    memcpy(g, code, size);
    uint32_t ct = d3dx9_consttable_from_shader(hle_va(g), &hr);
    if (!ct) hle_free(g);
    return ct;
}

/* ---- D3DXMACRO / ID3DXInclude adapters ---- */

typedef struct {
    uint32_t include_obj; /* guest ID3DXInclude* */
} include_ctx_t;

static int include_open(const char *filename, int local, const char *parent_data,
                        void *ctx, const void **data, size_t *size) {
    include_ctx_t *ic = (include_ctx_t *)ctx;
    uint32_t obj = ic ? ic->include_obj : 0u;
    if (!obj) return E_FAIL;
    uint32_t vt = MEM32(obj);
    uint32_t fn_open = MEM32(vt + 3u * 4u); /* Open is 4th method: QI,AddRef,Release,Open */
    /* alloc guest strings/buffers */
    uint32_t g_fname = hle_guest_strdup(filename ? filename : "");
    uint32_t g_ppdata = hle_va(hle_alloc(4));
    uint32_t g_pbytes = hle_va(hle_alloc(4));
    MEM32(g_ppdata) = 0; MEM32(g_pbytes) = 0;
    uint32_t args[6] = { obj, (uint32_t)(local ? 0u : 1u), g_fname, 0u, g_ppdata, g_pbytes };
    (void)parent_data;
    uint32_t hr = hle_call_guest(fn_open, 6, args);
    if (hr != 0) {
        hle_free(hle_ptr(g_fname)); hle_free(hle_ptr(g_ppdata)); hle_free(hle_ptr(g_pbytes));
        return (int)hr;
    }
    uint32_t d_va = MEM32(g_ppdata);
    uint32_t b = MEM32(g_pbytes);
    hle_free(hle_ptr(g_fname)); hle_free(hle_ptr(g_ppdata)); hle_free(hle_ptr(g_pbytes));
    if (!d_va) return E_FAIL;
    *data = hle_ptr(d_va);
    *size = b;
    return 0;
}

static void include_close(const void *data, size_t size, void *ctx) {
    include_ctx_t *ic = (include_ctx_t *)ctx;
    uint32_t obj = ic ? ic->include_obj : 0u;
    (void)size;
    if (!obj || !data) return;
    uint32_t vt = MEM32(obj);
    uint32_t fn_close = MEM32(vt + 4u * 4u); /* Close is 5th method */
    uint32_t args[2] = { obj, hle_va((void *)data) };
    hle_call_guest(fn_close, 2, args);
}

static hle_vkd3d_macro *build_macros(uint32_t macros_va, unsigned *out_count) {
    *out_count = 0;
    if (!macros_va) return NULL;
    /* count */
    unsigned n = 0;
    for (unsigned i = 0; i < 256; i++) {
        uint32_t name_va = MEM32(macros_va + i * 8u);
        if (!name_va) break;
        n++;
    }
    if (!n) return NULL;
    hle_vkd3d_macro *m = (hle_vkd3d_macro *)calloc(n, sizeof *m);
    if (!m) return NULL;
    for (unsigned i = 0; i < n; i++) {
        uint32_t name_va = MEM32(macros_va + i * 8u);
        uint32_t def_va = MEM32(macros_va + i * 8u + 4u);
        m[i].name = name_va ? (const char *)hle_ptr(name_va) : NULL;
        m[i].definition = def_va ? (const char *)hle_ptr(def_va) : NULL;
    }
    *out_count = n;
    return m;
}

/* ---- common compile/preprocess result handling ---- */

static void set_out_buf(uint32_t pp, uint32_t obj) {
    if (pp) MEM32(pp) = obj;
}

static int fail_unavailable(uint32_t ppShader, uint32_t ppErr, uint32_t ppCT, int nargs) {
    fprintf(stderr, "[d3dx9] D3DXCompileShader/Preprocess: vkd3d-shader unavailable\n");
    set_out_buf(ppShader, 0); set_out_buf(ppErr, 0); set_out_buf(ppCT, 0);
    return (int)D3DXERR_NOTAVAILABLE;
}

static int do_compile_common(const char *src, size_t src_len, const char *src_name,
                             uint32_t macros_va, uint32_t include_va,
                             const char *entry, const char *profile, uint32_t flags,
                             uint32_t ppShader, uint32_t ppErr, uint32_t ppCT, int nargs) {
    set_out_buf(ppShader, 0); set_out_buf(ppErr, 0); set_out_buf(ppCT, 0);
    if (!src || !entry || !profile) return (int)D3DERR_INVALIDCALL;
    if (!hle_vkd3d_available()) return fail_unavailable(ppShader, ppErr, ppCT, nargs);

    unsigned nmac = 0;
    hle_vkd3d_macro *macros = build_macros(macros_va, &nmac);
    include_ctx_t ic = { include_va };
    hle_vkd3d_code out = {0};
    char *messages = NULL;
    int rc = hle_vkd3d_compile_hlsl(src, src_len, src_name, entry, profile,
                                    macros, nmac,
                                    include_va ? include_open : NULL,
                                    include_va ? include_close : NULL,
                                    include_va ? &ic : NULL,
                                    flags, &out, &messages);
    free(macros);

    if (messages) {
        set_out_buf(ppErr, make_dxbuf(messages, strlen(messages) + 1));
        hle_vkd3d_free_messages(messages);
    }
    if (rc != HLE_VKD3D_OK) {
        hle_vkd3d_free_code(&out);
        fprintf(stderr, "[d3dx9] compile failed (%d) for %s/%s\n", rc, entry, profile);
        return (rc == HLE_VKD3D_ERR_UNAVAILABLE) ? (int)D3DXERR_NOTAVAILABLE : (int)E_FAIL;
    }
    set_out_buf(ppShader, make_dxbuf(out.code, out.size));
    if (ppCT) set_out_buf(ppCT, make_ct_from_bytecode(out.code, out.size));
    hle_vkd3d_free_code(&out);
    return D3D_OK;
}

/* Public compile entry: one HLSL source with entry point and profile to legacy
 * D3D9 bytecode plus constant table. Returns the HRESULT; the out pointers get
 * the guest COM objects (ID3DXBuffer) or 0. */
uint32_t hle_d3d9fx_compile_source(const char *src, size_t src_len, const char *src_name,
                                  uint32_t macros_va, uint32_t include_va,
                                  const char *entry, const char *profile, uint32_t flags,
                                  uint32_t ppShader, uint32_t ppErr, uint32_t ppCT) {
    return (uint32_t)do_compile_common(src, src_len, src_name, macros_va, include_va,
                                     entry, profile, flags, ppShader, ppErr, ppCT, 10);
}

static void s_D3DXCompileShader(void) {
    /* (pSrcData, srcDataLen, pDefines, pInclude, pFunctionName, pProfile, Flags, ppShader, ppErrorMsgs, ppConstantTable) */
    const char *src = ASTR(0);
    uint32_t src_len = A32(1);
    uint32_t defines = A32(2);
    uint32_t include = A32(3);
    const char *entry = ASTR(4);
    const char *profile = ASTR(5);
    uint32_t flags = A32(6);
    uint32_t ppShader = A32(7);
    uint32_t ppErr = A32(8);
    uint32_t ppCT = A32(9);
    if (!src) { set_out_buf(ppShader,0); set_out_buf(ppErr,0); set_out_buf(ppCT,0); RET(D3DERR_INVALIDCALL,10); return; }
    size_t slen = src_len ? (size_t)src_len : strlen(src);
    RET((uint32_t)do_compile_common(src, slen, "shader.hlsl", defines, include, entry, profile, flags, ppShader, ppErr, ppCT, 10), 10);
}

static void s_D3DXCompileShaderFromFileA(void) {
    /* (pSrcFile, pDefines, pInclude, pFunctionName, pProfile, Flags, ppShader, ppErrorMsgs, ppConstantTable) */
    const char *fname = ASTR(0);
    uint32_t defines = A32(1);
    uint32_t include = A32(2);
    const char *entry = ASTR(3);
    const char *profile = ASTR(4);
    uint32_t flags = A32(5);
    uint32_t ppShader = A32(6);
    uint32_t ppErr = A32(7);
    uint32_t ppCT = A32(8);
    set_out_buf(ppShader,0); set_out_buf(ppErr,0); set_out_buf(ppCT,0);
    if (!fname) { RET(D3DERR_INVALIDCALL,9); return; }
    char host[1024];
    if (!hle_host_path(fname, host, sizeof host)) {
        fprintf(stderr, "[d3dx9] CompileShaderFromFileA: not found: %s\n", fname);
        RET(D3DERR_INVALIDCALL, 9); return;
    }
    FILE *f = fopen(host, "rb");
    if (!f) { RET(D3DERR_INVALIDCALL, 9); return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); RET(E_FAIL, 9); return; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); RET(E_OUTOFMEMORY, 9); return; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = 0;
    RET((uint32_t)do_compile_common(buf, rd, fname, defines, include, entry, profile, flags, ppShader, ppErr, ppCT, 9), 9);
    free(buf);
}

static void s_D3DXPreprocessShader(void) {
    /* (pSrcData, srcDataLen, pDefines, pInclude, ppShaderText, ppErrorMsgs) */
    const char *src = ASTR(0);
    uint32_t src_len = A32(1);
    uint32_t defines = A32(2);
    uint32_t include = A32(3);
    uint32_t ppText = A32(4);
    uint32_t ppErr = A32(5);
    set_out_buf(ppText, 0); set_out_buf(ppErr, 0);
    if (!src) { RET(D3DERR_INVALIDCALL, 6); return; }
    if (!hle_vkd3d_available()) { fail_unavailable(ppText, ppErr, 0, 6); return; }
    size_t slen = src_len ? (size_t)src_len : strlen(src);
    unsigned nmac = 0;
    hle_vkd3d_macro *macros = build_macros(defines, &nmac);
    include_ctx_t ic = { include };
    hle_vkd3d_code out = {0};
    char *messages = NULL;
    int rc = hle_vkd3d_preprocess(src, slen, "shader.hlsl", macros, nmac,
                                  include ? include_open : NULL,
                                  include ? include_close : NULL,
                                  include ? &ic : NULL,
                                  &out, &messages);
    free(macros);
    if (messages) { set_out_buf(ppErr, make_dxbuf(messages, strlen(messages) + 1)); hle_vkd3d_free_messages(messages); }
    if (rc != HLE_VKD3D_OK) {
        hle_vkd3d_free_code(&out);
        fprintf(stderr, "[d3dx9] preprocess failed (%d)\n", rc);
        RET(rc == HLE_VKD3D_ERR_UNAVAILABLE ? D3DXERR_NOTAVAILABLE : E_FAIL, 6);
        return;
    }
    set_out_buf(ppText, make_dxbuf(out.code, out.size));
    hle_vkd3d_free_code(&out);
    RET(D3D_OK, 6);
}

/* D3DXCreateEffectEx and D3DXCreateEffectCompiler are implemented in
 * d3dx9_fxsource.c. They must not also appear here: hle_resolve returns the
 * first registration, and a stale "not available" stub would shadow the real
 * implementation. */

const win32hle_shim win32hle_d3dx9_compiler[] = {
    { "D3DXCompileShader",           s_D3DXCompileShader },
    { "D3DXCompileShaderFromFileA",  s_D3DXCompileShaderFromFileA },
    { "D3DXPreprocessShader",        s_D3DXPreprocessShader },
    { 0, 0 }
};
