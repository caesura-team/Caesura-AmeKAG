"""Android driver fixtures exercise files, ZIPs and owned small processes only."""
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
from contextlib import redirect_stdout
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / 'scripts'))
try:
    import run_android_validation as driver
except ModuleNotFoundError:
    driver = None
from package_runtime import run_runtime_command


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def put(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data.encode() if isinstance(data, str) else data)
    return path


def write_json(path, value):
    return put(path, json.dumps(value, ensure_ascii=False).encode())


def rewrite_zip(path, changes=(), remove=()):
    with zipfile.ZipFile(path) as archive:
        entries = {n: archive.read(n) for n in archive.namelist() if n not in remove}
    entries.update(changes)
    with zipfile.ZipFile(path, 'w') as archive:
        for name, data in entries.items():
            archive.writestr(name, data)


# Standard JAR entry shape observed in retained actual JDK17 signed JARs.
# These bytes intentionally are not a cryptographic signature; execution stays
# FIXTURE_ONLY, including the external Android/JDK verification commands.
SIGNATURE_FIXTURE = {'META-INF/MANIFEST.MF': b'Manifest-Version: 1.0\r\n\r\n',
                     'META-INF/CAESURA.SF': b'Signature-Version: 1.0\r\n\r\n',
                     'META-INF/CAESURA.RSA': b'NOT A SIGNATURE - FIXTURE ONLY'}


def elf():
    ident = b'\x7fELF' + bytes([2, 1, 1]) + bytes(9)
    return struct.pack('<16sHHIQQQIHHHHHH', ident, 3, 183, 1, 0, 64, 0, 0, 64, 56, 1, 64, 0, 0) + struct.pack('<IIQQQQQQ', 1, 5, 0, 0, 0, 120, 120, 4096)


def inventory(root, paths):
    files = {}
    for name in paths:
        item = root / name
        for p in ([item] if item.is_file() else item.rglob('*')):
            if p.is_file():
                files[p.relative_to(root).as_posix()] = sha(p)
    return dict(root=str(root), paths=paths, files=files)


class AndroidDriverTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='u24 driver 中文 ')
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name).resolve()
        self.repo = self.root / 'repo'
        self.repo.mkdir()
        self.git = str(Path(shutil.which('git')).resolve())
        if os.name == 'nt' and Path(self.git).stat().st_nlink != 1:
            candidate = Path(self.git).parent.parent / 'bin/git.exe'
            self.assertEqual(candidate.stat().st_nlink, 1)
            self.git = str(candidate.resolve())
        for args in (['init', '-q'], ['config', 'core.autocrlf', 'false'], ['config', 'user.name', 'fixture'], ['config', 'user.email', 'fixture@example.invalid']):
            subprocess.run([self.git, *args], cwd=self.repo, check=True, capture_output=True)
        files = {'CMakeLists.txt': 'project(CaesuraAmeKAG VERSION 1.0.1 LANGUAGES C CXX)\n',
                 '.gitignore': '*.ignored\n', 'src/main.cpp': '// fixture\n', 'scripts/config.lua': 'config.entry_script = "../demo/entry.lua"\n',
                 'scripts/kag/init.lua': 'return true\n', 'assets/fonts/test.ttf': 'fixture font',
                 'tests/projects/first_vn/entry.lua': 'return true\n', 'android/app/src/main/assets/stale.txt': 'stale',
                 'android/app/src/main/AndroidManifest.xml': '<manifest/>', 'android/app/src/main/java/Main.java': '// fixture',
                 'android/app/proguard-rules.pro': '', 'android/settings.gradle': 'include ":app"',
                 'android/gradle.properties': 'org.gradle.jvmargs=-Xmx2048m\n'}
        for name, data in files.items():
            put(self.repo / name, data)
        for name in ('android/build.gradle', 'android/app/build.gradle'):
            put(self.repo / name, (ROOT / name).read_bytes())
        self.commit()
        self.components = {}
        self.tools = {}
        roles = {'python': 'python', 'git': 'git', 'cmake': 'cmake', 'ninja': 'ninja',
                 'java': 'jdk', 'keytool': 'jdk', 'jarsigner': 'jdk', 'aapt2': 'sdk',
                 'zipalign': 'sdk', 'apksigner_jar': 'sdk', 'clang': 'ndk', 'readelf': 'ndk'}
        for name in set(roles.values()) | {'gradle', 'sdl', 'openssl'}:
            (self.root / 'tools' / name).mkdir(parents=True)
        for role, component in roles.items():
            root = self.root / 'tools' / component
            rel = {'java': 'bin/java', 'keytool': 'bin/keytool', 'jarsigner': 'bin/jarsigner',
                   'aapt2': 'build-tools/34.0.0/aapt2', 'zipalign': 'build-tools/34.0.0/zipalign',
                   'apksigner_jar': 'build-tools/34.0.0/lib/apksigner.jar'}.get(role, role)
            path = put(root / rel, ('fixture ' + role).encode())
            self.tools[role] = dict(component=component, relative_path=rel)
        # Read-only Git and controller identity use real locked executable files.
        self.components['git'] = inventory(Path(self.git).parent, [Path(self.git).name])
        self.tools['git']['relative_path'] = Path(self.git).name
        python = Path(sys.executable).resolve()
        self.components['python'] = inventory(python.parent, [python.name])
        self.tools['python']['relative_path'] = python.name
        extra = {'jdk': {'release': 'JAVA_VERSION="17.0.20"\n', 'lib/modules': 'fixture runtime', 'conf/security/java.security': 'fixture configuration'},
                 'gradle': {'lib/gradle-gradle-cli-main-8.9.jar': 'fixture', 'lib/agents/gradle-instrumentation-agent-8.9.jar': 'fixture'},
                 'ndk': {'source.properties': 'Pkg.Revision = 27.3.13750724\n', 'build/cmake/android.toolchain.cmake': '# fixture', 'toolchains/llvm/prebuilt/fixture/sysroot/include/a.h': '// fixture'},
                 'sdk': {'platforms/android-35/android.jar': 'fixture', 'platforms/android-35/source.properties': 'AndroidVersion.ApiLevel=35\n', 'build-tools/34.0.0/source.properties': 'Pkg.Revision=34.0.0\n'},
                 'sdl': {'lib/libSDL3.so': elf(), 'lib/cmake/SDL3/SDL3Config.cmake': '# fixture', 'include/SDL3/a.h': '// fixture'},
                 'openssl': {'lib/libssl.a': 'fixture', 'lib/libcrypto.a': 'fixture', 'include/openssl/a.h': '// fixture'}}
        for component, entries in extra.items():
            for name, data in entries.items():
                put(self.root / 'tools' / component / name, data)
        for component in set(roles.values()) | set(extra):
            if component not in self.components:
                self.components[component] = inventory(self.root / 'tools' / component, ['.'])
        self.toolchain = write_json(self.root / 'toolchain.json', dict(schema='caesura.android-toolchain.v1', components=self.components, tools=self.tools))
        self.seed = self.root / 'seed'
        put(self.seed / 'caches/modules-2/files-2.1/example/agp.jar', b'fixture dependency')
        put(self.seed / 'verification-metadata.xml', b'<verification-metadata/>')
        self.dependencies = write_json(self.root / 'dependencies.json', dict(schema='caesura.android-dependencies.v1', inventory=inventory(self.seed, ['.'])))
        self.value = dict(schema='caesura.android-validation-inputs.v1', repo=str(self.repo), source_sha=self.head,
                          configuration='Release', abi='arm64-v8a', min_sdk=24, compile_sdk=35, target_sdk=35,
                          stl='c++_static', package_name='com.caesura.app', version_name='1.0.1', version_code=1,
                          game_relative_path='tests/projects/first_vn', signing='ephemeral-test',
                          toolchain=dict(path=str(self.toolchain), sha256=sha(self.toolchain)),
                          dependencies=dict(path=str(self.dependencies), sha256=sha(self.dependencies)), jobs=2,
                          timeouts=dict(configure=30, compile=30, gradle=30, sign=30, verify=30))
        self.request = self.root / 'request.json'
        self.calls = []
        self.hook = None
        self.sequence = 0
        self.child = put(self.root / 'child.py', 'import sys\nprint(sys.argv[1],end="")\nraise SystemExit(int(sys.argv[2]))\n')

    def commit(self):
        subprocess.run([self.git, 'add', '.'], cwd=self.repo, check=True, capture_output=True)
        subprocess.run([self.git, 'commit', '-qm', 'fixture'], cwd=self.repo, check=True, capture_output=True)
        self.head = subprocess.check_output([self.git, 'rev-parse', 'HEAD'], cwd=self.repo).decode().strip()

    def save_request(self):
        write_json(self.request, self.value)

    def call(self):
        self.assertIsNotNone(driver, 'Controlled Android driver is missing')
        self.sequence += 1
        self.work = self.root / ('attempt-' + str(self.sequence))
        self.save_request()
        return driver.run_android_validation(self.request, sha(self.request), self.work, runner=self.runner)

    def runner(self, argv, **kw):
        name = Path(kw['control_dir']).parent.name
        if name == 'verify':
            name = 'verify-' + Path(kw['control_dir']).name.removesuffix('-process')
        self.calls.append((name, argv, kw['env']))
        text, code = self.produce(name, argv)
        if self.hook:
            self.hook(name, argv)
        return run_runtime_command([str(Path(sys.executable).resolve()), str(self.child), text, str(code)], **kw)

    def produce(self, name, argv):
        if name.endswith('-version'):
            text = {'java-version': 'openjdk version "17.0.20"\n', 'gradle-version': 'Gradle 8.9\n'}.get(name, 'fixture locked version\n')
            return text, 0
        work = self.work
        native = work / 'native'
        if name == 'configure':
            data = {a[2:].split('=', 1)[0]: a.split('=', 1)[1] for a in argv if a.startswith('-D')}
            data.update(CMAKE_HOME_DIRECTORY=str(self.repo), CMAKE_GENERATOR='Ninja',
                        CMAKE_C_COMPILER=str(self.root / 'tools/ndk/clang'), CMAKE_CXX_COMPILER=str(self.root / 'tools/ndk/clang'))
            put(native / 'CMakeCache.txt', ''.join(f'{k}:STRING={v}\n' for k, v in data.items()))
            command = str(self.root / 'tools/ndk/clang') + ' --target=aarch64-none-linux-android24 -c ' + str(self.repo / 'src/main.cpp')
            write_json(native / 'compile_commands.json', [dict(file=str(self.repo / 'src/main.cpp'), command=command)])
            put(native / 'build.ninja', '# synthetic build graph')
            # File API shape observed from actual CMake 4.3.3 / NDK27.3
            # configure. Tool execution remains an explicit fixture boundary.
            reply = native / '.cmake/api/v1/reply'
            refs = [dict(kind=kind, version=dict(major=major, minor=minor), jsonFile=f'{kind}-v{major}-fixture.json')
                    for kind, major, minor in [('codemodel', 2, 10), ('toolchains', 1, 1)]]
            write_json(reply / 'index-fixture.json', dict(cmake=dict(paths=dict(cmake=str(self.root / 'tools/cmake/cmake')),
                generator=dict(name='Ninja', multiConfig=False)), objects=refs,
                reply={f"{r['kind']}-v{r['version']['major']}": r for r in refs}))
            write_json(reply / refs[0]['jsonFile'], dict(kind='codemodel', version=refs[0]['version'],
                paths=dict(source=str(self.repo), build=str(native))))
            write_json(reply / refs[1]['jsonFile'], dict(kind='toolchains', version=refs[1]['version'],
                toolchains=[dict(language=language, compiler=dict(path=str(self.root / 'tools/ndk/clang'),
                    id='Clang', version='18.0.4', target='aarch64-none-linux-android24')) for language in ('C', 'CXX')]))
        elif name == 'compile':
            put(native / 'libCaesuraAmeKAG.so', elf())
        elif name == 'elf':
            return '  20: 1234 12 FUNC GLOBAL DEFAULT 10 SDL_main\n 0x1 (NEEDED) Shared library: [libSDL3.so]\n 0x1 (NEEDED) Shared library: [libc.so]\n', 0
        elif name == 'elf-sdl':
            return ' 0x1 (NEEDED) Shared library: [libc.so]\n', 0
        elif name == 'gradle':
            stage = work / 'android-stage'
            apk = {'AndroidManifest.xml': b'fixture binary XML', 'classes.dex': b'fixture dex'}
            aab = {'base/manifest/AndroidManifest.xml': b'fixture protobuf XML', 'base/dex/classes.dex': b'fixture dex', 'BundleConfig.pb': b'fixture config'}
            for directory, prefix in (('assets', 'assets/'), ('jniLibs', 'lib/')):
                root = stage / 'app/src/main' / directory
                for p in root.rglob('*'):
                    if p.is_file():
                        key = prefix + p.relative_to(root).as_posix()
                        apk[key] = p.read_bytes(); aab['base/' + key] = p.read_bytes()
            for path, entries in ((stage / 'app/build/outputs/apk/release/app-release-unsigned.apk', apk),
                                  (stage / 'app/build/outputs/bundle/release/app-release.aab', aab)):
                path.parent.mkdir(parents=True, exist_ok=True)
                with zipfile.ZipFile(path, 'w') as z:
                    for key, data in entries.items():
                        z.writestr(key, data)
        elif name == 'test-key':
            put(Path(argv[argv.index('-keystore') + 1]), b'NOT A PRIVATE KEY - FIXTURE ONLY')
        elif name == 'test-cert':
            put(Path(argv[argv.index('-file') + 1]), b'NOT A CERTIFICATE - FIXTURE ONLY')
        elif name == 'align':
            shutil.copyfile(argv[-2], argv[-1])
        elif name == 'sign-apk':
            shutil.copyfile(argv[-1], argv[argv.index('--out') + 1])
        elif name == 'sign-aab':
            target = Path(argv[argv.index('-signedjar') + 1])
            shutil.copyfile(argv[-2], target)
            rewrite_zip(target, SIGNATURE_FIXTURE)
        elif name == 'verify-aapt2':
            return "package: name='com.caesura.app' versionCode='1' versionName='1.0.1'\nsdkVersion:'24'\ntargetSdkVersion:'35'\nnative-code: 'arm64-v8a'\n", 0
        elif name == 'verify-apksigner':
            return 'Verifies\nNumber of signers: 1\nSigner #1 certificate SHA-256 digest: ' + sha(work / 'outputs/test-certificate.der') + '\n', 0
        elif name == 'verify-keytool':
            return 'Signer #1:\nCertificate #1:\nSHA256: ' + sha(work / 'outputs/test-certificate.der') + '\n', 0
        elif name == 'verify-jarsigner':
            return 'jar verified.\n', 0
        elif name != 'verify-zipalign':
            raise AssertionError('Unexpected phase ' + name)
        return '', 0

    def test_wrong_request_shape_and_bool_refuse_before_commands(self):
        for field, value in [('jobs', True), ('version_code', True), ('abi', 'x86_64'), ('signing', 'production'), ('extra', 'x')]:
            before = copy.deepcopy(self.value)
            self.value[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.call()
            self.value = before
        self.assertFalse(self.calls)

    def test_dirty_and_ignored_compile_input_refuse_before_commands(self):
        put(self.repo / 'src/main.cpp', 'dirty')
        with self.assertRaises(ValueError):
            self.call()
        self.commit();self.value['source_sha'] = self.head
        put(self.repo / 'src/surprise.ignored', 'undeclared glob input')
        with self.assertRaises(ValueError):
            self.call()
        self.assertFalse(self.calls)

    def test_external_locks_and_tool_addition_are_checked(self):
        put(self.root / 'tools/jdk/unlocked.jar', 'late extra')
        with self.assertRaises(ValueError):
            self.call()
        self.assertFalse(self.calls)

    def test_missing_dependency_and_source_version_refuse(self):
        (self.seed / 'caches/modules-2/files-2.1/example/agp.jar').unlink()
        with self.assertRaises(ValueError):
            self.call()
        self.assertFalse(self.calls)

    def test_hardlinked_input_is_refused(self):
        os.link(self.repo / 'src/main.cpp', self.root / 'alias')
        with self.assertRaises(ValueError):
            self.call()
        self.assertFalse(self.calls)

    def test_tracked_asset_link_stages_locked_regular_bytes_and_stable_checks_target(self):
        link = self.repo / 'assets/fonts/font-alias.ttf'
        link.symlink_to('test.ttf')
        self.assertEqual(link.read_bytes(), (self.repo / 'assets/fonts/test.ttf').read_bytes())
        self.commit(); self.value['source_sha'] = self.head
        result = self.call()
        self.assertEqual(result['status'], 'FIXTURE_ONLY')
        staged = self.work / 'android-stage/app/src/main/assets/game/assets/fonts/font-alias.ttf'
        self.assertFalse(staged.is_symlink())
        self.assertEqual(staged.read_bytes(), (self.repo / 'assets/fonts/test.ttf').read_bytes())
        self.assertEqual(driver.verify_android_validation_stable(result)['status'], 'ANDROID_VALIDATION_STABLE')
        link.unlink(); link.symlink_to('../src/main.cpp')
        with self.assertRaises(ValueError):
            driver.verify_android_validation_stable(result)

    def test_source_alias_changed_during_stage_copy_is_rejected(self):
        link = self.repo / 'assets/fonts/font-alias.ttf'
        link.symlink_to('test.ttf')
        self.assertEqual(link.read_bytes(), (self.repo / 'assets/fonts/test.ttf').read_bytes())
        self.commit(); self.value['source_sha'] = self.head
        original = driver._copy
        changed = []
        def change_after_copy(source, destination, digest):
            original(source, destination, digest)
            if Path(destination).name == 'font-alias.ttf':
                link.unlink(); link.symlink_to('../src/main.cpp')
                changed.append(True)
        with patch.object(driver, '_copy', side_effect=change_after_copy):
            with self.assertRaisesRegex(ValueError, 'Source link target differs'):
                self.call()
        self.assertEqual(changed, [True])
        self.assertFalse(any(name == 'gradle' for name, _, _ in self.calls))

    def test_existing_work_preserves_original_receipt(self):
        self.assertIsNotNone(driver, 'Controlled Android driver is missing')
        self.save_request()
        work = self.root / 'existing'
        receipt = put(work / 'android-validation.json', 'original failure')
        with self.assertRaises((ValueError, FileExistsError)):
            driver.run_android_validation(self.request, sha(self.request), work, runner=self.runner)
        self.assertEqual(receipt.read_text(), 'original failure')

    def test_complete_real_files_zips_owned_process_fixture_never_claims_build(self):
        poison = 'secret-do-not-inherit-android'
        password = 'FIXTURE-ONLY-private-password-do-not-record-987654321'
        with patch.dict(os.environ, {'CAESURA_ANDROID_KEYSTORE_PASS': poison, 'JAVA_TOOL_OPTIONS': poison, 'ORG_GRADLE_PROJECT_evil': poison}), patch.object(driver.secrets, 'token_urlsafe', return_value=password):
            result = self.call()
        self.assertEqual(result['status'], 'FIXTURE_ONLY')
        self.assertEqual(result['local_provenance'], 'FIXTURE_ONLY')
        self.assertEqual(result['runtime'], 'NOT_RUN'); self.assertEqual(result['device'], 'NOT_RUN')
        self.assertFalse(result['release_ready'])
        self.assertEqual(result['package']['status'], 'FIXTURE_PACKAGE_VERIFIED')
        self.assertEqual(driver.verify_android_validation_stable(result)['status'], 'ANDROID_VALIDATION_STABLE')
        self.assertEqual(result['private_cleanup'], 'COMPLETE')
        self.assertFalse((self.work / 'private-signing').exists())
        self.assertFalse((self.work / 'android-stage/app/src/main/assets/stale.txt').exists())
        gradle = next(args for name, args, env in self.calls if name == 'gradle')
        self.assertIn('--offline', gradle); self.assertIn('--dependency-verification', gradle)
        self.assertIn('-PcaesuraKeepJniBytes=true', gradle)
        self.assertIn('-PcaesuraNdkVersion=27.3.13750724', gradle)
        for name, args, env in self.calls:
            self.assertNotIn('JAVA_TOOL_OPTIONS', env); self.assertNotIn('CAESURA_ANDROID_KEYSTORE_PASS', env)
            self.assertNotIn(poison, json.dumps([args, env]))
        for path in self.work.rglob('*.json'):
            self.assertNotIn(poison, path.read_text(encoding='utf-8'))
            self.assertNotIn(password, path.read_text(encoding='utf-8'))
        for path in (self.work / 'commands').rglob('*'):
            if path.is_file(): self.assertNotIn(password.encode(), path.read_bytes())
        key_command = next(args for name, args, env in self.calls if name == 'test-key')
        self.assertIn('-storepass:file', key_command)

    def test_cli_fixture_77_and_receipt_digest(self):
        self.assertIsNotNone(driver)
        self.work = self.root / 'cli'; self.save_request()
        out = io.StringIO()
        with redirect_stdout(out):
            code = driver.main(['--request', str(self.request), '--request-sha256', sha(self.request), '--work', str(self.work)], runner=self.runner)
        self.assertEqual(code, 77, out.getvalue())
        result = json.loads(out.getvalue())
        self.assertEqual(result['receipt_sha256'], sha(result['receipt_path']))

    def test_exit_zero_without_new_native_output_never_adopts_stale(self):
        put(self.repo / 'android/app/src/main/jniLibs/arm64-v8a/libCaesuraAmeKAG.so', elf())
        self.commit();self.value['source_sha'] = self.head
        self.hook = lambda name, args: (self.work / 'native/libCaesuraAmeKAG.so').unlink() if name == 'compile' else None
        with self.assertRaises((ValueError, OSError)):
            self.call()
        self.assertFalse(any(name == 'gradle' for name, _, _ in self.calls))

    def test_jni_transformation_in_unsigned_zip_is_rejected(self):
        def hook(name, args):
            if name == 'gradle':
                path = self.work / 'android-stage/app/build/outputs/apk/release/app-release-unsigned.apk'
                with zipfile.ZipFile(path) as z:
                    entries = {n: z.read(n) for n in z.namelist()}
                entries['lib/arm64-v8a/libCaesuraAmeKAG.so'] += b'stripped-or-changed'
                with zipfile.ZipFile(path, 'w') as z:
                    for n, data in entries.items():z.writestr(n, data)
        self.hook = hook
        with self.assertRaisesRegex(ValueError, 'JNI/assets'):
            self.call()
        self.assertFalse(any(name == 'test-key' for name, _, _ in self.calls))

    def test_missing_aab_and_modified_stage_are_rejected(self):
        for mode in ('missing', 'stage'):
            def hook(name, args):
                if name == 'gradle':
                    if mode == 'missing':
                        (self.work / 'android-stage/app/build/outputs/bundle/release/app-release.aab').unlink()
                    else:
                        put(self.work / 'android-stage/app/src/main/assets/game/scripts/kag/init.lua', 'mutated')
            self.hook = hook
            with self.subTest(mode=mode), self.assertRaises((ValueError, OSError)):
                self.call()

    def test_final_verifier_failure_keeps_first_error_and_cleans_private(self):
        original = self.produce
        def producer(name, args):
            if name == 'verify-apksigner':
                return 'bad signature\n', 1
            return original(name, args)
        self.produce = producer
        with self.assertRaisesRegex(ValueError, 'Tool failed: apksigner'):
            self.call()
        report = json.loads((self.work / 'android-validation.json').read_text(encoding='utf-8'))
        self.assertEqual(report['status'], 'FAIL');self.assertEqual(report['private_cleanup'], 'COMPLETE')
        self.assertFalse((self.work / 'private-signing').exists())
        self.assertEqual(json.loads((self.work / 'verify/android-package.json').read_text(encoding='utf-8'))['status'], 'FAIL')

    def test_late_mutations_request_inputs_unsigned_final_receipts_all_reject(self):
        result = self.call()
        paths = [self.request, self.toolchain, self.seed / 'verification-metadata.xml', self.repo / 'src/main.cpp',
                 Path(result['outputs']['apk']['path']), Path(result['unsigned']['aab']['file']['path']),
                 Path(result['native']['library']['path']), Path(result['receipt_path']),
                 Path(result['commands'][0]['process_files'][0]['path'])]
        paths.extend(Path(item['path']) for item in result['native']['build_evidence']
                     if Path(item['path']).name in {'toolchains-v1', 'index-fixture.json', 'toolchains-v1-fixture.json'})
        for path in paths:
            before = path.read_bytes(); path.write_bytes(before + b'changed')
            with self.subTest(path=path), self.assertRaises((ValueError, RuntimeError)):
                driver.verify_android_validation_stable(result)
            path.write_bytes(before)

    def test_owned_timeout_preserves_failure_and_stops_later_phases(self):
        self.child.write_text('import time\ntime.sleep(10)\n')
        original = self.runner
        def timed(args, **kwargs):
            kwargs['timeout'] = .3
            return original(args, **kwargs)
        self.runner = timed
        with self.assertRaises(subprocess.TimeoutExpired):
            self.call()
        report = json.loads((self.work / 'commands/java-version/process/run.json').read_text(encoding='utf-8'))
        self.assertEqual(report['status'], 'TIMED_OUT'); self.assertEqual(report['owned_tree_cleanup'], 'COMPLETE')
        self.assertEqual(len(self.calls), 1)

    def test_invalid_compile_abi_cache_rejects(self):
        def hook(name, args):
            if name == 'configure':
                path = self.work / 'native/CMakeCache.txt'
                path.write_text(path.read_text(encoding='utf-8').replace('ANDROID_ABI:STRING=arm64-v8a', 'ANDROID_ABI:STRING=x86'), encoding='utf-8')
        self.hook = hook
        with self.assertRaisesRegex(ValueError, 'CMake selection'):
            self.call()

    def test_actual_cmake_comment_and_blank_line_layout_preserves_selections(self):
        def hook(name, args):
            if name == 'configure':
                path = self.work / 'native/CMakeCache.txt'
                entries = [line for line in path.read_text(encoding='utf-8').splitlines()
                           if not line.startswith(('CMAKE_C_COMPILER:', 'CMAKE_CXX_COMPILER:'))]
                # Layout and absent optional compiler entries observed in the real NDK cache.
                put(path, '# This is the CMakeCache file.\r\n\r\n'
                    + ''.join('//No help, variable specified on the command line.\r\n'
                              + entry + '\r\n\r\n' for entry in entries))
        self.hook = hook
        report = self.call()
        self.assertEqual(report['status'], 'FIXTURE_ONLY')
        self.assertFalse(report['release_ready'])

    def native_only(self, mutation=None):
        """Real driver/files; substitute only configure/build/readelf commands."""
        self.sequence += 1
        self.work = self.root / ('native-evidence-' + str(self.sequence))
        self.work.mkdir()
        owner = self
        class Commands:
            def run(self, name, argv, timeout):
                text, code = owner.produce(name, argv)
                owner.assertEqual(code, 0)
                if name == 'configure' and mutation:
                    mutation(owner.work / 'native')
                return text
        tools = {role: dict(path=str(Path(self.components[spec['component']]['root']) / spec['relative_path']))
                 for role, spec in self.tools.items()}
        return driver._build_native(self.value, dict(tools=tools, components=self.components), Commands(), self.work)

    def test_file_api_compilers_work_without_optional_cache_entries(self):
        def mutation(native):
            path = native / 'CMakeCache.txt'
            put(path, ''.join(line + '\n' for line in path.read_text(encoding='utf-8').splitlines()
                             if not line.startswith(('CMAKE_C_COMPILER:', 'CMAKE_CXX_COMPILER:'))))
        result = self.native_only(mutation)
        self.assertEqual(set(result['compilers']), {'C', 'CXX'})
        self.assertEqual(result['compilers']['C']['sha256'], sha(self.root / 'tools/ndk/clang'))
        locked = {Path(item['path']).name for item in result['build_evidence']}
        self.assertTrue({'toolchains-v1', 'index-fixture.json', 'toolchains-v1-fixture.json'} <= locked)

    def test_file_api_missing_ambiguous_and_mismatched_replies_reject(self):
        def changed(native, mode):
            reply = native / '.cmake/api/v1/reply'
            path = reply / 'index-fixture.json'
            index = json.loads(path.read_text(encoding='utf-8'))
            ref = index['reply']['toolchains-v1']
            tools = reply / 'toolchains-v1-fixture.json'
            value = json.loads(tools.read_text(encoding='utf-8'))
            if mode == 'missing-index': path.unlink(); return
            if mode == 'duplicate-index': write_json(reply / 'index-second.json', index); return
            if mode == 'missing-reply': del index['reply']['toolchains-v1']
            if mode == 'duplicate-object': index['objects'].append(copy.deepcopy(index['objects'][1]))
            if mode == 'object-mismatch': index['objects'][1]['jsonFile'] = 'different.json'
            if mode == 'escape': ref['jsonFile'] = '../toolchains.json'; index['objects'][1] = copy.deepcopy(ref)
            if mode == 'absolute': ref['jsonFile'] = str(tools); index['objects'][1] = copy.deepcopy(ref)
            if mode == 'wrong-cmake': index['cmake']['paths']['cmake'] = str(self.root / 'tools/ninja/ninja')
            if mode == 'wrong-generator': index['cmake']['generator']['name'] = 'Unix Makefiles'
            if mode == 'missing-file': tools.unlink()
            if mode == 'wrong-kind': value['kind'] = 'cache'
            if mode == 'wrong-version': value['version']['major'] = 2
            if mode == 'boolean-version': value['version']['major'] = True
            if mode == 'query-change': put(native / '.cmake/api/v1/query/toolchains-v1', 'changed')
            if mode == 'missing-C': value['toolchains'] = value['toolchains'][1:]
            if mode == 'missing-CXX': value['toolchains'] = value['toolchains'][:1]
            if mode == 'duplicate-language': value['toolchains'].append(copy.deepcopy(value['toolchains'][0]))
            if mode == 'wrong-source':
                codemodel = reply / 'codemodel-v2-fixture.json'
                data = json.loads(codemodel.read_text(encoding='utf-8')); data['paths']['source'] = str(self.root)
                write_json(codemodel, data)
            if mode == 'wrong-build':
                codemodel = reply / 'codemodel-v2-fixture.json'
                data = json.loads(codemodel.read_text(encoding='utf-8')); data['paths']['build'] = str(self.repo)
                write_json(codemodel, data)
            write_json(path, index)
            if mode == 'duplicate-json': put(tools, '{"kind":"toolchains",' + json.dumps(value)[1:])
            elif mode != 'missing-file': write_json(tools, value)
        for mode in ('missing-index', 'duplicate-index', 'missing-reply', 'duplicate-object', 'object-mismatch',
                     'escape', 'absolute', 'wrong-cmake', 'wrong-generator', 'missing-file', 'wrong-kind',
                     'wrong-version', 'boolean-version', 'query-change', 'missing-C', 'missing-CXX', 'duplicate-language', 'duplicate-json',
                     'wrong-source', 'wrong-build'):
            with self.subTest(mode=mode):
                with self.assertRaises((ValueError, OSError, RuntimeError)) as caught:
                    self.native_only(lambda native: changed(native, mode))
                self.assertNotIsInstance(caught.exception, UnicodeError)

    def test_file_api_compilers_remain_ndk_locked_and_cache_consistent(self):
        for mode in ('outside', 'unlocked', 'wrong-hash', 'cache-mismatch', 'cache-empty', 'relative', 'compile-target'):
            def mutation(native):
                path = native / '.cmake/api/v1/reply/toolchains-v1-fixture.json'
                value = json.loads(path.read_text(encoding='utf-8'))
                compiler = value['toolchains'][0]['compiler']
                if mode == 'outside': compiler['path'] = str(self.root / 'tools/cmake/cmake')
                if mode == 'unlocked': compiler['path'] = str(put(self.root / 'tools/ndk/unselected-clang', 'unselected'))
                if mode == 'wrong-hash': put(self.root / 'tools/ndk/clang', 'changed compiler')
                if mode == 'relative': compiler['path'] = 'clang'
                if mode.startswith('cache-'):
                    cache = native / 'CMakeCache.txt'
                    replacement = '' if mode == 'cache-empty' else str(self.root / 'tools/ndk/readelf')
                    put(cache, cache.read_text(encoding='utf-8').replace('CMAKE_C_COMPILER:STRING=' + str(self.root / 'tools/ndk/clang'), 'CMAKE_C_COMPILER:STRING=' + replacement))
                if mode == 'compile-target':
                    commands = native / 'compile_commands.json'
                    put(commands, commands.read_text(encoding='utf-8').replace('--target=aarch64', '--target=x86_64'))
                write_json(path, value)
            with self.subTest(mode=mode):
                with self.assertRaises((ValueError, OSError, RuntimeError)) as caught:
                    self.native_only(mutation)
                self.assertNotIsInstance(caught.exception, UnicodeError)
            put(self.root / 'tools/ndk/clang', 'fixture clang')

    def test_duplicate_cache_selection_is_rejected_even_when_last_value_matches(self):
        def hook(name, args):
            if name == 'configure':
                path = self.work / 'native/CMakeCache.txt'
                put(path, 'CMAKE_BUILD_TYPE:STRING=Debug\n' + path.read_text(encoding='utf-8'))
        self.hook = hook
        with self.assertRaisesRegex(ValueError, 'Duplicate CMake cache'):
            self.call()
        self.assertFalse(any(name == 'gradle' for name, _, _ in self.calls))

    def test_extra_or_changed_component_files_during_build_fail(self):
        self.hook = lambda name, args: put(self.root / 'tools/jdk/lib/late.jar', 'changed') if name == 'gradle' else None
        with self.assertRaisesRegex(ValueError, 'Inventory changed'):
            self.call()

    def test_duplicate_request_json_real_cli_retains_failure(self):
        self.assertIsNotNone(driver)
        self.request.write_text('{"schema":1,"schema":2}')
        work = self.root / 'cli-invalid'
        result = subprocess.run([sys.executable, str(ROOT / 'scripts/run_android_validation.py'), '--request', str(self.request),
                                 '--request-sha256', sha(self.request), '--work', str(work)], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 1)
        receipt = work / 'android-validation.json'
        self.assertEqual(json.loads(receipt.read_text(encoding='utf-8'))['status'], 'FAIL')

    def test_signing_manifest_is_owned_transform_not_presigned_payload_lock(self):
        original = self.produce
        def producer(name, argv):
            result = original(name, argv)
            if name == 'gradle':
                path = self.work / 'android-stage/app/build/outputs/bundle/release/app-release.aab'
                with zipfile.ZipFile(path, 'a') as z:
                    z.writestr('META-INF/MANIFEST.MF', b'Manifest-Version: 1.0\n')
            elif name == 'sign-aab':
                path = Path(argv[argv.index('-signedjar') + 1])
                with zipfile.ZipFile(path) as z:
                    entries = {n: z.read(n) for n in z.namelist()}
                entries['META-INF/MANIFEST.MF'] = b'Manifest-Version: 1.0\nFixture-Signature-Digest: observed\n'
                with zipfile.ZipFile(path, 'w') as z:
                    for n, data in entries.items(): z.writestr(n, data)
            return result
        self.produce = producer
        result = self.call()
        self.assertEqual(result['status'], 'FIXTURE_ONLY')
        self.assertNotIn('META-INF/MANIFEST.MF', result['package']['expected']['required_aab_entries'])
        self.assertIn('META-INF/MANIFEST.MF', result['unsigned']['aab']['all_entries'])

    def test_private_seed_artifact_changed_by_gradle_is_refused(self):
        def hook(name, args):
            if name == 'gradle':
                put(self.work / 'gradle-home/caches/modules-2/files-2.1/example/agp.jar', b'replaced private dependency')
        self.hook = hook
        with self.assertRaisesRegex(ValueError, 'digest'):
            self.call()

    def test_wrong_actual_gradle_version_refuses_before_tasks(self):
        original = self.produce
        def producer(name, args):
            return ('Gradle 8.10\n', 0) if name == 'gradle-version' else original(name, args)
        self.produce = producer
        with self.assertRaisesRegex(ValueError, 'Gradle'):
            self.call()
        self.assertFalse(any(n == 'gradle' for n, _, _ in self.calls))

    def test_component_launcher_only_inventory_cannot_omit_runtime(self):
        value = json.loads(self.toolchain.read_text(encoding='utf-8'))
        component = value['components']['jdk']
        component['paths'] = list(component['files'])
        write_json(self.toolchain, value);self.value['toolchain']['sha256'] = sha(self.toolchain)
        with self.assertRaisesRegex(ValueError, 'Full component runtime tree'):
            self.call()
        self.assertFalse(self.calls)

    def test_source_version_mismatch_and_inside_checkout_work(self):
        self.value['version_name'] = '1.0.2'
        with self.assertRaisesRegex(ValueError, 'CMake version'):
            self.call()
        self.save_request()
        with self.assertRaisesRegex(ValueError, 'outside every checkout'):
            driver.run_android_validation(self.request, sha(self.request), self.repo / 'attempt', runner=self.runner)

    def test_signing_failure_cleans_private_and_preserves_child_exit(self):
        original = self.produce
        def producer(name, args):
            result = original(name, args)
            return ('fixture signer failure\n', 9) if name == 'sign-apk' else result
        self.produce = producer
        with self.assertRaisesRegex(ValueError, 'Owned command failed: sign-apk'):
            self.call()
        report = json.loads((self.work / 'android-validation.json').read_text(encoding='utf-8'))
        self.assertEqual(report['private_cleanup'], 'COMPLETE')
        self.assertEqual(report['commands'][-1]['process']['actual_exit_code'], 9)
        self.assertFalse((self.work / 'verify').exists())

    def password_file_signer(self, mutation=None):
        """Real child/file reads at the signer boundary, not real APK signing.

        Selected apksigner help specifies one shared cursor per password file.
        Exercise that consumption rule with the production-generated argv.
        No credential bytes are sent to argv, stdout, stderr, or receipts.
        """
        child = put(self.root / 'password-consumer.py', '''import contextlib, shutil, sys
from pathlib import Path
args = sys.argv[1:]
with contextlib.ExitStack() as stack:
    streams = {}
    values = []
    for option in ('--ks-pass', '--key-pass'):
        spec = args[args.index(option) + 1]
        if not spec.startswith('file:'):
            raise SystemExit('fixture requires private password file input')
        path = Path(spec[5:]).resolve()
        if path not in streams:
            streams[path] = stack.enter_context(path.open('rb'))
        line = streams[path].readline()
        if not line:
            print('password read EOF at ' + option, file=sys.stderr)
            raise SystemExit(2)
        values.append(line.rstrip(b'\\r\\n'))
    if not values[0] or values[0] != values[1]:
        print('private key password differs from generated keystore password', file=sys.stderr)
        raise SystemExit(3)
shutil.copyfile(args[-1], args[args.index('--out') + 1])
print('both password reads completed; fixture artifact copied')
''')
        original = self.runner
        def runner(argv, **kw):
            if Path(kw['control_dir']).parent.name != 'sign-apk':
                return original(argv, **kw)
            self.calls.append(('sign-apk', argv, kw['env']))
            if mutation:
                mutation(argv)
            return run_runtime_command([str(Path(sys.executable).resolve()), '-I', str(child), *argv], **kw)
        self.runner = runner

    def test_apksigner_password_files_supply_both_sequential_reads(self):
        self.password_file_signer()
        result = self.call()
        self.assertEqual(result['status'], 'FIXTURE_ONLY')
        self.assertEqual(result['private_cleanup'], 'COMPLETE')
        self.assertFalse((self.work / 'private-signing').exists())
        signer = next(c for c in result['commands'] if c['name'] == 'sign-apk')
        self.assertEqual(signer['process']['actual_exit_code'], 0)
        self.assertIn('both password reads completed', Path(signer['stdout']['path']).read_text(encoding='utf-8'))

    def test_apksigner_password_files_eof_or_wrong_key_stop_and_clean(self):
        original = self.runner
        for mode, exit_code in (('eof', 2), ('wrong-key', 3)):
            self.runner = original
            def mutation(argv):
                store = Path(argv[argv.index('--ks-pass') + 1][len('file:'):])
                key = Path(argv[argv.index('--key-pass') + 1][len('file:'):])
                first = store.read_bytes().splitlines()[0] + b'\n'
                other = b'' if mode == 'eof' else b'deliberately-wrong-fixture-key\n'
                if store == key:
                    store.write_bytes(first + other)
                else:
                    key.write_bytes(other)
            self.password_file_signer(mutation)
            with self.subTest(mode=mode), self.assertRaisesRegex(ValueError, 'Owned command failed: sign-apk'):
                self.call()
            result = json.loads((self.work / 'android-validation.json').read_text(encoding='utf-8'))
            self.assertEqual(result['status'], 'FAIL')
            self.assertEqual(result['private_cleanup'], 'COMPLETE')
            self.assertFalse((self.work / 'private-signing').exists())
            self.assertEqual(result['commands'][-1]['process']['actual_exit_code'], exit_code)
            self.assertEqual(result['commands'][-1]['process']['owned_tree_cleanup'], 'COMPLETE')
            self.assertFalse(any(c['name'] == 'sign-aab' for c in result['commands']))
            self.assertFalse((self.work / 'verify').exists())
            self.assertEqual([p.name for p in (self.work / 'outputs').iterdir()], ['test-certificate.der'])

    def test_sdl_unstaged_transitive_native_dependency_refuses(self):
        original = self.produce
        def producer(name, args):
            if name == 'elf-sdl':
                return ' 0x1 (NEEDED) Shared library: [libc++_shared.so]\n', 0
            return original(name, args)
        self.produce = producer
        with self.assertRaisesRegex(ValueError, 'SDL shared dependency'):
            self.call()
        self.assertFalse(any(name == 'gradle' for name, _, _ in self.calls))

    def test_unknown_private_entry_cannot_prevent_known_secret_cleanup(self):
        original = self.produce
        def producer(name, args):
            result = original(name, args)
            if name == 'sign-apk':
                put(self.work / 'private-signing/tool-leftover.tmp', b'unknown fixture output')
                return 'fixture signer original failure\n', 9
            return result
        self.produce = producer
        with self.assertRaisesRegex(ValueError, 'Owned command failed: sign-apk'):
            self.call()
        report = json.loads((self.work / 'android-validation.json').read_text(encoding='utf-8'))
        self.assertEqual(report['status'], 'FAIL')
        self.assertEqual(report['private_cleanup'], 'FAILED')
        self.assertEqual(report['errors'][0], 'Owned command failed: sign-apk')
        self.assertTrue(any('Private cleanup:' in e for e in report['errors']))
        self.assertFalse((self.work / 'private-signing/password.txt').exists(), 'Known owned password must be removed even when unknown entries remain')
        self.assertFalse((self.work / 'private-signing/test.p12').exists(), 'Known owned key must be removed even when unknown entries remain')
        self.assertEqual((self.work / 'private-signing/tool-leftover.tmp').read_bytes(), b'unknown fixture output')

    def test_signers_pin_one_exact_rsa_metadata_name_and_preserve_business(self):
        original = self.produce
        def producer(name, argv):
            result = original(name, argv)
            if name == 'gradle':
                for path in (self.work / 'android-stage/app/build/outputs/apk/release/app-release-unsigned.apk',
                             self.work / 'android-stage/app/build/outputs/bundle/release/app-release.aab'):
                    rewrite_zip(path, {'META-INF/legal/license.txt': b'unsigned business metadata'})
            elif name == 'sign-apk':
                rewrite_zip(Path(argv[argv.index('--out') + 1]), SIGNATURE_FIXTURE)
            return result
        self.produce = producer
        result = self.call()
        apk = next(argv for name, argv, _ in self.calls if name == 'sign-apk')
        aab = next(argv for name, argv, _ in self.calls if name == 'sign-aab')
        self.assertIn('--v1-signer-name', apk)
        self.assertEqual(apk[apk.index('--v1-signer-name') + 1], 'CAESURA')
        self.assertIn('-sigfile', aab)
        self.assertEqual(aab[aab.index('-sigfile') + 1], 'CAESURA')
        for kind in ('apk', 'aab'):
            self.assertEqual(set(result['signature_transform'][kind]['metadata']), set(SIGNATURE_FIXTURE))
            self.assertEqual(result['signature_transform'][kind]['business_entries'], result['unsigned'][kind]['entries'])
            self.assertIn('META-INF/legal/license.txt', result['signature_transform'][kind]['business_entries'])
        self.assertEqual(driver.verify_android_validation_stable(result)['status'], 'ANDROID_VALIDATION_STABLE')

    def test_final_signing_cannot_add_assets_dex_or_fake_signature_paths(self):
        original = self.produce
        cases = [('apk', 'assets/game/unselected-review-resource.txt'), ('aab', 'base/dex/classes2.dex'),
                 ('aab', 'META-INF/assets/surprise.txt'), ('apk', 'META-INF/OTHER.RSA'),
                 ('aab', 'META-INF/nested/CAESURA.SF'), ('apk', 'meta-inf/manifest.mf')]
        for kind, entry in cases:
            def producer(name, argv):
                result = original(name, argv)
                if name == 'sign-' + kind:
                    path = Path(argv[argv.index('--out' if kind == 'apk' else '-signedjar') + 1])
                    rewrite_zip(path, {entry: b'UNSELECTED FIXTURE BYTES'})
                return result
            self.produce = producer
            with self.subTest(kind=kind, entry=entry):
                with self.assertRaisesRegex(ValueError, 'Signing changed business entries'):
                    self.call()
                receipt = json.loads((self.work / 'android-validation.json').read_text(encoding='utf-8'))
                self.assertEqual(receipt['status'], 'FAIL')
                self.assertEqual(receipt['private_cleanup'], 'COMPLETE')

    def test_final_partial_signature_group_or_unsigned_aab_is_rejected(self):
        original = self.produce
        for remove in [('META-INF/CAESURA.RSA',), tuple(SIGNATURE_FIXTURE)]:
            def producer(name, argv):
                result = original(name, argv)
                if name == 'sign-aab':
                    rewrite_zip(Path(argv[argv.index('-signedjar') + 1]), remove=remove)
                return result
            self.produce = producer
            with self.subTest(remove=remove), self.assertRaisesRegex(ValueError, 'Signature metadata set'):
                self.call()

    def test_apk_without_v1_must_preserve_existing_unsigned_manifest(self):
        original = self.produce
        for changed in (False, True):
            def producer(name, argv):
                result = original(name, argv)
                if name == 'gradle':
                    rewrite_zip(self.work / 'android-stage/app/build/outputs/apk/release/app-release-unsigned.apk',
                                {'META-INF/MANIFEST.MF': b'Manifest-Version: 1.0\r\nFixture: original\r\n'})
                elif changed and name == 'sign-apk':
                    rewrite_zip(Path(argv[argv.index('--out') + 1]), {'META-INF/MANIFEST.MF': b'changed without v1 signing'})
                return result
            self.produce = producer
            if changed:
                with self.assertRaisesRegex(ValueError, 'Unsigned APK manifest changed'):
                    self.call()
            else:
                result = self.call()
                self.assertEqual(result['status'], 'FIXTURE_ONLY')
                self.assertEqual(driver.verify_android_validation_stable(result)['status'], 'ANDROID_VALIDATION_STABLE')


class AndroidSourceLinkTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='u24-source-links-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.repo = self.root / 'repo'
        self.repo.mkdir()
        self.git = str(Path(shutil.which('git')).resolve())
        for args in (('init', '-q'), ('config', 'core.autocrlf', 'false'),
                     ('config', 'core.symlinks', 'true'), ('config', 'user.name', 'fixture'),
                     ('config', 'user.email', 'fixture@example.invalid')):
            self.command(*args)
        put(self.repo / '.gitignore', '*.ignored\n')
        put(self.repo / 'external/zstd/bin/zstd', 'regular zstd fixture bytes')

    def command(self, *args):
        return subprocess.run([self.git, *args], cwd=self.repo, env=os.environ,
                              check=True, capture_output=True).stdout

    def commit(self):
        self.command('add', '.')
        self.command('commit', '-qm', 'source fixture')

    def source(self):
        return driver._source(self.repo, self.git, dict(os.environ))

    def test_real_tracked_file_links_bind_index_target_and_regular_content(self):
        for name in ('unzstd', 'zstdcat'):
            (self.repo / 'external/zstd/bin' / name).symlink_to('zstd')
        self.commit()
        self.assertIn(b'120000 ', self.command('ls-files', '--stage'))
        result = self.source()
        for name in ('unzstd', 'zstdcat'):
            key = 'external/zstd/bin/' + name
            self.assertEqual(result['files'][key], sha(self.repo / 'external/zstd/bin/zstd'))
            self.assertEqual(result['links'][key]['mode'], '120000')
            self.assertEqual(result['links'][key]['target'], 'zstd')
            self.assertEqual(result['links'][key]['resolved'], 'external/zstd/bin/zstd')
        self.assertEqual(self.source(), result)
        with self.assertRaises(ValueError):
            driver._tree(self.repo, ['external'])
        with self.assertRaises(ValueError):
            driver.lock(self.repo / 'external/zstd/bin/unzstd')

    def test_git_symlinks_false_plain_representation_is_not_accepted_as_link(self):
        link = self.repo / 'external/zstd/bin/unzstd'
        link.symlink_to('zstd')
        self.commit()
        self.command('config', 'core.symlinks', 'false')
        link.unlink(); link.write_text('zstd', encoding='utf-8')
        self.assertEqual(self.command('status', '--porcelain'), b'')
        with self.assertRaises(ValueError):
            self.source()

    def test_escape_dangling_cycle_directory_and_untracked_terminal_are_rejected(self):
        put(self.root / 'outside', 'outside source')
        put(self.repo / 'external/zstd/bin/temporary.ignored', 'untracked terminal')
        link = self.repo / 'external/zstd/bin/alias'
        for target in ('../../../../outside', str(self.root / 'outside'), 'missing', 'alias', '..', 'temporary.ignored'):
            with self.subTest(target=target):
                link.symlink_to(target, target_is_directory=target == '..')
                self.commit()
                try:
                    with self.assertRaises((ValueError, OSError)):
                        self.source()
                finally:
                    link.unlink(); self.commit()

    def test_tracked_link_chain_and_ignored_untracked_link_are_rejected(self):
        base = self.repo / 'external/zstd/bin'
        (base / 'first').symlink_to('second')
        (base / 'second').symlink_to('zstd')
        self.commit()
        with self.assertRaises(ValueError):
            self.source()
        (base / 'first').unlink(); (base / 'second').unlink(); self.commit()
        (base / 'untracked.ignored').symlink_to('zstd')
        self.assertEqual(self.command('status', '--porcelain'), b'')
        with self.assertRaisesRegex(ValueError, 'Undeclared source link'):
            self.source()

    def test_link_mutation_during_source_hashing_is_rejected(self):
        base = self.repo / 'external/zstd/bin'
        put(base / 'other', (base / 'zstd').read_bytes())
        link = base / 'alias'
        link.symlink_to('zstd')
        self.commit()
        original = driver.lock
        changed = []
        def mutate(path):
            result = original(path)
            if path == base / 'zstd' and not changed:
                link.unlink(); link.symlink_to('other')
                changed.append(True)
            return result
        with patch.object(driver, 'lock', side_effect=mutate):
            with self.assertRaisesRegex(ValueError, 'Source link target differs'):
                self.source()
        self.assertEqual(changed, [True])

    def test_plain_source_receipt_keeps_original_shape(self):
        self.commit()
        self.assertEqual(set(self.source()), {'source_sha', 'files'})

    def dotdot_fixture(self, kind):
        put(self.repo / 'unscanned/payload.h', 'inside locked header')
        put(self.root / 'outside/payload.h', 'outside different header')
        (self.root / 'outside/nest').mkdir()
        put(self.repo / '.gitignore', '*.ignored\nunscanned/hop\n')
        hop = self.repo / 'unscanned/hop'
        if kind == 'symlink':
            hop.symlink_to('../../outside/nest', target_is_directory=True)
        elif kind == 'junction':
            subprocess.run(['cmd', '/c', 'mklink', '/J', str(hop), str(self.root / 'outside/nest')],
                           check=True, capture_output=True)
            self.addCleanup(hop.rmdir)
        elif kind == 'ordinary':
            hop.mkdir()
        alias = self.repo / 'src/alias.h'
        alias.parent.mkdir()
        alias.symlink_to('../unscanned/hop/../payload.h')
        self.commit()
        self.assertEqual(self.command('status', '--porcelain'), b'')
        return alias

    def test_dotdot_cannot_hide_an_ignored_intermediate_directory_link(self):
        self.dotdot_fixture('symlink')
        with self.assertRaises(ValueError):
            self.source()

    def test_dotdot_cannot_hide_a_missing_or_regular_file_component(self):
        alias = self.dotdot_fixture('missing')
        with self.assertRaises((ValueError, OSError)):
            self.source()
        (self.repo / 'unscanned/hop').write_text('not a directory')
        with self.assertRaises(ValueError):
            self.source()

    if os.name == 'posix':
        def test_dotdot_through_an_actual_ordinary_directory_binds_real_endpoint(self):
            alias = self.dotdot_fixture('ordinary')
            source = self.source()
            self.assertEqual(source['links']['src/alias.h']['resolved'], 'unscanned/payload.h')
            self.assertEqual(source['files']['src/alias.h'], hashlib.sha256(alias.read_bytes()).hexdigest())
            self.assertEqual(alias.resolve(strict=True), self.repo / 'unscanned/payload.h')

    if os.name == 'nt':
        def test_windows_unresolvable_raw_link_is_not_accepted_by_lexical_target(self):
            alias = self.dotdot_fixture('ordinary')
            # This Windows raw POSIX-slash/.. link can be stored in Git but the
            # OS cannot open it. It is not the POSIX ordinary-dir positive.
            with self.assertRaises(OSError):
                alias.read_bytes()
            with self.assertRaises((ValueError, OSError)):
                self.source()

        def test_dotdot_cannot_hide_an_ignored_intermediate_junction(self):
            self.dotdot_fixture('junction')
            with self.assertRaises(ValueError):
                self.source()

        def test_ignored_directory_junction_is_rejected_without_recursing(self):
            self.commit()
            outside = self.root / 'outside'
            put(outside / 'keep', 'do not traverse or change')
            junction = self.repo / 'external/extra.ignored'
            subprocess.run(['cmd', '/c', 'mklink', '/J', str(junction), str(outside)],
                           check=True, capture_output=True)
            try:
                self.assertTrue(junction.is_junction())
                self.assertEqual(self.command('status', '--porcelain'), b'')
                with self.assertRaisesRegex(ValueError, 'junction'):
                    self.source()
                self.assertEqual((outside / 'keep').read_text(), 'do not traverse or change')
            finally:
                junction.rmdir()


if __name__ == '__main__':
    unittest.main(verbosity=2)
