-- Execute production coercion and handlers; only native host calls are recorded.
-- A private module environment keeps this test independent of shared-suite mocks.
package.path = "scripts/?.lua;scripts/?/init.lua;" .. package.path
local passed, failed = 0, 0
local real_lpeg = require("lpeg")
local native_ime_rect = rawget(_G, "CONTRACT_NATIVE_IME_RECT")
local observed = function() return "no scene started" end
local function check(name, value)
    if value then passed = passed + 1; print("PASS " .. name)
    else failed = failed + 1; print("FAIL " .. name .. " -- " .. observed()) end
end

-- The scheduler yields after each ordinary token (scheduler.lua:1548).
-- These scenes contain at most three tokens. Drive at most 16 fixed 16ms
-- frames to either the real input boundary or [end], never clear input here.
local function drive(co, ctx)
    for _ = 1, 16 do
        if coroutine.status(co) == "dead" then return true end
        local ok, err = coroutine.resume(co, 16)
        ctx._test_frames = (ctx._test_frames or 0) + 1
        if not ok then ctx._test_error = tostring(err); return false, err end
        if ctx._command_error then return false, ctx.error_command end
        if ctx._inputMode then return true end
    end
    ctx._test_error = "16 frame fixture budget exhausted"
    return false, ctx._test_error
end

local function fixture()
    local env = setmetatable({}, {__index = _G})
    env._G = env
    env.Restore = {
        capture_font = function() return {version=1,active=true,font=0,path="",size=16} end,
        prepare_font = function(snapshot) return snapshot end,
        apply_font = function() return true end,
        discard_font = function() end,
    }
    local modules = {lpeg = real_lpeg}
    env.package = {loaded = modules, path = "scripts/?.lua;scripts/?/init.lua",
        config = package.config, cpath = package.cpath}
    local state = {paths = {}, starts = 0, stops = 0, videos = {}, video_stops = 0, audio_stops = {}}
    env._CAESURA_BACKEND = {
        render = function(method, ...)
            if method == "load_texture_async" then
                local path, callback = ...
                state.paths[#state.paths + 1] = path
                if state.async_rejected_id ~= nil then return state.async_rejected_id end
                if state.reject_texture then callback(false, path, 0)
                else callback(true, path, 41) end
                return 1
            elseif method == "create_solid_texture" then return 42
            elseif method == "create_viewport" then return 501
            elseif method == "destroy_viewport" then return true
            elseif method == "destroy_texture" or method == "text_set_font" then return true
            elseif method == "line_height" then return 24
            elseif method == "video_play" then
                state.videos[#state.videos + 1] = {...}; return 9
            elseif method == "video_is_playing" then return false
            elseif method == "video_stop" then state.video_stops = state.video_stops + 1; return true end
            error("Unexpected native render call: " .. tostring(method))
        end,
        audio = function(method, ...)
            if method == "stop_bgm" or method == "stop_voice" or method == "stop_se" then
                state.audio_stops[method] = true; return true
            elseif method == "play_se" then return false end
            error("Unexpected native audio call: " .. tostring(method))
        end,
        platform = function(method, ...)
            if method == "get_resolution" then return 1280, 720
            elseif method == "set_text_input_rect" then
                if native_ime_rect then native_ime_rect(...) end
                state.rect = {...}; return true
            elseif method == "start_text_input" then state.starts = state.starts + 1; return true
            elseif method == "stop_text_input" then state.stops = state.stops + 1; return true end
            error("Unexpected native platform call: " .. tostring(method))
        end,
    }
    env.require = function(name)
        if modules[name] == nil then
            local path = "scripts/" .. name:gsub("%.", "/") .. ".lua"
            local file = assert(io.open(path, "rb"))
            local source = file:read("*a"); file:close()
            source = source:gsub("^\239\187\191", "")
            modules[name] = assert(load(source, "@" .. path, "t", env))()
        end
        return modules[name]
    end
    local tokenizer = env.require("tokenizer")
    local compiler = env.require("kag.compiler")
    local scheduler = env.require("scheduler")
    env.require("kag")
    local function start(source, allow_error)
        local tokens = tokenizer.parse(source)
        compiler.compile(tokens)
        local ctx = {f = {}, sf = {}, tf = {}, mp = {}, lf = {}, variables = {},
            current_scene = "parameter-aliases.ks", token_index = 1, tokens = tokens,
            call_stack = {}, macro_args = {}, _session_active = true}
        local co = coroutine.create(function() scheduler.run(ctx, tokens, 1) end)
        observed = function()
            local resource = modules["kag.commands.resource"] or {}
            local function keys(t)
                local out = {}; for k,v in pairs(t or {}) do out[#out + 1] = tostring(k) .. ":" .. tostring(v) end
                table.sort(out); return table.concat(out, ",")
            end
            return table.concat({"co=" .. coroutine.status(co), "frames=" .. tostring(ctx._test_frames),
                "index=" .. tostring(ctx.token_index), "command=" .. tostring(ctx.error_command),
                "command_error=" .. tostring(ctx._command_error), "error=" .. tostring(ctx._test_error),
                "result=" .. tostring(ctx.f.result), "input=" .. tostring(ctx._inputMode),
                "starts/stops=" .. state.starts .. "/" .. state.stops,
                "paths=" .. table.concat(state.paths, ","), "texture_cache=" .. keys(resource._textureCache),
                "texture_pending=" .. keys(resource._pendingTextures), "audio_pending=" .. keys(resource._pendingAudio),
                "audio_cache=" .. keys(resource._audioCache), "unlocked=" .. keys(ctx.unlockedCG),
                "videos/stops=" .. #state.videos .. "/" .. state.video_stops,
                "video_file=" .. string.format("%q", tostring(state.videos[1] and state.videos[1][1])),
                "compiled_file=" .. string.format("%q", tostring(tokens._compiled.params[1].file)),
                "compiled_positional=" .. string.format("%q", tostring(tokens._compiled.params[1][1])),
                "source=" .. string.format("%q", source)}, "; ")
        end
        local ok, err = drive(co, ctx)
        if not allow_error then
            assert(ok, tostring(err))
            assert(not ctx._command_error, tostring(ctx.error_command))
        end
        return ctx, co, ok, err
    end
    return env, state, start
end

for _, case in ipairs({
    {name = "preload storage alias", params = 'storage="assets/alias.png"', expected = "assets/alias.png"},
    {name = "preload canonical path", params = 'path="assets/primary.png"', expected = "assets/primary.png"},
    {name = "preload explicit path wins", params = 'path="assets/primary.png" storage="assets/alias.png"', expected = "assets/primary.png"},
}) do
    local env, state, start = fixture()
    local ctx, co = start('[preload type="texture" wait="false" ' .. case.params .. ']\n[end]')
    check(case.name, coroutine.status(co) == "dead" and not ctx._command_error
        and #state.paths == 1 and state.paths[1] == case.expected
        and env.require("kag.commands.resource").is_loaded(case.expected))
    assert(coroutine.close(co))
end

for _, command in ipairs({"input", "edit"}) do
    for _, case in ipairs({
        {name = "max_length alias", params = "max_length=3", expected = "abc"},
        {name = "maxlen canonical", params = "maxlen=3", expected = "abc"},
        {name = "explicit maxlen wins", params = "maxlen=2 max_length=3", expected = "ab"},
        {name = "default length", params = "", expected = "abcde"},
    }) do
        local env, state, start = fixture()
        local ctx, co = start('[' .. command .. ' name="f.result" ' .. case.params .. ']\n[end]')
        assert(coroutine.status(co) == "suspended" and ctx._inputMode)
        env._KAG_onTextInput("abcde")
        env._KAG_onKeyDown(13, "return")
        assert(drive(co, ctx))
        check(command .. " " .. case.name, ctx.f.result == case.expected
            and coroutine.status(co) == "dead" and not ctx._command_error
            and state.starts == 1 and state.stops == 1 and not ctx._inputMode)
        assert(coroutine.close(co))
    end
end

for _, case in ipairs({
    {name = "video named file", source = '[video file="opening.mpg"]', valid = true},
    {name = "video positional file", source = '[video "opening.mpg"]', valid = true},
    {name = "video unquoted positional file", source = '[video opening.mpg]', valid = true},
    {name = "video single quoted positional file", source = "[video 'opening.mpg']", valid = true},
    {name = "video quoted space preserved", source = '[video "opening clip.mpg"]', valid = true, file = "opening clip.mpg"},
    {name = "video positional escape matches named convention", source = [=[[video "open\"ing.mpg"]]=], valid = true, file = [=[open\"ing.mpg]=]},
    {name = "video named escape unchanged", source = [=[[video file="open\"ing.mpg"]]=], valid = true, file = [=[open\"ing.mpg]=]},
    {name = "video empty quoted file rejected", source = '[video ""]', valid = false},
    {name = "video missing file rejected", source = '[video]', valid = false},
}) do
    local env, state, start = fixture()
    local ctx, co, ok, err = start(case.source .. '\n[end]', true)
    if case.valid then
        check(case.name, ok and not ctx._command_error and #state.videos == 1
            and state.videos[1][1] == (case.file or "opening.mpg") and state.video_stops == 1
            and coroutine.status(co) == "dead")
    else
        check(case.name, not ok and tostring(err):find("requires one of", 1, true) ~= nil
            and #state.videos == 0)
    end
    coroutine.close(co)
end

for _, close_scene in ipairs({false, true}) do
    local env, state, start = fixture()
    local ctx, co = start((close_scene and '[close]\n' or '') .. '[unlock id="after_close"]\n[end]')
    local reached = ctx.unlockedCG and ctx.unlockedCG.after_close == true
    check(close_scene and "close terminates before next command" or "normal command sentinel is reachable",
        coroutine.status(co) == "dead" and (close_scene and not reached or not close_scene and reached))
    if close_scene then
        check("close still cleans all audio buses", state.audio_stops.stop_bgm
            and state.audio_stops.stop_voice and state.audio_stops.stop_se)
    end
    assert(coroutine.close(co))
end

do
    local env, state, start = fixture()
    env._CAESURA_BACKEND = nil
    env.Render = {} -- Explicitly absent native host, not a failed queued request.
    local ctx, co = start('[preload type="texture" path="assets/no-host.png" wait="false"]\n[end]')
    local resource = env.require("kag.commands.resource")
    check("preload without async host leaves no pending request", not resource.is_pending("assets/no-host.png")
        and not resource.is_loaded("assets/no-host.png") and not ctx._command_error)
    assert(coroutine.close(co))
end
do
    local env, state, start = fixture()
    state.reject_texture = true
    local ctx, co = start('[preload type="texture" path="assets/rejected.png" wait="false"]\n[end]')
    local resource = env.require("kag.commands.resource")
    check("preload callback failure clears pending and does not cache", #state.paths == 1
        and not resource.is_pending("assets/rejected.png") and not resource.is_loaded("assets/rejected.png"))
    assert(coroutine.close(co))
end
do
    local env, state, start = fixture()
    local ctx, co = start('[preload type="audio" path="assets/rejected.wav" wait="false"]\n[end]')
    local resource = env.require("kag.commands.resource")
    check("preload audio false clears pending and does not cache", resource._pendingAudio["assets/rejected.wav"] == nil
        and resource._audioCache["assets/rejected.wav"] == nil and not state.audio_stops.stop_se)
    assert(coroutine.close(co))
end

for _, rejected in ipairs({0, -1}) do
    local env, state, start = fixture()
    state.async_rejected_id = rejected
    local ctx, co = start('[preload type="texture" path="assets/no-request.png" wait="false"]\n[end]')
    local resource = env.require("kag.commands.resource")
    check("preload rejected native request " .. rejected .. " leaves no pending", #state.paths == 1
        and not resource.is_pending("assets/no-request.png") and not resource.is_loaded("assets/no-request.png")
        and not ctx._command_error and coroutine.status(co) == "dead")
    assert(coroutine.close(co))
end

print(string.format("COMMAND PARAMETER ALIASES: %d passed, %d failed", passed, failed))
assert(failed == 0, "command parameter aliases failed")
