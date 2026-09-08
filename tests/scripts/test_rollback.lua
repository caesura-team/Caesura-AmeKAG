-- Rollback unit tests: snapshot capture/restore semantics + runner wiring.
-- Loaded via tests/scripts/run_lua_tests.lua; prints PASS/FAIL per check.
local results = {}  -- file scope: runner shares globals
local check = function(name, cond)
    if cond then print("PASS " .. name) else print("FAIL " .. name) end
        results[#results + 1] = cond
end

-- --- capture/restore round-trip -------------------------------------------
local snapshot = require("kag.snapshot")
local system = require("system")
local ctx = {
    current_scene = "demo/rollback_demo.ks",
    currentScene = "demo/rollback_demo.ks",
    token_index = 7,
    _resume_index = 9,
    f = { hp = 100, name = "Ame" },
    sf = { flag = true },
    lf = { local_value = 10 },
    variables = { gold = 5 },
    backlog = { { name = "a", text = "line1" }, { name = "b", text = "line2" } },
    seen_scenes = { ["demo/rollback_demo.ks"] = { [5] = true } },
    call_stack = { { tokens = {}, index = 3 } },
    text_speed = 40,
    skip_mode = false,
    auto_mode = true,
    waiting_input = true,
    reveal = { total = 6, elapsed = 3 },
    text_state = { line = 2, char_offset = 1, cursor_x = 32, cursor_y = 580, draws = {} },
    layers = { _snapshot_ok = true },
}
check("capture returns table", type(snapshot.capture(ctx)) == "table")
local snap = snapshot.capture(ctx)

-- mutate ctx after capture; restore must bring back the captured values
ctx.f.hp = 1
ctx.variables.gold = 99
ctx.token_index = 99
ctx._resume_index = 100
ctx.lf.local_value = 20
ctx.backlog[#ctx.backlog + 1] = { name = "c", text = "line3" }
ctx.reveal.elapsed = 0
ctx.text_state.line = 9

local ok = snapshot.restore(ctx, snap)
check("restore ok", ok == true)
check("restore scene", ctx.current_scene == "demo/rollback_demo.ks")
check("restore token_index", ctx.token_index == 7)
check("restore deep f", ctx.f.hp == 100)
check("restore deep variables", ctx.variables.gold == 5)
check("restore backlog truncated", #ctx.backlog == 2)
check("restore reveal complete", ctx.reveal.elapsed == ctx.reveal.total * ctx.text_speed
      and ctx.text_state.reveal_chars == ctx.reveal.total)
check("restore text_state", ctx.text_state.line == 2)
check("restore seen_scenes", ctx.seen_scenes["demo/rollback_demo.ks"][5] == true)

-- --- runner wiring ----------------------------------------------------------
local kag_runner = require("kag_runner")
check("rollback exists", type(kag_runner.rollback) == "function")

-- The former source-string checks are exercised through the real runner in
-- test_rollback_session.lua: clear stop_flag, first-click reveal without a
-- push, second-click capture and continuation. Keep direct value regressions
-- here; neither copied source snippets nor function spelling prove behavior.
check("restore preserves separate next execution cursor", ctx._resume_index == 9)
check("restore local frame", ctx.lf.local_value == 10)
ctx.text_state.draws[#ctx.text_state.draws + 1] = { text = "future" }
ctx.reveal.elapsed = 0
check("restored presentation stays independent from retained snapshot",
      #snap.text_state.draws == 0 and snap.reveal.elapsed == 240)
snap.lf = nil
ctx.lf = { only_future = true }
check("missing historical local frame clears future locals",
      snapshot.restore(ctx, snap) and next(ctx.lf) == nil)

for _, passed in ipairs(results) do assert(passed, "rollback check failed") end
print("ROLLBACK TESTS DONE")
