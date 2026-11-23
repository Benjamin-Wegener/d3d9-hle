/*
 * d3d9_query_selftest.c - COM contract of IDirect3DQuery9 (public d3d9.h).
 *
 * Every slot is entered directly through the guest vtable so a wrong stdcall
 * purge (esp imbalance) or a double RET cannot hide behind hle_call_guest().
 * The slot count and order are checked against the public Wine header.
 *
 * Public sources:
 *   https://raw.githubusercontent.com/wine-mirror/wine/master/include/d3d9.h
 *       IDirect3DQuery9 : IUnknown -- QueryInterface, AddRef, Release,
 *       GetDevice, GetType, GetDataSize, Issue, GetData (8 slots)
 *   https://raw.githubusercontent.com/wine-mirror/wine/master/include/d3d9types.h
 *       D3DQUERYTYPE_*, D3DISSUE_BEGIN/END, D3DGETDATA_FLUSH
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "host.h"
#include "d3d9_hle.h"
#include "d3d9_internal.h"

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

/* --- minimal mock backend ------------------------------------------------ */
static int mock_active(void) { return 1; }
static int mock_enable(void) { return 0; }
static int mock_init(uint32_t w, uint32_t h) { (void)w; (void)h; return 0; }
static void mock_shutdown(void) {}
static int mock_begin(void) { return 0; }
static int mock_end(void) { return 0; }
static int mock_clear(uint32_t f, uint32_t c, float z, uint32_t s) {
    (void)f; (void)c; (void)z; (void)s; return 0;
}
static int mock_present(void) { return 0; }
static void *mock_tex(uint32_t w, uint32_t h, uint32_t f, int c, uint32_t levels) {
    (void)w; (void)h; (void)f; (void)c; (void)levels; return (void *)1;
}
static void mock_tex_destroy(void *p) { (void)p; }
static void mock_tex_upload(void *t, int f, int l, const void *b, uint32_t r) {
    (void)t; (void)f; (void)l; (void)b; (void)r;
}
static void *mock_buf(uint32_t s, int i) { (void)s; (void)i; return (void *)2; }
static void mock_buf_destroy(void *p) { (void)p; }
static void mock_buf_upload(void *b, uint32_t o, const void *d, uint32_t s) {
    (void)b; (void)o; (void)d; (void)s;
}
static void *mock_pipe(const char *v, const char *p) { (void)v; (void)p; return (void *)3; }
static void mock_pipe_destroy(void *p) { (void)p; }
static int mock_draw(int t, uint32_t s, uint32_t c, void *p, void *v, uint32_t st) {
    (void)t; (void)s; (void)c; (void)p; (void)v; (void)st; return 0;
}
static void mock_set_rt(void *rt) { (void)rt; }
static void *mock_create_rt(uint32_t w, uint32_t h) { (void)w; (void)h; return (void *)4; }
static void mock_destroy_rt(void *rt) { (void)rt; }
static void mock_set_tex(void *tex) { (void)tex; }

static int g_q_issues, g_q_reads, g_q_destroyed;
static void *mock_query_create(uint32_t type) {
    return (void *)(uintptr_t)(0x2000u + type);
}
static int mock_query_issue(void *q, uint32_t flags) {
    g_q_issues++;
    return (q && (flags == D3DISSUE_BEGIN || flags == D3DISSUE_END)) ? 0 : -1;
}
static int mock_query_get_data(void *q, void *out, uint32_t size, uint32_t flags) {
    g_q_reads++;
    if (!q || !out || size < 4) return -1;
    *(uint32_t *)out = (flags & D3DGETDATA_FLUSH) ? 0x13572468u : 0u;
    return (flags & D3DGETDATA_FLUSH) ? 0 : 1;
}
static void mock_query_destroy(void *q) { (void)q; g_q_destroyed++; }

static const d3d9_gpu_ops_t g_mock_gpu = {
    "query-mock", mock_active, mock_enable, mock_init, mock_shutdown,
    mock_begin, mock_end, mock_clear, mock_present,
    mock_tex, mock_tex_destroy, mock_tex_upload,
    mock_buf, mock_buf_destroy, mock_buf_upload,
    mock_pipe, mock_pipe_destroy, mock_draw,
    mock_set_rt, mock_create_rt, mock_destroy_rt, mock_set_tex,
    .query_create = mock_query_create,
    .query_issue = mock_query_issue,
    .query_get_data = mock_query_get_data,
    .query_destroy = mock_query_destroy
};

/* --- raw guest-vtable entry with esp accounting -------------------------
 * hle_call_guest restores esp, so it would hide a wrong stdcall purge. */
static uint32_t raw_slot(uint32_t fva, int argc, const uint32_t *a,
                         uint32_t *delta, uint32_t *eax) {
    mach_enter();
    uint32_t esp0 = g_esp;
    for (int k = argc - 1; k >= 0; k--) PUSH32(g_esp, a[k]);
    PUSH32(g_esp, RECOMP_RETADDR);
    recomp_func_t f = recomp_lookup_import(fva);
    uint32_t called = 0;
    if (f) { called = 1; f(); }
    *delta = g_esp - esp0;
    if (eax) *eax = g_eax;
    g_esp = esp0;
    mach_leave();
    return called;
}
static void check_slot(const char *what, uint32_t obj, int slot, int argc,
                       const uint32_t *a, uint32_t want_eax) {
    uint32_t delta = 0xFFFFu, eax = 0xFFFFFFFFu;
    uint32_t called = raw_slot(MEM32(MEM32(obj) + 4u * (uint32_t)slot), argc, a, &delta, &eax);
    char msg[160];
    snprintf(msg, sizeof msg, "%s: entered via vtable, esp balanced (delta %u)", what, (unsigned)delta);
    ok(msg, called && delta == 0u);
    snprintf(msg, sizeof msg, "%s: result %u", what, (unsigned)want_eax);
    ok(msg, eax == want_eax);
}

const recomp_dispatch_entry_t recomp_dispatch_table[1];
const uint32_t recomp_dispatch_count = 0;

int main(void) {
    recomp_host_init();
    win32hle_register(win32hle_d3d9);

    const d3d9_gpu_ops_t *prev = hle_d3d9_gpu;
    hle_d3d9_set_gpu(&g_mock_gpu);

    uint32_t d3d9 = call("Direct3DCreate9", 1, (uint32_t[]){ 32 });
    ok("Direct3DCreate9", d3d9 != 0);
    D3DPRESENT_PARAMETERS *pp = (D3DPRESENT_PARAMETERS *)hle_alloc(sizeof(*pp));
    memset(pp, 0, sizeof(*pp));
    pp->BackBufferWidth = 32; pp->BackBufferHeight = 32;
    pp->BackBufferFormat = D3DFMT_A8R8G8B8; pp->BackBufferCount = 1;
    uint32_t *pDev = (uint32_t *)hle_alloc(4);
    ok("CreateDevice", call("IDirect3D9::CreateDevice", 7,
        (uint32_t[]){ d3d9, 0, D3DDEVTYPE_HAL, 0, D3DCREATE_HARDWARE_VERTEXPROCESSING,
                      A(pp), A(pDev) }) == D3D_OK && *pDev != 0);
    uint32_t dev = *pDev;

    /* ---- CreateQuery validation ----------------------------------------
     * Only the public EVENT (8) and OCCLUSION (9) types are available. */
    {
        static const uint32_t bad[] = { 0u, 1u, 2u, 3u, D3DQUERYTYPE_VCACHE,
            D3DQUERYTYPE_TIMESTAMP, D3DQUERYTYPE_TIMESTAMPDISJOINT,
            D3DQUERYTYPE_MEMORYPRESSURE, 20u, 0xFFFFFFFFu };
        for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            uint32_t *out = (uint32_t *)hle_alloc(4); *out = 0xDEADBEEFu;
            uint32_t hr = call("IDirect3DDevice9::CreateQuery", 3,
                               (uint32_t[]){ dev, bad[i], A(out) });
            char msg[128];
            snprintf(msg, sizeof msg, "CreateQuery type %u is D3DERR_NOTAVAILABLE", bad[i]);
            ok(msg, hr == D3DERR_NOTAVAILABLE);
            snprintf(msg, sizeof msg, "CreateQuery type %u leaves output untouched", bad[i]);
            ok(msg, *out == 0xDEADBEEFu);
        }
        ok("CreateQuery(NULL out) is D3DERR_INVALIDCALL",
           call("IDirect3DDevice9::CreateQuery", 3,
                (uint32_t[]){ dev, D3DQUERYTYPE_EVENT, 0 }) == D3DERR_INVALIDCALL);
    }

    uint32_t *pQuery = (uint32_t *)hle_alloc(4); *pQuery = 0;
    ok("CreateQuery(EVENT) succeeds", call("IDirect3DDevice9::CreateQuery", 3,
        (uint32_t[]){ dev, D3DQUERYTYPE_EVENT, A(pQuery) }) == D3D_OK && *pQuery != 0);
    uint32_t q = *pQuery;

    /* ---- vtable layout vs public source -------------------------------- */
    static const char *slot_name[8] = {
        "IDirect3DQuery9::QueryInterface", "IDirect3DQuery9::AddRef",
        "IDirect3DQuery9::Release", "IDirect3DQuery9::GetDevice",
        "IDirect3DQuery9::GetType", "IDirect3DQuery9::GetDataSize",
        "IDirect3DQuery9::Issue", "IDirect3DQuery9::GetData" };
    /* stdcall argc including `this`; GetData has 4 args total. */
    static const int nargs[8] = { 3, 1, 1, 2, 1, 1, 2, 4 };
    uint32_t vt = MEM32(q);
    for (int i = 0; i < 8; i++) {
        char msg[128];
        snprintf(msg, sizeof msg, "Query9 slot %d is %s", i, slot_name[i]);
        ok(msg, MEM32(vt + 4u * (uint32_t)i) == hle_resolve(slot_name[i]));
    }
    ok("Query9 vtable has exactly 8 slots (terminator after GetData)",
       MEM32(vt + 4u * 8u) == 0u);

    /* ---- every slot, raw vtable, esp checked --------------------------- */
    uint32_t *pOut = (uint32_t *)hle_alloc(4); *pOut = 0;
    uint32_t *qdata = (uint32_t *)hle_alloc(4); *qdata = 0;
    uint32_t args[8][4] = {
        { q, iid_unk(), A(pOut), 0 },       /* QueryInterface */
        { q, 0, 0, 0 },             /* AddRef */
        { q, 0, 0, 0 },             /* Release */
        { q, A(pOut), 0, 0 },       /* GetDevice */
        { q, 0, 0, 0 },             /* GetType */
        { q, 0, 0, 0 },             /* GetDataSize */
        { q, D3DISSUE_END, 0, 0 },  /* Issue */
        { q, A(qdata), 4, D3DGETDATA_FLUSH }  /* GetData */
    };
    /* refcount starts at 2 before the loop: QI -> 3, AddRef -> 4,
     * Release -> 3 (no free), so the remaining slots still have an object. */
    MEM32(q + 4) = 2u;
    uint32_t dev_ref0 = MEM32(dev + 4);
    uint32_t want[8] = { D3D_OK, 4u, 3u, D3D_OK, D3DQUERYTYPE_EVENT,
                         4u, D3D_OK, 0u };
    for (int i = 0; i < 8; i++) check_slot(slot_name[i], q, i, nargs[i], args[i], want[i]);

    ok("GetDevice returns the device and AddRefs it",
       *pOut == dev && MEM32(dev + 4) == dev_ref0 + 1u);
    ok("GetData wrote the backend value, not a constant 1", *qdata == 0x13572468u);

    uint32_t after_qi = 0, after_addref = 0, after_release = 0;
    {
        uint32_t delta, eax;
        uint32_t *pOut2 = (uint32_t *)hle_alloc(4); *pOut2 = 0;
        uint32_t a_qi[3] = { q, iid_unk(), A(pOut2) };
        MEM32(q + 4) = 2u;
        raw_slot(MEM32(vt), 3, a_qi, &delta, &eax); after_qi = MEM32(q + 4);
        raw_slot(MEM32(vt + 4u), 1, args[1], &delta, &eax); after_addref = MEM32(q + 4);
        raw_slot(MEM32(vt + 8u), 1, args[2], &delta, &eax); after_release = MEM32(q + 4);
        ok("QueryInterface out = self", *pOut2 == q);
    }
    ok("QueryInterface increments refcount", after_qi == 3u);
    ok("AddRef increments refcount", after_addref == 4u);
    ok("Release decrements refcount", after_release == 3u);

    /* release the query exactly once and confirm the backend handle is freed */
    MEM32(q + 4) = 1u;
    int destroyed0 = g_q_destroyed;
    ok("final Query::Release returns 0", call("IDirect3DQuery9::Release", 1, (uint32_t[]){ q }) == 0);
    ok("backend query handle destroyed exactly once", g_q_destroyed == destroyed0 + 1);

    /* OCCLUSION has the same COM shape and data size. */
    ok("CreateQuery(OCCLUSION) succeeds", call("IDirect3DDevice9::CreateQuery", 3,
        (uint32_t[]){ dev, D3DQUERYTYPE_OCCLUSION, A(pQuery) }) == D3D_OK && *pQuery != 0);
    ok("Occlusion GetType", call("IDirect3DQuery9::GetType", 1,
        (uint32_t[]){ *pQuery }) == D3DQUERYTYPE_OCCLUSION);
    ok("Occlusion GetDataSize", call("IDirect3DQuery9::GetDataSize", 1,
        (uint32_t[]){ *pQuery }) == 4);
    ok("Occlusion Issue(BEGIN) is D3D_OK", call("IDirect3DQuery9::Issue", 2,
        (uint32_t[]){ *pQuery, D3DISSUE_BEGIN }) == D3D_OK);
    ok("Occlusion Issue(END) is D3D_OK", call("IDirect3DQuery9::Issue", 2,
        (uint32_t[]){ *pQuery, D3DISSUE_END }) == D3D_OK);
    ok("Occlusion GetData with FLUSH is S_OK", call("IDirect3DQuery9::GetData", 4,
        (uint32_t[]){ *pQuery, A(qdata), 4, D3DGETDATA_FLUSH }) == 0);
    ok("Occlusion GetData relays backend count", *qdata == 0x13572468u);
    call("IDirect3DQuery9::Release", 1, (uint32_t[]){ *pQuery });

    hle_d3d9_set_gpu(prev);
    printf(fails == 0 ? "d3d9_query_selftest: OK\n" : "d3d9_query_selftest: FAILED\n");
    return fails != 0;
}
