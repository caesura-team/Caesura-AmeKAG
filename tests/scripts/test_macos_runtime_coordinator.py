"""Coordinator filesystem/owned-tool fixtures, not Apple signatures or ABI.

Synthetic image bytes are actually copied/edited/read by production parsers.
Only the Apple executable boundary is replaced with an owned Python process.
Positive results MUST be FIXTURE_ONLY. Root preserves the first actual RED.
"""
from __future__ import annotations
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import sys
import tempfile
import unittest
from unittest.mock import patch
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import install_macos_runtime as coordinator
import macos_runtime_selection as selector
from macho_dependencies import inspect_macho, inspect_package_closure
from package_runtime import run_runtime_command


def load_fixture(name, relative):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


binary = load_fixture('mac_install_binary_fixture', 'tests/scripts/test_native_package_contract.py')
cmake_fixture = load_fixture('mac_install_cmake_fixture', 'tests/scripts/test_package_runtime_install.py')


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_json(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=True, indent=2) + '\n', encoding='utf-8')


def reference(path):
    return {'path': str(Path(path).resolve(strict=True)), 'sha256': digest(path)}


def archive(marker):
    data = b'synthetic object ' + marker.encode('ascii')
    header = ('fixture.o/'.ljust(16) + '0'.ljust(12) + '0'.ljust(6)
              + '0'.ljust(6) + '100644'.ljust(8) + str(len(data)).ljust(10)
              + '`\n').encode('ascii')
    return b'!<arch>\n' + header + data + (b'\n' if len(data) % 2 else b'')


# Deliberately small format-fixture editor. This is NOT install_name_tool or a
# signature implementation. Appended markers are intentionally not LC_CODE_SIGNATURE.
TOOL_FIXTURE = r'''
import hashlib, json, os, pathlib, struct, sys
config = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding='utf-8'))
a = sys.argv[2:]
trace = pathlib.Path(config['trace'])
old = json.loads(trace.read_text(encoding='utf-8')) if trace.exists() else []
entry = {'argv':a, 'ordinal':len(old), 'evidence_kind':'FIXTURE_ONLY'}
old.append(entry); trace.write_text(json.dumps(old),encoding='utf-8')
if config.get('fail_at') == entry['ordinal']:
    print('fixture injected failure at ' + str(entry['ordinal']), file=sys.stderr)
    sys.exit(37)
p = pathlib.Path(a[-1])
if a[0] in ('-change', '-id'):
    if config.get('noop_edits'):
        sys.exit(0)
    raw = p.read_bytes(); header = list(struct.unpack('<8I', raw[:32]))
    end = 32 + header[5]; pos = 32; commands = []; changed = 0
    for _ in range(header[4]):
        cmd, size = struct.unpack_from('<II', raw, pos)
        part = raw[pos:pos+size]
        if cmd in (12, 13, 0x80000018, 0x8000001f, 0x80000023, 0x20):
            offset = struct.unpack_from('<I', part, 8)[0]
            value = part[offset:part.index(b'\0', offset)].decode()
            if a[0] == '-id' and cmd == 13 or a[0] == '-change' and cmd != 13 and value == a[1]:
                new = a[1] if a[0] == '-id' else a[2]
                text = new.encode() + b'\0'; new_size = (offset + len(text) + 7) & ~7
                part = bytearray(part[:offset] + text.ljust(new_size-offset, b'\0'))
                struct.pack_into('<I', part, 4, new_size); part = bytes(part); changed += 1
        commands.append(part); pos += size
    if not changed:
        print('fixture received an edit with no exact original command', file=sys.stderr); sys.exit(38)
    body = b''.join(commands); header[5] = len(body)
    p.write_bytes(struct.pack('<8I', *header) + body + raw[end:])
elif a[:3] == ['--force','--sign','-']:
    p.write_bytes(p.read_bytes() + b'\nFIXTURE_ONLY_NOT_A_SIGNATURE\n')
elif a[:2] == ['--verify','--strict']:
    if b'FIXTURE_ONLY_NOT_A_SIGNATURE' not in p.read_bytes():
        print('fixture missing sign marker', file=sys.stderr); sys.exit(39)
else:
    print('unexpected fixture argv', file=sys.stderr); sys.exit(40)
print(json.dumps(entry))
'''


class MacRuntimeCoordinatorTests(unittest.TestCase):
    def setUp(self):
        evidence = os.environ.get('CAESURA_MACOS_COORDINATOR_EVIDENCE')
        if evidence:
            self.root = Path(evidence).resolve() / (self._testMethodName + '-' + uuid.uuid4().hex)
            self.root.mkdir(parents=True)
        else:
            temporary = tempfile.TemporaryDirectory(prefix='caesura mac coordinator ')
            self.addCleanup(temporary.cleanup)
            self.root = Path(temporary.name).resolve()
        self.calls = []
        self.held = []
        self.tool_script = self.root / 'fixture-tool.py'
        self.tool_script.write_text(TOOL_FIXTURE, encoding='utf-8')
        self.tool_config = self.root / 'fixture-tool.json'
        self.trace = self.root / 'fixture-tool-trace.json'
        self.options = {'trace': str(self.trace)}
        write_json(self.tool_config, self.options)
        self.before_tool = None

    def fixture(self, *, linkage='shared', engine_extra=(), ssl_extra=(), wrong_cpu=False):
        root = self.root / ('fixture-' + uuid.uuid4().hex); root.mkdir()
        sdk = root / 'sdk original readonly'; sdk.mkdir()
        build = root / 'build original readonly'; build.mkdir()
        stage = root / 'stage 中文'; stage.mkdir()
        ssl, crypto = sdk / 'selected-ssl.dylib', sdk / 'selected-crypto.dylib'
        ssl_id, crypto_id = '/opt/selected/openssl/libssl.3.dylib', '/opt/selected/openssl/libcrypto.3.dylib'
        if linkage == 'shared':
            ssl.write_bytes(binary.macho_fixture([
                binary.macho_dylib_command(ssl_id, 13), binary.macho_dylib_command(crypto_id),
                binary.macho_dylib_command('/usr/lib/libSystem.B.dylib'),
                *(binary.macho_dylib_command(name) for name in ssl_extra)], file_type=6))
            crypto.write_bytes(binary.macho_fixture([
                binary.macho_dylib_command(crypto_id, 13),
                binary.macho_dylib_command('/usr/lib/libSystem.B.dylib')], file_type=6))
        else:
            ssl.write_bytes(archive('SSL')); crypto.write_bytes(archive('Crypto'))
        selected = selector.select_openssl(ssl, crypto, 'Release')
        selection = root / 'selection.json'; write_json(selection, selected)
        commands = [binary.macho_rpath_command('@loader_path'), binary.macho_dylib_command('@rpath/libSDL3.0.dylib')]
        if linkage == 'shared':
            commands += [binary.macho_dylib_command(ssl_id), binary.macho_dylib_command(crypto_id)]
        commands += [binary.macho_dylib_command(name) for name in engine_extra]
        engine_bytes = binary.macho_fixture(commands)
        if wrong_cpu:
            engine_bytes = engine_bytes[:4] + (0x01000007).to_bytes(4, 'little') + engine_bytes[8:]
        (build / 'Engine').write_bytes(engine_bytes)
        shutil.copyfile(build / 'Engine', stage / 'Engine')
        sdl = stage / 'libSDL3.0.dylib'
        sdl.write_bytes(binary.macho_fixture([
            binary.macho_dylib_command('@rpath/libSDL3.0.dylib', 13),
            binary.macho_dylib_command('/usr/lib/libSystem.B.dylib')], file_type=6))
        metadata = root / 'package-requirements.json'
        write_json(metadata, {'schema':'caesura.package-build.v1', 'platform':'macos',
            'configuration':'Release', 'version':'1.2.3', 'archive_basename':'Fixture',
            'engine_relative_path':'Engine', 'lua_relative_path':'external/lua/lua',
            'artifacts': {'tgz':'Fixture.tar.gz','dmg':'Fixture.dmg','zip':'Fixture.zip','appimage':'Fixture.AppImage'},
            'required_configuration':binary.config('macos', openssl_linkage=linkage,
                openssl_libraries=selected['libraries'])})
        request = {'schema':'caesura.macos-runtime-install-request.v1',
            'configuration':'Release', 'stage_root':str(stage),
            'selection':reference(selection), 'requirements':reference(metadata),
            'build_engine':reference(build / 'Engine'),
            'tools':{kind:reference(Path(sys.executable).resolve()) for kind in ('install_name_tool','codesign')},
            'previous_install':None}
        request_path = root / 'request.json'; write_json(request_path, request)
        originals = {str(p):digest(p) for p in (ssl, crypto, build / 'Engine', selection, metadata, sdl)}
        self.current_sources = [ssl, crypto]
        return {'root':root, 'sdk':sdk, 'build':build, 'stage':stage,
                'selection':selection, 'selected':selected, 'metadata':metadata,
                'request':request, 'request_path':request_path, 'originals':originals,
                'ssl_id':ssl_id, 'crypto_id':crypto_id}

    def rewrite_request(self, f):
        write_json(f['request_path'], f['request'])

    def runner(self, argv, cwd, env, control_dir, stdout, stderr, timeout, **kwargs):
        args = [str(x) for x in argv]
        self.calls.append(args)
        held = []
        for source in self.current_sources:
            expected = source.stat()
            found = False
            # Actual live Python file descriptors, not a production counter.
            for fd in range(256):
                try:
                    observed = os.fstat(fd)
                    found |= (observed.st_dev, observed.st_ino) == (expected.st_dev, expected.st_ino)
                except OSError:
                    pass
            held.append(found)
        self.held.append(held)
        if self.before_tool:
            self.before_tool(len(self.calls)-1, args)
        write_json(self.tool_config, self.options)
        child_env = dict(env); child_env['PYTHONDONTWRITEBYTECODE']='1'
        return run_runtime_command([str(Path(sys.executable).resolve()), '-B', str(self.tool_script),
                str(self.tool_config), *args[1:]], cwd, child_env, control_dir,
                stdout, stderr, timeout, **kwargs)

    def run_case(self, f, *, success, previous=None):
        if previous is not None:
            f['request']['previous_install'] = reference(previous)
            self.rewrite_request(f)
        report_dir = f['root'] / ('report-' + uuid.uuid4().hex)
        result = coordinator.install_macos_runtime(f['request_path'],
            request_sha256=digest(f['request_path']), report_dir=report_dir, runner=self.runner)
        self.assertEqual(result.get('success'), success, result)
        self.assertEqual(result.get('status'), 'FIXTURE_ONLY' if success else 'TAINTED_NOT_ACCEPTED', result)
        self.assertEqual(result.get('evidence_kind'), 'FIXTURE_ONLY')
        receipt = report_dir / 'receipt.json'
        self.assertTrue(receipt.is_file(), 'Every safe attempt retains its actual receipt')
        self.assertEqual(json.loads(receipt.read_bytes()), result)
        for name, expected in f['originals'].items():
            self.assertEqual(digest(name), expected, 'Readonly input changed: ' + name)
        for command in result.get('commands', []):
            run = command['runtime']
            self.assertEqual(run['owned_tree_cleanup'], 'COMPLETE')
            self.assertFalse(run['timed_out']); self.assertFalse(run['forced_kill'])
        if not success:
            self.assertTrue(result.get('error'))
        return result, receipt

    def test_shared_exact_edits_then_signatures_with_live_source_descriptors(self):
        f = self.fixture(); result, _ = self.run_case(f, success=True)
        expected = {
            ('-change', f['ssl_id'], '@loader_path/libssl.3.dylib', str(f['stage']/'Engine')),
            ('-change', f['crypto_id'], '@loader_path/libcrypto.3.dylib', str(f['stage']/'Engine')),
            ('-change', f['crypto_id'], '@loader_path/libcrypto.3.dylib', str(f['stage']/'libssl.3.dylib')),
            ('-id', '@rpath/libssl.3.dylib', str(f['stage']/'libssl.3.dylib')),
            ('-id', '@rpath/libcrypto.3.dylib', str(f['stage']/'libcrypto.3.dylib'))}
        edits = [tuple(a[1:]) for a in self.calls if a[1] in ('-change','-id')]
        self.assertEqual(set(edits), expected); self.assertEqual(len(edits), 5)
        first_sign = next(i for i,a in enumerate(self.calls) if a[1] == '--force')
        self.assertTrue(all(i < first_sign for i,a in enumerate(self.calls) if a[1] in ('-change','-id')))
        signed = [Path(a[-1]).name for a in self.calls if a[1:4] == ['--force','--sign','-']]
        verified = [Path(a[-1]).name for a in self.calls if a[1:3] == ['--verify','--strict']]
        self.assertEqual(signed, ['libssl.3.dylib','libcrypto.3.dylib','Engine'])
        self.assertEqual(verified, signed)
        self.assertEqual(self.held, [[True,True]]*len(self.calls))
        closure = inspect_package_closure(f['stage'], [f['stage']/'Engine'],
            [f['stage']/n for n in ('libSDL3.0.dylib','libssl.3.dylib','libcrypto.3.dylib')])
        self.assertEqual(closure['status'], 'MACHO_DEPENDENCY_CLOSURE_VERIFIED')
        final = {p['relative_path']:p for p in result['final_files']}
        self.assertEqual(set(final), {'Engine','libSDL3.0.dylib','libssl.3.dylib','libcrypto.3.dylib'})
        for name, item in final.items():
            self.assertEqual(item['sha256'], digest(f['stage']/name))
        for component in f['selected']['components']:
            dest = f['stage']/component['runtime_name']
            self.assertFalse(os.path.samefile(dest, component['resolved_path']))
            self.assertTrue(stat.S_ISREG(dest.lstat().st_mode)); self.assertEqual(dest.stat().st_nlink, 1)

    def test_static_still_signs_engine_and_verifies_closure(self):
        f = self.fixture(linkage='static'); self.run_case(f, success=True)
        self.assertEqual([a[1:] for a in self.calls], [
            ['--force','--sign','-',str(f['stage']/'Engine')],
            ['--verify','--strict',str(f['stage']/'Engine')]])
        self.assertEqual(sorted(p.name for p in f['stage'].iterdir()), ['Engine','libSDL3.0.dylib'])
        self.assertEqual(self.held, [[True,True]]*2)

    def test_bad_request_selection_metadata_and_tool_locks_reject_before_mutation(self):
        changes = ('config', 'selection-sha', 'tool-sha', 'metadata-sha', 'unknown-key',
                   'component-order', 'component-name', 'component-size', 'mixed-linkage')
        for change in changes:
            with self.subTest(change=change):
                f = self.fixture(); initial = {p.name:digest(p) for p in f['stage'].iterdir()}
                if change == 'config': f['request']['configuration']='Debug'
                elif change == 'selection-sha': f['request']['selection']['sha256']='0'*64
                elif change == 'tool-sha': f['request']['tools']['codesign']['sha256']='0'*64
                elif change == 'metadata-sha': f['request']['requirements']['sha256']='0'*64
                elif change == 'unknown-key': f['request']['skip_sign']=True
                else:
                    value = copy.deepcopy(f['selected'])
                    if change == 'component-order': value['components'].reverse()
                    if change == 'component-name': value['components'][0]['runtime_name']='unselected.dylib'
                    if change == 'component-size': value['components'][0]['size']+=1
                    if change == 'mixed-linkage': value['components'][0]['linkage']='static'
                    write_json(f['selection'], value)
                    f['request']['selection']=reference(f['selection'])
                    f['originals'][str(f['selection'])]=digest(f['selection'])
                self.rewrite_request(f); calls=len(self.calls)
                self.run_case(f, success=False)
                self.assertEqual(len(self.calls), calls)
                self.assertEqual({p.name:digest(p) for p in f['stage'].iterdir()}, initial)

    def test_wrong_dependency_identity_external_edge_and_cpu_reject_before_tools(self):
        cases = ({'engine_extra':['/unrelated/libssl.3.dylib']},
                 {'ssl_extra':['/opt/unselected/libother.dylib']}, {'wrong_cpu':True})
        for options in cases:
            with self.subTest(options=options):
                f=self.fixture(**options); calls=len(self.calls)
                self.run_case(f, success=False)
                self.assertEqual(len(self.calls), calls)

    def test_existing_original_bytes_allowed_but_unknown_or_hardlinked_target_refused(self):
        f=self.fixture()
        for item in f['selected']['components']:
            shutil.copyfile(item['resolved_path'], f['stage']/item['runtime_name'])
        self.run_case(f, success=True)
        for kind in ('unknown','hardlink'):
            with self.subTest(kind=kind):
                f=self.fixture(); dest=f['stage']/'libssl.3.dylib'
                if kind=='unknown': dest.write_bytes(b'unrelated existing payload')
                else: os.link(f['selected']['components'][0]['resolved_path'],dest)
                old=digest(dest); calls=len(self.calls)
                self.run_case(f,success=False)
                self.assertEqual(digest(dest),old); self.assertEqual(len(self.calls),calls)

    def test_finalized_targets_require_exact_previous_receipt_and_tamper_refused(self):
        f=self.fixture(); first, receipt=self.run_case(f,success=True)
        calls=len(self.calls); hashes={p.name:digest(p) for p in f['stage'].iterdir()}
        self.run_case(f,success=False)
        self.assertEqual(len(self.calls),calls)
        self.assertEqual({p.name:digest(p) for p in f['stage'].iterdir()},hashes)
        self.run_case(f,success=True,previous=receipt)
        bad=json.loads(receipt.read_bytes()); bad['final_files'][0]['sha256']='0'*64
        tampered=f['root']/'tampered-receipt.json'; write_json(tampered,bad)
        calls=len(self.calls); self.run_case(f,success=False,previous=tampered)
        self.assertEqual(len(self.calls),calls)

    def test_each_actual_tool_failure_stops_and_preserves_original_exit(self):
        # First establish positive exact plan, then fail each real child launch.
        f=self.fixture(); self.run_case(f,success=True)
        count=len(self.calls)
        for fail_at in range(count):
            with self.subTest(fail_at=fail_at):
                f=self.fixture(); self.calls=[]; self.held=[]
                self.trace=self.root/f'trace-failure-{fail_at}.json'
                self.options={'trace':str(self.trace),'fail_at':fail_at}
                result,_=self.run_case(f,success=False)
                self.assertEqual(len(self.calls),fail_at+1)
                self.assertEqual(result['commands'][-1]['runtime']['actual_exit_code'],37)
                self.assertEqual(len(json.loads(self.trace.read_bytes())),fail_at+1)

    def test_zero_exit_without_actual_transforms_cannot_be_accepted(self):
        f=self.fixture(); self.options['noop_edits']=True
        result,_=self.run_case(f,success=False)
        self.assertGreater(len(self.calls),0)
        self.assertTrue(result['error'])
        self.assertTrue(any(d['name'].startswith('/opt/') for d in inspect_macho(f['stage']/'Engine')['dependencies']))

    def test_sdk_change_after_copy_refuses_acceptance_and_never_changes_build_engine(self):
        f=self.fixture(); target=Path(f['selected']['components'][0]['resolved_path'])
        before=f['originals'].pop(str(target))
        def mutate(index, args):
            if index==0: target.write_bytes(target.read_bytes()+b' external mutation')
        self.before_tool=mutate
        self.run_case(f,success=False)
        self.assertNotEqual(digest(target),before)

    def test_source_change_during_held_stream_copy_is_refused(self):
        f=self.fixture(); target=Path(f['selected']['components'][0]['resolved_path'])
        original=f['originals'].pop(str(target)); actual_open=selector._open_regular
        actual_os_open=os.open; armed=[]; changed=[]
        stage_identity=f['stage'].stat()
        def observe_create(path, flags, *args, **kwargs):
            fd=actual_os_open(path,flags,*args,**kwargs)
            parent_is_stage=False
            if isinstance(path,(str,bytes,os.PathLike)):
                parent_is_stage=Path(os.fsdecode(path)).parent==f['stage']
            if kwargs.get('dir_fd') is not None:
                parent=os.fstat(kwargs['dir_fd'])
                parent_is_stage |= (parent.st_dev,parent.st_ino)==(stage_identity.st_dev,stage_identity.st_ino)
            if flags & os.O_CREAT and flags & os.O_EXCL and parent_is_stage:
                armed.append(True)
            return fd
        class ObservedStream:
            def __init__(self, stream): self.stream=stream
            def __getattr__(self,name): return getattr(self.stream,name)
            def __enter__(self): self.stream.__enter__(); return self
            def __exit__(self,*args): return self.stream.__exit__(*args)
            def mutate(self):
                if armed and not changed:
                    with target.open('ab') as writer: writer.write(b' concurrent source mutation')
                    changed.append(True)
            def read(self,*args):
                data=self.stream.read(*args); self.mutate(); return data
            def readinto(self,*args):
                count=self.stream.readinto(*args); self.mutate(); return count
        def observe_source(path,expected):
            stream,identity=actual_open(path,expected)
            return (ObservedStream(stream) if Path(path)==target else stream),identity
        with patch.object(os,'open',side_effect=observe_create), patch.object(selector,'_open_regular',side_effect=observe_source):
            self.run_case(f,success=False)
        self.assertTrue(armed,'The real exclusive stage copy was reached')
        self.assertEqual(changed,[True],'Mutation happened during the actual held-stream copy')
        self.assertNotEqual(digest(target),original)
        self.assertEqual(self.calls,[],'Copy identity failure must precede Apple tools')

    def test_unsafe_or_existing_report_directories_are_not_written(self):
        for kind in ('payload','sdk','build','existing'):
            with self.subTest(kind=kind):
                f=self.fixture()
                parent={'payload':f['stage'],'sdk':f['sdk'],'build':f['build'],'existing':f['root']}[kind]
                report=parent/'diagnostics'
                if kind=='existing': report.mkdir(); (report/'sentinel').write_bytes(b'keep')
                before={str(p):digest(p) for p in f['root'].rglob('*') if p.is_file()}
                with self.assertRaises((ValueError,OSError)):
                    coordinator.install_macos_runtime(f['request_path'],request_sha256=digest(f['request_path']),
                        report_dir=report,runner=self.runner)
                after={str(p):digest(p) for p in f['root'].rglob('*') if p.is_file()}
                self.assertEqual(after,before)
                if kind!='existing': self.assertFalse(report.exists())

    def test_production_cli_has_no_fixture_bypass_and_refuses_non_darwin(self):
        if sys.platform=='darwin':
            # This fixture passes Python as its Apple tools; even Darwin must
            # refuse it, never execute Python with install_name_tool arguments.
            expected='invalid Apple tool identities'
        else: expected='non-Darwin host'
        f=self.fixture(); control=f['root']/'cli-owned'; out=f['root']/'cli.stdout'; err=f['root']/'cli.stderr'
        with out.open('wb') as stdout, err.open('wb') as stderr:
            result=run_runtime_command([str(Path(sys.executable).resolve()),'-B',str(ROOT/'scripts/install_macos_runtime.py'),
                '--request',str(f['request_path']),'--request-sha256',digest(f['request_path']),
                '--report-dir',str(f['root']/'cli-report')],f['root'],dict(os.environ),control,stdout,stderr,20)
        self.assertNotEqual(result['actual_exit_code'],0,expected)
        self.assertEqual(result['owned_tree_cleanup'],'COMPLETE')
        self.assertEqual(sorted(p.name for p in f['stage'].iterdir()),['Engine','libSDL3.0.dylib'])
        with out.with_suffix('.bypass').open('wb') as stdout, err.with_suffix('.bypass').open('wb') as stderr:
            bypass=run_runtime_command([str(Path(sys.executable).resolve()),'-B',str(ROOT/'scripts/install_macos_runtime.py'),
                '--fixture-success'],f['root'],dict(os.environ),f['root']/'bypass-owned',stdout,stderr,20)
        self.assertNotEqual(bypass['actual_exit_code'],0)

    def test_posix_destination_symlink_is_never_followed(self):
        f=self.fixture(); outside=f['root']/'outside'; outside.write_bytes(b'unchanged outside')
        (f['stage']/'libssl.3.dylib').symlink_to(outside)
        self.run_case(f,success=False)
        self.assertEqual(outside.read_bytes(),b'unchanged outside'); self.assertEqual(self.calls,[])

    def test_posix_stage_alias_and_hardlinked_engine_are_refused(self):
        for kind in ('stage-alias','engine-hardlink'):
            with self.subTest(kind=kind):
                f=self.fixture(); before={p.name:digest(p) for p in f['stage'].iterdir()}
                if kind=='stage-alias':
                    alias=f['root']/'stage-alias'; alias.symlink_to(f['stage'],target_is_directory=True)
                    f['request']['stage_root']=str(alias)
                else:
                    (f['stage']/'Engine').unlink()
                    os.link(f['build']/'Engine',f['stage']/'Engine')
                self.rewrite_request(f)
                report=f['root']/('rejected-'+uuid.uuid4().hex)
                try:
                    result=coordinator.install_macos_runtime(f['request_path'],request_sha256=digest(f['request_path']),
                        report_dir=report,runner=self.runner)
                except (ValueError,OSError):
                    result={'success':False}
                self.assertFalse(result.get('success',True))
                self.assertEqual({p.name:digest(p) for p in f['stage'].iterdir()},before)
                self.assertEqual(self.calls,[])

    def test_posix_selected_alias_swap_after_descriptor_open_is_refused(self):
        f=self.fixture(); original=Path(f['selected']['components'][0]['resolved_path'])
        alias=f['sdk']/'ssl-alias'; alias.symlink_to(original.name)
        selected=selector.select_openssl(alias,f['selected']['components'][1]['selected_path'],'Release')
        f['selected']=selected; write_json(f['selection'],selected)
        f['request']['selection']=reference(f['selection']); self.rewrite_request(f)
        f['originals'][str(f['selection'])]=digest(f['selection'])
        other=f['sdk']/'other'; other.write_bytes(original.read_bytes()+b' alien')
        actual_open=selector._open_regular; changed=[]
        def swap(path, expected):
            answer=actual_open(path,expected)
            if Path(path)==original and not changed:
                alias.unlink(); alias.symlink_to(other.name); changed.append(True)
            return answer
        with patch.object(selector,'_open_regular',side_effect=swap):
            self.run_case(f,success=False)
        self.assertEqual(changed,[True]); self.assertEqual(self.calls,[])
        self.assertEqual(digest(original),f['originals'][str(original)])


    def test_previous_receipt_rejects_same_path_and_bytes_in_new_directory(self):
        f = self.fixture()
        _, receipt = self.run_case(f, success=True)
        original = f['stage'].stat()
        retained = f['root'] / 'retained-original-stage'
        self.assertEqual(retained.parent, f['stage'].parent)
        f['stage'].rename(retained)
        shutil.copytree(retained, f['stage'])
        replacement = f['stage'].stat()
        self.assertNotEqual((original.st_dev, original.st_ino),
                            (replacement.st_dev, replacement.st_ino))
        before = {p.name: digest(p) for p in f['stage'].iterdir()}
        calls = len(self.calls)
        result, _ = self.run_case(f, success=False, previous=receipt)
        self.assertIn('physical install directory', result['error'])
        self.assertEqual(len(self.calls), calls)
        self.assertEqual({p.name: digest(p) for p in f['stage'].iterdir()}, before)
        self.assertEqual({p.name: digest(p) for p in retained.iterdir()}, before)

    def test_source_same_bytes_replacement_after_selection_rejects_before_tools(self):
        f = self.fixture()
        target = Path(f['selected']['components'][0]['resolved_path'])
        retained = target.with_name('retained-original-ssl')
        self.assertEqual(retained.parent, f['sdk'])
        real_select = selector.select_openssl
        changed = []

        def replace_after_inspection(*args):
            result = real_select(*args)
            original = target.stat()
            target.rename(retained)
            target.write_bytes(retained.read_bytes())
            replacement = target.stat()
            self.assertNotEqual((original.st_dev, original.st_ino),
                                (replacement.st_dev, replacement.st_ino))
            self.assertEqual(digest(target), digest(retained))
            changed.append(True)
            return result

        before = {p.name: digest(p) for p in f['stage'].iterdir()}
        with patch.object(selector, 'select_openssl', side_effect=replace_after_inspection):
            result, _ = self.run_case(f, success=False)
        self.assertEqual(changed, [True])
        self.assertIn('Held source route differs', result['error'])
        self.assertEqual(self.calls, [])
        self.assertEqual({p.name: digest(p) for p in f['stage'].iterdir()}, before)

    def test_preflight_failure_receipt_retains_declared_bindings_and_first_error(self):
        f = self.fixture()
        f['request']['tools']['codesign']['sha256'] = '0' * 64
        self.rewrite_request(f)
        result, _ = self.run_case(f, success=False)
        for key in ('configuration', 'stage_root', 'selection', 'requirements', 'build_engine', 'tools'):
            self.assertEqual(result.get(key), f['request'][key], key)
        self.assertIn('Input digest mismatch', result['error'])
        self.assertEqual(result['commands'], [])
        self.assertEqual(self.calls, [])


# This boundary fixture receives the real generated install argv. It records
# them, without invoking or simulating production Apple-tool success.
CMAKE_BOUNDARY = r'''
import json, pathlib, sys
args=sys.argv[1:]; values=dict(zip(args[1::2],args[2::2]))
report=pathlib.Path(values['--report-parent'])
report.mkdir(parents=True,exist_ok=True)
p=report/('fixture-call-'+str(len(list(report.glob('fixture-call-*.json'))))+'.json')
p.write_text(json.dumps({'evidence_kind':'FIXTURE_ONLY','argv':args,'values':values}),encoding='utf-8')
if pathlib.Path(__file__).with_name('fail-boundary').exists(): sys.exit(29)
'''


class MacRuntimeCMakeDispatch(cmake_fixture.RuntimeInstallTests):
    def case(self, *, platform='Darwin', multi=False, static=False):
        root=self.root/('dispatch-'+uuid.uuid4().hex); source=root/'source'; source.mkdir(parents=True)
        (source/'cmake').mkdir(); (source/'scripts').mkdir()
        shutil.copyfile(ROOT/'cmake/CaesuraMacOSInstall.cmake',source/'cmake/CaesuraMacOSInstall.cmake')
        (source/'scripts/install_macos_runtime.py').write_text(CMAKE_BOUNDARY,encoding='utf-8')
        (source/'Engine').write_bytes(b'fixture Engine, not executed')
        project=f'''cmake_minimum_required(VERSION 3.25)
set(CMAKE_SYSTEM_NAME Darwin)
project(Engine LANGUAGES NONE)
set(CMAKE_SYSTEM_NAME {platform})
'''
        if platform=='Linux': project+='set(APPLE FALSE)\n'
        project+='''add_executable(Engine IMPORTED GLOBAL)
set_target_properties(Engine PROPERTIES IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/Engine")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/Engine" DESTINATION .)
'''
        for config in ('Debug','Release'):
            # The production Phase B suite separately proves selection bytes.
            # Here unique per-config bytes independently identify dispatch.
            content=json.dumps({'configuration':config,'linkage':'static' if static else 'shared'})
            project+=f'file(WRITE "${{CMAKE_BINARY_DIR}}/openssl-selection-{config}.json" [=[{content}]=])\n'
        project+='file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/package-requirements-$<CONFIG>.json" CONTENT "{}")\n'
        integration=(ROOT/'CMakeLists.txt').read_text(encoding='utf-8').split('# Finalize every desktop Mac install after Engine and SDL.',1)[1].split('include(CPack)',1)[0]
        project+=integration
        (source/'CMakeLists.txt').write_text(project,encoding='utf-8')
        build=root/'build'; generator=list(self.generator)
        if multi:
            ninja=next((s.split('=',1)[1] for s in generator if s.startswith('-DCMAKE_MAKE_PROGRAM=')),shutil.which('ninja'))
            self.assertTrue(ninja,'Ninja Multi-Config required')
            generator=['-G','Ninja Multi-Config','-DCMAKE_MAKE_PROGRAM='+str(ninja),'-DCMAKE_CONFIGURATION_TYPES=Debug;Release']
        self.command([self.cmake,'-S',source,'-B',build,*generator,'-DCMAKE_BUILD_TYPE=Release',
                      '-DPython3_EXECUTABLE='+str(Path(sys.executable).resolve())])
        return root,source,build

    def install_case(self, f, *, prefix=None, config='Release', destdir=None, failure=False):
        root,source,build=f; prefix=prefix or root/'installed 中文 with spaces'
        env={'DESTDIR':str(destdir)} if destdir else None
        self.command([self.cmake,'--install',build,'--config',config,'--prefix',prefix],env=env,expected=1 if failure else 0)
        calls=sorted(Path(str(build)+'-macos-install-reports').glob('fixture-call-*.json'))
        return prefix,[json.loads(p.read_bytes()) for p in calls]

    def test_actual_install_dispatches_config_and_prefix_once_for_shared_and_static(self):
        for static in (False,True):
            with self.subTest(static=static):
                f=self.case(multi=True,static=static); prefix,calls=self.install_case(f,config='Release')
                self.assertEqual(len(calls),1); args=calls[0]['values']
                self.assertEqual(args['--configuration'],'Release')
                self.assertEqual(Path(args['--stage-root']),prefix)
                self.assertEqual(Path(args['--selection-file']),f[2]/'openssl-selection-Release.json')
                self.assertEqual(args['--selection-sha256'],digest(f[2]/'openssl-selection-Release.json'))
                self.assertEqual(Path(args['--build-engine']),f[1]/'Engine')
                self.assertEqual(Path(args['--report-parent']),Path(str(f[2])+'-macos-install-reports'))
                self.assertEqual((prefix/'Engine').read_bytes(),b'fixture Engine, not executed')
                self.assertFalse((prefix/'macos-install-reports').exists())

    def test_actual_install_iOS_and_non_apple_do_not_dispatch(self):
        for platform in ('iOS','Linux'):
            with self.subTest(platform=platform):
                f=self.case(platform=platform); _,calls=self.install_case(f)
                self.assertEqual(calls,[])

    def test_actual_install_failure_is_nonzero_and_stops(self):
        f=self.case(); (f[1]/'scripts/fail-boundary').write_bytes(b'fail')
        _,calls=self.install_case(f,failure=True)
        self.assertEqual(len(calls),1)

    def test_posix_actual_destdir_is_composed_once(self):
        f=self.case(); prefix=Path('/caesura-fixture-prefix-'+uuid.uuid4().hex)
        destdir=f[0]/'physical destdir'
        self.assertFalse(prefix.exists())
        _,calls=self.install_case(f,prefix=prefix,destdir=destdir)
        expected=destdir/prefix.relative_to('/')
        self.assertEqual(Path(calls[0]['values']['--stage-root']),expected)
        self.assertTrue((expected/'Engine').is_file()); self.assertFalse(prefix.exists())


if __name__=='__main__':
    suite=unittest.TestSuite()
    for cls in (MacRuntimeCoordinatorTests,MacRuntimeCMakeDispatch):
        names=sorted(n for n,v in cls.__dict__.items() if n.startswith('test_') and callable(v)
                     and (os.name!='nt' or not n.startswith('test_posix_')))
        suite.addTests(cls(n) for n in names)
    print('FIXTURE_ONLY: synthetic Mach-O, owned Python tool boundary; no Apple signing/runtime evidence',flush=True)
    raise SystemExit(0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1)
