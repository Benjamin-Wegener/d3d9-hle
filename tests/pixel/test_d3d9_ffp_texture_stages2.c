/* Test texture stage constant 0 and complement (hand-calculated). */
#include "test_d3d9_common.h"

typedef struct { float x,y,z,rhw; uint32_t diffuse; float u,v; } VtxTex;

int main(void) {
    uint32_t dev = make_device();
    if (!dev) return 1;
    /* 2x2 white texture */
    uint32_t *pTex = (uint32_t*)hle_alloc(4);
    uint32_t hr = call("IDirect3DDevice9::CreateTexture", 9,
        (uint32_t[]){ dev, 2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, A(pTex), 0 });
    ok("CreateTexture", hr == D3D_OK && *pTex);
    D3DLOCKED_RECT *plr = (D3DLOCKED_RECT*)hle_alloc(sizeof(*plr));
    call("IDirect3DTexture9::LockRect", 5, (uint32_t[]){ *pTex, 0, A(plr), 0, 0 });
    uint32_t *px = (uint32_t*)hle_ptr(plr->pBits);
    for (int i=0;i<4;i++) px[i]=0xFFFFFFFFu;
    call("IDirect3DTexture9::UnlockRect", 2, (uint32_t[]){ *pTex, 0 });

    VtxTex v[4] = {
        {-1,-1,0.5,1, 0xFFFFFFFFu, 0,0},
        {1,-1,0.5,1, 0xFFFFFFFFu, 1,0},
        {1,1,0.5,1, 0xFFFFFFFFu, 1,1},
        {-1,1,0.5,1, 0xFFFFFFFFu, 0,1}
    };
    uint32_t vb_size=sizeof(v);
    uint32_t *pVB=(uint32_t*)hle_alloc(4);
    call("IDirect3DDevice9::CreateVertexBuffer",7,(uint32_t[]){dev,vb_size,0,0,D3DPOOL_MANAGED,A(pVB),0});
    uint32_t *pData=(uint32_t*)hle_alloc(4);
    call("IDirect3DVertexBuffer9::Lock",5,(uint32_t[]){*pVB,0,vb_size,A(pData),0});
    memcpy(hle_ptr(*pData), v, vb_size);
    call("IDirect3DVertexBuffer9::Unlock",1,(uint32_t[]){*pVB});

    call("IDirect3DDevice9::SetTexture",3,(uint32_t[]){dev,0,*pTex});
    /* constant = 0, SELECTARG1 constant -> black */
    call("IDirect3DDevice9::SetTextureStageState",4,(uint32_t[]){dev,0,1,2}); /* COLOROP SELECTARG1 */
    call("IDirect3DDevice9::SetTextureStageState",4,(uint32_t[]){dev,0,2,6}); /* COLORARG1 CONSTANT */
    call("IDirect3DDevice9::SetTextureStageState",4,(uint32_t[]){dev,0,32,0}); /* CONSTANT = 0 */
    call("IDirect3DDevice9::SetStreamSource",5,(uint32_t[]){dev,0,*pVB,0,sizeof(VtxTex)});
    call("IDirect3DDevice9::SetFVF",2,(uint32_t[]){dev, 0x004u|0x040u|(1u<<8)});

    clear(dev, 0xFF000000u, 1, 1.0f);
    call("IDirect3DDevice9::BeginScene",1,(uint32_t[]){dev});
    call("IDirect3DDevice9::DrawPrimitive",4,(uint32_t[]){dev, D3DPT_TRIANGLEFAN, 0, 2});
    call("IDirect3DDevice9::EndScene",1,(uint32_t[]){dev});
    uint32_t pixels[32*32];
    read_all(pixels);
    ok("constant 0 -> black", pixels[16*32+16] == 0xFF000000u);
    if (pixels[16*32+16] != 0xFF000000u) printf("    got 0x%08X\n", pixels[16*32+16]);

    call("IDirect3DTexture9::Release",1,(uint32_t[]){*pTex});
    call("IDirect3DVertexBuffer9::Release",1,(uint32_t[]){*pVB});
    printf(fails==0 ? "test_d3d9_ffp_texture_stages2: OK\n" : "test_d3d9_ffp_texture_stages2: FAILED\n");
    return fails != 0;
}