-- =============================================================================
--  Caesura (AmeKAG) �?kag/commands/audio.lua
--  KAG audio tag handlers: [playbgm], [stopbgm], [playse], [playvoice],
--  [fadebgm], [xfadebgm]
--  All audio calls route through backend.lua (unified C++ proxy).
--  Voice playback uses coroutine.yield (cooperative multitasking, no polling).
-- =============================================================================

local backend = require("backend")

local AudioCommands = {}

-- A real-time host must yield control while the audio clock is pending.
-- Ownership is scoped to this command, including coroutine close on replace.
local function audio_wait_scope(ctx, bus)
    if not ctx then return nil end
    local scope = { bus = bus }
    ctx._audio_wait = scope
    return setmetatable(scope, { __close = function()
        if ctx._audio_wait == scope then ctx._audio_wait = nil end
    end })
end

-- Internal: resolve file path (storage > path > file > positional)
local function resolve_file(params)
    -- string-only: with ALL-named params (even a typo), params[1] is
    -- the raw pair table -- a table would reach the backend binding and
    -- raise. A LEADING bare positional keeps params[1] as the string
    -- and wins when no named file exists (KAG3 behavior).
    -- (audit: same guard as jump/call/link)
    local f = params.storage or params.path or params.file
    if type(f) ~= "string" and type(params[1]) == "string" then
        f = params[1]
    end
    -- Mod resolution: enabled mods may override base assets
    -- (mods/<name>/<path>); falls back to the base path.
    if type(f) == "string" and #f > 0 then
        f = require("mods").resolve(f)
    end
    return f
end

-- =============================================================================
--  [playbgm storage="file.ogg" volume=0.8 fadein=2000 loop=true]
--  Load + play on BGM bus with optional fade-in and loop.
-- =============================================================================

-- Neo-Genesis contracts: typed + clamped via kag/schema.
local schema = require("kag.schema")
-- Volume setter family: clamped 0..1.5 like every other volume param
-- (security: no amplification through the set*volume entry points).
-- No default: the handler falls back to the positional params[1]
-- ([setbgmvolume 0.5]); a default would shadow it (coerce fills volume).
schema.define("setbgmvolume", {
    _meta = { category = "audio", blocking = false, desc = "KAG3-compatible setbgmvolume command" },
    volume = { type = "number", min = 0, max = 1.5 },
})
schema.define("setsevolume", {
    _meta = { category = "audio", blocking = false, desc = "KAG3-compatible setsevolume command" },
    volume = { type = "number", min = 0, max = 1.5 },
})
schema.define("setvoicevolume", {
    _meta = { category = "audio", blocking = false, desc = "KAG3-compatible setvoicevolume command" },
    volume = { type = "number", min = 0, max = 1.5 },
})
schema.define("playbgmstop", {
    _meta = { category = "audio", blocking = false, desc = "KAG3-compatible playbgmstop command" },
    file = { type = "file" },
    volume = { type = "number", default = 1.0, min = 0, max = 1.5 },
    fadeout = { type = "number", default = 0, min = 0, max = 30000 },
    fadein = { type = "number", default = 0, min = 0, max = 30000 },
})
schema.define("playbgm", {
    _meta = { category = "audio", blocking = false, desc = "KAG3-compatible playbgm command" },
    _require_any = { "file", "storage" },
    file    = { type = "file" },
    storage = { type = "file" },  -- KAG3 alias for file
    volume = { type = "number", default = 1.0, min = 0, max = 1.5 },
    fadein = { type = "number", default = 0, min = 0, max = 30000 },
    loop   = { type = "boolean", default = true },
})
schema.define("playse", {
    _meta = { category = "audio", blocking = false, desc = "KAG3-compatible playse command" },
    _require_any = { "file", "storage" },
    file    = { type = "file" },
    storage = { type = "file" },
    volume = { type = "number", default = 1.0, min = 0, max = 1.5 },
    fadein = { type = "number", default = 0, min = 0, max = 30000 },
})
schema.define("stopbgm", {
    _meta = { category = "audio", blocking = false, desc = "KAG3-compatible stopbgm command" },
    fadeout = { type = "number", default = 0, min = 0, max = 30000 },
    time = { type = "number", default = 0, min = 0, max = 30000 },  -- KAG3 alias
})
schema.define("stopse", {
    _meta = { category = "audio", blocking = false, desc = "KAG3-compatible stopse command" },
    fadeout = { type = "number", default = 0, min = 0, max = 30000 },
    time = { type = "number", default = 0, min = 0, max = 30000 },
})
schema.define("fadebgm", {
    _meta = { category = "audio", blocking = true, desc = "KAG3-compatible fadebgm command" },
    volume = { type = "number", default = 0, min = 0, max = 1.5 },
    time   = { type = "number", default = 1000, min = 0, max = 30000 },
    fadein = { type = "number", default = 0, min = 0, max = 30000 },
})
schema.define("fadevol", {
    _meta = { category = "audio", blocking = true, desc = "KAG3-compatible fadevol command" },
    volume = { type = "number", default = 1.0, min = 0, max = 1.5 },
    time   = { type = "number", default = 1000, min = 0, max = 30000 },
})

-- [fadevol volume=0.5 time=1000] -- smooth volume change on a bus
-- (schema existed with NO handler -- the scheduler fallback rendered
-- 'fadevol' as dialogue; audit: same class as [fadeout]/[delay]).
function AudioCommands.fadevol(ctx, params)
    local bus = params.bus or params.target or "bgm"
    backend.audio_fade_volume(bus, params.volume, params.time / 1000.0)
end

function AudioCommands.playbgm(ctx, params)
    local file = resolve_file(params)
    if not file then
        print("[AudioCmd] playbgm: no file specified")
        return
    end

    local volume = params.volume  -- schema-typed
    local fadein = params.fadein

    backend.audio_play("bgm", file, {
        fadein = fadein / 1000.0,   -- KAG uses ms, backend uses seconds
        volume = volume,
        loop   = (params.loop ~= false),
    })
end

-- =============================================================================
--  [stopbgm fadeout=2000]
--  Stop BGM with optional fade-out.
-- =============================================================================

function AudioCommands.stopbgm(ctx, params)
    local fadeout = params.fadeout or params.time or 0  -- KAG3 `time` alias

    -- Stop fades the clip; changing the bus here would mute its next BGM.
    backend.audio_stop("bgm", { fadeout = fadeout > 0 and fadeout / 1000.0 or 0 })
end

-- =============================================================================
--  [playbgmstop storage="file.ogg" fadeout=2000 fadein=2000]
--  krkrz KAG: stop current BGM with fadeout, then play new BGM with fadein.
--  Requests clip stop, then optional play; leaves the bus volume unchanged.
-- =============================================================================

function AudioCommands.playbgmstop(ctx, params)
    local file = resolve_file(params)
    local fadeout = params.fadeout
    local fadein  = params.fadein

    backend.audio_stop("bgm", { fadeout = fadeout > 0 and fadeout / 1000.0 or 0 })

    if file then
        backend.audio_play("bgm", file, {
            fadein = fadein / 1000.0,
            volume = params.volume,
        })
    end
end

-- =============================================================================
--  [fadebgm volume=0 time=2000]
--  Fade BGM bus volume to target without stopping playback.
--  KAG time is in milliseconds; backend uses seconds.
-- =============================================================================

function AudioCommands.fadebgm(ctx, params)
    local target = params.volume
    local time   = params.time

    backend.audio_fade_volume("bgm", target, time / 1000.0)
end

-- =============================================================================
--  [xfadebgm storage="file.ogg" time=2000]
--  Cross-fade: fade out current BGM, then start new BGM with fade-in.
-- =============================================================================

-- Neo-Genesis contract: typed crossfade (time clamped).
schema.define("xfadebgm", {
    _meta = { category = "audio", blocking = true, desc = "KAG3-compatible xfadebgm command" },
    file  = { type = "file" },
    storage = { type = "file" },
    time  = { type = "number", default = 2000, min = 0, max = 30000 },
})

function AudioCommands.xfadebgm(ctx, params)
    local file  = resolve_file(params)
    local time  = params.time  -- schema-typed
    if not file then
        print("[AudioCmd] xfadebgm: no file specified")
        return
    end

    backend.audio_xfade("bgm", file, time / 1000.0)
end

-- =============================================================================
--  [playse storage="click.wav" volume=0.8]
--  Play sound effect on SE bus �?fire and forget (no blocking).
-- =============================================================================

function AudioCommands.playse(ctx, params)
    local file = resolve_file(params)
    if not file then
        print("[AudioCmd] playse: no file specified")
        return
    end

    local volume = tonumber(params.volume) or 1.0

    backend.audio_play("se", file, {
        volume = volume,
    })
end

-- =============================================================================
--  [stopse]
--  Stop all currently playing sound effects.
-- =============================================================================

function AudioCommands.stopse(ctx, params)
    backend.audio_stop("se")
end

-- =============================================================================
--  [playvoice storage="line001.ogg"]
--  Play voice line on VOICE bus �?blocks until complete via coroutine.yield.
--  Each frame, the scheduler resumes and re-checks voice status.
--  When voice finishes (or _CAESURA_AUDIO_EVENT fires), the command returns.
-- =============================================================================

function AudioCommands.playvoice(ctx, params)
    -- [voice_off] mute: skip playback but keep the event flow (the wait
    -- loop still completes instantly -- no stuck dialogue).
    if ctx and ctx.voice_muted then
        _G._CAESURA_AUDIO_EVENT = "voice_end"
        return
    end
    local file = resolve_file(params)
    if not file then
        print("[AudioCmd] playvoice: no file specified")
        return
    end

    -- Clear any stale audio event before starting
    _G._CAESURA_AUDIO_EVENT = nil

    -- Play the voice line
    backend.audio_play("voice", file, {})

    local audio_wait <close> = audio_wait_scope(ctx, "voice")

    -- Block until voice finishes �?cooperative yield each frame.
    -- Two exit conditions: SoLoud handle invalid (normal) or C++ edge trigger.
    while backend.audio_is_playing("voice") do
        coroutine.yield()
        -- Check for C++ edge-triggered event (belt-and-suspenders)
        if _G._CAESURA_AUDIO_EVENT == "voice_end" then
            _G._CAESURA_AUDIO_EVENT = nil
            break
        end
    end
    _G._CAESURA_AUDIO_EVENT = nil
end

-- =============================================================================
--  [voice_wait] �� wait for the voice line but let a click skip it (the
--  standard VN pacing idiom: the player can cut a long line short).
--  KAG3 needed stopvoice glue + a hand-rolled loop for this.
-- =============================================================================
function AudioCommands.voice_wait(ctx, params)
    local audio_wait <close> = audio_wait_scope(ctx, "voice")
    -- Click detection uses the runner's consumed flag (on_click clears it
    -- and batch-resumes) -- _KAG_onClick is a permanent callback function,
    -- always truthy, and must NOT be used as a click indicator.
    -- waiting_input retains the runner's consumed-click signal: on_click
    -- clears it before batch-resuming. Ordinary frames preserve the flag.
    -- Unlike an ordinary click wait, voice completion must be polled by
    -- the runner every frame. Keep waiting_input for its existing click/skip
    -- arbitration; the dedicated marker permits exactly one frame resume.
    -- Coroutine close on stop/replacement also clears this owner's marker.
    local wait_scope <close> = setmetatable({}, { __close = function()
        if ctx then
            ctx.waiting_input = false
            ctx._voice_wait_poll = nil
        end
        _G._CAESURA_AUDIO_EVENT = nil
    end })
    if ctx then
        ctx.waiting_input = true
        ctx._voice_wait_poll = true
    end
    local ok, err = pcall(function()
        while backend.audio_is_playing and backend.audio_is_playing("voice") do
            if ctx and not ctx.waiting_input then  -- click cleared it: skip
                pcall(function() backend.audio_stop("voice") end)
                _G._CAESURA_AUDIO_EVENT = "voice_end"
                break
            end
            coroutine.yield()
        end
    end)
    -- Unconditional cleanup: even if the body raised, the flag and the
    -- event are cleared so the runner never stays blocked (LOW-1).
    if ctx then ctx.waiting_input = false end
    _G._CAESURA_AUDIO_EVENT = nil
    if not ok then error(err, 0) end  -- re-raise for the scheduler pcall
end

-- =============================================================================
--  [stopvoice]
--  Immediately stop the current voice line.
-- =============================================================================

function AudioCommands.stopvoice(ctx, params)
    backend.audio_stop("voice")
    -- Signal the voice_end edge-trigger so a pending playvoice wait loop
    -- (which polls _CAESURA_AUDIO_EVENT) unblocks immediately instead of
    -- spinning until SoLoud reports the handle invalid.
    _G._CAESURA_AUDIO_EVENT = "voice_end"
end

-- =============================================================================
--  [waitsound]
--  Block until all SE on the SE bus have finished playing.
-- =============================================================================

-- Bounded wait: a broken/muted track must not hang the runner forever
-- (audit: voice_wait had click-skip + pcall cleanup, these had none --
-- a stuck backend would yield indefinitely). 60s cap matches the
-- schema max; normal tracks finish far sooner.
local WAIT_AUDIO_LIMIT_MS = 60000

function AudioCommands.waitsound(ctx, params)
    local audio_wait <close> = audio_wait_scope(ctx, "se")
    local elapsed = 0
    while backend.audio_is_playing("se") and elapsed < WAIT_AUDIO_LIMIT_MS do
        elapsed = elapsed + (coroutine.yield() or 16)
    end
end

-- =============================================================================
--  [waitbgm]
--  Block until the current BGM finishes (for non-looping tracks).
-- =============================================================================

function AudioCommands.waitbgm(ctx, params)
    local audio_wait <close> = audio_wait_scope(ctx, "bgm")
    local elapsed = 0
    while backend.audio_is_playing("bgm") and elapsed < WAIT_AUDIO_LIMIT_MS do
        elapsed = elapsed + (coroutine.yield() or 16)
    end
end

-- =============================================================================
--  [setbgmvolume volume=0.8] / [setsevolume volume=0.5] / [setvoicevolume v=1.0]
-- =============================================================================

-- Positional path is live after the contract default removal: clamp it
-- here so [setbgmvolume 9] can't amplify (SoLoud has no clamp).
local function clampVolume(v)
    return math.min(1.5, math.max(0, v or 1.0))
end

function AudioCommands.setbgmvolume(ctx, params)
    local vol = clampVolume(tonumber(params.volume) or tonumber(params[1]) or 1.0)
    backend.audio_set_bus_volume("bgm", vol)
end

function AudioCommands.setsevolume(ctx, params)
    local vol = clampVolume(tonumber(params.volume) or tonumber(params[1]) or 1.0)
    backend.audio_set_bus_volume("se", vol)
end

function AudioCommands.setvoicevolume(ctx, params)
    local vol = clampVolume(tonumber(params.volume) or tonumber(params[1]) or 1.0)
    backend.audio_set_bus_volume("voice", vol)
end

-- =============================================================================
--  Round 51: contracts for voice/wait commands (audit: handlers existed
--  without schema contracts — no type checks, no editor validation).
-- =============================================================================
schema.define("playvoice", {
    _meta = { category = "audio", blocking = true, desc = "play a voiced line (blocks until finished)" },
    storage = { type = "file" },
    file = { type = "file" },
    path = { type = "file" },
    volume = { type = "number", default = 1.0, min = 0, max = 1.5 },
})
schema.define("voice", {
    _meta = { category = "audio", blocking = false, desc = "KAG3-compatible voice command" },
    storage = { type = "file" },
    file = { type = "file" },
    path = { type = "file" },
    volume = { type = "number", default = 1.0, min = 0, max = 1.5 },
})
schema.define("stopvoice", {
    _meta = { category = "audio", blocking = false, desc = "stop the current voice playback" },
})
schema.define("waitbgm", {
    _meta = { category = "audio", blocking = true, desc = "block until the BGM bus finishes" },
})
schema.define("waitsound", {
    _meta = { category = "audio", blocking = true, desc = "block until the SE bus finishes" },
})
schema.define("waitclick", {
    _meta = { category = "audio", blocking = true, desc = "block until a click (voice-oriented wait)" },
})

return AudioCommands
