"""Lifecycle checks for processes owned by one validation invocation."""
from __future__ import annotations

from pathlib import Path
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from validation_process import run_owned_command
import validation_process


class ValidationProcessTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="caesura-owned-process-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.stdout_path = self.root / "stdout.log"
        self.stderr_path = self.root / "stderr.log"
        self.ready = self.root / "child-ready"
        self.release = self.root / "release-child"
        self.escaped = self.root / "escaped-child"

    def invoke(self, argv, timeout=5):
        with self.stdout_path.open("wb") as out, self.stderr_path.open("wb") as err:
            return run_owned_command(argv, self.root, out, err, timeout)

    def parent_with_child(self, *, stay_alive=False, exit_code=0):
        # The child waits for a signal created only AFTER the API returns, so
        # the lifecycle assertion does not depend on a guessed startup delay.
        child = (
            "from pathlib import Path; import time; "
            f"ready=Path({str(self.ready)!r}); release=Path({str(self.release)!r}); "
            f"escaped=Path({str(self.escaped)!r}); "
            "ready.write_text('ready'); deadline=time.monotonic()+8\n"
            "while not release.exists() and time.monotonic()<deadline: time.sleep(0.01)\n"
            "if release.exists():\n"
            "    escaped.write_text('still running')\n"
            "    print('late child stdout', flush=True)\n"
            "    import sys; print('late child stderr', file=sys.stderr, flush=True)\n"
        )
        parent = (
            "from pathlib import Path; import subprocess,sys,time; "
            f"subprocess.Popen([sys.executable, '-c', {child!r}]); "
            f"ready=Path({str(self.ready)!r}); deadline=time.monotonic()+5\n"
            "while not ready.exists() and time.monotonic()<deadline: time.sleep(0.01)\n"
            "assert ready.exists(), 'child never started'\n"
            "print('parent ready', flush=True)\n"
            + ("time.sleep(20)\n" if stay_alive else "")
            + f"sys.exit({exit_code})\n"
        )
        return [sys.executable, "-c", parent]

    def assert_child_gone_and_logs_stable(self):
        self.assertTrue(self.ready.exists(), "the actual descendant must have run")
        stdout_before = self.stdout_path.read_bytes()
        stderr_before = self.stderr_path.read_bytes()
        self.release.write_text("go", encoding="utf-8")
        deadline = time.monotonic() + 0.6
        while time.monotonic() < deadline and not self.escaped.exists():
            time.sleep(0.01)
        self.assertFalse(self.escaped.exists(), "owned descendant survived API return")
        self.assertEqual(self.stdout_path.read_bytes(), stdout_before)
        self.assertEqual(self.stderr_path.read_bytes(), stderr_before)

    def test_preserves_real_exit_status_and_output(self):
        code = "import sys; print('stdout'); print('stderr', file=sys.stderr); sys.exit(23)"
        result = self.invoke([sys.executable, "-c", code])
        self.assertEqual(result, 23)
        self.assertIn(b"stdout", self.stdout_path.read_bytes())
        self.assertIn(b"stderr", self.stderr_path.read_bytes())

    def test_explicit_environment_reaches_owned_target_and_descendant_only(self):
        child = "import json,os; print(json.dumps({'value':os.environ.get('OWNED_ENV_FIXTURE')}))"
        parent = (
            "import json,os,subprocess,sys; "
            f"child=subprocess.run([sys.executable,'-c',{child!r}],capture_output=True,check=True); "
            "print(json.dumps({'target':os.environ.get('OWNED_ENV_FIXTURE'),"
            "'descendant':json.loads(child.stdout)['value']}))"
        )
        before = dict(os.environ)
        environment = dict(os.environ, OWNED_ENV_FIXTURE="literal & $(not a shell) 中文")
        with self.stdout_path.open("wb") as out, self.stderr_path.open("wb") as err:
            result = run_owned_command([sys.executable, "-c", parent], self.root,
                                       out, err, 10, env=environment)
        self.assertEqual(result, 0)
        observed = json.loads(self.stdout_path.read_text(encoding="utf-8"))
        self.assertEqual(observed, {"target": environment["OWNED_ENV_FIXTURE"],
                                    "descendant": environment["OWNED_ENV_FIXTURE"]})
        self.assertEqual(os.environ, before)
        self.assertEqual(environment, dict(before, OWNED_ENV_FIXTURE="literal & $(not a shell) 中文"))

    def test_parent_exit_reclaims_descendants_before_returning(self):
        result = self.invoke(self.parent_with_child(exit_code=17))
        self.assertEqual(result, 17)
        self.assert_child_gone_and_logs_stable()

    def test_timeout_reclaims_descendants_and_preserves_timeout(self):
        argv = self.parent_with_child(stay_alive=True)
        with self.assertRaises(subprocess.TimeoutExpired) as caught:
            self.invoke(argv, timeout=1)
        self.assertEqual(caught.exception.cmd, argv)
        self.assertEqual(caught.exception.timeout, 1)
        self.assert_child_gone_and_logs_stable()

    def test_keyboard_interrupt_reclaims_descendants_then_propagates(self):
        original_wait = subprocess.Popen.wait
        interrupted = False

        def interrupt_wait(process, *args, **kwargs):
            nonlocal interrupted
            if not interrupted:
                interrupted = True
                deadline = time.monotonic() + 5
                while not self.ready.exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue(self.ready.exists(), "interrupt only after a descendant starts")
                raise KeyboardInterrupt
            return original_wait(process, *args, **kwargs)

        with mock.patch.object(subprocess.Popen, "wait", interrupt_wait):
            with self.assertRaises(KeyboardInterrupt):
                self.invoke(self.parent_with_child(stay_alive=True))
        self.assert_child_gone_and_logs_stable()

    def test_arguments_remain_literal_without_a_shell(self):
        value = "literal & echo unsafe > injected.txt $(echo unexpected)"
        result = self.invoke([sys.executable, "-c", "import sys; print(sys.argv[1])", value])
        self.assertEqual(result, 0)
        self.assertIn(value.encode(), self.stdout_path.read_bytes())
        self.assertFalse((self.root / "injected.txt").exists())

    if os.name == "nt":
        def assert_exact_descendant_handles_signaled(self, *, timed_out):
            import ctypes
            from ctypes import wintypes

            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
            kernel.OpenProcess.restype = wintypes.HANDLE
            kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
            kernel.WaitForSingleObject.restype = wintypes.DWORD
            kernel.CloseHandle.argtypes = [wintypes.HANDLE]
            kernel.CloseHandle.restype = wintypes.BOOL
            identities = self.root / "descendants.json"
            child = "import time; time.sleep(60)"
            parent = (
                "import subprocess,sys,json,time; from pathlib import Path; "
                f"children=[subprocess.Popen([sys.executable,'-c',{child!r}]) for _ in range(32)]; "
                f"Path({str(identities)!r}).write_text(json.dumps([p.pid for p in children])); "
                + ("time.sleep(60)" if timed_out else "sys.exit(17)")
            )
            handles = []
            original_cleanup = validation_process._WindowsJob.terminate_and_wait

            def capture_before_termination(job):
                self.assertTrue(identities.is_file(), "actual descendants must start before the timeout")
                for pid in json.loads(identities.read_text()):
                    handle = kernel.OpenProcess(0x00101000, False, pid)  # SYNCHRONIZE | QUERY_LIMITED_INFORMATION
                    self.assertTrue(handle, f"cannot retain actual descendant handle {pid}")
                    handles.append((pid, handle))
                original_cleanup(job)

            try:
                with mock.patch.object(validation_process._WindowsJob, "terminate_and_wait", capture_before_termination):
                    if timed_out:
                        with self.assertRaises(subprocess.TimeoutExpired):
                            self.invoke([sys.executable, "-c", parent], timeout=3)
                    else:
                        self.assertEqual(self.invoke([sys.executable, "-c", parent]), 17)
                # An exact process HANDLE is signaled only after actual exit;
                # this assertion must happen immediately at the API boundary.
                states = [(pid, kernel.WaitForSingleObject(handle, 0)) for pid, handle in handles]
                self.assertEqual(len(states), 32)
                self.assertTrue(all(state == 0 for _, state in states),
                                f"owned process handles were not signaled at return: {states}")
            finally:
                # Preserve the first observed failure while allowing the test
                # directory to be removed after these exact children finish.
                for _, handle in handles:
                    kernel.WaitForSingleObject(handle, 10000)
                    kernel.CloseHandle(handle)

        def test_timeout_waits_for_exact_descendant_handles_before_returning(self):
            self.assert_exact_descendant_handles_signaled(timed_out=True)

        def test_parent_exit_waits_for_exact_descendant_handles_before_returning(self):
            self.assert_exact_descendant_handles_signaled(timed_out=False)

        def test_cleanup_refuses_actual_new_descendants_after_membership_seal(self):
            attempted = self.root / "attempted.json"
            unexpected = self.root / "new-child-ran"
            child = f"from pathlib import Path; Path({str(unexpected)!r}).write_text('escaped')"
            parent = (
                "import subprocess,sys,time,json; from pathlib import Path\n"
                f"Path({str(self.ready)!r}).write_text('ready')\n"
                f"while not Path({str(self.release)!r}).exists(): time.sleep(0.005)\n"
                "try:\n"
                f"    spawned=subprocess.Popen([sys.executable,'-c',{child!r}])\n"
                "    value={'spawned':True,'pid':spawned.pid}\n"
                "except OSError as error:\n"
                "    value={'spawned':False,'winerror':error.winerror}\n"
                f"Path({str(attempted)!r}).write_text(json.dumps(value))\n"
                "time.sleep(60)\n"
            )
            original_seal = validation_process._WindowsJob._stop_new_processes
            observed = []

            def seal_and_observe(job):
                original_seal(job)
                self.assertTrue(self.ready.is_file())
                self.release.write_text("attempt a child after membership sealed")
                deadline = time.monotonic() + 5
                while not attempted.is_file() and time.monotonic() < deadline:
                    time.sleep(0.005)
                self.assertTrue(attempted.is_file(), "existing job member must survive sealing to attempt its spawn")
                value = json.loads(attempted.read_text())
                observed.append(value)
                self.assertFalse(value["spawned"], value)
                self.assertFalse(unexpected.exists())

            with mock.patch.object(validation_process._WindowsJob, "_stop_new_processes", seal_and_observe):
                with self.assertRaises(subprocess.TimeoutExpired):
                    self.invoke([sys.executable, "-c", parent], timeout=1)
            self.assertEqual(len(observed), 1)
            self.assertFalse(unexpected.exists())

        def test_reused_pid_snapshot_does_not_retain_unrelated_process_handle(self):
            # A real unrelated child is returned at the PID-lookup boundary,
            # modeling a PID reused between the job query and OpenProcess.
            unrelated = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"],
                                         cwd=self.root, creationflags=subprocess.CREATE_NO_WINDOW)
            job = validation_process._WindowsJob()
            handles = []
            try:
                with mock.patch.object(job, "_process_ids", return_value=[unrelated.pid]):
                    job._retain_processes(handles)
                self.assertEqual(handles, [])
                job.terminate_and_wait()
                self.assertIsNone(unrelated.poll(), "unrelated process must survive owned job cleanup")
            finally:
                for handle in handles:
                    job._kernel.CloseHandle(handle)
                job.close()
                unrelated.terminate()
                unrelated.wait(timeout=10)

        def test_command_waits_for_job_assignment_before_starting(self):
            original_assign = validation_process._WindowsJob.assign

            def delayed_assign(job, process):
                time.sleep(0.2) # make the assign-before-launch race observable
                self.assertIsNone(process.poll())
                self.assertFalse(self.ready.exists(), "actual command escaped the handshake")
                original_assign(job, process)

            with mock.patch.object(validation_process._WindowsJob, "assign", delayed_assign):
                self.assertEqual(self.invoke(self.parent_with_child()), 0)
            self.assert_child_gone_and_logs_stable()

        def test_failed_job_assignment_reaps_launcher_without_starting_command(self):
            launchers = []

            def reject_assignment(job, process):
                launchers.append(process)
                raise OSError("injected job assignment failure")

            with mock.patch.object(validation_process._WindowsJob, "assign", reject_assignment):
                with self.assertRaisesRegex(OSError, "injected job assignment failure"):
                    self.invoke(self.parent_with_child())
            self.assertEqual(len(launchers), 1)
            self.assertIsNotNone(launchers[0].poll())
            self.assertFalse(self.ready.exists())
    else:
        def test_command_has_a_separate_owned_process_group(self):
            code = "import os; print(os.getpid(), os.getpgrp())"
            self.assertEqual(self.invoke([sys.executable, "-c", code]), 0)
            pid, group = map(int, self.stdout_path.read_text().split())
            self.assertEqual(pid, group)
            self.assertNotEqual(group, os.getpgrp())


if __name__ == "__main__":
    unittest.main()
