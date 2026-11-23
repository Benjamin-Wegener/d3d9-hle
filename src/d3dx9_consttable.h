/*
 * d3dx9_consttable.h - D3DX9 shader helpers and the ID3DXConstantTable object.
 *
 * Implemented only from the publicly documented D3DX9 structures
 * (d3dx9shader.h / Microsoft Learn): D3DXSHADER_CONSTANTTABLE (7 DWORDs),
 * D3DXSHADER_CONSTANTINFO and D3DXSHADER_TYPEINFO. Every offset in those
 * structures is a BYTE offset from the start of the constant table (the data
 * pointer returned by D3DXFindShaderComment for the 'CTAB' comment), never a
 * guest virtual address. The compiled-effect binary parser (d3dx9_fxbin,
 * commit 9b20a12) is neither read nor used. Registered via win32hle_d3dx9[]
 * in d3dx9.c.
 */
#ifndef WIN32HLE_D3DX9_CONSTTABLE_H
#define WIN32HLE_D3DX9_CONSTTABLE_H

#include "win32hle.h"

/* Publicly documented shader version helpers (d3dx9shader.h macros). */
#define D3DXSHADER_VERSION_MAJOR(_v) (((_v) >> 16) & 0xFFFFu)
#define D3DXSHADER_VERSION_MINOR(_v) ((_v) & 0xFFFFu)
#define D3DXSHADER_VERSION(_maj, _min) ((uint32_t)(((_maj) << 16) | (_min)))

/* D3D9 shader token stream: a comment is opcode 0xFFFE, a fourcc and data. */
#define D3DSIO_END     0x0000FFFFu
#define D3DSIO_COMMENT 0x0000FFFEu
#define CTAB_FOURCC    0x42415443u   /* 'C','T','A','B' as a little-endian DWORD */

/* D3DXSHADER_CONSTANTTABLE: 28 bytes, all members uint32_t. */
typedef struct _D3DXSHADER_CONSTANTTABLE {
    uint32_t Size;         /* sizeof(D3DXSHADER_CONSTANTTABLE) = 28 */
    uint32_t Creator;      /* byte offset to creator string */
    uint32_t Version;      /* shader version token */
    uint32_t Constants;    /* number of constant records */
    uint32_t ConstantInfo; /* byte offset to D3DXSHADER_CONSTANTINFO[Constants] */
    uint32_t Flags;        /* compile flags */
    uint32_t Target;       /* byte offset to target string */
} D3DXSHADER_CONSTANTTABLE;

/* D3DXSHADER_CONSTANTINFO: 20 bytes. Name/TypeInfo/DefaultValue are byte
 * offsets from the table start, not addresses. */
typedef struct _D3DXSHADER_CONSTANTINFO {
    uint32_t Name;
    uint16_t RegisterSet;
    uint16_t RegisterIndex;
    uint16_t RegisterCount;
    uint16_t Reserved;
    uint32_t TypeInfo;
    uint32_t DefaultValue;
} D3DXSHADER_CONSTANTINFO;

/* D3DXSHADER_TYPEINFO: 16 bytes. StructMemberInfo is also a table offset. */
typedef struct _D3DXSHADER_TYPEINFO {
    uint16_t Class;
    uint16_t Type;
    uint16_t Rows;
    uint16_t Columns;
    uint16_t Elements;
    uint16_t StructMembers;
    uint32_t StructMemberInfo;
} D3DXSHADER_TYPEINFO;

/* HRESULTs the public D3DX9 API returns. */
#define D3D_OK            0x00000000u
#define D3DERR_INVALIDCALL 0x8876086Cu
#define D3DERR_OUTOFVIDEOMEMORY 0x8876017Cu
#define E_NOTIMPL         0x80004001u
#define E_FAIL            0x80004005u

/* D3DXPARAMETERTYPE values used by D3DXSHADER_TYPEINFO.Type to pick the
 * register file for SetValue/SetDefaults (public d3dx9shader.h). */
#define D3DXPT_BOOL   1u
#define D3DXPT_INT    2u
#define D3DXPT_FLOAT  3u

/* A shader buffer is a token stream: DWORD 0 is the version token, a 0xFFFE
 * comment may follow at DWORD 1, and the stream ends with an END token. There
 * is no size field in the stream. */

/* D3DXCONSTANTTABLE_DESC returned by ID3DXConstantTable::GetDesc. */
typedef struct _D3DXCONSTANTTABLE_DESC {
    uint32_t Creator;      /* guest VA of the creator string, 0 when absent */
    uint32_t Version;
    uint32_t Constants;
} D3DXCONSTANTTABLE_DESC;

/* D3DXCONSTANT_DESC written by GetConstantDesc (d3dx9shader.h, 12 DWORDs). */
typedef struct _D3DXCONSTANT_DESC {
    uint32_t Name;           /* guest VA of the constant's name */
    uint32_t RegisterSet;
    uint32_t RegisterIndex;
    uint32_t RegisterCount;
    uint32_t Class;
    uint32_t Type;
    uint32_t Rows;
    uint32_t Columns;
    uint32_t Elements;
    uint32_t StructMembers;
    uint32_t Bytes;
    uint32_t DefaultValue;   /* guest VA, or 0 */
} D3DXCONSTANT_DESC;

/* The four D3DX shader helpers (defined in d3dx9_consttable.c, stdcall). */
void s_D3DXGetShaderVersion(void);
void s_D3DXGetShaderSize(void);
void s_D3DXFindShaderComment(void);
void s_D3DXGetShaderConstantTable(void);

#endif /* WIN32HLE_D3DX9_CONSTTABLE_H */
