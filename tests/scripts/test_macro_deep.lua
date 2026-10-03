-- =====================================================================
--  Caesura (AmeKAG) -- tests/scripts/test_macro_deep.lua
--  Macro system stage-D deepening: recursion guard, arg passthrough,
--  nested calls/definitions, [jump]/[return] interaction, name conflicts,
--  erasemacro rejection. Complements test_macro.lua / test_macro_nested.lua /
--  test_macro_bare.lua / test_alias_bare.lua.
--  Run: external/lua/lua.exe tests/scripts/test_macro_deep.lua
--
--  These cases exercise the runtime macro-splice path (and observe compiler
--  static-inlining output) with hand-built token arrays, independent of the
--  C++ instruction budget. The scheduler uses a mocked kag backend, exactly
--  like the sibling macro tests.
-- =====================================================================
package.path = "scripts/?.lua;scripts/kag/?.lua;scripts/kag/commands/?.lua;" .. package.path

-- Test handlers are explicit DSL extensions; a recording KAG table alone
-- does not register commands. Keep the production public selector intact.
require("kag.commands.text") -- actual ch/text contracts before recorder replacement

local passed, failed = 0, 0
local function check(name, cond, extra)
    if cond then
        passed = passed + 1
        print("PASS " .. name)
    else
        failed = failed + 1
        print("FAIL " .. name .. (extra and (" -- " .. tostring(extra)) or ""))
    end
end

-- kag mock: record every dispatch as {cmd, params}.
local dispatched = {}
local kag_orig = package.loaded["kag"]
package.loaded["kag"] = setmetatable({}, { __index = function(_, k)
    return function(c2, p2) dispatched[#dispatched + 1] = { k, p2 } end
end})

local scheduler = require("scheduler")

-- Run a hand-built token stream to completion inside a coroutine. Returns
-- (ctx, ok, err). Captures resume errors (a self-recursive macro budget
-- error is delivered as resume=false -- a naive loop until dead swallows it).
local function run_tokens(tokens, maxSteps)
    local ctx = { f = {}, tf = {}, sf = {}, mp = {}, variables = {},
        tokens = tokens, token_index = 1, current_scene = "t.ks", label_index = {} }
    local co = coroutine.create(function() scheduler.run(ctx, tokens, 1) end)
    local ok, err = true, nil
    local steps = 0
    while coroutine.status(co) ~= "dead" do
        local rok, rerr = coroutine.resume(co)
        steps = steps + 1
        if maxSteps and steps > maxSteps then
            ok, err = false, "STEP-LIMIT"
            break
        end
        if not rok then ok, err = false, rerr; break end
    end
    return ctx, ok, err
end

local function ch_texts()
    local out = {}
    for _, d in ipairs(dispatched) do
        if d[1] == "ch" then out[#out + 1] = d[2] and d[2].text end
    end
    return out
end

local function clear_dispatched() dispatched = {} end

-- =====================================================================
-- 1. Self-recursive macro guard (runtime path). Redefining the macro twice
-- makes it dynamic (compiler skips inlining), so a bottomless
-- [macro rr][rr][endmacro] must hit the scheduler per-context expansion
-- budget and ERROR -- not dead-loop the coroutine.
-- =====================================================================
do
    local toks = {
        { "macro", { name = "rr" } }, { "rr" }, { "endmacro" },
        { "macro", { name = "rr" } }, { "rr" }, { "endmacro" },
        { "rr" },
    }
    clear_dispatched()
    local ctx, ok, err = run_tokens(toks, 2500)
    local msg = tostring(err or "")
    check("self-recursive macro errors (not dead-loop)", not ok
          and msg:find("expansion depth") ~= nil, msg)
end

-- 2. Mutual recursion ([macro a][b][endmacro][macro b][a][endmacro][a]).
do
    local toks = {
        { "macro", { name = "ma" } }, { "mb" }, { "endmacro" },
        { "macro", { name = "mb" } }, { "ma" }, { "endmacro" },
        { "macro", { name = "ma" } }, { "mb" }, { "endmacro" },
        { "ma" },
    }
    clear_dispatched()
    local ctx, ok, err = run_tokens(toks, 2500)
    local msg = tostring(err or "")
    check("mutual recursion budgeted", not ok
          and msg:find("expansion depth") ~= nil, msg)
end

-- 3. Parameter passthrough (runtime path): args= named fill %who%.
do
    local toks = {
        { "macro", { name = "greet", args = "who" } }, { "ch", { text = "hi %who%" } }, { "endmacro" },
        { "macro", { name = "greet", args = "who" } }, { "ch", { text = "hi %who%" } }, { "endmacro" },
        { "greet", { who = "Sakura" } },
        { "greet", { who = "Kaito" } },
    }
    clear_dispatched()
    local ctx, ok, err = run_tokens(toks, 500)
    local t = ch_texts()
    check("arg passthrough named fills", ok and t[1] == "hi Sakura" and t[2] == "hi Kaito", table.concat(t or {}, "|"))
end

-- 4. Numeric placeholders + mismatched positional call-site (no crash, literal kept).
do
    local toks = {
        { "macro", { name = "pos", args = "1,2" } }, { "ch", { text = "(%1%,%2%)" } }, { "endmacro" },
        { "macro", { name = "pos", args = "1,2" } }, { "ch", { text = "(%1%,%2%)" } }, { "endmacro" },
        { "pos", { who = "x" } },
    }
    clear_dispatched()
    local ctx, ok, err = run_tokens(toks, 500)
    local t = ch_texts()
    check("numeric placeholder mismatch keeps literal", ok and t[1] == "(%1%,%2%)", table.concat(t or {}, "|"))
end

-- 5. Nested macro CALLS (outer body calls inner) -- dynamic path.
do
    local toks = {
        { "macro", { name = "inner", args = "who" } }, { "ch", { name = "%who%", text = "in" } }, { "endmacro" },
        { "macro", { name = "outer", args = "who" } },
            { "inner", { who = "%who%" } }, { "ch", { text = "out" } },
        { "endmacro" },
        { "macro", { name = "outer", args = "who" } },
            { "inner", { who = "%who%" } }, { "ch", { text = "out2" } },
        { "endmacro" },
        { "outer", { who = "Sakura" } },
    }
    clear_dispatched()
    local ctx, ok, err = run_tokens(toks, 500)
    local t = ch_texts()
    check("nested call (dynamic) inner-then-outer", ok and t[1] == "in" and t[2] == "out2", table.concat(t or {}, "|"))
end

-- 6. [jump] inside a macro body (intra-scene, leading *).
do
    local toks = {
        { "macro", { name = "jj" } }, { "ch", { text = "pre" } }, { "jump", { storage = "*dest" } }, { "endmacro" },
        { "macro", { name = "jj" } }, { "ch", { text = "pre2" } }, { "jump", { storage = "*dest" } }, { "endmacro" },
        { "jj" }, { "ch", { text = "unreach" } },
        { "label", { name = "dest" } }, { "ch", { text = "post" } },
    }
    clear_dispatched()
    local ctx, ok, err = run_tokens(toks, 500)
    local t = ch_texts()
    local skippedUnreach = true
    for _, v in ipairs(t or {}) do if v == "unreach" then skippedUnreach = false end end
    check("[jump *label] in macro body works (pre->post)", ok and t[1] == "pre2" and t[#t] == "post", table.concat(t or {}, "|"))
    check("jump body skips post-call token", skippedUnreach, table.concat(t or {}, "|"))
end

-- 7. [return] inside a macro body pops a [call] frame.
do
    local toks = {
        { "macro", { name = "rr" } }, { "ch", { text = "rrbody" } }, { "return" }, { "endmacro" },
        { "macro", { name = "rr" } }, { "ch", { text = "rrbody2" } }, { "return" }, { "endmacro" },
        { "call", { storage = "*sub" } },
        { "ch", { text = "AFTER-CALL" } },
        { "jump", { storage = "*end" } },
        { "label", { name = "sub" } }, { "rr" }, { "ch", { text = "UNREACH" } },
        { "label", { name = "end" } }, { "ch", { text = "finale" } },
    }
    clear_dispatched()
    local ctx, ok, err = run_tokens(toks, 500)
    local t = ch_texts()
    local unreach = false
    for _, v in ipairs(t or {}) do if v == "UNREACH" then unreach = true end end
    check("[return] in macro body returns from [call]", ok and t[1] == "rrbody2" and t[2] == "AFTER-CALL" and t[3] == "finale", table.concat(t or {}, "|"))
    check("no re-entry into subroutine after return", not unreach, table.concat(t or {}, "|"))
end

-- 8. erasemacro makes a later call NOT expand: body never dispatched.
do
    local toks = {
        { "macro", { name = "m" } }, { "ch", { text = "BODY" } }, { "endmacro" },
        { "macro", { name = "m" } }, { "ch", { text = "BODY" } }, { "endmacro" },
        { "erasemacro", { name = "m" } },
        { "m" },
    }
    clear_dispatched()
    local ctx, ok, err = run_tokens(toks, 500)
    local bodyRan = false
    for _, d in ipairs(dispatched) do
        if d[1] == "ch" and d[2] and d[2].text == "BODY" then bodyRan = true end
    end
    check("erasemacro: later call does NOT expand body", ok and not bodyRan, "dispatched=" .. #dispatched)
    check("erasemacro clears macro table entry", ok and ctx.macros and ctx.macros.m == nil, "m=" .. tostring(ctx.macros and ctx.macros.m))
end

-- 9. Macro name vs built-in command conflict (fall-through commands).
do
    local toks = {
        { "macro", { name = "text" } }, { "ch", { text = "custom" } }, { "endmacro" },
        { "macro", { name = "text" } }, { "ch", { text = "custom2" } }, { "endmacro" },
        { "text", { text = "builtin-arg" } },
    }
    clear_dispatched()
    local ctx, ok, err = run_tokens(toks, 500)
    local t = ch_texts()
    local gotBuiltinArg = false
    for _, d in ipairs(dispatched) do
        if d[1] == "text" and d[2] and d[2].text == "builtin-arg" then gotBuiltinArg = true end
    end
    check("macro named text overrides built-in", ok and t[1] == "custom2", table.concat(t or {}, "|"))
    check("no dispatch to built-in text while macro holds the name", not gotBuiltinArg)
end

-- 10. Nested macro DEFINITION (macro defining a macro in its body).
-- PINS CURRENT BEHAVIOR: the body scan stops at the first [endmacro], so a
-- nested [macro inner]... is mis-collected. Assert the process survives and
-- log the observed outcome for the report; this documents that the naive
-- scan emits a malformed body (a defect, reported -- scheduler/compiler are
-- main-agent files, so no fix here).
do
    local toks = {
        { "macro", { name = "outer" } },
            { "macro", { name = "inner" } }, { "ch", { text = "INNER" } }, { "endmacro" },
        { "endmacro" },
        { "outer" },
    }
    clear_dispatched()
    local ctx, ok, err = run_tokens(toks, 100)
    local innerLater = false
    for _, d in ipairs(dispatched) do
        if d[1] == "ch" and d[2] and d[2].text == "INNER" then innerLater = true end
    end
    print("  [report] nested-def: ok=" .. tostring(ok) .. " err=" .. tostring(err)
          .. " dispatched=" .. #dispatched .. " innerRan=" .. tostring(innerLater));
    -- Round 75 fix: depth-aware body collection -- the nested
    -- [macro inner]..[endmacro] pair belongs to the OUTER body, so the
    -- [outer] invocation re-executes the inner DEFINITION and registers
    -- inner for later use (its body does NOT run at definition time).
    check("nested macro definition completes", ok, err)
    local innerBody = ctx.macros and ctx.macros.inner
    check("nested macro definition registers inner via outer body",
          type(innerBody) == "table" and #innerBody == 1
          and innerBody[1][1] == "ch",
          "inner=" .. tostring(innerBody and #innerBody))
    -- A separate [inner] call then dispatches the inner body.
    ctx.macros = ctx.macros
    local toks3 = { { "inner" } }
    local ctx3 = { f = {}, tf = {}, sf = {}, mp = {}, variables = {},
        tokens = toks3, token_index = 1, current_scene = "t.ks", label_index = {} }
    ctx3.macros = ctx.macros
    local co4 = coroutine.create(function() scheduler.run(ctx3, toks3, 1) end)
    local steps4 = 0
    local ok4 = true
    while coroutine.status(co4) ~= "dead" and steps4 < 100 do
        local r4, e4 = coroutine.resume(co4)
        steps4 = steps4 + 1
        if not r4 then ok4 = false break end
    end
    local innerRan2 = false
    for _, d in ipairs(dispatched) do
        if d[1] == "ch" and d[2] and d[2].text == "INNER" then innerRan2 = true end
    end
    check("nested-defined inner invocable afterwards (INNER runs)",
          ok4 and innerRan2, "innerRan=" .. tostring(innerRan2))
end

-- 11. Round 75: expansion guard is DEPTH-based, not a per-ctx counter.
-- 1002 sequential (non-recursive) dynamic macro calls each complete their
-- body before the next splice -- the depth stack stays flat, so the run
-- must complete with all 1002 bodies dispatched (the old per-context
-- counter tripped at 1000 with a misleading recursion report).
do
    local parts = {
        { "macro", { name = "mm" } }, { "ch", { text = "x" } }, { "endmacro" },
        { "macro", { name = "mm" } }, { "ch", { text = "y" } }, { "endmacro" },
    }
    for i = 1, 1002 do parts[#parts + 1] = { "mm" } end
    clear_dispatched()
    local ctx, ok, err = run_tokens(parts, 4000)
    check("1002 sequential legit macro calls complete (depth guard)",
          ok, err)
    check("1002 sequential legit macro calls all dispatched",
          #dispatched == 1002, "dispatched=" .. #dispatched)
end

do
    local ctx,ok,err=run_tokens({
        {"if",{exp="true"}},
        {"macro",{name="temporary"}},{"ch",{text="runtime body"}},{"endmacro"},
        {"endif"},{"temporary"},{"erasemacro",{name="temporary"}},{"end"},
    },100)
    check("erased dynamic macro finishes",ok,err)
    check("macro splice keeps provenance after erasure",ctx.tokens._runtime_rewritten==true)
    check("rewritten stream cannot silently save an original-source cursor",
        not pcall(require('kag.transient_state').assert_saveable,ctx))
end

package.loaded["kag"] = kag_orig

print(string.format("\nMACRO DEEP: %d passed, %d failed", passed, failed))
if failed > 0 then os.exit(1) end
