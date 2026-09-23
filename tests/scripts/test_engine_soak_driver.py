"""Real owned Python children and synthetic PNGs; no native engine/GPU claim."""
from pathlib import Path
import copy
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'scripts'))
from package_runtime import process_identity, RuntimeContractError
from run_engine_soak import run_observed_command, verify_images
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
        self.assertEqual(result['status'],'OBSERVED')
        self.assertEqual(result['receipt']['actual_exit_code'],0)
        self.assertEqual(result['inspection']['observed_pid'],result['receipt']['process']['pid'])
        self.retired(result)

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
        self.temp=tempfile.TemporaryDirectory(prefix='soak-png-');self.root=Path(self.temp.name)
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


if __name__=='__main__':unittest.main(verbosity=2)
