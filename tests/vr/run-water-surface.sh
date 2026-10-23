#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
test_build=$(mktemp -d "${TMPDIR:-/tmp}/sonicr-vr-water.XXXXXX")
trap 'rm -rf "$test_build"' EXIT HUP INT TERM
${CC:-cc} -std=c11 -DSONICR_VR=1 -DGL_SILENCE_DEPRECATION -Isource/sdl/src \
    tests/vr/test_water_surface.c source/sdl/src/vr/vr_water.c -lm -o "$test_build/water-surface"
"$test_build/water-surface"
