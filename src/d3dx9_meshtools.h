#ifndef WIN32HLE_D3DX9_MESHTOOLS_H
#define WIN32HLE_D3DX9_MESHTOOLS_H

/* D3DX mesh utility functions (public D3DX9 API). */
void s_D3DXComputeNormals(void);
void s_D3DXWeldVertices(void);
void s_D3DXOptimizeFaces(void);
void s_D3DXOptimizeVertices(void);
void s_D3DXIntersect(void);
void s_D3DXIntersectTri(void);
void s_D3DXSphereBoundProbe(void);
void s_D3DXBoxBoundProbe(void);
void s_D3DXCreatePolygon(void);

#endif /* WIN32HLE_D3DX9_MESHTOOLS_H */
