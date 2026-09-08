-- =============================================================================
--  run_orphan_tests.lua — Standalone runner for tests that are order-
--  incompatible with the main suite (they create global mocks like
--  _G.layers/_G.backend and therefore must NOT run after sandbox.lua has
--  locked globals, nor pollute the main suite's module cache).
--
--  Usage: lua tests/scripts/run_orphan_tests.lua
--  Each test runs in this process sequentially; they were verified to pass
--  standalone (round 13: orphan-recovery audit). CI invokes this as a
--  separate step AFTER run_lua_tests.lua.
-- =============================================================================

local test_dir = arg and arg[0] and arg[0]:match("(.*[/\\])") or ""
package.path = test_dir .. "../../scripts/?.lua;" .. test_dir .. "?.lua;" .. package.path

local tests = {
    "test_tokenizer_adv",
    "test_full_story_parse",
    "test_integration",
    "test_kag_core",
    "test_kag_e2e",
    "test_kag_system_flow",
    "test_macro",
    "test_macro_deep",
    "test_p2_features",
    "test_contracts_runtime",
    "test_vfx_postfx",
    "test_tween",
    "test_layout_cmds",
    "test_settings_layout_pilot",
    "test_runtime_menu_cleanup",
    "test_contracts_runtime2",
    "test_contract_runtime_gaps",
    "test_saveflow",
    "test_save_load_samescene",
    "test_save_restore_transaction",
    "test_i18n_restore",
    "test_text_restore",
    "test_layer_restore",
    "test_transient_restore",
    "test_wait_restore_edges",
    "test_rollback_session",
    "test_rollback_boundaries",
    "test_rollback_presentation",
    "test_rollback_transaction",
    "test_rollback_capture",
    "test_rollback_stablepoints",
    "test_runtime_contracts",
    "test_flow_cache_compat",
    "test_bench_dispatch",
    "test_math_cmds",
    "test_character_cmds",
    "test_textspeed",
    "test_wait_delay",
    "test_kag3_import_e2e",
    "test_settings_config_deep",
    "test_select_crossscene_flow",
    "test_typewriter_sound",
    "test_hr",
    "test_errorui_wiring",
    "test_palette",
}

local passed, failed = 0, 0
print("=== Caesura Orphan Test Suite (isolated) ===\n")

-- Round 52 fix: tests that call os.exit (test_kag_core exits 0 on
-- success) would terminate THIS process mid-loop, silently skipping every
-- later test. Run each test in its own lua subprocess so os.exit is
-- contained (exit 0 = pass, non-zero = fail).
local lua_bin = arg and arg[-1] or "lua"

-- Native path separators: Windows cmd wants backslashes, POSIX sh wants
-- forward slashes (round 60: the orphan suite now runs on Linux/macOS CI
-- where tests\scripts\x.lua is a literal filename, not a path).
local SEP = package.config:sub(1, 1)
local function to_native(p)
    return SEP == "/" and p:gsub("\\", "/") or p:gsub("/", "\\")
end

local function run_isolated(name)
    -- Windows popen (cmd.exe /c) quoting rules:
    --  * unquoted relative exe works ("external\lua\lua.exe ...")
    --  * a command STARTING with a quote loses its outer quotes under
    --    cmd /c, so `"D:\path with space\lua.exe" ...` breaks into
    --    `D:\path` + garbage ('D:\...\Caesura' not recognized);
    --  * prefixing with `call` keeps the first char non-quote, so a
    --    quoted absolute exe (what git-bash argv[-1] resolves to) runs
    --    correctly. The test file path is always quoted.
    local bin = to_native(lua_bin)
    -- cmd splits UNQUOTED command tokens at spaces AND at parens
    -- (probe: 'D:\...\Caesura(AmeKAG)\lua.exe' -> 'D:\...\Caesura'
    -- not recognized), and an absolute exe is what git-bash argv[-1]
    -- resolves to. Wrap only when needed so the round-52 relative-exe
    -- path (no spaces/parens) keeps working unquoted.
    if bin:find("[ %(%)]") then
        bin = 'call "' .. bin .. '"'
    end
    local f = to_native(test_dir .. name .. '.lua')
    local cmd = bin .. ' "' .. f .. '"'
    local pf = io.popen(cmd)
    if not pf then return false, "cannot spawn" end
    local out = pf:read("*a")
    local ok = pf:close()
    if out and #out > 0 then
        print(out:sub(1, math.min(#out, 4000)))
    end
    -- io.popen close: true when exit code 0 (Windows/Lua convention)
    return ok, ok and "" or "subprocess exited non-zero"
end

for _, name in ipairs(tests) do
    print(string.format("Running %s.lua...", name))
    local ok, err = run_isolated(name)
    if ok then
        passed = passed + 1
        print(string.format("  [OK] %s\n", name))
    else
        failed = failed + 1
        print(string.format("  [FAIL] %s: %s\n", name, tostring(err)))
    end
end
print(string.format("Results: %d passed, %d failed, %d total",
    passed, failed, passed + failed))
if failed > 0 then os.exit(1) end
