# Sonic R Steam Frame native milestone

This ARM64 Linux package contains the SDL2 game build selected by
`sonicr_vr` in `BUILD-METADATA.txt` (0 = flat, 1 = OpenXR stereo), its
standalone OpenXR/GLX colour probe, the data importer, and the repository's two
documented native-only data extras. It does not contain Sonic R game data,
saves, Windows installers, GPU drivers, or a Steam registration/install script.
The probe and VR-enabled game statically link the pinned OpenXR loader; the SteamVR OpenXR runtime
still comes from SteamOS.

## Prepare owned game data

Keep runtime files under XDG data storage. The default is
`$XDG_DATA_HOME/sonic-r-vr/data`, or `~/.local/share/sonic-r-vr/data` when
`XDG_DATA_HOME` is unset. Import only from a Sonic R PC ZIP that you own:

```sh
./import_sonicr_data.py /path/to/SonicR.zip
```

The importer leaves the ZIP unchanged, validates every archive path and file
type, normalizes the PC game asset folders to uppercase for Linux, retains the
literal lowercase `MUSIC/track2.son` through `track21.son` names used by the
loader, and copies only the required native folders/music plus `NET01.RAW` and
the documented replay voices. It does not import saves, ghosts, settings,
mods, installers, or other Windows payloads. Existing files with different
contents are preserved and cause an explicit conflict error.

## Launch

```sh
./run-sonicr
```

The game starts fullscreen and loads the default XDG data directory. Set
`SONICR_DATA_DIR=/absolute/path/to/data` for a different data directory; the
game stores saves and settings there. Additional existing game flags can be
passed after the launcher name. A flat build uses the desktop SDL2 window. A VR build renders
the race through per-eye cameras, with existing menus and HUD on a finite
stereo panel. Both use the existing SDL keyboard/gamepad controls.

The standalone probe uses no game data:

```sh
./run-openxr-probe 10
```

It binds Sonic R's desktop SDL compatibility GLX context to
`XR_KHR_opengl_enable`, requests sRGB swapchain images, submits separate dim
red/blue eyes, follows OpenXR frame pacing/standby state, and prints session,
view-validity, frame-count and format diagnostics. The Steam Frame runtime is
selected through `XR_RUNTIME_JSON` when provided by the host; the launcher uses
`/opt/steamvr/steamxr_linuxarm64.json` only when that known host path exists and
the environment has not selected another runtime.

For an off-head swapchain check, run
`SONICR_PROBE_FORCE_COLOR_ONLY=1 ./run-openxr-probe 10`. It clears both sRGB
swapchains and reads one center pixel from each eye, then ends frames without a
projection layer. This diagnostic can verify GL clears while tracking is
unavailable; it does not prove headset display, valid tracking, or stereo game
rendering. Normal mode submits a projection only when both located eye poses are
valid.

## Runtime boundary

The package carries the non-platform SDL2_mixer codec/audio-support and
miniupnpc dependency closure in `lib/`, with its copyright notices. SteamOS
provides glibc (minimum required by these binaries: 2.34), SDL2, GL/GLX and its
driver stack, X11, PulseAudio/ALSA endpoints, and the active OpenXR runtime.
OpenGL, Mesa, GLX, Vulkan, DRM, GBM, and vendor GPU libraries are intentionally
not included. `BUILD-METADATA.txt`, `MANIFEST.sha256`,
`lib/THIRD-PARTY-COPYRIGHTS.txt`, the repository `LICENSE`, and
`licenses/OpenXR-SDK-LICENSE.txt` identify the build and included notices.

See [`docs/VR_ARCHITECTURE.md`](docs/VR_ARCHITECTURE.md) for the
renderer, timing, camera conventions, deterministic real-data host diagnostic,
and validation boundaries.
