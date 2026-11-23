/* Real IDirect3DQuery9 against the Metal backend.
 *
 * Occlusion uses MTLVisibilityResultBuffer counting: a near quad writes depth
 * in the left half, a far quad in the right half passes depth (count > 0), a
 * far quad in the left half fails depth (count == 0). Pixels are read back as
 * an independent check that the draws really happened.
 * Event is a fence on command-buffer completion, not a constant.
 *
 * Public values: Wine include/d3d9types.h and include/d3d9.h.
 */
#include "test_d3d9_common.h"

int main(void) {
    uint32_t dev = make_device();
    if (!dev) return 1;

    uint32_t *pQ = (uint32_t *)hle_alloc(4);
    uint32_t *data = (uint32_t *)hle_alloc(4);

    /* ---- unsupported public query types are honestly rejected ---------- */
    static const uint32_t unsupported[] = {
        D3DQUERYTYPE_VCACHE, D3DQUERYTYPE_TIMESTAMP,
        D3DQUERYTYPE_TIMESTAMPDISJOINT, D3DQUERYTYPE_MEMORYPRESSURE, 1u
    };
    for (unsigned i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); i++) {
        *pQ = 0xDEADBEEFu;
        uint32_t hr = call("IDirect3DDevice9::CreateQuery", 3,
                           (uint32_t[]){ dev, unsupported[i], A(pQ) });
        char msg[128];
        snprintf(msg, sizeof msg, "CreateQuery type %u -> D3DERR_NOTAVAILABLE", unsupported[i]);
        ok(msg, hr == D3DERR_NOTAVAILABLE && *pQ == 0xDEADBEEFu);
    }

    /* ---- create two occlusion queries and one event -------------------- */
    *pQ = 0;
    ok("CreateQuery(OCCLUSION) visible",
       call("IDirect3DDevice9::CreateQuery", 3,
            (uint32_t[]){ dev, D3DQUERYTYPE_OCCLUSION, A(pQ) }) == D3D_OK && *pQ != 0);
    uint32_t q_vis = *pQ;
    *pQ = 0;
    ok("CreateQuery(OCCLUSION) occluded",
       call("IDirect3DDevice9::CreateQuery", 3,
            (uint32_t[]){ dev, D3DQUERYTYPE_OCCLUSION, A(pQ) }) == D3D_OK && *pQ != 0);
    uint32_t q_occ = *pQ;
    *pQ = 0;
    ok("CreateQuery(EVENT)",
       call("IDirect3DDevice9::CreateQuery", 3,
            (uint32_t[]){ dev, D3DQUERYTYPE_EVENT, A(pQ) }) == D3D_OK && *pQ != 0);
    uint32_t q_ev = *pQ;
    ok("Occlusion GetType",
       call("IDirect3DQuery9::GetType", 1, (uint32_t[]){ q_vis }) == D3DQUERYTYPE_OCCLUSION);
    ok("Event GetType",
       call("IDirect3DQuery9::GetType", 1, (uint32_t[]){ q_ev }) == D3DQUERYTYPE_EVENT);
    ok("Occlusion GetDataSize == 4",
       call("IDirect3DQuery9::GetDataSize", 1, (uint32_t[]){ q_vis }) == 4);
    ok("Event GetDataSize == 4",
       call("IDirect3DQuery9::GetDataSize", 1, (uint32_t[]){ q_ev }) == 4);

    /* ---- geometry -------------------------------------------------------
     * near: left half, z=0.2, writes depth. far_right/far_left: z=0.6. */
    Vtx near[6] = {
        vertex(0, 0, 0.2f, 0, 1, 0, 1), vertex(16, 0, 0.2f, 0, 1, 0, 1),
        vertex(0, 32, 0.2f, 0, 1, 0, 1),
        vertex(0, 32, 0.2f, 0, 1, 0, 1), vertex(16, 0, 0.2f, 0, 1, 0, 1),
        vertex(16, 32, 0.2f, 0, 1, 0, 1)
    };
    Vtx far_right[6] = {
        vertex(16, 0, 0.6f, 1, 0, 0, 1), vertex(32, 0, 0.6f, 1, 0, 0, 1),
        vertex(16, 32, 0.6f, 1, 0, 0, 1),
        vertex(16, 32, 0.6f, 1, 0, 0, 1), vertex(32, 0, 0.6f, 1, 0, 0, 1),
        vertex(32, 32, 0.6f, 1, 0, 0, 1)
    };
    Vtx far_left[6] = {
        vertex(0, 0, 0.6f, 1, 0, 0, 1), vertex(16, 0, 0.6f, 1, 0, 0, 1),
        vertex(0, 32, 0.6f, 1, 0, 0, 1),
        vertex(0, 32, 0.6f, 1, 0, 0, 1), vertex(16, 0, 0.6f, 1, 0, 0, 1),
        vertex(16, 32, 0.6f, 1, 0, 0, 1)
    };

    clear(dev, 0xFF000000u, 1 | 2, 1.0f);
    call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, 7, 1 });  /* ZENABLE */
    call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, 14, 1 }); /* ZWRITEENABLE */
    call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, 23, 4 }); /* ZFUNC LESSEQUAL */

    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    make_vb(dev, near, 6);             /* depth writer, outside both queries */
    call("IDirect3DDevice9::DrawPrimitive", 4,
         (uint32_t[]){ dev, D3DPT_TRIANGLELIST, 0, 2 });

    /* ---- visible occlusion query --------------------------------------- */
    ok("Issue(BEGIN) visible", call("IDirect3DQuery9::Issue", 2,
        (uint32_t[]){ q_vis, D3DISSUE_BEGIN }) == D3D_OK);
    make_vb(dev, far_right, 6);
    call("IDirect3DDevice9::DrawPrimitive", 4,
         (uint32_t[]){ dev, D3DPT_TRIANGLELIST, 0, 2 });
    ok("Issue(END) visible", call("IDirect3DQuery9::Issue", 2,
        (uint32_t[]){ q_vis, D3DISSUE_END }) == D3D_OK);
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });

    *data = 0xDEADBEEFu;
    ok("visible GetData(FLUSH) S_OK", call("IDirect3DQuery9::GetData", 4,
        (uint32_t[]){ q_vis, A(data), 4, D3DGETDATA_FLUSH }) == D3D_OK);
    uint32_t visible_count = *data;
    /* The right-half quad covers 16 px x 32 px = 512 samples (no MSAA),
     * every one passing depth: Metal's counter must read exactly that. */
    ok("visible occlusion count == 512 (real passing samples)", visible_count == 512u);
    printf("    visible occlusion count = %u\n", visible_count);

    uint32_t pixels[32 * 32];
    read_all(pixels);
    ok("visible far quad drew red in the right half",
       pixels[16 * 32 + 24] == 0xFFFF0000u);
    ok("near quad still green in the left half",
       pixels[16 * 32 + 8] == 0xFF00FF00u);

    /* ---- occluded occlusion query -------------------------------------- */
    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    ok("Issue(BEGIN) occluded", call("IDirect3DQuery9::Issue", 2,
        (uint32_t[]){ q_occ, D3DISSUE_BEGIN }) == D3D_OK);
    make_vb(dev, far_left, 6);
    call("IDirect3DDevice9::DrawPrimitive", 4,
         (uint32_t[]){ dev, D3DPT_TRIANGLELIST, 0, 2 });
    ok("Issue(END) occluded", call("IDirect3DQuery9::Issue", 2,
        (uint32_t[]){ q_occ, D3DISSUE_END }) == D3D_OK);
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });

    *data = 0xDEADBEEFu;
    ok("occluded GetData(FLUSH) S_OK", call("IDirect3DQuery9::GetData", 4,
        (uint32_t[]){ q_occ, A(data), 4, D3DGETDATA_FLUSH }) == D3D_OK);
    uint32_t occluded_count = *data;
    ok("occluded occlusion count == 0 (all samples failed depth)", occluded_count == 0);
    printf("    occluded occlusion count = %u\n", occluded_count);

    read_all(pixels);
    ok("far quad did not overwrite the near green pixel",
       pixels[16 * 32 + 8] == 0xFF00FF00u);

    /* ---- event query: real command-buffer completion ------------------- */
    *data = 0xDEADBEEFu;
    ok("event GetData before Issue is S_FALSE", call("IDirect3DQuery9::GetData", 4,
        (uint32_t[]){ q_ev, A(data), 4, D3DGETDATA_FLUSH }) == 1);
    ok("event GetData before Issue writes 0", *data == 0u);
    ok("event Issue(END)", call("IDirect3DQuery9::Issue", 2,
        (uint32_t[]){ q_ev, D3DISSUE_END }) == D3D_OK);
    *data = 0xDEADBEEFu;
    ok("event GetData(FLUSH) S_OK", call("IDirect3DQuery9::GetData", 4,
        (uint32_t[]){ q_ev, A(data), 4, D3DGETDATA_FLUSH }) == D3D_OK);
    ok("event signaled after buffer completion", *data == 1u);

    call("IDirect3DQuery9::Release", 1, (uint32_t[]){ q_vis });
    call("IDirect3DQuery9::Release", 1, (uint32_t[]){ q_occ });
    call("IDirect3DQuery9::Release", 1, (uint32_t[]){ q_ev });

    printf(fails == 0 ? "test_d3d9_query: OK\n" : "test_d3d9_query: FAILED\n");
    return fails != 0;
}
