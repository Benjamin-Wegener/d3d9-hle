/*
 * d3d9_caps.c - capability answers derived from what this HLE can render.
 * See the evidence table in d3d9_caps.h.
 */
#include <string.h>
#include "d3d9_hle.h"
#include "d3d9_format.h"
#include "d3d9_caps.h"

uint32_t d3d9_caps_check_format(uint32_t adapter_format, uint32_t usage,
                                uint32_t res_type, uint32_t check_format)
{
    (void)adapter_format;
    /* The only real depth surface is the backend's Depth32Float attachment;
     * it has no stencil. d3d9_format.c covers the colour formats, not this. */
    if (usage & D3DUSAGE_DEPTHSTENCIL) {
        if ((usage & ~(uint32_t)D3DUSAGE_DEPTHSTENCIL) != 0u)
            return D3DERR_NOTAVAILABLE;
        return (res_type == D3DRTYPE_SURFACE &&
                check_format == D3DFMT_D32F_LOCKABLE) ? D3D_OK : D3DERR_NOTAVAILABLE;
    }
    /* d3d9_format.c is the single source of truth for pixel formats. */
    if (check_format == D3DFMT_UNKNOWN || !d3d9_format_is_supported(check_format))
        return D3DERR_NOTAVAILABLE;
    /* Metal has 2D textures (also cube) and surfaces, but no volume path. */
    if (res_type != D3DRTYPE_SURFACE && res_type != D3DRTYPE_TEXTURE &&
        res_type != D3DRTYPE_CUBETEXTURE)
        return D3DERR_NOTAVAILABLE;
    /* Render targets are BGRA8 only in the Metal backend (create_render_target). */
    if (usage & D3DUSAGE_RENDERTARGET) {
        if (check_format != D3DFMT_A8R8G8B8 && check_format != D3DFMT_X8R8G8B8)
            return D3DERR_NOTAVAILABLE;
    }
    return D3D_OK;
}

uint32_t d3d9_caps_check_multisample(uint32_t format, uint32_t windowed,
                                     uint32_t sample_type, uint32_t *quality)
{
    (void)windowed;
    /* The Metal backend renders into a real multisample texture and resolves
     * it with a blit pass (d3d9_metal.m). 2x and 4x are supported at quality 0. */
    if (quality) *quality = 0;
    if (sample_type == 0u || sample_type == 2u || sample_type == 4u) return D3D_OK;
    return D3DERR_NOTAVAILABLE;
}

uint32_t d3d9_caps_check_depth_stencil(uint32_t adapter, uint32_t device_type,
                                       uint32_t adapter_format, uint32_t rt_format,
                                       uint32_t ds_format)
{
    if (adapter != 0u || device_type != D3DDEVTYPE_HAL)
        return D3DERR_INVALIDCALL;
    /* Both formats must be ones this HLE can really create. */
    if (!d3d9_format_is_supported(adapter_format) ||
        d3d9_caps_check_format(adapter_format, D3DUSAGE_RENDERTARGET,
                               D3DRTYPE_SURFACE, rt_format) != D3D_OK)
        return D3DERR_NOTAVAILABLE;
    /* Backend attachment is MTLPixelFormatDepth32Float with no stencil. The
     * public D3D9 name for that depth format is D32F_LOCKABLE; every other
     * depth/stencil combination is unsupported. */
    return ds_format == D3DFMT_D32F_LOCKABLE ? D3D_OK : D3DERR_NOTAVAILABLE;
}

uint32_t d3d9_caps_mode_count(uint32_t format)
{
    if (!d3d9_format_is_supported(format)) return 0u;
    return D3D9_CAPS_ADAPTER_MODES;
}

uint32_t d3d9_caps_enum_mode(uint32_t format, uint32_t index, d3d9_caps_mode *out)
{
    /* Standard desktop modes. */
    static const uint16_t modes[D3D9_CAPS_ADAPTER_MODES][2] = {
        { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 720 }
    };
    if (!out || index >= D3D9_CAPS_ADAPTER_MODES) return D3DERR_INVALIDCALL;
    if (!d3d9_format_is_supported(format)) return D3DERR_NOTAVAILABLE;
    out->Width = modes[index][0];
    out->Height = modes[index][1];
    out->RefreshRate = 60u;
    out->Format = format ? format : D3DFMT_X8R8G8B8;
    return D3D_OK;
}

void d3d9_caps_display_mode(d3d9_caps_mode *out)
{
    if (!out) return;
    out->Width = D3D9_DEFAULT_WIDTH;
    out->Height = D3D9_DEFAULT_HEIGHT;
    out->RefreshRate = 60u;
    out->Format = D3DFMT_X8R8G8B8;
}

void d3d9_caps_fill(D3DCAPS9 *c)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->DeviceType = D3DDEVTYPE_HAL;
    c->AdapterOrdinal = 0;

    /* Shader models 1.1..3.0 are really translated by d3d9_shader.c. */
    c->VertexShaderVersion = D3DVS_VERSION(3, 0);
    c->PixelShaderVersion = D3DPS_VERSION(3, 0);
    c->MaxVertexShaderConst = 256;

    /* 6 user clip planes are honoured by the FFP and translated shaders
     * (d3d9_metal.m buffer(4), g_clip_planes[6][4]). */
    c->MaxUserClipPlanes = 6;
    /* Gamma ramps are applied to swap chain 0 at Present by a Metal pass;
     * D3DCAPS2_CANCALIBRATEGAMMA stays clear (no driver calibration). */
    c->Caps2 = D3D9_CAPS2_FULLSCREENGAMMA;

    /* d3dx9_texture.c uses 8192 as its dimension limit; larger levels are
     * rejected by d3dx9_texture_create(). */
    c->MaxTextureWidth = 8192;
    c->MaxTextureHeight = 8192;
    c->MaxTextureRepeat = 8192;
    c->MaxTextureAspectRatio = 8192;

    /* A vertex shader can transform vertices, so this bit is kept; the FFP
     * generator (d3d9_ffp.c) covers the public fixed-function operations. */
    c->DevCaps = 0x00020000u;        /* D3DDEVCAPS_HWTRANSFORMANDLIGHT */

    /* Only the modes cull_mode() and current_depth_state() implement. */
    c->PrimitiveMiscCaps = 0x00000070u; /* CULLNONE|CULLCW|CULLCCW */
    c->RasterCaps = D3D9_CAPS_RASTER_ZTEST | D3D9_CAPS_RASTER_SCISSORTEST |
                    D3D9_CAPS_RASTER_ANISOTROPY;
    c->ZCmpCaps = D3D9_CAPS_ZCMP_1_TO_8;
    c->SrcBlendCaps = D3D9_CAPS_BLEND_ZERO_TO_INVDESTCOLOR;
    c->DestBlendCaps = D3D9_CAPS_BLEND_ZERO_TO_INVDESTCOLOR;

    /* The sampler descriptor is nearest/min and nearest/mag with clamp; no
     * mipmaps are created, and cube textures use the same sampler state. */
    c->TextureFilterCaps = D3D9_CAPS_TEXFILTER_MINFPOINT |
                           D3D9_CAPS_TEXFILTER_MAGFPOINT;
    c->CubeTextureFilterCaps = c->TextureFilterCaps;
    c->TextureAddressCaps = D3D9_CAPS_ADDRESS_CLAMP;
    c->MaxAnisotropy = 1;

    /* 2D/cube textures with alpha and perspective-correct interpolation exist;
     * arbitrary dimensions are allowed, so POW2/SQUAREONLY are not advertised. */
    c->TextureCaps = D3D9_CAPS_TEX_PERSPECTIVE | D3D9_CAPS_TEX_ALPHA |
                     D3D9_CAPS_TEX_CUBEMAP;
    c->MaxVolumeExtent = 0;
    c->StencilCaps = 0;                 /* depth 32F only, no stencil attachment */
    c->MaxTextureBlendStages = 8;
    c->MaxSimultaneousTextures = 16;    /* 16 sampler slots in prepare_encoder() */
    c->MaxStreams = 16;
    c->MaxStreamStride = 1024;
    c->MaxPrimitiveCount = 0x00555555u;
    c->MaxVertexIndex = 0x00FFFFFFu;

    /* Only render-target index 0 is bound (dev9_SetRenderTarget). */
    c->NumSimultaneousRTs = 1;
}
