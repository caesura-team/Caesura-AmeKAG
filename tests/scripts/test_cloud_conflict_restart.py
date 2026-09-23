"""Cold-process conflict-store regression with retained owned-process evidence."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import stat
import sys
import time
import uuid


def require(value, message):
    if not value:
        raise AssertionError(message)


def sha_bytes(data):
    return hashlib.sha256(data).hexdigest()


def identity(path):
    value = Path(path).lstat()
    require(stat.S_ISREG(value.st_mode) and not Path(path).is_symlink(), f"not an ordinary file: {path}")
    require(not getattr(value, "st_file_attributes", 0) & 0x400, f"reparse file: {path}")
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    after = Path(path).stat()
    require((value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns) ==
            (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns), "artifact changed while hashing")
    return {"size": value.st_size, "sha256": h.hexdigest()}


def write_json(path, value):
    with Path(path).open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2)
        stream.write("\n")


def tree(root):
    result = {}
    for path in sorted(root.rglob("*")):
        item = path.lstat()
        require(not path.is_symlink() and not getattr(item, "st_file_attributes", 0) & 0x400,
                "record tree contains a link/reparse entry")
        key = path.relative_to(root).as_posix()
        if stat.S_ISDIR(item.st_mode):
            result[key] = {"kind": "directory"}
        else:
            result[key] = {"kind": "file", **identity(path)}
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--probe", required=True, type=Path)
    parser.add_argument("--attempt-root", required=True, type=Path)
    args = parser.parse_args()
    source = args.source_root.resolve(strict=True)
    probe = args.probe.resolve(strict=True)
    attempts = args.attempt_root.resolve(strict=True)
    require(source.is_dir() and attempts.is_dir(), "existing source and evidence roots required")
    attempt = attempts / ("u26-b1b-" + uuid.uuid4().hex)
    attempt.mkdir(mode=0o700)  # Never reuse or remove an old attempt.
    report = {"schema_version": 1, "status": "RUNNING", "attempt": str(attempt), "cases": []}
    sources = [source / name for name in (
        "src/storage/CloudConflictStore.cpp", "src/storage/CloudConflictStore.h",
        "src/storage/ISaveProvider.cpp", "src/storage/AtomicSaveFile.h",
        "src/storage/api/ICloudSaveSnapshotTransport.h", "src/archive/CryptoEngine.cpp",
        "scripts/package_runtime.py", "scripts/validation_process.py", "scripts/package_verification.py",
        "tests/probes/cloud_conflict_restart_probe.cpp")]
    sources.extend((probe, Path(__file__).resolve()))
    locks = {str(path): identity(path) for path in sources}
    report["inputs"] = locks
    sys.path.insert(0, str(source / "scripts"))
    from package_runtime import run_runtime_command
    environment = dict(os.environ)
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    environment["PYTHONIOENCODING"] = "utf-8"
    index = 0

    def launch(case, mode, request, expected_exit, expected_event):
        nonlocal index
        index += 1
        run = attempt / (f"{index:02d}-" + case["boundary"] + "-" + mode)
        run.mkdir()
        write_json(run / "request.json", {"schema_version": 1, **request, "mode": mode})
        begin = time.time_ns()
        with (run / "stdout").open("xb") as stdout, (run / "stderr").open("xb") as stderr:
            receipt = run_runtime_command([str(probe), "request.json"], run, environment,
                run / "owned-control", stdout, stderr, 20)
        record = {"mode": mode, "started_ns": begin, "completed_ns": time.time_ns(),
                  "receipt": receipt, "request": identity(run / "request.json"),
                  "stdout": identity(run / "stdout"), "stderr": identity(run / "stderr"),
                  "artifact_dir": str(run)}
        case["children"].append(record)
        write_json(run / "receipt.json", record)
        require(receipt["status"] == "EXITED", "child did not exit normally from the controller's view")
        require(receipt["actual_exit_code"] == expected_exit, f"unexpected child exit: {receipt['actual_exit_code']}")
        require(not receipt["forced_kill"] and not receipt["timed_out"] and
                receipt["owned_tree_cleanup"] == "COMPLETE", "child ownership/cleanup incomplete")
        raw = (run / "stdout").read_text(encoding="utf-8")
        events = [json.loads(line[len("U26_B1B_EVENT "):]) for line in raw.splitlines()
                  if line.startswith("U26_B1B_EVENT ")]
        require(len(events) == 1 and events[0]["event"] == expected_event, "missing or extra process-boundary event")
        require(events[0]["pid"] == receipt["process"]["pid"], "event PID differs from owned OS process")
        require(Path(receipt["process"]["executable"]).resolve() == probe, "wrong executable ran")
        record["event"] = events[0]
        return events[0], receipt

    try:
        for boundary in ("before-controlled", "before-abnormal", "after-normal", "after-abnormal", "after-hash-exception"):
            case = {"boundary": boundary, "children": [], "status": "RUNNING"}
            report["cases"].append(case)
            case_root = attempt / boundary
            case_root.mkdir()
            records = case_root / "records"
            records.mkdir()
            blobs = {"base": b"opaque\x00A\x00" + b"A" * 70000,
                     "local": b"opaque\x00B\x00" + b"B" * 70000,
                     "cloud": b"opaque\x00C\x00" + b"C" * 70000}
            fixture_paths = {}
            for role, data in blobs.items():
                path = case_root / (role + ".fixture")
                with path.open("xb") as output:
                    output.write(data)
                fixture_paths[role] = str(path)
            fixtures_before = {role: identity(path) for role, path in fixture_paths.items()}
            seed, _ = launch(case, "seed", {"root": str(records), "local": fixture_paths["base"],
                "cloud": fixture_paths["base"]}, 0, "seed")
            require(seed["result"]["code"] == "Preserved", "seed was not preserved")
            base_ref = seed["result"]["record"]["ref"]
            prior = tree(records)
            is_before = boundary.startswith("before-")
            is_indeterminate = boundary == "after-hash-exception"
            abnormal = boundary.endswith("-abnormal")
            expected_exit = (74 if os.name == "nt" else -signal.SIGKILL) if abnormal else (73 if is_before else 0)
            event, writer = launch(case, "write", {"root": str(records), **fixture_paths,
                "base_ref": base_ref, "boundary": boundary}, expected_exit,
                "precommit_boundary" if is_before else "indeterminate" if is_indeterminate else "committed")
            if is_before:
                ref = event["candidate_ref"]
                require(event["role"] == 4 and event["stage"] == "Replace" and
                        event["payloads_match"] and not event["manifest_exists"], "precommit boundary not proven")
            elif is_indeterminate:
                result = event["result"]
                require(result["code"] == "Indeterminate" and result["record"] is None,
                        "postcommit exception did not yield an unusable Indeterminate result")
                ref = result["candidate_ref"]
                require(ref is not None and result["operation_id"] == ref["id"],
                        "Indeterminate omitted or changed operation identity")
                fault = event["fault"]
                require(fault["opened_roles"] == 4 and fault["armed_count"] == 1 and fault["fired"] == 1 and
                        fault["delegated_armed_calls"] == 1 and not fault["armed_after"],
                        "backend fault did not occur exactly once at the manifest boundary")
                require(fault["published_manifest_observed"] and fault["input_equals_raw_manifest"] and
                        fault["real_digest_matches_prepared"] and fault["prepared_ref"] == ref,
                        "postcommit file/input/real-digest chain was not proven")
                require(event["termination"] == "normal_return", "unexpected Indeterminate writer termination")
                same_process = event["same_process_reopen"]
                require(same_process["code"] == "Complete" and same_process["record"] is not None,
                        "same-object active-call guard did not recover")
                same_record = same_process["record"]
                require(same_record["ref"] == ref and same_record["kind"] == "Conflict" and
                        same_record["base_ref"] == base_ref, "same-object recovery changed record identity")
                for payload_role in ("base", "local", "cloud"):
                    require(same_record[payload_role] == fixtures_before[payload_role],
                            "same-object recovery changed payload bytes")
            else:
                require(event["result"]["code"] == "Preserved", "writer was not fully preserved")
                ref = event["result"]["candidate_ref"]
                require(ref == event["result"]["record"]["ref"], "published reference differs")
            require(ref["id"] != base_ref["id"], "writer reused prior record")
            directory = records / ref["id"]
            for role, data in blobs.items():
                require((directory / (role + ".bin")).read_bytes() == data, "preserved payload differs")
            require((directory / "manifest.json").exists() != is_before, "wrong manifest publication state")
            after_write = tree(records)
            require(all(after_write.get(name) == value for name, value in prior.items()), "prior record changed")
            if is_before:
                temps = list(directory.glob(".caesura-save-tmp-*"))
                require(len(temps) == 1, "expected one uncommitted manifest temporary")
                require(identity(temps[0])["sha256"] == ref["manifest_sha256"], "prepared reference differs from retained bytes")
            else:
                require(identity(directory / "manifest.json")["sha256"] == ref["manifest_sha256"], "published manifest differs")
                require(not list(directory.glob(".caesura-save-tmp-*")), "successful writer retained temporary")

            # Both readers are independently exec'd after writer termination.
            for role, reference in (("base", base_ref), ("candidate", ref)):
                before_read = tree(records)
                readback, reader = launch(case, "read", {"root": str(records), "ref": reference}, 0, "readback")
                require((writer["process"]["pid"], writer["process"]["created"]) !=
                        (reader["process"]["pid"], reader["process"]["created"]), "reader is not a new process")
                expected_code = "Incomplete" if is_before and role == "candidate" else "Complete"
                result = readback["result"]
                require(result["code"] == expected_code, "cold read returned wrong record state")
                require((result["record"] is None) == (expected_code == "Incomplete"), "cold read exposed invalid record")
                if result["record"]:
                    record = result["record"]
                    require(record["ref"] == reference, "reader substituted external reference")
                    require(record["kind"] == ("EqualObserved" if role == "base" else "Conflict"),
                            "cold-read classification differs")
                    for payload_role in ("local", "cloud"):
                        require(record[payload_role] == fixtures_before["base" if role == "base" else payload_role],
                                "cold-read payload identity differs")
                    require(record["base"] == (None if role == "base" else fixtures_before["base"]), "cold-read base differs")
                    require(record["base_ref"] == (None if role == "base" else base_ref), "cold-read provenance differs")
                listing = readback["list"]
                expected_refs = [base_ref] if is_before else [base_ref, ref]
                require(listing["code"] == "Complete" and listing["incomplete"] == int(is_before) and
                        listing["invalid"] == 0 and sorted(listing["complete"], key=lambda r:r["id"]) ==
                        sorted(expected_refs, key=lambda r:r["id"]), "cold list chose or promoted an invalid record")
                require(tree(records) == before_read, "cold reader modified record namespace")
            if not is_before:
                bad_ref = dict(ref)
                bad_ref["manifest_sha256"] = ("0" if ref["manifest_sha256"][0] != "0" else "1") + ref["manifest_sha256"][1:]
                before_read = tree(records)
                bad, _ = launch(case, "read", {"root": str(records), "ref": bad_ref}, 0, "readback")
                require(bad["result"]["code"] == "InvalidRecord" and bad["result"]["record"] is None,
                        "cold reader ignored externally expected manifest hash")
                require(tree(records) == before_read, "negative reader modified record namespace")
            require({role: identity(path) for role, path in fixture_paths.items()} == fixtures_before,
                    "input fixtures changed")
            case.update(status="PASS", base_ref=base_ref, candidate_ref=ref, initial_tree=prior,
                        writer_tree=after_write, final_tree=tree(records), fixtures=fixtures_before)
        require(index == 23 and len(report["cases"]) == 5, "fixed five-scenario process matrix was incomplete")
        require({str(path): identity(path) for path in sources} == locks, "source or binary changed during run")
        report.update(status="PASS", source_locks_stable=True, actual_child_count=index)
    except Exception as error:
        report.update(status="FAIL", error_type=type(error).__name__, error=str(error), actual_child_count=index)
    finally:
        write_json(attempt / "report.json", report)
        print(json.dumps({"status": report["status"], "report": str(attempt / "report.json"),
                          "report_sha256": identity(attempt / "report.json")["sha256"]}))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
