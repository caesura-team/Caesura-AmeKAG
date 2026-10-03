"""Real local bundle files test verification; no build/runtime/hosted claims."""
from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from package_verification import inspect_inventory

MODULE = ROOT / 'scripts/verify_package_bundle.py'
bundle = None
if MODULE.is_file():
    spec = importlib.util.spec_from_file_location('package_bundle_contract', MODULE)
    bundle = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(bundle)

SOURCE = '1' * 40
VERSION = '1.2.3'
PRODUCER = dict(provider='github-actions', repository='owner/repo', repository_id=123,
                run_id=456, run_attempt=2, workflow_ref='owner/repo/.github/workflows/ci.yml@refs/heads/main',
                workflow_sha='2' * 40, job_key='release')


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class PackageBundleTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='caesura-bundle-中文-')
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name).resolve()
        self.root = self.base / 'download'; self.root.mkdir()
        self.platform = 'windows'
        self.name = 'Caesura-1.2.3-Windows.zip'
        self.kind = 'file'
        self.make_bundle()

    def save(self, name, value):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        return digest(path)

    def make_bundle(self):
        package = self.root / self.name
        if self.kind == 'directory':
            package.mkdir()
            (package/'index.html').write_text('<!doctype html>synthetic site', encoding='utf-8')
            self.final_sha = inspect_inventory(package)['sha256']
        else:
            package.write_bytes(b'explicit final package byte fixture')
            self.final_sha = digest(package)
        self.required = {self.name:self.kind}
        source = dict(source_sha=SOURCE, dirty=False, worktree_fingerprint='3' * 64)
        prep = dict(status='PREPARED', input_stable=True,
            input=dict(path='/producer/outputs/' + self.name, kind='directory' if self.kind=='directory' else 'archive',
                       archive_sha256=self.final_sha if self.kind=='file' else None),
            expected=dict(archive_sha256=self.final_sha if self.kind=='file' else None,
                          inventory_sha256=self.final_sha if self.kind=='directory' else None))
        if self.kind == 'directory':
            prep['input']['inventory'] = inspect_inventory(package)
        self.receipt = dict(schema='caesura.package-validation.v1', status='PASS', accepted=True,
            diagnostic=False, platform=self.platform, configuration='Release', expected_source_sha=SOURCE,
            source_before=source, source_after=copy.deepcopy(source), errors=[], container=None, container_format=None,
            preparation=prep, static={'status':'STATIC_PASS'}, runtime={'status':'RUNTIME_PASS'},
            stability=dict(status='STABLE', input_stable=True, package_stable=True),
            evidence={'static.json':'4'*64, 'runtime.json':'5'*64})
        self.requirements = dict(schema='caesura.package-build.v1', platform=self.platform,
            configuration='Release', version=VERSION, archive_basename=self.name.removesuffix('.zip'),
            artifacts={'zip':self.name}, required_configuration=dict(schema=1, sdl_linkage='static',
            sdl_libraries=[], ffmpeg=False, steam=False, live2d=False))
        required = None
        if self.platform != 'web':
            required = dict(name='package-requirements.json', sha256=self.save('package-requirements.json',self.requirements))
            self.receipt['requirements'] = dict(path='/producer/build/requirements.json',
                sha256=required['sha256'], value=copy.deepcopy(self.requirements))
        item = dict(name=self.name, kind=self.kind, sha256=self.final_sha)
        self.manifest = dict(schema='caesura.package-upload.v2', source_sha=SOURCE, platform=self.platform,
            configuration='Release', version=VERSION, provenance=dict(PRODUCER, authentication='NOT_VERIFIED'),
            requirements=required, files=[item], validations=[dict(name='validate-final', input=copy.deepcopy(item),
                receipt=dict(name='receipt-final.json', sha256=self.save('receipt-final.json',self.receipt)))])
        self.manifest_sha = self.save('upload-manifest.json',self.manifest)

    def rewrite(self, *, receipt=False, requirements=False):
        if requirements:
            self.manifest['requirements']['sha256'] = self.save('package-requirements.json',self.requirements)
        if receipt:
            self.manifest['validations'][0]['receipt']['sha256'] = self.save('receipt-final.json',self.receipt)
        self.manifest_sha = self.save('upload-manifest.json',self.manifest)

    def verify(self, **extra):
        self.assertIsNotNone(bundle, 'Receiver bundle verification is not implemented')
        options = dict(manifest_sha256=self.manifest_sha, source_sha=SOURCE, platform=self.platform,
            configuration='Release', version=VERSION, required_files=self.required, expected_producer=PRODUCER)
        options.update(extra)
        return bundle.verify_bundle(self.root, **options)

    def test_accepts_original_bytes_and_returns_complete_non_release_locks(self):
        result = self.verify()
        self.assertEqual(result['status'], 'BUNDLE_VERIFIED')
        self.assertIs(result['release_ready'], False)
        self.assertEqual(result['raw_stage_evidence'], 'RAW_STAGE_LOGS_NOT_INCLUDED_NOT_REPLAYED')
        self.assertEqual({Path(lock['path']).name for lock in result['locks']},
                         {self.name,'upload-manifest.json','package-requirements.json','receipt-final.json'})
        self.assertEqual(bundle.verify_bundle_stable(result)['status'], 'BUNDLE_STABLE')

    def test_every_changed_final_byte_is_refused(self):
        (self.root/self.name).write_bytes(b'replaced final package')
        with self.assertRaises(ValueError): self.verify()

    def test_new_manifest_package_digest_cannot_reuse_old_receipt(self):
        (self.root/self.name).write_bytes(b'resigned or recompressed package')
        new_digest = digest(self.root/self.name)
        self.manifest['files'][0]['sha256'] = new_digest
        self.manifest['validations'][0]['input']['sha256'] = new_digest
        self.rewrite()
        with self.assertRaises(ValueError): self.verify()

    def test_manifest_is_externally_locked(self):
        old = self.manifest_sha
        self.manifest['version'] = '9.9.9'; self.rewrite()
        with self.assertRaises(ValueError): self.verify(manifest_sha256=old)

    def test_receipt_source_status_and_stability_are_required(self):
        original = copy.deepcopy(self.receipt)
        changes = [dict(expected_source_sha='f'*40), dict(accepted=False), dict(accepted=1),
                   dict(status='DIAGNOSTIC_PASS'), dict(diagnostic=True), dict(errors=['hidden failure']),
                   dict(runtime={'status':'RUNTIME_FAIL'}), dict(static={'status':'STATIC_FAIL'}),
                   dict(stability={'status':'NOT_RUN'}), dict(configuration='Debug'), dict(platform='linux')]
        for change in changes:
            with self.subTest(change=change):
                self.receipt = dict(copy.deepcopy(original), **change); self.rewrite(receipt=True)
                with self.assertRaises(ValueError): self.verify()

    def test_dirty_changed_or_wrong_source_identity_is_refused(self):
        original = copy.deepcopy(self.receipt)
        for field, value in [('dirty',True), ('dirty',0), ('source_sha','f'*40), ('worktree_fingerprint','changed')]:
            with self.subTest(field=field,value=value):
                self.receipt=copy.deepcopy(original)
                self.receipt['source_after'][field]=value; self.rewrite(receipt=True)
                with self.assertRaises(ValueError): self.verify()

    def test_receipt_observed_and_expected_digests_must_match_final(self):
        original=copy.deepcopy(self.receipt)
        for parent, field in [('expected','archive_sha256'),('input','archive_sha256'),('input','path')]:
            with self.subTest(parent=parent,field=field):
                self.receipt=copy.deepcopy(original)
                self.receipt['preparation'][parent][field]='/other/wrong.zip' if field=='path' else 'f'*64
                self.rewrite(receipt=True)
                with self.assertRaises(ValueError): self.verify()

    def test_required_files_cannot_be_chosen_by_manifest(self):
        for required in ({}, {'another.zip':'file'}, {self.name:'directory'}, {self.name:'file','missing.zip':'file'}):
            with self.subTest(required=required), self.assertRaises(ValueError): self.verify(required_files=required)

    def test_duplicate_missing_and_extra_file_or_validation_declarations_fail(self):
        original=copy.deepcopy(self.manifest)
        for key in ('files','validations'):
            for action in ('remove','duplicate','extra'):
                with self.subTest(key=key,action=action):
                    self.manifest=copy.deepcopy(original)
                    if action=='remove': self.manifest[key]=[]
                    else:
                        item=copy.deepcopy(self.manifest[key][0])
                        if action=='extra': item['name']='unexpected.zip'
                        self.manifest[key].append(item)
                    self.rewrite()
                    with self.assertRaises(ValueError): self.verify()

    def test_receipt_and_requirements_missing_or_modified_fail(self):
        for name in ('receipt-final.json','package-requirements.json'):
            original=(self.root/name).read_bytes()
            for mode in ('missing','changed'):
                with self.subTest(name=name,mode=mode):
                    if mode=='missing': (self.root/name).unlink()
                    else: (self.root/name).write_bytes(original+b' ')
                    with self.assertRaises(ValueError): self.verify()
                    (self.root/name).write_bytes(original)

    def test_version_and_producer_context_must_equal_external_values(self):
        for options in ({'version':'1.2.4'}, {'source_sha':'f'*40}, {'configuration':'Debug'},
                        {'platform':'linux'}, {'expected_producer':dict(PRODUCER,run_attempt=3)},
                        {'expected_producer':dict(PRODUCER,run_id=True)}):
            with self.subTest(options=options), self.assertRaises(ValueError): self.verify(**options)

    def test_requirements_version_platform_and_receipt_binding_are_checked(self):
        original=copy.deepcopy(self.requirements)
        for field,value in [('version','9.9.9'),('platform','linux'),('configuration','Debug')]:
            with self.subTest(field=field):
                self.requirements=dict(original,**{field:value}); self.rewrite(requirements=True)
                with self.assertRaises(ValueError): self.verify()
        self.requirements=original; self.rewrite(requirements=True)
        self.receipt['requirements']['sha256']='f'*64; self.rewrite(receipt=True)
        with self.assertRaises(ValueError): self.verify()

    def test_duplicate_json_keys_nonfinite_and_nonobject_fail(self):
        original=(self.root/'upload-manifest.json').read_bytes()
        for raw in (original.replace(b'"version":', b'"version":"shadow", "version":',1),
                    original.rstrip()[:-1]+b',"extra":NaN}', original.rstrip()[:-1]+b',"extra":1e999}', b'[]'):
            with self.subTest(raw=raw[:40]):
                (self.root/'upload-manifest.json').write_bytes(raw)
                with self.assertRaises(ValueError): self.verify(manifest_sha256=digest(self.root/'upload-manifest.json'))

    def test_duplicate_keys_are_rejected_in_receipt_and_requirements_too(self):
        for name in ('receipt-final.json','package-requirements.json'):
            old=(self.root/name).read_bytes()
            (self.root/name).write_bytes(old.replace(b'"schema":',b'"schema":"shadow","schema":',1))
            target=self.manifest['validations'][0]['receipt'] if name.startswith('receipt') else self.manifest['requirements']
            target['sha256']=digest(self.root/name); self.rewrite()
            with self.assertRaises(ValueError): self.verify()
            (self.root/name).write_bytes(old); target['sha256']=digest(self.root/name)

    def test_json_read_has_a_finite_limit(self):
        self.assertIsNotNone(bundle)
        with patch.object(bundle,'MAX_JSON_BYTES',128):
            with self.assertRaises(ValueError): self.verify()

    def test_unsafe_or_case_colliding_paths_are_rejected(self):
        original=copy.deepcopy(self.manifest)
        for name in ('../evil.zip','/absolute.zip','C:/evil.zip','a\\evil.zip','a//evil.zip','a/./evil.zip','CON.zip','trailing.zip '):
            with self.subTest(name=name):
                self.manifest=copy.deepcopy(original)
                self.manifest['files'][0]['name']=name
                self.manifest['validations'][0]['input']['name']=name; self.rewrite()
                with self.assertRaises(ValueError): self.verify(required_files={name:'file'})
        self.manifest=original; self.rewrite()
        with self.assertRaises(ValueError): self.verify(required_files={self.name:'file',self.name.upper():'file'})

    def test_unlisted_physical_file_and_hard_link_are_rejected(self):
        extra=self.root/'unexpected.txt'; extra.write_text('not declared')
        with self.assertRaises(ValueError): self.verify()
        extra.unlink()
        outside=self.base/'outside-copy'; shutil.copyfile(self.root/self.name,outside)
        (self.root/self.name).unlink(); os.link(outside,self.root/self.name)
        with self.assertRaises(ValueError): self.verify()

    def test_locks_detect_mutation_and_extra_file_before_upload(self):
        result=self.verify()
        extra=self.root/'appeared-after-verification'; extra.write_bytes(b'new')
        with self.assertRaises(ValueError): bundle.verify_bundle_stable(result)
        extra.unlink()
        (self.root/self.name).write_bytes(b'changed after receiver verified')
        with self.assertRaises(ValueError): bundle.verify_bundle_stable(result)

    def test_change_during_receipt_inspection_is_rejected_before_return(self):
        self.assertIsNotNone(bundle)
        original=bundle._receipt
        def alter_after_check(*args):
            original(*args)
            (self.root/'upload-manifest.json').write_bytes(b'changed after parsed')
        with patch.object(bundle,'_receipt',alter_after_check):
            with self.assertRaises(ValueError): self.verify()

    def test_two_final_files_each_need_their_own_matching_receipt(self):
        other='Caesura-1.2.3-Windows.tar.gz'
        (self.root/other).write_bytes(b'second final package fixture')
        self.requirements['artifacts']['tgz']=other
        self.rewrite(requirements=True)
        self.receipt['requirements'].update(value=copy.deepcopy(self.requirements),sha256=self.manifest['requirements']['sha256'])
        self.rewrite(receipt=True)
        second=copy.deepcopy(self.receipt)
        second['preparation']['input'].update(path='/producer/'+other,archive_sha256=digest(self.root/other))
        second['preparation']['expected']['archive_sha256']=digest(self.root/other)
        item=dict(name=other,kind='file',sha256=digest(self.root/other))
        self.manifest['files'].append(item)
        self.manifest['validations'].append(dict(name='validate-tgz',input=copy.deepcopy(item),
            receipt=dict(name='receipt-tgz.json',sha256=self.save('receipt-tgz.json',second))))
        self.required[other]='file'; self.rewrite()
        self.assertEqual(len(self.verify()['files']),2)
        for name in self.required:
            old=(self.root/name).read_bytes(); (self.root/name).write_bytes(b'replaced')
            with self.subTest(name=name), self.assertRaises(ValueError): self.verify()
            (self.root/name).write_bytes(old)

    def test_current_producer_finish_creates_an_interoperable_download(self):
        import ci_package_lane as producer
        work=self.base/'producer'; (work/'outputs').mkdir(parents=True)
        report=dict(work=str(work),source_sha=SOURCE,platform=self.platform,configuration='Release',version=VERSION,
                    source_before=self.receipt['source_before'],provenance=dict(PRODUCER,authentication='NOT_VERIFIED'),locks=[])
        final=producer._lock(report,self.root/self.name)
        report['requirements']=producer._lock(report,self.root/'package-requirements.json')
        report['validations']=[dict(name='validate-final',input=final,receipt=producer._lock(report,self.root/'receipt-final.json'))]
        with patch.object(producer,'_identity',return_value=self.receipt['source_before']):
            producer._finish(report,[final])
        received=self.base/'received'; received.mkdir()
        for name in report['upload_files']:
            shutil.copyfile(name,received/Path(name).name)
        self.root=received; self.manifest_sha=report['manifest']['sha256']
        self.assertEqual(self.verify()['status'],'BUNDLE_VERIFIED')

    def test_web_directory_inventory_is_verified_and_locked(self):
        shutil.rmtree(self.root); self.root.mkdir()
        self.platform='web'; self.name='site'; self.kind='directory'; self.make_bundle()
        result=self.verify()
        self.assertEqual(result['files'][0]['kind'],'directory')
        (self.root/'site/index.html').write_text('different site')
        with self.assertRaises(ValueError): bundle.verify_bundle_stable(result)

    def test_container_receipt_binds_final_container_not_extracted_inventory(self):
        old=self.root/self.name
        self.platform='linux'; self.name='Caesura-1.2.3-Linux.AppImage'
        old.rename(self.root/self.name)
        self.required={self.name:'file'}
        self.manifest['platform']=self.platform
        self.manifest['files'][0]['name']=self.name
        self.manifest['validations'][0]['input']['name']=self.name
        self.requirements.update(platform=self.platform, artifacts={'appimage':self.name})
        self.rewrite(requirements=True)
        self.receipt.update(platform=self.platform,container_format='appimage')
        self.receipt['requirements'].update(value=copy.deepcopy(self.requirements),sha256=self.manifest['requirements']['sha256'])
        self.receipt['container']=dict(status='CONTAINER_PREPARED',format='appimage',expected_sha256=self.final_sha,
            input=dict(path='/producer/'+self.name,sha256_before=self.final_sha,sha256_after=self.final_sha),
            preparation=copy.deepcopy(self.receipt['preparation']))
        self.rewrite(receipt=True)
        self.assertEqual(self.verify()['status'],'BUNDLE_VERIFIED')
        self.receipt['container']['expected_sha256']='f'*64; self.rewrite(receipt=True)
        with self.assertRaises(ValueError): self.verify()

    if os.name != 'nt':
        def test_symlink_file_directory_and_root_are_rejected(self):
            outside=self.base/'outside'; shutil.copyfile(self.root/self.name,outside)
            (self.root/self.name).unlink(); (self.root/self.name).symlink_to(outside)
            with self.assertRaises(ValueError): self.verify()
            (self.root/self.name).unlink(); shutil.copyfile(outside,self.root/self.name)
            alias=self.base/'alias'; alias.symlink_to(self.root,target_is_directory=True)
            original=self.root; self.root=alias
            with self.assertRaises(ValueError): self.verify()
            self.root=original

        def test_link_within_web_directory_is_not_a_valid_inventory_input(self):
            shutil.rmtree(self.root); self.root.mkdir()
            self.platform='web'; self.name='site'; self.kind='directory'; self.make_bundle()
            outside=self.base/'external'; outside.mkdir(); (outside/'file').write_text('external')
            (self.root/'site/linked-dir').symlink_to(outside,target_is_directory=True)
            with self.assertRaises(ValueError): self.verify()


if __name__=='__main__':
    unittest.main(verbosity=2)
