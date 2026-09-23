"""Container protocols use a host-command boundary double and real file copies.

These tests do not turn Windows into an actual hdiutil/AppImage runtime lane.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import package_containers as containers
from package_verification import _sha256_file


class ContainerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="caesura-container-contract-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.source = self.root / "最终 包.dmg"
        self.source.write_bytes(b"container fixture bytes, never executable evidence")
        self.source.chmod(0o755)
        self.digest = _sha256_file(self.source)
        self.attempt = self.root / "attempt"
        self.tool = Path(sys.executable).resolve()  # Explicit identity, never executed as hdiutil.
        self.calls = []
        self.images = []
        self.behavior = "success"

    def image(self, source, mount, device="/dev/disk91s1"):
        return {"image-path": str(source), "system-entities": [
            {"dev-entry": device.removesuffix("s1")},
            {"dev-entry": device, "mount-point": str(mount)}]}

    def command(self, argv, cwd, env, control_dir, stdout, stderr, timeout, **kwargs):
        self.calls.append(list(argv))
        self.assertEqual(env["HOME"], str(self.attempt / "home"))
        self.assertEqual(env["PWD"], str(self.attempt / "work"))
        self.assertEqual(env["PATH"], "/usr/bin:/bin:/usr/sbin:/sbin")
        self.assertFalse({"CAESURA_LUA", "PYTHONPATH", "NODE_OPTIONS", "HTTP_PROXY"} & set(env))
        control = Path(control_dir)
        control.mkdir()
        receipt = {"status": "EXITED", "actual_exit_code": 0,
                   "owned_tree_cleanup": "COMPLETE", "process": None,
                   "test_boundary_double": True}
        if "--appimage-extract" in argv:
            extract = Path(cwd) / "squashfs-root"
            extract.mkdir()
            (extract / "AppRun").write_text("fixture AppRun", encoding="utf-8")
            (extract / "usr/bin").mkdir(parents=True)
            (extract / "usr/bin/CaesuraAmeKAG").write_bytes(b"fixture engine")
            if self.behavior == "outside-link":
                (extract / "leak").symlink_to(self.source)
            if self.behavior == "missing-root":
                extract.rename(Path(cwd) / "some-other-root")
            if self.behavior == "source-mutates":
                self.source.write_bytes(b"changed original")
            if self.behavior == "timeout":
                receipt.update(status="TIMED_OUT", actual_exit_code=None)
                (control / "run.json").write_text(json.dumps(receipt), encoding="utf-8")
                raise subprocess.TimeoutExpired(argv, timeout)
        elif argv[1] == "info":
            observed = json.loads(json.dumps(self.images))
            if self.behavior == "info-extra-fields":
                for row in observed:
                    for entry in row["system-entities"]:
                        entry["volume-kind"] = "test-host-description"
            stdout.write(plistlib.dumps({"images": observed}))
        elif argv[1] == "attach":
            source, mount = Path(argv[-1]), Path(argv[argv.index("-mountpoint") + 1])
            (mount / "CaesuraAmeKAG").write_bytes(b"fixture engine")
            (mount / "unrelated").mkdir()
            self.images.append(self.image(source, mount))
            if self.behavior == "foreign-source":
                self.images[-1]["image-path"] = str(self.root / "other.dmg")
            if self.behavior == "attach-mismatch":
                stdout.write(plistlib.dumps({"system-entities": [
                    {"dev-entry": "/dev/disk123s1", "mount-point": str(mount)}]}))
            elif self.behavior == "attach-timeout":
                receipt.update(status="TIMED_OUT", actual_exit_code=None)
                (control / "run.json").write_text(json.dumps(receipt), encoding="utf-8")
                raise subprocess.TimeoutExpired(argv, timeout)
            else:
                stdout.write(plistlib.dumps({"system-entities": self.images[-1]["system-entities"]}))
        elif argv[1] == "detach":
            if self.behavior == "replace-preparation":
                (self.attempt / "payload/preparation.json").write_text('{"status":"FAIL"}', encoding="utf-8")
            if self.behavior == "replace-command-log":
                (self.attempt / "commands/attach/stdout.log").write_bytes(b"replaced attach output")
            if self.behavior == "detach-fails":
                receipt["actual_exit_code"] = 1
            elif self.behavior != "detach-still-mounted":
                self.images = [row for row in self.images if not any(
                    entry["dev-entry"] == argv[-1] for entry in row["system-entities"])]
        else:
            raise AssertionError(argv)
        (control / "run.json").write_text(json.dumps(receipt), encoding="utf-8")
        return receipt

    def prepare(self, fmt="dmg", payload="."):
        with patch.object(containers, "_host_platform", return_value="macos" if fmt == "dmg" else "linux"), \
                patch.object(containers, "run_runtime_command", side_effect=self.command):
            return containers.prepare_container(
                self.source, self.attempt, container_format=fmt,
                expected_sha256=self.digest, payload_relative_path=payload,
                hdiutil_executable=self.tool if fmt == "dmg" else None)

    def assert_failure(self, report):
        self.assertEqual(report["status"], "CONTAINER_FAIL", report)
        self.assertEqual(report["runtime"], "NOT_RUN")
        self.assertFalse(report["accepted"])
        self.assertEqual(json.loads((self.attempt / "container-preparation.json").read_text(
            encoding="utf-8"))["status"], "CONTAINER_FAIL")

    def test_dmg_copies_exact_payload_and_detaches_only_owned_device(self):
        self.images = [self.image(self.root / "existing.dmg", self.root / "existing", "/dev/disk81s1")]
        report = self.prepare()
        self.assertEqual(report["status"], "CONTAINER_PREPARED", report)
        self.assertEqual(report["cleanup"]["status"], "DETACHED")
        self.assertEqual(report["runtime"], "NOT_RUN")
        self.assertFalse(report["accepted"])
        self.assertEqual(Path(report["package_path"]).joinpath("CaesuraAmeKAG").read_bytes(), b"fixture engine")
        self.assertEqual([call[-1] for call in self.calls if call[1] == "detach"], ["/dev/disk91s1"])
        attach = next(call for call in self.calls if call[1] == "attach")
        self.assertIn("-readonly", attach)
        self.assertIn("-plist", attach)
        self.assertEqual(len(self.images), 1)
        # An unmounted source directory cannot be the late stability oracle.
        shutil.rmtree(self.attempt / "mount")
        self.assertEqual(containers.verify_container_stable(report)["status"], "STABLE")

    def test_wrong_digest_does_not_launch_any_host_command(self):
        self.digest = "0" * 64
        self.assert_failure(self.prepare())
        self.assertEqual(self.calls, [])

    def test_platform_mismatch_records_not_run_without_launch(self):
        with patch.object(containers, "_host_platform", return_value="windows"), \
                patch.object(containers, "run_runtime_command", side_effect=self.command):
            report = containers.prepare_container(self.source, self.attempt, container_format="dmg",
                expected_sha256=self.digest, payload_relative_path=".", hdiutil_executable=self.tool)
        self.assert_failure(report)
        self.assertEqual(self.calls, [])

    def test_unsafe_payload_refused_before_launch(self):
        for index, payload in enumerate(("../outside", "/absolute", "a\\b", "", "a/../b", "C:/bad")):
            with self.subTest(payload=payload):
                self.attempt = self.root / f"attempt-{index}"
                self.assert_failure(self.prepare(payload=payload))
        self.assertEqual(self.calls, [])

    def test_existing_attempt_and_other_repository_are_never_written(self):
        self.attempt.mkdir()
        marker = self.attempt / "keep"
        marker.write_bytes(b"preserve")
        with self.assertRaises(Exception):
            self.prepare()
        self.assertEqual(list(self.attempt.iterdir()), [marker])
        repository = self.root / "other-repo"
        repository.mkdir()
        (repository / ".git").write_text("gitdir: metadata", encoding="utf-8")
        self.attempt = repository / "new-attempt"
        with self.assertRaises(Exception):
            self.prepare()
        self.assertFalse(self.attempt.exists())

    def test_resolved_existing_parent_alias_keeps_new_leaf_and_repository_checks(self):
        physical = self.root / "private/tmp"
        physical.mkdir(parents=True)
        alias = self.root / "tmp"
        alias.symlink_to(physical, target_is_directory=True)
        selected = containers._new_attempt(alias / "fresh")
        self.assertEqual(selected, physical / "fresh")
        self.assertTrue(selected.is_dir())
        existing = physical / "existing"
        existing.write_bytes(b"preserve")
        for leaf in ("existing", "linked"):
            if leaf == "linked":
                (physical / leaf).symlink_to(physical / "missing", target_is_directory=True)
            with self.subTest(leaf=leaf), self.assertRaises(ValueError):
                containers._new_attempt(alias / leaf)
        self.assertEqual(existing.read_bytes(), b"preserve")
        repository = physical / "repository"
        repository.mkdir()
        (repository / ".git").write_text("gitdir: metadata", encoding="utf-8")
        with self.assertRaises(ValueError):
            containers._new_attempt(alias / "repository/forbidden")
        self.assertFalse((repository / "forbidden").exists())

    def test_established_mount_devices_cannot_be_replaced_before_cleanup(self):
        command = self.command
        info_calls = 0
        def replace_after_preparation(argv, *args, **kwargs):
            nonlocal info_calls
            if argv[1] == "info":
                info_calls += 1
                if info_calls == 3:
                    row = self.images[0]
                    mount = next(entry["mount-point"] for entry in row["system-entities"] if "mount-point" in entry)
                    self.images = [self.image(Path(row["image-path"]), Path(mount), "/dev/disk92s1")]
            return command(argv, *args, **kwargs)
        self.command = replace_after_preparation
        report = self.prepare()
        self.assert_failure(report)
        self.assertEqual(report["mount_ownership"]["device"], "/dev/disk91s1")
        self.assertEqual(report["cleanup"]["status"], "FAILED_OR_UNPROVEN")
        self.assertFalse(any(call[1] == "detach" for call in self.calls))
        self.assertEqual(self.images[0]["system-entities"][1]["dev-entry"], "/dev/disk92s1")

    def test_foreign_mount_is_never_detached(self):
        self.behavior = "foreign-source"
        self.assert_failure(self.prepare())
        self.assertEqual([call for call in self.calls if call[1] == "detach"], [])

    def test_attach_plist_mismatch_fails_but_cleans_independently_owned_mount(self):
        self.behavior = "attach-mismatch"
        report = self.prepare()
        self.assert_failure(report)
        self.assertEqual(report["cleanup"]["status"], "DETACHED")
        self.assertEqual([call[-1] for call in self.calls if call[1] == "detach"], ["/dev/disk91s1"])

    def test_timeout_after_attach_still_observes_and_cleans_owned_mount(self):
        self.behavior = "attach-timeout"
        report = self.prepare()
        self.assert_failure(report)
        self.assertEqual(report["cleanup"]["status"], "DETACHED")
        self.assertEqual(len(self.images), 0)

    def test_detach_failure_and_false_success_both_prevent_prepared(self):
        for index, behavior in enumerate(("detach-fails", "detach-still-mounted")):
            with self.subTest(behavior=behavior):
                self.behavior, self.images = behavior, []
                self.attempt = self.root / f"attempt-{index}"
                report = self.prepare()
                self.assert_failure(report)
                self.assertNotEqual(report["cleanup"]["status"], "DETACHED")

    def test_missing_explicit_payload_does_not_select_a_child(self):
        report = self.prepare(payload="missing")
        self.assert_failure(report)
        self.assertEqual(report["cleanup"]["status"], "DETACHED")
        self.assertNotIn("preparation", report)

    def test_host_description_fields_do_not_replace_device_mount_identity(self):
        self.behavior = "info-extra-fields"
        report = self.prepare()
        self.assertEqual(report["status"], "CONTAINER_PREPARED", report)

    def test_completed_stage_evidence_cannot_be_replaced_by_later_command(self):
        for index, behavior in enumerate(("replace-preparation", "replace-command-log")):
            with self.subTest(behavior=behavior):
                self.behavior, self.images = behavior, []
                self.attempt = self.root / f"attempt-{index}"
                self.assert_failure(self.prepare())

    def test_old_device_identity_cannot_authorize_detach(self):
        self.images = [self.image(self.root / "old.dmg", self.root / "other-mount")]
        self.assert_failure(self.prepare())
        self.assertFalse(any(call[1] == "detach" for call in self.calls))

    def test_extracted_link_to_original_container_is_rejected(self):
        self.behavior = "outside-link"
        self.assert_failure(self.prepare("appimage"))

    def test_final_container_receipt_replacement_is_rejected(self):
        report = self.prepare("appimage")
        self.assertEqual(report["status"], "CONTAINER_PREPARED", report)
        (self.attempt / "container-preparation.json").write_text('{"status":"CONTAINER_FAIL"}', encoding="utf-8")
        with self.assertRaises(Exception):
            containers.verify_container_stable(report)

    def test_appimage_uses_locked_copy_and_preserves_actual_layout(self):
        report = self.prepare("appimage")
        self.assertEqual(report["status"], "CONTAINER_PREPARED", report)
        self.assertEqual(len(self.calls), 1)
        self.assertEqual(self.calls[0], [report["execution_input"]["path"], "--appimage-extract"])
        self.assertNotEqual(self.calls[0][0], str(self.source))
        package = Path(report["package_path"])
        self.assertTrue((package / "usr/bin/CaesuraAmeKAG").is_file())
        self.assertTrue((package / "AppRun").is_file())
        self.assertFalse((package / "CaesuraAmeKAG").exists())
        self.assertEqual(containers.verify_container_stable(report)["status"], "STABLE")

    def test_appimage_timeout_missing_root_source_mutation_all_fail(self):
        for index, behavior in enumerate(("timeout", "missing-root", "source-mutates")):
            with self.subTest(behavior=behavior):
                self.behavior = behavior
                self.attempt = self.root / f"attempt-{index}"
                self.assert_failure(self.prepare("appimage"))

    def test_late_mutation_of_original_copy_payload_or_receipt_is_rejected(self):
        for index, target in enumerate(("source", "execution_input", "package", "receipt")):
            with self.subTest(target=target):
                self.source.write_bytes(b"container fixture bytes, never executable evidence")
                self.attempt = self.root / f"attempt-{index}"
                report = self.prepare("appimage")
                self.assertEqual(report["status"], "CONTAINER_PREPARED", report)
                selected = {"source": self.source,
                    "execution_input": Path(report["execution_input"]["path"]),
                    "package": Path(report["package_path"]) / "AppRun",
                    "receipt": self.attempt / "payload/preparation.json"}[target]
                selected.write_bytes(b"changed")
                with self.assertRaises(Exception):
                    containers.verify_container_stable(report)


if __name__ == "__main__":
    unittest.main(verbosity=2)
