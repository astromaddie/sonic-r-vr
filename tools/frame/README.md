# Steam Frame native bootstrap

Run `tools/frame/build-linux-arm64.sh` on the Apple Silicon host to build and
stage the game and standalone OpenXR/GLX colour probe. The build currently
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

The host game screenshot and compact build/device logs are in
`build/frame-linux-aarch64`; all of `/build/` is ignored. The corrected package
is hash-verified on the Frame. Its Steam-launched OpenXR probe passed both
sRGB eye colour readbacks and orderly shutdown. The flat game test establishes
native process startup; a rendered race still needs separate device evidence.
See [`docs/VR_ARCHITECTURE.md`](../../docs/VR_ARCHITECTURE.md) for the installed
artifact identity, explicit diagnostic wrapper, dependency correction and
remaining validation gates.

The dependency stager follows each ELF's direct `DT_NEEDED` entries and recurses
only into private libraries. System SDL2, graphics/display/audio platform roots,
C/C++ runtimes and readline stay on SteamOS. This avoids shadowing the newer
Mesa driver's runtime with older build-image libraries. Run the focused
regression checks with `python3 tools/test_frame_package_runtime_libs.py`;
the production builder also runs them before packaging.
