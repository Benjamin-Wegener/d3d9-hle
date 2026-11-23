/*
 * d3d9_hle.h - Direct3D 9 for win32hle: definitions, types and interfaces.
 */
#ifndef WIN32HLE_D3D9_HLE_H
#define WIN32HLE_D3D9_HLE_H

#include <stdint.h>
#include <stddef.h>
#include "win32hle.h"

/* The adapter's default display mode. A back buffer size of 0 means "the
 * window's client size" in D3D9; the HLE has no window to ask, so it uses
 * this mode (also what GetAdapterDisplayMode reports). */
#define D3D9_DEFAULT_WIDTH   1024u
#define D3D9_DEFAULT_HEIGHT  768u

/* HRESULT codes */
#define D3D_OK                              0x00000000u
#define D3DERR_INVALIDCALL                  0x8876086Cu
#define D3DERR_NOTAVAILABLE                 0x8876086Au
#define D3DERR_DEVICELOST                   0x88760868u
#define D3DERR_DEVICENOTRESET               0x88760869u
#define D3DERR_DRIVERINTERNALERROR          0x88760872u
#define D3DERR_OUTOFVIDEOMEMORY             0x8876017Cu

#define D3DERR_NOTFOUND                     0x88760866u   /* MAKE_D3DHRESULT(2150) */
#define D3DERR_MOREDATA                     0x88760867u   /* MAKE_D3DHRESULT(2151) */

/* D3D9 device operational states (IDirect3DDevice9::TestCooperativeLevel).
 * A device is operational (OK), lost (LOST) or in the not-reset state (NOT_RESET)
 * after a failed Reset; the methods report and recover through these states. */
#define D3D9_DEVICE_STATE_OK                0u
#define D3D9_DEVICE_STATE_LOST              1u
#define D3D9_DEVICE_STATE_NOT_RESET         2u

/* D3DQUERYTYPE, D3DISSUE_* and D3DGETDATA_*.
 * Public values from Wine's include/d3d9types.h:
 *   https://raw.githubusercontent.com/wine-mirror/wine/master/include/d3d9types.h
 * Only the public numeric values and method order are reproduced here. */
#define D3DQUERYTYPE_VCACHE                 4u
#define D3DQUERYTYPE_RESOURCEMANAGER        5u
#define D3DQUERYTYPE_VERTEXSTATS            6u
#define D3DQUERYTYPE_EVENT                  8u
#define D3DQUERYTYPE_OCCLUSION              9u
#define D3DQUERYTYPE_TIMESTAMP             10u
#define D3DQUERYTYPE_TIMESTAMPDISJOINT     11u
#define D3DQUERYTYPE_TIMESTAMPFREQ         12u
#define D3DQUERYTYPE_PIPELINETIMINGS       13u
#define D3DQUERYTYPE_INTERFACETIMINGS      14u
#define D3DQUERYTYPE_VERTEXTIMINGS         15u
#define D3DQUERYTYPE_PIXELTIMINGS          16u
#define D3DQUERYTYPE_BANDWIDTHTIMINGS      17u
#define D3DQUERYTYPE_CACHEUTILIZATION      18u
#define D3DQUERYTYPE_MEMORYPRESSURE        19u

#define D3DISSUE_BEGIN                      (1u << 1)
#define D3DISSUE_END                        (1u << 0)
#define D3DGETDATA_FLUSH                    (1u << 0)

/* D3DDEVTYPE */
#define D3DDEVTYPE_HAL                      1u
#define D3DDEVTYPE_REF                      2u
#define D3DDEVTYPE_SW                       3u
#define D3DDEVTYPE_NULLREF                  4u

/* D3DADAPTER */
#define D3DADAPTER_DEFAULT                  0u

/* Behavior flags */
#define D3DCREATE_FPU_PRESERVE              0x00000002u
#define D3DCREATE_MULTITHREADED             0x00000004u
#define D3DCREATE_PUREDEVICE                0x00000010u
#define D3DCREATE_SOFTWARE_VERTEXPROCESSING 0x00000020u
#define D3DCREATE_HARDWARE_VERTEXPROCESSING 0x00000040u
#define D3DCREATE_MIXED_VERTEXPROCESSING    0x00000080u
#define D3DCREATE_DISABLE_DRIVER_MANAGEMENT 0x00000100u
#define D3DCREATE_ADAPTERGROUP_DEVICE       0x00000200u
#define D3DCREATE_DISABLE_PRINTSCREEN       0x00008000u
#define D3DCREATE_SCREENSAVER               0x10000000u

/* D3DFORMAT */
#define D3DFMT_UNKNOWN                      0u
#define D3DFMT_R8G8B8                       20u
#define D3DFMT_A8R8G8B8                     21u
#define D3DFMT_X8R8G8B8                     22u
#define D3DFMT_R5G6B5                       23u
#define D3DFMT_X1R5G5B5                     24u
#define D3DFMT_A1R5G5B5                     25u
#define D3DFMT_A4R4G4B4                     26u
#define D3DFMT_R3G3B2                       27u
#define D3DFMT_A8                           28u
#define D3DFMT_A8R3G3B2                     29u
#define D3DFMT_X4R4G4B4                     30u
#define D3DFMT_A2B10G10R10                  31u
#define D3DFMT_A8B8G8R8                     32u
#define D3DFMT_X8B8G8R8                     33u
#define D3DFMT_G16R16                       34u
#define D3DFMT_A2R10G10B10                  35u
#define D3DFMT_A16B16G16R16                 36u
#define D3DFMT_A8P8                         40u
#define D3DFMT_P8                           41u
#define D3DFMT_L8                           50u
#define D3DFMT_A8L8                         51u
#define D3DFMT_A4L4                         52u
#define D3DFMT_V8U8                         60u
#define D3DFMT_L6V5U5                       61u
#define D3DFMT_X8L8V8U8                     62u
#define D3DFMT_Q8W8V8U8                     63u
#define D3DFMT_V16U16                       64u
#define D3DFMT_A2W10V10U10                  67u
#define D3DFMT_D16_LOCKABLE                 70u
#define D3DFMT_D32                          71u
#define D3DFMT_D15S1                        73u
#define D3DFMT_D24S8                        75u
#define D3DFMT_D24X8                        77u
#define D3DFMT_D24X4S4                      79u
#define D3DFMT_D16                          80u
#define D3DFMT_D32F_LOCKABLE                82u
#define D3DFMT_D24FS8                       83u
#define D3DFMT_D32_LOCKABLE                 84u
#define D3DFMT_S8_LOCKABLE                  85u
#define D3DFMT_L16                          81u
#define D3DFMT_VERTEXDATA                   100u
#define D3DFMT_INDEX16                      101u
#define D3DFMT_INDEX32                      102u
#define D3DFMT_Q16W16V16U16                 110u
#define D3DFMT_R16F                         111u
#define D3DFMT_G16R16F                      112u
#define D3DFMT_A16B16G16R16F                113u
#define D3DFMT_R32F                         114u
#define D3DFMT_G32R32F                      115u
#define D3DFMT_A32B32G32R32F                116u
#define D3DFMT_CxV8U8                       117u

#define D3D_FOURCC(c0, c1, c2, c3) \
    ((uint32_t)(uint8_t)(c0) | ((uint32_t)(uint8_t)(c1) << 8) | \
    ((uint32_t)(uint8_t)(c2) << 16) | ((uint32_t)(uint8_t)(c3) << 24))

#define D3DFMT_DXT1                         D3D_FOURCC('D','X','T','1')
#define D3DFMT_DXT2                         D3D_FOURCC('D','X','T','2')
#define D3DFMT_DXT3                         D3D_FOURCC('D','X','T','3')
#define D3DFMT_DXT4                         D3D_FOURCC('D','X','T','4')
#define D3DFMT_DXT5                         D3D_FOURCC('D','X','T','5')
#define D3DFMT_NULL                         D3D_FOURCC('N','U','L','L')
#define D3DFMT_RESZ                         D3D_FOURCC('R','E','S','Z')
#define D3DFMT_ATI1                         D3D_FOURCC('A','T','I','1')
#define D3DFMT_ATI2                         D3D_FOURCC('A','T','I','2')
#define D3DFMT_INTZ                         D3D_FOURCC('I','N','T','Z')

/* D3DRESOURCETYPE */
#define D3DRTYPE_SURFACE                    1u
#define D3DRTYPE_VOLUME                     2u
#define D3DRTYPE_TEXTURE                    3u
#define D3DRTYPE_VOLUMETEXTURE              4u
#define D3DRTYPE_CUBETEXTURE                5u
#define D3DRTYPE_VERTEXBUFFER               6u
#define D3DRTYPE_INDEXBUFFER                7u

/* D3DPOOL */
#define D3DPOOL_DEFAULT                     0u
#define D3DPOOL_MANAGED                     1u
#define D3DPOOL_SYSTEMMEM                   2u
#define D3DPOOL_SCRATCH                     3u

/* D3DUSAGE */
#define D3DUSAGE_RENDERTARGET               0x00000001u
#define D3DUSAGE_DEPTHSTENCIL               0x00000002u
#define D3DUSAGE_DYNAMIC                    0x00000200u
#define D3DUSAGE_AUTOGENMIPMAP              0x00000400u
#define D3DUSAGE_WRITEONLY                  0x00000008u
#define D3DUSAGE_DONOTCLIP                  0x00000020u
#define D3DUSAGE_POINTS                     0x00000040u
#define D3DUSAGE_RTPATCHES                  0x00000080u
#define D3DUSAGE_NPATCHES                   0x00000100u
#define D3DUSAGE_SOFTWAREPROCESSING         0x00000010u

/* D3DSWAPEFFECT */
#define D3DSWAPEFFECT_DISCARD               1u
#define D3DSWAPEFFECT_FLIP                  2u
#define D3DSWAPEFFECT_COPY                  3u

/* Clear flags */
#define D3DCLEAR_TARGET                     0x00000001u
#define D3DCLEAR_ZBUFFER                    0x00000002u
#define D3DCLEAR_STENCIL                    0x00000004u

/* Lock flags */
#define D3DLOCK_READONLY                    0x00000010u
#define D3DLOCK_DISCARD                     0x00002000u
#define D3DLOCK_NOOVERWRITE                 0x00001000u
#define D3DLOCK_NOSYSLOCK                   0x00000800u
#define D3DLOCK_DONOTWAIT                   0x00004000u

/* Primitive types */
#define D3DPT_POINTLIST                     1u
#define D3DPT_LINELIST                      2u
#define D3DPT_LINESTRIP                     3u
#define D3DPT_TRIANGLELIST                  4u
#define D3DPT_TRIANGLESTRIP                 5u
#define D3DPT_TRIANGLEFAN                   6u

/* Shader version macros */
#define D3DVS_VERSION(major, minor)         (0xFFFE0000u | ((major) << 8) | (minor))
#define D3DPS_VERSION(major, minor)         (0xFFFF0000u | ((major) << 8) | (minor))

/* D3DDECLTYPE */
#define D3DDECLTYPE_FLOAT1                  0u
#define D3DDECLTYPE_FLOAT2                  1u
#define D3DDECLTYPE_FLOAT3                  2u
#define D3DDECLTYPE_FLOAT4                  3u
#define D3DDECLTYPE_D3DCOLOR                4u
#define D3DDECLTYPE_UBYTE4                  5u
#define D3DDECLTYPE_SHORT2                  6u
#define D3DDECLTYPE_SHORT4                  7u
#define D3DDECLTYPE_UBYTE4N                 8u
#define D3DDECLTYPE_SHORT2N                 9u
#define D3DDECLTYPE_SHORT4N                 10u
#define D3DDECLTYPE_USHORT2N                11u
#define D3DDECLTYPE_USHORT4N                12u
#define D3DDECLTYPE_UDEC3                   13u
#define D3DDECLTYPE_DEC3N                   14u
#define D3DDECLTYPE_FLOAT16_2               15u
#define D3DDECLTYPE_FLOAT16_4               16u
#define D3DDECLTYPE_UNUSED                  17u

/* D3DDECLUSAGE */
#define D3DDECLUSAGE_POSITION               0u
#define D3DDECLUSAGE_BLENDWEIGHT            1u
#define D3DDECLUSAGE_BLENDINDICES           2u
#define D3DDECLUSAGE_NORMAL                 3u
#define D3DDECLUSAGE_PSIZE                  4u
#define D3DDECLUSAGE_TEXCOORD               5u
#define D3DDECLUSAGE_TANGENT                6u
#define D3DDECLUSAGE_BINORMAL               7u
#define D3DDECLUSAGE_TESSFACTOR             8u
#define D3DDECLUSAGE_POSITIONT              9u
#define D3DDECLUSAGE_COLOR                  10u
#define D3DDECLUSAGE_FOG                    11u
#define D3DDECLUSAGE_DEPTH                  12u
#define D3DDECLUSAGE_SAMPLE                 13u

/* Structures */
#pragma pack(push, 4)

typedef struct {
    char            Driver[512];
    char            Description[512];
    char            DeviceName[32];
    uint32_t        DriverVersionLow;
    uint32_t        DriverVersionHigh;
    uint32_t        VendorId;
    uint32_t        DeviceId;
    uint32_t        SubSysId;
    uint32_t        Revision;
    uint8_t         DeviceIdentifier[16];
    uint32_t        WHQLLevel;
} D3DADAPTER_IDENTIFIER9;

typedef struct {
    uint32_t        Width;
    uint32_t        Height;
    uint32_t        RefreshRate;
    uint32_t        Format;
} D3DDISPLAYMODE;

typedef struct {
    uint32_t        BackBufferWidth;
    uint32_t        BackBufferHeight;
    uint32_t        BackBufferFormat;
    uint32_t        BackBufferCount;
    uint32_t        MultiSampleType;
    uint32_t        MultiSampleQuality;
    uint32_t        SwapEffect;
    uint32_t        hDeviceWindow;
    uint32_t        Windowed;
    uint32_t        EnableAutoDepthStencil;
    uint32_t        AutoDepthStencilFormat;
    uint32_t        Flags;
    uint32_t        FullScreen_RefreshRateInHz;
    uint32_t        PresentationInterval;
} D3DPRESENT_PARAMETERS;

typedef struct {
    uint32_t        X;
    uint32_t        Y;
    uint32_t        Width;
    uint32_t        Height;
    float           MinZ;
    float           MaxZ;
} D3DVIEWPORT9;

typedef struct {
    int32_t         x1;
    int32_t         y1;
    int32_t         x2;
    int32_t         y2;
} D3DRECT;

typedef struct {
    uint16_t        Stream;
    uint16_t        Offset;
    uint8_t         Type;
    uint8_t         Method;
    uint8_t         Usage;
    uint8_t         UsageIndex;
} D3DVERTEXELEMENT9;

typedef struct {
    uint32_t        Format;
    uint32_t        Type;
    uint32_t        Usage;
    uint32_t        Pool;
    uint32_t        Size;
    uint32_t        FVF;
} D3DVERTEXBUFFER_DESC;

typedef struct {
    uint32_t        Format;
    uint32_t        Type;
    uint32_t        Usage;
    uint32_t        Pool;
    uint32_t        Size;
} D3DINDEXBUFFER_DESC;

typedef struct {
    uint32_t        Format;
    uint32_t        Type;
    uint32_t        Usage;
    uint32_t        Pool;
    uint32_t        MultiSampleType;
    uint32_t        MultiSampleQuality;
    uint32_t        Width;
    uint32_t        Height;
} D3DSURFACE_DESC;

typedef struct {
    int32_t         Pitch;
    uint32_t        pBits;          /* guest address */
} D3DLOCKED_RECT;

typedef struct {
    uint32_t        Format;
    uint32_t        Type;
    uint32_t        Usage;
    uint32_t        Pool;
    uint32_t        Width;
    uint32_t        Height;
    uint32_t        Depth;
} D3DVOLUME_DESC;

typedef struct {
    int32_t         RowPitch;
    int32_t         SlicePitch;
    uint32_t        pBits;          /* guest address */
} D3DLOCKED_BOX;

typedef struct {
    uint32_t        Left;
    uint32_t        Top;
    uint32_t        Right;
    uint32_t        Bottom;
    uint32_t        Front;
    uint32_t        Back;
} D3DBOX;

typedef struct {
    float m[4][4];
} D3DMATRIX;

/* 304 bytes exact */
typedef struct {
    uint32_t        DeviceType;
    uint32_t        AdapterOrdinal;
    uint32_t        Caps;
    uint32_t        Caps2;
    uint32_t        Caps3;
    uint32_t        PresentationIntervals;
    uint32_t        CursorCaps;
    uint32_t        DevCaps;
    uint32_t        PrimitiveMiscCaps;
    uint32_t        RasterCaps;
    uint32_t        ZCmpCaps;
    uint32_t        SrcBlendCaps;
    uint32_t        DestBlendCaps;
    uint32_t        AlphaCmpCaps;
    uint32_t        ShadeCaps;
    uint32_t        TextureCaps;
    uint32_t        TextureFilterCaps;
    uint32_t        CubeTextureFilterCaps;
    uint32_t        VolumeTextureFilterCaps;
    uint32_t        TextureAddressCaps;
    uint32_t        VolumeTextureAddressCaps;
    uint32_t        LineCaps;
    uint32_t        MaxTextureWidth;
    uint32_t        MaxTextureHeight;
    uint32_t        MaxVolumeExtent;
    uint32_t        MaxTextureRepeat;
    uint32_t        MaxTextureAspectRatio;
    uint32_t        MaxAnisotropy;
    float           MaxVertexW;
    float           GuardBandLeft;
    float           GuardBandTop;
    float           GuardBandRight;
    float           GuardBandBottom;
    float           ExtentsAdjust;
    uint32_t        StencilCaps;
    uint32_t        FVFCaps;
    uint32_t        TextureOpCaps;
    uint32_t        MaxTextureBlendStages;
    uint32_t        MaxSimultaneousTextures;
    uint32_t        VertexProcessingCaps;
    uint32_t        MaxActiveLights;
    uint32_t        MaxUserClipPlanes;
    uint32_t        MaxVertexBlendMatrices;
    uint32_t        MaxVertexBlendMatrixIndex;
    float           MaxPointSize;
    uint32_t        MaxPrimitiveCount;
    uint32_t        MaxVertexIndex;
    uint32_t        MaxStreams;
    uint32_t        MaxStreamStride;
    uint32_t        VertexShaderVersion;
    uint32_t        MaxVertexShaderConst;
    uint32_t        PixelShaderVersion;
    float           PixelShader1xMaxValue;
    uint32_t        DevCaps2;
    float           MaxNpatchTessellationLevel;
    uint32_t        Reserved5;
    uint32_t        MasterAdapterOrdinal;
    uint32_t        AdapterOrdinalInGroup;
    uint32_t        NumberOfAdaptersInGroup;
    uint32_t        DeclTypes;
    uint32_t        NumSimultaneousRTs;
    uint32_t        StretchRectFilterCaps;
    uint32_t        VS20Caps_Caps;
    int32_t         VS20Caps_DynamicFlowControlDepth;
    int32_t         VS20Caps_NumTemps;
    int32_t         VS20Caps_StaticFlowControlDepth;
    uint32_t        PS20Caps_Caps;
    int32_t         PS20Caps_DynamicFlowControlDepth;
    int32_t         PS20Caps_NumTemps;
    int32_t         PS20Caps_StaticFlowControlDepth;
    int32_t         PS20Caps_NumInstructionSlots;
    uint32_t        VertexTextureFilterCaps;
    uint32_t        MaxVShaderInstructionsExecuted;
    uint32_t        MaxPShaderInstructionsExecuted;
    uint32_t        MaxVertexShader30InstructionSlots;
    uint32_t        MaxPixelShader30InstructionSlots;
} D3DCAPS9;

#pragma pack(pop)

/* GPU backend operations */
typedef struct {
    const char *name;
    int  (*is_active)(void);
    int  (*enable)(void);
    int  (*init_device)(uint32_t width, uint32_t height);
    void (*shutdown_device)(void);
    int  (*begin_scene)(void);
    int  (*end_scene)(void);
    int  (*clear)(uint32_t flags, uint32_t color, float z, uint32_t stencil);
    int  (*present)(void);
    void *(*create_texture)(uint32_t width, uint32_t height, uint32_t format, int is_cube, uint32_t levels);
    void  (*destroy_texture)(void *tex);
    void  (*upload_texture)(void *tex, int face, int level, const void *bytes, uint32_t bytes_per_row);
    void *(*create_buffer)(uint32_t size, int is_index);
    void  (*destroy_buffer)(void *buf);
    void  (*upload_buffer)(void *buf, uint32_t offset, const void *bytes, uint32_t size);
    void *(*create_pipeline)(const char *vs_msl, const char *ps_msl);
    void  (*destroy_pipeline)(void *pipe);
    int  (*draw)(int prim_type, uint32_t start_vert, uint32_t vert_count, void *pipe, void *vb, uint32_t stride);
    /* Render targets and stage-0 texture binding (needed for real pixel tests). */
    void  (*set_render_target)(void *rt);
    void *(*create_render_target)(uint32_t width, uint32_t height);
    void  (*destroy_render_target)(void *rt);
    void  (*set_texture)(void *tex);
    int   (*read_render_target)(void *rt, uint32_t x, uint32_t y, uint32_t w, uint32_t h, void *out, uint32_t out_bytes_per_row);
    int   (*copy_texture_rect)(void *src_tex, uint32_t sx, uint32_t sy, void *dst_tex, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h);
    /* Indexed drawing and the render states that change Metal pipeline state. */
    int   (*draw_indexed)(int prim_type, uint32_t base_vertex, uint32_t start_index,
                          uint32_t index_count, void *pipe, void *vb, void *ib, uint32_t stride);
    void  (*set_render_state)(uint32_t blend_enable, uint32_t src_blend, uint32_t dst_blend,
                              uint32_t z_enable, uint32_t z_write, uint32_t z_func,
                              uint32_t cull_mode, uint32_t scissor_enable, const int32_t scissor[4]);
    /* Shader resources: vertex declaration, F/I/B constants and samplers. */
    void  (*set_vertex_decl)(const D3DVERTEXELEMENT9 *elements, uint32_t count);
    void  (*set_shader_constants)(const float *vsf, const int32_t *vsi, const uint32_t *vsb,
                                  const float *psf, const int32_t *psi, const uint32_t *psb);
    void  (*set_sampler)(uint32_t stage, void *tex);
    void  (*set_ffp_uniforms)(const void *uniforms, uint32_t size);
    void  (*set_clip_planes)(const float planes[6][4], uint32_t mask);
    int   (*draw_indexed32)(int prim_type, uint32_t base_vertex, uint32_t start_index,
                            uint32_t index_count, void *pipe, void *vb, void *ib, uint32_t stride);
    void  (*set_gamma_ramp)(const uint16_t ramp[256][3]);
    void  (*set_stream_divisor)(uint32_t stream, uint32_t divisor);
    int   (*draw_instanced)(int prim_type, uint32_t start_vert, uint32_t vert_count,
                            uint32_t instance_count, void *pipe, void *vb, uint32_t stride);
    int   (*draw_indexed_instanced)(int prim_type, uint32_t base_vertex, uint32_t start_index,
                                    uint32_t index_count, uint32_t instance_count,
                                    void *pipe, void *vb, void *ib, uint32_t stride, int index32,
                                    uint32_t inst_stream, void *inst_vb, uint32_t inst_stride);
    void *(*create_volume_texture)(uint32_t width, uint32_t height, uint32_t depth, uint32_t levels, uint32_t format);
    void  (*upload_volume_texture)(void *tex, int level, const void *bytes, uint32_t bytes_per_row, uint32_t bytes_per_image);
    /* IDirect3DQuery9. Only EVENT and OCCLUSION are supported by the real
     * backend; all other public query types are rejected before these are
     * ever called. get_data returns 0 = S_OK, 1 = S_FALSE, -1 = error. */
    void *(*query_create)(uint32_t type);
    int   (*query_issue)(void *query, uint32_t flags);
    int   (*query_get_data)(void *query, void *out, uint32_t size, uint32_t flags);
    void  (*query_destroy)(void *query);
} d3d9_gpu_ops_t;

extern const d3d9_gpu_ops_t *hle_d3d9_gpu;
void hle_d3d9_set_gpu(const d3d9_gpu_ops_t *ops);

/* Shim table declaration (for host / selftest registration) */
extern const win32hle_shim win32hle_d3d9[];

/* Backend Metal enable (macOS) */
int hle_d3d9_metal_enable(void);

#endif /* WIN32HLE_D3D9_HLE_H */
