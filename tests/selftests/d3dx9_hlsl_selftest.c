/* d3dx9_hlsl_selftest.c - optional vkd3d-shader HLSL->D3D9 bridge.
 * Without libvkd3d-shader the test verifies an honest UNAVAILABLE error.
 * With HLE_VKD3D_SHADER set (or the library otherwise loadable) it performs
 * real compilations. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3dx9_hlsl.h"
#include "win32hle.h"

uint8_t *hle_szdd_expand(const uint8_t *in, uint32_t n, uint32_t *out_n); /* unused, link compat */
static void none(void) { }
const recomp_dispatch_entry_t recomp_dispatch_table[] = { { 0x1000u, none } };
const uint32_t recomp_dispatch_count = 1;

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } \
                             else { printf("ok: %s\n", msg); } } while (0)

struct inc_file { const char *name; const char *data; };
static int open_inc(const char *filename, int local, const char *parent_data,
                    void *ctx, const void **data, size_t *size) {
    struct inc_file *f = (struct inc_file *)ctx;
    (void)local; (void)parent_data;
    for (; f && f->name; f++) {
        if (strcmp(f->name, filename) == 0) {
            *data = f->data;
            *size = strlen(f->data);
            return 0;
        }
    }
    return -1;
}
static void close_inc(const void *data, size_t size, void *ctx) { (void)data; (void)size; (void)ctx; }

static void test_unavailable(void) {
    hle_vkd3d_code out = {0};
    char *msgs = NULL;
    int rc = hle_vkd3d_compile_hlsl("float4 main():SV_Position{return 0;}", 37,
                                    "t.hlsl", "main", "vs_2_0", NULL, 0,
                                    NULL, NULL, NULL, 0, &out, &msgs);
    CHECK(rc == HLE_VKD3D_ERR_UNAVAILABLE, "compile without library returns UNAVAILABLE");
    CHECK(out.code == NULL, "unavailable compile leaves out.code NULL");
    hle_vkd3d_free_messages(msgs);
    hle_vkd3d_free_code(&out);
}

static int has_d3d9_bytecode(const hle_vkd3d_code *c, uint32_t version_token) {
    const unsigned char *b;
    if (!c || !c->code || c->size < 8) return 0;
    b = (const unsigned char *)c->code;
    /* D3D9 bytecode starts with version token (e.g. 0xFFFE0200 vs_2_0, 0xFFFF0200 ps_2_0) */
    return b[0] == (version_token & 0xff) && b[1] == ((version_token >> 8) & 0xff) &&
           b[2] == ((version_token >> 16) & 0xff) && b[3] == ((version_token >> 24) & 0xff);
}

static void test_real(void) {
    hle_vkd3d_code out = {0};
    char *msgs = NULL;
    int rc;
    static const char vs_src[] =
        "float4 main(float4 p:POSITION):SV_Position { return p; }\n";
    static const char ps_src[] =
        "float4 main():COLOR { return float4(1,0,0,1); }\n";
    static const char ps_def_src[] =
        "float4 main():COLOR { return float4(RVAL,GVAL,0,1); }\n";
    static const char inc_main[] =
        "#include \"inc.h\"\nfloat4 main():COLOR { return C; }\n";
    struct inc_file incs[] = { { "inc.h", "static const float4 C = float4(0,1,0,1);\n" }, { NULL, NULL } };
    hle_vkd3d_macro defs[] = { { "RVAL", "0" }, { "GVAL", "1" } };

    rc = hle_vkd3d_compile_hlsl(vs_src, strlen(vs_src), "vs.hlsl", "main", "vs_2_0",
                                NULL, 0, NULL, NULL, NULL, 0, &out, &msgs);
    CHECK(rc == HLE_VKD3D_OK, "vs_2_0 compiles");
    CHECK(has_d3d9_bytecode(&out, 0xFFFE0200u), "vs_2_0 has vs_2_0 version token");
    hle_vkd3d_free_messages(msgs); msgs = NULL; hle_vkd3d_free_code(&out);

    rc = hle_vkd3d_compile_hlsl(ps_src, strlen(ps_src), "ps.hlsl", "main", "ps_2_0",
                                NULL, 0, NULL, NULL, NULL, 0, &out, &msgs);
    CHECK(rc == HLE_VKD3D_OK, "ps_2_0 compiles");
    CHECK(has_d3d9_bytecode(&out, 0xFFFF0200u), "ps_2_0 has ps_2_0 version token");
    hle_vkd3d_free_messages(msgs); msgs = NULL; hle_vkd3d_free_code(&out);

    /* defines */
    rc = hle_vkd3d_compile_hlsl(ps_def_src, strlen(ps_def_src), "def.hlsl", "main", "ps_2_0",
                                defs, 2, NULL, NULL, NULL, 0, &out, &msgs);
    CHECK(rc == HLE_VKD3D_OK, "ps_2_0 with defines compiles");
    hle_vkd3d_free_messages(msgs); msgs = NULL; hle_vkd3d_free_code(&out);

    /* include callback */
    rc = hle_vkd3d_compile_hlsl(inc_main, strlen(inc_main), "main.hlsl", "main", "ps_2_0",
                                NULL, 0, open_inc, close_inc, incs, 0, &out, &msgs);
    CHECK(rc == HLE_VKD3D_OK, "ps_2_0 with include callback compiles");
    CHECK(has_d3d9_bytecode(&out, 0xFFFF0200u), "include compile has ps_2_0 token");
    hle_vkd3d_free_messages(msgs); msgs = NULL; hle_vkd3d_free_code(&out);

    /* invalid shader returns COMPILE error and passes messages through */
    rc = hle_vkd3d_compile_hlsl("float4 main():COLOR { return undeclared; }", 42,
                                "bad.hlsl", "main", "ps_2_0",
                                NULL, 0, NULL, NULL, NULL, 0, &out, &msgs);
    CHECK(rc == HLE_VKD3D_ERR_COMPILE, "invalid shader returns COMPILE error");
    CHECK(msgs != NULL && msgs[0] != '\0', "invalid shader produces messages");
    hle_vkd3d_free_messages(msgs); msgs = NULL; hle_vkd3d_free_code(&out);

    /* bad args */
    rc = hle_vkd3d_compile_hlsl(NULL, 0, NULL, "main", "ps_2_0", NULL, 0, NULL, NULL, NULL, 0, &out, &msgs);
    CHECK(rc == HLE_VKD3D_ERR_INVALID, "NULL source returns INVALID");
}

int main(void) {
    const char *env = getenv("HLE_VKD3D_SHADER");
    int avail = hle_vkd3d_available();
    printf("d3dx9_hlsl_selftest: HLE_VKD3D_SHADER=%s available=%d\n",
           env ? env : "(unset)", avail);
    if (!avail) {
        /* Without library, the honest error path must work. */
        test_unavailable();
        printf("d3dx9_hlsl_selftest: %s (unavailable path)\n", fails ? "FAIL" : "PASS");
        return fails ? 1 : 0;
    }

    test_real();
    printf("d3dx9_hlsl_selftest: %s (real compile)\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
