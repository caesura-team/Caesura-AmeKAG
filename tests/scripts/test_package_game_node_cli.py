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
            destination = self.out_path / 'cache/story/story.lua'
            injection.write_text('''import fs from 'node:fs';
import {resolve} from 'node:path';
import {syncBuiltinESMExports} from 'node:module';
const destination = %s;
const packageRoot = %s;
const copy = fs.copyFileSync;
fs.copyFileSync = function(source,target,...args) {
  const result = copy(source,target,...args);
  if (resolve(String(target)) === resolve(destination)) {
    if (process.env.U14_COPY_MUTATION==='missing-runtime') {
      fs.unlinkSync(resolve(packageRoot,'scripts/kag.lua'));
      fs.unlinkSync(resolve(packageRoot,'scripts/kag/init.lua'));
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
                    env = dict(os.environ, NODE_OPTIONS='--import=' + injection.as_uri(), U14_COPY_MUTATION=mutation)
                    rc, out, err = run_cli_full('--out', self.out_name, FIRST_VN_KS,
                                                callee.relative_to(ROOT).as_posix(), env=env)
                    self.assertEqual(rc, 1, out + err)
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
            injection.write_text('''import child from 'node:child_process';
import {syncBuiltinESMExports} from 'node:module';
const spawn = child.spawnSync;
child.spawnSync = function(command,args,options) {
  if (args?.[0]==='-e' && args[1]?.includes('PACKAGE-SCENE-KEYS:')) {
    command = %s;
  }
  return spawn(command,args,options);
};
syncBuiltinESMExports();
''' % json.dumps(str(missing_lua)), encoding='utf-8')
            env = dict(os.environ, NODE_OPTIONS='--import=' + injection.as_uri())
            rc, out, err = run_cli_full('--out', self.out_name, FIRST_VN_KS, env=env)
            self.assertEqual(rc, 1, out + err)
            self.assertTrue((self.out_path / 'cache/story/story.lua').is_file())
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
