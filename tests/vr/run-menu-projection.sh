#!/bin/sh
# Host-only production BeginFrame projection regression; no SDL window/GL context.
set -eu
cd "$(dirname "$0")/../.."
TEST_CC=${CC:-cc}
test_build=$(mktemp -d "${TMPDIR:-/tmp}/sonicr-vr-menu-projection.XXXXXX")
cleanup() { python3 -c 'import shutil,sys; shutil.rmtree(sys.argv[1])' "$test_build"; }
trap cleanup EXIT HUP INT TERM
case $(uname -s) in
    Darwin) strip_flags=-Wl,-dead_strip ;;
    *) strip_flags=-Wl,--gc-sections ;;
esac
"$TEST_CC" -std=c11 -DSONICR_VR=0 -DSONICR_SDL -DGL_SILENCE_DEPRECATION \
    -ffunction-sections -fdata-sections -Isource/sdl/src -Isource/sdl/src/platform \
    $(sdl2-config --cflags) tests/vr/test_menu_projection.c \
    source/sdl/src/render_gl.c source/sdl/src/globals.c \
    $strip_flags -lm -o "$test_build/menu-projection"
"$test_build/menu-projection"
