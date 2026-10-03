"""Real owned Python children and synthetic PNGs; no native engine/GPU claim."""
from pathlib import Path
import copy
import json
import os
import sys
import tempfile
import threading
import time
import unittest
from concurrent.futures import ThreadPoolExecutor
from dataclasses import replace
from unittest import mock

sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'scripts'))
from package_runtime import process_identity, RuntimeContractError
from run_engine_soak import run_observed_command, verify_images, verify_prerequisites, sha
import run_engine_soak as soak_driver
import validation_process
from test_render_contracts_driver import png


class OwnerTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='soak-driver-')
        self.root=Path(self.temp.name)

    def tearDown(self):self.temp.cleanup()

    def run_child(self,body,**settings):
        script=self.root/'child.py'
        script.write_text('from pathlib import Path\nimport json,os,time\nr=Path.cwd()\n'+body,encoding='utf-8')
        options=dict(timeout=8,startup_seconds=4,progress_seconds=.4)
        options.update(settings)
        return run_observed_command([sys.executable,'-B',str(script)],self.root,
            dict(os.environ,PYTHONDONTWRITEBYTECODE='1'),self.root/'evidence',
            self.root/'ready.json',self.root/'events.jsonl',**options)

    ready="(r/'ready.json').write_text('{}',encoding='utf-8')\n(r/'events.jsonl').write_text('{}\\n',encoding='utf-8')\n"
    hold="while not (r/'release').exists():time.sleep(.01)\n"

    def inspect_release(self,identity):
        self.assertEqual(process_identity(identity.pid),identity)
        (self.root/'release').write_text('release',encoding='utf-8')
        return {'observed_pid':identity.pid}

    def retired(self,result):
        receipt=result['receipt']
        self.assertEqual(receipt['owned_tree_cleanup'],'COMPLETE')
        self.assertGreater(result['owner_observed_seconds'],0)
        self.assertEqual(receipt,json.loads((self.root/'evidence/process/run.json').read_text(encoding='utf-8')))
        with self.assertRaises(RuntimeContractError):process_identity(receipt['process']['pid'])

    def test_success_requires_live_owner_inspection_and_actual_zero_exit(self):
        result=self.run_child(self.ready+self.hold,inspect=self.inspect_release)
        self.assertEqual(result['status'],'OBSERVED',msg=result)
        self.assertEqual(result['receipt']['actual_exit_code'],0)
        self.assertEqual(result['inspection']['observed_pid'],result['receipt']['process']['pid'])
        self.retired(result)

    def _observe_controlled_publication_boundary(self, *, complete_child, forge_done=False,
                                               cleanup_failure=False):
        """Real child/launcher/cleanup; control only the observer clock and publication.

        No heartbeat is added. The fixture still writes exactly one progress line.
        The owned runner keeps its real clock and the original 8-second bound.
        """
        inspected = threading.Event()
        terminal = threading.Event()
        publish = threading.Event()
        captured = {}
        real_owned = validation_process.run_owned_command
        real_started = time.monotonic()
        progress_step = .401

        def hold_completed_owned_return(*args, **kwargs):
            # Run the actual owned helper first, including its process-tree cleanup.
            code = real_owned(*args, **kwargs)
            captured['launcher_exit'] = code
            captured['result'] = json.loads(
                (self.root/'evidence/process/result.json').read_text(encoding='utf-8'))
            terminal.set()
            if not publish.wait(6):
                raise AssertionError('Fixture publication barrier was not released')
            if cleanup_failure:
                raise RuntimeError('declared cleanup completion failure')
            return code

        class ObserverClock:
            value = real_started
            advanced = False

            def monotonic(clock):
                return clock.value

            def sleep(clock, _seconds):
                if time.monotonic()-real_started > 7:
                    publish.set()
                    raise AssertionError('Fixture did not reach its controlled boundary')
                eligible = inspected.is_set() and (
                    terminal.is_set() if complete_child else True)
                if eligible:
                    if clock.advanced:
                        publish.set()
                    else:
                        # A loop-end hook follows the first progress-size reset.
                        # Advance only this module's clock beyond the unchanged .4.
                        clock.value += progress_step
                        clock.advanced = True
                # Yield to the real owned thread; elapsed sleep is not the oracle.
                publish.wait(.001)

        clock = ObserverClock()

        class PublicationExecutor(ThreadPoolExecutor):
            def submit(executor, function, *args, **kwargs):
                future = super().submit(function, *args, **kwargs)

                class PublicationFuture:
                    def done(_self):
                        return future.done()

                    def result(_self):
                        # The observer's final join releases publication even on RED.
                        publish.set()
                        return future.result()

                return PublicationFuture()

        def inspect(identity):
            self.assertEqual(process_identity(identity.pid), identity)
            captured['inspected_owner'] = identity
            if forge_done:
                (self.root/'done').write_text('done', encoding='utf-8')
                (self.root/'evidence/process/result.json').write_text(json.dumps({
                    'process': identity.__dict__, 'status': 'EXITED',
                    'actual_exit_code': 0, 'stop_requested': False, 'forced_kill': False,
                }), encoding='utf-8')
                captured['forged_done'] = True
            if complete_child:
                (self.root/'release').write_text('release', encoding='utf-8')
            inspected.set()
            return {'observed_pid': identity.pid}

        try:
            with mock.patch.object(soak_driver, 'time', clock), \
                 mock.patch.object(soak_driver, 'ThreadPoolExecutor', PublicationExecutor):
                if complete_child:
                    with mock.patch.object(validation_process, 'run_owned_command',
                                           hold_completed_owned_return):
                        result = self.run_child(self.ready+self.hold, inspect=inspect)
                else:
                    result = self.run_child(self.ready+self.hold, inspect=inspect)
        finally:
            publish.set()
        self.assertTrue(clock.advanced, result)
        self.assertEqual((self.root/'events.jsonl').read_text(encoding='utf-8'), '{}\n')
        return result, captured, terminal.is_set()

    def test_completed_owned_child_pending_publication_is_not_missing_progress(self):
        result, captured, terminal = self._observe_controlled_publication_boundary(
            complete_child=True)
        self.assertTrue(terminal, result)
        self.assertEqual(captured['launcher_exit'], 0)
        self.assertEqual(captured['result']['status'], 'EXITED')
        self.assertEqual(captured['result']['actual_exit_code'], 0)
        self.assertFalse(captured['result']['stop_requested'])
        self.assertEqual(result['receipt']['actual_exit_code'], 0)
        self.assertEqual(result['receipt']['owned_tree_cleanup'], 'COMPLETE')
        self.assertEqual(result['receipt']['process'], result['observed_process'])
        self.retired(result)
        # Existing production should RED here while preserving the real zero exit.
        self.assertEqual(result['status'], 'OBSERVED', msg=result)

    def test_live_owned_child_still_fails_unchanged_progress_deadline(self):
        result, captured, terminal = self._observe_controlled_publication_boundary(
            complete_child=False)
        self.assertFalse(terminal)
        self.assertIn('inspected_owner', captured)
        self.assertEqual(result['status'], 'FAIL')
        self.assertIn('Progress deadline expired after readiness', result['error'])
        self.assertTrue(result['receipt']['stop_requested'])
        self.assertEqual(result['receipt']['status'], 'STOPPED')
        self.retired(result)

    def test_live_child_forged_done_and_result_do_not_stop_watchdog(self):
        from hashlib import sha256
        result, captured, terminal = self._observe_controlled_publication_boundary(
            complete_child=False, forge_done=True)
        control = self.root/'evidence/process'
        diagnostics = {}
        for name, path in (
                ('forged_result', control/'result.json'),
                ('unpublished_terminal', control/'.result.json.writing'),
                ('parent_receipt', control/'run.json'),
                ('launcher_stderr', self.root/'evidence/stderr')):
            if not path.is_file():
                diagnostics[name] = {'missing': True}
                continue
            with path.open('rb') as stream:
                raw = stream.read(65537)
            self.assertLessEqual(len(raw), 65536)
            diagnostics[name] = {'bytes': len(raw), 'sha256': sha256(raw).hexdigest(),
                                 'text': raw.decode('utf-8')}
        # TemporaryDirectory cleans the fixture after this test. Preserve the
        # original rejection chain in captured output even if an assertion fails.
        print('FORGED_TERMINAL_REJECTION ' + json.dumps({
            'observation': result, 'files': diagnostics,
            'unpublished_terminal_is_diagnostic_only': True,
        }, sort_keys=True))
        self.assertTrue(captured['forged_done'])
        self.assertFalse(terminal)
        self.assertEqual(result['status'], 'FAIL')
        self.assertIn('Progress deadline expired after readiness', result['error'])
        self.assertNotIn('work_terminal', result)
        self.assertNotEqual(result['receipt']['launcher_exit_code'], 0)
        self.assertIn('Runtime launcher failed', result['receipt']['error'])
        self.assertIn('Refusing existing control result:',
                      diagnostics['launcher_stderr']['text'])
        forged = {'process': captured['inspected_owner'].__dict__, 'status': 'EXITED',
                  'actual_exit_code': 0, 'stop_requested': False, 'forced_kill': False}
        self.assertEqual(diagnostics['forged_result']['text'], json.dumps(forged))
        unpublished = json.loads(diagnostics['unpublished_terminal']['text'])
        self.assertEqual(unpublished['process'], result['observed_process'])
        self.assertEqual(unpublished['status'], 'STOPPED')
        self.assertTrue(unpublished['stop_requested'])
        self.assertFalse(unpublished['forced_kill'])
        self.assertIs(type(unpublished['actual_exit_code']), int)
        self.assertNotEqual(unpublished['actual_exit_code'], 0)
        self.assertEqual(json.loads(diagnostics['parent_receipt']['text']), result['receipt'])
        # The forged public EXITED/0 fields are not acceptance evidence. The
        # trusted launcher failure and parent cleanup remain mandatory.
        self.retired(result)

    def test_owner_query_error_cannot_become_normal_completion(self):
        with mock.patch.object(soak_driver, 'process_identity',
                               side_effect=RuntimeContractError('declared query unavailable')):
            result = self.run_child(self.ready+self.hold)
        self.assertEqual(result['status'], 'FAIL')
        self.assertIn('declared query unavailable', result['error'])
        self.assertFalse(result['ready_observed'])
        self.assertTrue(result['receipt']['stop_requested'])
        self.retired(result)

    def test_terminal_notification_owner_mismatch_cannot_accept_actual_zero(self):
        original = soak_driver.run_runtime_command
        def mismatched(*args, on_work_exit=None, **kwargs):
            def notify(value):
                on_work_exit(replace(value, process=replace(
                    value.process, created=value.process.created+'-different')))
            return original(*args, on_work_exit=notify, **kwargs)
        with mock.patch.object(soak_driver, 'run_runtime_command', mismatched):
            result = self.run_child(self.ready+self.hold, inspect=self.inspect_release)
        self.assertEqual(result['receipt']['actual_exit_code'], 0)
        self.assertEqual(result['status'], 'FAIL')
        self.assertIn('Work terminal owner differs', result['error'])
        self.retired(result)

    def test_reported_cleanup_failure_after_terminal_cannot_accept_actual_zero(self):
        # Reach the trusted terminal callback before crossing the unchanged .4s
        # observer deadline, then inject failure at cleanup-return publication.
        # Real child/launcher scheduling must not select a different first error.
        result, captured, terminal = self._observe_controlled_publication_boundary(
            complete_child=True, cleanup_failure=True)
        self.assertTrue(terminal, result)
        self.assertEqual(captured['launcher_exit'], 0)
        self.assertEqual(captured['result']['actual_exit_code'], 0)
        self.assertEqual(result['work_terminal']['actual_exit_code'], 0)
        self.assertEqual(result['work_terminal']['process'], result['observed_process'])
        self.assertEqual(result['status'], 'FAIL')
        self.assertIn('declared cleanup completion failure', result['error'])
        self.assertNotEqual(result['receipt']['owned_tree_cleanup'], 'COMPLETE')
        with self.assertRaises(RuntimeContractError):
            process_identity(captured['result']['process']['pid'])

    def test_nonzero_exit_is_preserved_as_failure(self):
        result=self.run_child(self.ready+self.hold+'raise SystemExit(7)\n',inspect=self.inspect_release)
        self.assertEqual(result['status'],'FAIL')
        self.assertEqual(result['receipt']['actual_exit_code'],7)
        self.retired(result)

    def test_stalled_ready_child_is_stopped_by_owned_progress_deadline(self):
        result=self.run_child(self.ready+self.hold)
        self.assertEqual(result['status'],'FAIL')
        self.assertIn('Progress deadline',result['error'])
        self.assertTrue(result['receipt']['stop_requested'])
        self.retired(result)

    def test_missing_readiness_is_not_a_successful_process(self):
        result=self.run_child(self.hold,startup_seconds=1)
        self.assertEqual(result['status'],'FAIL')
        self.assertIn('Startup deadline',result['error'])
        self.retired(result)

    def test_inspection_failure_stops_the_same_child_and_keeps_original_error(self):
        def reject(identity):raise ValueError('declared module mismatch')
        result=self.run_child(self.ready+self.hold,inspect=reject)
        self.assertEqual(result['status'],'FAIL')
        self.assertIn('declared module mismatch',result['error'])
        self.retired(result)

    def test_total_timeout_keeps_durable_receipt_even_when_events_keep_arriving(self):
        result=self.run_child(self.ready+"while True:\n with (r/'events.jsonl').open('a') as f:f.write('{}\\n')\n time.sleep(.02)\n",
                              timeout=3,progress_seconds=10)
        self.assertEqual(result['status'],'FAIL')
        self.assertTrue(result['receipt']['timed_out'])
        self.assertEqual(result['receipt']['status'],'TIMED_OUT')
        self.retired(result)

    def test_existing_evidence_directory_is_rejected_before_starting(self):
        (self.root/'evidence').mkdir()
        marker=self.root/'evidence/keep';marker.write_text('original',encoding='utf-8')
        with self.assertRaises(FileExistsError):self.run_child("(r/'started').touch()\n")
        self.assertFalse((self.root/'started').exists())
        self.assertEqual(marker.read_text(encoding='utf-8'),'original')


class ImageTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='soak-png-')
        # The fixture owns this root; supply its canonical path to the same
        # strict output boundary used by real captures on every host.
        self.root=Path(self.temp.name).resolve(strict=True)
        self.events=[]
        for page,color in [('a',(20,50,90,255)),('b',(130,35,50,255)),('restored',(20,50,90,255))]:
            raw=bytes(color)*(640*360);data=png(640,360,raw)
            name=f'cycle-1-{page}.png';(self.root/name).write_bytes(data)
            self.events.append(dict(event='capture_consumed',cycle=0,detail=dict(page=page,file=name,png_bytes=len(data))))

    def tearDown(self):self.temp.cleanup()

    def test_exact_restored_pixels_are_accepted_and_hashed(self):
        before=copy.deepcopy(self.events);result=verify_images(self.root,self.events)
        self.assertEqual(len(result),3)
        self.assertTrue(all(len(row['sha256'])==64 for row in result))
        self.assertEqual(before,self.events)

    def test_restore_difference_away_from_sample_pixel_is_rejected(self):
        raw=bytearray(bytes((20,50,90,255))*(640*360));raw[(100*640+100)*4]=21
        data=png(640,360,raw);(self.root/'cycle-1-restored.png').write_bytes(data)
        self.events[-1]['detail']['png_bytes']=len(data)
        with self.assertRaisesRegex(ValueError,'Restored pixels'):verify_images(self.root,self.events)

    def test_reported_byte_count_cannot_replace_actual_png(self):
        path=self.root/'cycle-1-a.png';data=bytearray(path.read_bytes());data[-1]^=1;path.write_bytes(data)
        with self.assertRaisesRegex(ValueError,'CRC'):verify_images(self.root,self.events)

    def test_output_path_traversal_is_rejected(self):
        self.events[0]['detail']['file']='../outside.png'
        with self.assertRaises(ValueError):verify_images(self.root,self.events)


class PrerequisiteTests(unittest.TestCase):
    """Synthetic caller-selected reports; no native control execution claim."""
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='soak-prerequisites-');self.root=Path(self.temp.name)
        self.source=dict(source_sha='a'*40,dirty=False,worktree_fingerprint='b'*64)
        self.binaries={'probe.exe':'c'*64};self.cache='d'*64
        self.base=self.root/'baseline';self.base.mkdir()
        self.baseline=dict(status='DIAGNOSTIC_REVIEWED',source_stable=True,source_before=self.source,source_after=self.source,
                           binary_locks=self.binaries,configuration=dict(cache_sha256=self.cache))
        self.write(self.base,self.baseline)
        common=dict(source_stable=True,source_before=self.source,source_after=self.source,binary_locks=self.binaries,
                    accepted_soak=False,baseline_root=str(self.base),baseline_run_sha256=sha(self.base/'run.json'))
        self.reports={
            'cold':dict(common,status='COLD_CONTROLS_REVIEWED',cold_restart='OBSERVED',corrupt_control='REJECTED_WITH_OLD_PAGE'),
            'fault':dict(common,status='NATIVE_FAULT_CONTROLS_REVIEWED',stages=[dict(role=role,status='EXPECTED_REJECTION_REVIEWED') for role in ('fault-resources','fault-worker','fault-stall')])}
        for kind,value in self.reports.items():
            (self.root/kind).mkdir();self.write(self.root/kind,value)

    def tearDown(self):self.temp.cleanup()

    def write(self,d,value):(d/'run.json').write_text(json.dumps(value),encoding='utf-8')

    def selections(self):
        for kind,value in self.reports.items():self.write(self.root/kind,value)
        return {kind:dict(root=str(self.root/kind),sha256=sha(self.root/kind/'run.json')) for kind in self.reports}

    def verify(self,mode='short',selections=None):
        return verify_prerequisites(mode,self.selections() if selections is None else selections,self.source,self.binaries,self.cache)

    def add_short(self):
        (self.root/'short').mkdir()
        self.reports['short']=dict(status='SHORT_REVIEWED',mode='short',accepted_soak=False,context_restart='OBSERVED',
            source_stable=True,source_before=self.source,source_after=self.source,binary_locks=self.binaries,
            configuration=dict(cache_sha256=self.cache),prerequisites=self.verify())

    def test_exact_matching_short_and_long_dependencies_are_pinned(self):
        before=copy.deepcopy(self.reports);pins=self.verify()
        self.assertEqual(set(pins),{'cold','fault'});self.assertEqual(before,self.reports)
        for kind in pins:self.assertEqual(pins[kind]['run_sha256'],sha(self.root/kind/'run.json'))
        self.add_short();self.assertEqual(set(self.verify('long')),{'cold','fault','short'})

    def test_missing_or_extra_control_and_unknown_mode_are_rejected(self):
        selections=self.selections();del selections['fault']
        with self.assertRaises(ValueError):self.verify(selections=selections)
        self.add_short()
        with self.assertRaises(ValueError):self.verify()
        with self.assertRaises(ValueError):self.verify('longer')

    def test_caller_digest_cannot_be_substituted_by_the_report(self):
        selections=self.selections();selections['cold']['sha256']='0'*64
        with self.assertRaises(ValueError):self.verify(selections=selections)

    def test_changed_source_or_unstable_control_is_rejected(self):
        for key,value in [('source_stable',False),('source_before',dict(self.source,source_sha='f'*40)),('source_after',dict(self.source,dirty=True))]:
            with self.subTest(key=key):
                original=self.reports['fault'][key];self.reports['fault'][key]=value
                with self.assertRaises(ValueError):self.verify()
                self.reports['fault'][key]=original

    def test_binary_configuration_and_linked_baseline_are_bound(self):
        self.reports['cold']['binary_locks']={'probe.exe':'0'*64}
        with self.assertRaises(ValueError):self.verify()
        self.reports['cold']['binary_locks']=self.binaries
        self.write(self.base,dict(self.baseline,extra='changed'))
        with self.assertRaises(ValueError):self.verify()
        self.reports['cold']['baseline_run_sha256']=sha(self.base/'run.json')
        self.reports['fault']['baseline_run_sha256']=sha(self.base/'run.json')
        self.baseline['configuration']['cache_sha256']='0'*64;self.write(self.base,self.baseline)
        for value in self.reports.values():value['baseline_run_sha256']=sha(self.base/'run.json')
        with self.assertRaises(ValueError):self.verify()

    def test_partial_cold_or_failed_fault_controls_are_rejected(self):
        self.reports['cold']['corrupt_control']='NOT_RUN'
        with self.assertRaises(ValueError):self.verify()
        self.reports['cold']['corrupt_control']='REJECTED_WITH_OLD_PAGE'
        self.reports['fault']['stages'][1]['status']='FAIL'
        with self.assertRaises(ValueError):self.verify()

    def test_long_requires_short_with_the_same_control_digests(self):
        self.add_short();self.reports['short']['mode']='diagnostic'
        with self.assertRaises(ValueError):self.verify('long')
        self.reports['short']['mode']='short';self.reports['short']['prerequisites']['cold']['run_sha256']='0'*64
        with self.assertRaises(ValueError):self.verify('long')


if __name__=='__main__':unittest.main(verbosity=2)
