"""Exercise build-side package requirements with an actual CMake generation.

Imported fixture targets are never built or executed. This verifies configured
packaging names/dependency requirements, not a platform build or runtime.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "cmake/CaesuraPackageRequirements.cmake"


class PackageBuildMetadata(unittest.TestCase):
    def generate(self, *, sdl="SHARED", ffmpeg=False, steam=False):
        with tempfile.TemporaryDirectory(prefix="u22-cmake-metadata-") as temp:
            root = Path(temp)
            suffix = ".dll" if os.name == "nt" else ".so"
            platform = "windows" if os.name == "nt" else "linux"
            fixture = f'''cmake_minimum_required(VERSION 3.22)
project(PackageFixture VERSION 1.2.3 LANGUAGES NONE)
add_executable(PackageFixture IMPORTED)
set_target_properties(PackageFixture PROPERTIES IMPORTED_LOCATION "${{CMAKE_CURRENT_SOURCE_DIR}}/CaesuraAmeKAG{'.exe' if os.name == 'nt' else ''}")
add_executable(lua_cli IMPORTED)
set_target_properties(lua_cli PROPERTIES IMPORTED_LOCATION "${{CMAKE_CURRENT_SOURCE_DIR}}/lua{'.exe' if os.name == 'nt' else ''}")
add_library(SDL3::SDL3 {sdl} IMPORTED)
set_target_properties(SDL3::SDL3 PROPERTIES IMPORTED_LOCATION "${{CMAKE_CURRENT_SOURCE_DIR}}/SDL3{suffix}" IMPORTED_SONAME "SDL3{suffix}")
set(CPACK_PACKAGE_FILE_NAME "CaesuraAmeKAG-1.2.3-Fixture-x64")
set(CAESURA_CAPABILITY_PLATFORM "{platform}")
set(CAESURA_CAPABILITY_FFMPEG_JSON {'true' if ffmpeg else 'false'})
set(CAESURA_CAPABILITY_STEAM_JSON {'true' if steam else 'false'})
set(CAESURA_CAPABILITY_LIVE2D_JSON false)
include("{MODULE.as_posix()}")
'''
            (root / "CMakeLists.txt").write_text(fixture, encoding="utf-8")
            selected = os.environ.get("CAESURA_TEST_CMAKE_GENERATOR")
            if not selected and os.name == "nt":
                selected = "Ninja" if shutil.which("ninja") else "Visual Studio 17 2022"
            generator = ["-G", selected] if selected else []
            for variable, option in (("CAESURA_TEST_CMAKE_INSTANCE", "CMAKE_GENERATOR_INSTANCE"),
                                     ("CAESURA_TEST_CMAKE_MAKE_PROGRAM", "CMAKE_MAKE_PROGRAM")):
                if os.environ.get(variable):
                    generator.append("-D" + option + "=" + os.environ[variable])
            result = subprocess.run([shutil.which("cmake") or "cmake", *generator, "-S", str(root),
                                     "-B", str(root / "build"), "-DCMAKE_BUILD_TYPE=Release"],
                                    capture_output=True, text=True, encoding="utf-8", timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            # Multi-config generators also create Debug/RelWithDebInfo; the
            # caller names its selected configuration, never takes the first.
            path = root / "build/package-requirements-Release.json"
            self.assertTrue(path.is_file(), result.stdout)
            return json.loads(path.read_text(encoding="utf-8"))

    def test_shared_runtime_uses_configured_name_and_explicit_container_paths(self):
        report = self.generate()
        self.assertEqual(report["schema"], "caesura.package-build.v1")
        self.assertEqual(report["configuration"], "Release")
        self.assertEqual(report["version"], "1.2.3")
        stem = "CaesuraAmeKAG-1.2.3-Fixture-x64"
        self.assertEqual(report["archive_basename"], stem)
        self.assertEqual(report["artifacts"], {"zip": stem + ".zip", "tgz": stem + ".tar.gz",
                                             "dmg": stem + ".dmg", "appimage": stem + ".AppImage"})
        self.assertEqual(report["required_configuration"]["sdl_linkage"], "shared")
        self.assertEqual(report["required_configuration"]["sdl_libraries"],
                         ["SDL3.dll" if os.name == "nt" else "SDL3.so"])
        self.assertFalse(report["required_configuration"]["ffmpeg"])
        self.assertFalse(report["required_configuration"]["steam"])
        self.assertNotIn("runtime", report)
        self.assertNotIn("STATIC_PASS", json.dumps(report))

    def test_static_sdl_does_not_invent_a_dynamic_dependency(self):
        required = self.generate(sdl="STATIC")["required_configuration"]
        self.assertEqual(required["sdl_linkage"], "static")
        self.assertEqual(required["sdl_libraries"], [])

    def test_effective_enabled_sdk_flags_are_not_replaced_by_off_defaults(self):
        required = self.generate(ffmpeg=True, steam=True)["required_configuration"]
        self.assertTrue(required["ffmpeg"])
        self.assertTrue(required["steam"])
        self.assertFalse(required["live2d"])
        if os.name != "nt":
            # An unresolved POSIX enabled SDK must remain visibly unconfigured
            # for the package checker to reject, never imply static linkage.
            self.assertEqual(required["ffmpeg_linkage"], "unspecified")
            self.assertEqual(required["steam_linkage"], "unspecified")


if __name__ == "__main__":
    unittest.main()
