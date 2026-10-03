-- test_backend_guard.lua — Backend resolve-chain nil safety (audit)
package.path = "scripts/?.lua;scripts/kag/?.lua;" .. package.path
local passed, failed = 0, 0
local function check(name, cond)
    if cond then print("PASS " .. name) passed = passed + 1
    else print("FAIL " .. name) failed = failed + 1 end
end

-- NO backend registered: every Backend.* entry must return without
-- throwing (the resolve chain's nil guards).
local backend = require("backend")
local calls = {
    {"load_texture", "x.png"}, {"destroy_texture", 1},
    {"create_solid_texture", 255, 0, 0, 255},
    {"render_text", "hi", 10, 10, 255, 255, 255, 255},
    {"clear_text"}, {"text_set_font", "default", 28, "white"},
    {"audio_play", "bgm", "x.ogg", { volume = 1.0 }},
    {"audio_stop", "bgm", {}},
    {"audio_fade_volume", "bgm", 0.5, 2.0},
    {"audio_xfade", "bgm", "y.ogg", 2.0},
    {"audio_is_playing", "voice"},
    {"video_play", "v.mpg", {}},
    {"video_stop"},
    {"video_is_playing"},
    {"set_resolution", 1280, 720},
    {"get_resolution"},
    {"set_input_focus", "kag"},
    {"get_input_focus"},
    {"set_fullscreen", true},
    {"particles_create_emitter", { x = 0 }},
    {"particles_emit", 1, 5},
    {"particles_destroy_emitter", 1},
    {"clear_particles"},
    {"submit_transition", 0, 1, 2, 0, 0, 0.5},
    {"create_viewport", 100, 100},
    {"destroy_viewport", 1},
    {"draw_viewport", 1, 0, 0, 100, 100},
    {"log", "test"},
}
local allSafe = true
local firstErr = nil
for _, c in ipairs(calls) do
    local fn = backend[c[1]]
    if type(fn) ~= "function" then
        allSafe = false; firstErr = c[1] .. ": missing"
        break
    end
    local ok, err = pcall(function() return fn(table.unpack(c, 2)) end)
    if not ok then
        allSafe = false; firstErr = c[1] .. ": " .. tostring(err)
        break
    end
end
check("all backend fns nil-safe", allSafe, firstErr)
check("backend count", #calls >= 20)

-- backend_factory whitelist: the 5 new render commands must route
-- (security review MEDIUM: production registers the factory table as
-- _CAESURA_BACKEND and its render() whitelist ERRORED on unknown
-- commands -- [video] threw instead of playing)
-- source-level (the suite sandbox gates the require; the file is
-- always readable)
local fh = assert(io.open("scripts/backend_factory.lua", "r"))
local src = fh:read("*a")
fh:close()
check("factory text_set_font", src:find('"text_set_font" then return Render.text_set_font', 1, true) ~= nil)
check("factory text_reset_state", src:find('"text_reset_state" then return Render.text_reset_state', 1, true) ~= nil)
check("factory video_play", src:find('"video_play" then return KAG.video_asset_play', 1, true) ~= nil)
check("factory video_stop", src:find('"video_stop" then return KAG.video_asset_stop', 1, true) ~= nil)
check("factory video_is_playing", src:find('"video_is_playing" then return KAG.video_asset_is_playing', 1, true) ~= nil)

-- Execute the production factory in an isolated environment: the shared suite
-- may already have locked require and registered a different backend.
local cancel_calls = 0
local env = setmetatable({
    KAG = {}, DevCore = {}, Engine = {},
    Render = { cancel_async_loads = function()
        cancel_calls = cancel_calls + 1
        return "cancelled"
    end },
}, { __index = _G })
env._G = env
local factory = assert(load(src, "@scripts/backend_factory.lua", "t", env))()
local proxy = factory.create()
local ok, result = pcall(proxy.render, "cancel_async_loads")
check("factory forwards async cancellation", ok and result == "cancelled")
check("factory cancels exactly once", cancel_calls == 1)

if failed > 0 then os.exit(1) end
print("BACKEND GUARD TESTS DONE")
