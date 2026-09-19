"""Real Python/HTTP fixtures exercise orchestration, never Engine acceptance.

Only native Engine/Lua executable invocation is replaced with an explicit Python
protocol fixture. The owned runner, identity/socket checks, HTTP requests, file
copies, template provenance and mutation checks remain real.
"""
from __future__ import annotations

import importlib.util
import hashlib
import http.server
import json
import os
from pathlib import Path
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
MODULE = ROOT / "scripts/native_package_runtime.py"
if MODULE.is_file():
    spec = importlib.util.spec_from_file_location("native_runtime_contract", MODULE)
    runtime = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runtime)
else:
    runtime = None

# Native fixtures execute the actual current OS image, including framework Python
# hosts where sys.executable is only an exec launcher. No PATH lookup is involved.
FIXTURE_PYTHON = runtime.process_identity(os.getpid()).executable if runtime else sys.executable

ENGINE_FIXTURE = r'''
import http.server, json, os, secrets, sys
from pathlib import Path
root = Path.cwd()
mode = (root / 'fixture-mode.txt').read_text() if (root / 'fixture-mode.txt').exists() else ''
if '--editor' not in sys.argv:
    print('[KAG Runner] Started synthetic protocol scene', flush=True)
    if mode == 'render-disabled': print('rendering disabled (BGFX_DEBUG_IFH)', flush=True)
    if mode == 'mutate-created-game' and (root/'BUILD-INFO.json').exists():
        next((root/'projects').rglob('story.ks')).write_text('changed during game execution')
    raise SystemExit(0)
if mode == 'early-exit': raise SystemExit(7)
token = os.environ.get('CAESURA_EDITOR_TOKEN')
if not token:
    token = secrets.token_hex(24)
    if mode != 'missing-token': (root / '.caesura-editor-token').write_text(token)
    print('[EditorServer] Generated editor token: ' + token, file=sys.stderr, flush=True)
if mode == 'mutate': (root / 'immutable.txt').write_text('changed')
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def do_GET(self):
        if self.path.startswith('/api/'):
            authorized = self.headers.get('Authorization') == 'Bearer ' + token
            code = 200 if authorized or mode == 'open-api' else 401
            body = b'{"status":"ok","engine":"CaesuraAmeKAG"}' if code == 200 else b'{}'
        else:
            code, body = 200, (root / 'web-editor/dist/index.html').read_bytes()
            if mode == 'query-only' and self.headers.get('Authorization') != 'Bearer '+token and self.path != '/?token='+token:
                code = 401
        self.send_response(code); self.send_header('Content-Length', str(len(body))); self.end_headers()
        self.wfile.write(body)
http.server.HTTPServer(('127.0.0.1', int(os.environ['CAESURA_EDITOR_PORT'])), Handler).serve_forever()
'''

CLI_FIXTURE = r'''
import hashlib, json, shutil, sys
from pathlib import Path
package = Path(__file__).resolve().parents[1]
args = sys.argv[1:]
target = Path(args[args.index('--out') + 1])
if args[0] == 'create':
    source = package / 'tools/project_templates/basic'
    shutil.copytree(source, target)
    value = json.loads((target/'caesura.project.json').read_text())
    value['name'] = target.name
    (target/'caesura.project.json').write_text(json.dumps(value))
    print('(template from: ' + str(source) + ')')
else:
    if (package/'fixture-mode.txt').read_text() == 'bad-build': raise SystemExit(8)
    project = Path(args[1]); target.mkdir()
    binary = package / ('CaesuraAmeKAG.exe' if sys.platform == 'win32' else 'CaesuraAmeKAG')
    shutil.copy2(binary, target/binary.name)
    shutil.copy2(package/'fixture-mode.txt', target/'fixture-mode.txt')
    (target/'projects'/project.name).mkdir(parents=True)
    shutil.copy2(project/'story.ks', target/'projects'/project.name/'story.ks')
    (target/'projects'/project.name/'caesura-boot.lua').write_text('-- synthetic boot fixture')
    info = {'kind':'caesura-game-only','schema':1,'engine_binary':binary.name,
            'precompile':{'status':'ok','scene_count':1},'precompile_failures':[],
            'precompiled_scenes':['projects/'+project.name+'/story.ks'], 'scenes':['story.ks'],
            'entry_scene':'projects/'+project.name+'/story.ks',
            'boot_script':'projects/'+project.name+'/caesura-boot.lua',
            'assets_missing':[], 'capabilities':{'passed':True,'profile':{
             'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest()}}}
    (target/'BUILD-INFO.json').write_text(json.dumps(info))
    print('[build] ks_check: 1 scene(s) pass contracts')
    print('[build] precompile: 1/1 scene(s) cached into cache/ksc')
'''


class NativePackageRuntimeTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="u22-native-runtime-中文-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.package = self.root / "explicit package"
        self.package.mkdir()
        self.engine_name = "CaesuraAmeKAG.exe" if os.name == "nt" else "CaesuraAmeKAG"
        self.lua_name = "external/lua/lua.exe" if os.name == "nt" else "external/lua/lua"
        for relative in (self.engine_name, self.lua_name, "immutable.txt", "fixture-mode.txt"):
            path = self.package / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("synthetic executable boundary fixture" if relative != "fixture-mode.txt" else "")
        (self.package / "web-editor/dist").mkdir(parents=True)
        (self.package / "web-editor/dist/index.html").write_text("<!doctype html>Caesura Web Editor")
        shutil.copytree(ROOT / "tools/project_templates/basic", self.package / "tools/project_templates/basic")
        (self.package / "scripts").mkdir()
        (self.package / "scripts/caesura.py").write_text(CLI_FIXTURE, encoding="utf-8")
        self.engine_script = self.root / "engine_protocol_fixture.py"
        self.engine_script.write_text(ENGINE_FIXTURE, encoding="utf-8")
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            self.port = listener.getsockname()[1]
        self.config = {"schema":1,"sdl_linkage":"static","sdl_libraries":[],
                       "ffmpeg":False,"steam":False,"live2d":False}

    def invoke(self, *, mode="", attempt=None, launch=None, editor_port=None):
        self.assertIsNotNone(runtime, "Native package runtime controller is not implemented")
        (self.package / "fixture-mode.txt").write_text(mode)
        def native_boundary(executable, *args):
            if Path(executable).name == "AppRun":
                return [str(executable), *args]
            if Path(executable).name in ("lua", "lua.exe"):
                return [FIXTURE_PYTHON, "-I", "-c", "print('Lua 5.4 synthetic protocol fixture')"]
            return [FIXTURE_PYTHON, "-I", str(self.engine_script), *args]
        with patch.object(runtime, "_native_argv", native_boundary):
            return runtime.run_native_package(self.package, self.config, attempt or self.root / "attempt",
                                              python_executable=FIXTURE_PYTHON,
                                              command_timeout=12, readiness_timeout=1,
                                              editor_port=editor_port,
                                              **({"launch_relative_path":launch} if launch is not None else {}))

    def test_automatic_port_does_not_inherit_or_touch_an_unrelated_listener(self):
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', self.port)); listener.listen()
            with patch.dict(os.environ, {'CAESURA_EDITOR_PORT':str(self.port)}):
                report = self.invoke(editor_port=None)
            self.assertEqual(report['status'], 'RUNTIME_PASS', report)
            self.assertEqual(len(report['editor_endpoints']), 2)
            for endpoint in report['editor_endpoints']:
                self.assertNotEqual(endpoint['port'], self.port)
                self.assertEqual(endpoint['selection'], 'os_assigned')
                self.assertEqual(endpoint['race_policy'], 'FAIL_ON_FOREIGN_OWNER')
                self.assertFalse(runtime.loopback_listeners(endpoint['port']))
            self.assertTrue(runtime.loopback_listeners(self.port))
            for stage in report['stages'][:2]:
                self.assertEqual(stage['commands'][0]['observations']['port'], stage['editor_endpoint']['port'])

    def test_invalid_explicit_editor_port_fails_before_any_command(self):
        for index, port in enumerate((True, 0, -1, 65536, '9876')):
            with self.subTest(port=port):
                report = self.invoke(editor_port=port, attempt=self.root/f'invalid-{index}')
                self.assertEqual(report['status'], 'RUNTIME_FAIL')
                self.assertFalse(report['stages'])

    def test_unapproved_launcher_relative_path_is_refused_without_start(self):
        report = self.invoke(launch="../AppRun")
        self.assertEqual(report["status"], "RUNTIME_FAIL")
        self.assertEqual(report["runtime"], "NOT_RUN")
        self.assertFalse(report["stages"])

    if os.name == "nt":
        def test_apprun_is_not_claimed_on_windows(self):
            (self.package / "AppRun").write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            report = self.invoke(launch="AppRun")
            self.assertEqual(report["status"], "RUNTIME_FAIL")
            self.assertEqual(report["runtime"], "NOT_RUN")
            self.assertFalse(report["stages"])
    elif sys.platform.startswith("linux"):
        def install_apprun_fixture(self, *, change_directory=True):
            shutil.copy2(Path(FIXTURE_PYTHON).resolve(), self.package / self.engine_name)
            text = "#!/bin/sh\nAPPDIR=\"$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd -P)\"\n"
            if change_directory:
                text += 'cd "$APPDIR" || exit 1\n'
            text += 'exec "$APPDIR/CaesuraAmeKAG" -I ' + shlex.quote(str(self.engine_script)) + ' "$@"\n'
            (self.package / "AppRun").write_text(text, encoding="utf-8")
            (self.package / "AppRun").chmod(0o755)
            # The actual final-image process waits for the controller's identity
            # publication; no random sleep is used to make exec observable.
            barrier = """if root.name == 'runtime-package' and '--editor' not in sys.argv:
    import time
    ready = root.parent / 'commands/engine_frames/control/process.json'
    deadline = time.monotonic() + 4
    while not ready.exists() and time.monotonic() < deadline: time.sleep(0.01)
    assert ready.exists(), 'controller never observed final executable'
"""
            self.engine_script.write_text(ENGINE_FIXTURE.replace("if '--editor' not in sys.argv:\n", barrier + "if '--editor' not in sys.argv:\n"), encoding="utf-8")

        def test_actual_apprun_exec_runs_three_package_sessions_from_external_cwd(self):
            self.install_apprun_fixture()
            report = self.invoke(launch="AppRun")
            self.assertEqual(report["status"], "RUNTIME_PASS", report)
            self.assertEqual(report["launch_scope"]["fuse_mount_runtime"], "NOT_RUN")
            for stage in report["stages"][:3]:
                command = stage["commands"][0]
                self.assertEqual(Path(command["argv"][0]).name, "AppRun")
                self.assertEqual(Path(command["cwd"]), self.root / "attempt/work")
                self.assertEqual(command["run"]["exec_transition"]["status"], "VERIFIED")
                self.assertEqual(Path(command["run"]["process"]["executable"]), self.root / "attempt/runtime-package/CaesuraAmeKAG")
            for stage in report["stages"][3:]:
                self.assertFalse(any(Path(command["argv"][0]).name == "AppRun" for command in stage["commands"]))

        def test_apprun_that_does_not_change_to_payload_cwd_fails(self):
            self.install_apprun_fixture(change_directory=False)
            report = self.invoke(launch="AppRun")
            self.assertEqual(report["status"], "RUNTIME_FAIL", report)
            self.assertEqual(report["cleanup"], "COMPLETE", report)
            self.assert_editor_ports_closed(report)

    def test_missing_controller_is_red_then_protocol_success_preserves_source(self):
        report = self.invoke()
        self.assertEqual(report["status"], "RUNTIME_PASS", report)
        self.assertTrue(report["source_stable"])
        self.assertTrue(report["runtime_copy_stable"])
        self.assertEqual(len(report["stages"]), 5)
        self.assertEqual(report["cleanup"], "COMPLETE")
        self.assertFalse((self.package / ".caesura-editor-token").exists())
        saved = json.loads((self.root / "attempt/native-runtime.json").read_text(encoding="utf-8"))
        self.assertEqual(saved, report)
        generated = (self.root / "attempt/runtime-package/.caesura-editor-token").read_text()
        self.assertNotIn(generated, json.dumps(report))
        for stage in report["stages"]:
            for command in stage.get("commands", []):
                for channel in ("stdout", "stderr"):
                    self.assertEqual(len(command[channel]["sha256"]), 64)
                    actual = (self.root / "attempt" / command[channel]["path"]).read_bytes()
                    self.assertEqual(hashlib.sha256(actual).hexdigest(), command[channel]["sha256"])

    def test_foreign_listener_is_preserved_and_no_command_starts(self):
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", self.port)); listener.listen()
            report = self.invoke(editor_port=self.port)
            self.assertEqual(report["status"], "RUNTIME_FAIL")
            self.assertFalse(report["stages"])
            with socket.create_connection(("127.0.0.1", self.port), timeout=2):
                accepted, _ = listener.accept(); accepted.close()

    def test_listener_winning_release_race_is_not_contacted_or_terminated(self):
        execute = runtime._execute
        with socket.socket() as listener:
            def take_port(*args, **kwargs):
                listener.bind(('127.0.0.1', kwargs['editor_port']))
                listener.listen()
                return execute(*args, **kwargs)
            with patch.object(runtime, '_execute', take_port):
                report = self.invoke()
            self.assertEqual(report['status'], 'RUNTIME_FAIL')
            command = report['stages'][0]['commands'][0]
            self.assertEqual(command['run']['owned_tree_cleanup'], 'COMPLETE')
            self.assertTrue(runtime.loopback_listeners(listener.getsockname()[1]))
            listener.settimeout(0.1)
            with self.assertRaises(TimeoutError):
                listener.accept()  # The controller never sent HTTP to this owner.

    def assert_editor_ports_closed(self, report):
        for endpoint in report.get('editor_endpoints', []):
            self.assertFalse(runtime.loopback_listeners(endpoint['port']))

    def test_missing_packaged_lua_cannot_fall_back_to_host(self):
        (self.package / self.lua_name).unlink()
        report = self.invoke()
        self.assertEqual(report["status"], "RUNTIME_FAIL")
        self.assertFalse(report["stages"])

    def test_existing_attempt_is_not_reused(self):
        attempt = self.root / "existing"; attempt.mkdir()
        note = attempt / "user-note"; note.write_text("keep")
        report = self.invoke(attempt=attempt)
        self.assertEqual(report["status"], "RUNTIME_FAIL")
        self.assertEqual(note.read_text(), "keep")
        self.assertFalse((attempt / "native-runtime.json").exists())

    def test_auth_failure_stops_owned_editor_and_closes_port(self):
        report = self.invoke(mode="open-api")
        self.assertEqual(report["status"], "RUNTIME_FAIL")
        self.assertEqual(report["cleanup"], "COMPLETE")
        self.assert_editor_ports_closed(report)

    def test_editor_early_exit_is_not_readiness(self):
        report = self.invoke(mode="early-exit")
        self.assertEqual(report["status"], "RUNTIME_FAIL")
        self.assertEqual(report["cleanup"], "COMPLETE")

    def test_missing_generated_token_fails_and_cleans_up(self):
        report = self.invoke(mode="missing-token")
        self.assertEqual(report["status"], "RUNTIME_FAIL")
        self.assertEqual(report["cleanup"], "COMPLETE")
        self.assert_editor_ports_closed(report)

    def test_changed_original_file_in_runtime_copy_is_not_ignored(self):
        report = self.invoke(mode="mutate")
        self.assertEqual(report["status"], "RUNTIME_FAIL")
        self.assertTrue(report["source_stable"])
        self.assertFalse(report["runtime_copy_stable"])

    def test_build_nonzero_and_disabled_renderer_cannot_pass(self):
        for mode in ("bad-build", "render-disabled"):
            with self.subTest(mode=mode):
                report = self.invoke(mode=mode, attempt=self.root / mode)
                self.assertEqual(report["status"], "RUNTIME_FAIL")

    def test_later_stage_cannot_replace_or_delete_bound_command_evidence(self):
        for relative, action in (("stdout.log", "replace"), ("stderr.log", "delete"), ("control/run.json", "replace")):
            with self.subTest(relative=relative, action=action):
                mutation = ("target.unlink()" if action == "delete" else "target.write_text('changed after binding', encoding='utf-8')")
                injected = "if '--editor' not in sys.argv:\n    if (root/'BUILD-INFO.json').exists():\n        target = root.parent/'commands/editor_explicit_token'/" + repr(relative) + "\n        " + mutation + "\n"
                self.engine_script.write_text(ENGINE_FIXTURE.replace("if '--editor' not in sys.argv:\n", injected, 1), encoding="utf-8")
                attempt = self.root / ("evidence-" + relative.replace("/", "-") + "-" + action)
                report = self.invoke(attempt=attempt)
                self.assertTrue(all(stage["status"] == "PASS" for stage in report["stages"]), report)
                self.assertEqual(report["status"], "RUNTIME_FAIL", report)
                self.assertEqual(report["cleanup"], "COMPLETE")
                self.assertFalse(report["evidence_stable"])
                self.assertTrue((attempt / "native-runtime.json").is_file())
                command = report["stages"][0]["commands"][0]
                evidence = command["receipt" if relative == "control/run.json" else relative.split(".")[0]]
                if action == "replace":
                    actual = hashlib.sha256((attempt/evidence["path"]).read_bytes()).hexdigest()
                    self.assertNotEqual(evidence["sha256"], actual)

    if os.name == "nt":
        def test_required_library_can_load_after_first_observation_within_deadline(self):
            library = self.package / "u22-delayed-version.dll"
            shutil.copy2(Path(os.environ["SystemRoot"]) / "System32/version.dll", library)
            self.config.update(sdl_linkage="shared", sdl_libraries=[library.name])
            prefix = "import ctypes, time\nrelease = Path(os.environ['TEMP']) / ('load-library-' + root.name)\nif '--editor' not in sys.argv:\n    deadline = time.monotonic() + 5\n    while not release.exists():\n        if time.monotonic() >= deadline: raise RuntimeError('library barrier expired')\n        time.sleep(.005)\nmodule = ctypes.WinDLL(str(root/'u22-delayed-version.dll'))\nif '--editor' not in sys.argv:\n    observed = Path(os.environ['TEMP']) / ('library-observed-' + root.name)\n    while not observed.exists():\n        if time.monotonic() >= deadline: raise RuntimeError('observation barrier expired')\n        time.sleep(.005)\n"
            self.engine_script.write_text(ENGINE_FIXTURE.replace("if '--editor' not in sys.argv:\n", prefix + "if '--editor' not in sys.argv:\n", 1), encoding="utf-8")
            cli = CLI_FIXTURE.replace("shutil.copy2(binary, target/binary.name)", "shutil.copy2(binary, target/binary.name)\n    shutil.copy2(package/'u22-delayed-version.dll', target/'u22-delayed-version.dll')")
            (self.package / "scripts/caesura.py").write_text(cli, encoding="utf-8")
            observed_pending = []
            inspect = runtime._inspect_libraries
            def release_after_missing(identity, package, required):
                try:
                    result = inspect(identity, package, required)
                    if (self.root/'attempt/temp'/('load-library-' + package.name)).exists():
                        (self.root/'attempt/temp'/('library-observed-' + package.name)).write_text('this owned image was observed')
                    return result
                except runtime.RuntimeContractError:
                    observed_pending.append(str(package))
                    (self.root/'attempt/temp'/('load-library-' + package.name)).write_text('release this owned fixture')
                    raise
            with patch.object(runtime, "_inspect_libraries", release_after_missing):
                report = self.invoke()
            self.assertEqual(report["status"], "RUNTIME_PASS", report)
            for stage_name in ("engine_frames", "created_game_frames"):
                stage = next(item for item in report["stages"] if item["name"] == stage_name)
                self.assertEqual(stage["commands"][0]["observations"]["loaded_modules"]["status"], "VERIFIED")
            self.assertIn(str(self.root/'attempt/runtime-package'), observed_pending)
            self.assertIn(str(self.root/'attempt/作品 输出'), observed_pending)

        def test_foreign_library_and_changed_identity_do_not_wait_for_deadline(self):
            import ctypes
            name = "u22-shadow-version.dll"
            foreign = self.root / name
            expected = self.package / name
            shutil.copy2(Path(os.environ["SystemRoot"]) / "System32/version.dll", foreign)
            shutil.copy2(foreign, expected)
            loaded = ctypes.WinDLL(str(foreign))
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.FreeLibrary.argtypes = [ctypes.c_void_p]
            kernel.FreeLibrary.restype = ctypes.c_int
            try:
                identity = runtime.process_identity(os.getpid())
                with patch.object(runtime.time, "sleep", side_effect=AssertionError("Fatal observation must not wait")):
                    with self.assertRaises(runtime.RuntimeContractError) as caught:
                        runtime._loaded_libraries(identity, self.package, [name], time.monotonic() + 10)
                    self.assertIn("second source", caught.exception.observations["loaded_modules"]["error"])
                    changed = runtime.ProcessIdentity(identity.pid, identity.created + "1", identity.executable, identity.source)
                    with self.assertRaises(runtime.RuntimeContractError) as caught:
                        runtime._loaded_libraries(changed, self.package, [name], time.monotonic() + 10)
                    self.assertIn("owner changed", caught.exception.observations["loaded_modules"]["error"])
            finally:
                self.assertTrue(kernel.FreeLibrary(loaded._handle))

        def test_unreadable_bound_evidence_still_writes_failed_final_receipt(self):
            import ctypes
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
            kernel.CreateFileW.restype = ctypes.c_void_p
            kernel.CloseHandle.argtypes = [ctypes.c_void_p]
            handles = []
            execute = runtime._execute
            def hold_earlier_evidence(*args, **kwargs):
                result = execute(*args, **kwargs)
                if args[1] == "created_game_frames":
                    path = self.root/'attempt/commands/editor_explicit_token/stdout.log'
                    handle = kernel.CreateFileW(str(path), 0x80000000, 0, None, 3, 0x80, None)
                    self.assertNotEqual(handle, ctypes.c_void_p(-1).value, ctypes.get_last_error())
                    handles.append(handle)
                return result
            try:
                with patch.object(runtime, "_execute", hold_earlier_evidence):
                    report = self.invoke()
                self.assertEqual(report["status"], "RUNTIME_FAIL", report)
                self.assertFalse(report["evidence_stable"])
                saved = json.loads((self.root/'attempt/native-runtime.json').read_text(encoding="utf-8"))
                self.assertEqual(saved, report)
            finally:
                for handle in handles:
                    kernel.CloseHandle(handle)

    def test_actual_current_process_module_paths_are_observed(self):
        self.assertIsNotNone(runtime, "Native package runtime controller is not implemented")
        identity = runtime.process_identity(os.getpid())
        report = runtime.observe_loaded_modules(identity)
        self.assertEqual(report["status"], "OBSERVED")
        self.assertIn(str(Path(FIXTURE_PYTHON).resolve()), report["paths"])

    def test_file_presence_does_not_prove_required_library_loaded(self):
        (self.package / "unused-library.dll").write_bytes(b"present but never loaded")
        self.config.update(sdl_linkage="shared", sdl_libraries=["unused-library.dll"])
        report = self.invoke()
        self.assertEqual(report["status"], "RUNTIME_FAIL")
        command = report["stages"][0]["commands"][0]
        self.assertEqual(command["observations"]["loaded_modules"]["status"], "NOT_VERIFIED")
        self.assertEqual(report["cleanup"], "COMPLETE")

    def test_created_game_original_scene_cannot_change_during_execution(self):
        report = self.invoke(mode="mutate-created-game")
        self.assertEqual(report["status"], "RUNTIME_FAIL")
        self.assertTrue(report["source_stable"])
        self.assertTrue(report["runtime_copy_stable"])
        self.assertFalse(report["created_game_copy_stable"])

    def test_existing_browser_query_token_navigation_contract_is_preserved(self):
        report = self.invoke(mode="query-only")
        self.assertEqual(report["status"], "RUNTIME_PASS", report["errors"])
        self.assertEqual(report["stages"][0]["commands"][0]["observations"]["browser_mode"], "query_token")

    def execute_fixture(self, *, timeout, monitor=None, controlled=False):
        self.assertIsNotNone(runtime, "Native package runtime controller is not implemented")
        attempt = self.root / "direct"; attempt.mkdir()
        env = runtime.native_env(self.package, engine=self.package/self.engine_name,
                                 lua=self.package/self.lua_name, work=self.root,
                                 home=self.root, temp=self.root)
        return runtime._execute(attempt, "child", [FIXTURE_PYTHON, "-I", "-c", "import time; time.sleep(30)"],
                                self.root, env, timeout, 2, monitor=monitor, controlled=controlled,
                                editor_port=self.port if controlled else None)

    def test_actual_command_timeout_records_reaped_failure(self):
        report = self.execute_fixture(timeout=0.3)
        self.assertFalse(report["passed"])
        self.assertEqual(report["run"]["status"], "TIMED_OUT")
        self.assertEqual(report["run"]["owned_tree_cleanup"], "COMPLETE")

    def test_stop_request_write_error_still_collects_owned_completion(self):
        def monitor(identity, deadline):
            stop = self.root / "direct/commands/child/control/stop"
            stop.write_text("pre-existing control request")
            return {"process":identity.pid}
        report = self.execute_fixture(timeout=5, monitor=monitor, controlled=True)
        self.assertFalse(report["passed"])
        self.assertEqual(report["run"]["owned_tree_cleanup"], "COMPLETE")
        self.assertEqual(report["run"]["status"], "STOPPED")


class HttpSmokeStartupTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='caesura-http-smoke-contract-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()

    def invoke(self, *, port=None):
        env = dict(os.environ)
        env.pop('CAESURA_EDITOR_PORT', None)
        if port is not None:
            env['CAESURA_EDITOR_PORT'] = str(port)
        # Actual Python exits nonzero for --editor. It is an executable failure
        # fixture, not an Engine or GPU simulation.
        return subprocess.run([FIXTURE_PYTHON, '-B', '-X', 'utf8', str(ROOT/'tests/headless_http_smoke.py'),
            FIXTURE_PYTHON, '--output', str(self.root/'evidence')], env=env,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)

    def test_non_gpu_startup_failure_is_fail_with_retained_stderr(self):
        result = self.invoke()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertNotIn(b'NO GPU', result.stdout + result.stderr)
        self.assertRegex((self.root/'evidence/engine.stderr.log').read_bytes(), rb'(?i)unknown option:? --editor')
        report = json.loads((self.root/'evidence/result.json').read_text(encoding='utf-8'))
        self.assertEqual(report['status'], 'FAIL')
        self.assertEqual(report['actual_exit_code'], 2)
        self.assertFalse(report['forced_kill'])

    def test_occupied_explicit_port_is_not_replaced_and_listener_survives(self):
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0)); listener.listen()
            port = listener.getsockname()[1]
            result = self.invoke(port=port)
            self.assertEqual(result.returncode, 1, result.stdout)
            report = json.loads((self.root/'evidence/result.json').read_text(encoding='utf-8'))
            self.assertEqual(report['requested_port'], port)
            self.assertIsNone(report['process'])
            self.assertTrue(runtime.loopback_listeners(port))

    def test_error_response_is_read_and_closed_before_final_owner_check(self):
        spec = importlib.util.spec_from_file_location('http_smoke_contract', ROOT/'tests/headless_http_smoke.py')
        smoke = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(smoke)
        body = b'{"error":"synthetic owned HTTP response"}'
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass
            def do_GET(self):
                self.send_response(int(self.path[1:]))
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
        with http.server.HTTPServer(('127.0.0.1', 0), Handler) as server:
            worker = threading.Thread(target=server.serve_forever, daemon=True)
            worker.start()
            smoke.port = server.server_address[1]
            smoke.proc = smoke._ObservedEngine(runtime.process_identity(os.getpid()))
            try:
                for code in (400, 401, 500):
                    with self.subTest(status=code):
                        response = []
                        observations = []
                        def verify(identity, port):
                            runtime.verify_owned_listener(identity, port)
                            observations.append(response[0].closed if response else 'before')
                        with patch.object(smoke, 'verify_owned_listener', verify):
                            with smoke._open(f'http://127.0.0.1:{smoke.port}/{code}', timeout=2) as reply:
                                response.append(reply)
                                self.assertEqual(reply.status, code)
                                self.assertEqual(reply.read(), body)
                        self.assertEqual(observations, ['before', True])
            finally:
                server.shutdown()
                worker.join(timeout=3)
            self.assertFalse(worker.is_alive())

    def test_redirect_does_not_contact_another_endpoint_or_forward_token(self):
        spec = importlib.util.spec_from_file_location('http_smoke_redirect', ROOT/'tests/headless_http_smoke.py')
        smoke = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(smoke)
        received = []
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass
            def do_GET(self):
                if self.path == '/target':
                    received.append(self.headers.get('Authorization'))
                    self.send_response(200)
                    body = b'{"status":"ok"}'
                else:
                    self.send_response(302)
                    self.send_header('Location', f'http://127.0.0.1:{target.server_port}/target')
                    body = b''
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
        with http.server.HTTPServer(('127.0.0.1', 0), Handler) as target, \
             http.server.HTTPServer(('127.0.0.1', 0), Handler) as origin:
            workers = [threading.Thread(target=server.serve_forever, daemon=True) for server in (target, origin)]
            for worker in workers:
                worker.start()
            smoke.port = origin.server_port
            smoke.BASE = f'http://127.0.0.1:{smoke.port}'
            smoke.proc = smoke._ObservedEngine(runtime.process_identity(os.getpid()))
            try:
                status, body = smoke.request('/api/ping')
                self.assertEqual((status, received), (302, []))
            finally:
                for server in (target, origin):
                    server.shutdown()
                for worker in workers:
                    worker.join(timeout=3)
            self.assertTrue(all(not worker.is_alive() for worker in workers))


if __name__ == "__main__":
    unittest.main(verbosity=2)
