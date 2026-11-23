/*
 * d3dx9_envmap.c - D3DXCreateRenderToEnvMap and ID3DXRenderToEnvMap.
 *
 * Public references (ABI/signatures/order and documented behaviour only;
 * no Microsoft DLL, no foreign code):
 *   https://raw.githubusercontent.com/wine-mirror/wine/master/include/d3dx9core.h
 *       D3DXRTE_DESC {Size, MipLevels, Format, DepthStencil,
 *       DepthStencilFormat}; ID3DXRenderToEnvMap declares exactly thirteen
 *       methods in this order: QueryInterface, AddRef, Release, GetDevice,
 *       GetDesc, BeginCube, BeginSphere, BeginHemisphere, BeginParabolic,
 *       Face, End, OnLostDevice, OnResetDevice. IID_ID3DXRenderToEnvMap is
 *       in the same header.
 *   https://learn.microsoft.com/en-us/windows/win32/direct3d9/id3dxrendertoenvmap-begincube
 *   https://learn.microsoft.com/en-us/windows/win32/direct3d9/id3dxrendertoenvmap-face
 *   https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/d3dx9_36/render.c
 *       BeginCube validates the cube texture, saves the device state and
 *       keeps the destination alive; Face switches the device's render
 *       target to one cube face; End closes the last face, optionally filters
 *       mip levels and restores the captured state. Sphere, hemisphere and
 *       parabolic maps are not implemented in Wine either; the slots still
 *       exist and report an honest error.
 * Only order, signatures and documented behaviour were taken from those
 * sources.
 *
 * Every COM shim below has exactly one executed RET. The stdcall argc
 * INCLUDES `this` because the guest calls `call [vt+4*i]` after pushing the
 * object pointer. Nested device/texture calls go through hle_call_guest(),
 * which restores esp itself, so they cannot unbalance the outer shim.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "win32hle.h"
#include "d3d9_hle.h"
#include "d3dx9_surface.h"          /* D3DX_FILTER_NONE */
#include "d3dx9_envmap.h"

#define ENVE_E_NOINTERFACE  0x80004002u
#define ENVE_E_NOTIMPL      0x80004001u
#define ENVE_E_OUTOFMEMORY  0x8007000Eu

/* GUID bytes in guest little-endian layout (Data1/Data2/Data3). */
static const uint8_t iid_unknown[16] = {
    0x00,0x00,0x00,0x00, 0x00,0x00, 0x00,0x00,
    0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46
};
static const uint8_t iid_envmap[16] = {
    0x4b,0x1b,0x3f,0x31, 0xb0,0xc7, 0xa2,0x4f,
    0x9d,0x9d,0x8d,0x38,0x0b,0x64,0x38,0x5e
};

typedef struct {
    uint32_t device;
    uint32_t size, mip_levels, format;
    uint32_t depth_stencil, depth_format;
    uint32_t cube;                 /* AddRef'd between BeginCube and End */
    uint32_t face_surface;         /* AddRef'd surface of the active face */
    int cube_active;               /* 0 = INITIAL, 1 = BeginCube seen */
    int active_face;               /* -1 = no face, else 0..5 */
    int scene_open;
    int use_temp;                  /* cube level is not itself a render target */
    uint32_t temp_rt, temp_depth;
    uint32_t sysmem;               /* GPU -> CPU staging for temp RT copy-back */
    uint32_t saved_rt, saved_depth;/* AddRef'd by the Get* calls */
    D3DVIEWPORT9 saved_vp;
    int state_saved;
} env_state_t;
#define ES(o) ((env_state_t *)((char *)hle_ptr(o) + 8u))

static uint32_t g_vt_envmap;

/* ---- guest helpers (no RET, no stack effect of their own) ---- */
static uint32_t vcall(const char *name, int n, const uint32_t *a) {
    uint32_t fn = hle_resolve(name);
    if (!fn) {
        fprintf(stderr, "[d3dx9] ID3DXRenderToEnvMap: missing shim %s\n", name);
        return ENVE_E_NOTIMPL;                 /* never a silent OK */
    }
    return hle_call_guest(fn, n, a);
}
static void addref(uint32_t obj, const char *name) {
    if (obj) vcall(name, 1, (uint32_t[]){ obj });
}
static void release(uint32_t obj, const char *name) {
    if (obj) vcall(name, 1, (uint32_t[]){ obj });
}
static uint32_t get_viewport(uint32_t dev, D3DVIEWPORT9 *vp) {
    uint32_t out = hle_va(hle_alloc(sizeof *vp));
    uint32_t hr;
    if (!out) return ENVE_E_OUTOFMEMORY;
    memset(vp, 0, sizeof *vp);
    hr = vcall("IDirect3DDevice9::GetViewport", 2, (uint32_t[]){ dev, out });
    if (hr == D3D_OK) memcpy(vp, hle_ptr(out), sizeof *vp);
    hle_free(hle_ptr(out));
    return hr;
}
static uint32_t surface_ptr(uint32_t dev, const char *name, int argc) {
    uint32_t out = hle_va(hle_alloc(4));
    uint32_t hr;
    if (!out) return 0;
    MEM32(out) = 0;
    hr = argc == 3
        ? vcall(name, 3, (uint32_t[]){ dev, 0u, out })
        : vcall(name, 2, (uint32_t[]){ dev, out });
    if (hr != D3D_OK) { hle_free(hle_ptr(out)); return 0; }
    hr = MEM32(out);
    hle_free(hle_ptr(out));
    return hr;
}

/* BeginCube changes render target 0, the depth surface and the viewport.
 * The Get* calls AddRef the surfaces; those refs belong to us until End. */
static uint32_t capture_state(env_state_t *es) {
    uint32_t hr = get_viewport(es->device, &es->saved_vp);
    if (hr != D3D_OK) return hr;
    es->saved_rt = surface_ptr(es->device, "IDirect3DDevice9::GetRenderTarget", 3);
    es->saved_depth = surface_ptr(es->device, "IDirect3DDevice9::GetDepthStencilSurface", 2);
    es->state_saved = 1;
    return D3D_OK;
}
static void restore_state(env_state_t *es) {
    if (!es->state_saved) return;
    if (es->saved_rt) {
        vcall("IDirect3DDevice9::SetRenderTarget", 3,
              (uint32_t[]){ es->device, 0u, es->saved_rt });
        release(es->saved_rt, "IDirect3DSurface9::Release");
        es->saved_rt = 0;
    } else {
        fprintf(stderr, "[d3dx9] ID3DXRenderToEnvMap: no saved render target\n");
    }
    vcall("IDirect3DDevice9::SetDepthStencilSurface", 2,
          (uint32_t[]){ es->device, es->saved_depth });
    release(es->saved_depth, "IDirect3DSurface9::Release");
    es->saved_depth = 0;
    {
        uint32_t p = hle_va(hle_alloc(sizeof es->saved_vp));
        if (p) {
            memcpy(hle_ptr(p), &es->saved_vp, sizeof es->saved_vp);
            vcall("IDirect3DDevice9::SetViewport", 2, (uint32_t[]){ es->device, p });
            hle_free(hle_ptr(p));
        }
    }
    es->state_saved = 0;
}
static void drop_temps(env_state_t *es) {
    release(es->temp_rt, "IDirect3DSurface9::Release");
    es->temp_rt = 0;
    release(es->temp_depth, "IDirect3DSurface9::Release");
    es->temp_depth = 0;
    release(es->sysmem, "IDirect3DSurface9::Release");
    es->sysmem = 0;
}

/* For a cube level created without D3DUSAGE_RENDERTARGET the rendering went
 * into our temporary render target. Copy its real GPU result back into the
 * destination face before switching to the next face / restoring state. */
static uint32_t copy_temp_to_face(env_state_t *es) {
    uint32_t hr = vcall("IDirect3DDevice9::GetRenderTargetData", 3,
                        (uint32_t[]){ es->device, es->temp_rt, es->sysmem });
    D3DLOCKED_RECT src, dst;
    uint32_t src_va, dst_va;
    if (hr != D3D_OK) {
        fprintf(stderr, "[d3dx9] ID3DXRenderToEnvMap: GetRenderTargetData failed (0x%08X)\n", hr);
        return hr;
    }
    src_va = hle_va(hle_alloc(sizeof src));
    dst_va = hle_va(hle_alloc(sizeof dst));
    if (!src_va || !dst_va) {
        if (src_va) hle_free(hle_ptr(src_va));
        if (dst_va) hle_free(hle_ptr(dst_va));
        return ENVE_E_OUTOFMEMORY;
    }
    memset(&src, 0, sizeof src);
    memset(&dst, 0, sizeof dst);
    hr = vcall("IDirect3DSurface9::LockRect", 4,
               (uint32_t[]){ es->sysmem, src_va, 0u, 0u });
    if (hr == D3D_OK)
        hr = vcall("IDirect3DSurface9::LockRect", 4,
                   (uint32_t[]){ es->face_surface, dst_va, 0u, 0u });
    if (hr == D3D_OK) {
        uint32_t bytes, row, pitch;
        memcpy(&src, hle_ptr(src_va), sizeof src);
        memcpy(&dst, hle_ptr(dst_va), sizeof dst);
        pitch = (uint32_t)(src.Pitch < dst.Pitch ? src.Pitch : dst.Pitch);
        for (row = 0; row < es->size && src.pBits && dst.pBits; row++) {
            bytes = pitch;
            if (!bytes) break;
            memcpy(hle_ptr(dst.pBits + row * (uint32_t)dst.Pitch),
                   hle_ptr(src.pBits + row * (uint32_t)src.Pitch), bytes);
        }
    } else {
        fprintf(stderr, "[d3dx9] ID3DXRenderToEnvMap: face copy LockRect failed (0x%08X)\n", hr);
    }
    vcall("IDirect3DSurface9::UnlockRect", 1, (uint32_t[]){ es->face_surface });
    vcall("IDirect3DSurface9::UnlockRect", 1, (uint32_t[]){ es->sysmem });
    hle_free(hle_ptr(src_va));
    hle_free(hle_ptr(dst_va));
    return hr;
}

/* Close the active face (EndScene + optional copy) and drop its surface ref. */
static uint32_t close_face(env_state_t *es) {
    uint32_t hr = D3D_OK;
    if (es->scene_open) {
        hr = vcall("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ es->device });
        if (hr == D3D_OK && es->use_temp) hr = copy_temp_to_face(es);
        es->scene_open = 0;
    }
    release(es->face_surface, "IDirect3DSurface9::Release");
    es->face_surface = 0;
    es->active_face = -1;
    return hr;
}

/* ---- ID3DXRenderToEnvMap: exact public header order, one executed RET ---- */
void dxe_QueryInterface(void) {
    uint32_t self = A32(0), iid = A32(1), out = A32(2);
    uint32_t hr = ENVE_E_NOINTERFACE;
    if (!out) {
        fprintf(stderr, "[d3dx9] ID3DXRenderToEnvMap::QueryInterface: NULL out\n");
        hr = D3DERR_INVALIDCALL;
    } else {
        MEM32(out) = 0;
        if (iid && (!memcmp(hle_ptr(iid), iid_unknown, 16) ||
                    !memcmp(hle_ptr(iid), iid_envmap, 16))) {
            MEM32(out) = self;
            MEM32(self + 4)++;                  /* AddRef without a nested RET */
            hr = 0u;                            /* S_OK */
        } else {
            fprintf(stderr, "[d3dx9] ID3DXRenderToEnvMap::QueryInterface: unsupported IID\n");
        }
    }
    RET(hr, 3);                                 /* this, riid, out */
}
void dxe_AddRef(void) {
    uint32_t self = A32(0);
    MEM32(self + 4)++;
    RET(MEM32(self + 4), 1);                    /* this */
}
void dxe_Release(void) {
    uint32_t self = A32(0), n;
    env_state_t *es = ES(self);
    n = --MEM32(self + 4);
    if (!n) {
        release(es->face_surface, "IDirect3DSurface9::Release");
        es->face_surface = 0;
        if (es->cube_active) {
            close_face(es);
            restore_state(es);
            es->cube_active = 0;
        }
        release(es->cube, "IDirect3DCubeTexture9::Release");
        es->cube = 0;
        drop_temps(es);
        release(es->device, "IDirect3DDevice9::Release");
        hle_com_free(self);
    }
    RET(n, 1);                                  /* this */
}
void dxe_GetDevice(void) {
    uint32_t self = A32(0), out = A32(1);
    uint32_t hr = D3D_OK;
    if (!out) {
        fprintf(stderr, "[d3dx9] ID3DXRenderToEnvMap::GetDevice: NULL out\n");
        hr = D3DERR_INVALIDCALL;
    } else {
        MEM32(out) = ES(self)->device;
        MEM32(ES(self)->device + 4)++;          /* AddRef, still one RET */
    }
    RET(hr, 2);                                 /* this, out */
}
void dxe_GetDesc(void) {
    uint32_t self = A32(0), out = A32(1);
    uint32_t hr = D3D_OK;
    if (!out) {
        fprintf(stderr, "[d3dx9] ID3DXRenderToEnvMap::GetDesc: NULL desc\n");
        hr = D3DERR_INVALIDCALL;
    } else {
        env_state_t *es = ES(self);
        uint32_t *d = (uint32_t *)hle_ptr(out);
        d[0] = es->size; d[1] = es->mip_levels; d[2] = es->format;
        d[3] = es->depth_stencil; d[4] = es->depth_format;
    }
    RET(hr, 2);                                 /* this, desc */
}
void dxe_BeginCube(void) {
    uint32_t self = A32(0), cube = A32(1);
    env_state_t *es = ES(self);
    D3DSURFACE_DESC sd;
    uint32_t out, hr = D3D_OK;
    if (!cube || es->cube_active) {
        fprintf(stderr, "[d3dx9] ID3DXRenderToEnvMap::BeginCube: NULL or busy\n");
        hr = D3DERR_INVALIDCALL;
        goto done;
    }
    memset(&sd, 0, sizeof sd);
    out = hle_va(hle_alloc(sizeof sd));
    if (!out) { hr = ENVE_E_OUTOFMEMORY; goto done; }
    memset(hle_ptr(out), 0, sizeof sd);
    hr = vcall("IDirect3DCubeTexture9::GetLevelDesc", 3,
               (uint32_t[]){ cube, 0u, out });
    if (hr == D3D_OK) memcpy(&sd, hle_ptr(out), sizeof sd);
    hle_free(hle_ptr(out));
    if (hr != D3D_OK) goto done;
    if (sd.Width != es->size || sd.Height != es->size || sd.Format != es->format) {
        fprintf(stderr, "[d3dx9] ID3DXRenderToEnvMap::BeginCube: cube level mismatch\n");
        hr = D3DERR_INVALIDCALL;
        goto done;
    }
    es->use_temp = !(sd.Usage & D3DUSAGE_RENDERTARGET);
    hr = capture_state(es);
    if (hr != D3D_OK) goto done;

    if (es->use_temp) {
        out = hle_va(hle_alloc(4));
        if (!out) { hr = ENVE_E_OUTOFMEMORY; goto fail; }
        MEM32(out) = 0;
        hr = vcall("IDirect3DDevice9::CreateRenderTarget", 9,
                   (uint32_t[]){ es->device, es->size, es->size, es->format,
                                 0u, 0u, 0u, out, 0u });
        es->temp_rt = hr == D3D_OK ? MEM32(out) : 0;
        hle_free(hle_ptr(out));
        if (hr != D3D_OK || !es->temp_rt) {
            if (hr == D3D_OK) hr = D3DERR_INVALIDCALL;
            goto fail;
        }
        out = hle_va(hle_alloc(4));
        if (!out) { hr = ENVE_E_OUTOFMEMORY; goto fail; }
        MEM32(out) = 0;
        hr = vcall("IDirect3DDevice9::CreateOffscreenPlainSurface", 7,
                   (uint32_t[]){ es->device, es->size, es->size, es->format,
                                 D3DPOOL_SYSTEMMEM, out, 0u });
        es->sysmem = hr == D3D_OK ? MEM32(out) : 0;
        hle_free(hle_ptr(out));
        if (hr != D3D_OK || !es->sysmem) {
            if (hr == D3D_OK) hr = D3DERR_INVALIDCALL;
            goto fail;
        }
    }
    if (es->depth_stencil) {
        out = hle_va(hle_alloc(4));
        if (!out) { hr = ENVE_E_OUTOFMEMORY; goto fail; }
        MEM32(out) = 0;
        hr = vcall("IDirect3DDevice9::CreateDepthStencilSurface", 9,
                   (uint32_t[]){ es->device, es->size, es->size, es->depth_format,
                                 0u, 0u, 1u, out, 0u });
        es->temp_depth = hr == D3D_OK ? MEM32(out) : 0;
        hle_free(hle_ptr(out));
        if (hr != D3D_OK || !es->temp_depth) {
            if (hr == D3D_OK) hr = D3DERR_INVALIDCALL;
            goto fail;
        }
    }
    addref(cube, "IDirect3DCubeTexture9::AddRef");
    es->cube = cube;
    es->cube_active = 1;
    es->active_face = -1;                       /* CUBE_BEGIN, no face yet */
    goto done;

fail:
    drop_temps(es);
    restore_state(es);
done:
    RET(hr, 2);                                 /* this, cubetex */
}
void dxe_BeginSphere(void) {
    uint32_t self = A32(0), tex = A32(1);
    (void)self; (void)tex;
    fprintf(stderr, "[d3dx9] ID3DXRenderToEnvMap::BeginSphere: sphere maps not supported\n");
    RET(ENVE_E_NOTIMPL, 2);                     /* this, tex */
}
void dxe_BeginHemisphere(void) {
    uint32_t self = A32(0), zpos = A32(1), zneg = A32(2);
    (void)self; (void)zpos; (void)zneg;
    fprintf(stderr, "[d3dx9] ID3DXRenderToEnvMap::BeginHemisphere: hemisphere maps not supported\n");
    RET(ENVE_E_NOTIMPL, 3);                     /* this, texZPos, texZNeg */
}
void dxe_BeginParabolic(void) {
    uint32_t self = A32(0), zpos = A32(1), zneg = A32(2);
    (void)self; (void)zpos; (void)zneg;
    fprintf(stderr, "[d3dx9] ID3DXRenderToEnvMap::BeginParabolic: parabolic maps not supported\n");
    RET(ENVE_E_NOTIMPL, 3);                     /* this, texZPos, texZNeg */
}
void dxe_Face(void) {
    uint32_t self = A32(0), face = A32(1), filter = A32(2);
    env_state_t *es = ES(self);
    uint32_t out, target, hr = D3D_OK;
    (void)filter;                               /* mipfilter is used by End */
    if (!es->cube_active || face >= 6u) {
        fprintf(stderr, "[d3dx9] ID3DXRenderToEnvMap::Face: no cube or bad face %u\n", face);
        hr = D3DERR_INVALIDCALL;
        goto done;
    }
    if (es->active_face >= 0) {
        hr = close_face(es);
        if (hr != D3D_OK) goto done;
    }
    out = hle_va(hle_alloc(4));
    if (!out) { hr = ENVE_E_OUTOFMEMORY; goto done; }
    MEM32(out) = 0;
    hr = vcall("IDirect3DCubeTexture9::GetCubeMapSurface", 4,
               (uint32_t[]){ es->cube, face, 0u, out });
    if (hr != D3D_OK || !MEM32(out)) {
        if (hr == D3D_OK) hr = D3DERR_INVALIDCALL;
        hle_free(hle_ptr(out));
        goto done;
    }
    es->face_surface = MEM32(out);
    hle_free(hle_ptr(out));
    target = es->use_temp ? es->temp_rt : es->face_surface;
    hr = vcall("IDirect3DDevice9::SetRenderTarget", 3,
               (uint32_t[]){ es->device, 0u, target });
    if (hr != D3D_OK) goto done;
    hr = vcall("IDirect3DDevice9::SetDepthStencilSurface", 2,
               (uint32_t[]){ es->device, es->temp_depth });
    if (hr != D3D_OK) goto done;
    hr = vcall("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ es->device });
    if (hr != D3D_OK) goto done;
    es->active_face = (int)face;
    es->scene_open = 1;
done:
    RET(hr, 3);                                 /* this, face, mipfilter */
}
void dxe_End(void) {
    uint32_t self = A32(0), filter = A32(1);
    env_state_t *es = ES(self);
    uint32_t hr = D3D_OK;
    if (!es->cube_active) {
        fprintf(stderr, "[d3dx9] ID3DXRenderToEnvMap::End: no active cube\n");
        hr = D3DERR_INVALIDCALL;
        goto done;
    }
    hr = close_face(es);
    if (hr == D3D_OK && filter != D3DX_FILTER_NONE && es->mip_levels > 1) {
        hr = vcall("D3DXFilterTexture", 4,
                   (uint32_t[]){ es->cube, 0u, 0u, filter });
        if (hr != D3D_OK)
            fprintf(stderr, "[d3dx9] ID3DXRenderToEnvMap::End: mip filtering failed (0x%08X)\n", hr);
    }
    restore_state(es);
    drop_temps(es);
    release(es->cube, "IDirect3DCubeTexture9::Release");
    es->cube = 0;
    es->cube_active = 0;
done:
    RET(hr, 2);                                 /* this, mipfilter */
}
void dxe_OnLostDevice(void) {
    /* No persistent device resources: all targets exist only per cube pass. */
    fprintf(stderr, "[d3dx9] ID3DXRenderToEnvMap::OnLostDevice: no persistent resources\n");
    RET(D3D_OK, 1);                             /* this */
}
void dxe_OnResetDevice(void) {
    fprintf(stderr, "[d3dx9] ID3DXRenderToEnvMap::OnResetDevice: no persistent resources\n");
    RET(D3D_OK, 1);                             /* this */
}

/* The thirteen entries mirror the public ID3DXRenderToEnvMap vtable order. */
static const win32hle_shim envmap_methods[] = {
    { "dxe_QueryInterface", dxe_QueryInterface },
    { "dxe_AddRef",         dxe_AddRef },
    { "dxe_Release",        dxe_Release },
    { "dxe_GetDevice",      dxe_GetDevice },
    { "dxe_GetDesc",        dxe_GetDesc },
    { "dxe_BeginCube",      dxe_BeginCube },
    { "dxe_BeginSphere",    dxe_BeginSphere },
    { "dxe_BeginHemisphere",dxe_BeginHemisphere },
    { "dxe_BeginParabolic", dxe_BeginParabolic },
    { "dxe_Face",           dxe_Face },
    { "dxe_End",            dxe_End },
    { "dxe_OnLostDevice",   dxe_OnLostDevice },
    { "dxe_OnResetDevice",  dxe_OnResetDevice },
    { 0, 0 }
};

void s_D3DXCreateRenderToEnvMap(void) {
    /* (device, size, mipLevels, format, DepthStencil, DepthStencilFormat, out) */
    uint32_t dev = A32(0), size = A32(1), mips = A32(2), fmt = A32(3);
    uint32_t stencil = A32(4), sfmt = A32(5), out = A32(6);
    uint32_t hr = D3D_OK;
    if (!dev || !out) {
        fprintf(stderr, "[d3dx9] D3DXCreateRenderToEnvMap: NULL device or out\n");
        RET(D3DERR_INVALIDCALL, 7);
    }
    MEM32(out) = 0;
    /* Public D3DX defaults, as used by Wine's D3DXCheckTextureRequirements. */
    if (!size) size = 1;
    if (!mips) {
        uint32_t n = 1, t = size;
        while (t > 1) { t >>= 1; n++; }
        mips = n;
    }
    if (!fmt) fmt = D3DFMT_A8R8G8B8;
    if (stencil && !sfmt) sfmt = D3DFMT_D24S8;

    if (!g_vt_envmap) g_vt_envmap = hle_com_vtable(envmap_methods);
    {
        uint32_t obj = hle_com_new(g_vt_envmap, 8u + sizeof(env_state_t));
        if (!obj) { RET(ENVE_E_OUTOFMEMORY, 7); }
        {
            env_state_t *es = ES(obj);
            memset(es, 0, sizeof *es);
            es->device = dev;
            es->size = size; es->mip_levels = mips; es->format = fmt;
            es->depth_stencil = stencil; es->depth_format = sfmt;
            es->active_face = -1;
            addref(dev, "IDirect3DDevice9::AddRef");
            MEM32(out) = obj;
        }
    }
    RET(hr, 7);                                 /* 7 stdcall args, no this */
}
