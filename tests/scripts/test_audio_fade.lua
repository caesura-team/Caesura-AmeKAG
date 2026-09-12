-- test_audio_fade.lua — [fadebgm]/[xfadebgm] contracts (audit)
package.path = "scripts/?.lua;scripts/kag/?.lua;" .. package.path
local passed, failed = 0, 0
local function check(name, cond)
    if cond then print("PASS " .. name) passed = passed + 1
    else print("FAIL " .. name) failed = failed + 1 end
end

local Audio = package.loaded["kag.commands.audio"] or require("kag.commands.audio")
local calls = {}
local real_backend = _G._CAESURA_BACKEND
_G._CAESURA_BACKEND = { render = function() return true end,
    audio = function(cmd, ...)
        if cmd == "fade_volume" then calls[#calls + 1] = { "fade", ... } end
        if cmd == "xfade" then calls[#calls + 1] = { "xfade", ... } end
        return true
    end,
}
local ctx = { f = {}, tf = {}, sf = {}, mp = {}, variables = {},
    viewport = { width = 1280, height = 720 } }

-- fadebgm delegates ms -> seconds
Audio.fadebgm(ctx, { volume = 0.3, time = 2000 })
check("fadebgm delegates", calls[1] and calls[1][1] == "fade"
      and calls[1][2] == "bgm" and calls[1][3] == 0.3 and calls[1][4] == 2.0)

-- xfadebgm routes file + seconds
calls = {}
Audio.xfadebgm(ctx, { storage = "next.ogg", time = 3000 })
check("xfadebgm delegates", calls[1] and calls[1][1] == "xfade"
      and calls[1][2] == "bgm" and calls[1][3] == "next.ogg" and calls[1][4] == 3.0)

-- xfadebgm with NO file: no backend call (audit fix)
calls = {}
Audio.xfadebgm(ctx, { time = 1000 })
check("xfade no-file safe", #calls == 0)

-- schema clamps
local schema = require("kag.schema")
local c = schema.coerce("xfadebgm", { storage = "a.ogg", time = "999999" }, {})
check("xfade time clamped", c.time == 30000)
local c2 = schema.coerce("fadebgm", { volume = "9", time = "1" }, {})
check("fadebgm volume clamped", c2.volume == 1.5)

-- playse volume schema clamp
local c3 = schema.coerce("playse", { storage = "s.wav", volume = "9" }, {})
check("playse volume clamped", c3.volume == 1.5)

_G._CAESURA_BACKEND = real_backend

-- bare file (audit): [xfadebgm next.ogg] -> params[1]
-- (re-arm the mock: the earlier restore at line 48 removed it)
_G._CAESURA_BACKEND = { render = function() return true end,
    audio = function(cmd, ...)
        calls[#calls + 1] = { cmd, ... }
        return true end }
calls = {}
Audio.xfadebgm(ctx, { "next.ogg", time = 1000 })
check("xfade bare file", calls[1] and calls[1][3] == "next.ogg")
-- typo'd named param: params[1] is a pair table -- must not crash
calls = {}
Audio.xfadebgm(ctx, { { "storag", "x.ogg" }, time = 1000 })
check("xfade typo pair safe", #calls == 0)
_G._CAESURA_BACKEND = real_backend

-- U20: execute the actual schema/handlers/backend proxy. Only the host audio
-- boundary records requests and its configured bus values; no mixer is faked.
do
    local backend = require("backend")
    local requests, active_path = {}, nil
    local bus_values = { bgm = 1, se = 1, voice = 1 }
    _G._CAESURA_BACKEND = {
        audio = function(command, ...)
            local args = { ... }
            requests[#requests + 1] = { command, ... }
            if command == "set_bus_volume" or command == "fade_volume" then
                bus_values[args[1]] = args[2]
            elseif command == "play_bgm" then active_path = args[1]
            elseif command == "stop_bgm" then active_path = nil end
            return true
        end,
    }
    local function setup_owner()
        backend.audio_set_bus_volume("bgm", 0.6)
        backend.audio_set_bus_volume("se", 0.4)
        backend.audio_set_bus_volume("voice", 0.8)
        backend.audio_play("bgm", "assets/bgm/old.ogg", { fadein = 0 })
        requests = {}
    end
    local function same_bus_values()
        return bus_values.bgm == 0.6 and bus_values.se == 0.4 and bus_values.voice == 0.8
    end
    for _, case in ipairs({
        { name = "positive", input = { fadeout = 250 }, seconds = 0.25 },
        { name = "ordinary zero", input = { fadeout = 0 }, seconds = 0 },
        { name = "default", input = {}, seconds = 0 },
        { name = "coerced time shadow", input = { time = 900 }, seconds = 0 },
        { name = "schema upper bound", input = { fadeout = 999999 }, seconds = 30 },
    }) do
        setup_owner()
        Audio.stopbgm(ctx, schema.coerce("stopbgm", case.input, ctx))
        check("stopbgm single exact clip stop: " .. case.name,
            #requests == 1 and requests[1][1] == "stop_bgm" and requests[1][2] == case.seconds)
        check("stopbgm preserves configured buses: " .. case.name, same_bus_values())
        check("stopbgm releases the routed owner: " .. case.name, active_path == nil)
    end

    setup_owner()
    Audio.stopbgm(ctx, schema.coerce("stopbgm", { fadeout = 500 }, ctx))
    Audio.playbgm(ctx, schema.coerce("playbgm", { storage = "assets/bgm/next.ogg", fadein = 750 }, ctx))
    check("stop then play preserves request order, path and seconds",
        #requests == 2 and requests[1][1] == "stop_bgm" and requests[1][2] == 0.5
        and requests[2][1] == "play_bgm" and requests[2][2] == "assets/bgm/next.ogg" and requests[2][3] == 0.75)
    check("stop then play cannot leave the next clip on a muted bus",
        active_path == "assets/bgm/next.ogg" and same_bus_values())

    for _, milliseconds in ipairs({ 0, 400 }) do
        setup_owner()
        Audio.playbgmstop(ctx, schema.coerce("playbgmstop", {
            file = "assets/bgm/replacement.ogg", fadeout = milliseconds, fadein = 1250,
        }, ctx))
        check("playbgmstop submits one clip stop before play: " .. milliseconds,
            #requests == 2 and requests[1][1] == "stop_bgm" and requests[1][2] == milliseconds / 1000
            and requests[2][1] == "play_bgm" and requests[2][2] == "assets/bgm/replacement.ogg"
            and requests[2][3] == 1.25)
        check("playbgmstop preserves buses and replacement path: " .. milliseconds,
            same_bus_values() and active_path == "assets/bgm/replacement.ogg")
    end
    setup_owner()
    Audio.playbgmstop(ctx, schema.coerce("playbgmstop", { fadeout = 350 }, ctx))
    check("playbgmstop without a file only stops once at the requested time",
        #requests == 1 and requests[1][1] == "stop_bgm" and requests[1][2] == 0.35
        and active_path == nil and same_bus_values())

    requests = {}
    Audio.fadebgm(ctx, schema.coerce("fadebgm", { volume = 0.3, time = 2000 }, ctx))
    check("explicit fadebgm still changes only its bus at requested seconds",
        #requests == 1 and requests[1][1] == "fade_volume" and requests[1][2] == "bgm"
        and requests[1][3] == 0.3 and requests[1][4] == 2 and bus_values.bgm == 0.3
        and bus_values.se == 0.4 and bus_values.voice == 0.8)
    requests = {}
    Audio.fadevol(ctx, schema.coerce("fadevol", { volume = 0.2, time = 1500 }, ctx))
    check("explicit fadevol keeps the default bus fade contract",
        #requests == 1 and requests[1][1] == "fade_volume" and requests[1][2] == "bgm"
        and requests[1][3] == 0.2 and requests[1][4] == 1.5 and bus_values.bgm == 0.2
        and bus_values.se == 0.4 and bus_values.voice == 0.8)
    _G._CAESURA_BACKEND = real_backend
end

-- guard sweep (audit): layer resolve_file + layfade layerName +
-- eval/emb exp all reject the pair table from named params
local Layer3 = require("kag.commands.layer")
local calls3 = {}
local be3 = _G._CAESURA_BACKEND
_G._CAESURA_BACKEND = { render = function(cmd, ...)
    if cmd == "load_texture" then calls3[#calls3 + 1] = { ... } end
    return true end,
    platform = function() return true end }
local layers3 = package.loaded["layers"]
package.loaded["layers"] = { get = function() return nil end,
    add_layer = function() return { visible = true } end,
    set_layer_opacity = function() end, mark_dirty = function() end }
local ctxG = { f = {}, tf = {}, sf = {}, mp = {}, variables = {} }
-- typo'd named param on [bg]: pair table must not reach load_texture
pcall(Layer3.bg, ctxG, { { "storag", "x.png" } })
check("layer resolve pair safe", #calls3 == 0)
-- emb/assert exp: the guard must reject the pair table (source-lock
-- the guard form in the live handlers). [eval] is a registry stub since
-- t195 (its live path is scheduler.lua inline), so its exp-guard no
-- longer exists here -- the guard-lock now covers emb + assert.
local f2 = assert(io.open("scripts/kag/commands/system.lua", "r"))
local src2 = f2:read("*a")
f2:close()
local gcount = 0
local pos = 1
local needle = 'type(exp) ~= "string" and type(params[1]) == "string"'
while true do
    local p = src2:find(needle, pos, true)
    if not p then break end
    gcount = gcount + 1
    pos = p + 1
end
check("exp guarded in live handlers", gcount == 2)
package.loaded["layers"] = layers3
_G._CAESURA_BACKEND = be3

print(string.format("AUDIO FADE ROUTING: %d passed, %d failed", passed, failed))
if failed > 0 then os.exit(1) end
print("AUDIO FADE TESTS DONE")
