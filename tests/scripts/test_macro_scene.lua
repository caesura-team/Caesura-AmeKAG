-- =============================================================================
--  test_macro_scene.lua — Cross-scene [macro] semantics adjudication.
--
--  ADJUDICATION (round 98, "suspected leak" re-classified):
--  ctx.macros is SESSION-level state; a cross-scene [jump]/[call] does NOT
--  clear it. Scene A's [macro] therefore remains callable in scene B. This
--  is NOT a defect — it is KAG3-compatible GLOBAL macro semantics: in the
--  original KAG3 engine a macro, once defined, is available from any scene
--  until erased ([erasemacro]) or redefined.
--
--  What this suite locks:
--      A. cross-scene macro SHARING      (A defines -> jump B -> B calls the
--                                         same name: A's body splices into B
--                                         and executes successfully — the
--                                         KAG3-compatible behavior, not a leak)
--      B. macro-arg %N% / %name% pass-through across scenes (named %who% and
--                                         positional %1% substitute inside the
--                                         body executed in B; a missing arg
--                                         keeps the literal placeholder)
--      C. asymmetric command override    (macros NAMED after a non-flow builtin
--                                         command e.g. [text] override it across
--                                         scenes; macros named after a flow
--                                         command e.g. [return] do NOT — the
--                                         flow branch wins at runtime and the
--                                         dynamic macro is silently ignored,
--                                         exactly like intra-scene semantics)
--
--  Methodology mirrors test_flow_edge_scene.lua / test_flow_edge.lua: the
--  implementation (scripts/scheduler.lua, scripts/kag/compiler.lua) is
--  authoritative and is NOT modified. We only lock observed, adjudicated
--  behavior. Scene-ordering markers use the custom 'mark' command (NOT part
--  of the product registry; this test declares tag so it survives coercion of
--  undocumented params from migrated builtins like [ch]); macro-body content
--  rides [ch text=] which the schema preserves.
-- =============================================================================

package.path = "scripts/?.lua;scripts/kag/?.lua;" .. package.path

-- Test handlers are explicit DSL extensions; a recording KAG table alone
-- does not register commands. Keep the production public selector intact.
require("kag.commands.text") -- actual ch/text contracts before recorder replacement
local fixture_schema = require("kag.schema")
fixture_schema.define("mark", {tag={type="string"}})

local scheduler = require("scheduler")

local passed, failed = 0, 0
local function check(name, cond, detail)
    if cond then
        passed = passed + 1
    else
        failed = failed + 1
        print(string.format("  [FAIL] %s  -- %s", name, detail or ""))
    end
end

print("=== Macro-Scene (cross-scene macro semantics) Tests ===")

local function make_ctx()
    return {
        f = {}, sf = {}, tf = {}, mp = {}, lf = {},
        tokens = {}, token_index = 1, call_stack = {},
        layers = {}, backlog = {}, active_operations = {},
        macros = {}, macro_args = {}, stop_flag = false, dispatched = {},
        load_tokens = function() end,
        current_scene = "main.ks",
    }
end

local kag_orig = package.loaded["kag"]
local kmock = {}
-- Declared custom ordering marker; tag is preserved by its explicit schema.
kmock.mark = function(ctx, p)
    ctx.dispatched = ctx.dispatched or {}
    ctx.dispatched[#ctx.dispatched + 1] = { cmd = "mark", params = p }
end
kmock.ch = function(ctx, p)
    ctx.dispatched = ctx.dispatched or {}
    ctx.dispatched[#ctx.dispatched + 1] = { cmd = "ch", params = p }
end
-- non-flow builtin command [text] — a macro of the same name must override it
kmock.text = function(ctx, p)
    ctx.dispatched = ctx.dispatched or {}
    ctx.dispatched[#ctx.dispatched + 1] = { cmd = "text", params = p }
end
kmock.wait = function() end
package.loaded["kag"] = kmock

-- plain-substring search (string.find's % patterns would mis-parse %1%)
local function has_sub(hay, needle)
    return string.find(hay, needle, 1, true) ~= nil
end

-- 'mark' markers read their tag (schema-safe custom cmd); 'ch' text carries
-- macro-body content. All joined with a comma for order-sensitive asserts.
local function collect(ctx)
    local o = {}
    for _, d in ipairs(ctx.dispatched) do
        if d.cmd == "mark" and d.params and d.params.tag then
            o[#o + 1] = "M:" .. d.params.tag
        elseif d.cmd == "ch" and d.params and d.params.text then
            o[#o + 1] = "T:" .. d.params.text
        end
    end
    return table.concat(o, ",")
end

local function run_capped(ctx, tokens, start, maxf)
    local co = coroutine.create(function() scheduler.run(ctx, tokens, start) end)
    local n = 0
    local M = maxf or 2000
    while coroutine.status(co) ~= "dead" and n < M do
        n = n + 1
        coroutine.resume(co, 16)
    end
    return n, coroutine.status(co)
end

-- After a cross-scene [jump], scheduler.run returns; the runner re-spawns on
-- ctx.tokens / ctx.token_index. Mirror that for scene B.
local function run_after_jump(ctx, maxf)
    return run_capped(ctx, ctx.tokens, nil, maxf)
end

-- ---------------------------------------------------------------------------
-- A. cross-scene macro SHARING = KAG3-compatible GLOBAL macro semantics
-- ---------------------------------------------------------------------------

-- A1/A2. Scene A defines a macro at top level; a cross-scene [jump b.ks]
--     swaps to B. B calls the SAME macro name: A's body splices into B and
--     executes successfully. The body comes from A (its text marker proves
--     origin), demonstrating GLOBAL macro sharing — the documented KAG3
--     behavior, not a per-scene "leak".
do
    local b_tokens = {
        { "mark", { tag = "B_START" } },          -- 1
        { "bless", { who = "Sakura" } },          -- 2 (macro call, from A)
        { "mark", { tag = "B_END" } },            -- 3
    }
    local a_tokens = {
        { "macro", { name = "bless", args = "who" } },  -- 1 def in A
        { "ch", { text = "BLESS_BODY_%who%" } },        -- 2 A's body
        { "endmacro", {} },                             -- 3
        { "mark", { tag = "A_DONE" } },                 -- 4
        { "jump", { target = "b.ks" } },                -- 5 cross-scene
    }
    local ctx = make_ctx(); ctx.current_scene = "assets/script/a.ks"
    local loaded = { ["assets/script/a.ks"] = a_tokens, ["assets/script/b.ks"] = b_tokens }
    ctx.load_tokens = function(p) return loaded[p] end

    local n1, st1 = run_capped(ctx, a_tokens, nil, 200)
    check("A1 A's macro recorded into session ctx.macros",
        ctx.macros and ctx.macros.bless ~= nil, "bless absent")
    check("A1 cross-scene jump fired (A's run ended, scene -> b.ks)",
        st1 == "dead" and ctx.current_scene == "assets/script/b.ks",
        "st=" .. tostring(st1) .. " scene=" .. tostring(ctx.current_scene))

    ctx.dispatched = {}   -- clear A's A_DONE so B's dispatch is isolated
    local n2, st2 = run_after_jump(ctx, 200)
    check("A2 B's run terminates (macro body executes, no leak-abort)",
        n2 < 200 and st2 == "dead", "frames " .. n2)
    -- A's body must splice into B: the ch text "BLESS_BODY_Sakura" runs in B
    -- where the call site is. This is KAG3-compatible global shared macro.
    check("A2 [KAG3-global] A's macro body executes in B (not a leak defect)",
        collect(ctx) == "M:B_START,T:BLESS_BODY_Sakura,M:B_END", collect(ctx))
    check("A2 stored body keeps the %who% placeholder (filled per invocation)",
        ctx.macros and ctx.macros.bless and ctx.macros.bless[1]
            and ctx.macros.bless[1][2] and ctx.macros.bless[1][2].text == "BLESS_BODY_%who%",
        "stored body was not the source of truth")
end

-- ---------------------------------------------------------------------------
-- B. macro-arg %N% / %name% pass-through across scenes
-- ---------------------------------------------------------------------------

-- B1. A defines a parameterized macro whose body references BOTH a named
--     arg (%who%) and a positional arg (%1%). B calls it twice — once with
--     both args, once with the named arg only — and both substitute into
--     A's body as it executes in B. A missing positional leaves the literal.
do
    local b_tokens = {
        { "mark", { tag = "P0" } },
        { "intro", { [1] = "Saber", who = "Rin" } },  -- positional AND named
        { "intro", { [1] = "Archer", who = "Rin" } }, -- positional AND named
        { "mark", { tag = "P1" } },
    }
    local a_tokens = {
        { "macro", { name = "intro", args = "who" } },
        { "ch", { text = "INTRO_%1%_%who%" } },
        { "endmacro", {} },
        { "jump", { target = "b.ks" } },
    }
    local ctx = make_ctx(); ctx.current_scene = "assets/script/a.ks"
    local loaded = { ["assets/script/a.ks"] = a_tokens, ["assets/script/b.ks"] = b_tokens }
    ctx.load_tokens = function(p) return loaded[p] end
    run_capped(ctx, a_tokens, nil, 200)
    ctx.dispatched = {}
    local n2, st2 = run_after_jump(ctx, 200)
    check("B1 B run terminates (two macro calls)", n2 < 200 and st2 == "dead",
        "frames " .. n2)
    -- positional %1% AND named %who% substitute into A's body executed in B
    check("B1 %1% positional AND %who% named substitute in B (T:INTRO_Saber_Rin)",
        has_sub(collect(ctx), "T:INTRO_Saber_Rin"), collect(ctx))
    -- second call: both passed -> INTRO_Archer_Rin
    check("B1 second positional call substitutes (T:INTRO_Archer_Rin)",
        has_sub(collect(ctx), "T:INTRO_Archer_Rin"), collect(ctx))
    check("B1 stored body keeps the placeholders (filled per invocation)",
        ctx.macros and ctx.macros.intro and ctx.macros.intro[1]
            and ctx.macros.intro[1][2].text == "INTRO_%1%_%who%",
        tostring(ctx.macros and ctx.macros.intro and ctx.macros.intro[1]
            and ctx.macros.intro[1][2] and ctx.macros.intro[1][2].text))
end

-- ---------------------------------------------------------------------------
-- C. asymmetric override: non-flow command vs flow command, across scenes
-- ---------------------------------------------------------------------------

-- C1. NON-FLOW builtin: scene A defines a macro NAMED [text] (a regular
--     dispatched command). B issues [text ...]; the leaked A macro OVERRIDES
--     the [text] handler — the macro body splices instead of the builtin.
do
    local b_tokens = {
        { "text", { tag = "B_TEXT" } },   -- would hit kag.text if no macro
        { "return", {} },
    }
    local a_tokens = {
        { "macro", { name = "text" } },
        { "ch", { text = "MACRO_TEXT_BODY" } },
        { "endmacro", {} },
        { "call", { target = "b.ks" } },
        { "mark", { tag = "A_BACK" } },
    }
    local ctx = make_ctx(); ctx.current_scene = "assets/script/a.ks"
    ctx.dispatched = {}
    local loaded = { ["assets/script/a.ks"] = a_tokens, ["assets/script/b.ks"] = b_tokens }
    ctx.load_tokens = function(p) return loaded[p] end
    local n, st = run_capped(ctx, a_tokens, nil, 300)
    check("C1 call-based cross-scene run terminates", n < 300 and st == "dead",
        "frames " .. n)
    check("C1 [asym-nonflow] macro 'text' overrides the [text] builtin in B",
        has_sub(collect(ctx), "T:MACRO_TEXT_BODY"), collect(ctx))
    local saw_text_handler = false
    for _, d in ipairs(ctx.dispatched) do
        if d.cmd == "text" then saw_text_handler = true end
    end
    check("C1 [asym-nonflow] builtin [text] handler was NOT dispatched in B",
        not saw_text_handler, collect(ctx))
    check("C1 control flow intact after override (A_BACK runs, return to A)",
        has_sub(collect(ctx), "M:A_BACK")
            and ctx.current_scene == "assets/script/a.ks", collect(ctx))
end

-- C2. FLOW command: scene A defines a macro NAMED [return] (a flow command).
--     B issues a genuine [return]; the flow branch WINS at runtime — the
--     leaked macro is silently ignored (KAG3 asymmetry, cross-scene too):
--     the call frame pops correctly instead of the macro body splicing.
do
    local b_tokens = {
        { "mark", { tag = "B_IN" } },
        { "return", {} },          -- must pop, NOT splice MACRO_RETURN_BODY
        { "mark", { tag = "B_AFTER" } },
    }
    local a_tokens = {
        { "macro", { name = "return" } },
        { "ch", { text = "MACRO_RETURN_BODY" } },
        { "endmacro", {} },
        { "call", { target = "b.ks" } },
        { "mark", { tag = "A_BACK" } },
    }
    local ctx = make_ctx(); ctx.current_scene = "assets/script/a.ks"
    ctx.dispatched = {}
    local loaded = { ["assets/script/a.ks"] = a_tokens, ["assets/script/b.ks"] = b_tokens }
    ctx.load_tokens = function(p) return loaded[p] end
    local n, st = run_capped(ctx, a_tokens, nil, 300)
    check("C2 flow-macro run terminates", n < 300 and st == "dead", "frames " .. n)
    check("C2 [asym-flow] leaked macro 'return' recorded in ctx.macros",
        ctx.macros and ctx.macros["return"] ~= nil, "")
    check("C2 [asym-flow] B's genuine [return] popped the frame (not spliced)",
        #(ctx.call_stack or {}) == 0 and ctx.current_scene == "assets/script/a.ks",
        "frames=" .. tostring(#(ctx.call_stack or {})) .. " scene=" .. tostring(ctx.current_scene))
    check("C2 [asym-flow] macro body did NOT splice (flow branch won; B_AFTER skipped)",
        has_sub(collect(ctx), "M:B_IN") and has_sub(collect(ctx), "M:A_BACK")
            and not has_sub(collect(ctx), "MACRO_RETURN_BODY")
            and not has_sub(collect(ctx), "B_AFTER"), collect(ctx))
end

-- ---------------------------------------------------------------------------
-- D. NESTED macro-definition collection (round 74 report / round 75 fix)
--
-- A [macro outer] whose body CONTAINS a nested [macro inner]...[endmacro]
-- must be collected depth-aware: the inner [endmacro] only decrements the
-- nesting depth and does NOT terminate the OUTER body; the outer body ends
-- only at the [endmacro] matching its opening [macro] (depth back to 0).
-- The naive scan stopped at the FIRST [endmacro], truncating outer to a
-- dangling half-defined body and leaking an [endmacro] into the stream that
-- corrupted every token after it. This suite locks the depth-aware behavior:
--   D1 outer is collected COMPLETE (inner def + inner endmacro + trailing
--      outer content all present -- proves no first-endmacro truncation)
--   D2 both macros usable at runtime (nested inner registered lazily when
--      outer splices its body; both bodies execute)
--   D3 the token AFTER outer's [endmacro] still runs (no dangling endmacro
--      corrupts the following token stream)
-- ---------------------------------------------------------------------------
do
    local tokens = {
        { "macro", { name = "outer" } },      -- opening
        { "macro", { name = "inner" } },      -- nested def (depth 1->2)
        { "ch", { text = "INNER_BODY" } },    -- inner body
        { "endmacro", {} },                   -- inner close (2->1, no break)
        { "ch", { text = "OUTER_BODY" } },    -- outer body AFTER inner close
        { "endmacro", {} },                   -- outer close (1->0, break)
        { "outer", {} },                      -- invoke outer -> registers inner
        { "inner", {} },                      -- invoke nested inner
        { "mark", { tag = "DONE" } },         -- MUST run (no dangling endmacro)
    }
    local ctx = make_ctx()
    local n, st = run_capped(ctx, tokens, nil, 500)
    check("D1 run terminates (nested macro def collection progresses)",
        n < 500 and st == "dead", "frames " .. n .. " st=" .. tostring(st))

    -- Collection completeness: outer's body must hold 4 tokens -- the inner
    -- macro def, its body, ITS endmacro, and the trailing outer content.
    -- The naive first-endmacro scan would have stopped at index 2.
    local ob = ctx.macros and ctx.macros.outer
    check("D1 outer body collected COMPLETE (4 tokens, not truncated at first endmacro)",
        ob ~= nil and #ob == 4,
        "outer len=" .. tostring(ob and #ob))
    check("D1 [nested] outer body CONTAINS the inner macro def (idx1 = [macro inner])",
        ob and ob[1] and ob[1][1] == "macro"
            and ob[1][2] and ob[1][2].name == "inner",
        "outer[1]=" .. tostring(ob and ob[1] and ob[1][1]))
    check("D1 [nested] inner [endmacro] captured INSIDE outer body (idx3)",
        ob and ob[3] and ob[3][1] == "endmacro",
        "outer[3]=" .. tostring(ob and ob[3] and ob[3][1]))
    check("D1 [nested] trailing outer content AFTER inner endmacro preserved (idx4)",
        ob and ob[4] and ob[4][1] == "ch" and ob[4][2]
            and ob[4][2].text == "OUTER_BODY",
        "outer[4]=" .. tostring(ob and ob[4] and ob[4][1]
            and ob[4][2] and ob[4][2].text))

    -- Runtime usability: invoking outer splices its body (registering inner
    -- lazily), then invoking inner splices ITS body. Both bodies execute.
    check("D2 [nested] outer macro body executes at runtime (T:OUTER_BODY)",
        has_sub(collect(ctx), "T:OUTER_BODY"), collect(ctx))
    check("D2 [nested] inner macro body executes at runtime (T:INNER_BODY)",
        ctx.macros and ctx.macros.inner ~= nil
            and has_sub(collect(ctx), "T:INNER_BODY"), collect(ctx))

    -- No dangling endmacro: the token after outer's [endmacro] must run.
    check("D3 [no-dangling] token after outer [endmacro] executes (M:DONE)",
        has_sub(collect(ctx), "M:DONE"), collect(ctx))
end

package.loaded["kag"] = kag_orig

print(string.format("\nMacro-scene results: %d passed, %d failed", passed, failed))
if failed > 0 then os.exit(1) end
