/* Hand-calculated D3D9 texture stage op/arg tests.
 * Sources: Microsoft Learn D3DTEXTUREOP, D3DTA, D3DTEXTURESTAGESTATETYPE.
 * Colors are A8R8G8B8 (0xAARRGGBB). */
#include "test_d3d9_common.h"

typedef struct { float x,y,z,rhw; uint32_t diffuse; float u,v; } VtxRHW;

static uint32_t make_tex(uint32_t dev, uint32_t color) {
    uint32_t *pTex = (uint32_t*)hle_alloc(4);
    uint32_t hr = call("IDirect3DDevice9::CreateTexture", 9,
        (uint32_t[]){ dev, 2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, A(pTex), 0 });
    ok("CreateTexture", hr == D3D_OK && *pTex);
    D3DLOCKED_RECT *plr = (D3DLOCKED_RECT*)hle_alloc(sizeof(*plr));
    call("IDirect3DTexture9::LockRect", 5, (uint32_t[]){ *pTex, 0, A(plr), 0, 0 });
    uint32_t *px = (uint32_t*)hle_ptr(plr->pBits);
    for (int i=0;i<4;i++) px[i]=color;
    call("IDirect3DTexture9::UnlockRect", 2, (uint32_t[]){ *pTex, 0 });
    return *pTex;
}

static void draw_quad(uint32_t dev, uint32_t vb, uint32_t diffuse) {
    VtxRHW v[4] = {
        { 0, 0,0.5f,1, diffuse, 0,0},
        {32, 0,0.5f,1, diffuse, 1,0},
        {32,32,0.5f,1, diffuse, 1,1},
        { 0,32,0.5f,1, diffuse, 0,1}
    };
    uint32_t *pData=(uint32_t*)hle_alloc(4);
    call("IDirect3DVertexBuffer9::Lock",5,(uint32_t[]){vb,0,sizeof(v),A(pData),0});
    memcpy(hle_ptr(*pData), v, sizeof(v));
    call("IDirect3DVertexBuffer9::Unlock",1,(uint32_t[]){vb});
    call("IDirect3DDevice9::SetStreamSource",5,(uint32_t[]){dev,0,vb,0,sizeof(VtxRHW)});
    call("IDirect3DDevice9::SetFVF",2,(uint32_t[]){dev, 0x004u|0x040u|(1u<<8)});
    clear(dev, 0xFF000000u, 1, 1.0f);
    call("IDirect3DDevice9::BeginScene",1,(uint32_t[]){dev});
    call("IDirect3DDevice9::DrawPrimitive",4,(uint32_t[]){dev, D3DPT_TRIANGLEFAN, 0, 2});
    call("IDirect3DDevice9::EndScene",1,(uint32_t[]){dev});
}

static uint32_t center(void) {
    uint32_t px[32*32];
    read_all(px);
    return px[16*32+16];
}

static void set_stage(uint32_t dev, uint32_t stage, uint32_t cop, uint32_t ca1, uint32_t ca2, uint32_t aop, uint32_t aa1, uint32_t aa2) {
    call("IDirect3DDevice9::SetTextureStageState",4,(uint32_t[]){dev,stage,1,cop}); /* COLOROP */
    call("IDirect3DDevice9::SetTextureStageState",4,(uint32_t[]){dev,stage,2,ca1}); /* COLORARG1 */
    call("IDirect3DDevice9::SetTextureStageState",4,(uint32_t[]){dev,stage,3,ca2}); /* COLORARG2 */
    call("IDirect3DDevice9::SetTextureStageState",4,(uint32_t[]){dev,stage,4,aop}); /* ALPHAOP */
    call("IDirect3DDevice9::SetTextureStageState",4,(uint32_t[]){dev,stage,5,aa1}); /* ALPHAARG1 */
    call("IDirect3DDevice9::SetTextureStageState",4,(uint32_t[]){dev,stage,6,aa2}); /* ALPHAARG2 */
}

int main(void) {
    uint32_t dev = make_device();
    if (!dev) return 1;
    uint32_t tex_white = make_tex(dev, 0xFFFFFFFFu);
    uint32_t tex_red   = make_tex(dev, 0xFFFF0000u); /* A=FF R=FF G=00 B=00 */
    uint32_t tex_half  = make_tex(dev, 0xFF804020u); /* R=80 G=40 B=20 */
    uint32_t *pVB=(uint32_t*)hle_alloc(4);
    call("IDirect3DDevice9::CreateVertexBuffer",7,(uint32_t[]){dev,sizeof(VtxRHW)*4,0,0,D3DPOOL_MANAGED,A(pVB),0});

    /* MODULATE: texture white * diffuse (R=80,G=40,B=20,A=FF) => 0xFF804020 */
    call("IDirect3DDevice9::SetTexture",3,(uint32_t[]){dev,0,tex_white});
    set_stage(dev,0,4,2,0,2,2,0); /* MODULATE, TEXTURE, DIFFUSE, SELECTARG1 TEXTURE */
    draw_quad(dev,*pVB,0xFF804020u);
    { uint32_t got=center(); ok("MODULATE white*diffuse", got==0xFF804020u); if(got!=0xFF804020u) printf("  got 0x%08X\n", got); }

    /* SELECTARG1 TEXTURE: red texture => 0xFFFF0000 */
    call("IDirect3DDevice9::SetTexture",3,(uint32_t[]){dev,0,tex_red});
    set_stage(dev,0,2,2,0,2,2,0); /* SELECTARG1 TEXTURE */
    draw_quad(dev,*pVB,0xFFFFFFFFu);
    { uint32_t got=center(); ok("SELECTARG1 TEXTURE red", got==0xFFFF0000u); if(got!=0xFFFF0000u) printf("  got 0x%08X\n", got); }

    /* SELECTARG2 DIFFUSE */
    set_stage(dev,0,3,2,0,2,2,0); /* SELECTARG2 DIFFUSE */
    draw_quad(dev,*pVB,0xFF00FF00u);
    { uint32_t got=center(); ok("SELECTARG2 DIFFUSE green", got==0xFF00FF00u); if(got!=0xFF00FF00u) printf("  got 0x%08X\n", got); }

    /* ADD: half texture + half diffuse = 0xFF804020 + 0xFF102030 = 0xFF906050 */
    call("IDirect3DDevice9::SetTexture",3,(uint32_t[]){dev,0,tex_half});
    set_stage(dev,0,7,2,0,2,2,0); /* ADD TEXTURE DIFFUSE */
    draw_quad(dev,*pVB,0xFF102030u);
    { uint32_t got=center(); ok("ADD texture+diffuse", got==0xFF906050u); if(got!=0xFF906050u) printf("  got 0x%08X\n", got); }

    /* SUBTRACT: 0xFF804020 - 0xFF102010 = 0xFF702010 */
    set_stage(dev,0,10,2,0,2,2,0); /* SUBTRACT */
    draw_quad(dev,*pVB,0xFF102010u);
    { uint32_t got=center(); ok("SUBTRACT", got==0xFF702010u); if(got!=0xFF702010u) printf("  got 0x%08X\n", got); }

    /* COMPLEMENT arg: SELECTARG1 TEXTURE|COMPLEMENT on white texture => black */
    call("IDirect3DDevice9::SetTexture",3,(uint32_t[]){dev,0,tex_white});
    set_stage(dev,0,2,2|0x10,0,2,2,0); /* SELECTARG1, COMPLEMENT */
    draw_quad(dev,*pVB,0xFFFFFFFFu);
    { uint32_t got=center(); ok("COMPLEMENT white->black", got==0xFF000000u); if(got!=0xFF000000u) printf("  got 0x%08X\n", got); }

    /* TFACTOR: SELECTARG1 TFACTOR */
    call("IDirect3DDevice9::SetRenderState",3,(uint32_t[]){dev,60,0xFF112233u}); /* D3DRS_TEXTUREFACTOR */
    set_stage(dev,0,2,3,0,2,3,0); /* SELECTARG1 TFACTOR */
    draw_quad(dev,*pVB,0xFFFFFFFFu);
    { uint32_t got=center(); ok("TFACTOR", got==0xFF112233u); if(got!=0xFF112233u) printf("  got 0x%08X\n", got); }

    /* CONSTANT arg: D3DTSS_CONSTANT=0 => ARGB 0x00000000 (A=0,R=0,G=0,B=0).
       Prior rejection checked RGB black with alpha preserved; here ALPHAARG1=CONSTANT too. */
    set_stage(dev,0,2,6,0,2,6,0); /* SELECTARG1 CONSTANT */
    call("IDirect3DDevice9::SetTextureStageState",4,(uint32_t[]){dev,0,32,0}); /* CONSTANT=0 */
    draw_quad(dev,*pVB,0xFFFFFFFFu);
    { uint32_t got=center(); ok("CONSTANT 0", got==0x00000000u); if(got!=0x00000000u) printf("  got 0x%08X\n", got); }

    /* MODULATE2X: tex_half(0xFF804020) * diffuse(0xFF804020) * 2
       R: 128/255*128/255*2 = 0.5039 -> 0x80/0x81, G: 64/255*64/255*2 = 0.1260 -> 0x20, B: 32/255*32/255*2 = 0.0315 -> 0x08 */
    call("IDirect3DDevice9::SetTexture",3,(uint32_t[]){dev,0,tex_half});
    set_stage(dev,0,5,2,0,2,2,0); /* MODULATE2X TEXTURE DIFFUSE */
    draw_quad(dev,*pVB,0xFF804020u);
    { uint32_t got=center(); ok("MODULATE2X", got==0xFF802008u || got==0xFF812008u); if(!(got==0xFF802008u||got==0xFF812008u)) printf("  got 0x%08X\n", got); }

    /* ADDSIGNED: arg1+arg2-0.5. tex_half(0xFF804020)+black(0xFF000000)-0.5 => R=0.002 G<0 B<0 => 0xFF000000 */
    set_stage(dev,0,8,2,0,2,2,0); /* ADDSIGNED TEXTURE DIFFUSE */
    draw_quad(dev,*pVB,0xFF000000u);
    { uint32_t got=center(); ok("ADDSIGNED", got==0xFF000000u || got==0xFF010000u); if(!(got==0xFF000000u||got==0xFF010000u)) printf("  got 0x%08X\n", got); }

    /* MULTIPLYADD: arg0 + arg1*arg2. CURRENT=diffuse 0xFF101010, TEXTURE=tex_half 0xFF804020, ARG2? set ARG0=CURRENT,TEXTURE,DIFFUSE */
    call("IDirect3DDevice9::SetTextureStageState",4,(uint32_t[]){dev,0,1,25}); /* MULTIPLYADD */
    call("IDirect3DDevice9::SetTextureStageState",4,(uint32_t[]){dev,0,26,1}); /* COLORARG0 CURRENT */
    call("IDirect3DDevice9::SetTextureStageState",4,(uint32_t[]){dev,0,2,2});  /* COLORARG1 TEXTURE */
    call("IDirect3DDevice9::SetTextureStageState",4,(uint32_t[]){dev,0,3,0});  /* COLORARG2 DIFFUSE */
    call("IDirect3DDevice9::SetTextureStageState",4,(uint32_t[]){dev,0,4,2});  /* ALPHAOP SELECTARG1 */
    call("IDirect3DDevice9::SetTextureStageState",4,(uint32_t[]){dev,0,5,2});
    draw_quad(dev,*pVB,0xFF101010u); /* diffuse also arg2: 0x10+0x80*0x10/255 ~ 0x10+8=0x18 */
    { uint32_t got=center(); ok("MULTIPLYADD", (got&0xFF0000u)>=0x170000u && (got&0xFF0000u)<=0x190000u); if(!((got&0xFF0000u)>=0x170000u && (got&0xFF0000u)<=0x190000u)) printf("  got 0x%08X\n", got); }

    /* TEMP register: stage0 SELECTARG1 DIFFUSE -> TEMP, stage1 SELECTARG1 TEMP */
    set_stage(dev,0,3,0,0,3,0,0); /* stage0 SELECTARG2 DIFFUSE -> current = diffuse */
    call("IDirect3DDevice9::SetTextureStageState",4,(uint32_t[]){dev,0,28,5}); /* RESULTARG TEMP */
    set_stage(dev,1,2,5,0,2,5,0); /* stage1 SELECTARG1 TEMP */
    call("IDirect3DDevice9::SetTexture",3,(uint32_t[]){dev,1,tex_white});
    draw_quad(dev,*pVB,0xFF336699u);
    { uint32_t got=center(); ok("TEMP result/arg", got==0xFF336699u); if(got!=0xFF336699u) printf("  got 0x%08X\n", got); }
    call("IDirect3DDevice9::SetTextureStageState",4,(uint32_t[]){dev,0,28,1}); /* reset RESULTARG CURRENT */
    set_stage(dev,1,1,2,0,1,2,0); /* disable stage1 */

    /* ALPHAREPLICATE: TEXTURE alpha replicated to rgb. texture alpha=0x80 => rgb ~0x80 */
    {
        uint32_t tex_a = make_tex(dev, 0x80FFFFFFu);
        call("IDirect3DDevice9::SetTexture",3,(uint32_t[]){dev,0,tex_a});
        set_stage(dev,0,2,2|0x20,0,2,2,0); /* SELECTARG1 TEXTURE|ALPHAREPLICATE */
        draw_quad(dev,*pVB,0xFFFFFFFFu);
        { uint32_t got=center(); ok("ALPHAREPLICATE", (got&0xFF0000u)>=0x7F0000u && (got&0xFF0000u)<=0x810000u); if(!((got&0xFF0000u)>=0x7F0000u && (got&0xFF0000u)<=0x810000u)) printf("  got 0x%08X\n", got); }
        call("IDirect3DTexture9::Release",1,(uint32_t[]){tex_a});
    }

    /* DISABLE chain: stage0 DISABLE means subsequent stages ignored, result should be diffuse/current */
    call("IDirect3DDevice9::SetTexture",3,(uint32_t[]){dev,0,tex_red});
    set_stage(dev,0,1,2,0,1,2,0); /* stage0 DISABLE */
    set_stage(dev,1,2,2,0,2,2,0); /* stage1 would select texture, but must be skipped */
    draw_quad(dev,*pVB,0xFF00FF00u);
    { uint32_t got=center(); ok("DISABLE chain", got==0xFF00FF00u); if(got!=0xFF00FF00u) printf("  got 0x%08X\n", got); }
    set_stage(dev,1,1,2,0,1,2,0);

    call("IDirect3DTexture9::Release",1,(uint32_t[]){tex_white});
    call("IDirect3DTexture9::Release",1,(uint32_t[]){tex_red});
    call("IDirect3DTexture9::Release",1,(uint32_t[]){tex_half});
    call("IDirect3DVertexBuffer9::Release",1,(uint32_t[]){*pVB});
    printf(fails==0 ? "test_d3d9_ffp_texture_stages_ops: OK\n" : "test_d3d9_ffp_texture_stages_ops: FAILED\n");
    return fails != 0;
}
