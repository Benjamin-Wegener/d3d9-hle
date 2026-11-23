/*
 * d3d9_internal.h - Internal data structures and forward declarations for D3D9 HLE.
 */
#ifndef WIN32HLE_D3D9_INTERNAL_H
#define WIN32HLE_D3D9_INTERNAL_H

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include "win32hle.h"
#include "d3d9_hle.h"
#include "d3d9_format.h"
#include "d3d9_shader.h"
#include "d3d9_metal.h"

#define T(...) do { if (d3d9_trace()) fprintf(stderr, "[d3d9] " __VA_ARGS__); } while (0)
static inline int d3d9_trace(void) {
    static int tr = -1;
    if (tr < 0) tr = getenv("HLE_D3D9TRACE") != NULL;
    return tr;
}

/* COM vtables (guest addresses) */
extern uint32_t g_vt_d3d9;
extern uint32_t g_vt_device9;
extern uint32_t g_vt_surface9;
extern uint32_t g_vt_tex9;
extern uint32_t g_vt_cubetex9;
extern uint32_t g_vt_vb9;
extern uint32_t g_vt_ib9;
extern uint32_t g_vt_decl9;
extern uint32_t g_vt_vs9;
extern uint32_t g_vt_ps9;
extern uint32_t g_vt_query9;
extern uint32_t g_vt_sb9;
extern uint32_t g_vt_swapchain9;
extern uint32_t g_vt_volumetex9;
extern uint32_t g_vt_volume9;

/* Object headers in guest memory:
 * [0]: vtable pointer
 * [4]: refcount
 * [8]: pointer to host context struct (or internal fields)
 */
#define OBJ_CTX(self) (*(void **)(hle_ptr((self) + 8)))
#define SET_OBJ_CTX(self, p) do { *(void **)(hle_ptr((self) + 8)) = (p); } while (0)

/* ---- Device Context ---- */
#define MAX_STREAMS 16
#define MAX_SAMPLERS 16
#define MAX_TEXTURE_STAGES 8
#define MAX_RENDER_STATES 256
#define MAX_SAMPLER_STATES 16
#define MAX_TSS 36
#define MAX_VS_CONST_F 256
#define MAX_PS_CONST_F 224
#define D3D9_MAX_VS_CONST_I 16
#define D3D9_MAX_VS_CONST_B 16
#define D3D9_MAX_PS_CONST_I 16
#define D3D9_MAX_PS_CONST_B 16
#define D3D9_MAX_PALETTES 4
#define D3D9_MAX_LIGHTS 8
#define D3D9_MAX_CLIP_PLANES 6
#define D3D9_PALETTE_ENTRIES 256
/* Public Wine d3d9.h / d3d9types.h values used by the state implementations. */
#define D3DRS_CLIPPLANEENABLE  152u
#define D3DSGR_NO_CALIBRATION  0u
#define D3DSGR_CALIBRATE       1u
/* D3DSTREAMSOURCE special frequency settings.  Public Wine d3d9types.h:
 * #define D3DSTREAMSOURCE_INDEXEDDATA  (1u << 30)
 * #define D3DSTREAMSOURCE_INSTANCEDATA (2u << 30)
 * The remaining low bits carry the count/divider. */
#define D3DSTREAMSOURCE_INDEXEDDATA  0x40000000u
#define D3DSTREAMSOURCE_INSTANCEDATA 0x80000000u
#define D3DSTREAMSOURCE_MASK         0x3FFFFFFFu

/* 32-bit packed D3DCOLOR (ARGB) to 4 normalized floats in [0, 1]. */
static inline void d3d9_d3dcolor_to_float4(uint32_t c, float out[4]) {
    out[0] = ((c >> 16) & 0xFFu) / 255.0f;
    out[1] = ((c >> 8) & 0xFFu) / 255.0f;
    out[2] = (c & 0xFFu) / 255.0f;
    out[3] = ((c >> 24) & 0xFFu) / 255.0f;
}

/* Vertex attribute slots consumed by a bound vertex declaration / FVF. */
typedef struct {
    uint32_t mask;      /* bitmask of attributes 0..15 consumed from Stream 0 */
    uint32_t vs_mask;   /* bitmask of attributes 0..15 consumed by custom VS */
    uint32_t inst_mask; /* bitmask of attributes 0..15 fed by Stream > 0 or instanced Stream 0 */
    uint32_t tex_mask;  /* bitmask of texcoord indices 0..7 present in the declaration */
} d3d9_vs_layout_t;
/* Sizes of the public D3D9 structures that are only copied through. */
#define D3D9_MATERIAL_BYTES 68   /* D3DMATERIAL9 */
#define D3D9_LIGHT_BYTES 104     /* D3DLIGHT9 */
#define D3D9_GAMMARAMP_BYTES (3u * 256u * 2u)  /* D3DGAMMARAMP */

/* D3DSTATEBLOCKTYPE values from d3d9types.h. */
#define D3DSBT_ALL          1u
#define D3DSBT_PIXELSTATE   2u
#define D3DSBT_VERTEXSTATE  3u

/* One SetStreamSource binding. The frequency setting is part of the vertex
 * state captured by state blocks, so it lives next to vb/offset/stride. */
typedef struct {
    uint32_t vb;                 /* guest vertex buffer pointer */
    uint32_t offset;
    uint32_t stride;
    uint32_t freq;               /* SetStreamSourceFreq setting */
} d3d9_stream_t;

/* Which states a state block holds. A block of a D3DSBT_* type selects whole
 * groups; a block recorded between BeginStateBlock and EndStateBlock selects
 * exactly the states the application set. Capture and Apply touch only the
 * selected states. */
typedef struct {
    uint8_t rs[MAX_RENDER_STATES];
    uint8_t samp[MAX_SAMPLERS][MAX_SAMPLER_STATES];
    uint8_t tss[MAX_TEXTURE_STAGES][MAX_TSS];
    uint8_t xform[512];
    uint8_t light[D3D9_MAX_LIGHTS], light_en[D3D9_MAX_LIGHTS];
    uint8_t clip[D3D9_MAX_CLIP_PLANES], palette[D3D9_MAX_PALETTES];
    uint8_t vsf[MAX_VS_CONST_F], vsi[D3D9_MAX_VS_CONST_I], vsb[D3D9_MAX_VS_CONST_B];
    uint8_t psf[MAX_PS_CONST_F], psi[D3D9_MAX_PS_CONST_I], psb[D3D9_MAX_PS_CONST_B];
    uint8_t tex[MAX_SAMPLERS], stream[MAX_STREAMS], freq[MAX_STREAMS];
    uint8_t viewport, scissor, material, cur_palette, svp, npatch;
    uint8_t indices, decl, fvf, vs, ps;
} d3d9_sb_mask_t;

typedef struct {
    uint32_t    self;               /* guest COM pointer */
    uint32_t    d3d9_obj;           /* parent IDirect3D9 guest pointer */
    uint32_t    adapter;
    uint32_t    dev_type;
    uint32_t    focus_window;
    uint32_t    behavior_flags;
    D3DPRESENT_PARAMETERS pp;

    /* Operational state (D3D9 device-loss states): OK while the device can be
     * presented to, LOST after a loss, NOT_RESET while a reset is pending. */
    uint32_t    device_state;
    uint32_t    implicit_rt;         /* device-created back buffer (D3DPOOL_DEFAULT) */
    uint32_t    implicit_ds;         /* device-created depth stencil (D3DPOOL_DEFAULT) */

    /* State */
    D3DVIEWPORT9 viewport;
    D3DRECT     scissor_rect;
    uint32_t    srgb_write;              /* D3DRS_SRGBWRITEENABLE */
    uint32_t    render_states[MAX_RENDER_STATES];
    uint32_t    sampler_states[MAX_SAMPLERS][MAX_SAMPLER_STATES];
    uint32_t    texture_stage_states[MAX_TEXTURE_STAGES][MAX_TSS];
    D3DMATRIX   transforms[512];    /* World (256..511), View (2), Projection (3), etc. */

    /* Pure state: setters store the value, getters return it. None of these
     * affects the rendered image yet, so they are not "OK without effect"
     * stubs - the observable Set/Get contract is honoured. */
    uint8_t     material[D3D9_MATERIAL_BYTES];
    uint8_t     lights[D3D9_MAX_LIGHTS][D3D9_LIGHT_BYTES];
    uint32_t    light_enable[D3D9_MAX_LIGHTS];
    uint8_t     light_defined[D3D9_MAX_LIGHTS];
    float       clip_planes[D3D9_MAX_CLIP_PLANES][4];
    uint8_t     palette[D3D9_MAX_PALETTES][D3D9_PALETTE_ENTRIES * 4];
    uint32_t    current_texture_palette;
    uint8_t     gamma_ramp[D3D9_GAMMARAMP_BYTES];
    uint32_t    software_vertex_processing;
    float       n_patch_mode;
    int32_t     vs_const_i[D3D9_MAX_VS_CONST_I][4];
    uint32_t    vs_const_b[D3D9_MAX_VS_CONST_B];
    int32_t     ps_const_i[D3D9_MAX_PS_CONST_I][4];
    uint32_t    ps_const_b[D3D9_MAX_PS_CONST_B];

    /* Resources bound */
    uint32_t    render_target;      /* guest surface pointer */
    uint32_t    depth_stencil;      /* guest surface pointer */
    uint32_t    textures[MAX_SAMPLERS];

    /* Geometry pipeline */
    d3d9_stream_t streams[MAX_STREAMS];
    uint32_t    indices;            /* guest index buffer pointer */
    uint32_t    vertex_decl;        /* guest vertex declaration pointer */
    uint32_t    fvf;

    /* Shaders */
    uint32_t    vertex_shader;
    uint32_t    pixel_shader;
    float       vs_const_f[MAX_VS_CONST_F][4];
    float       ps_const_f[MAX_PS_CONST_F][4];

    /* Dynamic index buffer for topology conversions (e.g. TRIANGLEFAN -> TRIANGLELIST) */
    void       *dynamic_ib;
    uint32_t    dynamic_ib_size;

    /* The default swap chain (returned by GetSwapChain, shared with the
     * application's render loop; Present is called on it every frame). */
    uint32_t    swapchain;          /* guest IDirect3DSwapChain9 pointer */
    uint32_t    extra_swapchains[15];
    uint32_t    num_extra;

    /* Scene / state-block recording status */
    int         in_scene;
    uint32_t    cursor_visible;  /* ShowCursor state: 1 at device creation */
    uint32_t    recording_state_block; /* nonzero between Begin/EndStateBlock */
    void       *sb_rec;            /* d3d9_sb_ctx_t: states set while recording */
    void       *sb_base;           /* d3d9_sb_ctx_t: device state at BeginStateBlock */
} d3d9_device_ctx_t;

/* ---- Resource contexts ---- */
typedef struct {
    uint32_t    self;
    uint32_t    device;
    D3DSURFACE_DESC desc;
    uint32_t    pitch;
    uint8_t    *pixels;             /* allocated with hle_alloc */
    uint32_t    pixels_va;
    int         locked;
    void       *gpu_rt;             /* Metal render target for RT surfaces */
    uint32_t    msaa_samples;       /* 0 = normal RT, 2/4 = multisample RT */
    void       *parent_tex;         /* pointer to d3d9_texture_ctx_t if part of texture */
    uint32_t    level;              /* mip level in parent texture */
    uint32_t    face;               /* cube face (0-5), 0 for 2D */
} d3d9_surface_ctx_t;

typedef struct {
    uint32_t    self;
    uint32_t    device;
    D3DVERTEXBUFFER_DESC desc;
    uint8_t    *data;               /* allocated with hle_alloc */
    uint32_t    data_va;
    int         locked;
    void       *gpu_buf;
} d3d9_vb_ctx_t;

typedef struct {
    uint32_t    self;
    uint32_t    device;
    D3DINDEXBUFFER_DESC desc;
    uint8_t    *data;               /* allocated with hle_alloc */
    uint32_t    data_va;
    int         locked;
    void       *gpu_buf;
} d3d9_ib_ctx_t;

typedef struct {
    uint32_t    self;
    uint32_t    device;
    uint32_t    width;
    uint32_t    height;
    uint32_t    levels;
    uint32_t    usage;
    uint32_t    format;
    uint32_t    pool;
    uint32_t   *surfaces;           /* array of guest surface pointers */
    void       *gpu_tex;
} d3d9_texture_ctx_t;

typedef struct {
    uint32_t    self;
    uint32_t    device;
    uint32_t    edge_length;
    uint32_t    levels;
    uint32_t    usage;
    uint32_t    format;
    uint32_t    pool;
    uint32_t    surfaces[6][16];    /* 6 faces x mip levels */
    void       *gpu_tex;
} d3d9_cubetexture_ctx_t;

typedef struct {
    uint32_t    self;
    uint32_t    device;
    D3DVOLUME_DESC desc;
    uint32_t    row_pitch;
    uint32_t    slice_pitch;
    uint8_t    *pixels;             /* allocated with hle_alloc */
    uint32_t    pixels_va;
    int         locked;
    void       *parent_tex;         /* pointer to d3d9_volumetexture_ctx_t if part of texture */
    uint32_t    level;              /* mip level in parent volume texture */
} d3d9_volume_ctx_t;

typedef struct {
    uint32_t    self;
    uint32_t    device;
    uint32_t    width;
    uint32_t    height;
    uint32_t    depth;
    uint32_t    levels;
    uint32_t    usage;
    uint32_t    format;
    uint32_t    pool;
    uint32_t   *volumes;            /* array of guest IDirect3DVolume9 pointers */
    void       *gpu_tex;
} d3d9_volumetexture_ctx_t;

typedef struct {
    uint32_t    self;
    uint32_t    device;
    uint32_t    num_elements;
    D3DVERTEXELEMENT9 elements[64];
} d3d9_decl_ctx_t;

typedef struct {
    uint32_t    self;
    uint32_t    device;
    uint32_t    byte_count;
    uint32_t   *tokens;
    char       *msl;
    void       *metal_pipe;
} d3d9_shader_ctx_t;

typedef struct {
    uint32_t    self;
    uint32_t    device;
    uint32_t    type;      /* public D3DQUERYTYPE value */
    uint32_t    issued;
    void       *metal_q;   /* GPU-backend query handle, NULL if no backend */
    const d3d9_gpu_ops_t *gpu; /* backend that owns metal_q (for Release) */
} d3d9_query_ctx_t;

/* A state block is a full snapshot of the device's capturable state. It is
 * laid out to mirror d3d9_device_ctx_t so capture/apply are plain copies of
 * the fields selected by the block's D3DSBT_* type. */
typedef struct {
    uint32_t    self;
    uint32_t    device;
    uint32_t    type;               /* D3DSBT_ALL / PIXELSTATE / VERTEXSTATE, 0 = recorded */
    d3d9_sb_mask_t m;               /* the states this block holds */

    D3DVIEWPORT9 viewport;
    D3DRECT     scissor_rect;
    uint32_t    render_states[MAX_RENDER_STATES];
    uint32_t    sampler_states[MAX_SAMPLERS][MAX_SAMPLER_STATES];
    uint32_t    texture_stage_states[MAX_TEXTURE_STAGES][MAX_TSS];
    D3DMATRIX   transforms[512];
    uint8_t     material[D3D9_MATERIAL_BYTES];
    uint8_t     lights[D3D9_MAX_LIGHTS][D3D9_LIGHT_BYTES];
    uint32_t    light_enable[D3D9_MAX_LIGHTS];
    uint8_t     light_defined[D3D9_MAX_LIGHTS];
    float       clip_planes[D3D9_MAX_CLIP_PLANES][4];
    uint8_t     palette[D3D9_MAX_PALETTES][D3D9_PALETTE_ENTRIES * 4];
    uint32_t    current_texture_palette;
    uint32_t    software_vertex_processing;
    float       n_patch_mode;
    int32_t     vs_const_i[D3D9_MAX_VS_CONST_I][4];
    uint32_t    vs_const_b[D3D9_MAX_VS_CONST_B];
    int32_t     ps_const_i[D3D9_MAX_PS_CONST_I][4];
    uint32_t    ps_const_b[D3D9_MAX_PS_CONST_B];

    uint32_t    textures[MAX_SAMPLERS];

    d3d9_stream_t streams[MAX_STREAMS];
    uint32_t    indices;
    uint32_t    vertex_decl;
    uint32_t    fvf;

    uint32_t    vertex_shader;
    uint32_t    pixel_shader;
    float       vs_const_f[MAX_VS_CONST_F][4];
    float       ps_const_f[MAX_PS_CONST_F][4];
} d3d9_sb_ctx_t;

typedef struct {
    uint32_t    self;
    uint32_t    device;
    D3DPRESENT_PARAMETERS pp;
    uint32_t    backbuffer;   /* guest IDirect3DSurface9 pointer for this swapchain */
    uint8_t     gamma_ramp[D3D9_GAMMARAMP_BYTES];
} d3d9_swapchain_ctx_t;

void init_d3d9_vtables(void);
void fill_device_caps(D3DCAPS9 *c);
void d9_QueryInterface(void);
void d9_CreateDevice(void);
uint32_t create_surface_internal(d3d9_device_ctx_t *dev, uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool);
uint32_t create_swapchain_internal(d3d9_device_ctx_t *dev);

/* Device method implementations needed by vtables */
void dev9_QueryInterface(void);
void dev9_Release(void);
void dev9_TestCooperativeLevel(void);
void dev9_GetAvailableTextureMem(void);
void dev9_EvictManagedResources(void);
void dev9_GetDirect3D(void);
void dev9_GetDeviceCaps(void);
void dev9_GetDisplayMode(void);
void dev9_GetCreationParameters(void);
void dev9_SetCursorProperties(void);
void dev9_SetCursorPosition(void);
void dev9_ShowCursor(void);

/* Public D3DPERF_* exports (stdcall, no this): Wine 11.0 include/d3d9.h. */
void perf_BeginEvent(void);
void perf_EndEvent(void);
void perf_SetMarker(void);
void perf_SetRegion(void);
void perf_QueryRepeatFrame(void);
void perf_SetOptions(void);
void perf_GetStatus(void);
void dev9_CreateAdditionalSwapChain(void);
void dev9_GetSwapChain(void);
void dev9_GetNumberOfSwapChains(void);
void dev9_Reset(void);
void dev9_Present(void);
void dev9_GetBackBuffer(void);
void dev9_GetRasterStatus(void);
void dev9_SetDialogBoxMode(void);
void dev9_SetGammaRamp(void);
void dev9_GetGammaRamp(void);
void dev9_CreateTexture(void);
void dev9_CreateVolumeTexture(void);
void dev9_CreateCubeTexture(void);
void dev9_CreateVertexBuffer(void);
void dev9_CreateIndexBuffer(void);
void dev9_CreateRenderTarget(void);
void dev9_CreateDepthStencilSurface(void);
void dev9_UpdateSurface(void);
void dev9_UpdateTexture(void);
void dev9_GetRenderTargetData(void);
void dev9_GetFrontBufferData(void);
void dev9_StretchRect(void);
void dev9_ColorFill(void);
void dev9_CreateOffscreenPlainSurface(void);
void dev9_SetRenderTarget(void);
void dev9_GetRenderTarget(void);
void dev9_SetDepthStencilSurface(void);
void dev9_GetDepthStencilSurface(void);
void dev9_BeginScene(void);
void dev9_EndScene(void);
void dev9_Clear(void);
void dev9_SetTransform(void);
void dev9_GetTransform(void);
void dev9_MultiplyTransform(void);
void dev9_SetViewport(void);
void dev9_GetViewport(void);
void dev9_SetMaterial(void);
void dev9_GetMaterial(void);
void dev9_SetLight(void);
void dev9_GetLight(void);
void dev9_LightEnable(void);
void dev9_GetLightEnable(void);
void dev9_SetClipPlane(void);
void dev9_GetClipPlane(void);
void dev9_SetRenderState(void);
void dev9_GetRenderState(void);
void dev9_CreateStateBlock(void);
void dev9_BeginStateBlock(void);
void dev9_EndStateBlock(void);
void dev9_SetClipStatus(void);
void dev9_GetClipStatus(void);
void dev9_GetTexture(void);
void dev9_SetTexture(void);
void dev9_GetTextureStageState(void);
void dev9_SetTextureStageState(void);
void dev9_GetSamplerState(void);
void dev9_SetSamplerState(void);
void dev9_ValidateDevice(void);
void dev9_SetPaletteEntries(void);
void dev9_GetPaletteEntries(void);
void dev9_SetCurrentTexturePalette(void);
void dev9_GetCurrentTexturePalette(void);
void dev9_SetScissorRect(void);
void dev9_GetScissorRect(void);
void dev9_SetSoftwareVertexProcessing(void);
void dev9_GetSoftwareVertexProcessing(void);
void dev9_SetNPatchMode(void);
void dev9_GetNPatchMode(void);
void dev9_DrawPrimitive(void);
void dev9_DrawIndexedPrimitive(void);
void dev9_DrawPrimitiveUP(void);
void dev9_DrawIndexedPrimitiveUP(void);
void dev9_ProcessVertices(void);
void dev9_CreateVertexDeclaration(void);
void dev9_SetVertexDeclaration(void);
void dev9_GetVertexDeclaration(void);
void dev9_SetFVF(void);
void dev9_GetFVF(void);
void dev9_CreateVertexShader(void);
void dev9_SetVertexShader(void);
void dev9_GetVertexShader(void);
void dev9_SetVertexShaderConstantF(void);
void dev9_GetVertexShaderConstantF(void);
void dev9_SetVertexShaderConstantI(void);
void dev9_GetVertexShaderConstantI(void);
void dev9_SetVertexShaderConstantB(void);
void dev9_GetVertexShaderConstantB(void);
void dev9_SetStreamSource(void);
void dev9_GetStreamSource(void);
void dev9_SetStreamSourceFreq(void);
void dev9_GetStreamSourceFreq(void);
void dev9_SetIndices(void);
void dev9_GetIndices(void);
void dev9_CreatePixelShader(void);
void dev9_SetPixelShader(void);
void dev9_GetPixelShader(void);
void dev9_SetPixelShaderConstantF(void);
void dev9_GetPixelShaderConstantF(void);
void dev9_SetPixelShaderConstantI(void);
void dev9_GetPixelShaderConstantI(void);
void dev9_SetPixelShaderConstantB(void);
void dev9_GetPixelShaderConstantB(void);
void dev9_DrawRectPatch(void);
void dev9_DrawTriPatch(void);
void dev9_DeletePatch(void);
void dev9_CreateQuery(void);

/* Resource method implementations */
void surf9_Release(void);
void surf9_GetDesc(void);
void surf9_LockRect(void);
void surf9_UnlockRect(void);
void surf9_GetDC(void);
void surf9_ReleaseDC(void);
void surf9_GetContainer(void);

void vb9_Release(void);
void vb9_Lock(void);
void vb9_Unlock(void);
void vb9_GetDesc(void);

void ib9_Release(void);
void ib9_Lock(void);
void ib9_Unlock(void);
void ib9_GetDesc(void);

void tex9_Release(void);
void tex9_GetLevelCount(void);
void tex9_GetLevelDesc(void);
void tex9_GetSurfaceLevel(void);
void tex9_LockRect(void);
void tex9_UnlockRect(void);
void tex9_AddDirtyRect(void);

void cubetex9_Release(void);
void cubetex9_GetLevelCount(void);
void cubetex9_GetLevelDesc(void);
void cubetex9_GetCubeMapSurface(void);
void cubetex9_LockRect(void);
void cubetex9_UnlockRect(void);
void cubetex9_AddDirtyRect(void);

void volumetex9_Release(void);
void volumetex9_SetLOD(void);
void volumetex9_GetLOD(void);
void volumetex9_GetLevelCount(void);
void volumetex9_SetAutoGenFilterType(void);
void volumetex9_GetAutoGenFilterType(void);
void volumetex9_GenerateMipSubLevels(void);
void volumetex9_GetLevelDesc(void);
void volumetex9_GetVolumeLevel(void);
void volumetex9_LockBox(void);
void volumetex9_UnlockBox(void);
void volumetex9_AddDirtyBox(void);

void vol9_Release(void);
void vol9_SetPriority(void);
void vol9_GetPriority(void);
void vol9_PreLoad(void);
void vol9_GetType(void);
void vol9_GetDesc(void);
void vol9_LockBox(void);
void vol9_UnlockBox(void);
void vol9_GetContainer(void);

void d3d9_stub_set_priv_data(void);
void d3d9_stub_get_priv_data(void);
void d3d9_stub_free_priv_data(void);

void decl9_Release(void);
void decl9_GetDeclaration(void);

void shader9_Release(void);
void shader9_GetFunction(void);




void query9_Release(void);
void query9_GetType(void);
void query9_GetDataSize(void);
void query9_Issue(void);
void query9_GetData(void);

void sb9_Release(void);
void sb9_Capture(void);
void sb9_Apply(void);

/* Shared state-block helpers (device methods create/recapture blocks, the
 * COM methods reuse the same code path). */
void sb9_select_type(d3d9_sb_ctx_t *sb, uint32_t type);
void sb9_capture_state(d3d9_sb_ctx_t *sb, const d3d9_device_ctx_t *dev);
void sb9_apply_state(const d3d9_sb_ctx_t *sb, d3d9_device_ctx_t *dev);
void sb9_release_held(d3d9_sb_ctx_t *sb);

/* Recording: every state setter reports what it set. */
typedef enum {
    SBK_RS, SBK_SAMP, SBK_TSS, SBK_XFORM, SBK_LIGHT, SBK_LIGHT_EN, SBK_CLIP,
    SBK_PALETTE, SBK_VSF, SBK_VSI, SBK_VSB, SBK_PSF, SBK_PSI, SBK_PSB,
    SBK_TEX, SBK_STREAM, SBK_FREQ, SBK_VIEWPORT, SBK_SCISSOR, SBK_MATERIAL,
    SBK_CUR_PALETTE, SBK_SVP, SBK_NPATCH, SBK_INDICES, SBK_DECL, SBK_FVF,
    SBK_VS, SBK_PS
} sb9_kind;
/* Mark states [a, a+n) of kind k (SBK_SAMP/SBK_TSS: a = index*16 + type, n = 1). */
void sb9_mark(d3d9_device_ctx_t *dev, sb9_kind k, uint32_t a, uint32_t n);
int  sb9_begin_record(d3d9_device_ctx_t *dev);
/* Ends recording: builds the block, restores the device state. 0 on failure. */
d3d9_sb_ctx_t *sb9_end_record(d3d9_device_ctx_t *dev);
void sb9_abort_record(d3d9_device_ctx_t *dev);

/* Push the device's full state to the GPU backend (used after StateBlock::Apply). */
void d3d9_device_push_backend(const d3d9_device_ctx_t *ctx);

void sc9_Release(void);
void sc9_Present(void);
void sc9_GetFrontBufferData(void);
void sc9_GetBackBuffer(void);
void sc9_GetRasterStatus(void);
void sc9_GetDisplayMode(void);
void sc9_GetDevice(void);
void sc9_GetPresentParameters(void);

uint32_t format_bpp(uint32_t fmt);


/* d3d9_com.c: shared by all D3D9 / D3DX9 COM objects */
void d3d_QueryInterface(void);
void d3d_res_SetPrivateData(void);
void d3d_res_GetPrivateData(void);
void d3d_res_FreePrivateData(void);
void d3d_res_SetPriority(void);
void d3d_res_GetPriority(void);
void d3d_res_PreLoad(void);
void d3d_surface_GetType(void);
void d3d_texture_GetType(void);
void d3d_volumetex_GetType(void);
void d3d_cubetex_GetType(void);
void d3d_vb_GetType(void);
void d3d_ib_GetType(void);
void d3d9_free_obj(uint32_t obj);
void tex9_SetLOD(void); void tex9_GetLOD(void); void tex9_SetAutoGenFilterType(void); void tex9_GetAutoGenFilterType(void); void tex9_GenerateMipSubLevels(void);
void cubetex9_SetLOD(void); void cubetex9_GetLOD(void); void cubetex9_SetAutoGenFilterType(void); void cubetex9_GetAutoGenFilterType(void); void cubetex9_GenerateMipSubLevels(void);   /* private data + priority dropped, then hle_com_free */

#endif /* WIN32HLE_D3D9_INTERNAL_H */

/* IDirect3D9 method implementations */
void d9_QueryInterface(void);
void d9_RegisterSoftwareDevice(void);
void d9_GetAdapterCount(void);
void d9_GetAdapterIdentifier(void);
void d9_GetAdapterModeCount(void);
void d9_EnumAdapterModes(void);
void d9_GetAdapterDisplayMode(void);
void d9_CheckDeviceType(void);
void d9_CheckDeviceFormat(void);
void d9_CheckDeviceMultiSampleType(void);
void d9_CheckDepthStencilMatch(void);
void d9_CheckDeviceFormatConversion(void);
void d9_GetDeviceCaps(void);
void d9_GetAdapterMonitor(void);
void d9_CreateDevice(void);

/* MSAA render target host API (real implementation in d3d9_metal.m). */
void *hle_d3d9_metal_create_msaa_render_target(uint32_t width, uint32_t height, uint32_t samples);
void  hle_d3d9_metal_destroy_msaa_render_target(void *rt);
void  hle_d3d9_metal_set_msaa_render_target(void *rt);
void  hle_d3d9_metal_resolve_msaa_render_target(void *rt);
