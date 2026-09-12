# Headless JSON-RPC integration test: exercises the real engine's stdio RPC
# transport end-to-end (ping / eval / managed-coroutine run / post-run state).
import json
import os
import subprocess
import queue
import threading
import sys
import time

exe = sys.argv[1] if len(sys.argv) > 1 else "CaesuraAmeKAG.exe"
cwd = os.path.dirname(os.path.abspath(exe)) or "."

proc = subprocess.Popen(
    [exe, "--headless"],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.DEVNULL,
    cwd=cwd,
    bufsize=1,
    text=True,
    encoding="utf-8",
    errors="replace",
)

results = []


# Singleton stdout reader: one daemon thread pumps readline() into a queue.
# A fresh reader thread per request would race for lines with the previous
# (still-blocked) reader and swallow responses -- the request timeout then
# fires even though the engine replied.
_line_queue = queue.Queue()

def _reader_loop():
    while True:
        line = proc.stdout.readline()
        if not line:
            _line_queue.put(None)  # EOF sentinel
            break
        _line_queue.put(line)

reader = threading.Thread(target=_reader_loop, daemon=True)
reader.start()

# The same reader owns stdout from startup through exit. A synchronous
# readline in this loop would not actually obey the startup deadline.
startup_deadline = time.monotonic() + 45.0
boot_line = None
while time.monotonic() < startup_deadline:
    try:
        line = _line_queue.get(timeout=max(.001, startup_deadline - time.monotonic()))
    except queue.Empty:
        break
    if line is None: break
    boot_line = line.strip()
    if "Backends ready" in line: break
if not boot_line or "Backends ready" not in boot_line:
    if proc.poll() is None: proc.kill()
    proc.wait(timeout=10)
    reader.join(timeout=3)
    proc.stdin.close(); proc.stdout.close()
    raise RuntimeError("engine did not reach 'Backends ready' within 45s "
                       "(last line: %r)" % (boot_line or "<none>"))

def request(req, timeout=20):
    proc.stdin.write(json.dumps(req) + "\n")
    proc.stdin.flush()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise RuntimeError("timeout waiting for response to " + json.dumps(req))
        try:
            line = _line_queue.get(timeout=min(remaining, 5.0))
        except queue.Empty:
            continue  # no line yet; keep polling within the deadline
        if line is None:
            raise RuntimeError("engine closed stdout")
        try:
            parsed = json.loads(line)
        except ValueError:
            continue  # engine banner / plain log lines that are not JSON
        if isinstance(parsed, dict) and "event" in parsed:
            continue  # {"event":"log",...} push lines are not responses
        if isinstance(parsed, dict) and parsed.get('id') == req['id']:
            return parsed
    raise RuntimeError("timeout waiting for response to " + json.dumps(req))


def check(name, condition):
    results.append((name, bool(condition)))
    print(("PASS " if condition else "FAIL ") + name)


try:
    r = request({"id": 1, "method": "ping"})
    check("ping", r.get("result") == "ok")

    r = request({"id": 2, "method": "eval", "code": "return 1 + 1"})
    check("eval-arithmetic", r.get("status") == "ok" and r.get("result") == "2")

    r = request({"id": 3, "method": "eval", "code": "error('boom')"})
    check("eval-error-propagates", "error" in r)

    # The first managed run may take longer on slow CI runners (the engine
    # must reach its per-frame pump loop before the run is serviced); give
    # it the same window as the boot deadline instead of the default 20s.
    r = request({"id": 4, "method": "run",
                 "script": "print('managed-run-start'); "
                           "coroutine.yield(); "
                           "print('managed-run-resumed')"}, timeout=45)
    check("run-started", r.get("status") == "ok")

    # Give the managed coroutine time to yield and be resumed across frames.
    time.sleep(1.5)

    r = request({"id": 5, "method": "eval", "code": "return 2 * 3"})
    check("eval-after-run", r.get("status") == "ok" and r.get("result") == "6")

    r = request({"id": 6, "method": "eval", "code": "return type(_G.print)"})
    check("vm-alive-after-run", r.get("status") == "ok" and r.get("result") == "function")

    r = request({"id": 7, "method": "run", "script": "error('run-failure')"})
    check("run-with-error-accepted", r.get("status") == "ok")

    time.sleep(0.5)
    r = request({"id": 8, "method": "eval", "code": "return 7 + 7"})
    check("eval-after-failing-run", r.get("status") == "ok" and r.get("result") == "14")

    # Round 29: JSON \u escapes must decode BMP and astral-plane (surrogate
    # pair) code points into correct UTF-8 on the live readJsonString path.
    r = request({"id": 8, "method": "eval", "code": "return 'é中😀'"})
    check("eval-unicode-escapes",
          r.get("status") == "ok" and r.get("result") == "é中😀")

    # ---- KAG scene-level debugger (Neo-Genesis) ---------------------------
    r = request({"id": 9, "method": "kagSetBreakpoint",
                 "params": {"scene": "assets/script/main.ks", "cmd": "ch"}})
    check("kag-set-breakpoint", r.get("status") == "ok" and r.get("result") == "true")

    r = request({"id": 10, "method": "kagSetBreakpoint",
                 "params": {"scene": "assets/script/main.ks", "line": 42}})
    check("kag-set-breakpoint-line", r.get("status") == "ok")

    r = request({"id": 11, "method": "kagSetBreakpoint",
                 "params": {"scene": "assets/script/main.ks"}})
    check("kag-bad-breakpoint-rejected", r.get("error") is not None)

    r = request({"id": 12, "method": "kagInspectScopes", "params": {"scope": "f"}})
    check("kag-inspect-scope", r.get("status") == "ok")

    r = request({"id": 13, "method": "kagInspectScopes"})
    check("kag-inspect-all", r.get("status") == "ok")

    r = request({"id": 14, "method": "kagDebugStep"})
    check("kag-debug-step", r.get("status") == "ok")

    r = request({"id": 15, "method": "kagDebugContinue"})
    check("kag-debug-continue", r.get("status") == "ok")

    r = request({"id": 16, "method": "kagClearBreakpoints"})
    check("kag-clear-breakpoints", r.get("status") == "ok")

    # ---- scene hot reload (editor workflow) --------------------------------
    r = request({"id": 17, "method": "kagReloadScene",
                 "params": {"scene": "assets/script/__missing__.ks"}})
    check("kag-reload-scene-missing-graceful",
        r.get("status") == "ok" and "error" in (r.get("result") or ""))
    r = request({"id": 18, "method": "kagReloadScene"})
    check("kag-reload-scene-no-scene-graceful",
        r.get("status") == "ok")

    # ---- Round 57: LSP endpoints against the contract registry -----------
    # The editor's lspCall() bridges to kag.lsp.json() via /api/eval;
    # validate completion / hover / diagnostics for the round-51 contract
    # commands (schema-driven, same registry ks_check uses).
    _lsp_id = [19]

    def lsp_json(method, *args):
        code = "local lsp = require('kag.lsp'); return lsp.json('%s'%s)" % (
            method, "".join(", " + json.dumps(a) for a in args))
        _lsp_id[0] += 1
        return request({"id": _lsp_id[0], "method": "eval", "code": code})

    r = lsp_json("hover", "blur", "duration")
    _b = r.get("result") or ""
    check("lsp-hover-blur-contract",
        r.get("status") == "ok" and '"title"' in _b
        and "type=number" in _b and "duration" in _b)

    r = lsp_json("hover", "fade", "duration")
    _b = r.get("result") or ""
    check("lsp-hover-fade-contract",
        r.get("status") == "ok" and "type=number" in _b)

    r = lsp_json("hover", "unlock", "type")
    _b = r.get("result") or ""
    check("lsp-hover-unlock-param",
        r.get("status") == "ok" and "type=" in _b)

    r = lsp_json("hover", "saveload", "mode")
    _b = r.get("result") or ""
    check("lsp-hover-saveload-mode",
        r.get("status") == "ok" and "mode" in _b and "type=" in _b)

    r = lsp_json("hover", "select")
    _b = r.get("result") or ""
    check("lsp-hover-select-params", r.get("status") == "ok" and "params:" in _b)

    r = lsp_json("completion", "[sav")
    _b = r.get("result") or ""
    check("lsp-completion-saveload", r.get("status") == "ok" and "saveload" in _b)

    r = lsp_json("completion", "[bl")
    _b = r.get("result") or ""
    check("lsp-completion-blur", r.get("status") == "ok" and "blur" in _b)

    r = lsp_json("completion", "[ch ")
    _b = r.get("result") or ""
    check("lsp-completion-ch-params", r.get("status") == "ok" and "text" in _b)

    r = lsp_json("diagnostics", '[ch name="A" text="x"]\n[if exp="f.hp >"]')
    _b = r.get("result") or ""
    check("lsp-diag-bad-expr-compile",
        r.get("status") == "ok" and "does not compile" in _b)

    r = lsp_json("diagnostics", "[playbgm vol=1]")
    _b = r.get("result") or ""
    check("lsp-diag-missing-anyof",
        r.get("status") == "ok" and "requires one of" in _b)

    r = lsp_json("diagnostics", "[blurrr oops=1]")
    _b = r.get("result") or ""
    check("lsp-diag-unknown-command",
        r.get("status") == "ok" and len(_b) > 2)

    # round 71: interpolation diagnostics — a bad ${expr} inside a text
    # param is flagged (Schema.checkInterp shared compile path).
    r = lsp_json("diagnostics", '[ch text="a ${bad &&}"]')
    _b = r.get("result") or ""
    check("lsp-diag-bad-interpolation",
        r.get("status") == "ok" and "interpolation" in _b)

    # ---- Round 71-74: math / choice / text-speed / resource contracts ----
    # (a) Math command result executed through the real schema-coerce +
    # dispatch path (identical to the scheduler's non-flow-control dispatch:
    # params = Schema.coerce(cmd, params, ctx); handler(ctx, params) -- then
    # read back via eval.
    #
    # [Sprint 1] eval runs with _ENV == the real _G, so anchoring the ctx with
    # rawset(_G, '_smokeMathCtx', ctx) created a permanently leaked engine
    # global by smuggling the write past the sandbox's __newindex guard -- the
    # very escape that hardening closed (the old "(permitted)" note described
    # the bypass, not a sanctioned API). The ctx is now LOCAL in every request,
    # and the cross-request read goes through the engine's own persistence
    # (KAG.save_game / KAG.load_game -> SaveManager), the mechanism a real game
    # uses to carry a ctx across a boundary. Probe slot 14 is deleted after.
    r = request({"id": 100, "method": "eval", "code":
        "local kag=require('kag'); local s=require('kag.schema'); "
        "local ctx={f={},sf={},tf={},mp={},lf={}}; "
        "local p=s.coerce('add',{name='f.x',value=5},ctx); kag.add(ctx,p); "
        "local ok=KAG.save_game(14, ctx, 'assets/script/main.ks', 4, ''); "
        "if not ok then return 'unsaved' end; "
        "return tostring(ctx.f.x)"})
    check("math-add-executes",
        r.get("status") == "ok" and r.get("result") == "5")

    r = request({"id": 101, "method": "eval", "code":
        "local data = KAG.load_game(14); "
        "if not data then return 'absent' end; "
        "return tostring(data.f and data.f.x)"})
    check("math-add-read-via-eval",
        r.get("status") == "ok" and r.get("result") == "5")

    r = request({"id": 199, "method": "eval", "code":
        "return tostring(KAG.delete_save(14))"})
    check("math-probe-save-cleanup",
        r.get("status") == "ok" and r.get("result") == "true")

    # The rest of the math family shares the same coerce + dispatch path.
    r = request({"id": 102, "method": "eval", "code":
        "local kag=require('kag'); local s=require('kag.schema'); "
        "local ctx={f={},sf={},tf={},mp={},lf={}}; "
        "local function run(cmd, params) "
        "  local p=s.coerce(cmd, params or {}, ctx); kag[cmd](ctx,p) end "
        "ctx.f.n=10; "
        "run('mul',{name='f.n',value=3}); "
        "run('div',{name='f.n',value=2}); "
        "run('dec',{name='f.n',amount=3}); "
        "run('mod',{name='f.n',value=5}); "
        "run('sub',{name='f.n',value=1}); "
        "return tostring(ctx.f.n)"})
    check("math-family-mul-div-dec-mod-sub",
        r.get("status") == "ok" and r.get("result") == "1.0")

    # div/mod by zero print a visible error and no-op (no zero-division crash).
    r = request({"id": 103, "method": "eval", "code":
        "local kag=require('kag'); local s=require('kag.schema'); "
        "local ctx={f={},sf={},tf={},mp={},lf={}}; ctx.f.n=7; "
        "local p=s.coerce('div',{name='f.n',value=0},ctx); kag.div(ctx,p); "
        "return tostring(ctx.f.n)"})
    check("math-div-zero-noop",
        r.get("status") == "ok" and r.get("result") == "7")

    # (b) [sel]/[button] x= capture registration. KAG3 x="tf.choice" declares
    # the variable [endbutton] writes the chosen target into on selection;
    # the reachable non-blocking surface holds the x= slot on the choice.
    r = request({"id": 104, "method": "eval", "code":
        "local kag=require('kag'); "
        "local ctx={f={},sf={},tf={},mp={},lf={},_choiceButtons={}}; "
        "kag.button(ctx,{text='Go',target='*route_a',x='tf.choice'}); "
        "local b=ctx._choiceButtons and ctx._choiceButtons[1]; "
        "if not b then return 'no-choice' end; "
        "return tostring(#ctx._choiceButtons)..':'..tostring(b.target)..':'..tostring(b.x)"})
    check("sel-button-x-capture-registered",
        r.get("status") == "ok" and r.get("result") == "1:*route_a:tf.choice")

    # (c) Save slot list / load roundtrip through the KAG C++ bindings
    # (SaveManager) exposed via eval. A high probe slot is cleaned up after.
    r = request({"id": 105, "method": "eval", "code":
        "return type(KAG.list_saves)"})
    check("save-list-binding",
        r.get("status") == "ok" and r.get("result") == "function")

    r = request({"id": 106, "method": "eval", "code":
        "local ok = KAG.save_game(13, {f={probe=77}}, "
        "  'assets/script/main.ks', 4, ''); return ok and 'true' or 'false'"})
    check("save-slot-write",
        r.get("status") == "ok" and r.get("result") == "true")

    r = request({"id": 107, "method": "eval", "code":
        "local s=KAG.list_saves(); local hit=nil; "
        "for _,e in ipairs(s) do if e.slot==13 then hit=e end end; "
        "if not hit then return 'absent' end; "
        "return tostring(hit.slot)..':'..tostring(hit.scene)..':'..tostring(hit.token_index)"})
    check("save-slot-list-reflects",
        r.get("status") == "ok" and r.get("result") == "13:assets/script/main.ks:4")

    r = request({"id": 108, "method": "eval", "code":
        "local data, meta = KAG.load_game(13); "
        "if not data then return 'nil' end; "
        "return tostring(data.f and data.f.probe)"})
    check("load-slot-roundtrip",
        r.get("status") == "ok" and r.get("result") == "77")

    r = request({"id": 109, "method": "eval", "code":
        "return tostring(KAG.delete_save(13))"})
    check("save-slot-cleanup",
        r.get("status") == "ok" and r.get("result") == "true")

    # (d) Unknown-command diagnostic carries a concrete severity + message
    # (extending the round-57 length-only assertion).
    r = lsp_json("diagnostics", "[zzz oops=1]")
    _b = r.get("result") or ""
    check("lsp-diag-unknown-command-severity",
        r.get("status") == "ok" and '"severity":2' in _b
        and "unknown KAG command" in _b)

    # (e) The contract registry reflects the round 71-74 commands. The
    # schema registry is the authoritative contract count (assert a >=
    # baseline instead of an exact count so future migrations stay green);
    # the new commands are also checked individually for contract + handler.
    r = request({"id": 110, "method": "eval", "code":
        "return tostring(require('kag.schema').registrySize())"})
    _contract_n = 0
    try:
        _contract_n = int(r.get("result") or 0)
    except ValueError:
        _contract_n = 0
    check("contract-count-grows",
        r.get("status") == "ok" and _contract_n >= 84)

    r = request({"id": 111, "method": "eval", "code":
        "local s=require('kag.schema'); local kag=require('kag'); "
        "local need={'add','csp','textspeed','palette','preload','button'}; "
        "local ok=true; for _,c in ipairs(need) do "
        "  if not (s.isMigrated(c) and type(kag[c])=='function') then ok=false end end; "
        "return ok and 'true' or 'false'"})
    check("round71-74-contracts-present",
        r.get("status") == "ok" and r.get("result") == "true")

    # stats RPC stays healthy with the new contract surface loaded.
    r = request({"id": 112, "method": "stats"})
    _st = r.get("stats") or {}
    check("stats-reflects-engine",
        isinstance(_st.get("texture_budget_mb"), int)
        and isinstance(_st.get("job_workers"), int))

finally:
    try:
        proc.stdin.close()
    except Exception:
        pass
    try:
        proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        check("natural-eof-exit-without-kill", False)
        proc.kill()
        proc.wait(timeout=10)
    reader.join(timeout=3)
    check("natural-eof-exit", proc.returncode == 0 and not reader.is_alive())
    proc.stdout.close()

failed = [name for name, ok in results if not ok]
if failed:
    print("FAILED: " + ", ".join(failed))
    sys.exit(1)
print("ALL HEADLESS RPC TESTS PASSED")
