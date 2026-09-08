"""Tooling regressions only; these do not constitute Apple engine execution evidence."""
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

import run_apple_validation as driver


class DriverTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(dir=Path(__file__).parent)
        self.addCleanup(self.directory.cleanup)
        self.lane = driver.Lane.__new__(driver.Lane)
        self.lane.name = "unit-test"
        self.lane.evidence = Path(self.directory.name)
        self.lane.source = self.lane.evidence
        self.lane.receipt = {"commands": []}

    def run_command(self, source, **kwargs):
        with contextlib.redirect_stdout(io.StringIO()):
            return self.lane.run("probe", [sys.executable, "-c", source], **kwargs)

    def test_nonzero_exit_is_retained_in_original_logs(self):
        code, text = self.run_command("import sys; print('partial output'); sys.stderr.write('actual failure'); sys.exit(7)", allow_failure=True)
        self.assertEqual((code, text), (7, "partial output"))
        row = self.lane.receipt["commands"][0]
        self.assertEqual(row["exit_code"], 7)
        self.assertEqual((self.lane.evidence / row["stderr"]).read_text(), "actual failure")
        self.assertEqual(row["stderr_sha256"], driver.sha256(self.lane.evidence / row["stderr"]))

    def test_required_command_failure_raises_after_receipt_is_written(self):
        with self.assertRaisesRegex(RuntimeError, "exit code 8"):
            self.run_command("raise SystemExit(8)")
        saved = json.loads((self.lane.evidence / "eas-execution.json").read_text())
        self.assertEqual(saved["commands"][0]["exit_code"], 8)

    def test_timeout_is_failure_with_retained_receipt(self):
        code, _ = self.run_command("import time; time.sleep(30)", timeout=0.2, allow_failure=True)
        self.assertEqual(code, 124)
        self.assertEqual(self.lane.receipt["commands"][0]["error"], "timeout")

    def test_invalid_source_sha_never_reaches_git(self):
        for value in ("master", "a" * 39, "a" * 40 + "; touch unexpected", "g" * 40):
            with self.subTest(value=value), mock.patch.object(self.lane, "run") as run:
                self.lane.source_sha = value
                with self.assertRaises(ValueError):
                    self.lane.checkout()
                run.assert_not_called()

    def test_dirty_checkout_is_rejected(self):
        self.lane.root = self.lane.evidence
        self.lane.source_sha = "a" * 40
        with mock.patch.object(self.lane, "run", side_effect=[(0, ""), (0, ""), (0, ""), (0, "a" * 40), (0, " M src/main.cpp")]):
            with self.assertRaisesRegex(RuntimeError, "clean commit"):
                self.lane.checkout()

    def test_macos_binary_cannot_count_as_simulator_evidence(self):
        with mock.patch.object(self.lane, "run", side_effect=[(0, "Mach-O"), (0, "arm64"), (0, "platform MACOS")]):
            with self.assertRaisesRegex(RuntimeError, "IOSSIMULATOR"):
                self.lane.identify_binary("tests", self.lane.evidence / "test", "IOSSIMULATOR")

    def test_wrong_architecture_cannot_count_as_simulator_evidence(self):
        with mock.patch.object(self.lane, "run", side_effect=[(0, "Mach-O"), (0, "x86_64"), (0, "platform IOSSIMULATOR")]):
            with self.assertRaisesRegex(RuntimeError, "arm64"):
                self.lane.identify_binary("tests", self.lane.evidence / "test", "IOSSIMULATOR")


if __name__ == "__main__":
    unittest.main()
