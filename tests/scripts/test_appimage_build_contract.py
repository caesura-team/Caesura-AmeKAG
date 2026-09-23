"""Actual archive/installation tests with an explicit appimagetool boundary double."""
from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import build_appimage as builder
from package_verification import PackageVerificationError, _sha256_file
import test_native_package_contract as native_fixture


class AppImageBuildTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        native_fixture.NativePackageContract.setUpClass()

    @classmethod
    def tearDownClass(cls):
        native_fixture.NativePackageContract.tearDownClass()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="caesura-appimage-build-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.stem = "CaesuraAmeKAG-1.0.1-Linux-x86_64"
        self.install = self.root / "install"
        shutil.copytree(native_fixture.NativePackageContract.base, self.install)
        header = bytearray(b"\x7fELF\x02\x01\x01" + b"\0" * 57)
        struct.pack_into("<H", header, 18, 62)
        for relative in ("CaesuraAmeKAG", "external/lua/lua", "libSDL3.so.0"):
            path = self.install / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(header)
            path.chmod(0o755)
        (self.install / "projects/作者 项目.txt").write_bytes(b"project payload preserved")
        (self.install / "extra-library.so").write_bytes(b"unlisted future payload preserved")
        self.tgz = self.root / (self.stem + ".tar.gz")
        self.make_archive()
        self.requirements = self.root / "requirements.json"
        self.metadata = {"schema": "caesura.package-build.v1", "platform": "linux",
            "configuration": "Release", "version": "1.0.1", "archive_basename": self.stem,
            "engine_relative_path": "CaesuraAmeKAG", "lua_relative_path": "external/lua/lua",
            "artifacts": {"tgz": self.tgz.name, "appimage": self.stem + ".AppImage"},
            "required_configuration": native_fixture.config("linux")}
        self.requirements.write_text(json.dumps(self.metadata), encoding="utf-8")
        self.tool = self.root / "tool with space.AppImage"
        self.tool.write_bytes(b"Explicit locked tool boundary fixture, never executed")
        self.tool.chmod(0o755)
        self.runtime = self.root / "type2-runtime"
        runtime = bytearray(header)
        runtime[8:11] = b"AI\x02"
        self.runtime.write_bytes(runtime)
        self.work = self.root / "new-work"
        self.output = self.root / (self.stem + ".AppImage")
        self.behavior = "success"
        self.calls = []

    def make_archive(self, stem=None):
        with tarfile.open(self.tgz, "w:gz") as stream:
            stream.add(self.install, arcname=stem or self.stem)

    def owned(self, argv, cwd, stdout, stderr, timeout):
        self.calls.append(list(argv))
        request = json.loads(Path(argv[-1]).read_text(encoding="utf-8"))
        self.assertEqual(argv[1:3], ["-I", "-S"])
        self.assertEqual(request["argv"][1], str(self.work / "appdir/package"))
        self.assertEqual(request["argv"][3:], ["--runtime-file", str(self.work / "locked-runtime")])
        self.assertEqual(request["env"]["HOME"], str(self.work / "home"))
        self.assertEqual(request["env"]["APPIMAGE_EXTRACT_AND_RUN"], "1")
        self.assertEqual(request["env"]["ARCH"], "x86_64")
        self.assertFalse({"CAESURA_LUA", "LD_LIBRARY_PATH", "PYTHONPATH", "NODE_OPTIONS", "HTTP_PROXY"} & set(request["env"]))
        appdir = Path(request["argv"][1])
        self.assertEqual((appdir / ".DirIcon").read_bytes(), (appdir / "caesura-amekag.png").read_bytes())
        output = Path(request["argv"][2])
        data = bytearray(b"\x7fELF\x02\x01\x01\x00AI\x02" + b"\0" * 53)
        struct.pack_into("<H", data, 18, 62)
        if self.behavior != "missing-output":
            output.write_bytes(data if self.behavior != "invalid-output" else b"not an AppImage")
            output.chmod(0o755)
        if self.behavior == "mutate-input":
            self.tgz.write_bytes(b"mutated input")
        if self.behavior == "mutate-appdir":
            (appdir / "demo/entry.lua").write_bytes(b"mutated appdir")
        if self.behavior == "mutate-tool":
            self.tool.write_bytes(b"mutated tool")
        if self.behavior == "occupy-output":
            self.output.write_bytes(b"other producer owns this")
        result = {"status": "EXITED", "actual_exit_code": 7 if self.behavior == "tool-fails" else 0,
                  "pid": 12345, "test_boundary_double": True}
        Path(request["result_path"]).write_text(json.dumps(result), encoding="utf-8")
        if self.behavior == "timeout":
            raise subprocess.TimeoutExpired(argv, timeout)
        return 0

    def build(self, **changes):
        arguments = dict(tgz_path=self.tgz, expected_sha256=_sha256_file(self.tgz),
            requirements_path=self.requirements, requirements_sha256=_sha256_file(self.requirements),
            appimagetool_path=self.tool, appimagetool_sha256=_sha256_file(self.tool),
            runtime_file_path=self.runtime, runtime_sha256=_sha256_file(self.runtime),
            work_dir=self.work, output_path=self.output)
        arguments.update(changes)
        with patch.object(builder, "_host_platform", return_value="linux"), \
                patch.object(builder, "run_owned_command", side_effect=self.owned):
            return builder.build_appimage(**arguments)

    def fail_report(self, report):
        self.assertEqual(report["status"], "APPIMAGE_BUILD_FAIL", report.get("errors"))
        self.assertEqual(report["runtime"], "NOT_RUN")
        self.assertFalse(report["accepted"])
        self.assertEqual(json.loads((self.work / "appimage-build.json").read_text(encoding="utf-8"))["status"], report["status"])

    def test_complete_install_tree_stays_at_root_and_output_is_explicit(self):
        report = self.build()
        self.assertEqual(report["status"], "APPIMAGE_BUILT", report.get("errors"))
        self.assertEqual(report["runtime"], "NOT_RUN")
        self.assertFalse(report["accepted"])
        appdir = Path(report["appdir"])
        self.assertTrue((appdir / "CaesuraAmeKAG").is_file())
        self.assertFalse((appdir / "usr/bin/CaesuraAmeKAG").exists())
        for file in self.install.rglob("*"):
            if file.is_file():
                self.assertEqual(file.read_bytes(), (appdir / file.relative_to(self.install)).read_bytes())
        script = (appdir / "AppRun").read_text(encoding="utf-8")
        self.assertIn('exec "$APPDIR/CaesuraAmeKAG" "$@"', script)
        self.assertTrue((appdir / "caesura-amekag.desktop").is_file())
        self.assertEqual(report["output"]["sha256"], _sha256_file(self.output))
        self.assertEqual(len(self.calls), 1)

    def test_packaged_desktop_entries_have_freedesktop_line_endings(self):
        # appimagetool's real desktop validator rejects CRLF, including when
        # this builder consumes a Windows checkout from WSL.
        report = self.build()
        self.assertEqual(report["status"], "APPIMAGE_BUILT", report.get("errors"))
        appdir = Path(report["appdir"])
        for relative in ("caesura-amekag.desktop", "usr/share/applications/caesura-amekag.desktop"):
            with self.subTest(relative=relative):
                content = (appdir / relative).read_bytes()
                self.assertTrue(content.startswith(b"[Desktop Entry]\n"))
                self.assertNotIn(b"\r", content, "Desktop entry lines must use LF on every build host")
                self.assertTrue(content.endswith(b"\n"))

    def test_prelocked_digest_mismatch_never_launches_tool(self):
        self.fail_report(self.build(expected_sha256="0" * 64))
        self.assertEqual(self.calls, [])
        self.assertFalse(self.output.exists())

    def test_metadata_and_tool_digest_mismatch_never_launch(self):
        for index, key in enumerate(("requirements_sha256", "appimagetool_sha256", "runtime_sha256")):
            self.work = self.root / f"work-{index}"
            self.fail_report(self.build(**{key: "0" * 64}))
        self.assertEqual(self.calls, [])

    def test_wrong_archive_root_is_not_discovered(self):
        self.make_archive("another-root")
        self.fail_report(self.build())
        self.assertEqual(self.calls, [])

    def test_missing_required_sdl_and_demo_fail_before_tool(self):
        for index, relative in enumerate(("libSDL3.so.0", "demo/entry.lua")):
            original = (self.install / relative).read_bytes()
            (self.install / relative).unlink()
            self.make_archive()
            self.work = self.root / f"work-{index}"
            self.fail_report(self.build())
            (self.install / relative).write_bytes(original)
        self.assertEqual(self.calls, [])

    def test_existing_work_and_output_are_preserved(self):
        self.output.write_bytes(b"existing user's package")
        with self.assertRaises(Exception):
            self.build()
        self.assertFalse(self.work.exists())
        self.assertEqual(self.output.read_bytes(), b"existing user's package")
        self.output.unlink()
        self.work.mkdir()
        (self.work / "keep").write_bytes(b"existing work")
        with self.assertRaises(Exception):
            self.build()
        self.assertEqual(list(self.work.iterdir()), [self.work / "keep"])

    def test_repository_work_refusal_is_before_any_write(self):
        repository = self.root / "repository"
        repository.mkdir()
        (repository / ".git").write_bytes(b"marker")
        self.work = repository / "work"
        with self.assertRaises(Exception):
            self.build()
        self.assertFalse(self.work.exists())

    def test_failed_or_missing_or_invalid_output_never_publishes(self):
        for index, behavior in enumerate(("tool-fails", "missing-output", "invalid-output", "timeout")):
            self.behavior = behavior
            self.work = self.root / f"work-{index}"
            self.fail_report(self.build())
            self.assertFalse(self.output.exists())

    def test_mutated_inputs_appdir_and_racing_output_are_not_accepted(self):
        for index, behavior in enumerate(("mutate-input", "mutate-appdir", "mutate-tool", "occupy-output")):
            self.behavior = behavior
            self.work = self.root / f"work-{index}"
            self.make_archive()
            self.tool.write_bytes(b"Explicit locked tool boundary fixture, never executed")
            self.fail_report(self.build())
            if behavior == "occupy-output":
                self.assertEqual(self.output.read_bytes(), b"other producer owns this")
            else:
                self.assertFalse(self.output.exists())

    def test_tool_cannot_redirect_output_by_replacing_its_parent(self):
        parent = self.root / "explicit-output"
        parent.mkdir()
        redirected = self.root / "different-output"
        redirected.mkdir()
        self.output = parent / (self.stem + ".AppImage")
        original = self.owned
        attempted = []
        def replace_parent(*args, **kwargs):
            result = original(*args, **kwargs)
            attempted.append(True)
            parent.rename(self.root / "original-output-directory")
            parent.symlink_to(redirected, target_is_directory=True)
            return result
        self.owned = replace_parent
        report = self.build()
        self.assertTrue(attempted, report)
        self.fail_report(report)
        self.assertFalse((redirected / self.output.name).exists())
        self.assertFalse((self.root / "original-output-directory" / self.output.name).exists())

    if os.name != "nt":
        def test_posix_publish_uses_bound_parent_if_path_changes_after_verification(self):
            parent = self.root / "bound-output"
            parent.mkdir()
            redirected = self.root / "unrelated-output"
            redirected.mkdir()
            moved = self.root / "original-bound-output"
            initial = parent.stat()
            binding = builder._OutputParent(parent, (initial.st_dev, initial.st_ino))
            original_open = os.open
            calls = []
            def replace_at_create(path, flags, mode=0o777, *, dir_fd=None):
                if dir_fd == binding.fd:
                    calls.append((str(path), flags, dir_fd))
                    parent.rename(moved)
                    parent.symlink_to(redirected, target_is_directory=True)
                return original_open(path, flags, mode, dir_fd=dir_fd)
            try:
                with patch.object(builder.os, "open", side_effect=replace_at_create):
                    descriptor = binding.create(self.stem + ".AppImage")
                with os.fdopen(descriptor, "wb") as output:
                    output.write(b"bytes in the originally bound directory")
                self.assertEqual(len(calls), 1)
                self.assertTrue(calls[0][1] & os.O_EXCL)
                self.assertTrue(calls[0][1] & os.O_NOFOLLOW)
                self.assertFalse((redirected / (self.stem + ".AppImage")).exists())
                self.assertEqual((moved / (self.stem + ".AppImage")).read_bytes(), b"bytes in the originally bound directory")
                with self.assertRaises((ValueError, PackageVerificationError)):
                    binding.verify()
            finally:
                binding.close()

    def test_existing_apprun_is_not_overwritten(self):
        (self.install / "AppRun").write_bytes(b"existing payload")
        self.make_archive()
        self.fail_report(self.build())
        self.assertEqual(self.calls, [])

    def test_cli_requires_all_explicit_inputs(self):
        result = subprocess.run([sys.executable, str(ROOT / "scripts/build_appimage.py")],
                                capture_output=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"required", result.stderr)

    def test_actual_owned_helper_preserves_arguments_environment_and_exit(self):
        self.work.mkdir()
        (self.work / "cwd").mkdir()
        code = "import json,os,sys; print(json.dumps({'argument':sys.argv[1], 'marker':os.environ.get('BUILD_MARKER'), 'inherited':os.environ.get('UNEXPECTED_BUILD_SECRET')})); sys.exit(7)"
        environment = {"BUILD_MARKER": "explicit environment"}
        if os.name == "nt":
            environment["SystemRoot"] = os.environ["SystemRoot"]
        literal = "argument with spaces ; $(no shell)"
        python = Path(sys.executable).resolve(strict=True)
        with patch.dict(os.environ, {"UNEXPECTED_BUILD_SECRET": "must not inherit"}):
            report = builder._invoke_tool(self.work, python, _sha256_file(python),
                [str(python), "-I", "-c", code, literal], environment, 10)
        self.assertEqual(report["owned_tree_cleanup"], "COMPLETE", report)
        self.assertEqual(report["tool_result"]["actual_exit_code"], 7, report)
        self.assertEqual(report["status"], "FAILED", report)
        observed = json.loads(Path(report["stdout_path"]).read_text(encoding="utf-8"))
        self.assertEqual(observed, {"argument": literal, "marker": "explicit environment", "inherited": None})

    def test_actual_owned_helper_timeout_keeps_failed_receipt(self):
        self.work.mkdir()
        (self.work / "cwd").mkdir()
        python = Path(sys.executable).resolve(strict=True)
        report = builder._invoke_tool(self.work, python, _sha256_file(python),
            [str(python), "-I", "-c", "import time; time.sleep(60)"], {}, 0.4)
        self.assertEqual(report["status"], "TIMED_OUT", report)
        self.assertEqual(report["owned_tree_cleanup"], "COMPLETE", report)
        self.assertEqual(json.loads((self.work / "command/run.json").read_text(encoding="utf-8"))["status"], "TIMED_OUT")

    def test_shell_wrapper_forwards_literal_explicit_paths(self):
        configured = os.environ.get("CAESURA_TEST_BASH")
        bash = configured or ("C:/Program Files/Git/bin/bash.exe" if os.name == "nt" else shutil.which("bash"))
        self.assertTrue(bash and Path(bash).is_file(), "Explicit usable Bash is required for wrapper contract")
        python = Path(sys.executable).as_posix()
        if os.name == "nt":
            python = "/" + python[0].lower() + python[2:]
        output = self.root / "literal ; $(no-shell).AppImage"
        args = [str(bash), str(ROOT / "scripts/build_appimage.sh"), "--python", python,
            "--tgz", str(self.tgz), "--sha256", _sha256_file(self.tgz),
            "--requirements", str(self.requirements), "--requirements-sha256", _sha256_file(self.requirements),
            "--appimagetool", str(self.tool), "--appimagetool-sha256", _sha256_file(self.tool),
            "--runtime-file", str(self.runtime), "--runtime-sha256", _sha256_file(self.runtime),
            "--work", str(self.work), "--output", str(output)]
        result = subprocess.run(args, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 1, result.stderr)
        report = json.loads(result.stdout.decode("utf-8"))
        self.assertEqual(Path(report["requested_output"]), output)
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
