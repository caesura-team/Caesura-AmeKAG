-- test_video.lua — [video]/[stopvideo] contracts (audit)
package.path = "scripts/?.lua;scripts/kag/?.lua;" .. package.path
local passed, failed = 0, 0
local function check(name, cond)
    if cond then print("PASS " .. name) passed = passed + 1
    else print("FAIL " .. name) failed = failed + 1 end
end

local Video = package.loaded["kag.commands.video"] or require("kag.commands.video")
check("video handler", type(Video.video) == "function")
check("stopvideo handler", type(Video.stopvideo) == "function")

local KAG = require("kag")
check("registered on KAG", type(KAG.video) == "function"
      and type(KAG.stopvideo) == "function")

-- source: the 60s cap bounds the wait loop (same as waitsound)
local f = assert(io.open("scripts/kag/commands/video.lua", "r"))
local src = f:read("*a")
f:close()
check("wait capped", src:find("elapsed < 60000", 1, true) ~= nil)
check("cancel honored", src:find("not ct.cancelled and elapsed < 60000", 1, true) ~= nil)
-- loop is schema-boolean (no string dead branches)
check("loop boolean", src:find("params.loop == true", 1, true) ~= nil
      and not src:find('loop == "false"', 1, true))
-- stopvideo registered + schema typed (volume clamp)
local schema = require("kag.schema")
local coerced = schema.coerce("video", { file = "op.avi", volume = "9" })
check("volume clamped", coerced.volume == 1.5)

-- string-param sweep (audit): loop="true" (direct call, no coerce)
-- must loop; non-numeric opacity on fadeout must not raise
local Video2 = package.loaded["kag.commands.video"] or require("kag.commands.video")
local v_calls = {}
local be2 = _G._CAESURA_BACKEND
_G._CAESURA_BACKEND = { render = function(cmd, ...)
    if cmd == "video_play" then v_calls[#v_calls + 1] = { ... }; return 73 end
    if cmd == "video_is_playing" then return false end
    return true end }
local ctxV = { f = {}, tf = {}, sf = {}, mp = {}, variables = {}, viewport = { width = 1280, height = 720 } }
-- coroutine drive: video yields while waiting (non-coroutine pcall dies)
local coV = coroutine.create(function()
    Video2.video(ctxV, { file = "a.mpg", loop = "true", time = 1 })
end)
local okV = coroutine.resume(coV)
check("loop string tolerated", okV and v_calls[1] and v_calls[1][2].loop == true)
coroutine.close(coV)
_G._CAESURA_BACKEND = be2

local okF = pcall(KAG.fadeout, { f = {}, tf = {}, sf = {}, mp = {}, variables = {} },
                  { layer = "bg", opacity = "abc", time = 1 })
check("fadeout bad opacity no raise", okF)
_G._CAESURA_BACKEND = be2

-- Run the actual backend wrappers and the production factory forwarding path.
-- The safe asset C-boundary recorder uses the same session-token contract as C++.
-- Actual asset resolution and strict permission checks live in the native suite.
do
    local original_render, original_backend = _G.Render, _G._CAESURA_BACKEND
    local factory_file = assert(io.open("scripts/backend_factory.lua", "r"))
    local factory_source = factory_file:read("*a")
    factory_file:close()
    local function fixture(mode, options)
        options = options or {}
        local calls = {play={}, query={}, stop={}}
        local binding = {
            video_play = function(file, opts)
                calls.play[#calls.play+1]={file=file,options=opts}
                if options.open_error then error("injected open failure") end
                if options.bad_open then return options.open_result end
                return 347
            end,
            video_is_playing = function(handle)
                assert(handle==347, "query must receive the opened handle")
                calls.query[#calls.query+1]=handle
                if options.query_error then error("injected query failure") end
                return options.forever or #calls.query==1
            end,
            video_stop = function(handle)
                assert(handle==347, "close must receive the opened handle")
                calls.stop[#calls.stop+1]=handle
                return true
            end,
        }
        if mode=="factory" then
            local safe_binding = {video_asset_play=binding.video_play,
                video_asset_stop=binding.video_stop,video_asset_is_playing=binding.video_is_playing}
            local env = setmetatable({KAG=safe_binding,Render=binding,DevCore={},Engine={}}, {__index=_G})
            env._G=env
            local factory = assert(load(factory_source,"@scripts/backend_factory.lua","t",env))()
            _G._CAESURA_BACKEND=factory.create()
        else
            _G._CAESURA_BACKEND=nil
            _G.Render=binding
        end
        local ctx = {_session_active=true}
        local function start()
            local co=coroutine.create(function() return Video.video(ctx,{file="movie.mpg",volume=0.5,loop=true}) end)
            return co,coroutine.resume(co)
        end
        return ctx,calls,start
    end

    for _,mode in ipairs({"factory","direct"}) do
        local ctx,calls,start = fixture(mode)
        local co,resumed = start()
        check(mode..": actual handle is owned while playback is suspended", resumed and coroutine.status(co)=="suspended"
            and ctx._videoPlayback and ctx._videoPlayback.handle==347)
        check(mode..": open options reach safe asset binding unchanged", calls.play[1] and calls.play[1].file=="movie.mpg"
            and calls.play[1].options.volume==0.5 and calls.play[1].options.loop==true)
        resumed=coroutine.resume(co,16)
        check(mode..": natural completion queries and closes the opened handle", resumed and coroutine.status(co)=="dead"
            and #calls.query==2 and #calls.stop==1 and calls.stop[1]==347)
        check(mode..": natural completion releases owner and operation", ctx._videoPlayback==nil
            and #(ctx.active_operations or {})==0)
        coroutine.close(co)

        ctx,calls,start=fixture(mode,{forever=true})
        co,resumed=start()
        local token=ctx.active_operations and ctx.active_operations[1]
        local closed=coroutine.close(co)
        if token then token:cancel() end
        require("kag.operation").cancel_all(ctx)
        check(mode..": coroutine close and repeated cancellation close once", resumed and closed
            and #calls.stop==1 and calls.stop[1]==347 and ctx._videoPlayback==nil)

        ctx,calls,start=fixture(mode,{forever=true})
        co,resumed=start()
        require("kag.operation").cancel_all(ctx)
        closed=coroutine.close(co)
        check(mode..": cancellation before coroutine close is also idempotent", resumed and closed
            and #calls.stop==1 and ctx._videoPlayback==nil and #(ctx.active_operations or {})==0)

        ctx,calls,start=fixture(mode,{forever=true})
        co,resumed=start()
        local unrelated_ok=pcall(Video.stopvideo,{})
        check(mode..": stopvideo without an owner does not stop other playback", unrelated_ok and #calls.stop==0)
        local stopped=pcall(Video.stopvideo,ctx)
        local stopped_again=pcall(Video.stopvideo,ctx)
        check(mode..": stopvideo consumes only its owner handle once", stopped and stopped_again
            and #calls.stop==1 and ctx._videoPlayback==nil)
        local next_co,next_resumed=start() -- decoder reuses the same numeric handle
        local next_playback=ctx._videoPlayback
        coroutine.close(co)
        check(mode..": retired coroutine cannot close a replacement with a reused handle", next_resumed
            and #calls.stop==1 and ctx._videoPlayback==next_playback and next_playback~=nil)
        coroutine.close(next_co)
        check(mode..": replacement still closes its own playback", #calls.stop==2 and ctx._videoPlayback==nil)

        for _,failure in ipairs({
            {name="nil",bad_open=true}, {name="false",bad_open=true,open_result=false},
            {name="zero",bad_open=true,open_result=0}, {name="negative",bad_open=true,open_result=-1},
            {name="fraction",bad_open=true,open_result=1.5}, {name="boolean",bad_open=true,open_result=true},
            {name="string",bad_open=true,open_result="347"}, {name="infinite",bad_open=true,open_result=math.huge},
            {name="nan",bad_open=true,open_result=0/0},
            {name="overflow",bad_open=true,open_result=4294967296}, {name="throw",open_error=true},
        }) do
            ctx,calls,start=fixture(mode,failure)
            co,resumed=start()
            coroutine.close(co)
            check(mode..": failed open "..failure.name.." publishes no owner or invalid close",
                ctx._videoPlayback==nil and #calls.query==0 and #calls.stop==0
                and #(ctx.active_operations or {})==0 and (failure.open_error or resumed))
        end

        ctx,calls,start=fixture(mode,{forever=true})
        co,resumed=start()
        local finished=coroutine.resume(co,60000)
        check(mode..": timeout closes the owned loop handle", resumed and finished and coroutine.status(co)=="dead"
            and #calls.stop==1 and ctx._videoPlayback==nil)
        coroutine.close(co)

        ctx,calls,start=fixture(mode,{query_error=true})
        co,resumed=start()
        coroutine.close(co)
        check(mode..": query error is cleaned by closing the failed coroutine", not resumed
            and #calls.stop==1 and ctx._videoPlayback==nil and #(ctx.active_operations or {})==0)
    end
    _G.Render,_G._CAESURA_BACKEND=original_render,original_backend
end

print(string.format("VIDEO TESTS: %d passed, %d failed",passed,failed))
if failed > 0 then os.exit(1) end
print("VIDEO TESTS DONE")
