# Sonic R Steam Frame port: native bootstrap and VR gates

## Current milestone

The source builds as a native Linux ARM64 desktop game with `SONICR_VR=0` and
renders a supplied-data Resort Island attract race under Xvfb/llvmpipe. The
separate OpenXR/GLX colour probe statically links the OpenXR loader and bundles
private SDL2_mixer codec/audio-support plus miniupnpc libraries. The first
package and imported assets were copied to the Frame and their SHA-256 manifests
matched before and after the initial Steam launch.

The first probe stopped at `SDL_CreateWindow` with `Couldn't find matching GLX
visual`, before OpenXR initialization. A subsequent read-only dynamic-loader
check found a concrete dependency conflict: the package's older
`libstdc++.so.6` lacks `CXXABI_1.3.15` required by the Frame's Mesa Gallium
library and `GLIBCXX_3.4.32` required by its SPIR-V tools. Both system libraries
resolve without those errors when using the system C++ runtime. The original
bundle also shadows the host's readline library and breaks the `ldd` shell
interpreter. This is a packaging fault; context-profile changes are not yet
supported by the evidence.

The corrected stager now follows immediate ELF `DT_NEEDED` entries, recurses
through private codec/network dependencies, and stops at system ABI, audio,
display and graphics roots. In particular, C++, GCC support and readline use
the Frame's libraries. Regression tests cover host-subtree pruning, private
transitive closure, missing libraries, conflicting SONAMEs, and loader errors
that return zero. A corrected bundle still needs a fresh normal Steam launch;
the dependency diagnosis alone does not establish session creation.

The package contains no original game archive or user game data. Import owned
PC data into `${XDG_DATA_HOME:-$HOME/.local/share}/sonic-r-vr/data`; saves and
settings remain in that external data directory. The source checkout and
generated package are not data locations.

## Renderer, camera, and timing facts

The SDL platform creates a 640×480 desktop window, asks for a double-buffered
OpenGL compatibility context with a 24-bit depth buffer, and enables swap
interval 1. Fullscreen is an existing command-line option. Game state keeps a
640×480 logical canvas and its 4:3 viewport layout.

The GL renderer translates the original Direct3D-style drawing path into
fixed-function OpenGL. Game render code clips and projects track/model geometry
into the logical screen viewport; the GL backend emits textured polygons with
perspective-correct reciprocal-W vertices and maintains the original texture
page/draw ordering. It is not a standard world-space OpenGL camera pipeline
where a different projection matrix alone produces another eye view. A stereo
path must show how geometry and camera state are produced for each eye before
it can preserve depth and parallax.

For a normal race tick, `main.c` advances simulation, selects a camera per
viewport, calls `RenderHUD` once, then swaps with `FlipD3D` and waits in
`WaitForFrameCap`. The desktop cap is 33 ms (30 updates per second); that wait
continues pumping platform events. Menus and screens have their own loops and
also use the same 30 Hz cap. The current loop does not call OpenXR frame APIs
and does not separate rendering from simulation ticks.

`BuildCameraView` computes a rear-follow position from the player, camera
distance, and height, then converges camera position and yaw/pitch toward their
targets. `UpdateCameraConfig` can step distance/height for the existing camera
control. Camera state is per viewport, and the main loop has a guard against
calling its smoothing update twice in a frame. This is the flat-game camera;
there is no tracked head pose in the game source.

The race draw path in `hud_full.c` includes the background, parallax strips,
water reflection, playfield grid/sky, timer/status, track geometry, character
models, item boxes, collect effects, pickup and foot shadows, ground/weather
particles, and optional replay `R` model. Full-screen pause, split-screen
border, and fade overlays are drawn after each viewport. Menus and logo screens
use separate 2D/screen-space render paths.

Sonic R's `.son` music path is explicitly supported by `music_sdl.c`: it streams
headerless 44.1 kHz, 16-bit stereo PCM through the SDL_mixer WAV backend. The
importer therefore preserves literal lowercase `MUSIC/trackN.son` names while
normalizing the PC asset folders and file names to uppercase as the desktop
documentation requires.

These facts were traced in [`main.c`](../source/sdl/src/main.c),
[`platform_sdl.c`](../source/sdl/src/platform/platform_sdl.c),
[`render_scene.c`](../source/sdl/src/render_scene.c),
[`hud_full.c`](../source/sdl/src/hud_full.c),
[`render_gl.c`](../source/sdl/src/render_gl.c),
[`r_gl_backend.c`](../source/sdl/src/r_gl_backend.c),
[`screens.c`](../source/sdl/src/screens.c), and
[`music_sdl.c`](../source/sdl/src/sound/music_sdl.c).

## Agreed chase-camera direction

The game remains third person, following the racer from behind. The chase
camera should move smoothly in the spirit of F-Zero X. Head tracking should
rotate the view immediately with no added head-pose smoothing. This keeps
chase damping separate from directly responsive head rotation. No UI,
controls, or assets are being added in this bootstrap milestone.

`SONICR_VR` is currently only a validated build define, defaulting to `0`.
Future game-side VR code must stay behind that flag. No incomplete camera
module or speculative stereo renderer has been added.

## Remaining gates

| Gate | State | Evidence still needed |
|---|---|---|
| Linux ARM64 flat build and real game-data render | Complete on host | Frame performance and input remain unmeasured |
| Native package installation | Package and asset manifests matched | Replace the faulty dependency bundle before the next Steam launch |
| Standalone sRGB OpenXR/GLX color probe | First Steam launch failed at GLX visual selection | Correct dependency shadowing, then confirm session creation, both eye images, format, tracking validity, standby frames and orderly shutdown |
| Game's first stereo scene | Not started | Select a compatible way to render both eyes through the screen-projected drawing pipeline |
| Third-person camera plus immediate head pose | Design agreed, not implemented | Verify eye separation, tracked rotation, camera movement and comfort in headset |
| OpenXR pacing and 30 Hz game simulation | Not started | Separate prediction/render timing from simulation without advancing camera smoothing twice |
| Frame input, suspend/standby, audio, sustained gameplay | Not started | Device checks and user playtest after the separate device authorization gate |

## Initial device reproduction

The installed baseline is commit `e376997cd42abbca56f2262dc036b30e604c1a3d`,
under `/home/steamos/devkit-game/SonicRVR`, with data under
`/home/steamos/.local/share/sonic-r-vr/data`. The 68 package manifest entries and
449 imported data files matched their local counterparts. The game binary
SHA-256 is `4cc1420a236a51ddb9c08a8f3b22e3fc56f04959f5a4ba6c5ce3a56fd49de39c`;
the probe is `d32608bd916d1372819d4a959b998115b099e5585c25f07cb9a7b4e309aaf01f`.

The probe was launched through Steam with `run-openxr-probe 5` and
`SONICR_PROBE_FORCE_COLOR_ONLY=1`, using `SteamLinuxRuntime_4-arm64`. This mode
checks swapchain colour clears without submitting projection layers. The
2026-10-23 launch exited at GLX window creation, so it produced no session,
swapchain, or colour-readback evidence. The planned flat-game smoke run was
skipped after that failure. Diagnostic arguments were removed, the normal
`run-sonicr` shortcut was restored, and no Sonic R process remained.

To reproduce the dependency diagnosis without opening a display, run the
following on the Frame and inspect stderr as well as the exit status:

```sh
/lib/ld-linux-aarch64.so.1 \
  --library-path /home/steamos/devkit-game/SonicRVR/lib \
  --list /usr/lib/libGLX_mesa.so.0
```

Compare with the same command without `--library-path`. The loader returned
zero even with missing symbol-version errors in stderr. Compact manifests,
journal extracts and the loader result are retained locally under the ignored
`build/frame-linux-aarch64/device` directory.

The probe launcher selects `/opt/steamvr/steamxr_linuxarm64.json` when
`XR_RUNTIME_JSON` is unset and that file exists. Further device operations use
the shared Frame booking and explicit headset-off authorization procedure.
