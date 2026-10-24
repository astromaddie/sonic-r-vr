#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
test_build=$(mktemp -d "${TMPDIR:-/tmp}/sonicr-vr-perf.XXXXXX")
trap 'rm -rf "$test_build"' EXIT HUP INT TERM
case $(uname -s) in
 Darwin)
  strip_flags=-Wl,-dead_strip
  mkdir "$test_build/GL"
  sdk=$(xcrun --show-sdk-path)
  ln -s "$sdk/System/Library/Frameworks/OpenGL.framework/Headers/gl.h" "$test_build/GL/gl.h"
  ln -s "$sdk/System/Library/Frameworks/OpenGL.framework/Headers/glext.h" "$test_build/GL/glext.h"
  ;;
 *) strip_flags=-Wl,--gc-sections ;;
esac
${CC:-cc} -std=c11 -D_POSIX_C_SOURCE=200809L -DSONICR_VR=1 -DSONICR_SDL \
 -Wno-deprecated-declarations -ffunction-sections -fdata-sections \
 -I"$test_build" -I"${OPENXR_INCLUDE:-/opt/openxr/include}" $(sdl2-config --cflags) \
 -Isource/sdl/src -Isource/sdl/src/platform tests/vr/test_perf_fixture.c \
 source/sdl/src/globals.c source/sdl/src/globals_extra.c \
 $strip_flags -lm -o "$test_build/perf-fixture"
"$test_build/perf-fixture"

# macOS SDL omits X11 WM fields; expose its real X11 struct for the CPU-only
# OpenXR protocol fixture. Production Linux uses its normal SDK header.
if [ "$(uname -s)" = Darwin ]; then
 printf '#include <SDL.h>\n#define SDL_VIDEO_DRIVER_X11 1\n#include <%s/SDL_syswm.h>\n' "$(sdl2-config --prefix)/include/SDL2" > "$test_build/SDL_syswm.h"
fi
${CC:-cc} -std=c11 -DSONICR_VR=1 -DSONICR_SDL \
 -Wno-deprecated-declarations -ffunction-sections -fdata-sections \
 -I"$test_build" -I/opt/X11/include -I"${OPENXR_INCLUDE:-/opt/openxr/include}" $(sdl2-config --cflags) \
 -Isource/sdl/src -Isource/sdl/src/platform tests/vr/test_offhead_fixture.c \
 $strip_flags -lm -o "$test_build/offhead-fixture"
"$test_build/offhead-fixture"
