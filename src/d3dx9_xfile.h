#ifndef WIN32HLE_D3DX9_XFILE_H
#define WIN32HLE_D3DX9_XFILE_H

#include <stdint.h>
#include <stddef.h>

/* D3DXLoadMeshFromXA/W/InMemory (public D3DX9 API). */
void s_D3DXLoadMeshFromXA(void);
void s_D3DXLoadMeshFromXW(void);
void s_D3DXLoadMeshFromXInMemory(void);

/* D3DXLoadMeshHierarchyFromXA/W/InMemory and D3DXFrameDestroy. */
void s_D3DXLoadMeshHierarchyFromXA(void);
void s_D3DXLoadMeshHierarchyFromXW(void);
void s_D3DXLoadMeshHierarchyFromXInMemory(void);
void s_D3DXFrameDestroy(void);

/* Host-only parse check for sanitizer robustness tests (no device, no SDL). */
int d3dx9_xfile_parse_check(const char *data, size_t size);

#endif /* WIN32HLE_D3DX9_XFILE_H */
