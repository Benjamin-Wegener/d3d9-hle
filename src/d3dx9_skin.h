/*
 * d3dx9_skin.h - public ABI of D3DXCreateSkinInfo/CreateSkinInfoFVF and
 * ID3DXSkinInfo (d3dx9_skin.c).
 *
 * The method ORDER, signatures, slot count and structure sizes below are
 * taken ONLY from public documentation / Wine's public headers and source:
 *   https://raw.githubusercontent.com/wine-mirror/wine/master/include/d3dx9mesh.h
 *   https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/d3dx9_36/skin.c
 *   https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/d3dx9_36/d3dx9_36.spec
 * No Microsoft DLL or binary was consulted; only order/signatures are copied.
 * The slot count is part of the contract and is checked by the selftest.
 */
#ifndef WIN32HLE_D3DX9_SKIN_H
#define WIN32HLE_D3DX9_SKIN_H

#include <stdint.h>
#include "d3d9_hle.h"

/* 27 slots (0..26), exactly the Wine ID3DXSkinInfoVtbl order. */
#define ID3DXSKININFO_SLOTS 27

/* MAX_FVF_DECL_SIZE: 64 vertex elements + the D3DDECL_END marker. */
#define D3DX9_SKIN_DECL_SIZE 65u

/* D3DXCreate* stdcall shims. */
void s_D3DXCreateSkinInfo(void);
void s_D3DXCreateSkinInfoFVF(void);

/* ID3DXSkinInfo, slots 0..26. */
void si_QueryInterface(void);
void si_AddRef(void);
void si_Release(void);
void si_SetBoneInfluence(void);
void si_SetBoneVertexInfluence(void);
void si_GetNumBoneInfluences(void);
void si_GetBoneInfluence(void);
void si_GetBoneVertexInfluence(void);
void si_GetMaxVertexInfluences(void);
void si_GetNumBones(void);
void si_FindBoneVertexInfluenceIndex(void);
void si_GetMaxFaceInfluences(void);
void si_SetMinBoneInfluence(void);
void si_GetMinBoneInfluence(void);
void si_SetBoneName(void);
void si_GetBoneName(void);
void si_SetBoneOffsetMatrix(void);
void si_GetBoneOffsetMatrix(void);
void si_Clone(void);
void si_Remap(void);
void si_SetFVF(void);
void si_SetDeclaration(void);
void si_GetFVF(void);
void si_GetDeclaration(void);
void si_UpdateSkinnedMesh(void);
void si_ConvertToBlendedMesh(void);
void si_ConvertToIndexedBlendedMesh(void);

#endif /* WIN32HLE_D3DX9_SKIN_H */
