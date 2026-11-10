# Steam Frame Native and Stereo Builds

This port builds Sonic R as a native Linux ARM64 game. `SONICR_VR=0` keeps
the flat renderer; `SONICR_VR=1` enables OpenXR stereo through X11/GLX.
The VR renderer draws the scene separately for each eye, with an interpolated
third-person chase camera and immediate head tracking. It also adds Frame
controller input, a stereo HUD/menu panel and standby-aware simulation.
[VR_ARCHITECTURE.md](../../docs/VR_ARCHITECTURE.md) describes the renderer,
controls, settings and recorded checks.

## Build Requirements

You need Git and Docker with a running Linux ARM64 engine. The packaging
script also needs the existing local image `mariokart64recomp-frame:latest`:

```text
sha256:6cd15fa7941a706358920701a1ccb504852e8ebf1df6ebb0f4b92de6814455f4
```

This is a local image name, not a published registry image. A fresh clone
can't build the package without it. The repository doesn't include a tested
bootstrap that recreates that exact image. The current package build is
repeatable only with the recorded image and available Ubuntu package pins;
it isn't a portable from-scratch setup yet.

The image provides Ubuntu 22.04, GCC 12, SDL2 2.30.11, OpenGL/X11 headers,
and the static OpenXR SDK 1.1.63 loader under `/opt/openxr`. The container
installs `libsdl2-mixer-dev=2.0.4+dfsg1-4build1` and
`libminiupnpc-dev=2.2.3-1build1`, so it also needs network access to Ubuntu's
package repositories. If those exact versions disappear, the build stops.

The image must report `arm64`; the script rejects an x86-64 image. On an
x86-64 Docker host, running it also requires working ARM64 emulation. macOS,
Linux and Windows WSL2 can drive the script when those requirements are met.
The script uses the selected Docker context and doesn't start a VM or change
contexts for you.

## Build and Package

```sh
git clone https://github.com/astromaddie/sonic-r-vr.git
cd sonic-r-vr
docker info
docker image inspect mariokart64recomp-frame:latest --format '{{.Id}} {{.Architecture}}'
# Choose one. Both use the same output directory.
SONICR_VR=0 tools/frame/build-linux-arm64.sh
# SONICR_VR=1 tools/frame/build-linux-arm64.sh
```

The build cleans the game's release objects before compiling. Each invocation
replaces `build/frame-linux-aarch64/package/sonic-r-frame-native`; copy a
finished package elsewhere first if you want to keep both flat and VR builds.
Both variants include the standalone OpenXR/GLX colour probe, so even the
flat package requires the pinned OpenXR loader.

The wrapper checks the default image's identity and the container checks the
built loader archive's SHA-256:

```text
cff8b19b107889ecf14a6263884afffa1ebf077972aa6245ac41e83d23165294
```

`SONICR_BUILD_IMAGE` selects another local image. For the default image name,
`SONICR_EXPECTED_BUILD_IMAGE_ID` changes its expected ID after review. Neither
override disables the ARM64 or static-loader checks. A source-equivalent
OpenXR rebuild isn't guaranteed to have the same archive hash. Don't change
these checks just to make an unrelated image pass.

The container uses `--rm` and is capped at three CPUs. It doesn't access a
headset or package your game data. Build logs are under
`build/frame-linux-aarch64/logs`. The package's `BUILD-METADATA.txt` records
the source revision and digest, image ID, flat/VR flag, compiler, dependency
versions, OpenXR hashes and ELF details. `MANIFEST.sha256` records the files
actually staged. An older installed build is separate from this new package.

## Install Your Data and Launch

Copy the whole `sonic-r-frame-native` directory to the Frame, keeping its
`lib/` directory beside the executables. This repository doesn't include a
Steam registration or remote installer script. Add `run-sonicr` as a native
ARM64 Steam title and launch it from Steam for the normal runtime/controller
setup. Follow the [package README](package-README.md) for the owned PC data
ZIP importer, XDG paths and controller map:

```sh
# Run inside the package on the Frame, using your own PC game ZIP.
./import_sonicr_data.py /path/to/SonicR.zip
./run-sonicr
```

The command above is also useful from a shell that already has the display
and runtime environment. `run-sonicr` selects SteamVR's ARM64 OpenXR manifest
when present unless `XR_RUNTIME_JSON` is already set. Saves and settings stay
in `~/.local/share/sonic-r-vr/data` by default, outside the package. Set
`SONICR_DATA_DIR` for a different directory.

## Build Only the Game on Linux

The SDL game can also be built directly, without the package wrapper. This
needs a C compiler, Make, SDL2, SDL2_mixer, miniupnpc and OpenGL development
files. The VR variant additionally needs X11 headers and the static OpenXR
loader with headers under `OPENXR_PREFIX`:

```sh
make -C source/sdl release SONICR_VR=0 RELEASE_BUILDDIR=build-flat
make -C source/sdl release SONICR_VR=1 OPENXR_PREFIX=/opt/openxr RELEASE_BUILDDIR=build-vr
```

These produce `source/sdl/build-flat/sonicr` and
`source/sdl/build-vr/sonicr`. They build for the host architecture and don't
stage the probe, importer or private runtime libraries. A native x86-64 Linux
build isn't a Frame binary. The VR path needs Linux X11/GLX; macOS and Windows
retain the [desktop builds](../../docs/desktop.md).

## Checks and Troubleshooting

These packaging/data tests run on the host without Docker or game data:

```sh
python3 tools/test_frame_package_runtime_libs.py
python3 tools/test_import_sonicr_data.py
```

The production builder runs the runtime-library tests before packaging.
The dependency stager follows direct ELF `DT_NEEDED` entries and recurses
only into private libraries. SDL2, graphics/display/audio platform roots,
C/C++ runtimes and readline stay on SteamOS. This avoids shadowing the newer
Mesa driver's runtime with older build-image libraries.

- Docker can't connect: start your Docker engine and check `docker info`
  in the context you intend to use.
- Required image is missing: the custom-image bootstrap limitation above
  applies. Pulling the local tag name from a registry isn't a supported fix.
- Loader identity changed: review how the image and static loader were built;
  changing the image ID alone won't resolve this check.
- Game data is missing: import your owned PC ZIP with the package's importer.
  It preserves conflicting existing data rather than overwriting it.

The source and package checks don't establish headset behavior. The
architecture document keeps device results with their source/build identity
and describes the remaining renderer limits.
