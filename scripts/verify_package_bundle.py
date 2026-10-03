#!/usr/bin/env python3
"""Verify one downloaded v2 bundle against independently supplied expectations.

This is a local byte/context gate. The caller authenticates producer context and
manifest SHA, selects the required file set, and locks any saved result before
using it later. No producer-local receipt path is opened, no original runtime
logs are replayed, and this module never grants publication permission.
"""
from __future__ import annotations

import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import re
import stat

from package_verification import (_component, _reparse, _sha256_file, _signature,
                                  inspect_inventory, PackageVerificationError)
from verify_native_package import _configuration

SCHEMA = 'caesura.package-bundle-verification.v1'
MAX_JSON_BYTES = 16 * 1024 * 1024
MAX_ENTRIES = 100_000


class PackageBundleError(ValueError):
    pass


def _need(condition, message):
    if not condition:
        raise PackageBundleError(message)


def _object(value, where, keys=None):
    _need(type(value) is dict, f'{where} must be an object')
    if keys is not None:
        _need(set(value) == set(keys), f'{where} has missing or extra fields')
    return value


def _text(value, where, limit=1024):
    _need(type(value) is str and 0 < len(value) <= limit
          and not any(ord(c) < 32 or ord(c) == 127 for c in value), f'Invalid {where}')
    try:
        value.encode('utf-8')
    except UnicodeError as error:
        raise PackageBundleError(f'Invalid UTF-8 text in {where}') from error
    return value


def _sha(value, where='SHA256', length=64):
    _need(type(value) is str and re.fullmatch('[0-9a-f]{'+str(length)+'}', value), f'Invalid {where}')
    return value


def _relative(value):
    _text(value, 'relative bundle path', 1024)
    _need(not value.startswith('/') and '\\' not in value, 'Bundle path must be exact relative POSIX spelling')
    for part in value.split('/'):
        _component(part)
    return value


def _plain(path):
    info = path.lstat()
    _need(not stat.S_ISLNK(info.st_mode) and not _reparse(path), f'Linked bundle path: {path}')
    _need(stat.S_ISDIR(info.st_mode) or stat.S_ISREG(info.st_mode), f'Unsupported bundle entry: {path}')
    _need(not stat.S_ISREG(info.st_mode) or info.st_nlink == 1, f'Hardlinked bundle file: {path}')
    return info


def _path(root, name, kind):
    current = root
    _need(stat.S_ISDIR(_plain(root).st_mode), 'Bundle root is no longer a plain directory')
    parts = _relative(name).split('/')
    for index, part in enumerate(parts):
        # Check exact case/spelling even on a case-insensitive host.
        _need(part in {p.name for p in current.iterdir()}, f'Bundle path spelling/missing entry: {name}')
        current = current / part
        info = _plain(current)
        directory = index < len(parts)-1 or kind == 'directory'
        _need(stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode), f'Wrong bundle entry kind: {name}')
    _need(current.resolve(strict=True).is_relative_to(root), 'Bundle entry escapes root')
    return current


def _tree(root):
    entries, stack, spellings = [], [root], set()
    while stack:
        directory = stack.pop()
        for path in sorted(directory.iterdir()):
            name = _relative(path.relative_to(root).as_posix())
            _need(name.casefold() not in spellings, f'Case-colliding bundle paths: {name}')
            spellings.add(name.casefold())
            info = _plain(path)
            directory_entry = stat.S_ISDIR(info.st_mode)
            entries.append((name, 'directory' if directory_entry else 'file'))
            _need(len(entries) <= MAX_ENTRIES, 'Bundle entry count exceeds limit')
            if directory_entry:
                stack.append(path)
    return entries


def _pairs(pairs):
    result = {}
    for key, value in pairs:
        _need(key not in result, f'Duplicate JSON key: {key}')
        result[key] = value
    return result


def _finite(value):
    number = float(value)
    _need(math.isfinite(number), 'Nonfinite JSON number')
    return number


def _constant(value):
    raise PackageBundleError('Nonfinite JSON constant: ' + value)


def _json(root, name, expected):
    """Parse exactly the bounded bytes whose externally locked digest is checked."""
    path = _path(root, name, 'file')
    before = path.lstat()
    _need(before.st_size <= MAX_JSON_BYTES, f'JSON file exceeds limit: {name}')
    with path.open('rb') as stream:
        _need(_signature(before) == _signature(os.fstat(stream.fileno())), f'JSON changed while opening: {name}')
        raw = stream.read(MAX_JSON_BYTES + 1)
        _need(len(raw) <= MAX_JSON_BYTES, f'JSON file exceeds limit: {name}')
        after = os.fstat(stream.fileno())
    _path(root, name, 'file')
    _need(_signature(before) == _signature(after) == _signature(path.lstat()), f'JSON changed while reading: {name}')
    actual = hashlib.sha256(raw).hexdigest()
    _need(actual == _sha(expected), f'JSON digest mismatch: {name}')
    try:
        value = json.loads(raw.decode('utf-8'), object_pairs_hook=_pairs,
                           parse_float=_finite, parse_constant=_constant)
    except (UnicodeError, json.JSONDecodeError, RecursionError) as error:
        raise PackageBundleError(f'Invalid JSON in {name}: {error}') from error
    _object(value, name)
    return value, dict(path=str(path), name=name, kind='file', sha256=actual)


def _same(left, right):
    # JSON equality preserves distinctions such as false versus 0.
    return json.dumps(left, sort_keys=True, ensure_ascii=False, allow_nan=False) == json.dumps(
        right, sort_keys=True, ensure_ascii=False, allow_nan=False)


def _producer(value):
    _object(value, 'expected producer')
    keys = {'provider'}
    if value.get('provider') == 'github-actions':
        keys |= {'repository','repository_id','run_id','run_attempt','workflow_ref','workflow_sha','job_key'}
        for key in ('repository','workflow_ref','job_key'):
            _text(value.get(key), 'producer '+key)
        _sha(value.get('workflow_sha'), 'workflow SHA', 40)
        for key in ('repository_id','run_id','run_attempt'):
            _need(type(value.get(key)) is int and 0 < value[key] < 10**20, f'Invalid producer {key}')
    else:
        _need(value.get('provider') == 'local', 'Unsupported producer provider')
    _object(value, 'expected producer', keys)


def _origin(value, name):
    _text(value, 'producer input path', 8192)
    _need(PurePosixPath(value).is_absolute() or PureWindowsPath(value).is_absolute(), 'Producer input path must be absolute')
    _need(value.replace('\\','/').rsplit('/',1)[-1] == name.rsplit('/',1)[-1], 'Receipt input filename differs from selected final file')


def _source(value, source_sha):
    _object(value, 'source identity', {'source_sha','dirty','worktree_fingerprint'})
    _need(value['source_sha'] == source_sha and value['dirty'] is False, 'Receipt source must match the expected clean commit')
    _sha(value['worktree_fingerprint'], 'source fingerprint')


def _receipt(value, final, platform, source_sha, configuration, requirements, requirements_sha):
    _need(value.get('schema') == 'caesura.package-validation.v1' and value.get('status') == 'PASS'
          and value.get('accepted') is True and value.get('diagnostic') is False,
          'An original accepted non-diagnostic PASS receipt is required')
    _need(value.get('platform') == platform and value.get('configuration') == configuration
          and value.get('expected_source_sha') == source_sha, 'Receipt context mismatch')
    _need(type(value.get('errors')) is list and not value['errors'], 'Receipt records errors')
    _source(value.get('source_before'), source_sha); _source(value.get('source_after'), source_sha)
    _need(_same(value['source_before'], value['source_after']), 'Receipt source changed during validation')
    for field, status in (('static','STATIC_PASS'),('runtime','RUNTIME_PASS'),('stability','STABLE')):
        _need(_object(value.get(field), field).get('status') == status, f'Receipt {field} did not pass')
    _need(value['stability'].get('input_stable') is True and value['stability'].get('package_stable') is True,
          'Receipt input/package stability is incomplete')
    prep = _object(value.get('preparation'), 'preparation')
    _need(prep.get('status') == 'PREPARED' and prep.get('input_stable') is True, 'Receipt preparation is incomplete')
    container_format = 'appimage' if final['name'].lower().endswith('.appimage') else 'dmg' if final['name'].lower().endswith('.dmg') else None
    if container_format:
        _need(final['kind']=='file' and platform==('linux' if container_format=='appimage' else 'macos'), 'Container platform mismatch')
        container = _object(value.get('container'), 'container')
        _need(value.get('container_format') == container_format and container.get('format') == container_format
              and container.get('status') == 'CONTAINER_PREPARED', 'Wrong final container receipt')
        selected = _object(container.get('input'), 'container input')
        _need(container.get('expected_sha256') == selected.get('sha256_before') == selected.get('sha256_after') == final['sha256'],
              'Container receipt does not bind final bytes')
        _need(_same(container.get('preparation'), prep), 'Container and top-level preparation disagree')
    else:
        _need(value.get('container') is None and value.get('container_format') is None, 'Unexpected container receipt')
        selected = _object(prep.get('input'), 'prepared input')
        expected = _object(prep.get('expected'), 'prepared expected identity')
        if final['kind'] == 'file':
            _need(selected.get('kind') == 'archive' and selected.get('archive_sha256') == expected.get('archive_sha256') == final['sha256']
                  and expected.get('inventory_sha256') is None, 'Archive receipt does not bind final bytes')
        else:
            _need(selected.get('kind') == 'directory' and expected.get('inventory_sha256') == final['sha256']
                  and _object(selected.get('inventory'),'input inventory').get('sha256') == final['sha256']
                  and expected.get('archive_sha256') is None, 'Directory receipt does not bind final inventory')
    _origin(selected.get('path'), final['name'])
    if requirements is not None:
        bound = _object(value.get('requirements'), 'receipt requirements')
        _need(bound.get('sha256') == requirements_sha and _same(bound.get('value'), requirements), 'Receipt requirements binding mismatch')
    else:
        _need(value.get('requirements') is None, 'Web receipt must not declare native requirements')
    # These hashes are claims bound by the copied receipt, not replayed logs.
    for name, digest in _object(value.get('evidence'), 'receipt evidence').items():
        _relative(name); _sha(digest, 'stage evidence digest')


def _layout(root, locks):
    names = {}
    for lock in locks:
        name = _relative(lock['name'])
        folded = name.casefold()
        _need(folded not in names, 'Duplicate/case-colliding bundle dependency')
        _need(not any(folded.startswith(other+'/') or other.startswith(folded+'/') for other in names), 'Overlapping bundle dependencies')
        names[folded] = lock
    for name, kind in _tree(root):
        owned = name in {lock['name'] for lock in locks}
        inside = any(lock['kind']=='directory' and name.startswith(lock['name']+'/') for lock in locks)
        parent = kind=='directory' and any(lock['name'].startswith(name+'/') for lock in locks)
        _need(owned or inside or parent, f'Undeclared physical bundle entry: {name}')


def verify_bundle_stable(result):
    """Recheck a trusted, externally locked verification result before upload."""
    try:
        _need(result.get('schema') == SCHEMA and result.get('status') == 'BUNDLE_VERIFIED', 'Not a verified bundle result')
        root = Path(result['bundle_root'])
        _need(root.is_absolute() and root.resolve(strict=True) == root, 'Canonical bundle root changed')
        _plain(root)
        locks = result['locks']
        _need(type(locks) is list and 2 <= len(locks) <= 10, 'Invalid bundle locks')
        _layout(root, locks)
        for lock in locks:
            _object(lock, 'bundle lock', {'path','name','kind','sha256'})
            _need(lock['kind'] in ('file','directory'), 'Invalid lock kind')
            path = _path(root, lock['name'], lock['kind'])
            _need(str(path) == lock['path'], 'Canonical locked path changed')
            observed = inspect_inventory(path)['sha256'] if lock['kind']=='directory' else _sha256_file(path)
            _need(observed == _sha(lock['sha256']), f'Locked bundle input changed: {lock["name"]}')
        _layout(root, locks)
        return {'status':'BUNDLE_STABLE','release_ready':False,'locks':locks}
    except (OSError, KeyError, TypeError, PackageVerificationError) as error:
        raise PackageBundleError(f'Cannot recheck bundle: {error}') from error


def verify_bundle(bundle_dir, *, manifest_sha256, source_sha, platform, configuration,
                  version, required_files, expected_producer):
    """Match exact caller-selected files and authenticated producer expectations.

    required_files is {relative_name: 'file'|'directory'}, supplied by policy,
    never copied from the downloaded manifest. expected_producer excludes the
    manifest's authentication marker; this function performs no authentication.
    """
    try:
        _sha(manifest_sha256); _sha(source_sha,'expected source SHA',40)
        _need(platform in ('windows','linux','macos','web') and configuration == 'Release', 'Expected platform/Release configuration required')
        _need(type(version) is str and len(version)<=64 and re.fullmatch(r'\d+\.\d+\.\d+(?:\.\d+)?',version), 'Expected engine version required')
        _producer(expected_producer)
        _object(required_files, 'required files')
        _need(0 < len(required_files) <= 4, 'One to four independently selected final files required')
        for name, kind in required_files.items():
            _relative(name)
            _need(kind in ('file','directory') and (kind!='directory' or platform=='web'), 'Invalid required final file kind')
        _need(len({name.casefold() for name in required_files}) == len(required_files), 'Duplicate required filename spelling')
        given = Path(bundle_dir).absolute()
        _need(stat.S_ISDIR(_plain(given).st_mode), 'Bundle root must be a plain directory')
        root = given.resolve(strict=True)
        _tree(root)  # Refuse links/special entries before reading declared content.
        manifest, manifest_lock = _json(root, 'upload-manifest.json', manifest_sha256)
        _object(manifest,'upload manifest',{'schema','source_sha','platform','configuration','version','provenance','requirements','validations','files'})
        _need(manifest['schema']=='caesura.package-upload.v2' and manifest['source_sha']==source_sha
              and manifest['platform']==platform and manifest['configuration']==configuration and manifest['version']==version,
              'Upload manifest context/version mismatch')
        provenance = _object(manifest['provenance'],'manifest producer')
        _need(provenance.get('authentication')=='NOT_VERIFIED' and _same(
            provenance, dict(expected_producer,authentication='NOT_VERIFIED')), 'Producer context mismatch')
        files = manifest['files']; validations = manifest['validations']
        _need(type(files) is list and len(files)==len(required_files)
              and type(validations) is list and len(validations)==len(required_files), 'Missing/extra final file or validation declaration')
        locks, finals, seen = [manifest_lock], [], set()
        for item in files:
            _object(item,'final file',{'name','kind','sha256'})
            name = _relative(item['name']); _sha(item['sha256'])
            _need(name not in seen and required_files.get(name)==item['kind'], 'Unexpected/duplicate final file declaration')
            seen.add(name)
            path = _path(root,name,item['kind'])
            observed = inspect_inventory(path)['sha256'] if item['kind']=='directory' else _sha256_file(path)
            _need(observed==item['sha256'], 'Final package digest mismatch: '+name)
            lock=dict(item,path=str(path)); locks.append(lock); finals.append(lock)
        requirements = None; requirements_sha = None
        if platform=='web':
            _need(manifest['requirements'] is None, 'Web bundle must not declare native requirements')
        else:
            ref=_object(manifest['requirements'],'requirements reference',{'name','sha256'})
            requirements, lock = _json(root,ref['name'],ref['sha256']); locks.append(lock)
            requirements_sha=lock['sha256']
            _need(requirements.get('schema')=='caesura.package-build.v1' and requirements.get('platform')==platform
                  and requirements.get('configuration')==configuration and requirements.get('version')==version, 'Native requirements context/version mismatch')
            _configuration(platform, requirements.get('required_configuration'))
            artifacts=_object(requirements.get('artifacts'),'requirements artifacts')
            _need(all(item['name'] in artifacts.values() for item in files), 'Final file not associated with native requirements')
        seen_inputs, seen_names = set(), set()
        for validation in validations:
            _object(validation,'validation declaration',{'name','input','receipt'})
            name=_relative(validation['name'])
            _need(name.casefold() not in seen_names, 'Duplicate validation name'); seen_names.add(name.casefold())
            item=_object(validation['input'],'validation input',{'name','kind','sha256'})
            _need(item['name'] not in seen_inputs and any(_same(item,final) for final in files), 'Validation must match exactly one final file')
            seen_inputs.add(item['name'])
            ref=_object(validation['receipt'],'receipt reference',{'name','sha256'})
            receipt, lock = _json(root,ref['name'],ref['sha256']); locks.append(lock)
            _receipt(receipt,item,platform,source_sha,configuration,requirements,requirements_sha)
        result=dict(schema=SCHEMA,status='BUNDLE_VERIFIED',release_ready=False,
            authentication='NOT_PERFORMED',producer_context_matched=True,
            raw_stage_evidence='RAW_STAGE_LOGS_NOT_INCLUDED_NOT_REPLAYED',
            bundle_root=str(root),source_sha=source_sha,platform=platform,configuration=configuration,version=version,
            files=finals,manifest=manifest_lock,locks=locks)
        verify_bundle_stable(result)
        return result
    except (OSError, KeyError, TypeError, PackageVerificationError) as error:
        raise PackageBundleError(f'Cannot verify bundle: {error}') from error
