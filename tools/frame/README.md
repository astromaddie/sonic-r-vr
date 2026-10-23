# Steam Frame native and stereo builds

Run `tools/frame/build-linux-arm64.sh` to build the flat game, or
`SONICR_VR=1 tools/frame/build-linux-arm64.sh` to build the OpenXR stereo game.
Both stage the standalone OpenXR/GLX colour probe. The build currently
requires the existing
ARM64 Docker image `mariokart64recomp-frame:latest` (observed image ID
`sha256:6cd15fa7941a706358920701a1ccb504852e8ebf1df6ebb0f4b92de6814455f4`),
which supplies Ubuntu 22.04, GCC 12, SDL2 2.30.11, OpenGL/X11 headers, and
OpenXR SDK 1.1.63 under `/opt/openxr`. The script rejects non-ARM64 images,
caps the container at three CPUs, uses `--rm`, and installs exact Ubuntu
package versions for SDL2_mixer and miniupnpc. The image ID is pinned and
checked, but this milestone does not include a portable from-scratch Dockerfile
to recreate that custom image on another host. Recreating the image/toolchain
is a remaining build portability limitation; the source build is repeatable
only when that exact image is available. The build does not change Docker
contexts, create persistent containers, access a headset, or package user game
data. The selected image, toolchain, package versions, OpenXR archive and
header hashes, ELF details, source identity, and per-file package hashes are
recorded in `build/frame-linux-aarch64`.

The staged package is
`build/frame-linux-aarch64/package/sonic-r-frame-native`. Its two launchers
are `run-sonicr` and `run-openxr-probe`. Import the owned game ZIP with the
packaged `import_sonicr_data.py`; see the package `README.md`. Runtime data and
saves stay in the XDG data directory outside the checkout.

The host captures and compact build/device logs are in
`build/frame-linux-aarch64`; all of `/build/` is ignored. The stereo package
from `7347a3f` is installed and hash-verified on the Frame. Its Steam-launched
game rendered Resort Island into both 1728-square sRGB eye swapchains,
captured the settled third-person scene after 90 native ticks, and shut down
orderly. The bounded GPU diagnostic submitted zero composition layers.
The normal runtime separately held zero game ticks throughout an eight-second
standby check. The normal launcher and original data state were restored.
See [`docs/VR_ARCHITECTURE.md`](../../docs/VR_ARCHITECTURE.md) for the current
stereo architecture, exact artifact identity, host and device checks, and
remaining limits. `BUILD-METADATA.txt` identifies the flat/stereo build flag;
earlier installed artifacts are separate from a newly built package.

The dependency stager follows each ELF's direct `DT_NEEDED` entries and recurses
only into private libraries. System SDL2, graphics/display/audio platform roots,
C/C++ runtimes and readline stay on SteamOS. This avoids shadowing the newer
Mesa driver's runtime with older build-image libraries. Run the focused
regression checks with `python3 tools/test_frame_package_runtime_libs.py`;
the production builder also runs them before packaging.

## Build only the game on Linux

With SDL2, SDL2_mixer, miniupnpc, GL/X11 development packages and the OpenXR
loader installed, build from the source directory (the pinned image supplies
the OpenXR SDK under `/opt/openxr`):

```sh
make -C source/sdl release SONICR_VR=1 OPENXR_PREFIX=/opt/openxr
```

Use a clean build or a distinct `RELEASE_BUILDDIR` when changing the flag.
The VR path currently requires Linux X11/GLX; macOS and Windows retain the
[desktop builds](../../docs/desktop.md). The pinned ARM64 container route can
be driven from macOS, Linux Docker, or Windows WSL2 if that exact image is
available; it is not a published, reproducible image bootstrap.
