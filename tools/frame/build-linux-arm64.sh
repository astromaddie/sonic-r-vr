#!/usr/bin/env bash
# Repeatable Linux ARM64 build using the identity-pinned, Frame-capable local
# image. Fresh image bootstrap is documented as a portability limitation.
# The container is always removed and CPU-capped.
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
image="${SONICR_BUILD_IMAGE:-mariokart64recomp-frame:latest}"
expected_image_id="${SONICR_EXPECTED_BUILD_IMAGE_ID:-sha256:6cd15fa7941a706358920701a1ccb504852e8ebf1df6ebb0f4b92de6814455f4}"
sonicr_vr="${SONICR_VR:-0}"
case "$sonicr_vr" in 0|1) ;; *) echo "SONICR_VR must be 0 or 1" >&2; exit 2 ;; esac
build_root="$repo/build/frame-linux-aarch64"
logs="$build_root/logs"
mkdir -p "$logs"

if ! docker image inspect "$image" >/dev/null 2>&1; then
	cat >&2 <<EOF
Required local build image is missing: $image
Expected ARM64 Ubuntu 22.04 with gcc-12, SDL2, OpenGL/X11 development files,
and OpenXR SDK 1.1.63 at /opt/openxr. Set SONICR_BUILD_IMAGE to a compatible
image; the selected image identity and package versions are recorded.
EOF
	exit 2
fi

image_arch="$(docker image inspect "$image" --format '{{.Architecture}}')"
if [[ "$image_arch" != arm64 ]]; then
	echo "expected ARM64 image, got $image_arch ($image)" >&2
	exit 2
fi

source_rev="$(git -C "$repo" rev-parse HEAD)"
source_dirty="$(git -C "$repo" status --porcelain | wc -l | tr -d ' ')"
source_digest="$(cd "$repo" && {
	find source/sdl/src tools/frame tools/vr_probe -type f \
		! -path '*/__pycache__/*' ! -name '*.pyc' -print
	find DATA/BIN/OPTION -maxdepth 1 -type f -name NET01.RAW -print
	find DATA/SOUND/SFX -maxdepth 1 -type f -name 'REPLAY*.WAV' -print
	printf '%s\n' source/sdl/Makefile tools/import_sonicr_data.py docs/VR_ARCHITECTURE.md \
		tools/frame/OPENXR-SDK-LICENSE.txt tools/test_frame_package_runtime_libs.py
} | LC_ALL=C sort | xargs shasum -a 256 | shasum -a 256 | awk '{print $1}')"
image_id="$(docker image inspect "$image" --format '{{.Id}}')"
if [[ "$image" == mariokart64recomp-frame:latest && "$image_id" != "$expected_image_id" ]]; then
	echo "build image identity changed: expected $expected_image_id, got $image_id" >&2
	echo "Set SONICR_EXPECTED_BUILD_IMAGE_ID only after reviewing the replacement image." >&2
	exit 2
fi

{
	printf 'docker_context=%s\n' "$(docker context show)"
	docker image inspect "$image" --format 'image={{.RepoTags}} id={{.Id}} arch={{.Architecture}} os={{.Os}}'
	printf 'host_arch=%s\nsource_head=%s\nsource_dirty_paths=%s\nsource_digest=%s\n' \
		"$(uname -m)" "$source_rev" "$source_dirty" "$source_digest"
	printf 'container_policy=--rm --cpus=3; context/image unchanged; no device access\n'
} > "$logs/build-environment.txt"

docker run --rm --cpus=3 \
	-e SONICR_VR="$sonicr_vr" \
	-e SONICR_SOURCE_REV="$source_rev" \
	-e SONICR_SOURCE_DIRTY_PATHS="$source_dirty" \
	-e SONICR_SOURCE_DIGEST="$source_digest" \
	-e SONICR_BUILD_IMAGE_ID="$image_id" \
	-v "$repo:/src" -w /src "$image" \
	bash /src/tools/frame/build-linux-arm64-container.sh \
	2>&1 | tee "$logs/build-package.log"

printf 'Package: %s\n' "$build_root/package/sonic-r-frame-native"
