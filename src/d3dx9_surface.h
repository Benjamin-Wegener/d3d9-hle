/*
 * d3dx9_surface.h - D3DX9 surface helpers: copies between surfaces and
 * memory, mipmap filtering and BMP/DDS saving.
 *
 * Constants and stdcall argument positions are the public ones from
 * D3dx9tex.h / D3dx9.h (verified against Wine's headers). The image file
 * format values come from d3dx9_texture.h so the two modules agree:
 * D3DXIFF_BMP = 0 and D3DXIFF_DDS = 4 (1 is JPG, not DDS).
 *
 * Everything this module does not support is an error with a log line,
 * never a silent OK: palettes, unsupported pixel formats and file formats.
 */
#ifndef WIN32HLE_D3DX9_SURFACE_H
#define WIN32HLE_D3DX9_SURFACE_H

#include <stdint.h>
#include "d3dx9_texture.h"   /* D3DXIFF_*, D3DX_DEFAULT = 0xFFFFFFFF */

/* D3DX_FILTER (D3dx9tex.h). D3DX_DEFAULT is the "choose for me" value and
 * lives in d3dx9_texture.h; there is no D3DX_FILTER_DEFAULT constant. */
#define D3DX_FILTER_NONE      0x00000001u
#define D3DX_FILTER_POINT     0x00000002u
#define D3DX_FILTER_LINEAR    0x00000003u
#define D3DX_FILTER_TRIANGLE  0x00000004u
#define D3DX_FILTER_BOX       0x00000005u

/* The stdcall entry points, registered in d3dx9.c. Declared here so the
 * selftest can enter them directly and check their callee-side stack purge. */
void s_D3DXLoadSurfaceFromMemory(void);
void s_D3DXLoadSurfaceFromFileA(void);
void s_D3DXLoadSurfaceFromFileInMemory(void);
void s_D3DXLoadVolumeFromMemory(void);
void s_D3DXLoadSurfaceFromSurface(void);
void s_D3DXFilterTexture(void);
void s_D3DXSaveSurfaceToFileA(void);

#endif /* WIN32HLE_D3DX9_SURFACE_H */
