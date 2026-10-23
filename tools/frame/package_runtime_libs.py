#!/usr/bin/env python3
"""Stage private DT_NEEDED dependencies beside the Frame launchers.

SteamOS supplies the graphics/display/audio platform roots and the C/C++ ABI.
Those roots are boundaries: their own dependencies are resolved by SteamOS and
must not be copied into the package. Other direct dependencies are staged and
walked recursively so codec and networking support stays self-contained.
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
    "libutil.so.1",
    "libresolv.so.2",
    "libgcc_s.so.1",
    "libstdc++.so.6",
    "libSDL2-2.0.so.0",
    "libasound.so.2",
    "libpulse.so.0",
    "libpulse-simple.so.0",
    "libdbus-1.so.3",
    "libudev.so.1",
    "libreadline.so.8",
    "libX11.so.6",
    "libXau.so.6",
    "libXcursor.so.1",
    "libXdmcp.so.6",
    "libXext.so.6",
    "libXfixes.so.3",
    "libXi.so.6",
    "libXinerama.so.1",
    "libXrandr.so.2",
    "libXrender.so.1",
    "libxcb.so.1",
    "libxkbcommon.so.0",
    "libwayland-client.so.0",
    "libwayland-cursor.so.0",
    "libwayland-egl.so.1",
}
GPU_PREFIXES = (
    "libGL", "libEGL", "libGLES", "libOpenGL", "libvulkan", "libdrm",
    "libgbm", "libwayland-egl", "libnvidia", "libamd", "libMesa",
    "libgallium",
)
NEEDED_LINE = re.compile(r"\(NEEDED\).*?\[([^\]]+)\]")
LDD_RESOLVED_LINE = re.compile(r"^\s*(\S+)\s+=>\s+(/\S+)(?:\s|$)")
LDD_MISSING_LINE = re.compile(r"^\s*(\S+)\s+=>\s+not found\s*$")
LDD_ABSOLUTE_LINE = re.compile(r"^\s*(/\S+)\s+\(0x[0-9a-fA-F]+\)\s*$")


def needed_sonames(binary: Path) -> list[str]:
    result = subprocess.run(
        ["readelf", "-dW", str(binary)], check=True, text=True, capture_output=True
    )
    return NEEDED_LINE.findall(result.stdout)


def library_entries(binary: Path) -> list[tuple[str, Path]]:
    """Resolve only this ELF's immediate DT_NEEDED entries using the loader."""
    needed = needed_sonames(binary)
    if not needed:
        return []

    result = subprocess.run(["ldd", str(binary)], check=False, text=True, capture_output=True)
    # The dynamic loader can report missing symbol versions on stderr while
    # returning zero (observed with the original bundle and Frame's Mesa).
    loader_error = "not found" in result.stderr or "symbol lookup error" in result.stderr
    if result.returncode != 0 or loader_error:
        detail = (result.stderr or result.stdout).strip()
        raise RuntimeError(f"ldd failed for {binary}: {detail or f'exit {result.returncode}'}")

    resolved: dict[str, Path | None] = {}
    for line in result.stdout.splitlines():
        missing = LDD_MISSING_LINE.match(line)
        if missing:
            resolved[missing.group(1)] = None
            continue
        match = LDD_RESOLVED_LINE.match(line)
        if match:
            soname, path = match.groups()
            resolved[soname] = Path(path)
            continue
        absolute = LDD_ABSOLUTE_LINE.match(line)
        if absolute:
            path = Path(absolute.group(1))
            soname = path.name
            if soname in needed:
                resolved[soname] = path

    entries: list[tuple[str, Path]] = []
    for soname in needed:
        source = resolved.get(soname)
        if source is None:
            raise RuntimeError(f"{binary}: unresolved direct dependency: {soname}")
        if not source.is_file():
            raise RuntimeError(f"{binary}: ldd source library does not exist: {source}")
        entries.append((soname, source))
    return entries


def is_host_dependency(soname: str, host_sonames: set[str]) -> bool:
    return soname in host_sonames or soname.startswith(GPU_PREFIXES)


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


def stage_dependencies(
    binaries: list[Path],
    output: Path,
    *,
    host_sonames: set[str] = HOST_SONAMES,
) -> dict[str, Path]:
    """Copy private direct dependencies and recursively follow only private ELFs."""
    output.mkdir(parents=True, exist_ok=True)
    staged: dict[str, Path] = {}
    sources: dict[str, Path] = {}
    pending = [binary.resolve(strict=True) for binary in binaries]
    visited: set[Path] = set()

    while pending:
        binary = pending.pop()
        if binary in visited:
            continue
        visited.add(binary)

        for soname, source in library_entries(binary):
            if is_host_dependency(soname, host_sonames):
                continue

            if soname in staged:
                old_hash = hashlib.sha256(staged[soname].read_bytes()).digest()
                new_hash = hashlib.sha256(source.read_bytes()).digest()
                if old_hash != new_hash:
                    raise RuntimeError(f"conflicting build libraries share SONAME {soname}")
                continue

            target = output / soname
            shutil.copy2(source, target)
            staged[soname] = target
            sources[soname] = source
            pending.append(source.resolve(strict=True))

    if not staged:
        raise RuntimeError("no private runtime dependencies were staged")
    copy_rights(list(sources.values()), output)
    return staged


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("binaries", nargs="+", type=Path)
    args = parser.parse_args()

    try:
        staged = stage_dependencies(args.binaries, args.output)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"package-runtime-libs: {exc}", file=sys.stderr)
        return 1

    print(f"Staged {len(staged)} private shared libraries in {args.output}")
    for soname in sorted(staged):
        print(soname)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
