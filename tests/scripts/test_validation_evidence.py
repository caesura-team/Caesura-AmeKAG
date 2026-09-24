"""Validation evidence contracts. All fabricated inputs stay in test-fixture sandboxes."""
from __future__ import annotations

import copy
import contextlib
import hashlib
import io
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parents[2] / "scripts"
sys.path.insert(0, str(SCRIPTS))

from collect_validation_evidence import EvidenceError, collect_evidence, parse_report
from verify_release_candidate import verify_evidence


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class EvidenceFixture:
    """Explicit test data; even an internally valid fixture cannot pass release verification."""

    def __init__(self, root: Path):
        self.root = root
        self.raw = root / "raw"
        self.raw.mkdir()
        self.profile_path = root / "profiles.json"
        self.run_path = self.raw / "run.json"
        self.profile = {
            "schema_version": 1,
            "profiles": {"test-debug": {
                "platform": "test", "configuration": "Debug",
                "checks": [{"id": "cpp", "parser": "doctest", "required": True,
                            "min_discovered": 2, "allowed_skips": []}],
            }},
        }
        self.write_profile()
        self.run = {
            "schema_version": 1,
            "source_sha": "a" * 40, "dirty": False,
            "worktree_fingerprint": "b" * 64, "fixture_sha256": "c" * 64,
            "finished_worktree_fingerprint": "b" * 64, "source_changed_during_run": False,
            "finished_fixture_sha256": "c" * 64, "fixtures_changed_during_run": False,
            "profile_name": "test-debug",
            "run_id": "fixture-001", "run_attempt": 1,
            "repository": "test/fixture", "workflow": "fixture-workflow",
            "platform": "test", "configuration": "Debug",
            "started_at": "2026-09-05T01:00:00Z", "finished_at": "2026-09-05T01:00:02Z",
            "toolchain": {"python": sys.version.split()[0]}, "profile_variables": {},
            "profile_sha256": sha(self.profile_path), "purpose": "test-fixture",
            "checks": [{
                "id": "cpp", "command": ["fixture-runner", "--counted"], "cwd": str(root),
                "started_at": "2026-09-05T01:00:00Z", "finished_at": "2026-09-05T01:00:01Z",
                "exit_code": 0,
                "stdout": self.file("cpp.stdout", self.doctest()),
                "stderr": self.file("cpp.stderr", ""),
                "binary": self.file("runner.bin", "EXPLICIT TEST FIXTURE; NOT A RELEASE BINARY"),
                "executed_program": self.file("program.bin", "EXPLICIT TEST EXECUTABLE FIXTURE"),
            }],
        }
        self.output = root / "validation" / self.run["source_sha"] / self.run["run_id"] / "test-debug"
        self.write_run()

    @staticmethod
    def doctest(passed: int = 2, failed: int = 0, skipped: int = 0) -> str:
        total = passed + failed
        return (f"[doctest] test cases: {total} | {passed} passed | {failed} failed | {skipped} skipped\n"
                "[doctest] assertions: 4 | 4 passed | 0 failed |\n")

    def file(self, name: str, text: str) -> dict:
        path = self.raw / name
        path.write_text(text, encoding="utf-8")
        return {"path": name, "sha256": sha(path)}

    def write_profile(self):
        self.profile_path.write_text(json.dumps(self.profile), encoding="utf-8")

    def write_run(self):
        self.run_path.write_text(json.dumps(self.run), encoding="utf-8")

    def collect(self):
        self.write_run()
        return collect_evidence(self.profile_path, "test-debug", self.run_path, self.output)

    def verify(self, release=False):
        return verify_evidence(self.output, self.profile_path, "test-debug", self.run_path,
                               source_sha="a" * 40, release=release)


class ValidationEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="caesura-evidence-test-")
        self.addCleanup(self.tmp.cleanup)
        self.f = EvidenceFixture(Path(self.tmp.name))

    def test_complete_fixture_can_be_diagnosed_but_never_released(self):
        result = self.f.collect()
        self.assertEqual(result["result"], "PASS")
        self.assertEqual(result["checks"][0]["counts"]["passed"], 2)
        self.assertEqual(self.f.verify(), [])
        self.assertTrue(any("test-fixture" in error for error in self.f.verify(release=True)))

    def test_fake_pass_with_nonzero_exit_is_failed(self):
        self.f.run["checks"][0]["exit_code"] = 1
        self.assertEqual(self.f.collect()["result"], "FAIL")
        self.assertTrue(self.f.verify())

    def test_failed_test_cannot_be_hidden_by_zero_exit(self):
        self.f.run["checks"][0]["stdout"] = self.f.file("cpp.stdout", self.f.doctest(1, 1))
        self.assertEqual(self.f.collect()["result"], "FAIL")

    def test_zero_tests_rejected(self):
        self.f.run["checks"][0]["stdout"] = self.f.file("cpp.stdout", self.f.doctest(0))
        self.assertEqual(self.f.collect()["result"], "FAIL")

    def test_missing_summary_is_not_a_pass(self):
        self.f.run["checks"][0]["stdout"] = self.f.file("cpp.stdout", "PASS\n")
        self.assertEqual(self.f.collect()["result"], "FAIL")

    def test_missing_stdout_is_input_error(self):
        (self.f.raw / "cpp.stdout").unlink()
        with self.assertRaises(EvidenceError):
            self.f.collect()

    def test_empty_stderr_is_preserved(self):
        result = self.f.collect()
        path = self.f.output / result["checks"][0]["files"]["stderr"]["path"]
        self.assertEqual(path.read_bytes(), b"")

    def test_digest_mismatch_rejected_at_collection(self):
        self.f.run["checks"][0]["binary"]["sha256"] = "d" * 64
        with self.assertRaises(EvidenceError):
            self.f.collect()

    def test_executed_program_is_collected_and_verified(self):
        manifest = self.f.collect()
        reference = manifest["checks"][0]["files"]["executed_program"]
        program = self.f.output / reference["path"]
        self.assertEqual(sha(program), reference["sha256"])
        program.write_text("changed executable", encoding="utf-8")
        self.assertTrue(self.f.verify())

    def test_success_without_executed_program_is_rejected(self):
        del self.f.run["checks"][0]["executed_program"]
        with self.assertRaises(EvidenceError):
            self.f.collect()

    def test_executed_program_digest_mismatch_is_rejected(self):
        self.f.run["checks"][0]["executed_program"]["sha256"] = "d" * 64
        with self.assertRaises(EvidenceError):
            self.f.collect()

    def test_report_bytes_are_authenticated_before_they_are_parsed(self):
        from collect_validation_evidence import build_manifest
        log = self.f.raw / "cpp.stdout"
        original_open = Path.open
        reads = []

        def replace_on_second_read(path, mode="r", *args, **kwargs):
            if path == log and "r" in mode:
                reads.append(mode)
                if len(reads) == 2:
                    with original_open(path, "wb") as stream:
                        stream.write(self.f.doctest(9000).encode("utf-8"))
            return original_open(path, mode, *args, **kwargs)

        with patch.object(Path, "open", replace_on_second_read):
            manifest, _ = build_manifest(self.f.profile_path, "test-debug", self.f.run_path)
        self.assertEqual(manifest["checks"][0]["counts"]["passed"], 2)
        self.assertEqual(len(reads), 1)

    def test_report_read_is_bounded(self):
        self.f.run["checks"][0]["stdout"] = self.f.file("cpp.stdout", "x" * 8193)
        with patch("collect_validation_evidence.MAX_REPORT_BYTES", 8192):
            with self.assertRaises(EvidenceError):
                self.f.collect()

    def test_expected_unittest_failure_fails_required_gate(self):
        self.f.profile["profiles"]["test-debug"]["checks"][0]["parser"] = "unittest"
        self.f.write_profile()
        self.f.run["profile_sha256"] = sha(self.f.profile_path)
        self.f.run["checks"][0]["stdout"] = self.f.file(
            "cpp.stdout", "Ran 2 tests in 0.03s\n\nOK (expected failures=1)\n")
        manifest = self.f.collect()
        self.assertEqual(manifest["result"], "FAIL")
        self.assertEqual(manifest["checks"][0]["counts"]["passed"], 1)
        self.assertTrue(self.f.verify())

    def test_duplicate_run_does_not_overwrite_first(self):
        self.f.collect()
        before = (self.f.output / "manifest.json").read_bytes()
        with self.assertRaises(EvidenceError):
            self.f.collect()
        self.assertEqual((self.f.output / "manifest.json").read_bytes(), before)

    def test_unknown_check_rejected(self):
        self.f.run["checks"].append({**self.f.run["checks"][0], "id": "unknown"})
        with self.assertRaises(EvidenceError):
            self.f.collect()

    def test_duplicate_check_rejected(self):
        self.f.run["checks"].append(copy.deepcopy(self.f.run["checks"][0]))
        with self.assertRaises(EvidenceError):
            self.f.collect()

    def test_missing_required_check_is_not_run(self):
        self.f.run["checks"] = []
        result = self.f.collect()
        self.assertEqual(result["result"], "FAIL")
        self.assertEqual(result["checks"][0]["result"], "NOT_RUN")

    def test_wrong_profile_digest_rejected(self):
        self.f.run["profile_sha256"] = "f" * 64
        with self.assertRaises(EvidenceError):
            self.f.collect()

    def test_profile_commands_bound_to_executed_argv(self):
        check = self.f.profile["profiles"]["test-debug"]["checks"][0]
        check["command"] = ["{repo}/runner", "--all"]
        self.f.write_profile()
        self.f.run["profile_sha256"] = sha(self.f.profile_path)
        self.f.run["profile_variables"] = {"repo": str(self.f.root)}
        with self.assertRaises(EvidenceError):
            self.f.collect()

    def test_wrong_workflow_receipt_rejected_after_collection(self):
        self.f.collect()
        self.f.run["workflow"] = "untrusted-workflow"
        self.f.write_run()
        self.assertTrue(self.f.verify())

    def test_wrong_run_attempt_receipt_rejected_after_collection(self):
        self.f.collect()
        self.f.run["run_attempt"] = 2
        self.f.write_run()
        self.assertTrue(self.f.verify())

    def test_wrong_source_receipt_rejected_after_collection(self):
        self.f.collect()
        self.f.run["source_sha"] = "d" * 40
        self.f.write_run()
        self.assertTrue(self.f.verify())

    def test_tampered_manifest_counts_rejected(self):
        manifest = self.f.collect()
        manifest["checks"][0]["counts"]["passed"] = 9000
        (self.f.output / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        self.assertTrue(self.f.verify())

    def test_tampered_log_rejected(self):
        manifest = self.f.collect()
        log = self.f.output / manifest["checks"][0]["files"]["stdout"]["path"]
        log.write_text(self.f.doctest(9000), encoding="utf-8")
        self.assertTrue(self.f.verify())

    def test_dirty_validation_can_be_diagnosed_but_not_released(self):
        self.f.run["purpose"] = "validation"
        self.f.run["dirty"] = True
        self.f.collect()
        self.assertEqual(self.f.verify(), [])
        self.assertTrue(any("dirty" in error for error in self.f.verify(release=True)))

    def test_cli_collects_then_verifies_without_auto_go(self):
        result = subprocess.run([
            sys.executable, str(SCRIPTS / "verify_release_candidate.py"), "--generate-bundle",
            "--profile", str(self.f.profile_path), "--profile-name", "test-debug",
            "--run", str(self.f.run_path), "--expected-run", str(self.f.run_path),
            "--artifacts-dir", str(self.f.output), "--commit", "a" * 40, "--diagnostic",
        ], capture_output=True, text=True, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("RC-GO", result.stdout)
        self.assertTrue((self.f.output / "manifest.json").exists())

    def test_real_executor_collect_verify_roundtrip_in_fixture_sandbox(self):
        from run_validation import run_profile
        child = self.f.root / "child_test.py"
        child.write_text("import unittest\nclass Check(unittest.TestCase):\n def test_real(self): self.assertEqual(2+2,4)\nunittest.main()\n", encoding="utf-8")
        profile = self.f.profile["profiles"]["test-debug"]
        profile["platform"] = {"Windows": "windows", "Linux": "linux", "Darwin": "macos"}[platform.system()]
        profile["fixture_paths"] = ["child_test.py"]
        profile["checks"] = [{"id": "child", "parser": "unittest", "required": True,
                               "min_discovered": 1, "allowed_skips": [],
                               "command": ["{python}", "{repo}/child_test.py"],
                               "cwd": "{repo}", "binary": "{python}"}]
        self.f.write_profile()
        identity = {"source_sha": "a" * 40, "dirty": False, "worktree_fingerprint": "b" * 64}
        raw = self.f.root / "actual-execution"
        with patch("run_validation._source_identity", return_value=identity):
            receipt = run_profile(repo=self.f.root, profile_file=self.f.profile_path,
                                  profile_name="test-debug", build_dir=self.f.root / "build",
                                  configuration="Debug", run_dir=raw, purpose="test-fixture")
        output = self.f.root / "collected" / receipt["source_sha"] / receipt["run_id"] / "test-debug"
        manifest = collect_evidence(self.f.profile_path, "test-debug", raw / "run.json", output)
        self.assertEqual(manifest["checks"][0]["counts"]["passed"], 1)
        self.assertEqual(verify_evidence(output, self.f.profile_path, "test-debug", raw / "run.json",
                                         source_sha="a" * 40, release=False), [])
        self.assertTrue(verify_evidence(output, self.f.profile_path, "test-debug", raw / "run.json",
                                       source_sha="a" * 40, release=True))

    def test_profile_binary_binding_is_portable_between_verifier_hosts(self):
        from collect_validation_evidence import validate_check
        spec = {"id": "cpp", "binary": "{repo}/build/Debug/tests.exe", "cwd": "{repo}"}
        run = {**self.f.run, "profile_variables": {"repo": "/opt/ci/source"}}
        check = {**self.f.run["checks"][0], "cwd": "/opt/ci/source",
                 "binary": {"path": "/opt/ci/source/build/Debug/tests.exe", "sha256": "a" * 64}}
        validate_check(check, spec, run)

    def test_exit_code_check_can_have_empty_output(self):
        self.f.profile["profiles"]["test-debug"]["checks"][0].update(parser="exit-code", min_discovered=0)
        self.f.write_profile()
        self.f.run["profile_sha256"] = sha(self.f.profile_path)
        self.f.run["checks"][0]["stdout"] = self.f.file("cpp.stdout", "")
        manifest = self.f.collect()
        self.assertEqual(manifest["result"], "PASS")
        self.assertIsNone(manifest["checks"][0]["counts"])


class SanitizerEvidenceTests(unittest.TestCase):
    # Representative runtime report bytes are explicit fixture inputs. These
    # exercise the real collector and verifier, not an engine or runtime probe.
    DIAGNOSTICS = {
        "asan": (
            "==123==ERROR: AddressSanitizer: heap-use-after-free on address 0x602000000010\n"
            "READ of size 4 at 0x602000000010 thread T0\n"
        ),
        "ubsan": (
            "/tmp/sanitizer-control.cpp:3:12: runtime error: signed integer overflow: "
            "2147483647 + 1 cannot be represented in type 'int'\n"
        ),
        "tsan": (
            "WARNING: ThreadSanitizer: data race (pid=123)\n"
            "  Write of size 4 at 0x7fffffffe010 by thread T1:\n"
        ),
    }
    HARMLESS = (
        "configure: -fsanitize=address,undefined -fno-sanitize-recover=undefined\n"
        "AddressSanitizer: enabled for this validation configuration\n"
        "Tools: UndefinedBehaviorSanitizer and ThreadSanitizer are available\n"
        "Documentation: runtime error diagnostics make validation fail\n"
        "No AddressSanitizer errors detected\n"
        "WARNING: ThreadSanitizer instrumentation increases memory use\n"
    )

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="caesura-sanitizer-evidence-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def fixture(self, name, parser="doctest"):
        root = self.root / name
        root.mkdir()
        fixture = EvidenceFixture(root)
        spec = fixture.profile["profiles"]["test-debug"]["checks"][0]
        spec.update(parser=parser, min_discovered=0 if parser == "exit-code" else 2)
        fixture.write_profile()
        fixture.run["profile_sha256"] = sha(fixture.profile_path)
        summaries = {
            "doctest": fixture.doctest(),
            "lua": "Results: 2 passed, 0 failed, 2 total\n",
            "unittest": "Ran 2 tests in 0.01s\n\nOK\n",
            "exit-code": "",
            "ctest-junit": "100% tests passed, 0 tests failed out of 2\n",
        }
        fixture.run["checks"][0]["stdout"] = fixture.file("cpp.stdout", summaries[parser])
        return fixture

    def add_ctest_report(self, fixture, role, text, encoded=False):
        suite = ET.Element("testsuite", tests="2", failures="0", errors="0", skipped="0")
        first = ET.SubElement(suite, "testcase", name="native-child", status="run")
        ET.SubElement(first, role).text = text
        ET.SubElement(suite, "testcase", name="positive-control", status="run")
        report = ET.tostring(suite, encoding="unicode")
        if encoded:
            # XML report diagnostics must be inspected after entity decoding.
            report = report.replace("Sanitizer", "Saniti&#122;er")
            report = report.replace("runtime error:", "runtime&#32;error:")
        fixture.run["checks"][0]["report"] = fixture.file("ctest.xml", report)

    def assert_diagnostic_rejected(self, fixture, exit_code=0):
        manifest = fixture.collect()
        check = manifest["checks"][0]
        self.assertEqual(manifest["result"], "FAIL")
        self.assertEqual(check["result"], "FAIL")
        self.assertEqual(check["exit_code"], exit_code)
        self.assertTrue(any("sanitizer" in reason.lower() for reason in check["reasons"]))
        if check["counts"] is not None:
            # A runtime diagnostic is a gate failure, not an invented failed test.
            self.assertEqual(check["counts"], {
                "discovered": 2, "executed": 2, "passed": 2, "failed": 0, "skipped": 0,
            })
        # release=False avoids the unrelated test-fixture publication refusal.
        self.assertTrue(any("sanitizer" in error.lower() for error in fixture.verify()))
        return manifest

    def test_sanitizer_diagnostics_fail_green_process_summaries(self):
        for parser in ("doctest", "lua", "unittest", "exit-code"):
            for sanitizer, diagnostic in self.DIAGNOSTICS.items():
                for role in ("stdout", "stderr"):
                    with self.subTest(parser=parser, sanitizer=sanitizer, role=role):
                        fixture = self.fixture(f"{parser}-{sanitizer}-{role}", parser)
                        check = fixture.run["checks"][0]
                        path = fixture.raw / check[role]["path"]
                        check[role] = fixture.file(path.name, path.read_text() + diagnostic)
                        self.assert_diagnostic_rejected(fixture)

    def test_sanitizer_diagnostics_fail_passing_ctest_output(self):
        for sanitizer, diagnostic in self.DIAGNOSTICS.items():
            for role in ("system-out", "system-err"):
                for encoded in (False, True):
                    with self.subTest(sanitizer=sanitizer, role=role, encoded=encoded):
                        fixture = self.fixture(f"{sanitizer}-{role}-{encoded}", "ctest-junit")
                        self.add_ctest_report(fixture, role, diagnostic, encoded)
                        self.assert_diagnostic_rejected(fixture)

    def test_colored_ubsan_diagnostic_without_summary_fails_clean_exit(self):
        # Explicit test-fixture from Clang 21 color=always:print_summary=0 output
        # (actual exit 0). Only the machine-specific source path is replaced;
        # the SGR bytes and runtime-error layout are retained verbatim.
        # Source stderr SHA-256:
        # c6ba668ae287de753d32ddc0915669317d14ea416ea43335b16db2c3928fb01e
        diagnostic = (
            "\x1b[1mfixture.cpp:3:75:\x1b[1m\x1b[31m runtime error: "
            "\x1b[1m\x1b[0m\x1b[1msigned integer overflow: "
            "2147483647 + 1 cannot be represented in type 'int'\x1b[1m\x1b[0m\n"
        )
        for role in ("stdout", "stderr", "sidecar"):
            with self.subTest(role=role):
                fixture = self.fixture(f"colored-ubsan-{role}")
                check = fixture.run["checks"][0]
                if role == "sidecar":
                    fixture.profile["profiles"]["test-debug"]["sanitizer_capture"] = {
                        "version": 1, "required": True,
                    }
                    fixture.write_profile()
                    fixture.run["profile_sha256"] = sha(fixture.profile_path)
                    directory = "sanitizer/cpp"
                    (fixture.raw / directory).mkdir(parents=True)
                    path = fixture.raw / directory / "sanitizer.123"
                    path.write_bytes(diagnostic.encode("utf-8"))
                    check["sanitizer_capture"] = {
                        "version": 1, "complete": True, "directory": directory,
                        "prefix": "sanitizer", "options_contract": "llvm-common-log-path-v1",
                        "files": [{"path": f"{directory}/{path.name}",
                                   "sha256": sha(path), "size_bytes": path.stat().st_size}],
                    }
                else:
                    path = fixture.raw / check[role]["path"]
                    check[role] = fixture.file(path.name, path.read_text() + diagnostic)
                manifest = self.assert_diagnostic_rejected(fixture)
                retained = (manifest["checks"][0]["sanitizer_capture"]["files"][0]
                            if role == "sidecar" else manifest["checks"][0]["files"][role])
                # Classification normalization must not rewrite authenticated logs.
                self.assertEqual((fixture.output / retained["path"]).read_bytes(), path.read_bytes())

    def test_sanitizer_configuration_and_prose_are_not_diagnostics(self):
        for parser in ("doctest", "lua", "unittest", "exit-code"):
            with self.subTest(parser=parser):
                fixture = self.fixture(parser, parser)
                check = fixture.run["checks"][0]
                stdout = fixture.raw / check["stdout"]["path"]
                check["stdout"] = fixture.file(stdout.name, self.HARMLESS + stdout.read_text())
                check["stderr"] = fixture.file("cpp.stderr", self.HARMLESS)
                self.assertEqual(fixture.collect()["result"], "PASS")
                self.assertEqual(fixture.verify(), [])

    def test_sanitizer_ctest_configuration_and_prose_are_not_diagnostics(self):
        for role in ("system-out", "system-err"):
            with self.subTest(role=role):
                fixture = self.fixture(role, "ctest-junit")
                self.add_ctest_report(fixture, role, self.HARMLESS)
                self.assertEqual(fixture.collect()["result"], "PASS")
                self.assertEqual(fixture.verify(), [])

    def test_sanitizer_diagnostic_cannot_be_relabelled_pass_in_manifest(self):
        fixture = self.fixture("tampered-pass")
        fixture.run["checks"][0]["stderr"] = fixture.file("cpp.stderr", self.DIAGNOSTICS["asan"])
        manifest = self.assert_diagnostic_rejected(fixture)
        manifest["result"] = "PASS"
        manifest["checks"][0].update(result="PASS", reasons=[])
        (fixture.output / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        self.assertTrue(any("differs from actual reports" in error for error in fixture.verify()))

    def test_sanitizer_diagnostics_take_priority_over_exit_77(self):
        for sanitizer, diagnostic in self.DIAGNOSTICS.items():
            with self.subTest(sanitizer=sanitizer):
                fixture = self.fixture(sanitizer)
                fixture.run["checks"][0].update(
                    exit_code=77, stderr=fixture.file("cpp.stderr", diagnostic),
                )
                self.assert_diagnostic_rejected(fixture, exit_code=77)

    def test_exit_77_without_sanitizer_diagnostic_preserves_skip(self):
        fixture = self.fixture("ordinary-skip")
        fixture.run["checks"][0].update(
            exit_code=77, stderr=fixture.file("cpp.stderr", "Optional service is unavailable\n"),
        )
        manifest = fixture.collect()
        check = manifest["checks"][0]
        self.assertEqual(check["result"], "SKIP")
        self.assertEqual(check["exit_code"], 77)
        self.assertEqual(check["counts"]["failed"], 0)
        self.assertFalse(any("sanitizer" in reason.lower() for reason in check["reasons"]))
        # This fixture's check is required: preserving SKIP must not turn it PASS.
        self.assertEqual(manifest["result"], "FAIL")
        self.assertTrue(any("Required check cpp: SKIP" in error for error in fixture.verify()))


class SanitizerCaptureEvidenceTests(unittest.TestCase):
    """Transport contracts using real temporary files and the public evidence APIs."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="caesura-sanitizer-capture-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def fixture(self, name):
        root = self.root / name
        root.mkdir()
        fixture = EvidenceFixture(root)
        fixture.profile["profiles"]["test-debug"]["sanitizer_capture"] = {
            "version": 1, "required": True,
        }
        fixture.write_profile()
        fixture.run["profile_sha256"] = sha(fixture.profile_path)
        self.add_capture(fixture, fixture.run["checks"][0])
        return fixture

    def add_capture(self, fixture, check):
        directory = f"sanitizer/{check['id']}"
        (fixture.raw / directory).mkdir(parents=True)
        check["sanitizer_capture"] = {
            "version": 1, "complete": True, "directory": directory,
            "prefix": "sanitizer", "options_contract": "llvm-common-log-path-v1", "files": [],
        }

    def sidecar(self, fixture, text="retained control output\n", pid=123, check_id="cpp"):
        check = next(row for row in fixture.run["checks"] if row["id"] == check_id)
        name = f"sanitizer/{check_id}/sanitizer.{pid}"
        path = fixture.raw / name
        data = text.encode("utf-8")
        path.write_bytes(data)
        check["sanitizer_capture"]["files"].append({
            "path": name, "sha256": sha(path), "size_bytes": len(data),
        })
        return path

    def collect_with_capture(self, fixture):
        manifest = fixture.collect()
        for check in manifest["checks"]:
            self.assertIn("sanitizer_capture", check)
            self.assertTrue((fixture.output / "inputs" / check["id"] / "sanitizer").is_dir())
        return manifest

    def assert_bundle_rejected(self, fixture):
        # Exercise reconstruction directly as well as the verifier. Otherwise a
        # changed external receipt's digest could mask an ignored capture field.
        from collect_validation_evidence import build_manifest
        fixture.write_run()
        with self.assertRaises(EvidenceError):
            build_manifest(fixture.profile_path, "test-debug", fixture.run_path,
                           collected_root=fixture.output)
        self.assertTrue(fixture.verify())

    def assert_rejected(self, fixture, scope):
        if scope == "raw":
            with self.assertRaises(EvidenceError):
                fixture.collect()
        else:
            self.assert_bundle_rejected(fixture)

    def add_second_check(self, fixture):
        spec = copy.deepcopy(fixture.profile["profiles"]["test-debug"]["checks"][0])
        spec["id"] = "second"
        fixture.profile["profiles"]["test-debug"]["checks"].append(spec)
        fixture.write_profile()
        fixture.run["profile_sha256"] = sha(fixture.profile_path)
        check = copy.deepcopy(fixture.run["checks"][0])
        check["id"] = "second"
        fixture.run["checks"].append(check)
        self.add_capture(fixture, check)

    def redirect_directory(self, directory, target):
        directory.rename(target)
        if sys.platform == "win32":
            # Directory junction creation does not need symlink privilege.
            # Both endpoints are owned by this test's TemporaryDirectory.
            subprocess.run(
                ["cmd", "/d", "/c", "mklink", "/J", str(directory), str(target)],
                check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            )
        else:
            directory.symlink_to(target, target_is_directory=True)

    def test_empty_required_capture_roundtrips_without_fabricated_logs(self):
        fixture = self.fixture("empty")
        manifest = self.collect_with_capture(fixture)
        capture = manifest["checks"][0]["sanitizer_capture"]
        self.assertEqual(capture["files"], [])
        self.assertEqual(capture["directory"], "inputs/cpp/sanitizer")
        self.assertEqual(list((fixture.output / capture["directory"]).iterdir()), [])
        self.assertEqual(manifest["result"], "PASS")
        self.assertEqual(fixture.verify(), [])

    def test_sidecar_bytes_and_names_survive_collection(self):
        fixture = self.fixture("retained-bytes")
        originals = [self.sidecar(fixture, "first control\n", 123),
                     self.sidecar(fixture, "second control\n", 456)]
        manifest = self.collect_with_capture(fixture)
        capture = manifest["checks"][0]["sanitizer_capture"]
        self.assertEqual(capture["version"], 1)
        self.assertIs(capture["complete"], True)
        self.assertEqual(capture["prefix"], "sanitizer")
        self.assertEqual(capture["options_contract"], "llvm-common-log-path-v1")
        expected = {f"inputs/cpp/sanitizer/{path.name}": path for path in originals}
        self.assertEqual({entry["path"] for entry in capture["files"]}, set(expected))
        for entry in capture["files"]:
            original = expected[entry["path"]]
            copied = fixture.output / entry["path"]
            self.assertEqual(copied.read_bytes(), original.read_bytes())
            self.assertEqual(entry["sha256"], sha(original))
            self.assertEqual(entry["size_bytes"], len(original.read_bytes()))
        self.assertEqual(manifest["result"], "PASS")
        self.assertEqual(fixture.verify(), [])

    def test_sidecar_diagnostics_fail_even_when_outer_logs_are_clean(self):
        for sanitizer, diagnostic in SanitizerEvidenceTests.DIAGNOSTICS.items():
            for exit_code in (0, 77):
                with self.subTest(sanitizer=sanitizer, exit_code=exit_code):
                    fixture = self.fixture(f"{sanitizer}-{exit_code}")
                    self.sidecar(fixture, diagnostic)
                    fixture.run["checks"][0]["exit_code"] = exit_code
                    manifest = self.collect_with_capture(fixture)
                    check = manifest["checks"][0]
                    self.assertEqual(check["result"], "FAIL")
                    self.assertEqual(manifest["result"], "FAIL")
                    self.assertEqual(check["exit_code"], exit_code)
                    self.assertEqual(check["counts"]["passed"], 2)
                    self.assertEqual(check["counts"]["failed"], 0)
                    self.assertTrue(any("sanitizer" in reason.lower() for reason in check["reasons"]))
                    self.assertTrue(any("sanitizer" in error.lower() for error in fixture.verify()))

    def test_required_capture_fields_cannot_be_deleted_or_weakened(self):
        mutations = [
            ("capture", None), ("version", None), ("complete", None), ("directory", None),
            ("prefix", None), ("options_contract", None), ("files", None),
            ("version", 2), ("complete", False), ("prefix", "alternate"),
            ("options_contract", "unknown-options"),
        ]
        for scope in ("raw", "bundle"):
            for number, (field, value) in enumerate(mutations):
                with self.subTest(scope=scope, field=field, value=value):
                    fixture = self.fixture(f"{scope}-{number}")
                    if scope == "bundle":
                        self.collect_with_capture(fixture)
                    check = fixture.run["checks"][0]
                    if field == "capture":
                        del check["sanitizer_capture"]
                    elif value is None:
                        del check["sanitizer_capture"][field]
                    else:
                        check["sanitizer_capture"][field] = value
                    self.assert_rejected(fixture, scope)

    def test_capture_profile_requires_explicit_supported_policy(self):
        for scope in ("raw", "bundle"):
            for number, policy in enumerate((
                {"version": 1}, {"required": True}, {"version": 2, "required": True},
                {"version": 1, "required": "true"},
            )):
                with self.subTest(scope=scope, policy=policy):
                    fixture = self.fixture(f"{scope}-{number}")
                    if scope == "bundle":
                        self.collect_with_capture(fixture)
                    fixture.profile["profiles"]["test-debug"]["sanitizer_capture"] = policy
                    fixture.write_profile()
                    fixture.run["profile_sha256"] = sha(fixture.profile_path)
                    self.assert_rejected(fixture, scope)

    def test_capture_file_metadata_is_complete_unique_and_matches_bytes(self):
        mutations = ("duplicate", "missing-path", "missing-sha256", "missing-size_bytes",
                     "hash", "size", "negative-size", "boolean-size")
        for scope in ("raw", "bundle"):
            for mutation in mutations:
                with self.subTest(scope=scope, mutation=mutation):
                    fixture = self.fixture(f"{scope}-{mutation}")
                    self.sidecar(fixture)
                    if scope == "bundle":
                        self.collect_with_capture(fixture)
                    entries = fixture.run["checks"][0]["sanitizer_capture"]["files"]
                    entry = entries[0]
                    if mutation == "duplicate":
                        entries.append(copy.deepcopy(entry))
                    elif mutation.startswith("missing-"):
                        del entry[mutation.removeprefix("missing-")]
                    elif mutation == "hash":
                        entry["sha256"] = "0" * 64
                    elif mutation == "size":
                        entry["size_bytes"] += 1
                    elif mutation == "negative-size":
                        entry["size_bytes"] = -1
                    else:
                        entry["size_bytes"] = True
                    self.assert_rejected(fixture, scope)

    def test_raw_capture_inventory_rejects_missing_extra_and_unknown_check_files(self):
        for mutation in ("listed-missing", "unlisted", "unknown-check", "unknown-empty-check"):
            with self.subTest(mutation=mutation):
                fixture = self.fixture(mutation)
                original = self.sidecar(fixture)
                if mutation == "listed-missing":
                    original.unlink()
                elif mutation == "unlisted":
                    (original.parent / "sanitizer.456").write_text("unlisted\n", encoding="utf-8")
                else:
                    unknown = fixture.raw / "sanitizer" / "unknown"
                    unknown.mkdir()
                    if mutation == "unknown-check":
                        (unknown / "sanitizer.456").write_text("unbound\n", encoding="utf-8")
                self.assert_rejected(fixture, "raw")

    def test_bundle_capture_inventory_rejects_missing_extra_and_unknown_check_files(self):
        for mutation in ("listed-missing", "unlisted", "unknown-check", "unknown-empty-check"):
            with self.subTest(mutation=mutation):
                fixture = self.fixture(mutation)
                self.sidecar(fixture)
                self.collect_with_capture(fixture)
                copied = fixture.output / "inputs/cpp/sanitizer/sanitizer.123"
                if mutation == "listed-missing":
                    copied.unlink()
                elif mutation == "unlisted":
                    (copied.parent / "sanitizer.456").write_text("unlisted\n", encoding="utf-8")
                else:
                    unknown = fixture.output / "inputs/unknown/sanitizer"
                    unknown.mkdir(parents=True)
                    if mutation == "unknown-check":
                        (unknown / "sanitizer.456").write_text("unbound\n", encoding="utf-8")
                self.assert_bundle_rejected(fixture)

    def test_capture_paths_cannot_escape_or_alias_the_canonical_check_directory(self):
        for scope in ("raw", "bundle"):
            for number, bad_path in enumerate((
                "../sanitizer/cpp/sanitizer.123", "sanitizer/cpp/../cpp/sanitizer.123",
                "sanitizer/second/sanitizer.123", "sanitizer\\cpp\\sanitizer.123",
                "sanitizer/cpp/sanitizer.0", "sanitizer/cpp/sanitizer.-1",
                "sanitizer/cpp/sanitizer.123.extra", "absolute",
            )):
                with self.subTest(scope=scope, bad_path=bad_path):
                    fixture = self.fixture(f"{scope}-{number}")
                    original = self.sidecar(fixture)
                    if scope == "bundle":
                        self.collect_with_capture(fixture)
                    capture = fixture.run["checks"][0]["sanitizer_capture"]
                    capture["files"][0]["path"] = str(original) if bad_path == "absolute" else bad_path
                    self.assert_rejected(fixture, scope)

    def test_capture_directory_itself_must_be_the_canonical_check_directory(self):
        for scope in ("raw", "bundle"):
            for number, directory in enumerate(("sanitizer/second", "sanitizer/cpp/.", "../sanitizer/cpp")):
                with self.subTest(scope=scope, directory=directory):
                    fixture = self.fixture(f"{scope}-{number}")
                    if scope == "bundle":
                        self.collect_with_capture(fixture)
                    fixture.run["checks"][0]["sanitizer_capture"]["directory"] = directory
                    self.assert_rejected(fixture, scope)

    def test_raw_capture_metadata_cannot_be_swapped_between_checks(self):
        fixture = self.fixture("swapped-checks")
        self.add_second_check(fixture)
        self.sidecar(fixture, "first check\n", 123)
        self.sidecar(fixture, "second check\n", 456, "second")
        first, second = fixture.run["checks"]
        first["sanitizer_capture"], second["sanitizer_capture"] = (
            second["sanitizer_capture"], first["sanitizer_capture"],
        )
        self.assert_rejected(fixture, "raw")

    def test_bundle_capture_bytes_cannot_be_swapped_between_checks(self):
        fixture = self.fixture("swapped-bytes")
        self.add_second_check(fixture)
        self.sidecar(fixture, "first check\n", 123)
        self.sidecar(fixture, "second check\n", 456, "second")
        self.collect_with_capture(fixture)
        first = fixture.output / "inputs/cpp/sanitizer/sanitizer.123"
        second = fixture.output / "inputs/second/sanitizer/sanitizer.456"
        first_bytes, second_bytes = first.read_bytes(), second.read_bytes()
        first.write_bytes(second_bytes)
        second.write_bytes(first_bytes)
        self.assert_bundle_rejected(fixture)

    def test_raw_capture_rejects_redirected_check_directory(self):
        fixture = self.fixture("linked-raw")
        self.sidecar(fixture)
        self.redirect_directory(fixture.raw / "sanitizer/cpp", fixture.root / "redirected")
        self.assert_rejected(fixture, "raw")

    def test_bundle_capture_rejects_redirected_check_directory(self):
        fixture = self.fixture("linked-bundle")
        self.sidecar(fixture)
        self.collect_with_capture(fixture)
        self.redirect_directory(fixture.output / "inputs/cpp/sanitizer", fixture.root / "redirected")
        self.assert_bundle_rejected(fixture)

    def test_sanitizer_diagnostic_cannot_be_removed_from_collected_manifest(self):
        fixture = self.fixture("forged-pass")
        self.sidecar(fixture, SanitizerEvidenceTests.DIAGNOSTICS["ubsan"])
        manifest = self.collect_with_capture(fixture)
        self.assertEqual(manifest["result"], "FAIL")
        manifest["result"] = "PASS"
        manifest["checks"][0].update(result="PASS", reasons=[])
        manifest["checks"][0].pop("sanitizer_capture")
        (fixture.output / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        self.assertTrue(any("differs from actual reports" in error for error in fixture.verify()))

    def test_copied_profile_cannot_remove_required_capture_policy(self):
        fixture = self.fixture("profile-snapshot")
        self.collect_with_capture(fixture)
        profile_copy = fixture.output / "profile.json"
        profile = json.loads(profile_copy.read_text(encoding="utf-8"))
        del profile["profiles"]["test-debug"]["sanitizer_capture"]
        profile_copy.write_text(json.dumps(profile), encoding="utf-8")
        self.assertTrue(any("profile" in error.lower() for error in fixture.verify()))

    def test_sidecar_added_during_copy_is_found_by_second_raw_inventory(self):
        fixture = self.fixture("copy-race")
        source = self.sidecar(fixture)
        late = source.parent / "sanitizer.456"
        original_copyfile = shutil.copyfile
        injected = []

        def copy_then_add(src, dst, *args, **kwargs):
            result = original_copyfile(src, dst, *args, **kwargs)
            if Path(src) == source and not injected:
                late.write_text(SanitizerEvidenceTests.DIAGNOSTICS["asan"], encoding="utf-8")
                injected.append(late)
            return result

        # Only the real file-copy boundary is controlled; collector decisions,
        # re-enumeration and byte authentication are production behavior.
        with patch("collect_validation_evidence.shutil.copyfile", side_effect=copy_then_add):
            with self.assertRaises(EvidenceError):
                fixture.collect()
        self.assertEqual(injected, [late])
        self.assertFalse((fixture.output / "manifest.json").exists())


class ReportParserTests(unittest.TestCase):
    def parse(self, parser, text, report=None):
        return parse_report(parser, text, "", report)

    def test_doctest_discovery_includes_filtered_cases(self):
        counts, skipped = self.parse("doctest", EvidenceFixture.doctest(2, 0, 5))
        self.assertEqual(counts, {"discovered": 7, "executed": 2, "passed": 2, "failed": 0, "skipped": 5})

    def test_lua_uses_final_runner_summary(self):
        counts, _ = self.parse("lua", "Results: 123 passed, 0 failed\nResults: 3 passed, 0 failed, 3 total\n")
        self.assertEqual(counts["passed"], 3)

    def test_lua_inconsistent_summary_rejected(self):
        with self.assertRaises(EvidenceError):
            self.parse("lua", "Results: 2 passed, 1 failed, 2 total\n")

    def test_unittest_report_on_stderr_supported(self):
        counts, _ = parse_report("unittest", "", "Ran 5 tests in 0.03s\n\nOK\n", None)
        self.assertEqual(counts["passed"], 5)

    def test_unittest_failures_are_counted(self):
        counts, _ = self.parse("unittest", "Ran 5 tests in 0.03s\n\nFAILED (failures=1, errors=1)\n")
        self.assertEqual(counts["failed"], 2)

    def test_unittest_expected_failures_are_not_passes(self):
        counts, _ = self.parse("unittest", "Ran 5 tests in 0.03s\n\nOK (expected failures=2)\n")
        self.assertEqual(counts, {"discovered": 5, "executed": 5, "passed": 3, "failed": 2, "skipped": 0})

    def test_ctest_skips_have_names(self):
        xml = '<testsuite tests="2" failures="0" disabled="1" skipped="0"><testcase name="core" status="run"/><testcase name="ai" status="disabled"/></testsuite>'
        counts, skipped = self.parse("ctest-junit", "", xml)
        self.assertEqual(counts["skipped"], 1)
        self.assertEqual(skipped, ["ai"])

    def test_ctest_malformed_xml_rejected(self):
        with self.assertRaises(EvidenceError):
            self.parse("ctest-junit", "", "<testsuite>")

    def test_ctest_failure_summary_cannot_hide_failure(self):
        with self.assertRaises(EvidenceError):
            self.parse("ctest-junit", "", '<testsuite tests="1" failures="1"><testcase name="core"/></testsuite>')

    def test_ctest_skip_and_disabled_summary_must_match_cases(self):
        for attribute in ("skipped", "disabled"):
            skipped_case = '<testcase name="core"><skipped/></testcase>' if attribute == "skipped" else '<testcase name="core" status="disabled"/>'
            for declared, node in (("1", '<testcase name="core"/>'), ("0", skipped_case), ("invalid", '<testcase name="core"/>')):
                with self.subTest(attribute=attribute, declared=declared, node=node):
                    with self.assertRaises(EvidenceError):
                        self.parse("ctest-junit", "", f'<testsuite tests="1" {attribute}="{declared}">{node}</testsuite>')

    def test_ctest_disabled_and_runtime_skip_are_distinct_summary_categories(self):
        # Matches CTest --output-junit with DISABLED and SKIP_RETURN_CODE cases.
        xml = ('<testsuite tests="3" failures="0" disabled="1" skipped="1">'
               '<testcase name="pass" status="run"/>'
               '<testcase name="skip" status="notrun"><skipped message="SKIP_RETURN_CODE=77"/></testcase>'
               '<testcase name="disabled" status="disabled"/></testsuite>')
        counts, skipped = self.parse("ctest-junit", "", xml)
        self.assertEqual(counts, {"discovered": 3, "executed": 1, "passed": 1, "failed": 0, "skipped": 2})
        self.assertEqual(skipped, ["skip", "disabled"])

    def test_ctest_nested_suite_summaries_must_match_cases(self):
        xml = '<testsuites tests="1"><testsuite tests="1" skipped="1"><testcase name="core"/></testsuite></testsuites>'
        with self.assertRaises(EvidenceError):
            self.parse("ctest-junit", "", xml)

    def test_vitest_results_use_actual_assertions(self):
        report = json.dumps({"numTotalTests": 2, "numPassedTests": 1, "numFailedTests": 0,
                             "numPendingTests": 1, "testResults": [{"assertionResults": [
                                 {"fullName": "works", "status": "passed"},
                                 {"fullName": "asset", "status": "pending"}]}]})
        counts, skipped = self.parse("vitest-json", "", report)
        self.assertEqual(counts["passed"], 1)
        self.assertEqual(skipped, ["asset"])

    def test_vitest_summary_cannot_contradict_assertions(self):
        with self.assertRaises(EvidenceError):
            self.parse("vitest-json", "", json.dumps({"numTotalTests": 50, "testResults": []}))

    def test_vitest_failed_run_cannot_hide_behind_passed_assertions(self):
        report = {"success": False, "numTotalTests": 1, "numPassedTests": 1, "numFailedTests": 0,
                  "numPendingTests": 0, "testResults": [{"assertionResults": [{"fullName": "core", "status": "passed"}]}]}
        with self.assertRaises(EvidenceError):
            self.parse("vitest-json", "", json.dumps(report))


class EvidenceCliTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="caesura-evidence-cli-")
        self.addCleanup(temporary.cleanup)
        self.f = EvidenceFixture(Path(temporary.name))

    def invoke(self, main, args):
        stdout, stderr = io.StringIO(), io.StringIO()
        with patch.object(sys, "argv", ["evidence-cli", *map(str, args)]), contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = main()
        return code, stdout.getvalue() + stderr.getvalue()

    def collect_args(self):
        return ["--profile", self.f.profile_path, "--profile-name", "test-debug", "--run", self.f.run_path, "--output", self.f.output]

    def verify_args(self):
        return ["--profile", self.f.profile_path, "--profile-name", "test-debug", "--expected-run", self.f.run_path,
                "--artifacts-dir", self.f.output, "--commit", "a" * 40]

    def test_collector_exit_codes_distinguish_failure_and_success(self):
        from collect_validation_evidence import main
        code, _ = self.invoke(main, self.collect_args())
        self.assertEqual(code, 0)
        self.f.run["run_id"] = "failed-run"
        self.f.output = self.f.output.parent.parent / "failed-run" / "test-debug"
        self.f.run["checks"][0]["exit_code"] = 2
        self.f.write_run()
        code, _ = self.invoke(main, self.collect_args())
        self.assertEqual(code, 1)

    def test_collector_missing_input_returns_failure(self):
        from collect_validation_evidence import main
        (self.f.raw / "cpp.stdout").unlink()
        code, output = self.invoke(main, self.collect_args())
        self.assertEqual(code, 1)
        self.assertIn("Missing stdout", output)

    def test_verifier_optional_missing_is_skip_not_success(self):
        from verify_release_candidate import main
        code, output = self.invoke(main, ["--artifacts-dir", self.f.output, "--skip-if-missing"])
        self.assertEqual(code, 77)
        self.assertIn("SKIP", output)

    def test_verifier_requires_external_profile_and_receipt(self):
        from verify_release_candidate import main
        code, _ = self.invoke(main, ["--artifacts-dir", self.f.output])
        self.assertEqual(code, 1)

    def test_generate_from_receipt_and_release_refusal_leave_fixture_unchanged(self):
        from verify_release_candidate import main
        code, output = self.invoke(main, [*self.verify_args(), "--generate-bundle", "--run", self.f.run_path, "--diagnostic"])
        self.assertEqual(code, 0, output)
        before = (self.f.output / "manifest.json").read_bytes()
        code, output = self.invoke(main, self.verify_args())
        self.assertEqual(code, 1)
        self.assertIn("test-fixture", output)
        self.assertEqual((self.f.output / "manifest.json").read_bytes(), before)

    def test_legacy_manual_report_cannot_replace_receipt(self):
        from verify_release_candidate import main
        code, output = self.invoke(main, [*self.verify_args(), "--report-file", self.f.root / "legacy.md"])
        self.assertEqual(code, 1)
        self.assertIn("Legacy", output)

    def test_missing_git_never_returns_a_historic_commit(self):
        from verify_release_candidate import get_target_commit
        with self.assertRaises(EvidenceError):
            get_target_commit(self.f.root)
        with self.assertRaises(EvidenceError):
            get_target_commit(self.f.root, "a59bab9")

    def test_generate_requires_actual_receipt_argument(self):
        from verify_release_candidate import main
        code, output = self.invoke(main, [*self.verify_args(), "--generate-bundle", "--diagnostic"])
        self.assertEqual(code, 1)
        self.assertIn("--run", output)


if __name__ == "__main__":
    unittest.main(verbosity=2)
