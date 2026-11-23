/*
 * d3dx9_asm_selftest.c - D3DXAssembleShader tested as a guest sees it.
 *
 *  - fixed, hand-computed token streams (public d3d9types.h encoding) for
 *    vs_1_1 .. vs_3_0 and ps_1_1 .. ps_3_0, compared word for word;
 *  - the emitted bytecode is fed to the real d3d9_shader translator and must
 *    produce the expected MSL (no placeholder/Attrappe);
 *  - errors name the 1-based source line;
 *  - the returned ID3DXBuffer's five public methods are each called through
 *    the guest vtable with exact stdcall stack balance;
 *  - the vtable has exactly five slots (public d3dx9shader.h ID3DXBuffer).
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "host.h"
#include "d3dx9_asm.h"
#include "d3dx9_hle.h"
#include "d3d9_shader.h"

/* guest pointer to IID_IUnknown {00000000-0000-0000-C000-000000000046} */
static uint32_t iid_unk(void) {
    uint8_t *g = (uint8_t *)hle_alloc(16);
    memset(g, 0, 16); g[8] = 0xC0; g[15] = 0x46;
    return hle_va(g);
}

static int fails;
static void ok(const char *what, int cond) {
    if (!cond) { printf("FAIL %s\n", what); fails++; }
}
static uint32_t call(const char *name, int n, const uint32_t *a) {
    return hle_call_guest(hle_resolve(name), n, a);
}
#define A(p) hle_va(p)

/* Raw stdcall call: hle_call_guest saves/restores esp, so only this form can
 * prove the shim pops exactly ret + argc*4 bytes. */
static int balanced(const char *name, int n, const uint32_t *a) {
    uint32_t va = hle_resolve(name);
    int good;
    mach_enter();
    uint32_t esp0 = g_esp;
    for (int i = n - 1; i >= 0; i--) PUSH32(g_esp, a[i]);
    PUSH32(g_esp, RECOMP_RETADDR);
    recomp_lookup_import(va)();
    good = (g_esp == esp0);
    g_esp = esp0;
    mach_leave();
    return good;
}

const recomp_dispatch_entry_t recomp_dispatch_table[1];
const uint32_t recomp_dispatch_count = 0;

/* Exact public token values, computed independently of the assembler. */
#define V30 0xFFFE0300u
#define P30 0xFFFF0300u
#define INS(op, n) ((uint32_t)(op) | ((uint32_t)(n) << 24u))
#define REG(t, n) ((((uint32_t)(t) & 7u) << 28u) | ((((uint32_t)(t) >> 3u) & 3u) << 11u) | (uint32_t)(n))
#define DST(t, n, m) (0x80000000u | REG(t, n) | ((uint32_t)(m) << 16u))
#define SRC(t, n) (REG(t, n) | 0x00E40000u) /* .xyzw */
#define END 0x0000FFFFu

static int direct_ok(const char *src, const uint32_t *want, uint32_t n,
                     const char *label) {
    uint32_t *w = NULL, cnt = 0;
    char err[256];
    int rc = d3dx9_assemble(src, 0, &w, &cnt, err, sizeof(err));
    int good = (rc == 0 && cnt == n);
    if (good) for (uint32_t i = 0; i < n; i++)
        if (w[i] != want[i]) {
            printf("  %s token[%u] = 0x%08X, want 0x%08X\n",
                   label, i, w[i], want[i]);
            good = 0;
        }
    if (rc != 0) printf("  %s error: %s\n", label, err);
    ok(label, good);
    free(w);
    return good;
}

/* Call a vtable method exactly the way lifted guest code would, checking
 * that it pops ret + argc*4 bytes and returns the value we expect. */
static int vcall_balanced(uint32_t vt, int slot, int argc,
                          const uint32_t *args, uint32_t want_eax,
                          const char *label) {
    uint32_t fn = MEM32(vt + 4u * (uint32_t)slot);
    int good = fn != 0;
    if (good) {
        mach_enter();
        uint32_t esp0 = g_esp;
        for (int i = argc - 1; i >= 0; i--) PUSH32(g_esp, args[i]);
        PUSH32(g_esp, RECOMP_RETADDR);
        recomp_lookup_import(fn)();
        good = (g_esp == esp0) && (want_eax == 0xFFFFFFFFu || g_eax == want_eax);
        g_esp = esp0;
        mach_leave();
    }
    ok(label, good);
    return good;
}

static int msl_ok(const uint32_t *words, uint32_t n, int ps,
                  const char *needle, const char *label) {
    char msl[16384], err[256];
    int rc = d3d9_translate_shader_to_msl(words, n, ps, msl, sizeof(msl),
                                          err, sizeof(err));
    int good = (rc == 0 && (needle == NULL || strstr(msl, needle) != NULL));
    if (!good && rc == 0) printf("  %s: no '%s' in MSL\n", label, needle);
    if (rc != 0) printf("  %s: translate: %s\n", label, err);
    ok(label, good);
    return good;
}

/* Assemble through the real HLE shim and hand back the shader buffer VA. */
static uint32_t assemble_guest(const char *src, uint32_t *err_out) {
    void *gsrc = hle_alloc(1024);
    uint32_t *pShader = (uint32_t *)hle_alloc(4);
    uint32_t *pErr = (uint32_t *)hle_alloc(4);
    uint32_t a[7];
    memset(gsrc, 0, 1024);
    *pShader = 0; *pErr = 0;
    memcpy(gsrc, src, strlen(src) + 1);
    a[0] = A(gsrc); a[1] = (uint32_t)strlen(src); a[2] = 0;
    a[3] = 0; a[4] = 0; a[5] = A(pShader); a[6] = A(pErr);
    ok("D3DXAssembleShader stdcall stack balanced", balanced("D3DXAssembleShader", 7, a));
    call("D3DXAssembleShader", 7, a);
    if (err_out) *err_out = *pErr;
    return *pShader;
}

int main(void) {
    recomp_host_init();
    win32hle_register(win32hle_d3dx9);

    /* 1. Canonical vs_3_0: exactly the fixed tokens used by d3d9_selftest. */
    {
        static const uint32_t want[] = {
            V30,
            INS(0x1F, 2), 0u, REG(1, 0),              /* dcl_position v0 */
            INS(0x01, 2), DST(4, 0, 0xF), SRC(1, 0),   /* mov oPos, v0 */
            END
        };
        direct_ok("vs_3_0\ndcl_position v0\nmov oPos, v0\n",
                  want, sizeof(want)/sizeof(want[0]), "vs_3_0 fixed tokens");
        {
            char m[16384], e[256];
            int rc = d3d9_translate_shader_to_msl(want, 8, 0, m, sizeof(m), e, sizeof(e));
            ok("vs_3_0 MSL has vs_main", rc == 0 && strstr(m, "vs_main") != NULL);
        }
    }

    /* 2. ps_3_0 sampler + constants, tokens computed from the public header. */
    {
        static const uint32_t want[] = {
            P30,
            INS(0x1F, 2), 2u << 27u, REG(10, 0),        /* dcl_2d s0 */
            INS(0x01, 2), DST(8, 0, 0xF), SRC(2, 0),    /* mov oC0, c0 */
            END
        };
        direct_ok("ps_3_0\ndcl_2d s0\nmov oC0, c0\n",
                  want, sizeof(want)/sizeof(want[0]), "ps_3_0 fixed tokens");
        msl_ok(want, sizeof(want)/sizeof(want[0]), 1, "oC[0]",
               "ps_3_0 accepted by translator");
    }

    /* 3. swizzles, write masks and source modifiers are real token bits. */
    {
        static const uint32_t want[] = {
            V30,
            INS(0x02, 3), DST(0, 0, 0x3), REG(1, 0) | 0x00E10000u,
            REG(2, 1) | 0x00E40000u | (1u << 24u),      /* -c1 */
            END
        };
        direct_ok("vs_3_0\nadd r0.xy, v0.yxzw, -c1\n",
                  want, sizeof(want)/sizeof(want[0]), "swizzle/mask/neg tokens");
        msl_ok(want, sizeof(want)/sizeof(want[0]), 0, NULL,
               "swizzle/mask shader translates");
    }

    /* 4. def carries float bit patterns; defi carries integer patterns. */
    {
        uint32_t *w = NULL, n = 0;
        char err[256];
        float vals[4] = { 1.0f, -2.0f, 0.5f, 3.0f };
        uint32_t fb[4];
        memcpy(fb, vals, sizeof(fb));
        uint32_t want[] = {
            V30,
            INS(0x51, 5), DST(2, 0, 0xF), fb[0], fb[1], fb[2], fb[3],
            INS(0x01, 2), DST(0, 0, 0xF), SRC(2, 0),
            END
        };
        int rc = d3dx9_assemble("vs_3_0\ndef c0, 1.0, -2.0, 0.5, 3.0\nmov r0, c0\n",
                                0, &w, &n, err, sizeof(err));
        int good = rc == 0 && n == sizeof(want)/sizeof(want[0]);
        if (good) for (uint32_t i = 0; i < n; i++) if (w[i] != want[i]) good = 0;
        ok("def float bit patterns", good);
        if (rc == 0) msl_ok(w, n, 0, "float4(1,-2,0.5,3)", "def in MSL");
        free(w);
    }

    /* 5. Relative addressing and all public shader versions assemble. */
    {
        /* In vs_3_0, relative addressing c[a0+3] emits the c[3] rel token plus an
         * extra address-register token (bit 31 | a0), and instruction length is 3. */
        static const uint32_t want[] = {
            V30,
            INS(0x01, 3), DST(0, 0, 0xF),
            REG(2, 3) | (1u << 13u) | 0x00E40000u,     /* c[a0+3] */
            0x80000000u | REG(3, 0),                    /* a0 relative address token */
            END
        };
        direct_ok("vs_3_0\nmov r0, c[a0+3]\n", want,
                  sizeof(want)/sizeof(want[0]), "relative address token");
        /* Also verify that d3d9_translate_shader_to_msl accepts this bytecode. */
        {
            char m[16384], e[256];
            int rc = d3d9_translate_shader_to_msl(want, sizeof(want)/sizeof(want[0]), 0, m, sizeof(m), e, sizeof(e));
            ok("vs_3_0 relative MSL translation", rc == 0 && strstr(m, "c[int(s.a0.x) + 3]") != NULL);
        }
    }
    /* 5b. Source modifiers (e.g. _abs, _bias, _x2). */
    {
        uint32_t *w = NULL, n = 0;
        char err[256];
        int rc = d3dx9_assemble("vs_3_0\nmov r0, r1_abs\nadd r0, r1_bias, r2_x2\n",
                                0, &w, &n, err, sizeof(err));
        ok("source modifiers assemble without error", rc == 0);
        if (rc == 0) {
            /* Verify modifiers encoded in src tokens: _abs = 11 (0x0B000000), _bias = 2 (0x02000000), _x2 = 7 (0x07000000) */
            ok("mov r1_abs has mod 11", (w[3] >> 24) == 11u);
            ok("add r1_bias has mod 2", (w[6] >> 24) == 2u);
            ok("add r2_x2 has mod 7", (w[7] >> 24) == 7u);
            char m[16384], e[256];
            int trc = d3d9_translate_shader_to_msl(w, n, 0, m, sizeof(m), e, sizeof(e));
            ok("source modifiers MSL translation", trc == 0 && strstr(m, "abs(") != NULL);
            free(w);
        }
    }
    {
        const char *vers[] = { "vs_1_1", "vs_2_0", "vs_2_1", "vs_3_0",
                               "ps_1_1", "ps_1_2", "ps_1_3", "ps_1_4",
                               "ps_2_0", "ps_2_1", "ps_3_0" };
        for (unsigned i = 0; i < sizeof(vers)/sizeof(vers[0]); i++) {
            char src[128], label[64];
            uint32_t *w = NULL, n = 0;
            char err[256];
            snprintf(src, sizeof(src), "%s\nmov r0, r0\n", vers[i]);
            int rc = d3dx9_assemble(src, 0, &w, &n, err, sizeof(err));
            snprintf(label, sizeof(label), "model %s", vers[i]);
            ok(label, rc == 0 && n == 5 && w[0] != 0);
            free(w);
        }
    }

    /* 6. Error messages carry the 1-based source line. */
    {
        uint32_t *w = NULL, n = 0;
        char err[256];
        int rc = d3dx9_assemble("vs_3_0\nmov r0, r0\nfrobnicate r0, r0\n",
                                0, &w, &n, err, sizeof(err));
        ok("bad opcode rejected", rc != 0 && w == NULL);
        ok("error names line 3", strstr(err, "line 3") != NULL);
        if (rc != 0) printf("  error text: %s\n", err);
        free(w);
    }

    /* 7. The real shim returns an ID3DXBuffer with public ABI. */
    {
        uint32_t pErr = 0;
        uint32_t obj = assemble_guest("vs_3_0\nmov r0, r0\n", &pErr);
        ok("shim created shader buffer", obj != 0);
        if (obj && pErr) {
            uint32_t vt = MEM32(obj);
            uint32_t vp = 0, vs = 0;
            uint32_t a1[1] = { obj };
            /* public ID3DXBuffer methods, in d3dx9shader.h order */
            vcall_balanced(vt, 0, 3, (uint32_t[]){ obj, iid_unk(), hle_va(hle_alloc(4)) }, 0u, "QI slot balanced");
            vcall_balanced(vt, 1, 1, a1, 0xFFFFFFFFu, "AddRef slot balanced");
            vcall_balanced(vt, 2, 1, a1, 0xFFFFFFFFu, "Release slot balanced");
            vcall_balanced(vt, 3, 1, a1, 0xFFFFFFFFu, "GetBufferPointer balanced");
            vcall_balanced(vt, 4, 1, a1, 0xFFFFFFFFu, "GetBufferSize balanced");
            ok("vtable has exactly 5 slots",
               MEM32(vt) != 0 && MEM32(vt + 4) != 0 && MEM32(vt + 8) != 0 &&
               MEM32(vt + 12) != 0 && MEM32(vt + 16) != 0 && MEM32(vt + 20) == 0);
            vp = call("ID3DXBuffer::GetBufferPointer", 1, a1);
            vs = call("ID3DXBuffer::GetBufferSize", 1, a1);
            ok("buffer size is 5 DWORDs", vs == 5u * 4u);
            ok("buffer starts with version token", MEM32(vp) == V30);
            ok("buffer ends with END", MEM32(vp + 16) == END);
            /* Feed the guest buffer straight into the real translator. */
            {
                char msl[16384], err[256];
                int rc = d3d9_translate_shader_to_msl((const uint32_t *)hle_ptr(vp),
                                                      vs / 4, 0, msl, sizeof(msl),
                                                      err, sizeof(err));
                ok("shim output accepted by metal translator", rc == 0);
            }
            /* An error case must produce an error buffer, not a shader. */
            {
                uint32_t a2[7];
                uint32_t *pS2 = (uint32_t *)hle_alloc(4);
                uint32_t *pE2 = (uint32_t *)hle_alloc(4);
                void *bad = hle_alloc(128);
                const char *txt = "vs_3_0\nfrobnicate r0\n";
                *pS2 = 0; *pE2 = 0;
                memcpy(bad, txt, strlen(txt) + 1);
                a2[0] = A(bad); a2[1] = (uint32_t)strlen(txt); a2[2] = 0;
                a2[3] = 0; a2[4] = 0; a2[5] = A(pS2); a2[6] = A(pE2);
                call("D3DXAssembleShader", 7, a2);
                ok("error: no shader buffer", *pS2 == 0);
                ok("error: message buffer present", *pE2 != 0);
                if (*pE2) {
                    uint32_t ep = call("ID3DXBuffer::GetBufferPointer", 1, (uint32_t[]){ *pE2 });
                    const char *msg = (const char *)hle_ptr(ep);
                    ok("error buffer names line 2", strstr(msg, "line 2") != NULL);
                }
            }
        }
    }

    /* 8. A multi-opcode vertex shader: defi/defb, 3-source ALU, control flow,
     *    loop/endloop and ret, all with hand-computed public tokens. */
    {
        uint32_t want[] = {
            V30,
            INS(0x30, 5), DST(7, 0, 0xF), 3u, 0u, 1u, 0u,   /* defi i0 */
            INS(0x2F, 2), DST(14, 0, 0xF), 1u,                /* defb b0 */
            INS(0x04, 4), DST(0, 0, 0xF), SRC(1, 0), SRC(2, 0), SRC(2, 1), /* mad */
            INS(0x09, 3), DST(0, 1, 0xF), SRC(0, 0), SRC(2, 2),           /* dp4 */
            INS(0x1B, 2), SRC(15, 0), SRC(7, 0),               /* loop aL, i0 */
            INS(0x1D, 0),                                     /* endloop */
            INS(0x01, 2), DST(4, 0, 0xF), SRC(0, 0),           /* mov oPos, r0 */
            INS(0x1C, 0),                                     /* ret */
            END
        };
        const char *src =
            "vs_3_0\n"
            "defi i0, 3, 0, 1, 0\n"
            "defb b0, 1\n"
            "mad r0, v0, c0, c1\n"
            "dp4 r1, r0, c2\n"
            "loop aL, i0\n"
            "endloop\n"
            "mov oPos, r0\n"
            "ret\n";
        uint32_t *w = NULL, n = 0;
        char err[256];
        int rc = d3dx9_assemble(src, 0, &w, &n, err, sizeof(err));
        int good = rc == 0 && n == sizeof(want)/sizeof(want[0]);
        if (good) for (uint32_t i = 0; i < n; i++) {
            if (w[i] != want[i]) {
                printf("  multi-op token[%u] = 0x%08X, want 0x%08X\n",
                       i, w[i], want[i]);
                good = 0;
            }
        }
        if (rc != 0) printf("  multi-op error: %s\n", err);
        ok("multi-opcode vs_3_0 tokens", good);
        if (rc == 0) msl_ok(w, n, 0, "vs_main", "multi-opcode shader translates");
        free(w);
    }

    /* 9. Pixel shader texture path: dcl_2d, dcl_texcoord, tex, oC0. */
    {
        uint32_t want[] = {
            P30,
            INS(0x1F, 2), 2u << 27u, REG(10, 0),   /* dcl_2d s0 */
            INS(0x1F, 2), 5u, REG(3, 0),            /* dcl_texcoord t0 */
            INS(0x42, 3), DST(0, 0, 0xF), SRC(3, 0), SRC(10, 0), /* tex r0, t0, s0 */
            INS(0x01, 2), DST(8, 0, 0xF), SRC(0, 0),             /* mov oC0, r0 */
            END
        };
        direct_ok("ps_3_0\ndcl_2d s0\ndcl_texcoord t0\n"
                  "tex r0, t0, s0\nmov oC0, r0\n",
                  want, sizeof(want)/sizeof(want[0]), "ps_3_0 texture tokens");
        msl_ok(want, sizeof(want)/sizeof(want[0]), 1, "oC[0]",
               "ps_3_0 texture shader translates");
    }

    /* 10. Errors: wrong arity, stage-invalid registers and unsupported
     *     models must all be rejected with the offending line number. */
    {
        struct { const char *src; const char *line; const char *label; } cases[] = {
            { "vs_3_0\nmov r0\n",                 "line 2", "too few operands" },
            { "vs_3_0\nmov r0, r0, r0\n",          "line 2", "too many operands" },
            { "vs_3_0\nmov t0, r0\n",              "line 2", "t# rejected in VS" },
            { "ps_3_0\nmov a0, r0\n",              "line 2", "a0 rejected in PS" },
            { "vs_4_0\nmov r0, r0\n",              "line 1", "unsupported model" },
            { "vs_3_0\nfrobnicate r0, r0\n",       "line 2", "unknown opcode" },
            { "vs_3_0\nmov r0, -_bogus r1\n",      "line 2", "bad source modifier" },
            { "vs_3_0\nadd r0, v0.qqqq, r1\n",     "line 2", "bad swizzle" }
        };
        for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
            uint32_t *w = NULL, n = 0;
            char err[256];
            int rc = d3dx9_assemble(cases[i].src, 0, &w, &n, err, sizeof(err));
            ok(cases[i].label, rc != 0 && w == NULL && strstr(err, cases[i].line) != NULL);
            free(w);
        }
    }

    /* 11. Opcode-specific control bits: comparison control flow and the
     *     projected/biased texture-load flags from public d3d9types.h. */
    {
        uint32_t want[] = {
            V30,
            0x02030029u, SRC(0, 0), SRC(2, 0),        /* ifc_ge r0, c0 */
            INS(0x2A, 0),                              /* else */
            0x0205002Du, SRC(0, 0), SRC(2, 0),        /* breakc_ne r0, c0 */
            INS(0x2B, 0),                              /* endif */
            END
        };
        direct_ok("vs_3_0\nifc_ge r0, c0\nelse\nbreakc_ne r0, c0\nendif\n",
                  want, sizeof(want)/sizeof(want[0]), "comparison control tokens");
        msl_ok(want, sizeof(want)/sizeof(want[0]), 0, NULL,
               "comparison control translates");
    }
    {
        uint32_t want[] = {
            P30,
            INS(0x1F, 2), 2u << 27u, REG(10, 0),
            INS(0x1F, 2), 5u, REG(3, 0),
            0x0301005Fu, DST(0, 0, 0xF), SRC(3, 0), SRC(10, 0), /* texldp */
            0x0302005Fu, DST(0, 1, 0xF), SRC(3, 0), SRC(10, 0), /* texldb */
            INS(0x01, 2), DST(8, 0, 0xF), SRC(0, 0),
            END
        };
        direct_ok("ps_3_0\ndcl_2d s0\ndcl_texcoord t0\n"
                  "texldp r0, t0, s0\ntexldb r1, t0, s0\nmov oC0, r0\n",
                  want, sizeof(want)/sizeof(want[0]), "texld flags tokens");
        msl_ok(want, sizeof(want)/sizeof(want[0]), 1, NULL,
               "texld flags translate");
    }

    printf(fails ? "\n%d FAILED\n" : "\nALL OK\n", fails);
    return fails != 0;
}
