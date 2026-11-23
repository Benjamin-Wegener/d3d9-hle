# d3d9-hle

A general replacement for Direct3D 9 and D3DX9, written as a high-level
emulation (HLE) layer: instead of translating each D3D call at run time like a
wrapper DLL, it implements the `IDirect3D9` / `IDirect3DDevice9` COM interfaces,
the D3DX9 helper library and the Effect framework in portable C on top of a
small Win32 host layer, with a Metal backend for rendering on macOS.

> **Status: alpha.** Large parts work and are tested, but expect gaps, wrong
> corners and API changes. It is not a drop-in `d3d9.dll`. Please do not rely on
> it for anything important yet. Bug reports, especially with a tiny reproducer,
> are welcome.

## What is in it

| Area | State |
|---|---|
| `IDirect3D9`, `IDirect3DDevice9`, resources (textures, cube/volume, surfaces, buffers), swap chains, state blocks, queries, vertex declarations | implemented, tested |
| Fixed-function pipeline (transforms, lights, texture stages, fog, alpha/stencil) | implemented on Metal, tested with pixel checks |
| Vertex and pixel shaders (up to model 3.0, see the pixel tests) | translated to Metal shading language |
| D3DX9 math, meshes, shapes, textures and surfaces, fonts, sprites, lines, skinning, animation, spherical harmonics, matrix stack | implemented, tested |
| D3DX9 effects (`.fx` text and binary), HLSL | effect framework implemented; HLSL compile needs the optional vkd3d-shader library |
| Unsupported or not yet complete parts | report an explicit error or log line instead of pretending to work |

Unimplemented behaviour is meant to fail loudly. If you find a call that silently
does nothing, that is a bug.

## Layout

```
src/                 the library (D3D9, D3DX9, Metal backend, small Win32 host layer)
tests/selftests/     unit tests with a mock GPU backend, no GPU needed
tests/pixel/         Metal pixel tests: draw through the public API, read back pixels
```

The library is driven through the same entry points a Windows program would
call. A recompiler, loader or emulator that runs x86 code needs to sit above it;
that part is not in this repository.

## Build and test

macOS, a C compiler, `pkg-config`, SDL2 and SDL2_ttf (`brew install sdl2 sdl2_ttf`):

```sh
make            # build the objects
make test       # selftests, no GPU needed
make pixel      # Metal pixel tests, needs a GPU
```

HLSL source effects additionally need vkd3d-shader:
`HLE_VKD3D_SHADER=/path/to/libvkd3d-shader.dylib make pixel`.

Tests run with a 60 second limit each; exit code 142 means a test hung.

## Known issues

- Only the Metal backend exists, so rendering is macOS only. The selftests need
  no GPU and run anywhere SDL2 builds.
- Vertex, index and instance buffers are copied when they are bound for a draw,
  so that a later `Lock` cannot change a draw that has not reached the GPU yet.
  That is correct but costs a copy per draw; a cheaper scheme is still open.

## Recent changes

- `Present` now shows the back buffer in the SDL window (a `CAMetalLayer`),
  including the gamma ramp and scaling with `pSrcRect`/`pDestRect`; hidden
  windows and the SDL dummy driver still render off-screen only.
- Pre-transformed vertices (`D3DFVF_XYZRHW` / `POSITIONT`) use screen pixels
  relative to the viewport, as documented; world, view and projection are not
  applied to them.
- Fixed a race between `Present` and reading back the render target
  (`test_d3d9_gamma_ramp` is stable again).
- More fixed-function coverage (lights, texture stage operations, texture
  coordinate generation), alpha test and stencil, sampler address modes,
  `ProcessVertices`, `D3DPERF_*`, effect pools and texture format conversion.

## Contributing

Open an issue or a pull request. Keep to the public Microsoft documentation as
the reference, add a test with hand-computed expected values for every
behaviour you change, and do not paste code from Microsoft binaries, Wine or any
other source with an incompatible licence.

## Licence

Apache License 2.0, see `LICENSE`. Parts derived from pcrecomp are also
available under the MIT licence, see `NOTICE` and `LICENSES/`.
