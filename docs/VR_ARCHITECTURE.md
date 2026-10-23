# Sonic R: third-person stereo renderer

## Build and runtime boundary

`SONICR_VR=1` enables the Linux X11/GLX OpenXR path in the game. The default
`SONICR_VR=0` keeps the desktop renderer, frame cap, and native camera path.
The VR build statically links the OpenXR loader from `OPENXR_PREFIX` (default
`/opt/openxr`); SteamOS supplies the runtime, graphics drivers and system ABI.
The [Frame build guide](../tools/frame/README.md) describes the pinned ARM64
build image and private dependency boundary. `BUILD-METADATA.txt` records the
selected `sonicr_vr` value. No game data is bundled.

The separate GLX probe established GL 4.3 compatibility on Frame's Mesa Zink /
Turnip stack, `XR_KHR_opengl_enable`, two 1728-square `GL_SRGB8_ALPHA8`
swapchains, valid located-eye projection submission, and both explicit colour
readbacks. Those probe results are separate from the game renderer's checks.

## Rendering and camera

Sonic R transforms, clips, and projects geometry on the CPU. Merely changing
OpenGL's projection would retain the original eye's culling and flatten depth.
The VR path instead runs the actual scene renderer for each eye, applying the
eye camera before object rejection, near clipping, billboard construction, and
projection. Geometry behind the original chase view is therefore considered
when the head turns. Track, models, terrain grid, and effects use that eye's
camera and frustum.

`vr/vr.c` retains the previous/current native chase-camera poses at 30 Hz.
The presentation camera interpolates position and the shortest pitch/yaw arc,
then rebuilds an orthonormal basis. It retains the game's chase damping and
collision behavior. The current `xrLocateViews` eye orientation and position
are composed afterward without smoothing. Local-space yaw and head-center
position are anchored from the same located-eye poses used to render after
0.4 seconds of continuous FOCUSED state. Focus gain and menu/race transitions
restart the settle window and cut camera interpolation. Head-center lean is limited to 0.35 m, independently of eye IPD.
The game stays third person.

The native view convention is +X right, +Y up, +Z forward. OpenXR is +X right,
+Y up, -Z forward. `vr_math.h` reflects the forward axis on both sides of the
tracking rotation and converts tracking translations through the chase basis.
Native mirror-mode handedness is retained. Runtime eye positions supply IPD;
only the explicitly synthetic host diagnostic uses 64 mm.

The initial scale is 80 game units per metre: the chase target's 40-unit
vertical character-center offset is treated as 0.5 m. This is a calibration
assumption from a game dimension, exposed as `SONICR_VR_WORLD_SCALE=8..800`
(units/metre), rather than altering IPD. It is not a physical mesh measurement.

The CPU projection uses a conservative integer focal length derived from each
eye's asymmetric tangent bounds. At emission the backend recovers
`x=(sx-cx)*z/fx`, `y=(cy-sy)*z/fy`, `z=1/rhw`, then maps that same eye-space
vertex to the exact runtime floating FOV. This final correction only removes
integer frustum rounding; it does not rotate/recover a source-culled flat frame.
The original perspective-correct UV interpolation and depth ordering remain.

## Draw inventory

| Production path | VR rule |
|---|---|
| `RenderTrackD3D` object bounds, vertices, near clip and backfaces | Rerun with the eye camera before culling; existing mirror winding retained |
| `RenderAllCharacterModels`, limbs, balloons, rings and boxes | Recompute camera-space geometry per eye; animation phases remain tick-owned |
| `RenderPlayfieldGridD3D`, `SkyVertexTransform` | Pass the eye camera explicitly to both float transforms and grid rendering |
| `RenderBackground` | Full-eye clear/background; no screen-space world reprojection |
| `ComputeParallaxAndPlayfieldState`, `RenderParallaxStripsD3D` | Native panorama scroll/horizon derived from eye yaw/pitch; apply eye roll to the screen plane; continue each strip's top texels into the upper cap; no eye translation at infinity |
| `RenderWaterReflectionD3D` | Retain the native panorama reflection/ripple as part of the distant background; no false IPD at its arbitrary screen depth |
| `DrawCollectEffectsD3D`, item and ground particles | Eye-camera anchored billboards, native perspective size and fog |
| Foot/pickup shadows, trails, air/water character effects | Rebuild projected geometry per eye; native world anchors and transparency retained |
| `DrawSnowParticles` | Draw per eye; only the canonical tick render writes simulation-facing visibility |
| Timer, status, minimap, reverse indicator, replay R, pause | Existing content on a 1.6 × 1.2 m plane at 2 m following head yaw/pitch, excluding roll, with runtime eye separation |
| Wrong-way detection/SFX/counter decay and READY/SET/GO sounds | Canonical native render once per tick; repeat eye passes cannot advance it |
| Native fade/iris | Full-eye overlay preserving native fade animation |
| Logo, title, menus, results | Capture the native 4:3 viewport without drawable letterbox bars, render it as the same finite stereo panel, and refresh tracking at display cadence |
| Desktop aspect/scissor | Normal desktop 4:3 path; eye framebuffer uses its full extent with a 640 × 480 logical canvas |

The native panorama is a strip backdrop rather than a reconstructed sky dome;
its historical pitch/horizon model remains. High roll/pitch and track-specific
background transitions require separate visual checks. The initial stereo race
path targets one local viewport. Split-screen behavior is not an accepted
stereo mode.

## Frame ownership

1. `VR_RaceFrame` polls events, waits with `xrWaitFrame`, begins the frame and
   locates both views at the same predicted display time.
2. A display-time scheduler decides whether one native 30 Hz tick is due.
   It shows the interpolated interval between the previous/current camera
   samples. Standby or invalid poses freeze simulation and submit no layers.
   Large timing gaps re-anchor the clock rather than fast-forwarding gameplay.
3. A due tick executes the existing input, physics, weather, particles,
   animation, camera, RNG, and timer path once. It renders the canonical native
   scene once, preserving render-derived simulation feedback and the desktop
   baseline. Render-only display frames bypass all of these updates.
4. Both eye passes repeat rendering from the same simulation state. Per-view
   projected vertices, lighting, sky state and billboard scratch are recomputed.
   Snow visibility and reverse-warning mutations are suppressed in these passes.
5. Each eye has a depth attachment and an acquired/waited sRGB swapchain image.
   Framebuffer sRGB conversion stays disabled for the game's encoded colours.
   GL completion precedes image release, following the verified Frame Zink
   bridge requirement. This is a synchronization cost to measure on device.
6. `xrEndFrame` submits the two located poses and their original FOVs in one
   projection layer. Native vsync and the 33 ms sleep are bypassed.

Menu loops retain their native update cadence: `VR_Present` reuses the completed
screen while pacing and locating panel views at display cadence. A mode change
may discard an already begun race frame with zero layers before the menu frame.
Shutdown releases acquired images, closes a begun frame, requests session exit,
and destroys swapchains/session/instance before the SDL context.

There are no synchronous image readbacks in normal rendering. `glReadPixels`
is used only for explicitly requested one-shot diagnostic captures.

## Deterministic host diagnostic

The VR binary can run against the real owned data without a headset, OpenXR
runtime, Steam registration, or device access. The diagnostic skips menus,
loads Resort Island through the production race setup, uses the native chase
camera, runs synthetic 90 Hz display timing and the 30 Hz simulation, writes
six PPM files at the requested tick, and exits. It logs startup, race readiness,
progress every 30 ticks, camera/player positions, and completion counts.

Use a writable working/output directory and a read-only owned-data mount or a
small writable state overlay. Do not copy or overwrite the owned asset tree.
For a Linux host with Xvfb and Mesa's lavapipe/Zink:

```sh
mkdir -p /tmp/sonicr-vr-yaw0
SDL_AUDIODRIVER=dummy \
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.aarch64.json \
MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink \
SONICR_VR_DIAGNOSTIC=1 SONICR_VR_DIAG_YAW=0 \
SONICR_VR_DUMP_TICK=90 SONICR_VR_DUMP_DIR=/tmp/sonicr-vr-yaw0 \
timeout 120s xvfb-run -a -s '-screen 0 640x480x24' \
  /path/to/sonicr /path/to/owned-data
```

Use the host architecture's lavapipe ICD path. Repeat with separate output
folders and `SONICR_VR_DIAG_YAW=90` and `180`. The output consists of:

- `flat.ppm`: canonical native current-tick render.
- `flat-repeat.ppm`: a repeat of the native current-tick render on the same
  desktop target, isolating state changes from framebuffer-format differences.
- `identity.ppm`: the same current-tick renderer/camera/projection into the eye
  framebuffer with stereo remapping bypassed; compare pixel values to flat.
- `identity-linear.ppm`: the same identity pass using a linear RGBA8 attachment,
  isolating attachment encoding from the draw stream. Attachment formats and
  native vertex/render-state hashes are logged for all three baseline targets.
- `eye0.ppm`, `eye1.ppm`: actual production eye passes, with the display-time
  chase interpolation and synthetic runtime-shaped poses/FOVs.

The diagnostic directory must already exist. `SONICR_VR_DUMP_TICK` accepts
1–900 (default 90); yaw accepts -360–360 degrees.
`SONICR_VR_DIAG_ROLL=-90..90` exercises roll independently. Invalid numeric settings fail
explicitly. `SONICR_VR_DIAG_DRIVE=1` applies the native `PAD_ACCEL` input after
native input polling, only in synthetic diagnostics. Progress includes actual
player/camera positions and display interpolation samples. Use tick 180 for a
moving-race capture. Synthetic diagnostic captures do not establish runtime tracking,
controller input, or device performance.

## Bounded runtime diagnostics

`SONICR_VR_AUTORACE=1` starts the same production Resort Island race using the
real OpenXR runtime. `SONICR_VR_RUN_SECONDS=1..300` imposes a monotonic deadline,
including the event-polling wait before session READY and standby. Counters
separate wait frames, shouldRender=false frames, invalid/valid located views,
rendered eyes, submitted layers, and game ticks. Normal standby freezes the
simulation and submits no layers; startup still requires the focused settling
window. No synthetic input is injected into this runtime mode.

Set both `SONICR_VR_DUMP_TICK=N` and `SONICR_VR_DUMP_DIR=/existing/directory` for
one-shot `eye0.ppm`/`eye1.ppm` captures of the actual acquired runtime swapchains
at their actual dimensions, followed by orderly exit. The runtime capture does
not copy the desktop buffer. Without a capture request, the deadline ends the
run and reports counters, including zero rendered game frames when applicable.

For explicit bounded GPU diagnostics, `SONICR_VR_OFFHEAD_RENDER=1` requires
`AUTORACE=1`, `RUN_SECONDS=1..300`, and the real runtime. It renders while
shouldRender is false and bypasses focus settling only in this mode. Valid
located eye poses are used when available; otherwise it logs and uses neutral
64 mm synthetic eyes with the tested 90 by 74 degree frustum. **Every frame in
this mode submits zero composition layers**, even if session focus changes.
Counters distinguish located and synthetic poses. This mode provides device
GPU/image evidence, not runtime tracking or visible-submission acceptance.
The normal runtime path is unaffected. An external process timeout remains a
backstop for a runtime that blocks inside an OpenXR API call.

## Checks and remaining limits

The standalone host math regression covers neutral basis identity, coordinate
handedness, yaw, eye separation, asymmetric frustum boundaries, inverse-depth
stereo disparity, shortest-arc interpolation, 30/90 Hz tick scheduling, standby,
long-stall recovery at 72/90/120 Hz, roll-free HUD orientation, and focus-settle
reset behavior:

```sh
cc -std=c11 -Wall -Wextra -Werror -Isource/sdl/src \
  tests/vr/test_math.c -lm -o /tmp/sonicr-vr-math
/tmp/sonicr-vr-math
rm /tmp/sonicr-vr-math
```

ARM64 VR-enabled and flat builds passed. Real-data Resort Island diagnostics at
yaw 0, 90, 180 and roll 20 completed 90 native ticks over 268 synthetic display
frames through Zink/lavapipe, with 536 eye passes each. Captures showed distinct
world geometry at side/rear headings and depth-dependent near/far stereo
disparity. The roll capture retained covered corners and roll-free HUD-plane
orientation. The sky top extension samples the native panorama's own upper
texture row; its broad flat colour is an art limitation, not an uncovered region.

The desktop canonical and repeated native render matched exactly. Both sRGB
and linear RGBA8 identity framebuffer renders also matched each other exactly.
The desktop versus either FBO differed at 1,023 pixels (0.333%, mean absolute
channel error 0.0397732, maximum 232), concentrated around water edges. All
three repeat targets emitted 2,394 draws with the same vertex/render-state
hash `fa8c1c2f6d63abac`; each had D24, R8, zero samples and disabled framebuffer
sRGB conversion. This isolates a default-framebuffer/FBO output difference
with identical renderer inputs. It does not establish the precise driver cause
or exact desktop-to-FBO pixel identity.

The deterministic acceleration run completed 180 ticks, 538 display frames and
1,076 eye passes. Sonic moved through production physics to the nearby wall;
captures retained character/wall stereo disparity. Logged native chase positions
changed, with presentation interpolation samples at alpha 0, 1/3 and 2/3.
Every repeated pass fingerprints players, native camera smoothers, collect
effects, precipitation, frame count and random-ring index and fails on mutation;
these checks passed all four stationary poses and the moving run.

Both final ARM64 build variants, strict math regression, four private-library
packaging regressions, shell syntax and diff checks passed. The actual VR binary
rejected OFFHEAD_RENDER without its autorace/time-bound prerequisites with
exit 2 before OpenXR initialization. The compiled production-input manifest
contained 415 entries and stayed unchanged across the final build/test cycle
(SHA-256 `6b70f521cc43b4bc0efc41db7789872f00fa36247986e19080e1bdc6af28482c`).
These are host build/render checks; the runtime hooks do not themselves prove
device rendering, tracking, visible submission or performance.

Frame controller action bindings, a dedicated VR settings interface, alternate
comfort modes, depth submission, actor interpolation, and sustained device
performance are outside this renderer slice. Existing SDL keyboard/gamepad
input remains the gameplay input path. No new UI assets or screens are added.
