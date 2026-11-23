#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "win32hle.h"
void d3d_QueryInterface(void);
#include "d3dx9_matrixstack.h"
#include "d3dx9_math.h"

typedef struct {
    int capacity;
    int count;          /* number of matrices on stack */
    uint32_t mats;      /* guest VA of capacity D3DXMATRIX array */
    uint32_t device;    /* unused */
} ms_state_t;
#define MS(o) ((ms_state_t *)((char *)hle_ptr(o) + 8u))

static uint32_t g_vt_ms;

void ms_QueryInterface(void) {
    /* (this, riid, ppv) - 3 args. Must not call hle_com_AddRef() because it
     * does its own RET, which would double-RET and corrupt the guest stack. */
    uint32_t self=A32(0), ppv=A32(2); (void)A32(1);
    if (!ppv) { RET(D3DERR_INVALIDCALL,3); return; }
    MEM32(ppv) = self;
    MEM32(self + 4)++;
    RET(D3D_OK, 3);
}
void ms_AddRef(void) { uint32_t self=A32(0); MEM32(self+4)++; RET(MEM32(self+4),1); }
void ms_Release(void) { uint32_t self=A32(0); uint32_t n=--MEM32(self+4); if(!n) { ms_state_t *s=MS(self); if(s&&s->mats) hle_free(hle_ptr(s->mats)); hle_com_free(self); } RET(n,1); }

static d3dx9_matrix *top(ms_state_t *s) { return (d3dx9_matrix *)hle_ptr(s->mats + (s->count-1)*sizeof(d3dx9_matrix)); }

void ms_Push(void) {
    /* ID3DXMatrixStack::Push() takes only `this`. Reading A32(1) would pick up
     * garbage above the guest's return address and could overwrite the new
     * top; RET(argc=2) would also over-pop g_esp on every call.
     * Per D3DX9 docs the stack grows dynamically. */
    uint32_t self = A32(0);
    ms_state_t *s = MS(self);
    if (s->count >= s->capacity) {
        int newcap = s->capacity * 2;
        if (newcap < s->capacity + 1) newcap = s->capacity + 1;
        uint32_t newva = hle_va(hle_alloc((uint32_t)newcap * sizeof(d3dx9_matrix)));
        if (!newva) { RET(D3DERR_OUTOFVIDEOMEMORY, 1); return; }
        memcpy(hle_ptr(newva), hle_ptr(s->mats), (size_t)s->count * sizeof(d3dx9_matrix));
        hle_free(hle_ptr(s->mats));
        s->mats = newva;
        s->capacity = newcap;
    }
    d3dx9_matrix *src = top(s);           /* push a copy of the current matrix */
    s->count++;
    *top(s) = *src;
    RET(D3D_OK, 1);
}
void ms_Pop(void) {
    uint32_t self=A32(0);
    ms_state_t *s = MS(self);
    /* D3DX9 docs: if count==0 no action; if count==1 empties stack. Return D3D_OK. */
    if (s->count <= 1) { RET(D3D_OK, 1); return; }
    s->count--;
    RET(D3D_OK, 1);
}
void ms_GetTop(void) {
    uint32_t self=A32(0);
    ms_state_t *s = MS(self);
    RET(s->mats + (s->count-1)*sizeof(d3dx9_matrix), 1);
}

void ms_LoadIdentity(void) {
    ms_state_t *s = MS(A32(0));
    d3dx9_matrix_identity(top(s));
    RET(D3D_OK, 1);
}
void ms_LoadMatrix(void) {
    ms_state_t *s = MS(A32(0));
    uint32_t pm = A32(1);
    if (pm) { d3dx9_matrix m; memcpy(&m, hle_ptr(pm), sizeof m); *top(s) = m; }
    RET(D3D_OK, 2);
}
void ms_MultMatrix(void) {
    ms_state_t *s = MS(A32(0));
    uint32_t pm = A32(1);
    if (pm) {
        d3dx9_matrix m, r;
        memcpy(&m, hle_ptr(pm), sizeof m);
        d3dx9_matrix_multiply(&r, top(s), &m);
        *top(s) = r;
    }
    RET(D3D_OK, 2);
}
void ms_MultMatrixLocal(void) {
    ms_state_t *s = MS(A32(0));
    uint32_t pm = A32(1);
    if (pm) {
        d3dx9_matrix m, r;
        memcpy(&m, hle_ptr(pm), sizeof m);
        d3dx9_matrix_multiply(&r, &m, top(s));
        *top(s) = r;
    }
    RET(D3D_OK, 2);
}

static void build_rot_axis(d3dx9_matrix *m, const float *axis, float angle) {
    float x=axis[0], y=axis[1], z=axis[2];
    float len = sqrtf(x*x+y*y+z*z);
    if (len == 0) { d3dx9_matrix_identity(m); return; }
    x/=len; y/=len; z/=len;
    float c=cosf(angle), s=sinf(angle), t=1-c;
    m->m[0][0]=t*x*x+c; m->m[0][1]=t*x*y+s*z; m->m[0][2]=t*x*z-s*y; m->m[0][3]=0;
    m->m[1][0]=t*x*y-s*z; m->m[1][1]=t*y*y+c; m->m[1][2]=t*y*z+s*x; m->m[1][3]=0;
    m->m[2][0]=t*x*z+s*y; m->m[2][1]=t*y*z-s*x; m->m[2][2]=t*z*z+c; m->m[2][3]=0;
    m->m[3][0]=0; m->m[3][1]=0; m->m[3][2]=0; m->m[3][3]=1;
}

void ms_RotateAxis(void) {
    uint32_t self=A32(0);
    uint32_t pv = A32(1);
    float angle; uint32_t ab=A32(2); memcpy(&angle,&ab,4);
    ms_state_t *s = MS(self);
    if (pv) {
        float axis[3]; memcpy(axis, hle_ptr(pv), 12);
        d3dx9_matrix r; build_rot_axis(&r, axis, angle);
        d3dx9_matrix res; d3dx9_matrix_multiply(&res, top(s), &r);
        *top(s) = res;
    }
    RET(D3D_OK, 3);
}
void ms_RotateAxisLocal(void) {
    uint32_t self=A32(0);
    uint32_t pv = A32(1);
    float angle; uint32_t ab=A32(2); memcpy(&angle,&ab,4);
    ms_state_t *s = MS(self);
    if (pv) {
        float axis[3]; memcpy(axis, hle_ptr(pv), 12);
        d3dx9_matrix r; build_rot_axis(&r, axis, angle);
        d3dx9_matrix res; d3dx9_matrix_multiply(&res, &r, top(s));
        *top(s) = res;
    }
    RET(D3D_OK, 3);
}

void ms_RotateYawPitchRoll(void) {
    uint32_t self=A32(0);
    float yaw, pitch, roll;
    uint32_t yb=A32(1), pb=A32(2), rb=A32(3);
    memcpy(&yaw,&yb,4); memcpy(&pitch,&pb,4); memcpy(&roll,&rb,4);
    ms_state_t *s = MS(self);
    /* R = Ry(yaw) * Rx(pitch) * Rz(roll)? D3DX uses YawPitchRoll: Rz(roll)*Rx(pitch)*Ry(yaw) applied as... standard D3DXMatrixRotationYawPitchRoll = Ry(yaw)*Rx(pitch)*Rz(roll) */
    d3dx9_matrix ry, rx, rz, r, tmp;
    d3dx9_matrix_rotation_y(&ry, yaw);
    d3dx9_matrix_rotation_x(&rx, pitch);
    d3dx9_matrix_rotation_z(&rz, roll);
    /* D3DXMatrixRotationYawPitchRoll applies roll, then pitch, then yaw:
     * R = Rz(roll) * Rx(pitch) * Ry(yaw) for row vectors. */
    d3dx9_matrix_multiply(&tmp, &rz, &rx);
    d3dx9_matrix_multiply(&r, &tmp, &ry);
    d3dx9_matrix res; d3dx9_matrix_multiply(&res, top(s), &r);
    *top(s) = res;
    RET(D3D_OK, 4);
}
void ms_RotateYawPitchRollLocal(void) {
    uint32_t self=A32(0);
    float yaw, pitch, roll;
    uint32_t yb=A32(1), pb=A32(2), rb=A32(3);
    memcpy(&yaw,&yb,4); memcpy(&pitch,&pb,4); memcpy(&roll,&rb,4);
    ms_state_t *s = MS(self);
    d3dx9_matrix ry, rx, rz, r, tmp;
    d3dx9_matrix_rotation_y(&ry, yaw);
    d3dx9_matrix_rotation_x(&rx, pitch);
    d3dx9_matrix_rotation_z(&rz, roll);
    /* D3DXMatrixRotationYawPitchRoll applies roll, then pitch, then yaw:
     * R = Rz(roll) * Rx(pitch) * Ry(yaw) for row vectors. */
    d3dx9_matrix_multiply(&tmp, &rz, &rx);
    d3dx9_matrix_multiply(&r, &tmp, &ry);
    d3dx9_matrix res; d3dx9_matrix_multiply(&res, &r, top(s));
    *top(s) = res;
    RET(D3D_OK, 4);
}

void ms_Scale(void) {
    uint32_t self=A32(0);
    float x,y,z;
    uint32_t xb=A32(1), yb=A32(2), zb=A32(3);
    memcpy(&x,&xb,4); memcpy(&y,&yb,4); memcpy(&z,&zb,4);
    ms_state_t *s = MS(self);
    d3dx9_matrix sc; d3dx9_matrix_scaling(&sc, x, y, z);
    d3dx9_matrix res; d3dx9_matrix_multiply(&res, top(s), &sc);
    *top(s) = res;
    RET(D3D_OK, 4);
}
void ms_ScaleLocal(void) {
    uint32_t self=A32(0);
    float x,y,z;
    uint32_t xb=A32(1), yb=A32(2), zb=A32(3);
    memcpy(&x,&xb,4); memcpy(&y,&yb,4); memcpy(&z,&zb,4);
    ms_state_t *s = MS(self);
    d3dx9_matrix sc; d3dx9_matrix_scaling(&sc, x, y, z);
    d3dx9_matrix res; d3dx9_matrix_multiply(&res, &sc, top(s));
    *top(s) = res;
    RET(D3D_OK, 4);
}
void ms_Translate(void) {
    uint32_t self=A32(0);
    float x,y,z;
    uint32_t xb=A32(1), yb=A32(2), zb=A32(3);
    memcpy(&x,&xb,4); memcpy(&y,&yb,4); memcpy(&z,&zb,4);
    ms_state_t *s = MS(self);
    d3dx9_matrix tr; d3dx9_matrix_translation(&tr, x, y, z);
    d3dx9_matrix res; d3dx9_matrix_multiply(&res, top(s), &tr);
    *top(s) = res;
    RET(D3D_OK, 4);
}
void ms_TranslateLocal(void) {
    uint32_t self=A32(0);
    float x,y,z;
    uint32_t xb=A32(1), yb=A32(2), zb=A32(3);
    memcpy(&x,&xb,4); memcpy(&y,&yb,4); memcpy(&z,&zb,4);
    ms_state_t *s = MS(self);
    d3dx9_matrix tr; d3dx9_matrix_translation(&tr, x, y, z);
    d3dx9_matrix res; d3dx9_matrix_multiply(&res, &tr, top(s));
    *top(s) = res;
    RET(D3D_OK, 4);
}

/* Vtable order must match the public ID3DXMatrixStack declaration exactly:
 * IUnknown, then Pop, Push, LoadIdentity, ..., TranslateLocal, GetTop. A
 * compiled guest calls these by slot, not by import name. */
static const win32hle_shim ms_methods[] = {
    { "ID3DXMatrixStack::QueryInterface", d3d_QueryInterface },
    { "ID3DXMatrixStack::AddRef", ms_AddRef },
    { "ID3DXMatrixStack::Release", ms_Release },
    { "ID3DXMatrixStack::Pop", ms_Pop },
    { "ID3DXMatrixStack::Push", ms_Push },
    { "ID3DXMatrixStack::LoadIdentity", ms_LoadIdentity },
    { "ID3DXMatrixStack::LoadMatrix", ms_LoadMatrix },
    { "ID3DXMatrixStack::MultMatrix", ms_MultMatrix },
    { "ID3DXMatrixStack::MultMatrixLocal", ms_MultMatrixLocal },
    { "ID3DXMatrixStack::RotateAxis", ms_RotateAxis },
    { "ID3DXMatrixStack::RotateAxisLocal", ms_RotateAxisLocal },
    { "ID3DXMatrixStack::RotateYawPitchRoll", ms_RotateYawPitchRoll },
    { "ID3DXMatrixStack::RotateYawPitchRollLocal", ms_RotateYawPitchRollLocal },
    { "ID3DXMatrixStack::Scale", ms_Scale },
    { "ID3DXMatrixStack::ScaleLocal", ms_ScaleLocal },
    { "ID3DXMatrixStack::Translate", ms_Translate },
    { "ID3DXMatrixStack::TranslateLocal", ms_TranslateLocal },
    { "ID3DXMatrixStack::GetTop", ms_GetTop },
    { 0, 0 }
};

void s_D3DXCreateMatrixStack(void) {
    /* D3DXCreateMatrixStack(Flags, ppStack) = 2 args (public D3DX9 ABI). */
    uint32_t flags = A32(0);
    uint32_t pp = A32(1);
    (void)flags;
    if (!pp) { RET(D3DERR_INVALIDCALL, 2); return; }
    if (!g_vt_ms) g_vt_ms = hle_com_vtable(ms_methods);
    uint32_t obj = hle_com_new(g_vt_ms, 8 + sizeof(ms_state_t));
    ms_state_t *s = MS(obj);
    memset(s, 0, sizeof *s);
    s->capacity = 32;
    s->count = 1;
    s->mats = hle_va(hle_alloc(s->capacity * sizeof(d3dx9_matrix)));
    if (!s->mats) { hle_com_free(obj); RET(D3DERR_OUTOFVIDEOMEMORY, 2); return; }
    d3dx9_matrix_identity((d3dx9_matrix *)hle_ptr(s->mats));
    MEM32(pp) = obj;
    RET(D3D_OK, 2);
}