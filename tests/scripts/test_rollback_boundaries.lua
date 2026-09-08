-- Real tokenizer/compiler/scheduler/runner journeys across rollback barriers.
-- Only host bindings and scene reads are replaced; no synthetic snapshots.
package.path = 'scripts/?.lua;scripts/?/init.lua;' .. package.path

local function callable(fields)
    return setmetatable(fields or {}, { __index = function(self, key)
        if type(key) ~= 'string' then return nil end
        local fn = function() return true end
        rawset(self, key, fn)
        return fn
    end })
end
_G.KAG = callable({ is_voice_playing = function() return false end,
    is_bgm_playing = function() return false end,
    get_active_voices = function() return 0 end })
_G.Engine, _G.Render, _G.DevCore = callable(), callable(), callable()

local sources = {
    definition = [[
[ch text="BEFORE"]
[if exp="true"]
[macro name="dynamic"]
[ch text="DYNAMIC"]
[endmacro]
[endif]
[ch text="AFTER"]
]],
    redefine = [[
[macro name="dynamic"]
[ch text="OLD"]
[endmacro]
[ch text="BEFORE"]
[macro name="dynamic"]
[ch text="NEW"]
[endmacro]
[ch text="AFTER"]
]],
    erase = [[
[macro name="dynamic" args="who"]
[ch text="%who%"]
[endmacro]
[ch text="BEFORE"]
[erasemacro name="dynamic"]
[ch text="AFTER"]
]],
    absent_erase = [[
[macro name="retained"]
[endmacro]
[ch text="BEFORE"]
[erasemacro name="absent"]
[ch text="AFTER"]
]],
    dynamic = [[
[eval exp="f.reward=0; f.after=0"]
[if exp="true"]
[macro name="dynamic"]
[ch text="INNER1"]
[eval exp="f.reward=f.reward+1"]
[ch text="INNER2"]
[eval exp="f.reward=f.reward+1"]
[endmacro]
[endif]
[ch text="BEFORE"]
[dynamic]
[ch text="AFTER1"]
[eval exp="f.after=f.after+1"]
[ch text="AFTER2"]
]],
    nested = [[
[eval exp="f.reward=0"]
[macro name="outer"]
[macro name="inner"]
[ch text="INNER"]
[eval exp="f.reward=f.reward+1"]
[eval exp="f.reward=f.reward+1"]
[eval exp="f.reward=f.reward+1"]
[eval exp="f.reward=f.reward+1"]
[eval exp="f.reward=f.reward+1"]
[eval exp="f.reward=f.reward+1"]
[endmacro]
[inner]
[ch text="OUTER1"]
[ch text="OUTER2"]
[endmacro]
[ch text="BEFORE"]
[outer]
[ch text="AFTER"]
]],
    static = [[
[eval exp="f.reward=0"]
[macro name="fixed"]
[ch text="STATIC1"]
[eval exp="f.reward=f.reward+1"]
[ch text="STATIC2"]
[endmacro]
[ch text="BEFORE"]
[fixed]
[ch text="AFTER"]
]],
    choice = [[
[eval exp="f.reward=0"]
[ch text="BEFORE"]
[select]
[sel text="Take route" target="*route" x="f.selection"]
[endselect]
[end]
*route
[eval exp="f.reward=f.reward+1"]
[ch text="BRANCH1"]
[ch text="BRANCH2"]
]],
    hidden_choice = [[
[ch text="BEFORE"]
[button text="Hidden" target="*unreachable" cond="false"]
[endbutton]
[ch text="AFTER"]
[end]
*unreachable
[ch text="WRONG"]
]],
    empty_choice = '[ch text="BEFORE"]\n[endbutton]\n[ch text="AFTER"]',
}
local flow = require('flow')
flow.load_scene = function(path)
    local name = path:match('u12%-boundary%-(.+)%.ks$')
    local tokens = require('tokenizer').parse(assert(sources[name], path))
    require('kag.compiler').compile(tokens)
    return { tokens = tokens, labels = tokens._compiled.labels, path = path, base_path = path }
end
local runner = require('kag_runner')
local text_scene = require('kag.text_scene')
local passed, failed = 0, 0
local function check(label, ok)
    if ok then passed = passed + 1 else failed = failed + 1 end
    print((ok and 'PASS ' or 'FAIL ') .. label)
end
local function draws(c)
    local output = {}
    for _, draw in ipairs(c.text_state and c.text_state.draws or {}) do
        if draw.group ~= 'choices' then output[#output + 1] = draw.text or '' end
    end
    return table.concat(output, '|')
end
local function wait_for(predicate)
    for _ = 1, 128 do
        local c = runner.get_ctx()
        if predicate(c) then return c end
        runner.update(0)
    end
    error('runner did not reach expected state')
end
local function reach_wait()
    return wait_for(function(c) return c.waiting_input and not c._pendingRollback end)
end
local function reveal()
    local c = runner.get_ctx()
    if c.reveal and text_scene.get_state(c).reveal_chars < c.reveal.total then
        assert(runner.on_click())
    end
end
local function advance()
    reveal()
    assert(runner.on_click())
    return reach_wait()
end
local function start(name)
    assert(runner.start('u12-boundary-' .. name .. '.ks'))
    return reach_wait()
end
local function scenario(name, body)
    local ok, err = pcall(body)
    if not ok then check(name .. ' setup/continuation: ' .. tostring(err), false) end
    runner.stop()
end

for _, name in ipairs({ 'definition', 'redefine', 'erase' }) do
    scenario(name, function()
        local c = start(name)
        advance()
        check(name .. ' reaches AFTER', draws(c) == 'AFTER')
        check(name .. ' removes pre-mutation history', #c._undoStack == 0)
        if name == 'erase' then
            check('erase removes macro body and parameters', c.macros.dynamic == nil
                and c.macro_args.dynamic == nil)
        end
        local ok, reason = runner.rollback()
        check(name .. ' refuses stale history', not ok and reason == 'nothing-to-rollback')
        check(name .. ' refusal leaves AFTER unchanged', draws(c) == 'AFTER')
    end)
end

scenario('dynamic macro journey', function()
    local c = start('dynamic')
    advance()
    check('dynamic call actually splices tokens', c.tokens._runtime_rewritten == true)
    check('macro entry clears preceding history', draws(c) == 'INNER1' and #c._undoStack == 0)
    advance()
    check('macro ordinary continue reaches second line and rewards once', draws(c) == 'INNER2' and c.f.reward == 1)
    check('macro inner click does not capture an incomplete checkpoint', #c._undoStack == 0)
    advance()
    check('macro exit preserves continuation and total rewards', draws(c) == 'AFTER1' and c.f.reward == 2)
    check('macro final click does not leave an inner checkpoint', #c._undoStack == 0)
    advance()
    check('post-macro click starts new history', draws(c) == 'AFTER2' and #c._undoStack == 1)
    check('post-macro history rolls back normally', runner.rollback() and draws(c) == 'AFTER1'
        and c.f.reward == 2 and c.f.after == 0)
    advance()
    check('post-macro manual continue does not rerun macro reward', draws(c) == 'AFTER2'
        and c.f.reward == 2 and c.f.after == 1)
end)

scenario('macro rejection', function()
    local c = start('dynamic')
    advance()
    advance()
    local co, reward = c.co, c.f.reward
    local ok, reason = runner.rollback()
    check('active macro rollback returns an explanatory refusal', not ok and reason == 'macro-active')
    check('macro refusal preserves coroutine, page and rewards', c.co == co
        and draws(c) == 'INNER2' and c.f.reward == reward)
end)

scenario('nested expansion', function()
    local c = start('nested')
    advance()
    check('nested invocation reaches actual inner line', draws(c) == 'INNER')
    advance()
    check('nested expansion returns to outer tail with six rewards', draws(c) == 'OUTER1' and c.f.reward == 6)
    check('outer scope remains active after a growing inner splice', c._macroStack and #c._macroStack > 0)
    advance()
    check('outer tail continues without making macro history', draws(c) == 'OUTER2' and #c._undoStack == 0)
    advance()
    check('nested macro exits to outer continuation', draws(c) == 'AFTER')
    -- OUTER2 is the macro's final, completed token. Its checkpoint resumes
    -- outside every macro frame and is therefore an ordinary stable point.
    check('completed macro final line starts safe post-macro history', #c._undoStack == 1)
    check('nested macro retires all active frames at exit', #(c._macroStack or {}) == 0)
    check('nested macro exit preserves exactly six rewards', c.f.reward == 6)
    check('completed macro final line remains recoverable', runner.rollback() and draws(c) == 'OUTER2'
        and c.f.reward == 6)
    advance()
    check('completed macro final line continues without replaying reward', draws(c) == 'AFTER' and c.f.reward == 6)
end)

scenario('static expansion', function()
    local c = start('static')
    advance()
    check('static macro does not take runtime splice path', not c.tokens._runtime_rewritten
        and #(c._macroStack or {}) == 0)
    check('static macro keeps pre-call history', draws(c) == 'STATIC1' and #c._undoStack == 1)
    advance()
    check('static macro keeps interior history', draws(c) == 'STATIC2' and #c._undoStack == 2 and c.f.reward == 1)
    check('static macro interior rollback remains supported', runner.rollback() and draws(c) == 'STATIC1'
        and c.f.reward == 0)
    advance()
    check('static macro manual continue restores exactly one reward', draws(c) == 'STATIC2' and c.f.reward == 1)
end)

scenario('choice branch', function()
    local c = start('choice')
    advance()
    check('visible choice opens with history barrier already applied', c._choiceMode and #c._undoStack == 0)
    local ok, reason = runner.rollback()
    check('visible choice rollback refuses before empty-history check', not ok and reason == 'choice-open')
    ok, reason = runner.on_click()
    check('ordinary advance cannot dismiss a menu without selecting', not ok and reason == 'choice-open'
        and c._choiceMode and c.waiting_input)
    c.skip_mode = true
    runner.update(1)
    check('skip update cannot pass an unresolved choice', c._choiceMode and c.waiting_input and c.f.reward == 0)
    c.skip_mode, c.auto_mode = false, true
    runner.update(10)
    check('auto update cannot pass an unresolved choice', c._choiceMode and c.waiting_input and c.f.reward == 0)
    c.auto_mode = false
    local selected = assert(c._choiceButtonsActive[1])
    local stale_click = _G._KAG_onClick
    _G._GAME_MOUSE_X, _G._GAME_MOUSE_Y = 40, selected.y
    stale_click()
    check('selection accepts input without making history', not c._choiceMode and #c._undoStack == 0)
    wait_for(function(owner) return owner._pendingJump ~= nil end)
    check('real endselect stages deferred jump and result', c._pendingJump == '*route' and c.f.selection == '*route')
    ok, reason = runner.rollback()
    check('deferred choice cannot recover pre-branch history', not ok and reason == 'nothing-to-rollback')
    wait_for(function(owner) return owner.waiting_input and draws(owner) == 'BRANCH1' end)
    check('choice route executes reward once', c.f.reward == 1 and c._pendingJump == nil)
    stale_click()
    check('late choice callback cannot replay selection', c.f.reward == 1 and not c._choiceMode)
    advance()
    check('branch creates fresh linear history', draws(c) == 'BRANCH2' and #c._undoStack == 1)
    check('branch rollback restores its own first line', runner.rollback() and draws(c) == 'BRANCH1' and c.f.reward == 1)
    advance()
    check('branch manual continue does not duplicate selected reward', draws(c) == 'BRANCH2' and c.f.reward == 1)
end)

for _, name in ipairs({ 'hidden_choice', 'empty_choice', 'absent_erase' }) do
    scenario(name, function()
        local c = start(name)
        advance()
        check(name .. ' falls through and keeps legitimate history', draws(c) == 'AFTER'
            and #c._undoStack == 1 and not c._choiceMode)
        check(name .. ' previous line remains recoverable', runner.rollback() and draws(c) == 'BEFORE')
        advance()
        check(name .. ' manual continue still reaches AFTER', draws(c) == 'AFTER')
    end)
end

print(string.format('ROLLBACK BOUNDARIES TESTS: %d passed, %d failed', passed, failed))
assert(failed == 0, 'rollback boundaries regression')
