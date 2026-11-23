#ifndef WIN32HLE_D3DX9_MATRIXSTACK_H
#define WIN32HLE_D3DX9_MATRIXSTACK_H

#include <stdint.h>
#include "d3d9_hle.h"

void s_D3DXCreateMatrixStack(void);

/* The 18 ID3DXMatrixStack methods (IUnknown + the 15 documented methods).
 * OnLostDevice/OnResetDevice are deliberately absent: they are not methods
 * of ID3DXMatrixStack. */
void ms_QueryInterface(void); void ms_AddRef(void); void ms_Release(void);
void ms_Push(void); void ms_Pop(void); void ms_GetTop(void);
void ms_LoadIdentity(void); void ms_LoadMatrix(void);
void ms_MultMatrix(void); void ms_MultMatrixLocal(void);
void ms_RotateAxis(void); void ms_RotateAxisLocal(void);
void ms_RotateYawPitchRoll(void); void ms_RotateYawPitchRollLocal(void);
void ms_Scale(void); void ms_ScaleLocal(void);
void ms_Translate(void); void ms_TranslateLocal(void);

#endif /* WIN32HLE_D3DX9_MATRIXSTACK_H */
