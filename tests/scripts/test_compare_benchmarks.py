"""Real owned miniature processes test the collector, never Engine performance."""
from __future__ import annotations

import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import compare_benchmarks as bench
from collect_validation_evidence import collect_evidence
from run_validation import run_profile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    return {"path": str(path), "sha256": sha(path)}


WORKER = r'''
import json, os, sys, time
from pathlib import Path
if sys.argv[1] == "build":
    print("Owned Python fixture build phase; no C++ compilation")
    raise SystemExit(0)
if sys.argv[1] == "cpp":
    print("[doctest] test cases: 3 | 3 passed | 0 failed | 0 skipped")
    raise SystemExit(0)
mode, role, events = sys.argv[1:]
with open(events, "a", encoding="utf-8") as stream:
    stream.write(json.dumps({"role":role,"pid":os.getpid(),"uuid":os.environ["CAESURA_BENCH_RUN_UUID"]})+"\n")
if mode == "fail":
    print("original intentional fixture failure", flush=True)
    raise SystemExit(23)
if mode == "timeout":
    print("owned fixture waiting", flush=True)
    time.sleep(30)
if mode == "overflow":
    while True:
        print("x" * 8192, flush=True)
schema = "caesura.cpu-benchmark.v1"
token = os.environ["CAESURA_BENCH_RUN_UUID"]
metrics = {"lua_format_append_10000":(10000,10000), "lua_table_reads_10000":(10000,1515000), "sma_skin_8192x10":(81920,8192)}
def emit(row):
    print("[CAESURA_BENCH] " + json.dumps(dict(schema=schema,run_uuid=token,**row)), flush=True)
emit(dict(event="header",workload_sha256=os.environ["CAESURA_BENCH_WORKLOAD_SHA256"],seed=0,rng="none_fixed_workload",warmups=2,samples=10,metrics=list(metrics)))
for metric, (units, result) in metrics.items():
    for phase, count in (("warmup",2),("measurement",10)):
        for index in range(count):
            extra = {"result_sha256":"a"*64} if metric == "sma_skin_8192x10" else {}
            emit(dict(event="sample",metric=metric,phase=phase,index=index,elapsed_ns=100000,work_units=units,correctness_ok=True,observed_result=result,**extra))
emit(dict(event="footer",warmup_count=6,measurement_count=30,correctness_ok=True))
print("[doctest] test cases: 3 | 3 passed | 0 failed | 1421 skipped", flush=True)
time.sleep(.06)
'''


def records(token="00000000-0000-4000-8000-000000000001", workload="b" * 64):
    rows = [dict(schema=bench.PROTOCOL, event="header", run_uuid=token, workload_sha256=workload,
                 seed=0, rng="none_fixed_workload", warmups=2, samples=10, metrics=list(bench.METRICS))]
    for metric, (units, result) in bench.METRICS.items():
        for phase, count in (("warmup", 2), ("measurement", 10)):
            for index in range(count):
                row = dict(schema=bench.PROTOCOL, event="sample", run_uuid=token, metric=metric,
                           phase=phase, index=index, elapsed_ns=100000, work_units=units,
                           correctness_ok=True, observed_result=result)
                if metric == "sma_skin_8192x10":
                    row["result_sha256"] = "a" * 64
                rows.append(row)
    rows.append(dict(schema=bench.PROTOCOL, event="footer", run_uuid=token,
                     warmup_count=6, measurement_count=30, correctness_ok=True))
    return rows


def stdout(rows):
    return "\n".join(bench.PREFIX + json.dumps(row) for row in rows) + "\n[doctest] test cases: 3 | 3 passed | 0 failed | 1421 skipped\n"


def policy():
    return dict(schema="caesura.benchmark-policy.v1", comparison="three_paired_process_medians_all_or_inconclusive",
        metrics={metric:dict(unit="elapsed_ns_per_fixed_block", direction="lower", work_units=values[0],
            median_method="upper_middle", regression_fraction=.1, max_within_process_rmad=.1,
            max_between_process_spread=.5, max_order_drift=.5, min_sample_duration_ns=100)
            for metric, values in bench.METRICS.items()})


class ParserTests(unittest.TestCase):
    def parse(self, rows):
        return bench.parse_samples(stdout(rows), rows[0]["run_uuid"], "b" * 64)

    def test_complete_raw_protocol_preserves_warmups_and_filtered_count(self):
        result = self.parse(records())
        self.assertEqual(result["filtered_not_selected"], 1421)
        self.assertEqual(sum(len(row["measurement"]) for row in result["metrics"].values()), 30)
        self.assertEqual(sum(len(row["warmup"]) for row in result["metrics"].values()), 6)

    def test_missing_or_extra_samples_are_rejected(self):
        for delta in (-1, 1):
            row = records()
            if delta < 0:
                row.pop(4)
            else:
                row.insert(4, copy.deepcopy(row[3]))
            with self.subTest(delta=delta), self.assertRaises(ValueError):
                self.parse(row)

    def test_duplicate_index_and_measurement_before_warmup_rejected(self):
        for field, value in (("index", 1), ("phase", "measurement")):
            row = records()
            row[1][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.parse(row)

    def test_boolean_zero_negative_nonfinite_durations_rejected(self):
        for value in (True, 0, -1, float("nan"), float("inf"), "100000"):
            row = records()
            row[1]["elapsed_ns"] = value
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.parse(row)

    def test_wrong_units_result_or_fingerprint_rejected(self):
        for field, value in (("work_units", 999), ("observed_result", 0), ("correctness_ok", 1)):
            row = records()
            row[1][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.parse(row)
        row = records()
        row[-2]["result_sha256"] = "c" * 64
        with self.assertRaises(ValueError):
            self.parse(row)

    def test_seed_workload_extra_field_and_boolean_header_rejected(self):
        for field, value in (("seed", 1), ("seed", False), ("workload_sha256", "c" * 64), ("samples", 9), ("extra", "ignored")):
            row = records()
            row[0][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                self.parse(row)

    def test_duplicate_json_key_and_line_limit_rejected(self):
        raw = stdout(records()).replace('"elapsed_ns": 100000', '"elapsed_ns": 100000, "elapsed_ns": 100000', 1)
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            bench.parse_samples(raw, records()[0]["run_uuid"], "b" * 64)
        with self.assertRaisesRegex(ValueError, "limit"):
            bench.parse_samples(stdout(records()), records()[0]["run_uuid"], "b" * 64, 128)

    def test_missing_failed_or_wrong_doctest_selection_rejected(self):
        raw = stdout(records())
        for suffix in ("", "[doctest] test cases: 3 | 2 passed | 1 failed | 1421 skipped", "[doctest] test cases: 2 | 2 passed | 0 failed | 1422 skipped"):
            text = raw[:raw.index("[doctest]")] + suffix
            with self.subTest(suffix=suffix), self.assertRaises(ValueError):
                bench.parse_samples(text, records()[0]["run_uuid"], "b" * 64)


class MachineObservationTests(unittest.TestCase):
    def test_os_build_affinity_and_real_power_status_are_explicit(self):
        observed=bench.machine_observation()
        self.assertEqual(observed["version"], __import__("platform").version())
        self.assertIn("affinity",observed)
        self.assertIsInstance(observed["power_mode"],dict)
        self.assertIn(observed["power_mode"]["status"],("OBSERVED","UNKNOWN"))
        self.assertIn("source",observed["power_mode"])


class CalculationTests(unittest.TestCase):
    def rows(self, candidate=(100000, 100000, 100000)):
        result = []
        for index, slot in enumerate(bench.SCHEDULE):
            role, pair = slot.split(":")
            parsed = bench.parse_samples(stdout(records()), records()[0]["run_uuid"], "b" * 64)
            for metric in parsed["metrics"].values():
                metric["measurement"] = [candidate[int(pair)] if role == "candidate" else 100000] * 10
            result.append(dict(variant=role, pair_index=int(pair), schedule_index=index, parsed_samples=parsed))
        return result

    def test_three_agreeing_pairs_pass_pure_calculation_only(self):
        result = bench.evaluate_samples(self.rows(), policy(), "compare")
        self.assertEqual(result["comparison"], "PASS")
        self.assertNotIn("gate_pass", result)

    def test_repeatable_slowdown_is_regression(self):
        self.assertEqual(bench.evaluate_samples(self.rows((120000,) * 3), policy(), "compare")["comparison"], "REGRESSION")

    def test_mixed_pairs_are_inconclusive(self):
        self.assertEqual(bench.evaluate_samples(self.rows((105000, 115000, 105000)), policy(), "compare")["comparison"], "INCONCLUSIVE")

    def test_single_fast_sample_cannot_hide_regression(self):
        rows = self.rows((120000,) * 3)
        for row in rows:
            if row["variant"] == "candidate":
                for metric in row["parsed_samples"]["metrics"].values():
                    metric["measurement"][0] = 1000
        self.assertEqual(bench.evaluate_samples(rows, policy(), "compare")["comparison"], "REGRESSION")

    def test_noise_and_chronological_drift_are_inconclusive(self):
        rows = self.rows()
        rows[0]["parsed_samples"]["metrics"][next(iter(bench.METRICS))]["measurement"] = [50000, 150000] * 5
        self.assertEqual(bench.evaluate_samples(rows, policy(), "compare")["measurement_status"], "INCONCLUSIVE")
        limits = policy()
        for metric in limits["metrics"].values():
            metric["max_order_drift"] = .01
        self.assertEqual(bench.evaluate_samples(self.rows((100000, 102000, 104000)), limits, "compare")["comparison"], "INCONCLUSIVE")

    def test_baseline_only_never_evaluates_regression(self):
        result = bench.evaluate_samples([row for row in self.rows() if row["variant"] == "base"], policy(), "baseline_only")
        self.assertEqual((result["measurement_status"], result["comparison"]), ("MEASURED", "NOT_EVALUATED"))

    def test_missing_pair_and_boolean_policy_rejected(self):
        with self.assertRaises(ValueError):
            bench.evaluate_samples(self.rows()[:-1], policy(), "compare")
        limits = policy()
        limits["metrics"][next(iter(bench.METRICS))]["regression_fraction"] = True
        with self.assertRaises(ValueError):
            bench.evaluate_samples(self.rows(), limits, "compare")


class OwnedCollectionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        retained = os.environ.get("CAESURA_BENCH_TEST_RETAIN")
        if retained:
            cls.temp = None
            cls.root = Path(retained).resolve()
            if not cls.root.is_absolute() or cls.root.is_relative_to(ROOT):
                raise ValueError("Retained fixture must be outside repository")
            cls.root.mkdir(exist_ok=False)
        else:
            cls.temp = tempfile.TemporaryDirectory(prefix="caesura-benchmark-contract-")
            cls.root = Path(cls.temp.name).resolve()
        cls.repo = cls.root / "source"
        (cls.repo / "tests/cpp").mkdir(parents=True)
        (cls.repo / "src").mkdir()
        (cls.repo / "src/fixture.txt").write_text("explicit fixture source")
        cls.sampler = cls.repo / "tests/cpp/test_perf_bench.cpp"
        cls.sampler.write_text("explicit fixture sampler; never an Engine binary")
        cls.worker = cls.root / "worker.py"
        cls.worker.write_text(WORKER, encoding="utf-8")
        def git(*args):
            return subprocess.check_output(["git", *args], cwd=cls.repo, stderr=subprocess.PIPE, text=True).strip()
        git("init", "-q")
        git("config", "user.name", "Benchmark fixture")
        git("config", "user.email", "fixture@example.invalid")
        git("add", ".")
        git("commit", "-qm", "fixture source")
        head = git("rev-parse", "HEAD")
        cls.binary = str(Path(sys.executable).resolve())
        cls.profile_path = cls.root / "profile.json"
        profile = {"schema_version":1, "fixture_paths":["src", "tests"], "profiles":{"fixture-release":{
            "platform":{"win32":"windows", "darwin":"macos"}.get(sys.platform,"linux"), "configuration":"Release",
            "checks":[{"id":"build", "parser":"exit-code", "required":True,
                       "command":[cls.binary,"-B",str(cls.worker),"build"], "cwd":"{repo}"},
                      {"id":"cpp", "parser":"doctest", "required":True,"min_discovered":3,"allowed_skips":[],
                       "command":[cls.binary,"-B",str(cls.worker),"cpp"],"cwd":"{repo}","binary":cls.binary}]}}}
        write(cls.profile_path, profile)
        cls.run_dir = cls.root / "u1-raw"
        execution = run_profile(repo=cls.repo, profile_file=cls.profile_path, profile_name="fixture-release",
                    build_dir=cls.root / "unused-build", configuration="Release", run_dir=cls.run_dir, purpose="test-fixture")
        cls.bundle = cls.root / "u1-bundle" / head / execution["run_id"] / "fixture-release"
        collect_evidence(cls.profile_path, "fixture-release", cls.run_dir / "run.json", cls.bundle)
        variant = dict(production_source_sha=head,measurement_source_sha=head,sampler_sha256=sha(cls.sampler),
            source_dir=str(cls.repo),sampler_path="tests/cpp/test_perf_bench.cpp",binary={"path":cls.binary,"sha256":sha(Path(cls.binary))},
            cwd=str(cls.repo),runtime_inputs=[{"path":str(cls.worker),"sha256":sha(cls.worker)}],configuration="Release")
        binding = {key:variant[key] for key in ("production_source_sha","measurement_source_sha","sampler_sha256","binary","configuration")}
        binding.update(schema="caesura.benchmark-build.v1",profile={"path":str(cls.profile_path),"sha256":sha(cls.profile_path)},
            profile_name="fixture-release",execution_receipt={"path":str(cls.run_dir / "run.json"),"sha256":sha(cls.run_dir / "run.json")},
            evidence={"root":str(cls.bundle),"manifest_sha256":sha(cls.bundle / "manifest.json")})
        variant["build_receipt"] = write(cls.root / "binding.json", binding)
        workload = dict(schema=bench.PROTOCOL,sampler_sha256=sha(cls.sampler),metric_ids=list(bench.METRICS),seed=0,rng="none_fixed_workload",
                        state_policy="Explicit miniature fixture, no Engine timing or GC claim")
        workload_ref = write(cls.root / "workload.json", workload)
        machine = dict(schema="caesura.benchmark-machine.v1",observed=bench.machine_observation(),
                       declared=dict(concurrency="test fixture only"))
        machine_ref = write(cls.root / "machine.json", machine)
        cls.request = dict(schema="caesura.benchmark-request.v1",mode="compare",variants={"base":variant,"candidate":copy.deepcopy(variant)},
            workload={"schema":bench.PROTOCOL,**workload_ref,"metric_ids":list(bench.METRICS),"seed":0,"rng":"none_fixed_workload"},
            sampling=dict(processes=3,warmups=2,samples=10,schedule=bench.SCHEDULE),
            machine=dict(manifest_path=machine_ref["path"],manifest_sha256=machine_ref["sha256"],
                         required_fields=[*machine["observed"],"concurrency"],allowed_unknown_fields=["affinity","power_mode"]),
            policy=write(cls.root / "policy.json",policy()),timeouts=dict(per_process_seconds=10,total_seconds=120),
            limits=dict(stdout_bytes=1048576,stderr_bytes=1048576,json_line_bytes=16384))
        cls.request_path = cls.root / "request.json"
        write(cls.request_path, cls.request)
        cls.events = cls.root / "events.jsonl"
        cls.commands = {role:[cls.binary,"-B",str(cls.worker),"success",role,str(cls.events)] for role in ("base","candidate")}

    @classmethod
    def tearDownClass(cls):
        if cls.temp is not None:
            cls.temp.cleanup()

    def request_file(self, change=None):
        value = copy.deepcopy(self.request)
        if change:
            change(value)
        path = self.root / (self._testMethodName + "-request.json")
        write(path, value)
        return path

    def collect(self, change=None, commands=None):
        path = self.request_file(change)
        work = self.root / self._testMethodName
        result = bench.collect_benchmarks(path, sha(path), work, _fixture_commands=self.commands if commands is None else commands)
        return result, path, work

    def test_owned_six_processes_interleaved_and_compare_raw_bytes_fixture_only(self):
        result, request, work = self.collect()
        self.assertEqual(result["status"], "FIXTURE_ONLY", result)
        self.assertLessEqual(result["started_at"],result["finished_at"])
        self.assertEqual([row["variant"]+":"+str(row["pair_index"]) for row in result["processes"]], bench.SCHEDULE)
        self.assertEqual(len({row["run_uuid"] for row in result["processes"]}), 6)
        self.assertEqual(len({row["process_identity"]["pid"] for row in result["processes"]}), 6)
        raw = work / "collection.json"
        verdict = bench.compare_collection(request, sha(request), raw, sha(raw), self.root / "positive-comparison.json")
        self.assertEqual(verdict["status"], "FIXTURE_ONLY", verdict)
        self.assertEqual(verdict["comparison"], "PASS")
        self.assertFalse(verdict["gate_pass"])
        self.assertEqual(verdict["engine_performance"], "NOT_EVALUATED")
        runtime_request = Path(result["processes"][0]["runtime_request"]["path"])
        original_request = runtime_request.read_bytes()
        actual = json.loads(original_request)
        actual["env"]["CAESURA_BENCH_SEED"] = "17"
        runtime_request.write_text(json.dumps(actual),encoding="utf-8")
        altered = copy.deepcopy(result)
        altered["processes"][0]["runtime_request"]["sha256"] = sha(runtime_request)
        altered_path = self.root / "changed-env-collection.json"
        write(altered_path, altered)
        rejected = bench.compare_collection(request,sha(request),altered_path,sha(altered_path),self.root / "changed-env-verdict.json")
        self.assertEqual(rejected["comparison"],"INVALID",rejected)
        runtime_request.write_bytes(original_request)
        cli = subprocess.run([self.binary,"-B",str(ROOT / "scripts/compare_benchmarks.py"),"compare",
            "--request",str(request),"--request-sha256",sha(request),"--collection",str(raw),"--collection-sha256",sha(raw),
            "--output",str(self.root / "cli-comparison.json")],capture_output=True,timeout=40)
        self.assertEqual(cli.returncode,77,cli.stderr.decode(errors="replace"))
        for name, mutate in (("missing",lambda r:r["processes"].pop()),
                ("pair",lambda r:r["processes"][1].update(pair_index=2)),
                ("uuid",lambda r:r["processes"][1].update(run_uuid=r["processes"][0]["run_uuid"])),
                ("machine",lambda r:r["machine_after"].update(node="another-machine")),
                ("power",lambda r:r["machine_after"]["power_mode"].update(values={"scheme_guid":"different"})),
                ("utc",lambda r:r.update(finished_at="2000-01-01T00:00:00+00:00")),
                ("cached",lambda r:r["processes"][0]["parsed_samples"]["metrics"]["lua_table_reads_10000"]["measurement"].__setitem__(0,999))):
            altered=copy.deepcopy(result)
            mutate(altered)
            source=self.root / (name+"-collection.json")
            write(source,altered)
            rejected=bench.compare_collection(request,sha(request),source,sha(source),self.root / (name+"-verdict.json"))
            self.assertEqual(rejected["comparison"],"INVALID",(name,rejected))
        candidate_log=Path(result["processes"][1]["stdout"]["path"])
        candidate_raw=candidate_log.read_bytes()
        candidate_log.write_bytes(candidate_raw.replace(b'a'*64,b'c'*64))
        altered=copy.deepcopy(result)
        altered["processes"][1]["stdout"]["sha256"]=sha(candidate_log)
        altered["processes"][1]["parsed_samples"]["skin_result_sha256"]="c"*64
        source=self.root / "fingerprint-collection.json"
        write(source,altered)
        rejected=bench.compare_collection(request,sha(request),source,sha(source),self.root / "fingerprint-verdict.json")
        self.assertEqual(rejected["comparison"],"INVALID",rejected)
        self.assertIn("Workload output differs",rejected["first_failure"])
        candidate_log.write_bytes(candidate_raw)
        prior = raw.read_bytes()
        with self.assertRaisesRegex(ValueError, "existing"):
            bench.collect_benchmarks(request, sha(request), work, _fixture_commands=self.commands)
        self.assertEqual(raw.read_bytes(), prior)
        log = Path(result["processes"][0]["stdout"]["path"])
        original_log=log.read_bytes()
        log.write_text(log.read_text().replace('"elapsed_ns": 100000','"elapsed_ns": 100001',1),encoding="utf-8")
        changed = bench.compare_collection(request, sha(request), raw, sha(raw), self.root / "tampered-comparison.json")
        self.assertEqual(changed["comparison"], "INVALID")
        self.assertFalse(changed["gate_pass"])
        (self.root / "mutated-negative-stdout.log").write_bytes(log.read_bytes())
        log.write_bytes(original_log)

    def test_first_actual_failure_kept_and_no_replacement_samples(self):
        commands = copy.deepcopy(self.commands)
        commands["candidate"][3] = "fail"
        result, _, work = self.collect(commands=commands)
        self.assertEqual(result["status"], "FAIL")
        self.assertEqual(len(result["processes"]), 2)
        receipt = json.loads((work / "01-candidate-0/control/run.json").read_text())
        self.assertEqual(receipt["actual_exit_code"], 23)
        self.assertEqual(receipt["owned_tree_cleanup"], "COMPLETE")
        self.assertIn("original intentional fixture failure", (work / "01-candidate-0/stdout.log").read_text())
        self.assertEqual(result["first_failure"]["process_index"], 1)

    def test_timeout_reaps_owned_process_and_keeps_first_receipt(self):
        commands = copy.deepcopy(self.commands)
        commands["base"][3] = "timeout"
        result, _, work = self.collect(lambda r:r["timeouts"].update(per_process_seconds=.5),commands)
        self.assertEqual(result["status"], "FAIL")
        self.assertEqual(len(result["processes"]), 1)
        receipt = json.loads((work / "00-base-0/control/run.json").read_text())
        self.assertTrue(receipt["timed_out"])
        self.assertEqual(receipt["owned_tree_cleanup"], "COMPLETE")

    def test_output_overflow_stops_owned_process(self):
        commands = copy.deepcopy(self.commands)
        commands["base"][3] = "overflow"
        result, _, work = self.collect(lambda r:r["limits"].update(stdout_bytes=16384),commands)
        self.assertEqual(result["status"], "FAIL")
        self.assertEqual(len(result["processes"]), 1)
        receipt = json.loads((work / "00-base-0/control/run.json").read_text())
        self.assertEqual(receipt["owned_tree_cleanup"], "COMPLETE")
        self.assertTrue(receipt["stop_requested"])

    def test_formal_entry_refuses_real_u1_fixture_without_starting_process(self):
        path = self.request_file()
        result = bench.collect_benchmarks(path, sha(path), self.root / self._testMethodName)
        self.assertEqual(result["status"], "FAIL")
        self.assertEqual(result["processes"], [])
        self.assertIn("test-fixture", result["first_failure"]["message"])

    def test_prelocked_source_binary_policy_and_machine_reject_before_process(self):
        changes = [lambda r:r["variants"]["base"].update(measurement_source_sha="0"*40),
                   lambda r:r["variants"]["base"]["binary"].update(sha256="0"*64),
                   lambda r:r["policy"].update(sha256="0"*64),
                   lambda r:r["machine"].update(manifest_sha256="0"*64)]
        for index, change in enumerate(changes):
            value = copy.deepcopy(self.request)
            change(value)
            path = self.root / f"invalid-{index}.json"
            write(path,value)
            result = bench.collect_benchmarks(path,sha(path),self.root / f"invalid-work-{index}",_fixture_commands=self.commands)
            self.assertEqual(result["status"],"FAIL",result)
            self.assertEqual(result["processes"],[])

    def test_dirty_source_refused_before_measurement(self):
        marker = self.repo / "untracked.txt"
        marker.write_text("dirty")
        try:
            result, _, _ = self.collect()
        finally:
            marker.unlink()
        self.assertEqual(result["processes"],[])
        self.assertIn("Dirty",result["first_failure"]["message"])

    def test_affinity_and_power_cannot_be_silently_omitted(self):
        for key in ("affinity","power_mode"):
            request=copy.deepcopy(self.request)
            request["machine"]["required_fields"].remove(key)
            path=self.root / ("missing-"+key+".json")
            write(path,request)
            with self.assertRaisesRegex(ValueError,"required machine"):
                bench._request(path,sha(path))

    def test_unknown_power_requires_explicit_prelocked_permission(self):
        request=copy.deepcopy(self.request)
        machine=json.loads(Path(request["machine"]["manifest_path"]).read_text())
        machine["observed"]["power_mode"]={"status":"UNKNOWN","source":"controlled test boundary","values":{"reason":"unavailable"}}
        reference=write(self.root / "unknown-power.json",machine)
        request["machine"].update(manifest_path=reference["path"],manifest_sha256=reference["sha256"],allowed_unknown_fields=[])
        path=self.root / "unknown-power-request.json"
        write(path,request)
        with self.assertRaisesRegex(ValueError,"not permitted"):
            bench._request(path,sha(path))
        request["machine"]["allowed_unknown_fields"]=["affinity","power_mode"]
        write(path,request)
        bench._request(path,sha(path))

    def test_request_digest_and_fixed_sampling_refused(self):
        result, _, _ = self.collect(lambda r:r["sampling"].update(samples=11))
        self.assertEqual(result["status"],"FAIL")
        self.assertEqual(result["processes"],[])
        result = bench.collect_benchmarks(self.request_path,"0"*64,self.root / "wrong-request-digest",_fixture_commands=self.commands)
        self.assertEqual(result["status"],"FAIL")

    def test_work_inside_source_refused_without_creating_directory(self):
        path = self.request_file()
        work = self.repo / "unexpected-output"
        with self.assertRaises(ValueError):
            bench.collect_benchmarks(path,sha(path),work,_fixture_commands=self.commands)
        self.assertFalse(work.exists())

    def test_comparison_output_inside_source_writes_no_failure_receipt(self):
        path=self.request_file()
        output=self.repo / "rejected-comparison.json"
        try:
            with self.assertRaises(ValueError):
                bench.compare_collection(path,sha(path),self.root / "unused-collection.json","0"*64,output)
            self.assertFalse(output.exists())
        finally:
            if output.exists():
                output.unlink()

    def test_late_source_build_and_all_raw_mutations_rejected(self):
        result, request, work=self.collect()
        self.assertEqual(result["status"],"FIXTURE_ONLY",result)
        raw=work / "collection.json"
        row=result["processes"][0]
        targets={"source":self.repo / "src/fixture.txt",
                 "build":Path(self.request["variants"]["base"]["build_receipt"]["path"]),
                 **{key:Path(row[key]["path"]) for key in ("stdout","stderr","runtime_request","run_receipt")}}
        original=bench.parse_samples
        for name,target in targets.items():
            previous=target.read_bytes()
            changed=[]
            def late(*args,**kwargs):
                parsed=original(*args,**kwargs)
                if not changed:
                    target.write_bytes(previous+b'\nlate fixture mutation\n')
                    changed.append(True)
                return parsed
            try:
                with self.subTest(target=name), patch.object(bench,"parse_samples",side_effect=late):
                    verdict=bench.compare_collection(request,sha(request),raw,sha(raw),self.root / ("late-"+name+"-verdict.json"))
                    self.assertTrue(changed)
                    self.assertEqual(verdict["execution_result"],"FAIL",verdict)
                    self.assertEqual(verdict["comparison"],"INVALID")
                    self.assertFalse(verdict["gate_pass"])
            finally:
                target.write_bytes(previous)

    def test_baseline_only_real_three_processes_never_gate_passes(self):
        def change(req):
            req["mode"]="baseline_only"
            del req["variants"]["candidate"]
            req["sampling"]["schedule"]=["base:0","base:1","base:2"]
        result, request, work = self.collect(change,{"base":self.commands["base"]})
        self.assertEqual(result["status"],"FIXTURE_ONLY",result)
        raw=work / "collection.json"
        verdict=bench.compare_collection(request,sha(request),raw,sha(raw),self.root / "baseline-comparison.json")
        self.assertEqual(verdict["comparison"],"NOT_EVALUATED",verdict)
        self.assertEqual(verdict["measurement_status"],"MEASURED")
        self.assertFalse(verdict["gate_pass"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
