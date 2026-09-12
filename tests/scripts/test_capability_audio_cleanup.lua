-- Isolated U19 cleanup contract: actual backend/capability/public KAG code;
-- only host capability facts and audio operations are controlled boundaries.
package.path = "scripts/?.lua;scripts/?/init.lua;scripts/kag/?.lua;" .. package.path

local json = require("capability_json")
local target = require("target_capabilities")
local profile = {
    schema = 1, target = "web", platform = "browser", scope = "runtime",
    catalog_sha256 = target.catalog_sha256, compiled = {}, available = { audio = false },
}
_G.__CAESURA_CAPABILITY_PROFILE_JSON = function() return assert(json.encode(profile)) end

-- The owner exists only at this recorded host boundary. No AudioContext,
-- decoding, sound-card output or fake production handler is claimed here.
local owner = { playing = false }
local calls = { play = 0, stop = 0, fade = 0, stopFade = nil }
_G._CAESURA_BACKEND = {
    audio = function(method, ...)
        if method == "play_bgm" then
            calls.play = calls.play + 1
            owner.playing = true
            return 1
        end
        if method == "stop_bgm" then
            calls.stop = calls.stop + 1
            calls.stopFade = select(1, ...)
            owner.playing = false
            return true
        end
        if method == "fade_volume" then calls.fade = calls.fade + 1; return false end
        if method == "is_bgm_playing" then return owner.playing end
        if method == "get_bus_volume" or method == "get_global_volume" then return 1 end
        return false
    end,
    render = function(method)
        if method == "get_resolution" then return 1920, 1080 end
        return false
    end,
    platform = function(method)
        if method == "get_resolution" then return 1920, 1080 end
        return false
    end,
}

local backend = require("backend")
local runtime = require("capability_runtime")
local schema = require("kag.schema")
local kag = require("kag")
local passed, failed = 0, 0
local function check(name, condition)
    if condition then passed = passed + 1; print("PASS " .. name)
    else failed = failed + 1; print("FAIL " .. name) end
end
local function context(command)
    local ctx = { current_scene = "audio-cleanup.ks", token_index = 1,
        _executing_command = command, f = {}, sf = {}, tf = {} }
    _G._CAESURA_CTX = ctx
    return ctx
end
local function start_owner()
    if owner.playing then backend.audio_stop("bgm", { fadeout = 0 }) end
    profile.available.audio = true
    local value, result = backend.audio_play("bgm", "controlled.ogg", { fadein = 0 })
    check("setup creates a playing owner through the actual backend module",
        value == 1 and result.status == "applied" and backend.audio_is_playing("bgm"))
    -- The implementation now supports fade, but a later unavailable host must
    -- still allow cleanup of its previously registered playback owner.
    profile.available.audio = false
end
local function reported_unsupported(ctx)
    local entries = ctx.capability_diagnostics
    return type(entries) == "table" and #entries == 1
        and entries[1].status == "unsupported" and entries[1].feature == "audio.fade"
end

check("controlled host is currently unavailable despite implemented playback and fade",
    backend.get_capability("audio.play").status == "unsupported"
        and backend.get_capability("audio.fade").status == "unsupported")

for _, policy in ipairs({ "optional", "required" }) do
    -- An undeclared operation is required at runtime. Explicitly requiring an
    -- unsupported Web fade at boot would reject configuration before this test
    -- can establish an existing playback owner, so use the real default policy.
    local declaration = policy == "optional" and '{"capabilities":{"optional":["audio.fade"]}}' or '{}'
    check(policy .. " runtime policy configured", runtime.configure_project_json(declaration) == true)
    for _, command in ipairs({ "stopbgm", "playstop" }) do
        local label = policy .. " " .. command
        local ctx = context(command)
        start_owner()
        local before_stop, before_fade = calls.stop, calls.fade
        local params = schema.coerce(command, { fadeout = 250 }, ctx)
        local invoked, value, detail = pcall(kag[command], ctx, params)
        check(label .. " still stops the existing owner exactly once",
            not owner.playing and calls.stop == before_stop + 1)
        check(label .. " rejects both explicit and implicit backend fading",
            calls.stop == before_stop + 1 and calls.fade == before_fade and calls.stopFade == 0)
        check(label .. " reports the unsupported fade", reported_unsupported(ctx))
        if policy == "optional" then
            check(label .. " returns skip result after cleanup", invoked and value == false
                and type(detail) == "table" and detail.status == "unsupported" and detail.feature == "audio.fade")
        else
            check(label .. " retains required error after cleanup", not invoked and type(value) == "string"
                and value:find("status=unsupported", 1, true) ~= nil
                and value:find("feature=audio.fade", 1, true) ~= nil)
        end

        ctx = context(command)
        start_owner()
        before_stop, before_fade = calls.stop, calls.fade
        params = schema.coerce(command, { fadeout = 0 }, ctx)
        invoked = pcall(kag[command], ctx, params)
        check(label .. " ordinary stop remains a successful cleanup",
            invoked and not owner.playing and calls.stop == before_stop + 1 and calls.fade == before_fade)
        check(label .. " ordinary stop invents no unsupported fade", not reported_unsupported(ctx))
    end

    local ctx = context("audio_stop")
    start_owner()
    local before_stop, before_fade = calls.stop, calls.fade
    local invoked, value, detail = pcall(backend.audio_stop, "bgm", { fadeout = 0.25 })
    check(policy .. " direct backend stop still releases the owner once",
        invoked and value == true and not owner.playing and calls.stop == before_stop + 1)
    check(policy .. " direct backend stop does not pass unsupported fade to the host",
        calls.fade == before_fade and calls.stopFade == 0)
    check(policy .. " direct backend stop returns typed unsupported fade",
        type(detail) == "table" and detail.status == "unsupported" and detail.feature == "audio.fade")
    check(policy .. " direct backend stop records unsupported fade", reported_unsupported(ctx))

    ctx = context("audio_stop")
    start_owner()
    before_stop = calls.stop
    invoked, value = pcall(backend.audio_stop, "bgm", { fadeout = 0 })
    check(policy .. " direct zero-fade cleanup remains successful",
        invoked and value == true and not owner.playing and calls.stop == before_stop + 1 and calls.stopFade == 0)
    check(policy .. " direct zero-fade cleanup invents no unsupported fade", not reported_unsupported(ctx))
end

if owner.playing then backend.audio_stop("bgm", { fadeout = 0 }) end
_G._CAESURA_CTX = nil
print(string.format("CAPABILITY AUDIO CLEANUP: %d passed, %d failed", passed, failed))
if failed > 0 then os.exit(1) end
print("ALL CAPABILITY AUDIO CLEANUP TESTS PASSED")
