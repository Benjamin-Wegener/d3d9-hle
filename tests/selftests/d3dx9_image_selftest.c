/*
 * d3dx9_image_selftest.c - file/memory image API coverage for D3DX9.
 * Hand-expectation tests: sizes/formats/pixel values are derived from the
 * crafted input bytes, not from the code under test.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "win32hle.h"
#include "host.h"
#include "d3d9_hle.h"
#include "d3d9_format.h"
#include "d3dx9_hle.h"
#include "d3dx9_texture.h"
#include "d3dx9_surface.h"

const recomp_dispatch_entry_t recomp_dispatch_table[1];
const uint32_t recomp_dispatch_count = 0;

static int fails;
static void ok(const char *what, int cond) { if (!cond) { printf("FAIL %s\n", what); fails++; } }
static uint32_t call(const char *n, int c, const uint32_t *a) { return hle_call_guest(hle_resolve(n), c, a); }
#define A(p) hle_va(p)
static void put16(uint8_t *b,uint16_t v){b[0]=(uint8_t)v;b[1]=(uint8_t)(v>>8);}
static void put32(uint8_t *b,uint32_t v){b[0]=(uint8_t)v;b[1]=(uint8_t)(v>>8);b[2]=(uint8_t)(v>>16);b[3]=(uint8_t)(v>>24);}

static void write_file(const char *path, const void *data, size_t sz) {
    FILE *f=fopen(path,"wb"); if(!f){perror(path);exit(2);} fwrite(data,1,sz,f); fclose(f);
}

static void make_bmp2(uint8_t *d){
    memset(d,0,70); memcpy(d,"BM",2); put32(d+2,70); put32(d+10,54); put32(d+14,40);
    put32(d+18,2); put32(d+22,2); put16(d+26,1); put16(d+28,24); put32(d+30,0);
    put32(d+34,16);                                             /* BI_RGB image size */
    memcpy(d+54,(const uint8_t[6]){0xFF,0x00,0x00,0x00,0xFF,0xFF},6); /* bottom: blue,yellow */
    memcpy(d+62,(const uint8_t[6]){0x00,0x00,0xFF,0x00,0xFF,0x00},6); /* top: red,green */
}
static void make_tga2(uint8_t *d){
    memset(d,0,30); d[2]=2; put16(d+12,2); put16(d+14,2); d[16]=24;
    memcpy(d+18,(const uint8_t[6]){0xFF,0x00,0x00,0x00,0xFF,0xFF},6);
    memcpy(d+24,(const uint8_t[6]){0x00,0x00,0xFF,0x00,0xFF,0x00},6);
}
/* DDS 4x4 DXT1 with 3 mip levels: 4x4 (8 B) + 2x2 (8 B) + 1x1 (8 B).
 * Every level has a different DXT1 endpoint colour so a test can tell them
 * apart: level 0 red, level 1 green, level 2 blue. */
static void make_dds_dxt1_mips(uint8_t *d){
    memset(d,0,152);
    memcpy(d,"DDS ",4); put32(d+4,124);
    put32(d+8,0x1|0x2|0x4|0x1000|0x20000); /* CAPS HEIGHT WIDTH PIXELFORMAT MIPMAPCOUNT */
    put32(d+12,4); put32(d+16,4); put32(d+20,8); put32(d+28,3);
    put32(d+76,32); put32(d+80,0x4); memcpy(d+84,"DXT1",4);
    put32(d+108,0x1000|0x400008); /* TEXTURE|MIPMAP (as a DDS saver writes it) */
    /* level 0 4x4: c0 red, c1 green */
    memcpy(d+128,(const uint8_t[8]){0x00,0xF8,0xE0,0x07,0xE4,0x00,0x00,0x00},8);
    /* level 1 2x2: c0 green, c1 blue */
    memcpy(d+136,(const uint8_t[8]){0x00,0x07,0x1F,0x00,0x1B,0x00,0x00,0x00},8);
    /* level 2 1x1: c0 blue */
    memcpy(d+144,(const uint8_t[8]){0x1F,0x00,0x00,0x00,0x00,0x00,0x00,0x00},8);
}
/* DDS cubemap 2x2 DXT1, all 6 faces, 2 mip levels (2x2 + 1x1 per face).
 * Level 0 and level 1 of each face get different endpoint colours, and the
 * six faces differ too, so an unuploaded face/level cannot pass by accident. */
static void make_dds_cube(uint8_t *d){
    /* header 128 + 6 faces * (8 + 8) = 224 */
    static const uint16_t c0_l0[6] = {0xF800,0x07E0,0x001F,0xFFFF,0xF800,0x07E0};
    static const uint16_t c0_l1[6] = {0x001F,0xFFFF,0xF800,0x07E0,0x001F,0xFFFF};
    memset(d,0,224);
    memcpy(d,"DDS ",4); put32(d+4,124);
    put32(d+8,0x1|0x2|0x4|0x1000|0x20000);
    put32(d+12,2); put32(d+16,2); put32(d+20,8); put32(d+28,2);
    put32(d+76,32); put32(d+80,0x4); memcpy(d+84,"DXT1",4);
    put32(d+108,0x1000|0x8|0x400000); /* TEXTURE|COMPLEX|MIPMAP */
    put32(d+112,0x200|0xFC00); /* CUBEMAP|all faces */
    for(int face=0; face<6; face++){
        uint8_t *b=d+128+face*16;
        for (int level=0; level<2; level++) {
            uint16_t c0 = level ? c0_l1[face] : c0_l0[face];
            uint8_t *blk = b + level*8;
            blk[0]=(uint8_t)c0; blk[1]=(uint8_t)(c0>>8);
            for (int i=2;i<8;i++) blk[i]=0;
        }
    }
}

static uint32_t make_device(void){
    uint32_t *ppd3d = (uint32_t*)hle_alloc(4);
    uint32_t hr = call("Direct3DCreate9",1,(uint32_t[]){32});
    ok("Direct3DCreate9", hr!=0);
    /* CreateDevice: adapter,devtype,hwnd,flags,pp,ppdev */
    D3DPRESENT_PARAMETERS *pp=(D3DPRESENT_PARAMETERS*)hle_alloc(sizeof(*pp));
    memset(pp,0,sizeof(*pp)); pp->BackBufferWidth=64; pp->BackBufferHeight=64; pp->BackBufferFormat=D3DFMT_A8R8G8B8;
    pp->SwapEffect=1; pp->Windowed=1; pp->hDeviceWindow=0;
    uint32_t *pdev=(uint32_t*)hle_alloc(4); *pdev=0;
    hr = call("IDirect3D9::CreateDevice",7,(uint32_t[]){hr,0,1,0,0x20u,A(pp),A(pdev)});
    ok("CreateDevice", hr==D3D_OK && *pdev);
    uint32_t dev=*pdev; hle_free(pp); hle_free(pdev); hle_free(ppd3d); return dev;
}

static void check_pixels(const char *what, uint32_t tex, const uint32_t *exp, int n){
    D3DLOCKED_RECT *lr=(D3DLOCKED_RECT*)hle_alloc(sizeof(*lr));
    uint32_t hr=call("IDirect3DTexture9::LockRect",5,(uint32_t[]){tex,0,A(lr),0,0});
    if(hr!=D3D_OK||!lr->pBits){ ok(what,0); hle_free(lr); return; }
    const uint32_t *p=(const uint32_t*)hle_ptr(lr->pBits);
    int same=1; for(int i=0;i<n;i++) if(p[i]!=exp[i]) same=0;
    ok(what,same);
    call("IDirect3DTexture9::UnlockRect",2,(uint32_t[]){tex,0});
    hle_free(lr);
}

/* Read back one compressed DXT1 level through the public texture lock API and
 * compare all eight bytes with the values crafted into the source file. */
static void check_dds_level(const char *what, uint32_t tex, uint32_t level,
                            const uint8_t exp[8]){
    D3DLOCKED_RECT *lr=(D3DLOCKED_RECT*)hle_alloc(sizeof(*lr));
    uint32_t hr=call("IDirect3DTexture9::LockRect",5,(uint32_t[]){tex,level,A(lr),0,0});
    if(hr!=D3D_OK||!lr->pBits){ ok(what,0); hle_free(lr); return; }
    const uint8_t *p=(const uint8_t*)hle_ptr(lr->pBits);
    int same=1; for(int i=0;i<8;i++) if(p[i]!=exp[i]) same=0;
    ok(what,same);
    call("IDirect3DTexture9::UnlockRect",2,(uint32_t[]){tex,level});
    hle_free(lr);
}

/* Same for one face/level of a cube texture (LockRect has face and level). */
static void check_cube_block(const char *what, uint32_t cube, uint32_t face,
                             uint32_t level, uint16_t exp_c0){
    D3DLOCKED_RECT *lr=(D3DLOCKED_RECT*)hle_alloc(sizeof(*lr));
    uint32_t hr=call("IDirect3DCubeTexture9::LockRect",6,
                     (uint32_t[]){cube,face,level,A(lr),0,0});
    if(hr!=D3D_OK||!lr->pBits){ ok(what,0); hle_free(lr); return; }
    const uint8_t *p=(const uint8_t*)hle_ptr(lr->pBits);
    /* c0 is the first DXT1 endpoint; the crafted blocks have zero elsewhere. */
    uint16_t got_c0=(uint16_t)(p[0]|((uint16_t)p[1]<<8));
    int same=(got_c0==exp_c0);
    for(int i=2;i<8;i++) if(p[i]!=0) same=0;
    ok(what,same);
    call("IDirect3DCubeTexture9::UnlockRect",3,(uint32_t[]){cube,face,level});
    hle_free(lr);
}

static int file_equals(const char *path, const uint8_t *exp, size_t n){
    FILE *f=fopen(path,"rb"); if(!f) return 0;
    uint8_t buf[512];
    size_t got=fread(buf,1,sizeof buf,f);
    fclose(f);
    return got==n && memcmp(buf,exp,n)==0;
}

int main(void){
    recomp_host_init();
    win32hle_register(win32hle_d3d9);
    win32hle_register(win32hle_d3dx9);

    static const uint32_t exp[4]={0xFFFF0000u,0xFF00FF00u,0xFF0000FFu,0xFFFFFF00u};
    /* Independently written expectations for the crafted DDS mip blocks. */
    static const uint8_t exp_dds0[8]={0x00,0xF8,0xE0,0x07,0xE4,0x00,0x00,0x00};
    static const uint8_t exp_dds1[8]={0x00,0x07,0x1F,0x00,0x1B,0x00,0x00,0x00};
    static const uint8_t exp_dds2[8]={0x1F,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
    static const uint16_t exp_cube_l0[6]={0xF800,0x07E0,0x001F,0xFFFF,0xF800,0x07E0};
    static const uint16_t exp_cube_l1[6]={0x001F,0xFFFF,0xF800,0x07E0,0x001F,0xFFFF};
    uint8_t bmp[70], tga[30], dds[152], cube[224];
    make_bmp2(bmp); make_tga2(tga); make_dds_dxt1_mips(dds); make_dds_cube(cube);
    uint8_t *bmp_g=(uint8_t*)hle_alloc(sizeof bmp); memcpy(bmp_g,bmp,sizeof bmp);
    uint8_t *tga_g=(uint8_t*)hle_alloc(sizeof tga); memcpy(tga_g,tga,sizeof tga);
    uint8_t *dds_g=(uint8_t*)hle_alloc(sizeof dds); memcpy(dds_g,dds,sizeof dds);
    uint8_t *cube_g=(uint8_t*)hle_alloc(sizeof cube); memcpy(cube_g,cube,sizeof cube);
    write_file("/tmp/d3dx9_img_test.bmp", bmp, sizeof bmp);
    write_file("/tmp/d3dx9_img_test.tga", tga, sizeof tga);
    write_file("/tmp/d3dx9_img_test.dds", dds, sizeof dds);
    write_file("/tmp/d3dx9_img_cube.dds", cube, sizeof cube);
    write_file("/tmp/d3dx9_img_bad.dat", "garbage", 7);
    /* broken/truncated files for ASan/robustness */
    write_file("/tmp/d3dx9_img_trunc.dds", dds, 40);
    write_file("/tmp/d3dx9_img_trunc.bmp", bmp, 20);

    uint32_t dev = make_device();

    /* GetImageInfoFromFileA */
    d3dx9_texture_info *info=(d3dx9_texture_info*)hle_alloc(sizeof(*info));
    memset(info,0xCC,sizeof(*info));
    uint32_t hr = call("D3DXGetImageInfoFromFileA",2,(uint32_t[]){hle_guest_strdup("/tmp/d3dx9_img_test.bmp"),A(info)});
    ok("GetImageInfoFromFileA bmp hr", hr==D3D_OK);
    ok("GetImageInfoFromFileA bmp w", info->Width==2);
    ok("GetImageInfoFromFileA bmp h", info->Height==2);
    ok("GetImageInfoFromFileA bmp fmt", info->Format==D3DFMT_A8R8G8B8);
    ok("GetImageInfoFromFileA bmp iff", info->ImageFileFormat==D3DXIFF_BMP);

    /* GetImageInfoFromFileW */
    static const uint16_t wpath[]={'/','t','m','p','/','d','3','d','x','9','_','i','m','g','_','t','e','s','t','.','t','g','a',0};
    uint16_t *wpath_g=(uint16_t*)hle_alloc(sizeof(wpath)); memcpy(wpath_g,wpath,sizeof(wpath));
    memset(info,0,sizeof(*info));
    hr = call("D3DXGetImageInfoFromFileW",2,(uint32_t[]){A(wpath_g),A(info)});
    ok("GetImageInfoFromFileW tga hr", hr==D3D_OK);
    ok("GetImageInfoFromFileW tga fmt", info->ImageFileFormat==D3DXIFF_TGA);

    /* GetImageInfoFromFileA DDS with mips */
    memset(info,0,sizeof(*info));
    hr = call("D3DXGetImageInfoFromFileA",2,(uint32_t[]){hle_guest_strdup("/tmp/d3dx9_img_test.dds"),A(info)});
    ok("GetImageInfoFromFileA dds hr", hr==D3D_OK);
    ok("GetImageInfoFromFileA dds mips", info->MipLevels==3);
    ok("GetImageInfoFromFileA dds fmt", info->Format==D3DFMT_DXT1);

    /* broken files: honest error */
    hr = call("D3DXGetImageInfoFromFileA",2,(uint32_t[]){hle_guest_strdup("/tmp/d3dx9_img_bad.dat"),A(info)});
    ok("GetImageInfoFromFileA bad err", hr!=D3D_OK);
    hr = call("D3DXGetImageInfoFromFileA",2,(uint32_t[]){hle_guest_strdup("/tmp/d3dx9_img_trunc.dds"),A(info)});
    ok("GetImageInfoFromFileA trunc err", hr!=D3D_OK);

    /* CreateTextureFromFileExA - BMP, explicit size/defaults */
    uint32_t *ptex=(uint32_t*)hle_alloc(4); *ptex=0;
    hr = call("D3DXCreateTextureFromFileExA",14,(uint32_t[]){
        dev,hle_guest_strdup("/tmp/d3dx9_img_test.bmp"),D3DX_DEFAULT,D3DX_DEFAULT,D3DX_DEFAULT,0,
        D3DFMT_UNKNOWN,D3DPOOL_MANAGED,D3DX_FILTER_NONE,D3DX_FILTER_NONE,0,A(info),0,A(ptex)});
    ok("CreateTextureFromFileExA bmp hr", hr==D3D_OK && *ptex);
    check_pixels("CreateTextureFromFileExA bmp pixels", *ptex, exp, 4);
    call("IDirect3DTexture9::Release",1,(uint32_t[]){*ptex});

    /* CreateTextureFromFileExW - TGA */
    *ptex=0;
    hr = call("D3DXCreateTextureFromFileExW",14,(uint32_t[]){
        dev,A(wpath_g),D3DX_DEFAULT,D3DX_DEFAULT,D3DX_DEFAULT,0,
        D3DFMT_UNKNOWN,D3DPOOL_MANAGED,D3DX_FILTER_NONE,D3DX_FILTER_NONE,0,0,0,A(ptex)});
    ok("CreateTextureFromFileExW tga hr", hr==D3D_OK && *ptex);
    check_pixels("CreateTextureFromFileExW tga pixels", *ptex, exp, 4);
    call("IDirect3DTexture9::Release",1,(uint32_t[]){*ptex});

    /* DDS all mip levels */
    *ptex=0;
    hr = call("D3DXCreateTextureFromFileExA",14,(uint32_t[]){
        dev,hle_guest_strdup("/tmp/d3dx9_img_test.dds"),D3DX_DEFAULT,D3DX_DEFAULT,D3DX_DEFAULT,0,
        D3DFMT_UNKNOWN,D3DPOOL_MANAGED,D3DX_FILTER_NONE,D3DX_FILTER_NONE,0,A(info),0,A(ptex)});
    ok("CreateTextureFromFileExA dds hr", hr==D3D_OK && *ptex);
    ok("CreateTextureFromFileExA dds info mips", info->MipLevels==3);
    uint32_t levels = call("IDirect3DTexture9::GetLevelCount",1,(uint32_t[]){*ptex});
    ok("CreateTextureFromFileExA dds levels", levels==3);
    /* Every DXT1 level must contain its own crafted colour (red/green/blue). */
    check_dds_level("CreateTextureFromFileExA dds level0 red", *ptex, 0, exp_dds0);
    check_dds_level("CreateTextureFromFileExA dds level1 green", *ptex, 1, exp_dds1);
    check_dds_level("CreateTextureFromFileExA dds level2 blue", *ptex, 2, exp_dds2);
    call("IDirect3DTexture9::Release",1,(uint32_t[]){*ptex});

    /* CreateCubeTextureFromFileA/W */
    uint32_t *pcube=(uint32_t*)hle_alloc(4); *pcube=0;
    hr = call("D3DXCreateCubeTextureFromFileA",3,(uint32_t[]){dev,hle_guest_strdup("/tmp/d3dx9_img_cube.dds"),A(pcube)});
    ok("CreateCubeTextureFromFileA hr", hr==D3D_OK && *pcube);
    levels = call("IDirect3DCubeTexture9::GetLevelCount",1,(uint32_t[]){*pcube});
    ok("CreateCubeTextureFromFileA levels", levels==2);
    D3DSURFACE_DESC *desc=(D3DSURFACE_DESC*)hle_alloc(sizeof(*desc));
    hr = call("IDirect3DCubeTexture9::GetLevelDesc",3,(uint32_t[]){*pcube,0,A(desc)});
    ok("CreateCubeTextureFromFileA desc hr", hr==D3D_OK);
    ok("CreateCubeTextureFromFileA size", desc->Width==2 && desc->Height==2);
    ok("CreateCubeTextureFromFileA fmt", desc->Format==D3DFMT_DXT1);
    /* All six faces and both mip levels must hold their own crafted colour. */
    for (int face=0; face<6; face++) {
        char name[96];
        snprintf(name,sizeof name,"CreateCubeTextureFromFileA face%d level0 c0",face);
        check_cube_block(name,*pcube,(uint32_t)face,0,exp_cube_l0[face]);
        snprintf(name,sizeof name,"CreateCubeTextureFromFileA face%d level1 c0",face);
        check_cube_block(name,*pcube,(uint32_t)face,1,exp_cube_l1[face]);
    }
    call("IDirect3DCubeTexture9::Release",1,(uint32_t[]){*pcube});
    static const uint16_t wcube[]={'/','t','m','p','/','d','3','d','x','9','_','i','m','g','_','c','u','b','e','.','d','d','s',0};
    uint16_t *wcube_g=(uint16_t*)hle_alloc(sizeof(wcube)); memcpy(wcube_g,wcube,sizeof(wcube));
    *pcube=0;
    hr = call("D3DXCreateCubeTextureFromFileW",3,(uint32_t[]){dev,A(wcube_g),A(pcube)});
    ok("CreateCubeTextureFromFileW hr", hr==D3D_OK && *pcube);
    check_cube_block("CreateCubeTextureFromFileW face3 level0 c0",*pcube,3,0,exp_cube_l0[3]);
    check_cube_block("CreateCubeTextureFromFileW face3 level1 c0",*pcube,3,1,exp_cube_l1[3]);
    call("IDirect3DCubeTexture9::Release",1,(uint32_t[]){*pcube});

    /* LoadSurfaceFromFileInMemory */
    uint32_t *psurf=(uint32_t*)hle_alloc(4); *psurf=0;
    hr = call("IDirect3DDevice9::CreateOffscreenPlainSurface",6,(uint32_t[]){dev,2,2,D3DFMT_A8R8G8B8,D3DPOOL_SCRATCH,A(psurf),0});
    ok("CreateOffscreenPlainSurface", hr==D3D_OK && *psurf);
    D3DRECT *rct=(D3DRECT*)hle_alloc(sizeof(*rct)); rct->x1=0;rct->y1=0;rct->x2=2;rct->y2=2;
    hr = call("D3DXLoadSurfaceFromFileInMemory",9,(uint32_t[]){*psurf,0,0,A(bmp_g),sizeof bmp,A(rct),D3DX_FILTER_NONE,0,0});
    ok("LoadSurfaceFromFileInMemory bmp hr", hr==D3D_OK);
    D3DLOCKED_RECT *lr=(D3DLOCKED_RECT*)hle_alloc(sizeof(*lr));
    hr = call("IDirect3DSurface9::LockRect",4,(uint32_t[]){*psurf,A(lr),0,0});
    if (hr==D3D_OK) {
        const uint32_t *p=(const uint32_t*)hle_ptr(lr->pBits);
        int same=1; for(int i=0;i<4;i++) if(p[i]!=exp[i]) same=0;
        ok("LoadSurfaceFromFileInMemory pixels", same);
        call("IDirect3DSurface9::UnlockRect",1,(uint32_t[]){*psurf});
    } else ok("LoadSurfaceFromFileInMemory lock",0);
    call("IDirect3DSurface9::Release",1,(uint32_t[]){*psurf});

    /* LoadSurfaceFromFileA */
    *psurf=0;
    hr = call("IDirect3DDevice9::CreateOffscreenPlainSurface",6,(uint32_t[]){dev,2,2,D3DFMT_A8R8G8B8,D3DPOOL_SCRATCH,A(psurf),0});
    ok("CreateOffscreenPlainSurface2", hr==D3D_OK && *psurf);
    hr = call("D3DXLoadSurfaceFromFileA",8,(uint32_t[]){*psurf,0,0,hle_guest_strdup("/tmp/d3dx9_img_test.tga"),A(rct),D3DX_FILTER_NONE,0,0});
    ok("LoadSurfaceFromFileA tga hr", hr==D3D_OK);
    hr = call("IDirect3DSurface9::LockRect",4,(uint32_t[]){*psurf,A(lr),0,0});
    if (hr==D3D_OK) {
        const uint32_t *p=(const uint32_t*)hle_ptr(lr->pBits);
        int same=1; for(int i=0;i<4;i++) if(p[i]!=exp[i]) same=0;
        ok("LoadSurfaceFromFileA pixels", same);
        call("IDirect3DSurface9::UnlockRect",1,(uint32_t[]){*psurf});
    } else ok("LoadSurfaceFromFileA lock",0);
    call("IDirect3DSurface9::Release",1,(uint32_t[]){*psurf});

    /* LoadVolumeFromMemory */
    uint32_t *pvoltex=(uint32_t*)hle_alloc(4); *pvoltex=0;
    hr = call("D3DXCreateVolumeTexture",9,(uint32_t[]){dev,2,2,2,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,A(pvoltex)});
    ok("CreateVolumeTexture for load", hr==D3D_OK && *pvoltex);
    uint32_t *pvol=(uint32_t*)hle_alloc(4); *pvol=0;
    hr = call("IDirect3DVolumeTexture9::GetVolumeLevel",3,(uint32_t[]){*pvoltex,0,A(pvol)});
    ok("GetVolumeLevel", hr==D3D_OK && *pvol);
    uint32_t voldata[8];
    for(int i=0;i<8;i++) voldata[i]=0xFF000000u|(uint32_t)(i*0x102030);
    uint32_t *voldata_g=(uint32_t*)hle_alloc(sizeof voldata); memcpy(voldata_g,voldata,sizeof voldata);
    D3DBOX *box=(D3DBOX*)hle_alloc(sizeof(*box)); box->Left=0;box->Top=0;box->Right=2;box->Bottom=2;box->Front=0;box->Back=2;
    hr = call("D3DXLoadVolumeFromMemory",11,(uint32_t[]){*pvol,0,0,A(voldata_g),D3DFMT_A8R8G8B8,8,16,0,A(box),D3DX_FILTER_NONE,0});
    ok("LoadVolumeFromMemory hr", hr==D3D_OK);
    D3DLOCKED_BOX *lb=(D3DLOCKED_BOX*)hle_alloc(sizeof(*lb));
    hr = call("IDirect3DVolume9::LockBox",4,(uint32_t[]){*pvol,A(lb),0,0});
    if(hr==D3D_OK){
        int same=1;
        for(int z=0;z<2;z++) for(int y=0;y<2;y++) for(int x=0;x<2;x++){
            uint32_t got = *(uint32_t*)((uint8_t*)hle_ptr(lb->pBits)+z*(uint32_t)lb->SlicePitch+y*(uint32_t)lb->RowPitch+x*4);
            if(got!=voldata[z*4+y*2+x]) same=0;
        }
        ok("LoadVolumeFromMemory data", same);
        call("IDirect3DVolume9::UnlockBox",1,(uint32_t[]){*pvol});
    } else ok("LoadVolumeFromMemory lock",0);
    call("IDirect3DVolume9::Release",1,(uint32_t[]){*pvol});
    call("IDirect3DVolumeTexture9::Release",1,(uint32_t[]){*pvoltex});

    /* SaveTextureToFileA - DDS roundtrip all levels */
    *ptex=0;
    hr = call("D3DXCreateTextureFromFileExA",14,(uint32_t[]){
        dev,hle_guest_strdup("/tmp/d3dx9_img_test.dds"),D3DX_DEFAULT,D3DX_DEFAULT,D3DX_DEFAULT,0,
        D3DFMT_UNKNOWN,D3DPOOL_MANAGED,D3DX_FILTER_NONE,D3DX_FILTER_NONE,0,0,0,A(ptex)});
    ok("SaveTextureToFileA src", hr==D3D_OK && *ptex);
    hr = call("D3DXSaveTextureToFileA",4,(uint32_t[]){hle_guest_strdup("/tmp/d3dx9_img_out.dds"),D3DXIFF_DDS,*ptex,0});
    ok("SaveTextureToFileA dds hr", hr==D3D_OK);
    /* Compare the complete saved DDS with the independently crafted file:
     * header fields plus all three DXT1 level blocks, byte for byte. */
    ok("SaveTextureToFileA dds all bytes", file_equals("/tmp/d3dx9_img_out.dds", dds, sizeof dds));
    memset(info,0,sizeof(*info));
    hr = call("D3DXGetImageInfoFromFileA",2,(uint32_t[]){hle_guest_strdup("/tmp/d3dx9_img_out.dds"),A(info)});
    ok("SaveTextureToFileA dds info hr", hr==D3D_OK);
    ok("SaveTextureToFileA dds mips", info->MipLevels==3);
    ok("SaveTextureToFileA dds fmt", info->Format==D3DFMT_DXT1);
    call("IDirect3DTexture9::Release",1,(uint32_t[]){*ptex});

    /* SaveTextureToFileA BMP */
    *ptex=0;
    hr = call("D3DXCreateTextureFromFileExA",14,(uint32_t[]){
        dev,hle_guest_strdup("/tmp/d3dx9_img_test.bmp"),D3DX_DEFAULT,D3DX_DEFAULT,D3DX_DEFAULT,0,
        D3DFMT_UNKNOWN,D3DPOOL_MANAGED,D3DX_FILTER_NONE,D3DX_FILTER_NONE,0,0,0,A(ptex)});
    hr = call("D3DXSaveTextureToFileA",4,(uint32_t[]){hle_guest_strdup("/tmp/d3dx9_img_out.bmp"),D3DXIFF_BMP,*ptex,0});
    ok("SaveTextureToFileA bmp hr", hr==D3D_OK);
    /* The crafted BMP is itself a complete, spec-conformant expectation:
     * 54-byte header, two bottom-up BGR rows and two zero padding bytes.
     * Nothing less than byte-identical output may pass. */
    ok("SaveTextureToFileA bmp all bytes", file_equals("/tmp/d3dx9_img_out.bmp", bmp, sizeof bmp));
    call("IDirect3DTexture9::Release",1,(uint32_t[]){*ptex});

    /* unsupported filter: honest error, never a silent OK */
    *ptex=0;
    hr = call("D3DXCreateTextureFromFileExA",14,(uint32_t[]){
        dev,hle_guest_strdup("/tmp/d3dx9_img_test.bmp"),D3DX_DEFAULT,D3DX_DEFAULT,D3DX_DEFAULT,0,
        D3DFMT_UNKNOWN,D3DPOOL_MANAGED,D3DX_FILTER_TRIANGLE,D3DX_FILTER_NONE,0,0,0,A(ptex)});
    ok("unsupported Filter err", hr==D3DERR_NOTAVAILABLE && *ptex==0);
    *ptex=0;
    hr = call("D3DXCreateTextureFromFileExA",14,(uint32_t[]){
        dev,hle_guest_strdup("/tmp/d3dx9_img_test.bmp"),D3DX_DEFAULT,D3DX_DEFAULT,D3DX_DEFAULT,0,
        D3DFMT_UNKNOWN,D3DPOOL_MANAGED,D3DX_FILTER_NONE,D3DX_FILTER_TRIANGLE,0,0,0,A(ptex)});
    ok("unsupported MipFilter err", hr==D3DERR_NOTAVAILABLE && *ptex==0);

    /* unsupported format: honest error */
    hr = call("D3DXGetImageInfoFromFileA",2,(uint32_t[]){hle_guest_strdup("/tmp/d3dx9_img_bad.dat"),A(info)});
    ok("unsupported format err", hr!=D3D_OK);

    hle_free(info); hle_free(ptex); hle_free(pcube); hle_free(psurf); hle_free(rct); hle_free(lr); hle_free(pvoltex); hle_free(pvol); hle_free(box); hle_free(lb); hle_free(desc);

    if (fails) { printf("d3dx9_image_selftest: %d FAILS\n", fails); return 1; }
    printf("d3dx9_image_selftest: all tests passed\n");
    return 0;
}
