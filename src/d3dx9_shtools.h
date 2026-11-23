/* Public D3DX9 shader tools implemented with our own assembler/disassembler. */
#ifndef WIN32HLE_D3DX9_SHTOOLS_H
#define WIN32HLE_D3DX9_SHTOOLS_H

#include <stdint.h>
#include <stddef.h>

/* Disassemble legacy D3D9 token words to assembly text. Returns 0 on success
 * and malloc's *out (caller frees); on failure returns -1 with err[] filled. */
int d3dx9_disassemble_text(const uint32_t *words, uint32_t count,
                           char **out, char *err, size_t err_max);

/* Host helpers used by the stdcall entry points. */
uint32_t d3dx9_shader_profile(uint32_t dev, int pixel);

void s_D3DXDisassembleShader(void);
void s_D3DXGetVertexShaderProfile(void);
void s_D3DXGetPixelShaderProfile(void);
void s_D3DXAssembleShaderFromFileA(void);
void s_D3DXAssembleShaderFromFileW(void);
void s_D3DXPreprocessShaderFromFileA(void);

#endif
