"""Media dependencies from real creator/Node CLI tokens, with real package fixtures."""
from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import unittest
import wave

from test_package_output_transactions import OutputTransactions, tree_bytes

ROOT = Path(__file__).resolve().parents[2]


class AssetDependencyCases:
    setUpClass = classmethod(OutputTransactions.setUpClass.__func__)
    tearDownClass = classmethod(OutputTransactions.tearDownClass.__func__)
    paths = classmethod(OutputTransactions.paths.__func__)
    command = classmethod(OutputTransactions.command.__func__)
    save_log = classmethod(OutputTransactions.save_log.__func__)
    invoke = classmethod(OutputTransactions.invoke.__func__)
    setUp = OutputTransactions.setUp
    check_preserved = OutputTransactions.check_preserved

    def inspect_package(self):
        if self.kind == "native":
            info = json.loads((self.directory / "BUILD-INFO.json").read_text(encoding="utf-8"))
            return {"missing": info["assets_missing"], "dynamic": info["assets_runtime_computed"],
                    "skipped": info.get("assets_skipped_capabilities", []),
                    "unproven": info.get("asset_dependency_unproven", [])}
        script = """package.path='scripts/?.lua;scripts/?/init.lua;'..package.path
local b=assert(loadfile('cache/story/story.lua','t',{}))()
local json=require('capability_json')
-- Inspect the delivered values without recomputing classification or policy.
-- Lua literals do not carry capability_json's private array tags.
local delivered=b.asset_dependencies or {}
local dependencies={}
for _,field in ipairs({'dynamic','skipped'}) do
  if type(delivered[field])=='table' then
    local records=json.array()
    for _,record in ipairs(delivered[field]) do
      records[#records+1]={path=record.path,reason=record.reason}
    end
    dependencies[field]=records
  end
end
local encoded=assert(json.encode({assets=json.array(b.assets),dependencies=dependencies}))
io.write(encoded)
"""
        command = [str(self.lua), "-e", script]
        result = subprocess.run(command, cwd=self.directory, capture_output=True,
                                text=True, encoding="utf-8", errors="replace", timeout=30)
        self.save_log(self._testMethodName + "-inspect", command, result)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads(result.stdout)

    def write_story(self, text):
        (self.project / "story.ks").write_text(text, encoding="utf-8")

    def test_nested_scene_references_are_not_media_dependencies(self):
        scripts = self.project / "assets/script/旅程"
        scripts.mkdir(parents=True)
        (scripts / "存档点.ks").write_text('[ch text="CALLEE"]\n[return]\n', encoding="utf-8")
        (scripts / "终点.ks").write_text('[end]\n', encoding="utf-8")
        image = self.project / "assets/图片.png"
        shutil.copy2(ROOT / "assets/bg/classroom.png", image)
        sound = self.project / "assets/声音.wav"
        with wave.open(str(sound), "wb") as stream:
            stream.setnchannels(1); stream.setsampwidth(2); stream.setframerate(8000)
            stream.writeframes(b"\0\0" * 80)
        self.write_story('[bg storage="assets/图片.png"]\n'
                         '[playse storage="assets/声音.wav"]\n'
                         '[call storage="旅程/存档点.ks"]\n'
                         '[jump storage="旅程/终点.ks"]\n[end]\n')
        result = self.invoke(self.output, label=self._testMethodName)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = self.inspect_package()
        if self.kind == "native":
            self.assertEqual(data["missing"], [])
        else:
            self.assertCountEqual(data["assets"], ["assets/图片.png", "assets/声音.wav"])
        for source in (image, sound):
            self.assertEqual((self.directory / "assets" / source.name).read_bytes(), source.read_bytes())

    def test_comments_text_and_authored_code_do_not_create_media_dependencies(self):
        self.write_story('''; [bg storage="assets/fake-comment.png"]
[ch text='quoted storage="assets/fake-text.png"']
[iscript]
local value = 'file="assets/fake-code.wav"'
error('U21_AUTHORED_CODE_MUST_NOT_EXECUTE_WHILE_PACKAGING')
[endscript]
[end]
''')
        result = self.invoke(self.output, label=self._testMethodName)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("U21_AUTHORED_CODE_MUST_NOT_EXECUTE_WHILE_PACKAGING", result.stdout + result.stderr)
        data = self.inspect_package()
        self.assertEqual(data["missing"] if self.kind == "native" else list(data["assets"]), [])

    def test_missing_static_media_refuses_before_replacing_previous_delivery(self):
        for command, path in (("bg", "assets/missing-picture.png"),
                              ("playse", "assets/missing-sound.wav"),
                              ("image", "assets/media-with-scene-extension.ks")):
            with self.subTest(command=command):
                self.write_story('[' + command + ' storage="' + path + '"]\n[end]\n')
                before = tree_bytes(self.output)
                result = self.invoke(self.output, label=self._testMethodName + "-" + command)
                self.check_preserved(before, result)
                self.assertIn(path, result.stdout + result.stderr)

    def test_dynamic_media_is_unproved_and_never_a_literal_missing_file(self):
        reference = "assets/${f.choice}.png"
        self.write_story('[bg storage="' + reference + '"]\n[end]\n')
        result = self.invoke(self.output, label=self._testMethodName)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = self.inspect_package()
        if self.kind == "native":
            self.assertEqual(data["missing"], [])
            self.assertIn(reference, data["dynamic"])
        else:
            self.assertNotIn(reference, data["assets"])
            self.assertIn("dynamic", data["dependencies"])
            self.assertIn(reference, [item["path"] for item in data["dependencies"]["dynamic"]])

    def test_capability_skipped_media_is_not_claimed_as_verified_or_required(self):
        (self.project / "caesura.project.json").write_text(json.dumps({
            "capabilities": {"optional": ["video.play", "video.ffmpeg"]},
        }), encoding="utf-8")
        reference = "assets/optional-unavailable-video.mp4"
        self.write_story('[video storage="' + reference + '"]\n[end]\n')
        result = self.invoke(self.output, label=self._testMethodName)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = self.inspect_package()
        if self.kind == "native":
            self.assertEqual(data["missing"], [])
            self.assertIn(reference, data["skipped"])
        else:
            self.assertNotIn(reference, data["assets"])
            self.assertIn("skipped", data["dependencies"])
            self.assertIn(reference, [item["path"] for item in data["dependencies"]["skipped"]])

    def test_playbgmstop_replacement_media_is_required(self):
        reference = "assets/missing-replacement.wav"
        self.write_story('[playbgmstop storage="' + reference + '"]\n[end]\n')
        before = tree_bytes(self.output)
        result = self.invoke(self.output, label=self._testMethodName)
        self.check_preserved(before, result)
        self.assertIn(reference, result.stdout + result.stderr)

    def test_builtin_media_before_macro_definition_and_after_erase_is_required(self):
        reference = "assets/missing-builtin.png"
        for position, story in (("before", '[bg storage="' + reference + '"]\n[macro name="bg"]\n[endmacro]\n[end]\n'),
                                ("after-erase", '[macro name="bg"]\n[endmacro]\n[erasemacro name="bg"]\n[bg storage="' + reference + '"]\n[end]\n')):
            with self.subTest(position=position):
                self.write_story(story)
                before = tree_bytes(self.output)
                result = self.invoke(self.output, label=self._testMethodName + "-" + position)
                self.check_preserved(before, result)
                self.assertIn(reference, result.stdout + result.stderr)

    def test_active_sequential_macro_does_not_invent_builtin_media(self):
        self.write_story('[macro name="bg"]\n[ch text="%storage%"]\n[endmacro]\n'
                         '[bg storage="dialogue-not-an-image.png"]\n[end]\n')
        result = self.invoke(self.output, label=self._testMethodName)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = self.inspect_package()
        self.assertEqual(data["missing"] if self.kind == "native" else list(data["assets"]), [])

    def test_conditional_macro_dispatch_is_explicitly_unproved(self):
        reference = "assets/uncertain-macro-image.png"
        self.write_story('[if exp="f.define_macro"]\n[macro name="bg"]\n'
                         '[ch text="%storage%"]\n[endmacro]\n[endif]\n'
                         '[bg storage="' + reference + '"]\n[end]\n')
        result = self.invoke(self.output, label=self._testMethodName)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = self.inspect_package()
        if self.kind == "native":
            self.assertEqual(data["missing"], [])
            unproven = data["unproven"]
        else:
            self.assertNotIn(reference, data["assets"])
            unproven = data["dependencies"]["dynamic"]
        self.assertTrue(any(item["path"] == reference and item["reason"] == "macro_dispatch_unproven"
                            for item in unproven), unproven)


class TestNativeAssetDependencies(AssetDependencyCases, unittest.TestCase):
    kind = "native"


class TestWebAssetDependencies(AssetDependencyCases, unittest.TestCase):
    kind = "web"


if __name__ == "__main__":
    unittest.main(verbosity=2)
