-- U11: run actual runner/handlers; storage alone is an isolated value-copy fake.
package.path = "scripts/?.lua;scripts/?/init.lua;" .. package.path

local function callable(fields)
    return setmetatable(fields or {}, { __index = function(self, key)
        if type(key) ~= "string" then return nil end
        local fn = function() return true end
        rawset(self, key, fn)
        return fn
    end })
end

local function copy(value)
    if type(value) ~= "table" then return value end
    local result = {}
    for key, child in pairs(value) do result[key] = copy(child) end
    return result
end

local slots = {}
_G.KAG = callable({
    save_game = function(slot, state)
        slots[slot] = copy(state)
        return true
    end,
    load_game = function(slot)
        return copy(slots[slot]), { slot = slot }
    end,
    is_voice_playing = function() return false end,
    is_bgm_playing = function() return false end,
    get_active_voices = function() return 0 end,
})
_G.Engine, _G.Render, _G.DevCore = callable(), callable(), callable()
_G.backend = callable({
    is_voice_playing = function() return false end,
    is_bgm_playing = function() return false end,
    get_active_voices = function() return 0 end,
})

local runner = require("kag_runner")
local save = require("kag.commands.save")
local i18n = require("i18n")
local original_open = io.open
io.open = function(path, mode)
    if path == "assets/lang/u11_invalid.lua" then
        return { read = function() return "return {broken=" end, close = function() return true end }
    end
    return original_open(path, mode)
end
local passed, failed = 0, 0
local function check(name, value)
    if value then passed = passed + 1
    else failed = failed + 1; print("FAIL: " .. name) end
end

local function fresh()
    runner.stop()
    assert(runner.start("tests/projects/u11_restore/base.ks"))
    -- The scheduler yields between commands; reach the actual wait before
    -- treating the earlier set command as part of the saved state.
    for _ = 1, 4 do runner.update(0) end
    local ctx = runner.get_ctx()
    assert(ctx and ctx.co and coroutine.status(ctx.co) == "suspended")
    assert(ctx.f.route == "saved", "fixture must execute its initial set")
    ctx.f.nested = { route = "saved" }
    ctx.sf.owner = "original"
    ctx.tf.keep_on_failure = "old"
    ctx.lf = { owner = "saved-local" }
    ctx.mp = { argument = "saved-argument" }
    ctx.variables = { local_value = 7 }
    return ctx
end

local function try_load()
    local ok, err = pcall(save.load, runner.get_ctx(), { slot = 31 })
    return ok, err
end

do
    local ctx = fresh()
    save.save(ctx, {slot=31})
    check("absent textbox style is saved", ctx.tf.save_result == "ok" and slots[31].textbox_style == nil)
    check("absent textbox style loads", save.load(ctx, {slot=31}) == true)
    local restored = runner.get_ctx()
    check("missing textbox style stays absent after load", restored.textbox_style == nil)
    check("cl after loading missing textbox style succeeds",
        pcall(require("kag.commands.layer").cl, restored, {}))

    ctx = fresh()
    local style = {x=-12.5, y=42, w=640, h=96, color="12,34,56", opacity=0, visible=false}
    ctx.textbox_style = copy(style)
    save.save(ctx, {slot=31})
    check("complete textbox style saves", ctx.tf.save_result == "ok")
    check("complete textbox style loads", save.load(ctx, {slot=31}) == true)
    restored = runner.get_ctx()
    for key, value in pairs(style) do
        check("textbox style preserves " .. key, restored.textbox_style[key] == value)
    end
    check("cl after loading complete textbox style succeeds",
        pcall(require("kag.commands.layer").cl, restored, {}))

    -- The direct-host path reuses its context, so nil must clear a future style.
    runner.stop()
    slots[31].textbox_style = nil
    local direct = {f={}, tf={}, textbox_style=copy(style)}
    check("direct host loads absent style", save.load(direct, {slot=31}) == true)
    check("direct host clears later textbox style", direct.textbox_style == nil)
    check("direct host cl without saved style succeeds",
        pcall(require("kag.commands.layer").cl, direct, {}))
end

do
    local ctx=fresh()
    runner.update(20)
    slots[31]=copy(save.capture_state(ctx))
    check('timed wait captures only its remaining duration',slots[31].wait_snapshot
        and slots[31].wait_snapshot.remaining_ms==40000)
    check('remaining wait can be loaded',save.load(ctx,{slot=31})==true)
    local pending=save.capture_state(runner.get_ctx())
    check('immediate re-save preserves pending wait progress',pending.wait_snapshot
        and pending.wait_snapshot.remaining_ms==40000)
    runner.update(0)
    runner.update(39)
    check('restored wait does not end early',runner.get_ctx().f.after==nil)
    runner.update(1)
    runner.update(0)
    check('restored wait does not replay elapsed time',runner.get_ctx().f.after==1)
    local captured,finished=pcall(save.capture_state,runner.get_ctx())
    check('finished wait does not prevent later saves',captured and finished.wait_snapshot==nil)
end

for _, mutation in ipairs({
    { "missing scene", function(state)
        state.scene_path = "tests/projects/u11_restore/missing.ks"
    end },
    { "unsafe scene", function(state) state.scene_path = "../outside.ks" end },
    { "bad f", function(state) state.f = 9 end },
    { "bad sf", function(state) state.sf = 9 end },
    { "bad backlog", function(state) state.backlog = 9 end },
    { "bad backlog entries", function(state) state.backlog = {"not an entry"} end },
    { "bad backlog text", function(state) state.backlog = {{text={}}} end },
    { "bad backlog array", function(state) state.backlog = {named={text="bad"}} end },
    { "bad language resource", function(state) state.language = "u11_invalid" end },
    { "bad auto mode", function(state) state.auto_mode = "false" end },
    { "bad NVL mode", function(state) state.nvl_mode = 1 end },
    { "bad voice mute", function(state) state.voice_muted = {} end },
    { "bad skip mode", function(state) state.skip_mode = "all" end },
    { "empty textbox style", function(state) state.textbox_style = {} end },
    { "missing textbox color", function(state)
        state.textbox_style = {x=0,y=0,w=640,h=96,opacity=200,visible=true}
    end },
    { "bad textbox color", function(state)
        state.textbox_style = {x=0,y=0,w=640,h=96,color={},opacity=200,visible=true}
    end },
    { "bad textbox width", function(state)
        state.textbox_style = {x=0,y=0,w="640",h=96,color="0,0,0",opacity=200,visible=true}
    end },
    { "textbox width below contract", function(state)
        state.textbox_style = {x=0,y=0,w=63,h=96,color="0,0,0",opacity=200,visible=true}
    end },
    { "textbox height above contract", function(state)
        state.textbox_style = {x=0,y=0,w=640,h=1025,color="0,0,0",opacity=200,visible=true}
    end },
    { "bad textbox opacity", function(state)
        state.textbox_style = {x=0,y=0,w=640,h=96,color="0,0,0",opacity=256,visible=true}
    end },
    { "bad textbox visible", function(state)
        state.textbox_style = {x=0,y=0,w=640,h=96,color="0,0,0",opacity=200,visible="false"}
    end },
    { "negative remaining wait", function(state) state.wait_snapshot={scene=state.scene_path,token_index=state.token_index,remaining_ms=-1} end },
    { "too long remaining wait", function(state) state.wait_snapshot={scene=state.scene_path,token_index=state.token_index,remaining_ms=60001} end },
    { "wrong wait cursor", function(state) state.wait_snapshot={scene=state.scene_path,token_index=state.token_index+1,remaining_ms=1} end },
}) do
    local ctx = fresh()
    local co, flags, system_flags, temporary = ctx.co, ctx.f, ctx.sf, ctx.tf
    local language = i18n.current
    slots[31] = copy(save.capture_state(ctx))
    slots[31].f = { route = "corrupt-target" }
    slots[31].language = language == "en" and "zh" or "en"
    mutation[2](slots[31])
    local ok = try_load()
    check(mutation[1] .. ": rejection does not throw", ok)
    check(mutation[1] .. ": preserves the published session", runner.get_ctx() == ctx)
    check(mutation[1] .. ": preserves f identity and values",
        ctx.f == flags and flags.route == "saved" and flags.nested.route == "saved")
    check(mutation[1] .. ": preserves sf identity and values",
        ctx.sf == system_flags and system_flags.owner == "original")
    check(mutation[1] .. ": preserves temporary state",
        ctx.tf == temporary and temporary.keep_on_failure == "old")
    check(mutation[1] .. ": leaves the coroutine active",
        ctx.co == co and coroutine.status(co) == "suspended"
        and ctx._session_active and not ctx.stop_flag)
    check(mutation[1] .. ": leaves language unchanged", i18n.current == language)
    check(mutation[1] .. ": reports failed load", ctx.tf.load_result == "error")
end

do
    local ctx = fresh()
    ctx.settingsValues = { language = "en" }
    save.save(ctx, { slot = 31, thumbnail = "u11" })
    local old_co = ctx.co
    ctx.f.future_only, ctx.tf.future_only = true, true
    ctx.f.route, ctx.f.nested.route = "future", "future"
    ctx.lf.owner, ctx.mp.argument, ctx.variables.local_value = "future", "future", 99
    ctx.settingsValues.language = "zh"
    i18n.load("zh")
    ctx._forStack = { { var = "future", endv = 9, step = 1, pos = 1 } }
    ctx._whileStack = { { pos = 1, ended = false } }
    ctx._ifStack, ctx._switchStack = { { taken = true } }, { { matched = true } }
    check("successful load request does not throw", try_load())
    for _ = 1, 3 do runner.update(0) end
    local restored = runner.get_ctx()
    check("f exactly restores values and removes future keys",
        restored and restored.f.route == "saved" and restored.f.future_only == nil
        and restored.f.nested.route == "saved")
    check("tf is rebuilt for the restored session",
        restored and restored.tf.future_only == nil
        and restored.tf.keep_on_failure == nil and restored.tf.load_result == "ok")
    check("lf and mp survive restoration",
        restored and restored.lf.owner == "saved-local"
        and restored.mp.argument == "saved-argument")
    check("context variables survive restoration",
        restored and restored.variables.local_value == 7)
    check("successful commit applies the saved language",
        restored and restored.settingsValues.language == "en" and i18n.current == "en")
    check("empty control stacks cannot inherit future execution",
        restored and #(restored._forStack or {}) == 0
        and #(restored._whileStack or {}) == 0 and #(restored._ifStack or {}) == 0
        and #(restored._switchStack or {}) == 0)
    check("old coroutine is closed at successful commit",
        coroutine.status(old_co) == "dead" and restored.co ~= old_co)
    restored.settingsValues.language = "zh"
    i18n.load("zh")
    local current_co = restored.co
    local stale_result = save.load(ctx,{slot=31})
    check("stale native context cannot initiate another restore",stale_result == false)
    check("stale restore preserves the current session",runner.get_ctx() == restored and restored.co == current_co)
    check("stale restore cannot change the current locale",i18n.current == "zh")
end

do
    runner.stop()
    assert(runner.start("tests/projects/u11_restore/call.ks"))
    for _ = 1, 24 do runner.update(0) end
    local at_save = runner.get_ctx()
    assert(at_save._executing_command == "wait" and #at_save.call_stack == 1,
        "call fixture must be suspended inside the callee")
    assert(at_save.f.i == 1 and at_save.f.visits == 0 and at_save.lf.owner == "callee",
        "fixture state: i=" .. tostring(at_save.f.i) .. " visits=" .. tostring(at_save.f.visits)
        .. " local=" .. tostring(at_save.lf.owner))
    save.save(at_save, { slot = 32, thumbnail = "u11" })
    assert(at_save.tf.save_result == "ok", at_save.tf.save_error)
    local function finish_call()
        for _ = 1, 80 do
            local _, reason = runner.update(60)
            if reason == "ended" then return runner.get_ctx() end
        end
        error("call fixture did not reach its end")
    end
    local direct = finish_call()
    check("uninterrupted caller completes both loop iterations", direct.f.visits == 2)
    check("uninterrupted caller gets its local frame back", direct.f.caller_owner == "caller")
    local call_ok, accepted = pcall(save.load, direct, { slot = 32 })
    check("call snapshot is accepted", call_ok and accepted == true)
    local resumed = runner.get_ctx()
    check("call restore actually returns to the saved state",
        resumed ~= direct and resumed.f.visits == 0 and resumed.f.i == 1
        and not resumed.f.finished)
    local restored = finish_call()
    check("restored call preserves caller loop and branch continuation",
        restored.f.finished and restored.f.visits == direct.f.visits and restored.f.i == direct.f.i)
    check("restored call preserves callee and caller local frames",
        restored.f.callee_owner == "callee" and restored.f.caller_owner == "caller")
end

do
    runner.stop()
    assert(runner.start("tests/projects/u11_restore/text.ks"))
    for _ = 1, 20 do
        if runner.get_ctx().waiting_input then break end
        runner.update(0)
    end
    local original = runner.get_ctx()
    assert(original.waiting_input and original.f.steps == 0)
    runner.update(0.1)
    assert(original.text_state.reveal_chars == 2)
    save.save(original, {slot=33,thumbnail="u11"})
    assert(original.tf.save_result == "ok",original.tf.save_error)
    local _, first_click = runner.on_click()
    check("uninterrupted first click only reveals",first_click == "revealed" and original.f.steps == 0)
    runner.on_click()
    check("uninterrupted second click advances",original.f.steps == 1)
    assert(save.load(original,{slot=33}))
    local restored = runner.get_ctx()
    check("restored page keeps its partial reveal",restored.text_state.reveal_chars == 2)
    local _, restored_first = runner.on_click()
    check("restored first click only reveals",restored_first == "revealed" and restored.f.steps == 0)
    runner.on_click()
    check("restored second click advances without replaying the saved line",restored.f.steps == 1)
    check("restored second click displays the following page",
        restored.text_state.page_src[1] and restored.text_state.page_src[1].src == "SECOND")
end

do
    runner.stop()
    assert(runner.start("tests/projects/u11_restore/call_eof.ks"))
    for _ = 1, 24 do runner.update(0) end
    local finished = runner.get_ctx()
    assert(finished.f.finished and finished.f.returned_owner == "caller")
    assert(save.load(finished,{slot=34}))
    local restored = runner.get_ctx()
    check("callee EOF restore starts from the saved values",restored.f.saved_value == 42 and not restored.f.finished)
    for _ = 1, 24 do runner.update(0) end
    check("callee EOF restore returns to its caller",restored.f.finished == true)
    check("callee EOF restore reinstates the caller local frame",restored.f.returned_owner == "caller")
end

runner.stop()
-- Web hosts share the runner's coroutine owner rather than creating a loader
-- scene. Preparation has no publication/cancellation side effects.
do
    local current = fresh()
    local co, flags, temporary = current.co, current.f, current.tf
    slots[31] = copy(save.capture_state(current))
    local called, candidate = pcall(save.prepare_load, {slot=31})
    check("standalone slot preparation returns a candidate", called and type(candidate)=="table")
    check("slot preparation preserves the active owner and coroutine", runner.get_ctx()==current
        and current.co==co and coroutine.status(co)=="suspended" and current._session_active)
    check("slot preparation does not change existing values or feedback", current.f==flags and current.tf==temporary
        and current.tf.load_result==nil and current.tf.keep_on_failure=="old")
    if called and type(candidate)=="table" then
        check("slot preparation includes load metadata", candidate.tf.load_result=="ok"
            and candidate.tf.load_slot==31 and candidate.tf.load_meta.slot==31)
        require("kag.presentation").discard(candidate._presentation)
    end
    slots[31].scene_path = "../outside.ks"
    local rejected, value, reason = pcall(save.prepare_load, {slot=31})
    check("standalone invalid slot preparation returns an error without throwing", rejected and value==nil
        and type(reason)=="string")
    check("invalid standalone preparation preserves old feedback", current.tf==temporary and current.tf.load_result==nil)
end

do
    local current = fresh()
    local co = current.co
    local cancelled = 0
    local scope = require("kag.operation").start(current)
    scope.token:register(function() cancelled=cancelled+1 end)
    local accepted, reason = runner.start("tests/projects/u11_restore/base.ks")
    check("ordinary start still refuses a suspended session", not accepted and reason=="already-running")
    accepted, reason = runner.start("tests/projects/u11_restore/base.ks", {replace="true"})
    check("only boolean true authorizes start replacement", not accepted and reason=="already-running")
    accepted, reason = runner.start("tests/projects/u11_restore/missing.ks", {replace=true})
    check("replace attempts scene preparation before refusing invalid content", not accepted and reason=="scene-load-failed")
    check("failed replacement scene retains the old continuation and operations", runner.get_ctx()==current
        and current.co==co and coroutine.status(co)=="suspended" and cancelled==0 and not current.stop_flag)
    local presentation = require("kag.presentation")
    local prepare_font = presentation.prepare_start
    presentation.prepare_start = function() error("injected replacement font preparation") end
    accepted, reason = runner.start("tests/projects/u11_restore/base.ks", {replace=true})
    presentation.prepare_start = prepare_font
    check("replacement font preparation failure is returned", not accepted
        and tostring(reason):find("injected replacement font preparation",1,true)~=nil)
    check("failed replacement font retains the old continuation and operations", runner.get_ctx()==current
        and current.co==co and coroutine.status(co)=="suspended" and cancelled==0)
    accepted = runner.start("tests/projects/u11_restore/base.ks", {replace=true})
    check("explicit replacement publishes a fresh owned context", accepted and runner.get_ctx()~=current)
    check("explicit replacement closes and cancels the prior owner once", coroutine.status(co)=="dead"
        and not current._session_active and cancelled==1)
end

do
    local flow = require("flow")
    local original_load = flow.load_scene
    local nested_result, nested_reason, reads
    local kag = require("kag")
    kag.u11_reentrant_replace = function()
        nested_result, nested_reason = runner.start("nested.ks", {replace=true})
    end
    runner.stop()
    reads = 0
    flow.load_scene = function(path)
        reads = reads+1
        return {tokens={{"u11_reentrant_replace",{}},{"wait",{time=60000}}},labels={},path=path}
    end
    assert(runner.start("outer.ks"))
    check("replace cannot close a running scheduler", nested_result==false and nested_reason=="scheduler-running")
    check("running-scheduler replacement rejects before preparing another scene", reads==1)
    flow.load_scene = original_load
    kag.u11_reentrant_replace = nil
    runner.stop()
end

do
    local current = fresh()
    slots[31] = copy(save.capture_state(current))
    runner.stop()
    local prepared = require("kag.save_state").prepare(slots[31],save._safeScenePath,require("flow").prepare_scene)
    local called, accepted = pcall(runner.restore_candidate,nil,prepared)
    check("cold restore accepts a nil expected owner without a dummy scene", called and accepted==true
        and runner.get_ctx()~=nil and runner.get_ctx().f.route=="saved")
    if not called or not accepted then require("kag.presentation").discard(prepared._presentation) end
    local active = runner.get_ctx()
    if active then
        local duplicate = require("kag.save_state").prepare(slots[31],save._safeScenePath,require("flow").prepare_scene)
        accepted = runner.restore_candidate(nil,duplicate)
        check("cold restore cannot replace an owner that appeared meanwhile", not accepted and runner.get_ctx()==active)
        require("kag.presentation").discard(duplicate._presentation)
    end
    runner.stop()
    prepared = require("kag.save_state").prepare(slots[31],save._safeScenePath,require("flow").prepare_scene)
    local presentation = require("kag.presentation")
    local apply = presentation.apply
    local reached_apply = false
    presentation.apply = function() reached_apply=true; error("injected cold apply failure") end
    called, accepted = pcall(runner.restore_candidate,nil,prepared)
    presentation.apply = apply
    check("cold postcommit failure returns false and leaves no active context", called and accepted==false
        and reached_apply and runner.get_ctx()==nil)
    presentation.discard(prepared._presentation)
end

do
    local flow = require("flow")
    local original_load, original_policy = flow.load_scene, flow.is_restore_scene
    local reads = 0
    local function stage_history(path)
        local current = fresh()
        assert(runner.stop_for_reload())
        current._pendingJump = {scene=path,index=1}
        return current
    end
    local current = stage_history("owned.ks")
    flow.is_restore_scene = function(path) return path=="owned.ks" end
    flow.load_scene = function(path)
        reads=reads+1
        return {tokens={{"set",{var="f.history_restored",value=1}},{"end",{}}},labels={},path=path}
    end
    runner.update(0)
    check("history jumps use the host-owned scene policy", current.current_scene=="owned.ks"
        and current.f.history_restored==1 and reads==1)
    flow.load_scene, flow.is_restore_scene = original_load, original_policy
    current = stage_history("tests/projects/u11_restore/base.ks")
    flow.is_restore_scene = function() return false end
    flow.load_scene = function() error("disallowed history scene was read") end
    local called, accepted, reason = pcall(runner.update,0)
    check("host history policy does not fall back to filesystem prefixes", called and accepted==false
        and reason=="unsafe-jump-target")
    flow.load_scene, flow.is_restore_scene = original_load, original_policy
    runner.stop()
end

-- The same runner boundary must install the prepared layer tree, and a
-- post-commit GPU error must leave no resumable mixed session.
do
    local original_restore = _G.Restore
    local pending, discarded, applied, pause_prepare = {}, 0, 0, false
    _G.Restore = {
        capture_font=function() return {version=1,active=false} end,
        default_font=function() return {version=1,active=false} end,
        prepare_font=function()
            local ticket = {}
            pending[ticket] = true
            if pause_prepare then pause_prepare=false; coroutine.yield("asset-await") end
            return ticket
        end,
        discard_font=function(ticket)
            if pending[ticket] then pending[ticket]=nil; discarded=discarded+1 end
        end,
        apply_font=function(ticket) pending[ticket]=nil; applied=applied+1; return true end,
        clear_font=function() return true end,
    }
    for _, mode in ipairs({"new-owner", "unregistered-runner", "replaced-runner"}) do
        local original = fresh()
        slots[31] = copy(save.capture_state(original))
        pause_prepare = true
        local load_co = coroutine.create(function() return save.load(original,{slot=31}) end)
        local resumed, event = coroutine.resume(load_co)
        assert(resumed and event=="asset-await")
        local current = original
        if mode=="new-owner" then current=fresh()
        elseif mode=="unregistered-runner" then package.loaded.kag_runner=nil
        else package.loaded.kag_runner={get_ctx=function() return original end} end
        local layer = require("layers").add_layer(nil,{id="surviving-owner",w=0,h=0})
        local old_discarded, old_applied = discarded, applied
        local accepted, reason
        resumed, accepted, reason = coroutine.resume(load_co)
        package.loaded.kag_runner = runner
        check(mode .. ": prepared stale load is rejected", resumed and accepted==false and reason=="restore-owner-expired")
        check(mode .. ": prepared stale load releases its ticket without applying it",
            discarded==old_discarded+1 and applied==old_applied and next(pending)==nil)
        check(mode .. ": prepared stale load preserves the current presentation", runner.get_ctx()==current
            and require("layers").get_layer("surviving-owner")==layer)
        runner.stop()
    end
    local flow, operation = require("flow"), require("kag.operation")
    local original_load, original_cancel = flow.load_scene, operation.cancel_all
    local original_bind = Engine.bind_active_context
    for _, failure in ipairs({"cancel", "publish"}) do
        local original = fresh()
        slots[31] = copy(save.capture_state(original))
        runner.stop()
        local prepared
        require("kag").u11_pending_restore = function(owner)
            prepared = assert(save.prepare_load({slot=31}))
            local accepted, reason = runner.restore_candidate(owner,prepared)
            assert(accepted and reason=="restore-pending")
        end
        flow.load_scene = function(path, fresh)
            if path=="pending.ks" then
                return {tokens={{"u11_pending_restore",{}},{"end",{}}},labels={},path=path}
            end
            return original_load(path,fresh)
        end
        assert(runner.start("pending.ks"))
        local owner = runner.get_ctx()
        assert(owner._pendingRestore==prepared and next(pending)~=nil)
        if failure=="cancel" then
            operation.cancel_all = function(value) original_cancel(value); error("injected pending cancellation failure") end
        else
            Engine.bind_active_context = function(value)
                if value then error("injected pending publication failure") end
                return true
            end
        end
        local before = discarded
        local called, accepted = pcall(runner.update,0)
        operation.cancel_all, Engine.bind_active_context = original_cancel, original_bind
        flow.load_scene = original_load
        require("kag").u11_pending_restore = nil
        check(failure .. ": pending commit failure leaves no active owner", called and accepted==false and runner.get_ctx()==nil)
        check(failure .. ": pending commit failure disposes the accepted ticket", discarded==before+1 and next(pending)==nil)
        require("kag.presentation").discard(prepared._presentation)
        check(failure .. ": caller cleanup after commit failure is idempotent", discarded==before+1 and next(pending)==nil)
    end
    for _, retain_new_owner in ipairs({true,false}) do
        runner.stop()
        flow.load_scene = function(path)
            if path=="slow.ks" then coroutine.yield("scene-await"); path="tests/projects/u11_restore/base.ks" end
            return original_load(path)
        end
        local start_co = coroutine.create(function() return runner.start("slow.ks",{replace=true}) end)
        local resumed, event = coroutine.resume(start_co)
        assert(resumed and event=="scene-await")
        local next_owner = fresh()
        if not retain_new_owner then runner.stop(); next_owner=nil end
        local before = discarded
        local accepted, reason
        resumed, accepted, reason = coroutine.resume(start_co)
        check("async start revalidates publication history (retain="..tostring(retain_new_owner)..")",
            resumed and accepted==false and reason=="start-owner-expired" and runner.get_ctx()==next_owner)
        check("expired async start releases its prepared font", discarded==before+1 and next(pending)==nil)
        flow.load_scene = original_load
        runner.stop()
    end
    _G.Restore = original_restore
end

do
    local Layers=require("layers")
    local uploads,destroyed=0,0
    local fail_prepare,fail_upload=false,false
    _G.Restore={
        describe_texture=function() return {kind="asset",path="assets/a.bmp"} end,
        prepare_image=function() if fail_prepare then return nil,"prepare failed" end; return {} end,
        image_info=function() return 1,1 end,
        materialize_image=function() if fail_upload then return nil,"upload failed" end; uploads=uploads+1; return 50+uploads end,
        discard_image=function() end,
    }
    Render.create_viewport=function() return 17 end
    Render.destroy_texture=function() destroyed=destroyed+1 end
    local current=fresh()
    Layers.init()
    local old=Layers.add_layer(nil,{id="u11_bg",name="u11_bg",w=100,h=100})
    old.tex=7
    save.save(current,{slot=31,thumbnail="u11"})
    check("layer snapshot is captured",slots[31].layer_snapshot~=nil)
    fail_prepare=true
    local accepted=save.load(current,{slot=31})
    check("native layer preparation failure preserves old session",accepted==false
        and runner.get_ctx()==current and Layers.get_layer("u11_bg")==old)
    fail_prepare=false
    accepted=save.load(current,{slot=31})
    check("successful restore materializes and replaces layers",accepted==true
        and uploads==1 and Layers.get_layer("u11_bg")~=old and Layers.get_layer("u11_bg").tex==51)
    fail_upload=true
    accepted=save.load(runner.get_ctx(),{slot=31})
    check("post-commit GPU failure stops the session",accepted==false and runner.get_ctx()==nil)
    check("post-commit GPU failure clears old display and owned textures",Layers.count()==1 and destroyed==1)
    fail_upload=false
    current=fresh()
    check("explicit start works after failed restore",current._session_active==true)
    runner.stop()
end
do
    local playing={version=1,bgm=false}
    local stops,fail_prepare,fail_apply=0,false,false
    Restore.capture_audio=function() return copy(playing) end
    Restore.prepare_audio=function(snapshot)
        if fail_prepare then return nil,"audio prepare failed" end
        return copy(snapshot)
    end
    Restore.discard_audio=function() end
    Restore.apply_audio=function(snapshot)
        playing=copy(snapshot)
        if fail_apply then return false,"audio apply failed" end
        return true
    end
    Restore.stop_audio=function() stops=stops+1; playing={version=1,bgm=false}; return true end
    local current=fresh()
    playing={version=1,bgm={path="assets/a.wav",position=0.5,gain=0.75,looping=true}}
    save.save(current,{slot=31,thumbnail="u11"})
    check("audio declaration is captured",slots[31].audio_snapshot
        and slots[31].audio_snapshot.bgm.path=="assets/a.wav")
    playing={version=1,bgm={path="assets/b.wav",position=1,gain=1,looping=false}}
    fail_prepare=true
    local before=stops
    local accepted=save.load(current,{slot=31})
    check("audio prepare failure preserves the live context",accepted==false and runner.get_ctx()==current)
    check("audio prepare failure does not stop old sound",stops==before and playing.bgm.path=="assets/b.wav")
    fail_prepare=false
    accepted=save.load(current,{slot=31})
    check("audio resumes from its saved declaration",accepted==true and playing.bgm.path=="assets/a.wav"
        and playing.bgm.position==0.5 and playing.bgm.gain==0.75 and playing.bgm.looping)
    fail_apply=true
    accepted=save.load(runner.get_ctx(),{slot=31})
    check("audio apply failure stops the new context",accepted==false and runner.get_ctx()==nil)
    check("audio apply failure clears partial playback",playing.bgm==false)
    fail_apply=false
    local next_owner=fresh()
    playing={version=1,bgm={path="assets/c.wav",position=0,gain=1,looping=false}}
    local presentation=require("kag.presentation")
    presentation.stop(current)
    check("late cleanup cannot stop a newer session sound",playing.bgm.path=="assets/c.wav"
        and runner.get_ctx()==next_owner)
    runner.stop()
    check("ordinary session stop clears its audio",playing.bgm==false)
end
do
    local State=require("kag.save_state")
    local current=fresh()
    local snapshot=save.capture_state(current)
    snapshot.call_stack={
        {scene=snapshot.scene_path,index=1,lf={},mp={}},
        {scene=snapshot.scene_path,index=1,lf={},mp={}},
    }
    local reads=0
    local function scene_loader()
        reads=reads+1
        return assert(require('flow').prepare_scene(snapshot.scene_path))
    end
    local prepared=State.prepare(snapshot,save._safeScenePath,scene_loader)
    check("recursive scene prepares only one source snapshot",reads==1)
    check("caller frames use that same executable snapshot",prepared.tokens==prepared.call_stack[1].tokens
        and prepared.tokens==prepared.call_stack[2].tokens)
    local again=State.prepare(snapshot,save._safeScenePath,scene_loader)
    check("later restore gets a fresh scene snapshot",reads==2 and again.tokens~=prepared.tokens)
    require("kag.presentation").discard(prepared._presentation)
    require("kag.presentation").discard(again._presentation)

    snapshot.call_stack={}
    snapshot.schema_version=1
    snapshot.language=nil
    snapshot.language_default=nil
    i18n.load("en")
    i18n.default_language="ja"
    prepared=State.prepare(snapshot,save._safeScenePath,scene_loader)
    check("legacy missing language uses deterministic Chinese startup default",prepared.language=="zh")
    check("legacy language preparation preserves current language",i18n.current=="en")
    check("legacy default fallback is independent of the future session",prepared.language_default=="en"
        and i18n.default_language=="ja")
    require("kag.presentation").discard(prepared._presentation)
    slots[31]=snapshot
    assert(save.load(current,{slot=31}))
    check("legacy load does not borrow the future language",i18n.current=="zh"
        and runner.get_ctx().settingsValues.language=="zh")
    check("legacy load restores its fallback selection",i18n.default_language=="en")
    runner.stop()
end
do
    local selected={version=1,active=true,font=1,path="",size=32}
    local fail_prepare,fail_apply=false,false
    Restore.capture_font=function() return copy(selected) end
    Restore.default_font=function() return {version=1,active=true,font=0,path="",size=16} end
    Restore.prepare_font=function(snapshot)
        if fail_prepare then return nil,"font prepare failed" end
        return copy(snapshot)
    end
    Restore.discard_font=function() end
    Restore.apply_font=function(snapshot)
        if fail_apply then return false,"font upload failed" end
        selected=copy(snapshot); return true
    end
    Restore.clear_font=function() selected={version=1,active=false}; return true end
    local current=fresh()
    selected={version=1,active=true,font=1,path="",size=32}
    current.text_state={font_face="outdated requested name",draws={}}
    save.save(current,{slot=31,thumbnail="u11"})
    check("font capture uses actual backend selection",slots[31].font_snapshot
        and slots[31].font_snapshot.font==1)
    selected={version=1,active=true,font=0,path="",size=16}
    fail_prepare=true
    local accepted=save.load(current,{slot=31})
    check("font preparation failure preserves old context and font",accepted==false
        and runner.get_ctx()==current and selected.font==0)
    fail_prepare=false
    accepted=save.load(current,{slot=31})
    check("font restore installs the saved actual selection",accepted==true and selected.font==1)
    fail_apply=true
    accepted=save.load(runner.get_ctx(),{slot=31})
    check("font upload failure stops the restored session",accepted==false and runner.get_ctx()==nil)
    check("font upload failure clears the old font",selected.active==false)
    fail_apply=false
    local next_owner=fresh()
    check("explicit start restores a usable default after failure",selected.active==true and selected.font==0)
    require("kag.presentation").stop(current,true)
    check("late old cleanup cannot clear a newer font",selected.active==true and runner.get_ctx()==next_owner)
    local legacy=save.capture_state(next_owner)
    legacy.font_snapshot=nil
    slots[31]=legacy
    selected={version=1,active=true,font=1,path="",size=32}
    assert(save.load(next_owner,{slot=31}))
    check("legacy missing font uses the stable host default",selected.font==0)
    local commit_locale=i18n.commit
    i18n.commit=function() error("injected locale commit failure") end
    accepted=save.load(runner.get_ctx(),{slot=31})
    i18n.commit=commit_locale
    check("failure after applying the font stops the context",accepted==false and runner.get_ctx()==nil)
    check("failure after applying the font clears it",selected.active==false)
    runner.stop()
    local direct={f={future=true},tf={},_session_active=true,waiting_input=true}
    local font_was_applied=false
    i18n.commit=function()
        font_was_applied=selected.active==true
        error("injected direct host locale failure")
    end
    accepted=save.load(direct,{slot=31})
    i18n.commit=commit_locale
    check("direct host failure is injected after font application",font_was_applied)
    check("direct host failure stops the session",accepted==false
        and direct.stop_flag and not direct._session_active and not direct.waiting_input)
    check("direct host failure clears the applied font",selected.active==false)
end
io.open = original_open
print(string.format("U11 RESTORE TRANSACTION: %d passed, %d failed", passed, failed))
os.exit(failed == 0 and 0 or 1)
