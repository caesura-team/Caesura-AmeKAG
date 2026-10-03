"""Tooling regressions only; these do not constitute Apple engine execution evidence."""
import contextlib
import io
import json
import os
import subprocess
import textwrap
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


class SourceSnapshotImportTests(unittest.TestCase):
    """Real isolated Python/Git topology used by the uploaded EAS bootstrap."""
    def exercise(self, mode):
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as directory:
            root = Path(directory).resolve()
            upload = root / "upload/scripts/eas"
            upload.mkdir(parents=True)
            shutil.copyfile(driver.UPLOAD_ROOT / "scripts/eas/run_apple_validation.py",
                            upload / "run_apple_validation.py")
            shutil.copyfile(driver.UPLOAD_ROOT / "scripts/validation_process.py",
                            upload.parent / "validation_process.py")
            source = root / "fetched"
            (source / "scripts").mkdir(parents=True)
            for name in ("run_validation.py", "validation_process.py", "validation_sanitizer.py"):
                shutil.copyfile(driver.UPLOAD_ROOT / "scripts" / name, source / "scripts" / name)
            (source / "fixture").mkdir()
            (source / "fixture/input.txt").write_text("actual fetched fixture\n", encoding="utf-8")
            (source / "scripts/validation_profiles.json").write_text(
                json.dumps({"fixture_paths": ["fixture"]}), encoding="utf-8")
            env = dict(os.environ, GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull,
                       GIT_TERMINAL_PROMPT="0", GIT_OPTIONAL_LOCKS="0", PYTHONDONTWRITEBYTECODE="1")
            for args in (("init", "-q"), ("add", "."),
                         ("-c", "user.name=Snapshot test", "-c", "user.email=snapshot@example.invalid",
                          "-c", "commit.gpgsign=false", "commit", "-qm", "snapshot fixture")):
                # The first fixture init is within this checked repository;
                # later Git operations require the fixture's own .git.
                self.assertTrue((driver.UPLOAD_ROOT / ".git").exists())
                if args[0] != "init": self.assertTrue((source / ".git").exists())
                subprocess.run(["git", "-C", str(source), *args], env=env, check=True,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
            self.assertTrue((source / ".git").exists())
            head = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"],
                                           env=env, text=True, timeout=30).strip()
            child = textwrap.dedent(r"""
                import importlib.util, json, pathlib, sys, types
                upload, source, head, mode = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), sys.argv[3], sys.argv[4]
                spec = importlib.util.spec_from_file_location('actual_uploaded_driver', upload)
                driver = importlib.util.module_from_spec(spec); spec.loader.exec_module(driver)
                names = ('validation_process', 'validation_sanitizer')
                if mode in ('cached', 'missing_helper'):
                    foreign = types.ModuleType('validation_sanitizer')
                    foreign.capture_environment = foreign.create_capture = foreign.snapshot_capture = lambda *a, **k: None
                    sys.modules['validation_sanitizer'] = foreign
                if mode == 'blocked_cache':
                    sys.modules['validation_sanitizer'] = None
                if mode == 'profile_failure':
                    (source/'scripts/validation_profiles.json').write_text('{invalid', encoding='utf-8')
                if mode == 'import_failure':
                    (source/'scripts/validation_sanitizer.py').write_text('invalid python !', encoding='utf-8')
                if mode == 'missing_helper':
                    (source/'scripts/validation_sanitizer.py').unlink()
                if mode == 'finder_none':
                    from importlib.machinery import PathFinder
                    scripts = source/'scripts'
                    parked = source/'scripts-parked'
                    scripts.rename(parked)
                    try:
                        assert PathFinder.find_spec('validation_sanitizer', [str(scripts)]) is None
                        assert str(scripts) in sys.path_importer_cache
                        assert sys.path_importer_cache[str(scripts)] is None
                    finally:
                        parked.rename(scripts)
                    sys.stderr.write('REAL_PATH_FINDER_NONE_CACHE_REACHED\n')
                previous_path = sys.path[:]
                missing = object()
                previous = {name: sys.modules.get(name, missing) for name in names}
                previous_finder = sys.path_importer_cache.get(str(source/'scripts'), missing)
                observed = []
                def trace(frame, event, arg):
                    if event == 'call' and frame.f_code.co_name == '_source_identity':
                        observed.append([frame.f_globals['run_owned_command'].__code__.co_filename,
                                         frame.f_globals['capture_environment'].__code__.co_filename])
                lane = driver.Lane.__new__(driver.Lane); lane.source = source
                error = None; value = None
                sys.setprofile(trace)
                try: value = lane.source_snapshot()
                except Exception as exc: error = exc
                finally: sys.setprofile(None)
                assert sys.path == previous_path, 'sys.path leaked fetched source'
                assert all(sys.modules.get(name, missing) is previous[name] for name in names), 'dependency modules leaked/replaced'
                assert sys.path_importer_cache.get(str(source/'scripts'), missing) is previous_finder, 'fetched path finder leaked'
                if mode == 'profile_failure':
                    assert isinstance(error, json.JSONDecodeError), repr(error)
                elif mode == 'missing_helper':
                    assert isinstance(error, FileNotFoundError), repr(error)
                elif mode == 'import_failure':
                    assert isinstance(error, SyntaxError), repr(error)
                else:
                    if error: raise error
                    assert value['source_sha'] == head and value['dirty'] is False, value
                    assert len(value['worktree_fingerprint']) == len(value['fixture_sha256']) == 64
                    assert observed == [[str(source/'scripts/validation_process.py'),
                                         str(source/'scripts/validation_sanitizer.py')]], observed
                print(json.dumps({'mode':mode, 'snapshot':value, 'error_type':type(error).__name__ if error else None,
                                  'helpers':observed, 'import_state_restored':True,
                                  'real_negative_finder_created':mode == 'finder_none'}))
            """)
            result = subprocess.run([sys.executable, "-I", "-B", "-c", child,
                                     str(upload / "run_apple_validation.py"), str(source), head, mode],
                                    cwd=root, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    text=True, encoding="utf-8", timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            data = json.loads(result.stdout)
            self.assertTrue(data["import_state_restored"])
            print("SOURCE_SNAPSHOT_IMPORT " + result.stdout.strip())
            return data

    def test_uploaded_bootstrap_uses_fetched_source_dependencies(self):
        self.assertEqual(self.exercise("plain")["error_type"], None)

    def test_cached_bootstrap_module_cannot_replace_fetched_dependency(self):
        self.assertEqual(self.exercise("cached")["error_type"], None)

    def test_profile_exception_restores_import_state(self):
        self.assertEqual(self.exercise("profile_failure")["error_type"], "JSONDecodeError")

    def test_missing_fetched_dependency_is_not_satisfied_by_bootstrap(self):
        self.assertEqual(self.exercise("missing_helper")["error_type"], "FileNotFoundError")

    def test_dependency_import_exception_restores_import_state(self):
        self.assertEqual(self.exercise("import_failure")["error_type"], "SyntaxError")

    def test_existing_blocked_module_cache_entry_is_restored(self):
        self.assertEqual(self.exercise("blocked_cache")["error_type"], None)

    def test_real_negative_finder_cache_is_bypassed_and_restored(self):
        self.assertTrue(self.exercise("finder_none")["real_negative_finder_created"])



class CollectorImportTests(unittest.TestCase):
    """Real collector imports; Apple commands below are explicit unit fixtures."""

    def test_simulator_collector_uses_fetched_siblings_and_restores_import_state(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as directory:
            root = Path(directory).resolve()
            upload = root / "upload/scripts/eas"
            upload.mkdir(parents=True)
            for name in ("run_apple_validation.py", "simulator_cwd_probe.c"):
                shutil.copyfile(driver.UPLOAD_ROOT / "scripts/eas" / name, upload / name)
            shutil.copyfile(driver.UPLOAD_ROOT / "scripts/validation_process.py",
                            upload.parent / "validation_process.py")
            source = root / "fetched"
            scripts = source / "scripts"
            scripts.mkdir(parents=True)
            for name in ("collect_validation_evidence.py", "validation_sanitizer.py", "validation_process.py"):
                shutil.copyfile(driver.UPLOAD_ROOT / "scripts" / name, scripts / name)
            (scripts / "validation_profiles.json").write_text(json.dumps({
                "profiles": {"macos-debug": {"checks": [{"id": "cpp", "min_discovered": 2}]}}
            }), encoding="utf-8")
            child = textwrap.dedent(r"""
                import importlib.util, json, pathlib, sys
                upload, source, root = map(pathlib.Path, sys.argv[1:])
                spec = importlib.util.spec_from_file_location('actual_uploaded_driver', upload)
                driver = importlib.util.module_from_spec(spec); spec.loader.exec_module(driver)
                lane = driver.Lane.__new__(driver.Lane)
                lane.source, lane.root = source, root / 'lane'
                lane.root.mkdir(); lane.evidence = lane.root / 'evidence'; lane.evidence.mkdir()
                lane.receipt = {}; lane.simulator = None
                binary = lane.root / 'fixture-not-an-executable'
                binary.write_bytes(b'unit fixture; never executed')
                fixtures = lane.root / 'fixtures'; fixtures.mkdir()
                udid = '11111111-2222-3333-4444-555555555555'
                runtime = 'com.apple.CoreSimulator.SimRuntime.iOS-18-0'
                device_type = 'fixture-iPhone'
                inventory = {'runtimes': [{'identifier': runtime, 'version': '18.0', 'isAvailable': True}],
                             'devicetypes': [{'identifier': device_type, 'productFamily': 'iPhone'}],
                             'devices': {runtime: [{'isAvailable': True, 'deviceTypeIdentifier': device_type}]}}
                calls = []
                def fixture_command(name, argv, **kwargs):
                    calls.append(name)
                    if name in ('simulator-runtimes', 'simulator-created-device'):
                        return 0, json.dumps(inventory)
                    if name == 'simulator-create': return 0, udid
                    if name == 'simulator-sdk': return 0, str(root)
                    if name == 'simulator-cpp':
                        return 0, '[doctest] test cases: 2 | 2 passed | 0 failed | 0 skipped'
                    return 0, ''
                lane.run = fixture_command
                lane.deploy_simulator_fixtures = lambda *args: fixtures
                lane.identify_binary = lambda *args: None
                lane.save = lambda: None
                names = ('validation_process', 'validation_sanitizer')
                missing = object(); previous_path = sys.path[:]
                previous_modules = {name: sys.modules.get(name, missing) for name in names}
                previous_finder = sys.path_importer_cache.get(str(source/'scripts'), missing)
                observed = []
                def trace(frame, event, arg):
                    if event == 'call' and frame.f_code.co_name == 'parse_doctest':
                        observed.append([frame.f_code.co_filename,
                                         frame.f_globals['read_capture_files'].__code__.co_filename])
                error = None
                sys.setprofile(trace)
                try: lane.simulator_tests(binary, fixtures)
                except Exception as exc: error = exc
                finally: sys.setprofile(None)
                assert 'simulator-cpp' in calls, 'actual collector path was not reached'
                assert sys.path == previous_path, 'sys.path leaked'
                assert all(sys.modules.get(n, missing) is previous_modules[n] for n in names), 'modules leaked'
                assert sys.path_importer_cache.get(str(source/'scripts'), missing) is previous_finder, 'finder leaked'
                if error: raise error
                assert observed == [[str(source/'scripts/collect_validation_evidence.py'),
                                     str(source/'scripts/validation_sanitizer.py')]], observed
                assert lane.receipt['simulator_cpp']['counts']['discovered'] == 2
                assert lane.receipt['simulator_cpp']['status'] == 'PASS'
                print(json.dumps({'scope':'unit fixture, no Apple execution', 'fetched_imports':observed,
                                  'import_state_restored':True, 'actual_collector_path_reached':True}))
            """)
            env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
            result = subprocess.run([sys.executable, "-I", "-B", "-c", child,
                                     str(upload / "run_apple_validation.py"), str(source), str(root)],
                                    cwd=root, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    text=True, encoding="utf-8", timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            data = json.loads(result.stdout)
            self.assertTrue(data["import_state_restored"])
            print("COLLECTOR_IMPORT " + result.stdout.strip())

if __name__ == "__main__":
    unittest.main()
