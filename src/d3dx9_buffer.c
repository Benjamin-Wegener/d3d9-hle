/*
 * d3dx9_buffer.c - D3DXCreateBuffer and its ID3DXBuffer interface.
 *
 * Public references (ABI only - no Microsoft binary or foreign code used):
 *   https://raw.githubusercontent.com/wine-mirror/wine/master/include/d3dx9mesh.h
 *       HRESULT WINAPI D3DXCreateBuffer(DWORD size, ID3DXBuffer **buffer);
 *   https://raw.githubusercontent.com/wine-mirror/wine/master/include/d3dx9core.h
 *       DECLARE_INTERFACE_(ID3DXBuffer, IUnknown):
 *         QueryInterface(REFIID, void**) / AddRef() / Release() /
 *         GetBufferPointer() / GetBufferSize()   -- exactly five slots, in
 *         that order (IUnknown first, then the ID3DXBuffer methods).
 *   https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/d3dx9_36/core.c
 *       null out pointer -> D3DERR_INVALIDCALL; allocation failure ->
 *       E_OUTOFMEMORY; on success the payload is zero-filled.
 *
 * stdcall argument counts below INCLUDE `this` for the COM methods, as the
 * guest's `call [vt+4*i]` pushes it. Every shim ends in exactly one RET.
 */
#include <string.h>
#include "win32hle.h"
void d3d_QueryInterface(void);
#include "d3dx9_buffer.h"

#define D3D_OK               0x00000000u
#define S_OK                 0x00000000u
#define E_OUTOFMEMORY        0x8007000Eu
#define D3DERR_INVALIDCALL   0x8876086Cu

/* Object layout from hle_com_new(): +0 vtable, +4 refcount, +8 our state. */
typedef struct {
    uint32_t data_va;   /* guest VA of the (zeroed) payload, 0 when size == 0 */
    uint32_t size;      /* requested byte count */
} db_state_t;
#define DB(o) ((db_state_t *)((char *)hle_ptr(o) + 8u))

static uint32_t g_vt_dxbuf;

/* ---- the five ID3DXBuffer slots ---------------------------------------- */

void db_QueryInterface(void) {                     /* (this, riid, &out) */
    uint32_t self = A32(0);
    if (A32(2)) MEM32(A32(2)) = self;
    MEM32(self + 4)++;
    RET(S_OK, 3);
}

void db_AddRef(void) {                             /* (this) */
    RET(++MEM32(A32(0) + 4), 1);
}

void db_Release(void) {                            /* (this) */
    uint32_t self = A32(0);
    uint32_t n = --MEM32(self + 4);
    if (!n) {
        db_state_t *s = DB(self);
        if (s && s->data_va) hle_free(hle_ptr(s->data_va));
        hle_com_free(self);
    }
    RET(n, 1);
}

void db_GetBufferPointer(void) {                   /* (this) -> void * */
    db_state_t *s = DB(A32(0));
    RET(s ? s->data_va : 0u, 1);
}

void db_GetBufferSize(void) {                      /* (this) -> DWORD */
    db_state_t *s = DB(A32(0));
    RET(s ? s->size : 0u, 1);
}

static const win32hle_shim db_methods[] = {
    { "dxb_QueryInterface",    d3d_QueryInterface },
    { "dxb_AddRef",            db_AddRef },
    { "dxb_Release",           db_Release },
    { "dxb_GetBufferPointer",  db_GetBufferPointer },
    { "dxb_GetBufferSize",     db_GetBufferSize },
    { 0, 0 }
};

/* ---- D3DXCreateBuffer -------------------------------------------------- */

void s_D3DXCreateBuffer(void) {                    /* (size, &buffer) */
    uint32_t size = A32(0);
    uint32_t pp = A32(1);
    uint32_t hr = D3D_OK;

    if (!pp) {
        hr = D3DERR_INVALIDCALL;
    } else {
        MEM32(pp) = 0;
        if (!g_vt_dxbuf) g_vt_dxbuf = hle_com_vtable(db_methods);
        uint32_t obj = hle_com_new(g_vt_dxbuf, 8u + (uint32_t)sizeof(db_state_t));
        if (!obj) {
            hr = E_OUTOFMEMORY;
        } else {
            db_state_t *s = DB(obj);
            s->size = size;
            s->data_va = 0;
            if (size) {
                void *payload = hle_alloc(size);   /* zeroed by hle_alloc */
                if (!payload) {
                    hle_com_free(obj);
                    obj = 0;
                    hr = E_OUTOFMEMORY;
                } else {
                    s->data_va = hle_va(payload);
                }
            }
            if (obj) MEM32(pp) = obj;
        }
    }
    RET(hr, 2);
}

/* Host-side constructor for every other D3DX9 module that hands the guest an
 * ID3DXBuffer (shader blobs, error text, adjacency, ...): one implementation
 * of the interface for all of them. The payload is copied. 0 on failure. */
uint32_t d3dx9_buffer_new(const void *data, size_t size) {
    if (!g_vt_dxbuf) g_vt_dxbuf = hle_com_vtable(db_methods);
    uint32_t obj = hle_com_new(g_vt_dxbuf, 8u + (uint32_t)sizeof(db_state_t));
    if (!obj) return 0;
    db_state_t *s = DB(obj);
    s->size = (uint32_t)size;
    s->data_va = 0;
    if (size) {
        void *payload = hle_alloc((uint32_t)size);
        if (!payload) { hle_com_free(obj); return 0; }
        if (data) memcpy(payload, data, size);
        s->data_va = hle_va(payload);
    }
    return obj;
}
