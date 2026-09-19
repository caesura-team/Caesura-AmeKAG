"""Real filesystem contract fixtures; synthetic binary headers are never executed.

The Lua sources, template manifests/entries and font come from the maintained
source tree. Tiny PE/ELF/Mach-O headers test signature refusal only, not valid
executables, ABI, engine functionality or SDK support.
"""
from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "scripts/verify_native_package.py"
sys.path.insert(0, str(ROOT / "scripts"))
if MODULE.is_file():
    spec = importlib.util.spec_from_file_location("native_package_contract", MODULE)
    native = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(native)
else:
    native = None

TEMPLATES = ("basic", "blank", "kag3", "live2d", "showcase")
FFMPEG_DLLS = ("avcodec-62.dll", "avformat-62.dll", "avutil-60.dll", "swscale-9.dll", "swresample-6.dll")


def binary_header(platform):
    if platform == "windows":
        data = bytearray(128)
        data[:2] = b"MZ"
        struct.pack_into("<I", data, 60, 64)
        data[64:68] = b"PE\0\0"
        return bytes(data)
    if platform == "linux":
        return b"\x7fELF\x02\x01\x01" + b"\0" * 57
    return b"\xcf\xfa\xed\xfe" + b"\0" * 60


def config(platform="windows", **changes):
    value = {"schema": 1, "sdl_linkage": "shared",
             "sdl_libraries": [{"windows": "SDL3.dll", "linux": "libSDL3.so.0",
                                 "macos": "libSDL3.0.dylib"}[platform]],
             "ffmpeg": False, "steam": False, "live2d": False}
    value.update(changes)
    return value


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class NativePackageContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.base_temp = tempfile.TemporaryDirectory(prefix="u22-native-base-")
        cls.base = Path(cls.base_temp.name)
        shutil.copytree(ROOT / "scripts", cls.base / "scripts",
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
        (cls.base / "demo").mkdir()
        for name in ("cjk_smoke.ks", "entry.lua"):
            shutil.copy2(ROOT / "demo" / name, cls.base / "demo" / name)
        (cls.base / "projects").mkdir()
        font = Path("assets/fonts/NotoSansCJKsc-Regular.otf")
        (cls.base / font).parent.mkdir(parents=True)
        shutil.copy2(ROOT / font, cls.base / font)
        shutil.copytree(ROOT / "tools/project_templates", cls.base / "tools/project_templates")
        (cls.base / "web-editor/dist").mkdir(parents=True)
        shutil.copy2(ROOT / "web-editor/dist/index.html", cls.base / "web-editor/dist/index.html")
        (cls.base / "editor/dist/assets").mkdir(parents=True)
        (cls.base / "editor/dist/index.html").write_text(
            '<!doctype html><script type="module" src="./assets/index-fixture.js"></script>', encoding="utf-8")
        (cls.base / "editor/dist/assets/index-fixture.js").write_text(
            "/* Static contract fixture, not a runnable Studio build. */", encoding="utf-8")

    @classmethod
    def tearDownClass(cls):
        cls.base_temp.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="u22-native-中文-包-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.package = self.root / "explicit package"
        shutil.copytree(self.base, self.package)
        self.install_binaries("windows")

    def install_binaries(self, platform):
        for name in ("CaesuraAmeKAG.exe", "CaesuraAmeKAG", "external/lua/lua.exe", "external/lua/lua",
                     "SDL3.dll", "libSDL3.so.0", "libSDL3.0.dylib"):
            path = self.package / name
            if path.exists():
                path.unlink()
        executable = "CaesuraAmeKAG.exe" if platform == "windows" else "CaesuraAmeKAG"
        lua = "external/lua/lua.exe" if platform == "windows" else "external/lua/lua"
        for name in (executable, lua, config(platform)["sdl_libraries"][0]):
            path = self.package / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(binary_header(platform))
            path.chmod(0o755)

    def check_package(self, platform="windows", required=None, package=None):
        self.assertIsNotNone(native, "U22 native static checker is not implemented")
        return native.inspect_native_package(package or self.package, platform, required or config(platform))

    def assert_failed(self, report, offender=None):
        self.assertFalse(report["passed"], report)
        self.assertEqual(report["status"], "STATIC_ONLY")
        self.assertEqual(report["runtime"], "NOT_RUN")
        if offender:
            self.assertIn(offender, json.dumps(report["errors"], ensure_ascii=False))

    def test_windows_identity_is_static_only_and_does_not_mutate_input(self):
        report = self.check_package()
        self.assertTrue(report["passed"], report)
        self.assertEqual(report["status"], "STATIC_ONLY")
        self.assertEqual(report["runtime"], "NOT_RUN")
        self.assertEqual(report["abi"], "NOT_VERIFIED")
        self.assertTrue(report["input_stable"])
        for name, relative in (("engine", "CaesuraAmeKAG.exe"), ("lua", "external/lua/lua.exe")):
            self.assertEqual(report["binaries"][name]["sha256"], digest(self.package / relative))
            self.assertEqual(Path(report["binaries"][name]["resolved_path"]), (self.package / relative).resolve())
            self.assertEqual(report["binaries"][name]["header_format"], "PE")
            self.assertEqual(report["binaries"][name]["executable_validity"], "NOT_VERIFIED")

    def test_deleting_required_inputs_refuses(self):
        paths = ("CaesuraAmeKAG.exe", "external/lua/lua.exe", "SDL3.dll", "scripts/config.lua",
                 "scripts/kag/init.lua", "scripts/kag/layer_state.lua", "scripts/caesura.py",
                 "scripts/caesura_build.py", "tools/project_templates/manifest.json", "demo/cjk_smoke.ks", "demo/entry.lua",
                 "assets/fonts/NotoSansCJKsc-Regular.otf", "web-editor/dist/index.html",
                 "editor/dist/index.html", "editor/dist/assets/index-fixture.js")
        for name in paths:
            with self.subTest(path=name):
                path = self.package / name
                before = path.read_bytes()
                path.unlink()
                try:
                    self.assert_failed(self.check_package(), name)
                finally:
                    path.write_bytes(before)
                    if name.endswith(".exe"):
                        path.chmod(0o755)

    def test_all_existing_templates_require_actual_entries_and_manifests(self):
        for template in TEMPLATES:
            for name in ("caesura.project.json", "entry.lua", "story.ks"):
                relative = f"tools/project_templates/{template}/{name}"
                path = self.package / relative
                before = path.read_bytes()
                path.unlink()
                try:
                    self.assert_failed(self.check_package(), relative)
                finally:
                    path.write_bytes(before)

    def test_template_manifest_shape_is_checked(self):
        path = self.package / "tools/project_templates/basic/caesura.project.json"
        for value in ("not-json", "[]", '{}', '{"name":"basic","template":"different"}'):
            with self.subTest(value=value):
                path.write_text(value, encoding="utf-8")
                self.assert_failed(self.check_package(), "caesura.project.json")

    def test_empty_and_wrong_platform_binary_headers_refuse(self):
        for name in ("CaesuraAmeKAG.exe", "external/lua/lua.exe", "SDL3.dll"):
            path = self.package / name
            before = path.read_bytes()
            for value in (b"", b"not an executable", binary_header("linux")):
                path.write_bytes(value)
                self.assert_failed(self.check_package(), name)
            path.write_bytes(before)

    def test_enabled_windows_ffmpeg_requires_the_installed_five_dlls(self):
        required = config(ffmpeg=True)
        self.assert_failed(self.check_package(required=required), "avcodec-62.dll")
        for name in FFMPEG_DLLS:
            (self.package / name).write_bytes(binary_header("windows"))
        self.assertTrue(self.check_package(required=required)["passed"])
        for name in FFMPEG_DLLS:
            path = self.package / name
            path.unlink()
            self.assert_failed(self.check_package(required=required), name)
            path.write_bytes(binary_header("windows"))

    def test_enabled_windows_steam_requires_runtime_and_manifest_cannot_disable_it(self):
        (self.package / "BUILD-INFO.json").write_text(
            '{"capabilities":{"steam":false},"required":[]}', encoding="utf-8")
        required = config(steam=True)
        self.assert_failed(self.check_package(required=required), "steam_api64.dll")
        path = self.package / "steam_api64.dll"
        path.write_bytes(binary_header("windows"))
        self.assertTrue(self.check_package(required=required)["passed"])
        self.assertTrue(self.check_package()["passed"])
        self.assertEqual(path.read_bytes(), binary_header("windows"))

    def test_cubism_static_core_does_not_invent_a_runtime_dll(self):
        report = self.check_package(required=config(live2d=True))
        self.assertTrue(report["passed"], report)
        self.assertFalse(any("CubismCore" in item["relative_path"] for item in report["runtime_libraries"]))

    def test_linux_soname_relative_link_chain_is_resolved_inside_package(self):
        self.install_binaries("linux")
        (self.package / "libSDL3.so.0").unlink()
        actual = self.package / "libSDL3.so.0.2.0"
        actual.write_bytes(binary_header("linux"))
        (self.package / "libSDL3.so.0").symlink_to("libSDL3.so.0.2.0")
        (self.package / "libSDL3.so").symlink_to("libSDL3.so.0")
        report = self.check_package("linux")
        self.assertTrue(report["passed"], report)
        self.assertEqual(report["runtime_libraries"][0]["resolved_path"], str(actual.resolve()))

    def test_macos_bare_binary_and_framework_relative_links(self):
        self.install_binaries("macos")
        (self.package / "libSDL3.0.dylib").unlink()
        framework = self.package / "SDL3.framework"
        (framework / "Versions/A").mkdir(parents=True)
        (framework / "Versions/A/SDL3").write_bytes(binary_header("macos"))
        (framework / "Versions/Current").symlink_to("A", target_is_directory=True)
        # Match prepare_package(): materialized symlink targets use host
        # separators. Win32 rejects a raw multi-component POSIX target.
        (framework / "SDL3").symlink_to(Path("Versions") / "Current" / "SDL3")
        report = self.check_package("macos", config("macos", sdl_libraries=["SDL3.framework/SDL3"]))
        self.assertTrue(report["passed"], report)
        self.assertEqual(report["binaries"]["engine"]["relative_path"], "CaesuraAmeKAG")
        self.assertEqual(report["binaries"]["engine"]["header_format"], "Mach-O")

    def test_posix_enabled_sdks_require_caller_resolved_linkage_and_paths(self):
        for platform in ("linux", "macos"):
            self.install_binaries(platform)
            for feature in ("ffmpeg", "steam"):
                required = config(platform, **{feature: True})
                self.assert_failed(self.check_package(platform, required), feature)
                required[feature + "linkage"] = "shared"  # Unknown keys must not be ignored.
                self.assert_failed(self.check_package(platform, required))
                del required[feature + "linkage"]
                required[feature + "_linkage"] = "static"
                self.assertTrue(self.check_package(platform, required)["passed"])
                required[feature + "_linkage"] = "shared"
                required[feature + "_libraries"] = ["sdk-" + feature + ".bin"]
                self.assert_failed(self.check_package(platform, required), "sdk-" + feature + ".bin")
                (self.package / required[feature + "_libraries"][0]).write_bytes(binary_header(platform))
                self.assertTrue(self.check_package(platform, required)["passed"])

    def test_configuration_is_explicit_and_cannot_escape_package(self):
        self.assertIsNotNone(native, "U22 native static checker is not implemented")
        for required in (None, {}, {"schema": 1}, config(ffmpeg="false"), config(unknown=True)):
            self.assert_failed(native.inspect_native_package(self.package, "windows", required))
        for path in ("../outside.dll", "/outside.dll", "C:/outside.dll", "x\\outside.dll"):
            self.assert_failed(self.check_package("windows", config(sdl_libraries=[path])))
        self.assert_failed(native.inspect_native_package(self.package, "ios", config()))

    def test_external_or_dangling_runtime_links_are_rejected(self):
        (self.package / "SDL3.dll").unlink()
        outside = self.root / "outside.dll"
        outside.write_bytes(binary_header("windows"))
        link = self.package / "SDL3.dll"
        link.symlink_to(outside)
        self.assert_failed(self.check_package())
        self.assertEqual(outside.read_bytes(), binary_header("windows"))
        link.unlink()
        link.symlink_to("missing.dll")
        self.assert_failed(self.check_package())

    def test_root_symlink_is_not_silently_resolved(self):
        alias = self.root / "root-alias"
        alias.symlink_to(self.package, target_is_directory=True)
        self.assert_failed(self.check_package(package=alias))

    def test_required_source_bytes_cannot_be_replaced_by_nonempty_stubs(self):
        for relative in ("scripts/config.lua", "scripts/kag/asset_dependencies.lua",
                         "scripts/caesura_build.py", "tools/project_templates/basic/story.ks",
                         "assets/fonts/NotoSansCJKsc-Regular.otf"):
            with self.subTest(path=relative):
                path = self.package / relative
                before = path.read_bytes()
                path.write_bytes(b"plausible-looking but wrong bytes")
                try:
                    self.assert_failed(self.check_package(), relative)
                finally:
                    path.write_bytes(before)

    def test_unrelated_editor_chunk_does_not_cover_missing_referenced_chunk(self):
        path = self.package / "editor/dist/assets/index-fixture.js"
        path.unlink()
        (path.parent / "unrelated.js").write_text("/* decoy */", encoding="utf-8")
        self.assert_failed(self.check_package(), "editor/dist/assets/index-fixture.js")

    def test_editor_external_absolute_and_escape_references_refuse(self):
        path = self.package / "editor/dist/index.html"
        original = path.read_text(encoding="utf-8")
        for reference in ("https://example.invalid/a.js", "/assets/index-fixture.js",
                          "../a.js", "%2e%2e/a.js", "assets/%2e%2e/a.js"):
            with self.subTest(reference=reference):
                path.write_text(original.replace("./assets/index-fixture.js", reference), encoding="utf-8")
                self.assert_failed(self.check_package(), "editor/dist/index.html")

    def test_editor_base_href_cannot_relocate_packaged_references(self):
        page = self.package / "editor/dist/index.html"
        original = page.read_text(encoding="utf-8")
        for href in ("https://example.invalid/", "//example.invalid/", "./elsewhere/", ""):
            with self.subTest(href=href):
                page.write_text('<base href="' + href + '">' + original, encoding="utf-8")
                self.assert_failed(self.check_package(), "base href")

    def test_case_mismatch_is_rejected_on_case_insensitive_host_too(self):
        original = self.package / "scripts/config.lua"
        renamed = original.with_name("Config.lua")
        original.rename(renamed)
        self.assert_failed(self.check_package(), "scripts/config.lua")

    def test_already_inspected_binary_mutation_is_detected(self):
        self.assertIsNotNone(native, "U22 native static checker is not implemented")
        real_header = native._header
        engine = self.package / "CaesuraAmeKAG.exe"
        changed = False

        def mutate_after_read(path, platform):
            nonlocal changed
            value = real_header(path, platform)
            if path == engine and not changed:
                body = bytearray(engine.read_bytes())
                body[-1] ^= 1
                engine.write_bytes(body)
                changed = True
            return value

        with patch.object(native, "_header", side_effect=mutate_after_read):
            report = self.check_package()
        self.assertTrue(changed)
        self.assert_failed(report, "changed during static inspection")
        self.assertFalse(report["input_stable"])

    def test_disabled_features_cannot_supply_contradictory_linkage(self):
        for required in (config(ffmpeg_linkage="shared"), config(steam_libraries=["runtime.dll"]),
                         config(sdl_linkage="static"), config(sdl_libraries=[]), config(schema=True)):
            self.assert_failed(self.check_package(required=required))
        required = config(sdl_linkage="static", sdl_libraries=[])
        (self.package / "SDL3.dll").unlink()
        self.assertTrue(self.check_package(required=required)["passed"])

    def test_cli_returns_json_and_refuses_package_owned_configuration(self):
        self.assertIsNotNone(native, "U22 native static checker is not implemented")
        external = self.root / "required.json"
        external.write_text(json.dumps(config()), encoding="utf-8")
        command = [sys.executable, "-X", "utf8", str(MODULE), "--package-root", str(self.package),
                   "--platform", "windows", "--required-config", str(external)]
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout)["runtime"], "NOT_RUN")
        owned = self.package / "required.json"
        owned.write_bytes(external.read_bytes())
        command[-1] = str(owned)
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", timeout=60)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(json.loads(result.stdout)["passed"])

        owned.unlink()
        owned.symlink_to(external)
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", timeout=60)
        self.assertNotEqual(result.returncode, 0, "A package-owned configuration alias is not an external input")
        self.assertFalse(json.loads(result.stdout)["passed"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
