"""Actual ZIP/tar/proof-bundle files; no Engine, browser or hosted acceptance."""
from __future__ import annotations

import copy
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from package_verification import prepare_package
from verify_package_bundle import verify_bundle
try:
    import verify_pages_artifact as pages
except ModuleNotFoundError:
    pages = None

SOURCE = '1' * 40
PRODUCER = dict(provider='github-actions', repository='owner/repo', repository_id=123,
                run_id=456, run_attempt=1, workflow_ref='owner/repo/.github/workflows/ci.yml@main',
                workflow_sha='2' * 40, job_key='release-web')


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class PagesArtifactTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(pages, 'Pages receiver is not implemented')
        temporary = tempfile.TemporaryDirectory(prefix='caesura-pages-中文-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.count = 0

    def fixture(self, *, mode='w', extra_members=()):
        self.count += 1
        base = self.root / str(self.count); base.mkdir()
        proof = base / 'proof'; proof.mkdir()
        tar = proof / 'artifact.tar'
        with tarfile.open(tar, mode) as archive:
            for name, content in [('index.html', b'<!doctype html>Pages fixture'),
                                  ('assets/app.js', b'fixture();'), ('.nojekyll', b'')]:
                member = tarfile.TarInfo(name); member.size = len(content)
                archive.addfile(member, io.BytesIO(content))
            directory = tarfile.TarInfo('empty'); directory.type = tarfile.DIRTYPE
            archive.addfile(directory)
            for name, kind, target in extra_members:
                member = tarfile.TarInfo(name); member.type = kind
                if kind in (tarfile.SYMTYPE, tarfile.LNKTYPE): member.linkname = target
                archive.addfile(member, io.BytesIO(b'') if kind == tarfile.REGTYPE else None)
        with zipfile.ZipFile(proof / 'Web.zip', 'w') as archive:
            archive.writestr('index.html', '<!doctype html>ZIP fixture')
        source = dict(source_sha=SOURCE, dirty=False, worktree_fingerprint='3' * 64)
        manifest = dict(schema='caesura.package-upload.v2', source_sha=SOURCE, platform='web',
                        configuration='Release', version='1.2.3',
                        provenance=dict(PRODUCER, authentication='NOT_VERIFIED'),
                        requirements=None, files=[], validations=[])
        for index, name in enumerate(('Web.zip', 'artifact.tar')):
            final = dict(name=name, kind='file', sha256=sha(proof / name))
            receipt = dict(schema='caesura.package-validation.v1', status='PASS', accepted=True,
                           diagnostic=False, platform='web', configuration='Release',
                           expected_source_sha=SOURCE, source_before=source, source_after=source,
                           errors=[], container=None, container_format=None, requirements=None,
                           preparation=dict(status='PREPARED', input_stable=True,
                               input=dict(path='/producer/' + name, kind='archive', archive_sha256=final['sha256']),
                               expected=dict(archive_sha256=final['sha256'], inventory_sha256=None)),
                           static=dict(status='STATIC_PASS'), runtime=dict(status='RUNTIME_PASS'),
                           stability=dict(status='STABLE', input_stable=True, package_stable=True),
                           evidence={'runtime.json': '4' * 64})
            receipt_name = 'receipt-' + str(index) + '.json'
            (proof / receipt_name).write_text(json.dumps(receipt), encoding='utf-8')
            manifest['files'].append(final)
            manifest['validations'].append(dict(name='validate-' + str(index), input=final,
                receipt=dict(name=receipt_name, sha256=sha(proof / receipt_name))))
        (proof / 'upload-manifest.json').write_text(json.dumps(manifest), encoding='utf-8')
        transport = base / 'actions.zip'
        with zipfile.ZipFile(transport, 'w') as archive:
            archive.write(tar, 'artifact.tar')
        prepared = prepare_package(transport, base / 'outer', expected_sha256=sha(transport))
        return dict(base=base, proof=proof, payload=Path(prepared['package_path']),
                    manifest_sha256=sha(proof / 'upload-manifest.json'))

    def package(self, f):
        return verify_bundle(f['proof'], manifest_sha256=f['manifest_sha256'], source_sha=SOURCE,
            platform='web', configuration='Release', version='1.2.3',
            required_files={'Web.zip': 'file', 'artifact.tar': 'file'}, expected_producer=PRODUCER)

    def call(self, f, **changes):
        options = dict(package_result=self.package(f), manifest_sha256=f['manifest_sha256'],
                       work_dir=f['base'] / 'pages-work')
        options.update(changes)
        return pages.verify_pages_artifact(f['payload'], **options)

    def test_real_zip_tar_and_verified_bundle_bind_exact_bytes(self):
        f = self.fixture(); result = self.call(f)
        self.assertEqual(result['status'], 'PAGES_ARTIFACT_VERIFIED')
        self.assertIs(result['release_ready'], False)
        self.assertEqual(result['tar_sha256'], sha(f['payload'] / 'artifact.tar'))
        extracted = Path(result['prepared']['package_path'])
        self.assertEqual((extracted / 'index.html').read_bytes(), b'<!doctype html>Pages fixture')
        self.assertTrue((extracted / 'empty').is_dir())
        self.assertTrue((extracted / '.nojekyll').is_file())
        self.assertEqual(pages.verify_pages_artifact_stable(result)['status'], 'PAGES_ARTIFACT_STABLE')

    def test_wrong_manifest_platform_or_unverified_result_are_refused(self):
        for change in ({'manifest_sha256': 'f' * 64}, {'platform': 'windows'}, {'status': 'FAIL'}):
            f = self.fixture(); package = self.package(f)
            options = change if 'manifest_sha256' in change else {'package_result': dict(package, **change)}
            with self.subTest(change=change), self.assertRaises((ValueError, RuntimeError)):
                self.call(f, **options)

    def test_missing_or_unaccepted_original_receipt_cannot_supply_package_result(self):
        for missing in (False, True):
            f = self.fixture(); receipt = f['proof'] / 'receipt-1.json'
            if missing: receipt.unlink()
            else:
                data = json.loads(receipt.read_bytes()); data['accepted'] = False
                receipt.write_text(json.dumps(data), encoding='utf-8')
                manifest_path = f['proof'] / 'upload-manifest.json'
                manifest = json.loads(manifest_path.read_bytes())
                manifest['validations'][1]['receipt']['sha256'] = sha(receipt)
                manifest_path.write_text(json.dumps(manifest), encoding='utf-8')
                f['manifest_sha256'] = sha(manifest_path)
            with self.subTest(missing=missing), self.assertRaises((ValueError, RuntimeError)):
                self.call(f)

    def test_replaced_payload_tar_even_with_same_extracted_content_is_refused(self):
        f = self.fixture(); tar = f['payload'] / 'artifact.tar'
        tar.write_bytes(tar.read_bytes() + b'\0' * 512)
        with self.assertRaisesRegex(ValueError, 'digest|bytes|SHA'):
            self.call(f)

    def test_payload_must_have_exactly_one_correct_regular_file(self):
        for mode in ('file', 'directory', 'renamed'):
            f = self.fixture()
            if mode == 'file': (f['payload'] / 'extra').write_bytes(b'wrong')
            elif mode == 'directory': (f['payload'] / 'empty').mkdir()
            else: (f['payload'] / 'artifact.tar').rename(f['payload'] / 'Artifact.tar')
            with self.subTest(mode=mode), self.assertRaises(ValueError): self.call(f)

    def test_physical_hardlink_and_linked_payload_root_are_refused(self):
        f = self.fixture(); os.link(f['payload'] / 'artifact.tar', f['base'] / 'alias.tar')
        with self.assertRaises(ValueError): self.call(f)
        f = self.fixture(); alias = f['base'] / 'linked-payload'
        if os.name == 'nt':
            subprocess.run(['cmd', '/c', 'mklink', '/J', str(alias), str(f['payload'])],
                           check=True, capture_output=True)
        else: alias.symlink_to(f['payload'], target_is_directory=True)
        f['payload'] = alias
        with self.assertRaises(ValueError): self.call(f)

    def test_tar_links_and_special_members_are_refused_even_for_internal_targets(self):
        for kind in (tarfile.SYMTYPE, tarfile.LNKTYPE, tarfile.FIFOTYPE, tarfile.CHRTYPE, tarfile.BLKTYPE):
            f = self.fixture(extra_members=[('member', kind, 'index.html')])
            with self.subTest(kind=kind), self.assertRaises(ValueError): self.call(f)

    def test_compressed_tar_is_not_plain_pages_tar(self):
        for mode in ('w:gz', 'w:bz2', 'w:xz'):
            f = self.fixture(mode=mode)
            with self.subTest(mode=mode), self.assertRaises(ValueError): self.call(f)

    def test_tar_size_and_member_count_limits_are_strict(self):
        f = self.fixture(); size = (f['payload'] / 'artifact.tar').stat().st_size
        # Scale the same physical boundary to real small bytes; no huge allocation.
        with patch.object(pages, 'MAX_PAGES_BYTES', size), self.assertRaisesRegex(ValueError, 'smaller'):
            self.call(f)
        f = self.fixture()
        with patch.object(pages, 'MAX_PAGES_BYTES', size + 1): self.call(f)
        f = self.fixture()
        with patch.object(pages, 'MAX_ENTRIES', 3), self.assertRaisesRegex(ValueError, 'count'):
            self.call(f)

    def test_oversized_declared_member_cannot_hide_in_a_short_tar(self):
        f = self.fixture()
        member = tarfile.TarInfo('index.html'); member.size = pages.MAX_PAGES_BYTES
        raw = member.tobuf() + b'\0' * 1024
        for root in (f['proof'], f['payload']): (root / 'artifact.tar').write_bytes(raw)
        digest = sha(f['proof'] / 'artifact.tar')
        receipt_path = f['proof'] / 'receipt-1.json'; receipt = json.loads(receipt_path.read_bytes())
        for part in ('input', 'expected'): receipt['preparation'][part]['archive_sha256'] = digest
        receipt_path.write_text(json.dumps(receipt), encoding='utf-8')
        manifest_path = f['proof'] / 'upload-manifest.json'; manifest = json.loads(manifest_path.read_bytes())
        manifest['files'][1]['sha256'] = manifest['validations'][1]['input']['sha256'] = digest
        manifest['validations'][1]['receipt']['sha256'] = sha(receipt_path)
        manifest_path.write_text(json.dumps(manifest), encoding='utf-8'); f['manifest_sha256'] = sha(manifest_path)
        with self.assertRaisesRegex(ValueError, 'member exceeds size'): self.call(f)

    def test_generic_path_and_duplicate_safety_is_reused(self):
        for name in ('../escape', '/absolute', 'index.html', 'INDEX.HTML'):
            f = self.fixture(extra_members=[(name, tarfile.REGTYPE, '')])
            with self.subTest(name=name), self.assertRaises((ValueError, RuntimeError)): self.call(f)
            self.assertFalse((f['base'] / 'escape').exists())

    def test_bundle_tar_must_be_one_final_file_and_one_matching_lock(self):
        for mutation in ('missing', 'duplicate', 'directory', 'lock-mismatch'):
            f = self.fixture(); package = self.package(f)
            tar = next(x for x in package['files'] if x['name'] == 'artifact.tar')
            if mutation == 'missing': package['files'].remove(tar)
            elif mutation == 'duplicate': package['files'].append(copy.deepcopy(tar))
            elif mutation == 'directory': tar['kind'] = 'directory'
            else: tar['sha256'] = 'f' * 64
            with self.subTest(mutation=mutation), self.assertRaises((ValueError, RuntimeError)):
                self.call(f, package_result=package)

    def test_late_mutation_of_either_tar_receipt_or_prepared_bytes_is_refused(self):
        for target in ('payload-tar', 'proof-tar', 'receipt', 'prepared', 'extra-file', 'extra-directory'):
            f = self.fixture(); result = self.call(f)
            paths = {'payload-tar': f['payload'] / 'artifact.tar', 'proof-tar': f['proof'] / 'artifact.tar',
                     'receipt': f['proof'] / 'receipt-1.json',
                     'prepared': Path(result['prepared']['package_path']) / 'index.html'}
            if target == 'extra-file': (f['payload'] / 'unexpected').write_bytes(b'added')
            elif target == 'extra-directory': (f['payload'] / 'unexpected').mkdir()
            else: paths[target].write_bytes(paths[target].read_bytes() + b'changed')
            with self.subTest(target=target), self.assertRaises((ValueError, RuntimeError)):
                pages.verify_pages_artifact_stable(result)

    def test_existing_work_and_work_inside_inputs_are_refused_without_mutation(self):
        f = self.fixture(); work = f['base'] / 'existing'; work.mkdir()
        (work / 'sentinel').write_bytes(b'keep')
        for bad in (work, f['payload'] / 'new-work', f['proof'] / 'new-work', ROOT / 'forbidden-pages-work'):
            with self.subTest(work=bad), self.assertRaises((ValueError, RuntimeError, FileExistsError)):
                self.call(f, work_dir=bad)
        self.assertEqual((work / 'sentinel').read_bytes(), b'keep')
        self.assertEqual([p.name for p in f['payload'].iterdir()], ['artifact.tar'])
        self.assertFalse((f['proof'] / 'new-work').exists())
        self.assertFalse((ROOT / 'forbidden-pages-work').exists())

    def test_failed_attempt_is_retained_and_cannot_be_reused(self):
        f = self.fixture(); (f['payload'] / 'extra').write_bytes(b'wrong')
        with self.assertRaises(ValueError): self.call(f)
        receipt = f['base'] / 'pages-work/pages.json'; before = receipt.read_bytes()
        self.assertEqual(json.loads(before)['status'], 'FAIL')
        with self.assertRaises((ValueError, RuntimeError, FileExistsError)): self.call(f)
        self.assertEqual(receipt.read_bytes(), before)


if __name__ == '__main__':
    unittest.main(verbosity=2)
