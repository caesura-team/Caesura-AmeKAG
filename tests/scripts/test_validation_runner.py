"""Behavioral tests for the validation executor; fixtures are never release evidence."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from run_validation import fingerprint_paths, run_profile


class ValidationRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="caesura-validation-")
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name)
        # Repository rules require a .git marker before invoking Git.
        (self.repo / ".git").mkdir()
        subprocess.run(["git", "init", "-q"], cwd=self.repo, check=True)
        (self.repo / ".gitignore").write_text("raw/\n", encoding="utf-8")
        (self.repo / "fixture.txt").write_text("fixture v1\n", encoding="utf-8")
        subprocess.run(["git", "add", ".gitignore", "fixture.txt"], cwd=self.repo, check=True)
        subprocess.run(
            ["git", "-c", "user.name=Validation Fixture", "-c",
             "user.email=fixture@example.invalid", "commit", "-qm", "test fixture"],
            cwd=self.repo, check=True,
        )
        self.profile_file = self.repo / "profiles.json"

    def write_profile(self, checks, **overrides):
        self.profile_file.write_text(json.dumps({
            "schema_version": 1,
            "fixture_paths": ["fixture.txt"],
            "profiles": {
                "fixture": {
                    "platform": {"win32": "windows", "darwin": "macos"}.get(sys.platform, "linux"),
                    "configuration": "Debug",
                    "fixture_paths": ["fixture.txt"],
                    "checks": checks, **overrides,
                }
            },
        }), encoding="utf-8")
        return self.profile_file

    def check(self, name="sample", code="print('hello')", **extra):
        return {
            "id": name, "parser": "exit-code", "required": True,
            "command": ["{python}", "-c", code], "cwd": "{repo}",
            "binary": "{python}", "timeout_seconds": 5, **extra,
        }

    def execute(self, checks, directory="raw/one", configuration="Debug", **profile_overrides):
        self.write_profile(checks, **profile_overrides)
        return run_profile(
            repo=self.repo, profile_file=self.profile_file, profile_name="fixture",
            build_dir=self.repo / "build", configuration=configuration,
            run_dir=self.repo / directory, purpose="test-fixture",
        )

    def test_captures_real_exit_streams_digest_and_identity(self):
        result = self.execute([self.check(code="import sys; print('PASS'); print('error', file=sys.stderr); sys.exit(7)")])
        check = result["checks"][0]
        self.assertEqual(check["exit_code"], 7)
        self.assertEqual(result["purpose"], "test-fixture")
        self.assertTrue(result["dirty"])
        self.assertEqual(len(result["source_sha"]), 40)
        self.assertEqual(len(result["worktree_fingerprint"]), 64)
        for field, expected in [("stdout", b"PASS"), ("stderr", b"error")]:
            ref = check[field]
            data = (self.repo / "raw/one" / ref["path"]).read_bytes()
            self.assertIn(expected, data)
            self.assertEqual(hashlib.sha256(data).hexdigest(), ref["sha256"])
        self.assertLessEqual(check["started_at"], check["finished_at"])
        self.assertEqual(check["binary"]["sha256"], hashlib.sha256(Path(sys.executable).read_bytes()).hexdigest())
        saved = json.loads((self.repo / "raw/one/run.json").read_text(encoding="utf-8"))
        self.assertEqual(saved, result)

    def test_binds_sidecars_from_children_whose_wrapper_swallows_output(self):
        # These are transport fixtures written by actual Python children, not
        # claims that a compiler sanitizer ran. The wrapper accepts their
        # expected nonzero exits and deliberately withholds both output streams.
        child = (
            "import json,os,sys; from pathlib import Path; "
            "scope=json.loads(os.environ.get('CAESURA_VALIDATION_SANITIZER_CAPTURE','null')); "
            "payload=('TRANSPORT FIXTURE ONLY pid='+str(os.getpid())+'\\n').encode()\n"
            "if scope is not None:\n"
            "    (Path(scope['directory'])/(scope['prefix']+'.'+str(os.getpid()))).write_bytes(payload)\n"
            "print('swallowed child stdout'); print('swallowed child stderr',file=sys.stderr); "
            "sys.exit(17)\n"
        )
        wrapper = (
            "import json,subprocess,sys; "
            f"children=[subprocess.Popen([sys.executable,'-c',{child!r}],"
            "stdout=subprocess.PIPE,stderr=subprocess.PIPE) for _ in range(2)]\n"
            "for child in children:\n"
            "    stdout,stderr=child.communicate(timeout=10)\n"
            "    assert child.returncode==17 and b'swallowed' in stdout and b'swallowed' in stderr\n"
            "print(json.dumps({'pids':[child.pid for child in children]}))\n"
        )
        result = self.execute([self.check(code=wrapper, timeout_seconds=20)])
        row = result["checks"][0]
        self.assertEqual(row["exit_code"], 0)
        raw = self.repo / "raw/one"
        output = (raw / row["stdout"]["path"]).read_text(encoding="utf-8")
        pids = json.loads(output)["pids"]
        self.assertEqual(len(set(pids)), 2)
        self.assertNotIn("swallowed", output)
        self.assertEqual((raw / row["stderr"]["path"]).read_bytes(), b"")
        capture = row["sanitizer_capture"]
        self.assertEqual(capture["version"], 1)
        self.assertTrue(capture["complete"])
        self.assertEqual(capture["directory"], "sanitizer/sample")
        self.assertEqual(capture["prefix"], "sanitizer")
        self.assertEqual(capture["options_contract"], "llvm-common-log-path-v1")
        self.assertEqual({ref["path"] for ref in capture["files"]},
                         {f"sanitizer/sample/sanitizer.{pid}" for pid in pids})
        for ref in capture["files"]:
            payload = (raw / ref["path"]).read_bytes()
            self.assertTrue(payload.startswith(b"TRANSPORT FIXTURE ONLY pid="))
            self.assertEqual(ref["sha256"], hashlib.sha256(payload).hexdigest())
            self.assertEqual(ref["size_bytes"], len(payload))

    def test_clean_checks_have_separate_empty_capture_directories(self):
        result = self.execute([self.check("first"), self.check("second")])
        for row in result["checks"]:
            capture = row["sanitizer_capture"]
            self.assertTrue(capture["complete"])
            self.assertEqual(capture["directory"], "sanitizer/" + row["id"])
            self.assertEqual(capture["files"], [])
            self.assertEqual(list((self.repo / "raw/one" / capture["directory"]).iterdir()), [])

    def test_fixture_run_overrides_parent_capture_without_global_mutation(self):
        names = ("ASAN_OPTIONS", "UBSAN_OPTIONS", "LSAN_OPTIONS", "TSAN_OPTIONS")
        pollution = {name: "log_path=outside:suppressions=secret-file:print_summary=0" for name in names}
        pollution["CAESURA_VALIDATION_SANITIZER_CAPTURE"] = "invalid outer scope must be replaced"
        code = ("import json,os; print(json.dumps({key:os.environ.get(key) for key in "
                + repr((*names, "CAESURA_VALIDATION_SANITIZER_CAPTURE")) + "}))")
        with patch.dict(os.environ, pollution):
            before = dict(os.environ)
            result = self.execute([self.check(code=code)])
            self.assertEqual(os.environ, before)
        row = result["checks"][0]
        self.assertEqual(row["exit_code"], 0)
        observed = json.loads((self.repo / "raw/one" / row["stdout"]["path"]).read_text(encoding="utf-8"))
        scope = json.loads(observed["CAESURA_VALIDATION_SANITIZER_CAPTURE"])
        self.assertEqual(scope["run_id"], result["run_id"])
        self.assertEqual(scope["check_id"], "sample")
        self.assertEqual(scope["purpose"], "test-fixture")
        self.assertEqual(Path(scope["directory"]), (self.repo / "raw/one/sanitizer/sample").resolve())
        expected = f'log_path="{Path(scope["directory"]) / "sanitizer"}":log_exe_name=0:print_summary=1:color=never'
        for name in names:
            self.assertEqual(observed[name], expected)
            self.assertNotIn("secret-file", observed[name])

    def test_unconfirmed_owned_cleanup_cannot_claim_complete_capture(self):
        with patch("run_validation.run_owned_command", side_effect=OSError("owned cleanup failed")):
            result = self.execute([self.check()])
        row = result["checks"][0]
        self.assertNotEqual(row["exit_code"], 0)
        self.assertFalse(row["sanitizer_capture"]["complete"])

    def test_interrupt_does_not_publish_a_completed_run(self):
        with patch("run_validation.run_owned_command", side_effect=KeyboardInterrupt):
            with self.assertRaises(KeyboardInterrupt):
                self.execute([self.check()])
        self.assertFalse((self.repo / "raw/one/run.json").exists())

    def test_failed_check_does_not_hide_later_required_checks(self):
        result = self.execute([
            self.check("first", "raise SystemExit(3)"),
            self.check("second", "print('second really ran')"),
        ])
        self.assertEqual([c["exit_code"] for c in result["checks"]], [3, 0])

    def test_timeout_is_recorded_as_failure_with_partial_output(self):
        # The process manager has real timeout/descendant integration tests.
        # Inject its boundary here so this assertion is independent of Python startup speed.
        def timeout(argv, cwd, stdout, stderr, seconds, *, env=None):
            stdout.write(b"began\n")
            stdout.flush()
            raise subprocess.TimeoutExpired(argv, seconds)
        with patch("run_validation.run_owned_command", side_effect=timeout):
            result = self.execute([self.check(timeout_seconds=0.1)])
        check = result["checks"][0]
        self.assertNotEqual(check["exit_code"], 0)
        self.assertEqual(check["error"], "timeout")
        self.assertTrue(check["sanitizer_capture"]["complete"])
        self.assertIn("began", (self.repo / "raw/one" / check["stdout"]["path"]).read_text())

    def test_refuses_to_overwrite_existing_run(self):
        self.execute([self.check()])
        previous = (self.repo / "raw/one/run.json").read_bytes()
        with self.assertRaises(FileExistsError):
            self.execute([self.check(code="print('replacement')")])
        self.assertEqual((self.repo / "raw/one/run.json").read_bytes(), previous)

    def test_arguments_are_not_interpreted_as_shell_code(self):
        marker = self.repo / "injected.txt"
        payload = f"literal & echo injected > {marker}"
        result = self.execute([self.check(
            command=["{python}", "-c", "import sys; print(sys.argv[1])", payload],
        )])
        self.assertEqual(result["checks"][0]["exit_code"], 0)
        self.assertFalse(marker.exists())
        output = self.repo / "raw/one" / result["checks"][0]["stdout"]["path"]
        self.assertIn(payload, output.read_text(encoding="utf-8"))

    def test_missing_required_binary_is_recorded_and_not_run(self):
        result = self.execute([self.check(binary="{repo}/missing-program.exe")])
        self.assertNotEqual(result["checks"][0]["exit_code"], 0)
        self.assertEqual(result["checks"][0]["error"], "missing_binary")

    def test_duplicate_check_ids_and_unknown_variables_are_rejected(self):
        for checks in [
            [self.check("same"), self.check("same")],
            [self.check(command=["{unknown_program}"])],
        ]:
            with self.subTest(checks=checks), self.assertRaises(ValueError):
                self.execute(checks)

    def test_fixture_fingerprint_covers_names_content_and_missing_files(self):
        before = fingerprint_paths(self.repo, ["fixture.txt"])
        (self.repo / "fixture.txt").write_text("fixture v2\n", encoding="utf-8")
        self.assertNotEqual(before, fingerprint_paths(self.repo, ["fixture.txt"]))
        with self.assertRaises(FileNotFoundError):
            fingerprint_paths(self.repo, ["missing.fixture"])

    def fixture_root_aliases(self):
        root = self.repo / "fixture-root"
        root.mkdir()
        (root / "fixture.txt").write_text("fixture alias contents\n", encoding="utf-8")
        (root / "alias-segment").mkdir()
        # Parent traversal is a real root alias on every supported host and
        # needs no Windows symlink privilege. POSIX also tests a root symlink,
        # matching the macOS /var -> /private/var temporary-directory case.
        aliases = [root / "alias-segment" / ".."]
        if sys.platform != "win32":
            linked = self.repo / "fixture-root-link"
            linked.symlink_to(root, target_is_directory=True)
            aliases.append(linked)
        return root, aliases

    def test_fixture_fingerprint_accepts_equivalent_root_aliases(self):
        root, aliases = self.fixture_root_aliases()
        expected = fingerprint_paths(root.resolve(), ["fixture.txt"])
        for alias in aliases:
            with self.subTest(alias=alias):
                self.assertNotEqual(alias, alias.resolve())
                self.assertEqual(alias.resolve(), root.resolve())
                self.assertEqual(fingerprint_paths(alias, ["fixture.txt"]), expected)

    def test_fixture_fingerprint_alias_root_still_rejects_outside_targets(self):
        root, aliases = self.fixture_root_aliases()
        outside = self.repo / "outside.fixture"
        outside.write_text("outside the fixture root\n", encoding="utf-8")
        candidates = ["../outside.fixture", str(outside.resolve())]
        if sys.platform != "win32":
            (root / "outside-link.fixture").symlink_to(outside)
            candidates.append("outside-link.fixture")
        for alias in aliases:
            for candidate in candidates:
                with self.subTest(alias=alias, candidate=candidate):
                    with self.assertRaisesRegex(ValueError, "Fixture escapes repository"):
                        fingerprint_paths(alias, [candidate])

    def test_marks_source_changes_during_execution(self):
        code = "from pathlib import Path; Path('fixture.txt').write_text('changed during test')"
        result = self.execute([self.check(code=code)])
        self.assertTrue(result["source_changed_during_run"])
        self.assertNotEqual(result["worktree_fingerprint"], result["finished_worktree_fingerprint"])

    def write_cache(self, source=None, configuration="Debug"):
        build = self.repo / "build"
        build.mkdir()
        (build / "CMakeCache.txt").write_text(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={source or self.repo}\n"
            "CMAKE_GENERATOR:INTERNAL=Ninja\n"
            f"CMAKE_BUILD_TYPE:STRING={configuration}\n", encoding="utf-8",
        )

    def test_rejects_foreign_host_and_configuration(self):
        with self.assertRaises(ValueError):
            self.execute([self.check()], platform="another-platform")
        with self.assertRaises(ValueError):
            self.execute([self.check()], configuration="Release")

    def test_rejects_build_directory_from_another_checkout(self):
        self.write_cache(source=self.repo / "another-checkout")
        with self.assertRaises(ValueError):
            self.execute([self.check()], require_cmake_cache=True)

    def test_rejects_debug_single_config_build_labeled_release(self):
        self.write_cache(configuration="Debug")
        self.write_profile([self.check()], configuration="Release", require_cmake_cache=True)
        with self.assertRaises(ValueError):
            run_profile(repo=self.repo, profile_file=self.profile_file, profile_name="fixture",
                        build_dir=self.repo / "build", configuration="Release",
                        run_dir=self.repo / "raw/one", purpose="test-fixture")

    def test_rejects_build_without_required_validation_prerequisites(self):
        self.write_cache()
        with self.assertRaises(ValueError):
            self.execute([self.check()], require_cmake_cache=True,
                         expected_cache={"CAESURA_REQUIRE_TEST_PREREQUISITES": "ON"})

    def test_binary_replacement_cannot_relabel_the_executed_bytes(self):
        binary = self.repo / "binary.fixture"
        binary.write_bytes(b"before")
        expected = hashlib.sha256(binary.read_bytes()).hexdigest()
        result = self.execute([self.check(
            code="from pathlib import Path; Path('binary.fixture').write_bytes(b'after')",
            binary="{repo}/binary.fixture",
        )])
        check = result["checks"][0]
        self.assertEqual(check["binary"]["sha256"], expected)
        self.assertEqual(check["error"], "binary_changed")
        self.assertNotEqual(check["exit_code"], 0)

    def test_ignored_fixture_changes_are_reported_separately(self):
        ignored = self.repo / "ignored"
        ignored.mkdir()
        (ignored / "fixture.txt").write_text("before", encoding="utf-8")
        (self.repo / ".gitignore").write_text("raw/\nignored/\n", encoding="utf-8")
        result = self.execute([self.check(
            code="from pathlib import Path; Path('ignored/fixture.txt').write_text('after')",
        )], fixture_paths=["ignored"])
        self.assertTrue(result["fixtures_changed_during_run"])
        self.assertFalse(result["source_changed_during_run"])


if __name__ == "__main__":
    unittest.main()
