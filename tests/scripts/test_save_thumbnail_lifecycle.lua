-- Real SaveCommands and Operation; only host capture/storage boundaries are controlled.
-- Native SaveBinding + SaveManager disk integration lives in test_save_binding.cpp.
package.path = "scripts/?.lua;scripts/?/init.lua;" .. package.path
local Save = require("kag.commands.save")
local Operation = require("kag.operation")
require("layers").init()
local passed, failed = 0, 0
local function check(name, value)
    if value then passed = passed + 1
    else failed = failed + 1; print("FAIL: " .. name) end
end
local function fresh()
    return { f={route="A", nested={value=1}}, sf={}, tf={}, lf={}, mp={},
        variables={}, current_scene="tests/scripts/A.ks", token_index=7 }
end
local writes, requests, released = {}, {}, 0
local function reset()
    writes, requests, released = {}, {}, 0
    _G.KAG = {
        save_game = function(slot, state, scene, token, thumbnail)
            writes[#writes+1] = {slot=slot,state=state,scene=scene,token=token,thumbnail=thumbnail}
            return true
        end,
        capture_thumbnail = function(token)
            local request = {done=false, cancelled=false}
            requests[#requests+1] = request
            local function release()
                if request.released then return end
                request.released = true
                released = released + 1
            end
            local guard <close> = setmetatable({}, {__close=release})
            if token then token:register(function() request.cancelled=true; release() end) end
            repeat coroutine.yield() until request.done or request.cancelled
            if request.cancelled then return nil, "cancelled", "owner-cancelled" end
            return request.image, request.status or "completed", request.error
        end,
    }
end
local function start(ctx, params)
    local co = coroutine.create(function() Save.save(ctx, params) end)
    local ok, err = coroutine.resume(co)
    assert(ok, err)
    return co
end
local function resume(co, ...)
    local ok, err = coroutine.resume(co, ...)
    assert(ok, err)
end

reset()
do
    local ctx, params = fresh(), {slot=3, desc="frozen description"}
    local co = start(ctx, params)
    check("pending capture does not publish a slot", #writes == 0 and coroutine.status(co)=="suspended")
    check("save owns a cancellable operation while pending", ctx.active_operations and #ctx.active_operations==1)
    ctx.f.route, ctx.f.nested.value = "B", 2
    ctx.current_scene, ctx.token_index, params.slot = "tests/scripts/B.ks", 99, 4
    requests[1].done, requests[1].image = true, "QQ=="
    resume(co, 0.016, "extra resume arguments")
    check("slot state scene token and description freeze before capture", #writes==1
        and writes[1].slot==3 and writes[1].scene=="tests/scripts/A.ks"
        and writes[1].token==7 and writes[1].state.f.route=="A"
        and writes[1].state.f.nested.value==1 and writes[1].state.description=="frozen description")
    check("normal completion releases capture and operation", released==1
        and ctx.active_operations and #ctx.active_operations==0)
end

reset()
do
    local ctx = fresh()
    local a = start(ctx, {slot=1})
    ctx.f.route, ctx.current_scene = "B", "tests/scripts/B.ks"
    local b = start(ctx, {slot=2})
    requests[2].done, requests[2].image = true, "Qg=="
    resume(b)
    check("B can complete before A without committing A", #writes==1 and writes[1].slot==2
        and writes[1].thumbnail=="Qg==" and coroutine.status(a)=="suspended")
    requests[1].done, requests[1].image = true, "QQ=="
    resume(a)
    check("delayed A keeps its own image and scene", #writes==2 and writes[2].slot==1
        and writes[2].thumbnail=="QQ==" and writes[2].scene=="tests/scripts/A.ks")
end

for _, mode in ipairs({"cancel_all", "stop", "session", "cancelled-result", "close"}) do
    reset()
    local ctx = fresh()
    local co = start(ctx, {slot=8})
    if mode=="cancel_all" then Operation.cancel_all(ctx)
    elseif mode=="stop" then ctx.stop_flag=true
    elseif mode=="session" then ctx._session_active=false
    elseif mode=="cancelled-result" then
        requests[1].status, requests[1].error = "cancelled", "device-lost"
    elseif mode=="close" then assert(coroutine.close(co)) end
    if mode~="close" then
        requests[1].done, requests[1].image = true, mode=="cancelled-result" and nil or "late-image"
        resume(co)
    end
    check(mode .. " prevents every late slot commit", #writes==0)
    check(mode .. " releases capture ownership", released==1)
end

reset()
do
    local ctx = fresh()
    local a = start(ctx, {slot=5})
    local b = start(ctx, {slot=5})
    check("same owner and slot refuses a second pending save", #requests==1
        and coroutine.status(b)=="dead" and ctx.tf.save_error=="save-busy")
    assert(coroutine.close(a))
    if coroutine.status(b)~="dead" then assert(coroutine.close(b)) end
    local c = start(ctx, {slot=5})
    check("closing old save frees the slot for reuse", #requests>=2 and coroutine.status(c)=="suspended")
    assert(coroutine.close(c))
end

for _, outcome in ipairs({"failed", "unavailable"}) do
    reset()
    local ctx = fresh()
    _G.KAG.capture_thumbnail = function() return nil, outcome, "optional-image-unavailable" end
    Save.save(ctx, {slot=6})
    check(outcome .. " permits explicit state-only fallback", #writes==1 and writes[1].thumbnail=="")
    check(outcome .. " remains visible separately from successful save", ctx.tf.save_result=="ok"
        and ctx.tf.thumbnail_result==outcome and ctx.tf.thumbnail_error=="optional-image-unavailable")
end

reset()
do
    local ctx = fresh()
    Save.save(ctx, {slot=9, thumbnail="explicit-base64"})
    check("explicit thumbnail bypasses capture", #requests==0 and #writes==1
        and writes[1].thumbnail=="explicit-base64")
    ctx.captureThumbnail = function(token)
        check("custom hook receives operation token", token and type(token.register)=="function")
        return "custom-image"
    end
    Save.save(ctx, {slot=10})
    check("custom capture hook remains preferred", #requests==0 and #writes==2
        and writes[2].thumbnail=="custom-image")
end

reset()
do
    local ctx = fresh()
    local unrelated = Operation.start(ctx)
    local ok, reason = Save.save(ctx, {slot=11, thumbnail="explicit"})
    check("unrelated pending effects still block capture", ok==false
        and tostring(reason):find("unfinished",1,true) and #writes==0 and #requests==0)
    unrelated:cancel()
    unrelated:__close()
end

print(string.format("Save thumbnail lifecycle: %d passed, %d failed", passed, failed))
if failed>0 then os.exit(1) end
