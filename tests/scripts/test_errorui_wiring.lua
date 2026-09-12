-- test_errorui_wiring.lua — t212: runtime command error -> ErrorUI chain (G1/G2/G4)
-- Harness style: run_lua_tests.lua convention (check()/PASS/FAIL counters).
-- Suggested registration: tests/scripts/run_lua_tests.lua main suite (captain
-- wiring per task; this file is the new deliverable, registration deferred).
package.path = "scripts/?.lua;scripts/kag/?.lua;" .. package.path

local passed, failed = 0, 0
local function check(name, cond)
    if cond then print("PASS " .. name) passed = passed + 1
    else print("FAIL " .. name) failed = failed + 1 end
end

local kag_runner = require("kag_runner")

-- T1: install_error_handler installs a default on a fresh ctx
do
    local ctx = {}
    local h = kag_runner.install_error_handler(ctx)
    check("T1 default handler installed", type(h) == "function" and ctx.handle_error == h)
end

-- T2: first-definition-wins — a pre-set handler is kept untouched
do
    local custom = function() end
    local ctx = { handle_error = custom }
    local h = kag_runner.install_error_handler(ctx)
    check("T2 first-definition-wins", h == custom and ctx.handle_error == custom)
end

-- T3: default handler routes Engine.report_command_error(cmd, msg, scene, line)
--     with G4 traceback attached and Debug.log visibility (both stubs set)
do
    local engCalls, dbgCalls = {}, {}
    _G.Engine = { report_command_error = function(cmd, err, scene, line)
        engCalls[#engCalls + 1] = { cmd, err, scene, line } end }
    _G.Debug = { log = function(level, msg) dbgCalls[#dbgCalls + 1] = { level, msg } end }
    local ctx = {}
    kag_runner.install_error_handler(ctx)
    ctx.handle_error("ch", "boom: nil pointer", "assets/script/main.ks", 42)

    check("T3a Engine.report_command_error called once", #engCalls == 1)
    local c = engCalls[1] or {}
    check("T3b args: cmd/scene/line correct", c[1] == "ch" and c[3] == "assets/script/main.ks" and c[4] == 42)
    check("T3c G4 traceback attached to err", type(c[2]) == "string" and c[2]:find("stack traceback", 1, true) ~= nil)
    check("T3d Debug.log error entry present", #dbgCalls == 1 and dbgCalls[1][1] == "error"
          and dbgCalls[1][2]:find("[ErrorUI]", 1, true) ~= nil)
    _G.Engine, _G.Debug = nil, nil
end

-- T4: fallback — no Engine/Debug bindings: handler must not throw (print path)
do
    _G.Engine, _G.Debug = nil, nil
    local ctx = {}
    kag_runner.install_error_handler(ctx)
    local ok, err = pcall(ctx.handle_error, "assert", "assertion failed", "?", 7)
    check("T4 fallback without bindings does not throw", ok == true)
    _G.Engine, _G.Debug = nil, nil
end

-- T5: scheduler contract — the error hook arguments include scene + line and
--     the ctx stashes error_command/error_token_line (source contract lock;
--     the full dispatch path is exercised by the engine smoke run).
--     Static check: the installed default is what runtime errors reach, and
--     the scheduler block for handle_error passes 4 args (cmd, err, scene, line).
do
    local src = io.open("scripts/scheduler.lua", "rb")
    local body = src and src:read("*a") or ""
    if src then src:close() end
    check("T5 scheduler passes scene+line to handle_error",
          body:find("ctx.error_command = actual_cmd", 1, true) ~= nil
          and body:find("ctx.error_token_line", 1, true) ~= nil
          and body:find("pcall(ctx.handle_error, actual_cmd, error_message,", 1, true) ~= nil)
end

-- Real tokenizer/scheduler/runner dispatch: one accepted click must not run a
-- later command after the default error policy stops the failed session.
do
    local kag, flow = require("kag"), require("flow")
    local tokenizer = require("tokenizer")
    local old_load, old_engine, old_debug = flow.load_scene, _G.Engine, _G.Debug
    local after, reports, observed_stopped = 0, 0, false
    kag.u18fail = function() error("U18 command failure", 0) end
    kag.u18after = function() after = after + 1 end
    _G.Debug = {log = function() end}
    _G.Engine = {report_command_error = function()
        reports = reports + 1
        observed_stopped = kag_runner.get_ctx().stop_flag == true
    end}
    flow.load_scene = function()
        return {tokens=tokenizer.parse("[p]\n[u18fail]\n[u18after]"), labels={}}
    end
    local function start(handler)
        after = 0
        check("U18 previous session closes", kag_runner.stop() == true)
        local ok = kag_runner.start("tests/scripts/u18-error.ks")
        check("U18 real scene starts", ok == true)
        if handler then kag_runner.get_ctx().handle_error = handler end
        kag_runner.update(0.016)
        check("U18 real page wait reached", kag_runner.get_ctx().waiting_input == true)
    end
    start()
    local ok, reason = kag_runner.on_click()
    check("U18 default stops before native reporter", observed_stopped)
    check("U18 default error reports once", reports == 1)
    check("U18 default stops same-click next command", after == 0)
    check("U18 failed click returns failure", ok == false and reason == "command-error")
    check("U18 failed session is inactive", not kag_runner.get_ctx()._session_active)
    check("U18 failed ctx keeps command location", kag_runner.get_ctx().error_command == "u18fail"
        and kag_runner.get_ctx().error_token_line == 2)

    local custom_calls = 0
    start(function() custom_calls = custom_calls + 1 end)
    ok = kag_runner.on_click()
    check("U18 explicit custom recovery continues", ok == true and after == 1 and custom_calls == 1)
    check("U18 custom policy bypasses default reporter", reports == 1)

    start(function() error("U18 error handler failure", 0) end)
    ok, reason = kag_runner.on_click()
    check("U18 failed error handler stops dispatch", ok == false and reason == "command-error" and after == 0)
    check("U18 failed handler retained diagnostic", type(kag_runner.get_ctx().error_handler_error) == "string"
        and kag_runner.get_ctx().error_handler_error:find("U18 error handler failure", 1, true) ~= nil)

    local function unprintable_error()
        error(setmetatable({}, {__tostring=function() error("format failed", 0) end}), 0)
    end
    start(unprintable_error)
    ok, reason = kag_runner.on_click()
    local failed_ctx = kag_runner.get_ctx() or {}
    check("U18 unprintable handler error stops dispatch", ok == false and reason == "command-error" and after == 0)
    check("U18 unprintable handler error retains location", failed_ctx.error_command == "u18fail"
        and failed_ctx.error_token_line == 2 and type(failed_ctx.error_handler_error) == "string")

    kag.u18fail = unprintable_error
    start()
    ok, reason = kag_runner.on_click()
    failed_ctx = kag_runner.get_ctx() or {}
    check("U18 unprintable command error stops dispatch", ok == false and reason == "command-error" and after == 0)
    check("U18 unprintable command error retains location", failed_ctx.error_command == "u18fail"
        and failed_ctx.error_token_line == 2)

    local fail_now = true
    kag.u18fail = function() if fail_now then error("rollback failure", 0) end end
    start()
    ok, reason = kag_runner.on_click()
    check("U18 rollback fixture reaches command failure", ok == false and reason == "command-error")
    fail_now = false
    ok, reason = kag_runner.rollback()
    check("U18 rollback from failed session commits", ok == true)
    ok, reason = kag_runner.on_click()
    check("U18 rollback clears prior failure and resumes", ok == true and after == 1)

    -- Full restore publishes a different context. Its default error handler
    -- must close over that new owner, while a custom recovery policy survives.
    local save = require("kag.commands.save")
    local save_state = require("kag.save_state")
    local function restore_wait()
        local old_ctx = kag_runner.get_ctx()
        local captured = save.capture_state(old_ctx)
        local prepared = save_state.prepare(captured, save._safeScenePath, flow.load_scene)
        local restored = kag_runner.restore_candidate(old_ctx, prepared)
        check("U18 full restore commits", restored == true)
        local new_ctx = kag_runner.get_ctx()
        check("U18 full restore replaces context", new_ctx ~= old_ctx)
        kag_runner.update(0)
        check("U18 full restore reaches page wait", new_ctx.waiting_input == true)
        return old_ctx, new_ctx
    end
    fail_now = true
    start()
    local old_ctx, restored_ctx = restore_wait()
    local before_reports = reports
    ok, reason = kag_runner.on_click()
    print("U18 restored default observation", ok, reason, after, restored_ctx._command_error,
        restored_ctx.token_index, restored_ctx._resume_index, restored_ctx.waiting_input)
    check("U18 restored default stops its new owner", ok == false and reason == "command-error"
        and restored_ctx._command_error == true and after == 0)
    check("U18 restored default does not mark old owner failed", old_ctx._command_error == nil)
    check("U18 restored default reports once", reports == before_reports + 1)

    local restored_custom_calls = 0
    start(function() restored_custom_calls = restored_custom_calls + 1 end)
    restore_wait()
    ok, reason = kag_runner.on_click()
    print("U18 restored custom observation", ok, reason, after, restored_custom_calls)
    check("U18 full restore preserves custom recovery", ok == true and after == 1 and restored_custom_calls == 1)

    -- A completed [ch] followed by [p] shares the resume token with an
    -- already-suspended [p], but requires a different first-click behavior.
    flow.load_scene = function()
        return {tokens=tokenizer.parse("[ch text=FIRST]\n[p]\n[u18after]"), labels={}}
    end
    start()
    kag_runner.update(1) -- Finish the actual typewriter, preserving the wait.
    local before_page = save.capture_state(kag_runner.get_ctx())
    ok = kag_runner.on_click()
    check("U18 uninterrupted text click enters page wait", ok == true and after == 0
        and kag_runner.get_ctx()._executing_command == "p")
    local prepared = save_state.prepare(before_page, save._safeScenePath, flow.load_scene)
    check("U18 text-before-page restore commits", kag_runner.restore_candidate(kag_runner.get_ctx(), prepared) == true)
    kag_runner.update(0)
    ok = kag_runner.on_click()
    check("U18 restored text click also enters page wait", ok == true and after == 0
        and kag_runner.get_ctx()._executing_command == "p")
    check("U18 restored first click keeps the visible page", kag_runner.get_ctx().text_state.page_src[1]
        and kag_runner.get_ctx().text_state.page_src[1].src == "FIRST")
    kag_runner.on_click()
    check("U18 restored second click clears and advances", after == 1)

    flow.load_scene = function()
        return {tokens=tokenizer.parse("[p]\n[u18fail]\n[u18after]"), labels={}}
    end
    kag.u18fail = function() end
    start()
    prepared = save_state.prepare(save.capture_state(kag_runner.get_ctx()), save._safeScenePath, flow.load_scene)
    local old_paused = rawget(_G, "_CAESURA_DEBUG_PAUSED")
    _G._CAESURA_DEBUG_PAUSED = true
    ok, reason = kag_runner.restore_candidate(kag_runner.get_ctx(), prepared)
    check("U18 paused restore remains a committed waiting session", ok == true
        and kag_runner.get_ctx()._session_active and kag_runner.get_ctx().waiting_input)
    local paused_state = save.capture_state(kag_runner.get_ctx())
    check("U18 paused re-save preserves actual page wait identity", paused_state.resume_page_wait == true)
    prepared = save_state.prepare(paused_state, save._safeScenePath, flow.load_scene)
    check("U18 paused re-save loads again", kag_runner.restore_candidate(kag_runner.get_ctx(), prepared) == true)
    ok, reason = kag_runner.on_click()
    check("U18 paused restored page does not advance", ok == false and reason == "debug-paused" and after == 0)
    _G._CAESURA_DEBUG_PAUSED = false
    ok = kag_runner.on_click()
    check("U18 unpaused restored page advances on first accepted click", ok == true and after == 1)
    _G._CAESURA_DEBUG_PAUSED = old_paused

    local malformed = save_state.copy(paused_state)
    malformed.resume_page_wait = "yes"
    check("U18 page wait marker rejects non-boolean data", not pcall(save_state.prepare,
        malformed, save._safeScenePath, flow.load_scene))
    malformed = save_state.copy(paused_state)
    malformed.token_index = 2 -- u18fail is not a page-wait command.
    check("U18 page wait marker requires the saved p cursor", not pcall(save_state.prepare,
        malformed, save._safeScenePath, flow.load_scene))
    malformed = save_state.copy(paused_state)
    malformed.text_snapshot.waiting_input = false
    check("U18 page wait marker requires the matching visual wait", not pcall(save_state.prepare,
        malformed, save._safeScenePath, flow.load_scene))

    start()
    local legacy = save_state.copy(paused_state)
    legacy.resume_page_wait = nil
    prepared = save_state.prepare(legacy, save._safeScenePath, flow.load_scene)
    check("U18 legacy slot without marker still loads", kag_runner.restore_candidate(kag_runner.get_ctx(), prepared) == true)
    kag_runner.on_click()
    check("U18 legacy ambiguous cursor does not skip a wait", after == 0 and kag_runner.get_ctx().waiting_input)
    kag_runner.on_click()
    check("U18 legacy wait still resumes", after == 1)

    for triggering_probe = 1, 2 do
        start()
        prepared = save_state.prepare(save.capture_state(kag_runner.get_ctx()), save._safeScenePath, flow.load_scene)
        check("U18 prime reentry fixture restores", kag_runner.restore_candidate(kag_runner.get_ctx(), prepared) == true)
        local prime_owner = kag_runner.get_ctx()
        local nested = save_state.prepare(save.capture_state(prime_owner), save._safeScenePath, flow.load_scene)
        local probes, adapter_resumes = 0, 0
        kag_runner.set_resume_adapter({
            is_paused=function()
                probes = probes + 1
                if probes == triggering_probe then
                    check("U18 pause probe nested restore commits", kag_runner.restore_candidate(prime_owner, nested) == true)
                end
                return false
            end,
            resume=function(_, co, value)
                adapter_resumes = adapter_resumes + 1
                return coroutine.resume(co, value)
            end,
        })
        ok, reason = kag_runner.on_click()
        check("U18 expired prime cannot resume replacement owner", ok == false and reason == (triggering_probe == 1 and "click-owner-expired" or "restore-owner-expired")
            and adapter_resumes == 0 and kag_runner.get_ctx() ~= prime_owner and after == 0)
        check("U18 replacement retains its pending wait", kag_runner.get_ctx()._resumePageWait == true)
        kag_runner.set_resume_adapter(nil)
        ok = kag_runner.on_click()
        check("U18 replacement resumes only on a fresh click", ok == true and after == 1)
    end

    start()
    prepared = save_state.prepare(save.capture_state(kag_runner.get_ctx()), save._safeScenePath, flow.load_scene)
    local kag_debug = require("kag_debug")
    kag_debug.set_breakpoint("tests/scripts/u18-error.ks", "p")
    check("U18 KAG breakpoint fixture restores", kag_runner.restore_candidate(kag_runner.get_ctx(), prepared) == true)
    ok, reason = kag_runner.update(0)
    check("U18 KAG breakpoint before p keeps pending identity", ok == false and reason == "kag-paused"
        and kag_runner.get_ctx()._resumePageWait == true and after == 0)
    ok, reason = kag_runner.on_click()
    check("U18 click cannot bypass restored KAG breakpoint", ok == false and reason == "kag-paused" and after == 0)
    kag_runner.continue_scene_debugger()
    ok, reason = kag_runner.update(0)
    print("U18 resumed KAG observation", ok, reason, kag_runner.get_ctx()._resumePageWait,
        kag_runner.get_ctx()._kag_debug_paused, kag_runner.get_ctx()._executing_command,
        kag_runner.get_ctx().waiting_input)
    check("U18 actual p entry consumes pending identity", kag_runner.get_ctx()._resumePageWait == nil
        and kag_runner.get_ctx().waiting_input and after == 0)
    ok = kag_runner.on_click()
    check("U18 resumed KAG breakpoint needs one page click", ok == true and after == 1)
    kag_debug.clear_breakpoints("tests/scripts/u18-error.ks")

    kag.u18fail = function() end
    start()
    ok = kag_runner.on_click()
    check("U18 fresh healthy session still runs", ok == true and after == 1)
    kag_runner.stop()
    kag.u18fail, kag.u18after = nil, nil
    flow.load_scene, _G.Engine, _G.Debug = old_load, old_engine, old_debug
end

print(string.format("test_errorui_wiring: %d passed, %d failed", passed, failed))
if failed > 0 then os.exit(1) end
