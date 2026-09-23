#!/usr/bin/env python3
"""U22 final-package identity and isolated extraction, using only the stdlib.

PREPARED/STABLE describe filesystem contracts, never platform/runtime acceptance.
The caller supplies an independently locked archive SHA256 (ZIP/TGZ) or directory
inventory SHA256. No discovery, self-authorizing package manifest, or runtime is
used. A new attempt contains package/ and an exclusive preparation.json; failed
attempts retain the receipt and remove only their partial package.

Inventory v1 hashes canonical UTF-8 JSON of sorted path/type/content/link-target
entries, including empty directories. It excludes times, ownership, permissions,
and hardlink aliasing; observed permission bits are recorded separately. Archive
SHA256 binds *all* distribution bytes, including the archived metadata. Runtime
lanes must separately check executable permissions and actual dependency loading.

Relative symlink chains and regular-file tar hardlinks are supported, including
SONAME and macOS framework links. All targets must resolve within the package.
Member names use a portable spelling policy (no case/Unicode-normalization
collisions, Windows device/stream names, traversal, or backslashes). The attempt
parent must already exist and must not traverse a symlink/reparse point. This is
a private-workspace operation, not protection against an adversary concurrently
renaming the attempt's parent; callers must own that parent for the whole run.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import tarfile
from typing import BinaryIO
import unicodedata
import zipfile

SCHEMA = "caesura.package-preparation.v1"
INVENTORY_SCHEMA = "caesura.package-content-inventory.v1"
SCOPE = "package_identity_and_extraction"
MAX_ENTRIES = 200_000
MAX_BYTES = 64 * 1024**3
MAX_LINK_BYTES = 64 * 1024


class PackageVerificationError(RuntimeError):
    def __init__(self, message: str, report: dict | None = None):
        super().__init__(message)
        self.report = report


def _fail(message: str):
    raise PackageVerificationError(message)


def _now() -> str:
    return datetime.now(timezone.utc).isoformat()


def _canonical(value) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":")).encode("utf-8")


def _signature(info: os.stat_result) -> tuple:
    # Windows lstat/fstat can disagree on ctime for the same newly-written
    # handle; it is not the portable POSIX metadata-change clock there.
    # Path stat also infers execute bits from .exe/.bat/etc., unlike fstat.
    # These inferred bits are not file permissions. Keep file type/read/write
    # bits and all content-change signatures; POSIX modes remain exact.
    mode = info.st_mode & ~0o111 if os.name == "nt" else info.st_mode
    stable = (info.st_dev, info.st_ino, mode, info.st_size, info.st_mtime_ns)
    return stable if os.name == "nt" else (*stable, info.st_ctime_ns)


def _reparse(path: Path) -> bool:
    info = path.lstat()
    return bool(getattr(info, "st_file_attributes", 0) &
                getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400))


def _plain_root(path: Path, *, directory: bool = False) -> Path:
    info = path.lstat()
    if stat.S_ISLNK(info.st_mode) or _reparse(path):
        _fail(f"Input root must not be a symlink/reparse point: {path}")
    if directory and not stat.S_ISDIR(info.st_mode):
        _fail(f"Expected a directory: {path}")
    if not (stat.S_ISDIR(info.st_mode) or stat.S_ISREG(info.st_mode)):
        _fail(f"Unsupported input type: {path}")
    return path.resolve(strict=True)


def _sha256_file(path: Path) -> str:
    before = path.lstat()
    if not stat.S_ISREG(before.st_mode) or _reparse(path):
        _fail(f"Expected a plain regular file: {path}")
    with path.open("rb") as stream:
        opened = os.fstat(stream.fileno())
        if _signature(before) != _signature(opened):
            _fail(f"Input changed while opening: {path}")
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
        after = os.fstat(stream.fileno())
    if _signature(before) != _signature(after) or _signature(after) != _signature(path.lstat()):
        _fail(f"Input changed while hashing: {path}")
    return digest


def _expected(value: str | None, name: str, *, required: bool = True) -> str | None:
    if value is None and not required:
        return None
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", value):
        _fail(f"External {name} identity must be a SHA256 hex string")
    return value.lower()


def _component(value: str):
    if (not value or value in (".", "..") or value[-1] in " ."
            or any(ord(c) < 32 or ord(c) == 127 or c in '\\:*?"<>|' for c in value)
            or re.fullmatch(r"(?i)(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\..*)?", value)):
        _fail(f"Unsafe package path component: {value!r}")
    try:
        value.encode("utf-8")
    except UnicodeError:
        _fail("Package paths must be valid UTF-8 text")


def _name(value: str, *, root_directory: bool = False) -> str:
    if not isinstance(value, str) or value.startswith("/") or "\\" in value:
        _fail(f"Unsafe package path: {value!r}")
    while value.startswith("./"):
        value = value[2:]
    value = value.rstrip("/")
    if value in ("", ".") and root_directory:
        return ""
    for part in value.split("/"):
        _component(part)
    return value


@dataclass
class _Entry:
    path: str
    kind: str
    size: int = 0
    target: str | None = None
    mode: int = 0o644
    source: object = None
    signature: tuple | None = None


def _plan(entries: list[_Entry]) -> dict[str, _Entry]:
    if len(entries) > MAX_ENTRIES or sum(e.size for e in entries) > MAX_BYTES:
        _fail("Package exceeds extraction entry/byte limits")
    plan: dict[str, _Entry] = {}
    spelling: dict[str, str] = {}
    for entry in entries:
        entry.path = _name(entry.path, root_directory=entry.kind == "directory")
        if entry.path in plan:
            _fail(f"Duplicate archive path: {entry.path!r}")
        plan[entry.path] = entry
    for name in list(plan):
        parts = name.split("/")
        for count in range(1, len(parts)):
            parent = "/".join(parts[:count])
            if parent in plan and plan[parent].kind != "directory":
                _fail(f"Entry has a non-directory/link ancestor: {name!r}")
            plan.setdefault(parent, _Entry(parent, "directory", mode=0o755))
    plan.setdefault("", _Entry("", "directory", mode=0o755))
    if len(plan) > MAX_ENTRIES:
        _fail("Package exceeds extraction entry limit after parent expansion")
    for name in plan:
        folded = unicodedata.normalize("NFC", name).casefold()
        if folded in spelling and spelling[folded] != name:
            _fail(f"Colliding package path spellings: {spelling[folded]!r}, {name!r}")
        spelling[folded] = name
    for entry in plan.values():
        if entry.kind in ("symlink", "hardlink"):
            resolved = _resolve_link(entry.path, plan)
            if entry.kind == "hardlink" and plan[resolved].kind != "file":
                _fail(f"Hardlink does not resolve to a regular file: {entry.path}")
    return plan


def _link_parts(target: str) -> list[str]:
    if not target or target.startswith("/") or "\\" in target or len(target.encode("utf-8")) > MAX_LINK_BYTES:
        _fail(f"Unsafe link target: {target!r}")
    parts = target.split("/")
    for part in parts:
        if part not in (".", "..", ""):
            _component(part)
    return parts


def _resolve_link(name: str, plan: dict[str, _Entry]) -> str:
    pending = name.split("/") if name else []
    resolved: list[str] = []
    visited: set[tuple] = set()
    expansions = 0
    while pending:
        part = pending.pop(0)
        if part in ("", "."):
            continue
        if part == "..":
            if not resolved:
                _fail(f"Link escapes package: {name}")
            resolved.pop()
            continue
        resolved.append(part)
        current = "/".join(resolved)
        entry = plan.get(current)
        if entry is None:
            _fail(f"Link target is missing from package: {name} -> {current}")
        if entry.kind in ("symlink", "hardlink"):
            state = (current, tuple(pending))
            expansions += 1
            if state in visited or expansions > 128:
                _fail(f"Cyclic or excessive link chain: {name}")
            visited.add(state)
            pending = _link_parts(entry.target) + pending
            resolved = resolved[:-1] if entry.kind == "symlink" else []
        elif pending and entry.kind != "directory":
            _fail(f"Link traverses a non-directory: {name}")
    return "/".join(resolved)


def _directory_entries(root: Path) -> list[_Entry]:
    entries = []
    def walk(directory: Path):
        for child in sorted(directory.iterdir(), key=lambda p: p.name):
            info = child.lstat()
            name = child.relative_to(root).as_posix()
            mode = stat.S_IMODE(info.st_mode)
            if stat.S_ISLNK(info.st_mode):
                target = os.readlink(child)
                # Windows reparse targets need native separators to work; the
                # inventory represents those separators as portable '/'.
                if os.name == "nt":
                    target = target.replace("\\", "/")
                entries.append(_Entry(name, "symlink", target=target, mode=mode,
                                      signature=_signature(info)))
            elif _reparse(child):
                _fail(f"Unsupported directory reparse point: {name}")
            elif stat.S_ISDIR(info.st_mode):
                entries.append(_Entry(name, "directory", mode=mode, signature=_signature(info)))
                walk(child)
            elif stat.S_ISREG(info.st_mode):
                entries.append(_Entry(name, "file", size=info.st_size, mode=mode, source=child,
                                      signature=_signature(info)))
            else:
                _fail(f"Unsupported filesystem entry: {name}")
    walk(root)
    return entries


def inspect_inventory(directory: str | Path) -> dict:
    """Return a deterministic content identity; this does not authorize its use."""
    root = _plain_root(Path(directory), directory=True)
    entries = _directory_entries(root)
    plan = _plan(entries)
    records, modes = [], []
    for name, entry in sorted(plan.items()):
        if not name:
            continue
        record = {"path": name, "type": entry.kind}
        if entry.kind == "file":
            record.update(size=entry.size, sha256=_sha256_file(root / name))
            if (root / name).stat().st_size != entry.size:
                _fail(f"Directory input changed while inventorying: {name}")
        elif entry.kind == "symlink":
            record["target"] = entry.target
        records.append(record)
        modes.append({"path": name, "mode": oct(entry.mode)})
    # Keep the initial stat signatures through this final sweep: a same-size
    # edit/replacement of an earlier file after its hash would otherwise be
    # invisible in an unchanged path/type/size listing. Signatures are only
    # in-flight consistency checks, not part of the portable content digest.
    def structure(items):
        return [(e.path, e.kind, e.size, e.target, e.mode, e.signature) for e in items]
    if structure(entries) != structure(_directory_entries(root)):
        _fail("Directory input changed while inventorying its entry set")
    identity = {"schema": INVENTORY_SCHEMA, "entries": records}
    return {**identity, "sha256": hashlib.sha256(_canonical(identity)).hexdigest(),
            "observed_modes": modes}


def _copy_stream(stream: BinaryIO, destination: Path, expected_size: int):
    written = 0
    with destination.open("xb") as output:
        while chunk := stream.read(min(1024 * 1024, expected_size - written + 1)):
            written += len(chunk)
            if written > expected_size:
                _fail(f"Extracted member exceeds declared size: {destination.name}")
            output.write(chunk)
    if written != expected_size:
        _fail(f"Truncated package member: {destination.name}")


@contextmanager
def _archive_plan(source: Path, expected_sha256: str):
    with source.open("rb") as stream:
        before = os.fstat(stream.fileno())
        if hashlib.file_digest(stream, "sha256").hexdigest() != expected_sha256:
            _fail("Archive identity changed before extraction")
        stream.seek(0)
        if zipfile.is_zipfile(stream):
            stream.seek(0)
            with zipfile.ZipFile(stream) as archive:
                entries = []
                for info in archive.infolist():
                    mode = (info.external_attr >> 16) & 0xFFFF
                    kind = "directory" if info.is_dir() else "file"
                    file_type = stat.S_IFMT(mode)
                    if info.flag_bits & 1:
                        _fail(f"Encrypted ZIP member unsupported: {info.filename}")
                    if info.create_system == 3 and file_type == stat.S_IFLNK:
                        kind = "symlink"
                    elif file_type not in (0, stat.S_IFREG, stat.S_IFDIR):
                        _fail(f"Unsupported ZIP entry type: {info.filename}")
                    if info.orig_filename != info.filename:
                        _fail("ZIP path contains a NUL character")
                    target = None
                    if kind == "symlink":
                        if info.file_size > MAX_LINK_BYTES:
                            _fail("ZIP link target exceeds size limit")
                        target = archive.read(info).decode("utf-8")
                    entries.append(_Entry(info.filename, kind, info.file_size, target,
                                          stat.S_IMODE(mode) or (0o755 if kind == "directory" else 0o644), info))
                yield _plan(entries), archive.open
        else:
            stream.seek(0)
            with tarfile.open(fileobj=stream, mode="r:gz") as archive:
                entries = []
                for info in archive:
                    if info.isdir(): kind = "directory"
                    elif info.isfile(): kind = "file"
                    elif info.issym(): kind = "symlink"
                    elif info.islnk(): kind = "hardlink"
                    else: _fail(f"Unsupported TGZ entry type: {info.name}")
                    entries.append(_Entry(info.name, kind, info.size,
                                          info.linkname if kind in ("symlink", "hardlink") else None,
                                          stat.S_IMODE(info.mode), info))
                    if len(entries) > MAX_ENTRIES:
                        _fail("TGZ exceeds entry limit")
                yield _plan(entries), archive.extractfile
        stream.seek(0)
        after_digest = hashlib.file_digest(stream, "sha256").hexdigest()
        if after_digest != expected_sha256 or _signature(before) != _signature(os.fstat(stream.fileno())):
            _fail("Archive input changed during extraction")
        if _signature(before) != _signature(source.lstat()):
            _fail("Archive input path changed during extraction")


def _materialize(plan: dict[str, _Entry], destination: Path, opener):
    destination.mkdir()
    for name, entry in sorted(plan.items(), key=lambda item: (item[0].count("/"), item[0])):
        if name and entry.kind == "directory":
            (destination / name).mkdir()
    for name, entry in sorted(plan.items()):
        if entry.kind == "file":
            with opener(entry.source) as stream:
                _copy_stream(stream, destination / name, entry.size)
    # No content is ever written through a link. Resolve hardlinks directly to
    # the already-created regular file, then create symlinks in any order.
    for name, entry in sorted(plan.items()):
        if entry.kind == "hardlink":
            os.link(destination / _resolve_link(name, plan), destination / name)
    for name, entry in sorted(plan.items()):
        if entry.kind == "symlink":
            target_is_directory = plan[_resolve_link(name, plan)].kind == "directory"
            try:
                target = entry.target.replace("/", os.sep)
                os.symlink(target, destination / name, target_is_directory=target_is_directory)
            except OSError as error:
                _fail(f"Host could not preserve package symlink {name!r}: {error}")
    # Preserve executable/read bits on POSIX, without applying setuid/setgid or
    # ownership from an archive. Windows modes are observations, not POSIX proof.
    if os.name != "nt":
        for name, entry in sorted(plan.items(), key=lambda item: (-item[0].count("/"), item[0])):
            if name and entry.kind in ("file", "directory"):
                os.chmod(destination / name, entry.mode & 0o777)


def _new_attempt(source: Path, attempt: Path) -> tuple[Path, Path]:
    attempt = Path(os.path.abspath(attempt))
    if os.path.lexists(attempt):
        _fail(f"Attempt already exists; refusing any write: {attempt}")
    for parent in [attempt.parent, *attempt.parents]:
        if parent.is_symlink() or _reparse(parent):
            _fail(f"Attempt parent must not traverse links/reparse points: {parent}")
    parent = attempt.parent.resolve(strict=True)
    if not parent.is_dir():
        _fail("Attempt parent must be an existing directory")
    attempt = parent / attempt.name
    source_absolute = Path(os.path.abspath(source))
    source_resolved = source_absolute.resolve(strict=False)
    if attempt == source_resolved or source_resolved in attempt.parents:
        _fail("Attempt must be outside the input package")
    attempt.mkdir(mode=0o700)  # Exclusive ownership; never exist_ok=True.
    return source_absolute, attempt


def _cleanup_package(package: Path, attempt: Path) -> dict:
    if not os.path.lexists(package):
        return {"status": "NOT_NEEDED"}
    try:
        if (package.parent != attempt or package.name != "package"
                or package.is_symlink() or _reparse(package)
                or package.resolve(strict=True).parent != attempt.resolve(strict=True)):
            _fail("Cleanup target no longer belongs to this attempt")
        # Only this exclusive attempt's child, never input or previous attempts.
        shutil.rmtree(package)
        if os.path.lexists(package):
            _fail("Partial package remains after cleanup")
        return {"status": "REMOVED_PARTIAL_PACKAGE"}
    except Exception as error:
        return {"status": "FAILED", "error": str(error)}


def prepare_package(source: str | Path, attempt: str | Path, *,
                    expected_sha256: str | None = None,
                    expected_inventory_sha256: str | None = None) -> dict:
    """Verify explicit input and prepare a new isolated copy, or raise with receipt.

    The existing attempt refusal deliberately has no receipt: writing one there
    would erase or contaminate an earlier attempt. Other failures are recorded
    only after exclusive creation of the new attempt directory.
    """
    try:
        source, attempt = _new_attempt(Path(source), Path(attempt))
    except (OSError, RuntimeError) as error:
        raise PackageVerificationError(str(error)) from error
    package = attempt / "package"
    report = {"schema": SCHEMA, "scope": SCOPE, "status": "FAIL", "runtime": "NOT_RUN",
              "started_at": _now(), "attempt_path": str(attempt), "package_path": str(package),
              "input": {"path": str(source), "archive_sha256": None},
              "expected": {"archive_sha256": expected_sha256,
                           "inventory_sha256": expected_inventory_sha256},
              "input_stable": False, "cleanup": {"status": "NOT_NEEDED"}}
    try:
        source = _plain_root(source)
        report["input"]["path"] = str(source)
        expected_inventory_sha256 = _expected(expected_inventory_sha256, "inventory", required=source.is_dir())
        if source.is_dir():
            if expected_sha256 is not None:
                _fail("Directory inputs use inventory identity, not archive SHA256")
            before = inspect_inventory(source)
            report["input"].update(kind="directory", inventory=before)
            if before["sha256"] != expected_inventory_sha256:
                _fail("Directory inventory identity mismatch")
            _materialize(_plan(_directory_entries(source)), package, lambda path: path.open("rb"))
            after = inspect_inventory(source)
            report["input_stable"] = before == after
            if not report["input_stable"]:
                _fail("Directory input changed during preparation")
        else:
            expected_sha256 = _expected(expected_sha256, "archive")
            before = _sha256_file(source)
            report["input"].update(kind="archive", archive_sha256=before)
            if before != expected_sha256:
                _fail("Archive identity mismatch")
            with _archive_plan(source, before) as (plan, opener):
                _materialize(plan, package, opener)
            report["input_stable"] = _sha256_file(source) == before
            if not report["input_stable"]:
                _fail("Archive input changed during preparation")
        inventory = inspect_inventory(package)
        report["inventory"] = inventory
        if expected_inventory_sha256 is not None and inventory["sha256"] != expected_inventory_sha256:
            _fail("Extracted inventory identity mismatch")
        report["status"] = "PREPARED"
        report["cleanup"] = {"status": "RETAINED_FOR_CALLER"}
    except (Exception, KeyboardInterrupt) as error:
        report["error"] = str(error) or type(error).__name__
        report["cleanup"] = _cleanup_package(package, attempt)
    report["finished_at"] = _now()
    with (attempt / "preparation.json").open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(report, stream, ensure_ascii=False, indent=2, sort_keys=True)
        stream.write("\n")
    if report["status"] != "PREPARED":
        raise PackageVerificationError(report["error"], report)
    return report


def verify_stable(report: dict) -> dict:
    """Recheck the input and exact prepared contents; never rewrites old evidence.

    This consumes the caller's trusted preparation result. It is not a receipt
    signature validator; later U22/U23 integration binds evidence provenance.
    Runtime writes must live outside package/ or be handled by a separate lane.
    """
    if (report.get("schema") != SCHEMA or report.get("scope") != SCOPE
            or report.get("status") != "PREPARED" or report.get("runtime") != "NOT_RUN"):
        _fail("Expected a successful package preparation receipt")
    source = _plain_root(Path(report["input"]["path"]))
    if report["input"]["kind"] == "archive":
        if _sha256_file(source) != report["input"]["archive_sha256"]:
            _fail("Archive identity changed since preparation")
    elif inspect_inventory(source) != report["input"]["inventory"]:
        _fail("Directory input changed since preparation")
    if inspect_inventory(report["package_path"]) != report["inventory"]:
        _fail("Prepared package inventory changed since preparation")
    return {"schema": SCHEMA, "scope": SCOPE, "status": "STABLE", "runtime": "NOT_RUN",
            "input_stable": True, "package_stable": True, "checked_at": _now()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest="action", required=True)
    inventory = actions.add_parser("inventory", help="Observe directory content identity; no acceptance")
    inventory.add_argument("--input", required=True, type=Path)
    prepare = actions.add_parser("prepare", help="Verify explicit input and create a new attempt")
    prepare.add_argument("--input", required=True, type=Path)
    prepare.add_argument("--attempt", required=True, type=Path)
    prepare.add_argument("--expected-sha256")
    prepare.add_argument("--expected-inventory-sha256")
    stable = actions.add_parser("verify-stable", help="Read a trusted receipt and recheck identities")
    stable.add_argument("--receipt", required=True, type=Path)
    args = parser.parse_args()
    try:
        if args.action == "inventory":
            result = inspect_inventory(args.input)
        elif args.action == "prepare":
            result = prepare_package(args.input, args.attempt, expected_sha256=args.expected_sha256,
                                     expected_inventory_sha256=args.expected_inventory_sha256)
        else:
            result = verify_stable(json.loads(args.receipt.read_text(encoding="utf-8")))
        print(json.dumps(result, ensure_ascii=False, sort_keys=True))
        return 0
    except (OSError, ValueError, RuntimeError) as error:
        print(json.dumps({"status": "FAIL", "scope": SCOPE, "runtime": "NOT_RUN", "error": str(error)},
                         ensure_ascii=False))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
