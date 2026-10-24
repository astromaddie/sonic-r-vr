#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
test_build=$(mktemp -d "${TMPDIR:-/tmp}/sonicr-vr-weather.XXXXXX")
trap 'rm -rf "$test_build"' EXIT HUP INT TERM
case $(uname -s) in Darwin) strip_flags=-Wl,-dead_strip ;; *) strip_flags=-Wl,--gc-sections ;; esac
source_file=source/sdl/src/weather.c
if [ -n "${SONICR_TEST_LEGACY_REF:-}" ]; then
    source_file=$test_build/weather.c
    git show "$SONICR_TEST_LEGACY_REF:source/sdl/src/weather.c" > "$source_file"
fi
${CC:-cc} -std=c11 -DSONICR_VR=${SONICR_TEST_VR:-1} -DSONICR_SDL \
    -ffunction-sections -fdata-sections -Isource/sdl/src \
    tests/vr/test_weather_projection.c "$source_file" source/sdl/src/race_timing.c \
    source/sdl/src/globals.c source/sdl/src/globals_extra.c \
    $strip_flags -lm -o "$test_build/weather-projection"
"$test_build/weather-projection" "$@"
