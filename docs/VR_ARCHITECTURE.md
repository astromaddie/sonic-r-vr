# Sonic R Steam Frame port: native bootstrap and VR gates

## Current milestone

The source builds as a native Linux ARM64 desktop game with `SONICR_VR=0` and
renders a supplied-data Resort Island attract race under Xvfb/llvmpipe. The
separate OpenXR/GLX colour probe statically links the OpenXR loader and bundles
private SDL2_mixer codec/audio-support plus miniupnpc libraries. The host test
does not prove that the probe starts a Frame session or that the game runs on
the headset.

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

The game remains third person, following the racer from behind; this is not a
cockpit conversion. The vehicle/chase camera should move smoothly in the spirit
of F-Zero X. Head tracking should rotate the view immediately with no added
head-pose smoothing. This separation lets the game's existing chase motion
remain comfortable while head rotation stays directly responsive. No UI,
controls, or assets are being added in this bootstrap milestone.

`SONICR_VR` is currently only a validated build define, defaulting to `0`.
Future game-side VR code must stay behind that flag. No incomplete camera
module or speculative stereo renderer has been added.

## Remaining gates

| Gate | State | Evidence still needed |
|---|---|---|
| Linux ARM64 flat build and real game-data render | Complete on host | Frame performance and input remain unmeasured |
| Standalone sRGB OpenXR/GLX color probe | Built and packaged | Start a Frame session, confirm both eye images, format, tracking validity, standby frames and orderly shutdown |
| Game's first stereo scene | Not started | Select a compatible way to render both eyes through the screen-projected drawing pipeline |
| Third-person camera plus immediate head pose | Design agreed, not implemented | Verify eye separation, tracked rotation, camera movement and comfort in headset |
| OpenXR pacing and 30 Hz game simulation | Not started | Separate prediction/render timing from simulation without advancing camera smoothing twice |
| Frame input, suspend/standby, audio, sustained gameplay | Not started | Device checks and user playtest after the separate device authorization gate |

The probe package uses the Frame runtime path
`/opt/steamvr/steamxr_linuxarm64.json` when `XR_RUNTIME_JSON` is unset and that
file exists. Device install or launch remains a separate action; this host
milestone does not imply it has happened.
