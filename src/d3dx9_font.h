#ifndef WIN32HLE_D3DX9_FONT_H
#define WIN32HLE_D3DX9_FONT_H

#include <stdint.h>
#include "d3d9_hle.h"

/* D3DXFONT_DESCA (public layout, 56 bytes). */
typedef struct {
    int32_t  Height;
    uint32_t Width;
    uint32_t Weight;
    uint32_t MipLevels;
    int32_t  Italic;
    uint8_t  CharSet;
    uint8_t  OutputPrecision;
    uint8_t  Quality;
    uint8_t  PitchAndFamily;
    char     FaceName[32];
} D3DXFONT_DESCA;

/* DrawText format flags (subset). */
#define DT_TOP              0x00000000u
#define DT_LEFT             0x00000000u
#define DT_CENTER           0x00000001u
#define DT_RIGHT            0x00000002u
#define DT_VCENTER          0x00000004u
#define DT_BOTTOM           0x00000008u
#define DT_WORDBREAK        0x00000010u
#define DT_SINGLELINE       0x00000020u
#define DT_EXPANDTABS       0x00000040u
#define DT_TABSTOP          0x00000080u
#define DT_NOCLIP           0x00000100u
#define DT_CALCRECT         0x00000400u
#define DT_NOPREFIX         0x00000800u

/* D3DXCreateFontA/W (stdcall shims). */
void s_D3DXCreateFontA(void);
void s_D3DXCreateFontW(void);
void s_D3DXCreateFontIndirectA(void);
void s_D3DXCreateFontIndirectW(void);

/* ID3DXFont has exactly 18 methods in the public header order documented in
 * Wine's include/d3dx9core.h (ID3DXFontVtbl), slots 0..17:
 *   QueryInterface, AddRef, Release, GetDevice, GetDescA, GetDescW,
 *   GetTextMetricsA, GetTextMetricsW, GetDC, GetGlyphData,
 *   PreloadCharacters, PreloadGlyphs, PreloadTextA, PreloadTextW,
 *   DrawTextA, DrawTextW, OnLostDevice, OnResetDevice.
 * The object vtable must be built in this order; a guest calls by slot index. */
#define ID3DXFONT_SLOTS 18

void f_QueryInterface(void);
void f_AddRef(void);
void f_Release(void);
void f_GetDevice(void);
void f_GetDescA(void);
void f_GetDescW(void);
void f_GetTextMetricsA(void);
void f_GetTextMetricsW(void);
void f_GetDC(void);
void f_GetGlyphData(void);
void f_PreloadCharacters(void);
void f_PreloadGlyphs(void);
void f_PreloadTextA(void);
void f_PreloadTextW(void);
void f_DrawTextA(void);
void f_DrawTextW(void);
void f_OnLostDevice(void);
void f_OnResetDevice(void);

#endif /* WIN32HLE_D3DX9_FONT_H */
