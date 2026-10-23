#!/usr/bin/env python3
"""Production importer tests using small synthetic archives."""

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import import_sonicr_data as importer


def write_game_zip(path: Path, extra_members: dict[str, bytes] | None = None) -> None:
    members = {
        "SONICR/general/sonicr.bit": b"marker",
        "SONICR/ai/aistuffi.bin": b"ai",
        "SONICR/bin/object.dat": b"object",
        "SONICR/city/city.map": b"city",
        "SONICR/emerald/emerald.map": b"emerald",
        "SONICR/factory/factory.map": b"factory",
        "SONICR/island/island.map": b"island",
        "SONICR/ruin/ruin.map": b"ruin",
        "SONICR/sound/sfx/amy.wav": b"sound",
        "SONICR/music/track2.son": b"music",
        "SONICR/music/track22.son": b"out of range",
        "SONICR/music/music.txt": b"metadata, not read by the game",
        "SONICR/SAVE/SONICR.SAV": b"user save in source archive",
        "SONICR/GHOST/ghost.dat": b"user ghost in source archive",
        "SONICR/mods/readme.txt": b"not native data",
        "SONICR/setup.exe": b"windows installer",
    }
    if extra_members:
        members.update(extra_members)
    with zipfile.ZipFile(path, "w") as archive:
        for name, body in members.items():
            archive.writestr(name, body)


class ImporterTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.archive = self.root / "owned.zip"
        write_game_zip(self.archive)
        self.repo = self.root / "repo"
        (self.repo / "DATA/BIN/OPTION").mkdir(parents=True)
        (self.repo / "DATA/SOUND/SFX").mkdir(parents=True)
        (self.repo / "DATA/BIN/OPTION/NET01.RAW").write_bytes(b"network icons")
        (self.repo / "DATA/SOUND/SFX/REPLAY1A.WAV").write_bytes(b"replay voice")
        self.destination = self.root / "xdg/sonic-r-vr/data"

    def tearDown(self) -> None:
        self.temp.cleanup()

    def test_plan_normalizes_native_paths_and_excludes_non_runtime_payloads(self) -> None:
        plan = importer.archive_plan(self.archive)
        targets = {target.as_posix() for _, target in plan}
        self.assertIn("GENERAL/SONICR.BIT", targets)
        self.assertIn("SOUND/SFX/AMY.WAV", targets)
        self.assertIn("MUSIC/track2.son", targets)
        self.assertNotIn("MUSIC/track22.son", targets)
        self.assertNotIn("MUSIC/MUSIC.TXT", targets)
        self.assertFalse(any(target.parts[0] in {"SAVE", "GHOST", "MODS"} for target in map(Path, targets)))
        self.assertFalse(any(target.suffix.lower() == ".exe" for target in map(Path, targets)))

    def test_import_preserves_user_state_and_is_idempotent(self) -> None:
        (self.destination / "SAVE").mkdir(parents=True)
        (self.destination / "SAVE/SONICR.SAV").write_bytes(b"keep this save")
        (self.destination / "SONICR.INF").write_bytes(b"keep these settings")

        count, size = importer.import_archive(self.archive, self.destination, self.repo)
        self.assertEqual(count, 12)  # nine game folders, one music track, two documented extras
        self.assertGreater(size, 0)
        self.assertEqual((self.destination / "SAVE/SONICR.SAV").read_bytes(), b"keep this save")
        self.assertEqual((self.destination / "SONICR.INF").read_bytes(), b"keep these settings")
        self.assertEqual((self.destination / "MUSIC/track2.son").read_bytes(), b"music")
        self.assertTrue((self.destination / "BIN/OPTION/NET01.RAW").is_file())
        self.assertTrue((self.destination / "SOUND/SFX/REPLAY1A.WAV").is_file())
        self.assertFalse((self.destination / "SAVE/SONICR.SAV").samefile(self.archive))
        self.assertEqual(importer.import_archive(self.archive, self.destination, self.repo), (0, 0))

    def test_conflict_is_detected_before_any_archive_files_are_added(self) -> None:
        (self.destination / "ISLAND").mkdir(parents=True)
        (self.destination / "ISLAND/ISLAND.MAP").write_bytes(b"keep user file")
        with self.assertRaisesRegex(importer.ImportFailure, "refusing to overwrite"):
            importer.import_archive(self.archive, self.destination, None)
        self.assertFalse((self.destination / "GENERAL/SONICR.BIT").exists())
        self.assertEqual((self.destination / "ISLAND/ISLAND.MAP").read_bytes(), b"keep user file")

    def test_symlinked_destination_subdirectory_is_rejected(self) -> None:
        external = self.root / "outside"
        external.mkdir()
        self.destination.mkdir(parents=True)
        os.symlink(external, self.destination / "ISLAND")
        with self.assertRaisesRegex(importer.ImportFailure, "unsafe destination path component"):
            importer.import_archive(self.archive, self.destination, None)
        self.assertEqual(list(external.iterdir()), [])

    def test_symlinked_destination_root_is_rejected(self) -> None:
        external = self.root / "outside-root"
        external.mkdir()
        linked = self.root / "linked-data"
        os.symlink(external, linked)
        with self.assertRaisesRegex(importer.ImportFailure, "must not be a symlink"):
            importer.import_archive(self.archive, linked, None)
        self.assertEqual(list(external.iterdir()), [])

    def test_unsafe_zip_path_is_rejected_before_destination_creation(self) -> None:
        bad_archive = self.root / "unsafe.zip"
        write_game_zip(bad_archive, {"../outside.txt": b"escape"})
        destination = self.root / "never-created/data"
        with self.assertRaisesRegex(importer.ImportFailure, "unsafe archive path"):
            importer.import_archive(bad_archive, destination, None)
        self.assertFalse(destination.exists())

    def test_case_normalized_collision_is_rejected(self) -> None:
        bad_archive = self.root / "collision.zip"
        write_game_zip(bad_archive, {
            "SONICR/GENERAL/A.raw": b"one",
            "SONICR/general/a.RAW": b"two",
        })
        with self.assertRaisesRegex(importer.ImportFailure, "duplicate archive path"):
            importer.archive_plan(bad_archive)

    def test_default_destination_uses_xdg_data_home(self) -> None:
        previous = os.environ.get("XDG_DATA_HOME")
        try:
            os.environ["XDG_DATA_HOME"] = str(self.root / "xdg-home")
            self.assertEqual(importer.default_destination(), self.root / "xdg-home/sonic-r-vr/data")
        finally:
            if previous is None:
                os.environ.pop("XDG_DATA_HOME", None)
            else:
                os.environ["XDG_DATA_HOME"] = previous

    def test_packaged_importer_discovers_package_root_extras(self) -> None:
        package = self.root / "package"
        package.mkdir()
        shutil.copyfile(importer.__file__, package / "import_sonicr_data.py")
        (package / "DATA/BIN/OPTION").mkdir(parents=True)
        (package / "DATA/SOUND/SFX").mkdir(parents=True)
        (package / "DATA/BIN/OPTION/NET01.RAW").write_bytes(b"package icons")
        (package / "DATA/SOUND/SFX/REPLAY1A.WAV").write_bytes(b"package voice")
        destination = self.root / "runtime/data"

        result = subprocess.run(
            [sys.executable, str(package / "import_sonicr_data.py"), str(self.archive),
             "--dest", str(destination)],
            check=False, capture_output=True, text=True,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((destination / "BIN/OPTION/NET01.RAW").read_bytes(), b"package icons")
        self.assertEqual((destination / "SOUND/SFX/REPLAY1A.WAV").read_bytes(), b"package voice")
        self.assertIn("12 files", result.stdout)


if __name__ == "__main__":
    unittest.main()
