-- test_loop_control.lua — [break]/[continue] (Neo-Genesis)
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
local function run(tokens, f0)
    local d = {}
    local kag_orig = package.loaded["kag"]
    package.loaded["kag"] = setmetatable({}, { __index = function(_, k)
        return function(c2, p2) d[#d + 1] = { k, p2 } end
    end})
    local vars = f0 or {}
    local ctx = { f = vars, tf = {}, sf = {}, mp = {}, variables = {},
        _whileIterByScene = { ["t.ks"] = 0 },
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

-- break: while loop with an explicit early exit
local t1 = {
    { "for", { var = "i", start = "0", ["end"] = "9", step = "1" } },
    { "if", { exp = "i == 2" } },
    { "break" },
    { "endif" },
    { "ch", { name = "A", text = "body" } },
    { "endfor" },
    { "ch", { name = "B", text = "after" } },
}
-- i==2 -> break: bodies for i=0,1 run (2), then after
local d1, ok1, _, ctx1 = run(t1)
check("break exits early", #d1 == 3 and d1[1][2].text == "body"
      and d1[3][2].text == "after")
check("break counter stops at 2", ctx1.f.i == 2)

-- continue: skip bodies for even i (i=0,2 skipped; 1,3 run)
local t2 = {
    { "for", { var = "j", start = "0", ["end"] = "3", step = "1" } },
    { "if", { exp = "j % 2 == 0" } },
    { "continue" },
    { "endif" },
    { "ch", { name = "A", text = "body" } },
    { "endfor" },
}
local d2, ok2, _, ctx2 = run(t2)
check("continue skips evens", #d2 == 2 and d2[1][2].text == "body")
check("continue counter completes", ctx2.f.j == 4)

-- continue inside while: n decrements via eval; continue skips the ch
-- eval BEFORE the check so a skipped iteration still decrements
-- (continue jumping past the decrement would spin forever -- the
-- author's bug, correctly surfaced by the loop guard).
local t3 = {
    { "while", { exp = "n > 0" } },
    { "eval", { exp = "f.n = f.n - 1" } },
    { "if", { exp = "n == 1" } },
    { "continue" },
    { "endif" },
    { "ch", { name = "A", text = "body" } },
    { "endwhile" },
}
-- n: 3->2 body(2), 2->1 continue, 1->0 body(0): 2 bodies, n=0
local d3, ok3, _, ctx3 = run(t3, { n = 3 })
check("while continue works", #d3 == 2)
check("while continue terminates", ctx3.f.n == 0)

-- break outside a loop errors loudly
local t4 = {
    { "break" },
}
local d4, ok4, err4 = run(t4)
check("break outside errors", ok4 == false)
check("break error message", type(err4) == "string"
      and err4:find("outside a loop", 1, true) ~= nil)


-- Mixed nesting (review nit): break inside a for nested in a while must
-- exit the FOR only; the while continues.
do
    local scheduler = require("scheduler")
    local tokens = {
        { "while", { exp = "n > 0" } },
        { "eval", { exp = "f.n = f.n - 1" } },
        { "for", { var = "i", start = "0", ["end"] = "9", step = "1" } },
        { "if", { exp = "i == 1" } },
        { "break" },  -- breaks the FOR, not the while
        { "endif" },
        { "ch", { name = "A", text = "inner" } },
        { "endfor" },
        { "ch", { name = "B", text = "outer" } },
        { "endwhile" },
        { "ch", { name = "C", text = "final" } },
    }
    local d = {}
    local kag_orig = package.loaded["kag"]
    package.loaded["kag"] = setmetatable({}, { __index = function(_, k)
        return function(c2, p2) d[#d + 1] = { k, p2 } end
    end})
    local vars = { n = 2 }
    local ctx = { f = vars, tf = {}, sf = {}, mp = {}, variables = {},
        _whileIterByScene = { ["t.ks"] = 0 },
        macros = nil, macro_args = nil, current_scene = "t.ks", token_index = 1 }
    local co = coroutine.create(function() scheduler.run(ctx, tokens, 1) end)
    local ok = true
    while coroutine.status(co) ~= "dead" do ok = coroutine.resume(co) end
    package.loaded["kag"] = kag_orig

    local inner = 0
    local outer = 0
    for _, x in ipairs(d) do
        if x[2].text == "inner" then inner = inner + 1
        elseif x[2].text == "outer" then outer = outer + 1 end
    end
    -- n=2,1: each while round runs i=0 only (break at i==1) -> 2 inner,
    -- 2 outer; final runs
    local okall = ok and inner == 2 and outer == 2
        and d[#d] and d[#d][2].text == "final"
    if okall then print("PASS for-in-while break exits for only")
        passed = passed + 1
    else print("FAIL for-in-while break exits for only")
        failed = failed + 1 end
end



-- ---------------------------------------------------------------------------
-- Round 74 additions: [break]/[continue] boundary depth.
-- ---------------------------------------------------------------------------

-- continue inside a for nested in a while must target the FOR (innermost),
-- not the while: i==1 continues the for (so 1 body per for run), but the
-- while's n decrement runs every round.
do
    local scheduler = require("scheduler")
    local tokens = {
        { "while", { exp = "n > 0" } },
        { "eval", { exp = "f.n = f.n - 1" } },
        { "for", { var = "i", start = "0", ["end"] = "2", step = "1" } },
        { "if", { exp = "i == 1" } },
        { "continue" },
        { "endif" },
        { "z", { tag = "IN" } },
        { "endfor" },
        { "z", { tag = "OUT" } },
        { "endwhile" },
    }
    local d = {}
    local kag_orig = package.loaded["kag"]
    package.loaded["kag"] = setmetatable({}, { __index = function(_, k)
        return function(c2, p2) d[#d + 1] = { k, p2 } end
    end})
    local vars = { n = 2 }
    local ctx = { f = vars, tf = {}, sf = {}, mp = {}, variables = {},
        _whileIterByScene = { ["t.ks"] = 0 },
        macros = nil, macro_args = nil, current_scene = "t.ks", token_index = 1 }
    local co = coroutine.create(function() scheduler.run(ctx, tokens, 1) end)
    local ok = true
    while coroutine.status(co) ~= "dead" do ok = coroutine.resume(co) end
    package.loaded["kag"] = kag_orig
    local inC = 0
    local outC = 0
    for _, x in ipairs(d) do
        if x[1] == "z" then
            if x[2].tag == "IN" then inC = inC + 1 else outC = outC + 1 end
        end
    end
    -- n=2,1: for runs i=0 (body), i=1 (continue), i=2 (body) -> 2 IN per
    -- while round => 4 IN total, 2 OUT.
    local okall = ok and inC == 4 and outC == 2
    if okall then print("PASS continue-in-nested targets innermost for")
        passed = passed + 1
    else print("FAIL continue-in-nested targets innermost for (IN=" .. inC
        .. " OUT=" .. outC .. ")") failed = failed + 1 end
end

-- break AND continue in the same for: i=0 body, i=1 continue, i=2 break.
local t5 = {
    { "for", { var = "i", start = "0", ["end"] = "5", step = "1" } },
    { "if", { exp = "i == 2" } }, { "break" }, { "endif" },
    { "if", { exp = "i == 1" } }, { "continue" }, { "endif" },
    { "z", { tag = "B" } },
    { "endfor" },
    { "z", { tag = "AFTER" } },
}
local d5, ok5, _, ctx5 = run(t5, {})
local b5 = 0
local after5 = false
for _, x in ipairs(d5) do
    if x[1] == "z" then
        if x[2].tag == "B" then b5 = b5 + 1 elseif x[2].tag == "AFTER" then after5 = true end
    end
end
check("break+continue same loop runs once", ok5 and b5 == 1, tostring(b5))
check("break+continue stops at i==2", ctx5.f.i == 2, tostring(ctx5.f.i))
check("break+continue after runs", after5)

-- break inside an EMPTY-body first iteration still exits cleanly.
local t6 = {
    { "for", { var = "k", start = "0", ["end"] = "9", step = "1" } },
    { "if", { exp = "k == 0" } }, { "break" }, { "endif" },
    { "endfor" },
    { "z", { tag = "AFTER" } },
}
local d6, ok6, _, ctx6 = run(t6, {})
local after6 = false
for _, x in ipairs(d6) do if x[1] == "z" and x[2].tag == "AFTER" then after6 = true end end
check("break in empty first iteration", ok6 and after6)
check("break at k==0 counter", ctx6.f.k == 0, tostring(ctx6.f.k))
if failed > 0 then os.exit(1) end
print("LOOP CONTROL TESTS DONE")