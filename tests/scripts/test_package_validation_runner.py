"""Real archive/receipt boundaries with explicitly synthetic stage results.

These unit tests never execute an engine or grant package runtime acceptance.
The full runtime lanes execute separately against the final distribution bytes.
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
MODULE = ROOT / "scripts/run_package_validation.py"
if MODULE.exists():
    spec = importlib.util.spec_from_file_location("package_validation_runner", MODULE)
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
else:
    runner = None


class PackageValidationRunner(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(runner, "Missing final package validation runner")
        temporary = tempfile.TemporaryDirectory(prefix="u22-package-runner-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.platform = {"win32": "windows", "linux": "linux", "darwin": "macos"}[sys.platform]
        self.archive = self.root / "final.zip"
        with zipfile.ZipFile(self.archive, "w") as archive:
            archive.writestr("Engine/scripts/main.lua", "return 1")
        self.digest = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        self.requirements = self.root / "build-requirements.json"
        self.requirements.write_text(json.dumps({
            "schema": "caesura.package-build.v1", "platform": self.platform,
            "configuration": "Debug", "archive_basename": "Engine", "version": "1.2.3",
            "required_configuration": {"schema": 1, "sdl_linkage": "static", "sdl_libraries": [],
                                       "ffmpeg": False, "steam": False, "live2d": False},
        }), encoding="utf-8")
        self.requirements_sha = hashlib.sha256(self.requirements.read_bytes()).hexdigest()
        self.source = {"source_sha": "a" * 40, "dirty": True, "worktree_fingerprint": "fixture-source"}
        self.static = mock.patch.object(runner, "_static_stage", return_value={"status": "STATIC_PASS"}).start()
        self.runtime = mock.patch.object(runner, "_runtime_stage", return_value={"status": "RUNTIME_PASS"}).start()
        self.identity = mock.patch.object(runner, "_source_identity", return_value=self.source).start()
        self.addCleanup(mock.patch.stopall)

    def run_attempt(self, **changes):
        options = dict(input_path=self.archive, expected_sha256=self.digest,
                       attempt_dir=self.root / "attempt", platform=self.platform, configuration="Debug",
                       source_sha=self.source["source_sha"], requirements_path=self.requirements,
                       requirements_sha256=self.requirements_sha, diagnostic=True,
                       tool_paths={"python": Path(sys.executable)})
        options.update(changes)
        return runner.run_package_validation(**options)

    def test_real_zip_is_prepared_and_stable_but_dirty_evidence_is_diagnostic(self):
        result = self.run_attempt()
        self.assertEqual(result["status"], "DIAGNOSTIC_PASS")
        self.assertFalse(result["accepted"])
        self.assertEqual(result["preparation"]["input"]["archive_sha256"], self.digest)
        self.assertEqual(result["stability"]["status"], "STABLE")
        self.static.assert_called_once()
        self.runtime.assert_called_once()
        receipt = json.loads((self.root / "attempt/package-run.json").read_text(encoding="utf-8"))
        self.assertEqual(receipt, result)

    def test_native_audio_selection_is_external_and_reaches_runtime(self):
        result = self.run_attempt(audio_output='software')
        self.assertEqual(result['status'], 'DIAGNOSTIC_PASS', result['errors'])
        self.assertEqual(result['audio_output'], 'software')
        self.assertNotIn('audio_output', result['requirements']['value'])
        self.assertEqual(self.runtime.call_args.args[2]['audio_output'], 'software')

    def test_invalid_or_web_audio_selection_is_refused(self):
        for index, changes in enumerate(({'audio_output':'automatic'},
                                        {'audio_output':'software', 'platform':'web'})):
            result = self.run_attempt(attempt_dir=self.root/str(index), **changes)
            self.assertEqual(result['status'], 'FAIL')
        self.runtime.assert_not_called()

    def test_bad_final_digest_never_enters_static_or_runtime(self):
        result = self.run_attempt(expected_sha256="0" * 64)
        self.assertEqual(result["status"], "FAIL")
        self.static.assert_not_called()
        self.runtime.assert_not_called()

    def test_external_requirements_identity_and_configuration_are_mandatory(self):
        for index, changes in enumerate(({"requirements_sha256": "0" * 64},
                                         {"requirements_path": None}, {"configuration": "Release"},
                                         {"source_sha": "b" * 40}, {"diagnostic": False})):
            with self.subTest(changes=changes):
                result = self.run_attempt(attempt_dir=self.root / str(index), **changes)
                self.assertEqual(result["status"], "FAIL")
        self.runtime.assert_not_called()

    def test_static_failure_blocks_runtime_and_is_retained(self):
        self.static.return_value = {"status": "STATIC_FAIL", "errors": ["missing required runtime"]}
        result = self.run_attempt()
        self.assertEqual(result["status"], "FAIL")
        self.assertEqual(result["static"]["errors"], ["missing required runtime"])
        self.runtime.assert_not_called()

    def test_runtime_failure_or_missing_status_cannot_become_pass(self):
        for index, value in enumerate(({"status": "RUNTIME_FAIL"}, {}, {"status": "NOT_RUN"})):
            with self.subTest(value=value):
                self.runtime.return_value = value
                self.assertEqual(self.run_attempt(attempt_dir=self.root / str(index))["status"], "FAIL")

    def test_archive_mutation_after_runtime_invalidates_attempt(self):
        def mutate(*args, **kwargs):
            with self.archive.open("ab") as archive:
                archive.write(b"post-validation transformation")
            return {"status": "RUNTIME_PASS"}
        self.runtime.side_effect = mutate
        result = self.run_attempt()
        self.assertEqual(result["status"], "FAIL")
        self.assertFalse(result["accepted"])

    def test_source_or_required_configuration_change_invalidates_attempt(self):
        for index, mutation in enumerate(("source", "requirements")):
            with self.subTest(mutation=mutation):
                def mutate(*args, **kwargs):
                    if mutation == "source":
                        self.identity.return_value = {**self.source, "worktree_fingerprint": "changed"}
                    else:
                        self.requirements.write_text("{}", encoding="utf-8")
                    return {"status": "RUNTIME_PASS"}
                self.runtime.side_effect = mutate
                self.assertEqual(self.run_attempt(attempt_dir=self.root / str(index))["status"], "FAIL")
                self.identity.return_value = self.source

    def test_existing_attempt_is_never_overwritten(self):
        old = self.run_attempt()
        receipt = self.root / "attempt/package-run.json"
        before = receipt.read_bytes()
        with self.assertRaises((FileExistsError, ValueError)):
            self.run_attempt()
        self.assertEqual(receipt.read_bytes(), before)
        self.assertEqual(old["status"], "DIAGNOSTIC_PASS")

    def test_attempt_inside_repository_is_refused_before_side_effects(self):
        with self.assertRaises(ValueError):
            self.run_attempt(attempt_dir=ROOT / "artifacts/validation/never-create-package-attempt")
        self.runtime.assert_not_called()

    def test_attempt_inside_input_directory_does_not_modify_the_input(self):
        package = self.root / "directory package"
        package.mkdir()
        (package / "keep.txt").write_bytes(b"unchanged")
        with self.assertRaises(ValueError):
            self.run_attempt(input_path=package, attempt_dir=package / "attempt")
        self.assertEqual([path.name for path in package.iterdir()], ["keep.txt"])
        self.assertEqual((package / "keep.txt").read_bytes(), b"unchanged")
        self.runtime.assert_not_called()

    def test_stage_receipt_deletion_or_replacement_invalidates_attempt(self):
        for index, mutation in enumerate(("delete", "replace")):
            with self.subTest(mutation=mutation):
                attempt = self.root / str(index)
                def mutate(*args, **kwargs):
                    path = attempt / "static.json"
                    if mutation == "delete":
                        path.unlink()
                    else:
                        path.write_text('{"status":"STATIC_FAIL"}', encoding="utf-8")
                    return {"status": "RUNTIME_PASS"}
                self.runtime.side_effect = mutate
                result = self.run_attempt(attempt_dir=attempt)
                self.assertEqual(result["status"], "FAIL")
                self.assertEqual(result["static"]["status"], "STATIC_PASS")
                self.assertTrue(result["errors"])

    def test_other_repository_is_refused_before_creating_attempt(self):
        other = self.root / "another repository"
        other.mkdir()
        (other / ".git").mkdir()
        with self.assertRaises(ValueError):
            self.run_attempt(attempt_dir=other / "attempt")
        self.assertFalse((other / "attempt").exists())
        self.runtime.assert_not_called()

    def test_web_ui_actions_are_prelocked_forwarded_and_rechecked(self):
        action = self.root / 'web-actions.json'
        for index, mutate in enumerate((False, True)):
            with self.subTest(mutate=mutate):
                action.write_text('{"schema":1,"steps":[{"click":"#advance"}]}', encoding='utf-8')
                digest = hashlib.sha256(action.read_bytes()).hexdigest()
                def runtime(platform, payload, requirements, attempt, tools):
                    self.assertEqual(requirements['actions'], {'path':str(action), 'sha256':digest})
                    if mutate:
                        action.write_text('{}', encoding='utf-8')
                    return {'status':'RUNTIME_PASS'}
                self.runtime.side_effect = runtime
                result = self.run_attempt(platform='web', attempt_dir=self.root / str(index),
                    actions_path=action, actions_sha256=digest,
                    tool_paths={name:Path(sys.executable) for name in ('node','browser','lua')})
                self.assertEqual(result['status'], 'FAIL' if mutate else 'DIAGNOSTIC_PASS')
                self.assertEqual(result['actions']['sha256'], digest)

    def test_native_lane_refuses_web_action_configuration(self):
        result = self.run_attempt(actions_path=self.requirements, actions_sha256=self.requirements_sha)
        self.assertEqual(result['status'], 'FAIL')
        self.static.assert_not_called()
        self.runtime.assert_not_called()

    def container_fixture(self, format):
        """Real retained bytes, explicit fake extraction (no hdiutil/AppImage)."""
        import package_containers
        from package_verification import inspect_inventory, prepare_package
        platform = 'macos' if format == 'dmg' else 'linux'
        metadata = json.loads(self.requirements.read_text(encoding='utf-8'))
        metadata['platform'] = platform
        self.requirements.write_text(json.dumps(metadata), encoding='utf-8')
        self.requirements_sha = hashlib.sha256(self.requirements.read_bytes()).hexdigest()
        source = self.root / 'simulated extracted volume'
        source.mkdir()
        (source / 'required.lua').write_bytes(b'return true')
        def prepare(input_path, attempt, **kwargs):
            self.assertEqual(kwargs['container_format'], format)
            self.assertEqual(kwargs['expected_sha256'], self.digest)
            self.assertEqual(kwargs['payload_relative_path'], '.')
            attempt.mkdir()
            execution = attempt / 'locked-container'
            execution.write_bytes(self.archive.read_bytes())
            prepared = prepare_package(source, attempt / 'payload',
                expected_inventory_sha256=inspect_inventory(source)['sha256'])
            report = {'schema': package_containers.SCHEMA, 'status':'CONTAINER_PREPARED',
                'runtime':'NOT_RUN', 'accepted':False, 'attempt_path':str(attempt),
                'expected_sha256':self.digest, 'input':{'path':str(input_path)},
                'execution_input':{'path':str(execution)}, 'preparation':prepared,
                'package_path':prepared['package_path'], 'tools':{}, 'evidence':{},
                'cleanup':{'status':'DETACHED' if format == 'dmg' else 'NO_MOUNT'}}
            (attempt / 'container-preparation.json').write_text(
                json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True)+'\n', encoding='utf-8', newline='\n')
            return report
        mock.patch.object(runner, '_host_platform', return_value=platform).start()
        mock.patch.object(runner, 'prepare_container', side_effect=prepare).start()
        return dict(platform=platform, container_format=format, payload_relative_path='.',
            tool_paths={'python':Path(sys.executable), **({'hdiutil':Path(sys.executable)} if format == 'dmg' else {})})

    def test_container_payload_keeps_exact_root_and_verifies_retained_bytes(self):
        options = self.container_fixture('dmg')
        result = self.run_attempt(**options)
        self.assertEqual(result['status'], 'DIAGNOSTIC_PASS', result['errors'])
        self.assertEqual(result['container']['status'], 'CONTAINER_PREPARED')
        self.assertEqual(result['stability']['status'], 'STABLE')
        self.assertTrue((Path(result['payload']) / 'required.lua').is_file())
        self.assertNotIn('Engine', Path(result['payload']).parts)

    def test_appimage_marks_apprun_entry_for_runtime(self):
        options = self.container_fixture('appimage')
        def runtime(platform, payload, requirements, attempt, tools):
            self.assertEqual(requirements['container_format'], 'appimage')
            return {'status':'RUNTIME_PASS'}
        self.runtime.side_effect = runtime
        result = self.run_attempt(**options)
        self.assertEqual(result['status'], 'DIAGNOSTIC_PASS', result['errors'])

    def test_container_final_mutation_and_receipt_loss_both_fail(self):
        options = self.container_fixture('dmg')
        for index, mutation in enumerate(('container', 'receipt')):
            with self.subTest(mutation=mutation):
                attempt = self.root / str(index)
                def runtime(*args):
                    if mutation == 'container':
                        self.archive.write_bytes(b'changed container')
                    else:
                        (attempt / 'container/container-preparation.json').unlink()
                    return {'status':'RUNTIME_PASS'}
                before = self.archive.read_bytes()
                self.runtime.side_effect = runtime
                result = self.run_attempt(attempt_dir=attempt, **options)
                self.assertEqual(result['status'], 'FAIL')
                self.assertTrue(result['errors'])
                self.archive.write_bytes(before)

    def test_container_preparation_failure_never_reaches_runtime(self):
        options = self.container_fixture('dmg')
        with mock.patch.object(runner, 'prepare_container', return_value={
                'status':'CONTAINER_FAIL', 'errors':['mount identity changed']}):
            result = self.run_attempt(**options)
        self.assertEqual(result['status'], 'FAIL')
        self.assertEqual(result['container']['errors'], ['mount identity changed'])
        self.static.assert_not_called()
        self.runtime.assert_not_called()

    def test_container_options_are_explicit_and_platform_specific(self):
        for index, changes in enumerate(({'container_format':'dmg', 'platform':'web'},
                {'container_format':'unknown'}, {'container_format':'appimage', 'payload_relative_path':None},
                {'payload_relative_path':'.'}, {'container_format':'appimage', 'expected_sha256':None})):
            with self.subTest(changes=changes):
                result = self.run_attempt(attempt_dir=self.root / str(index), **changes)
                self.assertEqual(result['status'], 'FAIL')
        self.static.assert_not_called()
        self.runtime.assert_not_called()

    if sys.platform == "win32":
        def test_unreadable_stage_evidence_still_writes_failed_final_receipt(self):
            import ctypes
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
            kernel.CreateFileW.restype = ctypes.c_void_p
            kernel.CloseHandle.argtypes = [ctypes.c_void_p]
            kernel.CloseHandle.restype = ctypes.c_int
            handle = None
            def lock_own_receipt(*args, **kwargs):
                nonlocal handle
                handle = kernel.CreateFileW(str(self.root / "attempt/static.json"),
                                            0x80000000, 0, None, 3, 0x80, None)
                self.assertNotEqual(handle, ctypes.c_void_p(-1).value)
                return {"status": "RUNTIME_PASS"}
            self.runtime.side_effect = lock_own_receipt
            try:
                result = self.run_attempt()
                self.assertEqual(result["status"], "FAIL")
                self.assertTrue(result["errors"])
                receipt = json.loads((self.root / "attempt/package-run.json").read_text(encoding="utf-8"))
                self.assertEqual(receipt, result)
            finally:
                if handle is not None and handle != ctypes.c_void_p(-1).value:
                    kernel.CloseHandle(handle)


if __name__ == "__main__":
    unittest.main()
