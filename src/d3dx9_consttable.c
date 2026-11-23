/*
 * d3dx9_consttable.c - D3DXGetShaderVersion / D3DXGetShaderSize /
 * D3DXFindShaderComment / D3DXGetShaderConstantTable and the ID3DXConstantTable
 * object they return.
 *
 * The CTAB layout follows the public d3dx9shader.h structures: a 7-DWORD
 * D3DXSHADER_CONSTANTTABLE header followed by D3DXSHADER_CONSTANTINFO records
 * at the header's ConstantInfo byte offset and D3DXSHADER_TYPEINFO records at
 * each record's TypeInfo byte offset. Creator, target, names and default
 * values are byte offsets from the table start, never guest addresses. Every
 * offset is bounds-checked against the comment payload. The compiled-effect
 * parser (d3dx9_fxbin / 9b20a12) is not read or used.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "win32hle.h"
#include "d3dx9_consttable.h"

/* ID3DXConstantTable object in guest memory:
 *   [0] vtable VA, [4] refcount, [8..] ct_state_t. */
typedef struct {
    uint32_t shader;      /* guest VA of the shader buffer */
    uint32_t size;        /* total shader size in bytes */
    uint32_t table;       /* guest VA of the D3DXSHADER_CONSTANTTABLE */
    uint32_t table_bytes; /* length of the CTAB comment payload */
    uint32_t count;       /* Constants */
    uint32_t buffer;      /* guest VA of the copied CTAB payload (ID3DXBuffer) */
} ct_state_t;
#define CT(o) ((ct_state_t *)((char *)hle_ptr(o) + 8u))

static uint32_t g_vt_ct;
static const win32hle_shim ct_methods[];

/* ---- helpers ---- */

static D3DXSHADER_CONSTANTTABLE *ct_header(const ct_state_t *ct) {
    return ct ? (D3DXSHADER_CONSTANTTABLE *)hle_ptr(ct->table) : NULL;
}

/* Real D3D9 bytecode has the CTAB comment token at DWORD 1; DWORD 1 is NOT a
 * size field. Walk tokens to END, skipping comment payloads, and return the
 * number of DWORDs (bounded so a broken shader cannot loop forever). */
static uint32_t shader_dwords(uint32_t pf) {
    uint32_t i;
    if (!pf) return 0u;
    for (i = 0u; i < (1u << 20); i++) {
        uint32_t tok = MEM32(pf + 4u * i);
        uint32_t op = tok & 0xFFFFu, len = (tok >> 16u) & 0xFFFFu;
        if (op == D3DSIO_END) return i + 1u;
        if (op == D3DSIO_COMMENT) {
            if (!len || i + len >= (1u << 20)) return i + 1u;
            i += len;                       /* the token is followed by len DWORDs */
        }
    }
    return 0u;
}

/* Turn a documented byte offset from the table start into a guest VA, with a
 * bounds check. Zero means "absent" and never becomes a pointer. */
static uint32_t ct_off(const ct_state_t *ct, uint32_t off, uint32_t need) {
    if (!ct || off == 0u) return 0u;
    if (off > ct->table_bytes || need > ct->table_bytes - off) return 0u;
    return ct->table + off;
}

static D3DXSHADER_CONSTANTINFO *ct_record(const ct_state_t *ct, uint32_t index) {
    D3DXSHADER_CONSTANTTABLE *t = ct_header(ct);
    uint32_t va;
    if (!t || index == 0u || index > ct->count) return NULL;
    if (t->ConstantInfo > ct->table_bytes ||
        (uint64_t)index * sizeof(D3DXSHADER_CONSTANTINFO) > ct->table_bytes - t->ConstantInfo)
        return NULL;
    va = ct_off(ct, t->ConstantInfo + (index - 1u) * (uint32_t)sizeof(D3DXSHADER_CONSTANTINFO),
                (uint32_t)sizeof(D3DXSHADER_CONSTANTINFO));
    return (D3DXSHADER_CONSTANTINFO *)hle_ptr(va);
}

/* Bytes occupied by a constant, derived from its public TYPEINFO. Scalars and
 * vectors have one row; a matrix has Rows x Columns; arrays multiply by the
 * element count. Each component is a 4-byte float/int/bool register. */
static uint32_t ct_bytes(const D3DXSHADER_TYPEINFO *ti) {
    uint32_t rows = ti->Rows ? ti->Rows : 1u;
    uint32_t cols = ti->Columns ? ti->Columns : 1u;
    uint32_t elems = ti->Elements ? ti->Elements : 1u;
    return 4u * rows * cols * elems;
}

/* ---- D3DX shader helpers (stdcall) ---- */

void s_D3DXGetShaderVersion(void) {            /* (const DWORD *pFunction) -> DWORD */
    uint32_t pf = A32(0);
    RET(pf ? MEM32(pf) : 0u, 1);
}

void s_D3DXGetShaderSize(void) {               /* (const DWORD *pFunction) -> SIZE_T (bytes) */
    RET(shader_dwords(A32(0)) * 4u, 1);
}

void s_D3DXFindShaderComment(void) {           /* (pFunction, FourCC, ppData, pSize) -> HRESULT */
    uint32_t pf = A32(0), fourcc = A32(1), ppdata = A32(2), psize = A32(3);
    uint32_t n, i;
    if (!pf || !ppdata || !psize) RET(D3DERR_INVALIDCALL, 4);
    MEM32(ppdata) = 0u;
    MEM32(psize) = 0u;
    n = shader_dwords(pf);
    for (i = 1u; i + 1u < n; i++) {
        uint32_t tok = MEM32(pf + 4u * i);
        uint32_t len;
        if ((tok & 0xFFFFu) == D3DSIO_END) break;
        if ((tok & 0xFFFFu) != D3DSIO_COMMENT) continue;
        len = (tok >> 16) & 0xFFFFu;
        if (len >= 1u && i + len < n && MEM32(pf + 4u * (i + 1u)) == fourcc) {
            MEM32(ppdata) = pf + 4u * (i + 2u);          /* payload after the fourcc */
            MEM32(psize) = (len - 1u) * 4u;
            RET(D3D_OK, 4);
        }
        i += len;
    }
    RET(E_FAIL, 4);
}

/* The ID3DXConstantTable of the shader whose tokens are at guest address `pf`
 * (its CTAB comment). *hr gets the HRESULT; 0 is returned on failure. The table
 * keeps pointing at the tokens: they must outlive it. */
uint32_t d3dx9_consttable_from_shader(uint32_t pf, uint32_t *hr) {
    uint32_t n = shader_dwords(pf), i;
    *hr = E_FAIL;
    for (i = 1u; i + 1u < n; i++) {
        uint32_t tok = MEM32(pf + 4u * i);
        uint32_t len;
        if ((tok & 0xFFFFu) == D3DSIO_END) break;
        if ((tok & 0xFFFFu) != D3DSIO_COMMENT) continue;
        len = (tok >> 16) & 0xFFFFu;
        if (len >= 4u && i + len < n && MEM32(pf + 4u * (i + 1u)) == CTAB_FOURCC) {
            uint32_t tabva = pf + 4u * (i + 2u);
            uint32_t payload = (len - 1u) * 4u;
            D3DXSHADER_CONSTANTTABLE *t = (D3DXSHADER_CONSTANTTABLE *)hle_ptr(tabva);
            uint32_t obj;
            if (!t || payload < sizeof(*t) || t->Size != sizeof(*t) ||
                t->Constants > 1024u) {
                *hr = D3DERR_INVALIDCALL;
                return 0;
            }
            if (t->Constants > 0u) {
                if (t->ConstantInfo == 0u ||
                    (uint64_t)t->Constants * sizeof(D3DXSHADER_CONSTANTINFO) > payload - t->ConstantInfo) {
                    *hr = D3DERR_INVALIDCALL;
                    return 0;
                }
            }
            void *copy = hle_alloc(payload);
            if (!copy) { *hr = D3DERR_OUTOFVIDEOMEMORY; return 0; }
            memcpy(copy, t, payload);
            if (!g_vt_ct) g_vt_ct = hle_com_vtable(ct_methods);
            obj = hle_com_new(g_vt_ct, 8u + sizeof(ct_state_t));
            CT(obj)->shader = pf;
            CT(obj)->size = shader_dwords(pf) * 4u;
            CT(obj)->table = tabva;
            CT(obj)->table_bytes = payload;
            CT(obj)->count = t->Constants;
            CT(obj)->buffer = hle_va(copy);   /* ID3DXBuffer: a copy of the CTAB payload */
            *hr = D3D_OK;
            return obj;
        }
        i += len;
    }
    return 0;
}

void s_D3DXGetShaderConstantTable(void) {      /* (pFunction, ppTable) -> HRESULT */
    uint32_t pf = A32(0), pp = A32(1), hr;
    if (!pf || !pp) RET(D3DERR_INVALIDCALL, 2);
    MEM32(pp) = 0u;
    MEM32(pp) = d3dx9_consttable_from_shader(pf, &hr);
    RET(hr, 2);
}

/* ---- ID3DXConstantTable methods (this = A32(0)) ---- */

/* ID3DXBuffer: the CTAB payload copy, not the shader and not a token. */
static void ct_get_buffer_pointer(void) {
    ct_state_t *ct = CT(A32(0));
    RET(ct ? ct->buffer : 0u, 1);
}

static void ct_get_buffer_size(void) {
    ct_state_t *ct = CT(A32(0));
    RET(ct ? ct->table_bytes : 0u, 1);
}

/* Release the payload copy with the object. */
static void ct_release(void) {
    uint32_t self = A32(0), n = --MEM32(self + 4);
    ct_state_t *ct = CT(self);
    if (!n) {
        if (ct && ct->buffer) hle_free(hle_ptr(ct->buffer));
        hle_com_free(self);
    }
    RET(n, 1);
}

static void ct_get_desc(void) {                /* (this, D3DXCONSTANTTABLE_DESC*) */
    uint32_t desc = A32(1);
    ct_state_t *ct = CT(A32(0));
    D3DXSHADER_CONSTANTTABLE *t = ct_header(ct);
    D3DXCONSTANTTABLE_DESC d;
    if (!t || !desc) RET(D3DERR_INVALIDCALL, 2);
    memset(&d, 0, sizeof d);
    d.Creator = ct_off(ct, t->Creator, 1u);
    d.Version = t->Version;
    d.Constants = ct->count;
    memcpy(hle_ptr(desc), &d, sizeof d);
    RET(D3D_OK, 2);
}

static void ct_get_constant_desc(void) {       /* (this, h, D3DXCONSTANT_DESC*, UINT*) */
    uint32_t h = A32(1), desc = A32(2), pcount = A32(3);
    ct_state_t *ct = CT(A32(0));
    D3DXSHADER_CONSTANTINFO *c = ct_record(ct, h);
    D3DXCONSTANT_DESC d;
    D3DXSHADER_TYPEINFO ti;
    uint32_t tiva;
    if (!c || !desc || !pcount) RET(D3DERR_INVALIDCALL, 4);
    tiva = ct_off(ct, c->TypeInfo, (uint32_t)sizeof ti);
    if (!tiva) RET(D3DERR_INVALIDCALL, 4);
    memcpy(&ti, hle_ptr(tiva), sizeof ti);
    memset(&d, 0, sizeof d);
    d.Name = ct_off(ct, c->Name, 1u);
    if (!d.Name) RET(D3DERR_INVALIDCALL, 4);
    d.RegisterSet = c->RegisterSet;
    d.RegisterIndex = c->RegisterIndex;
    d.RegisterCount = c->RegisterCount;
    d.Class = ti.Class;
    d.Type = ti.Type;
    d.Rows = ti.Rows;
    d.Columns = ti.Columns;
    d.Elements = ti.Elements;
    d.StructMembers = ti.StructMembers;
    d.Bytes = ct_bytes(&ti);
    d.DefaultValue = ct_off(ct, c->DefaultValue, 1u);
    memcpy(hle_ptr(desc), &d, sizeof d);
    MEM32(pcount) = 1u;
    RET(D3D_OK, 4);
}

static void ct_get_sampler_index(void) {
    D3DXSHADER_CONSTANTINFO *c = ct_record(CT(A32(0)), A32(1));
    RET(c ? c->RegisterIndex : 0xFFFFFFFFu, 2);
}

static void ct_get_constant(void) {
    uint32_t index = A32(2);
    ct_state_t *ct = CT(A32(0));
    RET(ct && index < ct->count ? index + 1u : 0u, 3);
}

static void ct_get_constant_by_name(void) {
    uint32_t name = A32(2);
    ct_state_t *ct = CT(A32(0));
    uint32_t i;
    if (!ct || !name) RET(0u, 3);
    for (i = 1u; i <= ct->count; i++) {
        D3DXSHADER_CONSTANTINFO *c = ct_record(ct, i);
        uint32_t nva = c ? ct_off(ct, c->Name, 1u) : 0u;
        if (nva && !strcmp((const char *)hle_ptr(nva), (const char *)hle_ptr(name)))
            RET(i, 3);
    }
    RET(0u, 3);
}

static void ct_get_constant_element(void) {
    uint32_t index = A32(2);
    ct_state_t *ct = CT(A32(0));
    RET(ct && index < ct->count ? index + 1u : 0u, 3);
}

/* ---- value setters: forward to the real D3D9 device constant calls ----
 * kind 0 = FLOAT (16 bytes/register), 1 = INT (16), 2 = BOOL (4). */
static uint32_t ct_set_dev(ct_state_t *ct, uint32_t dev, uint32_t h,
                           const uint8_t *bytes, uint32_t nbytes, int kind, uint32_t nreg) {
    D3DXSHADER_CONSTANTINFO *c = ct_record(ct, h);
    uint32_t width = kind == 2 ? 4u : 16u, total, va;
    void *g;
    const char *vsn, *psn;
    if (!c || !dev || !bytes) return D3DERR_INVALIDCALL;
    if (!nreg) nreg = c->RegisterCount ? c->RegisterCount : 1u;
    if (nreg > 256u) return D3DERR_INVALIDCALL;
    total = nreg * width;
    g = hle_alloc(total);
    if (!g) return D3DERR_OUTOFVIDEOMEMORY;
    memset(g, 0, total);
    memcpy(g, bytes, nbytes < total ? nbytes : total);
    va = hle_va(g);
    vsn = kind == 0 ? "IDirect3DDevice9::SetVertexShaderConstantF" :
          kind == 1 ? "IDirect3DDevice9::SetVertexShaderConstantI" :
                      "IDirect3DDevice9::SetVertexShaderConstantB";
    psn = kind == 0 ? "IDirect3DDevice9::SetPixelShaderConstantF" :
          kind == 1 ? "IDirect3DDevice9::SetPixelShaderConstantI" :
                      "IDirect3DDevice9::SetPixelShaderConstantB";
    uint32_t vf = hle_resolve(vsn), pf = hle_resolve(psn);
    if (!vf || !pf) { hle_free(g); return D3DERR_INVALIDCALL; }
    hle_call_guest(vf, 4, (uint32_t[]){ dev, c->RegisterIndex, va, nreg });
    hle_call_guest(pf, 4, (uint32_t[]){ dev, c->RegisterIndex, va, nreg });
    hle_free(g);
    return D3D_OK;
}

/* SetDefaults writes each constant's CTAB default value (a byte offset from
 * the table start) into the device's register file at that constant's own
 * register index. Constants without a DefaultValue are left untouched. */
static void ct_set_defaults(void) {
    uint32_t self = A32(0), dev = A32(1);
    ct_state_t *ct = CT(self);
    if (!ct || !dev || !ct_header(ct)) RET(D3DERR_INVALIDCALL, 2);

    for (uint32_t i = 1u; i <= ct->count; i++) {
        D3DXSHADER_CONSTANTINFO *c = ct_record(ct, i);
        D3DXSHADER_TYPEINFO ti;
        uint32_t tiva, defva, avail, nbytes, nreg, kind, hr;

        if (!c) continue;
        tiva = ct_off(ct, c->TypeInfo, (uint32_t)sizeof ti);
        if (!tiva) continue;
        defva = ct_off(ct, c->DefaultValue, 1u);
        if (!defva) continue;              /* no default for this constant */
        memcpy(&ti, hle_ptr(tiva), sizeof ti);

        kind = ti.Type == D3DXPT_BOOL ? 2u : ti.Type == D3DXPT_INT ? 1u : 0u;
        nreg = c->RegisterCount ? c->RegisterCount : 1u;
        nbytes = nreg * (kind == 2u ? 4u : 16u);
        /* Do not read past the comment payload; ct_set_dev zero-fills any
         * missing tail bytes. */
        avail = c->DefaultValue <= ct->table_bytes ? ct->table_bytes - c->DefaultValue : 0u;
        if (nbytes > avail) nbytes = avail;

        hr = ct_set_dev(ct, dev, i, (const uint8_t *)hle_ptr(defva), nbytes, kind, nreg);
        if (hr != D3D_OK) RET(hr, 2);
    }
    RET(D3D_OK, 2);
}

static void transpose64(uint8_t *dst, const uint8_t *src) {
    const float *s = (const float *)src;
    float *d = (float *)dst;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) d[r * 4 + c] = s[c * 4 + r];
}

static void ct_set_value(void) {              /* (this, device, h, pData, Bytes) */
    uint32_t dev = A32(1), h = A32(2), data = A32(3), nbytes = A32(4);
    ct_state_t *ct = CT(A32(0));
    D3DXSHADER_CONSTANTINFO *c = ct_record(ct, h);
    D3DXSHADER_TYPEINFO ti;
    uint32_t tiva = c ? ct_off(ct, c->TypeInfo, (uint32_t)sizeof ti) : 0u;
    int kind;
    if (!c || !dev || !data || !nbytes || !tiva) RET(D3DERR_INVALIDCALL, 5);
    memcpy(&ti, hle_ptr(tiva), sizeof ti);
    kind = ti.Type == D3DXPT_BOOL ? 2u : ti.Type == D3DXPT_INT ? 1u : 0u;
    RET(ct_set_dev(ct, dev, h, (const uint8_t *)hle_ptr(data), nbytes, kind, c->RegisterCount), 5);
}
static void ct_set_bool(void) {
    uint32_t v = A32(3);
    RET(ct_set_dev(CT(A32(0)), A32(1), A32(2), (const uint8_t *)&v, 4u, 2, 1u), 4);
}
static void ct_set_bool_array(void) {
    uint32_t n = A32(4);
    if (!n || n > 256u || !A32(3)) RET(D3DERR_INVALIDCALL, 5);
    RET(ct_set_dev(CT(A32(0)), A32(1), A32(2), (const uint8_t *)hle_ptr(A32(3)), n * 4u, 2, n), 5);
}
static void ct_set_int(void) {
    int32_t v[4] = { (int32_t)A32(3), 0, 0, 0 };
    RET(ct_set_dev(CT(A32(0)), A32(1), A32(2), (const uint8_t *)v, 16u, 1, 1u), 4);
}
static void ct_set_int_array(void) {
    uint32_t n = A32(4);
    if (!n || n > 256u || !A32(3)) RET(D3DERR_INVALIDCALL, 5);
    RET(ct_set_dev(CT(A32(0)), A32(1), A32(2), (const uint8_t *)hle_ptr(A32(3)), n * 16u, 1, n), 5);
}
static void ct_set_float(void) {
    uint32_t bits = A32(3);
    float value;
    memcpy(&value, &bits, 4);
    float v[4] = { value, 0.0f, 0.0f, 0.0f };
    RET(ct_set_dev(CT(A32(0)), A32(1), A32(2), (const uint8_t *)v, 16u, 0, 1u), 4);
}
static void ct_set_float_array(void) {
    uint32_t n = A32(4);
    if (!n || n > 256u || !A32(3)) RET(D3DERR_INVALIDCALL, 5);
    RET(ct_set_dev(CT(A32(0)), A32(1), A32(2), (const uint8_t *)hle_ptr(A32(3)), n * 16u, 0, n), 5);
}
static void ct_set_vector(void) {
    RET(ct_set_dev(CT(A32(0)), A32(1), A32(2),
                   (const uint8_t *)hle_ptr(A32(3)), 16u, 0, 1u), 4);
}
static void ct_set_vector_array(void) {
    uint32_t n = A32(4);
    if (!n || n > 256u || !A32(3)) RET(D3DERR_INVALIDCALL, 5);
    RET(ct_set_dev(CT(A32(0)), A32(1), A32(2),
                   (const uint8_t *)hle_ptr(A32(3)), n * 16u, 0, n), 5);
}
static void ct_set_matrix(void) {
    RET(ct_set_dev(CT(A32(0)), A32(1), A32(2),
                   (const uint8_t *)hle_ptr(A32(3)), 64u, 0, 4u), 4);
}
static void ct_set_matrix_array(void) {
    uint32_t n = A32(4);
    if (!n || n > 64u || !A32(3)) RET(D3DERR_INVALIDCALL, 5);
    RET(ct_set_dev(CT(A32(0)), A32(1), A32(2),
                   (const uint8_t *)hle_ptr(A32(3)), n * 64u, 0, n * 4u), 5);
}
/* Pointer-array variants gather the matrices from the guest pointers first. */
static uint32_t ct_set_matrix_ptrs(uint32_t self, uint32_t pp, uint32_t n, int trans) {
    uint8_t *buf;
    if (!pp || !n || n > 64u) return D3DERR_INVALIDCALL;
    buf = (uint8_t *)malloc((size_t)n * 64u);
    if (!buf) return D3DERR_OUTOFVIDEOMEMORY;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *m = (const uint8_t *)hle_ptr(MEM32(pp + 4u * i));
        if (!m) { free(buf); return D3DERR_INVALIDCALL; }
        if (trans) transpose64(buf + i * 64u, m);
        else memcpy(buf + i * 64u, m, 64u);
    }
    uint32_t hr = ct_set_dev(CT(self), A32(1), A32(2), buf, n * 64u, 0, n * 4u);
    free(buf);
    return hr;
}
static void ct_set_matrix_pointer_array(void) {
    RET(ct_set_matrix_ptrs(A32(0), A32(3), A32(4), 0), 5);
}
static void ct_set_matrix_transpose(void) {
    uint8_t m[64];
    if (!A32(3)) RET(D3DERR_INVALIDCALL, 4);
    transpose64(m, (const uint8_t *)hle_ptr(A32(3)));
    RET(ct_set_dev(CT(A32(0)), A32(1), A32(2), m, 64u, 0, 4u), 4);
}
static void ct_set_matrix_transpose_array(void) {
    uint32_t n = A32(4);
    if (!n || n > 64u || !A32(3)) RET(D3DERR_INVALIDCALL, 5);
    uint8_t *buf = (uint8_t *)malloc((size_t)n * 64u);
    if (!buf) RET(D3DERR_OUTOFVIDEOMEMORY, 5);
    for (uint32_t i = 0; i < n; i++)
        transpose64(buf + i * 64u, (const uint8_t *)hle_ptr(A32(3)) + i * 64u);
    uint32_t hr = ct_set_dev(CT(A32(0)), A32(1), A32(2), buf, n * 64u, 0, n * 4u);
    free(buf);
    RET(hr, 5);
}
static void ct_set_matrix_transpose_pointer_array(void) {
    RET(ct_set_matrix_ptrs(A32(0), A32(3), A32(4), 1), 5);
}

/* ID3DXConstantTable's own QueryInterface. The shared d3d_QueryInterface from
 * d3d9_com.c answers only after looking up the vtable's registered name; the
 * standalone CTAB robustness harness links d3dx9_consttable.c without that
 * file, so keep this object's few IIDs right here. The IIDs are the published
 * GUIDs from Wine's include/d3dx9shader.h and d3dx9core.h. */
static void ct_QueryInterface(void) {                /* (this, riid, &out) */
    static const uint8_t iid_unknown[16] = {
        0,0,0,0, 0,0,0,0, 0xC0,0,0,0, 0,0,0,0x46 };
    static const uint8_t iid_ct[16] = {
        0x90,0x31,0xCA,0x9D, 0xB9,0x38, 0xC3,0x4F, 0x92,0xE3, 0x39,0xC6,0xDD,0xFB,0x35,0x8B };
    static const uint8_t iid_buffer[16] = {
        0x08,0xFB,0xA5,0x8B, 0x95,0x51, 0xE2,0x40, 0xAC,0x58, 0x0D,0x98,0x9C,0x3A,0x01,0x02 };
    uint32_t self = A32(0), riid = A32(1), ppv = A32(2);
    if (!ppv) RET(D3DERR_INVALIDCALL, 3);
    MEM32(ppv) = 0;
    const uint8_t *iid = riid ? (const uint8_t *)hle_ptr(riid) : NULL;
    if (!iid || (memcmp(iid, iid_unknown, 16) && memcmp(iid, iid_ct, 16) && memcmp(iid, iid_buffer, 16))) {
        fprintf(stderr, "[d3dx9] ID3DXConstantTable: interface not supported\n");
        RET(0x80004002u, 3);                        /* E_NOINTERFACE */
    }
    MEM32(ppv) = self;
    MEM32(self + 4)++;
    RET(D3D_OK, 3);
}

static const win32hle_shim ct_methods[] = {
    { "ID3DXConstantTable::QueryInterface", ct_QueryInterface },
    { "ID3DXConstantTable::AddRef",         hle_com_AddRef },
    { "ID3DXConstantTable::Release",        ct_release },
    { "ID3DXConstantTable::GetBufferPointer", ct_get_buffer_pointer },
    { "ID3DXConstantTable::GetBufferSize",    ct_get_buffer_size },
    { "ID3DXConstantTable::GetDesc",          ct_get_desc },
    { "ID3DXConstantTable::GetConstantDesc",  ct_get_constant_desc },
    { "ID3DXConstantTable::GetSamplerIndex",  ct_get_sampler_index },
    { "ID3DXConstantTable::GetConstant",      ct_get_constant },
    { "ID3DXConstantTable::GetConstantByName", ct_get_constant_by_name },
    { "ID3DXConstantTable::GetConstantElement", ct_get_constant_element },
    { "ID3DXConstantTable::SetDefaults",      ct_set_defaults },
    { "ID3DXConstantTable::SetValue",         ct_set_value },
    { "ID3DXConstantTable::SetBool",          ct_set_bool },
    { "ID3DXConstantTable::SetBoolArray",     ct_set_bool_array },
    { "ID3DXConstantTable::SetInt",           ct_set_int },
    { "ID3DXConstantTable::SetIntArray",      ct_set_int_array },
    { "ID3DXConstantTable::SetFloat",         ct_set_float },
    { "ID3DXConstantTable::SetFloatArray",    ct_set_float_array },
    { "ID3DXConstantTable::SetVector",        ct_set_vector },
    { "ID3DXConstantTable::SetVectorArray",   ct_set_vector_array },
    { "ID3DXConstantTable::SetMatrix",        ct_set_matrix },
    { "ID3DXConstantTable::SetMatrixArray",   ct_set_matrix_array },
    { "ID3DXConstantTable::SetMatrixPointerArray", ct_set_matrix_pointer_array },
    { "ID3DXConstantTable::SetMatrixTranspose", ct_set_matrix_transpose },
    { "ID3DXConstantTable::SetMatrixTransposeArray", ct_set_matrix_transpose_array },
    { "ID3DXConstantTable::SetMatrixTransposePointerArray", ct_set_matrix_transpose_pointer_array },
    { 0, 0 }
};
