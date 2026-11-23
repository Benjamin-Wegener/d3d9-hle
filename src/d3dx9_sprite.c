/*
 * d3dx9_sprite.c - ID3DXSprite, the D3DX9 2D sprite helper (D3DXCreateSprite).
 *
 * The sprite is a guest-side COM object (hle_com_new): [0] vtable, [4] refcount,
 * [8..] the sprite state below. It draws through the IDirect3DDevice9 the guest
 * hands it, by calling that device's own vtable slots (hle_call_guest) exactly
 * as a lifted guest would - so it is device-independent and needs no private
 * access to d3d9.c. Every method is stdcall, `this` first, and purges its frame.
 *
 * The behaviour follows the documented ID3DXSprite contract (Begin flags, a
 * transform applied to each quad, Draw queuing a textured quad, Flush turning
 * the queue into DrawPrimitiveUP calls, End restoring the saved state). The
 * vertex layout matches D3DX's sprite vertex (XYZ, D3DCOLOR, UV). No code or
 * format is taken from a disassembled Microsoft DLL; the D3D constants are the
 * public DirectX 9 values.
 */
#include <stdint.h>
#include <string.h>
#include "win32hle.h"
void d3d_QueryInterface(void);
#include "d3dx9_math.h"

#define D3D_OK              0u
#define D3DERR_INVALIDCALL  0x8876086Cu
#define E_NOTIMPL           0x80004001u
#define E_OUTOFMEMORY       0x8007000Eu

/* ---- ID3DXSprite / D3DXSPRITE_* flags (public D3DX9 values) ---- */
#define D3DXSPRITE_DONOTSAVESTATE          0x00000001u
#define D3DXSPRITE_DONOTMODIFY_RENDERSTATE 0x00000002u
#define D3DXSPRITE_OBJECTSPACE             0x00000004u
#define D3DXSPRITE_BILLBOARD               0x00000008u
#define D3DXSPRITE_ALPHABLEND              0x00000010u
#define D3DXSPRITE_SORT_TEXTURE            0x00000020u
#define D3DXSPRITE_SORT_DEPTH_FRONTTOBACK  0x00000040u
#define D3DXSPRITE_SORT_DEPTH_BACKTOFRONT  0x00000080u
#define D3DXSPRITE_DO_NOT_ADDREF_TEXTURE   0x00000100u
#define D3DXSPRITE_FLAGLIMIT               0x000001FFu

/* ---- IDirect3DDevice9 vtable slots (in the order d3d9.c registers them) ---- */
enum {
    DEV_AddRef = 1, DEV_Release = 2, DEV_GetDeviceCaps = 7,
    DEV_SetTransform = 44, DEV_GetViewport = 48,
    DEV_SetRenderState = 57, DEV_CreateStateBlock = 59,
    DEV_BeginStateBlock = 60, DEV_EndStateBlock = 61,
    DEV_SetTexture = 65, DEV_SetTextureStageState = 67, DEV_SetSamplerState = 69,
    DEV_SetNPatchMode = 79, DEV_DrawPrimitiveUP = 83,
    DEV_CreateVertexDeclaration = 86, DEV_SetVertexDeclaration = 87,
    DEV_SetVertexShader = 92, DEV_SetStreamSource = 100, DEV_SetIndices = 104,
    DEV_SetPixelShader = 107
};
/* ---- IDirect3DTexture9 / IDirect3DStateBlock9 / declaration slots ---- */
enum { TEX_AddRef = 1, TEX_Release = 2, TEX_GetLevelDesc = 17 };
enum { SB_Release = 2, SB_Capture = 4, SB_Apply = 5 };
enum { DECL_Release = 2 };

/* ---- device call: read a COM object's vtable and invoke one of its slots ---- */
static uint32_t vcall(uint32_t obj, int slot, int nargs, const uint32_t *args) {
    uint32_t a[8];
    a[0] = obj;
    for (int i = 0; i < nargs; i++) a[1 + i] = args[i];
    return hle_call_guest(MEM32(MEM32(obj) + 4u * (uint32_t)slot), nargs + 1, a);
}

/* ---- render-state constants (public DirectX 9 values) ---- */
enum {
    R_ALPHAREF = 24, R_ALPHAFUNC = 25, R_ALPHABLENDENABLE = 27, R_FOGENABLE = 28,
    R_SPECULARENABLE = 29, R_RANGEFOGENABLE = 48, R_STENCILENABLE = 52,
    R_WRAP0 = 128, R_CLIPPING = 136, R_LIGHTING = 137,
    R_DIFFUSEMATERIALSOURCE = 145, R_VERTEXBLEND = 151, R_CLIPPLANEENABLE = 152,
    R_INDEXEDVERTEXBLENDENABLE = 167, R_COLORWRITEENABLE = 168, R_BLENDOP = 171,
    R_SRCBLEND = 19, R_DESTBLEND = 20, R_CULLMODE = 22,
    R_ALPHATESTENABLE = 15, R_FILLMODE = 8, R_SHADEMODE = 9
};
enum { TFX_DISABLE = 0 };
enum { TSS_COLOROP = 1, TSS_COLORARG1 = 2, TSS_COLORARG2 = 3, TSS_ALPHAOP = 4,
       TSS_ALPHAARG1 = 5, TSS_ALPHAARG2 = 6, TSS_TEXCOORDINDEX = 11,
       TSS_TEXTURETRANSFORMFLAGS = 24 };
enum { T_texture = 2, T_diffuse = 0, TOP_MODULATE = 4, TOP_DISABLE = 1, TTF_DISABLE = 0 };
enum { S_ADDRESSU = 1, S_ADDRESSV = 2, S_MAGFILTER = 5, S_MINFILTER = 6, S_MIPFILTER = 7,
       S_MIPMAPLODBIAS = 8, S_MAXMIPLEVEL = 9, S_MAXANISOTROPY = 10, S_SRGBTEXTURE = 11 };
enum { TADDR_CLAMP = 3, TEXF_POINT = 1, TEXF_LINEAR = 2 };
enum { CULL_NONE = 1, FILL_SOLID = 3, SHADE_GOURAUD = 2, CMP_GREATER = 5,
       BLENDOP_ADD = 1, BLEND_SRCALPHA = 5, BLEND_INVSRCALPHA = 6, MCS_COLOR1 = 1,
       PT_TRIANGLELIST = 4 };
#define COLORWRITE_ALL 0x0Fu
#define D3DMCS_COLOR1  1

/* ---- one queued sprite (the guest's Draw arguments, snapshotted) ---- */
typedef struct {
    uint32_t texture;
    uint32_t texw, texh;
    int32_t  left, top, right, bottom;       /* RECT */
    d3dx9_vec3 center, pos;
    uint32_t color;
    d3dx9_matrix transform;
} sentry;

#define MAX_SPRITES 512

typedef struct {
    uint32_t     device, vdecl, stateblock;
    d3dx9_matrix transform, view;
    uint32_t     flags;
    uint32_t     ready;
    uint32_t     count;                      /* queued sprites */
    sentry       sprites[MAX_SPRITES];
} sprite_t;

#define SPR(o) ((sprite_t *)((char *)hle_ptr(o) + 8))
#define VERTEX_BYTES 24                      /* XYZ (12) + D3DCOLOR (4) + UV (8) */

static uint32_t g_vt;

/* ----------------------------------------------------- device state helpers */

static void set_render_state(uint32_t dev, uint32_t state, uint32_t value) {
    uint32_t a[2] = { state, value };
    vcall(dev, DEV_SetRenderState, 2, a);
}
static void set_tss(uint32_t dev, uint32_t stage, uint32_t state, uint32_t value) {
    uint32_t a[3] = { stage, state, value };
    vcall(dev, DEV_SetTextureStageState, 3, a);
}
static void set_sampler(uint32_t dev, uint32_t samp, uint32_t state, uint32_t value) {
    uint32_t a[3] = { samp, state, value };
    vcall(dev, DEV_SetSamplerState, 3, a);
}

/* Ortho off-centre, left-handed, in the D3DX row-major layout d3dx9_math.c
 * uses (v' = v * M): the symmetric case is d3dx9_matrix_ortho_lh. */
static void ortho_offcenter_lh(d3dx9_matrix *m, float l, float r, float b, float t,
                               float zn, float zf) {
    d3dx9_matrix x;
    memset(&x, 0, sizeof x);
    x.m[0][0] = 2.0f / (r - l);
    x.m[1][1] = 2.0f / (t - b);
    x.m[2][2] = 1.0f / (zf - zn);
    x.m[3][0] = (l + r) / (l - r);
    x.m[3][1] = (t + b) / (b - t);
    x.m[3][2] = zn / (zn - zf);
    x.m[3][3] = 1.0f;
    *m = x;
}

/* The fixed-function state a sprite needs, matching D3DX's sprite setup. */
static void set_states(sprite_t *s) {
    uint32_t dev = s->device;
    uint32_t z = 0;                     /* a NULL / zero by-value argument */
    float fzero = 0.0f; uint32_t fbits; memcpy(&fbits, &fzero, 4);

    vcall(dev, DEV_SetVertexShader, 1, &z);   /* NULL shader */
    vcall(dev, DEV_SetPixelShader, 1, &z);    /* NULL shader */
    vcall(dev, DEV_SetNPatchMode, 1, &fbits);

    set_render_state(dev, R_ALPHABLENDENABLE, 1);
    set_render_state(dev, R_ALPHAFUNC, CMP_GREATER);
    set_render_state(dev, R_ALPHAREF, 0x00);
    set_render_state(dev, R_ALPHATESTENABLE, 0);
    set_render_state(dev, R_BLENDOP, BLENDOP_ADD);
    set_render_state(dev, R_CLIPPING, 1);
    set_render_state(dev, R_CLIPPLANEENABLE, 0);
    set_render_state(dev, R_COLORWRITEENABLE, COLORWRITE_ALL);
    set_render_state(dev, R_CULLMODE, CULL_NONE);
    set_render_state(dev, R_DESTBLEND, BLEND_INVSRCALPHA);
    set_render_state(dev, R_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
    set_render_state(dev, R_FILLMODE, FILL_SOLID);
    set_render_state(dev, R_FOGENABLE, 0);
    set_render_state(dev, R_INDEXEDVERTEXBLENDENABLE, 0);
    set_render_state(dev, R_LIGHTING, 0);
    set_render_state(dev, R_RANGEFOGENABLE, 0);
    set_render_state(dev, R_SHADEMODE, SHADE_GOURAUD);
    set_render_state(dev, R_SPECULARENABLE, 0);
    set_render_state(dev, R_SRCBLEND, BLEND_SRCALPHA);
    set_render_state(dev, R_STENCILENABLE, 0);
    set_render_state(dev, R_VERTEXBLEND, 0);
    set_render_state(dev, R_WRAP0, 0);

    set_tss(dev, 0, TSS_ALPHAARG1, T_texture);
    set_tss(dev, 0, TSS_ALPHAARG2, T_diffuse);
    set_tss(dev, 0, TSS_ALPHAOP, TOP_MODULATE);
    set_tss(dev, 0, TSS_COLORARG1, T_texture);
    set_tss(dev, 0, TSS_COLORARG2, T_diffuse);
    set_tss(dev, 0, TSS_COLOROP, TOP_MODULATE);
    set_tss(dev, 0, TSS_TEXCOORDINDEX, 0);
    set_tss(dev, 0, TSS_TEXTURETRANSFORMFLAGS, TTF_DISABLE);
    set_tss(dev, 1, TSS_ALPHAOP, TOP_DISABLE);
    set_tss(dev, 1, TSS_COLOROP, TOP_DISABLE);

    set_sampler(dev, 0, S_ADDRESSU, TADDR_CLAMP);
    set_sampler(dev, 0, S_ADDRESSV, TADDR_CLAMP);
    set_sampler(dev, 0, S_MAGFILTER, TEXF_LINEAR);
    set_sampler(dev, 0, S_MINFILTER, TEXF_LINEAR);
    set_sampler(dev, 0, S_MIPFILTER, TEXF_POINT);
    set_sampler(dev, 0, S_MIPMAPLODBIAS, 0);
    set_sampler(dev, 0, S_MAXMIPLEVEL, 0);
    set_sampler(dev, 0, S_MAXANISOTROPY, 1);
    set_sampler(dev, 0, S_SRGBTEXTURE, 0);

    d3dx9_matrix mat;
    uint32_t matva = hle_va(hle_alloc(64));   /* the matrix SetTransform reads */

    d3dx9_matrix_identity(&mat);
    memcpy(hle_ptr(matva), &mat, 64);
    { uint32_t a[2] = { 3 /* D3DTS_WORLD */, matva }; vcall(dev, DEV_SetTransform, 2, a); }
    { uint32_t a[2] = { 2 /* D3DTS_VIEW */, hle_va(&s->view) }; vcall(dev, DEV_SetTransform, 2, a); }

    /* Projection: an ortho matching the current viewport (screen-space y down). */
    uint32_t vp = hle_va(hle_alloc(24));
    vcall(dev, DEV_GetViewport, 1, &vp);
    float vx = (float)MEM32(vp + 0), vy = (float)MEM32(vp + 4);
    float vw = (float)MEM32(vp + 8), vh = (float)MEM32(vp + 12);
    float mnz, mxy; memcpy(&mnz, hle_ptr(vp + 16), 4); memcpy(&mxy, hle_ptr(vp + 20), 4);
    hle_free(hle_ptr(vp));
    ortho_offcenter_lh(&mat, vx + 0.5f, vw + vx + 0.5f, vh + vy + 0.5f, vy + 0.5f, mnz, mxy);
    memcpy(hle_ptr(matva), &mat, 64);
    { uint32_t a[2] = { 1 /* D3DTS_PROJECTION */, matva }; vcall(dev, DEV_SetTransform, 2, a); }
    hle_free(hle_ptr(matva));
}

/* Build the sprite vertex declaration (XYZ, D3DCOLOR, UV) once. */
static uint32_t make_decl(uint32_t dev) {
    /* D3DVERTEXELEMENT9: u16 Stream, u16 Offset, u8 Type, u8 Method, u8 Usage, u8 UsageIndex */
    static const uint8_t elems[4 * 8] = {
        0, 0, 0, 0, 2 /*FLOAT3*/, 0 /*METHOD*/, 0 /*POSITION*/, 0,
        0, 0, 12, 0, 4 /*D3DCOLOR*/, 0, 10 /*COLOR*/, 0,
        0, 0, 16, 0, 1 /*FLOAT2*/, 0, 5 /*TEXCOORD*/, 0,
        0, 0, 0xFF, 0, 1 /*UNUSED*/, 0, 0, 0
    };
    uint32_t buf = hle_va(hle_alloc(sizeof elems));
    memcpy(hle_ptr(buf), elems, sizeof elems);
    uint32_t outp = hle_va(hle_alloc(4));
    uint32_t a[2] = { buf, outp };
    vcall(dev, DEV_CreateVertexDeclaration, 2, a);
    uint32_t decl = MEM32(outp);
    hle_free(hle_ptr(buf));
    hle_free(hle_ptr(outp));
    return decl;
}

/* ------------------------------------------------------------ COM methods */

static void sp_GetDevice(void) {                    /* (this, &device) */
    sprite_t *s = SPR(A32(0));
    uint32_t out = A32(1);
    if (!out) RET(D3DERR_INVALIDCALL, 2);
    MEM32(out) = s->device;
    vcall(s->device, DEV_AddRef, 0, 0);
    RET(D3D_OK, 2);
}
static void sp_GetTransform(void) {                 /* (this, *transform) */
    sprite_t *s = SPR(A32(0));
    uint32_t out = A32(1);
    if (!out) RET(D3DERR_INVALIDCALL, 2);
    memcpy(hle_ptr(out), &s->transform, 64);
    RET(D3D_OK, 2);
}
static void sp_SetTransform(void) {                 /* (this, *transform) */
    sprite_t *s = SPR(A32(0));
    uint32_t p = A32(1);
    if (!p) RET(D3DERR_INVALIDCALL, 2);
    memcpy(&s->transform, hle_ptr(p), 64);
    RET(D3D_OK, 2);
}
static void sp_SetWorldViewRH(void) { RET(E_NOTIMPL, 3); }
static void sp_SetWorldViewLH(void) { RET(E_NOTIMPL, 3); }

static void sp_Begin(void) {                        /* (this, flags) */
    sprite_t *s = SPR(A32(0));
    uint32_t flags = A32(1);
    if (flags > D3DXSPRITE_FLAGLIMIT || s->ready) RET(D3DERR_INVALIDCALL, 2);

    if (!s->vdecl) s->vdecl = make_decl(s->device);

    /* Save the *pre-sprite* device state: a state block must be captured
     * before set_states() changes anything, otherwise End would restore the
     * sprite's own state instead of the caller's. */
    if (!(flags & D3DXSPRITE_DONOTSAVESTATE)) {
        uint32_t outp = hle_va(hle_alloc(4));
        MEM32(outp) = 0;
        if (!s->stateblock) {
            uint32_t a[2] = { 0 /* D3DSBT_ALL */, outp };
            vcall(s->device, DEV_CreateStateBlock, 2, a);
            s->stateblock = MEM32(outp);
        }
        if (s->stateblock) vcall(s->stateblock, SB_Capture, 0, 0);
        hle_free(hle_ptr(outp));
    }

    set_states(s);
    /* The sprite's own fixed-function setup (declaration, stream, indices) is
     * applied after the state block was captured so it is not saved either. */
    {
        uint32_t z = 0;
        { uint32_t a[1] = { s->vdecl }; vcall(s->device, DEV_SetVertexDeclaration, 1, a); }
        { uint32_t vs[4] = { 0, 0, 0, VERTEX_BYTES }; vcall(s->device, DEV_SetStreamSource, 4, vs); }
        vcall(s->device, DEV_SetIndices, 1, &z);                   /* NULL */
        { uint32_t a[2] = { 0, z }; vcall(s->device, DEV_SetTexture, 2, a); }
    }
    s->flags = flags;
    s->ready = 1;
    s->count = 0;
    RET(D3D_OK, 2);
}

static void sp_Draw(void) {                         /* (this, tex, *rect, *center, *pos, color) */
    sprite_t *s = SPR(A32(0));
    uint32_t tex = A32(1), rect = A32(2), center = A32(3), pos = A32(4);
    uint32_t color = A32(5);
    if (!tex || !s->ready) RET(D3DERR_INVALIDCALL, 6);
    if (s->count >= MAX_SPRITES) RET(E_OUTOFMEMORY, 6);

    sentry *e = &s->sprites[s->count];
    memset(e, 0, sizeof *e);
    e->texture = tex;
    if (!(s->flags & D3DXSPRITE_DO_NOT_ADDREF_TEXTURE))
        vcall(tex, TEX_AddRef, 0, 0);

    /* texture size, for the default rect and the UVs */
    uint32_t desc = hle_va(hle_alloc(32));
    { uint32_t a[2] = { 0, desc }; vcall(tex, TEX_GetLevelDesc, 2, a); }
    e->texw = MEM32(desc + 24); e->texh = MEM32(desc + 28);
    hle_free(hle_ptr(desc));

    if (rect) {
        const int32_t *r = (const int32_t *)hle_ptr(rect);
        e->left = r[0]; e->top = r[1]; e->right = r[2]; e->bottom = r[3];
    } else {
        e->left = 0; e->top = 0; e->right = (int32_t)e->texw; e->bottom = (int32_t)e->texh;
    }
    if (center) memcpy(&e->center, hle_ptr(center), 12);
    if (pos)    memcpy(&e->pos, hle_ptr(pos), 12);
    e->color = color;
    e->transform = s->transform;
    s->count++;
    RET(D3D_OK, 6);
}

static uint32_t flush_impl(sprite_t *s) {           /* shared by Flush and End */
    if (!s->ready) return D3DERR_INVALIDCALL;
    uint32_t n = s->count;
    if (!n) return D3D_OK;

    uint8_t *vb = (uint8_t *)hle_alloc(n * 6u * VERTEX_BYTES);
    uint32_t vbva = hle_va(vb);

    for (uint32_t i = 0; i < n; i++) {
        sentry *e = &s->sprites[i];
        float w = (float)(e->right - e->left);
        float h = (float)(e->bottom - e->top);
        float x0 = e->pos.x - e->center.x;
        float y0 = e->pos.y - e->center.y;
        float z0 = e->pos.z - e->center.z;
        float px[6] = { x0,       x0 + w,   x0 + w,   x0,       x0,       x0 + w };
        float py[6] = { y0,       y0,       y0 + h,   y0 + h,   y0,       y0 + h };
        float u[6], vv[6];
        float tw = e->texw ? (float)e->texw : 1.0f;
        float th = e->texh ? (float)e->texh : 1.0f;
        u[0] = (float)e->left  / tw; vv[0] = (float)e->top    / th;
        u[1] = (float)e->right / tw; vv[1] = (float)e->top    / th;
        u[2] = (float)e->right / tw; vv[2] = (float)e->bottom / th;
        u[3] = (float)e->left  / tw; vv[3] = (float)e->bottom / th;
        u[4] = u[0]; vv[4] = vv[0];
        u[5] = u[2]; vv[5] = vv[2];

        for (int k = 0; k < 6; k++) {
            d3dx9_vec3 p = { px[k], py[k], z0 }, o;
            d3dx9_vec3_transform_coord(&o, &p, &e->transform);
            uint8_t *q = vb + (i * 6 + k) * VERTEX_BYTES;
            memcpy(q + 0, &o.x, 4);
            memcpy(q + 4, &o.y, 4);
            memcpy(q + 8, &o.z, 4);
            memcpy(q + 12, &e->color, 4);
            memcpy(q + 16, &u[k], 4);
            memcpy(q + 20, &vv[k], 4);
        }
    }

    /* batch consecutive sprites that share a texture, as D3DX does */
    for (uint32_t start = 0; start < n; ) {
        uint32_t cnt = 1;
        while (start + cnt < n && s->sprites[start + cnt].texture == s->sprites[start].texture)
            cnt++;
        uint32_t data = vbva + start * 6u * VERTEX_BYTES;
        uint32_t a[4] = { PT_TRIANGLELIST, 2u * cnt, data, VERTEX_BYTES };
        vcall(s->device, DEV_SetTexture, 2, (uint32_t[]){ 0, s->sprites[start].texture });
        { uint32_t a2[1] = { s->vdecl }; vcall(s->device, DEV_SetVertexDeclaration, 1, a2); }
        vcall(s->device, DEV_DrawPrimitiveUP, 4, a);
        start += cnt;
    }

    hle_free(vb);

    if (!(s->flags & D3DXSPRITE_DO_NOT_ADDREF_TEXTURE))
        for (uint32_t i = 0; i < n; i++) vcall(s->sprites[i].texture, TEX_Release, 0, 0);

    s->count = 0;
    return D3D_OK;   /* Flush may be called repeatedly; ready stays set */
}

static void sp_Flush(void) {                        /* (this) */
    RET(flush_impl(SPR(A32(0))), 1);
}

static void sp_End(void) {                          /* (this) */
    sprite_t *s = SPR(A32(0));
    if (!s->ready) RET(D3DERR_INVALIDCALL, 1);
    flush_impl(s);
    if (s->stateblock && !(s->flags & D3DXSPRITE_DONOTSAVESTATE))
        vcall(s->stateblock, SB_Apply, 0, 0);
    s->ready = 0;
    RET(D3D_OK, 1);
}

static void release_queue_textures(sprite_t *s) {
    if (!(s->flags & D3DXSPRITE_DO_NOT_ADDREF_TEXTURE))
        for (uint32_t i = 0; i < s->count; i++)
            if (s->sprites[i].texture) vcall(s->sprites[i].texture, TEX_Release, 0, 0);
    s->count = 0;
}

static void sp_OnLostDevice(void) {                 /* (this) */
    sprite_t *s = SPR(A32(0));
    if (s->stateblock) { vcall(s->stateblock, SB_Release, 0, 0); s->stateblock = 0; }
    if (s->vdecl) { vcall(s->vdecl, DECL_Release, 0, 0); s->vdecl = 0; }
    release_queue_textures(s);
    s->flags = 0;
    s->ready = 0;
    RET(D3D_OK, 1);
}

static void sp_OnResetDevice(void) {                /* (this) */
    sprite_t *s = SPR(A32(0));
    release_queue_textures(s);
    s->flags = 0;
    s->ready = 0;
    RET(D3D_OK, 1);   /* transforms are kept */
}

static void sp_Release(void) {                      /* (this) */
    uint32_t self = A32(0);
    uint32_t n = --MEM32(self + 4);
    if (!n) {
        sprite_t *s = SPR(self);
        release_queue_textures(s);
        if (s->stateblock) vcall(s->stateblock, SB_Release, 0, 0);
        if (s->vdecl) vcall(s->vdecl, DECL_Release, 0, 0);
        if (s->device) vcall(s->device, DEV_Release, 0, 0);
        hle_com_free(self);
    }
    RET(n, 1);
}

static const win32hle_shim sprite_methods[] = {
    { "ID3DXSprite::QueryInterface", d3d_QueryInterface },
    { "ID3DXSprite::AddRef",         hle_com_AddRef },
    { "ID3DXSprite::Release",        sp_Release },
    { "ID3DXSprite::GetDevice",      sp_GetDevice },
    { "ID3DXSprite::GetTransform",   sp_GetTransform },
    { "ID3DXSprite::SetTransform",   sp_SetTransform },
    { "ID3DXSprite::SetWorldViewRH", sp_SetWorldViewRH },
    { "ID3DXSprite::SetWorldViewLH", sp_SetWorldViewLH },
    { "ID3DXSprite::Begin",          sp_Begin },
    { "ID3DXSprite::Draw",           sp_Draw },
    { "ID3DXSprite::Flush",          sp_Flush },
    { "ID3DXSprite::End",            sp_End },
    { "ID3DXSprite::OnLostDevice",   sp_OnLostDevice },
    { "ID3DXSprite::OnResetDevice",  sp_OnResetDevice },
    { 0, 0 }
};

/* D3DXCreateSprite(device, &sprite): 2 args. Registered in d3dx9.c's table. */
void s_D3DXCreateSprite(void) {
    uint32_t device = A32(0), out = A32(1);
    if (!device || !out) { if (out) MEM32(out) = 0; RET(D3DERR_INVALIDCALL, 2); }
    if (!g_vt) g_vt = hle_com_vtable(sprite_methods);
    uint32_t obj = hle_com_new(g_vt, 8 + sizeof(sprite_t));
    sprite_t *s = SPR(obj);
    memset(s, 0, sizeof *s);
    s->device = device;
    d3dx9_matrix_identity(&s->transform);
    d3dx9_matrix_identity(&s->view);
    vcall(device, DEV_AddRef, 0, 0);
    MEM32(out) = obj;
    RET(D3D_OK, 2);
}