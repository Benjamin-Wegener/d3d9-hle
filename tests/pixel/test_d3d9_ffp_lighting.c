/* Test Fixed-Function Pipeline Lighting and Material on real Metal backend.
 *
 * Tests:
 * 1. Directional Light:
 *    - Light 0: Directional, Dir = (0, 0, 1), Diffuse = (1.0, 1.0, 1.0, 1.0)
 *    - Material: Diffuse = (0.5, 0.5, 0.5, 1.0), Ambient = (0.2, 0.2, 0.2, 1.0)
 *    - Quad normal = (0, 0, -1). L = -Dir = (0, 0, -1). N . L = 1.0
 *    - Diffuse contribution = 1.0 * (0.5 * 1.0) = 0.5 (128)
 * 2. Point Light:
 *    - Light 1: Point, Pos = (0, 0, -2), Diffuse = (0.0, 1.0, 0.0, 1.0), Range = 10.0
 *    - Attenuation0 = 1.0, Attenuation1 = 0.0, Attenuation2 = 0.0
 * 3. Spot Light:
 *    - Light 2: Spot with Theta = 30 deg, Phi = 60 deg, Falloff = 1.0
 * 4. Specular with Power:
 *    - Light 2 with specular and D3DRS_SPECULARENABLE
 * 5. Ambient, COLORVERTEX and edge cases
 */
#include "test_d3d9_common.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    float x, y, z;
    float nx, ny, nz;
    uint32_t diffuse;
} VtxLit;

int main(void) {
    uint32_t dev = make_device();
    if (!dev) return 1;

    /* Use a central vertex at (0, 0, 0) and 4 perimeter vertices to form
     * a 4-triangle fan. This guarantees that pixel (16, 16) matches the center vertex
     * calculation exactly, independent of vertex interpolation across the quad. */
    VtxLit v[6] = {
        {  0.0f,  0.0f, 0.0f,  0.0f, 0.0f, -1.0f,  0xFFFFFFFFu }, /* Center */
        { -1.0f, -1.0f, 0.0f,  0.0f, 0.0f, -1.0f,  0xFFFFFFFFu },
        {  1.0f, -1.0f, 0.0f,  0.0f, 0.0f, -1.0f,  0xFFFFFFFFu },
        {  1.0f,  1.0f, 0.0f,  0.0f, 0.0f, -1.0f,  0xFFFFFFFFu },
        { -1.0f,  1.0f, 0.0f,  0.0f, 0.0f, -1.0f,  0xFFFFFFFFu },
        { -1.0f, -1.0f, 0.0f,  0.0f, 0.0f, -1.0f,  0xFFFFFFFFu }
    };

    uint32_t vb_size = sizeof(v);
    uint32_t *pVB = (uint32_t *)hle_alloc(4);
    uint32_t hr = call("IDirect3DDevice9::CreateVertexBuffer", 7,
                       (uint32_t[]){ dev, vb_size, 0, 0, D3DPOOL_MANAGED, A(pVB), 0 });
    ok("CreateVertexBuffer", hr == D3D_OK && *pVB);
    uint32_t *pData = (uint32_t *)hle_alloc(4);
    call("IDirect3DVertexBuffer9::Lock", 5, (uint32_t[]){ *pVB, 0, vb_size, A(pData), 0 });
    memcpy(hle_ptr(*pData), v, vb_size);
    call("IDirect3DVertexBuffer9::Unlock", 1, (uint32_t[]){ *pVB });

    call("IDirect3DDevice9::SetStreamSource", 5,
         (uint32_t[]){ dev, 0, *pVB, 0, sizeof(VtxLit) });
    /* D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE */
    call("IDirect3DDevice9::SetFVF", 2,
         (uint32_t[]){ dev, 0x002u | 0x010u | 0x040u });

    /* Enable Lighting */
    call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, 137 /* D3DRS_LIGHTING */, 1 });

    /* Material: Diffuse (0.5, 0.5, 0.5, 1.0), Ambient (0.2, 0.2, 0.2, 1.0) */
    D3DMATERIAL9 mat;
    memset(&mat, 0, sizeof(mat));
    mat.Diffuse.r = 0.5f; mat.Diffuse.g = 0.5f; mat.Diffuse.b = 0.5f; mat.Diffuse.a = 1.0f;
    mat.Ambient.r = 0.2f; mat.Ambient.g = 0.2f; mat.Ambient.b = 0.2f; mat.Ambient.a = 1.0f;
    call("IDirect3DDevice9::SetMaterial", 2, (uint32_t[]){ dev, A(G_copy(&mat, sizeof(mat))) });

    /* Test 1: Directional light pointing towards scene (+Z): dir = (0, 0, 1)
     * Surface normal = (0, 0, -1). L = -dir = (0, 0, -1). N . L = 1.0
     * Light Diffuse = (1.0, 0.0, 0.0, 1.0) -> Pure Red diffuse: 0.5 * 1.0 = 0.5 -> 0x80 */
    D3DLIGHT9 lt0;
    memset(&lt0, 0, sizeof(lt0));
    lt0.Type = D3DLIGHT_DIRECTIONAL;
    lt0.Diffuse.r = 1.0f; lt0.Diffuse.g = 0.0f; lt0.Diffuse.b = 0.0f; lt0.Diffuse.a = 1.0f;
    lt0.Direction.x = 0.0f; lt0.Direction.y = 0.0f; lt0.Direction.z = 1.0f;
    call("IDirect3DDevice9::SetLight", 3, (uint32_t[]){ dev, 0, A(G_copy(&lt0, sizeof(lt0))) });
    call("IDirect3DDevice9::LightEnable", 3, (uint32_t[]){ dev, 0, 1 });

    clear(dev, 0xFF000000u, 1, 1.0f);
    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    hr = call("IDirect3DDevice9::DrawPrimitive", 4, (uint32_t[]){ dev, D3DPT_TRIANGLEFAN, 0, 4 });
    ok("DrawPrimitive lit directional", hr == D3D_OK);
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });

    uint32_t pixels[32 * 32];
    read_all(pixels);
    uint32_t p0 = pixels[16 * 32 + 16];
    uint32_t r0 = (p0 >> 16) & 0xFFu, g0 = (p0 >> 8) & 0xFFu, b0 = p0 & 0xFFu;
    /* Expected: R ~ 127/128 (0.5 * 255 = 127.5), G=0, B=0 */
    ok("Directional red diffuse channel ~ 127", r0 >= 120 && r0 <= 135);
    ok("Directional green channel == 0", g0 == 0);
    ok("Directional blue channel == 0", b0 == 0);

    /* Test 2: Disable Light 0, enable Point Light (Light 1)
     * Point light at (0, 0, -2) in world space, Quad at z = 0
     * Diffuse = Green (0.0, 1.0, 0.0, 1.0), Attenuation0 = 1.0
     * At centre (0, 0, 0): dist = 2.0 -> Attenuation = 1.0 / (1.0 + 0 + 0) = 1.0
     * L = normalize((0, 0, -2) - (0, 0, 0)) = (0, 0, -1). N = (0, 0, -1). N . L = 1.0
     * Diffuse Green = 0.5 * 1.0 = 0.5 -> ~127 */
    call("IDirect3DDevice9::LightEnable", 3, (uint32_t[]){ dev, 0, 0 });
    D3DLIGHT9 lt1;
    memset(&lt1, 0, sizeof(lt1));
    lt1.Type = D3DLIGHT_POINT;
    lt1.Position.x = 0.0f; lt1.Position.y = 0.0f; lt1.Position.z = -2.0f;
    lt1.Range = 10.0f;
    lt1.Attenuation0 = 1.0f;
    lt1.Diffuse.r = 0.0f; lt1.Diffuse.g = 1.0f; lt1.Diffuse.b = 0.0f; lt1.Diffuse.a = 1.0f;
    call("IDirect3DDevice9::SetLight", 3, (uint32_t[]){ dev, 1, A(G_copy(&lt1, sizeof(lt1))) });
    call("IDirect3DDevice9::LightEnable", 3, (uint32_t[]){ dev, 1, 1 });

    clear(dev, 0xFF000000u, 1, 1.0f);
    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    call("IDirect3DDevice9::DrawPrimitive", 4, (uint32_t[]){ dev, D3DPT_TRIANGLEFAN, 0, 4 });
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });
    read_all(pixels);
    uint32_t p1 = pixels[16 * 32 + 16];
    uint32_t r1 = (p1 >> 16) & 0xFFu, g1 = (p1 >> 8) & 0xFFu, b1 = p1 & 0xFFu;
    ok("Point green diffuse channel ~ 127", g1 >= 120 && g1 <= 135);
    ok("Point red channel == 0", r1 == 0);
    ok("Point blue channel == 0", b1 == 0);

    /* Test 3: Spot Light (Light 2) pointing straight at centre
     * Position = (0, 0, -2), Direction = (0, 0, 1), Range = 10
     * Theta = 30 deg, Phi = 60 deg, Falloff = 1.0, Diffuse = Blue (0, 0, 1, 1) */
    call("IDirect3DDevice9::LightEnable", 3, (uint32_t[]){ dev, 1, 0 });
    D3DLIGHT9 lt2;
    memset(&lt2, 0, sizeof(lt2));
    lt2.Type = D3DLIGHT_SPOT;
    lt2.Position.x = 0.0f; lt2.Position.y = 0.0f; lt2.Position.z = -2.0f;
    lt2.Direction.x = 0.0f; lt2.Direction.y = 0.0f; lt2.Direction.z = 1.0f;
    lt2.Range = 10.0f;
    lt2.Theta = (float)(30.0 * M_PI / 180.0);
    lt2.Phi = (float)(60.0 * M_PI / 180.0);
    lt2.Falloff = 1.0f;
    lt2.Attenuation0 = 1.0f;
    lt2.Diffuse.r = 0.0f; lt2.Diffuse.g = 0.0f; lt2.Diffuse.b = 1.0f; lt2.Diffuse.a = 1.0f;
    call("IDirect3DDevice9::SetLight", 3, (uint32_t[]){ dev, 2, A(G_copy(&lt2, sizeof(lt2))) });
    call("IDirect3DDevice9::LightEnable", 3, (uint32_t[]){ dev, 2, 1 });

    clear(dev, 0xFF000000u, 1, 1.0f);
    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    call("IDirect3DDevice9::DrawPrimitive", 4, (uint32_t[]){ dev, D3DPT_TRIANGLEFAN, 0, 4 });
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });
    read_all(pixels);
    uint32_t p2 = pixels[16 * 32 + 16];
    uint32_t r2 = (p2 >> 16) & 0xFFu, g2 = (p2 >> 8) & 0xFFu, b2 = p2 & 0xFFu;
    ok("Spot blue diffuse channel ~ 127", b2 >= 120 && b2 <= 135);
    ok("Spot red channel == 0", r2 == 0);
    ok("Spot green channel == 0", g2 == 0);

    /* Test 4: Specular lighting with Power and LOCALVIEWER */
    call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, 29 /* D3DRS_SPECULARENABLE */, 1 });
    mat.Specular.r = 1.0f; mat.Specular.g = 1.0f; mat.Specular.b = 1.0f; mat.Specular.a = 1.0f;
    mat.Power = 20.0f;
    call("IDirect3DDevice9::SetMaterial", 2, (uint32_t[]){ dev, A(G_copy(&mat, sizeof(mat))) });
    lt2.Specular.r = 1.0f; lt2.Specular.g = 1.0f; lt2.Specular.b = 1.0f; lt2.Specular.a = 1.0f;
    call("IDirect3DDevice9::SetLight", 3, (uint32_t[]){ dev, 2, A(G_copy(&lt2, sizeof(lt2))) });

    clear(dev, 0xFF000000u, 1, 1.0f);
    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    call("IDirect3DDevice9::DrawPrimitive", 4, (uint32_t[]){ dev, D3DPT_TRIANGLEFAN, 0, 4 });
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });
    read_all(pixels);
    uint32_t p3 = pixels[16 * 32 + 16];
    uint32_t r3 = (p3 >> 16) & 0xFFu, g3 = (p3 >> 8) & 0xFFu, b3 = p3 & 0xFFu;
    /* Specular white + Blue diffuse -> Red and Green are specular (> 0) and Blue is high */
    ok("Specular adds red highlight", r3 > 100);
    ok("Specular adds green highlight", g3 > 100);
    ok("Blue specular+diffuse is bright", b3 >= 200);

    /* Test 5: Global Ambient light */
    call("IDirect3DDevice9::LightEnable", 3, (uint32_t[]){ dev, 2, 0 });
    call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, 29 /* D3DRS_SPECULARENABLE */, 0 });
    /* Ambient = 0x00404040 (64/255 ~ 0.25). Material ambient = 0.2. Total ambient = 0.2 * 0.25 = 0.05 (~13) */
    call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, 139 /* D3DRS_AMBIENT */, 0x00404040u });
    clear(dev, 0xFF000000u, 1, 1.0f);
    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    call("IDirect3DDevice9::DrawPrimitive", 4, (uint32_t[]){ dev, D3DPT_TRIANGLEFAN, 0, 4 });
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });
    read_all(pixels);
    uint32_t p4 = pixels[16 * 32 + 16];
    uint32_t r4 = (p4 >> 16) & 0xFFu;
    ok("Ambient light produces expected dim color ~ 10-16", r4 >= 8 && r4 <= 20);

    /* Test 6: Light 7 (the highest index) must reach the GPU. While the FFP
     * uniform buffer was a fixed 1024 bytes, lights 6/7 were silently dropped
     * and this pixel stayed black. Directional red, material diffuse 0.5,
     * N.L = 1.0 -> red = 0.5 * 255 = 127.5 (~127), green/blue = 0. */
    call("IDirect3DDevice9::LightEnable", 3, (uint32_t[]){ dev, 0, 0 });
    call("IDirect3DDevice9::LightEnable", 3, (uint32_t[]){ dev, 1, 0 });
    call("IDirect3DDevice9::LightEnable", 3, (uint32_t[]){ dev, 2, 0 });
    call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, 139 /* D3DRS_AMBIENT */, 0x00000000u });
    call("IDirect3DDevice9::SetRenderState", 3, (uint32_t[]){ dev, 29 /* D3DRS_SPECULARENABLE */, 0 });
    memset(&mat, 0, sizeof(mat));
    mat.Diffuse.r = 0.5f; mat.Diffuse.g = 0.5f; mat.Diffuse.b = 0.5f; mat.Diffuse.a = 1.0f;
    call("IDirect3DDevice9::SetMaterial", 2, (uint32_t[]){ dev, A(G_copy(&mat, sizeof(mat))) });
    D3DLIGHT9 lt7;
    memset(&lt7, 0, sizeof(lt7));
    lt7.Type = D3DLIGHT_DIRECTIONAL;
    lt7.Diffuse.r = 1.0f; lt7.Diffuse.g = 0.0f; lt7.Diffuse.b = 0.0f; lt7.Diffuse.a = 1.0f;
    lt7.Direction.x = 0.0f; lt7.Direction.y = 0.0f; lt7.Direction.z = 1.0f;
    ok("SetLight(7) accepted", call("IDirect3DDevice9::SetLight", 3,
        (uint32_t[]){ dev, 7, A(G_copy(&lt7, sizeof(lt7))) }) == D3D_OK);
    call("IDirect3DDevice9::LightEnable", 3, (uint32_t[]){ dev, 7, 1 });

    clear(dev, 0xFF000000u, 1, 1.0f);
    call("IDirect3DDevice9::BeginScene", 1, (uint32_t[]){ dev });
    call("IDirect3DDevice9::DrawPrimitive", 4, (uint32_t[]){ dev, D3DPT_TRIANGLEFAN, 0, 4 });
    call("IDirect3DDevice9::EndScene", 1, (uint32_t[]){ dev });
    read_all(pixels);
    uint32_t p7 = pixels[16 * 32 + 16];
    uint32_t r7 = (p7 >> 16) & 0xFFu, g7 = (p7 >> 8) & 0xFFu, b7 = p7 & 0xFFu;
    ok("Light 7 red diffuse ~ 127 (upper light uploaded)", r7 >= 120 && r7 <= 135);
    ok("Light 7 green channel == 0", g7 == 0);
    ok("Light 7 blue channel == 0", b7 == 0);
    if (r7 < 120) printf("    got 0x%08X (light 7 lost?)\n", p7);

    call("IDirect3DVertexBuffer9::Release", 1, (uint32_t[]){ *pVB });
    printf(fails == 0 ? "test_d3d9_ffp_lighting: OK\n" : "test_d3d9_ffp_lighting: FAILED\n");
    return fails != 0;
}
