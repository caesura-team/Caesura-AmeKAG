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
    local modules = {lpeg = real_lpeg}
    env.package = {loaded = modules, path = "scripts/?.lua;scripts/?/init.lua",
        config = package.config, cpath = package.cpath}
    local state = {paths = {}, starts = 0, stops = 0, videos = {}, video_stops = 0, audio_stops = {},
        colors = {}, text = {}, batches = {}, destroyed = {}, font_size = 18, next_texture = 100}
    local tickets = {}
    env.Restore = {
        capture_font = function()
            return {version = 1, active = true, font = 2, path = "assets/fixture.ttf", size = state.font_size}
        end,
        prepare_font = function(snapshot)
            local ticket = {size = snapshot.size, face = state.face}
            tickets[ticket] = true; return ticket
        end,
        apply_font = function(ticket)
            assert(tickets[ticket], "font ticket is not owned")
            tickets[ticket] = nil; state.font_size = ticket.size; state.face = ticket.face; return true
        end,
        discard_font = function(ticket)
            assert(tickets[ticket], "font ticket is not owned"); tickets[ticket] = nil
        end,
    }
    state.font_tickets = tickets
    env._CAESURA_BACKEND = {
        render = function(method, ...)
            if method == "load_texture_async" then
                local path, callback = ...
                state.paths[#state.paths + 1] = path
                if state.async_rejected_id ~= nil then return state.async_rejected_id end
                if state.reject_texture then callback(false, path, 0)
                else callback(true, path, 41) end
                return 1
            elseif method == "create_solid_texture" then
                state.next_texture = state.next_texture + 1
                state.colors[state.next_texture] = {...}; return state.next_texture
            elseif method == "destroy_texture" then
                local id = ...; state.destroyed[id] = (state.destroyed[id] or 0) + 1; return true
            elseif method == "create_viewport" then return 501
            elseif method == "destroy_viewport" then return true
            elseif method == "submit_batch" then
                local batch = ...; local copy = {}; for i = 1, 1 + batch[1] * 16 do copy[i] = batch[i] end
                state.batches[#state.batches + 1] = copy; return true
            elseif method == "text_set_font" then
                local face, size = ...; state.font_size = size or 24; state.face = face; return true
            elseif method == "render_text" then
                local values = {...}; values.effective_size = state.font_size * (values[8] or 1)
                state.text[#state.text + 1] = values; return true
            elseif method == "line_height" then return 99 -- line spacing is intentionally not the 18px font size
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
    local function start(source, allow_error, setup)
        local tokens = tokenizer.parse(source)
        compiler.compile(tokens)
        local ctx = {f = {}, sf = {}, tf = {}, mp = {}, lf = {}, variables = {},
            current_scene = "input-appearance.ks", token_index = 1, tokens = tokens,
            call_stack = {}, macro_args = {}, _session_active = true}
        if setup then setup(ctx) end
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


for _, command in ipairs({"input", "edit"}) do
    for _, finish in ipairs({"enter", "cancel", "close"}) do
        local env, state, start = fixture()
        local layers = env.require("layers")
        local user = layers.add_layer(layers.get_root(), {id = "_text_input", w = 0, h = 0})
        user.tex = 99
        local before = layers.count()
        local ctx, co = start('[font face="fixture" size=18]\n[' .. command
            .. ' name="f.value" default="ABC" font_size=36 color="#123456" bg_color="#234567"'
            .. ' x=10 y=20 width=180 height=80 btn_cancel="Cancel"]\n[end]', false, function(prior)
                env.require("kag.text_scene").add_text(prior, "PREVIOUS", 40, 500,
                    {r=255,g=255,b=255,a=255}, "previous_page", 1, false, false, true)
            end)
        assert(ctx._inputMode and coroutine.status(co) == "suspended")
        layers.render()
        env.require("kag.text_scene").render(ctx)
        local text_ok, previous_ok, background, geometry = false, false, nil, false
        for _, values in ipairs(state.text) do
            if values[1] == "PREVIOUS" then previous_ok = values.effective_size == 18 end
            if values[1] == "ABC|" then
                text_ok = values[4] == 18 and values[5] == 52 and values[6] == 86
                    and values.effective_size == 36
            end
        end
        for id, color in pairs(state.colors) do
            if color[1] == 35 and color[2] == 69 and color[3] == 103 and color[4] == 255 then background = id end
        end
        for _, batch in ipairs(state.batches) do
            for i = 0, batch[1] - 1 do
                local base = 1 + i * 16
                if batch[base + 2] == background then
                    geometry = batch[base + 4] == 10 and batch[base + 5] == 20
                        and batch[base + 6] == 180 and batch[base + 7] == 80
                end
            end
        end
        check(command .. " " .. finish .. " exact text color and 36px font", text_ok)
        check(command .. " " .. finish .. " preserves previous page 18px font while input is visible", previous_ok)
        check(command .. " " .. finish .. " background color and visible rectangle", background ~= nil and geometry)
        check(command .. " " .. finish .. " preserves user layer while active", layers.get_layer("_text_input") == user and user.tex == 99)
        if finish == "close" then assert(coroutine.close(co))
        else
            env._KAG_onKeyDown(finish == "enter" and 13 or 27, finish == "enter" and "return" or "escape")
            assert(drive(co, ctx))
        end
        check(command .. " " .. finish .. " releases own background only", background ~= nil
            and state.destroyed[background] == 1 and layers.count() == before
            and layers.get_layer("_text_input") == user and user.tex == 99 and not state.destroyed[99])
        check(command .. " " .. finish .. " restores font and input ownership", state.font_size == 18
            and state.face == "fixture" and ctx.text_state.font_size == 18
            and state.starts == 1 and state.stops == 1 and not ctx._inputMode
            and next(state.font_tickets) == nil)
        check(command .. " " .. finish .. " retains value commit semantics",
            finish == "enter" and ctx.f.value == "ABC" or finish ~= "enter" and ctx.f.value == nil)
        if coroutine.status(co) ~= "dead" then assert(coroutine.close(co)) end
    end
end
do
local env, state, start = fixture()
local ctx, co = start('[font face="fixture" size=18]\n[input name="f.value" x=1.5 y=20.5 width=180.5 height=80.5]\n[end]')
assert(ctx._inputMode)
print("IME_RECT_OBSERVED " .. table.concat(state.rect, ","))
check("fractional input rectangle covers its visual bounds with integer native pixels", state.rect[1] == 1
    and state.rect[2] == 20 and state.rect[3] == 181 and state.rect[4] == 81 and state.rect[5] == 0)
env.require("layers").render()
local floating = false
for _, batch in ipairs(state.batches) do
    for i = 0, batch[1] - 1 do
        local b = 1 + i * 16
        if batch[b+4] == 1.5 and batch[b+5] == 20.5 and batch[b+6] == 180.5 and batch[b+7] == 80.5 then floating = true end
    end
end
check("fractional rendering coordinates are retained", floating)
assert(coroutine.close(co))
check("fractional input still retires", not ctx._inputMode and state.starts == 1 and state.stops == 1
    and next(state.font_tickets) == nil and state.font_size == 18)
end

print(string.format("INPUT APPEARANCE: %d passed, %d failed", passed, failed))
assert(failed == 0, "input appearance failed")
