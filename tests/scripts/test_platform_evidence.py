"""Platform evidence adapters; fabricated files never become runtime execution claims."""
from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from test_validation_evidence import EvidenceFixture
import test_package_bundle as package_fixture
from test_package_bundle import PRODUCER, SOURCE, VERSION

MODULE = ROOT / "scripts/platform_evidence.py"
adapter = None
if MODULE.is_file():
    spec = importlib.util.spec_from_file_location("platform_evidence", MODULE)
    adapter = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(adapter)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class PlatformEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(adapter, "Production platform evidence adapter is required")
        temporary = tempfile.TemporaryDirectory(prefix="caesura-platform-evidence-")
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name).resolve()
        self.f = EvidenceFixture(self.base)
        self.f.run["toolchain"]["cmake_cache"] = {"CAESURA_LIVE2D": "OFF"}
        self.manifest = self.f.collect()
        self.claim = dict(id="test-execution", kind="execution", platform="test", configuration="Debug",
            bundle=self.relative(self.f.output), manifest_sha256=sha(self.f.output / "manifest.json"),
            expected_run=self.relative(self.f.run_path), receipt_sha256=sha(self.f.run_path),
            profile_name="test-debug", profile_sha256=sha(self.f.profile_path),
            context={key:self.f.run[key] for key in ("run_id", "run_attempt", "repository", "workflow")},
            expected_cache={"CAESURA_LIVE2D":"OFF"})
        self.selection = dict(schema="caesura.platform-evidence-selection.v1",
            source_sha=self.f.run["source_sha"], claims=[self.claim])
        self.selection_path = self.base / "selection.json"

    def relative(self, path):
        return Path(path).relative_to(self.base).as_posix()

    def save(self):
        self.selection_path.write_text(json.dumps(self.selection), encoding="utf-8")

    def verify(self, *, diagnostic=True, **changes):
        self.save()
        args = dict(selection_sha256=sha(self.selection_path), expected_source_sha=self.f.run["source_sha"],
            profile_path=self.f.profile_path, evidence_root=self.base, diagnostic=diagnostic)
        args.update(changes)
        return adapter.verify_current_evidence(self.selection_path, **args)

    def failed(self, report, needle=None):
        self.assertEqual(report["status"], "FAIL", report)
        self.assertFalse(report["current_verified"])
        self.assertFalse(report["release_ready"])
        self.assertTrue(report["errors"])
        if needle is not None:
            self.assertIn(needle.lower(), str(report["errors"]).lower())

    def log(self):
        return self.f.output / self.manifest["checks"][0]["files"]["stdout"]["path"]

    def test_diagnostic_fixture_reparses_counts_without_current_or_release_claim(self):
        report = self.verify()
        self.assertEqual(report["status"], "DIAGNOSTIC_EVIDENCE_VERIFIED", report)
        self.assertFalse(report["current_verified"])
        self.assertFalse(report["release_ready"])
        claim = report["claims"][self.claim["id"]]
        self.assertEqual(claim["checks"][0]["counts"]["passed"], 2)
        self.assertEqual(claim["raw_stage_evidence"], "ORIGINAL_REPORTS_REPARSED")
        self.assertEqual(report["hosted_authentication"], "NOT_PERFORMED")

    def test_fixture_cannot_be_promoted_by_strict_caller(self):
        self.failed(self.verify(diagnostic=False), "test-fixture")

    def test_empty_required_claim_set_cannot_pass(self):
        self.selection["claims"] = []
        self.failed(self.verify(), "nonempty")

    def test_duplicate_claim_ids_cannot_overwrite_required_result(self):
        self.selection["claims"].append(copy.deepcopy(self.claim))
        self.failed(self.verify(), "duplicate")

    def test_unknown_claim_kind_fails_instead_of_receipt_only_fallback(self):
        self.claim["kind"] = "package_runtime"
        self.failed(self.verify(), "unsupported")

    def test_extra_root_or_claim_fields_are_rejected(self):
        for target in (self.selection, self.claim):
            with self.subTest(target="root" if target is self.selection else "claim"):
                target["result"] = "PASS"
                self.failed(self.verify(), "fields")
                del target["result"]

    def test_wrong_selection_digest_is_rejected_before_claim_processing(self):
        self.failed(self.verify(selection_sha256="0" * 64), "selection")

    def test_old_source_selection_cannot_replace_external_current_sha(self):
        self.failed(self.verify(expected_source_sha="e" * 40), "source")

    def test_receipt_source_cannot_differ_from_external_selection(self):
        self.selection["source_sha"] = "e" * 40
        self.failed(self.verify(expected_source_sha="e" * 40), "source")

    def test_full_lowercase_source_sha_is_required(self):
        for value in ("a" * 8, "A" * 40, None):
            with self.subTest(value=value):
                self.failed(self.verify(expected_source_sha=value), "source")

    def test_missing_original_log_fails_even_with_pass_manifest(self):
        self.log().unlink()
        self.failed(self.verify(), "file")

    def test_replaced_log_bytes_fail(self):
        self.log().write_text("PASS\n", encoding="utf-8")
        self.failed(self.verify(), "digest")

    def test_resealed_manifest_cannot_override_original_report_counts(self):
        path = self.f.output / "manifest.json"
        value = copy.deepcopy(self.manifest)
        value["checks"][0]["counts"]["passed"] = 9000
        path.write_text(json.dumps(value), encoding="utf-8")
        self.claim["manifest_sha256"] = sha(path)
        self.failed(self.verify(), "manifest")

    def test_wrong_manifest_receipt_or_profile_digest_fails(self):
        for key in ("manifest_sha256", "receipt_sha256", "profile_sha256"):
            with self.subTest(key=key):
                previous = self.claim[key]
                self.claim[key] = "0" * 64
                self.failed(self.verify(), "digest")
                self.claim[key] = previous

    def test_platform_and_configuration_are_independently_bound(self):
        for key, value in (("platform", "linux"), ("configuration", "Release")):
            with self.subTest(key=key):
                previous = self.claim[key]
                self.claim[key] = value
                self.failed(self.verify(), key)
                self.claim[key] = previous

    def test_run_workflow_attempt_and_repository_are_independently_bound(self):
        for key, value in (("run_id", "other-run"), ("run_attempt", 2),
                           ("repository", "other/repo"), ("workflow", "other-workflow")):
            with self.subTest(key=key):
                previous = self.claim["context"][key]
                self.claim["context"][key] = value
                self.failed(self.verify(), key)
                self.claim["context"][key] = previous

    def test_sdk_off_or_missing_cache_value_does_not_prove_sdk_on(self):
        for expected in ({"CAESURA_LIVE2D":"ON"}, {"CAESURA_HAS_STEAM":"ON"}):
            with self.subTest(expected=expected):
                self.claim["expected_cache"] = expected
                self.failed(self.verify(), "cache")

    def test_external_receipt_cannot_be_selected_from_collected_bundle(self):
        path = self.f.output / "execution-receipt.json"
        self.claim["expected_run"] = self.relative(path)
        self.failed(self.verify(), "outside")

    def test_trusted_profile_cannot_be_selected_from_collected_bundle(self):
        self.failed(self.verify(profile_path=self.f.output / "profile.json"), "outside")

    def test_absolute_and_parent_escaping_paths_are_rejected(self):
        for value in (str(self.f.output), "../escape", "raw/../run.json", "C:/outside", "raw\\run.json"):
            with self.subTest(value=value):
                self.claim["expected_run"] = value
                self.failed(self.verify(), "path")

    def test_missing_bundle_or_receipt_fails(self):
        for key in ("bundle", "expected_run"):
            with self.subTest(key=key):
                old = self.claim[key]
                self.claim[key] = "missing"
                self.failed(self.verify())
                self.claim[key] = old

    def test_duplicate_json_keys_are_rejected(self):
        self.save()
        raw = self.selection_path.read_text(encoding="utf-8")
        self.selection_path.write_text(raw.replace('"claims":', '"claims": [], "claims":'), encoding="utf-8")
        report = adapter.verify_current_evidence(self.selection_path, selection_sha256=sha(self.selection_path),
            expected_source_sha=self.f.run["source_sha"], profile_path=self.f.profile_path,
            evidence_root=self.base, diagnostic=True)
        self.failed(report, "duplicate")

    def test_corrupt_profile_required_set_is_not_accepted(self):
        self.f.profile["profiles"]["test-debug"]["checks"][0]["required"] = False
        self.f.write_profile()
        self.failed(self.verify(), "digest")

    def test_late_input_change_is_detected_before_success(self):
        original = adapter._execution
        def change_after_real_verification(*args, **kwargs):
            result = original(*args, **kwargs)
            self.log().write_text("changed after verification\n", encoding="utf-8")
            return result
        with patch.object(adapter, "_execution", side_effect=change_after_real_verification):
            report = self.verify()
            self.failed(report, "changed")
            self.assertEqual(report["claims"][self.claim["id"]]["result"], "FAIL")

    def test_late_selection_change_is_detected(self):
        original = adapter._execution
        def change_after_real_verification(*args, **kwargs):
            result = original(*args, **kwargs)
            self.selection_path.write_text("{}", encoding="utf-8")
            return result
        with patch.object(adapter, "_execution", side_effect=change_after_real_verification):
            self.failed(self.verify(), "changed")

    def test_hardlinked_external_receipt_is_refused(self):
        import os
        os.link(self.f.run_path, self.base / "receipt-alias.json")
        self.failed(self.verify(), "hardlink")

    def test_nonboolean_diagnostic_cannot_disable_strict_verification(self):
        self.failed(self.verify(diagnostic="false"), "boolean")

    def test_nonzero_original_exit_and_missing_required_checks_propagate_failure(self):
        from collect_validation_evidence import build_manifest
        for checks in ([dict(self.f.run["checks"][0], exit_code=7)], []):
            with self.subTest(checks=len(checks)):
                self.f.run["checks"] = checks
                self.f.write_run()
                (self.f.output / "execution-receipt.json").write_bytes(self.f.run_path.read_bytes())
                manifest, _ = build_manifest(self.f.profile_path, "test-debug", self.f.run_path,
                                             collected_root=self.f.output)
                path = self.f.output / "manifest.json"
                path.write_text(json.dumps(manifest), encoding="utf-8")
                self.claim.update(receipt_sha256=sha(self.f.run_path), manifest_sha256=sha(path))
                self.failed(self.verify(), "required")

    def package(self):
        self.p = package_fixture.PackageBundleTests()
        self.p.setUp()
        self.addCleanup(self.p.doCleanups)
        # The selected evidence root can contain copies of portable bundles;
        # no producer-local receipt path is followed.
        import shutil
        directory = self.base / "package"
        shutil.copytree(self.p.root, directory)
        claim = dict(id="windows-final-package", kind="package_receipt", platform="windows",
            configuration="Release", bundle="package", manifest_sha256=self.p.manifest_sha,
            version=VERSION, producer=copy.deepcopy(PRODUCER),
            expected_files={self.p.name:dict(kind="file", sha256=self.p.final_sha)})
        self.selection.update(source_sha=SOURCE, claims=[claim])
        self.claim = claim
        return directory

    def test_package_bytes_and_receipt_verification_never_claims_raw_log_replay(self):
        self.package()
        report = self.verify(diagnostic=False, expected_source_sha=SOURCE)
        self.assertEqual(report["status"], "CURRENT_EVIDENCE_VERIFIED", report)
        self.assertTrue(report["current_verified"])
        claim = report["claims"][self.claim["id"]]
        self.assertEqual(claim["raw_stage_evidence"], "RAW_STAGE_LOGS_NOT_INCLUDED_NOT_REPLAYED")
        self.assertEqual(claim["verification_scope"], "FINAL_PACKAGE_BYTES_AND_RECORDED_RECEIPTS")
        self.assertFalse(report["release_ready"])
        self.assertEqual(report["hosted_authentication"], "NOT_PERFORMED")

    def test_wrong_package_digest_and_replaced_package_fail(self):
        directory = self.package()
        self.claim["expected_files"][self.p.name]["sha256"] = "0" * 64
        self.failed(self.verify(expected_source_sha=SOURCE), "digest")
        self.claim["expected_files"][self.p.name]["sha256"] = self.p.final_sha
        (directory / self.p.name).write_bytes(b"different final bytes")
        self.failed(self.verify(expected_source_sha=SOURCE), "digest")

    def test_package_platform_source_and_producer_mismatch_fail(self):
        self.package()
        self.claim["platform"] = "linux"
        self.failed(self.verify(expected_source_sha=SOURCE), "context")
        self.claim["platform"] = "windows"
        self.claim["producer"]["run_attempt"] += 1
        self.failed(self.verify(expected_source_sha=SOURCE), "producer")

    def test_missing_package_receipt_fails(self):
        directory = self.package()
        (directory / "receipt-final.json").unlink()
        self.failed(self.verify(expected_source_sha=SOURCE), "missing")

    def test_package_kind_cannot_silently_accept_runtime_requirement(self):
        self.package()
        self.claim["kind"] = "package_runtime"
        self.failed(self.verify(expected_source_sha=SOURCE), "unsupported")

    def test_one_successful_claim_cannot_hide_another_missing_input(self):
        other = copy.deepcopy(self.claim)
        other.update(id="missing-execution", bundle="missing")
        self.selection["claims"].append(other)
        report = self.verify()
        self.failed(report)
        self.assertEqual(report["claims"]["test-execution"]["result"], "PASS")
        self.assertEqual(report["claims"]["missing-execution"]["result"], "FAIL")

    def test_inventory_rejection_returns_failure_report(self):
        import package_verification
        # Exercise the actual scanner and its typed rejection without a huge
        # fixture tree. The production limit is restored on leaving the scope.
        with patch.object(package_verification, "MAX_ENTRIES", 1):
            self.failed(self.verify(), "limits")

    def test_inventory_rejection_at_consumption_retires_prior_pass(self):
        import package_verification
        original = adapter._execution
        def lower_limit_after_real_verification(*args, **kwargs):
            result = original(*args, **kwargs)
            self.assertEqual(result['result'], 'PASS')
            package_verification.MAX_BYTES = 1
            return result
        with patch.object(package_verification, "MAX_BYTES", package_verification.MAX_BYTES):
            with patch.object(adapter, "_execution", side_effect=lower_limit_after_real_verification):
                report = self.verify()
                self.failed(report, "limits")
                self.assertEqual(report['claims'][self.claim['id']]['result'], 'FAIL')
                self.assertIn('Input stability recheck failed', str(report['claims'][self.claim['id']]['errors']))


if __name__ == "__main__":
    unittest.main()
