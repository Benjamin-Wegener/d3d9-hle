/*
 * d3d9_perf.c - the seven public D3DPERF_* profiling exports of d3d9.dll.
 *
 * These are plain WINAPI (stdcall) functions, not COM methods: there is no
 * `this` argument. Signatures come from the public Wine 11.0 header
 * wine-11.0/include/d3d9.h (lines 2210-2216), never from a Microsoft DLL:
 *   int    D3DPERF_BeginEvent(D3DCOLOR, const WCHAR *);
 *   int    D3DPERF_EndEvent(void);
 *   DWORD  D3DPERF_GetStatus(void);
 *   BOOL   D3DPERF_QueryRepeatFrame(void);
 *   void   D3DPERF_SetMarker(D3DCOLOR, const WCHAR *);
 *   void   D3DPERF_SetOptions(DWORD);
 *   void   D3DPERF_SetRegion(D3DCOLOR, const WCHAR *);
 *
 * The HLE has no PIX/GPU capture, so no capture is pretended: every call logs
 * what the guest asked for, BeginEvent/EndEvent track a deterministic nesting
 * level (zero-based, negative on an unmatched end) exactly as the public docs
 * and Wine 11.0 d3d9_main.c specify, GetStatus reports "no profiling tool
 * attached" and QueryRepeatFrame reports FALSE. No call silently claims that
 * a capture was made.
 */
#include "d3d9_internal.h"

/* Public docs: BeginEvent returns the zero-based level the event starts in,
 * EndEvent the level it ends in (negative on an unmatched end). Wine 11.0's
 * d3d9_main.c uses the same post-increment / pre-decrement counter. */
static int32_t g_event_level;

static void perf_name(uint32_t va, char *out, uint32_t cap) {
    if (!cap) return;
    out[0] = 0;
    if (!va) return;
    const uint16_t *w = (const uint16_t *)hle_ptr(va);
    uint32_t i;
    for (i = 0; i + 1 < cap && i < 128; i++) {
        uint16_t c = w[i];
        if (!c) break;
        out[i] = c < 128 ? (char)c : '?';   /* names are ASCII in practice */
    }
    out[i] = 0;
}

void perf_BeginEvent(void) {
    uint32_t color = A32(0), name = A32(1);
    char buf[128];
    perf_name(name, buf, sizeof buf);
    /* Post-increment: return the zero-based level this event starts in. */
    int32_t level = g_event_level++;
    fprintf(stderr, "[d3d9] D3DPERF_BeginEvent(color=0x%08X, name=\"%s\") -> %d (no PIX capture)\n",
            color, buf, level);
    RET(level, 2);
}

void perf_EndEvent(void) {
    /* Pre-decrement: return the level this event ends in; negative if the
     * guest ended an event that was never begun. */
    int32_t level = --g_event_level;
    fprintf(stderr, "[d3d9] D3DPERF_EndEvent -> %d (no PIX capture)\n", level);
    RET(level, 0);
}

void perf_SetMarker(void) {
    uint32_t color = A32(0), name = A32(1);
    char buf[128];
    perf_name(name, buf, sizeof buf);
    fprintf(stderr, "[d3d9] D3DPERF_SetMarker(color=0x%08X, name=\"%s\") (no PIX capture)\n",
            color, buf);
    RET(0, 2);
}

void perf_SetRegion(void) {
    uint32_t color = A32(0), name = A32(1);
    char buf[128];
    perf_name(name, buf, sizeof buf);
    fprintf(stderr, "[d3d9] D3DPERF_SetRegion(color=0x%08X, name=\"%s\") (no PIX capture)\n",
            color, buf);
    RET(0, 2);
}

void perf_QueryRepeatFrame(void) {
    /* BOOL: no profiler requested a repeated frame. */
    RET(0, 0);
}

void perf_SetOptions(void) {
    uint32_t opts = A32(0);
    fprintf(stderr, "[d3d9] D3DPERF_SetOptions(0x%08X) (no PIX capture)\n", opts);
    RET(0, 1);
}

void perf_GetStatus(void) {
    /* DWORD bit field: 0 = no profiling tool attached, no repeat requested. */
    RET(0, 0);
}
