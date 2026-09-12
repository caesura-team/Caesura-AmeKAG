-- U17: real tokenizer/compiler/scheduler/runner and commands. Only audio
-- host bindings are controlled. This test runs in its own orphan-suite VM.
package.path = "scripts/?.lua;scripts/?/init.lua;" .. package.path
local stage, mode = "tests/scripts", "integrated"
local created_scenes = {}
local function cleanup_scenes()
    for _, path in ipairs(created_scenes) do os.remove(path) end
    created_scenes = {}
end
local scene_scope <close> = setmetatable({}, { __close = cleanup_scenes })

local state = { playing=false, paused=false, stops=0, polls=0, plays=0 }
local function playing()
    state.polls = state.polls + 1
    if state.query_error then error("S8 injected audio query error") end
    return state.playing -- A paused voice remains allocated/playing.
end
_G.KAG = {
    play_voice = function(path)
        assert(path == "assets/voice/line01.wav")
        state.playing, state.plays = true, state.plays + 1
        return state.plays
    end,
    stop_voice = function()
        state.stops, state.playing = state.stops + 1, false
        if state.stop_error then error("S8 injected audio stop error") end
        return true
    end,
    is_voice_playing = playing,
    is_bgm_playing = function() return false end,
    stop_bgm = function() return true end,
    stop_se = function() return true end,
}

local runner = require("kag_runner")
local backend = require("backend")
local passed, failed, sequence = 0, 0, 0
local function check(name, condition)
    print((condition and "PASS " or "FAIL ") .. name)
    if condition then passed = passed + 1 else failed = failed + 1 end
end
local function frames(count)
    for _ = 1, count do runner.update(.016) end
end
local script = [[
[set var="f.before" value=1]
[voice_wait]
[set var="f.done" value=1]
[p]
[set var="f.beyond_page" value=1]
[end]
]]
local function scene()
    sequence = sequence + 1
    local path = stage .. "/_u17_voice_wait_" .. sequence .. ".ks"
    local existing = io.open(path,"rb")
    if existing then existing:close(); error("Test scene already exists: " .. path) end
    local file = assert(io.open(path,"wb")); created_scenes[#created_scenes+1]=path
    file:write(script); file:close()
    return path
end
local function begin(has_voice)
    state.query_error, state.stop_error = false, false
    _G.KAG.is_voice_playing = playing
    assert(runner.stop())
    assert(runner.start(scene()))
    local ctx = runner.get_ctx()
    assert(ctx.f.before == 1, "real first command must have yielded")
    local before = state.stops
    if has_voice then assert(backend.audio_play("voice", "assets/voice/line01.wav")) end
    runner.update(.016) -- Actual scheduler enters voice_wait after owner play.
    return ctx, before
end
local function at_page(name, ctx, before, expected_stops)
    check(name .. ": next command reached",ctx.f.done == 1)
    check(name .. ": following page remains held",ctx.waiting_input == true and ctx.f.beyond_page == nil)
    check(name .. ": voice stops",state.stops - before == expected_stops)
    frames(4)
    check(name .. ": no duplicate advance",ctx.f.beyond_page == nil)
end

do
    local ctx, before = begin(true)
    frames(4)
    check("normal: playing voice holds command",ctx.f.done == nil and state.playing)
    check("normal: waiting does not stop voice",state.stops == before)
    state.paused = true; frames(4)
    check("normal: paused voice is not natural completion",ctx.f.done == nil and state.playing)
    state.paused, state.playing = false, false -- Controlled natural completion.
    frames(4)
    at_page("normal completion without click",ctx,before,0)
end
do
    local ctx, before = begin(true)
    assert(runner.on_click())
    at_page("click skip",ctx,before,1)
end
do
    local ctx, before = begin(false)
    frames(4)
    at_page("no voice",ctx,before,0)
end
do
    local ctx, before = begin(true)
    _G.KAG.is_voice_playing = nil -- Host lacks the voice query binding.
    frames(4)
    at_page("absent audio query",ctx,before,0)
    _G.KAG.is_voice_playing = playing
end
do
    local ctx, before = begin(true)
    state.stop_error = true
    assert(runner.on_click())
    at_page("click stop failure still clears wait",ctx,before,1)
    state.stop_error = false
end
do
    local ctx, before = begin(true)
    ctx.skip_mode = "all"
    runner.update(.016)
    check("skip mode stops once",state.stops-before == 1)
    check("skip mode advances voice command",ctx.f.done == 1)
    ctx.skip_mode = nil
end
do
    local ctx, before = begin(true)
    ctx.auto_mode, ctx.auto_delay = true, 1500
    frames(4)
    check("auto mode holds playing voice",ctx.f.done == nil and state.stops == before)
    state.playing = false
    frames(4)
    at_page("auto natural completion",ctx,before,0)
    ctx.auto_mode = false
end
do
    local old, before = begin(true)
    local old_co = old.co
    assert(runner.stop())
    check("cancel closes old coroutine",coroutine.status(old_co)=="dead")
    check("cancel clears wait ownership",not old.waiting_input and not old._voice_wait_poll)
    check("cancel stops session audio once",state.stops-before == 1)
    state.playing = false; frames(3)
    check("cancel cannot advance old next command",old.f.done == nil)
end
do
    local old, before = begin(true)
    local old_co = old.co
    assert(runner.start(scene(), {replace=true}))
    local successor = runner.get_ctx()
    check("replace closes old owner",successor~=old and coroutine.status(old_co)=="dead"
        and not old.waiting_input and not old._voice_wait_poll)
    check("replace stops prior session audio once",state.stops-before==1)
    local after_replace = state.stops
    assert(backend.audio_play("voice", "assets/voice/line01.wav"))
    runner.update(.016); frames(3)
    check("replace successor waits independently",successor.f.done==nil and state.playing
        and state.stops==after_replace and old.f.done==nil)
    state.playing=false; frames(4)
    at_page("successor natural completion",successor,after_replace,0)
    check("old owner stays retired",old.f.done==nil)
end
do
    local ctx, before = begin(true)
    ctx.skip_mode = "seen" -- This voice point has not been marked as read.
    frames(3)
    check("unread skip holds playing voice",ctx.f.done == nil and state.stops==before)
    state.playing = false; frames(4)
    at_page("unread skip natural completion",ctx,before,0)
    ctx.skip_mode = nil
end
do
    local ctx, before = begin(true)
    state.query_error = true
    runner.update(.016)
    check("audio query failure reaches scheduler error contract",ctx.error_command == "voice_wait")
    state.query_error = false; frames(4)
    at_page("audio query failure cleanup",ctx,before,0)
    check("audio query failure clears poll owner",ctx._voice_wait_poll == nil)
end
state.query_error, state.stop_error = false, false
assert(runner.stop())
print(string.format("S8 %s: %d passed, %d failed; controlled audio host only",mode,passed,failed))
cleanup_scenes()
if failed>0 then os.exit(1) end
