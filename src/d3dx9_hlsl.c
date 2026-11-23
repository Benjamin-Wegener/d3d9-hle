/*
 * d3dx9_hlsl.c - optional HLSL -> D3D9 bytecode via libvkd3d-shader (dlopen).
 *
 * No vkd3d code or binaries are included in this repository. The library is
 * loaded at runtime from HLE_VKD3D_SHADER or a standard soname. Minimal ABI
 * structs/enums below mirror the public vkd3d-shader API (vkd3d_shader.h) so we
 * can call the dynamic library without bundling its headers.
 */
#include "d3dx9_hlsl.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- minimal vkd3d-shader ABI (public API) ---- */
enum {
    VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO = 0,
    VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO = 6,
    VKD3D_SHADER_STRUCTURE_TYPE_PREPROCESS_INFO = 7,
};
enum {
    VKD3D_SHADER_SOURCE_HLSL = 2,
    VKD3D_SHADER_TARGET_D3D_BYTECODE = 4,
};
enum {
    VKD3D_SHADER_LOG_NONE = 0,
    VKD3D_SHADER_LOG_ERROR = 1,
    VKD3D_SHADER_LOG_WARNING = 2,
    VKD3D_SHADER_LOG_INFO = 3,
};
enum {
    VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_ORDER = 0x00000005,
};
enum {
    VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_ROW_MAJOR = 1,
    VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_COLUMN_MAJOR = 2,
};

struct vkd3d_shader_code {
    const void *code;
    size_t size;
};
struct vkd3d_shader_compile_option {
    unsigned int name;
    unsigned int value;
};
struct vkd3d_shader_compile_info {
    unsigned int type;
    const void *next;
    struct vkd3d_shader_code source;
    unsigned int source_type;
    unsigned int target_type;
    const struct vkd3d_shader_compile_option *options;
    unsigned int option_count;
    unsigned int log_level;
    const char *source_name;
};
struct vkd3d_shader_hlsl_source_info {
    unsigned int type;
    const void *next;
    const char *entry_point;
    struct vkd3d_shader_code secondary_code;
    const char *profile;
};
struct vkd3d_shader_macro {
    const char *name;
    const char *value;
};
struct vkd3d_shader_dxbc_section_desc {
    uint32_t tag;
    struct vkd3d_shader_code data;
};
struct vkd3d_shader_dxbc_desc {
    uint32_t tag;
    uint32_t checksum[4];
    unsigned int version;
    size_t size;
    size_t section_count;
    struct vkd3d_shader_dxbc_section_desc *sections;
};
typedef int (*PFN_vkd3d_shader_open_include)(const char *filename, bool local,
        const char *parent_data, void *context, struct vkd3d_shader_code *out);
typedef void (*PFN_vkd3d_shader_close_include)(const struct vkd3d_shader_code *code, void *context);
struct vkd3d_shader_preprocess_info {
    unsigned int type;
    const void *next;
    const struct vkd3d_shader_macro *macros;
    unsigned int macro_count;
    PFN_vkd3d_shader_open_include pfn_open_include;
    PFN_vkd3d_shader_close_include pfn_close_include;
    void *include_context;
};

typedef int (*pfn_vkd3d_shader_compile)(const struct vkd3d_shader_compile_info *compile_info,
        struct vkd3d_shader_code *out, char **messages);
typedef int (*pfn_vkd3d_shader_preprocess)(const struct vkd3d_shader_compile_info *compile_info,
        struct vkd3d_shader_code *out, char **messages);
typedef void (*pfn_vkd3d_shader_free_messages)(char *messages);
typedef void (*pfn_vkd3d_shader_free_shader_code)(struct vkd3d_shader_code *code);
typedef int (*pfn_vkd3d_shader_parse_dxbc)(const struct vkd3d_shader_code *dxbc,
        uint32_t flags, struct vkd3d_shader_dxbc_desc *desc, char **messages);
typedef int (*pfn_vkd3d_shader_serialize_dxbc)(size_t section_count,
        const struct vkd3d_shader_dxbc_section_desc *sections,
        struct vkd3d_shader_code *dxbc, char **messages);
typedef void (*pfn_vkd3d_shader_free_dxbc)(struct vkd3d_shader_dxbc_desc *desc);

/* ---- dynamic loader ---- */
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void *g_lib;
static pfn_vkd3d_shader_compile g_compile;
static pfn_vkd3d_shader_preprocess g_preprocess;
static pfn_vkd3d_shader_free_messages g_free_messages;
static pfn_vkd3d_shader_free_shader_code g_free_code;
static pfn_vkd3d_shader_parse_dxbc g_parse_dxbc;
static pfn_vkd3d_shader_serialize_dxbc g_serialize_dxbc;
static pfn_vkd3d_shader_free_dxbc g_free_dxbc;
static int g_load_ok; /* -1 unavailable, 1 ok */

static void do_load(void) {
    const char *path = getenv("HLE_VKD3D_SHADER");
    const char *names[] = { path, "libvkd3d-shader.1.dylib", "libvkd3d-shader.dylib",
                            "libvkd3d-shader.so.1", "libvkd3d-shader.so", NULL };
    size_t i;
    for (i = 0; names[i]; i++) {
        if (!names[i] || !names[i][0]) continue;
        g_lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (g_lib) break;
    }
    if (!g_lib) {
        fprintf(stderr, "[d3dx9_hlsl] libvkd3d-shader unavailable: %s\n", dlerror());
        g_load_ok = -1;
        return;
    }
    g_compile = (pfn_vkd3d_shader_compile)dlsym(g_lib, "vkd3d_shader_compile");
    g_preprocess = (pfn_vkd3d_shader_preprocess)dlsym(g_lib, "vkd3d_shader_preprocess");
    g_free_messages = (pfn_vkd3d_shader_free_messages)dlsym(g_lib, "vkd3d_shader_free_messages");
    g_free_code = (pfn_vkd3d_shader_free_shader_code)dlsym(g_lib, "vkd3d_shader_free_shader_code");
    /* DXBC API (vkd3d-shader >= 1.7). Optional: its absence only disables the
     * dxbc shims, never the HLSL compile path. */
    g_parse_dxbc = (pfn_vkd3d_shader_parse_dxbc)dlsym(g_lib, "vkd3d_shader_parse_dxbc");
    g_serialize_dxbc = (pfn_vkd3d_shader_serialize_dxbc)dlsym(g_lib, "vkd3d_shader_serialize_dxbc");
    g_free_dxbc = (pfn_vkd3d_shader_free_dxbc)dlsym(g_lib, "vkd3d_shader_free_dxbc");
    if (!g_compile || !g_free_messages || !g_free_code) {
        fprintf(stderr, "[d3dx9_hlsl] libvkd3d-shader missing required symbols\n");
        dlclose(g_lib);
        g_lib = NULL;
        g_compile = NULL;
        g_preprocess = NULL;
        g_free_messages = NULL;
        g_free_code = NULL;
        g_parse_dxbc = NULL;
        g_serialize_dxbc = NULL;
        g_free_dxbc = NULL;
        g_load_ok = -1;
        return;
    }
    g_load_ok = 1;
}

static int ensure_loaded(void) {
    pthread_once(&g_once, do_load);
    return g_load_ok == 1;
}

int hle_vkd3d_available(void) {
    return ensure_loaded();
}

/* ---- include adapter ---- */
struct include_ctx {
    hle_vkd3d_open_include open;
    hle_vkd3d_close_include close;
    void *user;
};

static int open_include_cb(const char *filename, bool local,
        const char *parent_data, void *context, struct vkd3d_shader_code *out) {
    struct include_ctx *ic = (struct include_ctx *)context;
    const void *data = NULL;
    size_t size = 0;
    int rc;
    if (!ic || !ic->open) return -1;
    rc = ic->open(filename, local, parent_data, ic->user, &data, &size);
    if (rc != 0) return rc;
    out->code = data;
    out->size = size;
    return 0;
}

static void close_include_cb(const struct vkd3d_shader_code *code, void *context) {
    struct include_ctx *ic = (struct include_ctx *)context;
    if (ic && ic->close && code)
        ic->close(code->code, code->size, ic->user);
}

void hle_vkd3d_free_messages(char *messages) {
    if (!messages) return;
    if (ensure_loaded() && g_free_messages) g_free_messages(messages);
    else free(messages);
}

void hle_vkd3d_free_code(hle_vkd3d_code *code) {
    struct vkd3d_shader_code tmp;
    if (!code || !code->code) return;
    tmp.code = code->code;
    tmp.size = code->size;
    if (ensure_loaded() && g_free_code) g_free_code(&tmp);
    else free(code->code);
    code->code = NULL;
    code->size = 0;
}

int hle_vkd3d_compile_hlsl(const char *source, size_t source_size,
                           const char *source_name,
                           const char *entry_point, const char *profile,
                           const hle_vkd3d_macro *defines, unsigned define_count,
                           hle_vkd3d_open_include open_include,
                           hle_vkd3d_close_include close_include,
                           void *include_ctx,
                           unsigned flags,
                           hle_vkd3d_code *out, char **messages) {
    struct vkd3d_shader_compile_info info;
    struct vkd3d_shader_hlsl_source_info hlsl;
    struct vkd3d_shader_preprocess_info pre;
    struct vkd3d_shader_compile_option opt[1];
    unsigned int opt_count = 0;
    struct vkd3d_shader_macro *macros = NULL;
    struct include_ctx ic;
    struct vkd3d_shader_code vk_out;
    char *vk_messages = NULL;
    int rc;

    if (messages) *messages = NULL;
    if (out) { out->code = NULL; out->size = 0; }
    if (!source || !profile || !out) return HLE_VKD3D_ERR_INVALID;
    if (!ensure_loaded()) return HLE_VKD3D_ERR_UNAVAILABLE;

    memset(&info, 0, sizeof info);
    memset(&hlsl, 0, sizeof hlsl);
    memset(&pre, 0, sizeof pre);
    memset(&ic, 0, sizeof ic);
    memset(&vk_out, 0, sizeof vk_out);

    if (define_count && defines) {
        unsigned i;
        macros = (struct vkd3d_shader_macro *)calloc(define_count, sizeof *macros);
        if (!macros) return HLE_VKD3D_ERR_INVALID;
        for (i = 0; i < define_count; i++) {
            macros[i].name = defines[i].name;
            macros[i].value = defines[i].definition;
        }
    }

    pre.type = VKD3D_SHADER_STRUCTURE_TYPE_PREPROCESS_INFO;
    pre.next = NULL;
    pre.macros = macros;
    pre.macro_count = define_count;
    if (open_include) {
        ic.open = open_include;
        ic.close = close_include;
        ic.user = include_ctx;
        pre.pfn_open_include = open_include_cb;
        pre.pfn_close_include = close_include_cb;
        pre.include_context = &ic;
    }

    hlsl.type = VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO;
    hlsl.next = &pre;
    hlsl.entry_point = entry_point ? entry_point : "main";
    hlsl.profile = profile;

    /* Map D3DXSHADER_PACKMATRIX flags (0/1=row-major, 2=column-major). */
    if ((flags & 3u) == 1u || (flags & 3u) == 2u) {
        opt[0].name = VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_ORDER;
        opt[0].value = (flags & 3u) == 1u ? VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_ROW_MAJOR
                                           : VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_COLUMN_MAJOR;
        opt_count = 1;
    }

    info.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
    info.next = &hlsl;
    info.source.code = source;
    info.source.size = source_size;
    info.source_type = VKD3D_SHADER_SOURCE_HLSL;
    info.target_type = VKD3D_SHADER_TARGET_D3D_BYTECODE;
    info.options = opt_count ? opt : NULL;
    info.option_count = opt_count;
    info.log_level = VKD3D_SHADER_LOG_INFO;
    info.source_name = source_name;

    rc = g_compile(&info, &vk_out, &vk_messages);
    free(macros);

    if (messages) *messages = vk_messages;
    else if (vk_messages) g_free_messages(vk_messages);

    if (rc != 0) {
        if (vk_out.code) {
            struct vkd3d_shader_code tmp = vk_out;
            g_free_code(&tmp);
        }
        return HLE_VKD3D_ERR_COMPILE;
    }

    /* Copy bytecode to a malloc buffer owned by us, free vkd3d's buffer. */
    out->size = vk_out.size;
    out->code = malloc(vk_out.size ? vk_out.size : 1);
    if (!out->code) {
        struct vkd3d_shader_code tmp = vk_out;
        g_free_code(&tmp);
        out->size = 0;
        return HLE_VKD3D_ERR_INVALID;
    }
    if (vk_out.size) memcpy(out->code, vk_out.code, vk_out.size);
    {
        struct vkd3d_shader_code tmp = vk_out;
        g_free_code(&tmp);
    }
    return HLE_VKD3D_OK;
}

int hle_vkd3d_preprocess(const char *source, size_t source_size,
                         const char *source_name,
                         const hle_vkd3d_macro *defines, unsigned define_count,
                         hle_vkd3d_open_include open_include,
                         hle_vkd3d_close_include close_include,
                         void *include_ctx,
                         hle_vkd3d_code *out, char **messages) {
    struct vkd3d_shader_compile_info info;
    struct vkd3d_shader_preprocess_info pre;
    struct vkd3d_shader_macro *macros = NULL;
    struct include_ctx ic;
    struct vkd3d_shader_code vk_out;
    char *vk_messages = NULL;
    int rc;

    if (messages) *messages = NULL;
    if (out) { out->code = NULL; out->size = 0; }
    if (!source || !out) return HLE_VKD3D_ERR_INVALID;
    if (!ensure_loaded()) return HLE_VKD3D_ERR_UNAVAILABLE;
    if (!g_preprocess) return HLE_VKD3D_ERR_UNAVAILABLE;

    memset(&info, 0, sizeof info);
    memset(&pre, 0, sizeof pre);
    memset(&ic, 0, sizeof ic);
    memset(&vk_out, 0, sizeof vk_out);

    if (define_count && defines) {
        unsigned i;
        macros = (struct vkd3d_shader_macro *)calloc(define_count, sizeof *macros);
        if (!macros) return HLE_VKD3D_ERR_INVALID;
        for (i = 0; i < define_count; i++) {
            macros[i].name = defines[i].name;
            macros[i].value = defines[i].definition;
        }
    }

    pre.type = VKD3D_SHADER_STRUCTURE_TYPE_PREPROCESS_INFO;
    pre.next = NULL;
    pre.macros = macros;
    pre.macro_count = define_count;
    if (open_include) {
        ic.open = open_include;
        ic.close = close_include;
        ic.user = include_ctx;
        pre.pfn_open_include = open_include_cb;
        pre.pfn_close_include = close_include_cb;
        pre.include_context = &ic;
    }

    /* vkd3d_shader_validate_compile_info() requires COMPILE_INFO here even for
     * the preprocess entry point; PREPROCESS_INFO only describes the chained
     * pre structure. Using 7 here made vkd3d_shader_preprocess fail with
     * INVALID_ARGUMENT before it ever saw the source. */
    info.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
    info.next = &pre;
    info.source.code = source;
    info.source.size = source_size;
    info.source_type = VKD3D_SHADER_SOURCE_HLSL;
    info.log_level = VKD3D_SHADER_LOG_INFO;
    info.source_name = source_name;

    rc = g_preprocess(&info, &vk_out, &vk_messages);
    free(macros);

    if (messages) *messages = vk_messages;
    else if (vk_messages) g_free_messages(vk_messages);

    if (rc != 0) {
        if (vk_out.code) { struct vkd3d_shader_code tmp = vk_out; g_free_code(&tmp); }
        return HLE_VKD3D_ERR_COMPILE;
    }

    out->size = vk_out.size;
    out->code = malloc(vk_out.size ? vk_out.size : 1);
    if (!out->code) {
        struct vkd3d_shader_code tmp = vk_out; g_free_code(&tmp);
        out->size = 0;
        return HLE_VKD3D_ERR_INVALID;
    }
    if (vk_out.size) memcpy(out->code, vk_out.code, vk_out.size);
    { struct vkd3d_shader_code tmp = vk_out; g_free_code(&tmp); }
    return HLE_VKD3D_OK;
}

/* ---- DXBC blob API -------------------------------------------------------
 * Public API as documented in vkd3d_shader.h (vkd3d 1.7+). The mirrored host
 * structs have the same layout as the library's; the guest-side 32-bit layout
 * is translated by wined3d.c. */
typedef char hle_dxbc_section_layout[
    sizeof(struct hle_vkd3d_dxbc_section) == sizeof(struct vkd3d_shader_dxbc_section_desc) ? 1 : -1];
typedef char hle_dxbc_desc_layout[
    sizeof(struct hle_vkd3d_dxbc_desc) == sizeof(struct vkd3d_shader_dxbc_desc) ? 1 : -1];

int hle_vkd3d_dxbc_available(void) {
    return ensure_loaded() && g_parse_dxbc && g_serialize_dxbc && g_free_dxbc;
}

int hle_vkd3d_parse_dxbc(const void *data, size_t size, uint32_t flags,
                         struct hle_vkd3d_dxbc_desc *desc, char **messages) {
    struct vkd3d_shader_code code;
    if (!hle_vkd3d_dxbc_available() || !desc) return HLE_VKD3D_ERR_UNAVAILABLE;
    code.code = data;
    code.size = size;
    return g_parse_dxbc(&code, flags, (struct vkd3d_shader_dxbc_desc *)desc, messages);
}

int hle_vkd3d_serialize_dxbc(size_t section_count,
                             const struct hle_vkd3d_dxbc_section *sections,
                             void **data, size_t *size, char **messages) {
    struct vkd3d_shader_dxbc_section_desc *vs;
    struct vkd3d_shader_code out;
    int rc;
    size_t i;
    if (!hle_vkd3d_dxbc_available() || !data || !size) return HLE_VKD3D_ERR_UNAVAILABLE;
    if (section_count && !sections) return HLE_VKD3D_ERR_INVALID;
    vs = (struct vkd3d_shader_dxbc_section_desc *)calloc(
            section_count ? section_count : 1, sizeof *vs);
    if (!vs) return HLE_VKD3D_ERR_INVALID;
    for (i = 0; i < section_count; i++) {
        vs[i].tag = sections[i].tag;
        vs[i].data.code = sections[i].data;
        vs[i].data.size = sections[i].size;
    }
    memset(&out, 0, sizeof out);
    rc = g_serialize_dxbc(section_count, vs, &out, messages);
    free(vs);
    if (rc == 0) { *data = (void *)out.code; *size = out.size; }
    return rc;
}

void hle_vkd3d_free_dxbc(struct hle_vkd3d_dxbc_desc *desc) {
    if (!desc) return;
    if (hle_vkd3d_dxbc_available()) g_free_dxbc((struct vkd3d_shader_dxbc_desc *)desc);
    else free(desc->sections);   /* fallback for a desc built by our own code */
    desc->sections = NULL;
    desc->section_count = 0;
}
