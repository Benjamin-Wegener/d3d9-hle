/*
 * d3d9_prim.c - D3D9 primitive topology helpers (see d3d9_prim.h).
 */
#include <stdlib.h>
#include <string.h>
#include "d3d9_prim.h"

static int is_tri_type(uint32_t t)
{
    return t == D3DPT_TRIANGLELIST || t == D3DPT_TRIANGLESTRIP ||
           t == D3DPT_TRIANGLEFAN;
}

uint32_t d3d9_prim_vertex_count(uint32_t prim_type, uint32_t prim_count)
{
    uint64_t n = 0;
    switch (prim_type) {
    case D3DPT_POINTLIST:      n = (uint64_t)prim_count; break;
    case D3DPT_LINELIST:       n = (uint64_t)prim_count * 2u; break;
    case D3DPT_LINESTRIP:      n = (uint64_t)prim_count + 1u; break;
    case D3DPT_TRIANGLELIST:   n = (uint64_t)prim_count * 3u; break;
    case D3DPT_TRIANGLESTRIP:  n = (uint64_t)prim_count + 2u; break;
    case D3DPT_TRIANGLEFAN:    n = (uint64_t)prim_count + 2u; break;
    default: return 0;
    }
    if (n > 0xFFFFFFFFu) return 0;
    return (uint32_t)n;
}

uint32_t d3d9_prim_triangle_count(uint32_t prim_type, uint32_t prim_count)
{
    if (!is_tri_type(prim_type)) return 0;
    if (prim_type == D3DPT_TRIANGLELIST) {
        /* prim_count is number of triangles directly */
        return prim_count;
    }
    /* strip/fan: prim_count triangles */
    return prim_count;
}

int d3d9_prim_read_indices(uint32_t index_stride, const void *src,
                           uint32_t src_count, uint32_t base_vertex,
                           uint32_t num_vertices, uint32_t *dst,
                           uint32_t dst_capacity)
{
    if (!src || !dst) return -1;
    if (index_stride != D3D9_PRIM_IDX16 && index_stride != D3D9_PRIM_IDX32) return -1;
    if (src_count == 0) return 0; /* nothing to do */
    if (dst_capacity < src_count) return -1;
    const uint8_t *p = (const uint8_t *)src;
    for (uint32_t i = 0; i < src_count; i++) {
        uint32_t v;
        if (index_stride == D3D9_PRIM_IDX16) {
            v = (uint32_t)((const uint16_t *)p)[i];
        } else {
            v = ((const uint32_t *)p)[i];
        }
        /* bounds: index must be within the vertex range */
        if (v >= num_vertices) return -1;
        /* overflow check on base vertex addition */
        uint64_t sum = (uint64_t)v + (uint64_t)base_vertex;
        if (sum > 0xFFFFFFFFu) return -1;
        dst[i] = (uint32_t)sum;
    }
    return 0;
}

int d3d9_prim_expand(uint32_t prim_type, const uint32_t *src,
                     uint32_t src_count, uint32_t *dst,
                     uint32_t dst_capacity, uint32_t *out_count)
{
    if (!src || !dst || !out_count) return -1;
    if (!is_tri_type(prim_type)) return -1;

    uint32_t tris;
    if (prim_type == D3DPT_TRIANGLELIST) {
        if (src_count % 3u != 0) return -1;
        tris = src_count / 3u;
    } else {
        if (src_count < 3u) return -1;
        /* prim_count = src_count - 2 */
        uint64_t pc = (uint64_t)src_count - 2u;
        if (pc > 0xFFFFFFFFu) return -1;
        tris = (uint32_t)pc;
    }
    /* overflow: 3 * tris */
    uint64_t need = (uint64_t)tris * 3u;
    if (need > 0xFFFFFFFFu) return -1;
    if (dst_capacity < (uint32_t)need) return -1;

    uint32_t wi = 0;
    if (prim_type == D3DPT_TRIANGLELIST) {
        for (uint32_t i = 0; i < src_count; i++) dst[wi++] = src[i];
    } else if (prim_type == D3DPT_TRIANGLESTRIP) {
        for (uint32_t i = 0; i < tris; i++) {
            if ((i & 1u) == 0) {
                dst[wi++] = src[i];
                dst[wi++] = src[i + 1];
                dst[wi++] = src[i + 2];
            } else {
                dst[wi++] = src[i + 1];
                dst[wi++] = src[i];
                dst[wi++] = src[i + 2];
            }
        }
    } else { /* TRIANGLEFAN */
        for (uint32_t i = 0; i < tris; i++) {
            dst[wi++] = src[0];
            dst[wi++] = src[i + 1];
            dst[wi++] = src[i + 2];
        }
    }
    *out_count = (uint32_t)need;
    return 0;
}

int d3d9_prim_expand_nonindexed(uint32_t prim_type, uint32_t start_vertex,
                                uint32_t prim_count, uint32_t *dst,
                                uint32_t dst_capacity, uint32_t *out_count)
{
    if (!dst || !out_count) return -1;
    if (!is_tri_type(prim_type)) return -1;
    uint32_t tris = d3d9_prim_triangle_count(prim_type, prim_count);
    if (tris == 0) { *out_count = 0; return 0; }
    uint32_t vc = d3d9_prim_vertex_count(prim_type, prim_count);
    if (vc == 0) return -1;
    /* overflow check on start_vertex + vc */
    uint64_t maxv = (uint64_t)start_vertex + (uint64_t)vc;
    if (maxv > 0xFFFFFFFFu) return -1;

    uint32_t *src = (uint32_t *)malloc((size_t)vc * sizeof(uint32_t));
    if (!src && vc > 0) return -1;
    for (uint32_t i = 0; i < vc; i++) src[i] = start_vertex + i;
    int r = d3d9_prim_expand(prim_type, src, vc, dst, dst_capacity, out_count);
    free(src);
    return r;
}

int d3d9_prim_expand_indexed(uint32_t prim_type, const void *indices,
                             uint32_t index_stride, uint32_t start_index,
                             uint32_t prim_count, uint32_t base_vertex,
                             uint32_t num_vertices, uint32_t *dst,
                             uint32_t dst_capacity, uint32_t *out_count)
{
    if (!indices || !dst || !out_count) return -1;
    if (!is_tri_type(prim_type)) return -1;
    uint32_t vc = d3d9_prim_vertex_count(prim_type, prim_count);
    if (vc == 0) {
        if (prim_count == 0) { *out_count = 0; return 0; }
        return -1;
    }
    /* start_index + vc overflow */
    uint64_t end = (uint64_t)start_index + (uint64_t)vc;
    if (end > 0xFFFFFFFFu) return -1;

    /* read indices */
    uint32_t *src = (uint32_t *)malloc((size_t)vc * sizeof(uint32_t));
    if (!src) return -1;
    /* build a pointer to the start in the index buffer */
    const uint8_t *base_ptr;
    if (index_stride == D3D9_PRIM_IDX16) {
        base_ptr = (const uint8_t *)indices + (size_t)start_index * 2u;
    } else if (index_stride == D3D9_PRIM_IDX32) {
        base_ptr = (const uint8_t *)indices + (size_t)start_index * 4u;
    } else {
        free(src);
        return -1;
    }
    int r = d3d9_prim_read_indices(index_stride, base_ptr, vc, base_vertex,
                                   num_vertices, src, vc);
    if (r != 0) { free(src); return -1; }
    r = d3d9_prim_expand(prim_type, src, vc, dst, dst_capacity, out_count);
    free(src);
    return r;
}
