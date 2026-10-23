#!/usr/bin/env python3
"""Exercise the production private-dependency graph staging path."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).parent / "frame" / "package_runtime_libs.py"
SPEC = importlib.util.spec_from_file_location("package_runtime_libs", SCRIPT)
assert SPEC and SPEC.loader
package_runtime_libs = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(package_runtime_libs)


class DependencyGraphTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.files: dict[str, Path] = {}
        self.needed: dict[Path, list[str]] = {}
        self.ldd: dict[Path, str] = {}

    def tearDown(self) -> None:
        self.temp.cleanup()

    def elf(self, name: str, contents: str = "ELF fixture") -> Path:
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents)
        self.files[name] = path
        return path

    def set_elf(
        self,
        path: Path,
        needed: list[str],
        resolutions: dict[str, Path | None],
        *,
        absolute_entries: list[Path] | None = None,
    ) -> None:
        path = path.resolve()
        self.needed[path] = needed
        lines = [
            f"\t{soname} => {target if target else 'not found'} (0x0000)"
            for soname, target in resolutions.items()
        ]
        lines.extend(f"\t{entry} (0x0000)" for entry in absolute_entries or [])
        self.ldd[path] = "\n".join(lines)

    def run_tool(self, args: list[str], **kwargs):
        executable, *rest = args
        path = Path(rest[-1]).resolve()
        if executable == "readelf":
            lines = [
                f" 0x0000000000000001 (NEEDED) Shared library: [{soname}]"
                for soname in self.needed[path]
            ]
            return self._result("\n".join(lines), 0)
        if executable == "ldd":
            return self._result(self.ldd[path], 0)
        self.fail(f"unexpected external command: {args}")

    @staticmethod
    def _result(stdout: str, returncode: int):
        class Result:
            pass

        result = Result()
        result.stdout = stdout
        result.stderr = ""
        result.returncode = returncode
        return result

    def stage(self, binaries: list[Path], output: Path) -> dict[str, Path]:
        with (
            patch.object(package_runtime_libs.subprocess, "run", side_effect=self.run_tool),
            patch.object(package_runtime_libs, "copy_rights"),
        ):
            return package_runtime_libs.stage_dependencies(binaries, output)

    def test_stages_private_transitive_deps_and_stops_at_host_roots(self) -> None:
        app = self.elf("game")
        mixer = self.elf("libSDL2_mixer-2.0.so.0")
        loader = self.elf("ld-linux-aarch64.so.1")
        vorbis = self.elf("libvorbis.so.0")
        ogg = self.elf("libogg.so.0")
        gl = self.elf("libGL.so.1")
        mesa = self.elf("libGLX_mesa.so.0")
        gallium = self.elf("libgallium.so")
        cxx = self.elf("libstdc++.so.6")
        readline = self.elf("libreadline.so.8")
        tinfo = self.elf("libtinfo.so.6")
        math = self.elf("libm.so.6")

        self.set_elf(
            app,
            ["libSDL2_mixer-2.0.so.0", "libGL.so.1", "ld-linux-aarch64.so.1"],
            {
                "libSDL2_mixer-2.0.so.0": mixer,
                "libGL.so.1": gl,
                # These are in ldd's transitive closure, not in the game's DT_NEEDED.
                "libGLX_mesa.so.0": mesa,
                "libgallium.so": gallium,
                "libstdc++.so.6": cxx,
                "libreadline.so.8": readline,
                "libtinfo.so.6": tinfo,
            },
            absolute_entries=[loader],
        )
        self.set_elf(mixer, [
            "libvorbis.so.0", "libstdc++.so.6", "libGLX_mesa.so.0", "libreadline.so.8",
        ], {
            "libvorbis.so.0": vorbis,
            "libstdc++.so.6": cxx,
            "libGLX_mesa.so.0": mesa,
            "libreadline.so.8": readline,
            "libtinfo.so.6": tinfo,
        })
        self.set_elf(vorbis, ["libogg.so.0", "libm.so.6"], {
            "libogg.so.0": ogg,
            "libm.so.6": math,
        })
        self.set_elf(ogg, [], {})

        output = self.root / "stage"
        staged = self.stage([app], output)

        self.assertEqual(
            set(staged), {"libSDL2_mixer-2.0.so.0", "libvorbis.so.0", "libogg.so.0"}
        )
        self.assertEqual({path.name for path in output.iterdir()}, set(staged))
        self.assertNotIn("libGL.so.1", staged)
        self.assertNotIn("ld-linux-aarch64.so.1", staged)
        self.assertNotIn("libGLX_mesa.so.0", staged)
        self.assertNotIn("libgallium.so", staged)
        self.assertNotIn("libstdc++.so.6", staged)
        self.assertNotIn("libreadline.so.8", staged)
        self.assertNotIn("libtinfo.so.6", staged)
        self.assertNotIn("libm.so.6", staged)

    def test_missing_private_direct_dependency_fails_with_soname(self) -> None:
        app = self.elf("game")
        mixer = self.elf("libmixer.so.0")
        self.set_elf(app, ["libmixer.so.0"], {"libmixer.so.0": mixer})
        self.set_elf(mixer, ["libcodec-missing.so.1"], {"libcodec-missing.so.1": None})

        with self.assertRaisesRegex(RuntimeError, "unresolved direct dependency: libcodec-missing.so.1"):
            self.stage([app], self.root / "stage")

    def test_same_soname_from_different_roots_fails_on_content_conflict(self) -> None:
        first_app = self.elf("game-a")
        second_app = self.elf("game-b")
        first_codec = self.elf("a/libcodec.so.1", "first build")
        second_codec = self.elf("b/libcodec.so.1", "second build")
        self.set_elf(first_app, ["libcodec.so.1"], {"libcodec.so.1": first_codec})
        self.set_elf(second_app, ["libcodec.so.1"], {"libcodec.so.1": second_codec})
        self.set_elf(first_codec, [], {})
        self.set_elf(second_codec, [], {})

        with self.assertRaisesRegex(RuntimeError, "conflicting build libraries share SONAME libcodec.so.1"):
            self.stage([first_app, second_app], self.root / "stage")

    def test_loader_symbol_version_error_fails_even_with_zero_exit_status(self) -> None:
        app = self.elf("game")
        codec = self.elf("libcodec.so.1")
        self.set_elf(app, ["libcodec.so.1"], {"libcodec.so.1": codec})

        def failed_loader(args: list[str], **kwargs):
            result = self.run_tool(args, **kwargs)
            if args[0] == "ldd":
                result.stderr = "libstdc++.so.6: version `CXXABI_1.3.15' not found"
            return result

        with patch.object(package_runtime_libs.subprocess, "run", side_effect=failed_loader):
            with self.assertRaisesRegex(RuntimeError, "CXXABI_1.3.15.*not found"):
                package_runtime_libs.library_entries(app)


if __name__ == "__main__":
    unittest.main()
