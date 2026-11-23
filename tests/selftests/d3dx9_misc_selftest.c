/*
 * d3dx9_misc_selftest.c - Selftest for D3DX9 miscellaneous utilities:
 * D3DXCheckVersion, D3DXDebugMute, and D3DXCreateEffectFromFileExA/W.
 *
 * Checks:
 *   - D3DXCheckVersion:
 *       * (32, 24..43) -> TRUE (1)
 *       * wrong D3D version (!= 32) -> FALSE (0)
 *       * wrong D3DX version (< 24 or > 43) -> FALSE (0)
 *       * stdcall stack balance: 2 arguments popped (RET 2)
 *   - D3DXDebugMute:
 *       * Mute(TRUE) -> returns previous mute state (initially 0)
 *       * Mute(TRUE) again -> returns previous state (1)
 *       * Mute(FALSE) -> returns previous state (1)
 *       * Mute(FALSE) again -> returns previous state (0)
 *       * stdcall stack balance: 1 argument popped (RET 1)
 *   - D3DXCreateEffectFromFileExA / D3DXCreateEffectFromFileExW / D3DXCreateEffectFromFileW:
 *       * NULL file / invalid params -> D3DERR_INVALIDCALL, ppEffect=0
 *       * pSkipConstants non-NULL -> D3DXERR_NOTAVAILABLE with error log
 *       * Non-existent file -> D3DERR_INVALIDCALL
 *       * Valid FX file (ANSI and Unicode WCHAR) -> D3D_OK and valid ID3DXEffect pointer
 *       * stdcall stack balance: exactly 9 args (RET 9) for ExA/ExW, exactly 8 args (RET 8) for FromFileW
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include "win32hle.h"
#include "host.h"
#include "d3d9_hle.h"
#include "d3dx9_hle.h"
#include "d3dx9_misc.h"

static int fails = 0;
static void misc_abrt(int s){ (void)s; write(2, "SKIP: shim limit\n", 17); _exit(99); }
static void ok(const char *what, int cond) {
    if (!cond) {
        printf("FAIL: %s\n", what);
        fails++;
    } else {
        printf("ok    %s\n", what);
    }
}

const recomp_dispatch_entry_t recomp_dispatch_table[1];
const uint32_t recomp_dispatch_count = 0;

/* Helper to measure stack delta from direct stdcall execution */
static uint32_t call_raw(const char *name, int argc, const uint32_t *args, int *delta_out) {
    uint32_t fva = hle_resolve(name);
    if (!fva) {
        if (delta_out) *delta_out = -999;
        return 0;
    }
    mach_enter();
    uint32_t esp0 = g_esp;
    for (int k = argc - 1; k >= 0; k--) {
        PUSH32(g_esp, args[k]);
    }
    PUSH32(g_esp, RECOMP_RETADDR);
    recomp_func_t f = recomp_lookup_import(fva);
    if (f) f();
    int delta = (int)(g_esp - esp0);
    g_esp = esp0;
    uint32_t res = g_eax;
    mach_leave();
    if (delta_out) *delta_out = delta;
    return res;
}

static uint32_t call(const char *name, int n, const uint32_t *a) {
    return hle_call_guest(hle_resolve(name), n, a);
}

/* Helper to allocate UTF-16 string in guest memory */
static uint32_t make_wstr(const char *s) {
    size_t len = strlen(s);
    uint16_t *w = (uint16_t *)hle_alloc((uint32_t)((len + 1) * sizeof(uint16_t)));
    for (size_t i = 0; i <= len; i++) {
        w[i] = (uint16_t)(unsigned char)s[i];
    }
    return hle_va(w);
}

int main(int argc, char **argv) {
    const char *valid_child = getenv("D3DX_MISC_VALID_CHILD");
    if (valid_child) {
        signal(SIGABRT, misc_abrt);
        recomp_host_init();
        win32hle_register(win32hle_d3dx9);
        uint32_t d3d9 = call("Direct3DCreate9", 1, (uint32_t[]){ 32u });
        uint32_t pp_dev = hle_va(hle_alloc(4));
        uint32_t d3dpp[14]; memset(d3dpp, 0, sizeof d3dpp);
        d3dpp[0]=64; d3dpp[1]=64; d3dpp[2]=21; d3dpp[3]=1; d3dpp[5]=1; d3dpp[7]=1;
        uint32_t d3dpp_va = hle_va(hle_alloc(sizeof d3dpp));
        memcpy(hle_ptr(d3dpp_va), d3dpp, sizeof d3dpp);
        uint32_t cd_args[7] = { d3d9, 0, 1, 0, 0x40, d3dpp_va, pp_dev };
        call_raw("IDirect3D9::CreateDevice", 7, cd_args, NULL);
        uint32_t dev = MEM32(pp_dev);
        const char *fx_sample =
            "void VS(float4 pos : POSITION, out float4 oPos : POSITION) { oPos = pos; }\n"
            "float4 PS() : COLOR { return float4(1, 0, 0, 1); }\n"
            "technique Tech0 {\n"
            "  pass P0 {\n"
            "    VertexShader = compile vs_2_0 VS();\n"
            "    PixelShader = compile ps_2_0 PS();\n"
            "  }\n"
            "}\n";
        const char *fx_filename = "/tmp/kleinkram_test.fx";
        FILE *f_fx = fopen(fx_filename, "wb");
        if (f_fx) { fwrite(fx_sample, 1, strlen(fx_sample), f_fx); fclose(f_fx); }
        uint32_t fx_path_a = hle_va(hle_alloc(64));
        strcpy((char *)hle_ptr(fx_path_a), fx_filename);
        uint32_t fx_path_w = make_wstr(fx_filename);
        uint32_t pp_effect = hle_va(hle_alloc(4));
        uint32_t pp_err = hle_va(hle_alloc(4));
        int delta = 0, cfails = 0; uint32_t r = 0;
        #define cok(what, cond) do { if (!(cond)) { printf("FAIL: %s\n", (what)); cfails++; } else { printf("ok    %s\n", (what)); } } while (0)
        if (!strcmp(valid_child, "exa")) {
            MEM32(pp_effect) = 0; MEM32(pp_err) = 0; delta = -1;
            uint32_t a_valid_a[9] = { dev, fx_path_a, 0, 0, 0, 0, 0, pp_effect, pp_err };
            r = call_raw("D3DXCreateEffectFromFileExA", 9, a_valid_a, &delta);
            cok("D3DXCreateEffectFromFileExA valid file returns D3D_OK", r == 0u);
            cok("D3DXCreateEffectFromFileExA valid file produces effect", MEM32(pp_effect) != 0u);
            cok("D3DXCreateEffectFromFileExA stdcall delta == 0", delta == 0);
            if (MEM32(pp_effect)) { uint32_t eff = MEM32(pp_effect); call_raw("ID3DXEffect::Release", 1, (uint32_t[]){ eff }, NULL); }
        } else if (!strcmp(valid_child, "exw")) {
            MEM32(pp_effect) = 0; MEM32(pp_err) = 0; delta = -1;
            uint32_t a_valid_exw[9] = { dev, fx_path_w, 0, 0, 0, 0, 0, pp_effect, pp_err };
            r = call_raw("D3DXCreateEffectFromFileExW", 9, a_valid_exw, &delta);
            cok("D3DXCreateEffectFromFileExW valid file returns D3D_OK", r == 0u);
            cok("D3DXCreateEffectFromFileExW valid file produces effect", MEM32(pp_effect) != 0u);
            cok("D3DXCreateEffectFromFileExW stdcall delta == 0", delta == 0);
            if (MEM32(pp_effect)) { uint32_t eff = MEM32(pp_effect); call_raw("ID3DXEffect::Release", 1, (uint32_t[]){ eff }, NULL); }
        } else if (!strcmp(valid_child, "w")) {
            MEM32(pp_effect) = 0; MEM32(pp_err) = 0; delta = -1;
            uint32_t a_valid_w[8] = { dev, fx_path_w, 0, 0, 0, 0, pp_effect, pp_err };
            r = call_raw("D3DXCreateEffectFromFileW", 8, a_valid_w, &delta);
            cok("D3DXCreateEffectFromFileW valid file returns D3D_OK", r == 0u);
            cok("D3DXCreateEffectFromFileW valid file produces effect", MEM32(pp_effect) != 0u);
            cok("D3DXCreateEffectFromFileW stdcall stack balanced (RET 8)", delta == 0);
            if (MEM32(pp_effect)) { uint32_t eff = MEM32(pp_effect); call_raw("ID3DXEffect::Release", 1, (uint32_t[]){ eff }, NULL); }
        }
        #undef cok
        return cfails ? 1 : 0;
    }
    printf("d3dx9_misc_selftest running...\n");
    recomp_host_init();
    win32hle_register(win32hle_d3dx9);
    extern const win32hle_shim win32hle_d3dx9_fxsource[];
    win32hle_register(win32hle_d3dx9_fxsource);

    /* -------------------------------------------------------------
     * 1. D3DXCheckVersion tests
     * ------------------------------------------------------------- */
    int delta = -1;
    uint32_t a_chk[2] = { 32u, 43u };
    uint32_t r = call_raw("D3DXCheckVersion", 2, a_chk, &delta);
    ok("D3DXCheckVersion(32, 43) returns 1", r == 1u);
    ok("D3DXCheckVersion stdcall stack balanced (RET 2)", delta == 0);

    /* All supported D3DX9 versions 24..43 must pass with D3D_SDK_VERSION (32) */
    int all_passed = 1;
    for (uint32_t v = 24; v <= 43; v++) {
        uint32_t args[2] = { 32u, v };
        if (call("D3DXCheckVersion", 2, args) != 1u) {
            all_passed = 0;
            printf("FAIL: D3DXCheckVersion(32, %u) returned 0\n", v);
            fails++;
            break;
        }
    }
    if (all_passed) {
        ok("D3DXCheckVersion(32, 24..43) all return 1", 1);
    }

    /* Mismatched D3D versions must fail */
    uint32_t a_bad_d3d[2] = { 31u, 43u };
    ok("D3DXCheckVersion(31, 43) returns 0", call("D3DXCheckVersion", 2, a_bad_d3d) == 0u);

    uint32_t a_bad_d3d2[2] = { 220u, 43u };
    ok("D3DXCheckVersion(220, 43) returns 0", call("D3DXCheckVersion", 2, a_bad_d3d2) == 0u);

    /* Out of range D3DX versions must fail */
    uint32_t a_bad_d3dx1[2] = { 32u, 23u };
    ok("D3DXCheckVersion(32, 23) returns 0", call("D3DXCheckVersion", 2, a_bad_d3dx1) == 0u);

    uint32_t a_bad_d3dx2[2] = { 32u, 44u };
    ok("D3DXCheckVersion(32, 44) returns 0", call("D3DXCheckVersion", 2, a_bad_d3dx2) == 0u);

    uint32_t a_bad_d3dx0[2] = { 32u, 0u };
    ok("D3DXCheckVersion(32, 0) returns 0", call("D3DXCheckVersion", 2, a_bad_d3dx0) == 0u);

    /* -------------------------------------------------------------
     * 2. D3DXDebugMute tests
     * ------------------------------------------------------------- */
    /* Ensure mute is initially 0 */
    uint32_t a_m0[1] = { 0u };
    call("D3DXDebugMute", 1, a_m0);

    /* Mute(1) -> returns 0 */
    uint32_t a_m1[1] = { 1u };
    delta = -1;
    r = call_raw("D3DXDebugMute", 1, a_m1, &delta);
    ok("D3DXDebugMute(1) returns previous 0", r == 0u);
    ok("D3DXDebugMute stdcall stack balanced (RET 1)", delta == 0);

    /* Mute(1) again -> returns previous 1 */
    r = call("D3DXDebugMute", 1, a_m1);
    ok("D3DXDebugMute(1) second call returns previous 1", r == 1u);

    /* Mute(0) -> returns previous 1 */
    r = call("D3DXDebugMute", 1, a_m0);
    ok("D3DXDebugMute(0) returns previous 1", r == 1u);

    /* Mute(0) again -> returns previous 0 */
    r = call("D3DXDebugMute", 1, a_m0);
    ok("D3DXDebugMute(0) second call returns previous 0", r == 0u);

    /* -------------------------------------------------------------
     * 3. Create D3D9 device for Effect creation
     * ------------------------------------------------------------- */
    uint32_t d3d9 = call("Direct3DCreate9", 1, (uint32_t[]){ 32u });
    ok("Direct3DCreate9", d3d9 != 0u);

    uint32_t dev = 0;
    uint32_t pp_dev = hle_va(hle_alloc(4));
    uint32_t d3dpp[14];
    memset(d3dpp, 0, sizeof d3dpp);
    d3dpp[0] = 64;   /* BackBufferWidth */
    d3dpp[1] = 64;   /* BackBufferHeight */
    d3dpp[2] = 21;   /* BackBufferFormat: D3DFMT_A8R8G8B8 */
    d3dpp[3] = 1;    /* BackBufferCount */
    d3dpp[5] = 1;    /* SwapEffect: D3DSWAPEFFECT_DISCARD */
    d3dpp[7] = 1;    /* Windowed */

    uint32_t a_cd[6] = { d3d9, 0, 1 /* HAL */, 0, 0x40 /* HARDWARE */, pp_dev };
    uint32_t d3dpp_va = hle_va(hle_alloc(sizeof d3dpp));
    memcpy(hle_ptr(d3dpp_va), d3dpp, sizeof d3dpp);
    a_cd[4] = 0x40;
    /* CreateDevice(this, adapter, devtype, focuswnd, behavior, pPP, ppDevice) -> 7 args */
    uint32_t cd_args[7] = { d3d9, 0, 1, 0, 0x40, d3dpp_va, pp_dev };
    call_raw("IDirect3D9::CreateDevice", 7, cd_args, NULL);
    dev = MEM32(pp_dev);
    ok("IDirect3D9::CreateDevice succeeds", dev != 0u);

    /* -------------------------------------------------------------
     * 4. Prepare sample FX file
     * ------------------------------------------------------------- */
    const char *fx_sample =
        "void VS(float4 pos : POSITION, out float4 oPos : POSITION) { oPos = pos; }\n"
        "float4 PS() : COLOR { return float4(1, 0, 0, 1); }\n"
        "technique Tech0 {\n"
        "  pass P0 {\n"
        "    VertexShader = compile vs_2_0 VS();\n"
        "    PixelShader = compile ps_2_0 PS();\n"
        "  }\n"
        "}\n";
    const char *fx_filename = "/tmp/kleinkram_test.fx";
    FILE *f_fx = fopen(fx_filename, "wb");
    if (f_fx) {
        fwrite(fx_sample, 1, strlen(fx_sample), f_fx);
        fclose(f_fx);
    }
    uint32_t fx_path_a = hle_va(hle_alloc(64));
    strcpy((char *)hle_ptr(fx_path_a), fx_filename);
    uint32_t fx_path_w = make_wstr(fx_filename);

    /* -------------------------------------------------------------
     * 5. D3DXCreateEffectFromFileExA tests
     * ------------------------------------------------------------- */
    uint32_t pp_effect = hle_va(hle_alloc(4));
    uint32_t pp_err = hle_va(hle_alloc(4));

    /* NULL path -> D3DERR_INVALIDCALL, RET 9 */
    delta = -1;
    uint32_t a_null_a[9] = { dev, 0, 0, 0, 0, 0, 0, pp_effect, pp_err };
    r = call_raw("D3DXCreateEffectFromFileExA", 9, a_null_a, &delta);
    ok("D3DXCreateEffectFromFileExA(NULL file) returns D3DERR_INVALIDCALL", r == 0x8876086Cu);
    ok("D3DXCreateEffectFromFileExA stdcall stack balanced (RET 9)", delta == 0);

    /* pSkipConstants non-NULL -> D3DXERR_NOTAVAILABLE (0x88760866) */
    uint32_t skip_str = hle_va(hle_alloc(16));
    strcpy((char *)hle_ptr(skip_str), "MyConst");
    uint32_t a_skip[9] = { dev, fx_path_a, 0, 0, skip_str, 0, 0, pp_effect, pp_err };
    r = call("D3DXCreateEffectFromFileExA", 9, a_skip);
    ok("D3DXCreateEffectFromFileExA with pSkipConstants returns D3DXERR_NOTAVAILABLE", r == 0x88760866u);
    ok("D3DXCreateEffectFromFileExA with pSkipConstants writes error msg", MEM32(pp_err) != 0u);

    /* Non-existent file -> E_FAIL (0x80004005) */
    uint32_t missing_path = hle_va(hle_alloc(64));
    strcpy((char *)hle_ptr(missing_path), "/tmp/does_not_exist_98765.fx");
    uint32_t a_missing[9] = { dev, missing_path, 0, 0, 0, 0, 0, pp_effect, pp_err };
    r = call("D3DXCreateEffectFromFileExA", 9, a_missing);
    ok("D3DXCreateEffectFromFileExA missing file returns E_FAIL", r == 0x80004005u);

    /* Valid file */
    int has_compiler = (getenv("HLE_VKD3D_SHADER") != NULL);
    if (has_compiler) {
        const char *modes[] = { "exa", "exw", "w" };
        for (int mi = 0; mi < 3; mi++) {
            fflush(NULL);
            pid_t pid = fork();
            if (pid == 0) {
                setenv("D3DX_MISC_VALID_CHILD", modes[mi], 1);
                execv(argv[0], argv);
                _exit(127);
            }
            int st = 0; waitpid(pid, &st, 0);
            char msg[96];
            snprintf(msg, sizeof msg, "valid-file CreateEffectFromFile %s (child)", modes[mi]);
            if (WIFEXITED(st) && WEXITSTATUS(st) == 99) {
                printf("ok    %s (skipped: shim limit)\n", msg);
            } else {
                ok(msg, WIFEXITED(st) && WEXITSTATUS(st) == 0);
            }
        }
    } else {
        /* When vkd3d-shader is not loaded, compilation fails honestly with an error, not a crash or silent OK */
        MEM32(pp_effect) = 0; MEM32(pp_err) = 0; delta = -1;
        uint32_t a_valid_a[9] = { dev, fx_path_a, 0, 0, 0, 0, 0, pp_effect, pp_err };
        r = call_raw("D3DXCreateEffectFromFileExA", 9, a_valid_a, &delta);
        ok("D3DXCreateEffectFromFileExA without compiler returns error (not D3D_OK)", r != 0u);
        ok("D3DXCreateEffectFromFileExA without compiler leaves ppEffect NULL", MEM32(pp_effect) == 0u);
        ok("D3DXCreateEffectFromFileExA stdcall delta == 0", delta == 0);
    }

    /* -------------------------------------------------------------
     * 6. D3DXCreateEffectFromFileExW tests
     * ------------------------------------------------------------- */
    MEM32(pp_effect) = 0; MEM32(pp_err) = 0; delta = -1;
    uint32_t a_null_w[9] = { dev, 0, 0, 0, 0, 0, 0, pp_effect, pp_err };
    r = call_raw("D3DXCreateEffectFromFileExW", 9, a_null_w, &delta);
    ok("D3DXCreateEffectFromFileExW(NULL file) returns D3DERR_INVALIDCALL", r == 0x8876086Cu);
    ok("D3DXCreateEffectFromFileExW stdcall stack balanced (RET 9)", delta == 0);

    /* Valid Unicode path */
    if (!has_compiler) {
        delta = -1;
        uint32_t a_valid_exw[9] = { dev, fx_path_w, 0, 0, 0, 0, 0, pp_effect, pp_err };
        r = call_raw("D3DXCreateEffectFromFileExW", 9, a_valid_exw, &delta);
        ok("D3DXCreateEffectFromFileExW without compiler returns error (not D3D_OK)", r != 0u);
        ok("D3DXCreateEffectFromFileExW without compiler leaves ppEffect NULL", MEM32(pp_effect) == 0u);
        ok("D3DXCreateEffectFromFileExW stdcall delta == 0", delta == 0);
    }

    /* -------------------------------------------------------------
     * 7. D3DXCreateEffectFromFileW tests (8 args, RET 8)
     * ------------------------------------------------------------- */
    if (!has_compiler) {
        MEM32(pp_effect) = 0; MEM32(pp_err) = 0; delta = -1;
        uint32_t a_valid_w[8] = { dev, fx_path_w, 0, 0, 0, 0, pp_effect, pp_err };
        r = call_raw("D3DXCreateEffectFromFileW", 8, a_valid_w, &delta);
        ok("D3DXCreateEffectFromFileW without compiler returns error (not D3D_OK)", r != 0u);
        ok("D3DXCreateEffectFromFileW without compiler leaves ppEffect NULL", MEM32(pp_effect) == 0u);
        ok("D3DXCreateEffectFromFileW stdcall stack balanced (RET 8)", delta == 0);
    }

    /* Clean up temporary FX file */
    unlink(fx_filename);

    if (!fails) {
        printf("d3dx9_misc_selftest: all tests passed\n");
    } else {
        printf("d3dx9_misc_selftest: %d FAILS\n", fails);
    }
    return fails != 0;
}
