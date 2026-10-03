-- =============================================================================
--  test_select_crossscene_flow.lua — [select] deferred pending-jump vs
--  cross-scene [jump] (engine defect found by t33 / Golden Project v1).
--
--  The [select] handler resolves the chosen option via a DEFERRED pending-jump
--  (scripts/kag/commands/text.lua:1475 sets ctx._pendingJump = selected.target;
--  scripts/kag/kag_runner.lua consumes it in the dead-coroutine branch). A
--  cross-scene [jump] swaps ctx.tokens/labelMap BEFORE that consumption, so the
--  stale label jump resolves against the NEW scene's labels -- "Choice label
--  not found" -- and the run stalls not-running forever.
--
--  Pinned semantics (this suite):
--    A. select -> same-scene [jump *label] -> [end]  : chosen branch must
--       still run (deferred re-run walk-through preserved, no regression).
--    B. select -> immediate cross-scene [jump]       : after the fix the run
--       must NOT stall; the NEW scene must advance and reach [end] (DONE).
--       The stale scene-local pending-jump is dropped with a WARN (labels are
--       scene-local; the explicit [jump] is the new authority). The pre-fix
--       behavior in this configuration was a permanent not-running stall.
--
--  Standalone driver (orphan suite): mocks KAG/Render/DevCore/backend and
--  requires kag_runner -- exactly the global-mock shape run_orphan_tests.lua
--  requires. Creates disposable .ks fixtures under tmp/ (best-effort cleanup).
--
--  Usage:  build/lua/Debug/lua.exe tests/scripts/test_select_crossscene_flow.lua
--  Exit:   0 = all checks passed, 1 = any check failed.
-- =============================================================================

package.path = "scripts/?.lua;scripts/?/init.lua;scripts/kag/?.lua;scripts/kag/commands/?.lua;" .. package.path

local DIR = "tmp/test_select_crossscene"
local IS_WINDOWS = package.config:sub(1, 1) == "\\"

-- ---- Mock C++ bindings (identical to sample_game_headless.lua) ----
local function callable(t)
    return setmetatable(t or {}, {
        __index = function(self, key)
            if type(key) ~= "string" then return nil end
            rawset(self, key, function(...) return true end)
            return self[key]
        end,
    })
end

_G.KAG = callable({
    is_voice_playing  = function() return false end,
    is_bgm_playing    = function() return false end,
    get_active_voices = function() return 0 end,
})
_G.Render  = callable({})
_G.DevCore = callable({})
_G.Engine  = callable({})
_G.backend = callable({
    is_voice_playing  = function() return false end,
    is_bgm_playing    = function() return false end,
    get_active_voices = function() return 0 end,
})

local kag_runner = require("kag_runner")

local passed, failed = 0, 0
local function check(name, cond, detail)
    if cond then
        passed = passed + 1
    else
        failed = failed + 1
        print(string.format("  [FAIL] %s  -- %s", name, detail or ""))
    end
end

local function write_file(path, text)
    if IS_WINDOWS then
        pcall(os.execute, 'mkdir "' .. DIR:gsub('/', '\\') .. '" 2>nul')
    else
        pcall(os.execute, 'mkdir -p "' .. DIR .. '" 2>/dev/null')
    end
    local f = io.open(path, "w")
    if not f then return false end
    f:write(text)
    f:close()
    return true
end

local MAIN_FLAT = [[
*start
[select]
[sel target=*branch_a text="A / 甲"]
[sel target=*branch_b text="B / 乙"]
[endselect]

*branch_a
[set var="f.route" value="a"]
[jump *theend]

*branch_b
[set var="f.route" value="b"]
[jump *theend]

*theend
[ch name="N" text="end"]
[p]
[end]
]]

local MAIN_CROSS = [[
*start
[select]
[sel target=*branch_a text="A / 甲"]
[sel target=*branch_b text="B / 乙"]
[endselect]

*branch_a
[set var="f.route" value="a"]
[jump storage="sub_b.ks"]

*branch_b
[set var="f.route" value="b"]
[jump storage="sub_b.ks"]
]]

local SUB_B = [[
*start
[set var="f.sceneB" value=1]
[ch name="N" text="scene B"]
[p]
[end]
]]

-- ---- driver: start(ksPath, crossSeam[, seamPath]) -> {result, route, sceneB} ---
local function drive(story, crossSeam, seamPath)
    local started, err = kag_runner.start(story)
    if not started then
        print("FATAL_START: " .. tostring(err))
        return { result = "FATAL_START", route = "?", sceneB = nil }
    end
    local ctx0 = _G._CAESURA_CTX
    if crossSeam then
        local real_load = ctx0.load_tokens
        local want = seamPath or "assets/script/sub_b.ks"
        ctx0.load_tokens = function(path)
            if path == want then
                return real_load(crossSeam)
            end
            return real_load(path)
        end
    end
    local FMAX = 5000
    local frames, clicks = 0, 0
    local result = nil
    while frames < FMAX do
        frames = frames + 1
        local ok, reason = kag_runner.update(0.016)
        local ctx = _G._CAESURA_CTX
        if reason == "ended" then
            result = "DONE:" .. frames
            break
        end
        if ctx and ctx.tokens and ctx.token_index
           and ctx.token_index > #ctx.tokens then
            result = "DONE(exhausted):" .. frames
            break
        end
        if ctx and (ctx.waiting_input or ctx._choiceMode) then
            clicks = clicks + 1
            -- choose option 2 (B/乙) -- deferred pending-jump target *branch_b
            if ctx._choiceMode then
                _G._GAME_MOUSE_X = 100
                local cbs = ctx._choiceButtonsActive or ctx._choiceButtons
                local idx = (cbs and #cbs >= 2) and 2 or 1
                local ch = cbs and cbs[idx]
                _G._GAME_MOUSE_Y = (ch and tonumber(ch.y) or 450)
                                 + (ch and tonumber(ch.h) or 24) / 2
                local kc = _G._KAG_onClick
                if type(kc) == "function" then kc() end
                if ctx._choiceMode and type(cbs) == "table" and cbs[idx] then
                    ctx._selectedChoice      = cbs[idx]
                    ctx._choiceMode          = false
                    ctx._choiceButtonsActive = nil
                    ctx.waiting_input        = false
                end
            else
                kag_runner.on_click()
            end
        end
    end
    if not result then result = "FRAME_LIMIT" end
    local ctx = _G._CAESURA_CTX
    return {
        result = result,
        route  = (ctx and ctx.f and ctx.f.route) or "none",
        sceneB = (ctx and ctx.f and ctx.f.sceneB) or nil,
    }
end

print("=== [select] deferred pending-jump vs cross-scene [jump] Tests ===")

write_file(DIR .. "/main_flat.ks", MAIN_FLAT)
write_file(DIR .. "/main_cross.ks", MAIN_CROSS)
write_file(DIR .. "/sub_b.ks", SUB_B)

--  Case A: select -> same-scene label jump -> [end]  (NO regression)
--  Chosen option B: the deferred pending-jump must still re-run branch_b
--  (first_vn/golden semantics: walk-through first passage, then the chosen
--  branch runs; final f.route must be the CHOSEN one).
local aOut = drive(DIR .. "/main_flat.ks", nil)
print("  [A] " .. aOut.result .. " route=" .. tostring(aOut.route))
check("A: same-scene select flow reaches DONE", aOut.result:sub(1, 4) == "DONE", aOut.result)
check("A: chosen branch B runs (route=b, deferred jump intact)",
      aOut.route == "b", "route=" .. tostring(aOut.route))

--  Case B: select -> IMMEDIATE cross-scene [jump]  (the t33 stall)
--  Pre-fix: stale pending-jump (*branch_b) resolves against sub_b's labels ->
--  not-running stall -> FRAME_LIMIT. Post-fix: the switch wins, the stale
--  scene-local pending-jump is dropped with a WARN, sub_b advances to [end].
local bOut = drive(DIR .. "/main_cross.ks", DIR .. "/sub_b.ks")
print("  [B] " .. bOut.result .. " route=" .. tostring(bOut.route)
      .. " sceneB=" .. tostring(bOut.sceneB))
check("B: select -> cross-scene jump does NOT stall (DONE)",
      bOut.result:sub(1, 4) == "DONE", bOut.result)
check("B: new scene advances (sceneB=1, sub_b ran to [end])",
      bOut.sceneB == 1, "sceneB=" .. tostring(bOut.sceneB))

--  Case C: select -> cross-scene [call sub] -> [return] -> [end]  (t43)
--  The deferred choice jump must STILL resolve: [return] restores the
--  caller's tokens/labels AND clears the scene-switch signal (_scene_changed
--  at scheduler [return] / implicit-return restore), so the chosen-branch
--  replay is preserved. Regressor detector: the linear first passage leaves
--  f.route = "a" (branch_a runs, then [jump *theend]); only the REPLAY of
--  the chosen branch writes f.route = "b" -- a blanket drop yields "a".
local MAIN_CALL = [[
*start
[select]
[sel target=*branch_a text="A / 甲"]
[sel target=*branch_b text="B / 乙"]
[endselect]

*branch_a
[set var="f.route" value="a"]
[jump *theend]

*branch_b
[set var="f.route" value="b"]
[jump *theend]

*theend
[call sub_ret.ks]
[end]
]]

local SUB_RET = [[
*start
[set var="f.sceneB" value=1]
[return]
]]

write_file(DIR .. "/main_call.ks", MAIN_CALL)
write_file(DIR .. "/sub_ret.ks", SUB_RET)
local cOut = drive(DIR .. "/main_call.ks", DIR .. "/sub_ret.ks",
                   "assets/script/sub_ret.ks")
print("  [C] " .. cOut.result .. " route=" .. tostring(cOut.route)
      .. " sceneB=" .. tostring(cOut.sceneB))
check("C: select -> call-with-return does NOT stall (DONE)",
      cOut.result:sub(1, 4) == "DONE", cOut.result)
check("C: chosen branch replay preserved after [call] (route=b, not a)",
      cOut.route == "b", "route=" .. tostring(cOut.route))
check("C: the [call] itself executed (sub ran, sceneB=1)",
      cOut.sceneB == 1, "sceneB=" .. tostring(cOut.sceneB))

--  Case E (t49): a [select] INSIDE the callee that is NOT consumed before
--  [return] must not stall the caller. The deferred jump's label belongs to
--  the scene being LEFT (callee); at [return] the caller's labelMap is
--  restored and has no such label -- resolving it there printed "Choice
--  label not found: <callee-label>" and stalled (FRAME_LIMIT, t46 finding).
--  Post-fix: the return-restore point validates the pending jump against the
--  restored scene and loudly drops the dangling one; flow continues to DONE.
local MAIN_CSEL = [[
*start
[call sub_select.ks]
[set var="f.route" value="caller-done"]
[end]
]]

local SUB_SELECT = [[
*start
[select]
[sel target=*callee_opt text="X / x"]
[endselect]
*callee_opt
[set var="f.sceneB" value=1]
[return]
]]

write_file(DIR .. "/main_csel.ks", MAIN_CSEL)
write_file(DIR .. "/sub_select.ks", SUB_SELECT)
local eOut = drive(DIR .. "/main_csel.ks", DIR .. "/sub_select.ks",
                   "assets/script/sub_select.ks")
print("  [E] " .. eOut.result .. " route=" .. tostring(eOut.route)
      .. " sceneB=" .. tostring(eOut.sceneB))
check("E: callee-select un-consumed before [return] does NOT stall (DONE)",
      eOut.result:sub(1, 4) == "DONE", eOut.result)
check("E: caller tail ran to [end] (route=caller-done)",
      eOut.route == "caller-done", "route=" .. tostring(eOut.route))

-- AE3 regression: [call] enters a real callee scene, whose [select] is
-- followed by [end]. The pending choice must resolve inside that callee
-- before [return], unlike Case E's deliberately unconsumed fall-through.
-- Actual tokenizer/compiler/scheduler/runner and choice click hook are used;
-- only the existing host rendering bindings above are substitutes.
local CALL_DIR = "assets/script/test_select_crossscene_call"
if IS_WINDOWS then
    assert(os.execute('mkdir "' .. CALL_DIR:gsub('/', '\\') .. '" 2>nul'))
else
    assert(os.execute('mkdir -p "' .. CALL_DIR .. '"'))
end
local function call_fixture(name, text)
    local file = assert(io.open(CALL_DIR .. '/' .. name, 'w'))
    file:write(text)
    file:close()
end
call_fixture('main.ks', [[
[set var="lf.owner" value="caller"]
[call storage="test_select_crossscene_call/choice.ks"]
[set var="f.tail" value=1]
[end]
]])
call_fixture('nested.ks', [[
[set var="lf.owner" value="caller"]
[call storage="test_select_crossscene_call/middle.ks"]
[set var="f.tail" value=1]
[end]
]])
call_fixture('middle.ks', [[
[set var="lf.owner" value="middle"]
[call storage="test_select_crossscene_call/choice.ks"]
[eval exp="f.middle_return = lf.owner"]
[return]
]])
call_fixture('choice.ks', [[
[set var="lf.owner" value="callee"]
[select]
[sel target="*left" text="LEFT"]
[sel target="*right" text="RIGHT"]
[endselect]
[end]
*left
[set var="f.branch" value="left"]
[return]
*right
[set var="f.branch" value="right"]
[return]
]])
local function called_choice(story, selected)
    assert(kag_runner.start(CALL_DIR .. '/' .. story))
    local owner = kag_runner.get_ctx()
    local clicked, ended = 0, false
    for frame = 1, 400 do
        local _, reason = kag_runner.update(0.016)
        if reason == 'ended' then ended = true; break end
        local current = kag_runner.get_ctx()
        assert(current == owner, 'test unexpectedly replaced the session')
        if current._choiceMode then
            local buttons = assert(current._choiceButtonsActive)
            local button = assert(buttons[selected])
            _G._GAME_MOUSE_X = 100
            _G._GAME_MOUSE_Y = button.y + button.h / 2
            assert(type(_G._KAG_onClick) == 'function')
            _G._KAG_onClick()
            clicked = clicked + 1
        end
    end
    return {ended=ended, clicked=clicked, branch=owner.f.branch,
            tail=owner.f.tail, middle=owner.f.middle_return,
            owner=owner.lf.owner, call_depth=#(owner.call_stack or {})}
end
for _, story in ipairs({'main.ks', 'nested.ks'}) do
    for selected, expected in ipairs({'left', 'right'}) do
        local result = called_choice(story, selected)
        local label = 'F ' .. story .. ' ' .. expected
        check(label .. ': actual click selects callee-local branch',
              result.clicked == 1 and result.branch == expected,
              'clicks=' .. result.clicked .. ', branch=' .. tostring(result.branch))
        check(label .. ': caller resumes and ends', result.ended and result.tail == 1)
        check(label .. ': caller local frame restored', result.owner == 'caller' and result.call_depth == 0)
        if story == 'nested.ks' then
            check(label .. ': intermediate local frame restored', result.middle == 'middle')
        end
    end
end
for _, name in ipairs({'main.ks', 'nested.ks', 'middle.ks', 'choice.ks'}) do
    assert(os.remove(CALL_DIR .. '/' .. name))
end
if IS_WINDOWS then
    assert(os.execute('rmdir "' .. CALL_DIR:gsub('/', '\\') .. '"'))
else
    assert(os.execute('rmdir "' .. CALL_DIR .. '"'))
end

if IS_WINDOWS then
    pcall(os.execute, 'rmdir /s /q "' .. DIR:gsub('/', '\\') .. '" 2>nul')
else
    pcall(os.execute, 'rm -rf "' .. DIR .. '" 2>/dev/null')
end

print("")
print(string.format("select/cross-scene flow tests: %d passed, %d failed", passed, failed))
os.exit(failed == 0 and 0 or 1)
