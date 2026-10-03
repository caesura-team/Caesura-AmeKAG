-- test_transitions.lua — [trans]/[blur]/[move] contracts (audit)
package.path = "scripts/?.lua;scripts/kag/?.lua;" .. package.path
local passed, failed = 0, 0
local function check(name, cond)
    if cond then print("PASS " .. name) passed = passed + 1
    else print("FAIL " .. name) failed = failed + 1 end
end

local KAG = require("kag")
local schema = require("kag.schema")

-- trans schema: method default crossfade, time/duration clamps
local t = schema.coerce("trans", { time = "99999" }, {})
check("trans time clamped", t.time == 30000)
check("trans omitted aliases remain absent until runtime default", t.method == nil and t.type == nil and t.kind == nil)
local t2 = schema.coerce("trans", { method = "rule", duration = "1" }, {})
check("trans method kept", t2.method == "rule")
check("trans duration kept", t2.duration == 1)
local t3 = schema.coerce("trans", { duration = "99999" }, {})
check("trans duration clamped", t3.duration == 30000)
local m2 = schema.coerce("move", { duration = "99999" }, {})
check("move duration clamped", m2.duration == 30000)

-- move schema: x/y passthrough, time/duration clamps
local m = schema.coerce("move", { x = "-50", y = "120", time = "0" }, {})
check("move x kept", m.x == -50)
check("move y kept", m.y == 120)
check("move time kept", m.time == 0)

-- handlers registered
check("trans registered", type(KAG.trans) == "function")
check("blur registered", type(KAG.blur) == "function")
check("move registered", type(KAG.move) == "function")

-- source-level: trans cancels via Operation <close> (Transition.cancel
-- registered on the token); move uses the same pattern
local f = assert(io.open("scripts/kag/commands/transition.lua", "r"))
local src = f:read("*a")
f:close()
check("trans cancel registered for its owner", src:find("ct:register(function() Transition.cancel(tx) end)", 1, true) ~= nil)
check("trans operation pattern", src:find("operation <close> = Operation.start(ctx)", 1, true) ~= nil)
check("blur delegates VFX", src:find('VFX.blur(ctx, params)', 1, true) ~= nil)

if failed > 0 then os.exit(1) end
print("TRANSITIONS TESTS DONE")
