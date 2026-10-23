# Sonic R Steam Frame port: native bootstrap and VR gates

## Current milestone

The native Linux ARM64 game (`SONICR_VR=0`) and a separate OpenXR/GLX probe are
installed on the Frame. The corrected package has 47 manifest entries and 22
private libraries; all installed package hashes match. Original game assets
remain in the external data directory.

The Steam-launched probe creates an OpenXR session with a GL 4.3 compatibility
context on Mesa 26.3.0-devel zink/Turnip Adreno 750 and SteamVR/OpenXR 2.18.2.
Both `GL_SRGB8_ALPHA8` eye swapchains are 1728×1728 with three images each.
The explicit colour-only diagnostic passed both center-pixel checks: left
`46,9,9,255`, right `9,14,46,255`. It cleared both eyes for 343 frames, submitted
zero projection layers, and shut down orderly after five seconds. This verifies
the GLX binding, session, swapchains and diagnostic readbacks. It does not
establish game-side stereo or tracking behavior.

The flat game starts through Steam with the correct native executable and data
working directory. That smoke test established process execution; no usable
game-window capture or race progress evidence was obtained. The real-data Resort
Island render under host Xvfb/llvmpipe remains separate evidence. In source,
`SONICR_DEMO_TRACE` only shortens the title's attract timeout to one second; it
does not emit race diagnostics. The next flat-game gate needs an app capture or
explicit render/progress instrumentation.

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
| Native package installation | Corrected 47-entry package matched on Frame | Build portability still requires the pinned local image |
| Standalone sRGB OpenXR/GLX color probe | Session, both colour readbacks and orderly shutdown verified | Game-side stereo, tracking and visual acceptance remain separate gates |
| Flat game on Frame | Native Steam process launch verified | Obtain a game-window capture or race/render diagnostics, then check simulation, input and persistence |
| Game's first stereo scene | Not started | Select a compatible way to render both eyes through the screen-projected drawing pipeline |
| Third-person camera plus immediate head pose | Design agreed, not implemented | Verify eye separation, tracked rotation, camera movement and comfort in headset |
| OpenXR pacing and 30 Hz game simulation | Not started | Separate prediction/render timing from simulation without advancing camera smoothing twice |
| Frame input, suspend/standby, audio, sustained gameplay | Not started | Device checks and user playtest after the separate device authorization gate |

## Device artifact and reproduction

The installed artifact preserves the executable build from commit
`e376997cd42abbca56f2262dc036b30e604c1a3d` and records the dependency-only repack
at `4021c2daa35d2ba021806b1d151810078331ae97`. It lives under
`/home/steamos/devkit-game/SonicRVR`, with data under
`/home/steamos/.local/share/sonic-r-vr/data`. The package is 14,707,316 bytes and
contains no private C++/GCC runtime, readline or graphics driver. The game binary
SHA-256 is `4cc1420a236a51ddb9c08a8f3b22e3fc56f04959f5a4ba6c5ce3a56fd49de39c`;
the probe is `d32608bd916d1372819d4a959b998115b099e5585c25f07cb9a7b4e309aaf01f`.

The 2026-10-23 colour check ran through Steam with `SteamLinuxRuntime_4-arm64`,
using a temporary per-app wrapper that explicitly exported
`SONICR_PROBE_FORCE_COLOR_ONLY=1` and `SDL_AUDIODRIVER=dummy`, then executed
`run-openxr-probe 5`. The earlier devkit environment registration did not reach
the probe: its actual output identified normal projection mode and one submitted
frame. Verify the mode and result in the application log rather than inferring
them from the title configuration.

The required run logged both `[clear-check] ... PASS` lines and
`COLOR CLEARS VERIFIED; submitted_layers=0; tracking_validity=not_claimed`.
The flat smoke used its own explicit dummy-audio/attract wrapper. It was stopped
after about 39 seconds; no race-render conclusion follows from that run.

Both temporary wrappers were removed, the normal `run-sonicr` shortcut and
original settings were restored, and no Sonic R process remained. The traced
mutable paths (`SONICR.INF`, `JOYSTICK.INF`, `KEYS.BIN`, `ONLINE.DAT`, `SAVE/`,
`GHOST/`, `DEMOS/`) were recorded absent before the flat launch and remained
absent afterward. The installed package manifest passed again. Cleanup removed
the superseded package, recovering 22,188,032 bytes of measured device space;
the shared Frame booking was released and the next queued task notified.

The initial bundle failed GLX visual selection because its older C++ runtime
lacked Mesa's required `CXXABI_1.3.15` and SPIR-V tools' `GLIBCXX_3.4.32`.
It also shadowed host readline. The corrected stager follows immediate ELF
`DT_NEEDED` entries, recurses through private codec/network dependencies and
stops at system ABI, audio, display and graphics roots. Regression tests cover
that boundary, private transitive closure, unresolved libraries, SONAME conflicts,
absolute loader paths, and loader errors with zero exit status.

Check the current Mesa dependency resolution without opening a display:

```sh
/lib/ld-linux-aarch64.so.1 \
  --library-path /home/steamos/devkit-game/SonicRVR/lib \
  --list /usr/lib/libGLX_mesa.so.0
```

The corrected bundle resolves Mesa's libraries from `/usr/lib` with empty
stderr. Inspect stderr as well as the exit status: the broken bundle's loader
returned zero despite symbol-version errors. Compact manifests, journal extracts
and results remain in the ignored `build/frame-linux-aarch64/device` directory,
including `color-only-probe-result.txt`, `flat-smoke-result.txt` and
`final-device-state.txt`.

The probe launcher selects `/opt/steamvr/steamxr_linuxarm64.json` when
`XR_RUNTIME_JSON` is unset and that file exists. Further device operations use
the shared Frame booking and explicit headset-off authorization procedure.
