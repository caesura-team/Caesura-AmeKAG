#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
test_platform_matrix_adversarial.py — Adversarial Verification Suite for Platform Status Matrix

Adversarial Stress Test Suite covering:
1. Schema integrity and corruption attacks
2. Platform metadata and capability state mutations
3. Evidence requirements and commit/document validation
4. Iron rule enforcement (iOS hardware-gated constraint)
5. Edge case mutations (null/None fields, whitespace, casing)
6. CI freshness guard and desynchronization detection (--check)
7. CLI interfaces (--json, --json-output, --dry-run, --matrix, --output)
8. Production matrix zero-drift invariants
"""

import copy
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT))
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import generate_platform_status as gps


class TestPlatformMatrixAdversarial(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.matrix_path = REPO_ROOT / "docs" / "status" / "platform-matrix.yaml"
        cls.status_md_path = REPO_ROOT / "docs" / "status" / "platform-status.md"
        cls.script_path = REPO_ROOT / "scripts" / "generate_platform_status.py"
        cls.valid_data = gps.load_yaml(cls.matrix_path)
        # Snapshot consistency uses its recorded evidence commit. The checkout
        # can advance without making those historical observations current.
        cls.recorded_head = cls.valid_data["evidence_head_commit"]

    def get_clean_data(self) -> dict:
        return copy.deepcopy(self.valid_data)

    # -------------------------------------------------------------------------
    # 1. Root & Schema Level Mutation Attacks
    # -------------------------------------------------------------------------
    def test_root_not_dict_rejected(self):
        """Verify non-dictionary root YAML is rejected."""
        errors = gps.validate_matrix(["not", "a", "dict"], REPO_ROOT)
        self.assertTrue(len(errors) > 0)
        self.assertIn("must be a dictionary", errors[0])

        errors_str = gps.validate_matrix("just a string", REPO_ROOT)
        self.assertTrue(len(errors_str) > 0)
        self.assertIn("must be a dictionary", errors_str[0])

    def test_invalid_version_rejected(self):
        """Verify invalid or missing schema version is rejected."""
        data = self.get_clean_data()
        data["version"] = 2
        errors = gps.validate_matrix(data, REPO_ROOT)
        self.assertTrue(any("expected 1" in e for e in errors))

        data["version"] = "1"
        errors = gps.validate_matrix(data, REPO_ROOT)
        self.assertTrue(any("expected 1" in e for e in errors))

        del data["version"]
        errors = gps.validate_matrix(data, REPO_ROOT)
        self.assertTrue(any("expected 1" in e for e in errors))

    def test_allowed_status_enums_mutation_rejected(self):
        """Verify unknown or missing allowed_status_enums are rejected."""
        data = self.get_clean_data()
        data["allowed_status_enums"] = []
        errors = gps.validate_matrix(data, REPO_ROOT)
        self.assertTrue(any("allowed_status_enums missing or empty" in e for e in errors))

        data["allowed_status_enums"] = ["verified", "almost-done", "ready-ish", "complete"]
        errors = gps.validate_matrix(data, REPO_ROOT)
        self.assertTrue(any("Unknown status enums declared in allowed_status_enums" in e for e in errors))

    def test_missing_platforms_section_rejected(self):
        """Verify missing platforms section is rejected."""
        data = self.get_clean_data()
        del data["platforms"]
        errors = gps.validate_matrix(data, REPO_ROOT)
        self.assertTrue(any("platforms section is missing" in e for e in errors))

        data["platforms"] = []
        errors = gps.validate_matrix(data, REPO_ROOT)
        self.assertTrue(any("platforms section is missing" in e for e in errors))

    def test_missing_required_target_platforms_rejected(self):
        """Verify all 6 required target platforms must be present."""
        required = ["windows", "linux", "web", "android", "macos", "ios"]
        for target in required:
            data = self.get_clean_data()
            del data["platforms"][target]
            errors = gps.validate_matrix(data, REPO_ROOT)
            self.assertTrue(
                any(f"Missing required target platform: '{target}'" in e for e in errors),
                f"Validation failed to catch missing required platform '{target}'"
            )

    # -------------------------------------------------------------------------
    # 2. Platform-level Attribute Attacks
    # -------------------------------------------------------------------------
    def test_platform_not_dict_rejected(self):
        """Verify non-dict platform definition is rejected."""
        data = self.get_clean_data()
        data["platforms"]["windows"] = "invalid_string"
        errors = gps.validate_matrix(data, REPO_ROOT)
        self.assertTrue(any("Platform 'windows' must be a dictionary" in e for e in errors))

    def test_platform_missing_display_name_rejected(self):
        """Verify platform missing display_name is rejected."""
        data = self.get_clean_data()
        del data["platforms"]["linux"]["display_name"]
        errors = gps.validate_matrix(data, REPO_ROOT)
        self.assertTrue(any("Platform 'linux' missing display_name" in e for e in errors))

    def test_platform_invalid_tier_rejected(self):
        """Verify platform tier other than 1 or 2 is rejected."""
        data = self.get_clean_data()
        for bad_tier in [0, 3, 4, -1, "1", None]:
            data["platforms"]["windows"]["tier"] = bad_tier
            errors = gps.validate_matrix(data, REPO_ROOT)
            self.assertTrue(
                any("tier must be 1 or 2" in e for e in errors),
                f"Failed to catch invalid tier: {bad_tier}"
            )

    def test_platform_invalid_summary_status_rejected(self):
        """Verify invalid summary_status is rejected."""
        data = self.get_clean_data()
        invalid_statuses = ["almost-done", "basically done", "done", "complete", "green", "PASS", "ok"]
        for bad_status in invalid_statuses:
            data["platforms"]["windows"]["summary_status"] = bad_status
            errors = gps.validate_matrix(data, REPO_ROOT)
            self.assertTrue(
                any(f"invalid summary_status '{bad_status}'" in e for e in errors),
                f"Failed to catch invalid summary_status '{bad_status}'"
            )

    def test_platform_empty_capabilities_rejected(self):
        """Verify empty capabilities dictionary is rejected."""
        data = self.get_clean_data()
        data["platforms"]["web"]["capabilities"] = {}
        errors = gps.validate_matrix(data, REPO_ROOT)
        self.assertTrue(any("Platform 'web' has no capabilities defined" in e for e in errors))

        data["platforms"]["web"]["capabilities"] = None
        errors = gps.validate_matrix(data, REPO_ROOT)
        self.assertTrue(any("Platform 'web' has no capabilities defined" in e for e in errors))

    # -------------------------------------------------------------------------
    # 3. Capability Status & Evidence Integrity Attacks
    # -------------------------------------------------------------------------
    def test_capability_not_dict_rejected(self):
        """Verify non-dict capability definition is rejected."""
        data = self.get_clean_data()
        data["platforms"]["windows"]["capabilities"]["build"] = "verified"
        errors = gps.validate_matrix(data, REPO_ROOT)
        self.assertTrue(any("capability 'build' must be a dictionary" in e for e in errors))

    def test_capability_invalid_status_enum_rejected(self):
        """Verify capability with illegal status enum is rejected."""
        data = self.get_clean_data()
        invalid_enums = ["almost-done", "ready-ish", "complete", "finished", "passed", "true", "DONE"]
        for bad in invalid_enums:
            data["platforms"]["windows"]["capabilities"]["build"]["status"] = bad
            errors = gps.validate_matrix(data, REPO_ROOT)
            self.assertTrue(
                any(f"invalid status '{bad}'" in e for e in errors),
                f"Failed to catch invalid capability status '{bad}'"
            )

    def test_verified_missing_evidence_dict_rejected(self):
        """Verify verified capability missing evidence dict is rejected."""
        data = self.get_clean_data()
        data["platforms"]["windows"]["capabilities"]["build"]["status"] = "verified"
        del data["platforms"]["windows"]["capabilities"]["build"]["evidence"]
        errors = gps.validate_matrix(data, REPO_ROOT)
        self.assertTrue(
            any("is 'verified' but missing evidence dictionary" in e for e in errors)
        )

        data["platforms"]["windows"]["capabilities"]["build"]["evidence"] = {}
        errors = gps.validate_matrix(data, REPO_ROOT)
        self.assertTrue(
            any("is 'verified' but missing evidence dictionary" in e for e in errors)
        )

    def test_verified_invalid_commit_sha_rejected(self):
        """Verify malformed or invalid commit hashes are rejected."""
        data = self.get_clean_data()
        bad_commits = [
            "",                        # empty
            "   ",                     # whitespace
            "123456",                  # 6 chars (too short, min 7)
            "xyz12345",                # non-hex chars
            "62132e78ZZ",              # contains non-hex chars
            "a" * 41,                  # 41 chars (too long, max 40)
            "123456g",                 # 'g' is non-hex
            "commit-sha-here",         # placeholder string
        ]
        for bad_commit in bad_commits:
            data["platforms"]["windows"]["capabilities"]["build"]["evidence"]["commit"] = bad_commit
            errors = gps.validate_matrix(data, REPO_ROOT)
            self.assertTrue(
                any("evidence commit" in e and "is invalid" in e for e in errors),
                f"Failed to catch bad commit SHA: '{bad_commit}'"
            )

    def test_verified_valid_commit_sha_boundary_accepted(self):
        """Verify valid short (7-char) and full (40-char) hex SHAs pass."""
        data = self.get_clean_data()
        valid_commits = [
            "62132e7",                 # 7 hex chars (short git sha)
            "62132e78",                # 8 hex chars
            "62132e783dd238752659d4227ff26b0235258ea9",  # 40 hex chars (full git sha)
            "ABCDEF1",                 # 7 uppercase hex chars
            "0123456789abcdefABCDEF0123456789abcdef01",  # 40 mixed case hex chars
        ]
        for good_commit in valid_commits:
            data["platforms"]["windows"]["capabilities"]["build"]["evidence"]["commit"] = good_commit
            errors = gps.validate_matrix(data, REPO_ROOT)
            commit_errors = [e for e in errors if "evidence commit" in e]
            self.assertEqual(len(commit_errors), 0, f"Valid commit '{good_commit}' was rejected: {commit_errors}")

    def test_verified_empty_or_whitespace_document_path_rejected(self):
        """Verify empty or whitespace document path is rejected."""
        data = self.get_clean_data()
        for bad_doc in ["", "   "]:
            data["platforms"]["windows"]["capabilities"]["build"]["evidence"]["document"] = bad_doc
            errors = gps.validate_matrix(data, REPO_ROOT)
            self.assertTrue(
                any("evidence document path is empty" in e for e in errors),
                f"Failed to catch empty document path: '{bad_doc}'"
            )

    def test_verified_nonexistent_document_path_rejected(self):
        """Verify non-existent document path referenced in evidence is rejected."""
        data = self.get_clean_data()
        fake_docs = [
            "docs/fake_non_existent_report.md",
            "non_existent_dir/dummy.txt",
            "docs/plans/2099-01-01-imaginary-closure.md",
        ]
        for fake_doc in fake_docs:
            data["platforms"]["windows"]["capabilities"]["build"]["evidence"]["document"] = fake_doc
            errors = gps.validate_matrix(data, REPO_ROOT)
            self.assertTrue(
                any(f"referenced document does not exist: {fake_doc}" in e for e in errors),
                f"Failed to catch nonexistent document path: '{fake_doc}'"
            )

    def test_verified_empty_or_whitespace_test_command_rejected(self):
        """Verify empty or whitespace test command is rejected."""
        data = self.get_clean_data()
        for bad_cmd in ["", "   "]:
            data["platforms"]["windows"]["capabilities"]["build"]["evidence"]["test"] = bad_cmd
            errors = gps.validate_matrix(data, REPO_ROOT)
            self.assertTrue(
                any("evidence test command is empty" in e for e in errors),
                f"Failed to catch empty test command: '{bad_cmd}'"
            )

    def test_verified_empty_or_whitespace_verified_at_timestamp_rejected(self):
        """Verify empty or whitespace verified_at timestamp is rejected."""
        data = self.get_clean_data()
        for bad_time in ["", "   "]:
            data["platforms"]["windows"]["capabilities"]["build"]["evidence"]["verified_at"] = bad_time
            errors = gps.validate_matrix(data, REPO_ROOT)
            self.assertTrue(
                any("evidence verified_at timestamp is empty" in e for e in errors),
                f"Failed to catch empty verified_at: '{bad_time}'"
            )

    # -------------------------------------------------------------------------
    # 4. Iron Rule: iOS real_device Gating
    # -------------------------------------------------------------------------
    def test_ios_real_device_iron_rule_enforcement(self):
        """Verify iOS real_device CANNOT be set to verified, probe, or pending."""
        data = self.get_clean_data()
        illegal_statuses = [
            "verified",
            "probe",
            "pending",
            "credential-gated",
            "blocked",
            "not-applicable",
        ]
        for illegal in illegal_statuses:
            data["platforms"]["ios"]["capabilities"]["real_device"]["status"] = illegal
            if illegal == "verified":
                data["platforms"]["ios"]["capabilities"]["real_device"]["evidence"] = {
                    "commit": "8aa51c36",
                    "document": "README.md",
                    "test": "fake device run",
                    "verified_at": "2026-08-25T00:00:00Z"
                }
            errors = gps.validate_matrix(data, REPO_ROOT)
            self.assertTrue(
                any("Platform 'ios' capability 'real_device' must be 'hardware-gated'" in e for e in errors),
                f"Failed to reject illegal iOS real_device status '{illegal}'"
            )

    # -------------------------------------------------------------------------
    # 5. CLI Execution & Freshness Check (--check)
    # -------------------------------------------------------------------------
    def test_cli_check_success_on_unmodified_repo(self):
        """Verify the recorded snapshot matches its explicitly pinned evidence HEAD."""
        res = subprocess.run(
            [sys.executable, str(self.script_path), "--check", "--head", self.recorded_head],
            cwd=str(REPO_ROOT),
            capture_output=True,
            text=True,
        )
        self.assertEqual(res.returncode, 0, f"Expected returncode 0, got {res.returncode}. Stderr: {res.stderr}")
        self.assertIn("[OK] Platform status matrix is valid", res.stdout)

    def test_cli_check_detects_tampered_markdown(self):
        """Verify --check exits 1 when markdown output file content differs from YAML."""
        with tempfile.NamedTemporaryFile("w", delete=False, suffix=".md", encoding="utf-8") as tmp_md:
            tmp_md.write("# Tampered Markdown Content\n")
            tmp_md_path = Path(tmp_md.name)

        try:
            res = subprocess.run(
                [sys.executable, str(self.script_path), "--check", "--head", self.recorded_head,
                 "--output", str(tmp_md_path)],
                cwd=str(REPO_ROOT),
                capture_output=True,
                text=True,
            )
            self.assertEqual(res.returncode, 1, "Expected --check to fail on tampered markdown.")
            self.assertIn("is stale or modified", res.stderr)
        finally:
            if tmp_md_path.exists():
                tmp_md_path.unlink()

    def test_cli_check_rejects_mismatched_evidence_head(self) -> None:
        """Pinning snapshot checks must not disable the CLI's evidence drift guard."""
        other_prefix = "1" if self.recorded_head.startswith("0") else "0"
        mismatched_head = other_prefix + self.recorded_head[1:]
        res = subprocess.run(
            [sys.executable, str(self.script_path), "--check", "--head", mismatched_head],
            cwd=str(REPO_ROOT),
            capture_output=True,
            text=True,
        )
        self.assertEqual(res.returncode, 1, "A different evidence HEAD must be rejected.")
        self.assertIn("lags the code HEAD", res.stderr)
        self.assertIn(f"effective HEAD={mismatched_head} (source: cli)", res.stderr)

    def test_cli_check_detects_missing_output_file(self):
        """Verify --check exits 1 when output markdown does not exist."""
        fake_output = REPO_ROOT / "docs" / "status" / "non_existent_status_check_file.md"
        if fake_output.exists():
            fake_output.unlink()

        res = subprocess.run(
            [sys.executable, str(self.script_path), "--check", "--output", str(fake_output)],
            cwd=str(REPO_ROOT),
            capture_output=True,
            text=True,
        )
        self.assertEqual(res.returncode, 1, "Expected --check to fail when output file does not exist.")
        self.assertIn("does not exist", res.stderr)

    def test_cli_rejects_corrupted_yaml_matrix(self):
        """Verify CLI exits 1 and emits clear error messages when matrix is corrupted."""
        corrupted_yaml = """
version: 1
allowed_status_enums:
  - verified
  - probe
  - pending
  - hardware-gated
  - credential-gated
  - blocked
  - not-applicable
platforms:
  windows:
    display_name: "Windows"
    tier: 1
    summary_status: almost-done
    capabilities:
      build:
        status: almost-done
"""
        with tempfile.NamedTemporaryFile("w", delete=False, suffix=".yaml", encoding="utf-8") as tmp_y:
            tmp_y.write(corrupted_yaml)
            tmp_y_path = Path(tmp_y.name)

        try:
            res = subprocess.run(
                [sys.executable, str(self.script_path), "--matrix", str(tmp_y_path), "--dry-run"],
                cwd=str(REPO_ROOT),
                capture_output=True,
                text=True,
            )
            self.assertEqual(res.returncode, 1, "Expected CLI to fail with corrupted YAML matrix.")
            self.assertIn("Platform matrix validation failed", res.stderr)
            self.assertIn("invalid summary_status 'almost-done'", res.stderr)
            self.assertIn("Missing required target platform: 'linux'", res.stderr)
        finally:
            if tmp_y_path.exists():
                tmp_y_path.unlink()

    # -------------------------------------------------------------------------
    # 6. JSON Export & Dry Run
    # -------------------------------------------------------------------------
    def test_cli_json_export_file(self):
        """Verify --json-output writes valid JSON summary to file."""
        with tempfile.NamedTemporaryFile("w", delete=False, suffix=".json", encoding="utf-8") as tmp_j:
            tmp_j_path = Path(tmp_j.name)

        try:
            res = subprocess.run(
                [sys.executable, str(self.script_path), "--json-output", str(tmp_j_path), "--dry-run"],
                cwd=str(REPO_ROOT),
                capture_output=True,
                text=True,
            )
            self.assertEqual(res.returncode, 0, f"JSON file export failed: {res.stderr}")
            self.assertTrue(tmp_j_path.exists())
            parsed = json.loads(tmp_j_path.read_text(encoding="utf-8"))
            self.assertIn("platforms", parsed)
            self.assertEqual(parsed["platforms"]["android"]["summary_status"], "verified")
            self.assertEqual(parsed["platforms"]["ios"]["capabilities"]["real_device"]["status"], "hardware-gated")
        finally:
            if tmp_j_path.exists():
                tmp_j_path.unlink()

    # -------------------------------------------------------------------------
    # 7. Production Repository Invariant Assertions
    # -------------------------------------------------------------------------
    def test_production_matrix_passes_validation_zero_errors(self):
        """Verify production docs/status/platform-matrix.yaml passes with 0 errors."""
        errors = gps.validate_matrix(self.valid_data, REPO_ROOT)
        self.assertEqual(
            errors, [],
            f"Production docs/status/platform-matrix.yaml has validation errors: {errors}"
        )

    def test_production_all_referenced_documents_exist_on_disk(self):
        """Verify every evidence document path referenced across all platforms exists on disk."""
        platforms = self.valid_data.get("platforms", {})
        checked_docs = []
        for plat_id, plat in platforms.items():
            for cap_id, cap in plat.get("capabilities", {}).items():
                ev = cap.get("evidence")
                if ev and ev.get("document"):
                    doc_rel = ev.get("document")
                    doc_abs = REPO_ROOT / doc_rel
                    self.assertTrue(
                        doc_abs.exists(),
                        f"Referenced document '{doc_rel}' in platform '{plat_id}' capability '{cap_id}' does not exist on disk!"
                    )
                    checked_docs.append(doc_rel)
        self.assertTrue(len(checked_docs) >= 15, f"Expected at least 15 evidence documents, got {len(checked_docs)}")

    def test_production_markdown_is_strictly_in_sync(self):
        """Verify recorded markdown content matches the YAML at its evidence HEAD."""
        generated = gps.generate_markdown(self.valid_data, head_commit=self.recorded_head)
        self.assertTrue(self.status_md_path.exists(), "docs/status/platform-status.md must exist")
        actual = self.status_md_path.read_text(encoding="utf-8")
        # t196: Generated At is generator-execution time, so both sides are
        # normalized through the SAME helper the generator's --check uses
        # (scripts/generate_platform_status.py::normalize_freshness) -- a strict
        # byte compare would otherwise be red every run (timestamp always differs).
        self.assertEqual(
            gps.normalize_freshness(actual),
            gps.normalize_freshness(generated),
            "docs/status/platform-status.md is out of sync with generator output!"
        )

    # -------------------------------------------------------------------------
    # 8. Uncovered Edge Case Vulnerability Demonstrations
    # -------------------------------------------------------------------------
    def test_vulnerability_str_none_bypass_on_test_field(self):
        """
        Adversarial Observation:
        When a verified capability has test: null (None in Python),
        `str(evidence.get('test', ''))` becomes "None", which evaluates to non-empty.
        This allows null test commands to bypass validation.
        """
        data = self.get_clean_data()
        data["platforms"]["windows"]["capabilities"]["build"]["evidence"]["test"] = None
        errors = gps.validate_matrix(data, REPO_ROOT)
        # Demonstrating empirical finding:
        is_bypassed = not any("evidence test command is empty" in e for e in errors)
        self.assertTrue(
            is_bypassed,
            "Empirical vulnerability confirmed: test: None bypassed empty test command check."
        )

    def test_vulnerability_str_none_bypass_on_test_field(self):
        """
        Verify that test: null / None is caught and rejected as empty.
        """
        data = self.get_clean_data()
        data["platforms"]["windows"]["capabilities"]["build"]["evidence"]["test"] = None
        errors = gps.validate_matrix(data, REPO_ROOT)
        self.assertTrue(
            any("evidence test command is empty" in e for e in errors),
            "Null/None test command must be rejected with an error."
        )

    def test_vulnerability_str_none_bypass_on_verified_at_field(self):
        """
        Verify that verified_at: null / None is caught and rejected as empty.
        """
        data = self.get_clean_data()
        data["platforms"]["windows"]["capabilities"]["build"]["evidence"]["verified_at"] = None
        errors = gps.validate_matrix(data, REPO_ROOT)
        self.assertTrue(
            any("evidence verified_at timestamp is empty" in e for e in errors),
            "Null/None verified_at timestamp must be rejected with an error."
        )

    def test_vulnerability_probe_evidence_unchecked(self):
        """
        Verify that probe capabilities with non-existent evidence documents are rejected.
        """
        data = self.get_clean_data()
        data["platforms"]["macos"]["capabilities"]["build"]["evidence"] = {
            "commit": "6846796d",
            "document": "fake/non_existent_file.md",
            "test": "imaginary test",
            "verified_at": "2026-08-25T00:00:00Z"
        }
        errors = gps.validate_matrix(data, REPO_ROOT)
        self.assertTrue(
            any("referenced document does not exist" in e for e in errors),
            "Probe evidence pointing to missing documents must be caught."
        )


class TestPublishedEvidenceClaims(unittest.TestCase):
    def data(self):
        return gps.load_yaml(REPO_ROOT / 'docs/status/platform-matrix.yaml')

    def render(self, data):
        return gps.generate_markdown(data, head_commit=data['evidence_head_commit'])

    def test_document_references_are_not_execution_proof(self):
        md = self.render(self.data())
        self.assertNotIn('100% Evidence-Backed', md)
        self.assertIn('NOT_REVERIFIED', md)

    def test_footer_does_not_invent_fixed_test_totals(self):
        md = self.render(self.data()).split('## 4. Release Candidate Gate & Blockers', 1)[1]
        for stale in ('11/11 CTest', '368 tests / 27 files'):
            with self.subTest(stale=stale):
                self.assertNotIn(stale, md)

    def test_pending_capabilities_cannot_create_checked_pass(self):
        data = self.data()
        for platform in data['platforms'].values():
            platform['summary_status'] = 'pending'
            for capability in platform['capabilities'].values():
                capability['status'] = 'pending'
                capability.pop('evidence', None)
        data['platforms']['ios']['capabilities']['real_device']['status'] = 'hardware-gated'
        self.assertEqual([], gps.validate_matrix(data, REPO_ROOT))
        footer = self.render(data).split('## 4. Release Candidate Gate & Blockers', 1)[1]
        self.assertNotIn('- [x]', footer)
        self.assertNotIn('E2E verified', footer)
        self.assertIn('`pending`', footer)
        self.assertIn('`hardware-gated`', footer)

    def test_json_separates_recorded_status_from_execution_verification(self):
        summary = json.loads(gps.export_json(self.data()))
        self.assertEqual('NOT_REVERIFIED', summary['recorded_evidence_validation'])
        self.assertFalse(summary['release_ready'])
        self.assertEqual('NOT_RUN', summary['current_evidence']['status'])


class TestEvidenceCliOutputOwnership(unittest.TestCase):
    """Actual CLI against byte fixtures; no claimed engine or hosted execution."""
    def setUp(self):
        import hashlib
        import test_package_bundle as fixtures
        self.fixtures = fixtures
        self.f = fixtures.PackageBundleTests()
        self.f.setUp()
        self.addCleanup(self.f.doCleanups)
        self.selection = self.f.base / 'selection.json'
        claim = dict(id='package-bytes', kind='package_receipt', platform='windows',
                     configuration='Release', bundle='download', manifest_sha256=self.f.manifest_sha,
                     version=fixtures.VERSION, producer=fixtures.PRODUCER,
                     expected_files={self.f.name:dict(kind='file', sha256=self.f.final_sha)})
        self.selection.write_text(json.dumps(dict(schema='caesura.platform-evidence-selection.v1',
                                  source_sha=fixtures.SOURCE, claims=[claim])), encoding='utf-8')
        self.original_selection = self.selection.read_bytes()
        self.report = self.f.base / 'report.json'
        self.output = self.f.base / 'matrix.md'
        self.arguments = [sys.executable, '-X', 'utf8', '-B', str(REPO_ROOT / 'scripts/generate_platform_status.py'),
                          '--evidence-selection', str(self.selection), '--evidence-selection-sha256',
                          hashlib.sha256(self.original_selection).hexdigest(), '--candidate-source', fixtures.SOURCE,
                          '--evidence-root', str(self.f.base), '--evidence-profile',
                          str(REPO_ROOT / 'scripts/validation_profiles.json')]

    def cli(self, *, report=None, output=None, extra=()):
        return subprocess.run(self.arguments + ['--evidence-report', str(report or self.report),
                              '--output', str(output or self.output)] + list(extra),
                              cwd=REPO_ROOT, capture_output=True, encoding='utf-8', timeout=30)

    def test_report_cannot_alias_markdown_output(self):
        result = self.cli(output=self.report)
        self.assertNotEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertFalse(self.report.exists())

    def test_report_cannot_alias_json_output_after_normalization(self):
        alias = self.report.parent / '.' / self.report.name
        result = self.cli(extra=['--json-output', str(alias)])
        self.assertNotEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertFalse(self.report.exists())
        self.assertFalse(self.output.exists())

    def test_existing_output_is_preserved(self):
        self.output.write_bytes(b'original accepted output\n')
        result = self.cli()
        self.assertNotEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertEqual(b'original accepted output\n', self.output.read_bytes())
        self.assertFalse(self.report.exists())

    def test_output_cannot_overwrite_selected_input(self):
        result = self.cli(output=self.selection)
        self.assertNotEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertEqual(self.original_selection, self.selection.read_bytes())
        self.assertFalse(self.report.exists())

    def test_distinct_outputs_preserve_report_and_narrow_scope(self):
        result = self.cli()
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        report = json.loads(self.report.read_text(encoding='utf-8'))
        self.assertEqual('caesura.platform-evidence-verification.v1', report['schema'])
        self.assertFalse(report['release_ready'])
        self.assertEqual('NOT_PERFORMED', report['hosted_authentication'])
        md = self.output.read_text(encoding='utf-8')
        self.assertIn('FINAL_PACKAGE_BYTES_AND_RECORDED_RECEIPTS', md)
        self.assertIn('RAW_STAGE_LOGS_NOT_INCLUDED_NOT_REPLAYED', md)

    def test_wrong_source_preserves_failure_report_without_markdown(self):
        index = self.arguments.index('--candidate-source') + 1
        self.arguments[index] = 'a' * 40
        result = self.cli()
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)
        report = json.loads(self.report.read_text(encoding='utf-8'))
        self.assertEqual('FAIL', report['status'])
        self.assertFalse(report['current_verified'])
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
