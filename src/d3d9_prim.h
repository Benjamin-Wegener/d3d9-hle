/*
 * d3d9_prim.h - D3D9 primitive topology helpers: expand non-list primitive
 * types (triangle strips and fans) into flat triangle index lists, handle
 * 16- and 32-bit index buffers, apply BaseVertexIndex, and enforce bounds
 * and overflow checks.
 *
 * The Metal backend draws triangle lists (and strips) directly; triangle fans
 * (and any caller that wants a uniform representation) are converted to lists
 * through these helpers. All arithmetic is checked for overflow; any invalid
 * argument, out-of-range index, capacity shortfall or overflow yields a
 * non-zero (error) result and leaves the output untouched.
 */
#ifndef WIN32HLE_D3D9_PRIM_H
#define WIN32HLE_D3D9_PRIM_H

#include <stdint.h>
#include "d3d9_hle.h"

/* Index buffer element sizes. */
#define D3D9_PRIM_IDX16  2u
#define D3D9_PRIM_IDX32  4u

/* Number of source vertices consumed by one primitive run of `prim_count`
 * primitives of the given type. 0 on invalid type or overflow. */
uint32_t d3d9_prim_vertex_count(uint32_t prim_type, uint32_t prim_count);

/* Number of triangles such a run produces (0 for point/line types). */
uint32_t d3d9_prim_triangle_count(uint32_t prim_type, uint32_t prim_count);

/* Read `src_count` indices from a 16- or 32-bit index buffer (src), add
 * `base_vertex` to each, bounds-check against `num_vertices`, and write the
 * resolved indices to dst[0..src_count-1]. index_stride is 2 or 4.
 * Returns 0 on success, -1 on any error (bad stride, null ptr, overflow,
 * out-of-range index, capacity shortfall). */
int d3d9_prim_read_indices(uint32_t index_stride, const void *src,
                           uint32_t src_count, uint32_t base_vertex,
                           uint32_t num_vertices, uint32_t *dst,
                           uint32_t dst_capacity);

/* Expand an already-resolved vertex-index array src[0..src_count-1] (one
 * primitive run) into a triangle list in dst (3 indices per triangle).
 * dst_capacity is in indices. *out_count receives the number of output
 * indices (3 * triangle_count) on success. Returns 0 on success, -1 on
 * invalid type/count/capacity/overflow. */
int d3d9_prim_expand(uint32_t prim_type, const uint32_t *src,
                     uint32_t src_count, uint32_t *dst,
                     uint32_t dst_capacity, uint32_t *out_count);

/* Convenience: non-indexed primitive run starting at `start_vertex`. Expands
 * to a triangle list in dst. Returns 0 on success, -1 on error. */
int d3d9_prim_expand_nonindexed(uint32_t prim_type, uint32_t start_vertex,
                                uint32_t prim_count, uint32_t *dst,
                                uint32_t dst_capacity, uint32_t *out_count);

/* Convenience: indexed primitive run. Reads indices from `indices` starting
 * at `start_index` (in index units), applies `base_vertex`, bounds-checks
 * against `num_vertices`, expands to a triangle list in dst.
 * Returns 0 on success, -1 on error. */
int d3d9_prim_expand_indexed(uint32_t prim_type, const void *indices,
                             uint32_t index_stride, uint32_t start_index,
                             uint32_t prim_count, uint32_t base_vertex,
                             uint32_t num_vertices, uint32_t *dst,
                             uint32_t dst_capacity, uint32_t *out_count);

#endif /* WIN32HLE_D3D9_PRIM_H */
