/*
 * d3dx9_misc.h - Declarations for miscellaneous D3DX9 utilities:
 * D3DXCheckVersion, D3DXDebugMute, and file-based effect creation shims.
 *
 * Source: Public Microsoft DirectX 9 SDK headers (d3dx9core.h, d3dx9effect.h)
 * and Wine (dlls/d3dx9_36/d3dx9_36_main.c, dlls/d3dx9_36/effect.c).
 */
#ifndef HLE_D3DX9_MISC_H
#define HLE_D3DX9_MISC_H

#include "win32hle.h"

#define D3D_SDK_VERSION   32u
#define D3DX_SDK_VERSION  43u

void s_D3DXCheckVersion(void);
void s_D3DXDebugMute(void);
void s_D3DXCreateEffectFromFileExA(void);
void s_D3DXCreateEffectFromFileExW(void);
void s_D3DXCreateEffectFromFileW(void);

#endif /* HLE_D3DX9_MISC_H */
