"""Classify two CMake-selected OpenSSL inputs without executing their code.

This checks ordinary-file identity, bounded ar framing or the existing thin
Mach-O load-command contract. It does not prove object-code ABI, install,
relocation, signing or runtime loading. The returned table is build evidence,
not an installed package manifest.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys

from macho_dependencies import inspect_macho

MAX_FILE_BYTES = 512 * 1024 * 1024
MAX_MEMBERS = 65536
MAX_NAME_BYTES = 8 * 1024 * 1024
PLAIN_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+-]*\Z")


def _need(condition, message):
    if not condition:
        raise ValueError(message)


def _identity(info):
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns)


def _path_state(selected):
    selected = Path(selected)
    _need(selected.is_absolute() and '..' not in selected.parts,
          'Selected input must be an absolute path without parent traversal')
    _need(not any(ord(c) < 32 or ord(c) == 127 for c in str(selected)),
          'Selected input path contains a control character')
    resolved = selected.resolve(strict=True)
    state = []
    # Observe aliases and directory identities on both the original route and
    # its canonical route. Ordinary directory mtimes are not file identities:
    # unrelated sibling creation must not invalidate this read-only selection.
    for route in (selected, resolved):
        for item in (*reversed(route.parents), route):
            info = item.lstat()
            entry = {'path': str(item), 'device': info.st_dev, 'inode': info.st_ino,
                     'mode': info.st_mode, 'attributes': getattr(info, 'st_file_attributes', 0),
                     'reparse_tag': getattr(info, 'st_reparse_tag', 0)}
            if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_reparse_tag', 0):
                entry.update(target=os.readlink(item), identity=_identity(info))
            elif item == route:
                entry['identity'] = _identity(info)
            else:
                _need(stat.S_ISDIR(info.st_mode), f'Non-directory input parent: {item}')
            state.append(entry)
    info = resolved.lstat()
    _need(stat.S_ISREG(info.st_mode) and not getattr(info, 'st_reparse_tag', 0),
          f'Selected input endpoint is not an ordinary file: {resolved}')
    _need(0 < info.st_size <= MAX_FILE_BYTES, f'Selected input size is unsupported: {resolved}')
    _need(selected.resolve(strict=True) == resolved, 'Selected input alias changed during inspection')
    return selected, resolved, info, state


def _open_regular(path, expected):
    # NONBLOCK prevents a FIFO substituted after preflight from hanging open;
    # NOFOLLOW rejects a final alias substituted for the canonical endpoint.
    flags = os.O_RDONLY | getattr(os, 'O_BINARY', 0) | getattr(os, 'O_NONBLOCK', 0) | getattr(os, 'O_NOFOLLOW', 0)
    fd = os.open(path, flags)
    try:
        observed = os.fstat(fd)
        _need(stat.S_ISREG(observed.st_mode), f'Opened input is not regular: {path}')
        # Windows path stat and fd stat expose different ctime meanings. Each
        # remains checked before/after against itself; match their other fields.
        _need(_identity(observed)[:4] == _identity(expected)[:4],
              f'Opened input differs from the selected endpoint: {path}')
        return os.fdopen(fd, 'rb'), observed
    except BaseException:
        os.close(fd)
        raise


def _decimal(raw, label, *, empty=False):
    raw = raw.strip(b' ')
    _need((empty and not raw) or (raw and all(48 <= c <= 57 for c in raw)),
          f'Invalid ar {label}')
    return int(raw) if raw else 0


def _archive(stream, size):
    _need(stream.read(8) == b'!<arch>\n', 'Expected a full, non-thin ar archive')
    position, members, objects = 8, 0, 0
    names = None
    while position < size:
        members += 1
        _need(members <= MAX_MEMBERS and size - position >= 60, 'Invalid ar member count/header extent')
        header = stream.read(60)
        _need(len(header) == 60 and header[58:60] == b'`\n', 'Malformed ar member header')
        for begin, end, label in ((16, 28, 'timestamp'), (28, 34, 'uid'), (34, 40, 'gid')):
            _decimal(header[begin:end], label, empty=True)
        mode = header[40:48].strip(b' ')
        _need(not mode or all(48 <= c <= 55 for c in mode), 'Invalid ar member mode')
        length = _decimal(header[48:58], 'member size')
        start, end = position + 60, position + 60 + length
        _need(end <= size, 'ar member extends outside the selected file')
        name = header[:16].rstrip(b' ')
        _need(name and all(32 <= c < 127 for c in name), 'Invalid ar member name')
        data_length = length
        if name.startswith(b'#1/'):
            name_length = _decimal(name[3:], 'BSD extended name length')
            _need(0 < name_length <= min(length, MAX_NAME_BYTES), 'Invalid BSD ar name extent')
            name = stream.read(name_length).rstrip(b'\0')
            _need(name and b'\0' not in name, 'Invalid BSD ar extended name')
            data_length -= name_length
        elif name == b'//':
            _need(names is None and length <= MAX_NAME_BYTES, 'Invalid/duplicate ar name table')
            names = stream.read(length)
            _need(len(names) == length, 'Truncated ar name table')
        elif name.startswith(b'/') and name not in (b'/', b'/SYM64/'):
            offset = _decimal(name[1:], 'GNU name offset')
            _need(names is not None and offset < len(names)
                  and (offset == 0 or names[offset - 1:offset] == b'\n'), 'Invalid ar name table reference')
            name_end = names.find(b'/\n', offset)
            _need(name_end > offset, 'Unterminated ar name table entry')
            name = names[offset:name_end]
        if name not in (b'/', b'//', b'/SYM64/') and not name.rstrip(b'/').startswith(b'__.SYMDEF'):
            _need(data_length > 0, 'Empty ar object member')
            objects += 1
        stream.seek(end)
        if length % 2:
            _need(stream.read(1) == b'\n', 'Missing ar alignment byte')
            end += 1
        position = end
    _need(position == size and objects > 0, 'Archive contains no complete object members')
    return {'member_count': members, 'object_members': objects, 'object_abi': 'NOT_VERIFIED'}


def _runtime_name(identifier):
    path = PurePosixPath(identifier)
    _need(str(path) == identifier and '..' not in path.parts and '.' not in path.parts,
          f'Noncanonical dylib identifier: {identifier}')
    if identifier.startswith('@'):
        _need(path.parts[0] in ('@rpath', '@loader_path', '@executable_path') and len(path.parts) > 1,
              f'Unsupported dylib identifier: {identifier}')
    name = path.name
    _need(PLAIN_NAME.fullmatch(name), f'Dylib runtime name is not a plain filename: {identifier}')
    return name


def _inspect_selected(component, selected):
    selected, resolved, path_before, state = _path_state(selected)
    stream, before = _open_regular(resolved, path_before)
    with stream:
        magic = stream.read(8)
        stream.seek(0)
        if magic == b'!<arch>\n':
            observation = _archive(stream, before.st_size)
            linkage, name = 'static', None
        else:
            observation = inspect_macho(resolved)
            _need(observation['file_type'] == 6 and len(observation['identifiers']) == 1,
                  f'Selected shared input must be a dylib with one identifier: {resolved}')
            linkage = 'shared'
            name = _runtime_name(observation['identifiers'][0])
        stream.seek(0)
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        _need(_identity(os.fstat(stream.fileno())) == _identity(before), 'Selected file changed while reading')
        if linkage == 'shared':
            _need(observation['sha256'] == digest, 'Mach-O inspection and held input bytes differ')
        _need(_path_state(selected)[3] == state, 'Selected input route/identity changed while reading')
    return {'component': component, 'selected_path': str(selected), 'resolved_path': str(resolved),
            'sha256': digest, 'size': before.st_size, 'linkage': linkage, 'runtime_name': name,
            'path_observation': state, 'format_observation': observation}


def _recheck(record):
    selected, resolved, before, state = _path_state(record['selected_path'])
    _need(state == record['path_observation'] and str(resolved) == record['resolved_path'],
          'Selected input changed while selecting the other component')
    stream, fd_before = _open_regular(resolved, before)
    with stream:
        _need(hashlib.file_digest(stream, 'sha256').hexdigest() == record['sha256'],
              'Selected input digest changed while selecting the other component')
        _need(_identity(os.fstat(stream.fileno())) == _identity(fd_before)
              and _path_state(selected)[3] == state, 'Selected input changed during final readback')


def select_openssl(ssl_file, crypto_file, configuration):
    _need(isinstance(configuration, str) and PLAIN_NAME.fullmatch(configuration),
          'Configuration must be one nonempty plain name')
    components = [_inspect_selected('SSL', ssl_file), _inspect_selected('Crypto', crypto_file)]
    _need(components[0]['linkage'] == components[1]['linkage'], 'OpenSSL components have mixed linkage')
    _need(not os.path.samefile(components[0]['resolved_path'], components[1]['resolved_path']),
          'OpenSSL components select the same file')
    linkage = components[0]['linkage']
    if linkage == 'shared':
        _need(components[0]['runtime_name'].casefold() != components[1]['runtime_name'].casefold(),
              'OpenSSL component runtime names collide')
        _need(components[0]['format_observation']['cpu_type'] == components[1]['format_observation']['cpu_type'],
              'Selected OpenSSL dylibs have different CPUs')
    for record in components:
        _recheck(record)
    return {'schema': 'caesura.macos-openssl-selection.v1', 'configuration': configuration,
            'linkage': linkage, 'components': components,
            'libraries': [r['runtime_name'] for r in components] if linkage == 'shared' else [],
            'installation': 'NOT_RUN', 'abi': 'NOT_VERIFIED', 'signing': 'NOT_RUN'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ssl-file', required=True)
    parser.add_argument('--crypto-file', required=True)
    parser.add_argument('--configuration', required=True)
    args = parser.parse_args()
    try:
        result = select_openssl(args.ssl_file, args.crypto_file, args.configuration)
    except (OSError, ValueError, RuntimeError) as error:
        print(f'OpenSSL selection failed: {error}', file=sys.stderr)
        return 1
    print(json.dumps(result, ensure_ascii=True, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
