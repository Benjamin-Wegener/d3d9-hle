/*
 * d3d9_caps_selftest.c - capability answers must match what the HLE can
 * actually render (d3d9_format.c + d3d9_metal.m), never a title's needs.
 */
#define RECOMP_GENERATED_CODE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "win32hle.h"
#include "host.h"
#include "d3d9_hle.h"
#include "d3d9_format.h"
#include "d3d9_caps.h"

const recomp_dispatch_entry_t recomp_dispatch_table[] = { { 0, 0 } };
const uint32_t recomp_dispatch_count = 0;

static int fails;
static void ok(const char *what, int cond) { if (!cond) { printf("FAIL %s\n", what); fails++; } }
static uint32_t call(const char *name, int n, const uint32_t *a) {
    return hle_call_guest(hle_resolve(name), n, a);
}
#define A(p) hle_va(p)

int main(void)
{
    recomp_host_init();
    win32hle_register(win32hle_d3d9);
    uint32_t d3d9 = call("Direct3DCreate9", 1, (uint32_t[]){ 32 });
    ok("Direct3DCreate9", d3d9 != 0);

    /* ---- GetDeviceCaps ---- */
    D3DCAPS9 *caps = (D3DCAPS9 *)hle_alloc(sizeof(*caps));
    memset(caps, 0, sizeof(*caps));
    ok("GetDeviceCaps", call("IDirect3D9::GetDeviceCaps", 4,
                             (uint32_t[]){ d3d9, 0, D3DDEVTYPE_HAL, A(caps) }) == D3D_OK);
    ok("VS 3.0 really translated", caps->VertexShaderVersion == D3DVS_VERSION(3, 0));
    ok("PS 3.0 really translated", caps->PixelShaderVersion == D3DPS_VERSION(3, 0));
    ok("6 user clip planes really supported", caps->MaxUserClipPlanes == 6);
    ok("fullscreen gamma ramp advertised", caps->Caps2 == D3D9_CAPS2_FULLSCREENGAMMA);
    ok("no driver gamma calibration advertised", (caps->Caps2 & 0x00100000u) == 0);
    ok("max texture == d3d9_format/d3dx9_texture limit", caps->MaxTextureWidth == 8192);
    ok("no volume extent advertised", caps->MaxVolumeExtent == 0);
    ok("no stencil advertised", caps->StencilCaps == 0);
    ok("raster caps are ZTEST|SCISSORTEST|ANISOTROPY",
       caps->RasterCaps == (D3D9_CAPS_RASTER_ZTEST | D3D9_CAPS_RASTER_SCISSORTEST |
                            D3D9_CAPS_RASTER_ANISOTROPY));
    ok("compare caps are exactly D3DCMP_1..8", caps->ZCmpCaps == D3D9_CAPS_ZCMP_1_TO_8);
    ok("blend caps match blend_factor()'s 1..10",
       caps->SrcBlendCaps == D3D9_CAPS_BLEND_ZERO_TO_INVDESTCOLOR &&
       caps->DestBlendCaps == D3D9_CAPS_BLEND_ZERO_TO_INVDESTCOLOR);
    ok("min+mag point filtering advertised",
       caps->TextureFilterCaps == (D3D9_CAPS_TEXFILTER_MINFPOINT |
                                   D3D9_CAPS_TEXFILTER_MAGFPOINT));
    ok("cube sampler uses the same nearest filter",
       caps->CubeTextureFilterCaps == caps->TextureFilterCaps);
    ok("no mip filtering advertised",
       (caps->TextureFilterCaps & 0x00030000u) == 0);
    ok("clamp addressing advertised", caps->TextureAddressCaps == D3D9_CAPS_ADDRESS_CLAMP);
    ok("no anisotropy advertised", caps->MaxAnisotropy == 1);
    ok("texture caps are PERSPECTIVE|ALPHA|CUBEMAP only",
       caps->TextureCaps == (D3D9_CAPS_TEX_PERSPECTIVE | D3D9_CAPS_TEX_ALPHA |
                             D3D9_CAPS_TEX_CUBEMAP));
    ok("no POW2/SQUAREONLY restriction advertised",
       (caps->TextureCaps & 0x00000022u) == 0);
    ok("sampler slots match backend", caps->MaxSimultaneousTextures == 16);

    /* ---- CheckDeviceFormat: formats really supported by d3d9_format ---- */
    uint32_t *q = (uint32_t *)hle_alloc(4);
    (void)q;
    uint32_t a_fmt[7] = { d3d9, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
                          D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, D3DFMT_A8R8G8B8 };
    ok("A8R8G8B8 render target OK", call("IDirect3D9::CheckDeviceFormat", 7, a_fmt) == D3D_OK);
    a_fmt[6] = D3DFMT_DXT1;
    ok("DXT1 not available as render target", call("IDirect3D9::CheckDeviceFormat", 7, a_fmt) == D3DERR_NOTAVAILABLE);
    a_fmt[4] = 0; /* USAGE 0 -> texture usage path */
    a_fmt[6] = D3DFMT_DXT1;
    ok("DXT1 OK as texture", call("IDirect3D9::CheckDeviceFormat", 7, a_fmt) == D3D_OK);
    a_fmt[4] = D3DUSAGE_RENDERTARGET;
    a_fmt[6] = D3DFMT_R16F;
    ok("R16F is an honest error", call("IDirect3D9::CheckDeviceFormat", 7, a_fmt) == D3DERR_NOTAVAILABLE);
    a_fmt[6] = D3DFMT_DXT2;
    ok("DXT2 is an honest error", call("IDirect3D9::CheckDeviceFormat", 7, a_fmt) == D3DERR_NOTAVAILABLE);
    a_fmt[6] = D3DFMT_UNKNOWN;
    ok("UNKNOWN is an honest error", call("IDirect3D9::CheckDeviceFormat", 7, a_fmt) == D3DERR_NOTAVAILABLE);
    a_fmt[5] = D3DRTYPE_VOLUME; a_fmt[6] = D3DFMT_A8R8G8B8;
    ok("volume resource is an honest error",
       call("IDirect3D9::CheckDeviceFormat", 7, a_fmt) == D3DERR_NOTAVAILABLE);
    a_fmt[4] = D3DUSAGE_DEPTHSTENCIL;
    a_fmt[5] = D3DRTYPE_SURFACE;
    a_fmt[6] = D3DFMT_D32F_LOCKABLE;
    ok("D32F_LOCKABLE depth surface OK (Depth32Float backend)",
       call("IDirect3D9::CheckDeviceFormat", 7, a_fmt) == D3D_OK);
    a_fmt[6] = D3DFMT_D24S8;
    ok("D24S8 depth surface is an honest error (no stencil)",
       call("IDirect3D9::CheckDeviceFormat", 7, a_fmt) == D3DERR_NOTAVAILABLE);
    a_fmt[6] = D3DFMT_D16;
    ok("D16 depth surface is an honest error",
       call("IDirect3D9::CheckDeviceFormat", 7, a_fmt) == D3DERR_NOTAVAILABLE);

    /* ---- CheckDepthStencilMatch: bounded to the one real depth format ---- */
    uint32_t a_ds[6] = { d3d9, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
                         D3DFMT_A8R8G8B8, D3DFMT_D32F_LOCKABLE };
    ok("D32F_LOCKABLE depth match OK",
       call("IDirect3D9::CheckDepthStencilMatch", 6, a_ds) == D3D_OK);
    a_ds[5] = D3DFMT_D24S8;
    ok("D24S8 depth match is NOTAVAILABLE",
       call("IDirect3D9::CheckDepthStencilMatch", 6, a_ds) == D3DERR_NOTAVAILABLE);
    a_ds[5] = D3DFMT_D16;
    ok("D16 depth match is NOTAVAILABLE",
       call("IDirect3D9::CheckDepthStencilMatch", 6, a_ds) == D3DERR_NOTAVAILABLE);
    a_ds[1] = 1; a_ds[5] = D3DFMT_D32F_LOCKABLE;
    ok("nonzero adapter is INVALIDCALL",
       call("IDirect3D9::CheckDepthStencilMatch", 6, a_ds) == D3DERR_INVALIDCALL);
    a_ds[1] = 0; a_ds[4] = D3DFMT_R16F;
    ok("unsupported render target is NOTAVAILABLE",
       call("IDirect3D9::CheckDepthStencilMatch", 6, a_ds) == D3DERR_NOTAVAILABLE);
    a_ds[4] = D3DFMT_DXT1;  /* supported as a texture, not as a render target */
    ok("non-renderable render target is NOTAVAILABLE",
       call("IDirect3D9::CheckDepthStencilMatch", 6, a_ds) == D3DERR_NOTAVAILABLE);

    /* ---- CheckDeviceMultiSampleType: Metal RT has real sample counts ---- */
    uint32_t *quality = (uint32_t *)hle_alloc(4);
    *quality = 99;
    uint32_t a_ms[7] = { d3d9, 0, D3DDEVTYPE_HAL, D3DFMT_A8R8G8B8, 1, 0, A(quality) };
    ok("D3DMULTISAMPLE_NONE OK", call("IDirect3D9::CheckDeviceMultiSampleType", 7, a_ms) == D3D_OK);
    ok("quality is 0 for NONE", *quality == 0);
    a_ms[5] = 2;   /* D3DMULTISAMPLE_2_SAMPLES */
    *quality = 99;
    ok("2x MSAA is supported at quality 0",
       call("IDirect3D9::CheckDeviceMultiSampleType", 7, a_ms) == D3D_OK && *quality == 0);
    a_ms[5] = 4;
    ok("4x MSAA is supported at quality 0",
       call("IDirect3D9::CheckDeviceMultiSampleType", 7, a_ms) == D3D_OK && *quality == 0);
    a_ms[5] = 8;
    ok("8x MSAA is an honest error",
       call("IDirect3D9::CheckDeviceMultiSampleType", 7, a_ms) == D3DERR_NOTAVAILABLE);

    /* ---- Adapter modes: standard desktop resolutions ---- */
    uint32_t count = call("IDirect3D9::GetAdapterModeCount", 3,
                          (uint32_t[]){ d3d9, 0, D3DFMT_X8R8G8B8 });
    ok("mode count matches the caps table", count == D3D9_CAPS_ADAPTER_MODES);
    int distinct = 1;
    uint32_t first_w = 0, first_h = 0;
    for (uint32_t i = 0; i < count; i++) {
        D3DDISPLAYMODE *dm = (D3DDISPLAYMODE *)hle_alloc(sizeof(*dm));
        memset(dm, 0, sizeof(*dm));
        uint32_t hr = call("IDirect3D9::EnumAdapterModes", 5,
                           (uint32_t[]){ d3d9, 0, D3DFMT_X8R8G8B8, i, A(dm) });
        ok("enum mode call", hr == D3D_OK);
        if (i == 0) { first_w = dm->Width; first_h = dm->Height; }
        else if (dm->Width == first_w && dm->Height == first_h) distinct = 0;
        ok("mode has a refresh rate", dm->RefreshRate > 0);
    }
    ok("modes are distinct", distinct);
    ok("out of range mode is an error",
       call("IDirect3D9::EnumAdapterModes", 5,
            (uint32_t[]){ d3d9, 0, D3DFMT_X8R8G8B8, count, A(quality) }) != D3D_OK);

    D3DDISPLAYMODE *disp = (D3DDISPLAYMODE *)hle_alloc(sizeof(*disp));
    memset(disp, 0, sizeof(*disp));
    ok("GetAdapterDisplayMode", call("IDirect3D9::GetAdapterDisplayMode", 3,
                                     (uint32_t[]){ d3d9, 0, A(disp) }) == D3D_OK);
    ok("display mode is the documented default",
       disp->Width == D3D9_DEFAULT_WIDTH && disp->Height == D3D9_DEFAULT_HEIGHT);
    ok("display mode dimensions positive", disp->Width > 0 && disp->Height > 0);

    if (!fails) printf("d3d9_caps_selftest: all checks passed\n");
    return fails != 0;
}
