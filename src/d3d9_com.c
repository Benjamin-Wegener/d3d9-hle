/*
 * d3d9_com.c - what every D3D9 / D3DX9 COM object shares:
 *   - QueryInterface that answers only the IIDs the object really implements
 *     (IUnknown, its own interface and the interfaces it derives from),
 *     E_NOINTERFACE with a null out-pointer otherwise,
 *   - the IDirect3DResource9 slots SetPrivateData / GetPrivateData /
 *     FreePrivateData / SetPriority / GetPriority / PreLoad / GetType.
 *
 * Interface IDs: public Wine headers include/d3d9.h, d3dx9core.h, d3dx9mesh.h,
 * d3dx9anim.h, d3dx9effect.h, d3dx9shader.h, d3dx9math.h (read as a reference
 * for the published GUID values only).
 */
#include <stdio.h>
#include <string.h>
#include "d3d9_internal.h"

#define E_NOINTERFACE_ 0x80004002u
#define D3DSPD_IUNKNOWN_ 1u

typedef struct { const char *prefix; const char *iids[5]; } qi_class_t;
#define IID_UNKNOWN_TXT "00000000-0000-0000-c000-000000000046"
#define IID_RES9 "05eec05d-8f7d-4362-b999-d1baf357c704"
#define IID_BTEX9 "580ca87e-1d3c-4d54-991d-b7d3e3c298ce"
#define IID_BASEFX "017c18ac-103f-4417-8c51-6bf6ef1e56be"
#define IID_ANSET "698cfb3f-9289-4d95-9a57-33a94b5a65f9"
static const qi_class_t QI_CLASSES[] = {
    { "IDirect3D9", { "81bdcbca-64d4-426d-ae8d-ad0147f4275c" } },
    { "IDirect3DDevice9", { "d0223b96-bf7a-43fd-92bd-a43b0d82b9eb" } },
    { "IDirect3DSurface9", { IID_RES9, "0cfbaf3a-9ff6-429a-99b3-a2796af8b89b" } },
    { "IDirect3DVolume9", { "24f416e6-1f67-4aa7-b88e-d33f6f3128a1" } },
    { "IDirect3DVertexBuffer9", { IID_RES9, "b64bb1b5-fd70-4df6-bf91-19d0a12455e3" } },
    { "IDirect3DIndexBuffer9", { IID_RES9, "7c9dd65e-d3f7-4529-acee-785830acde35" } },
    { "IDirect3DTexture9", { IID_RES9, IID_BTEX9, "85c31227-3de5-4f00-9b3a-f11ac38c18b5" } },
    { "IDirect3DCubeTexture9", { IID_RES9, IID_BTEX9, "fff32f81-d953-473a-9223-93d652aba93f" } },
    { "IDirect3DVolumeTexture9", { IID_RES9, IID_BTEX9, "2518526c-e789-4111-a7b9-47ef328d13e6" } },
    { "IDirect3DVertexDeclaration9", { "dd13c59c-36fa-4098-a8fb-c7ed39dc8546" } },
    { "IDirect3DVertexShader9", { "efc5557e-6265-4613-8a94-43857889eb36" } },
    { "IDirect3DPixelShader9", { "6d3bdbdc-5b02-4415-b852-ce5e8bccb289" } },
    { "IDirect3DStateBlock9", { "b07c4fe5-310d-4ba8-a23c-4f0f206f218b" } },
    { "IDirect3DQuery9", { "d9771460-a695-4f26-bbd3-27b840b541cc" } },
    { "IDirect3DSwapChain9", { "794950f2-adfc-458a-905e-10a10b0b503b" } },
    { "ID3DXBuffer", { "8ba5fb08-5195-40e2-ac58-0d989c3a0102" } },
    { "ID3DXMeshBuffer", { "8ba5fb08-5195-40e2-ac58-0d989c3a0102" } },
    { "dxb", { "8ba5fb08-5195-40e2-ac58-0d989c3a0102" } },
    { "ID3DXConstantTable", { "9dca3190-38b9-4fc3-92e3-39c6ddfb358b", "ab3c758f-093e-4356-b762-4db18f1b3a01" } },
    { "ID3DXMatrixStack", { "c7885ba7-f990-4fe7-922d-8515e477dd85" } },
    { "ID3DXSprite", { "ba0b762d-7d28-43ec-b9dc-2f84443b0614" } },
    { "ID3DXLine", { "d379ba7f-9042-4ac4-9f5e-58192a4c6bd8" } },
    { "ID3DXFont", { "d79dbb70-5f21-4d36-bbc2-ff525c213cdc" } },
    { "dxr", { "6985f346-2c3d-43b3-be8b-daae8a03d894" } },
    { "ID3DXMesh", { "7ed943dd-52e8-40b5-a8d8-76685c406330", "4020e5c2-1403-4929-883f-e2e849fac195" } },
    { "ID3DXKeyframedAnimationSet", { IID_ANSET, "fa4e8e3a-9786-407d-8b4c-5995893764af" } },
    { "ID3DXAnimationController", { "ac8948ec-f86d-43e2-96de-31fc35f96d9e" } },
    { "ID3DXEffectPool", { "9537ab04-3250-412e-8213-fcd2f8677933" } },
    { "ID3DXEffectCompiler", { IID_BASEFX, "51b8a949-1a31-47e6-bea0-4b30db53f1e0" } },
    { "ID3DXEffect", { IID_BASEFX, "d165ccb1-62b0-4a33-b3fa-a92300305a11" /* d3dx9effect.h, SDK <= 25 */,
                       "c7b17651-5420-490e-8a7f-923675a2d687" /* SDK 26 */, "f6ceb4b3-4e4c-40dd-b883-8d8de5ea0cd5" /* SDK >= 27 */ } },
};

static int guid_matches(const uint8_t *b, const char *txt) {
    unsigned f[16];
    unsigned d1, d2, d3, d4a, d4b, d4c[6];
    if (sscanf(txt, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &d1, &d2, &d3, &d4a, &d4b,
               &d4c[0], &d4c[1], &d4c[2], &d4c[3], &d4c[4], &d4c[5]) != 11) return 0;
    f[0] = d1 & 0xFF; f[1] = (d1 >> 8) & 0xFF; f[2] = (d1 >> 16) & 0xFF; f[3] = d1 >> 24;
    f[4] = d2 & 0xFF; f[5] = d2 >> 8; f[6] = d3 & 0xFF; f[7] = d3 >> 8;
    f[8] = d4a; f[9] = d4b;
    for (int i = 0; i < 6; i++) f[10 + i] = d4c[i];
    for (int i = 0; i < 16; i++) if (b[i] != (uint8_t)f[i]) return 0;
    return 1;
}

/* QueryInterface shared by every D3D9 / D3DX9 object. The interface the object
 * implements is read off its vtable (slot 0's registered name). */
void d3d_QueryInterface(void) {                         /* (this, riid, &out) */
    uint32_t self = A32(0), riid = A32(1), ppv = A32(2);
    if (!ppv) RET(D3DERR_INVALIDCALL, 3);
    MEM32(ppv) = 0;
    if (!riid) RET(D3DERR_INVALIDCALL, 3);
    const uint8_t *iid = (const uint8_t *)hle_ptr(riid);
    int yes = guid_matches(iid, IID_UNKNOWN_TXT);
    const char *n0 = hle_name(MEM32(MEM32(self)));
    if (!yes && n0) {
        const char *q = strstr(n0, "::");
        if (!q) q = strchr(n0, '_');
        size_t pl = q ? (size_t)(q - n0) : 0;
        for (size_t c = 0; !yes && c < sizeof(QI_CLASSES) / sizeof(QI_CLASSES[0]); c++) {
            if (strlen(QI_CLASSES[c].prefix) != pl || strncmp(n0, QI_CLASSES[c].prefix, pl)) continue;
            for (int k = 0; k < 5 && QI_CLASSES[c].iids[k]; k++)
                if (guid_matches(iid, QI_CLASSES[c].iids[k])) { yes = 1; break; }
        }
    }
    if (!yes) {
        fprintf(stderr, "[d3d9] %s: interface not supported\n", n0 ? n0 : "QueryInterface");
        RET(E_NOINTERFACE_, 3);
    }
    MEM32(ppv) = self;
    MEM32(self + 4)++;
    RET(D3D_OK, 3);
}

/* ---- private data and priority, kept host side per object ---- */
typedef struct { uint32_t obj; uint8_t guid[16]; uint32_t flags, size, data; } pd_t;
typedef struct { uint32_t obj, prio, lod, filter; } prio_t;
static pd_t *g_pd; static int g_npd, g_cap_pd;
static prio_t *g_prio; static int g_nprio, g_cap_prio;

static uint32_t guest_release(uint32_t unk) {
    uint32_t a[1] = { unk };
    return hle_call_guest(MEM32(MEM32(unk) + 8), 1, a);
}
static void pd_drop(int i) {
    pd_t *p = &g_pd[i];
    if (p->flags & D3DSPD_IUNKNOWN_) { uint32_t u = p->data; g_pd[i] = g_pd[--g_npd]; guest_release(u); return; }
    if (p->data) hle_free(hle_ptr(p->data));
    g_pd[i] = g_pd[--g_npd];
}
static int pd_find(uint32_t obj, const uint8_t *guid) {
    for (int i = 0; i < g_npd; i++) if (g_pd[i].obj == obj && !memcmp(g_pd[i].guid, guid, 16)) return i;
    return -1;
}

/* Frees an object of this module: its private data and priority go with it. */
void d3d9_free_obj(uint32_t obj) {
    for (int i = g_npd - 1; i >= 0; i--) if (g_pd[i].obj == obj) pd_drop(i);
    for (int i = 0; i < g_nprio; i++) if (g_prio[i].obj == obj) { g_prio[i] = g_prio[--g_nprio]; break; }
    hle_com_free(obj);
}

void d3d_res_SetPrivateData(void) {                     /* (this, refguid, pData, SizeOfData, Flags) */
    uint32_t self = A32(0), guid = A32(1), data = A32(2), size = A32(3), flags = A32(4);
    if (!guid || !data || (flags & ~D3DSPD_IUNKNOWN_)) RET(D3DERR_INVALIDCALL, 5);
    if ((flags & D3DSPD_IUNKNOWN_) && size != 4) RET(D3DERR_INVALIDCALL, 5);
    const uint8_t *g = (const uint8_t *)hle_ptr(guid);
    int old = pd_find(self, g);
    if (old >= 0) pd_drop(old);
    if (g_npd == g_cap_pd) {
        int nc = g_cap_pd ? g_cap_pd * 2 : 16;
        pd_t *n = (pd_t *)realloc(g_pd, (size_t)nc * sizeof(pd_t));
        if (!n) RET(D3DERR_OUTOFVIDEOMEMORY, 5);
        g_pd = n; g_cap_pd = nc;
    }
    pd_t *p = &g_pd[g_npd];
    p->obj = self; memcpy(p->guid, g, 16); p->flags = flags; p->size = size; p->data = 0;
    if (flags & D3DSPD_IUNKNOWN_) {
        uint32_t u = MEM32(data);
        if (!u) RET(D3DERR_INVALIDCALL, 5);
        MEM32(u + 4)++;                      /* the block holds a reference */
        p->data = u;
    } else {
        void *copy = hle_alloc(size ? size : 1);
        if (!copy) RET(D3DERR_OUTOFVIDEOMEMORY, 5);
        memcpy(copy, hle_ptr(data), size);
        p->data = hle_va(copy);
    }
    g_npd++;
    RET(D3D_OK, 5);
}

void d3d_res_GetPrivateData(void) {                     /* (this, refguid, pData, pSizeOfData) */
    uint32_t self = A32(0), guid = A32(1), data = A32(2), psize = A32(3);
    if (!guid || !psize) RET(D3DERR_INVALIDCALL, 4);
    int i = pd_find(self, (const uint8_t *)hle_ptr(guid));
    if (i < 0) RET(D3DERR_NOTFOUND, 4);
    pd_t *p = &g_pd[i];
    uint32_t have = MEM32(psize);
    if (have < p->size || !data) { MEM32(psize) = p->size; RET(D3DERR_MOREDATA, 4); }
    if (p->flags & D3DSPD_IUNKNOWN_) { MEM32(data) = p->data; MEM32(p->data + 4)++; }
    else memcpy(hle_ptr(data), hle_ptr(p->data), p->size);
    MEM32(psize) = p->size;
    RET(D3D_OK, 4);
}

void d3d_res_FreePrivateData(void) {                    /* (this, refguid) */
    uint32_t self = A32(0), guid = A32(1);
    if (!guid) RET(D3DERR_INVALIDCALL, 2);
    int i = pd_find(self, (const uint8_t *)hle_ptr(guid));
    if (i < 0) RET(D3DERR_NOTFOUND, 2);
    pd_drop(i);
    RET(D3D_OK, 2);
}

/* per-object record (priority, LOD, auto-gen filter), made on first write */
static prio_t *extra(uint32_t obj, int create) {
    for (int i = 0; i < g_nprio; i++) if (g_prio[i].obj == obj) return &g_prio[i];
    if (!create) return NULL;
    if (g_nprio == g_cap_prio) {
        int nc = g_cap_prio ? g_cap_prio * 2 : 16;
        prio_t *n = (prio_t *)realloc(g_prio, (size_t)nc * sizeof(prio_t));
        if (!n) return NULL;
        g_prio = n; g_cap_prio = nc;
    }
    prio_t *p = &g_prio[g_nprio++];
    p->obj = obj; p->prio = 0; p->lod = 0; p->filter = 2u /* D3DTEXF_LINEAR */;
    return p;
}
void d3d_res_SetPriority(void) {                        /* (this, PriorityNew) -> previous */
    prio_t *p = extra(A32(0), 1);
    if (!p) RET(0, 2);
    uint32_t o = p->prio; p->prio = A32(1);
    RET(o, 2);
}
void d3d_res_GetPriority(void) {                        /* (this) */
    prio_t *p = extra(A32(0), 0);
    RET(p ? p->prio : 0, 1);
}

/* Texture LOD and auto-generated mip filter (IDirect3DBaseTexture9). LOD only
 * exists for managed textures (others: 0 and no effect). */
uint32_t d3d_tex_SetLOD(uint32_t self, uint32_t lod, uint32_t levels, uint32_t pool) {
    if (pool != 1u /* D3DPOOL_MANAGED */) return 0;
    prio_t *p = extra(self, 1);
    if (!p) return 0;
    uint32_t o = p->lod;
    p->lod = (levels && lod > levels - 1) ? levels - 1 : lod;
    return o;
}
uint32_t d3d_tex_GetLOD(uint32_t self, uint32_t pool) {
    prio_t *p = pool == 1u ? extra(self, 0) : NULL;
    return p ? p->lod : 0;
}
uint32_t d3d_tex_SetFilter(uint32_t self, uint32_t type, uint32_t usage) {
    if (!(usage & 0x400u /* D3DUSAGE_AUTOGENMIPMAP */)) return D3DERR_INVALIDCALL;
    if (type < 1u || type > 7u || type == 3u) return D3DERR_INVALIDCALL;   /* D3DTEXF_NONE and non-filters */
    prio_t *p = extra(self, 1);
    if (!p) return D3DERR_OUTOFVIDEOMEMORY;
    p->filter = type;
    return D3D_OK;
}
uint32_t d3d_tex_GetFilter(uint32_t self) {
    prio_t *p = extra(self, 0);
    return p ? p->filter : 2u;
}
void d3d_tex_GenerateMips(uint32_t usage, uint32_t levels) {
    if ((usage & 0x400u) && levels > 1u)
        fprintf(stderr, "[d3d9] IDirect3DBaseTexture9::GenerateMipSubLevels: mip generation not implemented\n");
}
void d3d_res_PreLoad(void) { RET(0, 1); }               /* (this): void; nothing to preload on the host */

#define RES_TYPE(fn, t) void fn(void) { RET(t, 1); }
RES_TYPE(d3d_surface_GetType, D3DRTYPE_SURFACE)
RES_TYPE(d3d_texture_GetType, D3DRTYPE_TEXTURE)
RES_TYPE(d3d_volumetex_GetType, D3DRTYPE_VOLUMETEXTURE)
RES_TYPE(d3d_cubetex_GetType, D3DRTYPE_CUBETEXTURE)
RES_TYPE(d3d_vb_GetType, D3DRTYPE_VERTEXBUFFER)
RES_TYPE(d3d_ib_GetType, D3DRTYPE_INDEXBUFFER)
