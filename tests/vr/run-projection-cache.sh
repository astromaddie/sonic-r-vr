#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
test_build=$(mktemp -d "${TMPDIR:-/tmp}/sonicr-vr-cache.XXXXXX")
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
python3 - "$test_build/legacy_emitter.h" <<'PY'
import hashlib,os,pathlib,subprocess,sys
reference=os.environ.get('SONICR_TEST_LEGACY_SOURCE')
if reference and not pathlib.Path(reference).is_absolute():
    raise SystemExit('SONICR_TEST_LEGACY_SOURCE must be an absolute baseline source path')
source=pathlib.Path(reference).read_text() if reference else subprocess.check_output(['git','show','f2491a8:source/sdl/src/vr/vr.c'],text=True)
a=source.index('void VR_EmitVertex(');b=source.index('int VR_Present(',a)
emitter=source[a:b]
expected='fddcecae21097cb10d898e5ab5c0f68964ebbc74d18879895a3d4bee6688044b'
if hashlib.sha256(emitter.encode()).hexdigest()!=expected:
    raise SystemExit('Legacy emitter hash mismatch: expected frozen uncached production source')
pathlib.Path(sys.argv[1]).write_text(emitter.replace('void VR_EmitVertex(', 'static void Legacy_EmitVertex(',1))
PY
${CC:-cc} ${TEST_OPT:--Os} -std=c11 -DSONICR_VR=1 -DSONICR_SDL \
 -Wno-deprecated-declarations -ffunction-sections -fdata-sections \
 -I"$test_build" -I"${OPENXR_INCLUDE:-/opt/openxr/include}" $(sdl2-config --cflags) \
 -Isource/sdl/src -Isource/sdl/src/platform tests/vr/test_projection_cache.c \
 source/sdl/src/globals.c source/sdl/src/globals_extra.c \
 $strip_flags -lm -o "$test_build/projection-cache"
"$test_build/projection-cache"
