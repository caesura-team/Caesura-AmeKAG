#!/usr/bin/env python3
"""Execute a fixed validation profile and record raw results, without declaring a release."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import uuid

from validation_process import run_owned_command
from validation_sanitizer import capture_environment, create_capture, snapshot_capture

ROOT = Path(__file__).resolve().parents[1]
VARIABLE = re.compile(r"\{([a-z_]+)\}")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _fingerprint_entries(root: Path, entries: list[Path], *, required: bool,
                         follow_links: bool = False) -> str:
    digest = hashlib.sha256()
    for path in sorted(set(entries), key=lambda p: p.relative_to(root).as_posix()):
        name = path.relative_to(root).as_posix()
        if path.is_symlink():
            linked = os.readlink(path).encode("utf-8")
            if follow_links:
                target = path.resolve()
                if not target.is_relative_to(root):
                    raise ValueError(f"Fixture link escapes repository: {path}")
                linked += b"\0" + sha256_file(target).encode("ascii")
            content = hashlib.sha256(linked).hexdigest()
        elif path.is_file():
            content = sha256_file(path)
        elif required:
            raise FileNotFoundError(path)
        else:
            content = "MISSING"
        digest.update((name + "\0" + content + "\n").encode("utf-8"))
    return digest.hexdigest()


def fingerprint_paths(root: Path, paths: list[str]) -> str:
    # Compare resolved fixture targets against the same canonical root, even
    # when callers reached it through a directory alias (macOS /var, etc.).
    root = root.resolve()
    entries = []
    stack = [(root / name, frozenset()) for name in paths]
    while stack:
        path, ancestors = stack.pop()
        target = path.resolve()
        if not target.is_relative_to(root):
            raise ValueError(f"Fixture escapes repository: {path}")
        if path.name == "__pycache__" or path.suffix in {".pyc", ".pyo"}:
            continue
        if target.is_dir():
            if target in ancestors:
                raise ValueError(f"Cyclic fixture link: {path}")
            stack.extend((child, ancestors | {target}) for child in path.iterdir())
        elif target.is_file():
            entries.append(path)
        else:
            raise FileNotFoundError(path)
    if not entries:
        raise ValueError("A validation profile needs non-empty fixture inputs")
    return _fingerprint_entries(root, entries, required=True, follow_links=True)


def _git(root: Path, *arguments: str) -> bytes:
    if not (root / ".git").exists():
        raise ValueError(f"No Git checkout at {root}; source identity is unavailable")
    return subprocess.run(
        ["git", *arguments], cwd=root, check=True, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    ).stdout


def _source_identity(root: Path) -> dict:
    names = _git(root, "ls-files", "-z", "--cached", "--others", "--exclude-standard")
    paths = [root / name.decode("utf-8") for name in names.split(b"\0") if name]
    return {
        "source_sha": _git(root, "rev-parse", "HEAD").decode("ascii").strip(),
        "dirty": bool(_git(root, "status", "--porcelain", "-z")),
        "worktree_fingerprint": _fingerprint_entries(root, paths, required=False),
    }


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def _expand(value: str, variables: dict[str, str]) -> str:
    def replace(match: re.Match) -> str:
        key = match.group(1)
        if key not in variables:
            raise ValueError(f"Unknown profile variable: {key}")
        return variables[key]
    return VARIABLE.sub(replace, value)


def _read_cmake_cache(build_dir: Path) -> dict[str, str]:
    path = build_dir / "CMakeCache.txt"
    if not path.is_file():
        return {}
    return {
        match[1]: match[2]
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines()
        if (match := re.match(r"([^:#]+):[^=]+=(.*)", line))
    }


def _validate_environment(profile: dict, repo: Path, build_dir: Path, configuration: str) -> dict:
    system = platform.system()
    host = {"Windows": "windows", "Linux": "linux", "Darwin": "macos"}.get(system)
    if profile.get("platform") != host or profile.get("configuration") != configuration:
        raise ValueError("Profile platform/configuration does not match the execution host/configuration")
    cache = _read_cmake_cache(build_dir)
    if not cache:
        if profile.get("require_cmake_cache"):
            raise ValueError("A configured build directory is required")
        return cache
    source = cache.get("CMAKE_HOME_DIRECTORY")
    if not source or os.path.normcase(str(Path(source).resolve())) != os.path.normcase(str(repo)):
        raise ValueError("Build directory belongs to a different source checkout")
    if not cache.get("CMAKE_GENERATOR"):
        raise ValueError("Build directory has no generator identity")
    configurations = cache.get("CMAKE_CONFIGURATION_TYPES", "").split(";")
    if configurations != [""]:
        if configuration not in configurations:
            raise ValueError("Configuration is not available in this multi-config build")
    elif cache.get("CMAKE_BUILD_TYPE") != configuration:
        raise ValueError("Single-config build type does not match requested configuration")
    for key, expected in profile.get("expected_cache", {}).items():
        if cache.get(key) != expected:
            raise ValueError(f"Build cache does not satisfy profile: {key} must be {expected}")
    for path in (build_dir / "CMakeFiles").glob("*/CMakeSystem.cmake"):
        text = path.read_text(encoding="utf-8", errors="replace")
        found = re.search(r'set\(CMAKE_SYSTEM_NAME "?([^"\s)]+)', text)
        if found and found[1] != system:
            raise ValueError("Cross-compiled build cannot be labeled as a native validation run")
    return cache


def _variables(repo: Path, build_dir: Path, configuration: str, run_dir: Path,
               cache: dict | None = None) -> dict[str, str]:
    windows = sys.platform == "win32"
    suffix = ".exe" if windows else ""
    multi_config = bool(cache.get("CMAKE_CONFIGURATION_TYPES")) if cache else windows
    config_dir = configuration if multi_config else ""
    return {
        "repo": str(repo), "build_dir": str(build_dir), "config": configuration,
        "run_dir": str(run_dir), "python": sys.executable,
        "cpp_binary": str(build_dir / "tests" / config_dir / ("CaesuraTests" + suffix)),
        "engine_binary": str(build_dir / config_dir / ("CaesuraAmeKAG" + suffix)),
        "lua_binary": str(build_dir / "lua" / config_dir / ("lua" + suffix)),
        "test_dir": str(build_dir / "tests" / config_dir),
    }


def _resolve_checks(profile: dict, variables: dict[str, str]) -> list[dict]:
    checks = profile.get("checks")
    if not isinstance(checks, list) or not checks:
        raise ValueError("Profile requires a non-empty check list")
    resolved, seen = [], set()
    for check in checks:
        name = check.get("id", "")
        if not re.fullmatch(r"[a-zA-Z][a-zA-Z0-9_-]*", name) or name in seen:
            raise ValueError(f"Invalid or duplicate check id: {name}")
        seen.add(name)
        command = check.get("command")
        if not isinstance(command, list) or not command or not all(isinstance(a, str) for a in command):
            raise ValueError(f"{name}: command must be a non-empty argv array")
        row = dict(check)
        row["command"] = [_expand(a, variables) for a in command]
        row["cwd"] = _expand(check.get("cwd", "{repo}"), variables)
        for field in ("binary", "report"):
            if field in check:
                row[field] = _expand(check[field], variables)
        timeout = row.get("timeout_seconds", 1200)
        if not isinstance(timeout, (int, float)) or isinstance(timeout, bool) or timeout <= 0:
            raise ValueError(f"{name}: timeout_seconds must be positive")
        resolved.append(row)
    return resolved


def _reference(path: Path, base: Path) -> dict[str, str]:
    # Retain the declared pathname so a symlink switch is detected by the
    # after-execution digest check; the executable itself is resolved below.
    absolute = path.absolute()
    name = absolute.relative_to(base).as_posix() if absolute.is_relative_to(base) else str(absolute)
    return {"path": name, "sha256": sha256_file(absolute)}


def _verify_executed_bytes(row: dict, run_dir: Path) -> None:
    for field in ("binary", "executed_program"):
        if field not in row:
            continue
        reference = row[field]
        path = run_dir / reference["path"]
        if not path.is_file() or sha256_file(path) != reference["sha256"]:
            row["process_exit_code"] = row["exit_code"]
            row.update(exit_code=125, error="binary_changed")


def _execute_check(check: dict, run_dir: Path, *, run_id: str, purpose: str) -> dict:
    name = check["id"]
    scope = create_capture(run_dir, run_id=run_id, check_id=name, purpose=purpose)
    environment = capture_environment(dict(os.environ), scope=scope)
    cleanup_complete = False
    stdout = run_dir / f"{name}.stdout.log"
    stderr = run_dir / f"{name}.stderr.log"
    row = {
        "id": name, "command": check["command"], "cwd": check["cwd"],
        "started_at": _utc_now(), "exit_code": 127,
    }
    binary = Path(check["binary"]) if check.get("binary") else None
    with stdout.open("wb") as out, stderr.open("wb") as err:
        if binary is not None and not binary.is_file():
            row["error"] = "missing_binary"
            err.write(f"Required binary is missing: {binary}\n".encode("utf-8"))
        else:
            try:
                executable = shutil.which(check["command"][0])
                if not executable:
                    raise FileNotFoundError(check["command"][0])
                executable = str(Path(executable).resolve())
                if os.name == "nt" and Path(executable).suffix.lower() in {".bat", ".cmd"}:
                    raise ValueError("Use the underlying executable instead of a shell wrapper")
                command = [executable, *check["command"][1:]]
                row["executed_program"] = _reference(Path(executable), run_dir)
                if binary is not None:
                    row["binary"] = _reference(binary, run_dir)
                try:
                    row["exit_code"] = run_owned_command(
                        command, check["cwd"], out, err, check.get("timeout_seconds", 1200),
                        env=environment,
                    )
                    cleanup_complete = True
                except subprocess.TimeoutExpired:
                    # This exception is propagated only after the owned runner's
                    # finally has finished reclaiming the entire process tree.
                    cleanup_complete = True
                    row.update(exit_code=124, error="timeout")
            except (OSError, ValueError) as exc:
                row["error"] = "launch_failed"
                err.write((str(exc) + "\n").encode("utf-8"))
    # The wrappers may consume a native child's stderr or accept its nonzero
    # exit. Bind the files produced in this check's scope after writers stop.
    row["sanitizer_capture"] = snapshot_capture(run_dir, name, complete=cleanup_complete)
    row["finished_at"] = _utc_now()
    row["stdout"] = _reference(stdout, run_dir)
    row["stderr"] = _reference(stderr, run_dir)
    _verify_executed_bytes(row, run_dir)
    report = Path(check["report"]) if check.get("report") else None
    if report is not None and report.is_file():
        row["report"] = _reference(report, run_dir)
    return row


def _toolchain(build_dir: Path) -> dict:
    result = {
        "python": platform.python_version(), "system": platform.system(),
        "system_release": platform.release(), "architecture": platform.machine(),
    }
    cache = build_dir / "CMakeCache.txt"
    keys = {
        "CMAKE_GENERATOR", "CMAKE_CXX_COMPILER", "CMAKE_BUILD_TYPE",
        "CAESURA_ENABLE_FFMPEG", "CAESURA_LIVE2D", "CAESURA_HAS_STEAM",
        "CAESURA_SANITIZERS",
    }
    if cache.is_file():
        result["cmake_cache"] = {
            match[1]: match[2]
            for line in cache.read_text(encoding="utf-8", errors="replace").splitlines()
            if (match := re.match(r"([^:#]+):[^=]+=(.*)", line)) and match[1] in keys
        }
        values = _read_cmake_cache(build_dir)
        version = ".".join(values.get(f"CMAKE_CACHE_{part}_VERSION", "")
                           for part in ("MAJOR", "MINOR", "PATCH"))
        compiler = build_dir / "CMakeFiles" / version / "CMakeCXXCompiler.cmake"
        if compiler.is_file():
            source = compiler.read_text(encoding="utf-8", errors="replace")
            result["cxx_compiler"] = dict(re.findall(
                r'set\((CMAKE_CXX_COMPILER(?:_ID|_VERSION)?) "([^"]*)"\)', source,
            ))
    return result


def run_profile(*, repo: Path, profile_file: Path, profile_name: str,
                build_dir: Path, configuration: str, run_dir: Path,
                purpose: str = "validation") -> dict:
    repo, build_dir, run_dir = repo.resolve(), build_dir.resolve(), run_dir.resolve()
    if purpose not in {"validation", "test-fixture"}:
        raise ValueError("Unknown evidence purpose")
    profile_bytes = profile_file.read_bytes()
    data = json.loads(profile_bytes.decode("utf-8-sig"))
    if data.get("schema_version") != 1 or profile_name not in data.get("profiles", {}):
        raise ValueError(f"Unknown profile or schema: {profile_name}")
    profile = data["profiles"][profile_name]
    cache = _validate_environment(profile, repo, build_dir, configuration)
    variables = _variables(repo, build_dir, configuration, run_dir, cache)
    checks = _resolve_checks(profile, variables)
    fixtures = profile.get("fixture_paths", data.get("fixture_paths", []))
    fixture_hash = fingerprint_paths(repo, fixtures)
    before = _source_identity(repo)
    run_dir.mkdir(parents=True, exist_ok=False)
    run_id = str(uuid.uuid4())
    result = {
        **before, "schema_version": 1, "purpose": purpose, "profile_name": profile_name,
        "run_id": run_id, "run_attempt": int(os.environ.get("GITHUB_RUN_ATTEMPT", "1")),
        "repository": os.environ.get("GITHUB_REPOSITORY", profile.get("repository", repo.name)),
        "workflow": os.environ.get("GITHUB_WORKFLOW_REF", "local/run_validation.py"),
        "platform": profile["platform"], "configuration": configuration,
        "started_at": _utc_now(), "toolchain": _toolchain(build_dir),
        "profile_sha256": hashlib.sha256(profile_bytes).hexdigest(),
        "fixture_sha256": fixture_hash, "profile_variables": variables,
        "checks": [_execute_check(check, run_dir, run_id=run_id, purpose=purpose) for check in checks],
    }
    result["finished_at"] = _utc_now()
    after = _source_identity(repo)
    result["finished_worktree_fingerprint"] = after["worktree_fingerprint"]
    result["source_changed_during_run"] = before != after
    try:
        result["finished_fixture_sha256"] = fingerprint_paths(repo, fixtures)
    except (OSError, ValueError) as exc:
        result["finished_fixture_sha256"] = None
        result["fixture_error"] = str(exc)
    result["fixtures_changed_during_run"] = result["finished_fixture_sha256"] != fixture_hash
    (run_dir / "run.json").write_text(
        json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8",
    )
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=ROOT)
    parser.add_argument("--profile", type=Path, default=ROOT / "scripts/validation_profiles.json")
    parser.add_argument("--profile-name", required=True)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--configuration", default="Debug")
    parser.add_argument("--run-dir", type=Path, required=True)
    parser.add_argument("--purpose", choices=["validation", "test-fixture"], default="validation")
    args = parser.parse_args()
    try:
        result = run_profile(
            repo=args.repo, profile_file=args.profile, profile_name=args.profile_name,
            build_dir=args.build_dir, configuration=args.configuration,
            run_dir=args.run_dir, purpose=args.purpose,
        )
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        print(f"Validation execution could not start: {exc}", file=sys.stderr)
        return 2
    print(f"Raw execution receipt: {args.run_dir.resolve() / 'run.json'}")
    print("Collect and verify this receipt before interpreting test or release status.")
    return 1 if any(check["exit_code"] != 0 for check in result["checks"]) else 0


if __name__ == "__main__":
    raise SystemExit(main())
