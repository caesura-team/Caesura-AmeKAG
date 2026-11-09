-- ===========================================================================
--  Caesura (AmeKAG) — settings.lua  [P1.4]
--  Interactive settings menu using layer-based UI.
--  Volume sliders, text speed, skip/auto toggles, fullscreen, language.
--  Input focus switches to GAME mode during settings to prevent KAG misinput.
-- ===========================================================================

local Settings = {}
local backend = require("backend")
local layers  = require("layers")
local audio   = require("audio")
local i18n    = require("i18n")
local Operation = require("kag.operation")
local layout_math = require("kag.layout_math")  -- [R107-A] declarative vbox/hbox/grid math

-- Language hot-switch full-page redraw: after i18n.load the already
-- displayed page, backlog, choice labels and closed captions re-localize
-- with the new language (exceeds Ren'Py, which keeps displayed lines in
-- the original language). Lazy require keeps the settings menu decoupled
-- from the KAG command chain at startup.
local function relocalize_after_switch(ctx)
    pcall(function()
        local kt = require("kag.commands.text")
        if kt and kt.relocalize_page then kt.relocalize_page(ctx) end
    end)
end

local function apply_autosave_interval(seconds)
    local engine = rawget(_G, "_CAESURA_ENGINE")
    if engine and type(engine.setAutoSaveInterval) == "function" then
        engine.setAutoSaveInterval(seconds)
    end
end

-- ===========================================================================
-- Settings state
-- ===========================================================================
local state = {
    active = false,
    cursor = 1,         -- selected menu item index
    items = {},         -- { {label, key, type, value, min, max, step, action}, ... }
    bgLayer = nil,
    cursorLayer = nil,
    panelLayer = nil,   -- centered panel background
    borderLayer = nil,  -- panel border
}

-- Helper: safe solid texture
local function solid(r, g, b, a)
    return backend.create_solid_texture(math.floor(r), math.floor(g), math.floor(b), math.floor(a or 255))
end

-- Cancellation retires the UI without applying its pending settings.
local function cleanup(ctx, owner)
    if not state.active or state.ctx ~= ctx then return false end
    if owner and state.scope ~= owner then return false end
    state.active = false
    state.ctx = nil
    state.scope = nil
    if state.edits then
        ctx.settingsValues = state.previousValues
        state.edits, state.previousValues = nil, nil
    end
    ctx._settingsActive = false
    for _, key in ipairs({"bgLayer", "borderLayer", "panelLayer", "cursorLayer"}) do
        local layer = state[key]
        state[key] = nil
        if layer then
            local texture = layer.texture
            layer.visible, layer.texture = false, nil
            if texture then pcall(backend.destroy_texture, texture) end
        end
    end
    if ctx.input_focus == "settings" then
        ctx.input_focus = state.previousFocus
        for _, key in ipairs({"UP", "DOWN", "LEFT", "RIGHT", "ENTER", "ESC"}) do
            _G["_GAME_KEY_" .. key] = false
        end
        pcall(backend.set_input_focus, state.previousBackendFocus)
    end
    state.items = {}
    state.panelX, state.panelY, state.panelW, state.panelH = nil, nil, nil, nil
    state.previousFocus, state.previousBackendFocus = nil, nil
    return true
end

-- ===========================================================================
-- Settings defaults
-- ===========================================================================
local defaults = {
    volume_bgm   = 80,
    volume_se    = 80,
    volume_voice = 100,
    cc_mode      = false,  -- accessibility: closed captions for voiced lines
    text_speed   = 50,   -- ms per char
    fullscreen   = false,
    skip_mode    = false,
    skip_auto    = false,  -- read-skip: force-past unseen text at 2x instead of stopping
    auto_mode    = false,
    autosave_interval = 60,   -- seconds; 0 disables the auto-save timer
}

-- ===========================================================================
-- Settings._buildMenu(ctx) -> items table
-- ===========================================================================
function Settings._buildMenu(ctx)
    ctx.settingsValues = ctx.settingsValues or {}
    local sv = ctx.settingsValues
    for k, v in pairs(defaults) do
        if sv[k] == nil then sv[k] = v end
    end

    local items = {
        {label = i18n.t("volume_bgm"),   key = "volume_bgm",   type = "slider", value = sv.volume_bgm,   min = 0, max = 100},
        {label = i18n.t("volume_se"),    key = "volume_se",    type = "slider", value = sv.volume_se,    min = 0, max = 100},
        {label = i18n.t("volume_voice"), key = "volume_voice", type = "slider", value = sv.volume_voice, min = 0, max = 100},
        {label = i18n.t("text_speed"),   key = "text_speed",   type = "slider", value = sv.text_speed,   min = 10, max = 200},
        {label = i18n.t("skip_mode"),    key = "skip_mode",    type = "toggle", value = sv.skip_mode},
        {label = i18n.t("skip_auto"),    key = "skip_auto",    type = "toggle", value = sv.skip_auto},
        {label = i18n.t("auto_mode"),    key = "auto_mode",    type = "toggle", value = sv.auto_mode},
        -- Accessibility (Neo-Genesis): closed captions draw voiced
        -- dialogue at a fixed bottom position (see kag_runner.render).
        {label = i18n.t("cc_mode"),      key = "cc_mode",      type = "toggle", value = sv.cc_mode},
        {label = i18n.t("fullscreen"),   key = "fullscreen",   type = "toggle", value = sv.fullscreen},
        {label = i18n.t("language") .. ": " .. (i18n.current or "zh"),
         key = "language", type = "cycle", value = i18n.current,
         options = i18n.available()},
        {label = i18n.t("autosave_interval") .. ": " ..
             (sv.autosave_interval > 0 and sv.autosave_interval .. "s" or "Off"),
         key = "autosave_interval", type = "cycle", value = sv.autosave_interval,
         options = {30, 60, 120, 0}},
        {label = i18n.t("back"), key = "back", type = "action"},
    }
    return items
end

-- Settings._adjust(ctx, dir) — slider +/- / toggle flip / cycle next
function Settings._adjust(ctx, dir)
    local item = state.items and state.items[state.cursor]
    if not item then return end
    local sv = ctx.settingsValues or {}
    if item.type == "slider" then
        local v = (sv[item.key] or item.value or 0) + dir * 5
        sv[item.key] = math.max(item.min, math.min(item.max, v))
    elseif item.type == "toggle" then
        sv[item.key] = not sv[item.key]
    elseif item.type == "cycle" then
        local opts = item.options or {}
        local idx = 0
        local current = sv[item.key] or item.value
        for i, o in ipairs(opts) do if o == current then idx = i break end end
        idx = ((idx + dir - 1) % #opts) + 1
        sv[item.key] = opts[idx]
        if item.key == "language" and not state.edits and i18n and i18n.load then
            pcall(function() i18n.load(opts[idx]) end)
            relocalize_after_switch(ctx)
        end
    end
    item.value = sv[item.key]
    if state.edits then state.edits[item.key] = item.value end
end

-- ===========================================================================
-- Settings.show(ctx) — open settings menu
-- ===========================================================================
function Settings.show(ctx, edits_only)
    if state.active or (edits_only and ctx.input_focus == "history") then return end
    local scope <close> = Operation.start(ctx)
    scope.token:register(function() cleanup(ctx, scope) end)
    state.scope = scope
    state.ctx = ctx
    state.previousFocus = ctx.input_focus or "kag"
    local ok, focus = pcall(backend.get_input_focus)
    state.previousBackendFocus = ok and type(focus) == "string" and focus
        or (state.previousFocus == "kag" and "KAG" or "GAME")
    state.active = true
    ctx._settingsActive = true
    ctx.input_focus = "settings"
    -- In a running scene, defaults are for display. Closing the panel only
    -- applies deliberate edits and cancellation discards the draft.
    if edits_only then
        state.edits = {}
        state.previousValues = ctx.settingsValues
        local draft = {}
        for key, value in pairs(ctx.settingsValues or {}) do draft[key] = value end
        for _, key in ipairs({"text_speed", "auto_mode", "skip_mode", "skip_auto", "cc_mode"}) do
            if ctx[key] ~= nil then draft[key] = ctx[key] end
        end
        for _, bus in ipairs({"bgm", "se", "voice"}) do
            local volume = backend.audio_get_bus_volume(bus)
            if type(volume) == "number" then
                draft["volume_" .. bus] = math.floor(math.max(0, math.min(1, volume)) * 100 + 0.5)
            end
        end
        ctx.settingsValues = draft
    end
    state.items = Settings._buildMenu(ctx)
    state.cursor = 1

    -- Switch input focus to GAME to prevent KAG text advancement
    backend.set_input_focus("GAME")

    local w, h = backend.get_resolution()
    local vw, vh = require("viewport").wh()  -- viewported from 1280x720
    w = w or vw
    h = h or vh

    -- Panel dimensions (centered, with border)
    local panelW = 680
    local panelH = 420
    local panelX = (w - panelW) / 2
    local panelY = (h - panelH) / 2

    -- Layer 1: Dark semi-transparent full-screen background
    local bg = layers.ensure(ctx, "_settings_bg", 94)
    bg.visible = true
    bg.x, bg.y = 0, 0
    bg.w, bg.h = w, h
    bg.texture = solid(0, 0, 0, 160)
    state.bgLayer = bg

    -- Layer 2: Panel border (2px visible border around the panel)
    local border = layers.ensure(ctx, "_settings_border", 95)
    border.visible = true
    border.x, border.y = panelX - 2, panelY - 2
    border.w, border.h = panelW + 4, panelH + 4
    border.texture = solid(80, 80, 180, 255)
    state.borderLayer = border

    -- Layer 3: Panel background (slightly dark blue-ish panel)
    local panel = layers.ensure(ctx, "_settings_panel", 96)
    panel.visible = true
    panel.x, panel.y = panelX, panelY
    panel.w, panel.h = panelW, panelH
    panel.texture = solid(15, 15, 40, 235)
    state.panelLayer = panel

    -- Layer 4: Cursor highlight layer (colored rectangle behind selected row)
    local cursor = layers.ensure(ctx, "_settings_cursor", 97)
    cursor.visible = true
    cursor.texture = solid(60, 100, 200, 100)
    cursor.w = panelW - 80
    cursor.h = 28
    state.cursorLayer = cursor

    -- Store panel geometry for rendering
    state.panelX = panelX
    state.panelY = panelY
    state.panelW = panelW
    state.panelH = panelH

    -- INPUT LOOP (audit: show() rendered once and returned -- the
    -- 'Up/Down to navigate' hint was a lie; the settings menu was
    -- dead). Same per-frame pattern as Gallery/MusicRoom.
    while true do
        Settings._render(ctx)
        coroutine.yield()
        if scope.token.cancelled or state.scope ~= scope then return end

        if _G._GAME_KEY_UP == true then
            _G._GAME_KEY_UP = false
            state.cursor = math.max(1, state.cursor - 1)
        elseif _G._GAME_KEY_DOWN == true then
            _G._GAME_KEY_DOWN = false
            state.cursor = math.min(#state.items, state.cursor + 1)
        elseif _G._GAME_KEY_LEFT == true then
            _G._GAME_KEY_LEFT = false
            Settings._adjust(ctx, -1)
        elseif _G._GAME_KEY_RIGHT == true then
            _G._GAME_KEY_RIGHT = false
            Settings._adjust(ctx, 1)
        elseif _G._GAME_KEY_ENTER == true then
            _G._GAME_KEY_ENTER = false
            local item = state.items[state.cursor]
            if item and item.type == "action" then
                if item.key == "back" then break end
            else
                Settings._adjust(ctx, 1)
            end
        elseif _G._GAME_KEY_ESC == true then
            _G._GAME_KEY_ESC = false
            break
        end
    end
    Settings.hide(ctx)
    scope:complete()
end

-- ===========================================================================
-- Settings.hide(ctx) — close settings, restore KAG focus
-- ===========================================================================
function Settings.hide(ctx)
    local edits = state.edits
    if not cleanup(ctx) then return end
    if edits and next(edits) then
        ctx.settingsValues = ctx.settingsValues or {}
        for key, value in pairs(edits) do ctx.settingsValues[key] = value end
        if edits.language then
            i18n.load(edits.language)
            relocalize_after_switch(ctx)
        end
        if edits.autosave_interval then apply_autosave_interval(edits.autosave_interval) end
    end
    Settings._applyAll(ctx, edits)
    print("[Settings] Menu closed.")
end

-- ===========================================================================
-- Settings._renderVolumeBar(value, max) -> string
-- ===========================================================================
function Settings._renderVolumeBar(value, max)
    local blocks = math.floor(value / (max / 10))  -- 10 segments
    local filled = string.rep("|", blocks)
    local empty = string.rep(" ", 10 - blocks)
    return "[" .. filled .. empty .. "] " .. tostring(value) .. "%"
end

-- ===========================================================================
-- Settings._volumeBand(ctx) -> { row1..row3 = {x,y,w,h} }  (R107-A [layout])
-- Declarative vbox (via kag/layout_math) for the 3-bus volume slider rows
-- (menu items 1..3). Anchored at the exact historic row geometry
-- (px+30, startY; lineH=30) so the resolved slot rects are coordinate-
-- identical to the previous hand-rolled absolute layout:
--   row[i].x = px + 30, row[i].y = startY + (i-1)*30
-- Uses the authoritative R107-A contract: layout_math.vbox returns each
-- slot relative to the container content origin; we add the panel origin.
-- ===========================================================================
function Settings._volumeBand(ctx)
    local px = state.panelX or 340
    local py = state.panelY or 120
    local startY = py + 50
    local lineH = 30
    local bandW = (state.panelW or 600) - 100

    -- 3 flexible rows over h = 3*lineH -> each row is exactly lineH tall.
    local items = { { h = 0 }, { h = 0 }, { h = 0 } }
    local res = layout_math.vbox(items, { w = bandW, h = 3 * lineH, gap = 0 })
    local originX, originY = px + 30, startY

    local band = {}
    for i, slot in ipairs(res.slots or {}) do
        band["row" .. i] = {
            x = originX + slot.x,
            y = originY + slot.y,
            w = slot.w,
            h = slot.h,
        }
    end
    return band
end

-- Row geometry for the menu list; the 3 volume rows (indices 1..3) are
-- sourced from the declarative _volumeBand, all other rows keep the historic
-- formula. Both branches resolve to identical coordinates (proving the
-- declarative layout is coordinate-equivalent to the old absolute layout).
-- Returns { startY, lineH, rowY = function(i) }, where rowY(i)/rowX(i) are ints.
function Settings._rowGeometry(ctx)
    local px = state.panelX or 340
    local py = state.panelY or 120
    local startY = py + 50
    local lineH = 30
    local band = Settings._volumeBand(ctx)
    return {
        startY = startY,
        lineH = lineH,
        rowY = function(i)
            if i >= 1 and i <= 3 and band["row" .. i] then
                return band["row" .. i].y
            end
            return startY + (i - 1) * lineH
        end,
        rowX = function(i)
            if i >= 1 and i <= 3 and band["row" .. i] then
                return band["row" .. i].x
            end
            return px + 30
        end,
    }
end


-- ===========================================================================
-- Settings._render(ctx) — draw settings menu text
-- ===========================================================================
function Settings._render(ctx)
    if not state.active then return end
    local w, h = backend.get_resolution()
    local vw, vh = require("viewport").wh()  -- viewported from 1280x720
    w = w or vw
    h = h or vh
    local px = state.panelX or 340
    local py = state.panelY or 120
    local pw = state.panelW or 600
    local ph = state.panelH or 360

    local geo = Settings._rowGeometry(ctx)
    local startY = geo.startY
    local lineH = geo.lineH

    -- Title: "Settings / 設置" at top of panel
    local titleStr = "Settings / 設置"
    local titleX = px + (pw - #titleStr * 9) / 2  -- rough centering
    backend.render_text(titleStr, titleX, py + 12, 255, 200, 60, 255)

    -- Title separator line
    backend.render_text(string.rep("-", math.floor(pw / 9)), px + 20, py + 28, 80, 80, 140, 200)

    -- Menu items
    -- R107-A PILOT: row x/y for the 3 volume-slider rows come from the
    -- declarative vbox (_volumeBand); all other rows keep the historic
    -- formula. Both resolve to identical coordinates (proven in the loop).
    for i, item in ipairs(state.items) do
        local lineY = geo.rowY(i)
        local lineX = geo.rowX(i)
        local isSelected = (i == state.cursor)
        local prefix = isSelected and " > " or "   "

        -- Build value display string
        local valStr = ""
        if item.type == "slider" then
            valStr = " " .. Settings._renderVolumeBar(item.value, item.max or 100)
        elseif item.type == "toggle" then
            valStr = item.value and " [ON]" or " [OFF]"
        elseif item.type == "cycle" then
            valStr = " <" .. tostring(item.value) .. ">"
        end

        local line = prefix .. item.label .. valStr
        local r, g, b = 220, 220, 255  -- default text color
        if isSelected then
            r, g, b = 255, 255, 150  -- yellow for selected
        elseif item.type == "action" then
            r, g, b = 180, 180, 200  -- dim for action items
        elseif item.type == "toggle" and item.value then
            r, g, b = 100, 255, 100  -- green for ON toggles
        end
        backend.render_text(line, lineX, lineY, r, g, b, 255)
    end

    -- Cursor highlight position (position behind the selected row)
    if state.cursorLayer then
        state.cursorLayer.x = px + 26
        state.cursorLayer.y = geo.rowY(state.cursor) - 2
    end

    -- Footer inside panel
    local footerY = py + ph - 24
    backend.render_text("Arrow Keys: Navigate | Left/Right: Adjust | Enter: Confirm | ESC: Back", px + 20, footerY, 150, 150, 200, 255)
end

-- ===========================================================================
-- Settings._applyAll(ctx) — apply all settings values
-- ===========================================================================
function Settings._applyAll(ctx, edits)
    local sv = edits or ctx.settingsValues
    if not sv then return end
    if sv.volume_bgm then audio.set_bgm_volume(sv.volume_bgm / 100) end
    if sv.volume_se then audio.set_se_volume(sv.volume_se / 100) end
    -- Keys must match the runner/commands (snake_case); camelCase variants
    -- were silently ignored by kag_runner/TextCommands.
    if sv.text_speed then ctx.text_speed = sv.text_speed end
    if sv.skip_mode ~= nil then ctx.skip_mode = sv.skip_mode end
    if sv.skip_auto ~= nil then ctx.skip_auto = sv.skip_auto end
    if sv.auto_mode ~= nil then ctx.auto_mode = sv.auto_mode end
    if sv.volume_voice then audio.set_voice_volume(sv.volume_voice / 100) end
    if sv.fullscreen ~= nil then backend.set_fullscreen(sv.fullscreen) end
    -- Accessibility: closed captions mirror into BOTH the context flag
    -- (read by TextCommands.ch / kag_runner.render; no module coupling)
    -- and config (persisted default).
    if sv.cc_mode ~= nil then
        ctx.cc_mode = sv.cc_mode == true
        local okC, config = pcall(require, "config")
        if okC then
            config.accessibility = config.accessibility or {}
            config.accessibility.cc_mode = ctx.cc_mode
        end
    end
end

-- ===========================================================================
-- Settings.navigate(ctx, direction) — move cursor Up/Down
-- ===========================================================================
function Settings.navigate(ctx, direction)
    if not state.active then return end
    if direction == "up" then
        state.cursor = state.cursor - 1
        if state.cursor < 1 then state.cursor = #state.items end
    elseif direction == "down" then
        state.cursor = state.cursor + 1
        if state.cursor > #state.items then state.cursor = 1 end
    end
    Settings._render(ctx)
end

-- ===========================================================================
-- Settings.adjust(ctx, direction) — adjust selected value Left/Right
-- ===========================================================================
function Settings.adjust(ctx, direction)
    if not state.active then return end
    local item = state.items[state.cursor]
    if not item then return end
    local sv = ctx.settingsValues

    if item.type == "slider" then
        local step = item.step or 5
        if direction == "left" then
            item.value = math.max(item.min, item.value - step)
        else
            item.value = math.min(item.max, item.value + step)
        end
        sv[item.key] = item.value
        if not state.edits then
            if item.key == "volume_bgm" then audio.set_bgm_volume(item.value / 100)
            elseif item.key == "volume_se" then audio.set_se_volume(item.value / 100) end
        end

    elseif item.type == "toggle" then
        item.value = not item.value
        sv[item.key] = item.value

    elseif item.type == "cycle" then
        local opts = item.options or {}
        if #opts > 0 then
            local curIdx = 1
            for i, o in ipairs(opts) do if o == item.value then curIdx = i; break end end
            if direction == "left" then curIdx = curIdx - 1 else curIdx = curIdx + 1 end
            if curIdx < 1 then curIdx = #opts end
            if curIdx > #opts then curIdx = 1 end
            item.value = opts[curIdx]
            sv[item.key] = item.value
            if item.key == "language" then
                -- Update label for display
                item.label = i18n.t("language") .. ": " .. item.value
                if not state.edits then
                    i18n.load(item.value)
                    relocalize_after_switch(ctx)
                    state.items = Settings._buildMenu(ctx)
                end
            elseif item.key == "autosave_interval" then
                item.label = i18n.t("autosave_interval") .. ": " ..
                    (item.value > 0 and item.value .. "s" or "Off")
                if not state.edits then pcall(apply_autosave_interval, item.value) end
            end
        end
    end
    if state.edits and item.type ~= "action" then state.edits[item.key] = sv[item.key] end
    Settings._render(ctx)
end

-- ===========================================================================
-- Settings.confirm(ctx) — activate selected item (Enter/Click)
-- ===========================================================================
function Settings.confirm(ctx)
    if not state.active then return end
    local item = state.items[state.cursor]
    if not item then return end

    if item.type == "action" and item.key == "back" then
        Settings.hide(ctx)
        return
    end
    -- Toggle items also activate on confirm
    if item.type == "toggle" then
        Settings.adjust(ctx, "right")
    end
end

-- ===========================================================================
-- Settings.onClick(ctx, x, y) — handle click in settings menu
-- ===========================================================================
function Settings.onClick(ctx, x, y)
    if not state.active then return false end
    -- Map y coordinate to menu item within the panel. Row geometry (including
    -- the declarative 3 volume-slider rows) is sourced from _rowGeometry so
    -- click targetting stays aligned with the rendered layout.
    local geo = Settings._rowGeometry(ctx)
    local lineH = geo.lineH
    local clickedIdx = math.floor((y - geo.startY) / lineH) + 1
    if clickedIdx >= 1 and clickedIdx <= #state.items then
        state.cursor = clickedIdx
        Settings.confirm(ctx)
    end
    return true  -- consumed
end

return Settings
