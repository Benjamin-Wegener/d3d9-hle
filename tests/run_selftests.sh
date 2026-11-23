#!/bin/sh
# Link and run every tests/selftests/*_selftest.c against the library objects.
# The selftests use a mock GPU backend, so the Metal backend is not linked.
# Every test runs with a 60 s limit; exit 142 means it hung.
cd "$(dirname "$0")/.." || exit 1
mkdir -p build/bin
OBJS=$(ls build/obj/*.o | grep -v d3d9_metal.o)
fail=0; n=0
for t in tests/selftests/*_selftest.c; do
  b=$(basename "$t" .c)
  if ! $CC $CFLAGS -Isrc "$t" $OBJS $SDLLIBS -lm -lpthread -o "build/bin/$b" 2> "build/bin/$b.log"; then
    echo "BUILD FAIL $b"; cat "build/bin/$b.log"; fail=1; continue
  fi
  SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy /usr/bin/perl -e 'alarm shift; exec @ARGV or die' 60 "build/bin/$b" > "build/bin/$b.out" 2>&1
  rc=$?
  if [ $rc -eq 0 ]; then echo "ok    $b"; n=$((n+1)); else echo "FAIL  $b (rc=$rc)"; tail -5 "build/bin/$b.out"; fail=1; fi
done
echo "$n selftests passed"
exit $fail
