#!/usr/bin/env python3
"""Run the complete Web runtime unit suite on a real second Linux CI filesystem."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import sys
import tempfile

from package_runtime import run_runtime_command
from run_validation import _source_identity

ROOT = Path(__file__).resolve().parents[1]
SUITE = ROOT / "tests/scripts/test_web_package_runtime.py"
MINIMUM_LINUX_TESTS = 30

# This preflight and the original unittest entry execute in the same child.
# No test filtering, replacement lease or alternate test loader is introduced.
CHILD = r"""
import json, os, runpy, sys, tempfile
from pathlib import Path
work, suite = map(Path, sys.argv[1:])
actual = Path(tempfile.gettempdir()).resolve(strict=True)
requested = Path(os.environ['TMPDIR']).resolve(strict=True)
chrome_base = Path('/tmp').resolve(strict=True)
proof = dict(pid=os.getpid(),requested_tmpdir=str(requested),actual_tempfile_root=str(actual),
             actual_st_dev=actual.stat().st_dev,chrome_base=str(chrome_base),
             chrome_st_dev=chrome_base.stat().st_dev,suite=str(suite),unfiltered=True)
with (work/'child-preflight.json').open('x',encoding='utf-8') as out:
    json.dump(proof,out,indent=2);out.write('\n')
assert actual == requested, 'Python selected a different temporary root'
assert actual.stat().st_dev != chrome_base.stat().st_dev, 'Cross-filesystem prerequisite absent'
sys.argv = [str(suite), '-v']
runpy.run_path(str(suite),run_name='__main__')
"""


def file_ref(path: Path) -> dict:
    return {"path": str(path), "bytes": path.stat().st_size,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def write(path: Path, value: dict) -> None:
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2)
        stream.write("\n")


def plain_directory(path: Path) -> Path:
    for item in (path, *path.parents):
        if not stat.S_ISDIR(item.lstat().st_mode) or item.is_symlink():
            raise ValueError(f"Expected plain directory: {item}")
    if path.resolve(strict=True) != path:
        raise ValueError(f"Expected canonical directory: {path}")
    return path


def identity(path: Path) -> tuple[int, int, int]:
    value = path.lstat()
    return value.st_dev, value.st_ino, value.st_uid


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--source-sha", required=True)
    args = parser.parse_args()
    if sys.platform != "linux" or os.environ.get("GITHUB_ACTIONS") != "true" or os.environ.get("RUNNER_OS") != "Linux":
        raise ValueError("This entry requires the ephemeral GitHub Linux runner")
    if not re.fullmatch(r"[0-9a-f]{40}", args.source_sha):
        raise ValueError("An exact source SHA is required")
    base = plain_directory(Path(os.environ["RUNNER_TEMP"]))
    work = args.work.absolute()
    if work.parent != base or not work.name.startswith("u29-web-runtime-crossfs-") or work.exists() or work.is_symlink():
        raise ValueError("Evidence must use a new direct RUNNER_TEMP child")
    work.mkdir(mode=0o700)
    report = {"status": "FAIL", "source_sha": args.source_sha,
              "scope": "Complete Linux Web runtime unit suite with real cross-filesystem TMPDIR",
              "minimum_linux_tests": MINIMUM_LINUX_TESTS, "errors": []}
    private = None
    private_identity = None
    launched = False
    source = None
    inputs = None
    try:
        if not (ROOT / ".git").exists():
            raise ValueError("Repository identity requires .git")
        source = _source_identity(ROOT)
        report["source_before"] = source
        if source["source_sha"] != args.source_sha or source["dirty"]:
            raise ValueError("Source is not the requested clean commit")
        inputs = [file_ref(path) for path in (Path(__file__), SUITE,
                  ROOT / "scripts/web_package_runtime.py", ROOT / "scripts/package_runtime.py",
                  ROOT / "scripts/validation_process.py")]
        report["inputs_before"] = inputs
        shared = plain_directory(Path("/dev/shm"))
        chrome_base = Path("/tmp").resolve(strict=True)
        report["mounts"] = {"fixture_base": str(shared), "fixture_st_dev": shared.stat().st_dev,
                            "chrome_base": str(chrome_base), "chrome_st_dev": chrome_base.stat().st_dev}
        if shared.stat().st_dev == chrome_base.stat().st_dev:
            raise ValueError("/dev/shm and the Chrome /tmp base must be different filesystems")
        private = Path(tempfile.mkdtemp(prefix="caesura-web-crossfs-", dir=shared))
        plain_directory(private)
        private_identity = identity(private)
        if private_identity[2] != os.getuid() or private.parent != shared:
            raise ValueError("Temporary root ownership differs")
        report["private_root"] = {"path": str(private), "identity": list(private_identity)}
        for name in ("home", "local", "roaming"):
            (private / name).mkdir(mode=0o700)
        env = {key: os.environ[key] for key in ("PATH", "LANG", "LC_ALL") if key in os.environ}
        env.update(TMPDIR=str(private), TEMP=str(private), TMP=str(private), HOME=str(private / "home"),
                   USERPROFILE=str(private / "home"), APPDATA=str(private / "roaming"),
                   LOCALAPPDATA=str(private / "local"), PYTHONDONTWRITEBYTECODE="1", PYTHONUTF8="1")
        argv = [sys.executable, "-B", "-c", CHILD, str(work), str(SUITE)]
        with (work / "stdout.log").open("xb") as stdout, (work / "stderr.log").open("xb") as stderr:
            launched = True
            owned = run_runtime_command(argv, ROOT, env, work / "owned", stdout, stderr, 180)
        report["owned"] = owned
        if (owned["status"] != "EXITED" or type(owned["actual_exit_code"]) is not int or owned["actual_exit_code"] != 0
                or type(owned["launcher_exit_code"]) is not int or owned["launcher_exit_code"] != 0
                or owned["owned_tree_cleanup"] != "COMPLETE"
                or any(owned[key] for key in ("timed_out", "forced_kill", "stop_requested"))):
            raise ValueError("Complete owned suite did not exit successfully")
        child = json.loads((work / "child-preflight.json").read_bytes())
        if (child["pid"] != owned["process"]["pid"] or child["actual_tempfile_root"] != str(private)
                or child["actual_st_dev"] == child["chrome_st_dev"]):
            raise ValueError("Child mount proof is not bound to this owned process")
        text = (work / "stderr.log").read_text(encoding="utf-8", errors="replace")
        counts = re.findall(r"^Ran (\d+) tests? in ", text, re.MULTILINE)
        if not counts or int(counts[-1]) < MINIMUM_LINUX_TESTS or not re.search(r"^OK$", text, re.MULTILINE):
            raise ValueError("Full unittest discovery/pass summary missing or reduced")
        if re.search(r"skipped=|unexpected successes=", text):
            raise ValueError("Complete cross-filesystem suite must not skip tests")
        if not re.search(r"^test_chrome_temp_replacement_is_retained_without_deleting_either_tree .* \.\.\. ok$", text, re.MULTILINE):
            raise ValueError("Real directory-replacement regression did not pass")
        report.update(status="PASS", discovered=int(counts[-1]), failed=0, skipped=0, child_preflight=child)
    except Exception as error:
        report["errors"].append({"type": type(error).__name__, "message": str(error)})
    finally:
        if (work / "owned/run.json").is_file():
            report["owned"] = json.loads((work / "owned/run.json").read_bytes())
        if private is not None:
            try:
                if launched and report.get("owned", {}).get("owned_tree_cleanup") != "COMPLETE":
                    raise ValueError("Retaining private root without complete owned cleanup")
                plain_directory(private)
                if private.parent != Path("/dev/shm") or not private.name.startswith("caesura-web-crossfs-") or identity(private) != private_identity:
                    raise ValueError("Refusing cleanup of changed private root")
                shutil.rmtree(private)
                report["private_cleanup"] = "COMPLETE"
            except Exception as error:
                report["status"] = "FAIL"
                report["errors"].append({"cleanup": str(error)})
        if source is not None:
            try:
                if not (ROOT / ".git").exists():
                    raise ValueError("Repository .git disappeared")
                report["source_after"] = _source_identity(ROOT)
                report["inputs_after"] = [file_ref(Path(row["path"])) for row in inputs or []]
                if report["source_after"] != source or report["inputs_after"] != inputs:
                    raise ValueError("Source or input bytes changed")
            except Exception as error:
                report["status"] = "FAIL"
                report["errors"].append({"stability": str(error)})
        report["evidence"] = [file_ref(path) for path in sorted(work.rglob("*")) if path.is_file()]
        write(work / "report.json", report)
    print(json.dumps({"status": report["status"], "errors": report["errors"], "work": str(work)}))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
