"""Real Python/HTTP fixtures exercise orchestration, never Engine acceptance.

Only native Engine/Lua executable invocation is replaced with an explicit Python
protocol fixture. The owned runner, identity/socket checks, HTTP requests, file
copies, template provenance and mutation checks remain real.
"""
from __future__ import annotations

import importlib.util
import base64
import hashlib
import http.server
from contextlib import ExitStack, redirect_stderr
import io
import json
import os
from pathlib import Path
import shlex
import shutil
import socket
from socketserver import TCPServer
import subprocess
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
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


def _preserve_actual_observer_failure(test_id, error):
    """Persist this failed observation only; never mask it with a capture error."""
    try:
        selected = Path(os.environ.get("RUNNER_TEMP") or tempfile.gettempdir())
        if not selected.is_absolute():
            raise ValueError("Observer diagnostic temporary root must be absolute")
        parent = selected.resolve(strict=True) / "caesura-native-observer"
        parent.mkdir(mode=0o700, exist_ok=True)
        if parent.resolve(strict=True) != parent:
            raise ValueError("Observer diagnostic directory must not be a link")
        directory = Path(tempfile.mkdtemp(prefix="observation-", dir=parent))
        path = directory / "observation.json"
        payload = {"schema_version": 1, "kind": "native-observer-test-failure",
                   "test_id": test_id,
                   "error": {"type": type(error).__name__, "message": str(error)},
                   "module_observation": getattr(error, "module_observation", None)}
        data = (json.dumps(payload, ensure_ascii=True, indent=2) + "\n").encode("utf-8")
        with path.open("xb") as stream:
            stream.write(data)
        print("[native-observer-diagnostic] " + json.dumps({
            "path": str(path), "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}),
            file=sys.stderr, flush=True)
    except Exception as capture_error:
        # Diagnostics are secondary to the original observer exception. Even a
        # broken stderr must not replace the failure the test is reporting.
        try:
            print("[native-observer-diagnostic] capture failed: " +
                  type(capture_error).__name__ + ": " + str(capture_error), file=sys.stderr, flush=True)
        except Exception:
            pass


class LoopbackHTTPServer(http.server.HTTPServer):
    def server_bind(self):
        TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]

ENGINE_FIXTURE = r'''
import http.server, json, os, secrets, sys
from socketserver import TCPServer
from pathlib import Path
root = Path.cwd()
mode = (root / 'fixture-mode.txt').read_text() if (root / 'fixture-mode.txt').exists() else ''
if '--editor' not in sys.argv:
    print('[KAG Runner] Started synthetic protocol scene', flush=True)
    audio = sys.argv[sys.argv.index('--audio-output') + 1]
    physical = 'NOT_RUN' if audio == 'software' else 'NOT_VERIFIED'
    print('[Audio] Output mode: ' + audio + '; physical_device=' + physical)
    print('[Audio] SoLoud initialized: 3 buses (BGM, VOICE, SE) ready.')
    if audio == 'software':
        stats = dict(frames=480, samples=960, nonzero_samples=500, nonfinite_samples=0,
                     peak=0.5, absolute_energy=10.0, sample_rate=48000, channels=2,
                     physical_device='NOT_RUN', saturated=False)
        if mode == 'audio-empty': stats.update(nonzero_samples=0, peak=0, absolute_energy=0)
        if mode != 'audio-missing': print('[Audio] Software mix stats: ' + json.dumps(stats))
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
class LoopbackHTTPServer(http.server.HTTPServer):
    def server_bind(self):
        TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]
LoopbackHTTPServer(('127.0.0.1', int(os.environ['CAESURA_EDITOR_PORT'])), Handler).serve_forever()
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


class AudioEvidenceTests(unittest.TestCase):
    def text(self, **changes):
        stats = dict(frames=480, samples=960, nonzero_samples=500, nonfinite_samples=0,
                     peak=0.5, absolute_energy=10.0, sample_rate=48000, channels=2,
                     physical_device='NOT_RUN', saturated=False)
        stats.update(changes)
        return ('[Audio] Output mode: software; physical_device=NOT_RUN\n'
                '[Audio] SoLoud initialized: 3 buses (BGM, VOICE, SE) ready.\n'
                '[Audio] Software mix stats: ' + json.dumps(stats) + '\n')

    def test_pcm_statistics_contract_and_silent_created_game(self):
        result = runtime._audio_evidence(self.text(), 'software', require_signal=True)
        self.assertEqual(result['physical_device'], 'NOT_RUN')
        self.assertEqual(result['statistics']['frames'], 480)
        runtime._audio_evidence(self.text(nonzero_samples=0, peak=0, absolute_energy=0),
                                'software', require_signal=False)

    def test_missing_duplicate_wrong_mode_and_bad_pcm_statistics_fail(self):
        invalid = [self.text().replace('Output mode: software', 'Output mode: device'),
                   self.text() + self.text(), self.text().split('[Audio] Software mix stats:')[0],
                   self.text() + '[BackendRegistry] Using NullAudioBackend.\n']
        invalid += [self.text(**change) for change in (
            {'frames':0, 'samples':0}, {'frames':True}, {'samples':959},
            {'nonzero_samples':961}, {'nonzero_samples':0}, {'nonfinite_samples':1},
            {'peak':float('nan')}, {'absolute_energy':float('inf')}, {'absolute_energy':-1},
            {'sample_rate':44100}, {'channels':1}, {'physical_device':'VERIFIED'},
            {'saturated':True})]
        for text in invalid:
            with self.subTest(text=text), self.assertRaises((runtime.RuntimeContractError, ValueError)):
                runtime._audio_evidence(text, 'software', require_signal=True)


class MacMappedLibraryTests(unittest.TestCase):
    """Real files/unlink, with only macOS lsof/identity/path spelling replaced.

    These tests run on every host; they do not claim an actual macOS mapping.
    The original hosted failure supplies that platform evidence separately.
    """
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="u22-mac-mapped-library-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.package = self.root / "package"
        self.package.mkdir()
        self.library = self.package / "libSDL3.0.dylib"
        self.library.write_bytes(b"required library byte fixture")
        self.engine = self.package / "CaesuraAmeKAG"
        self.engine.write_bytes(b"owned engine byte fixture")
        self.cache = self.root / ".plist-cache.transient"
        self.cache.write_bytes(b"unrelated mapped cache fixture")
        self.lsof = self.root / "lsof"
        self.lsof.write_bytes(b"tool boundary fixture")
        self.identity = runtime.ProcessIdentity(1234, "creation-fixture", str(self.engine), "fixture")

    def inspect(self, paths, *, exit_code=0, identities=None):
        # Map explicit POSIX lsof names into real fixture paths on Windows as
        # well as POSIX. Path resolution and file reads themselves stay real.
        prefix = "/lsof-fixture/"
        def host_path(value):
            value = str(value)
            if value == "/usr/sbin/lsof":
                return self.lsof
            if value.startswith(prefix):
                return self.root / value[len(prefix):]
            return Path(value)
        output = "p1234\n" + "".join("n" + prefix + path.relative_to(self.root).as_posix() + "\n" for path in paths)
        with ExitStack() as stack:
            stack.enter_context(patch.object(runtime, "os", SimpleNamespace(name="posix")))
            stack.enter_context(patch.object(runtime, "sys", SimpleNamespace(platform="darwin")))
            stack.enter_context(patch.object(runtime, "Path", host_path))
            stack.enter_context(patch.object(runtime, "process_identity", side_effect=identities or [self.identity, self.identity]))
            call = stack.enter_context(patch.object(runtime.subprocess, "run",
                return_value=SimpleNamespace(returncode=exit_code, stdout=output.encode("utf-8"), stderr=b"")))
            result = runtime._inspect_libraries(self.identity, self.package, [self.library.name])
            self.assertEqual(call.call_args.args[0], [str(self.lsof), "-a", "-p", "1234", "-d", "txt", "-Fn"])
            return result

    def test_disappeared_unrelated_txt_does_not_hide_required_library(self):
        self.cache.unlink()  # Deterministic disappearance after lsof's snapshot.
        report = self.inspect([self.engine, self.cache, self.library])
        self.assertEqual(report["status"], "VERIFIED")
        self.assertEqual(report["required"][0]["sha256"], hashlib.sha256(self.library.read_bytes()).hexdigest())
        self.assertEqual(report["missing_paths"][0]["path"], str(self.cache))
        self.assertIn(str(self.cache), report["paths"])
        self.assertIn("FileNotFoundError", report["missing_paths"][0]["error"])

    def test_required_library_disappearance_remains_fatal(self):
        self.library.unlink()
        with self.assertRaises((FileNotFoundError, runtime.RuntimeContractError)):
            self.inspect([self.engine, self.library])

    def test_reappearing_required_path_cannot_replace_missing_mapping(self):
        self.library.unlink()
        observe = runtime.observe_loaded_modules
        def restore_after_observation(identity):
            result = observe(identity)
            self.library.write_bytes(b"replacement after observed disappearance")
            return result
        with patch.object(runtime, "observe_loaded_modules", restore_after_observation):
            with self.assertRaises((FileNotFoundError, runtime.RuntimeContractError)):
                self.inspect([self.engine, self.library])

    def test_unreadable_required_library_remains_fatal(self):
        open_file = Path.open
        def deny_required(path, *args, **kwargs):
            if path == self.library:
                raise PermissionError("required library read denied by fixture boundary")
            return open_file(path, *args, **kwargs)
        with patch.object(Path, "open", deny_required), self.assertRaises(PermissionError):
            self.inspect([self.engine, self.library])

    def test_foreign_same_name_remains_fatal_even_after_disappearing(self):
        foreign = self.root / self.library.name
        for disappeared in (False, True):
            with self.subTest(disappeared=disappeared):
                foreign.write_bytes(self.library.read_bytes())
                if disappeared:
                    foreign.unlink()
                with self.assertRaises((FileNotFoundError, runtime.RuntimeContractError)):
                    self.inspect([self.engine, foreign, self.library])

    def test_unrelated_permission_error_is_not_swallowed(self):
        resolve = Path.resolve
        def deny_cache(path, *args, **kwargs):
            if path == self.cache:
                raise PermissionError("unrelated path resolution denied by fixture boundary")
            return resolve(path, *args, **kwargs)
        with patch.object(Path, "resolve", deny_cache), self.assertRaises(PermissionError):
            self.inspect([self.engine, self.cache, self.library])

    def test_inaccessible_unrelated_txt_preserves_error_and_required_library(self):
        resolve = Path.resolve
        def deny_strict_cache(path, *args, **kwargs):
            if path == self.cache and kwargs.get("strict"):
                raise PermissionError(13, "Permission denied", str(path))
            return resolve(path, *args, **kwargs)
        with patch.object(Path, "resolve", deny_strict_cache):
            report = self.inspect([self.engine, self.cache, self.library])
        self.assertEqual(report["status"], "VERIFIED")
        self.assertEqual(report["required"][0]["sha256"], hashlib.sha256(self.library.read_bytes()).hexdigest())
        self.assertEqual(report["missing_paths"], [])
        self.assertIn(str(self.cache), report["paths"])
        self.assertEqual(report["inaccessible_paths"][0]["path"], str(self.cache))
        self.assertIn("PermissionError", report["inaccessible_paths"][0]["error"])
        self.assertIn("[Errno 13]", report["inaccessible_paths"][0]["error"])

    def test_required_mapping_resolution_denial_remains_fatal_after_access_returns(self):
        resolve = Path.resolve
        denied = False
        def deny_once(path, *args, **kwargs):
            nonlocal denied
            if path == self.library and kwargs.get("strict") and not denied:
                denied = True
                raise PermissionError(13, "Permission denied", str(path))
            return resolve(path, *args, **kwargs)
        with patch.object(Path, "resolve", deny_once):
            with self.assertRaisesRegex(runtime.RuntimeContractError, "Required library mapping inaccessible"):
                self.inspect([self.engine, self.library])

    def test_inaccessible_foreign_same_name_mapping_remains_fatal(self):
        foreign = self.root / self.library.name
        foreign.write_bytes(self.library.read_bytes())
        resolve = Path.resolve
        def deny_foreign(path, *args, **kwargs):
            if path == foreign and kwargs.get("strict"):
                raise PermissionError(13, "Permission denied", str(path))
            return resolve(path, *args, **kwargs)
        with patch.object(Path, "resolve", deny_foreign):
            with self.assertRaisesRegex(runtime.RuntimeContractError, "second source for required library"):
                self.inspect([self.engine, foreign, self.library])

    if os.name != "nt":
        def test_inaccessible_foreign_declared_name_with_required_symlink_is_rejected(self):
            actual = self.package / "libSDL3.actual.dylib"
            self.library.rename(actual)
            self.library.symlink_to(actual.name)
            self.assertEqual(self.inspect([self.engine, self.library])["status"], "VERIFIED")
            foreign = self.root / self.library.name
            foreign.write_bytes(actual.read_bytes())
            resolve = Path.resolve
            def deny_foreign(path, *args, **kwargs):
                if path == foreign and kwargs.get("strict"):
                    raise PermissionError(13, "Permission denied", str(path))
                return resolve(path, *args, **kwargs)
            with patch.object(Path, "resolve", deny_foreign):
                with self.assertRaisesRegex(runtime.RuntimeContractError, "second source for required library"):
                    self.inspect([self.engine, foreign, self.library])

        def test_inaccessible_foreign_alias_cannot_hide_behind_different_target_name(self):
            actual = self.package / "libSDL3.actual.dylib"
            self.library.rename(actual)
            self.library.symlink_to(actual.name)
            foreign_target = self.root / "foreign-image.bin"
            foreign_target.write_bytes(actual.read_bytes())
            foreign = self.root / self.library.name
            foreign.symlink_to(foreign_target.name)
            resolve = Path.resolve
            def deny_foreign(path, *args, **kwargs):
                if path == foreign and kwargs.get("strict"):
                    raise PermissionError(13, "Permission denied", str(path))
                return resolve(path, *args, **kwargs)
            with patch.object(Path, "resolve", deny_foreign):
                with self.assertRaisesRegex(runtime.RuntimeContractError, "second source for required library"):
                    self.inspect([self.engine, foreign, self.library])

    def test_failed_observer_and_changed_owner_remain_fatal(self):
        with self.assertRaisesRegex(runtime.RuntimeContractError, "observation failed"):
            self.inspect([self.engine, self.library], exit_code=1)
        changed = runtime.ProcessIdentity(self.identity.pid, "different-creation", str(self.engine), "fixture")
        with self.assertRaisesRegex(runtime.RuntimeContractError, "owner changed"):
            self.inspect([self.engine, self.library], identities=[self.identity, changed])


class MacLsofFieldProtocolTests(unittest.TestCase):
    """Replay real lsof C-locale wire spellings through an owned command seam.

    The byte vectors were observed with Linux lsof 4.99.4, both -Fn and -F0n.
    This tests the macOS observer's parser/command contract on every host; it
    does not represent a real macOS mapping or validate a native Engine.
    """
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="u22-lsof-field-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.package = self.root / "作品 输出"
        self.package.mkdir()
        self.library = self.package / "libSDL3.0.dylib"
        self.library.write_bytes(b"required SDL image protocol fixture")
        self.engine = self.package / "CaesuraAmeKAG"
        self.engine.write_bytes(b"owned engine protocol fixture")
        self.identity = runtime.ProcessIdentity(1234, "creation-fixture", str(self.engine), "fixture")
        self.capture = self.root / "command.json"
        self.tool = self.root / "lsof-command.py"
        self.tool.write_text(
            "import json,os,pathlib,sys\n"
            "capture,lines,nuls,code=sys.argv[1:5]\n"
            "args=sys.argv[5:]\n"
            "pathlib.Path(capture).write_text(json.dumps(dict(argv=args,environment=dict(os.environ))),encoding='utf-8')\n"
            "null_fields=any(a.startswith('-F') and '0' in a[2:] for a in args)\n"
            "sys.stdout.buffer.write(pathlib.Path(nuls if null_fields else lines).read_bytes())\n"
            "sys.stderr.write('controlled lsof failure' if int(code) else '')\n"
            "raise SystemExit(int(code))\n", encoding="utf-8")
        # Exact C-locale n-field bytes observed from the real lsof command;
        # expected Unicode is supplied separately by the actual filesystem.
        self.unicode_directory = br"\xe4\xbd\x9c\xe5\x93\x81 \xe8\xbe\x93\xe5\x87\xba"

    def inspect(self, wire_paths, *, exit_code=0, loaded=False):
        fields = [b"p1234"] + [b"n/lsof-fixture/" + path for path in wire_paths]
        lines, nuls = self.root / "lines.bin", self.root / "nuls.bin"
        lines.write_bytes(b"\n".join(fields) + b"\n")
        nuls.write_bytes(b"\x00\n".join(fields) + b"\x00\n")
        prefix = "/lsof-fixture/"
        def host_path(value):
            value = str(value)
            if value == "/usr/sbin/lsof":
                return self.tool
            if value.startswith(prefix):
                return self.root / value[len(prefix):]
            return Path(value)
        actual_run = subprocess.run
        def command(argv, **kwargs):
            self.assertEqual(argv[0], str(self.tool))
            # Execute an actual bounded child with the production-selected
            # environment; only the lsof executable itself is substituted.
            return actual_run([FIXTURE_PYTHON, "-B", str(self.tool), str(self.capture),
                               str(lines), str(nuls), str(exit_code), *argv[1:]], **kwargs)
        with ExitStack() as stack:
            stack.enter_context(patch.object(runtime, "os", SimpleNamespace(name="posix")))
            stack.enter_context(patch.object(runtime, "sys", SimpleNamespace(platform="darwin")))
            stack.enter_context(patch.object(runtime, "Path", host_path))
            stack.enter_context(patch.object(runtime, "process_identity", return_value=self.identity))
            stack.enter_context(patch.object(runtime.subprocess, "run", side_effect=command))
            inspect = runtime._loaded_libraries if loaded else runtime._inspect_libraries
            return inspect(self.identity, self.package, [self.library.name])

    def test_unicode_n_field_identifies_actual_required_image(self):
        result = self.inspect([self.unicode_directory + b"/CaesuraAmeKAG",
                               self.unicode_directory + b"/libSDL3.0.dylib"])
        self.assertEqual(result["status"], "VERIFIED")
        self.assertEqual(result["required"][0]["resolved_path"], str(self.library))
        self.assertEqual(result["required"][0]["sha256"], hashlib.sha256(self.library.read_bytes()).hexdigest())

    def test_observer_command_has_an_explicit_stable_c_locale(self):
        self.package = self.root / "ascii"
        self.package.mkdir()
        self.library = self.package / "libSDL3.0.dylib"
        self.library.write_bytes(b"ASCII positive command control")
        result = self.inspect([b"ascii/libSDL3.0.dylib"])
        self.assertEqual(result["status"], "VERIFIED")
        observed = json.loads(self.capture.read_text(encoding="utf-8"))
        self.assertEqual(observed["environment"].get("LC_ALL"), "C")
        self.assertEqual(observed["argv"][:5], ["-a", "-p", "1234", "-d", "txt"])
        self.assertEqual(observed["environment"]["PATH"], "/usr/bin:/bin:/usr/sbin")

    def test_foreign_and_mixed_same_name_images_remain_rejected(self):
        foreign = self.root / self.library.name
        foreign.write_bytes(self.library.read_bytes())
        for paths in ([b"libSDL3.0.dylib"],
                      [self.unicode_directory + b"/libSDL3.0.dylib", b"libSDL3.0.dylib"]):
            with self.subTest(paths=paths), self.assertRaisesRegex(runtime.RuntimeContractError, "second source"):
                self.inspect(paths)

    def test_unknown_or_malformed_path_escape_does_not_hide_beside_valid_sdl(self):
        self.package = self.root / "ascii"
        self.package.mkdir()
        self.library = self.package / "libSDL3.0.dylib"
        self.library.write_bytes(b"required image beside invalid observer field")
        for malformed in (br"bad\q/cache", br"bad\xQ1/cache", br"bad\x1/cache", b"trailing\\"):
            with self.subTest(malformed=malformed), self.assertRaises((ValueError, runtime.RuntimeContractError)):
                self.inspect([b"ascii/libSDL3.0.dylib", malformed])

    def test_failed_observer_still_rejects_valid_fields(self):
        with self.assertRaisesRegex(runtime.RuntimeContractError, "observation failed"):
            self.inspect([self.unicode_directory + b"/libSDL3.0.dylib"], exit_code=1)
        observed = json.loads(self.capture.read_text(encoding="utf-8"))
        self.assertEqual(observed["argv"][:5], ["-a", "-p", "1234", "-d", "txt"])

    def test_failed_observation_retains_original_command_bytes_paths_and_error(self):
        foreign = self.root / self.library.name
        foreign.write_bytes(b"foreign library fixture")
        cases = [([b"libSDL3.0.dylib"], 0, "second source"),
                 ([self.unicode_directory + b"/libSDL3.0.dylib"], 1, "observation failed"),
                 ([br"bad\q/cache"], 0, "escape")]
        for paths, code, error in cases:
            with self.subTest(paths=paths, code=code):
                with self.assertRaises(runtime._ObservationError) as raised:
                    self.inspect(paths, exit_code=code, loaded=True)
                observed = raised.exception.observations["loaded_modules"]
                self.assertEqual(observed["status"], "NOT_VERIFIED")
                self.assertIn(error, observed["error"])
                observer = observed["observer"]
                raw = (self.root / "lines.bin").read_bytes()
                self.assertEqual(base64.b64decode(observer["stdout"]["base64"], validate=True), raw)
                self.assertEqual(observer["stdout"]["sha256"], hashlib.sha256(raw).hexdigest())
                self.assertEqual(observer["returncode"], code)
                self.assertEqual(observer["tool"]["sha256"], hashlib.sha256(self.tool.read_bytes()).hexdigest())
                self.assertEqual(observer["environment"]["LC_ALL"], "C")
                stderr = b"controlled lsof failure" if code else b""
                self.assertEqual(base64.b64decode(observer["stderr"]["base64"], validate=True), stderr)
                self.assertIn("reported_paths", observed)
                if error == "second source":
                    self.assertIn(str(foreign), observed["paths"])

    def test_ambiguous_caret_control_spelling_is_rejected(self):
        self.package = self.root / "ascii"
        self.package.mkdir()
        self.library = self.package / "libSDL3.0.dylib"
        self.library.write_bytes(b"required image beside ambiguous observer field")
        # Real lsof emits the same ^A for byte 0x01 and literal caret+A.
        # Guessing either path would not prove the loaded image's source.
        with self.assertRaises((ValueError, runtime.RuntimeContractError)):
            self.inspect([b"ascii/libSDL3.0.dylib", b"unrelated-^A/cache"])

    if os.name != "nt":
        def test_literal_backslash_x_path_is_not_decoded_as_unicode(self):
            literal = self.root / r"\xe4\xbd\x9c\xe5\x93\x81 \xe8\xbe\x93\xe5\x87\xba"
            literal.mkdir()
            (literal / self.library.name).write_bytes(b"different literal-backslash image")
            escaped_literal = self.unicode_directory.replace(b"\\", b"\\\\")
            # The foreign literal path must not be mistaken for the Unicode
            # package, even though both have the same required basename.
            with self.assertRaisesRegex(runtime.RuntimeContractError, "second source"):
                self.inspect([escaped_literal + b"/libSDL3.0.dylib"])
            self.package, self.library = literal, literal / self.library.name
            result = self.inspect([escaped_literal + b"/libSDL3.0.dylib"])
            self.assertEqual(result["status"], "VERIFIED")
            self.assertEqual(result["required"][0]["sha256"], hashlib.sha256(self.library.read_bytes()).hexdigest())


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

    def invoke(self, *, mode="", attempt=None, launch=None, editor_port=None, audio_output='device'):
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
                                              editor_port=editor_port, audio_output=audio_output,
                                              **({"launch_relative_path":launch} if launch is not None else {}))

    def test_explicit_software_selection_binds_argv_and_completed_pcm_observation(self):
        report = self.invoke(audio_output='software')
        self.assertEqual(report['status'], 'RUNTIME_PASS', report)
        self.assertEqual(report['physical_audio_output'], 'NOT_RUN')
        for stage in report['stages']:
            if stage['name'] == 'author_create_build': continue
            argv = stage['commands'][0]['argv']
            self.assertEqual(argv[argv.index('--audio-output')+1], 'software')
            if stage['name'].endswith('_frames'):
                self.assertEqual(stage['audio']['statistics']['frames'], 480)

    def test_software_missing_statistics_or_silent_demo_cannot_pass(self):
        for mode in ('audio-empty', 'audio-missing'):
            with self.subTest(mode=mode):
                report = self.invoke(audio_output='software', mode=mode, attempt=self.root/mode)
                self.assertEqual(report['status'], 'RUNTIME_FAIL')
                self.assertEqual(report['stages'][-1]['name'], 'engine_frames')
                self.assertEqual(report['cleanup'], 'COMPLETE')

    def test_unknown_audio_selection_fails_before_commands(self):
        report = self.invoke(audio_output='automatic')
        self.assertEqual(report['status'], 'RUNTIME_FAIL')
        self.assertFalse(report['stages'])

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

    def test_literal_loopback_fixture_does_not_enter_blocked_dns_resolver(self):
        self.engine_script.write_text(
            'import socket, threading\n'
            'def blocked_resolver(*args):\n'
            '    print("blocked resolver entered", flush=True)\n'
            '    threading.Event().wait(30)\n'
            'socket.getfqdn = blocked_resolver\n' + ENGINE_FIXTURE, encoding='utf-8')
        report = self.invoke()
        self.assertEqual(report['status'], 'RUNTIME_PASS', report)

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
        try:
            report = runtime.observe_loaded_modules(identity)
        except Exception as error:
            _preserve_actual_observer_failure(self.id(), error)
            raise
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
        with LoopbackHTTPServer(('127.0.0.1', 0), Handler) as server:
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
        with LoopbackHTTPServer(('127.0.0.1', 0), Handler) as target, \
             LoopbackHTTPServer(('127.0.0.1', 0), Handler) as origin:
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


class ActualObserverDiagnosticTests(unittest.TestCase):
    """Exercise the real self-observation test's failure boundary, without rerunning lsof."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="caesura-observer-diagnostic-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.environment = patch.dict(os.environ, {
            "RUNNER_TEMP": str(self.root), "CAESURA_DIAGNOSTIC_SECRET": "do-not-copy-env-7259"})
        self.environment.start()
        self.addCleanup(self.environment.stop)
        self.error = runtime.RuntimeContractError("Unknown lsof image field")
        self.error.unrelated_secret = "do-not-copy-exception-9361"
        self.stdout = b"p1234\n?synthetic-rejected-field\nn/fixture/\\xe4\\xbd\\x9c\\xe5\\x93\\x81\n"
        self.stderr = b"original observer diagnostic\xff\n"
        self.observation = {"status": "NOT_VERIFIED", "process": {"pid": 1234},
            "reported_paths": ["/fixture/\\xe4\\xbd\\x9c\\xe5\\x93\\x81"],
            "observer": {"tool": {"path": "/fixture/lsof", "sha256": "a" * 64},
                         "argv": ["/fixture/lsof", "-a", "-p", "1234", "-d", "txt", "-Fn"],
                         "environment": {"PATH": "/usr/bin:/bin:/usr/sbin", "LC_ALL": "C"},
                         "returncode": 0, "stdout": runtime._observer_bytes(self.stdout),
                         "stderr": runtime._observer_bytes(self.stderr)}}
        self.error.module_observation = self.observation

    def exercise_failure(self):
        case = NativePackageRuntimeTests("test_actual_current_process_module_paths_are_observed")
        output = io.StringIO()
        with patch.object(runtime, "observe_loaded_modules", side_effect=self.error) as observer, redirect_stderr(output):
            with self.assertRaises(runtime.RuntimeContractError) as caught:
                case.test_actual_current_process_module_paths_are_observed()
        self.assertIs(caught.exception, self.error)
        observer.assert_called_once()
        return output.getvalue()

    def reports(self, expected=1):
        paths = list((self.root / "caesura-native-observer").glob("*/observation.json"))
        self.assertEqual(len(paths), expected)
        return paths

    def test_same_observation_raw_bytes_and_original_exception_are_preserved(self):
        original = json.dumps(self.observation, sort_keys=True)
        output = self.exercise_failure()
        path, = self.reports()
        raw = path.read_bytes()
        report = json.loads(raw)
        self.assertEqual(report["module_observation"], self.observation)
        self.assertEqual(report["error"], {"type": "RuntimeContractError", "message": str(self.error)})
        self.assertTrue(report["test_id"].endswith("NativePackageRuntimeTests.test_actual_current_process_module_paths_are_observed"))
        for name, expected in (("stdout", self.stdout), ("stderr", self.stderr)):
            value = report["module_observation"]["observer"][name]
            self.assertEqual(base64.b64decode(value["base64"], validate=True), expected)
            self.assertEqual(value["sha256"], hashlib.sha256(expected).hexdigest())
            self.assertEqual(value["size"], len(expected))
        self.assertEqual(json.dumps(self.observation, sort_keys=True), original)
        self.assertIn(hashlib.sha256(raw).hexdigest(), output)
        for secret in (b"do-not-copy-env-7259", b"do-not-copy-exception-9361"):
            self.assertNotIn(secret, raw + output.encode("utf-8"))

    def test_repeated_failures_use_exclusive_directories_without_overwriting(self):
        self.exercise_failure()
        first, = self.reports()
        before = first.read_bytes()
        self.exercise_failure()
        self.reports(expected=2)
        self.assertEqual(first.read_bytes(), before)

    def test_missing_runner_temp_uses_local_temporary_root(self):
        with patch.dict(os.environ):
            os.environ.pop("RUNNER_TEMP", None)
            with patch.object(tempfile, "gettempdir", return_value=str(self.root)):
                self.exercise_failure()
        self.reports()

    def test_capture_directory_failure_does_not_replace_original_exception(self):
        blocked = self.root / "caesura-native-observer"
        blocked.write_bytes(b"preexisting owned negative-control file")
        self.exercise_failure()
        self.assertEqual(blocked.read_bytes(), b"preexisting owned negative-control file")

    def test_missing_attached_observation_still_preserves_original_error(self):
        del self.error.module_observation
        self.exercise_failure()
        path, = self.reports()
        report = json.loads(path.read_bytes())
        self.assertIsNone(report["module_observation"])
        self.assertEqual(report["error"]["message"], str(self.error))

    def test_success_keeps_original_assertions_and_does_not_create_diagnostics(self):
        case = NativePackageRuntimeTests("test_actual_current_process_module_paths_are_observed")
        with patch.object(runtime, "observe_loaded_modules", return_value={
                "status": "OBSERVED", "paths": [str(Path(FIXTURE_PYTHON).resolve())]}) as observer:
            case.test_actual_current_process_module_paths_are_observed()
        observer.assert_called_once()
        self.assertEqual(list(self.root.iterdir()), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
