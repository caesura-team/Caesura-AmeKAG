#!/usr/bin/env python3
"""Build and validate explicitly named final CI packages before upload.

This orchestrates the real package validator; command-fixture tests are not
runtime acceptance. Every run owns a NEW directory outside every checkout.
The caller owns that directory until upload; this is a byte stability gate,
not hosted artifact provenance or permission to publish a release.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import sys
import tarfile
import time
import uuid
import zipfile

from package_verification import _component, _sha256_file, inspect_inventory
from run_validation import _source_identity
from validation_process import run_owned_command

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = "caesura.ci-package-lane.v1"
UPLOAD_SCHEMA = "caesura.package-upload.v2"


def _require(condition, message):
    if not condition:
        raise ValueError(message)


def _save(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def _read(path):
    _require(Path(path).stat().st_size <= 16 * 1024 * 1024, "JSON receipt exceeds limit")
    return json.loads(Path(path).read_text(encoding="utf-8"))


def _name(value):
    _require(isinstance(value, str) and "/" not in value and len(value) <= 200, "Expected one explicit filename")
    _component(value)
    return value


def _new_work(value):
    path = Path(value).absolute()
    _require(path.parent.is_dir(), "Work parent must already exist")
    for parent in (path.parent, *path.parent.parents):
        _require(not parent.is_symlink() and not parent.is_junction(), "Work parent must not traverse a link")
        _require(not (parent / ".git").exists(), "Work must be outside every checkout")
    _require(not ROOT.resolve().is_relative_to(path), "Work must not contain the checkout")
    path.mkdir()  # exclusive; an existing failed attempt is never reused
    return path.resolve()


def _identity():
    _require((ROOT / ".git").exists(), "Source checkout has no .git identity")
    return _source_identity(ROOT)


def _start(work_dir, platform, source_sha):
    work = _new_work(work_dir)
    (work / "outputs").mkdir(); (work / "commands").mkdir()
    return {"schema": SCHEMA, "status": "FAIL", "accepted": False,
            "platform": platform, "configuration": "Release", "source_sha": source_sha,
            "work": str(work), "locks": [], "commands": [], "validations": [], "errors": []}


def _source_start(report):
    _require(re.fullmatch(r"[0-9a-f]{40}", report["source_sha"] or ""), "Expected full lowercase source SHA")
    report["source_before"] = _identity()
    _require(report["source_before"]["source_sha"] == report["source_sha"], "Source SHA mismatch")
    _require(not report["source_before"]["dirty"], "CI acceptance requires clean source")
    cmake = _lock(report, ROOT / "CMakeLists.txt")
    source = Path(cmake["path"]).read_text(encoding="utf-8-sig")
    versions = re.findall(r"(?im)^\s*project\(\s*CaesuraAmeKAG\s+VERSION\s+(\d+\.\d+\.\d+(?:\.\d+)?)\s+LANGUAGES\b", source)
    _require(len(versions) == 1, "Expected one explicit engine project version in CMakeLists.txt")
    report["version"] = versions[0]
    report["provenance"] = _producer_context()


def _producer_context():
    # These are recorded claims from the controlled producer. U23 authenticates
    # them against GitHub separately; environment values alone prove no origin.
    context = {"authentication": "NOT_VERIFIED", "provider": "local"}
    if os.environ.get("GITHUB_ACTIONS") != "true":
        return context
    context["provider"] = "github-actions"
    for key, variable in (("repository", "GITHUB_REPOSITORY"), ("workflow_ref", "GITHUB_WORKFLOW_REF"),
                          ("workflow_sha", "GITHUB_WORKFLOW_SHA"), ("job_key", "GITHUB_JOB")):
        value = os.environ.get(variable, "")
        _require(value and len(value) <= 1024 and not any(c in value for c in "\r\n\x00"), "Missing/invalid producer context: " + variable)
        context[key] = value
    _require(re.fullmatch(r"[0-9a-f]{40}", context["workflow_sha"]), "Invalid producer workflow SHA")
    for key, variable in (("repository_id", "GITHUB_REPOSITORY_ID"), ("run_id", "GITHUB_RUN_ID"),
                          ("run_attempt", "GITHUB_RUN_ATTEMPT")):
        value = os.environ.get(variable, "")
        _require(re.fullmatch(r"[1-9][0-9]{0,19}", value), "Missing/invalid producer context: " + variable)
        context[key] = int(value)
    return context


def _lock(report, value, *, expected=None, executable=False):
    supplied = Path(value)
    _require(supplied.is_absolute(), "Input/tool path must be explicit and absolute")
    path = supplied.resolve(strict=True)
    _require(not executable or path.suffix.lower() not in (".cmd", ".bat"), "Use an actual executable")
    kind = "directory" if path.is_dir() else "file"
    sha = inspect_inventory(path)["sha256"] if kind == "directory" else _sha256_file(path)
    if expected is not None:
        _require(isinstance(expected, str) and re.fullmatch(r"[0-9a-f]{64}", expected), "Expected lowercase SHA256")
        _require(sha == expected, f"Prelocked identity mismatch: {path}")
    lock = {"path": str(path), "kind": kind, "sha256": sha}
    report["locks"].append(lock)
    return lock


def _check_lock(lock):
    path = Path(lock["path"])
    # Keep the canonical pathname as well as bytes; do not silently adopt a
    # replacement symlink resolving to a different input before upload.
    _require(str(path.resolve(strict=True)) == str(path), f"Canonical input path changed: {path}")
    actual = inspect_inventory(path)["sha256"] if lock["kind"] == "directory" else _sha256_file(path)
    _require(actual == lock["sha256"], f"Locked input changed: {path}")


def _stable(report):
    for lock in report["locks"]:
        _check_lock(lock)
    _require(_identity() == report["source_before"], "Source identity changed during package lane")


@contextmanager
def _environment(values):
    before = {key: os.environ.get(key) for key in values}
    try:
        os.environ.update(values)
        yield
    finally:
        for key, value in before.items():
            if value is None: os.environ.pop(key, None)
            else: os.environ[key] = value


def _execute(report, name, argv, cwd, *, env_delta=None, timeout=900):
    command = {"name": name, "argv": [str(arg) for arg in argv], "cwd": str(cwd),
               "timeout_seconds": timeout, "environment_delta": env_delta or {}}
    report["commands"].append(command)
    base = Path(report["work"]) / "commands" / name
    started = time.monotonic()
    try:
        with base.with_suffix(".stdout.log").open("xb") as stdout, base.with_suffix(".stderr.log").open("xb") as stderr:
            with _environment(env_delta or {}):
                command["exit_code"] = run_owned_command(command["argv"], cwd, stdout, stderr, timeout)
        return command["exit_code"]
    except Exception as error:
        command["error"] = f"{type(error).__name__}: {error}"
        raise
    finally:
        command["seconds"] = time.monotonic() - started
        _save(base.with_suffix(".json"), command)


def _run(report, name, argv, cwd, **kwargs):
    _require(_execute(report, name, [str(arg) for arg in argv], cwd, **kwargs) == 0,
             f"Command failed: {name}; see its original logs")


def _validate(report, name, artifact, python, extra):
    work = Path(report["work"])
    attempt = work / name
    argv = [python, ROOT / "scripts/run_package_validation.py", "--input", artifact["path"],
            "--inventory-sha256" if artifact["kind"] == "directory" else "--sha256", artifact["sha256"],
            "--attempt", attempt, "--platform", report["platform"], "--configuration", "Release",
            "--source-sha", report["source_sha"], *extra]
    _run(report, name, argv, ROOT, env_delta={"CI": "true"}, timeout=900)
    receipt = _lock(report, attempt / "package-run.json")
    value = _read(receipt["path"])
    _require(value.get("schema") == "caesura.package-validation.v1"
             and value.get("status") == "PASS" and value.get("accepted") is True,
             f"Package validation did not accept {name}")
    _require(value.get("expected_source_sha") == report["source_sha"]
             and value.get("platform") == report["platform"] and value.get("configuration") == "Release",
             "Package validation receipt context mismatch")
    if "--container-format" in extra:
        selected = value.get("container", {})
        observed = selected.get("expected_sha256")
    else:
        selected = value.get("preparation", {})
        observed = selected.get("expected", {}).get(
            "inventory_sha256" if artifact["kind"] == "directory" else "archive_sha256")
    _require(observed == artifact["sha256"], "Validation receipt does not bind the initial package digest")
    _require(selected.get("input", {}).get("path") == artifact["path"],
             "Validation receipt does not bind the selected input path")
    _check_lock(artifact)
    report["validations"].append({"name": name, "input": artifact, "receipt": receipt})


def _portable_file(report, original, name):
    _check_lock(original)
    path = Path(report["work"]) / "outputs" / _name(name)
    with Path(original["path"]).open("rb") as src, path.open("xb") as dst:
        shutil.copyfileobj(src, dst)
    copied = _lock(report, path, expected=original["sha256"])
    _check_lock(original)
    return {"name": path.name, "sha256": copied["sha256"]}


def _finish(report, artifacts):
    _stable(report)
    manifest = {"schema": UPLOAD_SCHEMA, "source_sha": report["source_sha"], "platform": report["platform"],
                "configuration": report["configuration"], "version": report["version"],
                "provenance": report["provenance"], "requirements": None, "validations": [],
                "files": [{"name": Path(item["path"]).name, "kind": item["kind"], "sha256": item["sha256"]}
                          for item in artifacts]}
    if report.get("requirements"):
        manifest["requirements"] = _portable_file(report, report["requirements"], "package-requirements.json")
    for artifact, declared in zip(artifacts, manifest["files"]):
        matches = [value for value in report["validations"] if value["input"] == artifact]
        _require(len(matches) == 1, "Each final file requires exactly one matching package validation")
        validation = matches[0]
        receipt = _portable_file(report, validation["receipt"], "receipt-" + validation["name"] + ".json")
        manifest["validations"].append({"name": validation["name"], "input": declared, "receipt": receipt})
    _stable(report)
    path = Path(report["work"]) / "outputs/upload-manifest.json"
    _save(path, manifest)
    report["manifest"] = _lock(report, path)
    report["artifacts"] = artifacts
    portable = [entry["receipt"] for entry in manifest["validations"]]
    if manifest["requirements"] is not None:
        portable.append(manifest["requirements"])
    report["upload_files"] = ([item["path"] for item in artifacts] + [str(path)]
        + [str(path.parent / item["name"]) for item in portable])
    report.update(status="PASS", accepted=True)


def run_native_lane(*, build_dir, requirements_path, platform, source_sha, work_dir,
                    cpack_executable, python_executable, hdiutil_executable=None,
                    appimagetool_path=None, appimagetool_sha256=None, runtime_path=None, runtime_sha256=None):
    report = _start(work_dir, platform, source_sha)
    try:
        _source_start(report)
        _require(platform in ("windows", "linux", "macos"), "Unknown desktop platform")
        build = Path(build_dir).resolve(strict=True)
        config = _lock(report, build / "CPackConfig.cmake")
        requirements = _lock(report, Path(requirements_path).absolute())
        metadata = _read(requirements["path"])
        _require(metadata.get("schema") == "caesura.package-build.v1" and metadata.get("platform") == platform
                 and metadata.get("configuration") == "Release", "CPack requirements context mismatch")
        _require(metadata.get("version") == report["version"], "CPack requirements version differs from engine CMake version")
        report["requirements"] = requirements
        names = {fmt: _name(metadata["artifacts"][fmt]) for fmt in ("zip", "tgz", "dmg", "appimage")}
        _name(metadata["archive_basename"])
        _require(len(set(names.values())) == 4, "Artifact names must be distinct")
        cpack = _lock(report, cpack_executable, executable=True)["path"]
        python = _lock(report, python_executable, executable=True)["path"]
        if platform == "macos":
            hdiutil = _lock(report, hdiutil_executable, executable=True)["path"]
        if platform == "linux":
            _require(appimagetool_sha256 is not None and runtime_sha256 is not None, "AppImage needs both external builder pins")
            tool = _lock(report, appimagetool_path, expected=appimagetool_sha256, executable=True)
            runtime = _lock(report, runtime_path, expected=runtime_sha256)
        extra = ["--requirements", requirements["path"], "--requirements-sha256", requirements["sha256"], "--python", python]
        outputs = Path(report["work"]) / "outputs"
        artifacts = []
        formats = ("zip",) if platform == "windows" else ("tgz", "appimage" if platform == "linux" else "dmg")
        for fmt in formats:
            output = outputs / names[fmt]
            _require(not output.exists(), "Expected final output is already occupied")
            if fmt == "appimage":
                tgz = artifacts[0]
                _run(report, "appimage-build", [python, ROOT / "scripts/build_appimage.py", "--tgz", tgz["path"],
                    "--sha256", tgz["sha256"], "--requirements", requirements["path"],
                    "--requirements-sha256", requirements["sha256"], "--appimagetool", tool["path"],
                    "--appimagetool-sha256", tool["sha256"], "--runtime-file", runtime["path"],
                    "--runtime-sha256", runtime["sha256"], "--work", Path(report["work"]) / "appimage-build",
                    "--output", output], ROOT)
            else:
                # This engine is a relocatable directory, not an .app bundle.
                # DragNDrop otherwise adds an absolute /Applications shortcut,
                # which violates the complete contained-package inventory.
                cpack_options = ["-D", "CPACK_DMG_DISABLE_APPLICATIONS_SYMLINK=ON"] if fmt == "dmg" else []
                _run(report, "cpack-" + fmt, [cpack, "--config", config["path"], "-C", "Release", "-G",
                    {"zip": "ZIP", "tgz": "TGZ", "dmg": "DragNDrop"}[fmt], "-B", outputs, *cpack_options], build)
            artifact = _lock(report, output)
            selected = list(extra)
            if fmt in ("dmg", "appimage"):
                selected.extend(["--container-format", fmt, "--payload-relative-path", "."])
            if fmt == "dmg": selected.extend(["--hdiutil", hdiutil])
            _validate(report, "validate-" + fmt, artifact, python, selected)
            artifacts.append(artifact)
        _finish(report, artifacts)
    except Exception as error:
        report["errors"].append(f"{type(error).__name__}: {error}")
    finally:
        _save(Path(report["work"]) / "lane.json", report)
    return report


def _pages_source(site, entry):
    path = site / entry["path"]
    info = path.lstat()
    _require(entry["type"] in ("directory", "file")
             and not getattr(info, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400),
             "Pages tar requires plain directories and regular files")
    _require((entry["type"] == "directory" and stat.S_ISDIR(info.st_mode))
             or (entry["type"] == "file" and stat.S_ISREG(info.st_mode) and info.st_nlink == 1),
             "Pages tar does not support links, hardlinks or special files")
    return path, info


def _write_pages_tar(site, inventory, output):
    class HashedReader:
        def __init__(self, stream):
            self.stream, self.digest = stream, hashlib.sha256()
        def read(self, size=-1):
            data = self.stream.read(size)
            self.digest.update(data)
            return data

    # Every member comes from the same frozen inventory used for the ZIP. Never
    # recursively discover extra paths, dereference links or overwrite a tar.
    with tarfile.open(output, "x", format=tarfile.PAX_FORMAT) as archive:
        for entry in inventory["entries"]:
            path, info = _pages_source(site, entry)
            member = archive.gettarinfo(str(path), arcname=entry["path"])
            _require((entry["type"] == "directory" and member.isdir())
                     or (entry["type"] == "file" and member.isfile()), "Pages member changed type")
            if member.isdir():
                archive.addfile(member)
            else:
                with path.open("rb") as stream:
                    opened = os.fstat(stream.fileno())
                    _require((opened.st_dev, opened.st_ino, opened.st_size, opened.st_nlink)
                             == (info.st_dev, info.st_ino, entry["size"], 1), "Pages input changed while opening")
                    reader = HashedReader(stream)
                    archive.addfile(member, reader)
                    _require(reader.digest.hexdigest() == entry["sha256"], "Pages member differs from the locked site inventory")


def run_web_lane(*, game, source_sha, work_dir, node_executable, lua_executable, browser_executable,
                 zip_name=None, actions_path=None, actions_sha256=None, pages_tar=False):
    report = _start(work_dir, "web", source_sha)
    try:
        _require(isinstance(pages_tar, bool), "Pages tar selection must be a boolean")
        _require(not pages_tar or zip_name, "Pages tar requires an explicit final ZIP name")
        _source_start(report)
        chosen = Path(game) if Path(game).is_absolute() else ROOT / game
        chosen = chosen.resolve(strict=True)
        _require(chosen.is_relative_to(ROOT.resolve()), "Chosen game must be in the source checkout")
        _lock(report, chosen)
        node = _lock(report, node_executable, executable=True)["path"]
        lua = _lock(report, lua_executable, executable=True)["path"]
        browser = _lock(report, browser_executable, executable=True)["path"]
        python = _lock(report, Path(sys.executable).resolve(), executable=True)["path"]
        vite = _lock(report, ROOT / "web/node_modules/vite/bin/vite.js")["path"]
        extra = ["--node", node, "--lua", lua, "--browser", browser]
        _require((actions_path is None) == (actions_sha256 is None), "Actions path and external SHA must be paired")
        if actions_path is not None:
            action = _lock(report, Path(actions_path).absolute(), expected=actions_sha256)
            extra.extend(["--actions", action["path"], "--actions-sha256", action["sha256"]])
        if zip_name is not None: _require(_name(zip_name).endswith(".zip"), "Web final archive must be ZIP")
        site = Path(report["work"]) / "outputs/site"
        _run(report, "web-bake", [lua, ROOT / "scripts/ks_bake.lua", "--dir", ROOT / "demo", "--web", ROOT / "cache/story"], ROOT)
        _run(report, "web-build", [node, vite, "build"], ROOT / "web")
        _run(report, "web-package", [node, ROOT / "scripts/package_game.mjs", "--no-web-build", "--out", site, chosen], ROOT,
             env_delta={"CAESURA_LUA": lua})
        directory = _lock(report, site)
        report["site"] = str(site)
        _validate(report, "validate-web-directory", directory, python, extra)
        artifacts = [directory]
        if zip_name:
            inventory = inspect_inventory(site)
            _require(inventory["sha256"] == directory["sha256"], "Site changed before final ZIP transform")
            output = site.parent / zip_name
            with zipfile.ZipFile(output, "x", compression=zipfile.ZIP_DEFLATED) as archive:
                for entry in inventory["entries"]:
                    _require(entry["type"] in ("directory", "file"), "Web ZIP does not support linked content")
                    archive.write(site / entry["path"], entry["path"])
            _check_lock(directory)
            artifact = _lock(report, output)
            _validate(report, "validate-web-zip", artifact, python, extra)
            artifacts = [artifact]
            if pages_tar:
                _check_lock(directory)
                output = site.parent / "artifact.tar"
                _write_pages_tar(site, inventory, output)
                _check_lock(directory)
                artifact = _lock(report, output)
                _validate(report, "validate-pages-tar", artifact, python, extra)
                for entry in inventory["entries"]:
                    _pages_source(site, entry)
                artifacts.append(artifact)
                report["pages_file"] = str(output)
        _finish(report, artifacts)
    except Exception as error:
        report["errors"].append(f"{type(error).__name__}: {error}")
    finally:
        _save(Path(report["work"]) / "lane.json", report)
    return report


def verify_lane(receipt_path, expected_receipt_sha):
    path = Path(receipt_path).resolve(strict=True)
    _require(_sha256_file(path) == expected_receipt_sha, "Lane receipt changed before upload")
    report = _read(path)
    _require(report.get("schema") == SCHEMA and report.get("status") == "PASS" and report.get("accepted") is True,
             "Only an accepted package lane can be uploaded")
    _stable(report)
    return {"status": "UPLOAD_READY", "receipt": str(path), "receipt_sha256": expected_receipt_sha}


def collect_bundles(*, bundles, source_sha, work_dir):
    """Verify named downloaded bundles against hashes from these producer jobs.

    This transfers already validated bytes. U23 independently owns workflow and
    artifact provenance authentication; this function makes no such claim.
    """
    work = _new_work(work_dir)
    files = []
    for directory, expected in bundles:
        root = Path(directory).resolve(strict=True)
        manifest = root / "upload-manifest.json"
        _require(_sha256_file(manifest) == expected, "Downloaded manifest differs from producer job output")
        value = _read(manifest)
        _require(value.get("schema") == UPLOAD_SCHEMA and value.get("source_sha") == source_sha, "Downloaded bundle context mismatch")
        _require(isinstance(value.get("files"), list) and 0 < len(value["files"]) <= 4, "Invalid explicit bundle file list")
        for entry in value["files"]:
            name = _name(entry["name"])
            _require(entry["kind"] == "file", "Release collection requires final archives")
            _require(name.casefold() != "checksums.txt", "Package name conflicts with generated checksums")
            _require(name.casefold() not in {item["name"].casefold() for item in files}, "Duplicate release filename")
            source = root / name
            _require(_sha256_file(source) == entry["sha256"], "Downloaded final package digest mismatch")
            with source.open("rb") as src, (work / name).open("xb") as dst:
                shutil.copyfileobj(src, dst)
            _require(_sha256_file(source) == entry["sha256"] and _sha256_file(work / name) == entry["sha256"], "Package changed during collection")
            files.append(entry)
    _require(files, "No explicit bundles supplied")
    checksums = work / "checksums.txt"
    checksums.write_text("".join(f'{item["sha256"]}  {item["name"]}\n' for item in files), encoding="utf-8")
    return {"status": "COLLECTED", "upload_files": [str(work / item["name"]) for item in files] + [str(checksums)]}


def _outputs(path, values):
    if path is None: return
    with Path(path).open("a", encoding="utf-8") as stream:
        for key, value in values.items():
            delimiter = "caesura_" + uuid.uuid4().hex
            stream.write(f"{key}<<{delimiter}\n{value}\n{delimiter}\n")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("native", "web"):
        part = commands.add_parser(name)
        part.add_argument("--source-sha", required=True)
        part.add_argument("--work", type=Path, required=True)
        part.add_argument("--github-output", type=Path)
        if name == "native":
            for argument in ("build", "requirements", "cpack", "python"):
                part.add_argument("--" + argument, type=Path, required=True)
            part.add_argument("--platform", choices=("windows", "linux", "macos"), required=True)
            for argument in ("hdiutil", "appimagetool", "runtime"):
                part.add_argument("--" + argument, type=Path)
            for argument in ("appimagetool-sha256", "runtime-sha256"):
                part.add_argument("--" + argument)
        else:
            part.add_argument("--game", type=Path, required=True)
            for argument in ("node", "lua", "browser"):
                part.add_argument("--" + argument, type=Path, required=True)
            part.add_argument("--zip-name")
            part.add_argument("--pages-tar", action="store_true")
            part.add_argument("--actions", type=Path)
            part.add_argument("--actions-sha256")
    verify = commands.add_parser("verify")
    verify.add_argument("--receipt", type=Path, required=True)
    verify.add_argument("--sha256", required=True)
    collect = commands.add_parser("collect")
    collect.add_argument("--bundle", nargs=2, action="append", required=True, metavar=("DIRECTORY", "MANIFEST_SHA256"))
    collect.add_argument("--work", type=Path, required=True)
    collect.add_argument("--source-sha", required=True)
    collect.add_argument("--github-output", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "verify": result = verify_lane(args.receipt, args.sha256)
        elif args.command == "collect":
            result = collect_bundles(bundles=args.bundle, source_sha=args.source_sha, work_dir=args.work)
            _outputs(args.github_output, {"upload_files": "\n".join(result["upload_files"])})
        else:
            common = dict(source_sha=args.source_sha, work_dir=args.work)
            if args.command == "native":
                result = run_native_lane(**common, build_dir=args.build, requirements_path=args.requirements,
                    platform=args.platform, cpack_executable=args.cpack, python_executable=args.python,
                    hdiutil_executable=args.hdiutil, appimagetool_path=args.appimagetool,
                    appimagetool_sha256=args.appimagetool_sha256, runtime_path=args.runtime, runtime_sha256=args.runtime_sha256)
            else:
                result = run_web_lane(**common, game=args.game, node_executable=args.node, lua_executable=args.lua,
                    browser_executable=args.browser, zip_name=args.zip_name, actions_path=args.actions,
                    actions_sha256=args.actions_sha256, pages_tar=args.pages_tar)
            if result["status"] == "PASS":
                receipt = Path(result["work"]) / "lane.json"
                _outputs(args.github_output, {"receipt": str(receipt), "receipt_sha256": _sha256_file(receipt),
                    "manifest_sha256": result["manifest"]["sha256"], "site": result.get("site", ""),
                    "pages_file": result.get("pages_file", ""),
                    "upload_files": "\n".join(result["upload_files"])})
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0 if result["status"] in ("PASS", "UPLOAD_READY", "COLLECTED") else 1
    except Exception as error:
        print(f"{type(error).__name__}: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
