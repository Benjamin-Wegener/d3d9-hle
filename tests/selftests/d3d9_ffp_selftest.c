/* d3d9_ffp_selftest.c - fixed cases and (when available) real Metal compile. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "d3d9_ffp.h"
#include "win32hle.h"

static void none(void) { }
const recomp_dispatch_entry_t recomp_dispatch_table[] = { { 0x1000u, none } };
const uint32_t recomp_dispatch_count = 1;

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } \
                             else { printf("ok: %s\n", msg); } } while (0)

static int can_metal(void) {
    int rc = system("xcrun -sdk macosx metal --version >/dev/null 2>&1");
    return rc == 0;
}

static int compile_msl(const char *msl) {
    char src[64], obj[64];
    FILE *f;
    int rc;
    snprintf(src, sizeof src, "/tmp/ffp_%d.metal", (int)getpid());
    snprintf(obj, sizeof obj, "/tmp/ffp_%d.air", (int)getpid());
    f = fopen(src, "w");
    if (!f) return -1;
    fputs(msl, f);
    fclose(f);
    char cmd[256];
    snprintf(cmd, sizeof cmd, "xcrun -sdk macosx metal -std=macos-metal2.0 -c '%s' -o '%s' >/dev/null 2>&1", src, obj);
    rc = system(cmd);
    unlink(src); unlink(obj);
    return rc == 0 ? 0 : -1;
}

static void check_case(const char *name, const d3d9_ffp_desc_t *d, int want_ok, const char *must_contain) {
    char *msl = NULL; size_t len = 0; const char *err = NULL;
    int rc = d3d9_ffp_generate(d, &msl, &len, &err);
    if (!want_ok) {
        CHECK(rc != 0, name);
        free(msl);
        return;
    }
    CHECK(rc == 0 && msl && len > 0, name);
    if (rc == 0 && msl && must_contain) {
        CHECK(strstr(msl, must_contain) != NULL, "contains expected token");
    }
    if (rc == 0 && msl) {
        if (can_metal()) CHECK(compile_msl(msl) == 0, "metal compiles");
        else printf("ok: metal compile skipped (no xcrun metal)\n");
    }
    free(msl);
}

int main(void) {
    d3d9_ffp_stage_t modulate = { D3D9_TEXOP_MODULATE, D3D9_TA_TEXTURE, D3D9_TA_DIFFUSE,
                                  D3D9_TEXOP_SELECTARG1, D3D9_TA_TEXTURE, 0 };
    d3d9_ffp_stage_t sel_tex = { D3D9_TEXOP_SELECTARG1, D3D9_TA_TEXTURE, 0,
                                 D3D9_TEXOP_SELECTARG1, D3D9_TA_TEXTURE, 0 };
    d3d9_ffp_desc_t d;

    memset(&d, 0, sizeof d);
    d.fvf = D3D9_FVF_XYZ | D3D9_FVF_DIFFUSE;
    d.fog_mode = D3D9_FOG_NONE;
    check_case("xyz+diffuse unlit", &d, 1, "ffp_vs");

    memset(&d, 0, sizeof d);
    d.fvf = D3D9_FVF_XYZRHW | D3D9_FVF_DIFFUSE | (1u << D3D9_FVF_TEXCOUNT_SHIFT);
    d.stages = &modulate; d.stage_count = 1;
    check_case("xyzrhw+diffuse+tex1 modulate", &d, 1, "texture2d");

    memset(&d, 0, sizeof d);
    d.fvf = D3D9_FVF_XYZ | (1u << D3D9_FVF_TEXCOUNT_SHIFT);
    d.stages = &sel_tex; d.stage_count = 1;
    check_case("xyz+tex1 selectarg1", &d, 1, "sample");

    memset(&d, 0, sizeof d);
    d.fvf = D3D9_FVF_XYZ | D3D9_FVF_DIFFUSE;
    d.fog_mode = D3D9_FOG_LINEAR; d.fog_start = 0; d.fog_end = 100;
    check_case("linear fog", &d, 1, "fogFactor");

    memset(&d, 0, sizeof d);
    d.fvf = D3D9_FVF_XYZ | D3D9_FVF_DIFFUSE;
    d.alpha_test_enable = 1; d.alpha_func = D3D9_CMP_GREATEREQUAL; d.alpha_ref = 128;
    check_case("alpha test >=", &d, 1, "discard_fragment");

    /* NORMAL is now supported (needed for CAMERASPACE texgen). */
    memset(&d, 0, sizeof d);
    d.fvf = D3D9_FVF_XYZ | D3D9_FVF_NORMAL;
    check_case("xyz+normal unlit", &d, 1, "_csNormal");

    /* CAMERASPACE texgen modes require a normal in the FVF. */
    {
        d3d9_ffp_stage_t cs = { D3D9_TEXOP_SELECTARG1, D3D9_TA_TEXTURE, 0,
                                D3D9_TEXOP_SELECTARG1, D3D9_TA_TEXTURE, 0 };
        cs.texcoord_index = D3D9_TCI_CAMERASPACENORMAL;
        memset(&d, 0, sizeof d);
        d.fvf = D3D9_FVF_XYZ; d.stages = &cs; d.stage_count = 1;
        check_case("camera-space normal without FVF normal -> honest error", &d, 0, NULL);

        d.fvf = D3D9_FVF_XYZ | D3D9_FVF_NORMAL;
        check_case("camera-space normal", &d, 1, "_csNormal");
        cs.texcoord_index = D3D9_TCI_CAMERASPACEPOSITION;
        check_case("camera-space position", &d, 1, "_csPos");
        cs.texcoord_index = D3D9_TCI_CAMERASPACEREFLECTIONVECTOR;
        check_case("camera-space reflection", &d, 1, "reflect");
        cs.texcoord_index = D3D9_TCI_SPHEREMAP;
        check_case("sphere map", &d, 1, "_sm");

        /* TEXTURETRANSFORMFLAGS COUNT1..4 with PROJECTED: divisor depends on count. */
        cs.texcoord_index = 0;
        cs.texture_transform_flags = D3D9_TTFF_COUNT2 | D3D9_TTFF_PROJECTED;
        check_case("COUNT2|PROJECTED divides by _t.y", &d, 1, "_div = _t.y");
        cs.texture_transform_flags = D3D9_TTFF_COUNT3 | D3D9_TTFF_PROJECTED;
        check_case("COUNT3|PROJECTED divides by _t.z", &d, 1, "_div = _t.z");
        cs.texture_transform_flags = D3D9_TTFF_COUNT4 | D3D9_TTFF_PROJECTED;
        check_case("COUNT4|PROJECTED divides by _t.w", &d, 1, "_div = _t.w");
    }

    /* lighting and normal supported */
    memset(&d, 0, sizeof d);
    d.fvf = D3D9_FVF_XYZ | D3D9_FVF_NORMAL;
    d.lighting = 1;
    check_case("xyz+normal lit", &d, 1, "lit_diff");

    memset(&d, 0, sizeof d);
    d.fvf = D3D9_FVF_XYZ | D3D9_FVF_NORMAL | D3D9_FVF_DIFFUSE;
    d.lighting = 1;
    d.specular_enable = 1;
    d.color_vertex = 1;
    check_case("xyz+normal+diffuse lit specular", &d, 1, "lit_spec");

    printf("d3d9_ffp_selftest: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
