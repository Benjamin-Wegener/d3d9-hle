/*
 * d3dx9_surface_selftest.c - D3DX surface helpers.
 *
 * Every call uses the *public* stdcall argument count and order (D3dx9tex.h),
 * with literal public constant values. Fixed pixel vectors are checked against
 * values computed here, independently of the surface HLE. The four shims are
 * also entered directly to verify their callee-side stack purge, something
 * hle_call_guest's register save/restore would otherwise hide.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "win32hle.h"
#include "host.h"
#include "d3d9_hle.h"
#include "d3dx9_hle.h"
#include "d3dx9_surface.h"

const recomp_dispatch_entry_t recomp_dispatch_table[1];
const uint32_t recomp_dispatch_count = 0;

static int fails;
static void ok(const char *what, int cond) { if (!cond) { printf("FAIL %s\n", what); fails++; } }
static uint32_t call(const char *name, int n, const uint32_t *a) {
    return hle_call_guest(hle_resolve(name), n, a);
}

/* Daten in Gast-Speicher kopieren und die Gast-VA zurückgeben. */
static uint32_t guest_va(const void *data, uint32_t n) {
    void *gb = hle_alloc(n ? n : 1);
    memcpy(gb, data, n);
    return hle_va(gb);
}

/* ---- Gerät, Oberflächen, Texturen ---- */

static uint32_t create_device(void) {
    uint32_t a1[1] = { 32u };
    uint32_t d3d9 = call("Direct3DCreate9", 1, a1);
    if (!d3d9) return 0;
    void *pp_h = hle_alloc(sizeof(D3DPRESENT_PARAMETERS));
    D3DPRESENT_PARAMETERS *pp = (D3DPRESENT_PARAMETERS *)pp_h;
    memset(pp, 0, sizeof *pp);
    pp->BackBufferWidth = 64;
    pp->BackBufferHeight = 64;
    pp->BackBufferFormat = D3DFMT_A8R8G8B8;
    pp->BackBufferCount = 1;
    pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp->Windowed = 1;
    void *out_h = hle_alloc(4);
    uint32_t a[7] = { d3d9, 0, D3DDEVTYPE_HAL, 0, 0x44u, hle_va(pp_h), hle_va(out_h) };
    if (call("IDirect3D9::CreateDevice", 7, a) != D3D_OK) return 0;
    uint32_t dev;
    memcpy(&dev, out_h, 4);
    return dev;
}

static uint32_t create_surface(uint32_t dev, uint32_t w, uint32_t h, uint32_t fmt) {
    void *out_h = hle_alloc(4);
    uint32_t a[7] = { dev, w, h, fmt, D3DPOOL_SYSTEMMEM, hle_va(out_h), 0 };
    if (call("IDirect3DDevice9::CreateOffscreenPlainSurface", 7, a) != D3D_OK) return 0;
    uint32_t surf;
    memcpy(&surf, out_h, 4);
    return surf;
}

static uint32_t create_texture(uint32_t dev, uint32_t w, uint32_t h,
                               uint32_t levels, uint32_t fmt) {
    void *out_h = hle_alloc(4);
    uint32_t a[9] = { dev, w, h, levels, 0, fmt, D3DPOOL_SYSTEMMEM, hle_va(out_h), 0 };
    if (call("IDirect3DDevice9::CreateTexture", 9, a) != D3D_OK) return 0;
    uint32_t tex;
    memcpy(&tex, out_h, 4);
    return tex;
}

static uint32_t mkrect(int x1, int y1, int x2, int y2) {
    int32_t r[4] = { (int32_t)x1, (int32_t)y1, (int32_t)x2, (int32_t)y2 };
    void *gb = hle_alloc(16);
    memcpy(gb, r, 16);
    return hle_va(gb);
}

static int lock_read(uint32_t surf, void *dst, uint32_t n) {
    void *lr_h = hle_alloc(sizeof(D3DLOCKED_RECT));
    uint32_t la[4] = { surf, hle_va(lr_h), 0, 0 };
    if (call("IDirect3DSurface9::LockRect", 4, la) != D3D_OK) return 0;
    D3DLOCKED_RECT *lr = (D3DLOCKED_RECT *)lr_h;
    if (!lr->pBits) {
        uint32_t ua[1] = { surf };
        call("IDirect3DSurface9::UnlockRect", 1, ua);
        return 0;
    }
    memcpy(dst, hle_ptr(lr->pBits), n);
    uint32_t ua[1] = { surf };
    call("IDirect3DSurface9::UnlockRect", 1, ua);
    return 1;
}

static int tex_level_lock(uint32_t tex, uint32_t level, D3DLOCKED_RECT *lr) {
    void *lr_h = hle_alloc(sizeof(D3DLOCKED_RECT));
    uint32_t a[5] = { tex, level, hle_va(lr_h), 0, 0 };
    if (call("IDirect3DTexture9::LockRect", 5, a) != D3D_OK) return 0;
    *lr = *(D3DLOCKED_RECT *)lr_h;
    return lr->pBits != 0;
}
static void tex_level_unlock(uint32_t tex, uint32_t level) {
    uint32_t a[2] = { tex, level };
    call("IDirect3DTexture9::UnlockRect", 2, a);
}

/* ---- public-ABI thin wrappers (literal argument positions) ---- */

/* D3DXLoadSurfaceFromMemory: 10 args. srcpal is arg6, srcrect arg7,
 * filter arg8, colorkey arg9. */
static uint32_t load_mem(uint32_t surf, uint32_t drect, uint32_t srcmem,
                         uint32_t srcfmt, uint32_t srcpitch, uint32_t srcpal,
                         uint32_t srect, uint32_t filter, uint32_t ckey) {
    uint32_t a[10] = { surf, 0u, drect, srcmem, srcfmt, srcpitch,
                       srcpal, srect, filter, ckey };
    return call("D3DXLoadSurfaceFromMemory", 10, a);
}

/* D3DXLoadSurfaceFromSurface: 8 args, colorkey is arg7. */
static uint32_t load_surf(uint32_t dst, uint32_t dstpal, uint32_t drect,
                          uint32_t src, uint32_t srcpal, uint32_t srect,
                          uint32_t filter, uint32_t ckey) {
    uint32_t a[8] = { dst, dstpal, drect, src, srcpal, srect, filter, ckey };
    return call("D3DXLoadSurfaceFromSurface", 8, a);
}

/* D3DXFilterTexture: 4 args (texture, palette, srclevel, filter). */
static uint32_t filter_tex(uint32_t tex, uint32_t pal, uint32_t level, uint32_t filter) {
    uint32_t a[4] = { tex, pal, level, filter };
    return call("D3DXFilterTexture", 4, a);
}

/* D3DXSaveSurfaceToFileA: 5 args, arg4 is a RECT* (not a filter). */
static uint32_t save_file(const char *name, uint32_t ffmt, uint32_t surf,
                          uint32_t pal, uint32_t srect) {
    uint32_t a[5] = { guest_va(name, (uint32_t)strlen(name) + 1u), ffmt, surf, pal, srect };
    return call("D3DXSaveSurfaceToFileA", 5, a);
}

/* ---- direct stdcall-entry ABI checks ---- */

static void check_abi(const char *what, const char *name, int n, const uint32_t *a) {
    /* Same proven pattern as d3dx9_selftest.c: enter the lifted function
     * directly so its callee-side stdcall stack purge becomes observable. */
    uint32_t va = hle_resolve(name);
    ok(name, va != 0);
    if (!va) return;
    mach_enter();
    uint32_t esp0 = g_esp;
    for (int i = n - 1; i >= 0; i--) PUSH32(g_esp, a[i]);
    PUSH32(g_esp, RECOMP_RETADDR);
    recomp_func_t f = recomp_lookup_import(va);
    if (f) f();
    ok(what, g_esp == esp0);                    /* callee purged ret + exactly n args */
    g_esp = esp0;
    mach_leave();
}

static void test_abi(void) {
    uint32_t a10[10] = { 0 };
    check_abi("FromMemory purges ret + 10 args", "D3DXLoadSurfaceFromMemory", 10, a10);
    uint32_t a8[8] = { 0 };
    check_abi("FromSurface purges ret + 8 args", "D3DXLoadSurfaceFromSurface", 8, a8);
    uint32_t a4[4] = { 0 };
    check_abi("FilterTexture purges ret + 4 args", "D3DXFilterTexture", 4, a4);
    uint32_t a5[5] = { 0 };
    check_abi("SaveToFile purges ret + 5 args", "D3DXSaveSurfaceToFileA", 5, a5);
}

/* ---- D3DXLoadSurfaceFromMemory ---- */

static void test_basic(void) {
    uint32_t dev = create_device();
    ok("Gerät angelegt", dev != 0);
    if (!dev) return;

    uint32_t dst = create_surface(dev, 4, 1, D3DFMT_A8R8G8B8);
    ok("Zieloberfläche angelegt", dst != 0);
    if (!dst) return;

    /* R5G6B5: rot, grün, blau, weiß -> known ARGB8 values. */
    uint16_t r5[4] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
    uint32_t smem = guest_va(r5, sizeof r5);
    uint32_t srect = mkrect(0, 0, 4, 1);

    /* Literal public D3DX_DEFAULT (0xFFFFFFFF), not a private alias. */
    int rc = (int)load_mem(dst, 0, smem, D3DFMT_R5G6B5, 8, 0, srect, 0xFFFFFFFFu, 0);
    ok("LoadSurfaceFromMemory R5G6B5->A8R8G8B8 == D3D_OK", rc == D3D_OK);

    uint32_t px[4];
    ok("Zieloberfläche lesbar", lock_read(dst, px, sizeof px));
    ok("Pixel 0 rot", px[0] == 0xFFFF0000u);
    ok("Pixel 1 grün", px[1] == 0xFF00FF00u);
    ok("Pixel 2 blau", px[2] == 0xFF0000FFu);
    ok("Pixel 3 weiß", px[3] == 0xFFFFFFFFu);

    /* ColorKey is arg9: the black L8 pixel is skipped, gray is converted. */
    uint8_t l8[2] = { 0x00, 0x7F };
    uint32_t smem2 = guest_va(l8, sizeof l8);
    rc = (int)load_mem(dst, mkrect(0, 0, 2, 1), smem2, D3DFMT_L8, 2, 0,
                       mkrect(0, 0, 2, 1), 1u /* D3DX_FILTER_NONE */, 0xFF000000u);
    ok("LoadSurfaceFromMemory ColorKey == D3D_OK", rc == D3D_OK);
    ok("ColorKey-Pixel unberührt", lock_read(dst, px, 8) && px[0] == 0xFFFF0000u);
    ok("ColorKey-Pixel konvertiert", px[1] == 0xFF7F7F7Fu);

    /* Zielrechteck (1,0)-(2,0) <- Quellrechteck (0,0)-(1,0). */
    uint16_t red = 0xF800;
    rc = (int)load_mem(dst, mkrect(1, 0, 2, 1), guest_va(&red, 2), D3DFMT_R5G6B5, 2,
                       0, mkrect(0, 0, 1, 1), 0xFFFFFFFFu, 0);
    ok("LoadSurfaceFromMemory Rechteck == D3D_OK", rc == D3D_OK);
    ok("Rechteck-Zielpixel rot", lock_read(dst, px, 8) && px[1] == 0xFFFF0000u);
    ok("Rechteck außerhalb unberührt", px[0] == 0xFFFF0000u);

    /*BOX-Filter: 2x2 (rot,grün,blau,weiß) -> 1x1 = (127,127,127).*/
    uint32_t small = create_surface(dev, 1, 1, D3DFMT_A8R8G8B8);
    if (small) {
        uint16_t quad[4] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
        rc = (int)load_mem(small, 0, guest_va(quad, 8), D3DFMT_R5G6B5, 4, 0,
                           mkrect(0, 0, 2, 2), 5u /* D3DX_FILTER_BOX */, 0);
        ok("LoadSurfaceFromMemory Box-Skalierung == D3D_OK", rc == D3D_OK);
        uint32_t avg;
        ok("Box-Mittel 0xFF7F7F7F", lock_read(small, &avg, 4) && avg == 0xFF7F7F7Fu);
    }
}

static void test_formats(void) {
    uint32_t dev = create_device();
    if (!dev) return;
    uint32_t dst = create_surface(dev, 2, 1, D3DFMT_X8R8G8B8);
    if (!dst) return;

    uint32_t argb = 0x80FF0000u;
    int rc = (int)load_mem(dst, mkrect(0, 0, 1, 1), guest_va(&argb, 4),
                           D3DFMT_A8R8G8B8, 4, 0, mkrect(0, 0, 1, 1), 1u, 0);
    ok("A8R8G8B8->X8R8G8B8 == D3D_OK", rc == D3D_OK);
    uint32_t px;
    ok("X8-Zielpixel", lock_read(dst, &px, 4) && px == 0x80FF0000u);

    uint32_t l8surf = create_surface(dev, 1, 1, D3DFMT_L8);
    if (l8surf) {
        uint16_t white = 0xFFFF;
        rc = (int)load_mem(l8surf, 0, guest_va(&white, 2), D3DFMT_R5G6B5, 2, 0,
                           mkrect(0, 0, 1, 1), 1u, 0);
        ok("R5G6B5->L8 == D3D_OK", rc == D3D_OK);
        uint8_t lum;
        ok("L8-Ziel 0xFF", lock_read(l8surf, &lum, 1) && lum == 0xFFu);
    }

    uint32_t r5surf = create_surface(dev, 1, 1, D3DFMT_R5G6B5);
    if (r5surf) {
        uint32_t red = 0xFFFF0000u;
        rc = (int)load_mem(r5surf, 0, guest_va(&red, 4), D3DFMT_A8R8G8B8, 4, 0,
                           mkrect(0, 0, 1, 1), 1u, 0);
        ok("A8R8G8B8->R5G6B5 == D3D_OK", rc == D3D_OK);
        uint16_t v;
        ok("R5G6B5-Ziel 0xF800", lock_read(r5surf, &v, 2) && v == 0xF800u);
    }
}

static int write_surface_raw(uint32_t surf, const void *data, uint32_t n) {
    void *lr_h = hle_alloc(sizeof(D3DLOCKED_RECT));
    uint32_t la[4] = { surf, hle_va(lr_h), 0, 0 };
    if (call("IDirect3DSurface9::LockRect", 4, la) != D3D_OK) return 0;
    D3DLOCKED_RECT *lr = (D3DLOCKED_RECT *)lr_h;
    if (!lr->pBits) return 0;
    memcpy(hle_ptr(lr->pBits), data, n);
    uint32_t ua[1] = { surf };
    call("IDirect3DSurface9::UnlockRect", 1, ua);
    return 1;
}

static void test_block_compressed(void) {
    uint32_t dev = create_device();
    if (!dev) return;
    uint32_t dx = create_surface(dev, 4, 4, D3DFMT_DXT1);
    uint32_t dst = create_surface(dev, 4, 4, D3DFMT_A8R8G8B8);
    ok("DXT: Oberflächen angelegt", dx && dst);
    if (!dx || !dst) return;

    /* Ein DXT1-Block direkt in die komprimierte Oberfläche: c0=rot, c1=grün,
     * alle Indizes 0 -> rot. In eine DXT-Oberfläche zu schreiben wird von
     * LoadSurfaceFromMemory bewusst abgelehnt, daher der Rohzugriff. */
    uint8_t block[8] = { 0x00, 0xF8, 0xE0, 0x07, 0, 0, 0, 0 };
    ok("DXT: Rohblock geschrieben", write_surface_raw(dx, block, sizeof block));
    ok("DXT: FromSurface == D3D_OK",
       load_surf(dst, 0, 0, dx, 0, 0, 1u, 0) == D3D_OK);
    uint32_t px[16];
    ok("DXT: Pixel rot", lock_read(dst, px, sizeof px) && px[0] == 0xFFFF0000u);
    ok("DXT: alle Pixel rot", px[15] == 0xFFFF0000u);
    ok("DXT: blockverrücktes Quellrechteck -> INVALIDCALL",
       load_surf(dst, 0, 0, dx, 0, mkrect(1, 0, 4, 4), 1u, 0) == D3DERR_INVALIDCALL);
    ok("DXT: Schreiben in DXT-Ziel -> NOTAVAILABLE",
       load_mem(dx, 0, guest_va(block, 8), D3DFMT_DXT1, 8, 0,
                mkrect(0, 0, 4, 4), 1u, 0) == D3DERR_NOTAVAILABLE);

    char name[128];
    snprintf(name, sizeof name, "/tmp/d3dx9_surface_dxt_%d.dds", (int)getpid());
    ok("DXT: DDS-Speichern == D3D_OK", save_file(name, 4u, dx, 0, 0) == D3D_OK);
    FILE *f = fopen(name, "rb");
    ok("DXT: DDS existiert", f != NULL);
    if (f) {
        uint8_t hdr[124];
        ok("DXT: DDS-Kopf lesbar", fread(hdr, 1, 124, f) == 124);
        ok("DXT: DXT1-FourCC",
           hdr[76 + 8] == 'D' && hdr[76 + 9] == 'X' &&
           hdr[76 + 10] == 'T' && hdr[76 + 11] == '1');
        uint8_t raw[8];
        ok("DXT: DDS-Block lesbar", fread(raw, 1, 8, f) == 8);
        ok("DXT: DDS-Block unverändert", memcmp(raw, block, 8) == 0);
        fclose(f);
        unlink(name);
    }
}

static void test_errors(void) {
    uint32_t dev = create_device();
    if (!dev) return;
    uint32_t dst = create_surface(dev, 2, 2, D3DFMT_A8R8G8B8);
    if (!dst) return;

    uint8_t mem[16];
    memset(mem, 0xFF, sizeof mem);
    uint32_t smem = guest_va(mem, sizeof mem);
    uint32_t srect = mkrect(0, 0, 2, 2);

    ok("NULL-Oberfläche -> INVALIDCALL",
       load_mem(0, 0, smem, D3DFMT_R5G6B5, 8, 0, srect, 1u, 0) == D3DERR_INVALIDCALL);
    ok("unbekanntes Quellformat -> INVALIDCALL",
       load_mem(dst, 0, smem, 12345u, 8, 0, srect, 1u, 0) == D3DERR_INVALIDCALL);
    ok("ungültiger Filter (0x1234) -> INVALIDCALL",
       load_mem(dst, 0, smem, D3DFMT_R5G6B5, 8, 0, srect, 0x1234u, 0) == D3DERR_INVALIDCALL);
    ok("fehlendes Quellrechteck -> INVALIDCALL",
       load_mem(dst, 0, smem, D3DFMT_R5G6B5, 8, 0, 0, 1u, 0) == D3DERR_INVALIDCALL);
    ok("Quell-Palette (arg6) -> NOTAVAILABLE",
       load_mem(dst, 0, smem, D3DFMT_R5G6B5, 8, hle_va(hle_alloc(4)), srect, 1u, 0)
           == D3DERR_NOTAVAILABLE);
    ok("Ziel-Palette (arg1) -> NOTAVAILABLE",
       load_surf(dst, hle_va(hle_alloc(4)), 0, dst, 0, 0, 1u, 0)
           == D3DERR_NOTAVAILABLE);
    ok("Pitch 0 -> INVALIDCALL",
       load_mem(dst, 0, smem, D3DFMT_R5G6B5, 0, 0, srect, 1u, 0) == D3DERR_INVALIDCALL);
    ok("invertiertes Zielrechteck -> INVALIDCALL",
       load_mem(dst, mkrect(2, 2, 0, 0), smem, D3DFMT_R5G6B5, 8, 0, srect, 1u, 0)
           == D3DERR_INVALIDCALL);
}

/* ---- D3DXLoadSurfaceFromSurface ---- */

static void test_from_surface(void) {
    uint32_t dev = create_device();
    ok("FromSurface: Gerät angelegt", dev != 0);
    if (!dev) return;

    uint32_t src = create_surface(dev, 2, 1, D3DFMT_R5G6B5);
    uint32_t dst = create_surface(dev, 2, 1, D3DFMT_A8R8G8B8);
    ok("FromSurface: Oberflächen angelegt", src && dst);
    if (!src || !dst) return;

    uint16_t r5[2] = { 0xF800, 0x001F };       /* rot, blau */
    ok("FromSurface: Quelle gefüllt",
       load_mem(src, 0, guest_va(r5, 4), D3DFMT_R5G6B5, 4, 0, mkrect(0, 0, 2, 1), 1u, 0)
           == D3D_OK);

    ok("FromSurface: kompletter Copy == D3D_OK",
       load_surf(dst, 0, 0, src, 0, 0, 1u, 0) == D3D_OK);
    uint32_t px[2];
    ok("FromSurface: Pixel rot/blau",
       lock_read(dst, px, 8) && px[0] == 0xFFFF0000u && px[1] == 0xFF0000FFu);

    /* Box-Skalierung 2x1 -> 1x1: Mittel aus rot und blau = (127,0,127). */
    uint32_t one = create_surface(dev, 1, 1, D3DFMT_A8R8G8B8);
    if (one) {
        ok("FromSurface: Box-Skalierung == D3D_OK",
           load_surf(one, 0, 0, src, 0, 0, 5u /* BOX */, 0) == D3D_OK);
        uint32_t avg;
        ok("FromSurface: Box-Mittel 0xFF7F007F",
           lock_read(one, &avg, 4) && avg == 0xFF7F007Fu);
    }

    /* ColorKey arg7: rotes Quellpixel wird ausgespart, weiß bleibt. */
    uint32_t white[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu };
    load_mem(dst, 0, guest_va(white, 8), D3DFMT_A8R8G8B8, 8, 0, mkrect(0, 0, 2, 1), 1u, 0);
    ok("FromSurface: ColorKey == D3D_OK",
       load_surf(dst, 0, 0, src, 0, 0, 1u, 0xFFFF0000u) == D3D_OK);
    ok("FromSurface: ColorKey-Pixel weiß",
       lock_read(dst, px, 8) && px[0] == 0xFFFFFFFFu);
    ok("FromSurface: anderes Pixel blau", px[1] == 0xFF0000FFu);

    /* Rechteck */
    ok("FromSurface: Rechteck == D3D_OK",
       load_surf(dst, 0, mkrect(0, 0, 1, 1), src, 0, mkrect(0, 0, 1, 1), 1u, 0) == D3D_OK);
    ok("FromSurface: Rechteck-Pixel rot", lock_read(dst, px, 8) && px[0] == 0xFFFF0000u);

    /* Fehlerpfade */
    ok("FromSurface: NULL-Ziel -> INVALIDCALL",
       load_surf(0, 0, 0, src, 0, 0, 1u, 0) == D3DERR_INVALIDCALL);
    ok("FromSurface: NULL-Quelle -> INVALIDCALL",
       load_surf(dst, 0, 0, 0, 0, 0, 1u, 0) == D3DERR_INVALIDCALL);
    ok("FromSurface: Quell-Palette -> NOTAVAILABLE",
       load_surf(dst, 0, 0, src, hle_va(hle_alloc(4)), 0, 1u, 0) == D3DERR_NOTAVAILABLE);
    ok("FromSurface: ungültiger Filter -> INVALIDCALL",
       load_surf(dst, 0, 0, src, 0, 0, 0x1234u, 0) == D3DERR_INVALIDCALL);
}

/* ---- D3DXFilterTexture ---- */

static void test_filter_texture(void) {
    uint32_t dev = create_device();
    ok("FilterTexture: Gerät angelegt", dev != 0);
    if (!dev) return;

    uint32_t tex = create_texture(dev, 4, 4, 3, D3DFMT_A8R8G8B8);
    ok("FilterTexture: Textur angelegt", tex != 0);
    if (!tex) return;

    D3DLOCKED_RECT l0;
    ok("FilterTexture: Level 0 lesbar", tex_level_lock(tex, 0, &l0));
    if (l0.pBits) {
        uint32_t *px = (uint32_t *)hle_ptr(l0.pBits);
        for (uint32_t y = 0; y < 4; y++)
            for (uint32_t x = 0; x < 4; x++)
                px[y * 4 + x] = ((x + y) & 1) ? 0u : 0xFFFFFFFFu;   /* Schachbrett */
        tex_level_unlock(tex, 0);
    }

    /* Literal public constants: srclevel D3DX_DEFAULT, filter D3DX_FILTER_BOX. */
    ok("FilterTexture: == D3D_OK",
       filter_tex(tex, 0, 0xFFFFFFFFu, 5u) == D3D_OK);

    D3DLOCKED_RECT l1;
    ok("FilterTexture: Level 1 lesbar", tex_level_lock(tex, 1, &l1));
    if (l1.pBits) {
        uint32_t m1[4];
        memcpy(m1, hle_ptr(l1.pBits), 16);
        ok("FilterTexture: Level 1 grau 0x7F",
           m1[0] == 0x7F7F7F7Fu && m1[1] == 0x7F7F7F7Fu &&
           m1[2] == 0x7F7F7F7Fu && m1[3] == 0x7F7F7F7Fu);
        tex_level_unlock(tex, 1);
    }
    D3DLOCKED_RECT l2;
    ok("FilterTexture: Level 2 lesbar", tex_level_lock(tex, 2, &l2));
    if (l2.pBits) {
        uint32_t m2;
        memcpy(&m2, hle_ptr(l2.pBits), 4);
        ok("FilterTexture: Level 2 grau 0x7F", m2 == 0x7F7F7F7Fu);
        tex_level_unlock(tex, 2);
    }

    /* srclevel=1 filtert nur Level 2. Level 1 bewusst auf Schachbrett setzen. */
    if (tex_level_lock(tex, 1, &l1) && l1.pBits) {
        uint32_t *px = (uint32_t *)hle_ptr(l1.pBits);
        px[0] = 0xFFFFFFFFu; px[1] = 0u; px[2] = 0u; px[3] = 0xFFFFFFFFu;
        tex_level_unlock(tex, 1);
    }
    if (tex_level_lock(tex, 2, &l2) && l2.pBits) {
        *(uint32_t *)hle_ptr(l2.pBits) = 0u;
        tex_level_unlock(tex, 2);
    }
    ok("FilterTexture: srclevel=1 == D3D_OK",
       filter_tex(tex, 0, 1u, 5u) == D3D_OK);
    if (tex_level_lock(tex, 2, &l2) && l2.pBits) {
        uint32_t m2;
        memcpy(&m2, hle_ptr(l2.pBits), 4);
        ok("FilterTexture: srclevel=1 -> Level 2 grau 0x7F", m2 == 0x7F7F7F7Fu);
        tex_level_unlock(tex, 2);
    }

    ok("FilterTexture: NULL -> INVALIDCALL",
       filter_tex(0, 0, 0u, 5u) == D3DERR_INVALIDCALL);
    ok("FilterTexture: Palette -> NOTAVAILABLE",
       filter_tex(tex, hle_va(hle_alloc(4)), 0u, 5u) == D3DERR_NOTAVAILABLE);
    ok("FilterTexture: ungültiger Filter -> INVALIDCALL",
       filter_tex(tex, 0, 0u, 0x1234u) == D3DERR_INVALIDCALL);
    ok("FilterTexture: SrcLevel zu groß -> INVALIDCALL",
       filter_tex(tex, 0, 3u, 5u) == D3DERR_INVALIDCALL);
    uint32_t surf = create_surface(dev, 4, 4, D3DFMT_A8R8G8B8);
    if (surf)
        ok("FilterTexture: Oberfläche ist keine Textur -> INVALIDCALL",
           filter_tex(surf, 0, 0u, 5u) == D3DERR_INVALIDCALL);
}

/* ---- D3DXSaveSurfaceToFileA ---- */

static void test_save_file(void) {
    uint32_t dev = create_device();
    ok("SaveToFile: Gerät angelegt", dev != 0);
    if (!dev) return;

    uint32_t surf = create_surface(dev, 2, 2, D3DFMT_A8R8G8B8);
    ok("SaveToFile: Oberfläche angelegt", surf != 0);
    if (!surf) return;
    uint32_t cols[4] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFFFFu };
    ok("SaveToFile: Oberfläche gefüllt",
       load_mem(surf, 0, guest_va(cols, 16), D3DFMT_A8R8G8B8, 8, 0,
                mkrect(0, 0, 2, 2), 1u, 0) == D3D_OK);

    char bmp[128], dds[128];
    snprintf(bmp, sizeof bmp, "/tmp/d3dx9_surface_selftest_%d.bmp", (int)getpid());
    snprintf(dds, sizeof dds, "/tmp/d3dx9_surface_selftest_%d.dds", (int)getpid());

    /* Literal public D3DXIFF_BMP=0 and D3DXIFF_DDS=4. */
    ok("SaveToFile: BMP == D3D_OK", save_file(bmp, 0u, surf, 0, 0) == D3D_OK);
    ok("SaveToFile: DDS == D3D_OK", save_file(dds, 4u, surf, 0, 0) == D3D_OK);
    ok("SaveToFile: JPG(1) -> NOTAVAILABLE", save_file(bmp, 1u, surf, 0, 0) == D3DERR_NOTAVAILABLE);

    FILE *f = fopen(bmp, "rb");
    ok("SaveToFile: BMP existiert", f != NULL);
    if (f) {
        uint8_t hdr[54];
        ok("SaveToFile: BMP-Kopf lesbar", fread(hdr, 1, 54, f) == 54);
        ok("SaveToFile: BM-Magic", hdr[0] == 'B' && hdr[1] == 'M');
        uint32_t w, h; uint16_t bc;
        memcpy(&w, hdr + 18, 4); memcpy(&h, hdr + 22, 4); memcpy(&bc, hdr + 28, 2);
        ok("SaveToFile: 2x2 @ 24 bit", w == 2 && h == 2 && bc == 24);
        uint8_t row0[8];
        ok("SaveToFile: BMP-Pixel lesbar", fread(row0, 1, 8, f) == 8);
        /* Zeile 0 liegt unten: blau, weiß (BGR). */
        ok("SaveToFile: Zeile unten blau/weiss (BGR)",
           row0[0] == 0xFF && row0[1] == 0x00 && row0[2] == 0x00 &&
           row0[3] == 0xFF && row0[4] == 0xFF && row0[5] == 0xFF);
        fclose(f);
        unlink(bmp);
    }

    f = fopen(dds, "rb");
    ok("SaveToFile: DDS existiert", f != NULL);
    if (f) {
        uint8_t hdr[124];
        ok("SaveToFile: DDS-Kopf lesbar", fread(hdr, 1, 124, f) == 124);
        ok("SaveToFile: DDS-Magic", hdr[0] == 'D' && hdr[1] == 'D' &&
           hdr[2] == 'S' && hdr[3] == ' ');
        uint32_t sz, w, h;
        memcpy(&sz, hdr + 4, 4); memcpy(&h, hdr + 12, 4); memcpy(&w, hdr + 16, 4);
        ok("SaveToFile: 124er-Kopf, 2x2", sz == 124 && w == 2 && h == 2);
        uint32_t pf_size, pf_flags, pf_fourcc, pf_count;
        memcpy(&pf_size, hdr + 76, 4); memcpy(&pf_flags, hdr + 80, 4);
        memcpy(&pf_fourcc, hdr + 84, 4); memcpy(&pf_count, hdr + 88, 4); /* RGBBitCount */
        ok("SaveToFile: A8R8G8B8-Pixelformat (DDPF_RGB, keine FOURCC)",
           pf_size == 32 && (pf_flags & 0x40u) && !(pf_flags & 0x4u) &&
           pf_fourcc == 0 && pf_count == 32);
        uint32_t px[4];
        ok("SaveToFile: rohe Pixel lesbar", fread(px, 1, 16, f) == 16);
        ok("SaveToFile: Pixel rot/gruen/blau/weiss",
           px[0] == 0xFFFF0000u && px[1] == 0xFF00FF00u &&
           px[2] == 0xFF0000FFu && px[3] == 0xFFFFFFFFu);
        fclose(f);
        unlink(dds);
    }

    /* srcrect (arg4) schneidet zu: 1x1 grün. */
    ok("SaveToFile: srcrect == D3D_OK",
       save_file(bmp, 0u, surf, 0, mkrect(1, 0, 2, 1)) == D3D_OK);
    f = fopen(bmp, "rb");
    if (f) {
        uint8_t hdr[54], pix[4];
        ok("SaveToFile: srcrect Kopf lesbar", fread(hdr, 1, 54, f) == 54);
        uint32_t w, h;
        memcpy(&w, hdr + 18, 4); memcpy(&h, hdr + 22, 4);
        ok("SaveToFile: srcrect 1x1", w == 1 && h == 1);
        ok("SaveToFile: srcrect Pixel lesbar", fread(pix, 1, 4, f) == 4);
        ok("SaveToFile: srcrect grün (BGR)",
           pix[0] == 0x00 && pix[1] == 0xFF && pix[2] == 0x00);
        fclose(f);
        unlink(bmp);
    }

    ok("SaveToFile: Palette -> NOTAVAILABLE",
       save_file(bmp, 0u, surf, hle_va(hle_alloc(4)), 0) == D3DERR_NOTAVAILABLE);
    ok("SaveToFile: NULL-Oberfläche -> INVALIDCALL",
       save_file(bmp, 0u, 0, 0, 0) == D3DERR_INVALIDCALL);
    ok("SaveToFile: invertiertes srcrect -> INVALIDCALL",
       save_file(bmp, 0u, surf, 0, mkrect(2, 2, 0, 0)) == D3DERR_INVALIDCALL);
}

int main(void) {
    recomp_host_init();
    win32hle_register(win32hle_d3d9);
    win32hle_register(win32hle_d3dx9);

    test_abi();
    test_basic();
    test_formats();
    test_block_compressed();
    test_errors();
    test_from_surface();
    test_filter_texture();
    test_save_file();

    if (!fails) printf("d3dx9_surface_selftest: ok\n");
    else printf("d3dx9_surface_selftest: %d failed\n", fails);
    return fails != 0;
}
