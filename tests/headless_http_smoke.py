# Headless HTTP editor smoke test: exercises the real engine's HTTP editor
# transport end-to-end (ping / eval / getState / breakpoint lifecycle /
# continue / inspect reachability) plus the Live2D model-load route.
import json
import argparse
from contextlib import contextmanager
from dataclasses import asdict
import hashlib
import os
from pathlib import Path
import re
import sys
import time
import urllib.error
import urllib.request
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from native_package_runtime import _execute, _reserve_editor_port
from package_runtime import RuntimeContractError, process_identity, verify_owned_listener

exe = cwd = BASE = None
port = proc = None
# [Sprint 1 t4] The HTTP editor is default-deny: it now requires a bearer
# token and generates one when none is configured. The smoke test supplies
# its own token so it does not have to read the generated one back, and sends
# it on every request.
TOKEN = "headless-http-smoke-token"
results = []


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        # Every observation is bound to the selected Engine listener. Do not
        # forward even the disposable token to an unchecked redirect target.
        return None


_opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), _NoRedirect())


class _ObservedEngine:
    def __init__(self, identity):
        self.identity = identity

    def poll(self):
        # Only distinguishes live from unavailable. The owned runner records
        # the real OS exit code; do not invent one from a failed observation.
        try:
            return None if process_identity(self.identity.pid) == self.identity else "identity changed"
        except RuntimeContractError:
            return "exited"


@contextmanager
def _open(request, timeout):
    verify_owned_listener(proc.identity, port)
    try:
        try:
            response = _opener.open(request, timeout=timeout)
        except urllib.error.HTTPError as error:
            # Error status/body belongs to the same owned response lifecycle.
            response = error
        with response:
            yield response
    finally:
        verify_owned_listener(proc.identity, port)


def _repo_root_for_import():
    # The test's source anchor also covers presets and out-of-tree builds,
    # whose executable may be more than two parents below (or outside) src/.
    source = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if os.path.isdir(os.path.join(source, "src")):
        return source
    # Keep executable-relative lookup for staged standalone hosts.
    candidate = os.path.abspath(cwd)
    while True:
        if os.path.isdir(os.path.join(candidate, "src")):
            return candidate
        parent = os.path.dirname(candidate)
        if parent == candidate:
            break
        candidate = parent
    return os.path.abspath(cwd)


def check(name, ok, detail=""):
    results.append((name, bool(ok)))
    if not ok:
        print("FAIL", name, detail)


def request(path, data=None, timeout=30, headers=None):
    hdrs = {"Content-Type": "application/json",
            "Authorization": "Bearer " + TOKEN}
    if headers:
        hdrs.update(headers)
    req = urllib.request.Request(
        BASE + path, data=data, headers=hdrs,
    )
    try:
        with _open(req, timeout=timeout) as r:
            body = r.read().decode("utf-8", errors="replace")
            try:
                return r.status, json.loads(body)
            except Exception:
                return r.status, {"raw": body[:200]}
    except Exception as e:
        return -1, {"error": str(e)}


def main():
    # Wait for the HTTP server to come up.
    ready = False
    for _ in range(80):
        if proc.poll() is not None:
            break
        try:
            _probe = urllib.request.Request(
                BASE + "/api/ping",
                headers={"Authorization": "Bearer " + TOKEN})
            with _open(_probe, timeout=1) as r:
                if r.status == 200:
                    ready = True
                    break
        except RuntimeContractError:
            # No listener yet is normal startup; any observed foreign listener
            # or changed process must not be retried or contacted.
            from package_runtime import loopback_listeners
            if loopback_listeners(port) or proc.poll() is not None:
                raise
            time.sleep(0.5)
        except Exception:
            time.sleep(0.5)
    check("server-ready", ready)
    if not ready:
        print("HTTP smoke: editor startup did not provide an owned HTTP endpoint; see retained Engine logs")
        finish(1)

    st, resp = request("/api/ping")
    check("ping", st == 200 and resp.get("status") == "ok", "%s %s" % (st, resp))

    st, resp = request("/api/status")
    check("status", st == 200 and resp.get("status") == "ok", "%s %s" % (st, resp))

    st, resp = request("/api/eval", data=b"return 6 * 7")
    check("eval", st == 200 and resp.get("result") == "42", "%s %s" % (st, resp))

    st, resp = request("/api/eval", data=b"error('boom')")
    check("eval-error-surfaced", st == 500 and "boom" in str(resp), "%s %s" % (st, resp))

    st, resp = request("/api/debug/getState")
    check("getState", st == 200 and resp.get("status") == "ok", "%s %s" % (st, resp))

    # Canonical /api/state (IDE preview panel): typed fields present.
    st, resp = request("/api/state")
    check("state-endpoint", st == 200 and resp.get("status") == "ok"
          and isinstance(resp.get("scene"), str)
          and isinstance(resp.get("token_index"), int)
          and isinstance(resp.get("language"), str)
          and isinstance(resp.get("backlog_count"), int)
          and isinstance(resp.get("layer_count"), int),
          "%s %s" % (st, resp))

    # /api/pick (round 23): hit-test returns JSON hits array text.
    st, resp = request("/api/pick?x=640&y=360")
    check("pick-endpoint", st == 200 and resp.get("status") == "ok"
          and isinstance(resp.get("hits"), str) and "[" in resp.get("hits", ""),
          "%s %s" % (st, resp))
    st, resp = request("/api/pick?x=-1&y=0")
    check("pick-invalid-coords", st == 400, "%s %s" % (st, resp))

    # /api/stats: engine runtime statistics. Status ok and every numeric
    # budget/counter field must come back as an int (typed assertions).
    st, resp = request("/api/stats")
    check("stats-endpoint", st == 200 and resp.get("status") == "ok"
          and isinstance(resp.get("texture_budget_mb"), int)
          and isinstance(resp.get("texture_tier"), int)
          and isinstance(resp.get("texture_tier_name"), str)
          and isinstance(resp.get("mesh_count"), int)
          and isinstance(resp.get("job_workers"), int)
          and isinstance(resp.get("job_pending"), int)
          and isinstance(resp.get("lua_kb"), int),
          "%s %s" % (st, resp))

    # /api/sma/validate (round 19): valid asset ok, broken asset lists
    # field-located violations, unsafe paths rejected.
    st, resp = request("/api/sma/validate?path=demo/assets/sma/hero.json")
    check("sma-validate-hero", st == 200 and resp.get("status") == "ok"
          and resp.get("ok") is True and resp.get("errors") == []
          and isinstance(resp.get("meta"), str) and "bones" in resp.get("meta", "")
          and "boneTree" in resp.get("meta", "") and "animDetails" in resp.get("meta", ""),
          "%s %s" % (st, resp))
    st, resp = request("/api/sma/validate?path=demo/assets/sma/_broken_example.json")
    check("sma-validate-broken", st == 200 and resp.get("status") == "ok"
          and resp.get("ok") is False
          and len(resp.get("errors", [])) >= 4
          and any("undefined bone" in e for e in resp.get("errors", [])),
          "%s %s" % (st, resp))
    st, resp = request("/api/sma/validate?path=../../outside.json")
    check("sma-validate-unsafe-path", st == 400, "%s %s" % (st, resp))
    st, resp = request("/api/sma/validate?path=demo/assets/sma/missing.json")
    check("sma-validate-missing-file", st == 200 and resp.get("ok") is False
          and resp.get("errors") and "cannot open" in resp.get("errors", [])[0],
          "%s %s" % (st, resp))

    # /api/sma/save (round 26): POST {path, content} — shared validator
    # (sma_check.validate_text) gates the write. Good content round-trips
    # unchanged (ok:true, no errors); broken JSON is rejected with errors;
    # unsafe paths (non-assets prefix / "..") return 400.
    hero_path = os.path.join(cwd, "demo", "assets", "sma", "hero.json")
    hero_text = ""
    try:
        with open(hero_path, "r", encoding="utf-8") as hf:
            hero_text = hf.read()
    except Exception as e:
        hero_text = ""
    check("sma-save-source-readable", bool(hero_text.strip()), "reading %s: %r" % (hero_path, hero_text[:60]))

    st, resp = request("/api/sma/save",
                       data=json.dumps({"path": "demo/assets/sma/hero.json",
                                        "content": hero_text}).encode())
    check("sma-save-ok", st == 200 and resp.get("status") == "ok"
          and resp.get("ok") is True and resp.get("errors") == [], "%s %s" % (st, resp))

    st, resp = request("/api/sma/save",
                       data=json.dumps({"path": "demo/assets/sma/hero.json",
                                        "content": '{"bones":[]}'}).encode())
    check("sma-save-invalid-json-rejected", st == 200 and resp.get("status") == "ok"
          and resp.get("ok") is False and resp.get("errors"), "%s %s" % (st, resp))

    st, resp = request("/api/sma/save",
                       data=json.dumps({"path": "../../evil.json",
                                        "content": '{"bones":[]}'}).encode())
    check("sma-save-unsafe-path", st == 400, "%s %s" % (st, resp))

    # Rejected: line must be a positive int32. 4294967297 (2^32+1) previously
    # wrapped to a positive int (line 1) via unchecked get<int>(), silently
    # setting a wrong breakpoint.
    st, resp = request("/api/debug/setBreakpoint",
                       data=json.dumps({"source": "demo_story.ks", "line": 0}).encode())
    check("setBreakpoint-line0-rejected", st == 400, "%s %s" % (st, resp))
    st, resp = request("/api/debug/setBreakpoint",
                       data=json.dumps({"source": "demo_story.ks",
                                        "line": 4294967297}).encode())
    check("setBreakpoint-overflow-rejected", st == 400, "%s %s" % (st, resp))

    st, resp = request("/api/debug/setBreakpoint",
                       data=json.dumps({"source": "demo_story.ks", "line": 10}).encode())
    check("setBreakpoint", st == 200 and resp.get("status") == "ok", "%s %s" % (st, resp))

    st, resp = request("/api/debug/removeBreakpoint",
                       data=json.dumps({"source": "demo_story.ks", "line": 10}).encode())
    check("removeBreakpoint", st == 200 and resp.get("status") == "ok", "%s %s" % (st, resp))

    st, resp = request("/api/debug/clearBreakpoints", data=b"")
    check("clearBreakpoints", st == 200 and resp.get("status") == "ok", "%s %s" % (st, resp))

    # continue/inspect without an active pause: must produce a semantic JSON
    # error (not a crash / empty body).
    st, resp = request("/api/debug/continue", data=b"{}")
    check("continue-reachable", st == 400 and "stale_pause" in str(resp), "%s %s" % (st, resp))

    st, resp = request("/api/debug/inspect?name=x&global=1")
    check("inspect-reachable", st == 400 and "inspection_unavailable" in str(resp),
          "%s %s" % (st, resp))

    # --editor runs with GPU enabled, so a real frame capture may succeed
    # (200 + base64) or fail (500 + capture_failed); either is a valid answer.
    st, resp = request("/api/debug/getFrame?w=320&h=240")
    ok_frame = (st == 200 and isinstance(resp.get("base64"), str) and resp.get("base64"))
    ok_frame = ok_frame or (st == 500 and "capture_failed" in str(resp))
    check("getFrame-reachable", ok_frame, "%s %s" % (st, resp))

    # Live2D model-load route must respond (Haru lives under live2d_test in
    # the build output; load may fail if the file is absent, but the route
    # must answer with a JSON error, never hang or crash).
    st, resp = request("/api/live2d/load",
                       data=json.dumps({"modelPath": "live2d_test/Haru/Haru.model3.json",
                                        "x": 0, "y": 0, "scale": 1, "show": True}).encode(),
                       timeout=40)
    check("live2d-load-route", st in (200, 500) and "raw" not in resp, "%s %s" % (st, resp))

    # ---- Round 71-79 command surface (over HTTP /api/eval) --------------
    # The HTTP editor exposes the same Lua eval dispatch as the stdio RPC
    # (POST /api/eval: raw Lua body -> {status, result}), so the round-75
    # schema.coerce + handler patterns run unchanged over HTTP (the ctx is
    # local per request and persisted through the save layer when a later
    # request must read it). There is no dedicated save / math / textspeed HTTP name --
    # the generic eval route is the reachable surface for these commands.
    # (Save slot I/O goes through the KAG C++ bindings over the same eval
    # bridge; /api/stats stays the first-class health route.)

    # (a) Math command chain via schema.coerce + handler, then read f.x back in
    # a LATER request.
    #
    # [Sprint 1] /api/eval runs with _ENV == the real _G, so the ctx used to be
    # anchored with rawset(_G, '_smokeHttpMathCtx', ctx): a genuinely new,
    # permanently leaked engine global, written by smuggling the assignment past
    # the sandbox's __newindex guard. That bypass is closed, and the old comment
    # calling it "(permitted)" described exactly the escape being fixed. The ctx
    # is now LOCAL in every request, and the cross-request read goes through the
    # engine's own persistence (KAG.save_game / KAG.load_game -> SaveManager),
    # which is the mechanism a real game uses to carry a ctx across a boundary.
    # Probe slot 24 is deleted at the end of the chain.
    st, resp = request("/api/eval", data=b"local kag=require('kag'); local s=require('kag.schema'); "
        b"local ctx={f={},sf={},tf={},mp={},lf={}}; "
        b"local p=s.coerce('add',{name='f.x',value=5},ctx); kag.add(ctx,p); "
        b"local ok=KAG.save_game(24, ctx, 'assets/script/main.ks', 4, ''); "
        b"if not ok then return 'unsaved' end; "
        b"return tostring(ctx.f.x)")
    check("http-math-add-executes",
        st == 200 and resp.get("result") == "5", "%s %s" % (st, resp))

    st, resp = request("/api/eval", data=b"local kag=require('kag'); local s=require('kag.schema'); "
        b"local ctx={f={},sf={},tf={},mp={},lf={}}; "
        b"local function run(c,p) local q=s.coerce(c,p or {},ctx); kag[c](ctx,q) end "
        b"ctx.f.n=10; run('mul',{name='f.n',value=3}); run('div',{name='f.n',value=2}); "
        b"run('dec',{name='f.n',amount=3}); run('mod',{name='f.n',value=5}); run('sub',{name='f.n',value=1}); "
        b"return tostring(ctx.f.n)")
    check("http-math-family-mul-div-dec-mod-sub",
        st == 200 and resp.get("result") == "1.0", "%s %s" % (st, resp))

    # Cross-request read: the ctx the FIRST eval computed and persisted is read
    # back in this separate request through the save layer.
    st, resp = request("/api/eval",
        data=b"local data = KAG.load_game(24); "
              b"if not data then return 'absent' end; "
              b"return tostring(data.f and data.f.x)")
    check("http-math-read-via-eval",
        st == 200 and resp.get("result") == "5", "%s %s" % (st, resp))

    st, resp = request("/api/eval", data=b"return tostring(KAG.delete_save(24))")
    check("http-math-probe-save-cleanup",
        st == 200 and resp.get("result") == "true", "%s %s" % (st, resp))

    # (b) The contract registry reflects the round 71-79 commands over HTTP.
    # registrySize() is authoritative (assert >= 118 -- the exact current
    # count is 118 -- so future migrations stay green); the newest commands
    # are also checked individually for contract + handler.
    st, resp = request("/api/eval", data=b"return tostring(require('kag.schema').registrySize())")
    _http_contracts = 0
    try:
        _http_contracts = int(resp.get("result") or 0)
    except (TypeError, ValueError):
        _http_contracts = 0
    check("http-contract-count",
        st == 200 and _http_contracts >= 118, "%s %s" % (st, resp))

    st, resp = request("/api/eval", data=b"local s=require('kag.schema'); local kag=require('kag'); "
        b"local need={'add','mul','mod','textspeed','palette','notify','i18n'}; "
        b"local ok=true; for _,c in ipairs(need) do "
        b"  if not (s.isMigrated(c) and type(kag[c])=='function') then ok=false end end; "
        b"return ok and 'true' or 'false'")
    check("http-round79-contracts-present",
        st == 200 and resp.get("result") == "true", "%s %s" % (st, resp))

    # (c) /api/stats stays healthy with the round-71-79 surface loaded
    # (the earlier stats-endpoint check already typed the numeric fields;
    # this re-probes the route after the new command surface ran).
    st, resp = request("/api/stats")
    check("http-stats-healthy", st == 200 and resp.get("status") == "ok"
          and isinstance((resp or {}).get("lua_kb"), int),
          "%s %s" % (st, resp))

    # (d) Save slot write/list/load/delete roundtrip over HTTP via the KAG
    # C++ bindings (SaveManager) reachable through /api/eval. The high probe
    # slot is cleaned up after.
    st, resp = request("/api/eval", data=b"return type(KAG.list_saves)")
    check("http-save-list-binding",
        st == 200 and resp.get("result") == "function", "%s %s" % (st, resp))

    st, resp = request("/api/eval", data=b"local ok = KAG.save_game(23, {f={probe=99}}, "
        b"  'assets/script/main.ks', 4, ''); return ok and 'true' or 'false'")
    check("http-save-slot-write",
        st == 200 and resp.get("result") == "true", "%s %s" % (st, resp))

    st, resp = request("/api/eval", data=b"local s=KAG.list_saves(); local hit=nil; "
        b"for _,e in ipairs(s) do if e.slot==23 then hit=e end end; "
        b"if not hit then return 'absent' end; "
        b"return tostring(hit.slot)..':'..tostring(hit.scene)..':'..tostring(hit.token_index)")
    check("http-save-slot-list-reflects",
        st == 200 and resp.get("result") == "23:assets/script/main.ks:4",
        "%s %s" % (st, resp))

    st, resp = request("/api/eval", data=b"local data, meta = KAG.load_game(23); "
        b"if not data then return 'nil' end; return tostring(data.f and data.f.probe)")
    check("http-load-slot-roundtrip",
        st == 200 and resp.get("result") == "99", "%s %s" % (st, resp))

    st, resp = request("/api/eval", data=b"return tostring(KAG.delete_save(23))")
    check("http-save-slot-cleanup",
        st == 200 and resp.get("result") == "true", "%s %s" % (st, resp))

    # (e) New-command dispatch probes: [notify] degrades headless-safe (no
    # raise), and [i18n] with a missing required param is a schema error
    # surfaced through the shared schema.coerce path.
    st, resp = request("/api/eval", data=b"local kag=require('kag'); local s=require('kag.schema'); "
        b"local ctx={f={},sf={},tf={},mp={},lf={}}; "
        b"local ok=pcall(function() local p=s.coerce('notify',{msg='hi'},ctx); kag.notify(ctx,p) end); "
        b"return ok and 'true' or 'false'")
    check("http-notify-degrade-safe",
        st == 200 and resp.get("result") == "true", "%s %s" % (st, resp))

    st, resp = request("/api/eval", data=b"local s=require('kag.schema'); "
        b"local ok,err=pcall(function() s.coerce('i18n',{}) end); "
        b"if ok then return 'no-error' end; "
        b"return (err and string.find(tostring(err),'language') and 'true') or 'false'")
    check("http-i18n-missing-param",
        st == 200 and resp.get("result") == "true", "%s %s" % (st, resp))

    # CORS: only localhost/127.0.0.1 origins are allowed.
    st, resp = request("/api/status", headers={"Origin": "http://evil.example.com"})
    check("cors-evil-origin-rejected", st == 403, "%s %s" % (st, resp))
    st, resp = request("/api/status", headers={"Origin": "http://localhost.evil.com"})
    check("cors-evil-subdomain-rejected", st == 403, "%s %s" % (st, resp))
    # Short / malformed Origin headers must not crash the editor.
    st, resp = request("/api/status", headers={"Origin": "http:/"})
    check("cors-short-origin-no-crash", st == 403, "%s %s" % (st, resp))
    st, resp = request("/api/status", headers={"Origin": "http://localhost:5173"})
    check("cors-localhost-allowed", st == 200, "%s %s" % (st, resp))
    # [Sprint 1 t4] Auth is default-deny: a request WITHOUT the bearer token
    # must be refused even though the editor binds to loopback only.
    _anon = urllib.request.Request(BASE + "/api/status",
                                   headers={"Content-Type": "application/json"})
    try:
        with _open(_anon, timeout=10) as r:
            _anon_status = r.status
            r.read()
    except Exception:
        _anon_status = -1
    check("anonymous-request-rejected", _anon_status == 401, str(_anon_status))
    # The configured token opens the gate.
    st, resp = request("/api/status")
    check("token-authorized", st == 200, "%s %s" % (st, resp))

    # ---- Project Manager endpoints (Sprint 2, task book §6.3) ----
    # GET /api/project/templates -> 5 templates in manifest order.
    st, resp = request("/api/project/templates")
    names = [t["id"] for t in resp] if isinstance(resp, list) else []
    check("project-templates-list",
          st == 200 and len(names) == 5
          and all(x in names for x in ("blank","basic","live2d","kag3","showcase")),
          "%s %s" % (st, resp))

    # POST /api/project/create with default template -> ok + path under projects/.
    import time as _t
    pname = "smoke_%d" % int(_t.time())
    st, resp = request("/api/project/create",
                       data=json.dumps({"template": "basic", "name": pname}).encode())
    check("project-create-ok",
          st == 200 and resp.get("ok") is True and str(resp.get("path","")).endswith(pname),
          "%s %s" % (st, resp))

    # Malformed name (spaces / slashes / path separators) -> 400.
    st, resp = request("/api/project/create",
                       data=json.dumps({"template": "basic", "name": "bad name"}).encode())
    check("project-create-invalid-name", st == 400, "%s %s" % (st, resp))
    st, resp = request("/api/project/create",
                       data=json.dumps({"template": "basic", "name": "a/../b"}).encode())
    check("project-create-path-traversal", st == 400, "%s %s" % (st, resp))

    # Unknown template -> 400.
    st, resp = request("/api/project/create",
                       data=json.dumps({"template": "nope", "name": "x1"}).encode())
    check("project-create-unknown-template", st == 400, "%s %s" % (st, resp))

    # Duplicate the created project.
    st, resp = request("/api/project/duplicate",
                       data=json.dumps({"srcPath": pname, "name": pname + "_copy"}).encode())
    check("project-duplicate-ok",
          st == 200 and resp.get("ok") is True
          and str(resp.get("path","")).endswith(pname + "_copy"),
          "%s %s" % (st, resp))

    # Duplicate of a nonexistent source -> 404.
    st, resp = request("/api/project/duplicate",
                       data=json.dumps({"srcPath": "does_not_exist", "name": "z9"}).encode())
    check("project-duplicate-missing-src", st == 404, "%s %s" % (st, resp))

    # Import an existing on-disk directory (absolute path outside projects/).
    # [Sprint 1 t4] Import sources are confined to the allowed roots
    # (projects/, the engine source root, CAESURA_EDITOR_IMPORT_ROOTS), so the
    # fixture lives inside the repo instead of the system temp directory.
    import tempfile
    _import_root = os.path.join(_repo_root_for_import(), "tmp")
    os.makedirs(_import_root, exist_ok=True)
    src_tmp = tempfile.mkdtemp(prefix="caesura_import_", dir=_import_root)
    with open(os.path.join(src_tmp, "story.ks"), "w", encoding="utf-8") as f:
        f.write("; imported by headless_http_smoke\n[p]")
    empty_tmp = tempfile.mkdtemp(prefix="caesura_empty_", dir=_import_root)
    iname = "smoke_import_%d" % int(_t.time())
    st, resp = request("/api/project/import",
                       data=json.dumps({"srcPath": src_tmp, "name": iname}).encode())
    check("project-import-ok",
          st == 200 and resp.get("ok") is True
          and str(resp.get("path","")).endswith(iname),
          "%s %s" % (st, resp))

    # Invalid destination name -> 400.
    st, resp = request("/api/project/import",
                       data=json.dumps({"srcPath": src_tmp, "name": "bad name"}).encode())
    check("project-import-invalid-name", st == 400, "%s %s" % (st, resp))

    # Nonexistent source directory -> 404.
    st, resp = request("/api/project/import",
                       data=json.dumps({"srcPath": os.path.join(src_tmp, "nope"),
                                        "name": "z8"}).encode())
    check("project-import-missing-src", st == 404, "%s %s" % (st, resp))

    # [Sprint 1 t4] A source OUTSIDE the allowed import roots is refused with
    # 403 -- /api/project/import must not be an arbitrary-directory copy.
    _outside = tempfile.mkdtemp(prefix="caesura_outside_")
    with open(os.path.join(_outside, "story.ks"), "w", encoding="utf-8") as f:
        f.write("; outside root\n[p]")
    st, resp = request("/api/project/import",
                       data=json.dumps({"srcPath": _outside,
                                        "name": "z6"}).encode())
    check("project-import-outside-root-rejected", st == 403, "%s %s" % (st, resp))
    import shutil as _sh_outside
    _sh_outside.rmtree(_outside, ignore_errors=True)

    # Source without a story entry point (story.ks/entry.lua) -> 400.
    st, resp = request("/api/project/import",
                       data=json.dumps({"srcPath": empty_tmp, "name": "z7"}).encode())
    check("project-import-no-story", st == 400, "%s %s" % (st, resp))

    # Re-import under an already-taken name -> 409.
    st, resp = request("/api/project/import",
                       data=json.dumps({"srcPath": src_tmp, "name": iname}).encode())
    check("project-import-duplicate-name", st == 409, "%s %s" % (st, resp))

    # GET /api/project/list reflects the created projects.
    st, resp = request("/api/project/list")
    listed = [p["name"] for p in resp] if isinstance(resp, list) else []
    check("project-list-reflects-created",
          st == 200 and pname in listed and (pname + "_copy") in listed,
          "%s %s" % (st, resp))
    check("project-list-reflects-imported",
          st == 200 and iname in listed,
          "%s %s" % (st, resp))

    # ---- Project metadata (PM settings, task book §6.3) ----
    # GET without caesura.project.json -> inferred defaults; POST writes
    # the file; GET then reflects the saved values. Runs against pname
    # created above; cleanup below removes the written file with the dir.
    st, resp = request("/api/project/meta?path=projects/" + pname)
    _m = resp.get("meta", {}) if isinstance(resp, dict) else {}
    # Templates now ship caesura.project.json (round 131), so a fresh
    # project may or may not be "inferred" -- both are valid; the contract
    # is ok=True with resolved name/language/version.
    check("project-meta-default",
          st == 200 and resp.get("ok") is True
          and _m.get("name") == pname and _m.get("language") == "zh"
          and _m.get("version") == "1.0" and isinstance(_m.get("created"), str),
          "%s %s" % (st, resp))

    st, resp = request("/api/project/meta",
                       data=json.dumps({"path": "projects/" + pname,
                                        "meta": {"language": "en",
                                                 "description": "smoke meta"}}).encode())
    _m = resp.get("meta", {}) if isinstance(resp, dict) else {}
    check("project-meta-save",
          st == 200 and resp.get("ok") is True
          and _m.get("language") == "en" and _m.get("description") == "smoke meta"
          and isinstance(_m.get("modified"), str) and bool(_m.get("modified")),
          "%s %s" % (st, resp))

    st, resp = request("/api/project/meta?path=projects/" + pname)
    _m = resp.get("meta", {}) if isinstance(resp, dict) else {}
    check("project-meta-get-reflects-save",
          st == 200 and resp.get("inferred") is False
          and _m.get("language") == "en" and _m.get("description") == "smoke meta"
          and _m.get("created"),
          "%s %s" % (st, resp))

    # Validation gates: closed language set, path traversal, missing param.
    st, resp = request("/api/project/meta",
                       data=json.dumps({"path": "projects/" + pname,
                                        "meta": {"language": "klingon"}}).encode())
    check("project-meta-invalid-language", st == 400, "%s %s" % (st, resp))

    st, resp = request("/api/project/meta?path=projects/../evil")
    check("project-meta-path-traversal", st == 400, "%s %s" % (st, resp))

    st, resp = request("/api/project/meta")
    check("project-meta-missing-param", st == 400, "%s %s" % (st, resp))

    st, resp = request("/api/project/meta?path=projects/no_such_project_zz")
    check("project-meta-missing-project", st == 404, "%s %s" % (st, resp))

    # Cleanup: remove created project dirs so the repo stays clean.
    for d in (pname, pname + "_copy", iname):
        try:
            import shutil
            shutil.rmtree(os.path.join(cwd, "projects", d), ignore_errors=True)
        except Exception:
            pass
    shutil.rmtree(src_tmp, ignore_errors=True)
    shutil.rmtree(empty_tmp, ignore_errors=True)

    # ---- Web packaging endpoint (POST /api/package/web, Sprint 6) ----
    # The engine wraps scripts/package_game.sh: whitelisted repo-relative
    # story paths (assets/ demo/ tests/projects/ projects/) are packaged
    # into dist/<outName>. The engine runs with cwd=build[/<config>] here, so a
    # success also proves the endpoint locates the repository root itself.
    import shutil as _shutil
    # package_game.sh can only assemble when web/node_modules/vite exists
    # (it rebuilds web/dist) or a prebuilt web/dist/index.html is present
    # (it reuses it); with NEITHER, the script fails BY DESIGN (step 3
    # honest degradation). Bare CI build jobs and fresh clones lack both --
    # assert only what the environment supports and SKIP loudly otherwise.
    # First seen: run 33232664396 macOS · Clang, where the audio fallback
    # un-skipped this smoke on mac for the first time and only these two
    # checks hit the gap (73/75). CAESURA_SMOKE_FORCE_NO_WEB=1 forces the
    # skip branch (detector for the guard itself).
    _smoke_repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    _web_ready = (os.path.isdir(os.path.join(_smoke_repo, "web", "node_modules", "vite"))
                  or os.path.isfile(os.path.join(_smoke_repo, "web", "dist", "index.html")))
    if os.environ.get("CAESURA_SMOKE_FORCE_NO_WEB") == "1":
        _web_ready = False
    if _web_ready:
        import uuid
        # Inspect only this invocation's output, never a prior smoke package.
        _pkg_name = "smoke_pkg_" + uuid.uuid4().hex
        st, resp = request("/api/package/web",
                           data=json.dumps({"storyPath": "demo/example_game/story.ks",
                                            "outName": _pkg_name}).encode(),
                           timeout=300)
        check("package-web-ok", st == 200 and resp.get("ok") is True
              and resp.get("outputDir") == "dist/" + _pkg_name
              and isinstance(resp.get("logTail"), str)
              and "PACKAGE COMPLETE" in resp.get("logTail", ""),
              "%s %s" % (st, str(resp)[:400]))

        # Real artifacts must exist on disk (static site + baked story bundle).
        # package_game.mjs resolves dist/ against its source root. Retain the
        # engine-CWD candidate for staged hosts, and use this test's source anchor.
        _repo_root = _repo_root_for_import()
        _pkg_candidates = [
            os.path.join(cwd, "dist", _pkg_name),
            os.path.join(_repo_root, "dist", _pkg_name),
        ]
        _pkg_dir = next((p for p in _pkg_candidates if os.path.isdir(p)),
                        _pkg_candidates[0])
        check("package-web-artifacts",
              os.path.isfile(os.path.join(_pkg_dir, "index.html"))
              and os.path.isfile(os.path.join(_pkg_dir, "MANIFEST.txt"))
              and os.path.isfile(os.path.join(_pkg_dir, "cache", "story", "story.lua")),
              _pkg_dir)
        _shutil.rmtree(_pkg_dir, ignore_errors=True)
    else:
        print("SKIP package-web-ok (web toolchain absent: neither "
              "web/node_modules/vite nor web/dist/index.html -- "
              "package_game.sh degrades by design)")
        print("SKIP package-web-artifacts (same precondition)")

    # Path traversal -> 400.
    st, resp = request("/api/package/web",
                       data=json.dumps({"storyPath": "../evil.ks"}).encode())
    check("package-web-traversal-rejected", st == 400, "%s %s" % (st, resp))

    # Absolute / drive path -> 400.
    st, resp = request("/api/package/web",
                       data=json.dumps({"storyPath": "C:/Windows/win.ini"}).encode())
    check("package-web-absolute-rejected", st == 400, "%s %s" % (st, resp))

    # Existing file OUTSIDE the whitelist -> 400.
    st, resp = request("/api/package/web",
                       data=json.dumps({"storyPath": "web/index.html"}).encode())
    check("package-web-outside-whitelist", st == 400, "%s %s" % (st, resp))

    time.sleep(1)
    check("engine-alive", proc.poll() is None)

    finish(0 if all(ok for _, ok in results) else 1)


def finish(rc):
    # The owned runner publishes the real exit code and reaps its process tree.
    raise SystemExit(rc)


def run_smoke(executable, output=None):
    global exe, cwd, BASE, port, proc
    exe = str(Path(executable).resolve(strict=True))
    cwd = str(Path(exe).parent)
    results.clear()
    output = Path(output) if output else Path(cwd) / "artifacts/validation" / ("http-smoke-" + uuid.uuid4().hex)
    output = output.absolute()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.mkdir()  # Never overwrite evidence from an earlier attempt.
    report = {"schema":"caesura.http-smoke.v1", "status":"FAIL", "requested_port":None,
              "process":None, "actual_exit_code":None, "forced_kill":False,
              "command":None, "checks":[], "errors":[]}
    reservation = None
    sources = [Path(exe), Path(__file__), ROOT / "scripts/native_package_runtime.py",
               ROOT / "scripts/package_runtime.py", ROOT / "scripts/validation_process.py",
               ROOT / "scripts/validation_sanitizer.py"]

    def inputs():
        return {str(path.resolve()):hashlib.sha256(path.read_bytes()).hexdigest() for path in sources}

    try:
        report["inputs_before"] = inputs()
        raw_port = os.environ.get("CAESURA_EDITOR_PORT")
        if raw_port is not None:
            if not re.fullmatch(r"[0-9]{1,5}", raw_port):
                raise RuntimeContractError("CAESURA_EDITOR_PORT must be an integer in 1..65535")
            report["requested_port"] = int(raw_port)
        reservation, endpoint = _reserve_editor_port(report["requested_port"])
        report["editor_endpoint"] = endpoint
        port = endpoint["port"]
        BASE = "http://127.0.0.1:" + str(port)
        environment = dict(os.environ, CAESURA_EDITOR_TOKEN=TOKEN, CAESURA_EDITOR_PORT=str(port))
        smoke_exit = None

        def exercise(identity, deadline):
            nonlocal smoke_exit
            global proc
            proc = _ObservedEngine(identity)
            report["process"] = asdict(identity)
            try:
                main()
            except SystemExit as error:
                smoke_exit = error.code
            return {"port":port, "checks":list(results), "smoke_exit_code":smoke_exit}

        reservation.close()
        reservation = None
        command = _execute(output, "http_smoke", [exe, "--editor"], Path(cwd), environment,
                           82, 42, monitor=exercise, controlled=True, editor_port=port)
        report["command"] = command
        run = command.get("run") or {}
        report.update(process=run.get("process"), actual_exit_code=run.get("actual_exit_code"),
                      forced_kill=run.get("forced_kill", False))
        report["errors"].extend(command["errors"])
        if smoke_exit != 0 or not results or not all(ok for _, ok in results):
            report["errors"].append("HTTP route checks did not complete successfully")
    except Exception as error:
        report["errors"].append(f"{type(error).__name__}: {error}")
    finally:
        if reservation is not None:
            reservation.close()
        report["checks"] = [{"name":name, "passed":ok} for name, ok in results]
        try:
            report["inputs_after"] = inputs()
            report["inputs_stable"] = report.get("inputs_before") == report["inputs_after"]
            if not report["inputs_stable"]:
                report["errors"].append("Engine or smoke controller inputs changed during execution")
        except OSError as error:
            report["errors"].append(f"Cannot verify original inputs: {error}")
        # Keep original owned-runner files, and convenient raw Engine log copies.
        report["logs"] = {}
        for channel in ("stdout", "stderr"):
            raw = output / "commands/http_smoke" / (channel + ".log")
            data = raw.read_bytes() if raw.is_file() else b""
            path = output / ("engine." + channel + ".log")
            path.write_bytes(data)
            report["logs"][channel] = {"path":path.name, "sha256":hashlib.sha256(data).hexdigest(), "size":len(data)}
        report["status"] = "FAIL" if report["errors"] else "PASS"
        temporary = output / ".result.json.writing"
        with temporary.open("x", encoding="utf-8") as stream:
            json.dump(report, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
        os.replace(temporary, output / "result.json")
    print("HTTP smoke: %d/%d passed" % (sum(ok for _, ok in results), len(results)))
    print("HTTP smoke evidence:", output / "result.json")
    if report["status"] == "PASS":
        print("ALL HEADLESS HTTP SMOKE TESTS PASSED")
        return 0
    print("HTTP smoke failed:", "; ".join(report["errors"]))
    for channel in ("stdout", "stderr"):
        text = (output / ("engine." + channel + ".log")).read_bytes()[-8192:].decode("utf-8", errors="replace")
        if text:
            print("Engine " + channel + " (last 8192 bytes):\n" + text)
    return 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Exercise real, owned Engine HTTP routes with retained startup evidence")
    parser.add_argument("executable", nargs="?", default="CaesuraAmeKAG.exe")
    parser.add_argument("--output", type=Path, help="New evidence directory; existing directories are refused")
    args = parser.parse_args()
    sys.exit(run_smoke(args.executable, args.output))
