#include "d3d9_internal.h"

/* The GPU renderer operations (Metal on macOS, else software/headless) */
const d3d9_gpu_ops_t *hle_d3d9_gpu = NULL;
void hle_d3d9_set_gpu(const d3d9_gpu_ops_t *ops) {
    hle_d3d9_gpu = ops;
}

/* Global vtable IDs */
uint32_t g_vt_d3d9;
uint32_t g_vt_device9;
uint32_t g_vt_surface9;
uint32_t g_vt_tex9;
uint32_t g_vt_cubetex9;
uint32_t g_vt_vb9;
uint32_t g_vt_ib9;
uint32_t g_vt_decl9;
uint32_t g_vt_vs9;
uint32_t g_vt_ps9;
uint32_t g_vt_query9;
uint32_t g_vt_sb9;
uint32_t g_vt_swapchain9;
uint32_t g_vt_volumetex9;
uint32_t g_vt_volume9;

/* =========================================================================
 * Vtable Setup
 * ========================================================================= */

void init_d3d9_vtables(void) {
    if (g_vt_d3d9) return;

    /* IDirect3D9 (17 methods) */
    static const win32hle_shim s_d3d9_methods[] = {
        { "IDirect3D9::QueryInterface", d3d_QueryInterface },
        { "IDirect3D9::AddRef", hle_com_AddRef },
        { "IDirect3D9::Release", hle_com_Release },
        { "IDirect3D9::RegisterSoftwareDevice", d9_RegisterSoftwareDevice },
        { "IDirect3D9::GetAdapterCount", d9_GetAdapterCount },
        { "IDirect3D9::GetAdapterIdentifier", d9_GetAdapterIdentifier },
        { "IDirect3D9::GetAdapterModeCount", d9_GetAdapterModeCount },
        { "IDirect3D9::EnumAdapterModes", d9_EnumAdapterModes },
        { "IDirect3D9::GetAdapterDisplayMode", d9_GetAdapterDisplayMode },
        { "IDirect3D9::CheckDeviceType", d9_CheckDeviceType },
        { "IDirect3D9::CheckDeviceFormat", d9_CheckDeviceFormat },
        { "IDirect3D9::CheckDeviceMultiSampleType", d9_CheckDeviceMultiSampleType },
        { "IDirect3D9::CheckDepthStencilMatch", d9_CheckDepthStencilMatch },
        { "IDirect3D9::CheckDeviceFormatConversion", d9_CheckDeviceFormatConversion },
        { "IDirect3D9::GetDeviceCaps", d9_GetDeviceCaps },
        { "IDirect3D9::GetAdapterMonitor", d9_GetAdapterMonitor },
        { "IDirect3D9::CreateDevice", d9_CreateDevice },
        { 0, 0 }
    };
    g_vt_d3d9 = hle_com_vtable(s_d3d9_methods);

    /* IDirect3DDevice9 (119 methods) */
    static const win32hle_shim s_dev9_methods[] = {
        { "IDirect3DDevice9::QueryInterface", d3d_QueryInterface },
        { "IDirect3DDevice9::AddRef", hle_com_AddRef },
        { "IDirect3DDevice9::Release", dev9_Release },
        { "IDirect3DDevice9::TestCooperativeLevel", dev9_TestCooperativeLevel },
        { "IDirect3DDevice9::GetAvailableTextureMem", dev9_GetAvailableTextureMem },
        { "IDirect3DDevice9::EvictManagedResources", dev9_EvictManagedResources },
        { "IDirect3DDevice9::GetDirect3D", dev9_GetDirect3D },
        { "IDirect3DDevice9::GetDeviceCaps", dev9_GetDeviceCaps },
        { "IDirect3DDevice9::GetDisplayMode", dev9_GetDisplayMode },
        { "IDirect3DDevice9::GetCreationParameters", dev9_GetCreationParameters },
        { "IDirect3DDevice9::SetCursorProperties", dev9_SetCursorProperties },
        { "IDirect3DDevice9::SetCursorPosition", dev9_SetCursorPosition },
        { "IDirect3DDevice9::ShowCursor", dev9_ShowCursor },
        { "IDirect3DDevice9::CreateAdditionalSwapChain", dev9_CreateAdditionalSwapChain },
        { "IDirect3DDevice9::GetSwapChain", dev9_GetSwapChain },
        { "IDirect3DDevice9::GetNumberOfSwapChains", dev9_GetNumberOfSwapChains },
        { "IDirect3DDevice9::Reset", dev9_Reset },
        { "IDirect3DDevice9::Present", dev9_Present },
        { "IDirect3DDevice9::GetBackBuffer", dev9_GetBackBuffer },
        { "IDirect3DDevice9::GetRasterStatus", dev9_GetRasterStatus },
        { "IDirect3DDevice9::SetDialogBoxMode", dev9_SetDialogBoxMode },
        { "IDirect3DDevice9::SetGammaRamp", dev9_SetGammaRamp },
        { "IDirect3DDevice9::GetGammaRamp", dev9_GetGammaRamp },
        { "IDirect3DDevice9::CreateTexture", dev9_CreateTexture },
        { "IDirect3DDevice9::CreateVolumeTexture", dev9_CreateVolumeTexture },
        { "IDirect3DDevice9::CreateCubeTexture", dev9_CreateCubeTexture },
        { "IDirect3DDevice9::CreateVertexBuffer", dev9_CreateVertexBuffer },
        { "IDirect3DDevice9::CreateIndexBuffer", dev9_CreateIndexBuffer },
        { "IDirect3DDevice9::CreateRenderTarget", dev9_CreateRenderTarget },
        { "IDirect3DDevice9::CreateDepthStencilSurface", dev9_CreateDepthStencilSurface },
        { "IDirect3DDevice9::UpdateSurface", dev9_UpdateSurface },
        { "IDirect3DDevice9::UpdateTexture", dev9_UpdateTexture },
        { "IDirect3DDevice9::GetRenderTargetData", dev9_GetRenderTargetData },
        { "IDirect3DDevice9::GetFrontBufferData", dev9_GetFrontBufferData },
        { "IDirect3DDevice9::StretchRect", dev9_StretchRect },
        { "IDirect3DDevice9::ColorFill", dev9_ColorFill },
        { "IDirect3DDevice9::CreateOffscreenPlainSurface", dev9_CreateOffscreenPlainSurface },
        { "IDirect3DDevice9::SetRenderTarget", dev9_SetRenderTarget },
        { "IDirect3DDevice9::GetRenderTarget", dev9_GetRenderTarget },
        { "IDirect3DDevice9::SetDepthStencilSurface", dev9_SetDepthStencilSurface },
        { "IDirect3DDevice9::GetDepthStencilSurface", dev9_GetDepthStencilSurface },
        { "IDirect3DDevice9::BeginScene", dev9_BeginScene },
        { "IDirect3DDevice9::EndScene", dev9_EndScene },
        { "IDirect3DDevice9::Clear", dev9_Clear },
        { "IDirect3DDevice9::SetTransform", dev9_SetTransform },
        { "IDirect3DDevice9::GetTransform", dev9_GetTransform },
        { "IDirect3DDevice9::MultiplyTransform", dev9_MultiplyTransform },
        { "IDirect3DDevice9::SetViewport", dev9_SetViewport },
        { "IDirect3DDevice9::GetViewport", dev9_GetViewport },
        { "IDirect3DDevice9::SetMaterial", dev9_SetMaterial },
        { "IDirect3DDevice9::GetMaterial", dev9_GetMaterial },
        { "IDirect3DDevice9::SetLight", dev9_SetLight },
        { "IDirect3DDevice9::GetLight", dev9_GetLight },
        { "IDirect3DDevice9::LightEnable", dev9_LightEnable },
        { "IDirect3DDevice9::GetLightEnable", dev9_GetLightEnable },
        { "IDirect3DDevice9::SetClipPlane", dev9_SetClipPlane },
        { "IDirect3DDevice9::GetClipPlane", dev9_GetClipPlane },
        { "IDirect3DDevice9::SetRenderState", dev9_SetRenderState },
        { "IDirect3DDevice9::GetRenderState", dev9_GetRenderState },
        { "IDirect3DDevice9::CreateStateBlock", dev9_CreateStateBlock },
        { "IDirect3DDevice9::BeginStateBlock", dev9_BeginStateBlock },
        { "IDirect3DDevice9::EndStateBlock", dev9_EndStateBlock },
        { "IDirect3DDevice9::SetClipStatus", dev9_SetClipStatus },
        { "IDirect3DDevice9::GetClipStatus", dev9_GetClipStatus },
        { "IDirect3DDevice9::GetTexture", dev9_GetTexture },
        { "IDirect3DDevice9::SetTexture", dev9_SetTexture },
        { "IDirect3DDevice9::GetTextureStageState", dev9_GetTextureStageState },
        { "IDirect3DDevice9::SetTextureStageState", dev9_SetTextureStageState },
        { "IDirect3DDevice9::GetSamplerState", dev9_GetSamplerState },
        { "IDirect3DDevice9::SetSamplerState", dev9_SetSamplerState },
        { "IDirect3DDevice9::ValidateDevice", dev9_ValidateDevice },
        { "IDirect3DDevice9::SetPaletteEntries", dev9_SetPaletteEntries },
        { "IDirect3DDevice9::GetPaletteEntries", dev9_GetPaletteEntries },
        { "IDirect3DDevice9::SetCurrentTexturePalette", dev9_SetCurrentTexturePalette },
        { "IDirect3DDevice9::GetCurrentTexturePalette", dev9_GetCurrentTexturePalette },
        { "IDirect3DDevice9::SetScissorRect", dev9_SetScissorRect },
        { "IDirect3DDevice9::GetScissorRect", dev9_GetScissorRect },
        { "IDirect3DDevice9::SetSoftwareVertexProcessing", dev9_SetSoftwareVertexProcessing },
        { "IDirect3DDevice9::GetSoftwareVertexProcessing", dev9_GetSoftwareVertexProcessing },
        { "IDirect3DDevice9::SetNPatchMode", dev9_SetNPatchMode },
        { "IDirect3DDevice9::GetNPatchMode", dev9_GetNPatchMode },
        { "IDirect3DDevice9::DrawPrimitive", dev9_DrawPrimitive },
        { "IDirect3DDevice9::DrawIndexedPrimitive", dev9_DrawIndexedPrimitive },
        { "IDirect3DDevice9::DrawPrimitiveUP", dev9_DrawPrimitiveUP },
        { "IDirect3DDevice9::DrawIndexedPrimitiveUP", dev9_DrawIndexedPrimitiveUP },
        { "IDirect3DDevice9::ProcessVertices", dev9_ProcessVertices },
        { "IDirect3DDevice9::CreateVertexDeclaration", dev9_CreateVertexDeclaration },
        { "IDirect3DDevice9::SetVertexDeclaration", dev9_SetVertexDeclaration },
        { "IDirect3DDevice9::GetVertexDeclaration", dev9_GetVertexDeclaration },
        { "IDirect3DDevice9::SetFVF", dev9_SetFVF },
        { "IDirect3DDevice9::GetFVF", dev9_GetFVF },
        { "IDirect3DDevice9::CreateVertexShader", dev9_CreateVertexShader },
        { "IDirect3DDevice9::SetVertexShader", dev9_SetVertexShader },
        { "IDirect3DDevice9::GetVertexShader", dev9_GetVertexShader },
        { "IDirect3DDevice9::SetVertexShaderConstantF", dev9_SetVertexShaderConstantF },
        { "IDirect3DDevice9::GetVertexShaderConstantF", dev9_GetVertexShaderConstantF },
        { "IDirect3DDevice9::SetVertexShaderConstantI", dev9_SetVertexShaderConstantI },
        { "IDirect3DDevice9::GetVertexShaderConstantI", dev9_GetVertexShaderConstantI },
        { "IDirect3DDevice9::SetVertexShaderConstantB", dev9_SetVertexShaderConstantB },
        { "IDirect3DDevice9::GetVertexShaderConstantB", dev9_GetVertexShaderConstantB },
        { "IDirect3DDevice9::SetStreamSource", dev9_SetStreamSource },
        { "IDirect3DDevice9::GetStreamSource", dev9_GetStreamSource },
        { "IDirect3DDevice9::SetStreamSourceFreq", dev9_SetStreamSourceFreq },
        { "IDirect3DDevice9::GetStreamSourceFreq", dev9_GetStreamSourceFreq },
        { "IDirect3DDevice9::SetIndices", dev9_SetIndices },
        { "IDirect3DDevice9::GetIndices", dev9_GetIndices },
        { "IDirect3DDevice9::CreatePixelShader", dev9_CreatePixelShader },
        { "IDirect3DDevice9::SetPixelShader", dev9_SetPixelShader },
        { "IDirect3DDevice9::GetPixelShader", dev9_GetPixelShader },
        { "IDirect3DDevice9::SetPixelShaderConstantF", dev9_SetPixelShaderConstantF },
        { "IDirect3DDevice9::GetPixelShaderConstantF", dev9_GetPixelShaderConstantF },
        { "IDirect3DDevice9::SetPixelShaderConstantI", dev9_SetPixelShaderConstantI },
        { "IDirect3DDevice9::GetPixelShaderConstantI", dev9_GetPixelShaderConstantI },
        { "IDirect3DDevice9::SetPixelShaderConstantB", dev9_SetPixelShaderConstantB },
        { "IDirect3DDevice9::GetPixelShaderConstantB", dev9_GetPixelShaderConstantB },
        { "IDirect3DDevice9::DrawRectPatch", dev9_DrawRectPatch },
        { "IDirect3DDevice9::DrawTriPatch", dev9_DrawTriPatch },
        { "IDirect3DDevice9::DeletePatch", dev9_DeletePatch },
        { "IDirect3DDevice9::CreateQuery", dev9_CreateQuery },
        { 0, 0 }
    };
    g_vt_device9 = hle_com_vtable(s_dev9_methods);

    /* IDirect3DSurface9 (13 methods) */
    static const win32hle_shim s_surface9_methods[] = {
        { "IDirect3DSurface9::QueryInterface", d3d_QueryInterface },
        { "IDirect3DSurface9::AddRef", hle_com_AddRef },
        { "IDirect3DSurface9::Release", surf9_Release },
        { "IDirect3DSurface9::GetDevice", dev9_GetDirect3D },
        { "IDirect3DSurface9::SetPrivateData", d3d_res_SetPrivateData },
        { "IDirect3DSurface9::GetPrivateData", d3d_res_GetPrivateData },
        { "IDirect3DSurface9::FreePrivateData", d3d_res_FreePrivateData },
        { "IDirect3DSurface9::SetPriority", d3d_res_SetPriority },
        { "IDirect3DSurface9::GetPriority", d3d_res_GetPriority },
        { "IDirect3DSurface9::PreLoad", d3d_res_PreLoad },
        { "IDirect3DSurface9::GetType", d3d_surface_GetType },
        { "IDirect3DSurface9::GetContainer", surf9_GetContainer },
        { "IDirect3DSurface9::GetDesc", surf9_GetDesc },
        { "IDirect3DSurface9::LockRect", surf9_LockRect },
        { "IDirect3DSurface9::UnlockRect", surf9_UnlockRect },
        { "IDirect3DSurface9::GetDC", surf9_GetDC },
        { "IDirect3DSurface9::ReleaseDC", surf9_ReleaseDC },
        { 0, 0 }
    };
    g_vt_surface9 = hle_com_vtable(s_surface9_methods);

    /* IDirect3DVertexBuffer9 */
    static const win32hle_shim s_vb9_methods[] = {
        { "IDirect3DVertexBuffer9::QueryInterface", d3d_QueryInterface },
        { "IDirect3DVertexBuffer9::AddRef", hle_com_AddRef },
        { "IDirect3DVertexBuffer9::Release", vb9_Release },
        { "IDirect3DVertexBuffer9::GetDevice", dev9_GetDirect3D },
        { "IDirect3DVertexBuffer9::SetPrivateData", d3d_res_SetPrivateData },
        { "IDirect3DVertexBuffer9::GetPrivateData", d3d_res_GetPrivateData },
        { "IDirect3DVertexBuffer9::FreePrivateData", d3d_res_FreePrivateData },
        { "IDirect3DVertexBuffer9::SetPriority", d3d_res_SetPriority },
        { "IDirect3DVertexBuffer9::GetPriority", d3d_res_GetPriority },
        { "IDirect3DVertexBuffer9::PreLoad", d3d_res_PreLoad },
        { "IDirect3DVertexBuffer9::GetType", d3d_vb_GetType },
        { "IDirect3DVertexBuffer9::Lock", vb9_Lock },
        { "IDirect3DVertexBuffer9::Unlock", vb9_Unlock },
        { "IDirect3DVertexBuffer9::GetDesc", vb9_GetDesc },
        { 0, 0 }
    };
    g_vt_vb9 = hle_com_vtable(s_vb9_methods);

    /* IDirect3DIndexBuffer9 */
    static const win32hle_shim s_ib9_methods[] = {
        { "IDirect3DIndexBuffer9::QueryInterface", d3d_QueryInterface },
        { "IDirect3DIndexBuffer9::AddRef", hle_com_AddRef },
        { "IDirect3DIndexBuffer9::Release", ib9_Release },
        { "IDirect3DIndexBuffer9::GetDevice", dev9_GetDirect3D },
        { "IDirect3DIndexBuffer9::SetPrivateData", d3d_res_SetPrivateData },
        { "IDirect3DIndexBuffer9::GetPrivateData", d3d_res_GetPrivateData },
        { "IDirect3DIndexBuffer9::FreePrivateData", d3d_res_FreePrivateData },
        { "IDirect3DIndexBuffer9::SetPriority", d3d_res_SetPriority },
        { "IDirect3DIndexBuffer9::GetPriority", d3d_res_GetPriority },
        { "IDirect3DIndexBuffer9::PreLoad", d3d_res_PreLoad },
        { "IDirect3DIndexBuffer9::GetType", d3d_ib_GetType },
        { "IDirect3DIndexBuffer9::Lock", ib9_Lock },
        { "IDirect3DIndexBuffer9::Unlock", ib9_Unlock },
        { "IDirect3DIndexBuffer9::GetDesc", ib9_GetDesc },
        { 0, 0 }
    };
    g_vt_ib9 = hle_com_vtable(s_ib9_methods);

    /* IDirect3DTexture9 */
    static const win32hle_shim s_tex9_methods[] = {
        { "IDirect3DTexture9::QueryInterface", d3d_QueryInterface },
        { "IDirect3DTexture9::AddRef", hle_com_AddRef },
        { "IDirect3DTexture9::Release", tex9_Release },
        { "IDirect3DTexture9::GetDevice", dev9_GetDirect3D },
        { "IDirect3DTexture9::SetPrivateData", d3d_res_SetPrivateData },
        { "IDirect3DTexture9::GetPrivateData", d3d_res_GetPrivateData },
        { "IDirect3DTexture9::FreePrivateData", d3d_res_FreePrivateData },
        { "IDirect3DTexture9::SetPriority", d3d_res_SetPriority },
        { "IDirect3DTexture9::GetPriority", d3d_res_GetPriority },
        { "IDirect3DTexture9::PreLoad", d3d_res_PreLoad },
        { "IDirect3DTexture9::GetType", d3d_texture_GetType },
        { "IDirect3DTexture9::SetLOD", tex9_SetLOD },
        { "IDirect3DTexture9::GetLOD", tex9_GetLOD },
        { "IDirect3DTexture9::GetLevelCount", tex9_GetLevelCount },
        { "IDirect3DTexture9::SetAutoGenFilterType", tex9_SetAutoGenFilterType },
        { "IDirect3DTexture9::GetAutoGenFilterType", tex9_GetAutoGenFilterType },
        { "IDirect3DTexture9::GenerateMipSubLevels", tex9_GenerateMipSubLevels },
        { "IDirect3DTexture9::GetLevelDesc", tex9_GetLevelDesc },
        { "IDirect3DTexture9::GetSurfaceLevel", tex9_GetSurfaceLevel },
        { "IDirect3DTexture9::LockRect", tex9_LockRect },
        { "IDirect3DTexture9::UnlockRect", tex9_UnlockRect },
        { "IDirect3DTexture9::AddDirtyRect", tex9_AddDirtyRect },
        { 0, 0 }
    };
    g_vt_tex9 = hle_com_vtable(s_tex9_methods);

    /* IDirect3DCubeTexture9 */
    static const win32hle_shim s_cubetex9_methods[] = {
        { "IDirect3DCubeTexture9::QueryInterface", d3d_QueryInterface },
        { "IDirect3DCubeTexture9::AddRef", hle_com_AddRef },
        { "IDirect3DCubeTexture9::Release", cubetex9_Release },
        { "IDirect3DCubeTexture9::GetDevice", dev9_GetDirect3D },
        { "IDirect3DCubeTexture9::SetPrivateData", d3d_res_SetPrivateData },
        { "IDirect3DCubeTexture9::GetPrivateData", d3d_res_GetPrivateData },
        { "IDirect3DCubeTexture9::FreePrivateData", d3d_res_FreePrivateData },
        { "IDirect3DCubeTexture9::SetPriority", d3d_res_SetPriority },
        { "IDirect3DCubeTexture9::GetPriority", d3d_res_GetPriority },
        { "IDirect3DCubeTexture9::PreLoad", d3d_res_PreLoad },
        { "IDirect3DCubeTexture9::GetType", d3d_cubetex_GetType },
        { "IDirect3DCubeTexture9::SetLOD", cubetex9_SetLOD },
        { "IDirect3DCubeTexture9::GetLOD", cubetex9_GetLOD },
        { "IDirect3DCubeTexture9::GetLevelCount", cubetex9_GetLevelCount },
        { "IDirect3DCubeTexture9::SetAutoGenFilterType", cubetex9_SetAutoGenFilterType },
        { "IDirect3DCubeTexture9::GetAutoGenFilterType", cubetex9_GetAutoGenFilterType },
        { "IDirect3DCubeTexture9::GenerateMipSubLevels", cubetex9_GenerateMipSubLevels },
        { "IDirect3DCubeTexture9::GetLevelDesc", cubetex9_GetLevelDesc },
        { "IDirect3DCubeTexture9::GetCubeMapSurface", cubetex9_GetCubeMapSurface },
        { "IDirect3DCubeTexture9::LockRect", cubetex9_LockRect },
        { "IDirect3DCubeTexture9::UnlockRect", cubetex9_UnlockRect },
        { "IDirect3DCubeTexture9::AddDirtyRect", cubetex9_AddDirtyRect },
        { 0, 0 }
    };
    g_vt_cubetex9 = hle_com_vtable(s_cubetex9_methods);

    /* IDirect3DVolumeTexture9 */
    static const win32hle_shim s_volumetex9_methods[] = {
        { "IDirect3DVolumeTexture9::QueryInterface", d3d_QueryInterface },
        { "IDirect3DVolumeTexture9::AddRef", hle_com_AddRef },
        { "IDirect3DVolumeTexture9::Release", volumetex9_Release },
        { "IDirect3DVolumeTexture9::GetDevice", dev9_GetDirect3D },
        { "IDirect3DVolumeTexture9::SetPrivateData", d3d_res_SetPrivateData },
        { "IDirect3DVolumeTexture9::GetPrivateData", d3d_res_GetPrivateData },
        { "IDirect3DVolumeTexture9::FreePrivateData", d3d_res_FreePrivateData },
        { "IDirect3DVolumeTexture9::SetPriority", d3d_res_SetPriority },
        { "IDirect3DVolumeTexture9::GetPriority", d3d_res_GetPriority },
        { "IDirect3DVolumeTexture9::PreLoad", d3d_res_PreLoad },
        { "IDirect3DVolumeTexture9::GetType", d3d_volumetex_GetType },
        { "IDirect3DVolumeTexture9::SetLOD", volumetex9_SetLOD },
        { "IDirect3DVolumeTexture9::GetLOD", volumetex9_GetLOD },
        { "IDirect3DVolumeTexture9::GetLevelCount", volumetex9_GetLevelCount },
        { "IDirect3DVolumeTexture9::SetAutoGenFilterType", volumetex9_SetAutoGenFilterType },
        { "IDirect3DVolumeTexture9::GetAutoGenFilterType", volumetex9_GetAutoGenFilterType },
        { "IDirect3DVolumeTexture9::GenerateMipSubLevels", volumetex9_GenerateMipSubLevels },
        { "IDirect3DVolumeTexture9::GetLevelDesc", volumetex9_GetLevelDesc },
        { "IDirect3DVolumeTexture9::GetVolumeLevel", volumetex9_GetVolumeLevel },
        { "IDirect3DVolumeTexture9::LockBox", volumetex9_LockBox },
        { "IDirect3DVolumeTexture9::UnlockBox", volumetex9_UnlockBox },
        { "IDirect3DVolumeTexture9::AddDirtyBox", volumetex9_AddDirtyBox },
        { 0, 0 }
    };
    g_vt_volumetex9 = hle_com_vtable(s_volumetex9_methods);

    /* IDirect3DVolume9 */
    static const win32hle_shim s_volume9_methods[] = {
        { "IDirect3DVolume9::QueryInterface", d3d_QueryInterface },
        { "IDirect3DVolume9::AddRef", hle_com_AddRef },
        { "IDirect3DVolume9::Release", vol9_Release },
        { "IDirect3DVolume9::GetDevice", dev9_GetDirect3D },
        { "IDirect3DVolume9::SetPrivateData", d3d_res_SetPrivateData },
        { "IDirect3DVolume9::GetPrivateData", d3d_res_GetPrivateData },
        { "IDirect3DVolume9::FreePrivateData", d3d_res_FreePrivateData },
        { "IDirect3DVolume9::GetContainer", vol9_GetContainer },
        { "IDirect3DVolume9::GetDesc", vol9_GetDesc },
        { "IDirect3DVolume9::LockBox", vol9_LockBox },
        { "IDirect3DVolume9::UnlockBox", vol9_UnlockBox },
        { 0, 0 }
    };
    g_vt_volume9 = hle_com_vtable(s_volume9_methods);

    /* IDirect3DVertexDeclaration9 */
    static const win32hle_shim s_decl9_methods[] = {
        { "IDirect3DVertexDeclaration9::QueryInterface", d3d_QueryInterface },
        { "IDirect3DVertexDeclaration9::AddRef", hle_com_AddRef },
        { "IDirect3DVertexDeclaration9::Release", decl9_Release },
        { "IDirect3DVertexDeclaration9::GetDevice", dev9_GetDirect3D },
        { "IDirect3DVertexDeclaration9::GetDeclaration", decl9_GetDeclaration },
        { 0, 0 }
    };
    g_vt_decl9 = hle_com_vtable(s_decl9_methods);

    /* IDirect3DVertexShader9 */
    static const win32hle_shim s_vs9_methods[] = {
        { "IDirect3DVertexShader9::QueryInterface", d3d_QueryInterface },
        { "IDirect3DVertexShader9::AddRef", hle_com_AddRef },
        { "IDirect3DVertexShader9::Release", shader9_Release },
        { "IDirect3DVertexShader9::GetDevice", dev9_GetDirect3D },
        { "IDirect3DVertexShader9::GetFunction", shader9_GetFunction },
        { 0, 0 }
    };
    g_vt_vs9 = hle_com_vtable(s_vs9_methods);

    /* IDirect3DPixelShader9 */
    static const win32hle_shim s_ps9_methods[] = {
        { "IDirect3DPixelShader9::QueryInterface", d3d_QueryInterface },
        { "IDirect3DPixelShader9::AddRef", hle_com_AddRef },
        { "IDirect3DPixelShader9::Release", shader9_Release },
        { "IDirect3DPixelShader9::GetDevice", dev9_GetDirect3D },
        { "IDirect3DPixelShader9::GetFunction", shader9_GetFunction },
        { 0, 0 }
    };
    g_vt_ps9 = hle_com_vtable(s_ps9_methods);

    /* IDirect3DQuery9 */
    static const win32hle_shim s_query9_methods[] = {
        { "IDirect3DQuery9::QueryInterface", d3d_QueryInterface },
        { "IDirect3DQuery9::AddRef", hle_com_AddRef },
        { "IDirect3DQuery9::Release", query9_Release },
        { "IDirect3DQuery9::GetDevice", dev9_GetDirect3D },
        { "IDirect3DQuery9::GetType", query9_GetType },
        { "IDirect3DQuery9::GetDataSize", query9_GetDataSize },
        { "IDirect3DQuery9::Issue", query9_Issue },
        { "IDirect3DQuery9::GetData", query9_GetData },
        { 0, 0 }
    };
    g_vt_query9 = hle_com_vtable(s_query9_methods);

    /* IDirect3DStateBlock9 */
    static const win32hle_shim s_sb9_methods[] = {
        { "IDirect3DStateBlock9::QueryInterface", d3d_QueryInterface },
        { "IDirect3DStateBlock9::AddRef", hle_com_AddRef },
        { "IDirect3DStateBlock9::Release", sb9_Release },
        { "IDirect3DStateBlock9::GetDevice", dev9_GetDirect3D },
        { "IDirect3DStateBlock9::Capture", sb9_Capture },
        { "IDirect3DStateBlock9::Apply", sb9_Apply },
        { 0, 0 }
    };
    g_vt_sb9 = hle_com_vtable(s_sb9_methods);

    /* IDirect3DSwapChain9 (10 methods) */
    static const win32hle_shim s_swapchain9_methods[] = {
        { "IDirect3DSwapChain9::QueryInterface", d3d_QueryInterface },
        { "IDirect3DSwapChain9::AddRef", hle_com_AddRef },
        { "IDirect3DSwapChain9::Release", sc9_Release },
        { "IDirect3DSwapChain9::Present", sc9_Present },
        { "IDirect3DSwapChain9::GetFrontBufferData", sc9_GetFrontBufferData },
        { "IDirect3DSwapChain9::GetBackBuffer", sc9_GetBackBuffer },
        { "IDirect3DSwapChain9::GetRasterStatus", sc9_GetRasterStatus },
        { "IDirect3DSwapChain9::GetDisplayMode", sc9_GetDisplayMode },
        { "IDirect3DSwapChain9::GetDevice", sc9_GetDevice },
        { "IDirect3DSwapChain9::GetPresentParameters", sc9_GetPresentParameters },
        { 0, 0 }
    };
    g_vt_swapchain9 = hle_com_vtable(s_swapchain9_methods);
}

/* =========================================================================
 * Direct3DCreate9 Entry Point
 * ========================================================================= */

static void s_Direct3DCreate9(void) {
    uint32_t sdk_version = A32(0);
    init_d3d9_vtables();

    uint32_t o = hle_com_new(g_vt_d3d9, 16);
    T("Direct3DCreate9(SDKVersion=%u) -> 0x%08X\n", sdk_version, o);
    RET(o, 1);
}

const win32hle_shim win32hle_d3d9[] = {
    { "d3d9.dll#Direct3DCreate9", s_Direct3DCreate9 },
    { "Direct3DCreate9", s_Direct3DCreate9 },
    /* PIX profiling exports: public Wine 11.0 include/d3d9.h, WINAPI stdcall. */
    { "D3DPERF_BeginEvent", perf_BeginEvent },
    { "D3DPERF_EndEvent", perf_EndEvent },
    { "D3DPERF_SetMarker", perf_SetMarker },
    { "D3DPERF_SetRegion", perf_SetRegion },
    { "D3DPERF_QueryRepeatFrame", perf_QueryRepeatFrame },
    { "D3DPERF_SetOptions", perf_SetOptions },
    { "D3DPERF_GetStatus", perf_GetStatus },
    { 0, 0 }
};
