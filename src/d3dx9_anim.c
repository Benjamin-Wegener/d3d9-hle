/*
 * d3dx9_anim.c - D3DXCreateAnimationController + D3DXCreateKeyframedAnimationSet.
 *
 * Public ABI (method order, signatures, struct layouts) is taken ONLY from
 * Wine's public header:
 *   https://raw.githubusercontent.com/wine-mirror/wine/master/include/d3dx9anim.h
 * Behaviour (key interpolation, playback/period, track weights/priorities)
 * follows the documented D3DX semantics as also implemented in Wine's public
 * source:
 *   https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/d3dx9_36/animation.c
 * No Microsoft DLL, no foreign code, no game-specific cases.
 *
 * Time base: keys are in source ticks; positions, periods and AdvanceTime are in
 * seconds (tick = seconds * TicksPerSecond). Tracks start with weight 1, speed 1,
 * LOW priority and disabled. Blending: within a priority group the weighted
 * scale/translation are summed and the rotations are composed from
 * slerp(identity, q, weight); the groups are then lerped/slerped by the priority
 * blend (0 = LOW only, 1 = HIGH only); one group alone is used as is. The
 * controller and every Get* hold references to the sets they hand out.
 * Callbacks (GetCallback, the handler of AdvanceTime) are not supported.
 *
 * Every COM shim has exactly ONE RET with the correct stdcall argc (this
 * included). AddRef/Release edit the refcount directly - calling a helper that
 * RETs would pop another return address and corrupt the guest stack. Unsupported
 * methods still occupy their slot, log and return an honest E_NOTIMPL (or 0 for
 * value-returning methods), never a false D3D_OK.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "win32hle.h"
void d3d_QueryInterface(void);
#include "d3d9_hle.h"
#include "d3dx9_math.h"
#include "d3dx9_anim.h"

#define ANIM_E_NOTIMPL   0x80004001u
#define ANIM_E_OUTOFMEM  0x8007000Eu
#define ANIM_NAME_MAX    64u

/* double/float results leave on the x87 stack (ST0), as on real x86 stdcall. */
#define RETD(v, argc) do { fp_push_impl(g_st, &g_fp_top, (double)(v)); \
                           g_esp += 4u + 4u * (uint32_t)(argc); return; } while (0)

/* ---- guest stack argument helpers ---------------------------------------- */
static float  farg(int i) { uint32_t u = A32(i); float f; memcpy(&f, &u, 4); return f; }
static double darg(int i) {
    uint64_t bits = ((uint64_t)A32(i + 1) << 32) | A32(i);
    double d; memcpy(&d, &bits, 8); return d;
}

/* ---- guest key/vector helpers -------------------------------------------- */
static void rd_v3(uint32_t va, d3dx9_vec3 *v) {
    v->x = MEMF(va); v->y = MEMF(va + 4u); v->z = MEMF(va + 8u);
}
static void wr_v3(uint32_t va, const d3dx9_vec3 *v) {
    if (!va) return;
    MEMF(va) = v->x; MEMF(va + 4u) = v->y; MEMF(va + 8u) = v->z;
}
static void rd_q4(uint32_t va, d3dx9_quat *q) {
    q->x = MEMF(va); q->y = MEMF(va + 4u); q->z = MEMF(va + 8u); q->w = MEMF(va + 12u);
}
static void wr_q4(uint32_t va, const d3dx9_quat *q) {
    if (!va) return;
    MEMF(va) = q->x; MEMF(va + 4u) = q->y; MEMF(va + 8u) = q->z; MEMF(va + 12u) = q->w;
}
static void rd_vkey(uint32_t va, float *t, d3dx9_vec3 *v) {
    *t = MEMF(va); rd_v3(va + 4u, v);
}
static void rd_qkey(uint32_t va, float *t, d3dx9_quat *q) {
    *t = MEMF(va); rd_q4(va + 4u, q);
}
static void wr_vkey(uint32_t va, float t, const d3dx9_vec3 *v) {
    MEMF(va) = t; wr_v3(va + 4u, v);
}
static void wr_qkey(uint32_t va, float t, const d3dx9_quat *q) {
    MEMF(va) = t; wr_q4(va + 4u, q);
}
static void v3_lerp(d3dx9_vec3 *o, const d3dx9_vec3 *a, const d3dx9_vec3 *b, float t) {
    o->x = a->x + (b->x - a->x) * t;
    o->y = a->y + (b->y - a->y) * t;
    o->z = a->z + (b->z - a->z) * t;
}

/* ---- internal data ------------------------------------------------------- */
typedef struct { float time; d3dx9_vec3 v; } a_vkey;
typedef struct { float time; d3dx9_quat q; } a_qkey;

typedef struct {
    char      name[ANIM_NAME_MAX];
    a_vkey   *scale; unsigned ns;
    a_qkey   *rot;   unsigned nr;
    a_vkey   *trans; unsigned nt;
} a_anim;

typedef struct {
    char      name[ANIM_NAME_MAX];
    double    tps;
    int       playback;       /* D3DXPLAY_LOOP / ONCE / PINGPONG */
    unsigned  nanim;
    a_anim   *anims;
    struct { float time; uint32_t data; } *cb;
    unsigned  ncb;
} kfset_t;
#define KFS(o) ((kfset_t *)((char *)hle_ptr(o) + 8u))

typedef struct {
    char     name[ANIM_NAME_MAX];
    uint32_t matrix, scale, rot, trans;   /* guest destination pointers */
} a_output;

typedef struct {
    uint32_t set_obj;         /* VA of the registered animation set, 0 = none */
    int      priority;        /* D3DXPRIORITY_LOW / HIGH */
    float    weight, speed;
    double   position;        /* in the set's time units */
    int      enable;
} a_track;

typedef struct {
    unsigned  max_outputs, max_sets, max_tracks, max_events;
    double    time;
    float     priority_blend;
    a_output *outputs; unsigned n_outputs;
    uint32_t *sets;    unsigned n_sets;
    a_track  *tracks;           /* exactly max_tracks entries */
} ctrl_t;
#define CTRL(o) ((ctrl_t *)((char *)hle_ptr(o) + 8u))

static uint32_t g_vt_kf, g_vt_ctrl;

static void set_name(char *dst, const char *src) {
    if (!src) { dst[0] = 0; return; }
    strncpy(dst, src, ANIM_NAME_MAX - 1u);
    dst[ANIM_NAME_MAX - 1u] = 0;
}

/* ---- interpolation ------------------------------------------------------- */
/* The period in seconds: the last key's time (ticks) / TicksPerSecond. */
static double set_period(const kfset_t *s) {
    double p = 0.0;
    for (unsigned i = 0; i < s->nanim; i++) {
        const a_anim *a = &s->anims[i];
        if (a->ns) p = fmax(p, a->scale[a->ns - 1].time);
        if (a->nr) p = fmax(p, a->rot[a->nr - 1].time);
        if (a->nt) p = fmax(p, a->trans[a->nt - 1].time);
    }
    return s->tps > 0.0 ? p / s->tps : 0.0;
}
static double periodic_pos(const kfset_t *s, double pos) {
    double p = set_period(s);
    if (p <= 0.0) return 0.0;
    if (s->playback == 1) {                       /* D3DXPLAY_ONCE */
        if (pos < 0.0) return 0.0;
        return pos > p ? p : pos;
    }
    if (s->playback == 2) {                       /* D3DXPLAY_PINGPONG */
        double m = fmod(pos, 2.0 * p); if (m < 0.0) m += 2.0 * p;
        return m <= p ? m : 2.0 * p - m;
    }
    {                                             /* D3DXPLAY_LOOP (default) */
        double m = fmod(pos, p); if (m < 0.0) m += p;
        return m;
    }
}
static void interp_v(const a_vkey *k, unsigned n, float t, d3dx9_vec3 *out) {
    if (!n) { out->x = out->y = out->z = 0.0f; return; }
    if (n == 1 || t <= k[0].time) { *out = k[0].v; return; }
    if (t >= k[n - 1].time) { *out = k[n - 1].v; return; }
    for (unsigned i = 1; i < n; i++) {
        if (t <= k[i].time) {
            float span = k[i].time - k[i - 1].time;
            float u = span > 0.0f ? (t - k[i - 1].time) / span : 0.0f;
            v3_lerp(out, &k[i - 1].v, &k[i].v, u);
            return;
        }
    }
    *out = k[n - 1].v;
}
static void interp_q(const a_qkey *k, unsigned n, float t, d3dx9_quat *out) {
    if (!n) { out->x = out->y = out->z = 0.0f; out->w = 1.0f; return; }
    if (n == 1 || t <= k[0].time) { *out = k[0].q; return; }
    if (t >= k[n - 1].time) { *out = k[n - 1].q; return; }
    for (unsigned i = 1; i < n; i++) {
        if (t <= k[i].time) {
            float span = k[i].time - k[i - 1].time;
            float u = span > 0.0f ? (t - k[i - 1].time) / span : 0.0f;
            d3dx9_quat_slerp(out, &k[i - 1].q, &k[i].q, u);
            return;
        }
    }
    *out = k[n - 1].q;
}
/* GetSRT on a keyframed set; `pos` is a periodic position in set time units. */
static uint32_t kf_get_srt(kfset_t *s, double pos, unsigned anim,
                           uint32_t vp, uint32_t qp, uint32_t tp) {
    if (anim >= s->nanim) return D3DERR_INVALIDCALL;
    a_anim *a = &s->anims[anim];
    float t = (float)(pos * s->tps);                     /* seconds -> key ticks */
    d3dx9_vec3 v;
    d3dx9_quat q;
    interp_v(a->scale, a->ns, t, &v); wr_v3(vp, &v);
    interp_q(a->rot,   a->nr, t, &q); wr_q4(qp, &q);
    interp_v(a->trans, a->nt, t, &v); wr_v3(tp, &v);
    return D3D_OK;
}
/* One priority group: weighted sum of scale/translation, composed rotation. */
typedef struct { d3dx9_vec3 s, t; d3dx9_quat q; int any; } a_group;

static void group_init(a_group *g) {
    memset(g, 0, sizeof *g);
    g->q.w = 1.0f;
}

/* Recompute every registered output from the current track state. An output
 * that no enabled track animates (no animation of that name) is left alone. */
static void recompute_outputs(ctrl_t *c) {
    for (unsigned oi = 0; oi < c->n_outputs; oi++) {
        a_output *out = &c->outputs[oi];
        a_group grp[2];
        group_init(&grp[0]); group_init(&grp[1]);
        for (unsigned ti = 0; ti < c->max_tracks; ti++) {
            a_track *tr = &c->tracks[ti];
            if (!tr->enable || !tr->set_obj || tr->weight == 0.0f) continue;
            kfset_t *s = KFS(tr->set_obj);
            unsigned ai = 0;
            for (; ai < s->nanim; ai++)
                if (strcmp(s->anims[ai].name, out->name) == 0) break;
            if (ai >= s->nanim) continue;                 /* no animation of that name */
            float t = (float)(periodic_pos(s, tr->position) * s->tps);
            d3dx9_vec3 v, vt; d3dx9_quat q, step, acc;
            interp_v(s->anims[ai].scale, s->anims[ai].ns, t, &v);
            interp_q(s->anims[ai].rot,   s->anims[ai].nr, t, &q);
            interp_v(s->anims[ai].trans, s->anims[ai].nt, t, &vt);
            a_group *g = &grp[tr->priority ? 1 : 0];
            float w = tr->weight;
            g->s.x += v.x * w;  g->s.y += v.y * w;  g->s.z += v.z * w;
            g->t.x += vt.x * w; g->t.y += vt.y * w; g->t.z += vt.z * w;
            { d3dx9_quat id = {0,0,0,1};
              d3dx9_quat_slerp(&step, &id, &q, w);
              d3dx9_quat_multiply(&acc, &g->q, &step); d3dx9_quat_normalize(&g->q, &acc); }
            g->any = 1;
        }
        if (!grp[0].any && !grp[1].any) continue;
        d3dx9_vec3 vs, ts; d3dx9_quat qs;
        if (grp[0].any && grp[1].any) {                   /* LOW and HIGH: the priority blend */
            v3_lerp(&vs, &grp[0].s, &grp[1].s, c->priority_blend);
            v3_lerp(&ts, &grp[0].t, &grp[1].t, c->priority_blend);
            d3dx9_quat_slerp(&qs, &grp[0].q, &grp[1].q, c->priority_blend);
        } else {
            a_group *g = grp[1].any ? &grp[1] : &grp[0];
            vs = g->s; ts = g->t; qs = g->q;
        }
        wr_v3(out->scale, &vs); wr_q4(out->rot, &qs); wr_v3(out->trans, &ts);
        if (out->matrix) { /* compose row-major SRT matrix into guest memory */
            float m[16] = {0};
            float xx=qs.x+qs.x, yy=qs.y+qs.y, zz=qs.z+qs.z;
            float wx=qs.w*xx, wy=qs.w*yy, wz=qs.w*zz;
            float x2=qs.x*xx, xy=qs.x*yy, xz=qs.x*zz;
            float y2=qs.y*yy, yz=qs.y*zz, z2=qs.z*zz;
            m[0]=(1.0f-(y2+z2))*vs.x; m[1]=(xy+wz)*vs.x;   m[2]=(xz-wy)*vs.x;
            m[4]=(xy-wz)*vs.y;   m[5]=(1.0f-(x2+z2))*vs.y; m[6]=(yz+wx)*vs.y;
            m[8]=(xz+wy)*vs.z;   m[9]=(yz-wx)*vs.z;       m[10]=(1.0f-(x2+y2))*vs.z;
            m[12]=ts.x; m[13]=ts.y; m[14]=ts.z; m[15]=1.0f;
            for (unsigned i = 0; i < 16; i++) MEMF(out->matrix + 4u*i) = m[i];
        }
    }
}

/* Reference counting of the sets the controller holds or hands out. */
static void obj_addref(uint32_t o) { if (o) MEM32(o + 4)++; }
static void obj_release(uint32_t o) {
    if (o) { uint32_t a[1] = { o }; hle_call_guest(MEM32(MEM32(o) + 8u), 1, a); }  /* vtable slot 2 */
}

/* =========================================================================
 * ID3DXKeyframedAnimationSet - slots 0..34
 * ========================================================================= */
#define KF_LOG(n)  fprintf(stderr, "[d3dx9anim] ID3DXKeyframedAnimationSet::%s: unsupported\n", (n))
#define KF_STUB(n, argc)      void n(void) { KF_LOG(#n); RET(ANIM_E_NOTIMPL, argc); }
#define KF_STUB0(n, argc)     void n(void) { KF_LOG(#n); RET(0u, argc); }
#define KF_STUBD(n, argc)     void n(void) { KF_LOG(#n); RETD(0.0, argc); }

void ka_QueryInterface(void) {
    uint32_t self = A32(0), ppv = A32(2);
    uint32_t hr = ppv ? D3D_OK : D3DERR_INVALIDCALL;
    if (ppv) { MEM32(ppv) = self; MEM32(self + 4)++; }
    RET(hr, 3);
}
void ka_AddRef(void) { uint32_t s = A32(0); MEM32(s + 4)++; RET(MEM32(s + 4), 1); }
void ka_Release(void) {
    uint32_t s = A32(0); uint32_t n = --MEM32(s + 4);
    if (!n) { kfset_t *k = KFS(s);
        for (unsigned i = 0; i < k->nanim; i++) { hle_free(k->anims[i].scale); hle_free(k->anims[i].rot); hle_free(k->anims[i].trans); }
        hle_free(k->anims); hle_free(k->cb); hle_com_free(s); }
    RET(n, 1);
}
void ka_GetName(void) { RET(hle_va(KFS(A32(0))->name), 1); }
void ka_GetPeriod(void) { RETD(set_period(KFS(A32(0))), 1); }
void ka_GetPeriodicPosition(void) {
    RETD(periodic_pos(KFS(A32(0)), darg(1)), 3);
}
void ka_GetNumAnimations(void) { RET(KFS(A32(0))->nanim, 1); }
void ka_GetAnimationNameByIndex(void) {
    uint32_t self = A32(0), idx = A32(1), out = A32(2);
    kfset_t *k = KFS(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (out && idx < k->nanim) { MEM32(out) = hle_va(k->anims[idx].name); hr = D3D_OK; }
    RET(hr, 3);
}
void ka_GetAnimationIndexByName(void) {
    uint32_t self = A32(0), name = A32(1), out = A32(2);
    kfset_t *k = KFS(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (out && name) {
        for (unsigned i = 0; i < k->nanim; i++)
            if (strcmp(k->anims[i].name, (const char *)hle_ptr(name)) == 0) {
                MEM32(out) = i; hr = D3D_OK; break;
            }
        if (hr != D3D_OK) MEM32(out) = 0xFFFFFFFFu;
    }
    RET(hr, 3);
}
void ka_GetSRT(void) {
    uint32_t self = A32(0), vp = A32(4), qp = A32(5), tp = A32(6);
    RET(kf_get_srt(KFS(self), darg(1), A32(3), vp, qp, tp), 7);
}
KF_STUB(ka_GetCallback, 6)
void ka_GetPlaybackType(void) { RET((uint32_t)KFS(A32(0))->playback, 1); }
void ka_GetSourceTicksPerSecond(void) { RETD(KFS(A32(0))->tps, 1); }

void ka_GetNumScaleKeys(void) {
    uint32_t self = A32(0); unsigned idx = A32(1); kfset_t *k = KFS(self);
    RET(idx < k->nanim ? k->anims[idx].ns : 0u, 2);
}
void ka_GetScaleKeys(void) {
    uint32_t self = A32(0), idx = A32(1), dst = A32(2); kfset_t *k = KFS(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (dst && idx < k->nanim) {
        a_anim *a = &k->anims[idx];
        for (unsigned i = 0; i < a->ns; i++) wr_vkey(dst + ANIM_VKEY_SIZE*i, a->scale[i].time, &a->scale[i].v);
        hr = D3D_OK;
    }
    RET(hr, 3);
}
void ka_GetScaleKey(void) {
    uint32_t self = A32(0), idx = A32(1), key = A32(2), dst = A32(3); kfset_t *k = KFS(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (dst && idx < k->nanim && key < k->anims[idx].ns) {
        a_vkey *v = &k->anims[idx].scale[key]; wr_vkey(dst, v->time, &v->v); hr = D3D_OK;
    }
    RET(hr, 4);
}
void ka_SetScaleKey(void) {
    uint32_t self = A32(0), idx = A32(1), key = A32(2), src = A32(3); kfset_t *k = KFS(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (src && idx < k->nanim && key < k->anims[idx].ns) {
        rd_vkey(src, &k->anims[idx].scale[key].time, &k->anims[idx].scale[key].v); hr = D3D_OK;
    }
    RET(hr, 4);
}
void ka_GetNumRotationKeys(void) {
    uint32_t self = A32(0); unsigned idx = A32(1); kfset_t *k = KFS(self);
    RET(idx < k->nanim ? k->anims[idx].nr : 0u, 2);
}
void ka_GetRotationKeys(void) {
    uint32_t self = A32(0), idx = A32(1), dst = A32(2); kfset_t *k = KFS(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (dst && idx < k->nanim) {
        a_anim *a = &k->anims[idx];
        for (unsigned i = 0; i < a->nr; i++) wr_qkey(dst + ANIM_QKEY_SIZE*i, a->rot[i].time, &a->rot[i].q);
        hr = D3D_OK;
    }
    RET(hr, 3);
}
void ka_GetRotationKey(void) {
    uint32_t self = A32(0), idx = A32(1), key = A32(2), dst = A32(3); kfset_t *k = KFS(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (dst && idx < k->nanim && key < k->anims[idx].nr) {
        a_qkey *v = &k->anims[idx].rot[key]; wr_qkey(dst, v->time, &v->q); hr = D3D_OK;
    }
    RET(hr, 4);
}
void ka_SetRotationKey(void) {
    uint32_t self = A32(0), idx = A32(1), key = A32(2), src = A32(3); kfset_t *k = KFS(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (src && idx < k->nanim && key < k->anims[idx].nr) {
        rd_qkey(src, &k->anims[idx].rot[key].time, &k->anims[idx].rot[key].q); hr = D3D_OK;
    }
    RET(hr, 4);
}
void ka_GetNumTranslationKeys(void) {
    uint32_t self = A32(0); unsigned idx = A32(1); kfset_t *k = KFS(self);
    RET(idx < k->nanim ? k->anims[idx].nt : 0u, 2);
}
void ka_GetTranslationKeys(void) {
    uint32_t self = A32(0), idx = A32(1), dst = A32(2); kfset_t *k = KFS(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (dst && idx < k->nanim) {
        a_anim *a = &k->anims[idx];
        for (unsigned i = 0; i < a->nt; i++) wr_vkey(dst + ANIM_VKEY_SIZE*i, a->trans[i].time, &a->trans[i].v);
        hr = D3D_OK;
    }
    RET(hr, 3);
}
void ka_GetTranslationKey(void) {
    uint32_t self = A32(0), idx = A32(1), key = A32(2), dst = A32(3); kfset_t *k = KFS(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (dst && idx < k->nanim && key < k->anims[idx].nt) {
        a_vkey *v = &k->anims[idx].trans[key]; wr_vkey(dst, v->time, &v->v); hr = D3D_OK;
    }
    RET(hr, 4);
}
void ka_SetTranslationKey(void) {
    uint32_t self = A32(0), idx = A32(1), key = A32(2), src = A32(3); kfset_t *k = KFS(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (src && idx < k->nanim && key < k->anims[idx].nt) {
        rd_vkey(src, &k->anims[idx].trans[key].time, &k->anims[idx].trans[key].v); hr = D3D_OK;
    }
    RET(hr, 4);
}
void ka_GetNumCallbackKeys(void) { RET(KFS(A32(0))->ncb, 1); }
void ka_GetCallbackKeys(void) {
    uint32_t self = A32(0), dst = A32(1); kfset_t *k = KFS(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (dst) {
        for (unsigned i = 0; i < k->ncb; i++) {
            MEMF(dst + ANIM_CKEY_SIZE*i) = k->cb[i].time;
            MEM32(dst + ANIM_CKEY_SIZE*i + 4u) = k->cb[i].data;
        }
        hr = D3D_OK;
    }
    RET(hr, 2);
}
void ka_GetCallbackKey(void) {
    uint32_t self = A32(0), key = A32(1), dst = A32(2); kfset_t *k = KFS(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (dst && key < k->ncb) {
        MEMF(dst) = k->cb[key].time; MEM32(dst + 4u) = k->cb[key].data; hr = D3D_OK;
    }
    RET(hr, 3);
}
void ka_SetCallbackKey(void) {
    uint32_t self = A32(0), key = A32(1), src = A32(2); kfset_t *k = KFS(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (src && key < k->ncb) {
        k->cb[key].time = MEMF(src); k->cb[key].data = MEM32(src + 4u); hr = D3D_OK;
    }
    RET(hr, 3);
}
/* Unregistering a key removes it and shifts the following keys down. */
void ka_UnregisterScaleKey(void) {
    uint32_t self = A32(0), idx = A32(1), key = A32(2); kfset_t *k = KFS(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (idx < k->nanim && key < k->anims[idx].ns) {
        a_vkey *a = k->anims[idx].scale; unsigned n = k->anims[idx].ns;
        memmove(&a[key], &a[key+1], (n - key - 1) * sizeof *a); k->anims[idx].ns--; hr = D3D_OK;
    }
    RET(hr, 3);
}
void ka_UnregisterRotationKey(void) {
    uint32_t self = A32(0), idx = A32(1), key = A32(2); kfset_t *k = KFS(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (idx < k->nanim && key < k->anims[idx].nr) {
        a_qkey *a = k->anims[idx].rot; unsigned n = k->anims[idx].nr;
        memmove(&a[key], &a[key+1], (n - key - 1) * sizeof *a); k->anims[idx].nr--; hr = D3D_OK;
    }
    RET(hr, 3);
}
void ka_UnregisterTranslationKey(void) {
    uint32_t self = A32(0), idx = A32(1), key = A32(2); kfset_t *k = KFS(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (idx < k->nanim && key < k->anims[idx].nt) {
        a_vkey *a = k->anims[idx].trans; unsigned n = k->anims[idx].nt;
        memmove(&a[key], &a[key+1], (n - key - 1) * sizeof *a); k->anims[idx].nt--; hr = D3D_OK;
    }
    RET(hr, 3);
}
void ka_RegisterAnimationSRTKeys(void) {
    uint32_t self = A32(0), name = A32(1);
    unsigned ns = A32(2), nr = A32(3), nt = A32(4);
    uint32_t sk = A32(5), rk = A32(6), tk = A32(7), out = A32(8);
    kfset_t *k = KFS(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if ((ns && !sk) || (nr && !rk) || (nt && !tk)) RET(D3DERR_INVALIDCALL, 9);
    a_anim *na = (a_anim *)hle_alloc(sizeof *na);
    if (!na) RET(ANIM_E_OUTOFMEM, 9);
    memset(na, 0, sizeof *na);
    set_name(na->name, (const char *)hle_ptr(name));
    if (ns) { na->scale = (a_vkey *)hle_alloc(ns * sizeof(a_vkey));
        if (!na->scale) { hle_free(na); RET(ANIM_E_OUTOFMEM, 9); }
        for (unsigned i = 0; i < ns; i++) rd_vkey(sk + ANIM_VKEY_SIZE*i, &na->scale[i].time, &na->scale[i].v);
        na->ns = ns; }
    if (nr) { na->rot = (a_qkey *)hle_alloc(nr * sizeof(a_qkey));
        if (!na->rot) { hle_free(na->scale); hle_free(na); RET(ANIM_E_OUTOFMEM, 9); }
        for (unsigned i = 0; i < nr; i++) rd_qkey(rk + ANIM_QKEY_SIZE*i, &na->rot[i].time, &na->rot[i].q);
        na->nr = nr; }
    if (nt) { na->trans = (a_vkey *)hle_alloc(nt * sizeof(a_vkey));
        if (!na->trans) { hle_free(na->rot); hle_free(na->scale); hle_free(na); RET(ANIM_E_OUTOFMEM, 9); }
        for (unsigned i = 0; i < nt; i++) rd_vkey(tk + ANIM_VKEY_SIZE*i, &na->trans[i].time, &na->trans[i].v);
        na->nt = nt; }
    a_anim *grown = (a_anim *)hle_alloc((k->nanim + 1) * sizeof(a_anim));
    if (!grown) { hle_free(na->trans); hle_free(na->rot); hle_free(na->scale); hle_free(na); RET(ANIM_E_OUTOFMEM, 9); }
    if (k->nanim) memcpy(grown, k->anims, k->nanim * sizeof(a_anim));
    grown[k->nanim] = *na; hle_free(na); hle_free(k->anims); k->anims = grown;
    if (out) MEM32(out) = k->nanim;
    k->nanim++; hr = D3D_OK;
    RET(hr, 9);
}
KF_STUB(ka_Compress, 5)
void ka_UnregisterAnimation(void) {
    uint32_t self = A32(0), idx = A32(1); kfset_t *k = KFS(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (idx < k->nanim) {
        a_anim *a = &k->anims[idx];
        hle_free(a->scale); hle_free(a->rot); hle_free(a->trans);
        memmove(a, a + 1, (k->nanim - idx - 1) * sizeof(a_anim));
        k->nanim--; hr = D3D_OK;
    }
    RET(hr, 2);
}

/* =========================================================================
 * ID3DXAnimationController - slots 0..41
 * ========================================================================= */
#define AC_LOG(n)  fprintf(stderr, "[d3dx9anim] ID3DXAnimationController::%s: unsupported\n", (n))
#define AC_STUB(n, argc)      void n(void) { AC_LOG(#n); RET(ANIM_E_NOTIMPL, argc); }
#define AC_STUB0(n, argc)     void n(void) { AC_LOG(#n); RET(0u, argc); }
#define AC_STUBD(n, argc)     void n(void) { AC_LOG(#n); RETD(0.0, argc); }

void ac_QueryInterface(void) {
    uint32_t self = A32(0), ppv = A32(2);
    uint32_t hr = ppv ? D3D_OK : D3DERR_INVALIDCALL;
    if (ppv) { MEM32(ppv) = self; MEM32(self + 4)++; }
    RET(hr, 3);
}
void ac_AddRef(void) { uint32_t s = A32(0); MEM32(s + 4)++; RET(MEM32(s + 4), 1); }
void ac_Release(void) {
    uint32_t s = A32(0); uint32_t n = --MEM32(s + 4);
    if (!n) { ctrl_t *c = CTRL(s);
        for (unsigned i = 0; i < c->max_tracks; i++) obj_release(c->tracks[i].set_obj);
        for (unsigned i = 0; i < c->n_sets; i++) obj_release(c->sets[i]);
        hle_free(c->outputs); hle_free(c->sets); hle_free(c->tracks); hle_com_free(s); }
    RET(n, 1);
}
void ac_GetMaxNumAnimationOutputs(void) { RET(CTRL(A32(0))->max_outputs, 1); }
void ac_GetMaxNumAnimationSets(void)    { RET(CTRL(A32(0))->max_sets, 1); }
void ac_GetMaxNumTracks(void)           { RET(CTRL(A32(0))->max_tracks, 1); }
void ac_GetMaxNumEvents(void)           { RET(CTRL(A32(0))->max_events, 1); }
void ac_RegisterAnimationOutput(void) {
    uint32_t self = A32(0), name = A32(1); ctrl_t *c = CTRL(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (name && c->n_outputs < c->max_outputs) {
        a_output *o = &c->outputs[c->n_outputs++];
        set_name(o->name, (const char *)hle_ptr(name));
        o->matrix = A32(2); o->scale = A32(3); o->rot = A32(4); o->trans = A32(5);
        hr = D3D_OK;
    }
    RET(hr, 6);
}
void ac_RegisterAnimationSet(void) {
    uint32_t self = A32(0), set = A32(1); ctrl_t *c = CTRL(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (set && c->n_sets < c->max_sets) {
        for (unsigned i = 0; i < c->n_sets; i++) if (c->sets[i] == set) RET(D3D_OK, 2);
        c->sets[c->n_sets++] = set; obj_addref(set); hr = D3D_OK;
    }
    RET(hr, 2);
}
void ac_UnregisterAnimationSet(void) {
    uint32_t self = A32(0), set = A32(1); ctrl_t *c = CTRL(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    for (unsigned i = 0; i < c->n_sets; i++) if (c->sets[i] == set) {
        for (unsigned t = 0; t < c->max_tracks; t++)       /* tracks playing it are cleared */
            if (c->tracks[t].set_obj == set) { c->tracks[t].set_obj = 0; obj_release(set); }
        memmove(&c->sets[i], &c->sets[i+1], (c->n_sets - i - 1) * sizeof(uint32_t)); c->n_sets--;
        obj_release(set);                                  /* the registration's reference */
        hr = D3D_OK; break;
    }
    RET(hr, 2);
}
void ac_GetNumAnimationSets(void) { RET(CTRL(A32(0))->n_sets, 1); }
void ac_GetAnimationSet(void) {
    uint32_t self = A32(0), idx = A32(1), out = A32(2); ctrl_t *c = CTRL(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (out && idx < c->n_sets) { MEM32(out) = c->sets[idx]; obj_addref(c->sets[idx]); hr = D3D_OK; }
    RET(hr, 3);
}
void ac_GetAnimationSetByName(void) {
    uint32_t self = A32(0), name = A32(1), out = A32(2); ctrl_t *c = CTRL(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (out && name) {
        for (unsigned i = 0; i < c->n_sets; i++) {
            kfset_t *s = KFS(c->sets[i]);
            if (strcmp(s->name, (const char *)hle_ptr(name)) == 0) { MEM32(out) = c->sets[i]; obj_addref(c->sets[i]); hr = D3D_OK; break; }
        }
    }
    RET(hr, 3);
}
void ac_AdvanceTime(void) {   /* (this, double TimeDelta, pCallbackHandler) */
    uint32_t self = A32(0), handler = A32(3), hr = D3D_OK; double dt = darg(1); ctrl_t *c = CTRL(self);
    int callbacks = 0;
    c->time += dt;
    for (unsigned i = 0; i < c->max_tracks; i++) {
        a_track *t = &c->tracks[i];
        if (t->set_obj && t->enable) {
            kfset_t *s = KFS(t->set_obj);
            t->position += dt * (double)t->speed;         /* seconds */
            if (s->ncb) callbacks = 1;
        }
    }
    recompute_outputs(c);
    if (handler && callbacks) {
        fprintf(stderr, "[d3dx9anim] ID3DXAnimationController::AdvanceTime: callback keys are not delivered\n");
        hr = D3DERR_NOTAVAILABLE;
    }
    RET(hr, 4);
}
void ac_ResetTime(void) {
    ctrl_t *c = CTRL(A32(0)); c->time = 0.0;
    for (unsigned i = 0; i < c->max_tracks; i++) c->tracks[i].position = 0.0;
    recompute_outputs(c); RET(D3D_OK, 1);
}
void ac_GetTime(void) { RETD(CTRL(A32(0))->time, 1); }
void ac_SetTrackAnimationSet(void) {
    uint32_t self = A32(0), tr = A32(1), set = A32(2); ctrl_t *c = CTRL(self);
    uint32_t hr = tr < c->max_tracks ? D3D_OK : D3DERR_INVALIDCALL;
    if (hr == D3D_OK && c->tracks[tr].set_obj != set) {
        uint32_t old = c->tracks[tr].set_obj;
        obj_addref(set);
        c->tracks[tr].set_obj = set;
        obj_release(old);
    }
    RET(hr, 3);
}
void ac_GetTrackAnimationSet(void) {
    uint32_t self = A32(0), tr = A32(1), out = A32(2); ctrl_t *c = CTRL(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (out && tr < c->max_tracks) { MEM32(out) = c->tracks[tr].set_obj; obj_addref(c->tracks[tr].set_obj); hr = D3D_OK; }
    RET(hr, 3);
}
void ac_SetTrackPriority(void) {
    uint32_t self = A32(0), tr = A32(1); ctrl_t *c = CTRL(self);
    uint32_t hr = tr < c->max_tracks ? D3D_OK : D3DERR_INVALIDCALL;
    if (hr == D3D_OK) c->tracks[tr].priority = (int)A32(2);
    RET(hr, 3);
}
void ac_SetTrackSpeed(void) {
    uint32_t self = A32(0), tr = A32(1); ctrl_t *c = CTRL(self);
    uint32_t hr = tr < c->max_tracks ? D3D_OK : D3DERR_INVALIDCALL;
    if (hr == D3D_OK) c->tracks[tr].speed = farg(2);
    RET(hr, 3);
}
void ac_SetTrackWeight(void) {
    uint32_t self = A32(0), tr = A32(1); ctrl_t *c = CTRL(self);
    uint32_t hr = tr < c->max_tracks ? D3D_OK : D3DERR_INVALIDCALL;
    if (hr == D3D_OK) c->tracks[tr].weight = farg(2);
    RET(hr, 3);
}
void ac_SetTrackPosition(void) {
    uint32_t self = A32(0), tr = A32(1); ctrl_t *c = CTRL(self);
    uint32_t hr = tr < c->max_tracks ? D3D_OK : D3DERR_INVALIDCALL;
    if (hr == D3D_OK) c->tracks[tr].position = darg(2);
    RET(hr, 4);
}
void ac_SetTrackEnable(void) {
    uint32_t self = A32(0), tr = A32(1); ctrl_t *c = CTRL(self);
    uint32_t hr = tr < c->max_tracks ? D3D_OK : D3DERR_INVALIDCALL;
    if (hr == D3D_OK) c->tracks[tr].enable = (int)A32(2);
    RET(hr, 3);
}
void ac_SetTrackDesc(void) {
    uint32_t self = A32(0), tr = A32(1), desc = A32(2); ctrl_t *c = CTRL(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (desc && tr < c->max_tracks) {
        a_track *t = &c->tracks[tr];
        t->priority = (int)MEM32(desc);
        t->weight = MEMF(desc + 4u); t->speed = MEMF(desc + 8u);
        t->position = *(double *)(hle_ptr(desc + 16u));
        t->enable = (int)MEM32(desc + 24u); hr = D3D_OK;
    }
    RET(hr, 3);
}
void ac_GetTrackDesc(void) {
    uint32_t self = A32(0), tr = A32(1), desc = A32(2); ctrl_t *c = CTRL(self);
    uint32_t hr = D3DERR_INVALIDCALL;
    if (desc && tr < c->max_tracks) {
        a_track *t = &c->tracks[tr];
        MEM32(desc) = (uint32_t)t->priority; MEMF(desc + 4u) = t->weight; MEMF(desc + 8u) = t->speed;
        *(double *)(hle_ptr(desc + 16u)) = t->position; MEM32(desc + 24u) = (uint32_t)t->enable;
        hr = D3D_OK;
    }
    RET(hr, 3);
}
void ac_SetPriorityBlend(void) { CTRL(A32(0))->priority_blend = farg(1); RET(D3D_OK, 2); }
void ac_GetPriorityBlend(void) { RETD(CTRL(A32(0))->priority_blend, 1); }

AC_STUB(ac_KeyTrackSpeed, 8)
AC_STUB(ac_KeyTrackWeight, 8)
AC_STUB(ac_KeyTrackPosition, 6)
AC_STUB(ac_KeyTrackEnable, 5)
AC_STUB(ac_KeyPriorityBlend, 7)
AC_STUB(ac_UnkeyEvent, 2)
AC_STUB(ac_UnkeyAllTrackEvents, 2)
AC_STUB0(ac_UnkeyAllPriorityBlends, 1)
AC_STUB0(ac_GetCurrentTrackEvent, 3)
AC_STUB0(ac_GetCurrentPriorityBlend, 1)
AC_STUB0(ac_GetUpcomingTrackEvent, 3)
AC_STUB0(ac_GetUpcomingPriorityBlend, 2)
AC_STUB(ac_ValidateEvent, 2)
AC_STUB(ac_GetEventDesc, 3)
AC_STUB(ac_CloneAnimationController, 6)

/* =========================================================================
 * Vtables + create shims
 * ========================================================================= */
static const win32hle_shim kf_methods[] = {
    { "ID3DXKeyframedAnimationSet::QueryInterface", d3d_QueryInterface },
    { "ID3DXKeyframedAnimationSet::AddRef", ka_AddRef },
    { "ID3DXKeyframedAnimationSet::Release", ka_Release },
    { "ID3DXKeyframedAnimationSet::GetName", ka_GetName },
    { "ID3DXKeyframedAnimationSet::GetPeriod", ka_GetPeriod },
    { "ID3DXKeyframedAnimationSet::GetPeriodicPosition", ka_GetPeriodicPosition },
    { "ID3DXKeyframedAnimationSet::GetNumAnimations", ka_GetNumAnimations },
    { "ID3DXKeyframedAnimationSet::GetAnimationNameByIndex", ka_GetAnimationNameByIndex },
    { "ID3DXKeyframedAnimationSet::GetAnimationIndexByName", ka_GetAnimationIndexByName },
    { "ID3DXKeyframedAnimationSet::GetSRT", ka_GetSRT },
    { "ID3DXKeyframedAnimationSet::GetCallback", ka_GetCallback },
    { "ID3DXKeyframedAnimationSet::GetPlaybackType", ka_GetPlaybackType },
    { "ID3DXKeyframedAnimationSet::GetSourceTicksPerSecond", ka_GetSourceTicksPerSecond },
    { "ID3DXKeyframedAnimationSet::GetNumScaleKeys", ka_GetNumScaleKeys },
    { "ID3DXKeyframedAnimationSet::GetScaleKeys", ka_GetScaleKeys },
    { "ID3DXKeyframedAnimationSet::GetScaleKey", ka_GetScaleKey },
    { "ID3DXKeyframedAnimationSet::SetScaleKey", ka_SetScaleKey },
    { "ID3DXKeyframedAnimationSet::GetNumRotationKeys", ka_GetNumRotationKeys },
    { "ID3DXKeyframedAnimationSet::GetRotationKeys", ka_GetRotationKeys },
    { "ID3DXKeyframedAnimationSet::GetRotationKey", ka_GetRotationKey },
    { "ID3DXKeyframedAnimationSet::SetRotationKey", ka_SetRotationKey },
    { "ID3DXKeyframedAnimationSet::GetNumTranslationKeys", ka_GetNumTranslationKeys },
    { "ID3DXKeyframedAnimationSet::GetTranslationKeys", ka_GetTranslationKeys },
    { "ID3DXKeyframedAnimationSet::GetTranslationKey", ka_GetTranslationKey },
    { "ID3DXKeyframedAnimationSet::SetTranslationKey", ka_SetTranslationKey },
    { "ID3DXKeyframedAnimationSet::GetNumCallbackKeys", ka_GetNumCallbackKeys },
    { "ID3DXKeyframedAnimationSet::GetCallbackKeys", ka_GetCallbackKeys },
    { "ID3DXKeyframedAnimationSet::GetCallbackKey", ka_GetCallbackKey },
    { "ID3DXKeyframedAnimationSet::SetCallbackKey", ka_SetCallbackKey },
    { "ID3DXKeyframedAnimationSet::UnregisterScaleKey", ka_UnregisterScaleKey },
    { "ID3DXKeyframedAnimationSet::UnregisterRotationKey", ka_UnregisterRotationKey },
    { "ID3DXKeyframedAnimationSet::UnregisterTranslationKey", ka_UnregisterTranslationKey },
    { "ID3DXKeyframedAnimationSet::RegisterAnimationSRTKeys", ka_RegisterAnimationSRTKeys },
    { "ID3DXKeyframedAnimationSet::Compress", ka_Compress },
    { "ID3DXKeyframedAnimationSet::UnregisterAnimation", ka_UnregisterAnimation },
    { 0, 0 }
};
static const win32hle_shim ctrl_methods[] = {
    { "ID3DXAnimationController::QueryInterface", d3d_QueryInterface },
    { "ID3DXAnimationController::AddRef", ac_AddRef },
    { "ID3DXAnimationController::Release", ac_Release },
    { "ID3DXAnimationController::GetMaxNumAnimationOutputs", ac_GetMaxNumAnimationOutputs },
    { "ID3DXAnimationController::GetMaxNumAnimationSets", ac_GetMaxNumAnimationSets },
    { "ID3DXAnimationController::GetMaxNumTracks", ac_GetMaxNumTracks },
    { "ID3DXAnimationController::GetMaxNumEvents", ac_GetMaxNumEvents },
    { "ID3DXAnimationController::RegisterAnimationOutput", ac_RegisterAnimationOutput },
    { "ID3DXAnimationController::RegisterAnimationSet", ac_RegisterAnimationSet },
    { "ID3DXAnimationController::UnregisterAnimationSet", ac_UnregisterAnimationSet },
    { "ID3DXAnimationController::GetNumAnimationSets", ac_GetNumAnimationSets },
    { "ID3DXAnimationController::GetAnimationSet", ac_GetAnimationSet },
    { "ID3DXAnimationController::GetAnimationSetByName", ac_GetAnimationSetByName },
    { "ID3DXAnimationController::AdvanceTime", ac_AdvanceTime },
    { "ID3DXAnimationController::ResetTime", ac_ResetTime },
    { "ID3DXAnimationController::GetTime", ac_GetTime },
    { "ID3DXAnimationController::SetTrackAnimationSet", ac_SetTrackAnimationSet },
    { "ID3DXAnimationController::GetTrackAnimationSet", ac_GetTrackAnimationSet },
    { "ID3DXAnimationController::SetTrackPriority", ac_SetTrackPriority },
    { "ID3DXAnimationController::SetTrackSpeed", ac_SetTrackSpeed },
    { "ID3DXAnimationController::SetTrackWeight", ac_SetTrackWeight },
    { "ID3DXAnimationController::SetTrackPosition", ac_SetTrackPosition },
    { "ID3DXAnimationController::SetTrackEnable", ac_SetTrackEnable },
    { "ID3DXAnimationController::SetTrackDesc", ac_SetTrackDesc },
    { "ID3DXAnimationController::GetTrackDesc", ac_GetTrackDesc },
    { "ID3DXAnimationController::SetPriorityBlend", ac_SetPriorityBlend },
    { "ID3DXAnimationController::GetPriorityBlend", ac_GetPriorityBlend },
    { "ID3DXAnimationController::KeyTrackSpeed", ac_KeyTrackSpeed },
    { "ID3DXAnimationController::KeyTrackWeight", ac_KeyTrackWeight },
    { "ID3DXAnimationController::KeyTrackPosition", ac_KeyTrackPosition },
    { "ID3DXAnimationController::KeyTrackEnable", ac_KeyTrackEnable },
    { "ID3DXAnimationController::KeyPriorityBlend", ac_KeyPriorityBlend },
    { "ID3DXAnimationController::UnkeyEvent", ac_UnkeyEvent },
    { "ID3DXAnimationController::UnkeyAllTrackEvents", ac_UnkeyAllTrackEvents },
    { "ID3DXAnimationController::UnkeyAllPriorityBlends", ac_UnkeyAllPriorityBlends },
    { "ID3DXAnimationController::GetCurrentTrackEvent", ac_GetCurrentTrackEvent },
    { "ID3DXAnimationController::GetCurrentPriorityBlend", ac_GetCurrentPriorityBlend },
    { "ID3DXAnimationController::GetUpcomingTrackEvent", ac_GetUpcomingTrackEvent },
    { "ID3DXAnimationController::GetUpcomingPriorityBlend", ac_GetUpcomingPriorityBlend },
    { "ID3DXAnimationController::ValidateEvent", ac_ValidateEvent },
    { "ID3DXAnimationController::GetEventDesc", ac_GetEventDesc },
    { "ID3DXAnimationController::CloneAnimationController", ac_CloneAnimationController },
    { 0, 0 }
};

void s_D3DXCreateKeyframedAnimationSet(void) {
    /* (name, ticks_per_second[2], playback, animation_count, callback_key_count,
     *  callback_keys, out) = 1+2+1+1+1+1+1 = 8 stack arguments. */
    uint32_t name = A32(0), out = A32(7);
    uint32_t hr = (out && darg(1) > 0.0) ? D3D_OK : D3DERR_INVALIDCALL;
    if (hr == D3D_OK) {
        kfset_t *k; uint32_t obj;
        if (!g_vt_kf) g_vt_kf = hle_com_vtable(kf_methods);
        obj = hle_com_new(g_vt_kf, 8u + sizeof(kfset_t));
        if (!obj) hr = ANIM_E_OUTOFMEM;
        else {
            unsigned ncb = A32(5), cbva = A32(6);   /* after name,tps(2),playback,anim_count */
            k = KFS(obj); memset(k, 0, sizeof *k);
            set_name(k->name, (const char *)hle_ptr(name));
            k->tps = darg(1); k->playback = (int)A32(3);
            k->ncb = ncb;
            if (ncb) {
                k->cb = (void *)hle_alloc(ncb * 8u);
                if (!k->cb) { hle_com_free(obj); RET(ANIM_E_OUTOFMEM, 8); }
                for (unsigned i = 0; i < ncb; i++) {
                    k->cb[i].time = MEMF(cbva + 8u*i);
                    k->cb[i].data = MEM32(cbva + 8u*i + 4u);
                }
            }
            MEM32(out) = obj;
        }
    }
    RET(hr, 8);
}
void s_D3DXCreateAnimationController(void) {
    /* (max_outputs, max_sets, max_tracks, max_events, out) = 5 args. */
    uint32_t mo = A32(0), ms = A32(1), mt = A32(2), me = A32(3), out = A32(4);
    uint32_t hr = out ? D3D_OK : D3DERR_INVALIDCALL;
    if (hr == D3D_OK) {
        if (!mo || !ms || !mt) RET(D3DERR_INVALIDCALL, 5);
        uint32_t obj; ctrl_t *c;
        if (!g_vt_ctrl) g_vt_ctrl = hle_com_vtable(ctrl_methods);
        obj = hle_com_new(g_vt_ctrl, 8u + sizeof(ctrl_t));
        if (!obj) hr = ANIM_E_OUTOFMEM;
        else {
            c = CTRL(obj); memset(c, 0, sizeof *c);
            c->max_outputs = mo; c->max_sets = ms; c->max_tracks = mt; c->max_events = me;
            c->priority_blend = 0.5f;
            c->outputs = (a_output *)hle_alloc(mo * sizeof(a_output));
            c->sets = (uint32_t *)hle_alloc(ms * sizeof(uint32_t));
            c->tracks = (a_track *)hle_alloc(mt * sizeof(a_track));
            if (!c->outputs || !c->sets || !c->tracks) {
                hle_free(c->outputs); hle_free(c->sets); hle_free(c->tracks); hle_com_free(obj);
                RET(ANIM_E_OUTOFMEM, 5);
            }
            memset(c->outputs, 0, mo * sizeof(a_output));
            memset(c->sets, 0, ms * sizeof(uint32_t));
            memset(c->tracks, 0, mt * sizeof(a_track));
            for (unsigned i = 0; i < mt; i++) { c->tracks[i].weight = 1.0f; c->tracks[i].speed = 1.0f; }   /* LOW, disabled */
            MEM32(out) = obj;
        }
    }
    RET(hr, 5);
}
