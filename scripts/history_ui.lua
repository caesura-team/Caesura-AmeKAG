-- ===========================================================================
--  Caesura (AmeKAG) — history_ui.lua
--  Backlog display interface. Renders scrollable semi-transparent overlay
--  with text history entries, supports jump-to-scene and voice replay.
--  Spec: U3 backlog display — [history] tag triggers HistoryUI.show(ctx).
--
--  Controller: [↑↓] navigate, [Enter] jump, [V] replay voice, [Esc] close,
--              mouse wheel scrolls, click selects/closes.
--  Renders via the layer system (settings.lua pattern) + backend.render_text;
--  input via the _GAME_KEY_* polled globals (Engine.cpp dispatches them).
-- ===========================================================================

local HistoryUI = {}
local backend = require("backend")
local layers  = require("layers")

-- Reusable solid-color texture
local function solid(r, g, b, a)
    return backend.create_solid_texture(math.floor(r), math.floor(g), math.floor(b), math.floor(a or 255))
end

-- Hide every layer this UI created (background/title/footer/sep/highlights).
function HistoryUI._hideAll(ctx)
    local names = { "_history_bg", "_history_title", "_history_sep",
                    "_history_footer" }
    for _, n in ipairs(names) do
        local l = layers.get_layer(n)
        if l then l.visible = false end
    end
    for i = 1, (ctx.backlog and #ctx.backlog or 0) do
        local hl = layers.get_layer("_history_hl" .. i)
        if hl then hl.visible = false end
    end
end

-- Consume a one-shot key (engine sets true on down, false on up)
local function key_consumed(name)
    local v = _G[name]
    if v then
        _G[name] = false  -- one-shot semantics for menu navigation
        return true
    end
    return false
end

function HistoryUI.show(ctx)
    -- Guard: nothing to show
    if not ctx.backlog or #ctx.backlog == 0 then
        return
    end

    local previous_focus = ctx.input_focus or "kag"
    local scope <close> = setmetatable({}, {__close = function()
        if ctx.input_focus == "history" then
            ctx.input_focus = previous_focus
            pcall(function() backend.set_input_focus(string.upper(previous_focus)) end)
        end
        HistoryUI._hideAll(ctx)
    end})
    ctx.input_focus = "history"
    local selected = #ctx.backlog
    local scroll   = 0
    local ITEMS    = 12

    -- Filter state: F key cycles 0=all, 1=voice-only, 2=sprite-only.
    local filter = 0
    local function filteredIndex(i)  -- backlog index of the i-th filtered entry
        if filter == 0 then return i end
        local n = 0
        for idx = 1, #ctx.backlog do
            local e = ctx.backlog[idx]
            local keep = (filter == 1 and e.voice and #e.voice > 0)
                      or (filter == 2 and e.sprite and #e.sprite > 0)
            if keep then
                n = n + 1
                if n == i then return idx end
            end
        end
        return nil
    end
    local function filteredCount()
        if filter == 0 then return #ctx.backlog end
        local n = 0
        for _, e in ipairs(ctx.backlog) do
            if (filter == 1 and e.voice and #e.voice > 0)
            or (filter == 2 and e.sprite and #e.sprite > 0) then
                n = n + 1
            end
        end
        return n
    end

    -- Overlay layers (created once, reused per frame)
    local bgLayer   = layers.ensure(ctx, "_history_bg", 94)
    local titleLayer = layers.ensure(ctx, "_history_title", 95)
    local footerLayer = layers.ensure(ctx, "_history_footer", 97)
    bgLayer.visible = true
    titleLayer.visible = true
    footerLayer.visible = true

    while true do
        local w, h = require("viewport").wh()  -- viewported from 1280x720
        local footerY = h - 30

        -- Full-screen dark overlay
        bgLayer.x, bgLayer.y = 0, 0
        bgLayer.w, bgLayer.h = w, h
        bgLayer.texture = solid(0, 0, 0, 200)

        -- Title bar strip
        titleLayer.x, titleLayer.y = 0, 0
        titleLayer.w, titleLayer.h = w, 40
        titleLayer.texture = solid(16, 16, 48, 224)

        backend.render_text("Backlog / 消息记录", 20, 8, 255, 200, 60, 255)
        backend.render_text("Entry " .. selected .. " / " .. #ctx.backlog, math.floor(w / 2), 8, 60, 200, 120, 255)

        -- Separator under title
        local sep = layers.ensure(ctx, "_history_sep", 96)
        sep.visible = true
        sep.x, sep.y, sep.w, sep.h = 0, 40, w, 1
        sep.texture = solid(96, 64, 160, 255)

        -- Scrollable entries
        local y = 52
        local entryH = 22
        local totalShown = filteredCount()
        local lastVisible = math.min(scroll + ITEMS, totalShown)
        for i = scroll + 1, lastVisible do
            local e = ctx.backlog[filteredIndex(i)]
            local isSelected = (filteredIndex(i) == selected)

            -- Highlight selected entry
            if isSelected then
                local hl = layers.ensure(ctx, "_history_hl" .. i, 98)
                hl.visible = true
                hl.x, hl.y = 10, y - 1
                hl.w, hl.h = w - 20, entryH + 2
                hl.texture = solid(32, 64, 160, 100)
            end

            local prefix = isSelected and "> " or "  "
            local preview = (e.text or ""):sub(1, 60)
            if #(e.text or "") > 60 then preview = preview .. "..." end
            local speaker = e.name or ""
            if speaker == "" then speaker = "(Narration)" end
            local voiceIndicator = ""
            if e.voice and #e.voice > 0 then voiceIndicator = " [V]" end

            local r, g, b = 255, 255, 255
            if isSelected then r, g, b = 255, 220, 80 end
            local line = string.format("%s[%s] %s%s", prefix, speaker, preview, voiceIndicator)
            backend.render_text(line, 20, y + 1, r, g, b, 255)

            -- Timestamp
            if e.time then
                backend.render_text(e.time, w - 130, y + 1, 140, 140, 160, 255)
            elseif e.timestamp then
                backend.render_text(os.date("%H:%M", e.timestamp), w - 130, y + 1, 140, 140, 160, 255)
            end

            -- Scene tag
            if e.scene and #e.scene > 0 then
                local sceneShort = e.scene:match("[^/]+$") or e.scene
                sceneShort = sceneShort:sub(1, 20)
                backend.render_text("[" .. sceneShort .. "]", math.floor(w * 750 / 1280), y + 1, 90, 150, 255, 255)
            end
            y = y + entryH + 2
        end
        -- Hide stale highlight layers beyond the visible window
        for i = lastVisible + 1, #ctx.backlog do
            local stale = layers.get_layer("_history_hl" .. i)
            if stale then stale.visible = false end
        end

        -- Footer
        footerLayer.x, footerLayer.y = 0, footerY
        footerLayer.w, footerLayer.h = w, 30
        footerLayer.texture = solid(16, 16, 48, 224)
        local filterLabel = (filter == 0) and "All"
                         or (filter == 1) and "Voice-only"
                         or "Sprite-only"
        backend.render_text("[Up/Down] Navigate  [Enter] Jump  [V] Voice  [F] Filter: " .. filterLabel .. "  [Esc] Close", 20, footerY + 3, 60, 200, 120, 255)

        -- Yield to engine frame
        coroutine.yield()

        -- Input handling (polled one-shot globals)
        local wheel = _G._KAG_MOUSE_WHEEL_Y or 0
        if wheel ~= 0 then
            _G._KAG_MOUSE_WHEEL_Y = 0
            if wheel > 0 then selected = math.max(1, selected - 1)
            else selected = math.min(filteredCount(), selected + 1) end
        end
        if key_consumed("_GAME_KEY_F") then
            -- Cycle filter; reselect the last filtered entry.
            filter = (filter + 1) % 3
            selected = filteredCount()
        elseif key_consumed("_GAME_KEY_UP") then
            selected = math.max(1, selected - 1)
        elseif key_consumed("_GAME_KEY_DOWN") then
            selected = math.min(filteredCount(), selected + 1)
        elseif key_consumed("_GAME_KEY_ENTER") then
            local e = ctx.backlog[selected]
            if e and e.scene and e.token_index then
                return { jump = true, scene = e.scene, index = e.token_index }
            end
        elseif key_consumed("_GAME_KEY_ESC") then
            return
        end
        -- V: replay voice (key dispatched by the engine when available)
        if _G._GAME_KEY_V == true then
            _G._GAME_KEY_V = false
            local e = ctx.backlog[selected]
            -- Voice file from a crafted save: require an audio extension AND
            -- reject traversal so SoLoud never decodes an arbitrary file.
            local v = e and e.voice
            if type(v) == "string" and #v > 0
                and not v:find("..", 1, true)
                and (v:match("%.ogg$") or v:match("%.wav$")) then
                backend.audio_play("voice", v)
            end
        end

        -- Scroll clamping
        if selected <= scroll then
            scroll = selected - 1
        elseif selected >= scroll + ITEMS then
            scroll = selected - ITEMS + 1
        end
        scroll = math.max(0, math.min(#ctx.backlog - ITEMS, scroll))
    end
end

return HistoryUI
