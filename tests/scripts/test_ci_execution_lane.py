"""Actual small-process U1 orchestration and reusable producer wiring contracts."""
from __future__ import annotations

import hashlib
import io
import json
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import tarfile
import unittest
import zipfile
from unittest.mock import patch

import yaml

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import ci_execution_lane as lane
from package_verification import PackageVerificationError, prepare_package
from execution_transport import TRANSPORT_NAME, create_execution_transport, prepare_execution_transport
from verify_execution_bundle import verify_execution_bundle, verify_execution_bundle_stable


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class ExecutionLaneTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="caesura-execution-lane-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.repo = self.root / "repo"
        self.repo.mkdir()
        self.git("init", "-q")
        self.git("config", "user.name", "Execution fixture")
        self.git("config", "user.email", "fixture@example.invalid")
        (self.repo / ".gitignore").write_text("build/\n__pycache__/\n", encoding="utf-8")
        (self.repo / "CMakeLists.txt").write_text("project(CaesuraAmeKAG VERSION 1.2.3 LANGUAGES CXX)\n")
        (self.repo / "probe.py").write_text(
            "from pathlib import Path\nimport sys\n"
            "p=Path(sys.argv[1]); p.write_text(str(int(p.read_text())+1) if p.exists() else '1')\n"
            "print('real miniature command completed')\n"
            "raise SystemExit(int(sys.argv[2]))\n", encoding="utf-8")
        host = {"Windows":"windows", "Linux":"linux", "Darwin":"macos"}[platform.system()]
        self.name = host + "-debug"
        self.profile = self.repo / "profiles.json"
        self.data = {"schema_version":1, "fixture_paths":["probe.py"], "profiles":{self.name:{
            "platform":host, "configuration":"Debug", "repository":"test/fixture",
            "checks":[{"id":"probe", "parser":"exit-code", "required":True,
                "command":["{python}", "{repo}/probe.py", "{run_dir}/count.txt", "0"],
                "cwd":"{repo}", "binary":"{python}", "timeout_seconds":15}]}}}
        self.commit_profile()
        self.args = dict(repo=self.repo, profile_file=self.profile, profile_sha256=sha(self.profile),
            profile_name=self.name, source_sha=self.head, build_dir=self.repo / "build",
            configuration="Debug", work_dir=self.root / "attempt")

    def git(self, *args):
        return subprocess.check_output(["git", *args], cwd=self.repo, stderr=subprocess.PIPE, text=True).strip()

    def commit_profile(self):
        self.profile.write_text(json.dumps(self.data), encoding="utf-8")
        self.git("add", ".")
        self.git("commit", "-qm", "fixture source")
        self.head = self.git("rev-parse", "HEAD")

    def run_lane(self, **changes):
        return lane.run_execution_lane(**(self.args | changes))

    def test_real_command_runs_once_and_strict_u1_exports_original_receipt(self):
        result = self.run_lane()
        self.assertEqual(result["status"], "EXECUTION_UPLOAD_READY")
        self.assertFalse(result["release_ready"])
        raw = self.root / "attempt/raw/run.json"
        self.assertEqual((raw.parent / "count.txt").read_text(), "1")
        self.assertEqual(result["receipt_sha256"], sha(raw))
        self.assertEqual(result["run_uuid"], json.loads(raw.read_text())["run_id"])
        bundle = Path(result["bundle_dir"])
        self.assertFalse(raw.is_relative_to(bundle))
        self.assertEqual((bundle / "execution-receipt.json").read_bytes(), raw.read_bytes())
        lane.verify_lane(result["lane_receipt"], sha(Path(result["lane_receipt"])))

    def downloaded_payload(self, result):
        archive = self.root / "actions.zip"
        # The real Actions transport stores file leaves, not empty directories.
        workflow = yaml.safe_load((ROOT / ".github/workflows/validate-engine.yml").read_text(encoding="utf-8"))
        upload = next(s for s in workflow["jobs"]["build-windows-debug"]["steps"]
                      if s.get("id") == "execution_upload")
        key = upload["with"]["path"].removeprefix("${{ steps.execution.outputs.").removesuffix(" }}")
        selected = Path(result[key])
        with zipfile.ZipFile(archive, "w") as stream:
            stream.write(selected, selected.name)
        prepared = prepare_package(archive, self.root / "download", expected_sha256=sha(archive))
        contents = prepare_execution_transport(prepared["package_path"], self.root / "contents")
        return Path(contents["package_path"])

    def verify_download(self, result, payload):
        raw = json.loads(Path(result["raw_receipt"]).read_bytes())
        return verify_execution_bundle(payload,
            manifest_sha256=result["manifest_sha256"], receipt_sha256=result["receipt_sha256"],
            profile_path=self.profile, profile_sha256=sha(self.profile), profile_name=self.name,
            source_sha=self.head, expected_context={key:raw[key] for key in
                ("run_id", "run_attempt", "repository", "workflow", "platform", "configuration")},
            trusted_dir=self.root / "trusted")

    def rewrite_transport(self, result, transform):
        archive = Path(result["transport_file"])
        with tarfile.open(archive, "r") as stream:
            records = [(item, stream.extractfile(item).read() if item.isfile() else None)
                       for item in stream.getmembers()]
        with tarfile.open(archive, "w") as stream:
            for item, data in transform(records):
                if data is not None:
                    item.size = len(data)
                stream.addfile(item, io.BytesIO(data) if data is not None else None)

    def test_empty_capture_survives_file_only_artifact_roundtrip(self):
        result = self.run_lane()
        capture = Path(result["bundle_dir"]) / "inputs/probe/sanitizer"
        self.assertTrue(capture.is_dir())
        self.assertEqual(list(capture.iterdir()), [])
        payload = self.downloaded_payload(result)
        self.assertTrue((payload / "inputs/probe/sanitizer").is_dir())
        self.assertEqual(list((payload / "inputs/probe/sanitizer").iterdir()), [])
        verified = self.verify_download(result, payload)
        self.assertEqual(verified["status"], "EXECUTION_BUNDLE_VERIFIED")
        verify_execution_bundle_stable(verified)
        (payload / "inputs/probe/sanitizer").rmdir()
        with self.assertRaisesRegex(ValueError, "changed"):
            verify_execution_bundle_stable(verified)

    def test_missing_capture_directory_in_tar_is_still_rejected(self):
        result = self.run_lane()
        self.rewrite_transport(result, lambda rows: [(i,d) for i,d in rows
            if i.name.rstrip("/") != "inputs/probe/sanitizer"])
        with self.assertRaisesRegex(ValueError, "sanitizer"):
            self.verify_download(result, self.downloaded_payload(result))

    def test_extra_capture_record_in_tar_is_still_rejected(self):
        result = self.run_lane()
        extra = tarfile.TarInfo("inputs/probe/sanitizer/sanitizer.123")
        self.rewrite_transport(result, lambda rows: rows + [(extra, b"unbound diagnostic")])
        with self.assertRaisesRegex(ValueError, "inventory differs"):
            self.verify_download(result, self.downloaded_payload(result))

    def test_tampered_bound_report_in_tar_is_still_rejected(self):
        result = self.run_lane()
        self.rewrite_transport(result, lambda rows: [(i, b"replacement" if i.name == "inputs/probe/stdout" else d)
            for i,d in rows])
        with self.assertRaisesRegex(ValueError, "hash mismatch|digest|bytes|mismatch"):
            self.verify_download(result, self.downloaded_payload(result))

    def test_real_child_diagnostic_remains_failure_after_transport(self):
        # This child emits transport-fixture diagnostic bytes. It does not claim
        # that a compiler sanitizer found a defect in the engine.
        probe = self.repo / "probe.py"
        probe.write_text(probe.read_text().replace("raise SystemExit(int(sys.argv[2]))", "") +
            "import json, os\n"
            "scope=json.loads(os.environ['CAESURA_VALIDATION_SANITIZER_CAPTURE'])\n"
            "(Path(scope['directory']) / ('sanitizer.'+str(os.getpid()))).write_text('ERROR: AddressSanitizer: transport-fixture diagnostic\\n')\n",
            encoding="utf-8")
        self.commit_profile()
        with self.assertRaisesRegex(ValueError, "sanitizer|Sanitizer"):
            self.run_lane(source_sha=self.head, profile_sha256=sha(self.profile))
        result = json.loads((self.root / "attempt/lane.json").read_bytes())
        payload = self.downloaded_payload(result)
        logs = list((payload / "inputs/probe/sanitizer").iterdir())
        self.assertEqual(len(logs), 1)
        self.assertIn(b"transport-fixture diagnostic", logs[0].read_bytes())
        with self.assertRaisesRegex(ValueError, "sanitizer|Sanitizer"):
            self.verify_download(result, payload)

    def test_transport_change_is_rejected_before_upload(self):
        result = self.run_lane()
        Path(result["transport_file"]).write_bytes(b"changed archive")
        with self.assertRaisesRegex(ValueError, "transport changed"):
            lane.verify_lane(result["lane_receipt"], sha(Path(result["lane_receipt"])))

    def test_outer_transport_requires_exact_single_archive(self):
        root = self.root / "outer"; root.mkdir()
        with self.assertRaisesRegex(ValueError, "exactly"):
            prepare_execution_transport(root, self.root / "absent")
        (root / TRANSPORT_NAME).write_bytes(b"placeholder")
        (root / "extra.txt").write_text("not accepted")
        with self.assertRaisesRegex(ValueError, "exactly"):
            prepare_execution_transport(root, self.root / "extra")

    def test_nested_tar_traversal_is_rejected_by_safe_extractor(self):
        result = self.run_lane()
        outside = tarfile.TarInfo("../outside")
        self.rewrite_transport(result, lambda rows: rows + [(outside, b"escape")])
        with self.assertRaises(PackageVerificationError):
            self.downloaded_payload(result)
        self.assertFalse((self.root / "outside").exists())

    def test_packing_change_is_rejected_and_original_archive_is_not_overwritten(self):
        source = self.root / "packing"; source.mkdir()
        data = source / "data"; data.write_bytes(b"original")
        target = self.root / TRANSPORT_NAME
        addfile = tarfile.TarFile.addfile
        def change(stream, info, fileobj=None):
            result = addfile(stream, info, fileobj)
            data.write_bytes(b"changed")
            return result
        with patch.object(tarfile.TarFile, "addfile", change), self.assertRaisesRegex(ValueError, "changed"):
            create_execution_transport(source, target)
        preserved = target.read_bytes()
        with self.assertRaises(FileExistsError):
            create_execution_transport(source, target)
        self.assertEqual(target.read_bytes(), preserved)

    def test_nonzero_command_retains_first_raw_failure_and_never_exports_ready(self):
        self.data["profiles"][self.name]["checks"][0]["command"][-1] = "23"
        self.commit_profile()
        with self.assertRaisesRegex(ValueError, "Required check|FAIL"):
            self.run_lane(source_sha=self.head, profile_sha256=sha(self.profile))
        raw = self.root / "attempt/raw/run.json"
        self.assertEqual(json.loads(raw.read_text())["checks"][0]["exit_code"], 23)
        first = raw.read_bytes()
        with self.assertRaises(FileExistsError):
            self.run_lane()
        self.assertEqual(raw.read_bytes(), first)
        self.assertEqual(json.loads((raw.parent.parent / "lane.json").read_text())["status"], "FAIL")

    def test_wrong_source_profile_or_dirty_checkout_rejected_before_execution(self):
        for index, changes in enumerate(({"source_sha":"a"*40}, {"profile_sha256":"0"*64}, {})):
            with self.subTest(changes=changes):
                if not changes:
                    (self.repo / "dirty").write_text("untracked")
                target = self.root / ("attempt-" + str(index))
                with self.assertRaises(ValueError):
                    self.run_lane(work_dir=target, **changes)
                self.assertFalse((target / "raw").exists())

    def test_work_inside_source_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "outside"):
            self.run_lane(work_dir=self.repo / "attempt")

    def test_profile_or_bundle_mutation_is_rejected_before_upload(self):
        result = self.run_lane()
        receipt = Path(result["lane_receipt"])
        lock = sha(receipt)
        (Path(result["bundle_dir"]) / "inputs/probe/stdout").write_text("changed")
        with self.assertRaisesRegex(ValueError, "changed|digest|mismatch"):
            lane.verify_lane(receipt, lock)

    def test_original_receipt_and_lane_digest_mutations_are_rejected(self):
        result = self.run_lane()
        receipt = Path(result["lane_receipt"])
        with self.assertRaisesRegex(ValueError, "digest"):
            lane.verify_lane(receipt, "0"*64)
        digest = sha(receipt)
        Path(result["raw_receipt"]).write_text("{}")
        with self.assertRaisesRegex(ValueError, "Original receipt changed"):
            lane.verify_lane(receipt, digest)

    def test_source_change_after_execution_rejected_before_upload(self):
        result = self.run_lane()
        receipt = Path(result["lane_receipt"])
        (self.repo / "probe.py").write_text("changed")
        with self.assertRaisesRegex(ValueError, "Source changed"):
            lane.verify_lane(receipt, sha(receipt))

    def test_embedded_identity_and_work_path_commands_write_real_output_lines(self):
        workflow = yaml.safe_load((ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8"))
        identity = next(s["run"] for s in workflow["jobs"]["prepare"]["steps"] if s.get("id") == "identity")
        code = identity.split("\n", 1)[1].rsplit("\nPY", 1)[0]
        target = self.root / "github-output"
        env = dict(lane.os.environ, SOURCE_SHA=self.head, GITHUB_OUTPUT=str(target),
                   GITHUB_REPOSITORY="test/fixture", GITHUB_REPOSITORY_ID="123",
                   GITHUB_RUN_ID="456", GITHUB_RUN_ATTEMPT="1", GITHUB_WORKFLOW_SHA="b"*40,
                   GITHUB_WORKFLOW_REF="test/fixture/.github/workflows/ci.yml@refs/heads/main")
        subprocess.run([sys.executable, "-c", code], cwd=self.repo, env=env, check=True)
        lines = target.read_text().splitlines()
        self.assertEqual(lines[0], "source_sha=" + self.head)
        if len(lines) > 1:
            expected = json.loads(lines[1].removeprefix("expected_json="))
            self.assertEqual(expected["source_sha"], self.head)
            self.assertEqual(expected["workflow_sha"], "b"*40)
        workflow = yaml.safe_load((ROOT / ".github/workflows/validate-engine.yml").read_text(encoding="utf-8"))
        work_code = next(s["run"] for s in workflow["jobs"]["release"]["steps"] if s.get("shell") == "python")
        env.update(RUNNER_TEMP=str(self.root), GITHUB_ENV=str(self.root / "github-env"),
                   GITHUB_JOB="release", GITHUB_RUN_ID="123", GITHUB_RUN_ATTEMPT="2")
        subprocess.run([sys.executable, "-c", work_code], env=env, check=True)
        lines = (self.root / "github-env").read_text().splitlines()
        self.assertEqual(len(lines), 2)
        self.assertEqual(lines[0], "EXECUTION_WORK=" + str(self.root / "u23-execution-release-123-2"))

    def test_context_guard_rejects_unrelated_source_and_workflow(self):
        env = {"GITHUB_ACTIONS":"true", "GITHUB_SHA":self.head, "GITHUB_REPOSITORY":"owner/repo",
            "GITHUB_WORKFLOW_REF":"owner/repo/.github/workflows/ci.yml@refs/heads/main",
            "GITHUB_WORKFLOW_SHA":"b"*40, "SOURCE_SHA":self.head, "TRIGGER_HEAD_SHA":self.head,
            "SOURCE_MODE":"head", "CALLER_WORKFLOW_PATH":".github/workflows/ci.yml",
            "CALLER_WORKFLOW_REF":"owner/repo/.github/workflows/ci.yml@refs/heads/main",
            "CALLER_WORKFLOW_SHA":"b"*40, "CALLED_WORKFLOW_SHA":"b"*40,
            "CALLED_WORKFLOW_PATH":".github/workflows/validate-engine.yml", "POLICY_SHA256":"c"*64,
            "PROFILE_SHA256":sha(self.profile)}
        env.update({key:lane.os.environ[key] for key in ("PATH", "SystemRoot") if key in lane.os.environ})
        with patch.dict(lane.os.environ, env, clear=True):
            lane.guard_context(self.repo, self.profile)
            for key, wrong in (("SOURCE_SHA", "c"*40), ("CALLER_WORKFLOW_SHA", "d"*40),
                               ("CALLED_WORKFLOW_SHA", "d"*40), ("PROFILE_SHA256", "e"*64),
                               ("TRIGGER_HEAD_SHA", "f"*40)):
                with self.subTest(key=key), patch.dict(lane.os.environ, {key:wrong}), self.assertRaises(ValueError):
                    lane.guard_context(self.repo, self.profile)
            with patch.dict(lane.os.environ, {"ENGINE_VERSION":"9.9.9"}), self.assertRaisesRegex(ValueError, "version"):
                lane.guard_context(self.repo, self.profile)
            with patch.dict(lane.os.environ, {"EXPECTED_RUNNER_ARCH":"ARM64", "RUNNER_ARCH":"X64"}), self.assertRaisesRegex(ValueError, "architecture"):
                lane.guard_context(self.repo, self.profile)
            event = self.root / "event.json"
            event.write_text(json.dumps({"number":25, "pull_request":{"head":{"sha":self.head}}}))
            with patch.dict(lane.os.environ, {"GITHUB_SHA":"e"*40, "GITHUB_EVENT_NAME":"pull_request",
                                            "GITHUB_EVENT_PATH":str(event), "PULL_REQUEST_NUMBER":"25"}):
                lane.guard_context(self.repo, self.profile)
                with patch.dict(lane.os.environ, {"PULL_REQUEST_NUMBER":"26"}), self.assertRaisesRegex(ValueError, "PR"):
                    lane.guard_context(self.repo, self.profile)


class WorkflowContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ci = yaml.safe_load((ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8-sig"))
        cls.workflow = yaml.safe_load((ROOT / ".github/workflows/validate-engine.yml").read_text(encoding="utf-8-sig"))
        cls.jobs = cls.workflow["jobs"]

    def test_ci_is_a_direct_readonly_wrapper_with_prelocked_policy(self):
        self.assertEqual(self.ci["jobs"]["validate"]["uses"], "./.github/workflows/validate-engine.yml")
        self.assertEqual(self.ci["jobs"]["validate"]["name"], "Validate engine")
        self.assertIn("prepare", self.ci["jobs"]["validate"]["needs"])
        self.assertTrue(all(value == "read" for value in self.ci["permissions"].values()))
        text = json.dumps(self.ci["jobs"]["prepare"])
        self.assertIn("prepare_release_policy.py", text)
        self.assertIn("policy_sha256", json.dumps(self.ci["jobs"]["validate"]["with"]))

    def test_six_profiles_have_distinct_outputs_and_four_package_uploads(self):
        pairs = {"build-windows-debug":"windows-debug", "release":"windows-release",
                 "build-linux":"linux-debug", "release-linux":"linux-release",
                 "build-macos":"macos-debug", "release-macos":"macos-release"}
        for key, profile in pairs.items():
            with self.subTest(profile=profile):
                job = self.jobs[key]
                self.assertNotIn("strategy", job)
                execution = [s for s in job["steps"] if s.get("id") == "execution"]
                self.assertEqual(len(execution), 1)
                self.assertIn("--profile-name " + profile, execution[0]["run"])
                self.assertNotIn("--diagnostic", execution[0]["run"])
                for field in ("execution_artifact_id", "execution_artifact_digest", "execution_manifest_sha256",
                              "execution_receipt_sha256", "execution_run_uuid"):
                    self.assertIn(field, job["outputs"])
                accepted = [s for s in job["steps"] if s.get("id") == "execution_upload"]
                self.assertEqual(accepted[0]["with"]["if-no-files-found"], "error")
                self.assertEqual(accepted[0]["with"]["path"], "${{ steps.execution.outputs.transport_file }}")
        for key in ("release", "release-linux", "release-macos", "release-web"):
            self.assertIn("package_artifact_id", self.jobs[key]["outputs"])
            self.assertTrue(any(s.get("id") == "package_upload" for s in self.jobs[key]["steps"]))

    def test_policy_role_outputs_and_declared_names_match_without_claiming_hosted_observation(self):
        policy = json.loads((ROOT / "scripts/release_input_policy.json").read_text(encoding="utf-8"))
        outputs = self.workflow["on"]["workflow_call"]["outputs"]
        for role, prefix in policy["output_prefixes"].items():
            fields = ["artifact_id", "artifact_digest", "manifest_sha256"]
            if role in policy["execution_inputs"]:
                fields += ["receipt_sha256", "run_uuid"]
            for field in fields:
                self.assertIn(prefix + "_" + field, outputs)
        for spec in policy["package_inputs"].values():
            self.assertIn(spec["job_key"], self.jobs)
        declared = {"Validate engine / " + j["name"] for j in self.jobs.values()}
        self.assertTrue(set(policy["required_jobs"].values()).issubset(declared))

    def test_profile_runs_replace_duplicate_engine_build_ctest_and_lua_suites(self):
        for key in ("build-windows-debug", "release", "build-linux", "release-linux", "build-macos", "release-macos"):
            job = self.jobs[key]
            commands = [s.get("run", "") for s in job["steps"]]
            self.assertFalse(any("ctest --test-dir build" in c for c in commands), key)
            self.assertFalse(any("cmake --build build --config" in c for c in commands), key)
            configure = [c for c in commands if "cmake -B build -S ." in c]
            self.assertEqual(len(configure), 1)
            self.assertIn("CAESURA_REQUIRE_TEST_PREREQUISITES=ON", configure[0])

    def test_all_producer_checkouts_bind_source_and_guard_before_build(self):
        for key, job in self.jobs.items():
            with self.subTest(job=key):
                steps = job["steps"]
                checkout = next(s for s in steps if s.get("uses", "").startswith("actions/checkout@"))
                self.assertEqual(checkout["with"]["ref"], "${{ inputs.source_sha }}")
                guard = next(i for i, s in enumerate(steps) if s.get("id") == "source_guard")
                first_run = next(i for i, s in enumerate(steps) if "run" in s)
                self.assertEqual(guard, first_run)

    def test_early_source_failure_cannot_expand_diagnostic_upload_to_root(self):
        selected = 0
        for job in self.jobs.values():
            for step in job["steps"]:
                if not step.get("uses", "").startswith("actions/upload-artifact@"):
                    continue
                path = step.get("with", {}).get("path", "")
                for variable in ("EXECUTION_WORK", "PACKAGE_WORK"):
                    if "${{ env." + variable + " }}" in path:
                        selected += 1
                        self.assertIn("env." + variable + " != ''", step.get("if", ""),
                                      "Unset work path after a guard failure must never resolve to runner root")
        self.assertEqual(selected, 11)

    def test_existing_hard_gates_and_optional_android_state_are_retained(self):
        self.assertTrue(self.jobs["android-compile"]["continue-on-error"])
        for key in ("ios-compile", "android-static", "build-linux", "build-windows-debug"):
            self.assertNotIn("continue-on-error", self.jobs[key])
        text = json.dumps(self.workflow)
        for command in ("web_audio_smoke.mjs", "test_replay_cli.py", "verify_android_regression.py",
                        "test_package_output_transactions.py", "test_package_asset_dependencies.py",
                        "generate_platform_status.py", "generate_plan_status.py", "capability_closure.py",
                        "ks_check.lua", "sample_game_headless.lua", "test_run_apple_validation.py"):
            self.assertIn(command, text)
        self.assertIn("Preserve first package attempt evidence", text)


if __name__ == "__main__":
    unittest.main(verbosity=2)
