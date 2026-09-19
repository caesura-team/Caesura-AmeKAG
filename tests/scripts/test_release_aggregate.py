"""Aggregate orchestration fixtures; no hosted run or publication is simulated as real."""
from __future__ import annotations

import hashlib
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import test_package_bundle as package_fixture
from test_package_bundle import SOURCE, VERSION, PRODUCER
from test_validation_evidence import EvidenceFixture
import aggregate_release_inputs as aggregate
import verify_execution_bundle as execution_bundle
from package_verification import PackageVerificationError


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class AggregateTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="caesura-aggregate-fixture-")
        self.addCleanup(temp.cleanup)
        self.base = Path(temp.name).resolve()
        self.repo = self.base / "repo"; self.repo.mkdir(); (self.repo / ".git").mkdir()
        (self.repo / "scripts").mkdir()
        (self.repo / "CMakeLists.txt").write_text(f"project(CaesuraAmeKAG VERSION {VERSION} LANGUAGES C CXX)\n")
        self.p = package_fixture.PackageBundleTests(); self.p.setUp(); self.addCleanup(self.p.doCleanups)
        self.p.receipt["source_before"]["source_sha"] = SOURCE
        (self.base / "execution").mkdir()
        self.e = EvidenceFixture(self.base / "execution")
        self.e.run.update(source_sha=SOURCE, repository=PRODUCER["repository"],
            workflow=PRODUCER["workflow_ref"], run_attempt=PRODUCER["run_attempt"])
        self.e.output = self.e.root / "validation" / SOURCE / self.e.run["run_id"] / "test-debug"
        self.e.collect()
        (self.repo / "scripts/validation_profiles.json").write_bytes(self.e.profile_path.read_bytes())
        self.policy = {"schema_version":1, "version":VERSION,
            "required_jobs":{"native":"Validate / Native"},
            "artifact_roles":{"package":"native", "execution":"native"},
            "source_files":{name:sha(self.repo / name) for name in ("CMakeLists.txt", "scripts/validation_profiles.json")},
            "package_inputs":{"package":{"platform":"windows", "configuration":"Release",
                "required_files":self.p.required, "job_key":PRODUCER["job_key"]}},
            "execution_inputs":{"execution":{"profile_name":"test-debug", "platform":"test", "configuration":"Debug"}}}
        self.policy_path = self.repo / "policy.json"; self.write_policy()
        self.archives = {}
        for role, directory in (("package",self.p.root),("execution",self.e.output)):
            archive = self.base / (role + ".zip")
            with zipfile.ZipFile(archive,"w") as stream:
                for path in directory.rglob("*"):
                    if path.is_file(): stream.write(path,path.relative_to(directory).as_posix())
            self.archives[role] = archive
        self.hosted = {"schema_version":1,"kind":"caesura.hosted-inputs.v1","status":"HOSTED_INPUTS_VERIFIED",
            "transport":"fixture", "errors":[], "policy":{key:copy.deepcopy(self.policy[key]) for key in
                ("schema_version","required_jobs","artifact_roles")},
            "expected":{"source_sha":SOURCE,"repository":PRODUCER["repository"],
                "repository_id":PRODUCER["repository_id"],"run_id":PRODUCER["run_id"],
                "run_attempt":PRODUCER["run_attempt"],"workflow_ref":PRODUCER["workflow_ref"],
                "workflow_sha":PRODUCER["workflow_sha"], "workflow_path":".github/workflows/ci.yml",
                "called_workflow_sha":PRODUCER["workflow_sha"]},
            "artifacts":{role:{"artifact_id":100+i,"artifact_digest":"sha256:"+sha(path),
                "manifest_sha256":sha(self.p.root/"upload-manifest.json") if role=="package" else sha(self.e.output/"manifest.json")}
                for i,(role,path) in enumerate(self.archives.items())}}
        self.claims = {"execution":{"receipt_sha256":sha(self.e.run_path),"run_id":self.e.run["run_id"]}}
        self.identity = {"source_sha":SOURCE,"dirty":False,"worktree_fingerprint":"c"*64}
        self.counter=0

    def write_policy(self):
        self.policy_path.write_text(json.dumps(self.policy),encoding="utf-8")

    def call(self, *, fake_u1=True, **changes):
        self.counter += 1
        args=dict(hosted=self.hosted,policy_path=self.policy_path,policy_sha256=sha(self.policy_path),
            archives=self.archives,execution_claims=self.claims,repo_root=self.repo,
            work_dir=self.base/("attempt-"+str(self.counter)),release_tag="v"+VERSION)
        args.update(changes)
        with patch.object(aggregate,"_source_identity",return_value=self.identity):
            if fake_u1:
                # Only U1's verdict is a seam; transport, context, copies and
                # the execution adapter's four locks all remain real.
                with patch.object(execution_bundle,"verify_evidence",return_value=[]):
                    return aggregate.verify_downloaded_inputs(**args)
            return aggregate.verify_downloaded_inputs(**args)

    def test_exact_artifacts_assemble_fixture_only_result_and_explicit_upload_list(self):
        result=self.call()
        self.assertEqual(result["status"],"INPUTS_VERIFIED")
        self.assertFalse(result["release_ready"])
        self.assertEqual(result["transport"],"fixture")
        self.assertEqual([item["name"] for item in result["upload_files"]],[self.p.name])
        self.assertEqual(result["packages"]["package"]["status"],"BUNDLE_VERIFIED")
        self.assertEqual(len(result["executions"]["execution"]["locks"]),4)

    def test_policy_digest_and_authenticated_required_set_cannot_change(self):
        with self.assertRaisesRegex(ValueError,"policy.*digest"):
            self.call(policy_sha256="0"*64)
        self.policy["required_jobs"]["other"]="Validate / Other";self.write_policy()
        with self.assertRaisesRegex(ValueError,"policy"):
            self.call()

    def test_missing_or_extra_archive_and_claim_are_rejected(self):
        for field,value in (("archives",{}),("archives",self.archives|{"unknown":self.archives["package"]}),
                            ("execution_claims",{}),("execution_claims",self.claims|{"unknown":{}})):
            with self.subTest(field=field),self.assertRaises(ValueError):self.call(**{field:value})

    def test_changed_transport_and_final_payload_are_rejected(self):
        self.archives["package"].write_bytes(b"different bytes")
        with self.assertRaisesRegex(PackageVerificationError,"Archive identity"):
            self.call()

    def test_source_version_tag_and_profile_are_prelocked(self):
        with self.assertRaisesRegex(ValueError,"tag"):
            self.call(release_tag="v9.9.9")
        (self.repo/"scripts/validation_profiles.json").write_text("{}")
        with self.assertRaisesRegex(ValueError,"source.*digest"):
            self.call()

    def test_dirty_or_wrong_checkout_is_refused(self):
        self.identity["dirty"]=True
        with self.assertRaisesRegex(ValueError,"clean"):
            self.call()
        self.identity.update(dirty=False,source_sha="f"*40)
        with self.assertRaisesRegex(ValueError,"source"):
            self.call()

    def test_real_u1_fixture_still_cannot_pass_through_aggregate(self):
        with self.assertRaisesRegex(ValueError,"test-fixture evidence cannot be used for release verification"):
            self.call(fake_u1=False)

    def test_local_saved_fixture_aggregate_never_authorizes_preupload(self):
        result=self.call()
        receipt=Path(result["receipt_path"])
        with self.assertRaisesRegex(ValueError,"fixture|github"):
            aggregate.verify_preupload(receipt,sha(receipt))
        with self.assertRaisesRegex(ValueError,"digest"):
            aggregate.verify_preupload(receipt,"0"*64)

    def test_caller_workflow_path_and_same_commit_callee_are_bound(self):
        for key,value in (("workflow_path",".github/workflows/another.yml"),
                          ("called_workflow_sha","f"*40)):
            with self.subTest(key=key):
                original=self.hosted["expected"][key]
                self.hosted["expected"][key]=value
                with self.assertRaisesRegex(ValueError,"workflow"):
                    self.call()
                self.hosted["expected"][key]=original

    def preupload_fixture(self, mutate):
        # A controlled caller seam only: no GitHub authentication is performed.
        self.hosted["transport"]="github"
        result=self.call()
        receipt=Path(result["receipt_path"])
        expected=sha(receipt)
        mutate(result)
        with patch.object(aggregate,"_source_identity",return_value=self.identity):
            return aggregate.verify_preupload(receipt,expected)

    def test_preupload_rechecks_final_payload_after_validation(self):
        with self.assertRaisesRegex(PackageVerificationError,"inventory changed"):
            self.preupload_fixture(lambda result: Path(result["upload_files"][0]["path"]).write_bytes(b"recompressed after validation"))

    def test_preupload_rechecks_transport_and_original_policy(self):
        with self.assertRaisesRegex(PackageVerificationError,"Archive identity changed"):
            self.preupload_fixture(lambda result:self.archives["package"].write_bytes(b"substituted Actions ZIP"))
        # Restore the transport separately, without manufacturing a pass.
        with zipfile.ZipFile(self.archives["package"],"w") as stream:
            for path in self.p.root.rglob("*"):
                if path.is_file():stream.write(path,path.relative_to(self.p.root).as_posix())
        self.hosted["artifacts"]["package"]["artifact_digest"]="sha256:"+sha(self.archives["package"])
        with self.assertRaisesRegex(ValueError,"source input changed.*digest"):
            self.preupload_fixture(lambda result:self.policy_path.write_text("{}"))

    def test_preupload_refuses_source_changes_since_aggregation(self):
        with self.assertRaisesRegex(ValueError,"Source checkout changed"):
            self.preupload_fixture(lambda result:self.identity.update(dirty=True))

    def test_preupload_positive_is_only_exact_bytes_not_release_permission(self):
        result=self.preupload_fixture(lambda result:None)
        self.assertEqual(result["status"],"UPLOAD_INPUTS_VERIFIED")
        self.assertFalse(result["release_ready"])

    def test_failed_aggregation_preserves_receipt_and_original_failure(self):
        with self.assertRaisesRegex(ValueError,"tag"):
            self.call(release_tag="v9.9.9")
        failure=json.loads((self.base/"attempt-1/aggregate.json").read_text())
        self.assertEqual(failure["status"],"FAIL")
        self.assertFalse(failure["release_ready"])
        self.assertIn("tag",failure["errors"][0])


if __name__=="__main__":unittest.main(verbosity=2)
