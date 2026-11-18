-- ===========================================================================
--  Caesura (AmeKAG) -- vfx.lua
--  GPU-backed visual effects: Quake, Shake, Flash, Fade, Blur.
--  All blocking functions use coroutine.yield for frame-stepped animation.
--  KAG durations and coroutine frame deltas are milliseconds. Convert to
--  seconds only for time-domain math such as sinusoidal shake frequency.
--  Spec [2.5]: quake = random offset × linear decay | shake = random × sin(freq)
-- ===========================================================================

local backend = require("backend")
local layers  = require("layers")
local Operation = require("kag.operation")
local VFX = {}

local math_sin, math_abs, math_random = math.sin, math.abs, math.random
local DEFAULT_FRAME_MS = 16

local function next_frame_ms(is_coroutine)
    if not is_coroutine then return DEFAULT_FRAME_MS end
    return tonumber((coroutine.yield())) or DEFAULT_FRAME_MS
end

-- ===========================================================================
-- VFX.quake(ctx, params) -- BLOCKING random screen shake
-- params: { time=500, amplitudex=10, amplitudey=5, decay=true }
-- Spec [2.5]: offset_x = random(-ampl_x, +ampl_x) * decay
--             offset_y = random(-ampl_y, +ampl_y) * decay
-- ===========================================================================
function VFX.quake(ctx, params)
    local operation <close> = Operation.start(ctx or {})
    local ct = operation.token
    local affected = {}
    local function cleanup()
        for _, entry in ipairs(affected) do
            if entry.effect._operation == ct then
                entry.effect._operation = nil
                entry.effect.active = false
                entry.effect.offset_x, entry.effect.offset_y = 0, 0
                entry.node.dirty = true
            end
        end
        if VFX._quakeToken == ct then VFX._quakeToken, VFX._quakeActive = nil, false end
    end
    ct:register(cleanup)
    VFX._quakeToken = ct
    VFX._quakeActive = true
    local isCoroutine = coroutine.isyieldable()
    local dur_ms    = tonumber(params.time) or 500
    local ampl_x    = tonumber(params.amplitudex or params.intensity or params.strength or params.power) or 8
    local ampl_y    = tonumber(params.amplitudey or params.intensity or params.strength or params.power) or ampl_x or 8
    local decay     = params.decay ~= "false" and params.decay ~= false
    local elapsed_ms = 0

    -- Activate quake on all visible layers via the layers module
    layers.forEach(function(id, node)
        if node.visible then
            affected[#affected + 1] = {node=node, effect=node.quake}
            node.quake._operation = ct
            node.quake.active = true
            node.quake.amplitude_x = ampl_x
            node.quake.amplitude_y = ampl_y
        end
    end)

    while elapsed_ms < dur_ms do
        elapsed_ms = elapsed_ms + next_frame_ms(isCoroutine)
        if ct.cancelled or VFX._quakeToken ~= ct then return end
        local t = elapsed_ms / dur_ms
        if t > 1 then t = 1 end

        -- Spec [2.5]: random offset × linear decay
        local decay_factor = decay and (1.0 - t) or 1.0
        local ox = (math_random() * 2.0 - 1.0) * ampl_x * decay_factor
        local oy = (math_random() * 2.0 - 1.0) * ampl_y * decay_factor

        layers.forEach(function(id, node)
            if node.visible and node.quake.active and node.quake._operation == ct then
                node.quake.offset_x = ox
                node.quake.offset_y = oy
                node.dirty = true
            end
        end)
    end

    cleanup()
    operation:complete()
end

-- ===========================================================================
-- VFX.shake(ctx, params) -- BLOCKING sinusoidal screen shake
-- Spec [2.5]: random offset × sin(frequency)
-- params: { time=500, frequency=20, amplitude=6 }
-- ===========================================================================
function VFX.shake(ctx, params)
    local operation <close> = Operation.start(ctx or {})
    local ct = operation.token
    local affected = {}
    local function cleanup()
        for _, entry in ipairs(affected) do
            if entry.effect._operation == ct then
                entry.effect._operation = nil
                entry.effect.active = false
                entry.effect.offset_x, entry.effect.offset_y = 0, 0
                entry.node.dirty = true
            end
        end
        if VFX._shakeToken == ct then VFX._shakeToken, VFX._shakeActive = nil, false end
    end
    ct:register(cleanup)
    VFX._shakeToken = ct
    VFX._shakeActive = true
    local isCoroutine = coroutine.isyieldable()
    local dur_ms    = tonumber(params.time) or 500
    local freq      = tonumber(params.frequency) or 20
    local amplitude = tonumber(params.amplitude or params.intensity) or 6
    local elapsed_ms = 0

    -- Activate shake on all visible layers via the layers module
    layers.forEach(function(id, node)
        if node.visible then
            affected[#affected + 1] = {node=node, effect=node.shake}
            node.shake._operation = ct
            node.shake.active = true
        end
    end)

    while elapsed_ms < dur_ms do
        elapsed_ms = elapsed_ms + next_frame_ms(isCoroutine)
        if ct.cancelled or VFX._shakeToken ~= ct then return end
        local t = elapsed_ms / dur_ms
        if t > 1 then t = 1 end

        -- Spec [2.5]: random offset × sin(frequency) with decay
        local decay = 1.0 - t
        local elapsed_s = elapsed_ms / 1000.0
        local ox = (math_random() * 2.0 - 1.0) * amplitude * math_sin(elapsed_s * freq) * decay
        local oy = (math_random() * 2.0 - 1.0) * amplitude * math_sin(elapsed_s * freq * 1.3 + 1.0) * decay

        layers.forEach(function(id, node)
            if node.visible and node.shake.active and node.shake._operation == ct then
                node.shake.offset_x = ox
                node.shake.offset_y = oy
                node.dirty = true
            end
        end)
    end

    cleanup()
    operation:complete()
end

-- ===========================================================================
-- VFX.fade(ctx, params) -- BLOCKING global fullscreen fade
-- params: { time=1000, direction="out", r=0, g=0, b=0 }
-- ===========================================================================
function VFX.fade(ctx, params)
    local operation <close> = Operation.start(ctx)
    local ct = operation.token
    local isCoroutine = coroutine.isyieldable()
    local dur_ms    = tonumber(params.time) or 1000
    local direction = params.direction or params.dir or "out"
    local r = tonumber(params.r or params.red)   or 0
    local g = tonumber(params.g or params.green) or 0
    local b = tonumber(params.b or params.blue)  or 0
    local elapsed_ms = 0

    local texId = ctx._bgTexture or 0
    while elapsed_ms < dur_ms do
        elapsed_ms = elapsed_ms + next_frame_ms(isCoroutine)
        if ct.cancelled then return end
        local t = elapsed_ms / dur_ms
        if t > 1 then t = 1 end

        local alpha = (direction == "in") and (1.0 - t) or t
        if texId > 0 then
            backend.submit_vfx(texId, 1, alpha, r/255, g/255, b/255, 0, 0, 0)
        end
    end
    operation:complete()
end

-- ===========================================================================
-- VFX.fade_layer(ctx, layer, params) -- BLOCKING per-layer fade
-- params: { time=1000, direction="in" }
-- ===========================================================================
function VFX.fade_layer(ctx, layer, params)
    local operation <close> = Operation.start(ctx or {})
    local ct = operation.token
    layer._fadeOperation = ct
    local function cleanup()
        if layer._fadeOperation == ct then layer._fadeOperation = nil end
    end
    ct:register(cleanup)
    local isCoroutine = coroutine.isyieldable()
    local dur_ms    = tonumber(params.time) or 1000
    local direction = params.direction or params.dir or "in"
    local elapsed_ms = 0

    local origAlpha = layer.alpha or 1.0
    while elapsed_ms < dur_ms do
        elapsed_ms = elapsed_ms + next_frame_ms(isCoroutine)
        if ct.cancelled or layer._fadeOperation ~= ct then return end
        local t = elapsed_ms / dur_ms
        if t > 1 then t = 1 end

        layer.alpha = (direction == "in") and (origAlpha * t) or (origAlpha * (1 - t))
        layer.dirty = true
    end
    layer.alpha = (direction == "in") and origAlpha or 0
    layer.dirty = true
    cleanup()
    operation:complete()
end

-- ===========================================================================
-- VFX.blur(ctx, params) -- blocking blur of the composed scene
-- params: { time=300, amount=4 } (milliseconds, pixels)
-- ===========================================================================
local blur_owner
function VFX.blur(ctx, params)
    local duration = tonumber(params.time or params.duration) or 300
    local radius = tonumber(params.amount or params.strength or params.intensity or params.blurlevel) or 4
    if duration <= 0 or radius <= 0 or not coroutine.isyieldable() then return end

    local operation <close> = Operation.start(ctx)
    local ct = operation.token
    blur_owner = ct
    local function cleanup()
        if blur_owner == ct then
            blur_owner = nil
            backend.set_scene_blur(0)
        end
    end
    ct:register(cleanup)

    local elapsed = 0
    local frame_ms = next_frame_ms(true)
    repeat
        if ct.cancelled or blur_owner ~= ct then return end
        elapsed = elapsed + math.max(frame_ms, 0)
        if not backend.set_scene_blur(radius * math.min(elapsed / duration, 1), elapsed >= duration) then
            error("Scene blur could not be applied", 0)
        end
        -- Leave even the last radius set through the render that follows this update.
        frame_ms = next_frame_ms(true)
    until elapsed >= duration
    cleanup()
    operation:complete()
end

-- ===========================================================================
-- VFX.blur_layer(layer, params, co) -- Per-layer blur wrapper
-- Spec [2.5] kag.lua integration
-- ===========================================================================
function VFX.blur_layer(layer, params, co)
    if not layer or type(layer) ~= "table" then return end
    -- This legacy helper has no ctx argument; use the published session when
    -- driven by the engine, and retain scoped cleanup in standalone hosts.
    local operation <close> = Operation.start(rawget(_G, "_CAESURA_CTX") or {})
    local ct = operation.token
    local dur_ms   = tonumber(params.time) or 500
    local strength = tonumber(params.strength or params.blurlevel) or 4
    local elapsed_ms = 0
    local rtt = require("rtt")

    local tmpA, tmpB = 0, 0
    local function free_a()
        local id = tmpA; tmpA = 0
        if id and id > 0 then rtt.free(id) end
    end
    local function free_b()
        local id = tmpB; tmpB = 0
        if id and id > 0 then rtt.free(id) end
    end
    ct:register(free_a); ct:register(free_b)
    tmpA = rtt and rtt.alloc(1280, 720) or 0
    tmpB = rtt and rtt.alloc(1280, 720) or 0

    while elapsed_ms < dur_ms do
        if co and coroutine.isyieldable() then
            elapsed_ms = elapsed_ms + (tonumber((co.yield())) or DEFAULT_FRAME_MS)
        else
            elapsed_ms = elapsed_ms + DEFAULT_FRAME_MS
        end
        if ct.cancelled then return end
        local t = elapsed_ms / dur_ms
        if t > 1 then t = 1 end

        local radius = strength * t
        if layer.texture and layer.texture > 0 then
            backend.submit_vfx(layer.texture, 2, 0, 0, 0, 0, radius, 0, 0)
        end
        layer.dirty = true
    end

    free_a(); free_b()
    operation:complete()
end

-- ===========================================================================
-- VFX.flash(ctx, params) -- BLOCKING white/colored flash
-- params: { time=200, r=255, g=255, b=255 }
-- ===========================================================================
-- Byte clamp for flash color components (security info: vfx flash fed
-- script numbers straight to create_solid_texture -- "999" reached the
-- backend unclamped; same pattern as the textbox clamp audit).
local function clamp_byte(v)
    v = math.floor(tonumber(v) or 0)
    return math.max(0, math.min(255, v))
end

function VFX.flash(ctx, params)
    local operation <close> = Operation.start(ctx)
    local ct = operation.token
    local isCoroutine = coroutine.isyieldable()
    local dur_ms = tonumber(params.time) or 200
    local r = clamp_byte(params.r or params.red or 255)
    local g = clamp_byte(params.g or params.green or 255)
    local b = clamp_byte(params.b or params.blue or 255)
    local elapsed_ms = 0

    -- Create flash layer if not exists
    local flashLayer = ctx._flashLayer
    if not flashLayer then
        flashLayer = { name="__flash", z=9998, visible=true, dirty=true, _prevTexture=0,
                       x=0, y=0, w=1280, h=720, alpha=0, blend="additive" }
        ctx._flashLayer = flashLayer
        ctx.layers = ctx.layers or {}
        ctx.layers["__flash"] = flashLayer
    end
    flashLayer._operation = ct
    local function cleanup()
        if flashLayer._operation == ct then
            flashLayer._operation = nil
            flashLayer.alpha = 0
            flashLayer.dirty = true
        end
    end
    ct:register(cleanup)

    local midPoint_ms = dur_ms / 2

    while elapsed_ms < dur_ms do
        elapsed_ms = elapsed_ms + next_frame_ms(isCoroutine)
        if ct.cancelled or flashLayer._operation ~= ct then return end

        local alpha_val
        if elapsed_ms < midPoint_ms then
            alpha_val = elapsed_ms / midPoint_ms
        else
            alpha_val = (dur_ms - elapsed_ms) / midPoint_ms
        end
        if alpha_val < 0 then alpha_val = 0 end
        if alpha_val > 1 then alpha_val = 1 end

        flashLayer.alpha = alpha_val * 0.5
        flashLayer.dirty = true

        if not flashLayer.texture then
            pcall(function()
                flashLayer.texture = backend.create_solid_texture(r, g, b, 255)
            end)
        end
    end

    cleanup()
    operation:complete()
end

-- ===========================================================================
-- VFX.stop_quake() -- immediately terminate quake and restore positions
-- ===========================================================================
function VFX.stop_quake()
    if VFX._quakeToken then VFX._quakeToken:cancel() end
    VFX._quakeActive = false
    layers.forEach(function(id, node)
        if node.quake.active then
            node.quake.active = false
            node.quake.offset_x = 0
            node.quake.offset_y = 0
            node.dirty = true
        end
    end)
end

-- ===========================================================================
-- VFX.stop_shake() -- immediately terminate sinusoidal shake
-- ===========================================================================
function VFX.stop_shake()
    if VFX._shakeToken then VFX._shakeToken:cancel() end
    VFX._shakeActive = false
    layers.forEach(function(id, node)
        if node.shake.active then
            node.shake.active = false
            node.shake.offset_x = 0
            node.shake.offset_y = 0
            node.dirty = true
        end
    end)
end

-- ===========================================================================
-- VFX.stop_all() -- immediately terminate all active VFX
-- ===========================================================================
function VFX.stop_all()
    VFX.stop_quake()
    VFX.stop_shake()
    if blur_owner then blur_owner:cancel() end
end

return VFX
