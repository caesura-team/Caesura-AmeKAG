-- =============================================================================
--  Caesura (AmeKAG) — kag/commands/video.lua
--  Phase 4: KAG video tag handlers — [video], [stopvideo]
--  Delegates to the C++ pl_mpeg video player via backend.
--  Spec [5.1]: PTS audio sync, click-to-skip, CancelToken support.
-- =============================================================================

local Operation   = require("kag.operation")
local backend     = require("backend")

-- Round 51 contract: [stopvideo] (audit: handler lacked a schema).
local _schema4 = require("kag.schema")
_schema4.define("stopvideo", {
    _meta = { category = "video", blocking = false, desc = "stop the current video playback" },
})

local VideoCommands = {}

local function close_playback(ctx, playback)
    if not playback or playback.closed then return true end
    playback.closed = true
    if ctx._videoPlayback == playback then ctx._videoPlayback = nil end
    if playback.handle then return backend.video_stop(playback.handle) end
    return true
end

local function resolve_file(params)
    -- string-only bare fallback (audit sweep: same as audio/layer)
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

-- ═══════════════════════════════════════════════════════════════════════════
--  [video storage="opening.mpg" loop=false volume=1.0]
--  Play a video file. Blocks coroutine until video ends or user clicks.
--  Spec [10.2.2]: PTS sync via SoLoud audio position.
--  Click during video triggers CancelToken → stop video → resume script.
-- ═══════════════════════════════════════════════════════════════════════════

-- Neo-Genesis contract: typed + clamped via kag/schema.
require("kag.schema").define("video", {
    _meta = { category = "video", blocking = true, desc = "KAG3-compatible video command" },
    file = { type = "string", positional_index = 1 },
    storage = { type = "string" },  -- KAG3 alias for file
    _require_any = { "file", "storage" },
    volume = { type = "number", default = 1.0, min = 0, max = 1.5 },
    loop = { type = "boolean", default = false },
    x = { type = "number", default = 0 },
    y = { type = "number", default = 0 },
    w = { type = "number", default = 0, min = 0, max = 8192 },
    h = { type = "number", default = 0, min = 0, max = 8192 },
})

function VideoCommands.video(ctx, params)
    local file   = resolve_file(params)
    -- schema already coerces loop to a boolean (the string forms below
    -- were dead after the schema contract landed -- audit cleanup)
    -- schema coerces loop to boolean; tolerate direct-call strings too
    -- (== never raises in 5.4, but 'true' would silently not loop)
    local loop   = params.loop == true or params.loop == "true" or params.loop == 1
    local volume = params.volume  -- schema-typed

    if not file then
        print("[VideoCmd] video: no file specified")
        return
    end

    close_playback(ctx, ctx._videoPlayback)
    local operation <close> = Operation.start(ctx)
    local ct = operation.token
    -- Allocate ownership before open. A later playback may reuse the same
    -- numeric decoder handle, so late cancellation owns this record only.
    local playback = {handle=false, closed=false,
        x=params.x or 0, y=params.y or 0, w=params.w or 0, h=params.h or 0}
    local function stop_video()
        return close_playback(ctx, playback)
    end
    ct:register(stop_video)

    -- Start video playback via backend
    local handle = backend.video_play and backend.video_play(file, {
        loop   = loop,
        volume = volume,
    })
    if type(handle) ~= "number" or handle < 1 or handle > 4294967295
        or handle ~= math.floor(handle) then
        print("[VideoCmd] video: failed to play " .. file)
        return
    end
    playback.handle = handle
    ctx._videoPlayback = playback

    -- Block until video ends, user cancels, or the 60s cap (a stuck
    -- decoder must not hang the runner -- same bound as waitsound).
    -- loop=true videos therefore auto-stop after the cap; scripts that
    -- need longer loops should re-issue [video] or use [stopvideo].
    local elapsed = 0
    while not playback.closed and backend.video_is_playing and backend.video_is_playing(handle)
          and not ct.cancelled and elapsed < 60000 do
        elapsed = elapsed + (coroutine.yield() or 16)
    end

    -- Cleanup: stop video and free decoder resources
    stop_video()
    if not ct.cancelled then
        operation:complete()
    end
end

-- ═══════════════════════════════════════════════════════════════════════════
--  [stopvideo]
--  Immediately stop video playback and free decoder resources.
--  Used mid-scene or before [jump]/[link].
-- ═══════════════════════════════════════════════════════════════════════════

function VideoCommands.stopvideo(ctx, params)
    return close_playback(ctx, ctx._videoPlayback)
end

-- Engine advances the decoder before its render callback. Rendering here keeps
-- the current frame visible without decoding twice or drawing during update.
function VideoCommands.render(ctx)
    local playback=ctx and ctx._videoPlayback
    if not playback or playback.closed then return false end
    return backend.video_draw(playback.handle,playback.x,playback.y,playback.w,playback.h)
end

return VideoCommands
