"""Copy, relocate and ad-hoc sign one explicitly selected macOS install stage.

Injected Python runners are filesystem/orchestration fixtures only. The public
CLI has no fixture or skip-signing mode. This is not notarization or ABI proof.
"""
from __future__ import annotations

import argparse
from contextlib import ExitStack
import copy
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys
import uuid

import macos_runtime_selection as selection
import macho_dependencies as macho
from package_runtime import run_runtime_command
from verify_native_package import _configuration

REQUEST_SCHEMA = 'caesura.macos-runtime-install-request.v1'
RECEIPT_SCHEMA = 'caesura.macos-runtime-install-receipt.v1'
MAX_JSON = 4 * 1024 * 1024
SHA256 = re.compile(r'[0-9a-f]{64}\Z')
REQUEST_KEYS = {'schema', 'configuration', 'stage_root', 'selection', 'requirements',
                'build_engine', 'tools', 'previous_install'}
APPLE_TOOLS = {name: Path('/usr/bin') / name for name in ('install_name_tool', 'codesign')}


def _need(condition, message):
    if not condition:
        raise ValueError(message)


def _plain(value, label):
    _need(isinstance(value, str) and selection.PLAIN_NAME.fullmatch(value),
          f'{label} must be a plain nonempty name')
    return value


def _absolute(value):
    _need(isinstance(value, (str, os.PathLike)), 'Expected an absolute pathname')
    path = Path(value)
    _need(path.is_absolute() and '..' not in path.parts
          and not any(ord(c) < 32 or ord(c) == 127 for c in str(path)),
          f'Unsafe absolute pathname: {path}')
    return path


def _directory_state(path):
    path = _absolute(path)
    _need(path.resolve(strict=True) == path, f'Directory is not physical/canonical: {path}')
    state = []
    for item in (*reversed(path.parents), path):
        info = item.lstat()
        _need(stat.S_ISDIR(info.st_mode) and not getattr(info, 'st_reparse_tag', 0),
              f'Directory route is not ordinary: {item}')
        state.append((str(item), info.st_dev, info.st_ino, info.st_mode,
                      getattr(info, 'st_file_attributes', 0)))
    return state


def _ordinary(path, *, single_link=False):
    path = _absolute(path)
    _directory_state(path.parent)
    info = path.lstat()
    _need(stat.S_ISREG(info.st_mode) and not getattr(info, 'st_reparse_tag', 0),
          f'Expected an ordinary file: {path}')
    _need(not single_link or info.st_nlink == 1, f'Stage file has another hardlink: {path}')
    return info


def _unique_object(pairs):
    value = {}
    for key, item in pairs:
        _need(key not in value, f'Duplicate JSON key: {key}')
        value[key] = item
    return value


def _decode(raw):
    _need(len(raw) <= MAX_JSON, 'JSON input exceeds the bounded input size')
    return json.loads(raw.decode('utf-8'), object_pairs_hook=_unique_object,
                      parse_constant=lambda value: (_ for _ in ()).throw(ValueError('Nonfinite JSON value')))


class _Input:
    """Hold and recheck an ordinary input, including its permitted SDK aliases."""
    def __init__(self, path, *, digest=None, aliases=False):
        self.path = _absolute(path)
        if not aliases:
            _ordinary(self.path)
        _, self.resolved, path_info, self.route = selection._path_state(self.path)
        self.stream, self.info = selection._open_regular(self.resolved, path_info)
        try:
            self.sha256 = self.hash()
            _need(digest is None or self.sha256 == digest, f'Input digest mismatch: {self.path}')
            self.check()
        except BaseException:
            self.stream.close()
            raise

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        self.stream.close()

    def hash(self):
        self.stream.seek(0)
        return hashlib.file_digest(self.stream, 'sha256').hexdigest()

    def check(self):
        _need(selection._identity(os.fstat(self.stream.fileno())) == selection._identity(self.info),
              f'Held input changed: {self.path}')
        _need(selection._path_state(self.path)[3] == self.route,
              f'Input route/identity changed: {self.path}')
        _need(self.hash() == self.sha256, f'Held input bytes changed: {self.path}')
        _need(selection._identity(os.fstat(self.stream.fileno())) == selection._identity(self.info)
              and selection._path_state(self.path)[3] == self.route,
              f'Input changed during final read: {self.path}')

    def json(self):
        _need(self.info.st_size <= MAX_JSON, f'JSON input is too large: {self.path}')
        self.stream.seek(0)
        value = _decode(self.stream.read(MAX_JSON + 1))
        self.check()
        return value

    def reference(self):
        return {'path': str(self.path), 'sha256': self.sha256}


def _reference(value, label):
    _need(isinstance(value, dict) and set(value) == {'path', 'sha256'}, f'Invalid {label} reference')
    _absolute(value['path'])
    _need(isinstance(value['sha256'], str) and SHA256.fullmatch(value['sha256']),
          f'Invalid {label} SHA256')
    return value


def _protected_roots(request, selected):
    """Only establish diagnostic containment here; full validation follows."""
    _need(isinstance(request, dict) and isinstance(selected, dict), 'Malformed install inputs')
    protected = [_absolute(request['stage_root']), _absolute(request['build_engine']['path']).parent]
    for item in selected['components']:
        for key in ('selected_path', 'resolved_path'):
            given = _absolute(item[key])
            protected.extend((given.parent, given.resolve(strict=True).parent))
    return tuple(path.resolve(strict=True) for path in protected)


def _safe_report_parent(parent, protected):
    parent = _absolute(parent)
    _directory_state(parent)
    for root in protected:
        _need(not parent.is_relative_to(root), f'Diagnostic directory overlaps protected input: {parent}')
    return parent


def _new_report(path, protected):
    path = _absolute(path)
    _safe_report_parent(path.parent, protected)
    _need(not path.exists() and not path.is_symlink(), f'Diagnostic directory must be new: {path}')
    path.mkdir(mode=0o700)
    return _directory_state(path)


def _json_write(path, value):
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, 'O_BINARY', 0) | getattr(os, 'O_NOFOLLOW', 0)
    fd = os.open(path, flags, 0o600)
    with os.fdopen(fd, 'wb') as stream:
        stream.write((json.dumps(value, ensure_ascii=True, indent=2) + '\n').encode('utf-8'))


def _log_digest(path):
    # Empty stdout/stderr is normal. SDK selection's positive size bound does
    # not apply to logs. Still bind the opened regular descriptor and pathname.
    before = _ordinary(path)
    stream, fd_before = selection._open_regular(path, before)
    with stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        after, path_after = os.fstat(stream.fileno()), path.stat()
        _need(selection._identity(fd_before) == selection._identity(after)
              and selection._identity(before) == selection._identity(path_after)
              and selection._identity(after)[:4] == selection._identity(path_after)[:4],
              f'Log changed during readback: {path}')
    return digest


def _stage_file(path):
    info = _ordinary(path, single_link=True)
    with _Input(path) as locked:
        return {'identity': list(selection._identity(info)), 'sha256': locked.sha256, 'size': info.st_size}


def _metadata(value, request, selected):
    _need(isinstance(value, dict) and value.get('schema') == 'caesura.package-build.v1'
          and value.get('platform') == 'macos' and value.get('configuration') == request['configuration'],
          'Package requirements do not describe this macOS configuration')
    engine = _plain(value.get('engine_relative_path'), 'Engine filename')
    required, libraries = _configuration('macos', value.get('required_configuration'))
    _need(required.get('openssl_linkage') == selected['linkage']
          and required.get('openssl_libraries') == selected['libraries'],
          'Package requirements differ from selected OpenSSL linkage/names')
    _need(engine.casefold() not in {name.casefold() for name in libraries}, 'Engine/library name collision')
    declarations = [name for key, value in required.items() if key.endswith('_libraries')
                    and isinstance(value, list) for name in value]
    _need(len(declarations) == len({name.casefold() for name in declarations}), 'Runtime library name collision')
    return engine, libraries


def _previous(value, request, expected_names, stage, stage_identity, fixture):
    _need(isinstance(value, dict) and value.get('schema') == RECEIPT_SCHEMA and value.get('success') is True,
          'Previous install is not a successful coordinator receipt')
    expected_kind = 'FIXTURE_ONLY' if fixture else 'APPLE_TOOLS'
    expected_status = 'FIXTURE_ONLY' if fixture else 'MACOS_RUNTIME_INSTALLED'
    _need(value.get('evidence_kind') == expected_kind and value.get('status') == expected_status,
          'Previous receipt has another evidence scope')
    for key in ('configuration', 'stage_root', 'selection', 'requirements', 'build_engine', 'tools'):
        _need(value.get(key) == request[key], f'Previous receipt has another {key}')
    _need(value.get('stage_identity') == stage_identity,
          'Previous receipt belongs to another physical install directory')
    records = value.get('final_files')
    _need(isinstance(records, list) and len(records) == len(expected_names), 'Previous receipt file set is incomplete')
    files = {}
    for item in records:
        _need(isinstance(item, dict) and set(item) == {'relative_path', 'sha256', 'size'},
              'Invalid previous final file record')
        name = item['relative_path']
        _need(type(item['size']) is int and item['size'] > 0, 'Invalid previous file size')
        _need(name in expected_names and name not in files, 'Previous receipt has an unexpected/duplicate file')
        current = _stage_file(stage / name)
        _need(current['sha256'] == item['sha256'] and current['size'] == item['size'],
              f'Previous final file differs: {name}')
        files[name] = item
    _need(set(files) == set(expected_names), 'Previous receipt file set differs')
    return files


def _match_source(name, components):
    matches = []
    for component in components:
        image = component['format_observation']
        if name in (image['identifiers'][0], component['selected_path'], component['resolved_path']):
            matches.append(component)
    _need(len(matches) <= 1, f'Ambiguous selected dependency identity: {name}')
    return matches[0] if matches else None


def _plan(stage, engine, libraries, components, previous):
    images = {engine: macho.inspect_macho(stage / engine)}
    _need(images[engine]['file_type'] == 2, 'Installed Engine is not an executable')
    selected_names = {item['runtime_name'] for item in components}
    for name in libraries:
        if name not in selected_names:
            images[name] = macho.inspect_macho(stage / name)
    edits = []
    for item in components:
        name = item['runtime_name']
        images[name] = copy.deepcopy(item['format_observation'])
    cpu = images[engine]['cpu_type']
    for name, image in images.items():
        _need(image['cpu_type'] == cpu, f'Runtime image CPU differs: {name}')
        _need(name == engine or image['file_type'] in (6, 8), f'Runtime library is another file type: {name}')
    engine_seen, ssl_seen = set(), set()
    for name, image in images.items():
        is_ssl = bool(components) and name == components[0]['runtime_name']
        for edge in image['dependencies']:
            old = edge['name']
            component = _match_source(old, components)
            if component is not None:
                _need(name == engine or is_ssl and component['component'] == 'Crypto',
                      f'Unexpected source OpenSSL dependency from {name}: {old}')
                new = '@loader_path/' + component['runtime_name']
                if name == engine:
                    engine_seen.add(component['component'])
                if is_ssl:
                    ssl_seen.add(component['component'])
                if old != new:
                    edits.append({'image': name, 'arguments': ['-change', old, new]})
                edge['name'] = new
            elif name == engine and previous and old.startswith('@loader_path/'):
                for item in components:
                    if old == '@loader_path/' + item['runtime_name']:
                        engine_seen.add(item['component'])
        if name in selected_names:
            identifier = '@rpath/' + name
            edits.append({'image': name, 'arguments': ['-id', identifier]})
            image['identifiers'] = [identifier]
    if components:
        _need(engine_seen == {'SSL', 'Crypto'}, 'Engine lacks the exact selected SSL/Crypto identity edges')
        _need(ssl_seen == {'Crypto'}, 'Selected SSL lacks its exact selected Crypto dependency')
    # Preflight the planned graph without touching stage bytes. Use the existing
    # parser's local-path expansion; only selected originals are substituted.
    root_engine = stage / engine
    inherited = macho._run_paths(images[engine], root_engine, root_engine, stage)
    paths = {stage / name: name for name in images}
    for name, image in images.items():
        owner = stage / name
        search = tuple(dict.fromkeys([*macho._run_paths(image, owner, root_engine, stage), *inherited]))
        for identifier in image['identifiers']:
            _need(identifier.startswith(('@rpath/', '@loader_path/', '@executable_path/'))
                  and '..' not in PurePosixPath(identifier).parts,
                  f'Nonlocal runtime identifier in {name}: {identifier}')
        for edge in image['dependencies']:
            dependency = edge['name']
            if macho.is_system_library(dependency):
                continue
            if dependency.startswith('@rpath/'):
                suffix = dependency[len('@rpath/'):]
                _need(suffix and '..' not in PurePosixPath(suffix).parts, 'Invalid planned rpath dependency')
                candidates = [Path(prefix) / suffix for prefix in search]
            else:
                candidates = [macho._expand_local(dependency, owner, root_engine, stage)]
            chosen = None
            for candidate in candidates:
                resolved = macho._contained(candidate, stage, must_exist=False)
                if resolved in paths:
                    chosen = resolved
                    break
                _need(not candidate.exists() and not candidate.is_symlink(),
                      f'Earlier runtime search candidate is undeclared: {candidate}')
            _need(chosen is not None,
                  f'Unresolved or undeclared runtime dependency in {name}: {dependency}')
    unique = []
    for edit in edits:
        if edit not in unique:
            unique.append(edit)
    return images, unique


def _check_images(stage, expected):
    fields = ('cpu_type', 'cpu_subtype', 'file_type', 'dependencies', 'identifiers', 'rpaths')
    for name, wanted in expected.items():
        observed = macho.inspect_macho(stage / name)
        for key in fields:
            _need(observed[key] == wanted[key], f'Transformed image differs in {key}: {name}')


def _copy_source(source, destination, check, temporary, *, replace_existing):
    check()
    temp = destination.with_name('.caesura-runtime-' + uuid.uuid4().hex + '.tmp')
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, 'O_BINARY', 0) | getattr(os, 'O_NOFOLLOW', 0)
    fd = os.open(temp, flags, 0o600)
    info = os.fstat(fd)
    temporary[str(temp)] = (info.st_dev, info.st_ino)
    with os.fdopen(fd, 'wb') as output:
        source.stream.seek(0)
        digest = hashlib.sha256()
        length = 0
        while True:
            chunk = source.stream.read(1024 * 1024)
            if not chunk:
                break
            length += len(chunk)
            _need(length <= source.info.st_size, 'Selected input grew while copying')
            digest.update(chunk)
            output.write(chunk)
        _need(length == source.info.st_size and digest.hexdigest() == source.sha256,
              'Selected bytes differ during copy')
        output.flush()
        if hasattr(os, 'fchmod'):
            os.fchmod(output.fileno(), stat.S_IMODE(source.info.st_mode))
        os.fsync(output.fileno())
    source.check()
    check()
    _need(_stage_file(temp)['sha256'] == source.sha256, 'Copied bytes differ from held input')
    if replace_existing:
        # Only an explicitly preflighted original/receipt-bound destination
        # reaches replacement. The surrounding stage checks detect interference.
        os.replace(temp, destination)
    else:
        # Exclusive publication: a competing new destination must not be
        # silently overwritten between the final check and publication.
        os.link(temp, destination)
        temp.unlink()
    temporary.pop(str(temp))
    _need(_stage_file(destination)['sha256'] == source.sha256, 'Published source copy differs')


def _tool_environment(report):
    env = {key: value for key, value in os.environ.items()
           if key in ('SystemRoot', 'WINDIR', 'COMSPEC', 'PATHEXT', 'TEMP', 'TMP', 'TMPDIR',
                      'HOME', 'USERPROFILE', 'LANG', 'LC_ALL')}
    env.update(PATH='/usr/bin:/bin:/usr/sbin:/sbin' if os.name != 'nt' else os.environ.get('PATH', ''),
               PYTHONDONTWRITEBYTECODE='1', PYTHONIOENCODING='utf-8')
    return env


def _run_tool(argv, stage, report_dir, result, runner):
    index = len(result['commands'])
    record = {'argv': argv, 'stdout': str(report_dir / f'command-{index:03d}.stdout'),
              'stderr': str(report_dir / f'command-{index:03d}.stderr')}
    result['commands'].append(record)
    control = report_dir / f'command-{index:03d}.owned'
    first_error = None
    try:
        with open(record['stdout'], 'xb') as stdout, open(record['stderr'], 'xb') as stderr:
            record['runtime'] = runner(argv, stage, _tool_environment(report_dir), control, stdout, stderr, 60)
        run = record['runtime']
        _need(run.get('actual_exit_code') == 0 and run.get('launcher_exit_code') == 0
              and run.get('stop_requested') is False and run.get('owned_tree_cleanup') == 'COMPLETE'
              and not run.get('timed_out') and not run.get('forced_kill') and run.get('status') == 'EXITED',
              f'Apple tool did not complete successfully: exit={run.get("actual_exit_code")}, '
              f'status={run.get("status")}, launcher={run.get("launcher_exit_code")}, '
              f'stop_requested={run.get("stop_requested")}')
    except Exception as error:
        first_error = error
        durable = control / 'run.json'
        if 'runtime' not in record and durable.is_file():
            try:
                record['runtime'] = _decode(durable.read_bytes())
            except (OSError, ValueError):
                pass
    for key in ('stdout', 'stderr'):
        path = Path(record[key])
        try:
            if path.is_file():
                record[key + '_sha256'] = _log_digest(path)
        except (OSError, ValueError) as error:
            record.setdefault('log_readback_errors', []).append(str(error))
            if first_error is None:
                first_error = error
    if first_error is not None:
        raise first_error


def install_macos_runtime(request_path, *, request_sha256, report_dir, runner=None):
    """Return a retained receipt; unsafe diagnostic paths raise without writes."""
    fixture = runner is not None
    report_dir = _absolute(report_dir)
    # Discover protected roots without trusting declarations as acceptance.
    # Malformed inputs which cannot establish containment do not get a report.
    with _Input(request_path) as preliminary:
        request = preliminary.json()
    with _Input(request['selection']['path']) as preliminary:
        raw_selected = preliminary.json()
    protected = _protected_roots(request, raw_selected)
    report_state = _new_report(report_dir, protected)
    result = {'schema': RECEIPT_SCHEMA, 'success': False, 'status': 'TAINTED_NOT_ACCEPTED',
              'evidence_kind': 'FIXTURE_ONLY' if fixture else 'APPLE_TOOLS',
              'request_sha256': request_sha256, 'commands': [], 'edits': [], 'final_files': []}
    # Retain the declared bindings even if validation fails before any mutation.
    # Their presence is not acceptance: only success plus final checks provides it.
    for key in ('configuration', 'stage_root', 'selection', 'requirements', 'build_engine', 'tools'):
        if key in request:
            result[key] = copy.deepcopy(request[key])
    temporary = {}
    try:
        _need(fixture or sys.platform == 'darwin', 'Mac runtime installation requires Darwin')
        _need(isinstance(request_sha256, str) and SHA256.fullmatch(request_sha256), 'Invalid request digest')
        _need(set(request) == REQUEST_KEYS and request.get('schema') == REQUEST_SCHEMA, 'Invalid install request schema/keys')
        _plain(request['configuration'], 'Configuration')
        stage = _absolute(request['stage_root'])
        stage_state = _directory_state(stage)
        stage_identity = {'device': stage_state[-1][1], 'inode': stage_state[-1][2]}
        result['stage_identity'] = stage_identity
        _need(not report_dir.is_relative_to(stage), 'Report is within the package')
        _need(not _absolute(request_path).is_relative_to(stage), 'Install request must be outside the payload')
        _need(isinstance(request['tools'], dict) and set(request['tools']) == set(APPLE_TOOLS), 'Invalid Apple tool set')
        with ExitStack() as held:
            inputs = []
            def lock(path, digest=None, *, aliases=False):
                value = held.enter_context(_Input(path, digest=digest, aliases=aliases))
                inputs.append(value)
                return value
            request_input = lock(request_path, request_sha256)
            _need(request_input.json() == request, 'Install request changed after diagnostic preflight')
            references = {}
            for key in ('selection', 'requirements', 'build_engine'):
                ref = _reference(request[key], key)
                _need(not _absolute(ref['path']).is_relative_to(stage), f'{key} reference is inside the payload')
                references[key] = lock(ref['path'], ref['sha256'])
            selected = references['selection'].json()
            _need(selected.get('schema') == 'caesura.macos-openssl-selection.v1'
                  and selected.get('configuration') == request['configuration'], 'Selection configuration/schema differs')
            declared = selected.get('components')
            _need(isinstance(declared, list) and len(declared) == 2
                  and [item.get('component') for item in declared] == ['SSL', 'Crypto'], 'Invalid selected component order')
            fresh = selection.select_openssl(declared[0]['selected_path'], declared[1]['selected_path'], request['configuration'])
            _need(json.dumps(fresh, sort_keys=True, separators=(',', ':'))
                  == json.dumps(selected, sort_keys=True, separators=(',', ':')),
                  'Selected declaration differs from actual readonly inputs')
            sources = [lock(item['selected_path'], item['sha256'], aliases=True) for item in declared]
            for source, item in zip(sources, declared):
                _need(str(source.resolved) == item['resolved_path'], 'Selected source endpoint differs')
                _need(json.dumps(source.route, sort_keys=True)
                      == json.dumps(item['path_observation'], sort_keys=True),
                      'Held source route differs from the selected identity')
                _need(not source.resolved.is_relative_to(stage), 'SDK source is inside install stage')
            tools = {}
            for kind in APPLE_TOOLS:
                ref = _reference(request['tools'][kind], kind)
                if not fixture:
                    _need(_absolute(ref['path']) == APPLE_TOOLS[kind].resolve(strict=True), 'Production requires the selected system Apple tools')
                    _need(os.access(ref['path'], os.X_OK), f'Apple tool is not executable: {kind}')
                tools[kind] = lock(ref['path'], ref['sha256'])
            engine, libraries = _metadata(references['requirements'].json(), request, selected)
            names = [engine, *libraries]
            components = declared if selected['linkage'] == 'shared' else []
            copied = {item['runtime_name']: item for item in components}
            stage_files = {}
            for name in names:
                path = stage / name
                if path.exists() or path.is_symlink():
                    stage_files[name] = _stage_file(path)
                else:
                    _need(name in copied, f'Missing mandatory installed image: {name}')
                    stage_files[name] = None
            previous = None
            if request['previous_install'] is not None:
                ref = _reference(request['previous_install'], 'previous install')
                _need(not _absolute(ref['path']).is_relative_to(stage), 'Previous receipt must be external')
                previous = _previous(lock(ref['path'], ref['sha256']).json(), request, names,
                                     stage, stage_identity, fixture)
            for name, component in copied.items():
                current = stage_files[name]
                _need(current is None or current['sha256'] == component['sha256'] or previous is not None,
                      f'Existing runtime has neither original bytes nor explicit previous receipt: {name}')
            # Untouched libraries are descriptor-locked for the whole transaction.
            for name in libraries:
                if name not in copied:
                    lock(stage / name, stage_files[name]['sha256'])
            expected, edits = _plan(stage, engine, libraries, components, previous)
            result['edits'] = edits
            def check():
                _need(_directory_state(stage) == stage_state, 'Physical stage binding changed')
                _need(_directory_state(report_dir) == report_state, 'Diagnostic directory binding changed')
                for value in inputs:
                    value.check()
                for name, baseline in stage_files.items():
                    path = stage / name
                    if baseline is None:
                        _need(not path.exists() and not path.is_symlink(), f'Unexpected runtime destination appeared: {name}')
                    else:
                        _need(_stage_file(path) == baseline, f'Stage image changed outside its owned operation: {name}')
            check()
            for component, source in zip(components, sources):
                name = component['runtime_name']
                _copy_source(source, stage / name, check, temporary,
                             replace_existing=stage_files[name] is not None)
                stage_files[name] = _stage_file(stage / name)
            tool_runner = runner or run_runtime_command
            def run(kind, arguments, name):
                check()
                argv = [str(tools[kind].path), *arguments, str(stage / name)]
                _run_tool(argv, stage, report_dir, result, tool_runner)
                # Only this exact regular image can change during the command.
                stage_files[name] = _stage_file(stage / name)
                check()
            for edit in edits:
                run('install_name_tool', edit['arguments'], edit['image'])
            _check_images(stage, expected)
            signed = [*copied, engine]
            for name in signed:
                run('codesign', ['--force', '--sign', '-'], name)
            for name in signed:
                # Verification must not change even its target.
                before = stage_files[name]
                run('codesign', ['--verify', '--strict'], name)
                _need(stage_files[name] == before, f'Signature verification mutated image: {name}')
            _check_images(stage, expected)
            result['closure'] = macho.inspect_package_closure(stage, [stage / engine], [stage / name for name in libraries])
            _need({item['relative_path'] for item in result['closure']['images']} == set(names),
                  'Final runtime closure differs from declared images')
            check()
            result['final_files'] = [{'relative_path': name, 'sha256': stage_files[name]['sha256'],
                                      'size': stage_files[name]['size']} for name in names]
            result.update(success=True, status='FIXTURE_ONLY' if fixture else 'MACOS_RUNTIME_INSTALLED')
    except Exception as error:
        result.update(success=False, status='TAINTED_NOT_ACCEPTED')
        result['error'] = f'{type(error).__name__}: {error}'
    finally:
        for value, identity in temporary.items():
            path = Path(value)
            try:
                _need(_directory_state(path.parent) == stage_state, 'Cannot clean a temporary in a replaced stage')
                info = path.lstat()
                if (info.st_dev, info.st_ino) == identity and stat.S_ISREG(info.st_mode):
                    path.unlink()
            except FileNotFoundError:
                pass
            except (OSError, ValueError) as error:
                result.update(success=False, status='TAINTED_NOT_ACCEPTED')
                result.setdefault('error', f'Temporary cleanup failed: {error}')
                result.setdefault('cleanup_errors', []).append(str(error))
        try:
            _need(_directory_state(report_dir) == report_state, 'Cannot publish receipt into changed diagnostic directory')
            _json_write(report_dir / 'receipt.json', result)
        except (OSError, ValueError) as publication_error:
            if 'error' in result:
                raise RuntimeError(result['error'] + '; receipt publication failed: ' + str(publication_error)) from publication_error
            raise
    return result


def _physical_install_path(value, *, existing=True):
    # CMake on macOS commonly spells /tmp and /var through OS-owned aliases.
    # Only those known root aliases may be canonicalized by the adapter. Do not
    # turn a mutable stage child symlink into an apparently ordinary new root.
    path = _absolute(value)
    allowed = {Path('/tmp'): Path('/private/tmp'), Path('/var'): Path('/private/var')}
    for item in (*reversed(path.parents), path):
        try:
            info = item.lstat()
        except FileNotFoundError:
            _need(not existing and item == path, f'Missing install path parent: {item}')
            break
        if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_reparse_tag', 0):
            _need(item in allowed and getattr(info, 'st_uid', -1) == 0
                  and item.resolve(strict=True) == allowed[item], f'Untrusted install path alias: {item}')
    return path.resolve(strict=existing)


def _cmake_request(args):
    """Adapt explicit install argv; no second copy/finalization implementation."""
    _need(sys.platform == 'darwin', 'Mac CMake install adapter requires Darwin')
    required = ('selection_file', 'selection_sha256', 'requirements_file', 'configuration',
                'stage_root', 'build_engine', 'report_parent')
    _need(all(getattr(args, name) for name in required), 'Incomplete CMake install adapter arguments')
    _plain(args.configuration, 'Configuration')
    with ExitStack() as held:
        selected = held.enter_context(_Input(_physical_install_path(args.selection_file), digest=args.selection_sha256))
        requirements = held.enter_context(_Input(_physical_install_path(args.requirements_file)))
        engine = held.enter_context(_Input(_physical_install_path(args.build_engine)))
        tools = {name: held.enter_context(_Input(path.resolve(strict=True))) for name, path in APPLE_TOOLS.items()}
        request = {'schema': REQUEST_SCHEMA, 'configuration': args.configuration,
                   'stage_root': str(_physical_install_path(args.stage_root)),
                   'selection': selected.reference(), 'requirements': requirements.reference(),
                   'build_engine': engine.reference(), 'tools': {name: value.reference() for name, value in tools.items()},
                   'previous_install': None}
        protected = _protected_roots(request, selected.json())
        parent = _physical_install_path(args.report_parent, existing=False)
        # The parent can be new, but only below an existing safe physical parent.
        if not parent.exists() and not parent.is_symlink():
            _safe_report_parent(parent.parent, protected)
            parent.mkdir(mode=0o700)
        _safe_report_parent(parent, protected)
        attempt = parent / ('attempt-' + uuid.uuid4().hex)
        _new_report(attempt, protected)
        request_path = attempt / 'request.json'
        _json_write(request_path, request)
        for value in (selected, requirements, engine, *tools.values()):
            value.check()
        with _Input(request_path) as locked:
            request_hash = locked.sha256
    return install_macos_runtime(request_path, request_sha256=request_hash, report_dir=attempt / 'result')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--request')
    parser.add_argument('--request-sha256')
    parser.add_argument('--report-dir')
    parser.add_argument('--cmake-install', action='store_true')
    for option in ('selection-file', 'selection-sha256', 'requirements-file', 'configuration',
                   'stage-root', 'build-engine', 'report-parent'):
        parser.add_argument('--' + option)
    args = parser.parse_args(argv)
    try:
        if args.cmake_install:
            _need(not any((args.request, args.request_sha256, args.report_dir)), 'Do not mix CMake and request-file modes')
            result = _cmake_request(args)
        else:
            _need(all((args.request, args.request_sha256, args.report_dir)), 'Request, digest and report directory are required')
            _need(not any((args.selection_file, args.selection_sha256, args.requirements_file, args.configuration,
                           args.stage_root, args.build_engine, args.report_parent)), 'Do not mix request and adapter arguments')
            result = install_macos_runtime(args.request, request_sha256=args.request_sha256, report_dir=args.report_dir)
        print(json.dumps(result, ensure_ascii=True))
        return 0 if result['success'] else 1
    except (OSError, ValueError, RuntimeError, KeyError, TypeError) as error:
        print(f'Mac runtime installation failed: {type(error).__name__}: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
