#!/usr/bin/env python3
"""Stage non-platform ELF dependencies beside the Frame launchers.

The SteamOS host supplies SDL2, GL/GLX, X11, and the glibc base. Decoder,
audio-support, and networking libraries are copied from the pinned Jammy build
environment. OpenGL/driver libraries are explicitly excluded.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import re
import shutil
import subprocess
import sys


HOST_SONAMES = {
    "ld-linux-aarch64.so.1",
    "libc.so.6",
    "libm.so.6",
    "libpthread.so.0",
    "libdl.so.2",
    "librt.so.1",
    "libSDL2-2.0.so.0",
    "libGL.so.1",
    "libGLX.so.0",
    "libGLdispatch.so.0",
    "libX11.so.6",
    "libasound.so.2",
    "libpulse.so.0",
    "libpulse-simple.so.0",
}
GPU_PREFIXES = (
    "libGL", "libEGL", "libGLES", "libOpenGL", "libvulkan", "libdrm",
    "libgbm", "libwayland-egl", "libnvidia", "libamd", "libMesa",
)
LDD_LINE = re.compile(r"^\s*(\S+)\s+=>\s+(\S+)")


def library_entries(binary: Path) -> list[tuple[str, Path]]:
    result = subprocess.run(["ldd", str(binary)], check=True, text=True, capture_output=True)
    entries: list[tuple[str, Path]] = []
    for line in result.stdout.splitlines():
        if "=> not found" in line:
            raise RuntimeError(f"{binary}: unresolved dependency: {line.strip()}")
        match = LDD_LINE.match(line)
        if match:
            soname, path = match.groups()
            if path.startswith("/"):
                source = Path(path)
                if not source.is_file():
                    raise RuntimeError(f"ldd source library does not exist: {source}")
                # Preserve the loader-visible path for dpkg-query -S: resolving
                # symlinks can produce an unowned multiarch versioned filename.
                entries.append((soname, source))
    return entries


def copy_rights(libraries: list[Path], output: Path) -> None:
    packages: set[str] = set()
    for path in libraries:
        ownership = None
        for candidate in dict.fromkeys((path, path.resolve(strict=True))):
            result = subprocess.run(
                ["dpkg-query", "-S", str(candidate)], text=True, capture_output=True
            )
            if result.returncode == 0:
                ownership = result
                break
        if ownership is None:
            raise RuntimeError(f"no Debian package owns staged library {path}")
        package = ownership.stdout.split(": ", 1)[0].split(":", 1)[0]
        packages.add(package)

    sections: list[str] = []
    for package in sorted(packages):
        copyright_file = Path("/usr/share/doc") / package / "copyright"
        if not copyright_file.is_file():
            raise RuntimeError(f"missing copyright file for packaged dependency {package}")
        sections.append(f"===== {package} =====\n{copyright_file.read_text(errors='replace').rstrip()}\n")
    (output / "THIRD-PARTY-COPYRIGHTS.txt").write_text("\n".join(sections))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("binaries", nargs="+", type=Path)
    args = parser.parse_args()
    output = args.output
    output.mkdir(parents=True, exist_ok=True)
    staged: dict[str, Path] = {}
    sources: dict[str, Path] = {}

    try:
        for binary in args.binaries:
            for soname, source in library_entries(binary.resolve(strict=True)):
                if soname in HOST_SONAMES or soname.startswith(GPU_PREFIXES):
                    continue
                target = output / soname
                if soname in staged:
                    old_hash = hashlib.sha256(staged[soname].read_bytes()).digest()
                    new_hash = hashlib.sha256(source.read_bytes()).digest()
                    if old_hash != new_hash:
                        raise RuntimeError(f"conflicting build libraries share SONAME {soname}")
                    continue
                shutil.copy2(source, target)
                staged[soname] = target
                sources[soname] = source

        if not staged:
            raise RuntimeError("no private runtime dependencies were staged")
        copy_rights(list(sources.values()), output)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"package-runtime-libs: {exc}", file=sys.stderr)
        return 1

    print(f"Staged {len(staged)} private shared libraries in {output}")
    for soname in sorted(staged):
        print(soname)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
