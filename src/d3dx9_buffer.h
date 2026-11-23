#ifndef WIN32HLE_D3DX9_BUFFER_H
#define WIN32HLE_D3DX9_BUFFER_H

#include <stdint.h>

/* D3DXCreateBuffer(size, &out): creates an ID3DXBuffer with a zeroed payload. */
void s_D3DXCreateBuffer(void);
#include <stddef.h>
#include <stdint.h>
/* One ID3DXBuffer implementation for the whole D3DX9 layer; copies the payload. */
uint32_t d3dx9_buffer_new(const void *data, size_t size);

/* The five ID3DXBuffer methods. These shim/symbol names are deliberately
 * distinct from the ID3DXBuffer implementation already in d3dx9_compiler.c
 * (used for compiled shader blobs): win32hle_register() does not reject
 * duplicate names and hle_resolve() returns the first match, so a second
 * object type needs its own registry keys. The guest only ever reaches these
 * through the vtable built below, so the internal names are not ABI-visible. */
void db_QueryInterface(void);
void db_AddRef(void);
void db_Release(void);
void db_GetBufferPointer(void);
void db_GetBufferSize(void);

#endif /* WIN32HLE_D3DX9_BUFFER_H */
