"""Observe existing, owned native Live2D evidence; NEVER launch an engine.

Opt-in: --positive-case-dir CASE [--negative-root ROOT]. CASE is produced by
CaesuraCommandContractHost and the command corpus driver, with a licensed Haru
fixture supplied externally, captured at fixed 16 ms steps. Use the reviewed
baseline/load/show/mouth-0/mouth-.8/hide/unload scene at (64,48), scale .5.
The script verifies phase records, not arbitrary frame labels. ROOT contains
live2d-shader-{missing,corrupt} independent process controls. Neither SDK nor
model assets are bundled here. Pillow is needed only for this opt-in PNG read;
the default unittest suite uses pure pixel arrays and needs no Pillow/GPU.
No claim about all models, audio lip sync, or GPU draw counts is made.
"""
import argparse
import hashlib
import json
from pathlib import Path


class EvidenceError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise EvidenceError(message)


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def check_native(host, owned):
    runs = owned.get('runs', [])
    require(owned.get('status') == 'EXECUTION_COMPLETE'
            and len(runs) == 1 and owned.get('inputs_stable') is True, 'incomplete or unstable native run')
    receipt = runs[0]['receipt']
    require(receipt.get('status') == 'EXITED' and receipt.get('actual_exit_code') == 0
            and receipt.get('owned_tree_cleanup') == 'COMPLETE'
            and receipt.get('timed_out') is False and receipt.get('forced_kill') is False
            and receipt.get('launcher_exit_code') == 0 and receipt.get('stop_requested') is False,
            'native process did not finish cleanly')
    require(host.get('actual_backend') == 'Direct3D 11'
            and host.get('requested_backend') == 'dx11', 'wrong backend')
    require(host.get('fixed_step_ms') == 16, 'wrong fixed clock')
    require(host.get('engine_shutdown_returned') is True
            and host.get('script_status') == 'PASS' and host.get('script_complete') is True
            and host.get('script_error_count') == 0 and host.get('render_failed') is False
            and host.get('frame_limit_reached') is False and host.get('actual_exit') == 0
            and host.get('native_quit_observed') is True
            and host.get('error_ui_quit_injected') is False, 'host did not complete')
    require(host.get('pid') == receipt.get('process', {}).get('pid'), 'process mismatch')


def locked_record(record, path):
    content = path.read_bytes()
    require(record.get('sha256') == hashlib.sha256(content).hexdigest()
            and record.get('bytes') == len(content), 'acceptance file digest mismatch')


SAMPLES = {2: 'baseline', 15: 'mouth_closed', 30: 'mouth_open', 47: 'hidden', 55: 'unloaded'}


def check_phases(script, host):
    require(host.get('capture_every_rendered_frame') is True, 'captures not enabled')
    require(script.get('status') == 'PASS' and script.get('command_error') is False,
            'script not accepted')
    rows = script['live2d']['frames']
    phases = {row['frame']: row['phase'] for row in rows}
    require(len(phases) == len(rows), 'duplicate frame phase')
    for frame, phase in SAMPLES.items():
        # PNG export indices are zero-based; driver render-frame records are
        # one-based. Require both neighbors to be within the stable phase so
        # a sample at an event boundary cannot accidentally pass by shifting.
        require(phases.get(frame) == phase and phases.get(frame + 1) == phase
                and host['completed_owner_frames'] > frame,
                'capture phase mismatch')
    calls = script['live2d']['calls']
    methods = [call['method'] for call in calls]
    require(methods == ['live2d_load', 'live2d_show', 'live2d_set_mouth',
                       'live2d_set_mouth', 'live2d_set_voice_lipsync',
                       'live2d_hide', 'live2d_unload'], 'unexpected lifecycle calls')
    handle = calls[0]['result']
    require(type(handle) is int and handle > 0, 'model load failed')
    expected = [[handle, 64, 48, .5], [handle, 0], [handle, .8],
                [handle, False], [handle], [handle]]
    for call, args in zip(calls[1:], expected):
        require(call['args'] == args and call['result'] is True, 'lifecycle arguments failed')
    require(2 < calls[0]['frame'] < calls[1]['frame'] <= calls[2]['frame'] < 15
            < calls[3]['frame'] < 30 < calls[4]['frame'] <= calls[5]['frame'] < 47
            < calls[6]['frame'] < 55, 'sample not inside lifecycle interval')


def check_pixels(images, width, height, rectangle):
    """RGB tuple arrays; synthetic unittest input is not native GPU evidence."""
    require(set(images) == set(SAMPLES), 'missing capture')
    require(all(len(pixels) == width * height for pixels in images.values()), 'wrong image size')
    diffs = {}
    for a, b in ((2, 15), (15, 30), (2, 47), (2, 55)):
        changed = [i for i, (x, y) in enumerate(zip(images[a], images[b])) if x != y]
        if a == 2 and b in (47, 55):
            require(not changed, 'hide/unload left visible pixels')
        else:
            require(bool(changed), 'model or mouth positive control is blank')
            x0, y0, x1, y1 = rectangle
            require(all(x0 <= i % width < x1 and y0 <= i // width < y1 for i in changed),
                    'pixel change outside model rectangle')
        diffs[f'{a}-{b}'] = len(changed)
    return diffs


def observe_positive(case):
    acceptance = read_json(case / 'root-acceptance.json')
    require(acceptance.get('status') == 'PASS_SELECTED_NATIVE_SCENE'
            and acceptance.get('case_id') == 'live2d-lifecycle', 'root acceptance missing')
    owned_path, host_path = case / 'owned-01/report.json', case / 'host-01/host-report.json'
    locked_record(acceptance['outer'], owned_path)
    locked_record(acceptance['host'], host_path)
    owned, host = read_json(owned_path), read_json(host_path)
    check_native(host, owned)
    check_phases(acceptance['script'], host)
    from PIL import Image
    images, captures = {}, {}
    for frame in SAMPLES:
        path = case / f'host-01/png/frame_{frame:05}.png'
        require(path.is_file(), 'missing capture')
        with Image.open(path) as image:
            require(image.size == (1920, 1080), 'wrong capture size')
            # ImagingCore retains its pixels without allocating millions of
            # Python RGB tuples; check_pixels also accepts pure-array controls.
            images[frame] = image.convert('RGB').getdata()
        captures[str(frame)] = hashlib.sha256(path.read_bytes()).hexdigest()
    return {'status': 'PASS_SELECTED_LIVE2D_PIXELS', 'capture_sha256': captures,
            'changed_rgb_pixels': check_pixels(images, 1920, 1080, (64, 48, 704, 408))}


def observe_negative(root):
    accepted = read_json(root / 'acceptance.json')
    require(accepted.get('status') == 'CONTROLS_PASS', 'negative acceptance absent')
    results = []
    for suffix in ('missing', 'corrupt'):
        name = 'live2d-shader-' + suffix
        case = root / name
        check_native(read_json(case / 'host-01/host-report.json'),
                     read_json(case / 'owned-01/report.json'))
        text = (case / 'owned-01/engine/stdout.log').read_text(encoding='utf-8', errors='strict')
        marker = 'LIVE2D_SHADERGUARD_JSON:'
        records = [json.loads(line[len(marker):]) for line in text.splitlines() if line.startswith(marker)]
        require(len(records) == 1, 'missing or duplicate refusal record')
        script = records[0]
        require(script.get('status') == 'PASS' and script.get('case_id') == name,
                'refusal script failed')
        attempts = script.get('attempts', [])
        require(len(attempts) == 2, 'repeat load not observed')
        for index, attempt in enumerate(attempts, 1):
            require(attempt == {'attempt': index, 'models': 0, 'returned_nil': True,
                                'reason': 'Live2D model load failed'}, 'load was not refused')
        matches = [row for row in accepted['cases'] if row['case_id'] == name]
        require(len(matches) == 1 and matches[0]['script'] == script, 'negative receipt differs')
        require(text.count('Fail Compile shader') == 1
                and '[Live2D] Renderer created' not in text and '[Live2D] Model loaded' not in text,
                'shader failure cache or model creation regression')
        results.append({'case_id': name, 'status': 'PASS_LOAD_REFUSAL', 'gpu_draw_count_measured': False})
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--positive-case-dir', type=Path, required=True)
    parser.add_argument('--negative-root', type=Path)
    args = parser.parse_args()
    report = {'positive': observe_positive(args.positive_case_dir),
              'scope': 'Observer of existing evidence only; not an engine execution or all-command proof.'}
    if args.negative_root:
        report['negative'] = observe_negative(args.negative_root)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
