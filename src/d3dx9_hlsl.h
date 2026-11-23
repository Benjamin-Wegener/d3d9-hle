/*
 * d3dx9_hlsl.h - optional HLSL -> D3D9 bytecode bridge via libvkd3d-shader.
 *
 * vkd3d-shader is loaded dynamically at runtime (dlopen). No vkd3d code or
 * binary is shipped in this repository; the library is LGPL 2.1+ and must be
 * provided by the user/environment. Set HLE_VKD3D_SHADER to the full path of
 * libvkd3d-shader, or place the library under its standard name where dlopen
 * can find it.
 */
#ifndef WIN32HLE_D3DX9_HLSL_H
#define WIN32HLE_D3DX9_HLSL_H

#include <stddef.h>
#include <stdint.h>

/* Result codes mirror vkd3d_result / HRESULT-style failures used by D3DX. */
#define HLE_VKD3D_OK              0
#define HLE_VKD3D_ERR_UNAVAILABLE (-1)  /* library not found / not loadable */
#define HLE_VKD3D_ERR_COMPILE     (-2)  /* compilation failed */
#define HLE_VKD3D_ERR_INVALID     (-3)  /* bad arguments */

typedef struct {
    void *code;      /* malloc'ed by vkd3d-shader; free with hle_vkd3d_free_code */
    size_t size;
} hle_vkd3d_code;

/* A D3DXMACRO-style define: {name, definition}, terminated by {NULL,NULL}. */
typedef struct {
    const char *name;
    const char *definition;
} hle_vkd3d_macro;

/* Include callback. Return 0 on success and fill *data / *size (caller-owned
 * buffer that stays valid until close_include). Return non-zero on failure. */
typedef int  (*hle_vkd3d_open_include)(const char *filename, int local,
                                       const char *parent_data, void *ctx,
                                       const void **data, size_t *size);
typedef void (*hle_vkd3d_close_include)(const void *data, size_t size, void *ctx);

/* Compile HLSL source to legacy D3D9 bytecode (D3D_BYTECODE).
 * entry_point/profile may be NULL to use vkd3d defaults ("main"/source-declared).
 * defines is a NULL-terminated array or NULL. include callbacks may be NULL.
 * flags are passed through as vkd3d compile options where applicable; 0 is fine.
 * On success, out->code must be released with hle_vkd3d_free_code().
 * On any return, *messages (if messages!=NULL) receives a malloc'ed NUL-terminated
 * string or NULL; release with hle_vkd3d_free_messages(). */
int hle_vkd3d_compile_hlsl(const char *source, size_t source_size,
                           const char *source_name,
                           const char *entry_point, const char *profile,
                           const hle_vkd3d_macro *defines, unsigned define_count,
                           hle_vkd3d_open_include open_include,
                           hle_vkd3d_close_include close_include,
                           void *include_ctx,
                           unsigned flags,
                           hle_vkd3d_code *out, char **messages);

/* Preprocess HLSL source. Same define/include conventions as compile. */
int hle_vkd3d_preprocess(const char *source, size_t source_size,
                         const char *source_name,
                         const hle_vkd3d_macro *defines, unsigned define_count,
                         hle_vkd3d_open_include open_include,
                         hle_vkd3d_close_include close_include,
                         void *include_ctx,
                         hle_vkd3d_code *out, char **messages);

void hle_vkd3d_free_code(hle_vkd3d_code *code);
void hle_vkd3d_free_messages(char *messages);

/* ---- DXBC blob API (public vkd3d-shader API, since 1.7) ----
 * Host-side mirrors of the public vkd3d_shader_dxbc_* structs. The guest is a
 * 32-bit PE, so wined3d.c translates these to/from the guest's 32-bit layout.
 * A desc returned by hle_vkd3d_parse_dxbc() must be released with
 * hle_vkd3d_free_dxbc(); a block returned by hle_vkd3d_serialize_dxbc() is
 * owned by vkd3d-shader and can be released with hle_vkd3d_free_code(). */
struct hle_vkd3d_dxbc_section {
    uint32_t tag;
    const void *data;
    size_t size;
};
struct hle_vkd3d_dxbc_desc {
    uint32_t tag;
    uint32_t checksum[4];
    unsigned int version;
    size_t size;
    size_t section_count;
    struct hle_vkd3d_dxbc_section *sections;
};
int  hle_vkd3d_parse_dxbc(const void *data, size_t size, uint32_t flags,
                          struct hle_vkd3d_dxbc_desc *desc, char **messages);
int  hle_vkd3d_serialize_dxbc(size_t section_count,
                              const struct hle_vkd3d_dxbc_section *sections,
                              void **data, size_t *size, char **messages);
void hle_vkd3d_free_dxbc(struct hle_vkd3d_dxbc_desc *desc);
int  hle_vkd3d_dxbc_available(void);

/* Returns 1 if libvkd3d-shader can be loaded, 0 otherwise. */
int hle_vkd3d_available(void);

#endif /* WIN32HLE_D3DX9_HLSL_H */
