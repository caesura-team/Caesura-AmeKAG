-- Isolated host-boundary fixture: real backend_factory and strict sandbox.
-- The native consumer regression supplies the actual C++ texture query; this
-- test checks that the strict proxy forwards its read-only boolean contract
-- without exposing the adjacent mutating or privileged APIs.
package.path = "scripts/?.lua;scripts/?/init.lua;" .. package.path
local passed, failed = 0, 0
local function check(name, condition, detail)
    print((condition and "PASS " or "FAIL ") .. name .. (detail and (": " .. tostring(detail)) or ""))
    if condition then passed = passed + 1 else failed = failed + 1 end
end

local live = {[17] = true}
local queries, privileged_calls = {}, 0
Render = {
    is_valid_handle = function(kind, id)
        queries[#queries + 1] = {kind, id}
        return kind == 0 and live[id] == true
    end,
    invalidate_handles = function() privileged_calls = privileged_calls + 1 end,
}
DevCore = {quit = function() privileged_calls = privileged_calls + 1 end}
KAG = {}
_CAESURA_CONFIG = {dev_mode = false}
local backend = require("backend_factory").create()
local Sandbox = require("sandbox")
check("strict mode is active", Sandbox.is_strict())

local ok, value = pcall(backend.render, "is_valid_handle", 0, 17)
check("strict factory forwards a valid texture query", ok and value == true, value)
ok, value = pcall(backend.render, "is_valid_handle", 0, 0)
check("strict factory preserves false for an invalid texture", ok and value == false, value)
live[17] = nil -- Host retirement; no mutation of the sandbox module table.
ok, value = pcall(backend.render, "is_valid_handle", 0, 17)
check("strict factory observes a retired texture", ok and value == false, value)
check("query calls preserve handle type and identity", #queries == 3
    and queries[1][1] == 0 and queries[1][2] == 17
    and queries[2][1] == 0 and queries[2][2] == 0
    and queries[3][1] == 0 and queries[3][2] == 17)

ok, value = pcall(function() Render.invalidate_handles(0) end)
check("generation invalidation remains blocked", not ok
    and tostring(value):find("Render.invalidate_handles is blocked in strict mode", 1, true), value)
ok, value = pcall(function() DevCore.quit() end)
check("privileged quit remains blocked", not ok
    and tostring(value):find("DevCore.quit is blocked in strict mode", 1, true), value)
ok, value = pcall(function() Render.is_valid_handle = function() return true end end)
check("Render module writes remain blocked", not ok
    and tostring(value):find("cannot modify Render table", 1, true), value)
check("blocked APIs never reached their host functions", privileged_calls == 0)
check("strict mode remains active", Sandbox.is_strict())

print(string.format("Results: %d passed, %d failed", passed, failed))
assert(failed == 0, "Strict render-query regression failed")
