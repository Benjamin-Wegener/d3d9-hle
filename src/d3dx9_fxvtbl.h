#ifndef WIN32HLE_D3DX9_FXVTBL_H
#define WIN32HLE_D3DX9_FXVTBL_H
#include <stdint.h>
#include "win32hle.h"
/* Vtable of ID3DXEffect (which = 0, 79 slots) or ID3DXEffectCompiler (which = 1, 61 slots);
 * `impl` is a {0,0}-terminated list of {unprefixed method name, fn}. */
uint32_t d3dx9_fx_vtable(const char *iface, int which, const win32hle_shim *impl);
#endif
