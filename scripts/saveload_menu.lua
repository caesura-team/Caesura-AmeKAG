-- ===========================================================================
--  saveload_menu.lua — Save/Load slot UI (KiriKiri-style, scheduler-driven)
--  Lists save slots (filled + empty), lets the player pick one to save into
--  or load from. Run via the [saveload mode=save|load] KAG command so the
--  scheduler coroutine drives the per-frame yield (never a bare resume).
-- ===========================================================================

local SaveLoad = {}
local backend = require("backend")
local layers = require("layers")
local Operation = require("kag.operation")
local i18n = require("i18n")
local draw_text = layers.queue_overlay_text or backend.render_text
local clear_text = layers.clear_overlay_text or function() end
local create_layer = layers.create_menu_layer or layers.ensure
local remove_layer = layers.remove_menu_layer or layers.remove_layer

local NUM_SLOTS = 10

local function solid(r, g, b, a)
    return backend.create_solid_texture(math.floor(r), math.floor(g), math.floor(b), math.floor(a or 255))
end

--- SaveLoad.show(ctx, mode, existingOnly) — returns {slot=, action=} or nil.
function SaveLoad.show(ctx, mode, existingOnly)
    mode = mode or "save"
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
        if ctx.input_focus == "saveload" then
            ctx.input_focus = previousFocus
            for _, key in ipairs({"UP", "DOWN", "ENTER", "ESC"}) do
                _G["_GAME_KEY_" .. key] = false
            end
            pcall(backend.set_input_focus, previousBackendFocus)
        end
    end
    local function finish(result)
        cleanup()
        scope:complete()
        return result
    end
    scope.token:register(cleanup)
    ctx.input_focus = "saveload"
    backend.set_input_focus("GAME")

    bg = create_layer(ctx, "_saveload_bg", 192)
    bg.visible = true
    local vw, vh = require("viewport").wh()  -- viewported from 1280x720
    bg.x, bg.y, bg.w, bg.h = 0, 0, vw, vh
    texture = solid(0, 0, 0, 210)
    bg.texture = texture

    require("kag.commands.save").listsaves(ctx, {})
    local slots = ctx.tf and ctx.tf.save_list
    -- slots is a list of {slot, desc, has_data} or similar
    local meta = {}
    if type(slots) == "table" then
        for _, s in ipairs(slots) do
            if s and s.slot then meta[s.slot] = s end
        end
    end

    local slotIds = {}
    if existingOnly then
        for slot in pairs(meta) do slotIds[#slotIds + 1] = slot end
        table.sort(slotIds)
    else
        for slot = 1, NUM_SLOTS do slotIds[slot] = slot end
    end
    local count = #slotIds
    local cursor, scroll, ITEMS = 1, 0, 8
    while true do
        clear_text()
        local title = (mode == "save") and "Save / 保存" or "Load / 读档"
        draw_text(title, 20, 8, 255, 200, 60, 255)
        local y = 52
        if count == 0 then
            draw_text("No saves / 暂无存档", 40, y, 255, 255, 255, 255)
        end
        for i = scroll + 1, math.min(scroll + ITEMS, count) do
            local slot = slotIds[i]
            local m = meta[slot]
            local label = "Slot " .. slot
            if m then
                local detail = type(m.desc) == "string" and m.desc ~= "" and m.desc or m.scene
                if type(detail) == "string" and detail ~= "" then label = label .. "  " .. detail end
            else
                label = label .. " (empty)"
            end
            local prefix = (i == cursor) and "> " or "  "
            local r, g, b = 255, 255, 255
            if i == cursor then r, g, b = 255, 220, 80 end
            draw_text(prefix .. label, 40, y, r, g, b, 255)
            y = y + 52
        end
        draw_text("[Up/Down] Select  [Enter] Confirm  [Esc] Cancel", 20, vh - 30, 120, 120, 160, 255)
        coroutine.yield()
        if scope.token.cancelled then return end

        if _G._GAME_KEY_UP == true then
            _G._GAME_KEY_UP = false
            cursor = math.max(1, cursor - 1)
        elseif _G._GAME_KEY_DOWN == true then
            _G._GAME_KEY_DOWN = false
            cursor = math.min(math.max(1, count), cursor + 1)
        elseif _G._GAME_KEY_ENTER == true then
            _G._GAME_KEY_ENTER = false
            if slotIds[cursor] ~= nil then
                return finish({ slot = slotIds[cursor], action = mode })
            end
        elseif _G._GAME_KEY_ESC == true then
            _G._GAME_KEY_ESC = false
            bg.visible = false
            return finish(nil)
        end
        if cursor <= scroll then scroll = cursor - 1
        elseif cursor >= scroll + ITEMS then scroll = cursor - ITEMS + 1 end
        scroll = math.max(0, math.min(count - ITEMS, scroll))
    end
end

return SaveLoad
