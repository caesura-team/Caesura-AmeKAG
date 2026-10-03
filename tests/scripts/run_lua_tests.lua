-- =============================================================================
--  run_lua_tests.lua — Standalone Lua test runner
--  Usage: lua tests/scripts/run_lua_tests.lua
-- =============================================================================

-- Add scripts/ to package.path
local test_dir = arg and arg[0] and arg[0]:match("(.*[/\\])") or ""
package.path = test_dir .. "../../scripts/?.lua;" .. test_dir .. "?.lua;" .. package.path

-- Save globals that sandbox may replace
local _real_os_exit = os.exit
local _real_dofile  = dofile

-- Preload modules the sandbox-locked tests need (mirrors scripts/kag/init.lua:
-- sandbox.lua replaces _G.require with a package.loaded-only wrapper, so any
-- module first required AFTER test_sandbox runs must already be cached).
-- Preload ONLY dependency-free modules the sandbox-locked tests need
-- (replay). Do NOT add kag_runner/scheduler here: requiring them pulls
-- the whole command module chain before the tests that mock the C
-- bindings, changing suite behavior (see git history).
local okp, errp = pcall(require, "replay")
local okpal, errpal = pcall(require, "palette")
local okvp, errvp = pcall(require, "viewport")
if not okvp then print("[run_lua_tests] preload viewport failed: " .. tostring(errvp)) end
if not okpal then print("[run_lua_tests] preload palette failed: " .. tostring(errpal)) end
if not okp then print("[run_lua_tests] preload replay failed: " .. tostring(errp)) end

-- NOTE: test_kag_commands must run BEFORE test_scheduler because
-- scheduler internally loads kag module which caches a partial table.
local tests = {
    "test_tokenizer",
    "test_kag_commands",
    "test_scheduler",
    "test_compiler",
    "test_bytecode_cache",
    "test_carc_import",
    "test_ks_bake",
    "test_lsp",
    "test_kag_semantic",
    "test_live2d_cmds",
    "test_schema_types",
    "test_determinism",
    "test_fuzz",
    "test_aiwriter",
    "test_aidev",
    "test_lua_bracket",
    "test_tutorial_scene",
    "test_example_game",
    "test_rollback_memory",
    "test_rollback",
    "test_backlog",
    "test_kag3_compat",
    "test_choice",
    "test_history_cleanup",
    "test_input_session_cleanup",
    "test_command_parameter_aliases",
    "test_command_dispatch_boundary",
    "test_command_alias_precedence",
    "test_vfx_parameter_aliases",
    "test_input_appearance",
    "test_text_markup",
    "test_i18n",
    "test_i18n_cmd",
    "test_ks_i18n_flow",
    "test_sma",
    "test_sma_check",
    "test_sma_demo",
    "test_title_menu",
    "test_title_entry",
    "test_benchmark",
    "test_schema",
    "test_schema_coerce",
    "test_kag3_import",
    "test_layers",
    -- Per-frame allocation budget for the pooled render batch: the command
    -- tables are recycled, so a steady scene must not grow the Lua heap.
    "test_layers_alloc",
    "test_mods",
    "test_replay",
    "test_scene_reload",
    "test_ai",
    "test_color_filter",
    "test_sandbox",
    -- Escape regressions for the hardened sandbox: rawset/debug.getmetatable
    -- routes are gone and load() is host-only, so these must stay red-on-regress.
    "test_sandbox_escape",
    "test_label_index",
    "test_expr_cache",
    "test_expr_lang",
    "test_expr_lang2",
    "test_variables",
    "test_control_flow",
    "test_flow_edge",
    "test_flow_edge_call",
    "test_flow_edge_scene",
    "test_scale_stress",
    "test_macro_scene",
    "test_scene_switch_budget",
    "test_modern_commands",
    "test_kag_debug",
    "test_multiline",
    "test_bg_dedup",
    "test_macro_nested",
    "test_macro_deep2",
    "test_scene_restore",
    "test_elseif",
    "test_elsif_alias",
    "test_if_nested",
    "test_switch",
    "test_switch_exotic",
    "test_switch_scan",
    "test_switch_taken_nested",
    "test_switch_deep2",
    "test_while",
    "test_for",
    "test_loop_control",
    "test_effect_aliases",
    "test_waitclick",
    "test_iscript_sandbox",
    "test_eval_env",
    "test_sandbox_create",
    "test_select",
    "test_label_jump",
    "test_end_title",
    "test_history",
    "test_unlock",
    "test_saveplace",
    "test_story_meta",
    "test_saveload",
    "test_golden_save_migration",
    "test_audio_cmds",
    "test_textflow",
    "test_vib_camera",
    "test_transitions",
    "test_layer_cmds",
    "test_layopt",
    "test_text_deco",
    "test_reset_pt",
    "test_emb_sync",
    "test_operation",
    "test_fade_audio",
    "test_volume",
    "test_ch_state",
    "test_playbgmstop",
    "test_layout",
    "test_resolve",
    "test_sprite_family",
    "test_toast",
    "test_textbox",
    "test_nvl",
    "test_vfx_clamp",
    "test_delay",
    "test_wait_audio",
    "test_video",
    "test_font",
    "test_jump_path",
    "test_bareval",
    "test_alias_bare",
    "test_unlock_bare",
    "test_macro_bare",
    "test_save_slot",
    "test_gallery_bare",
    "test_offsets",
    "test_scene_preload",
    "test_ks_check",
    "test_four_remaining",
    "test_scroll",
    "test_reveal",
    "test_audio_fade",
    "test_trans_behavior",
    "test_trans_scheduler_visual",
    "test_fadeout",
    "test_layfade_contract",
    "test_backend_guard",
    "test_accessibility",
    "test_label_bench",
    "test_chapter_select",
    "test_music_room",
    "test_gallery_loop",
    "test_settings",
    "test_frame_bench",
    "test_input_cmd",
    "test_postprocess",
    "test_particle_weather",
    "test_layered_vfs",
    "test_steam_achievements",
    "test_quickmenu",
}

local passed, failed = 0, 0
print("\n=== Caesura Lua Test Suite ===\n")

for _, name in ipairs(tests) do
    print(string.format("Running %s.lua...", name))
    local ok, err = pcall(function() _real_dofile(test_dir .. name .. ".lua") end)
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
if failed > 0 then _real_os_exit(1) end
