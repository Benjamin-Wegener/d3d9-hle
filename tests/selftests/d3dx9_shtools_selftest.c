/*
 * d3dx9_shtools_selftest.c - shader tools with hand-derived expectations.
 * The disassembly text and the token stream are compared to independently
 * written values, not to anything computed by the code under test.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "win32hle.h"
#include "host.h"
#include "d3d9_hle.h"
#include "d3dx9_hle.h"
#include "d3dx9_asm.h"
#include "d3dx9_shtools.h"

#define D3D_OK_ 0u

const recomp_dispatch_entry_t recomp_dispatch_table[] = { { 0, 0 } };
const uint32_t recomp_dispatch_count = 0;

static int fails;
static void ok(const char *what, int cond) { if (!cond) { printf("FAIL %s\n", what); fails++; } }
static uint32_t call(const char *n, int c, const uint32_t *a) { return hle_call_guest(hle_resolve(n), c, a); }

int main(void) {
    recomp_host_init();
    win32hle_register(win32hle_d3dx9);

    /* ---- disassembly of a hand-built token stream ---- */
    /* vs_1_1; dcl_position v0; mov oPos, v0; END
     * Tokens are the public D3D9 encoding, written out by hand. */
    uint32_t toks[8] = {
        0xFFFE0101u,                       /* version vs_1_1 */
        0x0200001Fu, 0x00000000u, 0x10000000u,  /* dcl len 2, usage position, v0 */
        0x02000001u, 0xC00F0000u, 0x10E40000u,  /* mov len 2, oPos(dst), v0(src) */
        /* END appended below */
    };
    char *text = NULL; char err[256];
    int drc = d3dx9_disassemble_text(toks, 7, &text, err, sizeof err);
    ok("disassemble hand-built tokens", drc == 0);
    if (text) {
        const char *want = "vs_1_1\ndcl_position v0\nmov oPos, v0\n\n";
        ok("disassembly text matches hand-written expectation", strcmp(text, want) == 0);
        if (strcmp(text, want) != 0) printf("  got: [%s]\n", text);
        free(text);
    }

    /* ---- assemble -> disassemble -> assemble roundtrip with the real HLE ---- */
    const char *asm_src = "vs_1_1\ndcl_position v0\nadd r0, v0, c0\nmov oPos, r0\n";
    void *gsrc = hle_alloc(strlen(asm_src)+1);
    memcpy(gsrc, asm_src, strlen(asm_src)+1);
    uint32_t nsrc = (uint32_t)strlen(asm_src);
    uint32_t *pSh = (uint32_t *)hle_alloc(4), *pErr = (uint32_t *)hle_alloc(4);
    *pSh = 0; *pErr = 0;
    ok("D3DXAssembleShader", call("D3DXAssembleShader", 7,
        (uint32_t[]){ hle_va(gsrc), nsrc, 0, 0, 0, hle_va(pSh), hle_va(pErr) }) == D3D_OK_ && *pSh);
    if (*pSh) {
        uint32_t pdata = call("ID3DXBuffer::GetBufferPointer",1,(uint32_t[]){*pSh});
        uint32_t psize = call("ID3DXBuffer::GetBufferSize",1,(uint32_t[]){*pSh});
        uint32_t *pDis = (uint32_t *)hle_alloc(4); *pDis = 0;
        ok("D3DXDisassembleShader", call("D3DXDisassembleShader", 4,
            (uint32_t[]){ *pSh, 0, 0, hle_va(pDis) }) == D3D_OK_ && *pDis);
        if (*pDis) {
            uint32_t dp = call("ID3DXBuffer::GetBufferPointer",1,(uint32_t[]){*pDis});
            char *dtext = (char *)hle_ptr(dp);
            ok("roundtrip disassembly has all three instructions",
               strstr(dtext,"dcl_position v0") && strstr(dtext,"add r0, v0, c0") &&
               strstr(dtext,"mov oPos, r0"));
            void *g2 = hle_alloc(strlen(dtext)+1);
            memcpy(g2, dtext, strlen(dtext)+1);
            uint32_t *pSh2 = (uint32_t *)hle_alloc(4); *pSh2 = 0;
            ok("reassembled disassembly", call("D3DXAssembleShader", 7,
                (uint32_t[]){ hle_va(g2), (uint32_t)strlen(dtext), 0,0,0, hle_va(pSh2), hle_va(pErr) }) == D3D_OK_ && *pSh2);
            if (*pSh2) {
                uint32_t d2 = call("ID3DXBuffer::GetBufferPointer",1,(uint32_t[]){*pSh2});
                uint32_t z2 = call("ID3DXBuffer::GetBufferSize",1,(uint32_t[]){*pSh2});
                ok("roundtrip token size identical", z2 == psize);
                ok("roundtrip tokens byte-identical", z2 == psize && memcmp(hle_ptr(d2), hle_ptr(pdata), psize) == 0);
            }
        }
    }

    /* ---- profiles follow the real device caps ---- */
    uint32_t d3d9 = call("Direct3DCreate9", 1, (uint32_t[]){ 32u });
    ok("Direct3DCreate9", d3d9 != 0);
    D3DPRESENT_PARAMETERS *pp = (D3DPRESENT_PARAMETERS *)hle_alloc(sizeof *pp);
    memset(pp, 0, sizeof *pp);
    pp->BackBufferWidth = 32; pp->BackBufferHeight = 32; pp->BackBufferFormat = 21;
    pp->BackBufferCount = 1; pp->Windowed = 1; pp->SwapEffect = 1;
    uint32_t *pDev = (uint32_t *)hle_alloc(4); *pDev = 0;
    ok("CreateDevice", call("IDirect3D9::CreateDevice", 7,
        (uint32_t[]){ d3d9, 0, D3DDEVTYPE_HAL, 0, D3DCREATE_HARDWARE_VERTEXPROCESSING,
                      hle_va(pp), hle_va(pDev) }) == D3D_OK_ && *pDev);
    if (*pDev) {
        uint32_t vsp = call("D3DXGetVertexShaderProfile",1,(uint32_t[]){*pDev});
        uint32_t psp = call("D3DXGetPixelShaderProfile",1,(uint32_t[]){*pDev});
        ok("VS profile is vs_3_0 (caps report SM3)", vsp && strcmp((char*)hle_ptr(vsp),"vs_3_0")==0);
        ok("PS profile is ps_3_0 (caps report SM3)", psp && strcmp((char*)hle_ptr(psp),"ps_3_0")==0);
        ok("NULL device has no profile", call("D3DXGetVertexShaderProfile",1,(uint32_t[]){0}) == 0);
    }

    /* ---- file entry points with self-written files ---- */
    const char *src2 = "ps_2_0\nmov oC0, c0\n";
    const char *path = "/tmp/d3dx9_shtools_test.hlsl";
    FILE *f = fopen(path,"wb"); fwrite(src2,1,strlen(src2),f); fclose(f);
    void *gpath = hle_alloc(strlen(path)+1); memcpy(gpath,path,strlen(path)+1);
    uint32_t *pf = (uint32_t *)hle_alloc(4), *pe = (uint32_t *)hle_alloc(4);
    *pf=0; *pe=0;
    ok("AssembleShaderFromFileA", call("D3DXAssembleShaderFromFileA",6,
        (uint32_t[]){ hle_va(gpath),0,0,0,hle_va(pf),hle_va(pe) }) == D3D_OK_ && *pf);
    if (*pf) {
        uint32_t d = call("ID3DXBuffer::GetBufferPointer",1,(uint32_t[]){*pf});
        ok("file assembly produced the mov opcode", ((uint32_t*)hle_ptr(d))[1] == 0x02000001u);
    }
    *pf=0;
    ok("AssembleShaderFromFileA missing file fails", call("D3DXAssembleShaderFromFileA",6,
        (uint32_t[]){ 0,0,0,0,hle_va(pf),hle_va(pe) }) != D3D_OK_);

    uint16_t *wpath = (uint16_t *)hle_alloc((strlen(path)+1)*2);
    for (size_t i=0;i<=strlen(path);i++) wpath[i]=(uint16_t)(unsigned char)path[i];
    *pf=0;
    ok("AssembleShaderFromFileW", call("D3DXAssembleShaderFromFileW",6,
        (uint32_t[]){ hle_va(wpath),0,0,0,hle_va(pf),hle_va(pe) }) == D3D_OK_ && *pf);

    *pf=0;
    ok("AssembleShaderFromFileA with defines is an error", call("D3DXAssembleShaderFromFileA",6,
        (uint32_t[]){ hle_va(gpath), 0x1234u, 0, 0, hle_va(pf), hle_va(pe) }) == 0x88760866u);

    *pf=0;
    /* PreprocessShaderFromFileA(filename, defines, include, shader, error_messages) = 5 args */
    ok("PreprocessShaderFromFileA", call("D3DXPreprocessShaderFromFileA",5,
        (uint32_t[]){ hle_va(gpath),0,0,hle_va(pf),hle_va(pe) }) == D3D_OK_ && *pf);
    if (*pf) {
        uint32_t d = call("ID3DXBuffer::GetBufferPointer",1,(uint32_t[]){*pf});
        ok("preprocessed text equals the source", strcmp((char*)hle_ptr(d),src2)==0);
    }

    /* Real preprocessing: a source-internal object-like macro and a
     * conditional must be expanded, not copied verbatim. Expected output is
     * written by hand, independently of the preprocessor implementation. */
    const char *macro_src =
        "#define RED c0\n"
        "ps_2_0\n"
        "#ifdef RED\n"
        "mov oC0, RED\n"
        "#else\n"
        "mov oC0, c1\n"
        "#endif\n";
    const char *macro_path = "/tmp/d3dx9_shtools_macro.hlsl";
    f = fopen(macro_path, "wb");
    fwrite(macro_src, 1, strlen(macro_src), f); fclose(f);
    void *gmacro = hle_alloc(strlen(macro_path) + 1);
    memcpy(gmacro, macro_path, strlen(macro_path) + 1);
    *pf = 0; *pe = 0;
    ok("PreprocessShaderFromFileA expands macros", call("D3DXPreprocessShaderFromFileA",5,
        (uint32_t[]){ hle_va(gmacro),0,0,hle_va(pf),hle_va(pe) }) == D3D_OK_ && *pf);
    if (*pf) {
        uint32_t d = call("ID3DXBuffer::GetBufferPointer",1,(uint32_t[]){*pf});
        const char *want = "ps_2_0\nmov oC0, c0\n";
        ok("macro expansion matches hand-written expectation", strcmp((char*)hle_ptr(d), want) == 0);
        if (strcmp((char*)hle_ptr(d), want) != 0) printf("  got: [%s]\n", (char*)hle_ptr(d));
    }
    remove(macro_path);

    /* Unsupported directives must fail honestly with an error buffer. */
    const char *bad_src = "#include \"other.hlsl\"\nps_2_0\n";
    const char *bad_path = "/tmp/d3dx9_shtools_bad.hlsl";
    f = fopen(bad_path, "wb");
    fwrite(bad_src, 1, strlen(bad_src), f); fclose(f);
    void *gbad = hle_alloc(strlen(bad_path) + 1);
    memcpy(gbad, bad_path, strlen(bad_path) + 1);
    *pf = 0; *pe = 0;
    ok("unsupported #include fails", call("D3DXPreprocessShaderFromFileA",5,
        (uint32_t[]){ hle_va(gbad),0,0,hle_va(pf),hle_va(pe) }) != D3D_OK_);
    ok("error buffer has a message", *pe && strstr((char*)hle_ptr(call("ID3DXBuffer::GetBufferPointer",1,(uint32_t[]){*pe})), "unsupported"));
    remove(bad_path);
    remove(path);

    printf(fails ? "d3dx9_shtools_selftest: FAILED (%d)\n" : "d3dx9_shtools_selftest: OK\n", fails);
    return fails != 0;
}
