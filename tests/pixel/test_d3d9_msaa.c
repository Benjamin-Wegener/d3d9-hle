/* MSAA render target with real resolve on Metal.
 * A red triangle over a green clear: the diagonal edge pixel must be a
 * blend, which cannot happen without multisampling + resolve. */
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
typedef struct { float x,y,z,w; uint32_t color; } V;
static V cv(float x,float y,uint32_t c){ V t={x,y,0.5f,1,c}; return t; }

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

    uint32_t *prt=(uint32_t*)hle_alloc(4); *prt=0;
    /* CreateRenderTarget(dev,w,h,fmt,MultiSample=4,Quality=0,Lockable,pp,shared) */
    uint32_t hr=call("IDirect3DDevice9::CreateRenderTarget",9,
        (uint32_t[]){dev,W,H,D3DFMT_A8R8G8B8,4,0,0,A(prt),0});
    ok("CreateRenderTarget MSAA 4x",hr==D3D_OK && *prt);
    if(!*prt) return 1;
    uint32_t rt=*prt;
    ok("SetRenderTarget(MSAA)",call("IDirect3DDevice9::SetRenderTarget",3,(uint32_t[]){dev,0,rt})==D3D_OK);

    uint32_t *pvb=(uint32_t*)hle_alloc(4); *pvb=0;
    uint32_t sz=sizeof(V)*3;
    call("IDirect3DDevice9::CreateVertexBuffer",7,(uint32_t[]){dev,sz,0,0,D3DPOOL_MANAGED,A(pvb),0});
    uint32_t *pdata=(uint32_t*)hle_alloc(4);
    call("IDirect3DVertexBuffer9::Lock",5,(uint32_t[]){*pvb,0,sz,A(pdata),0});
    V *v=(V*)hle_ptr(*pdata);
    /* Triangle with a diagonal edge through the centre of the target. */
    v[0]=cv( 0.0f, 0.0f,0xFFFF0000u);
    v[1]=cv(32.0f, 0.0f,0xFFFF0000u);
    v[2]=cv( 0.0f,32.0f,0xFFFF0000u);
    call("IDirect3DVertexBuffer9::Unlock",1,(uint32_t[]){*pvb});
    call("IDirect3DDevice9::SetStreamSource",5,(uint32_t[]){dev,0,*pvb,0,sz});
    call("IDirect3DDevice9::SetFVF",2,(uint32_t[]){dev,0x44u /* XYZRHW|DIFFUSE */});

    call("IDirect3DDevice9::SetRenderState",3,(uint32_t[]){dev,7 /*ZENABLE*/,0});
    call("IDirect3DDevice9::Clear",7,(uint32_t[]){dev,0,0,1,0xFF00FF00u,0,0});
    call("IDirect3DDevice9::BeginScene",1,(uint32_t[]){dev});
    ok("DrawPrimitive triangle",call("IDirect3DDevice9::DrawPrimitive",4,(uint32_t[]){dev,D3DPT_TRIANGLELIST,0,1})==D3D_OK);
    call("IDirect3DDevice9::EndScene",1,(uint32_t[]){dev});

    /* GPU readback of the current render target resolves the multisample
     * texture into its single-sample resolve texture first. */
    uint32_t pixels[W*H];
    ok("Metal readback resolves MSAA", hle_d3d9_metal_read_pixels(0,0,W,H,pixels,W*4)==0);
    const uint32_t *rx=pixels;
    uint32_t inside=rx[8*W+8], outside=rx[24*W+24];
    printf("    inside=0x%08X outside=0x%08X\n",inside,outside);
    ok("triangle interior is red",inside==0xFFFF0000u);
    ok("triangle exterior is green",outside==0xFF00FF00u);
    /* Find any pixel that is neither pure red nor pure green: MSAA coverage. */
    int blended=0;
    for(uint32_t i=0;i<W*H;i++) if(rx[i]!=0xFFFF0000u && rx[i]!=0xFF00FF00u) blended++;
    printf("    blended pixels: %d\n",blended);
    ok("resolve produced antialiased edge pixels",blended>0);

    call("IDirect3DDevice9::SetRenderTarget",3,(uint32_t[]){dev,0,0});
    call("IDirect3DVertexBuffer9::Release",1,(uint32_t[]){*pvb});
    call("IDirect3DSurface9::Release",1,(uint32_t[]){rt});
    call("IDirect3DDevice9::Release",1,(uint32_t[]){dev});
    call("IDirect3D9::Release",1,(uint32_t[]){d3d9});
    hle_d3d9_metal_shutdown_device();
    printf(fails?"test_d3d9_msaa: FAILED\n":"test_d3d9_msaa: OK\n");
    return fails!=0;
}
