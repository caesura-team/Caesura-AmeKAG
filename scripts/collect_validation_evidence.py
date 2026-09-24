#!/usr/bin/env python3
"""Collect execution receipts and original reports without running or inventing tests.

The receipt is supplied by a controlled executor (run_validation.py), not by the
artifact under review. Checksums prove identity, not that an untrusted executor
ran a command. Release verification requires the separately trusted receipt and
profile; Actions workflow/run/artifact authentication belongs to the CI caller.
"""
from __future__ import annotations

import argparse
from datetime import datetime
import hashlib
import json
import ntpath
from pathlib import Path
import posixpath
import re
import shutil
import sys
from typing import Any
import xml.etree.ElementTree as ET

from validation_sanitizer import CaptureError, LOG_NAME, OPTIONS_CONTRACT, plain_path, read_capture_files

VERSION = 1
PARSERS = {"doctest", "lua", "ctest-junit", "vitest-json", "unittest", "exit-code"}
ID_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,127}\Z")
SHA_RE = re.compile(r"[0-9a-f]{64}\Z")
SOURCE_RE = re.compile(r"[0-9a-f]{40}\Z")
MAX_REPORT_BYTES = 64 * 1024 * 1024


class EvidenceError(ValueError):
    """Malformed, incomplete, mismatched or non-reproducible evidence input."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise EvidenceError(message)


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def read_bounded(path: Path) -> bytes:
    """Read one bounded snapshot; a stat followed by an unbounded read can race."""
    require(path.is_file(), f"Missing file: {path}")
    with path.open("rb") as stream:
        content = stream.read(MAX_REPORT_BYTES + 1)
    require(len(content) <= MAX_REPORT_BYTES, f"File too large: {path}")
    return content


def read_json(path: Path, *, content: bytes | None = None) -> dict[str, Any]:
    content = read_bounded(path) if content is None else content
    try:
        value = json.loads(content.decode("utf-8-sig"))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise EvidenceError(f"Invalid JSON: {path}: {error}") from error
    require(isinstance(value, dict), f"JSON root must be an object: {path}")
    return value


def integer(value: Any, name: str, minimum: int = 0) -> int:
    require(type(value) is int and value >= minimum, f"Invalid {name}: {value!r}")
    return value


def timestamp(value: Any) -> datetime:
    require(isinstance(value, str), "Timestamp must be an ISO 8601 string")
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as error:
        raise EvidenceError(f"Invalid timestamp: {value}") from error
    require(parsed.tzinfo is not None, f"Timestamp needs timezone: {value}")
    return parsed


def counts(passed: int, failed: int, skipped: int = 0) -> dict[str, int]:
    return {"discovered": passed + failed + skipped, "executed": passed + failed,
            "passed": passed, "failed": failed, "skipped": skipped}


def parse_doctest(text: str) -> tuple[dict, list[str]]:
    matches = re.findall(
        r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed\s*\|\s*(\d+) skipped", text)
    require(bool(matches), "Missing complete doctest summary")
    total, passed, failed, skipped = map(int, matches[-1])
    require(total == passed + failed, "Inconsistent doctest summary")
    return counts(passed, failed, skipped), ["<doctest-filtered>"] * skipped


def parse_lua(text: str) -> tuple[dict, list[str]]:
    matches = re.findall(r"^Results:\s*(\d+) passed,\s*(\d+) failed,\s*(\d+) total\s*$", text, re.M)
    require(bool(matches), "Missing complete Lua runner summary")
    passed, failed, total = map(int, matches[-1])
    require(total == passed + failed, "Inconsistent Lua summary")
    return counts(passed, failed), []


def parse_unittest(text: str) -> tuple[dict, list[str]]:
    matches = list(re.finditer(r"^Ran (\d+) tests? in [^\r\n]+\r?\n", text, re.M))
    require(bool(matches), "Missing unittest run count")
    match = matches[-1]
    tail = text[match.end():].strip()
    conclusion = re.match(r"(OK|FAILED)(?: \(([^\r\n]+)\))?(?:\r?\n|$)", tail)
    require(conclusion is not None, "Missing unittest completion")
    detail = dict(re.findall(r"(failures|errors|skipped|expected failures|unexpected successes)=(\d+)", conclusion[2] or ""))
    unexpected = sum(int(detail.get(key, 0)) for key in ("failures", "errors", "unexpected successes"))
    failed = unexpected + int(detail.get("expected failures", 0))
    skipped = int(detail.get("skipped", 0))
    total = int(match[1])
    require(total >= failed + skipped, "Inconsistent unittest summary")
    require((conclusion[1] == "FAILED") == (unexpected > 0), "Inconsistent unittest conclusion")
    names = re.findall(r"^(.+?) \.\.\. skipped [^\r\n]+", text, re.M)
    if len(names) != skipped:
        names = ["<unnamed-unittest-skip>"] * skipped
    return counts(total - failed - skipped, failed, skipped), names


def ctest_counts(cases: list[ET.Element]) -> tuple[dict, list[str], int]:
    passed = failed = disabled = 0
    skipped = []
    for case in cases:
        name = case.get("name")
        require(bool(name), "Unnamed CTest case")
        if case.find("failure") is not None or case.find("error") is not None:
            failed += 1
        elif case.get("status") == "disabled":
            disabled += 1
            skipped.append(name)
        elif case.find("skipped") is not None or case.get("status") in {"notrun", "disabled"}:
            skipped.append(name)
        else:
            passed += 1
    return counts(passed, failed, len(skipped)), skipped, disabled


def parse_ctest(report: str) -> tuple[dict, list[str]]:
    try:
        root = ET.fromstring(report)
    except ET.ParseError as error:
        raise EvidenceError(f"Invalid CTest JUnit XML: {error}") from error
    require(root.tag in {"testsuite", "testsuites"}, "Not a JUnit test suite")
    for suite in root.iter():
        if suite.tag not in {"testsuite", "testsuites"}:
            continue
        summary, _, disabled = ctest_counts(list(suite.iter("testcase")))
        for attribute, count in (("tests", summary["discovered"]), ("skipped", summary["skipped"] - disabled), ("disabled", disabled)):
            if attribute in suite.attrib:
                value = suite.attrib[attribute]
                require(value.isdigit() and int(value) == count, f"Inconsistent CTest {attribute} count")
        if "failures" in suite.attrib or "errors" in suite.attrib:
            totals = [suite.attrib.get(key, "0") for key in ("failures", "errors")]
            require(all(value.isdigit() for value in totals) and sum(map(int, totals)) == summary["failed"],
                    "Inconsistent CTest failure count")
    summary, skipped, _ = ctest_counts(list(root.iter("testcase")))
    return summary, skipped


def parse_vitest(report: str) -> tuple[dict, list[str]]:
    try:
        data = json.loads(report)
    except json.JSONDecodeError as error:
        raise EvidenceError(f"Invalid Vitest JSON: {error}") from error
    require(isinstance(data, dict) and isinstance(data.get("testResults"), list), "Missing Vitest testResults")
    passed = failed = 0
    skipped = []
    for suite in data["testResults"]:
        require(isinstance(suite, dict) and isinstance(suite.get("assertionResults"), list), "Missing Vitest assertionResults")
        for assertion in suite["assertionResults"]:
            require(isinstance(assertion, dict), "Invalid Vitest assertion")
            status = assertion.get("status")
            if status == "passed":
                passed += 1
            elif status == "failed":
                failed += 1
            elif status in {"pending", "todo", "skipped", "disabled"}:
                require(bool(assertion.get("fullName")), "Unnamed Vitest skip")
                skipped.append(assertion["fullName"])
            else:
                raise EvidenceError(f"Unknown Vitest assertion status: {status}")
    result = counts(passed, failed, len(skipped))
    todo = integer(data.get("numTodoTests", 0), "numTodoTests")
    for field, expected in (("numTotalTests", result["discovered"]), ("numPassedTests", passed),
                            ("numFailedTests", failed), ("numPendingTests", len(skipped) - todo)):
        require(field in data and type(data[field]) is int and data[field] == expected, f"Inconsistent Vitest {field}")
    require(data.get("numRuntimeErrorTestSuites", 0) == 0, "Vitest runtime errors")
    require(data.get("success", True) is True, "Vitest run reported failure")
    require(data.get("numFailedTestSuites", 0) == 0 or failed > 0, "Vitest suite failed without assertion results")
    return result, skipped


def parse_report(parser: str, stdout: str, stderr: str, report: str | None) -> tuple[dict | None, list[str]]:
    """Parse actual completion records. Never infer test success from registration."""
    require(parser in PARSERS, f"Unknown parser: {parser}")
    text = stdout + "\n" + stderr
    if parser == "exit-code":
        return None, []
    if parser in {"ctest-junit", "vitest-json"}:
        require(isinstance(report, str) and bool(report.strip()), f"Missing {parser} report")
        return parse_ctest(report) if parser == "ctest-junit" else parse_vitest(report)
    return {"doctest": parse_doctest, "lua": parse_lua, "unittest": parse_unittest}[parser](text)


def load_profile(path: Path, name: str, *, content: bytes | None = None) -> dict:
    data = read_json(path, content=content)
    require(data.get("schema_version") == VERSION, "Unsupported profile schema_version")
    require(isinstance(data.get("profiles"), dict) and name in data["profiles"], f"Unknown profile: {name}")
    profile = data["profiles"][name]
    require(isinstance(profile, dict), "Invalid profile object")
    if "sanitizer_capture" in profile:
        policy = profile["sanitizer_capture"]
        require(isinstance(policy, dict) and set(policy) == {"version", "required"}
                and type(policy["version"]) is int and policy["version"] == 1
                and policy["required"] is True, "Unsupported sanitizer capture profile policy")
    require(isinstance(profile.get("checks"), list) and bool(profile["checks"]), "Profile has no checks")
    seen = set()
    for check in profile["checks"]:
        require(isinstance(check, dict), "Invalid profile check")
        ident = check.get("id", "")
        require(isinstance(ident, str) and ID_RE.fullmatch(ident) is not None, "Invalid check id")
        require(ident not in seen, f"Duplicate profile check: {ident}")
        seen.add(ident)
        require(check.get("parser") in PARSERS, f"Invalid parser for {ident}")
        require(type(check.get("required")) is bool, f"Missing required flag for {ident}")
        integer(check.get("min_discovered", 1 if check["parser"] != "exit-code" else 0), "min_discovered")
        allowed = check.get("allowed_skips", [])
        require(isinstance(allowed, list) and all(isinstance(item, str) and item for item in allowed), "Invalid allowed_skips")
    require(any(check["required"] for check in profile["checks"]), "Profile has no required checks")
    return profile


def validate_fixture_identity(run: dict) -> None:
    require("finished_fixture_sha256" in run, "Missing finished_fixture_sha256")
    fixture_finished = run["finished_fixture_sha256"]
    fixture_error = run.get("fixture_error")
    require("fixture_error" not in run or isinstance(fixture_error, str) and bool(fixture_error), "Invalid fixture_error")
    if fixture_finished is None:
        require(bool(fixture_error), "Null finished_fixture_sha256 requires a fixture computation error")
    else:
        require(isinstance(fixture_finished, str) and SHA_RE.fullmatch(fixture_finished) is not None,
                "Invalid finished_fixture_sha256")
        require(fixture_error is None, "Fixture computation error contradicts finished_fixture_sha256")
    require(type(run.get("fixtures_changed_during_run")) is bool, "Missing fixtures_changed_during_run")
    require(run["fixtures_changed_during_run"] == (fixture_finished != run["fixture_sha256"]),
            "Inconsistent fixtures_changed_during_run flag")


def validate_context(run: dict, profile: dict, profile_sha256: str) -> None:
    require(run.get("schema_version") == VERSION, "Unsupported execution receipt schema_version")
    require(isinstance(run.get("source_sha"), str) and SOURCE_RE.fullmatch(run["source_sha"]) is not None, "Invalid source_sha")
    for key in ("worktree_fingerprint", "fixture_sha256", "profile_sha256"):
        require(isinstance(run.get(key), str) and SHA_RE.fullmatch(run[key]) is not None, f"Invalid {key}")
    require(run["profile_sha256"] == profile_sha256, "Wrong profile digest")
    require(type(run.get("dirty")) is bool, "Missing dirty state")
    finished = run.get("finished_worktree_fingerprint")
    require(isinstance(finished, str) and SHA_RE.fullmatch(finished) is not None, "Missing finished_worktree_fingerprint")
    require(type(run.get("source_changed_during_run")) is bool, "Missing source_changed_during_run")
    require(run["source_changed_during_run"] or finished == run["worktree_fingerprint"], "Changed source fingerprint without changed flag")
    validate_fixture_identity(run)
    require(run.get("purpose") in {"validation", "test-fixture"}, "Invalid purpose")
    require(isinstance(run.get("run_id"), str) and ID_RE.fullmatch(run["run_id"]) is not None, "Invalid run_id")
    integer(run.get("run_attempt"), "run_attempt", 1)
    for key in ("repository", "workflow", "platform", "configuration"):
        require(isinstance(run.get(key), str) and bool(run[key]), f"Missing {key}")
    for key in ("platform", "configuration"):
        require(run[key] == profile.get(key), f"Wrong profile {key}")
    require(isinstance(run.get("toolchain"), dict) and bool(run["toolchain"]), "Missing toolchain")
    variables = run.get("profile_variables", {})
    require(isinstance(variables, dict) and all(isinstance(k, str) and isinstance(v, str) for k, v in variables.items()), "Invalid profile_variables")
    require(timestamp(run.get("started_at")) <= timestamp(run.get("finished_at")), "Run finishes before it starts")


def execution_path(value: str) -> str:
    """Compare paths in the executor's OS, independent of the verifier host."""
    if re.match(r"^[A-Za-z]:[\\/]|^\\\\", value):
        return ntpath.normcase(ntpath.normpath(value))
    require(value.startswith("/"), f"Execution path must be absolute: {value}")
    return posixpath.normpath(value)


def validate_check(check: dict, spec: dict, run: dict) -> None:
    command = check.get("command")
    require(isinstance(command, list) and bool(command) and all(isinstance(arg, str) and arg for arg in command), "Missing executed command")
    require(isinstance(check.get("cwd"), str) and bool(check["cwd"]), "Missing executed cwd")
    require(type(check.get("exit_code")) is int, "Missing actual exit_code")
    require(check["exit_code"] != 0 or "executed_program" in check, "Missing executed_program for successful execution")
    start, end = timestamp(check.get("started_at")), timestamp(check.get("finished_at"))
    require(timestamp(run["started_at"]) <= start <= end <= timestamp(run["finished_at"]), "Check time outside execution window")
    variables = run.get("profile_variables", {})
    try:
        if "command" in spec:
            require(command == [arg.format_map(variables) for arg in spec["command"]], f"Executed command differs from profile: {spec['id']}")
        if "cwd" in spec:
            require(execution_path(check["cwd"]) == execution_path(spec["cwd"].format_map(variables)), "Executed cwd differs from profile")
        if "binary" in spec:
            require("binary" in check, "Missing expected binary")
            expected = spec["binary"].format_map(variables)
            actual = check["binary"]["path"]
            require(execution_path(actual) == execution_path(expected), "Executed binary differs from profile")
    except (KeyError, AttributeError, TypeError, ValueError) as error:
        if isinstance(error, EvidenceError):
            raise
        raise EvidenceError(f"Invalid command/cwd/binary profile binding: {error}") from error


def contained_path(path: Path, root: Path) -> Path:
    resolved = path.resolve()
    require(resolved.is_relative_to(root.resolve()), f"Evidence file escapes collected bundle: {path}")
    return resolved


def reference_path(ref: Any, base: Path, role: str, *, confined: bool = False) -> Path:
    require(isinstance(ref, dict) and isinstance(ref.get("path"), str) and bool(ref["path"]), f"Missing {role} file reference")
    require(isinstance(ref.get("sha256"), str) and SHA_RE.fullmatch(ref["sha256"]) is not None, f"Missing {role} digest")
    path = Path(ref["path"])
    path = path if path.is_absolute() else base / path
    if confined:
        path = contained_path(path, base)
    require(path.is_file(), f"Missing {role} file: {path}")
    return path


def file_name(ident: str, role: str) -> str:
    return f"inputs/{ident}/{role}"


def check_inputs(spec: dict, check: dict, base: Path, *, confined: bool) -> tuple[dict[str, Path], dict[str, str]]:
    roles = ["stdout", "stderr"] + [role for role in ("report", "binary", "executed_program") if role in check]
    require(spec["parser"] not in {"doctest", "lua"} or "binary" in roles, f"Missing runner binary: {spec['id']}")
    files, reports = {}, {}
    for role in roles:
        ref = check.get(role)
        if confined and isinstance(ref, dict):
            ref = {**ref, "path": file_name(spec["id"], role)}
        files[role] = reference_path(ref, base, role, confined=confined)
        if role in {"stdout", "stderr", "report"}:
            content = read_bounded(files[role])
            actual_digest = hashlib.sha256(content).hexdigest()
        else:
            actual_digest = digest(files[role])
        require(actual_digest == ref["sha256"], f"Wrong {role} digest: {files[role]}")
        if role in {"stdout", "stderr", "report"}:
            reports[role] = content.decode("utf-8-sig", errors="replace")
    return files, reports


def sanitizer_diagnostics(parser: str, reports: dict[str, str]) -> list[str]:
    """Reject runtime diagnostics in authenticated output, independently of exit/counts.

    This observes retained output only. Child reports discarded by a test need
    their own authenticated capture; bare tool names/flags are not diagnostics.
    """
    streams = [(role, reports[role]) for role in ("stdout", "stderr")]
    streams.extend((role, text) for role, text in reports.items() if role.startswith("sanitizer/"))
    if parser == "ctest-junit" and reports.get("report"):
        try:
            root = ET.fromstring(reports["report"])
        except ET.ParseError:
            pass  # The normal report parser will reject the malformed XML.
        else:
            for index, node in enumerate(root.iter()):
                if node.tag in {"system-out", "system-err"}:
                    streams.append((f"report/{node.tag}[{index}]", "".join(node.itertext())))
    family = r"(?:Address|Leak|UndefinedBehavior|Thread|Memory)Sanitizer"
    diagnostic = re.compile(
        rf"^[ \t]*(?:==\d+==)?(?:ERROR|WARNING|SUMMARY|FATAL):\s*{family}:"
        rf"|^[ \t]*{family}:DEADLYSIGNAL\b"
        r"|^[^\r\n]+:\d+(?::\d+)?:\s*runtime error:\s*\S"
    )
    reasons = []
    sgr = re.compile(r"\x1b\[[0-9;:]*m")
    for role, text in streams:
        for number, line in enumerate(text.splitlines(), 1):
            # LLVM can insert SGR colors inside "runtime error" diagnostics.
            # Normalize only the matching view; authenticated bytes stay intact.
            if diagnostic.search(sgr.sub("", line)):
                # Logs remain bound in full. Keep the manifest compact and do
                # not copy arbitrary program output into its diagnostic text.
                reasons.append(f"Sanitizer diagnostic in {role} at line {number}")
                break
    return reasons


def capture_inputs(check: dict, base: Path, *, required: bool, confined: bool) -> tuple[dict | None, dict, dict]:
    """Bind the full actual directory to the external executor's inventory."""
    capture = check.get("sanitizer_capture")
    require(capture is not None or not required, "Missing required sanitizer capture")
    if capture is None:
        require("sanitizer_capture" not in check, "Invalid null sanitizer capture")
        return None, {}, {}
    require(isinstance(capture, dict) and set(capture) == {
        "version", "complete", "directory", "prefix", "options_contract", "files",
    }, "Invalid sanitizer capture fields")
    require(type(capture["version"]) is int and capture["version"] == 1,
            "Unsupported sanitizer capture version")
    require(capture["complete"] is True, "Incomplete sanitizer capture")
    require(capture["prefix"] == "sanitizer" and capture["options_contract"] == OPTIONS_CONTRACT,
            "Unsupported sanitizer capture options contract")
    ident = check["id"]
    raw_directory = f"sanitizer/{ident}"
    bundle_directory = f"inputs/{ident}/sanitizer"
    require(capture["directory"] == raw_directory, "Wrong sanitizer capture directory")
    require(isinstance(capture["files"], list), "Missing sanitizer capture file list")
    expected = {}
    for ref in capture["files"]:
        require(isinstance(ref, dict) and set(ref) == {"path", "sha256", "size_bytes"},
                "Invalid sanitizer capture file reference")
        name = ref["path"]
        require(isinstance(name, str) and name.startswith(raw_directory + "/")
                and LOG_NAME.fullmatch(name[len(raw_directory) + 1:]) is not None,
                "Invalid sanitizer capture file path")
        basename = name[len(raw_directory) + 1:]
        require(basename not in expected, "Duplicate sanitizer capture file")
        require(isinstance(ref["sha256"], str) and SHA_RE.fullmatch(ref["sha256"]) is not None,
                "Invalid sanitizer capture digest")
        integer(ref["size_bytes"], "sanitizer capture size_bytes")
        expected[basename] = ref
    directory = base / (bundle_directory if confined else raw_directory)
    try:
        actual = read_capture_files(directory)
    except (CaptureError, OSError) as error:
        raise EvidenceError(str(error)) from error
    require(set(actual) == set(expected), "Sanitizer capture inventory differs from executor receipt")
    refs, copies, reports = [], {}, {}
    for name, data in actual.items():
        ref = expected[name]
        require(len(data) == ref["size_bytes"] and hashlib.sha256(data).hexdigest() == ref["sha256"],
                f"Sanitizer capture bytes differ from executor receipt: {name}")
        target = bundle_directory + "/" + name
        refs.append({**ref, "path": target})
        copies[target] = directory / name
        reports[raw_directory + "/" + name] = data.decode("utf-8-sig", errors="replace")
    return {**capture, "directory": bundle_directory, "files": refs}, copies, reports


def capture_inventory(base: Path, actual: dict, copies: dict, *, confined: bool) -> None:
    """Reject unknown/renamed/unbound files, including empty extra check dirs."""
    captured = {ident for ident, check in actual.items() if "sanitizer_capture" in check}
    try:
        if not confined:
            namespace = base / "sanitizer"
            if not captured and not namespace.exists() and not namespace.is_symlink():
                return
            plain_path(namespace, directory=True)
            require({path.name for path in namespace.iterdir()} == captured,
                    "Sanitizer capture namespace differs from executed checks")
            return
        # Each bound file and ancestor is canonical. Extra payload cannot hide a
        # diagnostic by renaming it outside the reserved sanitizer subdirectory.
        allowed_files = set(copies) | {"profile.json", "execution-receipt.json", "manifest.json"}
        allowed_dirs = {f"inputs/{ident}/sanitizer" for ident in captured}
        for name in allowed_files | set(allowed_dirs):
            allowed_dirs.update(p.as_posix() for p in Path(name).parents if p != Path("."))
        pending = [base]
        while pending:
            directory = pending.pop()
            plain_path(directory, directory=True)
            for path in directory.iterdir():
                relative = path.relative_to(base).as_posix()
                if path.is_dir():
                    require(relative in allowed_dirs, f"Unbound bundle directory: {relative}")
                    pending.append(path)
                else:
                    plain_path(path, directory=False)
                    require(relative in allowed_files, f"Unbound bundle file: {relative}")
    except (CaptureError, OSError) as error:
        raise EvidenceError(str(error)) from error


def result_for_check(spec: dict, check: dict, files: dict[str, Path], reports: dict[str, str]) -> dict:
    parsed_counts = None
    skipped = []
    diagnostics = sanitizer_diagnostics(spec["parser"], reports)
    reasons = list(diagnostics)
    if check["exit_code"] != 0:
        reasons.append(f"Process exit code {check['exit_code']}")
    try:
        parsed_counts, skipped = parse_report(spec["parser"], reports["stdout"], reports["stderr"], reports.get("report"))
        if parsed_counts is not None:
            if parsed_counts["executed"] == 0:
                reasons.append("No tests executed")
            if parsed_counts["discovered"] < spec.get("min_discovered", 1):
                reasons.append("Test discovery below profile minimum")
            if parsed_counts["failed"]:
                reasons.append("Tests failed")
            if any(name not in spec.get("allowed_skips", []) for name in skipped):
                reasons.append("Unexpected skipped tests: " + ", ".join(skipped))
    except EvidenceError as error:
        reasons.append(str(error))
    result = "PASS" if not reasons else "FAIL"
    if check["exit_code"] == 77 and not diagnostics:
        result = "SKIP"
    return {"id": spec["id"], "parser": spec["parser"], "required": spec["required"],
            "command": check["command"], "cwd": check["cwd"],
            "started_at": check["started_at"], "finished_at": check["finished_at"],
            "exit_code": check["exit_code"], "result": result, "reasons": reasons,
            "counts": parsed_counts, "skipped_tests": skipped,
            "files": {role: {"path": file_name(spec["id"], role), "sha256": check[role]["sha256"]} for role in files}}


def build_manifest(profile_path: Path, name: str, receipt_path: Path, *, collected_root: Path | None = None) -> tuple[dict, dict[str, Path]]:
    profile_bytes, receipt_bytes = read_bounded(profile_path), read_bounded(receipt_path)
    profile_sha256 = hashlib.sha256(profile_bytes).hexdigest()
    profile = load_profile(profile_path, name, content=profile_bytes)
    run = read_json(receipt_path, content=receipt_bytes)
    validate_context(run, profile, profile_sha256)
    require(run.get("profile_name") == name, "Execution receipt selects a different profile_name")
    require(isinstance(run.get("checks"), list), "Missing executed checks")
    specs = {spec["id"]: spec for spec in profile["checks"]}
    actual = {}
    for check in run["checks"]:
        require(isinstance(check, dict) and check.get("id") in specs, "Unknown executed check")
        require(check["id"] not in actual, "Duplicate executed check")
        actual[check["id"]] = check
    results = []
    copies = {}
    for ident, spec in specs.items():
        if ident not in actual:
            results.append({"id": ident, "parser": spec["parser"], "required": spec["required"],
                            "result": "NOT_RUN", "reasons": ["No execution receipt for this check"]})
            continue
        check = actual[ident]
        validate_check(check, spec, run)
        files, reports = check_inputs(spec, check, collected_root or receipt_path.parent, confined=collected_root is not None)
        copies.update({file_name(ident, role): path for role, path in files.items()})
        capture, capture_copies, capture_reports = capture_inputs(
            check, collected_root or receipt_path.parent,
            required="sanitizer_capture" in profile, confined=collected_root is not None)
        copies.update(capture_copies)
        reports.update(capture_reports)
        item = result_for_check(spec, check, files, reports)
        if capture is not None:
            item["sanitizer_capture"] = capture
        results.append(item)
    if "sanitizer_capture" in profile or any("sanitizer_capture" in check for check in actual.values()):
        capture_inventory(collected_root or receipt_path.parent, actual, copies, confined=collected_root is not None)
    successful = all(item["result"] == "PASS" for item in results if item["required"])
    changed = run["source_changed_during_run"] or run["fixtures_changed_during_run"]
    result = "PASS" if successful and not changed else "FAIL"
    manifest = {"schema_version": VERSION, "kind": "caesura-validation-evidence",
                "profile_name": name, "profile_sha256": profile_sha256,
                "receipt_sha256": hashlib.sha256(receipt_bytes).hexdigest(),
                "context": {key: value for key, value in run.items() if key != "checks"},
                "result": result, "checks": results}
    return manifest, copies


def collect_evidence(profile_path: Path, profile_name: str, run_path: Path, output: Path) -> dict:
    """Copy a single complete receipt to a new immutable run directory."""
    require(not output.exists(), f"Output/run_id already exists; refusing to overwrite: {output}")
    manifest, sources = build_manifest(profile_path, profile_name, run_path)
    context = manifest["context"]
    require(tuple(output.parts[-3:]) == (context["source_sha"], context["run_id"], profile_name),
            "Output must end with <source_sha>/<run_id>/<profile_name>")
    try:
        output.mkdir(parents=True, exist_ok=False)
        for check in manifest["checks"]:
            if "sanitizer_capture" in check:
                (output / check["sanitizer_capture"]["directory"]).mkdir(parents=True, exist_ok=False)
        for relative, source in sources.items():
            target = output / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
            require(digest(target) == digest(source), f"File changed while collecting: {source}")
        shutil.copyfile(run_path, output / "execution-receipt.json")
        shutil.copyfile(profile_path, output / "profile.json")
        # Re-read copies against the external receipt. A source changing during
        # copying can never acquire a completed manifest through a TOCTOU gap.
        copied, _ = build_manifest(profile_path, profile_name, run_path, collected_root=output)
        require(copied == manifest, "Execution input changed while collecting")
        original, _ = build_manifest(profile_path, profile_name, run_path)
        require(original == manifest, "Original execution input changed while collecting")
        require(digest(output / "execution-receipt.json") == manifest["receipt_sha256"], "Receipt changed while collecting")
        require(digest(output / "profile.json") == manifest["profile_sha256"], "Profile changed while collecting")
        with (output / "manifest.json").open("x", encoding="utf-8", newline="\n") as stream:
            json.dump(manifest, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
    except FileExistsError as error:
        raise EvidenceError(f"Output/run_id already exists: {output}") from error
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--profile-name", required=True)
    parser.add_argument("--run", type=Path, required=True, help="Receipt produced by the controlled executor")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        manifest = collect_evidence(args.profile, args.profile_name, args.run, args.output)
        print(f"EVIDENCE COLLECTION {manifest['result']}: {args.output / 'manifest.json'}")
        return 0 if manifest["result"] == "PASS" else 1
    except (EvidenceError, OSError) as error:
        print(f"EVIDENCE COLLECTION FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
