"""Actual child/environment/socket ownership checks; no Engine or fixed port."""
from __future__ import annotations

from concurrent.futures import Future, ThreadPoolExecutor
from dataclasses import FrozenInstanceError, replace
import json
import hashlib
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


def _publish_json_code(path, expression):
    """Child-side fixture publication; the reader never sees partial JSON."""
    return (f"_ready=Path({str(path)!r}); "
            "_writing=_ready.with_name('.'+_ready.name+'.writing'); "
            f"_writing.write_text(json.dumps({expression}), encoding='utf-8'); "
            "os.replace(_writing,_ready); ")


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

    def retain_wait_diagnostics(self, path, future):
        # Never retain request.json: it contains the complete effective env.
        # These exact fixture receipts/logs contain only this owned test run.
        base = getattr(self, "diagnostics_root", ROOT / "artifacts/validation/package-runtime-fixture-failures")
        base.mkdir(parents=True, exist_ok=True)
        destination = Path(tempfile.mkdtemp(prefix=self._testMethodName + "-", dir=base))
        self.last_wait_diagnostics = destination
        control = path.parent
        sources = {name: control / name for name in ("run.json", "result.json", "process.json")}
        sources.update({control.name + suffix: self.root / (control.name + suffix)
                        for suffix in (".log", "-out.log", "-err.log")})

        def snapshot(phase):
            folder = destination / phase
            folder.mkdir()
            state = {"state": "PENDING"}
            if future.cancelled():
                state = {"state": "CANCELLED"}
            elif future.done():
                error = future.exception()
                state = ({"state": "FAILED", "error_type": type(error).__name__, "error": str(error)}
                         if error is not None else {"state": "COMPLETED"})
            record = {"test": self.id(), "awaited": str(path), "future": state, "files": {}}
            for name, source in sources.items():
                if not source.exists():
                    record["files"][name] = {"status": "MISSING"}
                    continue
                if source.is_symlink() or not source.is_file():
                    record["files"][name] = {"status": "NOT_REGULAR"}
                    continue
                with source.open("rb") as stream:
                    raw = stream.read(65537)
                kept = raw[:65536]
                (folder / name).write_bytes(kept)
                record["files"][name] = {"status": "RETAINED", "bytes": len(kept),
                    "sha256": hashlib.sha256(kept).hexdigest(), "truncated": len(raw) > 65536,
                    "tail": kept[-4096:].decode("utf-8", errors="replace")}
            (folder / "diagnostics.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
            return record

        initial = snapshot("wait")
        if initial["future"]["state"] == "PENDING":
            # A failed wait unwinds the test's release barrier and executor
            # first. Capture the final owned cleanup before TemporaryDirectory
            # runs, and emit bounded data into CTest's retained LastTest.log.
            def after_owned_cleanup():
                try:
                    final = snapshot("cleanup")
                except Exception as error:
                    final = {"diagnostic_capture_error": str(error)}
                print("runtime fixture final diagnostics: " + json.dumps(final), file=sys.stderr)
            self.addCleanup(after_owned_cleanup)
        return "runtime fixture diagnostics: " + json.dumps(initial) + "\nretained at " + str(destination)

    def await_json(self, path, timeout=6, *, future=None):
        try:
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                if future is not None and future.done():
                    future.result()  # Propagate the real owned launcher failure, including its traceback.
                    self.fail("owned command completed before live publication was observed: " + str(path))
                if path.exists():
                    return json.loads(path.read_text(encoding="utf-8"))
                time.sleep(0.01)
            self.fail("actual child did not publish " + str(path))
        except BaseException as error:
            if future is not None:
                try:
                    error.add_note(self.retain_wait_diagnostics(path, future))
                except Exception as diagnostic_error:
                    error.add_note("runtime fixture diagnostic capture failed: " + str(diagnostic_error))
            raise

    def launch_listener(self, *, family=socket.AF_INET, address="127.0.0.1", port=0):
        ready = self.root / ("listener-%s-%s.json" % (family, time.monotonic_ns()))
        code = ("import socket,json,time,os; from pathlib import Path; "
                f"s=socket.socket({family},socket.SOCK_STREAM); "
                f"s.bind(({address!r},{port})); s.listen(); "
                + _publish_json_code(ready, "{'pid':os.getpid(),'port':s.getsockname()[1]}")
                + "time.sleep(30)")
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

    def test_controlled_capture_crosses_clean_environment_without_options_or_secret_leaks(self):
        # Construct only the transport scope here, so this behavior test can
        # fail against the old production boundary before the helper exists.
        run_dir = self.root / "capture run 中文"
        directory = run_dir / "sanitizer/runtime"
        directory.mkdir(parents=True)
        scope = {"version": 1, "run_id": "fixture-capture-01", "check_id": "runtime",
                 "purpose": "test-fixture", "run_dir": str(run_dir),
                 "directory": str(directory), "prefix": "sanitizer"}
        names = ("ASAN_OPTIONS", "UBSAN_OPTIONS", "LSAN_OPTIONS", "TSAN_OPTIONS")
        pollution = {name: "log_path=outside:suppressions=private-file:print_summary=0" for name in names}
        pollution.update(CAESURA_VALIDATION_SANITIZER_CAPTURE=json.dumps(scope),
                         UNRELATED_SECRET="must remain in the controller", NODE_OPTIONS="--require private.js")
        code = "import json,os; print(json.dumps(dict(os.environ),ensure_ascii=True))"
        with mock.patch.dict(os.environ, pollution):
            before = dict(os.environ)
            report = self.invoke(code)
            self.assertEqual(os.environ, before)
        self.assertEqual(report["actual_exit_code"], 0)
        observed = json.loads((self.root / "control-out.log").read_text(encoding="utf-8"))
        self.assertEqual(json.loads(observed["CAESURA_VALIDATION_SANITIZER_CAPTURE"]), scope)
        expected = f'log_path="{directory / "sanitizer"}":log_exe_name=0:print_summary=1:color=never'
        for name in names:
            self.assertEqual(observed[name], expected)
            self.assertNotIn(name, self.environ)
        for name in ("UNRELATED_SECRET", "NODE_OPTIONS", "LD_PRELOAD", "DYLD_LIBRARY_PATH"):
            self.assertNotIn(name, observed)
        self.assertEqual(observed["PATH"], self.environ["PATH"])
        self.assertEqual(observed["CAESURA_LUA"], str(self.lua))

    def test_malformed_capture_scope_is_not_silently_dropped_at_clean_boundary(self):
        with mock.patch.dict(os.environ, {"CAESURA_VALIDATION_SANITIZER_CAPTURE": "not valid JSON"}):
            with self.assertRaises(ValueError):
                self.invoke("print('must not execute without diagnostic capture')")
        self.assertFalse((self.root / "control/process.json").exists())

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

    def test_listener_publication_never_exposes_a_partial_json_file(self):
        blocked = self.root / "publication-write-blocked"
        release = self.root / "release-publication-write"
        actual_popen = subprocess.Popen
        # Hold a real child's write after one byte. The final readiness path
        # must stay absent until the complete temporary file is renamed.
        prefix = ("import time; from pathlib import Path\n"
                  "_original_write_text = Path.write_text\n"
                  "def _held_write_text(path, text, *args, **kwargs):\n"
                  "    with path.open('w', encoding='utf-8') as stream:\n"
                  "        stream.write(text[:1]); stream.flush()\n"
                  f"    Path({str(blocked)!r}).write_bytes(b'blocked')\n"
                  f"    while not Path({str(release)!r}).exists(): time.sleep(0.01)\n"
                  "    return _original_write_text(path, text, *args, **kwargs)\n"
                  "Path.write_text = _held_write_text\n")

        def launch(argv, **kwargs):
            argv = list(argv)
            argv[-1] = prefix + argv[-1]
            return actual_popen(argv, **kwargs)

        with mock.patch.object(subprocess, "Popen", side_effect=launch), \
                ThreadPoolExecutor(max_workers=1) as executor:
            future = executor.submit(self.launch_listener)
            try:
                deadline = time.monotonic() + 6
                while not blocked.exists() and not future.done() and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue(blocked.exists(), "actual child never reached the publication barrier")
                self.assertEqual(list(self.root.glob("listener-*.json")), [],
                                 "reader can observe the half-written readiness file")
                self.assertFalse(future.done(), "reader returned before atomic publication")
            finally:
                release.write_bytes(b'release')
            process, ready = future.result(timeout=6)
        self.assertEqual(process_identity(process.pid).pid, ready["pid"])
        self.assertTrue(verify_owned_listener(process_identity(process.pid), ready["port"]))

    def test_malformed_published_json_is_rejected_without_retry(self):
        malformed = self.root / "malformed-ready.json"
        malformed.write_text("{", encoding="utf-8")
        with self.assertRaises(json.JSONDecodeError):
            self.await_json(malformed)

    def test_completed_owned_child_without_publication_cannot_report_readiness(self):
        self.diagnostics_root = self.root / "retained-diagnostics"
        with ThreadPoolExecutor(max_workers=1) as executor:
            future = executor.submit(self.invoke, "print('owned child finished without readiness')",
                                     control="unpublished")
            report = future.result(timeout=10)
            with self.assertRaisesRegex(AssertionError, "completed before live publication") as caught:
                self.await_json(self.root / "unpublished/never-published.json", future=future)
        self.assertEqual(report["status"], "EXITED")
        self.assertEqual(report["actual_exit_code"], 0)
        saved = json.loads((self.last_wait_diagnostics / "wait/diagnostics.json").read_text())
        self.assertEqual(saved["future"]["state"], "COMPLETED")
        self.assertIn("owned child finished without readiness", saved["files"]["unpublished-out.log"]["tail"])
        self.assertIn("runtime fixture diagnostics", str(caught.exception.__notes__))
        self.assertFalse((self.root / "unpublished/never-published.json").exists())

    def test_wait_failure_diagnostics_are_bounded_and_never_copy_request_environment(self):
        self.diagnostics_root = self.root / "retained-diagnostics"
        control = self.root / "diagnostic"
        control.mkdir()
        (control / "request.json").write_text(json.dumps({"env": {"TEST_SECRET": "do-not-retain-this-sentinel"}}))
        (control / "result.json").write_text(json.dumps({"status": "LAUNCH_FAILED"}))
        raw = b"bounded-child-log\xff\n" * 5000
        (self.root / "diagnostic.log").write_bytes(raw)
        future = Future()
        original = RuntimeContractError("original controlled fixture failure")
        future.set_exception(original)
        with self.assertRaises(RuntimeContractError) as caught:
            self.await_json(control / "process.json", future=future)
        self.assertIs(caught.exception, original)
        saved = json.loads((self.last_wait_diagnostics / "wait/diagnostics.json").read_text())
        self.assertNotIn("request.json", saved["files"])
        self.assertNotIn("do-not-retain-this-sentinel", json.dumps(saved) + str(caught.exception.__notes__))
        entry = saved["files"]["diagnostic.log"]
        kept = (self.last_wait_diagnostics / "wait/diagnostic.log").read_bytes()
        self.assertEqual(kept, raw[:65536])
        self.assertTrue(entry["truncated"])
        self.assertEqual(entry["sha256"], hashlib.sha256(kept).hexdigest())
        self.assertLessEqual(len(entry["tail"]), 4096)

    def test_pending_wait_failure_retains_final_owned_receipt_before_temp_cleanup(self):
        with tempfile.TemporaryDirectory(prefix="caesura-retained-wait-") as evidence:
            self.diagnostics_root = Path(evidence)
            ready, release = self.root / "diagnostic-started.json", self.root / "release-diagnostic-child"
            code = ("import os,json,time; from pathlib import Path; "
                    + _publish_json_code(ready, "{'pid':os.getpid()}")
                    + f"release=Path({str(release)!r})\n"
                    "while not release.exists(): time.sleep(0.01)\n"
                    "print('released owned diagnostic child')")
            with ThreadPoolExecutor(max_workers=1) as executor:
                future = executor.submit(self.invoke, code, control="pending")
                try:
                    child = self.await_json(ready)
                    with self.assertRaisesRegex(AssertionError, "did not publish"):
                        self.await_json(self.root / "pending/never-published.json", timeout=0.03, future=future)
                    diagnostic = self.last_wait_diagnostics
                    initial = json.loads((diagnostic / "wait/diagnostics.json").read_text())
                    self.assertEqual(initial["future"]["state"], "PENDING")
                finally:
                    release.write_bytes(b"release")
                self.assertEqual(future.result(timeout=10)["owned_tree_cleanup"], "COMPLETE")
            self.doCleanups()  # Run the retained diagnostic before the original fixture directory cleanup.
            final = json.loads((diagnostic / "cleanup/diagnostics.json").read_text())
            self.assertEqual(final["future"]["state"], "COMPLETED")
            saved_run = json.loads((diagnostic / "cleanup/run.json").read_text())
            self.assertEqual(saved_run["actual_exit_code"], 0)
            self.assertEqual(saved_run["owned_tree_cleanup"], "COMPLETE")
            self.assertIn("released owned diagnostic child", final["files"]["pending-out.log"]["tail"])
            with self.assertRaises(RuntimeContractError):
                process_identity(child["pid"])

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
                + _publish_json_code(ready, "{'pid':os.getpid()}") + "time.sleep(30)")
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

    def test_cleanup_reclaims_started_descendant_before_application_publication(self):
        started = self.root / "descendant-started.json"
        release = self.root / "release-descendant-publication"
        published = self.root / "descendant-application.json"
        child = ("import os,json,time; from pathlib import Path; "
                 + _publish_json_code(started, "{'pid':os.getpid(),'ppid':os.getppid()}")
                 + f"release=Path({str(release)!r})\n"
                 "while not release.exists(): time.sleep(0.01)\n"
                 + _publish_json_code(published, "{'pid':os.getpid()}") + "time.sleep(30)")
        parent = ("import subprocess,sys,time; "
                  f"subprocess.Popen([sys.executable,'-I','-c',{child!r}]); time.sleep(30)")
        with ThreadPoolExecutor(max_workers=1) as executor:
            future = executor.submit(self.invoke, parent, stop=True)
            try:
                descendant = self.await_json(started)
                owner = ProcessIdentity(**self.await_json(self.root / "control/process.json"))
                identity = process_identity(descendant["pid"])
                self.assertEqual(descendant["ppid"], owner.pid)
                self.assertEqual(Path(identity.executable), Path(self.python_image))
                self.assertNotEqual(identity.pid, owner.pid)
                self.assertFalse(published.exists())
            finally:
                if (self.root / "control").is_dir():
                    (self.root / "control/stop").write_text("stop", encoding="utf-8")
            report = future.result(timeout=8)
        self.assertEqual(report["status"], "STOPPED")
        self.assertEqual(report["owned_tree_cleanup"], "COMPLETE")
        self.assertFalse(report["timed_out"])
        self.assertFalse(report["forced_kill"])
        self.assertFalse(release.exists(), "application publication barrier was never released")
        self.assertFalse(published.exists(), "cleanup need not wait for application publication")
        for identity in (owner, identity):
            with self.assertRaises(RuntimeContractError):
                process_identity(identity.pid)

    def test_timeout_reclaims_actual_descendants_and_never_claims_stop(self):
        ready = self.root / "descendant.json"
        child = ("import os,json,time; from pathlib import Path; "
                 + _publish_json_code(ready, "{'pid':os.getpid()}") + "time.sleep(30)")
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
                 + _publish_json_code(ready, "{'pid':os.getpid()}") + "time.sleep(30)")
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
            # On the hosted Mac, /bin/sh can be observed by libproc as
            # /bin/bash. Declare the actual interpreter for these fixtures;
            # keep the production exact-image rejection unchanged.
            interpreter = "/bin/bash" if sys.platform == "darwin" else "/bin/sh"
            script.write_text("#!" + interpreter + "\n" + body + "\n", encoding="utf-8")
            script.chmod(0o755)
            with (self.root / (control + ".log")).open("wb") as log:
                return run_runtime_command([str(script)], self.root, self.environ,
                    self.root / control, log, log, 8,
                    expected_final_executable=expected or self.python_image,
                    exec_observation_timeout=observe)

        def test_declared_fixture_shell_matches_actual_held_interpreter(self):
            ready = self.root / "shell-ready.json"
            temporary = self.root / "shell-ready.writing"
            shell_release = self.root / "release-shell"
            python_release = self.root / "release-python"
            code = ("from pathlib import Path; import time; "
                    f"release=Path({str(python_release)!r})\n"
                    "while not release.exists(): time.sleep(0.01)")
            body = ("printf '{\"pid\":%s}\\n' \"$$\" > " + shlex.quote(str(temporary)) + "\n"
                    + "mv " + shlex.quote(str(temporary)) + " " + shlex.quote(str(ready)) + "\n"
                    + "while [ ! -f " + shlex.quote(str(shell_release)) + " ]; do sleep 0.01; done\n"
                    + "exec " + shlex.quote(self.python_image) + " -I -c " + shlex.quote(code))
            with ThreadPoolExecutor(max_workers=1) as executor:
                future = executor.submit(self.exec_script, body, observe=4)
                try:
                    started = self.await_json(ready, future=future)
                    shell = process_identity(started["pid"])
                    script = self.root / "mapping-AppRun"
                    declared = script.read_text(encoding="utf-8").splitlines()[0][2:]
                    self.assertEqual(Path(shell.executable), Path(declared).resolve())
                    self.assertFalse((self.root / "mapping/process.json").exists())
                    shell_release.write_bytes(b"release")
                    final = ProcessIdentity(**self.await_json(self.root / "mapping/process.json", future=future))
                    self.assertEqual((shell.pid, shell.created), (final.pid, final.created))
                    self.assertEqual(Path(final.executable), Path(self.python_image))
                finally:
                    shell_release.write_bytes(b"release")
                    python_release.write_bytes(b"release")
                report = future.result(timeout=8)
            self.assertEqual(report["exec_transition"]["status"], "VERIFIED")
            self.assertEqual(report["actual_exit_code"], 0)
            self.assertEqual(report["owned_tree_cleanup"], "COMPLETE")
            with self.assertRaises(RuntimeContractError):
                process_identity(final.pid)

        def test_actual_exec_refusal_surfaces_original_future_error_and_raw_receipts(self):
            self.diagnostics_root = self.root / "retained-diagnostics"
            with ThreadPoolExecutor(max_workers=1) as executor:
                future = executor.submit(self.exec_script, "echo controlled-wrong-image >&2\nexec /bin/sleep 20")
                with self.assertRaises(RuntimeContractError) as caught:
                    self.await_json(self.root / "mapping/process.json", future=future)
            self.assertIs(caught.exception, future.exception())
            saved = json.loads((self.last_wait_diagnostics / "wait/diagnostics.json").read_text())
            self.assertEqual(saved["future"]["error"], str(caught.exception))
            report = json.loads((self.last_wait_diagnostics / "wait/run.json").read_text())
            self.assertEqual(report["status"], "LAUNCH_FAILED")
            self.assertEqual(report["owned_tree_cleanup"], "COMPLETE")
            self.assertIsNone(report["process"])
            self.assertFalse((self.root / "mapping/process.json").exists())
            for name in ("run.json", "result.json"):
                self.assertEqual((self.last_wait_diagnostics / "wait" / name).read_bytes(),
                                 (self.root / "mapping" / name).read_bytes())
            self.assertEqual((self.last_wait_diagnostics / "wait/mapping.log").read_bytes(),
                             (self.root / "mapping.log").read_bytes())

        def test_actual_same_pid_exec_publishes_only_final_engine_identity(self):
            release = self.root / "release"
            code = ("from pathlib import Path; import time; "
                    f"release=Path({str(release)!r}); deadline=time.monotonic()+5\n"
                    "while not release.exists() and time.monotonic()<deadline: time.sleep(0.01)\n"
                    "assert release.exists()")
            with ThreadPoolExecutor(max_workers=1) as executor:
                future = executor.submit(self.exec_script, "exec " + shlex.quote(self.python_image) + " -I -c " + shlex.quote(code))
                try:
                    observed = self.await_json(self.root / "mapping/process.json", future=future)
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
            publish_release = self.root / "release-spawned-publication"
            child = ("import json,os,time; from pathlib import Path; "
                     f"release=Path({str(publish_release)!r})\n"
                     "while not release.exists(): time.sleep(0.01)\n"
                     + _publish_json_code(child_pid, "{'pid':os.getpid()}") + "time.sleep(20)")
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
                    self.assertIsNone(report["process"])
                    self.assertNotEqual(report["exec_transition"]["status"], "VERIFIED")
                    self.assertFalse((self.root / name / "process.json").exists())
                    with self.assertRaises(RuntimeContractError):
                        process_identity(report["exec_transition"]["child_pid"])
                    # Exec rejection may reap the descendant before Python
                    # publishes anything. Started-child cleanup is checked by
                    # the separate handshake/barrier test above, not by waiting
                    # for a file which a correctly killed process cannot write.
                    if index == 1:
                        self.assertFalse(publish_release.exists())
                        self.assertFalse(child_pid.exists())

        def test_launch_script_change_after_exec_is_not_accepted(self):
            release = self.root / "release"
            code = ("from pathlib import Path; import time; "
                    f"release=Path({str(release)!r}); deadline=time.monotonic()+5\n"
                    "while not release.exists() and time.monotonic()<deadline: time.sleep(0.01)")
            with ThreadPoolExecutor(max_workers=1) as executor:
                future = executor.submit(self.exec_script, "exec " + shlex.quote(self.python_image) + " -I -c " + shlex.quote(code))
                try:
                    self.await_json(self.root / "mapping/process.json", future=future)
                    script = self.root / "mapping-AppRun"
                    shebang = script.read_text(encoding="utf-8").splitlines()[0]
                    script.write_text(shebang + "\nexit 0\n", encoding="utf-8")
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
                        self.await_json(self.root / "mapping/process.json", future=future)
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
