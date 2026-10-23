#!/usr/bin/env python3
"""Import only the native Sonic R Linux data from an owned PC archive.

The game reads data from its current working directory. This importer keeps that
directory outside the checkout/container, normalizes the PC data tree for Linux,
and never imports saves, ghosts, mods, installers, or Windows payloads.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import stat
import sys
import tempfile
import zipfile


ROOT_NAME = "SONICR"
REQUIRED_DIRS = {
    "AI", "BIN", "CITY", "EMERALD", "FACTORY", "GENERAL", "ISLAND", "RUIN", "SOUND"
}
MAX_SELECTED_BYTES = 2 * 1024 * 1024 * 1024
MUSIC_TRACK = re.compile(r"track(?:[2-9]|1[0-9]|20|21)\.son", re.IGNORECASE)
BUILTIN_EXTRA_PATHS = ("BIN/OPTION/NET01.RAW",)


class ImportFailure(Exception):
    """An archive or destination failed a safety or compatibility check."""


def _relative_member(info: zipfile.ZipInfo) -> tuple[str, ...]:
    name = info.filename
    if not name or "\\" in name or "\x00" in name:
        raise ImportFailure(f"unsafe archive path: {name!r}")
    if name.startswith("/") or re.match(r"^[A-Za-z]:", name):
        raise ImportFailure(f"absolute archive path: {name!r}")
    raw_parts = name.split("/")
    if raw_parts[-1] == "":
        raw_parts.pop()
    if not raw_parts or any(part in ("", ".", "..") for part in raw_parts):
        raise ImportFailure(f"unsafe archive path: {name!r}")
    mode = (info.external_attr >> 16) & 0xFFFF
    if stat.S_ISLNK(mode):
        raise ImportFailure(f"archive symlink is not allowed: {name!r}")
    return tuple(raw_parts)


def archive_plan(archive: Path) -> list[tuple[zipfile.ZipInfo, Path]]:
    """Validate every member, then return the selected archive files and targets."""
    try:
        zf = zipfile.ZipFile(archive)
    except (OSError, zipfile.BadZipFile) as exc:
        raise ImportFailure(f"cannot read ZIP archive: {exc}") from exc

    with zf:
        all_names: set[str] = set()
        selected: list[tuple[zipfile.ZipInfo, Path]] = []
        selected_names: set[str] = set()
        required_found: set[str] = set()
        total = 0

        for info in zf.infolist():
            parts = _relative_member(info)
            name_key = "/".join(parts).casefold()
            if name_key in all_names:
                raise ImportFailure(f"duplicate archive path: {info.filename!r}")
            all_names.add(name_key)
            if info.is_dir():
                continue
            if len(parts) < 2 or parts[0].casefold() != ROOT_NAME.casefold():
                continue

            source_top = parts[1].upper()
            if source_top in REQUIRED_DIRS:
                normalized = tuple(part.upper() for part in parts[1:])
                target = Path(*normalized)
                required_found.add(source_top)
            elif source_top == "MUSIC" and len(parts) == 3 and MUSIC_TRACK.fullmatch(parts[2]):
                # music_sdl.c opens literal lowercase trackN.son under MUSIC.
                target = Path("MUSIC") / parts[2].lower()
            else:
                continue

            target_key = target.as_posix().casefold()
            if target_key in selected_names:
                raise ImportFailure(f"case-normalized target collision: {target.as_posix()}")
            selected_names.add(target_key)
            if info.file_size < 0 or info.file_size > MAX_SELECTED_BYTES:
                raise ImportFailure(f"invalid or oversized archive member: {info.filename!r}")
            total += info.file_size
            if total > MAX_SELECTED_BYTES:
                raise ImportFailure("selected game data exceeds the 2 GiB safety limit")
            selected.append((info, target))

    missing = REQUIRED_DIRS - required_found
    if missing:
        raise ImportFailure("archive is missing required game folders: " + ", ".join(sorted(missing)))
    if Path("GENERAL/SONICR.BIT").as_posix().casefold() not in selected_names:
        raise ImportFailure("archive does not contain GENERAL/SONICR.BIT")
    return selected


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _safe_existing_path(root: Path, relative: Path) -> Path:
    """Return a target only when existing path components are ordinary dirs/files."""
    current = root
    if current.exists() or current.is_symlink():
        if current.is_symlink() or not current.is_dir():
            raise ImportFailure(f"destination is not a real directory: {current}")
    for component in relative.parts[:-1]:
        current = current / component
        if current.exists() or current.is_symlink():
            if current.is_symlink() or not current.is_dir():
                raise ImportFailure(f"unsafe destination path component: {current}")
    target = root / relative
    if target.exists() or target.is_symlink():
        if target.is_symlink() or not target.is_file():
            raise ImportFailure(f"destination file path is not a regular file: {target}")
    return target


def _bundled_extras(repo_root: Path | None) -> list[tuple[Path, Path]]:
    if repo_root is None:
        return []
    data_root = repo_root / "DATA"
    if not data_root.is_dir():
        return []
    candidates = [data_root / path for path in BUILTIN_EXTRA_PATHS]
    candidates.extend(sorted((data_root / "SOUND/SFX").glob("REPLAY*.WAV")))
    result: list[tuple[Path, Path]] = []
    for source in candidates:
        if not source.exists():
            continue
        if source.is_symlink() or not source.is_file():
            raise ImportFailure(f"bundled extra is not a regular file: {source}")
        result.append((source, source.relative_to(data_root)))
    return result


def import_archive(archive: Path, destination: Path, repo_root: Path | None = None) -> tuple[int, int]:
    """Stage, validate, and add selected files without overwriting user data."""
    archive = archive.expanduser().resolve(strict=True)
    destination = destination.expanduser()
    if destination.is_symlink():
        raise ImportFailure(f"runtime data directory must not be a symlink: {destination}")
    destination = destination.resolve(strict=False)
    if repo_root is not None:
        repo_root = repo_root.expanduser().resolve(strict=True)
        try:
            destination.relative_to(repo_root)
        except ValueError:
            pass
        else:
            raise ImportFailure("runtime data must be outside the source checkout")

    plan = archive_plan(archive)
    extras = _bundled_extras(repo_root)
    target_paths = {target for _, target in plan}
    target_paths.update(relative for _, relative in extras)

    # Refuse any existing file with different contents before creating or
    # changing the runtime directory. Identical files make repeated imports safe.
    for relative in target_paths:
        _safe_existing_path(destination, relative)

    destination.parent.mkdir(parents=True, exist_ok=True)
    staged_root = Path(tempfile.mkdtemp(prefix=".sonicr-import-", dir=destination.parent))
    committed: list[Path] = []
    made_dirs: list[Path] = []
    try:
        stage = staged_root / "stage"
        stage.mkdir()
        with zipfile.ZipFile(archive) as zf:
            for info, relative in plan:
                staged_file = stage / "zip" / relative
                staged_file.parent.mkdir(parents=True, exist_ok=True)
                written = 0
                with zf.open(info, "r") as source, staged_file.open("xb") as output:
                    while block := source.read(1024 * 1024):
                        output.write(block)
                        written += len(block)
                if written != info.file_size:
                    raise ImportFailure(f"size mismatch while reading {info.filename!r}")

        for source, relative in extras:
            staged_file = stage / "extra" / relative
            staged_file.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, staged_file)

        # Detect collisions between archive files and checkout extras before
        # comparing or adding anything to the runtime directory.
        all_staged: dict[Path, Path] = {}
        for _, relative in plan:
            all_staged[relative] = stage / "zip" / relative
        for source, relative in extras:
            staged_file = stage / "extra" / relative
            if relative in all_staged:
                if _sha256(all_staged[relative]) != _sha256(staged_file):
                    raise ImportFailure(f"archive and bundled extra differ at {relative}")
                continue
            all_staged[relative] = staged_file

        # Refuse differing pre-existing user files before the first commit.
        existing: set[Path] = set()
        for relative, staged_file in all_staged.items():
            target = _safe_existing_path(destination, relative)
            if target.exists():
                if _sha256(target) != _sha256(staged_file):
                    raise ImportFailure(f"refusing to overwrite different user data: {target}")
                existing.add(relative)

        added_bytes = 0
        for relative, staged_file in all_staged.items():
            target = _safe_existing_path(destination, relative)
            if target.exists():
                continue
            parent = target.parent
            missing_dirs: list[Path] = []
            while not parent.exists():
                missing_dirs.append(parent)
                parent = parent.parent
            for directory in reversed(missing_dirs):
                directory.mkdir()
                made_dirs.append(directory)
            try:
                os.link(staged_file, target)
            except OSError:
                # Same-filesystem hard links are the normal path; exclusive
                # creation keeps the fallback from ever replacing user data.
                with staged_file.open("rb") as source:
                    fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
                    try:
                        with os.fdopen(fd, "wb") as output:
                            shutil.copyfileobj(source, output, 1024 * 1024)
                    except BaseException:
                        target.unlink(missing_ok=True)
                        raise
            committed.append(target)
            added_bytes += staged_file.stat().st_size

        return len(all_staged) - len(existing), added_bytes
    except BaseException:
        for path in reversed(committed):
            path.unlink(missing_ok=True)
        for path in reversed(made_dirs):
            try:
                path.rmdir()
            except OSError:
                pass
        raise
    finally:
        shutil.rmtree(staged_root, ignore_errors=True)


def default_destination() -> Path:
    data_home = os.environ.get("XDG_DATA_HOME")
    if not data_home:
        data_home = str(Path.home() / ".local/share")
    return Path(data_home) / "sonic-r-vr" / "data"


def default_repo_root() -> Path:
    """Find bundled extras from either the checkout or the staged package."""
    script_dir = Path(__file__).resolve().parent
    for candidate in (script_dir, script_dir.parent.parent):
        if (candidate / "DATA").is_dir():
            return candidate
    return script_dir.parent.parent


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path, help="owned Sonic R PC ZIP, left unchanged")
    parser.add_argument("--dest", type=Path, default=default_destination(), help="runtime data directory (default: XDG_DATA_HOME/sonic-r-vr/data)")
    parser.add_argument("--repo-root", type=Path, default=default_repo_root(),
                        help="checkout supplying documented NET01.RAW and replay voice extras")
    parser.add_argument("--no-bundled-extras", action="store_true", help="import only files from the supplied ZIP")
    args = parser.parse_args(argv)
    try:
        repo_root = None if args.no_bundled_extras else args.repo_root
        count, size = import_archive(args.archive, args.dest, repo_root)
    except (ImportFailure, OSError, zipfile.BadZipFile) as exc:
        print(f"sonic-r-data-import: {exc}", file=sys.stderr)
        return 1
    print(f"Imported {count} files ({size} new bytes) into {args.dest.expanduser().resolve()}")
    print("User saves/configuration were preserved; imported files contain no save, ghost, mod, or Windows payloads.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
