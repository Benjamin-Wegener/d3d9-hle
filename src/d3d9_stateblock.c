/*
 * d3d9_stateblock.c - IDirect3DStateBlock9 implementation for win32hle.
 *
 * A state block holds a set of device states, chosen by a mask (d3d9_sb_mask_t):
 *  - CreateStateBlock(ALL/PIXELSTATE/VERTEXSTATE) selects whole groups;
 *  - BeginStateBlock/EndStateBlock records exactly the states the application
 *    set in between (every setter reports through sb9_mark);
 *  - Capture re-reads the selected states from the device, Apply writes them
 *    back (and re-synchronises the GPU backend).
 * Resource pointers held by a block are AddRef'ed on capture and released when
 * the block is recaptured or destroyed. Render targets, depth-stencil surfaces
 * and the gamma ramp are not state-block state in D3D9 and are never held.
 *
 * Which states belong to the pixel or vertex group follows the D3D9
 * documentation ("State Blocks Save and Restore State"); the D3DRS_/D3DTSS_/
 * D3DSAMP_ numbers are those of the public d3d9types.h. Wine's wined3d
 * stateblock.c was read as a second reference for the grouping only; no code
 * or table was copied from it.
 */
#include "d3d9_internal.h"

/* Render states that belong to the pixel group (D3DRS_* values). */
static const uint16_t s_rs_pixel[] = {
     7,  8,  9, 14, 15, 16, 19, 20, 23, 24, 25, 26, 27,
    36, 37, 38,                                /* FOGSTART/END/DENSITY (both) */
    52, 53, 54, 55, 56, 57, 58, 59, 60,        /* stencil, TEXTUREFACTOR */
   128,129,130,131,132,133,134,135,            /* WRAP0-7 */
   168,171,174,175,176,                        /* COLORWRITEENABLE, BLENDOP, scissor, bias, AA line */
   185,186,187,188,189,190,191,192,193,194,195,
   198,199,200,201,202,203,204,205,206,207,208,209
};
/* Render states that belong to the vertex group. */
static const uint16_t s_rs_vertex[] = {
     9, 22, 28, 29, 34, 35, 36, 37, 38, 48,
   136,137,139,140,141,142,143,145,146,147,148,
   151,152,154,155,156,157,158,159,160,161,162,163,
   166,167,170,172,173,178,179,180,181,182,183,184
};
#define SB_SAMP_DMAPOFFSET       13u  /* D3DSAMP_DMAPOFFSET: vertex group */
#define SB_TSS_TEXCOORDINDEX     11u  /* vertex group */
#define SB_TSS_TEXTRANSFORMFLAGS 24u  /* vertex group */

static int sb_in_list(const uint16_t *list, size_t n, uint32_t value) {
    for (size_t i = 0; i < n; i++) if (list[i] == value) return 1;
    return 0;
}

#define SB_SET(a) memset((a), 1, sizeof(a))

/* Select the states a D3DSBT_* block holds. */
void sb9_select_type(d3d9_sb_ctx_t *sb, uint32_t type) {
    d3d9_sb_mask_t *m = &sb->m;
    int all = (type == D3DSBT_ALL);
    int pix = all || type == D3DSBT_PIXELSTATE;
    int vtx = all || type == D3DSBT_VERTEXSTATE;
    memset(m, 0, sizeof(*m));
    sb->type = type;

    for (uint32_t i = 0; i < MAX_RENDER_STATES; i++) {
        int is_p = sb_in_list(s_rs_pixel,  sizeof(s_rs_pixel)  / sizeof(s_rs_pixel[0]),  i);
        int is_v = sb_in_list(s_rs_vertex, sizeof(s_rs_vertex) / sizeof(s_rs_vertex[0]), i);
        if (all || (pix && is_p) || (vtx && is_v)) m->rs[i] = 1;
    }
    for (uint32_t s = 0; s < MAX_SAMPLERS; s++)
        for (uint32_t t = 0; t < MAX_SAMPLER_STATES; t++) {
            int is_v = (t == SB_SAMP_DMAPOFFSET);
            if (all || (is_v ? vtx : pix)) m->samp[s][t] = 1;
        }
    for (uint32_t s = 0; s < MAX_TEXTURE_STAGES; s++)
        for (uint32_t t = 0; t < MAX_TSS; t++) {
            int is_v = (t == SB_TSS_TEXCOORDINDEX || t == SB_TSS_TEXTRANSFORMFLAGS);
            if (all || (is_v ? vtx : pix)) m->tss[s][t] = 1;
        }
    if (pix) {
        SB_SET(m->psf); SB_SET(m->psi); SB_SET(m->psb);
        SB_SET(m->tex);
        m->ps = 1;
    }
    if (vtx) {
        SB_SET(m->xform); SB_SET(m->light); SB_SET(m->light_en); SB_SET(m->clip);
        SB_SET(m->vsf); SB_SET(m->vsi); SB_SET(m->vsb);
        SB_SET(m->stream); SB_SET(m->freq);
        m->material = m->svp = m->npatch = 1;
        m->indices = m->decl = m->fvf = m->vs = 1;
    }
    if (all) {
        SB_SET(m->palette);
        m->viewport = m->scissor = m->cur_palette = 1;
    }
}

/* Release a COM reference with the object's own Release method so resource
 * contexts (GPU buffers, textures, ...) are cleaned up at zero. */
static void sb_release_ref(uint32_t obj) {
    if (!obj) return;
    uint32_t vt = MEM32(obj);
    const char *name = NULL;
    if      (vt == g_vt_surface9)  name = "IDirect3DSurface9::Release";
    else if (vt == g_vt_tex9)      name = "IDirect3DTexture9::Release";
    else if (vt == g_vt_cubetex9)  name = "IDirect3DCubeTexture9::Release";
    else if (vt == g_vt_volumetex9) name = "IDirect3DVolumeTexture9::Release";
    else if (vt == g_vt_vb9)       name = "IDirect3DVertexBuffer9::Release";
    else if (vt == g_vt_ib9)       name = "IDirect3DIndexBuffer9::Release";
    else if (vt == g_vt_decl9)     name = "IDirect3DVertexDeclaration9::Release";
    else if (vt == g_vt_vs9)       name = "IDirect3DVertexShader9::Release";
    else if (vt == g_vt_ps9)       name = "IDirect3DPixelShader9::Release";

    if (name) {
        uint32_t a[1] = { obj };
        hle_call_guest(hle_resolve(name), 1, a);
    } else {
        uint32_t n = --MEM32(obj + 4);
        if (!n) d3d9_free_obj(obj);
    }
}

/* Transfer one held reference. Keeping the same object keeps the existing
 * reference; replacing or clearing it releases the old one. */
static void sb_hold(uint32_t *slot, uint32_t obj) {
    if (*slot == obj) return;
    if (*slot) sb_release_ref(*slot);
    *slot = obj;
    if (obj) MEM32(obj + 4)++;
}

/* Move the selected states between a block and the device. D = destination
 * context, S = source context, HOLD(slot, value) stores a resource pointer
 * (the block takes a reference, the device does not). */
#define SB_XFER(D, S, HOLD) do {                                                  \
    const d3d9_sb_mask_t *m = &sb->m;                                             \
    for (uint32_t i = 0; i < MAX_RENDER_STATES; i++)                              \
        if (m->rs[i]) (D)->render_states[i] = (S)->render_states[i];              \
    for (uint32_t i = 0; i < MAX_SAMPLERS; i++)                                   \
        for (uint32_t t = 0; t < MAX_SAMPLER_STATES; t++)                         \
            if (m->samp[i][t]) (D)->sampler_states[i][t] = (S)->sampler_states[i][t]; \
    for (uint32_t i = 0; i < MAX_TEXTURE_STAGES; i++)                             \
        for (uint32_t t = 0; t < MAX_TSS; t++)                                    \
            if (m->tss[i][t]) (D)->texture_stage_states[i][t] = (S)->texture_stage_states[i][t]; \
    for (uint32_t i = 0; i < 512; i++)                                            \
        if (m->xform[i]) (D)->transforms[i] = (S)->transforms[i];                 \
    for (uint32_t i = 0; i < D3D9_MAX_LIGHTS; i++) {                              \
        if (m->light[i]) {                                                         \
            memcpy((D)->lights[i], (S)->lights[i], D3D9_LIGHT_BYTES);              \
            (D)->light_defined[i] = (S)->light_defined[i];                         \
        }                                                                          \
        if (m->light_en[i]) (D)->light_enable[i] = (S)->light_enable[i];          \
    }                                                                             \
    for (uint32_t i = 0; i < D3D9_MAX_CLIP_PLANES; i++)                           \
        if (m->clip[i]) memcpy((D)->clip_planes[i], (S)->clip_planes[i], 16);     \
    for (uint32_t i = 0; i < D3D9_MAX_PALETTES; i++)                              \
        if (m->palette[i]) memcpy((D)->palette[i], (S)->palette[i], D3D9_PALETTE_ENTRIES * 4); \
    for (uint32_t i = 0; i < MAX_VS_CONST_F; i++)                                 \
        if (m->vsf[i]) memcpy((D)->vs_const_f[i], (S)->vs_const_f[i], 16);        \
    for (uint32_t i = 0; i < D3D9_MAX_VS_CONST_I; i++)                            \
        if (m->vsi[i]) memcpy((D)->vs_const_i[i], (S)->vs_const_i[i], 16);        \
    for (uint32_t i = 0; i < D3D9_MAX_VS_CONST_B; i++)                            \
        if (m->vsb[i]) (D)->vs_const_b[i] = (S)->vs_const_b[i];                   \
    for (uint32_t i = 0; i < MAX_PS_CONST_F; i++)                                 \
        if (m->psf[i]) memcpy((D)->ps_const_f[i], (S)->ps_const_f[i], 16);        \
    for (uint32_t i = 0; i < D3D9_MAX_PS_CONST_I; i++)                            \
        if (m->psi[i]) memcpy((D)->ps_const_i[i], (S)->ps_const_i[i], 16);        \
    for (uint32_t i = 0; i < D3D9_MAX_PS_CONST_B; i++)                            \
        if (m->psb[i]) (D)->ps_const_b[i] = (S)->ps_const_b[i];                   \
    for (uint32_t i = 0; i < MAX_SAMPLERS; i++)                                   \
        if (m->tex[i]) HOLD((D)->textures[i], (S)->textures[i]);                  \
    for (uint32_t i = 0; i < MAX_STREAMS; i++) {                                  \
        if (m->stream[i]) {                                                       \
            HOLD((D)->streams[i].vb, (S)->streams[i].vb);                         \
            (D)->streams[i].offset = (S)->streams[i].offset;                      \
            (D)->streams[i].stride = (S)->streams[i].stride;                      \
        }                                                                         \
        if (m->freq[i]) (D)->streams[i].freq = (S)->streams[i].freq;              \
    }                                                                             \
    if (m->viewport)    (D)->viewport = (S)->viewport;                            \
    if (m->scissor)     (D)->scissor_rect = (S)->scissor_rect;                    \
    if (m->material)    memcpy((D)->material, (S)->material, D3D9_MATERIAL_BYTES); \
    if (m->cur_palette) (D)->current_texture_palette = (S)->current_texture_palette; \
    if (m->svp)         (D)->software_vertex_processing = (S)->software_vertex_processing; \
    if (m->npatch)      (D)->n_patch_mode = (S)->n_patch_mode;                    \
    if (m->indices)     HOLD((D)->indices, (S)->indices);                         \
    if (m->decl)        HOLD((D)->vertex_decl, (S)->vertex_decl);                 \
    if (m->fvf)         (D)->fvf = (S)->fvf;                                      \
    if (m->vs)          HOLD((D)->vertex_shader, (S)->vertex_shader);             \
    if (m->ps)          HOLD((D)->pixel_shader, (S)->pixel_shader);               \
} while (0)

#define SB_HOLD_REF(slot, v)  sb_hold(&(slot), (v))
#define SB_PLAIN(slot, v)     ((slot) = (v))

void sb9_release_held(d3d9_sb_ctx_t *sb) {
    for (uint32_t i = 0; i < MAX_SAMPLERS; i++) sb_hold(&sb->textures[i], 0);
    for (uint32_t i = 0; i < MAX_STREAMS; i++) sb_hold(&sb->streams[i].vb, 0);
    sb_hold(&sb->indices, 0);
    sb_hold(&sb->vertex_decl, 0);
    sb_hold(&sb->vertex_shader, 0);
    sb_hold(&sb->pixel_shader, 0);
}

void sb9_capture_state(d3d9_sb_ctx_t *sb, const d3d9_device_ctx_t *dev) {
    if (!sb || !dev) return;
    SB_XFER(sb, dev, SB_HOLD_REF);
}

void sb9_apply_state(const d3d9_sb_ctx_t *sb, d3d9_device_ctx_t *dev) {
    if (!sb || !dev) return;
    SB_XFER(dev, sb, SB_PLAIN);
}

/* ------------------------------------------------------------- recording */

void sb9_mark(d3d9_device_ctx_t *dev, sb9_kind k, uint32_t a, uint32_t n) {
    d3d9_sb_ctx_t *r = dev ? (d3d9_sb_ctx_t *)dev->sb_rec : NULL;
    if (!r) return;
    d3d9_sb_mask_t *m = &r->m;
#define MARK(arr) do { for (uint32_t i = a; i < a + n && i < sizeof(arr) / sizeof((arr)[0]); i++) (arr)[i] = 1; } while (0)
    switch (k) {
    case SBK_RS:        MARK(m->rs); break;
    case SBK_SAMP:      if (a / 16 < MAX_SAMPLERS && (a % 16) < MAX_SAMPLER_STATES) m->samp[a / 16][a % 16] = 1; break;
    case SBK_TSS:       if (a / 64 < MAX_TEXTURE_STAGES && (a % 64) < MAX_TSS) m->tss[a / 64][a % 64] = 1; break;
    case SBK_XFORM:     MARK(m->xform); break;
    case SBK_LIGHT:     MARK(m->light); break;
    case SBK_LIGHT_EN:  MARK(m->light_en); break;
    case SBK_CLIP:      MARK(m->clip); break;
    case SBK_PALETTE:   MARK(m->palette); break;
    case SBK_VSF:       MARK(m->vsf); break;
    case SBK_VSI:       MARK(m->vsi); break;
    case SBK_VSB:       MARK(m->vsb); break;
    case SBK_PSF:       MARK(m->psf); break;
    case SBK_PSI:       MARK(m->psi); break;
    case SBK_PSB:       MARK(m->psb); break;
    case SBK_TEX:       MARK(m->tex); break;
    case SBK_STREAM:    MARK(m->stream); break;
    case SBK_FREQ:      MARK(m->freq); break;
    case SBK_VIEWPORT:  m->viewport = 1; break;
    case SBK_SCISSOR:   m->scissor = 1; break;
    case SBK_MATERIAL:  m->material = 1; break;
    case SBK_CUR_PALETTE: m->cur_palette = 1; break;
    case SBK_SVP:       m->svp = 1; break;
    case SBK_NPATCH:    m->npatch = 1; break;
    case SBK_INDICES:   m->indices = 1; break;
    case SBK_DECL:      m->decl = 1; break;
    case SBK_FVF:       m->fvf = 1; break;
    case SBK_VS:        m->vs = 1; break;
    case SBK_PS:        m->ps = 1; break;
    }
#undef MARK
}

/* Recording keeps the setters' effect on the device (they write the device
 * state directly) and remembers the device state from before, so that
 * EndStateBlock can build the block from the states that were set and then put
 * the device back as it was: nothing set while recording stays applied. */
int sb9_begin_record(d3d9_device_ctx_t *dev) {
    d3d9_sb_ctx_t *rec = (d3d9_sb_ctx_t *)calloc(1, sizeof(*rec));
    d3d9_sb_ctx_t *base = (d3d9_sb_ctx_t *)calloc(1, sizeof(*base));
    if (!rec || !base) { free(rec); free(base); return 0; }
    rec->device = base->device = dev->self;
    sb9_select_type(base, D3DSBT_ALL);
    sb9_capture_state(base, dev);
    dev->sb_rec = rec;
    dev->sb_base = base;
    dev->recording_state_block = 1;
    return 1;
}

static void sb_drop_base(d3d9_device_ctx_t *dev) {
    d3d9_sb_ctx_t *base = (d3d9_sb_ctx_t *)dev->sb_base;
    if (base) { sb9_release_held(base); free(base); }
    dev->sb_base = NULL;
    dev->sb_rec = NULL;
    dev->recording_state_block = 0;
}

d3d9_sb_ctx_t *sb9_end_record(d3d9_device_ctx_t *dev) {
    d3d9_sb_ctx_t *rec = (d3d9_sb_ctx_t *)dev->sb_rec;
    d3d9_sb_ctx_t *base = (d3d9_sb_ctx_t *)dev->sb_base;
    if (!rec || !base) return NULL;
    sb9_capture_state(rec, dev);        /* the values that were set */
    sb9_apply_state(base, dev);         /* the device as it was before Begin */
    sb_drop_base(dev);
    d3d9_device_push_backend(dev);
    return rec;
}

/* Recording ends without a block: the device goes back to its state from
 * before BeginStateBlock (the caller re-synchronises the backend if needed). */
void sb9_abort_record(d3d9_device_ctx_t *dev) {
    d3d9_sb_ctx_t *rec = (d3d9_sb_ctx_t *)dev->sb_rec;
    d3d9_sb_ctx_t *base = (d3d9_sb_ctx_t *)dev->sb_base;
    if (rec) { sb9_release_held(rec); free(rec); }
    if (base) sb9_apply_state(base, dev);
    sb_drop_base(dev);
}

void sb9_Release(void) {
    uint32_t self = A32(0);
    uint32_t n = --MEM32(self + 4);
    if (!n) {
        d3d9_sb_ctx_t *ctx = (d3d9_sb_ctx_t *)OBJ_CTX(self);
        if (ctx) {
            sb9_release_held(ctx);
            free(ctx);
        }
        d3d9_free_obj(self);
    }
    RET(n, 1);
}

void sb9_Capture(void) {
    uint32_t self = A32(0);
    d3d9_sb_ctx_t *sb = (d3d9_sb_ctx_t *)OBJ_CTX(self);
    d3d9_device_ctx_t *dev = sb ? (d3d9_device_ctx_t *)OBJ_CTX(sb->device) : NULL;
    if (!sb || !dev || dev->recording_state_block) RET(D3DERR_INVALIDCALL, 1);
    sb9_capture_state(sb, dev);
    RET(D3D_OK, 1);
}

void sb9_Apply(void) {
    uint32_t self = A32(0);
    d3d9_sb_ctx_t *sb = (d3d9_sb_ctx_t *)OBJ_CTX(self);
    d3d9_device_ctx_t *dev = sb ? (d3d9_device_ctx_t *)OBJ_CTX(sb->device) : NULL;
    if (!sb || !dev || dev->recording_state_block) RET(D3DERR_INVALIDCALL, 1);
    sb9_apply_state(sb, dev);
    /* Bulk state changes bypass the setters, so re-synchronise the backend. */
    d3d9_device_push_backend(dev);
    RET(D3D_OK, 1);
}
