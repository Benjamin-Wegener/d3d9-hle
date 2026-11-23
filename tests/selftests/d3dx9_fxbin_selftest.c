/*
 * d3dx9_fxbin_selftest.c - the compiled-effect container parser against a blob
 * in the real d3dx9_34 layout (CTAB strings + header counts + parameter,
 * technique, pass, annotation and state records), including malformed input,
 * and the D3DXCreateEffectFromMemory -> SetTechnique -> Begin -> BeginPass ->
 * EndPass -> End path through the real COM objects.
 */
#define RECOMP_GENERATED_CODE
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "win32hle.h"
#include "host.h"
#include "d3dx9_effect_hle.h"
#include "d3dx9_fxbin_hle.h"

#define ENTRY_VA 0x00401000u
#define D3DERR_INVALIDCALL 0x8876086Cu

const recomp_dispatch_entry_t recomp_dispatch_table[1];
const uint32_t recomp_dispatch_count = 0;

static int fails;
static void ok(const char *what, int cond) { if (!cond) { printf("FAIL %s\n", what); fails++; } }

static uint32_t esp0;
#define XCALL(va, n, ...)                                                    \
    do {                                                                     \
        uint32_t _xa[8] = { __VA_ARGS__ };                                   \
        esp0 = esp;                                                          \
        for (int _xi = (int)(n) - 1; _xi >= 0; _xi--) PUSH32(esp, _xa[_xi]); \
        RECOMP_ICALL(va);                                                    \
        ok(#va " stack", esp == esp0);                                       \
    } while (0)

static void put32(uint8_t *b, uint32_t byte_off, uint32_t v) {
    b[byte_off] = (uint8_t)v; b[byte_off + 1] = (uint8_t)(v >> 8);
    b[byte_off + 2] = (uint8_t)(v >> 16); b[byte_off + 3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t *b, uint32_t byte_off) {
    return (uint32_t)b[byte_off] | ((uint32_t)b[byte_off + 1] << 8) |
           ((uint32_t)b[byte_off + 2] << 16) | ((uint32_t)b[byte_off + 3] << 24);
}

/* Real layout: CTAB of 48 bytes at DWORD 2; counts; 2 param records; one
 * technique with one annotation and two passes (one pass with one state). */
static uint32_t build_blob(uint32_t *size_out) {
    uint32_t va = hle_va(hle_alloc(2048));
    uint8_t *b = (uint8_t *)hle_ptr(va);
    static const struct { uint32_t off; const char *s; } str[] = {
        { 0, "Time" }, { 8, "World" }, { 16, "Base" },
        { 24, "Lit" }, { 32, "Shadow" }, { 40, "Note" }
    };
    memset(b, 0, 2048);
    for (size_t i = 0; i < sizeof str / sizeof *str; i++)
        memcpy(b + 8 + str[i].off, str[i].s, strlen(str[i].s) + 1);

    put32(b, 0, D3DXFX_TOKEN);
    put32(b, 4, 48u);                              /* CTAB size in bytes */
    put32(b, 8 + 12 * 4, 2u);                      /* param count */
    put32(b, 8 + 13 * 4, 1u);                      /* technique count */
    put32(b, 8 + 14 * 4, 0u);                      /* object count */
    put32(b, 8 + 15 * 4, 0u);                      /* group count */
    /* parameters (4 words each) */
    put32(b, 8 + 16 * 4, 0u);  put32(b, 8 + 17 * 4, 0u);
    put32(b, 8 + 18 * 4, 0x2004u); put32(b, 8 + 19 * 4, 0u);
    put32(b, 8 + 20 * 4, 8u);  put32(b, 8 + 21 * 4, 0u);
    put32(b, 8 + 22 * 4, 0x2004u); put32(b, 8 + 23 * 4, 0u);
    /* technique: Base, one annotation, two passes */
    put32(b, 8 + 24 * 4, 16u); put32(b, 8 + 25 * 4, 1u); put32(b, 8 + 26 * 4, 2u);
    put32(b, 8 + 27 * 4, 40u); put32(b, 8 + 28 * 4, 0u);   /* annotation Note */
    /* pass Lit: one state */
    put32(b, 8 + 29 * 4, 24u); put32(b, 8 + 30 * 4, 0u); put32(b, 8 + 31 * 4, 1u);
    put32(b, 8 + 32 * 4, 8u);  put32(b, 8 + 33 * 4, 1u);
    put32(b, 8 + 34 * 4, 0u);  put32(b, 8 + 35 * 4, 0u);   /* state value */
    /* pass Shadow: no states */
    put32(b, 8 + 36 * 4, 32u); put32(b, 8 + 37 * 4, 0u); put32(b, 8 + 38 * 4, 0u);
    *size_out = 8 + 39 * 4;
    return va;
}

static uint32_t V_pool, V_create_fx;
static uint32_t g_blob, g_blob_size, g_out, g_npass, g_name_base;

static void guest(void) {
    uint32_t _cf = 0; uint32_t _flag_k = FK_NONE; uint32_t _flag_a = 0, _flag_b = 0;
    uint32_t pool, effect, m[12], tech, npass;
    RECOMP_ENTER(ENTRY_VA);

    XCALL(V_pool, 1, g_out);
    ok("create pool", eax == 0u);
    pool = MEM32(g_out);
    ok("pool object", pool != 0u);

    XCALL(V_create_fx, 8, g_blob, g_blob_size, 0u, 0u, 0u, pool, g_out, 0u);
    ok("D3DXCreateEffectFromMemory", eax == 0u);
    effect = MEM32(g_out);
    ok("effect object", effect != 0u);
    if (!effect) return;

    /* public 79-slot ID3DXEffect layout: 13 GetTechniqueByName, 58 SetTechnique, 63 Begin,
     * 64 BeginPass, 66 EndPass, 67 End */
    static const int EMAP[12] = { 0, 1, 2, 63, 67, 64, 66, 65, 13, 15, 58, 59 };
    for (int i = 0; i < 12; i++) m[i] = MEM32(MEM32(effect) + 4u * (uint32_t)EMAP[i]);
    XCALL(m[8], 2, effect, g_name_base);
    ok("parsed technique lookup", eax != 0u);
    tech = eax;
    XCALL(m[10], 2, effect, tech);
    ok("SetTechnique", eax == 0u);
    XCALL(m[3], 3, effect, g_npass, 0u);
    npass = MEM32(g_npass);
    ok("Begin pass count", eax == 0u && npass == 2u);
    XCALL(m[5], 2, effect, 1u);
    ok("BeginPass", eax == 0u);
    XCALL(m[6], 1, effect);
    ok("EndPass", eax == 0u);
    XCALL(m[4], 1, effect);
    ok("End", eax == 0u);

    XCALL(m[2], 1, effect);
    ok("release parsed effect", eax == 0u);
    XCALL(MEM32(MEM32(pool) + 8u), 1, pool);       /* pool Release */
}

int main(void) {
    fxbin_desc_t desc;
    uint8_t *b;
    uint32_t saved_token, saved_csize, saved_param_name;
    recomp_host_init();
    win32hle_register(win32hle_d3d9fx);
    win32hle_register(win32hle_d3d9fxbin);
    V_pool = hle_resolve("D3DXCreateEffectPool");
    V_create_fx = hle_resolve("D3DXCreateEffectFromMemory");
    ok("shims resolve", V_pool && V_create_fx);

    g_blob = build_blob(&g_blob_size);
    ok("valid blob parses", hle_d3d9fxbin_parse((const uint8_t *)hle_ptr(g_blob), g_blob_size, &desc) == 1);
    ok("parsed params", desc.nparam == 2u && !strcmp(desc.param[0], "Time") && !strcmp(desc.param[1], "World"));
    ok("parsed technique", desc.ntech == 1u && !strcmp(desc.tech[0], "Base"));
    ok("parsed passes", desc.npass[0] == 2u && !strcmp(desc.pass[0][0], "Lit") && !strcmp(desc.pass[0][1], "Shadow"));

    b = (uint8_t *)hle_ptr(g_blob);
    saved_token = get32(b, 0);
    saved_csize = get32(b, 4);
    saved_param_name = get32(b, 8 + 16 * 4);
    put32(b, 0, 0u);
    ok("bad token rejected", hle_d3d9fxbin_parse(b, g_blob_size, &desc) == 0);
    put32(b, 0, saved_token);
    ok("truncated blob rejected", hle_d3d9fxbin_parse(b, g_blob_size - 4u, &desc) == 0);
    put32(b, 4, 0x7FFFFFFFu);
    ok("oversized CTAB rejected", hle_d3d9fxbin_parse(b, g_blob_size, &desc) == 0);
    put32(b, 4, saved_csize);
    put32(b, 8 + 16 * 4, 0x7FFFFFFFu);
    ok("out-of-range name offset rejected", hle_d3d9fxbin_parse(b, g_blob_size, &desc) == 0);
    put32(b, 8 + 16 * 4, saved_param_name);
    ok("restored blob parses again", hle_d3d9fxbin_parse(b, g_blob_size, &desc) == 1);

    g_out = hle_va(hle_alloc(4));
    g_npass = hle_va(hle_alloc(4));
    g_name_base = hle_guest_strdup("Base");

    mach_enter();
    guest();
    mach_leave();

    if (!fails) printf("d3dx9_fxbin_selftest: all checks passed\n");
    return fails != 0;
}
