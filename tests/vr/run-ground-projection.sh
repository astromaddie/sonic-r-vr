#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
test_build=$(mktemp -d "${TMPDIR:-/tmp}/sonicr-vr-ground.XXXXXX")
trap 'rm -rf "$test_build"' EXIT HUP INT TERM
case $(uname -s) in Darwin) strip_flags=-Wl,-dead_strip ;; *) strip_flags=-Wl,--gc-sections ;; esac
sources="render_track_d3d.c hud_full.c render_character_d3d.c"
source_dir=source/sdl/src
if [ -n "${SONICR_TEST_LEGACY_REF:-}" ]; then
    source_dir=$test_build
    for file in $sources; do git show "$SONICR_TEST_LEGACY_REF:source/sdl/src/$file" > "$source_dir/$file"; done
fi
${CC:-cc} -std=c11 -DSONICR_VR=${SONICR_TEST_VR:-1} -DSONICR_SDL \
    -ffunction-sections -fdata-sections -Isource/sdl/src \
    tests/vr/test_ground_projection.c "$source_dir/render_track_d3d.c" \
    "$source_dir/hud_full.c" "$source_dir/render_character_d3d.c" \
    source/sdl/src/globals.c source/sdl/src/globals_extra.c \
    $strip_flags -lm -o "$test_build/ground-projection"
"$test_build/ground-projection" "$@"
