#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "win32hle.h"
#include "host.h"
#include "d3dx9_fxtext.h"

const recomp_dispatch_entry_t recomp_dispatch_table[1];
const uint32_t recomp_dispatch_count = 0;

static int fails;
static void ok(const char *what, int cond) { if (!cond) { printf("FAIL %s\n", what); fails++; } }

static d3dx9_fx_param *find_param(d3dx9_fx_effect *fx, const char *name) {
    for (int i = 0; i < fx->param_count; i++)
        if (strcmp(fx->params[i].name, name) == 0) return &fx->params[i];
    return NULL;
}
static d3dx9_fx_state *find_sampler_state(d3dx9_fx_param *p, const char *key) {
    for (int i = 0; i < p->sampler_state_count; i++)
        if (strcmp(p->sampler_states[i].key, key) == 0) return &p->sampler_states[i];
    return NULL;
}

int main(void)
{
    recomp_host_init();

    /* ---- simple effect ---- */
    const char *src1 =
        "float4x4 WorldViewProj : WORLDVIEWPROJECTION;\n"
        "float Intensity = 0.5f;\n"
        "texture Tex;\n"
        "sampler Samp = sampler_state { Texture = (Tex); MinFilter = Linear; };\n"
        "float4 VS(float4 pos : POSITION) : POSITION { return mul(pos, WorldViewProj); }\n"
        "float4 PS(float2 uv : TEXCOORD0) : COLOR { return tex2D(Samp, uv); }\n"
        "technique T {\n"
        "  pass P {\n"
        "    VertexShader = compile vs_1_1 VS();\n"
        "    PixelShader = compile ps_1_1 PS(uv);\n"
        "    ZEnable = true;\n"
        "  }\n"
        "}\n";

    d3dx9_fx_effect fx;
    memset(&fx, 0, sizeof fx);
    int r = d3dx9_fxtext_parse(src1, &fx);
    ok("parse simple ok", r == 0);
    if (r != 0) printf("DEBUG parse error line=%d [%s]\n", fx.error_line, fx.error);
    ok("param count", fx.param_count == 4); /* WorldViewProj, Intensity, Tex, Samp */
    ok("tech count", fx.tech_count == 1);
    ok("tech name", strcmp(fx.techniques[0].name, "T") == 0);
    ok("pass count", fx.techniques[0].pass_count == 1);
    d3dx9_fx_pass *pass = &fx.techniques[0].passes[0];
    ok("pass name", strcmp(pass->name, "P") == 0);
    ok("has vs", pass->has_vs == 1);
    ok("vs profile", strcmp(pass->vs.profile, "vs_1_1") == 0);
    ok("vs entry", strcmp(pass->vs.entry, "VS") == 0);
    ok("has ps", pass->has_ps == 1);
    ok("ps profile", strcmp(pass->ps.profile, "ps_1_1") == 0);
    ok("ps entry", strcmp(pass->ps.entry, "PS") == 0);
    ok("ps args", strcmp(pass->ps.args, "uv") == 0);
    ok("state count", pass->state_count == 1);
    ok("state key", strcmp(pass->states[0].key, "ZEnable") == 0);
    ok("state val", strcmp(pass->states[0].value, "true") == 0);
    ok("state has line", pass->states[0].line > 0);

    d3dx9_fx_param *wvp = find_param(&fx, "WorldViewProj");
    ok("WVP found", wvp != NULL);
    if (wvp) ok("WVP semantic", strcmp(wvp->semantic, "WORLDVIEWPROJECTION") == 0);
    d3dx9_fx_param *inten = find_param(&fx, "Intensity");
    ok("Intensity default", inten && strstr(inten->default_value, "0.5") != NULL);

    /* sampler_state is structured, not an opaque annotation string */
    d3dx9_fx_param *samp = find_param(&fx, "Samp");
    ok("sampler found", samp != NULL);
    if (samp) {
        ok("sampler type", strcmp(samp->type, "sampler") == 0);
        ok("sampler state count == 2", samp->sampler_state_count == 2);
        d3dx9_fx_state *tex = find_sampler_state(samp, "Texture");
        d3dx9_fx_state *minf = find_sampler_state(samp, "MinFilter");
        ok("Texture state parsed", tex && strstr(tex->value, "Tex") != NULL && tex->value[0] == '(');
        ok("MinFilter state parsed", minf && strcmp(minf->value, "Linear") == 0);
        ok("sampler state has line", tex && tex->line > 0);
        ok("annotation is not the state blob", samp->annotation[0] == 0);
    }

    /* shader function bodies are preserved for later compilation */
    ok("function count == 2", fx.function_count == 2);
    if (fx.function_count == 2) {
        ok("function 0 name", strcmp(fx.functions[0].name, "VS") == 0);
        ok("function 0 return type", strcmp(fx.functions[0].return_type, "float4") == 0);
        ok("function 0 keeps its body", strstr(fx.functions[0].source, "return mul(pos, WorldViewProj);") != NULL);
        ok("function 1 name", strcmp(fx.functions[1].name, "PS") == 0);
        ok("function 1 keeps its body", strstr(fx.functions[1].source, "return tex2D(Samp, uv);") != NULL);
        ok("functions have lines", fx.functions[0].line > 0 && fx.functions[1].line > 0);
    }
    d3dx9_fxtext_free(&fx);
    ok("free clears effect", fx.params == NULL && fx.functions == NULL && fx.param_count == 0);

    /* ---- sampler dimensions ---- */
    const char *src_dims =
        "sampler1D A1 = sampler_state { Texture = (T); };\n"
        "sampler2D A2 = sampler_state { Texture = (T); };\n"
        "sampler3D A3 = sampler_state { Texture = (T); };\n"
        "samplerCUBE AC = sampler_state { Texture = (T); };\n";
    d3dx9_fx_effect fx_dims;
    memset(&fx_dims, 0, sizeof fx_dims);
    r = d3dx9_fxtext_parse(src_dims, &fx_dims);
    ok("sampler dimensions parse", r == 0 && fx_dims.param_count == 4);
    if (fx_dims.param_count == 4) {
        ok("sampler1D type", strcmp(fx_dims.params[0].type, "sampler1D") == 0);
        ok("sampler2D type", strcmp(fx_dims.params[1].type, "sampler2D") == 0);
        ok("sampler3D type", strcmp(fx_dims.params[2].type, "sampler3D") == 0);
        ok("samplerCUBE type", strcmp(fx_dims.params[3].type, "samplerCUBE") == 0);
    }
    d3dx9_fxtext_free(&fx_dims);

    /* ---- VertexShader = NULL ---- */
    const char *src_null =
        "technique T { pass P { VertexShader = NULL; PixelShader = compile ps_2_0 P(); } }\n";
    d3dx9_fx_effect fx_null;
    memset(&fx_null, 0, sizeof fx_null);
    r = d3dx9_fxtext_parse(src_null, &fx_null);
    ok("NULL shader parses", r == 0);
    if (r == 0) {
        d3dx9_fx_pass *pn = &fx_null.techniques[0].passes[0];
        ok("has_vs false for NULL", pn->has_vs == 0);
        ok("has_ps true", pn->has_ps == 1);
        ok("NULL recorded as a state", pn->state_count == 1 && strcmp(pn->states[0].value, "NULL") == 0);
    }
    d3dx9_fxtext_free(&fx_null);

    /* ---- missing 'compile' is an error with a line ---- */
    const char *src_badcompile =
        "technique T { pass P { VertexShader = vs_2_0 V(); } }\n";
    d3dx9_fx_effect fx_bc;
    memset(&fx_bc, 0, sizeof fx_bc);
    r = d3dx9_fxtext_parse(src_badcompile, &fx_bc);
    ok("missing compile is an error", r != 0);
    ok("missing compile has a line", fx_bc.error_line > 0);
    ok("missing compile names the cause", strstr(fx_bc.error, "compile") != NULL);
    d3dx9_fxtext_free(&fx_bc);

    /* ---- unknown sampler state is an error with a line ---- */
    const char *src_badstate =
        "sampler S = sampler_state { BogusKey = 1; };\n";
    d3dx9_fx_effect fx_bs;
    memset(&fx_bs, 0, sizeof fx_bs);
    r = d3dx9_fxtext_parse(src_badstate, &fx_bs);
    ok("unknown sampler state is an error", r != 0);
    ok("unknown sampler state has a line", fx_bs.error_line == 1);
    d3dx9_fxtext_free(&fx_bs);

    /* ---- comments and strings ---- */
    const char *src2 =
        "/* block comment */\n"
        "// line comment\n"
        "float X = 1.0f; // trailing\n"
        "technique T { pass P { VertexShader = compile vs_2_0 V(); } }\n";
    d3dx9_fx_effect fx2;
    memset(&fx2, 0, sizeof fx2);
    r = d3dx9_fxtext_parse(src2, &fx2);
    ok("parse comments ok", r == 0);
    ok("comments param count", fx2.param_count == 1);
    d3dx9_fxtext_free(&fx2);

    /* ---- nested braces in annotation ---- */
    const char *src3 =
        "float X < string name = \"a{b}c\"; > = 1;\n"
        "technique T { pass P { VertexShader = compile vs_1_1 V(); } }\n";
    d3dx9_fx_effect fx3;
    memset(&fx3, 0, sizeof fx3);
    r = d3dx9_fxtext_parse(src3, &fx3);
    ok("parse nested braces ok", r == 0);
    d3dx9_fxtext_free(&fx3);

    /* ---- input bounds: a >1 MB source is rejected, not truncated silently ---- */
    char *huge = (char *)malloc(D3DX9_FXTEXT_MAX_SRC + 2);
    ok("huge source allocation", huge != NULL);
    if (huge) {
        memset(huge, 'x', D3DX9_FXTEXT_MAX_SRC + 1);
        huge[D3DX9_FXTEXT_MAX_SRC + 1] = 0;
        d3dx9_fx_effect fxh;
        memset(&fxh, 0, sizeof fxh);
        r = d3dx9_fxtext_parse(huge, &fxh);
        ok("oversized source is an error", r != 0 && strstr(fxh.error, "exceeds") != NULL);
        free(huge);
    }

    /* ---- very long identifier: truncated safely, no overrun ---- */
    char longsrc[4096];
    longsrc[0] = 0;
    strcat(longsrc, "float ");
    for (int i = 0; i < 2000; i++) strcat(longsrc, "A");
    strcat(longsrc, " = 1;\n");
    d3dx9_fx_effect fxl;
    memset(&fxl, 0, sizeof fxl);
    r = d3dx9_fxtext_parse(longsrc, &fxl);
    ok("long identifier truncates safely", r == 0);
    if (r == 0) ok("truncated name is NUL-terminated", strlen(fxl.params[0].name) < sizeof fxl.params[0].name);
    d3dx9_fxtext_free(&fxl);

    /* ---- error cases ---- */
    const char *src4 = "float X = 1\ntechnique T { pass P { VertexShader = compile vs_1_1 V(); } }\n";
    d3dx9_fx_effect fx4;
    memset(&fx4, 0, sizeof fx4);
    r = d3dx9_fxtext_parse(src4, &fx4);
    ok("missing ; is error", r != 0);
    ok("error has line", fx4.error_line > 0);
    d3dx9_fxtext_free(&fx4);

    d3dx9_fx_effect fx5;
    memset(&fx5, 0, sizeof fx5);
    r = d3dx9_fxtext_parse("foo bar baz;\n", &fx5);
    ok("unknown top-level is error", r != 0);
    d3dx9_fxtext_free(&fx5);

    d3dx9_fx_effect fx6;
    memset(&fx6, 0, sizeof fx6);
    r = d3dx9_fxtext_parse("", &fx6);
    ok("empty input ok", r == 0);
    d3dx9_fxtext_free(&fx6);

    d3dx9_fx_effect fx7;
    memset(&fx7, 0, sizeof fx7);
    r = d3dx9_fxtext_parse(NULL, &fx7);
    ok("NULL input is error", r != 0);

    if (!fails) printf("d3dx9_fxtext_selftest: all checks passed\n");
    return fails != 0;
}
