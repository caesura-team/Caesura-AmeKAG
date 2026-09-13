#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
test_caesura_build_cli.py — CLI contract tests for "caesura build" / "caesura package"
(scripts/caesura_build.py, wired into scripts/caesura.py).

Designed to run in the normal gate as the ctest test CaesuraBuildCli
(registered in tests/CMakeLists.txt — see the add_test block under the
Python3_EXECUTABLE guard at the end of that file); the repo CI does not
otherwise execute tests/*.py. NOTE: the registration must exist for this
statement to be true — this file never registers itself.

CTest supplies exact CAESURA_ENGINE / CAESURA_LUA paths for the active build.
Invalid configured paths fail; the selected paths propagate to every real CLI
subprocess, including scene validation and package precompilation.

Unconfigured manual SKIP semantics: the engine binary lives under
build/ | bin/, which are gitignored — it is NOT a repo invariant. Everything
that can be verified WITHOUT a binary (error paths, project resolution,
argument parsing, the asset scanner) always runs. The build/package cases and
the ks_check gating case skip when no binary exists (caesura build resolves
the engine BEFORE running ks_check, so the gate is only reachable with an
engine present); the runner then exits 77 and ctest reports SKIP.

Exit precedence is FAIL (1) > SKIP (77) > PASS (0). A green tick must never
mean "packaging verified" on a host that never packaged anything: reporting
PASS while the engine-dependent half silently skipped is exactly the
"skip counted as a pass" miscount this suite exists to prevent.

Layers under test are the REAL ones: every case shells out to
'python scripts/caesura.py ...' exactly as a creator would. No handler is
called directly, so a broken argparse wiring or a broken import cannot pass.
"""

import filecmp
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
CLI = ROOT / "scripts" / "caesura.py"
SKIP_EXIT = 77

sys.path.insert(0, str(ROOT / "scripts"))
import caesura_build  # noqa: E402


def run_cli(*args, timeout=900):
    """Invoke the real CLI. Returns (rc, combined output)."""
    res = subprocess.run(
        [sys.executable, str(CLI)] + [str(a) for a in args],
        cwd=str(ROOT), capture_output=True, text=True,
        encoding="utf-8", errors="replace", timeout=timeout,
    )
    return res.returncode, (res.stdout or "") + (res.stderr or "")


def engine_available():
    try:
        return caesura_build.find_engine()
    except caesura_build.BuildError as error:
        if "CAESURA_ENGINE" in os.environ:
            raise SystemExit("Configured build CLI engine is invalid: %s" % error)
        return None


ENGINE = engine_available()
if "CAESURA_LUA" in os.environ:
    try:
        caesura_build.find_lua()
    except caesura_build.BuildError as error:
        raise SystemExit("Configured build CLI Lua interpreter is invalid: %s" % error)


class TestErrorPaths(unittest.TestCase):
    """Actionable diagnostics, never a traceback (no engine binary required)."""

    def test_help_lists_build_and_package(self):
        rc, out = run_cli("--help")
        self.assertEqual(rc, 0, out)
        self.assertIn("build", out)
        self.assertIn("package", out)

    def test_missing_project_is_actionable(self):
        rc, out = run_cli("build", "definitely_not_a_project_9f3a")
        self.assertEqual(rc, 1, out)
        self.assertIn("Project not found", out)
        self.assertIn("Searched", out)
        self.assertIn("caesura.py create", out)      # tells the user what to do
        self.assertNotIn("Traceback", out)

    def test_bad_engine_path_is_actionable(self):
        with tempfile.TemporaryDirectory() as td:
            rc, out = run_cli("build", "basic", "--engine", os.path.join(td, "nope"))
        self.assertEqual(rc, 1, out)
        self.assertIn("--engine", out)
        self.assertNotIn("Traceback", out)

    def test_project_without_scenes_is_actionable(self):
        with tempfile.TemporaryDirectory() as td:
            rc, out = run_cli("build", td)
        self.assertEqual(rc, 1, out)
        self.assertIn("No .ks scenes", out)
        self.assertNotIn("Traceback", out)

    @unittest.skipIf(ENGINE is None, "no engine binary: caesura build resolves the "
                            "engine before ks_check, so the gate is only reachable with an engine")
    def test_ks_check_failure_blocks_and_quotes_the_violation(self):
        with tempfile.TemporaryDirectory() as td:
            # [playbgm] with neither file= nor storage= is a real contract
            # violation (scripts/ks_check.lua), not a synthetic marker.
            (Path(td) / "story.ks").write_text(
                '*start\n[playbgm volume=0.5]\n[ch name="A" text="hi"]\n[end]\n',
                encoding="utf-8")
            rc, out = run_cli("build", td)
        self.assertEqual(rc, 1, out)
        self.assertIn("ks_check", out)
        self.assertIn("playbgm", out)                # the actual violation text
        self.assertIn("--skip-check", out)           # the escape hatch
        self.assertNotIn("Traceback", out)

    def test_engine_missing_diagnostic_shape(self):
        """The no-engine message must name the search list AND the two fixes."""
        try:
            caesura_build.find_engine(explicit=None, config="NoSuchConfig")
        except caesura_build.BuildError as e:
            msg = str(e)
            self.assertIn("Engine binary not found", msg)
            self.assertIn("Searched", msg)
            self.assertIn("CAESURA_ENGINE", msg)
            self.assertIn("cmake --build", msg)
            self.assertIn("doctor", msg)
        else:
            # An engine exists on this host, so the failure branch cannot be
            # exercised end-to-end. Assert that fact rather than pretending the
            # branch was covered.
            self.assertIsNotNone(ENGINE)


class TestEntryAndWebForwarding(unittest.TestCase):
    """Real CLI entry resolution and the actual Python-to-Node argv boundary."""

    def build_entry(self, directory, names, requested):
        project = Path(directory) / "作品 中文 空格"
        project.mkdir()
        for name in names:
            scene = project / name
            scene.parent.mkdir(parents=True, exist_ok=True)
            scene.write_text("[end]\n", encoding="utf-8")
        # A missing engine is an intentional downstream boundary: successful
        # entry selection must reach its diagnostic; invalid entry must not.
        return run_cli("build", project, "--entry", requested,
                       "--engine", project / "not-an-engine.exe")

    def test_entry_does_not_match_an_arbitrary_filename_suffix(self):
        with tempfile.TemporaryDirectory() as directory:
            rc, out = self.build_entry(directory, ["prefixcustom.ks"], "custom.ks")
        self.assertEqual(rc, 1, out)
        self.assertIn("--entry scene not found", out)
        self.assertNotIn("--engine", out)

    def test_entry_rejects_an_ambiguous_bare_basename(self):
        with tempfile.TemporaryDirectory() as directory:
            rc, out = self.build_entry(directory, ["第一章/opening.ks", "第二章/opening.ks"], "opening.ks")
        self.assertEqual(rc, 1, out)
        self.assertIn("ambiguous", out)
        self.assertIn("第一章/opening.ks", out)
        self.assertIn("第二章/opening.ks", out)
        self.assertNotIn("--engine", out)

    def test_entry_normalizes_windows_project_relative_separators(self):
        with tempfile.TemporaryDirectory() as directory:
            rc, out = self.build_entry(directory, ["story.ks", "章节 一/custom.ks"], "章节 一\\custom.ks")
        self.assertEqual(rc, 1, out)
        self.assertIn("--engine", out)
        self.assertNotIn("--entry scene not found", out)

    def test_entry_retains_unique_basename_compatibility(self):
        with tempfile.TemporaryDirectory() as directory:
            rc, out = self.build_entry(directory, ["story.ks", "章节 一/custom.ks"], "custom.ks")
        self.assertEqual(rc, 1, out)
        self.assertIn("--engine", out)
        self.assertNotIn("--entry scene not found", out)

    def test_web_wrapper_forwards_selected_entry_and_project_asset_source(self):
        node = caesura_build.find_node()
        with tempfile.TemporaryDirectory(prefix="u21-forward-") as directory:
            project = Path(directory) / "作品 中文 空格"
            (project / "章节 一").mkdir(parents=True)
            (project / "assets").mkdir()
            (project / "story.ks").write_text("[end]\n", encoding="utf-8")
            (project / "章节 一/custom.ks").write_text("[end]\n", encoding="utf-8")
            observation = Path(directory) / "node-argv.json"
            observer = Path(directory) / "observe-node.mjs"
            observer.write_text("""import {writeFileSync} from 'node:fs';
if ((process.argv[1] || '').replaceAll('\\\\','/').endsWith('/package_game.mjs')) {
  writeFileSync(process.env.U21_FORWARD_ARGS, JSON.stringify(process.argv.slice(2)));
  process.exit(23);
}
""", encoding="utf-8")
            env = dict(os.environ, CAESURA_NODE=str(node),
                       NODE_OPTIONS="--import=" + observer.as_uri(), U21_FORWARD_ARGS=str(observation))
            result = subprocess.run([sys.executable, str(CLI), "package", str(project),
                                     "--target", "web", "--entry", "章节 一/custom.ks",
                                     "--skip-check", "--out", str(Path(directory) / "output")],
                                    cwd=ROOT, env=env, capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", timeout=30)
            self.assertEqual(result.returncode, 23, result.stdout + result.stderr)
            forwarded = json.loads(observation.read_text(encoding="utf-8"))
            self.assertIn("--skip-check", forwarded)
            self.assertIn("--entry", forwarded)
            self.assertEqual(forwarded[forwarded.index("--entry") + 1].replace("\\", "/"), "章节 一/custom.ks")
            self.assertIn("--assets", forwarded)
            self.assertEqual(Path(forwarded[forwarded.index("--assets") + 1]).resolve(), (project / "assets").resolve())


class TestAssetScanner(unittest.TestCase):
    """The reference scanner decides whether a package ships holes."""

    def test_scans_storage_sprite_file_attributes(self):
        with tempfile.TemporaryDirectory() as td:
            s = Path(td) / "a.ks"
            s.write_text(
                '[bg storage="assets/bg/x.png"]\n'
                '[ch sprite="assets/fg/y.png" text="hi"]\n'
                '[video file="assets/mov/z.mp4"]\n'
                '[bg storage="assets/bg/x.png"]\n'          # duplicate
                '[bg storage="../escape.png"]\n',           # traversal: rejected
                encoding="utf-8")
            refs = caesura_build.scan_asset_refs([s])
        self.assertEqual(refs, ["assets/bg/x.png", "assets/fg/y.png", "assets/mov/z.mp4"])

    def test_runtime_computed_paths_are_not_reported_as_missing(self):
        """A macro-parameter path (demo/example_game/story.ks:16) is not static."""
        with tempfile.TemporaryDirectory() as td:
            s = Path(td) / "a.ks"
            s.write_text('[bg storage="assets/bg/%bg%"]\n'
                         '[bg storage="assets/bg/real.png"]\n', encoding="utf-8")
            static = caesura_build.scan_asset_refs([s])
            dynamic = caesura_build.scan_dynamic_asset_refs([s])
        self.assertEqual(static, ["assets/bg/real.png"])
        self.assertEqual(dynamic, ["assets/bg/%bg%"])

    def test_missing_reference_is_reported_not_swallowed(self):
        with tempfile.TemporaryDirectory() as td:
            proj = Path(td) / "p"
            out = Path(td) / "o"
            proj.mkdir()
            out.mkdir()
            copied, missing = caesura_build.resolve_referenced_assets(
                ["assets/bg/classroom.png", "assets/bg/__no_such_asset__.png"], proj, out)
        self.assertIn("assets/bg/classroom.png", copied)     # pulled from repo pool
        self.assertIn("assets/bg/__no_such_asset__.png", missing)


class TestCreateCommand(unittest.TestCase):
    """caesura create: template resolution (no engine binary needed)."""

    def test_showcase_fallback_resolves_from_non_repo_cwd(self):
        """N4 regression: the legacy demo/example_game fallback must anchor on
        the CLI script, not the CWD.

        Reproduce with a minimal extracted-CLI tree in a temp dir and a CWD
        that is NOT the repo root: the old CWD-relative
        os.path.join('demo','example_game') could never find the template
        there and 'create' failed with "template 'showcase' not found".
        """
        with tempfile.TemporaryDirectory() as td:
            pkg = Path(td) / "pkg"
            (pkg / "scripts").mkdir(parents=True)
            shutil.copy2(ROOT / "scripts" / "caesura.py", pkg / "scripts" / "caesura.py")
            shutil.copy2(ROOT / "scripts" / "caesura_build.py", pkg / "scripts" / "caesura_build.py")
            shutil.copytree(ROOT / "demo" / "example_game", pkg / "demo" / "example_game")
            workdir = Path(td) / "somewhere-else"
            workdir.mkdir()
            res = subprocess.run(
                [sys.executable, str(pkg / "scripts" / "caesura.py"),
                 "create", "myproj", "--template", "showcase"],
                cwd=str(workdir), capture_output=True, text=True,
                encoding="utf-8", errors="replace", timeout=300)
            joined = res.stdout + res.stderr
            self.assertEqual(res.returncode, 0,
                             "showcase fallback must work from a non-repo CWD\n" + joined)
            self.assertNotIn("Traceback", joined)
            created = workdir / "myproj"
            self.assertTrue((created / "story.ks").is_file(),
                            "myproj/story.ks missing:\n" +
                            "\n".join(sorted(p.name for p in created.iterdir()))
                            if created.exists() else "myproj was not created")

    def test_create_posts_meta_name_basename_and_iso_dates(self):
        """M1-L: default metadata name = target dir basename; created ==
        modified is a valid ISO-8601 UTC timestamp; template untouched."""
        with tempfile.TemporaryDirectory() as td:
            target = Path(td) / "demo_x"
            rc, out = run_cli("create", str(target), "--template", "blank")
            self.assertEqual(rc, 0, "create failed\n" + out)
            meta = json.loads((target / "caesura.project.json").read_text(encoding="utf-8"))
            self.assertEqual(meta["name"], "demo_x", "name must be target basename")
            self.assertEqual(meta["template"], "blank", "template field must survive")
            datetime.strptime(meta["created"], "%Y-%m-%dT%H:%M:%SZ")
            datetime.strptime(meta["modified"], "%Y-%m-%dT%H:%M:%SZ")
            self.assertEqual(meta["created"], meta["modified"], "created==modified")

    def test_create_posts_meta_name_override(self):
        """M1-L: --name overrides the metadata name; template still intact."""
        with tempfile.TemporaryDirectory() as td:
            target = Path(td) / "demo_y"
            rc, out = run_cli("create", str(target), "--template", "blank",
                              "--name", "My Studio Project")
            self.assertEqual(rc, 0, "create failed\n" + out)
            meta = json.loads((target / "caesura.project.json").read_text(encoding="utf-8"))
            self.assertEqual(meta["name"], "My Studio Project",
                             "--name must override the basename")
            self.assertEqual(meta["template"], "blank",
                             "template field must not be rewritten")

    def test_create_posts_meta_description_only_when_given(self):
        """M1-L: description stays empty without --description and is stored
        with it; JSON is written with indent (not a single line)."""
        with tempfile.TemporaryDirectory() as td:
            target = Path(td) / "demo_z"
            rc, out = run_cli("create", str(target), "--template", "blank",
                              "--description", "A demo VN with \u6c49\u8bed\u6587\u672c")
            self.assertEqual(rc, 0, "create failed\n" + out)
            pj = target / "caesura.project.json"
            meta = json.loads(pj.read_text(encoding="utf-8"))
            self.assertEqual(meta["description"], "A demo VN with \u6c49\u8bed\u6587\u672c",
                             "--description must be stored")
            text = pj.read_text(encoding="utf-8")
            self.assertTrue(text.endswith("\n"), "json must end with newline")
            self.assertIn('  "name":', text, "json must be indented")


@unittest.skipIf(ENGINE is None, "no engine binary (build/ is gitignored)")
class TestPrecompileEvidence(unittest.TestCase):
    """Real CLI, packaged Lua compiler, and disk artifacts with I/O faults."""

    def _build_with_lua_prefix(self, root, prefix, project="basic"):
        out = root / "game"
        driver = root / "precompile_fault_driver.py"
        driver.write_text(
            "import sys\n"
            "sys.path.insert(0, %r)\n" % str(ROOT / "scripts") +
            "import caesura_build\n"
            "caesura_build.PRECOMPILE_LUA = %r + caesura_build.PRECOMPILE_LUA\n"
            % prefix.replace("%", "%%") +
            "sys.argv = ['caesura.py', 'build', %r, '-o', sys.argv[1]]\n" % str(project) +
            "import caesura\n"
            "caesura.main()\n", encoding="utf-8")
        result = subprocess.run([sys.executable, str(driver), str(out)],
                                cwd=str(ROOT), capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=900)
        return result.returncode, result.stdout + result.stderr, out

    def test_cache_write_failure_preserves_source_fallback_without_claiming_precompile(self):
        prefix = '''local real_open = io.open
io.open = function(path, mode)
    if mode == "w" and path:match("%.ksc$") then return nil, "injected disk full" end
    return real_open(path, mode)
end
'''
        with tempfile.TemporaryDirectory() as td:
            rc, log, out = self._build_with_lua_prefix(Path(td), prefix)
            self.assertEqual(rc, 0, log)
            info = json.loads((out / "BUILD-INFO.json").read_text(encoding="utf-8"))
            self.assertEqual(info["precompiled_scenes"], [], info)
            self.assertEqual(info["precompile"]["status"], "partial", info)
            self.assertEqual(info["precompile"]["scene_count"], 0, info)
            self.assertEqual(len(info["precompile_failures"]), len(info["scenes"]), info)
            self.assertTrue((out / "projects/basic/story.ks").is_file())
            self.assertEqual(list((out / "cache/ksc").glob("*.ksc")), [])

    def test_incompatible_written_cache_refuses_package_and_cleans_output(self):
        # Change the serialized compatibility identity at the file-write
        # boundary. Tokenization, compilation, and disk read remain real.
        mutations = {
            "compiler-semantics-mismatch": 'data.compatibility.semantics = "u14-other-runtime"',
            "cache-format-mismatch": 'data.version = -1',
            "command-contract-mismatch": 'local cmd = next(data.compatibility.commands); '
                                         'data.compatibility.commands[cmd] = "u14-other-contract"',
            "source-hash-mismatch": 'data._srcHash = "u14-other-source"',
        }
        for reason, mutation in mutations.items():
            with self.subTest(reason=reason), tempfile.TemporaryDirectory() as td:
                prefix = '''local real_open = io.open
io.open = function(path, mode)
    local file, err = real_open(path, mode)
    if file and mode == "w" and path:match("%.ksc$") then
        return {
            write = function(_, text)
                local data = assert(load(text, "=test-written-cache", "t", {}))()
                MUTATION
                return file:write("return " .. require("kag.compiler").encode_lua_literal(data))
            end,
            close = function() return file:close() end,
        }
    end
    return file, err
end
'''.replace("MUTATION", mutation)
                rc, log, out = self._build_with_lua_prefix(Path(td), prefix)
                self.assertNotEqual(rc, 0, log)
                self.assertIn("incompatible", log.lower())
                self.assertIn(reason, log)
                self.assertNotIn("Traceback", log)
                self.assertFalse(out.exists(), "incompatible package must not survive\n" + log)

    def test_incompatible_returned_tokens_are_rejected_even_with_valid_disk_cache(self):
        # Corrupt only the live token identity after the real cache writer
        # completes. A disk-only check would incorrectly accept this scene.
        prefix = '''package.path = "scripts/?.lua;scripts/?/init.lua;" .. package.path
local compiler = require("kag.compiler")
local real_write = compiler.writeCache
compiler.writeCache = function(tokens, path)
    local written = real_write(tokens, path)
    tokens._compiled.compatibility.semantics = "u14-incompatible-live-tokens"
    return written
end
'''
        with tempfile.TemporaryDirectory() as td:
            rc, log, out = self._build_with_lua_prefix(Path(td), prefix)
            self.assertNotEqual(rc, 0, log)
            self.assertIn("compiled tokens: compiler-semantics-mismatch", log)
            self.assertFalse(out.exists(), log)

    def test_partial_cache_write_is_optional_and_never_successful_precompile(self):
        prefix = '''local real_open = io.open
io.open = function(path, mode)
    local file, err = real_open(path, mode)
    if file and mode == "w" and path:match("%.ksc$") then
        return {
            write = function(_, text)
                file:write(text:sub(1, 12))
                return nil, "injected disk full after partial write"
            end,
            close = function() return file:close() end,
        }
    end
    return file, err
end
'''
        with tempfile.TemporaryDirectory() as td:
            rc, log, out = self._build_with_lua_prefix(Path(td), prefix)
            self.assertEqual(rc, 0, log)
            info = json.loads((out / "BUILD-INFO.json").read_text(encoding="utf-8"))
            self.assertEqual(info["precompiled_scenes"], [], info)
            self.assertEqual(info["precompile"]["status"], "partial", info)
            self.assertEqual(len(info["precompile_failures"]), len(info["scenes"]), info)
            self.assertTrue((out / "projects/basic/story.ks").is_file())
            self.assertTrue(list((out / "cache/ksc").glob("*.ksc")))

    def test_stdout_success_without_verifying_inputs_is_not_precompile_evidence(self):
        prefix = 'print("PRECOMPILE-OK projects/basic/story.ks tokens=999")\nos.exit(0)\n'
        with tempfile.TemporaryDirectory() as td:
            rc, log, out = self._build_with_lua_prefix(Path(td), prefix)
            self.assertEqual(rc, 0, log)
            info = json.loads((out / "BUILD-INFO.json").read_text(encoding="utf-8"))
            self.assertEqual(info["precompiled_scenes"], [], info)
            self.assertEqual(info["precompile"]["status"], "partial", info)
            self.assertEqual(len(info["precompile_failures"]), len(info["scenes"]), info)

    def test_early_exit_keeps_verified_scene_but_accounts_for_every_untested_input(self):
        prefix = '''local real_open = io.open
io.open = function(path, mode)
    if mode == "w" and path:match("_story%.ksc$") then os.exit(0) end
    return real_open(path, mode)
end
'''
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            project = root / "space game"
            project.mkdir()
            for name in ("first chapter.ks", "story.ks"):
                (project / name).write_text("*start\n[end]\n", encoding="utf-8")
            rc, log, out = self._build_with_lua_prefix(root, prefix, project)
            self.assertEqual(rc, 0, log)
            info = json.loads((out / "BUILD-INFO.json").read_text(encoding="utf-8"))
            self.assertEqual(info["precompiled_scenes"], ["projects/space game/first chapter.ks"], info)
            self.assertEqual(info["precompile"]["status"], "partial", info)
            self.assertEqual(info["precompile"]["scene_count"], 1, info)
            self.assertEqual(len(info["precompile_failures"]), 1, info)
            self.assertIn("projects/space game/story.ks", info["precompile_failures"][0])


@unittest.skipIf(ENGINE is None, "no engine binary (build/ is gitignored)")
class TestGameOnlyBuild(unittest.TestCase):
    """Full build against the real engine binary and the stock basic template."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="caesura-build-test-")
        cls.out = Path(cls.tmp) / "basic-game"
        cls.rc, cls.log = run_cli("build", "basic", "-o", str(cls.out))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_build_succeeds(self):
        self.assertEqual(self.rc, 0, self.log)

    def test_engine_and_runtime_libs_are_beside_the_binary(self):
        exe = self.out / caesura_build._exe_name()
        self.assertTrue(exe.is_file(), self.log)
        self.assertTrue(filecmp.cmp(exe, ENGINE, shallow=False),
                        "package did not copy the selected engine binary")
        if os.name == "nt":
            # A player must not copy DLLs by hand: SDL3 has to be in the folder.
            self.assertTrue((self.out / "SDL3.dll").is_file(),
                            sorted(p.name for p in self.out.iterdir()))

    def test_engine_required_assets_present(self):
        # No font => no text at all (src/entry/Engine.cpp:366).
        fonts = list((self.out / "assets" / "fonts").glob("*.otf"))
        self.assertTrue(fonts, "packaged assets/fonts is empty")
        self.assertTrue((self.out / "assets" / "lang").is_dir())

    def test_game_lives_under_the_sandbox_allowlisted_root(self):
        # scripts/sandbox.lua:314-316 allowlists projects/ for post-lockdown
        # io.open; a game outside it cannot cross-scene [jump].
        self.assertTrue((self.out / "projects" / "basic" / "story.ks").is_file())

    def test_runtime_config_points_at_the_generated_boot_shim(self):
        cfg = (self.out / "scripts" / "config.lua").read_bytes()
        self.assertIn(b"projects/basic/caesura-boot.lua", cfg)
        self.assertIn(b"config.dev_mode = false", cfg)      # release default

    def test_boot_shim_installs_frame_hooks(self):
        # tools/project_templates/*/entry.lua define NO engine_update /
        # engine_render, so without these the story loads and never advances.
        shim = (self.out / "projects" / "basic" / "caesura-boot.lua").read_text(encoding="utf-8")
        for hook in ("engine_update", "engine_render", "_KAG_onClick"):
            self.assertIn(hook, shim)

    def test_referenced_assets_resolved(self):
        info = json.loads((self.out / "BUILD-INFO.json").read_text(encoding="utf-8"))
        self.assertEqual(info["kind"], "caesura-game-only")
        self.assertEqual(info["assets_missing"], [], info)
        self.assertGreater(info["asset_refs"], 0)
        for ref in caesura_build.scan_asset_refs(
                sorted((self.out / "projects" / "basic").rglob("*.ks"))):
            self.assertTrue((self.out / ref).is_file(), "missing packaged asset: %s" % ref)

    def test_scenes_are_precompiled_into_the_ksc_cache(self):
        # A cold compile of a large scene blows the 20,000,000-instruction
        # startup budget (src/script/vm/LuaManager.cpp:63) -- reproduced with
        # demo/example_game/story.ks, which only booted once cache/ksc existed.
        info = json.loads((self.out / "BUILD-INFO.json").read_text(encoding="utf-8"))
        self.assertEqual(info["precompile_failures"], [], info)
        cached = list((self.out / "cache" / "ksc").glob("*.ksc"))
        self.assertTrue(cached, "cache/ksc is empty: the player pays a cold compile at boot")
        self.assertEqual(len(info["precompiled_scenes"]), len(info["scenes"]), info)
        # N5: BUILD-INFO must carry an honest precompile record (status /
        # reason / scene_count) -- never a silent single-WARN skip.
        pc = info.get("precompile")
        self.assertIsNotNone(pc, info)
        self.assertEqual(set(pc.keys()), {"status", "reason", "scene_count"}, pc)
        self.assertIn(pc["status"], {"ok", "skipped", "partial"}, pc)
        self.assertIsInstance(pc["reason"], str, pc)
        self.assertEqual(pc["scene_count"], len(info["precompiled_scenes"]), pc)

    def test_no_developer_tooling_shipped(self):
        stray = [p.name for p in (self.out / "scripts").rglob("*")
                 if p.is_file() and p.suffix.lower() in caesura_build.DEV_SCRIPT_SUFFIXES]
        self.assertEqual(stray, [], "dev-only files leaked into the player package")

    def test_no_stray_dash_p_directory_in_package(self):
        """t24 sentinel: compiler.lua's Windows 'mkdir -p' defect was fixed in
        b38ac5de (Windows branch is 'if not exist ... mkdir'). A literal '-p'
        directory anywhere in the assembled output would mean the defect is
        back or an old scripts tree got packaged -- the caesura_build.py
        cleanup workaround was removed on this guarantee."""
        stray = [p for p in self.out.rglob("-p")]
        self.assertEqual(stray, [], "literal '-p' directory in package: "
                                    "re-check scripts/kag/compiler.lua mkdir")

    def test_package_does_not_reference_the_repo(self):
        """Nothing inside the package may point back at the source checkout.

        N3 upgrade: scan EVERY text file in the assembled output (including
        BUILD-INFO.json) for the checkout root under BOTH slash forms -- the
        old engine_source leaked the build machine's absolute path into the
        player package, and a two-file probe would have missed it.
        """
        needle_fwd = str(ROOT).replace("\\", "/").lower()
        needle_bwd = str(ROOT).replace("/", "\\").lower()
        offenders, scanned = [], 0
        for p in self.out.rglob("*"):
            if not p.is_file():
                continue
            raw = p.read_bytes()
            if b"\x00" in raw:
                continue                       # binaries: exe/dll/fonts/ksc/...
            scanned += 1
            text = raw.decode("utf-8", errors="replace").lower()
            if needle_fwd in text.replace("\\", "/") or needle_bwd in text.replace("/", "\\"):
                offenders.append(p.relative_to(self.out).as_posix())
        self.assertGreater(scanned, 20, "text-file scan did not cover the package")
        self.assertEqual(offenders, [])

    def test_failed_mid_build_cleans_up_and_same_out_reruns(self):
        """t19/A2: a failure AFTER assemble() owns the output must not leave a
        half-written directory that blocks the same -o forever.

        The S2 guard refuses any non-empty dir without BUILD-INFO.json, so a
        build that crashed before that file was written used to leave exactly
        that state, permanently. Inject a real disk-class IO error (raised by
        shutil.copy2 after the engine binary was copied) through a driver that
        runs the REAL argparse -> cmd_build -> assemble chain; then re-run the
        real CLI to the same -o and expect success.
        """
        with tempfile.TemporaryDirectory() as td:
            out = Path(td) / "game"
            driver = Path(td) / "t19_fault_driver.py"
            driver.write_text(
                "import sys\n"
                "sys.path.insert(0, %r)\n" % str(ROOT / "scripts") +
                "import caesura_build\n"
                "_real_copy2 = caesura_build.shutil.copy2\n"
                "_count = {'n': 0}\n"
                "def _flaky(src, dst, *a, **k):\n"
                "    _count['n'] += 1\n"
                "    if _count['n'] == 2:\n"
                "        # fail right after the engine binary (marker) was copied\n"
                "        raise OSError('t19: injected disk error after engine copy')\n"
                "    return _real_copy2(src, dst, *a, **k)\n"
                "caesura_build.shutil.copy2 = _flaky\n"
                "sys.argv = ['caesura.py', 'build', 'basic', '-o', sys.argv[1]]\n"
                "import caesura\n"
                "caesura.main()\n",
                encoding="utf-8")
            res = subprocess.run([sys.executable, str(driver), str(out)],
                                 cwd=str(ROOT), capture_output=True, text=True,
                                 encoding="utf-8", errors="replace", timeout=900)
            joined = res.stdout + res.stderr
            self.assertNotEqual(res.returncode, 0, joined)
            self.assertNotIn("Traceback", joined)          # clean error, not a dump
            self.assertIn("t19: injected disk error", joined)
            self.assertFalse(out.exists(),
                             "mid-build failure left a partial output behind:\n" + joined)
            rc2, log2 = run_cli("build", "basic", "-o", str(out))
            self.assertEqual(rc2, 0, log2)
            self.assertTrue((out / "BUILD-INFO.json").is_file(), log2)

    def test_out_guard_names_failed_build_residue(self):
        """t19/(b): a crashed build's residue (marker files, no BUILD-INFO.json)
        gets a message that says so and tells the user what to do -- not the
        identical 'must be from caesura build' refusal."""
        with tempfile.TemporaryDirectory() as td:
            residue = Path(td) / "game"
            residue.mkdir()
            (residue / "CaesuraAmeKAG.exe").write_bytes(b"MZ-fake-marker")
            rc, out = run_cli("build", "basic", "-o", str(residue))
            self.assertEqual(rc, 1, out)
            self.assertIn("FAILED before BUILD-INFO.json", out)
            self.assertIn("Delete it", out)
            self.assertNotIn("not a previous", out)
            # the residue is NOT touched (S2 guarantee)
            self.assertTrue((residue / "CaesuraAmeKAG.exe").is_file(), out)

    def test_out_guard_refuses_unrelated_dir_without_touching_it(self):
        """t19/(b): an unrelated user directory (no caesura markers) is refused
        with the original wording and left COMPLETELY untouched."""
        with tempfile.TemporaryDirectory() as td:
            unrelated = Path(td) / "my-notes"
            unrelated.mkdir()
            (unrelated / "notes.txt").write_text("hello, world", encoding="utf-8")
            rc, out = run_cli("build", "basic", "-o", str(unrelated))
            self.assertEqual(rc, 1, out)
            self.assertIn("not a previous", out)
            self.assertNotIn("FAILED before BUILD-INFO.json", out)
            self.assertEqual(
                (unrelated / "notes.txt").read_text(encoding="utf-8"), "hello, world", out)


@unittest.skipIf(ENGINE is None, "no engine binary (build/ is gitignored)")
class TestDesktopPackage(unittest.TestCase):
    """caesura package --target windows produces a self-contained archive."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="caesura-pkg-test-")
        cls.rc, cls.log = run_cli("package", "basic", "--target", "windows", "-o", cls.tmp)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_archive_created(self):
        self.assertEqual(self.rc, 0, self.log)
        zips = list(Path(self.tmp).glob("*.zip"))
        self.assertEqual(len(zips), 1, [p.name for p in Path(self.tmp).iterdir()])

    def test_archive_has_single_top_level_dir_with_engine_and_game(self):
        zips = list(Path(self.tmp).glob("*.zip"))
        self.assertTrue(zips, self.log)
        with zipfile.ZipFile(zips[0]) as zf:
            names = zf.namelist()
        tops = {n.split("/")[0] for n in names}
        self.assertEqual(len(tops), 1, tops)              # never unzips into CWD
        self.assertTrue(any(n.endswith(caesura_build._exe_name()) for n in names))
        self.assertTrue(any("projects/basic/story.ks" in n for n in names))
        self.assertTrue(any("assets/fonts/" in n for n in names))


if __name__ == "__main__":
    suite = unittest.TestLoader().loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    ran = result.testsRun
    skipped = len(result.skipped)
    failed = len(result.failures) + len(result.errors)
    print("")
    print("=" * 70)
    print("caesura build/package CLI: %d run, %d passed, %d failed, %d skipped"
          % (ran, ran - failed - skipped, failed, skipped))
    if ENGINE is None:
        print("engine binary: NOT FOUND -> build/package cases SKIPPED "
              "(build/ and bin/ are gitignored; not a repo invariant)")
    else:
        print("engine binary: %s" % ENGINE)
    print("=" * 70)
    if failed:
        sys.exit(1)
    # SKIP semantics: with no engine binary, the engine-dependent half of the
    # suite (TestGameOnlyBuild, TestDesktopPackage, ks_check gating) did not
    # run. Report SKIP (77) to ctest, never PASS -- a green tick must not mean
    # "packaging verified" on a host that never packaged anything. (The old
    # predicate 'ran - skipped == 0' could not trip in a partial-skip run: the
    # engine-independent cases always execute, so a skipped engine half was
    # counted as a pass.)
    if ENGINE is None:
        print("caesura build/package CLI: %d passed, %d skipped; reporting SKIP "
              "(77) -- no engine binary on this host" % (ran - failed - skipped, skipped))
        sys.exit(SKIP_EXIT)
    if skipped:
        print("NOTE: %d case(s) skipped." % skipped)
    print("ALL CAESURA BUILD CLI TESTS PASSED")
    sys.exit(0)
