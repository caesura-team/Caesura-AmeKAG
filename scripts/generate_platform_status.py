#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
generate_platform_status.py — Authoritative Platform Status Matrix Generator

Parses docs/status/platform-matrix.yaml and validates recorded declarations and
document references. Original execution logs are verified only when explicitly
selected using --evidence-selection and its external identity locks.

Supports:
  --check        CI mode: validates schema and ensures docs/status/platform-status.md is fresh (exit 0 on match, 1 on diff/error).
                 Drift guard: when the effective head (--head or the auto-discovered evidence HEAD -- the
                 newest commit touching any path outside docs/) differs from the yaml 'evidence_head_commit', the
                 check fails loudly (exit 1) with a fix hint.
  --head <sha>   Explicit matrix head commit. Priority: --head > evidence HEAD > yaml static value; a
                 missing git env prints a [WARN] and falls back to yaml.
  --json         Outputs machine-readable JSON status summary to stdout (or --json-output <path>).
  --matrix <p>   Path to custom platform matrix YAML (default: docs/status/platform-matrix.yaml).
  --output <p>   Path to markdown output file (default: docs/status/platform-status.md).

Iron Rules:
  - Allowed status enums strictly restricted to 7 values.
  - Every 'verified' status MUST contain commit, document, test, and verified_at timestamp.
  - Referenced document paths must exist in repository.
  - iOS real device must remain hardware-gated without physical device evidence.
"""

import argparse
import datetime
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Any, Tuple, List, Dict, Optional

# Try importing yaml; provide robust fallback if pyyaml is unavailable
try:
    import yaml
    HAS_PYYAML = True
except ImportError:
    HAS_PYYAML = False

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_MATRIX_PATH = ROOT / "docs" / "status" / "platform-matrix.yaml"
DEFAULT_OUTPUT_PATH = ROOT / "docs" / "status" / "platform-status.md"


def normalize_freshness(md_text: str) -> str:
    """t196: Generated At is generator-execution time, so it is normalized out of
    byte comparisons (a re-check seconds later would otherwise look stale). Single
    source for both --check and the adversarial strict-sync gate
    (tests/scripts/test_platform_matrix_adversarial.py); same discipline as the
    capability_closure EOL normalization precedent."""
    return re.sub(r"> \*\*Generated At\*\*: .*", "",
                  md_text.replace("\r\n", "\n")).strip()

VALID_STATUS_ENUMS = {
    "verified",
    "probe",
    "pending",
    "hardware-gated",
    "credential-gated",
    "blocked",
    "not-applicable",
}

STATUS_BADGES = {
    "verified": "🟢 `verified`",
    "probe": "🟡 `probe`",
    "pending": "⏳ `pending`",
    "hardware-gated": "🔒 `hardware-gated`",
    "credential-gated": "🔑 `credential-gated`",
    "blocked": "🔴 `blocked`",
    "not-applicable": "⚪ `n/a`",
}

STATUS_SYMBOLS = {
    "verified": "🟢 Verified",
    "probe": "🟡 Probe",
    "pending": "⏳ Pending",
    "hardware-gated": "🔒 Hardware-gated",
    "credential-gated": "🔑 Credential-gated",
    "blocked": "🔴 Blocked",
    "not-applicable": "⚪ N/A",
}


def _parse_val(val_str: str) -> Any:
    val_str = val_str.strip()
    if val_str in ("null", "~", "", "None"):
        return None
    if val_str in ("true", "True"):
        return True
    if val_str in ("false", "False"):
        return False
    if (val_str.startswith('"') and val_str.endswith('"')) or (val_str.startswith("'") and val_str.endswith("'")):
        return val_str[1:-1]
    try:
        if "." in val_str:
            return float(val_str)
        return int(val_str)
    except ValueError:
        return val_str


def _parse_yaml_fallback(text: str) -> Any:
    """Fallback recursive indentation-based YAML parser for basic types, dicts and lists."""
    lines = text.splitlines()
    cleaned = []
    for line in lines:
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        indent = len(line) - len(line.lstrip(" "))
        cleaned.append((indent, stripped))

    if not cleaned:
        return {}

    def _parse_block(idx: int, parent_indent: int) -> Tuple[Any, int]:
        if idx >= len(cleaned):
            return {}, idx
        cur_indent, first_line = cleaned[idx]
        if cur_indent < parent_indent:
            return {}, idx

        if first_line.startswith("- "):
            result_list = []
            while idx < len(cleaned):
                ind, line = cleaned[idx]
                if ind < cur_indent:
                    break
                if ind == cur_indent and line.startswith("- "):
                    item_content = line[2:].strip()
                    if not item_content:
                        sub, next_idx = _parse_block(idx + 1, cur_indent + 1)
                        result_list.append(sub)
                        idx = next_idx
                    elif ":" in item_content and not item_content.startswith('"') and not item_content.startswith("'"):
                        k, v = item_content.split(":", 1)
                        k = k.strip()
                        v = v.strip()
                        sub_dict = {k: _parse_val(v) if v else {}}
                        if not v:
                            nested, next_idx = _parse_block(idx + 1, cur_indent + 2)
                            sub_dict[k] = nested
                            idx = next_idx
                        else:
                            idx += 1
                        result_list.append(sub_dict)
                    else:
                        result_list.append(_parse_val(item_content))
                        idx += 1
                else:
                    break
            return result_list, idx
        else:
            result_dict = {}
            while idx < len(cleaned):
                ind, line = cleaned[idx]
                if ind < cur_indent:
                    break
                if ind == cur_indent:
                    if ":" in line:
                        k, v = line.split(":", 1)
                        k = k.strip()
                        v = v.strip()
                        if not v:
                            sub, next_idx = _parse_block(idx + 1, cur_indent + 1)
                            result_dict[k] = sub
                            idx = next_idx
                        else:
                            result_dict[k] = _parse_val(v)
                            idx += 1
                    else:
                        idx += 1
                else:
                    break
            return result_dict, idx

    res, _ = _parse_block(0, 0)
    return res


def load_yaml(file_path: Path) -> dict:
    """Load YAML file with PyYAML or robust fallback parser."""
    if not file_path.exists():
        raise FileNotFoundError(f"Matrix file not found: {file_path}")

    text = file_path.read_text(encoding="utf-8")
    if HAS_PYYAML:
        return yaml.safe_load(text)

    return _parse_yaml_fallback(text)


def validate_matrix(data: dict, repo_root: Path) -> list[str]:
    """Validate matrix schema and evidence integrity. Returns list of errors."""
    errors = []

    if not isinstance(data, dict):
        return ["Root YAML content must be a dictionary."]

    version = data.get("version")
    if version != 1:
        errors.append(f"Invalid or missing version: {version} (expected 1)")

    allowed_enums = set(data.get("allowed_status_enums", []))
    if not allowed_enums:
        errors.append("allowed_status_enums missing or empty in matrix.")
    else:
        diff = allowed_enums - VALID_STATUS_ENUMS
        if diff:
            errors.append(f"Unknown status enums declared in allowed_status_enums: {diff}")

    platforms = data.get("platforms")
    if not isinstance(platforms, dict) or not platforms:
        errors.append("platforms section is missing or not a dictionary.")
        return errors

    expected_platforms = ["windows", "linux", "web", "android", "macos", "ios"]
    for p in expected_platforms:
        if p not in platforms:
            errors.append(f"Missing required target platform: '{p}'")

    hex_re = re.compile(r"^[0-9a-fA-F]{7,40}$")

    for plat_name, plat_data in platforms.items():
        if not isinstance(plat_data, dict):
            errors.append(f"Platform '{plat_name}' must be a dictionary.")
            continue

        display_name = plat_data.get("display_name")
        if not display_name:
            errors.append(f"Platform '{plat_name}' missing display_name.")

        tier = plat_data.get("tier")
        if tier not in (1, 2):
            errors.append(f"Platform '{plat_name}' tier must be 1 or 2, got {tier}.")

        summary_status = plat_data.get("summary_status")
        if summary_status not in VALID_STATUS_ENUMS:
            errors.append(
                f"Platform '{plat_name}' invalid summary_status '{summary_status}'."
            )

        caps = plat_data.get("capabilities", {})
        if not isinstance(caps, dict) or not caps:
            errors.append(f"Platform '{plat_name}' has no capabilities defined.")
            continue

        for cap_name, cap_info in caps.items():
            if not isinstance(cap_info, dict):
                errors.append(
                    f"Platform '{plat_name}' capability '{cap_name}' must be a dictionary."
                )
                continue

            status = cap_info.get("status")
            if status not in VALID_STATUS_ENUMS:
                errors.append(
                    f"Platform '{plat_name}' capability '{cap_name}' invalid status '{status}'. "
                    f"Must be one of: {sorted(VALID_STATUS_ENUMS)}"
                )
                continue

            # Strict evidence check for 'verified' capabilities
            if status == "verified":
                evidence = cap_info.get("evidence")
                if not isinstance(evidence, dict) or not evidence:
                    errors.append(
                        f"Platform '{plat_name}' capability '{cap_name}' is 'verified' but missing evidence dictionary."
                    )
                    continue

                raw_commit = evidence.get("commit")
                commit = str(raw_commit).strip() if raw_commit is not None else ""
                if not commit or not hex_re.match(commit):
                    errors.append(
                        f"Platform '{plat_name}' capability '{cap_name}' evidence commit '{commit}' is invalid (must be 7-40 hex chars)."
                    )

                raw_doc = evidence.get("document")
                doc_rel = str(raw_doc).strip() if raw_doc is not None else ""
                if not doc_rel or doc_rel.lower() == "none":
                    errors.append(
                        f"Platform '{plat_name}' capability '{cap_name}' evidence document path is empty."
                    )
                else:
                    doc_path = repo_root / doc_rel
                    if not doc_path.exists():
                        errors.append(
                            f"Platform '{plat_name}' capability '{cap_name}' referenced document does not exist: {doc_rel}"
                        )

                raw_test = evidence.get("test")
                test_cmd = str(raw_test).strip() if raw_test is not None else ""
                if not test_cmd or test_cmd.lower() == "none":
                    errors.append(
                        f"Platform '{plat_name}' capability '{cap_name}' evidence test command is empty."
                    )

                raw_verified_at = evidence.get("verified_at")
                verified_at = str(raw_verified_at).strip() if raw_verified_at is not None else ""
                if not verified_at or verified_at.lower() == "none":
                    errors.append(
                        f"Platform '{plat_name}' capability '{cap_name}' evidence verified_at timestamp is empty."
                    )

            elif status == "probe" and isinstance(cap_info.get("evidence"), dict):
                evidence = cap_info["evidence"]
                raw_doc = evidence.get("document")
                if raw_doc:
                    doc_path = repo_root / str(raw_doc)
                    if not doc_path.exists():
                        errors.append(
                            f"Platform '{plat_name}' probe capability '{cap_name}' referenced document does not exist: {raw_doc}"
                        )

            # iOS real_device MUST be hardware-gated
            if plat_name == "ios" and cap_name == "real_device":
                if status != "hardware-gated":
                    errors.append(
                        f"Platform 'ios' capability 'real_device' must be 'hardware-gated', got '{status}'."
                    )

    return errors


def _git_evidence_head() -> Optional[str]:
    """Return the newest commit that touched any path OUTSIDE docs/ (evidence HEAD).

    `git log -1 --format=%H -- . ':(exclude)docs'` (git path args passed as a
    Python list -- never through a shell). A matrix-sync commit that ONLY
    changes docs/ therefore does not move this pointer: comparing the sync
    commit's own tip against the yaml would be self-referential (the sync
    commit's sha depends on the yaml content), making the drift gate perpetually
    red on the very commit that syncs the matrix.

    Shallow-clone guard: CI checks out with fetch-depth:1, where `git log` has
    no parent diff -- the HEAD commit looks like a root commit that touched
    every path, so the log would report the docs-only sync tip as the evidence
    HEAD and falsely red the gate. When `git rev-parse --is-shallow-repository`
    says the repo is shallow, a [WARN] is printed and None is returned (the yaml
    fallback wins and drift is skipped). A failing is-shallow probe is NOT
    fatal: the git log path proceeds exactly as before.
    """
    try:
        shallow_proc = subprocess.run(
            ["git", "rev-parse", "--is-shallow-repository"],
            cwd=str(ROOT),
            capture_output=True,
            text=True,
            timeout=15,
        )
    except Exception:
        shallow_proc = None
    if (
        shallow_proc is not None
        and shallow_proc.returncode == 0
        and (shallow_proc.stdout or "").strip() == "true"
    ):
        print(
            "[WARN] git repo is shallow; evidence HEAD unavailable -- using matrix yaml evidence_head_commit.",
            file=sys.stderr,
        )
        return None
    try:
        proc = subprocess.run(
            ["git", "log", "-1", "--format=%H", "--", ".", ":(exclude)docs"],
            cwd=str(ROOT),
            capture_output=True,
            text=True,
            timeout=15,
        )
    except Exception:
        return None
    if proc.returncode != 0:
        return None
    head = (proc.stdout or "").strip()
    return head[:40] if head else None


def _yaml_evidence_head(data: dict, default: str = "") -> str:
    """Read the matrix's evidence head commit (key 'evidence_head_commit' wins).

    Backward compatibility: a legacy 'head_commit' key is still accepted, so
    pre-migration boards keep working until the docs sync task renames the key.
    The generator itself never writes the YAML (read-only consumer).
    """
    val = data.get("evidence_head_commit")
    if val is None:
        val = data.get("head_commit")
    return str(val).strip() if val is not None else default


def resolve_head_commit(cli_head: Optional[str], data: dict) -> Tuple[Optional[str], str]:
    """Resolve the effective matrix head commit.

    Priority: --head > evidence HEAD (`git log -1 --format=%H -- . ':(exclude)docs'`,
    newest commit outside docs/) > yaml static value.
    Returns (effective_head, source) with source in {'cli', 'git', 'yaml'}; the
    input data dict is never mutated. When the evidence HEAD cannot be resolved
    (git missing, ROOT not a repository, or a shallow clone) a [WARN] line goes
    to stderr and the yaml value is used, so a degraded git env never falsely
    reds --check (drift is then skipped).
    """
    if cli_head:
        return cli_head.strip()[:40], "cli"
    git_head = _git_evidence_head()
    if git_head:
        return git_head, "git"
    yaml_str = _yaml_evidence_head(data)
    print(
        f"[WARN] evidence HEAD unavailable (git missing, ROOT not a repository, or shallow clone); "
        f"falling back to matrix evidence_head_commit={yaml_str!r}.",
        file=sys.stderr,
    )
    return (yaml_str if yaml_str else None), "yaml"


def _unverified_current_evidence() -> dict:
    return {"status": "NOT_RUN", "current_verified": False, "release_ready": False,
            "claims": {}, "scope": "No external execution or package selection was supplied."}


def _markdown_cell(value: Any) -> str:
    return str(value).replace("|", "\\|").replace("\r", " ").replace("\n", " ").replace("`", "'")


def _current_evidence_markdown(verification: Optional[dict]) -> list[str]:
    result = verification or _unverified_current_evidence()
    lines = ["## Explicitly Selected Current Evidence", "",
             f"Verification result: `{_markdown_cell(result['status'])}`. Publication approval: not granted."]
    if result.get("source_sha"):
        lines.append(f"Selected source: `{_markdown_cell(result['source_sha'])}`.")
    if not result.get("claims"):
        lines.extend(["No original execution or package bytes were selected for verification in this generation.", ""])
        return lines
    lines.extend(["", "| Claim | Result | Verification scope | Raw stage evidence |",
                  "|---|---|---|---|"])
    for claim_id, claim in result['claims'].items():
        cells = [claim_id, claim.get('result'), claim.get('verification_scope'), claim.get('raw_stage_evidence', 'NOT_RUN')]
        lines.append("| " + " | ".join(_markdown_cell(value) for value in cells) + " |")
    lines.extend(["", "Execution profiles, package byte checks, physical devices and hosted authentication remain separate scopes.", ""])
    return lines


def generate_markdown(data: dict, head_commit: Optional[str] = None,
                      evidence_verification: Optional[dict] = None) -> str:
    """Generate docs/status/platform-status.md content from validated matrix data.

    head_commit is the effective commit resolved by resolve_head_commit(); when
    None it is resolved here the same way (evidence HEAD -- newest commit outside
    docs/ -- else the yaml static value) so a plain library call mirrors the CLI
    output byte-for-byte. The rendered bytes depend ONLY on the final head value,
    never on how it was resolved (git / --head / yaml), so a shallow clone and a
    full-history clone produce identical markdown. The input data dict is never
    mutated.
    """
    if head_commit is None:
        eager_git = _git_evidence_head()
        if eager_git:
            head_commit = eager_git
        else:
            head_commit = _yaml_evidence_head(data, default="unknown")
    # t196: generated_at = generator EXECUTION time. The yaml's legacy
    # `last_updated` was a hand-edited value (it drifted from reality) and is
    # no longer read -- an old yaml carrying the key is simply ignored. The
    # timestamp is display-only and is normalized out of the --check byte
    # comparison (see below).
    generated_at = datetime.datetime.now(datetime.timezone.utc).isoformat()
    platforms = data.get("platforms", {})

    lines = []
    lines.append("<!-- AUTO-GENERATED FILE — DO NOT EDIT DIRECTLY -->")
    lines.append("<!-- Generated by scripts/generate_platform_status.py from docs/status/platform-matrix.yaml -->")
    lines.append("")
    lines.append("# Caesura (AmeKAG) — Unified Platform Status Matrix")
    lines.append("")
    lines.append(f"> **Single Source of Truth**: [`docs/status/platform-matrix.yaml`](platform-matrix.yaml)<br>")
    lines.append(f"> **Documentation Synchronization Commit**: `{head_commit}`<br>")
    lines.append(f"> **Generated At**: `{generated_at}`<br>")
    lines.append("> **Recorded Evidence Validation**: `NOT_REVERIFIED` — schema and document references checked; original logs, packages and devices are not revalidated by this table.")
    lines.append("> Capability statuses below preserve their recorded commits and dates. The synchronization commit is not an execution identity or release approval.")
    lines.append("")
    lines.extend(_current_evidence_markdown(evidence_verification))
    lines.append("")
    lines.append("---")
    lines.append("")
    lines.append("## 1. Recorded Platform Status Summary")
    lines.append("")
    lines.append("| Platform | Tier | Summary Status | Build | Runtime | First-VN | Device / Browser | Package / Sign | Release Gate |")
    lines.append("|---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|")

    for plat_id, plat in platforms.items():
        name = plat.get("display_name", plat_id)
        tier = plat.get("tier", 1)
        summary = STATUS_BADGES.get(plat.get("summary_status"), plat.get("summary_status"))
        caps = plat.get("capabilities", {})

        b_stat = STATUS_BADGES.get(caps.get("build", {}).get("status", "not-applicable"))
        r_stat = STATUS_BADGES.get(caps.get("runtime", {}).get("status", "not-applicable"))
        f_stat = STATUS_BADGES.get(caps.get("first_vn", {}).get("status", "not-applicable"))

        # Device / Browser column
        if "real_device" in caps:
            d_stat = STATUS_BADGES.get(caps["real_device"].get("status"))
        elif "browser" in caps:
            d_stat = STATUS_BADGES.get(caps["browser"].get("status"))
        else:
            d_stat = "—"

        # Package / Sign column
        if "signing" in caps:
            p_stat = STATUS_BADGES.get(caps["signing"].get("status"))
        elif "packaging" in caps:
            p_stat = STATUS_BADGES.get(caps["packaging"].get("status"))
        elif "metal" in caps:
            p_stat = STATUS_BADGES.get(caps["metal"].get("status"))
        else:
            p_stat = "—"

        rel_stat = STATUS_BADGES.get(
            caps.get("release", {}).get("status", caps.get("release_candidate", {}).get("status", "pending"))
        )

        lines.append(f"| **{name}** | Tier {tier} | {summary} | {b_stat} | {r_stat} | {f_stat} | {d_stat} | {p_stat} | {rel_stat} |")

    lines.append("")
    lines.append("### Status Enum Definitions")
    lines.append("")
    lines.append("| Enum | Symbol | Definition & Release Rules |")
    lines.append("|---|---|---|")
    lines.append("| `verified` | 🟢 `verified` | Historical verification recorded at the row's commit and date; original execution is not reverified by document-reference validation. |")
    lines.append("| `probe` | 🟡 `probe` | Recorded compiler / toolchain / shader probe scope; windowed or device runtime is not implied. |")
    lines.append("| `pending` | ⏳ `pending` | Capability planned or currently undergoing implementation / verification. |")
    lines.append("| `hardware-gated` | 🔒 `hardware-gated` | Blocked exclusively by physical absence of target hardware (e.g., physical Apple Silicon Mac / iPhone). |")
    lines.append("| `credential-gated` | 🔑 `credential-gated` | Blocked exclusively by missing production certificates or deployment credentials (e.g. Apple Developer ID). |")
    lines.append("| `blocked` | 🔴 `blocked` | Execution halted due to an unresolved upstream or architectural defect. |")
    lines.append("| `not-applicable` | ⚪ `n/a` | Dimension does not apply to target platform. |")
    lines.append("")
    lines.append("---")
    lines.append("")
    lines.append("## 2. Platform-by-Platform Detailed Breakdown")
    lines.append("")

    for plat_id, plat in platforms.items():
        name = plat.get("display_name", plat_id)
        tier = plat.get("tier", 1)
        summary = STATUS_BADGES.get(plat.get("summary_status"), plat.get("summary_status"))
        env = plat.get("environment", {})
        gates = plat.get("gates", {})
        caps = plat.get("capabilities", {})

        lines.append(f"### 2.{list(platforms.keys()).index(plat_id) + 1} {name} (Tier {tier}) — {summary}")
        lines.append("")

        if env:
            lines.append("#### Environment & Runtime Stack")
            lines.append("")
            for k, v in env.items():
                label = k.replace("_", " ").title()
                lines.append(f"- **{label}**: `{v}`")
            lines.append("")

        if gates:
            lines.append("#### Gating Boundaries")
            lines.append("")
            for gk, gv in gates.items():
                glabel = gk.replace("_", " ").title()
                lines.append(f"- **{glabel} Gate**: {gv}")
            lines.append("")

        lines.append("#### Capability Matrix & Evidence")
        lines.append("")
        lines.append("| Capability | Status | Evidence Document | Verification Command / Procedure | Commit | Verified At | Notes |")
        lines.append("|---|:---:|---|---|:---:|:---:|---|")

        for cap_id, cap in caps.items():
            cap_title = cap_id.replace("_", " ").title()
            c_stat = STATUS_BADGES.get(cap.get("status"), cap.get("status"))
            ev = cap.get("evidence", {})
            doc = f"[`{ev.get('document')}`](../../{ev.get('document')})" if ev.get("document") else "—"
            test_cmd = f"`{ev.get('test')}`" if ev.get("test") else "—"
            commit = f"`{ev.get('commit')}`" if ev.get("commit") else "—"
            verified_at = f"`{ev.get('verified_at')}`" if ev.get("verified_at") else "—"
            notes = ev.get("notes", "—")

            lines.append(f"| **{cap_title}** | {c_stat} | {doc} | {test_cmd} | {commit} | {verified_at} | {notes} |")

        lines.append("")

    lines.append("---")
    lines.append("")
    lines.append("## 3. Global Evidence Registry Index")
    lines.append("")
    lines.append("These links locate recorded declarations and execution references. An existing source, workflow or document is not proof that its command ran or that an original log or package is available:")
    lines.append("")

    evidence_items = []
    for plat_id, plat in platforms.items():
        pname = plat.get("display_name", plat_id)
        for cap_id, cap in plat.get("capabilities", {}).items():
            ev = cap.get("evidence")
            if ev and ev.get("document"):
                evidence_items.append((pname, cap_id, cap.get("status"), ev))

    lines.append("| Platform | Capability | Status | Anchor Document | Execution Test Command | Commit SHA |")
    lines.append("|---|---|:---:|---|---|:---:|")
    for pname, cap_id, stat, ev in evidence_items:
        s_badge = STATUS_BADGES.get(stat, stat)
        doc = f"[`{ev.get('document')}`](../../{ev.get('document')})"
        test = f"`{ev.get('test')}`"
        commit = f"`{ev.get('commit')}`"
        cap_title = cap_id.replace("_", " ").title()
        lines.append(f"| {pname} | {cap_title} | {s_badge} | {doc} | {test} | {commit} |")

    lines.append("")
    lines.append("---")
    lines.append("")
    lines.append("## 4. Release Candidate Gate & Blockers")
    lines.append("")
    lines.append("The following values come from the recorded matrix. They are not a current candidate gate and do not grant publication approval.")
    lines.append("")
    for plat_id, plat in platforms.items():
        caps = plat.get("capabilities", {})
        values = [f"{name}=`{cap.get('status', 'pending')}`" for name, cap in caps.items()]
        lines.append(f"- **{plat.get('display_name', plat_id)}**: " + "; ".join(values) + ". Original evidence: `NOT_REVERIFIED`.")
    lines.append("")
    lines.append("<!-- End of auto-generated platform status matrix -->")
    lines.append("")

    return "\n".join(lines)


def export_json(data: dict, evidence_verification: Optional[dict] = None) -> str:
    """Export clean summary JSON representation of platform status."""
    platforms = data.get("platforms", {})
    summary = {
        "schema_version": data.get("schema_version", "1.0.0"),
        "evidence_head_commit": _yaml_evidence_head(data),
        "generated_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "recorded_evidence_validation": "NOT_REVERIFIED",
        "release_ready": False,
        "current_evidence": evidence_verification or _unverified_current_evidence(),
        "platforms": {},
    }

    for plat_id, plat in platforms.items():
        summary["platforms"][plat_id] = {
            "display_name": plat.get("display_name"),
            "tier": plat.get("tier"),
            "summary_status": plat.get("summary_status"),
            "capabilities": {
                cap_id: {
                    "status": cap_info.get("status"),
                    "evidence": cap_info.get("evidence"),
                }
                for cap_id, cap_info in plat.get("capabilities", {}).items()
            },
        }

    return json.dumps(summary, indent=2, ensure_ascii=False)


def main():
    parser = argparse.ArgumentParser(
        description="Authoritative Platform Status Matrix Generator and Validator"
    )
    parser.add_argument(
        "--matrix",
        type=Path,
        default=DEFAULT_MATRIX_PATH,
        help="Path to platform-matrix.yaml (default: docs/status/platform-matrix.yaml)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=DEFAULT_OUTPUT_PATH,
        help="Path to generated markdown output (default: docs/status/platform-status.md)",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="CI freshness check: validate schema and verify generated markdown matches existing file.",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Emit JSON summary to stdout.",
    )
    parser.add_argument(
        "--json-output",
        type=Path,
        default=None,
        help="Write JSON summary to specified path.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Validate schema and generate markdown in memory without writing to disk.",
    )
    parser.add_argument(
        "--head",
        type=str,
        default=None,
        metavar="SHA",
        help="Explicit matrix head commit SHA (overrides auto git HEAD; used by generation and --check drift detection).",
    )
    parser.add_argument("--evidence-selection", type=Path,
                        help="External immutable selection of original execution bundles or final package receipts.")
    parser.add_argument("--evidence-selection-sha256", help="Caller-selected SHA256 of the selection file.")
    parser.add_argument("--candidate-source", help="Full source SHA expected in every selected claim; not the docs synchronization anchor.")
    parser.add_argument("--evidence-root", type=Path, help="Explicit root containing selected artifacts; internal paths must remain within it.")
    parser.add_argument("--evidence-profile", type=Path, help="Caller-selected validation profile, outside each execution bundle.")
    parser.add_argument("--evidence-report", type=Path, help="Write a new verification JSON report, including failures; never overwrite an earlier report.")

    args = parser.parse_args()
    evidence_arguments = (args.evidence_selection, args.evidence_selection_sha256, args.candidate_source,
                          args.evidence_root, args.evidence_profile)
    if any(value is not None for value in evidence_arguments) or args.evidence_report is not None:
        if not all(value is not None for value in evidence_arguments):
            parser.error("Evidence verification requires --evidence-selection, --evidence-selection-sha256, "
                         "--candidate-source, --evidence-root and --evidence-profile together.")
        if not args.dry_run and args.output.resolve() == DEFAULT_OUTPUT_PATH.resolve():
            parser.error("Selected local evidence requires --dry-run or a separate --output artifact; "
                         "the default repository document records historical declarations without local artifacts.")
        output_paths = [path for path in (args.evidence_report, args.json_output,
                        args.output if not args.dry_run and not args.check else None) if path is not None]
        resolved_outputs = [path.resolve() for path in output_paths]
        if len(set(resolved_outputs)) != len(resolved_outputs):
            parser.error("Evidence report, JSON and Markdown outputs must be distinct paths.")
        if any(path.exists() or path.is_symlink() for path in output_paths):
            parser.error("Selected evidence outputs must be new paths; existing evidence will not be overwritten.")

    # 1. Load YAML
    try:
        data = load_yaml(args.matrix)
    except Exception as e:
        print(f"[ERROR] Failed to load matrix YAML '{args.matrix}': {e}", file=sys.stderr)
        sys.exit(1)

    # 2. Validate Schema & Evidence Integrity
    errors = validate_matrix(data, ROOT)
    if errors:
        print(f"[ERROR] Platform matrix validation failed with {len(errors)} error(s):", file=sys.stderr)
        for err in errors:
            print(f"  - {err}", file=sys.stderr)
        sys.exit(1)

    # 2b. Resolve the effective head commit (--head > evidence HEAD > yaml static value).
    effective_head, head_source = resolve_head_commit(args.head, data)
    evidence_verification = None
    if args.evidence_selection is not None:
        from platform_evidence import verify_current_evidence
        evidence_verification = verify_current_evidence(
            args.evidence_selection, selection_sha256=args.evidence_selection_sha256,
            expected_source_sha=args.candidate_source, profile_path=args.evidence_profile,
            evidence_root=args.evidence_root)
        if args.evidence_report is not None:
            try:
                args.evidence_report.parent.mkdir(parents=True, exist_ok=True)
                with args.evidence_report.open('x', encoding='utf-8') as stream:
                    json.dump(evidence_verification, stream, ensure_ascii=False, indent=2)
                    stream.write('\n')
            except OSError as error:
                print(f"[ERROR] Cannot preserve evidence report: {error}", file=sys.stderr)
                return 1
        if evidence_verification.get('status') != 'CURRENT_EVIDENCE_VERIFIED' or not evidence_verification.get('current_verified'):
            print("[ERROR] Selected current evidence failed verification: " +
                  json.dumps(evidence_verification, ensure_ascii=False), file=sys.stderr)
            return 1

    # 3. Handle JSON output
    if args.json or args.json_output:
        json_str = export_json(data, evidence_verification)
        if args.json_output:
            args.json_output.parent.mkdir(parents=True, exist_ok=True)
            try:
                with args.json_output.open('x' if evidence_verification is not None else 'w', encoding='utf-8') as stream:
                    stream.write(json_str + '\n')
            except OSError as error:
                print(f"[ERROR] Cannot write JSON output: {error}", file=sys.stderr)
                return 1
            print(f"[OK] Wrote JSON status to {args.json_output}")
        if args.json:
            print(json_str)
        if not args.check and not args.output:
            return

    # 4. Generate Markdown (the matrix documents the effective head, not the raw yaml value)
    generated_md = generate_markdown(data, effective_head, evidence_verification)

    # 5. Check mode (CI Freshness Guard)
    if args.check:
        if not args.output.exists():
            print(
                f"[ERROR] Output file '{args.output}' does not exist. Run generator to create it.",
                file=sys.stderr,
            )
            sys.exit(1)

        # 5a. Drift guard: the matrix must describe the commit the code is at.
        # The baseline is the evidence HEAD (newest commit outside docs/), so a
        # matrix-sync commit that only touches docs/ does not itself create
        # drift. The yaml may hold a short sha (the existing 8-char convention,
        # e.g. 93bd5c33) that is a PREFIX of the 40-char effective HEAD -- those
        # denote the same commit and count as synced. An unresolved effective
        # head (no --head and no usable git) skips this check -- a missing git
        # env must not falsely red the freshness gate.
        yaml_head_raw = _yaml_evidence_head(data)
        if yaml_head_raw and effective_head is not None:
            same_commit = (
                effective_head == yaml_head_raw
                or effective_head.startswith(yaml_head_raw)
                or yaml_head_raw.startswith(effective_head)
            )
            if not same_commit:
                print(
                    f"[ERROR] Platform matrix data lags the code HEAD: yaml evidence_head_commit={yaml_head_raw}, "
                    f"effective HEAD={effective_head} (source: {head_source})",
                    file=sys.stderr,
                )
                print(
                    "        Fix: update `evidence_head_commit` in the matrix YAML "
                    "(docs/status/platform-matrix.yaml) to the current evidence HEAD and re-run the generator "
                    "(python scripts/generate_platform_status.py), or run `--check --head <sha>` to verify the docs "
                    "against a specific commit only.",
                    file=sys.stderr,
                )
                sys.exit(1)

        existing_md = args.output.read_text(encoding="utf-8")
        # t196: Generated At is generator-execution time, so it is normalized
        # out of the byte comparison (a re-check seconds later would otherwise
        # look stale). See module-level normalize_freshness() for the single
        # source shared with the adversarial strict-sync gate.
        if normalize_freshness(existing_md) != normalize_freshness(generated_md):
            print(
                f"[ERROR] '{args.output}' is stale or modified. Run `python scripts/generate_platform_status.py` to regenerate.",
                file=sys.stderr,
            )
            sys.exit(1)

        print(f"[OK] Platform status matrix is valid and '{args.output}' is up-to-date.")
        sys.exit(0)

    # 6. Write Output
    if not args.dry_run:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        try:
            with args.output.open('x' if evidence_verification is not None else 'w', encoding='utf-8', newline='\n') as stream:
                stream.write(generated_md)
        except OSError as error:
            print(f"[ERROR] Cannot write Markdown output: {error}", file=sys.stderr)
            return 1
        print(f"[OK] Successfully validated schema and generated {args.output} ({len(generated_md.splitlines())} lines).")
    else:
        print(f"[OK] Dry run successful. Generated {len(generated_md.splitlines())} lines (not written to disk).")


if __name__ == "__main__":
    sys.exit(main())
