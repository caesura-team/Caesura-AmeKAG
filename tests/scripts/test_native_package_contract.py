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
import stat
import time
import uuid
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "scripts/verify_native_package.py"
sys.path.insert(0, str(ROOT / "scripts"))
import package_runtime as process_runtime
if MODULE.is_file():
    spec = importlib.util.spec_from_file_location("native_package_contract", MODULE)
    native = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(native)
else:
    native = None

TEMPLATES = ("basic", "blank", "kag3", "live2d", "showcase")
FFMPEG_DLLS = ("avcodec-62.dll", "avformat-62.dll", "avutil-60.dll", "swscale-9.dll", "swresample-6.dll")

# Exact LC_LOAD_DYLIB bytes from the fixed final TGZ in run 35474593317,
# artifact 10593789574, Engine SHA256 8564619c916ee80556b53e15a9bc4cd259
# cc14c1f674be2b615dd2ad5e58a245, at offsets 2600/2672. The enclosing
# minimal files below are parser fixtures, never runnable Engine evidence.
HOSTED_OPENSSL_LOAD_COMMANDS = (
    bytes.fromhex("0c0000004800000018000000020000000000030000000300"
                  "2f6f70742f686f6d65627265772f6f70742f6f70656e73736c4033"
                  "2f6c69622f6c696273736c2e332e64796c69620000"),
    bytes.fromhex("0c0000005000000018000000020000000000030000000300"
                  "2f6f70742f686f6d65627265772f6f70742f6f70656e73736c4033"
                  "2f6c69622f6c696263727970746f2e332e64796c696200000000000000"),
)


def macho_dylib_command(name, command=0xC):
    encoded = name.encode("utf-8") + b"\0"
    size = (24 + len(encoded) + 7) & ~7
    return struct.pack("<6I", command, size, 24, 2, 0x30000, 0x30000) + encoded.ljust(size - 24, b"\0")


def macho_rpath_command(name):
    encoded = name.encode("utf-8") + b"\0"
    size = (12 + len(encoded) + 7) & ~7
    return struct.pack("<3I", 0x8000001C, size, 12) + encoded.ljust(size - 12, b"\0")


def macho_fixture(commands=(), *, file_type=2):
    """Bounded arm64 Mach-O header/load commands; no native code is executed."""
    commands = tuple(commands)
    body = b"".join(commands)
    return struct.pack("<8I", 0xFEEDFACF, 0x0100000C, 0, file_type,
                       len(commands), len(body), 0, 0) + body


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
        cls.base = Path(cls.base_temp.name).resolve()
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
        self.root = Path(self.temp.name).resolve()
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
        system = macho_dylib_command("/usr/lib/libSystem.B.dylib")
        (self.package / "CaesuraAmeKAG").write_bytes(macho_fixture([
            macho_dylib_command("@loader_path/SDL3.framework/SDL3"), system]))
        (self.package / "external/lua/lua").write_bytes(macho_fixture([system]))
        (self.package / "libSDL3.0.dylib").unlink()
        framework = self.package / "SDL3.framework"
        (framework / "Versions/A").mkdir(parents=True)
        (framework / "Versions/A/SDL3").write_bytes(macho_fixture([
            macho_dylib_command("@rpath/SDL3.framework/SDL3", 0xD), system], file_type=6))
        (framework / "Versions/Current").symlink_to("A", target_is_directory=True)
        # Match prepare_package(): materialized symlink targets use host
        # separators. Win32 rejects a raw multi-component POSIX target.
        (framework / "SDL3").symlink_to(Path("Versions") / "Current" / "SDL3")
        report = self.check_package("macos", config("macos", sdl_libraries=["SDL3.framework/SDL3"]))
        self.assertTrue(report["passed"], report)
        self.assertEqual(report["binaries"]["engine"]["relative_path"], "CaesuraAmeKAG")
        self.assertEqual(report["binaries"]["engine"]["header_format"], "Mach-O")

    def install_macos_dependency_fixture(self):
        self.install_binaries("macos")
        system = macho_dylib_command("/usr/lib/libSystem.B.dylib")
        for name in ("libSDL3.0.dylib", "libcrypto.3.dylib"):
            (self.package / name).write_bytes(macho_fixture(
                [macho_dylib_command("@rpath/" + name, 0xD), system], file_type=6))
        (self.package / "libssl.3.dylib").write_bytes(macho_fixture([
            macho_dylib_command("@rpath/libssl.3.dylib", 0xD),
            macho_dylib_command("@loader_path/libcrypto.3.dylib"), system], file_type=6))
        (self.package / "external/lua/lua").write_bytes(macho_fixture([system]))
        (self.package / "CaesuraAmeKAG").write_bytes(macho_fixture([
            macho_dylib_command("@rpath/libSDL3.0.dylib"),
            macho_dylib_command("@rpath/libssl.3.dylib"),
            macho_dylib_command("@rpath/libcrypto.3.dylib"),
            macho_dylib_command("/System/Library/Frameworks/Metal.framework/Versions/A/Metal"),
            system, macho_rpath_command("@loader_path")]))

    def test_macos_dependency_closure_rejects_actual_hosted_absolute_load_commands(self):
        self.install_macos_dependency_fixture()
        self.assertEqual([hashlib.sha256(v).hexdigest() for v in HOSTED_OPENSSL_LOAD_COMMANDS], [
            "a476ae0a6076a6723f0281412bc376f6ca5de975c9c25b8bda3ec791540d6552",
            "da7ddf8316174b13f9cd83decb286007d992ebf4a1c2721184a5d626eb1d8740"])
        (self.package / "CaesuraAmeKAG").write_bytes(macho_fixture([
            *HOSTED_OPENSSL_LOAD_COMMANDS, macho_dylib_command("@rpath/libSDL3.0.dylib"),
            macho_rpath_command("@loader_path")]))
        # Deliberately retain the old external required set (SDL only): merely
        # supplying valid SDL must not hide the binary's actual host dependency.
        self.assert_failed(self.check_package("macos"), "libssl.3.dylib")

    def test_macos_dependency_closure_rejects_transitive_host_crypto(self):
        self.install_macos_dependency_fixture()
        (self.package / "libssl.3.dylib").write_bytes(macho_fixture([
            macho_dylib_command("@rpath/libssl.3.dylib", 0xD),
            HOSTED_OPENSSL_LOAD_COMMANDS[1]], file_type=6))
        self.assert_failed(self.check_package("macos"), "libcrypto.3.dylib")

    def test_macos_dependency_closure_rejects_missing_package_crypto(self):
        self.install_macos_dependency_fixture()
        (self.package / "libcrypto.3.dylib").unlink()
        self.assert_failed(self.check_package("macos"), "libcrypto.3.dylib")

    def test_macos_dependency_closure_rejects_rpath_escape(self):
        self.install_macos_dependency_fixture()
        foreign = self.root / "foreign"
        foreign.mkdir()
        (foreign / "libssl.3.dylib").write_bytes((self.package / "libssl.3.dylib").read_bytes())
        (self.package / "CaesuraAmeKAG").write_bytes(macho_fixture([
            macho_dylib_command("@rpath/libssl.3.dylib"), macho_rpath_command("@loader_path/../foreign")]))
        self.assert_failed(self.check_package("macos"))

    def test_macos_dependency_closure_rejects_malformed_load_command_bounds(self):
        self.install_macos_dependency_fixture()
        good = macho_fixture([macho_dylib_command("@rpath/libSDL3.0.dylib")])
        cases = {}
        for label, offset, value in (("zero-command-size", 36, 0),
                                     ("short-command", 36, 7),
                                     ("past-file-command", 36, len(good) + 8),
                                     ("name-inside-header", 40, 8),
                                     ("name-past-command", 40, len(good)),
                                     ("extra-command-count", 16, 2),
                                     ("oversized-command-region", 20, len(good) + 8)):
            changed = bytearray(good)
            struct.pack_into("<I", changed, offset, value)
            cases[label] = bytes(changed)
        cases["unterminated-name"] = macho_fixture([
            struct.pack("<6I", 0xC, 32, 24, 2, 0x30000, 0x30000) + b"no-zero!"])
        cases["truncated-header"] = good[:24]
        for label, data in cases.items():
            with self.subTest(case=label):
                (self.package / "CaesuraAmeKAG").write_bytes(data)
                self.assert_failed(self.check_package("macos"))

    def test_macos_dependency_closure_accepts_complete_local_and_system_graph(self):
        self.install_macos_dependency_fixture()
        report = self.check_package("macos")
        self.assertTrue(report["passed"], report)
        self.assertEqual(report["runtime"], "NOT_RUN")
        # Apple shared-cache dependencies need not be ordinary package files.
        self.assertFalse((self.package / "usr/lib/libSystem.B.dylib").exists())
        self.assertFalse((self.package / "System/Library/Frameworks/Metal.framework").exists())

    def test_macos_dependency_closure_rejects_system_prefix_lookalikes(self):
        self.install_macos_dependency_fixture()
        for name in ("/usr/library-foreign/libcrypto.3.dylib",
                     "/System/LibraryFake/libcrypto.3.dylib"):
            with self.subTest(dependency=name):
                (self.package / "CaesuraAmeKAG").write_bytes(macho_fixture([macho_dylib_command(name)]))
                self.assert_failed(self.check_package("macos"), "libcrypto.3.dylib")

    def test_posix_enabled_sdks_require_caller_resolved_linkage_and_paths(self):
        for platform in ("linux", "macos"):
            self.install_binaries(platform)
            if platform == "macos":
                system = macho_dylib_command("/usr/lib/libSystem.B.dylib")
                (self.package / "CaesuraAmeKAG").write_bytes(macho_fixture([
                    macho_dylib_command("@loader_path/libSDL3.0.dylib"), system]))
                (self.package / "external/lua/lua").write_bytes(macho_fixture([system]))
                (self.package / "libSDL3.0.dylib").write_bytes(macho_fixture([
                    macho_dylib_command("@rpath/libSDL3.0.dylib", 0xD), system], file_type=6))
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
                library_name = required[feature + "_libraries"][0]
                library_bytes = (macho_fixture([
                    macho_dylib_command("@rpath/" + library_name, 0xD), system], file_type=6)
                    if platform == "macos" else binary_header(platform))
                (self.package / library_name).write_bytes(library_bytes)
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


FIFO_SELECTION_CHILD = r'''
import hashlib, json, os, stat, sys, time, traceback
from pathlib import Path
source, root = map(Path, sys.argv[1:])
sys.path.insert(0, str(source / 'scripts'))
import macos_runtime_selection as selection
parser = selection.inspect_macho
deadline = time.monotonic() + 15
while not (root / 'controller-ready').exists() and time.monotonic() < deadline:
    time.sleep(0.02)
if not (root / 'controller-ready').exists():
    raise RuntimeError('Controller ownership barrier was not released')
positive = parser(root / 'ssl.dylib')
if positive['sha256'] != hashlib.sha256((root / 'ssl.dylib').read_bytes()).hexdigest():
    raise RuntimeError('Ordinary fixture positive control failed')
def descriptor_snapshot():
    result = []
    # Popen close_fds leaves 0/1/2; this private single-thread child opens its
    # fixture descriptors immediately above them. No /proc is needed on Mac.
    for fd in range(64):
        try:
            info = os.fstat(fd)
        except OSError:
            continue
        result.append([fd, info.st_dev, info.st_ino, stat.S_IFMT(info.st_mode)])
    return result
before = descriptor_snapshot()
def actual_fifo_boundary(path):
    path = Path(path)
    if path != root / 'ssl.dylib' or (root / 'before-parser.json').exists():
        raise RuntimeError('Unexpected parser boundary')
    path.rename(root / 'held-original.dylib')
    os.mkfifo(path, 0o600)
    marker = root / 'before-parser.json'
    temporary_marker = root / 'before-parser.tmp'
    temporary_marker.write_text(json.dumps({'actual_fifo':stat.S_ISFIFO(path.lstat().st_mode)}))
    os.replace(temporary_marker, marker)
    return parser(path)
selection.inspect_macho = actual_fifo_boundary
try:
    selection.select_openssl(root / 'ssl.dylib', root / 'crypto.dylib', 'Release')
except (OSError, ValueError) as error:
    result = {'status':'REJECTED', 'error':str(error), 'exception':type(error).__name__}
else:
    result = {'status':'UNEXPECTED_ACCEPT'}
result.update(positive_sha256=positive['sha256'], descriptors_before=before,
              descriptors_after=descriptor_snapshot())
(root / 'child-result.json').write_text(json.dumps(result,indent=2))
print(json.dumps(result),flush=True)
sys.exit(0 if result['status']=='REJECTED' else 2)
'''


class MachOInputOpenContract(unittest.TestCase):
    if os.name == 'posix':
        def test_selector_fifo_reopen_rejects_without_writer_and_closes_descriptors(self):
            temporary = tempfile.TemporaryDirectory(prefix='caesura-macho-fifo-')
            root = Path(temporary.name).resolve()
            evidence = os.environ.get('CAESURA_RUNTIME_INSTALL_EVIDENCE')
            if evidence:
                # Retain first failures including the actual FIFO; explicit
                # detachment prevents TemporaryDirectory finalization deletion.
                temporary._finalizer.detach()
                locator = Path(evidence).resolve() / ('fifo-maintenance-' + uuid.uuid4().hex + '.json')
                locator.parent.mkdir(parents=True, exist_ok=True)
                locator.write_text(json.dumps({'retained_fixture':str(root)}), encoding='utf-8')
            else:
                self.addCleanup(temporary.cleanup)
            for name in ('ssl', 'crypto'):
                (root / (name + '.dylib')).write_bytes(macho_fixture([
                    macho_dylib_command('@rpath/' + name + '.dylib', 0xD)], file_type=6))
            script = root / 'fifo-child.py'
            script.write_text(FIFO_SELECTION_CHILD, encoding='utf-8')
            source_paths = [ROOT / 'scripts/macos_runtime_selection.py', ROOT / 'scripts/macho_dependencies.py']
            digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
            source_before = {str(p):digest(p) for p in source_paths}
            python = process_runtime.process_identity(os.getpid()).executable
            process = owner = writer = None
            record = {'stop_requested':False, 'forced_kill':False, 'writer_needed':False}
            with (root / 'child.stdout.log').open('wb') as stdout, (root / 'child.stderr.log').open('wb') as stderr:
                try:
                    process = subprocess.Popen([python, '-I', '-B', str(script), str(ROOT), str(root)],
                                               stdout=stdout, stderr=stderr, cwd=root, close_fds=True)
                    owner = process_runtime.process_identity(process.pid)
                    record['owner'] = vars(owner)
                    (root / 'controller-ready').write_text('owned', encoding='ascii')
                    deadline = time.monotonic() + 15
                    marker = root / 'before-parser.json'
                    while not marker.exists() and process.poll() is None and time.monotonic() < deadline:
                        time.sleep(0.02)
                    self.assertTrue(marker.exists(), 'Child failed before the actual parser boundary')
                    self.assertTrue(json.loads(marker.read_bytes())['actual_fifo'])
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        record['writer_needed'] = True
                        self.assertEqual(process_runtime.process_identity(process.pid), owner)
                        fifo = root / 'ssl.dylib'
                        self.assertTrue(stat.S_ISFIFO(fifo.lstat().st_mode))
                        # Release the old bug by a real nonblocking rendezvous,
                        # then let its actual fstat rejection finish naturally.
                        writer = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK | getattr(os, 'O_NOFOLLOW', 0))
                        process.wait(timeout=3)
                finally:
                    if writer is not None:
                        os.close(writer)
                    if process is not None and process.poll() is None:
                        if owner is None:
                            # The startup barrier makes a fresh identity query
                            # possible before this private child can run work.
                            # If inspection itself failed, let its bounded
                            # unreleased startup barrier exit and reap it.
                            try:
                                owner = process_runtime.process_identity(process.pid)
                            except process_runtime.RuntimeContractError:
                                process.wait(timeout=16)
                        try:
                            same_owner = process.poll() is None and process_runtime.process_identity(process.pid) == owner
                        except process_runtime.RuntimeContractError:
                            process.wait(timeout=1)
                        else:
                            if process.poll() is None:
                                self.assertTrue(same_owner, 'Refusing to stop a changed process identity')
                                record['stop_requested'] = True
                                process.terminate()
                                try:
                                    process.wait(timeout=2)
                                except subprocess.TimeoutExpired:
                                    self.assertEqual(process_runtime.process_identity(process.pid), owner)
                                    record['forced_kill'] = True
                                    process.kill()
                                    process.wait(timeout=2)
                    record['exit_code'] = process.poll() if process is not None else None
                    record['cleanup'] = 'COMPLETE' if process is not None and process.returncode is not None else 'INCOMPLETE'
                    (root / 'owned-result.json').write_text(json.dumps(record, indent=2), encoding='utf-8')
            text = (root / 'child.stdout.log').read_text(encoding='utf-8', errors='replace') + (root / 'child.stderr.log').read_text(encoding='utf-8', errors='replace')
            self.assertFalse(record['writer_needed'], 'Actual parser blocked until an external FIFO writer arrived\n' + text)
            self.assertEqual(record['exit_code'], 0, text)
            self.assertEqual(record['cleanup'], 'COMPLETE')
            self.assertFalse(record['stop_requested'])
            self.assertFalse(record['forced_kill'])
            result = json.loads((root / 'child-result.json').read_bytes())
            self.assertEqual(result['status'], 'REJECTED')
            self.assertIn('not a regular file', result['error'])
            self.assertEqual(result['descriptors_before'], result['descriptors_after'])
            self.assertEqual(result['positive_sha256'], digest(root / 'held-original.dylib'))
            self.assertEqual(source_before, {str(p):digest(p) for p in source_paths})



if __name__ == "__main__":
    unittest.main(verbosity=2)
