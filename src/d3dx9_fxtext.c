#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "d3dx9_fxtext.h"

#define MAXTOK 8192

typedef enum { T_EOF, T_IDENT, T_NUMBER, T_STRING, T_PUNCT } toktype_t;

typedef struct { toktype_t type; char text[512]; int line, off; } tok_t;

typedef struct {
    tok_t *toks;                 /* heap: ~4 MB on the stack is not acceptable */
    int ntok;
    int pos;
    const char *source;          /* for slicing exact HLSL function text */
    d3dx9_fx_effect *out;
} parser_t;

static void seterr(parser_t *p, int line, const char *msg) {
    if (p->out) { snprintf(p->out->error, D3DX9_FXTEXT_MAX_ERR, "%s", msg); p->out->error_line = line; }
}

/* Public D3D9 sampler_state names. Anything else is an error with its line. */
static int sampler_key_known(const char *k) {
    static const char *names[] = {
        "Texture", "AddressU", "AddressV", "AddressW", "BorderColor",
        "MinFilter", "MagFilter", "MipFilter", "MaxAnisotropy", "MaxMipLevel",
        "MipMapLodBias", "SRGBTexture", "ElementIndex",
        "Texture1D", "Texture2D", "Texture3D", "TextureCUBE"
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strcmp(k, names[i]) == 0) return 1;
    return 0;
}

/* ---- tokenizer ---- */

static int tokenize(const char *src, tok_t *toks, int *ntok, d3dx9_fx_effect *out) {
    int i = 0, line = 1, n = 0;
    int len = (int)strlen(src);
    while (i < len) {
        char c = src[i];
        if (c == '\n') { line++; i++; continue; }
        if (isspace((unsigned char)c)) { i++; continue; }
        if (c == '/' && i + 1 < len && src[i+1] == '/') {
            i += 2; while (i < len && src[i] != '\n') i++; continue;
        }
        if (c == '/' && i + 1 < len && src[i+1] == '*') {
            i += 2;
            while (i + 1 < len && !(src[i] == '*' && src[i+1] == '/')) { if (src[i]=='\n') line++; i++; }
            if (i + 1 >= len) { snprintf(out->error, D3DX9_FXTEXT_MAX_ERR, "unterminated comment"); out->error_line = line; return -1; }
            i += 2; continue;
        }
        if (n >= MAXTOK) { snprintf(out->error, D3DX9_FXTEXT_MAX_ERR, "too many tokens"); out->error_line = line; return -1; }
        if (c == '"') {
            int j = i + 1; char buf[512]; int bi = 0;
            while (j < len && src[j] != '"') { if (src[j]=='\n') line++; if (bi < 511) buf[bi++] = src[j]; j++; }
            if (j >= len) { snprintf(out->error, D3DX9_FXTEXT_MAX_ERR, "unterminated string"); out->error_line = line; return -1; }
            buf[bi] = 0;
            toks[n].type = T_STRING; toks[n].line = line; toks[n].off = i;
            snprintf(toks[n].text, sizeof toks[n].text, "\"%s\"", buf);
            n++; i = j + 1; continue;
        }
        if (strchr("{}()[];,:=<>*+-/%", c)) {
            char two[3] = { c, 0, 0 };
            if (i + 1 < len) {
                char c2 = src[i+1];
                if ((c=='='&&c2=='=')||(c=='!'&&c2=='=')||(c=='<'&&c2=='=')||(c=='>'&&c2=='=')||(c=='<'&&c2=='<')||(c=='>'&&c2=='>')) {
                    two[1]=c2;
                    toks[n].type=T_PUNCT; toks[n].line=line; toks[n].off=i;
                    snprintf(toks[n].text, sizeof toks[n].text, "%s", two);
                    n++; i+=2; continue;
                }
            }
            toks[n].type=T_PUNCT; toks[n].line=line; toks[n].off=i;
            toks[n].text[0]=c; toks[n].text[1]=0;
            n++; i++; continue;
        }
        if (isalnum((unsigned char)c) || c == '_' || c == '.') {
            int j = i; char buf[512]; int bi = 0;
            while (j < len && (isalnum((unsigned char)src[j]) || src[j]=='_' || src[j]=='.')) {
                if (bi < 511) buf[bi++]=src[j]; j++;
            }
            buf[bi]=0;
            toks[n].type = (isdigit((unsigned char)buf[0]) || buf[0]=='.') ? T_NUMBER : T_IDENT;
            toks[n].line = line; toks[n].off = i;
            snprintf(toks[n].text, sizeof toks[n].text, "%s", buf);
            n++; i = j; continue;
        }
        snprintf(out->error, D3DX9_FXTEXT_MAX_ERR, "unexpected character '%c'", c);
        out->error_line = line; return -1;
    }
    toks[n].type = T_EOF; toks[n].line = line; toks[n].off = i; toks[n].text[0]=0; n++;
    *ntok = n; return 0;
}

/* ---- parser helpers ---- */
static tok_t *cur(parser_t *p) { return &p->toks[p->pos]; }
static tok_t *peek(parser_t *p, int k) {
    int idx = p->pos + k;
    if (idx >= p->ntok) idx = p->ntok - 1;
    return &p->toks[idx];
}
static int accept(parser_t *p, const char *text) {
    if (strcmp(cur(p)->text, text) == 0) { p->pos++; return 1; }
    return 0;
}
static int expect(parser_t *p, const char *text) {
    if (accept(p, text)) return 0;
    char msg[128]; snprintf(msg, sizeof msg, "expected '%s' got '%s'", text, cur(p)->text);
    seterr(p, cur(p)->line, msg); return -1;
}

static void ensure_param_cap(d3dx9_fx_effect *e) {
    if (e->param_count >= e->param_cap) {
        e->param_cap = e->param_cap ? e->param_cap*2 : 16;
        e->params = realloc(e->params, e->param_cap * sizeof *e->params);
    }
}
static void ensure_tech_cap(d3dx9_fx_effect *e) {
    if (e->tech_count >= e->tech_cap) {
        e->tech_cap = e->tech_cap ? e->tech_cap*2 : 8;
        e->techniques = realloc(e->techniques, e->tech_cap * sizeof *e->techniques);
    }
}
static void ensure_pass_cap(d3dx9_fx_technique *t) {
    if (t->pass_count >= t->pass_cap) {
        t->pass_cap = t->pass_cap ? t->pass_cap*2 : 8;
        t->passes = realloc(t->passes, t->pass_cap * sizeof *t->passes);
    }
}
static void ensure_state_cap(d3dx9_fx_pass *pass) {
    if (pass->state_count >= pass->state_cap) {
        pass->state_cap = pass->state_cap ? pass->state_cap*2 : 16;
        pass->states = realloc(pass->states, pass->state_cap * sizeof *pass->states);
    }
}

/* capture tokens until terminator at depth 0 */
static int capture_until(parser_t *p, const char *term, char *buf, int bufsize) {
    buf[0] = 0; int bi = 0; int depth = 0;
    while (cur(p)->type != T_EOF) {
        if (depth == 0 && strcmp(cur(p)->text, term) == 0) break;
        if (strcmp(cur(p)->text, "{")==0 || strcmp(cur(p)->text, "(")==0 || strcmp(cur(p)->text, "[")==0) depth++;
        else if (strcmp(cur(p)->text, "}")==0 || strcmp(cur(p)->text, ")")==0 || strcmp(cur(p)->text, "]")==0) {
            if (depth > 0) depth--; else break;
        }
        int tlen = (int)strlen(cur(p)->text);
        if (bi + tlen + 2 < bufsize) {
            if (bi > 0) buf[bi++] = ' ';
            memcpy(buf + bi, cur(p)->text, tlen); bi += tlen; buf[bi] = 0;
        }
        p->pos++;
    }
    return 0;
}

/* ---- parse param ---- */
static int parse_param(parser_t *p) {
    if (cur(p)->type != T_IDENT) return -1;
    char type[64]; snprintf(type, sizeof type, "%s", cur(p)->text);
    if (strcmp(type,"technique")==0 || strcmp(type,"pass")==0 ||
        strcmp(type,"sampler")==0 || strcmp(type,"sampler_state")==0 ||
        strcmp(type,"vertexshader")==0 || strcmp(type,"pixelshader")==0)
        return -2; /* not a param */

    p->pos++;
    if (cur(p)->type != T_IDENT) { seterr(p, cur(p)->line, "expected parameter name"); return -1; }
    char name[64]; snprintf(name, sizeof name, "%s", cur(p)->text);
    p->pos++;

    /* optional array */
    if (accept(p, "[")) {
        while (cur(p)->type != T_EOF && !accept(p, "]")) p->pos++;
    }

    char semantic[64] = "";
    if (accept(p, ":")) {
        if (cur(p)->type == T_IDENT) { snprintf(semantic, sizeof semantic, "%s", cur(p)->text); p->pos++; }
        else { seterr(p, cur(p)->line, "expected semantic"); return -1; }
    }

    char defaultv[D3DX9_FXTEXT_MAX_VALUE] = "";
    char annotation[D3DX9_FXTEXT_MAX_VALUE] = "";
    /* annotation and default may appear in either order */
    int progressed = 1;
    while (progressed) {
        progressed = 0;
        if (cur(p)->type == T_PUNCT && strcmp(cur(p)->text, "<") == 0) {
            accept(p, "<");
            int bi = (int)strlen(annotation);
            int depth = 0;
            while (cur(p)->type != T_EOF) {
                if (depth == 0 && strcmp(cur(p)->text, ">") == 0) break;
                if (strcmp(cur(p)->text, "<")==0) depth++;
                else if (strcmp(cur(p)->text, ">")==0) { if (depth>0) depth--; else break; }
                int tlen = (int)strlen(cur(p)->text);
                if (bi + tlen + 2 < D3DX9_FXTEXT_MAX_VALUE) {
                    if (bi > 0 && annotation[bi-1] != ' ') annotation[bi++] = ' ';
                    memcpy(annotation + bi, cur(p)->text, tlen); bi += tlen; annotation[bi] = 0;
                }
                p->pos++;
            }
            if (!accept(p, ">")) { seterr(p, cur(p)->line, "expected '>' in annotation"); return -1; }
            progressed = 1;
        } else if (cur(p)->type == T_PUNCT && strcmp(cur(p)->text, "=") == 0) {
            accept(p, "=");
            int bi = (int)strlen(defaultv);
            int depth = 0;
            while (cur(p)->type != T_EOF) {
                if (depth == 0 && (strcmp(cur(p)->text, ";") == 0 || strcmp(cur(p)->text, "<") == 0)) break;
                if (strcmp(cur(p)->text, "{")==0 || strcmp(cur(p)->text, "(")==0 || strcmp(cur(p)->text, "[")==0) depth++;
                else if (strcmp(cur(p)->text, "}")==0 || strcmp(cur(p)->text, ")")==0 || strcmp(cur(p)->text, "]")==0) { if (depth>0) depth--; else break; }
                int tlen = (int)strlen(cur(p)->text);
                if (bi + tlen + 2 < D3DX9_FXTEXT_MAX_VALUE) {
                    if (bi > 0) defaultv[bi++] = ' ';
                    memcpy(defaultv + bi, cur(p)->text, tlen); bi += tlen; defaultv[bi] = 0;
                }
                p->pos++;
            }
            progressed = 1;
        } else {
            break;
        }
    }

    if (!accept(p, ";")) { seterr(p, cur(p)->line, "expected ';' after parameter"); return -1; }

    ensure_param_cap(p->out);
    d3dx9_fx_param *param = &p->out->params[p->out->param_count++];
    memset(param, 0, sizeof *param);
    snprintf(param->type, sizeof param->type, "%s", type);
    snprintf(param->name, sizeof param->name, "%s", name);
    snprintf(param->semantic, sizeof param->semantic, "%s", semantic);
    snprintf(param->default_value, sizeof param->default_value, "%s", defaultv);
    snprintf(param->annotation, sizeof param->annotation, "%s", annotation);
    return 0;
}

/* ---- parse sampler ---- */
static int parse_sampler(parser_t *p) {
    if (strncmp(cur(p)->text, "sampler", 7) != 0) return -2;
    char type[64]; snprintf(type, sizeof type, "%s", cur(p)->text);
    p->pos++;
    if (cur(p)->type != T_IDENT) { seterr(p, cur(p)->line, "expected sampler name"); return -1; }
    char name[64]; snprintf(name, sizeof name, "%s", cur(p)->text);
    p->pos++;

    if (accept(p, "=")) {
        if (cur(p)->type == T_IDENT && strcmp(cur(p)->text, "sampler_state") == 0) p->pos++;
        else { seterr(p, cur(p)->line, "expected sampler_state"); return -1; }
    }
    if (!accept(p, "{")) { seterr(p, cur(p)->line, "expected '{' in sampler_state"); return -1; }
    /* Structured sampler_state: key = value ; pairs, not an opaque string. */
    d3dx9_fx_state states[32];
    int nstates = 0;
    while (cur(p)->type != T_EOF && strcmp(cur(p)->text, "}") != 0) {
        if (cur(p)->type != T_IDENT) { seterr(p, cur(p)->line, "expected sampler state name"); return -1; }
        int line = cur(p)->line;
        char key[D3DX9_FXTEXT_MAX_NAME];
        snprintf(key, sizeof key, "%s", cur(p)->text);
        p->pos++;
        if (!sampler_key_known(key)) { seterr(p, line, "unknown sampler state"); return -1; }
        if (!accept(p, "=")) { seterr(p, cur(p)->line, "expected '=' in sampler_state"); return -1; }
        char value[D3DX9_FXTEXT_MAX_VALUE] = "";
        int bi = 0, depth = 0;
        while (cur(p)->type != T_EOF) {
            if (depth == 0 && strcmp(cur(p)->text, ";") == 0) break;
            if (strcmp(cur(p)->text, "(")==0 || strcmp(cur(p)->text, "{")==0 || strcmp(cur(p)->text, "[")==0) depth++;
            else if (strcmp(cur(p)->text, ")")==0 || strcmp(cur(p)->text, "}")==0 || strcmp(cur(p)->text, "]")==0) {
                if (depth > 0) depth--; else break;
            }
            int tlen = (int)strlen(cur(p)->text);
            if (bi + tlen + 2 < (int)sizeof value) {
                if (bi > 0) value[bi++] = ' ';
                memcpy(value + bi, cur(p)->text, (size_t)tlen);
                bi += tlen; value[bi] = 0;
            }
            p->pos++;
        }
        if (!accept(p, ";")) { seterr(p, cur(p)->line, "expected ';' in sampler_state"); return -1; }
        if (nstates >= (int)(sizeof states / sizeof states[0])) {
            seterr(p, line, "too many sampler states"); return -1;
        }
        d3dx9_fx_state *st = &states[nstates++];
        memset(st, 0, sizeof *st);
        snprintf(st->key, sizeof st->key, "%s", key);
        snprintf(st->value, sizeof st->value, "%s", value);
        st->line = line;
    }
    if (!accept(p, "}")) { seterr(p, cur(p)->line, "expected '}' in sampler_state"); return -1; }
    ensure_param_cap(p->out);
    d3dx9_fx_param *param = &p->out->params[p->out->param_count++];
    memset(param, 0, sizeof *param);
    snprintf(param->type, sizeof param->type, "%s", type);
    snprintf(param->name, sizeof param->name, "%s", name);
    if (nstates) {
        param->sampler_states = malloc((size_t)nstates * sizeof *param->sampler_states);
        if (!param->sampler_states) { seterr(p, cur(p)->line, "out of memory"); return -1; }
        memcpy(param->sampler_states, states, (size_t)nstates * sizeof *states);
        param->sampler_state_count = nstates;
    }
    accept(p, ";");
    return 0;
}

/* ---- parse pass ---- */
static int parse_pass(parser_t *p, d3dx9_fx_pass *pass) {
    memset(pass, 0, sizeof *pass);
    if (cur(p)->type == T_IDENT && strcmp(cur(p)->text, "pass") == 0) p->pos++;
    if (cur(p)->type == T_IDENT) {
        snprintf(pass->name, sizeof pass->name, "%s", cur(p)->text);
        p->pos++;
    }
    if (!accept(p, "{")) { seterr(p, cur(p)->line, "expected '{' in pass"); return -1; }
    while (cur(p)->type != T_EOF && strcmp(cur(p)->text, "}") != 0) {
        if (cur(p)->type != T_IDENT) { seterr(p, cur(p)->line, "expected state name in pass"); return -1; }
        char lhs[64]; snprintf(lhs, sizeof lhs, "%s", cur(p)->text);
        int state_line = cur(p)->line;
        p->pos++;
        if (!accept(p, "=")) { seterr(p, cur(p)->line, "expected '=' in pass state"); return -1; }

        if (strcmp(lhs, "VertexShader") == 0 || strcmp(lhs, "PixelShader") == 0) {
            int is_vs = (strcmp(lhs, "VertexShader") == 0);
            if (cur(p)->type == T_IDENT && strcmp(cur(p)->text, "NULL") == 0) {
                p->pos++;
                if (!accept(p, ";")) { seterr(p, cur(p)->line, "expected ';' after NULL shader"); return -1; }
                ensure_state_cap(pass);
                d3dx9_fx_state *st = &pass->states[pass->state_count++];
                memset(st, 0, sizeof *st);
                snprintf(st->key, sizeof st->key, "%s", lhs);
                snprintf(st->value, sizeof st->value, "NULL");
                st->line = cur(p)->line;
                continue;                       /* no shader attached */
            }
            if (!accept(p, "compile")) {
                seterr(p, cur(p)->line, "expected 'compile' before shader profile");
                return -1;
            }
            if (cur(p)->type != T_IDENT) { seterr(p, cur(p)->line, "expected shader profile"); return -1; }
            char profile[64]; snprintf(profile, sizeof profile, "%s", cur(p)->text);
            p->pos++;
            if (cur(p)->type != T_IDENT) { seterr(p, cur(p)->line, "expected shader entry"); return -1; }
            char entry[64]; snprintf(entry, sizeof entry, "%s", cur(p)->text);
            p->pos++;
            char args[D3DX9_FXTEXT_MAX_VALUE] = "";
            if (accept(p, "(")) {
                int bi = 0, depth = 1;
                while (cur(p)->type != T_EOF && depth > 0) {
                    if (strcmp(cur(p)->text, "(") == 0) depth++;
                    else if (strcmp(cur(p)->text, ")") == 0) {
                        depth--;
                        if (depth == 0) { p->pos++; break; }
                    }
                    int tlen = (int)strlen(cur(p)->text);
                    if (bi + tlen + 2 < D3DX9_FXTEXT_MAX_VALUE) {
                        if (bi > 0) args[bi++] = ' ';
                        memcpy(args + bi, cur(p)->text, tlen); bi += tlen; args[bi] = 0;
                    }
                    p->pos++;
                }
            }
            if (!accept(p, ";")) { seterr(p, cur(p)->line, "expected ';' after shader"); return -1; }
            d3dx9_fx_shader_ref *ref = is_vs ? &pass->vs : &pass->ps;
            if (is_vs) pass->has_vs = 1; else pass->has_ps = 1;
            snprintf(ref->profile, sizeof ref->profile, "%s", profile);
            snprintf(ref->entry, sizeof ref->entry, "%s", entry);
            snprintf(ref->args, sizeof ref->args, "%s", args);
            snprintf(ref->name, sizeof ref->name, "%s", entry);
        } else {
            /* generic state: capture RHS until ; */
            char value[D3DX9_FXTEXT_MAX_VALUE] = "";
            int bi = 0, depth = 0;
            while (cur(p)->type != T_EOF) {
                if (depth == 0 && strcmp(cur(p)->text, ";") == 0) break;
                if (strcmp(cur(p)->text, "{")==0 || strcmp(cur(p)->text, "(")==0 || strcmp(cur(p)->text, "[")==0) depth++;
                else if (strcmp(cur(p)->text, "}")==0 || strcmp(cur(p)->text, ")")==0 || strcmp(cur(p)->text, "]")==0) { if (depth>0) depth--; else break; }
                int tlen = (int)strlen(cur(p)->text);
                if (bi + tlen + 2 < D3DX9_FXTEXT_MAX_VALUE) {
                    if (bi > 0) value[bi++] = ' ';
                    memcpy(value + bi, cur(p)->text, tlen); bi += tlen; value[bi] = 0;
                }
                p->pos++;
            }
            if (!accept(p, ";")) { seterr(p, cur(p)->line, "expected ';' in pass state"); return -1; }
            ensure_state_cap(pass);
            d3dx9_fx_state *st = &pass->states[pass->state_count++];
            memset(st, 0, sizeof *st);
            snprintf(st->key, sizeof st->key, "%s", lhs);
            snprintf(st->value, sizeof st->value, "%s", value);
            st->line = state_line;
        }
    }
    if (!accept(p, "}")) { seterr(p, cur(p)->line, "expected '}' to close pass"); return -1; }
    return 0;
}

/* ---- parse technique ---- */
static int parse_technique(parser_t *p) {
    if (strcmp(cur(p)->text, "technique") != 0) return -2;
    p->pos++;
    ensure_tech_cap(p->out);
    d3dx9_fx_technique *tech = &p->out->techniques[p->out->tech_count++];
    memset(tech, 0, sizeof *tech);

    if (cur(p)->type == T_IDENT) {
        snprintf(tech->name, sizeof tech->name, "%s", cur(p)->text);
        p->pos++;
    } else {
        snprintf(tech->name, sizeof tech->name, "technique");
    }
    /* optional params in parens */
    if (accept(p, "(")) {
        while (cur(p)->type != T_EOF && !accept(p, ")")) p->pos++;
    }
    if (!accept(p, "{")) { seterr(p, cur(p)->line, "expected '{' in technique"); return -1; }
    while (cur(p)->type != T_EOF && strcmp(cur(p)->text, "}") != 0) {
        if (cur(p)->type == T_IDENT && strcmp(cur(p)->text, "pass") == 0) {
            ensure_pass_cap(tech);
            d3dx9_fx_pass *pass = &tech->passes[tech->pass_count++];
            memset(pass, 0, sizeof *pass);
            if (parse_pass(p, pass) != 0) return -1;
        } else {
            /* skip unknown statement until ; */
            while (cur(p)->type != T_EOF && strcmp(cur(p)->text, ";") != 0) p->pos++;
            accept(p, ";");
        }
    }
    if (!accept(p, "}")) { seterr(p, cur(p)->line, "expected '}' to close technique"); return -1; }
    return 0;
}

/* ---- top level ---- */
int d3dx9_fxtext_parse(const char *source, d3dx9_fx_effect *out) {
    if (!source || !out) return -1;
    memset(out, 0, sizeof *out);
    size_t srclen = strlen(source);
    if (srclen > D3DX9_FXTEXT_MAX_SRC) {
        snprintf(out->error, sizeof out->error, "source exceeds %u bytes", D3DX9_FXTEXT_MAX_SRC);
        out->error_line = 1;
        return -1;
    }
    parser_t p; memset(&p, 0, sizeof p);
    p.out = out;
    p.source = source;
    p.toks = (tok_t *)calloc(MAXTOK, sizeof(tok_t));
    if (!p.toks) { snprintf(out->error, sizeof out->error, "out of memory"); return -1; }
    if (tokenize(source, p.toks, &p.ntok, out) != 0) { free(p.toks); return -1; }
    while (cur(&p)->type != T_EOF) {
        if (cur(&p)->type == T_IDENT && strcmp(cur(&p)->text, "technique") == 0) {
            if (parse_technique(&p) != 0) goto fail;
        } else if (cur(&p)->type == T_IDENT && strncmp(cur(&p)->text, "sampler", 7) == 0) {
            if (parse_sampler(&p) != 0) goto fail;
        } else if (cur(&p)->type == T_IDENT && peek(&p, 1)->type == T_IDENT &&
                   strcmp(peek(&p, 2)->text, "(") == 0) {
            /* Function definition: keep the exact HLSL text so a caller can
             * compile it later, instead of discarding the body. */
            int start = cur(&p)->off, line = cur(&p)->line, end = (int)strlen(p.source);
            char rettype[64], name[64], args[D3DX9_FXTEXT_MAX_VALUE] = "";
            snprintf(rettype, sizeof rettype, "%s", cur(&p)->text);
            snprintf(name, sizeof name, "%s", peek(&p, 1)->text);
            p.pos += 2;
            if (accept(&p, "(")) {
                int depth = 1;
                while (cur(&p)->type != T_EOF && depth > 0) {
                    if (strcmp(cur(&p)->text, "(") == 0) depth++;
                    else if (strcmp(cur(&p)->text, ")") == 0) depth--;
                    if (depth > 0) {
                        int tlen = (int)strlen(cur(&p)->text);
                        if ((int)strlen(args) + tlen + 2 < (int)sizeof args) {
                            if (args[0]) strcat(args, " ");
                            strcat(args, cur(&p)->text);
                        }
                        p.pos++;
                    } else { p.pos++; break; }
                }
            }
            if (accept(&p, ":")) {
                while (cur(&p)->type == T_IDENT) p.pos++;
            }
            if (accept(&p, "{")) {
                int depth = 1;
                while (cur(&p)->type != T_EOF && depth > 0) {
                    if (strcmp(cur(&p)->text, "{") == 0) depth++;
                    else if (strcmp(cur(&p)->text, "}") == 0) depth--;
                    if (depth > 0) p.pos++;
                    else { end = cur(&p)->off + (int)strlen(cur(&p)->text); p.pos++; break; }
                }
            } else {
                while (cur(&p)->type != T_EOF && strcmp(cur(&p)->text, ";") != 0) p.pos++;
                end = cur(&p)->off + (int)strlen(cur(&p)->text);
                accept(&p, ";");
            }
            {
                d3dx9_fx_effect *e = p.out;
                if (e->function_count >= e->function_cap) {
                    e->function_cap = e->function_cap ? e->function_cap * 2 : 8;
                    e->functions = realloc(e->functions, e->function_cap * sizeof *e->functions);
                    if (!e->functions) { snprintf(e->error, sizeof e->error, "out of memory"); goto fail; }
                }
                d3dx9_fx_function *fn = &e->functions[e->function_count++];
                memset(fn, 0, sizeof *fn);
                snprintf(fn->name, sizeof fn->name, "%s", name);
                snprintf(fn->return_type, sizeof fn->return_type, "%s", rettype);
                snprintf(fn->args, sizeof fn->args, "%s", args);
                fn->line = line;
                size_t n = (size_t)(end - start);
                if (n >= sizeof fn->source) n = sizeof fn->source - 1;
                memcpy(fn->source, p.source + start, n);
                fn->source[n] = 0;
            }
        } else {
            int r = parse_param(&p);
            if (r == -2) {
                seterr(&p, cur(&p)->line, "unexpected top-level construct");
                goto fail;
            }
            if (r != 0) goto fail;
        }
    }
    free(p.toks);
    return 0;
fail:
    free(p.toks);
    return -1;
}

void d3dx9_fxtext_free(d3dx9_fx_effect *out) {
    if (!out) return;
    for (int i = 0; i < out->param_count; i++) free(out->params[i].sampler_states);
    free(out->functions);
    for (int i = 0; i < out->tech_count; i++) {
        for (int j = 0; j < out->techniques[i].pass_count; j++) {
            free(out->techniques[i].passes[j].states);
        }
        free(out->techniques[i].passes);
    }
    free(out->techniques);
    free(out->params);
    memset(out, 0, sizeof *out);
}
