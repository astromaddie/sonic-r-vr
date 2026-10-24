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
The original perspective-correct UV interpolation and producer depth biases
remain. The race-eye world-depth correction below changes depth interpolation.

## Draw inventory

| Production path | VR rule |
|---|---|
| `RenderTrackD3D` object bounds, vertices, near clip and backfaces | Rerun with the eye camera before culling; existing mirror winding retained |
| `RenderAllCharacterModels`, limbs, balloons, rings and boxes | Recompute camera-space geometry per eye; animation phases remain tick-owned |
| `RenderPlayfieldGridD3D`, `SkyVertexTransform` | Pass the eye camera explicitly to both float transforms and grid rendering |
| `RenderBackground` | Full-eye clear/background; no screen-space world reprojection |
| `ComputeParallaxAndPlayfieldState`, `RenderParallaxStripsD3D` | Native panorama scroll/horizon derived from eye yaw/pitch; anchor strips to the optical axis, rotate angular coordinates for roll and apply the exact asymmetric FOV; continue each strip's top texels into the upper cap; no eye translation at infinity |
| `RenderWaterReflectionD3D` | Retain the native panorama reflection/ripple as part of the distant background; no false IPD at its arbitrary screen depth |
| `DrawCollectEffectsD3D`, item and ground particles | Eye-camera anchored billboards with fractional projected corners, native perspective size and fog |
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
is used only for explicitly requested diagnostic captures and observers.

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

### Ground decals and water

Ground-facing quads use fractional projected coordinates in eye passes too:
`SubmitGroundShadow` (pickup/balloon shadows), `DrawFootShadows` (foot/boost
trails), `SetupCharacterShadowQuad`, `DrawMovingPickupShadow`, and
`DrawGroundParticlesD3D` (water-ring/snow decals). Their culling uses those same
coordinates. Native depth/fog biases and flat-mode integer projection remain
unchanged. Foot-shadow integer scratch is still populated, while the eye uses
local fractional corners. Native culling retains integer products (double
locals preserve every integer corner before emission); a near-collinear
production control guards against float cancellation changing its sign. The
ground particle renderer may clear a culled slot only on the canonical pass; an eye cannot remove a particle for its partner.

Water is the upstream port's animated, tinted panorama reflection plus bottom
fill, with ambient `GENERAL/PLOP2.RAW` ring decals loaded by `LoadPlopSprites`.
It has no separate continuous water-surface texture. The SDL reflection pass
now explicitly scopes opaque/modulate/depth-write-off state, followed by its
existing additive tint, and restores the caller's state just as the DC path
does. This does not introduce a new water material. Baseline native and eye
Resort Island captures already show the reflection and blue tint.

`tests/vr/run-ground-projection.sh` calls the five production ground emitters
across 81 camera positions and both asymmetric principal points, repeating in
reverse eye order (324 passes). It checks nonzero area, canonical particle
ownership, water state restoration/independence and animation, and depth at
324 interior samples against the actual track near-clipping emitter, including
a large underlying track quad at grazing distance and a near-clipped track.
`--flat` compares native draw streams; `SONICR_TEST_VR=0` checks the flat build.
`SONICR_TEST_LEGACY_REF=c4c0698` runs the same fixture against the three previous
renderer translation units. `--particle` and `--water` isolate those state
regressions. These CPU checks establish geometry/state behavior, not wearer
comfort or complete GPU coverage. Use the real-data temporal stereo diagnostic
and its uninstrumented endpoint control before headset acceptance. Its state
fingerprint now includes ground particle slots/visibility.

### Stereo sprite regression

Eye passes retain fractional billboard corners for rings, collect effects,
character sprites, held items and item shields. Native integer rounding on the
640 × 480 canvas could turn a small quad into zero area in only one eye; that
information cannot be recovered by the final FOV remap or higher eye resolution.
Flat and canonical native passes retain the original integer projection.
The underlying native camera-space integer transforms remain unchanged.

Sky and water atlas strips share an angular origin relative to the optical
axis. An asymmetric eye's left clipping edge must not move the panorama's
texture directions. Preceding strips are included when needed for left-edge
coverage. Sky emission rotates normalized angular coordinates and then applies
the exact eye FOV, including the CPU projection's integer-rounding correction.

`tests/vr/run-sprite-projection.sh` calls production ring, collect-effect,
character/item, sky and water renderers with a recording draw boundary. It
checks 81 animated camera samples, repeated and reversed eye order (486
passes), noncollapsed small sprites and asymmetric-eye panorama alignment.
`--flat` checks the canonical native path; `SONICR_TEST_VR=0` compiles the flat
variant. `SONICR_TEST_LEGACY_REF=ddff6ee` compiles the affected renderer files
from the prior source in a temporary directory with the same test fixture.
The legacy eye test fails: rings and collect effects collapse in 75/81 camera
samples at depth 16000; corresponding sky boundaries differ by 0.25 in tangent
space. The fixed source passes those geometry assertions. Flat draw-stream
hashes can be compared directly between revisions and build variants.
These are CPU geometry checks, not a GPU rasterization or performance claim.

For actual production-scene temporal evidence, add
`SONICR_VR_DIAG_STEREO_CHECK=1` to the host diagnostic command. This opt-in
observer uses asymmetric eye FOVs, a small deterministic yaw sweep, and the
last four native ticks before `DUMP_TICK`. Each display frame renders left/right,
then right/left and left/right again without a simulation or pose update.
It requires exact draw-stream and full-image equality for each same-eye repeat,
and validates consecutive unchanged readbacks before comparisons. It writes
`frameNNNN-eye0.ppm` / `frameNNNN-eye1.ppm` for temporal inspection and exits
on observer, state or image mismatch. Readbacks and extra rendering occur only
in this synthetic diagnostic. For matched legacy image evidence, retain this
observer while substituting the legacy sprite/sky renderers and legacy sky
emission block; ordinary symmetric baseline dumps do not exercise this defect.

### Weather projection and performance fixture

The added procedural water surface has been removed at the user's request.
Native reflected/tinted water and frozen snow-weather water remain; no collision
or drowning rule changed. Sky and ground-decal precision fixes are retained.
Snow/rain and ambient particle quads now use fractional eye corners, preserving
native integer corners when VR is disabled or rendering the canonical pass.
`tests/vr/run-weather-projection.sh` compiles the actual weather initialization
and emitters: 81 camera/atlas samples, two eyes and reversed same-eye repeats,
canonical visibility ownership, rain initialization and render-state restoration.
`SONICR_TEST_LEGACY_REF=f2491a8` runs the same fixture against the old emitter;
`--flat` and `SONICR_TEST_VR=0` are native draw-stream controls.

For an actual normal-race fixture, set `SONICR_VR_AUTORACE=1` (runtime) or
`SONICR_VR_DIAGNOSTIC=1` (host), then explicitly set:

- `SONICR_VR_FIXTURE_TRACK=1..5` (default 1).
- `SONICR_VR_FIXTURE_WEATHER=0..2` (clear/rain/snow, default clear).
- `SONICR_VR_FIXTURE_TIME=0..3` (sunrise/day/sunset/night, default sunrise).

Any fixture setting selects five-player Grand Prix, fixed character IDs and a
fixed random seed **before** native track/weather selection, texture loading and
particle initialization. Normal menu-selected races are unaffected. Radiant
Emerald still forces native clear weather. The ready log records resolved
weather/time and precipitation range/size/texture; visible precipitation draws
must also be verified. The original unset-fixture one-player clear time attack
remains available for comparisons to prior captures.

`SONICR_VR_PERF=1` records roughly one-second CPU wall-time windows in normal or
autorace play. Stage `*_ms` values are totals divided by display-frame count:
XR begin/pacing, simulation/input, canonical render, both eye acquisitions,
both eye command submissions, existing eye release/completion, XR end, and
desktop mirror swap. `draws` and `weather_draws` are raw window counts in
canonical,left,right order; weather time is nested within render time.
`max_interval_ms` is the largest interval between returned race frames.
These are CPU spans, **not GPU timings** or frame-time percentiles. The disabled
path does not read clocks or count draws; the test in `run-perf-fixture.sh`
exercises the actual recorder with a controlled clock. Performance runs must
omit captures and the stereo observer; use compositor telemetry independently.

For a reproducible device workload independent of its resting pose, explicitly
add `SONICR_VR_OFFHEAD_NEUTRAL=1`. This requires the existing bounded runtime
`AUTORACE=1`, `OFFHEAD_RENDER=1`, `RUN_SECONDS=1..300` combination. XR wait/begin
pacing and real swapchain dimensions remain; both views use neutral 64 mm IPD
and 90 by 74 degree FOV. `forced_neutral_pose` counts these frames separately
from valid located views and invalid-pose fallback. All such frames submit zero
layers, including if focus changes. This is off-head GPU/CPU workload evidence,
not worn tracking or display-performance acceptance. No release synchronization
or native 30 Hz simulation/input scheduling was changed.

Eye projection now caches FOV tangents and HUD pose terms at every eye target;
sky roll sine/cosine refresh with each applied game camera. The per-vertex
arithmetic order, homogeneous depth/W and native/identity bypass are unchanged.
`tests/vr/run-projection-cache.sh` compares the actual emitter and backend dispatch
against the uncached production function extracted from `f2491a8` (identical to
the frozen instrumented baseline). It exercises asymmetric/canted views, head
poses, camera/mirror changes, four layers, eye-order repeats and menu/identity/
native routes with bitwise XYZW comparison. This is CPU equivalence evidence;
matched device pixels and timing determine whether the optimization is retained.

Adjacent eye primitives are grouped into `GL_TRIANGLES` runs, expanding each
original fan in `(0,i,i+1)` order through the same immediate vertex emitter.
Identical packed vertex colors reuse current GL color only within an open eye
batch; batch boundaries invalidate that cache. Native/identity fans still emit
every color. The backend defers its native reciprocal until after the eye
emitter's early return, avoiding duplicate W calculation without changing math.
Runs stop at changes to desired render state, texture/filter or dirty texture,
and at explicit state/resource/camera/layer/frame/readback boundaries. There is
no geometry sorting, deferred vertex storage or new GPU wait. Native/canonical
and identity rendering retain per-fan submission. The existing per-eye
`glFlush`/`glFinish` completion contract is unchanged.

`SONICR_VR_BATCH=0` selects the original per-fan eye path in the same binary;
unset defaults to batching. Startup logs the setting. With `PERF=1`,
`gl_submits` and `gl_vertices` report backend GL primitive submissions and emitted
vertices, separately from original `draws`; all are raw window counts in
canonical,left,right order. These are API submissions, not measured GPU draw
calls; drivers can split them. Triangle expansion may increase vertex emissions.
`tests/vr/run-adjacent-batch.sh` compares extracted legacy submission, opt-out and
batching using a strict GL recorder: exact ordered triangle vertices/effective
state/resource events, no illegal GL calls inside Begin, unchanged native and
identity routes, and actual lifecycle/texture/clear/readback/menu boundaries.
Source-only snapshots can set `SONICR_TEST_LEGACY_BACKEND` to the absolute frozen
baseline `r_gl_backend.c`; extracted legacy fan and vertex-emitter hashes are verified.
Matched real-game pixels and capture-free total frame timing remain required
for acceptance; CPU fixture batch-count reduction alone is insufficient.

`SONICR_VR_EARLY_FLUSH_DRAWS=N` defaults to 2048; explicit 0 disables it.
After N original backend fans in each race-eye target, it closes the
current batch and issues one `glFlush`, allowing the driver to submit earlier
work while CPU rendering continues. It never adds a finish/readback or changes
vertex/state order. The allowance resets per target and is disabled on release,
shutdown, native/identity and menu paths; frames below the threshold add no
flush. Startup and performance logs record `early_flush_draws`. Use the same
binary with 0 versus 2048, matching scene,
calibrated FOV and completion settings. Strict GL/stream tests cover both batch
modes, threshold crossing, repeated eyes and completion/capture ordering;
all 26 matched actual GPU captures remained byte-identical. At 2160² with
calibrated ~109° FOV, a moving clear fixture measured 116.66 FPS with 0 versus
119.08 with 2048 before helper inlining; 256 regressed to 114.05. Inlining plus 2048
measured 119.23 FPS in clear, 119.65 in rain, and 118.38 in snow over
33 steady windows each. These bounded zero-layer runs measure application
cadence; occasional missed frame intervals remain, and complete-race display
pacing needs compositor timing.

Race rendering uses a shared completion by default;
`SONICR_VR_PAIRED_COMPLETE=0` retains the established per-eye control path. Each eye still ends and
flushes its own picture. Both acquired/waited images remain owned until one
`glFlush`/`glFinish` completes their GL work, then both are released before
`xrEndFrame`. Menus and synthetic diagnostics retain their existing path.
The `paired_complete` performance field and startup log identify the selection.
This scheduling change does not alter geometry or quality.

Runtime dump captures in paired mode happen after that shared finish and
before either release; they never insert a readback between eye draws. Normal
frames add no readbacks. Use bounded neutral off-head runtime captures for
actual-driver correctness, then separate capture-free matched timing runs.
Synthetic diagnostic parity alone does not exercise the XR completion change.
The production race-loop/strict-GL fixture checks capture ordering and unchanged
canonical/eye pass counts; the XR fixture checks completion before release,
partial failures, abandonment, shutdown and wait timeouts. OpenXR requires
successful acquire/wait before use/release; `XR_TIMEOUT_EXPIRED` is success-class
but does not grant image availability ([primary specification](https://github.com/KhronosGroup/OpenXR-Docs/blob/main/specification/sources/chapters/rendering.adoc)).

On Frame's Zink/Turnip driver, a matched native GCC 15.1.1 executable with
batching disabled/enabled rendered the five-player Resort Island snow fixture
at 44.66/94.87 application FPS. Both used actual 2160-square XR swapchains,
neutral off-head views, zero submitted layers and the unchanged 30 Hz game
tick. Runs lasted 25 seconds; the first two telemetry windows were excluded.
Eye API submissions fell from about 3,800 to 217 per eye/frame, while the CPU
wall span around existing release/completion fell from 17.12 to 6.05 ms.
Clear and rain controls with batching reached 99.69 and 91.30 application FPS.
All 26 matched GPU diagnostic captures were byte-identical to the unbatched
path, including asymmetric and repeated/reversed eyes. These measurements
establish an off-head workload improvement, not sustained worn 120 Hz or
acceptance of every moving race scene.

This Frame-specific validation build uses a private static OpenXR 1.1.63 loader
and requires GLIBC 2.38 / GLIBCXX 3.4.32. Its package records the game's native
toolchain separately from reused probe/runtime components built in the pinned
image; it does not claim the pinned image produced the native game executable.

## Controller input

VR builds create and attach one OpenXR action set before beginning the session.
`XR_VALVE_frame_controller_interaction` is enabled only when advertised, with
Frame bindings suggested first. The physical **right Menu/hamburger button**
binds to Start/Pause through `/user/hand/right/input/menu/click`; System is never
bound. An advertised Frame profile that rejects its binding table fails startup
explicitly, instead of silently using a fallback whose Menu is on the left.
Index and Touch suggestions provide fallback profiles on other controllers.
Paths follow [Valve's Steam Frame input documentation](https://partner.steamgames.com/doc/steamhardware/steamframe/input).

OpenXR inputs merge into native P1 pad slot 0, including when SDL reports no pad.
The shared native `GCBTN_*` button table supplies the existing controls: A is
jump/menu confirm; B or X is accelerate/menu back; Y is camera; bumpers/triggers
are left/right brake; left stick and D-pad navigate/steer. Right stick has no
native axis function and is not mapped. Start's physical binding stays fixed.
SDL per-device mappings and saved configuration are unchanged. The existing SDL
remapping screen does not remap the separate XR virtual pad.

Actions sync every XR display frame in menus and races. Rising presses latch
until the next native tick; the published pad snapshot remains stable across
multiple native polls in that tick. Start publishes only its rising edge, so a
held press cannot repeatedly activate subsequent native frames. Focus loss or
failed synchronization clears held, pending and published input. Controls held
on focus gain remain blocked until released; stick directions require neutral.

Startup/profile-change diagnostics report suggested binding results, attached
action set, active hand profiles and resolved Start sources. Optional
`SONICR_VR_INPUT_TRACE=1` logs changes to the consumed native pad word without
per-frame logging. These runtime observations establish binding state; physical
press delivery must be checked separately.

The host regression runs the production action lifecycle/sampling/mapping plus
`InitJoystickConfig`, SDL platform polling, `ReadInput`, and
`UpdatePerPlayerInput`, with only the XR device API and unrelated audio/pause
callbacks replaced. It covers absent/present Frame extension, rejected Frame
bindings and attach failures, right Menu, title/menu high-byte controls, P1
race input without an SDL pad, short Start at 72/90/120 Hz, held Start, focus
release/rearming, and unchanged customized SDL slot tables:

```sh
OPENXR_INCLUDE=/path/to/openxr/include tests/vr/run-input.sh
```

The SDK directory must contain `openxr/`; SDL2 and SDL2_mixer development headers
must also be available. Tests create no GL window or Steam/OpenXR session, and
remove their temporary executable afterward. `SONICR_VR=0` retains SDL-only input.

## Startup projection

The opening Sega logo renders before a configured viewport initializes
`g_screenHeightFull`. `BeginFrame` uses the established logical render height
until that configured height is positive, avoiding equal top/bottom bounds in
`glOrtho`. Positive configured heights retain the existing projection math.
This applies to both flat and VR builds.

`tests/vr/run-menu-projection.sh` calls the production `BeginFrame` with a fake
GL/platform boundary. It checks nondegenerate startup bounds and screen-corner
and center mappings, plus the unchanged positive-height path. The regression
fails against the earlier implementation with zero startup viewport height.

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
These host build/render checks are separate from the packaged executable's
Frame integration check below.

A dedicated VR settings interface, alternate comfort modes, depth submission,
actor interpolation and sustained device performance are outside this slice.
No new UI assets or screens are added.

## Frame integration check, 2026-10-23

The installed ARM64 stereo package was built from clean source commit
`7347a3ff936f53d4b6651701f92a2df052b57aa6`, with source digest
`8bca00050e52d5960d50138b14ce89ee68a1a6a5b7c79bbed1dccf565024b4f1`.
The game ELF SHA-256 is
`58358f51903312d6193f8b4de72114d1f8e6b71180bf5fbf4df010cd81e3789b`;
its build ID is `a1ce40094dbea0afeb13220e443dd31c32a98d07`.
All 47 package manifest entries matched before and after the runs. The package
retains 22 private libraries; game/probe dependency checks and the host Mesa
GLX loader had empty stderr and no unresolved dependencies.

Steam launched the actual packaged game as `SonicRVR` (appid 2861238290,
rungameid 12288924881646518272), using `SteamLinuxRuntime_4-arm64`.
The game created two 1728-by-1728 `GL_SRGB8_ALPHA8` swapchains with three images
each on GL 4.3 compatibility, Mesa 26.3.0-devel, Zink/Turnip Adreno 750.
SDL enumerated the mapped Steam Frame Controllers with 17 buttons.

The bounded GPU check used dummy SDL audio, `SONICR_VR_AUTORACE=1`,
`SONICR_VR_RUN_SECONDS=20`, `SONICR_VR_OFFHEAD_RENDER=1`, and a capture at tick
90. It completed 90 native ticks, 143 display frames and 286 eye passes in
5,568 ms including startup. All 143 located-eye pairs were valid, with no
synthetic pose fallback; 142 frames had shouldRender=false. Every frame
submitted zero composition layers. The captured eyes show textured Resort
Island, the third-person Sonic chase view, HUD, and distinct eye views. These
are GPU/image and lifecycle checks; the diagnostic's standby frame timing is
not a display-performance measurement or visible projection test.

With `OFFHEAD_RENDER=0` and an eight-second deadline, the normal runtime
completed 395 wait frames (394 with shouldRender=false), zero native ticks,
zero eye passes and zero submitted layers. Both runs ended through the normal
OpenXR STOPPING/EXITING lifecycle. The compositor process stayed unchanged.

The original `run-sonicr` Steam arguments and native compatibility settings
were restored byte for byte, with no per-title diagnostic environment or
wrapper remaining. All 449 original data files matched their pre-run hashes.
The game generated a 160-byte `SONICR.INF`; its recorded prior absence was
restored after the check. Game data remains outside the package at
`/home/steamos/.local/share/sonic-r-vr/data`.

Compact logs, manifests, settings-preservation evidence and lossless settled
eye PNGs are retained under the ignored
`build/frame-linux-aarch64/device-stereo` directory. Host captures and their
reproduction record are in `build/frame-linux-aarch64/host-evidence`.
Superseded builds and raw captures were removed after lossless conversion;
the retained host artifacts occupy about 110 MiB less allocated space than the
temporary trees. Removing the replaced device package and diagnostic scratch
recovered 50,831,360 bytes, confirmed by the device filesystem's free-space
change. The current package, source, original archive and owned data remain.

## Controller and startup follow-up, 2026-10-23

The installed follow-up package includes controller commit `76b8aa2` and the
startup projection fix at clean source commit
`6e9f6017e7a0f5c43d8d8a8ac23db96c5f2237a3`. Its game ELF SHA-256 is
`a09ea9228ad73dfbc3520ee50587a857473c3a6bbc90621f76f4e4c75f493b01`,
with build ID `6403fcee79f61f4156bd7435dc91b56c49be9c75` and source-input
digest `820c7d20e4762d4898c4873b3f526a3fe38ae1542e6196d2f35b1b01c91b08a1`.
All 51 package entries matched on the Frame, and dependency checks had no
unresolved libraries or stderr. The package includes its incremental-build
provenance: three rebuilt translation units, 359 audited unchanged objects,
and the original full compiler transcript. The packaged host race diagnostic
again completed 180 ticks, 538 display frames and 1,076 eye passes with
unchanged simulation state across repeated render passes.

At 19:09 JST, Steam launched this package through the existing `SonicRVR`
entry with dummy audio, normal startup, an eight-second deadline,
`MESA_DEBUG=1`, and autorace/off-head/diagnostic rendering disabled. SteamVR
accepted Frame/Index/Touch binding suggestions (17/11/12 bindings, each result
0), attached the action set with the Frame extension enabled, and created
two 2160-by-2160 sRGB eye swapchains. The headset stayed in standby: 660 waits,
659 standby frames, zero native ticks, rendered eyes or submitted layers.
The deadline fired at 8,006 ms, followed by STOPPING, IDLE and EXITING; Steam
removed the game processes at 19:09:13 without termination signals. Neither
the game nor XR-client log contained the previous `glOrtho` / `0x501` error.

The original launcher arguments/settings bytes and modes were restored,
the temporary wrapper was removed, and the per-title environment file remains
absent. All 449 original game-data files matched exactly after the run; only
the newly generated, previously absent `SONICR.INF` was removed. The shared
Frame slot was then released to the next queued session.

For off-head coverage using the runtime's calibrated view bounds, add
`SONICR_VR_OFFHEAD_RUNTIME_FOV=1` to bounded `OFFHEAD_RENDER=1`,
`OFFHEAD_NEUTRAL=1`, `AUTORACE=1`, `RUN_SECONDS=1..300`. Only the FOV comes
from `xrLocateViews`; poses remain explicitly synthetic, swapchain dimensions
and XR pacing remain native, and no layers are submitted. Both FOVs are logged
once. Missing views, nonfinite angles, unordered bounds or angles outside
(-pi/2, pi/2) fail the diagnostic, including when located poses are invalid.
The default neutral fixture remains 90 by 74 degrees for prior comparisons;
normal worn tracking does not use this option.


Race-eye depth correction is enabled by default; `SONICR_VR_DEPTH=0` retains
the previous depth path for matched diagnostics. Native D3D-style `sz` is linear camera
depth. Interpolating it in screen space gives differently tessellated coplanar
track and terrain different depths, allowing terrain squares across the track.
For native normalized depth `s`, the eye emitter uses direct reversed depth
`n*(1-s)/(s*(1-n))`, where `n=1/(farClip+64)`. This preserves each producer's
native numerator bias and far-divisor offset, the one-unit near geometry and
`s=1` far boundary. Grid screen-edge clipping interpolates the mapped depth;
UV, fog and near-plane geometry retain their native paths.

Race eyes require verified32-bit floating depth, zero-to-one clip control and
a Z-identity projection, with native XY/W unchanged. Logical less/less-equal
comparisons become greater/greater-equal and depth clears to zero. HUD, sky,
reflection and fade depths are complemented to preserve their existing order.
Every eye unbind and shutdown restores the projection stack, conventional clip
mode, clear value and logical comparison. Identity targets and menus use D24;
attachment reuse checks format as well as dimensions. Canonical and `VR=0`
rendering stay on the existing path. No readback or completion is added to
ordinary frames; attachment type is checked only when storage is allocated.

This precision requirement is substantive: Frame's tested driver provides only
24 normalized bits for `GL_DEPTH_COMPONENT32`. Forward float32 also loses the
needed far-distance separation. Reversed float32 with zero-to-one clipping
preserves it. See the [clip-control specification](https://registry.khronos.org/OpenGL/extensions/ARB/ARB_clip_control.txt)
for the depth-range precision loss avoided by zero-to-one mapping.
`tests/vr/run-world-depth.sh` exercises actual track/grid/shadow producers,
backend and emitter with strict post-storage ordering (ties fail), tilted and
near-clipped planes, camera motion, asymmetric/repeated eyes, ranges up to
88000 units, and context/attachment restoration. Native Frame checks verify32-bit float attachment support, repeated-eye
image stability, and exact native/identity captures against the previous depth
path. These checks do not establish comfort or sustained display cadence.


For a moving off-head benchmark, `SONICR_VR_FIXTURE_REPLAY=1` reads the owned
`BIN/DEMOS/ISLAND.DEM` into private memory. It requires runtime
`AUTORACE=1`, `OFFHEAD_RENDER=1` and `RUN_SECONDS=1..300`; it rejects synthetic
diagnostics and other tracks. The fixture uses an Island GP race with five
players and the selected weather/time. It retains `DEMO_NONE`, so normal chase
camera updates resume at GO instead of the attract-mode camera.

The fifteen-word little-endian header is validated before loading the P1 pads.
Replay starts with countdown60, matching native recording/playback's
`countdown<61` condition. Only the canonical native input hook consumes a pad;
eye repeats do not. Gameplay actions are retained, Start/menu input is masked,
paused/postrace ticks hold the cursor, and exhaustion yields neutral input.
Race restart reloads the private stream; shutdown frees it. Native ghost
recording cannot overwrite this private copy. The performance log includes
`fixture_replay` and `replay_pad`; compare moving runs before exhaustion.
`tests/vr/run-replay-fixture.sh` tests the production parser, startup gates,
configuration and input/reset lifecycle. Optional `SONICR_TEST_DEMO` points to
an owned file for additional parser verification; no game data is committed.


Eye client arrays are enabled by default; `SONICR_VR_CLIENT_ARRAYS=0` restores
immediate submission. Arrays require adjacent eye batching. Each original fan is expanded in the
same triangle order and transformed immediately into a bounded 3072-vertex
client array of float XYZW, RGBA and UV. Compatible runs use fixed-function
`glDrawArrays(GL_TRIANGLES)`; full storage splits only between triangles.
Every existing state/resource/layer/camera/target barrier submits pending data.
The early-flush 2048 logical-fan threshold and paired completion remain unchanged.
Native, identity and direct menu rendering retain immediate mode.

The desktop renderer owns array use and explicitly binds client memory (array
buffer 0) and client texture unit 0. Each submission saves/restores client vertex
state and restores the final current color/UV, whose values become unspecified
after an array draw. There are no VBOs, shaders, new waits, GL queries or runtime
readbacks. See Khronos [DrawArrays](https://registry.khronos.org/OpenGL-Refpages/gl2.1/xhtml/glDrawArrays.xml)
and [client attribute stack](https://registry.khronos.org/OpenGL-Refpages/gl2.1/xhtml/glPushClientAttrib.xml).
CPU tests compare exact transformed vertices, float colors, UVs and effective
state, including overflow and immediate-mode continuation. Matched Frame GPU
captures are byte-exact across all 26 diagnostic images and both 2160-square
runtime-FOV moving replay eyes. Capture-free moving Resort Island comparisons
reduce eye-command CPU time from about 3.0 to 1.7 ms. App cadence improves from
118.25 to 119.50 FPS in clear weather, 118.50 to 119.88 in snow and 119.55 to
119.73 in rain. These zero-layer measurements still contain missed deadlines;
they do not establish sustained 120 Hz compositor delivery.
The optional CPU performance log includes actual `gl_array_submits` per pass,
separately from total submissions, so runtime A/B can prove the path was active.
