-- Drive the production title entry and menus with recording host boundaries.
package.path = "scripts/?.lua;scripts/?/init.lua;" .. package.path

local function fixture(slots)
    local f = { slots = slots or {}, draws = {}, focus = "KAG" }
    local env = setmetatable({}, { __index = function(_, key)
        if key == "_CAESURA_CTX" then return nil end
        return _G[key]
    end })
    env._G = env
    env.config = { title_story = "demo/galgame_demo.ks" }
    env.KAG = { save_exists = function(slot) return f.slots[slot] ~= nil end }
    env.DevCore = { quit = function() f.quit = true end }
    local overlay = {}

    local modules = {
        ["kag.operation"] = require("kag.operation"),
        ["system"] = { AUTOSAVE_SLOT = -2 },
        ["kag.presentation"] = { discard = function() end },
        ["flow"] = { is_restore_scene = function(path) return path == "demo/replay-other.ks" end },
        ["i18n"] = { t = function(key) return key end },
        ["viewport"] = { wh = function() return 1280, 720 end },
        ["layers"] = {
            ensure = function() return {} end,
            render = function() f.layers_rendered = true end,
            clear_overlay_text = function() overlay = {} end,
            queue_overlay_text = function(text) overlay[#overlay + 1] = text end,
            render_overlay_text = function()
                assert(f.layers_rendered)
                for _, text in ipairs(overlay) do f.draws[text] = true end
                overlay = {}
            end,
        },
        ["backend"] = {
            create_solid_texture = function() return 1 end,
            destroy_texture = function() end,
            get_input_focus = function() return f.focus end,
            set_input_focus = function(focus) f.focus = focus end,
            render_text = function(text) f.draws[text] = true end,
        },
        ["settings"] = { show = function(ctx)
            f.draws.settings_panel = true
            repeat coroutine.yield() until env._GAME_KEY_ESC
            env._GAME_KEY_ESC = false
            ctx.settingsValues = { text_speed = 75 }
        end },
        ["kag.commands.save"] = {
            listsaves = function(ctx)
                local list = {}
                for _, entry in pairs(f.slots) do list[#list + 1] = entry end
                ctx.tf = { save_list = list }
            end,
            prepare_load = function(params)
                f.requested_slot = params.slot
                if f.refuse then return nil, "corrupt fixture" end
                return { slot = params.slot }
            end,
        },
        ["kag_runner"] = {
            get_ctx = function() return f.owner end,
            start = function(path, options)
                f.started, f.start_options = path, options
                f.owner = { seen_endings = {
                    good = { name = "Good End", scene = "demo/replay.ks" },
                    other = { name = "Good End", scene = "demo/replay-other.ks" },
                    broken = { name = {}, scene = 12 },
                } }
                return true
            end,
            restore_candidate = function(owner, candidate)
                assert(owner == nil, "title restore must start from a cold runner")
                f.restored_slot, f.owner = candidate.slot, {}
                return true
            end,
            stop = function() f.owner = nil; return true end,
            update = function() if f.ended then return false, "ended" end return true end,
            render = function() f.rendered = true end,
        },
    }
    env.require = function(name)
        if name == "title_menu" or name == "saveload_menu" then
            modules[name] = modules[name] or assert(loadfile("scripts/" .. name .. ".lua", "t", env))()
        end
        return assert(modules[name], name)
    end

    assert(loadfile("scripts/title_demo_entry.lua", "t", env))()
    function f.tick(key)
        f.draws = {}
        f.layers_rendered = false
        if key then env["_GAME_KEY_" .. key] = true end
        env.engine_update(0.016)
        env.engine_render()
    end
    function f.render() env.engine_render() end
    return f
end

local cases = {
    { "cold startup, settings, new game and return after an ending", function()
        local f = fixture()
        f.tick()
        assert(f.draws.title_screen and f.focus == "GAME" and not f.owner)
        f.tick("DOWN")
        f.tick("DOWN")
        f.tick("ENTER")
        assert(f.draws.settings_panel and not f.started)
        f.tick("ESC")
        assert(f.draws.title_screen and not f.started)
        f.tick("ENTER")
        assert(f.started == "demo/galgame_demo.ks" and f.focus == "KAG")
        assert(f.start_options.playback_settings.text_speed == 75)
        f.render()
        assert(f.rendered)
        f.ended = true
        f.tick()
        f.tick()
        assert(f.draws.title_screen and not f.owner)
        f.tick("DOWN")
        f.tick("DOWN")
        f.tick("DOWN")
        f.tick("ENTER")
        assert(f.draws.endings_title)
        f.ended = false
        f.tick("DOWN")
        f.tick("ENTER")
        assert(f.started == "demo/replay-other.ks")
    end },
    { "Continue uses the real autosave slot and failed restore returns to title", function()
        local f = fixture({ [-2] = { slot = -2, desc = "Auto Save" } })
        f.tick()
        assert(f.draws["  continue"] and not f.owner)
        f.refuse = true
        f.tick("DOWN")
        f.tick("ENTER")
        assert(f.requested_slot == -2 and f.draws.title_screen and not f.started and not f.owner)
        f.refuse = false
        f.tick("DOWN")
        f.tick("ENTER")
        assert(f.restored_slot == -2 and not f.started and f.focus == "KAG")
    end },
    { "Load lists an actual manual slot zero without offering Continue", function()
        local f = fixture({ [0] = { slot = 0, scene = "assets/script/manual.ks" } })
        f.tick()
        assert(not f.draws["  continue"])
        f.tick("DOWN")
        f.tick("ENTER")
        assert(f.draws["> Slot 0  assets/script/manual.ks"] and f.focus == "GAME" and not f.owner)
        f.tick("ENTER")
        assert(f.restored_slot == 0 and not f.started)
    end },
    { "Escape quits from a cold title", function()
        local f = fixture()
        f.tick()
        f.tick("ESC")
        assert(f.quit and not f.owner)
    end },
}

local failed = 0
for _, case in ipairs(cases) do
    local ok, err = pcall(case[2])
    if ok then print("PASS " .. case[1])
    else failed = failed + 1; print("FAIL " .. case[1] .. ": " .. tostring(err)) end
end
if failed > 0 then error("title entry regressions: " .. failed .. " failed", 0) end
print("TITLE ENTRY TESTS DONE")
