/* Anisotropic filtering on real Metal: a minified checkerboard sampled with
 * D3DTEXF_ANISOTROPIC + MAXANISOTROPY must differ from POINT sampling. */
#define RECOMP_GENERATED_CODE
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "win32hle.h"
#include "host.h"
#include "d3d9_hle.h"
#include "d3d9_metal.h"

const recomp_dispatch_entry_t recomp_dispatch_table[] = { { 0, 0 } };
const uint32_t recomp_dispatch_count = 0;
static int fails;
static void ok(const char *w, int c) { printf("  [%s] %s\n", c?"PASS":"FAIL", w); if(!c) fails++; }
static uint32_t call(const char*n,int c,const uint32_t*a){return hle_call_guest(hle_resolve(n),c,a);}
#define A(p) hle_va(p)
#define W 32
#define H 32
typedef struct { float x,y,z,w,r,g,b,a,u,v; } V;
static V qv(float x,float y,float u,float v){ V t={x,y,0.5f,1,1,1,1,1,u,v}; return t; }

int main(void) {
    recomp_host_init();
    win32hle_register(win32hle_d3d9);
    if (hle_d3d9_metal_enable()!=0) { printf("  [FAIL] Metal\n"); return 1; }
    uint32_t d3d9=call("Direct3DCreate9",1,(uint32_t[]){32});
    D3DPRESENT_PARAMETERS *pp=(D3DPRESENT_PARAMETERS*)hle_alloc(sizeof(*pp));
    memset(pp,0,sizeof(*pp)); pp->BackBufferWidth=W; pp->BackBufferHeight=H;
    pp->BackBufferFormat=D3DFMT_A8R8G8B8; pp->BackBufferCount=1;
    uint32_t *pd=(uint32_t*)hle_alloc(4); *pd=0;
    ok("CreateDevice",call("IDirect3D9::CreateDevice",7,(uint32_t[]){d3d9,0,D3DDEVTYPE_HAL,0,D3DCREATE_HARDWARE_VERTEXPROCESSING,A(pp),A(pd)})==D3D_OK && *pd);
    uint32_t dev=*pd;

    /* 64x64 red/green checkerboard minified onto a small quad. */
    uint32_t *pt=(uint32_t*)hle_alloc(4); *pt=0;
    call("IDirect3DDevice9::CreateTexture",9,(uint32_t[]){dev,64,64,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,A(pt),0});
    ok("CreateTexture(64x64)",*pt!=0);
    uint32_t tex=*pt;
    D3DLOCKED_RECT *lr=(D3DLOCKED_RECT*)hle_alloc(8);
    call("IDirect3DTexture9::LockRect",5,(uint32_t[]){tex,0,A(lr),0,0});
    for(int y=0;y<64;y++) for(int x=0;x<64;x++)
        ((uint32_t*)hle_ptr(lr->pBits))[y*64+x] = ((x+y)&1) ? 0xFF00FF00u : 0xFFFF0000u;
    call("IDirect3DTexture9::UnlockRect",2,(uint32_t[]){tex,0});
    call("IDirect3DDevice9::SetTexture",3,(uint32_t[]){dev,0,tex});

    uint32_t *pvb=(uint32_t*)hle_alloc(4); *pvb=0;
    uint32_t sz=sizeof(V)*4;
    call("IDirect3DDevice9::CreateVertexBuffer",7,(uint32_t[]){dev,sz,0,0,D3DPOOL_MANAGED,A(pvb),0});
    uint32_t *pdata=(uint32_t*)hle_alloc(4);
    call("IDirect3DVertexBuffer9::Lock",5,(uint32_t[]){*pvb,0,sz,A(pdata),0});
    V *v=(V*)hle_ptr(*pdata);
    /* A quarter-size quad: heavy minification. */
    v[0]=qv(-0.5f, 0.5f,0,0); v[1]=qv(0.5f, 0.5f,1,0);
    v[2]=qv(-0.5f,-0.5f,0,1); v[3]=qv(0.5f,-0.5f,1,1);
    call("IDirect3DVertexBuffer9::Unlock",1,(uint32_t[]){*pvb});
    call("IDirect3DDevice9::SetStreamSource",5,(uint32_t[]){dev,0,*pvb,0,sz});
    call("IDirect3DDevice9::SetFVF",2,(uint32_t[]){dev,0});
    call("IDirect3DDevice9::SetSamplerState",4,(uint32_t[]){dev,0,7,0}); /* MIPFILTER NONE */

    uint32_t pixels[W*H];
    #define DRAW do{ call("IDirect3DDevice9::Clear",7,(uint32_t[]){dev,0,0,1,0xFF000000u,0,0}); \
      call("IDirect3DDevice9::BeginScene",1,(uint32_t[]){dev}); \
      call("IDirect3DDevice9::DrawPrimitive",4,(uint32_t[]){dev,D3DPT_TRIANGLESTRIP,0,2}); \
      call("IDirect3DDevice9::EndScene",1,(uint32_t[]){dev}); \
      hle_d3d9_metal_read_pixels(0,0,W,H,pixels,W*4); }while(0)

    /* POINT: one texel, pure red or pure green. */
    call("IDirect3DDevice9::SetSamplerState",4,(uint32_t[]){dev,0,6,1});
    DRAW;
    uint32_t point = pixels[16*W+16];
    /* ANISOTROPIC with 8x: averaged footprint. */
    call("IDirect3DDevice9::SetSamplerState",4,(uint32_t[]){dev,0,6,3});
    call("IDirect3DDevice9::SetSamplerState",4,(uint32_t[]){dev,0,8,8});
    DRAW;
    uint32_t aniso = pixels[16*W+16];
    printf("    point=0x%08X aniso=0x%08X\n", point, aniso);
    ok("POINT yields a single checkerboard texel",
       point==0xFFFF0000u || point==0xFF00FF00u);
    ok("anisotropic sampling differs from POINT", aniso != point);

    call("IDirect3DVertexBuffer9::Release",1,(uint32_t[]){*pvb});
    call("IDirect3DTexture9::Release",1,(uint32_t[]){tex});
    call("IDirect3DDevice9::Release",1,(uint32_t[]){dev});
    call("IDirect3D9::Release",1,(uint32_t[]){d3d9});
    hle_d3d9_metal_shutdown_device();
    printf(fails?"test_d3d9_aniso: FAILED\n":"test_d3d9_aniso: OK\n");
    return fails!=0;
}
