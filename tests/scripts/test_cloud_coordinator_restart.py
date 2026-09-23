"""Actual CAES coordinator cold recovery, with retained native child evidence.

The test-only cloud side is a second file read through the maintained typed
reader. This does not certify an HTTP/Steam backend, account or atomic CAS.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import signal
import sys
import tempfile
import time
import uuid


def physical_entries(root):
    """Exclude access time: reads may legitimately update it on either host."""
    result = {}
    for path in (root, *sorted(root.rglob("*"))):
        value = path.lstat()
        result[path.relative_to(root).as_posix()] = [value.st_dev, value.st_ino,
            value.st_size, value.st_mtime_ns, value.st_ctime_ns]
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--probe", required=True, type=Path)
    parser.add_argument("--attempt-root", required=True, type=Path)
    parser.add_argument("--fixture-root", type=Path, default=Path(tempfile.gettempdir()))
    args = parser.parse_args()
    source = args.source_root.resolve(strict=True)
    probe = args.probe.resolve(strict=True)
    attempts = args.attempt_root.resolve(strict=True)
    fixture_parent = args.fixture_root.resolve(strict=True)
    sys.path.insert(0, str(source / "tests/scripts"))
    from test_cloud_conflict_restart import require, identity, tree, write_json
    sys.path.insert(0, str(source / "scripts"))
    from package_runtime import run_runtime_command

    attempt = attempts / ("u26-coord-" + uuid.uuid4().hex)
    attempt.mkdir(mode=0o700)
    # Short, unique and retained root. Never delete or reuse a prior attempt;
    # the report binds this path and its physical identity, independent of cwd.
    fixtures = Path(tempfile.mkdtemp(prefix="uc-", dir=fixture_parent)).resolve(strict=True)
    st = fixtures.stat()
    report = dict(schema_version=1, status="RUNNING", attempt=str(attempt),
        fixture_root=str(fixtures), fixture_root_identity=[st.st_dev, st.st_ino], cases=[],
        scope="real CAES, real coordinator files/cursor, independent native processes; file-backed test cloud")
    paths = [source / name for name in (
        "src/storage/SaveManager.cpp", "src/storage/SaveManager.h",
        "src/storage/CloudCoordinatorState.cpp", "src/storage/CloudCoordinatorState.h",
        "src/storage/CloudConflictStore.cpp", "src/storage/CloudConflictStore.h",
        "src/storage/CloudSaveSnapshot.h",
        "src/storage/ISaveProvider.cpp", "src/storage/AtomicSaveFile.h",
        "src/storage/api/ICloudSaveCoordinator.h", "src/storage/api/ISaveManager.h",
        "src/storage/api/ICloudSaveSnapshotTransport.h", "src/storage/api/ISaveProvider.h",
        "src/archive/CryptoEngine.cpp", "src/archive/CryptoEngine.h",
        "scripts/package_runtime.py", "scripts/validation_process.py", "scripts/package_verification.py",
        "tests/probes/cloud_coordinator_restart_probe.cpp", "tests/scripts/test_cloud_conflict_restart.py")]
    paths.extend((probe, Path(__file__).resolve()))
    locks = {str(path): identity(path) for path in paths}
    report["inputs"] = locks
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", PYTHONIOENCODING="utf-8")
    child_count = 0
    process_ids = set()
    sessions = set()
    scope = "0123456789abcdef0123456789abcdef"
    token_a, token_b, export_token = (f"{n:032x}" for n in (1, 2, 3))
    no_reads = dict(local=0, cloud=0, legacy=0, writes=0)
    two_reads = dict(local=1, cloud=1, legacy=0, writes=0)

    def launch(case, mode, request, expected_exit=0, event_name="readback"):
        nonlocal child_count
        child_count += 1
        run = attempt / f"{child_count:02d}-{mode}"
        run.mkdir()
        write_json(run / "request.json", dict(schema_version=1, mode=mode, **request))
        begin = time.time_ns()
        with (run / "stdout").open("xb") as stdout, (run / "stderr").open("xb") as stderr:
            receipt = run_runtime_command([str(probe), "request.json"], run, env,
                run / "owned-control", stdout, stderr, 20)
        item = dict(mode=mode, started_ns=begin, completed_ns=time.time_ns(), receipt=receipt,
            artifact_dir=str(run), request=identity(run / "request.json"),
            stdout=identity(run / "stdout"), stderr=identity(run / "stderr"))
        case["children"].append(item)
        write_json(run / "receipt.json", item)
        require(receipt["status"] == "EXITED" and receipt["actual_exit_code"] == expected_exit,
            f"unexpected child outcome: {receipt['status']} / {receipt['actual_exit_code']}")
        require(not receipt["forced_kill"] and not receipt["timed_out"] and
            receipt["owned_tree_cleanup"] == "COMPLETE", "child cleanup incomplete")
        proc = receipt["process"]
        require(Path(proc["executable"]).resolve() == probe, "wrong executable ran")
        process_id = (proc["pid"], proc["created"])
        require(process_id not in process_ids, "native process identity reused")
        process_ids.add(process_id)
        prefix = "U26_COORD_EVENT "
        events = [json.loads(line[len(prefix):]) for line in (run / "stdout").read_text(encoding="utf-8").splitlines()
            if line.startswith(prefix)]
        require(len(events) == 1 and events[0]["event"] == event_name, "missing/extra boundary event")
        event = events[0]
        require(event["pid"] == proc["pid"], "event process differs from OS identity")
        if event.get("result", {}).get("session"):
            session = event["result"]["session"]
            require(session not in sessions, "coordinator session reused across processes")
            sessions.add(session)
        item["event"] = event
        return event

    def valid(r, ref, record, payload, base):
        require(r["code"] == "Replayed" and r["equal"] and r["preserved"], "cold receipt not complete")
        require(r["preparation"] == ref and r["record"] == record and r["base"] == base,
            "external receipt/record/base identity substituted")
        require(r["local"] == payload and r["cloud"] == payload and r["local_valid"] and r["cloud_valid"],
            "cold policy validation or original bytes differ")

    try:
        setup = dict(boundary="fixture-generation", children=[])
        report["setup"] = setup
        generated = launch(setup, "fixtures", dict(root=str(fixtures)), event_name="fixtures")
        inputs = {name: identity(fixtures / name / "save_3.json") for name in ("a", "b")}
        require(generated["generated"] == inputs and inputs["a"] != inputs["b"], "CAES fixtures are not distinct")
        for name in inputs:
            raw = (fixtures / name / "save_3.json").read_bytes()
            require(raw.startswith(b"CAES") and len(raw) > 70000, "fixture is not actual CAES")
        for ordinal, boundary in enumerate(("before-controlled", "before-abnormal", "after-normal",
                                            "after-abnormal", "after-hash-exception")):
            case = dict(boundary=boundary, children=[], status="RUNNING")
            report["cases"].append(case)
            base_dir = fixtures / str(ordinal)
            base_dir.mkdir()
            history, local_dir, cloud_path = base_dir / "h", base_dir / "l", base_dir / "cloud.caes"
            history.mkdir(); local_dir.mkdir()
            local_path = local_dir / "save_3.json"
            data_a, data_b = ((fixtures / n / "save_3.json").read_bytes() for n in ("a", "b"))
            local_path.write_bytes(data_a); cloud_path.write_bytes(data_a)
            common = dict(root=str(history), local_dir=str(local_dir), cloud_path=str(cloud_path))
            seed = launch(case, "seed", dict(**common, token=token_a), event_name="seed")
            seed_r = seed["result"]
            require(seed_r["code"] == "Complete" and seed_r["ancestor"] == "Selected" and
                seed_r["equal"] and seed_r["preserved"] and seed["counts"] == two_reads,
                "initial equal CAES ancestor not selected")
            context = history / scope / "1"
            cursor = context / "selected/slot_3.json"
            prior = tree(history)
            old_cursor = identity(cursor)
            require(old_cursor == seed["cursor"], "seed cursor event differs from actual file")
            local_path.write_bytes(data_b); cloud_path.write_bytes(data_b)
            endpoint_inputs = {str(p): identity(p) for p in (local_path, cloud_path)}
            before = boundary.startswith("before-")
            abnormal = boundary.endswith("abnormal")
            expected_exit = (74 if os.name == "nt" else -signal.SIGKILL) if abnormal else (73 if before else 0)
            written = launch(case, "write", dict(**common, token=token_b, boundary=boundary), expected_exit,
                "before-cursor" if before else "written")
            require(written["counts"] == two_reads, "write used unexpected reads or any endpoint writes")
            if before:
                reference = written["candidate"]["preparation"]
                candidate_record = written["candidate"]["record"]
                candidate_sha = written["candidate_cursor"]["sha256"]
                require(written["old_cursor"] == old_cursor and identity(cursor) == old_cursor,
                    "pre-Replace termination changed selected cursor")
            else:
                r = written["result"]
                reference, candidate_record, candidate_sha = r["preparation"], r["record"], r["candidate_cursor_sha256"]
                uncertain = boundary == "after-hash-exception"
                require(r["code"] == ("Indeterminate" if uncertain else "Complete") and
                    r["ancestor"] == ("Indeterminate" if uncertain else "Selected"), "wrong post-publication outcome")
                require(identity(cursor)["sha256"] == candidate_sha and identity(cursor) != old_cursor,
                    "post-publication candidate is not the actual selected cursor")
                if uncertain:
                    require(written["fault"] == dict(hits=1, delegated_real_sha=1, armed_count=1,
                        published_cursor_observed=True, input_equals_published_cursor=True), "fault was not proven exactly once")
            require(reference["token"] == token_b and candidate_record != seed_r["record"], "candidate identity reused")
            require(identity(context / "operations" / token_b / "receipt.json")["sha256"] == reference["receipt_sha256"],
                "external receipt identity differs from retained receipt")
            for ref, current, base in ((seed_r["record"], "a", None), (candidate_record, "b", "a")):
                record_dir = context / "records" / ref["id"]
                require(identity(record_dir / "manifest.json")["sha256"] == ref["manifest_sha256"],
                    "record manifest differs from external expected SHA")
                require(identity(record_dir / "local.bin") == inputs[current] and
                    identity(record_dir / "cloud.bin") == inputs[current], "raw preserved CAES differs from fixture")
                if base:
                    require(identity(record_dir / "base.bin") == inputs[base], "raw base provenance differs")
                else:
                    require(not (record_dir / "base.bin").exists(), "seed invented base payload")
            after_write = tree(history)
            after_write_physical = physical_entries(history)
            for mode, role in (("reopen", "candidate"), ("reopen", "base"), ("replay", "base"), ("replay", "candidate")):
                ref = reference if role == "candidate" else seed_r["preparation"]
                readback = launch(case, mode, dict(**common, preparation=ref))
                r = readback["result"]
                valid(r, ref, candidate_record if role == "candidate" else seed_r["record"],
                    inputs["b" if role == "candidate" else "a"], seed_r["record"] if role == "candidate" else None)
                expected = ("RecoveredNotSelected" if before else "RecoveredSelected") if role == "candidate" else (
                    "RecoveredSelected" if before else "Superseded")
                require(r["ancestor"] == expected, "cold recovery selected/promoted the wrong ancestor")
                require(readback["counts"] == no_reads, "cold read or duplicate token consulted live endpoints")
                listing = readback["list"]
                require(listing["code"] == "Complete" and listing["incomplete"] == 0 and listing["invalid"] == 0 and
                    sorted(listing["preparations"], key=lambda x:x["token"]) == [seed_r["preparation"], reference],
                    "cold list substituted or lost a complete receipt")
                require(tree(history) == after_write, "cold read or duplicate token modified history/cursor")
                require(physical_entries(history) == after_write_physical,
                    "cold read or duplicate token rewrote unchanged bytes or filesystem entries")
            bad_ref = dict(reference)
            bad_ref["receipt_sha256"] = ("0" if reference["receipt_sha256"][0] != "0" else "1") + reference["receipt_sha256"][1:]
            bad = launch(case, "reopen", dict(**common, preparation=bad_ref))
            require(bad["result"]["code"] == "InvalidInput" and bad["counts"] == no_reads, "wrong expected SHA accepted")
            require(tree(history) == after_write, "wrong expected SHA mutated storage")
            require(physical_entries(history) == after_write_physical, "wrong expected SHA rewrote storage")
            wrong_key = launch(case, "reopen", dict(**common, preparation=reference, wrong_key=True))
            require(wrong_key["result"]["code"] == "InvalidSave" and wrong_key["counts"] == no_reads,
                "new process trusted historical policy instead of authenticating CAES with current key")
            require(tree(history) == after_write, "current-policy refusal mutated storage")
            require(physical_entries(history) == after_write_physical, "current-policy refusal rewrote storage")
            export = launch(case, "export", dict(**common, preparation=reference, export_token=export_token), event_name="exported")
            require(export["code"] == "Complete" and export["bytes"] == inputs["b"] and export["counts"] == no_reads,
                "historical export changed bytes or read/wrote endpoints")
            export_path = context / "exports" / export_token / "save.bin"
            require(Path(export["path"]).resolve() == export_path.resolve() and identity(export_path) == inputs["b"],
                "export path or actual raw bytes differ")
            final_tree = tree(history)
            final_physical = physical_entries(history)
            require(all(final_tree[k] == v for k, v in after_write.items()), "export changed existing history")
            require(all(final_physical[k] == after_write_physical[k] for k,v in after_write.items()
                if v["kind"] == "file"), "export rewrote an existing history file")
            added = set(final_tree) - set(after_write)
            prefix = f"{scope}/1/exports/{export_token}"
            require(added == {prefix, prefix+"/request.json", prefix+"/save.bin", prefix+"/receipt.json"},
                "export mutated outside exclusive history copy")
            replay = launch(case, "export", dict(**common, preparation=reference, export_token=export_token), event_name="exported")
            require({k:v for k,v in replay.items() if k not in ("code", "pid")} ==
                {k:v for k,v in export.items() if k not in ("code", "pid")} and replay["code"] == "Replayed",
                "duplicate export changed receipt/path/bytes")
            require(tree(history) == final_tree, "duplicate export rewrote storage")
            require(physical_entries(history) == final_physical, "duplicate export rewrote identical bytes")
            require({str(p): identity(p) for p in (local_path, cloud_path)} == endpoint_inputs,
                "live endpoints changed during preparation/recovery/export")
            case.update(status="PASS", seed_preparation=seed_r["preparation"], candidate_preparation=reference,
                candidate_cursor_sha256=candidate_sha, before_tree=prior, writer_tree=after_write,
                final_tree=final_tree, writer_physical=after_write_physical, final_physical=final_physical,
                endpoints=endpoint_inputs, context_root=str(context))
        require(child_count == 51 and len(report["cases"]) == 5, "five-boundary native process matrix incomplete")
        require({n: identity(fixtures/n/"save_3.json") for n in inputs} == inputs, "CAES fixtures changed")
        require({str(p): identity(p) for p in paths} == locks, "source or probe changed during run")
        require([fixtures.stat().st_dev, fixtures.stat().st_ino] == report["fixture_root_identity"], "fixture root replaced")
        report.update(status="PASS", source_locks_stable=True, actual_child_count=child_count,
            actual_independent_process_identities=len(process_ids), fixture_inputs=inputs)
    except Exception as error:
        report.update(status="FAIL", error_type=type(error).__name__, error=str(error), actual_child_count=child_count)
    finally:
        write_json(attempt / "report.json", report)
        print(json.dumps(dict(status=report["status"], report=str(attempt/"report.json"),
            report_sha256=identity(attempt/"report.json")["sha256"])))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
