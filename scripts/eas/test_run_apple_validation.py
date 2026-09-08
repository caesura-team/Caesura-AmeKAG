"""Tooling regressions only; these do not constitute Apple engine execution evidence."""
import contextlib
import io
import json
from pathlib import Path
import shutil
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

    def make_ios_fixtures(self):
        source = self.lane.evidence / "source"
        for relative in ("scripts", "assets", "demo", "tests/audio"):
            directory = source / relative
            directory.mkdir(parents=True)
            (directory / "fixture.txt").write_text(relative, encoding="utf-8")
        shutil.copyfile(driver.UPLOAD_ROOT / "tests/SyncTestAssets.cmake", source / "tests/SyncTestAssets.cmake")
        self.lane.source = source
        build = source / "build/eas-ios-simulator"
        binaries = {name: build / ("tests" if name == "CaesuraTests" else "") / "Debug-iphonesimulator" / (name + ".app") / name
                    for name in ("CaesuraAmeKAG", "CaesuraTests")}
        for binary in binaries.values():
            binary.parent.mkdir(parents=True)
            binary.write_bytes(b"tooling-fixture-path-only")
        return source, build, binaries

    def make_simulator_inventory(self):
        simulated_home = self.lane.evidence / "worker-home"
        self.lane.simulator = "B2EDD32F-A2A0-471B-AB51-AECB1AA67BDA"
        data = simulated_home / "Library/Developer/CoreSimulator/Devices" / self.lane.simulator / "data"
        data.mkdir(parents=True)
        inventory = {"devices": {"iOS": [{"udid": self.lane.simulator, "dataPath": str(data)}]}}
        return simulated_home, data, inventory

    def test_sync_uses_real_products_when_generated_xcode_path_is_unexpanded(self):
        source, build, binaries = self.make_ios_fixtures()
        generated = build / "tests/sync_caesura_test_assets_Debug.cmake"
        literal_test = build / "tests/Debug${EFFECTIVE_PLATFORM_NAME}/CaesuraTests.app"
        literal_app = build / "Debug${EFFECTIVE_PLATFORM_NAME}/CaesuraAmeKAG.app"
        generated.write_text(
            f"set(CAESURA_FIXTURE_SOURCE_ROOT [[{source.as_posix()}]])\n"
            f"set(CAESURA_FIXTURE_BUILD_ROOT [[{build.as_posix()}]])\n"
            f"set(CAESURA_FIXTURE_TEST_OUTPUT [[{literal_test.as_posix()}]])\n"
            f"set(CAESURA_FIXTURE_APP_OUTPUT [[{literal_app.as_posix()}]])\n"
            f"include([[{(source / 'tests/SyncTestAssets.cmake').as_posix()}]])\n", encoding="utf-8")
        with contextlib.redirect_stdout(io.StringIO()):
            self.lane.run("generated-sync-reproduction", ["cmake", "-P", generated])
            self.assertTrue((literal_test / "assets/fixture.txt").is_file())
            self.assertFalse((binaries["CaesuraTests"].parent / "assets").exists())
            actual = self.lane.prepare_ios_fixtures(build, binaries)
        self.assertEqual(actual, binaries["CaesuraTests"].parent.resolve())
        for relative in ("scripts", "assets", "demo", "tests/audio"):
            self.assertEqual((actual / relative / "fixture.txt").read_text(), relative)
        self.assertIn("${EFFECTIVE_PLATFORM_NAME}", self.lane.receipt["test_fixture"]["generated_directory"])
        self.assertTrue(any(row["id"] == "sync-ios-fixtures" and row["exit_code"] == 0 for row in self.lane.receipt["commands"]))

    def test_deploys_all_fixtures_to_only_the_created_simulator_data_directory(self):
        source, _, _ = self.make_ios_fixtures()
        simulated_home, data, inventory = self.make_simulator_inventory()
        other = data.parent.parent / "UNRELATED-DEVICE/data"
        other.mkdir(parents=True)
        sentinel = other / "keep.txt"
        sentinel.write_text("unrelated", encoding="utf-8")
        with mock.patch.object(Path, "home", return_value=simulated_home), contextlib.redirect_stdout(io.StringIO()):
            actual = self.lane.deploy_simulator_fixtures(source, inventory)
        self.assertEqual(actual, data.resolve())
        self.assertEqual(sentinel.read_text(), "unrelated")
        for relative in ("scripts", "assets", "demo", "tests/audio"):
            self.assertEqual((actual / relative / "fixture.txt").read_text(), relative)
        proof = self.lane.receipt["simulator_fixture_deployment"]
        self.assertEqual(proof["source_inventory"], proof["deployed_inventory"])
        self.assertEqual(len([row for row in self.lane.receipt["commands"] if row["id"].startswith("simulator-copy-")]), 4)

    def test_missing_fixture_preflight_prevents_partial_simulator_copy(self):
        source, _, _ = self.make_ios_fixtures()
        (source / "demo/fixture.txt").unlink()
        (source / "demo").rmdir()
        simulated_home, data, inventory = self.make_simulator_inventory()
        with mock.patch.object(Path, "home", return_value=simulated_home):
            with self.assertRaisesRegex(ValueError, "fixture"):
                self.lane.deploy_simulator_fixtures(source, inventory)
        self.assertEqual(list(data.iterdir()), [])
        self.assertEqual(self.lane.receipt["commands"], [])

    def test_simulator_data_path_for_another_udid_is_rejected(self):
        source, _, _ = self.make_ios_fixtures()
        simulated_home, data, inventory = self.make_simulator_inventory()
        inventory["devices"]["iOS"][0]["dataPath"] = str(data.parent.parent / "WRONG-UDID/data")
        with mock.patch.object(Path, "home", return_value=simulated_home):
            with self.assertRaisesRegex(ValueError, "simulator"):
                self.lane.deploy_simulator_fixtures(source, inventory)
        self.assertEqual(list(data.iterdir()), [])
        self.assertEqual(self.lane.receipt["commands"], [])

    def test_existing_simulator_fixture_is_preserved_and_rejected(self):
        source, _, _ = self.make_ios_fixtures()
        simulated_home, data, inventory = self.make_simulator_inventory()
        (data / "assets").mkdir()
        sentinel = data / "assets/existing.txt"
        sentinel.write_text("preserve", encoding="utf-8")
        with mock.patch.object(Path, "home", return_value=simulated_home):
            with self.assertRaisesRegex(ValueError, "existing"):
                self.lane.deploy_simulator_fixtures(source, inventory)
        self.assertEqual(sentinel.read_text(), "preserve")
        self.assertFalse((data / "scripts").exists())
        self.assertEqual(self.lane.receipt["commands"], [])


if __name__ == "__main__":
    unittest.main()
