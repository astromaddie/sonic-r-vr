#!/bin/sh
# Host-only: production action/pad/input code, fake OpenXR API, no GL context.
set -eu
cd "$(dirname "$0")/../.."
OPENXR_INCLUDE=${OPENXR_INCLUDE:-/opt/openxr/include}
TEST_CC=${CC:-cc}
test_build=$(mktemp -d "${TMPDIR:-/tmp}/sonicr-vr-input.XXXXXX")
cleanup() { python3 -c 'import shutil,sys; shutil.rmtree(sys.argv[1])' "$test_build"; }
trap cleanup EXIT HUP INT TERM
case $(uname -s) in
    Darwin) strip_flags=-Wl,-dead_strip ;;
    *) strip_flags=-Wl,--gc-sections ;;
esac
# The production SDL file includes mixer declarations; unused audio functions
# are discarded. An SDK include directory may supply SDL_mixer.h for the host.
mixer_flags=$(pkg-config --cflags SDL2_mixer 2>/dev/null || true)
"$TEST_CC" -std=c11 -Wall -Wextra -Werror -DSONICR_VR=1 \
    -Isource/sdl/src -I"$OPENXR_INCLUDE" -fsyntax-only source/sdl/src/vr/vr_input.c
# Intentional shell splitting of compiler/pkg-config flags.
# shellcheck disable=SC2046,SC2086
"$TEST_CC" -std=c11 -DSONICR_VR=1 -DSONICR_SDL -DGL_SILENCE_DEPRECATION \
    -ffunction-sections -fdata-sections -Isource/sdl/src -Isource/sdl/src/platform \
    -I"$OPENXR_INCLUDE" $(sdl2-config --cflags) $mixer_flags \
    tests/vr/test_input.c source/sdl/src/vr/vr_input.c \
    source/sdl/src/input/input.c source/sdl/src/init.c source/sdl/src/globals.c \
    source/sdl/src/globals_extra.c source/sdl/src/platform/platform_sdl.c \
    $(sdl2-config --libs) $strip_flags -lm -o "$test_build/input"
"$test_build/input"
