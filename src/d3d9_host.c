/*
 * d3d9_host.c - minimal host initialiser for the d3d9-hle test programs.
 *
 * Registers only the shim tables this repository ships: Direct3D 9, D3DX9 and
 * the small slice of the Win32 HLE layer they depend on (kernel32, GDI, COM).
 *
 * Copyright 2026 the d3d9-hle authors. Licensed under the Apache License 2.0.
 */
#include <stdint.h>
#include "win32hle.h"

extern const win32hle_shim win32hle_kernel32[], win32hle_kernel32_crt[], win32hle_gdidc[],
    win32hle_gdi32[], win32hle_ole32[], win32hle_d3d9[], win32hle_d3dx9[], win32hle_d3d9fx[],
    win32hle_d3d9fxbin[], win32hle_d3dx9_compiler[], win32hle_d3dx9_fxsource[];

void recomp_host_init(void) {
    win32hle_register(win32hle_kernel32);
    win32hle_register(win32hle_kernel32_crt);
    win32hle_register(win32hle_gdidc);
    win32hle_register(win32hle_gdi32);
    win32hle_register(win32hle_ole32);
    win32hle_register(win32hle_d3d9);
    win32hle_register(win32hle_d3dx9);
    win32hle_register(win32hle_d3d9fx);
    win32hle_register(win32hle_d3d9fxbin);
    win32hle_register(win32hle_d3dx9_compiler);
    win32hle_register(win32hle_d3dx9_fxsource);
}
