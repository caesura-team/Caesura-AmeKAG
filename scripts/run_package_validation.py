#!/usr/bin/env python3
"""Validate one explicitly identified final package outside the repo.

The receipt binds preparation, static checks and the actual host runtime lane.
It is not publication authorization or hosted workflow/artifact authentication.
DMG/AppImage use their actual final container and an explicit payload root.
Their preparation, cleanup and retained bytes remain bound to the receipt.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import uuid

from package_verification import prepare_package, verify_stable, PackageVerificationError, _sha256_file
from package_containers import prepare_container, verify_container_stable
from run_validation import _source_identity

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = "caesura.package-validation.v1"


def _now():
    return datetime.now(timezone.utc).isoformat()


def _host_platform():
    return {"win32": "windows", "linux": "linux", "darwin": "macos"}.get(sys.platform)


def _digest(path):
    return _sha256_file(Path(path))


def _require(value, message):
    if not value:
        raise ValueError(message)


def _save(path, value):
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2)
        stream.write("\n")


def _attempt(path, input_path):
    supplied = Path(path).absolute()
    # Resolve an existing parent once (/var on macOS may be an alias), keeping
    # the new leaf intact so old attempts and leaf links cannot be overwritten.
    parent = supplied.parent.resolve(strict=True)
    _require(parent.is_dir(), "Attempt parent must already be a directory")
    selected = parent / supplied.name
    source = Path(input_path)
    if source.is_dir():
        _require(not selected.is_relative_to(source.resolve()), "Attempt must not be inside package input")
    for repo in (ROOT, *ROOT.parents):
        if (repo / ".git").exists():
            _require(not selected.is_relative_to(repo.resolve()), "Attempt must be outside every enclosing repository")
    for ancestor in (parent, *parent.parents):
        _require(not (ancestor / ".git").exists(), "Attempt must be outside every enclosing repository")
    if selected.exists() or selected.is_symlink():
        raise FileExistsError(f"Attempt already exists: {selected}")
    selected.mkdir()
    return selected


def _tools(paths):
    result = {}
    for key, value in paths.items():
        given = Path(value)
        _require(given.is_absolute(), f"Tool {key} must have an explicit absolute path")
        path = given.resolve(strict=True)
        _require(path.is_file() and path.stat().st_size > 0, f"Missing tool {key}")
        _require(path.suffix.lower() not in (".cmd", ".bat"), f"Tool {key} cannot be a shell wrapper")
        result[key] = {"path": str(path), "sha256": _digest(path)}
    return result


def _static_stage(platform, payload, requirements, tools):
    if platform == "web":
        from verify_web_package import inspect_web_package
        return inspect_web_package(payload, tools["lua"]["path"])
    from verify_native_package import inspect_native_package
    result = inspect_native_package(payload, platform, requirements["required_configuration"])
    result["status"] = "STATIC_PASS" if result.get("passed") is True else "STATIC_FAIL"
    return result


def _runtime_stage(platform, payload, requirements, attempt, tools):
    if platform == "web":
        from web_package_runtime import run_web_package
        action = (requirements or {}).get("actions")
        return run_web_package(payload, attempt, node_executable=tools["node"]["path"],
                               browser_executable=tools["browser"]["path"], lua_executable=tools["lua"]["path"],
                               actions_path=action["path"] if action else None,
                               actions_sha256=action["sha256"] if action else None)
    from native_package_runtime import run_native_package
    options = {"launch_relative_path": "AppRun"} if requirements.get("container_format") == "appimage" else {}
    return run_native_package(payload, requirements["required_configuration"], attempt,
                              python_executable=tools["python"]["path"], **options)


def run_package_validation(*, input_path, expected_sha256=None, expected_inventory_sha256=None,
                           attempt_dir, platform, configuration, source_sha,
                           requirements_path=None, requirements_sha256=None,
                           diagnostic=False, tool_paths=None, actions_path=None, actions_sha256=None,
                           container_format=None, payload_relative_path=None):
    attempt = _attempt(attempt_dir, input_path)
    report = {"schema": SCHEMA, "run_id": str(uuid.uuid4()), "started_at": _now(),
              "status": "FAIL", "accepted": False, "platform": platform,
              "configuration": configuration, "expected_source_sha": source_sha,
              "diagnostic": bool(diagnostic), "attempt": str(attempt), "errors": [],
              "scope": "one final package and this host runtime; no publication approval",
              "preparation": None, "static": None, "runtime": None, "stability": None,
              "container": None, "container_format": container_format,
              "evidence": {}}
    requirements = None
    requirement_path = None
    source = None
    try:
        _require(platform in ("windows", "linux", "macos", "web"), "Unknown package platform")
        host = _host_platform()
        _require(platform == "web" or platform == host, "Native runtime must execute on the selected platform")
        _require(container_format in (None, "dmg", "appimage"), "Unknown explicit container format")
        if container_format is not None:
            _require(platform == ("macos" if container_format == "dmg" else "linux"),
                     "Container format does not match the selected native platform")
            _require(isinstance(payload_relative_path, str) and bool(payload_relative_path),
                     "Container payload-relative path must be explicit")
            _require(expected_sha256 is not None and expected_inventory_sha256 is None,
                     "Final container requires a prelocked archive digest")
        else:
            _require(payload_relative_path is None, "Payload-relative path only applies to containers")
        _require(isinstance(source_sha, str) and re.fullmatch("[0-9a-f]{40}", source_sha), "Expected source must be a full commit SHA")
        _require(bool(configuration), "Configuration is required")
        _require((ROOT / ".git").exists(), "Source checkout has no .git identity")
        report["source_before"] = _source_identity(ROOT)
        _require(report["source_before"]["source_sha"] == source_sha, "Wrong source SHA")
        _require(diagnostic or not report["source_before"]["dirty"], "Dirty source requires explicit diagnostic mode")
        source = Path(input_path).absolute()
        if actions_path is not None or actions_sha256 is not None:
            _require(platform == "web", "UI action configuration only applies to Web packages")
            from web_package_runtime import _lock_actions
            report["actions"] = _lock_actions(actions_path, actions_sha256, source.resolve(strict=True), attempt)
            requirements = {"actions":report["actions"]}
        if source.is_dir():
            _require(not attempt.is_relative_to(source.resolve()), "Attempt must not be inside package input")
        required_tools = {"node", "browser", "lua"} if platform == "web" else {"python"}
        if container_format == "dmg":
            required_tools.add("hdiutil")
        else:
            _require("hdiutil" not in (tool_paths or {}), "hdiutil only applies to DMG validation")
        _require(required_tools <= set(tool_paths or {}), "Missing explicit host tools: " + ", ".join(sorted(required_tools)))
        report["tools"] = _tools(tool_paths)
        report["controller_python"] = _tools({"python": Path(sys.executable)})["python"]
        if platform != "web":
            _require(requirements_path is not None and requirements_sha256 is not None,
                     "Native package requires independently locked build requirements")
            requirement_path = Path(requirements_path).resolve(strict=True)
            _require(not requirement_path.is_relative_to(attempt), "Requirements cannot come from this attempt")
            _require(not source.is_dir() or not requirement_path.is_relative_to(source.resolve()),
                     "Requirements must be outside the package")
            raw = requirement_path.read_bytes()
            actual_sha = hashlib.sha256(raw).hexdigest()
            _require(actual_sha == requirements_sha256, "Wrong external requirements identity")
            requirements = json.loads(raw)
            _require(requirements.get("schema") == "caesura.package-build.v1", "Unknown build requirements schema")
            _require(requirements.get("platform") == platform, "Requirements platform mismatch")
            _require(requirements.get("configuration") == configuration, "Requirements configuration mismatch")
            report["requirements"] = {"path": str(requirement_path), "sha256": actual_sha, "value": requirements}
        if container_format is not None:
            _require(source.is_file(), "Final container input must be a file")
            container = prepare_container(source, attempt / "container", container_format=container_format,
                expected_sha256=expected_sha256, payload_relative_path=payload_relative_path,
                hdiutil_executable=report["tools"]["hdiutil"]["path"] if container_format == "dmg" else None)
            report["container"] = container
            _require(container.get("status") == "CONTAINER_PREPARED", "Final container preparation did not pass")
            prepared = container["preparation"]
            report["evidence"]["container/container-preparation.json"] = _digest(attempt / "container/container-preparation.json")
            # Runtime must exercise the extracted AppRun as well as Engine.
            # This input is caller-selected, never supplied by the package.
            requirements = {**requirements, "container_format": container_format}
        else:
            prepared = prepare_package(source, attempt / "prepared", expected_sha256=expected_sha256,
                                       expected_inventory_sha256=expected_inventory_sha256)
            report["evidence"]["prepared/preparation.json"] = _digest(attempt / "prepared/preparation.json")
        report["preparation"] = prepared
        payload = Path(prepared["package_path"])
        if platform != "web" and source.is_file() and container_format is None:
            # CPack's ZIP/TGZ prefix is supplied by the external build contract.
            # No glob, first child, package self-manifest or mtime selection.
            stem = requirements.get("archive_basename")
            _require(isinstance(stem, str) and stem not in ("", ".", "..")
                     and "/" not in stem and "\\" not in stem, "Invalid externally specified CPack root")
            payload = payload / stem
        _require(payload.is_dir(), "Explicit package payload root is missing")
        report["payload"] = str(payload)
        report["static"] = _static_stage(platform, payload, requirements, report["tools"])
        _save(attempt / "static.json", report["static"])
        report["evidence"]["static.json"] = _digest(attempt / "static.json")
        _require(report["static"].get("status") == "STATIC_PASS", "Required static stage did not pass")
        report["runtime"] = _runtime_stage(platform, payload, requirements, attempt / "runtime", report["tools"])
        _save(attempt / "runtime.json", report["runtime"])
        report["evidence"]["runtime.json"] = _digest(attempt / "runtime.json")
        _require(report["runtime"].get("status") == "RUNTIME_PASS", "Required runtime stage did not pass")
    except (Exception, KeyboardInterrupt) as error:
        if isinstance(error, PackageVerificationError) and error.report:
            report["preparation"] = error.report
        report["errors"].append(f"{type(error).__name__}: {error}")
    finally:
        # Every completed/failed stage retains its original result. Stability
        # failure is appended; it never rewrites a previous stage as passing.
        try:
            if report["container"] and report["container"].get("status") == "CONTAINER_PREPARED":
                report["stability"] = verify_container_stable(report["container"])
            elif report["preparation"] and report["preparation"].get("status") == "PREPARED":
                report["stability"] = verify_stable(report["preparation"])
            if "source_before" in report:
                report["source_after"] = _source_identity(ROOT)
                _require(report["source_before"] == report["source_after"], "Source changed during package validation")
            if requirement_path is not None and "requirements" in report:
                _require(_digest(requirement_path) == report["requirements"]["sha256"], "Build requirements changed during validation")
            if report.get("actions") is not None:
                action = report["actions"]
                _require(_digest(action["path"]) == action["sha256"], "UI action input changed during validation")
            for name, item in report.get("tools", {}).items():
                _require(_digest(item["path"]) == item["sha256"], f"Tool {name} changed during validation")
            if "controller_python" in report:
                item = report["controller_python"]
                _require(_digest(item["path"]) == item["sha256"], "Controller Python changed during validation")
        except Exception as error:
            report["errors"].append(f"Final verification: {type(error).__name__}: {error}")
        for relative, expected in report["evidence"].items():
            try:
                _require(_digest(attempt / relative) == expected, f"Stage evidence changed: {relative}")
            except Exception as error:
                report["errors"].append(f"Stage evidence {relative}: {type(error).__name__}: {error}")
        if (not report["errors"] and report["static"] and report["runtime"]
                and report["stability"] and report["stability"].get("status") == "STABLE"):
            report["status"] = "DIAGNOSTIC_PASS" if diagnostic else "PASS"
            report["accepted"] = not diagnostic
        report["finished_at"] = _now()
        _save(attempt / "package-run.json", report)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    identity = parser.add_mutually_exclusive_group(required=True)
    identity.add_argument("--sha256")
    identity.add_argument("--inventory-sha256")
    parser.add_argument("--attempt", type=Path, required=True)
    parser.add_argument("--platform", choices=("windows", "linux", "macos", "web"), required=True)
    parser.add_argument("--configuration", required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--requirements", type=Path)
    parser.add_argument("--requirements-sha256")
    parser.add_argument("--actions", type=Path)
    parser.add_argument("--actions-sha256")
    parser.add_argument("--container-format", choices=("dmg", "appimage"))
    parser.add_argument("--payload-relative-path")
    parser.add_argument("--diagnostic", action="store_true")
    for name in ("python", "lua", "node", "browser", "hdiutil"):
        parser.add_argument("--" + name, type=Path)
    args = parser.parse_args()
    try:
        report = run_package_validation(input_path=args.input, expected_sha256=args.sha256,
            expected_inventory_sha256=args.inventory_sha256, attempt_dir=args.attempt,
            platform=args.platform, configuration=args.configuration, source_sha=args.source_sha,
            requirements_path=args.requirements, requirements_sha256=args.requirements_sha256,
            actions_path=args.actions, actions_sha256=args.actions_sha256,
            container_format=args.container_format, payload_relative_path=args.payload_relative_path,
            diagnostic=args.diagnostic, tool_paths={key: getattr(args, key) for key in
                                                  ("python", "lua", "node", "browser", "hdiutil") if getattr(args, key)})
        print(json.dumps({"status": report["status"], "accepted": report["accepted"],
                          "receipt": str(Path(report["attempt"]) / "package-run.json"),
                          "errors": report["errors"]}, ensure_ascii=False))
        return 0 if report["status"] in ("PASS", "DIAGNOSTIC_PASS") else 1
    except (ValueError, OSError) as error:
        print(f"Package validation refused: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
