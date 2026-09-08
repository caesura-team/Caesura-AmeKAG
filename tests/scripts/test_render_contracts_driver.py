"""Synthetic receipt/pixel tests only: no GPU, FreeType, Node or probe execution."""
from __future__ import annotations

import copy
import hashlib
import json
import math
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import verify_render_contracts as driver


def digest(data):
    return hashlib.sha256(data).hexdigest()


def chunk(kind, body):
    return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)


def directory_link(link, target):
    """Actual temporary junction on Windows, symlink elsewhere; no user paths."""
    if os.name != 'nt':
        link.symlink_to(target, target_is_directory=True)
        return
    import ctypes
    from ctypes import wintypes
    link.mkdir()
    substitute = ('\\??\\' + str(target.resolve())).encode('utf-16-le')
    display = str(target.resolve()).encode('utf-16-le')
    data = struct.pack('<HHHH', 0, len(substitute), len(substitute) + 2, len(display)) + substitute + b'\0\0' + display + b'\0\0'
    buffer = ctypes.create_string_buffer(struct.pack('<IHH', 0xA0000003, len(data), 0) + data)
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.DeviceIoControl.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    kernel.DeviceIoControl.restype = wintypes.BOOL
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel.CreateFileW(str(link), 0x40000000, 7, None, 3, 0x02200000, None)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        returned = wintypes.DWORD()
        if not kernel.DeviceIoControl(handle, 0x000900A4, buffer, len(buffer) - 1, None, 0, ctypes.byref(returned), None):
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        kernel.CloseHandle(handle)


def remove_directory_link(link):
    """Remove only the link itself; never resolve/rmtree its external target."""
    if not os.path.lexists(link):
        return
    info = link.lstat()
    if stat.S_ISLNK(info.st_mode):
        link.unlink()
    elif os.name == 'nt' and info.st_file_attributes & 0x400:
        os.rmdir(link)
    else:
        raise AssertionError('Refusing to remove a non-link directory')


def png(width, height, rgba, filters=None, interlace=0):
    """Encode tiny synthetic RGBA fixtures; never a screenshot producer."""
    rows = []
    stride = width * 4
    previous = bytes(stride)
    for y in range(height):
        row = rgba[y * stride:(y + 1) * stride]
        filt = filters[y] if filters else 0
        encoded = bytearray()
        for x, value in enumerate(row):
            a, b, c = (row[x - 4] if x >= 4 else 0), previous[x], (previous[x - 4] if x >= 4 else 0)
            if filt == 0: predictor = 0
            elif filt == 1: predictor = a
            elif filt == 2: predictor = b
            elif filt == 3: predictor = (a + b) // 2
            elif filt == 4:
                distances = [abs(b - c), abs(a - c), abs(a + b - 2 * c)]
                predictor = [a, b, c][distances.index(min(distances))]
            else: predictor = 0
            encoded.append((value - predictor) % 256)
        rows.append(bytes([filt]) + encoded)
        previous = row
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, interlace))
            + chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b''))


class SyntheticFixture:
    def __init__(self, root, case_id='shader-optional-transition', backend='dx11'):
        self.root = root
        self.resources = root / 'synthetic-resources'
        self.resources.mkdir()
        self.manifest = copy.deepcopy(json.loads((ROOT / 'tests/projects/render_contracts/manifest.json').read_text(encoding='utf-8')))
        font = self.resources / self.manifest['font']['path']
        font.parent.mkdir(parents=True)
        font.write_bytes(b'SYNTHETIC FONT IDENTITY ONLY - NOT A FONT OR GPU EVIDENCE')
        self.manifest['font']['sha256'] = digest(font.read_bytes())
        self.manifest_file = self.resources / 'manifest.json'
        self.manifest_file.write_text(json.dumps(self.manifest), encoding='utf-8')
        self.case = next(case for case in self.manifest['cases'] if case['id'] == case_id)
        self.probe = root / 'synthetic-probe.exe'
        self.probe.write_bytes(b'NOT EXECUTABLE; run_owned_command is mocked by these unit tests')
        self.output = root / 'synthetic-case-output'
        self.output.mkdir()
        self.backend = backend

    def canonical_inputs(self):
        """Orchestration uses the real frozen manifest/font, still no GPU/FT."""
        case_id = self.case['id']
        self.manifest = copy.deepcopy(json.loads((ROOT / 'tests/projects/render_contracts/manifest.json').read_text(encoding='utf-8')))
        self.case = next(case for case in self.manifest['cases'] if case['id'] == case_id)
        self.manifest_file.write_text(json.dumps(self.manifest, ensure_ascii=False), encoding='utf-8')
        (self.resources / self.manifest['font']['path']).write_bytes((ROOT / self.manifest['font']['path']).read_bytes())
        return self

    def receipt(self):
        core_failure = self.case['expected'] == 'core_failure'
        return {
            'schema_version': 1, 'suite_id': self.manifest['suite_id'], 'case_id': self.case['id'],
            'requested_backend': self.backend, 'actual_backend': self.backend,
            'backend_name': 'Synthetic receipt; no actual GPU', 'binary_path': str(self.probe.resolve()), 'pid': 12345,
            'expected': self.case['expected'], 'status': 'EXPECTED_FAILURE_OBSERVED' if core_failure else 'PASS',
            'shutdown_completed': True,
            'shader_health': {'core_ready': not core_failure, 'rendering_disabled': core_failure, 'ifh': core_failure,
                'fault_requested': self.case['fault_requested'], 'fault_applied_count': int(self.case['fault_requested'] != 'none'),
                'failed_programs': self.case['failed_programs']},
            'checks': [{'id': key, 'passed': True, 'detail': {}} for key in self.case['required_checks']], 'captures': [],
        }

    def write(self, receipt=None):
        receipt = receipt or self.receipt()
        for index, expected in enumerate(self.case['captures']):
            width, height = expected['width'], expected['height']
            rgba = bytearray(bytes(expected['background']) * (width * height))
            for region in expected['regions']:
                if region['expect']['kind'] != 'rgb':
                    raise AssertionError('This synthetic success fixture deliberately uses only literal-color cases')
                color = bytes(region['expect']['value'] + [255])
                left, top, right, bottom = region['rect']
                for y in range(top, bottom):
                    rgba[(y * width + left) * 4:(y * width + right) * 4] = color * (right - left)
            name = expected['id']
            encoded = png(width, height, rgba)
            (self.output / f'{name}.png').write_bytes(encoded)
            (self.output / f'{name}.rgba').write_bytes(rgba)
            receipt['captures'].append({'id': name, 'png': f'{name}.png', 'rgba': f'{name}.rgba',
                'width': width, 'height': height, 'png_sha256': digest(encoded), 'rgba_sha256': digest(rgba),
                'ticket': {'request_id': index + 1, 'generation': 7, 'frame_id': index + 1, 'status': 'Completed'}})
        self.save_receipt(receipt)
        return receipt

    def save_receipt(self, receipt):
        self.write_receipt_at(self.output, receipt)

    def write_receipt_at(self, output, receipt=None):
        (output / 'result.json').write_text(json.dumps(receipt or self.receipt()), encoding='utf-8')
        (output / 'checkpoint.json').write_text(json.dumps({
            'phase': 'after-shutdown', 'owner_advances': max(1, len(self.case['captures'])),
        }), encoding='utf-8')

    def write_synthetic_font_case(self):
        """Deliberately artificial glyph masks/metrics test the validator only."""
        receipt = self.receipt()
        references = []
        for index, capture in enumerate(self.case['captures']):
            width, height = capture['width'], capture['height']
            contract = capture['font']
            mask, glyphs, origins = bytearray(width * height), [], []
            widths = [sum(8 if ch == ' ' else 14 for ch in run['text']) for run in contract['runs']]
            for run_id, run in enumerate(contract['runs']):
                scale = run['scale']
                if 'center_over' in run:
                    base = run['center_over']
                    origin = origins[base] + (widths[base] * contract['runs'][base]['scale'] - widths[run_id] * scale) / 2
                else:
                    origin = run['x']
                origins.append(origin); pen = origin
                for char in run['text']:
                    space = char == ' '
                    w, h, advance = (0, 0, 8) if space else (8, 12, 14)
                    baseline = run['y'] + 28 * scale
                    top = baseline - 12 * scale
                    glyphs.append({'run': run_id, 'codepoint': ord(char), 'glyph_index': 1 if space else 2,
                        'advance_x': advance, 'bitmap_left': 0, 'bitmap_top': 12, 'bitmap_width': w, 'bitmap_height': h,
                        'pen_x': pen, 'baseline': baseline, 'left': pen, 'top': top, 'width': w * scale, 'height': h * scale})
                    if not space:
                        for y in range(math.floor(top), math.ceil(top + h * scale)):
                            for x in range(math.floor(pen), math.ceil(pen + w * scale)):
                                mask[y * width + x] = 128 if y == math.floor(top) else 255
                    pen += advance * scale
            raw = bytearray(bytes(capture['background']) * (width * height))
            foreground = contract['color']
            for position, coverage in enumerate(mask):
                if not coverage: continue
                alpha = foreground[3] * coverage / 65025
                raw[position * 4:position * 4 + 3] = bytes(math.floor(foreground[ch] * alpha + capture['background'][ch] * (1 - alpha) + .5) for ch in range(3))
            name = capture['id']; encoded = png(width, height, raw)
            (self.output / f'{name}.png').write_bytes(encoded); (self.output / f'{name}.rgba').write_bytes(raw)
            (self.output / f'{name}.coverage').write_bytes(mask)
            receipt['captures'].append({'id':name,'png':f'{name}.png','rgba':f'{name}.rgba','width':width,'height':height,
                'png_sha256':digest(encoded),'rgba_sha256':digest(raw),
                'ticket':{'request_id':index+1,'generation':7,'frame_id':index+1,'status':'Completed'}})
            references.append({'capture_id':name,'width':width,'height':height,'coverage':f'{name}.coverage',
                'coverage_sha256':digest(mask),'opaque_pixels':mask.count(255),'edge_pixels':sum(0 < value < 255 for value in mask),'glyphs':glyphs})
        font = self.manifest['font']
        font_report = {'schema_version':1,'font_path':font['path'],'font_sha256':font['sha256'],'freetype_version':'2.0.0',
            'pixel_size':28,'load_flags':font['load_flags'],'render_mode':font['render_mode'],'sampling':font['sampling'],
            'ascender':28,'references':references}
        (self.output / 'font-reference.json').write_text(json.dumps(font_report), encoding='utf-8')
        self.save_receipt(receipt)
        return receipt, font_report

    def validate(self):
        return driver.validate_result(self.manifest, self.case, self.output, self.probe, self.resources, self.backend)


class RenderContractDriverTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='caesura-synthetic-render-contract-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def fixture(self, case_id='shader-optional-transition'):
        return SyntheticFixture(self.root, case_id)

    def test_frozen_manifest_has_exact_nine_cases_and_two_backends(self):
        manifest = driver.load_manifest(ROOT / 'tests/projects/render_contracts/manifest.json')
        self.assertEqual([case['id'] for case in manifest['cases']], list(driver.CASE_IDS))
        self.assertEqual(manifest['required_backends'], ['dx11', 'opengl'])
        self.assertEqual(sum(len(case['captures']) for case in manifest['cases']), 35)

    def test_manifest_rejects_missing_cases_extra_backend_and_relaxed_threshold(self):
        fixture = self.fixture()
        for edit in (lambda value: value['cases'].pop(), lambda value: value['required_backends'].append('noop'),
                     lambda value: value['font'].__setitem__('opaque_tolerance', 99),
                     lambda value: value['cases'][0]['required_checks'].pop()):
            value = copy.deepcopy(fixture.manifest)
            edit(value)
            fixture.manifest_file.write_text(json.dumps(value), encoding='utf-8')
            with self.assertRaises(driver.ContractError): driver.load_manifest(fixture.manifest_file)

    def test_duplicate_json_keys_are_rejected(self):
        path = self.root / 'duplicate.json'
        path.write_text('{"schema_version":1,"schema_version":1}', encoding='utf-8')
        with self.assertRaises(driver.ContractError): driver.load_manifest(path)

    def test_png_filters_zero_through_four_decode_exactly(self):
        data = bytes([12, 31, 250, 255, 94, 17, 2, 128, 1, 240, 100, 0,
                      77, 19, 33, 200, 2, 253, 60, 255, 21, 39, 79, 64])
        for filt in range(5):
            with self.subTest(filter=filt):
                self.assertEqual(driver.decode_png(png(3, 2, data, [filt, filt])), (3, 2, data))

    def test_png_crc_truncation_trailing_bytes_and_interlace_reject(self):
        good = png(1, 1, b'\x01\x02\x03\xff')
        broken = bytearray(good); broken[30] ^= 1
        for value in (bytes(broken), good[:-3], good + b'old data', png(1, 1, b'\x01\x02\x03\xff', interlace=1)):
            with self.assertRaises(driver.ContractError): driver.decode_png(value)

    def test_png_rejects_inflation_larger_than_declared_geometry(self):
        encoded = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 6, 0, 0, 0))
                   + chunk(b'IDAT', zlib.compress(bytes(10000))) + chunk(b'IEND', b''))
        with self.assertRaises(driver.ContractError): driver.decode_png(encoded)

    def test_reference_math_is_not_derived_from_candidate_pixels(self):
        self.assertEqual(driver.expected_rgb({'kind':'alpha_over','src':[234,170,106],'dst':[18,35,52],'texture_alpha':255,'opacity':128}), [126,103,79])
        self.assertEqual(driver.expected_rgb({'kind':'lerp','from':[204,51,17],'to':[17,102,221],'progress':.5}), [111,77,119])
        self.assertEqual(driver.expected_rgb({'kind':'lut','input':[51,102,204],'transform':'swap_rb','strength':.5}), [128,102,128])

    def test_synthetic_optional_failure_validates_without_claiming_real_gpu(self):
        fixture = self.fixture(); fixture.write()
        result = fixture.validate()
        self.assertTrue(result['passed'], result['errors'])

    def test_checkpoint_is_required(self):
        fixture = self.fixture(); fixture.write()
        (fixture.output / 'checkpoint.json').unlink()
        result = fixture.validate()
        self.assertFalse(result['passed'])
        self.assertTrue(any('checkpoint.json' in error for error in result['errors']))

    def test_checkpoint_must_be_after_shutdown(self):
        fixture = self.fixture(); fixture.write()
        (fixture.output / 'checkpoint.json').write_text(json.dumps({'phase':'before-shutdown','owner_advances':2}), encoding='utf-8')
        self.assertFalse(fixture.validate()['passed'])

    def test_checkpoint_rejects_missing_and_extra_keys(self):
        fixture = self.fixture(); fixture.write()
        for value in ({'phase':'after-shutdown'}, {'phase':'after-shutdown','owner_advances':2,'ignored':True}):
            (fixture.output / 'checkpoint.json').write_text(json.dumps(value), encoding='utf-8')
            self.assertFalse(fixture.validate()['passed'])

    def test_checkpoint_advances_are_bounded_integers_covering_all_captures(self):
        fixture = self.fixture(); fixture.write()
        for advances in (0, 1, -1, 129, 2.5, True, '2', None):
            with self.subTest(advances=advances):
                (fixture.output / 'checkpoint.json').write_text(json.dumps({'phase':'after-shutdown','owner_advances':advances}), encoding='utf-8')
                self.assertFalse(fixture.validate()['passed'])
        (fixture.output / 'checkpoint.json').write_text(json.dumps({'phase':'after-shutdown','owner_advances':128}), encoding='utf-8')
        self.assertTrue(fixture.validate()['passed'])

    def test_core_failure_checkpoint_still_requires_one_owner_advance(self):
        fixture = self.fixture('shader-core-blend'); fixture.write()
        self.assertTrue(fixture.validate()['passed'])
        (fixture.output / 'checkpoint.json').write_text(json.dumps({'phase':'after-shutdown','owner_advances':0}), encoding='utf-8')
        self.assertFalse(fixture.validate()['passed'])

    def test_checkpoint_is_limited_to_sixteen_kib(self):
        fixture = self.fixture(); fixture.write()
        path = fixture.output / 'checkpoint.json'
        path.write_bytes(path.read_bytes() + b' ' * (16 * 1024))
        result = fixture.validate()
        self.assertFalse(result['passed'])
        self.assertTrue(any('checkpoint.json' in error for error in result['errors']))

    def test_pass_receipt_cannot_include_exception_diagnostic(self):
        fixture = self.fixture(); fixture.write()
        (fixture.output / 'diagnostic.json').write_text(json.dumps({'error':'synthetic probe exception'}), encoding='utf-8')
        result = fixture.validate()
        self.assertFalse(result['passed'])
        self.assertTrue(any('diagnostic.json' in error for error in result['errors']))

    def test_core_failure_requires_exact_fault_and_no_image(self):
        fixture = self.fixture('shader-core-blend'); receipt = fixture.write()
        self.assertTrue(fixture.validate()['passed'])
        receipt['shader_health']['fault_applied_count'] = 0; fixture.save_receipt(receipt)
        self.assertFalse(fixture.validate()['passed'])

    def test_noop_wrong_backend_and_positive_ifh_never_pass(self):
        fixture = self.fixture(); original = fixture.write()
        for edit in (lambda value: value.__setitem__('actual_backend', 'noop'),
                     lambda value: value.__setitem__('actual_backend', 'opengl'),
                     lambda value: value['shader_health'].__setitem__('ifh', True),
                     lambda value: value['shader_health'].__setitem__('core_ready', False)):
            receipt = copy.deepcopy(original); edit(receipt); fixture.save_receipt(receipt)
            self.assertFalse(fixture.validate()['passed'])

    def test_missing_duplicate_false_checks_and_wrong_binary_reject(self):
        fixture = self.fixture(); original = fixture.write()
        for edit in (lambda value: value['checks'].pop(), lambda value: value['checks'].append(value['checks'][0]),
                     lambda value: value['checks'][0].__setitem__('passed', False),
                     lambda value: value.__setitem__('binary_path', str(self.root / 'different.exe')),
                     lambda value: value.__setitem__('shutdown_completed', False)):
            receipt = copy.deepcopy(original); edit(receipt); fixture.save_receipt(receipt)
            self.assertFalse(fixture.validate()['passed'])

    def test_check_event_order_does_not_change_required_set(self):
        fixture = self.fixture(); receipt = fixture.write()
        receipt['checks'].reverse(); fixture.save_receipt(receipt)
        self.assertTrue(fixture.validate()['passed'])

    def test_pending_duplicate_and_backwards_ticket_identity_reject(self):
        fixture = self.fixture(); original = fixture.write()
        for edit in (lambda value: value['captures'][0]['ticket'].__setitem__('status','Pending'),
                     lambda value: value['captures'][1]['ticket'].__setitem__('request_id',1),
                     lambda value: value['captures'][1]['ticket'].__setitem__('frame_id',1),
                     lambda value: value['captures'][1]['ticket'].__setitem__('generation',8)):
            receipt = copy.deepcopy(original); edit(receipt); fixture.save_receipt(receipt)
            self.assertFalse(fixture.validate()['passed'])

    def test_malformed_trailing_capture_is_not_ignored(self):
        fixture = self.fixture(); receipt = fixture.write()
        receipt['captures'].append(None); fixture.save_receipt(receipt)
        self.assertFalse(fixture.validate()['passed'])

    def test_capture_path_escape_absolute_path_and_alias_reject(self):
        fixture = self.fixture(); original = fixture.write()
        for escaped in ('../baseline.png', str(fixture.output / 'baseline.png'), 'sub/../baseline.png', 'C:baseline.png'):
            receipt = copy.deepcopy(original); receipt['captures'][0]['png'] = escaped; fixture.save_receipt(receipt)
            self.assertFalse(fixture.validate()['passed'])
        receipt = copy.deepcopy(original); receipt['captures'][1]['png'] = receipt['captures'][0]['png']; fixture.save_receipt(receipt)
        self.assertFalse(fixture.validate()['passed'])

    def test_raw_png_disagreement_and_false_hash_reject(self):
        fixture = self.fixture(); receipt = fixture.write()
        path = fixture.output / receipt['captures'][0]['rgba']
        data = bytearray(path.read_bytes()); data[0] ^= 1; path.write_bytes(data)
        receipt['captures'][0]['rgba_sha256'] = digest(data); fixture.save_receipt(receipt)
        self.assertFalse(fixture.validate()['passed'])
        receipt['captures'][0]['png_sha256'] = '0' * 64; fixture.save_receipt(receipt)
        self.assertFalse(fixture.validate()['passed'])

    def test_candidate_with_matching_hashes_but_wrong_pixels_rejects(self):
        fixture = self.fixture(); receipt = fixture.write()
        capture = receipt['captures'][0]
        data = bytearray((fixture.output / capture['rgba']).read_bytes())
        offset = (88 * 640 + 112) * 4; data[offset:offset + 3] = b'\0\0\0'
        encoded = png(640, 360, data)
        (fixture.output / capture['rgba']).write_bytes(data); (fixture.output / capture['png']).write_bytes(encoded)
        capture['png_sha256'], capture['rgba_sha256'] = digest(encoded), digest(data); fixture.save_receipt(receipt)
        self.assertFalse(fixture.validate()['passed'])

    def test_stale_font_reference_cannot_validate(self):
        fixture = self.fixture('text-cjk-ruby')
        (fixture.output / 'font-reference.json').write_text(json.dumps({'schema_version':1,'font_sha256':'0'*64}), encoding='utf-8')
        with self.assertRaises(driver.ContractError):
            driver.validate_font_references(fixture.manifest, fixture.case, fixture.output, fixture.resources, {}, set())

    def test_synthetic_font_equations_and_masks_have_a_positive_control(self):
        fixture = self.fixture('text-cjk-ruby'); fixture.write_synthetic_font_case()
        result = fixture.validate()
        self.assertTrue(result['passed'], result['errors'])

    def test_font_missing_glyph_bad_center_and_wrong_flags_reject(self):
        fixture = self.fixture('text-cjk-ruby'); _, original = fixture.write_synthetic_font_case()
        for edit in (lambda value: value['references'][0]['glyphs'].pop(),
                     lambda value: value['references'][2]['glyphs'][2].__setitem__('pen_x',100),
                     lambda value: value.__setitem__('load_flags','FT_LOAD_NO_HINTING'),
                     lambda value: value['references'][0]['glyphs'][0].__setitem__('glyph_index',0)):
            report = copy.deepcopy(original); edit(report)
            (fixture.output / 'font-reference.json').write_text(json.dumps(report), encoding='utf-8')
            self.assertFalse(fixture.validate()['passed'])

    def test_font_empty_mask_and_outside_roi_cannot_pass_by_updating_hash(self):
        fixture = self.fixture('text-cjk-ruby'); _, original = fixture.write_synthetic_font_case()
        path = fixture.output / original['references'][0]['coverage']; old = path.read_bytes()
        for data in (bytes(len(old)), bytes([255]) + old[1:]):
            report = copy.deepcopy(original); reference = report['references'][0]
            path.write_bytes(data); reference['coverage_sha256'] = digest(data)
            reference['opaque_pixels'] = data.count(255); reference['edge_pixels'] = sum(0 < value < 255 for value in data)
            (fixture.output / 'font-reference.json').write_text(json.dumps(report), encoding='utf-8')
            self.assertFalse(fixture.validate()['passed'])

    def test_opaque_missing_pixels_fail_even_when_png_raw_hashes_match(self):
        fixture = self.fixture('text-cjk-ruby'); receipt, font = fixture.write_synthetic_font_case()
        mask = (fixture.output / font['references'][0]['coverage']).read_bytes()
        index = mask.index(255); capture = receipt['captures'][0]
        raw = bytearray((fixture.output / capture['rgba']).read_bytes())
        raw[index*4:index*4+3] = bytes(fixture.case['captures'][0]['background'][:3])
        encoded = png(capture['width'],capture['height'],raw)
        (fixture.output / capture['rgba']).write_bytes(raw); (fixture.output / capture['png']).write_bytes(encoded)
        capture['rgba_sha256'],capture['png_sha256'] = digest(raw),digest(encoded);fixture.save_receipt(receipt)
        self.assertFalse(fixture.validate()['passed'])

    def test_core_failure_cannot_hide_a_produced_png(self):
        fixture = self.fixture('shader-core-blend'); fixture.write()
        (fixture.output / 'unexpected.png').write_bytes(png(1,1,b'\0\0\0\xff'))
        self.assertFalse(fixture.validate()['passed'])

    def test_resource_error_logs_override_self_reported_success(self):
        fixture = self.fixture(); fixture.write()
        (fixture.output / 'stderr.log').write_text('Invalid texture handle', encoding='utf-8')
        self.assertFalse(fixture.validate()['passed'])

    def test_default_driver_attempts_exactly_eighteen_processes_without_retry(self):
        fixture = self.fixture().canonical_inputs(); output = self.root / 'suite'
        with mock.patch.object(driver, 'run_owned_command', return_value=23) as owned:
            result = driver.run_suite(fixture.probe, fixture.manifest_file, fixture.resources, output)
        self.assertEqual(owned.call_count, 18)
        self.assertTrue(result['scope_complete'])
        self.assertEqual(result['status'], 'FAIL')
        self.assertEqual([call.args[0][2] for call in owned.call_args_list], ['dx11'] * 9 + ['opengl'] * 9)
        self.assertTrue(all(call.kwargs['timeout'] == 60 for call in owned.call_args_list))

    def test_subset_is_partial_even_with_a_valid_expected_failure(self):
        fixture = self.fixture('shader-core-blend').canonical_inputs()
        def write_negative(argv, **kwargs):
            output = Path(argv[argv.index('--output-dir') + 1])
            receipt = fixture.receipt()
            fixture.write_receipt_at(output, receipt)
            return 0
        with mock.patch.object(driver, 'run_owned_command', side_effect=write_negative) as owned:
            result = driver.run_suite(fixture.probe, fixture.manifest_file, fixture.resources, self.root / 'subset', ['dx11'], ['shader-core-blend'])
        self.assertEqual(owned.call_count, 1)
        self.assertFalse(result['scope_complete'])
        self.assertEqual(result['status'], 'PARTIAL')

    def test_timeout_is_preserved_and_does_not_retry(self):
        fixture = self.fixture('shader-core-blend').canonical_inputs()
        with mock.patch.object(driver,'run_owned_command',side_effect=subprocess.TimeoutExpired(['synthetic'],60)) as owned:
            result = driver.run_suite(fixture.probe,fixture.manifest_file,fixture.resources,self.root/'timeout',['dx11'],['shader-core-blend'])
        self.assertEqual(owned.call_count,1); self.assertEqual(result['status'],'FAIL')
        self.assertTrue(result['cases'][0]['timed_out'])

    def test_changed_input_identity_prevents_pass(self):
        fixture = self.fixture('shader-core-blend').canonical_inputs()
        def run_and_change(argv,**kwargs):
            destination = Path(argv[argv.index('--output-dir')+1])
            fixture.write_receipt_at(destination)
            (fixture.resources/fixture.manifest['font']['path']).write_bytes(b'changed during synthetic execution')
            return 0
        with mock.patch.object(driver,'run_owned_command',side_effect=run_and_change):
            result = driver.run_suite(fixture.probe,fixture.manifest_file,fixture.resources,self.root/'changed',['dx11'],['shader-core-blend'])
        self.assertEqual(result['status'],'FAIL'); self.assertFalse(result['inputs_stable'])

    def test_interruption_stops_after_one_owned_command(self):
        fixture = self.fixture().canonical_inputs()
        with mock.patch.object(driver,'run_owned_command',side_effect=KeyboardInterrupt) as owned:
            result = driver.run_suite(fixture.probe,fixture.manifest_file,fixture.resources,self.root/'interrupt')
        self.assertEqual(owned.call_count,1); self.assertEqual(result['status'],'INTERRUPTED')

    def test_existing_output_is_not_overwritten(self):
        fixture = self.fixture(); output = self.root / 'existing'; output.mkdir()
        sentinel = output / 'keep'; sentinel.write_text('old evidence', encoding='utf-8')
        with mock.patch.object(driver, 'run_owned_command') as owned:
            with self.assertRaises(driver.ContractError): driver.run_suite(fixture.probe, fixture.manifest_file, fixture.resources, output)
        owned.assert_not_called(); self.assertEqual(sentinel.read_text(), 'old evidence')

    def test_safe_file_rejects_a_real_rebound_directory_root(self):
        base, outside = self.root / 'base', self.root / 'outside'
        base.mkdir(); outside.mkdir()
        (outside / 'result.json').write_text('{}', encoding='utf-8')
        base.rename(self.root / 'original-base')
        directory_link(base, outside)
        try:
            with self.assertRaises(driver.ContractError): driver.safe_file(base, 'result.json')
        finally:
            remove_directory_link(base)

    def test_case_root_junction_replacement_never_writes_external_sentinels(self):
        fixture = self.fixture('shader-core-blend').canonical_inputs()
        outside = self.root / 'synthetic-external'; outside.mkdir()
        fixture.write_receipt_at(outside)
        for name in ('process.json','validation.json'):
            (outside / name).write_bytes(b'SYNTHETIC EXTERNAL SENTINEL - MUST NOT CHANGE')
        original = {file.name:file.read_bytes() for file in outside.iterdir()}
        links = []
        def replace_root(argv, **kwargs):
            # Close owned synthetic streams to model the post-child cleanup
            # boundary even on Windows; no actual executable is launched.
            kwargs['stdout'].close(); kwargs['stderr'].close()
            case_root = Path(argv[argv.index('--output-dir')+1])
            case_root.rename(case_root.parent / 'original-case')
            directory_link(case_root, outside); links.append(case_root)
            return 0
        try:
            with mock.patch.object(driver,'run_owned_command',side_effect=replace_root):
                report = driver.run_suite(fixture.probe,fixture.manifest_file,fixture.resources,self.root/'run',['dx11'],['shader-core-blend'])
            self.assertEqual({file.name:file.read_bytes() for file in outside.iterdir()}, original)
            self.assertNotIn(report['status'], ('PASS','PARTIAL'))
            self.assertTrue((self.root/'run'/'run.json').is_file())
        finally:
            for link in links: remove_directory_link(link)

    def test_regular_case_directory_replacement_is_also_rejected(self):
        fixture = self.fixture('shader-core-blend').canonical_inputs()
        replaced, attempts = [], []
        def replace_root(argv, **kwargs):
            kwargs['stdout'].close(); kwargs['stderr'].close()
            case_root = Path(argv[argv.index('--output-dir')+1])
            attempts.append((case_root, case_root.stat().st_ino))
            case_root.rename(case_root.parent/'original-case')
            case_root.mkdir(); replaced.append(case_root)
            fixture.write_receipt_at(case_root)
            return 0
        with mock.patch.object(driver,'run_owned_command',side_effect=replace_root):
            report = driver.run_suite(fixture.probe,fixture.manifest_file,fixture.resources,self.root/'run',['dx11'],['shader-core-blend'])
        self.assertNotIn(report['status'],('PASS','PARTIAL'))
        if replaced:
            self.assertEqual({file.name for file in replaced[0].iterdir()}, {'result.json', 'checkpoint.json'})
        else:
            # Windows can prevent rename while the parent metadata inode is
            # held open. That is protection, not a reason to weaken checks.
            self.assertEqual(len(attempts), 1)
            self.assertEqual(attempts[0][0].stat().st_ino, attempts[0][1])
            self.assertTrue(report['cases'][0].get('launch_error'))

    def test_pinned_boundary_rejects_actual_plain_directory_replacement(self):
        root=self.root/'pinned-run'; case=root/'dx11'/'case'; case.mkdir(parents=True)
        boundary=driver.DirectoryBoundary(root,case)
        case.rename(case.parent/'old-case'); case.mkdir()
        # No metadata handle is open here: this proves the identity check
        # itself, in addition to the Windows handle protection above.
        with self.assertRaises(driver.OutputBoundaryError): boundary.verify(contents=True)

    def test_pinned_boundary_rejects_actual_ancestor_junction(self):
        root=self.root/'pinned-run'; case=root/'dx11'/'case'; case.mkdir(parents=True)
        outside=self.root/'synthetic-external'; (outside/'case').mkdir(parents=True)
        victim=outside/'case'/'keep'; victim.write_bytes(b'unchanged outside run')
        boundary=driver.DirectoryBoundary(root,case)
        (root/'dx11').rename(root/'original-backend')
        directory_link(root/'dx11',outside)
        try:
            with self.assertRaises(driver.OutputBoundaryError): boundary.verify(contents=True)
            self.assertEqual(victim.read_bytes(),b'unchanged outside run')
        finally:
            remove_directory_link(root/'dx11')

    def test_reserved_metadata_hardlinks_never_overwrite_external_files(self):
        for leaf in ('process.json','validation.json'):
            with self.subTest(leaf=leaf):
                sub = self.root / leaf; sub.mkdir()
                fixture = SyntheticFixture(sub,'shader-core-blend').canonical_inputs()
                victim = sub/'synthetic-external.json'; victim.write_bytes(b'SYNTHETIC EXTERNAL LEAF')
                def replace_leaf(argv, **kwargs):
                    kwargs['stdout'].close(); kwargs['stderr'].close()
                    case_root=Path(argv[argv.index('--output-dir')+1])
                    fixture.write_receipt_at(case_root)
                    destination=case_root/leaf
                    if destination.exists(): destination.unlink()
                    os.link(victim,destination)
                    return 0
                with mock.patch.object(driver,'run_owned_command',side_effect=replace_leaf):
                    report=driver.run_suite(fixture.probe,fixture.manifest_file,fixture.resources,sub/'run',['dx11'],['shader-core-blend'])
                self.assertEqual(victim.read_bytes(),b'SYNTHETIC EXTERNAL LEAF')
                self.assertNotIn(report['status'],('PASS','PARTIAL'))

    def test_oversized_log_is_rejected_before_content_read(self):
        fixture=self.fixture('shader-core-blend').canonical_inputs()
        reads=[]; original_read=Path.read_text
        def track_read(path,*args,**kwargs):
            if path.name=='stdout.log': reads.append(path)
            return original_read(path,*args,**kwargs)
        def oversized(argv,**kwargs):
            kwargs['stdout'].close();kwargs['stderr'].close()
            case_root=Path(argv[argv.index('--output-dir')+1])
            fixture.write_receipt_at(case_root)
            (case_root/'stdout.log').write_bytes(b'x'*(8*1024*1024+1))
            return 0
        with mock.patch.object(driver,'run_owned_command',side_effect=oversized), mock.patch.object(Path,'read_text',track_read):
            report=driver.run_suite(fixture.probe,fixture.manifest_file,fixture.resources,self.root/'run',['dx11'],['shader-core-blend'])
        self.assertEqual(reads,[])
        self.assertNotIn(report['status'],('PASS','PARTIAL'))

    def test_reserved_log_hardlink_is_not_read(self):
        fixture=self.fixture('shader-core-blend').canonical_inputs()
        victim=self.root/'synthetic-external.log'; victim.write_bytes(b'SYNTHETIC EXTERNAL LOG - DO NOT READ')
        reads=[]; original_read=Path.read_text
        def track_read(path,*args,**kwargs):
            if path.name=='stdout.log': reads.append(path)
            return original_read(path,*args,**kwargs)
        def linked_log(argv,**kwargs):
            kwargs['stdout'].close(); kwargs['stderr'].close()
            case_root=Path(argv[argv.index('--output-dir')+1])
            fixture.write_receipt_at(case_root)
            (case_root/'stdout.log').unlink()
            os.link(victim,case_root/'stdout.log')
            return 0
        with mock.patch.object(driver,'run_owned_command',side_effect=linked_log), mock.patch.object(Path,'read_text',track_read):
            report=driver.run_suite(fixture.probe,fixture.manifest_file,fixture.resources,self.root/'run',['dx11'],['shader-core-blend'])
        self.assertEqual(reads,[])
        self.assertEqual(victim.read_bytes(),b'SYNTHETIC EXTERNAL LOG - DO NOT READ')
        self.assertNotIn(report['status'],('PASS','PARTIAL'))

    def test_cli_contract_rejects_valid_but_changed_lut_recipe(self):
        fixture=self.fixture('shader-core-blend').canonical_inputs()
        lut=next(case for case in fixture.manifest['cases'] if case['id']=='lut3d')
        lut['captures'][1]['parameters']['lut_size']=0
        fixture.manifest_file.write_text(json.dumps(fixture.manifest),encoding='utf-8')
        with mock.patch.object(driver,'run_owned_command') as owned:
            with self.assertRaises(driver.ContractError):
                driver.run_suite(fixture.probe,fixture.manifest_file,fixture.resources,self.root/'run')
        owned.assert_not_called()

    def test_canonical_manifest_identity_accepts_formatting_not_value_changes(self):
        fixture=self.fixture().canonical_inputs()
        fixture.manifest_file.write_text(json.dumps(fixture.manifest,sort_keys=True,ensure_ascii=True,indent=4),encoding='utf-8')
        parsed=driver.load_manifest(fixture.manifest_file)
        self.assertEqual(driver.canonical_manifest_sha256(parsed),'37e8832971dac6a1c440caac54f14e2efebf2c5707b58623dea4c6af1be97095')
        parsed['cases'][0]['captures'][0]['background'][0]+=1
        fixture.manifest_file.write_text(json.dumps(parsed),encoding='utf-8')
        with self.assertRaises(driver.ContractError): driver.load_manifest(fixture.manifest_file)


if __name__ == '__main__':
    unittest.main(verbosity=2)
