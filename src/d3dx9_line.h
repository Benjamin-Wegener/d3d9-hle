#ifndef WIN32HLE_D3DX9_LINE_H
#define WIN32HLE_D3DX9_LINE_H

#include <stdint.h>
#include "d3d9_hle.h"

typedef struct { float x, y; } D3DXVECTOR2;
typedef struct { float x, y, z; } D3DXVECTOR3;
typedef struct { float _11,_12,_13,_14, _21,_22,_23,_24, _31,_32,_33,_34, _41,_42,_43,_44; } D3DXMATRIX;

void s_D3DXCreateLine(void);
void l_QueryInterface(void); void l_AddRef(void); void l_Release(void);
void l_GetDevice(void);
void l_Begin(void); void l_Draw(void); void l_DrawTransform(void); void l_End(void);
void l_SetPattern(void); void l_GetPattern(void);
void l_SetPatternScale(void); void l_GetPatternScale(void);
void l_SetWidth(void); void l_GetWidth(void);
void l_SetAntialias(void); void l_GetAntialias(void);
void l_SetGLLines(void); void l_GetGLLines(void);
void l_OnLostDevice(void); void l_OnResetDevice(void);

#endif /* WIN32HLE_D3DX9_LINE_H */
