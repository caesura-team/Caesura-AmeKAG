"""CI orchestration contracts with real files and explicit command boundaries.

The command fixtures do not build or execute a game/browser and are not package
acceptance evidence. Production delegates to the actual package validator.
"""
from __future__ import annotations
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import ci_package_lane as lane
from package_verification import inspect_inventory

SHA = '1' * 40
SOURCE = {'source_sha': SHA, 'dirty': False, 'worktree_fingerprint': 'fixed-source'}
digest = lambda path: hashlib.sha256(Path(path).read_bytes()).hexdigest()


class PackageLaneTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='caesura-ci-lane-test-')
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name).resolve()
        self.repo = self.base / 'repo'; self.repo.mkdir(); (self.repo / '.git').mkdir()
        self.build = self.repo / 'build'; self.build.mkdir()
        (self.build / 'CPackConfig.cmake').write_text('fixture build config', encoding='utf-8')
        (self.repo / 'scripts').mkdir()
        (self.repo / 'web/node_modules/vite/bin').mkdir(parents=True)
        (self.repo / 'web/node_modules/vite/bin/vite.js').write_text('fixture Vite entry', encoding='utf-8')
        self.game = self.repo / 'chosen story.ks'; self.game.write_text('*start\nChosen story\n[p]\n', encoding='utf-8')
        self.python = Path(sys.executable).resolve()
        self.requirements = self.build / 'package-requirements-Release.json'
        self.work = self.base / 'lane'
        self.commands = []
        self.patch(lane, 'ROOT', self.repo)
        self.patch(lane, '_source_identity', return_value=SOURCE)
        self.patch(lane, '_execute', side_effect=self.command)

    def patch(self, target, name, *args, **kwargs):
        patch = mock.patch.object(target, name, *args, **kwargs)
        value = patch.start(); self.addCleanup(patch.stop); return value

    def metadata(self, platform='windows'):
        value = {'schema': 'caesura.package-build.v1', 'platform': platform, 'configuration': 'Release',
                 'archive_basename': 'ExactlyNamed', 'artifacts': {'zip': 'ExactlyNamed.zip', 'tgz': 'ExactlyNamed.tar.gz',
                 'dmg': 'ExactlyNamed.dmg', 'appimage': 'ExactlyNamed.AppImage'}, 'required_configuration': {'schema': 1}}
        self.requirements.write_text(json.dumps(value), encoding='utf-8')
        return value

    def native(self, platform='windows', **kwargs):
        return lane.run_native_lane(build_dir=self.build, requirements_path=self.requirements,
            platform=platform, source_sha=SHA, work_dir=self.work, cpack_executable=self.python,
            python_executable=self.python, **kwargs)

    def web(self, **kwargs):
        return lane.run_web_lane(game=self.game, source_sha=SHA, work_dir=self.work,
            node_executable=self.python, lua_executable=self.python, browser_executable=self.python, **kwargs)

    def command(self, report, name, argv, cwd, **kwargs):
        self.commands.append((name, list(argv)))
        args = list(argv)
        if name.startswith('cpack-'):
            fmt = {'ZIP': 'zip', 'TGZ': 'tgz', 'DragNDrop': 'dmg'}[args[args.index('-G') + 1]]
            value = json.loads(self.requirements.read_text())
            output = Path(args[args.index('-B') + 1]) / value['artifacts'][fmt]
            output.write_bytes(('final ' + fmt).encode())
        elif name == 'appimage-build':
            Path(args[args.index('--output') + 1]).write_bytes(b'final transformed AppImage')
        elif name == 'web-package':
            site = Path(args[args.index('--out') + 1]); site.mkdir()
            (site / 'index.html').write_text('<!doctype html>chosen package fixture', encoding='utf-8')
            (site / 'story.lua').write_text('return "chosen story"', encoding='utf-8')
        elif name.startswith('validate-'):
            attempt = Path(args[args.index('--attempt') + 1]); attempt.mkdir()
            path = Path(args[args.index('--input') + 1])
            expected = args[args.index('--inventory-sha256') + 1] if path.is_dir() else args[args.index('--sha256') + 1]
            value = {'schema': 'caesura.package-validation.v1', 'status': 'PASS', 'accepted': True,
                     'expected_source_sha': SHA, 'platform': args[args.index('--platform') + 1], 'configuration': 'Release',
                     'preparation': {'input': {'path': str(path)}, 'expected': {
                         'inventory_sha256': expected if path.is_dir() else None,
                         'archive_sha256': expected if path.is_file() else None}}}
            if '--container-format' in args:
                value['container'] = {'expected_sha256': expected, 'input': {'path': str(path)}}
            (attempt / 'package-run.json').write_text(json.dumps(value), encoding='utf-8')
        return 0

    def test_windows_uses_metadata_exact_name_and_prelocked_final_zip(self):
        self.metadata()
        report = self.native()
        self.assertEqual(report['status'], 'PASS')
        self.assertEqual([name for name, _ in self.commands], ['cpack-zip', 'validate-zip'])
        argv = self.commands[-1][1]
        package = Path(argv[argv.index('--input') + 1])
        self.assertEqual(package.name, 'ExactlyNamed.zip')
        self.assertEqual(argv[argv.index('--sha256') + 1], digest(package))
        self.assertEqual(argv[argv.index('--requirements-sha256') + 1], digest(self.requirements))
        self.assertFalse(Path(argv[argv.index('--attempt') + 1]).is_relative_to(self.repo))
        self.assertEqual(lane.verify_lane(self.work / 'lane.json', digest(self.work / 'lane.json'))['status'], 'UPLOAD_READY')

    def test_other_repository_and_existing_work_are_refused_before_commands(self):
        self.metadata()
        occupied = self.base / 'occupied'; occupied.mkdir(); (occupied / 'note').write_text('preserve')
        for work in (occupied, self.repo / 'new-lane'):
            self.work = work
            with self.assertRaises((ValueError, FileExistsError)):
                self.native()
        self.assertFalse(self.commands); self.assertEqual((occupied / 'note').read_text(), 'preserve')

    def test_metadata_cannot_select_paths_or_another_platform(self):
        for index, change in enumerate(({'platform': 'linux'}, {'configuration': 'Debug'}, {'artifacts': {'zip': '../escape.zip'}})):
            value = self.metadata(); value.update(change); self.requirements.write_text(json.dumps(value))
            self.work = self.base / f'bad-metadata-{index}'
            report = self.native()
            self.assertEqual(report['status'], 'FAIL')
        self.assertFalse(self.commands)

    def test_missing_exact_cpack_output_does_not_pick_another_archive(self):
        self.metadata()
        def wrong(report, name, argv, cwd, **kwargs):
            if name.startswith('cpack-'):
                (Path(argv[argv.index('-B') + 1]) / 'other.zip').write_bytes(b'wrong candidate')
                self.commands.append((name, argv)); return 0
            return self.command(report, name, argv, cwd, **kwargs)
        with mock.patch.object(lane, '_execute', side_effect=wrong):
            result = self.native()
        self.assertEqual(result['status'], 'FAIL')
        self.assertEqual(len(self.commands), 1)
        self.assertFalse((self.work / 'outputs/upload-manifest.json').exists())

    def test_macos_validates_both_final_tgz_and_dmg_with_explicit_root_and_tool(self):
        self.metadata('macos')
        report = self.native('macos', hdiutil_executable=self.python)
        self.assertEqual(report['status'], 'PASS')
        validations = [args for name, args in self.commands if name.startswith('validate-')]
        self.assertEqual(len(validations), 2)
        dmg = next(args for args in validations if '--container-format' in args)
        self.assertEqual(dmg[dmg.index('--container-format') + 1], 'dmg')
        self.assertEqual(dmg[dmg.index('--payload-relative-path') + 1], '.')
        self.assertEqual(dmg[dmg.index('--hdiutil') + 1], str(self.python))

    def test_dmg_creation_does_not_add_an_absolute_applications_symlink(self):
        self.metadata('macos')
        result = self.native('macos', hdiutil_executable=self.python)
        self.assertEqual(result['status'], 'PASS')
        command = next(args for name, args in self.commands if name == 'cpack-dmg')
        self.assertIn('CPACK_DMG_DISABLE_APPLICATIONS_SYMLINK=ON', command)
        self.assertEqual(command[command.index('CPACK_DMG_DISABLE_APPLICATIONS_SYMLINK=ON') - 1], '-D')

    def test_linux_prelocks_builder_inputs_and_validates_the_transformed_appimage(self):
        self.metadata('linux')
        tool = self.base / 'tool'; tool.write_bytes(b'locked tool')
        runtime = self.base / 'runtime'; runtime.write_bytes(b'locked runtime')
        report = self.native('linux', appimagetool_path=tool, appimagetool_sha256=digest(tool),
            runtime_path=runtime, runtime_sha256=digest(runtime))
        self.assertEqual(report['status'], 'PASS')
        builder = next(args for name, args in self.commands if name == 'appimage-build')
        self.assertEqual(builder[builder.index('--appimagetool-sha256') + 1], digest(tool))
        validations = [args for name, args in self.commands if name.startswith('validate-')]
        self.assertEqual(len(validations), 2)
        image = next(args for args in validations if '--container-format' in args)
        self.assertEqual(image[image.index('--sha256') + 1], hashlib.sha256(b'final transformed AppImage').hexdigest())

    def test_bad_builder_pin_fails_before_any_command(self):
        self.metadata('linux')
        result = self.native('linux', appimagetool_path=self.python, appimagetool_sha256='0' * 64,
            runtime_path=self.python, runtime_sha256=digest(self.python))
        self.assertEqual(result['status'], 'FAIL'); self.assertFalse(self.commands)

    def test_failed_or_diagnostic_validation_never_retries_or_emits_upload_manifest(self):
        self.metadata()
        for diagnostic in (False, True):
            self.work = self.base / ('diagnostic' if diagnostic else 'failure')
            self.commands = []
            def reject(report, name, argv, cwd, **kwargs):
                result = self.command(report, name, argv, cwd, **kwargs)
                if name.startswith('validate-'):
                    path = Path(argv[argv.index('--attempt') + 1]) / 'package-run.json'
                    value = json.loads(path.read_text()); value.update(status='DIAGNOSTIC_PASS' if diagnostic else 'FAIL', accepted=False)
                    path.write_text(json.dumps(value)); return 0 if diagnostic else 1
                return result
            with mock.patch.object(lane, '_execute', side_effect=reject):
                report = self.native()
            self.assertEqual(report['status'], 'FAIL'); self.assertEqual(len(self.commands), 2)
            self.assertFalse((self.work / 'outputs/upload-manifest.json').exists())
            self.assertTrue((self.work / 'validate-zip/package-run.json').exists())

    def test_artifact_mutation_during_validation_is_not_adopted_as_a_new_identity(self):
        self.metadata()
        def mutate(report, name, argv, cwd, **kwargs):
            result = self.command(report, name, argv, cwd, **kwargs)
            if name.startswith('validate-'):
                Path(argv[argv.index('--input') + 1]).write_bytes(b'modified after locked identity')
            return result
        with mock.patch.object(lane, '_execute', side_effect=mutate):
            report = self.native()
        self.assertEqual(report['status'], 'FAIL')

    def test_upload_recheck_rejects_changed_receipt_and_changed_package(self):
        self.metadata(); self.native()
        receipt = self.work / 'lane.json'; expected = digest(receipt)
        (self.work / 'outputs/ExactlyNamed.zip').write_bytes(b'changed before upload')
        with self.assertRaises(ValueError): lane.verify_lane(receipt, expected)
        receipt.write_text('{}')
        with self.assertRaises(ValueError): lane.verify_lane(receipt, expected)

    def test_web_explicit_game_bake_build_package_and_directory_validation(self):
        before = self.game.read_bytes(); report = self.web()
        self.assertEqual(report['status'], 'PASS')
        self.assertEqual([name for name, _ in self.commands], ['web-bake', 'web-build', 'web-package', 'validate-web-directory'])
        package = self.commands[2][1]
        self.assertEqual(package[-1], str(self.game)); self.assertIn('--out', package)
        self.assertEqual(self.game.read_bytes(), before)
        validation = self.commands[3][1]
        self.assertIn('--inventory-sha256', validation); self.assertNotIn('--diagnostic', validation)

    def test_web_zip_is_a_final_transform_with_its_own_validation(self):
        report = self.web(zip_name='chosen-game.zip')
        self.assertEqual(report['status'], 'PASS')
        validations = [args for name, args in self.commands if name.startswith('validate-')]
        self.assertEqual(len(validations), 2)
        final = validations[-1]; package = Path(final[final.index('--input') + 1])
        self.assertEqual(package.name, 'chosen-game.zip')
        self.assertEqual(final[final.index('--sha256') + 1], digest(package))
        manifest = json.loads((self.work / 'outputs/upload-manifest.json').read_text())
        self.assertEqual([entry['name'] for entry in manifest['files']], ['chosen-game.zip'])

    def test_web_build_failure_stops_without_packaging_or_retries(self):
        def fail_build(report, name, argv, cwd, **kwargs):
            self.commands.append((name, argv)); return 9 if name == 'web-build' else 0
        with mock.patch.object(lane, '_execute', side_effect=fail_build):
            report = self.web()
        self.assertEqual(report['status'], 'FAIL')
        self.assertEqual([name for name, _ in self.commands], ['web-bake', 'web-build'])

    def test_web_actions_remain_explicit_and_prelocked(self):
        action = self.repo / 'actions.json'; action.write_text('{"schema":1,"steps":[{"click":"#declared"}]}')
        expected = digest(action)
        report = self.web(actions_path=action, actions_sha256=expected)
        self.assertEqual(report['status'], 'PASS')
        argv = self.commands[-1][1]
        self.assertEqual(argv[argv.index('--actions') + 1], str(action))
        self.assertEqual(argv[argv.index('--actions-sha256') + 1], expected)
        self.assertEqual(digest(action), expected)

    def test_wrong_input_path_in_accepted_receipt_is_rejected(self):
        self.metadata()
        def unrelated(report, name, argv, cwd, **kwargs):
            result = self.command(report, name, argv, cwd, **kwargs)
            if name.startswith('validate-'):
                receipt = Path(argv[argv.index('--attempt') + 1]) / 'package-run.json'
                value = json.loads(receipt.read_text())
                value['preparation']['input']['path'] = str(self.base / 'another.zip')
                receipt.write_text(json.dumps(value))
            return result
        with mock.patch.object(lane, '_execute', side_effect=unrelated):
            result = self.native()
        self.assertEqual(result['status'], 'FAIL')
        self.assertFalse((self.work / 'outputs/upload-manifest.json').exists())

    def test_dirty_source_cannot_start_any_package_command(self):
        self.metadata()
        with mock.patch.object(lane, '_source_identity', return_value={**SOURCE, 'dirty': True}):
            result = self.native()
        self.assertEqual(result['status'], 'FAIL')
        self.assertFalse(self.commands)

    def test_source_change_before_upload_invalidates_the_accepted_lane(self):
        self.metadata(); self.native()
        receipt = self.work / 'lane.json'; expected = digest(receipt)
        with mock.patch.object(lane, '_source_identity', return_value={**SOURCE, 'worktree_fingerprint': 'changed'}):
            with self.assertRaises(ValueError):
                lane.verify_lane(receipt, expected)

    def bundle(self, directory='download', name='final.zip', payload=b'accepted final archive'):
        root = self.base / directory; root.mkdir()
        (root / name).write_bytes(payload)
        manifest = root / 'upload-manifest.json'
        manifest.write_text(json.dumps({'schema': lane.UPLOAD_SCHEMA, 'source_sha': SHA,
            'platform': 'web', 'files': [{'name': name, 'kind': 'file', 'sha256': digest(root / name)}]}))
        return root, digest(manifest)

    def test_release_collection_copies_only_explicit_final_files_and_records_checksums(self):
        bundle = self.bundle()
        (bundle[0] / 'unverified.zip').write_bytes(b'not selected')
        output = self.base / 'collected'
        result = lane.collect_bundles(bundles=[bundle], source_sha=SHA, work_dir=output)
        self.assertEqual(result['status'], 'COLLECTED')
        self.assertEqual(result['upload_files'], [str(output / 'final.zip'), str(output / 'checksums.txt')])
        self.assertEqual((output / 'final.zip').read_bytes(), b'accepted final archive')
        self.assertEqual((output / 'checksums.txt').read_text(), f'{digest(output / "final.zip")}  final.zip\n')
        self.assertFalse((output / 'unverified.zip').exists())

    def test_release_collection_refuses_replaced_manifest_or_archive(self):
        for index, changed in enumerate(('upload-manifest.json', 'final.zip')):
            root, expected = self.bundle(directory=f'download-{index}')
            (root / changed).write_bytes(b'replaced downloaded bytes')
            with self.assertRaises(ValueError):
                lane.collect_bundles(bundles=[(root, expected)], source_sha=SHA, work_dir=self.base / f'collect-{index}')

    def test_release_collection_rejects_duplicate_names_and_checksum_overwrite(self):
        duplicate = [self.bundle('one'), self.bundle('two')]
        with self.assertRaises(ValueError):
            lane.collect_bundles(bundles=duplicate, source_sha=SHA, work_dir=self.base / 'duplicate')
        reserved = self.bundle('reserved', 'checksums.txt')
        with self.assertRaises(ValueError):
            lane.collect_bundles(bundles=[reserved], source_sha=SHA, work_dir=self.base / 'reserved-output')


if __name__ == '__main__':
    unittest.main()
