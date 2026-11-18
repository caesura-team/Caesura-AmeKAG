-- =============================================================================
--  Caesura (AmeKAG) — kag_demo_entry.lua
--  KAG demo entry point. Loads and runs a .ks script via the KAG runner,
--  wiring the coroutine bridge into the engine's per-frame callbacks.
--
--  To activate: change config.entry_script = "kag_demo_entry.lua" in config.lua
--  No C++ rebuild needed — this is pure Lua, loaded via luaL_dofile.
-- =============================================================================

-- [[ P0-1: self-locating scripts path. A bare `lua <this file>` has no
-- scripts/ entry in package.path, so resolve the engine root from this
-- file's location and prepend it. Prepend-only & idempotent; a no-op when
-- the engine (which configures the path itself) or a headless driver
-- already supplies the scripts path. ]]
pcall(function()
    local root, dir = nil, nil
    local a0 = arg and arg[0]
    if type(a0) == "string" and a0 ~= "" then
        dir = a0:gsub("\\", "/"):match("^(.*)/[^/]+$")
    end
    if not dir then
        local src = debug and debug.getinfo and debug.getinfo(1, "S") and debug.getinfo(1, "S").source
        if type(src) == "string" and src:sub(1, 1) == "@" then
            dir = src:sub(2):gsub("\\", "/"):match("^(.*)/[^/]+$")
        end
    end
    if not package.path:find("scripts/?/init.lua", 1, true) then
        local probes, probe = {}, dir
        while probe and probe ~= "" do
            probes[#probes + 1] = probe
            local up = probe:match("^(.*)/[^/]+$")
            if not up or up == probe then break end
            probe = up
        end
        probes[#probes + 1] = "."
        for _, p in ipairs(probes) do
            local ok, f = pcall(io.open, p .. "/scripts/kag_runner.lua", "r")
            if ok and f then
                f:close()
                root = p
                break
            end
        end
        if root and not package.path:find(root .. "/scripts/?/init.lua", 1, true) then
            package.path = root .. "/scripts/?.lua;"
                         .. root .. "/scripts/?/init.lua;"
                         .. root .. "/scripts/kag/?.lua;"
                         .. root .. "/scripts/kag/commands/?.lua;"
                         .. package.path
        end
    end
end)

local kag_runner = require("kag_runner")
local layers = require("layers")

-- [end] returns to the title: the title overlay coroutine (spawned when
-- the runner reports "ended", resumed once per frame -- review should-fix:
-- this must be a LOCAL, not an undeclared global).
local title_co = nil

-- Start the demo story
kag_runner.start("scripts/demo_story.ks")

-- ── Engine update callback (called each frame by C++ Engine::run) ────────────

-- History overlay coroutine: HistoryUI.show yields once per frame, so the
-- wrapper must be resumed every frame until it finishes (a single resume
-- would render one frame then freeze with ctx.input_focus stuck on
-- "history", deadlocking clicks/skip/auto).
local history_co = nil
local _toast = pcall(require, "toast") and require("toast") or nil
local _devHud = pcall(require, "dev_hud") and require("dev_hud") or nil

-- Ctrl hold-to-skip: D9.6 dispatches _KAG_onCtrlDown/Up from Engine.cpp
-- (key-repeat guarded); wire them so Ctrl skips while held and releases
-- restore the previous skip mode -- standard VN hold-skip behavior.
local _prevSkipMode = nil

function _KAG_onCtrlDown()
    local ctx = _G._CAESURA_CTX
    if not ctx then return end
    if _prevSkipMode == nil then _prevSkipMode = ctx.skip_mode end
    ctx.skip_mode = true
end

function _KAG_onCtrlUp()
    local ctx = _G._CAESURA_CTX
    if not ctx then return end
    ctx.skip_mode = _prevSkipMode
    _prevSkipMode = nil
end

-- t109/t125: native swipe consumers (MobileAdapter injects SDLK_SPACE /
-- SDLK_PAGEUP; Engine.cpp routes them here via _KAG_onKeySpace /
-- _KAG_onKeyPageUp). A RUNTIME DEFAULT now exists (scripts/kag.lua setup
-- tail) with the same semantics; this entry KEEPS its override
-- deliberately: its engine_update drives the history overlay coroutine
-- every frame (history_ui.show is a yield-forever loop), and the demo's
-- driver owns its own `history_co` -- dropping this override would route
-- PAGEUP to the runtime coroutine, which nothing drives in this entry
-- (overlay would freeze with input_focus stuck on "history" and clicks
-- blocked). Entries without a driver should leave the runtime default in
-- place and extend their own engine_update if they want frame-driven
-- overlays.
-- Web mirror: web/main.mjs:634-640 SwipeDown -> hide/toggle dialogue box;
-- web/main.mjs:641-647 SwipeUp -> open backlog view (bottom-scrolled).
function _KAG_onKeySpace()
    local ctx = _G._CAESURA_CTX
    if not ctx then return end
    local msg = layers.get("message")
    if msg then
        msg.visible = not msg.visible
    end
end

function _KAG_onKeyPageUp()
    local ctx = _G._CAESURA_CTX
    if ctx and ctx.input_focus ~= "history" and not history_co then
        history_co = coroutine.create(function()
            require("kag.commands.system").history(ctx, {})
        end)
    end
end

function engine_update(dt)
    -- F4: toggle the developer HUD (perf overlay)
    if _G._GAME_KEY_F4 then
        _G._GAME_KEY_F4 = false
        if _devHud then pcall(function() _devHud.toggle() end) end
    end
    if _devHud then
        pcall(function() _devHud.update(dt * 1000) end)
    end
    if _toast then
        pcall(function() _toast.update(dt) end)
    end
    -- H key: open the backlog overlay ([history] command).
    if _G._GAME_KEY_H then
        _G._GAME_KEY_H = false
        local ctx = _G._CAESURA_CTX
        if ctx and ctx.input_focus ~= "history" and not history_co then
            history_co = coroutine.create(function()
                require("kag.commands.system").history(ctx, {})
            end)
        end
    end
    if history_co then
        local ok, err = coroutine.resume(history_co)
        if not ok then
            print("[History] overlay error: " .. tostring(err))            history_co = nil
            -- The overlay may have set input_focus="history" before dying;
            -- reset it or clicks/H re-open stay deadlocked (soft-lock).
            local ctx = _G._CAESURA_CTX
            if ctx then
                ctx.input_focus = "kag"
                pcall(function()
                    require("history_ui")._hideAll(ctx)
                end)
            end
        elseif coroutine.status(history_co) == "dead" then
            history_co = nil
        end
    end
    -- V key: replay the current line's voice (backlog latest entry).
    if _G._GAME_KEY_V then
        _G._GAME_KEY_V = false
        local ctx = _G._CAESURA_CTX
        if ctx and ctx.input_focus ~= "history" and ctx.backlog and ctx.backlog[1] then
            local e = ctx.backlog[1]
            local v = e.voice
            if type(v) == "string" and #v > 0
                and not v:find("..", 1, true)
                and (v:match("%.ogg$") or v:match("%.wav$")) then
                pcall(function() backend.audio_play("voice", v) end)
            end
        end
    end
    -- A key: toggle auto mode in-game (Ren'Py-style quick toggle).
    if _G._GAME_KEY_A == true then
        _G._GAME_KEY_A = false
        local ctx = _G._CAESURA_CTX
        if ctx then
            ctx.auto_mode = not ctx.auto_mode
            if _toast then
                pcall(function() _toast.show(ctx.auto_mode and "Auto ON" or "Auto OFF") end)
            end
        end
    end
    local _, reason = kag_runner.update(dt)
    if reason == "ended" and not title_co then
        -- [end]: the script finished -- return to the title menu
        -- (KAG3 semantics; the previous code just stopped).
        print("[KAG] Script ended -- returning to title")
        local ctx = _G._CAESURA_CTX
        if ctx then
            local menu = { seen_endings = ctx.seen_endings }
            title_co = coroutine.create(function()
                return require("title_menu").show(menu)
            end)
        end
    end
    if title_co then
        -- Resume the title overlay once per frame; the show() return
        -- value (the chosen action) arrives on the resume that makes it
        -- dead -- resume()'s second result carries it.
        local ok2, action, replay = coroutine.resume(title_co)
        if not ok2 then
            print("[Title] overlay error: " .. tostring(action))
            title_co = nil
        elseif coroutine.status(title_co) == "dead" then
            title_co = nil
            if action == "new" then
                local ctx = _G._CAESURA_CTX
                if ctx then kag_runner.start("scripts/demo_story.ks") end
            elseif action == "replay" then
                local ctx = _G._CAESURA_CTX
                if ctx then ctx._pendingJump = { scene = replay } end
            end
        end
    end
    -- SMA skeletal-mesh actors ([sma_play]): advance animation time and
    -- re-skin every frame (inert when no actors / no sma module).
    local ctxS = _G._CAESURA_CTX
    if ctxS and ctxS.sma_actors then
        pcall(function()
            require("kag.sma").update(ctxS, dt)
        end)
    end
end

-- ── Engine render callback (called each frame after update) ──────────────────
-- KAG commands ([bg], [fg], [ch], etc.) manipulate layer state; this call
-- submits the layer tree to the GPU render pipeline.

function engine_render()
    layers.render()
    if title_co then layers.render_overlay_text()
    else kag_runner.render() end
    -- SMA skeletal-mesh actors: draw after the layer tree (inert when
    -- no actors / no sma module / no GPU binding).
    local ctxS = _G._CAESURA_CTX
    if ctxS and ctxS.sma_actors then
        pcall(function()
            require("kag.sma").render(ctxS)
        end)
    end
end

-- ── Input callback (called by C++ processEvents on mouse click, KAG focus) ───

function _KAG_onClick()
    kag_runner.on_click()
end

print("[KAG Demo] Entry loaded — running demo_story.ks")
