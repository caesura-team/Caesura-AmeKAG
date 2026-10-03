#!/usr/bin/env python3
"""Bind downloaded U1 evidence to externally selected producer outputs.

This adapter copies only already hash-authenticated bytes to a new controlled
snapshot outside the download. It delegates all report parsing and strict
source/test checks to U1. Its caller must authenticate the producer job and
artifact; local hashes and this function cannot prove hosted provenance.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re

from package_verification import _sha256_file, inspect_inventory
from verify_release_candidate import verify_evidence

MAX_JSON = 64 * 1024 * 1024
CONTEXT_KEYS = {"run_id", "run_attempt", "repository", "workflow", "platform", "configuration"}


def _need(condition, message):
    if not condition:
        raise ValueError(message)


def _no_links(value):
    path = Path(value).absolute()
    for item in (path, *path.parents):
        _need(not item.is_symlink() and not item.is_junction(), "Evidence path must not traverse a link")
    _need(path == path.resolve(strict=True), "Evidence path is not canonical")
    return path


def _snapshot(path, expected, name):
    _need(isinstance(expected, str) and re.fullmatch("[0-9a-f]{64}", expected), f"Invalid {name} digest")
    _no_links(path)
    with path.open("rb") as stream:
        raw = stream.read(MAX_JSON + 1)
    _need(len(raw) <= MAX_JSON, f"{name} exceeds JSON limit")
    _need(hashlib.sha256(raw).hexdigest() == expected, f"External {name} digest mismatch")
    def pairs(items):
        result = {}
        for key, value in items:
            _need(key not in result, f"Duplicate JSON key in {name}")
            result[key] = value
        return result
    def constant(_):
        raise ValueError(f"Non-finite JSON value in {name}")
    value = json.loads(raw.decode("utf-8-sig"), object_pairs_hook=pairs, parse_constant=constant)
    _need(isinstance(value, dict), f"Invalid {name} object")
    return raw, value


def _tree_lock(root):
    inventory = inspect_inventory(root)
    _need(all(entry["type"] in ("file", "directory") for entry in inventory["entries"]),
          "Execution evidence must not contain linked entries")
    return {"path":str(root), "kind":"directory", "sha256":inventory["sha256"]}


def verify_execution_bundle(evidence_dir, *, manifest_sha256, receipt_sha256,
                            profile_path, profile_sha256, profile_name, source_sha,
                            expected_context, trusted_dir):
    """Verify exact producer bytes; never derive trusted expectations from them.

    receipt_sha256 and expected_context must come from authenticated producer
    outputs. profile_path/profile_sha256/profile_name come from the frozen
    candidate policy. trusted_dir is a new path outside the downloaded bundle.
    No publication or hosted-authentication claim is returned.
    """
    root = _no_links(evidence_dir)
    _need(root.is_dir(), "Missing execution evidence directory")
    profile = _no_links(profile_path)
    _need(not profile.is_relative_to(root), "Trusted profile must be outside evidence bundle")
    trusted = Path(trusted_dir).absolute()
    _need(not trusted.exists(), "Trusted snapshot directory already exists")
    _no_links(trusted.parent)
    _need(not trusted.is_relative_to(root) and not root.is_relative_to(trusted),
          "Trusted snapshot must be outside evidence bundle")
    _need(not profile.is_relative_to(trusted), "Trusted profile must exist outside new snapshot")
    _need(isinstance(source_sha, str) and re.fullmatch("[0-9a-f]{40}", source_sha), "Invalid source SHA")
    _need(isinstance(profile_name, str) and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,127}", profile_name),
          "Invalid profile name")
    _need(isinstance(expected_context, dict) and set(expected_context) == CONTEXT_KEYS,
          "External execution context must supply exactly the required fields")
    _need(type(expected_context["run_attempt"]) is int and expected_context["run_attempt"] > 0,
          "Invalid context run_attempt")
    _need(all(isinstance(value, str) and value and len(value) <= 1024
              and not any(char in value for char in "\r\n\x00")
              for key, value in expected_context.items() if key != "run_attempt"), "Invalid context field")
    tree = _tree_lock(root)
    manifest_path = root / "manifest.json"
    receipt_path = root / "execution-receipt.json"
    _snapshot(manifest_path, manifest_sha256, "manifest")
    receipt_raw, receipt = _snapshot(receipt_path, receipt_sha256, "receipt")
    profile_raw, _ = _snapshot(profile, profile_sha256, "profile")
    _need(receipt.get("source_sha") == source_sha, "Execution source does not match expected source")
    for key, expected in expected_context.items():
        actual = receipt.get(key)
        _need(type(actual) is type(expected) and actual == expected, "Execution context mismatch: " + key)
    _need(receipt.get("profile_name") == profile_name, "Execution profile name mismatch")
    trusted.mkdir()
    receipt_copy, profile_copy = trusted / "execution-receipt.json", trusted / "profile.json"
    for path, raw in ((receipt_copy, receipt_raw), (profile_copy, profile_raw)):
        with path.open("xb") as stream:
            stream.write(raw)
    locks = [tree,
        {"path":str(profile), "kind":"file", "sha256":profile_sha256},
        {"path":str(receipt_copy), "kind":"file", "sha256":receipt_sha256},
        {"path":str(profile_copy), "kind":"file", "sha256":profile_sha256}]
    errors = verify_evidence(root, profile_copy, profile_name, receipt_copy, source_sha=source_sha, release=True)
    _need(not errors, "Strict U1 verification failed: " + "; ".join(errors))
    result = {"schema":"caesura.execution-bundle.v1", "status":"EXECUTION_BUNDLE_VERIFIED",
        "release_ready":False, "hosted_provenance":"CALLER_MUST_AUTHENTICATE", "source_sha":source_sha,
        "profile_name":profile_name, "context":dict(expected_context), "manifest_sha256":manifest_sha256,
        "receipt_sha256":receipt_sha256, "profile_sha256":profile_sha256, "locks":locks}
    verify_execution_bundle_stable(result)
    return result


def verify_execution_bundle_stable(result):
    """Reopen precisely the validated paths immediately before consuming them."""
    _need(result.get("schema") == "caesura.execution-bundle.v1"
          and result.get("status") == "EXECUTION_BUNDLE_VERIFIED", "Invalid execution bundle result")
    _need(isinstance(result.get("locks"), list) and len(result["locks"]) == 4, "Missing execution locks")
    for lock in result["locks"]:
        path = _no_links(lock["path"])
        actual = _tree_lock(path)["sha256"] if lock["kind"] == "directory" else _sha256_file(path)
        _need(actual == lock["sha256"], f"Verified execution input changed: {path}")
    return {"status":"STABLE", "release_ready":False}
