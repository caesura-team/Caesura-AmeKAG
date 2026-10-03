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
    local state = {paths = {}, starts = 0, stops = 0, videos = {}, video_stops = 0, audio_stops = {}, audio_calls = {}, draws = {}, offsets = {}}
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

for _, command in ipairs({"typewriter", "typewriter_sound"}) do
    for _, case in ipairs({
        {name="canonical", attrs='sound="primary.wav"', sound="primary.wav"},
        {name="file alias", attrs='file="alias.wav"', sound="alias.wav"},
        {name="both prefer sound", attrs='sound="primary.wav" file="alias.wav"', sound="primary.wav"},
        {name="omitted default", attrs='', sound=""},
    }) do
        local env,state,start=fixture()
        local ctx,co=start('['..command..' '..case.attrs..']\n[end]')
        check(command..' '..case.name,ctx.typewriter_sound==case.sound)
        assert(coroutine.close(co))
    end
end
for _, case in ipairs({
    {name="fadeout alpha alias", tag='fadeout alpha=0.5 time=32', target=127,frames=4,node="bg"},
    {name="fadeout opacity canonical", tag='fadeout opacity=0.5 time=32',target=127,frames=4,node="bg"},
    {name="fadeout both opacity wins",tag='fadeout opacity=0.25 alpha=0.5 time=32',target=63,frames=4,node="bg"},
    {name="fadeout tiny opacity stays byte one",tag='fadeout opacity=0.005 time=0',target=1,frames=2,node="bg"},
    {name="fadeout duration alias",tag='fadeout opacity=0.5 duration=32',target=127,frames=4,node="bg"},
    {name="fadeout both time wins",tag='fadeout opacity=0.5 time=32 duration=48',target=127,frames=4,node="bg"},
    {name="fadeout name alias",tag='fadeout name="alt" opacity=0.5 time=32',target=127,frames=4,node="alt"},
    {name="fadeout omitted defaults",tag='fadeout',target=0,frames=34,node="bg"},
    {name="layfade duration alias",tag='layfade to=128 duration=32',target=128,frames=4,node="bg"},
    {name="layfade both time wins",tag='layfade to=128 time=32 duration=48',target=128,frames=4,node="bg"},
    {name="layfade name alias",tag='layfade name="alt" to=128 time=32',target=128,frames=4,node="alt"},
    {name="layfade omitted defaults",tag='layfade',target=255,frames=21,node="bg"},
}) do
    local env,state,start=fixture();local layers=env.require("layers")
    local nodes={}
    for _,name in ipairs({"bg","alt"}) do nodes[name]=layers.add_layer(layers.get_root(),{id=name,name=name,opacity=255,w=0,h=0}) end
    local ctx,co=start('['..case.tag..']\n[end]')
    print('ALIAS_FADE_OBSERVED '..case.name..' bg='..nodes.bg.opacity..' alt='..nodes.alt.opacity..' frames='..ctx._test_frames)
    check(case.name,nodes[case.node].opacity==case.target and ctx._test_frames==case.frames)
    check(case.name..' does not change other layer',nodes[case.node=="bg" and "alt" or "bg"].opacity==255)
    assert(coroutine.close(co))
end
for _, command in ipairs({"position","move"}) do
    for _,case in ipairs({
        {name="name alias",attrs='name="alt"',node="alt"},
        {name="layer canonical",attrs='layer="alt"',node="alt"},
        {name="both prefer layer",attrs='layer="fg" name="alt"',node="fg"},
        {name="omitted layer defaults fg",attrs='',node="fg"},
    }) do
        local env,state,start=fixture();local layers=env.require("layers");local nodes={}
        for _,name in ipairs({"fg","alt"}) do nodes[name]=layers.add_layer(layers.get_root(),{id=name,name=name,w=0,h=0}) end
        local timing=command=="move" and ' time=32' or ''
        local ctx,co=start('['..command..' '..case.attrs..' x=0.25 y=0.5'..timing..']\n[end]')
        local node=nodes[case.node];local x=command=="position" and node.pos_x or node.x;local y=command=="position" and node.pos_y or node.y
        print('ALIAS_POSITION_OBSERVED '..command..' '..case.name..' x='..tostring(x)..' y='..tostring(y))
        check(command..' '..case.name,x==0.25 and y==0.5)
        assert(coroutine.close(co))
    end
end
for _,case in ipairs({
    {name="button caption alias",command="button",attrs='caption="ALIAS"',expected="ALIAS"},
    {name="button canonical",command="button",attrs='text="PRIMARY"',expected="PRIMARY"},
    {name="button both prefer text",command="button",attrs='text="PRIMARY" caption="ALIAS"',expected="PRIMARY"},
    {name="text message alias",command="text",attrs='message="ALIAS"',expected="ALIAS"},
    {name="text content alias",command="text",attrs='content="CONTENT"',expected="CONTENT"},
    {name="text canonical",command="text",attrs='text="PRIMARY"',expected="PRIMARY"},
    {name="text both prefer text",command="text",attrs='text="PRIMARY" message="ALIAS"',expected="PRIMARY"},
    {name="ch message alias",command="ch",attrs='message="ALIAS" character="ALIAS_NAME"',expected="ALIAS",speaker="ALIAS_NAME"},
    {name="ch canonical",command="ch",attrs='text="PRIMARY" name="PRIMARY_NAME"',expected="PRIMARY",speaker="PRIMARY_NAME"},
    {name="ch both prefer canonical",command="ch",attrs='text="PRIMARY" message="ALIAS" name="PRIMARY_NAME" character="ALIAS_NAME"',expected="PRIMARY",speaker="PRIMARY_NAME"},
}) do
    local env,state,start=fixture();local ctx,co=start('['..case.command..' '..case.attrs..']\n[end]')
    local actual=case.command=="button" and ctx._choiceButtons[1].text or (ctx.backlog and ctx.backlog[#ctx.backlog] and ctx.backlog[#ctx.backlog].text)
    print('ALIAS_TEXT_OBSERVED '..case.name..' value='..tostring(actual)..' speaker='..tostring(ctx.current_speaker))
    check(case.name,actual==case.expected and (not case.speaker or ctx.current_speaker==case.speaker))
    assert(coroutine.close(co))
end
for _,case in ipairs({
    {name="ch voicefile alias",attrs='voicefile="alias.wav"',expected="alias.wav"},
    {name="ch voice canonical",attrs='voice="primary.wav"',expected="primary.wav"},
    {name="ch both prefer voice",attrs='voice="primary.wav" voicefile="alias.wav"',expected="primary.wav"},
}) do
    local env,state,start=fixture();local ctx,co=start('[ch text="LINE" '..case.attrs..']\n[end]')
    local actual
    for _,call in ipairs(state.audio_calls) do if call[1]=="play_voice" then actual=call[2] end end
    print('ALIAS_VOICE_OBSERVED '..case.name..' file='..tostring(actual))
    check(case.name,actual==case.expected and ctx.backlog[#ctx.backlog].voice==case.expected)
    assert(coroutine.close(co))
end
for _,case in ipairs({
    {name="scroll positional alias",attrs='"ALIAS"',expected="ALIAS"},
    {name="scroll canonical",attrs='text="PRIMARY"',expected="PRIMARY"},
    {name="scroll both prefer text",attrs='text="PRIMARY" "ALIAS"',expected="PRIMARY"},
}) do
    local env,state,start=fixture();local ctx,co=start('[scroll '..case.attrs..' speed=1000]\n[end]')
    print('ALIAS_SCROLL_OBSERVED '..case.name..' draws='..#state.draws..' text='..tostring(state.draws[1] and state.draws[1][1]))
    check(case.name,#state.draws>0 and state.draws[1][1]==case.expected)
    assert(coroutine.close(co))
end
do
    local env,state,start=fixture()
    local ctx,co,ok,err=start('[camera x=10 y=20 time=32]\n[end]',true)
    local target=false
    for _,offset in ipairs(state.offsets) do if offset[1]==10 and offset[2]==20 then target=true end end
    local last=state.offsets[#state.offsets]
    check("camera Lua54 easing reaches target then restores",ok and not ctx._command_error and target
        and last and last[1]==0 and last[2]==0)
    coroutine.close(co)
end
do
    local env,state,start=fixture();local schema=env.require("kag.schema")
    local calls=0;local context={f={probe=function() calls=calls+1;return "RUNTIME" end}}
    local source="${f.probe()}"
    local static,fields=schema.validate_static("text",{message=source},context)
    check("alias static validation preserves dynamic canonical value",static.text==source and calls==0 and fields[1]=="text")
    local runtime=schema.coerce("text",static,context)
    check("alias runtime evaluates preserved expression once",runtime.text=="RUNTIME" and calls==1)
    local twice=schema.coerce("text",runtime,context)
    check("second coerce keeps runtime value instead of default",twice.text=="RUNTIME" and calls==1)
    local bounded=schema.coerce("fadeout",{alpha="9",duration="32000"},{})
    check("aliases share canonical numeric bounds",bounded.opacity==1 and bounded.time==30000)
    local zero=schema.coerce("fadeout",{opacity=0,alpha=0.5,time=0,duration=32},{})
    check("explicit canonical zero beats alias",zero.opacity==0 and zero.time==0)
    local valid,error_text=pcall(schema.coerce,"fadeout",{alpha="bad"},{})
    check("invalid numeric alias is rejected",not valid and tostring(error_text):find("expects a number",1,true)~=nil)
    local empty=schema.coerce("text",{text="",message="SHADOW"},{})
    check("explicit empty canonical text beats alias",empty.text=="")
    local compiler=env.require("kag.compiler");local tokens=env.require("tokenizer").parse('[fadeout alpha=0.5 time=0]')
    compiler.compile(tokens);local packed=assert(compiler.serialize(tokens))
    local spec=schema.specs("fadeout").opacity;local old_alias=spec.aliases[1];spec.aliases[1]="changed_alias"
    check("alias metadata participates in cache identity",compiler.validateSerialized(packed)==false)
    spec.aliases[1]=old_alias
    check("exact restored alias contract restores compatibility",compiler.validateSerialized(packed)==true)
end
for _,command in ipairs({"text","button"}) do
    local env,state,start=fixture()
    local field=command=="text" and "message" or "caption"
    local ctx,co=start('[set var="f.label" value="RUNTIME"]\n['..command..' '..field..'="${f.label}"]\n[end]')
    local actual=command=="text" and ctx.backlog[#ctx.backlog].text or ctx._choiceButtons[1].text
    check(command.." alias interpolates through actual compiled dispatch",actual=="RUNTIME")
    assert(coroutine.close(co))
end
for _,case in ipairs({
    {name="fade duration alias",attrs='duration=32 from=255 to=128',target=128,frames=4},
    {name="fade canonical time",attrs='time=32 from=255 to=128',target=128,frames=4},
    {name="fade both prefer time",attrs='time=32 duration=48 from=255 to=128',target=128,frames=4},
    {name="fade explicit zero wins",attrs='time=0 duration=32 from=255 to=128',target=128,frames=2},
    {name="fade omitted defaults",attrs='',target=255,frames=34},
}) do
    local env,state,start=fixture();local layers=env.require("layers")
    local node=layers.add_layer(layers.get_root(),{id="fg",name="fg",w=0,h=0,opacity=200})
    local ctx,co=start('[fade '..case.attrs..']\n[end]')
    print('FADE_DURATION_OBSERVED '..case.name..' opacity='..node.opacity..' frames='..ctx._test_frames)
    check(case.name,node.opacity==case.target and ctx._test_frames==case.frames)
    assert(coroutine.close(co))
end
print(string.format("COMMAND ALIAS PRECEDENCE: %d passed, %d failed",passed,failed))
assert(failed==0,"command alias precedence failed")
