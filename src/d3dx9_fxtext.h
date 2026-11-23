/*
 * d3dx9_fxtext.h - Parser for D3DX .fx effect source text.
 *
 * Reads an effect source file into a structured representation: global
 * parameters (type, name, semantic, default, annotation, structured sampler
 * states), HLSL function definitions with their complete source, techniques
 * with passes, per-pass states and the VertexShader/PixelShader compile
 * directives. Shader bodies are not compiled here; the recorded HLSL source
 * and each compile directive let a caller compile them individually later.
 *
 * Line comments, C-style block comments, string literals and nested braces
 * are handled.
 * Unknown or malformed constructs are reported as an error with a line number.
 * All inputs are bounded.
 */
#ifndef WIN32HLE_D3DX9_FXTEXT_H
#define WIN32HLE_D3DX9_FXTEXT_H

#include <stdint.h>

#define D3DX9_FXTEXT_MAX_NAME   64
#define D3DX9_FXTEXT_MAX_VALUE  512
#define D3DX9_FXTEXT_MAX_ERR    256
#define D3DX9_FXTEXT_MAX_FN    16384       /* one function's source */
#define D3DX9_FXTEXT_MAX_SRC  (1u << 20)   /* total effect source */

typedef struct {
    char key[D3DX9_FXTEXT_MAX_NAME];
    char value[D3DX9_FXTEXT_MAX_VALUE];
    int line;
} d3dx9_fx_state;

typedef struct {
    char type[D3DX9_FXTEXT_MAX_NAME];
    char name[D3DX9_FXTEXT_MAX_NAME];
    char semantic[D3DX9_FXTEXT_MAX_NAME];
    char default_value[D3DX9_FXTEXT_MAX_VALUE];
    char annotation[D3DX9_FXTEXT_MAX_VALUE];
    d3dx9_fx_state *sampler_states;        /* structured sampler_state body */
    int sampler_state_count;
} d3dx9_fx_param;

typedef struct {
    char name[D3DX9_FXTEXT_MAX_NAME];
    char return_type[D3DX9_FXTEXT_MAX_NAME];
    char args[D3DX9_FXTEXT_MAX_VALUE];
    char source[D3DX9_FXTEXT_MAX_FN];      /* exact HLSL text, including the body */
    int line;
} d3dx9_fx_function;

typedef struct {
    char name[D3DX9_FXTEXT_MAX_NAME];
    char profile[D3DX9_FXTEXT_MAX_NAME];   /* e.g. vs_1_1 */
    char entry[D3DX9_FXTEXT_MAX_NAME];     /* entry function name */
    char args[D3DX9_FXTEXT_MAX_VALUE];     /* argument list text */
} d3dx9_fx_shader_ref;

typedef struct {
    char name[D3DX9_FXTEXT_MAX_NAME];
    d3dx9_fx_state *states;
    int state_count, state_cap;
    int has_vs, has_ps;
    d3dx9_fx_shader_ref vs, ps;
} d3dx9_fx_pass;

typedef struct {
    char name[D3DX9_FXTEXT_MAX_NAME];
    d3dx9_fx_pass *passes;
    int pass_count, pass_cap;
} d3dx9_fx_technique;

typedef struct {
    d3dx9_fx_param *params;
    int param_count, param_cap;
    d3dx9_fx_function *functions;
    int function_count, function_cap;
    d3dx9_fx_technique *techniques;
    int tech_count, tech_cap;
    char error[D3DX9_FXTEXT_MAX_ERR];
    int error_line;
} d3dx9_fx_effect;

/* Parse source into *out. Returns 0 on success, -1 on error (out->error). */
int d3dx9_fxtext_parse(const char *source, d3dx9_fx_effect *out);

/* Free owned arrays. */
void d3dx9_fxtext_free(d3dx9_fx_effect *out);

#endif /* WIN32HLE_D3DX9_FXTEXT_H */
