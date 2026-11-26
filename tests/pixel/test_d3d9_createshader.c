/* Create(Vertex|Pixel)Shader must return D3DERR_INVALIDCALL (and *ppShader=0)
 * when the shader translation fails, instead of a shader object with empty MSL
 * (a false OK that crashes later). Expected values are independent of the
 * implementation: a shader whose translation fails -> INVALIDCALL + *pp==0; a
 * valid shader -> D3D_OK + *pp!=0. */
#define RECOMP_GENERATED_CODE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "win32hle.h"
#include "host.h"
#include "d3d9_hle.h"

const recomp_dispatch_entry_t recomp_dispatch_table[] = { {0,0} };
const uint32_t recomp_dispatch_count = 0;
static int fails;
static void ok(const char *w,int c){ printf("  [%s] %s\n",c?"PASS":"FAIL",w); if(!c) fails++; }
static uint32_t call(const char *n,int c,const uint32_t *a){ return hle_call_guest(hle_resolve(n),c,a); }
#define A(p) hle_va(p)
#define REG(t,n) ((((uint32_t)(t)&7u)<<28u)|((((uint32_t)(t)>>3u)&3u)<<11u)|(uint32_t)(n))
#define DST(t,n) (0x80000000u|REG(t,n)|(0xFu<<16u))
#define SRC(t,n) (REG(t,n)|(0xE4u<<16u))
#define END 0x0000FFFFu

static void *G_copy(const void *s,size_t n){ void *p=hle_alloc((uint32_t)n); memcpy(p,s,n); return p; }

int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    recomp_host_init(); win32hle_register(win32hle_d3d9);
    if (hle_d3d9_metal_enable()!=0){ ok("Metal backend available",0); return 1; }
    uint32_t d3d9=call("Direct3DCreate9",1,(uint32_t[]){32});
    if(!d3d9){ ok("Direct3DCreate9",0); return 1; }
    D3DPRESENT_PARAMETERS *pp=(D3DPRESENT_PARAMETERS*)hle_alloc(sizeof(*pp));
    memset(pp,0,sizeof(*pp)); pp->BackBufferWidth=32; pp->BackBufferHeight=32;
    pp->BackBufferFormat=D3DFMT_A8R8G8B8; pp->BackBufferCount=1; pp->Windowed=1;
    uint32_t *pd=(uint32_t*)hle_alloc(4); *pd=0;
    ok("CreateDevice",call("IDirect3D9::CreateDevice",7,
       (uint32_t[]){d3d9,0,D3DDEVTYPE_HAL,0,D3DCREATE_HARDWARE_VERTEXPROCESSING,A(pp),A(pd)})==D3D_OK && *pd);
    uint32_t dev=*pd;

    /* Valid ps_2_0: def c0=(0,1,0,1); mov oC0,c0. */
    uint32_t ps_ok[]={0xFFFF0200u,
        0x05000051u,0xA00F0000u,0x00000000u,0x3F800000u,0x00000000u,0x3F800000u,
        0x02000001u,DST(8,0),SRC(2,0),END};
    /* Failing ps_2_0: unknown opcode 0x61 -- the translator has no emitter. */
    uint32_t ps_bad[]={0xFFFF0200u,0x00000061u,END};
    /* Valid vs_3_0: dcl_position v0; mov oPos,v0. */
    uint32_t vs_ok[]={0xFFFE0300u,
        0x0200001Fu,0x80000000u,0x900F0000u,
        0x02000001u,0xC00F0000u,0x90E40000u,END};
    /* Failing vs_3_0: unknown opcode 0x61 (translator has no emitter). */
    uint32_t vs_bad[]={0xFFFE0300u,0x00000061u,END};

    uint32_t *p=(uint32_t*)hle_alloc(4);

    *p=0xDEADBEEFu;
    uint32_t hr=call("IDirect3DDevice9::CreatePixelShader",3,(uint32_t[]){dev,A(G_copy(ps_bad,sizeof ps_bad)),A(p)});
    ok("CreatePixelShader(bad) == D3DERR_INVALIDCALL", hr==D3DERR_INVALIDCALL);
    ok("CreatePixelShader(bad) sets *ppShader=0", *p==0);

    *p=0xDEADBEEFu;
    hr=call("IDirect3DDevice9::CreateVertexShader",3,(uint32_t[]){dev,A(G_copy(vs_bad,sizeof vs_bad)),A(p)});
    ok("CreateVertexShader(bad) == D3DERR_INVALIDCALL", hr==D3DERR_INVALIDCALL);
    ok("CreateVertexShader(bad) sets *ppShader=0", *p==0);

    *p=0;
    hr=call("IDirect3DDevice9::CreatePixelShader",3,(uint32_t[]){dev,A(G_copy(ps_ok,sizeof ps_ok)),A(p)});
    ok("CreatePixelShader(valid) == D3D_OK", hr==D3D_OK && *p!=0);

    *p=0;
    hr=call("IDirect3DDevice9::CreateVertexShader",3,(uint32_t[]){dev,A(G_copy(vs_ok,sizeof vs_ok)),A(p)});
    ok("CreateVertexShader(valid) == D3D_OK", hr==D3D_OK && *p!=0);

    printf(fails==0?"test_d3d9_createshader: OK\n":"test_d3d9_createshader: FAILED\n");
    return fails!=0;
}