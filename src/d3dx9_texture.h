/*
 * d3dx9_texture.h - D3DX9 texture creation: D3DXCreateTexture and the
 * D3DXCreateTextureFromFileInMemory family of functions plus
 * D3DXGetImageInfoFromFileInMemory and D3DXCheckTextureRequirements.
 *
 * Public contract after the D3DX9 documentation (D3dx9tex.h) and the D3D9
 * format/file-format values in d3d9_hle.h/d3d9_format.h:
 *
 * - D3DXCreateTexture creates an empty texture, D3D9 underneath.
 * - D3DXCreateTextureFromFileA/InMemory/InMemoryEx load BMP, TGA and DDS
 *   images; DDS may use DXT1/3/5 or the direct uncompressed formats of
 *   d3d9_format.h. Anything else is an error (D3DERR_INVALIDCALL or
 *   D3DERR_NOTAVAILABLE): we do not guess at formats we cannot decode.
 * - Data is filled through the real D3D9 calls (IDirect3DDevice9::
 *   CreateTexture, IDirect3DTexture9::LockRect/UnlockRect), so no pixel is
 *   written directly into the HLE surface buffers.
 * - D3DXIMAGE_INFO is the public info struct of D3DXGetImageInfoFromFileInMemory.
 *
 * Host-side helpers (used by d3dx9_texture.c and d3dx9_texture_selftest.c)
 * take host pointers for the out-parameters (Width, Height, Format, ...);
 * the shims in d3dx9.c pass guest pointers (hle_ptr) into the same functions.
 */
#ifndef WIN32HLE_D3DX9_TEXTURE_H
#define WIN32HLE_D3DX9_TEXTURE_H

#include <stdint.h>
#include "d3d9_hle.h"

/* D3DXIMAGE_FILEFORMAT (D3dx9tex.h). Only the formats we support on input. */
#define D3DXIFF_BMP  0u
#define D3DXIFF_JPG  1u
#define D3DXIFF_TGA  2u
#define D3DXIFF_PNG  3u
#define D3DXIFF_DDS  4u
#define D3DXIFF_PPM  5u
#define D3DXIFF_DIB  6u
#define D3DXIFF_HDR  7u
#define D3DXIFF_PFM  8u

/* D3DX_DEFAULT (D3dx9.h): a complete mipmap chain is created, and 0/D3DX_DEFAULT
 * means "the file's value" for width/height/mips/format. */
#define D3DX_DEFAULT 0xFFFFFFFFu

/* D3DXIMAGE_INFO (D3dx9tex.h) - layout identical to the public struct. */
typedef struct {
    uint32_t Width;                 /* original width in pixels */
    uint32_t Height;                /* original height in pixels */
    uint32_t Depth;                 /* original depth in pixels (volumes) */
    uint32_t MipLevels;             /* number of mip levels in the file */
    uint32_t Format;                /* D3DFORMAT that describes the data */
    uint32_t ResourceType;          /* D3DRTYPE_TEXTURE for a 2D image */
    uint32_t ImageFileFormat;       /* D3DXIFF_* */
} d3dx9_texture_info;

/* Limits matching the HLE's device caps (d3d9_idirect3d9.c). */
#define D3D9_TEX_MAX_DIM    8192u
#define D3D9_TEX_MAX_LEVELS 32u
#define D3D9_TEX_MAX_SRC    0x10000000u   /* 256 MB source blobs */

int d3dx9_texture_create(uint32_t dev, uint32_t w, uint32_t h, uint32_t levels,
                         uint32_t usage, uint32_t fmt, uint32_t pool,
                         uint32_t *ppTex);

/* D3DXCreateTextureFromFileInMemoryEx: width/height/mips 0/D3DX_DEFAULT means
 * "as in the file"; an explicit non-matching width/height is an error (we do
 * not resample). fmt 0/D3DFMT_UNKNOWN means "as in the file". Only
 * D3DX_FILTER_NONE and D3DX_FILTER_POINT (nearest) are implemented; every
 * other Filter/MipFilter is an honest D3DERR_NOTAVAILABLE with a log message.
 * ColorKey must be 0; color-keying is not implemented and a non-zero ColorKey
 * is an error. */
int d3dx9_texture_create_from_file_memory(uint32_t dev, const void *pSrcData, uint32_t size,
                                          uint32_t width, uint32_t height, uint32_t mipLevels,
                                          uint32_t usage, uint32_t fmt, uint32_t pool,
                                          uint32_t *ppTex, d3dx9_texture_info *pSrcInfo);

int d3dx9_texture_create_from_file_a(uint32_t dev, const char *pSrcFile,
                                     uint32_t *ppTex, d3dx9_texture_info *pSrcInfo);
int d3dx9_texture_create_from_file_ex_a(uint32_t dev, const char *pSrcFile,
                                        uint32_t width, uint32_t height, uint32_t mipLevels,
                                        uint32_t usage, uint32_t fmt, uint32_t pool,
                                        uint32_t filter, uint32_t mipFilter, uint32_t colorKey,
                                        d3dx9_texture_info *pSrcInfo, void *pPalette,
                                        uint32_t *ppTex);
int d3dx9_texture_create_from_file_ex_w(uint32_t dev, const uint16_t *pSrcFile,
                                        uint32_t width, uint32_t height, uint32_t mipLevels,
                                        uint32_t usage, uint32_t fmt, uint32_t pool,
                                        uint32_t filter, uint32_t mipFilter, uint32_t colorKey,
                                        d3dx9_texture_info *pSrcInfo, void *pPalette,
                                        uint32_t *ppTex);

int d3dx9_texture_get_image_info(const void *pSrcData, uint32_t size,
                                 d3dx9_texture_info *pSrcInfo);
int d3dx9_texture_get_image_info_from_file_a(const char *pSrcFile, d3dx9_texture_info *pSrcInfo);
int d3dx9_texture_get_image_info_from_file_w(const uint16_t *pSrcFile, d3dx9_texture_info *pSrcInfo);
/* Decode top image level for surface loading. For BMP/TGA returns A8R8G8B8 in
 * *decoded_rgba (caller frees). For DDS returns pointer into pSrcData with
 * *decoded_rgba==NULL and *top_pitch set to source row pitch. */
int d3dx9_texture_decode_top_level(const void *pSrcData, uint32_t size,
                                   d3dx9_texture_info *pSrcInfo,
                                   const uint8_t **top_data, uint32_t *top_pitch,
                                   uint32_t *top_fmt, uint8_t **decoded_rgba);

/* D3DXCheckTextureRequirements: width/height 0/D3DX_DEFAULT are corrected to
 * 256 or to each other (square); pFormat D3DFMT_UNKNOWN -> A8R8G8B8;
 * pNumMipLevels 0/D3DX_DEFAULT -> the complete chain. Pointers may be NULL. */
int d3dx9_texture_check_requirements(uint32_t dev, uint32_t *pWidth, uint32_t *pHeight,
                                     uint32_t *pNumMipLevels, uint32_t Usage,
                                     uint32_t *pFormat, uint32_t Pool);

int d3dx9_create_cube_texture(uint32_t dev, uint32_t edge, uint32_t levels,
                              uint32_t usage, uint32_t fmt, uint32_t pool,
                              uint32_t *ppTex);
int d3dx9_create_cube_texture_from_file_a(uint32_t dev, const char *pSrcFile,
                                          uint32_t *ppCubeTexture);
int d3dx9_create_cube_texture_from_file_w(uint32_t dev, const uint16_t *pSrcFile,
                                          uint32_t *ppCubeTexture);
int d3dx9_save_texture_to_file_a(const char *pDestFile, uint32_t DestFormat,
                                 uint32_t pSrcTexture, const void *pSrcPalette);
int d3dx9_create_volume_texture(uint32_t dev, uint32_t w, uint32_t h, uint32_t d,
                                uint32_t levels, uint32_t usage, uint32_t fmt,
                                uint32_t pool, uint32_t *ppTex);
int d3dx9_fill_texture(uint32_t tex, uint32_t func, uint32_t data);
int d3dx9_compute_normal_map(uint32_t tex, uint32_t src_tex, uint32_t flags,
                              uint32_t channel, float amplitude);

#endif /* WIN32HLE_D3DX9_TEXTURE_H */
