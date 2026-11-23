/* D3DXCreateRenderToSurface and ID3DXRenderToSurface (d3dx9core.h). */
#ifndef WIN32HLE_D3DX9_RTS_H
#define WIN32HLE_D3DX9_RTS_H

void s_D3DXCreateRenderToSurface(void);
void dxr_QueryInterface(void);
void dxr_AddRef(void);
void dxr_Release(void);
void dxr_GetDevice(void);
void dxr_GetDesc(void);
void dxr_BeginScene(void);
void dxr_EndScene(void);
void dxr_OnLostDevice(void);
void dxr_OnResetDevice(void);

#endif
