/*
 * d3dx9_xfile_robust.c - deterministic sanitizer robustness test for the
 * .x text parser. Only the parser's direct dependencies are linked (no SDL2,
 * no full HLE) because SDL2's dyld constructor opens a modal dialog under
 * ASan. Every input is self-written; broken inputs and non-NUL-terminated
 * buffers are included. Compile with ASan/UBSan and run with a hard timeout.
 */
#define _GNU_SOURCE

/* Only the standalone ASan harness defines this; build_selftests.sh compiles
 * every *.c as core, so an unguarded copy would duplicate dispatch symbols. */
#ifdef D3DX9_XFILE_STANDALONE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "win32hle.h"
#include "d3dx9_xfile.h"

/* Minimal host links for the parser-only harness. parse_check never creates a
 * mesh, so the adjacency writer is never called; a stub keeps the link small. */
const recomp_dispatch_entry_t recomp_dispatch_table[] = { { 0, 0 } };
const uint32_t recomp_dispatch_count = 0;
void d3dx9_mesh_write_adjacency(uint32_t mesh, uint32_t ppAdjacency) {
    (void)mesh; (void)ppAdjacency;
}

static unsigned rng_state = 0x51CE0F17u;
static unsigned rng(void) { rng_state = rng_state * 1103515245u + 12345u; return rng_state >> 8; }

static const char *valid_x =
    "xof 0302txt 0064\n"
    "Mesh {\n 1;\n 0;0;0;;\n 0;\n 3;0,0,0;;\n}\n";

static void case_raw(const char *id, const char *data, size_t len) {
    printf("case %s (len %zu)\n", id, len);
    fflush(stdout);
    d3dx9_xfile_parse_check(data, len);
}

/* A malloc'd buffer of exactly len bytes, with no terminating NUL. */
static void case_nonul(const char *id, const char *src, size_t len) {
    char *buf = (char *)malloc(len ? len : 1);
    if (!buf) return;
    memcpy(buf, src, len);
    case_raw(id, buf, len);
    free(buf);
}

int main(void) {
    case_raw("empty", "", 0);
    case_raw("one byte", "x", 1);

    /* Valid text, with and without its trailing NUL. */
    case_raw("valid with NUL", valid_x, strlen(valid_x) + 1u);
    case_nonul("valid no NUL", valid_x, strlen(valid_x));

    /* Truncated at every possible byte offset: forces strtol/strtod to run
     * right up to the buffer end. */
    size_t n = strlen(valid_x);
    for (size_t k = 1; k < n; k++) case_nonul("trunc", valid_x, k);

    /* Numbers at the very end of a non-NUL buffer (the strtod/strtol case). */
    case_nonul("ends integer", "xof 0302txt 0064\nMesh { 123456", 30);
    case_nonul("ends float", "xof 0302txt 0064\nMesh { 1; 0.12345", 35);

    /* Random bytes and mutated valid text. */
    char buf[512];
    for (int i = 0; i < 200; i++) {
        size_t len = 1 + rng() % 256u;
        for (size_t k = 0; k < len; k++) buf[k] = (char)(rng() & 0xFFu);
        case_raw("random", buf, len);
    }
    for (int i = 0; i < 200; i++) {
        size_t len = strlen(valid_x);
        memcpy(buf, valid_x, len);
        int muts = 1 + (int)(rng() % 4u);
        for (int m = 0; m < muts; m++) buf[rng() % len] = (char)(rng() & 0xFFu);
        case_nonul("mutated", buf, len);
    }
    printf("d3dx9_xfile_robust: done\n");
    return 0;
}
#endif /* D3DX9_XFILE_STANDALONE */
