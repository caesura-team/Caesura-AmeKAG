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
-- These scenes contain at most three tokens. Drive at most 128 fixed 16ms
-- frames to either the real input boundary or [end], never clear input here.
local function drive(co, ctx)
    for _ = 1, 128 do
        if coroutine.status(co) == "dead" then return true end
        local ok, err = coroutine.resume(co, 16)
        ctx._test_frames = (ctx._test_frames or 0) + 1
        if not ok then ctx._test_error = tostring(err); return false, err end
        if ctx._command_error then return false, ctx.error_command end
        if ctx._inputMode then return true end
    end
    ctx._test_error = "128 frame fixture budget exhausted"
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
    local state = {paths = {}, starts = 0, stops = 0, videos = {}, video_stops = 0, audio_stops = {}, audio_calls = {}, draws = {}, offsets = {}, emitters = {}, colors = {}, warnings = {}}
    env.print = function(...)
        local fields={...}; for i,v in ipairs(fields) do fields[i]=tostring(v) end
        state.warnings[#state.warnings+1]=table.concat(fields," ")
        print(...)
    end
    env.VFX = {
        particles_create_emitter=function(config) state.emitters[#state.emitters+1]=config; return 7 end,
        particles_destroy_emitter=function() return true end,
    }
    env._CAESURA_BACKEND = {
        render = function(method, ...)
            if method == "load_texture_async" then
                local path, callback = ...
                state.paths[#state.paths + 1] = path
                if state.async_rejected_id ~= nil then return state.async_rejected_id end
                if state.reject_texture then callback(false, path, 0)
                else callback(true, path, 41) end
                return 1
            elseif method == "create_solid_texture" then state.colors[#state.colors+1]={...}; return 42
            elseif method == "create_viewport" then return 501
            elseif method == "destroy_viewport" then return true
            elseif method == "destroy_texture" or method == "text_set_font" then return true
            elseif method == "set_screen_offset" then state.offsets[#state.offsets + 1] = {...}; return true
            elseif method == "clear_text" then return true
            elseif method == "render_text" then state.draws[#state.draws + 1] = {...}; return true
            elseif method == "line_height" then return 24
            elseif method == "video_play" then
                state.videos[#state.videos + 1] = {...}; return 9
            elseif method == "video_is_playing" then return false
            elseif method == "video_stop" then state.video_stops = state.video_stops + 1; return true end
            error("Unexpected native render call: " .. tostring(method))
        end,
        audio = function(method, ...)
            state.audio_calls[#state.audio_calls + 1] = {method, ...}
            if method == "play_voice" or method == "play_se" then return true end
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

local undeclared_aliases={life_min=true,speed_min=true,size_min=true,angle_min=true}
local function warned(state,field)
    for _,line in ipairs(state.warnings) do if line:find("unknown param '"..field.."' ignored",1,true) then return true end end
    return false
end
local aliases={
    {"lifeMin","life_min",1.25,0.5},{"lifeMax","life_max",3.25,2},
    {"speedMin","speed_min",22,10},{"speedMax","speed_max",66,50},
    {"sizeMin","size_min",4,2},{"sizeMax","size_max",12,8},
    {"angleMin","angle_min",0.5,0},{"angleMax","angle_max",2.5,6.283},
    {"r","red",0.25,1},{"g","green",0.5,1},{"b","blue",0.75,1},{"a","alpha",0.5,1},
}
for _,field in ipairs(aliases) do
    for _,mode in ipairs({"alias","canonical","both","omitted"}) do
        local env,state,start=fixture();local attrs="";local expected=field[3]
        if mode=="alias" then attrs=field[2].."="..field[3]; if undeclared_aliases[field[2]] then expected=field[4] end
        elseif mode=="canonical" then attrs=field[1].."="..field[3]
        elseif mode=="both" then attrs=field[1].."=0 "..field[2].."="..field[3];expected=0
        else expected=field[4] end
        local ctx,co=start('[particles '..attrs..']\n[end]')
        local actual=state.emitters[1] and state.emitters[1][field[1]]
        print('PARTICLE_ALIAS_OBSERVED '..field[1]..' '..mode..' value='..tostring(actual))
        check('particles '..field[1]..' '..mode,#state.emitters==1 and actual==expected and ctx._particleEmitters[7]==true
            and (not undeclared_aliases[field[2]] or (mode~="alias" and mode~="both") or warned(state,field[2])))
        assert(coroutine.close(co))
    end
end
for _,case in ipairs({
    {name="flash undeclared RGB names warn and keep white",attrs='red=10 green=20 blue=30',rgb={255,255,255},warnings=true},
    {name="flash RGB canonical",attrs='r=10 g=20 b=30',rgb={10,20,30}},
    {name="flash canonical zero beats aliases",attrs='r=0 g=0 b=0 red=10 green=20 blue=30',rgb={0,0,0}},
    {name="flash omitted RGB remains white",attrs='',rgb={255,255,255}},
}) do
    local env,state,start=fixture();local ctx,co=start('[flash '..case.attrs..' time=32]\n[end]')
    local rgb=state.colors[1]
    print('FLASH_ALIAS_OBSERVED '..case.name..' rgb='..(rgb and table.concat(rgb,',') or 'nil'))
    check(case.name,rgb and rgb[1]==case.rgb[1] and rgb[2]==case.rgb[2] and rgb[3]==case.rgb[3]
        and (not case.warnings or (warned(state,"red") and warned(state,"green") and warned(state,"blue"))))
    assert(coroutine.close(co))
end
for _,case in ipairs({
    {name="quake amplitude alias",attrs='amplitude=12 time=32',amplitude=12,frames=4},
    {name="quake undeclared strength warns and keeps default",attrs='strength=12 time=32',amplitude=5,frames=4,warning="strength"},
    {name="quake undeclared power warns and keeps default",attrs='power=12 time=32',amplitude=5,frames=4,warning="power"},
    {name="quake intensity canonical",attrs='intensity=12 time=32',amplitude=12,frames=4},
    {name="quake explicit zero wins",attrs='intensity=0 amplitude=12 strength=18 time=32',amplitude=0,frames=4},
    {name="quake duration alias",attrs='intensity=12 duration=32',amplitude=12,frames=4},
    {name="quake omitted defaults",attrs='',amplitude=5,frames=21},
}) do
    local env,state,start=fixture();local layers=env.require("layers")
    local node=layers.add_layer(layers.get_root(),{id="probe",name="probe",w=0,h=0})
    local ctx,co=start('[quake '..case.attrs..']\n[end]')
    print('QUAKE_ALIAS_OBSERVED '..case.name..' amplitude='..tostring(node.quake.amplitude_x)..' frames='..ctx._test_frames)
    check(case.name,node.quake.amplitude_x==case.amplitude and node.quake.amplitude_y==case.amplitude
        and (not case.warning or warned(state,case.warning))
        and ctx._test_frames==case.frames and not node.quake.active and node.quake.offset_x==0 and node.quake.offset_y==0)
    assert(coroutine.close(co))
end
print(string.format("VFX PARAMETER ALIASES: %d passed, %d failed",passed,failed))
assert(failed==0,"VFX alias mapping failed")
