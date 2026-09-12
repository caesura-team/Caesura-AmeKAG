#!/usr/bin/env python3
"""Verify the fixed U16 D3D11/OpenGL suite; never retry or train pixel references."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import stat
import struct
import subprocess
import sys
import time
import zlib

from validation_process import run_owned_command

SUITE_ID = 'u16-render-contracts-v4'
BACKENDS = ('dx11', 'opengl')
CAPTURE_IDS = {
    'text-cjk-ruby': ('text-opaque', 'text-alpha', 'ruby-cjk', 'ruby-ascii'),
    'alpha-layers-batch': ('alpha-texture', 'opacity-unbatched', 'opacity-batched', 'multitexture-unbatched', 'multitexture-batched'),
    'rtt-fill-resize': ('fill-control', 'fill-first-a', 'fill-same-a', 'fill-changed-b', 'resized', 'returned', 'recreated', 'rtt-orientation'),
    'transition': ('blend-0', 'blend-025', 'blend-050', 'blend-1', 'wipe-half', 'rule-half'),
    'lut3d': ('baseline', 'identity-16', 'identity-64', 'swap-16', 'swap-64', 'half-16', 'half-64', 'strength-zero', 'cleared', 'borrowed-after-clear', 'postfx-destroy-last', 'postfx-invalid-only', 'postfx-clear-after-begin', 'postfx-swap-invalid-tail'),
    'shader-core-fallback': (), 'shader-core-blend': (),
    'shader-optional-softblur': ('baseline', 'degraded'), 'shader-optional-transition': ('baseline', 'degraded'),
}
CASE_IDS = tuple(CAPTURE_IDS)
FAULTS = {
    'shader-core-fallback': ('core_failure', 'fallback-missing-fragment', 'Fallback'),
    'shader-core-blend': ('core_failure', 'blend-missing-fragment', 'Blend'),
    'shader-optional-softblur': ('optional_degrade', 'softblur-missing-fragment', 'PostFxSoftBlur'),
    'shader-optional-transition': ('optional_degrade', 'transition-missing-fragment', 'Transition'),
}
DEFAULTS = {'width': 640, 'height': 360, 'case_timeout_seconds': 60, 'rgb_tolerance': 2, 'effect_tolerance': 3, 'edge_tolerance': 4}
FONT_FIXED = {'path': 'assets/fonts/NotoSansCJKsc-Regular.otf', 'pixel_size': 28,
    'load_flags': 'FT_LOAD_DEFAULT', 'render_mode': 'FT_RENDER_MODE_NORMAL', 'sampling': 'pixel-center-bilinear-zero-extended',
    'minimum_opaque_pixels': 64, 'minimum_edge_pixels': 16, 'opaque_tolerance': 2, 'edge_tolerance': 4, 'center_tolerance': .75}
SHA = re.compile(r'^[0-9a-f]{64}$')
MAX_FILE_BYTES = 64 * 1024 * 1024
MAX_LOG_BYTES = 8 * 1024 * 1024
MAX_CHECKPOINT_BYTES = 16 * 1024
MAX_PIXELS = 8 * 1024 * 1024
# JSON semantic identity, versioned with this suite. Whitespace/key ordering
# changes are harmless; recipes, font identity, parameters and colors are not.
CANONICAL_MANIFEST_SHA256 = '87454abdd3448c49d271419d90b95217b84576ce9e1461a1d4073be0ed4ad064'


class ContractError(ValueError):
    pass


class OutputBoundaryError(ContractError):
    pass


def require(condition, message):
    if not condition:
        raise ContractError(message)


def fields(value, required, optional=()):
    require(isinstance(value, dict), 'Expected a JSON object')
    require(set(required) <= set(value) <= set(required) | set(optional),
            f'Object fields differ: missing={sorted(set(required)-set(value))}, extra={sorted(set(value)-set(required)-set(optional))}')


def integer(value, minimum, maximum):
    require(type(value) is int and minimum <= value <= maximum, f'Invalid integer: {value!r}')
    return value


def number(value, minimum, maximum):
    require(type(value) in (int, float) and math.isfinite(value) and minimum <= value <= maximum, f'Invalid number: {value!r}')
    return value


def color(value, channels=3):
    require(isinstance(value, list) and len(value) == channels, 'Invalid color length')
    for channel in value:
        integer(channel, 0, 255)
    return value


def rectangle(value, width, height):
    require(isinstance(value, list) and len(value) == 4, 'Invalid rectangle')
    left, top, right, bottom = value
    for coordinate in value:
        integer(coordinate, 0, max(width, height))
    require(0 <= left < right <= width and 0 <= top < bottom <= height, 'Empty/out-of-bounds rectangle')
    return value


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f'Duplicate JSON key: {key}')
        result[key] = value
    return result


def read_json(path):
    try:
        require(path.stat().st_size <= 2 * 1024 * 1024, f'JSON exceeds limit: {path.name}')
        return json.loads(path.read_text(encoding='utf-8-sig'), object_pairs_hook=unique_object,
                          parse_constant=lambda value: (_ for _ in ()).throw(ContractError(f'Nonfinite JSON number: {value}')))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ContractError(f'Cannot read {path.name}: {error}') from error


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def canonical_manifest_sha256(value):
    encoded = json.dumps(value, sort_keys=True, ensure_ascii=False, separators=(',', ':'), allow_nan=False).encode('utf-8')
    return hashlib.sha256(encoded).hexdigest()


def redirected(info):
    return stat.S_ISLNK(info.st_mode) or bool(getattr(info, 'st_file_attributes', 0) & 0x400)


def filesystem_identity(info):
    # mtime/ctime may change legitimately when a child writes files. Birth time
    # (where available) and filesystem object identity do not.
    return info.st_dev, info.st_ino, getattr(info, 'st_birthtime_ns', None)


def checked_directory_chain(path):
    """Inspect lexical components BEFORE resolve can follow a replaced root."""
    path = Path(os.path.abspath(path))
    identities = []
    try:
        for directory in (*reversed(path.parents), path):
            info = directory.lstat()
            if redirected(info) or not stat.S_ISDIR(info.st_mode):
                raise OutputBoundaryError(f'Linked/non-directory output component: {directory}')
            identities.append((directory, filesystem_identity(info)))
        if path.resolve(strict=True) != path:
            raise OutputBoundaryError(f'Output canonical path changed: {path}')
    except OSError as error:
        raise OutputBoundaryError(f'Cannot inspect output directory: {path}: {error}') from error
    return path, identities


class DirectoryBoundary:
    """Immutable pre-child directory chain and parent-owned file identities."""
    def __init__(self, run_root, leaf=None):
        self.root, _ = checked_directory_chain(run_root)
        self.leaf, self.identities = checked_directory_chain(leaf or run_root)
        if not self.leaf.is_relative_to(self.root):
            raise OutputBoundaryError('Case directory is outside the original run root')
        self.parent_files = {}

    def pin_parent_file(self, name, stream):
        self.verify()
        file = self.leaf / name
        info = file.lstat()
        if redirected(info) or not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
            raise OutputBoundaryError(f'Invalid parent-owned output file: {file}')
        if filesystem_identity(info) != filesystem_identity(os.fstat(stream.fileno())):
            raise OutputBoundaryError(f'Parent-owned output file changed while opening: {file}')
        self.parent_files[name] = filesystem_identity(info)

    def verify(self, contents=False):
        _, current = checked_directory_chain(self.leaf)
        if current != self.identities or not self.leaf.is_relative_to(self.root):
            raise OutputBoundaryError(f'Original output directory identity changed: {self.leaf}')
        for name, identity in self.parent_files.items():
            try:
                info = (self.leaf / name).lstat()
            except OSError as error:
                raise OutputBoundaryError(f'Parent output file disappeared: {name}') from error
            if redirected(info) or not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or filesystem_identity(info) != identity:
                raise OutputBoundaryError(f'Parent output file was linked/replaced: {name}')
        if not contents:
            return
        pending, count = [self.leaf], 0
        while pending:
            directory = pending.pop()
            for file in directory.iterdir():
                count += 1
                if count > 4096:
                    raise OutputBoundaryError('Output entry count exceeds limit')
                info = file.lstat()
                if redirected(info):
                    raise OutputBoundaryError(f'Linked output entry: {file}')
                if stat.S_ISDIR(info.st_mode):
                    pending.append(file)
                elif not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
                    raise OutputBoundaryError(f'Non-regular/aliased output entry: {file}')
                elif info.st_size > (MAX_LOG_BYTES if file.name in ('stdout.log', 'stderr.log') else MAX_FILE_BYTES):
                    raise OutputBoundaryError(f'Output file exceeds byte budget: {file.name}')


class OwnedJsonFile:
    """Exclusive parent metadata inode, never reopened through a child path.

    The run report stays attached to its original file handle if a child
    replaces a directory. Failure reporting therefore cannot follow the new
    junction into an external directory. The original report may have moved
    with its original directory, but no external file is opened for writing.
    """
    def __init__(self, path):
        self.stream = path.open('x+', encoding='utf-8', newline='\n')

    def update(self, value):
        info = os.fstat(self.stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_nlink > 1:
            raise OutputBoundaryError('Parent metadata inode gained an external hardlink')
        self.stream.seek(0)
        json.dump(value, self.stream, ensure_ascii=False, indent=2)
        self.stream.write('\n')
        self.stream.truncate()
        self.stream.flush()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.stream.close()


def safe_file(base, relative, used=None, limit=MAX_FILE_BYTES):
    require(isinstance(relative, str) and relative and not any(ch in relative for ch in ('\\', ':', '\0')),
            f'Invalid relative output path: {relative!r}')
    require(not relative.startswith('/') and all(part not in ('', '.', '..') for part in relative.split('/')),
            f'Output traversal/absolute path: {relative!r}')
    try:
        base, _ = checked_directory_chain(base)
        path = (base / relative).resolve(strict=True)
        require(path.is_relative_to(base) and path.is_file(), f'Output escapes its directory: {relative}')
        cursor = base
        for part in relative.split('/'):
            cursor = cursor / part
            require(not redirected(cursor.lstat()), f'Linked output is not allowed: {relative}')
        stat = path.stat()
        require(stat.st_size <= limit, f'File exceeds byte budget: {relative}')
        if used is not None:
            identities = {('path', str(path).casefold()), ('inode', stat.st_dev, stat.st_ino)}
            require(not identities & used and stat.st_nlink == 1, f'Aliased output file: {relative}')
            used.update(identities)
        return path
    except OSError as error:
        raise ContractError(f'Cannot access {relative!r}: {error}') from error


def expected_rgb(expect):
    require(isinstance(expect, dict), 'Pixel expectation must be an object')
    kind = expect.get('kind')
    if kind == 'rgb':
        fields(expect, ('kind', 'value'))
        return color(expect['value'])
    if kind == 'lut_texel':
        fields(expect, ('kind', 'size', 'indices', 'transform'))
        size = integer(expect['size'], 16, 64)
        require(size in (16, 64), 'Only the fixed 16/64 LUT atlas sizes are accepted')
        indices = expect['indices']
        require(isinstance(indices, list) and len(indices) == 3, 'LUT texel requires r/g/b indices')
        rgb = [math.floor(integer(index, 0, size - 1) * 255 / (size - 1) + .5) for index in indices]
        require(expect['transform'] in ('identity', 'swap_rb'), 'Unknown LUT atlas transform')
        return rgb if expect['transform'] == 'identity' else [rgb[2], rgb[1], rgb[0]]
    if kind == 'alpha_over':
        fields(expect, ('kind', 'src', 'dst', 'texture_alpha', 'opacity'))
        src, dst = color(expect['src']), color(expect['dst'])
        amount = integer(expect['texture_alpha'], 0, 255) * integer(expect['opacity'], 0, 255) / (255 * 255)
    elif kind == 'lerp':
        fields(expect, ('kind', 'from', 'to', 'progress'))
        src, dst = color(expect['to']), color(expect['from'])
        amount = number(expect['progress'], 0, 1)
    elif kind == 'lut':
        fields(expect, ('kind', 'input', 'transform', 'strength'))
        dst = color(expect['input'])
        require(expect['transform'] in ('identity', 'swap_rb'), 'Unknown LUT transform')
        src = dst if expect['transform'] == 'identity' else [dst[2], dst[1], dst[0]]
        amount = number(expect['strength'], 0, 1)
    else:
        raise ContractError(f'Unknown pixel formula: {kind!r}')
    return [math.floor(amount * source + (1 - amount) * destination + .5) for source, destination in zip(src, dst)]


def required_checks(case_id):
    expected = FAULTS.get(case_id, ('rendered', 'none', None))[0]
    result = ['actual_backend', 'shutdown']
    if expected == 'core_failure':
        result += ['fault_applied_once', 'core_failed', 'capture_rejected', 'no_invalid_submit']
    else:
        result += ['core_ready', 'not_ifh'] + ['capture:' + name for name in CAPTURE_IDS[case_id]]
        if expected == 'optional_degrade':
            result += ['fault_applied_once', 'optional_degraded', 'no_invalid_submit']
    if case_id == 'rtt-fill-resize':
        result += ['fill_cache_reuse', 'resize_roundtrip', 'rtt_recreated']
    if case_id == 'lut3d':
        result += ['lut_borrowed_texture_alive', 'postfx_destroy_last_exercised', 'postfx_invalid_only_exercised',
                   'postfx_clear_after_begin_exercised', 'postfx_swap_invalid_tail_exercised']
    return result


def validate_font_contract(font, width, height):
    fields(font, ('runs', 'color', 'rect'))
    color(font['color'], 4)
    rectangle(font['rect'], width, height)
    require(isinstance(font['runs'], list) and 1 <= len(font['runs']) <= 16, 'Invalid font run count')
    for index, run in enumerate(font['runs']):
        fields(run, ('text', 'x', 'y', 'scale'), ('center_over',))
        require(isinstance(run['text'], str) and 1 <= len(run['text']) <= 256, 'Invalid fixed text')
        try:
            run['text'].encode('utf-8')
        except UnicodeError as error:
            raise ContractError('Text has invalid Unicode') from error
        require(all(ord(char) >= 32 for char in run['text']), 'Control characters in font fixture')
        number(run['x'], 0, width); number(run['y'], 0, height)
        require(type(run['scale']) in (int, float) and run['scale'] in (.5, 1), 'Font scale must be 0.5 or 1')
        if 'center_over' in run:
            integer(run['center_over'], 0, index - 1)


def load_manifest(path):
    manifest = read_json(Path(path))
    fields(manifest, ('schema_version', 'suite_id', 'required_backends', 'defaults', 'font', 'cases'))
    integer(manifest['schema_version'], 1, 1)
    require(manifest['suite_id'] == SUITE_ID, 'Wrong suite ID')
    require(manifest['required_backends'] == list(BACKENDS), 'Exactly dx11 then opengl are required')
    require(manifest['defaults'] == DEFAULTS, 'Default dimensions/timeouts/tolerances changed')
    fields(manifest['font'], (*FONT_FIXED, 'sha256'))
    require({key: manifest['font'][key] for key in FONT_FIXED} == FONT_FIXED, 'Font flags/geometry/thresholds changed')
    require(isinstance(manifest['font']['sha256'], str) and SHA.fullmatch(manifest['font']['sha256']), 'Invalid font hash')
    cases = manifest['cases']
    require(isinstance(cases, list) and all(isinstance(case, dict) for case in cases)
            and [case.get('id') for case in cases] == list(CASE_IDS), 'Fixed nine case IDs/order required')
    for case in cases:
        fields(case, ('id', 'expected', 'fault_requested', 'failed_programs', 'required_checks', 'recipe', 'captures'))
        case_id = case['id']
        expected, fault, failed = FAULTS.get(case_id, ('rendered', 'none', None))
        require((case['expected'], case['fault_requested'], case['failed_programs']) == (expected, fault, [failed] if failed else []), 'Case fault/expectation was changed')
        require(case['required_checks'] == required_checks(case_id), f'Required checks changed: {case_id}')
        require(isinstance(case['recipe'], dict) and case['recipe'].get('kind') == case_id, 'Wrong recipe kind')
        captures = case['captures']
        require(isinstance(captures, list) and all(isinstance(capture, dict) for capture in captures)
                and [capture.get('id') for capture in captures] == list(CAPTURE_IDS[case_id]), f'Capture IDs/order changed: {case_id}')
        previous = set()
        for capture in captures:
            fields(capture, ('id', 'width', 'height', 'background', 'parameters', 'regions'), ('font', 'compare_to'))
            width, height = integer(capture['width'], 1, 8192), integer(capture['height'], 1, 8192)
            canvas = ((4096, 224) if case_id == 'lut3d' and capture['id'] == 'borrowed-after-clear'
                      else (800, 450) if case_id == 'rtt-fill-resize' and capture['id'] == 'resized' else (640, 360))
            require((width, height) == canvas, 'Capture canvas changed')
            color(capture['background'], 4)
            require(isinstance(capture['parameters'], dict), 'Capture parameters must be an object')
            require(isinstance(capture['regions'], list), 'Regions must be an array')
            require(bool(capture['regions']) or 'font' in capture, 'Capture has no independent pixel oracle')
            for region in capture['regions']:
                fields(region, ('rect', 'expect', 'tolerance'))
                left, top, right, bottom = rectangle(region['rect'], width, height)
                expected_rgb(region['expect'])
                if region['expect'].get('kind') == 'lut_texel':
                    require(case_id == 'lut3d' and capture['id'] == 'borrowed-after-clear'
                            and right - left == 1 and bottom - top == 1 and region['tolerance'] == 2,
                            'Single-pixel LUT observations are fixed to borrowed-after-clear with tolerance 2')
                else:
                    require(right - left >= 16 and bottom - top >= 16, 'Pixel ROI too small')
                require(type(region['tolerance']) is int and region['tolerance'] in (2, 3), 'Pixel tolerance must remain 2 or 3')
            if 'compare_to' in capture:
                require(capture['compare_to'] in previous, 'Comparison must name an earlier capture')
            if 'font' in capture:
                validate_font_contract(capture['font'], width, height)
            previous.add(capture['id'])
    require(canonical_manifest_sha256(manifest) == CANONICAL_MANIFEST_SHA256,
            'Manifest semantic identity differs from the frozen u16-render-contracts-v4 contract')
    return manifest


def decode_png(encoded):
    """Strict, bounded, noninterlaced RGBA8 PNG decoder (filters 0..4)."""
    require(isinstance(encoded, bytes) and len(encoded) <= MAX_FILE_BYTES, 'PNG byte budget/type')
    require(encoded.startswith(b'\x89PNG\r\n\x1a\n'), 'Bad PNG signature')
    offset, width, height = 8, None, None
    idat = bytearray()
    started_idat = ended_idat = ended = False
    while offset < len(encoded):
        require(offset + 12 <= len(encoded), 'Truncated PNG chunk')
        length = struct.unpack_from('>I', encoded, offset)[0]
        require(length <= MAX_FILE_BYTES and offset + 12 + length <= len(encoded), 'Invalid PNG chunk length')
        kind = encoded[offset + 4:offset + 8]
        require(all(65 <= value <= 90 or 97 <= value <= 122 for value in kind)
                and not kind[2] & 32, 'Invalid PNG chunk type/reserved bit')
        body = encoded[offset + 8:offset + 8 + length]
        crc = struct.unpack_from('>I', encoded, offset + 8 + length)[0]
        require(zlib.crc32(kind + body) & 0xffffffff == crc, 'PNG CRC mismatch')
        require(width is not None or kind == b'IHDR', 'IHDR must be first')
        if kind == b'IHDR':
            require(width is None and length == 13, 'Duplicate/invalid IHDR')
            width, height, depth, mode, compression, filtering, interlace = struct.unpack('>IIBBBBB', body)
            require(0 < width <= 8192 and 0 < height <= 8192 and width * height <= MAX_PIXELS, 'PNG geometry exceeds limit')
            require((depth, mode, compression, filtering, interlace) == (8, 6, 0, 0, 0), 'Only noninterlaced RGBA8 PNG is accepted')
        elif kind == b'IDAT':
            require(not ended_idat, 'PNG IDAT chunks must be consecutive')
            started_idat = True
            idat.extend(body)
        elif kind == b'IEND':
            require(started_idat and length == 0, 'Invalid IEND')
            offset += length + 12
            require(offset == len(encoded), 'Trailing PNG bytes')
            ended = True
            break
        else:
            require(kind and kind[0] & 32, f'Unsupported critical PNG chunk: {kind!r}')
            require(kind not in (b'acTL', b'fcTL', b'fdAT'), 'Animated PNG not supported')
            if started_idat:
                ended_idat = True
        offset += length + 12
    require(ended and width is not None and bool(idat), 'Incomplete PNG')
    stride = width * 4
    expected = (stride + 1) * height
    try:
        inflater = zlib.decompressobj()
        filtered = inflater.decompress(idat, expected + 1)
    except zlib.error as error:
        raise ContractError(f'Invalid PNG zlib stream: {error}') from error
    require(len(filtered) == expected and inflater.eof and not inflater.unused_data and not inflater.unconsumed_tail, 'PNG decompressed length/stream mismatch')
    rgba = bytearray(stride * height)
    for y in range(height):
        mode = filtered[y * (stride + 1)]
        require(mode <= 4, 'Invalid PNG scanline filter')
        source = filtered[y * (stride + 1) + 1:(y + 1) * (stride + 1)]
        if mode == 0:
            rgba[y * stride:(y + 1) * stride] = source
            continue
        for x, value in enumerate(source):
            index = y * stride + x
            a = rgba[index - 4] if x >= 4 else 0
            b = rgba[index - stride] if y else 0
            c = rgba[index - stride - 4] if y and x >= 4 else 0
            if mode == 1: predictor = a
            elif mode == 2: predictor = b
            elif mode == 3: predictor = (a + b) // 2
            else:
                estimate = a + b - c
                da, db, dc = abs(estimate - a), abs(estimate - b), abs(estimate - c)
                predictor = a if da <= db and da <= dc else b if db <= dc else c
            rgba[index] = (value + predictor) & 255
    return width, height, bytes(rgba)


def check_regions(capture, rgba):
    errors = []
    width = capture['width']
    for index, region in enumerate(capture['regions']):
        expected = expected_rgb(region['expect'])
        left, top, right, bottom = region['rect']
        first = None
        for y in range(top, bottom):
            for x in range(left, right):
                offset = (y * width + x) * 4
                actual = rgba[offset:offset + 3]
                if any(abs(value - wanted) > region['tolerance'] for value, wanted in zip(actual, expected)):
                    first = (x, y, list(actual)); break
            if first: break
        if first:
            errors.append(f"{capture['id']} ROI {index} differs at {first[:2]}: observed={first[2]}, expected={expected}, tolerance={region['tolerance']}")
    return errors


def validate_font_references(manifest, case, output, resources, images, used):
    expected = {capture['id']: capture for capture in case['captures'] if 'font' in capture}
    if not expected:
        require(not (output / 'font-reference.json').exists(), 'Unexpected font reference')
        return []
    reference_file = safe_file(output, 'font-reference.json', used, 2 * 1024 * 1024)
    report = read_json(reference_file)
    fields(report, ('schema_version', 'font_path', 'font_sha256', 'freetype_version', 'pixel_size', 'load_flags', 'render_mode', 'sampling', 'ascender', 'references'))
    integer(report['schema_version'], 1, 1)
    contract = manifest['font']
    require(report['font_path'] == contract['path'] and report['font_sha256'] == contract['sha256'], 'Stale/wrong font reference hash/path')
    require(sha256(safe_file(resources, contract['path'])) == contract['sha256'], 'Actual font bytes differ from manifest')
    for key in ('pixel_size', 'load_flags', 'render_mode', 'sampling'):
        require(report[key] == contract[key], f'Font reference parameter changed: {key}')
    require(isinstance(report['freetype_version'], str) and re.fullmatch(r'\d+\.\d+\.\d+', report['freetype_version']), 'Missing FreeType version')
    ascender = number(report['ascender'], 1, 128)
    references = report['references']
    require(isinstance(references, list) and all(isinstance(item, dict) for item in references)
            and [item.get('capture_id') for item in references] == list(expected), 'Font reference capture IDs/order differ')
    errors = []
    for reference in references:
        fields(reference, ('capture_id', 'width', 'height', 'coverage', 'coverage_sha256', 'opaque_pixels', 'edge_pixels', 'glyphs'))
        name = reference['capture_id']; capture = expected[name]; font = capture['font']
        width, height = capture['width'], capture['height']
        require((reference['width'], reference['height']) == (width, height), 'Font mask dimensions differ')
        mask_file = safe_file(output, reference['coverage'], used, width * height)
        require(isinstance(reference['coverage_sha256'], str) and SHA.fullmatch(reference['coverage_sha256']), 'Invalid coverage hash')
        require(sha256(mask_file) == reference['coverage_sha256'], 'Coverage hash mismatch')
        mask = mask_file.read_bytes()
        require(len(mask) == width * height, 'Wrong coverage byte length')
        opaque, edges = mask.count(255), sum(0 < value < 255 for value in mask)
        require(type(reference['opaque_pixels']) is int and type(reference['edge_pixels']) is int
                and (reference['opaque_pixels'], reference['edge_pixels']) == (opaque, edges), 'Reported font counts differ from mask')
        require(opaque >= 64 and edges >= 16, 'Empty/inadequate font oracle')
        left, top, right, bottom = font['rect']
        for y in range(height):
            row = mask[y * width:(y + 1) * width]
            require(not any(row) if y < top or y >= bottom else not any(row[:left]) and not any(row[right:]), 'Coverage outside fixed font ROI')
        glyphs = reference['glyphs']
        expected_sequence = [(index, ord(char)) for index, run in enumerate(font['runs']) for char in run['text']]
        require(isinstance(glyphs, list) and all(isinstance(glyph, dict) for glyph in glyphs)
                and [(glyph.get('run'), glyph.get('codepoint')) for glyph in glyphs] == expected_sequence, 'Missing/reordered font glyphs')
        groups = [[glyph for glyph in glyphs if glyph['run'] == index] for index in range(len(font['runs']))]
        advances = []
        for group in groups:
            advances.append(sum(integer(glyph.get('advance_x'), 0, 256) for glyph in group))
        origins = []
        for index, (run, group) in enumerate(zip(font['runs'], groups)):
            scale = run['scale']
            if 'center_over' in run:
                base = run['center_over']
                origin = origins[base] + (advances[base] * font['runs'][base]['scale'] - advances[index] * scale) / 2
            else:
                origin = run['x']
            origins.append(origin)
            pen = origin
            for glyph in group:
                fields(glyph, ('run', 'codepoint', 'glyph_index', 'advance_x', 'bitmap_left', 'bitmap_top', 'bitmap_width', 'bitmap_height', 'pen_x', 'baseline', 'left', 'top', 'width', 'height'))
                integer(glyph['run'], 0, len(font['runs']) - 1)
                integer(glyph['codepoint'], 32, 0x10ffff)
                integer(glyph['glyph_index'], 1, 2**32 - 1)
                integer(glyph['bitmap_left'], -256, 256); integer(glyph['bitmap_top'], -256, 256)
                integer(glyph['bitmap_width'], 0, 256); integer(glyph['bitmap_height'], 0, 256)
                if glyph['codepoint'] != 32:
                    require(glyph['bitmap_width'] > 0 and glyph['bitmap_height'] > 0, 'Non-space glyph has no bitmap')
                for key in ('pen_x', 'baseline', 'left', 'top', 'width', 'height'):
                    number(glyph[key], -256, 8192)
                require(abs(glyph['pen_x'] - pen) <= (.75 if 'center_over' in run else 1e-6), 'Glyph origin/advance/centering differs')
                require(abs(glyph['baseline'] - (run['y'] + ascender * scale)) <= 1e-6, 'Glyph baseline differs')
                require(abs(glyph['left'] - (glyph['pen_x'] + glyph['bitmap_left'] * scale)) <= 1e-6
                        and abs(glyph['top'] - (glyph['baseline'] - glyph['bitmap_top'] * scale)) <= 1e-6
                        and abs(glyph['width'] - glyph['bitmap_width'] * scale) <= 1e-6
                        and abs(glyph['height'] - glyph['bitmap_height'] * scale) <= 1e-6, 'Glyph bearing/extent equation differs')
                if glyph['codepoint'] != 32:
                    x0, y0 = math.floor(glyph['left']), math.floor(glyph['top'])
                    x1, y1 = math.ceil(glyph['left'] + glyph['width']), math.ceil(glyph['top'] + glyph['height'])
                    require(left <= x0 < x1 <= right and top <= y0 < y1 <= bottom, 'Glyph outside fixed ROI')
                    require(any(mask[y * width + x] for y in range(y0, y1) for x in range(x0, x1)), 'Reference glyph has no coverage')
                pen += glyph['advance_x'] * scale
        rgba = images[name][2]
        foreground, background = font['color'], capture['background']
        first = None
        for y in range(top, bottom):
            for x in range(left, right):
                coverage = mask[y * width + x]
                alpha = coverage * foreground[3] / (255 * 255)
                wanted = [math.floor(foreground[ch] * alpha + background[ch] * (1 - alpha) + .5) for ch in range(3)]
                actual = rgba[(y * width + x) * 4:(y * width + x) * 4 + 3]
                tolerance = 2 if coverage == 255 else 4
                if any(abs(value - goal) > tolerance for value, goal in zip(actual, wanted)):
                    first = (x, y, coverage, list(actual), wanted); break
            if first: break
        if first:
            errors.append(f'{name}: font mask mismatch x/y/coverage/actual/expected={first}')
    return errors


def validate_result(manifest, case, output, probe, resources, backend, boundary=None):
    errors, images = [], {}
    try:
        boundary = boundary or DirectoryBoundary(output)
        boundary.verify(contents=True)
        used = set()
        result = read_json(safe_file(output, 'result.json', used, 2 * 1024 * 1024))
        fields(result, ('schema_version', 'suite_id', 'case_id', 'requested_backend', 'actual_backend', 'backend_name', 'binary_path', 'pid', 'expected', 'status', 'shutdown_completed', 'shader_health', 'checks', 'captures'))
        integer(result['schema_version'], 1, 1); integer(result['pid'], 1, 2**32 - 1)
        require(result['suite_id'] == SUITE_ID and result['case_id'] == case['id'], 'Wrong suite/case identity')
        require(result['requested_backend'] == backend and result['actual_backend'] == backend and backend in BACKENDS, 'Noop or wrong actual/requested backend')
        require(isinstance(result['backend_name'], str) and result['backend_name'].strip() and 'noop' not in result['backend_name'].lower(), 'Missing/Noop backend name')
        require(isinstance(result['binary_path'], str) and Path(result['binary_path']).is_absolute()
                and Path(result['binary_path']).resolve() == probe.resolve(), 'Wrong executable identity')
        require(result['shutdown_completed'] is True and result['expected'] == case['expected'], 'Wrong expected mode or incomplete shutdown')
        expected_status = 'EXPECTED_FAILURE_OBSERVED' if case['expected'] == 'core_failure' else 'PASS'
        require(result['status'] == expected_status, f"Probe terminal status: {result['status']!r}")
        health = result['shader_health']
        fields(health, ('core_ready', 'rendering_disabled', 'ifh', 'fault_requested', 'fault_applied_count', 'failed_programs'))
        for key in ('core_ready', 'rendering_disabled', 'ifh'):
            require(type(health[key]) is bool, f'Nonboolean shader health: {key}')
        faulted = case['expected'] == 'core_failure'
        require(health['core_ready'] is not faulted and health['rendering_disabled'] is faulted, 'Core shader/disabled state disagrees')
        require(faulted or health['ifh'] is False, 'IFH cannot pass a rendered/optional case')
        integer(health['fault_applied_count'], 0, 1)
        require(health['fault_requested'] == case['fault_requested'] and health['fault_applied_count'] == int(case['fault_requested'] != 'none')
                and health['failed_programs'] == case['failed_programs'], 'Fault application/program identity differs')
        checks = result['checks']
        require(isinstance(checks, list) and all(isinstance(check, dict) and isinstance(check.get('id'), str) for check in checks), 'Invalid checks array')
        check_ids = [check['id'] for check in checks]
        require(len(check_ids) == len(set(check_ids)) and set(check_ids) == set(case['required_checks']), 'Missing/duplicate/extra required checks')
        for check in checks:
            fields(check, ('id', 'passed', 'detail'))
            require(check['passed'] is True and isinstance(check['detail'], dict), f"Probe check failed: {check['id']}")
        captures = result['captures']
        require(isinstance(captures, list) and all(isinstance(capture, dict) for capture in captures)
                and [capture.get('id') for capture in captures] == [capture['id'] for capture in case['captures']], 'Capture set/order differs')
        # The real probe publishes this side file at every phase. A terminal
        # result alone must not conceal an incomplete shutdown/checkpoint.
        checkpoint = read_json(safe_file(output, 'checkpoint.json', used, MAX_CHECKPOINT_BYTES))
        fields(checkpoint, ('phase', 'owner_advances'))
        require(checkpoint['phase'] == 'after-shutdown', 'checkpoint.json did not reach after-shutdown')
        integer(checkpoint['owner_advances'], max(1, len(captures)), 128)
        requests, previous_frame, generation = set(), 0, None
        for capture, expected in zip(captures, case['captures']):
            fields(capture, ('id', 'png', 'rgba', 'width', 'height', 'png_sha256', 'rgba_sha256', 'ticket'))
            width, height = integer(capture['width'], 1, 8192), integer(capture['height'], 1, 8192)
            require((width, height) == (expected['width'], expected['height']), 'Capture dimensions differ')
            ticket = capture['ticket']
            fields(ticket, ('request_id', 'generation', 'frame_id', 'status'))
            for key in ('request_id', 'generation', 'frame_id'):
                integer(ticket[key], 1, 2**64 - 1)
            require(ticket['status'] == 'Completed' and ticket['request_id'] not in requests, 'Capture is pending/cancelled or ticket reused')
            require(ticket['frame_id'] > previous_frame and (generation is None or ticket['generation'] == generation), 'Capture submission order/generation changed')
            requests.add(ticket['request_id']); previous_frame = ticket['frame_id']; generation = ticket['generation']
            for key in ('png_sha256', 'rgba_sha256'):
                require(isinstance(capture[key], str) and SHA.fullmatch(capture[key]), f'Invalid {key}')
            png_file, raw_file = safe_file(output, capture['png'], used), safe_file(output, capture['rgba'], used, width * height * 4)
            require(sha256(png_file) == capture['png_sha256'] and sha256(raw_file) == capture['rgba_sha256'], 'Capture file hash mismatch')
            decoded = decode_png(png_file.read_bytes())
            require(decoded[:2] == (width, height), 'PNG IHDR differs from requested dimensions')
            raw = raw_file.read_bytes()
            require(len(raw) == width * height * 4 and decoded[2] == raw, 'PNG pixels and production-decoded RGBA disagree')
            images[capture['id']] = decoded
            errors.extend(check_regions(expected, raw))
            if 'compare_to' in expected:
                reference = images[expected['compare_to']]
                require(reference[:2] == decoded[:2], 'Cross-capture comparison dimensions differ')
                tolerance = 3 if case['id'] == 'lut3d' else 2
                if any(abs(a - b) > tolerance for index, (a, b) in enumerate(zip(raw, reference[2])) if index % 4 != 3):
                    errors.append(f"{capture['id']} differs from {expected['compare_to']} beyond {tolerance}")
        errors.extend(validate_font_references(manifest, case, output, resources, images, used))
        declared = {identity[1] for identity in used if identity[0] == 'path'}
        permitted_logs = {'stdout.log', 'stderr.log', 'process.json', 'validation.json'}
        for file in output.rglob('*'):
            if file.is_file():
                require(str(file.resolve()).casefold() in declared or file.parent == output and file.name in permitted_logs,
                        f'Undeclared output file (including unexpected core-failure image): {file.relative_to(output)}')
        for log_name in ('stdout.log', 'stderr.log'):
            log = output / log_name
            if not log.is_file():
                continue  # Standalone validator unit fixtures do not execute a child.
            log = safe_file(output, log_name, used, MAX_LOG_BYTES)
            for line in log.read_text(encoding='utf-8', errors='replace').splitlines():
                lower = line.lower()
                if any(marker in lower for marker in ('invalid handle.', 'already destroyed', 'invalid texture handle')):
                    errors.append(f'{log_name}: unexpected resource diagnostic: {line[:500]}')
                if not faulted and any(marker in lower for marker in ('bgfx_debug_ifh', 'rendering disabled')):
                    errors.append(f'{log_name}: unexpected disabled renderer: {line[:500]}')
    except (ContractError, OSError, KeyError, TypeError, ValueError) as error:
        errors.append(str(error))
    return {'passed': not errors, 'errors': errors, 'verified_capture_ids': list(images)}


def write_json(path, value):
    # New parent metadata is exclusive. Existing metadata is updated only via
    # its pre-child OwnedJsonFile handle, never by reopening a child path.
    with OwnedJsonFile(path) as owned:
        owned.update(value)


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def input_identity(probe, manifest_file, resources, manifest):
    paths = {'probe': probe, 'manifest': manifest_file, 'font': safe_file(resources, manifest['font']['path']),
             'driver': Path(__file__).resolve(), 'process_owner': Path(__file__).with_name('validation_process.py').resolve()}
    for file in sorted(probe.parent.glob('*.dll')):
        paths['dependency:' + file.name] = file
    for relative in ('src/render', 'src/platform', 'src/entry', 'tests/probes'):
        folder = resources / relative
        if folder.is_dir():
            for file in sorted(folder.rglob('*')):
                if file.is_file() and file.suffix in ('.cpp', '.h'):
                    paths['source:' + file.relative_to(resources).as_posix()] = file
    return {key: {'path': str(file.resolve()), 'sha256': sha256(file)} for key, file in paths.items()}


def run_suite(probe, manifest_file, resources, output, backends=None, cases=None):
    probe, manifest_file, resources = Path(probe).resolve(strict=True), Path(manifest_file).resolve(strict=True), Path(resources).resolve(strict=True)
    output = Path(os.path.abspath(output))
    require(probe.is_file() and probe.suffix.lower() not in ('.bat', '.cmd') and resources.is_dir(), 'Invalid probe/resources')
    require(not os.path.lexists(output), 'Output already exists; no overwrite/reuse')
    # Canonicalize caller-chosen ancestors once, before creating the run. Every
    # created directory's lexical chain is subsequently pinned and rechecked.
    output = output.resolve()
    require(output != resources and not resources.is_relative_to(output), 'Output would overlap the resource root')
    for relative in ('src', 'scripts', 'assets', 'tests/projects/render_contracts'):
        require(not output.is_relative_to(resources / relative), 'Output must not write inside source/fixtures')
    manifest = load_manifest(manifest_file)
    require(sha256(safe_file(resources, manifest['font']['path'])) == manifest['font']['sha256'], 'Actual font differs from manifest')
    selected_backends = list(BACKENDS) if backends is None else list(backends)
    selected_cases = list(CASE_IDS) if cases is None else list(cases)
    require(selected_backends and len(set(selected_backends)) == len(selected_backends) and all(value in BACKENDS for value in selected_backends), 'Unknown/duplicate backend selection')
    require(selected_cases and len(set(selected_cases)) == len(selected_cases) and all(value in CASE_IDS for value in selected_cases), 'Unknown/duplicate case selection')
    # A caller spelling a subset flag remains diagnostic, even when it happens
    # to enumerate the full set. Only the default invocation is the full gate.
    complete = backends is None and cases is None
    before = input_identity(probe, manifest_file, resources, manifest)
    output.mkdir(parents=True, exist_ok=False)
    run_boundary = DirectoryBoundary(output)
    boundaries = {}
    # Create and pin the entire selected directory tree before any child runs;
    # a prior child cannot redirect a later mkdir through a replaced ancestor.
    for backend in selected_backends:
        run_boundary.verify()
        backend_output = output / backend
        backend_output.mkdir()
        backend_boundary = DirectoryBoundary(output, backend_output)
        for case_id in selected_cases:
            backend_boundary.verify()
            case_output = backend_output / case_id
            case_output.mkdir()
            boundaries[backend, case_id] = DirectoryBoundary(output, case_output)
    report = {'schema_version': 1, 'suite_id': SUITE_ID, 'status': 'RUNNING', 'scope_complete': complete,
              'selected_backends': selected_backends, 'selected_cases': selected_cases, 'started_utc': utc_now(),
              'identity_before': before, 'cases': [], 'retries': 0,
              'canonical_manifest_sha256': CANONICAL_MANIFEST_SHA256,
              'failure_report_binding': 'Original exclusive run.json file handle; never reopen through child-controlled paths',
              'identity_scope': 'Observed source/binary/dependency bytes; source-to-binary build correlation belongs to the integration build receipt'}
    with OwnedJsonFile(output / 'run.json') as run_record:
        run_boundary.pin_parent_file('run.json', run_record.stream)
        run_record.update(report)
        process = None
        try:
            for backend in selected_backends:
                for case_id in selected_cases:
                    run_boundary.verify()
                    case = next(item for item in manifest['cases'] if item['id'] == case_id)
                    case_output = output / backend / case_id
                    boundary = boundaries[backend, case_id]
                    boundary.verify()
                    if any(case_output.iterdir()):
                        raise OutputBoundaryError(f'Pre-created case directory was populated before its child: {case_output}')
                    command = [str(probe), '--backend', backend, '--case', case_id, '--manifest', str(manifest_file),
                               '--resource-root', str(resources), '--output-dir', str(case_output)]
                    process = {'backend': backend, 'case_id': case_id, 'command': command, 'cwd': str(resources),
                               'started_utc': utc_now(), 'timeout_seconds': 60, 'exit_code': None, 'timed_out': False}
                    started = time.monotonic()
                    with OwnedJsonFile(case_output / 'process.json') as process_record:
                        boundary.pin_parent_file('process.json', process_record.stream)
                        process_record.update(process)
                        with (case_output / 'stdout.log').open('xb') as stdout, (case_output / 'stderr.log').open('xb') as stderr:
                            boundary.pin_parent_file('stdout.log', stdout)
                            boundary.pin_parent_file('stderr.log', stderr)
                            try:
                                process['exit_code'] = run_owned_command(command, cwd=resources, stdout=stdout, stderr=stderr, timeout=60)
                            except subprocess.TimeoutExpired:
                                process['timed_out'] = True
                            except OSError as error:
                                process['launch_error'] = str(error)
                        process['elapsed_seconds'] = time.monotonic() - started
                        process['finished_utc'] = utc_now()
                        # No result/log reads or path-based writes occur before
                        # verifying the original run/case chain and owned leaves.
                        run_boundary.verify()
                        boundary.verify(contents=True)
                        with OwnedJsonFile(case_output / 'validation.json') as validation_record:
                            boundary.pin_parent_file('validation.json', validation_record.stream)
                            validation = validate_result(manifest, case, case_output, probe, resources, backend, boundary)
                            if process['exit_code'] != 0 or process['timed_out']:
                                validation['passed'] = False
                                validation['errors'].append(f"Owned process exit={process['exit_code']}, timeout={process['timed_out']}")
                            process['status'] = 'PASS' if validation['passed'] else 'FAIL'
                            run_boundary.verify()
                            boundary.verify(contents=True)
                            try:
                                raw_result = read_json(safe_file(case_output, 'result.json'))
                                process['probe_pid'] = raw_result.get('pid')
                                process['probe_status'] = raw_result.get('status')
                            except ContractError:
                                pass
                            boundary.verify(contents=True)
                            validation_record.update(validation)
                            process_record.update(process)
                    report['cases'].append(process)
                    run_boundary.verify()
                    run_record.update(report)
                    process = None
            run_boundary.verify()
            after = input_identity(probe, manifest_file, resources, manifest)
            report['identity_after'] = after
            report['inputs_stable'] = before == after
            passed = report['inputs_stable'] and all(item['status'] == 'PASS' for item in report['cases'])
            report['status'] = ('PASS' if complete else 'PARTIAL') if passed else 'FAIL'
        except OutputBoundaryError as error:
            report['status'] = 'FAIL'
            report['output_boundary_error'] = str(error)
            if process is not None:
                process['status'] = 'FAIL'
                process['output_boundary_error'] = str(error)
                report['cases'].append(process)
            # Only the already-open original run report receives this failure.
            # Never clean up, traverse, or create metadata in the replaced path.
        except KeyboardInterrupt:
            report['status'] = 'INTERRUPTED'
        except Exception as error:
            report['status'] = 'RUNNER_ERROR'; report['error'] = str(error)
        report['scope_complete'] = complete and len(report['cases']) == len(BACKENDS) * len(CASE_IDS)
        report['finished_utc'] = utc_now()
        run_record.update(report)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', required=True, type=Path)
    parser.add_argument('--manifest', required=True, type=Path)
    parser.add_argument('--resource-root', required=True, type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--backend', choices=BACKENDS, action='append', help='Diagnostic subset; cannot produce full PASS')
    parser.add_argument('--case', choices=CASE_IDS, action='append', help='Diagnostic subset; cannot produce full PASS')
    args = parser.parse_args(argv)
    try:
        result = run_suite(args.probe, args.manifest, args.resource_root, args.output_dir, args.backend, args.case)
    except (ContractError, OSError) as error:
        print(f'render-contract preflight failed: {error}', file=sys.stderr)
        return 2
    print(json.dumps({'status': result['status'], 'scope_complete': result['scope_complete'], 'report': str(args.output_dir.absolute() / 'run.json'),
                      'report_binding': result['failure_report_binding']}, ensure_ascii=False))
    return {'PASS': 0, 'PARTIAL': 2, 'INTERRUPTED': 130}.get(result['status'], 1)


if __name__ == '__main__':
    raise SystemExit(main())
