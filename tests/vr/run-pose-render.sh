#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
test_build=$(mktemp -d "${TMPDIR:-/tmp}/sonicr-vr-pose.XXXXXX")
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
a=source.index('static inline void R_EmitVertex(');b=source.index('void R_DrawTriFan(',a)
emitter=source[a:b]
if hashlib.sha256(emitter.encode()).hexdigest()!='28997d1f663c9cf385d8566ca42f82b2054c84172b540015ad99f28e908f47bb':
    raise SystemExit('Legacy vertex emitter hash mismatch')
pathlib.Path(sys.argv[1]).write_text(emitter.replace('static inline void R_EmitVertex(', 'static inline void Legacy_BackendEmitVertex(',1)+body.replace('void R_DrawTriFan(', 'static void Legacy_DrawTriFan(',1).replace('R_EmitVertex(', 'Legacy_BackendEmitVertex('))
PYREF
python3 - "$test_build/legacy_pose.h" <<'PYPOSE'
import hashlib,os,pathlib,subprocess,sys
root=os.environ.get('SONICR_TEST_POSE_BASELINE')
if root and not pathlib.Path(root).is_absolute():
    raise SystemExit('SONICR_TEST_POSE_BASELINE must be an absolute baseline source/sdl/src directory')
def read(path):
    return (pathlib.Path(root)/path).read_text() if root else subprocess.check_output(['git','show','2ebbb33:source/sdl/src/'+path],text=True)
def check(text,expected):
    if hashlib.sha256(text.encode()).hexdigest()!=expected:
        raise SystemExit('Pose baseline hash mismatch: expected installed 2ebbb33 production source')
    return text
def function(path,name,sha):
    source=read(path)
    a=source.index('void '+name+'(');b=source.index('{',a);depth=1;i=b+1
    while depth:
        depth += (source[i]=='{')-(source[i]=='}');i+=1
    return check(source[a:i],sha)
a=function('vr/vr.c','transformVertex','d5b2264a06d5ccee7a3f3883fbee707345d2d4309cede0cc9678e4d5fc19cb42').replace('void transformVertex(', 'static void LegacyPoseTransform(')
b=function('render_track_d3d.c','RenderParallaxStripsD3D','0960844e7c13f99a04a0196928f0f13a91bb21ab6ae86270782297c8515139b3').replace('void RenderParallaxStripsD3D(', 'static void LegacyPanorama(').replace('EmitSkyWaterQuad(', 'R_DrawQuad(').replace('recip256','(1.0f/256)')
c=""
c+=function('render_track_d3d.c','SubmitGroundShadow','981d60932f1cef03882be597367737746ec745df35029e8df730eafef1db292f').replace('void SubmitGroundShadow(', 'static void Legacy_SubmitGroundShadow(')+'\n'
c+=function('hud_full.c','DrawFootShadows','35c22b17e32f82378d5ba927a12785ba524b6cd73faa8de9089bfb0bab77f9be').replace('void DrawFootShadows(', 'static void Legacy_DrawFootShadows(')+'\n'
c+=function('hud_full.c','DrawMovingPickupShadow','2a853ee12f8793f20a62a2818bf1eec783649c6ad00d48c3c67af260963be11d').replace('void DrawMovingPickupShadow(', 'static void Legacy_DrawMovingPickupShadow(')+'\n'
pathlib.Path(sys.argv[1]).write_text(a+'\n'+b+'\n'+c)
pathlib.Path(sys.argv[1]).with_name('legacy_track.c').write_text(check(read('render_track_sdl.c'),'7bf415ea94ca25cf3e1bfa235e2a89b0c938e20e50195e6c5559382ba8010017'))
PYPOSE
if [ "${SONICR_TEST_COMFORT_ONLY:-0}" = 1 ]; then
python3 - "$test_build/legacy_camera.h" <<'PYCAMERA'
import hashlib,os,pathlib,subprocess,sys
root=os.environ.get('SONICR_TEST_CAMERA_BASELINE')
if root and not pathlib.Path(root).is_absolute():
    raise SystemExit('SONICR_TEST_CAMERA_BASELINE must be an absolute baseline source/sdl/src directory')
source=(pathlib.Path(root)/'vr/vr.c').read_text() if root else subprocess.check_output(['git','show','d298137:source/sdl/src/vr/vr.c'],text=True)
a=source.index('void VR_ApplyCamera(');b=source.index('void VR_HudBegin',a)
body=source[a:b]
assert hashlib.sha256(body.encode()).hexdigest()=='13f3fcf4eeae0d7e9b1878ca643e0c2d5f6d412c248b318bcd47942773cd3715'
pathlib.Path(sys.argv[1]).write_text(body.replace('void VR_ApplyCamera(', 'static void Baseline_ApplyCamera(',1))
PYCAMERA
fi
fixture_source=tests/vr/test_pose_render.c
if [ "${SONICR_TEST_COMFORT_ONLY:-0}" = 1 ]; then fixture_source=tests/vr/test_comfort.c; fi
${CC:-cc} -Os -std=c11 -DSONICR_VR=1 -DSONICR_SDL -DRenderTrackD3D=Legacy_RenderTrackD3D \
 -ffunction-sections -fdata-sections -Isource/sdl/src -c "$test_build/legacy_track.c" -o "$test_build/legacy_track.o"
${CC:-cc} -Os -std=c11 -DSONICR_VR=1 -DSONICR_SDL -DRenderHUD=Production_RenderHUD \
 -ffunction-sections -fdata-sections -Isource/sdl/src -c source/sdl/src/hud_full.c -o "$test_build/hud.o"
${CC:-cc} -Os -std=c11 -D_POSIX_C_SOURCE=200809L -DSONICR_VR=1 -DSONICR_SDL \
 -Wno-deprecated-declarations -ffunction-sections -fdata-sections \
 -I"$test_build" -I"${OPENXR_INCLUDE:-/opt/openxr/include}" $(sdl2-config --cflags) \
 -Isource/sdl/src -Isource/sdl/src/platform "$fixture_source" "$test_build/legacy_track.o" "$test_build/hud.o" source/sdl/src/render_track_d3d.c source/sdl/src/vr/vr_water.c source/sdl/src/render_helpers.c source/sdl/src/camera.c source/sdl/src/pause_menu.c source/sdl/src/sky_render.c source/sdl/src/input/input.c \
 source/sdl/src/globals.c source/sdl/src/globals_extra.c \
 $strip_flags -lm -o "$test_build/pose-render"
if "$test_build/pose-render" "$test_build" 2>"$test_build/diagnostic.log"; then :; else
 tail -30 "$test_build/diagnostic.log" >&2
 exit 1
fi

if [ "${SONICR_TEST_COMFORT_ONLY:-0}" = 1 ]; then exit 0; fi
python3 - "$test_build/stream.jsonl" <<'PYJSON'
import json,sys,math
rows=[json.loads(line) for line in open(sys.argv[1])]
assert len(rows)==2 and len(rows[0]['basis'])==9 and rows[0]['camera']==0
r=rows[1]
assert r['eye']==0 and r['stage']==2 and r['paused']==1 and r['reverse']==1 and r['tick']==90
assert len(bytes.fromhex(r['state']))==60
assert len(r['vertices'])==1 and len(r['vertices'][0])==11
assert all(math.isfinite(x) for x in r['vertices'][0])
print('CPU producer stream: independently parsed JSON, full source/output vertices, native paused state and effective backend state present')
PYJSON
