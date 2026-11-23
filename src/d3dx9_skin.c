/*
 * d3dx9_skin.c - D3DXCreateSkinInfo/D3DXCreateSkinInfoFVF and ID3DXSkinInfo.
 *
 * Public ABI (method order, signatures, slot count, structure offsets) comes
 * ONLY from public documentation / Wine's public headers and source:
 *   https://raw.githubusercontent.com/wine-mirror/wine/master/include/d3dx9mesh.h
 *   https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/d3dx9_36/skin.c
 *   https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/d3dx9_36/d3dx9_36.spec
 *   https://raw.githubusercontent.com/wine-mirror/wine/master/include/d3d9types.h
 *   https://learn.microsoft.com/en-us/windows/win32/direct3d9/id3dxskininfo--updateskinnedmesh
 * No Microsoft DLL or binary, no foreign code, no game-specific cases. Only
 * the order/signatures and documented semantics are reproduced.
 *
 * Skinning follows Wine's public algorithm: every POSITION/NORMAL element of
 * each destination vertex starts at zero and accumulates weight *
 * TransformCoord(src, bone_transforms[b]) (position) or weight *
 * TransformNormal(src, inv_transpose[b] ? inv_transpose[b] : bone_transforms[b])
 * (normal). Other elements are copied from source to destination. The caller
 * supplies the final bone matrices; the stored offset matrices are data
 * returned by GetBoneOffsetMatrix, not applied implicitly.
 *
 * Every vtable shim has exactly ONE RET with the correct stdcall argc (this
 * included). AddRef/Release edit the refcount directly; no helper that RETs is
 * called. Unsupported methods still occupy their slot, log and return an
 * honest E_NOTIMPL, never a false success.
 */
#include <stdio.h>
#include <string.h>
#include "win32hle.h"
#include "d3d9_hle.h"
#include "d3dx9_mesh.h"
#include "d3dx9_skin.h"

#define SKIN_E_NOTIMPL  0x80004001u
#define SKIN_E_OUTOFMEM 0x8007000Eu
#define SKIN_E_FAIL     0x80004005u
#define SKIN_NAME_MAX   64u
#define SKIN_MAT_BYTES  64u                 /* D3DXMATRIX: 4x4 floats */

/* A FLOAT result leaves on the x87 stack (ST0), as on real x86 stdcall. */
#define RETF(v, argc) do { fp_push_impl(g_st, &g_fp_top, (double)(v)); \
                           g_esp += 4u + 4u * (uint32_t)(argc); return; } while (0)

static void note(const char *what, const char *why) {
    fprintf(stderr, "[d3dx9_skin] %s: %s\n", what, why);
}
static float farg(int i) { uint32_t u = A32(i); float f; memcpy(&f, &u, 4); return f; }

/* ---- private state (host pointers live in the guest object's tail) ------- */
typedef struct {
    uint32_t *v;     /* vertex indices (host array) */
    float    *w;     /* weights (host array) */
    uint32_t  n;
} skin_infl_t;

typedef struct {
    uint32_t vertex_count, bone_count;
    uint32_t fvf;                            /* 0 for declaration-based objects */
    uint32_t ndecl;                          /* elements before D3DDECL_END */
    uint32_t stride;
    uint16_t pos_off, norm_off;
    uint8_t  pos_type, norm_type;            /* D3DDECLTYPE, 0xFF = absent */
    float    min_infl;
    D3DVERTEXELEMENT9 decl[D3DX9_SKIN_DECL_SIZE];

    skin_infl_t *infl;                       /* host array [bone_count] */
    uint32_t off_va;                         /* guest array [bone_count * 64] matrices */
    uint32_t names_va;                       /* guest array [bone_count * 64] chars */
} skin_t;
#define SKIN(o) ((skin_t *)((char *)hle_ptr(o) + 8u))

static uint32_t g_vt_si;
static uint32_t skin_vtable(void);

static uint32_t decl_type_size(uint8_t t) {
    /* D3DDECLTYPE_FLOAT1..4, D3DCOLOR, UBYTE4, SHORT2, SHORT4, UBYTE4N,
     * SHORT2N, SHORT4N, USHORT2N, USHORT4N, UDEC3, DEC3N, FLOAT16_2, FLOAT16_4 */
    static const uint8_t sz[17] = { 4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 4, 4, 4, 8 };
    return t < 17 ? sz[t] : 0;
}

/* Read a declaration from guest memory, element by element, stopping at the
 * end marker. Returns the number of real elements, or 0 on a bad declaration. */
static uint32_t read_decl(uint32_t va, D3DVERTEXELEMENT9 *d) {
    uint32_t n;
    if (!va) return 0;
    memset(d, 0, D3DX9_SKIN_DECL_SIZE * sizeof(*d));
    for (n = 0; n < D3DX9_SKIN_DECL_SIZE - 1u; n++) {
        memcpy(&d[n], hle_ptr(va + 8u * n), sizeof(*d));
        if (d[n].Stream == 0xFF) return n;
    }
    return 0;                                /* no end marker within 64 elements */
}

/* Derive stride and the float3 POSITION/NORMAL offsets from the stored decl. */
static void layout_from_decl(skin_t *s) {
    s->ndecl = 0;
    s->stride = 0;
    s->pos_off = 0; s->norm_off = 0;
    s->pos_type = 0xFF; s->norm_type = 0xFF;
    while (s->ndecl < D3DX9_SKIN_DECL_SIZE - 1u && s->decl[s->ndecl].Stream != 0xFF)
        s->ndecl++;
    for (uint32_t i = 0; i < s->ndecl; i++) {
        const D3DVERTEXELEMENT9 *e = &s->decl[i];
        uint32_t sz = decl_type_size(e->Type);
        if (e->Stream == 0 && e->Offset + sz > s->stride) s->stride = e->Offset + sz;
        if (e->Stream == 0 && e->Usage == D3DDECLUSAGE_POSITION && e->UsageIndex == 0) {
            s->pos_off = e->Offset; s->pos_type = e->Type;
        }
        if (e->Stream == 0 && e->Usage == D3DDECLUSAGE_NORMAL && e->UsageIndex == 0) {
            s->norm_off = e->Offset; s->norm_type = e->Type;
        }
    }
}

static void identity_matrix(uint32_t va) {
    uint32_t i;
    memset(hle_ptr(va), 0, SKIN_MAT_BYTES);
    for (i = 0; i < 4; i++) MEMF(va + i * 20u) = 1.0f;
}

static void skin_free_all(uint32_t obj) {
    skin_t *s = SKIN(obj);
    if (s->infl) {
        for (uint32_t i = 0; i < s->bone_count; i++) {
            if (s->infl[i].v) hle_free(s->infl[i].v);
            if (s->infl[i].w) hle_free(s->infl[i].w);
        }
        hle_free(s->infl);
    }
    if (s->off_va) hle_free(hle_ptr(s->off_va));
    if (s->names_va) hle_free(hle_ptr(s->names_va));
    hle_com_free(obj);
}

/* Build a fresh object from a validated declaration. On success *obj is the
 * guest address of the object. */
static uint32_t skin_new(uint32_t vertex_count, uint32_t bone_count,
                         const D3DVERTEXELEMENT9 *decl, uint32_t ndecl,
                         uint32_t fvf, uint32_t *obj) {
    skin_t probe;
    uint32_t o;
    if (!vertex_count || !bone_count || !decl || !ndecl ||
        ndecl >= D3DX9_SKIN_DECL_SIZE) return D3DERR_INVALIDCALL;
    memset(&probe, 0, sizeof probe);
    memcpy(probe.decl, decl, (ndecl + 1u) * sizeof(*decl));
    layout_from_decl(&probe);
    if (probe.pos_type != 2 || !probe.stride) {  /* D3DDECLTYPE_FLOAT3 */
        note("create", "declaration needs a stream-0 FLOAT3 POSITION element");
        return D3DERR_INVALIDCALL;
    }
    o = hle_com_new(skin_vtable(), 8u + sizeof(skin_t));
    if (!o) return SKIN_E_OUTOFMEM;
    {
        skin_t *s = SKIN(o);
        *s = probe;
        s->vertex_count = vertex_count;
        s->bone_count = bone_count;
        s->fvf = fvf;
        s->min_infl = 0.0f;
        s->infl = (skin_infl_t *)hle_alloc(bone_count * sizeof(skin_infl_t));
        s->off_va = hle_va(hle_alloc(bone_count * SKIN_MAT_BYTES));
        s->names_va = hle_va(hle_alloc(bone_count * SKIN_NAME_MAX));
        if (!s->infl || !s->off_va || !s->names_va) {
            skin_free_all(o);
            return SKIN_E_OUTOFMEM;
        }
        memset(s->infl, 0, bone_count * sizeof(skin_infl_t));
        for (uint32_t i = 0; i < bone_count; i++)
            identity_matrix(s->off_va + i * SKIN_MAT_BYTES);
        memset(hle_ptr(s->names_va), 0, bone_count * SKIN_NAME_MAX);
    }
    *obj = o;
    return D3D_OK;
}

/* =========================================================================
 * Create shims (4 stdcall args each)
 * ========================================================================= */
void s_D3DXCreateSkinInfo(void) {
    uint32_t nv = A32(0), declva = A32(1), nb = A32(2), out = A32(3);
    uint32_t hr = D3DERR_INVALIDCALL, obj = 0;
    D3DVERTEXELEMENT9 decl[D3DX9_SKIN_DECL_SIZE];
    if (out) {
        uint32_t n = read_decl(declva, decl);
        if (n) hr = skin_new(nv, nb, decl, n, 0, &obj);
        else note("D3DXCreateSkinInfo", "invalid declaration");
    } else note("D3DXCreateSkinInfo", "null out parameter");
    if (hr == D3D_OK) MEM32(out) = obj;
    RET(hr, 4);
}

void s_D3DXCreateSkinInfoFVF(void) {
    uint32_t nv = A32(0), fvf = A32(1), nb = A32(2), out = A32(3);
    uint32_t hr = D3DERR_INVALIDCALL, obj = 0;
    D3DVERTEXELEMENT9 decl[D3DX9_SKIN_DECL_SIZE];
    if (out) {
        if (d3dx9_fvf_vertex_size(fvf)) {
            uint32_t n = d3dx9_decl_from_fvf(fvf, decl);
            if (n) hr = skin_new(nv, nb, decl, n, fvf, &obj);
            else note("D3DXCreateSkinInfoFVF", "FVF has no representable declaration");
        } else note("D3DXCreateSkinInfoFVF", "unsupported FVF");
    } else note("D3DXCreateSkinInfoFVF", "null out parameter");
    if (hr == D3D_OK) MEM32(out) = obj;
    RET(hr, 4);
}

/* =========================================================================
 * ID3DXSkinInfo, slots 0..26
 * ========================================================================= */
void si_QueryInterface(void) {
    uint32_t self = A32(0), ppv = A32(2);
    uint32_t hr = ppv ? D3D_OK : D3DERR_INVALIDCALL;
    if (ppv) { MEM32(ppv) = self; MEM32(self + 4)++; }
    RET(hr, 3);
}
void si_AddRef(void) { uint32_t s = A32(0); uint32_t n = ++MEM32(s + 4); RET(n, 1); }
void si_Release(void) {
    uint32_t s = A32(0); uint32_t n = --MEM32(s + 4);
    if (!n) skin_free_all(s);
    RET(n, 1);
}

void si_SetBoneInfluence(void) {
    uint32_t self = A32(0), bone = A32(1), n = A32(2), verts = A32(3), weights = A32(4);
    skin_t *s = SKIN(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (bone < s->bone_count && verts && weights && n <= s->vertex_count) {
        uint32_t *nv = n ? (uint32_t *)hle_alloc(n * 4u) : 0;
        float *nw = n ? (float *)hle_alloc(n * 4u) : 0;
        if (n && (!nv || !nw)) {
            if (nv) hle_free(nv);
            if (nw) hle_free(nw);
            note("SetBoneInfluence", "out of memory");
            hr = SKIN_E_OUTOFMEM;
        } else {
            for (uint32_t i = 0; i < n; i++) {
                nv[i] = MEM32(verts + 4u * i);
                nw[i] = MEMF(weights + 4u * i);
            }
            if (s->infl[bone].v) hle_free(s->infl[bone].v);
            if (s->infl[bone].w) hle_free(s->infl[bone].w);
            s->infl[bone].v = nv; s->infl[bone].w = nw; s->infl[bone].n = n;
            hr = D3D_OK;
        }
    } else note("SetBoneInfluence", "invalid bone, vertex list or count");
    RET(hr, 5);
}

void si_SetBoneVertexInfluence(void) {
    uint32_t self = A32(0), bone = A32(1), idx = A32(2);
    skin_t *s = SKIN(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (bone < s->bone_count && idx < s->infl[bone].n) {
        s->infl[bone].w[idx] = farg(3);
        hr = D3D_OK;
    } else note("SetBoneVertexInfluence", "influence index out of range");
    RET(hr, 4);
}

void si_GetNumBoneInfluences(void) {
    uint32_t self = A32(0), bone = A32(1);
    skin_t *s = SKIN(self);
    RET(bone < s->bone_count ? s->infl[bone].n : 0u, 2);
}

void si_GetBoneInfluence(void) {
    uint32_t self = A32(0), bone = A32(1), verts = A32(2), weights = A32(3);
    skin_t *s = SKIN(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (bone < s->bone_count && verts && weights) {
        const skin_infl_t *in = &s->infl[bone];
        for (uint32_t i = 0; i < in->n; i++) {
            MEM32(verts + 4u * i) = in->v[i];
            MEMF(weights + 4u * i) = in->w[i];
        }
        hr = D3D_OK;
    }
    RET(hr, 4);
}

void si_GetBoneVertexInfluence(void) {
    uint32_t self = A32(0), bone = A32(1), idx = A32(2), pw = A32(3), pv = A32(4);
    skin_t *s = SKIN(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (bone < s->bone_count && idx < s->infl[bone].n && pw && pv) {
        MEMF(pw) = s->infl[bone].w[idx];
        MEM32(pv) = s->infl[bone].v[idx];
        hr = D3D_OK;
    }
    RET(hr, 5);
}

void si_GetMaxVertexInfluences(void) {
    uint32_t self = A32(0), out = A32(1);
    skin_t *s = SKIN(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (out) {
        uint32_t *cnt = (uint32_t *)hle_alloc(s->vertex_count * 4u);
        if (!cnt) {
            note("GetMaxVertexInfluences", "out of memory");
            hr = SKIN_E_OUTOFMEM;
        } else {
            uint32_t mx = 0;
            memset(cnt, 0, s->vertex_count * 4u);
            for (uint32_t b = 0; b < s->bone_count; b++)
                for (uint32_t i = 0; i < s->infl[b].n; i++)
                    if (s->infl[b].v[i] < s->vertex_count) cnt[s->infl[b].v[i]]++;
            for (uint32_t i = 0; i < s->vertex_count; i++) if (cnt[i] > mx) mx = cnt[i];
            MEM32(out) = mx;
            hle_free(cnt);
            hr = D3D_OK;
        }
    }
    RET(hr, 2);
}

void si_GetNumBones(void) { RET(SKIN(A32(0))->bone_count, 1); }

void si_FindBoneVertexInfluenceIndex(void) {
    uint32_t self = A32(0), bone = A32(1), vertex = A32(2), out = A32(3);
    skin_t *s = SKIN(self);
    uint32_t hr = SKIN_E_FAIL;
    if (bone < s->bone_count && out) {
        const skin_infl_t *in = &s->infl[bone];
        for (uint32_t i = 0; i < in->n; i++) {
            if (in->v[i] == vertex) { MEM32(out) = i; hr = D3D_OK; break; }
        }
    }
    RET(hr, 4);
}

void si_GetMaxFaceInfluences(void) {
    note("GetMaxFaceInfluences", "index-buffer face influence counting is not implemented");
    RET(SKIN_E_NOTIMPL, 4);
}

void si_SetMinBoneInfluence(void) {
    skin_t *s = SKIN(A32(0));
    s->min_infl = farg(1);
    RET(D3D_OK, 2);
}
void si_GetMinBoneInfluence(void) { RETF(SKIN(A32(0))->min_infl, 1); }

void si_SetBoneName(void) {
    uint32_t self = A32(0), bone = A32(1), name = A32(2);
    skin_t *s = SKIN(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (bone < s->bone_count && name) {
        char *dst = (char *)hle_ptr(s->names_va + bone * SKIN_NAME_MAX);
        strncpy(dst, (const char *)hle_ptr(name), SKIN_NAME_MAX - 1u);
        dst[SKIN_NAME_MAX - 1u] = 0;
        hr = D3D_OK;
    }
    RET(hr, 3);
}
void si_GetBoneName(void) {
    uint32_t self = A32(0), bone = A32(1);
    skin_t *s = SKIN(self);
    RET(bone < s->bone_count ? s->names_va + bone * SKIN_NAME_MAX : 0u, 2);
}

void si_SetBoneOffsetMatrix(void) {
    uint32_t self = A32(0), bone = A32(1), m = A32(2);
    skin_t *s = SKIN(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (bone < s->bone_count && m) {
        memcpy(hle_ptr(s->off_va + bone * SKIN_MAT_BYTES), hle_ptr(m), SKIN_MAT_BYTES);
        hr = D3D_OK;
    }
    RET(hr, 3);
}
void si_GetBoneOffsetMatrix(void) {
    uint32_t self = A32(0), bone = A32(1);
    skin_t *s = SKIN(self);
    RET(bone < s->bone_count ? s->off_va + bone * SKIN_MAT_BYTES : 0u, 2);
}

void si_Clone(void) {
    uint32_t self = A32(0), out = A32(1);
    const skin_t *src = SKIN(self);
    uint32_t hr = D3DERR_INVALIDCALL, obj = 0;
    if (out) {
        hr = skin_new(src->vertex_count, src->bone_count, src->decl, src->ndecl, src->fvf, &obj);
        if (hr == D3D_OK) {
            skin_t *dst = SKIN(obj);
            dst->min_infl = src->min_infl;
            memcpy(hle_ptr(dst->off_va), hle_ptr(src->off_va), src->bone_count * SKIN_MAT_BYTES);
            memcpy(hle_ptr(dst->names_va), hle_ptr(src->names_va), src->bone_count * SKIN_NAME_MAX);
            for (uint32_t b = 0; b < src->bone_count; b++) {
                uint32_t n = src->infl[b].n;
                if (!n) continue;
                dst->infl[b].v = (uint32_t *)hle_alloc(n * 4u);
                dst->infl[b].w = (float *)hle_alloc(n * 4u);
                if (!dst->infl[b].v || !dst->infl[b].w) {
                    skin_free_all(obj);
                    hr = SKIN_E_OUTOFMEM;
                    break;
                }
                memcpy(dst->infl[b].v, src->infl[b].v, n * 4u);
                memcpy(dst->infl[b].w, src->infl[b].w, n * 4u);
                dst->infl[b].n = n;
            }
            if (hr == D3D_OK) MEM32(out) = obj;
        }
    }
    RET(hr, 2);
}

void si_Remap(void) {
    note("Remap", "vertex remapping is not implemented");
    RET(SKIN_E_NOTIMPL, 3);
}

void si_SetFVF(void) {
    uint32_t self = A32(0), fvf = A32(1);
    skin_t *s = SKIN(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    D3DVERTEXELEMENT9 decl[D3DX9_SKIN_DECL_SIZE];
    if (d3dx9_fvf_vertex_size(fvf)) {
        uint32_t n = d3dx9_decl_from_fvf(fvf, decl);
        if (n) {
            skin_t tmp = *s;                 /* validate before committing */
            memcpy(tmp.decl, decl, (n + 1u) * sizeof(*decl));
            layout_from_decl(&tmp);
            if (tmp.pos_type == 2 && tmp.stride) {
                *s = tmp;
                s->fvf = fvf;
                hr = D3D_OK;
            } else note("SetFVF", "FVF has no FLOAT3 position");
        }
    }
    RET(hr, 2);
}
void si_SetDeclaration(void) {
    uint32_t self = A32(0), dv = A32(1);
    skin_t *s = SKIN(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    D3DVERTEXELEMENT9 decl[D3DX9_SKIN_DECL_SIZE];
    uint32_t n = read_decl(dv, decl);
    if (n) {
        skin_t tmp = *s;
        memcpy(tmp.decl, decl, (n + 1u) * sizeof(*decl));
        layout_from_decl(&tmp);
        if (tmp.pos_type == 2 && tmp.stride) {
            *s = tmp;
            s->fvf = 0;
            hr = D3D_OK;
        } else note("SetDeclaration", "declaration has no FLOAT3 position");
    }
    RET(hr, 2);
}
void si_GetFVF(void) { RET(SKIN(A32(0))->fvf, 1); }
void si_GetDeclaration(void) {
    uint32_t self = A32(0), out = A32(1);
    skin_t *s = SKIN(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (out) {
        memset(hle_ptr(out), 0, D3DX9_SKIN_DECL_SIZE * sizeof(D3DVERTEXELEMENT9));
        memcpy(hle_ptr(out), s->decl, (s->ndecl + 1u) * sizeof(D3DVERTEXELEMENT9));
        hr = D3D_OK;
    }
    RET(hr, 2);
}

void si_UpdateSkinnedMesh(void) {
    uint32_t self = A32(0), bt = A32(1), it = A32(2), src = A32(3), dst = A32(4);
    skin_t *s = SKIN(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (bt && src && dst && s->stride &&
        s->pos_type == 2 && (s->norm_type == 0xFF || s->norm_type == 2)) {
        /* Start from the source, zeroing POSITION/NORMAL elements. */
        for (uint32_t vi = 0; vi < s->vertex_count; vi++) {
            uint32_t sbase = src + vi * s->stride, dbase = dst + vi * s->stride;
            for (uint32_t e = 0; e < s->ndecl; e++) {
                const D3DVERTEXELEMENT9 *el = &s->decl[e];
                uint32_t sz = decl_type_size(el->Type);
                uint32_t soff = sbase + el->Offset, doff = dbase + el->Offset;
                int is_skin = el->Stream == 0 &&
                    (el->Usage == D3DDECLUSAGE_POSITION || el->Usage == D3DDECLUSAGE_NORMAL);
                for (uint32_t k = 0; k < sz; k++)
                    MEM8(doff + k) = is_skin ? 0u : MEM8(soff + k);
            }
        }
        /* Accumulate weight * transformed source for every influence. */
        for (uint32_t b = 0; b < s->bone_count; b++) {
            uint32_t mva = bt + b * SKIN_MAT_BYTES;
            uint32_t inva = it ? it + b * SKIN_MAT_BYTES : mva;
            const skin_infl_t *in = &s->infl[b];
            for (uint32_t j = 0; j < in->n; j++) {
                uint32_t vi = in->v[j];
                float w = in->w[j], sx, sy, sz, px, py, pz;
                uint32_t pout;
                if (vi >= s->vertex_count) continue;
                sx = MEMF(src + vi * s->stride + s->pos_off);
                sy = MEMF(src + vi * s->stride + s->pos_off + 4u);
                sz = MEMF(src + vi * s->stride + s->pos_off + 8u);
                /* row-vector * matrix: col j = x*M[0][j]+y*M[1][j]+z*M[2][j]+M[3][j] */
                px = sx * MEMF(mva)       + sy * MEMF(mva + 16u) + sz * MEMF(mva + 32u) + MEMF(mva + 48u);
                py = sx * MEMF(mva + 4u)  + sy * MEMF(mva + 20u) + sz * MEMF(mva + 36u) + MEMF(mva + 52u);
                pz = sx * MEMF(mva + 8u)  + sy * MEMF(mva + 24u) + sz * MEMF(mva + 40u) + MEMF(mva + 56u);
                pout = dst + vi * s->stride + s->pos_off;
                MEMF(pout)      += w * px;
                MEMF(pout + 4u) += w * py;
                MEMF(pout + 8u) += w * pz;
                if (s->norm_type == 2) {
                    float nx = MEMF(src + vi * s->stride + s->norm_off);
                    float ny = MEMF(src + vi * s->stride + s->norm_off + 4u);
                    float nz = MEMF(src + vi * s->stride + s->norm_off + 8u);
                    float qx = nx * MEMF(inva)       + ny * MEMF(inva + 16u) + nz * MEMF(inva + 32u);
                    float qy = nx * MEMF(inva + 4u)  + ny * MEMF(inva + 20u) + nz * MEMF(inva + 36u);
                    float qz = nx * MEMF(inva + 8u)  + ny * MEMF(inva + 24u) + nz * MEMF(inva + 40u);
                    uint32_t nout = dst + vi * s->stride + s->norm_off;
                    MEMF(nout)      += w * qx;
                    MEMF(nout + 4u) += w * qy;
                    MEMF(nout + 8u) += w * qz;
                }
            }
        }
        hr = D3D_OK;
    } else note("UpdateSkinnedMesh", "invalid buffers or unsupported vertex element type");
    RET(hr, 5);
}

void si_ConvertToBlendedMesh(void) {
    note("ConvertToBlendedMesh", "mesh blending conversion is not implemented");
    RET(SKIN_E_NOTIMPL, 11);
}
void si_ConvertToIndexedBlendedMesh(void) {
    note("ConvertToIndexedBlendedMesh", "indexed mesh blending conversion is not implemented");
    RET(SKIN_E_NOTIMPL, 12);
}

/* ---- vtable: 27 slots in exact public order, plus the NULL terminator ---- */
static const win32hle_shim si_methods[] = {
    { "ID3DXSkinInfo::QueryInterface", si_QueryInterface },
    { "ID3DXSkinInfo::AddRef", si_AddRef },
    { "ID3DXSkinInfo::Release", si_Release },
    { "ID3DXSkinInfo::SetBoneInfluence", si_SetBoneInfluence },
    { "ID3DXSkinInfo::SetBoneVertexInfluence", si_SetBoneVertexInfluence },
    { "ID3DXSkinInfo::GetNumBoneInfluences", si_GetNumBoneInfluences },
    { "ID3DXSkinInfo::GetBoneInfluence", si_GetBoneInfluence },
    { "ID3DXSkinInfo::GetBoneVertexInfluence", si_GetBoneVertexInfluence },
    { "ID3DXSkinInfo::GetMaxVertexInfluences", si_GetMaxVertexInfluences },
    { "ID3DXSkinInfo::GetNumBones", si_GetNumBones },
    { "ID3DXSkinInfo::FindBoneVertexInfluenceIndex", si_FindBoneVertexInfluenceIndex },
    { "ID3DXSkinInfo::GetMaxFaceInfluences", si_GetMaxFaceInfluences },
    { "ID3DXSkinInfo::SetMinBoneInfluence", si_SetMinBoneInfluence },
    { "ID3DXSkinInfo::GetMinBoneInfluence", si_GetMinBoneInfluence },
    { "ID3DXSkinInfo::SetBoneName", si_SetBoneName },
    { "ID3DXSkinInfo::GetBoneName", si_GetBoneName },
    { "ID3DXSkinInfo::SetBoneOffsetMatrix", si_SetBoneOffsetMatrix },
    { "ID3DXSkinInfo::GetBoneOffsetMatrix", si_GetBoneOffsetMatrix },
    { "ID3DXSkinInfo::Clone", si_Clone },
    { "ID3DXSkinInfo::Remap", si_Remap },
    { "ID3DXSkinInfo::SetFVF", si_SetFVF },
    { "ID3DXSkinInfo::SetDeclaration", si_SetDeclaration },
    { "ID3DXSkinInfo::GetFVF", si_GetFVF },
    { "ID3DXSkinInfo::GetDeclaration", si_GetDeclaration },
    { "ID3DXSkinInfo::UpdateSkinnedMesh", si_UpdateSkinnedMesh },
    { "ID3DXSkinInfo::ConvertToBlendedMesh", si_ConvertToBlendedMesh },
    { "ID3DXSkinInfo::ConvertToIndexedBlendedMesh", si_ConvertToIndexedBlendedMesh },
    { 0, 0 }
};

static uint32_t skin_vtable(void) {
    if (!g_vt_si) g_vt_si = hle_com_vtable(si_methods);
    return g_vt_si;
}
