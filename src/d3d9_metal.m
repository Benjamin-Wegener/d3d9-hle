/*
 * d3d9_metal.m - Direct3D 9 Metal GPU backend for macOS.
 *
 * Implements the hardware rendering backend on Apple Metal:
 * device, command queue, swapchain, render passes, texture/buffer uploads,
 * pipeline state compilation from MSL, and drawing.
 *
 * Built without ARC (explicit retain/release).
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "d3d9_metal.h"
#include "d3d9_hle.h"

static int g_metal_active = 0;
static id<MTLDevice> g_dev = nil;
static id<MTLCommandQueue> g_cmd_queue = nil;
static id<MTLCommandBuffer> g_curr_cmd_buf = nil;
static id<MTLRenderCommandEncoder> g_curr_encoder = nil;
static void flush_gpu(void);

/* Occlusion queries: one shared MTLBuffer bound as every render pass's
 * visibilityResultBuffer. Each query owns an 8-byte slot; Metal writes a
 * uint64 count of depth/stencil-passing samples there while counting is on.
 * 8-byte alignment is required by setVisibilityResultMode:offset:. */
#define VIS_SLOT_SIZE 8u
#define VIS_BUF_SIZE  (64u * 1024u)
static id<MTLBuffer> g_vis_buf = nil;
static uint32_t g_vis_next_offset = 0;
static uint32_t g_vis_free[VIS_BUF_SIZE / VIS_SLOT_SIZE];
static uint32_t g_vis_free_n = 0;

static void apply_visibility(MTLRenderPassDescriptor *rpd) {
    if (g_vis_buf) rpd.visibilityResultBuffer = g_vis_buf;
}

/* Default offscreen render target and depth-stencil */
static id<MTLTexture> g_rt_tex = nil;
static id<MTLTexture> msaa_resolved_texture(void);
static uint32_t g_rt_samples = 1;
static id<MTLTexture> g_msaa_ds_tex = nil;
static int g_msaa_needs_resolve = 0;
static int g_srgb_write = 0;
static id<MTLTexture> g_rt_srgb_view = nil;
static id<MTLTexture> g_ds_tex = nil;
static id<MTLTexture> g_owned_rt = nil;        /* backbuffer texture created by init_device */

/* An MSAA render target owns a sampleCount>1 colour texture and a 1-sample
 * resolve texture; the public D3D9 surface reads the resolved one. */
typedef struct {
    id<MTLTexture> colour, resolved;
    uint32_t w, h, samples;
} msaa_rt_t;
static msaa_rt_t *g_msaa_rt = NULL;
static id<MTLTexture> current_ds_texture(void) { return g_msaa_ds_tex ? g_msaa_ds_tex : g_ds_tex; }
/* Set the render pass to resolve the MSAA attachment into its single-sample
 * texture when the pass ends. This is the portable Metal resolve path. */
static id<MTLTexture> current_rt_view(void) {
    return (g_srgb_write && g_rt_srgb_view) ? g_rt_srgb_view : g_rt_tex;
}
static void rebuild_rt_srgb_view(void) {
    if (g_rt_srgb_view) { [g_rt_srgb_view release]; g_rt_srgb_view = nil; }
    if (g_srgb_write && g_rt_tex && !g_msaa_rt) {
        g_rt_srgb_view = [[g_rt_tex newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm_sRGB] retain];
        if (!g_rt_srgb_view) fprintf(stderr, "[d3d9_metal] sRGB render target view failed\n");
    }
}
void hle_d3d9_metal_set_srgb_write(int enabled) {
    g_srgb_write = enabled ? 1 : 0;
    rebuild_rt_srgb_view();
}
static void apply_msaa_resolve(MTLRenderPassDescriptor *rpd) {
    if (!g_msaa_rt) return;
    rpd.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
    rpd.colorAttachments[0].resolveTexture = g_msaa_rt->resolved;
}
static id<MTLTexture> g_bound_texture = nil;   /* sampler slot 0 */
static id<MTLTexture> g_samplers[16];           /* sampler slots 0..15 */
static id<MTLTexture> g_samplers_srgb[16];      /* sRGB views of the same textures */
static id<MTLSamplerState> g_sampler = nil;
static uint32_t g_samp_min = 1; /* D3DTEXF_POINT */
static uint32_t g_samp_mag = 1;
static uint32_t g_samp_mip = 0; /* D3DTEXF_NONE */
static uint32_t g_samp_aniso = 1;             /* D3DSAMP_MAXANISOTROPY */
static int g_samp_srgb = 0;                   /* D3DSAMP_SRGBTEXTURE */
static uint32_t g_samp_addr_u = 3; /* D3DTADDRESS_CLAMP */
static uint32_t g_samp_addr_v = 3;
static uint32_t g_samp_addr_w = 3;
static uint32_t g_samp_border = 0;            /* D3DCOLOR, only logged */

/* Shader constants uploaded to [[buffer(1..3)]] for both shader stages. */
static float g_vs_const_f[256][4], g_ps_const_f[256][4];
static int32_t g_vs_const_i[16][4], g_ps_const_i[16][4];
static uint32_t g_vs_const_b[16][4], g_ps_const_b[16][4];  /* bool4: x = value */
/* Vertex declaration of the current draw (when a shader is bound). */
static D3DVERTEXELEMENT9 g_decl[64];
static uint32_t g_decl_count;
static uint32_t g_decl_hash;
static uint32_t g_rt_width = 0;
static uint32_t g_rt_height = 0;

/* FFP uniforms buffer */
static uint8_t g_ffp_uniforms[256];
static uint32_t g_ffp_uniforms_size = 0;

/* User clip planes.  Rendered by the generated MSL, not by any render-pass
 * property: bit i of g_clip_mask enables g_clip_planes[i] (a,b,c,d). */
static float g_clip_planes[6][4];
static uint32_t g_clip_mask;
static uint8_t g_clip_uniforms[16 + 6 * 16];  /* uint mask + 6 float4, buffer(4) */

/* Indexed instancing: per-stream vertex layouts and the instance count. */
typedef struct {
    void   *buf;
    uint32_t stride;
    uint32_t offset;
    uint32_t divisor;   /* 0 = per vertex, 1 = per instance */
} stream_layout_t;
static stream_layout_t g_streams[16];
static uint32_t g_instanced_stream = 0xFFFFFFFFu;

/* Gamma ramp for the implicit swap chain.  Applied by a real Metal pass at
 * Present, writing a separate texture; the display gamma is never changed. */
static id<MTLTexture> g_gamma_lut = nil;
static id<MTLTexture> g_present_tex = nil;
static id<MTLRenderPipelineState> g_gamma_pipe = nil;
static id<MTLCommandBuffer> g_present_cb = nil;
static int g_gamma_active = 0;

/* Default shaders */
static const char *s_default_shader =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct D3D9Clip { uint4 mask; float4 planes[6]; };\n"
"struct VS_IN {\n"
"    float4 pos [[attribute(0)]];\n"
"    float4 color [[attribute(1)]];\n"
"    float2 uv [[attribute(2)]];\n"
"};\n"
"struct VS_OUT {\n"
"    float4 pos [[position]];\n"
"    float4 color;\n"
"    float2 uv;\n"
"    float4 clip_pos;\n"
"};\n"
"vertex VS_OUT vs_main(VS_IN in [[stage_in]], constant D3D9Clip& clip [[buffer(4)]]) {\n"
"    VS_OUT out;\n"
"    out.pos = in.pos;\n"
"    out.color = in.color;\n"
"    out.uv = in.uv;\n"
"    out.clip_pos = in.pos;\n"
"    return out;\n"
"}\n"
"fragment float4 ps_main(VS_OUT in [[stage_in]],\n"
"                        constant D3D9Clip& clip [[buffer(4)]],\n"
"                        texture2d<float> tx [[texture(0)]],\n"
"                        sampler smp [[sampler(0)]]) {\n"
"    for (uint i = 0u; i < 6u; ++i)\n"
"        if ((clip.mask.x & (1u << i)) != 0u && dot(clip.planes[i], in.clip_pos) < 0.0f)\n"
"            discard_fragment();\n"
"    return in.color;\n"
"}\n";

/* Same input layout, but the fragment stage samples the bound texture. */
static const char *s_textured_ps =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct D3D9Clip { uint4 mask; float4 planes[6]; };\n"
"struct VS_OUT {\n"
"    float4 pos [[position]];\n"
"    float4 color;\n"
"    float2 uv;\n"
"    float4 clip_pos;\n"
"};\n"
"fragment float4 ps_tex(VS_OUT in [[stage_in]],\n"
"                       constant D3D9Clip& clip [[buffer(4)]],\n"
"                       texture2d<float> tx [[texture(0)]],\n"
"                       sampler smp [[sampler(0)]]) {\n"
"    for (uint i = 0u; i < 6u; ++i)\n"
"        if ((clip.mask.x & (1u << i)) != 0u && dot(clip.planes[i], in.clip_pos) < 0.0f)\n"
"            discard_fragment();\n"
"    return tx.sample(smp, in.uv);\n"
"}\n";

/* Render states that affect a Metal pipeline, kept by the backend between
 * draws. Scissor is an encoder property and applied separately. */
static uint32_t g_blend_enable = 0, g_src_blend = 5, g_dst_blend = 6;
static uint32_t g_z_enable = 0, g_z_write = 0, g_z_func = 4;
static uint32_t g_cull_mode = 0, g_scissor_enable = 0;
static int32_t g_scissor[4] = { 0, 0, 0, 0 };

typedef struct {
    char *vs, *ps;               /* MSL sources */
    const char *vs_fn, *ps_fn;   /* entry point names */
    int translated;              /* translator output: 16 float4 attributes */
    uint32_t keys[16];
    id<MTLRenderPipelineState> pipes[16];
    int n;
} pipe_bundle_t;

static pipe_bundle_t *g_default_bundle = NULL, *g_textured_bundle = NULL;
static pipe_bundle_t *g_pipeline_cache[64];
static int g_pipeline_n;
static id<MTLDepthStencilState> g_depth_states[16];
static uint32_t g_depth_keys[16];
static int g_depth_n = 0;

static uint32_t state_key(void) {
    return (g_blend_enable ? 1u : 0u) | ((g_src_blend & 0xFu) << 1) |
           ((g_dst_blend & 0xFu) << 5) | ((g_z_enable ? 1u : 0u) << 9) |
           ((g_z_write ? 1u : 0u) << 10) | ((g_z_func & 0xFu) << 11) |
           ((g_cull_mode & 3u) << 15) | (g_decl_hash << 17);
}

static MTLBlendFactor blend_factor(uint32_t d3d) {
    switch (d3d) {
    case 1:  return MTLBlendFactorZero;
    case 2:  return MTLBlendFactorOne;
    case 3:  return MTLBlendFactorSourceColor;
    case 4:  return MTLBlendFactorOneMinusSourceColor;
    case 5:  return MTLBlendFactorSourceAlpha;
    case 6:  return MTLBlendFactorOneMinusSourceAlpha;
    case 7:  return MTLBlendFactorDestinationAlpha;
    case 8:  return MTLBlendFactorOneMinusDestinationAlpha;
    case 9:  return MTLBlendFactorDestinationColor;
    case 10: return MTLBlendFactorOneMinusDestinationColor;
    default: return MTLBlendFactorOne;
    }
}

static MTLCompareFunction depth_compare(uint32_t d3d) {
    switch (d3d) {
    case 1:  return MTLCompareFunctionNever;
    case 2:  return MTLCompareFunctionLess;
    case 3:  return MTLCompareFunctionEqual;
    case 4:  return MTLCompareFunctionLessEqual;
    case 5:  return MTLCompareFunctionGreater;
    case 6:  return MTLCompareFunctionNotEqual;
    case 7:  return MTLCompareFunctionGreaterEqual;
    default: return MTLCompareFunctionAlways;
    }
}

static MTLCullMode cull_mode(uint32_t d3d) {
    if (d3d == 1) return MTLCullModeFront;   /* D3DCULL_CW */
    if (d3d == 2) return MTLCullModeBack;    /* D3DCULL_CCW */
    return MTLCullModeNone;
}

static id<MTLDepthStencilState> current_depth_state(void) {
    uint32_t key = g_z_enable ? (g_z_func | (g_z_write ? 0x100u : 0)) : 0;
    for (int i = 0; i < g_depth_n; i++)
        if (g_depth_keys[i] == key) return g_depth_states[i];
    if (g_depth_n >= 16) return nil;
    MTLDepthStencilDescriptor *d = [[MTLDepthStencilDescriptor alloc] init];
    d.depthCompareFunction = g_z_enable ? depth_compare(g_z_func) : MTLCompareFunctionAlways;
    d.depthWriteEnabled = g_z_enable && g_z_write ? YES : NO;
    id<MTLDepthStencilState> st = [g_dev newDepthStencilStateWithDescriptor:d];
    [d release];
    g_depth_keys[g_depth_n] = key;
    g_depth_states[g_depth_n] = st;
    g_depth_n++;
    return st;
}

static void apply_scissor(void) {
    if (!g_curr_encoder) return;
    if (g_scissor_enable) {
        int32_t x = g_scissor[0], y = g_scissor[1];
        int32_t w = g_scissor[2] - x, h = g_scissor[3] - y;
        if (w < 0) w = 0;
        if (h < 0) h = 0;
        [g_curr_encoder setScissorRect:(MTLScissorRect){(NSUInteger)x, (NSUInteger)y,
                                                        (NSUInteger)w, (NSUInteger)h}];
    } else {
        [g_curr_encoder setScissorRect:(MTLScissorRect){0, 0, g_rt_width, g_rt_height}];
    }
}

void hle_d3d9_metal_set_render_state(uint32_t blend_enable, uint32_t src_blend,
                                     uint32_t dst_blend, uint32_t z_enable,
                                     uint32_t z_write, uint32_t z_func,
                                     uint32_t cull_mode, uint32_t scissor_enable,
                                     const int32_t scissor[4]) {
    g_blend_enable = blend_enable;
    g_src_blend = src_blend;
    g_dst_blend = dst_blend;
    g_z_enable = z_enable;
    g_z_write = z_write;
    g_z_func = z_func;
    g_cull_mode = cull_mode;
    g_scissor_enable = scissor_enable;
    if (scissor) memcpy(g_scissor, scissor, sizeof g_scissor);
    apply_scissor();
}

static MTLVertexFormat d3d_decl_format(uint8_t type) {
    switch (type) {
    case 0:  return MTLVertexFormatFloat;       /* FLOAT1 */
    case 1:  return MTLVertexFormatFloat2;      /* FLOAT2 */
    case 2:  return MTLVertexFormatFloat3;      /* FLOAT3 */
    case 3:  return MTLVertexFormatFloat4;      /* FLOAT4 */
    case 4:  return MTLVertexFormatUChar4Normalized_BGRA; /* D3DCOLOR: ARGB in memory -> BGRA */
    case 5:  return MTLVertexFormatUChar4;      /* UBYTE4 */
    case 6:  return MTLVertexFormatShort2;      /* SHORT2 */
    case 7:  return MTLVertexFormatShort4;      /* SHORT4 */
    case 8:  return MTLVertexFormatUChar4Normalized; /* UBYTE4N */
    case 9:  return MTLVertexFormatShort2Normalized; /* SHORT2N */
    case 10: return MTLVertexFormatShort4Normalized; /* SHORT4N */
    case 11: return MTLVertexFormatUShort2Normalized; /* USHORT2N */
    case 12: return MTLVertexFormatUShort4Normalized; /* USHORT4N */
    case 13: return MTLVertexFormatUInt1010102Normalized; /* UDEC3 (approx) */
    case 14: return MTLVertexFormatInt1010102Normalized;  /* DEC3N (approx) */
    case 15: return MTLVertexFormatHalf2;       /* FLOAT16_2 */
    case 16: return MTLVertexFormatHalf4;       /* FLOAT16_4 */
    default: return MTLVertexFormatFloat4;
    }
}

/* Interleaved D3D9-style vertices as the standalone tests upload them:
 * float4 position, float4 diffuse colour, float2 texture coordinates. */
static MTLVertexDescriptor *make_vertex_descriptor(int translated) {
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
    if (translated == 2) {
        /* FFP pipeline: layout matches g_decl exactly with dense stride */
        uint32_t stride = 0;
        for (uint32_t k = 0; k < g_decl_count; k++) {
            const D3DVERTEXELEMENT9 *e = &g_decl[k];
            if (e->Stream == 0xFF) break;
            if (e->Stream != 0) continue;
            vd.attributes[k].format = d3d_decl_format(e->Type);
            vd.attributes[k].offset = e->Offset;
            vd.attributes[k].bufferIndex = 0;
            uint32_t sz = 16;
            switch (e->Type) {
            case 0: sz = 4; break;
            case 1: sz = 8; break;
            case 2: sz = 12; break;
            case 3: sz = 16; break;
            case 4: sz = 4; break;
            default: sz = 16; break;
            }
            if (e->Offset + sz > stride) stride = e->Offset + sz;
        }
        vd.layouts[0].stride = stride ? stride : 32;
        return vd;
    }
    if (translated) {
        /* The translator declares v0..v15 as float4 attributes at indices 0..15.
         * The D3D9 vertex declaration maps stream elements onto those slots;
         * when present, use its offsets/formats so the declaration really
         * drives the layout. Stride stays 256 (16 float4) as the translator
         * ABI unless a decl element requires a larger span.
         *
         * Indexed instancing (public SetStreamSourceFreq + D3DSTREAMSOURCE_*)
         * keeps geometry in stream 0 and moves the per-instance attributes to
         * a stream > 0. Those attributes come from a separate Metal buffer
         * with a per-instance step function so one draw covers every copy. */
        uint32_t stride = 256;
        for (uint32_t i = 0; i < 16; i++) {
            vd.attributes[i].format = MTLVertexFormatFloat4;
            vd.attributes[i].offset = i * 16;
            vd.attributes[i].bufferIndex = 0;
        }
        uint32_t inst_stride = 0;
        if (g_decl_count > 0) {
            for (uint32_t k = 0; k < g_decl_count; k++) {
                const D3DVERTEXELEMENT9 *e = &g_decl[k];
                if (e->Stream == 0xFF) break;
                uint32_t idx = k; /* decl order -> vN/attribute N */
                if (idx >= 16) continue;
                vd.attributes[idx].format = d3d_decl_format(e->Type);
                vd.attributes[idx].offset = e->Offset;
                uint32_t end = e->Offset + 16u; /* conservative span */
                if (e->Stream > 0) {
                    vd.attributes[idx].bufferIndex = 5;
                    if (end > inst_stride) inst_stride = end;
                } else {
                    vd.attributes[idx].bufferIndex = 0;
                    if (end > stride) stride = end;
                }
            }
        }
        vd.layouts[0].stride = stride;
        if (inst_stride) {
            vd.layouts[5].stepFunction = MTLVertexStepFunctionPerInstance;
            vd.layouts[5].stepRate = 1;
            vd.layouts[5].stride = inst_stride;
        }
        return vd;
    }
    vd.layouts[0].stride = 40;
    vd.attributes[0].format = MTLVertexFormatFloat4;
    vd.attributes[0].offset = 0;
    vd.attributes[0].bufferIndex = 0;
    vd.attributes[1].format = MTLVertexFormatFloat4;
    vd.attributes[1].offset = 16;
    vd.attributes[1].bufferIndex = 0;
    vd.attributes[2].format = MTLVertexFormatFloat2;
    vd.attributes[2].offset = 32;
    vd.attributes[2].bufferIndex = 0;
    return vd;
}

static id<MTLRenderPipelineState> make_pipeline_functions(id<MTLFunction> vs, id<MTLFunction> ps,
                                                          int translated) {
    MTLRenderPipelineDescriptor *pdesc = [[MTLRenderPipelineDescriptor alloc] init];
    pdesc.vertexFunction = vs;
    pdesc.fragmentFunction = ps;
    pdesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pdesc.sampleCount = g_rt_samples;   /* MSAA pipelines must match the RT */
    pdesc.vertexDescriptor = make_vertex_descriptor(translated);
    pdesc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    if (g_blend_enable) {
        MTLRenderPipelineColorAttachmentDescriptor *ca = pdesc.colorAttachments[0];
        ca.blendingEnabled = YES;
        ca.sourceRGBBlendFactor = blend_factor(g_src_blend);
        ca.destinationRGBBlendFactor = blend_factor(g_dst_blend);
        ca.sourceAlphaBlendFactor = blend_factor(g_src_blend);
        ca.destinationAlphaBlendFactor = blend_factor(g_dst_blend);
    }
    NSError *err = nil;
    id<MTLRenderPipelineState> pipe = [g_dev newRenderPipelineStateWithDescriptor:pdesc error:&err];
    if (!pipe) {
        fprintf(stderr, "[d3d9_metal] Pipeline create error: %s\n",
                err ? err.localizedDescription.UTF8String : "unknown");
    }
    [pdesc release];
    return pipe;
}

static id<MTLRenderPipelineState> make_pipeline_msl(pipe_bundle_t *b) {
    @autoreleasepool {
        NSError *err = nil;
        id<MTLLibrary> vlib = [g_dev newLibraryWithSource:[NSString stringWithUTF8String:b->vs]
                                                  options:nil error:&err];
        id<MTLLibrary> plib = [g_dev newLibraryWithSource:[NSString stringWithUTF8String:b->ps]
                                                  options:nil error:&err];
        id<MTLFunction> vs = vlib ? [vlib newFunctionWithName:[NSString stringWithUTF8String:b->vs_fn]] : nil;
        id<MTLFunction> ps = plib ? [plib newFunctionWithName:[NSString stringWithUTF8String:b->ps_fn]] : nil;
        id<MTLRenderPipelineState> pipe = nil;
        if (vs && ps) pipe = make_pipeline_functions(vs, ps, b->translated);
        if (vs) [vs release];
        if (ps) [ps release];
        if (vlib) [vlib release];
        if (plib) [plib release];
        return pipe;
    }
}

static pipe_bundle_t *make_bundle(const char *vs_msl, const char *ps_msl,
                                 const char *vs_fn, const char *ps_fn, int translated) {
    pipe_bundle_t *b = (pipe_bundle_t *)calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->vs = vs_msl ? strdup(vs_msl) : NULL;
    b->ps = ps_msl ? strdup(ps_msl) : NULL;
    b->vs_fn = vs_fn;
    b->ps_fn = ps_fn;
    b->translated = translated;
    return b;
}

static void destroy_bundle(pipe_bundle_t *b) {
    if (!b) return;
    for (int i = 0; i < b->n; i++) if (b->pipes[i]) [b->pipes[i] release];
    free(b->vs); free(b->ps); free(b);
}

/* A pipeline captures blend/cull/depth settings, so keep one variant per
 * render-state signature instead of silently reusing the wrong pipeline. */
static id<MTLRenderPipelineState> bundle_pipeline(pipe_bundle_t *b) {
    if (!b || !b->vs || !b->ps) return nil;
    uint32_t key = state_key();
    for (int i = 0; i < b->n; i++) if (b->keys[i] == key) return b->pipes[i];
    if (b->n >= 16) return nil;
    id<MTLRenderPipelineState> p = make_pipeline_msl(b);
    if (!p) return nil;
    b->keys[b->n] = key;
    b->pipes[b->n] = [p retain];
    b->n++;
    return p;
}

static const d3d9_gpu_ops_t s_ops = {
    "metal",
    hle_d3d9_metal_is_active,
    hle_d3d9_metal_enable,
    hle_d3d9_metal_init_device,
    hle_d3d9_metal_shutdown_device,
    hle_d3d9_metal_begin_scene,
    hle_d3d9_metal_end_scene,
    hle_d3d9_metal_clear,
    hle_d3d9_metal_present,
    hle_d3d9_metal_create_texture,
    hle_d3d9_metal_destroy_texture,
    hle_d3d9_metal_upload_texture,
    hle_d3d9_metal_create_buffer,
    hle_d3d9_metal_destroy_buffer,
    hle_d3d9_metal_upload_buffer,
    hle_d3d9_metal_create_pipeline,
    hle_d3d9_metal_destroy_pipeline,
    hle_d3d9_metal_draw,
    hle_d3d9_metal_set_render_target,
    hle_d3d9_metal_create_render_target,
    hle_d3d9_metal_destroy_render_target,
    hle_d3d9_metal_set_texture,
    hle_d3d9_metal_read_render_target,
    hle_d3d9_metal_copy_texture_rect,
    hle_d3d9_metal_draw_indexed,
    hle_d3d9_metal_set_render_state,
    hle_d3d9_metal_set_vertex_decl,
    hle_d3d9_metal_set_shader_constants,
    hle_d3d9_metal_set_sampler,
    hle_d3d9_metal_set_ffp_uniforms,
    hle_d3d9_metal_set_clip_planes,
    hle_d3d9_metal_draw_indexed32,
    hle_d3d9_metal_set_gamma_ramp,
    hle_d3d9_metal_set_stream_divisor,
    hle_d3d9_metal_draw_instanced,
    hle_d3d9_metal_draw_indexed_instanced,
    hle_d3d9_metal_create_volume_texture,
    hle_d3d9_metal_upload_volume_texture,
    hle_d3d9_metal_query_create,
    hle_d3d9_metal_query_issue,
    hle_d3d9_metal_query_get_data,
    hle_d3d9_metal_query_destroy
};

/* Register the backend at load time. Creating the Metal device stays lazy in
 * hle_d3d9_metal_init_device(), so a headless process can still load the HLE
 * and d3d9.c can size the render targets on CreateDevice. */
__attribute__((constructor)) static void d3d9_metal_register(void) {
    hle_d3d9_set_gpu(&s_ops);
}

int hle_d3d9_metal_is_active(void) {
    return g_metal_active;
}

uint64_t hle_d3d9_metal_working_set_bytes(void) {
    if (!g_metal_active) return 0;
    return (uint64_t)[g_dev recommendedMaxWorkingSetSize];
}

static MTLSamplerAddressMode d3d9_mtl_address(uint32_t mode);

int hle_d3d9_metal_enable(void) {
    if (g_metal_active) return 0;

    @autoreleasepool {
        g_dev = MTLCreateSystemDefaultDevice();
        if (!g_dev) {
            fprintf(stderr, "[d3d9_metal] No Metal device found\n");
            return -1;
        }

        g_cmd_queue = [g_dev newCommandQueue];
        if (!g_cmd_queue) {
            fprintf(stderr, "[d3d9_metal] Failed to create MTLCommandQueue\n");
            [g_dev release];
            g_dev = nil;
            return -1;
        }

        /* Built-in shader sources are kept as bundles so a pipeline variant
         * can be compiled for each render-state signature. */
        g_default_bundle = make_bundle(s_default_shader, s_default_shader, "vs_main", "ps_main", 0);
        g_textured_bundle = make_bundle(s_default_shader, s_textured_ps, "vs_main", "ps_tex", 0);

        MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
        sd.minFilter = MTLSamplerMinMagFilterNearest;
        sd.magFilter = MTLSamplerMinMagFilterNearest;
        sd.sAddressMode = d3d9_mtl_address(g_samp_addr_u);
        sd.tAddressMode = d3d9_mtl_address(g_samp_addr_v);
        sd.rAddressMode = d3d9_mtl_address(g_samp_addr_w);
        /* D3DTEXF_ANISOTROPIC on either filter selects anisotropic sampling;
         * Metal caps it at the device's maxAnisotropy. */
        if (g_samp_min == 3 || g_samp_mag == 3)
            sd.maxAnisotropy = g_samp_aniso > 16u ? 16u : g_samp_aniso;
        g_sampler = [g_dev newSamplerStateWithDescriptor:sd];
        [sd release];

        g_metal_active = 1;
        hle_d3d9_set_gpu(&s_ops);
        fprintf(stderr, "[d3d9_metal] Metal backend initialized on %s\n", g_dev.name.UTF8String);
    }
    return 0;
}

int hle_d3d9_metal_init_device(uint32_t width, uint32_t height) {
    if (!g_metal_active && hle_d3d9_metal_enable() != 0) return -1;

    @autoreleasepool {
        if (width == 0) width = D3D9_DEFAULT_WIDTH;
        if (height == 0) height = D3D9_DEFAULT_HEIGHT;

        if (g_rt_tex && g_rt_width == width && g_rt_height == height) {
            return 0;
        }

        /* g_rt_tex may be a texture borrowed from a render-target surface;
         * that surface owns it and releases it in surf9_Release. Only the
         * backbuffer created here may be released by the backend. */
        if (g_rt_tex == g_owned_rt) g_rt_tex = nil;
        if (g_owned_rt) { [g_owned_rt release]; g_owned_rt = nil; }
        if (g_ds_tex) { [g_ds_tex release]; g_ds_tex = nil; }

        g_rt_width = width;
        g_rt_height = height;

        /* Create render target texture */
        MTLTextureDescriptor *rt_desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                          width:width
                                                                                         height:height
                                                                                      mipmapped:NO];
        rt_desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        rt_desc.storageMode = MTLStorageModeShared;
        g_owned_rt = [g_dev newTextureWithDescriptor:rt_desc];  /* backend-owned */
        g_rt_tex = g_owned_rt;      /* until a surface RT is bound */

        /* Create depth-stencil texture */
        MTLTextureDescriptor *ds_desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                          width:width
                                                                                         height:height
                                                                                      mipmapped:NO];
        ds_desc.usage = MTLTextureUsageRenderTarget;
        ds_desc.storageMode = MTLStorageModePrivate;
        g_ds_tex = [g_dev newTextureWithDescriptor:ds_desc];

        /* One shared, CPU-visible visibility result buffer for all occlusion
         * queries. Created once; resizing the render target does not invalidate
         * query slots. */
        if (!g_vis_buf) {
            g_vis_buf = [g_dev newBufferWithLength:VIS_BUF_SIZE
                                           options:MTLResourceStorageModeShared];
            if (g_vis_buf) memset([g_vis_buf contents], 0, VIS_BUF_SIZE);
        }
    }
    return (g_rt_tex != nil) ? 0 : -1;
}

void hle_d3d9_metal_shutdown_device(void) {
    @autoreleasepool {
        if (g_curr_encoder) { [g_curr_encoder endEncoding]; [g_curr_encoder release]; g_curr_encoder = nil; }
        if (g_curr_cmd_buf) { [g_curr_cmd_buf commit]; [g_curr_cmd_buf release]; g_curr_cmd_buf = nil; }
        /* g_rt_tex may be a borrowed surface texture; only the backend-owned
         * backbuffer and the objects created here are released. */
        if (g_owned_rt) { [g_owned_rt release]; g_owned_rt = nil; }
        g_rt_tex = nil;
        if (g_bound_texture) { [g_bound_texture release]; g_bound_texture = nil; }
        if (g_ds_tex) { [g_ds_tex release]; g_ds_tex = nil; }
        if (g_vis_buf) { [g_vis_buf release]; g_vis_buf = nil; }
        if (g_gamma_lut) { [g_gamma_lut release]; g_gamma_lut = nil; }
        if (g_present_tex) { [g_present_tex release]; g_present_tex = nil; }
        if (g_gamma_pipe) { [g_gamma_pipe release]; g_gamma_pipe = nil; }
        if (g_present_cb) { [g_present_cb release]; g_present_cb = nil; }
        g_gamma_active = 0;
        g_vis_next_offset = 0;
        g_vis_free_n = 0;
        destroy_bundle(g_default_bundle); g_default_bundle = NULL;
        destroy_bundle(g_textured_bundle); g_textured_bundle = NULL;
        for (int i = 0; i < g_pipeline_n; i++) destroy_bundle(g_pipeline_cache[i]);
        g_pipeline_n = 0;
        for (int i = 0; i < g_depth_n; i++) if (g_depth_states[i]) [g_depth_states[i] release];
        g_depth_n = 0;
        if (g_sampler) { [g_sampler release]; g_sampler = nil; }
        if (g_cmd_queue) { [g_cmd_queue release]; g_cmd_queue = nil; }
        if (g_dev) { [g_dev release]; g_dev = nil; }
        g_metal_active = 0;
    }
}

int hle_d3d9_metal_clear(uint32_t flags, uint32_t color, float z, uint32_t stencil) {
    (void)stencil;
    if (!g_metal_active || !g_rt_tex) return 0;

    @autoreleasepool {
        if (g_curr_encoder) {
            [g_curr_encoder endEncoding];
            [g_curr_encoder release];       /* retained when stored in the global */
            g_curr_encoder = nil;
        }
        if (!g_curr_cmd_buf) {
            /* commandBuffer is autoreleased: keep it past this pool's drain. */
            g_curr_cmd_buf = [[g_cmd_queue commandBuffer] retain];
        }

        MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
        apply_visibility(rpd);
        if (flags & D3DCLEAR_TARGET) {
            rpd.colorAttachments[0].texture = current_rt_view();
            rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
            rpd.colorAttachments[0].storeAction = MTLStoreActionStore; apply_msaa_resolve(rpd);
            float a = ((color >> 24) & 0xFF) / 255.0f;
            float r = ((color >> 16) & 0xFF) / 255.0f;
            float g = ((color >> 8) & 0xFF) / 255.0f;
            float b = (color & 0xFF) / 255.0f;
            rpd.colorAttachments[0].clearColor = MTLClearColorMake(r, g, b, a);
        }

        if ((flags & D3DCLEAR_ZBUFFER) && g_ds_tex) {
            rpd.depthAttachment.texture = current_ds_texture();
            rpd.depthAttachment.loadAction = MTLLoadActionClear;
            rpd.depthAttachment.storeAction = MTLStoreActionDontCare;
            rpd.depthAttachment.clearDepth = (double)z;
        }

        id<MTLRenderCommandEncoder> enc = [g_curr_cmd_buf renderCommandEncoderWithDescriptor:rpd];
        [enc endEncoding];
    }
    return 0;
}

int hle_d3d9_metal_begin_scene(void) {
    if (!g_metal_active || !g_rt_tex) return 0;
    @autoreleasepool {
        if (!g_curr_cmd_buf) {
            g_curr_cmd_buf = [[g_cmd_queue commandBuffer] retain];
        }
        if (!g_curr_encoder) {
            MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
            rpd.colorAttachments[0].texture = current_rt_view();
            rpd.colorAttachments[0].loadAction = MTLLoadActionLoad;
            rpd.colorAttachments[0].storeAction = MTLStoreActionStore; apply_msaa_resolve(rpd);
            if (g_ds_tex) {
                rpd.depthAttachment.texture = current_ds_texture();
                rpd.depthAttachment.loadAction = MTLLoadActionLoad;
                rpd.depthAttachment.storeAction = MTLStoreActionStore;
            }
            apply_visibility(rpd);
            /* The encoder is autoreleased too: keep it across pools so
             * DrawPrimitive can use it after BeginScene returns. */
            g_curr_encoder = [[g_curr_cmd_buf renderCommandEncoderWithDescriptor:rpd] retain];
        }
    }
    return 0;
}

int hle_d3d9_metal_end_scene(void) {
    if (!g_metal_active) return 0;
    @autoreleasepool {
        if (g_curr_encoder) {
            [g_curr_encoder endEncoding];
            [g_curr_encoder release];
            g_curr_encoder = nil;
        }
    }
    return 0;
}

/* Present-time gamma pass.  The D3DGAMMARAMP entries are 16-bit values; a
 * 256x1 RGBA16Unorm texture stores them and the fragment shader remaps each
 * 8-bit channel through its own ramp.  Rendering into a separate texture
 * keeps GetRenderTargetData / surface reads linear, matching D3D9 semantics
 * (the ramp belongs to the swap chain, not to the render target). */
static const char *s_gamma_vs =
"#include <metal_stdlib>\nusing namespace metal;\n"
"struct GOut { float4 pos [[position]]; };\n"
"vertex GOut gamma_vs(uint vid [[vertex_id]]) {\n"
"  GOut o;\n"
"  float x = (vid == 1u) ? 3.0f : -1.0f;\n"
"  float y = (vid == 2u) ? 3.0f : -1.0f;\n"
"  o.pos = float4(x, y, 0.0f, 1.0f);\n"
"  return o;\n}\n";

static const char *s_gamma_fs =
"#include <metal_stdlib>\nusing namespace metal;\n"
"fragment float4 gamma_fs(float4 pos [[position]],\n"
"  texture2d<float, access::read> src [[texture(0)]],\n"
"  texture2d<float, access::read> lut [[texture(1)]]) {\n"
"  float4 c = src.read(uint2(pos.xy));\n"
"  uint ri = clamp(uint(round(c.r * 255.0f)), 0u, 255u);\n"
"  uint gi = clamp(uint(round(c.g * 255.0f)), 0u, 255u);\n"
"  uint bi = clamp(uint(round(c.b * 255.0f)), 0u, 255u);\n"
"  float4 r = lut.read(uint2(ri, 0u));\n"
"  float4 g = lut.read(uint2(gi, 0u));\n"
"  float4 b = lut.read(uint2(bi, 0u));\n"
"  return float4(r.r, g.g, b.b, c.a);\n}\n";

void hle_d3d9_metal_set_gamma_ramp(const uint16_t ramp[256][3]) {
    if (!g_metal_active || !ramp) return;
    @autoreleasepool {
        if (!g_gamma_lut) {
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Unorm
                                                                                        width:256 height:1 mipmapped:NO];
            d.usage = MTLTextureUsageShaderRead;
            d.storageMode = MTLStorageModeShared;
            g_gamma_lut = [g_dev newTextureWithDescriptor:d];
        }
        uint16_t pixels[256][4];
        for (unsigned i = 0; i < 256; i++) {
            pixels[i][0] = ramp[i][0];
            pixels[i][1] = ramp[i][1];
            pixels[i][2] = ramp[i][2];
            pixels[i][3] = 0xFFFFu;
        }
        [g_gamma_lut replaceRegion:MTLRegionMake2D(0, 0, 256, 1)
                       mipmapLevel:0 withBytes:pixels bytesPerRow:256 * sizeof(pixels[0])];
        g_gamma_active = 1;
    }
}

static id<MTLRenderPipelineState> gamma_pipeline(void) {
    if (g_gamma_pipe) return g_gamma_pipe;
    NSError *err = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:[NSString stringWithUTF8String:s_gamma_vs]
                                            options:nil error:&err];
    id<MTLFunction> vs = lib ? [lib newFunctionWithName:@"gamma_vs"] : nil;
    [lib release];
    lib = [g_dev newLibraryWithSource:[NSString stringWithUTF8String:s_gamma_fs]
                              options:nil error:&err];
    id<MTLFunction> fs = lib ? [lib newFunctionWithName:@"gamma_fs"] : nil;
    [lib release];
    if (!vs || !fs) {
        fprintf(stderr, "[d3d9_metal] gamma pipeline compile failed: %s\n",
                err ? err.localizedDescription.UTF8String : "unknown");
        [vs release]; [fs release];
        return nil;
    }
    MTLRenderPipelineDescriptor *pdesc = [[MTLRenderPipelineDescriptor alloc] init];
    pdesc.vertexFunction = vs;
    pdesc.fragmentFunction = fs;
    pdesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    g_gamma_pipe = [g_dev newRenderPipelineStateWithDescriptor:pdesc error:&err];
    [pdesc release];
    [vs release]; [fs release];
    return g_gamma_pipe;
}

int hle_d3d9_metal_read_present_pixels(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                       void *out, uint32_t out_bytes_per_row) {
    if (!g_metal_active || !g_present_tex || !out || !w || !h) return -1;
    @autoreleasepool {
        flush_gpu();
        if (g_present_cb) {
            [g_present_cb waitUntilCompleted];
            [g_present_cb release];
            g_present_cb = nil;
        }
        [g_present_tex getBytes:out bytesPerRow:out_bytes_per_row
                    fromRegion:MTLRegionMake2D(x, y, w, h) mipmapLevel:0];
    }
    return 0;
}

int hle_d3d9_metal_present(void) {
    if (!g_metal_active) return 0;
    @autoreleasepool {
        if (g_curr_encoder) {
            [g_curr_encoder endEncoding];
            [g_curr_encoder release];
            g_curr_encoder = nil;
        }
        if (g_curr_cmd_buf) {
            [g_curr_cmd_buf commit];
            [g_curr_cmd_buf release];
            g_curr_cmd_buf = nil;
        }
        if (g_gamma_active && g_rt_tex) {
            if (!g_present_tex || g_present_tex.width != g_rt_tex.width ||
                g_present_tex.height != g_rt_tex.height) {
                if (g_present_tex) { [g_present_tex release]; g_present_tex = nil; }
                MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                           width:g_rt_tex.width
                                                                                          height:g_rt_tex.height
                                                                                       mipmapped:NO];
                d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                d.storageMode = MTLStorageModeShared;
                g_present_tex = [g_dev newTextureWithDescriptor:d];
            }
            id<MTLRenderPipelineState> pipe = gamma_pipeline();
            if (g_present_tex && pipe && g_gamma_lut) {
                id<MTLCommandBuffer> cb = [g_cmd_queue commandBuffer];
                MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
                rpd.colorAttachments[0].texture = g_present_tex;
                rpd.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                rpd.colorAttachments[0].storeAction = MTLStoreActionStore; apply_msaa_resolve(rpd);
                id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rpd];
                [enc setRenderPipelineState:pipe];
                [enc setFragmentTexture:msaa_resolved_texture() atIndex:0];
                [enc setFragmentTexture:g_gamma_lut atIndex:1];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [enc endEncoding];
                if (g_present_cb) [g_present_cb release];
                g_present_cb = [cb retain];
                [cb commit];
            } else {
                fprintf(stderr, "[d3d9_metal] Present: gamma pass unavailable\n");
            }
        }
    }
    return 0;
}

static MTLPixelFormat d3d_to_mtl_format(uint32_t fmt) {
    switch (fmt) {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
        return MTLPixelFormatBGRA8Unorm;
    case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8:
        return MTLPixelFormatRGBA8Unorm;
    case D3DFMT_DXT1:
        return MTLPixelFormatBC1_RGBA;
    case D3DFMT_DXT2:
    case D3DFMT_DXT3:
        return MTLPixelFormatBC2_RGBA;
    case D3DFMT_DXT4:
    case D3DFMT_DXT5:
        return MTLPixelFormatBC3_RGBA;
    case D3DFMT_A16B16G16R16F:
        return MTLPixelFormatRGBA16Float;
    case D3DFMT_R16F:
        return MTLPixelFormatR16Float;
    case D3DFMT_R32F:
        return MTLPixelFormatR32Float;
    case D3DFMT_G16R16F:
        return MTLPixelFormatRG16Float;
    default:
        return MTLPixelFormatBGRA8Unorm;
    }
}

static uint32_t d3d9_mip_levels(uint32_t w, uint32_t h) {
    uint32_t n = 1;
    while (w > 1 || h > 1) { if (w > 1) w >>= 1; if (h > 1) h >>= 1; n++; }
    return n;
}

void *hle_d3d9_metal_create_texture(uint32_t width, uint32_t height, uint32_t format, int is_cube, uint32_t levels) {
    if (!g_metal_active && hle_d3d9_metal_enable() != 0) return NULL;

    @autoreleasepool {
        uint32_t w = width ? width : 1;
        uint32_t h = height ? height : 1;
        uint32_t maxLevels = d3d9_mip_levels(w, h);
        if (levels == 0) levels = maxLevels;
        if (levels > maxLevels) levels = maxLevels;
        MTLPixelFormat mtl_fmt = d3d_to_mtl_format(format);
        MTLTextureDescriptor *desc = nil;
        if (is_cube) {
            desc = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:mtl_fmt
                                                                        size:w
                                                                   mipmapped:(levels > 1)];
            desc.textureType = MTLTextureTypeCube;
        } else {
            desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:mtl_fmt
                                                                     width:w
                                                                    height:h
                                                                 mipmapped:(levels > 1)];
        }
        if (levels > 1) desc.mipmapLevelCount = levels;
        desc.usage = MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModeShared;
        id<MTLTexture> tex = [g_dev newTextureWithDescriptor:desc];
        if (!tex) fprintf(stderr, "[d3d9_metal] create_texture failed w=%u h=%u fmt=%u cube=%d levels=%u\n", w, h, format, is_cube, levels);
        return (void *)tex;
    }
}

void *hle_d3d9_metal_create_volume_texture(uint32_t width, uint32_t height, uint32_t depth, uint32_t levels, uint32_t format) {
    if (!g_metal_active && hle_d3d9_metal_enable() != 0) return NULL;

    @autoreleasepool {
        MTLPixelFormat mtl_fmt = d3d_to_mtl_format(format);
        MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:mtl_fmt
                                                                                        width:width ? width : 1
                                                                                       height:height ? height : 1
                                                                                    mipmapped:(levels > 1)];
        desc.textureType = MTLTextureType3D;
        desc.depth = depth ? depth : 1;
        if (levels > 1) {
            desc.mipmapLevelCount = levels;
        }
        desc.usage = MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModeShared;
        id<MTLTexture> tex = [g_dev newTextureWithDescriptor:desc];
        return (void *)tex;
    }
}

void hle_d3d9_metal_upload_volume_texture(void *tex, int level, const void *bytes, uint32_t bytes_per_row, uint32_t bytes_per_image) {
    if (!tex || !bytes) return;
    @autoreleasepool {
        id<MTLTexture> mtl_tex = (id<MTLTexture>)tex;
        NSUInteger w = mtl_tex.width >> level;
        NSUInteger h = mtl_tex.height >> level;
        NSUInteger d = mtl_tex.depth >> level;
        if (w == 0) w = 1;
        if (h == 0) h = 1;
        if (d == 0) d = 1;

        MTLRegion region = MTLRegionMake3D(0, 0, 0, w, h, d);
        [mtl_tex replaceRegion:region
                   mipmapLevel:(NSUInteger)level
                         slice:0
                     withBytes:bytes
                   bytesPerRow:(NSUInteger)bytes_per_row
                 bytesPerImage:(NSUInteger)bytes_per_image];
    }
}

int hle_d3d9_metal_read_volume_texture(void *tex, int level, void *out, uint32_t bytes_per_row, uint32_t bytes_per_image) {
    if (!g_metal_active || !tex || !out) return -1;
    @autoreleasepool {
        flush_gpu();
        id<MTLTexture> mtl_tex = (id<MTLTexture>)tex;
        NSUInteger w = mtl_tex.width >> level;
        NSUInteger h = mtl_tex.height >> level;
        NSUInteger d = mtl_tex.depth >> level;
        if (w == 0) w = 1;
        if (h == 0) h = 1;
        if (d == 0) d = 1;

        MTLRegion region = MTLRegionMake3D(0, 0, 0, w, h, d);
        [mtl_tex getBytes:out
              bytesPerRow:(NSUInteger)bytes_per_row
            bytesPerImage:(NSUInteger)bytes_per_image
               fromRegion:region
              mipmapLevel:(NSUInteger)level
                    slice:0];
    }
    return 0;
}

void hle_d3d9_metal_destroy_texture(void *tex) {
    if (tex) {
        id<MTLTexture> mtl_tex = (id<MTLTexture>)tex;
        [mtl_tex release];
    }
}

void hle_d3d9_metal_upload_texture(void *tex, int face, int level, const void *bytes, uint32_t bytes_per_row) {
    if (!tex || !bytes) return;
    @autoreleasepool {
        flush_gpu();
        id<MTLTexture> mtl_tex = (id<MTLTexture>)tex;
        NSUInteger w = mtl_tex.width >> level;
        NSUInteger h = mtl_tex.height >> level;
        if (w == 0) w = 1;
        if (h == 0) h = 1;

        MTLRegion region = MTLRegionMake2D(0, 0, w, h);
        [mtl_tex replaceRegion:region
                   mipmapLevel:(NSUInteger)level
                         slice:(NSUInteger)face
                     withBytes:bytes
                   bytesPerRow:(NSUInteger)bytes_per_row
                 bytesPerImage:0];
    }
}

void *hle_d3d9_metal_create_buffer(uint32_t size, int is_index) {
    (void)is_index;
    if (!g_metal_active && hle_d3d9_metal_enable() != 0) return NULL;
    @autoreleasepool {
        if (size == 0) size = 16;
        id<MTLBuffer> buf = [g_dev newBufferWithLength:size options:MTLResourceStorageModeShared];
        return (void *)buf;
    }
}

void hle_d3d9_metal_destroy_buffer(void *buf) {
    if (buf) {
        id<MTLBuffer> mtl_buf = (id<MTLBuffer>)buf;
        [mtl_buf release];
    }
}

void hle_d3d9_metal_upload_buffer(void *buf, uint32_t offset, const void *bytes, uint32_t size) {
    if (!buf || !bytes || size == 0) return;
    id<MTLBuffer> mtl_buf = (id<MTLBuffer>)buf;
    if (offset + size <= mtl_buf.length) {
        memcpy((uint8_t *)mtl_buf.contents + offset, bytes, size);
    }
}

void *hle_d3d9_metal_create_pipeline(const char *vs_msl, const char *ps_msl) {
    if (!g_metal_active && hle_d3d9_metal_enable() != 0) return NULL;
    if (!vs_msl || !ps_msl) return NULL;
    /* Cache by the shader pair: a new PS with the same VS needs a new
     * pipeline, and caching only on the VS would silently keep the old one. */
    for (int i = 0; i < g_pipeline_n; i++) {
        pipe_bundle_t *b = g_pipeline_cache[i];
        if (b && !strcmp(b->vs, vs_msl) && !strcmp(b->ps, ps_msl)) return b;
    }
    if (g_pipeline_n >= 64) return NULL;
    int is_ffp = (strstr(vs_msl, "ffp_vs") != NULL);
    const char *vs_fn = is_ffp ? "ffp_vs" : "vs_main";
    const char *ps_fn = strstr(ps_msl, "ffp_ps") ? "ffp_ps" : "ps_main";
    pipe_bundle_t *b = make_bundle(vs_msl, ps_msl, vs_fn, ps_fn, is_ffp ? 2 : 1);
    if (b) g_pipeline_cache[g_pipeline_n++] = b;
    return b;
}

void hle_d3d9_metal_destroy_pipeline(void *pipe) {
    destroy_bundle((pipe_bundle_t *)pipe);
}

static MTLPrimitiveType primitive_type(int prim_type) {
    switch (prim_type) {
    case 1:  return MTLPrimitiveTypePoint;      /* D3DPT_POINTLIST */
    case 2:  return MTLPrimitiveTypeLine;       /* D3DPT_LINELIST */
    case 3:  return MTLPrimitiveTypeLineStrip;
    case 4:  return MTLPrimitiveTypeTriangle;
    case 5:  return MTLPrimitiveTypeTriangleStrip;
    default: return MTLPrimitiveTypeTriangle;
    }
}

static id<MTLRenderPipelineState> select_pipeline(void *pipe) {
    pipe_bundle_t *b = (pipe_bundle_t *)pipe;
    if (!b) b = g_bound_texture ? g_textured_bundle : g_default_bundle;
    return bundle_pipeline(b);
}

static void prepare_encoder(id<MTLRenderPipelineState> pipe, void *vb, pipe_bundle_t *bundle) {
    [g_curr_encoder setRenderPipelineState:pipe];
    [g_curr_encoder setCullMode:cull_mode(g_cull_mode)];
    id<MTLDepthStencilState> ds = current_depth_state();
    if (ds) [g_curr_encoder setDepthStencilState:ds];
    apply_scissor();
    if (vb) [g_curr_encoder setVertexBuffer:(id<MTLBuffer>)vb offset:0 atIndex:0];
    if (bundle && bundle->translated == 2 && g_ffp_uniforms_size > 0) {
        [g_curr_encoder setVertexBytes:g_ffp_uniforms length:g_ffp_uniforms_size atIndex:1];
        [g_curr_encoder setFragmentBytes:g_ffp_uniforms length:g_ffp_uniforms_size atIndex:1];
    } else {
        /* Translated shaders read c/ci/cb from [[buffer(1..3)]] in both stages. */
        [g_curr_encoder setVertexBytes:g_vs_const_f length:sizeof(g_vs_const_f) atIndex:1];
        [g_curr_encoder setFragmentBytes:g_ps_const_f length:sizeof(g_ps_const_f) atIndex:1];
        [g_curr_encoder setVertexBytes:g_vs_const_i length:sizeof(g_vs_const_i) atIndex:2];
        [g_curr_encoder setFragmentBytes:g_ps_const_i length:sizeof(g_ps_const_i) atIndex:2];
        [g_curr_encoder setVertexBytes:g_vs_const_b length:sizeof(g_vs_const_b) atIndex:3];
        [g_curr_encoder setFragmentBytes:g_ps_const_b length:sizeof(g_ps_const_b) atIndex:3];
    }
    /* buffer(4): user clip planes for both FFP and translated shaders. */
    [g_curr_encoder setVertexBytes:g_clip_uniforms length:sizeof(g_clip_uniforms) atIndex:4];
    [g_curr_encoder setFragmentBytes:g_clip_uniforms length:sizeof(g_clip_uniforms) atIndex:4];
    for (uint32_t i = 0; i < 16; i++) {
        if (!g_samplers[i]) continue;
        [g_curr_encoder setFragmentTexture:(g_samp_srgb && g_samplers_srgb[i]) ? g_samplers_srgb[i] : g_samplers[i] atIndex:i];
        [g_curr_encoder setFragmentSamplerState:g_sampler atIndex:i];
    }
}

int hle_d3d9_metal_draw(int prim_type, uint32_t start_vert, uint32_t vert_count, void *pipe, void *vb, uint32_t stride) {
    (void)stride;
    if (!g_metal_active || !g_curr_encoder) return 0;
    @autoreleasepool {
        pipe_bundle_t *b = (pipe_bundle_t *)pipe;
        id<MTLRenderPipelineState> mtl_pipe = select_pipeline(pipe);
        if (!mtl_pipe) return 0;
        prepare_encoder(mtl_pipe, vb, b);
        [g_curr_encoder drawPrimitives:primitive_type(prim_type)
                          vertexStart:start_vert vertexCount:vert_count];
    }
    return 0;
}

int hle_d3d9_metal_draw_indexed(int prim_type, uint32_t base_vertex, uint32_t start_index,
                                uint32_t index_count, void *pipe, void *vb, void *ib,
                                uint32_t stride) {
    (void)stride;
    if (!g_metal_active || !g_curr_encoder || !ib) return 0;
    @autoreleasepool {
        pipe_bundle_t *b = (pipe_bundle_t *)pipe;
        id<MTLRenderPipelineState> mtl_pipe = select_pipeline(pipe);
        if (!mtl_pipe) return 0;
        prepare_encoder(mtl_pipe, vb, b);
        [g_curr_encoder drawIndexedPrimitives:primitive_type(prim_type)
                                   indexCount:index_count
                                    indexType:MTLIndexTypeUInt16
                                  indexBuffer:(id<MTLBuffer>)ib
                            indexBufferOffset:(NSUInteger)start_index * 2u
                                instanceCount:1
                                   baseVertex:(NSInteger)base_vertex
                                  baseInstance:0];
    }
    return 0;
}

int hle_d3d9_metal_draw_indexed32(int prim_type, uint32_t base_vertex, uint32_t start_index,
                                  uint32_t index_count, void *pipe, void *vb, void *ib,
                                  uint32_t stride) {
    (void)stride;
    if (!g_metal_active || !g_curr_encoder || !ib) return 0;
    @autoreleasepool {
        pipe_bundle_t *b = (pipe_bundle_t *)pipe;
        id<MTLRenderPipelineState> mtl_pipe = select_pipeline(pipe);
        if (!mtl_pipe) return 0;
        prepare_encoder(mtl_pipe, vb, b);
        [g_curr_encoder drawIndexedPrimitives:primitive_type(prim_type)
                                   indexCount:index_count
                                    indexType:MTLIndexTypeUInt32
                                  indexBuffer:(id<MTLBuffer>)ib
                            indexBufferOffset:(NSUInteger)start_index * 4u
                                instanceCount:1
                                   baseVertex:(NSInteger)base_vertex
                                  baseInstance:0];
    }
    return 0;
}

void hle_d3d9_metal_set_stream_divisor(uint32_t stream, uint32_t divisor) {
    if (stream >= 16) return;
    g_streams[stream].divisor = divisor;
    if (divisor) g_instanced_stream = stream;
    else if (g_instanced_stream == stream) g_instanced_stream = 0xFFFFFFFFu;
}

int hle_d3d9_metal_draw_instanced(int prim_type, uint32_t start_vert, uint32_t vert_count,
                                  uint32_t instance_count, void *pipe, void *vb, uint32_t stride) {
    (void)stride;
    if (!g_metal_active || !g_curr_encoder || !instance_count) return -1;
    @autoreleasepool {
        pipe_bundle_t *b = (pipe_bundle_t *)pipe;
        id<MTLRenderPipelineState> mtl_pipe = select_pipeline(pipe);
        if (!mtl_pipe) return -1;
        prepare_encoder(mtl_pipe, vb, b);
        [g_curr_encoder drawPrimitives:primitive_type(prim_type)
                          vertexStart:start_vert vertexCount:vert_count
                        instanceCount:instance_count baseInstance:0];
    }
    return 0;
}

int hle_d3d9_metal_draw_indexed_instanced(int prim_type, uint32_t base_vertex, uint32_t start_index,
                                          uint32_t index_count, uint32_t instance_count,
                                          void *pipe, void *vb, void *ib, uint32_t stride, int index32,
                                          uint32_t inst_stream, void *inst_vb, uint32_t inst_stride) {
    (void)stride; (void)inst_stream; (void)inst_stride;
    if (!g_metal_active || !g_curr_encoder || !ib || !instance_count) return -1;
    @autoreleasepool {
        pipe_bundle_t *b = (pipe_bundle_t *)pipe;
        id<MTLRenderPipelineState> mtl_pipe = select_pipeline(pipe);
        if (!mtl_pipe) return -1;
        prepare_encoder(mtl_pipe, vb, b);
        /* Per-instance attributes declared in streams > 0 use Metal buffer 5. */
        if (inst_vb) [g_curr_encoder setVertexBuffer:(id<MTLBuffer>)inst_vb offset:0 atIndex:5];
        [g_curr_encoder drawIndexedPrimitives:primitive_type(prim_type)
                                   indexCount:index_count
                                    indexType:index32 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                                  indexBuffer:(id<MTLBuffer>)ib
                            indexBufferOffset:(NSUInteger)start_index * (index32 ? 4u : 2u)
                                instanceCount:instance_count
                                   baseVertex:(NSInteger)base_vertex
                                  baseInstance:0];
    }
    return 0;
}

/* ---- Render target selection, texture binding, pixel readback ---- */

void *hle_d3d9_metal_create_msaa_render_target(uint32_t width, uint32_t height, uint32_t samples) {
    if (!g_metal_active && hle_d3d9_metal_enable() != 0) return NULL;
    if (samples != 2u && samples != 4u) return NULL;
    @autoreleasepool {
        MTLTextureDescriptor *d = [[MTLTextureDescriptor alloc] init];
        d.textureType = MTLTextureType2DMultisample;
        d.pixelFormat = MTLPixelFormatBGRA8Unorm;
        d.width = width ? width : 1;
        d.height = height ? height : 1;
        d.depth = 1;
        d.mipmapLevelCount = 1;
        d.arrayLength = 1;
        d.storageMode = MTLStorageModePrivate;
        d.sampleCount = samples;
        d.usage = MTLTextureUsageRenderTarget;   /* MSAA textures are render targets only */
        id<MTLTexture> c = [g_dev newTextureWithDescriptor:d];
        d.textureType = MTLTextureType2D;       /* the resolve target is single-sample */
        d.sampleCount = 1;
        d.storageMode = MTLStorageModeShared;   /* CPU readback via getBytes */
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        id<MTLTexture> r = [g_dev newTextureWithDescriptor:d];
        if (!c || !r) { fprintf(stderr, "[d3d9_metal] MSAA RT allocation failed\n"); [c release]; [r release]; return NULL; }
        msaa_rt_t *rt = (msaa_rt_t *)calloc(1, sizeof *rt);
        rt->colour = [c retain]; rt->resolved = [r retain];
        rt->w = width; rt->h = height; rt->samples = samples;
        [c release]; [r release];
        return rt;
    }
}

void hle_d3d9_metal_destroy_msaa_render_target(void *rt) {
    if (!rt) return;
    msaa_rt_t *m = (msaa_rt_t *)rt;
    if (g_msaa_ds_tex && (m->w != g_rt_width || m->h != g_rt_height)) { /* leave depth alone */ }
    [m->colour release]; [m->resolved release];
    free(m);
}

void hle_d3d9_metal_resolve_msaa_render_target(void *rt) {
    /* The render pass resolves into the single-sample texture via
     * MTLStoreActionMultisampleResolve; just make sure pending work is done. */
    (void)rt;
    if (g_curr_encoder) {
        [g_curr_encoder endEncoding];
        [g_curr_encoder release];
        g_curr_encoder = nil;
    }
    flush_gpu();
    g_msaa_needs_resolve = 0;
}

void *hle_d3d9_metal_create_render_target(uint32_t width, uint32_t height) {
    if (!g_metal_active && hle_d3d9_metal_enable() != 0) return NULL;
    if (!width) width = 1;
    if (!height) height = 1;
    @autoreleasepool {
        MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                       width:width
                                                                                      height:height
                                                                                   mipmapped:NO];
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModeShared;
        id<MTLTexture> rt = [g_dev newTextureWithDescriptor:desc];  /* +1: owned by the surface ctx */
        return (void *)rt;
    }
}

void hle_d3d9_metal_destroy_render_target(void *rt) {
    if (rt) [(id<MTLTexture>)rt release];
}

static void bind_rt_texture(id<MTLTexture> next) {
    /* Borrowed pointer: the surface context owns the texture. NULL restores
     * the backbuffer texture created by init_device. */
    if (next == g_rt_tex) return;
    @autoreleasepool {
        /* An encoder opened by BeginScene still targets the old texture: a
         * Metal render pass cannot change its colour attachment in place.
         * End it, bind the new target, and reopen so draws in the same scene
         * go to the new target instead of silently using the old one. */
        BOOL was_in_scene = (g_curr_encoder != nil);
        if (g_curr_encoder) {
            [g_curr_encoder endEncoding];
            [g_curr_encoder release];
            g_curr_encoder = nil;
        }
        g_rt_tex = next;
        rebuild_rt_srgb_view();
        if (was_in_scene && g_curr_cmd_buf) {
            MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
            apply_visibility(rpd);
            rpd.colorAttachments[0].texture = current_rt_view();
            rpd.colorAttachments[0].loadAction = MTLLoadActionLoad;
            rpd.colorAttachments[0].storeAction = MTLStoreActionStore; apply_msaa_resolve(rpd);
            if (g_ds_tex) {
                rpd.depthAttachment.texture = current_ds_texture();
                rpd.depthAttachment.loadAction = MTLLoadActionLoad;
                rpd.depthAttachment.storeAction = MTLStoreActionStore;
            }
            g_curr_encoder = [[g_curr_cmd_buf renderCommandEncoderWithDescriptor:rpd] retain];
        }
    }
}

void hle_d3d9_metal_set_render_target(void *rt) {
    if (g_msaa_rt) { hle_d3d9_metal_resolve_msaa_render_target(g_msaa_rt); g_msaa_rt = NULL; }
    g_rt_samples = 1;
    bind_rt_texture(rt ? (id<MTLTexture>)rt : g_owned_rt);
}

void hle_d3d9_metal_set_msaa_render_target(void *rt) {
    msaa_rt_t *m = (msaa_rt_t *)rt;
    if (g_msaa_rt && g_msaa_rt != m) hle_d3d9_metal_resolve_msaa_render_target(g_msaa_rt);
    g_msaa_rt = m;
    if (g_msaa_ds_tex) { [g_msaa_ds_tex release]; g_msaa_ds_tex = nil; }
    if (m) {
        @autoreleasepool {
            MTLTextureDescriptor *dd = [[MTLTextureDescriptor alloc] init];
            dd.textureType = MTLTextureType2DMultisample;
            dd.pixelFormat = MTLPixelFormatDepth32Float;
            dd.width = m->w; dd.height = m->h; dd.depth = 1;
            dd.mipmapLevelCount = 1; dd.arrayLength = 1;
            dd.usage = MTLTextureUsageRenderTarget;
            dd.storageMode = MTLStorageModePrivate;
            dd.sampleCount = m->samples;
            g_msaa_ds_tex = [[g_dev newTextureWithDescriptor:dd] retain];
        }
    }
    uint32_t samples = m ? m->samples : 1u;
    if (samples != g_rt_samples) {
        g_rt_samples = samples;
        /* Pipelines encode sampleCount; rebuild them for the new RT. */
        for (int i = 0; i < g_pipeline_n; i++) destroy_bundle(g_pipeline_cache[i]);
        g_pipeline_n = 0;
        if (g_default_bundle) { destroy_bundle(g_default_bundle); g_default_bundle = NULL; }
        if (g_textured_bundle) { destroy_bundle(g_textured_bundle); g_textured_bundle = NULL; }
    }
    bind_rt_texture(m ? m->colour : g_owned_rt);
}

static id<MTLTexture> msaa_resolved_texture(void) {
    if (g_msaa_rt) {
        hle_d3d9_metal_resolve_msaa_render_target(g_msaa_rt);
        return g_msaa_rt->resolved;
    }
    return g_rt_tex;
}

void hle_d3d9_metal_set_texture(void *tex) {
    if (g_bound_texture == (id<MTLTexture>)tex) return;
    if (g_bound_texture) [g_bound_texture release];
    g_bound_texture = (id<MTLTexture>)tex;
    if (g_bound_texture) [g_bound_texture retain];
    hle_d3d9_metal_set_sampler(0, tex);
}

static MTLSamplerMinMagFilter d3d9_mtl_filter(uint32_t f) {
    /* D3DTEXF_NONE=0, POINT=1, LINEAR=2, ANISOTROPIC=3 */
    return (f == 2 || f == 3) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
}
static MTLSamplerMipFilter d3d9_mtl_mip(uint32_t f) {
    if (f == 1) return MTLSamplerMipFilterNearest;
    if (f == 2 || f == 3) return MTLSamplerMipFilterLinear;
    return MTLSamplerMipFilterNotMipmapped;
}
static MTLSamplerAddressMode d3d9_mtl_address(uint32_t mode) {
    /* D3DTADDRESS: WRAP=1, MIRROR=2, CLAMP=3, BORDER=4, MIRRORONCE=5 */
    switch (mode) {
        case 1: return MTLSamplerAddressModeRepeat;
        case 2: return MTLSamplerAddressModeMirrorRepeat;
        case 3: return MTLSamplerAddressModeClampToEdge;
        case 4: return MTLSamplerAddressModeClampToZero; /* border color set via sd.borderColor below */
        case 5: return MTLSamplerAddressModeMirrorClampToEdge;
        default: return MTLSamplerAddressModeClampToEdge;
    }
}

/* D3DCOLOR = 0xAARRGGBB -> MTLSamplerBorderColor. Metal kennt nur die
 * Standard-Farben TransparentBlack/OpaqueBlack/OpaqueWhite; beliebige
 * Custom-Farben werden nicht unterstützt und fallen auf TransparentBlack
 * zurück (dazu wird geloggt). Quelle: Apple MTLSamplerBorderColor-Doku. */
static MTLSamplerBorderColor d3d9_mtl_border_color(uint32_t d3dcolor) {
    uint8_t a = (uint8_t)(d3dcolor >> 24);
    uint8_t r = (uint8_t)(d3dcolor >> 16);
    uint8_t g = (uint8_t)(d3dcolor >> 8);
    uint8_t b = (uint8_t)d3dcolor;
    if (a == 0) return MTLSamplerBorderColorTransparentBlack;
    if (r == 0 && g == 0 && b == 0 && a == 255) return MTLSamplerBorderColorOpaqueBlack;
    if (r == 255 && g == 255 && b == 255 && a == 255) return MTLSamplerBorderColorOpaqueWhite;
    fprintf(stderr, "[d3d9_metal] Border color 0x%08X: Metal kann keine beliebige Custom-Border-Farbe, verwende TransparentBlack\n", d3dcolor);
    return MTLSamplerBorderColorTransparentBlack;
}

static void d3d9_metal_rebuild_sampler(void) {
    if (!g_dev) return;
    @autoreleasepool {
        MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
        sd.minFilter = d3d9_mtl_filter(g_samp_min);
        sd.magFilter = d3d9_mtl_filter(g_samp_mag);
        sd.mipFilter = d3d9_mtl_mip(g_samp_mip);
        sd.sAddressMode = d3d9_mtl_address(g_samp_addr_u);
        sd.tAddressMode = d3d9_mtl_address(g_samp_addr_v);
        sd.rAddressMode = d3d9_mtl_address(g_samp_addr_w);
        /* D3DTADDRESS_BORDER: Metal hat keinen "clamp to border with custom
         * color"-Modus. sd.borderColor wird gesetzt, sobald U/V/W auf BORDER
         * steht; ob Metal die Farbe tatsächlich anwendet, hängt vom Device ab. */
        if (g_samp_addr_u == 4 || g_samp_addr_v == 4 || g_samp_addr_w == 4) {
            sd.borderColor = d3d9_mtl_border_color(g_samp_border);
            fprintf(stderr, "[d3d9_metal] BORDER address mode, border color 0x%08X -> %s\n",
                g_samp_border,
                sd.borderColor == MTLSamplerBorderColorTransparentBlack ? "TransparentBlack" :
                sd.borderColor == MTLSamplerBorderColorOpaqueBlack ? "OpaqueBlack" :
                sd.borderColor == MTLSamplerBorderColorOpaqueWhite ? "OpaqueWhite" : "Custom");
        }
        /* D3DTEXF_ANISOTROPIC on either filter selects anisotropic sampling;
         * Metal caps it at the device's maxAnisotropy. */
        if (g_samp_min == 3 || g_samp_mag == 3)
            sd.maxAnisotropy = g_samp_aniso > 16u ? 16u : g_samp_aniso;
        id<MTLSamplerState> ns = [g_dev newSamplerStateWithDescriptor:sd];
        [sd release];
        if (ns) {
            if (g_sampler) [g_sampler release];
            g_sampler = ns;
        }
    }
}

static void rebuild_srgb_views(void) {
    for (unsigned i = 0; i < 16; i++) {
        if (g_samplers_srgb[i]) { [g_samplers_srgb[i] release]; g_samplers_srgb[i] = nil; }
        if (g_samp_srgb && g_samplers[i]) {
            id<MTLTexture> view = [g_samplers[i] newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm_sRGB];
            if (view) g_samplers_srgb[i] = view;
            else fprintf(stderr, "[d3d9_metal] sRGB view failed for stage %u\n", i);
        }
    }
}

void hle_d3d9_metal_set_sampler_state(uint32_t stage, uint32_t type, uint32_t value) {
    (void)stage; /* global sampler used for all stages currently */
    /* D3DSAMP: ADDRESSU=1, ADDRESSV=2, ADDRESSW=3, BORDERCOLOR=4,
     * MAGFILTER=5, MINFILTER=6, MIPFILTER=7, MAXANISOTROPY=8,
     * MIPMAPLODBIAS=9, MAXMIPLEVEL=10, SRGBTEXTURE=11 */
    if (type == 1) { g_samp_addr_u = value ? value : 3; }
    else if (type == 2) { g_samp_addr_v = value ? value : 3; }
    else if (type == 3) { g_samp_addr_w = value ? value : 3; }
    else if (type == 4) { g_samp_border = value; }
    else if (type == 5) g_samp_mag = value;
    else if (type == 6) g_samp_min = value;
    else if (type == 7) g_samp_mip = value;
    else if (type == 8) {
        g_samp_aniso = value ? value : 1u;
        d3d9_metal_rebuild_sampler();
        return;
    } else if (type == 11) {
        g_samp_srgb = value ? 1 : 0;
        rebuild_srgb_views();
        return;
    } else return;
    d3d9_metal_rebuild_sampler();
}

void hle_d3d9_metal_set_sampler(uint32_t stage, void *tex) {
    if (stage >= 16) return;
    id<MTLTexture> t = (id<MTLTexture>)tex;
    if (g_samplers[stage] == t) return;
    if (g_samplers[stage]) [g_samplers[stage] release];
    g_samplers[stage] = t;
    if (t) [t retain];
    if (g_samplers_srgb[stage]) { [g_samplers_srgb[stage] release]; g_samplers_srgb[stage] = nil; }
    if (t && g_samp_srgb) {
        /* A sRGB view decodes the same texels on read (public D3DSAMP_SRGBTEXTURE). */
        id<MTLTexture> view = [t newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm_sRGB];
        if (view) g_samplers_srgb[stage] = view;
        else fprintf(stderr, "[d3d9_metal] sRGB texture view failed for stage %u\n", stage);
    }
}

void hle_d3d9_metal_set_clip_planes(const float planes[6][4], uint32_t mask) {
    if (planes) memcpy(g_clip_planes, planes, sizeof(g_clip_planes));
    else memset(g_clip_planes, 0, sizeof(g_clip_planes));
    g_clip_mask = mask;
    memset(g_clip_uniforms, 0, sizeof(g_clip_uniforms));
    memcpy(g_clip_uniforms, &mask, sizeof(mask));
    memcpy(g_clip_uniforms + 16, g_clip_planes, sizeof(g_clip_planes));
}

void hle_d3d9_metal_set_ffp_uniforms(const void *uniforms, uint32_t size) {
    if (!uniforms || size == 0) {
        g_ffp_uniforms_size = 0;
        return;
    }
    if (size > sizeof(g_ffp_uniforms)) size = sizeof(g_ffp_uniforms);
    memcpy(g_ffp_uniforms, uniforms, size);
    g_ffp_uniforms_size = size;
}

static void copy_consts(uint32_t dst_b[16][4], const uint32_t *src_b) {
    for (uint32_t k = 0; k < 16; k++) dst_b[k][0] = src_b ? src_b[k] : 0;
}

void hle_d3d9_metal_set_shader_constants(const float *vsf, const int32_t *vsi, const uint32_t *vsb,
                                         const float *psf, const int32_t *psi, const uint32_t *psb) {
    if (vsf) memcpy(g_vs_const_f, vsf, sizeof(g_vs_const_f));
    if (vsi) memcpy(g_vs_const_i, vsi, sizeof(g_vs_const_i));
    copy_consts(g_vs_const_b, vsb);
    if (psf) memcpy(g_ps_const_f, psf, sizeof(g_ps_const_f));
    if (psi) memcpy(g_ps_const_i, psi, sizeof(g_ps_const_i));
    copy_consts(g_ps_const_b, psb);
}

void hle_d3d9_metal_set_vertex_decl(const D3DVERTEXELEMENT9 *elements, uint32_t count) {
    if (count > 64) count = 64;
    uint32_t hash = count;
    for (uint32_t k = 0; k < count; k++) {
        g_decl[k] = elements[k];
        hash = hash * 33u + elements[k].Offset + elements[k].Type * 7u +
               elements[k].Usage * 11u + elements[k].UsageIndex * 13u;
    }
    g_decl_count = count;
    g_decl_hash = hash;
}

/* End the open encoder, submit and wait, so a following getBytes sees every
 * clear and draw that the D3D9 calls issued. */
static void flush_gpu(void) {
    if (g_curr_encoder) {
        [g_curr_encoder endEncoding];
        [g_curr_encoder release];
        g_curr_encoder = nil;
    }
    if (g_curr_cmd_buf) {
        [g_curr_cmd_buf commit];
        [g_curr_cmd_buf waitUntilCompleted];
        [g_curr_cmd_buf release];
        g_curr_cmd_buf = nil;
    }
}

/* ---- IDirect3DQuery9 backend ------------------------------------------- */

typedef struct {
    uint32_t type;                       /* D3DQUERYTYPE_EVENT / _OCCLUSION */
    uint32_t offset;                     /* visibility result buffer slot */
    uint32_t counting;                   /* 1 between BEGIN and END */
    id<MTLCommandBuffer> cb;             /* retained; EVENT only */
} metal_query_t;

void *hle_d3d9_metal_query_create(uint32_t type) {
    if (!g_metal_active) return NULL;
    if (type != D3DQUERYTYPE_OCCLUSION && type != D3DQUERYTYPE_EVENT) return NULL;
    if (type == D3DQUERYTYPE_OCCLUSION && !g_vis_buf) return NULL;

    metal_query_t *q = (metal_query_t *)calloc(1, sizeof(*q));
    if (!q) return NULL;
    q->type = type;
    if (type == D3DQUERYTYPE_OCCLUSION) {
        if (g_vis_free_n) {
            q->offset = g_vis_free[--g_vis_free_n];
        } else {
            if (g_vis_next_offset + VIS_SLOT_SIZE > VIS_BUF_SIZE) { free(q); return NULL; }
            q->offset = g_vis_next_offset;
            g_vis_next_offset += VIS_SLOT_SIZE;
        }
        memset((uint8_t *)[g_vis_buf contents] + q->offset, 0, VIS_SLOT_SIZE);
    }
    return q;
}

int hle_d3d9_metal_query_issue(void *handle, uint32_t flags) {
    metal_query_t *q = (metal_query_t *)handle;
    if (!q || !g_metal_active) return -1;
    if (!(flags & (D3DISSUE_BEGIN | D3DISSUE_END))) return -1;

    @autoreleasepool {
        if (q->type == D3DQUERYTYPE_OCCLUSION) {
            if (!g_vis_buf) return -1;
            if (flags & D3DISSUE_BEGIN) {
                memset((uint8_t *)[g_vis_buf contents] + q->offset, 0, VIS_SLOT_SIZE);
                if (g_curr_encoder) {
                    [g_curr_encoder setVisibilityResultMode:MTLVisibilityResultModeCounting
                                                    offset:(NSUInteger)q->offset];
                }
                q->counting = 1;
            }
            if (flags & D3DISSUE_END) {
                if (g_curr_encoder) {
                    [g_curr_encoder setVisibilityResultMode:MTLVisibilityResultModeDisabled
                                                    offset:(NSUInteger)0];
                }
                q->counting = 0;
            }
        } else if (q->type == D3DQUERYTYPE_EVENT) {
            if (flags & D3DISSUE_END) {
                /* An event is a fence: everything encoded so far belongs to
                 * the command buffer we commit here. Its completion status is
                 * what GetData reports -- no fabricated "done" value. */
                if (g_curr_encoder) {
                    [g_curr_encoder endEncoding];
                    [g_curr_encoder release];
                    g_curr_encoder = nil;
                }
                if (!g_curr_cmd_buf)
                    g_curr_cmd_buf = [[g_cmd_queue commandBuffer] retain];
                if (q->cb) [q->cb release];
                q->cb = [g_curr_cmd_buf retain];        /* keep the fenced buffer alive */
                [g_curr_cmd_buf commit];
                [g_curr_cmd_buf release];
                g_curr_cmd_buf = nil;
                /* Draws after Issue(END) go to a fresh render pass. */
                hle_d3d9_metal_begin_scene();
            }
        } else {
            return -1;
        }
    }
    return 0;
}

int hle_d3d9_metal_query_get_data(void *handle, void *out, uint32_t size, uint32_t flags) {
    metal_query_t *q = (metal_query_t *)handle;
    if (!q || !out || size < 4 || !g_metal_active) return -1;

    @autoreleasepool {
        if (q->type == D3DQUERYTYPE_OCCLUSION) {
            if (!g_vis_buf || q->counting) return -1;   /* Issue(END) missing */
            if (flags & D3DGETDATA_FLUSH) {
                flush_gpu();                            /* commit + wait, read real counter */
                hle_d3d9_metal_begin_scene();           /* keep drawing after GetData */
            }
            uint64_t count = 0;
            memcpy(&count, (const uint8_t *)[g_vis_buf contents] + q->offset, sizeof(count));
            *(uint32_t *)out = (uint32_t)count;
        } else if (q->type == D3DQUERYTYPE_EVENT) {
            if (!q->cb) { *(uint32_t *)out = 0; return 1; }  /* Issue(END) missing: S_FALSE */
            if (flags & D3DGETDATA_FLUSH) [q->cb waitUntilCompleted];
            MTLCommandBufferStatus st = [q->cb status];
            int done = (st == MTLCommandBufferStatusCompleted);
            *(uint32_t *)out = done ? 1 : 0;
            if (!done) return 1;                        /* S_FALSE, not an error */
        } else {
            return -1;
        }
    }
    return 0;
}

void hle_d3d9_metal_query_destroy(void *handle) {
    metal_query_t *q = (metal_query_t *)handle;
    if (!q) return;
    @autoreleasepool {
        if (q->cb) { [q->cb release]; q->cb = nil; }
        if (q->type == D3DQUERYTYPE_OCCLUSION && g_vis_buf &&
            g_vis_free_n < VIS_BUF_SIZE / VIS_SLOT_SIZE) {
            g_vis_free[g_vis_free_n++] = q->offset;   /* recycle the 8-byte slot */
        }
    }
    free(q);
}

int hle_d3d9_metal_read_pixels(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                               void *out, uint32_t out_bytes_per_row) {
    if (!g_metal_active || !g_rt_tex || !out || !w || !h) return -1;
    @autoreleasepool {
        flush_gpu();
        MTLRegion region = MTLRegionMake2D(x, y, w, h);
        [msaa_resolved_texture() getBytes:out bytesPerRow:out_bytes_per_row fromRegion:region mipmapLevel:0];
    }
    return 0;
}

int hle_d3d9_metal_read_render_target(void *rt, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                      void *out, uint32_t out_bytes_per_row) {
    id<MTLTexture> target_tex = rt ? (id<MTLTexture>)rt : g_rt_tex;
    if (g_msaa_rt && rt == (void *)g_msaa_rt) {
        hle_d3d9_metal_resolve_msaa_render_target(g_msaa_rt);
        target_tex = g_msaa_rt->resolved;
    }
    if (!g_metal_active || !target_tex || !out || !w || !h) return -1;
    @autoreleasepool {
        flush_gpu();
        MTLRegion region = MTLRegionMake2D(x, y, w, h);
        [target_tex getBytes:out bytesPerRow:out_bytes_per_row fromRegion:region mipmapLevel:0];
    }
    return 0;
}

int hle_d3d9_metal_copy_texture_rect(void *src_tex, uint32_t sx, uint32_t sy,
                                     void *dst_tex, uint32_t dx, uint32_t dy,
                                     uint32_t w, uint32_t h) {
    if (!g_metal_active || !src_tex || !dst_tex || !w || !h) return -1;
    @autoreleasepool {
        flush_gpu();
        id<MTLCommandBuffer> cmd_buf = [g_cmd_queue commandBuffer];
        if (!cmd_buf) return -1;
        id<MTLBlitCommandEncoder> blit = [cmd_buf blitCommandEncoder];
        if (!blit) return -1;
        [blit copyFromTexture:(id<MTLTexture>)src_tex
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(sx, sy, 0)
                   sourceSize:MTLSizeMake(w, h, 1)
                    toTexture:(id<MTLTexture>)dst_tex
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(dx, dy, 0)];
        [blit endEncoding];
        [cmd_buf commit];
        [cmd_buf waitUntilCompleted];
    }
    return 0;
}
