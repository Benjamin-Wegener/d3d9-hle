#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "win32hle.h"
#include "host.h"
#include "d3d9_hle.h"
#include "d3d9_prim.h"

const recomp_dispatch_entry_t recomp_dispatch_table[1];
const uint32_t recomp_dispatch_count = 0;

static int fails;
static void ok(const char *what, int cond) { if (!cond) { printf("FAIL %s\n", what); fails++; } }

int main(void)
{
    recomp_host_init();

    /* ---- vertex/triangle counts ---- */
    ok("vc fan 4 tris? prim_count=4 -> 6 verts", d3d9_prim_vertex_count(D3DPT_TRIANGLEFAN, 4) == 6);
    ok("tc fan 4 -> 4 tris", d3d9_prim_triangle_count(D3DPT_TRIANGLEFAN, 4) == 4);
    ok("vc strip 4 -> 6", d3d9_prim_vertex_count(D3DPT_TRIANGLESTRIP, 4) == 6);
    ok("tc strip 4 -> 4", d3d9_prim_triangle_count(D3DPT_TRIANGLESTRIP, 4) == 4);
    ok("vc list 4 -> 12", d3d9_prim_vertex_count(D3DPT_TRIANGLELIST, 4) == 12);
    ok("tc list 4 -> 4", d3d9_prim_triangle_count(D3DPT_TRIANGLELIST, 4) == 4);
    ok("overflow vc returns 0", d3d9_prim_vertex_count(D3DPT_TRIANGLELIST, 0xFFFFFFFFu) == 0);

    /* ---- fan expansion (non-indexed) ---- */
    {
        uint32_t dst[12];
        uint32_t out = 0;
        int r = d3d9_prim_expand_nonindexed(D3DPT_TRIANGLEFAN, 10, 2, dst, 12, &out);
        ok("fan nonindexed ok", r == 0);
        ok("fan nonindexed out count", out == 6);
        /* tris: (10,11,12),(10,12,13) */
        ok("fan nonindexed t0", dst[0]==10 && dst[1]==11 && dst[2]==12);
        ok("fan nonindexed t1", dst[3]==10 && dst[4]==12 && dst[5]==13);
    }

    /* ---- strip winding ---- */
    {
        uint32_t src[6] = { 0,1,2,3,4,5 };
        uint32_t dst[12];
        uint32_t out = 0;
        int r = d3d9_prim_expand(D3DPT_TRIANGLESTRIP, src, 6, dst, 12, &out);
        ok("strip expand2 ok", r == 0);
        ok("strip out count2", out == 12);
        /* tri0 even: 0,1,2 ; tri1 odd: 2? wait formula: odd i: (i+1, i, i+2) with i=1 -> src[2],src[1],src[3] = 2,1,3 */
        ok("strip t0", dst[0]==0 && dst[1]==1 && dst[2]==2);
        ok("strip t1", dst[3]==2 && dst[4]==1 && dst[5]==3);
        ok("strip t2", dst[6]==2 && dst[7]==3 && dst[8]==4);
        ok("strip t3", dst[9]==4 && dst[10]==3 && dst[11]==5);
    }

    /* ---- fan expansion indexed ---- */
    {
        uint16_t ib2[5] = { 10, 20, 30, 40, 50 };
        uint32_t dst[9];
        uint32_t out = 0;
        int r = d3d9_prim_expand_indexed(D3DPT_TRIANGLEFAN, ib2, D3D9_PRIM_IDX16, 0, 3,
                                         5, 100, dst, 9, &out);
        ok("fan indexed ok", r == 0);
        ok("fan indexed out", out == 9);
        /* src after base+5: 15,25,35,45,55? wait base 5 -> 15,25,35,45,55; fan tris: (15,25,35),(15,35,45) */
        ok("fan idx t0", dst[0]==15 && dst[1]==25 && dst[2]==35);
        ok("fan idx t1", dst[3]==15 && dst[4]==35 && dst[5]==45);
    }

    /* ---- 32-bit indices ---- */
    {
        uint32_t ib[4] = { 0, 1, 2, 3 };
        uint32_t dst[3];
        uint32_t out = 0;
        int r = d3d9_prim_expand_indexed(D3DPT_TRIANGLELIST, ib, D3D9_PRIM_IDX32, 0, 1,
                                         100, 200, dst, 3, &out);
        ok("list32 ok", r == 0);
        ok("list32 out", out == 3);
        ok("list32 vals", dst[0]==100 && dst[1]==101 && dst[2]==102);
    }

    /* ---- base vertex overflow ---- */
    {
        uint16_t ib[3] = { 0, 1, 2 };
        uint32_t dst[3];
        uint32_t out = 0;
        int r = d3d9_prim_expand_indexed(D3DPT_TRIANGLELIST, ib, D3D9_PRIM_IDX16, 0, 1,
                                         0xFFFFFFFFu, 10, dst, 3, &out);
        ok("overflow base rejected", r != 0);
    }

    /* ---- out of range index ---- */
    {
        uint16_t ib[3] = { 0, 1, 5 };
        uint32_t dst[3];
        uint32_t out = 0;
        int r = d3d9_prim_expand_indexed(D3DPT_TRIANGLELIST, ib, D3D9_PRIM_IDX16, 0, 1,
                                         0, 3, dst, 3, &out);
        ok("oob index rejected", r != 0);
    }

    /* ---- bad stride ---- */
    {
        uint8_t ib[6] = { 0 };
        uint32_t dst[3];
        uint32_t out = 0;
        int r = d3d9_prim_expand_indexed(D3DPT_TRIANGLELIST, ib, 3, 0, 1,
                                         0, 3, dst, 3, &out);
        ok("bad stride rejected", r != 0);
    }

    /* ---- read_indices directly ---- */
    {
        uint16_t ib[3] = { 0, 1, 2 };
        uint32_t dst[3];
        int r = d3d9_prim_read_indices(D3D9_PRIM_IDX16, ib, 3, 10, 5, dst, 3);
        ok("read_indices ok", r == 0);
        ok("read_indices vals", dst[0]==10 && dst[1]==11 && dst[2]==12);
    }

    /* ---- capacity shortfall ---- */
    {
        uint32_t src[3] = { 0,1,2 };
        uint32_t dst[2];
        uint32_t out = 0;
        int r = d3d9_prim_expand(D3DPT_TRIANGLELIST, src, 3, dst, 2, &out);
        ok("capacity shortfall rejected", r != 0);
    }

    /* ---- fan invalid (too few verts) ---- */
    {
        uint32_t src[2] = { 0,1 };
        uint32_t dst[3];
        uint32_t out = 0;
        int r = d3d9_prim_expand(D3DPT_TRIANGLEFAN, src, 2, dst, 3, &out);
        ok("fan too few rejected", r != 0);
    }

    /* ---- non-indexed start overflow ---- */
    {
        uint32_t dst[3];
        uint32_t out = 0;
        int r = d3d9_prim_expand_nonindexed(D3DPT_TRIANGLELIST, 0xFFFFFFFEu, 1, dst, 3, &out);
        ok("nonindexed overflow rejected", r != 0);
    }

    if (!fails) printf("d3d9_prim_selftest: all checks passed\n");
    return fails != 0;
}
