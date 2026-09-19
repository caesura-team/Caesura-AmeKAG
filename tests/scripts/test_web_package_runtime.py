"""Real HTTP/owned-child controller tests. No Chrome or game acceptance proof."""
from __future__ import annotations

import http.client
from http.server import BaseHTTPRequestHandler
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock
from urllib.error import HTTPError, URLError
from urllib.request import build_opener, ProxyHandler

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from package_runtime import ProcessIdentity, RuntimeContractError, process_identity, loopback_listeners
from package_verification import inspect_inventory
from web_package_runtime import (run_web_package, start_package_server, isolated_web_env,
                                 _OwnedCommand, _server_ready, _chrome_ready)
import web_package_runtime as runtime


class WebPackageRuntimeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='caesura-web-runtime-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.package = self.root / 'final package'
        self.package.mkdir()
        (self.package / 'index.html').write_text('<!doctype html><title>HTTP fixture only</title>', encoding='utf-8')
        (self.package / 'assets').mkdir()
        (self.package / 'assets' / '中文 文件.txt').write_text('exact Unicode bytes', encoding='utf-8')
        (self.package / 'glue.wasm').write_bytes(b'\x00asm\x01\x00\x00\x00')
        (self.package / 'empty-dir').mkdir()
        for name in ('home', 'temp', 'work'):
            (self.root / name).mkdir()
        self.env = isolated_web_env(self.root / 'home', self.root / 'temp', self.root / 'work')
        self.opener = build_opener(ProxyHandler({}))

    def server(self, prefix='/', port=0, name='scenario'):
        scenario = self.root / name
        scenario.mkdir()
        service = start_package_server(self.package, scenario, self.env, prefix=prefix, port=port)
        self.addCleanup(service.stop)
        return service

    def get(self, service, path):
        with self.opener.open(f'http://127.0.0.1:{service.port}{path}', timeout=4) as response:
            return response.status, response.headers, response.read()

    def test_root_serves_exact_package_bytes_and_wasm_mime(self):
        service = self.server()
        self.assertEqual(self.get(service, '/')[2], (self.package / 'index.html').read_bytes())
        _, headers, body = self.get(service, '/glue.wasm')
        self.assertEqual(body, (self.package / 'glue.wasm').read_bytes())
        self.assertEqual(headers['Content-Type'], 'application/wasm')

    def test_literal_loopback_readiness_does_not_enter_blocked_dns_resolver(self):
        entered, release = self.root / 'dns-entered', self.root / 'dns-release'
        ready = self.root / 'dns-ready.json'
        child = self.root / 'blocked-dns.py'
        child.write_text(
            'import socket, sys, time\nfrom pathlib import Path\n'
            f'sys.path.insert(0, {str(ROOT / "scripts")!r})\n'
            'import web_package_runtime as runtime\n'
            'def blocked_resolver(*args):\n'
            f'    Path({str(entered)!r}).write_text(repr(args))\n'
            f'    while not Path({str(release)!r}).exists(): time.sleep(0.01)\n'
            '    return "resolver-released.invalid"\n'
            'socket.getfqdn = blocked_resolver\n'
            f'runtime.serve_package(Path({str(self.package)!r}), Path({str(ready)!r}))\n',
            encoding='utf-8')
        command = _OwnedCommand([sys.executable, '-I', '-S', str(child)], self.root,
                                self.env, self.root / 'dns-command', timeout=20)
        self.addCleanup(command.stop)
        identity = command.wait_identity()
        command.wait_for(lambda: True if ready.exists() or entered.exists() else None,
                         timeout=8, description='ready or resolver barrier')
        consulted_dns = entered.exists()
        if consulted_dns:
            self.assertFalse(ready.exists(), 'the blocked resolver precedes listen/readiness')
            self.assertEqual(process_identity(identity.pid), identity)
            release.write_text('release the actual resolver boundary', encoding='utf-8')
        record = command.wait_for(lambda: _server_ready(command, ready, self.package, '/'),
                                  timeout=5, description='owned readiness after resolver barrier')
        with self.opener.open(f'http://127.0.0.1:{record["port"]}/', timeout=4) as response:
            self.assertEqual(response.read(), (self.package / 'index.html').read_bytes())
        self.assertFalse(consulted_dns, 'literal loopback readiness must not depend on DNS')

    def test_startup_timeout_retains_child_stack_in_failure(self):
        wrapper = self.root / 'blocked-bind.py'
        wrapper.write_text(
            'import sys, threading\n'
            f'sys.path.insert(0, {str(ROOT / "scripts")!r})\n'
            'import web_package_runtime as runtime\n'
            'def blocked_bind(port):\n'
            '    print("startup reached blocked_bind", file=sys.stderr, flush=True)\n'
            '    threading.Event().wait(60)\n'
            'runtime._bind_http = blocked_bind\n'
            'runtime.main()\n', encoding='utf-8')
        def owned_child(argv, *args, **kwargs):
            argv = list(argv)
            argv[3] = str(wrapper)
            return _OwnedCommand(argv, *args, **kwargs)
        scenario = self.root / 'blocked-startup'
        scenario.mkdir()
        with mock.patch.object(runtime, '_OwnedCommand', side_effect=owned_child):
            with self.assertRaises(RuntimeContractError) as failed:
                start_package_server(self.package, scenario, self.env)
        self.assertIn('owned HTTP readiness', str(failed.exception))
        self.assertIn('startup reached blocked_bind', str(failed.exception))
        self.assertIn('blocked_bind', (scenario / 'server/stderr.log').read_text())
        self.assertIn('Timeout (', (scenario / 'server/stderr.log').read_text())
        cleanup = json.loads((scenario / 'server/cleanup.json').read_text())
        self.assertEqual(cleanup['status'], 'CLEANUP_PASS')
        self.assertTrue(cleanup['process_exited'])

    def test_utf8_lua_response_declares_charset_without_changing_wire_bytes(self):
        content = 'return {message="语言 • café"}\n'.encode('utf-8')
        (self.package / 'language.lua').write_bytes(content)
        service = self.server()
        _, headers, body = self.get(service, '/language.lua')
        self.assertEqual(body, content)
        self.assertEqual(headers.get_content_charset(), 'utf-8')
        self.assertEqual(body.decode(headers.get_content_charset()).encode('utf-8'), content)

    def test_subpath_is_exact_and_unicode_query_does_not_change_file(self):
        service = self.server('/games/package/')
        self.assertEqual(self.get(service, '/games/package/')[0], 200)
        self.assertEqual(self.get(service, '/games/package/assets/%E4%B8%AD%E6%96%87%20%E6%96%87%E4%BB%B6.txt?v=1')[2],
                         b'exact Unicode bytes')
        for path in ('/', '/games/packageish/index.html', '/games/package'):
            with self.subTest(path=path), self.assertRaises(HTTPError) as error:
                self.get(service, path)
            self.assertEqual(error.exception.code, 404)

    def test_traversal_and_windows_path_forms_never_leave_package(self):
        (self.root / 'secret.txt').write_text('must never be served', encoding='utf-8')
        service = self.server()
        for path in ('/../secret.txt', '/%2e%2e/secret.txt', '/%2e%2e%2fsecret.txt',
                     '/%5c..%5csecret.txt', '/C:/secret.txt', '/index.html:stream', '/%00'):
            with self.subTest(path=path):
                connection = http.client.HTTPConnection('127.0.0.1', service.port, timeout=4)
                try:
                    connection.request('GET', path)
                    response = connection.getresponse()
                    body = response.read()
                    self.assertIn(response.status, (400, 403, 404))
                    self.assertNotIn(b'must never be served', body)
                finally:
                    connection.close()

    def test_outside_symlink_is_rejected_and_no_directory_listing(self):
        outside = self.root / 'outside.txt'
        outside.write_text('outside secret', encoding='utf-8')
        os.symlink(outside, self.package / 'escape.txt')
        service = self.server()
        for path in ('/escape.txt', '/empty-dir/'):
            with self.subTest(path=path), self.assertRaises(HTTPError) as error:
                self.get(service, path)
            self.assertIn(error.exception.code, (403, 404))

    def test_service_requests_and_logs_never_modify_package(self):
        before = inspect_inventory(self.package)
        service = self.server()
        self.get(service, '/')
        connection = http.client.HTTPConnection('127.0.0.1', service.port, timeout=4)
        try:
            connection.request('POST', '/created.txt', body=b'must not write')
            response = connection.getresponse()
            self.assertEqual(response.status, 405)
            response.read()
        finally:
            connection.close()
        service.stop()
        self.assertEqual(inspect_inventory(self.package), before)
        self.assertTrue((self.root / 'scenario' / 'server' / 'stdout.log').is_file())

    def test_observed_http_response_digests_bind_to_exact_package_files(self):
        service = self.server('/games/package/')
        body = self.get(service, '/games/package/glue.wasm')[2]
        resource = dict(url=service.url + 'glue.wasm', bytes=len(body), sha256=hashlib.sha256(body).hexdigest(), body_encoding='base64')
        result = runtime._verify_response_bytes({'resources': [resource]}, self.package, service.url)
        self.assertEqual(result['status'], 'RESPONSE_BODY_PASS')
        self.assertEqual(result['checked'], 1)
        for changed in (dict(resource, sha256='0' * 64), dict(resource, bytes=len(body) + 1),
                        dict(resource, url=service.url + '%2e%2e/secret'),
                        dict(resource, url='http://outside.invalid/glue.wasm')):
            with self.subTest(resource=changed), self.assertRaises(RuntimeContractError):
                runtime._verify_response_bytes({'resources': [changed]}, self.package, service.url)

    def test_cdp_decoded_utf8_bom_is_distinct_from_original_package_bytes(self):
        text = 'return "真实文本 • café"\n'.encode('utf-8')
        original = b'\xef\xbb\xbf' + text
        path = self.package / 'utf8.lua'
        path.write_bytes(original)
        base = 'http://127.0.0.1:45678/'
        resource = dict(url=base+'utf8.lua', bytes=len(text), sha256=hashlib.sha256(text).hexdigest(), body_encoding='utf8')
        result = runtime._verify_response_bytes({'resources':[resource]}, self.package, base)
        self.assertEqual(result['status'], 'RESPONSE_BODY_PASS')
        bound = result['resources'][0]
        self.assertEqual(bound['representation'], 'CDP_DECODED_UTF8')
        self.assertTrue(bound['utf8_bom_removed'])
        self.assertEqual(bound['package_sha256'], hashlib.sha256(original).hexdigest())
        self.assertEqual(bound['package_bytes'], len(original))
        self.assertEqual(path.read_bytes(), original)
        for changed in (dict(resource, body_encoding='base64'), dict(resource, body_encoding=None),
                        dict(resource, sha256='0'*64), dict(resource, bytes=len(original))):
            with self.subTest(changed=changed), self.assertRaises(RuntimeContractError):
                runtime._verify_response_bytes({'resources':[changed]}, self.package, base)
        path.write_bytes(b'\xffnot utf8')
        with self.assertRaises(RuntimeContractError):
            runtime._verify_response_bytes({'resources':[resource]}, self.package, base)

    if os.name == 'nt':
        def test_failed_exit_witness_identity_query_closes_its_actual_handle(self):
            from ctypes import wintypes
            import ctypes
            service = self.server()
            kernel = ctypes.WinDLL('kernel32', use_last_error=True)
            kernel.GetCurrentProcess.restype = wintypes.HANDLE
            kernel.GetProcessHandleCount.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
            def count():
                value = wintypes.DWORD()
                self.assertTrue(kernel.GetProcessHandleCount(kernel.GetCurrentProcess(), ctypes.byref(value)))
                return value.value
            before = count()
            with mock.patch.object(runtime, 'process_identity', side_effect=RuntimeContractError('identity query failed')):
                with self.assertRaises(RuntimeContractError):
                    runtime._ExitWitness(service.identity)
            self.assertEqual(count(), before, 'an identity-query failure must not leak the retained OS handle')

    def test_ready_file_is_bound_to_actual_root_pid_and_listener(self):
        service = self.server('/games/package/')
        ready = json.loads(service.ready_path.read_text(encoding='utf-8'))
        self.assertEqual(ready['pid'], service.identity.pid)
        self.assertEqual(ready['root'], str(self.package))
        self.assertEqual(ready['prefix'], '/games/package/')
        self.assertEqual(process_identity(service.identity.pid), service.identity)
        self.assertEqual({row['pid'] for row in loopback_listeners(service.port)}, {service.identity.pid})
        ready['pid'] = os.getpid()
        service.ready_path.write_text(json.dumps(ready), encoding='utf-8')
        with self.assertRaises(RuntimeContractError):
            _server_ready(service.command, service.ready_path, self.package, '/games/package/')

    def test_stop_waits_for_exact_process_exit_and_port_closure(self):
        service = self.server()
        identity, port = service.identity, service.port
        report = service.stop()
        self.assertEqual(report['status'], 'CLEANUP_PASS')
        self.assertTrue(report['process_exited'])
        self.assertTrue(report['port_closed'])
        self.assertFalse(loopback_listeners(port))
        with self.assertRaises(RuntimeContractError):
            process_identity(identity.pid)
        with self.assertRaises((URLError, OSError)):
            self.get(service, '/')
        self.assertEqual(service.stop(), report, 'cleanup is idempotent for this exact object')

    def test_occupied_port_fails_without_touching_existing_listener(self):
        first = self.server(name='first')
        second = self.root / 'second'
        second.mkdir()
        with self.assertRaises(RuntimeContractError):
            start_package_server(self.package, second, self.env, port=first.port)
        self.assertEqual(self.get(first, '/')[0], 200)
        self.assertEqual(process_identity(first.identity.pid), first.identity)

    def test_browser_blocked_explicit_port_fails_before_binding(self):
        with mock.patch.object(runtime, '_LoopbackHTTPServer',
                               return_value=mock.Mock(server_address=('127.0.0.1', 6665))) as bind:
            with self.assertRaisesRegex(RuntimeContractError, 'browser-blocked'):
                runtime.serve_package(self.package, self.root / 'ready.json', port=6665)
        bind.assert_not_called()
        self.assertFalse((self.root / 'ready.json').exists())

    def test_os_assigned_browser_blocked_port_is_retained_until_safe_bind_then_closed(self):
        blocked = mock.Mock(server_address=('127.0.0.1', 6665))
        safe = mock.Mock(server_address=('127.0.0.1', 43210))
        def bind(*args):
            if bind.calls == 0:
                bind.calls += 1
                return blocked
            blocked.server_close.assert_not_called()
            return safe
        bind.calls = 0
        with mock.patch.object(runtime, '_LoopbackHTTPServer', side_effect=bind):
            runtime.serve_package(self.package, self.root / 'ready.json')
        blocked.serve_forever.assert_not_called()
        blocked.server_close.assert_called_once()
        safe.serve_forever.assert_called_once()
        safe.server_close.assert_called_once()
        ready = json.loads((self.root / 'ready.json').read_text(encoding='utf-8'))
        self.assertEqual(ready['port'], 43210)
        self.assertEqual(ready['rejected_browser_ports'], [6665])

    def test_stop_request_write_failure_still_reaps_server_and_records_failure(self):
        def short_command(*args, **kwargs):
            return _OwnedCommand(*args, **dict(kwargs, timeout=3))
        with mock.patch.object(runtime, '_OwnedCommand', side_effect=short_command):
            service = self.server()
        original_open = Path.open
        stop_path = service.command.control / 'stop'
        def deny_stop(path, *args, **kwargs):
            if path == stop_path:
                raise PermissionError('test denied own stop request')
            return original_open(path, *args, **kwargs)
        with mock.patch.object(Path, 'open', deny_stop):
            report = service.stop()
        self.assertEqual(report['status'], 'CLEANUP_FAIL')
        self.assertTrue(report['errors'])
        self.assertTrue(report['process_exited'])
        self.assertTrue(report['port_closed'])
        self.assertTrue(service.command.future.done())
        self.assertIsNone(service.command.witness.handle)
        self.assertFalse(loopback_listeners(service.port))
        persisted = json.loads((service.command.directory / 'cleanup.json').read_text(encoding='utf-8'))
        self.assertEqual(persisted, report)

    if os.name == 'nt':
        def test_unreadable_timeout_receipt_still_closes_witness_and_records_failure(self):
            from ctypes import wintypes
            import ctypes
            def short_command(*args, **kwargs):
                return _OwnedCommand(*args, **dict(kwargs, timeout=3))
            with mock.patch.object(runtime, '_OwnedCommand', side_effect=short_command):
                service = self.server()
            with self.assertRaises(subprocess.TimeoutExpired):
                service.command.future.result(timeout=25)
            receipt = service.command.control / 'run.json'
            self.assertTrue(receipt.is_file())
            kernel = ctypes.WinDLL('kernel32', use_last_error=True)
            kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                          ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
            kernel.CreateFileW.restype = wintypes.HANDLE
            kernel.CloseHandle.argtypes = [wintypes.HANDLE]
            # The completed runner's actual receipt is unreadable while this
            # exact handle denies sharing. No cleanup or OS process API is mocked.
            lock = kernel.CreateFileW(str(receipt), 0x80000000, 0, None, 3, 0x80, None)
            self.assertNotIn(lock, (None, ctypes.c_void_p(-1).value), ctypes.get_last_error())
            try:
                report = service.stop()
                self.assertEqual(report['status'], 'CLEANUP_FAIL')
                self.assertTrue(any('TimeoutExpired' in error for error in report['errors']))
                self.assertTrue(any('PermissionError' in error for error in report['errors']))
                self.assertTrue(report['port_closed'])
                self.assertTrue(service.command.future.done())
                self.assertIsNone(service.command.witness.handle)
                self.assertFalse(loopback_listeners(service.port))
                persisted = json.loads((service.command.directory / 'cleanup.json').read_text(encoding='utf-8'))
                self.assertEqual(persisted, report)
            finally:
                kernel.CloseHandle(lock)
                # A failing regression must not leak its own retained witness.
                service.command.witness.close()

    def test_early_exiting_child_cannot_supply_readiness(self):
        command = _OwnedCommand([sys.executable, '-I', '-c', 'import sys; sys.exit(19)'],
                                self.root / 'work', self.env, self.root / 'early', timeout=6)
        self.addCleanup(command.stop)
        with self.assertRaises(RuntimeContractError):
            command.wait_for(lambda: None, timeout=3, description='nonexistent readiness')
        self.assertEqual(command.stop()['run']['actual_exit_code'], 19)

    def test_stale_devtools_port_cannot_connect_to_unrelated_service(self):
        service = self.server()
        other = _OwnedCommand([sys.executable, '-I', '-c', 'import time; time.sleep(20)'],
                             self.root / 'work', self.env, self.root / 'other', timeout=25)
        self.addCleanup(other.stop)
        other.wait_identity()
        profile = self.root / 'profile'
        profile.mkdir()
        (profile / 'DevToolsActivePort').write_text(f'{service.port}\n/devtools/browser/not-ours\n', encoding='utf-8')
        with self.assertRaises(RuntimeContractError):
            _chrome_ready(other, profile)
        self.assertEqual(self.get(service, '/')[0], 200)

    def test_explicit_debugger_port_requires_owned_listener_before_http_discovery(self):
        # Actual HTTP and OS listener ownership; endpoint metadata is a fixture.
        requests = []
        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                requests.append(self.path)
                body = json.dumps({'Browser':'Chrome/protocol-fixture',
                    'webSocketDebuggerUrl':f'ws://127.0.0.1:{self.server.server_port}/devtools/browser/fixture-nonce'}).encode()
                self.send_response(200)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            def log_message(self, *args):
                pass
        server = runtime._LoopbackHTTPServer(('127.0.0.1', 0), Handler)
        worker = threading.Thread(target=server.serve_forever, kwargs={'poll_interval':0.01})
        worker.start()
        try:
            current = process_identity(os.getpid())
            command = mock.Mock()
            command.wait_identity.return_value = current
            profile = self.root / 'unused-private-profile'
            observed = _chrome_ready(command, profile, expected_port=server.server_port)
            self.assertEqual(observed['port'], server.server_port)
            self.assertEqual(observed['identity']['pid'], current.pid)
            self.assertIsNone(observed['devtools_file_sha256'])
            self.assertEqual(requests, ['/json/version'])
            command.wait_identity.return_value = ProcessIdentity(current.pid, 'wrong-creation', current.executable, current.source)
            with self.assertRaises(RuntimeContractError):
                _chrome_ready(command, profile, expected_port=server.server_port)
            self.assertEqual(requests, ['/json/version'], 'wrong owner must be rejected before HTTP')
        finally:
            server.shutdown()
            worker.join(timeout=2)
            server.server_close()

    def test_environment_omits_proxy_language_and_application_pollution(self):
        polluted = dict(os.environ, PATH='developer-tools', HTTP_PROXY='http://proxy.invalid',
                        HTTPS_PROXY='http://proxy.invalid', ALL_PROXY='http://proxy.invalid',
                        NODE_OPTIONS='--require bad.js', LUA_PATH='hostlua', CAESURA_ROOT='repository',
                        PYTHONPATH='host-python', DISPLAY=':18', RUNTIME_SECRET='secret')
        before = dict(os.environ)
        env = isolated_web_env(self.root / 'home', self.root / 'temp', self.root / 'work', inherited=polluted)
        self.assertEqual(os.environ, before)
        self.assertEqual(env['DISPLAY'], ':18')
        self.assertEqual(env['HOME'], str(self.root / 'home'))
        self.assertNotIn('developer-tools', env['PATH'])
        for name in ('HTTP_PROXY', 'HTTPS_PROXY', 'ALL_PROXY', 'NODE_OPTIONS', 'LUA_PATH',
                     'CAESURA_ROOT', 'PYTHONPATH', 'RUNTIME_SECRET'):
            self.assertNotIn(name, env)

    def test_ui_actions_require_prelocked_external_plain_input(self):
        action = self.root / 'actions.json'
        action.write_text('{"schema":1,"steps":[{"click":"#advance"}]}', encoding='utf-8')
        digest = hashlib.sha256(action.read_bytes()).hexdigest()
        attempt = self.root / 'new-runtime'
        self.assertEqual(runtime._lock_actions(action, digest, self.package, attempt),
                         {'path':str(action), 'sha256':digest})
        for path, expected in ((action, None), (None, digest), (action, '0' * 64),
                               (Path('actions.json'), digest)):
            with self.subTest(path=path, digest=expected), self.assertRaises(RuntimeContractError):
                runtime._lock_actions(path, expected, self.package, attempt)
        inside = self.package / 'actions.json'
        inside.write_bytes(action.read_bytes())
        with self.assertRaises(RuntimeContractError):
            runtime._lock_actions(inside, digest, self.package, attempt)

    def test_changed_ui_action_input_invalidates_even_completed_scenarios(self):
        action = self.root / 'actions.json'
        action.write_text('{"schema":1,"steps":[{"click":"#advance"}]}', encoding='utf-8')
        digest = hashlib.sha256(action.read_bytes()).hexdigest()
        def scenario(package, attempt, name, prefix, tools, mode, actions):
            self.assertEqual(actions, {'path':str(action), 'sha256':digest})
            action.write_text('{}', encoding='utf-8')
            return {'name':name, 'status':'SCENARIO_PASS'}
        with mock.patch.object(runtime, '_scenario', side_effect=scenario):
            result = run_web_package(self.package, self.root / 'action-mutation',
                node_executable=sys.executable, browser_executable=sys.executable,
                lua_executable=sys.executable, actions_path=action, actions_sha256=digest)
        self.assertEqual(result['status'], 'RUNTIME_FAIL')
        self.assertTrue(any('UI action input changed' in item for item in result['errors']))

    def test_real_wrong_browser_exits_and_controller_reclaims_server(self):
        # Explicit Python in the browser slot is an actual early-exit negative
        # control; it does not simulate a successful Chrome/CDP execution.
        before = inspect_inventory(self.package)
        report = run_web_package(self.package, self.root / 'runtime-attempt',
                                 node_executable=sys.executable, browser_executable=sys.executable,
                                 lua_executable=sys.executable)
        self.assertEqual(report['status'], 'RUNTIME_FAIL')
        self.assertEqual(report['scenarios'][0]['status'], 'SCENARIO_FAIL')
        server_cleanup = report['scenarios'][0]['cleanup']['server']
        self.assertEqual(server_cleanup['status'], 'CLEANUP_PASS')
        self.assertTrue(server_cleanup['port_closed'])
        self.assertFalse(loopback_listeners(report['scenarios'][0]['server']['port']))
        self.assertEqual(inspect_inventory(self.package), before)
        self.assertEqual(report['tools']['lua']['usage'], 'declared_for_static_stage_not_executed_by_runtime')
        self.assertTrue((self.root / 'runtime-attempt' / 'report.json').is_file())

    def test_existing_attempt_or_repository_attempt_is_refused_before_launch(self):
        kwargs = dict(node_executable=sys.executable, browser_executable=sys.executable,
                      lua_executable=sys.executable)
        existing = self.root / 'existing'
        existing.mkdir()
        (existing / 'notes').write_text('preserve', encoding='utf-8')
        for attempt in (existing, ROOT / 'artifacts' / 'must-not-create-web-runtime-test'):
            with self.subTest(attempt=attempt), self.assertRaises(RuntimeContractError):
                run_web_package(self.package, attempt, **kwargs)
        self.assertEqual((existing / 'notes').read_text(), 'preserve')
        self.assertFalse((ROOT / 'artifacts' / 'must-not-create-web-runtime-test').exists())


if __name__ == '__main__':
    unittest.main()
