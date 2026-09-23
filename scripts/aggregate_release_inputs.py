#!/usr/bin/env python3
"""Join authenticated job selections with exact downloaded U1/U22 bytes.

The hosted argument is an in-memory result from verify_hosted_inputs, never a
JSON report selected by a downloaded artifact. This module neither queries a
different run nor publishes anything. Its saved result must itself be locked
by the controlled caller before the preupload recheck.
"""
from __future__ import annotations

import json
from pathlib import Path
import re

from ci_package_lane import _new_work
from package_verification import _sha256_file, prepare_package, verify_stable
from run_validation import _source_identity
from verify_execution_bundle import (_no_links, _snapshot, verify_execution_bundle,
                                     verify_execution_bundle_stable)
from verify_package_bundle import verify_bundle, verify_bundle_stable
from verify_pages_artifact import verify_pages_artifact, verify_pages_artifact_stable

SCHEMA = "caesura.release-inputs.v1"
HOSTED_POLICY_KEYS = ("schema_version", "required_jobs", "artifact_roles")
SOURCE_FILES = {"CMakeLists.txt", "scripts/validation_profiles.json"}


def _need(condition, message):
    if not condition:
        raise ValueError(message)


def _file_lock(path, expected, name):
    path = _no_links(path)
    _need(isinstance(expected, str) and re.fullmatch("[0-9a-f]{64}", expected), "Invalid " + name + " digest")
    _need(_sha256_file(path) == expected, name + " digest mismatch")
    return {"path":str(path), "kind":"file", "sha256":expected}


def _save(path, report):
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(report, stream, ensure_ascii=False, indent=2)
        stream.write("\n")


def _stable(report):
    _need(_source_identity(_no_links(report["source_root"])) == report["source_before"],
          "Source checkout changed since aggregation")
    for lock in report["source_locks"]:
        _file_lock(Path(lock["path"]), lock["sha256"], "Verified source input changed")
    for prepared in report["transports"].values():
        verify_stable(prepared)
    for package in report["packages"].values():
        verify_bundle_stable(package)
    for execution in report["executions"].values():
        verify_execution_bundle_stable(execution)
    for pages in report["pages"].values():
        verify_pages_artifact_stable(pages)
    # The upload list is separately locked even though package rechecks cover
    # the same paths. No late glob or filename selection is permitted.
    excluded = {(spec["package_role"], spec["file"]) for spec in report["pages_specs"].values()}
    selected = [item for role, package in report["packages"].items() for item in package["files"]
                if (role, item["name"]) not in excluded]
    _need(report["upload_files"] == selected, "Explicit upload list changed")
    _need(_source_identity(_no_links(report["source_root"])) == report["source_before"],
          "Source checkout changed during byte recheck")
    return {"status":"STABLE"}


def verify_downloaded_inputs(*, hosted, policy_path, policy_sha256, archives,
                             execution_claims, repo_root, work_dir, release_tag=None):
    """Consume preauthenticated selections without allowing bundles to choose scope.

    archives maps each artifact role to its original Actions transfer ZIP.
    execution_claims contains receipt_sha256 and run_id from that producer's
    independent job outputs. policy_path is frozen before producer fanout;
    its source_files pins the CMake version and complete U1 profile bytes.
    """
    work = _new_work(work_dir)
    receipt_path = work / "aggregate.json"
    report = {"schema":SCHEMA, "status":"FAIL", "release_ready":False,
        "receipt_path":str(receipt_path), "transport":hosted.get("transport"),
        "source_locks":[], "transports":{}, "packages":{}, "executions":{}, "pages":{}, "pages_specs":{}, "upload_files":[], "errors":[]}
    try:
        _need(hosted.get("kind") == "caesura.hosted-inputs.v1"
              and hosted.get("status") == "HOSTED_INPUTS_VERIFIED" and hosted.get("errors") == []
              and hosted.get("transport") in ("github", "fixture"), "Successful controlled hosted result required")
        expected = hosted["expected"]
        policy_path = _no_links(policy_path)
        _, policy = _snapshot(policy_path, policy_sha256, "policy")
        _need({key:policy.get(key) for key in HOSTED_POLICY_KEYS}
              == {key:hosted["policy"].get(key) for key in HOSTED_POLICY_KEYS}, "Authenticated policy selection differs")
        package_specs, execution_specs = policy.get("package_inputs"), policy.get("execution_inputs")
        _need(isinstance(package_specs, dict) and package_specs and isinstance(execution_specs, dict) and execution_specs,
              "Policy must preselect package and execution inputs")
        pages_specs = policy.get("pages_inputs", {})
        _need(isinstance(pages_specs, dict), "Pages policy must be a mapping")
        roles = set(policy["artifact_roles"])
        groups = (set(package_specs), set(execution_specs), set(pages_specs))
        _need(not any(groups[i] & groups[j] for i in range(3) for j in range(i+1,3))
              and set.union(*groups) == roles, "Policy input roles do not exactly cover artifacts")
        for role, spec in pages_specs.items():
            _need(isinstance(spec, dict) and set(spec) == {"package_role", "file"}
                  and spec.get("file") == "artifact.tar", "Pages must select the final artifact.tar")
            package_role = spec.get("package_role")
            _need(isinstance(package_role, str) and package_role in package_specs
                  and package_specs[package_role].get("platform") == "web"
                  and package_specs[package_role].get("required_files", {}).get("artifact.tar") == "file",
                  "Pages needs an independently accepted Web tar")
            _need(policy["artifact_roles"][role] == policy["artifact_roles"][package_role],
                  "Pages and accepted package must share the same producer job role")
            _need(hosted["artifacts"][role].get("job_id") == hosted["artifacts"][package_role].get("job_id")
                  and type(hosted["artifacts"][role].get("job_id")) is int,
                  "Pages and accepted package must share the same authenticated job ID")
            _need(hosted["artifacts"][role]["manifest_sha256"] == hosted["artifacts"][package_role]["manifest_sha256"],
                  "Pages manifest output differs from the accepted package producer")
        report["pages_specs"] = pages_specs
        _need(isinstance(archives, dict) and set(archives) == roles and set(hosted["artifacts"]) == roles,
              "Downloaded archive roles must exactly match policy")
        _need(isinstance(execution_claims, dict) and set(execution_claims) == set(execution_specs),
              "Controlled execution claims must exactly match policy")
        repo = _no_links(repo_root)
        _need((repo / ".git").exists(), "Source checkout identity is required")
        before = dict(_source_identity(repo))
        _need(before.get("source_sha") == expected["source_sha"], "Wrong source checkout")
        _need(before.get("dirty") is False, "A clean source checkout is required")
        sources = policy.get("source_files")
        _need(isinstance(sources, dict) and set(sources) == SOURCE_FILES, "Policy must pin exact version/profile source files")
        report["source_locks"] = [_file_lock(policy_path, policy_sha256, "policy")]
        for name, digest in sources.items():
            report["source_locks"].append(_file_lock(repo / name, digest, "source input"))
        version = policy.get("version")
        _need(isinstance(version, str) and re.fullmatch(r"\d+\.\d+\.\d+(?:\.\d+)?", version), "Invalid policy version")
        cmake = (repo / "CMakeLists.txt").read_text(encoding="utf-8-sig")
        versions = re.findall(r"(?im)^\s*project\(\s*CaesuraAmeKAG\s+VERSION\s+(\d+\.\d+\.\d+(?:\.\d+)?)\s+LANGUAGES\b", cmake)
        _need(versions == [version], "CMake and policy version differ")
        _need(release_tag is None or release_tag == "v" + version, "Release tag does not match engine version")
        workflow_ref = expected.get("workflow_ref")
        workflow_sha = expected.get("workflow_sha")
        prefix = expected["repository"] + "/" + expected["workflow_path"] + "@"
        _need(isinstance(workflow_ref, str) and workflow_ref.startswith(prefix)
              and len(workflow_ref) > len(prefix) and not any(c in workflow_ref for c in "\r\n\x00"),
              "Expected caller workflow_ref must match authenticated workflow path")
        _need(isinstance(workflow_sha, str) and re.fullmatch("[0-9a-f]{40}", workflow_sha), "Expected caller workflow_sha required")
        _need(workflow_sha == expected["called_workflow_sha"],
              "Same-repository relative workflow call must use the authenticated caller commit")
        report.update(source_sha=expected["source_sha"], version=version, release_tag=release_tag,
                      expected=expected, policy_sha256=policy_sha256, source_before=before, source_root=str(repo))
        names = set()
        for role in sorted(roles):
            entry = hosted["artifacts"][role]
            digest = entry["artifact_digest"]
            _need(isinstance(digest, str) and re.fullmatch("sha256:[0-9a-f]{64}", digest), "Invalid transport digest")
            prepared = prepare_package(archives[role], work / ("transport-" + role), expected_sha256=digest[7:])
            report["transports"][role] = prepared
            payload = Path(prepared["package_path"])
            if role in package_specs:
                spec = package_specs[role]
                producer = {key:expected[key] for key in ("repository", "repository_id", "run_id", "run_attempt")}
                producer.update(provider="github-actions", workflow_ref=workflow_ref, workflow_sha=workflow_sha, job_key=spec["job_key"])
                value = verify_bundle(payload, manifest_sha256=entry["manifest_sha256"], source_sha=expected["source_sha"],
                    platform=spec["platform"], configuration=spec["configuration"], version=version,
                    required_files=spec["required_files"], expected_producer=producer)
                for item in value["files"]:
                    _need(item["name"].casefold() not in names, "Duplicate final upload filename")
                    names.add(item["name"].casefold())
                report["packages"][role] = value
                report["upload_files"].extend(value["files"])
            elif role in execution_specs:
                spec, claim = execution_specs[role], execution_claims[role]
                _need(isinstance(claim, dict) and set(claim) == {"receipt_sha256", "run_id"}, "Invalid independent execution claim")
                context = {"run_id":claim["run_id"], "run_attempt":expected["run_attempt"],
                    "repository":expected["repository"], "workflow":workflow_ref,
                    "platform":spec["platform"], "configuration":spec["configuration"]}
                report["executions"][role] = verify_execution_bundle(payload,
                    manifest_sha256=entry["manifest_sha256"], receipt_sha256=claim["receipt_sha256"],
                    profile_path=repo / "scripts/validation_profiles.json", profile_sha256=sources["scripts/validation_profiles.json"],
                    profile_name=spec["profile_name"], source_sha=expected["source_sha"], expected_context=context,
                    trusted_dir=work / ("execution-" + role))
        for role, spec in pages_specs.items():
            report["pages"][role] = verify_pages_artifact(
                Path(report["transports"][role]["package_path"]),
                package_result=report["packages"][spec["package_role"]],
                manifest_sha256=hosted["artifacts"][role]["manifest_sha256"],
                tar_name=spec["file"], work_dir=work / ("pages-" + role))
        # Pages tar has a separate deployment channel; it is not a Release asset.
        excluded = {(spec["package_role"], spec["file"]) for spec in pages_specs.values()}
        report["upload_files"] = [item for role, package in report["packages"].items()
            for item in package["files"] if (role, item["name"]) not in excluded]
        report["stability"] = _stable(report)
        report["source_after"] = _source_identity(repo)
        _need(report["source_after"] == before, "Source checkout changed during aggregation")
        report["status"] = "INPUTS_VERIFIED"
        return report
    except Exception as error:
        report["errors"].append(str(error))
        raise
    finally:
        _save(receipt_path, report)


def verify_preupload(receipt_path, expected_sha256):
    """Recheck an externally locked result on this same controlled host.

    Hosted state must also be freshly authenticated by the caller immediately
    before publication. An arbitrary downloaded JSON cannot supply its own SHA.
    """
    path = _no_links(receipt_path)
    _, report = _snapshot(path, expected_sha256, "aggregate receipt")
    _need(report.get("schema") == SCHEMA and report.get("status") == "INPUTS_VERIFIED"
          and report.get("errors") == [], "Only a verified aggregate can be rechecked")
    _need(report.get("transport") == "github", "A fixture aggregate cannot authorize github preupload")
    _need(str(path) == report.get("receipt_path"), "Aggregate moved to a different host/path")
    _stable(report)
    return {"status":"UPLOAD_INPUTS_VERIFIED", "release_ready":False,
        "source_sha":report["source_sha"], "version":report["version"], "upload_files":report["upload_files"],
        "scope":"Exact local bytes only; caller must reauthenticate hosted state and hold publication authorization"}
