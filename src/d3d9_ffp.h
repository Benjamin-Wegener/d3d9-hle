/*
 * d3d9_ffp.h - Fixed Function Pipeline MSL generator (D3D9 semantics).
 */
#ifndef WIN32HLE_D3D9_FFP_H
#define WIN32HLE_D3D9_FFP_H

#include <stdint.h>
#include <stddef.h>

/* D3DFVF bits (public D3D9). */
#define D3D9_FVF_XYZ      0x002u
#define D3D9_FVF_XYZRHW   0x004u
#define D3D9_FFV_XYZB1    0x006u
#define D3D9_FVF_NORMAL   0x010u
#define D3D9_FVF_PSIZE    0x020u
#define D3D9_FVF_DIFFUSE  0x040u
#define D3D9_FVF_SPECULAR 0x080u
#define D3D9_FVF_TEXCOUNT_MASK 0xF00u
#define D3D9_FVF_TEXCOUNT_SHIFT 8u

/* Texture stage ops (D3D9 D3DTEXTUREOP values). */
#define D3D9_TEXOP_DISABLE                 1u
#define D3D9_TEXOP_SELECTARG1              2u
#define D3D9_TEXOP_SELECTARG2              3u
#define D3D9_TEXOP_MODULATE                4u
#define D3D9_TEXOP_MODULATE2X              5u
#define D3D9_TEXOP_MODULATE4X              6u
#define D3D9_TEXOP_ADD                     7u
#define D3D9_TEXOP_ADDSIGNED               8u
#define D3D9_TEXOP_ADDSIGNED2X             9u
#define D3D9_TEXOP_SUBTRACT                10u
#define D3D9_TEXOP_ADDSMOOTH               11u
#define D3D9_TEXOP_BLENDDIFFUSEALPHA       12u
#define D3D9_TEXOP_BLENDTEXTUREALPHA       13u
#define D3D9_TEXOP_BLENDFACTORALPHA        14u
#define D3D9_TEXOP_BLENDTEXTUREALPHAPM     15u
#define D3D9_TEXOP_BLENDCURRENTALPHA       16u
#define D3D9_TEXOP_PREMODULATE             17u
#define D3D9_TEXOP_MODULATEALPHA_ADDCOLOR  18u
#define D3D9_TEXOP_MODULATECOLOR_ADDALPHA  19u
#define D3D9_TEXOP_MODULATEINVALPHA_ADDCOLOR 20u
#define D3D9_TEXOP_MODULATEINVCOLOR_ADDALPHA 21u
#define D3D9_TEXOP_BUMPENVMAP              22u
#define D3D9_TEXOP_BUMPENVMAPLUMINANCE     23u
#define D3D9_TEXOP_DOTPRODUCT3             24u
#define D3D9_TEXOP_MULTIPLYADD             25u
#define D3D9_TEXOP_LERP                    26u

/* Texture args (D3DTA) */
#define D3D9_TA_DIFFUSE        0u
#define D3D9_TA_CURRENT        1u
#define D3D9_TA_TEXTURE        2u
#define D3D9_TA_TFACTOR        3u
#define D3D9_TA_SPECULAR       4u
#define D3D9_TA_TEMP           5u
#define D3D9_TA_CONSTANT       6u
#define D3D9_TA_COMPLEMENT     0x10u
#define D3D9_TA_ALPHAREPLICATE 0x20u
#define D3D9_TA_SELECTMASK     0x0Fu

/* Texcoord index camera-space flags */
#define D3D9_TCI_PASSTHRU                  0x00000u
#define D3D9_TCI_CAMERASPACENORMAL         0x10000u
#define D3D9_TCI_CAMERASPACEPOSITION       0x20000u
#define D3D9_TCI_CAMERASPACEREFLECTIONVECTOR 0x30000u
#define D3D9_TCI_SPHEREMAP                 0x40000u

/* Texture transform flags */
#define D3D9_TTFF_DISABLE   0u
#define D3D9_TTFF_COUNT1    1u
#define D3D9_TTFF_COUNT2    2u
#define D3D9_TTFF_COUNT3    3u
#define D3D9_TTFF_COUNT4    4u
#define D3D9_TTFF_PROJECTED 256u

/* Fog modes */
#define D3D9_FOG_NONE   0u
#define D3D9_FOG_EXP    1u
#define D3D9_FOG_EXP2   2u
#define D3D9_FOG_LINEAR 3u

/* Alpha compare funcs (D3DCMPFUNC) */
#define D3D9_CMP_NEVER        1u
#define D3D9_CMP_LESS         2u
#define D3D9_CMP_EQUAL        3u
#define D3D9_CMP_LESSEQUAL    4u
#define D3D9_CMP_GREATER      5u
#define D3D9_CMP_NOTEQUAL     6u
#define D3D9_CMP_GREATEREQUAL 7u
#define D3D9_CMP_ALWAYS       8u

typedef struct {
    uint32_t color_op;
    uint32_t color_arg1;
    uint32_t color_arg2;
    uint32_t alpha_op;
    uint32_t alpha_arg1;
    uint32_t alpha_arg2;
    /* extended */
    uint32_t color_arg0;
    uint32_t alpha_arg0;
    uint32_t result_arg;   /* D3DTA_CURRENT or D3DTA_TEMP */
    uint32_t texcoord_index;
    uint32_t texture_transform_flags;
    uint32_t constant;     /* D3DTSS_CONSTANT color */
    float bumpenv_mat00, bumpenv_mat01, bumpenv_mat10, bumpenv_mat11;
    float bumpenv_lscale, bumpenv_loffset;
} d3d9_ffp_stage_t;

typedef struct {
    uint32_t fvf;
    const d3d9_ffp_stage_t *stages;
    uint32_t stage_count;
    uint32_t fog_mode;
    float fog_start;
    float fog_end;
    float fog_density;
    uint32_t fog_color;
    uint32_t alpha_test_enable;
    uint32_t alpha_func;    /* D3D9_CMP_* */
    uint32_t alpha_ref;     /* 0..255 */
    /* lighting */
    uint32_t lighting;          /* 0 = unlit, 1 = lighting enabled */
    uint32_t specular_enable;   /* D3DRS_SPECULARENABLE */
    uint32_t local_viewer;      /* D3DRS_LOCALVIEWER */
    uint32_t normalize_normals; /* D3DRS_NORMALIZENORMALS */
    uint32_t color_vertex;      /* D3DRS_COLORVERTEX */
    uint32_t diff_mat_source;   /* D3DRS_DIFFUSEMATERIALSOURCE: 0=MAT, 1=COLOR1, 2=COLOR2 */
    uint32_t spec_mat_source;   /* D3DRS_SPECULARMATERIALSOURCE: 0=MAT, 1=COLOR1, 2=COLOR2 */
    uint32_t amb_mat_source;    /* D3DRS_AMBIENTMATERIALSOURCE: 0=MAT, 1=COLOR1, 2=COLOR2 */
    uint32_t emiss_mat_source;  /* D3DRS_EMISSIVEMATERIALSOURCE: 0=MAT, 1=COLOR1, 2=COLOR2 */
    /* texture stages / texgen (texturstufen) */
    uint32_t texture_factor;
    float world_matrix[16];
    float view_matrix[16];
    float proj_matrix[16];
    float texture_transforms[8][16];
} d3d9_ffp_desc_t;

int d3d9_ffp_generate(const d3d9_ffp_desc_t *desc, char **out_msl, size_t *out_len, const char **err);

#endif
