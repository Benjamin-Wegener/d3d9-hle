#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "win32hle.h"
#include "host.h"
#include "d3d9_hle.h"
#include "d3dx9_hle.h"
#include "d3dx9_buffer.h"
#include "d3dx9_float16.h"

/* guest pointer to IID_IUnknown {00000000-0000-0000-C000-000000000046} */
static uint32_t iid_unk(void) {
    uint8_t *g = (uint8_t *)hle_alloc(16);
    memset(g, 0, 16); g[8] = 0xC0; g[15] = 0x46;
    return hle_va(g);
}

const recomp_dispatch_entry_t recomp_dispatch_table[1];
const uint32_t recomp_dispatch_count = 0;

static int fails;
static void ok(const char *what, int cond) { if (!cond) { printf("FAIL %s\n", what); fails++; } }
static uint32_t call(const char *name, int n, const uint32_t *a) { return hle_call_guest(hle_resolve(name), n, a); }

/* Enter a guest vtable slot DIRECTLY. hle_call_guest() saves/restores esp, so
 * an esp delta measured around it would always be zero and prove nothing. */
static uint32_t vslot(uint32_t obj, int slot, int argc, const uint32_t *extra,
                      int *called, uint32_t *delta) {
    uint32_t fva = MEM32(MEM32(obj) + 4u * (uint32_t)slot);
    *called = 0; *delta = 0;
    if (!fva) return 0u;
    uint32_t a[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    a[0] = obj;                                   /* `this` is arg 0 */
    for (int k = 1; k < argc; k++) a[k] = extra[k - 1];
    mach_enter();
    uint32_t esp0 = g_esp;
    for (int k = argc - 1; k >= 0; k--) PUSH32(g_esp, a[k]);
    PUSH32(g_esp, RECOMP_RETADDR);
    uint32_t eax = 0;
    recomp_func_t f = recomp_lookup_import(fva);
    if (f) { *called = 1; f(); eax = g_eax; }
    *delta = g_esp - esp0;
    g_esp = esp0;                                  /* undo the simulated call */
    mach_leave();
    return eax;
}

int main(void) {
    recomp_host_init();                            /* registers all shim modules */
    win32hle_register(win32hle_d3dx9);

    /* ---------------- D3DXCreateBuffer ---------------- */
    ok("D3DXCreateBuffer(NULL out) -> D3DERR_INVALIDCALL",
       call("D3DXCreateBuffer", 2, (uint32_t[]){ 32u, 0u }) == 0x8876086Cu);

    uint32_t *slot = (uint32_t *)hle_alloc(4);
    MEM32(hle_va(slot)) = 0xDEADBEEFu;
    ok("D3DXCreateBuffer(32) -> S_OK",
       call("D3DXCreateBuffer", 2, (uint32_t[]){ 32u, hle_va(slot) }) == 0u);
    uint32_t buf = MEM32(hle_va(slot));
    ok("D3DXCreateBuffer(32) hands back an object", buf != 0u);
    uint32_t vt = buf ? MEM32(buf) : 0u;
    ok("object has a vtable", vt != 0u);

    int called; uint32_t delta;
    ok("GetBufferSize(vt+16) == 32",
       vslot(buf, 4, 1, NULL, &called, &delta) == 32u && called && delta == 0u);
    uint32_t data = vslot(buf, 3, 1, NULL, &called, &delta);
    ok("GetBufferPointer(vt+12) non-null, balanced",
       data != 0u && called && delta == 0u);

    if (data) {
        const volatile uint8_t *p = (const volatile uint8_t *)hle_ptr(data);
        int zeroed = 1;
        for (uint32_t i = 0; i < 32u; ++i) if (p[i]) zeroed = 0;
        ok("payload is zero-filled", zeroed);
        MEM32(data) = 0x12345678u;
        ok("payload is writable", MEM32(data) == 0x12345678u);
    }

    /* size == 0: a valid object, zero size, null data pointer. */
    MEM32(hle_va(slot)) = 0xDEADBEEFu;
    ok("D3DXCreateBuffer(0) -> S_OK",
       call("D3DXCreateBuffer", 2, (uint32_t[]){ 0u, hle_va(slot) }) == 0u);
    uint32_t buf0 = MEM32(hle_va(slot));
    ok("D3DXCreateBuffer(0) hands back an object", buf0 != 0u);
    ok("GetBufferSize(empty) == 0",
       vslot(buf0, 4, 1, NULL, &called, &delta) == 0u && called && delta == 0u);
    ok("GetBufferPointer(empty) == NULL",
       vslot(buf0, 3, 1, NULL, &called, &delta) == 0u && called && delta == 0u);

    /* IUnknown refcounting through the guest vtable. */
    uint32_t *outp = (uint32_t *)hle_alloc(4);
    uint32_t qiargs[2] = { iid_unk(), hle_va(outp) };  /* riid, &out */
    ok("QueryInterface -> S_OK and out == this",
       vslot(buf, 0, 3, qiargs, &called, &delta) == 0u &&
       called && delta == 0u && MEM32(hle_va(outp)) == buf);
    ok("AddRef -> 3", vslot(buf, 1, 1, NULL, &called, &delta) == 3u && called && delta == 0u);
    ok("Release -> 2", vslot(buf, 2, 1, NULL, &called, &delta) == 2u && called && delta == 0u);

    /* ---------------- COM checklist: EVERY slot, esp, slot count ----------------
     * The five methods and their order come from the public d3dx9core.h
     * ID3DXBuffer declaration. argc includes `this`, as the guest's
     * `call [eax+4*i]` pushes it. */
    {
        static const char *const names[5] = {
            "QueryInterface", "AddRef", "Release", "GetBufferPointer", "GetBufferSize"
        };
        static const int argc[5] = { 3, 1, 1, 1, 1 };
        int nonzero = 0;
        for (int i = 0; i < 5; ++i) if (MEM32(vt + 4u * (uint32_t)i)) nonzero++;
        ok("vtable has all 5 ID3DXBuffer slots", nonzero == 5);
        ok("vtable ends after 5 slots", MEM32(vt + 4u * 5u) == 0u);

        for (int i = 0; i < 5; ++i) {
            uint32_t extra[2] = { 0, 0 };
            if (i == 0) extra[1] = hle_va(outp);       /* QueryInterface's &out */
            int c = 0; uint32_t d = 0;
            vslot(buf, i, argc[i], extra, &c, &d);
            char desc[128];
            snprintf(desc, sizeof desc, "slot %d %s: called + esp restored (argc %d)",
                     i, names[i], argc[i]);
            ok(desc, c && d == 0u);
        }
    }

    /* ---------------- D3DXFloat32To16Array ----------------
     * Independently known IEEE half patterns: 0, signed zero, 1, -1, 0.5,
     * 2, 1.5, max finite (65504), overflow, Inf/NaN collapse, smallest
     * normal, smallest subnormal, underflow, round-half-to-even both ways. */
    {
        static const uint32_t in[21] = {
            0x00000000u, 0x80000000u, 0x3f800000u, 0xbf800000u, 0x3f000000u,
            0x40000000u, 0x3fc00000u, 0x477fe000u, 0x47800000u, 0x48000000u,
            0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00000u, 0x38800000u,
            0x33800000u, 0x33000000u, 0x33c00000u, 0x3f801000u, 0x3f803000u,
            0x3f80b800u  /* 1 + 23/4096: fraction 0.75 -> round up */
        };
        static const uint16_t want[21] = {
            0x0000u, 0x8000u, 0x3C00u, 0xBC00u, 0x3800u,
            0x4000u, 0x3E00u, 0x7BFFu, 0x7C00u, 0x7FFFu,
            0x7FFFu, 0xFFFFu, 0x7FFFu, 0xFFFFu, 0x0400u,
            0x0001u, 0x0000u, 0x0002u, 0x3C00u, 0x3C02u,
            0x3C06u
        };
        const uint32_t n = 21u;
        uint32_t *pin = (uint32_t *)hle_alloc(4u * n);
        uint16_t *pout = (uint16_t *)hle_alloc(2u * n + 4u);
        for (uint32_t i = 0; i < n; ++i) MEM32(hle_va(pin) + 4u * i) = in[i];
        for (uint32_t i = 0; i < n + 2u; ++i) MEM16(hle_va(pout) + 2u * i) = 0xA5A5u;

        ok("F32To16 returns pout",
           call("D3DXFloat32To16Array", 3,
                (uint32_t[]){ hle_va(pout), hle_va(pin), n }) == hle_va(pout));
        for (uint32_t i = 0; i < n; ++i) {
            char desc[128];
            snprintf(desc, sizeof desc, "F32To16[%u] in=0x%08X (got 0x%04X want 0x%04X)",
                     i, in[i], MEM16(hle_va(pout) + 2u * i), want[i]);
            ok(desc, MEM16(hle_va(pout) + 2u * i) == want[i]);
        }
        ok("F32To16 wrote exactly n halfs",
           MEM16(hle_va(pout) + 2u * n) == 0xA5A5u);
        ok("F32To16 n=0 returns pout",
           call("D3DXFloat32To16Array", 3,
                (uint32_t[]){ hle_va(pout), hle_va(pin), 0u }) == hle_va(pout));
    }

    /* ---------------- D3DXFloat16To32Array ----------------
     * Includes the documented D3DX quirk: half exponent 31 (Inf/NaN field)
     * is converted as an ordinary exponent, so 0x7fff -> 131008.0f. */
    {
        static const uint16_t in[14] = {
            0x0000u, 0x8000u, 0x3C00u, 0xBC00u, 0x4000u, 0x3800u, 0x3E00u,
            0x7BFFu, 0x0400u, 0x0001u, 0x7C00u, 0x7FFFu, 0xFC00u, 0xFFFFu
        };
        static const uint32_t want[14] = {
            0x00000000u, 0x80000000u, 0x3f800000u, 0xbf800000u, 0x40000000u,
            0x3f000000u, 0x3fc00000u, 0x477fe000u, 0x38800000u, 0x33800000u,
            0x47800000u, 0x47ffe000u, 0xc7800000u, 0xc7ffe000u
        };
        const uint32_t n = 14u;
        uint16_t *pin = (uint16_t *)hle_alloc(2u * n);
        uint32_t *pout = (uint32_t *)hle_alloc(4u * n + 4u);
        for (uint32_t i = 0; i < n; ++i) MEM16(hle_va(pin) + 2u * i) = in[i];
        for (uint32_t i = 0; i < n + 1u; ++i) MEM32(hle_va(pout) + 4u * i) = 0xDEADBEEFu;

        ok("F16To32 returns pout",
           call("D3DXFloat16To32Array", 3,
                (uint32_t[]){ hle_va(pout), hle_va(pin), n }) == hle_va(pout));
        for (uint32_t i = 0; i < n; ++i) {
            char desc[96];
            snprintf(desc, sizeof desc, "F16To32[%u] in=0x%04X (want 0x%08X)",
                     i, in[i], want[i]);
            ok(desc, MEM32(hle_va(pout) + 4u * i) == want[i]);
        }
        ok("F16To32 wrote exactly n floats",
           MEM32(hle_va(pout) + 4u * n) == 0xDEADBEEFu);
        ok("F16To32 n=0 returns pout",
           call("D3DXFloat16To32Array", 3,
                (uint32_t[]){ hle_va(pout), hle_va(pin), 0u }) == hle_va(pout));
    }

    if (!fails) printf("d3dx9_buffer_selftest: all checks passed\n");
    return fails != 0;
}
