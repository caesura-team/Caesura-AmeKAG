"""Downloaded U1 boundary fixtures; these tests do not authenticate hosted jobs."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from test_validation_evidence import EvidenceFixture
import verify_execution_bundle as bundle


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class ExecutionBundleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="caesura-u23-execution-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.fixture = EvidenceFixture(self.root)
        self.fixture.collect()
        self.args = dict(
            manifest_sha256=sha(self.fixture.output / "manifest.json"),
            receipt_sha256=sha(self.fixture.run_path),
            profile_path=self.fixture.profile_path,
            profile_sha256=sha(self.fixture.profile_path),
            profile_name="test-debug", source_sha="a" * 40,
            expected_context={key:self.fixture.run[key] for key in
                ("run_id", "run_attempt", "repository", "workflow", "platform", "configuration")},
            trusted_dir=self.root / "trusted",
        )

    def verify(self, **changes):
        return bundle.verify_execution_bundle(self.fixture.output, **(self.args | changes))

    def test_delegates_to_strict_u1_using_locked_external_snapshots(self):
        # Only this orchestration positive substitutes U1's verdict. Real
        # fixture evidence remains rejected by the unmocked validator below.
        with patch.object(bundle, "verify_evidence", return_value=[]) as verify:
            result = self.verify()
        self.assertEqual(result["status"], "EXECUTION_BUNDLE_VERIFIED")
        self.assertFalse(result["release_ready"])
        args, kwargs = verify.call_args
        self.assertEqual(args[0], self.fixture.output)
        self.assertFalse(args[1].is_relative_to(self.fixture.output))
        self.assertFalse(args[3].is_relative_to(self.fixture.output))
        self.assertEqual(args[1].read_bytes(), self.fixture.profile_path.read_bytes())
        self.assertEqual(args[3].read_bytes(), self.fixture.run_path.read_bytes())
        self.assertEqual(kwargs, {"source_sha":"a" * 40, "release":True})
        bundle.verify_execution_bundle_stable(result)

    def test_test_fixture_is_rejected_by_real_u1(self):
        with self.assertRaisesRegex(ValueError, "test-fixture"):
            self.verify()

    def test_receipt_digest_cannot_be_selected_by_bundle(self):
        with self.assertRaisesRegex(ValueError, "receipt.*digest"):
            self.verify(receipt_sha256="0" * 64)

    def test_manifest_digest_is_externally_locked(self):
        with self.assertRaisesRegex(ValueError, "manifest.*digest"):
            self.verify(manifest_sha256="0" * 64)

    def test_external_profile_digest_is_checked(self):
        with self.assertRaisesRegex(ValueError, "profile.*digest"):
            self.verify(profile_sha256="0" * 64)

    def test_context_and_source_must_match_trusted_job_outputs(self):
        for key in self.args["expected_context"]:
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "context"):
                context = self.args["expected_context"] | {key:2 if key == "run_attempt" else "wrong"}
                self.verify(expected_context=context)
        with self.assertRaisesRegex(ValueError, "source"):
            self.verify(source_sha="b" * 40)

    def test_boolean_attempt_is_not_integer_one(self):
        with self.assertRaisesRegex(ValueError, "context"):
            self.verify(expected_context=self.args["expected_context"] | {"run_attempt":True})

    def test_cannot_weaken_context_or_trusted_profile(self):
        with self.assertRaisesRegex(ValueError, "context"):
            self.verify(expected_context={})
        with self.assertRaisesRegex(ValueError, "profile.*outside"):
            self.verify(profile_path=self.fixture.output / "profile.json")

    def test_cannot_copy_receipt_into_the_bundle(self):
        with self.assertRaisesRegex(ValueError, "outside"):
            self.verify(trusted_dir=self.fixture.output / "trusted")

    def test_existing_trusted_attempt_is_never_overwritten(self):
        self.args["trusted_dir"].mkdir()
        marker = self.args["trusted_dir"] / "prior-failure.json"
        marker.write_text("original", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "exist"):
            self.verify()
        self.assertEqual(marker.read_text(encoding="utf-8"), "original")

    def test_json_duplicate_keys_rejected_even_when_digest_matches(self):
        path = self.fixture.output / "execution-receipt.json"
        raw = path.read_bytes().rstrip()[:-1] + b', "purpose":"validation"}'
        path.write_bytes(raw)
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            self.verify(receipt_sha256=sha(path))

    def test_failure_in_original_report_is_not_replaced_by_manifest_pass(self):
        self.fixture.run["checks"][0]["stdout"] = self.fixture.file("cpp.stdout", self.fixture.doctest(1, 1))
        self.fixture.output = self.root / "failed" / self.fixture.run["source_sha"] / self.fixture.run["run_id"] / "test-debug"
        self.fixture.collect()
        self.args.update(manifest_sha256=sha(self.fixture.output / "manifest.json"), receipt_sha256=sha(self.fixture.run_path))
        with self.assertRaisesRegex(ValueError, "Tests failed"):
            self.verify()

    def test_changed_download_after_validation_is_rejected(self):
        with patch.object(bundle, "verify_evidence", return_value=[]):
            result = self.verify()
        log = self.fixture.output / "inputs/cpp/stdout"
        log.write_text("replacement", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "changed"):
            bundle.verify_execution_bundle_stable(result)

    def test_changed_trusted_snapshot_is_rejected(self):
        with patch.object(bundle, "verify_evidence", return_value=[]):
            result = self.verify()
        (self.args["trusted_dir"] / "execution-receipt.json").write_text("{}", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "changed"):
            bundle.verify_execution_bundle_stable(result)

    def test_mutation_during_u1_is_rejected(self):
        def mutate(*args, **kwargs):
            (self.fixture.output / "inputs/cpp/stdout").write_text("changed", encoding="utf-8")
            return []
        with patch.object(bundle, "verify_evidence", side_effect=mutate):
            with self.assertRaisesRegex(ValueError, "changed"):
                self.verify()


if __name__ == "__main__":
    unittest.main(verbosity=2)
