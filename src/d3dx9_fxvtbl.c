/*
 * d3dx9_fxvtbl.c - the public vtable layouts of ID3DXEffect and
 * ID3DXEffectCompiler (d3dx9effect.h: IUnknown, then ID3DXBaseEffect's 54
 * methods, then the interface's own), built from a list of the methods the
 * HLE implements. Every other slot is a real slot with the right stdcall
 * argument count that logs and reports "not supported" (E_NOTIMPL, or the
 * NULL handle / FALSE that means "not found" for methods returning a handle or
 * BOOL) -- never a made-up success.
 *
 * Order and signatures: public SDK documentation (ID3DXBaseEffect, ID3DXEffect,
 * ID3DXEffectCompiler) and Wine's include/d3dx9effect.h (read as a reference,
 * no code taken). ID3DXEffect: 79 slots (SDK >= 27 layout); ID3DXEffectCompiler: 61.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "d3dx9_fxvtbl.h"

#define E_NOTIMPL_ 0x80004001u

/* X(name, argc incl. this, kind): kind 0 = HRESULT, 1 = handle / BOOL (0 = not found) */
#define FX_BASE(X) \
    X(GetDesc, 2, 0) \
    X(GetParameterDesc, 3, 0) \
    X(GetTechniqueDesc, 3, 0) \
    X(GetPassDesc, 3, 0) \
    X(GetFunctionDesc, 3, 0) \
    X(GetParameter, 3, 1) \
    X(GetParameterByName, 3, 1) \
    X(GetParameterBySemantic, 3, 1) \
    X(GetParameterElement, 3, 1) \
    X(GetTechnique, 2, 1) \
    X(GetTechniqueByName, 2, 1) \
    X(GetPass, 3, 1) \
    X(GetPassByName, 3, 1) \
    X(GetFunction, 2, 1) \
    X(GetFunctionByName, 2, 1) \
    X(GetAnnotation, 3, 1) \
    X(GetAnnotationByName, 3, 1) \
    X(SetValue, 4, 0) \
    X(GetValue, 4, 0) \
    X(SetBool, 3, 0) \
    X(GetBool, 3, 0) \
    X(SetBoolArray, 4, 0) \
    X(GetBoolArray, 4, 0) \
    X(SetInt, 3, 0) \
    X(GetInt, 3, 0) \
    X(SetIntArray, 4, 0) \
    X(GetIntArray, 4, 0) \
    X(SetFloat, 3, 0) \
    X(GetFloat, 3, 0) \
    X(SetFloatArray, 4, 0) \
    X(GetFloatArray, 4, 0) \
    X(SetVector, 3, 0) \
    X(GetVector, 3, 0) \
    X(SetVectorArray, 4, 0) \
    X(GetVectorArray, 4, 0) \
    X(SetMatrix, 3, 0) \
    X(GetMatrix, 3, 0) \
    X(SetMatrixArray, 4, 0) \
    X(GetMatrixArray, 4, 0) \
    X(SetMatrixPointerArray, 4, 0) \
    X(GetMatrixPointerArray, 4, 0) \
    X(SetMatrixTranspose, 3, 0) \
    X(GetMatrixTranspose, 3, 0) \
    X(SetMatrixTransposeArray, 4, 0) \
    X(GetMatrixTransposeArray, 4, 0) \
    X(SetMatrixTransposePointerArray, 4, 0) \
    X(GetMatrixTransposePointerArray, 4, 0) \
    X(SetString, 3, 0) \
    X(GetString, 3, 0) \
    X(SetTexture, 3, 0) \
    X(GetTexture, 3, 0) \
    X(GetPixelShader, 3, 0) \
    X(GetVertexShader, 3, 0) \
    X(SetArrayRange, 4, 0)

#define FX_EFFECT(X) \
    X(GetPool, 2, 0) \
    X(SetTechnique, 2, 0) \
    X(GetCurrentTechnique, 1, 1) \
    X(ValidateTechnique, 2, 0) \
    X(FindNextValidTechnique, 3, 0) \
    X(IsParameterUsed, 3, 1) \
    X(Begin, 3, 0) \
    X(BeginPass, 2, 0) \
    X(CommitChanges, 1, 0) \
    X(EndPass, 1, 0) \
    X(End, 1, 0) \
    X(GetDevice, 2, 0) \
    X(OnLostDevice, 1, 0) \
    X(OnResetDevice, 1, 0) \
    X(SetStateManager, 2, 0) \
    X(GetStateManager, 2, 0) \
    X(BeginParameterBlock, 1, 0) \
    X(EndParameterBlock, 1, 1) \
    X(ApplyParameterBlock, 2, 0) \
    X(DeleteParameterBlock, 2, 0) \
    X(CloneEffect, 3, 0) \
    X(SetRawValue, 5, 0)

#define FX_COMPILER(X) \
    X(SetLiteral, 3, 0) \
    X(GetLiteral, 3, 0) \
    X(CompileEffect, 4, 0) \
    X(CompileShader, 7, 0)

#define X(name, argc, kind) \
    static void fxu_##name(void) { \
        fprintf(stderr, "[d3dx9] ID3DXEffect/ID3DXEffectCompiler::" #name ": not supported\n"); \
        RET((kind) ? 0u : E_NOTIMPL_, argc); \
    }
X(GetDesc, 2, 0)
X(GetParameterDesc, 3, 0)
X(GetTechniqueDesc, 3, 0)
X(GetPassDesc, 3, 0)
X(GetFunctionDesc, 3, 0)
X(GetParameter, 3, 1)
X(GetParameterByName, 3, 1)
X(GetParameterBySemantic, 3, 1)
X(GetParameterElement, 3, 1)
X(GetTechnique, 2, 1)
X(GetTechniqueByName, 2, 1)
X(GetPass, 3, 1)
X(GetPassByName, 3, 1)
X(GetFunction, 2, 1)
X(GetFunctionByName, 2, 1)
X(GetAnnotation, 3, 1)
X(GetAnnotationByName, 3, 1)
X(SetValue, 4, 0)
X(GetValue, 4, 0)
X(SetBool, 3, 0)
X(GetBool, 3, 0)
X(SetBoolArray, 4, 0)
X(GetBoolArray, 4, 0)
X(SetInt, 3, 0)
X(GetInt, 3, 0)
X(SetIntArray, 4, 0)
X(GetIntArray, 4, 0)
X(SetFloat, 3, 0)
X(GetFloat, 3, 0)
X(SetFloatArray, 4, 0)
X(GetFloatArray, 4, 0)
X(SetVector, 3, 0)
X(GetVector, 3, 0)
X(SetVectorArray, 4, 0)
X(GetVectorArray, 4, 0)
X(SetMatrix, 3, 0)
X(GetMatrix, 3, 0)
X(SetMatrixArray, 4, 0)
X(GetMatrixArray, 4, 0)
X(SetMatrixPointerArray, 4, 0)
X(GetMatrixPointerArray, 4, 0)
X(SetMatrixTranspose, 3, 0)
X(GetMatrixTranspose, 3, 0)
X(SetMatrixTransposeArray, 4, 0)
X(GetMatrixTransposeArray, 4, 0)
X(SetMatrixTransposePointerArray, 4, 0)
X(GetMatrixTransposePointerArray, 4, 0)
X(SetString, 3, 0)
X(GetString, 3, 0)
X(SetTexture, 3, 0)
X(GetTexture, 3, 0)
X(GetPixelShader, 3, 0)
X(GetVertexShader, 3, 0)
X(SetArrayRange, 4, 0)
X(GetPool, 2, 0)
X(SetTechnique, 2, 0)
X(GetCurrentTechnique, 1, 1)
X(ValidateTechnique, 2, 0)
X(FindNextValidTechnique, 3, 0)
X(IsParameterUsed, 3, 1)
X(Begin, 3, 0)
X(BeginPass, 2, 0)
X(CommitChanges, 1, 0)
X(EndPass, 1, 0)
X(End, 1, 0)
X(GetDevice, 2, 0)
X(OnLostDevice, 1, 0)
X(OnResetDevice, 1, 0)
X(SetStateManager, 2, 0)
X(GetStateManager, 2, 0)
X(BeginParameterBlock, 1, 0)
X(EndParameterBlock, 1, 1)
X(ApplyParameterBlock, 2, 0)
X(DeleteParameterBlock, 2, 0)
X(CloneEffect, 3, 0)
X(SetRawValue, 5, 0)
X(SetLiteral, 3, 0)
X(GetLiteral, 3, 0)
X(CompileEffect, 4, 0)
X(CompileShader, 7, 0)
#undef X

typedef struct { const char *name; recomp_func_t fn; } stub_t;
static const stub_t STUBS[] = {
    { "GetDesc", fxu_GetDesc },
    { "GetParameterDesc", fxu_GetParameterDesc },
    { "GetTechniqueDesc", fxu_GetTechniqueDesc },
    { "GetPassDesc", fxu_GetPassDesc },
    { "GetFunctionDesc", fxu_GetFunctionDesc },
    { "GetParameter", fxu_GetParameter },
    { "GetParameterByName", fxu_GetParameterByName },
    { "GetParameterBySemantic", fxu_GetParameterBySemantic },
    { "GetParameterElement", fxu_GetParameterElement },
    { "GetTechnique", fxu_GetTechnique },
    { "GetTechniqueByName", fxu_GetTechniqueByName },
    { "GetPass", fxu_GetPass },
    { "GetPassByName", fxu_GetPassByName },
    { "GetFunction", fxu_GetFunction },
    { "GetFunctionByName", fxu_GetFunctionByName },
    { "GetAnnotation", fxu_GetAnnotation },
    { "GetAnnotationByName", fxu_GetAnnotationByName },
    { "SetValue", fxu_SetValue },
    { "GetValue", fxu_GetValue },
    { "SetBool", fxu_SetBool },
    { "GetBool", fxu_GetBool },
    { "SetBoolArray", fxu_SetBoolArray },
    { "GetBoolArray", fxu_GetBoolArray },
    { "SetInt", fxu_SetInt },
    { "GetInt", fxu_GetInt },
    { "SetIntArray", fxu_SetIntArray },
    { "GetIntArray", fxu_GetIntArray },
    { "SetFloat", fxu_SetFloat },
    { "GetFloat", fxu_GetFloat },
    { "SetFloatArray", fxu_SetFloatArray },
    { "GetFloatArray", fxu_GetFloatArray },
    { "SetVector", fxu_SetVector },
    { "GetVector", fxu_GetVector },
    { "SetVectorArray", fxu_SetVectorArray },
    { "GetVectorArray", fxu_GetVectorArray },
    { "SetMatrix", fxu_SetMatrix },
    { "GetMatrix", fxu_GetMatrix },
    { "SetMatrixArray", fxu_SetMatrixArray },
    { "GetMatrixArray", fxu_GetMatrixArray },
    { "SetMatrixPointerArray", fxu_SetMatrixPointerArray },
    { "GetMatrixPointerArray", fxu_GetMatrixPointerArray },
    { "SetMatrixTranspose", fxu_SetMatrixTranspose },
    { "GetMatrixTranspose", fxu_GetMatrixTranspose },
    { "SetMatrixTransposeArray", fxu_SetMatrixTransposeArray },
    { "GetMatrixTransposeArray", fxu_GetMatrixTransposeArray },
    { "SetMatrixTransposePointerArray", fxu_SetMatrixTransposePointerArray },
    { "GetMatrixTransposePointerArray", fxu_GetMatrixTransposePointerArray },
    { "SetString", fxu_SetString },
    { "GetString", fxu_GetString },
    { "SetTexture", fxu_SetTexture },
    { "GetTexture", fxu_GetTexture },
    { "GetPixelShader", fxu_GetPixelShader },
    { "GetVertexShader", fxu_GetVertexShader },
    { "SetArrayRange", fxu_SetArrayRange },
    { "GetPool", fxu_GetPool },
    { "SetTechnique", fxu_SetTechnique },
    { "GetCurrentTechnique", fxu_GetCurrentTechnique },
    { "ValidateTechnique", fxu_ValidateTechnique },
    { "FindNextValidTechnique", fxu_FindNextValidTechnique },
    { "IsParameterUsed", fxu_IsParameterUsed },
    { "Begin", fxu_Begin },
    { "BeginPass", fxu_BeginPass },
    { "CommitChanges", fxu_CommitChanges },
    { "EndPass", fxu_EndPass },
    { "End", fxu_End },
    { "GetDevice", fxu_GetDevice },
    { "OnLostDevice", fxu_OnLostDevice },
    { "OnResetDevice", fxu_OnResetDevice },
    { "SetStateManager", fxu_SetStateManager },
    { "GetStateManager", fxu_GetStateManager },
    { "BeginParameterBlock", fxu_BeginParameterBlock },
    { "EndParameterBlock", fxu_EndParameterBlock },
    { "ApplyParameterBlock", fxu_ApplyParameterBlock },
    { "DeleteParameterBlock", fxu_DeleteParameterBlock },
    { "CloneEffect", fxu_CloneEffect },
    { "SetRawValue", fxu_SetRawValue },
    { "SetLiteral", fxu_SetLiteral },
    { "GetLiteral", fxu_GetLiteral },
    { "CompileEffect", fxu_CompileEffect },
    { "CompileShader", fxu_CompileShader },
    { 0, 0 }
};

static recomp_func_t stub_for(const char *name) {
    for (const stub_t *s = STUBS; s->name; s++) if (!strcmp(s->name, name)) return s->fn;
    return 0;
}

static const char *const BASE_NAMES[] = {
    "GetDesc",
    "GetParameterDesc",
    "GetTechniqueDesc",
    "GetPassDesc",
    "GetFunctionDesc",
    "GetParameter",
    "GetParameterByName",
    "GetParameterBySemantic",
    "GetParameterElement",
    "GetTechnique",
    "GetTechniqueByName",
    "GetPass",
    "GetPassByName",
    "GetFunction",
    "GetFunctionByName",
    "GetAnnotation",
    "GetAnnotationByName",
    "SetValue",
    "GetValue",
    "SetBool",
    "GetBool",
    "SetBoolArray",
    "GetBoolArray",
    "SetInt",
    "GetInt",
    "SetIntArray",
    "GetIntArray",
    "SetFloat",
    "GetFloat",
    "SetFloatArray",
    "GetFloatArray",
    "SetVector",
    "GetVector",
    "SetVectorArray",
    "GetVectorArray",
    "SetMatrix",
    "GetMatrix",
    "SetMatrixArray",
    "GetMatrixArray",
    "SetMatrixPointerArray",
    "GetMatrixPointerArray",
    "SetMatrixTranspose",
    "GetMatrixTranspose",
    "SetMatrixTransposeArray",
    "GetMatrixTransposeArray",
    "SetMatrixTransposePointerArray",
    "GetMatrixTransposePointerArray",
    "SetString",
    "GetString",
    "SetTexture",
    "GetTexture",
    "GetPixelShader",
    "GetVertexShader",
    "SetArrayRange",
    0
};
static const char *const EFFECT_NAMES[] = {
    "GetPool",
    "SetTechnique",
    "GetCurrentTechnique",
    "ValidateTechnique",
    "FindNextValidTechnique",
    "IsParameterUsed",
    "Begin",
    "BeginPass",
    "CommitChanges",
    "EndPass",
    "End",
    "GetDevice",
    "OnLostDevice",
    "OnResetDevice",
    "SetStateManager",
    "GetStateManager",
    "BeginParameterBlock",
    "EndParameterBlock",
    "ApplyParameterBlock",
    "DeleteParameterBlock",
    "CloneEffect",
    "SetRawValue",
    0
};
static const char *const COMPILER_NAMES[] = {
    "SetLiteral",
    "GetLiteral",
    "CompileEffect",
    "CompileShader",
    0
};

/* impl: {unprefixed method name, function} for IUnknown's three and every method
 * the caller implements. */
uint32_t d3dx9_fx_vtable(const char *iface, int which, const win32hle_shim *impl) {
    static const char *const IUNK[] = { "QueryInterface", "AddRef", "Release", 0 };
    win32hle_shim *tab = (win32hle_shim *)calloc(1 + 3 + 54 + 22, sizeof *tab);
    char *nm;
    int n = 0;
    if (!tab) return 0;
    const char *const *lists[3] = { IUNK, BASE_NAMES, which == 0 ? EFFECT_NAMES : COMPILER_NAMES };
    for (int l = 0; l < 3; l++)
        for (const char *const *p = lists[l]; *p; p++) {
            recomp_func_t fn = 0;
            for (const win32hle_shim *s = impl; s->name; s++) if (!strcmp(s->name, *p)) { fn = s->fn; break; }
            if (!fn) fn = stub_for(*p);
            if (!fn) { free(tab); return 0; }
            nm = (char *)malloc(strlen(iface) + strlen(*p) + 3);
            if (!nm) { free(tab); return 0; }
            sprintf(nm, "%s::%s", iface, *p);
            tab[n].name = nm; tab[n].fn = fn; n++;
        }
    uint32_t vt = hle_com_vtable(tab);
    free(tab);                       /* the vtable keeps its own copy; the name strings stay registered */
    return vt;
}
