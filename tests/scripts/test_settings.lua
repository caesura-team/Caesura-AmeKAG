local saved_backend=rawget(_G,"_CAESURA_BACKEND")
local saved_layers=package.loaded["layers"]
local saved_KAG = rawget(_G, "KAG")
local fixture_KAG = {}
if type(saved_KAG)=="table" then for k,v in pairs(saved_KAG) do fixture_KAG[k]=v end end
fixture_KAG.list_assets=function(dir,max_entries,max_bytes)
    assert(dir=="assets/lang" and max_entries==4096 and max_bytes==1024*1024)
    return {"en.lua","ja.lua","zh.lua"},nil
end
rawset(_G,"KAG",fixture_KAG)
local fixture_ok, fixture_error=pcall(function()
-- test_settings.lua — settings menu input loop (audit)
package.path = "scripts/?.lua;scripts/kag/?.lua;" .. package.path
local passed, failed = 0, 0
local function check(name, cond)
    if cond then print("PASS " .. name) passed = passed + 1
    else print("FAIL " .. name) failed = failed + 1 end
end

local Settings = require("settings")
local real_backend = _G._CAESURA_BACKEND
local audio_volumes, audio_calls = {}, 0
_G._CAESURA_BACKEND = { render = function() return true end,
    audio = function(method, bus, value)
        assert(method=="set_bus_volume", "unexpected settings audio method: "..tostring(method))
        assert(bus=="bgm" or bus=="se" or bus=="voice", "unexpected settings bus")
        assert(type(value)=="number" and value==value and value>=0 and value<=1,
            "settings volume must be a normalized finite number")
        audio_calls=audio_calls+1
        audio_volumes[bus]=value
        return true -- backend setter result contract; no command replacement
    end,
    platform = function(cmd)
        if cmd == "get_resolution" then return 1280, 720 end
        if cmd == "set_input_focus" then return true end
        return true end }
local layers_backup = package.loaded["layers"]
package.loaded["layers"] = { ensure = function() return { visible = true } end,
    find = function() return nil end, set_layer_visible = function() end,
    set_z = function() end }
local ctx = { f = {}, sf = {}, tf = {}, mp = {}, variables = {}, settingsValues = {} }
local co = coroutine.create(function() Settings.show(ctx) end)
local function resume_checked()
    local ok, err=coroutine.resume(co)
    if not ok then
        coroutine.close(co) -- run actual <close> owners before reporting failure
        error(tostring(err),0)
    end
end
resume_checked()
resume_checked()
check("settings opens", ctx._settingsActive == true)
-- slider: right at cursor 1 bumps volume_bgm by 5
local before = ctx.settingsValues.volume_bgm
_G._GAME_KEY_RIGHT = true
resume_checked()
check("slider adjusts", ctx.settingsValues.volume_bgm == (before or 0) + 5)
-- toggle: down to skip_mode (item 5) then right flips
for _ = 1, 4 do
    _G._GAME_KEY_DOWN = true
    resume_checked()
end
local sm_before = ctx.settingsValues.skip_mode
_G._GAME_KEY_RIGHT = true
resume_checked()
check("toggle flips", ctx.settingsValues.skip_mode ~= sm_before)
-- ESC closes
_G._GAME_KEY_ESC = true
resume_checked()
check("esc closes", coroutine.status(co) == "dead" and ctx._settingsActive == false)
check("settings submits all three normalized audio buses",
    audio_calls>=3 and audio_volumes.bgm==ctx.settingsValues.volume_bgm/100
    and audio_volumes.se==ctx.settingsValues.volume_se/100
    and audio_volumes.voice==ctx.settingsValues.volume_voice/100)
package.loaded["layers"] = layers_backup
_G._CAESURA_BACKEND = real_backend

if failed > 0 then os.exit(1) end
print("SETTINGS TESTS DONE")

end)
rawset(_G,"KAG",saved_KAG)
rawset(_G,"_CAESURA_BACKEND",saved_backend)
package.loaded["layers"]=saved_layers
assert(fixture_ok,fixture_error)
