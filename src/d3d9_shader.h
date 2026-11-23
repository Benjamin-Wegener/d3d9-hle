/*
 * d3d9_shader.h - D3D9 bytecode to Metal Shading Language (MSL) translator.
 */
#ifndef WIN32HLE_D3D9_SHADER_H
#define WIN32HLE_D3D9_SHADER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Translates D3D9 token bytecode (vs_1_1 .. vs_3_0, ps_1_1 .. ps_3_0) into
 * standard Metal Shading Language (MSL) shader code.
 *
 * Parameters:
 *   tokens: Pointer to D3D9 bytecode tokens (starts with version token).
 *   token_count: Number of 32-bit tokens.
 *   is_pixel_shader: 1 for pixel shader, 0 for vertex shader.
 *   out_msl: Output buffer for null-terminated MSL source code.
 *   out_max: Size of out_msl buffer.
 *   err_msg: Output buffer for error messages (can be NULL).
 *   err_max: Size of err_msg buffer.
 *
 * Returns:
 *   0 on success, negative value on error.
 */
int d3d9_translate_shader_to_msl(const uint32_t *tokens,
                                uint32_t token_count,
                                int is_pixel_shader,
                                char *out_msl,
                                size_t out_max,
                                char *err_msg,
                                size_t err_max);

#ifdef __cplusplus
}
#endif

#endif /* WIN32HLE_D3D9_SHADER_H */
