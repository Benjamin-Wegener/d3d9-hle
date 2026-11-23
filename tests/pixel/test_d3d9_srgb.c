/* sRGB texture reads through D3DSAMP_SRGBTEXTURE on real Metal.
 * The same opaque gray texel must read differently with the sRGB view. */
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

    uint32_t *pt=(uint32_t*)hle_alloc(4); *pt=0;
    ok("CreateTexture",call("IDirect3DDevice9::CreateTexture",9,(uint32_t[]){dev,2,2,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,A(pt),0})==D3D_OK && *pt);
    uint32_t tex=*pt;
    D3DLOCKED_RECT *lr=(D3DLOCKED_RECT*)hle_alloc(8);
    call("IDirect3DTexture9::LockRect",5,(uint32_t[]){tex,0,A(lr),0,0});
    uint32_t gray=0xFF808080u;
    for(int i=0;i<4;i++) ((uint32_t*)hle_ptr(lr->pBits))[i]=gray;
    call("IDirect3DTexture9::UnlockRect",2,(uint32_t[]){tex,0});
    call("IDirect3DDevice9::SetTexture",3,(uint32_t[]){dev,0,tex});

    uint32_t *pvb=(uint32_t*)hle_alloc(4); *pvb=0;
    uint32_t sz=sizeof(V)*4;
    call("IDirect3DDevice9::CreateVertexBuffer",7,(uint32_t[]){dev,sz,0,0,D3DPOOL_MANAGED,A(pvb),0});
    uint32_t *pdata=(uint32_t*)hle_alloc(4);
    call("IDirect3DVertexBuffer9::Lock",5,(uint32_t[]){*pvb,0,sz,A(pdata),0});
    V *v=(V*)hle_ptr(*pdata);
    v[0]=qv(-1,1,0,0); v[1]=qv(1,1,1,0); v[2]=qv(-1,-1,0,1); v[3]=qv(1,-1,1,1);
    call("IDirect3DVertexBuffer9::Unlock",1,(uint32_t[]){*pvb});
    call("IDirect3DDevice9::SetStreamSource",5,(uint32_t[]){dev,0,*pvb,0,sz});
    call("IDirect3DDevice9::SetFVF",2,(uint32_t[]){dev,0});

    uint32_t pixels[W*H];
    #define DRAW do{ call("IDirect3DDevice9::Clear",7,(uint32_t[]){dev,0,0,1,0xFF000000u,0,0}); \
      call("IDirect3DDevice9::BeginScene",1,(uint32_t[]){dev}); \
      call("IDirect3DDevice9::DrawPrimitive",4,(uint32_t[]){dev,D3DPT_TRIANGLESTRIP,0,2}); \
      call("IDirect3DDevice9::EndScene",1,(uint32_t[]){dev}); \
      hle_d3d9_metal_read_pixels(0,0,W,H,pixels,W*4); }while(0)
    /* D3DSAMP_SRGBTEXTURE = 11 */
    call("IDirect3DDevice9::SetSamplerState",4,(uint32_t[]){dev,0,11,0});
    DRAW;
    uint32_t linear = pixels[16*W+16];
    ok("linear sample returns the stored gray",
       (linear & 0xFFu) >= 0x7Eu && (linear & 0xFFu) <= 0x82u &&
       ((linear >> 8) & 0xFFu) >= 0x7Eu && ((linear >> 8) & 0xFFu) <= 0x82u);
    call("IDirect3DDevice9::SetSamplerState",4,(uint32_t[]){dev,0,11,1});
    DRAW;
    uint32_t srgb = pixels[16*W+16];
    printf("    linear=0x%08X srgb=0x%08X\n", linear, srgb);
    ok("sRGB sample differs from linear", srgb != linear);
    ok("sRGB decode darkens mid-gray", (srgb & 0xFFu) < (linear & 0xFFu));

    /* sRGB write: D3DRS_SRGBWRITEENABLE makes the render pass target an sRGB
     * view, so a linear 0.5 clear is stored as an encoded (> 0x80) value. */
    call("IDirect3DDevice9::SetRenderState",3,(uint32_t[]){dev,7 /*ZENABLE*/,0});
    call("IDirect3DDevice9::SetRenderState",3,(uint32_t[]){dev,4034 /*SRGBWRITEENABLE*/,0});
    call("IDirect3DDevice9::Clear",7,(uint32_t[]){dev,0,0,1,0xFF808080u,0,0});
    hle_d3d9_metal_read_pixels(0,0,W,H,pixels,W*4);
    uint32_t lin_write = pixels[16*W+16];
    call("IDirect3DDevice9::SetRenderState",3,(uint32_t[]){dev,4034,1});
    call("IDirect3DDevice9::Clear",7,(uint32_t[]){dev,0,0,1,0xFF808080u,0,0});
    hle_d3d9_metal_read_pixels(0,0,W,H,pixels,W*4);
    uint32_t srgb_write = pixels[16*W+16];
    printf("    write: linear=0x%08X srgb=0x%08X\n", lin_write, srgb_write);
    ok("linear write stores 0x80", (lin_write & 0xFFu) >= 0x7Eu && (lin_write & 0xFFu) <= 0x82u);
    ok("sRGB write encodes above 0x80", (srgb_write & 0xFFu) > 0x90u);
    call("IDirect3DDevice9::SetRenderState",3,(uint32_t[]){dev,4034,0});

    call("IDirect3DVertexBuffer9::Release",1,(uint32_t[]){*pvb});
    call("IDirect3DTexture9::Release",1,(uint32_t[]){tex});
    call("IDirect3DDevice9::Release",1,(uint32_t[]){dev});
    call("IDirect3D9::Release",1,(uint32_t[]){d3d9});
    hle_d3d9_metal_shutdown_device();
    printf(fails?"test_d3d9_srgb: FAILED\n":"test_d3d9_srgb: OK\n");
    return fails!=0;
}
