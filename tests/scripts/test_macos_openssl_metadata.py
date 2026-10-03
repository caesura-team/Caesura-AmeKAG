"""UNEXECUTED candidate: real CMake selection and production Python receiver.

No build, compiler, Engine, install_name_tool or codesign is invoked. Archive
members and Mach-O load commands are synthetic format fixtures, not executable
OpenSSL evidence. Keep every first RED/tool failure in a new retained attempt.
When promoted to tests/scripts, change only ROOT to parents[2]. The direct entry
selects methods defined here, never repeats inherited maintenance test bodies.
"""
from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import sys
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))


def load_fixture(name, relative):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


install = load_fixture('phase_b_install_fixture', 'tests/scripts/test_package_runtime_install.py')
contract = load_fixture('phase_b_native_fixture', 'tests/scripts/test_native_package_contract.py')
MODULE = ROOT / 'cmake/CaesuraPackageRequirements.cmake'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def static_archive(component):
    # A physical regular ar file with a correctly sized/aligned member. The
    # MH_OBJECT header and marker are synthetic; no ABI/code claim is made.
    payload = contract.macho_fixture(file_type=1) + component.encode('ascii')
    header = ('fixture.o/'.ljust(16) + '0'.ljust(12) + '0'.ljust(6)
              + '0'.ljust(6) + '100644'.ljust(8) + str(len(payload)).ljust(10)
              + '`\n').encode('ascii')
    assert len(header) == 60
    return b'!<arch>\n' + header + payload + (b'\n' if len(payload) % 2 else b'')


def shared_image(runtime_name, *, identifier=None):
    name = identifier if identifier is not None else '@rpath/' + runtime_name
    commands = [contract.macho_dylib_command(name, 0xD),
                contract.macho_dylib_command('/usr/lib/libSystem.B.dylib')]
    return contract.macho_fixture(commands, file_type=6)


def required(linkage='shared', names=None):
    return contract.config('macos', openssl_linkage=linkage,
                           openssl_libraries=(['libssl.3.dylib', 'libcrypto.3.dylib']
                                              if names is None and linkage == 'shared'
                                              else names or []))


class MacOpenSSLBuildMetadata(install.RuntimeInstallTests):
    """Execute actual CMake with UNKNOWN imported targets and inspect bytes."""

    def setUp(self):
        super().setUp()
        # The maintained command driver records real argv/stdout/stderr. Add
        # this slice's exact production and fixture input identities.
        paths = [MODULE, ROOT / 'scripts/verify_native_package.py',
                 ROOT / 'scripts/macho_dependencies.py', Path(__file__).resolve(),
                 ROOT / 'tests/scripts/test_native_package_contract.py']
        (self.root / 'metadata-input-identities.json').write_text(
            json.dumps({str(p): digest(p) for p in paths}, indent=2), encoding='utf-8')

    def metadata_fixture(self, *, release=('shared', 'shared'),
                         debug=('shared', 'shared'), platform='macos',
                         missing=None, invalid=None, omit=None, mapped=False):
        root = self.root / ('metadata-' + uuid.uuid4().hex)
        source = root / 'source'; source.mkdir(parents=True)
        sdk = root / 'sdk ordinary files'; sdk.mkdir()
        paths, names = {}, {}
        definitions = []
        for number, component in enumerate(('ssl', 'crypto')):
            symbol = 'OpenSSL::' + ('SSL' if component == 'ssl' else 'Crypto')
            paths[component], names[component] = {}, {}
            for config, kinds in (('Release', release), ('Debug', debug)):
                kind = kinds[number]
                runtime_name = f'lib{component}.{3 if config == "Release" else 99}.dylib'
                # The physical selected filename is deliberately different
                # from LC_ID_DYLIB: metadata must use the shared runtime table.
                name = f'lib{component}-{config}.a' if kind == 'static' else f'selected-{component}-{config}.dylib'
                p = sdk / name
                content = static_archive(component) if kind == 'static' else shared_image(runtime_name)
                if invalid == (component, config, 'garbage'):
                    content = b'not an archive or Mach-O even though the suffix looks right'
                elif invalid == (component, config, 'unsafe-id'):
                    content = shared_image(runtime_name, identifier='@rpath/../escape.dylib')
                elif invalid == (component, config, 'empty-archive'):
                    content = b'!<arch>\n'
                elif invalid == (component, config, 'directory'):
                    p.mkdir()
                elif invalid == (component, config, 'duplicate-name'):
                    content = shared_image('libssl.3.dylib')
                if invalid != (component, config, 'directory') and missing != (component, config):
                    p.write_bytes(content)
                paths[component][config] = p
                names[component][config] = runtime_name
            if component == omit:
                continue
            definitions += [f'add_library({symbol} UNKNOWN IMPORTED GLOBAL)']
            # Debug is deliberately first and the fallback; Release must use
            # CMake's selected configuration rather than first/generic path.
            definitions += [f'set_target_properties({symbol} PROPERTIES',
                '  IMPORTED_CONFIGURATIONS "DEBUG;RELEASE;PROFILE"',
                f'  IMPORTED_LOCATION "{paths[component]["Debug"].as_posix()}"',
                f'  IMPORTED_LOCATION_DEBUG "{paths[component]["Debug"].as_posix()}"',
                f'  IMPORTED_LOCATION_RELEASE "{paths[component]["Release"].as_posix()}"',
                f'  IMPORTED_LOCATION_PROFILE "{paths[component]["Release"].as_posix()}"',
                ('  MAP_IMPORTED_CONFIG_RELEASE "PROFILE"' if mapped else ''), ')']
            if mapped:
                # Make the direct RELEASE location wrong: CMake maps Release
                # to PROFILE, which still points to actual Release bytes.
                definitions += [f'set_property(TARGET {symbol} PROPERTY IMPORTED_LOCATION_RELEASE '
                                f'"{paths[component]["Debug"].as_posix()}")']
        # Non-Mac controls provide no usable OpenSSL targets at all.
        if platform != 'macos':
            definitions = []
        host_system = {'macos': 'Darwin', 'windows': 'Windows', 'linux': 'Linux', 'ios': 'Darwin'}[platform]
        guards = 'set(CMAKE_SYSTEM_NAME iOS)\n' if platform == 'ios' else ''
        cmake = f'''cmake_minimum_required(VERSION 3.25)
set(CMAKE_SYSTEM_NAME {host_system})
project(PackageFixture VERSION 1.2.3 LANGUAGES NONE)
{guards}add_executable(PackageFixture IMPORTED)
set_target_properties(PackageFixture PROPERTIES IMPORTED_LOCATION "${{CMAKE_CURRENT_SOURCE_DIR}}/Engine")
add_executable(lua_cli IMPORTED)
set_target_properties(lua_cli PROPERTIES IMPORTED_LOCATION "${{CMAKE_CURRENT_SOURCE_DIR}}/lua")
add_library(SDL3::SDL3 STATIC IMPORTED)
set_target_properties(SDL3::SDL3 PROPERTIES IMPORTED_LOCATION "${{CMAKE_CURRENT_SOURCE_DIR}}/libSDL3.a")
set(CPACK_PACKAGE_FILE_NAME "CaesuraAmeKAG-1.2.3-Fixture")
set(CAESURA_CAPABILITY_PLATFORM {platform})
set(CAESURA_CAPABILITY_FFMPEG_JSON false)
set(CAESURA_CAPABILITY_STEAM_JSON false)
set(CAESURA_CAPABILITY_LIVE2D_JSON false)
'''
        cmake += '\n'.join(definitions) + '\n'
        if platform == 'macos' and not omit:
            # Independent native CMake oracle for selected target locations;
            # no Python mirror of IMPORTED_CONFIG or MAP_IMPORTED_CONFIG.
            cmake += 'file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/selected-$<CONFIG>.txt" CONTENT '
            cmake += '"$<TARGET_FILE:OpenSSL::SSL>\\n$<TARGET_FILE:OpenSSL::Crypto>\\n")\n'
        cmake += f'include("{MODULE.as_posix()}")\n'
        (source / 'CMakeLists.txt').write_text(cmake, encoding='utf-8')
        files = {str(p): digest(p) for component in paths.values() for p in component.values() if p.is_file()}
        (root / 'fixture-inputs.json').write_text(json.dumps(files, indent=2), encoding='utf-8')
        return {'source': source, 'build': root / 'build', 'paths': paths, 'names': names,
                'inputs': files, 'platform': platform}

    def generate(self, f, *, multi=False, expect_success=True):
        generator = list(self.generator)
        if multi:
            ninja = next((arg.split('=', 1)[1] for arg in generator if arg.startswith('-DCMAKE_MAKE_PROGRAM=')),
                         shutil.which('ninja'))
            self.assertTrue(ninja, 'Ninja Multi-Config must be available; no missing-tool skip')
            generator = ['-G', 'Ninja Multi-Config', '-DCMAKE_MAKE_PROGRAM=' + str(ninja),
                         '-DCMAKE_CONFIGURATION_TYPES=Debug;Release']
        self.command([self.cmake, '-S', f['source'], '-B', f['build'], *generator,
                      '-DCMAKE_BUILD_TYPE=Release', '-DPython3_EXECUTABLE=' + sys.executable],
                     expected=0 if expect_success else 1)
        self.assertEqual({path: digest(Path(path)) for path in f['inputs']}, f['inputs'])

    def assert_metadata(self, f, config, linkage):
        m = json.loads((f['build'] / f'package-requirements-{config}.json').read_bytes())
        self.assertEqual((m['platform'], m['configuration'], m['version']), ('macos', config, '1.2.3'))
        expected = [f['names'][c][config] for c in ('ssl', 'crypto')] if linkage == 'shared' else []
        self.assertEqual(m['required_configuration'].get('openssl_linkage'), linkage)
        self.assertEqual(m['required_configuration'].get('openssl_libraries'), expected)
        selected = (f['build'] / f'selected-{config}.txt').read_text(encoding='utf-8').splitlines()
        self.assertEqual([Path(p).resolve() for p in selected],
                         [f['paths'][c][config].resolve() for c in ('ssl', 'crypto')])
        # The exact freshly generated external object is submitted to the
        # maintained receiver, not merely checked as a CMake string.
        snapshot, libraries = contract.native._configuration('macos', m['required_configuration'])
        self.assertEqual(snapshot, m['required_configuration'])
        self.assertEqual(libraries, expected)  # SDL is deliberately static.

    def test_unknown_release_static_debug_shared_selects_actual_files(self):
        f = self.metadata_fixture(release=('static', 'static'))
        self.generate(f, multi=True)
        self.assert_metadata(f, 'Release', 'static')
        self.assert_metadata(f, 'Debug', 'shared')

    def test_unknown_release_shared_debug_static_selects_actual_files(self):
        f = self.metadata_fixture(debug=('static', 'static'))
        self.generate(f, multi=True)
        self.assert_metadata(f, 'Release', 'shared')
        self.assert_metadata(f, 'Debug', 'static')

    def test_unknown_imported_map_uses_cmake_selected_files(self):
        f = self.metadata_fixture(debug=('static', 'static'), mapped=True)
        self.generate(f)
        self.assert_metadata(f, 'Release', 'shared')

    def test_unknown_missing_file_or_component_refuses_configuration(self):
        for options in ({'missing': ('crypto', 'Release')}, {'omit': 'crypto'}):
            with self.subTest(options=options):
                self.generate(self.metadata_fixture(**options), expect_success=False)

    def test_unknown_mixed_component_linkage_refuses_configuration(self):
        for kinds in (('shared', 'static'), ('static', 'shared')):
            with self.subTest(release=kinds):
                self.generate(self.metadata_fixture(release=kinds), expect_success=False)

    def test_unknown_physical_framing_and_regular_file_are_required(self):
        for failure in ('garbage', 'directory', 'empty-archive'):
            with self.subTest(failure=failure):
                kinds = ('static', 'static') if failure == 'empty-archive' else ('shared', 'shared')
                self.generate(self.metadata_fixture(release=kinds, invalid=('crypto', 'Release', failure)),
                              expect_success=False)

    def test_unknown_exact_names_refuse_escape_and_component_collision(self):
        for failure in ('unsafe-id', 'duplicate-name'):
            with self.subTest(failure=failure):
                self.generate(self.metadata_fixture(invalid=('crypto', 'Release', failure)),
                              expect_success=False)

    def test_nonmac_metadata_keeps_existing_required_object_without_openssl(self):
        for platform in ('windows', 'linux'):
            with self.subTest(platform=platform):
                f = self.metadata_fixture(platform=platform)
                self.generate(f)
                m = json.loads((f['build'] / 'package-requirements-Release.json').read_bytes())
                expected = {'schema': 1, 'sdl_linkage': 'static', 'sdl_libraries': [],
                            'ffmpeg': False, 'steam': False, 'live2d': False}
                self.assertEqual(m['required_configuration'], expected)
                self.assertEqual(contract.native._configuration(platform, expected), (expected, []))
                self.assertEqual(m['platform'], platform)

    def test_ios_has_no_desktop_metadata_or_openssl_selection(self):
        f = self.metadata_fixture(platform='ios')
        self.generate(f)
        self.assertEqual(list(f['build'].glob('package-requirements-*.json')), [])
        self.assertEqual(list(f['build'].glob('openssl-selection-*.json')), [])


class MacOpenSSLReceiver(unittest.TestCase):
    def test_legacy_without_both_fields_is_not_relabelled_static(self):
        value = contract.config('macos')
        original = copy.deepcopy(value)
        snapshot, libraries = contract.native._configuration('macos', value)
        self.assertEqual(snapshot, original)
        self.assertEqual(libraries, ['libSDL3.0.dylib'])
        self.assertNotIn('openssl_linkage', snapshot)
        self.assertNotIn('openssl_libraries', snapshot)

    def test_shared_and_static_explicit_metadata_are_accepted(self):
        for value, expected in ((required(), ['libSDL3.0.dylib', 'libssl.3.dylib', 'libcrypto.3.dylib']),
                                (required('static'), ['libSDL3.0.dylib'])):
            with self.subTest(linkage=value['openssl_linkage']):
                original = copy.deepcopy(value)
                snapshot, libs = contract.native._configuration('macos', value)
                self.assertEqual(snapshot, original)
                self.assertEqual(libs, expected)
                snapshot['openssl_libraries'].append('mutated-private-result')
                self.assertEqual(value, original, 'Receiver must not share caller state')

    def test_missing_mixed_unsafe_duplicate_and_ambiguous_metadata_are_refused(self):
        cases = []
        for key in ('openssl_linkage', 'openssl_libraries'):
            value = required(); value.pop(key); cases.append(value)
        for linkage in (None, True, 'unspecified', 'mixed', ['shared']):
            value = required(); value['openssl_linkage'] = linkage; cases.append(value)
        for names in ([], ['only.dylib'], ['one', 'two', 'three'],
                      ['same.dylib', 'same.dylib'], ['../escape', 'crypto.dylib'],
                      ['/absolute', 'crypto.dylib'], ['dir/ssl.dylib', 'crypto.dylib'],
                      ['C:ssl.dylib', 'crypto.dylib'], ['', 'crypto.dylib'],
                      ['ssl name.dylib', 'crypto.dylib'], [False, 'crypto.dylib'], 'not-a-list'):
            value = required(); value['openssl_libraries'] = names; cases.append(value)
        cases.append(required('static', ['unexpected.dylib']))
        for value in cases:
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    contract.native._configuration('macos', value)

    def test_nonmac_cannot_silently_enable_the_mac_only_metadata(self):
        for platform in ('windows', 'linux'):
            for value in (required(), required('static')):
                with self.subTest(platform=platform, linkage=value['openssl_linkage']):
                    with self.assertRaises(ValueError):
                        contract.native._configuration(platform, value)


class MacOpenSSLCompletePackage(contract.NativePackageContract):
    def setUp(self):
        evidence = os.environ.get('CAESURA_RUNTIME_INSTALL_EVIDENCE')
        if not evidence:
            super().setUp()
            return
        # Retain the actual package as well as the report in RED attempts.
        self.root = Path(evidence).resolve() / (self._testMethodName + '-' + uuid.uuid4().hex)
        self.root.mkdir(parents=True)
        self.package = self.root / 'explicit package'
        shutil.copytree(self.base, self.package)
        self.install_binaries('windows')

    def test_external_shared_metadata_binds_exact_ordinary_package_files(self):
        self.install_macos_dependency_fixture()
        before = {p.relative_to(self.package).as_posix(): digest(p)
                  for p in self.package.rglob('*') if p.is_file()}
        report = self.check_package('macos', required())
        (self.root / 'actual-static-report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
        (self.root / 'fixture-before.json').write_text(json.dumps(before, indent=2), encoding='utf-8')
        self.assertTrue(report['passed'], report)
        self.assertEqual(report['runtime'], 'NOT_RUN')
        self.assertTrue(report['input_stable'])
        observed = {item['relative_path']: item for item in report['runtime_libraries']}
        for name in ('libssl.3.dylib', 'libcrypto.3.dylib'):
            self.assertFalse((self.package / name).is_symlink())
            self.assertEqual(observed[name]['sha256'], before[name])
            self.assertEqual(Path(observed[name]['resolved_path']), (self.package / name).resolve())
        self.assertEqual({p.relative_to(self.package).as_posix(): digest(p)
                          for p in self.package.rglob('*') if p.is_file()}, before)
        # Declared Crypto cannot be satisfied by the differently named local
        # image already present in the closed dependency graph.
        self.assert_failed(self.check_package('macos', required(names=['libssl.3.dylib', 'absent-crypto.dylib'])))


if __name__ == '__main__':
    classes = (MacOpenSSLBuildMetadata, MacOpenSSLReceiver, MacOpenSSLCompletePackage)
    suite = unittest.TestSuite(cls(name) for cls in classes
                              for name in cls.__dict__ if name.startswith('test_'))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    sys.exit(not result.wasSuccessful())
