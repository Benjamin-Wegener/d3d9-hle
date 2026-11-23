/*
 * d3dx9_sprite_selftest.c - D3DXCreateSprite / ID3DXSprite.
 *
 * The sprite draws through the IDirect3DDevice9 vtable it is given, so the test
 * hands it a mock device (a guest COM object whose vtable records the calls a
 * sprite makes): GetLevelDesc, SetTexture and DrawPrimitiveUP. It then checks
 * the geometry (quad corner positions), the UVs (rect / texture size), the
 * per-vertex colour, that the sprite transform is applied, and that Begin/End
 * capture and restore the device state (the state block). The exact stdcall ABI
 * is used throughout: methods are invoked by vtable slot, floats by value.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "win32hle.h"
#include "host.h"
#include "d3dx9_hle.h"

#define D3D_OK             0u
#define D3DERR_INVALIDCALL 0x8876086Cu

const recomp_dispatch_entry_t recomp_dispatch_table[1];
const uint32_t recomp_dispatch_count = 0;

static int fails;
static void ok(const char *what, int cond) { if (!cond) { printf("FAIL %s\n", what); fails++; } }

static uint32_t call(const char *name, int n, const uint32_t *a) { return hle_call_guest(hle_resolve(name), n, a); }
static uint32_t method(uint32_t obj, int i) { return MEM32(MEM32(obj) + 4u * (uint32_t)i); }
static int nearf(float a, float b) { return fabsf(a - b) < 1e-3f; }

/* ---- what the mock device records ---- */
static uint32_t g_texw = 64, g_texh = 32;
static uint32_t g_bound_tex, g_primcount, g_stride, g_draws;
static uint8_t  g_verts[64 * 24];
static uint32_t g_capture, g_apply, g_decl_release, g_sb_release, g_begin_stateblock;
static uint32_t g_setrs, g_capture_before_setrs;

/* vertex accessor: XYZ, D3DCOLOR, UV (24 bytes) */
static float vgf(uint32_t k, uint32_t off) { uint32_t o; memcpy(&o, g_verts + k * 24 + off, 4); float f; memcpy(&f, &o, 4); return f; }
static float vx(uint32_t k) { return vgf(k, 0); }
static float vy(uint32_t k) { return vgf(k, 4); }
static float vz(uint32_t k) { return vgf(k, 8); }
static uint32_t vcol(uint32_t k) { uint32_t o; memcpy(&o, g_verts + k * 24 + 12, 4); return o; }
static float vu(uint32_t k) { return vgf(k, 16); }
static float vw(uint32_t k) { return vgf(k, 20); }

/* resolved mock method VAs */
static uint32_t s_noop, s_addref, s_release, s_getlevel, s_settex, s_draw, s_createdecl, s_declr;
static uint32_t s_beginblock, s_endblock, s_capture, s_apply, s_getvp, s_sb_release;
static uint32_t s_createblock, s_setrs;
static uint32_t s_sbvt, s_declvt;

/* ---- mock device/resource shims (stdcall; `this` is arg 0) ---- */
static void m_AddRef(void)  { RET(++MEM32(A32(0) + 4), 1); }
static void m_Release(void) { RET(--MEM32(A32(0) + 4), 1); }
static void m_noop(void)    { RET(0, 0); }
static void m_DeclRelease(void) { g_decl_release++; RET(--MEM32(A32(0) + 4), 1); }
static void m_Capture(void) { if (g_setrs == 0) g_capture_before_setrs = 1; g_capture++; RET(D3D_OK, 1); }
static void m_Apply(void)   { g_apply++;   RET(D3D_OK, 1); }
static void m_SBRelease(void) { g_sb_release++; RET(--MEM32(A32(0) + 4), 1); }
static void m_BeginStateBlock(void) { g_begin_stateblock++; RET(D3D_OK, 1); }
static void m_SetRenderState(void) { g_setrs++; RET(D3D_OK, 3); }
static void m_CreateStateBlock(void) {              /* (this, type, **out) */
    uint32_t out = A32(2);
    if (out) MEM32(out) = hle_com_new(s_sbvt, 8);
    RET(D3D_OK, 3);
}

static void m_GetLevelDesc(void) {                /* (this, level, *desc) */
    uint32_t d = A32(2);
    if (d) { MEM32(d + 24) = g_texw; MEM32(d + 28) = g_texh; }
    RET(D3D_OK, 3);
}
static void m_SetTexture(void) { g_bound_tex = A32(2); RET(D3D_OK, 3); }
static void m_DrawPrimitiveUP(void) {             /* (this, type, primcount, data, stride) */
    g_primcount = A32(2); g_stride = A32(4);
    uint32_t data = A32(3), bytes = g_primcount * 3u * 24u;
    if (data && bytes <= sizeof g_verts) memcpy(g_verts, hle_ptr(data), bytes);
    g_draws++;
    RET(D3D_OK, 5);
}
static void m_CreateVertexDeclaration(void) {     /* (this, elems, **out) */
    uint32_t out = A32(2);
    if (out) MEM32(out) = hle_com_new(s_declvt, 8);
    RET(D3D_OK, 3);
}
static void m_EndStateBlock(void) {               /* (this, **out) */
    uint32_t out = A32(1);
    if (out) MEM32(out) = hle_com_new(s_sbvt, 8);
    RET(D3D_OK, 2);
}
static void m_GetViewport(void) {                 /* (this, *vp) */
    uint32_t v = A32(1);
    /* X=0, Y=0, W=640, H=480, MinZ=0, MaxZ=1 */
    if (v) { MEM32(v+0)=0; MEM32(v+4)=0; MEM32(v+8)=640; MEM32(v+12)=480; MEM32(v+16)=0; MEM32(v+20)=0x3f800000u; }
    RET(D3D_OK, 2);
}

static const win32hle_shim mock_methods[] = {
    { "sprite_test::AddRef", m_AddRef },
    { "sprite_test::Release", m_Release },
    { "sprite_test::Noop", m_noop },
    { "sprite_test::DeclRelease", m_DeclRelease },
    { "sprite_test::Capture", m_Capture },
    { "sprite_test::Apply", m_Apply },
    { "sprite_test::SBRelease", m_SBRelease },
    { "sprite_test::BeginStateBlock", m_BeginStateBlock },
    { "sprite_test::GetLevelDesc", m_GetLevelDesc },
    { "sprite_test::SetTexture", m_SetTexture },
    { "sprite_test::DrawPrimitiveUP", m_DrawPrimitiveUP },
    { "sprite_test::CreateVertexDeclaration", m_CreateVertexDeclaration },
    { "sprite_test::EndStateBlock", m_EndStateBlock },
    { "sprite_test::GetViewport", m_GetViewport },
    { "sprite_test::SetRenderState", m_SetRenderState },
    { "sprite_test::CreateStateBlock", m_CreateStateBlock },
    { 0, 0 }
};

/* Build a guest vtable of n slots filled with noop, then override some. */
static uint32_t build_vt(uint32_t n, const int *slots, const uint32_t *vals, int nset) {
    uint32_t *vt = (uint32_t *)hle_alloc(4u * n);
    for (uint32_t i = 0; i < n; i++) vt[i] = s_noop;
    for (int i = 0; i < nset; i++) vt[slots[i]] = vals[i];
    return hle_va(vt);
}

int main(void) {
    recomp_host_init();
    win32hle_register(win32hle_d3dx9);
    win32hle_register(mock_methods);

    s_noop     = hle_resolve("sprite_test::Noop");
    s_addref   = hle_resolve("sprite_test::AddRef");
    s_release  = hle_resolve("sprite_test::Release");
    s_getlevel = hle_resolve("sprite_test::GetLevelDesc");
    s_settex   = hle_resolve("sprite_test::SetTexture");
    s_draw     = hle_resolve("sprite_test::DrawPrimitiveUP");
    s_createdecl = hle_resolve("sprite_test::CreateVertexDeclaration");
    s_declr    = hle_resolve("sprite_test::DeclRelease");
    s_beginblock = hle_resolve("sprite_test::BeginStateBlock");
    s_endblock = hle_resolve("sprite_test::EndStateBlock");
    s_capture  = hle_resolve("sprite_test::Capture");
    s_apply    = hle_resolve("sprite_test::Apply");
    s_getvp    = hle_resolve("sprite_test::GetViewport");
    s_sb_release = hle_resolve("sprite_test::SBRelease");
    s_setrs = hle_resolve("sprite_test::SetRenderState");
    s_createblock = hle_resolve("sprite_test::CreateStateBlock");

    /* resource vtables: decl has Release at slot 2; stateblock Capture(4)/Apply(5)/Release(2) */
    { int ds[1] = { 2 }; uint32_t dv[1] = { s_declr };    s_declvt = build_vt(5, ds, dv, 1); }
    { int ss[3] = { 2, 4, 5 }; uint32_t sv[3] = { s_sb_release, s_capture, s_apply };
      s_sbvt = build_vt(6, ss, sv, 3); }

    /* mock texture (AddRef 1, Release 2, GetLevelDesc 17) */
    { int ts[3] = { 1, 2, 17 }; uint32_t tv[3] = { s_addref, s_release, s_getlevel };
      uint32_t texvt = build_vt(22, ts, tv, 3);
      uint32_t texture = hle_com_new(texvt, 8);

      /* mock device */
      int dslots[10] = { 1, 2, 48, 57, 59, 60, 61, 65, 83, 86 };
      uint32_t dvals[10] = { s_addref, s_release, s_getvp, s_setrs, s_createblock,
                             s_beginblock, s_endblock, s_settex, s_draw, s_createdecl };
      uint32_t devvt = build_vt(119, dslots, dvals, 10);
      uint32_t device = hle_com_new(devvt, 8);

      /* create the sprite */
      uint32_t out = hle_va(hle_alloc(4));
      ok("D3DXCreateSprite", call("D3DXCreateSprite", 2, (uint32_t[]){ device, out }) == D3D_OK);
      uint32_t sprite = MEM32(out);
      ok("sprite object", sprite != 0);
      if (!sprite) { printf("d3dx9_sprite_selftest: no sprite\n"); return 1; }

      uint32_t mQI=method(sprite,0), mAR=method(sprite,1), mRel=method(sprite,2);
      uint32_t mGD=method(sprite,3), mGT=method(sprite,4), mST=method(sprite,5);
      uint32_t mBegin=method(sprite,8), mDraw=method(sprite,9), mFlush=method(sprite,10), mEnd=method(sprite,11);
      uint32_t mLost=method(sprite,12), mReset=method(sprite,13);
      ok("all 14 sprite vtable methods",
         mQI && mAR && mRel && mGD && mGT && mST && mBegin && mDraw && mFlush && mEnd && mLost && mReset);
      ok("D3DXCreateSprite AddRef'd the device", MEM32(device + 4) == 2u);

      /* GetDevice returns the device and AddRefs it */
      uint32_t devout = hle_va(hle_alloc(4));
      ok("GetDevice", hle_call_guest(mGD, 2, (uint32_t[]){ sprite, devout }) == D3D_OK && MEM32(devout) == device);
      ok("GetDevice AddRef", MEM32(device + 4) == 3u);

      /* SetTransform / GetTransform roundtrip, and NULL rejected */
      float T[16] = { 1,0,0,5, 0,1,0,0, 0,0,1,0, 0,0,0,1 };  /* translation x+5 (m[0][3]) */
      uint32_t matva = hle_va(hle_alloc(64)); memcpy(hle_ptr(matva), T, 64);
      ok("SetTransform", hle_call_guest(mST, 2, (uint32_t[]){ sprite, matva }) == D3D_OK);
      uint32_t got = hle_va(hle_alloc(64));
      ok("GetTransform", hle_call_guest(mGT, 2, (uint32_t[]){ sprite, got }) == D3D_OK && !memcmp(hle_ptr(got), T, 64));
      ok("SetTransform(NULL) fails", hle_call_guest(mST, 2, (uint32_t[]){ sprite, 0 }) == D3DERR_INVALIDCALL);

      /* Begin(0): state block captured, ready */
      ok("Begin", hle_call_guest(mBegin, 2, (uint32_t[]){ sprite, 0 }) == D3D_OK);
      ok("Begin captured state", g_capture == 1u);
      ok("state captured before sprite render states", g_capture_before_setrs == 1u && g_setrs > 0u);
      ok("second Begin fails", hle_call_guest(mBegin, 2, (uint32_t[]){ sprite, 0 }) == D3DERR_INVALIDCALL);
      ok("Begin(flags>511) fails", hle_call_guest(mBegin, 2, (uint32_t[]){ sprite, 512u }) == D3DERR_INVALIDCALL);

      /* Draw a full-texture quad at (10,20), colour 0xFF102030 */
      float pf[3] = { 10.0f, 20.0f, 0.0f }, cf[3] = { 0, 0, 0 };
      uint32_t pva = hle_va(hle_alloc(12)), cva = hle_va(hle_alloc(12));
      memcpy(hle_ptr(pva), pf, 12); memcpy(hle_ptr(cva), cf, 12);
      ok("Draw", hle_call_guest(mDraw, 6, (uint32_t[]){ sprite, texture, 0, cva, pva, 0xFF102030u }) == D3D_OK);
      ok("texture AddRef'd on Draw", MEM32(texture + 4) == 2u);

      /* Flush: 6 vertices (2 triangles), geometry / UV / colour, transform applied */
      g_primcount = 0; g_stride = 0; g_draws = 0; memset(g_verts, 0, sizeof g_verts);
      ok("Flush", hle_call_guest(mFlush, 1, (uint32_t[]){ sprite }) == D3D_OK);
      ok("one draw call", g_draws == 1u);
      ok("two triangles", g_primcount == 2u);
      ok("vertex stride 24", g_stride == 24u);
      ok("bound texture", g_bound_tex == texture);
      /* transform x+5 applied: x0 = 10-0+5 = 15 */
      ok("v0 pos", nearf(vx(0), 15.0f) && nearf(vy(0), 20.0f) && nearf(vz(0), 0.0f));
      ok("v1 pos", nearf(vx(1), 15.0f + 64.0f) && nearf(vy(1), 20.0f));
      ok("v2 pos", nearf(vx(2), 15.0f + 64.0f) && nearf(vy(2), 20.0f + 32.0f));
      ok("v3 pos", nearf(vx(3), 15.0f) && nearf(vy(3), 20.0f + 32.0f));
      ok("v4==v0, v5==v2", nearf(vx(4), vx(0)) && nearf(vx(5), vx(2)));
      ok("all one colour", vcol(0)==0xFF102030u && vcol(1)==0xFF102030u && vcol(5)==0xFF102030u);
      ok("uv 0,1,1,0 / 0,0,1,1",
         nearf(vu(0),0) && nearf(vw(0),0) && nearf(vu(1),1) && nearf(vw(2),1) && nearf(vu(3),0) && nearf(vw(3),1));
      ok("texture released after Flush", MEM32(texture + 4) == 1u);

      /* End: state block applied, not ready; a second End fails */
      ok("End", hle_call_guest(mEnd, 1, (uint32_t[]){ sprite }) == D3D_OK);
      ok("End applied state", g_apply == 1u);
      ok("second End fails", hle_call_guest(mEnd, 1, (uint32_t[]){ sprite }) == D3DERR_INVALIDCALL);

      /* DONOTSAVESTATE: no new capture */
      g_capture = 0;
      ok("Begin(DONOTSAVESTATE)", hle_call_guest(mBegin, 2, (uint32_t[]){ sprite, 1u }) == D3D_OK);
      ok("no capture with DONOTSAVESTATE", g_capture == 0u);
      ok("End (DONOTSAVESTATE)", hle_call_guest(mEnd, 1, (uint32_t[]){ sprite }) == D3D_OK);

      /* partial rect, identity transform */
      float I[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
      memcpy(hle_ptr(matva), I, 64);
      hle_call_guest(mST, 2, (uint32_t[]){ sprite, matva });
      ok("Begin for rect test", hle_call_guest(mBegin, 2, (uint32_t[]){ sprite, 0 }) == D3D_OK);
      int32_t rect[4] = { 10, 5, 42, 21 }; uint32_t rva = hle_va(hle_alloc(16)); memcpy(hle_ptr(rva), rect, 16);
      g_primcount = 0; memset(g_verts, 0, sizeof g_verts); g_draws = 0;
      ok("Draw with rect", hle_call_guest(mDraw, 6, (uint32_t[]){ sprite, texture, rva, cva, pva, 0xFFFFFFFFu }) == D3D_OK);
      ok("Flush rect", hle_call_guest(mFlush, 1, (uint32_t[]){ sprite }) == D3D_OK);
      ok("rect v0 pos", nearf(vx(0), 10.0f) && nearf(vy(0), 20.0f));
      ok("rect v1 x", nearf(vx(1), 10.0f + 32.0f));
      ok("rect v2 y", nearf(vy(2), 20.0f + 16.0f));
      ok("rect uv0", nearf(vu(0), 10.0f/64.0f) && nearf(vw(0), 5.0f/32.0f));
      ok("rect uv2", nearf(vu(2), 42.0f/64.0f) && nearf(vw(2), 21.0f/32.0f));
      ok("End rect", hle_call_guest(mEnd, 1, (uint32_t[]){ sprite }) == D3D_OK);

      /* Draw before Begin must fail */
      ok("Draw before Begin fails", hle_call_guest(mDraw, 6, (uint32_t[]){ sprite, texture, 0, cva, pva, 0 }) == D3DERR_INVALIDCALL);

      /* OnLostDevice releases the device objects (decl + state block) */
      uint32_t declr0 = g_decl_release, sbr0 = g_sb_release;
      ok("OnLostDevice", hle_call_guest(mLost, 1, (uint32_t[]){ sprite }) == D3D_OK);
      ok("lost device released decl+stateblock", g_decl_release > declr0 && g_sb_release > sbr0);
      ok("OnResetDevice", hle_call_guest(mReset, 1, (uint32_t[]){ sprite }) == D3D_OK);

      /* Release: refcount 1 -> 0 frees, releasing the device */
      uint32_t devref = MEM32(device + 4);
      ok("sprite Release", hle_call_guest(mRel, 1, (uint32_t[]){ sprite }) == 0u);
      ok("final release freed device ref", MEM32(device + 4) == devref - 1u);
    }

    if (!fails) printf("d3dx9_sprite_selftest: all checks passed\n");
    return fails != 0;
}