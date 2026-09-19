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
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from package_runtime import (ProcessIdentity, RuntimeContractError, native_env,
                             process_identity, loopback_listeners,
                             verify_owned_listener, run_runtime_command)


class PackageRuntimeTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="caesura-package-runtime-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
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
        self.assertEqual(Path(identity.executable).resolve(), Path(sys.executable).resolve())
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
        other, _ = self.launch_listener(address="127.0.0.2", port=ready["port"])
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
                    expected_final_executable=expected or str(Path(sys.executable).resolve()),
                    exec_observation_timeout=observe)

        def test_actual_same_pid_exec_publishes_only_final_engine_identity(self):
            release = self.root / "release"
            code = ("from pathlib import Path; import time; "
                    f"release=Path({str(release)!r}); deadline=time.monotonic()+5\n"
                    "while not release.exists() and time.monotonic()<deadline: time.sleep(0.01)\n"
                    "assert release.exists()")
            with ThreadPoolExecutor(max_workers=1) as executor:
                future = executor.submit(self.exec_script, "exec " + shlex.quote(sys.executable) + " -I -c " + shlex.quote(code))
                try:
                    observed = self.await_json(self.root / "mapping/process.json")
                    self.assertEqual(Path(observed["executable"]), Path(sys.executable).resolve())
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
            python = shlex.quote(sys.executable)
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
                future = executor.submit(self.exec_script, "exec " + shlex.quote(sys.executable) + " -I -c " + shlex.quote(code))
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
