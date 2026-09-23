"""Actual CMake/CPack tests for explicit macOS OpenSSL runtime installation.

Reuse the maintained fixture driver and execute the production copy helper.
Arbitrary payload bytes are not Mach-O. No compiler, Engine, otool,
install_name_tool or codesign is invoked; these tests establish copy, naming,
configuration and staging behavior, not macOS binary relocation or signing.
The direct entry selects only methods defined by this class, so inherited
runtime-install cases remain in their original suite and are not counted twice.
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    'maintained_runtime_install', ROOT / 'tests/scripts/test_package_runtime_install.py')
maintained = importlib.util.module_from_spec(spec)
spec.loader.exec_module(maintained)


class OpenSSLRuntimeInstallTests(maintained.RuntimeInstallTests):
    """Explicit linkage and exact runtime names for imported OpenSSL targets.

    With old helper these extra CMake arguments are ignored and UNKNOWN targets
    return early. Positive copy checks therefore fail for missing real files,
    rather than merely requiring a new function name to exist.
    """

    def openssl_fixture(self, *, linkage='shared', missing=False,
                        bad_name=None, chain=False, multi=False):
        f = self.fixture(kind='ABSENT', darwin=True)
        source, sdk = f['source'], f['real'].parent
        calls, expected, inputs = [], {}, []
        for component in ('ssl', 'crypto'):
            name = f'lib{component}.a' if linkage == 'static' else f'lib{component}.3.dylib'
            selected = sdk / name
            selected.write_bytes(b'NON-MACHO copy fixture ' + component.encode() + b' Release\x00\xff')
            inputs.append(selected)
            location = selected
            if chain:
                middle = sdk / (name + '.alias-1')
                middle.symlink_to(selected.name)
                location = sdk / (name + '.alias-2')
                location.symlink_to(middle.name)
            if missing and component == 'crypto':
                # A genuinely absent input, without deleting any input artifact.
                location = sdk / 'never-created-libcrypto.3.dylib'
            symbol = 'OpenSSL::' + ('SSL' if component == 'ssl' else 'Crypto')
            calls += [f'add_library({symbol} UNKNOWN IMPORTED GLOBAL)',
                      f'set_target_properties({symbol} PROPERTIES IMPORTED_LOCATION "{location.as_posix()}")']
            if multi:
                debug = sdk / ('debug-' + name)
                debug.write_bytes(b'WRONG DEBUG ' + component.encode())
                inputs.append(debug)
                calls += [f'set_target_properties({symbol} PROPERTIES IMPORTED_CONFIGURATIONS "DEBUG;RELEASE" '
                          f'IMPORTED_LOCATION_DEBUG "{debug.as_posix()}" IMPORTED_LOCATION_RELEASE "{location.as_posix()}")']
            runtime_name = bad_name if bad_name is not None else name
            calls += [f'caesura_install_shared_runtime({symbol} LINKAGE "{linkage}" RUNTIME_NAME "{runtime_name}")']
            if linkage == 'shared':
                expected[name] = selected.read_bytes()
        with (source / 'CMakeLists.txt').open('a', encoding='utf-8') as stream:
            stream.write('\n' + '\n'.join(calls) + '\n')
        f.update(expected=expected, inputs=inputs)
        f['input_sha256'] = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        (source / 'openssl-inputs.json').write_text(json.dumps(f['input_sha256'], indent=2), encoding='utf-8')
        return f

    def exact_outputs(self, f, prefix=None):
        root = prefix or f['prefix']
        actual = {}
        if root.exists():
            for p in root.rglob('*'):
                self.assertFalse(p.is_symlink(), str(p))
                if p.is_file():
                    actual[p.relative_to(root).as_posix()] = p.read_bytes()
        self.assertEqual(actual, f['expected'])
        self.assertEqual({str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in f['inputs']},
                         f['input_sha256'], 'Installing must not edit selected SDK bytes')

    def package_outputs(self, f):
        out = f['build'].parent / 'openssl-packages'
        self.command([self.cpack, '--config', f['build'] / 'CPackConfig.cmake',
                      '-C', 'Release', '-G', 'TGZ', '-B', out])
        archive = out / 'runtime-fixture.tar.gz'
        prepared = maintained.prepare_package(
            archive, out / 'prepared', expected_sha256=hashlib.sha256(archive.read_bytes()).hexdigest())
        self.assertEqual(prepared['status'], 'PREPARED', prepared)
        self.exact_outputs(f, Path(prepared['package_path']) / 'runtime-fixture')
        self.assertEqual(maintained.verify_stable(prepared)['status'], 'STABLE')

    def test_unknown_openssl_targets_install_and_package_exact_files(self):
        f = self.openssl_fixture()
        self.configure(f)
        self.install(f)
        self.exact_outputs(f)
        self.package_outputs(f)

    def test_unknown_static_openssl_does_not_install_shared_bytes(self):
        f = self.openssl_fixture(linkage='static')
        self.configure(f)
        self.install(f)
        self.exact_outputs(f)

    def test_unknown_missing_required_crypto_fails_install_and_cpack(self):
        f = self.openssl_fixture(missing=True)
        self.configure(f)
        self.install(f, expected=1)
        self.command([self.cpack, '--config', f['build'] / 'CPackConfig.cmake',
                      '-C', 'Release', '-G', 'TGZ', '-B', f['build'].parent / 'failed-cpack'], expected=1)

    def test_unknown_unsafe_runtime_name_fails_before_escape(self):
        for name in ('../escape.dylib', '/absolute.dylib', 'dir/file.dylib', ''):
            with self.subTest(name=name):
                f = self.openssl_fixture(bad_name=name)
                # Proposed argument checks are configure-time; retain the actual
                # failing configure output, not a regex against generated code.
                self.command([self.cmake, '-S', f['source'], '-B', f['build'],
                              *self.generator, '-DCMAKE_BUILD_TYPE=Release',
                              '-DCMAKE_INSTALL_PREFIX=' + str(f['prefix'])], expected=1)
                self.assertFalse((f['prefix'].parent / 'escape.dylib').exists())

    def test_unknown_invalid_linkage_fails_configuration(self):
        f = self.openssl_fixture(linkage='unspecified')
        self.command([self.cmake, '-S', f['source'], '-B', f['build'],
                      *self.generator, '-DCMAKE_BUILD_TYPE=Release'], expected=1)

    def test_unknown_multi_config_install_selects_release_not_debug(self):
        f = self.openssl_fixture(multi=True)
        ninja = self.generator[-1].split('=', 1)[1] if self.generator else shutil.which('ninja')
        self.assertTrue(ninja, 'Actual Ninja is required; no silent skip')
        self.command([self.cmake, '-S', f['source'], '-B', f['build'],
                      '-G', 'Ninja Multi-Config', '-DCMAKE_MAKE_PROGRAM=' + str(ninja),
                      '-DCMAKE_CONFIGURATION_TYPES=Debug;Release',
                      '-DCMAKE_INSTALL_PREFIX=' + str(f['prefix'])])
        self.install(f)
        self.exact_outputs(f)

    if os.name != 'nt':
        def test_unknown_sdk_links_become_plain_copy_even_under_symlink_install_mode(self):
            f = self.openssl_fixture(chain=True)
            # Test environment restoration in the actual installation process.
            with (f['source'] / 'CMakeLists.txt').open('a', encoding='utf-8') as stream:
                stream.write('install(CODE [[if(NOT "$ENV{CMAKE_INSTALL_MODE}" STREQUAL "ABS_SYMLINK")\n'
                             'message(FATAL_ERROR "install mode was not restored")\nendif()]])\n')
            self.configure(f)
            self.install(f, env={'CMAKE_INSTALL_MODE': 'ABS_SYMLINK'})
            self.exact_outputs(f)

        def test_unknown_destdir_receives_files_without_touching_unstaged_prefix(self):
            f = self.openssl_fixture()
            stage = f['build'].parent / 'destdir'
            # Unique absolute prefix is already inside this owned fixture; it
            # must remain absent when CMake installs into DESTDIR + prefix.
            self.configure(f)
            self.install(f, env={'DESTDIR': str(stage)})
            staged = stage / f['prefix'].as_posix().lstrip('/')
            self.exact_outputs(f, staged)
            self.assertFalse(f['prefix'].exists())


if __name__ == '__main__':
    names = [name for name in OpenSSLRuntimeInstallTests.__dict__ if name.startswith('test_')]
    suite = unittest.TestSuite(OpenSSLRuntimeInstallTests(name) for name in names)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    sys.exit(not result.wasSuccessful())
