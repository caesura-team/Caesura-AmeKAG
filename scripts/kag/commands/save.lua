-- =============================================================================
--  Caesura (AmeKAG) — kag/commands/save.lua
--  Phase 6: KAG save/load tag handlers — [save], [load]
--  Serializes KAG context to JSON → C++ SaveManager → disk.
--  All I/O goes through KAG.save_game / KAG.load_game (C++ bindings).
-- =============================================================================

-- Cache sandbox-vulnerable globals before lockdown
local _type    = type

local System = require("system")
local SaveState = require("kag.save_state")
local Presentation = require("kag.presentation") -- preload before sandbox lockdown
local Transients = require("kag.transient_state")
local WaitState = require("kag.wait_state")
local Operation = require("kag.operation")

local SaveCommands = {}

-- Scene-path allowlist (pure, unit-tested). Real layout: production entry
-- is scripts/demo_story.ks and scheduler jumps write assets/script/<t>.ks
-- (singular); assets/scripts and demo/ are also accepted. Any ".." segment
-- rejects so traversal cannot smuggle an arbitrary file past the prefix
-- check. Non-string/empty -> false.
-- C++ bindings live on the global KAG table (set by KAGBinding.cpp);
-- direct-API contexts (tests, editor) have none -- resolve like
-- backend.lua does (audit fix).
local function kag_binding(name)
    local g = rawget(_G, "KAG")
    local v = g and g[name]
    if type(v) == "function" then return v end
    return nil
end

function SaveCommands._safeScenePath(sp)
    if _type(sp) ~= "string" or #sp == 0 then return false end
    if sp:find("..", 1, true) then return false end
    if sp:find("^scripts/") == 1 or sp:find("^assets/script/") == 1
        or sp:find("^assets/scripts/") == 1 or sp:find("^demo/") == 1
        or sp:find("^tests/scripts/") == 1
        or sp:find("^tests/projects/") == 1
        or sp:find("^projects/") == 1 then
        return sp:find("%.ks$") ~= nil
    end
    return false
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Internal: serialize KAG context values to a flat Lua table
--  Captures: f (global flags), sf (system flags), token_index, scene_path
--  Does NOT capture: tf (temp flags), co
-- ═══════════════════════════════════════════════════════════════════════════

local function capture_state(ctx)
    Transients.assert_saveable(ctx)
    local state = {}

    -- f-variables (global flags)
    state.f = ctx.f or {}

    -- sf-variables (system flags)
    state.sf = {}
    if ctx.sf then
        for k, v in pairs(ctx.sf) do
            if k ~= "save_list" then
                state.sf[k] = v
            end
        end
    end

    -- Token position
    state.token_index = ctx._executing_index or ctx._resume_index or ctx.token_index or 1
    state.display_token_index = ctx.token_index or state.token_index
    if ctx._executing_command == "save" or ctx._executing_command == "saveload" then
        state.token_index = state.token_index + 1
    end

    -- Scene path: scheduler writes current_scene (snake_case) on
    -- jump/call/link; currentScene is the legacy camelCase alias.
    state.scene_path = ctx.current_scene or ctx.currentScene or ""

    -- Backlog (recent N entries)
    state.backlog = {}
    if ctx.backlog then
        for i = math.max(1, #ctx.backlog - 99), #ctx.backlog do
            local entry = ctx.backlog[i]
            state.backlog[#state.backlog + 1] = {
                name        = entry.name or "",
                text        = entry.text or "",
                voice       = entry.voice or "",
                timestamp   = entry.timestamp or 0,
                scene       = entry.scene or "",
                token_index = entry.token_index or 1,
                -- Pre-localize source: lets the language hot-switch
                -- redraw re-localize a restored backlog too. Absent in
                -- older saves -- those entries keep their stored text.
                src         = entry.src,
            }
        end
    end

    -- Label map (for jump targets)
    state.label_map = {}
    if ctx.labelMap then
        for k, v in pairs(ctx.labelMap) do
            if _type(k) == "string" and _type(v) == "number" then
                state.label_map[k] = v
            end
        end
    end

    -- Save description
    state.description = ctx.saveDescription or ""

    -- Unlock state (gallery CGs + music room tracks)
    state.unlockedCG = {}
    if ctx.unlockedCG then
        for k, v in pairs(ctx.unlockedCG) do
            state.unlockedCG[k] = v
        end
    end
    state.unlockedMusic = {}
    if ctx.unlockedMusic then
        for k, v in pairs(ctx.unlockedMusic) do
            state.unlockedMusic[k] = v
        end
    end

    -- Call stack (for [call]/[return] nested execution)
    state.call_stack = {}
    if ctx.call_stack then
        for _, frame in ipairs(ctx.call_stack) do
            table.insert(state.call_stack, {
                scene = frame.scene or ctx.current_scene or ctx.currentScene,
                index  = frame.index or 1,
                lf = frame.lf or {}, mp = frame.mp or {},
                control = frame.control,
            })
        end
    end

    -- Live loop/branch stacks (round 75): scheduler.run hoisted them to
    -- ctx so a save taken INSIDE a [for]/[while] body (or an [if]/[case]
    -- chain) can restore them on [load] -- otherwise the re-spawned run
    -- starts with empty stacks and the enclosing loop silently ends
    -- (round-74 defect). Entries are plain JSON-able values (var/endv/
    -- step/pos/ended for for; pos/ended for while; booleans for if and
    -- switch). Deep-copy defensively; missing/non-table = no restore.
    local function copyStack(t)
        if type(t) ~= "table" or #t == 0 then return nil end
        local out = {}
        for i2, e in ipairs(t) do out[i2] = e end
        return out
    end
    state.loop_stacks = {
        for_ = copyStack(ctx._forStack),
        while_ = copyStack(ctx._whileStack),
        if_ = copyStack(ctx._ifStack),
        switch = copyStack(ctx._switchStack),
    }

    -- Schema version (engine-defined)
    state.schema_version = 2  -- bumped: added unlock state

    -- [R6-FIX] Persist skip/auto mode so they survive save/load cycles
    state.skip_mode = ctx.skip_mode or false
    state.auto_mode = ctx.auto_mode or false
    -- [nvl] mode persists with the save (a reloaded save must resume the
    -- same full-screen text mode, not fall back to the message window).
    state.nvl_mode = ctx.nvl_mode or false
    -- [voice_off] muting persists with the save (audit: it was the only
    -- playback setting NOT captured -- reload reset the mute).
    state.voice_muted = ctx.voice_muted or false
    -- Persist the UI language (settings hot-switch) so a reloaded save
    -- restores the player's locale instead of resetting to zh.
    state.language = (ctx.settingsValues and ctx.settingsValues.language)
        or require("i18n").current or "zh"
    state.language_default = require("i18n").default_language or "en"

    -- [R7-FIX] Persist seen flags for Read Skip
    state.seen_scenes = ctx.seen_scenes or {}
    state.seen_endings = ctx.seen_endings or {}

    -- Visual/text state (audit: saves restored logic but not the
    -- message-window style or font -- a reloaded save reset the look).
    -- Only plain string/number fields are captured (no handles/tables).
    if ctx.text_state and _type(ctx.text_state) == "table" then
        local ts = ctx.text_state
        local vis = {}
        if _type(ts.font_face) == "string" then vis.font_face = ts.font_face end
        if _type(ts.font_size) == "number" then vis.font_size = ts.font_size end
        if _type(ts.font_color) == "string" then vis.font_color = ts.font_color end
        if next(vis) then state.text_state = vis end
    end
    if ctx.textbox_style and _type(ctx.textbox_style) == "table" then
        local st = ctx.textbox_style
        if _type(st.w) == "number" and _type(st.h) == "number" then
            state.textbox_style = {
                x = st.x or 0, y = st.y or 0, w = st.w, h = st.h,
                color = _type(st.color) == "string" and st.color or "0,0,0",
                opacity = st.opacity or 200,
                visible = st.visible ~= false,
            }
        end
    end

    state.schema_version = SaveState.VERSION
    state.lf, state.mp = ctx.lf or {}, ctx.mp or {}
    state.variables = ctx.variables or {}
    state.text_snapshot = require("kag.text_scene").capture(ctx)
    state.wait_snapshot = WaitState.capture(ctx)
    local presentation = Presentation.capture()
    state.layer_snapshot, state.audio_snapshot = presentation.layers, presentation.audio
    state.font_snapshot = presentation.font
    state.layers, state.characters = ctx.layers or {}, ctx.characters or {}
    state.loop_stacks = SaveState.capture_control(ctx)
    state.seen_scenes = SaveState.capture_seen(ctx.seen_scenes)
    state.sf.save_list = nil
    return SaveState.copy(state)
end

-- ═══════════════════════════════════════════════════════════════════════════
-- Neo-Genesis contracts: slot typed + bounded 0..99 (matches the C++
-- SaveManager guard; out-of-range values clamp to the bound).
require("kag.schema").define("save", {
    _meta = { category = "save", blocking = true, desc = "KAG3-compatible save command" },
    -- NO default: coerce would inject slot=0 and shadow the bare
    -- positional [save 3] (same pattern as the wait aliases).
    -- min = -2: named negative slots (system -1/-2) must flow through
    -- to the C++ guard, not clamp to 0 (review should-fix).
    slot = { type = "number", min = -2, max = 99 },
})
require("kag.schema").define("load", {
    _meta = { category = "save", blocking = true, desc = "KAG3-compatible load command" },
    -- NO default: coerce would inject slot=0 and shadow the bare
    -- positional [save 3] (same pattern as the wait aliases).
    -- min = -2: named negative slots (system -1/-2) must flow through
    -- to the C++ guard, not clamp to 0 (review should-fix).
    slot = { type = "number", min = -2, max = 99 },
})

-- Resolve the save slot: named slot= (schema-typed 0..99), else the
-- KAG3 bare positional [save 1], clamped here too (the numeric key
-- bypasses schema coerce -- same pattern as the wait clamp).
SaveCommands.capture_state = capture_state

local function resolve_slot(params)
    -- tonumber(params.slot): a direct caller could pass a string
    -- (review nit -- coerce + all callers pass numbers today)
    local slot = tonumber(params.slot) or tonumber(params[1]) or 0
    -- NEGATIVE slots are SYSTEM slots (quicksave=-1, autosave=-2):
    -- do NOT clamp them to 0 -- that would make F5/autosave silently
    -- overwrite the player's manual slot 0 (security review warn).
    -- They pass through; the C++ SaveManager 0..99 guard rejects them,
    -- so system slots stay inert until a dedicated mapping lands.
    if slot < 0 then return slot end
    if slot > 99 then return 99 end
    return math.floor(slot)
end

function SaveCommands.save(ctx, params)
    local slot = resolve_slot(params)
    local desc = params.desc or params.description or ""
    local thumbnail = params.thumbnail or ""
    local runner = package.loaded["kag_runner"]
    local runner_owned = ctx._native_runner_owner or (runner and runner.get_ctx() == ctx)
    local owner_co = ctx.co
    local retained_owner = runner_owned and runner
        and type(runner.can_save_retained_context) == "function"
        and runner.can_save_retained_context(ctx)
    local function owner_active()
        if runner_owned and (package.loaded["kag_runner"] ~= runner
            or not runner or runner.get_ctx() ~= ctx or ctx.co ~= owner_co) then return false end
        if retained_owner then return runner.can_save_retained_context(ctx) end
        return not ctx.stop_flag and ctx._session_active ~= false
    end
    local function rejected(reason)
        ctx.tf = ctx.tf or {}
        ctx.tf.save_result, ctx.tf.save_error = "error", reason
        return false, reason
    end
    if not owner_active() then return rejected("save-owner-expired") end
    ctx._pending_save_slots = ctx._pending_save_slots or {}
    if ctx._pending_save_slots[slot] then return rejected("save-busy") end

    -- Other pending saves own readback leases, not unrestorable scene effects.
    -- Keep all unrelated operation tokens in the existing transient guard.
    local capture_ctx, save_tokens = {}, {}
    for key, value in pairs(ctx) do capture_ctx[key] = value end
    for _, pending in pairs(ctx._pending_save_slots) do save_tokens[pending.token] = true end
    capture_ctx.active_operations = {}
    for _, token in ipairs(ctx.active_operations or {}) do
        if not save_tokens[token] then
            capture_ctx.active_operations[#capture_ctx.active_operations+1] = token
        end
    end
    ctx.saveDescription, capture_ctx.saveDescription = desc, desc
    local captured, state = pcall(capture_state, capture_ctx)
    if not captured then return rejected(tostring(state)) end
    local sceneName = state.scene_path ~= "" and state.scene_path or "unknown"
    local tokenIdx = state.token_index

    local operation_owner = ctx
    if retained_owner then
        -- This is a new snapshot request, not resumed scene execution. Share
        -- the owner's cancellation list so stop/replacement still cancels it,
        -- without reactivating the completed scheduler or its old operations.
        ctx.active_operations = ctx.active_operations or {}
        operation_owner = {active_operations=ctx.active_operations}
    end
    local operation <close> = Operation.start(operation_owner)
    local function release_slot()
        if ctx._pending_save_slots[slot] == operation then
            ctx._pending_save_slots[slot] = nil
        end
    end
    operation.token:register(release_slot)
    ctx._pending_save_slots[slot] = operation

    -- Every persistent argument is frozen before capture may suspend this owner.
    local thumbnail_result, thumbnail_error = "provided", nil
    if #thumbnail == 0 then
        local capture = ctx.captureThumbnail or kag_binding("capture_thumbnail")
        thumbnail_result = "unavailable"
        thumbnail_error = "capture-unavailable"
        if type(capture) == "function" then
            -- Lua pcall forwards native continuation yields. A Cancelled result
            -- is never interchangeable with the ordinary optional-image fallback.
            local okT, thumb, status, reason = pcall(capture, operation.token)
            if not okT then
                thumbnail_result, thumbnail_error = "failed", tostring(thumb)
            else
                thumbnail_result = status or (type(thumb)=="string" and #thumb>0
                    and "completed" or "unavailable")
                thumbnail_error = reason
                if thumbnail_result == "completed" and type(thumb)=="string" then
                    thumbnail = thumb
                end
            end
        end
    end
    if operation.token.cancelled or not owner_active() or thumbnail_result == "cancelled" then
        return rejected("save-cancelled")
    end
    ctx.tf = ctx.tf or {}
    ctx.tf.thumbnail_result, ctx.tf.thumbnail_error = thumbnail_result, thumbnail_error
    local binding = kag_binding("save_game")
    if not binding then return rejected("save-backend-unavailable") end
    -- One synchronous U7 atomic/encrypted commit, with this operation's image.
    local ok, reason = binding(slot, state, sceneName, tokenIdx, thumbnail)
    release_slot()
    operation:complete()
    pcall(function() collectgarbage("collect") end)
    if ok then
        print("[SaveCmd] Saved to slot " .. slot .. " (" .. sceneName .. ")")
        ctx.tf.save_result, ctx.tf.save_error, ctx.tf.save_slot = "ok", nil, slot
        return true
    end
    print("[SaveCmd] Save failed for slot " .. slot)
    return rejected(reason or "save-failed")
end

-- ═══════════════════════════════════════════════════════════════════════════
--  [load slot=0]
--  Load save data → JSON decode → restore ctx state → resume scene
--  After restoring variables, re-executes scene script from the saved position.
-- ═══════════════════════════════════════════════════════════════════════════

-- Prepare a slot without touching any live context, coroutine or presentation.
-- The caller owns the returned candidate until it commits or discards it.
function SaveCommands.prepare_load(params)
    if params ~= nil and type(params) ~= "table" then return nil, "Invalid load parameters" end
    local slot = resolve_slot(params or {})
    local binding = kag_binding("load_game")
    if not binding then return nil, "save backend unavailable" end
    local read, state, meta = pcall(binding, slot)
    if not read or type(state) ~= "table" then
        return nil, read and (meta or "save unavailable") or state
    end
    local provider = require("flow")
    local safe_scene = type(provider.is_restore_scene) == "function"
        and provider.is_restore_scene or SaveCommands._safeScenePath
    local prepared, candidate = pcall(SaveState.prepare, state, safe_scene, provider.prepare_scene)
    if not prepared then return nil, candidate end
    candidate.tf.load_result, candidate.tf.load_slot = "ok", slot
    candidate.tf.load_meta = meta
    return candidate
end

function SaveCommands.load(ctx, params)
    local function rejected(reason)
        ctx.tf = ctx.tf or {}
        ctx.tf.load_result, ctx.tf.load_error = "error", tostring(reason)
        return false, reason
    end
    local runner = package.loaded["kag_runner"]
    local runner_owned = ctx._native_runner_owner or (runner and runner.get_ctx() == ctx)
    local owner_co = ctx.co
    if runner_owned and (not runner or runner.get_ctx() ~= ctx) then
        return rejected("restore-owner-expired")
    end
    local candidate, prepare_error = SaveCommands.prepare_load(params)
    if not candidate then return rejected(prepare_error) end
    -- Asset preparation may yield. A retired runner owner must never fall
    -- through into the direct-host path when the awaited work completes.
    if package.loaded["kag_runner"] ~= runner or (runner_owned
        and (runner.get_ctx() ~= ctx or ctx.co ~= owner_co)) then
        local discarded, discard_error = Presentation.discard(candidate._presentation)
        return rejected("restore-owner-expired" .. (discarded and "" or "; discard: " .. tostring(discard_error)))
    end
    if runner_owned then
        local restored, reason = runner.restore_candidate(ctx, candidate)
        if not restored then
            Presentation.discard(candidate._presentation)
            return rejected(reason)
        end
        return true, reason
    end
    -- Hosts that drive scheduler.run directly consume the existing scene
    -- continuation fields. They still receive a fully prepared value set.
    local applied, apply_error = pcall(function()
    SaveState.apply_values(ctx, candidate)
    Presentation.apply(candidate._presentation, ctx)
    if candidate.language then
        ctx.settingsValues = ctx.settingsValues or {}
        ctx.settingsValues.language = candidate.language
        require("i18n").commit(candidate._locale)
    end
    ctx._pendingLoadScene = candidate.current_scene
    ctx._pendingLoadToken = candidate._resume_index
    ctx._preparedRestore = candidate
    ctx.stop_flag = true
    end)
    if not applied then
        ctx.stop_flag, ctx._session_active, ctx.waiting_input = true, false, false
        ctx.text_state = {draws={}}
        Presentation.stop(ctx, true)
        return rejected(apply_error)
    end
    return true
end

-- ═══════════════════════════════════════════════════════════════════════════
--  [listsaves] — populate ctx.sf.save_list with available saves
--  Used by save/load UI to display slots.
-- ═══════════════════════════════════════════════════════════════════════════

-- [saveload mode=save|load] — slot-selection UI (scheduler-driven)
-- Round 51 contract: [saveload] (audit: handler lacked a schema).
require("kag.schema").define("saveload", {
    _meta = { category = "save", blocking = true, desc = "open the save/load menu (mode: save|load)" },
    mode = { type = "string" },
})

function SaveCommands.saveload(ctx, params)
    -- Round 52 audit: headless/editor environments have no UI overlay —
    -- saveload_menu may not be loadable at all, so pcall the require.
    local okL, SaveLoad = pcall(require, "saveload_menu")
    if not okL then SaveLoad = nil end
    -- string guard on the bare mode (audit: a pair table from named
    -- params must not reach the menu as the mode)
    local mode = params.mode
    if type(mode) ~= "string" then
        mode = (type(params[1]) == "string") and params[1] or nil
    end
    -- Round 52 audit: headless/editor environments have no UI overlay —
    -- require("saveload_menu") may be absent or lack .show.
    if type(SaveLoad) ~= "table" or type(SaveLoad.show) ~= "function" then
        print("[SaveCmd] saveload: menu unavailable (headless) — skipping")
        return
    end
    local chosen = SaveLoad.show(ctx, mode or "save")
    if chosen then
        if chosen.action == "save" then
            SaveCommands.save(ctx, { slot = chosen.slot })
        else
            SaveCommands.load(ctx, { slot = chosen.slot })
        end
    end
    return chosen
end

function SaveCommands.listsaves(ctx, params)
    -- Round 52 audit: headless/web environments may lack the list_saves
    -- binding (no C++ SaveManager) — degrade to an empty list instead of
    -- crashing on a nil call.
    local fn = kag_binding("list_saves")
    if type(fn) ~= "function" then
        print("[SaveCmd] listsaves: binding unavailable (headless) — empty list")
        ctx.sf = ctx.sf or {}
        ctx.sf.save_list = {}
        ctx.tf = ctx.tf or {}
        ctx.tf.save_list = {}
        return
    end
    local saves = fn()
    ctx.sf = ctx.sf or {}
    ctx.sf.save_list = saves
    -- Also set as tf for immediate access
    ctx.tf = ctx.tf or {}
    ctx.tf.save_list = saves
end

-- ═══════════════════════════════════════════════════════════════════════════
--  [saveplace] / [loadplace] — in-memory scene bookmarks
--  Independent of save slots.  No disk writes.
--  Delegates to System.saveplace / System.loadplace (scripts/system.lua).
-- ═══════════════════════════════════════════════════════════════════════════

function SaveCommands.saveplace(ctx, params)
    System.saveplace(ctx)
end

function SaveCommands.loadplace(ctx, params)
    System.loadplace(ctx)
end

return SaveCommands
