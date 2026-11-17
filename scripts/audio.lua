-- =============================================================================
--  Caesura (AmeKAG) — audio.lua
--  Spec [3.1]: Lua-side audio API wrapping C++ SoLoud bindings.
--  All calls route through backend.lua (registry handle pattern).
--  Three buses: BGM (cross-fade), VOICE (interrupt), SE (3D spatial).
-- =============================================================================

local backend = require("backend")

local Audio = {}

-- =========================================================================
-- BGM — background music with optional fade-in, loop, and position query
-- =========================================================================

function Audio.play_bgm(file, opts)
    opts = opts or {}
    return backend.audio_play("bgm", file, {
        fadein = tonumber(opts.fadein) or 1.0,
        loop   = (opts.loop ~= false),
        volume = tonumber(opts.volume),
    })
end

function Audio.stop_bgm(fadeout)
    return backend.audio_stop("bgm", { fadeout = tonumber(fadeout) or 1.0 })
end

-- Clamp volume at every setter (crafted settings files must not
-- amplify; mirrors kag.commands.audio's clampVolume).
local function clampVol(v)
    return math.min(1.5, math.max(0, tonumber(v) or 1.0))
end

function Audio.set_bgm_volume(vol)
    return backend.audio_set_bus_volume("bgm", clampVol(vol))
end

function Audio.get_bgm_volume()
    return backend.audio_get_bus_volume("bgm")
end

function Audio.get_bgm_position()
    return backend.audio_get_position("bgm")
end

-- =========================================================================
-- VOICE — voice line with interrupt (new voice cuts old) + position query
-- =========================================================================

function Audio.play_voice(file, opts)
    return backend.audio_play("voice", file, opts or {})
end

function Audio.stop_voice()
    return backend.audio_stop("voice")
end

function Audio.set_voice_volume(vol)
    return backend.audio_set_bus_volume("voice", clampVol(vol))
end

function Audio.get_voice_volume()
    return backend.audio_get_bus_volume("voice")
end

function Audio.get_voice_position()
    return backend.audio_get_position("voice")
end

-- =========================================================================
-- SE — sound effects (2D and 3D spatial)
-- =========================================================================

function Audio.play_se(file, opts)
    return backend.audio_play("se", file, opts or {})
end

function Audio.stop_se()
    return backend.audio_stop("se")
end

function Audio.play_se_3d(file, x, y, z, opts)
    return backend.audio_play("se", file, {
        x = x or 0, y = y or 0, z = z or 0,
        volume = opts and tonumber(opts.volume),
    })
end

function Audio.set_se_volume(vol)
    return backend.audio_set_bus_volume("se", clampVol(vol))
end

function Audio.get_se_volume()
    return backend.audio_get_bus_volume("se")
end

-- =========================================================================
-- 3D Listener — position the audio listener in 3D space
-- =========================================================================

function Audio.set_listener(px, py, pz, ax, ay, az)
    return backend.audio_set_listener(px, py, pz, ax, ay, az)
end

-- =========================================================================
-- Audio status queries (via backend) — Spec [3.1]
-- =========================================================================

function Audio.is_playing(bus)
    return backend.audio_is_playing(bus or "voice")
end

function Audio.is_bgm_playing()
    return backend.audio_is_playing("bgm")
end

function Audio.is_voice_playing()
    return backend.audio_is_playing("voice")
end

-- =========================================================================
-- Cross-fade BGM — Spec [3.2]
-- =========================================================================

function Audio.crossfade_bgm(new_file, fade_time)
    if new_file then
        return backend.audio_xfade("bgm", new_file, tonumber(fade_time) or 1.0)
    end
    if Audio.is_bgm_playing() then
        Audio.stop_bgm(fade_time)
    end
end

-- =========================================================================
-- Voice state queries — Spec [3.3]
-- =========================================================================

function Audio.get_voice_time()
    return backend.audio_get_position("voice")
end

function Audio.get_voice_length()
    return backend.audio_get_length("voice")
end

function Audio.get_voice_progress()
    local len = Audio.get_voice_length()
    if not len or len <= 0 then return 0 end
    return (Audio.get_voice_time() or 0) / len
end

-- =========================================================================
-- waitsound — blocking wait for all SE to stop
-- =========================================================================

function Audio.wait_sound()
    while backend.audio_is_playing("se") do
        coroutine.yield()
    end
end

-- =========================================================================
-- Fade bus volume without stopping — Spec [3.2]
-- =========================================================================

function Audio.fade_bgm_volume(target_vol, fade_time)
    return backend.audio_fade_volume("bgm", target_vol, fade_time)
end

function Audio.fade_se_volume(target_vol, fade_time)
    return backend.audio_fade_volume("se", target_vol, fade_time)
end

return Audio
