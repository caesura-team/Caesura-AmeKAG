"""Synthetic observer controls only: no engine, GPU, SDK, or Pillow execution."""
import copy
import unittest

from verify_live2d_native_contract import EvidenceError, check_native, check_phases, check_pixels


def native_documents():
    host = {'actual_backend': 'Direct3D 11', 'requested_backend': 'dx11',
            'fixed_step_ms': 16, 'engine_shutdown_returned': True,
            'script_status': 'PASS', 'script_complete': True, 'script_error_count': 0,
            'render_failed': False, 'frame_limit_reached': False, 'pid': 123,
            'actual_exit': 0, 'native_quit_observed': True, 'error_ui_quit_injected': False,
            'capture_every_rendered_frame': True, 'completed_owner_frames': 59}
    owned = {'status': 'EXECUTION_COMPLETE', 'inputs_stable': True, 'runs': [{'receipt': {
        'status': 'EXITED', 'actual_exit_code': 0, 'owned_tree_cleanup': 'COMPLETE',
        'launcher_exit_code': 0, 'stop_requested': False,
        'timed_out': False, 'forced_kill': False, 'process': {'pid': 123}}}]}
    return host, owned


def pixels():
    baseline = [(0, 0, 0)] * 16
    closed, opened = baseline.copy(), baseline.copy()
    closed[5] = (40, 40, 40)
    opened[5] = (80, 40, 40)
    return {2: baseline, 15: closed, 30: opened, 47: baseline.copy(), 55: baseline.copy()}


def phases():
    methods = ['live2d_load', 'live2d_show', 'live2d_set_mouth',
               'live2d_set_mouth', 'live2d_set_voice_lipsync', 'live2d_hide', 'live2d_unload']
    arguments = [['Haru.model3.json', 'Haru'], [1, 64, 48, .5], [1, 0],
                 [1, .8], [1, False], [1], [1]]
    return {'status': 'PASS', 'command_error': False, 'live2d': {
        'frames': [{'frame': n + offset, 'phase': p} for n, p in
                   ((2, 'baseline'), (15, 'mouth_closed'), (30, 'mouth_open'),
                    (47, 'hidden'), (55, 'unloaded')) for offset in (0, 1)],
        'calls': [{'method': m, 'args': a, 'frame': f, 'result': 1 if i == 0 else True}
                  for i, (m, a, f) in enumerate(zip(methods, arguments, (5, 6, 7, 25, 43, 44, 52)))]}}


class ObserverControls(unittest.TestCase):
    def test_native_metadata_positive(self):
        check_native(*native_documents())

    def test_wrong_backend(self):
        host, owned = native_documents()
        host['actual_backend'] = 'Noop'
        with self.assertRaises(EvidenceError):
            check_native(host, owned)

    def test_wrong_clock(self):
        host, owned = native_documents()
        host['fixed_step_ms'] = 33
        with self.assertRaises(EvidenceError):
            check_native(host, owned)

    def test_abnormal_exit(self):
        host, owned = native_documents()
        owned['runs'][0]['receipt']['actual_exit_code'] = 3221225477
        with self.assertRaises(EvidenceError):
            check_native(host, owned)

    def test_timeout_or_forced_kill(self):
        for field in ('timed_out', 'forced_kill'):
            with self.subTest(field=field):
                host, owned = native_documents()
                owned['runs'][0]['receipt'][field] = True
                with self.assertRaises(EvidenceError):
                    check_native(host, owned)

    def test_completion_prerequisites_missing_or_failed(self):
        fields = (('owned', 'status', 'RUNNING'),
                  ('receipt', 'launcher_exit_code', 1),
                  ('receipt', 'stop_requested', True),
                  ('host', 'actual_exit', 1),
                  ('host', 'native_quit_observed', False),
                  ('host', 'error_ui_quit_injected', True))
        for owner, field, bad_value in fields:
            for missing in (False, True):
                with self.subTest(owner=owner, field=field, missing=missing):
                    host, owned = native_documents()
                    target = {'host': host, 'owned': owned,
                              'receipt': owned['runs'][0]['receipt']}[owner]
                    if missing:
                        del target[field]
                    else:
                        target[field] = bad_value
                    with self.assertRaises(EvidenceError):
                        check_native(host, owned)

    def test_positive_pixels(self):
        self.assertEqual(check_pixels(pixels(), 4, 4, (1, 1, 3, 3)),
                         {'2-15': 1, '15-30': 1, '2-47': 0, '2-55': 0})

    def test_all_background(self):
        data = pixels()
        data[15] = data[2].copy()
        with self.assertRaises(EvidenceError):
            check_pixels(data, 4, 4, (1, 1, 3, 3))

    def test_mouth_unchanged(self):
        data = pixels()
        data[30] = data[15].copy()
        with self.assertRaises(EvidenceError):
            check_pixels(data, 4, 4, (1, 1, 3, 3))

    def test_hide_or_unload_residual(self):
        for frame in (47, 55):
            with self.subTest(frame=frame):
                data = pixels()
                data[frame] = data[15].copy()
                with self.assertRaises(EvidenceError):
                    check_pixels(data, 4, 4, (1, 1, 3, 3))

    def test_missing_capture(self):
        data = pixels()
        del data[30]
        with self.assertRaises(EvidenceError):
            check_pixels(data, 4, 4, (1, 1, 3, 3))

    def test_change_outside_rectangle(self):
        data = pixels()
        data[30][0] = (255, 0, 0)
        with self.assertRaises(EvidenceError):
            check_pixels(data, 4, 4, (1, 1, 3, 3))

    def test_phase_positive(self):
        check_phases(phases(), native_documents()[0])

    def test_phase_label_or_sample_before_command(self):
        original = phases()
        for index in (0, 1):
            data = copy.deepcopy(original)
            if index == 0:
                data['live2d']['frames'][2]['phase'] = 'baseline'
            else:
                data['live2d']['calls'][3]['frame'] = 31
            with self.assertRaises(EvidenceError):
                check_phases(data, native_documents()[0])

    def test_zero_based_capture_mapping_rejects_phase_boundary(self):
        data = phases()
        # Export frame 15 maps to driver frame 16. A correct label on driver
        # frame 15 alone must not authorize a boundary/shifted PNG sample.
        data['live2d']['frames'][3]['phase'] = 'mouth_open'
        with self.assertRaises(EvidenceError):
            check_phases(data, native_documents()[0])


if __name__ == '__main__':
    unittest.main()
