#!/usr/bin/env python3
"""Own the HTTP/browser/probe lifecycle for one explicitly inspected Web package.

The caller first accepts verify_web_package.STATIC_PASS. This controller never
fills missing files from a checkout, discovers a browser, reuses a profile or
connects to a previously running browser. Environment filtering is not an OS
filesystem sandbox. Real browser acceptance requires both real probe phases;
the protocol fixtures and HTTP tests do not provide that evidence.
"""
from __future__ import annotations

import argparse
from concurrent.futures import Future, TimeoutError as FutureTimeout
import ctypes
from dataclasses import asdict
from datetime import datetime, timezone
import faulthandler
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import mimetypes
import os
from pathlib import Path
import re
import shutil
import socket
from socketserver import TCPServer
import stat
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import unquote_to_bytes, urlsplit
from urllib.request import build_opener, ProxyHandler

SCRIPT_ROOT = Path(__file__).resolve().parent
SOURCE_ROOT = SCRIPT_ROOT.parent
# The trusted serve mode also works under Python -I -S. Its modules are bound to
# this explicit validator source, never a package or inherited Python path.
if str(SCRIPT_ROOT) not in sys.path:
    sys.path.insert(0, str(SCRIPT_ROOT))
from package_runtime import ProcessIdentity, RuntimeContractError, process_identity, loopback_listeners, verify_owned_listener, run_runtime_command
from package_verification import inspect_inventory

SCHEMA = 'caesura.web-package-runtime.v1'
PROBE = SCRIPT_ROOT / 'web_package_probe.mjs'
PREFIXES = (('root', '/'), ('subpath', '/games/package/'))
# WHATWG Fetch 2.9, checked 2026-09-13: https://fetch.spec.whatwg.org/#port-blocking
# An OS ephemeral port can be in this set on a host with a custom port range.
# Reject it before advertising a page URL; do not override browser policy.
BROWSER_BLOCKED_PORTS = frozenset((
    0, 1, 7, 9, 11, 13, 15, 17, 19, 20, 21, 22, 23, 25, 37, 42, 43, 53, 69,
    77, 79, 87, 95, 101, 102, 103, 104, 109, 110, 111, 113, 115, 117, 119,
    123, 135, 137, 139, 143, 161, 179, 389, 427, 465, 512, 513, 514, 515,
    526, 530, 531, 532, 540, 548, 554, 556, 563, 587, 601, 636, 989, 990,
    993, 995, 1719, 1720, 1723, 2049, 3659, 4045, 4190, 5060, 5061, 6000,
    6566, 6665, 6666, 6667, 6668, 6669, 6679, 6697, 10080))


def _now():
    return datetime.now(timezone.utc).isoformat()


def _plain_file(path: Path) -> bool:
    info = path.lstat()
    return stat.S_ISREG(info.st_mode) and not (getattr(info, 'st_file_attributes', 0) & 0x400)


def _digest(path: Path) -> str:
    before = path.stat()
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    after = path.stat()
    if (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) != (
            after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns):
        raise RuntimeContractError(f'Tool/log changed while hashing: {path}')
    return digest


def _json_new(path: Path, value: dict):
    temporary = path.with_name('.' + path.name + '.writing')
    with temporary.open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2)
        stream.write('\n')
    if path.exists() or path.is_symlink():
        raise RuntimeContractError(f'Refusing existing report/control file: {path}')
    os.replace(temporary, path)


def _read_json(path: Path) -> dict:
    if not _plain_file(path) or path.stat().st_size > 16 * 1024 * 1024:
        raise RuntimeContractError(f'Invalid JSON control file: {path}')
    value = json.loads(path.read_text(encoding='utf-8'))
    if not isinstance(value, dict):
        raise RuntimeContractError(f'Expected an object in {path}')
    return value


def isolated_web_env(home: Path, temp: Path, work: Path, *, inherited=None) -> dict[str, str]:
    """Preserve explicit OS/display inputs only; all runtime tools use absolute argv."""
    home, temp, work = (Path(path).resolve(strict=True) for path in (home, temp, work))
    if not all(path.is_dir() for path in (home, temp, work)):
        raise RuntimeContractError('Private home/temp/work directories must exist')
    inherited = os.environ if inherited is None else inherited
    names = {'DISPLAY', 'WAYLAND_DISPLAY', 'XAUTHORITY', 'XDG_RUNTIME_DIR',
             'DBUS_SESSION_BUS_ADDRESS', 'LANG', 'LANGUAGE', 'LC_ALL', 'LC_CTYPE', 'TZ'}
    env = {key: value for key, value in inherited.items() if key in names}
    if os.name == 'nt':
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.GetWindowsDirectoryW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint]
        kernel.GetWindowsDirectoryW.restype = ctypes.c_uint
        buffer = ctypes.create_unicode_buffer(32768)
        length = kernel.GetWindowsDirectoryW(buffer, len(buffer))
        if not 0 < length < len(buffer):
            raise RuntimeContractError('Cannot determine the OS Windows directory')
        windows = Path(buffer.value)
        env.update(PATH=str(windows / 'System32'), SystemRoot=str(windows), WINDIR=str(windows),
                   ComSpec=str(windows / 'System32/cmd.exe'), USERPROFILE=str(home),
                   ProgramData=str(home), ALLUSERSPROFILE=str(home),
                   APPDATA=str(home / 'AppData/Roaming'), LOCALAPPDATA=str(home / 'AppData/Local'))
    else:
        env['PATH'] = '/usr/bin:/bin'
    env.update(HOME=str(home), TMP=str(temp), TEMP=str(temp), TMPDIR=str(temp), PWD=str(work),
               XDG_CONFIG_HOME=str(home / '.config'), XDG_CACHE_HOME=str(home / '.cache'),
               XDG_DATA_HOME=str(home / '.local/share'), XDG_STATE_HOME=str(home / '.local/state'))
    return env


class _ExitWitness:
    """Retain an exact Windows process object instead of guessing from an open error."""
    def __init__(self, identity: ProcessIdentity):
        self.identity, self.handle, self.kernel = identity, None, None
        if os.name == 'nt':
            from ctypes import wintypes
            kernel = ctypes.WinDLL('kernel32', use_last_error=True)
            kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
            kernel.OpenProcess.restype = wintypes.HANDLE
            kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
            kernel.WaitForSingleObject.restype = wintypes.DWORD
            kernel.CloseHandle.argtypes = [wintypes.HANDLE]
            self.kernel = kernel
            self.handle = kernel.OpenProcess(0x00101000, False, identity.pid)
            if not self.handle:
                raise RuntimeContractError('Cannot retain the observed process exit witness')
            try:
                if process_identity(identity.pid) != identity:
                    raise RuntimeContractError('Process changed before its exit witness was retained')
            except BaseException:
                self.close()
                raise

    def exited(self):
        if self.handle is not None:
            state = self.kernel.WaitForSingleObject(self.handle, 0)
            if state not in (0, 258):
                raise RuntimeContractError('Cannot read process exit signal')
            return state == 0
        try:
            current = process_identity(self.identity.pid)
            return current != self.identity
        except RuntimeContractError:
            # Permission/query failure is not absence. For POSIX require ESRCH
            # after reaping, or a positively observed different creation identity.
            try:
                os.kill(self.identity.pid, 0)
            except ProcessLookupError:
                return True
            except PermissionError:
                return False
            return False

    def close(self):
        if self.handle is not None:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


class _OwnedCommand:
    def __init__(self, argv, cwd, env, directory: Path, *, timeout=180):
        self.directory = Path(directory)
        self.directory.mkdir()
        self.argv, self.cwd, self.env, self.timeout = list(argv), Path(cwd), dict(env), timeout
        self.control = self.directory / 'control'
        self.identity = None
        self.witness = None
        self.port = None
        self.cleanup = None
        self.future = Future()
        def run():
            try:
                with (self.directory / 'stdout.log').open('xb') as out, (self.directory / 'stderr.log').open('xb') as err:
                    result = run_runtime_command(self.argv, self.cwd, self.env, self.control, out, err,
                                                 self.timeout, stop_request=self.control / 'stop')
                self.future.set_result(result)
            except BaseException as error:
                self.future.set_exception(error)
        self.thread = threading.Thread(target=run, name='caesura-web-owned-command', daemon=False)
        self.thread.start()

    def wait_for(self, predicate, *, timeout, description):
        deadline = time.monotonic() + timeout
        while True:
            value = predicate()
            if value is not None:
                return value
            if self.future.done():
                try:
                    result = self.future.result()
                    message = f'exit {result.get("actual_exit_code")}'
                except BaseException as error:
                    message = f'{type(error).__name__}: {error}'
                raise RuntimeContractError(f'Process ended before {description}: {message}')
            if time.monotonic() >= deadline:
                raise RuntimeContractError(f'Timed out waiting for {description}')
            time.sleep(0.02)

    def wait_identity(self, timeout=15):
        if self.identity is not None:
            return self.identity
        def inspect():
            path = self.control / 'process.json'
            if not path.exists():
                return None
            identity = ProcessIdentity(**_read_json(path))
            if process_identity(identity.pid) != identity:
                raise RuntimeContractError('Published process identity has already changed')
            self.witness = _ExitWitness(identity)
            self.identity = identity
            return identity
        return self.wait_for(inspect, timeout=timeout, description='owned process identity')

    def wait_result(self):
        try:
            return self.future.result(timeout=self.timeout + 20)
        except FutureTimeout as error:
            raise RuntimeContractError('Owned runner failed to finish within its outer cleanup bound') from error

    def stop(self):
        if self.cleanup is not None:
            return self.cleanup
        errors = []
        if not self.future.done():
            deadline = time.monotonic() + 5
            while not self.control.is_dir() and not self.future.done() and time.monotonic() < deadline:
                time.sleep(0.02)
            if self.control.is_dir() and not self.future.done():
                stop = self.control / 'stop'
                if not stop.exists():
                    try:
                        with stop.open('x', encoding='utf-8') as stream:
                            stream.write('controller requested stop\n')
                    except FileExistsError:
                        pass
                    except OSError as error:
                        # A denied stop request must still join the bounded
                        # owned runner and retain its cleanup/exit evidence.
                        errors.append(f'Cannot publish stop request: {type(error).__name__}: {error}')
        result = None
        try:
            result = self.wait_result()
        except BaseException as error:
            errors.append(f'{type(error).__name__}: {error}')
            path = self.control / 'run.json'
            try:
                if path.is_file():
                    result = _read_json(path)
            except BaseException as receipt_error:
                # A timeout can coincide with an unreadable/corrupt receipt.
                # Keep both failures and still release our retained process
                # witness, observe the port and publish a failed cleanup result.
                errors.append(f'runner receipt: {type(receipt_error).__name__}: {receipt_error}')
        self.thread.join(timeout=0)
        exited = bool(self.future.done() and result and result.get('owned_tree_cleanup') == 'COMPLETE')
        if self.witness is not None:
            try:
                deadline = time.monotonic() + 5
                while not self.witness.exited() and time.monotonic() < deadline:
                    time.sleep(0.02)
                exited = exited and self.witness.exited()
            except BaseException as error:
                exited = False
                errors.append(f'exit witness: {type(error).__name__}: {error}')
            finally:
                self.witness.close()
        listeners = None
        try:
            listeners = loopback_listeners(self.port) if self.port is not None else []
        except BaseException as error:
            errors.append(f'port observation: {type(error).__name__}: {error}')
        complete = exited and (self.port is None or listeners == []) and not errors
        self.cleanup = dict(status='CLEANUP_PASS' if complete else 'CLEANUP_FAIL',
                            process_exited=exited, identity_observed=self.identity is not None,
                            identity=asdict(self.identity) if self.identity else None,
                            port=self.port, port_closed=listeners == [] if self.port is not None else None,
                            port_check='OBSERVED' if self.port is not None else 'NOT_APPLICABLE', listeners_after=listeners,
                            run=result, errors=errors)
        _json_new(self.directory / 'cleanup.json', self.cleanup)
        return self.cleanup


class _PackageHandler(BaseHTTPRequestHandler):
    server_version = 'CaesuraPackageProbe/1'
    sys_version = ''

    def log_message(self, format, *args):
        print(json.dumps({'request': format % args}, ensure_ascii=True), flush=True)

    def do_POST(self):
        self.send_error(405, 'Read-only package service')

    def do_PUT(self):
        self.send_error(405, 'Read-only package service')

    def do_DELETE(self):
        self.send_error(405, 'Read-only package service')

    def do_HEAD(self):
        self._send_file(head=True)

    def do_GET(self):
        self._send_file(head=False)

    def _send_file(self, *, head):
        try:
            raw = urlsplit(self.path)
            if raw.scheme or raw.netloc or re.search(r'%(?:2f|5c)', raw.path, flags=re.I):
                raise ValueError('nonlocal/encoded separator')
            path = unquote_to_bytes(raw.path).decode('utf-8', errors='strict')
            if '\\' in path or ':' in path or any(ord(c) < 32 or ord(c) == 127 for c in path):
                raise ValueError('unsafe path character')
            if not path.startswith(self.server.package_prefix):
                self.send_error(404)
                return
            local = path[len(self.server.package_prefix):]
            parts = local.split('/')
            if any(part in ('.', '..') for part in parts):
                raise ValueError('traversal')
            target = self.server.package_root.joinpath(*parts).resolve(strict=True)
            if not target.is_relative_to(self.server.package_root):
                self.send_error(403)
                return
            if target.is_dir():
                target = (target / 'index.html').resolve(strict=True)
            if not target.is_relative_to(self.server.package_root) or not target.is_file():
                self.send_error(404)
                return
            with target.open('rb') as stream:
                size = os.fstat(stream.fileno()).st_size
                mime = {'.wasm': 'application/wasm', '.webmanifest': 'application/manifest+json',
                        '.js': 'text/javascript', '.mjs': 'text/javascript', '.lua': 'text/plain'}.get(target.suffix.lower())
                mime = mime or mimetypes.guess_type(target.name)[0] or 'application/octet-stream'
                if mime.startswith('text/') or mime in ('application/json', 'application/manifest+json'):
                    mime += '; charset=utf-8'
                self.send_response(200)
                self.send_header('Content-Type', mime)
                self.send_header('Content-Length', str(size))
                self.send_header('Cache-Control', 'no-cache')
                self.send_header('X-Content-Type-Options', 'nosniff')
                self.end_headers()
                if not head:
                    shutil.copyfileobj(stream, self.wfile)
        except (ValueError, UnicodeError):
            self.send_error(400)
        except (FileNotFoundError, NotADirectoryError, PermissionError):
            self.send_error(404)
        except (BrokenPipeError, ConnectionResetError):
            return


class _LoopbackHTTPServer(ThreadingHTTPServer):
    def server_bind(self):
        # This service advertises only the explicit numeric loopback address.
        # HTTPServer's reverse-DNS server_name lookup adds no useful identity
        # and can block before listen()/the owned readiness record exists.
        TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]


def _bind_http(port):
    if port and port in BROWSER_BLOCKED_PORTS:
        raise RuntimeContractError(f'Explicit HTTP port {port} is browser-blocked')
    rejected = []
    try:
        # This only allocates an admissible listener. No browser or validation
        # stage has started, so rejected allocations are not runtime retries.
        for _ in range(32):
            server = _LoopbackHTTPServer(('127.0.0.1', port), _PackageHandler)
            if server.server_address[1] not in BROWSER_BLOCKED_PORTS:
                return server, [item.server_address[1] for item in rejected]
            rejected.append(server)
        raise RuntimeContractError('OS did not assign a browser-usable HTTP port in 32 allocations')
    finally:
        for server in rejected:
            server.server_close()


def serve_package(package_root: Path, ready_path: Path, *, prefix='/', port=0):
    root = Path(package_root).resolve(strict=True)
    ready = Path(ready_path)
    if not root.is_dir() or prefix not in ('/', '/games/package/'):
        raise RuntimeContractError('Explicit package directory and supported prefix are required')
    if not ready.is_absolute() or not ready.parent.is_dir() or ready.exists() or ready.is_symlink():
        raise RuntimeContractError('Ready file must be new below an existing private directory')
    ready = ready.parent.resolve(strict=True) / ready.name
    if ready.is_relative_to(root):
        raise RuntimeContractError('Ready file must stay outside the package')
    if isinstance(port, bool) or not isinstance(port, int) or not 0 <= port <= 65535:
        raise RuntimeContractError('Port must be an integer in 0..65535')
    server = None
    try:
        # A startup hang must leave an original child stack before the owner's
        # 15-second readiness deadline reaps the process. Cancel once ready so
        # normal serving does not emit a spurious timeout traceback.
        faulthandler.dump_traceback_later(5, file=sys.stderr)
        try:
            server, rejected_ports = _bind_http(port)
            server.package_root, server.package_prefix = root, prefix
            _json_new(ready, dict(schema='caesura.web-package-http.v1', pid=os.getpid(),
                                  root=str(root), prefix=prefix, host='127.0.0.1',
                                  port=server.server_address[1], read_only=True,
                                  rejected_browser_ports=rejected_ports))
        finally:
            faulthandler.cancel_dump_traceback_later()
        server.serve_forever(poll_interval=0.05)
    finally:
        if server is not None:
            server.server_close()


def _server_ready(command, ready_path, root, prefix):
    if not Path(ready_path).exists():
        return None
    record = _read_json(Path(ready_path))
    identity = command.wait_identity()
    port = record.get('port')
    if (record.get('schema') != 'caesura.web-package-http.v1' or record.get('pid') != identity.pid
            or record.get('root') != str(Path(root).resolve(strict=True)) or record.get('prefix') != prefix
            or record.get('host') != '127.0.0.1' or record.get('read_only') is not True
            or type(port) is not int or not 1 <= port <= 65535):
        raise RuntimeContractError('HTTP readiness does not match this package/process/prefix')
    verify_owned_listener(identity, port)
    command.port = port
    return record


class _Service:
    def __init__(self, command, ready_path, ready):
        self.command, self.ready_path = command, ready_path
        self.identity, self.port = command.identity, ready['port']
        self.url = f'http://127.0.0.1:{self.port}{ready["prefix"]}'
        self.ready = ready

    def stop(self):
        return self.command.stop()


def start_package_server(package_root, scenario_dir, env, *, prefix='/', port=0):
    directory = Path(scenario_dir)
    ready = directory / 'server-ready.json'
    argv = [sys.executable, '-I', '-S', str(Path(__file__).resolve()), '--serve',
            '--package-root', str(Path(package_root).resolve(strict=True)), '--ready', str(ready),
            '--prefix', prefix, '--port', str(port)]
    command = _OwnedCommand(argv, directory, env, directory / 'server', timeout=300)
    try:
        command.wait_identity()
        record = command.wait_for(lambda: _server_ready(command, ready, package_root, prefix),
                                  timeout=15, description='owned HTTP readiness')
        return _Service(command, ready, record)
    except BaseException as error:
        command.stop()
        if isinstance(error, Exception):
            # Keep the raw file and also carry its bounded tail into the error
            # report/CTest output, which survives temporary fixture cleanup.
            try:
                with (command.directory / 'stderr.log').open('rb') as stream:
                    stream.seek(0, os.SEEK_END)
                    stream.seek(max(0, stream.tell() - 8192))
                    stderr = stream.read().decode('utf-8', errors='replace')
            except OSError as read_error:
                stderr = f'Cannot read child stderr: {read_error}'
            raise RuntimeContractError(f'{error}\nOwned HTTP startup stderr (last 8192 bytes):\n'
                                       + (stderr or '<empty>')) from error
        raise


def _chrome_ready(command, profile, *, expected_port=None):
    identity = command.wait_identity()
    path = Path(profile) / 'DevToolsActivePort'
    expected_ws_path = file_digest = None
    if expected_port is None:
        if not path.exists():
            return None
        if not _plain_file(path) or path.stat().st_size > 4096:
            raise RuntimeContractError('Invalid private-profile DevToolsActivePort file')
        lines = path.read_text(encoding='utf-8').splitlines()
        if len(lines) < 2:
            return None
        if len(lines) != 2 or not lines[0].isdigit() or not re.fullmatch(r'/devtools/browser/[A-Za-z0-9_-]+', lines[1]):
            raise RuntimeContractError('Malformed private-profile DevToolsActivePort contents')
        port, expected_ws_path, file_digest = int(lines[0]), lines[1], _digest(path)
    else:
        port = expected_port
        if type(port) is not int or not 1 <= port <= 65535 or port in BROWSER_BLOCKED_PORTS:
            raise RuntimeContractError('Explicit debugger port must be browser-usable')
        if not loopback_listeners(port):
            return None
    verify_owned_listener(identity, port)
    command.port = port
    cdp = f'http://127.0.0.1:{port}'
    opener = build_opener(ProxyHandler({}))
    with opener.open(cdp + '/json/version', timeout=3) as response:
        raw = response.read(65537)
    if len(raw) > 65536:
        raise RuntimeContractError('Oversized browser version response')
    version = json.loads(raw)
    ws = urlsplit(version.get('webSocketDebuggerUrl', ''))
    if (ws.scheme != 'ws' or ws.hostname != '127.0.0.1' or ws.port != port
            or not re.fullmatch(r'/devtools/browser/[A-Za-z0-9_-]+', ws.path)
            or (expected_ws_path is not None and ws.path != expected_ws_path)
            or ws.query or ws.fragment or ws.username or ws.password
            or not str(version.get('Browser', '')).startswith(('Chrome/', 'HeadlessChrome/'))):
        raise RuntimeContractError('Browser endpoint differs from the new profile nonce')
    verify_owned_listener(identity, port)
    return dict(port=port, cdp_url=cdp, browser_ws_url=version['webSocketDebuggerUrl'],
                browser_version=version['Browser'], devtools_file_sha256=file_digest,
                identity=asdict(identity))


def _tool(path, usage):
    given = Path(path)
    if not given.is_absolute():
        raise RuntimeContractError('Every validation tool must have an explicit absolute executable path')
    resolved = given.resolve(strict=True)
    if not resolved.is_file() or resolved.stat().st_size == 0:
        raise RuntimeContractError(f'Missing/empty validation tool: {resolved}')
    return dict(path=str(resolved), sha256=_digest(resolved), usage=usage)


def _new_attempt(package, attempt):
    requested = Path(attempt).absolute()
    parent = requested.parent.resolve(strict=True)
    path = parent / requested.name
    repository_roots = [SOURCE_ROOT, *(ancestor for ancestor in SOURCE_ROOT.parents if (ancestor / '.git').exists())]
    if path.exists() or path.is_symlink() or path.is_relative_to(package) or any(path.is_relative_to(root) for root in repository_roots):
        raise RuntimeContractError('Runtime attempt must be new and outside the package and repositories')
    path.mkdir()
    return path


def _verify_response_bytes(report, package, base_url):
    """Bind binary bytes or CDP-decoded UTF-8 text to the mounted package.

    Network.getResponseBody returns a text representation for text resources;
    Chromium removes an initial UTF-8 BOM. Preserve the raw package identity
    separately, never label that decoded representation as original wire bytes.
    """
    base = urlsplit(base_url)
    package = Path(package).resolve(strict=True)
    resources = report.get('resources')
    if not isinstance(resources, list) or not resources:
        raise RuntimeContractError('Probe supplied no response bodies to bind to the package')
    checked = []
    for resource in resources:
        try:
            url = urlsplit(resource['url'])
            if ((url.scheme, url.hostname, url.port) != (base.scheme, base.hostname, base.port)
                    or url.username or url.password or url.fragment
                    or re.search(r'%(?:2f|5c)', url.path, re.I)):
                raise ValueError('response outside explicit origin or encoded separator')
            decoded = unquote_to_bytes(url.path).decode('utf-8', errors='strict')
            if (not decoded.startswith(base.path) or '\\' in decoded or ':' in decoded
                    or any(ord(c) < 32 or ord(c) == 127 for c in decoded)):
                raise ValueError('response outside exact package mount')
            parts = decoded[len(base.path):].split('/')
            if any(part in ('.', '..') for part in parts):
                raise ValueError('response path traversal')
            path = package.joinpath(*parts).resolve(strict=True)
            if path.is_dir():
                path = (path / 'index.html').resolve(strict=True)
            if not path.is_relative_to(package) or not path.is_file():
                raise ValueError('response does not resolve inside this package')
            package_size, package_digest = path.stat().st_size, _digest(path)
            encoding = resource['body_encoding']
            if encoding == 'base64':
                expected_size, expected_digest, bom_removed = package_size, package_digest, False
                representation = 'BINARY_BYTES'
            elif encoding == 'utf8':
                if package_size > 32 * 1024 * 1024:
                    raise ValueError('text body exceeds the explicit CDP capture bound')
                raw = path.read_bytes()
                if hashlib.sha256(raw).hexdigest() != package_digest:
                    raise ValueError('package changed while reading the text representation')
                decoded_text = raw.decode('utf-8-sig', errors='strict').encode('utf-8')
                expected_size, expected_digest = len(decoded_text), hashlib.sha256(decoded_text).hexdigest()
                bom_removed = raw.startswith(b'\xef\xbb\xbf')
                representation = 'CDP_DECODED_UTF8'
            else:
                raise ValueError('missing or unsupported captured body representation')
            if (type(resource['bytes']) is not int or resource['bytes'] != expected_size
                    or not isinstance(resource['sha256'], str)
                    or not re.fullmatch('[0-9a-f]{64}', resource['sha256'])
                    or expected_digest != resource['sha256']):
                raise ValueError('captured response body differs from the package representation')
            checked.append(dict(url=resource['url'], path=path.relative_to(package).as_posix(),
                                bytes=resource['bytes'], sha256=resource['sha256'], representation=representation,
                                utf8_bom_removed=bom_removed, package_bytes=package_size, package_sha256=package_digest))
        except (KeyError, TypeError, ValueError, OSError) as error:
            raise RuntimeContractError(f'Cannot bind browser response to package: {resource!r}: {error}') from error
    return dict(status='RESPONSE_BODY_PASS', checked=len(checked), resources=checked,
                scope='binary_bytes_and_decoded_utf8_text; dynamic_completeness_and_text_wire_bytes_not_proven')


class _ChromeTemporaryDirectory:
    """Own a short POSIX socket root; persistent profile/logs stay in the attempt."""

    @staticmethod
    def _identity(info):
        if not stat.S_ISDIR(info.st_mode):
            raise RuntimeContractError('Chrome temporary path is not a plain directory')
        return dict(device=info.st_dev, inode=info.st_ino, uid=info.st_uid,
                    mode=stat.S_IMODE(info.st_mode))

    def __init__(self):
        # macOS /tmp is normally an alias for /private/tmp. Resolve that known
        # OS root once; the allocated path and every subsequent use are plain.
        base = Path('/tmp').resolve(strict=True)
        self.parents = {path: self._identity(path.lstat()) for path in (base, *base.parents)}
        self.path = Path(tempfile.mkdtemp(prefix='caesura-chrome-', dir=base))
        self.parent_fd = self.root_fd = None
        try:
            flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
            self.parent_fd = os.open(base, flags)
            self.root_fd = os.open(self.path.name, flags, dir_fd=self.parent_fd)
            identity = self._identity(os.fstat(self.root_fd))
            if identity['uid'] != os.geteuid() or identity['mode'] != 0o700:
                raise RuntimeContractError('Chrome temporary root must be owned by this user with mode 0700')
            # Leave room for Chrome's random subdirectory and SingletonSocket;
            # 103 bytes also fits Darwin's smaller sockaddr_un.sun_path.
            socket_bytes = len(os.fsencode(self.path / 'com.google.Chrome.XXXXXXXX' / 'SingletonSocket'))
            if socket_bytes > 103:
                raise RuntimeContractError('Canonical Chrome temporary root exceeds the POSIX socket budget')
            self.record = dict(path=str(self.path), requested_base='/tmp', canonical_base=str(base),
                               identity=identity, socket_path_budget_bytes=socket_bytes,
                               scope='Chrome transient TMP/TEMP/TMPDIR only')
            self._check()
        except BaseException as error:
            self._close()
            # Retain any allocated directory on uncertain ownership.
            raise RuntimeContractError(f'Chrome temporary allocation failed at {self.path}: {error}') from error

    def _check(self):
        if any(self._identity(path.lstat()) != identity for path, identity in self.parents.items()):
            raise RuntimeContractError('Chrome temporary parent identity changed')
        if (self._identity(os.fstat(self.parent_fd)) != self.parents[self.path.parent]
                or self._identity(self.path.lstat()) != self.record['identity']
                or self._identity(os.fstat(self.root_fd)) != self.record['identity']):
            raise RuntimeContractError('Chrome temporary root identity changed')

    def _close(self):
        for name in ('root_fd', 'parent_fd'):
            descriptor = getattr(self, name)
            if descriptor is not None:
                os.close(descriptor)
                setattr(self, name, None)

    def cleanup(self, *, process_exited):
        result = dict(self.record, status='CLEANUP_FAIL', removed=False,
                      process_cleanup_confirmed=process_exited, errors=[])
        try:
            if not process_exited:
                raise RuntimeContractError('Retaining Chrome temporary root without complete process cleanup')
            self._check()
            if not shutil.rmtree.avoids_symlink_attacks:
                raise RuntimeContractError('Descriptor-relative safe directory cleanup is unavailable')
            # Recurse only through the retained owned directory descriptor;
            # symlinks and socket leaves are unlinked without following them.
            for name in os.listdir(self.root_fd):
                info = os.stat(name, dir_fd=self.root_fd, follow_symlinks=False)
                if stat.S_ISDIR(info.st_mode):
                    shutil.rmtree(name, dir_fd=self.root_fd)
                else:
                    os.unlink(name, dir_fd=self.root_fd)
            self._check()
            os.rmdir(self.path.name, dir_fd=self.parent_fd)
            result.update(status='CLEANUP_PASS', removed=True)
        except BaseException as error:
            result['errors'].append(f'{type(error).__name__}: {error}')
        finally:
            self._close()
        return result


def _probe(phase, scenario, server, browser, endpoint, node, *, env, previous=None, actions=None):
    output = scenario / phase
    argv = [node, str(PROBE), '--phase', phase, '--url', server.url,
            '--cdp-url', endpoint['cdp_url'], '--browser-pid', str(browser.identity.pid),
            '--output', str(output), '--timeout-ms', '90000']
    if actions is not None:
        argv.extend(('--actions', actions['path'], '--actions-sha256', actions['sha256']))
    if previous is not None:
        record = _read_json(previous)
        if (record.get('status') != 'BOOT_READY' or record.get('browser_pid') != browser.identity.pid
                or record.get('browser_pid_verified') is not True or record.get('url') != server.url
                or record.get('cdp_url') != endpoint['cdp_url'] or not record.get('target_id')):
            raise RuntimeContractError('Boot session provenance changed before offline probe')
        argv.extend(('--target-id', record['target_id'], '--previous', str(previous)))
    command = _OwnedCommand(argv, scenario, env, scenario / (phase + '-command'), timeout=120)
    result = None
    try:
        run = command.wait_result()
        report = _read_json(output / 'report.json')
        if (run.get('actual_exit_code') != 0 or report.get('schema') != 'caesura.web-package-probe.v1'
                or report.get('status') != 'PROBE_PASS'
                or report.get('phase') != phase or report.get('browser_pid') != browser.identity.pid
                or report.get('browser_pid_verified') is not True or report.get('url') != server.url
                or report.get('cdp_url') != endpoint['cdp_url']):
            raise RuntimeContractError(f'{phase} probe did not pass its exact browser/session contract')
        if actions is not None and any((report.get('actions') or {}).get(key) != actions[key]
                                       for key in ('path', 'sha256')):
            raise RuntimeContractError('Probe did not bind the declared UI action input')
        binding = _verify_response_bytes(report, Path(server.ready['root']), server.url)
        result = dict(run=run, report=report, response_body_binding=binding, path=str(output / 'report.json'),
                      sha256=_digest(output / 'report.json'))
    finally:
        cleanup = command.stop()
        if cleanup['status'] != 'CLEANUP_PASS':
            raise RuntimeContractError(f'{phase} probe process tree did not finish cleanly')
    result['cleanup'] = cleanup
    return result


def _scenario(package, attempt, name, prefix, tools, mode, actions=None):
    directory = attempt / name
    directory.mkdir()
    for child in ('home', 'temp', 'profile'):
        (directory / child).mkdir()
    if os.name == 'nt':
        # Windows known-folder resolution requires the redirected roots to
        # exist before Chrome checks its default versus private data directory.
        for child in ('Local', 'Roaming'):
            (directory / 'home/AppData' / child).mkdir(parents=True)
    env = isolated_web_env(directory / 'home', directory / 'temp', directory)
    result = dict(name=name, prefix=prefix, status='SCENARIO_FAIL', stages={}, cleanup={}, errors=[])
    server = browser = chrome_temp = None
    try:
        server = start_package_server(package, directory, env, prefix=prefix)
        result['server'] = dict(server.ready, url=server.url, identity=asdict(server.identity))
        # Chrome's port=0 can select a Fetch/WebSocket-blocked port. Reserve a
        # browser-usable port, release only this socket, then insist that the
        # launched Chrome identity exclusively owns it before any HTTP/CDP.
        reservation, rejected_ports = _bind_http(0)
        debugger_port = reservation.server_address[1]
        reservation.server_close()
        result['debugger_allocation'] = dict(port=debugger_port, rejected_browser_ports=rejected_ports,
                                             reservation='CLOSED_BEFORE_CHROME', race_policy='FAIL_ON_FOREIGN_OWNER')
        argv = [tools['browser']['path'], '--remote-debugging-address=127.0.0.1', f'--remote-debugging-port={debugger_port}',
                '--user-data-dir=' + str(directory / 'profile'), '--no-first-run', '--no-default-browser-check',
                '--disable-background-networking', '--disable-component-update', '--disable-sync',
                '--disable-default-apps', '--no-proxy-server', '--mute-audio', '--window-size=1280,900']
        if os.name == 'nt':
            # An elevated Windows host must retain this owned browser PID;
            # Chrome's automatic de-elevation otherwise exits and relaunches.
            argv.append('--do-not-de-elevate')
        if mode == 'headless=new':
            argv.append('--headless=new')
        argv.append('about:blank')
        result['browser_argv'], result['browser_mode'] = argv, mode
        browser_env = dict(env)
        if os.name == 'posix':
            chrome_temp = _ChromeTemporaryDirectory()
            result['chrome_temp'] = chrome_temp.record
            _json_new(directory / 'chrome-temp.json', chrome_temp.record)
            browser_env.update(TMP=str(chrome_temp.path), TEMP=str(chrome_temp.path), TMPDIR=str(chrome_temp.path))
        browser = _OwnedCommand(argv, directory, browser_env, directory / 'browser', timeout=300)
        browser.wait_identity(timeout=30)
        endpoint = browser.wait_for(lambda: _chrome_ready(browser, directory / 'profile', expected_port=debugger_port),
                                    timeout=45, description='owned private-profile Chrome debugger')
        result['browser'] = endpoint
        result['stages']['boot'] = _probe('boot', directory, server, browser, endpoint, tools['node']['path'], env=env, actions=actions)
        result['cleanup']['server'] = server.stop()
        if result['cleanup']['server']['status'] != 'CLEANUP_PASS':
            raise RuntimeContractError('HTTP service did not close before offline verification')
        verify_owned_listener(browser.identity, endpoint['port'])
        result['stages']['offline'] = _probe('offline', directory, server, browser, endpoint,
                                            tools['node']['path'], env=env, previous=directory / 'boot/session.json', actions=actions)
        result['status'] = 'SCENARIO_PASS'
    except BaseException as error:
        result['errors'].append(f'{type(error).__name__}: {error}')
    finally:
        for label, owned in (('server', server), ('browser', browser)):
            if owned is None:
                continue
            try:
                result['cleanup'][label] = owned.stop()
            except BaseException as error:
                result['cleanup'][label] = dict(status='CLEANUP_FAIL', error=f'{type(error).__name__}: {error}')
            if result['cleanup'][label]['status'] != 'CLEANUP_PASS':
                result['status'] = 'SCENARIO_FAIL'
        if chrome_temp is not None:
            browser_cleanup = result['cleanup'].get('browser', {})
            complete = browser is None or (browser_cleanup.get('status') == 'CLEANUP_PASS'
                                           and browser_cleanup.get('process_exited') is True)
            result['cleanup']['chrome_temp'] = chrome_temp.cleanup(process_exited=complete)
            if result['cleanup']['chrome_temp']['status'] != 'CLEANUP_PASS':
                result['status'] = 'SCENARIO_FAIL'
        _json_new(directory / 'scenario.json', result)
    return result


def _lock_actions(path, digest, package, attempt):
    if path is None and digest is None:
        return None
    if path is None or not isinstance(digest, str) or not re.fullmatch('[0-9a-f]{64}', digest):
        raise RuntimeContractError('UI actions need both an explicit file and its prelocked SHA256')
    supplied = Path(path)
    if not supplied.is_absolute() or not _plain_file(supplied):
        raise RuntimeContractError('UI actions must be an absolute plain file')
    resolved = supplied.resolve(strict=True)
    if resolved.is_relative_to(package) or resolved.is_relative_to(attempt):
        raise RuntimeContractError('UI actions must be supplied outside the package and new attempt')
    if not 0 < resolved.stat().st_size <= 65536 or _digest(resolved) != digest:
        raise RuntimeContractError('UI action file is oversized, empty or differs from its prelocked SHA256')
    return dict(path=str(resolved), sha256=digest)


def run_web_package(package_root, attempt_dir, *, node_executable, browser_executable, lua_executable,
                    actions_path=None, actions_sha256=None):
    """Run root/subpath in separate real browser/server processes and keep all evidence.

    The caller's static stage uses Lua; runtime records its identity but never
    claims to execute it. An invalid/reused attempt path raises before mutation;
    all failures inside an allocated attempt return and persist RUNTIME_FAIL.
    """
    package = Path(package_root).resolve(strict=True)
    attempt = _new_attempt(package, attempt_dir)
    report = dict(schema=SCHEMA, status='RUNTIME_FAIL', started_at=_now(), package_root=str(package),
                  attempt_dir=str(attempt), tools={}, scenarios=[], errors=[],
                  static_stage='REQUIRED_BY_CALLER', environment_scope='filtered_environment_not_filesystem_sandbox',
                  not_run=['other_browser_families', 'other_host_platforms', 'PWA_OS_installation', 'physical_audio_output'])
    before = None
    try:
        report['actions'] = _lock_actions(actions_path, actions_sha256, package, attempt)
        report['tools'] = dict(node=_tool(node_executable, 'CDP_probe'), browser=_tool(browser_executable, 'real_browser'),
                               lua=_tool(lua_executable, 'declared_for_static_stage_not_executed_by_runtime'),
                               python=_tool(sys.executable, 'HTTP_and_owned_launchers'))
        report['validator_sources'] = {str(path): _digest(path) for path in (
            Path(__file__).resolve(), SCRIPT_ROOT / 'package_runtime.py', SCRIPT_ROOT / 'validation_process.py',
            SCRIPT_ROOT / 'package_verification.py', PROBE)}
        before = inspect_inventory(package)
        report['package_before'] = before
        ci = os.environ.get('CI', '').lower() in {'1', 'true', 'yes'}
        mode = 'headless=new' if ci else 'headed'
        report['browser_mode'] = mode
        for name, prefix in PREFIXES:
            scenario = _scenario(package, attempt, name, prefix, report['tools'], mode, report['actions'])
            report['scenarios'].append(scenario)
            if scenario['status'] != 'SCENARIO_PASS':
                break  # retain the first failure; do not retry or relabel it
        if len(report['scenarios']) == 2 and all(item['status'] == 'SCENARIO_PASS' for item in report['scenarios']):
            report['status'] = 'RUNTIME_PASS'
    except BaseException as error:
        report['errors'].append(f'{type(error).__name__}: {error}')
    finally:
        try:
            if report.get('actions') is not None:
                action = report['actions']
                if _digest(Path(action['path'])) != action['sha256']:
                    raise RuntimeContractError('UI action input changed during runtime validation')
            report['package_after'] = inspect_inventory(package)
            report['package_stable'] = report['package_after'] == before if before is not None else None
            report['tools_stable'] = (all(_digest(Path(item['path'])) == item['sha256'] for item in report['tools'].values())
                                      if report['tools'] else None)
            report['validator_sources_stable'] = (all(_digest(Path(path)) == digest for path, digest in report.get('validator_sources', {}).items())
                                                  if report.get('validator_sources') else None)
            if not report['package_stable'] or not report['tools_stable'] or not report['validator_sources_stable']:
                report['status'] = 'RUNTIME_FAIL'
                report['errors'].append('Package/tool/validator identity validation did not complete or changed')
        except BaseException as error:
            report['status'] = 'RUNTIME_FAIL'
            report['errors'].append(f'Final identity check: {type(error).__name__}: {error}')
        report['evidence_files'] = []
        report['scenario_status'] = {name: next((item['status'] for item in report['scenarios'] if item['name'] == name),
                                               'NOT_RUN') for name, _ in PREFIXES}
        # Browser profile/cache files are mutable implementation state, not raw
        # verification evidence. Keep them on disk; hash only explicit reports/logs.
        for path in sorted(attempt.rglob('*')):
            if path.is_file() and 'profile' not in path.relative_to(attempt).parts and path.suffix in ('.json', '.jsonl', '.log', '.png'):
                try:
                    if not _plain_file(path) or not path.resolve(strict=True).is_relative_to(attempt):
                        raise RuntimeContractError('Evidence path must be a plain file inside this attempt')
                    report['evidence_files'].append(dict(path=str(path), sha256=_digest(path), size=path.stat().st_size))
                except BaseException as error:
                    report['status'] = 'RUNTIME_FAIL'
                    report['errors'].append(f'Evidence identity: {path}: {type(error).__name__}: {error}')
        report['finished_at'] = _now()
        _json_new(attempt / 'report.json', report)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serve', action='store_true')
    parser.add_argument('--package-root', type=Path, required=True)
    parser.add_argument('--ready', type=Path, required=True)
    parser.add_argument('--prefix', choices=('/', '/games/package/'), default='/')
    parser.add_argument('--port', type=int, default=0, help='0 selects a fresh OS port; explicit ports support conflict negative controls')
    args = parser.parse_args()
    if not args.serve:
        parser.error('Import run_web_package from the final-package controller')
    serve_package(args.package_root, args.ready, prefix=args.prefix, port=args.port)


if __name__ == '__main__':
    main()
