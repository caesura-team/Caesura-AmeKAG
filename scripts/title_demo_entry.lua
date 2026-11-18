-- Native title entry for a configurable KAG scene.
local runner = require("kag_runner")
local layers = require("layers")
local title = require("title_menu")
local settings = require("settings")
local saveload = require("saveload_menu")
local saves = require("kag.commands.save")
local presentation = require("kag.presentation")
local flow = require("flow")
local autosaveSlot = require("system").AUTOSAVE_SLOT
local safe_replay_scene = flow.is_restore_scene or saves._safeScenePath

local okToast, toast = pcall(require, "toast")
if not okToast then toast = nil end
local quit = type(DevCore) == "table" and DevCore.quit
local story = type(config) == "table" and config.title_story or "demo/galgame_demo.ks"
local menu = { input_focus = "kag", continue_slot = autosaveSlot, seen_endings = {} }
local playing = false

local function show_error(message, reason)
    print("[Title] " .. message .. ": " .. tostring(reason))
    if toast then toast.show(message) end
end

local function restore_slot(slot)
    local candidate, reason = saves.prepare_load({ slot = slot })
    if not candidate then
        show_error("Load failed", reason)
        return false
    end

    local restored, restore_error = runner.restore_candidate(nil, candidate)
    if not restored then
        presentation.discard(candidate._presentation)
        show_error("Load failed", restore_error)
    end
    return restored
end

local function start_story(path)
    local started, reason = runner.start(path, { playback_settings = menu.settingsValues })
    if not started then show_error("Could not start story", reason) end
    return started
end

local function choose_game()
    while true do
        local action, replay = title.show(menu)
        if action == nil or action == "exit" then
            if type(quit) == "function" then quit() end
            return false
        elseif action == "new" then
            if start_story(story) then return true end
        elseif action == "replay" then
            if not safe_replay_scene(replay) then
                show_error("Replay unavailable", replay)
            elseif start_story(replay) then return true end
        elseif action == "continue" then
            if restore_slot(autosaveSlot) then return true end
        elseif action == "load" then
            local selected = saveload.show(menu, "load", true)
            if selected and restore_slot(selected.slot) then return true end
        elseif action == "settings" then
            settings.show(menu, true)
        end
    end
end

local menu_co = coroutine.create(choose_game)

function engine_update(dt)
    if toast then toast.update(dt) end
    if menu_co then
        local resumed, result = coroutine.resume(menu_co)
        if not resumed then
            coroutine.close(menu_co)
            menu_co = nil
            show_error("Title menu failed", result)
        elseif coroutine.status(menu_co) == "dead" then
            menu_co = nil
            playing = result == true
        end
        return
    end

    if playing then
        local _, reason = runner.update(dt)
        if reason == "ended" then
            local owner = runner.get_ctx()
            for id, ending in pairs(owner and owner.seen_endings or {}) do
                menu.seen_endings[id] = ending
            end
            playing = false
            local stopped, stop_error = runner.stop()
            if stopped then menu_co = coroutine.create(choose_game)
            else show_error("Could not return to title", stop_error) end
        end
    end
end

function engine_render()
    layers.render()
    if playing then runner.render()
    elseif layers.render_overlay_text then layers.render_overlay_text() end
end

function _KAG_onClick()
    if playing then runner.on_click() end
end
