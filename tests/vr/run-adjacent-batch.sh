#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
test_build=$(mktemp -d "${TMPDIR:-/tmp}/sonicr-vr-batch.XXXXXX")
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
python3 - "$test_build/legacy_draw.h" <<'PYREF'
import hashlib,os,pathlib,subprocess,sys
reference=os.environ.get('SONICR_TEST_LEGACY_BACKEND')
if reference and not pathlib.Path(reference).is_absolute():
    raise SystemExit('SONICR_TEST_LEGACY_BACKEND must be absolute')
source=pathlib.Path(reference).read_text() if reference else subprocess.check_output(['git','show','f2491a8:source/sdl/src/r_gl_backend.c'],text=True)
a=source.index('void R_DrawTriFan(');b=source.index('void R_DrawTri(',a)
body=source[a:b]
if hashlib.sha256(body.encode()).hexdigest()!='269c066dd7552dd16a6e8c6063bee9ea8f13399db8228e0dedf4c1a654741579':
    raise SystemExit('Legacy fan function hash mismatch')
pathlib.Path(sys.argv[1]).write_text(body.replace('void R_DrawTriFan(', 'static void Legacy_DrawTriFan(',1))
PYREF
${CC:-cc} -Os -std=c11 -D_POSIX_C_SOURCE=200809L -DSONICR_VR=1 -DSONICR_SDL \
 -Wno-deprecated-declarations -ffunction-sections -fdata-sections \
 -I"$test_build" -I"${OPENXR_INCLUDE:-/opt/openxr/include}" $(sdl2-config --cflags) \
 -Isource/sdl/src -Isource/sdl/src/platform tests/vr/test_adjacent_batch.c \
 source/sdl/src/globals.c source/sdl/src/globals_extra.c \
 $strip_flags -lm -o "$test_build/adjacent-batch"
"$test_build/adjacent-batch" "$test_build"
