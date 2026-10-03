-- test_for.lua — numeric [for] loops (Neo-Genesis)
package.path = "scripts/?.lua;scripts/kag/?.lua;" .. package.path

-- Test handlers are explicit DSL extensions; a recording KAG table alone
-- does not register commands. Keep the production public selector intact.
require("kag.commands.text") -- actual ch/text contracts before recorder replacement
local fixture_schema = require("kag.schema")
fixture_schema.define("z", {v={type="string"},tag={type="string"}})
local passed, failed = 0, 0
local function check(name, cond)
    if cond then print("PASS " .. name) passed = passed + 1
    else print("FAIL " .. name) failed = failed + 1 end
end

local scheduler = require("scheduler")
local function runFor(tokens, f0, pre_iter)
    local d = {}
    local kag_orig = package.loaded["kag"]
    package.loaded["kag"] = setmetatable({}, { __index = function(_, k)
        return function(c2, p2) d[#d + 1] = { k, p2 } end
    end})
    local vars = f0 or {}
    local ctx = { f = vars, tf = {}, sf = {}, mp = {}, variables = {},
        _whileIterByScene = { ["t.ks"] = pre_iter or 0 },
        macros = nil, macro_args = nil, current_scene = "t.ks", token_index = 1 }
    local co = coroutine.create(function() scheduler.run(ctx, tokens, 1) end)
    local ok, err = true
    while coroutine.status(co) ~= "dead" do
        ok, err = coroutine.resume(co)
        if not ok then break end
    end
    package.loaded["kag"] = kag_orig
    return d, ok, err, ctx
end

-- basic: i 0..2 (3 iterations), counter visible in f, after runs
local t1 = {
    { "for", { var = "i", start = "0", ["end"] = "2", step = "1" } },
    { "ch", { name = "A", text = "body" } },
    { "endfor" },
    { "ch", { name = "B", text = "after" } },
}
local d1, ok1, _, ctx1 = runFor(t1)
check("for runs 3 times",
      #d1 == 4 and d1[1][2].text == "body" and d1[4][2].text == "after")
check("counter ends past end", ctx1.f.i == 3)
check("after runs", d1[#d1] and d1[#d1][2].text == "after")

-- descending: i 3..1 step -1 (3 iterations)
local t2 = {
    { "for", { var = "j", start = "3", ["end"] = "1", step = "-1" } },
    { "ch", { name = "A", text = "body" } },
    { "endfor" },
}
local d2, ok2, _, ctx2 = runFor(t2)
check("descending runs 3 times", #d2 == 3)
check("descending counter", ctx2.f.j == 0)

-- zero iterations: start past end (step +1)
local t3 = {
    { "for", { var = "k", start = "5", ["end"] = "2", step = "1" } },
    { "ch", { name = "A", text = "body" } },
    { "endfor" },
    { "ch", { name = "B", text = "after" } },
}
local d3, ok3 = runFor(t3)
check("empty for skips body", #d3 == 1 and d3[1][2].text == "after")

-- guard: step=0 degenerates to 1 (no hang); pre-seed near cap fires
local t4 = {
    { "for", { var = "z", start = "0", ["end"] = "999", step = "0" } },
    { "ch", { name = "A", text = "body" } },
    { "endfor" },
}
local d4, ok4, err4 = runFor(t4, {}, 65536)
check("for guard errors loudly", ok4 == false)
check("guard message mentions iterations",
      type(err4) == "string" and err4:find("iterations", 1, true) ~= nil)

-- nested for: 2x2 with independent counters
local t5 = {
    { "for", { var = "a", start = "0", ["end"] = "1", step = "1" } },
    { "for", { var = "b", start = "0", ["end"] = "1", step = "1" } },
    { "ch", { name = "A", text = "body" } },
    { "endfor" },
    { "endfor" },
    { "ch", { name = "B", text = "after" } },
}
local d5, ok5, _, ctx5 = runFor(t5)
check("nested for runs 4 times", #d5 == 5)  -- 4 bodies + after
check("nested counters independent", ctx5.f.a == 2 and ctx5.f.b == 2)
check("nested after runs", d5[#d5] and d5[#d5][2].text == "after")


-- Sequential same-name for after an EMPTY for: the mark must be cleared
-- (review should-fix) or the second loop reuses the stale counter.
do
    local scheduler = require("scheduler")
    local tokens = {
        { "for", { var = "i", start = "9", ["end"] = "1", step = "1" } },  -- empty
        { "ch", { name = "A", text = "skip" } },
        { "endfor" },
        { "for", { var = "i", start = "0", ["end"] = "1", step = "1" } },  -- 2 iters
        { "ch", { name = "B", text = "body" } },
        { "endfor" },
    }
    local d = {}
    local kag_orig = package.loaded["kag"]
    package.loaded["kag"] = setmetatable({}, { __index = function(_, k)
        return function(c2, p2) d[#d + 1] = { k, p2 } end
    end})
    local vars = {}
    local ctx = { f = vars, tf = {}, sf = {}, mp = {}, variables = {},
        _whileIterByScene = { ["t.ks"] = 0 },
        macros = nil, macro_args = nil, current_scene = "t.ks", token_index = 1 }
    local co = coroutine.create(function() scheduler.run(ctx, tokens, 1) end)
    local ok = true
    while coroutine.status(co) ~= "dead" do ok = coroutine.resume(co) end
    package.loaded["kag"] = kag_orig

    local bodyc = 0
    for _, x in ipairs(d) do if x[2].text == "body" then bodyc = bodyc + 1 end end
    local skipc = 0
    for _, x in ipairs(d) do if x[2].text == "skip" then skipc = skipc + 1 end end
    local okall = ok and bodyc == 2 and skipc == 0 and vars.i == 2
    if okall then print("PASS empty-for mark cleared; second for runs 2x")
        passed = passed + 1
    else print("FAIL empty-for mark cleared; second for runs 2x")
        failed = failed + 1 end
end

-- ---------------------------------------------------------------------------
-- Round 74 additions: range/boundary depth for numeric [for].
-- ---------------------------------------------------------------------------

-- Variable bounds: start=f.s / end=f.e read from the game frame each entry.
local t6 = {
    { "for", { var = "i", start = "f.s", ["end"] = "f.e", step = "1" } },
    { "eval", { exp = "f.sum = f.sum + f.i" } },
    { "z", { v = "x" } },
    { "endfor" },
    { "z", { v = "after" } },
}
local d6, ok6, _, ctx6 = runFor(t6, { s = 2, e = 5, sum = 0 })
local b6 = 0
for _, x in ipairs(d6) do if x[1] == "z" and x[2].v == "x" then b6 = b6 + 1 end end
check("for variable bounds run 4 times", ok6 and b6 == 4)
check("for variable bounds sum", ctx6.f.sum == 14, tostring(ctx6.f.sum))  -- 2+3+4+5
check("for variable bounds counter past end", ctx6.f.i == 6)

-- Variable descending bounds with a negative step EXPRESSION (f.pace).
local t7 = {
    { "for", { var = "i", start = "f.hi", ["end"] = "f.lo", step = "f.pace" } },
    { "eval", { exp = "f.sum = f.sum + f.i" } },
    { "z", { v = "x" } },
    { "endfor" },
}
local d7, ok7, _, ctx7 = runFor(t7, { hi = 8, lo = 2, pace = -2, sum = 0 })
local b7 = 0
for _, x in ipairs(d7) do if x[1] == "z" and x[2].v == "x" then b7 = b7 + 1 end end
check("for descending expr-step runs 4", ok7 and b7 == 4)
check("for descending expr-step sum 8+6+4+2", ctx7.f.sum == 20, tostring(ctx7.f.sum))
check("for descending expr-step bounds", ctx7.f.i == 0, tostring(ctx7.f.i))

-- Large range: 0..9999 (10000 iterations) must run WITHOUT the 65536
-- per-scene guard misfiring. Accumulate the sum via [eval].
local t8 = {
    { "for", { var = "i", start = "0", ["end"] = "9999", step = "1" } },
    { "eval", { exp = "f.sum = f.sum + f.i" } },
    { "z", { v = "x" } },
    { "endfor" },
}
local d8, ok8, _, ctx8 = runFor(t8, { sum = 0 })
local b8 = 0
for _, x in ipairs(d8) do if x[1] == "z" and x[2].v == "x" then b8 = b8 + 1 end end
check("for 10000 iterations count", ok8 and b8 == 10000, tostring(b8))
check("for 10000 iterations sum 49995000", ctx8.f.sum == 49995000, tostring(ctx8.f.sum))
check("for 10000 iterations counter", ctx8.f.i == 10000, tostring(ctx8.f.i))

-- Guard boundary: exactly 65536 iterations is permitted (the guard trips
-- only ABOVE WHILE_MAX_ITERS) -- the loop must complete, not error.
local t9 = {
    { "for", { var = "i", start = "0", ["end"] = "65535", step = "1" } },
    { "z", { v = "x" } },
    { "endfor" },
}
local d9, ok9, err9, ctx9 = runFor(t9, {})
local b9 = 0
for _, x in ipairs(d9) do if x[1] == "z" and x[2].v == "x" then b9 = b9 + 1 end end
check("for at-65536 boundary completes", ok9 and b9 == 65536, tostring(b9) .. " ok=" .. tostring(ok9))
check("for at-65536 boundary counter", ctx9.f.i == 65536, tostring(ctx9.f.i))

-- Mutable END bound (documented anti-pattern): end=f.e is re-evaluated at
-- every loop head, so shrinking it in the body shortens the span. Guard the
-- run against a hang and report the observed length.
local t10 = {
    { "for", { var = "i", start = "0", ["end"] = "f.e", step = "1" } },
    { "eval", { exp = "f.e = f.e - 1" } },
    { "z", { v = "x" } },
    { "endfor" },
}
local d10, ok10, err10, ctx10 = runFor(t10, { e = 5 })
local b10 = 0
for _, x in ipairs(d10) do if x[1] == "z" and x[2].v == "x" then b10 = b10 + 1 end end
check("for mutable end completes (no hang)", ok10)
check("for mutable end observed span", b10 == 4, tostring(b10))

-- Interpolation is NOT an expression-language construct: a bound of ${f.s} fails
-- to compile and the body is skipped loudly (graceful, no crash).
local t11 = {
    { "for", { var = "i", start = "${f.s}", ["end"] = "3", step = "1" } },
    { "z", { v = "x" } },
    { "endfor" },
    { "z", { v = "after" } },
}
local d11, ok11, _, _ = runFor(t11, { s = 1 })
local b11 = 0
local after11 = false
for _, x in ipairs(d11) do
    if x[1] == "z" then
        if x[2].v == "x" then b11 = b11 + 1 elseif x[2].v == "after" then after11 = true end
    end
end
check("for interpolation bound skips body (no crash)", ok11 and b11 == 0)
check("for interpolation bound continues after", after11)
if failed > 0 then os.exit(1) end
print("FOR TESTS DONE")