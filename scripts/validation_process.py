"""Run a validation command and finish its owned process tree before returning."""
from __future__ import annotations

import os
import math
from pathlib import Path
import signal
import subprocess
import sys
import time
from typing import BinaryIO, Sequence


class _WindowsJob:
    """A non-inheritable job handle owned exclusively by the validation parent."""

    def __init__(self):
        import ctypes
        from ctypes import wintypes

        class BasicLimits(ctypes.Structure):
            _fields_ = [
                ("PerProcessUserTimeLimit", ctypes.c_longlong),
                ("PerJobUserTimeLimit", ctypes.c_longlong),
                ("LimitFlags", wintypes.DWORD),
                ("MinimumWorkingSetSize", ctypes.c_size_t),
                ("MaximumWorkingSetSize", ctypes.c_size_t),
                ("ActiveProcessLimit", wintypes.DWORD),
                ("Affinity", ctypes.c_size_t),
                ("PriorityClass", wintypes.DWORD),
                ("SchedulingClass", wintypes.DWORD),
            ]

        class IoCounters(ctypes.Structure):
            _fields_ = [(name, ctypes.c_ulonglong) for name in (
                "ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
                "ReadTransferCount", "WriteTransferCount", "OtherTransferCount",
            )]

        class ExtendedLimits(ctypes.Structure):
            _fields_ = [
                ("BasicLimitInformation", BasicLimits),
                ("IoInfo", IoCounters),
                ("ProcessMemoryLimit", ctypes.c_size_t),
                ("JobMemoryLimit", ctypes.c_size_t),
                ("PeakProcessMemoryUsed", ctypes.c_size_t),
                ("PeakJobMemoryUsed", ctypes.c_size_t),
            ]

        class Accounting(ctypes.Structure):
            _fields_ = [
                ("TotalUserTime", ctypes.c_longlong),
                ("TotalKernelTime", ctypes.c_longlong),
                ("ThisPeriodTotalUserTime", ctypes.c_longlong),
                ("ThisPeriodTotalKernelTime", ctypes.c_longlong),
                ("TotalPageFaultCount", wintypes.DWORD),
                ("TotalProcesses", wintypes.DWORD),
                ("ActiveProcesses", wintypes.DWORD),
                ("TotalTerminatedProcesses", wintypes.DWORD),
            ]

        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
        kernel.CreateJobObjectW.restype = wintypes.HANDLE
        kernel.SetInformationJobObject.argtypes = [
            wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD,
        ]
        kernel.SetInformationJobObject.restype = wintypes.BOOL
        kernel.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
        kernel.AssignProcessToJobObject.restype = wintypes.BOOL
        kernel.TerminateJobObject.argtypes = [wintypes.HANDLE, wintypes.UINT]
        kernel.TerminateJobObject.restype = wintypes.BOOL
        kernel.QueryInformationJobObject.argtypes = [
            wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD),
        ]
        kernel.QueryInformationJobObject.restype = wintypes.BOOL
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.IsProcessInJob.argtypes = [wintypes.HANDLE, wintypes.HANDLE, ctypes.POINTER(wintypes.BOOL)]
        kernel.IsProcessInJob.restype = wintypes.BOOL
        kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        kernel.WaitForSingleObject.restype = wintypes.DWORD
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel.CloseHandle.restype = wintypes.BOOL
        self._ctypes, self._kernel, self._accounting = ctypes, kernel, Accounting
        self._wintypes = wintypes
        self._handle = kernel.CreateJobObjectW(None, None)
        if not self._handle:
            raise ctypes.WinError(ctypes.get_last_error())
        limits = ExtendedLimits()
        self._limits = limits
        limits.BasicLimitInformation.LimitFlags = 0x00002000  # KILL_ON_JOB_CLOSE
        if not kernel.SetInformationJobObject(
            self._handle, 9, ctypes.byref(limits), ctypes.sizeof(limits)
        ):
            error = ctypes.WinError(ctypes.get_last_error())
            self.close()
            raise error

    def assign(self, process: subprocess.Popen) -> None:
        # CPython retains the exact process HANDLE, avoiding a PID lookup that
        # could accidentally select a reused PID if the launcher exits early.
        if not self._kernel.AssignProcessToJobObject(self._handle, int(process._handle)):
            raise self._ctypes.WinError(self._ctypes.get_last_error())

    def _stop_new_processes(self) -> None:
        # Seal membership before taking the final handle snapshot. Setting the
        # limit does not terminate current members; a later process association
        # fails before that process can execute. Keep KILL_ON_JOB_CLOSE enabled.
        self._limits.BasicLimitInformation.LimitFlags |= 0x00000008
        self._limits.BasicLimitInformation.ActiveProcessLimit = 0
        if not self._kernel.SetInformationJobObject(
            self._handle, 9, self._ctypes.byref(self._limits), self._ctypes.sizeof(self._limits)
        ):
            raise self._ctypes.WinError(self._ctypes.get_last_error())

    def _process_ids(self) -> list[int]:
        # JobObjectBasicProcessIdList includes nested child jobs. Grow from a
        # small buffer using the kernel's assigned count, never a machine scan.
        capacity = 16
        while True:
            class ProcessIds(self._ctypes.Structure):
                _fields_ = [("assigned", self._wintypes.DWORD),
                            ("listed", self._wintypes.DWORD),
                            ("ids", self._ctypes.c_size_t * capacity)]
            info = ProcessIds()
            ok = self._kernel.QueryInformationJobObject(
                self._handle, 3, self._ctypes.byref(info), self._ctypes.sizeof(info), None
            )
            if not ok and self._ctypes.get_last_error() != 234:  # ERROR_MORE_DATA
                raise self._ctypes.WinError(self._ctypes.get_last_error())
            if ok and info.listed == info.assigned and info.listed <= capacity:
                return list(info.ids[:info.listed])
            capacity = max(capacity * 2, info.assigned)
            if capacity > 1_000_000:
                raise RuntimeError("Owned Windows job process list exceeds the bounded snapshot")

    def _retain_processes(self, handles: list[int]) -> None:
        for pid in self._process_ids():
            handle = self._kernel.OpenProcess(0x00101000, False, pid)
            if not handle:
                error = self._ctypes.get_last_error()
                if error == 87:  # ERROR_INVALID_PARAMETER: process already destroyed
                    continue
                raise self._ctypes.WinError(error)
            try:
                belongs = self._wintypes.BOOL()
                if not self._kernel.IsProcessInJob(handle, self._handle, self._ctypes.byref(belongs)):
                    raise self._ctypes.WinError(self._ctypes.get_last_error())
                # A PID may disappear/reuse between query and OpenProcess.
                # Never wait on or terminate an unrelated replacement.
                if belongs.value:
                    handles.append(handle)
                    handle = None
            finally:
                if handle:
                    self._kernel.CloseHandle(handle)

    def terminate_and_wait(self) -> None:
        deadline = time.monotonic() + 10
        handles = []
        try:
            try:
                self._stop_new_processes()
                self._retain_processes(handles)
            finally:
                # Enumeration/ownership errors must still terminate this job.
                if not self._kernel.TerminateJobObject(self._handle, 1):
                    raise self._ctypes.WinError(self._ctypes.get_last_error())
                for handle in handles:
                    remaining = max(0, math.ceil((deadline - time.monotonic()) * 1000))
                    status = self._kernel.WaitForSingleObject(handle, remaining)
                    if status == 258:  # WAIT_TIMEOUT
                        raise TimeoutError("Owned Windows process did not signal exit within 10 seconds")
                    if status != 0:  # WAIT_OBJECT_0
                        raise self._ctypes.WinError(self._ctypes.get_last_error())
            info = self._accounting()
            if not self._kernel.QueryInformationJobObject(
                self._handle, 1, self._ctypes.byref(info), self._ctypes.sizeof(info), None
            ):
                raise self._ctypes.WinError(self._ctypes.get_last_error())
            if info.ActiveProcesses != 0:
                raise RuntimeError("Owned Windows job retained active processes after exit waits")
        finally:
            for handle in handles:
                self._kernel.CloseHandle(handle)

    def close(self) -> None:
        if self._handle:
            self._kernel.CloseHandle(self._handle)
            self._handle = None


def _windows_launch_target(argv: list[str]) -> int:
    # A single byte fits in the pipe even before Python startup finishes.
    # The parent sends it only AFTER placing this launcher in its job. The
    # real command inherits that job and can never start in the assign gap.
    if sys.stdin.buffer.read(1) != b"G":
        return 125
    process = subprocess.Popen(argv, stdin=subprocess.DEVNULL, shell=False)
    return process.wait()


def _cleanup_posix_group(process: subprocess.Popen) -> None:
    # Do not return early when the leader has exited: its descendants can
    # still own the process group and the output file descriptors.
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()


def run_owned_command(argv: Sequence[str], cwd: str | Path, stdout: BinaryIO,
                      stderr: BinaryIO, timeout: float) -> int:
    """Return the command's exit code only after its remaining children stop.

    TimeoutExpired names the original argv. KeyboardInterrupt is propagated
    after cleanup. No shell, global process search, or port-based cleanup is
    used. Commands are non-interactive (stdin is DEVNULL).
    """
    command = list(argv)
    if not command or not all(isinstance(arg, str) for arg in command):
        raise ValueError("argv must be a non-empty sequence of strings")
    if isinstance(timeout, bool) or not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("timeout must be positive")
    if os.name == "nt" and Path(command[0]).suffix.lower() in {".cmd", ".bat"}:
        raise ValueError("Use an executable rather than a Windows shell wrapper")

    process = None
    job = _WindowsJob() if os.name == "nt" else None
    started = time.monotonic()
    try:
        if job is not None:
            launcher = [sys.executable, "-I", "-S", str(Path(__file__).resolve()),
                        "--owned-launcher", *command]
            process = subprocess.Popen(
                launcher, cwd=cwd, stdin=subprocess.PIPE, stdout=stdout, stderr=stderr,
                shell=False, creationflags=subprocess.CREATE_NO_WINDOW,
            )
            job.assign(process)
            process.stdin.write(b"G")
            process.stdin.close()
        else:
            process = subprocess.Popen(
                command, cwd=cwd, stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
                shell=False, start_new_session=True,
            )
        try:
            return process.wait(timeout=max(0, timeout - (time.monotonic() - started)))
        except subprocess.TimeoutExpired:
            raise subprocess.TimeoutExpired(command, timeout) from None
    finally:
        try:
            if job is not None:
                try:
                    job.terminate_and_wait()
                finally:
                    # An unassigned launcher cannot have started the actual
                    # command: the handshake byte was never sent. Reap it even
                    # if querying/terminating the job itself reports an error.
                    if process is not None and process.poll() is None:
                        process.kill()
                    if process is not None:
                        process.wait()
            elif process is not None:
                _cleanup_posix_group(process)
        finally:
            if process is not None and process.stdin is not None:
                process.stdin.close()
            if job is not None:
                job.close()


if __name__ == "__main__":
    if os.name != "nt" or len(sys.argv) < 3 or sys.argv[1] != "--owned-launcher":
        raise SystemExit("This module is imported by the validation runner")
    raise SystemExit(_windows_launch_target(sys.argv[2:]))
