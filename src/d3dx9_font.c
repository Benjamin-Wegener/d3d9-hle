#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
void d3d_QueryInterface(void);
#include "d3dx9_font.h"
#include <SDL2/SDL_ttf.h>

/* UTF-16 -> UTF-8, including surrogate pairs. Count is in UTF-16 code units;
 * -1 means NUL-terminated. Writes at most cap-1 bytes plus a final NUL and
 * returns the byte count (without the NUL). Truncating to 0xFF used to turn
 * every non-ASCII WCHAR into invalid UTF-8 that SDL_ttf could not measure. */
static size_t utf16_to_utf8(const uint16_t *src, int count, char *dst, size_t cap)
{
    size_t n = 0;
    if (!src || cap == 0) return 0;
    if (count < 0) { count = 0; while (src[count]) count++; }
    for (int i = 0; i < count; i++) {
        uint32_t cp = src[i];
        if (cp >= 0xD800u && cp <= 0xDBFFu && i + 1 < count &&
            src[i + 1] >= 0xDC00u && src[i + 1] <= 0xDFFFu) {
            cp = 0x10000u + ((cp - 0xD800u) << 10) + (src[i + 1] - 0xDC00u);
            i++;
        } else if (cp >= 0xD800u && cp <= 0xDFFFu) {
            cp = 0xFFFDu;                      /* unpaired surrogate */
        }
        if (cp < 0x80u) {
            if (n + 1 >= cap) break;
            dst[n++] = (char)cp;
        } else if (cp < 0x800u) {
            if (n + 2 >= cap) break;
            dst[n++] = (char)(0xC0u | (cp >> 6));
            dst[n++] = (char)(0x80u | (cp & 0x3Fu));
        } else if (cp < 0x10000u) {
            if (n + 3 >= cap) break;
            dst[n++] = (char)(0xE0u | (cp >> 12));
            dst[n++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            dst[n++] = (char)(0x80u | (cp & 0x3Fu));
        } else {
            if (n + 4 >= cap) break;
            dst[n++] = (char)(0xF0u | (cp >> 18));
            dst[n++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
            dst[n++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            dst[n++] = (char)(0x80u | (cp & 0x3Fu));
        }
    }
    dst[n] = 0;
    return n;
}

typedef struct {
    char     face[32];
    int32_t  height;
    uint32_t width;
    uint32_t weight;
    uint32_t mipLevels;
    int32_t  italic;
    uint8_t  charSet;
    uint8_t  outputPrecision;
    uint8_t  quality;
    uint8_t  pitchAndFamily;
    uint32_t device;
    TTF_Font *ttf;
    int      lost;      /* between OnLostDevice and OnResetDevice */
} font_state_t;
#define FS(o) ((font_state_t *)((char *)hle_ptr(o) + 8u))

static uint32_t g_vt_font;

static void font_ensure_ttf(font_state_t *fs) {
    if (fs->ttf || fs->lost) return;
    if (!TTF_WasInit() && TTF_Init() != 0) return;
    const char *file = hle_font_file(fs->face, fs->weight >= 600, fs->italic);
    if (!file) return;
    int em = fs->height < 0 ? -fs->height : (fs->height > 0 ? fs->height * 87 / 100 : 11);
    if (em <= 0) em = 11;
    fs->ttf = TTF_OpenFont(file, em);
}

/* Exactly one RET per shim. Incrementing the refcount directly is deliberate:
 * hle_com_AddRef() is itself a RET-shim, calling it here would pop the stack
 * twice (the d3dx-matrixstack rejection of 52d11ead was this exact bug). */
void f_QueryInterface(void) { uint32_t self=A32(0), ppv=A32(2); (void)A32(1); if(!ppv) RET(D3DERR_INVALIDCALL,3); MEM32(ppv)=self; MEM32(self+4)++; RET(D3D_OK,3); }
void f_AddRef(void) { uint32_t self=A32(0); MEM32(self+4)++; RET(MEM32(self+4),1); }
void f_Release(void) { uint32_t self=A32(0); uint32_t n=--MEM32(self+4); if(!n) hle_com_free(self); RET(n,1); }

static int build_lines(TTF_Font *font, const char *text, int count, int maxw, int wrap, char lines[][256], int *nlines) {
    *nlines = 0;
    if (count < 0) count = (int)strlen(text);
    char word[256]; int wi=0;
    int ln = 0;
    lines[ln][0]=0;
    int idx = 0;
    while (idx <= count) {
        char c = (idx < count) ? text[idx] : ' ';
        if (c == '\n' || c == ' ' || idx == count) {
            if (wi > 0) {
                word[wi]=0;
                char test[512];
                if (lines[ln][0]) snprintf(test, sizeof test, "%s %s", lines[ln], word);
                else snprintf(test, sizeof test, "%s", word);
                int tw=0, th=0;
                if (font) TTF_SizeUTF8(font, test, &tw, &th);
                if (wrap && maxw > 0 && lines[ln][0] && tw > maxw) {
                    ln++;
                    if (ln >= 64) break;
                    lines[ln][0]=0;
                    snprintf(lines[ln], 256, "%s", word);
                } else {
                    size_t cur = strlen(lines[ln]);
                    if (cur > 0 && cur + 1 < 256) {
                        lines[ln][cur++] = ' ';
                        lines[ln][cur] = 0;
                    }
                    if (cur < 255) {
                        snprintf(lines[ln] + cur, 256 - cur, "%s", word);
                    }
                }
                wi=0;
            }
            if (c == '\n') {
                ln++;
                if (ln >= 64) break;
                lines[ln][0]=0;
            }
        } else {
            if (wi < 255) word[wi++]=c;
        }
        idx++;
    }
    *nlines = ln + 1;
    if (*nlines > 64) *nlines = 64;
    if (*nlines < 1) *nlines = 1;
    return *nlines;
}

/* shared implementation taking a host C string */
static int draw_text_impl(font_state_t *fs, const char *text, int count, uint32_t rect_va, uint32_t format, uint32_t color) {
    /* After OnLostDevice the font has no device resources; DrawText returns 0
     * until OnResetDevice, as a failed ID3DXFont::DrawText does. */
    if (fs->lost) return 0;
    font_ensure_ttf(fs);
    if (count < 0 && text) count = (int)strlen(text);
    if (count < 0) count = 0;

    int32_t *r = rect_va ? (int32_t *)hle_ptr(rect_va) : NULL;
    int left=0,top=0,right=0,bottom=0;
    if (r) { left=r[0]; top=r[1]; right=r[2]; bottom=r[3]; }
    int maxw = (right > left) ? (right - left) : 0;

    char lines[64][256];
    for (int i=0;i<64;i++) lines[i][0]=0;
    int nlines = 0;
    int wrap = (format & DT_WORDBREAK) && !(format & DT_SINGLELINE);
    build_lines(fs->ttf, text ? text : "", count, maxw, wrap, lines, &nlines);
    if (nlines < 1) nlines = 1;

    int fh = fs->ttf ? TTF_FontHeight(fs->ttf) : 12;
    int totalh = nlines * fh;

    int starty = top;
    if (format & DT_VCENTER) starty = top + (bottom - top - totalh) / 2;
    else if (format & DT_BOTTOM) starty = bottom - totalh;

    if (!(format & DT_CALCRECT)) {
        uint32_t dev = fs->device;
        if (dev) {
            uint32_t pp = hle_va(hle_alloc(4));
            uint32_t aa[3] = { dev, 0, pp };
            uint32_t grt = hle_resolve("IDirect3DDevice9::GetRenderTarget");
            if (grt) {
                hle_call_guest(grt, 3, aa);
                uint32_t surf = MEM32(pp);
                if (surf) {
                    void *lrp = hle_alloc(8);
                    uint32_t lr_va = hle_va(lrp);
                    uint32_t la[4] = { surf, lr_va, 0, 0 };
                    uint32_t lock = hle_resolve("IDirect3DSurface9::LockRect");
                    if (lock && hle_call_guest(lock, 4, la) == D3D_OK) {
                        int32_t pitch = ((int32_t*)lrp)[0];
                        uint32_t bits = ((uint32_t*)lrp)[1];
                        uint8_t *dst = (uint8_t *)hle_ptr(bits);
                        SDL_Color col = { (Uint8)(color & 0xFF), (Uint8)((color>>8)&0xFF), (Uint8)((color>>16)&0xFF), (Uint8)((color>>24)&0xFF) };
                        for (int li=0; li<nlines; li++) {
                            if (!lines[li][0]) continue;
                            SDL_Surface *ts = fs->ttf ? TTF_RenderUTF8_Blended(fs->ttf, lines[li], col) : NULL;
                            if (!ts) continue;
                            int tw = ts->w, th = ts->h;
                            int x = left;
                            if (format & DT_CENTER) x = left + (maxw - tw) / 2;
                            else if (format & DT_RIGHT) x = right - tw;
                            int y = starty + li * fh;
                            for (int yy=0; yy<th; yy++) {
                                for (int xx=0; xx<tw; xx++) {
                                    int dx = x+xx, dy = y+yy;
                                    if (dx < left || dx >= right || dy < top || dy >= bottom) continue;
                                    uint32_t *srcpx = (uint32_t*)((uint8_t*)ts->pixels + yy*ts->pitch);
                                    uint32_t *dstpx = (uint32_t*)(dst + (size_t)dy*pitch + (size_t)dx*4);
                                    uint32_t sp = srcpx[xx];
                                    if ((sp >> 24) > 0) *dstpx = sp;
                                }
                            }
                            SDL_FreeSurface(ts);
                        }
                        uint32_t unl = hle_resolve("IDirect3DSurface9::UnlockRect");
                        if (unl) { uint32_t ua[1]={surf}; hle_call_guest(unl,1,ua); }
                    }
                    hle_free(lrp);
                }
            }
        }
    }

    int maxline = 0;
    for (int li=0; li<nlines; li++) {
        int tw=0,th=0;
        if (fs->ttf) TTF_SizeUTF8(fs->ttf, lines[li], &tw,&th);
        if (tw > maxline) maxline = tw;
    }
    if (format & DT_CALCRECT) {
        if (r) {
            r[2] = r[0] + maxline;
            r[3] = r[1] + totalh;
        }
    }

    /* D3D9: return height in logical units; if VCENTER/BOTTOM return offset
     * from pRect->top to bottom of drawn text. */
    if (format & DT_VCENTER || format & DT_BOTTOM)
        return starty + totalh - top;
    return totalh;
}

void f_DrawTextA(void) {
    uint32_t self=A32(0);
    (void)A32(1);
    const char *text = (const char *)hle_ptr(A32(2));
    int count = (int)A32(3);
    uint32_t rect_va = A32(4);
    uint32_t format = A32(5);
    uint32_t color = A32(6);
    /* Wine ID3DXFontImpl_DrawTextA/W: !string || !count returns 0. */
    if (!text || count == 0) RET(0, 7);
    font_state_t *fs = FS(self);
    int r = draw_text_impl(fs, text, count, rect_va, format, color);
    RET((uint32_t)r, 7);
}

void f_DrawTextW(void) {
    uint32_t self=A32(0);
    (void)A32(1);
    const uint16_t *wtext = (const uint16_t *)hle_ptr(A32(2));
    int count = (int)A32(3);
    uint32_t rect_va = A32(4);
    uint32_t format = A32(5);
    uint32_t color = A32(6);
    if (!wtext || count == 0) RET(0, 7);
    font_state_t *fs = FS(self);
    char buf[4096];
    buf[0] = 0;
    utf16_to_utf8(wtext, count, buf, sizeof buf);
    int r = draw_text_impl(fs, buf, -1, rect_va, format, color);
    RET((uint32_t)r, 7);
}

/* Slot 3: GetDevice(this, ppDevice) -> HRESULT. Wine adds a reference. */
void f_GetDevice(void) {
    uint32_t self=A32(0), pp=A32(1);
    if (!pp) RET(D3DERR_INVALIDCALL,2);
    uint32_t dev = FS(self)->device;
    MEM32(pp) = dev;
    if (dev) MEM32(dev + 4)++;
    RET(D3D_OK,2);
}

/* D3DXFONT_DESCA is 56 bytes (FaceName[32] char at offset 24);
 * D3DXFONT_DESCW is 88 bytes (FaceName[32] WCHAR at the same offset).
 * Public layout, see Wine include/d3dx9core.h. */
static void write_desc(font_state_t *fs, uint8_t *d, int wide) {
    memset(d, 0, wide ? 88u : 56u);
    *(int32_t  *)(d + 0)  = fs->height;
    *(uint32_t *)(d + 4)  = fs->width;
    *(uint32_t *)(d + 8)  = fs->weight;
    *(uint32_t *)(d + 12) = fs->mipLevels;
    *(int32_t  *)(d + 16) = fs->italic;
    d[20] = fs->charSet;
    d[21] = fs->outputPrecision;
    d[22] = fs->quality;
    d[23] = fs->pitchAndFamily;
    if (wide) {
        for (unsigned i = 0; i < 32 && fs->face[i]; i++) {
            uint16_t ch = (uint8_t)fs->face[i];
            memcpy(d + 24 + 2u * i, &ch, 2);
        }
    } else {
        snprintf((char *)(d + 24), 32, "%s", fs->face);
    }
}

void f_GetDescA(void) {
    uint32_t self=A32(0), pDesc=A32(1);
    if (!pDesc) RET(D3DERR_INVALIDCALL,2);
    write_desc(FS(self), (uint8_t *)hle_ptr(pDesc), 0);
    RET(D3D_OK,2);
}

void f_GetDescW(void) {
    uint32_t self=A32(0), pDesc=A32(1);
    if (!pDesc) RET(D3DERR_INVALIDCALL,2);
    write_desc(FS(self), (uint8_t *)hle_ptr(pDesc), 1);
    RET(D3D_OK,2);
}

/* GetTextMetricsA/W return BOOL, not HRESULT: 1 on success, 0 on failure.
 * TEXTMETRICA is 56 bytes; TEXTMETRICW is 60 bytes (four WCHAR fields at
 * offsets 44/46/48/50), see Wine include/wingdi.h. */
static int write_metrics(font_state_t *fs, uint8_t *m, int wide) {
    memset(m, 0, wide ? 60u : 56u);
    int32_t tmHeight = 12, tmAscent = 10, tmDescent = 2;
    int32_t aveW = 6, maxW = 12;
    if (fs->ttf) {
        tmHeight = TTF_FontHeight(fs->ttf);
        tmAscent = TTF_FontAscent(fs->ttf);
        /* SDL_ttf returns negative descent; TEXTMETRIC expects positive */
        int32_t dsc = TTF_FontDescent(fs->ttf);
        tmDescent = dsc < 0 ? -dsc : dsc;
        int minx=0,maxx=0,miny=0,maxy=0,adv=0;
        if (TTF_GlyphMetrics(fs->ttf, 'M', &minx,&maxx,&miny,&maxy,&adv) == 0) aveW = adv;
        if (TTF_GlyphMetrics(fs->ttf, 'W', &minx,&maxx,&miny,&maxy,&adv) == 0) maxW = adv;
        if (maxW < aveW) maxW = aveW;
    }
    *(int32_t *)(m + 0)  = tmHeight;
    *(int32_t *)(m + 4)  = tmAscent;
    *(int32_t *)(m + 8)  = tmDescent;
    *(int32_t *)(m + 20) = aveW;
    *(int32_t *)(m + 24) = maxW;
    *(int32_t *)(m + 28) = (int32_t)fs->weight;
    if (wide) {
        uint16_t first = 32, last = 255, def = 32, brk = 32;
        memcpy(m + 44, &first, 2);
        memcpy(m + 46, &last, 2);
        memcpy(m + 48, &def, 2);
        memcpy(m + 50, &brk, 2);
        m[52] = fs->italic ? 1 : 0;
        m[55] = fs->pitchAndFamily;
        m[56] = fs->charSet;
    } else {
        m[44] = 32;  /* tmFirstChar */
        m[45] = 255; /* tmLastChar */
        m[46] = 32;  /* tmDefaultChar */
        m[47] = 32;  /* tmBreakChar */
        m[48] = fs->italic ? 1 : 0;
        m[51] = fs->pitchAndFamily;
        m[52] = fs->charSet;
    }
    return 1;
}

void f_GetTextMetricsA(void) {
    uint32_t self=A32(0), ptm=A32(1);
    font_state_t *fs=FS(self);
    if (!ptm || fs->lost) RET(0,2);
    font_ensure_ttf(fs);
    RET((uint32_t)write_metrics(fs, (uint8_t *)hle_ptr(ptm), 0),2);
}

void f_GetTextMetricsW(void) {
    uint32_t self=A32(0), ptm=A32(1);
    font_state_t *fs=FS(self);
    if (!ptm || fs->lost) RET(0,2);
    font_ensure_ttf(fs);
    RET((uint32_t)write_metrics(fs, (uint8_t *)hle_ptr(ptm), 1),2);
}

/* Slots 8..13 have no host resource behind them (no HDC, no glyph atlas).
 * They occupy their documented slots and report a failure, never a fake OK. */
void f_GetDC(void) {
    fprintf(stderr, "[d3dx9] ID3DXFont::GetDC: no host HDC, returning NULL\n");
    RET(0,1);
}
void f_GetGlyphData(void) {
    fprintf(stderr, "[d3dx9] ID3DXFont::GetGlyphData: glyph atlas not available\n");
    RET(D3DERR_INVALIDCALL,5);
}
void f_PreloadCharacters(void) {
    fprintf(stderr, "[d3dx9] ID3DXFont::PreloadCharacters: glyph atlas not available\n");
    RET(D3DERR_INVALIDCALL,3);
}
void f_PreloadGlyphs(void) {
    fprintf(stderr, "[d3dx9] ID3DXFont::PreloadGlyphs: glyph atlas not available\n");
    RET(D3DERR_INVALIDCALL,3);
}
void f_PreloadTextA(void) {
    fprintf(stderr, "[d3dx9] ID3DXFont::PreloadTextA: glyph atlas not available\n");
    RET(D3DERR_INVALIDCALL,3);
}
void f_PreloadTextW(void) {
    fprintf(stderr, "[d3dx9] ID3DXFont::PreloadTextW: glyph atlas not available\n");
    RET(D3DERR_INVALIDCALL,3);
}

/* Slots 16/17: real device-resource lifecycle, not OK stubs. */
void f_OnLostDevice(void) {
    font_state_t *fs = FS(A32(0));
    if (fs->ttf) { TTF_CloseFont(fs->ttf); fs->ttf = NULL; }
    fs->lost = 1;
    RET(D3D_OK, 1);
}
void f_OnResetDevice(void) {
    font_state_t *fs = FS(A32(0));
    fs->lost = 0;
    RET(D3D_OK, 1);
}

/* Guest-visible vtable in the exact public header order (Wine d3dx9core.h,
 * ID3DXFontVtbl). hle_com_vtable builds slots in array order; a guest calling
 * by slot index therefore reaches the same method native D3DX9 would. */
static const win32hle_shim font_methods[] = {
    { "ID3DXFont::QueryInterface",   d3d_QueryInterface },   /* slot 0 */
    { "ID3DXFont::AddRef",           f_AddRef },           /* slot 1 */
    { "ID3DXFont::Release",          f_Release },          /* slot 2 */
    { "ID3DXFont::GetDevice",        f_GetDevice },        /* slot 3 */
    { "ID3DXFont::GetDescA",         f_GetDescA },         /* slot 4 */
    { "ID3DXFont::GetDescW",         f_GetDescW },         /* slot 5 */
    { "ID3DXFont::GetTextMetricsA",  f_GetTextMetricsA },  /* slot 6 */
    { "ID3DXFont::GetTextMetricsW",  f_GetTextMetricsW },  /* slot 7 */
    { "ID3DXFont::GetDC",            f_GetDC },            /* slot 8 */
    { "ID3DXFont::GetGlyphData",     f_GetGlyphData },     /* slot 9 */
    { "ID3DXFont::PreloadCharacters",f_PreloadCharacters}, /* slot 10 */
    { "ID3DXFont::PreloadGlyphs",    f_PreloadGlyphs },    /* slot 11 */
    { "ID3DXFont::PreloadTextA",     f_PreloadTextA },     /* slot 12 */
    { "ID3DXFont::PreloadTextW",     f_PreloadTextW },     /* slot 13 */
    { "ID3DXFont::DrawTextA",        f_DrawTextA },        /* slot 14 */
    { "ID3DXFont::DrawTextW",        f_DrawTextW },        /* slot 15 */
    { "ID3DXFont::OnLostDevice",     f_OnLostDevice },     /* slot 16 */
    { "ID3DXFont::OnResetDevice",    f_OnResetDevice },    /* slot 17 */
    { 0, 0 }
};

static uint32_t create_font_obj(uint32_t dev, int32_t height, uint32_t width, uint32_t weight,
                                 uint32_t mipLevels, int32_t italic, uint8_t charSet,
                                 uint8_t outPrec, uint8_t quality, uint8_t pitchFamily,
                                 const char *face) {
    if (!g_vt_font) g_vt_font = hle_com_vtable(font_methods);
    uint32_t obj = hle_com_new(g_vt_font, 8 + sizeof(font_state_t));
    font_state_t *fs = FS(obj);
    memset(fs, 0, sizeof *fs);
    fs->height = height;
    fs->width = width;
    fs->weight = weight;
    fs->mipLevels = mipLevels;
    fs->italic = italic;
    fs->charSet = charSet;
    fs->outputPrecision = outPrec;
    fs->quality = quality;
    fs->pitchAndFamily = pitchFamily;
    fs->device = dev;
    if (face && face[0]) snprintf(fs->face, sizeof fs->face, "%s", face);
    else strcpy(fs->face, "Arial");
    return obj;
}

void s_D3DXCreateFontA(void) {
    uint32_t dev=A32(0);
    int32_t height=(int32_t)A32(1);
    uint32_t width=A32(2);
    uint32_t weight=A32(3);
    uint32_t mipLevels=A32(4);
    int32_t italic=(int32_t)A32(5);
    uint8_t charSet=(uint8_t)A32(6);
    uint8_t outPrec=(uint8_t)A32(7);
    uint8_t quality=(uint8_t)A32(8);
    uint8_t pitchFamily=(uint8_t)A32(9);
    const char *face = (const char *)hle_ptr(A32(10));
    uint32_t pp=A32(11);
    /* Wine D3DXCreateFontA: !device || !font -> D3DERR_INVALIDCALL. */
    if (!dev || !pp) { RET(D3DERR_INVALIDCALL, 12); return; }
    MEM32(pp) = create_font_obj(dev, height, width, weight, mipLevels, italic, charSet, outPrec, quality, pitchFamily, face);
    RET(D3D_OK, 12);
}

void s_D3DXCreateFontW(void) {
    uint32_t dev=A32(0);
    int32_t height=(int32_t)A32(1);
    uint32_t width=A32(2);
    uint32_t weight=A32(3);
    uint32_t mipLevels=A32(4);
    int32_t italic=(int32_t)A32(5);
    uint8_t charSet=(uint8_t)A32(6);
    uint8_t outPrec=(uint8_t)A32(7);
    uint8_t quality=(uint8_t)A32(8);
    uint8_t pitchFamily=(uint8_t)A32(9);
    const uint16_t *facew = (const uint16_t *)hle_ptr(A32(10));
    uint32_t pp=A32(11);
    if (!dev || !pp) { RET(D3DERR_INVALIDCALL, 12); return; }
    char face[128];
    face[0] = 0;
    utf16_to_utf8(facew, -1, face, sizeof face);
    MEM32(pp) = create_font_obj(dev, height, width, weight, mipLevels, italic, charSet, outPrec, quality, pitchFamily, face[0] ? face : NULL);
    RET(D3D_OK, 12);
}

void s_D3DXCreateFontIndirectA(void) {
    /* (device, pDesc, ppFont) */
    uint32_t dev = A32(0);
    uint32_t pDesc = A32(1);
    uint32_t pp = A32(2);
    /* Wine D3DXCreateFontIndirectA/W: !device || !desc || !font -> INVALIDCALL. */
    if (!dev || !pDesc || !pp) { RET(D3DERR_INVALIDCALL, 3); return; }
    uint8_t *d = (uint8_t *)hle_ptr(pDesc);
    int32_t height = *(int32_t *)(d + 0);
    uint32_t width = *(uint32_t *)(d + 4);
    uint32_t weight = *(uint32_t *)(d + 8);
    uint32_t mipLevels = *(uint32_t *)(d + 12);
    int32_t italic = *(int32_t *)(d + 16);
    uint8_t charSet = d[20], outPrec = d[21], quality = d[22], pitchFamily = d[23];
    char face[32]; memcpy(face, d + 24, 31); face[31] = 0;
    MEM32(pp) = create_font_obj(dev, height, width, weight, mipLevels, italic, charSet, outPrec, quality, pitchFamily, face);
    RET(D3D_OK, 3);
}

void s_D3DXCreateFontIndirectW(void) {
    uint32_t dev = A32(0);
    uint32_t pDesc = A32(1);
    uint32_t pp = A32(2);
    if (!dev || !pDesc || !pp) { RET(D3DERR_INVALIDCALL, 3); return; }
    uint8_t *d = (uint8_t *)hle_ptr(pDesc);
    int32_t height = *(int32_t *)(d + 0);
    uint32_t width = *(uint32_t *)(d + 4);
    uint32_t weight = *(uint32_t *)(d + 8);
    uint32_t mipLevels = *(uint32_t *)(d + 12);
    int32_t italic = *(int32_t *)(d + 16);
    uint8_t charSet = d[20], outPrec = d[21], quality = d[22], pitchFamily = d[23];
    /* FaceName is UTF-16 in D3DXFONT_DESCW at the same offset, 32 WCHARs */
    char face[128];
    face[0] = 0;
    utf16_to_utf8((const uint16_t *)(d + 24), 32, face, sizeof face);
    MEM32(pp) = create_font_obj(dev, height, width, weight, mipLevels, italic, charSet, outPrec, quality, pitchFamily, face);
    RET(D3D_OK, 3);
}
