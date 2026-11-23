/*
 * d3dx9_asm.h - D3DXAssembleShader: D3D9 shader assembly text to bytecode.
 *
 * The emitted tokens follow Microsoft's public d3d9types.h layout and are
 * accepted by d3d9_shader.c (the D3D9 bytecode -> MSL translator). No external
 * assembler code or Microsoft binary is used.
 */
#ifndef WIN32HLE_D3DX9_ASM_H
#define WIN32HLE_D3DX9_ASM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Assemble `len` bytes of shader source into D3D9 bytecode.
 * Returns 0 on success and fills *out_words / *out_count (caller frees with
 * free()). On failure returns -1, *out_words stays NULL and err[] holds a
 * message with the 1-based source line number. */
int d3dx9_assemble(const char *src, uint32_t len,
                   uint32_t **out_words, uint32_t *out_count,
                   char *err, size_t err_max);

/* Wrap bytes in a guest ID3DXBuffer (used by the shader tool entry points). */
uint32_t d3dx9_asm_make_buffer(const void *data, size_t size);

/* HLE stdcall shim: 7 args (data, len, defines, include, flags,
 * ppShader, ppErrorMsgs), per public d3dx9shader.h. */
void s_D3DXAssembleShader(void);
void asmbuf_get_ptr(void);
void asmbuf_get_size(void);
void asmbuf_release(void);

/* All shims are registered from win32hle_d3dx9[] in d3dx9.c. */

#ifdef __cplusplus
}
#endif

#endif /* WIN32HLE_D3DX9_ASM_H */
