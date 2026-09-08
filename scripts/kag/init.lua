-- ===========================================================================
--  Caesura (AmeKAG) — kag/init.lua
--  KAG module entry point. Loads all core Lua libraries.
--  Spec: Modules are loaded in dependency order; tokenizer + scheduler
--  supersede the legacy parser.lua + conductor.lua.
-- ===========================================================================

-- Core script engine
local kag        = require("kag")
local tokenizer  = require("tokenizer")
local scheduler  = require("scheduler")
local kag_debug  = require("kag_debug")   -- KAG scene debugger (preload for sandbox)
local kag_runner = require("kag_runner")  -- KAG coroutine bridge (preload for sandbox)
local lsp        = require("kag.lsp")     -- language service (Battle 2; preload for sandbox)
local semantic   = require("kag.semantic") -- unified semantic model & AST (preload for sandbox)
local aiwriter   = require("kag.aiwriter") -- AI scene writing (Battle 4c; preload)
local aidev      = require("kag.aidev")   -- AI dev assistant (Battle 4e; preload for sandbox)
local sma        = require("kag.sma")     -- skeletal mesh animation (Battle 4d S3; preload for sandbox)
local sma_check  = require("kag.sma_check") -- SMA asset validator (Battle 4d; preload for sandbox/RPC)
local mods       = require("mods")        -- mod loader (preload for sandbox)
local replay     = require("replay")      -- input recording/playback (preload for sandbox)
local layers     = require("layers")      -- layer tree (preload for sandbox; color filter submit)
local palette    = require("palette")     -- color LUT palette (preload for sandbox; [palette] cmd)
local flow       = require("flow")
local quickmenu  = require("kag.quickmenu") -- QuickMenu bindings (auto/skip/log/qsave/qload/title/config)
local toast      = require("toast")       -- corner notifications; QuickMenu shows "Auto ON"/"Saved (slot n)"
                                          -- through it, and the sandbox resolves require() from
                                          -- package.loaded only, so it must be preloaded here or the
                                          -- feedback stays permanently silent on device.

-- Graphics
local layers     = require("layers")
local viewport   = require("viewport")  -- logical-resolution layout helpers (1920x1080 default)
local rtt        = require("rtt")
local blend      = require("blend")
local transition = require("transition")
local transform  = require("transform")
local vfx        = require("vfx")

-- Audio
local audio      = require("audio")

-- System
local system     = require("system")
local config     = require("config")
local backend    = require("backend")

-- Save/Load (Phase 6)
local save_cmds  = require("kag.commands.save")
-- Runtime-required kag submodules must be preloaded for the sandboxed
-- require wrapper (package.searchers disabled after lockdown): any module
-- first required inside a frame/click callback (kag_runner.on_click ->
-- kag.snapshot) would otherwise hard-error "not preloaded".
local snapshot    = require("kag.snapshot")   -- rollback snapshots (on_click)
local rollback_presentation = require("kag.rollback_presentation") -- prepared rollback resources
local text_scene  = require("kag.text_scene") -- dialogue draw list (render)

-- Declarative tween commands (round 106): preloaded so sandbox require
-- resolves before any [tween] token runs (kag_runner hooks call it).
local tween_cmds = require("kag.commands.tween")
-- Declarative layout containers (round 107): settings.lua consumes
-- kag.layout_math directly; preload both for sandbox require.
local layout_cmds = require("kag.commands.layout")
local layout_math = require("kag.layout_math")

-- P1 extensions
local gallery    = require("gallery")
local music_room = require("music_room")
local pool       = require("pool")

local i18n       = require("i18n")
local settings   = require("settings")

-- Legacy (backward compat) — removed from auto-load
-- [R3-FIX] Legacy parser/conductor removed from auto-load. Use tokenizer + scheduler directly.
-- These modules remain available for explicit require() if old scripts need them.
-- local parser     = require("kag.parser")
-- local conductor  = require("kag.conductor")

-- Register global _T shortcut for i18n
_G._T = i18n.t

print("[kag/init] All KAG libraries loaded.")
