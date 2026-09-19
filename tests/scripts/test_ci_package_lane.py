"""CI orchestration contracts with real files and explicit command boundaries.

The command fixtures do not build or execute a game/browser and are not package
acceptance evidence. Production delegates to the actual package validator.
"""
from __future__ import annotations
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import ci_package_lane as lane
from package_verification import inspect_inventory

SHA = '1' * 40
SOURCE = {'source_sha': SHA, 'dirty': False, 'worktree_fingerprint': 'fixed-source'}
digest = lambda path: hashlib.sha256(Path(path).read_bytes()).hexdigest()
REAL_EXECUTE = lane._execute


def initialize_fixture_git(repo):
    subprocess.run(['git', 'init', '--quiet', str(repo)], check=True, capture_output=True)
    (repo / '.gitignore').write_text('/dist/\n', encoding='utf-8')


def directory_link(link, target):
    if os.name == 'nt':
        subprocess.run([os.environ['ComSpec'], '/c', 'mklink', '/J', str(link), str(target)],
                       check=True, capture_output=True)
    else:
        link.symlink_to(target, target_is_directory=True)


class PackageLaneTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='caesura-ci-lane-test-')
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name).resolve()
        self.repo = self.base / 'repo'; self.repo.mkdir(); (self.repo / '.git').mkdir()
        initialize_fixture_git(self.repo)
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
            if '--audio-output' in args:
                value['audio_output'] = args[args.index('--audio-output') + 1]
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

    def test_audio_mode_is_caller_selected_and_bound_to_validation_receipt(self):
        self.metadata()
        report = self.native(audio_output='software')
        self.assertEqual(report['status'], 'PASS', report['errors'])
        self.assertEqual(report['audio_output'], 'software')
        args = self.commands[-1][1]
        self.assertEqual(args[args.index('--audio-output')+1], 'software')
        self.work = self.base / 'wrong-audio-receipt'
        def mismatch(report, name, argv, cwd, **kwargs):
            code = self.command(report, name, argv, cwd, **kwargs)
            if name.startswith('validate-'):
                path = Path(argv[argv.index('--attempt')+1])/'package-run.json'
                value = json.loads(path.read_text()); value['audio_output'] = 'device'
                path.write_text(json.dumps(value))
            return code
        with mock.patch.object(lane, '_execute', side_effect=mismatch):
            result = self.native(audio_output='software')
        self.assertEqual(result['status'], 'FAIL')
        self.assertFalse((self.work/'outputs/upload-manifest.json').exists())

    def test_invalid_audio_mode_never_packages(self):
        self.metadata()
        result = self.native(audio_output='automatic')
        self.assertEqual(result['status'], 'FAIL')
        self.assertFalse(self.commands)

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

    def test_web_stage_is_new_ignored_and_final_validation_remains_outside(self):
        report = self.web()
        self.assertEqual(report['status'], 'PASS', report['errors'])
        args = next(args for name, args in self.commands if name == 'web-package')
        stage = Path(args[args.index('--out') + 1])
        self.assertTrue(stage.is_relative_to(self.repo))
        self.assertEqual(subprocess.run(['git', '-C', str(self.repo), 'check-ignore', '--quiet', '--', str(stage)]).returncode, 0)
        self.assertFalse(Path(report['site']).is_relative_to(self.repo))
        self.assertEqual(inspect_inventory(stage), inspect_inventory(report['site']))
        self.work = self.base / 'second-web-lane'
        second = self.web()
        self.assertEqual(second['status'], 'PASS', second['errors'])
        second_args = next(args for name, args in reversed(self.commands) if name == 'web-package')
        self.assertNotEqual(stage, Path(second_args[second_args.index('--out')+1]))

    def test_web_nonignored_stage_is_refused(self):
        (self.repo / '.gitignore').write_text('', encoding='utf-8')
        report = self.web()
        self.assertEqual(report['status'], 'FAIL')
        self.assertNotIn('web-package', [name for name, _ in self.commands])

    def test_web_copy_changed_bytes_cannot_pass_after_source_is_restored(self):
        copy = shutil.copytree
        def change_during_copy(source, destination, **kwargs):
            path = Path(source) / 'story.lua'
            previous = path.read_bytes()
            path.write_bytes(b'changed only while copying')
            try:
                return copy(source, destination, **kwargs)
            finally:
                path.write_bytes(previous)
        with mock.patch.object(lane.shutil, 'copytree', side_effect=change_during_copy):
            report = self.web()
        self.assertEqual(report['status'], 'FAIL')
        self.assertFalse(any(name.startswith('validate-') for name, _ in self.commands))
        self.assertFalse((self.work / 'outputs/upload-manifest.json').exists())

    def test_web_same_bytes_replacement_of_stage_directory_is_refused(self):
        copy = shutil.copytree
        def replace_after_copy(source, destination, **kwargs):
            result = copy(source, destination, **kwargs)
            source = Path(source)
            previous = source.with_name('old-site-retained')
            source.rename(previous)
            copy(previous, source)
            return result
        with mock.patch.object(lane.shutil, 'copytree', side_effect=replace_after_copy):
            report = self.web()
        self.assertEqual(report['status'], 'FAIL')
        self.assertFalse(any(name.startswith('validate-') for name, _ in self.commands))

    def test_web_copy_failure_retains_failed_attempt_without_validation(self):
        with mock.patch.object(lane.shutil, 'copytree', side_effect=OSError('copy boundary failed')):
            report = self.web()
        self.assertEqual(report['status'], 'FAIL')
        self.assertFalse(any(name.startswith('validate-') for name, _ in self.commands))
        self.assertTrue((self.work / 'lane.json').is_file())

    def test_web_linked_stage_parent_is_refused_without_touching_target(self):
        target = self.base / 'unrelated'; target.mkdir()
        (target / 'preserve').write_bytes(b'previous data')
        directory_link(self.repo / 'dist', target)
        report = self.web()
        self.assertEqual(report['status'], 'FAIL')
        self.assertNotIn('web-package', [name for name, _ in self.commands])
        self.assertEqual(list(target.iterdir()), [target / 'preserve'])
        self.assertEqual((target / 'preserve').read_bytes(), b'previous data')

    def test_web_parent_link_replacement_during_copy_is_refused(self):
        copy = shutil.copytree
        for which in ('source', 'destination'):
            with self.subTest(parent=which):
                self.work = self.base / ('parent-change-' + which)
                self.commands = []
                def replace_parent(source, destination, **kwargs):
                    result = copy(source, destination, **kwargs)
                    parent = Path(source if which == 'source' else destination).parent
                    previous = parent.with_name(parent.name + '-retained')
                    parent.rename(previous)
                    directory_link(parent, previous)
                    return result
                with mock.patch.object(lane.shutil, 'copytree', side_effect=replace_parent):
                    report = self.web()
                self.assertEqual(report['status'], 'FAIL')
                self.assertFalse(any(name.startswith('validate-') for name, _ in self.commands))

    def test_web_stage_links_and_hardlinks_are_refused(self):
        for kind in ('directory-link', 'hardlink'):
            with self.subTest(kind=kind):
                self.work = self.base / kind; self.commands = []
                def add_link(report, name, argv, cwd, **kwargs):
                    result = self.command(report, name, argv, cwd, **kwargs)
                    if name == 'web-package':
                        stage = Path(argv[argv.index('--out') + 1])
                        if kind == 'hardlink':
                            os.link(stage / 'story.lua', stage / 'alias.lua')
                        else:
                            target = stage / 'content'; target.mkdir()
                            directory_link(stage / 'alias', target)
                    return result
                with mock.patch.object(lane, '_execute', side_effect=add_link):
                    report = self.web()
                self.assertEqual(report['status'], 'FAIL')
                self.assertFalse(any(name.startswith('validate-') for name, _ in self.commands))

    def test_web_existing_stage_container_and_copy_target_are_not_overwritten(self):
        occupied = self.repo / 'dist/caesura-web-collision'
        occupied.mkdir(parents=True); (occupied / 'preserve').write_bytes(b'old stage')
        with mock.patch.object(lane.uuid, 'uuid4', return_value=SimpleNamespace(hex='collision')):
            report = self.web()
        self.assertEqual(report['status'], 'FAIL')
        self.assertEqual((occupied / 'preserve').read_bytes(), b'old stage')
        self.assertNotIn('web-package', [name for name, _ in self.commands])
        self.work = self.base / 'copy-collision'
        def occupy_copy(report, name, argv, cwd, **kwargs):
            result = self.command(report, name, argv, cwd, **kwargs)
            if name == 'web-package':
                target = Path(report['work']) / 'outputs/site'; target.mkdir()
                (target / 'preserve').write_bytes(b'old destination')
            return result
        with mock.patch.object(lane, '_execute', side_effect=occupy_copy):
            report = self.web()
        self.assertEqual(report['status'], 'FAIL')
        self.assertEqual((self.work / 'outputs/site/preserve').read_bytes(), b'old destination')

    def test_web_late_same_byte_stage_replacement_fails_final_stability(self):
        def replace_stage(report, name, argv, cwd, **kwargs):
            result = self.command(report, name, argv, cwd, **kwargs)
            if name == 'validate-web-directory':
                stage = Path(report['web_staging']); previous = stage.with_name('retained-original')
                stage.rename(previous); shutil.copytree(previous, stage)
            return result
        with mock.patch.object(lane, '_execute', side_effect=replace_stage):
            report = self.web()
        self.assertEqual(report['status'], 'FAIL')
        self.assertFalse((self.work / 'outputs/upload-manifest.json').exists())

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


class RealWebPackerBoundaryTests(unittest.TestCase):
    """Actual Node/Lua CLI; synthetic player unless explicitly selected."""
    def test_actual_packer_uses_source_stage_then_exact_outside_tree(self):
        import caesura_build
        node = caesura_build.find_node()
        lua = os.environ.get('CAESURA_TEST_LUA') or caesura_build.find_lua()
        # Manual runs can discover a PATH name (e.g. lua5.4). The production
        # lane deliberately accepts explicit absolute executable paths only.
        lua = Path(shutil.which(str(lua)) or lua).resolve(strict=True)
        temporary = tempfile.TemporaryDirectory(prefix='u22-web-lane-cli-')
        evidence = os.environ.get('CAESURA_LANE_TEST_EVIDENCE')
        if evidence:
            temporary._finalizer.detach()  # retain this exact actual CLI attempt
        else:
            self.addCleanup(temporary.cleanup)
        base = Path(temporary.name).resolve()
        repo = base / 'repo'; repo.mkdir()
        for name in ('scripts', 'config', 'web'):
            shutil.copytree(ROOT / name, repo / name,
                ignore=shutil.ignore_patterns('node_modules', '__pycache__', 'dist'))
        for name in ('package.json', 'package-lock.json', 'npm-shrinkwrap.json'):
            if (ROOT / name).is_file():shutil.copy2(ROOT / name, repo / name)
        player = repo / 'web/dist'
        selected_player = os.environ.get('CAESURA_TEST_WEB_DIST')
        if selected_player:
            selected_player = Path(selected_player).resolve(strict=True)
            profile = json.loads((selected_player / 'capabilities-build.json').read_text(encoding='utf-8'))
            for name in (*profile['bundle_files'], 'capabilities-build.json'):
                target = player / name; target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(selected_player / name, target)
        else:
            # Ordinary CTest runs before any Web build. This tiny player is an
            # explicit fixture, while the production Node packer and Lua run.
            (player / 'web-assets').mkdir(parents=True)
            (player / 'index.html').write_text('<html><head></head><body></body></html>', encoding='utf-8')
            (player / 'manifest.webmanifest').write_text('{"start_url":"./index.html"}', encoding='utf-8')
            (player / 'sw.js').write_text('const REQUIRE_OFFLINE_MANIFEST = true;', encoding='utf-8')
            (player / 'web-assets/player.js').write_text('// synthetic player boundary', encoding='utf-8')
            (player / 'web-assets/glue.wasm').write_bytes(b'\0asm\1\0\0\0')
            for source in (repo / 'scripts').rglob('*.lua'):
                target = player / 'scripts' / source.relative_to(repo / 'scripts')
                target.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(source, target)
            generated = subprocess.run([str(node), str(repo / 'web/gen-index.mjs'),
                str(player / 'scripts'), str(player / 'scripts/index.json')], cwd=repo,
                capture_output=True, text=True, encoding='utf-8', timeout=30)
            self.assertEqual(generated.returncode, 0, generated.stdout + generated.stderr)
            driver = """import fs from 'node:fs';
const {createWebCapabilityProfile,collectWebSourceFiles}=await import(process.argv[1]);
fs.writeFileSync(process.argv[2]+'/capabilities-build.json',JSON.stringify(createWebCapabilityProfile(process.argv[2],{sourceFiles:collectWebSourceFiles()})));"""
            profiled = subprocess.run([str(node), '--input-type=module', '-e', driver,
                (repo / 'scripts/web_capability_profile.mjs').as_uri(), str(player)], cwd=repo,
                capture_output=True, text=True, encoding='utf-8', timeout=30)
            self.assertEqual(profiled.returncode, 0, profiled.stdout + profiled.stderr)
        (repo / 'assets').mkdir()
        for name in ('icon-192.png', 'icon-512.png'):
            shutil.copy2(ROOT / 'assets' / name, repo / 'assets' / name)
        game = repo / 'chosen'; game.mkdir()
        (game / 'story.ks').write_text('[end]\n', encoding='utf-8')
        (game / 'caesura.project.json').write_text('{"capabilities":{}}\n', encoding='utf-8')
        vite = repo / 'web/node_modules/vite/bin/vite.js'; vite.parent.mkdir(parents=True)
        vite.write_text('// prebuilt player boundary only', encoding='utf-8')
        initialize_fixture_git(repo)
        self.commands = []
        def boundary(report, name, argv, cwd, **kwargs):
            if name == 'web-package':
                self.commands.append((name, list(argv)))
                return REAL_EXECUTE(report, name, argv, cwd, **kwargs)
            return PackageLaneTests.command(self, report, name, argv, cwd, **kwargs)
        work = base / 'outside-lane'
        with mock.patch.object(lane, 'ROOT', repo), mock.patch.object(lane, '_source_identity', return_value=SOURCE), \
             mock.patch.object(lane, '_execute', side_effect=boundary), mock.patch.dict(os.environ, {'NODE_OPTIONS':''}):
            report = lane.run_web_lane(game=game, source_sha=SHA, work_dir=work,
                node_executable=node, lua_executable=lua, browser_executable=Path(sys.executable).resolve())
        error_path = work / 'commands/web-package.stderr.log'
        error = error_path.read_text(encoding='utf-8') if error_path.is_file() else 'Packer was not reached; inspect lane errors.'
        if evidence:
            output = Path(evidence); output.mkdir(parents=True, exist_ok=True)
            for path in (work / 'commands').iterdir():
                shutil.copy2(path, output / path.name)
            shutil.copy2(work / 'lane.json', output / 'lane.json')
            (output / 'real-cli.json').write_text(json.dumps({'fixture':str(base), 'work':str(work),
                'node':str(node), 'node_sha256':digest(node), 'lua':str(lua), 'lua_sha256':digest(lua),
                'player_profile_sha256':digest(player / 'capabilities-build.json'),
                'selected_player':str(selected_player) if selected_player else None,
                'scope':'Actual packer Node/Lua CLI; player is explicitly selected build or self-contained synthetic fixture; bake/Vite and final browser acceptance are fixture boundaries.'}, indent=2), encoding='utf-8')
        self.assertEqual(report['status'], 'PASS', str(report['errors']) + '\n' + error)
        argv = next(args for name, args in self.commands if name == 'web-package')
        stage = Path(argv[argv.index('--out')+1])
        self.assertTrue(stage.is_relative_to(repo))
        self.assertEqual(inspect_inventory(stage), inspect_inventory(report['site']))
        self.assertTrue(Path(report['site']).is_relative_to(work))
        self.assertIn('PACKAGE COMPLETE', (work / 'commands/web-package.stdout.log').read_text(encoding='utf-8'))
        denied = subprocess.run([str(node), str(repo / 'scripts/package_game.mjs'), '--no-web-build',
            '--out', str(base / 'still-forbidden'), str(game)], cwd=repo, capture_output=True,
            text=True, encoding='utf-8', timeout=60, env=dict(os.environ, CAESURA_LUA=str(lua), NODE_OPTIONS=''))
        self.assertNotEqual(denied.returncode, 0)
        self.assertIn('--out must stay inside the repo root', denied.stderr)
        self.assertFalse((base / 'still-forbidden').exists())


if __name__ == '__main__':
    unittest.main()
