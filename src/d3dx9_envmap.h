/* D3DXCreateRenderToEnvMap and ID3DXRenderToEnvMap (d3dx9core.h). */
#ifndef WIN32HLE_D3DX9_ENVMAP_H
#define WIN32HLE_D3DX9_ENVMAP_H

void s_D3DXCreateRenderToEnvMap(void);

void dxe_QueryInterface(void);
void dxe_AddRef(void);
void dxe_Release(void);
void dxe_GetDevice(void);
void dxe_GetDesc(void);
void dxe_BeginCube(void);
void dxe_BeginSphere(void);
void dxe_BeginHemisphere(void);
void dxe_BeginParabolic(void);
void dxe_Face(void);
void dxe_End(void);
void dxe_OnLostDevice(void);
void dxe_OnResetDevice(void);

#endif
