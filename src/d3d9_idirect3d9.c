#include "d3d9_internal.h"
#include "d3d9_caps.h"

/* =========================================================================
 * IDirect3D9 Implementation
 * ========================================================================= */

void d9_QueryInterface(void) {
    uint32_t self = A32(0);
    uint32_t riid = A32(1);
    uint32_t ppv = A32(2);
    (void)riid;
    if (!ppv) RET(D3DERR_INVALIDCALL, 3);
    MEM32(ppv) = self;
    MEM32(self + 4)++;
    RET(D3D_OK, 3);
}

void d9_RegisterSoftwareDevice(void) {
    RET(D3D_OK, 2);
}

void d9_GetAdapterCount(void) {
    RET(1, 1);
}

void d9_GetAdapterIdentifier(void) {
    uint32_t adapter = A32(1);
    uint32_t flags = A32(2);
    uint32_t pOut = A32(3);
    (void)flags;
    if (adapter != 0 || !pOut) RET(D3DERR_INVALIDCALL, 4);

    D3DADAPTER_IDENTIFIER9 *id = (D3DADAPTER_IDENTIFIER9 *)hle_ptr(pOut);
    memset(id, 0, sizeof(*id));

    snprintf(id->Driver, sizeof(id->Driver), "d3d9hle.dll");
    snprintf(id->Description, sizeof(id->Description), "D3D9 HLE (Metal backend)");
    snprintf(id->DeviceName, sizeof(id->DeviceName), "\\\\.\\DISPLAY1");
    id->DriverVersionLow = 0x00010001;
    id->DriverVersionHigh = 0x00010000;
    id->VendorId = 0x0000;          /* generic */
    id->DeviceId = 0x0000;          /* generic */
    id->SubSysId = 0;
    id->Revision = 0;
    id->WHQLLevel = 0;

    /* A stable pseudo-GUID for the adapter */
    static const uint8_t s_guid[16] = {
        0xD7, 0xB7, 0x1E, 0x7E, 0x42, 0xD1, 0x11, 0xCF,
        0xA2, 0x23, 0x00, 0xAA, 0x00, 0x48, 0x49, 0x73
    };
    memcpy(id->DeviceIdentifier, s_guid, 16);

    RET(D3D_OK, 4);
}

void d9_GetAdapterModeCount(void) {
    uint32_t adapter = A32(1), format = A32(2);
    if (adapter != 0) RET(0, 3);
    RET(d3d9_caps_mode_count(format), 3);
}

void d9_EnumAdapterModes(void) {
    uint32_t adapter = A32(1), format = A32(2), mode = A32(3), pMode = A32(4);
    if (adapter != 0 || !pMode) RET(D3DERR_INVALIDCALL, 5);
    d3d9_caps_mode m;
    uint32_t hr = d3d9_caps_enum_mode(format, mode, &m);
    if (hr == D3D_OK) {
        D3DDISPLAYMODE *dm = (D3DDISPLAYMODE *)hle_ptr(pMode);
        dm->Width = m.Width; dm->Height = m.Height;
        dm->RefreshRate = m.RefreshRate; dm->Format = m.Format;
    }
    RET(hr, 5);
}

void d9_GetAdapterDisplayMode(void) {
    uint32_t adapter = A32(1);
    uint32_t pMode = A32(2);
    if (adapter != 0 || !pMode) RET(D3DERR_INVALIDCALL, 3);

    D3DDISPLAYMODE *dm = (D3DDISPLAYMODE *)hle_ptr(pMode);
    d3d9_caps_mode m;
    d3d9_caps_display_mode(&m);
    dm->Width = m.Width; dm->Height = m.Height;
    dm->RefreshRate = m.RefreshRate; dm->Format = m.Format;
    RET(D3D_OK, 3);
}

void d9_CheckDeviceType(void) {
    uint32_t adapter = A32(1);
    (void)adapter;
    RET(D3D_OK, 6);
}

void d9_CheckDeviceFormat(void) {
    uint32_t adapter = A32(1);
    if (adapter != 0) RET(D3DERR_INVALIDCALL, 7);
    RET(d3d9_caps_check_format(A32(3), A32(4), A32(5), A32(6)), 7);
}

void d9_CheckDeviceMultiSampleType(void) {
    uint32_t adapter = A32(1);
    if (adapter != 0) RET(D3DERR_INVALIDCALL, 7);
    RET(d3d9_caps_check_multisample(A32(3), A32(4), A32(5), A32(6) ? (uint32_t *)hle_ptr(A32(6)) : NULL), 7);
}

void d9_CheckDepthStencilMatch(void) {
    /* (this, Adapter, DeviceType, AdapterFormat, RenderTargetFormat,
     *  DepthStencilFormat) */
    RET(d3d9_caps_check_depth_stencil(A32(1), A32(2), A32(3), A32(4), A32(5)), 6);
}

void d9_CheckDeviceFormatConversion(void) {
    RET(D3D_OK, 5);
}

void fill_device_caps(D3DCAPS9 *c) {
    /* Single source of truth in d3d9_caps.c (evidence table in its header). */
    d3d9_caps_fill(c);
}

void d9_GetDeviceCaps(void) {
    uint32_t adapter = A32(1);
    uint32_t pCaps = A32(3);
    if (adapter != 0 || !pCaps) RET(D3DERR_INVALIDCALL, 4);

    D3DCAPS9 *c = (D3DCAPS9 *)hle_ptr(pCaps);
    fill_device_caps(c);
    RET(D3D_OK, 4);
}

void d9_GetAdapterMonitor(void) {
    RET(0x10001u, 2);
}


/* =========================================================================
 * CreateDevice Implementation
 * ========================================================================= */

void d9_CreateDevice(void) {
    /* (this, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface) */
    uint32_t self = A32(0);
    uint32_t adapter = A32(1);
    uint32_t dev_type = A32(2);
    uint32_t focus_window = A32(3);
    uint32_t flags = A32(4);
    uint32_t pParams = A32(5);
    uint32_t ppDevice = A32(6);

    if (adapter != 0 || !pParams || !ppDevice) {
        RET(D3DERR_INVALIDCALL, 7);
    }

    init_d3d9_vtables();

    d3d9_device_ctx_t *ctx = (d3d9_device_ctx_t *)calloc(1, sizeof(*ctx));
    /* Public D3D9 defaults the HLE cannot take from zero-init:
     * D3DRS_COLORWRITEENABLE* default to 0xF (MSDN "Saving Pixel State"). */
    ctx->render_states[D3DRS_COLORWRITEENABLE] = 0xFu;
    ctx->render_states[D3DRS_COLORWRITEENABLE1] = 0xFu;
    ctx->render_states[D3DRS_COLORWRITEENABLE2] = 0xFu;
    ctx->render_states[D3DRS_COLORWRITEENABLE3] = 0xFu;
    ctx->d3d9_obj = self;
    ctx->cursor_visible = 1;
    ctx->adapter = adapter;
    ctx->dev_type = dev_type;
    ctx->focus_window = focus_window;
    ctx->behavior_flags = flags;
    memcpy(&ctx->pp, hle_ptr(pParams), sizeof(D3DPRESENT_PARAMETERS));

    /* Initialize default viewport */
    ctx->viewport.X = 0;
    ctx->viewport.Y = 0;
    ctx->viewport.Width = ctx->pp.BackBufferWidth ? ctx->pp.BackBufferWidth : D3D9_DEFAULT_WIDTH;
    ctx->viewport.Height = ctx->pp.BackBufferHeight ? ctx->pp.BackBufferHeight : D3D9_DEFAULT_HEIGHT;
    ctx->viewport.MinZ = 0.0f;
    ctx->viewport.MaxZ = 1.0f;

    /* Initialize default scissor rect */
    ctx->scissor_rect.x1 = 0;
    ctx->scissor_rect.y1 = 0;
    ctx->scissor_rect.x2 = (int32_t)ctx->viewport.Width;
    ctx->scissor_rect.y2 = (int32_t)ctx->viewport.Height;

    /* Identity matrices for standard transforms */
    for (int t = 0; t < 256; t++) {
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                ctx->transforms[t].m[i][j] = (i == j) ? 1.0f : 0.0f;
            }
        }
    }
    /* D3DTSS_CONSTANT defaults to 0xFFFFFFFF (D3D9). */
    for (int st = 0; st < MAX_TEXTURE_STAGES; st++)
        for (int ty = 0; ty < MAX_TSS; ty++)
            ctx->texture_stage_states[st][ty] = 0;
    for (int st = 0; st < MAX_TEXTURE_STAGES; st++)
        ctx->texture_stage_states[st][32] = 0xFFFFFFFFu;

    /* Allocate device COM object in guest memory */
    uint32_t o = hle_com_new(g_vt_device9, 16);
    ctx->self = o;
    SET_OBJ_CTX(o, ctx);

    /* Create initial default backbuffer render target */
    uint32_t bb_fmt = ctx->pp.BackBufferFormat ? ctx->pp.BackBufferFormat : D3DFMT_A8R8G8B8;
    ctx->render_target = create_surface_internal(ctx, ctx->viewport.Width, ctx->viewport.Height, bb_fmt, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT);

    /* Create depth-stencil surface if requested */
    if (ctx->pp.EnableAutoDepthStencil) {
        uint32_t ds_fmt = ctx->pp.AutoDepthStencilFormat ? ctx->pp.AutoDepthStencilFormat : D3DFMT_D24S8;
        ctx->depth_stencil = create_surface_internal(ctx, ctx->viewport.Width, ctx->viewport.Height, ds_fmt, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT);
    }

    /* Bring the GPU backend up with the requested backbuffer size. Without
     * this the Metal backend keeps its render target nil and every Clear /
     * BeginScene / Draw becomes a silent no-op (black screen). init_device
     * enables the backend lazily and is idempotent for an unchanged size. */
    if (hle_d3d9_gpu && hle_d3d9_gpu->init_device) {
        hle_d3d9_gpu->init_device(ctx->viewport.Width, ctx->viewport.Height);
        T("CreateDevice: GPU init_device %ux%u\n", ctx->viewport.Width, ctx->viewport.Height);
    }
    /* The device-created back buffer / depth stencil are owned by the device
     * and do not count as application-held D3DPOOL_DEFAULT resources when the
     * app releases its explicit ones before a Reset (see dev9_Reset). */
    ctx->device_state = D3D9_DEVICE_STATE_OK;
    ctx->implicit_rt = ctx->render_target;
    ctx->implicit_ds = ctx->depth_stencil;
    /* Draw into the same surface GetBackBuffer returns, so Clear/Draw and a
     * pixel readback of that render target see the same GPU texture. */
    if (hle_d3d9_gpu && hle_d3d9_gpu->set_render_target) {
        d3d9_surface_ctx_t *sc = (d3d9_surface_ctx_t *)OBJ_CTX(ctx->render_target);
        hle_d3d9_gpu->set_render_target(sc ? sc->gpu_rt : NULL);
    }

    MEM32(ppDevice) = o;
    T("CreateDevice -> 0x%08X (resolution %ux%u format=%u flags=0x%X)\n",
      o, ctx->viewport.Width, ctx->viewport.Height, bb_fmt, flags);

    RET(D3D_OK, 7);
}
