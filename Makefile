# d3d9-hle - build the library objects, then run the tests.
#
#   make            build all objects
#   make test       build and run the selftests (no GPU needed)
#   make pixel      build and run the Metal pixel tests (macOS, needs a GPU)
#   make clean
#
# Needs a C compiler, pkg-config, SDL2 and SDL2_ttf. On macOS: brew install sdl2 sdl2_ttf
CC       ?= clang
SDLFLAGS := $(shell pkg-config --cflags sdl2 SDL2_ttf)
SDLLIBS  := $(shell pkg-config --libs sdl2 SDL2_ttf)
CFLAGS   ?= -O1 -Wall -Wno-unused-function -Wno-unused-but-set-variable
CFLAGS   += -DRECOMP_MEM_BASE=0x100000000000ull -Isrc $(SDLFLAGS)

SRCS := $(wildcard src/*.c)
OBJS := $(patsubst src/%.c,build/obj/%.o,$(SRCS))

all: $(OBJS)

build/obj/%.o: src/%.c | build/obj
	$(CC) $(CFLAGS) -c $< -o $@

build/obj/d3d9_metal.o: src/d3d9_metal.m | build/obj
	$(CC) $(CFLAGS) -fno-objc-arc -c $< -o $@

build/obj:
	mkdir -p build/obj

test: $(OBJS)
	CC="$(CC)" CFLAGS="$(CFLAGS)" SDLLIBS="$(SDLLIBS)" sh tests/run_selftests.sh

pixel: $(OBJS) build/obj/d3d9_metal.o
	CC="$(CC)" CFLAGS="$(CFLAGS)" SDLLIBS="$(SDLLIBS)" sh tests/run_pixel.sh

clean:
	rm -rf build

.PHONY: all test pixel clean
