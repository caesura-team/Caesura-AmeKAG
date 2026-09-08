-- test_compiler.lua — compile-time front-end tests (scripts/kag/compiler.lua)
-- Phase A of the Neo-Genesis core rewrite: token streams compile once into
-- a side table (flow jump table, pre-translated expressions, normalized
-- params, handler bindings, label index) that the scheduler consumes.
package.path = "scripts/?.lua;scripts/kag/?.lua;" .. package.path
local passed, failed = 0, 0
local function check(name, cond)
    if cond then print("PASS " .. name) passed = passed + 1
    else print("FAIL " .. name) failed = failed + 1 end
end

local tokenizer = require("tokenizer")
local compiler = require("kag.compiler")

-- ---------------------------------------------------------------------------
-- 1. compile() produces the side table; idempotent recompile
-- ---------------------------------------------------------------------------
local toks = tokenizer.parse(
    "[ch text=\"a\"]\n*start\n[if exp=\"f.hp > 10 && f.flag != 0\"]\n"
    .. "[ch text=\"ok\"]\n[else]\n[ch text=\"no\"]\n[endif]\n[jump target=*start]")
compiler.compile(toks)
check("compiled side table present", toks._compiled ~= nil)
check("labels indexed at compile time", toks._compiled.labels.start == 2)
local flow = toks._compiled.flow
check("if jump_false compiled (first elseif/else/endif)",
      flow[3] and flow[3].kind == "if" and flow[3].jump_false == 5)
check("else jump_chain_end compiled", flow[5] and flow[5].kind == "else"
      and flow[5].jump_chain_end == 7)
check("jump target recorded", flow[8] and flow[8].kind == "jump"
      and flow[8].target == "*start")
check("TJS && translated at compile time",
      toks._compiled.exprs[3] == "f.hp > 10 and f.flag ~= 0")

-- idempotent: recompile keeps the same side table (no double work)
local first = toks._compiled
compiler.compile(toks)
check("recompile is a no-op", toks._compiled == first)

-- invalidate drops the table; recompile rebuilds
compiler.invalidate(toks)
check("invalidate drops side table", toks._compiled == nil)
compiler.compile(toks)
check("recompile after invalidate rebuilds", toks._compiled ~= nil)

-- ---------------------------------------------------------------------------
-- 2. params normalized: bare positional -> numeric key, pairs -> named
-- ---------------------------------------------------------------------------
local toks2 = tokenizer.parse("[delay 500][ch text=\"x\" speed=10]")
compiler.compile(toks2)
local p1 = toks2._compiled.params[1]
check("bare positional -> params[1] numeric", p1[1] == "500")
local p2 = toks2._compiled.params[2]
check("named params normalized", p2.text == "x" and p2.speed == "10")

-- 2b. compile-time positional -> named via contract positional_index
pcall(require, "kag.commands.system")
pcall(require, "kag.commands.video")
local toks2b = tokenizer.parse("[set f.hp 30][video test.mpg]")
compiler.compile(toks2b)
local ps = toks2b._compiled.params
check("set bare -> var mapped", ps[1].var == "f.hp" and ps[1][1] == "f.hp")
check("video bare -> file mapped", ps[2].file == "test.mpg")
check("unmigrated bare keeps numeric key only",
      toks2._compiled.params[1][1] == "500"
      and toks2._compiled.params[1].ms == nil)

-- ---------------------------------------------------------------------------
-- 3. while/for/break/continue jump targets compiled
-- ---------------------------------------------------------------------------
local toks3 = tokenizer.parse(
    "[while exp=\"f.i < 5\"]\n[ch text=\"loop\"]\n[if exp=\"f.i > 3\"]\n"
    .. "[break]\n[endif]\n[endwhile]\n[for var=i start=0 end=3]\n"
    .. "[continue]\n[endfor]")
compiler.compile(toks3)
local flow3 = toks3._compiled.flow
check("while skip_body_to compiled", flow3[1] and flow3[1].kind == "while"
      and flow3[1].skip_body_to == 6)
check("break loop_end compiled", flow3[4] and flow3[4].kind == "break"
      and flow3[4].loop_end == 6)
check("for skip_body_to compiled", flow3[7] and flow3[7].kind == "for"
      and flow3[7].skip_body_to == 9)

-- ---------------------------------------------------------------------------
-- 4. switch case table: depth-aware, O(1) by value
-- ---------------------------------------------------------------------------
local toks4 = tokenizer.parse(
    "[switch mode]\n[case fast]\n[ch text=\"F\"]\n[switch mode]\n"
    .. "[case slow]\n[ch text=\"NESTED\"]\n[endswitch]\n[default]\n"
    .. "[ch text=\"D\"]\n[endswitch]")
compiler.compile(toks4)
local f4 = toks4._compiled.flow[1]
check("switch case table built", f4.kind == "switch" and f4.cases ~= nil)
check("depth-1 case mapped", f4.cases.fast == 3)
check("nested case NOT in outer table", f4.cases.slow == nil)
check("default mapped", f4.default == 9)
check("endswitch index", f4.endswitch == 10)

-- ---------------------------------------------------------------------------
-- 5. handler bindings resolved at compile time
-- ---------------------------------------------------------------------------
local saved_kag = package.loaded["kag"]
package.loaded["kag"] = { ch = function() end, wait = function() end }
local toks5 = tokenizer.parse("[ch text=\"a\"][wait 100]")
compiler.compile(toks5)
local h = toks5._compiled.handlers
check("handler bound for ch", h[1] ~= nil)
check("handler bound for wait", h[2] ~= nil)
package.loaded["kag"] = saved_kag

-- ---------------------------------------------------------------------------
-- 6. scheduler fast path: compiled stream executes identically
-- ---------------------------------------------------------------------------
local scheduler = require("scheduler")
-- NOTE: the mock kag table must be installed BEFORE compile so handler
-- bindings capture the mock (compile binds kag[cmd] at compile time).
local kag_orig = package.loaded["kag"]
local d = {}
package.loaded["kag"] = setmetatable({}, { __index = function(_, k)
    return function(c2, p2) d[#d + 1] = { k, p2 } end
end})
local toks6 = tokenizer.parse(
    "[if exp=\"f.a > 1 && f.b != 2\"]\n[ch text=\"taken\"]\n[else]\n"
    .. "[ch text=\"not-taken\"]\n[endif]\n*end\n[stop]")
compiler.compile(toks6)
local ctx6 = { f = { a = 5, b = 1 }, tf = {}, sf = {}, mp = {},
    variables = {}, current_scene = "t.ks", token_index = 1,
    label_index = toks6._compiled.labels }
local co = coroutine.create(function() scheduler.run(ctx6, toks6, 1) end)
while coroutine.status(co) ~= "dead" do coroutine.resume(co) end
check("compiled stream dispatches if-taken", d[1] and d[1][2].text == "taken")

-- same scene with the OTHER branch taken (env differs, expr cache keyed by
-- env identity -- compiled source must not leak stale values)
-- NOTE: install a FRESH mock before compile -- handler bindings capture
-- the kag table present at compile time (compile binds kag[cmd] once).
local d2 = {}
package.loaded["kag"] = setmetatable({}, { __index = function(_, k)
    return function(c2, p2) d2[#d2 + 1] = { k, p2 } end
end})
local toks6b = tokenizer.parse(
    "[if exp=\"f.a > 1 && f.b != 2\"]\n[ch text=\"taken\"]\n[else]\n"
    .. "[ch text=\"not-taken\"]\n[endif]\n*end\n[stop]")
compiler.compile(toks6b)
local ctx6b = { f = { a = 0, b = 2 }, tf = {}, sf = {}, mp = {},
    variables = {}, current_scene = "t.ks", token_index = 1,
    label_index = toks6b._compiled.labels }
local co2 = coroutine.create(function() scheduler.run(ctx6b, toks6b, 1) end)
while coroutine.status(co2) ~= "dead" do coroutine.resume(co2) end
check("compiled stream dispatches else branch (env-identity cache)",
      d2[1] and d2[1][2].text == "not-taken")
package.loaded["kag"] = kag_orig

-- ---------------------------------------------------------------------------
-- 7. record-format input (tokenizer.parse output) compiles too
-- ---------------------------------------------------------------------------
local toks7 = tokenizer.parse("[ch text=\"plain\"]")
-- tokenizer returns record format; compiler accepts both
compiler.compile(toks7)
check("record-format stream compiled", toks7._compiled ~= nil)

-- ---------------------------------------------------------------------------
-- 8. Battle 1d regression: statically-safe macros inline at COMPILE time for
--    real .ks scenes (tokenizer.parse raw pair-array params) — zero runtime
--    splice; erasemacro / branch-defined macros keep the runtime path.
-- ---------------------------------------------------------------------------
local function count_cmd(stream, cmd)
    local n = 0
    for _, t in ipairs(stream) do
        if type(t) == "table" and t[1] == cmd then n = n + 1 end
    end
    return n
end

-- static-safe: definition at depth 0, before calls, no erase/redef
local toks8 = tokenizer.parse([[
*start
[macro shout args="who,msg"]
[ch name="%who%" text="%msg%！"]
[endmacro]
[shout who="Hero" msg="参数化宏"]
[shout who="Side" msg="second"]
]])
compiler.compile(toks8)
check("8a: macro call sites inlined at compile time",
      count_cmd(toks8, "shout") == 0)
local texts8 = {}
for _, t in ipairs(toks8) do
    if t[1] == "ch" and t[2] and t[2].text then texts8[#texts8 + 1] = t[2].text end
end
check("8b: %arg% preserved at inlined call sites",
      texts8[2] == "参数化宏！" and texts8[3] == "second！")
check("8c: inlined body bound a handler",
      toks8._compiled.handlers[5] ~= nil)

-- erasemacro anywhere -> call site stays on the runtime splice path
local toks9 = tokenizer.parse([[
*start
[macro shout args="who"]
[ch text="%who%"]
[endmacro]
[erasemacro shout]
[shout who="X"]
]])
compiler.compile(toks9)
check("8d: erasemacro keeps runtime splice path",
      count_cmd(toks9, "shout") == 1)

-- definition inside a flow branch -> not statically safe
local toks10 = tokenizer.parse([[
*start
[if exp="f.x > 1"]
[macro inner args="a"]
[ch text="%a%"]
[endmacro]
[endif]
[inner a="Y"]
]])
compiler.compile(toks10)
check("8e: branch-defined macro keeps runtime splice path",
      count_cmd(toks10, "inner") == 1)

-- ---------------------------------------------------------------------------
-- 9. Non-flow expression commands: [button cond] AOT at compile time
-- ---------------------------------------------------------------------------
local toksB = tokenizer.parse("[button cond=\"f.hp != 0\" text=\"go\"]\n[endbutton]\n")
compiler.compile(toksB)
check("9a: [button cond] translated at compile time",
      toksB[1][2] and toksB[1][2].cond == "f.hp ~= 0")
local exprLang = require("kag.expr")
local ctx9 = { f = { hp = 5 }, sf = {}, tf = {}, mp = {}, lf = {} }
local ok9, v9 = exprLang.evaluateTranslated(ctx9, toksB[1][2].cond, toksB[1][2].cond)
check("9b: translated cond evaluates via evaluateTranslated",
      ok9 == true and v9 == true)
ctx9.f.hp = 0
local ok9b, v9b = exprLang.evaluateTranslated(ctx9, toksB[1][2].cond, toksB[1][2].cond)
check("9c: translated cond respects variable change",
      ok9b == true and v9b == false)


-- ---------------------------------------------------------------------------
-- 10. Lua 5.4 const-loop-variable regression (round 31 wasmoon spike):
--     for x in ... do x = ... end is a compile ERROR in 5.4. These paths
--     were never exercised before; the spike surfaced them.
-- ---------------------------------------------------------------------------
local schema = require("kag.schema")

-- 10a. Schema.coerce with a list-typed param (comma-separated string)
local saved10 = package.loaded["kag"]
schema.define("__spike_list_test", {
    colors = { type = "list", item_type = "string" },
    nums = { type = "list", item_type = "number" },
    flags = { type = "list", item_type = "boolean" },
})
local okL, coL = pcall(schema.coerce, "__spike_list_test",
    { colors = "red, green, blue", nums = "1,2, 3", flags = "true,no,1" })
check("10a: list params coerce comma-separated strings",
      okL and coL.colors[1] == "red" and coL.colors[2] == "green"
      and coL.colors[3] == "blue"
      and coL.nums[2] == 2 and coL.nums[3] == 3
      and coL.flags[1] == true and coL.flags[2] == false and coL.flags[3] == true)
check("10b: list keeps trimmed elements only", okL and #coL.colors == 3)
package.loaded["kag"] = saved10

-- 10c. scheduler runtime macro-args path (branch-defined macro forces the
--      runtime splice path, which re-reads params.args and trims it)
local scheduler10 = require("scheduler")
local kag10 = {}
local disp10 = {}
setmetatable(kag10, { __index = function(_, k)
    return function(c2, p2) disp10[#disp10 + 1] = { k, p2 } end
end})
package.loaded["kag"] = kag10
local toks10 = tokenizer.parse(
    "*start\n"
    .. "[if exp=\"f.x > 1\"]\n"
    .. "[macro shout args=\"who,msg\"]\n"
    .. "[ch text=\"%who% %msg%\"]\n"
    .. "[endmacro]\n"
    .. "[endif]\n"
    .. "[shout who=\"Hero\" msg=\"hi\"]\n")
compiler.compile(toks10)
local ctx10 = { f = { x = 2 }, tf = {}, sf = {}, mp = {}, lf = {},
    variables = {}, current_scene = "t.ks", token_index = 1,
    label_index = toks10._compiled.labels }
local co10 = coroutine.create(function() scheduler10.run(ctx10, toks10, 1) end)
while coroutine.status(co10) ~= "dead" do coroutine.resume(co10) end
check("10c: runtime macro args path survives (no const-var crash)",
      #disp10 >= 1)
package.loaded["kag"] = nil

-- 10d. ks_bake line-normalization path is guarded by pcall — just ensure
--      the module still loads (the loop was fixed).
local okBake = pcall(require, "ks_bake")
check("10d: ks_bake loads after loop-var fix", okBake == true)
-- ---------------------------------------------------------------------------
-- 11a-11e. [set f.x = 5] assignment-sugar "=" separator (round 46).
--  [set f.coins = 100] tokenizes as bare {1,"f.coins"},{2,"="},{3,"100"};
--  normalize_params must drop the standalone "=" and shift positions so
--  positional_index mapping yields var="f.coins", value="100" (was: the
--  literal "=" was stored and branches never varied).
-- ---------------------------------------------------------------------------
local sys11 = require("kag.commands.system")
local toks11 = tokenizer.parse("[set f.coins = 100]")
compiler.compile(toks11)
local p11 = toks11[1][2]
check("11a: '=' separator stripped, value shifts down",
      p11.var == "f.coins" and p11.value == "100")
local ctx11 = { f = {}, sf = {}, tf = {}, mp = {}, lf = {},
    variables = {}, current_scene = "t.ks", token_index = 1,
    tokens = toks11 }
local co11 = coroutine.create(function() scheduler.run(ctx11, toks11, 1) end)
while coroutine.status(co11) ~= "dead" do coroutine.resume(co11) end
check("11b: [set f.x = N] stores the number", ctx11.f.coins == 100)

-- 11c. a non-bare "=" value is NOT a separator: the bare positional
--      grammar (uval) keeps quotes as literals, so '[set f.s "="]'
--      stores the 3-char string '"="' — the "=" strip must not touch it.
local toks11c = tokenizer.parse('[set f.s "="]')
compiler.compile(toks11c)
local ctx11c = { f = {}, sf = {}, tf = {}, mp = {}, lf = {},
    variables = {}, current_scene = "t.ks", token_index = 1,
    tokens = toks11c }
local co11c = coroutine.create(function() scheduler.run(ctx11c, toks11c, 1) end)
while coroutine.status(co11c) ~= "dead" do coroutine.resume(co11c) end
check("11c: quoted '=' value survives (uval literal)", ctx11c.f.s == '"="')

-- 11d. no-equals form still works ([set f.x 42])
local toks11d = tokenizer.parse("[set f.x 42]")
compiler.compile(toks11d)
local ctx11d = { f = {}, sf = {}, tf = {}, mp = {}, lf = {},
    variables = {}, current_scene = "t.ks", token_index = 1,
    tokens = toks11d }
local co11d = coroutine.create(function() scheduler.run(ctx11d, toks11d, 1) end)
while coroutine.status(co11d) ~= "dead" do coroutine.resume(co11d) end
check("11d: bare positional form still works", ctx11d.f.x == 42)

-- 11e. expression assignment belongs to [eval exp="..."], not [set]
local toks11e = tokenizer.parse('[eval exp="f.luck = math.random(2)"]')
compiler.compile(toks11e)
local ctx11e = { f = {}, sf = {}, tf = {}, mp = {}, lf = {},
    variables = {}, current_scene = "t.ks", token_index = 1,
    tokens = toks11e }
local co11e = coroutine.create(function() scheduler.run(ctx11e, toks11e, 1) end)
while coroutine.status(co11e) ~= "dead" do coroutine.resume(co11e) end
check("11e: eval expression assignment runs", type(ctx11e.f.luck) == "number")

-- 11f. [eval] TJS operator translation (round 61: && || ! != previously
--      failed as invalid Lua — eval is a statement, so operators only)
local toks11f = tokenizer.parse('[eval exp="f.ok = f.hp > 10 && f.flag"]')
compiler.compile(toks11f)
local ctx11f = { f = { hp = 30, flag = true }, sf = {}, tf = {}, mp = {}, lf = {},
    variables = {}, current_scene = "t.ks", token_index = 1,
    tokens = toks11f }
local co11f = coroutine.create(function() scheduler.run(ctx11f, toks11f, 1) end)
while coroutine.status(co11f) ~= "dead" do coroutine.resume(co11f) end
check("11f: eval && assignment translates", ctx11f.f.ok == true)

local toks11g = tokenizer.parse('[eval exp="f.alt = f.missing || 7"]')
compiler.compile(toks11g)
local ctx11g = { f = {}, sf = {}, tf = {}, mp = {}, lf = {},
    variables = {}, current_scene = "t.ks", token_index = 1,
    tokens = toks11g }
local co11g = coroutine.create(function() scheduler.run(ctx11g, toks11g, 1) end)
while coroutine.status(co11g) ~= "dead" do coroutine.resume(co11g) end
check("11g: eval || assignment translates", ctx11g.f.alt == 7)

local toks11h = tokenizer.parse('[eval exp="f.neg = !f.flag"]')
compiler.compile(toks11h)
local ctx11h = { f = { flag = false }, sf = {}, tf = {}, mp = {}, lf = {},
    variables = {}, current_scene = "t.ks", token_index = 1,
    tokens = toks11h }
local co11h = coroutine.create(function() scheduler.run(ctx11h, toks11h, 1) end)
while coroutine.status(co11h) ~= "dead" do coroutine.resume(co11h) end
check("11h: eval ! assignment translates", ctx11h.f.neg == true)

-- 11i. eval ternary assignment (round 68: RHS full pipeline)
local toks11i = tokenizer.parse('[eval exp="f.pick = f.flag ? 1 : 2"]')
compiler.compile(toks11i)
local ctx11i = { f = { flag = true }, sf = {}, tf = {}, mp = {}, lf = {},
    variables = {}, current_scene = "t.ks", token_index = 1,
    tokens = toks11i }
local co11i = coroutine.create(function() scheduler.run(ctx11i, toks11i, 1) end)
while coroutine.status(co11i) ~= "dead" do coroutine.resume(co11i) end
check("11i: eval ternary assignment then-branch", ctx11i.f.pick == 1)
local toks11j = tokenizer.parse('[eval exp="f.pick2 = f.flag ? 1 : 2"]')
compiler.compile(toks11j)
local ctx11j = { f = { flag = false }, sf = {}, tf = {}, mp = {}, lf = {},
    variables = {}, current_scene = "t.ks", token_index = 1,
    tokens = toks11j }
local co11j = coroutine.create(function() scheduler.run(ctx11j, toks11j, 1) end)
while coroutine.status(co11j) ~= "dead" do coroutine.resume(co11j) end
check("11j: eval ternary assignment else-branch", ctx11j.f.pick2 == 2)

-- 11k-11l. eval ternary conds with >= / != (round 70 review C1)
local toks11k = tokenizer.parse('[eval exp="f.pick = f.lv >= 5 ? 1 : 0"]')
compiler.compile(toks11k)
local ctx11k = { f = { lv = 5 }, sf = {}, tf = {}, mp = {}, lf = {},
    variables = {}, current_scene = "t.ks", token_index = 1,
    tokens = toks11k }
local co11k = coroutine.create(function() scheduler.run(ctx11k, toks11k, 1) end)
while coroutine.status(co11k) ~= "dead" do coroutine.resume(co11k) end
check("11k: eval >= ternary cond translates", ctx11k.f.pick == 1)
local toks11l = tokenizer.parse('[eval exp="f.pick2 = f.hp != 0 ? 7 : 9"]')
compiler.compile(toks11l)
local ctx11l = { f = { hp = 3 }, sf = {}, tf = {}, mp = {}, lf = {},
    variables = {}, current_scene = "t.ks", token_index = 1,
    tokens = toks11l }
local co11l = coroutine.create(function() scheduler.run(ctx11l, toks11l, 1) end)
while coroutine.status(co11l) ~= "dead" do coroutine.resume(co11l) end
check("11l: eval != ternary cond translates", ctx11l.f.pick2 == 7)
-- ---------------------------------------------------------------------------
-- 12a-12e. Dotted-key assignment + interpolation (round 50).
--  [set f.name = "Aoi"] must store the UNQUOTED string via the tokenizer's
--  dotted-key branch; ${expr} must translate TJS operators
--  (?: && !=) before load() — previously a ternary stayed verbatim.
-- ---------------------------------------------------------------------------
local function run_set(src)
    local toks = tokenizer.parse(src)
    compiler.compile(toks)
    local ctx = { f = {}, sf = {}, tf = {}, mp = {}, lf = {},
        variables = {}, current_scene = "t.ks", token_index = 1,
        tokens = toks }
    local co = coroutine.create(function() scheduler.run(ctx, toks, 1) end)
    while coroutine.status(co) ~= "dead" do coroutine.resume(co) end
    return ctx
end

local c12 = run_set('[set f.name = "Aoi"]')
check("12a: dotted [set f.x = \"v\"] strips quotes", c12.f.name == "Aoi")
local c12b = run_set("[set f.hp = 30]")
check("12b: dotted numeric assignment", c12b.f.hp == 30)
local c12c = run_set('[set f.msg = "hello world"]')
check("12c: dotted string with space", c12c.f.msg == "hello world")

-- ${ TJS operators (schema interpolation path)
local ctx12 = { f = { hp = 30, flag = true, name = "Aoi" }, sf = {}, tf = {},
    mp = {}, lf = {} }
local toks12 = tokenizer.parse('[ch name="N" text="Rank ${f.hp > 20 ? \'high\' : \'low\'}"]')
compiler.compile(toks12)
local p12 = schema.coerce("ch", toks12[1][2] or toks12[1].params, ctx12)
check("12d: ${ ternary translated (TJS ?:)", p12.text == "Rank high")
local toks12e = tokenizer.parse('[ch name="N" text="OK=${f.flag && f.hp > 10}"]')
compiler.compile(toks12e)
local p12e = schema.coerce("ch", toks12e[1][2] or toks12e[1].params, ctx12)
check("12e: ${ && translated", p12e.text == "OK=true")
local toks12f = tokenizer.parse('[ch name="N" text="T=${ {a=1,b=2}.b}"]')
compiler.compile(toks12f)
local p12f = schema.coerce("ch", toks12f[1][2] or toks12f[1].params, ctx12)
check("12f: ${ nested table constructor (balanced braces)", p12f.text == "T=2")



do
    local source = tokenizer.parse('*entry\n[if exp="true"]\n[set var="f.value" value=1]\n[endif]\n[end]')
    compiler.compile(source)
    local packed = assert(compiler.serialize(source))
    local first = assert(compiler.deserialize(packed))
    local second = assert(compiler.deserialize(packed))
    check("13a: deserialize does not attach runtime state to the bundle", packed.tokens._compiled == nil)
    check("13b: each executable stream owns its tokens", first ~= second and first ~= packed.tokens)
    check("13c: params sharing stays local to each stream", first[3][2] == first._compiled.params[3]
        and second[3][2] == second._compiled.params[3] and first[3][2] ~= second[3][2])
    first[3][2].value = 'mutated'
    first._compiled.labels.entry = 9
    first._compiled.flow[2].jump = -99
    check("13d: token mutations cannot edit the trusted bundle or another stream",
        packed.params[3].value ~= 'mutated' and second[3][2].value ~= 'mutated')
    check("13e: prepared label maps are independent", packed.labels.entry == 1 and second._compiled.labels.entry == 1)
    check("13f: prepared control metadata is independent", packed.flow[2].jump ~= -99 and second._compiled.flow[2].jump ~= -99)
end

-- AST compilation must execute the same language through the real scheduler.
do
    local semantic = require("kag.semantic")
    local scheduler = require("scheduler")
    local function run_ast_case(source, lane)
        local stream = lane == "ast" and compiler.compile_from_ast(semantic.parse(source, "parity.ks"))
            or compiler.compile(tokenizer.parse(source))
        local ctx = { f = {}, sf = {}, tf = {}, mp = {}, lf = {}, variables = {},
            current_scene = "parity.ks", token_index = 1, tokens = stream }
        local co = coroutine.create(function() scheduler.run(ctx, stream, 1) end)
        for _ = 1, 100 do
            if coroutine.status(co) == "dead" then return ctx, stream end
            local ok, err = coroutine.resume(co)
            assert(ok, err)
        end
        error("AST parity scene exceeded 100 scheduler resumes")
    end
    for _, lane in ipairs({ "source", "ast" }) do
        for _, syntax in ipairs({ "named", "bare" }) do
            local target = syntax == "named" and "target=" or ""
            local ctx, stream = run_ast_case('[eval exp="f.trace=\'begin\'"]\n'
                .. '[call ' .. target .. '*sub]\n[eval exp="f.trace=f.trace .. \'|return\'"]\n'
                .. '[jump ' .. target .. '*finish]\n*sub\n'
                .. '[eval exp="f.trace=f.trace .. \'|sub\'"]\n[return]\n*finish\n'
                .. '[eval exp="f.trace=f.trace .. \'|finish\'"]\n[end]', lane)
            check("14: " .. lane .. " " .. syntax .. " call returns and jump reaches finish",
                ctx.f.trace == "begin|sub|return|finish")
            check("14: " .. lane .. " " .. syntax .. " runtime label map excludes star",
                stream._compiled.labels.sub == 5 and stream._compiled.labels.finish == 8
                and stream._compiled.labels["*sub"] == nil)
        end
        local switched = run_ast_case('[eval exp="f.flag=\'yes\'"]\n[switch flag]\n'
            .. '[case no]\n[eval exp="f.branch=\'wrong\'"]\n'
            .. '[case yes]\n[eval exp="f.branch=\'selected\'"]\n'
            .. '[default]\n[eval exp="f.branch=\'default\'"]\n[endswitch]', lane)
        check("14: " .. lane .. " bare switch and case select the matching branch",
            switched.f.branch == "selected")
        local assigned_ok, assigned = pcall(run_ast_case, '[set f.name = "Aoi"]\n[set f.count = 3]\n'
            .. '[set f.count 7 value=9]', lane)
        check("14: " .. lane .. " dotted assignment and explicit named override execute",
            assigned_ok and assigned.f.name == "Aoi" and assigned.f.count == 9)
        if not assigned_ok then print("PARITY ERROR " .. lane .. ": " .. tostring(assigned)) end
        local missing = run_ast_case('[call target=*missing]\n[jump target=*absent]\n'
            .. '[eval exp="f.fell_through=true"]\n[end]', lane)
        check("14: " .. lane .. " unresolved targets still fall through",
            missing.f.fell_through == true)
        local duplicates = lane == "ast"
            and compiler.compile_from_ast(semantic.parse("*same\n[end]\n*same\n[end]", "duplicate.ks"))
            or compiler.compile(tokenizer.parse("*same\n[end]\n*same\n[end]"))
        check("14: " .. lane .. " duplicate labels still resolve to first definition",
            duplicates._compiled.labels.same == 1)
    end
end

if failed > 0 then
    print(string.format("COMPILER TESTS: %d passed, %d FAILED", passed, failed))
    os.exit(1)
end
print(string.format("COMPILER TESTS DONE (%d passed)", passed))
