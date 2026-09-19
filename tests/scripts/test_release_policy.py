"""Policy preparation file regressions; source identity is a controlled fixture."""
from __future__ import annotations

import hashlib
import contextlib
import io
import json
from pathlib import Path
import sys
import os
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/"scripts"))
import prepare_release_policy as policy


class PolicyTests(unittest.TestCase):
    def setUp(self):
        temporary=tempfile.TemporaryDirectory(prefix="caesura-policy-")
        self.addCleanup(temporary.cleanup)
        self.base=Path(temporary.name).resolve()
        self.repo=self.base/"repo";self.repo.mkdir();(self.repo/".git").mkdir();(self.repo/"scripts").mkdir()
        self.template_path=self.repo/"scripts/release_input_policy.json"
        self.template=json.loads((ROOT/"scripts/release_input_policy.json").read_text(encoding="utf-8"))
        self.profiles={"profiles":{f"{platform}-{configuration.lower()}":{"platform":platform,"configuration":configuration}
            for platform in ("windows","linux","macos") for configuration in ("Debug","Release")}}
        self.cmake=self.repo/"CMakeLists.txt"
        self.cmake.write_text("project(CaesuraAmeKAG VERSION 1.2.3 LANGUAGES C CXX)\n")
        self.identity={"source_sha":"a"*40,"dirty":False,"worktree_fingerprint":"b"*64}
        self.output=self.base/"frozen-policy.json"
        self.write()

    def write(self):
        self.template_path.write_text(json.dumps(self.template),encoding="utf-8")
        (self.repo/"scripts/validation_profiles.json").write_text(json.dumps(self.profiles),encoding="utf-8")

    def call(self,**changes):
        args=dict(repo=self.repo,output=self.output,source_sha="a"*40)
        args.update(changes)
        with patch.object(policy,"_source_identity",return_value=self.identity):
            return policy.prepare_policy(**args)

    def test_freezes_exact_source_bytes_version_and_all_input_roles(self):
        result=self.call()
        raw=self.output.read_bytes();value=json.loads(raw)
        self.assertEqual(result["policy_sha256"],hashlib.sha256(raw).hexdigest())
        self.assertEqual(json.loads(result["policy_json"]),value)
        self.assertEqual(value["source_sha"],"a"*40)
        self.assertEqual(len(value["artifact_roles"]),10)
        self.assertEqual(value["version"],"1.2.3")
        self.assertIn("CaesuraAmeKAG-1.2.3-Web.zip",value["package_inputs"]["web-package"]["required_files"])
        self.assertEqual(value["source_files"]["CMakeLists.txt"],hashlib.sha256(self.cmake.read_bytes()).hexdigest())
        self.assertFalse(result["release_ready"])

    def test_failed_or_existing_output_is_not_overwritten(self):
        self.output.write_bytes(b"first failure evidence")
        with self.assertRaises(FileExistsError):self.call()
        self.assertEqual(self.output.read_bytes(),b"first failure evidence")

    def test_dirty_or_different_source_refused(self):
        self.identity["dirty"]=True
        with self.assertRaisesRegex(ValueError,"clean"):self.call()
        self.identity["dirty"]=False
        with self.assertRaisesRegex(ValueError,"source"):self.call(source_sha="f"*40)
        self.assertFalse(self.output.exists())

    def test_missing_required_job_or_input_cannot_silently_reduce_scope(self):
        self.template["required_jobs"].pop("ios-compile");self.write()
        with self.assertRaisesRegex(ValueError,"required"):self.call()

    def test_profile_platform_or_configuration_must_match_lane(self):
        self.profiles["profiles"]["linux-release"]["configuration"]="Debug";self.write()
        with self.assertRaisesRegex(ValueError,"profile"):self.call()

    def test_unknown_placeholders_and_escaping_paths_are_refused(self):
        files=self.template["package_inputs"]["web-package"]["required_files"]
        for name in ("{unknown}.zip","../escape.zip","a/b.zip"):
            with self.subTest(name=name):
                files.clear();files[name]="file";self.write()
                with self.assertRaises(ValueError):self.call()

    def test_duplicate_job_names_and_output_prefixes_refused(self):
        self.template["required_jobs"]["ios-compile"]=self.template["required_jobs"]["android-static"];self.write()
        with self.assertRaisesRegex(ValueError,"distinct"):self.call()

    def test_multiple_engine_versions_or_tag_mismatch_refused(self):
        with self.assertRaisesRegex(ValueError,"tag"):self.call(release_tag="v8.0.0")
        self.cmake.write_text(self.cmake.read_text()*2)
        with self.assertRaisesRegex(ValueError,"version"):self.call()

    def cli(self,github_output):
        output=io.StringIO()
        with patch.object(policy,"_source_identity",return_value=self.identity), contextlib.redirect_stdout(output):
            code=policy.main(["--repo",str(self.repo),"--source-sha","a"*40,
                "--output",str(self.output),"--github-output",str(github_output)])
        return code,output.getvalue()

    def test_cli_output_cannot_alias_policy(self):
        code,stdout=self.cli(self.output)
        self.assertEqual(code,1,stdout)
        self.assertFalse(self.output.exists())

    def test_cli_output_cannot_mutate_source(self):
        original=self.cmake.read_bytes()
        code,stdout=self.cli(self.cmake)
        self.assertEqual(code,1,stdout)
        self.assertFalse(self.output.exists())
        self.assertEqual(self.cmake.read_bytes(),original)

    def test_cli_refuses_hardlinked_github_output(self):
        alias=self.base/"linked-output"
        os.link(self.cmake,alias)
        original=self.cmake.read_bytes()
        code,stdout=self.cli(alias)
        self.assertEqual(code,1,stdout)
        self.assertEqual(self.cmake.read_bytes(),original)
        self.assertFalse(self.output.exists())

    def test_cli_separate_output_preserves_exact_json_bytes(self):
        target=self.base/"job-output"
        code,stdout=self.cli(target)
        self.assertEqual(code,0,stdout)
        result=json.loads(stdout)
        self.assertEqual(result["policy_sha256"],hashlib.sha256(self.output.read_bytes()).hexdigest())
        self.assertEqual(json.loads(self.output.read_bytes())["version"],"1.2.3")
        self.assertIn(result["policy_sha256"],target.read_text(encoding="utf-8"))


if __name__=="__main__":unittest.main(verbosity=2)
