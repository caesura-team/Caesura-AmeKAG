#!/usr/bin/env python3
"""Run one locked U1 profile once and expose its strictly verified original bytes.

Workflow authentication and publication remain separate caller responsibilities.
This wrapper delegates execution, report parsing and acceptance to U1.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import sys

from ci_package_lane import _new_work, _outputs
from collect_validation_evidence import collect_evidence
from package_verification import _sha256_file, inspect_inventory
from run_validation import _source_identity, run_profile
from verify_release_candidate import verify_evidence

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = "caesura.execution-lane.v1"


def _need(condition, message):
    if not condition:
        raise ValueError(message)


def _digest(value, size=64):
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{" + str(size) + "}", value)


def guard_context(repo=ROOT, profile_file=None):
    """Check explicit local source/context claims; the aggregate authenticates API identity."""
    repo = Path(repo).resolve(strict=True)
    profile = Path(profile_file or repo / "scripts/validation_profiles.json")
    env = os.environ
    _need(env.get("GITHUB_ACTIONS") == "true", "Controlled GitHub producer context required")
    for key in ("SOURCE_SHA", "TRIGGER_HEAD_SHA", "CALLER_WORKFLOW_SHA", "CALLED_WORKFLOW_SHA"):
        _need(_digest(env.get(key), 40), "Invalid " + key)
    for key in ("POLICY_SHA256", "PROFILE_SHA256"):
        _need(_digest(env.get(key)), "Invalid " + key)
    _need(env.get("SOURCE_MODE") == "head" and env["SOURCE_SHA"] == env["TRIGGER_HEAD_SHA"],
          "Producer requires the explicit trigger head source")
    trigger = env.get("GITHUB_SHA")
    if env.get("GITHUB_EVENT_PATH"):
        event_path = Path(env["GITHUB_EVENT_PATH"])
        _need(event_path.stat().st_size <= 16 * 1024 * 1024, "Event payload exceeds limit")
        event = json.loads(event_path.read_text(encoding="utf-8"))
        if env.get("GITHUB_EVENT_NAME") == "pull_request":
            trigger = event["pull_request"]["head"]["sha"]
            _need(env.get("PULL_REQUEST_NUMBER") == str(event["number"]), "PR selection differs from trigger")
    _need(trigger == env["TRIGGER_HEAD_SHA"], "Trigger head differs from caller event")
    ref = env.get("CALLER_WORKFLOW_REF", "")
    path = env.get("CALLER_WORKFLOW_PATH", "")
    _need(re.fullmatch(r"\.github/workflows/[A-Za-z0-9_.-]+\.ya?ml", path)
          and ref.startswith(env.get("GITHUB_REPOSITORY", "") + "/" + path + "@"), "Caller workflow path mismatch")
    _need(ref == env.get("GITHUB_WORKFLOW_REF")
          and env["CALLER_WORKFLOW_SHA"] == env.get("GITHUB_WORKFLOW_SHA"), "Caller workflow identity mismatch")
    _need(env.get("CALLED_WORKFLOW_PATH") == ".github/workflows/validate-engine.yml"
          and env["CALLED_WORKFLOW_SHA"] == env["CALLER_WORKFLOW_SHA"], "Relative called workflow identity mismatch")
    identity = _source_identity(repo)
    _need(identity["source_sha"] == env["SOURCE_SHA"] and not identity["dirty"], "Wrong or dirty source checkout")
    _need(_sha256_file(profile) == env["PROFILE_SHA256"], "Locked profile digest mismatch")
    if env.get("EXPECTED_RUNNER_ARCH"):
        _need(env.get("RUNNER_ARCH") == env["EXPECTED_RUNNER_ARCH"], "Runner architecture differs from package policy")
    if env.get("ENGINE_VERSION"):
        versions = re.findall(r"(?im)^\s*project\(\s*CaesuraAmeKAG\s+VERSION\s+(\d+\.\d+\.\d+(?:\.\d+)?)\s+LANGUAGES\b",
                              (repo / "CMakeLists.txt").read_text(encoding="utf-8-sig"))
        _need(versions == [env["ENGINE_VERSION"]], "Locked engine version mismatch")
    return identity


def _stable(report):
    _need(_source_identity(Path(report["repo"])) == report["source_before"], "Source changed before execution upload")
    _need(_sha256_file(Path(report["profile_file"])) == report["profile_sha256"], "Profile changed before execution upload")
    _need(_sha256_file(Path(report["raw_receipt"])) == report["receipt_sha256"], "Original receipt changed before upload")
    _need(inspect_inventory(report["bundle_dir"])["sha256"] == report["bundle_sha256"], "Execution bundle changed before upload")
    errors = verify_evidence(Path(report["bundle_dir"]), Path(report["profile_file"]), report["profile_name"],
                             Path(report["raw_receipt"]), source_sha=report["source_sha"], release=True)
    _need(not errors, "Strict U1 verification FAIL: " + "; ".join(errors))


def run_execution_lane(*, repo, profile_file, profile_sha256, profile_name, source_sha,
                       build_dir, configuration, work_dir):
    repo, profile_file = Path(repo).resolve(strict=True), Path(profile_file).resolve(strict=True)
    work = _new_work(work_dir)
    receipt = work / "lane.json"
    report = {"schema":SCHEMA, "status":"FAIL", "release_ready":False,
        "hosted_provenance":"CALLER_MUST_AUTHENTICATE", "repo":str(repo), "source_sha":source_sha,
        "profile_file":str(profile_file), "profile_sha256":profile_sha256, "profile_name":profile_name,
        "configuration":configuration, "lane_receipt":str(receipt), "errors":[]}
    try:
        _need(not work.is_relative_to(repo) and not repo.is_relative_to(work), "Execution work must be outside source")
        _need(_digest(source_sha, 40) and _digest(profile_sha256), "Full source/profile digests required")
        _need(_sha256_file(profile_file) == profile_sha256, "Locked profile digest mismatch")
        before = _source_identity(repo)
        report["source_before"] = before
        _need(before["source_sha"] == source_sha and not before["dirty"], "Wrong or dirty source checkout")
        # run_profile owns each subprocess and records all first results, even
        # when a required check fails. There is no fallback run or diagnostic mode.
        run = run_profile(repo=repo, profile_file=profile_file, profile_name=profile_name,
            build_dir=Path(build_dir), configuration=configuration, run_dir=work / "raw")
        raw = work / "raw/run.json"
        bundle = work / "bundle" / source_sha / run["run_id"] / profile_name
        report.update(raw_receipt=str(raw), receipt_sha256=_sha256_file(raw), run_uuid=run["run_id"], bundle_dir=str(bundle))
        collect_evidence(profile_file, profile_name, raw, bundle)
        report.update(manifest_sha256=_sha256_file(bundle / "manifest.json"),
                      bundle_sha256=inspect_inventory(bundle)["sha256"])
        _stable(report)
        report["status"] = "EXECUTION_UPLOAD_READY"
        return report
    except (OSError, ValueError) as error:
        report["errors"].append(str(error))
        raise
    finally:
        with receipt.open("x", encoding="utf-8", newline="\n") as stream:
            json.dump(report, stream, ensure_ascii=False, indent=2)
            stream.write("\n")


def verify_lane(receipt_path, receipt_sha256):
    path = Path(receipt_path).resolve(strict=True)
    _need(_digest(receipt_sha256) and _sha256_file(path) == receipt_sha256, "Execution lane receipt digest mismatch")
    _need(path.stat().st_size <= 1024 * 1024, "Execution lane receipt exceeds limit")
    report = json.loads(path.read_text(encoding="utf-8"))
    _need(report.get("schema") == SCHEMA and report.get("status") == "EXECUTION_UPLOAD_READY", "Execution lane is not accepted")
    _stable(report)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    guard = commands.add_parser("guard")
    guard.add_argument("--repo", type=Path, default=ROOT)
    run = commands.add_parser("run")
    run.add_argument("--repo", type=Path, default=ROOT)
    run.add_argument("--profile", type=Path, default=ROOT / "scripts/validation_profiles.json")
    for name in ("profile-name", "profile-sha256", "source-sha", "configuration"):
        run.add_argument("--" + name, required=True)
    run.add_argument("--build", type=Path, required=True)
    run.add_argument("--work", type=Path, required=True)
    run.add_argument("--github-output", type=Path)
    verify = commands.add_parser("verify")
    verify.add_argument("--receipt", type=Path, required=True)
    verify.add_argument("--sha256", required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "guard":
            guard_context(args.repo)
        elif args.command == "verify":
            verify_lane(args.receipt, args.sha256)
        else:
            report = run_execution_lane(repo=args.repo, profile_file=args.profile, profile_name=args.profile_name,
                profile_sha256=args.profile_sha256, source_sha=args.source_sha, configuration=args.configuration,
                build_dir=args.build, work_dir=args.work)
            _outputs(args.github_output, {key:report[key] for key in
                ("bundle_dir", "manifest_sha256", "receipt_sha256", "run_uuid", "profile_sha256", "lane_receipt")} |
                {"lane_sha256":_sha256_file(Path(report["lane_receipt"]))})
        return 0
    except (OSError, ValueError) as error:
        print("CI execution lane FAIL: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
