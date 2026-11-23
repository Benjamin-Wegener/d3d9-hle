/*
 * ole32.c - OLE32 and OLEAUT32: COM's bookkeeping, and a way for a host to
 * serve COM objects of its own.
 *
 * A COM object a guest holds is guest memory whose first dword points at a
 * vtable of method addresses; on win32hle those are shim VAs, so a guest's
 * `call [eax+0Ch]` dispatches to C (hle_com_vtable builds one from a shim
 * table). CoCreateInstance asks the classes a host registered
 * (hle_com_register_class), and otherwise answers REGDB_E_CLASSNOTREG, as for
 * a COM server that is not installed.
 *
 * Structured storage, OLE embedding and type libraries are not here: they
 * fail with E_NOTIMPL.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "win32hle.h"

#define S_OK                 0u
#define E_NOTIMPL            0x80004001u
#define E_NOINTERFACE        0x80004002u
#define E_FAIL               0x80004005u
#define REGDB_E_CLASSNOTREG  0x80040154u
#define CO_E_CLASSSTRING     0x800401F3u

/* ---- GUIDs ---- */
void hle_guid_string(const uint8_t *g, char *out) {      /* {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx} */
    sprintf(out, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", *(const uint32_t *)g,
            *(const uint16_t *)(g + 4), *(const uint16_t *)(g + 6), g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}
static int parse_guid(const char *s, uint8_t *g) {
    unsigned a, b, c, d[8];
    if (sscanf(s, "{%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}", &a, &b, &c, &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]) != 11)
        return 0;
    *(uint32_t *)g = a, *(uint16_t *)(g + 4) = (uint16_t)b, *(uint16_t *)(g + 6) = (uint16_t)c;
    for (int i = 0; i < 8; i++) g[8 + i] = (uint8_t)d[i];
    return 1;
}

/* ---- host-served classes ---- */
#define MAX_CLASSES 16
static struct { uint8_t clsid[16]; uint32_t (*create)(const uint8_t *iid, uint32_t *out); } g_class[MAX_CLASSES];
static int g_nclass;
void hle_com_register_class(const uint8_t *clsid, uint32_t (*create)(const uint8_t *iid, uint32_t *out)) {
    if (g_nclass < MAX_CLASSES) memcpy(g_class[g_nclass].clsid, clsid, 16), g_class[g_nclass++].create = create;
}

/* A vtable of shim VAs, in guest-readable memory (it lives for the run). */
uint32_t hle_com_vtable(const win32hle_shim *methods) {
    int n = 0;
    while (methods[n].name) n++;
    uint32_t *vt = (uint32_t *)hle_alloc(4u * (uint32_t)n + 4u);
    win32hle_register(methods);
    for (int i = 0; i < n; i++) vt[i] = hle_resolve(methods[i].name);
    return hle_va(vt);
}
/* An object: the vtable pointer, a reference count, then `size` - 8 bytes of
 * the host's own state. */
uint32_t hle_com_new(uint32_t vtable, uint32_t size) {
    uint32_t *o = (uint32_t *)hle_alloc(size < 8 ? 8 : size);
    o[0] = vtable, o[1] = 1;
    return hle_va(o);
}
void hle_com_free(uint32_t obj) { hle_free(hle_ptr(obj)); }
/* IUnknown for such objects: any interface is this one (a host serves one
 * interface per object), counted at +4, freed at zero. */
void hle_com_QueryInterface(void) {                      /* (this, iid, &out) */
    uint32_t self = A32(0);
    if (A32(2)) MEM32(A32(2)) = self;
    MEM32(self + 4)++;
    RET(S_OK, 3);
}
void hle_com_AddRef(void) { RET(++MEM32(A32(0) + 4), 1); }
void hle_com_Release(void) {
    uint32_t self = A32(0), n = --MEM32(self + 4);
    if (!n) hle_com_free(self);
    RET(n, 1);
}

/* Class objects the guest registered itself (CoRegisterClassObject): a game
 * that serves its own classes in process (Tiberian Sun's locomotors) creates
 * them through COM, so CoCreateInstance has to find its factories. */
#define MAX_GUEST_CLASSES 512                    /* Red Alert 2: past 64, its saved classes and locomotors */
static struct { uint8_t clsid[16]; uint32_t unk, cookie; } g_guest_class[MAX_GUEST_CLASSES];
static uint32_t g_next_cookie = 0x100;
static const uint8_t IID_IClassFactory[16] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                               0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 };
static uint32_t vcall(uint32_t obj, int slot, int nargs, const uint32_t *args) {
    uint32_t a[8] = { obj };
    for (int i = 0; i < nargs; i++) a[1 + i] = args[i];
    return hle_call_guest(MEM32(MEM32(obj) + 4u * (uint32_t)slot), nargs + 1, a);
}

static void o_CoCreateInstance(void) {                   /* (clsid, outer, ctx, iid, &out) */
    const uint8_t *clsid = (const uint8_t *)APTR(0);
    char s[40];
    if (A32(4)) MEM32(A32(4)) = 0;
    for (int i = 0; clsid && i < g_nclass; i++)
        if (!memcmp(g_class[i].clsid, clsid, 16)) RET(g_class[i].create((const uint8_t *)APTR(3), (uint32_t *)APTR(4)), 5);
    for (int i = 0; clsid && i < MAX_GUEST_CLASSES; i++)
        if (g_guest_class[i].unk && !memcmp(g_guest_class[i].clsid, clsid, 16)) {
            uint32_t mark = hle_scratch_mark(), cf = hle_scratch(4);
            uint32_t qi[2] = { hle_scratch_copy(IID_IClassFactory, 16), cf };
            uint32_t hr = vcall(g_guest_class[i].unk, 0, 2, qi);           /* QueryInterface */
            uint32_t f = MEM32(cf);
            hle_scratch_release(mark);
            if (hr) RET(hr, 5);
            uint32_t ci[3] = { A32(1), A32(3), A32(4) };
            hr = vcall(f, 3, 3, ci);                                        /* IClassFactory::CreateInstance */
            vcall(f, 2, 0, NULL);                                           /* Release */
            RET(hr, 5);
        }
    if (clsid) hle_guid_string(clsid, s);
    fprintf(stderr, "[ole32] CoCreateInstance(%s): no such class on this host\n", clsid ? s : "(null)");
    RET(REGDB_E_CLASSNOTREG, 5);
}
static void o_OleInitialize(void)   { RET(S_OK, 1); }
static void o_OleUninitialize(void) { RETV(0); }
static void o_CoInitialize(void)    { RET(S_OK, 1); }
static void o_CoUninitialize(void)  { RETV(0); }
static void o_CoRegisterClassObject(void) {              /* (clsid, unk, ctx, flags, &cookie) */
    for (int i = 0; i < MAX_GUEST_CLASSES; i++)
        if (!g_guest_class[i].unk) {
            memcpy(g_guest_class[i].clsid, APTR(0), 16);
            g_guest_class[i].unk = A32(1), g_guest_class[i].cookie = g_next_cookie++;
            vcall(A32(1), 1, 0, NULL);                   /* AddRef: COM holds it */
            if (A32(4)) MEM32(A32(4)) = g_guest_class[i].cookie;
            RET(S_OK, 5);
        }
    fprintf(stderr, "[ole32] CoRegisterClassObject: more than %d classes\n", MAX_GUEST_CLASSES);
    RET(0x8007000Eu, 5);                                 /* E_OUTOFMEMORY */
}
static void o_CoRevokeClassObject(void) {
    for (int i = 0; i < MAX_GUEST_CLASSES; i++)
        if (g_guest_class[i].unk && g_guest_class[i].cookie == A32(0)) {
            uint32_t u = g_guest_class[i].unk;
            g_guest_class[i].unk = 0;
            vcall(u, 2, 0, NULL);                        /* Release */
            RET(S_OK, 1);
        }
    RET(0x80040011u /* CO_E_OBJNOTREG */, 1);
}
static void o_CoDisconnectObject(void)    { RET(S_OK, 2); }
static void o_CoFileTimeNow(void) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ft = ((uint64_t)ts.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)ts.tv_nsec / 100u;
    if (A32(0)) MEM32(A32(0)) = (uint32_t)ft, MEM32(A32(0) + 4) = (uint32_t)(ft >> 32);
    RET(S_OK, 1);
}
static void o_CLSIDFromString(void) {                    /* (wide string, &clsid) */
    const uint16_t *w = (const uint16_t *)APTR(0);
    char s[64];
    int k = 0;
    for (; w && w[k] && k < 63; k++) s[k] = (char)w[k];
    s[k] = 0;
    RET(parse_guid(s, (uint8_t *)APTR(1)) ? S_OK : CO_E_CLASSSTRING, 2);
}
static void o_StringFromGUID2(void) {                    /* (guid, wide buf, n) -> chars incl. NUL */
    char s[40];
    uint16_t *w = (uint16_t *)APTR(1);
    hle_guid_string((const uint8_t *)APTR(0), s);
    if ((int)A32(2) < 39) RET(0, 3);
    for (int i = 0; i < 39; i++) w[i] = (uint8_t)s[i];
    RET(39, 3);
}
static void o_StringFromCLSID(void) {                    /* (clsid, &wide string) */
    char s[40];
    uint16_t *w = (uint16_t *)hle_alloc(80);
    hle_guid_string((const uint8_t *)APTR(0), s);
    for (int i = 0; i < 39; i++) w[i] = (uint8_t)s[i];
    if (A32(1)) MEM32(A32(1)) = hle_va(w);
    RET(S_OK, 2);
}
/* OleRun: an object a host serves is in process, so always running. */
static void o_OleRun(void) { RET(A32(0) ? S_OK : E_FAIL, 1); }
static void o_notimpl_1(void) { RET(E_NOTIMPL, 1); }
static void o_notimpl_2(void) { RET(E_NOTIMPL, 2); }
static void o_notimpl_4(void) { RET(E_NOTIMPL, 4); }

/* ---- OLEAUT32: BSTRs (length-prefixed wide strings) and VARIANTs ---- */
static void a_SysAllocString(void) {
    const uint16_t *w = (const uint16_t *)APTR(0);
    uint32_t n = 0;
    if (!w) RET(0, 1);
    while (w[n]) n++;
    uint8_t *b = (uint8_t *)hle_alloc(4 + 2 * n + 2);
    *(uint32_t *)b = 2 * n;
    memcpy(b + 4, w, 2 * n);
    RETP(b + 4, 1);
}
static void a_SysFreeString(void) { if (A32(0)) hle_free((uint8_t *)APTR(0) - 4); RETV(1); }
static void a_VariantInit(void)   { if (A32(0)) memset(APTR(0), 0, 16); RETV(1); }
static void a_VariantClear(void)  { if (A32(0)) memset(APTR(0), 0, 16); RET(S_OK, 1); }
static void a_VariantChangeType(void) { RET(E_NOTIMPL, 4); }
static void a_GetErrorInfo(void)  { if (A32(1)) MEM32(A32(1)) = 0; RET(1u /* S_FALSE */, 2); }

/* CoCreateGuid(&guid): a random (version 4) GUID, from the kernel's randomness. */
static void o_CoCreateGuid(void) {
    uint8_t *g = (uint8_t *)APTR(0);
    FILE *f = fopen("/dev/urandom", "rb");
    if (!g) RET(0x80070057u, 1);                         /* E_INVALIDARG */
    if (!f || fread(g, 1, 16, f) != 16) for (int i = 0; i < 16; i++) g[i] = (uint8_t)rand();
    if (f) fclose(f);
    g[7] = (uint8_t)((g[7] & 0x0F) | 0x40), g[8] = (uint8_t)((g[8] & 0x3F) | 0x80);
    RET(S_OK, 1);
}


/* ---- CreateStreamOnHGlobal: a guest IStream over guest memory -------------
 * IStream's 14 methods (IUnknown's three first) are listed in the exact order
 * of the public interface; every slot is occupied, and unsupported methods
 * return E_NOTIMPL with their real stdcall stack size. No code or GUID comes
 * from a Microsoft binary -- only the public interface order/signatures. */
#define E_INVALIDARG  0x80070057u
#define E_NOTIMPL_OLE 0x80004001u
#define STGTY_STREAM  2u

typedef struct { uint32_t data, size, pos, del; } stream_t;
#define STREAM(o) ((stream_t *)((char *)hle_ptr(o) + 8u))
static uint32_t g_vt_stream;

static void s_QueryInterface(void)  { hle_com_QueryInterface(); }
static void s_AddRef(void)          { hle_com_AddRef(); }
static void s_Release(void) {
    uint32_t self = A32(0), n = --MEM32(self + 4);
    if (!n) {
        stream_t *st = STREAM(self);
        if (st) {
            if (st->data) hle_free(hle_ptr(st->data));
            if (st->del && hle_heap_ok(hle_ptr(st->del))) hle_free(hle_ptr(st->del));
        }
        hle_com_free(self);
    }
    RET(n, 1);
}
static void s_Read(void) {  /* (this, pv, cb, pcbRead) - 4 */
    stream_t *st = STREAM(A32(0));
    uint32_t want = A32(2), have = st ? st->size - (st->pos < st->size ? st->pos : st->size) : 0;
    uint32_t n = want < have ? want : have;
    if (n && A32(1) && st) memcpy(hle_ptr(A32(1)), hle_ptr(st->data + st->pos), n);
    if (st) st->pos += n;
    if (A32(3)) MEM32(A32(3)) = n;
    RET(S_OK, 4);
}
static void s_Write(void) { /* (this, pv, cb, pcbWritten) - 4 */
    stream_t *st = STREAM(A32(0));
    uint32_t want = A32(2);
    if (!st || (want && !A32(1))) RET(E_INVALIDARG, 4);
    if (st->pos + want > st->size) {
        void *p = hle_heap_realloc(hle_ptr(st->data), st->pos + want, 1);
        if (!p) RET(0x8007000Eu, 4);      /* E_OUTOFMEMORY */
        st->data = hle_va(p), st->size = st->pos + want;
    }
    if (want) memcpy(hle_ptr(st->data + st->pos), hle_ptr(A32(1)), want);
    st->pos += want;
    if (A32(3)) MEM32(A32(3)) = want;
    RET(S_OK, 4);
}
static void s_Seek(void) {  /* (this, LARGE_INTEGER dlibMove, DWORD dwOrigin, ULARGE_INTEGER *plibNewPosition) - 5 stdcall dwords */
    stream_t *st = STREAM(A32(0));
    int64_t off = (int64_t)((uint64_t)A32(1) | ((uint64_t)A32(2) << 32));
    uint32_t origin = A32(3);
    uint32_t newpos = A32(4);
    uint64_t base;
    if (!st) RET(E_INVALIDARG, 5);
    base = origin == 0 ? 0 : origin == 1 ? st->pos : origin == 2 ? st->size : 0;
    if (off < 0 && (uint64_t)(-off) > base) RET(E_INVALIDARG, 5);
    st->pos = (uint32_t)(base + (uint64_t)off);
    if (newpos) { MEM32(newpos) = st->pos; MEM32(newpos + 4) = 0; }
    RET(S_OK, 5);
}
static void s_SetSize(void) { /* (this, uint64 size) - 3 */
    stream_t *st = STREAM(A32(0));
    uint32_t n = A32(1);
    void *p;
    if (!st || A32(2)) RET(E_INVALIDARG, 3);
    p = hle_heap_realloc(hle_ptr(st->data), n ? n : 1, 1);
    if (!p) RET(0x8007000Eu, 3);
    st->data = hle_va(p), st->size = n;
    if (st->pos > n) st->pos = n;
    RET(S_OK, 3);
}
static void s_CopyTo(void) { /* (this, dst, uint64 cb, pcbRead, pcbWritten) - 6 */
    fprintf(stderr, "[ole32] IStream::CopyTo is not supported by the HLE\n"); RET(E_NOTIMPL_OLE, 6);
}
static void s_Commit(void)   { fprintf(stderr, "[ole32] IStream::Commit is not supported\n"); RET(E_NOTIMPL_OLE, 2); }
static void s_Revert(void)   { fprintf(stderr, "[ole32] IStream::Revert is not supported\n"); RET(E_NOTIMPL_OLE, 1); }
static void s_LockRegion(void)   { fprintf(stderr, "[ole32] IStream::LockRegion is not supported\n"); RET(E_NOTIMPL_OLE, 6); }
static void s_UnlockRegion(void) { fprintf(stderr, "[ole32] IStream::UnlockRegion is not supported\n"); RET(E_NOTIMPL_OLE, 6); }
static void s_Stat(void) { /* (this, STATSTG*, flag) - 3 */
    stream_t *st = STREAM(A32(0));
    uint32_t m = A32(1);
    if (!st || !m) RET(E_INVALIDARG, 3);
    memset(hle_ptr(m), 0, 72);
    MEM32(m + 4) = STGTY_STREAM;
    MEM32(m + 8) = st->size; MEM32(m + 12) = 0;
    MEM32(m + 40) = 1;       /* grfMode: STGM_READWRITE */
    RET(S_OK, 3);
}
static void s_Clone(void) { fprintf(stderr, "[ole32] IStream::Clone is not supported\n"); RET(E_NOTIMPL_OLE, 2); }

/* The public IStream vtable, in declaration order; 14 slots including IUnknown. */
static const win32hle_shim stream_methods[] = {
    { "IStream::QueryInterface", s_QueryInterface },
    { "IStream::AddRef",         s_AddRef },
    { "IStream::Release",        s_Release },
    { "IStream::Read",           s_Read },
    { "IStream::Write",          s_Write },
    { "IStream::Seek",           s_Seek },
    { "IStream::SetSize",        s_SetSize },
    { "IStream::CopyTo",         s_CopyTo },
    { "IStream::Commit",         s_Commit },
    { "IStream::Revert",         s_Revert },
    { "IStream::LockRegion",     s_LockRegion },
    { "IStream::UnlockRegion",   s_UnlockRegion },
    { "IStream::Stat",           s_Stat },
    { "IStream::Clone",          s_Clone },
    { NULL, NULL }
};
static void o_CreateStreamOnHGlobal(void) { /* (hglobal, deleteOnRelease, &stream) - stdcall 3 */
    uint32_t hg = A32(0), del = A32(1), out = A32(2), obj;
    stream_t *st;
    if (!out) RET(E_INVALIDARG, 3);
    MEM32(out) = 0;
    if (!g_vt_stream) g_vt_stream = hle_com_vtable(stream_methods);
    obj = hle_com_new(g_vt_stream, 8u + sizeof(stream_t));
    if (!obj) RET(0x8007000Eu, 3);
    st = STREAM(obj);
    st->size = hle_heap_ok(hle_ptr(hg)) ? hle_heap_size(hle_ptr(hg)) : 0;
    st->data = hle_va(hle_alloc(st->size ? st->size : 1));
    if (st->size) memcpy(hle_ptr(st->data), hle_ptr(hg), st->size);
    st->del = del ? hg : 0;
    MEM32(out) = obj;
    RET(S_OK, 3);
}

const win32hle_shim win32hle_ole32[] = {
    { "CreateStreamOnHGlobal", o_CreateStreamOnHGlobal },
    { "CoCreateGuid",          o_CoCreateGuid },
    { "CoCreateInstance",      o_CoCreateInstance },
    { "OleInitialize",         o_OleInitialize },
    { "OleUninitialize",       o_OleUninitialize },
    { "CoInitialize",          o_CoInitialize },
    { "CoUninitialize",        o_CoUninitialize },
    { "CoRegisterClassObject", o_CoRegisterClassObject },
    { "CoRevokeClassObject",   o_CoRevokeClassObject },
    { "CoDisconnectObject",    o_CoDisconnectObject },
    { "CoFileTimeNow",         o_CoFileTimeNow },
    { "CLSIDFromString",       o_CLSIDFromString },
    { "StringFromGUID2",       o_StringFromGUID2 },
    { "StringFromCLSID",       o_StringFromCLSID },
    { "OleRun",                o_OleRun },
#define AUT(ord, name, fn) { "oleaut32.dll#" #ord, fn }, { name, fn }
    AUT(2, "SysAllocString",        a_SysAllocString),
    AUT(6, "SysFreeString",         a_SysFreeString),
    AUT(8, "VariantInit",           a_VariantInit),
    AUT(9, "VariantClear",          a_VariantClear),
    AUT(12, "VariantChangeType",    a_VariantChangeType),
    AUT(200, "GetErrorInfo",        a_GetErrorInfo),
    AUT(201, "SetErrorInfo",        o_notimpl_2),
    AUT(202, "CreateErrorInfo",     o_notimpl_1),
    AUT(161, "LoadTypeLib",         o_notimpl_2),
    AUT(33, "RegisterActiveObject", o_notimpl_4),
    AUT(34, "RevokeActiveObject",   o_notimpl_2),
    { 0, 0 }
};
