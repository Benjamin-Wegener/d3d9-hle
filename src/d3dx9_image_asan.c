/*
 * d3dx9_image_asan.c - host-only robustness harness for the DDS/BMP/TGA
 * decoders. No SDL2, no full HLE: links only d3dx9_texture.c and
 * d3dx9_format.c (plus tiny stubs) so ASan/UBSan can inspect the parsers.
 * Every input is self-written and derived from a valid file's layout.
 */
#define _GNU_SOURCE

/* Only the standalone ASan harness defines this; build_selftests.sh compiles
 * every *.c as core, so an unguarded copy would duplicate dispatch symbols. */
#ifdef D3DX9_IMAGE_STANDALONE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "win32hle.h"
#include "d3dx9_texture.h"

const recomp_dispatch_entry_t recomp_dispatch_table[] = { { 0, 0 } };
const uint32_t recomp_dispatch_count = 0;

static unsigned rng_state = 0x51CE0F17u;
static unsigned rng(void) { rng_state = rng_state * 1103515245u + 12345u; return rng_state >> 8; }
static void put32(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void put16(uint8_t *p, uint16_t v) { p[0]=v; p[1]=v>>8; }

/* Minimal valid 4x4 A8R8G8B8 DDS header + one 64-byte top level. */
static void make_dds(uint8_t *d) {
    memset(d, 0, 128 + 64);
    d[0]='D'; d[1]='D'; d[2]='S'; d[3]=' ';
    put32(d+4, 124);
    put32(d+12, 4); put32(d+16, 4);          /* height, width */
    put32(d+76, 32);                          /* DDPF_RGB size */
    put32(d+80, 0x41);                        /* ALPHAPIXELS|RGB */
    put32(d+88, 32);                          /* RGBBitCount */
    put32(d+92, 0x00FF0000); put32(d+96, 0x0000FF00);
    put32(d+100, 0x000000FF); put32(d+104, 0xFF000000);
}
/* Minimal valid 2x2 24-bit BMP (54-byte header + 8 bytes rows). */
static void make_bmp(uint8_t *d) {
    memset(d, 0, 62);
    d[0]='B'; d[1]='M'; put32(d+10, 54);
    put32(d+14, 40); put32(d+18, 2); put32(d+22, 2);
    put16(d+26, 1); put16(d+28, 24);
}
/* Minimal valid 2x2 24-bit TGA (18-byte header + 12 bytes pixels). */
static void make_tga(uint8_t *d) {
    memset(d, 0, 30);
    d[2]=2; put16(d+12, 2); put16(d+14, 2); d[16]=24; d[17]=0x20;
}

static void probe(const char *id, const uint8_t *d, uint32_t n) {
    d3dx9_texture_info info;
    const uint8_t *top; uint32_t pitch, fmt; uint8_t *rgba;
    d3dx9_texture_get_image_info(d, n, &info);
    d3dx9_texture_decode_top_level(d, n, &info, &top, &pitch, &fmt, &rgba);
    free(rgba);
    (void)id;
}

int main(void) {
    uint8_t buf[70000];
    uint8_t *dds = (uint8_t *)malloc(128 + 64), *bmp = (uint8_t *)malloc(62), *tga = (uint8_t *)malloc(30);
    make_dds(dds); make_bmp(bmp); make_tga(tga);

    probe("empty", buf, 0);
    probe("one byte", buf, 1);
    probe("DDS valid", dds, 192);
    probe("BMP valid", bmp, 62);
    probe("TGA valid", tga, 30);

    /* Truncation at every byte of each valid file. */
    for (uint32_t k = 1; k < 192; k++) probe("DDS truncated", dds, k);
    for (uint32_t k = 1; k < 62; k++) probe("BMP truncated", bmp, k);
    for (uint32_t k = 1; k < 30; k++) probe("TGA truncated", tga, k);

    /* Huge dimensions and offsets in otherwise valid headers. */
    for (int which = 0; which < 3; which++) {
        for (int it = 0; it < 200; it++) {
            memcpy(buf, which==0?dds:which==1?bmp:tga, which==0?192:which==1?62:30);
            uint32_t n = which==0?192:which==1?62:30;
            uint32_t count = 1 + rng() % 6u;
            for (uint32_t i = 0; i < count; i++) {
                uint32_t at = rng() % n;
                buf[at] = (uint8_t)(rng() & 0xFFu);
            }
            probe("mutated header", buf, n);
        }
    }
    /* Huge declared dimensions with a tiny payload. */
    memcpy(buf, dds, 128); put32(buf+12, 0xFFFFFFFFu); put32(buf+16, 0xFFFFFFFFu);
    probe("DDS huge dims", buf, 128);
    memcpy(buf, bmp, 54); put32(buf+18, 0x7FFFFFFFu); put32(buf+22, 0x7FFFFFFFu);
    probe("BMP huge dims", buf, 54);
    put32(buf+10, 0xFFFFFFF0u); probe("BMP huge offset", buf, 54);
    memcpy(buf, tga, 18); put16(buf+12, 0xFFFFu); put16(buf+14, 0xFFFFu); buf[16]=32;
    probe("TGA huge dims", buf, 18);

    /* Random blobs and a large garbage file with a valid magic. */
    for (int i = 0; i < 300; i++) {
        uint32_t n = 1 + rng() % 512u;
        for (uint32_t k = 0; k < n; k++) buf[k] = (uint8_t)(rng() & 0xFFu);
        probe("random", buf, n);
    }
    memset(buf, 0x5A, sizeof buf);
    buf[0]='D'; buf[1]='D'; buf[2]='S'; buf[3]=' ';
    probe("64K garbage DDS", buf, sizeof buf);

    free(dds); free(bmp); free(tga);
    printf("d3dx9_image_asan: done\n");
    return 0;
}
#endif /* D3DX9_IMAGE_STANDALONE */
