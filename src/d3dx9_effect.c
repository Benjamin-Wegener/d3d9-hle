/*
 * d3dx9_effect.c - first step of the compiled D3DX9 effect framework.
 *
 * ID3DXEffectPool is the shared parameter store every effect built from the
 * same compiled data writes into. It needs neither a d3d9 device nor the D3DX
 * math module, so it can be built and tested on its own now; technique/pass
 * evaluation and the fx_2_0 binary parser come in later steps.
 *
 * The object is a guest-side COM object (hle_com_new): [0] vtable, [4] refcount,
 * [8..] the parameter table below. All state lives in guest memory. Every
 * method is stdcall with `this` as its first argument and purges its own stack.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "win32hle.h"
void d3d_QueryInterface(void);
#include "d3dx9_effect_hle.h"
#include "d3dx9_fxvtbl.h"

#define D3D_OK              0u
#define D3DERR_INVALIDCALL  0x8876086Cu
#define MAX_PARAMS  16
#define NAME_LEN    64

enum { PT_FLOAT = 1, PT_INT, PT_BOOL, PT_VECTOR, PT_MATRIX, PT_TEXTURE };

typedef struct {
    char name[NAME_LEN];
    uint32_t type;
    float f[16];                 /* float/int/bool/vector/matrix (ints in f[0]'s bits) */
    uint32_t tex;                /* a guest texture handle */
    uint32_t stage;              /* sampler stage for a texture/sampler parameter */
} param_t;
typedef struct { uint32_t n; param_t p[MAX_PARAMS]; } pool_t;

#define POOL(o) ((pool_t *)((char *)hle_ptr(o) + 8))

static uint32_t g_vt;
static uint32_t g_vt_effect;

static param_t *find(pool_t *pool, const char *name, int create) {
    if (!name) return 0;
    for (uint32_t i = 0; i < pool->n; i++)
        if (!strcmp(pool->p[i].name, name)) return &pool->p[i];
    if (create && pool->n < MAX_PARAMS) {
        param_t *p = &pool->p[pool->n++];
        snprintf(p->name, NAME_LEN, "%s", name);
        return p;
    }
    return 0;
}

/* The value's first component is shared by float/int/bool; vector uses four,
 * matrix sixteen. f32(a32) reinterprets a by-value float argument. */
static float f32(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

static uint32_t set_param(uint32_t this, uint32_t name, uint32_t type, uint32_t src, uint32_t count) {
    pool_t *pool = POOL(this);
    param_t *p = find(pool, (const char *)hle_ptr(name), 1);
    if (!p) return D3DERR_INVALIDCALL;
    p->type = type;
    memset(p->f, 0, sizeof p->f);
    if (type == PT_TEXTURE) p->tex = src;
    else if (src) memcpy(p->f, hle_ptr(src), 4 * (size_t)count);
    return D3D_OK;
}
static uint32_t get_param(uint32_t this, uint32_t name, uint32_t type, uint32_t dst, uint32_t count) {
    pool_t *pool = POOL(this);
    param_t *p = find(pool, (const char *)hle_ptr(name), 0);
    if (!p || p->type != type) return D3DERR_INVALIDCALL;
    if (type == PT_TEXTURE) { if (dst) MEM32(dst) = p->tex; }
    else if (dst) memcpy(hle_ptr(dst), p->f, 4 * (size_t)count);
    return D3D_OK;
}

/* ---- ID3DXEffectPool methods (stdcall) ----
 * The scalar setters read their by-value argument directly; a vector/matrix is
 * a guest pointer and goes through the shared copy helper. */
static void p_set_float(void) {
    uint32_t this = A32(0), bits = A32(2);
    pool_t *pool = POOL(this);
    param_t *p = find(pool, (const char *)hle_ptr(A32(1)), 1);
    if (!p) RET(D3DERR_INVALIDCALL, 3);
    p->type = PT_FLOAT, p->f[0] = f32(bits);
    RET(D3D_OK, 3);
}
static void p_get_float(void) { RET(get_param(A32(0), A32(1), PT_FLOAT, A32(2), 1), 3); }
static void p_set_int(void) {
    uint32_t this = A32(0), v = A32(2);
    pool_t *pool = POOL(this);
    param_t *p = find(pool, (const char *)hle_ptr(A32(1)), 1);
    if (!p) RET(D3DERR_INVALIDCALL, 3);
    p->type = PT_INT, p->f[0] = f32(v);
    RET(D3D_OK, 3);
}
static void p_get_int(void) { RET(get_param(A32(0), A32(1), PT_INT, A32(2), 1), 3); }
static void p_set_bool(void) {
    uint32_t this = A32(0), v = A32(2);
    pool_t *pool = POOL(this);
    param_t *p = find(pool, (const char *)hle_ptr(A32(1)), 1);
    if (!p) RET(D3DERR_INVALIDCALL, 3);
    p->type = PT_BOOL, p->f[0] = v ? 1.0f : 0.0f;
    RET(D3D_OK, 3);
}
static void p_get_bool(void) { RET(get_param(A32(0), A32(1), PT_BOOL, A32(2), 1), 3); }
static void p_set_vector(void) { RET(set_param(A32(0), A32(1), PT_VECTOR, A32(2), 4), 3); }
static void p_get_vector(void) { RET(get_param(A32(0), A32(1), PT_VECTOR, A32(2), 4), 3); }
static void p_set_matrix(void) { RET(set_param(A32(0), A32(1), PT_MATRIX, A32(2), 16), 3); }
static void p_get_matrix(void) { RET(get_param(A32(0), A32(1), PT_MATRIX, A32(2), 16), 3); }
static void p_set_texture(void) { RET(set_param(A32(0), A32(1), PT_TEXTURE, A32(2), 0), 3); }
static void p_get_texture(void) { RET(get_param(A32(0), A32(1), PT_TEXTURE, A32(2), 0), 3); }

/* The IUnknown methods are the shared COM helpers; the prefixed names keep
 * them in this vtable without polluting the global shim namespace. */
static const win32hle_shim pool_methods[] = {
    { "ID3DXEffectPool::QueryInterface", d3d_QueryInterface },
    { "ID3DXEffectPool::AddRef", hle_com_AddRef },
    { "ID3DXEffectPool::Release", hle_com_Release },
    { 0, 0 }
};



static void f_create_pool(void) {                       /* D3DXCreateEffectPool(ppPool): 1 arg */
    uint32_t out = A32(0);
    if (!out) RET(D3DERR_INVALIDCALL, 1);
    if (!g_vt) g_vt = hle_com_vtable(pool_methods);
    MEM32(out) = hle_com_new(g_vt, 8 + sizeof(pool_t));
    RET(MEM32(out) ? D3D_OK : 0x8007000Eu /* E_OUTOFMEMORY */, 1);
}

uint32_t hle_d3d9fx_pool_values(uint32_t pool) {
    pool_t *p = pool ? POOL(pool) : 0;
    return p ? p->n : 0;
}
uint32_t hle_d3d9fx_pool_param(uint32_t pool, const char *name, int create) {
    pool_t *p = pool ? POOL(pool) : 0;
    param_t *v = p ? find(p, name, create) : 0;
    return v ? hle_va(v) : 0;
}
uint32_t hle_d3d9fx_pool_new(void) {
    if (!g_vt) g_vt = hle_com_vtable(pool_methods);
    return hle_com_new(g_vt, 8 + sizeof(pool_t));
}
/* The public ID3DXEffectPool interface is IUnknown-only (Wine d3dx9effect.h).
 * The .fx source loader writes initial parameter values through these host
 * helpers instead of calling non-public vtable slots. */
uint32_t hle_d3d9fx_param_set_float(uint32_t handle, float value) {
    param_t *p = (param_t *)hle_ptr(handle);
    if (!p) return D3DERR_INVALIDCALL;
    p->type = PT_FLOAT, p->f[0] = value;
    return D3D_OK;
}
uint32_t hle_d3d9fx_param_set_vector(uint32_t handle, const float *value) {
    param_t *p = (param_t *)hle_ptr(handle);
    if (!p || !value) return D3DERR_INVALIDCALL;
    p->type = PT_VECTOR;
    memcpy(p->f, value, 4 * sizeof(float));
    return D3D_OK;
}
uint32_t hle_d3d9fx_param_set_texture(uint32_t handle, uint32_t texture) {
    param_t *p = (param_t *)hle_ptr(handle);
    if (!p) return D3DERR_INVALIDCALL;
    p->type = PT_TEXTURE, p->tex = texture;
    return D3D_OK;
}
void hle_d3d9fx_param_set_stage(uint32_t handle, uint32_t stage) {
    if (!handle) return;
    ((param_t *)hle_ptr(handle))->stage = stage;
}
uint32_t hle_d3d9fx_param_get_stage(uint32_t handle) {
    return handle ? ((param_t *)hle_ptr(handle))->stage : 0u;
}

/* ---- technique / pass state machine ----
 * The future fx_2_0 parser fills this description; for now a host API seeds it
 * so the COM methods and their stdcall frames can be tested on their own. */
#define MAX_TECH 8
#define MAX_PASS 8

typedef struct {
    char name[64];
    uint32_t pass[MAX_PASS];                 /* guest VAs of pass name strings */
    uint32_t npass;
    uint32_t vs[MAX_PASS];                   /* compiled vertex shader token VAs (0 = none) */
    uint32_t ps[MAX_PASS];                   /* compiled pixel shader token VAs (0 = none) */
    uint32_t vs_ct[MAX_PASS];                /* ID3DXConstantTable for the VS */
    uint32_t ps_ct[MAX_PASS];                /* ID3DXConstantTable for the PS */
    uint32_t vs_obj[MAX_PASS];               /* IDirect3DVertexShader9 made from vs[] (0 = not yet) */
    uint32_t ps_obj[MAX_PASS];               /* IDirect3DPixelShader9 made from ps[] */
} tech_t;

/* A device state an effect pass applies: a D3DRS render state or a D3DSAMP
 * sampler state on a given stage. The public enum values are used directly. */
#define MAX_STATES 128
typedef struct {
    uint32_t kind;                           /* 0 = D3DRS, 1 = D3DSAMP */
    uint32_t stage;                          /* sampler stage when kind == 1 */
    uint32_t state;
    uint32_t value;
    int16_t  tech;                           /* technique index, -1 = any */
    int16_t  pass;                           /* pass index within technique, -1 = any */
} fxstate_t;

typedef struct {
    uint32_t pool;                           /* the shared pool object */
    uint32_t ntech;
    tech_t tech[MAX_TECH];
    uint32_t device;                         /* the IDirect3DDevice9 the effect draws with */
    uint32_t current;                        /* 1-based technique index, 0 = none */
    int begun, pass;                         /* pass: -1 = none */
    uint32_t commits;
    uint32_t shaders_created, shaders_live;  /* device shader objects made / still held */
    fxstate_t state[MAX_STATES];             /* render + sampler states */
    uint32_t nstate;
    uint32_t blocks;                         /* first parameter block (guest VA) */
    uint32_t recording;                      /* open parameter block, 0 = none */
} effect_t;
#define EFFECT(o) ((effect_t *)((char *)hle_ptr(o) + 8))

/* A parameter block records the values of the parameters changed through this
 * effect between BeginParameterBlock and EndParameterBlock. The handle given
 * to the guest is the guest VA of one of these blocks; they are kept in a
 * per-effect list so a foreign handle can be rejected by comparing addresses
 * before anything is dereferenced. */
#define PARAMBLOCK_MAGIC 0x424D5042u /* 'BMPB' */
typedef struct {
    uint32_t magic;
    uint32_t id;
    uint32_t next;               /* guest VA of the next block, 0 ends the list */
    uint32_t nrec;               /* number of recorded values */
    param_t rec[MAX_PARAMS];     /* one snapshot per changed parameter */
} paramblock_t;

static uint32_t g_paramblock_id;

static paramblock_t *pb_from_handle(const effect_t *e, uint32_t h) {
    uint32_t cur;
    if (!e || !h) return 0;
    for (cur = e->blocks; cur; ) {
        paramblock_t *b = (paramblock_t *)hle_ptr(cur);
        if (!b) return 0;
        if (cur == h) return b->magic == PARAMBLOCK_MAGIC ? b : 0;
        cur = b->next;
    }
    return 0;
}

static void pb_record(effect_t *e, const param_t *p) {
    paramblock_t *b;
    uint32_t i;
    if (!e || !e->recording || !p) return;
    b = (paramblock_t *)hle_ptr(e->recording);
    if (!b || b->magic != PARAMBLOCK_MAGIC) return;
    for (i = 0; i < b->nrec; i++)
        if (!strcmp(b->rec[i].name, p->name)) { b->rec[i] = *p; return; }
    if (b->nrec < MAX_PARAMS) b->rec[b->nrec++] = *p;
}

static void pb_free_list(effect_t *e) {
    uint32_t cur;
    if (!e) return;
    cur = e->blocks;
    while (cur) {
        paramblock_t *b = (paramblock_t *)hle_ptr(cur);
        if (!b) break;
        cur = b->next;
        hle_free(b);
    }
    e->blocks = 0;
    e->recording = 0;
}

/* ---- IDirect3DDevice9 vtable slots (in the order d3d9.c registers them) ---- */
enum {
    DEV_AddRef = 1, DEV_Release = 2, DEV_GetDeviceCaps = 7,
    DEV_SetTransform = 44, DEV_GetViewport = 48,
    DEV_SetRenderState = 57, DEV_CreateStateBlock = 59,
    DEV_BeginStateBlock = 60, DEV_EndStateBlock = 61,
    DEV_SetTexture = 65, DEV_SetTextureStageState = 67, DEV_SetSamplerState = 69,
    DEV_SetNPatchMode = 79, DEV_DrawPrimitiveUP = 83,
    DEV_CreateVertexDeclaration = 86, DEV_SetVertexDeclaration = 87,
    DEV_CreateVertexShader = 91, DEV_SetVertexShader = 92, DEV_GetVertexShader = 93,
    DEV_SetStreamSource = 100, DEV_SetIndices = 104,
    DEV_CreatePixelShader = 106, DEV_SetPixelShader = 107, DEV_GetPixelShader = 108,
    DEV_SetPixelShaderConstantF = 109,
    DEV_SetVertexShaderConstantF = 94
};

/* Device call: read a COM object's vtable and invoke one of its slots. */
static uint32_t vcall(uint32_t obj, int slot, int nargs, const uint32_t *args) {
    uint32_t a[8];
    a[0] = obj;
    for (int i = 0; i < nargs; i++) a[1 + i] = args[i];
    return hle_call_guest(MEM32(MEM32(obj) + 4u * (uint32_t)slot), nargs + 1, a);
}

/* Ask the compiler's constant table for the register that backs `name`.
 * D3DXCONSTANT_DESC layout: Name(0), RegisterSet(4), RegisterIndex(8),
 * RegisterCount(12). Only float4 registers can be set through
 * Set*ShaderConstantF; a parameter absent from the table is simply unused by
 * that shader and is skipped. Returns 1 when idx/count were filled. */
#define D3DXRS_FLOAT4 2u
static int ct_register(uint32_t ct, const char *name, uint32_t *idx, uint32_t *count) {
    if (!ct || !name) return 0;
    uint32_t nv = hle_guest_strdup(name);
    if (!nv) return 0;
    uint32_t h = hle_call_guest(hle_resolve("ID3DXConstantTable::GetConstantByName"), 3,
                                (uint32_t[]){ ct, 0u, nv });
    hle_free(hle_ptr(nv));
    if (!h) return 0;
    void *desc = hle_alloc(48);
    uint32_t *pcount = (uint32_t *)hle_alloc(4);
    if (!desc || !pcount) { if (desc) hle_free(desc); if (pcount) hle_free(pcount); return 0; }
    MEM32(hle_va(pcount)) = 0;
    uint32_t rc = hle_call_guest(hle_resolve("ID3DXConstantTable::GetConstantDesc"), 4,
                                 (uint32_t[]){ ct, h, hle_va(desc), hle_va(pcount) });
    int ok = 0;
    if (rc == D3D_OK && MEM32(hle_va(pcount)) >= 1u) {
        uint32_t *d = (uint32_t *)desc;
        if (d[1] == D3DXRS_FLOAT4 && d[2] < 256u) {
            *idx = d[2];
            uint32_t n = d[3] ? d[3] : 1u;
            *count = n > 4u ? 4u : n;
            ok = 1;
        }
    }
    hle_free(desc);
    hle_free(pcount);
    return ok;
}

/* Push the shared parameter store to the device. Constant register indices
 * come from each shader's constant table (no declaration-order guessing);
 * textures are bound at the sampler stage recorded for the parameter, with
 * SetTexture(stage, texture) in the public argument order. */
int hle_d3d9fx_pool_apply(uint32_t pool, uint32_t dev, uint32_t vs_ct, uint32_t ps_ct) {
    pool_t *p = POOL(pool);
    if (!p || !dev) return 0;
    for (uint32_t i = 0; i < p->n; i++) {
        param_t *q = &p->p[i];
        if (q->type == PT_TEXTURE) {
            if (q->tex) {
                uint32_t ta[2] = { q->stage, q->tex };   /* (Stage, pTexture) */
                vcall(dev, DEV_SetTexture, 2, ta);
            }
            continue;
        }
        uint32_t idx, count;
        if (ct_register(ps_ct, q->name, &idx, &count)) {
            uint32_t a[3] = { idx, hle_va(q->f), count };
            vcall(dev, DEV_SetPixelShaderConstantF, 3, a);
        }
        if (ct_register(vs_ct, q->name, &idx, &count)) {
            uint32_t a[3] = { idx, hle_va(q->f), count };
            vcall(dev, DEV_SetVertexShaderConstantF, 3, a);
        }
    }
    return 0;
}

/* Apply recorded render/sampler states for the current technique+pass.
 * Sampler states (kind==1) are effect-global (tech/pass < 0). Pass render
 * states are only applied when they belong to the given technique and pass. */
static void apply_states(uint32_t effect, uint32_t dev, int tech, int pass) {
    effect_t *e = EFFECT(effect);
    for (uint32_t i = 0; i < e->nstate; i++) {
        fxstate_t *s = &e->state[i];
        if (s->kind != 1u) {
            if (s->tech >= 0 && s->tech != (int16_t)tech) continue;
            if (s->pass >= 0 && s->pass != (int16_t)pass) continue;
        }
        uint32_t a[3];
        if (s->kind == 1u) {
            a[0] = s->stage; a[1] = s->state; a[2] = s->value; /* SetSamplerState */
            vcall(dev, DEV_SetSamplerState, 3, a);
        } else {
            a[0] = s->state; a[1] = s->value;                  /* SetRenderState */
            vcall(dev, DEV_SetRenderState, 2, a);
        }
    }
}

/* Device shader objects are made once per effect and pass, on the first
 * BeginPass that needs them, and live until the effect is released or the
 * device is lost (hle_d3d9fx_effect_on_lost). */
static uint32_t make_shader(effect_t *e, int create_slot, uint32_t tokens) {
    uint32_t *pout = (uint32_t *)hle_alloc(4), obj = 0;
    if (!pout) return 0;
    *pout = 0;
    uint32_t a[2] = { tokens, hle_va(pout) };
    if (vcall(e->device, create_slot, 2, a) == D3D_OK && *pout) {
        obj = *pout;
        e->shaders_created++; e->shaders_live++;
    }
    hle_free(pout);
    return obj;
}

/* Drop the effect's reference to a shader object. The device keeps only a
 * pointer to the shader it has bound (no reference), so first unbind it when it
 * is ours; an application that took its own reference keeps the object alive. */
static void drop_shader(effect_t *e, uint32_t *obj, int get_slot, int set_slot) {
    if (!*obj) return;
    /* Get*Shader hands out an AddRef'ed pointer. */
    uint32_t *pcur = (uint32_t *)hle_alloc(4);
    if (pcur) {
        uint32_t a[1] = { hle_va(pcur) }, cur;
        *pcur = 0;
        if (vcall(e->device, get_slot, 1, a) == D3D_OK && *pcur) {
            cur = *pcur;
            uint32_t r[1] = { 0 };
            if (cur == *obj) vcall(e->device, set_slot, 1, r);
            hle_call_guest(MEM32(MEM32(cur) + 8u), 1, (uint32_t[]){ cur });   /* drop the Get's reference */
        }
        hle_free(pcur);
    }
    hle_call_guest(MEM32(MEM32(*obj) + 8u), 1, (uint32_t[]){ *obj });          /* the effect's reference */
    *obj = 0;
    e->shaders_live--;
}

static void release_shaders(effect_t *e) {
    if (!e->device) return;
    for (uint32_t i = 0; i < e->ntech; i++)
        for (uint32_t k = 0; k < e->tech[i].npass; k++) {
            drop_shader(e, &e->tech[i].vs_obj[k], DEV_GetVertexShader, DEV_SetVertexShader);
            drop_shader(e, &e->tech[i].ps_obj[k], DEV_GetPixelShader, DEV_SetPixelShader);
        }
}

/* Apply one pass of one technique to the device: create (when needed) and bind
 * the compiled vertex/pixel shader. A pass without compiled shaders keeps the
 * previously bound pipeline (a host may have set its own shader objects). */
static void apply_pass_shaders(uint32_t effect, int pass_idx) {
    effect_t *e = EFFECT(effect);
    uint32_t dev = e->device;
    if (!dev) return;
    tech_t *t = &e->tech[e->current ? e->current - 1 : 0];
    uint32_t vs = pass_idx >= 0 && (uint32_t)pass_idx < t->npass ? t->vs[pass_idx] : 0;
    uint32_t ps = pass_idx >= 0 && (uint32_t)pass_idx < t->npass ? t->ps[pass_idx] : 0;
    uint32_t vct = pass_idx >= 0 && (uint32_t)pass_idx < t->npass ? t->vs_ct[pass_idx] : 0;
    uint32_t pct = pass_idx >= 0 && (uint32_t)pass_idx < t->npass ? t->ps_ct[pass_idx] : 0;
    hle_d3d9fx_pool_apply(e->pool, dev, vct, pct);
    int tech_idx = e->current ? (int)e->current - 1 : 0;
    apply_states(effect, dev, tech_idx, pass_idx);
    if (vs) {
        if (!t->vs_obj[pass_idx]) t->vs_obj[pass_idx] = make_shader(e, DEV_CreateVertexShader, vs);
        if (t->vs_obj[pass_idx]) {
            uint32_t sa[1] = { t->vs_obj[pass_idx] };
            vcall(dev, DEV_SetVertexShader, 1, sa);
        }
    }
    if (ps) {
        if (!t->ps_obj[pass_idx]) t->ps_obj[pass_idx] = make_shader(e, DEV_CreatePixelShader, ps);
        if (t->ps_obj[pass_idx]) {
            uint32_t sa[1] = { t->ps_obj[pass_idx] };
            vcall(dev, DEV_SetPixelShader, 1, sa);
        }
    }
}

static void e_begin(void) {                  /* (this, &numPasses, Flags) */
    effect_t *e = EFFECT(A32(0));
    uint32_t t = e->current ? e->current - 1 : 0;
    if (A32(2) & ~0x00000070u /* D3DXFX_DONOTSAVE{STATE,SHADERSTATE,SAMPLERSTATE} */) RET(D3DERR_INVALIDCALL, 3);
    if (e->begun || !e->ntech) RET(D3DERR_INVALIDCALL, 3);
    if (A32(1)) MEM32(A32(1)) = e->tech[t].npass;
    e->begun = 1, e->pass = -1;
    apply_pass_shaders(A32(0), 0);
    RET(D3D_OK, 3);
}
static void e_end(void) {
    effect_t *e = EFFECT(A32(0));
    if (!e->begun) RET(D3DERR_INVALIDCALL, 1);
    e->begun = 0, e->pass = -1;
    RET(D3D_OK, 1);
}
static void e_beginpass(void) {              /* (this, pass index) */
    effect_t *e = EFFECT(A32(0));
    uint32_t p = A32(1), t = e->current ? e->current - 1 : 0;
    if (!e->begun || p >= e->tech[t].npass) RET(D3DERR_INVALIDCALL, 2);
    e->pass = (int)p;
    apply_pass_shaders(A32(0), (int)p);
    RET(D3D_OK, 2);
}
static void e_endpass(void) {
    effect_t *e = EFFECT(A32(0));
    if (!e->begun || e->pass < 0) RET(D3DERR_INVALIDCALL, 1);
    e->pass = -1;
    RET(D3D_OK, 1);
}
/* CommitChanges really applies the current parameter values and states to the
 * device (not just a counter). The technique/pass state does not change. */
static void e_commit(void) {
    uint32_t effect = A32(0);
    effect_t *e = EFFECT(effect);
    e->commits++;
    if (e->device) {
        int tech_idx = e->current ? (int)e->current - 1 : 0;
        tech_t *t = &e->tech[tech_idx];
        int pass = e->pass >= 0 && (uint32_t)e->pass < t->npass ? e->pass : 0;
        hle_d3d9fx_pool_apply(e->pool, e->device, t->vs_ct[pass], t->ps_ct[pass]);
        apply_states(effect, e->device, tech_idx, pass);
    }
    RET(D3D_OK, 1);
}
static void e_tech_by_name(void) {           /* (this, name) -> technique handle */
    effect_t *e = EFFECT(A32(0));
    const char *name = (const char *)hle_ptr(A32(1));
    if (name) for (uint32_t i = 0; i < e->ntech; i++)
        if (!strcmp(e->tech[i].name, name)) RET(hle_va(&e->tech[i]), 2);
    RET(0, 2);
}
static void e_pass_by_name(void) {           /* (this, technique, name) -> pass handle */
    tech_t *t = (tech_t *)hle_ptr(A32(1));
    const char *name = (const char *)hle_ptr(A32(2));
    if (t && name) for (uint32_t i = 0; i < t->npass; i++) {
        const char *pn = (const char *)hle_ptr(t->pass[i]);
        if (pn && !strcmp(pn, name)) RET(t->pass[i], 3);
    }
    RET(0, 3);
}
static void e_set_tech(void) {               /* (this, technique handle) */
    effect_t *e = EFFECT(A32(0));
    uint32_t idx = 0;
    for (uint32_t i = 0; i < e->ntech; i++)
        if (A32(1) && hle_va(&e->tech[i]) == A32(1)) { idx = i + 1; break; }
    if (!idx) RET(D3DERR_INVALIDCALL, 2);
    e->current = idx, e->begun = 0, e->pass = -1;
    RET(D3D_OK, 2);
}
static void e_current_tech(void) {
    effect_t *e = EFFECT(A32(0));
    RET(e->current ? hle_va(&e->tech[e->current - 1]) : 0, 1);
}
static void e_param_by_name(void) {          /* (this, parent handle, name) -> parameter handle */
    effect_t *e = EFFECT(A32(0));
    uint32_t name = A32(2);                  /* A32(1) is the parent D3DXHANDLE */
    RET(name ? hle_d3d9fx_pool_param(e->pool, (const char *)hle_ptr(name), 0) : 0u, 3);
}
static void e_set_float(void) {              /* (this, parameter handle, value) */
    uint32_t h = A32(1);
    if (!h) RET(D3DERR_INVALIDCALL, 3);
    param_t *p = (param_t *)hle_ptr(h);
    p->type = PT_FLOAT, p->f[0] = f32(A32(2));
    pb_record(EFFECT(A32(0)), p);
    RET(D3D_OK, 3);
}
static void e_set_vector(void) {           /* (this, handle, const float4*) */
    uint32_t h = A32(1), v = A32(2);
    param_t *p = h ? (param_t *)hle_ptr(h) : NULL;
    if (!p || !v) RET(D3DERR_INVALIDCALL, 3);
    p->type = PT_VECTOR;
    memcpy(p->f, hle_ptr(v), 16);
    pb_record(EFFECT(A32(0)), p);
    RET(D3D_OK, 3);
}
static void e_get_float(void) {
    uint32_t h = A32(1);
    if (!h) RET(D3DERR_INVALIDCALL, 3);
    param_t *p = (param_t *)hle_ptr(h);
    if (p->type != PT_FLOAT || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    memcpy(hle_ptr(A32(2)), &p->f[0], 4);
    RET(D3D_OK, 3);
}
static void e_set_texture(void) {
    uint32_t h = A32(1);
    if (!h) RET(D3DERR_INVALIDCALL, 3);
    param_t *p = (param_t *)hle_ptr(h);
    p->type = PT_TEXTURE, p->tex = A32(2);
    pb_record(EFFECT(A32(0)), p);
    RET(D3D_OK, 3);
}
static void e_get_texture(void) {
    uint32_t h = A32(1);
    if (!h) RET(D3DERR_INVALIDCALL, 3);
    param_t *p = (param_t *)hle_ptr(h);
    if (p->type != PT_TEXTURE || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    MEM32(A32(2)) = p->tex;
    RET(D3D_OK, 3);
}

/* The last Release frees the shader objects and lets go of the device. */
static void e_release(void) {
    uint32_t self = A32(0), n = --MEM32(self + 4);
    if (!n) {
        effect_t *e = EFFECT(self);
        release_shaders(e);
        pb_free_list(e);
        if (e->device) hle_call_guest(MEM32(MEM32(e->device) + 8u), 1, (uint32_t[]){ e->device });
        hle_com_free(self);
    }
    RET(n, 1);
}

/* ID3DXEffect::GetDevice(ppDevice): the device the effect was created for. */
static void e_get_device(void) {
    effect_t *e = EFFECT(A32(0));
    uint32_t out = A32(1);
    if (!out) RET(D3DERR_INVALIDCALL, 2);
    MEM32(out) = e->device;
    if (e->device) MEM32(e->device + 4)++;
    RET(e->device ? D3D_OK : D3DERR_INVALIDCALL, 2);
}
/* ID3DXEffect::GetPool(ppPool) */
static void e_get_pool(void) {
    effect_t *e = EFFECT(A32(0));
    uint32_t out = A32(1);
    if (!out) RET(D3DERR_INVALIDCALL, 2);
    MEM32(out) = e->pool;
    if (e->pool) MEM32(e->pool + 4)++;
    RET(D3D_OK, 2);
}
/* OnLostDevice: the device shader objects go; OnResetDevice has nothing to rebuild
 * (BeginPass remakes the shaders on demand). */
static void e_on_lost(void) { hle_d3d9fx_effect_on_lost(A32(0)); RET(D3D_OK, 1); }
static void e_on_reset(void) { RET(D3D_OK, 1); }

/* ---- ID3DXEffect methods added for this task ----
 * Signatures and order follow Wine's include/d3dx9effect.h, ID3DXEffect with
 * D3DX_SDK_VERSION >= 27. Every method has exactly one RET with the stdcall
 * argument count of its header declaration (this included). Base slots this
 * HLE does not implement keep the logging stubs in d3dx9_fxvtbl.c. */

/* BeginParameterBlock(): starts recording; Wine declares an HRESULT return. */
static void e_begin_paramblock(void) {
    uint32_t hr = D3DERR_INVALIDCALL;
    uint32_t self = A32(0);
    effect_t *e = self ? EFFECT(self) : 0;
    if (e && !e->recording) {
        paramblock_t *b = (paramblock_t *)hle_alloc(sizeof *b);
        if (b) {
            memset(b, 0, sizeof *b);
            b->magic = PARAMBLOCK_MAGIC;
            b->id = ++g_paramblock_id;
            e->recording = hle_va(b);
            hr = D3D_OK;
        } else hr = 0x8007000Eu; /* E_OUTOFMEMORY */
    }
    RET(hr, 1);
}

/* EndParameterBlock(): returns the handle of the finished block. */
static void e_end_paramblock(void) {
    uint32_t h = 0;
    uint32_t self = A32(0);
    effect_t *e = self ? EFFECT(self) : 0;
    if (e && e->recording) {
        paramblock_t *b = (paramblock_t *)hle_ptr(e->recording);
        if (b && b->magic == PARAMBLOCK_MAGIC) {
            b->next = e->blocks;
            e->blocks = e->recording;
            h = e->recording;
        }
        e->recording = 0;
    }
    RET(h, 1);
}

/* ApplyParameterBlock(this, handle): replay the recorded values. */
static void e_apply_paramblock(void) {
    uint32_t hr = D3DERR_INVALIDCALL;
    uint32_t self = A32(0);
    effect_t *e = self ? EFFECT(self) : 0;
    paramblock_t *b = pb_from_handle(e, A32(1));
    if (b && b->nrec) {
        pool_t *pool = POOL(e->pool);
        uint32_t i;
        if (pool) {
            for (i = 0; i < b->nrec; i++) {
                param_t *src = &b->rec[i], *dst = find(pool, src->name, 1);
                if (!dst) break;
                dst->type = src->type;
                memcpy(dst->f, src->f, sizeof dst->f);
                dst->tex = src->tex;
                dst->stage = src->stage;
            }
            if (i == b->nrec) hr = D3D_OK;
        }
    }
    RET(hr, 2);
}

/* DeleteParameterBlock(this, handle): unlink and free one of our blocks. */
static void e_delete_paramblock(void) {
    uint32_t hr = D3DERR_INVALIDCALL;
    uint32_t self = A32(0), h = A32(1);
    effect_t *e = self ? EFFECT(self) : 0;
    paramblock_t *b = pb_from_handle(e, h);
    if (b) {
        uint32_t *link = e ? &e->blocks : 0;
        while (link && *link && *link != h) {
            paramblock_t *cur = (paramblock_t *)hle_ptr(*link);
            if (!cur) break;
            link = &cur->next;
        }
        if (link && *link == h) *link = b->next;
        if (e && e->recording == h) e->recording = 0;
        hle_free(b);
        hr = D3D_OK;
    }
    RET(hr, 2);
}

/* ValidateTechnique(this, handle): a D3DXHANDLE identifies a technique by
 * address; it is never an array index. */
static void e_validate_technique(void) {
    uint32_t hr = D3DERR_INVALIDCALL;
    uint32_t self = A32(0), h = A32(1);
    effect_t *e = self ? EFFECT(self) : 0;
    if (e && h) {
        for (uint32_t i = 0; i < e->ntech; i++)
            if (hle_va(&e->tech[i]) == h) { hr = D3D_OK; break; }
    }
    RET(hr, 2);
}

/* FindNextValidTechnique(this, handle, &next): a NULL handle asks for the
 * first technique. Every technique in this HLE is valid; with no more, the
 * out handle is NULL and S_FALSE is returned (Wine). */
static void e_find_next_valid_technique(void) {
    uint32_t hr = D3DERR_INVALIDCALL;
    uint32_t self = A32(0), h = A32(1), out = A32(2);
    effect_t *e = self ? EFFECT(self) : 0;
    if (e && out) {
        uint32_t start = 0;
        int valid = 1;
        if (h) {
            valid = 0;
            for (uint32_t i = 0; i < e->ntech; i++)
                if (hle_va(&e->tech[i]) == h) { start = i + 1; valid = 1; break; }
        }
        if (valid) {
            if (start < e->ntech) { MEM32(out) = hle_va(&e->tech[start]); hr = D3D_OK; }
            else { MEM32(out) = 0; hr = 1u; /* S_FALSE */ }
        }
    }
    RET(hr, 3);
}

/* CloneEffect(this, device, &out): the device is required and the clone
 * shares the source pool, with its own copy of the techniques and states. */
static void e_clone_effect(void) {
    uint32_t hr = D3DERR_INVALIDCALL;
    uint32_t self = A32(0), dev = A32(1), out = A32(2);
    effect_t *src = self ? EFFECT(self) : 0;
    if (src && dev && out) {
        uint32_t clone = hle_d3d9fx_effect_new(src->pool);
        if (clone) {
            effect_t *dst = EFFECT(clone);
            for (uint32_t i = 0; i < src->ntech; i++) {
                const char *passes[MAX_PASS] = { 0 };
                for (uint32_t j = 0; j < src->tech[i].npass; j++)
                    passes[j] = (const char *)hle_ptr(src->tech[i].pass[j]);
                hle_d3d9fx_effect_add_tech_ct(clone, src->tech[i].name, passes,
                    src->tech[i].npass, src->tech[i].vs, src->tech[i].ps,
                    src->tech[i].vs_ct, src->tech[i].ps_ct);
            }
            for (uint32_t i = 0; i < src->nstate; i++)
                hle_d3d9fx_effect_add_state(clone, src->state[i].kind, src->state[i].stage,
                    src->state[i].state, src->state[i].value, src->state[i].tech,
                    src->state[i].pass);
            hle_d3d9fx_effect_set_device(clone, dev);
            dst->current = src->current;
            MEM32(out) = clone;
            hr = D3D_OK;
        } else hr = 0x8007000Eu; /* E_OUTOFMEMORY */
    }
    RET(hr, 3);
}

static uint32_t param_data_size(const param_t *p) {
    if (!p) return 0;
    if (p->type == PT_VECTOR) return 16;
    if (p->type == PT_MATRIX) return 64;
    return 4;
}

/* A parameter handle is a guest VA into the shared pool. Compare addresses
 * instead of dereferencing an unchecked guest value. */
static param_t *effect_param(const effect_t *e, uint32_t h) {
    pool_t *pool;
    if (!e || !h) return 0;
    pool = POOL(e->pool);
    if (!pool) return 0;
    for (uint32_t i = 0; i < pool->n; i++)
        if (hle_va(&pool->p[i]) == h) return &pool->p[i];
    return 0;
}

/* SetRawValue(this, parameter, data, byte_offset, bytes). */
static void e_set_raw_value(void) {
    uint32_t hr = D3DERR_INVALIDCALL;
    uint32_t self = A32(0), h = A32(1), data = A32(2), off = A32(3), bytes = A32(4);
    effect_t *e = self ? EFFECT(self) : 0;
    param_t *p = effect_param(e, h);
    if (p && data) {
        if (p->type == PT_TEXTURE) {
            if (off == 0 && bytes == 4) { p->tex = MEM32(data); hr = D3D_OK; }
        } else {
            uint32_t total = param_data_size(p);
            if (off <= total && bytes <= total - off) {
                memcpy((char *)p->f + off, hle_ptr(data), bytes);
                hr = D3D_OK;
            }
        }
        if (hr == D3D_OK) pb_record(e, p);
    }
    RET(hr, 5);
}

/* Implemented methods; every other slot of the public 79-slot layout comes from
 * d3dx9_fxvtbl.c as a logged "not supported". */
static const win32hle_shim effect_impl[] = {
    { "GetPool", e_get_pool },
    { "QueryInterface", d3d_QueryInterface },
    { "AddRef", hle_com_AddRef },
    { "Release", e_release },
    { "Begin", e_begin }, { "End", e_end },
    { "BeginPass", e_beginpass }, { "EndPass", e_endpass },
    { "CommitChanges", e_commit },
    { "GetTechniqueByName", e_tech_by_name },
    { "GetPassByName", e_pass_by_name },
    { "SetTechnique", e_set_tech },
    { "GetCurrentTechnique", e_current_tech },
    { "GetParameterByName", e_param_by_name },
    { "SetFloat", e_set_float }, { "GetFloat", e_get_float },
    { "SetTexture", e_set_texture }, { "GetTexture", e_get_texture },
    { "SetVector", e_set_vector },
    { "GetDevice", e_get_device },
    { "OnLostDevice", e_on_lost },
    { "OnResetDevice", e_on_reset },
    { "ValidateTechnique", e_validate_technique },
    { "FindNextValidTechnique", e_find_next_valid_technique },
    { "BeginParameterBlock", e_begin_paramblock },
    { "EndParameterBlock", e_end_paramblock },
    { "ApplyParameterBlock", e_apply_paramblock },
    { "DeleteParameterBlock", e_delete_paramblock },
    { "CloneEffect", e_clone_effect },
    { "SetRawValue", e_set_raw_value },
    { 0, 0 }
};

uint32_t hle_d3d9fx_effect_new(uint32_t pool) {
    if (!g_vt_effect) g_vt_effect = d3dx9_fx_vtable("ID3DXEffect", 0, effect_impl);
    uint32_t obj = hle_com_new(g_vt_effect, 8 + sizeof(effect_t));
    effect_t *e = EFFECT(obj);
    memset(e, 0, sizeof *e);
    e->pool = pool, e->pass = -1;
    return obj;
}
int hle_d3d9fx_effect_add_tech(uint32_t effect, const char *name, const char **passes, int npass) {
    return hle_d3d9fx_effect_add_tech_full(effect, name, passes, npass, NULL, NULL);
}
int hle_d3d9fx_effect_add_tech_full(uint32_t effect, const char *name, const char **passes, int npass, const uint32_t *vs, const uint32_t *ps) {
    return hle_d3d9fx_effect_add_tech_ct(effect, name, passes, npass, vs, ps, NULL, NULL);
}
int hle_d3d9fx_effect_add_tech_ct(uint32_t effect, const char *name, const char **passes, int npass,
                                  const uint32_t *vs, const uint32_t *ps,
                                  const uint32_t *vs_ct, const uint32_t *ps_ct) {
    if (!effect) return 0;
    effect_t *e = EFFECT(effect);
    if (e->ntech >= MAX_TECH) return 0;
    tech_t *t = &e->tech[e->ntech];
    snprintf(t->name, sizeof t->name, "%s", name ? name : "");
    t->npass = 0;
    for (int i = 0; i < npass && i < MAX_PASS; i++) {
        if (!passes || !passes[i]) break;
        t->pass[t->npass] = hle_guest_strdup(passes[i]);
        t->npass++;
        t->vs[i] = vs ? vs[i] : 0;
        t->ps[i] = ps ? ps[i] : 0;
        t->vs_ct[i] = vs_ct ? vs_ct[i] : 0;
        t->ps_ct[i] = ps_ct ? ps_ct[i] : 0;
    }
    return (int)++e->ntech;
}
int hle_d3d9fx_effect_add_state(uint32_t effect, uint32_t kind, uint32_t stage,
                                uint32_t state, uint32_t value, int tech, int pass) {
    effect_t *e = effect ? EFFECT(effect) : NULL;
    if (!e || kind > 1u || e->nstate >= MAX_STATES) return 0;
    fxstate_t *s = &e->state[e->nstate++];
    s->kind = kind; s->stage = stage; s->state = state; s->value = value;
    s->tech = (int16_t)tech; s->pass = (int16_t)pass;
    return 1;
}
uint32_t hle_d3d9fx_effect_set_device(uint32_t effect, uint32_t dev) {
    if (!effect) return 0;
    effect_t *e = EFFECT(effect);
    if (e->device == dev) return dev;
    if (dev) hle_call_guest(MEM32(MEM32(dev) + 4u), 1, (uint32_t[]){ dev });             /* AddRef */
    if (e->device) { release_shaders(e); hle_call_guest(MEM32(MEM32(e->device) + 8u), 1, (uint32_t[]){ e->device }); }
    e->device = dev;
    return dev;
}
/* Device lost: the shader objects belong to the device and are dropped; the next
 * BeginPass makes them again. */
void hle_d3d9fx_effect_on_lost(uint32_t effect) {
    if (effect) release_shaders(EFFECT(effect));
}
uint32_t hle_d3d9fx_effect_shader_stats(uint32_t effect, uint32_t *live) {
    effect_t *e = effect ? EFFECT(effect) : 0;
    if (live) *live = e ? e->shaders_live : 0;
    return e ? e->shaders_created : 0;
}
uint32_t hle_d3d9fx_effect_stats(uint32_t effect, uint32_t *commits) {
    effect_t *e = effect ? EFFECT(effect) : 0;
    if (!e) return 0;
    if (commits) *commits = e->commits;
    return (e->ntech << 16) | ((uint32_t)(e->pass + 1) << 8) | (uint32_t)(e->begun != 0);
}

const win32hle_shim win32hle_d3d9fx[] = {
    { "D3DXCreateEffectPool", f_create_pool },
    { "d3dx9fx_host::PoolSetFloat", p_set_float }, { "d3dx9fx_host::PoolGetFloat", p_get_float },
    { "d3dx9fx_host::PoolSetInt", p_set_int }, { "d3dx9fx_host::PoolGetInt", p_get_int },
    { "d3dx9fx_host::PoolSetBool", p_set_bool }, { "d3dx9fx_host::PoolGetBool", p_get_bool },
    { "d3dx9fx_host::PoolSetVector", p_set_vector }, { "d3dx9fx_host::PoolGetVector", p_get_vector },
    { "d3dx9fx_host::PoolSetMatrix", p_set_matrix }, { "d3dx9fx_host::PoolGetMatrix", p_get_matrix },
    { "d3dx9fx_host::PoolSetTexture", p_set_texture }, { "d3dx9fx_host::PoolGetTexture", p_get_texture },
    { 0, 0 }
};
