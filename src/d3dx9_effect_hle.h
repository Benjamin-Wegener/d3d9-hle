#ifndef D3DX9_EFFECT_HLE_H
#define D3DX9_EFFECT_HLE_H

#include "win32hle.h"

/* First step of the compiled-effect framework: the shared parameter pool that
 * ID3DXEffect methods read and write. No d3d9 device and no D3DX math needed.
 * Register in the host (reported as FÜR HAUPT). */
extern const win32hle_shim win32hle_d3d9fx[];

/* Shared parameter pool */
uint32_t hle_d3d9fx_pool_values(uint32_t pool);   /* diagnostics: number of stored parameters */
uint32_t hle_d3d9fx_pool_param(uint32_t pool, const char *name, int create); /* handle, 0 if absent */
uint32_t hle_d3d9fx_pool_new(void);
/* ID3DXEffectPool has only the three IUnknown slots in d3dx9effect.h. Values
 * are written by the host that builds an effect, not through invented vtable
 * slots; these helpers take the handle returned by hle_d3d9fx_pool_param(). */
uint32_t hle_d3d9fx_param_set_float(uint32_t handle, float value);
uint32_t hle_d3d9fx_param_set_vector(uint32_t handle, const float *value);
uint32_t hle_d3d9fx_param_set_texture(uint32_t handle, uint32_t texture);
void     hle_d3d9fx_param_set_stage(uint32_t handle, uint32_t stage); /* sampler stage of a texture param */
uint32_t hle_d3d9fx_param_get_stage(uint32_t handle);
int      hle_d3d9fx_pool_apply(uint32_t pool, uint32_t dev, uint32_t vs_ct, uint32_t ps_ct);

/* Effect description + technique/pass state machine. A caller that builds an
 * effect from .fx text supplies the compiled shader bytecode and the two
 * ID3DXConstantTable objects so constant registers come from the compiler,
 * never from declaration order. */
uint32_t hle_d3d9fx_effect_new(uint32_t pool);
int      hle_d3d9fx_effect_add_tech(uint32_t effect, const char *name, const char **passes, int npass);
int      hle_d3d9fx_effect_add_tech_full(uint32_t effect, const char *name, const char **passes, int npass, const uint32_t *vs, const uint32_t *ps);
int      hle_d3d9fx_effect_add_tech_ct(uint32_t effect, const char *name, const char **passes, int npass,
                                       const uint32_t *vs, const uint32_t *ps,
                                       const uint32_t *vs_ct, const uint32_t *ps_ct);
/* kind: 0 = D3DRS render state, 1 = D3DSAMP sampler state on `stage`.
 * tech/pass: -1 = applies to any technique/pass (samplers). For pass render
 * states, pass is the pass index in the technique they belong to. */
int      hle_d3d9fx_effect_add_state(uint32_t effect, uint32_t kind, uint32_t stage,
                                     uint32_t state, uint32_t value, int tech, int pass);
uint32_t hle_d3d9fx_effect_set_device(uint32_t effect, uint32_t dev);
/* The device's shader objects of an effect: returns how many were made so far, *live
 * how many it still holds. They are made once per pass and freed with the effect. */
uint32_t hle_d3d9fx_effect_shader_stats(uint32_t effect, uint32_t *live);
void     hle_d3d9fx_effect_on_lost(uint32_t effect);   /* device lost: drop the shaders, BeginPass remakes them */
uint32_t hle_d3d9fx_effect_stats(uint32_t effect, uint32_t *commits); /* ntech<<16 | (pass+1)<<8 | begun */

#endif /* D3DX9_EFFECT_HLE_H */
