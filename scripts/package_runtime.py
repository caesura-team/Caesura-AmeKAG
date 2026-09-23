"""Environment and process provenance for explicit final-package runs.

This is environment sanitization, not a filesystem sandbox. Native programs
must be supplied as absolute paths from the inspected package. Host tools used
by a controller must likewise be passed explicitly. The existing validation
runner owns and reaps the entire process tree; this module never kills by PID,
process name, or port. Socket observations establish readiness at one instant,
not a claim that the process will stay alive afterwards.
"""
from __future__ import annotations

from dataclasses import asdict, dataclass
import ctypes
import ipaddress
import json
import math
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import time
from typing import BinaryIO, Mapping, Sequence


class RuntimeContractError(RuntimeError):
    """The requested package/runtime identity cannot be established."""


@dataclass(frozen=True)
class ProcessIdentity:
    pid: int
    created: str
    executable: str
    source: str


def _directory(path: str | Path) -> Path:
    try:
        resolved = Path(path).resolve(strict=True)
        if not resolved.is_dir():
            raise RuntimeContractError(f"Not a directory: {path}")
        return resolved
    except OSError as error:
        raise RuntimeContractError(f"Cannot resolve directory: {path}") from error


def _executable(path: str | Path) -> Path:
    try:
        given = Path(path)
        if not given.is_absolute():
            raise RuntimeContractError(f"Executable must have an absolute path: {path}")
        resolved = given.resolve(strict=True)
        if not resolved.is_file() or resolved.stat().st_size == 0:
            raise RuntimeContractError(f"Missing or empty executable: {path}")
        return resolved
    except OSError as error:
        raise RuntimeContractError(f"Cannot resolve executable: {path}") from error


def native_env(package: str | Path, *, engine: str | Path, lua: str | Path,
               work: str | Path, home: str | Path, temp: str | Path,
               inherited: Mapping[str, str] | None = None) -> dict[str, str]:
    """Build a fresh native environment with an authoritative packaged Lua.

    Paths must already exist; the caller allocates the private home/temp. Files
    are checked for package containment and home/temp stay outside it; ABI,
    executable permission, byte hashes and loaded modules belong to the static
    inspector/runtime receipt. No developer PATH, language search path, proxy,
    application override, or arbitrary secret is copied into this environment.
    """
    package_path = _directory(package)
    binaries = [_executable(engine), _executable(lua)]
    if any(not path.is_relative_to(package_path) for path in binaries):
        raise RuntimeContractError("Engine and Lua must resolve inside this package")
    work_path, home_path, temp_path = map(_directory, (work, home, temp))
    if any(path.is_relative_to(package_path) for path in (home_path, temp_path)):
        raise RuntimeContractError("Private home and temp must stay outside the inspected package")
    inherited = os.environ if inherited is None else inherited
    allowed = {"DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY", "XDG_RUNTIME_DIR",
               "DBUS_SESSION_BUS_ADDRESS", "LANG", "LANGUAGE", "LC_ALL", "LC_CTYPE",
               "LC_MESSAGES", "TZ", "TERM", "COLORTERM"}
    result = {key: value for key, value in inherited.items() if key in allowed}
    path_parts = list(dict.fromkeys(str(path) for path in
                                   (binaries[0].parent, binaries[1].parent, package_path)))
    if os.name == "nt":
        buffer = ctypes.create_unicode_buffer(32768)
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.GetWindowsDirectoryW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint]
        kernel.GetWindowsDirectoryW.restype = ctypes.c_uint
        length = kernel.GetWindowsDirectoryW(buffer, len(buffer))
        if not 0 < length < len(buffer):
            raise RuntimeContractError("Cannot establish the OS Windows directory")
        windows = Path(buffer.value)
        system = windows / "System32"
        path_parts.append(str(system))
        result.update(SystemRoot=str(windows), WINDIR=str(windows),
                      ComSpec=str(system / "cmd.exe"),
                      # Drivers also resolve shared data through these names;
                      # absence can turn their log path into a CWD-relative one.
                      ProgramData=str(home_path), ALLUSERSPROFILE=str(home_path),
                      USERPROFILE=str(home_path), APPDATA=str(home_path / "AppData/Roaming"),
                      LOCALAPPDATA=str(home_path / "AppData/Local"))
    else:
        # System utilities are explicit OS locations, never the developer PATH.
        path_parts.extend(("/usr/bin", "/bin"))
    result.update(PATH=os.pathsep.join(path_parts), CAESURA_LUA=str(binaries[1]),
                  HOME=str(home_path), TMP=str(temp_path), TEMP=str(temp_path),
                  TMPDIR=str(temp_path), PWD=str(work_path),
                  XDG_CONFIG_HOME=str(home_path / ".config"),
                  XDG_CACHE_HOME=str(home_path / ".cache"),
                  XDG_DATA_HOME=str(home_path / ".local/share"),
                  XDG_STATE_HOME=str(home_path / ".local/state"))
    return result


def _windows_identity(pid: int) -> ProcessIdentity:
    from ctypes import wintypes
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    kernel.GetProcessTimes.restype = wintypes.BOOL
    kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD,
                                                wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    kernel.QueryFullProcessImageNameW.restype = wintypes.BOOL
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.WaitForSingleObject.restype = wintypes.DWORD
    handle = kernel.OpenProcess(0x1000 | 0x00100000, False, pid)
    if not handle:
        raise RuntimeContractError(f"Cannot open live process {pid}: {ctypes.get_last_error()}")
    try:
        if kernel.WaitForSingleObject(handle, 0) != 258:  # WAIT_TIMEOUT = still running
            raise RuntimeContractError(f"Process {pid} has exited")
        created, exited, cpu_kernel, cpu_user = (wintypes.FILETIME() for _ in range(4))
        if not kernel.GetProcessTimes(handle, ctypes.byref(created), ctypes.byref(exited),
                                      ctypes.byref(cpu_kernel), ctypes.byref(cpu_user)):
            raise RuntimeContractError(f"Cannot read creation time for process {pid}")
        buffer = ctypes.create_unicode_buffer(32768)
        length = wintypes.DWORD(len(buffer))
        if not kernel.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(length)):
            raise RuntimeContractError(f"Cannot read executable for process {pid}")
        if kernel.WaitForSingleObject(handle, 0) != 258:
            raise RuntimeContractError(f"Process {pid} exited during identity observation")
        stamp = (created.dwHighDateTime << 32) | created.dwLowDateTime
        return ProcessIdentity(pid, str(stamp), str(Path(buffer.value).resolve()),
                               "windows:GetProcessTimes/QueryFullProcessImageNameW")
    finally:
        kernel.CloseHandle(handle)


def _linux_identity(pid: int) -> ProcessIdentity:
    directory = Path("/proc") / str(pid)
    def stat_identity():
        # comm can contain spaces and parentheses; fields after the final ')' start at state.
        raw = (directory / "stat").read_text()
        fields = raw[raw.rindex(")") + 2:].split()
        if fields[0] in {"Z", "X", "x"}:
            raise RuntimeContractError(f"Process {pid} has exited")
        return fields[19]
    before = stat_identity()
    executable = str((directory / "exe").resolve(strict=True))
    after = stat_identity()
    if before != after:
        raise RuntimeContractError(f"Process {pid} changed during identity observation")
    boot = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
    return ProcessIdentity(pid, f"{boot}:{before}", executable, "linux:procfs/boot-id/starttime")


def _macos_identity(pid: int) -> ProcessIdentity:
    # Layout from Apple's xnu bsd/sys/proc_info.h, PROC_PIDTBSDINFO.
    class BsdInfo(ctypes.Structure):
        _fields_ = [(name, ctypes.c_uint32) for name in (
            "flags", "status", "xstatus", "pid", "ppid", "uid", "gid", "ruid",
            "rgid", "svuid", "svgid", "reserved")] + [
                ("comm", ctypes.c_char * 16), ("name", ctypes.c_char * 32)] + [
                (name, ctypes.c_uint32) for name in
                ("nfiles", "pgid", "jobc", "tdev", "tpgid")] + [
                ("nice", ctypes.c_int32), ("start_sec", ctypes.c_uint64),
                ("start_usec", ctypes.c_uint64)]
    library = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
    library.proc_pidinfo.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_uint64,
                                     ctypes.c_void_p, ctypes.c_int]
    library.proc_pidinfo.restype = ctypes.c_int
    library.proc_pidpath.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
    library.proc_pidpath.restype = ctypes.c_int
    def read_info():
        info = BsdInfo()
        size = library.proc_pidinfo(pid, 3, 0, ctypes.byref(info), ctypes.sizeof(info))
        if size != ctypes.sizeof(info) or info.pid != pid or info.status == 5:  # SZOMB
            raise RuntimeContractError(f"Cannot read live macOS process {pid}")
        return info.start_sec, info.start_usec
    before = read_info()
    buffer = ctypes.create_string_buffer(4096)
    if library.proc_pidpath(pid, buffer, len(buffer)) <= 0:
        raise RuntimeContractError(f"Cannot read executable for process {pid}")
    if before != read_info():
        raise RuntimeContractError(f"Process {pid} changed during identity observation")
    return ProcessIdentity(pid, f"{before[0]}:{before[1]:06d}",
                           str(Path(os.fsdecode(buffer.value)).resolve(strict=True)),
                           "macos:libproc/PROC_PIDTBSDINFO/proc_pidpath")


def process_identity(pid: int) -> ProcessIdentity:
    """Observe a live PID with OS creation precision; an exited PID is an error."""
    if isinstance(pid, bool) or not isinstance(pid, int) or pid <= 0:
        raise RuntimeContractError("PID must be a positive integer")
    try:
        if os.name == "nt":
            return _windows_identity(pid)
        if sys.platform.startswith("linux"):
            return _linux_identity(pid)
        if sys.platform == "darwin":
            return _macos_identity(pid)
        raise RuntimeContractError(f"Unsupported process identity platform: {sys.platform}")
    except (OSError, ValueError, IndexError) as error:
        raise RuntimeContractError(f"Cannot establish live process identity for {pid}: {error}") from error


def _reaches_loopback(address: str) -> bool:
    value = ipaddress.ip_address(address.split("%", 1)[0])
    return value.is_loopback or value.is_unspecified or (
        isinstance(value, ipaddress.IPv6Address) and value.ipv4_mapped is not None
        and value.ipv4_mapped.is_loopback)


def _windows_listeners(port: int) -> list[dict]:
    from ctypes import wintypes
    class Tcp4(ctypes.Structure):
        _fields_ = [(name, wintypes.DWORD) for name in
                    ("state", "local", "port", "remote", "remote_port", "pid")]
    class Tcp6(ctypes.Structure):
        _fields_ = [("local", ctypes.c_ubyte * 16), ("scope", wintypes.DWORD),
                    ("port", wintypes.DWORD), ("remote", ctypes.c_ubyte * 16),
                    ("remote_scope", wintypes.DWORD), ("remote_port", wintypes.DWORD),
                    ("state", wintypes.DWORD), ("pid", wintypes.DWORD)]
    library = ctypes.WinDLL("iphlpapi", use_last_error=True)
    library.GetExtendedTcpTable.argtypes = [ctypes.c_void_p, ctypes.POINTER(wintypes.DWORD),
                                            wintypes.BOOL, wintypes.ULONG, ctypes.c_int,
                                            wintypes.ULONG]
    library.GetExtendedTcpTable.restype = wintypes.DWORD
    rows = []
    for family, row_type in ((socket.AF_INET, Tcp4), (socket.AF_INET6, Tcp6)):
        size = wintypes.DWORD(0)
        result = library.GetExtendedTcpTable(None, ctypes.byref(size), False, family, 3, 0)
        if result not in (0, 122):  # ERROR_INSUFFICIENT_BUFFER
            raise RuntimeContractError(f"Cannot inspect TCP table: Windows error {result}")
        for _ in range(4):
            buffer = ctypes.create_string_buffer(size.value)
            result = library.GetExtendedTcpTable(buffer, ctypes.byref(size), False, family, 3, 0)
            if result != 122:
                break
        if result:
            raise RuntimeContractError(f"Cannot inspect TCP table: Windows error {result}")
        count = wintypes.DWORD.from_buffer_copy(buffer.raw[:4]).value
        if 4 + count * ctypes.sizeof(row_type) > len(buffer):
            raise RuntimeContractError("Invalid Windows TCP table length")
        for index in range(count):
            row = row_type.from_buffer_copy(buffer, 4 + index * ctypes.sizeof(row_type))
            observed_port = socket.ntohs(row.port & 0xffff)
            address = socket.inet_ntop(family, struct.pack("<I", row.local)
                                      if family == socket.AF_INET else bytes(row.local))
            if row.state == 2 and observed_port == port and _reaches_loopback(address):
                rows.append(dict(pid=row.pid, address=address, port=port,
                                 family="ipv4" if family == socket.AF_INET else "ipv6"))
    return rows


def _linux_listeners(port: int) -> list[dict]:
    rows = []
    for name, family in (("tcp", socket.AF_INET), ("tcp6", socket.AF_INET6)):
        table = Path("/proc/net") / name
        if name == "tcp6" and not table.exists():
            continue
        for line in table.read_text().splitlines()[1:]:
            fields = line.split()
            address_hex, port_hex = fields[1].split(":")
            if fields[3] != "0A" or int(port_hex, 16) != port:
                continue
            chunks = [address_hex[index:index + 8] for index in range(0, len(address_hex), 8)]
            raw = b"".join(int(chunk, 16).to_bytes(4, sys.byteorder) for chunk in chunks)
            address = socket.inet_ntop(family, raw)
            if _reaches_loopback(address):
                rows.append(dict(pid=None, address=address, port=port,
                                 family="ipv4" if family == socket.AF_INET else "ipv6",
                                 inode=fields[9]))
    owners = {row["inode"]: set() for row in rows}
    if not owners:
        return []
    for directory in Path("/proc").iterdir():
        if not directory.name.isdigit():
            continue
        try:
            for fd in (directory / "fd").iterdir():
                try:
                    target = os.readlink(fd)
                except OSError:
                    continue
                if target.startswith("socket:[") and target[8:-1] in owners:
                    owners[target[8:-1]].add(int(directory.name))
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue
    result = []
    for row in rows:
        inode = row.pop("inode")
        for pid in sorted(owners[inode]) or [None]:
            result.append(dict(row, pid=pid))
    return result


def _macos_listeners(port: int) -> list[dict]:
    command = ["/usr/sbin/lsof", "-nP", "-a", f"-iTCP:{port}", "-sTCP:LISTEN", "-Fpcnt"]
    observed = subprocess.run(command, capture_output=True, text=True, timeout=10,
                              env={"PATH": "/usr/bin:/bin:/usr/sbin", "LC_ALL": "C"})
    if observed.returncode == 1 and not observed.stdout and not observed.stderr:
        return []
    if observed.returncode != 0 or observed.stderr:
        raise RuntimeContractError(f"Cannot establish macOS TCP owners: {observed.stderr.strip()}")
    rows, pid, family = [], None, None
    for field in observed.stdout.splitlines():
        if field.startswith("p"):
            pid = int(field[1:])
        elif field.startswith("t"):
            family = field[1:].lower()
        elif field.startswith("n"):
            address, observed_port = field[1:].rsplit(":", 1)
            address = address.strip("[]")
            if address == "*":
                if family not in {"ipv4", "ipv6"}:
                    raise RuntimeContractError("Wildcard TCP listener lacks an address family")
                address = "::" if family == "ipv6" else "0.0.0.0"
            if int(observed_port) == port and _reaches_loopback(address):
                rows.append(dict(pid=pid, address=address, port=port,
                                 family="ipv6" if ":" in address else "ipv4"))
    return rows


def loopback_listeners(port: int) -> list[dict]:
    """All TCP listeners reaching loopback, including wildcard and IPv6 binds.

    A Linux row whose owner is inaccessible has pid=None, which readiness will
    reject. Ownership uncertainty never becomes an empty-port assertion.
    """
    if isinstance(port, bool) or not isinstance(port, int) or not 1 <= port <= 65535:
        raise RuntimeContractError("Port must be an integer in 1..65535")
    try:
        if os.name == "nt":
            return _windows_listeners(port)
        if sys.platform.startswith("linux"):
            return _linux_listeners(port)
        if sys.platform == "darwin":
            return _macos_listeners(port)
        raise RuntimeContractError(f"Unsupported listener platform: {sys.platform}")
    except (OSError, ValueError, IndexError, subprocess.SubprocessError) as error:
        raise RuntimeContractError(f"Cannot inspect TCP port {port}: {error}") from error


def verify_owned_listener(identity: ProcessIdentity, port: int) -> list[dict]:
    """Refuse wrong/reused/exited owners before and after inspecting all rows."""
    if not isinstance(identity, ProcessIdentity) or process_identity(identity.pid) != identity:
        raise RuntimeContractError("Process identity no longer matches the recorded owner")
    rows = loopback_listeners(port)
    if not rows or any(row["pid"] != identity.pid for row in rows):
        raise RuntimeContractError(f"TCP port {port} is not exclusively owned by process {identity.pid}")
    # In particular, Linux obtains the socket table and fd owners separately.
    # A second complete observation must agree, rather than accepting a socket
    # that disappeared/rebound while its owner was being enumerated.
    observed_again = loopback_listeners(port)
    canonical = lambda observed: sorted((row["family"], row["address"], row["port"],
                                         str(row["pid"])) for row in observed)
    if canonical(rows) != canonical(observed_again):
        raise RuntimeContractError(f"TCP port {port} changed during ownership observation")
    if process_identity(identity.pid) != identity:
        raise RuntimeContractError("Process changed while its listener was being observed")
    return rows


def _write_json(path: Path, value: dict) -> None:
    # Publish complete JSON for a concurrent controller. These paths belong to
    # a newly created control directory; previous attempts are never replaced.
    temporary = path.with_name("." + path.name + ".writing")
    with temporary.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    if path.exists() or path.is_symlink():
        raise RuntimeContractError(f"Refusing existing control result: {path}")
    os.replace(temporary, path)


def _control_path(control: Path, path: str | Path | None, default: str | None) -> Path | None:
    if path is None:
        return control / default if default is not None else None
    candidate = Path(path)
    if not candidate.is_absolute():
        candidate = control / candidate
    candidate = candidate.resolve(strict=False)
    if candidate == control or not candidate.is_relative_to(control):
        raise RuntimeContractError(f"Control file must be inside the fresh control directory: {path}")
    return candidate


def _exec_file_identity(path: Path) -> dict:
    from package_verification import _sha256_file
    return {"path": str(path), "sha256": _sha256_file(path)}


def _exec_contract(command, expected, observation_timeout, timeout):
    launch = Path(command[0])
    controller = None
    if expected is None:
        if sys.platform != "darwin" or launch != _executable(sys.executable):
            return None
        # CPython's macOS framework bin/python stub uses POSIX_SPAWN_SETEXEC
        # to enter Python.app, while sys.executable deliberately names the
        # stub. Bind only our exact current entry point to the OS-observed
        # image of this controller. Never infer another interpreter from a
        # basename, framework layout, PATH, or a child's self-report.
        controller = process_identity(os.getpid())
        final = _executable(controller.executable)
        if final == launch:
            return None
    if not (sys.platform.startswith("linux") or sys.platform == "darwin"):
        raise RuntimeContractError("Final executable mapping requires an actual POSIX host")
    if (isinstance(observation_timeout, bool) or not math.isfinite(observation_timeout)
            or observation_timeout <= 0):
        raise RuntimeContractError("Exec observation timeout must be positive and finite")
    before = _exec_file_identity(launch)
    if controller is None:
        final = _executable(expected)
        with launch.open("rb") as stream:
            line = stream.readline(4096)
        match = re.fullmatch(rb"#![ \t]*(/[^\s]+)[ \t]*\r?\n", line)
        if not match:
            raise RuntimeContractError("Exec mapping requires an explicit single shebang interpreter without dispatcher arguments")
        interpreter = _executable(os.fsdecode(match[1]))
        if final in (launch, interpreter):
            raise RuntimeContractError("The final executable must differ from its launch script and interpreter")
    else:
        interpreter = launch
    contract = {"launcher": before, "interpreter": _exec_file_identity(interpreter),
                "final_executable": _exec_file_identity(final),
                "observation_timeout": min(observation_timeout, timeout)}
    if controller is not None:
        contract.update(kind="macos-current-python", controller_process=asdict(controller))
    _check_exec_files(contract)
    return contract


def _check_exec_files(contract):
    if contract is None:
        return
    for role in ("launcher", "interpreter", "final_executable"):
        if _exec_file_identity(Path(contract[role]["path"])) != contract[role]:
            raise RuntimeContractError(f"Exec {role} changed after dispatch")


def _wait_for_final_executable(process, contract, transition):
    deadline = time.monotonic() + contract["observation_timeout"]
    creation, previous_final = None, None
    expected = contract["final_executable"]["path"]
    interpreter = contract["interpreter"]["path"]
    transition.update(child_pid=process.pid, observations=[], status="OBSERVING")
    while time.monotonic() < deadline:
        if process.poll() is not None:
            if contract.get("kind") == "macos-current-python":
                transition["status"] = "EXITED_BEFORE_VERIFICATION"
                return None
            raise RuntimeContractError("Launch child exited before its final executable was observed")
        try:
            identity = process_identity(process.pid)
        except RuntimeContractError as error:
            transition["last_observation_error"] = str(error)
            time.sleep(0.01)
            continue
        if process.poll() is not None:
            if contract.get("kind") == "macos-current-python":
                transition["status"] = "EXITED_BEFORE_VERIFICATION"
                return None
            raise RuntimeContractError("Launch child exited during final executable observation")
        observed_creation = (identity.pid, identity.created, identity.source)
        if creation is None:
            creation = observed_creation
        if observed_creation != creation:
            raise RuntimeContractError("Launch child PID/creation identity changed before exec")
        if len(transition["observations"]) < 32 and (not transition["observations"] or
                transition["observations"][-1] != asdict(identity)):
            transition["observations"].append(asdict(identity))
        if identity.executable not in (interpreter, expected):
            raise RuntimeContractError("Launch child mapped an undeclared executable")
        if identity.executable == expected:
            if previous_final == identity:
                transition.update(status="VERIFIED", final_process=asdict(identity),
                    interpreter_observed=any(item["executable"] == interpreter for item in transition["observations"]))
                return identity
            previous_final = identity
        else:
            previous_final = None
        time.sleep(0.01)
    raise RuntimeContractError("Final executable was not observed within the explicit exec deadline")


def _runtime_launcher(request_path: Path) -> int:
    request = json.loads(request_path.read_text(encoding="utf-8"))
    result_path = Path(request["result_path"])
    identity_path = Path(request["identity_path"])
    stop_path = Path(request["stop_request"]) if request["stop_request"] else None
    result = dict(process=None, actual_exit_code=None, stop_requested=False,
                  forced_kill=False, status="LAUNCH_FAILED")
    contract = request.get("exec_contract")
    if contract is not None:
        result["exec_transition"] = {"status": "NOT_OBSERVED", "contract": contract,
                                     "inputs_stable": False}
    process = None
    try:
        _check_exec_files(contract)
        flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
        process = subprocess.Popen(request["argv"], cwd=request["cwd"], env=request["env"],
                                   stdin=subprocess.DEVNULL, shell=False, creationflags=flags)
        try:
            if contract is not None:
                identity = _wait_for_final_executable(process, contract, result["exec_transition"])
            else:
                identity = process_identity(process.pid)
                if Path(identity.executable) != Path(request["argv"][0]):
                    raise RuntimeContractError("Started process executable differs from explicit command")
            if identity is not None:
                result["process"] = asdict(identity)
                _write_json(identity_path, asdict(identity))
            else:
                # As for a fast directly launched command, retain the real
                # exit but do not invent a live process identity/readiness.
                result["identity_error"] = "Python exited before its final image could be verified"
        except RuntimeContractError as error:
            # A command may legitimately finish before observation. Its exit
            # remains useful, but no process identity/readiness is invented.
            result["identity_error"] = str(error)
            if contract is not None or process.poll() is None:
                raise
        while process.poll() is None:
            if contract is not None:
                try:
                    current = process_identity(process.pid)
                except RuntimeContractError as error:
                    # The executable mapping can disappear before poll sees
                    # the child exit. Only a wait on this retained Popen child
                    # can prove exit; query failure alone is never proof.
                    try:
                        exit_code = process.wait(timeout=1)
                    except subprocess.TimeoutExpired:
                        raise error
                    result["exit_observation"] = dict(source="retained-child-wait",
                        identity_error=str(error), wait_exit_code=exit_code)
                    break
                if current != identity:
                    raise RuntimeContractError("Verified final executable changed while the owned child was running")
            if stop_path is not None and (stop_path.exists() or stop_path.is_symlink()):
                if stop_path.is_symlink() or not stop_path.is_file():
                    raise RuntimeContractError("Stop request must be a regular control file")
                result["stop_requested"] = True
                process.terminate()  # retained Popen HANDLE/child, never a port or searched PID
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    result["forced_kill"] = True
                    process.kill()
                    process.wait()
                break
            time.sleep(0.02)
        result["actual_exit_code"] = process.wait()
        result["status"] = "STOPPED" if result["stop_requested"] else "EXITED"
        return 0
    except Exception as error:
        result["error"] = f"{type(error).__name__}: {error}"
        return 125
    finally:
        if process is not None:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    result["forced_kill"] = True
                    process.kill()
                    process.wait()
            result["actual_exit_code"] = process.wait()
        if contract is not None:
            try:
                _check_exec_files(contract)
                result["exec_transition"]["inputs_stable"] = True
            except Exception as error:
                result["status"] = "LAUNCH_FAILED"
                result["error"] = (result.get("error", "") + "; " + str(error)).lstrip("; ")
        _write_json(result_path, result)


def run_runtime_command(argv: Sequence[str], cwd: str | Path, env: Mapping[str, str],
                        control_dir: str | Path, stdout: BinaryIO, stderr: BinaryIO,
                        timeout: float, *, identity_path: str | Path | None = None,
                        stop_request: str | Path | None = None,
                        expected_final_executable: str | Path | None = None,
                        exec_observation_timeout: float = 5) -> dict:
    """Run with explicit env under the existing owned-tree runner.

    control_dir must not exist, and its parent must exist. stdout/stderr are
    open binary streams. process.json (or identity_path) is atomically published
    for a concurrent monitor; a monitor may create stop_request after verifying
    readiness. run.json is durable after all owned processes are reaped.

    A controlled stop returns STOPPED with the actual termination exit code,
    never a fabricated zero. TimeoutExpired is preserved after a TIMED_OUT
    run.json is written. A failed launcher raises RuntimeContractError. The
    stop grace is at most two seconds and still subject to the total timeout.
    POSIX launch scripts may explicitly name exactly one expected final executable.
    That mode locks the script, plain shebang interpreter and final executable,
    observes the same retained child's creation identity across exec, and publishes
    process.json only after two matching observations of the final image. It never
    follows a spawned child PID or accepts an unobserved early zero exit.
    The current macOS Python entry point also locks its controller's OS image
    and waits across the framework stub's exec before publishing readiness.
    If that Python command exits too early, only its exit is retained; no live
    identity or verified exec transition is claimed.
    """
    from validation_process import run_owned_command
    command = list(argv)
    if not command or not all(isinstance(arg, str) and "\0" not in arg for arg in command):
        raise RuntimeContractError("argv must contain literal strings without NUL")
    command[0] = str(_executable(command[0]))
    if os.name == "nt" and Path(command[0]).suffix.lower() in {".bat", ".cmd"}:
        raise RuntimeContractError("Windows shell wrappers cannot be runtime executables")
    if isinstance(timeout, bool) or not math.isfinite(timeout) or timeout <= 0:
        raise RuntimeContractError("Timeout must be positive and finite")
    contract = _exec_contract(command, expected_final_executable, exec_observation_timeout, timeout)
    environment = dict(env)
    if not all(isinstance(key, str) and key and "=" not in key and "\0" not in key
               and isinstance(value, str) and "\0" not in value
               for key, value in environment.items()):
        raise RuntimeContractError("Environment must contain valid string keys and values")
    cwd_path = _directory(cwd)
    given_control = Path(control_dir).absolute()
    control = _directory(given_control.parent) / given_control.name
    if control.exists() or control.is_symlink():
        raise RuntimeContractError(f"Control directory must be new: {control}")
    identity_file = _control_path(control, identity_path, "process.json")
    stop_file = _control_path(control, stop_request, None)
    # Internal atomic-publication files also occupy paths. Reject ancestor and
    # temporary-file collisions before creating the control directory or child.
    layout = [control / name for name in ("request.json", "result.json", "run.json")]
    layout.extend(path.with_name("." + path.name + ".writing") for path in tuple(layout))
    layout.extend((identity_file, identity_file.with_name("." + identity_file.name + ".writing")))
    if stop_file is not None:
        layout.append(stop_file)
    for index, path in enumerate(layout):
        if any(path.is_relative_to(other) or other.is_relative_to(path) for other in layout[:index]):
            raise RuntimeContractError("Identity, stop and internal control paths must not overlap")
    control.mkdir()
    for path in (identity_file, stop_file):
        if path is not None:
            path.parent.mkdir(parents=True, exist_ok=True)
    request = dict(argv=command, cwd=str(cwd_path), env=environment,
                   identity_path=str(identity_file), result_path=str(control / "result.json"),
                   stop_request=str(stop_file) if stop_file else None, exec_contract=contract)
    request_file = control / "request.json"
    _write_json(request_file, request)
    report = dict(schema_version=1, argv=command, cwd=str(cwd_path), control_dir=str(control),
                  status="LAUNCH_FAILED", actual_exit_code=None, process=None,
                  stop_requested=False, forced_kill=False, timed_out=False,
                  owned_tree_cleanup="NOT_COMPLETED")
    try:
        launch = [sys.executable, "-I", "-S", str(Path(__file__).resolve()),
                  "--runtime-launcher", str(request_file)]
        report["launcher_exit_code"] = run_owned_command(launch, cwd_path, stdout, stderr, timeout)
        report["owned_tree_cleanup"] = "COMPLETE"
        result_file = control / "result.json"
        if not result_file.is_file():
            raise RuntimeContractError("Runtime launcher exited without a completion receipt")
        report.update(json.loads(result_file.read_text(encoding="utf-8")))
        if report["launcher_exit_code"] != 0 or report["status"] == "LAUNCH_FAILED":
            raise RuntimeContractError(report.get("error", "Runtime launcher failed"))
        return report
    except subprocess.TimeoutExpired:
        report.update(status="TIMED_OUT", timed_out=True, owned_tree_cleanup="COMPLETE",
                      forced_kill=True)
        result_file = control / "result.json"
        if result_file.is_file():
            completed = json.loads(result_file.read_text(encoding="utf-8"))
            report.update({key: completed[key] for key in
                           ("process", "actual_exit_code", "stop_requested")})
        raise subprocess.TimeoutExpired(command, timeout) from None
    except BaseException as error:
        report["error"] = f"{type(error).__name__}: {error}"
        raise
    finally:
        if report["process"] is None and identity_file.is_file():
            report["process"] = json.loads(identity_file.read_text(encoding="utf-8"))
        _write_json(control / "run.json", report)


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] != "--runtime-launcher":
        raise SystemExit("Import this module from the package validation controller")
    # The owned launcher is isolated with -I -S. Only its maintained sibling
    # hash helper is added, never an inherited Python import path.
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    raise SystemExit(_runtime_launcher(Path(sys.argv[2])))
