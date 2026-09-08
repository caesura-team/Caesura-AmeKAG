-- Real tokenizer/compiler/scheduler/runner rollback regressions. Host bindings
-- and scene reads are replaced; one test command owns a real Operation scope.
-- Run in isolation from the main suite's sandbox and cached mocks.
package.path = 'scripts/?.lua;scripts/?/init.lua;' .. package.path

local function callable(fields)
    return setmetatable(fields or {}, { __index = function(self, key)
        if type(key) ~= 'string' then return nil end
        local fn = function() return true end
        rawset(self, key, fn)
        return fn
    end })
end
_G.KAG = callable({
    is_voice_playing = function() return false end,
    is_bgm_playing = function() return false end,
    get_active_voices = function() return 0 end,
})
_G.Engine, _G.Render, _G.DevCore = callable(), callable(), callable()
local sources = {
    ['u12-memory-linear.ks'] = [[
[set var="f.after_a" value=0]
[set var="lf.local_value" value=10]
[ch text="ALPHA"]
[set var="f.after_a" value=1]
[set var="lf.local_value" value=20]
[ch text="BRAVO"]
[set var="f.after_b" value=1]
[ch text="CHARLIE"]
[end]
]],
    ['u12-memory-inline.ks'] = [[
[set var="f.value" value=10]
[ch text="ALPHA"]
[set var="f.value" value=20]
[rollback]
[set var="f.leaked" value=1]
[ch text="UNREACHABLE"]
]],
    ['u12-memory-scope.ks'] = [[
[set var="f.value" value=10]
[ch text="ALPHA"]
[set var="f.value" value=20]
[u12_old_scope]
[set var="f.leaked" value=1]
]],
    ['u12-memory-page.ks'] = [[
[ch text="ALPHA"]
[p]
[set var="f.after_page" value=1]
[ch text="BRAVO"]
]],
}
sources['u12-memory-bounded.ks'] = table.concat({
    '[ch text="P1"]', '[ch text="P2"]', '[ch text="P3"]',
    '[ch text="P4"]', '[ch text="P5"]',
}, '\n')
local flow = require('flow')
flow.load_scene = function(path)
    local tokens = require('tokenizer').parse(assert(sources[path]))
    require('kag.compiler').compile(tokens)
    return { tokens = tokens, labels = tokens._compiled.labels, path = path, base_path = path }
end
local runner = require('kag_runner')
local text_scene = require('kag.text_scene')
local backend = require('backend')
local operation = require('kag.operation')
local passed, failed = 0, 0
local function check(label, ok)
    if ok then passed = passed + 1 else failed = failed + 1 end
    print((ok and 'PASS ' or 'FAIL ') .. label)
end
local function page(c)
    local output = {}
    text_scene.render(c, { render_text = function(text) output[#output + 1] = text end })
    return table.concat(output, '|')
end
local function draws(c)
    local output = {}
    for _, draw in ipairs(c.text_state and c.text_state.draws or {}) do
        output[#output + 1] = draw.text or ''
    end
    return table.concat(output, '|')
end
local function reach_wait()
    for _ = 1, 32 do
        local c = runner.get_ctx()
        if c.waiting_input and not c._pendingRollback then return true end
        runner.update(0)
    end
    return false
end
local function reveal()
    local c = runner.get_ctx()
    if c.reveal and c.text_state.reveal_chars < c.reveal.total then return runner.on_click() end
    return true, 'already-visible'
end
local function start(name)
    assert(runner.start('u12-memory-' .. name .. '.ks'))
    assert(reach_wait())
    return runner.get_ctx()
end
local function advance()
    reveal()
    assert(runner.on_click())
    assert(reach_wait())
end

local c = start('linear')
check('setup reaches actual ALPHA click wait', draws(c) == 'ALPHA')
local initial_history = #c._undoStack
local ok, reason = reveal()
check('first click reveals A without pushing history', ok and reason == 'revealed'
    and #c._undoStack == initial_history and page(c) == 'ALPHA')
check('second click advances to B', runner.on_click() and reach_wait() and draws(c) == 'BRAVO')
check('A continuation executes before B', c.f.after_a == 1 and c.lf.local_value == 20)
c.lf.only_future = true
check('A click creates exactly one real snapshot', #c._undoStack == 1)
reveal()
local old_co = c.co
local voice_stops, original_audio_stop = 0, backend.audio_stop
backend.audio_stop = function(channel)
    if channel == 'voice' then voice_stops = voice_stops + 1 end
end
check('public rollback accepts the actual A snapshot', runner.rollback() == true)
check('rollback stops voice through the module without a backend global',
    rawget(_G, 'backend') == nil and voice_stops == 1)
backend.audio_stop = original_audio_stop
check('rollback displays A immediately', page(c) == 'ALPHA')
check('rollback restores pre-A-continuation f value', c.f.after_a == 0)
check('rollback restores pre-A-continuation lf value', c.lf.local_value == 10 and c.lf.only_future == nil)
check('rollback consumes exactly one history entry', #c._undoStack == 0)
runner.update(0.016)
check('first update after rollback keeps A fully displayed', page(c) == 'ALPHA')
check('rollback reaches a stable click wait without user input', reach_wait())
check('rollback settlement still displays A', page(c) == 'ALPHA')
check('retired B coroutine is dead before continuation', coroutine.status(old_co) == 'dead')
check('settlement does not execute A continuation', c.f.after_a == 0)
check('rollback respawn clears stop_flag', not c.stop_flag)
local overlay_pumps = 0
c._gesture_history_co = coroutine.create(function() overlay_pumps = overlay_pumps + 1 end)
runner.update(0.016)
check('restored wait still services newly opened history overlay', overlay_pumps == 1)
local restored_draws = c.text_state.draws
runner.update(10)
check('settled checkpoint does not replay completed ch', c.text_state.draws == restored_draws and page(c) == 'ALPHA')
check('next advance after rollback succeeds', runner.on_click() == true and reach_wait())
reveal()
check('next advance reaches B exactly as normal A continuation', page(c) == 'BRAVO')
check('next advance executes A continuation', c.f.after_a == 1 and c.lf.local_value == 20)
check('next advance does not execute B continuation', c.f.after_b == nil)
advance()
reveal()
check('three pages create two history entries', page(c) == 'CHARLIE' and #c._undoStack == 2)
check('first consecutive rollback reaches B', runner.rollback() and page(c) == 'BRAVO' and #c._undoStack == 1)
check('second consecutive rollback reaches A', runner.rollback() and page(c) == 'ALPHA' and #c._undoStack == 0)
check('empty rollback preserves A', not runner.rollback() and page(c) == 'ALPHA')
runner.update(0.016)
check('consecutive rollback continuation remains stable', page(c) == 'ALPHA' and c.f.after_a == 0)
runner.stop()

c = start('inline')
reveal()
check('inline rollback request is accepted', runner.on_click() == true)
check('inline rollback waits for host boundary', c._pendingRollback ~= nil and c.f.value == 20)
old_co = c.co
runner.update(0.016)
check('inline commit restores A before another token', page(c) == 'ALPHA' and c.f.value == 10 and c.f.leaked == nil)
check('inline commit closes previous coroutine', coroutine.status(old_co) == 'dead')
runner.update(1)
check('inline rollback stays at A until a click', page(c) == 'ALPHA' and c.waiting_input and c.f.leaked == nil)
runner.stop()

local cleanups, completed, events, requests = 0, 0, {}, {}
local old_token
require('kag').u12_old_scope = function(owner)
    local scope <close> = operation.start(owner)
    old_token = scope.token
    scope.token:register(function()
        cleanups = cleanups + 1
        events[#events + 1] = 'cleanup:' .. tostring(owner.f.value)
        requests[#requests + 1] = 'cleanup-request'
    end)
    requests[#requests + 1] = 'old-request'
    owner.waiting_input = true
    coroutine.yield()
    completed = completed + 1
    scope:complete()
end
backend.cancel_async_loads = function()
    events[#events + 1] = 'cancel:' .. tostring(runner.get_ctx().f.value)
    requests = {}
end
backend.ai_cancel = function() events[#events + 1] = 'ai-cancel' end
c = start('scope')
cleanups, completed, events = 0, 0, {}
advance()
old_co = c.co
local old_tween = { cancelled = false }
c.tweens = { old_tween }
check('scope rollback succeeds', runner.rollback() == true)
check('old operation closes before snapshot installation', events[1] == 'cleanup:20')
check('cleanup-created work is invalidated before restore', events[2] == 'cancel:20'
    and events[3] == 'ai-cancel' and #requests == 0)
check('old scope closes exactly once without completion', cleanups == 1 and completed == 0
    and coroutine.status(old_co) == 'dead' and old_token.cancelled)
check('rollback cancels nonblocking work before next update', old_tween.cancelled and #c.tweens == 0)
check('scope cleanup cannot modify restored values', c.f.value == 10 and page(c) == 'ALPHA')
local next_scope = operation.start(c)
old_token:cancel()
runner.update(0.016)
check('retired cleanup is idempotent and leaves new operation alive', cleanups == 1
    and not next_scope.token.cancelled and #c.active_operations == 1)
next_scope:complete()
next_scope:__close()
runner.stop()

c = start('page')
advance()
check('explicit page break keeps current text until click', page(c) == 'ALPHA')
advance()
reveal()
check('page break continuation reaches B', page(c) == 'BRAVO' and c.f.after_page == 1)
check('rollback restores explicit page-break wait', runner.rollback() and page(c) == 'ALPHA')
runner.update(0.016)
check('restored page wait stays visible', page(c) == 'ALPHA' and c.f.after_page == nil)
advance()
reveal()
check('restored page click clears and continues once', page(c) == 'BRAVO' and c.f.after_page == 1)
runner.stop()

c = start('bounded')
c._undoLimit = 2
for _ = 1, 4 do advance() end
reveal()
check('real click history retains configured upper bound', #c._undoStack == 2 and page(c) == 'P5')
check('bounded history rolls back to newest retained point', runner.rollback() and page(c) == 'P4')
check('bounded history rolls back to oldest retained point', runner.rollback() and page(c) == 'P3')
check('evicted click points cannot be restored', not runner.rollback() and #c._undoStack == 0)
runner.stop()

c = start('linear')
c.auto_mode, c.skip_mode = true, 'all'
advance()
check('rollback accepts auto-skip checkpoint', runner.rollback() == true)
runner.update(100)
check('auto-skip cannot advance restored checkpoint without input', page(c) == 'ALPHA'
    and c.f.after_a == 0 and c.waiting_input)
advance()
check('explicit click continues restored auto-skip checkpoint', draws(c) == 'BRAVO' and c.f.after_a == 1)
runner.stop()

print(string.format('ROLLBACK SESSION TESTS: %d passed, %d failed', passed, failed))
assert(failed == 0, 'rollback session regression')
