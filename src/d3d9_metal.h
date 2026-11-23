/*
 * d3d9_metal.h - Direct3D 9 Metal GPU backend interface for macOS.
 */
#ifndef WIN32HLE_D3D9_METAL_H
#define WIN32HLE_D3D9_METAL_H

#include <stdint.h>
#include <stddef.h>
#include "d3d9_hle.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Enables the Metal GPU backend for D3D9.
 * Returns 0 on success, -1 on failure.
 */
int hle_d3d9_metal_enable(void);

/* Returns 1 if Metal backend is active, 0 otherwise */
int hle_d3d9_metal_is_active(void);

/* Direct3D 9 Metal operations */
int  hle_d3d9_metal_init_device(uint32_t width, uint32_t height);
void hle_d3d9_metal_shutdown_device(void);
int  hle_d3d9_metal_begin_scene(void);
int  hle_d3d9_metal_end_scene(void);
int  hle_d3d9_metal_clear(uint32_t flags, uint32_t color, float z, uint32_t stencil);
int  hle_d3d9_metal_present(void);

/* Texture / Buffer management */
void *hle_d3d9_metal_create_texture(uint32_t width, uint32_t height, uint32_t format, int is_cube, uint32_t levels);
void *hle_d3d9_metal_create_volume_texture(uint32_t width, uint32_t height, uint32_t depth, uint32_t levels, uint32_t format);
void  hle_d3d9_metal_destroy_texture(void *tex);
void  hle_d3d9_metal_upload_texture(void *tex, int face, int level, const void *bytes, uint32_t bytes_per_row);
void  hle_d3d9_metal_upload_volume_texture(void *tex, int level, const void *bytes, uint32_t bytes_per_row, uint32_t bytes_per_image);
int   hle_d3d9_metal_read_volume_texture(void *tex, int level, void *out, uint32_t bytes_per_row, uint32_t bytes_per_image);

void *hle_d3d9_metal_create_buffer(uint32_t size, int is_index);
void  hle_d3d9_metal_destroy_buffer(void *buf);
void  hle_d3d9_metal_upload_buffer(void *buf, uint32_t offset, const void *bytes, uint32_t size);

/* Pipeline & Shaders */
void *hle_d3d9_metal_create_pipeline(const char *vs_msl, const char *ps_msl);
void  hle_d3d9_metal_destroy_pipeline(void *pipe);

/* Draw calls */
int  hle_d3d9_metal_draw(int prim_type, uint32_t start_vert, uint32_t vert_count, void *pipe, void *vb, uint32_t stride);
int  hle_d3d9_metal_draw_indexed(int prim_type, uint32_t base_vertex, uint32_t start_index,
                                 uint32_t index_count, void *pipe, void *vb, void *ib, uint32_t stride);
void hle_d3d9_metal_set_render_state(uint32_t blend_enable, uint32_t src_blend, uint32_t dst_blend,
                                     uint32_t z_enable, uint32_t z_write, uint32_t z_func,
                                     uint32_t cull_mode, uint32_t scissor_enable, const int32_t scissor[4]);
void hle_d3d9_metal_set_vertex_decl(const D3DVERTEXELEMENT9 *elements, uint32_t count);
void hle_d3d9_metal_set_shader_constants(const float *vsf, const int32_t *vsi, const uint32_t *vsb,
                                         const float *psf, const int32_t *psi, const uint32_t *psb);
void hle_d3d9_metal_set_sampler(uint32_t stage, void *tex);
void hle_d3d9_metal_set_sampler_state(uint32_t stage, uint32_t type, uint32_t value);
/* D3DRS_SRGBWRITEENABLE: render through an sRGB view of the current target. */
void hle_d3d9_metal_set_srgb_write(int enabled);

/* Fixed-function lighting/material uniform payload uploaded by
 * hle_d3d9_metal_set_ffp_uniforms. The layout must match the FFP MSL shader's
 * uniform struct in d3d9_metal.m byte for byte. Shared here so the device
 * (which fills it) and the Metal backend (which buffers/uploads it) agree on
 * sizeof; the backend buffer is sized from this struct, never a magic number. */
typedef struct {
    float diffuse[4];
    float specular[4];
    float ambient[4];
    float pos[4];
    float dir[4];
    float params1[4]; /* range, falloff, theta, phi */
    float params2[4]; /* att0, att1, att2, type */
} ffp_light_payload_t;

typedef struct {
    float diffuse[4];
    float ambient[4];
    float specular[4];
    float emissive[4];
    float power_pad[4];
} ffp_mat_payload_t;

typedef struct {
    float mvp[16];
    float fogColor[4];
    float fogStart;
    float fogEnd;
    float fogDensity;
    float _pad;
    float world[16];   /* world-space user clip planes (public SetClipPlane) */
    float view[16];    /* camera-space texgen (texturstufen) */
    float proj[16];
    float textureFactor[4];
    float globalAmbient[4];
    ffp_mat_payload_t mat;
    ffp_light_payload_t lights[8];
    float texTransform[8][16];
    float viewportXYWH[4];  /* X, Y, Width, Height — XYZRHW screen-pixel→clip conversion */
    float viewportZ[4];     /* MinZ, MaxZ, 0, 0 */
} ffp_uniforms_payload_t;

void hle_d3d9_metal_set_ffp_uniforms(const void *uniforms, uint32_t size);
int  hle_d3d9_metal_draw_indexed32(int prim_type, uint32_t base_vertex, uint32_t start_index,
                                   uint32_t index_count, void *pipe, void *vb, void *ib, uint32_t stride);

/* User clip planes: planes[i] = (a,b,c,d) with visible ax+by+cz+dw >= 0.
 * mask bit i enables plane i.  FFP planes arrive in world space, translated
 * shader planes in clip space (public SetClipPlane docs). */
void hle_d3d9_metal_set_clip_planes(const float planes[6][4], uint32_t mask);

/* D3D9 indexed instancing: geometry in stream 0, per-instance attributes in
 * the stream marked D3DSTREAMSOURCE_INSTANCEDATA. */
void hle_d3d9_metal_set_stream_divisor(uint32_t stream, uint32_t divisor);
int  hle_d3d9_metal_draw_instanced(int prim_type, uint32_t start_vert, uint32_t vert_count,
                                   uint32_t instance_count, void *pipe, void *vb, uint32_t stride);
int  hle_d3d9_metal_draw_indexed_instanced(int prim_type, uint32_t base_vertex, uint32_t start_index,
                                           uint32_t index_count, uint32_t instance_count,
                                           void *pipe, void *vb, void *ib, uint32_t stride, int index32,
                                           uint32_t inst_stream, void *inst_vb, uint32_t inst_stride);

/* Gamma ramp for the implicit swap chain.  The ramp is applied by a real
 * Metal pass at Present into g_present_tex; no system display gamma is set. */
void hle_d3d9_metal_set_gamma_ramp(const uint16_t ramp[256][3]);
int  hle_d3d9_metal_read_present_pixels(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                        void *out, uint32_t out_bytes_per_row);

/* Windowed presentation (public D3D9 swap chain).
 *
 * hle_d3d9_metal_set_window() takes an SDL_Window*: the backend obtains its
 * CAMetalLayer with SDL_Metal_CreateView/SDL_Metal_GetLayer. With no real
 * display (SDL_VIDEODRIVER=dummy) or a hidden window, the backend does not
 * bind a layer and Present keeps the previous offscreen-only behaviour.
 * hle_d3d9_metal_set_layer() takes a CAMetalLayer* directly (void* here so C
 * guests need no Objective-C headers); it is useful for hosts that already
 * own a layer and for headless drawable tests. NULL disables display.
 * hle_d3d9_metal_create_layer()/release_layer() create a framebufferOnly=NO
 * CAMetalLayer for such tests; the returned handle is owned by the caller. */
void  hle_d3d9_metal_set_window(void *sdl_window);
void  hle_d3d9_metal_set_layer(void *cametal_layer);
void *hle_d3d9_metal_create_layer(void);
void  hle_d3d9_metal_release_layer(void *layer);

/* Parameters of the last IDirect3DDevice9::Present call. The device layer
 * stores the guest RECTs here before calling the backend present; a NULL rect
 * means "whole target". Kept as int[4] = (left, top, right, bottom). */
void hle_d3d9_metal_set_present_params(const int32_t src_rect[4],
                                       const int32_t dst_rect[4],
                                       uint32_t fullscreen);

/* Read BGRA8 pixels from the most recently presented CAMetalLayer drawable.
 * The layer is created with framebufferOnly=NO so getBytes is legal. Returns
 * -1 when no display layer/drawable is active. */
int  hle_d3d9_metal_read_drawable_pixels(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                         void *out, uint32_t out_bytes_per_row);

/* Render target selection. A NULL handle selects the device's backbuffer. */
void *hle_d3d9_metal_create_render_target(uint32_t width, uint32_t height);

/* MSAA render target with a real sampleCount and a resolve texture.
 * samples is the public D3DMULTISAMPLE_* count (2 or 4). */
void *hle_d3d9_metal_create_msaa_render_target(uint32_t width, uint32_t height, uint32_t samples);
void  hle_d3d9_metal_destroy_msaa_render_target(void *rt);
void  hle_d3d9_metal_resolve_msaa_render_target(void *rt);
void  hle_d3d9_metal_destroy_render_target(void *rt);
void  hle_d3d9_metal_set_render_target(void *rt);

/* Texture binding for stage 0 (used by the built-in textured pipeline). */
void hle_d3d9_metal_set_texture(void *tex);

/* Public Metal API: recommendedMaxWorkingSetSize of the active MTLDevice.
 * Returns 0 when no device is available. Used by GetAvailableTextureMem so
 * that value is measured, not a hard-coded constant. */
uint64_t hle_d3d9_metal_working_set_bytes(void);

/* Finish pending GPU work and read BGRA8 pixels from the current render target.
 * out_bytes_per_row must be at least w*4. Returns 0 on success, -1 on failure. */
int  hle_d3d9_metal_read_pixels(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                void *out, uint32_t out_bytes_per_row);
int  hle_d3d9_metal_read_render_target(void *rt, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                       void *out, uint32_t out_bytes_per_row);
int  hle_d3d9_metal_copy_texture_rect(void *src_tex, uint32_t sx, uint32_t sy,
                                      void *dst_tex, uint32_t dx, uint32_t dy,
                                      uint32_t w, uint32_t h);

/* IDirect3DQuery9 backend.
 *
 * Occlusion uses the real Metal visibility result buffer: Issue(BEGIN) opens
 * MTLVisibilityResultModeCounting at a per-query offset, the draws between
 * BEGIN and END increment that 64-bit counter for every sample passing
 * depth/stencil, and GetData reads the counter back after the GPU is flushed.
 * Events are bound to the completion of the command buffer that held the
 * commands up to Issue(END); GetData reports that buffer's real status.
 *
 * Returns NULL / -1 for an unsupported type or when Metal is not active.
 * get_data writes at most 4 bytes and returns 0 = data available,
 * 1 = not available yet (S_FALSE), -1 = error.
 */
void *hle_d3d9_metal_query_create(uint32_t type);
int   hle_d3d9_metal_query_issue(void *query, uint32_t issue_flags);
int   hle_d3d9_metal_query_get_data(void *query, void *out, uint32_t size, uint32_t flags);
void  hle_d3d9_metal_query_destroy(void *query);

#ifdef __cplusplus
}
#endif

#endif /* WIN32HLE_D3D9_METAL_H */
