#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "win32hle.h"
void d3d_QueryInterface(void);
#include "d3dx9_line.h"

/* The ID3DXLine method order and signatures below are taken from the public
 * D3DX9 header d3dx9core.h (ID3DXLine declaration; the same declaration is in
 * Wine/ReactOS sdk/include/dxsdk/d3dx9core.h). Only the order and signatures
 * are reproduced here; nothing was taken from a Microsoft binary. The 20
 * methods are: IUnknown(0-2), GetDevice, Begin, Draw, DrawTransform,
 * Set/GetPattern, Set/GetPatternScale, Set/GetWidth, Set/GetAntialias,
 * Set/GetGLLines, End, OnLostDevice, OnResetDevice.
 *
 * Every COM method below has exactly one RET with the correct stdcall argc
 * including `this`. In particular AddRef is done by editing the refcount
 * directly, never by calling hle_com_AddRef(), whose own RET(...,1) would
 * pop another 8 bytes and corrupt the guest stack. */

/* D3DXLine draws 2D screen-space line strips via IDirect3DDevice9.
 * Vertex format for DrawPrimitiveUP: XYZRHW|DIFFUSE (20 bytes). */
#define D3DFVF_XYZRHW_DIFFUSE 0x00000044u
#define D3DPT_LINESTRIP 3u

typedef struct {
    uint32_t device;
    float    width;
    uint32_t pattern;
    float    pattern_scale;
    int32_t  antialias;
    int32_t  gllines;
    int      in_begin;
    /* saved state for Begin/End */
    uint32_t saved_fvf;
    uint32_t saved_tex0;
    uint32_t saved_alphablend;
    uint32_t saved_srcblend;
    uint32_t saved_destblend;
    uint32_t saved_zenable;
    uint32_t saved_lighting;
} line_state_t;
#define LS(o) ((line_state_t *)((char *)hle_ptr(o) + 8u))

static uint32_t g_vt_line;

typedef struct { float x, y, z, rhw; uint32_t color; } line_vert_t;

static uint32_t dev_get_u32(uint32_t dev, const char *name, int nargs, const uint32_t *args) {
    uint32_t fn = hle_resolve(name);
    if (!fn) return 0;
    return hle_call_guest(fn, nargs, args);
}
static void dev_set_render_state(uint32_t dev, uint32_t state, uint32_t value) {
    uint32_t a[3] = { dev, state, value };
    uint32_t fn = hle_resolve("IDirect3DDevice9::SetRenderState");
    if (fn) hle_call_guest(fn, 3, a);
}
static uint32_t dev_get_render_state(uint32_t dev, uint32_t state) {
    uint32_t out = 0;
    uint32_t *p = (uint32_t *)hle_alloc(4);
    if (!p) return 0;
    *p = 0;
    uint32_t a[3] = { dev, state, hle_va(p) };
    uint32_t fn = hle_resolve("IDirect3DDevice9::GetRenderState");
    if (fn) hle_call_guest(fn, 3, a);
    out = *p;
    hle_free(p);
    return out;
}
static void dev_set_fvf(uint32_t dev, uint32_t fvf) {
    uint32_t a[2] = { dev, fvf };
    uint32_t fn = hle_resolve("IDirect3DDevice9::SetFVF");
    if (fn) hle_call_guest(fn, 2, a);
}
static uint32_t dev_get_fvf(uint32_t dev) {
    uint32_t *p = (uint32_t *)hle_alloc(4);
    if (!p) return 0;
    *p = 0;
    uint32_t a[2] = { dev, hle_va(p) };
    uint32_t fn = hle_resolve("IDirect3DDevice9::GetFVF");
    if (fn) hle_call_guest(fn, 2, a);
    uint32_t v = *p;
    hle_free(p);
    return v;
}
static void dev_set_texture(uint32_t dev, uint32_t stage, uint32_t tex) {
    uint32_t a[3] = { dev, stage, tex };
    uint32_t fn = hle_resolve("IDirect3DDevice9::SetTexture");
    if (fn) hle_call_guest(fn, 3, a);
}
static uint32_t dev_get_texture(uint32_t dev, uint32_t stage) {
    uint32_t *p = (uint32_t *)hle_alloc(4);
    if (!p) return 0;
    *p = 0;
    uint32_t a[3] = { dev, stage, hle_va(p) };
    uint32_t fn = hle_resolve("IDirect3DDevice9::GetTexture");
    if (fn) hle_call_guest(fn, 3, a);
    uint32_t v = *p;
    hle_free(p);
    return v;
}

void l_QueryInterface(void) {
    uint32_t self = A32(0), ppv = A32(2);
    uint32_t hr = ppv ? D3D_OK : D3DERR_INVALIDCALL;
    if (ppv) {
        MEM32(ppv) = self;
        MEM32(self + 4)++;          /* AddRef without a nested RET */
    }
    RET(hr, 3);                     /* (this, riid, ppv) - one purge only */
}
void l_AddRef(void) { uint32_t self=A32(0); MEM32(self+4)++; RET(MEM32(self+4),1); }
void l_Release(void) { uint32_t self=A32(0); uint32_t n=--MEM32(self+4); if(!n) hle_com_free(self); RET(n,1); }

void l_GetDevice(void) {
    /* GetDevice(this, ppDevice) - (this, out) */
    uint32_t self = A32(0), pp = A32(1);
    line_state_t *ls = LS(self);
    uint32_t hr = pp ? D3D_OK : D3DERR_INVALIDCALL;
    if (pp) {
        MEM32(pp) = ls->device;
        if (ls->device) {
            uint32_t a[1] = { ls->device };
            uint32_t fn = hle_resolve("IDirect3DDevice9::AddRef");
            if (fn) hle_call_guest(fn, 1, a);   /* hle_call_guest restores esp */
            else MEM32(ls->device + 4)++;
        }
    }
    RET(hr, 2);
}

static void begin_state(line_state_t *ls) {
    uint32_t dev = ls->device;
    if (!dev) return;
    ls->saved_fvf = dev_get_fvf(dev);
    ls->saved_tex0 = dev_get_texture(dev, 0);
    ls->saved_alphablend = dev_get_render_state(dev, 27);
    ls->saved_srcblend = dev_get_render_state(dev, 19);
    ls->saved_destblend = dev_get_render_state(dev, 20);
    ls->saved_zenable = dev_get_render_state(dev, 7);
    ls->saved_lighting = dev_get_render_state(dev, 137);
    dev_set_texture(dev, 0, 0);
    dev_set_fvf(dev, D3DFVF_XYZRHW_DIFFUSE);
    dev_set_render_state(dev, 137, 0);
    dev_set_render_state(dev, 7, 0);
    dev_set_render_state(dev, 27, 1);
    dev_set_render_state(dev, 19, 5);
    dev_set_render_state(dev, 20, 6);
}
static void end_state(line_state_t *ls) {
    uint32_t dev = ls->device;
    if (!dev) return;
    dev_set_texture(dev, 0, ls->saved_tex0);
    dev_set_fvf(dev, ls->saved_fvf);
    dev_set_render_state(dev, 137, ls->saved_lighting);
    dev_set_render_state(dev, 7, ls->saved_zenable);
    dev_set_render_state(dev, 27, ls->saved_alphablend);
    dev_set_render_state(dev, 19, ls->saved_srcblend);
    dev_set_render_state(dev, 20, ls->saved_destblend);
}
void l_Begin(void) {
    /* Begin(this) */
    uint32_t self=A32(0);
    line_state_t *ls = LS(self);
    begin_state(ls);
    ls->in_begin = 1;
    RET(D3D_OK, 1);
}

static int draw_strip(uint32_t dev, const D3DXVECTOR2 *pts, uint32_t count, uint32_t color) {
    if (count < 2 || !dev || !pts) return D3D_OK;
    size_t bytes = (size_t)count * sizeof(line_vert_t);
    line_vert_t *v = (line_vert_t *)hle_alloc((uint32_t)bytes);
    if (!v) return D3DERR_OUTOFVIDEOMEMORY;
    for (uint32_t i = 0; i < count; i++) {
        v[i].x = pts[i].x;
        v[i].y = pts[i].y;
        v[i].z = 0.0f;
        v[i].rhw = 1.0f;
        v[i].color = color;
    }
    uint32_t dpu = hle_resolve("IDirect3DDevice9::DrawPrimitiveUP");
    if (dpu) {
        uint32_t a[5] = { dev, D3DPT_LINESTRIP, count - 1u, hle_va(v), (uint32_t)sizeof(line_vert_t) };
        hle_call_guest(dpu, 5, a);
    }
    hle_free(v);
    return D3D_OK;
}


void l_Draw(void) {
    /* Draw(this, pVertexList, dwVertexListCount, Color) - 4 args */
    uint32_t self = A32(0), pv = A32(1), count = A32(2), color = A32(3);
    line_state_t *ls = LS(self);
    uint32_t hr = D3D_OK;
    if (count >= 2 && !pv) hr = D3DERR_INVALIDCALL;
    else if (count >= 2) {
        int auto_begin = !ls->in_begin;
        if (auto_begin) begin_state(ls);
        hr = (uint32_t)draw_strip(ls->device, (const D3DXVECTOR2 *)hle_ptr(pv), count, color);
        if (auto_begin) end_state(ls);
    }
    RET(hr, 4);
}

void l_DrawTransform(void) {
    /* DrawTransform(this, pVertexList, dwVertexListCount, pTransform, Color) - 5 args */
    uint32_t self = A32(0), pv = A32(1), count = A32(2), pm = A32(3), color = A32(4);
    line_state_t *ls = LS(self);
    uint32_t hr = D3D_OK;
    if (count >= 2 && (!pv || !pm)) hr = D3DERR_INVALIDCALL;
    else if (count >= 2) {
        const D3DXVECTOR3 *src = (const D3DXVECTOR3 *)hle_ptr(pv);
        const D3DXMATRIX *m = (const D3DXMATRIX *)hle_ptr(pm);
        D3DXVECTOR2 *dst = (D3DXVECTOR2 *)hle_alloc((uint32_t)((size_t)count * sizeof(D3DXVECTOR2)));
        if (!dst) hr = D3DERR_OUTOFVIDEOMEMORY;
        else {
            for (uint32_t i = 0; i < count; i++) {
                float x = src[i].x, y = src[i].y, z = src[i].z;
                /* Row-vector D3D convention: [x y z 1] * M */
                float xp = x*m->_11 + y*m->_21 + z*m->_31 + m->_41;
                float yp = x*m->_12 + y*m->_22 + z*m->_32 + m->_42;
                float wp = x*m->_14 + y*m->_24 + z*m->_34 + m->_44;
                if (wp == 0.0f) wp = 1.0f;
                dst[i].x = xp / wp;
                dst[i].y = yp / wp;
            }
            int auto_begin = !ls->in_begin;
            if (auto_begin) begin_state(ls);
            hr = (uint32_t)draw_strip(ls->device, dst, count, color);
            if (auto_begin) end_state(ls);
            hle_free(dst);
        }
    }
    RET(hr, 5);
}

void l_End(void) {
    /* End(this) */
    uint32_t self=A32(0);
    line_state_t *ls = LS(self);
    if (ls->in_begin) end_state(ls);
    ls->in_begin = 0;
    RET(D3D_OK, 1);
}

void l_SetPattern(void) { LS(A32(0))->pattern = A32(1); RET(D3D_OK, 2); }
void l_GetPattern(void) { RET(LS(A32(0))->pattern, 1); }
void l_SetPatternScale(void) { uint32_t _v=A32(1); float _f; memcpy(&_f,&_v,4); LS(A32(0))->pattern_scale=_f; RET(D3D_OK,2); }
void l_GetPatternScale(void) { float f=LS(A32(0))->pattern_scale; uint32_t b; memcpy(&b,&f,4); RET(b,1); }
void l_SetWidth(void) { uint32_t _v=A32(1); float _f; memcpy(&_f,&_v,4); LS(A32(0))->width=_f; RET(D3D_OK,2); }
void l_GetWidth(void) { float f=LS(A32(0))->width; uint32_t b; memcpy(&b,&f,4); RET(b,1); }
void l_SetAntialias(void) { LS(A32(0))->antialias = (int32_t)A32(1); RET(D3D_OK,2); }
void l_GetAntialias(void) { RET((uint32_t)LS(A32(0))->antialias, 1); }
void l_SetGLLines(void) { LS(A32(0))->gllines = (int32_t)A32(1); RET(D3D_OK,2); }
void l_GetGLLines(void) { RET((uint32_t)LS(A32(0))->gllines, 1); }
void l_OnLostDevice(void) { RET(D3D_OK, 1); }
void l_OnResetDevice(void) { RET(D3D_OK, 1); }

static const win32hle_shim line_methods[] = {
    { "ID3DXLine::QueryInterface", d3d_QueryInterface },
    { "ID3DXLine::AddRef",         l_AddRef },
    { "ID3DXLine::Release",        l_Release },
    { "ID3DXLine::GetDevice",      l_GetDevice },
    { "ID3DXLine::Begin",          l_Begin },
    { "ID3DXLine::Draw",           l_Draw },
    { "ID3DXLine::DrawTransform",  l_DrawTransform },
    { "ID3DXLine::SetPattern",     l_SetPattern },
    { "ID3DXLine::GetPattern",     l_GetPattern },
    { "ID3DXLine::SetPatternScale", l_SetPatternScale },
    { "ID3DXLine::GetPatternScale", l_GetPatternScale },
    { "ID3DXLine::SetWidth",       l_SetWidth },
    { "ID3DXLine::GetWidth",       l_GetWidth },
    { "ID3DXLine::SetAntialias",   l_SetAntialias },
    { "ID3DXLine::GetAntialias",   l_GetAntialias },
    { "ID3DXLine::SetGLLines",     l_SetGLLines },
    { "ID3DXLine::GetGLLines",     l_GetGLLines },
    { "ID3DXLine::End",            l_End },
    { "ID3DXLine::OnLostDevice",   l_OnLostDevice },
    { "ID3DXLine::OnResetDevice",  l_OnResetDevice },
    { 0, 0 }
};

void s_D3DXCreateLine(void) {
    /* D3DXCreateLine(pDevice, ppLine) - 2 args, one RET */
    uint32_t dev = A32(0), pp = A32(1);
    uint32_t hr = pp ? D3D_OK : D3DERR_INVALIDCALL;
    if (pp) {
        if (!g_vt_line) g_vt_line = hle_com_vtable(line_methods);
        uint32_t obj = hle_com_new(g_vt_line, 8 + sizeof(line_state_t));
        if (!obj) hr = 0x8007000Eu;  /* E_OUTOFMEMORY */
        else {
            line_state_t *ls = LS(obj);
            memset(ls, 0, sizeof *ls);
            ls->device = dev;
            ls->width = 1.0f;
            ls->pattern = 0xFFFFFFFFu;
            ls->pattern_scale = 1.0f;
            MEM32(pp) = obj;
        }
    }
    RET(hr, 2);
}
