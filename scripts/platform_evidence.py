#!/usr/bin/env python3
"""Recheck caller-selected U1 reports and U22 package bytes for one candidate.

This read-only adapter performs no execution or hosted authentication. Selection
and its digest are supplied by the controlled caller, never discovered from a
bundle. A package receipt check does not replay its original runtime logs. The
default document generator must not promote a saved report to a new verification.
"""
from __future__ import annotations

from pathlib import Path
import re

from collect_validation_evidence import build_manifest, digest
from package_verification import PackageVerificationError
from verify_execution_bundle import _no_links, _snapshot, _tree_lock
from verify_package_bundle import verify_bundle, verify_bundle_stable
from verify_release_candidate import verify_evidence


SELECTION_SCHEMA = "caesura.platform-evidence-selection.v1"
REPORT_SCHEMA = "caesura.platform-evidence-verification.v1"
COMMON_FIELDS = {"id", "kind", "platform", "configuration", "bundle", "manifest_sha256"}
EXECUTION_FIELDS = COMMON_FIELDS | {
    "expected_run", "receipt_sha256", "profile_name", "profile_sha256", "context", "expected_cache",
}
PACKAGE_FIELDS = COMMON_FIELDS | {"version", "producer", "expected_files"}
CONTEXT_FIELDS = {"run_id", "run_attempt", "repository", "workflow"}


def _need(condition, message):
    if not condition:
        raise ValueError(message)


def _object(value, keys, name):
    _need(type(value) is dict and set(value) == keys, f"Invalid {name} fields")


def _text(value, name):
    _need(type(value) is str and 0 < len(value) <= 1024
          and not any(ord(char) < 32 or ord(char) == 127 for char in value), f"Invalid {name}")


def _relative(value):
    _text(value, "evidence path")
    _need(not value.startswith("/") and ":" not in value and "\\" not in value
          and all(part not in ("", ".", "..") for part in value.split("/")),
          "Evidence path must use contained relative POSIX components")
    return value


def _path(root, value, *, directory=False):
    value = _relative(value)
    path = _no_links(root / value)
    _need(path.is_relative_to(root), "Evidence path escapes the selected root")
    _need(path.is_dir() if directory else path.is_file(), "Missing evidence directory/file: " + value)
    _need(directory or path.stat().st_nlink == 1, "Hardlinked evidence file is not supported")
    return path


def _locked_json(path, expected, name, locks):
    _, value = _snapshot(path, expected, name)
    locks.append(dict(path=str(path), kind="file", sha256=expected))
    return value


def _stable(locks):
    for lock in locks:
        path = _no_links(lock["path"])
        actual = _tree_lock(path)["sha256"] if lock["kind"] == "directory" else digest(path)
        _need(actual == lock["sha256"], "Verified input changed before consumption: " + str(path))


def _execution(claim, *, root, profile_path, source_sha, diagnostic, locks):
    _object(claim, EXECUTION_FIELDS, "execution claim")
    _object(claim["context"], CONTEXT_FIELDS, "external execution context")
    for name, value in claim["context"].items():
        if name == "run_attempt":
            _need(type(value) is int and value > 0, "Invalid external run_attempt")
        else:
            _text(value, "external " + name)
    _text(claim["profile_name"], "profile name")
    _need(type(claim["expected_cache"]) is dict, "Expected cache must be a mapping")
    for key, value in claim["expected_cache"].items():
        _text(key, "cache key")
        _text(value, "cache value")
    bundle = _path(root, claim["bundle"], directory=True)
    run_path = _path(root, claim["expected_run"])
    profile = _no_links(profile_path)
    _need(not run_path.is_relative_to(bundle), "Expected receipt must be outside the bundle")
    _need(not profile.is_relative_to(bundle), "Trusted profile must be outside the bundle")
    locks.append(_tree_lock(bundle))
    _locked_json(bundle / "manifest.json", claim["manifest_sha256"], "manifest", locks)
    receipt = _locked_json(run_path, claim["receipt_sha256"], "receipt", locks)
    _locked_json(profile, claim["profile_sha256"], "profile", locks)
    expected_context = dict(claim["context"], platform=claim["platform"], configuration=claim["configuration"])
    _need(receipt.get("source_sha") == source_sha, "Execution source differs from selected current source")
    for name, value in expected_context.items():
        _need(type(receipt.get(name)) is type(value) and receipt.get(name) == value,
              "Execution context mismatch: " + name)
    cache = receipt.get("toolchain", {}).get("cmake_cache", {})
    _need(type(cache) is dict, "Invalid recorded CMake cache")
    for key, value in claim["expected_cache"].items():
        _need(cache.get(key) == value, "Recorded cache does not match selected configuration: " + key)
    errors = verify_evidence(bundle, profile, claim["profile_name"], run_path,
                             source_sha=source_sha, release=not diagnostic)
    _need(not errors, "U1 evidence verification failed: " + "; ".join(errors))
    manifest, _ = build_manifest(profile, claim["profile_name"], run_path, collected_root=bundle)
    _need(manifest["result"] == "PASS", "Required execution profile did not pass")
    return dict(result="PASS", verification_scope="SELECTED_EXECUTION_PROFILE",
        raw_stage_evidence="ORIGINAL_REPORTS_REPARSED", profile_name=claim["profile_name"],
        profile_sha256=claim["profile_sha256"], receipt_sha256=claim["receipt_sha256"],
        manifest_sha256=claim["manifest_sha256"], run_id=receipt["run_id"],
        run_attempt=receipt["run_attempt"], source_sha=source_sha,
        purpose=receipt["purpose"], expected_cache=dict(claim["expected_cache"]),
        checks=[{key:check.get(key) for key in ("id", "result", "required", "counts", "skipped_tests")}
                for check in manifest["checks"]],
        not_run=["unselected_profiles", "unselected_feature_or_device_runtime"], errors=[])


def _package(claim, *, root, source_sha, locks):
    _object(claim, PACKAGE_FIELDS, "package receipt claim")
    files = claim["expected_files"]
    _need(type(files) is dict and bool(files), "Expected final package files must be nonempty")
    for name, expected in files.items():
        _relative(name)
        _object(expected, {"kind", "sha256"}, "expected package file")
        _need(expected["kind"] in ("file", "directory"), "Invalid expected package kind")
        _need(type(expected["sha256"]) is str and re.fullmatch("[0-9a-f]{64}", expected["sha256"]),
              "Invalid expected package digest")
    bundle = _path(root, claim["bundle"], directory=True)
    locks.append(_tree_lock(bundle))
    result = verify_bundle(bundle, manifest_sha256=claim["manifest_sha256"], source_sha=source_sha,
        platform=claim["platform"], configuration=claim["configuration"], version=claim["version"],
        required_files={name:value["kind"] for name, value in files.items()}, expected_producer=claim["producer"])
    actual_files = {item["name"]:{key:item[key] for key in ("kind", "sha256")} for item in result["files"]}
    _need(actual_files == files, "Final package digest differs from the external selection")
    verify_bundle_stable(result)
    return dict(result="PASS", verification_scope="FINAL_PACKAGE_BYTES_AND_RECORDED_RECEIPTS",
        raw_stage_evidence=result["raw_stage_evidence"], authentication=result["authentication"],
        producer_context_matched=result["producer_context_matched"], source_sha=source_sha,
        version=result["version"], manifest_sha256=claim["manifest_sha256"], files=actual_files,
        not_run=["original_package_runtime_logs_replay", "new_runtime_execution", "hosted_authentication"], errors=[])


def verify_current_evidence(selection_path, *, selection_sha256, expected_source_sha,
                            profile_path, evidence_root, diagnostic=False):
    """Consume exact external selections; every selected claim is required.

    profile_path is independently selected from the trusted candidate source,
    outside each bundle. This API does not infer the current Git SHA or obtain
    GitHub authentication: callers supply their independently fixed identities.
    Diagnostic mode permits U1 fixtures/dirty runs but never current_verified.
    Package receipt claims prove only the scope explicitly returned for them.
    """
    report = dict(schema=REPORT_SCHEMA, status="FAIL", current_verified=False,
        source_sha=expected_source_sha, selection_sha256=selection_sha256,
        verification_scope="EXPLICIT_SELECTION_LOCAL_BYTES_AND_CONTROLLED_RECEIPTS",
        hosted_authentication="NOT_PERFORMED", release_ready=False,
        diagnostic=diagnostic, claims={}, input_locks=[], errors=[])
    locks = report["input_locks"]
    try:
        _need(type(diagnostic) is bool, "Diagnostic must be a boolean")
        _need(type(expected_source_sha) is str and re.fullmatch("[0-9a-f]{40}", expected_source_sha),
              "Expected source must be a full lowercase SHA")
        root = _no_links(evidence_root)
        _need(root.is_dir(), "Evidence root must be a directory")
        path = _no_links(selection_path)
        selected = _locked_json(path, selection_sha256, "selection", locks)
        _object(selected, {"schema", "source_sha", "claims"}, "selection")
        _need(selected["schema"] == SELECTION_SCHEMA, "Unsupported platform selection schema")
        _need(selected["source_sha"] == expected_source_sha, "Selected source differs from external current source")
        claims = selected["claims"]
        _need(type(claims) is list and 0 < len(claims) <= 128, "Required claims must be a bounded nonempty list")
        identifiers = set()
        for claim in claims:
            _need(type(claim) is dict, "Invalid claim fields")
            ident = claim.get("id")
            _need(type(ident) is str and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,127}", ident), "Invalid claim id")
            _need(ident not in identifiers, "Duplicate required claim id")
            identifiers.add(ident)
        for claim in claims:
            ident = claim["id"]
            value = dict(kind=claim.get("kind"), platform=claim.get("platform"),
                         configuration=claim.get("configuration"), result="FAIL", errors=[])
            report["claims"][ident] = value
            try:
                _text(claim.get("platform"), "platform")
                _text(claim.get("configuration"), "configuration")
                if claim.get("kind") == "execution":
                    value.update(_execution(claim, root=root, profile_path=profile_path,
                        source_sha=expected_source_sha, diagnostic=diagnostic, locks=locks))
                elif claim.get("kind") == "package_receipt":
                    value.update(_package(claim, root=root, source_sha=expected_source_sha, locks=locks))
                else:
                    raise ValueError("Unsupported evidence kind; no receipt-only fallback: " + str(claim.get("kind")))
            except (ValueError, OSError, TypeError, KeyError, AttributeError, PackageVerificationError) as error:
                message = f"{ident}: {error}"
                value["errors"].append(message)
                report["errors"].append(message)
        try:
            _stable(locks)
        except (ValueError, OSError, TypeError, KeyError, AttributeError, PackageVerificationError) as error:
            # A row verified earlier in this call must not remain a reusable
            # PASS after the selection or one of its bound inputs changed.
            for value in report["claims"].values():
                if value["result"] == "PASS":
                    value["result"] = "FAIL"
                    value["errors"].append("Input stability recheck failed: " + str(error))
            raise
        if not report["errors"]:
            report["status"] = "DIAGNOSTIC_EVIDENCE_VERIFIED" if diagnostic else "CURRENT_EVIDENCE_VERIFIED"
            report["current_verified"] = not diagnostic
    except (ValueError, OSError, TypeError, KeyError, AttributeError, PackageVerificationError) as error:
        report["errors"].append(str(error))
    return report
