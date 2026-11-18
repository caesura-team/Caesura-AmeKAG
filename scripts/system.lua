-- ===========================================================================
--  Caesura (AmeKAG) �� system.lua
--  System facilities: save/load, config, backlog, saveplace/loadplace.
--  Spec [4.1]: Backlog with voice + layers snapshots
--  Spec [4.2]: Save/Load with full ctx serialization
--  Spec [4.3]: Config with window + volume persistence
--  Krkrz reference: StorageIntf.h, TextStream.h, SystemIntf.h
-- ===========================================================================

local System = {}

-- Forward declarations (sandbox-safe: no globals)
local serialize_table

-- Cache sandbox-vulnerable globals before lockdown (C3 + W8)
local _os_execute = os.execute
local _io_open = io.open

local _IS_WINDOWS = package.config:sub(1,1) == "\""

-- ===========================================================================
-- escape_string(str) -> Lua-safe string literal
-- ===========================================================================

local function escape_string(s)
    return (s:gsub("\\", "\\\\")
            :gsub("\"", "\\\"")
            :gsub("\n", "\\n")
            :gsub("\r", "\\r")
            :gsub("\t", "\\t"))
end

-- ===========================================================================
-- ===========================================================================
-- ===========================================================================
-- table_deep_copy(t) -> table -- recursive deep copy (no metatables)
-- ===========================================================================
local function table_deep_copy(orig, copies)
    copies = copies or {}
    if type(orig) ~= "table" then return orig end
    if copies[orig] then return copies[orig] end
    local copy = {}
    copies[orig] = copy
    for k, v in next, orig do
        copy[table_deep_copy(k, copies)] = table_deep_copy(v, copies)
    end
    return copy
end

-- serialize_value(v, indent) -> string
-- Handles nil, boolean, number, string, table (recursive).
-- ===========================================================================

local function serialize_value(v, indent, visited)
    local t = type(v)
    if t == "nil" then
        return "nil"
    elseif t == "boolean" then
        return v and "true" or "false"
    elseif t == "number" then
        return tostring(v)
    elseif t == "string" then
        return '"' .. escape_string(v) .. '"'
    elseif t == "table" then
        return serialize_table(v, indent, visited)
    else
        return "nil --[[unsupported type: " .. t .. "]]"
    end
end

-- ===========================================================================
-- serialize_table(t, indent) -> string (Lua table constructor)
-- ===========================================================================

function serialize_table(t, indent, visited)
    if type(t) ~= "table" then
        return tostring(t)
    end
    visited = visited or {}
    if visited[t] then
        return '"<circular reference>"'
    end
    visited[t] = true
    indent = indent or ""
    local result = "{"
    local first = true
    local nextIndent = indent .. "  "

    -- Detect if array-like (consecutive integer keys starting at 1)
    local isArray = true
    local maxIdx = 0
    for k in pairs(t) do
        if type(k) ~= "number" or k < 1 or k ~= math.floor(k) then
            isArray = false
            break
        end
        if k > maxIdx then maxIdx = k end
    end
    if isArray and maxIdx == 0 then isArray = false end

    if isArray then
        for i = 1, maxIdx do
            local v = t[i]
            if v ~= nil then
                if not first then result = result .. "," end
                result = result .. "\n" .. nextIndent .. serialize_value(v, nextIndent, visited)
                first = false
            end
        end
    else
        -- Sort keys for deterministic output
        local keys = {}
        for k in pairs(t) do table.insert(keys, k) end
        table.sort(keys, function(a, b)
            if type(a) == type(b) then return tostring(a) < tostring(b) end
            return type(a) < type(b)
        end)

        for _, k in ipairs(keys) do
            local v = t[k]
            if not first then result = result .. "," end
            result = result .. "\n" .. nextIndent .. "[" .. serialize_value(k, nextIndent, visited) .. "] = " .. serialize_value(v, nextIndent, visited)
            first = false
        end
    end

    if not first then result = result .. "\n" .. indent end
    visited[t] = nil

    result = result .. "}"
    return result
end

-- ===========================================================================
-- Backlog management -- Spec [4.1]
-- ===========================================================================

--- System.push_backlog(ctx, text, voice_file) -- push entry to backlog
--- BacklogEntry: { text, voice, layers, timestamp, chapter, scene, token_index }
function System.push_backlog(ctx, text, voice_file)
    -- [R5-FIX] Delegated to canonical implementation in text commands
    local kag_text = require("kag.commands.text")
    if kag_text.push_backlog then
        return kag_text.push_backlog(ctx, "", text, voice_file)
    end
    -- Fallback if text module not available
    if not ctx then return end
    ctx.backlog = ctx.backlog or {}
    ctx.backlog_max = ctx.backlog_max or 500

    -- Capture layer snapshot if LayerTree available
    local layersSnapshot = nil
    pcall(function()
        local layers = require("layers")
        if layers and layers.capture_snapshot then
            layersSnapshot = layers.capture_snapshot(ctx.layers)
        end
    end)

    local entry = {
        text      = text or "",
        voice     = voice_file or "",
        layers    = layersSnapshot,
        timestamp = os.time(),
        chapter   = ctx.currentChapter or "",
        scene     = ctx.current_scene,
        token_index = ctx.token_index,
    }
    table.insert(ctx.backlog, entry)

    -- Trim if over max
    while #ctx.backlog > ctx.backlog_max do
        table.remove(ctx.backlog, 1)
    end
end

--- System.get_backlog_page(backlog, start_idx, count) -> BacklogEntry[]
function System.get_backlog_page(backlog, start_idx, count)
    backlog = backlog or {}
    count = count or 20
    start_idx = math.max(1, start_idx or 1)
    local result = {}
    for i = start_idx, math.min(start_idx + count - 1, #backlog) do
        table.insert(result, backlog[i])
    end
    return result
end

--- System.show_backlog(backlog) -- display backlog in UI
--- Spec [4.1] kag.history integration
function System.show_backlog(backlog)
    backlog = backlog or {}
    if #backlog == 0 then
        print("[System] Backlog is empty.")
        return
    end
    print("[System] === Backlog History ===")
    for i, entry in ipairs(backlog) do
        if type(entry) == "table" then
            local voice_info = (entry.voice and entry.voice ~= "") and (" [voice: " .. entry.voice .. "]") or ""
            print(string.format("  %d: %s%s", i, entry.text or "", voice_info))
        else
            print(string.format("  %d: %s", i, tostring(entry)))
        end
    end
    print("[System] === End Backlog ===")
end

--- System.replay_voice(index, ctx) -- replay voice from backlog entry
function System.replay_voice(index, ctx)
    if not ctx or not ctx.backlog then return false end
    local entry = ctx.backlog[index]
    if not entry then return false end

    -- Spec [4.1]: voice is stored as `voice` field
    local file = type(entry) == "table" and entry.voice or nil
    if not file or file == "" then return false end

    local Audio = require("audio")
    Audio.play_voice(file)
    return true
end

--- System.save_backlog(backlog, filepath) -- serialize backlog to file
function System.save_backlog(backlog, filepath)
    local serialized = System._serialize_backlog(backlog or {})
    local f, err = io.open(filepath, "w")
    if not f then
        print("[System] Cannot save backlog: " .. tostring(err))
        return false
    end
    f:write("return " .. serialize_table({entries = serialized}) .. "\n")
    f:close()
    return true
end

--- System.load_backlog(filepath) -- load backlog from file
function System.load_backlog(filepath)
    local f, err = io.open(filepath, "r")
    if not f then return {} end
    local code = f:read("*a")
    f:close()
    local chunk, loadErr = load(code, "backlog_load", "t", {})
    if not chunk then return {} end
    local ok, data = pcall(chunk)
    if ok and type(data) == "table" and data.entries then
        return data.entries
    end
    return {}
end

--- System.clear_backlog(ctx) -- clear all backlog entries
function System.clear_backlog(ctx)
    if ctx then ctx.backlog = {} end
end

-- Helper: serialize backlog entries preserving voice + layers fields
function System._serialize_backlog(backlog)
    local result = {}
    for i, entry in ipairs(backlog) do
        if type(entry) == "table" then
            result[i] = {
                text      = entry.text or "",
                voice     = entry.voice or "",
                timestamp = entry.timestamp or 0,
                chapter   = entry.chapter or "",
            }
        else
            result[i] = { text = tostring(entry), voice = "", timestamp = 0, chapter = "" }
        end
    end
    return result
end

-- ===========================================================================
-- Save / Load -- Spec [4.2]
-- SaveData: version, timestamp, scene, line_index, variables, sys_flags,
--           temp, layers, audio, config, backlog, call_stack, screenshot
-- ===========================================================================

--- System.capture_screenshot(ctx) -> binary PNG (C++ backend)
--- Spec [4.2]: uses bgfx requestScreenCap / readScreenCap
-- Public utility API (kept after the Path B save/load removal; usable by
-- content scripts, e.g. custom screenshot features).
function System.capture_screenshot(ctx)
    local b = rawget(_G, "_CAESURA_BACKEND")
    if b then
        return b.render("capture_screenshot")
    end
    -- Fallback stub
    print("[System] Screenshot capture requires backend support.")
    return nil
end

-- Saveplace / Loadplace -- in-memory scene bookmark
-- Spec [10.2.38]: independent temporary slot, no disk writes.
-- ===========================================================================

System._placeData = nil

--- System.saveplace(ctx) -- save scene bookmark (in-memory only)
-- Shared style-only text-state copy for the bookmark (audit nits:
-- single field list, type-checked)
function System._copyPlaceTextState(ts)
    if type(ts) ~= "table" then return {} end
    local st = {}
    for _, k in ipairs({ "font_face", "font_size", "font_color", "line", "char_offset" }) do
        if ts[k] ~= nil then st[k] = ts[k] end
    end
    return st
end

function System.saveplace(ctx)
    -- Audit fix: ctx.pc was a dead field (scheduler drives token_index) --
    -- the bookmark always saved nil and loadplace restored nothing. Use
    -- the REAL position: scene + token_index (the runner's jump path
    -- consumes both via _pendingJump).
    System._placeData = {
        label = ctx.current_label,
        scene = ctx.current_scene or ctx.currentScene or "",
        index = ctx.token_index or 1,
        tf = ctx.tf and table_deep_copy(ctx.tf) or {},
        dialog_index = ctx.dialog_index,
        -- STYLE-ONLY text state (audit: deep-copying the whole
        -- text_state dragged per-frame draws + stale texture handles
        -- into the bookmark; only the font/layout fields matter)
        text_state = System._copyPlaceTextState(ctx.text_state),
        layers = ctx.layers,  -- reference is fine
    }
    print("[System] Place saved (temporary bookmark).")
end

--- System.loadplace(ctx) -- restore scene bookmark from memory
function System.loadplace(ctx)
    if not System._placeData then
        print("[System] No place bookmark found.")
        return false
    end
    local pd = System._placeData
    ctx.current_label = pd.label
    -- Resume via the runner's jump path (scene + token index); a bare
    -- token_index write would strand the running coroutine at the old
    -- position.
    ctx._pendingJump = { scene = pd.scene, index = pd.index or 1 }
    ctx.stop_flag = true
    ctx.tf = pd.tf
    ctx.dialog_index = pd.dialog_index
    -- style-only restore: never resurrect stale per-frame draws
    -- (audit: their texture handles may belong to a previous scene)
    ctx.text_state = System._copyPlaceTextState(pd.text_state)
    if pd.text_state and pd.text_state.line then
        pcall(function()
            local layers = require("layers")
            if layers and layers.restore_text_state then
                layers.restore_text_state(ctx.text_state)
            end
        end)
    end
    print("[System] Place loaded.")
    return true
end

--- System.quick_save(ctx) -- alias for saveplace (backward compat)
function System.quick_save(ctx)
    System.saveplace(ctx)
end

--- System.quick_load(ctx) -- alias for loadplace (backward compat)
function System.quick_load(ctx)
return System.loadplace(ctx)
end

-- ===========================================================================
-- Config management -- Spec [4.3]
-- ===========================================================================

-- viewport-relative defaults (logical resolution; viewport helper is preloaded)
local _vw, _vh = require("viewport").wh()
System.defaults = {
    -- Audio
    bgm_volume    = 1.0,
    voice_volume  = 1.0,
    se_volume     = 1.0,

    -- Text
    text_speed    = 50,
    auto_speed    = 3000,
    skip_mode     = "read",
    font_size     = 24,

    -- Display
    screen_width  = _vw,   -- viewport-relative logical width  (was 1280)
    screen_height = _vh,   -- viewport-relative logical height (was 720)
    fullscreen    = false,
    window_title  = "Caesura",

    -- Feature toggles
    show_backlog  = true,
    enable_skip   = true,
    enable_auto   = true,
}

--- System.set_config(ctx, key, value) -- set a config item and notify subsystems
--- Spec [4.3]: Config:set propagates to AudioBus, SDL3, etc.
function System.set_config(ctx, key, value)
    System._config = System._config or {}
    System._config[key] = value

    -- Notify subsystems based on key
    if key == "bgm_volume" then
        pcall(function()
            local Audio = require("audio")
            if Audio and Audio.set_bus_volume then
                Audio.set_bus_volume("bgm", tonumber(value) or 1.0)
            end
        end)
        -- Also sync config.lua
        pcall(function()
            local config = require("config")
            if config then config.bgm_volume = tonumber(value) or 1.0 end
        end)
    elseif key == "voice_volume" then
        pcall(function()
            local Audio = require("audio")
            if Audio and Audio.set_bus_volume then
                Audio.set_bus_volume("voice", tonumber(value) or 1.0)
            end
        end)
        pcall(function()
            local config = require("config")
            if config then config.voice_volume = tonumber(value) or 1.0 end
        end)
    elseif key == "se_volume" then
        pcall(function()
            local Audio = require("audio")
            if Audio and Audio.set_bus_volume then
                Audio.set_bus_volume("se", tonumber(value) or 1.0)
            end
        end)
        pcall(function()
            local config = require("config")
            if config then config.se_volume = tonumber(value) or 1.0 end
        end)
    elseif key == "screen_width" or key == "screen_height" then
        local vw, vh = require("viewport").wh()  -- viewport-relative fallback (was 1280/720)
        local w = System._config.screen_width or vw
        local h = System._config.screen_height or vh
        pcall(function()
            local backend = require("backend")
            if backend and backend.set_resolution then
                backend.set_resolution(w, h)
            end
        end)
        pcall(function()
            local config = require("config")
            if config then
                config.window_width = w
                config.window_height = h
            end
        end)
    elseif key == "fullscreen" then
        pcall(function()
            local backend = require("backend")
            if backend and backend.set_fullscreen then
                backend.set_fullscreen(value == true or value == "true")
            end
        end)
    end

    -- Auto-persist
    System.save_config(System._config, "settings/config.lua")
end

--- System.get_config(key) -> value
--- Spec [4.3]: Config:get with defaults fallback
function System.get_config(key)
    System._config = System._config or {}
    local val = System._config[key]
    if val ~= nil then return val end
return System.defaults[key]
end

--- System.load_config(filepath) -> config table
function System.load_config(filepath)
    local f, err = io.open(filepath, "r")
    if not f then return nil end
    local code = f:read("*a")
    f:close()
    local chunk, loadErr = load(code, "config_load", "t", {})
    if not chunk then return nil end
    local ok, data = pcall(chunk)
    if ok and type(data) == "table" then
        System._config = data
        return data
    end
    return nil
end

--- System.save_config(config, filepath) -> boolean
function System.save_config(config, filepath)
    -- Ensure directory
    local dir = filepath:match("^(.-)[^/\\]+$") or ""
    if dir ~= "" and _IS_WINDOWS then
        _os_execute('if not exist "' .. dir:gsub("/$", ""):gsub("\\$", "") .. '" mkdir "' .. dir:gsub("/$", ""):gsub("\\$", "") .. '"')
    elseif dir ~= "" then
        _os_execute('mkdir -p "' .. dir .. '"')
    end

    local f, err = io.open(filepath, "w")
    if not f then
        print("[System] Cannot save config: " .. tostring(err))
        return false
    end
    f:write("return " .. serialize_table(config) .. "\n")
    f:close()
    return true
end

--- System.reset_config() -> defaults table
function System.reset_config()
    System._config = {}
    for k, v in pairs(System.defaults) do
        System._config[k] = v
    end
    System.save_config(System._config, "settings/config.lua")
return System._config
end

-- ===========================================================================
-- Init: load persisted config on first require
-- ===========================================================================
System._config = System.load_config("settings/config.lua") or {}
if next(System._config) == nil then
    System._config = {}
    for k, v in pairs(System.defaults) do
        System._config[k] = v
    end
end


-- ===========================================================================
-- Phase G8-U1: Explicit GC hooks
-- ===========================================================================

--- System.collect_step(size) ? incremental GC step (size in KB)
function System.collect_step(size)
    collectgarbage("step", size or 10)
end

--- System.collect_full() ? full GC collect + audio cache cleanup
function System.collect_full()
    collectgarbage("collect")
    -- Clear audio cache if Audio module available
    pcall(function()
        local Audio = require("audio")
        if Audio and Audio.clear_cache then
            Audio.clear_cache()
        end
    end)
end


-- ===========================================================================
-- Quick-save / Quick-load / Auto-save (SU-3)
-- ===========================================================================
-- Called from C++ Engine when F5/F6 pressed or auto-save timer fires.
-- These call into the KAG save command flow via KAG.save_game/KAG.load_game.

local QUICKSAVE_SLOT = -1
local AUTOSAVE_SLOT  = -2
System.AUTOSAVE_SLOT = AUTOSAVE_SLOT

--- quicksave() — F5 hotkey handler
function quicksave()
    local ctx = rawget(_G, "_CAESURA_CTX")
    if not ctx then
        print("[Quicksave] No KAG context available")
        return
    end
    local SaveCommands = require("kag.commands.save")
    local ok = SaveCommands.save(ctx, { slot = QUICKSAVE_SLOT, desc = "Quick Save" })
    require("toast").show(ok and "已保存" or "保存失败", 1.5)
end

--- quickload() — F6 hotkey handler
function quickload()
    local ctx = rawget(_G, "_CAESURA_CTX")
    if not ctx then
        print("[Quickload] No KAG context available")
        return
    end
    local SaveCommands = require("kag.commands.save")
    local ok = SaveCommands.load(ctx, { slot = QUICKSAVE_SLOT })
    require("toast").show(ok ~= false and "已读档" or "读档失败", 1.5)
end

--- autosave() — auto-save timer handler (called from C++ Engine)
function autosave()
    local ctx = rawget(_G, "_CAESURA_CTX")
    if not ctx then return end
    local SaveCommands = require("kag.commands.save")
    SaveCommands.save(ctx, { slot = AUTOSAVE_SLOT, desc = "Auto Save" })
end

--- System.setAutoSaveInterval(seconds) — configure auto-save frequency
function System.setAutoSaveInterval(seconds)
    -- Set via C++ Engine (exposed as global)
    pcall(function()
        local engine = rawget(_G, "_CAESURA_ENGINE")
        if engine and engine.setAutoSaveInterval then
            engine:setAutoSaveInterval(tonumber(seconds) or 0)
        end
    end)
end

-- Wire quicksave/quickload/autosave globals for C++ to call
rawset(_G, "quicksave", quicksave)
rawset(_G, "quickload", quickload)
rawset(_G, "autosave", autosave)

return System
