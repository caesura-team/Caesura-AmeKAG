-- =============================================================================
--  test_bench_dispatch.lua — Scheduler hot-loop dispatch benchmark
--  Round-66 perf-baseline style: deterministic correctness + loose wall-clock
--  bounds (CI-friendly). Builds a compiled token stream via
--  tokenizer.parse + compiler.compile, drives scheduler.run to completion,
--  asserts the dispatch count and a generous completion bound, and reports
--  throughput (tokens/sec) — printed, never asserted.
--  Standalone: run with external/lua/lua.exe tests/scripts/test_bench_dispatch.lua
-- =============================================================================

package.path = "scripts/?.lua;scripts/kag/?.lua;" .. package.path

-- Test handlers are explicit DSL extensions; a recording KAG table alone
-- does not register commands. Keep the production public selector intact.
require("kag.commands.text") -- actual ch/text contracts before recorder replacement

local tokenizer = require("tokenizer")
local compiler = require("kag.compiler")
local scheduler = require("scheduler")

local passed, failed = 0, 0
local function check(name, cond, detail)
    if cond then
        passed = passed + 1
        print(string.format("  [PASS] %s", name))
    else
        failed = failed + 1
        print(string.format("  [FAIL] %s  -- %s", name, detail or ""))
    end
end

print("\n=== Scheduler Dispatch Benchmark ===\n")

-- Mock kag table (test_scheduler pattern): handlers record into ctx.dispatched
-- so we can count real dispatches without a GPU. 'ch' retains its real contract
-- while only the handler boundary is replaced by this recorder.
local kag_mock = {}
kag_mock.ch = function(ctx, params)
    ctx.dispatched[#ctx.dispatched + 1] = { cmd = "ch", params = params }
end
-- generic fallback for any unexpected command we might emit
local function mock_generic(cmd)
    return function(ctx, params)
        ctx.dispatched[#ctx.dispatched + 1] = { cmd = cmd, params = params }
    end
end
for _, cmd in ipairs(
    {"ch","p","bg","fg","cl","wait","playbgm","playse","playvoice","quake",
     "flash","fade","blur","transition","stop","ruby"}
) do
    kag_mock[cmd] = kag_mock[cmd] or mock_generic(cmd)
end
-- Must be loaded BEFORE compiler.compile: the compiler binds handlers by
-- require("kag") at compile time — a late install would bind the real handlers
-- and blow the dispatch count.
package.loaded["kag"] = kag_mock

local function make_ctx()
    local ctx = {
        f = {}, sf = {}, tf = {}, mp = {}, lf = {},
        tokens = {}, token_index = 1, call_stack = {},
        layers = {}, backlog = {}, active_operations = {},
        macros = {}, stop_flag = false, dispatched = {},
        load_tokens = function() end,
    }
    return ctx
end

-- Drive scheduler.run(tokens) to completion (ch is non-blocking -> no yields,
-- finish in a single resume; loop defensively with a safety cap).
local function run_full(tokens)
    local ctx = make_ctx()
    local co = coroutine.create(function() scheduler.run(ctx, tokens, 1) end)
    local resumes = 0
    while coroutine.status(co) ~= "dead" and resumes < 100000 do
        resumes = resumes + 1
        local ok = coroutine.resume(co, 16)
        if not ok then break end
    end
    return ctx, resumes
end

-- ---------------------------------------------------------------------------
-- 1) Hot loop: ~2000 [ch] commands, compiled once, dispatched to completion.
-- ---------------------------------------------------------------------------
do
    local N = 2000
    local lines = {}
    for i = 1, N do
        lines[#lines + 1] = string.format('[ch name="A" text="Line number %d"]', i)
    end
    local src = table.concat(lines, "\n")

    local t0 = os.clock()
    local tokens = tokenizer.parse(src)
    local tParse = os.clock() - t0
    check("tokenizer parses " .. N .. " ch commands", #tokens == N,
          "got " .. #tokens .. " tokens")

    -- explicit compile phase (per task spec)
    local t1 = os.clock()
    compiler.compile(tokens)
    local tCompile = os.clock() - t1
    check("compiler marks stream compiled", tokens._compiled ~= nil)
    check("compile-time handlers bound for every ch",
          tokens._compiled and #tokens._compiled.handlers == N)

    -- hot loop to completion
    local t2 = os.clock()
    local ctx, resumes = run_full(tokens)
    local tRun = os.clock() - t2
    local elapsed = (os.clock() - t0)

    check("dispatch count == " .. N, #ctx.dispatched == N,
          "got " .. #ctx.dispatched)
    -- loose wall-clock bound for the whole parse+compile+run pipeline
    check("total under 10s", elapsed < 10.0, string.format("%.2fs", elapsed))

    local tokPerSec = N / math.max(tRun, 1e-9)
    print(string.format(
        "  [bench] hot loop: %d ch dispatched in %.1f ms = %.0f tokens/sec",
        #ctx.dispatched, tRun * 1000, tokPerSec))
    print(string.format(
        "  [bench] pipeline: parse %.1f ms / compile %.1f ms / run %.1f ms / total %.1f ms",
        tParse * 1000, tCompile * 1000, tRun * 1000, elapsed * 1000))
end

-- ---------------------------------------------------------------------------
-- 2) Flow tokens mixed in: [if]/[else]/[endif] branch + [jump]/[label].
--    Covers branch dispatch through the compiled flow table.
-- ---------------------------------------------------------------------------
do
    local toks = {
        { "if",    { exp = "f.branch == 1" } },
        { "ch",    { text = "taken" } },
        { "else",  { } },
        { "ch",    { text = "not-taken" } },
        { "endif", { } },
        { "jump",  { storage = "*L2" } },
        { "ch",    { text = "skipped" } },
        { "label", { name = "L2" } },
        { "ch",    { text = "after" } },
    }
    local ctx = make_ctx()
    ctx.f.branch = 1
    -- hand-built stream compiles lazily on first run; ensure compiled flow
    compiler.compile(toks)
    local co = coroutine.create(function() scheduler.run(ctx, toks, 1) end)
    local n = 0
    while coroutine.status(co) ~= "dead" and n < 1000 do
        n = n + 1
        coroutine.resume(co, 16)
    end
    local texts = {}
    for _, d in ipairs(ctx.dispatched) do
        texts[#texts + 1] = (d.params and d.params.text) or d.cmd
    end
    check("branch dispatch order", table.concat(texts, ",") == "taken,after",
          table.concat(texts, ","))
end

print(string.format("\nResults: %d passed, %d failed", passed, failed))
if failed > 0 then os.exit(1) end
print("BENCH DISPATCH TESTS DONE")