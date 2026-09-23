"""Actual CMake install/CPack of the production SDL runtime install section.

Payloads are byte fixtures, not actual SDL binaries. No Engine/compiler/device
is used; these tests prove installed names, file bytes and safe archive closure.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from package_verification import prepare_package, verify_stable


class RuntimeInstallTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tools = {name:shutil.which(name) for name in ('cmake','cpack')}
        if not all(tools.values()):
            raise RuntimeError('Actual CMake and CPack executables are required')
        cls.cmake = Path(tools['cmake']).resolve(strict=True)
        cls.cpack = Path(tools['cpack']).resolve(strict=True)
        cls.generator = []
        if os.name == 'nt':
            ninja = shutil.which('ninja')
            if not ninja:
                vswhere = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
                if vswhere.is_file():
                    found = subprocess.run([str(vswhere),'-latest','-products','*','-find',
                        'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'],
                        capture_output=True, text=True, encoding='utf-8', timeout=15, check=True)
                    ninja = next(iter(found.stdout.splitlines()), None)
            if not ninja:
                # An existing VS installation can be absent from vswhere's
                # registry. Probe only its standard CMake tool locations.
                visual_studio = Path(os.environ.get('ProgramFiles', 'C:/Program Files')) / 'Microsoft Visual Studio'
                candidates = (visual_studio / year / edition / 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
                              for year in ('2022','18') for edition in ('Community','Professional','Enterprise','BuildTools'))
                ninja = next((str(path) for path in candidates if path.is_file()), None)
            if not ninja:
                raise RuntimeError('Actual Ninja on PATH or in Visual Studio is required for the compiler-free Windows fixture')
            cls.generator = ['-G','Ninja','-DCMAKE_MAKE_PROGRAM='+str(Path(ninja).resolve(strict=True))]
        cls.section = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')
        cls.section = cls.section.split('# Bundle the SDL3 runtime DLL', 1)[1]
        cls.section = '# Bundle the SDL3 runtime DLL' + cls.section.split('# Without the Steam runtime DLL', 1)[0]

    def setUp(self):
        evidence = os.environ.get('CAESURA_RUNTIME_INSTALL_EVIDENCE')
        if evidence:
            self.root = Path(evidence).resolve() / (self._testMethodName + '-' + uuid.uuid4().hex)
            self.root.mkdir(parents=True)
        else:
            temporary = tempfile.TemporaryDirectory(prefix='caesura runtime install ')
            self.addCleanup(temporary.cleanup)
            self.root = Path(temporary.name).resolve()
        self.commands = []
        self.counter = 0
        inputs = [self.cmake, self.cpack, ROOT / 'CMakeLists.txt']
        helper = ROOT / 'cmake/CaesuraRuntimeInstall.cmake'
        if helper.exists():inputs.append(helper)
        if self.generator:inputs.append(Path(self.generator[-1].split('=',1)[1]))
        (self.root / 'input-identities.json').write_text(json.dumps(
            {str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}, indent=2), encoding='utf-8')

    def command(self, argv, *, env=None, expected=0):
        child_env = dict(os.environ)
        for key in ('CMAKE_INSTALL_MODE', 'DESTDIR'):
            child_env.pop(key, None)
        child_env.update(env or {})
        proc = subprocess.run([str(a) for a in argv], cwd=self.root, env=child_env,
                              capture_output=True, timeout=90)
        base = self.root / f'command-{self.counter:02d}'
        self.counter += 1
        base.with_suffix('.stdout.log').write_bytes(proc.stdout)
        base.with_suffix('.stderr.log').write_bytes(proc.stderr)
        self.commands.append({'argv':[str(a) for a in argv], 'exit_code':proc.returncode,
                              'environment_delta':env or {}, 'stdout_sha256':hashlib.sha256(proc.stdout).hexdigest(),
                              'stderr_sha256':hashlib.sha256(proc.stderr).hexdigest()})
        (self.root / 'commands.json').write_text(json.dumps(self.commands, indent=2), encoding='utf-8')
        text = (proc.stdout + proc.stderr).decode('utf-8', errors='replace')
        if expected == 0:
            self.assertEqual(proc.returncode, 0, text)
        else:
            self.assertNotEqual(proc.returncode, 0, text)
        return text

    def fixture(self, *, kind='SHARED', versioned=False, location_chain=False,
                missing=False, configurations=False, ios=False, darwin=False):
        root = self.root / ('case-' + uuid.uuid4().hex)
        source = root / 'source'; source.mkdir(parents=True)
        sdk = root / 'sdk with space'; sdk.mkdir()
        build = root / 'build'; prefix = root / 'installed'
        name = 'libSDL3.0.dylib' if darwin or sys.platform == 'darwin' else 'SDL3.dll' if os.name == 'nt' else 'libSDL3.so.0'
        soname_value = '@rpath/' + name if darwin else name
        payload = b'fixture shared runtime release bytes\x00\xff\n'
        real = sdk / (name + '.2.0' if versioned else name)
        if not missing:real.write_bytes(payload)
        location = real
        if location_chain:
            middle = sdk / 'imported-mid'; middle.symlink_to(real.name)
            location = sdk / 'imported-location'; location.symlink_to(middle.name)
        soname = sdk / name
        if versioned:soname.symlink_to(location.name)
        definitions = ''
        if kind != 'ABSENT':
            definitions = f'add_library(runtime_fixture {kind} IMPORTED GLOBAL)\nadd_library(SDL3::SDL3 ALIAS runtime_fixture)\n'
            if kind != 'INTERFACE':
                definitions += f'set_target_properties(runtime_fixture PROPERTIES IMPORTED_LOCATION "{location.as_posix()}" IMPORTED_SONAME "{soname_value}")\n'
                if configurations:
                    debug = sdk / 'debug-runtime'; debug.write_bytes(b'wrong debug bytes')
                    definitions += f'set_target_properties(runtime_fixture PROPERTIES IMPORTED_CONFIGURATIONS "DEBUG;RELEASE" IMPORTED_LOCATION_DEBUG "{debug.as_posix()}" IMPORTED_LOCATION_RELEASE "{location.as_posix()}" IMPORTED_SONAME_DEBUG "{name}" IMPORTED_SONAME_RELEASE "{name}")\n'
        # Execute the actual maintained section, including its platform guards.
        # The iOS guard is a configure-time fixture, not an iOS toolchain claim.
        platform = 'set(WIN32 FALSE)\nset(UNIX TRUE)\nset(APPLE TRUE)\nset(CMAKE_SYSTEM_NAME iOS)\n' if ios else ''
        helper = ROOT / 'cmake/CaesuraRuntimeInstall.cmake'
        if helper.exists():
            (source / 'cmake').mkdir()
            shutil.copy2(helper, source / 'cmake' / helper.name)
        (source / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.25)\n'
            + ('set(CMAKE_SYSTEM_NAME Darwin)\n' if darwin else '')
            + 'project(runtime_install_fixture LANGUAGES NONE)\n'
            'add_executable(${PROJECT_NAME} IMPORTED GLOBAL)\n'
            + definitions + platform + self.section
            + '\nset(CPACK_PACKAGE_NAME RuntimeFixture)\nset(CPACK_PACKAGE_VERSION 1.0.0)\n'
              'set(CPACK_PACKAGE_FILE_NAME runtime-fixture)\nset(CPACK_GENERATOR TGZ)\ninclude(CPack)\n',
            encoding='utf-8')
        return {'source':source,'build':build,'prefix':prefix,'name':name,'payload':payload,'real':real}

    def configure(self, f):
        self.command([self.cmake,'-S',f['source'],'-B',f['build'],
                      *self.generator,'-DCMAKE_BUILD_TYPE=Release','-DCMAKE_INSTALL_PREFIX='+str(f['prefix'])])

    def install(self, f, *, env=None, expected=0):
        return self.command([self.cmake,'--install',f['build'],'--config','Release'], env=env, expected=expected)

    def assert_plain(self, f):
        target=f['prefix']/f['name']
        self.assertFalse(target.is_symlink(), 'Installed SONAME must contain runtime bytes, not an SDK link')
        self.assertTrue(target.is_file(), 'Installed runtime target is absent')
        self.assertEqual(target.read_bytes(), f['payload'])
        self.assertEqual(sorted(p.name for p in f['prefix'].iterdir()), [f['name']])

    def package(self, f, *, env=None):
        output=f['build'].parent/'outputs'
        self.command([self.cpack,'--config',f['build']/'CPackConfig.cmake','-C','Release','-G','TGZ','-B',output], env=env)
        archive=output/'runtime-fixture.tar.gz'
        digest=hashlib.sha256(archive.read_bytes()).hexdigest()
        prepared=prepare_package(archive, f['build'].parent/'prepared', expected_sha256=digest)
        self.assertEqual(prepared['status'], 'PREPARED', prepared)
        package=Path(prepared['package_path'])
        matches=list(package.rglob(f['name']))
        self.assertEqual(len(matches), 1)
        self.assertFalse(matches[0].is_symlink())
        self.assertEqual(matches[0].read_bytes(), f['payload'])
        self.assertEqual(verify_stable(prepared)['status'], 'STABLE')

    def test_regular_shared_imported_alias_installs_and_packages_exact_bytes(self):
        f=self.fixture(); self.configure(f); self.install(f); self.assert_plain(f); self.package(f)

    def test_selected_release_configuration_installs_release_bytes(self):
        f=self.fixture(configurations=True); self.configure(f); self.install(f); self.assert_plain(f)

    def test_darwin_rpath_soname_installs_plain_basename(self):
        # Actual Darwin CMake platform/generator expressions, without claiming
        # a macOS toolchain, binary, execution or device validation.
        f=self.fixture(darwin=True); self.configure(f); self.install(f); self.assert_plain(f); self.package(f)

    def test_static_interface_and_absent_targets_install_no_runtime(self):
        for kind in ('STATIC','INTERFACE','ABSENT'):
            with self.subTest(kind=kind):
                f=self.fixture(kind=kind); self.configure(f); self.install(f)
                self.assertFalse(f['prefix'].exists() and any(f['prefix'].iterdir()))

    def test_ios_install_section_does_not_copy_desktop_runtime(self):
        f=self.fixture(ios=True); self.configure(f); self.install(f)
        self.assertFalse(f['prefix'].exists() and any(f['prefix'].iterdir()))

    def test_scripts_install_and_cpack_exclude_python_cache_bytes(self):
        source = self.root / 'script-source'; source.mkdir()
        scripts = source / 'scripts'; (scripts / 'nested' / '__pycache__').mkdir(parents=True)
        (scripts / '__pycache__').mkdir()
        wanted = {'entry.py': b'print("entry")\n', 'nested/helper.py': b'value = 7\n',
                  'nested/data.json': b'{"fixture":true}\n'}
        for name, payload in wanted.items():
            (scripts / name).write_bytes(payload)
        for name in ('__pycache__/entry.cpython-314.pyc', 'nested/__pycache__/helper.cpython-314.pyc',
                     'nested/legacy.pyc', 'legacy.pyo'):
            (scripts / name).write_bytes(b'host Python cache must not ship')
        maintained = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')
        sections = re.findall(r'install\(DIRECTORY scripts/\s+DESTINATION scripts\b.*?\)', maintained, re.S)
        self.assertEqual(len(sections), 1, 'Use the actual maintained scripts install rule')
        (source / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.25)\nproject(script_install_fixture LANGUAGES NONE)\n'
            + sections[0] + '\nset(CPACK_PACKAGE_NAME ScriptFixture)\nset(CPACK_PACKAGE_VERSION 1.0.0)\n'
            'set(CPACK_PACKAGE_FILE_NAME script-fixture)\nset(CPACK_GENERATOR TGZ)\ninclude(CPack)\n',
            encoding='utf-8')
        build = self.root / 'script-build'; prefix = self.root / 'script-installed'
        self.command([self.cmake, '-S', source, '-B', build, *self.generator,
                      '-DCMAKE_INSTALL_PREFIX=' + str(prefix)])
        self.command([self.cmake, '--install', build])
        def assert_content(root):
            files = {p.relative_to(root).as_posix(): p.read_bytes() for p in root.rglob('*') if p.is_file()}
            self.assertEqual(files, {'scripts/' + n: v for n, v in wanted.items()})
            self.assertFalse(any(p.name == '__pycache__' for p in root.rglob('*')))
        assert_content(prefix)
        output = self.root / 'script-packages'
        self.command([self.cpack, '--config', build / 'CPackConfig.cmake', '-G', 'TGZ', '-B', output])
        archive = output / 'script-fixture.tar.gz'
        prepared = prepare_package(archive, self.root / 'script-prepared',
                                   expected_sha256=hashlib.sha256(archive.read_bytes()).hexdigest())
        self.assertEqual(prepared['status'], 'PREPARED', prepared)
        assert_content(Path(prepared['package_path']) / 'script-fixture')
        self.assertEqual(verify_stable(prepared)['status'], 'STABLE')

    def test_missing_shared_runtime_fails_install_and_package(self):
        f=self.fixture(missing=True); self.configure(f)
        self.install(f, expected=1)
        self.command([self.cpack,'--config',f['build']/'CPackConfig.cmake','-C','Release','-B',f['build'].parent/'outputs'], expected=1)

    if os.name != 'nt':
        def test_versioned_soname_link_is_a_plain_installed_payload(self):
            f=self.fixture(versioned=True); self.configure(f); self.install(f); self.package(f); self.assert_plain(f)

        def test_imported_target_location_chain_is_resolved_before_copy(self):
            f=self.fixture(versioned=True,location_chain=True); self.configure(f); self.install(f); self.assert_plain(f); self.package(f)

        def test_install_mode_cannot_turn_runtime_into_an_external_sdk_link(self):
            f=self.fixture(); self.configure(f)
            env={'CMAKE_INSTALL_MODE':'ABS_SYMLINK'}
            self.install(f,env=env); self.assert_plain(f); self.package(f,env=env)


if __name__ == '__main__':
    unittest.main(verbosity=2)
