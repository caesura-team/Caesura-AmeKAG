#!/usr/bin/env python3
"""Prepare an explicitly locked final DMG/AppImage on its actual host platform.

The original container is copied byte-for-byte into a new private attempt before
mount/extraction. DMG ownership requires before/after hdiutil inventories, the
exact copied image path, the requested new mountpoint and a new device. Only that
device may be detached; failed/ambiguous observations never authorize cleanup of
another mount. AppImage extraction executes the locked copy under the owned-tree
runner. This is environment isolation, not a filesystem sandbox for executable
containers. Neither extraction nor this receipt proves AppRun/Engine execution,
signing, notarization or package acceptance.
"""
from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import plistlib
import re
import shutil
import sys

from package_runtime import run_runtime_command
from package_verification import (PackageVerificationError, _component, _plain_root,
                                  _reparse, _sha256_file, inspect_inventory,
                                  prepare_package)

SCHEMA = "caesura.package-container-preparation.v1"


def _now() -> str:
    return datetime.now(timezone.utc).isoformat()


def _host_platform() -> str:
    return "macos" if sys.platform == "darwin" else "linux" if sys.platform.startswith("linux") else "windows" if os.name == "nt" else sys.platform


def _save(path: Path, report: dict) -> None:
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(report, stream, ensure_ascii=False, indent=2, sort_keys=True)
        stream.write("\n")


def _new_attempt(value: str | Path) -> Path:
    given = Path(value).absolute()
    if os.path.lexists(given):
        raise ValueError("Container attempt must be new; refusing to write existing evidence")
    # macOS /tmp and /var are ordinary parent aliases. Resolve the existing
    # parent once, then retain the uncreated leaf without following leaf links.
    parent = given.parent.resolve(strict=True)
    if not parent.is_dir():
        raise ValueError("Container attempt needs an existing directory parent")
    if any(os.path.lexists(ancestor / ".git") for ancestor in (parent, *parent.parents)):
        raise ValueError("Container attempt must be outside every repository")
    selected = parent / given.name
    if os.path.lexists(selected):
        raise ValueError("Container attempt must be new; refusing to write existing evidence")
    selected.mkdir(mode=0o700)
    return selected


def _payload(root: Path, relative: str) -> Path:
    selected = root if relative == "." else root.joinpath(*relative.split("/"))
    # No normalization of traversal and no first-child/layout discovery.
    for parent in (selected, *selected.parents):
        if parent == root.parent:
            break
        if parent.is_symlink() or _reparse(parent):
            raise ValueError("Selected container payload traverses a link/reparse point")
    resolved = _plain_root(selected, directory=True)
    if not resolved.is_relative_to(root.resolve(strict=True)):
        raise ValueError("Selected container payload escaped its root")
    return resolved


def _environment(attempt: Path) -> dict[str, str]:
    # Mount/extraction needs no GUI, package Lua, proxies, SDK override or user PATH.
    home, temp, work = (attempt / name for name in ("home", "temp", "work"))
    for directory in (home, temp, work):
        directory.mkdir(mode=0o700)
    return {"PATH": "/usr/bin:/bin:/usr/sbin:/sbin", "HOME": str(home),
            "TMP": str(temp), "TEMP": str(temp), "TMPDIR": str(temp),
            "XDG_CONFIG_HOME": str(home / ".config"),
            "XDG_CACHE_HOME": str(home / ".cache"),
            "XDG_DATA_HOME": str(home / ".local/share"),
            "LANG": "en_US.UTF-8", "LC_ALL": "C", "PWD": str(work)}


def _record_evidence(report: dict, paths):
    attempt = Path(report["attempt_path"])
    for path in paths:
        if path.is_symlink() or _reparse(path):
            raise ValueError(f"Container evidence cannot be a link/reparse point: {path}")
        if path.is_dir():
            continue
        relative = path.relative_to(attempt).as_posix()
        digest = _sha256_file(path)
        if relative in report["evidence"] and digest != report["evidence"][relative]:
            raise ValueError(f"Completed container stage evidence changed: {relative}")
        report["evidence"][relative] = digest


def _run(report: dict, label: str, argv: list[str], environment: dict,
         timeout: float) -> tuple[dict, bytes]:
    attempt = Path(report["attempt_path"])
    command_dir = attempt / "commands" / label
    command_dir.mkdir(mode=0o700)
    out_path, err_path = command_dir / "stdout.log", command_dir / "stderr.log"
    entry = {"label": label, "argv": argv, "cwd": str(attempt / "work"),
             "stdout_path": str(out_path), "stderr_path": str(err_path),
             "status": "FAILED"}
    report["commands"].append(entry)
    try:
        with out_path.open("xb") as out, err_path.open("xb") as err:
            result = run_runtime_command(argv, attempt / "work", environment,
                                         command_dir / "control", out, err, timeout)
        entry["runtime_receipt"] = result
        if (result.get("status") != "EXITED" or result.get("actual_exit_code") != 0
                or result.get("owned_tree_cleanup") != "COMPLETE"):
            raise ValueError(f"{label} did not exit successfully with owned-tree cleanup")
        entry["status"] = "EXITED"
        return result, out_path.read_bytes()
    except (Exception, KeyboardInterrupt) as error:
        entry["error"] = f"{type(error).__name__}: {error}"
        run_file = command_dir / "control/run.json"
        if run_file.is_file():
            try:
                entry["runtime_receipt"] = json.loads(run_file.read_text(encoding="utf-8"))
            except (OSError, ValueError) as receipt_error:
                entry["receipt_error"] = str(receipt_error)
        raise
    finally:
        try:
            # Lock each completed command now, before any later command runs.
            _record_evidence(report, command_dir.rglob("*"))
        except (Exception, KeyboardInterrupt) as error:
            report["errors"].append(f"Command evidence {label}: {type(error).__name__}: {error}")


def _plist(data: bytes) -> dict:
    value = plistlib.loads(data)
    if not isinstance(value, dict):
        raise ValueError("hdiutil output must be a plist dictionary")
    return value


def _images(value: dict) -> list[dict]:
    images = value.get("images")
    if not isinstance(images, list) or any(not isinstance(row, dict) for row in images):
        raise ValueError("hdiutil info lacks a complete image list")
    for row in images:
        if not isinstance(row.get("image-path"), str) or not Path(row["image-path"]).is_absolute():
            raise ValueError("hdiutil image has no absolute source path")
        _entities(row)
    return images


def _entities(value: dict) -> list[dict]:
    entries = value.get("system-entities")
    if not isinstance(entries, list) or not entries:
        raise ValueError("hdiutil image lacks device entities")
    for entry in entries:
        if (not isinstance(entry, dict) or not isinstance(entry.get("dev-entry"), str)
                or not re.fullmatch(r"/dev/disk[0-9]+(?:s[0-9]+)*", entry["dev-entry"])):
            raise ValueError("hdiutil image has an invalid device identity")
        if "mount-point" in entry and (not isinstance(entry["mount-point"], str)
                or not Path(entry["mount-point"]).is_absolute()):
            raise ValueError("hdiutil image has an invalid mountpoint")
    return entries


def _owned(before: list[dict], after: list[dict], source: Path, mount: Path) -> dict:
    previous_devices = {entry["dev-entry"] for row in before for entry in _entities(row)}
    previous_mounts = {entry.get("mount-point") for row in before for entry in _entities(row)}
    if str(mount) in previous_mounts or any(row["image-path"] == str(source) for row in before):
        raise ValueError("Requested image or mountpoint was already present before attach")
    matches = [row for row in after if row["image-path"] == str(source)]
    if len(matches) != 1:
        raise ValueError("Cannot bind exactly one mounted image to the locked input copy")
    row = matches[0]
    mounted = [entry for entry in _entities(row) if entry.get("mount-point") == str(mount)]
    if len(mounted) != 1:
        raise ValueError("Locked image does not own exactly the requested mountpoint")
    entries = _entities(row)
    if any(entry["dev-entry"] in previous_devices for entry in entries):
        raise ValueError("Mounted image reuses a device observed before attach")
    # A second image claiming any of these devices/mountpoints is ambiguous.
    devices = {entry["dev-entry"] for entry in entries}
    if any(other is not row and any(entry["dev-entry"] in devices or
            entry.get("mount-point") == str(mount) for entry in _entities(other)) for other in after):
        raise ValueError("Another image claims the requested mountpoint or device")
    return {"image_path": str(source), "mount_point": str(mount),
            "device": mounted[0]["dev-entry"], "devices": sorted(devices),
            "entities": entries}


def _dmg(report: dict, tool: Path, environment: dict, timeout: float, relative: str):
    attempt, source = Path(report["attempt_path"]), Path(report["execution_input"]["path"])
    mount = attempt / "mount"
    mount.mkdir(mode=0o700)
    def info(label):
        if _sha256_file(tool) != report["tools"][str(tool)]:
            raise ValueError("hdiutil changed during the attempt")
        _, output = _run(report, label, [str(tool), "info", "-plist"], environment, timeout)
        return _images(_plist(output))
    before = info("info-before")
    report["mount_observations"] = {"before": before}
    if any(row["image-path"] == str(source) or any(entry.get("mount-point") == str(mount)
            for entry in _entities(row)) for row in before):
        raise ValueError("Mount destination/input copy already appears in hdiutil info")
    attached = None
    ownership = None
    identities = lambda entries: sorted((entry["dev-entry"], entry.get("mount-point", ""))
                                        for entry in entries)
    attach_attempted = False
    try:
        attach_attempted = True
        _, output = _run(report, "attach", [str(tool), "attach", "-readonly", "-nobrowse",
                            "-noautoopen", "-mountpoint", str(mount), "-plist", str(source)],
                            environment, timeout)
        attached = _entities(_plist(output))
        after = info("info-after")
        report["mount_observations"]["after"] = after
        ownership = _owned(before, after, source, mount)
        report["mount_ownership"] = ownership
        if identities(attached) != identities(ownership["entities"]):
            raise ValueError("Attach plist and independently observed mounted devices differ")
        report["attach_plist_matched"] = True
        _prepare_payload(report, _payload(mount, relative))
    finally:
        if attach_attempted:
            # Re-establish ownership even if attach timed out or returned malformed
            # output. Cleanup authority comes from the independent inventories.
            try:
                current = info("info-before-detach")
                report["mount_observations"]["before_detach"] = current
                observed = _owned(before, current, source, mount)
                if ownership is not None and identities(observed["entities"]) != identities(ownership["entities"]):
                    raise ValueError("Established mounted device identity changed before cleanup")
                if ownership is None:
                    # Attach may have timed out before an initial binding. In
                    # that case this independent inventory establishes it.
                    ownership = observed
                    report["mount_ownership"] = ownership
                _, _ = _run(report, "detach", [str(tool), "detach", ownership["device"]],
                            environment, timeout)
                final = info("info-after-detach")
                report["mount_observations"]["after_detach"] = final
                if any(row["image-path"] == str(source) or any(
                        entry["dev-entry"] in ownership["devices"] or entry.get("mount-point") == str(mount)
                        for entry in _entities(row)) for row in final):
                    raise ValueError("Owned image/device/mountpoint remains after detach")
                report["cleanup"] = {"status": "DETACHED", "device": ownership["device"],
                                     "confirmed_absent": True}
            except (Exception, KeyboardInterrupt) as error:
                report["cleanup"] = {"status": "FAILED_OR_UNPROVEN", "error": str(error)}
                report["errors"].append(f"DMG cleanup: {type(error).__name__}: {error}")


def _prepare_payload(report: dict, payload: Path):
    inventory = inspect_inventory(payload)
    prepared = prepare_package(payload, Path(report["attempt_path"]) / "payload",
                               expected_inventory_sha256=inventory["sha256"])
    report["preparation"] = prepared
    report["package_path"] = prepared["package_path"]
    report["payload_inventory"] = inventory
    _record_evidence(report, [Path(prepared["attempt_path"]) / "preparation.json"])


def prepare_container(input_path: str | Path, attempt_dir: str | Path, *,
                      container_format: str, expected_sha256: str,
                      payload_relative_path: str,
                      hdiutil_executable: str | Path | None = None,
                      timeout_seconds: float = 120) -> dict:
    """Return CONTAINER_PREPARED/CONTAINER_FAIL, always runtime NOT_RUN.

    Existing/unsafe attempt paths raise without writing there. Once this call
    creates an attempt, all controlled failures retain container-preparation.json.
    payload_relative_path='.' selects the whole actual volume/AppDir; nested
    paths must be explicit, canonical, contained directories.
    """
    attempt = _new_attempt(attempt_dir)
    report = {"schema": SCHEMA, "status": "CONTAINER_FAIL", "accepted": False,
              "runtime": "NOT_RUN", "scope": "container_identity_and_preparation",
              "attempt_path": str(attempt), "format": container_format,
              "host_platform": _host_platform(), "started_at": _now(),
              "input": {"path": str(input_path), "sha256_before": None, "sha256_after": None},
              "expected_sha256": expected_sha256, "payload_relative_path": payload_relative_path,
              "commands": [], "evidence": {}, "tools": {}, "errors": [],
              "cleanup": {"status": "NOT_NEEDED"}}
    try:
        if container_format not in {"dmg", "appimage"}:
            raise ValueError("Only explicit dmg/appimage formats are supported")
        if (not isinstance(expected_sha256, str) or
                not re.fullmatch(r"[0-9a-fA-F]{64}", expected_sha256)):
            raise ValueError("An externally locked SHA256 is mandatory")
        report["expected_sha256"] = expected_sha256.lower()
        if not isinstance(payload_relative_path, str) or not payload_relative_path:
            raise ValueError("An explicit payload-relative path is mandatory")
        if payload_relative_path != ".":
            for part in payload_relative_path.split("/"):
                _component(part)
        if isinstance(timeout_seconds, bool) or not math.isfinite(timeout_seconds) or timeout_seconds <= 0:
            raise ValueError("Container command timeout must be positive and finite")
        required_platform = "macos" if container_format == "dmg" else "linux"
        if report["host_platform"] != required_platform:
            raise ValueError(f"Actual {required_platform} host required for {container_format}; NOT_RUN")
        given = Path(input_path)
        if not given.is_absolute():
            raise ValueError("Container input path must be absolute")
        source = _plain_root(given)
        if source.is_relative_to(attempt):
            raise ValueError("Container input must precede and stay outside this attempt")
        report["input"]["path"] = str(source)
        digest = _sha256_file(source)
        report["input"]["sha256_before"] = digest
        if digest != report["expected_sha256"]:
            raise ValueError("Original container SHA256 differs from prelocked input")
        execution = attempt / ("locked-input.dmg" if container_format == "dmg" else "locked-input.AppImage")
        shutil.copyfile(source, execution)
        shutil.copymode(source, execution)
        report["execution_input"] = {"path": str(execution), "sha256_before": _sha256_file(execution)}
        if report["execution_input"]["sha256_before"] != digest or _sha256_file(source) != digest:
            raise ValueError("Container changed while making the isolated locked copy")
        environment = _environment(attempt)
        (attempt / "commands").mkdir(mode=0o700)
        python = Path(sys.executable).resolve(strict=True)
        report["tools"][str(python)] = _sha256_file(python)
        if container_format == "dmg":
            if hdiutil_executable is None or not Path(hdiutil_executable).is_absolute():
                raise ValueError("DMG requires an explicit absolute hdiutil executable")
            tool = _plain_root(Path(hdiutil_executable))
            if tool.is_relative_to(attempt):
                raise ValueError("hdiutil must be an independently supplied host tool")
            report["tools"][str(tool)] = _sha256_file(tool)
            _dmg(report, tool, environment, timeout_seconds, payload_relative_path)
        else:
            if hdiutil_executable is not None:
                raise ValueError("AppImage does not accept a hdiutil tool override")
            if not os.access(execution, os.X_OK):
                raise ValueError("The locked AppImage is not executable")
            _run(report, "appimage-extract", [str(execution), "--appimage-extract"],
                 environment, timeout_seconds)
            extracted = _plain_root(attempt / "work/squashfs-root", directory=True)
            # Check the complete extracted layout even when a nested payload was
            # requested, so discarded siblings cannot hide escaping links.
            report["extracted_inventory"] = inspect_inventory(extracted)
            _prepare_payload(report, _payload(extracted, payload_relative_path))
            report["cleanup"] = {"status": "NO_MOUNT", "owned_tree_cleanup": "COMPLETE"}
    except (Exception, KeyboardInterrupt) as error:
        if isinstance(error, PackageVerificationError) and error.report is not None:
            report["failed_payload_preparation"] = error.report
        report["errors"].append(f"{type(error).__name__}: {error}")
    finally:
        try:
            if report["input"]["sha256_before"] is not None:
                report["input"]["sha256_after"] = _sha256_file(Path(report["input"]["path"]))
                if report["input"]["sha256_after"] != report["expected_sha256"]:
                    raise ValueError("Original container changed during preparation")
            if "execution_input" in report:
                report["execution_input"]["sha256_after"] = _sha256_file(Path(report["execution_input"]["path"]))
                if report["execution_input"]["sha256_after"] != report["expected_sha256"]:
                    raise ValueError("Locked execution copy changed during preparation")
            for tool, digest in report["tools"].items():
                if _sha256_file(Path(tool)) != digest:
                    raise ValueError(f"Host tool changed during preparation: {tool}")
            # Check the original expected identities, including now-missing files.
            for relative, digest in report["evidence"].items():
                if _sha256_file(attempt / relative) != digest:
                    raise ValueError(f"Completed container evidence changed: {relative}")
            if "preparation" in report and inspect_inventory(report["package_path"]) != report["preparation"]["inventory"]:
                raise ValueError("Prepared payload changed before the final receipt")
        except (Exception, KeyboardInterrupt) as error:
            report["errors"].append(f"Final stability: {type(error).__name__}: {error}")
        if not report["errors"] and "preparation" in report and report["cleanup"]["status"] in {"DETACHED", "NO_MOUNT"}:
            report["status"] = "CONTAINER_PREPARED"
        report["finished_at"] = _now()
        _save(attempt / "container-preparation.json", report)
    return report


def verify_container_stable(report: dict) -> dict:
    """Recheck the retained original, execution copy, payload and evidence.

    The volume has already been detached. Do not call directory verify_stable
    against its vanished mountpoint. This consumes the caller's trusted result;
    the top-level validation lane binds that result's external provenance.
    """
    if (report.get("schema") != SCHEMA or report.get("status") != "CONTAINER_PREPARED"
            or report.get("runtime") != "NOT_RUN" or report.get("accepted") is not False):
        raise ValueError("Expected a successful container preparation receipt")
    if report["cleanup"]["status"] not in {"DETACHED", "NO_MOUNT"}:
        raise ValueError("Container cleanup is not complete")
    for item in (report["input"], report["execution_input"]):
        if _sha256_file(Path(item["path"])) != report["expected_sha256"]:
            raise ValueError("Container identity changed since preparation")
    if inspect_inventory(report["package_path"]) != report["preparation"]["inventory"]:
        raise ValueError("Prepared container payload changed since preparation")
    attempt = Path(report["attempt_path"])
    expected_receipt = (json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode("utf-8")
    if _sha256_file(attempt / "container-preparation.json") != hashlib.sha256(expected_receipt).hexdigest():
        raise ValueError("Final container preparation receipt changed since preparation")
    for relative, digest in report["evidence"].items():
        if _sha256_file(attempt / relative) != digest:
            raise ValueError(f"Container evidence changed since preparation: {relative}")
    for tool, digest in report["tools"].items():
        if _sha256_file(Path(tool)) != digest:
            raise ValueError("Container host tool changed since preparation")
    return {"schema": SCHEMA, "status": "STABLE", "runtime": "NOT_RUN",
            "input_stable": True, "package_stable": True, "checked_at": _now()}
