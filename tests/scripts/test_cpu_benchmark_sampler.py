"""Exercise the real compiled sampler protocol; these are not performance baselines."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
import uuid

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from package_runtime import run_runtime_command

PREFIX = '[CAESURA_BENCH] '
METRICS = {'lua_format_append_10000': (10000, 10000),
           'lua_table_reads_10000': (10000, 1515000),
           'sma_skin_8192x10': (81920, 8192)}
BINARY = None


class CompiledSamplerTests(unittest.TestCase):
    def setUp(self):
        self.environment = {k: v for k, v in os.environ.items() if not k.startswith('CAESURA_BENCH_')}
        self.formal = dict(CAESURA_BENCH_PROTOCOL='caesura.cpu-benchmark.v1',
                           CAESURA_BENCH_RUN_UUID=str(uuid.uuid4()),
                           CAESURA_BENCH_WORKLOAD_SHA256=hashlib.sha256(b'protocol-regression-only').hexdigest(),
                           CAESURA_BENCH_WARMUPS='2', CAESURA_BENCH_SAMPLES='10', CAESURA_BENCH_SEED='0')

    def run_binary(self, extra):
        with tempfile.TemporaryDirectory(prefix='caesura-sampler-contract-') as directory:
            work = Path(directory).resolve()
            with (work / 'stdout.log').open('wb') as out, (work / 'stderr.log').open('wb') as err:
                result = run_runtime_command([str(BINARY), '--test-case=Perf:*', '--no-colors'],
                    BINARY.parent, dict(self.environment, **extra), work / 'control', out, err, 120)
            stdout = (work / 'stdout.log').read_text(encoding='utf-8', errors='replace')
            stderr = (work / 'stderr.log').read_text(encoding='utf-8', errors='replace')
            self.assertEqual(result['owned_tree_cleanup'], 'COMPLETE', (result, stderr))
            self.assertFalse(result['forced_kill'] or result['timed_out'], (result, stdout, stderr))
            return result['actual_exit_code'], stdout, stderr

    def test_actual_fixed_workloads_publish_complete_serial_observations(self):
        code, stdout, stderr = self.run_binary(self.formal)
        self.assertEqual(code, 0, (stdout, stderr))
        rows = [json.loads(line[len(PREFIX):]) for line in stdout.splitlines() if line.startswith(PREFIX)]
        self.assertEqual(len(rows), 38, stdout)
        header, footer = rows[0], rows[-1]
        self.assertEqual(header['event'], 'header')
        self.assertEqual(header['metrics'], list(METRICS))
        self.assertEqual(header['workload_sha256'], self.formal['CAESURA_BENCH_WORKLOAD_SHA256'])
        self.assertEqual((header['seed'], header['rng'], header['warmups'], header['samples']),
                         (0, 'none_fixed_workload', 2, 10))
        self.assertEqual((footer['event'], footer['warmup_count'], footer['measurement_count'], footer['correctness_ok']),
                         ('footer', 6, 30, True))
        for row in rows:
            self.assertEqual(row['schema'], self.formal['CAESURA_BENCH_PROTOCOL'])
            self.assertEqual(row['run_uuid'], self.formal['CAESURA_BENCH_RUN_UUID'])
        for metric, (units, expected) in METRICS.items():
            samples = [row for row in rows[1:-1] if row['metric'] == metric]
            self.assertEqual([(row['phase'], row['index']) for row in samples],
                             [('warmup', i) for i in range(2)] + [('measurement', i) for i in range(10)])
            for row in samples:
                self.assertEqual(row['event'], 'sample')
                self.assertIs(type(row['elapsed_ns']), int)
                self.assertGreater(row['elapsed_ns'], 0)
                self.assertEqual(row['work_units'], units)
                self.assertEqual(row['observed_result'], expected)
                self.assertIs(row['correctness_ok'], True)
            if metric == 'sma_skin_8192x10':
                hashes = {row['result_sha256'] for row in samples}
                self.assertEqual(len(hashes), 1)
                self.assertRegex(next(iter(hashes)), r'^[0-9a-f]{64}$')
            else:
                self.assertTrue(all('result_sha256' not in row for row in samples))

    def test_ordinary_mode_keeps_existing_three_workloads(self):
        code, stdout, stderr = self.run_binary({})
        self.assertEqual(code, 0, (stdout, stderr))
        self.assertNotIn(PREFIX, stdout)
        for text in ('10k format+append', '10k table reads', 'CPU skin 8k verts'):
            self.assertIn(text, stdout)

    def test_every_incomplete_protocol_request_fails_without_observations(self):
        for key in self.formal:
            with self.subTest(missing=key):
                env = dict(self.formal)
                del env[key]
                code, stdout, stderr = self.run_binary(env)
                self.assertNotEqual(code, 0, (stdout, stderr))
                self.assertIn('Invalid or incomplete CAESURA_BENCH', stdout)
                self.assertNotIn(PREFIX, stdout)

    def test_malformed_or_changed_workload_parameters_fail(self):
        for key, value in [('CAESURA_BENCH_PROTOCOL', 'unknown'), ('CAESURA_BENCH_RUN_UUID', '"\\n'),
                           ('CAESURA_BENCH_WORKLOAD_SHA256', 'g' * 64), ('CAESURA_BENCH_WARMUPS', '0'),
                           ('CAESURA_BENCH_SAMPLES', '9'), ('CAESURA_BENCH_SEED', '1'),
                           ('CAESURA_BENCH_SAMPLES', '')]:
            with self.subTest(key=key, value=value):
                code, stdout, stderr = self.run_binary(dict(self.formal, **{key: value}))
                self.assertNotEqual(code, 0, (stdout, stderr))
                self.assertNotIn(PREFIX, stdout)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    args, remaining = parser.parse_known_args()
    BINARY = args.binary.resolve(strict=True)
    unittest.main(argv=[sys.argv[0], *remaining])
