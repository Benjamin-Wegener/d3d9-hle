#ifndef WIN32HLE_D3DX9_SHAPES_H
#define WIN32HLE_D3DX9_SHAPES_H

#include <stdint.h>
#include "d3d9_hle.h"

void s_D3DXCreateBox(void);
void s_D3DXCreateSphere(void);
void s_D3DXCreateCylinder(void);
void s_D3DXCreateTorus(void);
void s_D3DXComputeBoundingBox(void);
void s_D3DXComputeBoundingSphere(void);
void s_D3DXGetFVFVertexSize(void);
void s_D3DXDeclaratorFromFVF(void);

#endif /* WIN32HLE_D3DX9_SHAPES_H */
