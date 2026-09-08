-- test_operation.lua — Operation cancel semantics (audit)
package.path = "scripts/?.lua;scripts/kag/?.lua;" .. package.path
local passed, failed = 0, 0
local function check(name, cond)
    if cond then print("PASS " .. name) passed = passed + 1
    else print("FAIL " .. name) failed = failed + 1 end
end

local Operation = require("kag.operation")
local CancelToken = require("kag.cancel_token")

-- start registers a token; cancel_all cancels + fires callbacks in
-- reverse registration order
local ctx = {}
local order = {}
local op1 = Operation.start(ctx)
op1.token:register(function() order[#order + 1] = "cb1" end)
local op2 = Operation.start(ctx)
op2.token:register(function() order[#order + 1] = "cb2" end)
check("start registers tokens", #ctx.active_operations == 2)
Operation.cancel_all(ctx)
check("cancel_all clears list", type(ctx.active_operations) == "table"
      and #ctx.active_operations == 0)
check("tokens cancelled", op1.token.cancelled == true
      and op2.token.cancelled == true)
check("callbacks reverse order", order[1] == "cb2" and order[2] == "cb1")

-- cancel_all on a ctx without operations is a no-op
local ctx2 = {}
local ok = pcall(Operation.cancel_all, ctx2)
check("cancel_all nil-safe", ok == true)

-- A callback may cancel its own token again. Bound the re-entry so the
-- regression fails with duplicate calls instead of overflowing the stack.
local reentrant = CancelToken.new()
local calls = { a = 0, b = 0 }
local reentrant_order = {}
reentrant:register(function()
    calls.a = calls.a + 1
    reentrant_order[#reentrant_order + 1] = "a"
end)
reentrant:register(function()
    calls.b = calls.b + 1
    reentrant_order[#reentrant_order + 1] = "b"
    if calls.b == 1 then reentrant:cancel() end
end)
reentrant:cancel()
check("reentrant cancel invokes first callback once", calls.a == 1)
check("reentrant cancel invokes second callback once", calls.b == 1)
check("reentrant cancel preserves reverse order",
      table.concat(reentrant_order, ",") == "b,a")
reentrant:cancel()
check("cancel after re-entry does not replay callbacks", calls.a == 1 and calls.b == 1)

-- Registration during cancellation is immediate, and a callback error must
-- not prevent the remaining callbacks from releasing their resources.
local throwing = CancelToken.new()
local throwing_order = {}
throwing:register(function() throwing_order[#throwing_order + 1] = "a" end)
throwing:register(function()
    throwing_order[#throwing_order + 1] = "b"
    throwing:register(function() throwing_order[#throwing_order + 1] = "late" end)
    error("expected cleanup failure")
end)
check("callback errors stay contained", pcall(throwing.cancel, throwing))
check("late callback is immediate and errors preserve remaining order",
      table.concat(throwing_order, ",") == "b,late,a")

-- [wait] cancels cleanly: the wait loop breaks on cancelled and skips
-- complete (source-level: the guard shape)
local f = assert(io.open("scripts/kag/commands/system.lua", "r"))
local src = f:read("*a")
f:close()
check("wait checks cancelled", src:find("not ct.cancelled", 1, true) ~= nil)
check("wait breaks on cancel", src:find("if ct.cancelled then break end", 1, true) ~= nil)
-- the complete call is guarded by the same cancelled check (both halves)
check("wait complete guard", src:find("if not ct.cancelled then", 1, true) ~= nil
      and src:find("operation:complete()", 1, true) ~= nil)

if failed > 0 then os.exit(1) end
print("OPERATION TESTS DONE")
