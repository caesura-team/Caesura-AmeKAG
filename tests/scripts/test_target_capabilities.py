#!/usr/bin/env python3
"""Target capability contracts through selected native and author CLI processes."""
import hashlib
import json
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import caesura_build


class NativeCapabilityQuery(unittest.TestCase):
    def test_selected_binary_reports_effective_build_without_initializing_backends(self):
        binary = caesura_build.find_engine()
        before = hashlib.sha256(binary.read_bytes()).hexdigest()
        with tempfile.TemporaryDirectory(prefix="caesura-capabilities-") as scratch:
            result = subprocess.run([str(binary), "--capabilities-json"], cwd=scratch,
                                    capture_output=True, text=True, encoding="utf-8",
                                    errors="replace", timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            profile = json.loads(result.stdout)
            self.assertEqual(list(Path(scratch).iterdir()), [])
        self.assertEqual(hashlib.sha256(binary.read_bytes()).hexdigest(), before)
        self.assertEqual(profile["schema"], 1)
        self.assertEqual(profile["target"], "native")
        self.assertEqual(profile["scope"], "build")
        self.assertIn(profile["platform"], ("windows", "linux", "macos", "ios", "android"))
        catalog = (ROOT / "config/runtime-capabilities.json").read_text(encoding="utf-8")
        digest = hashlib.sha256(catalog.replace("\r\n", "\n").encode()).hexdigest()
        self.assertEqual(profile["catalog_sha256"], digest)
        self.assertEqual(set(profile["compiled"]), {"ffmpeg", "live2d", "steam"})
        for name, value in profile["compiled"].items():
            self.assertIs(type(value), bool)
            expected = os.environ.get("CAESURA_EXPECT_" + name.upper())
            if expected is not None:
                self.assertIn(expected, ("0", "1"))
                self.assertEqual(value, expected == "1", name)
        self.assertNotIn("[BackendFactory]", result.stdout + result.stderr)
        self.assertNotIn("Entering main loop", result.stdout + result.stderr)


class CapabilityPreflight(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lua = caesura_build.find_lua()
        binary = caesura_build.find_engine()
        result = subprocess.run([str(binary), "--capabilities-json"], capture_output=True,
                                text=True, encoding="utf-8", timeout=15)
        if result.returncode:
            raise RuntimeError("selected binary lacks a capability profile: " + result.stderr)
        cls.native = json.loads(result.stdout)
        cls.web = {"schema": 1, "target": "web", "platform": "browser", "scope": "build",
                   "catalog_sha256": cls.native["catalog_sha256"], "compiled": {}}

    def check_project(self, scene, declaration=None, *, profile=None, target="web"):
        with tempfile.TemporaryDirectory(prefix="caesura-preflight-") as scratch:
            directory = Path(scratch) / "项目 空格"
            directory.mkdir()
            story = directory / "story.ks"
            story.write_text(scene, encoding="utf-8")
            project = directory / "caesura.project.json"
            project.write_text(json.dumps({} if declaration is None else declaration), encoding="utf-8")
            facts = directory / "profile.json"
            facts.write_text(json.dumps(profile or self.web), encoding="utf-8")
            report = directory / "report.json"
            result = subprocess.run([self.lua, str(ROOT / "scripts/ks_check.lua"),
                                     "--target", target, "--profile", str(facts),
                                     "--project", str(project), "--json-output", str(report),
                                     "--capabilities-only", str(story)],
                                    cwd=ROOT, capture_output=True, text=True, encoding="utf-8", timeout=30)
            self.assertTrue(report.is_file(), result.stdout + result.stderr)
            return result.returncode, json.loads(report.read_text(encoding="utf-8")), result.stdout + result.stderr

    def test_static_video_is_required_without_a_declaration(self):
        rc, report, output = self.check_project('[video file="opening.mpg"]\n[p]\n')
        self.assertEqual(rc, 1, output)
        self.assertFalse(report["passed"])
        self.assertIn("video.play", json.dumps(report["errors"]))
        self.assertIn("story.ks", json.dumps(report["requirements"]))

    def test_optional_video_is_reported_as_a_skip(self):
        rc, report, output = self.check_project('[video file="opening.mpg"]\n[p]\n',
            {"capabilities": {"optional": ["video.play"]}})
        self.assertEqual(rc, 0, output)
        self.assertTrue(report["passed"])
        self.assertTrue(report["warnings"])
        self.assertIn("unsupported", json.dumps(report["warnings"]))

    def test_declared_required_feature_is_checked_without_an_executed_call(self):
        rc, report, output = self.check_project('[p]\n',
            {"capabilities": {"required": ["video.play"]}})
        self.assertEqual(rc, 1, output)
        self.assertFalse(report["passed"])

    def test_approximation_requires_explicit_acceptance(self):
        scene = '[palette effect="apply" id="night"]\n[p]\n'
        rc, denied, _ = self.check_project(scene)
        self.assertEqual(rc, 1)
        self.assertFalse(denied["passed"])
        rc, accepted, output = self.check_project(scene,
            {"capabilities": {"accept_approximate": ["render.postfx.lut3d"]}})
        self.assertEqual(rc, 0, output)
        self.assertTrue(accepted["passed"])
        self.assertIn("approximate", json.dumps(accepted["requirements"]))

    def test_dynamic_lua_is_unproved_without_rejecting_the_whole_project(self):
        scene = '[iscript]\nif false then require("backend").video_play("opening.mpg") end\n[endscript]\n[p]\n'
        rc, report, output = self.check_project(scene)
        self.assertEqual(rc, 0, output)
        self.assertTrue(report["passed"])
        self.assertTrue(report["not_proven"])

    def test_bad_declaration_and_catalog_identity_fail_before_packaging(self):
        for caps in ({"requred": ["video.play"]}, {"required": "video.play"},
                     {"required": ["video.paly"]}, {"required": ["video.play", "video.play"]},
                     {"required": ["video.play"], "optional": ["video.play"]}, None):
            with self.subTest(caps=caps):
                rc, report, _ = self.check_project('[p]\n', {"capabilities": caps})
                self.assertEqual(rc, 1)
                self.assertFalse(report["passed"])
        wrong = dict(self.web, catalog_sha256="0" * 64)
        rc, report, _ = self.check_project('[p]\n', profile=wrong)
        self.assertEqual(rc, 1)
        self.assertFalse(report["passed"])

    def test_native_sdk_off_does_not_erase_basic_mpeg_eligibility(self):
        profile = json.loads(json.dumps(self.native))
        profile["compiled"]["ffmpeg"] = False
        rc, report, output = self.check_project('[video file="opening.mpg"]\n[p]\n',
            profile=profile, target="native")
        self.assertEqual(rc, 0, output)
        self.assertTrue(report["passed"])
        rc, report, _ = self.check_project('[video file="opening.mp4"]\n[p]\n',
            profile=profile, target="native")
        self.assertEqual(rc, 1)
        self.assertIn("video.ffmpeg", json.dumps(report["errors"]))


class AuthorCapabilityEntrypoints(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.engine = caesura_build.find_engine()
        cls.lua = caesura_build.find_lua()

    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="caesura-author-capability-")
        self.addCleanup(self.scratch.cleanup)
        self.directory = Path(self.scratch.name)
        self.project = self.directory / "作者 项目"
        self.project.mkdir()
        self.story = self.project / "story.ks"
        self.story.write_text('[p]\n[end]\n', encoding="utf-8")

    def creator(self, *args):
        env = dict(os.environ, CAESURA_ENGINE=str(self.engine), CAESURA_LUA=str(self.lua))
        return subprocess.run([sys.executable, str(ROOT / "scripts/caesura.py"), *map(str, args)],
                              cwd=self.directory, env=env, capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=60)

    def test_check_entrypoint_uses_project_declaration_from_an_external_cwd(self):
        self.story.write_text('[video file="opening.mpg"]\n[p]\n', encoding="utf-8")
        (self.project / 'caesura.project.json').write_text(
            '{"capabilities":{"optional":["video.play"]}}', encoding="utf-8")
        report = self.directory / "result.json"
        result = self.creator("check", self.story, "--target", "web", "--json-output", report)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertTrue(data["passed"])
        self.assertTrue(data["warnings"])

    def test_build_and_package_cannot_skip_required_capabilities_or_touch_existing_output(self):
        self.story.write_text('[vfx type="blur" time=10]\n[p]\n', encoding="utf-8")
        output = self.directory / "previous-output"
        output.mkdir()
        sentinel = output / "user-kept.txt"
        sentinel.write_bytes(b"retain existing output")
        for command in ("build", "package"):
            for skip in (False, True):
                with self.subTest(command=command, skip=skip):
                    args = [command, self.project, "--out", output, "--engine", self.engine]
                    if skip: args.append("--skip-check")
                    result = self.creator(*args)
                    self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                    self.assertIn("render.blur", result.stdout + result.stderr)
                    self.assertEqual(list(output.iterdir()), [sentinel])
                    self.assertEqual(sentinel.read_bytes(), b"retain existing output")

    def test_changed_checked_inputs_are_rejected_before_assembly_acquires_output(self):
        metadata = self.project / "caesura.project.json"
        metadata.write_text('{}', encoding="utf-8")
        for mutation in ("scene", "new-scene", "declaration"):
            with self.subTest(mutation=mutation):
                report = caesura_build.run_capability_check(self.project, [self.story], "native", engine=self.engine)
                output = self.directory / ("previous-" + mutation)
                output.mkdir()
                (output / "BUILD-INFO.json").write_text('{"kind":"caesura-game-only"}', encoding="utf-8")
                sentinel = output / "retained.txt"
                sentinel.write_bytes(b"prior complete output")
                extra = self.project / "new.ks"
                if mutation == "scene": self.story.write_text('[video file="opening.mp4"]\n', encoding="utf-8")
                elif mutation == "new-scene": extra.write_text('[vfx type="blur"]\n', encoding="utf-8")
                else: metadata.write_text('{"capabilities":{"required":["kag.live2d_motion"]}}', encoding="utf-8")
                try:
                    with self.assertRaisesRegex(caesura_build.BuildError, "inputs changed"):
                        caesura_build._assemble_clean(self.project, self.story, self.engine, output,
                            shared_assets=False, dev_mode=False, capabilities=report)
                    self.assertEqual(sentinel.read_bytes(), b"prior complete output")
                    self.assertTrue((output / "BUILD-INFO.json").is_file())
                finally:
                    self.story.write_text('[p]\n[end]\n', encoding="utf-8")
                    metadata.write_text('{}', encoding="utf-8")
                    extra.unlink(missing_ok=True)

    def test_native_checks_reject_stale_source_catalog_even_when_binary_and_generated_data_match(self):
        engine_root = self.directory / "tool-copy"
        shutil.copytree(ROOT / "scripts", engine_root / "scripts")
        (engine_root / "config").mkdir()
        source = (ROOT / "config/runtime-capabilities.json").read_text(encoding="utf-8")
        (engine_root / "config/runtime-capabilities.json").write_text(source + "\n", encoding="utf-8")
        with mock.patch.object(caesura_build, "ROOT", engine_root):
            with self.assertRaisesRegex(caesura_build.BuildError, "catalog is stale"):
                caesura_build.run_capability_check(self.project, [self.story], "native", engine=self.engine)

    def test_actual_copied_runtime_and_project_cannot_inherit_an_earlier_pass(self):
        (self.project / "caesura.project.json").write_text('{}', encoding="utf-8")
        original_copy = caesura_build._copy_tree
        for mutation in ("runtime", "project"):
            with self.subTest(mutation=mutation):
                report = caesura_build.run_capability_check(self.project, [self.story], "native", engine=self.engine)
                output = self.directory / ("new-copy-" + mutation)
                changed = []
                def corrupt_owned_copy(source, destination):
                    original_copy(source, destination)
                    if mutation == "runtime" and source.resolve() == (ROOT / "scripts").resolve():
                        with (destination / "backend.lua").open("a", encoding="utf-8") as file:
                            file.write("\n-- changed after preflight\n")
                        changed.append("runtime")
                    if mutation == "project" and source.resolve() == self.project.resolve():
                        (destination / "caesura.project.json").write_text(
                            '{"capabilities":{"required":["kag.live2d_motion"]}}', encoding="utf-8")
                        changed.append("project")
                with mock.patch.object(caesura_build, "_copy_tree", side_effect=corrupt_owned_copy):
                    with self.assertRaisesRegex(caesura_build.BuildError, "Copied " + mutation + " differs"):
                        caesura_build._assemble_clean(self.project, self.story, self.engine, output,
                            shared_assets=False, dev_mode=False, quiet=True, capabilities=report)
                self.assertEqual(changed, [mutation])
                self.assertFalse(output.exists(), "failed owned assembly must not retain a successful output")

    def test_manifests_and_real_copy_share_the_existing_development_file_filter(self):
        (self.project / "entry.lua").write_text('-- authored dynamic entry\n', encoding="utf-8")
        for directory in ("node_modules", ".git", "__pycache__"):
            ignored = self.project / directory
            ignored.mkdir()
            (ignored / "helper.lua").write_text('-- development dependency\n', encoding="utf-8")
            (ignored / "unshipped.ks").write_text('[vfx type="blur"]\n', encoding="utf-8")
        scenes = caesura_build.collect_scenes(self.project)
        self.assertEqual(scenes, [self.story])
        report = caesura_build.run_capability_check(self.project, scenes, "native", engine=self.engine)
        self.assertTrue(report["passed"])
        self.assertIn("entry.lua", report["inputs"]["project_files"])
        self.assertFalse(any("node_modules" in key for key in report["inputs"]["project_files"]))
        copy = self.directory / "actual-copy"
        caesura_build._copy_tree(self.project, copy)
        self.assertEqual(caesura_build._project_capability_files(copy), report["inputs"]["project_files"])
        self.assertEqual(caesura_build._runtime_capability_files(copy), {"entry.lua": report["inputs"]["project_files"]["entry.lua"]})

    def test_packaged_profile_does_not_copy_optional_host_provenance(self):
        profile = caesura_build.native_capability_profile(self.engine)
        profile["binary"] = r"C:\private-host\bin\CaesuraAmeKAG.exe"
        profile["bundle_files"] = {"/private-host/runtime/path": "0" * 64}
        report = caesura_build._packaged_capability_report({"profile": profile}, self.project)
        self.assertNotIn("private-host", json.dumps(report))
        self.assertNotIn("bundle_files", report["profile"])
        self.assertEqual(report["profile"]["binary"], "CaesuraAmeKAG.exe")
        self.assertEqual(report["profile"]["binary_sha256"], profile["binary_sha256"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
