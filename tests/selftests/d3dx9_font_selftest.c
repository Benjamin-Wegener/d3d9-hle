/*
 * d3dx9_font_selftest.c - D3DXCreateFontA/W + ID3DXFont.
 *
 * The guest reaches ID3DXFont only through its COM vtable. This test therefore
 * invokes EVERY slot by index through the guest object, checks the stdcall
 * stack balance of each slot with a raw call, and verifies that slot i holds
 * the implementation named by the public header (Wine include/d3dx9core.h,
 * ID3DXFontVtbl: 18 methods, slots 0..17). Functional checks cover DT_CALCRECT,
 * horizontal/vertical alignment, word break, UTF-16 conversion, A/W
 * descriptors and metrics, failure paths and device loss/restore.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "win32hle.h"
#include "host.h"
#include "d3d9_hle.h"
#include "d3dx9_hle.h"
#include "d3dx9_font.h"

/* guest pointer to IID_IUnknown {00000000-0000-0000-C000-000000000046} */
static uint32_t iid_unk(void) {
    uint8_t *g = (uint8_t *)hle_alloc(16);
    memset(g, 0, 16); g[8] = 0xC0; g[15] = 0x46;
    return hle_va(g);
}

const recomp_dispatch_entry_t recomp_dispatch_table[1];
const uint32_t recomp_dispatch_count = 0;

static int fails;
static void ok(const char *what, int cond) { if (!cond) { printf("FAIL %s\n", what); fails++; } }
static uint32_t call(const char *name, int n, const uint32_t *a) { return hle_call_guest(hle_resolve(name), n, a); }

/* Public method order and count: Wine include/d3dx9core.h, ID3DXFontVtbl.
 * Names are the source of truth for both the count and the slot positions. */
enum { PUBLIC_ID3DXFONT_SLOTS = 18 };
static const char *slot_names[PUBLIC_ID3DXFONT_SLOTS] = {
    "ID3DXFont::QueryInterface",   /*  0 */
    "ID3DXFont::AddRef",           /*  1 */
    "ID3DXFont::Release",          /*  2 */
    "ID3DXFont::GetDevice",        /*  3 */
    "ID3DXFont::GetDescA",         /*  4 */
    "ID3DXFont::GetDescW",         /*  5 */
    "ID3DXFont::GetTextMetricsA",  /*  6 */
    "ID3DXFont::GetTextMetricsW",  /*  7 */
    "ID3DXFont::GetDC",            /*  8 */
    "ID3DXFont::GetGlyphData",     /*  9 */
    "ID3DXFont::PreloadCharacters",/* 10 */
    "ID3DXFont::PreloadGlyphs",    /* 11 */
    "ID3DXFont::PreloadTextA",     /* 12 */
    "ID3DXFont::PreloadTextW",     /* 13 */
    "ID3DXFont::DrawTextA",        /* 14 */
    "ID3DXFont::DrawTextW",        /* 15 */
    "ID3DXFont::OnLostDevice",     /* 16 */
    "ID3DXFont::OnResetDevice"     /* 17 */
};
enum {
    S_QI=0, S_ADDREF=1, S_RELEASE=2, S_GETDEV=3, S_GETDESCA=4, S_GETDESCW=5,
    S_GETTMA=6, S_GETTMW=7, S_GETDC=8, S_GETGLYPH=9, S_PRECHAR=10, S_PREGLYPH=11,
    S_PRETEXTA=12, S_PRETEXTW=13, S_DRAWTEXTA=14, S_DRAWTEXTW=15,
    S_LOST=16, S_RESET=17
};

static uint32_t g_font, g_font_vt, g_stack;
static uint32_t slot_va(int slot) { return MEM32(g_font_vt + 4u * (uint32_t)slot); }

/* Call a vtable slot through the guest object (used for functional checks). */
static uint32_t fcall(int slot, int n, const uint32_t *a) {
    return hle_call_guest(slot_va(slot), n, a);
}

/* Raw call: nothing restores esp afterwards, so the returned esp proves that
 * the shim's single RET popped exactly 4 + 4*argc bytes. */
static uint32_t raw_vcall(int slot, int argc, const uint32_t *args, uint32_t *out_eax) {
    uint32_t va = slot_va(slot);
    if (!va) { *out_eax = 0; return 0; }
    uint32_t sp = g_stack + 0x800u;
    for (int i = argc - 1; i >= 0; i--) { sp -= 4; MEM32(sp) = args[i]; }
    sp -= 4; MEM32(sp) = 0xDEADBEEFu;
    mach_enter();
    g_esp = sp;
    recomp_func_t f = recomp_lookup(va);
    if (!f) f = recomp_lookup_import(va);
    if (f) f();
    *out_eax = g_eax;
    uint32_t after = g_esp;
    mach_leave();
    return after;
}

static void clear_rt(uint32_t dev) {
    /* CPU clear: selftests run without Metal backend. */
    uint32_t pp = hle_va(hle_alloc(4));
    uint32_t a[3] = { dev, 0, pp };
    if (call("IDirect3DDevice9::GetRenderTarget", 3, a) != D3D_OK) return;
    uint32_t surf = MEM32(pp);
    if (!surf) return;
    void *lrp = hle_alloc(8);
    uint32_t la[4] = { surf, hle_va(lrp), 0, 0 };
    if (call("IDirect3DSurface9::LockRect", 4, la) == D3D_OK) {
        int32_t pitch = ((int32_t*)lrp)[0];
        uint32_t bits = ((uint32_t*)lrp)[1];
        uint8_t *dst = (uint8_t *)hle_ptr(bits);
        for (int y = 0; y < 64; y++)
            memset(dst + (size_t)y * pitch, 0, (size_t)128 * 4);
        uint32_t ua[1] = { surf };
        call("IDirect3DSurface9::UnlockRect", 1, ua);
    }
    hle_free(lrp);
}

static int count_nonblack(uint32_t dev, int w, int h) {
    uint32_t pp = hle_va(hle_alloc(4));
    uint32_t a[3] = { dev, 0, pp };
    if (call("IDirect3DDevice9::GetRenderTarget", 3, a) != D3D_OK) return 0;
    uint32_t surf = MEM32(pp);
    if (!surf) return 0;
    void *lrp = hle_alloc(8);
    uint32_t lr_va = hle_va(lrp);
    uint32_t la[4] = { surf, lr_va, 0, 0 };
    int n = 0;
    if (call("IDirect3DSurface9::LockRect", 4, la) == D3D_OK) {
        int32_t pitch = ((int32_t*)lrp)[0];
        uint32_t bits = ((uint32_t*)lrp)[1];
        uint8_t *dst = (uint8_t *)hle_ptr(bits);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint32_t px = *(uint32_t *)(dst + (size_t)y * pitch + (size_t)x * 4);
                if ((px & 0x00FFFFFFu) != 0) n++;
            }
        uint32_t ua[1] = { surf };
        call("IDirect3DSurface9::UnlockRect", 1, ua);
    }
    hle_free(lrp);
    return n;
}

/* Count lit pixels in either half of the render target. */
static void halves(uint32_t dev, int *left, int *right) {
    *left = *right = 0;
    uint32_t pp = hle_va(hle_alloc(4));
    uint32_t a[3] = { dev, 0, pp };
    if (call("IDirect3DDevice9::GetRenderTarget", 3, a) != D3D_OK) return;
    uint32_t surf = MEM32(pp);
    if (!surf) return;
    void *lrp = hle_alloc(8);
    uint32_t la[4] = { surf, hle_va(lrp), 0, 0 };
    if (call("IDirect3DSurface9::LockRect", 4, la) == D3D_OK) {
        int32_t pitch = ((int32_t*)lrp)[0];
        uint32_t bits = ((uint32_t*)lrp)[1];
        uint8_t *dst = (uint8_t *)hle_ptr(bits);
        for (int y = 0; y < 64; y++) for (int x = 0; x < 128; x++) {
            uint32_t px = *(uint32_t *)(dst + (size_t)y * pitch + (size_t)x * 4);
            if ((px & 0x00FFFFFFu) != 0) { if (x < 64) (*left)++; else (*right)++; }
        }
        uint32_t ua[1] = { surf };
        call("IDirect3DSurface9::UnlockRect", 1, ua);
    }
    hle_free(lrp);
}

/* Count lit pixels in top/bottom halves (vertical placement checks). */
static void vbands(uint32_t dev, int *top, int *bottom) {
    *top = *bottom = 0;
    uint32_t pp = hle_va(hle_alloc(4));
    uint32_t a[3] = { dev, 0, pp };
    if (call("IDirect3DDevice9::GetRenderTarget", 3, a) != D3D_OK) return;
    uint32_t surf = MEM32(pp);
    if (!surf) return;
    void *lrp = hle_alloc(8);
    uint32_t la[4] = { surf, hle_va(lrp), 0, 0 };
    if (call("IDirect3DSurface9::LockRect", 4, la) == D3D_OK) {
        int32_t pitch = ((int32_t*)lrp)[0];
        uint32_t bits = ((uint32_t*)lrp)[1];
        uint8_t *dst = (uint8_t *)hle_ptr(bits);
        for (int y = 0; y < 64; y++) for (int x = 0; x < 128; x++) {
            uint32_t px = *(uint32_t *)(dst + (size_t)y * pitch + (size_t)x * 4);
            if ((px & 0x00FFFFFFu) != 0) { if (y < 32) (*top)++; else (*bottom)++; }
        }
        uint32_t ua[1] = { surf };
        call("IDirect3DSurface9::UnlockRect", 1, ua);
    }
    hle_free(lrp);
}

int main(void) {
    recomp_host_init();
    win32hle_register(win32hle_d3dx9);
    win32hle_register(win32hle_d3d9);
    void *stack = hle_alloc(4096);
    g_stack = hle_va(stack);

    uint32_t a_create[1] = { 32 };
    uint32_t d3d9 = call("Direct3DCreate9", 1, a_create);
    ok("Direct3DCreate9", d3d9 != 0);

    D3DPRESENT_PARAMETERS *pp = (D3DPRESENT_PARAMETERS *)hle_alloc(sizeof(*pp));
    memset(pp, 0, sizeof(*pp));
    pp->BackBufferWidth = 128; pp->BackBufferHeight = 64;
    pp->BackBufferFormat = D3DFMT_A8R8G8B8; pp->BackBufferCount = 1;
    pp->Windowed = 1; pp->SwapEffect = 1;
    uint32_t *pDev = (uint32_t *)hle_alloc(4);
    uint32_t a_cdev[7] = { d3d9, 0, D3DDEVTYPE_HAL, 0,
                             D3DCREATE_SOFTWARE_VERTEXPROCESSING, hle_va(pp), hle_va(pDev) };
    uint32_t hr = call("IDirect3D9::CreateDevice", 7, a_cdev);
    ok("CreateDevice", hr == D3D_OK && *pDev != 0);
    uint32_t dev = *pDev;

    char *face = (char *)hle_alloc(16); strcpy(face, "Arial");
    uint32_t *ppf = (uint32_t *)hle_alloc(4);
    uint32_t args[12] = { dev, 16, 0, 400, 1, 0, 0, 0, 0, 0, (uint32_t)hle_va(face), (uint32_t)hle_va(ppf) };
    hr = call("D3DXCreateFontA", 12, args);
    ok("D3DXCreateFontA", hr == D3D_OK && *ppf != 0);
    g_font = *ppf;
    g_font_vt = MEM32(g_font);
    ok("font has a vtable", g_font_vt != 0);

    /* ---- Vtable audit: slot count and order against the public source ---- */
    ok("ID3DXFONT_SLOTS == public 18", ID3DXFONT_SLOTS == PUBLIC_ID3DXFONT_SLOTS);
    for (int i = 0; i < PUBLIC_ID3DXFONT_SLOTS; i++) {
        uint32_t va = slot_va(i);
        ok(slot_names[i], va != 0 && va == hle_resolve(slot_names[i]));
    }
    ok("no slot beyond index 17", MEM32(g_font_vt + 4u * PUBLIC_ID3DXFONT_SLOTS) == 0);

    /* ---- Every slot: real vtable call, exact stdcall stack balance ---- */
    uint32_t eax;
    uint32_t ppv = hle_va(hle_alloc(4));
    uint32_t a_qi[3] = { g_font, iid_unk(), ppv };
    ok("slot 0 QueryInterface stack", raw_vcall(S_QI, 3, a_qi, &eax) == g_stack + 0x800u);
    ok("QueryInterface returns font", MEM32(ppv) == g_font && eax == 0);
    uint32_t a_ar[1] = { g_font };
    ok("slot 1 AddRef stack", raw_vcall(S_ADDREF, 1, a_ar, &eax) == g_stack + 0x800u);
    ok("AddRef raises count", eax == 3);
    uint32_t a_rel[1] = { g_font };
    ok("slot 2 Release stack", raw_vcall(S_RELEASE, 1, a_rel, &eax) == g_stack + 0x800u);
    ok("Release lowers count", eax == 2);

    uint32_t ppdev = hle_va(hle_alloc(4));
    uint32_t a_gd[2] = { g_font, ppdev };
    ok("slot 3 GetDevice stack", raw_vcall(S_GETDEV, 2, a_gd, &eax) == g_stack + 0x800u);
    ok("GetDevice device and HRESULT", eax == D3D_OK && MEM32(ppdev) == dev);

    uint8_t *descA = (uint8_t *)hle_alloc(88);
    uint32_t a_da[2] = { g_font, hle_va(descA) };
    ok("slot 4 GetDescA stack", raw_vcall(S_GETDESCA, 2, a_da, &eax) == g_stack + 0x800u);
    ok("GetDescA HRESULT", eax == D3D_OK);
    uint8_t *descW = (uint8_t *)hle_alloc(88);
    uint32_t a_dw[2] = { g_font, hle_va(descW) };
    ok("slot 5 GetDescW stack", raw_vcall(S_GETDESCW, 2, a_dw, &eax) == g_stack + 0x800u);
    ok("GetDescW HRESULT", eax == D3D_OK);

    uint8_t *tmA = (uint8_t *)hle_alloc(60);
    uint32_t a_ta[2] = { g_font, hle_va(tmA) };
    ok("slot 6 GetTextMetricsA stack", raw_vcall(S_GETTMA, 2, a_ta, &eax) == g_stack + 0x800u);
    ok("GetTextMetricsA BOOL", eax == 1);
    uint8_t *tmW = (uint8_t *)hle_alloc(60);
    uint32_t a_tw[2] = { g_font, hle_va(tmW) };
    ok("slot 7 GetTextMetricsW stack", raw_vcall(S_GETTMW, 2, a_tw, &eax) == g_stack + 0x800u);
    ok("GetTextMetricsW BOOL", eax == 1);

    uint32_t a_dc[1] = { g_font };
    ok("slot 8 GetDC stack", raw_vcall(S_GETDC, 1, a_dc, &eax) == g_stack + 0x800u);
    ok("GetDC has no host HDC", eax == 0);

    uint32_t pptex = hle_va(hle_alloc(4));
    uint32_t blackbox = hle_va(hle_alloc(16));
    uint32_t cellinc = hle_va(hle_alloc(8));
    uint32_t a_gl[5] = { g_font, 'A', pptex, blackbox, cellinc };
    ok("slot 9 GetGlyphData stack", raw_vcall(S_GETGLYPH, 5, a_gl, &eax) == g_stack + 0x800u);
    ok("GetGlyphData failure", eax == 0x8876086Cu);

    uint32_t a_pc[3] = { g_font, 32, 126 };
    ok("slot 10 PreloadCharacters stack", raw_vcall(S_PRECHAR, 3, a_pc, &eax) == g_stack + 0x800u);
    ok("PreloadCharacters failure", eax == 0x8876086Cu);
    uint32_t a_pg[3] = { g_font, 32, 126 };
    ok("slot 11 PreloadGlyphs stack", raw_vcall(S_PREGLYPH, 3, a_pg, &eax) == g_stack + 0x800u);
    ok("PreloadGlyphs failure", eax == 0x8876086Cu);
    uint32_t strA = hle_va(hle_alloc(8)); strcpy((char *)hle_ptr(strA), "Hi");
    uint32_t a_pta[3] = { g_font, strA, (uint32_t)-1 };
    ok("slot 12 PreloadTextA stack", raw_vcall(S_PRETEXTA, 3, a_pta, &eax) == g_stack + 0x800u);
    ok("PreloadTextA failure", eax == 0x8876086Cu);
    uint16_t *wstr = (uint16_t *)hle_alloc(8); wstr[0]='H'; wstr[1]='i'; wstr[2]=0;
    uint32_t a_ptw[3] = { g_font, hle_va(wstr), (uint32_t)-1 };
    ok("slot 13 PreloadTextW stack", raw_vcall(S_PRETEXTW, 3, a_ptw, &eax) == g_stack + 0x800u);
    ok("PreloadTextW failure", eax == 0x8876086Cu);

    char *probe = (char *)hle_alloc(8); strcpy(probe, "X");
    int *rprobe = (int *)hle_alloc(16);
    rprobe[0]=0; rprobe[1]=0; rprobe[2]=128; rprobe[3]=64;
    uint32_t a_da2[7] = { g_font, 0, (uint32_t)hle_va(probe), -1,
                          hle_va(rprobe), DT_CALCRECT, 0 };
    ok("slot 14 DrawTextA stack", raw_vcall(S_DRAWTEXTA, 7, a_da2, &eax) == g_stack + 0x800u);
    ok("DrawTextA CALCRECT height", eax > 0);
    uint16_t *wp = (uint16_t *)hle_alloc(8); wp[0]='X'; wp[1]=0;
    uint32_t a_dw2[7] = { g_font, 0, hle_va(wp), -1,
                          hle_va(rprobe), DT_CALCRECT, 0 };
    ok("slot 15 DrawTextW stack", raw_vcall(S_DRAWTEXTW, 7, a_dw2, &eax) == g_stack + 0x800u);
    ok("DrawTextW CALCRECT height", eax > 0);

    uint32_t a_lost[1] = { g_font };
    ok("slot 16 OnLostDevice stack", raw_vcall(S_LOST, 1, a_lost, &eax) == g_stack + 0x800u);
    ok("OnLostDevice HRESULT", eax == D3D_OK);
    uint32_t a_reset[1] = { g_font };
    ok("slot 17 OnResetDevice stack", raw_vcall(S_RESET, 1, a_reset, &eax) == g_stack + 0x800u);
    ok("OnResetDevice HRESULT", eax == D3D_OK);

    /* ---- Functional checks, all through the real vtable slots ---- */

    /* CALCRECT: return value is the height of the text, not a char count. */
    char *text = (char *)hle_alloc(8); strcpy(text, "Hi");
    int *rect = (int *)hle_alloc(16); rect[0]=0; rect[1]=0; rect[2]=0; rect[3]=0;
    uint32_t rva = hle_va(rect);
    uint32_t a_draw[7] = { g_font, 0, (uint32_t)hle_va(text), -1, rva, DT_CALCRECT, 0xFFFFFFFFu };
    uint32_t ret = fcall(S_DRAWTEXTA, 7, a_draw);
    ok("DrawTextA CALCRECT returns height > 0", ret > 0);
    ok("CALCRECT width > 0", rect[2] > 0);
    ok("CALCRECT height > 0", rect[3] > 0);
    ok("CALCRECT ret == height", (int)ret == (rect[3] - rect[1]));

    /* DrawTextW with Count=-1 actually draws pixels. */
    clear_rt(dev);
    int *rectW = (int *)hle_alloc(16); rectW[0]=0; rectW[1]=0; rectW[2]=128; rectW[3]=64;
    uint32_t a_drawW[7] = { g_font, 0, hle_va(wstr), (uint32_t)-1,
                            hle_va(rectW), DT_LEFT | DT_TOP, 0xFFFF0000u };
    ret = fcall(S_DRAWTEXTW, 7, a_drawW);
    ok("DrawTextW returns height > 0", ret > 0);
    ok("DrawTextW draws pixels", count_nonblack(dev, 128, 64) > 0);

    /* Left alignment: lit pixels sit predominantly in the left half. */
    clear_rt(dev);
    int *r2 = (int *)hle_alloc(16); r2[0]=0; r2[1]=0; r2[2]=128; r2[3]=64;
    uint32_t a2[7] = { g_font, 0, (uint32_t)hle_va(text), -1, hle_va(r2),
                       DT_LEFT | DT_TOP, 0xFFFF0000u };
    ret = fcall(S_DRAWTEXTA, 7, a2);
    ok("DrawTextA render returns height", ret > 0);
    int l=0, r=0; halves(dev, &l, &r);
    ok("left-aligned text has pixels", (l + r) > 0);
    ok("left-aligned text mostly left", l >= r);

    /* DT_RIGHT: lit pixels sit predominantly in the right half. */
    clear_rt(dev);
    int *rR = (int *)hle_alloc(16); rR[0]=0; rR[1]=0; rR[2]=128; rR[3]=64;
    uint32_t aR[7] = { g_font, 0, (uint32_t)hle_va(text), -1, hle_va(rR),
                       DT_RIGHT | DT_TOP, 0xFF00FF00u };
    fcall(S_DRAWTEXTA, 7, aR);
    l=0; r=0; halves(dev, &l, &r);
    ok("right-aligned text has pixels", (l + r) > 0);
    ok("right-aligned text mostly right", r >= l);

    /* DT_CENTER: lit pixels sit near the horizontal middle. */
    clear_rt(dev);
    int *rC = (int *)hle_alloc(16); rC[0]=0; rC[1]=0; rC[2]=128; rC[3]=64;
    uint32_t aC[7] = { g_font, 0, (uint32_t)hle_va(text), -1, hle_va(rC),
                       DT_CENTER | DT_TOP, 0xFFFFFFFFu };
    fcall(S_DRAWTEXTA, 7, aC);
    l=0; r=0; halves(dev, &l, &r);
    ok("center-aligned text has pixels", (l + r) > 0);
    /* Both halves should carry glyph pixels (text straddles x=64). */
    ok("center-aligned text spans middle", l > 0 && r > 0);

    /* DT_BOTTOM and DT_VCENTER: vertical placement. */
    clear_rt(dev);
    int *rB = (int *)hle_alloc(16); rB[0]=0; rB[1]=0; rB[2]=128; rB[3]=64;
    uint32_t aB[7] = { g_font, 0, (uint32_t)hle_va(text), -1, hle_va(rB),
                       DT_LEFT | DT_BOTTOM, 0xFFFFFFFFu };
    fcall(S_DRAWTEXTA, 7, aB);
    int t=0, b=0; vbands(dev, &t, &b);
    ok("bottom-aligned text mostly bottom", b >= t && (t + b) > 0);

    clear_rt(dev);
    int *rV = (int *)hle_alloc(16); rV[0]=0; rV[1]=0; rV[2]=128; rV[3]=64;
    uint32_t aV[7] = { g_font, 0, (uint32_t)hle_va(text), -1, hle_va(rV),
                       DT_LEFT | DT_VCENTER, 0xFFFFFFFFu };
    fcall(S_DRAWTEXTA, 7, aV);
    t=0; b=0; vbands(dev, &t, &b);
    ok("vcenter text has pixels", (t + b) > 0);

    /* DT_WORDBREAK: four words that fit one word but not two must wrap to
     * exactly the height of four explicit newlines. */
    char *one = (char *)hle_alloc(8); strcpy(one, "X");
    int *rOne = (int *)hle_alloc(16); rOne[0]=0; rOne[1]=0; rOne[2]=512; rOne[3]=64;
    uint32_t aOne[7] = { g_font, 0, (uint32_t)hle_va(one), -1, hle_va(rOne), DT_CALCRECT, 0 };
    int lineh = (int)fcall(S_DRAWTEXTA, 7, aOne);
    int onew = rOne[2] - rOne[0];
    ok("single line height known", lineh > 0 && onew > 0);

    char *nl = (char *)hle_alloc(8); strcpy(nl, "X\nX\nX\nX");
    int *rNl = (int *)hle_alloc(16); rNl[0]=0; rNl[1]=0; rNl[2]=512; rNl[3]=64;
    uint32_t aNl[7] = { g_font, 0, (uint32_t)hle_va(nl), -1, hle_va(rNl), DT_CALCRECT, 0 };
    int exp4 = (int)fcall(S_DRAWTEXTA, 7, aNl);
    ok("explicit 4 lines height", exp4 == 4 * lineh);

    char *words = (char *)hle_alloc(8); strcpy(words, "X X X X");
    int narrow = onew + onew / 2;
    int *rW2 = (int *)hle_alloc(16); rW2[0]=0; rW2[1]=0; rW2[2]=narrow; rW2[3]=64;
    uint32_t aWb[7] = { g_font, 0, (uint32_t)hle_va(words), -1, hle_va(rW2),
                        DT_CALCRECT | DT_WORDBREAK, 0xFFFFFFFFu };
    ret = fcall(S_DRAWTEXTA, 7, aWb);
    ok("WORDBREAK wraps to 4 lines", (int)ret == exp4 && (int)ret == 4 * lineh);
    ok("WORDBREAK rect height matches return", rW2[3] - rW2[1] == (int)ret);
    ok("WORDBREAK rect width <= given width", rW2[2] - rW2[0] <= narrow);

    /* GetTextMetricsA: 56-byte TEXTMETRICA layout, BOOL return. */
    uint8_t *tm = (uint8_t *)hle_alloc(64);
    memset(tm, 0xAA, 64);
    uint32_t a_tm[2] = { g_font, hle_va(tm) };
    ok("GetTextMetricsA BOOL", fcall(S_GETTMA, 2, a_tm) == 1);
    ok("tmHeight > 0", *(int32_t*)tm > 0);
    ok("tmAscent > 0", *(int32_t*)(tm+4) > 0);
    ok("tmAveCharWidth > 0", *(int32_t*)(tm+20) > 0);
    ok("tmWeight == 400", *(int32_t*)(tm+28) == 400);
    ok("tmFirstChar", tm[44] == 32);

    /* GetTextMetricsW: 60-byte TEXTMETRICW, WCHAR fields at 44/46/48/50. */
    uint8_t *tmw = (uint8_t *)hle_alloc(64);
    memset(tmw, 0xAA, 64);
    uint32_t a_tmw[2] = { g_font, hle_va(tmw) };
    ok("GetTextMetricsW BOOL", fcall(S_GETTMW, 2, a_tmw) == 1);
    ok("tmWeight W == 400", *(int32_t*)(tmw+28) == 400);
    ok("tmFirstChar W == 32", *(uint16_t*)(tmw+44) == 32);
    ok("tmCharSet W", tmw[56] == 0);

    /* GetDescA: 56-byte D3DXFONT_DESCA, FaceName at offset 24. */
    uint8_t *desc = (uint8_t *)hle_alloc(64);
    memset(desc, 0xCC, 64);
    uint32_t a_desc[2] = { g_font, hle_va(desc) };
    ok("GetDescA HRESULT", fcall(S_GETDESCA, 2, a_desc) == D3D_OK);
    ok("GetDesc Height", *(int32_t*)(desc+0) == 16);
    ok("GetDesc Weight", *(uint32_t*)(desc+8) == 400);
    ok("GetDesc Italic", *(int32_t*)(desc+16) == 0);
    ok("GetDesc FaceName at 24", strcmp((char*)(desc+24), "Arial") == 0);
    ok("GetDesc CharSet/OutPrec/Quality/Pitch", desc[20]==0 && desc[21]==0 && desc[22]==0 && desc[23]==0);

    /* GetDescW: 88-byte D3DXFONT_DESCW, FaceName is UTF-16 at offset 24. */
    memset(descW, 0xCC, 88);
    ok("GetDescW HRESULT", fcall(S_GETDESCW, 2, a_dw) == D3D_OK);
    ok("GetDescW Height", *(int32_t*)(descW+0) == 16);
    const char *arial = "Arial";
    int face_ok = 1;
    for (int i = 0; i < 5; i++)
        if (*(uint16_t *)(descW + 24 + 2*i) != (unsigned char)arial[i]) face_ok = 0;
    if (*(uint16_t *)(descW + 24 + 10) != 0) face_ok = 0;
    ok("GetDescW FaceName UTF-16", face_ok);

    /* DrawTextW must convert UTF-16 to UTF-8, not truncate to one byte. */
    uint16_t *uml = (uint16_t *)hle_alloc(8);
    uml[0] = 0x00C4; uml[1] = 0;
    int *rU = (int *)hle_alloc(16); rU[0]=0; rU[1]=0; rU[2]=128; rU[3]=64;
    uint32_t aU[7] = { g_font, 0, hle_va(uml), -1, hle_va(rU), DT_CALCRECT, 0 };
    ret = fcall(S_DRAWTEXTW, 7, aU);
    ok("DrawTextW non-ASCII converts", ret > 0 && rU[2] > rU[0]);

    /* D3DXCreateFontW: UTF-16 face name. */
    uint16_t *faceW = (uint16_t *)hle_alloc(32);
    for (int i = 0; i <= 5; i++) faceW[i] = (uint16_t)(unsigned char)arial[i];
    uint32_t *ppw = (uint32_t *)hle_alloc(4);
    uint32_t a_cw[12] = { dev, 16, 0, 400, 1, 0, 0, 0, 0, 0, hle_va(faceW), hle_va(ppw) };
    ok("D3DXCreateFontW", call("D3DXCreateFontW", 12, a_cw) == D3D_OK && *ppw != 0);
    uint32_t old = g_font, oldvt = g_font_vt;
    g_font = *ppw; g_font_vt = MEM32(g_font);
    uint8_t *descFW = (uint8_t *)hle_alloc(88);
    uint32_t a_dFW[2] = { g_font, hle_va(descFW) };
    ok("D3DXCreateFontW GetDescA", fcall(S_GETDESCA, 2, a_dFW) == D3D_OK);
    ok("D3DXCreateFontW FaceName", strcmp((char*)(descFW + 24), "Arial") == 0);
    g_font = old; g_font_vt = oldvt;

    /* D3DXCreateFontIndirectA: 56-byte D3DXFONT_DESCA. */
    D3DXFONT_DESCA *fd = (D3DXFONT_DESCA *)hle_alloc(sizeof *fd);
    memset(fd, 0, sizeof *fd);
    fd->Height = 20; fd->Width = 7; fd->Weight = 700; fd->MipLevels = 2; fd->Italic = 1;
    fd->CharSet = 1; fd->OutputPrecision = 2; fd->Quality = 3; fd->PitchAndFamily = 4;
    strcpy(fd->FaceName, "Arial");
    uint32_t *ppi = (uint32_t *)hle_alloc(4);
    uint32_t a_ci[3] = { dev, hle_va(fd), hle_va(ppi) };
    ok("D3DXCreateFontIndirectA", call("D3DXCreateFontIndirectA", 3, a_ci) == D3D_OK && *ppi != 0);
    g_font = *ppi; g_font_vt = MEM32(g_font);
    uint8_t *descI = (uint8_t *)hle_alloc(64);
    uint32_t a_di[2] = { g_font, hle_va(descI) };
    ok("D3DXCreateFontIndirectA GetDescA", fcall(S_GETDESCA, 2, a_di) == D3D_OK);
    ok("IndirectA Height", *(int32_t*)(descI + 0) == 20);
    ok("IndirectA Weight", *(uint32_t*)(descI + 8) == 700);
    ok("IndirectA byte fields", descI[20]==1 && descI[21]==2 && descI[22]==3 && descI[23]==4);
    ok("IndirectA FaceName", strcmp((char*)(descI + 24), "Arial") == 0);
    g_font = old; g_font_vt = oldvt;

    /* D3DXCreateFontIndirectW: FaceName is UTF-16 at offset 24. */
    uint8_t *fdw = (uint8_t *)hle_alloc(88);
    memset(fdw, 0, 88);
    *(int32_t *)(fdw + 0) = 22;
    *(uint32_t *)(fdw + 8) = 600;
    fdw[20] = 5; fdw[21] = 6; fdw[22] = 7; fdw[23] = 8;
    for (int i = 0; i <= 5; i++) ((uint16_t *)(fdw + 24))[i] = (uint16_t)(unsigned char)arial[i];
    uint32_t *ppiw = (uint32_t *)hle_alloc(4);
    uint32_t a_ciw[3] = { dev, hle_va(fdw), hle_va(ppiw) };
    ok("D3DXCreateFontIndirectW", call("D3DXCreateFontIndirectW", 3, a_ciw) == D3D_OK && *ppiw != 0);
    g_font = *ppiw; g_font_vt = MEM32(g_font);
    uint8_t *descIW = (uint8_t *)hle_alloc(64);
    uint32_t a_diw[2] = { g_font, hle_va(descIW) };
    ok("D3DXCreateFontIndirectW GetDescA", fcall(S_GETDESCA, 2, a_diw) == D3D_OK);
    ok("IndirectW Height", *(int32_t*)(descIW + 0) == 22);
    ok("IndirectW Weight", *(uint32_t*)(descIW + 8) == 600);
    ok("IndirectW byte fields", descIW[20]==5 && descIW[21]==6 && descIW[22]==7 && descIW[23]==8);
    ok("IndirectW FaceName", strcmp((char*)(descIW + 24), "Arial") == 0);
    g_font = old; g_font_vt = oldvt;

    /* Error paths modelled on Wine: null device/count return INVALIDCALL/0. */
    uint32_t a_badcre[12] = { 0, 16, 0, 400, 1, 0, 0, 0, 0, 0,
                             (uint32_t)hle_va(face), (uint32_t)hle_va(ppf) };
    ok("CreateFontA null device", call("D3DXCreateFontA", 12, a_badcre) == 0x8876086Cu);
    uint32_t a_badind[3] = { 0, hle_va(fd), hle_va(ppi) };
    ok("CreateFontIndirectA null device", call("D3DXCreateFontIndirectA", 3, a_badind) == 0x8876086Cu);
    uint32_t a_zero[7] = { g_font, 0, (uint32_t)hle_va(text), 0,
                           hle_va(r2), 0, 0 };
    ok("DrawTextA count=0 returns 0", fcall(S_DRAWTEXTA, 7, a_zero) == 0);
    uint32_t a_zw[7] = { g_font, 0, hle_va(wstr), 0, hle_va(rprobe), 0, 0 };
    ok("DrawTextW count=0 returns 0", fcall(S_DRAWTEXTW, 7, a_zw) == 0);

    /* Lost device: DrawText returns 0, metrics fail; Reset restores them. */
    uint32_t a_lost2[1] = { g_font };
    ok("OnLostDevice", fcall(S_LOST, 1, a_lost2) == D3D_OK);
    clear_rt(dev);
    int *rLost = (int *)hle_alloc(16); rLost[0]=0; rLost[1]=0; rLost[2]=128; rLost[3]=64;
    uint32_t aLost[7] = { g_font, 0, (uint32_t)hle_va(text), -1, hle_va(rLost), 0, 0 };
    ok("DrawText while lost returns 0", fcall(S_DRAWTEXTA, 7, aLost) == 0);
    ok("DrawText while lost draws nothing", count_nonblack(dev, 128, 64) == 0);
    uint32_t a_tm_lost[2] = { g_font, hle_va(tm) };
    ok("GetTextMetricsA while lost returns 0", fcall(S_GETTMA, 2, a_tm_lost) == 0);
    ok("OnResetDevice", fcall(S_RESET, 1, a_lost2) == D3D_OK);
    uint32_t aAfter[7] = { g_font, 0, (uint32_t)hle_va(text), -1, hle_va(rLost), 0, 0 };
    ok("DrawText after reset works", fcall(S_DRAWTEXTA, 7, aAfter) > 0);

    if (!fails) printf("d3dx9_font_selftest: all checks passed\n");
    return fails != 0;
}
