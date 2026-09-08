#!/usr/bin/env python3
"""Explicit build-product selection; sentinel interpreters prove dispatch only."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import caesura_build

BASH = None


class OutputPaths(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="caesura-output-paths-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "隔离 构建"
        self.root.mkdir()

    def binary(self, relative, content=b"selected fixture"):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    def run_python(self, script, args, environment):
        return subprocess.run(
            [sys.executable, str(script), *args], cwd=ROOT, env=environment,
            capture_output=True, text=True, encoding="utf-8", errors="replace",
            timeout=90,
        )

    def test_engine_override_is_authoritative_even_with_a_stale_default(self):
        stale = self.binary("build/Release/" + caesura_build._exe_name(), b"stale")
        selected = self.binary("当前 输出/" + caesura_build._exe_name())
        with patch.object(caesura_build, "ROOT", self.root):
            with patch.dict(os.environ, {"CAESURA_ENGINE": str(selected)}):
                self.assertEqual(caesura_build.find_engine(), selected.resolve())
                self.assertNotEqual(caesura_build.find_engine(), stale)
            for invalid in (str(selected.parent / "missing"), ""):
                with self.subTest(invalid=invalid):
                    with patch.dict(os.environ, {"CAESURA_ENGINE": invalid}):
                        with self.assertRaisesRegex(caesura_build.BuildError, "CAESURA_ENGINE"):
                            caesura_build.find_engine()

    def test_lua_override_is_authoritative_even_with_packaged_and_path_fallbacks(self):
        self.binary("external/lua/lua.exe", b"stale")
        self.binary("external/lua/lua", b"stale")
        selected = self.binary("当前 输出/lua interpreter")
        with patch.object(caesura_build, "ROOT", self.root):
            with patch.object(caesura_build.shutil, "which", side_effect=AssertionError("PATH probe")):
                with patch.dict(os.environ, {"CAESURA_LUA": str(selected)}):
                    self.assertEqual(Path(caesura_build.find_lua()), selected.resolve())
                for invalid in (str(selected.parent / "missing"), str(selected.parent), ""):
                    with self.subTest(invalid=invalid):
                        with patch.dict(os.environ, {"CAESURA_LUA": invalid}):
                            with self.assertRaisesRegex(caesura_build.BuildError, "CAESURA_LUA"):
                                caesura_build.find_lua()

    def test_unconfigured_manual_binary_discovery_remains_available(self):
        engine = self.binary("build/Release/" + caesura_build._exe_name())
        lua = self.binary("external/lua/lua.exe")
        with patch.object(caesura_build, "ROOT", self.root), patch.dict(os.environ):
            os.environ.pop("CAESURA_ENGINE", None)
            os.environ.pop("CAESURA_LUA", None)
            self.assertEqual(caesura_build.find_engine(), engine)
            self.assertEqual(Path(caesura_build.find_lua()), lua)

    def test_real_cli_fails_for_invalid_configured_paths_including_precompile(self):
        engine = self.binary("当前 输出/" + caesura_build._exe_name())
        missing = str(engine.parent / "missing product")
        cases = (("CAESURA_ENGINE", []), ("CAESURA_LUA", []),
                 ("CAESURA_LUA", ["--skip-check"]))
        for index, (name, options) in enumerate(cases):
            with self.subTest(name=name, options=options):
                environment = os.environ.copy()
                environment["CAESURA_ENGINE"] = str(engine)
                environment[name] = missing
                out = self.root / ("game-" + str(index))
                result = self.run_python(
                    ROOT / "scripts/caesura.py",
                    ["build", "basic", "-o", str(out), *options], environment,
                )
                log = result.stdout + result.stderr
                self.assertEqual(result.returncode, 1, log)
                self.assertIn(name, log)
                self.assertNotIn("Traceback", log)
                self.assertFalse(out.exists(), "failed configuration left a partial game")

    def test_cli_gate_rejects_invalid_configured_paths_instead_of_skipping(self):
        engine = self.binary("当前 输出/" + caesura_build._exe_name())
        for name in ("CAESURA_ENGINE", "CAESURA_LUA"):
            with self.subTest(name=name):
                environment = os.environ.copy()
                environment["CAESURA_ENGINE"] = str(engine)
                environment[name] = str(engine.parent / "missing product")
                result = self.run_python(
                    ROOT / "tests/scripts/test_caesura_build_cli.py", [], environment,
                )
                log = result.stdout + result.stderr
                self.assertEqual(result.returncode, 1, log)
                self.assertIn(name, log)
                self.assertNotIn("SKIPPED", log)
                self.assertNotIn("Traceback", log)

    def golden_fixture(self):
        script = self.root / "scripts/verify_golden_vn.sh"
        script.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / "scripts/verify_golden_vn.sh", script)
        self.binary("tests/projects/golden_vn/story.ks", b"[end]\n")
        stale = self.binary("external/lua/lua", b"#!/bin/sh\necho STALE_LUA_PROBE\nexit 1\n")
        stale.chmod(0o755)
        selected = self.binary("当前 输出/selected lua", b"#!/bin/sh\necho SELECTED_LUA_PROBE\nexit 1\n")
        selected.chmod(0o755)
        return script, selected

    def run_golden(self, script, explicit):
        self.assertIsNotNone(BASH, "invoke this suite with --bash <exact executable>")
        environment = os.environ.copy()
        environment.pop("CAESURA_LUA", None)
        if explicit is not None:
            environment["CAESURA_LUA"] = explicit
        return subprocess.run(
            [BASH, script.as_posix()], cwd=self.root, env=environment,
            capture_output=True, text=True, encoding="utf-8", errors="replace",
            timeout=30,
        )

    def test_golden_executes_the_explicit_unicode_space_path(self):
        script, selected = self.golden_fixture()
        result = self.run_golden(script, str(selected))
        log = result.stdout + result.stderr
        # These are intentionally failing sentinel programs, not fake Golden PASS.
        self.assertEqual(result.returncode, 1, log)
        self.assertIn("SELECTED_LUA_PROBE", log)
        self.assertNotIn("STALE_LUA_PROBE", log)

    def test_golden_invalid_explicit_path_fails_before_any_fallback_or_gate(self):
        script, selected = self.golden_fixture()
        for invalid in (str(selected.parent / "missing"), str(selected.parent), ""):
            with self.subTest(invalid=invalid):
                result = self.run_golden(script, invalid)
                log = result.stdout + result.stderr
                self.assertEqual(result.returncode, 1, log)
                self.assertIn("FATAL", log)
                self.assertIn("CAESURA_LUA", log)
                self.assertNotIn("STALE_LUA_PROBE", log)
                self.assertNotIn("Step 1:", log)

    def test_golden_rejects_every_nonzero_contract_checker_exit(self):
        script, selected = self.golden_fixture()
        selected.write_bytes(b"#!/bin/sh\necho SELECTED_LUA_PROBE\nexit 42\n")
        result = self.run_golden(script, str(selected))
        log = result.stdout + result.stderr
        self.assertEqual(result.returncode, 1, log)
        self.assertRegex(log, r"FAIL\s+ks_check: clean contract")
        self.assertNotRegex(log, r"PASS\s+ks_check: clean contract")

    def test_unconfigured_golden_keeps_its_manual_probe(self):
        script, _ = self.golden_fixture()
        result = self.run_golden(script, None)
        log = result.stdout + result.stderr
        self.assertEqual(result.returncode, 1, log)
        self.assertIn("STALE_LUA_PROBE", log)
        self.assertNotIn("SELECTED_LUA_PROBE", log)

    def branch_probe_interpreter(self, *, clicks, exit_code, progress):
        script, selected = self.golden_fixture()
        lines = ["#!/bin/sh", f'echo "RESULT DONE:200 token=122 clicks={clicks}"']
        if progress:
            lines.append('echo "BRANCH_PROGRESS route=$SAMPLE_BRANCH_ROUTE text=true"')
        lines.append(f"exit {exit_code}")
        selected.write_bytes(("\n".join(lines) + "\n").encode("utf-8"))
        return self.run_golden(script, str(selected))

    def test_golden_branch_accepts_observed_progress_without_timing_click_counts(self):
        result = self.branch_probe_interpreter(clicks=47, exit_code=0, progress=True)
        log = result.stdout + result.stderr
        # A sentinel proves only verifier dispatch/result handling. The actual
        # scene and driver still execute in the separate CaesuraGoldenVn gate.
        for route in ("forest", "city"):
            self.assertRegex(log, rf"PASS\s+route_{route} branch reachable -> DONE")

    def test_golden_branch_rejects_hollow_completion_even_with_many_clicks(self):
        result = self.branch_probe_interpreter(clicks=500, exit_code=0, progress=False)
        log = result.stdout + result.stderr
        for route in ("forest", "city"):
            self.assertRegex(log, rf"FAIL\s+route_{route} branch reachable -> DONE")
            self.assertNotRegex(log, rf"PASS\s+route_{route} branch reachable -> DONE")

    def test_golden_branch_preserves_nonzero_exit_after_a_done_marker(self):
        result = self.branch_probe_interpreter(clicks=500, exit_code=42, progress=True)
        log = result.stdout + result.stderr
        for route in ("forest", "city"):
            self.assertRegex(log, rf"FAIL\s+route_{route} branch reachable -> DONE")
            self.assertNotRegex(log, rf"PASS\s+route_{route} branch reachable -> DONE")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bash", required=True)
    options = parser.parse_args()
    if not Path(options.bash).is_file():
        parser.error("--bash must name the configured Bash executable")
    BASH = str(Path(options.bash).resolve())
    unittest.main(argv=[sys.argv[0]], verbosity=2)
