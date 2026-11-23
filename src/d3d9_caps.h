/*
 * d3d9_caps.h - what this HLE's D3D9 implementation actually supports.
 *
 * The answers here are derived from the code that implements them, not from
 * what any one application expects. Supported formats come from d3d9_format.c; rendering
 * features from d3d9_metal.m; shader models from d3d9_shader.c.
 */
#ifndef WIN32HLE_D3D9_CAPS_H
#define WIN32HLE_D3D9_CAPS_H

#include <stdint.h>
#include "d3d9_hle.h"

#define D3D9_CAPS_ADAPTER_MODES 4

/* Public D3D9 cap bits used below, named so the reported value and its
 * meaning cannot drift apart (the previous numeric comment did). */
#define D3D9_CAPS_RASTER_ZTEST              0x00000010u /* D3DPRASTERCAPS_ZTEST */
#define D3D9_CAPS_RASTER_SCISSORTEST        0x01000000u /* D3DPRASTERCAPS_SCISSORTEST */
#define D3D9_CAPS_TEX_PERSPECTIVE           0x00000001u /* D3DPTEXTURECAPS_PERSPECTIVE */
#define D3D9_CAPS_TEX_ALPHA                 0x00000004u /* D3DPTEXTURECAPS_ALPHA */
#define D3D9_CAPS_TEX_CUBEMAP               0x00000800u /* D3DPTEXTURECAPS_CUBEMAP */
#define D3D9_CAPS_TEXFILTER_MINFPOINT       0x00000100u /* D3DPTFILTERCAPS_MINFPOINT */
#define D3D9_CAPS_TEXFILTER_MAGFPOINT       0x01000000u /* D3DPTFILTERCAPS_MAGFPOINT */
#define D3D9_CAPS_RASTER_ANISOTROPY         0x00020000u /* D3DPRASTERCAPS_ANISOTROPY */
#define D3D9_CAPS_ADDRESS_CLAMP             0x00000004u /* D3DPTADDRESSCAPS_CLAMP */
#define D3D9_CAPS_ZCMP_1_TO_8               0x000000FEu /* D3DCMP_NEVER..ALWAYS */
#define D3D9_CAPS_BLEND_ZERO_TO_INVDESTCOLOR 0x000007FEu /* D3DBLEND_1..10 */
/* Public d3d9caps.h: full-screen gamma ramps are applied by d3d9_metal.m's
 * Present pass. CANCALIBRATEGAMMA is deliberately NOT advertised: D3DSGR_
 * CALIBRATE transfers the ramp unchanged without hardware calibration. */
#define D3D9_CAPS2_FULLSCREENGAMMA           0x00020000u /* D3DCAPS2_FULLSCREENGAMMA */

/* An adapter mode list independent of any single title: standard desktop
 * resolutions plus refresh rate 60 Hz. */
typedef struct {
    uint32_t Width, Height, RefreshRate, Format;
} d3d9_caps_mode;

/* Evidence table (all rows verified against the current source):
 *
 * | Answer                          | Code evidence                                   |
 * |---------------------------------|-------------------------------------------------|
 * | A8/L8/A8L8/R5G6B5/X1R5G5B5/     | d3d9_format.c d3d9_format_bytes_per_pixel()      |
 * | A1R5G5B5/A4R4G4B4/X4R4G4B4/     |                                                 |
 * | A8R8G8B8/X8R8G8B8                |                                                 |
 * | DXT1/DXT3/DXT5                  | d3d9_format.c d3d9_format_block_bytes()          |
 * | everything else: not available  | d3d9_format_is_supported() returns 0             |
 * | MSAA_NONE only                  | d3d9_metal.m: render pass has no sample count    |
 * | one render target (index 0)     | dev9_SetRenderTarget handles only index 0        |
 * | depth 32F, no stencil           | d3d9_metal.m init_device: Depth32Float           |
 * | D32F_LOCKABLE is the only depth/stencil format match (no stencil ops) | d3d9_metal.m ds_desc: MTLPixelFormatDepth32Float, StencilCaps == 0 |
 * | 2D texture + 2D render target   | d3d9_metal.m create_texture / create_render_target|
 * | arbitrary W/H, not square/POW2 only | d3d9_metal.m create_texture(width, height), NPOT allowed with clamp |
 * | cube texture layout supported   | d3d9_metal.m create_texture(is_cube != 0)        |
 * | 2D/cube mipmaps                 | d3d9_metal.m create_texture(levels), MIPFILTER in sampler |
 * | no volume texture / volume RT   | no backend entry point                           |
 * | VS/PS SM 1.1..3.0               | d3d9_shader.c d3d9_translate_shader_to_msl()     |
 * | MIN/MAG/MIP filtering           | d3d9_metal.m g_sampler: D3DSAMP 5/6/7 -> MTLSampler filters |
 * | clamp addressing only           | d3d9_metal.m g_sampler: s/t ClampToEdge          |
 * | ZTEST + SCISSORTEST only        | d3d9_metal.m current_depth_state()/apply_scissor()|
 * | compare funcs 1..8              | d3d9_metal.m depth_compare()                     |
 * | blend factors 1..10             | d3d9_metal.m blend_factor()                      |
 * | cull none/CW/CCW                | d3d9_metal.m cull_mode()                         |
 * | max texture 8192                | d3dx9_texture.h D3D9_TEX_MAX_DIM                 |
 */

uint32_t d3d9_caps_check_format(uint32_t adapter_format, uint32_t usage,
                                uint32_t res_type, uint32_t check_format);
uint32_t d3d9_caps_check_multisample(uint32_t format, uint32_t windowed,
                                     uint32_t sample_type, uint32_t *quality);
uint32_t d3d9_caps_check_depth_stencil(uint32_t adapter, uint32_t device_type,
                                       uint32_t adapter_format, uint32_t rt_format,
                                       uint32_t ds_format);
uint32_t d3d9_caps_mode_count(uint32_t format);
uint32_t d3d9_caps_enum_mode(uint32_t format, uint32_t index, d3d9_caps_mode *out);
void d3d9_caps_display_mode(d3d9_caps_mode *out);
void d3d9_caps_fill(D3DCAPS9 *caps);

#endif /* WIN32HLE_D3D9_CAPS_H */
