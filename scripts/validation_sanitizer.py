"""Owned validation diagnostic transport, not an adversarial-process sandbox.

The runner replaces inherited sanitizer options with a fixed log destination.
Native launchers may restore this narrow contract across their clean environment
boundary. A PID suffix identifies a retained file, not a unique process lifetime.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import stat
from typing import Mapping

MARKER = "CAESURA_VALIDATION_SANITIZER_CAPTURE"
OPTION_NAMES = ("ASAN_OPTIONS", "UBSAN_OPTIONS", "LSAN_OPTIONS", "TSAN_OPTIONS")
OPTIONS_CONTRACT = "llvm-common-log-path-v1"
ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,127}\Z")
LOG_NAME = re.compile(r"sanitizer\.[1-9][0-9]*\Z")
MAX_FILES = 4096
MAX_BYTES = 64 * 1024 * 1024


class CaptureError(ValueError):
    """Missing, escaping, incomplete or unsupported capture transport."""


def ensure(condition: bool, message: str) -> None:
    if not condition:
        raise CaptureError(message)


def plain_path(path: Path, *, directory: bool) -> Path:
    """Require an existing path without any symbolic/reparse components."""
    path = path.absolute()
    for part in (*reversed(path.parents), path):
        info = part.lstat()
        ensure(not stat.S_ISLNK(info.st_mode) and not (
            getattr(info, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT
        ), f"Sanitizer capture cannot use links/reparse points: {part}")
    ensure(path.is_dir() if directory else path.is_file(),
           f"Invalid sanitizer capture {'directory' if directory else 'file'}: {path}")
    return path


def _scope(scope: dict) -> dict:
    ensure(isinstance(scope, dict) and set(scope) == {
        "version", "run_id", "check_id", "purpose", "run_dir", "directory", "prefix",
    }, "Invalid sanitizer capture scope fields")
    ensure(type(scope["version"]) is int and scope["version"] == 1,
           "Unsupported sanitizer capture scope version")
    ensure(scope["purpose"] in ("validation", "test-fixture") and scope["prefix"] == "sanitizer",
           "Invalid sanitizer capture purpose/prefix")
    for key in ("run_id", "check_id"):
        ensure(isinstance(scope[key], str) and ID.fullmatch(scope[key]) is not None,
               f"Invalid sanitizer capture {key}")
    for key in ("run_dir", "directory"):
        value = scope[key]
        ensure(isinstance(value, str) and value and not any(c in value for c in '\x00\r\n\"\''),
               f"Unrepresentable sanitizer capture {key}")
        path = Path(value)
        ensure(path.is_absolute() and str(path.resolve()) == value,
               f"Sanitizer capture {key} must be canonical and absolute")
        plain_path(path, directory=True)
    expected = Path(scope["run_dir"]) / "sanitizer" / scope["check_id"]
    ensure(Path(scope["directory"]) == expected, "Sanitizer capture directory escapes check scope")
    return dict(scope)


def create_capture(run_dir: Path, *, run_id: str, check_id: str, purpose: str) -> dict:
    ensure(isinstance(check_id, str) and ID.fullmatch(check_id) is not None, "Invalid capture check id")
    root = plain_path(run_dir, directory=True)
    namespace = root / "sanitizer"
    namespace.mkdir(exist_ok=True)
    plain_path(namespace, directory=True)
    directory = namespace / check_id
    directory.mkdir(exist_ok=False)
    return _scope(dict(version=1, run_id=run_id, check_id=check_id, purpose=purpose,
                       run_dir=str(root), directory=str(directory), prefix="sanitizer"))


def capture_environment(env: Mapping[str, str], *, scope: dict | None = None,
                        inherited: Mapping[str, str] | None = None) -> dict[str, str]:
    """Copy env and reconstruct only the fixed transport; never merge options."""
    result = dict(env)
    if scope is None:
        parent = os.environ if inherited is None else inherited
        if MARKER not in parent:
            return result
        try:
            scope = json.loads(parent[MARKER])
        except (TypeError, ValueError) as error:
            raise CaptureError("Invalid sanitizer capture scope JSON") from error
    validated = _scope(scope)
    options = (f'log_path="{Path(validated["directory"]) / "sanitizer"}":'
               'log_exe_name=0:print_summary=1:color=never')
    result.update({name: options for name in OPTION_NAMES})
    result[MARKER] = json.dumps(validated, ensure_ascii=True, separators=(",", ":"))
    return result


def read_capture_files(directory: Path) -> dict[str, bytes]:
    """Inventory all actual leaves with bounded reads, without inventing logs."""
    plain_path(directory, directory=True)
    content: dict[str, bytes] = {}
    total = 0
    for path in directory.iterdir():
        ensure(LOG_NAME.fullmatch(path.name) is not None, f"Unexpected sanitizer capture entry: {path}")
        ensure(len(content) < MAX_FILES, "Too many sanitizer capture files")
        plain_path(path, directory=False)
        with path.open("rb") as stream:
            data = stream.read(MAX_BYTES - total + 1)
        total += len(data)
        ensure(total <= MAX_BYTES, "Sanitizer capture exceeds byte limit")
        content[path.name] = data
    return dict(sorted(content.items()))


def snapshot_capture(run_dir: Path, check_id: str, *, complete: bool) -> dict:
    ensure(isinstance(check_id, str) and ID.fullmatch(check_id) is not None, "Invalid capture check id")
    ensure(type(complete) is bool, "Invalid sanitizer capture completion state")
    relative = "sanitizer/" + check_id
    files = read_capture_files(run_dir / relative)
    return dict(version=1, complete=complete, directory=relative, prefix="sanitizer",
                options_contract=OPTIONS_CONTRACT,
                files=[dict(path=relative + "/" + name, sha256=hashlib.sha256(data).hexdigest(),
                            size_bytes=len(data)) for name, data in files.items()])
