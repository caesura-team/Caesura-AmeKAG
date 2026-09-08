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
const copy = fs.copyFileSync;
fs.copyFileSync = function(source,target,...args) {
  const result = copy(source,target,...args);
  if (resolve(String(target)) === resolve(destination)) {
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
''' % json.dumps(str(destination)), encoding='utf-8')
            for mutation, reason in (('semantics', 'compiler-semantics-mismatch'),
                                     ('missing-scene', 'missing-bundle-scene:callee.ks')):
                with self.subTest(mutation=mutation):
                    env = dict(os.environ, NODE_OPTIONS='--import=' + injection.as_uri(), U14_COPY_MUTATION=mutation)
                    rc, out, err = run_cli_full('--out', self.out_name, FIRST_VN_KS,
                                                callee.relative_to(ROOT).as_posix(), env=env)
                    self.assertEqual(rc, 1, out + err)
                    self.assertIn(reason, out + err)
                    self.assertNotIn('PACKAGE COMPLETE', out)
                    self.assertFalse((self.out_path / 'MANIFEST.txt').exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
