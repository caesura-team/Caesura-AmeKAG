"""Real-process RPC formatting and managed-run terminal regression; no fake VM/queue.

Usage: python headless_managed_rpc_smoke.py BINARY [NEW_OUTPUT_DIR] [--include-http]
Every timeout or forced termination is a failure.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import socket
import subprocess
import threading
import time
import urllib.error
import urllib.request
import uuid


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


class EngineProcess:
    def __init__(self, binary, record, http=False):
        self.record = record
        self.stdout_lines = []
        self.stderr_lines = []
        self.incoming = queue.Queue()
        self.serial = 1800
        self.http = http
        self.port = None
        self.token = 'u18-managed-test-' + str(uuid.uuid4())
        env = dict(os.environ)
        env.pop("CAESURA_TEST_STALL_MS", None)
        env["CAESURA_RPC_DISPATCH_TIMEOUT_MS"] = "5000"
        if http:
            with socket.socket() as listener:
                listener.bind(("127.0.0.1", 0))
                self.port = listener.getsockname()[1]
            env["CAESURA_EDITOR_PORT"] = str(self.port)
            env["CAESURA_EDITOR_TOKEN"] = self.token
        self.proc = subprocess.Popen(
            [str(binary), "--editor" if http else "--headless"],
            cwd=binary.parent, env=env, stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", errors="replace", bufsize=1,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        record["pid"] = self.proc.pid
        record["http_port"] = self.port
        self.readers = []
        for stream, target, protocol in (
            (self.proc.stdout, self.stdout_lines, not http),
            (self.proc.stderr, self.stderr_lines, False),
        ):
            reader = threading.Thread(target=self._read, args=(stream, target, protocol), daemon=True)
            reader.start()
            self.readers.append(reader)

    def _read(self, stream, target, protocol):
        try:
            for line in stream:
                target.append(line)
                if protocol:
                    self.incoming.put(line)
        finally:
            if protocol:
                self.incoming.put(None)

    def rpc(self, method, timeout=15, **fields):
        self.serial += 1
        request = {"id": self.serial, "method": method, **fields}
        self.proc.stdin.write(json.dumps(request, ensure_ascii=False) + "\n")
        self.proc.stdin.flush()
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("reply timeout: method=" + method)
            line = self.incoming.get(timeout=remaining)
            if line is None:
                raise RuntimeError("stdout closed before reply: method=" + method)
            try:
                response = json.loads(line)
            except ValueError:
                continue
            if isinstance(response, dict) and response.get("id") == request["id"]:
                return response

    def http_request(self, path, data=None, timeout=10):
        request = urllib.request.Request(
            "http://127.0.0.1:%d%s" % (self.port, path), data=data,
            headers={"Authorization": "Bearer " + self.token,
                     "Content-Type": "text/plain"},
        )
        try:
            response = urllib.request.urlopen(request, timeout=timeout)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            return response.code, json.loads(response.read().decode("utf-8"))

    def ready(self):
        if not self.http:
            response = self.rpc("ping", timeout=45)
            self.record["checks"]["transport_ready"] = response.get("result") == "ok"
            return
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("HTTP engine exited before ready")
            try:
                status, response = self.http_request("/api/ping", timeout=1)
                if status == 200:
                    self.record["checks"]["transport_ready"] = True
                    return
            except (OSError, ValueError):
                pass
            time.sleep(0.05)  # Read-only readiness observation, never a mutation retry.
        raise TimeoutError("HTTP startup deadline")

    def evaluate(self, code):
        if self.http:
            _, response = self.http_request("/api/eval", code.encode("utf-8"))
            return response
        return self.rpc("eval", code=code)

    def run(self, script):
        if self.http:
            _, response = self.http_request("/api/run", script.encode("utf-8"))
            return response
        return self.rpc("run", script=script)

    def wait_value(self, code, expected):
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            response = self.evaluate(code)
            if response.get("status") == "ok" and response.get("result") == expected:
                return
            time.sleep(0.01)  # Read-only probe of an explicit Lua release gate.
        raise TimeoutError("managed state did not reach " + expected)

    def stop(self):
        if self.http:
            status, response = self.http_request("/api/stop", b"")
            return status == 200 and response.get("status") == "ok"
        return self.rpc("stop").get("result") == "ok"

    def finish(self):
        if not self.proc.stdin.closed:
            try:
                self.proc.stdin.close()
            except OSError as error:
                self.record["stdin_close_error"] = repr(error)
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.record["forced_termination"] = True
            self.proc.kill()
            self.proc.wait(timeout=10)
        for reader in self.readers:
            reader.join(timeout=3)
        self.record["exit_code"] = self.proc.returncode
        self.record["checks"]["natural_exit"] = (
            self.proc.returncode == 0 and not self.record["forced_termination"]
            and all(not reader.is_alive() for reader in self.readers)
        )
        self.proc.stdout.close()
        self.proc.stderr.close()


SOURCE_SECRET = "U18_PRIVATE_SOURCE_SENTINEL"
FAILURE_SECRET = "U18_PRIVATE_FAILURE_SENTINEL"
SETUP = "local k=require('kag'); k._u18_managed={release=false,phase='armed',closes=0,steps=0}; return 'armed'"
PHASE = "local s=require('kag')._u18_managed; return s.phase..':'..s.closes..':'..s.steps"


def managed_script(fail=False):
    end = "s.phase='failed'; error('" + FAILURE_SECRET + "',0)" if fail else "s.phase='completed'"
    return (
        "local source_marker='" + SOURCE_SECRET + "'; "
        "local s=require('kag')._u18_managed; "
        "local guard <close> = setmetatable({}, {__close=function() "
        "s.closes=s.closes+1; print('U18_MANAGED_CLOSE:'..s.closes) end}); "
        "s.phase='yielded'; repeat coroutine.yield() until s.release; "
        "s.steps=s.steps+1; " + end
    )


def check_trace(record, process, expected):
    text = "".join(process.stdout_lines + process.stderr_lines)
    accepted = re.findall(r"\[RpcRequest\] id=(\d+) op=run phase=accepted\b", text)
    run_accepted = re.findall(r"\[RpcRun\] id=(\d+) phase=accepted\b", text)
    terminals = re.findall(r"\[RpcRun\] id=(\d+) phase=(completed|failed|cancelled)\b", text)
    record["accepted_dispatch_ids"] = accepted
    record["managed_accepted_ids"] = run_accepted
    record["managed_terminals"] = terminals
    checks = record["checks"]
    checks["one_accepted_dispatch"] = len(accepted) == 1
    checks["managed_acceptance_links_dispatch"] = run_accepted == accepted and len(accepted) == 1
    checks["one_correlated_terminal"] = len(terminals) == 1 and terminals[0] == (
        accepted[0] if accepted else "missing", expected)
    checks["close_guard_once"] = text.count("U18_MANAGED_CLOSE:1") == 1 and "U18_MANAGED_CLOSE:2" not in text
    checks["no_source_or_body_logs"] = SOURCE_SECRET not in text and FAILURE_SECRET not in text


def execute_case(binary, out, name):
    record = {"name": name, "checks": {}, "forced_termination": False}
    process = None
    expected_terminal = None
    try:
        process = EngineProcess(binary, record, http=name == "http-run-submitted")
        process.ready()
        checks = record["checks"]
        checks["healthy_eval"] = process.evaluate("return 6*7").get("result") == "42"
        if name.startswith("eval-tostring"):
            good = process.evaluate("return setmetatable({}, {__tostring=function() return 'U18_VALUE_OK' end})")
            checks["valid_tostring_preserved"] = good.get("result") == "U18_VALUE_OK"
            error = "error('U18_TOSTRING_FAILURE',0)"
            if name.endswith("nonstring"):
                error = "error(setmetatable({}, {__tostring=function() error('DO_NOT_FORMAT_ERROR') end}),0)"
            response = process.evaluate("return setmetatable({}, {__tostring=function() " + error + " end})")
            record["error_response"] = response
            checks["structured_format_error"] = response.get("status") == "error" and response.get("code") == "eval_result_error"
            checks["fresh_eval_after_format_error"] = process.evaluate("return 21*2").get("result") == "42"
        elif name == "kag-continue":
            scene = 'tests/scripts/u18-rpc-debug.ks'
            breakpoint = process.rpc('kagSetBreakpoint', scene=scene, cmd='p')
            checks['real_kag_breakpoint_installed'] = breakpoint.get('status') == 'ok'
            setup = process.evaluate(
                "local kr=require('kag_runner'); kr.stop(); local k=require('kag'); "
                "k.u18after=function(c) c.f.u18_after=(c.f.u18_after or 0)+1 end; "
                "local flow=require('flow'); local tokenize=require('tokenizer'); local old=flow.load_scene; "
                "flow.load_scene=function() return {tokens=tokenize.parse('[p]\\n[u18after]'),labels={}} end; "
                "local ok=kr.start('" + scene + "'); flow.load_scene=old; assert(ok); kr.update(0); "
                "return tostring(kr.get_ctx()._kag_debug_paused)"
            )
            checks['actual_scene_breakpoint_paused'] = setup.get('result') == 'true'
            resumed = process.rpc('kagDebugContinue')
            checks['continue_acknowledged'] = resumed.get('status') == 'ok' and resumed.get('result') == 'ok'
            state = process.evaluate(
                "local kr=require('kag_runner'); kr.update(0); local c=kr.get_ctx(); "
                "return tostring(c._kag_debug_paused)..':'..tostring(c.waiting_input)..':'..tostring(c.f.u18_after or 0)"
            )
            record['continued_state'] = state
            checks['rpc_continue_reaches_actual_page_wait'] = state.get('result') == 'false:true:0'
            advanced = process.evaluate(
                "local kr=require('kag_runner'); kr.on_click(); return kr.get_ctx().f.u18_after"
            )
            checks['one_click_after_continue_executes_once'] = advanced.get('result') == '1'
        elif name == "kag-debug-tostring":
            setup = process.evaluate(
                "local kd=require('kag_debug'); local kr=require('kag_runner'); "
                "kr.get_ctx=function() return {} end; "
                "kd.serialize_json=function() return setmetatable({}, "
                "{__tostring=function() error('U18_KAG_TOSTRING_FAILURE',0) end}) end; return 'armed'"
            )
            checks["format_fixture_armed"] = setup.get("result") == "armed"
            response = process.rpc("kagInspectScopes", params={"scope": "all"})
            record["error_response"] = response
            checks["structured_format_error"] = response.get("status") == "error" and response.get("code") == "kag_debug_result_error"
            checks["fresh_eval_after_format_error"] = process.evaluate("return 21*2").get("result") == "42"
        else:
            checks["fixture_armed"] = process.evaluate(SETUP).get("result") == "armed"
            response = process.run(managed_script(fail=name == "run-fail"))
            record["acceptance_response"] = response
            checks["run_initially_accepted"] = response.get("status") == "ok"
            process.wait_value(PHASE, "yielded:0:0")
            checks["actual_coroutine_yield_observed"] = True
            if name == "http-run-submitted":
                _, logs = process.http_request("/api/logs")
                messages = [row.get("message", "") for row in logs]
                record["http_run_messages"] = messages
                checks["http_does_not_claim_completion"] = "Scene script completed." not in messages
                checks["http_reports_submission"] = "Scene script submitted." in messages
                expected_terminal = "cancelled"
            elif name == "run-stop":
                expected_terminal = "cancelled"
            else:
                release = process.evaluate("require('kag')._u18_managed.release=true; return 'released'")
                checks["one_release_submitted"] = release.get("result") == "released"
                expected_terminal = "failed" if name == "run-fail" else "completed"
                process.wait_value(PHASE, expected_terminal + ":1:1")
                checks["fresh_eval_after_terminal"] = process.evaluate("return 21*2").get("result") == "42"
        checks["stop_acknowledged"] = process.stop()
    except Exception as error:
        record["error"] = repr(error)
        record["checks"]["protocol_completed"] = False
    finally:
        if process is not None:
            process.finish()
            (out / (name + ".stdout.log")).write_text("".join(process.stdout_lines), encoding="utf-8")
            (out / (name + ".stderr.log")).write_text("".join(process.stderr_lines), encoding="utf-8")
            if expected_terminal is not None:
                check_trace(record, process, expected_terminal)
    record["passed"] = bool(record["checks"]) and all(record["checks"].values()) and not record["forced_termination"]
    return record


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("out", type=Path, nargs='?', default=Path.cwd() / 'artifacts' / 'validation' / ('managed-rpc-' + str(uuid.uuid4())))
    parser.add_argument("--case", action="append", choices=[
        "eval-tostring", "eval-tostring-nonstring", "kag-debug-tostring", "kag-continue",
        "run-complete", "run-fail", "run-stop", "http-run-submitted",
    ])
    parser.add_argument("--include-http", action="store_true")
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    before = digest(binary)
    cases = args.case or ["eval-tostring", "eval-tostring-nonstring", "kag-debug-tostring", "kag-continue",
                          "run-complete", "run-fail", "run-stop"]
    if args.include_http and "http-run-submitted" not in cases:
        cases.append("http-run-submitted")
    report = {"binary": str(binary), "binary_sha256_before": before, "cases": []}
    for name in cases:
        record = execute_case(binary, out, name)
        report["cases"].append(record)
        print(json.dumps(record, ensure_ascii=False), flush=True)
    report["binary_sha256_after"] = digest(binary)
    report["binary_unchanged"] = before == report["binary_sha256_after"]
    report["passed"] = report["binary_unchanged"] and all(row["passed"] for row in report["cases"])
    (out / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print("MANAGED RPC PROBE " + ("PASS" if report["passed"] else "FAIL"), flush=True)
    if report['passed']: print('ALL MANAGED RPC TESTS PASSED', flush=True)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
