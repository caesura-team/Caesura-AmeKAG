"""U22 static Web package contracts using the actual host Lua compiler.

Set CAESURA_TEST_LUA to an explicit Lua executable. No browser, engine, build,
packager, network service, or installed dependency is used by these fixtures.
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "scripts/verify_web_package.py"
sys.path.insert(0, str(ROOT / "scripts"))
if MODULE.exists():
    SPEC = importlib.util.spec_from_file_location("verify_web_package", MODULE)
    verify = importlib.util.module_from_spec(SPEC)
    sys.modules[SPEC.name] = verify
    SPEC.loader.exec_module(verify)
else:
    verify = None

BAKE_FIXTURE = r'''
package.path='scripts/?.lua;scripts/?/init.lua'
package.cpath=''
local json=require('capability_json')
local request=assert(json.decode(io.read('*a')))
require('kag')
local compiler=require('kag.compiler')
local tokenizer=require('tokenizer')
local assets=require('kag.asset_dependencies')
local keys=assert(compiler.bundleSceneKeys(request.paths))
local bundle={version=1,scenes={},entry=keys[1]}
local dependencies=assets.new_report()
for index,source in ipairs(request.sources) do
    local tokens=tokenizer.parse(source)
    assets.extend(dependencies,assets.collect(tokens,keys[index]))
    compiler.compile(tokens)
    bundle.scenes[keys[index]]=assert(compiler.serialize(tokens))
end
bundle.asset_dependencies=assets.apply_capabilities(dependencies,request.capabilities)
bundle.assets=bundle.asset_dependencies.static
assert(compiler.validateBundle(bundle,keys))
local encoded=assert(json.encode({bundle='return '..compiler.encode_lua_literal(bundle)..'\n',
    entry=bundle.entry,static=#bundle.asset_dependencies.static,
    dynamic=#bundle.asset_dependencies.dynamic,skipped=#bundle.asset_dependencies.skipped}))
io.write(encoded)
'''


class WebPackageContract(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(verify, "Missing U22 implementation: scripts/verify_web_package.py")
        explicit_lua = os.environ.get("CAESURA_TEST_LUA")
        self.assertTrue(explicit_lua, "CAESURA_TEST_LUA must explicitly identify the host Lua tool")
        self.lua = Path(explicit_lua)
        self.assertTrue(self.lua.is_file(), "Explicit host Lua executable is missing")
        self.temp = tempfile.TemporaryDirectory(prefix="caesura-u22-web-static-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def fixture(self, name="最终 Web 包", *, boundaries=False, author_marker=None):
        package = self.root / name
        package.mkdir()
        for source in (ROOT / "scripts").rglob("*.lua"):
            destination = package / "scripts" / source.relative_to(ROOT / "scripts")
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
        modules = {p.relative_to(package / "scripts").as_posix()[:-4].replace("/", "."): True
                   for p in (package / "scripts").rglob("*.lua")}
        (package / "scripts/index.json").write_text(
            json.dumps(modules, ensure_ascii=False, sort_keys=True, indent=1), encoding="utf-8")
        self.asset = "assets/媒体/图,像's.png"
        sources = ['[bg storage="' + self.asset + '"]\n[end]\n', '[end]\n']
        capabilities = {"passed": True, "requirements": []}
        if boundaries:
            sources[0] = sources[0].replace('[end]', '[playbgm storage="$f.bgm"]\n'
                '[video storage="assets/optional-video.mp4"]\n[end]')
            capabilities["requirements"] = [{"feature": "video.play", "decision": "skip"},
                                            {"feature": "video.ffmpeg", "decision": "skip"}]
        if author_marker:
            body = "io.open('" + author_marker.as_posix() + "','w'):write('AUTHOR EXECUTED')"
            sources[0] = '[iscript]\n' + body + '\n[endscript]\n' + sources[0]
        keys = ["章节 一/同名.ks", "章节 二/同名.ks"]
        request = {"paths": keys, "sources": sources, "capabilities": capabilities}
        baked = subprocess.run([str(self.lua), "-E", "-e", BAKE_FIXTURE], cwd=ROOT,
                               input=json.dumps(request, ensure_ascii=False).encode("utf-8"),
                               capture_output=True, timeout=10, check=False)
        self.assertEqual(baked.returncode, 0, baked.stderr.decode("utf-8", "replace"))
        data = json.loads(baked.stdout)
        self.fixture_data = data
        files = {
            "index.html": '<!doctype html><html><head>'
                '<style>@font-face{font-family:CaesuraNoto;'
                'src:url("assets/fonts/NotoSansCJKsc-Regular.otf")}</style>'
                '<link rel="stylesheet" href="./web-assets/style.css">'
                '<link rel="manifest" href="./web-assets/player.webmanifest">'
                '<script>self.__CAESURA_WASM_FILE__ = new URL("web-assets/glue.wasm", '
                'document.baseURI || location.href).href</script>'
                '<script type="module" src="./web-assets/main.js"></script>'
                '</head><body><div id="stage"></div></body></html>',
            "web-assets/main.js": 'import "./initial.js"; import("./lazy.js"); '
                'new URL("./glue.hash.wasm",import.meta.url);',
            "web-assets/player.webmanifest": json.dumps({
                "start_url": "../index.html", "scope": "../",
                "icons": [{"src": "../assets/icon.png", "sizes": "192x192", "type": "image/png"}]}),
            "assets/icon.png": b"\x89PNG\r\n\x1a\n",
            "web-assets/initial.js": 'export const okay=true;',
            "web-assets/lazy.js": 'export { okay } from "./initial.js";',
            "web-assets/style.css": '@import "./nested/theme.css";body{'
                'background:url("../assets/媒体/图%2C像%27s.png")}',
            "web-assets/nested/theme.css": 'body{margin:0}',
            "cache/story/story.lua": data["bundle"],
            "CAPABILITIES.json": json.dumps(capabilities),
            "assets/fonts/NotoSansCJKsc-Regular.otf": b"OTTO\0\0\0\0",
            "web-assets/glue.wasm": b"\0asm\1\0\0\0",
            "web-assets/glue.hash.wasm": b"\0asm\1\0\0\0",
            self.asset: b"\x89PNG\r\n\x1a\n",
        }
        for key, source in zip(keys, sources):
            files["demo/fixture 中文/" + key] = source
        for relative, value in files.items():
            destination = package / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(value.encode("utf-8") if isinstance(value, str) else value)
        self.manifest(package)
        return package

    def manifest(self, package):
        data = self.fixture_data
        lines = ["Caesura (AmeKAG) web package: fixture 中文", "built: fixture", "scenes: 2",
                 "entry scene: " + data["entry"], "static media dependencies: " + str(data["static"]),
                 "dynamic media dependencies (not proven): " + str(data["dynamic"]),
                 "capability-skipped media dependencies (not verified): " + str(data["skipped"]),
                 "---", "files (size bytes, path):"]
        for path in sorted(package.rglob("*")):
            if path.is_file() and path.name != "MANIFEST.txt":
                lines.append(str(path.stat().st_size) + "\t" + path.relative_to(package).as_posix())
        (package / "MANIFEST.txt").write_text("\n".join(lines) + "\n---\ntotal KB: 0\n", encoding="utf-8")

    def inspect(self, package, **kwargs):
        result = verify.inspect_web_package(package, self.lua, **kwargs)
        self.assertEqual(result["runtime"], "NOT_RUN")
        return result

    def assert_failed(self, package, offender=None, **kwargs):
        result = self.inspect(package, **kwargs)
        self.assertEqual(result["status"], "STATIC_FAIL", result)
        if offender:
            self.assertIn(offender, json.dumps(result, ensure_ascii=False))
        return result

    def test_real_compiler_nested_scene_and_escaped_asset_positive(self):
        package = self.fixture()
        bundle = package / "cache/story/story.lua"
        bundle.write_text(bundle.read_text(encoding="utf-8").replace('"assets/', '"\\97ssets/'), encoding="utf-8")
        self.manifest(package)
        result = self.inspect(package)
        self.assertEqual(result["status"], "STATIC_PASS", result)
        self.assertEqual(result["bundle"]["entry"], "章节 一/同名.ks")
        self.assertEqual(set(result["bundle"]["scene_keys"]), {"章节 一/同名.ks", "章节 二/同名.ks"})
        self.assertIn(self.asset, result["bundle"]["dependencies"]["static"])
        self.assertEqual(result["tools"]["lua"]["role"], "host_validation_tool")

    def test_each_required_resource_is_checked_at_its_actual_path(self):
        names = ["web-assets/glue.wasm", "web-assets/main.js", "web-assets/initial.js",
                 "web-assets/lazy.js", "web-assets/glue.hash.wasm", "web-assets/nested/theme.css",
                 "assets/fonts/NotoSansCJKsc-Regular.otf", "scripts/kag/init.lua"]
        for index, name in enumerate(names):
            with self.subTest(name=name):
                package = self.fixture(str(index))
                (package / name).unlink()
                # A fresh self-generated manifest must not hide a missing file.
                self.manifest(package)
                self.assert_failed(package, name)

    def test_missing_static_asset_is_not_satisfied_by_another_file_or_source_tree(self):
        package = self.fixture()
        (package / self.asset).unlink()
        (package / "assets/unrelated.png").write_bytes(b"another asset")
        self.manifest(package)
        self.assert_failed(package, self.asset)

    def test_bad_entry_and_manifest_entry_mismatch(self):
        for mutation in ("bundle", "manifest"):
            with self.subTest(mutation=mutation):
                package = self.fixture(mutation)
                if mutation == "bundle":
                    bundle = package / "cache/story/story.lua"
                    text = bundle.read_text(encoding="utf-8")
                    bundle.write_text(text.replace('["entry"]="章节 一/同名.ks"',
                                                   '["entry"]="missing.ks"'), encoding="utf-8")
                    self.manifest(package)
                else:
                    manifest = package / "MANIFEST.txt"
                    manifest.write_text(manifest.read_text(encoding="utf-8").replace(
                        "entry scene: 章节 一/同名.ks", "entry scene: 章节 二/同名.ks"), encoding="utf-8")
                self.assert_failed(package, "entry")

    def test_dynamic_and_optional_skip_are_separate_unverified_boundaries(self):
        package = self.fixture(boundaries=True)
        result = self.inspect(package)
        self.assertEqual(result["status"], "STATIC_PASS", result)
        dependencies = result["bundle"]["dependencies"]
        self.assertEqual(len(dependencies["dynamic"]), 1)
        self.assertEqual(len(dependencies["skipped"]), 1)
        self.assertNotIn("assets/optional-video.mp4", dependencies["static"])
        self.assertFalse((package / "assets/optional-video.mp4").exists())

    def test_bundle_cannot_hide_static_media_by_erasing_dependency_lists(self):
        package = self.fixture()
        bundle = package / "cache/story/story.lua"
        text = bundle.read_text(encoding="utf-8")
        text = text.replace('["static"]={"' + self.asset + '"}', '["static"]={}')
        text = text.replace('["assets"]={"' + self.asset + '"}', '["assets"]={}')
        bundle.write_text(text, encoding="utf-8")
        self.manifest(package)
        self.assert_failed(package, "dependencies")

    def test_author_iscript_is_data_and_lua_init_is_ignored(self):
        author_marker = self.root / "author-ran.txt"
        init_marker = self.root / "lua-init-ran.txt"
        package = self.fixture(author_marker=author_marker)
        hostile_init = "io.open('" + init_marker.as_posix() + "','w'):write('INIT')"
        with mock.patch.dict(os.environ, {"LUA_INIT": hostile_init, "LUA_INIT_5_4": hostile_init}):
            result = self.inspect(package)
        self.assertEqual(result["status"], "STATIC_PASS", result)
        self.assertFalse(author_marker.exists())
        self.assertFalse(init_marker.exists())

    def test_executable_bundle_and_instruction_budget_are_rejected(self):
        marker = self.root / "bundle-executed.txt"
        package = self.fixture()
        (package / "cache/story/story.lua").write_text(
            "return (function() io.open('" + marker.as_posix() + "','w'):write('BAD'); return {} end)()",
            encoding="utf-8")
        self.manifest(package)
        self.assert_failed(package, "literal")
        self.assertFalse(marker.exists())
        package = self.fixture("budget")
        self.assert_failed(package, "instruction", instruction_limit=1)

    def test_manifest_urls_resolve_against_the_actual_manifest_path(self):
        for name, field, value in (("start", "start_url", "./index.html"),
                                   ("icon", "icons", [{"src": "assets/icon.png"}]),
                                   ("scope", "scope", "./"),
                                   ("escape", "start_url", "../../index.html"),
                                   ("external", "icons", [{"src": "https://example.invalid/icon.png"}])):
            with self.subTest(name=name):
                package = self.fixture(name)
                path = package / "web-assets/player.webmanifest"
                data = json.loads(path.read_text(encoding="utf-8"))
                data[field] = value
                path.write_text(json.dumps(data), encoding="utf-8")
                self.manifest(package)
                self.assert_failed(package, "manifest")
        package = self.fixture("missing-icon")
        (package / "assets/icon.png").unlink()
        self.manifest(package)
        self.assert_failed(package, "assets/icon.png")

    def test_manifest_default_scope_and_encoded_icon_are_valid(self):
        package = self.fixture()
        path = package / "web-assets/player.webmanifest"
        data = json.loads(path.read_text(encoding="utf-8"))
        data.pop("scope")
        data["icons"][0]["src"] = "../assets/%69con.png"
        path.write_text(json.dumps(data), encoding="utf-8")
        self.manifest(package)
        result = self.inspect(package)
        self.assertEqual(result["status"], "STATIC_PASS", result)
        self.assertEqual(result["web_manifests"][0]["start_url"], "index.html")
        self.assertEqual(result["web_manifests"][0]["scope"], "")
        self.assertEqual(result["web_manifests"][0]["icons"], ["assets/icon.png"])

    def test_reference_path_case_is_exact_even_on_windows(self):
        package = self.fixture()
        page = package / "index.html"
        page.write_text(page.read_text(encoding="utf-8").replace(
            "./web-assets/main.js", "./web-assets/Main.js"), encoding="utf-8")
        self.manifest(package)
        self.assert_failed(package, "Main.js")

    def test_real_lua_child_is_stopped_by_external_timeout(self):
        package = self.fixture()
        result = self.assert_failed(package, "external timeout", timeout_seconds=0.000001)
        self.assertTrue(result["lua_process"]["timed_out"])
        self.assertTrue(result["input_stable"])

    def test_package_compiler_is_not_executed_or_trusted_as_its_own_validator(self):
        package = self.fixture()
        marker = self.root / "compiler-executed.txt"
        (package / "scripts/kag/compiler.lua").write_text(
            "io.open('" + marker.as_posix() + "','w'):write('BAD'); return {}", encoding="utf-8")
        self.manifest(package)
        self.assert_failed(package, "scripts/kag/compiler.lua")
        self.assertFalse(marker.exists())

    def test_nonlocal_or_escaping_references_and_stale_index_fail(self):
        for index, url in enumerate(("https://example.invalid/player.js", "/web-assets/main.js",
                                     "../../escape.js", "%2e%2e/%2e%2e/escape.js")):
            package = self.fixture(str(index))
            path = package / "index.html"
            path.write_text(path.read_text(encoding="utf-8").replace("./web-assets/main.js", url), encoding="utf-8")
            self.manifest(package)
            self.assert_failed(package)
        package = self.fixture("index")
        (package / "scripts/index.json").write_text('{"kag.init":true}', encoding="utf-8")
        self.manifest(package)
        self.assert_failed(package, "scripts/index.json")

    def test_changed_host_source_cannot_be_hashed_as_different_executed_bytes(self):
        package = self.fixture()
        trusted = self.root / "trusted-validator"
        shutil.copytree(ROOT / "scripts", trusted)
        compiler = trusted / "kag/compiler.lua"
        original = compiler.read_bytes()
        replacement = original + b"\n-- Later validator revision.\n"
        (package / "scripts/kag/compiler.lua").write_bytes(replacement)
        self.manifest(package)
        real_read = Path.read_bytes
        changed = []

        def race_read(path):
            data = real_read(path)
            if path == compiler and not changed:
                changed.append(True)
                compiler.write_bytes(replacement)
            return data

        with mock.patch.object(verify, "SCRIPT_ROOT", trusted), \
                mock.patch.object(Path, "read_bytes", race_read):
            result = self.assert_failed(package, "changed")
        self.assertTrue(changed, "The actual trusted module read must be exercised")
        self.assertTrue(result["input_stable"])

    def test_failed_capability_report_cannot_produce_static_pass(self):
        package = self.fixture()
        path = package / "CAPABILITIES.json"
        data = json.loads(path.read_text(encoding="utf-8"))
        data["passed"] = False
        path.write_text(json.dumps(data), encoding="utf-8")
        self.manifest(package)
        self.assert_failed(package, "capability report")

    def test_cli_new_report_and_old_report_preservation(self):
        package = self.fixture()
        report = self.root / "report.json"
        command = [sys.executable, "-X", "utf8", str(MODULE), "--package-root", str(package),
                   "--lua", str(self.lua), "--report", str(report)]
        result = subprocess.run(command, capture_output=True, timeout=20, check=False)
        self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", "replace"))
        recorded = report.read_bytes()
        self.assertEqual(json.loads(recorded)["status"], "STATIC_PASS")
        result = subprocess.run(command, capture_output=True, timeout=20, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(report.read_bytes(), recorded)


if __name__ == "__main__":
    unittest.main()
