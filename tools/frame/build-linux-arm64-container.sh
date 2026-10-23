#!/usr/bin/env bash
# Runs only inside the ARM64 build image, with the checkout mounted at /src.
set -euo pipefail
cd /src

build_root=/src/build/frame-linux-aarch64
package_parent="$build_root/package"
logs="$build_root/logs"
mkdir -p "$package_parent" "$logs"
if ! apt-get update -qq > "$logs/apt-update.log" 2>&1; then
	tail -80 "$logs/apt-update.log" >&2
	exit 1
fi
if ! apt-get install -y -qq --no-install-recommends \
	libsdl2-mixer-dev=2.0.4+dfsg1-4build1 \
	libminiupnpc-dev=2.2.3-1build1 > "$logs/apt-install.log" 2>&1; then
	tail -80 "$logs/apt-install.log" >&2
	exit 1
fi

stage="$package_parent/.sonic-r-frame-native.new"
package="$package_parent/sonic-r-frame-native"
rm -rf "$stage"
mkdir -p "$stage/DATA/BIN/OPTION" "$stage/DATA/SOUND/SFX" "$stage/docs" \
	"$stage/licenses" "$stage/lib"

if ! make -s -C source/sdl release-clean > "$logs/game-clean.log" 2>&1; then
	tail -80 "$logs/game-clean.log" >&2
	exit 1
fi
if ! make -s -C source/sdl release CC=gcc-12 SONICR_VR=0 -j3 > "$logs/game-build.log" 2>&1; then
	tail -100 "$logs/game-build.log" >&2
	exit 1
fi
cp source/sdl/build-release/sonicr "$stage/sonicr"
if ! make -s -C source/sdl release-clean > "$logs/game-clean-after-build.log" 2>&1; then
	tail -80 "$logs/game-clean-after-build.log" >&2
	exit 1
fi

test -r /opt/openxr/lib/libopenxr_loader.a
probe_loader_hash="$(sha256sum /opt/openxr/lib/libopenxr_loader.a | awk '{print $1}')"
expected_probe_loader_hash=cff8b19b107889ecf14a6263884afffa1ebf077972aa6245ac41e83d23165294
expected_openxr_source_hash=5b8dac0608f49498fe8e95f562fe6bcf0bc781e561f137348a8c251d06a88b6f
if [[ "$probe_loader_hash" != "$expected_probe_loader_hash" ]]; then
	echo "OpenXR loader archive identity changed: $probe_loader_hash" >&2
	exit 1
fi
if [[ "$(sha256sum tools/frame/OPENXR-SDK-LICENSE.txt | awk '{print $1}')" != cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30 ]]; then
	echo "OpenXR SDK license file changed from pinned release-1.1.63 source" >&2
	exit 1
fi
if ! gcc-12 -O2 -Wall -Wextra -Werror \
	-I/opt/openxr/include $(sdl2-config --cflags) \
	tools/vr_probe/sonicr_openxr_glx_probe.c \
	-o "$stage/sonicr_openxr_glx_probe" \
	/opt/openxr/lib/libopenxr_loader.a \
	$(sdl2-config --libs) -lGL -lX11 -ldl -lpthread -lrt -lstdc++ \
	> "$logs/probe-build.log" 2>&1; then
	tail -100 "$logs/probe-build.log" >&2
	exit 1
fi

cp tools/import_sonicr_data.py "$stage/import_sonicr_data.py"
cp tools/frame/run-sonicr "$stage/run-sonicr"
cp tools/frame/run-openxr-probe "$stage/run-openxr-probe"
cp tools/frame/package-README.md "$stage/README.md"
cp docs/VR_ARCHITECTURE.md "$stage/docs/VR_ARCHITECTURE.md"
cp LICENSE "$stage/LICENSE"
cp tools/frame/OPENXR-SDK-LICENSE.txt "$stage/licenses/OpenXR-SDK-LICENSE.txt"
cp DATA/BIN/OPTION/NET01.RAW "$stage/DATA/BIN/OPTION/NET01.RAW"
shopt -s nullglob
replay_extras=(DATA/SOUND/SFX/REPLAY*.WAV)
if [[ ${#replay_extras[@]} -ne 13 ]]; then
	echo "expected 13 documented replay voice files, got ${#replay_extras[@]}" >&2
	exit 1
fi
cp "${replay_extras[@]}" "$stage/DATA/SOUND/SFX/"
shopt -u nullglob

python3 tools/frame/package_runtime_libs.py \
	--output "$stage/lib" \
	"$stage/sonicr" "$stage/sonicr_openxr_glx_probe"

# Resolve from the staged relative library directory. GL/GLX/driver libraries
# remain provided by SteamOS and are never copied into this package.
LD_LIBRARY_PATH="$stage/lib" ldd "$stage/sonicr" > "$build_root/logs/package-game-ldd.txt"
LD_LIBRARY_PATH="$stage/lib" ldd "$stage/sonicr_openxr_glx_probe" > "$build_root/logs/package-probe-ldd.txt"
if grep -q 'not found' "$build_root/logs/package-game-ldd.txt" "$build_root/logs/package-probe-ldd.txt"; then
	echo "unresolved runtime dependency after packaging" >&2
	exit 1
fi

# Fail the package if any driver/display stack library slipped through.
if find "$stage/lib" -maxdepth 1 -type f -printf '%f\n' | grep -E '^(lib(GL|EGL|GLES|OpenGL|vulkan|drm|gbm|wayland-egl|nvidia|amd|Mesa))'; then
	echo "OpenGL/driver library found in private runtime directory" >&2
	exit 1
fi

source_status=dirty
if [[ "${SONICR_SOURCE_DIRTY_PATHS:-1}" == 0 ]]; then source_status=clean; fi
{
	printf 'source_head=%s\nsource_status=%s\nsource_digest=%s\n' \
		"${SONICR_SOURCE_REV:-unknown}" "$source_status" "${SONICR_SOURCE_DIGEST:-unknown}"
	printf 'image_id=%s\nimage_arch=%s\n' "${SONICR_BUILD_IMAGE_ID:-unknown}" "$(dpkg --print-architecture)"
	printf 'os_release=%s\n' "$(. /etc/os-release; echo "$PRETTY_NAME")"
	printf 'compiler=%s\n' "$(gcc-12 --version | head -1)"
	printf 'sdl2=%s\n' "$(sdl2-config --version)"
	printf 'libsdl2-mixer-dev=%s\n' "$(dpkg-query -W -f='${Version}' libsdl2-mixer-dev)"
	printf 'libminiupnpc-dev=%s\n' "$(dpkg-query -W -f='${Version}' libminiupnpc-dev)"
	printf 'openxr_sdk_release=1.1.63\nopenxr_source_tar_sha256=%s\n' "$expected_openxr_source_hash"
	printf 'openxr_loader_archive_sha256=%s\n' "$probe_loader_hash"
	sha256sum /opt/openxr/include/openxr/openxr.h | sed 's/^/openxr_core_header_sha256=/'
	sha256sum tools/frame/OPENXR-SDK-LICENSE.txt | sed 's/^/openxr_license_sha256=/'
	printf 'private_library_count=%s\n' "$(find "$stage/lib" -maxdepth 1 -type f -name '*.so*' | wc -l | tr -d ' ')"
	for binary in sonicr sonicr_openxr_glx_probe; do
		printf '\n[%s]\n' "$binary"
		file -b "$stage/$binary"
		printf 'sha256=%s\n' "$(sha256sum "$stage/$binary" | awk '{print $1}')"
		readelf -n "$stage/$binary" | sed -n '/Build ID/p'
		readelf --version-info "$stage/$binary" | grep -o 'GLIBC_[0-9][0-9.]*' | sort -Vu | tail -1 | sed 's/^/max_required_/'
	done
} > "$stage/BUILD-METADATA.txt"

(cd "$stage" && find . -type f ! -name MANIFEST.sha256 -print0 | sort -z | xargs -0 sha256sum > MANIFEST.sha256)

if [[ -e "$package" ]]; then
	previous="$package_parent/.sonic-r-frame-native.previous"
	rm -rf "$previous"
	mv "$package" "$previous"
	if mv "$stage" "$package"; then
		rm -rf "$previous"
	else
		mv "$previous" "$package"
		exit 1
	fi
else
	mv "$stage" "$package"
fi

du -sh "$package"
find "$package" -maxdepth 2 -type f -printf '%P %s bytes\n' | sort
