#!/usr/bin/env python3
"""Owned offline Android compile/package/test-signature validation, never a release.

Request, component inventories and dependency seed are externally pinned inputs.
No setup, downloads, license acceptance, device access or production key input.
The first supported slice is Release arm64, API 24/35, JDK17, Gradle8.9, NDK27.3.
Injected execution exercises the real pipeline but is always FIXTURE_ONLY (77).
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import stat
import subprocess
import sys
import zipfile

sys.dont_write_bytecode = True

import android_package_contract as package
from ci_package_lane import _new_work
from package_runtime import run_runtime_command
from package_verification import _name, _sha256_file, prepare_package, verify_stable
from verify_execution_bundle import _no_links

SCHEMA = 'caesura.android-validation.v2'
INPUT_SCHEMA = 'caesura.android-validation-inputs.v2'
COMPONENTS = {'python', 'git', 'cmake', 'ninja', 'jdk', 'gradle', 'ndk', 'sdk', 'sdl', 'openssl', 'bundletool'}
ROLES = dict(python='python', git='git', cmake='cmake', ninja='ninja', java='jdk',
             keytool='jdk', jarsigner='jdk', aapt2='sdk', zipalign='sdk', apksigner_jar='sdk',
             clang='ndk', readelf='ndk', bundletool_jar='bundletool')
NDK = '27.3.13750724'
SIGNER_NAME = 'CAESURA'
JAR_MANIFEST = 'META-INF/MANIFEST.MF'
SIGNATURE_ENTRIES = {JAR_MANIFEST, 'META-INF/' + SIGNER_NAME + '.SF', 'META-INF/' + SIGNER_NAME + '.RSA'}
MAX_FILES = 200_000
MAX_LOG = 128 * 1024 * 1024
MAX_JSON = 64 * 1024 * 1024
need = package._need
save = package._save
file_lock = package._file


def load(path, digest):
    path = file_lock(path, digest)
    need(path.stat().st_size <= MAX_JSON, 'JSON exceeds size limit')
    with path.open('rb') as stream:
        raw = stream.read(MAX_JSON + 1)
    need(len(raw) <= MAX_JSON and hashlib.sha256(raw).hexdigest() == digest, 'JSON changed during read')
    def pairs(items):
        result = {}
        for key, value in items:
            need(key not in result, 'Duplicate JSON key')
            result[key] = value
        return result
    def invalid(_):
        raise ValueError('Non-integer JSON number')
    try:
        return json.loads(raw.decode('utf-8'), object_pairs_hook=pairs, parse_float=invalid, parse_constant=invalid)
    except (UnicodeError, json.JSONDecodeError, RecursionError) as error:
        raise ValueError('Malformed JSON') from error


def lock(path):
    path = _no_links(path)
    digest = _sha256_file(path)
    file_lock(path, digest)
    return dict(path=str(path), sha256=digest)


def relative(value):
    need(type(value) is str and value and _name(value) == value and '\\' not in value, 'Unsafe relative path')
    return value


def _keys(value, keys, message):
    need(type(value) is dict and set(value) == set(keys), message)


def _tree(root, paths):
    """Exact selected physical input trees, including empty directories."""
    root = _no_links(root)
    need(root.is_dir(), 'Inventory root must be a directory')
    result = {}
    dirs = set()
    stack = [root if p == '.' else root / relative(p) for p in paths]
    while stack:
        path = _no_links(stack.pop())
        info = path.stat()
        rel = path.relative_to(root).as_posix()
        need(len(result) + len(dirs) <= MAX_FILES, 'Inventory exceeds entry limit')
        if stat.S_ISDIR(info.st_mode):
            dirs.add(rel)
            stack.extend(path.iterdir())
        else:
            need(stat.S_ISREG(info.st_mode) and info.st_nlink == 1, 'Inventory rejects special files and hardlinks')
            result[rel] = _sha256_file(path)
    return dict(files=result, directories=sorted(dirs))


def _inventory(value):
    _keys(value, ('root', 'paths', 'files'), 'Inventory requires root/paths/files')
    need(type(value['root']) is str and Path(value['root']).is_absolute(), 'Absolute inventory root required')
    root = _no_links(value['root'])
    need(str(root) == value['root'], 'Canonical inventory root required')
    paths = value['paths']
    need(type(paths) is list and 0 < len(paths) <= MAX_FILES and len(set(paths)) == len(paths), 'Invalid inventory selection')
    for i, p in enumerate(paths):
        need(type(p) is str and (p == '.' or relative(p) == p), 'Invalid inventory path')
        for q in paths[:i]:
            need(p != '.' and q != '.' and not Path(p).is_relative_to(q) and not Path(q).is_relative_to(p), 'Overlapping inventory selections')
    files = value['files']
    need(type(files) is dict and 0 < len(files) <= MAX_FILES, 'Nonempty file inventory required')
    for name, digest in files.items():
        relative(name)
        need(package._digest(digest), 'Invalid inventory digest')
    actual = _tree(root, paths)
    need(actual['files'] == files, 'Component/dependency inventory changed')
    return dict(value, directories=actual['directories'])


def _inventory_stable(value):
    need(_tree(Path(value['root']), value['paths']) == dict(files=value['files'], directories=value['directories']), 'Inventory changed after selection')


def _covered(component, name):
    need(name in component['files'], 'Required component file is not externally locked: ' + name)
    return Path(component['root']) / name


def _toolchain(value):
    _keys(value, ('schema', 'components', 'tools'), 'Invalid toolchain fields')
    need(value['schema'] == 'caesura.android-toolchain.v2', 'Invalid toolchain schema')
    _keys(value['components'], COMPONENTS, 'Exact toolchain component set required')
    components = {name: _inventory(item) for name, item in value['components'].items()}
    _keys(value['tools'], ROLES, 'Exact launcher set required')
    tools = {}
    for role, spec in value['tools'].items():
        _keys(spec, ('component', 'relative_path'), 'Invalid tool selection')
        need(spec['component'] == ROLES[role], 'Tool selected from wrong component')
        name = relative(spec['relative_path'])
        path = _covered(components[spec['component']], name)
        need(path.suffix.lower() not in ('.bat', '.cmd'), 'Shell tool wrappers are unsupported')
        tools[role] = lock(path)
    need(Path(tools['python']['path']) == Path(sys.executable).resolve(), 'Controller must use the locked Python executable')
    required = {'jdk': ('release', 'lib/modules'),
                'gradle': ('lib/gradle-gradle-cli-main-8.9.jar', 'lib/agents/gradle-instrumentation-agent-8.9.jar'),
                'ndk': ('source.properties', 'build/cmake/android.toolchain.cmake'),
                'sdk': ('platforms/android-35/android.jar', 'platforms/android-35/source.properties', 'build-tools/34.0.0/source.properties'),
                'sdl': ('lib/libSDL3.so', 'lib/cmake/SDL3/SDL3Config.cmake'),
                'openssl': ('lib/libssl.a', 'lib/libcrypto.a')}
    for name, entries in required.items():
        for entry in entries:
            _covered(components[name], entry)
    # A list of launcher hashes is insufficient: require full runtime/header/
    # sysroot trees at these known component boundaries, including future files.
    trees = dict(jdk=('bin', 'lib', 'conf'), gradle=('lib',),
                 ndk=('build/cmake', 'toolchains/llvm/prebuilt'),
                 sdk=('platforms/android-35', 'build-tools/34.0.0'),
                 sdl=('include', 'lib'), openssl=('include', 'lib'))
    for name, prefixes in trees.items():
        for prefix in prefixes:
            c = components[name]
            need((Path(c['root']) / prefix).is_dir() and any(p == '.' or Path(prefix).is_relative_to(p) for p in c['paths']),
                 'Full component runtime tree must be selected: ' + name + '/' + prefix)
    for name in ('sdl', 'openssl'):
        need(any(p.startswith('include/') for p in components[name]['files']), 'Locked dependency headers required')
    need(re.search(r'^JAVA_VERSION="17(?:\.|\")', _covered(components['jdk'], 'release').read_text(encoding='utf-8'), re.M), 'JDK17 metadata required')
    need(re.search(r'^Pkg.Revision\s*=\s*' + re.escape(NDK) + r'\s*$', _covered(components['ndk'], 'source.properties').read_text(encoding='utf-8'), re.M), 'Selected NDK revision differs')
    return dict(components=components, tools=tools)


def _dependencies(value):
    _keys(value, ('schema', 'inventory'), 'Invalid dependency input')
    need(value['schema'] == 'caesura.android-dependencies.v1', 'Invalid dependency schema')
    result = _inventory(value['inventory'])
    names = set(result['files'])
    need('verification-metadata.xml' in names and any(n.startswith('caches/modules-2/files-2.1/') for n in names), 'Locked Gradle artifacts and strict metadata required')
    need(all(n == 'verification-metadata.xml' or n.startswith('caches/modules-2/') for n in names), 'Dependency seed may only contain module cache and verification metadata')
    return result


def _request(value):
    keys = ('schema', 'repo', 'source_sha', 'configuration', 'abi', 'min_sdk', 'compile_sdk', 'target_sdk', 'stl',
            'package_name', 'version_name', 'version_code', 'game_relative_path', 'signing', 'toolchain', 'dependencies', 'jobs', 'timeouts')
    _keys(value, keys, 'Invalid request fields')
    fixed = dict(schema=INPUT_SCHEMA, configuration='Release', abi='arm64-v8a', min_sdk=24, compile_sdk=35,
                 target_sdk=35, stl='c++_static', package_name='com.caesura.app', game_relative_path='tests/projects/first_vn', signing='ephemeral-test')
    for key, wanted in fixed.items():
        need(type(value[key]) is type(wanted) and value[key] == wanted, 'Unsupported request ' + key)
    need(type(value['source_sha']) is str and re.fullmatch('[0-9a-f]{40}', value['source_sha']), 'Invalid source SHA')
    need(type(value['repo']) is str and Path(value['repo']).is_absolute(), 'Absolute source checkout required')
    need(type(value['version_name']) is str and re.fullmatch(r'\d+\.\d+\.\d+', value['version_name']), 'Invalid version')
    need(type(value['version_code']) is int and 0 < value['version_code'] < 2**31, 'Invalid version code')
    need(type(value['jobs']) is int and 1 <= value['jobs'] <= 2, 'Jobs must be 1 or 2')
    _keys(value['timeouts'], ('configure', 'compile', 'gradle', 'sign', 'verify'), 'Invalid timeout set')
    for name, number in value['timeouts'].items():
        limit = 300 if name == 'verify' else 7200
        need(type(number) is int and 0 < number <= limit, 'Invalid timeout')
    for key in ('toolchain', 'dependencies'):
        _keys(value[key], ('path', 'sha256'), 'Invalid external input lock')
    return copy.deepcopy(value)


def _env(toolchain, work):
    env = {k: os.environ[k] for k in ('SystemRoot', 'WINDIR', 'COMSPEC', 'SYSTEMDRIVE') if k in os.environ}
    component = toolchain['components']
    env.update(JAVA_HOME=component['jdk']['root'], ANDROID_HOME=component['sdk']['root'],
               ANDROID_SDK_ROOT=component['sdk']['root'], ANDROID_NDK_ROOT=component['ndk']['root'],
               GRADLE_USER_HOME=str(work / 'gradle-home'), HOME=str(work / 'home'), USERPROFILE=str(work / 'home'),
               TEMP=str(work / 'tmp'), TMP=str(work / 'tmp'), TMPDIR=str(work / 'tmp'), LANG='C', LC_ALL='C',
               GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL=os.devnull, GIT_TERMINAL_PROMPT='0')
    env['PATH'] = os.pathsep.join(sorted({str(Path(t['path']).parent) for t in toolchain['tools'].values()}))
    return env


def _source_link_file(repo, name, target):
    """Only a tracked source alias may use this single-hop file contract."""
    path = repo / relative(name)
    _no_links(path.parent)
    before = path.lstat()
    need(stat.S_ISLNK(before.st_mode) and not path.is_junction(), 'Expected physical source symlink: ' + name)
    need(type(target) is str and target and len(target.encode('utf-8')) <= 4096
         and not Path(target).is_absolute() and not any(c in target for c in ('\\', ':'))
         and all(ord(c) >= 32 for c in target), 'Unsafe source link target: ' + name)
    need(os.readlink(path) == target, 'Source link target differs from Git index: ' + name)
    terminal = path.parent
    for component in target.split('/'):
        # Do not collapse x/.. lexically: x might be a directory link, missing,
        # or a regular file. Validate each actual prefix before consuming '..'.
        _no_links(terminal)
        need(terminal.is_dir(), 'Source link intermediate component is not a directory')
        if component == '..':
            need(terminal != repo, 'Source link escapes checkout: ' + name)
            terminal = terminal.parent
        elif component not in ('', '.'):
            terminal = terminal / component
    # Reject chains, directory links and all reparse/junction parents, even if
    # following them could eventually produce an in-checkout regular file.
    terminal = _no_links(terminal)
    info = terminal.stat()
    need(stat.S_ISREG(info.st_mode) and info.st_nlink == 1, 'Source link must end at a single-link regular file')
    need(path.resolve(strict=True) == terminal, 'Source link OS endpoint differs from its checked components')
    after = path.lstat()
    signature = lambda s: (s.st_dev, s.st_ino, s.st_mode, s.st_size, s.st_mtime_ns)
    need(signature(before) == signature(after) and os.readlink(path) == target, 'Source link changed while observed')
    return terminal


def _source_tree(repo, selected, files, links):
    """Enumerate actual compile inputs without traversing any directory link."""
    stack, count = [repo / selected], 0
    while stack:
        path = stack.pop()
        count += 1
        need(count + len(stack) <= MAX_FILES, 'Source inventory exceeds entry limit')
        info = path.lstat()
        name = path.relative_to(repo).as_posix()
        need(not path.is_junction(), 'Source inventory rejects junctions')
        if stat.S_ISLNK(info.st_mode):
            need(name in links, 'Undeclared source link: ' + name)
            terminal = _source_link_file(repo, name, links[name]['target'])
            need(terminal.relative_to(repo).as_posix() == links[name]['resolved'], 'Source link endpoint changed')
            file_lock(terminal, files[name])
        elif stat.S_ISDIR(info.st_mode):
            _no_links(path)
            for child in path.iterdir():
                stack.append(child)
                need(count + len(stack) <= MAX_FILES, 'Source inventory exceeds entry limit')
        else:
            need(name in files and name not in links, 'Undeclared compile/stage input: ' + name)
            need(lock(path)['sha256'] == files[name], 'Source file changed while enumerating: ' + name)


def _source(repo, git, env):
    repo = _no_links(repo)
    need((repo / '.git').exists(), 'Git checkout required')
    def read(*args):
        return subprocess.run([git, '--no-optional-locks', '-c', 'core.fsmonitor=false', *args], cwd=repo, env=env,
                              capture_output=True, check=True, timeout=30).stdout
    head = read('rev-parse', 'HEAD').decode('ascii').strip()
    need(not read('status', '--porcelain', '-z', '--untracked-files=all'), 'Clean source checkout required')
    raw_index = read('ls-files', '--stage', '-z')
    entries = {}
    for row in raw_index.split(b'\0'):
        if not row:
            continue
        metadata, raw_name = row.split(b'\t', 1)
        mode, oid, stage = metadata.decode('ascii').split()
        name = raw_name.decode('utf-8')
        relative(name)
        need(name not in entries and stage == '0' and mode in ('100644', '100755', '120000'), 'Unsupported source index entry')
        entries[name] = dict(mode=mode, git_oid=oid)
    need(0 < len(entries) <= MAX_FILES, 'Invalid source file set')
    files, links = {}, {}
    for name, entry in entries.items():
        if entry['mode'] != '120000':
            files[name] = lock(repo / name)['sha256']
    for name, entry in entries.items():
        if entry['mode'] == '120000':
            target = read('cat-file', 'blob', entry['git_oid']).decode('utf-8')
            terminal = _source_link_file(repo, name, target)
            resolved = terminal.relative_to(repo).as_posix()
            need(resolved in files and entries[resolved]['mode'] in ('100644', '100755'),
                 'Source link endpoint must be a tracked regular file')
            file_lock(terminal, files[resolved])
            files[name] = files[resolved]
            links[name] = dict(entry, target=target, resolved=resolved, target_mode=entries[resolved]['mode'])
    # CMake glob/include and package inputs must not consume Git-ignored extras.
    for selected in ('src', 'cmake', 'external', 'scripts', 'assets', 'fonts', 'lang', 'tests'):
        if not os.path.lexists(repo / selected):
            continue
        # The controller itself may create Python bytecode; disable bytecode in
        # the CLI before import and require clean selected inputs for acceptance.
        _source_tree(repo, selected, files, links)
    need(read('ls-files', '--stage', '-z') == raw_index and read('rev-parse', 'HEAD').decode('ascii').strip() == head
         and not read('status', '--porcelain', '-z', '--untracked-files=all'), 'Source Git identity changed during selection')
    return dict(source_sha=head, files=files, **({'links': links} if links else {}))


class Commands:
    def __init__(self, report, work, env, runner):
        self.report, self.work, self.env = report, work, env
        self.execute = runner or run_runtime_command

    def run(self, name, argv, timeout, cwd=None):
        self.report['stage'] = name
        directory = self.work / 'commands' / name
        directory.mkdir()
        out, err = directory / 'stdout', directory / 'stderr'
        item = dict(name=name, argv=argv, process_files=[])
        self.report['commands'].append(item)
        with out.open('xb') as stdout, err.open('xb') as stderr:
            try:
                result = self.execute(argv, cwd=cwd or self.work, env=self.env, control_dir=directory / 'process',
                                      stdout=stdout, stderr=stderr, timeout=timeout)
                item['process'] = result
            finally:
                stdout.flush(); stderr.flush()
                item.update(stdout=lock(out), stderr=lock(err))
                if (directory / 'process').exists():
                    for p in sorted((directory / 'process').iterdir()):
                        item['process_files'].append(lock(p))
        need(result.get('status') == 'EXITED' and type(result.get('actual_exit_code')) is int
             and result['actual_exit_code'] == 0 and result.get('owned_tree_cleanup') == 'COMPLETE'
             and not result.get('forced_kill') and not result.get('timed_out'), 'Owned command failed: ' + name)
        need(out.stat().st_size + err.stat().st_size <= MAX_LOG, 'Command log exceeds limit')
        return out.read_text(encoding='utf-8', errors='strict') + '\n' + err.read_text(encoding='utf-8', errors='strict')


def _copy(source, target, digest):
    file_lock(source, digest)
    target.parent.mkdir(parents=True, exist_ok=True)
    with Path(source).open('rb') as src, target.open('xb') as dst:
        shutil.copyfileobj(src, dst)
    file_lock(source, digest); file_lock(target, digest)


def _cmake_cache(path):
    # CMake inserts blank lines and // comments between entries. Parse one
    # physical line at a time so character classes cannot consume newlines.
    data = {}
    for line in path.read_text(encoding='utf-8').splitlines():
        if not line.strip() or line.startswith(('#', '//')):
            continue
        entry = re.fullmatch(r'([^#/:=\r\n][^:=\r\n]*):[^=\r\n]+=(.*)', line)
        need(entry is not None, 'Malformed CMake cache entry')
        key, value = entry.groups()
        need(key not in data, 'Duplicate CMake cache entry: ' + key)
        data[key] = value
    return data


def _cmake_toolchains(native, value, tc, cache):
    """Follow this completed configure's File API index, never a reply glob."""
    reply = _no_links(native / '.cmake/api/v1/reply')
    indexes = [p for p in reply.iterdir() if p.name.startswith('index-') and p.name.endswith('.json')]
    need(len(indexes) == 1, 'CMake File API requires one completed reply index')
    index_lock = lock(indexes[0])
    index = load(index_lock['path'], index_lock['sha256'])
    need(type(index) is dict and type(index.get('cmake')) is dict, 'Invalid CMake File API index')
    cmake = index['cmake']
    need(type(cmake.get('paths')) is dict and type(cmake['paths'].get('cmake')) is str
         and Path(cmake['paths']['cmake']).is_absolute()
         and _no_links(cmake['paths']['cmake']) == Path(tc['tools']['cmake']['path']), 'File API CMake identity differs')
    need(type(cmake.get('generator')) is dict and cmake['generator'].get('name') == 'Ninja'
         and cmake['generator'].get('multiConfig') is False, 'File API generator differs')
    objects, replies = index.get('objects'), index.get('reply')
    need(type(objects) is list and all(type(item) is dict for item in objects)
         and type(replies) is dict, 'Invalid CMake File API references')
    selected, evidence = {}, [index_lock]
    for kind, major in (('codemodel', 2), ('toolchains', 1)):
        ref = replies.get(f'{kind}-v{major}')
        need(type(ref) is dict and ref.get('kind') == kind and type(ref.get('version')) is dict,
             'Missing CMake File API query response: ' + kind)
        version = ref['version']
        need(type(version.get('major')) is int and version['major'] == major
             and type(version.get('minor')) is int and version['minor'] >= 0, 'Invalid File API object version')
        matches = [item for item in objects if item.get('kind') == kind]
        need(matches == [ref], 'File API index object differs or is duplicated: ' + kind)
        name = relative(ref.get('jsonFile'))
        need(Path(name).name == name and name.endswith('.json'), 'Unsafe File API reply reference')
        record = lock(reply / name)
        data = load(record['path'], record['sha256'])
        need(type(data) is dict and data.get('kind') == kind and type(data.get('version')) is dict
             and all(type(data['version'].get(key)) is int for key in ('major', 'minor'))
             and data['version'] == version,
             'File API response kind/version differs from index')
        selected[kind] = data; evidence.append(record)
    paths = selected['codemodel'].get('paths')
    need(type(paths) is dict, 'File API codemodel paths missing')
    for name, wanted in (('source', Path(value['repo'])), ('build', native)):
        need(type(paths.get(name)) is str and Path(paths[name]).is_absolute()
             and _no_links(paths[name]) == wanted, 'File API codemodel path differs: ' + name)
    chains = selected['toolchains'].get('toolchains')
    need(type(chains) is list and all(type(item) is dict for item in chains), 'Invalid File API toolchains')
    component = tc['components']['ndk']; ndk = Path(component['root'])
    compilers = {}
    for language in ('C', 'CXX'):
        matches = [item for item in chains if item.get('language') == language]
        need(len(matches) == 1 and type(matches[0].get('compiler')) is dict,
             'Missing or duplicated File API compiler: ' + language)
        compiler = matches[0]['compiler']; name = compiler.get('path')
        need(type(name) is str and Path(name).is_absolute(), 'Absolute File API compiler path required')
        path = _no_links(name)
        need(path.is_relative_to(ndk), 'Compiler is outside selected NDK')
        rel = path.relative_to(ndk).as_posix()
        _covered(component, rel)
        file_lock(path, component['files'][rel])
        key = f'CMAKE_{language}_COMPILER'
        if key in cache:
            need(Path(cache[key]).is_absolute() and _no_links(cache[key]) == path,
                 'Explicit cache compiler differs from File API: ' + key)
        compilers[language] = dict(path=str(path), sha256=component['files'][rel])
    return compilers, evidence


def _build_native(value, tc, command, work):
    tool = lambda n: tc['tools'][n]['path']
    root = lambda n: Path(tc['components'][n]['root'])
    native = work / 'native'
    native.mkdir()
    query = native / '.cmake/api/v1/query'
    query.mkdir(parents=True)
    (query / 'codemodel-v2').write_bytes(b'')
    (query / 'toolchains-v1').write_bytes(b'')
    query_locks = [lock(query / name) for name in ('codemodel-v2', 'toolchains-v1')]
    argv = [tool('cmake'), '-S', value['repo'], '-B', str(native), '-G', 'Ninja',
            '-DCMAKE_MAKE_PROGRAM=' + tool('ninja'), '-DCMAKE_TOOLCHAIN_FILE=' + str(root('ndk') / 'build/cmake/android.toolchain.cmake'),
            '-DANDROID_NDK=' + str(root('ndk')), '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-24',
            '-DANDROID_STL=c++_static', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
            '-DCAESURA_LIVE2D=OFF', '-DCAESURA_ENABLE_FFMPEG=OFF', '-DSOLOUD_BACKEND_OPENSLES=ON',
            '-DSDL3_DIR=' + str(root('sdl') / 'lib/cmake/SDL3'), '-DOPENSSL_ROOT_DIR=' + str(root('openssl')),
            '-DOPENSSL_USE_STATIC_LIBS=TRUE', '-DPython3_EXECUTABLE=' + tool('python')]
    command.run('configure', argv, value['timeouts']['configure'])
    command.run('compile', [tool('cmake'), '--build', str(native), '--target', 'CaesuraAmeKAG', '--parallel', str(value['jobs']), '--verbose'], value['timeouts']['compile'])
    cache = native / 'CMakeCache.txt'
    data = _cmake_cache(cache)
    exact = dict(CMAKE_BUILD_TYPE='Release', CMAKE_GENERATOR='Ninja', ANDROID_ABI='arm64-v8a',
                 ANDROID_PLATFORM='android-24', ANDROID_STL='c++_static', CAESURA_LIVE2D='OFF',
                 CAESURA_ENABLE_FFMPEG='OFF', SOLOUD_BACKEND_OPENSLES='ON')
    for key, expected in exact.items():
        need(data.get(key) == expected, 'Actual CMake selection differs: ' + key)
    for key, expected in dict(CMAKE_HOME_DIRECTORY=Path(value['repo']), CMAKE_MAKE_PROGRAM=Path(tool('ninja')),
                              CMAKE_TOOLCHAIN_FILE=root('ndk') / 'build/cmake/android.toolchain.cmake',
                              SDL3_DIR=root('sdl') / 'lib/cmake/SDL3', OPENSSL_ROOT_DIR=root('openssl')).items():
        need(key in data and Path(data[key]).resolve() == expected.resolve(), 'Actual CMake path differs: ' + key)
    # Compilers are not necessarily cache entries (e.g. NDK with CMake 4).
    # Read actual compiler selections through our requested File API replies.
    for item in query_locks:
        file_lock(item['path'], item['sha256'])
    compilers, api_evidence = _cmake_toolchains(native, value, tc, data)
    compiles = native / 'compile_commands.json'
    entries = load(compiles, _sha256_file(compiles))
    need(type(entries) is list and entries, 'Actual compile commands missing')
    for entry in entries:
        need(type(entry) is dict and type(entry.get('file')) is str and Path(entry['file']).is_absolute(), 'Invalid compile source')
        source = Path(entry['file']).resolve()
        need(source.is_relative_to(Path(value['repo'])) or source.is_relative_to(native), 'Compile input outside controlled source/build')
        line = entry.get('command', '')
        need(type(line) is str and '--target=aarch64' in line and 'android24' in line and str(root('ndk')).replace('\\', '/') in line.replace('\\', '/'), 'Actual compile target/toolchain differs')
    compiled = native / 'libCaesuraAmeKAG.so'
    package._elf(compiled)
    output = command.run('elf', [tool('readelf'), '--dyn-syms', '--dynamic', str(compiled)], value['timeouts']['configure'])
    need(re.search(r'\bGLOBAL\s+DEFAULT\s+(?!UND\b)\S+\s+SDL_main\s*$', output, re.M), 'Compiled JNI has no exported SDL_main')
    needed = re.findall(r'\(NEEDED\).*\[([^\]]+)\]', output)
    platform = {'libandroid.so', 'liblog.so', 'libGLESv1_CM.so', 'libGLESv2.so', 'libGLESv3.so', 'libEGL.so',
                'libOpenSLES.so', 'libm.so', 'libdl.so', 'libc.so', 'libz.so', 'libjnigraphics.so', 'libvulkan.so', 'libmediandk.so'}
    need('libSDL3.so' in needed and set(needed) <= platform | {'libSDL3.so'}, 'Unexplained native shared dependency')
    sdl = _covered(tc['components']['sdl'], 'lib/libSDL3.so')
    package._elf(sdl)
    sdl_output = command.run('elf-sdl', [tool('readelf'), '--dynamic', str(sdl)], value['timeouts']['configure'])
    sdl_needed = re.findall(r'\(NEEDED\).*\[([^\]]+)\]', sdl_output)
    need(set(sdl_needed) <= platform, 'Unstaged SDL shared dependency')
    evidence = [lock(cache), lock(compiles), lock(native / 'build.ninja'), *query_locks, *api_evidence]
    reply = native / '.cmake/api/v1/reply'
    for path in sorted(reply.iterdir()):
        if str(path) not in {item['path'] for item in evidence}:
            evidence.append(lock(path))
    return dict(library=lock(compiled), build_evidence=evidence, needed=needed, sdl_needed=sdl_needed, compilers=compilers)


def _stage(value, source, native, tc, dep, work):
    repo = Path(value['repo']); stage = work / 'android-stage'
    stage.mkdir()
    stage_files = {}
    def copy_file(name, dest):
        digest = source['files'][name]
        link = source.get('links', {}).get(name)
        original = _source_link_file(repo, name, link['target']) if link else repo / name
        if link:
            need(original.relative_to(repo).as_posix() == link['resolved'], 'Source link endpoint changed before stage')
        _copy(original, stage / dest, digest)
        if link:
            need(_source_link_file(repo, name, link['target']) == original, 'Source link changed during stage')
            file_lock(original, digest)
        stage_files[dest] = dict(source=name, source_sha256=digest, sha256=digest)
    project_exact = {'android/build.gradle', 'android/settings.gradle', 'android/gradle.properties',
                     'android/app/build.gradle', 'android/app/proguard-rules.pro', 'android/app/src/main/AndroidManifest.xml'}
    need(project_exact <= source['files'].keys(), 'Android project inputs missing')
    for name in sorted(source['files']):
        if name in project_exact or name.startswith(('android/app/src/main/java/', 'android/app/src/main/res/', 'android/app/libs/')):
            copy_file(name, name[len('android/'):])
        destination = None
        if name.startswith('scripts/') and name.endswith('.lua'):
            destination = 'app/src/main/assets/game/' + name
        elif name.startswith(('assets/', 'fonts/', 'lang/')):
            destination = 'app/src/main/assets/game/' + name
        elif name.startswith(value['game_relative_path'] + '/'):
            destination = 'app/src/main/assets/game/demo/first_vn/' + name[len(value['game_relative_path']) + 1:]
        if destination:
            copy_file(name, destination)
    config = stage / 'app/src/main/assets/game/scripts/config.lua'
    text = config.read_text(encoding='utf-8')
    text, count = re.subn(r'(?m)^(config\.entry_script\s*=\s*)["\'][^"\'\r\n]*["\']', r'\1"../demo/first_vn/entry.lua"', text)
    need(count == 1, 'Expected one entry_script declaration')
    config.write_text(text, encoding='utf-8', newline='')
    stage_files[config.relative_to(stage).as_posix()].update(sha256=_sha256_file(config), transform='select-first-vn-entry-v1')
    for required in ('scripts/kag/init.lua', 'demo/first_vn/entry.lua'):
        need((stage / 'app/src/main/assets/game' / required).is_file(), 'Required runtime asset missing')
    for name, selected in (('libCaesuraAmeKAG.so', native['library']), ('libSDL3.so', lock(_covered(tc['components']['sdl'], 'lib/libSDL3.so')))):
        package._elf(Path(selected['path']))
        dest = stage / 'app/src/main/jniLibs/arm64-v8a' / name
        _copy(selected['path'], dest, selected['sha256'])
        stage_files[dest.relative_to(stage).as_posix()] = dict(source=selected['path'], source_sha256=selected['sha256'], sha256=selected['sha256'])
    for name, digest in dep['files'].items():
        dest = stage / 'gradle/verification-metadata.xml' if name == 'verification-metadata.xml' else work / 'gradle-home' / name
        _copy(Path(dep['root']) / name, dest, digest)
    # Java properties escapes Windows separators/colon, without embedding secrets.
    prop = lambda n: tc['components'][n]['root'].replace('\\', '/').replace(':', '\\:')
    # AGP still supports explicit ndk.dir when its revision matches ndkVersion.
    # This intentionally avoids selecting another SDK-side installed NDK.
    (stage / 'local.properties').write_text('sdk.dir=' + prop('sdk') + '\nndk.dir=' + prop('ndk') + '\n', encoding='utf-8')
    return dict(path=str(stage), origin=stage_files, snapshot=_stage_snapshot(stage))


def _stage_snapshot(stage):
    paths = [p.name for p in stage.iterdir() if p.name not in ('.gradle', 'build', 'app')]
    paths += ['app/' + p.name for p in (stage / 'app').iterdir() if p.name != 'build']
    return dict(paths=sorted(paths), **_tree(stage, sorted(paths)))


def _gradle(value, tc, stage, work, command):
    gradle = Path(tc['components']['gradle']['root']); tool = tc['tools']['java']['path']
    args = [tool, '-Xmx64m', '-Xms64m', '-javaagent:' + str(gradle / 'lib/agents/gradle-instrumentation-agent-8.9.jar'),
            '-Dorg.gradle.appname=gradle', '-Duser.home=' + str(work / 'home'), '-Duser.language=en', '-Duser.country=US', '-Dfile.encoding=UTF-8',
            '-classpath', str(gradle / 'lib/gradle-gradle-cli-main-8.9.jar'), 'org.gradle.launcher.GradleMain',
            '--project-dir', stage['path'], '--gradle-user-home', str(work / 'gradle-home'),
            '--no-daemon', '--console=plain', '--offline', '--dependency-verification', 'strict', '--no-build-cache',
            '--no-configuration-cache', '--max-workers=' + str(value['jobs']), '-Dorg.gradle.java.home=' + tc['components']['jdk']['root'],
            '-PcaesuraVersionName=' + value['version_name'], '-PcaesuraVersionCode=' + str(value['version_code']),
            '-PcaesuraNdkVersion=' + NDK, '-PcaesuraBuildToolsVersion=34.0.0', '-PcaesuraKeepJniBytes=true',
            '-Pandroid.builder.sdkDownload=false',
            ':app:assembleRelease', ':app:bundleRelease']
    version = command.run('gradle-version', args[:-2] + ['--version'], value['timeouts']['gradle'], Path(stage['path']))
    need(re.findall(r'^Gradle ([^\r\n]+)\s*$', version, re.M) == ['8.9'], 'Actual Gradle version differs')
    command.run('gradle', args, value['timeouts']['gradle'], Path(stage['path']))
    need(_stage_snapshot(Path(stage['path'])) == stage['snapshot'], 'Gradle modified staged input')
    unsigned = {}
    for kind, name in (('apk', 'app/build/outputs/apk/release/app-release-unsigned.apk'), ('aab', 'app/build/outputs/bundle/release/app-release.aab')):
        produced = lock(Path(stage['path']) / name)
        target = work / 'unsigned' / ('unsigned.' + kind)
        _copy(produced['path'], target, produced['sha256'])
        need(0 < target.stat().st_size <= package.MAX_ARCHIVE, 'Unsigned archive exceeds size limit')
        with zipfile.ZipFile(target) as archive:
            entries = archive.infolist()
            need(len(entries) <= package.MAX_ENTRIES and sum(e.file_size for e in entries) <= package.MAX_EXPANDED, 'Unsigned ZIP exceeds expanded limits')
        prepared = prepare_package(target, work / ('unsigned-' + kind), expected_sha256=produced['sha256'])
        root = Path(prepared['package_path'])
        mapping = _tree(root, ['.'])['files']
        need(not any(n.upper().startswith('META-INF/') and n.upper().endswith(('.RSA', '.DSA', '.EC', '.SF')) for n in mapping), 'Gradle output must be unsigned')
        # Independently locked JNI/assets must be the complete archive sets.
        expected = {}
        for rel, record in stage['origin'].items():
            if rel.startswith('app/src/main/assets/'):
                key = 'assets/' + rel[len('app/src/main/assets/'):]
            elif rel.startswith('app/src/main/jniLibs/'):
                key = 'lib/' + rel[len('app/src/main/jniLibs/'):]
            else:
                continue
            expected[('base/' if kind == 'aab' else '') + key] = record['sha256']
        prefixes = ('base/assets/', 'base/lib/') if kind == 'aab' else ('assets/', 'lib/')
        need({n: h for n, h in mapping.items() if n.startswith(prefixes)} == expected, 'Packaged JNI/assets differ from independent stage')
        # JAR/APK v1 signing owns the JAR manifest and signature blocks. Preserve
        # its unsigned bytes in the raw/prepared lock, not as final payload bytes.
        payload = {n: h for n, h in mapping.items() if n != JAR_MANIFEST}
        unsigned[kind] = dict(file=lock(target), producer_file=produced, prepared=prepared,
                              entries=payload, all_entries=mapping,
                              signature_transform='JAR manifest/signature blocks only; business entries unchanged')
    return unsigned


def _sign(value, tc, unsigned, command, work, report):
    private = work / 'private-signing'
    private.mkdir(mode=0o700)
    report['private_cleanup'] = 'PENDING'
    password = private / 'password.txt'; key = private / 'test.p12'
    with password.open('xb') as stream:
        os.chmod(password, 0o600)
        # apksigner consumes this shared file once for each password option.
        # keytool/jarsigner independently open it and consume the first line.
        line = secrets.token_urlsafe(48).encode('ascii') + b'\n'
        stream.write(line + line)
    tool = lambda n: tc['tools'][n]['path']
    flags = [*package.JAVA_FLAGS, '-J-Duser.home=' + str(work / 'home')]
    timeout = value['timeouts']['sign']
    command.run('test-key', [tool('keytool'), *flags, '-genkeypair', '-noprompt', '-storetype', 'PKCS12', '-keystore', str(key),
                            '-storepass:file', str(password), '-keypass:file', str(password), '-alias', 'caesura-test',
                            '-keyalg', 'RSA', '-keysize', '2048', '-validity', '2', '-dname', 'CN=Caesura TEST ONLY,O=Unpublished Test'], timeout)
    need(key.is_file(), 'Test key was not produced')
    certificate = work / 'outputs/test-certificate.der'
    command.run('test-cert', [tool('keytool'), *flags, '-exportcert', '-alias', 'caesura-test', '-keystore', str(key),
                             '-storepass:file', str(password), '-file', str(certificate)], timeout)
    cert = lock(certificate)
    need(certificate.stat().st_size > 0, 'Empty test certificate')
    aligned = work / 'aligned/aligned.apk'
    command.run('align', [tool('zipalign'), '-v', '4', unsigned['apk']['file']['path'], str(aligned)], timeout)
    prefix = 'CaesuraAmeKAG-' + value['version_name'] + '-Android-arm64-v8a-test'
    apk, aab = work / 'outputs' / (prefix + '.apk'), work / 'outputs' / (prefix + '.aab')
    command.run('sign-apk', [tool('java'), '-Duser.language=en', '-Duser.country=US', '-Dfile.encoding=UTF-8',
                             '-Duser.home=' + str(work / 'home'), '-jar', tool('apksigner_jar'), 'sign', '--ks', str(key),
                             '--ks-key-alias', 'caesura-test', '--ks-pass', 'file:' + str(password), '--key-pass', 'file:' + str(password),
                             '--v1-signer-name', SIGNER_NAME, '--out', str(apk), str(aligned)], timeout)
    command.run('sign-aab', [tool('jarsigner'), *flags, '-keystore', str(key), '-storepass:file', str(password),
                            '-keypass:file', str(password), '-sigfile', SIGNER_NAME,
                            '-signedjar', str(aab), unsigned['aab']['file']['path'], 'caesura-test'], timeout)
    return dict(apk=lock(apk), aab=lock(aab), certificate=cert, aligned=lock(aligned))


def _signature_closure(report):
    """Only the fixed RSA signer's exact JAR entries may change during signing.

    Actual signature validity belongs to the package verifier. Its bounded safe
    ZIP preparation also bounds these metadata bytes (100k entries / 4 GiB
    expanded); manifests may legitimately grow with the whole business set.
    APK v2/v3 signature blocks are outside the ZIP file-entry namespace.
    """
    result = {}
    for kind in ('apk', 'aab'):
        unsigned = report['unsigned'][kind]
        before = _tree(Path(unsigned['prepared']['package_path']), ['.'])['files']
        need(before == unsigned['all_entries'], 'Unsigned business inventory changed')
        business = {n: h for n, h in before.items() if n != JAR_MANIFEST}
        need(business == unsigned['entries'], 'Unsigned business selection changed')
        root = Path(report['package']['prepared'][kind]['package_path'])
        after = _tree(root, ['.'])['files']
        metadata = {n: h for n, h in after.items() if n in SIGNATURE_ENTRIES}
        has_v1 = bool(set(metadata) - {JAR_MANIFEST})
        if kind == 'aab' or has_v1:
            need(set(metadata) == SIGNATURE_ENTRIES, 'Signature metadata set is incomplete or missing: ' + kind)
            need(all((root / n).stat().st_size > 0 for n in metadata), 'Signature metadata must be nonempty: ' + kind)
        else:
            # With no APK v1 signature, no signer owns a manifest transform.
            need(after.get(JAR_MANIFEST) == before.get(JAR_MANIFEST), 'Unsigned APK manifest changed without v1 signing')
        actual_business = {n: h for n, h in after.items() if n not in metadata}
        need(actual_business == business, 'Signing changed business entries: ' + kind)
        result[kind] = dict(business_entries=actual_business, metadata=metadata)
    return result


def _cleanup_private(work, report):
    private = work / 'private-signing'
    if not private.exists():
        report['private_cleanup'] = 'NOT_CREATED'
        return
    _no_links(private)
    # Only the exact files this attempt creates may be removed, never a glob.
    unknown = {p.name for p in private.iterdir()} - {'password.txt', 'test.p12'}
    errors = []
    for name in ('password.txt', 'test.p12'):
        path = private / name
        if path.exists():
            try:
                lock(path)
                path.unlink()
            except (ValueError, OSError) as error:
                errors.append(name + ': ' + str(error))
    # Unknown tool output is a failed cleanup, but cannot prevent attempts to
    # remove both known secrets. Keep the original tool failure in the receipt.
    if unknown or errors:
        report['private_cleanup'] = 'FAILED'
        raise ValueError('Unexpected private signing output or cleanup failure: ' + repr(sorted(unknown)) + '; '.join(errors))
    private.rmdir()
    report['private_cleanup'] = 'COMPLETE'


def _check(report):
    value = load(report['request']['path'], report['request']['sha256'])
    need(value == report['inputs'], 'Original request changed')
    for name in ('toolchain', 'dependencies'):
        need(load(value[name]['path'], value[name]['sha256']) == report['external_inputs'][name], 'External control input changed')
    for component in report['toolchain']['components'].values():
        _inventory_stable(component)
    _inventory_stable(report['dependencies'])
    work = Path(report['work']); tc = report['toolchain']
    for name, digest in report['dependencies']['files'].items():
        if name.startswith('caches/modules-2/files-2.1/'):
            file_lock(work / 'gradle-home' / name, digest)
    need(_source(Path(value['repo']), tc['tools']['git']['path'], _env(tc, work)) == report['source'], 'Source changed during validation')
    need(_stage_snapshot(Path(report['staging']['path'])) == report['staging']['snapshot'], 'Staging changed')
    for selected in report['native']['build_evidence'] + [report['native']['library']] + list(report['outputs'].values()):
        file_lock(selected['path'], selected['sha256'])
    for item in report['unsigned'].values():
        for name in ('file', 'producer_file'):
            file_lock(item[name]['path'], item[name]['sha256'])
        verify_stable(item['prepared'])
    for command in report['commands']:
        for selected in [command['stdout'], command['stderr'], *command['process_files']]:
            file_lock(selected['path'], selected['sha256'])
    package.verify_android_package_stable(report['package'])
    need(_signature_closure(report) == report['signature_transform'], 'Signature transform changed after validation')
    need(not (work / 'private-signing').exists() and report['private_cleanup'] == 'COMPLETE', 'Private signing files remain')


def verify_android_validation_stable(result):
    need(type(result) is dict and result.get('schema') == SCHEMA and result.get('status') in ('ANDROID_VALIDATION_VERIFIED', 'FIXTURE_ONLY') and result.get('release_ready') is False, 'Completed Android validation required')
    saved = load(result['receipt_path'], result['receipt_sha256'])
    need(saved == {k: v for k, v in result.items() if k != 'receipt_sha256'}, 'Driver receipt/selection changed')
    _check(result)
    return dict(status='ANDROID_VALIDATION_STABLE', release_ready=False)


def run_android_validation(request_path, request_sha256, work_dir, *, runner=None):
    work = _new_work(work_dir)
    receipt = work / 'android-validation.json'
    report = dict(schema=SCHEMA, status='FAIL', release_ready=False, runtime='NOT_RUN', device='NOT_RUN', install='NOT_RUN',
                  stage='inputs', work=str(work), receipt_path=str(receipt), commands=[], errors=[], private_cleanup='NOT_CREATED',
                  transport='fixture' if runner is not None else 'local-owned-tools',
                  limitations=['No hosted attestation', 'Prebuilt SDL/OpenSSL compilation provenance NOT_VERIFIED',
                               'AAB manifest verification covers base identity/version/SDK only', 'Host OS/runtime environment is not hermetic'])
    try:
        value = _request(load(request_path, request_sha256))
        report.update(inputs=value, request=lock(request_path))
        external = {n: load(value[n]['path'], value[n]['sha256']) for n in ('toolchain', 'dependencies')}
        tc = _toolchain(external['toolchain']); dep = _dependencies(external['dependencies'])
        repo = _no_links(value['repo'])
        need(str(repo) == value['repo'], 'Canonical source path required')
        inputs = [repo, Path(request_path), Path(value['toolchain']['path']), Path(value['dependencies']['path']),
                  Path(dep['root']), *[Path(c['root']) for c in tc['components'].values()]]
        need(not any(p.is_relative_to(work) or work.is_relative_to(p) for p in inputs), 'Work must be outside all source/tool/input trees')
        for name in ('home', 'tmp', 'commands', 'gradle-home', 'unsigned', 'aligned', 'outputs'):
            (work / name).mkdir()
        env = _env(tc, work)
        source = _source(repo, tc['tools']['git']['path'], env)
        need(source['source_sha'] == value['source_sha'], 'Source HEAD differs')
        version = re.findall(r'project\s*\(\s*CaesuraAmeKAG\s+VERSION\s+(\d+\.\d+\.\d+)', (repo / 'CMakeLists.txt').read_text(encoding='utf-8'), re.I)
        need(version == [value['version_name']], 'CMake version differs')
        if runner is None:
            need(Path(__file__).resolve() == repo / 'scripts/run_android_validation.py', 'Actual driver must come from selected clean source')
        report.update(external_inputs=external, toolchain=tc, dependencies=dep, source=source)
        command = Commands(report, work, env, runner)
        java = command.run('java-version', [tc['tools']['java']['path'], '-version'], value['timeouts']['configure'])
        need(re.search(r'\bversion "17(?:\.|\")', java), 'Actual Java is not JDK17')
        for name in ('cmake', 'ninja', 'clang', 'readelf'):
            text = command.run(name + '-version', [tc['tools'][name]['path'], '--version'], value['timeouts']['configure'])
            need(text.strip(), 'Version probe returned no identity')
        report['native'] = _build_native(value, tc, command, work)
        report['staging'] = _stage(value, source, report['native'], tc, dep, work)
        report['unsigned'] = _gradle(value, tc, report['staging'], work, command)
        report['outputs'] = _sign(value, tc, report['unsigned'], command, work, report)
        report['stage'] = 'final-package-verification'
        expected = {n: value[n] for n in ('source_sha', 'package_name', 'version_name', 'version_code', 'abi', 'min_sdk', 'target_sdk')}
        expected.update(certificate_sha256=report['outputs']['certificate']['sha256'],
                        required_apk_entries=report['unsigned']['apk']['entries'], required_aab_entries=report['unsigned']['aab']['entries'])
        report['package'] = package.verify_android_package(
            apk_path=report['outputs']['apk']['path'], apk_sha256=report['outputs']['apk']['sha256'],
            aab_path=report['outputs']['aab']['path'], aab_sha256=report['outputs']['aab']['sha256'],
            expected=expected, tools={n: tc['tools'][n] for n in package.TOOL_NAMES}, work_dir=work / 'verify',
            runner=runner, timeout=value['timeouts']['verify'])
        need(report['package']['status'] == ('FIXTURE_PACKAGE_VERIFIED' if runner is not None else 'ANDROID_PACKAGE_VERIFIED'), 'Unexpected package verification scope')
        report['signature_transform'] = _signature_closure(report)
        _cleanup_private(work, report)
        report['stage'] = 'stability'; _check(report)
        report.update(status='FIXTURE_ONLY' if runner is not None else 'ANDROID_VALIDATION_VERIFIED', stage='complete',
                      local_provenance='FIXTURE_ONLY' if runner is not None else 'OWNED_COMPILE_PACKAGE_TEST_SIGNATURE_VERIFIED')
    except Exception as error:
        report['status'] = 'FAIL'; report['errors'].append(str(error))
        raise
    finally:
        try:
            if (work / 'private-signing').exists() and report['private_cleanup'] != 'FAILED':
                _cleanup_private(work, report)
        except Exception as error:
            report.update(status='FAIL', private_cleanup='FAILED')
            report['errors'].append('Private cleanup: ' + str(error))
        save(receipt, report)
    need(report['status'] != 'FAIL', 'Private signing cleanup failed')
    return dict(report, receipt_sha256=_sha256_file(receipt))


def main(argv=None, *, runner=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--request', type=Path, required=True)
    parser.add_argument('--request-sha256', required=True)
    parser.add_argument('--work', type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        result = run_android_validation(args.request, args.request_sha256, args.work, runner=runner)
        print(json.dumps(result, ensure_ascii=True))
        return 77 if result['status'] == 'FIXTURE_ONLY' else 0
    except (ValueError, RuntimeError, OSError, KeyError, TypeError, subprocess.SubprocessError, zipfile.BadZipFile) as error:
        print(json.dumps(dict(schema=SCHEMA, status='FAIL', release_ready=False, error=str(error))))
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
