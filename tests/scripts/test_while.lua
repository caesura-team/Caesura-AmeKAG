-- test_while.lua — bounded [while] (Neo-Genesis)
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
local function run(tokens, variables, pre_iter)
    local d = {}
    local kag_orig = package.loaded["kag"]
    package.loaded["kag"] = setmetatable({}, { __index = function(_, k)
        return function(c2, p2) d[#d + 1] = { k, p2 } end
    end})
    local vars = variables or {}
    local ctx = { f = vars, tf = {}, sf = {}, mp = {}, variables = {},
        _whileIterByScene = { ["t.ks"] = pre_iter or 0 },
        macros = nil, macro_args = nil, current_scene = "t.ks", token_index = 1 }
    local co = coroutine.create(function()
        scheduler.run(ctx, tokens, 1)
    end)
    local ok, err = true
    while coroutine.status(co) ~= "dead" do
        ok, err = coroutine.resume(co)
        if not ok then break end
    end
    package.loaded["kag"] = kag_orig
    return d, ok, err
end

-- bounded true loop: counter variable 0..2 (3 iterations), then done
local tokens = {
    { "while", { exp = "i < 3" } },
    { "ch", { name = "A", text = "loop-body" } },
    { "eval", { exp = "i = i + 1" } },
    { "endwhile" },
    { "ch", { name = "B", text = "after" } },
}
-- eval must be dispatched (KAG binds it) -- capture the counter via the
-- ch handler only; use a plain while over a decrementing var instead:
local tokens2 = {
    { "while", { exp = "n > 0" } },
    { "ch", { name = "A", text = "body" } },
    { "endwhile" },
    { "ch", { name = "B", text = "after" } },
}
-- n starts 0: body skipped
local d = run(tokens2, { n = 0 })
check("false while skips body", #d == 1 and d[1][2].text == "after")

-- n starts 3: body runs, but n never changes -- the bound must trip.
-- Instead of erroring, verify the loop guard fires via a small run.
local d2arr, d2ok, d2err = run(tokens2, { n = 3 }, 65536)
-- pre-seeded near the cap: the guard fires instantly, no long spin
-- the guard errors after WHILE_MAX_ITERS; coroutine.resume returns
-- false + the error message. Check it errors loudly and does NOT hang:
check("runaway loop errors loudly", d2ok == false)
check("error mentions iterations", type(d2err) == "string" and
      d2err:find("iterations", 1, true) ~= nil)

-- bounded via a changing variable (eval increments): 3 iterations
local tokens3 = {
    { "while", { exp = "k < 3" } },
    { "ch", { name = "A", text = "body" } },
    { "eval", { exp = "f.k = f.k + 1" } },
    { "endwhile" },
    { "ch", { name = "B", text = "after" } },
}
-- eval dispatch is implemented as a KAG command; mock it to mutate k
local kag_orig = package.loaded["kag"]
local mut = { k = 0 }
package.loaded["kag"] = setmetatable({}, { __index = function(_, k)
    return function(c2, p2)
        -- [eval] runs INLINE in the scheduler (env.f = ctx.f) -- the mock
        -- must not mutate again; it only records the dispatch.
    end
end})
local ctx = { f = mut, tf = {}, sf = {}, mp = {}, variables = {},
    macros = nil, macro_args = nil, current_scene = "t.ks", token_index = 1 }
local d3 = {}
local co3 = coroutine.create(function()
    scheduler.run(ctx, tokens3, 1)
end)
local ok3 = true
while coroutine.status(co3) ~= "dead" do
    ok3 = coroutine.resume(co3)
end
package.loaded["kag"] = kag_orig
check("bounded loop terminates", ok3)
check("loop ran 3 times", mut.k == 3)

-- nested while: inner loop completes, outer continues
local tokens4 = {
    { "while", { exp = "a < 2" } },
    { "ch", { name = "A", text = "outer" } },
    { "eval", { exp = "f.b = 0" } },  -- reset inner counter each outer round
    { "while", { exp = "b < 2" } },
    { "ch", { name = "B", text = "inner" } },
    { "eval", { exp = "f.b = f.b + 1" } },
    { "endwhile" },
    { "eval", { exp = "f.a = f.a + 1" } },
    { "endwhile" },
    { "ch", { name = "C", text = "final" } },
}
local mut2 = { a = 0, b = 0 }
local counts = { outer = 0, inner = 0 }
local d4 = {}
local kag_orig2 = package.loaded["kag"]
package.loaded["kag"] = setmetatable({}, { __index = function(_, k)
    return function(c2, p2)
        if k == "eval" then
            -- scheduler runs [eval] inline; mock only observes
        elseif k == "ch" then
            d4[#d4 + 1] = { k, p2 }
            if p2.text == "outer" then counts.outer = counts.outer + 1
            elseif p2.text == "inner" then counts.inner = counts.inner + 1 end
        end
    end
end})
local ctx4 = { f = mut2, tf = {}, sf = {}, mp = {}, variables = {},
    macros = nil, macro_args = nil, current_scene = "t.ks", token_index = 1 }
local co4 = coroutine.create(function()
    scheduler.run(ctx4, tokens4, 1)
end)
local ok4 = true
while coroutine.status(co4) ~= "dead" do
    ok4 = coroutine.resume(co4)
end
package.loaded["kag"] = kag_orig2
check("nested loop terminates", ok4)
check("outer ran twice", counts.outer == 2)
check("inner ran four times", counts.inner == 4)
check("final runs", d4[#d4] and d4[#d4][2].text == "final")


-- ---------------------------------------------------------------------------
-- Round 74 additions: range/boundary depth for [while].
-- ---------------------------------------------------------------------------

-- While condition with TJS operators AND ternary, terminating via body [eval].
-- (n>0 ? 1 : 0) == 1 && done != 1 : runs while n>0 and done not set.
local t5 = {
    { "while", { exp = "(n > 0 ? 1 : 0) == 1 && done != 1" } },
    { "eval", { exp = "f.n = f.n - 1" } },
    { "z", { v = "x" } },
    { "endwhile" },
    { "ch", { name = "B", text = "after" } },
}
local vars5 = { n = 4, done = 0 }
local d5 = {}
local kag5 = package.loaded["kag"]
package.loaded["kag"] = setmetatable({}, { __index = function(_, k)
    return function(c2, p2)
        if k == "z" then d5[#d5 + 1] = p2.v
        elseif k == "ch" then d5[#d5 + 1] = "ch" end
    end
end})
local ctx5 = { f = vars5, tf = {}, sf = {}, mp = {}, variables = {},
    _whileIterByScene = { ["t.ks"] = 0 },
    macros = nil, macro_args = nil, current_scene = "t.ks", token_index = 1 }
local co5 = coroutine.create(function() scheduler.run(ctx5, t5, 1) end)
local ok5 = true
while coroutine.status(co5) ~= "dead" do ok5 = coroutine.resume(co5) end
package.loaded["kag"] = kag5
local body5 = 0
for _, v in ipairs(d5) do if v == "x" then body5 = body5 + 1 end end
check("while TJS ternary+&& terminates", ok5)
check("while ternary+&& body count", body5 == 4, tostring(body5))
check("while ternary+&& ran to n=0", vars5.n == 0, tostring(vars5.n))
check("while ternary+&& after runs", d5[#d5] == "ch")

-- Large range: a legitimate 10000-iteration while must complete WITHOUT the
-- 65536 per-scene guard misfiring. n decrements each body run.
local t6 = {
    { "while", { exp = "n > 0" } },
    { "eval", { exp = "f.n = f.n - 1" } },
    { "z", { v = "x" } },
    { "endwhile" },
}
local vars6 = { n = 10000 }
local d6count = 0
local kag6 = package.loaded["kag"]
package.loaded["kag"] = setmetatable({}, { __index = function(_, k)
    return function(c2, p2) if k == "z" then d6count = d6count + 1 end end
end})
local ctx6 = { f = vars6, tf = {}, sf = {}, mp = {}, variables = {},
    _whileIterByScene = { ["t.ks"] = 0 },
    macros = nil, macro_args = nil, current_scene = "t.ks", token_index = 1 }
local co6 = coroutine.create(function() scheduler.run(ctx6, t6, 1) end)
local ok6 = true
while coroutine.status(co6) ~= "dead" do ok6 = coroutine.resume(co6) end
package.loaded["kag"] = kag6
check("while 10000 iterations completes", ok6)
check("while 10000 iterations count", d6count == 10000, tostring(d6count))
check("while 10000 iterations decrements to 0", vars6.n == 0, tostring(vars6.n))
if failed > 0 then os.exit(1) end
print("WHILE TESTS DONE")