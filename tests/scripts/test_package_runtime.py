"""Actual child/environment/socket ownership checks; no Engine or fixed port."""
from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from dataclasses import FrozenInstanceError, replace
import json
import os
from pathlib import Path
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from package_runtime import (ProcessIdentity, RuntimeContractError, native_env,
                             process_identity, loopback_listeners,
                             verify_owned_listener, run_runtime_command)
import package_runtime as runtime


class PythonFrameworkIdentityTests(unittest.TestCase):
    """OS-boundary fixtures exercise the real contract and launcher, not macOS."""

    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="caesura-framework-identity-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name).resolve()
        self.stub = self.root / "bin/python3.14"
        self.image = self.root / "Resources/Python.app/Contents/MacOS/Python"
        for path in (self.stub, self.image):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"OS executable identity fixture: " + path.name.encode())
        self.controller = ProcessIdentity(os.getpid(), "controller-creation", str(self.image), "macos:libproc")
        self.initial = ProcessIdentity(3127, "child-creation", str(self.stub), "macos:libproc")
        self.final = replace(self.initial, executable=str(self.image))
        self.launch_count = 0

    def contract(self, command=None, controller=None):
        # Only the OS identity boundary is replaced; no macOS API runs on this host.
        with mock.patch.object(runtime.sys, "platform", "darwin"), \
                mock.patch.object(runtime.sys, "executable", str(self.stub)), \
                mock.patch.object(runtime, "process_identity", return_value=controller or self.controller):
            return runtime._exec_contract([str(command or self.stub)], None, 0.2, 3)

    def launch(self, observations, *, exited=False, exit_code=0, contract=None):
        contract = self.contract() if contract is None else contract
        self.launch_count += 1
        control = self.root / f"control-{self.launch_count}"
        control.mkdir()
        request = dict(argv=[str(self.stub), "-I", "-c", "pass"], cwd=str(self.root), env={},
                       identity_path=str(control / "process.json"),
                       result_path=str(control / "result.json"), stop_request=None,
                       exec_contract=contract)
        request_file = control / "request.json"
        request_file.write_text(json.dumps(request), encoding="utf-8")
        child = mock.Mock(pid=self.initial.pid)
        # Each observation has a before/after poll. The child then exits normally.
        child.poll.side_effect = ([exit_code] if exited else [None] * (2 * len(observations))) + [exit_code] * 8
        child.wait.return_value = exit_code
        elapsed = 0.0
        def advance(duration):
            nonlocal elapsed
            elapsed = round(elapsed + duration, 9)
        # This entire child/identity boundary is synthetic. Advance its deadline
        # with its requested waits, independently of hosted scheduling latency.
        # Real child tests below keep the production wall clock and sleeps.
        clock = SimpleNamespace(monotonic=lambda: elapsed, sleep=advance)
        with mock.patch.object(runtime.subprocess, "Popen", return_value=child), \
                mock.patch.object(runtime, "process_identity", side_effect=observations), \
                mock.patch.object(runtime, "time", clock):
            status = runtime._runtime_launcher(request_file)
        result = json.loads((control / "result.json").read_text(encoding="utf-8"))
        identity = control / "process.json"
        return status, result, json.loads(identity.read_text(encoding="utf-8")) if identity.exists() else None

    def test_framework_stub_is_never_published_before_exact_final_image(self):
        status, result, identity = self.launch([self.initial, self.initial, self.final, self.final])
        self.assertEqual(status, 0, result)
        self.assertEqual(identity["executable"], str(self.image))
        self.assertEqual(identity["pid"], self.initial.pid)
        self.assertEqual(identity["created"], self.initial.created)
        self.assertEqual(result["exec_transition"]["status"], "VERIFIED")
        self.assertTrue(result["exec_transition"]["inputs_stable"])

    def test_framework_final_image_can_already_be_mapped_at_first_observation(self):
        status, result, identity = self.launch([self.final, self.final])
        self.assertEqual(status, 0, result)
        self.assertEqual(identity["executable"], str(self.image))

    def test_framework_fixture_observations_are_independent_of_host_sleep_delay(self):
        actual_sleep = time.sleep
        # The child and OS observations are already simulated. Scheduling delay
        # must not determine whether their declared final-image sequence passes.
        with mock.patch.object(runtime.time, "sleep", side_effect=lambda _: actual_sleep(0.08)):
            status, result, identity = self.launch([self.initial, self.initial, self.final, self.final])
        self.assertEqual(status, 0, result)
        self.assertEqual(identity["executable"], str(self.image))
        self.assertEqual(result["exec_transition"]["status"], "VERIFIED")

    def test_framework_deadline_rejects_unverified_or_only_once_observed_final_image(self):
        for name, observations in (("no-final", [self.initial] * 100),
                                   ("one-final", [self.initial] * 19 + [self.final, self.final])):
            with self.subTest(name=name):
                status, result, identity = self.launch(observations)
                self.assertEqual(status, 125, result)
                self.assertIsNone(identity)
                self.assertEqual(result["status"], "LAUNCH_FAILED")
                self.assertEqual(result["exec_transition"]["contract"]["observation_timeout"], 0.2)
                self.assertEqual(result["identity_error"],
                                 "Final executable was not observed within the explicit exec deadline")
                if name == "one-final":
                    self.assertEqual(result["exec_transition"]["observations"][-1]["executable"], str(self.image))

    def test_framework_rejects_another_interpreter_and_changed_creation(self):
        for name, wrong in (("interpreter", replace(self.final, executable=str(self.root / "other-python"))),
                            ("creation", replace(self.final, created="different-creation"))):
            with self.subTest(name=name):
                status, result, identity = self.launch([self.initial, wrong])
                self.assertEqual(status, 125, result)
                self.assertIsNone(identity)
                self.assertEqual(result["status"], "LAUNCH_FAILED")

    def test_framework_locks_both_declared_stub_and_observed_image(self):
        contract = self.contract()
        self.assertIsNotNone(contract)
        for path in (self.stub, self.image):
            with self.subTest(path=path):
                before = path.read_bytes()
                path.write_bytes(before + b"changed")
                with self.assertRaises(RuntimeContractError):
                    runtime._check_exec_files(contract)
                path.write_bytes(before)

    def test_framework_early_exit_retains_exit_without_publishing_readiness(self):
        status, result, identity = self.launch([], exited=True, exit_code=23)
        self.assertEqual(status, 0, result)
        self.assertEqual(result["actual_exit_code"], 23)
        self.assertEqual(result["status"], "EXITED")
        self.assertIsNone(identity)
        self.assertEqual(result["exec_transition"]["status"], "EXITED_BEFORE_VERIFICATION")

    def test_framework_mapping_is_limited_to_current_explicit_python(self):
        other = self.root / "other-python"
        other.write_bytes(self.stub.read_bytes())
        self.assertIsNone(self.contract(command=other))
        self.assertIsNone(self.contract(controller=replace(self.controller, executable=str(self.stub))))


class RetainedChildExitTests(unittest.TestCase):
    """A real child blocks on a release barrier; only the retained wait opens it."""

    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="caesura-retained-exit-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name).resolve()

    def monitor_child(self, name, *, exit_code=0, release_on_wait=True, changed=False):
        control = self.root / name
        control.mkdir()
        release = control / "release"
        image = Path(process_identity(os.getpid()).executable)
        # The monitoring contract is injected, not a claim of Windows exec.
        # Popen, poll, process identity before the fault and wait are real OS calls.
        locked = runtime._exec_file_identity(image)
        contract = dict(launcher=locked, interpreter=locked, final_executable=locked,
                        observation_timeout=2)
        code = ("import sys,time; from pathlib import Path; "
                f"release=Path({str(release)!r}); deadline=time.monotonic()+5\n"
                "while not release.exists() and time.monotonic()<deadline: time.sleep(0.005)\n"
                f"sys.exit({exit_code} if release.exists() else 91)")
        request = dict(argv=[str(image), "-I", "-c", code], cwd=str(control), env=dict(os.environ),
                       identity_path=str(control / "process.json"), result_path=str(control / "result.json"),
                       stop_request=None, exec_contract=contract)
        request_file = control / "request.json"
        request_file.write_text(json.dumps(request), encoding="utf-8")
        actual_popen, actual_identity = subprocess.Popen, process_identity
        state = dict(observations=0, identity_failed=False, terminated=False, wait_released=False)
        children = []

        def start(*args, **kwargs):
            child = actual_popen(*args, **kwargs)
            children.append(child)
            actual_wait, actual_terminate = child.wait, child.terminate
            def wait(timeout=None):
                if state["identity_failed"] and not state["terminated"] and release_on_wait:
                    # The old immediate poll cannot complete this barrier. The
                    # actual retained wait must begin before the child can exit.
                    release.write_text("release", encoding="utf-8")
                    state["wait_released"] = True
                return actual_wait(timeout=timeout)
            def terminate():
                state["terminated"] = True
                return actual_terminate()
            child.wait, child.terminate = wait, terminate
            return child

        def observe(pid):
            identity = actual_identity(pid)
            state["observations"] += 1
            if state["observations"] <= 2:
                return identity
            self.assertTrue((control / "process.json").is_file())
            self.assertIsNone(children[0].poll(), "the controlled child is still blocked at the observation fault")
            if changed:
                return replace(identity, created=identity.created + "-different")
            state["identity_failed"] = True
            raise RuntimeContractError("controlled executable-query failure during exit")

        try:
            with mock.patch.object(runtime.subprocess, "Popen", side_effect=start), \
                    mock.patch.object(runtime, "process_identity", side_effect=observe):
                result_code = runtime._runtime_launcher(request_file)
        finally:
            for child in children:
                if child.poll() is None:
                    child.terminate()
                child.wait(timeout=3)
        report = json.loads((control / "result.json").read_text(encoding="utf-8"))
        self.assertEqual(report["exec_transition"]["status"], "VERIFIED")
        self.assertTrue(report["exec_transition"]["inputs_stable"])
        with self.assertRaises(RuntimeContractError):
            actual_identity(children[0].pid)
        return result_code, report, state

    def test_observation_failure_then_proven_exit_preserves_actual_zero_and_nonzero(self):
        for expected_exit in (0, 23):
            with self.subTest(exit_code=expected_exit):
                result_code, report, state = self.monitor_child("exit-" + str(expected_exit), exit_code=expected_exit)
                self.assertEqual(result_code, 0, report)
                self.assertEqual(report["status"], "EXITED")
                self.assertEqual(report["actual_exit_code"], expected_exit)
                self.assertEqual(report["exit_observation"]["wait_exit_code"], expected_exit)
                self.assertEqual(report["exit_observation"]["source"], "retained-child-wait")
                self.assertIn("controlled executable-query failure", report["exit_observation"]["identity_error"])
                self.assertFalse(state["terminated"])
                self.assertTrue(state["wait_released"])
                self.assertFalse(report["forced_kill"])
                self.assertFalse(report["stop_requested"])

    def test_observation_failure_for_still_running_child_remains_failure_and_is_reaped(self):
        result_code, report, state = self.monitor_child("still-live", release_on_wait=False)
        self.assertEqual(result_code, 125)
        self.assertEqual(report["status"], "LAUNCH_FAILED")
        self.assertIn("controlled executable-query failure", report["error"])
        self.assertNotIn("exit_observation", report)
        self.assertTrue(state["terminated"])

    def test_changed_live_identity_cannot_take_the_exit_confirmation_path(self):
        result_code, report, state = self.monitor_child("changed-identity", changed=True)
        self.assertEqual(result_code, 125)
        self.assertEqual(report["status"], "LAUNCH_FAILED")
        self.assertIn("Verified final executable changed", report["error"])
        self.assertNotIn("exit_observation", report)
        self.assertTrue(state["terminated"])
        self.assertFalse(state["wait_released"])


class PackageRuntimeTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="caesura-package-runtime-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.python_image = process_identity(os.getpid()).executable
        self.package = self.root / "包 with spaces"
        self.package.mkdir()
        self.engine = self.package / "engine.exe"
        self.lua = self.package / "lua.exe"
        self.engine.write_bytes(b"static identity fixture, not executable proof")
        self.lua.write_bytes(b"static Lua identity fixture")
        for name in ("work", "home", "temp"):
            (self.root / name).mkdir()
        self.environ = native_env(self.package, engine=self.engine, lua=self.lua,
                                  work=self.root / "work", home=self.root / "home",
                                  temp=self.root / "temp")

    def invoke(self, code, *, timeout=8, control="control", stop=False):
        directory = self.root / control
        with (self.root / (control + "-out.log")).open("wb") as out, \
                (self.root / (control + "-err.log")).open("wb") as err:
            return run_runtime_command(
                [sys.executable, "-I", "-c", code], self.root / "work", self.environ,
                directory, out, err, timeout,
                stop_request=directory / "stop" if stop else None)

    def await_json(self, path, timeout=6):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if path.exists():
                return json.loads(path.read_text(encoding="utf-8"))
            time.sleep(0.01)
        self.fail("actual child did not publish " + str(path))

    def launch_listener(self, *, family=socket.AF_INET, address="127.0.0.1", port=0):
        ready = self.root / ("listener-%s-%s.json" % (family, time.monotonic_ns()))
        code = ("import socket,json,time,os; from pathlib import Path; "
                f"s=socket.socket({family},socket.SOCK_STREAM); "
                f"s.bind(({address!r},{port})); s.listen(); "
                f"Path({str(ready)!r}).write_text(json.dumps({{'pid':os.getpid(),"
                "'port':s.getsockname()[1]})); time.sleep(30)")
        options = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}
        process = subprocess.Popen([sys.executable, "-I", "-c", code], **options)
        def cleanup():
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=5)
        self.addCleanup(cleanup)
        return process, self.await_json(ready)

    def test_environment_discards_developer_paths_and_preserves_display(self):
        polluted = dict(os.environ, PATH="unrelated-tools", CAESURA_ROOT="repository",
                        CAESURA_LUA="host-lua", LUA_PATH="host/?.lua", LUA_CPATH="host.dll",
                        PYTHONPATH="hostpython", NODE_OPTIONS="--require bad.js",
                        LD_PRELOAD="bad.so", DYLD_LIBRARY_PATH="hostlibs",
                        DISPLAY=":27", HTTPS_PROXY="private proxy", UNRELATED_SECRET="secret")
        snapshot = dict(os.environ)
        result = native_env(self.package, engine=self.engine, lua=self.lua,
                            work=self.root / "work", home=self.root / "home",
                            temp=self.root / "temp", inherited=polluted)
        self.assertEqual(os.environ, snapshot)
        self.assertEqual(polluted["CAESURA_LUA"], "host-lua")
        self.assertEqual(result["CAESURA_LUA"], str(self.lua))
        self.assertEqual(result["HOME"], str(self.root / "home"))
        self.assertEqual(result["TMPDIR"], str(self.root / "temp"))
        self.assertEqual(result["DISPLAY"], ":27")
        self.assertNotIn("unrelated-tools", result["PATH"])
        if os.name == "nt":
            self.assertEqual(result["ProgramData"], str(self.root / "home"))
            self.assertEqual(result["ALLUSERSPROFILE"], str(self.root / "home"))
        for name in ("CAESURA_ROOT", "LUA_PATH", "LUA_CPATH", "PYTHONPATH", "NODE_OPTIONS",
                     "LD_PRELOAD", "DYLD_LIBRARY_PATH", "HTTPS_PROXY", "UNRELATED_SECRET"):
            self.assertNotIn(name, result)

    def test_environment_refuses_external_or_missing_executables(self):
        for engine, lua in ((Path(sys.executable), self.lua), (self.engine, self.root / "absent")):
            with self.subTest(engine=engine, lua=lua), self.assertRaises(RuntimeContractError):
                native_env(self.package, engine=engine, lua=lua, work=self.root / "work",
                           home=self.root / "home", temp=self.root / "temp")

    def test_mutable_home_or_temp_cannot_point_inside_the_package(self):
        for home, temp in ((self.package, self.root / "temp"),
                           (self.root / "home", self.package)):
            with self.subTest(home=home, temp=temp), self.assertRaises(RuntimeContractError):
                native_env(self.package, engine=self.engine, lua=self.lua,
                           work=self.package, home=home, temp=temp)

    def test_actual_child_receives_exact_environment_without_global_mutation(self):
        before = dict(os.environ)
        self.environ["RUNTIME_TEST_VALUE"] = "literal & $(not a shell) 中文"
        # ASCII JSON preserves Unicode values without assuming the native
        # child's Windows stdout encoding is UTF-8.
        code = "import os,json; print(json.dumps(dict(os.environ),ensure_ascii=True))"
        report = self.invoke(code)
        observed = json.loads((self.root / "control-out.log").read_text(encoding="utf-8"))
        self.assertEqual(observed["RUNTIME_TEST_VALUE"], self.environ["RUNTIME_TEST_VALUE"])
        self.assertEqual(observed["CAESURA_LUA"], str(self.lua))
        self.assertEqual(observed["PATH"], self.environ["PATH"])
        self.assertEqual(report["actual_exit_code"], 0)
        self.assertEqual(report["status"], "EXITED")
        self.assertEqual(os.environ, before)

    def test_identity_is_immutable_and_matches_actual_child(self):
        process, ready = self.launch_listener()
        identity = process_identity(process.pid)
        self.assertIsInstance(identity, ProcessIdentity)
        self.assertEqual(identity.pid, ready["pid"])
        self.assertEqual(Path(identity.executable).resolve(), Path(self.python_image))
        self.assertTrue(identity.created)
        self.assertEqual(process_identity(process.pid), identity)
        with self.assertRaises(FrozenInstanceError):
            identity.pid = 1

    def test_actual_loopback_listener_requires_exact_owner(self):
        process, ready = self.launch_listener()
        owner = process_identity(process.pid)
        rows = verify_owned_listener(owner, ready["port"])
        self.assertEqual({row["pid"] for row in rows}, {process.pid})
        self.assertTrue(any(row["address"] == "127.0.0.1" for row in rows))
        with self.assertRaises(RuntimeContractError):
            verify_owned_listener(process_identity(os.getpid()), ready["port"])
        self.assertIsNone(process.poll(), "rejected unrelated owner must remain alive")

    def test_wildcard_listener_is_not_ignored(self):
        process, ready = self.launch_listener(address="0.0.0.0")
        rows = verify_owned_listener(process_identity(process.pid), ready["port"])
        self.assertTrue(any(row["address"] == "0.0.0.0" for row in rows))

    def test_same_port_with_another_loopback_owner_is_rejected(self):
        first, ready = self.launch_listener(address="127.0.0.1")
        # macOS only assigns 127.0.0.1 by default. IPv6 gives another real
        # loopback listener on the same port without changing host interfaces.
        other, _ = self.launch_listener(family=socket.AF_INET6, address="::1", port=ready["port"])
        rows = loopback_listeners(ready["port"])
        self.assertEqual({row["pid"] for row in rows}, {first.pid, other.pid})
        with self.assertRaises(RuntimeContractError):
            verify_owned_listener(process_identity(first.pid), ready["port"])
        self.assertIsNone(first.poll())
        self.assertIsNone(other.poll(), "inspection must never terminate an unrelated listener")

    def test_ipv6_loopback_listener_is_observed(self):
        process, ready = self.launch_listener(family=socket.AF_INET6, address="::1")
        rows = verify_owned_listener(process_identity(process.pid), ready["port"])
        self.assertTrue(any(row["address"] == "::1" for row in rows))

    def test_changed_creation_or_executable_cannot_reuse_live_pid(self):
        process, ready = self.launch_listener()
        actual = process_identity(process.pid)
        for forged in (replace(actual, created=actual.created + "-old"),
                       replace(actual, executable=str(self.engine))):
            with self.subTest(identity=forged), self.assertRaises(RuntimeContractError):
                verify_owned_listener(forged, ready["port"])
        self.assertIsNone(process.poll())

    def test_early_exit_and_missing_listener_cannot_report_readiness(self):
        process, ready = self.launch_listener()
        identity = process_identity(process.pid)
        process.terminate()
        process.wait(timeout=5)
        with self.assertRaises(RuntimeContractError):
            verify_owned_listener(identity, ready["port"])
        self.assertFalse(loopback_listeners(ready["port"]))
        with self.assertRaises(RuntimeContractError):
            verify_owned_listener(process_identity(os.getpid()), ready["port"])

    def test_nonzero_exit_is_retained_and_not_reported_as_stop(self):
        report = self.invoke("import sys; print('real failure'); sys.exit(23)")
        self.assertEqual(report["actual_exit_code"], 23)
        self.assertEqual(report["status"], "EXITED")
        self.assertFalse(report["stop_requested"])
        self.assertFalse(report["timed_out"])

    def test_controlled_stop_records_identity_and_real_exit(self):
        ready = self.root / "running.json"
        code = ("import os,json,time; from pathlib import Path; "
                f"Path({str(ready)!r}).write_text(json.dumps({{'pid':os.getpid()}})); time.sleep(30)")
        with ThreadPoolExecutor(max_workers=1) as executor:
            future = executor.submit(self.invoke, code, stop=True)
            child = self.await_json(ready)
            identity = ProcessIdentity(**self.await_json(self.root / "control" / "process.json"))
            self.assertEqual(identity.pid, child["pid"])
            self.assertEqual(process_identity(identity.pid), identity)
            (self.root / "control" / "stop").write_text("stop", encoding="utf-8")
            report = future.result(timeout=8)
        self.assertEqual(report["status"], "STOPPED")
        self.assertTrue(report["stop_requested"])
        self.assertFalse(report["timed_out"])
        self.assertFalse(report["forced_kill"])
        self.assertNotEqual(report["actual_exit_code"], 0)
        with self.assertRaises(RuntimeContractError):
            process_identity(child["pid"])

    def test_timeout_reclaims_actual_descendants_and_never_claims_stop(self):
        ready = self.root / "descendant.json"
        child = ("import os,json,time; from pathlib import Path; "
                 f"Path({str(ready)!r}).write_text(json.dumps({{'pid':os.getpid()}})); time.sleep(30)")
        parent = ("import subprocess,sys,time; "
                  f"subprocess.Popen([sys.executable,'-I','-c',{child!r}]); time.sleep(30)")
        with self.assertRaises(subprocess.TimeoutExpired) as caught:
            self.invoke(parent, timeout=3)
        descendant = self.await_json(ready)
        self.assertEqual(caught.exception.timeout, 3)
        report = json.loads((self.root / "control" / "run.json").read_text(encoding="utf-8"))
        self.assertTrue(report["timed_out"])
        self.assertEqual(report["status"], "TIMED_OUT")
        self.assertFalse(report["stop_requested"])
        with self.assertRaises(RuntimeContractError):
            process_identity(descendant["pid"])

    def test_normal_parent_exit_also_reclaims_actual_descendants(self):
        ready = self.root / "descendant.json"
        child = ("import os,json,time; from pathlib import Path; "
                 f"Path({str(ready)!r}).write_text(json.dumps({{'pid':os.getpid()}})); time.sleep(30)")
        parent = ("from pathlib import Path; import subprocess,sys,time; "
                  f"subprocess.Popen([sys.executable,'-I','-c',{child!r}]); "
                  f"ready=Path({str(ready)!r}); deadline=time.monotonic()+4\n"
                  "while not ready.exists() and time.monotonic()<deadline: time.sleep(0.01)\n"
                  "assert ready.exists(); sys.exit(17)")
        report = self.invoke(parent)
        self.assertEqual(report["actual_exit_code"], 17)
        descendant = self.await_json(ready)
        with self.assertRaises(RuntimeContractError):
            process_identity(descendant["pid"])

    def test_stale_or_escaping_control_paths_are_rejected_before_launch(self):
        for name, identity, stop in (("outside-id", self.root / "outside.json", None),
                                     ("outside-stop", None, self.root / "stop")):
            with self.subTest(name=name):
                control = self.root / name
                with (self.root / (name + ".log")).open("wb") as log:
                    with self.assertRaises(RuntimeContractError):
                        run_runtime_command([sys.executable, "-I", "-c", "raise Exception()"],
                                            self.root, self.environ, control, log, log, 5,
                                            identity_path=identity, stop_request=stop)
        self.invoke("print('first')")
        before = (self.root / "control" / "run.json").read_bytes()
        with self.assertRaises(RuntimeContractError):
            self.invoke("raise Exception('must not start')")
        self.assertEqual((self.root / "control" / "run.json").read_bytes(), before)

    def test_overlapping_identity_stop_and_internal_paths_are_rejected_before_launch(self):
        for name, identity_name, stop_name in (
                ("stop-parent", "signal/identity.json", "signal"),
                ("result-temporary", ".result.json.writing", "stop")):
            with self.subTest(name=name):
                control = self.root / name
                with (self.root / (name + ".log")).open("wb") as log:
                    with self.assertRaises(RuntimeContractError):
                        run_runtime_command([sys.executable, "-I", "-c", "print('must not start')"],
                                            self.root, self.environ, control, log, log, 5,
                                            identity_path=control / identity_name,
                                            stop_request=control / stop_name)
                self.assertFalse(control.exists(), "invalid control layout must fail before preparing a launch")

    if os.name == "nt":
        def test_exec_mapping_is_explicitly_unavailable_on_windows(self):
            with (self.root / "mapping.log").open("wb") as log:
                with self.assertRaises(RuntimeContractError):
                    run_runtime_command([sys.executable, "-c", "print('must not start')"],
                        self.root, self.environ, self.root / "mapping", log, log, 5,
                        expected_final_executable=sys.executable)
            self.assertFalse((self.root / "mapping").exists())
    else:
        def exec_script(self, body, *, control="mapping", expected=None, observe=0.5):
            script = self.root / (control + "-AppRun")
            script.write_text("#!/bin/sh\n" + body + "\n", encoding="utf-8")
            script.chmod(0o755)
            with (self.root / (control + ".log")).open("wb") as log:
                return run_runtime_command([str(script)], self.root, self.environ,
                    self.root / control, log, log, 8,
                    expected_final_executable=expected or self.python_image,
                    exec_observation_timeout=observe)

        def test_actual_same_pid_exec_publishes_only_final_engine_identity(self):
            release = self.root / "release"
            code = ("from pathlib import Path; import time; "
                    f"release=Path({str(release)!r}); deadline=time.monotonic()+5\n"
                    "while not release.exists() and time.monotonic()<deadline: time.sleep(0.01)\n"
                    "assert release.exists()")
            with ThreadPoolExecutor(max_workers=1) as executor:
                future = executor.submit(self.exec_script, "exec " + shlex.quote(self.python_image) + " -I -c " + shlex.quote(code))
                try:
                    observed = self.await_json(self.root / "mapping/process.json")
                    self.assertEqual(Path(observed["executable"]), Path(self.python_image))
                finally:
                    release.write_text("release", encoding="utf-8")
                report = future.result()
            self.assertEqual(report["process"], observed)
            self.assertEqual(report["status"], "EXITED")
            self.assertEqual(report["actual_exit_code"], 0)
            self.assertEqual(report["exec_transition"]["status"], "VERIFIED")
            self.assertEqual(report["exec_transition"]["child_pid"], observed["pid"])
            with self.assertRaises(RuntimeContractError):
                process_identity(observed["pid"])

        def test_wrong_exec_spawn_without_exec_early_exit_and_interpreter_timeout_fail(self):
            python = shlex.quote(self.python_image)
            child_pid = self.root / "spawned-child.json"
            child = ("import json,os,time; from pathlib import Path; "
                     f"Path({str(child_pid)!r}).write_text(json.dumps({{'pid':os.getpid()}})); time.sleep(20)")
            bodies = ("exec /bin/sleep 20", python + " -I -c " + shlex.quote(child) + " &\nwait",
                      "exit 0", "sleep 20")
            for index, body in enumerate(bodies):
                with self.subTest(body=body):
                    name = "mapping-" + str(index)
                    with self.assertRaises(RuntimeContractError):
                        self.exec_script(body, control=name)
                    report = self.await_json(self.root / name / "run.json")
                    self.assertEqual(report["status"], "LAUNCH_FAILED")
                    self.assertEqual(report["owned_tree_cleanup"], "COMPLETE")
                    self.assertFalse((self.root / name / "process.json").exists())
                    with self.assertRaises(RuntimeContractError):
                        process_identity(report["exec_transition"]["child_pid"])
                    if index == 1:
                        descendant = self.await_json(child_pid)
                        with self.assertRaises(RuntimeContractError):
                            process_identity(descendant["pid"])

        def test_launch_script_change_after_exec_is_not_accepted(self):
            release = self.root / "release"
            code = ("from pathlib import Path; import time; "
                    f"release=Path({str(release)!r}); deadline=time.monotonic()+5\n"
                    "while not release.exists() and time.monotonic()<deadline: time.sleep(0.01)")
            with ThreadPoolExecutor(max_workers=1) as executor:
                future = executor.submit(self.exec_script, "exec " + shlex.quote(self.python_image) + " -I -c " + shlex.quote(code))
                try:
                    self.await_json(self.root / "mapping/process.json")
                    (self.root / "mapping-AppRun").write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
                finally:
                    release.write_text("release", encoding="utf-8")
                with self.assertRaises(RuntimeContractError):
                    future.result()
            report = self.await_json(self.root / "mapping/run.json")
            self.assertEqual(report["owned_tree_cleanup"], "COMPLETE")
            self.assertEqual(report["status"], "LAUNCH_FAILED")

        def test_undeclared_shebang_dispatcher_is_refused_before_launch(self):
            script = self.root / "AppRun"
            script.write_text("#!/usr/bin/env sh\nexit 0\n", encoding="utf-8")
            script.chmod(0o755)
            with (self.root / "invalid-map.log").open("wb") as log:
                with self.assertRaises(RuntimeContractError):
                    run_runtime_command([str(script)], self.root, self.environ,
                        self.root / "invalid-map", log, log, 5,
                        expected_final_executable=sys.executable)
            self.assertFalse((self.root / "invalid-map").exists())

        if sys.platform.startswith("linux"):
            def test_final_executable_replacement_after_exec_is_rejected(self):
                engine = self.root / "Engine"
                shutil.copy2(Path(sys.executable).resolve(), engine)
                release = self.root / "release"
                code = ("from pathlib import Path; import time; "
                        f"release=Path({str(release)!r}); deadline=time.monotonic()+5\n"
                        "while not release.exists() and time.monotonic()<deadline: time.sleep(0.01)")
                with ThreadPoolExecutor(max_workers=1) as executor:
                    future = executor.submit(self.exec_script,
                        "exec " + shlex.quote(str(engine)) + " -I -c " + shlex.quote(code), expected=str(engine))
                    try:
                        self.await_json(self.root / "mapping/process.json")
                        replacement = self.root / "changed-engine"
                        replacement.write_bytes(b"different final executable bytes")
                        replacement.replace(engine)
                    finally:
                        release.write_text("release", encoding="utf-8")
                    with self.assertRaises(RuntimeContractError):
                        future.result()
                report = self.await_json(self.root / "mapping/run.json")
                self.assertEqual(report["status"], "LAUNCH_FAILED")
                self.assertEqual(report["owned_tree_cleanup"], "COMPLETE")


if __name__ == "__main__":
    unittest.main()
