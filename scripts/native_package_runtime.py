#!/usr/bin/env python3
"""Run one explicitly prepared native payload in a fresh external workspace.

The caller locks package/source/configuration and performs static inspection.
This lane never selects an archive, supplies missing assets, or publishes a
release. All executable lifecycles use package_runtime's existing owned runner.
Raw logs can contain the disposable editor token; reports contain hashes only.
"""
from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from dataclasses import asdict
import base64
import ctypes
import hashlib
import http.client
import json
import math
import os
from pathlib import Path
import re
import secrets
import shutil
import socket
import subprocess
import sys
import time
from urllib.parse import quote

from package_runtime import (ProcessIdentity, RuntimeContractError, native_env,
                             process_identity, loopback_listeners,
                             verify_owned_listener, run_runtime_command)
from package_verification import inspect_inventory, _sha256_file, _reparse
from verify_native_package import _configuration
from macho_dependencies import (is_macho, inspect_macho, inspect_package_closure,
                                is_system_library, library_path_hint)

class _ObservationError(RuntimeContractError):
    def __init__(self, message, observations):
        super().__init__(message)
        self.observations = observations


class _LibrariesPending(RuntimeContractError):
    """The owned process has not mapped every required image yet."""


def _sha(path: Path) -> str:
    return _sha256_file(path)


def _need(condition, message):
    if not condition:
        raise RuntimeContractError(message)


def _reserve_editor_port(requested=None):
    """Reserve one usable loopback port; caller closes only this reservation.

    None asks the OS for a port. Explicit ports never silently fall back. Engine
    accepts the selected value through CAESURA_EDITOR_PORT. Releasing the socket
    before Engine binds leaves a race: subsequent PID/creation/listener checks
    must fail on a foreign owner, never adopt or terminate it.
    """
    _need(requested is None or (type(requested) is int and 1 <= requested <= 65535),
          "Explicit editor port must be an integer in 1..65535")
    if requested is not None:
        _need(not loopback_listeners(requested), f"Editor port {requested} is already owned")
    reservation = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        if os.name == "nt":
            reservation.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        reservation.bind(("127.0.0.1", requested if requested is not None else 0))
        selected = reservation.getsockname()[1]
        _need(not loopback_listeners(selected), f"Editor port {selected} has another loopback owner")
        reservation.listen(1)
        return reservation, {"port":selected, "requested_port":requested,
            "selection":"os_assigned" if requested is None else "explicit",
            "address":"127.0.0.1", "race_policy":"FAIL_ON_FOREIGN_OWNER"}
    except BaseException:
        reservation.close()
        raise


def _file(path: Path) -> dict:
    resolved = path.resolve(strict=True)
    _need(resolved.is_file() and resolved.stat().st_size > 0, f"Missing/empty file: {path}")
    return {"path": str(path), "resolved_path": str(resolved), "sha256": _sha(resolved)}


def _native_argv(executable: Path, *arguments: str) -> list[str]:
    return [str(executable), *arguments]


def _audio_evidence(text, mode, *, require_signal):
    """Bind explicit output selection and software mixer observations to raw logs.

    Device initialization is not proof of physical audibility. Software output
    is deliberately a discard sink and must never claim a physical device test.
    """
    _need(mode in ('device', 'software'), 'Unknown audio output mode')
    physical = 'NOT_RUN' if mode == 'software' else 'NOT_VERIFIED'
    markers = re.findall(r'^\[Audio\] Output mode: (.*)$', text, re.MULTILINE)
    _need(markers == [f'{mode}; physical_device={physical}'], 'Audio output selection mismatch')
    _need('[Audio] SoLoud initialized: 3 buses' in text and 'Using NullAudioBackend' not in text,
          'Selected real audio backend did not initialize')
    result = {'mode':mode, 'physical_device':physical}
    if mode == 'device':
        return result
    rows = re.findall(r'^\[Audio\] Software mix stats: (.*)$', text, re.MULTILINE)
    _need(len(rows) == 1, 'Software audio requires one completed mixer session')
    def unique(pairs):
        value = {}
        for key, item in pairs:
            _need(key not in value, 'Duplicate software audio statistic')
            value[key] = item
        return value
    stats = json.loads(rows[0], object_pairs_hook=unique)
    _need(isinstance(stats, dict), 'Software audio statistics must be an object')
    for key in ('frames', 'samples', 'nonzero_samples', 'nonfinite_samples'):
        _need(type(stats.get(key)) is int and 0 <= stats[key] < 2**64, f'Invalid audio counter: {key}')
    _need(stats['frames'] > 0 and stats['samples'] == 2 * stats['frames'], 'Software mixer did not advance stereo PCM')
    _need(stats['nonfinite_samples'] == 0 and stats['nonzero_samples'] <= stats['samples'], 'Invalid software PCM samples')
    for key in ('peak', 'absolute_energy'):
        _need(type(stats.get(key)) in (int, float) and math.isfinite(stats[key]) and stats[key] >= 0,
              f'Invalid software PCM magnitude: {key}')
    _need(type(stats.get('sample_rate')) is int and stats['sample_rate'] == 48000
          and type(stats.get('channels')) is int and stats['channels'] == 2
          and stats.get('physical_device') == 'NOT_RUN' and stats.get('saturated') is False,
          'Software mixer configuration or finite accumulation mismatch')
    signal = stats['nonzero_samples'] > 0
    _need((stats['peak'] > 0) == signal and (stats['absolute_energy'] > 0) == signal,
          'Software PCM energy and signal counters disagree')
    _need(not require_signal or signal, 'Packaged demo produced no nonzero software PCM')
    result['statistics'] = stats
    return result


def _host_platform() -> str:
    if os.name == "nt":
        return "windows"
    if sys.platform.startswith("linux"):
        return "linux"
    if sys.platform == "darwin":
        return "macos"
    raise RuntimeContractError(f"No native runtime lane for this host: {sys.platform}")


def _lsof_path(field: bytes) -> str:
    """Decode exactly one C-locale lsof name field, never Python escapes."""
    _need(field.startswith(b"/"), "lsof returned a non-absolute image path")
    _need(all(32 <= char < 127 for char in field), "lsof returned a non-ASCII C-locale field")
    # lsof emits ^A for both byte 0x01 and literal '^A'. That representation
    # cannot prove a path identity, even with -F0; do not guess either spelling.
    _need(not re.search(rb"\^[@-\x5f?]", field), "Ambiguous lsof caret path spelling")
    output = bytearray()
    escapes = {ord("\\"):92, ord("b"):8, ord("f"):12,
               ord("r"):13, ord("n"):10, ord("t"):9}
    index = 0
    while index < len(field):
        char = field[index]
        if char != 92:
            output.append(char)
            index += 1
            continue
        _need(index + 1 < len(field), "Incomplete lsof path escape")
        code = field[index + 1]
        if code == ord("x"):
            digits = field[index + 2:index + 4]
            _need(len(digits) == 2 and re.fullmatch(rb"[0-9a-fA-F]{2}", digits),
                  "Invalid lsof hexadecimal path escape")
            output.append(int(digits, 16))
            index += 4
        else:
            _need(code in escapes, "Unknown lsof path escape")
            output.append(escapes[code])
            index += 2
    _need(0 not in output, "NUL in lsof image path")
    return output.decode("utf-8", errors="strict")


def _observer_bytes(data: bytes) -> dict:
    return {"base64":base64.b64encode(data).decode("ascii"),
            "sha256":hashlib.sha256(data).hexdigest(), "size":len(data)}


def observe_loaded_modules(identity: ProcessIdentity) -> dict:
    """Read this live process's mapped images; retain failures without accepting them."""
    report = {"status":"NOT_VERIFIED", "process":asdict(identity),
              "paths":[], "reported_paths":[]}
    try:
        return _observe_loaded_modules(identity, report)
    except (OSError, ValueError, RuntimeContractError, subprocess.SubprocessError) as error:
        error.module_observation = report
        raise


def _observe_loaded_modules(identity: ProcessIdentity, report: dict) -> dict:
    _need(process_identity(identity.pid) == identity, "Module owner changed before observation")
    if os.name == "nt":
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        psapi = ctypes.WinDLL("psapi", use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        psapi.EnumProcessModulesEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                                               ctypes.POINTER(wintypes.DWORD), wintypes.DWORD]
        psapi.EnumProcessModulesEx.restype = wintypes.BOOL
        psapi.GetModuleFileNameExW.argtypes = [wintypes.HANDLE, wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD]
        psapi.GetModuleFileNameExW.restype = wintypes.DWORD
        handle = kernel.OpenProcess(0x0410, False, identity.pid)  # QUERY_INFORMATION | VM_READ
        _need(handle, "Cannot open the recorded process for module inspection")
        try:
            count = 256
            while True:
                modules = (wintypes.HMODULE * count)()
                needed = wintypes.DWORD()
                _need(psapi.EnumProcessModulesEx(handle, modules, ctypes.sizeof(modules),
                                                ctypes.byref(needed), 3), "Cannot enumerate loaded modules")
                if needed.value <= ctypes.sizeof(modules):
                    break
                count = (needed.value + ctypes.sizeof(wintypes.HMODULE) - 1) // ctypes.sizeof(wintypes.HMODULE)
                _need(count <= 65536, "Unbounded loaded module enumeration")
            paths = []
            for module in modules[:needed.value // ctypes.sizeof(wintypes.HMODULE)]:
                buffer = ctypes.create_unicode_buffer(32768)
                size = psapi.GetModuleFileNameExW(handle, module, buffer, len(buffer))
                _need(0 < size < len(buffer) - 1, "Cannot read an exact loaded module path")
                paths.append(buffer.value)
        finally:
            kernel.CloseHandle(handle)
        source = "windows:EnumProcessModulesEx/GetModuleFileNameExW"
    elif sys.platform.startswith("linux"):
        paths = []
        for line in Path(f"/proc/{identity.pid}/maps").read_text().splitlines():
            fields = line.split(maxsplit=5)
            if len(fields) == 6 and "x" in fields[1] and fields[5].startswith("/"):
                _need(not fields[5].endswith(" (deleted)"), "An executable mapping was deleted")
                paths.append(fields[5].replace("\\012", "\n"))
        source = "linux:/proc/pid/maps executable mappings"
    elif sys.platform == "darwin":
        # txt descriptors are mapped program text, including private dylibs.
        # Shared-cache system images may be absent; required packaged images
        # must actually appear. Required-image and observer errors fail the lane.
        tool = Path("/usr/sbin/lsof")
        _need(tool.is_file(), "macOS mapped-image observer /usr/sbin/lsof is unavailable")
        source = "macos:lsof txt mapped program text"
        argv = [str(tool), "-a", "-p", str(identity.pid), "-d", "txt", "-Fn"]
        environment = {"PATH":"/usr/bin:/bin:/usr/sbin", "LC_ALL":"C"}
        observer = {"tool":_file(tool), "argv":argv, "environment":environment,
                    "returncode":None, "stdout":_observer_bytes(b""), "stderr":_observer_bytes(b"")}
        report.update(source=source, observer=observer)
        try:
            result = subprocess.run(argv, capture_output=True, timeout=10, env=environment)
        except subprocess.TimeoutExpired as error:
            observer.update(stdout=_observer_bytes(error.stdout or b""),
                            stderr=_observer_bytes(error.stderr or b""))
            raise
        observer.update(returncode=result.returncode, stdout=_observer_bytes(result.stdout),
                        stderr=_observer_bytes(result.stderr))
        fields = result.stdout.split(b"\n")
        report["reported_paths"] = [line[1:].decode("ascii", errors="backslashreplace")
                                    for line in fields if line.startswith(b"n")]
        _need(result.returncode == 0, "macOS mapped-image observation failed")
        _need(fields[-1] == b"" and fields[0] == f"p{identity.pid}".encode("ascii"),
              "Invalid lsof field framing or process owner")
        image_fields = fields[1:-1]
        # Apple's lsof includes the file descriptor even with -Fn. Newer lsof
        # may omit it. Accept complete txt/name pairs or name-only records;
        # never silently discard unknown descriptors or broken boundaries.
        if image_fields[:1] == [b"ftxt"]:
            _need(len(image_fields) % 2 == 0 and
                  all(line == b"ftxt" for line in image_fields[::2]),
                  "Invalid lsof txt record framing")
            image_fields = image_fields[1::2]
        _need(all(line.startswith(b"n/") for line in image_fields), "Unknown lsof image field")
        paths = [_lsof_path(line[1:]) for line in image_fields]
        report["decoded_paths"] = paths
    else:
        raise RuntimeContractError("Loaded module observation is NOT_VERIFIED on this host")
    _need(process_identity(identity.pid) == identity, "Module owner changed during observation")
    _need(paths, "No executable modules were observed")
    resolved, missing, inaccessible = set(), [], []
    report.update(source=source, missing_paths=missing, inaccessible_paths=inaccessible)
    for path in paths:
        try:
            normalized = str(Path(path).resolve(strict=True))
        except (FileNotFoundError, PermissionError) as error:
            if sys.platform != "darwin":
                raise
            # lsof txt also includes non-library mappings such as macOS's
            # transient logging cache or protected analytics files. Retain
            # unresolved paths and the original error. Required or foreign
            # same-name images remain fatal below; unrelated system mappings
            # must not hide the actually observed packaged SDL image.
            normalized = str(Path(path).resolve(strict=False))
            affected = inaccessible if isinstance(error, PermissionError) else missing
            affected.append({"path":normalized, "reported_path":path,
                             "error":f"{type(error).__name__}: {error}"})
        resolved.add(normalized)
        report["paths"] = sorted(resolved)
    report["status"] = "OBSERVED"
    return report


def _macos_dependency_images(identity: ProcessIdentity, package: Path, report: dict) -> list[str]:
    """Check actual mapped images while retaining unrelated txt resources.

    lsof also reports fonts, .car files and protected/transient OS caches.
    Their inability to be re-opened is not proof that they are dylibs. Actual
    Mach-O files and unresolved dylib/framework image names are treated as code.
    The static lane separately proves the executable's dependency closure.
    """
    observed = {"status":"NOT_VERIFIED", "images":[], "system_paths":[],
                "non_image_paths":[], "unavailable_nonlibrary_paths":[]}
    report["dependency_images"] = observed
    package = package.resolve(strict=True)
    unavailable = {item["path"] for key in ("missing_paths", "inaccessible_paths")
                   for item in report.get(key, [])}
    for value in report["paths"]:
        path = Path(value)
        if is_system_library(path.as_posix()):
            observed["system_paths"].append(value)
            continue
        if value in unavailable:
            _need(not library_path_hint(value), f"Mapped non-system library is unavailable: {value}")
            observed["unavailable_nonlibrary_paths"].append(value)
            continue
        try:
            image = is_macho(path)
        except (FileNotFoundError, PermissionError):
            _need(not library_path_hint(value), f"Mapped non-system library disappeared or became inaccessible: {value}")
            observed["unavailable_nonlibrary_paths"].append(value)
            continue
        if not image:
            # A mapped resource is not code merely because it is inside the
            # package. This classification does not replace static image checks.
            _need(path.is_relative_to(package) or not library_path_hint(value),
                  f"Mapped external library is not a verifiable Mach-O image: {value}")
            observed["non_image_paths"].append(value)
            continue
        _need(path.is_relative_to(package), f"Mapped non-system Mach-O image is outside package: {value}")
        parsed = inspect_macho(path)
        observed["images"].append({"path":value, "sha256":parsed["sha256"], "cpu_type":parsed["cpu_type"],
                                   "file_type":parsed["file_type"]})
    derived = []
    executable = Path(identity.executable)
    if is_macho(executable):
        closure = inspect_package_closure(package, [executable], [])
        report["dependency_closure"] = closure
        derived = [image["relative_path"] for image in closure["images"]
                   if image["file_type"] in (6, 8)]
    observed["status"] = "MAPPED_IMAGES_CHECKED"
    return derived


def _inspect_libraries(identity: ProcessIdentity, package: Path, required: list[str]) -> dict:
    if not required and sys.platform != "darwin":
        return {"status":"NOT_REQUIRED", "required":[], "reason":"No shared libraries required by external configuration"}
    report = observe_loaded_modules(identity)
    try:
        observed = {Path(path) for path in report["paths"]}
        disappeared = {Path(item[key]) for item in report.get("missing_paths", [])
                       for key in ("path", "reported_path")}
        inaccessible = {Path(item[key]) for item in report.get("inaccessible_paths", [])
                        for key in ("path", "reported_path")}
        matched = []
        missing = []

        def inspect_required(relative):
            declared = package / relative
            expected = declared.resolve(strict=True)
            _need(expected.is_relative_to(package), f"Required library escapes package: {relative}")
            _need(not {expected, declared} & disappeared, f"Required library mapping disappeared: {relative}")
            _need(not {expected, declared} & inaccessible, f"Required library mapping inaccessible: {relative}")
            names = {declared.name.casefold(), expected.name.casefold()}
            _need(not any(path.name.casefold() in names and path not in {expected, declared}
                          for path in observed | disappeared | inaccessible),
                  f"A second source for required library was observed: {relative}")
            if expected not in observed:
                missing.append(relative)
                return
            matched.append({"relative_path":relative, "resolved_path":str(expected), "sha256":_sha(expected)})
        for relative in required:
            inspect_required(relative)
        if missing:
            raise _LibrariesPending("Required library not observed from this runtime package: " + ", ".join(missing))
        if sys.platform == "darwin":
            try:
                derived = _macos_dependency_images(identity, package, report)
            except ValueError as error:
                raise RuntimeContractError(f"Mapped Mach-O dependencies are NOT_VERIFIED: {error}") from error
            for relative in derived:
                if relative not in required:
                    inspect_required(relative)
            if missing:
                raise _LibrariesPending("Required dependency not observed from this runtime package: " + ", ".join(missing))
        return {**report, "status":"VERIFIED", "required":matched}
    except (OSError, ValueError, RuntimeContractError) as error:
        error.module_observation = report
        raise


def _loaded_libraries(identity: ProcessIdentity, package: Path, required: list[str], deadline=None) -> dict:
    try:
        while True:
            try:
                return _inspect_libraries(identity, package, required)
            except _LibrariesPending:
                if deadline is None or time.monotonic() >= deadline:
                    raise
                time.sleep(min(0.02, max(0, deadline - time.monotonic())))
    except (OSError, ValueError, RuntimeContractError, subprocess.SubprocessError) as error:
        details = dict(getattr(error, "module_observation", {}))
        details.update(status="NOT_VERIFIED", required=required, error=str(error),
                       error_type=type(error).__name__, process=asdict(identity))
        observed = {"loaded_modules":details}
        raise _ObservationError("Required loaded-library provenance is NOT_VERIFIED", observed) from error


def _http(identity: ProcessIdentity, port: int, path: str, token: str | None = None) -> tuple[int, bytes]:
    verify_owned_listener(identity, port)
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
    try:
        headers = {"Authorization":"Bearer " + token} if token is not None else {}
        connection.request("GET", path, headers=headers)
        response = connection.getresponse()
        body = response.read(4 * 1024 * 1024 + 1)
        _need(len(body) <= 4 * 1024 * 1024, "Editor HTTP response exceeds its bounded read")
        code = response.status
    finally:
        connection.close()
    verify_owned_listener(identity, port)
    return code, body


def _editor_protocol(identity, port, package, token, deadline, libraries) -> dict:
    token_file = package / ".caesura-editor-token"
    generated = token is None
    while True:
        _need(process_identity(identity.pid) == identity, "Editor exited before readiness")
        rows = loopback_listeners(port)
        if rows:
            verify_owned_listener(identity, port)  # A foreign owner fails immediately.
        if generated and token is None and token_file.exists():
            _need(token_file.is_file() and not token_file.is_symlink(), "Generated token must be a regular file")
            raw = token_file.read_bytes()
            _need(0 < len(raw) <= 4096, "Generated token has invalid size")
            token = raw.decode("utf-8").strip()
            _need(token and not any(ord(c) < 33 or ord(c) > 126 for c in token), "Generated token has invalid bytes")
        if rows and token:
            break
        _need(time.monotonic() < deadline, "Editor did not publish owned readiness and its required token")
        time.sleep(0.02)
    expected = (package / "web-editor/dist/index.html").read_bytes()
    code, body = _http(identity, port, "/", token)
    _need(code == 200 and body == expected and b"Caesura Web Editor" in body, "Authenticated editor HTML differs from this package")
    plain_code, plain = _http(identity, port, "/")
    browser_mode = "public"
    if plain_code != 200 or plain != expected:
        plain_code, plain = _http(identity, port, "/?token=" + quote(token, safe=""))
        browser_mode = "query_token"
    _need(plain_code == 200 and plain == expected, "Ordinary browser navigation cannot obtain packaged editor HTML")
    ping_code, ping = _http(identity, port, "/api/ping", token)
    _need(ping_code == 200 and json.loads(ping).get("status") == "ok", "Authenticated ping is not ok")
    denied, _ = _http(identity, port, "/api/ping")
    _need(denied == 401, "Unauthenticated editor API did not return 401")
    modules = _loaded_libraries(identity, package, libraries, deadline)
    return {"port":port, "html_status":code, "html_sha256":hashlib.sha256(body).hexdigest(),
            "browser_status":plain_code, "browser_mode":browser_mode,
            "ping_status":ping_code, "unauthenticated_status":denied,
            "token_source":"generated_file" if generated else "explicit_environment",
            "token_sha256":hashlib.sha256(token.encode()).hexdigest(), "loaded_modules":modules}


def _execute(attempt, name, argv, cwd, env, timeout, readiness_timeout, *, monitor=None, controlled=False,
             expected_final_executable=None, editor_port=None) -> dict:
    directory = attempt / "commands" / name
    directory.mkdir(parents=True)
    control = directory / "control"
    record = {"name":name, "argv":argv, "cwd":str(cwd), "executable":_file(Path(argv[0])),
              "passed":False, "errors":[], "run":None}
    if controlled:
        _need(type(editor_port) is int and 1 <= editor_port <= 65535, "Controlled editor needs its explicit port")
        record["editor_port"] = editor_port
    options = {"stop_request":control / "stop" if controlled else None}
    if expected_final_executable is not None:
        record["expected_final_executable"] = _file(Path(expected_final_executable))
        options.update(expected_final_executable=str(expected_final_executable),
                       exec_observation_timeout=readiness_timeout)
    with (directory / "stdout.log").open("xb") as out, (directory / "stderr.log").open("xb") as err:
        with ThreadPoolExecutor(max_workers=1) as executor:
            future = executor.submit(run_runtime_command, argv, cwd, env, control, out, err, timeout,
                                     **options)
            identity = None
            try:
                if monitor:
                    deadline = time.monotonic() + readiness_timeout
                    identity_file = control / "process.json"
                    while True:
                        _need(not future.done(), "Command exited before a readable process identity")
                        _need(time.monotonic() < deadline, "No readable owned process identity before readiness deadline")
                        try:
                            identity_json = identity_file.read_text(encoding="utf-8")
                        except (FileNotFoundError, PermissionError):
                            # Atomic publication guarantees complete JSON, but
                            # visibility need not imply readability on Windows.
                            # Wait only within this original readiness deadline;
                            # completed children and malformed identities fail.
                            time.sleep(0.02)
                            continue
                        identity = ProcessIdentity(**json.loads(identity_json))
                        break
                    record["observations"] = monitor(identity, deadline)
            except Exception as error:
                record["errors"].append(f"{type(error).__name__}: {error}")
                if isinstance(error, _ObservationError):
                    record["observations"] = error.observations
            finally:
                if controlled and control.is_dir() and not future.done():
                    try:
                        with (control / "stop").open("x", encoding="utf-8") as stream:
                            stream.write("stop this owned editor\n")
                    except OSError as error:
                        # Still join the owned runner and bind its failure/
                        # completion receipt if publishing stop itself fails.
                        record["errors"].append(f"Cannot publish controlled stop: {error}")
                try:
                    record["run"] = future.result()
                except Exception as error:
                    record["errors"].append(f"{type(error).__name__}: {error}")
                    if (control / "run.json").is_file():
                        record["run"] = json.loads((control / "run.json").read_text(encoding="utf-8"))
                if controlled:
                    try:
                        _need(not loopback_listeners(editor_port), "Editor port remains occupied after owned shutdown")
                        if identity:
                            try:
                                current = process_identity(identity.pid)
                            except RuntimeContractError:
                                current = None
                            _need(current != identity, "Owned editor is still alive after command return")
                    except RuntimeContractError as error:
                        record["errors"].append(str(error))
    for channel in ("stdout", "stderr"):
        path = directory / (channel + ".log")
        record[channel] = {"path":path.relative_to(attempt).as_posix(), "sha256":_sha(path), "size":path.stat().st_size}
    if (control / "run.json").is_file():
        record["receipt"] = {"path":(control / "run.json").relative_to(attempt).as_posix(),
                             "sha256":_sha(control / "run.json")}
    result = record["run"] or {}
    if result.get("owned_tree_cleanup") != "COMPLETE":
        record["errors"].append("Owned process tree cleanup was not confirmed")
    if expected_final_executable is not None:
        transition = result.get("exec_transition", {})
        if transition.get("status") != "VERIFIED" or transition.get("inputs_stable") is not True:
            record["errors"].append("Package launcher did not preserve and enter the declared final Engine")
        if (result.get("process") or {}).get("executable") != record["expected_final_executable"]["resolved_path"]:
            record["errors"].append("Observed launch child is not the expected Engine in this payload")
    if controlled:
        if result.get("status") != "STOPPED" or not result.get("stop_requested") or result.get("forced_kill"):
            record["errors"].append("Editor did not complete its requested controlled stop")
    elif result.get("status") != "EXITED" or result.get("actual_exit_code") != 0:
        record["errors"].append("Command did not exit normally with code 0")
    record["passed"] = not record["errors"]
    return record


def _log(attempt, command) -> str:
    return "\n".join((attempt / command[channel]["path"]).read_text(encoding="utf-8", errors="replace")
                     for channel in ("stdout", "stderr"))


def _verify_evidence(attempt, stages) -> list[str]:
    """Recheck the original bindings after all later commands have finished."""
    errors = []
    bindings = []
    for stage in stages:
        for command in stage["commands"]:
            for key in ("stdout", "stderr", "receipt"):
                if key in command:
                    bindings.append(command[key])
                else:
                    errors.append(f"Missing bound {key} evidence for command {command['name']}")
        if "build_info" in stage:
            bindings.append(stage["build_info"])
    for item in bindings:
        try:
            path = attempt / item["path"]
            _need(not Path(item["path"]).is_absolute() and path.resolve(strict=True).is_relative_to(attempt),
                  "Bound evidence escapes its runtime attempt")
            _need(_sha(path) == item["sha256"], "Bound evidence changed after its command completed")
        except Exception as error:
            errors.append(f"Evidence {item['path']}: {type(error).__name__}: {error}")
    return errors


def _copy_changes(before: dict, after: dict) -> dict:
    original = {item["path"]:item for item in before["entries"]}
    current = {item["path"]:item for item in after["entries"]}
    changed = [path for path, item in original.items() if current.get(path) != item]
    additions = [item for path, item in current.items() if path not in original]
    def allowed(item):
        path = item["path"]
        if item["type"] not in ("file", "directory"):
            return False
        if path == ".caesura-editor-token":
            return item["type"] == "file"
        return path.split("/", 1)[0] in {"logs", "cache", "saves", "settings"}
    unexpected = [item["path"] for item in additions if not allowed(item)]
    return {"passed":not changed and not unexpected, "changed_original_paths":changed,
            "unexpected_new_paths":unexpected, "allowed_additions":[item for item in additions if allowed(item)]}


def run_native_package(package_root, required_configuration, attempt_dir, *, python_executable,
                       command_timeout=120, readiness_timeout=45, launch_relative_path=None,
                       editor_port=None, audio_output='device') -> dict:
    report = {"schema":"caesura.native-package-runtime.v1", "status":"RUNTIME_FAIL",
              "runtime":"NOT_RUN", "source_stable":False, "runtime_copy_stable":False,
              "evidence_stable":False, "cleanup":"NOT_STARTED", "stages":[], "errors":[]}
    attempt = package = copy = before = copied = game = game_before = None
    reservation = None
    try:
        _need(audio_output in ('device', 'software'), 'Unknown audio output mode')
        report['audio_output'] = audio_output
        report['physical_audio_output'] = 'NOT_RUN' if audio_output == 'software' else 'NOT_VERIFIED'
        for value in (command_timeout, readiness_timeout):
            _need(not isinstance(value, bool) and math.isfinite(value) and value > 0, "Timeouts must be positive finite numbers")
        platform = _host_platform()
        _need(editor_port is None or (type(editor_port) is int and 1 <= editor_port <= 65535),
              "Explicit editor port must be an integer in 1..65535")
        _need(launch_relative_path in (None, "AppRun"), "Only an explicit packaged AppRun launcher is supported")
        _need(launch_relative_path is None or platform in ("linux", "macos"), "AppRun requires an actual POSIX host")
        configuration, libraries = _configuration(platform, required_configuration)
        report.update(platform=platform, required_configuration=configuration)
        package = Path(package_root).absolute()
        before = inspect_inventory(package)
        package = package.resolve(strict=True)
        report["input"] = {"path":str(package), "inventory_sha256":before["sha256"]}
        tool = Path(python_executable)
        _need(tool.is_absolute(), "Host Python must be an explicit absolute executable")
        tool = tool.resolve(strict=True)
        report["host_tools"] = {"author_python":_file(tool), "controller_python":_file(Path(sys.executable))}
        requested = Path(attempt_dir).absolute()
        parent = requested.parent.resolve(strict=True)
        _need(not any((p / ".git").exists() for p in (parent, *parent.parents)), "Runtime attempt must be outside repositories")
        candidate = parent / requested.name
        _need(not candidate.exists() and not candidate.is_symlink(), "Runtime attempt must be new")
        _need(not candidate.is_relative_to(package) and not package.is_relative_to(candidate), "Runtime attempt and input must be separate")
        _need(not (package / ".caesura-editor-token").exists(), "Input package already contains an editor token")
        candidate.mkdir(mode=0o700)
        attempt = candidate
        report["attempt_dir"] = str(attempt)
        copy = attempt / "runtime-package"
        shutil.copytree(package, copy, symlinks=True)
        copied = inspect_inventory(copy)
        _need(copied["sha256"] == before["sha256"], "Runtime copy differs from the explicit payload")
        for directory in ("home", "temp", "work"):
            (attempt / directory).mkdir()
        name = "CaesuraAmeKAG.exe" if platform == "windows" else "CaesuraAmeKAG"
        engine = copy / name
        lua = copy / ("external/lua/lua.exe" if platform == "windows" else "external/lua/lua")
        report["binaries"] = {"engine":_file(engine), "lua":_file(lua)}
        _need(all(Path(item["resolved_path"]).is_relative_to(copy) for item in report["binaries"].values()), "Runtime executable escapes package")
        env = native_env(copy, engine=engine, lua=lua, work=copy, home=attempt / "home", temp=attempt / "temp")
        launcher, launch_cwd, launch_options = engine, copy, {}
        if launch_relative_path is not None:
            launcher = copy / launch_relative_path
            _need(not launcher.is_symlink() and not _reparse(launcher) and launcher.is_file(),
                  "AppRun must be a plain file in the prepared package")
            _need(os.access(launcher, os.X_OK), "Packaged AppRun must be executable")
            report["binaries"]["launcher"] = _file(launcher)
            launch_cwd = attempt / "work"  # AppRun itself must change to its package root.
            launch_options["expected_final_executable"] = engine.resolve(strict=True)
            report["launch_scope"] = {"scope":"prepared_AppRun_and_Engine",
                "container_identity":"BOUND_BY_CALLER", "fuse_mount_runtime":"NOT_RUN",
                "apprun_stages":["editor_explicit_token", "editor_generated_token", "engine_frames"],
                "author_cli":"PACKAGED_CLI_COMMAND", "created_game":"ACTUAL_CLI_OUTPUT_ENGINE"}
        reservation, endpoint = _reserve_editor_port(editor_port)
        report["editor_endpoints"] = []
        report.update(runtime="RUNNING", cleanup="COMPLETE")

        def stage(title):
            value = {"name":title, "status":"RUNNING", "commands":[]}
            report["stages"].append(value)
            return value

        def command(current, label, argv, cwd, environment, **kwargs):
            value = _execute(attempt, label, argv, cwd, environment, command_timeout, readiness_timeout, **kwargs)
            current["commands"].append(value)
            _need(value["passed"], f"Command {label} failed; inspect bound command receipt and logs")
            return value

        for generated in (False, True):
            if reservation is None:
                # A completed HTTP session may leave TIME_WAIT entries. Let
                # the OS choose a fresh usable port for each default session;
                # explicit ports retain strict no-fallback behavior.
                reservation, endpoint = _reserve_editor_port(editor_port)
            current = stage("editor_generated_token" if generated else "editor_explicit_token")
            port = endpoint["port"]
            current["editor_endpoint"] = endpoint
            report["editor_endpoints"].append(dict(endpoint, stage=current["name"]))
            token = None if generated else secrets.token_hex(32)
            editor_env = dict(env)
            if token is not None:
                editor_env["CAESURA_EDITOR_TOKEN"] = token
            else:
                _need(not (copy / ".caesura-editor-token").exists(), "Generated-token session requires a fresh token file")
            editor_env["PWD"] = str(launch_cwd)
            editor_env["CAESURA_EDITOR_PORT"] = str(port)
            reservation.close()
            reservation = None
            _need(not loopback_listeners(port), "Editor port is occupied before a new session")
            value = command(current, current["name"], _native_argv(launcher, "--editor", "--audio-output", audio_output), launch_cwd, editor_env,
                            controlled=True, editor_port=port,
                            monitor=lambda identity, deadline: _editor_protocol(identity, port, copy, token, deadline, libraries),
                            **launch_options)
            if generated:
                _need("Generated editor token" in _log(attempt, value), "Generated-token startup marker was not logged")
            _need("web-editor/dist not found" not in _log(attempt, value), "Editor reported a missing web root")
            current["status"] = "PASS"

        current = stage("engine_frames")
        value = command(current, "engine_frames", _native_argv(launcher, "--frames", "60", "--audio-output", audio_output), launch_cwd, dict(env, PWD=str(launch_cwd)),
                        monitor=(lambda identity, deadline: {"loaded_modules":_loaded_libraries(identity, copy, libraries, deadline)}) if libraries else None,
                        **launch_options)
        text = _log(attempt, value)
        _need("rendering disabled (BGFX_DEBUG_IFH)" not in text and "[caesura] FATAL" not in text,
              "Ordinary engine reported a fatal boot or disabled rendering")
        current['audio'] = _audio_evidence(text, audio_output, require_signal=True)
        current["status"] = "PASS"

        current = stage("author_create_build")
        project = attempt / "作品 中文 项目"
        game = attempt / "作品 输出"
        probe = command(current, "packaged_lua", _native_argv(lua, "-v"), copy, env)
        _need("Lua 5." in _log(attempt, probe), "Packaged Lua did not report its version")
        cli = copy / "scripts/caesura.py"
        current["cli"] = _file(cli)
        current["authoritative_lua"] = report["binaries"]["lua"]
        created = command(current, "author_create", [str(tool), "-B", "-X", "utf8", str(cli), "create", project.name,
                                                     "--template", "basic", "--out", str(project)], attempt / "work", env)
        source = copy / "tools/project_templates/basic"
        _need(f"(template from: {source})" in _log(attempt, created), "Create did not identify this package's basic template")
        for path in source.rglob("*"):
            if path.is_file() and path.name != "caesura.project.json":
                relative = path.relative_to(source)
                _need((project / relative).is_file() and _sha(project / relative) == _sha(path), f"Created project differs from packaged template: {relative}")
        metadata = json.loads((project / "caesura.project.json").read_text(encoding="utf-8"))
        _need(metadata.get("name") == project.name and metadata.get("template") == "basic", "Created project metadata does not identify basic project")
        built = command(current, "author_build", [str(tool), "-B", "-X", "utf8", str(cli), "build", str(project),
                                                  "--engine", str(copy), "--out", str(game)], attempt / "work", env)
        info_path = game / "BUILD-INFO.json"
        info = json.loads(info_path.read_text(encoding="utf-8"))
        _need(info.get("kind") == "caesura-game-only" and info.get("schema") == 1, "Build did not produce game-only provenance")
        _need(info.get("precompile", {}).get("status") == "ok" and info["precompile"].get("scene_count", 0) > 0
              and not info.get("precompile_failures") and not info.get("assets_missing"), "Build did not verify all scenes and required assets")
        text = _log(attempt, built)
        _need(re.search(r"\[build\] ks_check: [1-9][0-9]* scene\(s\) pass contracts", text)
              and re.search(r"\[build\] precompile: [1-9][0-9]*/[1-9][0-9]* scene\(s\) cached into cache/ksc", text), "Required packaged-Lua checks were not reported")
        _need(info.get("capabilities", {}).get("profile", {}).get("binary_sha256") == report["binaries"]["engine"]["sha256"], "Build capability profile refers to another engine")
        _need(_sha(game / name) == report["binaries"]["engine"]["sha256"], "Built game engine bytes differ from this package")
        for key in ("entry_scene", "boot_script"):
            entry = (game / info[key]).resolve(strict=True)
            _need(entry.is_relative_to(game) and entry.is_file(), f"Invalid generated {key}")
        current.update(status="PASS", build_info={"path":info_path.relative_to(attempt).as_posix(), "sha256":_sha(info_path)},
                       project_path=str(project), game_path=str(game))
        game_before = inspect_inventory(game)
        report["created_game_input_sha256"] = game_before["sha256"]

        current = stage("created_game_frames")
        game_env = dict(env, PWD=str(game))
        # The game must not find missing runtime libraries in the source package
        # via PATH. It has embedded Lua; the standalone CLI interpreter is unused.
        game_env.pop("CAESURA_LUA", None)
        game_env["PATH"] = os.pathsep.join([str(game)] + [part for part in env["PATH"].split(os.pathsep)
                                                       if not Path(part).is_relative_to(copy)])
        game_libraries = []
        for relative in libraries:
            target = game / Path(relative).name
            _need(target.is_file() and _sha(target) == _sha(copy / relative), f"Created game lacks its own required library: {relative}")
            game_libraries.append(target.name)
        value = command(current, "created_game_frames", _native_argv(game / name, "--frames", "60", "--audio-output", audio_output), game, game_env,
                        monitor=(lambda identity, deadline: {"loaded_modules":_loaded_libraries(identity, game, game_libraries, deadline)}) if game_libraries else None)
        text = _log(attempt, value)
        _need("KAG Runner] Started" in text and "[caesura] FATAL" not in text
              and "rendering disabled (BGFX_DEBUG_IFH)" not in text, "Created game did not start its KAG runner with rendering enabled")
        current['audio'] = _audio_evidence(text, audio_output, require_signal=False)
        current["status"] = "PASS"
    except Exception as error:
        report["errors"].append(f"{type(error).__name__}: {error}")
        if report["stages"] and report["stages"][-1]["status"] == "RUNNING":
            report["stages"][-1]["status"] = "FAIL"
    finally:
        if reservation is not None:
            reservation.close()
        if package is not None and before is not None:
            try:
                final = inspect_inventory(package)
                report["source_stable"] = final["sha256"] == before["sha256"]
                report["input_after_sha256"] = final["sha256"]
                _need(report["source_stable"], "Original payload changed during runtime validation")
            except Exception as error:
                report["errors"].append(str(error))
        if copy is not None and copied is not None:
            try:
                changes = _copy_changes(copied, inspect_inventory(copy))
                report["runtime_copy_changes"] = changes
                report["runtime_copy_stable"] = changes["passed"]
                _need(changes["passed"], "Runtime copy changed existing inputs or added unapproved paths")
            except Exception as error:
                report["errors"].append(str(error))
        if game is not None and game_before is not None:
            try:
                changes = _copy_changes(game_before, inspect_inventory(game))
                report["created_game_copy_changes"] = changes
                report["created_game_copy_stable"] = changes["passed"]
                _need(changes["passed"], "Created game changed existing build files or added unapproved paths")
            except Exception as error:
                report["errors"].append(str(error))
        commands = [command for stage in report["stages"] for command in stage["commands"]]
        if commands:
            report["cleanup"] = "COMPLETE" if all((command.get("run") or {}).get("owned_tree_cleanup") == "COMPLETE" for command in commands) else "FAILED"
            evidence_errors = _verify_evidence(attempt, report["stages"])
            report["errors"].extend(evidence_errors)
            report["evidence_stable"] = not evidence_errors
        if not report["errors"] and len(report["stages"]) == 5 and all(stage["status"] == "PASS" for stage in report["stages"]):
            report.update(status="RUNTIME_PASS", runtime="PASS")
        elif report["runtime"] != "NOT_RUN":
            report["runtime"] = "FAIL"
        if attempt is not None:
            with (attempt / "native-runtime.json").open("x", encoding="utf-8") as stream:
                json.dump(report, stream, ensure_ascii=False, indent=2)
                stream.write("\n")
    return report
