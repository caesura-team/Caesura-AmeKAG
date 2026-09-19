#!/usr/bin/env python3
"""Collect locked Release CPU samples and compare three paired processes.

The caller selects and hashes request, policy and strict U1 build evidence
before measuring. This tool neither chooses tolerances nor authorizes release.
It never retries a process, drops a sample or extends a failed collection.
"""
from __future__ import annotations

import argparse
import ctypes
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import shutil
import stat
import subprocess
import sys
import threading
import time
import uuid

from collect_validation_evidence import parse_doctest
from package_runtime import run_runtime_command
from verify_release_candidate import verify_evidence

PROTOCOL = "caesura.cpu-benchmark.v1"
PREFIX = "[CAESURA_BENCH] "
METRICS = {"lua_format_append_10000": (10000, 10000),
           "lua_table_reads_10000": (10000, 1515000),
           "sma_skin_8192x10": (81920, 8192)}
SCHEDULE = ["base:0", "candidate:0", "candidate:1", "base:1", "base:2", "candidate:2"]
MAX_JSON = 16 * 1024 * 1024
ROOT = Path(__file__).resolve().parents[1]


class BenchmarkError(ValueError):
    pass


def need(ok, message):
    if not ok:
        raise BenchmarkError(message)


def fields(value, names, label):
    need(isinstance(value, dict) and set(value) == set(names.split()), f"Invalid {label} fields")
    return value


def integer(value, low, high, label):
    need(type(value) is int and low <= value <= high, f"Invalid {label}")
    return value


def number(value, low, high, label):
    need(type(value) in (int, float) and math.isfinite(value) and low <= value <= high,
         f"Invalid {label}")
    return value


def digest_string(value, length=64):
    need(isinstance(value, str) and re.fullmatch(r"[0-9a-f]{%d}" % length, value), "Invalid digest")
    return value


def canonical(value, *, directory=False):
    need(isinstance(value, (str, Path)) and Path(value).is_absolute(), "Path must be absolute")
    path = Path(value)
    for part in (path, *path.parents):
        need(not part.is_symlink() and not part.is_junction(), f"Linked path: {path}")
    need(path == path.resolve(strict=True), f"Path must be canonical: {path}")
    info = path.stat()
    need(stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode), f"Wrong path type: {path}")
    if not directory:
        need(info.st_nlink == 1, f"Hard-linked input: {path}")
    return path


def sha(path):
    path = canonical(path)
    before = path.stat()
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    after = path.stat()
    need((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) ==
         (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns), "File changed while hashing")
    return digest.hexdigest()


def reference(path):
    return {"path": str(canonical(path)), "sha256": sha(path)}


def locked(ref):
    fields(ref, "path sha256", "file reference")
    digest_string(ref["sha256"])
    path = canonical(ref["path"])
    need(sha(path) == ref["sha256"], f"External digest mismatch: {path}")
    return path


def decode(raw):
    def pairs(items):
        result = {}
        for key, value in items:
            need(key not in result, "Duplicate JSON key")
            result[key] = value
        return result
    def constant(value):
        raise BenchmarkError("Non-finite JSON constant: " + value)
    return json.loads(raw, object_pairs_hook=pairs, parse_constant=constant)


def read_json(path):
    with canonical(path).open("rb") as stream:
        raw = stream.read(MAX_JSON + 1)
    need(len(raw) <= MAX_JSON, "JSON limit exceeded")
    return decode(raw.decode("utf-8"))


def write_new(path, value):
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, indent=2, ensure_ascii=False, allow_nan=False)
        stream.write("\n")


def _utc_now():
    return datetime.now(timezone.utc).isoformat(timespec="microseconds")


def _utc(value):
    need(isinstance(value, str), "Missing UTC timestamp")
    result = datetime.fromisoformat(value)
    need(result.tzinfo is not None and result.utcoffset().total_seconds() == 0, "Timestamp must be UTC")
    return result


def _power_observation():
    """Read power policy without changing it; unsupported is an explicit fact."""
    if os.name == "nt":
        source = "windows:powercfg /getactivescheme"
        executable = Path(os.environ.get("SystemRoot", "C:/Windows")) / "System32/powercfg.exe"
        try:
            result = subprocess.run([str(executable), "/getactivescheme"], capture_output=True,
                                    timeout=5, check=False, creationflags=subprocess.CREATE_NO_WINDOW)
            match = re.search(rb"[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}", result.stdout)
            if result.returncode == 0 and match:
                return dict(status="OBSERVED", source=source, values={"scheme_guid": match[0].decode("ascii").lower()})
            reason = "Query failed or returned no scheme GUID"
        except (OSError, subprocess.SubprocessError):
            reason = "Power query unavailable or timed out"
        return dict(status="UNKNOWN", source=source, values={"reason": reason})
    if sys.platform.startswith("linux"):
        source = "linux:sysfs scaling_governor"
        governors = {}
        try:
            candidates = sorted(Path("/sys/devices/system/cpu").glob("cpu[0-9]*/cpufreq/scaling_governor"))
            need(len(candidates) <= 4096, "CPU governor query exceeds limit")
            for path in candidates:
                with path.open("r", encoding="ascii") as stream:
                    value = stream.read(257).strip()
                need(re.fullmatch(r"[a-zA-Z0-9_-]{1,128}", value), "Invalid CPU governor value")
                governors[path.parent.parent.name] = value
        except (OSError, UnicodeError, ValueError):
            return dict(status="UNKNOWN", source=source, values={"reason": "CPU governor query incomplete"})
        if governors:
            return dict(status="OBSERVED", source=source, values=governors)
        return dict(status="UNKNOWN", source=source, values={"reason": "No readable CPU scaling governors"})
    return dict(status="UNKNOWN", source="unsupported:power policy", values={"reason": "No power observer for this OS"})


def machine_observation():
    """Observe OS build, affinity and power; exclusivity remains a declaration."""
    processor = platform.processor()
    if not processor and sys.platform.startswith("linux"):
        text = Path("/proc/cpuinfo").read_text(encoding="utf-8")
        match = re.search(r"^model name\s*:\s*(.+)$", text, re.M)
        processor = match[1] if match else "NOT_AVAILABLE"
    affinity = "UNKNOWN"
    if hasattr(os, "sched_getaffinity"):
        affinity = sorted(os.sched_getaffinity(0))
    elif os.name == "nt":
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.GetCurrentProcess.restype = ctypes.c_void_p
        kernel.GetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t)]
        own, system = ctypes.c_size_t(), ctypes.c_size_t()
        if kernel.GetProcessAffinityMask(kernel.GetCurrentProcess(), ctypes.byref(own), ctypes.byref(system)):
            affinity = [bit for bit in range(own.value.bit_length()) if own.value & (1 << bit)]
    return dict(system=platform.system(), release=platform.release(), version=platform.version(), machine=platform.machine(),
                node=platform.node(), cpu_count=os.cpu_count(), processor=processor, affinity=affinity,
                power_mode=_power_observation())


def _git(repo, *args):
    need((repo / ".git").exists(), "Source must be a Git checkout")
    executable = shutil.which("git")
    need(executable is not None, "Git unavailable")
    env = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
    run = subprocess.run([str(Path(executable).resolve()), "--no-optional-locks", *args], cwd=repo,
                         env=env, capture_output=True, timeout=20, check=False)
    need(run.returncode == 0, "Git identity check failed: " + run.stderr.decode("utf-8", errors="replace")[:1000])
    return run.stdout


def _source(variant):
    repo = canonical(variant["source_dir"], directory=True)
    need(_git(repo, "rev-parse", "HEAD").decode().strip() == variant["measurement_source_sha"], "Wrong measurement source")
    need(not _git(repo, "status", "--porcelain", "--untracked-files=all"), "Dirty source is not formal benchmark input")
    production = variant["production_source_sha"]
    need(_git(repo, "rev-parse", production + "^{commit}").decode().strip() == production, "Unknown production commit")
    need(not _git(repo, "diff", "--no-ext-diff", "--no-textconv", production, variant["measurement_source_sha"], "--", "src/", "scripts/"),
         "Sampler overlay changed production src/scripts")
    sampler = variant["sampler_path"]
    need(sampler == "tests/cpp/test_perf_bench.cpp", "Unexpected sampler path")
    need(sha(repo / sampler) == variant["sampler_sha256"], "Sampler bytes mismatch")
    return {"source_dir": str(repo), "measurement_source_sha": variant["measurement_source_sha"],
            "production_source_sha": production, "sampler_sha256": variant["sampler_sha256"], "clean": True}


def _build(variant, fixture):
    binding = read_json(locked(variant["build_receipt"]))
    fields(binding, "schema configuration production_source_sha measurement_source_sha sampler_sha256 binary profile profile_name execution_receipt evidence", "build evidence")
    need(binding["schema"] == "caesura.benchmark-build.v1", "Wrong build schema")
    for key in ("configuration", "production_source_sha", "measurement_source_sha", "sampler_sha256", "binary"):
        need(binding[key] == variant[key], "Build binding mismatch: " + key)
    profile_path = locked(binding["profile"])
    run_path = locked(binding["execution_receipt"])
    evidence = fields(binding["evidence"], "root manifest_sha256", "U1 evidence")
    root = canonical(evidence["root"], directory=True)
    locked({"path": str(root / "manifest.json"), "sha256": evidence["manifest_sha256"]})
    profile = read_json(profile_path)["profiles"][binding["profile_name"]]
    run = read_json(run_path)
    need(profile["configuration"] == run["configuration"] == "Release", "Build must use Release configuration")
    need(run["source_sha"] == variant["measurement_source_sha"] and run["dirty"] is False, "U1 source mismatch or dirty")
    errors = verify_evidence(root, profile_path, binding["profile_name"], run_path,
                             source_sha=variant["measurement_source_sha"], release=not fixture)
    need(not errors, "Strict U1 verification failed: " + "; ".join(errors))
    checks = {row["id"]: row for row in run["checks"]}
    specs = {row["id"]: row for row in profile["checks"]}
    need(len(checks) == len(run["checks"]), "Duplicate U1 checks")
    for key in ("build", "cpp"):
        need(key in checks and key in specs and specs[key]["required"] is True,
             "Missing required U1 " + key)
        need(type(checks[key]["exit_code"]) is int and checks[key]["exit_code"] == 0,
             "U1 build/cpp did not succeed")
    need(specs["cpp"]["parser"] == "doctest", "U1 cpp must parse real doctest summary")
    if not fixture:
        need(profile.get("require_cmake_cache") is True, "Formal U1 must validate CMake configuration")
        need(specs["build"]["parser"] == "exit-code", "Wrong U1 build parser")
        command = checks["build"]["command"]
        need("--build" in command and Path(command[0]).stem.lower() == "cmake", "U1 did not execute a CMake build")
        cache = run["toolchain"].get("cmake_cache", {})
        configured = ("--config" in command and command.index("--config") + 1 < len(command)
                      and command[command.index("--config") + 1] == "Release")
        need(configured or ("--config" not in command and cache.get("CMAKE_BUILD_TYPE") == "Release"),
             "U1 build did not select Release")
    cpp = checks["cpp"]["binary"]
    actual_path = (run_path.parent / cpp["path"]).resolve(strict=True)
    need(actual_path == Path(variant["binary"]["path"]) and cpp["sha256"] == variant["binary"]["sha256"],
         "U1 cpp binary does not match selected executable")
    need(checks["cpp"]["command"][0] == str(actual_path), "U1 cpp command does not execute selected binary")
    if not fixture:
        need(checks["cpp"]["command"] == [str(actual_path), "--no-colors"], "Formal U1 cpp must be unfiltered")
    return {"binding": variant["build_receipt"], "manifest_sha256": evidence["manifest_sha256"],
            "receipt_sha256": binding["execution_receipt"]["sha256"], "purpose": run["purpose"]}


def _policy(value):
    fields(value, "schema comparison metrics", "policy")
    need(value["schema"] == "caesura.benchmark-policy.v1" and value["comparison"] ==
         "three_paired_process_medians_all_or_inconclusive", "Wrong policy schema/algorithm")
    need(isinstance(value["metrics"], dict) and set(value["metrics"]) == set(METRICS), "Wrong policy metrics")
    for key, row in value["metrics"].items():
        fields(row, "unit direction work_units median_method regression_fraction max_within_process_rmad max_between_process_spread max_order_drift min_sample_duration_ns", "metric policy")
        need(row["unit"] == "elapsed_ns_per_fixed_block" and row["direction"] == "lower"
             and row["median_method"] == "upper_middle", "Unknown metric policy")
        need(integer(row["work_units"], 1, 1000000, "work units") == METRICS[key][0], "Wrong work units")
        for field in ("regression_fraction", "max_within_process_rmad", "max_between_process_spread", "max_order_drift"):
            number(row[field], 0, 10, field)
        integer(row["min_sample_duration_ns"], 1, 10**12, "sample duration floor")
    return value


def _request(path, expected):
    req = read_json(locked({"path": str(path), "sha256": expected}))
    fields(req, "schema mode variants workload sampling machine policy timeouts limits", "request")
    need(req["schema"] == "caesura.benchmark-request.v1" and req["mode"] in ("compare", "baseline_only"), "Wrong request schema/mode")
    roles = {"base", "candidate"} if req["mode"] == "compare" else {"base"}
    need(isinstance(req["variants"], dict) and set(req["variants"]) == roles, "Wrong variant set")
    for variant in req["variants"].values():
        fields(variant, "production_source_sha measurement_source_sha sampler_sha256 source_dir sampler_path binary build_receipt cwd runtime_inputs configuration", "variant")
        for name in ("production_source_sha", "measurement_source_sha"):
            digest_string(variant[name], 40)
        digest_string(variant["sampler_sha256"])
        need(variant["configuration"] == "Release", "Release is required")
        locked(variant["binary"])
        canonical(variant["cwd"], directory=True)
        need(isinstance(variant["runtime_inputs"], list) and len(variant["runtime_inputs"]) <= 10000, "Invalid runtime input list")
        paths = [str(locked(ref)) for ref in variant["runtime_inputs"]]
        need(len(paths) == len(set(paths)), "Duplicate runtime input")
    need(len({v["sampler_sha256"] for v in req["variants"].values()}) == 1, "Sampler differs between variants")
    workload = fields(req["workload"], "schema path sha256 metric_ids seed rng", "workload")
    need(workload["schema"] == PROTOCOL and workload["metric_ids"] == list(METRICS)
         and type(workload["seed"]) is int and workload["seed"] == 0
         and workload["rng"] == "none_fixed_workload", "Wrong workload contract")
    manifest = read_json(locked({"path": workload["path"], "sha256": workload["sha256"]}))
    fields(manifest, "schema sampler_sha256 metric_ids seed rng state_policy", "workload manifest")
    need(all(manifest[key] == workload[key] for key in ("schema", "metric_ids", "seed", "rng")), "Workload manifest mismatch")
    need(manifest["sampler_sha256"] == next(iter(req["variants"].values()))["sampler_sha256"], "Workload sampler mismatch")
    need(isinstance(manifest["state_policy"], str) and 1 <= len(manifest["state_policy"]) <= 4096, "Missing fixed state/GC policy")
    sampling = fields(req["sampling"], "processes warmups samples schedule", "sampling")
    expected_schedule = SCHEDULE if req["mode"] == "compare" else ["base:0", "base:1", "base:2"]
    need(all(type(sampling[key]) is int and sampling[key] == count for key, count in
             (("processes", 3), ("warmups", 2), ("samples", 10))) and sampling["schedule"] == expected_schedule,
         "Sampling must use fixed 3x10, two warmups and AB/BA/AB order")
    fields(req["timeouts"], "per_process_seconds total_seconds", "timeouts")
    for value in req["timeouts"].values():
        number(value, .01, 86400, "timeout")
    fields(req["limits"], "stdout_bytes stderr_bytes json_line_bytes", "limits")
    for value in req["limits"].values():
        integer(value, 128, 64 * 1024 * 1024, "byte limit")
    machine_ref = fields(req["machine"], "manifest_path manifest_sha256 required_fields allowed_unknown_fields", "machine")
    machine = read_json(locked({"path": machine_ref["manifest_path"], "sha256": machine_ref["manifest_sha256"]}))
    fields(machine, "schema observed declared", "machine manifest")
    need(machine["schema"] == "caesura.benchmark-machine.v1", "Wrong machine schema")
    fields(machine["observed"], "system release version machine node cpu_count processor affinity power_mode", "machine observations")
    fields(machine["declared"], "concurrency", "machine declarations")
    power = fields(machine["observed"]["power_mode"], "status source values", "power observation")
    need(power["status"] in ("OBSERVED", "UNKNOWN") and isinstance(power["source"], str)
         and power["source"] and isinstance(power["values"], dict) and power["values"], "Invalid power observation")
    required = machine_ref["required_fields"]
    available = machine["observed"] | machine["declared"]
    need(isinstance(required, list) and all(isinstance(key, str) for key in required)
         and len(required) == len(set(required))
         and set(required) == set(available), "Missing required machine fields")
    need(all(available[key] not in (None, "", "NOT_AVAILABLE", "NOT_OBSERVED", []) for key in required), "Required machine fact unavailable")
    allowed = machine_ref["allowed_unknown_fields"]
    need(isinstance(allowed, list) and all(isinstance(key, str) for key in allowed)
         and len(allowed) == len(set(allowed)) and set(allowed) <= {"affinity", "power_mode"}, "Invalid allowed unknown machine fields")
    unknown = {key for key in ("affinity", "power_mode") if available[key] == "UNKNOWN"
               or (key == "power_mode" and power["status"] == "UNKNOWN")}
    need(unknown <= set(allowed), "Unknown machine observation was not permitted before execution")
    policy = _policy(read_json(locked(req["policy"])))
    return req, policy, machine


def parse_samples(text, run_uuid, workload_sha256, line_limit=16384):
    """Strictly reparse all raw records; no samples come from a cached summary."""
    need(str(uuid.UUID(run_uuid)) == run_uuid, "Invalid run UUID")
    digest_string(workload_sha256)
    records = []
    for line in text.splitlines():
        if line.startswith(PREFIX):
            need(len(line.encode("utf-8")) <= line_limit, "Protocol line exceeds limit")
            records.append(decode(line[len(PREFIX):]))
    need(len(records) == 38, "Expected header, 36 samples and footer")
    header = fields(records[0], "schema event run_uuid workload_sha256 seed rng warmups samples metrics", "sampler header")
    expected = dict(schema=PROTOCOL, event="header", run_uuid=run_uuid, workload_sha256=workload_sha256,
                    seed=0, rng="none_fixed_workload", warmups=2, samples=10, metrics=list(METRICS))
    need(header == expected and all(type(header[key]) is int for key in ("seed", "warmups", "samples")), "Sampler header mismatch")
    footer = fields(records[-1], "schema event run_uuid warmup_count measurement_count correctness_ok", "sampler footer")
    need(footer == dict(schema=PROTOCOL, event="footer", run_uuid=run_uuid, warmup_count=6,
                        measurement_count=30, correctness_ok=True)
         and type(footer["warmup_count"]) is int and type(footer["measurement_count"]) is int
         and footer["correctness_ok"] is True, "Sampler footer mismatch")
    grouped = {metric: {"warmup": [], "measurement": []} for metric in METRICS}
    skin_results = set()
    for row in records[1:-1]:
        need(isinstance(row, dict) and row.get("metric") in METRICS, "Unknown sample metric")
        extra = " result_sha256" if row["metric"] == "sma_skin_8192x10" else ""
        fields(row, "schema event run_uuid metric phase index elapsed_ns work_units correctness_ok observed_result" + extra, "sample")
        metric, phase = row["metric"], row["phase"]
        need(row["schema"] == PROTOCOL and row["event"] == "sample" and row["run_uuid"] == run_uuid, "Wrong sample identity")
        need(phase in ("warmup", "measurement"), "Wrong sample phase")
        values = grouped[metric][phase]
        need(integer(row["index"], 0, 9, "sample index") == len(values), "Duplicate or out-of-order sample index")
        need(len(values) < (2 if phase == "warmup" else 10), "Too many samples")
        if phase == "measurement":
            need(len(grouped[metric]["warmup"]) == 2, "Measurement occurred before warmups")
        else:
            need(not grouped[metric]["measurement"], "Warmup occurred after measurement")
        integer(row["elapsed_ns"], 1, 10**15, "sample duration")
        need(integer(row["work_units"], 1, 1000000, "sample units") == METRICS[metric][0], "Wrong sample work units")
        need(row["correctness_ok"] is True and integer(row["observed_result"], 0, 10**9, "observed result") == METRICS[metric][1], "Incorrect workload result")
        if extra:
            skin_results.add(digest_string(row["result_sha256"]))
        values.append(row["elapsed_ns"])
    need(all(len(row["warmup"]) == 2 and len(row["measurement"]) == 10 for row in grouped.values()), "Incomplete samples")
    need(len(skin_results) == 1, "Skinning correctness fingerprint changed")
    counts, _ = parse_doctest(text)
    need(counts["executed"] == counts["passed"] == 3 and counts["failed"] == 0, "Exactly three successful Perf tests required")
    return {"metrics": grouped, "skin_result_sha256": next(iter(skin_results)),
            "doctest": counts, "filtered_not_selected": counts["skipped"]}


def _fresh(value, sources=()):
    path = Path(value)
    need(path.is_absolute(), "Output must be absolute")
    canonical(path.parent, directory=True)
    need(not path.exists() and not path.is_symlink(), "Refusing existing output")
    need(not any(path.is_relative_to(root) for root in (ROOT, *sources)), "Output must be outside source checkout")
    return path


def _request_output(value, request_path):
    # Reject protected destinations before any catch-and-write failure path.
    # These preliminary path exclusions do not authenticate the request; the
    # complete externally hashed request is still verified inside the attempt.
    preliminary = read_json(canonical(request_path))
    sources = []
    if isinstance(preliminary, dict) and isinstance(preliminary.get("variants"), dict):
        for variant in preliminary["variants"].values():
            if isinstance(variant, dict) and isinstance(variant.get("source_dir"), str):
                sources.append(Path(variant["source_dir"]).resolve())
    return _fresh(value, sources)


def _stable_inputs(req):
    for variant in req["variants"].values():
        locked(variant["binary"])
        for item in variant["runtime_inputs"]:
            locked(item)


def _environment(work, variant, run_uuid, workload):
    env = {key: os.environ[key] for key in ("SystemRoot", "WINDIR") if key in os.environ}
    for name in ("home", "tmp"):
        (work / name).mkdir()
    env.update(HOME=str(work / "home"), USERPROFILE=str(work / "home"), TMP=str(work / "tmp"),
               TEMP=str(work / "tmp"), TMPDIR=str(work / "tmp"), PYTHONDONTWRITEBYTECODE="1",
               PATH=str(Path(variant["binary"]["path"]).parent), LANG="C.UTF-8", LC_ALL="C.UTF-8")
    env.update(CAESURA_BENCH_PROTOCOL=PROTOCOL, CAESURA_BENCH_RUN_UUID=run_uuid,
               CAESURA_BENCH_WORKLOAD_SHA256=workload["sha256"], CAESURA_BENCH_WARMUPS="2",
               CAESURA_BENCH_SAMPLES="10", CAESURA_BENCH_SEED="0")
    return env


def _run(argv, cwd, env, directory, timeout, limits):
    out_path, err_path = directory / "stdout.log", directory / "stderr.log"
    control = directory / "control"
    done, exceeded = threading.Event(), []
    def monitor():
        while not done.wait(.01):
            if any(p.exists() and p.stat().st_size > limits[key] for p, key in
                   ((out_path, "stdout_bytes"), (err_path, "stderr_bytes"))):
                exceeded.append("Process output byte limit exceeded")
                # This control directory is private to this exact invocation.
                # The helper terminates only its retained child, never a PID search.
                if control.is_dir():
                    try:
                        (control / "stop").open("x").close()
                    except FileExistsError:
                        pass
                return
    watcher = threading.Thread(target=monitor, name="benchmark-output-bound", daemon=True)
    with out_path.open("xb") as out, err_path.open("xb") as err:
        watcher.start()
        try:
            receipt = run_runtime_command(argv, cwd, env, control, out, err, timeout, stop_request="stop")
        finally:
            done.set()
            watcher.join()
    need(not exceeded, exceeded[0] if exceeded else "Output monitor failed")
    need(out_path.stat().st_size <= limits["stdout_bytes"] and err_path.stat().st_size <= limits["stderr_bytes"], "Process output byte limit exceeded")
    need(receipt["status"] == "EXITED" and type(receipt["actual_exit_code"]) is int
         and receipt["actual_exit_code"] == 0 and receipt["owned_tree_cleanup"] == "COMPLETE"
         and receipt["timed_out"] is False and receipt["forced_kill"] is False
         and receipt["stop_requested"] is False, "Owned benchmark process failed")
    return receipt


def collect_benchmarks(request_path, request_sha256, work_dir, *, _fixture_commands=None):
    """Collect once. Explicit injected miniature commands can only be fixtures."""
    work = _request_output(work_dir, request_path)
    work.mkdir()
    fixture = _fixture_commands is not None
    report = dict(schema="caesura.benchmark-collection.v1", request={"path": str(request_path), "sha256": request_sha256},
                  fixture=fixture, status="FAIL", execution_result="FAIL", processes=[], first_failure=None,
                  release_ready=False, engine_performance="NOT_EVALUATED", started_at=_utc_now())
    started = time.monotonic()
    try:
        req, _, machine = _request(request_path, request_sha256)
        need(not any(work.is_relative_to(Path(v["source_dir"])) for v in req["variants"].values()), "Work must be outside variant checkout")
        report["policy"] = req["policy"]
        report["machine_before"] = machine_observation()
        need(report["machine_before"] == machine["observed"], "Machine does not match locked manifest")
        report["sources"] = {key: _source(v) for key, v in req["variants"].items()}
        report["builds"] = {key: _build(v, fixture) for key, v in req["variants"].items()}
        if fixture:
            need(isinstance(_fixture_commands, dict) and set(_fixture_commands) == set(req["variants"]), "Fixture command roles mismatch")
        for index, slot in enumerate(req["sampling"]["schedule"]):
            role, pair = slot.split(":")
            variant = req["variants"][role]
            _stable_inputs(req)
            remaining = req["timeouts"]["total_seconds"] - (time.monotonic() - started)
            need(remaining > 0, "Total collection deadline exceeded")
            directory = work / f"{index:02d}-{role}-{pair}"
            directory.mkdir()
            run_uuid = str(uuid.uuid4())
            argv = list(_fixture_commands[role]) if fixture else [variant["binary"]["path"], "--test-case=Perf:*", "--no-colors"]
            need(argv and argv[0] == variant["binary"]["path"], "Command must use locked executable")
            env = _environment(directory, variant, run_uuid, req["workload"])
            row = dict(variant=role, pair_index=int(pair), schedule_index=index, run_uuid=run_uuid,
                       argv=argv, cwd=variant["cwd"], env_allowlist=env, binary_before=reference(argv[0]),
                       directory=str(directory), started_at=_utc_now())
            report["processes"].append(row)
            try:
                receipt = _run(argv, variant["cwd"], env, directory,
                               min(remaining, req["timeouts"]["per_process_seconds"]), req["limits"])
                row["process_identity"] = receipt["process"]
                row["parsed_samples"] = parse_samples((directory / "stdout.log").read_text(encoding="utf-8"),
                    run_uuid, req["workload"]["sha256"], req["limits"]["json_line_bytes"])
            finally:
                row["finished_at"] = _utc_now()
                for name, filename in (("stdout", "stdout.log"), ("stderr", "stderr.log"),
                                       ("run_receipt", "control/run.json"), ("runtime_request", "control/request.json")):
                    if (directory / filename).is_file():
                        row[name] = reference(directory / filename)
                row["binary_after"] = reference(argv[0])
            _stable_inputs(req)
        need(time.monotonic() - started <= req["timeouts"]["total_seconds"], "Total collection deadline exceeded")
        report["machine_after"] = machine_observation()
        need(report["machine_after"] == report["machine_before"], "Machine changed during collection")
        need(report["sources"] == {key: _source(v) for key, v in req["variants"].items()}, "Source changed during collection")
        need(report["builds"] == {key: _build(v, fixture) for key, v in req["variants"].items()}, "Build evidence changed")
        _request(request_path, request_sha256)
        report.update(status="FIXTURE_ONLY" if fixture else "COLLECTED", execution_result="PASS")
    except (ValueError, OSError, KeyError, TypeError, subprocess.SubprocessError) as error:
        report["first_failure"] = {"process_index": len(report["processes"]) - 1,
                                    "type": type(error).__name__, "message": str(error)}
    finally:
        report["elapsed_seconds"] = time.monotonic() - started
        report["finished_at"] = _utc_now()
        if _utc(report["finished_at"]) < _utc(report["started_at"]):
            report.update(status="FAIL", execution_result="FAIL")
            report["first_failure"] = report["first_failure"] or {"type":"ClockError", "message":"UTC clock moved backwards"}
        write_new(work / "collection.json", report)
    return report


def _median(values):
    return sorted(values)[len(values) // 2]


def evaluate_samples(processes, policy, mode):
    """Pure calculation, not a provenance or Engine performance verdict."""
    policy = _policy(policy)
    need(mode in ("compare", "baseline_only"), "Invalid comparison mode")
    metrics, conclusions = {}, []
    for metric, spec in policy["metrics"].items():
        by_role, reasons = {}, []
        for role in (("base", "candidate") if mode == "compare" else ("base",)):
            rows = sorted((row for row in processes if row["variant"] == role), key=lambda row: row["pair_index"])
            need([row["pair_index"] for row in rows] == [0, 1, 2], "Missing or duplicate paired processes")
            medians, rmads = [], []
            for row in rows:
                values = row["parsed_samples"]["metrics"][metric]["measurement"]
                need(len(values) == 10, "Ten raw measurements required")
                for value in values:
                    integer(value, 1, 10**15, "sample duration")
                median = _median(values)
                medians.append(median)
                rmads.append(1.4826 * _median([abs(value - median) for value in values]) / median)
                if min(values) < spec["min_sample_duration_ns"]:
                    reasons.append(role + ": sample below prelocked duration floor")
            spread = (max(medians) - min(medians)) / _median(medians)
            # Per-role chronological order, not pair index or value sorting.
            chronological = sorted(rows, key=lambda row: row["schedule_index"])
            chronological_medians = [_median(row["parsed_samples"]["metrics"][metric]["measurement"]) for row in chronological]
            drift = abs(chronological_medians[-1] - chronological_medians[0]) / chronological_medians[0]
            if max(rmads) > spec["max_within_process_rmad"]:
                reasons.append(role + ": within-process noise")
            if spread > spec["max_between_process_spread"]:
                reasons.append(role + ": between-process noise")
            if drift > spec["max_order_drift"]:
                reasons.append(role + ": chronological drift")
            by_role[role] = dict(medians=medians, relative_mads=rmads, spread=spread, drift=drift,
                                 chronological_medians=chronological_medians)
        ratios = []
        if reasons:
            result = "INCONCLUSIVE"
        elif mode == "baseline_only":
            result = "NOT_EVALUATED"
        else:
            ratios = [candidate / base for base, candidate in zip(by_role["base"]["medians"], by_role["candidate"]["medians"])]
            over = [ratio > 1 + spec["regression_fraction"] for ratio in ratios]
            result = "REGRESSION" if all(over) else "PASS" if not any(over) else "INCONCLUSIVE"
            if result == "INCONCLUSIVE":
                reasons.append("Paired medians disagree across tolerance")
        metrics[metric] = dict(processes=by_role, paired_ratios=ratios, comparison=result, reasons=reasons)
        conclusions.append(result)
    overall = "REGRESSION" if "REGRESSION" in conclusions else "INCONCLUSIVE" if "INCONCLUSIVE" in conclusions else "NOT_EVALUATED" if mode == "baseline_only" else "PASS"
    return {"metrics": metrics, "comparison": overall,
            "measurement_status": "INCONCLUSIVE" if "INCONCLUSIVE" in conclusions else "MEASURED"}


def compare_collection(request_path, request_sha256, collection_path, collection_sha256, output):
    output = _request_output(output, request_path)
    report = dict(schema="caesura.benchmark-comparison.v1", collection={"path": str(collection_path), "sha256": collection_sha256},
                  status="FAIL", execution_result="FAIL", measurement_status="NOT_MEASURED", comparison="INVALID",
                  gate_pass=False, release_ready=False, engine_performance="NOT_EVALUATED")
    try:
        req, policy, machine = _request(request_path, request_sha256)
        need(not any(output.is_relative_to(Path(v["source_dir"])) for v in req["variants"].values()), "Output must be outside source checkout")
        raw = read_json(locked(report["collection"]))
        need(raw["request"] == {"path": str(request_path), "sha256": request_sha256}, "Collection request mismatch")
        need(type(raw["fixture"]) is bool and raw["status"] == ("FIXTURE_ONLY" if raw["fixture"] else "COLLECTED")
             and raw["execution_result"] == "PASS" and raw["first_failure"] is None, "Collection did not succeed")
        need(raw["machine_before"] == raw["machine_after"] == machine["observed"], "Collection machine mismatch")
        need(raw["policy"] == req["policy"], "Collection policy mismatch")
        beginning, ending = _utc(raw["started_at"]), _utc(raw["finished_at"])
        need(beginning <= ending, "Invalid collection UTC interval")
        number(raw["elapsed_seconds"], 0, 86400, "collection elapsed seconds")
        need(raw["sources"] == {key: _source(v) for key, v in req["variants"].items()}, "Source no longer matches collection")
        need(raw["builds"] == {key: _build(v, raw["fixture"]) for key, v in req["variants"].items()}, "Build evidence no longer matches collection")
        rows = raw["processes"]
        need(len(rows) == len(req["sampling"]["schedule"]), "Incomplete collection")
        uuids, fingerprints = set(), set()
        for index, (slot, row) in enumerate(zip(req["sampling"]["schedule"], rows)):
            role, pair = slot.split(":")
            variant = req["variants"][role]
            need(beginning <= _utc(row["started_at"]) <= _utc(row["finished_at"]) <= ending, "Process UTC interval outside collection")
            need(row["variant"] == role and type(row["pair_index"]) is int and row["pair_index"] == int(pair)
                 and type(row["schedule_index"]) is int and row["schedule_index"] == index, "Wrong process ordering/pair")
            token = row["run_uuid"]
            need(str(uuid.UUID(token)) == token and token not in uuids, "Invalid or reused process UUID")
            uuids.add(token)
            need(row["binary_before"] == row["binary_after"] == variant["binary"], "Process binary binding mismatch")
            expected_argv = [variant["binary"]["path"], "--test-case=Perf:*", "--no-colors"]
            need(raw["fixture"] or row["argv"] == expected_argv, "Wrong benchmark argv")
            receipt = read_json(locked(row["run_receipt"]))
            runtime_request = read_json(locked(row["runtime_request"]))
            need(runtime_request["argv"] == row["argv"] and runtime_request["cwd"] == row["cwd"]
                 and runtime_request["env"] == row["env_allowlist"], "Raw runtime request mismatch")
            expected_environment = dict(CAESURA_BENCH_PROTOCOL=PROTOCOL, CAESURA_BENCH_RUN_UUID=token,
                CAESURA_BENCH_WORKLOAD_SHA256=req["workload"]["sha256"], CAESURA_BENCH_WARMUPS="2",
                CAESURA_BENCH_SAMPLES="10", CAESURA_BENCH_SEED="0")
            need(all(runtime_request["env"].get(key) == value for key, value in expected_environment.items()),
                 "Raw runtime sampler environment mismatch")
            need(receipt["argv"] == row["argv"] and receipt["cwd"] == row["cwd"] == variant["cwd"], "Owned receipt command/cwd mismatch")
            need(receipt["status"] == "EXITED" and type(receipt["actual_exit_code"]) is int
                 and receipt["actual_exit_code"] == 0 and receipt["owned_tree_cleanup"] == "COMPLETE"
                 and receipt["timed_out"] is False and receipt["forced_kill"] is False and receipt["stop_requested"] is False,
                 "Owned receipt did not succeed")
            need(row["process_identity"] == receipt["process"], "Process identity mismatch")
            stdout, stderr = locked(row["stdout"]), locked(row["stderr"])
            need(stdout.stat().st_size <= req["limits"]["stdout_bytes"] and stderr.stat().st_size <= req["limits"]["stderr_bytes"], "Output byte limit exceeded")
            parsed = parse_samples(stdout.read_text(encoding="utf-8"), token, req["workload"]["sha256"], req["limits"]["json_line_bytes"])
            need(parsed == row["parsed_samples"], "Cached samples differ from raw output")
            fingerprints.add(parsed["skin_result_sha256"])
        need(len(fingerprints) == 1, "Workload output differs across variants/processes")
        calculated = evaluate_samples(rows, policy, req["mode"])
        report.update(calculated, execution_result="PASS", policy_sha256=req["policy"]["sha256"],
                      fixture=raw["fixture"], status="FIXTURE_ONLY" if raw["fixture"] else "COMPARED",
                      started_at=raw["started_at"], finished_at=raw["finished_at"],
                      machine_unknowns=[key for key in ("affinity", "power_mode") if machine["observed"][key] == "UNKNOWN"
                          or (key == "power_mode" and machine["observed"][key]["status"] == "UNKNOWN")],
                      gate_pass=not raw["fixture"] and calculated["comparison"] == "PASS",
                      engine_performance="NOT_EVALUATED" if raw["fixture"] else calculated["comparison"])
        _stable_inputs(req)
        locked(report["collection"])
        _request(request_path, request_sha256)
        # Computation can be lengthy and its inputs remain ordinary files.
        # Reopen every declared source/build/raw lock before publishing a
        # verdict; earlier successful reads are not a stability guarantee.
        need(raw["sources"] == {key: _source(v) for key, v in req["variants"].items()}, "Source changed during comparison")
        need(raw["builds"] == {key: _build(v, raw["fixture"]) for key, v in req["variants"].items()}, "Build evidence changed during comparison")
        for row in rows:
            for role in ("stdout", "stderr", "runtime_request", "run_receipt"):
                locked(row[role])
    except (ValueError, OSError, KeyError, TypeError, subprocess.SubprocessError) as error:
        report.update(status="FAIL", execution_result="FAIL", measurement_status="NOT_MEASURED", comparison="INVALID",
                      gate_pass=False, engine_performance="NOT_EVALUATED", first_failure=str(error))
    write_new(output, report)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    for command in ("collect", "compare"):
        child = sub.add_parser(command)
        child.add_argument("--request", type=Path, required=True)
        child.add_argument("--request-sha256", required=True)
        if command == "collect":
            child.add_argument("--work", type=Path, required=True)
        else:
            child.add_argument("--collection", type=Path, required=True)
            child.add_argument("--collection-sha256", required=True)
            child.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "collect":
            result = collect_benchmarks(args.request, args.request_sha256, args.work)
        else:
            result = compare_collection(args.request, args.request_sha256, args.collection, args.collection_sha256, args.output)
        print(json.dumps(result, ensure_ascii=False, allow_nan=False))
        if result["execution_result"] != "PASS":
            return 1
        if result.get("fixture"):
            return 77
        return 0 if args.command == "collect" or result["gate_pass"] else 2
    except (ValueError, OSError) as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
