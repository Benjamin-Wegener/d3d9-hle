/*
 * d3dx9_rts.c - D3DXCreateRenderToSurface and ID3DXRenderToSurface.
 *
 * Public references (ABI/semantics only; no Microsoft DLL or foreign code):
 *   https://raw.githubusercontent.com/wine-mirror/wine/master/include/d3dx9core.h
 *       D3DXRTS_DESC {Width, Height, Format, DepthStencil,
 *       DepthStencilFormat}; ID3DXRenderToSurface declares exactly nine
 *       methods in this order: QueryInterface, AddRef, Release, GetDevice,
 *       GetDesc, BeginScene, EndScene, OnLostDevice, OnResetDevice. The two
 *       IIDs are in the same header.
 *   https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/d3dx9_36/render.c
 *       D3DXCreateRenderToSurface validates device/out, starts at refcount 1,
 *       copies the description; BeginScene captures the device's render
 *       target, depth surface and viewport, binds the destination (creating
 *       temporary RT/depth surfaces when needed), and EndScene restores the
 *       captured state and releases the temporary resources.
 * Only the method order, signatures and documented behaviour were used.
 *
 * Every COM shim below has exactly one RET; the stdcall argc INCLUDES `this`
 * because the guest calls `call [vt+4*i]` after pushing the object pointer.
 * Nested device/surface calls go through hle_call_guest(), which restores
 * esp itself, so they cannot unbalance the outer shim's stack.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "win32hle.h"
void d3d_QueryInterface(void);
#include "d3d9_hle.h"
#include "d3dx9_rts.h"

#define RTS_E_NOINTERFACE 0x80004002u
#define RTS_E_OUTOFMEMORY 0x8007000Eu

/* GUID bytes as laid out in memory (little-endian Data1/Data2/Data3). */
static const uint8_t iid_unknown[16] = {
    0x00,0x00,0x00,0x00, 0x00,0x00, 0x00,0x00,
    0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46
};
static const uint8_t iid_rts[16] = {
    0x46,0xF3,0x85,0x69, 0x3D,0x2C, 0xB3,0x43,
    0xBE,0x8B,0xDA,0xAE,0x8A,0x03,0xD8,0x94
};

typedef struct {
    uint32_t device;
    uint32_t width, height, format;
    uint32_t depth_stencil, depth_format;
    uint32_t dst_surface;       /* AddRef'd while a scene is active */
    uint32_t temp_rt;           /* created only for non-rendertarget surfaces */
    uint32_t temp_depth;
    uint32_t saved_rt;          /* AddRef'd by GetRenderTarget */
    uint32_t saved_depth;       /* AddRef'd by GetDepthStencilSurface */
    D3DVIEWPORT9 saved_vp;
    int state_saved;
    int in_scene;
} rts_state_t;
#define RS(o) ((rts_state_t *)((char *)hle_ptr(o) + 8u))

static uint32_t g_vt_rts;

/* ---- guest device/surface helpers (no RET, no stack effects of their own) ---- */
static uint32_t vcall(const char *name, int n, const uint32_t *a) {
    uint32_t fn = hle_resolve(name);
    if (!fn) {
        fprintf(stderr, "[d3dx9] ID3DXRenderToSurface: missing shim %s\n", name);
        return 0x80004001u;                 /* E_NOTIMPL, never a false OK */
    }
    return hle_call_guest(fn, n, a);
}
static void addref(uint32_t obj, const char *name) {
    if (obj) vcall(name, 1, (uint32_t[]){ obj });
}
static void release(uint32_t obj, const char *name) {
    if (obj) vcall(name, 1, (uint32_t[]){ obj });
}
static uint32_t surface_desc(uint32_t surf, D3DSURFACE_DESC *desc) {
    uint32_t out = hle_va(hle_alloc(sizeof *desc));
    uint32_t hr;
    if (!out) return RTS_E_OUTOFMEMORY;
    memset(desc, 0, sizeof *desc);
    hr = vcall("IDirect3DSurface9::GetDesc", 2, (uint32_t[]){ surf, out });
    if (hr == D3D_OK) memcpy(desc, hle_ptr(out), sizeof *desc);
    hle_free(hle_ptr(out));
    return hr;
}
static uint32_t get_viewport(uint32_t dev, D3DVIEWPORT9 *vp) {
    uint32_t out = hle_va(hle_alloc(sizeof *vp));
    uint32_t hr;
    if (!out) return RTS_E_OUTOFMEMORY;
    memset(vp, 0, sizeof *vp);
    hr = vcall("IDirect3DDevice9::GetViewport", 2, (uint32_t[]){ dev, out });
    if (hr == D3D_OK) memcpy(vp, hle_ptr(out), sizeof *vp);
    hle_free(hle_ptr(out));
    return hr;
}
static uint32_t surface_ptr(uint32_t dev, const char *get_name, int argc) {
    uint32_t out = hle_va(hle_alloc(4));
    uint32_t hr;
    if (!out) return 0;
    MEM32(out) = 0;
    hr = argc == 3
        ? vcall(get_name, 3, (uint32_t[]){ dev, 0u, out })
        : vcall(get_name, 2, (uint32_t[]){ dev, out });
    if (hr != D3D_OK) { hle_free(hle_ptr(out)); return 0; }
    hr = MEM32(out);
    hle_free(hle_ptr(out));
    return hr;
}

/* Capture the three pieces of state that BeginScene changes. The Get*
 * methods AddRef the surfaces they hand back; those refs are ours until the
 * matching restore in EndScene. */
static uint32_t capture_state(rts_state_t *rs) {
    uint32_t hr = get_viewport(rs->device, &rs->saved_vp);
    if (hr != D3D_OK) return hr;
    rs->saved_rt = surface_ptr(rs->device, "IDirect3DDevice9::GetRenderTarget", 3);
    rs->saved_depth = surface_ptr(rs->device, "IDirect3DDevice9::GetDepthStencilSurface", 2);
    rs->state_saved = 1;
    return D3D_OK;
}
static void restore_state(rts_state_t *rs) {
    if (!rs->state_saved) return;
    if (rs->saved_rt) {
        vcall("IDirect3DDevice9::SetRenderTarget", 3,
              (uint32_t[]){ rs->device, 0u, rs->saved_rt });
        release(rs->saved_rt, "IDirect3DSurface9::Release");
        rs->saved_rt = 0;
    } else {
        fprintf(stderr, "[d3dx9] ID3DXRenderToSurface: no saved render target\n");
    }
    vcall("IDirect3DDevice9::SetDepthStencilSurface", 2,
          (uint32_t[]){ rs->device, rs->saved_depth });
    release(rs->saved_depth, "IDirect3DSurface9::Release");
    rs->saved_depth = 0;
    {
        uint32_t p = hle_va(hle_alloc(sizeof rs->saved_vp));
        if (p) {
            memcpy(hle_ptr(p), &rs->saved_vp, sizeof rs->saved_vp);
            vcall("IDirect3DDevice9::SetViewport", 2, (uint32_t[]){ rs->device, p });
            hle_free(hle_ptr(p));
        }
    }
    rs->state_saved = 0;
}
static void drop_temps(rts_state_t *rs) {
    release(rs->temp_rt, "IDirect3DSurface9::Release");
    rs->temp_rt = 0;
    release(rs->temp_depth, "IDirect3DSurface9::Release");
    rs->temp_depth = 0;
}

/* ---- ID3DXRenderToSurface: exact header order, one RET per shim ---- */
void dxr_QueryInterface(void) {
    uint32_t self = A32(0), iid = A32(1), out = A32(2);
    uint32_t hr = RTS_E_NOINTERFACE;
    if (!out) {
        fprintf(stderr, "[d3dx9] ID3DXRenderToSurface::QueryInterface: NULL out\n");
        hr = D3DERR_INVALIDCALL;
    } else {
        MEM32(out) = 0;
        if (iid && (!memcmp(hle_ptr(iid), iid_unknown, 16) ||
                    !memcmp(hle_ptr(iid), iid_rts, 16))) {
            MEM32(out) = self;
            MEM32(self + 4)++;              /* AddRef without a nested RET */
            hr = 0u;                        /* S_OK */
        } else {
            fprintf(stderr, "[d3dx9] ID3DXRenderToSurface::QueryInterface: unsupported IID\n");
        }
    }
    RET(hr, 3);                             /* this, riid, out */
}
void dxr_AddRef(void) { uint32_t self = A32(0); MEM32(self + 4)++; RET(MEM32(self + 4), 1); }
void dxr_Release(void) {
    uint32_t self = A32(0);
    rts_state_t *rs = RS(self);
    uint32_t n = --MEM32(self + 4);
    if (!n) {
        release(rs->dst_surface, "IDirect3DSurface9::Release");
        rs->dst_surface = 0;
        drop_temps(rs);
        if (rs->state_saved) {
            release(rs->saved_rt, "IDirect3DSurface9::Release");
            release(rs->saved_depth, "IDirect3DSurface9::Release");
            rs->state_saved = 0;
        }
        release(rs->device, "IDirect3DDevice9::Release");
        hle_com_free(self);
    }
    RET(n, 1);
}
void dxr_GetDevice(void) {
    uint32_t self = A32(0), out = A32(1);
    uint32_t hr = D3D_OK;
    if (!out) {
        fprintf(stderr, "[d3dx9] ID3DXRenderToSurface::GetDevice: NULL out\n");
        hr = D3DERR_INVALIDCALL;
    } else {
        MEM32(out) = RS(self)->device;
        MEM32(RS(self)->device + 4)++;      /* AddRef, one RET only */
    }
    RET(hr, 2);                             /* this, out */
}
void dxr_GetDesc(void) {
    uint32_t self = A32(0), out = A32(1);
    uint32_t hr = D3D_OK;
    if (!out) {
        fprintf(stderr, "[d3dx9] ID3DXRenderToSurface::GetDesc: NULL desc\n");
        hr = D3DERR_INVALIDCALL;
    } else {
        rts_state_t *rs = RS(self);
        uint32_t *d = (uint32_t *)hle_ptr(out);
        d[0] = rs->width; d[1] = rs->height; d[2] = rs->format;
        d[3] = rs->depth_stencil; d[4] = rs->depth_format;
    }
    RET(hr, 2);                             /* this, desc */
}
void dxr_BeginScene(void) {
    uint32_t self = A32(0), surf = A32(1), vpva = A32(2);
    rts_state_t *rs = RS(self);
    D3DSURFACE_DESC sd;
    D3DVIEWPORT9 vp;
    uint32_t hr, target = 0, pout;

    if (!surf || rs->dst_surface) {
        fprintf(stderr, "[d3dx9] ID3DXRenderToSurface::BeginScene: NULL or busy\n");
        RET(D3DERR_INVALIDCALL, 3);
    }
    hr = surface_desc(surf, &sd);
    if (hr != D3D_OK) RET(hr, 3);
    if (sd.Format != rs->format || sd.Width != rs->width || sd.Height != rs->height) {
        fprintf(stderr, "[d3dx9] ID3DXRenderToSurface::BeginScene: surface mismatch\n");
        RET(D3DERR_INVALIDCALL, 3);
    }
    if (vpva) {
        memcpy(&vp, hle_ptr(vpva), sizeof vp);
        if (vp.X > rs->width || vp.Y > rs->height ||
            vp.X + vp.Width > rs->width || vp.Y + vp.Height > rs->height ||
            !(sd.Usage & D3DUSAGE_RENDERTARGET) &&
                (vp.X || vp.Y || vp.Width != rs->width || vp.Height != rs->height)) {
            fprintf(stderr, "[d3dx9] ID3DXRenderToSurface::BeginScene: bad viewport\n");
            RET(D3DERR_INVALIDCALL, 3);
        }
    }
    hr = capture_state(rs);
    if (hr != D3D_OK) RET(hr, 3);

    if (sd.Usage & D3DUSAGE_RENDERTARGET) {
        target = surf;
        hr = vcall("IDirect3DDevice9::SetRenderTarget", 3,
                   (uint32_t[]){ rs->device, 0u, target });
    } else {
        pout = hle_va(hle_alloc(4));
        if (!pout) hr = RTS_E_OUTOFMEMORY;
        else {
            MEM32(pout) = 0;
            hr = vcall("IDirect3DDevice9::CreateRenderTarget", 9,
                       (uint32_t[]){ rs->device, rs->width, rs->height, rs->format,
                                     0u, 0u, 0u, pout, 0u });
            rs->temp_rt = hr == D3D_OK ? MEM32(pout) : 0;
            hle_free(hle_ptr(pout));
            if (hr == D3D_OK && rs->temp_rt)
                hr = vcall("IDirect3DDevice9::SetRenderTarget", 3,
                           (uint32_t[]){ rs->device, 0u, rs->temp_rt });
            else if (hr == D3D_OK) {
                fprintf(stderr, "[d3dx9] ID3DXRenderToSurface::BeginScene: no render target\n");
                hr = D3DERR_INVALIDCALL;
            }
        }
    }
    if (hr != D3D_OK) goto fail;

    if (rs->depth_stencil) {
        pout = hle_va(hle_alloc(4));
        if (!pout) hr = RTS_E_OUTOFMEMORY;
        else {
            MEM32(pout) = 0;
            hr = vcall("IDirect3DDevice9::CreateDepthStencilSurface", 9,
                       (uint32_t[]){ rs->device, rs->width, rs->height,
                                     rs->depth_format, 0u, 0u, 1u, pout, 0u });
            rs->temp_depth = hr == D3D_OK ? MEM32(pout) : 0;
            hle_free(hle_ptr(pout));
        }
        if (hr != D3D_OK) goto fail;
    }
    hr = vcall("IDirect3DDevice9::SetDepthStencilSurface", 2,
               (uint32_t[]){ rs->device, rs->temp_depth });
    if (hr != D3D_OK) goto fail;
    if (vpva) {
        hr = vcall("IDirect3DDevice9::SetViewport", 2,
                   (uint32_t[]){ rs->device, vpva });
        if (hr != D3D_OK) goto fail;
    }
    addref(surf, "IDirect3DSurface9::AddRef");
    rs->dst_surface = surf;
    rs->in_scene = 1;
    hr = vcall("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ rs->device });
    if (hr == D3D_OK) RET(hr, 3);

fail:
    rs->in_scene = 0;
    release(rs->dst_surface, "IDirect3DSurface9::Release");
    rs->dst_surface = 0;
    drop_temps(rs);
    restore_state(rs);
    RET(hr, 3);                             /* this, surface, viewport */
}
void dxr_EndScene(void) {
    uint32_t self = A32(0), filter = A32(1);
    rts_state_t *rs = RS(self);
    uint32_t hr;
    if (!rs->in_scene) {
        fprintf(stderr, "[d3dx9] ID3DXRenderToSurface::EndScene: no active scene\n");
        RET(D3DERR_INVALIDCALL, 2);
    }
    hr = vcall("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ rs->device });
    if (rs->temp_rt) {
        uint32_t copy = vcall("D3DXLoadSurfaceFromSurface", 8,
            (uint32_t[]){ rs->dst_surface, 0u, 0u, rs->temp_rt, 0u, 0u,
                          filter, 0u });
        if (copy != D3D_OK) {
            fprintf(stderr, "[d3dx9] ID3DXRenderToSurface::EndScene: surface copy failed (0x%08X)\n", copy);
            hr = copy;
        }
    }
    restore_state(rs);
    drop_temps(rs);
    release(rs->dst_surface, "IDirect3DSurface9::Release");
    rs->dst_surface = 0;
    rs->in_scene = 0;
    RET(hr, 2);                             /* this, mipfilter */
}
void dxr_OnLostDevice(void) {
    /* This object allocates no persistent device resources: RT and depth
     * surfaces exist only between BeginScene and EndScene. Nothing to free. */
    fprintf(stderr, "[d3dx9] ID3DXRenderToSurface::OnLostDevice: no persistent resources\n");
    RET(D3D_OK, 1);                         /* this */
}
void dxr_OnResetDevice(void) {
    fprintf(stderr, "[d3dx9] ID3DXRenderToSurface::OnResetDevice: no persistent resources\n");
    RET(D3D_OK, 1);                         /* this */
}

/* The nine entries mirror the public ID3DXRenderToSurface vtable order. */
static const win32hle_shim rts_methods[] = {
    { "dxr_QueryInterface", d3d_QueryInterface },
    { "dxr_AddRef",         dxr_AddRef },
    { "dxr_Release",        dxr_Release },
    { "dxr_GetDevice",      dxr_GetDevice },
    { "dxr_GetDesc",        dxr_GetDesc },
    { "dxr_BeginScene",     dxr_BeginScene },
    { "dxr_EndScene",       dxr_EndScene },
    { "dxr_OnLostDevice",   dxr_OnLostDevice },
    { "dxr_OnResetDevice",  dxr_OnResetDevice },
    { 0, 0 }
};

void s_D3DXCreateRenderToSurface(void) {
    /* (device, width, height, format, stencil, stencil_format, out) */
    uint32_t dev = A32(0), w = A32(1), h = A32(2), fmt = A32(3);
    uint32_t stencil = A32(4), sfmt = A32(5), out = A32(6);
    uint32_t hr;
    if (!dev || !out) {
        fprintf(stderr, "[d3dx9] D3DXCreateRenderToSurface: NULL device or out\n");
        RET(D3DERR_INVALIDCALL, 7);
    }
    MEM32(out) = 0;
    if (!g_vt_rts) g_vt_rts = hle_com_vtable(rts_methods);
    {
        uint32_t obj = hle_com_new(g_vt_rts, 8u + sizeof(rts_state_t));
        if (!obj) { RET(RTS_E_OUTOFMEMORY, 7); }
        {
            rts_state_t *rs = RS(obj);
            memset(rs, 0, sizeof *rs);
            rs->device = dev;
            rs->width = w; rs->height = h; rs->format = fmt;
            rs->depth_stencil = stencil; rs->depth_format = sfmt;
            addref(dev, "IDirect3DDevice9::AddRef");
            MEM32(out) = obj;
        }
    }
    hr = D3D_OK;
    RET(hr, 7);
}
