-- Real transition/operation/RTT modules; only the host render boundary is replaced.
package.path = "scripts/?.lua;scripts/kag/?.lua;" .. package.path
local passed, failed = 0, 0
local function check(name, cond)
    if cond then print("PASS " .. name); passed = passed + 1
    else print("FAIL " .. name); failed = failed + 1 end
end
local Transition = require("transition")
local T = require("kag.commands.transition")
local real_backend = _G._CAESURA_BACKEND
local frame, next_id = 1, 11
local calls, captures, destroyed = {}, {}, {}
_G._CAESURA_BACKEND = {render = function(cmd, ...)
    if cmd == "capture_scene" then
        local id = next_id; next_id = next_id + 1
        captures[#captures + 1] = {id, frame}
        return id, frame
    elseif cmd == "destroy_viewport" then destroyed[#destroyed + 1] = (...)
    elseif cmd == "submit_transition" then calls[#calls + 1] = {...}
    elseif cmd ~= "cancel_transition" then error("unexpected render command: " .. cmd) end
    return true
end}
local ctx = {f={}, tf={}, sf={}, mp={}, variables={}, _preloadPending=false}
local co = coroutine.create(function() T.trans(ctx, {method="wipe", time=32}) end)
local ok, err = coroutine.resume(co)
check("from hold suspends on real frame boundary", ok and coroutine.status(co)=="suspended")
check("numeric owned source held before destination", #calls==1 and calls[1][2]==11
    and calls[1][3]==11 and calls[1][5]==2 and calls[1][6]==0)
for _=1,3 do
    frame = frame + 1
    ok, err = coroutine.resume(co,16)
    if not ok then break end
end
check("real handler completes", ok and coroutine.status(co)=="dead")
check("destination copied from newer frame", #captures==2 and captures[2][2]>captures[1][2])
check("final actual submission reaches destination", #calls==4 and calls[4][2]==11
    and calls[4][3]==12 and calls[4][6]==1)
check("owned snapshots released once", #destroyed==2 and destroyed[1]==11 and destroyed[2]==12)
check("operation list empty", #(ctx.active_operations or {})==0)
local before = #calls
check("zero time immediate", pcall(T.trans, ctx, {time=0}) and #calls==before)
Transition.clear(ctx)
_G._CAESURA_BACKEND = real_backend
if failed > 0 then os.exit(1) end
print("TRANS BEHAVIOR TESTS DONE")
