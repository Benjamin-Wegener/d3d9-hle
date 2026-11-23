/*
 * d3dx9_texture_selftest.c - D3DX9 texture creation against a real device,
 * from self-crafted images: DDS with DXT1 (4x4), TGA 24-bit (2x2, odd
 * dims), BMP 24-bit (2x2), the public info structs, the requirements check,
 * and broken inputs (bad magics, truncated data, DXT2). All pixels are read
 * back from the D3D9 surfaces via IDirect3DTexture9::LockRect.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "win32hle.h"
#include "host.h"
#include "d3d9_hle.h"
#include "d3d9_format.h"
#include "d3dx9_texture.h"

const recomp_dispatch_entry_t recomp_dispatch_table[1];
const uint32_t recomp_dispatch_count = 0;

static int fails;
static void ok(const char *what, int cond) { if (!cond) { printf("FAIL %s\n", what); fails++; } }
static uint32_t call(const char *name, int n, const uint32_t *a) { return hle_call_guest(hle_resolve(name), n, a); }
static uint32_t fb(float f){ uint32_t u; memcpy(&u,&f,4); return u; }
#define A(p) hle_va(p)

static void put16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24); }

/* ---- DDS: magic + DDS_HEADER + one DXT1 block of a 4x4 texture. ---- */
/* DXT1 4x4: colour0 = 0xF800 (red), colour1 = 0x07E0 (green), pixel
 * indices 0,1,2,3 (LSB first -> 0xE4). */
#define G_DDS_SIZE 136
static void make_dds_dxt1(uint8_t *d)
{
    memset(d, 0, G_DDS_SIZE);
    memcpy(d, "DDS ", 4);
    put32(d + 4, 124);                          /* dwSize */
    put32(d + 8, 0x1 | 0x2 | 0x4 | 0x1000);     /* CAPS|HEIGHT|WIDTH|PIXELFORMAT */
    put32(d + 12, 4);                           /* dwHeight */
    put32(d + 16, 4);                           /* dwWidth */
    put32(d + 24, 0);                           /* dwDepth */
    put32(d + 28, 1);                           /* dwMipMapCount */
    /* ddspf (file offset 76): dwSize, dwFlags=DDPF_FOURCC, dwFourCC='DXT1' */
    put32(d + 76, 32);
    put32(d + 80, 0x4);
    put32(d + 84, 0x31545844);
    memcpy(d + 128, "\x00\xF8\xE0\x07\xE4\x00\x00\x00", 8);
}

/* ---- TGA 24-bit 2x2, bottom-up, distinct colours. ---- */
#define G_TGA_SIZE 30
static void make_tga_2x2(uint8_t *d)
{
    memset(d, 0, G_TGA_SIZE);
    d[0] = 0x00;                                /* idLength */
    d[2] = 0x02;                                /* imageType: truecolor */
    put16(d + 12, 2);                           /* width */
    put16(d + 14, 2);                           /* height */
    d[16] = 24;                                 /* bitsPerPixel */
    d[17] = 0;                                  /* descriptor: bottom-up */
    /* file row 0 = image bottom row (0,1): blue, yellow (B,G,R) */
    memcpy(d + 18, (const uint8_t[6]){ 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF }, 6);
    /* file row 1 = image top row (0,0): red, green */
    memcpy(d + 24, (const uint8_t[6]){ 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00 }, 6);
}

/* ---- BMP 24-bit 2x2, bottom-up. ---- */
#define G_BMP_SIZE 70
static void make_bmp_2x2(uint8_t *d)
{
    memset(d, 0, G_BMP_SIZE);
    memcpy(d, "BM", 2);
    put32(d + 2, 70);                           /* bfSize */
    put32(d + 10, 54);                          /* bfOffBits */
    put32(d + 14, 40);                          /* biSize (BITMAPINFOHEADER) */
    put32(d + 18, 2);                           /* biWidth */
    put32(d + 22, 2);                           /* biHeight: positive = bottom-up */
    put16(d + 26, 1);                           /* biPlanes */
    put16(d + 28, 24);                          /* biBitCount */
    put32(d + 30, 0);                           /* biCompression: BI_RGB */
    /* pixel rows, bottom-up: row 0 = image bottom row (0,1): blue, yellow;
     * row 1 = image top row (0,0): red, green; B,G,R per pixel; rows are
     * 8 bytes with 2 bytes padding. */
    memcpy(d + 54, (const uint8_t[6]){ 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF }, 6);
    memcpy(d + 62, (const uint8_t[6]){ 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00 }, 6);
}

/* Expected A8R8G8B8 top-down pixels of the TGA and BMP test images:
 * (0,0)=red 0xFFFF0000, (1,0)=green 0xFF00FF00, (0,1)=blue 0xFF0000FF,
 * (1,1)=yellow 0xFFFFFF00. */
static const uint32_t exp_pixels[4] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFF00u };

/* FillTexture callback: (pOut, pTexCoord, pTexelSize, pData) stdcall void */
static void fill_cb(void) {
    uint32_t pOut=A32(0), pCoord=A32(1);
    float *out = (float*)hle_ptr(pOut);
    float *uv = (float*)hle_ptr(pCoord);
    /* deterministic: red=uv.x, green=uv.y, blue=0, alpha=1 */
    out[0]=uv[0]; out[1]=uv[1]; out[2]=0.0f; out[3]=1.0f;
    RETV(4);
}
static const win32hle_shim g_fill_shims[] = { { "test_fill_cb", fill_cb }, { 0, 0 } };

static uint8_t *g_dds, *g_tga, *g_bmp;

static int check_pixels_bgra(const char *what, uint32_t tex, const uint32_t *exp, int n)
{
    D3DLOCKED_RECT *lr = (D3DLOCKED_RECT *)hle_alloc(8);
    uint32_t a[5] = { tex, 0, A(lr), 0, 0 };
    uint32_t hr = call("IDirect3DTexture9::LockRect", 5, a);
    if (hr != D3D_OK || !lr->pBits) { ok(what, 0); return -1; }
    const uint32_t *pbits = hle_ptr(lr->pBits);
    int same = 1;
    for (int i = 0; i < n; i++) if (pbits[i] != exp[i]) same = 0;
    ok(what, same);
    uint32_t b[2] = { tex, 0 };
    call("IDirect3DTexture9::UnlockRect", 2, b);
    return same ? 0 : -1;
}

int main(void)
{
    recomp_host_init();
    win32hle_register(win32hle_d3d9);
    win32hle_register(g_fill_shims);

    uint32_t a_create[1] = { 32 };
    uint32_t d3d9 = call("Direct3DCreate9", 1, a_create);
    ok("Direct3DCreate9", d3d9 != 0);
    D3DPRESENT_PARAMETERS *pp = (D3DPRESENT_PARAMETERS *)hle_alloc(sizeof(*pp));
    memset(pp, 0, sizeof(*pp));
    pp->BackBufferWidth = 64;
    pp->BackBufferHeight = 64;
    pp->BackBufferFormat = D3DFMT_A8R8G8B8;
    pp->BackBufferCount = 1;
    uint32_t *pDev = (uint32_t *)hle_alloc(4);
    uint32_t a_cdev[7] = { d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, 0,
                           D3DCREATE_HARDWARE_VERTEXPROCESSING, A(pp), A(pDev) };
    uint32_t hr = call("IDirect3D9::CreateDevice", 7, a_cdev);
    ok("CreateDevice", hr == D3D_OK && *pDev != 0);
    uint32_t dev = *pDev;

    g_dds = (uint8_t *)hle_alloc(G_DDS_SIZE); make_dds_dxt1(g_dds);
    g_tga = (uint8_t *)hle_alloc(G_TGA_SIZE); make_tga_2x2(g_tga);
    g_bmp = (uint8_t *)hle_alloc(G_BMP_SIZE); make_bmp_2x2(g_bmp);

    /* ---- D3DXCreateTexture (shim, table) ---- */
    {
        uint32_t *ptex = (uint32_t *)hle_alloc(4);
        uint32_t a_ct[8] = { dev, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, A(ptex) };
        ok("D3DXCreateTexture(4x4)", call("D3DXCreateTexture", 8, a_ct) == D3D_OK && *ptex != 0);

        uint32_t a_ct2[8] = { dev, 3, 5, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, A(ptex) };
        ok("D3DXCreateTexture(3x5, odd)", call("D3DXCreateTexture", 8, a_ct2) == D3D_OK && *ptex != 0);

        uint32_t a_ct3[8] = { dev, 0, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, A(ptex) };
        ok("D3DXCreateTexture rejects w=0", call("D3DXCreateTexture", 8, a_ct3) != D3D_OK);

        uint32_t a_ct4[8] = { dev, 4, 4, 1, 0, 12345u, D3DPOOL_MANAGED, A(ptex) };
        ok("D3DXCreateTexture rejects unsupported fmt", call("D3DXCreateTexture", 8, a_ct4) != D3D_OK);

        uint32_t a_ct5[8] = { dev, 4, 4, D3DX_DEFAULT, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, A(ptex) };
        ok("D3DXCreateTexture MipLevels=D3DX_DEFAULT", call("D3DXCreateTexture", 8, a_ct5) == D3D_OK && *ptex != 0);
    }
    /* odd size + LockRect pitch (host API) */
    {
        uint32_t ht = 0;
        ok("d3dx9_texture_create(3x5)", d3dx9_texture_create(dev, 3, 5, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &ht) == 0 && ht != 0);
        D3DLOCKED_RECT *lr = (D3DLOCKED_RECT *)hle_alloc(8);
        uint32_t a[5] = { ht, 0, A(lr), 0, 0 };
        ok("LockRect odd", call("IDirect3DTexture9::LockRect", 5, a) == D3D_OK && lr->pBits != 0);
        ok("LockRect odd pitch", lr->Pitch == 3 * 4);
    }

    /* ---- D3DXCheckTextureRequirements (shim) ---- */
    {
        uint32_t *pw = (uint32_t *)hle_alloc(4), *ph = (uint32_t *)hle_alloc(4),
                 *pm = (uint32_t *)hle_alloc(4), *pf = (uint32_t *)hle_alloc(4);
        *pw = 0; *ph = 0; *pm = 0; *pf = D3DFMT_UNKNOWN;
        uint32_t a[7] = { dev, A(pw), A(ph), A(pm), 0, A(pf), D3DPOOL_MANAGED };
        ok("CheckTextureRequirements", call("D3DXCheckTextureRequirements", 7, a) == D3D_OK);
        ok("CheckTextureRequirements w=256", *pw == 256);
        ok("CheckTextureRequirements h=256", *ph == 256);
        ok("CheckTextureRequirements mips=full chain", *pm == 9);   /* 256x256 -> 9 levels */
        ok("CheckTextureRequirements fmt=A8R8G8B8", *pf == D3DFMT_A8R8G8B8);

        *pw = 0; *ph = 4;
        ok("CheckTextureRequirements square rule", call("D3DXCheckTextureRequirements", 7, a) == D3D_OK && *pw == 4 && *ph == 4);
    }

    /* ---- D3DXGetImageInfoFromFileInMemory (shim) ---- */
    {
        d3dx9_texture_info *info = (d3dx9_texture_info *)hle_alloc(sizeof *info);
        uint32_t a[3] = { A(g_tga), G_TGA_SIZE, A(info) };
        ok("GetImageInfo TGA 2x2", call("D3DXGetImageInfoFromFileInMemory", 3, a) == D3D_OK);
        ok("TGA info w/h", info->Width == 2 && info->Height == 2);
        ok("TGA info mips", info->MipLevels == 1);
        ok("TGA info fmt", info->Format == D3DFMT_A8R8G8B8);
        ok("TGA info type", info->ResourceType == D3DRTYPE_TEXTURE);
        ok("TGA info file fmt", info->ImageFileFormat == D3DXIFF_TGA);
    }
    {
        d3dx9_texture_info *info = (d3dx9_texture_info *)hle_alloc(sizeof *info);
        uint32_t a[3] = { A(g_bmp), G_BMP_SIZE, A(info) };
        ok("GetImageInfo BMP 2x2", call("D3DXGetImageInfoFromFileInMemory", 3, a) == D3D_OK);
        ok("BMP info w/h", info->Width == 2 && info->Height == 2);
        ok("BMP info fmt", info->Format == D3DFMT_A8R8G8B8);
        ok("BMP info file fmt", info->ImageFileFormat == D3DXIFF_BMP);
    }
    {
        d3dx9_texture_info *info = (d3dx9_texture_info *)hle_alloc(sizeof *info);
        uint32_t a[3] = { A(g_dds), G_DDS_SIZE, A(info) };
        ok("GetImageInfo DDS DXT1 4x4", call("D3DXGetImageInfoFromFileInMemory", 3, a) == D3D_OK);
        ok("DDS info w/h", info->Width == 4 && info->Height == 4);
        ok("DDS info mips", info->MipLevels == 1);
        ok("DDS info fmt", info->Format == D3DFMT_DXT1);
        ok("DDS info type", info->ResourceType == D3DRTYPE_TEXTURE);
        ok("DDS info file fmt", info->ImageFileFormat == D3DXIFF_DDS);
    }

    /* ---- D3DXCreateTextureFromFileInMemory (shim) ---- */
    {
        uint32_t *ptex = (uint32_t *)hle_alloc(4);
        uint32_t a[4] = { dev, A(g_tga), G_TGA_SIZE, A(ptex) };
        ok("CreateTextureFromFileInMemory(TGA)", call("D3DXCreateTextureFromFileInMemory", 4, a) == D3D_OK && *ptex != 0);
        check_pixels_bgra("TGA pixels read back", *ptex, exp_pixels, 4);
    }
    {
        uint32_t *ptex = (uint32_t *)hle_alloc(4);
        uint32_t a[4] = { dev, A(g_bmp), G_BMP_SIZE, A(ptex) };
        ok("CreateTextureFromFileInMemory(BMP)", call("D3DXCreateTextureFromFileInMemory", 4, a) == D3D_OK && *ptex != 0);
        check_pixels_bgra("BMP pixels read back", *ptex, exp_pixels, 4);
    }
    {
        /* DXT1 DDS: the raw block lands in the surface unchanged, and the
         * HLE's DXT1 decoder reads it back as the four known colours. */
        uint32_t *ptex = (uint32_t *)hle_alloc(4);
        uint32_t a[4] = { dev, A(g_dds), G_DDS_SIZE, A(ptex) };
        ok("CreateTextureFromFileInMemory(DDS DXT1)", call("D3DXCreateTextureFromFileInMemory", 4, a) == D3D_OK && *ptex != 0);
        D3DLOCKED_RECT *lr = (D3DLOCKED_RECT *)hle_alloc(8);
        uint32_t a2[5] = { *ptex, 0, A(lr), 0, 0 };
        ok("DXT1 surface lock", call("IDirect3DTexture9::LockRect", 5, a2) == D3D_OK && lr->pBits != 0);
        ok("DXT1 raw block stored", memcmp(hle_ptr(lr->pBits), "\x00\xF8\xE0\x07\xE4\x00\x00\x00", 8) == 0);
        uint32_t px[4] = { 0, 0, 0, 0 };
        ok("DXT1 decode via d3d9_format", d3d9_format_to_bgra8(D3DFMT_DXT1, hle_ptr(lr->pBits), lr->Pitch, 4, 4, px, 16) == 0);
        ok("DXT1 pixels", px[0] == 0xFFFF0000u && px[1] == 0xFF00FF00u && px[2] == 0xFFAA5500u && px[3] == 0xFF55AA00u);
        uint32_t b[2] = { *ptex, 0 };
        call("IDirect3DTexture9::UnlockRect", 2, b);
    }
    {
        /* Ex variant with defaults. */
        uint32_t *ptex = (uint32_t *)hle_alloc(4);
        uint32_t a[15] = { dev, A(g_dds), G_DDS_SIZE,
                           D3DX_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, 0, D3DFMT_UNKNOWN,
                           D3DPOOL_MANAGED, D3DX_DEFAULT, D3DX_DEFAULT, 0, 0, 0, A(ptex) };
        ok("CreateTextureFromFileInMemoryEx(DDS DXT1)", call("D3DXCreateTextureFromFileInMemoryEx", 15, a) == D3D_OK && *ptex != 0);
    }
    {
        /* Non-zero ColorKey is not implemented -> error. */
        uint32_t *ptex = (uint32_t *)hle_alloc(4);
        uint32_t a[15] = { dev, A(g_dds), G_DDS_SIZE,
                           D3DX_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, 0, D3DFMT_UNKNOWN,
                           D3DPOOL_MANAGED, D3DX_DEFAULT, D3DX_DEFAULT, 0x00FF0000u, 0, 0, A(ptex) };
        ok("Ex rejects non-zero ColorKey", call("D3DXCreateTextureFromFileInMemoryEx", 15, a) != D3D_OK);
    }

    /* ---- broken inputs ---- */
    {
        uint8_t *bad = (uint8_t *)hle_alloc(128);
        uint32_t *ptex = (uint32_t *)hle_alloc(4);
        memcpy(bad, g_dds, 128);
        ok("GetImageInfo truncated DDS fails",
           call("D3DXGetImageInfoFromFileInMemory", 3, (uint32_t[]){ A(bad), 4, 0 }) != D3D_OK);
        ok("CreateTexture truncated DDS fails",
           call("D3DXCreateTextureFromFileInMemory", 4, (uint32_t[]){ dev, A(bad), 4, A(ptex) }) != D3D_OK);

        memcpy(bad, g_dds, 128);
        bad[0] = 'X';
        ok("bad magic DDS fails",
           call("D3DXGetImageInfoFromFileInMemory", 3, (uint32_t[]){ A(bad), 128, 0 }) != D3D_OK);

        memcpy(bad, g_dds, 128);
        put32(bad + 84, 0x32545844);            /* 'D','X','T','2' */
        ok("DXT2 DDS fails",
           call("D3DXGetImageInfoFromFileInMemory", 3, (uint32_t[]){ A(bad), 128, 0 }) != D3D_OK);
        ok("DXT2 create fails",
           call("D3DXCreateTextureFromFileInMemory", 4, (uint32_t[]){ dev, A(bad), 128, A(ptex) }) != D3D_OK);

        ok("TGA truncated fails",
           call("D3DXGetImageInfoFromFileInMemory", 3, (uint32_t[]){ A(g_tga), 3, 0 }) != D3D_OK);
        ok("BMP truncated fails",
           call("D3DXGetImageInfoFromFileInMemory", 3, (uint32_t[]){ A(g_bmp), 40, 0 }) != D3D_OK);
        ok("garbage fails",
           call("D3DXGetImageInfoFromFileInMemory", 3, (uint32_t[]){ A(g_tga) + 20, 6, 0 }) != D3D_OK);
        uint32_t *junk = (uint32_t *)hle_alloc(32);
        memset(junk, 0, 32);
        ok("all-zero data fails",
           call("D3DXGetImageInfoFromFileInMemory", 3, (uint32_t[]){ A(junk), 32, 0 }) != D3D_OK);
    }

    /* ---- D3DXCreateTextureFromFileA (host, with a real file) ---- */
    {
        FILE *f = fopen("/tmp/d3dx9_texture_selftest.bmp", "wb");
        int wrote = 0;
        if (f) { wrote = fwrite(g_bmp, 1, G_BMP_SIZE, f) == G_BMP_SIZE && fclose(f) == 0; }
        if (wrote) {
            uint32_t hb = 0;
            d3dx9_texture_info info2;
            ok("d3dx9_texture_create_from_file_a",
               d3dx9_texture_create_from_file_a(dev, "/tmp/d3dx9_texture_selftest.bmp", &hb, &info2) == 0 && hb != 0);
            ok("FromFileA info", info2.Width == 2 && info2.Height == 2 && info2.ImageFileFormat == D3DXIFF_BMP);
            check_pixels_bgra("FromFileA pixels", hb, exp_pixels, 4);
            remove("/tmp/d3dx9_texture_selftest.bmp");
        }
    }

    /* ---- D3DXCreateCubeTexture ---- */
    {
        uint32_t *ptex = (uint32_t *)hle_alloc(4); *ptex=0;
        uint32_t hr = call("D3DXCreateCubeTexture", 7, (uint32_t[]){dev,64,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,A(ptex)});
        ok("CreateCubeTexture hr", hr==D3D_OK);
        ok("CreateCubeTexture tex", *ptex!=0);
        if (*ptex) {
            D3DLOCKED_RECT *lr = (D3DLOCKED_RECT *)hle_alloc(8);
            /* IDirect3DCubeTexture9::LockRect(this, Face, Level, pLockedRect, pRect, Flags) */
            hr = call("IDirect3DCubeTexture9::LockRect", 6, (uint32_t[]){*ptex,0,0,A(lr),0,0});
            ok("CubeTexture LockRect face0", hr==D3D_OK);
            call("IDirect3DCubeTexture9::UnlockRect", 3, (uint32_t[]){*ptex,0,0});
        }
    }
    /* ---- D3DXCreateVolumeTexture ---- */
    {
        uint32_t *ptex = (uint32_t *)hle_alloc(4); *ptex=0;
        uint32_t hr = call("D3DXCreateVolumeTexture", 9, (uint32_t[]){dev,8,4,2,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,A(ptex)});
        ok("CreateVolumeTexture hr", hr==D3D_OK);
        ok("CreateVolumeTexture tex", *ptex!=0);
    }
    /* ---- D3DXFillTexture ---- */
    {
        uint32_t *ptex = (uint32_t *)hle_alloc(4); *ptex=0;
        uint32_t hr = call("D3DXCreateTexture", 8, (uint32_t[]){dev,2,2,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,A(ptex)});
        ok("FillTexture create", hr==D3D_OK && *ptex!=0);
        uint32_t cb = hle_resolve("test_fill_cb");
        ok("FillTexture cb resolved", cb!=0);
        hr = call("D3DXFillTexture", 3, (uint32_t[]){*ptex, cb, 0});
        ok("FillTexture hr", hr==D3D_OK);
        D3DLOCKED_RECT *lr = (D3DLOCKED_RECT *)hle_alloc(8);
        hr = call("IDirect3DTexture9::LockRect", 5, (uint32_t[]){*ptex,0,A(lr),0,0});
        ok("FillTexture lock", hr==D3D_OK);
        if (hr==D3D_OK) {
            uint8_t *row0 = (uint8_t*)hle_ptr(lr->pBits);
            uint8_t *row1 = row0 + (uint32_t)lr->Pitch;
            /* texel (0,0): u=0.25,v=0.25 => R=63/64, G=63/64, B=0, A=255 */
            uint32_t p00 = *(uint32_t*)row0;
            uint32_t p10 = *(uint32_t*)(row0+4);
            uint32_t p01 = *(uint32_t*)row1;
            uint32_t p11 = *(uint32_t*)(row1+4);
            /* check red channel grows with x, green with y */
            ok("FillTexture p00 blue=0 alpha=255", (p00&0xFF)==0 && ((p00>>24)&0xFF)==255);
            ok("FillTexture red increases x", ((p10>>16)&0xFF) > ((p00>>16)&0xFF));
            ok("FillTexture green increases y", ((p01>>8)&0xFF) > ((p00>>8)&0xFF));
            ok("FillTexture p11 green > p10 green", ((p11>>8)&0xFF) > ((p10>>8)&0xFF));
            ok("FillTexture p11 red > p01 red", ((p11>>16)&0xFF) > ((p01>>16)&0xFF));
            call("IDirect3DTexture9::UnlockRect", 2, (uint32_t[]){*ptex,0});
        }
    }
    /* ---- D3DXComputeNormalMap ---- */
    {
        uint32_t *src = (uint32_t *)hle_alloc(4); *src=0;
        uint32_t *dst = (uint32_t *)hle_alloc(4); *dst=0;
        call("D3DXCreateTexture", 8, (uint32_t[]){dev,4,4,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,A(src)});
        call("D3DXCreateTexture", 8, (uint32_t[]){dev,4,4,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,A(dst)});
        ok("ComputeNormalMap textures", *src!=0 && *dst!=0);
        /* fill src with a gradient: L8-like in red channel */
        D3DLOCKED_RECT *lr = (D3DLOCKED_RECT *)hle_alloc(8);
        if (call("IDirect3DTexture9::LockRect",5,(uint32_t[]){*src,0,A(lr),0,0})==D3D_OK) {
            for(int y=0;y<4;y++) for(int x=0;x<4;x++){
                uint8_t v = (uint8_t)(x*64);
                uint32_t *px=(uint32_t*)((uint8_t*)hle_ptr(lr->pBits)+y*(uint32_t)lr->Pitch+x*4);
                *px = 0xFF000000u | (v<<16) | (v<<8) | v;
            }
            call("IDirect3DTexture9::UnlockRect",2,(uint32_t[]){*src,0});
        }
        uint32_t hr = call("D3DXComputeNormalMap", 6, (uint32_t[]){*dst,*src,0,0,1,fb(1.0f)});
        ok("ComputeNormalMap hr", hr==D3D_OK);
        if (hr==D3D_OK && call("IDirect3DTexture9::LockRect",5,(uint32_t[]){*dst,0,A(lr),0,0})==D3D_OK) {
            uint32_t *px=(uint32_t*)((uint8_t*)hle_ptr(lr->pBits)+1*(uint32_t)lr->Pitch+1*4);
            uint32_t p = *px;
            /* normal should have some blue component (nz) and be normalized-ish */
            ok("ComputeNormalMap has blue", ((p>>0)&0xFF) > 100);
            call("IDirect3DTexture9::UnlockRect",2,(uint32_t[]){*dst,0});
        }
    }

    if (!fails) printf("d3dx9_texture_selftest: all checks passed\n");
    return fails != 0;
}
