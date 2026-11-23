#ifndef D3DX9_FXBIN_HLE_H
#define D3DX9_FXBIN_HLE_H

#include "win32hle.h"
#include "d3dx9_effect_hle.h"

/* D3DX9 compiled-effect container, reconstructed from the shipped d3dx9_34.dll
 * parser (D3DXCreateEffectEx -> FUN_00544a19 and its record readers). This is an
 * independent implementation derived from observed behaviour, not copied code.
 *
 * All fields are little-endian DWORDs; offsets are BYTE offsets into the CTAB
 * blob. Strings there are NUL-terminated ASCII.
 *
 *   [0] token = 0xFEFF0901
 *   [1] ctab_size in bytes, followed by ceil(ctab_size/4) DWORDs of CTAB data
 *   [..] param_count, technique_count, object_count, group_count
 *
 *   param[param_count]:
 *       name, semantic, type_flags, annotation_count
 *       annotation[annotation_count]: name, value
 *   technique[technique_count]:
 *       name, annotation_count, pass_count
 *       annotation[annotation_count]: name, value
 *       pass[pass_count]:
 *           name, annotation_count, state_count
 *           annotation[annotation_count]: name, value
 *           state[state_count]: state_type, value_type, name, value   (4 DWORDs)
 *
 * The XNA4 wrapper token carries a DWORD offset to the payload. Object and
 * group records are not needed for technique/pass discovery and are skipped.
 */
#define D3DXFX_TOKEN        0xFEFF0901u
#define D3DXFX_TOKEN_XNA4   0xBCF00BCFu
#define D3DXFX_MAX_PARAM    16
#define D3DXFX_MAX_TECH     8
#define D3DXFX_MAX_PASS     16
#define D3DXFX_NAME_LEN     64

typedef struct {
    char param[D3DXFX_MAX_PARAM][D3DXFX_NAME_LEN];
    uint32_t nparam;
    char tech[D3DXFX_MAX_TECH][D3DXFX_NAME_LEN];
    char pass[D3DXFX_MAX_TECH][D3DXFX_MAX_PASS][D3DXFX_NAME_LEN];
    uint32_t npass[D3DXFX_MAX_TECH];
    uint32_t ntech;
} fxbin_desc_t;

/* 1 on a well-formed blob whose offsets stay inside the buffer. */
int      hle_d3d9fxbin_parse(const uint8_t *data, uint32_t size, fxbin_desc_t *out);
uint32_t hle_d3d9fx_effect_from_desc(uint32_t pool, const fxbin_desc_t *desc);
extern const win32hle_shim win32hle_d3d9fxbin[];

#endif /* D3DX9_FXBIN_HLE_H */
