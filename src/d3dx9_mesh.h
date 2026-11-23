/*
 * d3dx9_mesh.h - ID3DXMesh (d3dx9_mesh.c) and what the shape generators
 * (d3dx9_shapes.c) need from it.
 */
#ifndef WIN32HLE_D3DX9_MESH_H
#define WIN32HLE_D3DX9_MESH_H

#include <stdint.h>
#include "d3d9_hle.h"

#define D3DX9_MAX_FVF_DECL_SIZE 65   /* MAX_FVF_DECL_SIZE: 64 elements + D3DDECL_END */

/* D3DXCreateMeshFVF(NumFaces, NumVertices, Options, FVF, pDevice, ppMesh)
 * D3DXCreateMesh(NumFaces, NumVertices, Options, pDeclaration, pDevice, ppMesh) */
void s_D3DXCreateMeshFVF(void);
void s_D3DXCreateMesh(void);

/* ID3DXMesh methods: ID3DXBaseMesh slots 0-23, ID3DXMesh slots 24-28. */
void mesh_QueryInterface(void);
void mesh_AddRef(void);
void mesh_Release(void);
void mesh_DrawSubset(void);
void mesh_GetNumFaces(void);
void mesh_GetNumVertices(void);
void mesh_GetFVF(void);
void mesh_GetDeclaration(void);
void mesh_GetNumBytesPerVertex(void);
void mesh_GetOptions(void);
void mesh_GetDevice(void);
void mesh_CloneMeshFVF(void);
void mesh_CloneMesh(void);
void mesh_GetVertexBuffer(void);
void mesh_GetIndexBuffer(void);
void mesh_LockVertexBuffer(void);
void mesh_UnlockVertexBuffer(void);
void mesh_LockIndexBuffer(void);
void mesh_UnlockIndexBuffer(void);
void mesh_GetAttributeTable(void);
void mesh_ConvertPointRepsToAdjacency(void);
void mesh_ConvertAdjacencyToPointReps(void);
void mesh_GenerateAdjacency(void);
void mesh_UpdateSemantics(void);
void mesh_LockAttributeBuffer(void);
void mesh_UnlockAttributeBuffer(void);
void mesh_Optimize(void);
void mesh_OptimizeInplace(void);
void mesh_SetAttributeTable(void);

/* FVF helpers (host side). Both return 0 for an FVF this module does not
 * describe (no position, XYZB5, LASTBETA flags, XYZW, ...). */
uint32_t d3dx9_fvf_vertex_size(uint32_t fvf);
/* Writes the declaration for `fvf` plus D3DDECL_END into d[D3DX9_MAX_FVF_DECL_SIZE];
 * returns the element count without the end marker, 0 if the FVF is unsupported. */
uint32_t d3dx9_decl_from_fvf(uint32_t fvf, D3DVERTEXELEMENT9 *d);

/* For the shape generators: a mesh of 16-bit indices and the given FVF whose
 * vertex/index data they fill in. 0 on failure. */
uint32_t d3dx9_mesh_new_fvf(uint32_t dev, uint32_t fvf, uint32_t nfaces, uint32_t nverts);
void     d3dx9_mesh_data(uint32_t mesh, void **vertices, void **indices16);
/* ppAdjacency of the D3DXCreate* shape functions: an ID3DXBuffer with the
 * 3 * NumFaces adjacency DWORDs of `mesh` (vertices closer than epsilon are the
 * same point). Writes the buffer pointer to *ppAdjacency (guest address). */
void     d3dx9_mesh_write_adjacency(uint32_t mesh, uint32_t ppAdjacency);

#endif /* WIN32HLE_D3DX9_MESH_H */
