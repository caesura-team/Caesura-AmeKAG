"""Real creator/Node CLI output transactions; only filesystem fault boundaries vary."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import caesura_build


def tree_bytes(root):
    if not root.exists():
        return None
    result = {}
    for path in sorted(root.rglob("*")):
        name = path.relative_to(root).as_posix()
        result[name] = "directory" if path.is_dir() else hashlib.sha256(path.read_bytes()).hexdigest()
    return result


class OutputTransactions:
    """Use a small project plus actual runtime/binary/profile, never a fake packager."""
    kind = None

    @classmethod
    def setUpClass(cls):
        cls.node = caesura_build.find_node()
        cls.lua = caesura_build.find_lua()
        cls.engine = caesura_build.find_engine() if cls.kind in ("native", "both") else None
        base = ROOT / "tmp"
        base.mkdir(exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix="u21-output-transactions-", dir=base)
        cls.fixture = Path(cls.temp.name)
        if not cls.fixture.resolve().is_relative_to(base.resolve()):
            raise RuntimeError("fixture must remain under the owned test root")
        cls.tool = cls.fixture / "tool"
        cls.tool.mkdir()
        for directory in ("scripts", "config", "web"):
            shutil.copytree(ROOT / directory, cls.tool / directory,
                            ignore=shutil.ignore_patterns("node_modules", "__pycache__", "dist"))
        for name in ("package.json", "package-lock.json", "npm-shrinkwrap.json"):
            if (ROOT / name).is_file():
                shutil.copy2(ROOT / name, cls.tool / name)
        if cls.kind in ("web", "both"):
            dist = ROOT / "web/dist"
            profile = json.loads((dist / "capabilities-build.json").read_text(encoding="utf-8"))
            for name in (*profile["bundle_files"], "capabilities-build.json"):
                target = cls.tool / "web/dist" / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(dist / name, target)
            if cls.kind == "both":
                # The real Python wrapper uses the normal Web CLI path, whose
                # tool-cache precondition precedes existing-dist reuse.
                cache = cls.tool / "cache/story/story.lua"
                cache.parent.mkdir(parents=True)
                shutil.copy2(ROOT / "cache/story/story.lua", cache)
        for name in ("fonts", "lang"):
            shutil.copytree(ROOT / "assets" / name, cls.tool / "assets" / name)
        cls.project = cls.tool / "项目 有空格"
        cls.project.mkdir()
        (cls.project / "caesura.project.json").write_text('{"capabilities":{}}\n', encoding="utf-8")
        (cls.project / "story.ks").write_text('[ch text="BASELINE_PACKAGE"]\n[end]\n', encoding="utf-8")
        cls.baseline = cls.tool / "基线 成品"
        cls.serial = 0
        cls.evidence = Path(os.environ["U21_TXN_EVIDENCE"]) if os.environ.get("U21_TXN_EVIDENCE") else None
        if cls.evidence:
            cls.evidence.mkdir(parents=True, exist_ok=True)
        result = cls.invoke(cls.baseline, label="baseline")
        if result.returncode:
            raise RuntimeError("Real CLI baseline failed:\n" + result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        if not cls.fixture.resolve().is_relative_to((ROOT / "tmp").resolve()):
            raise RuntimeError("unsafe fixture cleanup")
        cls.temp.cleanup()

    @classmethod
    def paths(cls, parent):
        if cls.kind in ("native", "both"):
            tag = "win64" if os.name == "nt" else sys.platform
            return parent / (cls.project.name + "-game"), parent / (cls.project.name + "-" + tag + ".zip")
        return parent / "作品 成品", parent / "作品.zip"

    @classmethod
    def native_driver(cls, fault):
        driver = cls.fixture / ("native-" + fault + ".py")
        code = """import os, runpy, sys, zipfile
from pathlib import Path
sys.path.insert(0, sys.argv[1])
import caesura_build
fault=os.environ.get('U21_TXN_FAULT')
copy=caesura_build.shutil.copy2
write=Path.write_text
archive_write=zipfile.ZipFile.write
rename=Path.rename
promotion_failed=False
def copy_boundary(source,destination,*args,**kwargs):
    result=copy(source,destination,*args,**kwargs)
    if Path(source).resolve()==Path(os.environ['CAESURA_ENGINE']).resolve() and fault=='concurrent-note':
        Path(os.environ['U21_PUBLIC_DIRECTORY'],'notes-during-build.txt').write_bytes(b'user edit while packaging')
        print('U21_REAL_CONCURRENT_NOTE',flush=True)
    if Path(source).resolve()==Path(os.environ['CAESURA_ENGINE']).resolve() and fault in ('copy','interrupt'):
        print('U21_REAL_COPY_BOUNDARY',flush=True)
        if fault=='interrupt':
            Path(os.environ['U21_TXN_BARRIER']).write_bytes(b'copied')
            while True: __import__('time').sleep(0.1)
        raise OSError('U21_INJECTED_COPY_FAILURE')
    return result
def write_boundary(path,*args,**kwargs):
    result=write(path,*args,**kwargs)
    if path.name=='HOW-TO-PLAY.txt' and fault=='completion':
        print('U21_REAL_COMPLETION_BOUNDARY',flush=True)
        raise OSError('U21_INJECTED_COMPLETION_FAILURE')
    return result
def zip_boundary(self,*args,**kwargs):
    result=archive_write(self,*args,**kwargs)
    if fault=='zip':
        print('U21_REAL_ZIP_BOUNDARY',flush=True)
        raise OSError('U21_INJECTED_ZIP_FAILURE')
    return result
def rename_boundary(path,destination,*args,**kwargs):
    global promotion_failed
    if fault in ('input-between-targets','asset-between-targets') and not promotion_failed and Path(destination).name==Path(os.environ['U21_SOURCE_STORY']).parent.name+'-game':
        result=rename(path,destination,*args,**kwargs)
        promotion_failed=True
        if fault=='input-between-targets':
            Path(os.environ['U21_SOURCE_STORY']).write_text('[ch text="CHANGED_DURING_BOTH"]\\n[end]\\n',encoding='utf-8')
            print('U21_REAL_INPUT_CHANGED_BETWEEN_TARGETS',flush=True)
        else:
            asset=Path(os.environ['U21_CHANGE_INPUT'])
            if os.environ['U21_CHANGE_OPERATION']=='delete': asset.unlink()
            else: asset.write_bytes(b'CHANGED_ASSET_DURING_BOTH')
            print('U21_REAL_ASSET_CHANGED_BETWEEN_TARGETS',flush=True)
        return result
    if fault=='promotion' and not promotion_failed and Path(destination)==Path(os.environ['U21_PUBLIC_DIRECTORY']):
        promotion_failed=True
        print('U21_REAL_PROMOTION_BOUNDARY',flush=True)
        raise OSError('U21_INJECTED_PROMOTION_FAILURE')
    return rename(path,destination,*args,**kwargs)
caesura_build.shutil.copy2=copy_boundary
Path.write_text=write_boundary
zipfile.ZipFile.write=zip_boundary
Path.rename=rename_boundary
script=str(Path(sys.argv[1])/'caesura.py')
sys.argv=[script]+sys.argv[2:]
runpy.run_path(script,run_name='__main__')
"""
        driver.write_text(code, encoding="utf-8")
        return driver

    @classmethod
    def node_loader(cls, fault):
        loader = cls.fixture / ("node-" + fault + ".mjs")
        loader.write_text("""import fs from 'node:fs';
import child from 'node:child_process';
import {basename,resolve,join} from 'node:path';
import {syncBuiltinESMExports} from 'node:module';
const fault=process.env.U21_TXN_FAULT;
const copy=fs.copyFileSync,write=fs.writeFileSync,spawn=child.spawnSync;
const rename=fs.renameSync;
let promotionFailed=false;
fs.copyFileSync=function(source,destination,...args){
  const result=copy(source,destination,...args);
  if(String(destination).replaceAll('\\\\','/').endsWith('/cache/story/story.lua') && fault==='concurrent-note'){
    write(join(process.env.U21_PUBLIC_DIRECTORY,'notes-during-build.txt'),'user edit while packaging');
    console.log('U21_REAL_CONCURRENT_NOTE');
  }
  if(String(destination).replaceAll('\\\\','/').endsWith('/cache/story/story.lua') && ['copy','interrupt'].includes(fault)){
    console.log('U21_REAL_COPY_BOUNDARY');
    if(fault==='interrupt'){
      write(process.env.U21_TXN_BARRIER,'copied');
      Atomics.wait(new Int32Array(new SharedArrayBuffer(4)),0,0);
    }
    throw new Error('U21_INJECTED_COPY_FAILURE');
  }
  return result;
};
fs.renameSync=function(source,destination,...args){
  if(fault==='promotion' && !promotionFailed && resolve(destination)===resolve(process.env.U21_PUBLIC_DIRECTORY)){
    promotionFailed=true;
    console.log('U21_REAL_PROMOTION_BOUNDARY');
    throw new Error('U21_INJECTED_PROMOTION_FAILURE');
  }
  return rename(source,destination,...args);
};
fs.writeFileSync=function(path,...args){
  const result=write(path,...args);
  if(basename(String(path))==='MANIFEST.txt' && fault==='completion'){
    console.log('U21_REAL_COMPLETION_BOUNDARY');
    throw new Error('U21_INJECTED_COMPLETION_FAILURE');
  }
  return result;
};
child.spawnSync=function(command,args,options){
  if(fault==='zip' && args?.[0]==='-c' && String(args[1]).includes('zipfile.ZipFile')){
    const prefix="import zipfile\\n_old_write=zipfile.ZipFile.write\\ndef _fail_write(self,*a,**k):\\n r=_old_write(self,*a,**k)\\n print('U21_REAL_ZIP_BOUNDARY',flush=True)\\n raise OSError('U21_INJECTED_ZIP_FAILURE')\\nzipfile.ZipFile.write=_fail_write\\n";
    args=[args[0],prefix+args[1],...args.slice(2)];
  }
  return spawn(command,args,options);
};
syncBuiltinESMExports();
""", encoding="utf-8")
        return loader

    @classmethod
    def command(cls, parent, fault=None, barrier=None):
        env = dict(os.environ, CAESURA_LUA=str(cls.lua), CAESURA_NODE=str(cls.node))
        env.pop("NODE_OPTIONS", None)
        if cls.engine:
            env["CAESURA_ENGINE"] = str(cls.engine)
        if fault:
            env["U21_TXN_FAULT"] = fault
            env["U21_PUBLIC_DIRECTORY"] = str(cls.paths(parent)[0])
        if barrier:
            env["U21_TXN_BARRIER"] = str(barrier)
        if cls.kind == "native":
            args = ["package", str(cls.project), "--target", "windows", "--out", str(parent), "--engine", str(cls.engine)]
            command = [sys.executable, str(cls.tool / "scripts/caesura.py"), *args]
            if fault:
                command = [sys.executable, str(cls.native_driver(fault)), str(cls.tool / "scripts"), *args]
        else:
            directory, archive = cls.paths(parent)
            command = [str(cls.node), str(cls.tool / "scripts/package_game.mjs"), "--no-web-build",
                       "--out", str(directory), "--zip", str(archive), str(cls.project)]
            if fault:
                env["NODE_OPTIONS"] = "--import=" + cls.node_loader(fault).as_uri()
        return command, env

    @classmethod
    def save_log(cls, label, command, result):
        if not cls.evidence:
            return
        stem = cls.kind + "-" + label
        (cls.evidence / (stem + ".stdout.log")).write_text(result.stdout, encoding="utf-8")
        (cls.evidence / (stem + ".stderr.log")).write_text(result.stderr, encoding="utf-8")
        (cls.evidence / (stem + ".json")).write_text(json.dumps({
            "command": command, "cwd": str(cls.tool), "exit_code": result.returncode,
        }, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    @classmethod
    def invoke(cls, parent, fault=None, label=None):
        command, env = cls.command(parent, fault)
        result = subprocess.run(command, cwd=cls.tool, env=env, capture_output=True,
                                text=True, encoding="utf-8", errors="replace", timeout=180)
        cls.save_log(label or fault or "run", command, result)
        return result

    def setUp(self):
        type(self).serial += 1
        self.case = self.tool / ("独占 case " + str(type(self).serial))
        self.case.mkdir()
        self.output = self.case / "交付 输出"
        shutil.copytree(self.baseline, self.output)
        self.directory, self.archive = self.paths(self.output)
        (self.project / "story.ks").write_text('[ch text="REPLACEMENT_PACKAGE"]\n[end]\n', encoding="utf-8")
        self.maxDiff = 1200

    def check_preserved(self, before, result):
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("COMPLETE", result.stdout)
        self.assertEqual(tree_bytes(self.output), before, "existing delivery/user bytes changed")

    def test_untouched_owned_output_can_be_rebuilt(self):
        result = self.invoke(self.output, label=self._testMethodName)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PACKAGE COMPLETE", result.stdout)
        self.assertTrue(self.archive.is_file())
        target = self.directory / ("projects" if self.kind == "native" else "demo") / self.project.name / "story.ks"
        self.assertIn("REPLACEMENT_PACKAGE", target.read_text(encoding="utf-8"))

    def test_added_notes_are_preserved_and_replacement_is_refused(self):
        (self.directory / "notes.txt").write_text("user notes must survive", encoding="utf-8")
        before = tree_bytes(self.output)
        self.check_preserved(before, self.invoke(self.output, label=self._testMethodName))

    def test_new_save_is_preserved_and_replacement_is_refused(self):
        save = self.directory / "saves/user-slot.bin"
        save.parent.mkdir(exist_ok=True)
        save.write_bytes(b"user save after playing")
        before = tree_bytes(self.output)
        self.check_preserved(before, self.invoke(self.output, label=self._testMethodName))

    def test_modified_generated_file_is_preserved(self):
        runtime = self.directory / "scripts/backend.lua"
        runtime.write_bytes(runtime.read_bytes() + b"\n-- USER MODIFICATION\n")
        before = tree_bytes(self.output)
        self.check_preserved(before, self.invoke(self.output, label=self._testMethodName))

    def test_same_size_generated_edit_is_preserved(self):
        runtime = self.directory / "scripts/backend.lua"
        original = runtime.read_bytes()
        runtime.write_bytes(bytes([original[0] ^ 1]) + original[1:])
        self.assertEqual(runtime.stat().st_size, len(original))
        before = tree_bytes(self.output)
        self.check_preserved(before, self.invoke(self.output, label=self._testMethodName))

    def test_legacy_marker_without_content_ownership_is_preserved(self):
        # A former version's marker cannot distinguish same-size user edits.
        (self.directory / ".caesura-output.json").unlink(missing_ok=True)
        before = tree_bytes(self.output)
        self.check_preserved(before, self.invoke(self.output, label=self._testMethodName))

    def test_modified_archive_is_preserved(self):
        self.archive.write_bytes(self.archive.read_bytes() + b"USER ARCHIVE CHANGE")
        before = tree_bytes(self.output)
        self.check_preserved(before, self.invoke(self.output, label=self._testMethodName))

    def test_copy_failure_keeps_the_previous_delivery(self):
        before = tree_bytes(self.output)
        result = self.invoke(self.output, "copy", self._testMethodName)
        self.assertIn("U21_REAL_COPY_BOUNDARY", result.stdout)
        self.check_preserved(before, result)

    def test_completion_write_failure_keeps_the_previous_delivery(self):
        before = tree_bytes(self.output)
        result = self.invoke(self.output, "completion", self._testMethodName)
        self.assertIn("U21_REAL_COMPLETION_BOUNDARY", result.stdout)
        self.check_preserved(before, result)

    def test_zip_failure_keeps_old_zip_and_reports_no_success(self):
        before = tree_bytes(self.output)
        result = self.invoke(self.output, "zip", self._testMethodName)
        self.assertIn("U21_REAL_ZIP_BOUNDARY", result.stdout)
        self.check_preserved(before, result)

    def test_promotion_failure_restores_directory_zip_and_receipt(self):
        before = tree_bytes(self.output)
        result = self.invoke(self.output, "promotion", self._testMethodName)
        self.assertIn("U21_REAL_PROMOTION_BOUNDARY", result.stdout)
        self.check_preserved(before, result)

    def test_notes_added_during_packaging_are_preserved(self):
        before = tree_bytes(self.output)
        name = self.directory.relative_to(self.output).as_posix() + "/notes-during-build.txt"
        before[name] = hashlib.sha256(b"user edit while packaging").hexdigest()
        result = self.invoke(self.output, "concurrent-note", self._testMethodName)
        self.assertIn("U21_REAL_CONCURRENT_NOTE", result.stdout)
        self.check_preserved(before, result)

    def test_interruption_after_a_real_copy_keeps_previous_delivery(self):
        before = tree_bytes(self.output)
        barrier = self.case / "copy-barrier"
        command, env = self.command(self.output, "interrupt", barrier)
        process = subprocess.Popen(command, cwd=self.tool, env=env, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace",
                                   creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        try:
            deadline = time.monotonic() + 30
            while not barrier.exists() and process.poll() is None and time.monotonic() < deadline:
                time.sleep(0.02)
            reached = barrier.exists()
            if process.poll() is None:
                process.kill()  # Exact owned process; barrier has no active compiler/child.
            stdout, stderr = process.communicate(timeout=10)
        finally:
            if process.poll() is None:
                process.kill(); process.wait(timeout=10)
        result = subprocess.CompletedProcess(command, process.returncode, stdout, stderr)
        self.save_log(self._testMethodName, command, result)
        self.assertTrue(reached, stdout + stderr)
        self.check_preserved(before, result)


class TestNativeOutputTransactions(OutputTransactions, unittest.TestCase):
    kind = "native"

    def test_both_rejects_web_destination_escape_before_native_publication(self):
        for existing in (False, True):
            with self.subTest(existing=existing):
                outside = self.fixture / ("outside-tool-delivery-" + str(existing))
                if existing:
                    shutil.copytree(self.baseline, outside)
                before = tree_bytes(outside)
                command, env = self.command(outside)
                command[command.index("windows")] = "both"
                result = subprocess.run(command, cwd=self.tool, env=env, capture_output=True,
                                        text=True, encoding="utf-8", errors="replace", timeout=180)
                self.save_log(self._testMethodName + "-" + str(existing), command, result)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("Web output must stay inside", result.stderr)
                self.assertNotIn("COMPLETE", result.stdout)
                self.assertEqual(tree_bytes(outside), before)


class TestWebOutputTransactions(OutputTransactions, unittest.TestCase):
    kind = "web"

    def test_prototype_named_user_file_is_preserved(self):
        (self.directory / "__proto__").write_bytes(b"user file with a JavaScript property name")
        before = tree_bytes(self.output)
        self.check_preserved(before, self.invoke(self.output, label=self._testMethodName))


class TestCombinedOutputTransactions(unittest.TestCase):
    """One real --target both command owns the complete delivery transaction."""
    kind = "both"
    setUpClass = classmethod(OutputTransactions.setUpClass.__func__)
    tearDownClass = classmethod(OutputTransactions.tearDownClass.__func__)
    paths = classmethod(OutputTransactions.paths.__func__)
    native_driver = classmethod(OutputTransactions.native_driver.__func__)
    node_loader = classmethod(OutputTransactions.node_loader.__func__)
    save_log = classmethod(OutputTransactions.save_log.__func__)
    invoke = classmethod(OutputTransactions.invoke.__func__)
    setUp = OutputTransactions.setUp
    check_preserved = OutputTransactions.check_preserved

    @classmethod
    def command(cls, parent, fault=None, barrier=None):
        env = dict(os.environ, CAESURA_LUA=str(cls.lua), CAESURA_NODE=str(cls.node),
                   CAESURA_ENGINE=str(cls.engine))
        env.pop("NODE_OPTIONS", None)
        args = ["package", str(cls.project), "--target", "both", "--out", str(parent),
                "--engine", str(cls.engine)]
        command = [sys.executable, str(cls.tool / "scripts/caesura.py"), *args]
        if fault:
            env["U21_TXN_FAULT"] = fault
            env["U21_PUBLIC_DIRECTORY"] = str(parent / (cls.project.name + "-web"))
            env["U21_SOURCE_STORY"] = str(cls.project / "story.ks")
            if fault.startswith("asset-"):
                _, scope, operation = fault.split("-")
                env["U21_TXN_FAULT"] = "asset-between-targets"
                env["U21_CHANGE_OPERATION"] = operation
                source = cls.project if scope == "project" else cls.tool
                env["U21_CHANGE_INPUT"] = str(source / "assets/资源.bin")
            env["NODE_OPTIONS"] = "--import=" + cls.node_loader(fault).as_uri()
            if fault in ("promotion", "input-between-targets") or fault.startswith("asset-"):
                # Old code promotes Web in Node; unified commit promotes in the
                # real Python parent. Inject the same final-path rename in both.
                command = [sys.executable, str(cls.native_driver(fault)), str(cls.tool / "scripts"), *args]
        return command, env

    def test_success_publishes_both_targets_with_one_completion(self):
        (self.output / "out-root-notes.txt").write_bytes(b"unrelated output-root content")
        result = self.invoke(self.output, label=self._testMethodName)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.count("PACKAGE COMPLETE"), 1, result.stdout)
        for target, scene_root in (("game", "projects"), ("web", "demo")):
            directory = self.output / (self.project.name + "-" + target)
            scene = directory / scene_root / self.project.name / "story.ks"
            self.assertIn("REPLACEMENT_PACKAGE", scene.read_text(encoding="utf-8"))
            self.assertTrue((directory / ".caesura-output.json").is_file())
        self.assertEqual(len(list(self.output.glob("*.zip"))), 2)
        self.assertEqual((self.output / "out-root-notes.txt").read_bytes(), b"unrelated output-root content")

    def test_second_target_copy_failure_preserves_every_previous_output(self):
        before = tree_bytes(self.output)
        result = self.invoke(self.output, "copy", self._testMethodName)
        self.assertIn("U21_REAL_COPY_BOUNDARY", result.stdout)
        self.check_preserved(before, result)

    def test_second_target_completion_failure_preserves_every_previous_output(self):
        before = tree_bytes(self.output)
        result = self.invoke(self.output, "completion", self._testMethodName)
        self.assertIn("U21_REAL_COMPLETION_BOUNDARY", result.stdout)
        self.check_preserved(before, result)

    def test_second_target_zip_failure_preserves_every_previous_output(self):
        before = tree_bytes(self.output)
        result = self.invoke(self.output, "zip", self._testMethodName)
        self.assertIn("U21_REAL_ZIP_BOUNDARY", result.stdout)
        self.check_preserved(before, result)

    def test_final_promotion_failure_restores_every_previous_output(self):
        before = tree_bytes(self.output)
        result = self.invoke(self.output, "promotion", self._testMethodName)
        self.assertIn("U21_REAL_PROMOTION_BOUNDARY", result.stdout)
        self.check_preserved(before, result)

    def test_inputs_changed_between_targets_cannot_publish_mixed_versions(self):
        before = tree_bytes(self.output)
        result = self.invoke(self.output, "input-between-targets", self._testMethodName)
        self.assertIn("U21_REAL_INPUT_CHANGED_BETWEEN_TARGETS", result.stdout)
        self.check_preserved(before, result)

    def test_second_target_failure_never_publishes_a_new_native_delivery(self):
        fresh = self.case / "全新 成品"
        self.assertFalse(fresh.exists())
        result = self.invoke(fresh, "zip", self._testMethodName)
        self.assertIn("U21_REAL_ZIP_BOUNDARY", result.stdout)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("COMPLETE", result.stdout)
        self.assertFalse(fresh.exists(), "failed both command exposed a new first-target delivery")

    def check_asset_changes_between_targets(self, scope):
        source = self.project if scope == "project" else self.tool
        asset = source / "assets/资源.bin"
        asset.parent.mkdir(exist_ok=True)
        for operation in ("modify", "add", "delete"):
            with self.subTest(operation=operation):
                if operation == "add":
                    asset.unlink(missing_ok=True)
                else:
                    asset.write_bytes(b"asset selected before either target was prepared")
                output = self.case / (scope + "-" + operation)
                shutil.copytree(self.baseline, output)
                before = tree_bytes(output)
                result = self.invoke(output, "asset-" + scope + "-" + operation,
                                     self._testMethodName + "-" + operation)
                self.assertIn("U21_REAL_ASSET_CHANGED_BETWEEN_TARGETS", result.stdout)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertNotIn("COMPLETE", result.stdout)
                self.assertEqual(tree_bytes(output), before)

    def test_project_asset_changes_between_targets_preserve_all_outputs(self):
        self.check_asset_changes_between_targets("project")

    def test_shared_asset_changes_between_targets_preserve_all_outputs(self):
        # Not referenced by the Native scene: Web still ships the shared pool.
        self.check_asset_changes_between_targets("shared")


if __name__ == "__main__":
    unittest.main(verbosity=2)
