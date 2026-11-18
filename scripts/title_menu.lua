-- ===========================================================================
--  title_menu.lua — Title screen (VN standard UI, differentiator)
--  New Game / Load Game / Settings / Exit. i18n tri-lingual (zh/en/ja),
--  layer-based UI (settings.lua pattern), keyboard + mouse navigation.
--  Call TitleMenu.show(ctx) from a Lua entry; returns an action:
--    "new", "load", "settings", "exit", or nil when dismissed.
-- ===========================================================================

local TitleMenu = {}
local backend = require("backend")
local layers = require("layers")
local Operation = require("kag.operation")
local i18n = require("i18n")
local draw_text = layers.queue_overlay_text or backend.render_text
local clear_text = layers.clear_overlay_text or function() end
local create_layer = layers.create_menu_layer or layers.ensure
local remove_layer = layers.remove_menu_layer or layers.remove_layer

local function solid(r, g, b, a)
    return backend.create_solid_texture(math.floor(r), math.floor(g), math.floor(b), math.floor(a or 255))
end

--- TitleMenu.showEndings(ctx, token) — inline sub-screen (no separate coroutine:
-- keeps the same yield-loop pattern as the title itself, so no orphan
-- coroutine soft-lock like the gallery/music experiment). Lists unlocked
-- endings from ctx.seen_endings ([ending id=] records them).
local function showEndings(ctx, token)
    local endings = ctx.seen_endings
    if type(endings) ~= "table" then endings = {} end
    local entries = {}
    for id, info in pairs(endings) do
        if type(id) == "string" then
            local name = type(info) == "table" and info.name
            local scene = type(info) == "table" and info.scene
            entries[#entries + 1] = { id = id,
                name = type(name) == "string" and name ~= "" and name or id,
                scene = type(scene) == "string" and scene or nil }
        end
    end
    table.sort(entries, function(a, b)
        if a.name == b.name then return a.id < b.id end
        return a.name < b.name
    end)
    local cursor = 1
    local vw, vh = require("viewport").wh()  -- viewported from 1280x720
    while true do
        clear_text()
        draw_text(i18n.t("endings_title") or "Endings", math.floor(vw / 2) - 60, 180, 220, 180, 80, 255)
        if #entries == 0 then
            draw_text("(none yet)", math.floor(vw / 2) - 60, 300, 120, 120, 160, 255)
        else
            local y = 300
            for i, entry in ipairs(entries) do
                local prefix = (i == cursor) and "> " or "  "
                local r, g, b = 255, 255, 255
                if i == cursor then r, g, b = 255, 220, 80 end
                draw_text(prefix .. "[OK] " .. entry.name, math.floor(vw / 2) - 80, y, r, g, b, 255)
                y = y + 40
            end
        end
        draw_text("[Up/Down] Select  [Enter] Replay  [Esc] Back", 20, vh - 30, 120, 120, 160, 255)
        coroutine.yield()
        if token.cancelled then return end
        if _G._GAME_KEY_UP == true then
            _G._GAME_KEY_UP = false
            cursor = cursor - 1
            if cursor < 1 then cursor = math.max(1, #entries) end
        elseif _G._GAME_KEY_DOWN == true then
            _G._GAME_KEY_DOWN = false
            cursor = cursor + 1
            if cursor > #entries then cursor = 1 end
        elseif _G._GAME_KEY_ENTER == true then
            _G._GAME_KEY_ENTER = false
            local target = entries[cursor] and entries[cursor].scene
            if target and #target > 0 then
                return target
            end
        elseif _G._GAME_KEY_ESC == true then
            _G._GAME_KEY_ESC = false
            return
        end
    end
end

--- TitleMenu.show(ctx) returns an action and, for replay, a scene path.
function TitleMenu.show(ctx)
    local menuItems = { "new_game", "load_game", "settings", "endings", "exit" }
    if ctx.continue_slot ~= nil and type(KAG) == "table"
        and type(KAG.save_exists) == "function" and KAG.save_exists(ctx.continue_slot) then
        table.insert(menuItems, 2, "continue")
    end
    local scope <close> = Operation.start(ctx)
    local previousFocus = ctx.input_focus or "kag"
    local ok, focus = pcall(backend.get_input_focus)
    local previousBackendFocus = ok and type(focus) == "string" and focus
        or (previousFocus == "kag" and "KAG" or "GAME")
    local bg, texture, cleaned
    local function cleanup()
        if cleaned then return end
        cleaned = true
        clear_text()
        if bg then
            bg.visible, bg.texture = false, nil
            pcall(remove_layer, bg)
        end
        if texture then pcall(backend.destroy_texture, texture) end
        if ctx.input_focus == "title" then
            ctx.input_focus = previousFocus
            for _, key in ipairs({"UP", "DOWN", "ENTER", "ESC"}) do
                _G["_GAME_KEY_" .. key] = false
            end
            pcall(backend.set_input_focus, previousBackendFocus)
        end
    end
    local function finish(result, scene)
        cleanup()
        scope:complete()
        return result, scene
    end
    scope.token:register(cleanup)
    ctx.input_focus = "title"
    backend.set_input_focus("GAME")

    bg = create_layer(ctx, "_title_bg", 90)
    bg.visible = true
    bg.x, bg.y = 0, 0
    local vw, vh = require("viewport").wh()  -- viewported from 1280x720
    bg.w, bg.h = vw, vh
    texture = solid(8, 8, 20, 255)
    bg.texture = texture

    local cursor = 1
    while true do
        clear_text()
        -- Title
        draw_text(i18n.t("title_screen"), math.floor(vw / 2) - 120, 180, 220, 180, 80, 255)

        -- Menu items
        local y = 320
        for i, key in ipairs(menuItems) do
            local label = i18n.t(key)
            if i == cursor then
                draw_text("> " .. label, math.floor(vw / 2) - 80, y, 255, 220, 80, 255)
            else
                draw_text("  " .. label, math.floor(vw / 2) - 80, y, 180, 180, 200, 255)
            end
            y = y + 44
        end
        draw_text("[Up/Down] Select  [Enter] Confirm  [Esc] Exit", 20, vh - 30, 120, 120, 160, 255)

        coroutine.yield()
        if scope.token.cancelled then return end

        -- Input (polled one-shot globals)
        if _G._GAME_KEY_UP == true then
            _G._GAME_KEY_UP = false
            cursor = cursor - 1
            if cursor < 1 then cursor = #menuItems end
        elseif _G._GAME_KEY_DOWN == true then
            _G._GAME_KEY_DOWN = false
            cursor = cursor + 1
            if cursor > #menuItems then cursor = 1 end
        elseif _G._GAME_KEY_ENTER == true then
            _G._GAME_KEY_ENTER = false
            local key = menuItems[cursor]
            if key == "new_game" then return finish("new")
            elseif key == "continue" then return finish("continue")
            elseif key == "load_game" then return finish("load")
            elseif key == "settings" then return finish("settings")
            elseif key == "endings" then
                local scene = showEndings(ctx, scope.token)
                if scene then return finish("replay", scene) end
                if scope.token.cancelled then return end
            else return finish("exit") end
        elseif _G._GAME_KEY_ESC == true then
            _G._GAME_KEY_ESC = false
            bg.visible = false
            return finish(nil)
        end
    end
end

return TitleMenu
