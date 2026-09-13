#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
test_package_game_node_cli.py — CLI contract tests for scripts/package_game.mjs
(t179). Direct run:  python tests/scripts/test_package_game_node_cli.py

Covers:
  1. full default path (vite rebuild if present) with a single .ks input
  2. --no-web-build fast path (reuses web/dist; skips if absent)
  3. unknown option -> rc=1 with FATAL in output

All runs use --out dist/<unique>; the artifact is removed in tearDown.
"""

import os
import json
import tempfile
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NODE = shutil.which("node")
FIRST_VN_KS = "tests/projects/first_vn/story.ks"


def run_cli_full(*args, timeout=300, env=None):
    cmd = [NODE, "scripts/package_game.mjs", *args]
    # The repo root sits under a non-ASCII path (D:\...\文件存放处\...) and the
    # mjs FATAL messages echo it as UTF-8 bytes; decode as UTF-8 with a
    # replacement fallback instead of the console GBK codec (t193).
    proc = subprocess.run(
        cmd, cwd=str(ROOT), capture_output=True, text=True,
        encoding="utf-8", errors="replace", timeout=timeout, env=env
    )
    return proc.returncode, proc.stdout, proc.stderr


class PackageGameCliTest(unittest.TestCase):
    out_dir = None

    def setUp(self):
        import uuid
        self.out_name = "dist/t179-cli-" + uuid.uuid4().hex[:10]
        self.out_path = ROOT / self.out_name

    def tearDown(self):
        if self.out_path.exists():
            shutil.rmtree(self.out_path, ignore_errors=True)

    def test_01_full_package_single_ks(self):
        """Default path: single .ks input -> full package with expected tree."""
        if NODE is None:
            self.skipTest("node not found on PATH")
        rc, out, err = run_cli_full(
            "--out", self.out_name, FIRST_VN_KS
        )
        self.assertEqual(rc, 0, "rc=%s stdout=%s stderr=%s" % (rc, out[-500:], err[-500:]))
        self.assertIn("PACKAGE COMPLETE", out)
        self.assertIn("Step 1/5: ks_check", out)
        self.assertTrue((self.out_path / "index.html").exists())
        self.assertTrue((self.out_path / "cache" / "story" / "story.lua").exists())
        self.assertTrue((self.out_path / "demo" / "first_vn" / "story.ks").exists())
        self.assertTrue((self.out_path / "MANIFEST.txt").exists())
        self.assertTrue((self.out_path / "scripts" / "index.json").exists())
        self.assertTrue((self.out_path / "assets").is_dir())
        manifest = (self.out_path / "MANIFEST.txt").read_text(encoding="utf-8")
        self.assertIn("total KB:", manifest)
        self.assertIn("first_vn", manifest)
        self.assertIn("files (size bytes, path):", manifest)

    def test_02_no_web_build_fast_path(self):
        """--no-web-build reuses web/dist (skip when gitignored web/dist absent)."""
        if NODE is None:
            self.skipTest("node not found on PATH")
        if not (ROOT / "web" / "dist" / "index.html").exists():
            self.skipTest("web/dist/index.html absent; --no-web-build needs it")
        rc, out, err = run_cli_full(
            "--no-web-build", "--out", self.out_name, FIRST_VN_KS
        )
        self.assertEqual(rc, 0, "rc=%s stdout=%s stderr=%s" % (rc, out[-500:], err[-500:]))
        self.assertIn("PACKAGE COMPLETE", out)
        self.assertTrue((self.out_path / "index.html").exists())
        self.assertTrue((self.out_path / "cache" / "story" / "story.lua").exists())
        self.assertTrue((self.out_path / "MANIFEST.txt").exists())
        self.assertTrue((self.out_path / "scripts" / "index.json").exists())

    def test_03_unknown_option_fatal(self):
        """Unknown option -> rc=1 and FATAL line."""
        if NODE is None:
            self.skipTest("node not found on PATH")
        rc, out, err = run_cli_full("--bogus-option")
        self.assertEqual(rc, 1)
        self.assertTrue("FATAL" in (out + err), "stdout=%s stderr=%s" % (out, err))

    def test_04_out_escape_guard(self):
        """t186 A2: --out escaping the repo root must rc=1, no deletion outside.

        '..' traversal ('--out ../t193-escape-<uuid>') and root-identity
        ('--out dist/..') are both denied by the OUT_PATH guard; assert the
        sibling directory was never created (no recursive delete of anything
        outside the repo).
        """
        if NODE is None:
            self.skipTest("node not found on PATH")
        import uuid
        esc = "t193-escape-" + uuid.uuid4().hex[:10]
        # '..' traversal: resolves to ROOT/../<esc> (outside the repo root)
        rc, out, err = run_cli_full("--no-web-build", "--out", "../" + esc, FIRST_VN_KS)
        self.assertEqual(rc, 1)
        self.assertTrue("FATAL" in (out + err), "stdout=%s stderr=%s" % (out, err))
        self.assertFalse((ROOT.parent / esc).exists(), "escape dir must not exist")
        # root-identity: resolves to ROOT itself -> denied
        rc2, out2, err2 = run_cli_full("--no-web-build", "--out", "dist/..", FIRST_VN_KS)
        self.assertEqual(rc2, 1)
        self.assertTrue("FATAL" in (out2 + err2), "stdout=%s stderr=%s" % (out2, err2))

    def test_05_final_copied_bundle_must_match_packaged_runtime(self):
        """A compatible staging artifact cannot bless a changed final copy."""
        if NODE is None:
            self.skipTest("node not found on PATH")
        (ROOT / 'tmp').mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='u14-web-copy-', dir=ROOT / 'tmp') as temp:
            injection = Path(temp) / 'copy-boundary.mjs'
            callee = Path(temp) / 'callee.ks'
            callee.write_text('[end]\n', encoding='utf-8')
            first_scene = Path(temp) / 'story.ks'
            shutil.copy2(ROOT / FIRST_VN_KS, first_scene)
            shutil.copy2(ROOT / 'tests/projects/first_vn/caesura.project.json',
                         Path(temp) / 'caesura.project.json')
            destination = self.out_path / 'cache/story/story.lua'
            injection.write_text('''import fs from 'node:fs';
import {resolve,dirname} from 'node:path';
import {syncBuiltinESMExports} from 'node:module';
const destination = %s;
const packageRoot = %s;
const copy = fs.copyFileSync;
const ownsPackage = (process.argv[1] || '').replaceAll('\\\\','/').endsWith('/package_game.mjs');
fs.copyFileSync = function(source,target,...args) {
  const result = copy(source,target,...args);
  if (ownsPackage && String(target).replaceAll('\\\\','/').endsWith('/cache/story/story.lua')) {
    console.log('U14_REAL_PACKAGE_COPY_MUTATED');
    if (process.env.U14_COPY_MUTATION==='missing-runtime') {
      const actualRoot=dirname(dirname(dirname(resolve(String(target)))));
      fs.unlinkSync(resolve(actualRoot,'scripts/kag.lua'));
      fs.unlinkSync(resolve(actualRoot,'scripts/kag/init.lua'));
      return result;
    }
    const text=fs.readFileSync(target,'utf8');
    const changed=process.env.U14_COPY_MUTATION==='missing-scene'
      ? 'local b=(function() '+text+' end)(); b.scenes["callee.ks"]=nil; return b'
      : text.replace('caesura-kag-2','injected-incompatible-runtime');
    if (changed===text) throw new Error('U14 fixture did not mutate copied semantics');
    fs.writeFileSync(target,changed,'utf8');
  }
  return result;
};
syncBuiltinESMExports();
''' % (json.dumps(str(destination)), json.dumps(str(self.out_path))), encoding='utf-8')
            for mutation, reason in (('semantics', 'compiler-semantics-mismatch'),
                                     ('missing-scene', 'missing-bundle-scene:callee.ks'),
                                     ('missing-runtime', "module 'kag' not found")):
                with self.subTest(mutation=mutation):
                    # Each mutation starts from a fresh test-owned delivery;
                    # a failed prior copy deliberately has no package marker.
                    if self.out_path.exists():
                        self.assertTrue(self.out_path.resolve().is_relative_to((ROOT / 'dist').resolve()))
                        self.assertTrue(self.out_path.name.startswith('t179-cli-'))
                        shutil.rmtree(self.out_path)
                    env = dict(os.environ, NODE_OPTIONS='--import=' + injection.as_uri(), U14_COPY_MUTATION=mutation)
                    rc, out, err = run_cli_full('--out', self.out_name, first_scene.relative_to(ROOT).as_posix(),
                                                callee.relative_to(ROOT).as_posix(), env=env)
                    self.assertEqual(rc, 1, out + err)
                    self.assertEqual(out.count('U14_REAL_PACKAGE_COPY_MUTATED'), 1, out + err)
                    self.assertIn(reason, out + err)
                    self.assertNotIn('PACKAGE COMPLETE', out)
                    self.assertFalse((self.out_path / 'MANIFEST.txt').exists())

    def test_06_colliding_basename_scenes_survive_the_final_package(self):
        """The real demo inputs contain two independent story.ks scenes."""
        if NODE is None:
            self.skipTest("node not found on PATH")
        scenes = ['demo/example_game/story.ks', 'demo/template/story.ks']
        rc, out, err = run_cli_full('--out', self.out_name, *scenes)
        self.assertEqual(rc, 0, out + err)
        self.assertIn('delivered bundle matches packaged runtime', out)
        self.assertIn('PACKAGE COMPLETE', out)
        bundle = (self.out_path / 'cache/story/story.lua').read_text(encoding='utf-8')
        for scene in scenes:
            key = scene.removeprefix('demo/')
            self.assertIn('["' + key + '"]=', bundle)
            copy = self.out_path / 'demo/example_game' / key
            self.assertTrue(copy.is_file(), str(copy))
            self.assertEqual(copy.read_bytes(), (ROOT / scene).read_bytes())
        manifest = (self.out_path / 'MANIFEST.txt').read_text(encoding='utf-8')
        self.assertIn('scenes: 2', manifest)

    def test_07_final_runtime_spawn_failure_reports_os_error(self):
        """A real OS spawn failure must retain its cause in the package log."""
        if NODE is None:
            self.skipTest("node not found on PATH")
        (ROOT / 'tmp').mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='u14-runtime-spawn-', dir=ROOT / 'tmp') as temp:
            injection = Path(temp) / 'spawn-boundary.mjs'
            missing_lua = Path(temp) / 'missing-lua-interpreter.exe'
            copied_bundle = Path(temp) / 'actual-staged-story.lua'
            injection.write_text('''import child from 'node:child_process';
import fs from 'node:fs';
import {join} from 'node:path';
import {syncBuiltinESMExports} from 'node:module';
const spawn = child.spawnSync;
child.spawnSync = function(command,args,options) {
  if (args?.[0]==='-e' && args[1]?.includes('PACKAGE-SCENE-KEYS:')) {
    fs.copyFileSync(join(options.cwd,'cache/story/story.lua'), %s);
    command = %s;
  }
  return spawn(command,args,options);
};
syncBuiltinESMExports();
''' % (json.dumps(str(copied_bundle)), json.dumps(str(missing_lua))), encoding='utf-8')
            env = dict(os.environ, NODE_OPTIONS='--import=' + injection.as_uri())
            rc, out, err = run_cli_full('--out', self.out_name, FIRST_VN_KS, env=env)
            self.assertEqual(rc, 1, out + err)
            # Preserve the real-copy oracle at the actual verifier boundary;
            # a failed transaction must not publish that staged copy at --out.
            self.assertTrue(copied_bundle.is_file())
            self.assertFalse(self.out_path.exists())
            self.assertIn('runtime verifier failed', out + err)
            self.assertIn('ENOENT', out + err)
            self.assertIn('missing-lua-interpreter.exe', out + err)
            self.assertIn('status=null', out + err)
            self.assertIn('signal=none', out + err)
            self.assertNotIn('PACKAGE COMPLETE', out)
            self.assertFalse((self.out_path / 'MANIFEST.txt').exists())

    def test_08_unicode_output_retains_the_packaged_lua_runtime(self):
        """The final runtime must exist at the requested Unicode output path."""
        if NODE is None:
            self.skipTest("node not found on PATH")
        self.out_name += '-故事输出-🧪(50%)'
        self.out_path = ROOT / self.out_name
        rc, out, err = run_cli_full('--out', self.out_name, FIRST_VN_KS)
        self.assertEqual(rc, 0, out + err)
        self.assertIn('PACKAGE COMPLETE', out)
        for file in ['scripts/kag.lua', 'scripts/kag/init.lua', 'scripts/kag/compiler.lua']:
            self.assertEqual((self.out_path / file).read_bytes(), (ROOT / file).read_bytes())
        self.assertEqual((self.out_path / 'demo/first_vn/story.ks').read_bytes(),
                         (ROOT / FIRST_VN_KS).read_bytes())
        self.assertTrue((self.out_path / 'MANIFEST.txt').is_file())

    def test_09_directory_copy_preserves_unicode_bytes_and_overwrites(self):
        """The production copy helper handles fresh and merged Unicode trees."""
        if NODE is None:
            self.skipTest("node not found on PATH")
        (ROOT / 'tmp').mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='u14-unicode-copy-', dir=ROOT / 'tmp') as temp:
            source = Path(temp) / '源目录🧪'
            destination = Path(temp) / '故事输出'
            nested = source / '子目录'
            nested.mkdir(parents=True)
            (source / '空目录').mkdir()
            original = b'\x00\x01\xffUTF8-path-binary\r\n'
            (source / 'kag.lua').write_bytes(b'return 1\n')
            (nested / '故事.bin').write_bytes(original)
            driver = '''const {copyDirectorySync}=await import(process.argv[1]);
copyDirectorySync(process.argv[2],process.argv[3]);'''

            def copy(target=destination):
                proc = subprocess.run([NODE, '--input-type=module', '-e', driver,
                                       (ROOT / 'scripts/copy_tree.mjs').as_uri(), str(source), str(target)],
                                      cwd=ROOT, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=30)
                self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

            copy()
            self.assertEqual((destination / '子目录/故事.bin').read_bytes(), original)
            self.assertEqual((destination / 'kag.lua').read_bytes(), b'return 1\n')
            self.assertTrue((destination / '空目录').is_dir())
            (destination / 'keep.txt').write_bytes(b'existing unrelated file')
            (source / 'kag.lua').write_bytes(b'return 2\n')
            (nested / '故事.bin').write_bytes(original + b'updated')
            copy()
            self.assertEqual((destination / 'kag.lua').read_bytes(), b'return 2\n')
            self.assertEqual((destination / '子目录/故事.bin').read_bytes(), original + b'updated')
            self.assertEqual((destination / 'keep.txt').read_bytes(), b'existing unrelated file')
            for mode in ['readonly', 'hardlink']:
                with self.subTest(overwrite=mode):
                    target = Path(temp) / ('覆盖目标-' + mode)
                    target.mkdir()
                    old_file = target / 'kag.lua'
                    other = Path(temp) / 'unrelated-hardlink.bin'
                    if mode == 'readonly':
                        old_file.write_bytes(b'readonly old contents')
                        old_file.chmod(0o444)
                    else:
                        other.write_bytes(b'other hardlink contents must survive')
                        os.link(other, old_file)
                    copy(target)
                    self.assertEqual(old_file.read_bytes(), b'return 2\n')
                    self.assertEqual((source / 'kag.lua').read_bytes(), b'return 2\n')
                    self.assertEqual((nested / '故事.bin').read_bytes(), original + b'updated')
                    if mode == 'hardlink':
                        self.assertEqual(other.read_bytes(), b'other hardlink contents must survive')

    def test_10_directory_copy_rejects_recursive_destinations(self):
        """Copying into the source must fail before any output is created."""
        if NODE is None:
            self.skipTest("node not found on PATH")
        (ROOT / 'tmp').mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='u14-copy-recursion-', dir=ROOT / 'tmp') as temp:
            source = Path(temp) / '中文源'
            source.mkdir()
            original = b'original source must survive'
            (source / 'source.bin').write_bytes(original)
            driver = '''const {copyDirectorySync}=await import(process.argv[1]);
copyDirectorySync(process.argv[2],process.argv[3]);'''
            for destination in [source, source / 'nested-output']:
                with self.subTest(destination=str(destination)):
                    proc = subprocess.run([NODE, '--input-type=module', '-e', driver,
                                           (ROOT / 'scripts/copy_tree.mjs').as_uri(), str(source), str(destination)],
                                          cwd=ROOT, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=30)
                    self.assertNotEqual(proc.returncode, 0)
                    self.assertIn('Cannot copy a directory into itself', proc.stderr)
                    self.assertEqual((source / 'source.bin').read_bytes(), original)
                    self.assertFalse((source / 'nested-output').exists())

    def test_11_directory_copy_rejects_destination_parent_alias(self):
        """A real parent junction/symlink cannot route output into the source."""
        if NODE is None:
            self.skipTest("node not found on PATH")
        (ROOT / 'tmp').mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='u14-copy-parent-alias-', dir=ROOT / 'tmp') as temp:
            source = Path(temp) / '中文源'
            source.mkdir()
            original = b'original source must survive alias rejection'
            (source / 'source.bin').write_bytes(original)
            alias = Path(temp) / 'outside-alias'
            destination = alias / 'nested-output'
            # The alias and realpath checks use the real filesystem. A mutation
            # guard makes old code fail at its first mkdir instead of recursively
            # filling the source through the alias while establishing RED.
            driver = '''import fs from 'node:fs';
import {resolve} from 'node:path';
import {syncBuiltinESMExports} from 'node:module';
const [module,source,alias,destination]=process.argv.slice(1);
fs.symlinkSync(source,alias,process.platform==='win32'?'junction':'dir');
console.log('ALIAS_CREATED');
const mkdir=fs.mkdirSync;
let attempted=0;
fs.mkdirSync=function(path,...args) {
  if(resolve(String(path))===resolve(destination)) {
    ++attempted;
    throw new Error('fixture stopped unexpected destination mkdir');
  }
  return mkdir(path,...args);
};
syncBuiltinESMExports();
const {copyDirectorySync}=await import(module);
try { copyDirectorySync(source,destination); }
catch(error) { console.error(error.message); process.exitCode=1; }
console.log('destination-mkdir-attempts='+attempted);'''
            proc = subprocess.run([NODE, '--input-type=module', '-e', driver,
                                   (ROOT / 'scripts/copy_tree.mjs').as_uri(), str(source), str(alias), str(destination)],
                                  cwd=ROOT, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=30)
            self.assertIn('ALIAS_CREATED', proc.stdout, proc.stderr)
            self.assertNotEqual(proc.returncode, 0)
            self.assertIn('Cannot copy a directory into itself', proc.stderr)
            self.assertIn('destination-mkdir-attempts=0', proc.stdout)
            self.assertEqual((source / 'source.bin').read_bytes(), original)
            self.assertFalse((source / 'nested-output').exists())


class PackageLuaSelectionCliTest(unittest.TestCase):
    """Exercise the real Node CLI and a real Lua process, without a web build.

    LUA_INIT exits at the interpreter boundary before ks_check. Its stdout
    reports the executable name supplied by the actual Lua arg table. No probe
    function, filesystem lookup, spawn result, or interpreter is mocked.
    """

    @classmethod
    def setUpClass(cls):
        if NODE is None:
            raise unittest.SkipTest("node not found on PATH")
        sys.path.insert(0, str(ROOT / "scripts"))
        import caesura_build
        try:
            cls.lua_binary = Path(caesura_build.find_lua()).resolve()
        except caesura_build.BuildError as error:
            if "CAESURA_LUA" in os.environ:
                raise RuntimeError("Configured fixture interpreter is invalid: " + str(error)) from error
            raise unittest.SkipTest("real Lua fixture interpreter unavailable: " + str(error)) from error

    def setUp(self):
        (ROOT / "tmp").mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="u2-package-lua-", dir=ROOT / "tmp")
        self.addCleanup(self.temporary.cleanup)
        self.fixture = Path(self.temporary.name)
        for relative in ("scripts/package_game.mjs", "scripts/copy_tree.mjs",
                         "scripts/web_capability_profile.mjs", "web/lua-value.js",
                         "web/capability-catalog.js", "web/package.json",
                         "config/runtime-capabilities.json"):
            target = self.fixture / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, target)
        (self.fixture / "game.ks").write_text("[end]\n", encoding="utf-8")
        (self.fixture / "scripts/ks_check.lua").write_text(
            "error('LUA_INIT execution control must run first')\n", encoding="utf-8")
        self.path_dir = self.fixture / "path-lua"
        self.path_dir.mkdir()
        self.executable = "lua.exe" if os.name == "nt" else "lua"
        self.legacy = [self.fixture / "external/lua" / self.executable,
                       self.fixture / "build/lua/Release" / self.executable,
                       self.path_dir / self.executable]

    def plant(self, destination):
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(self.lua_binary, destination)
        return destination

    def cli(self, explicit=None):
        env = dict(os.environ)
        env.pop("NODE_OPTIONS", None)
        env.pop("CAESURA_LUA", None)
        if explicit is not None:
            env["CAESURA_LUA"] = explicit
        env["PATH"] = str(self.path_dir)
        control = r'''local exe=assert(arg[-1]):gsub("\\","/"):match("([^/]+)$")
io.write("U2-LUA-EXECUTED:"..exe..":".._VERSION.."\n")
os.exit(42)'''
        env["LUA_INIT"] = control
        env["LUA_INIT_5_4"] = control
        return subprocess.run([NODE, "scripts/package_game.mjs", "--no-web-build", "game.ks"],
                              cwd=self.fixture, env=env, capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=30)

    def test_12_explicit_lua_executes_before_packaged_release_and_path(self):
        """Spaces/Unicode in the authoritative path reach that exact binary."""
        for candidate in self.legacy:
            self.plant(candidate)
        name = "configured lua.exe" if os.name == "nt" else "configured lua"
        selected = self.plant(self.fixture / "Lua 解释器 (测试)" / name)
        for configured in (str(selected), str(selected.relative_to(self.fixture))):
            with self.subTest(configured=configured):
                result = self.cli(configured)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("U2-LUA-EXECUTED:" + name + ":Lua 5.4", result.stdout)
                self.assertEqual(result.stdout.count("U2-LUA-EXECUTED:"), 1)
                self.assertIn("required Web capabilities or scene contracts are not satisfied", result.stderr)
                self.assertNotIn("PACKAGE COMPLETE", result.stdout)

    def test_13_invalid_explicit_lua_never_falls_back(self):
        """Empty/missing/directory selections fail before a valid stale Lua runs."""
        for candidate in self.legacy:
            self.plant(candidate)
        directory = self.fixture / "configured directory"
        directory.mkdir()
        for configured in ("", str(self.fixture / "missing lua.exe"), str(directory)):
            with self.subTest(configured=configured):
                result = self.cli(configured)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("CAESURA_LUA does not point at a Lua interpreter", result.stderr)
                self.assertIn(configured or "<empty>", result.stderr)
                self.assertNotIn("U2-LUA-EXECUTED:", result.stdout)
                self.assertNotIn("Step 1/5", result.stdout)
                self.assertNotIn("PACKAGE COMPLETE", result.stdout)

    def test_14_unconfigured_legacy_lua_probe_remains_available(self):
        """Each legacy location still launches a real Lua when no override exists."""
        for candidate in self.legacy:
            with self.subTest(candidate=str(candidate.relative_to(self.fixture))):
                self.plant(candidate)
                result = self.cli()
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("U2-LUA-EXECUTED:" + self.executable + ":Lua 5.4", result.stdout)
                self.assertEqual(result.stdout.count("U2-LUA-EXECUTED:"), 1)
                self.assertIn("required Web capabilities or scene contracts are not satisfied", result.stderr)
                candidate.unlink()


class PackageWebCapabilitiesCliTest(unittest.TestCase):
    """Use the real Node packager, Lua preflight and a copy of the built player."""

    @classmethod
    def setUpClass(cls):
        if NODE is None:
            raise unittest.SkipTest("node not found on PATH")
        sys.path.insert(0, str(ROOT / "scripts"))
        import caesura_build
        cls.lua = caesura_build.find_lua()
        if not (ROOT / "web/dist/capabilities-build.json").is_file():
            raise RuntimeError("Build the real Web player before running packaging capability tests")

    def setUp(self):
        (ROOT / "tmp").mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="u19-node-capabilities-", dir=ROOT / "tmp")
        self.addCleanup(self.temp.cleanup)
        self.fixture = Path(self.temp.name)
        for directory in ("scripts", "config", "web"):
            shutil.copytree(ROOT / directory, self.fixture / directory,
                            ignore=shutil.ignore_patterns("node_modules", "__pycache__", "dist"))
        shutil.copytree(ROOT / "web/dist", self.fixture / "web/dist")
        for name in ("package.json", "package-lock.json", "npm-shrinkwrap.json"):
            if (ROOT / name).is_file():
                shutil.copy2(ROOT / name, self.fixture / name)
        (self.fixture / "assets").mkdir()
        self.project = self.fixture / "项目 空格"
        self.project.mkdir()
        (self.project / "story.ks").write_text("[end]\n", encoding="utf-8")
        self.out = self.fixture / "output"
        self.out.mkdir()
        self.original = b"previous deliverable must remain byte-identical"
        (self.out / "MANIFEST.txt").write_text("Caesura (AmeKAG) web package: old\n", encoding="utf-8")
        (self.out / "sentinel.bin").write_bytes(self.original)
        self.old_files = {p.name: p.read_bytes() for p in self.out.iterdir()}

    def declare(self, capabilities):
        (self.project / "caesura.project.json").write_text(
            json.dumps({"capabilities": capabilities}), encoding="utf-8")

    def cli(self, *args, extra_env=None):
        env = dict(os.environ, CAESURA_LUA=str(self.lua))
        env.pop("NODE_OPTIONS", None)
        env.update(extra_env or {})
        return subprocess.run([NODE, str(self.fixture / "scripts/package_game.mjs"),
                               "--no-web-build", "--out", str(self.out), *map(str, args)],
                              cwd=self.fixture, env=env, capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=60)

    def unchanged(self, result, reason):
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(reason, result.stdout + result.stderr)
        self.assertNotIn("PACKAGE COMPLETE", result.stdout)
        self.assertEqual({p.name for p in self.out.iterdir()}, set(self.old_files))
        self.assertEqual({p.name: p.read_bytes() for p in self.out.iterdir()}, self.old_files)

    def reprofile(self):
        driver = """import fs from 'node:fs';
const {createWebCapabilityProfile,collectWebSourceFiles}=await import(process.argv[1]);
fs.writeFileSync(process.argv[2]+'/capabilities-build.json',JSON.stringify(createWebCapabilityProfile(process.argv[2],{sourceFiles:collectWebSourceFiles()})));"""
        result = subprocess.run([NODE, "--input-type=module", "-e", driver,
                                 (self.fixture / "scripts/web_capability_profile.mjs").as_uri(),
                                 str(self.fixture / "web/dist")], cwd=self.fixture,
                                capture_output=True, text=True, encoding="utf-8", timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_required_declaration_survives_skip_check(self):
        self.declare({"required": ["video.play"]})
        for flags in ((), ("--skip-check",)):
            with self.subTest(flags=flags):
                self.unchanged(self.cli(*flags, self.project), "video.play")

    def test_nested_scene_is_checked(self):
        nested = self.project / "chapter"
        nested.mkdir()
        (nested / "opening.ks").write_text('[video file="opening.mpg"]\n[end]\n', encoding="utf-8")
        self.unchanged(self.cli("--skip-check", self.project), "video.play")

    def test_explicit_entry_outside_selected_files_is_checked(self):
        opening = self.project / "opening.ks"
        opening.write_text('[video file="opening.mpg"]\n[end]\n', encoding="utf-8")
        self.unchanged(self.cli("--skip-check", "--entry", opening, self.project / "story.ks"), "video.play")

    def test_invalid_html_rejects_before_replacing_previous_package(self):
        (self.fixture / "web/dist/index.html").write_text("<html><body>no head</body></html>", encoding="utf-8")
        self.reprofile()
        self.unchanged(self.cli(self.project), "metadata insertion point")

    def test_optional_feature_has_inspectable_delivered_report(self):
        self.out = self.fixture / "optional-fresh-output"
        self.declare({"optional": ["video.play"]})
        (self.project / "story.ks").write_text('[video file="opening.mpg"]\n[end]\n', encoding="utf-8")
        result = self.cli(self.project)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = json.loads((self.out / "CAPABILITIES.json").read_text(encoding="utf-8"))
        self.assertTrue(report["passed"])
        self.assertIn('"decision": "skip"', json.dumps(report))
        self.assertIn("self.__CAESURA_PROJECT_CAPABILITIES__", (self.out / "index.html").read_text(encoding="utf-8"))
        self.assertIn("story.ks", report["checked_inputs"])
        profile = json.loads((self.out / "capabilities-build.json").read_text(encoding="utf-8"))
        self.assertEqual(report["profile"], profile)
        import hashlib
        for file, digest in profile["bundle_files"].items():
            self.assertEqual(hashlib.sha256((self.out / file).read_bytes()).hexdigest(), digest, file)

    def test_modified_built_runtime_rejects_before_replacing_output(self):
        with (self.fixture / "web/dist/scripts/capability_runtime.lua").open("a", encoding="utf-8") as stream:
            stream.write("\n-- altered after profile\n")
        self.unchanged(self.cli(self.project), "after capability profiling changed")

    def test_source_runtime_changes_make_no_web_build_stale(self):
        with (self.fixture / "scripts/target_capabilities.lua").open("a", encoding="utf-8") as stream:
            stream.write("\n-- changed after selected Web build\n")
        self.unchanged(self.cli(self.project), "build source changed")

    def test_modified_service_worker_cannot_keep_the_previous_profile(self):
        worker = self.fixture / "web/dist/sw.js"
        self.assertTrue(worker.is_file(), "actual Vite fixture must include its Service Worker")
        with worker.open("a", encoding="utf-8") as stream:
            stream.write("\n// modified after profile\n")
        self.unchanged(self.cli(self.project), "after capability profiling changed: sw.js")

    def test_copied_player_mutation_cannot_receive_a_success_profile(self):
        injection = self.fixture / "mutate-copy.mjs"
        injection.write_text("""import fs from 'node:fs';
import {resolve} from 'node:path';
import {syncBuiltinESMExports} from 'node:module';
const target=process.env.U19_COPY_TARGET;
const copy=fs.copyFileSync;
fs.copyFileSync=function(source,destination,...args) {
  const value=copy(source,destination,...args);
  if(String(destination).replaceAll('\\\\','/').endsWith('/'+target)) {
    fs.appendFileSync(destination,'\\n'+process.env.U19_COPY_COMMENT+' changed final copy\\n');
    console.log('U19_REAL_COPY_MUTATED');
  }
  return value;
};
syncBuiltinESMExports();
""", encoding="utf-8")
        for file, comment in (("scripts/capability_runtime.lua", "--"), ("sw.js", "//")):
            with self.subTest(file=file):
                self.out = self.fixture / ("copied-fresh-" + file.replace("/", "-"))
                result = self.cli(self.project, extra_env={
                    "NODE_OPTIONS": "--import=" + injection.as_uri(),
                    "U19_COPY_TARGET": file, "U19_COPY_COMMENT": comment})
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("U19_REAL_COPY_MUTATED", result.stdout)
                self.assertIn("Copied Web player changed: " + file, result.stderr)
                self.assertNotIn("PACKAGE COMPLETE", result.stdout)
                self.assertFalse((self.out / "CAPABILITIES.json").exists())
                self.assertFalse((self.out / "MANIFEST.txt").exists())


class PackageEntryAssetsCliTest(unittest.TestCase):
    """Real Node/Lua packaging against the already built, profiled Web player."""

    @classmethod
    def setUpClass(cls):
        PackageWebCapabilitiesCliTest.setUpClass.__func__(cls)

    def setUp(self):
        PackageWebCapabilitiesCliTest.setUp(self)
        # This slice always creates a fresh package; replacement ownership and
        # ZIP transaction tests belong to their separate delivery workstream.
        self.out = self.fixture / "作品 成品"
        self.declare({})

    cli = PackageWebCapabilitiesCliTest.cli
    declare = PackageWebCapabilitiesCliTest.declare

    def packaged_entry(self):
        code = """package.path='scripts/?.lua;scripts/?/init.lua;'..package.path
require('kag')
local bundle=assert(loadfile('cache/story/story.lua','t',{}))()
assert(type(bundle.entry)=='string' and bundle.scenes[bundle.entry], 'missing-valid-bundle-entry')
local tokens,reason=require('kag.compiler').deserialize(bundle.scenes[bundle.entry])
assert(tokens,reason)
local text=''
for _,token in ipairs(tokens) do
  if token[1]=='ch' then text=text..tostring(token[2].text or '') end
end
io.write('U21-ENTRY:',bundle.entry,'\\nU21-TEXT:',text)
"""
        result = subprocess.run([str(self.lua), "-e", code], cwd=self.out,
                                capture_output=True, text=True, encoding="utf-8",
                                errors="replace", timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_entry_already_in_inputs_is_written_as_the_actual_bundle_default(self):
        (self.project / "story.ks").write_text('[ch text="DEFAULT_STORY"]\n[end]\n', encoding="utf-8")
        (self.project / "selected.ks").write_text('[ch text="SELECTED_ENTRY"]\n[end]\n', encoding="utf-8")
        result = self.cli("--entry", "selected.ks", self.project)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.packaged_entry(), "U21-ENTRY:selected.ks\nU21-TEXT:SELECTED_ENTRY")

    def test_project_relative_entry_keeps_duplicate_basename_scene_keys_distinct(self):
        for chapter, marker in (("章节 一", "FIRST_ENTRY"), ("章节 二", "SECOND_ENTRY")):
            scene = self.project / chapter / "opening.ks"
            scene.parent.mkdir()
            scene.write_text('[ch text="' + marker + '"]\n[end]\n', encoding="utf-8")
        result = self.cli("--entry", "章节 二\\opening.ks", self.project)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.packaged_entry(), "U21-ENTRY:章节 二/opening.ks\nU21-TEXT:SECOND_ENTRY")

    def test_bare_entry_basename_is_rejected_when_ambiguous(self):
        for chapter in ("chapter-a", "chapter-b"):
            scene = self.project / chapter / "opening.ks"
            scene.parent.mkdir()
            scene.write_text("[end]\n", encoding="utf-8")
        result = self.cli("--entry", "opening.ks", self.project)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("ambiguous", result.stderr)
        self.assertFalse(self.out.exists())

    def test_project_assets_overlay_shared_assets_at_the_fixed_package_root(self):
        shared = self.fixture / "assets"
        (shared / "shared-only.bin").write_bytes(b"shared template resource")
        (shared / "same.bin").write_bytes(b"shared version")
        own = self.project / "assets"
        own.mkdir()
        (own / "project-only.bin").write_bytes(b"project unique resource")
        (own / "same.bin").write_bytes(b"project override")
        result = self.cli(self.project)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.out / "assets/shared-only.bin").read_bytes(), b"shared template resource")
        self.assertEqual((self.out / "assets/project-only.bin").read_bytes(), b"project unique resource")
        self.assertEqual((self.out / "assets/same.bin").read_bytes(), b"project override")

    def test_absolute_asset_source_does_not_become_a_package_destination_path(self):
        selected = self.fixture / "额外 资源"
        selected.mkdir()
        (selected / "chosen.bin").write_bytes(b"selected absolute source")
        result = self.cli("--assets", selected, self.project)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.out / "assets/chosen.bin").read_bytes(), b"selected absolute source")
        self.assertFalse((self.out / selected.name).exists())

    def test_delivered_entry_mutation_cannot_pass_bundle_verification(self):
        injection = self.fixture / "mutate-entry-copy.mjs"
        injection.write_text("""import fs from 'node:fs';
import {resolve} from 'node:path';
import {syncBuiltinESMExports} from 'node:module';
const original=fs.copyFileSync;
fs.copyFileSync=function(source,destination,...args) {
  const result=original(source,destination,...args);
  if(String(destination).replaceAll('\\\\','/').endsWith('/cache/story/story.lua')) {
    const copied=fs.readFileSync(destination,'utf8');
    fs.writeFileSync(destination,'local bundle=(function() '+copied+' end)()\\nbundle.entry="missing-entry.ks"\\nreturn bundle\\n');
    console.log('U21_ENTRY_COPY_MUTATED');
  }
  return result;
};
syncBuiltinESMExports();
""", encoding="utf-8")
        result = self.cli("--entry", "story.ks", self.project, extra_env={
            "NODE_OPTIONS": "--import=" + injection.as_uri(),
            "U21_BUNDLE_COPY": str(self.out / "cache/story/story.lua")})
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("U21_ENTRY_COPY_MUTATED", result.stdout)
        self.assertIn("entry", (result.stdout + result.stderr).lower())
        self.assertNotIn("PACKAGE COMPLETE", result.stdout)
        self.assertFalse((self.out / "MANIFEST.txt").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
