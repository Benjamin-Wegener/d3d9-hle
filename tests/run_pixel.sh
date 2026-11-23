#!/bin/sh
# Build and run the Metal pixel tests. They draw only through the public
# IDirect3D9/IDirect3DDevice9 calls and read the result back from the Metal
# render target. The .fx source test needs the optional vkd3d-shader library:
#   HLE_VKD3D_SHADER=/path/to/libvkd3d-shader.dylib make pixel
cd "$(dirname "$0")/.." || exit 1
[ -n "$HLE_VKD3D_SHADER" ] && export HLE_VKD3D_SHADER
mkdir -p build/bin
fail=0; n=0
for t in tests/pixel/test_*.c; do
  b=$(basename "$t" .c)
  if [ "$b" = test_d3d9_fx_effect ] && [ -z "$HLE_VKD3D_SHADER" ]; then echo "skip  $b (set HLE_VKD3D_SHADER to run it)"; continue; fi
  if ! $CC $CFLAGS -Isrc -Itests/pixel "$t" build/obj/*.o $SDLLIBS -lm -lpthread \
        -framework Metal -framework Foundation -framework QuartzCore -o "build/bin/$b" 2> "build/bin/$b.log"; then
    echo "BUILD FAIL $b"; cat "build/bin/$b.log"; fail=1; continue
  fi
  out=$(/usr/bin/perl -e 'alarm shift; exec @ARGV or die' 60 "build/bin/$b" 2>&1); rc=$?
  if [ $rc -eq 0 ]; then echo "ok    $b"; n=$((n+1)); else echo "FAIL  $b (rc=$rc)"; echo "$out" | tail -8; fail=1; fi
done
echo "$n pixel tests passed"
exit $fail
