-- ═══════════════════════════════════════════════════════════════════════
--  Caesura (AmeKAG) — rtt.lua
--  Lua-side Render-To-Texture wrapper around C++ Render bindings.
--  Manages viewport handles, blitting, and lifecycle.
-- ═══════════════════════════════════════════════════════════════════════

local backend = require("backend")
-- All-platform default canvas size follows the engine backbuffer (1920x1080
-- default, --resolution override). Falls back to 1920x1080 when the query
-- fails (tests/headless).
local function default_res()
    local ok, w, h = pcall(backend.get_resolution)
    if ok and w and h and type(w) == "number" and w > 0 and h > 0 then
        return w, h
    end
    return 1920, 1080
end


local RTT = {}

-- An owned copy of an actually submitted scene, never an empty new viewport.
-- frame identifies the completed scene; callers must not capture A and B from
-- the same frame. Destroy these leases directly rather than pooling them.
function RTT.capture_scene()
    local id, frame = backend.capture_scene()
    if type(id) ~= "number" or id <= 0 or type(frame) ~= "number" or frame <= 0 then
        error("[RTT] no rendered scene available for transition capture")
    end
    return id, frame
end

-- ═══════════════════════════════════════════════════════════════════════
-- RTT.create(width, height) → handleId
-- ═══════════════════════════════════════════════════════════════════════

function RTT.create(width, height)
    local dw, dh = default_res()
    local function pixels(value)
        local n = tonumber(value)
        assert(n and n == n and n > 0 and n <= 65535,
            "RTT dimensions must be finite positive pixel sizes within uint16 range")
        return math.ceil(n)
    end
    -- Layout stays fractional; allocation covers every requested logical
    -- pixel. Never truncate/wrap the native uint16 texture dimensions.
    return backend.create_viewport(pixels(width == nil and dw or width),
        pixels(height == nil and dh or height))
end

-- ─── Pooled lifecycle (perf: layer add/remove/resize no longer allocates
-- ─── GPU viewports every time; same-size layers reuse handles). ───────────
local pool = {}  -- "WxH" -> { handle, ... }

function RTT.acquire(width, height)
    local dw, dh = default_res()
    width, height = width or dw, height or dh
    local key = width .. "x" .. height
    local bucket = pool[key]
    if bucket and #bucket > 0 then
        return table.remove(bucket)
    end
    return RTT.create(width, height)
end

function RTT.release(handle, width, height)
    if not handle then return end
    -- Return to pool keyed by the caller-known size (bounded 8 per size).
    -- Callers pass w/h because the backend has no viewport-size getter.
    local dw, dh = default_res()
    local key = (width or dw) .. "x" .. (height or dh)
    local bucket = pool[key] or {}
    if #bucket < 8 then
        bucket[#bucket + 1] = handle
        pool[key] = bucket
    else
        RTT.destroy(handle)
    end
end

function RTT.poolSize()
    local n = 0
    for _, b in pairs(pool) do n = n + #b end
    return n
end

-- ═══════════════════════════════════════════════════════════════════════
-- RTT.destroy(handleId)
-- ═══════════════════════════════════════════════════════════════════════

function RTT.destroy(handleId)
    backend.destroy_viewport(handleId)
end

-- ═══════════════════════════════════════════════════════════════════════
-- RTT.blit(handleId, x, y, w, h) → draw RTT texture to VIEW_MAIN
-- ═══════════════════════════════════════════════════════════════════════

function RTT.fill(handleId, r, g, b, a)
    return backend.fill_viewport(handleId, r or 100, g or 100, b or 200, a or 255)
end

function RTT.blit(handleId, x, y, w, h)
    backend.draw_viewport(handleId, x or 0, y or 0, w or 1280, h or 720)
end

-- ═══════════════════════════════════════════════════════════════════════
-- RTT.get_resolution() → width, height
-- ═══════════════════════════════════════════════════════════════════════

function RTT.get_resolution()
    return backend.get_resolution()
end

return RTT
