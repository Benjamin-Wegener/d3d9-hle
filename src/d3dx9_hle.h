/*
 * d3dx9_hle.h - the host-facing exports of the D3DX9 module (d3dx9.c and its
 * companions d3dx9_math.c / d3dx9_effect.c): the shim table the main session
 * registers with win32hle_register().
 *
 * Every d3dx9_*.dll (d3dx9_24.dll through d3dx9_43.dll) exports the same
 * names, so they are registered without a DLL prefix, exactly as a program's
 * import table names them.
 */
#ifndef WIN32HLE_D3DX9_H
#define WIN32HLE_D3DX9_H

#include "win32hle.h"

/* The module's shim table (d3dx9.c). */
extern const win32hle_shim win32hle_d3dx9[];

#endif /* WIN32HLE_D3DX9_H */
