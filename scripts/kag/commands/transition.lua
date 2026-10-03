-- =============================================================================
--  Caesura (AmeKAG) — kag/commands/transition.lua
--  Phase 4: KAG transition tag handlers — [trans], [move], [quake], [fade]
--  Wires KAG tags to the GPU-backed transition.lua engine (crossfade/wipe/rule).
--  All coroutine-based for blocking semantics with CancelToken support.
-- =============================================================================

local Operation   = require("kag.operation")
local Transition  = require("transition")
local backend     = require("backend")
local layers      = require("layers")


-- ═══════════════════════════════════════════════════════════════════════════
--  [10.2.25] LUT (Look-Up Table) cache — one texture, progress uniform
--  Pre-generate easing LUT textures; never create per-frame.
--  Shader uses progress uniform + hardcoded easing to interpolate.
-- ═══════════════════════════════════════════════════════════════════════════

local LUTCache = {
    _textures = {},   -- easing_name → texture handle
    _generated = false,
}

--- Pre-generate all LUT textures at init (called once from kag/init.lua)
function LUTCache.pregen()
    if LUTCache._generated then return end
    -- Standard easing LUTs: 256x1 R8 textures
    -- Each texel = precomputed easing(t/255)
    local easings = { "linear", "ease_in", "ease_out", "ease_in_out",
                      "cubic_in", "cubic_out", "cubic_in_out",
                      "quart_in", "quart_out", "back_in", "back_out", "elastic_out" }
    for _, name in ipairs(easings) do
        local tex = backend.create_lut_texture and backend.create_lut_texture(name)
        if tex then
            LUTCache._textures[name] = tex
        end
    end
    LUTCache._generated = true
    print("[LUT] Pre-generated " .. #easings .. " easing LUT textures.")
end

--- Get LUT texture handle by easing name (cached, no per-frame creation)
function LUTCache.get(name)
    return LUTCache._textures[name or "linear"]
end

--- Release all LUT textures
function LUTCache.release()
    for name, tex in pairs(LUTCache._textures) do
        if tex then backend.destroy_texture(tex) end
    end
    LUTCache._textures = {}
    LUTCache._generated = false
end

-- ═══════════════════════════════════════════════════════════════════════════
--  [10.2.41] Cubic Bezier easing — control point interpolation
--  P0=(0,0), P1=(cp1x,cp1y), P2=(cp2x,cp2y), P3=(1,1)
--  Evaluates Y at given X using de Casteljau / Newton-Raphson.
-- ═══════════════════════════════════════════════════════════════════════════

local Bezier = {}

--- Evaluate cubic bezier at parameter t
--- cp1x,cp1y: first control point (relative to start 0,0)
--- cp2x,cp2y: second control point (relative to end 1,1)
--- Returns (x, y) at parameter t
function Bezier.eval(cp1x, cp1y, cp2x, cp2y, t)
    local u = 1 - t
    local tt = t * t
    local uu = u * u
    local uuu = uu * u
    local ttt = tt * t
    local x = uuu * 0 + 3 * uu * t * cp1x + 3 * u * tt * cp2x + ttt * 1
    local y = uuu * 0 + 3 * uu * t * cp1y + 3 * u * tt * cp2y + ttt * 1
    return x, y
end

--- Solve bezier Y at given X using Newton-Raphson (8 iterations)
function Bezier.solve_y(cp1x, cp1y, cp2x, cp2y, x)
    local t = x  -- initial guess
    for _ = 1, 8 do
        local bx, by = Bezier.eval(cp1x, cp1y, cp2x, cp2y, t)
        -- Derivative dx/dt
        local u = 1 - t
        local dx = 3 * u * u * cp1x + 6 * u * t * (cp2x - cp1x) + 3 * t * t * (1 - cp2x)
        if math.abs(dx) < 0.0001 then break end
        t = t - (bx - x) / dx
        t = math.max(0, math.min(1, t))
    end
    local _, y = Bezier.eval(cp1x, cp1y, cp2x, cp2y, t)
    return y
end

-- Preset bezier curves matching common CSS easing
Bezier.PRESETS = {
    ease        = { 0.25, 0.1, 0.25, 1.0 },
    ["ease-in"] = { 0.42, 0.0, 1.0,  1.0 },
    ["ease-out"]= { 0.0,  0.0, 0.58, 1.0 },
    ["ease-in-out"] = { 0.42, 0.0, 0.58, 1.0 },
    linear      = { 0.0, 0.0, 1.0, 1.0 },
}

--- Apply a named bezier preset to a progress value t
function Bezier.apply(preset_name, t)
    local p = Bezier.PRESETS[preset_name or "linear"]
    if not p then return t end
    return Bezier.solve_y(p[1], p[2], p[3], p[4], t)
end


-- Round 51 contracts: [blur]/[fade] (audit: handlers lacked schemas).
local _schema2 = require("kag.schema")
_schema2.define("blur", {
    _meta = { category = "transition", blocking = true, desc = "gaussian blur the whole frame" },
    amount = { type = "number", default = 0, min = 0, max = 100 },
    time = { type = "number", default = 300, min = 0, max = 30000 },
    duration = { type = "number", default = 300, min = 0, max = 30000 },
})
_schema2.define("fade", {
    _meta = { category = "transition", blocking = true, desc = "fade a layer between from/to opacity" },
    layer = { type = "string", default = "fg" },
    from = { type = "number", default = 0, min = 0, max = 255 },
    to = { type = "number", default = 255, min = 0, max = 255 },
    time = { type = "number", default = 500, min = 0, max = 30000, aliases = {"duration"} },
    duration = { type = "number", default = 500, min = 0, max = 30000 },
})

local TransCommands = {}

local function find_layer(layerName)
    return layers.find(layerName) or layers.get(layerName)
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Internal: helper to resolve transition method from KAG params
-- ═══════════════════════════════════════════════════════════════════════════

local function resolve_method(params)
    -- Keep omitted aliases absent through schema coercion. Only explicitly
    -- supplied values compete: canonical method, then type, then legacy kind.
    local kind = params.method or params.type or params.kind or "crossfade"
    local map  = {
        crossfade = "crossfade",
        rule      = "rule",
        wipe      = "wipe",
        universal   = "crossfade",
        scroll      = "wipe",
        ["scroll_right"] = "wipe",
        -- KiriKiri-style aliases: dissolve/gradient are crossfade variants,
        -- mask is a rule-image transition, slide is a directional wipe.
        dissolve    = "crossfade",
        gradient    = "crossfade",
        mask        = "rule",
        slide       = "wipe",
        fade        = "crossfade",
    }
    return map[kind] or "crossfade"
end

local function resolve_direction(params)
    local dir = params.direction or params.dir or "left"
    local map = {
        left   = "left",   right  = "right",
        top    = "top",    bottom = "bottom",
        up     = "top",    down   = "bottom",
    }
    return map[dir] or "left"
end

-- ═══════════════════════════════════════════════════════════════════════════
--  [trans time=500 kind="crossfade" rule="rule_01.png"]
--  GPU-backed scene transition. Blocks coroutine until complete.
--  Spec [2.3] [10.2.33]: CancelToken, yield-based blocking.
--  Option: wait_preload=true (default) blocks until transition-slot
--  preloaded textures are ready.
-- ═══════════════════════════════════════════════════════════════════════════

-- [scroll text=... speed=60] — vertical scrolling text (KiriKiri ED credits).
-- Coroutine yield loop (same pattern as VFX.flash): draws the text at an
-- upward-shifting y each frame until it exits the viewport, then clears.
-- Neo-Genesis contracts: typed + clamped via kag/schema (replaces inline
-- tonumber fallbacks; contract violations report cmd@scene:token).
local schema = require("kag.schema")
schema.define("scroll", {
    _meta = { category = "transition", blocking = false, desc = "KAG3-compatible scroll command" },
    text  = { type = "string", default = "", positional_index = 1 },
    speed = { type = "number", default = 60, min = 1, max = 1000 },
    size  = { type = "number", default = 28, min = 8, max = 128 },
    color = { type = "string", default = "white" },
})
schema.define("trans", {
    _meta = { category = "transition", blocking = true, desc = "KAG3-compatible trans command" },
    method = { type = "string" }, -- runtime default crossfade after alias resolution
    type = { type = "string" },   -- KAG3 alias
    kind = { type = "string" },   -- legacy alias
    time   = { type = "number", min = 0, max = 30000 },
    duration = { type = "number", min = 0, max = 30000 }, -- default 500 after alias resolution
})
schema.define("move", {
    _meta = { category = "transition", blocking = true, desc = "KAG3-compatible move command" },
    x = { type = "number", default = 0 },
    y = { type = "number", default = 0 },
    time = { type = "number", default = 300, min = 0, max = 30000, aliases = {"duration"} },
    duration = { type = "number", default = 300, min = 0, max = 30000 },
    layer = { type = "string", default = "fg", aliases = {"name"} },  -- KAG3 layer name
    name = { type = "string", default = "" },
})
schema.define("quake", {
    _meta = { category = "transition", blocking = true, desc = "KAG3-compatible quake command" },
    time = { type = "number", default = 300, min = 0, max = 30000, aliases = {"duration"} },
    duration = { type = "number", default = 300, min = 0, max = 30000 },
    intensity = { type = "number", default = 5, min = 0, max = 100, aliases = {"amplitude"} },
    amplitude = { type = "number", default = 5, min = 0, max = 100 },
})

function TransCommands.scroll(ctx, params)
    local text = params.text or params[1] or ""
    local speed = params.speed  -- schema-typed
    local size = params.size
    local color = params.color
    local vw, vh = require("viewport").logical(1920, 1080)  -- viewport-relative fallback (was 1280/720)
    local width = (ctx.viewport and ctx.viewport.width) or vw
    local height = (ctx.viewport and ctx.viewport.height) or vh
    if #text == 0 then return end

    -- Resolve the color to components: "r,g,b" string or named.
    -- render_text's real contract is (text, x, y, r, g, b, a) --
    -- the old call passed (text, size, 32, y, color), silently
    -- shifting every argument (audit: size became x, the color
    -- string became a byte channel and clamped to 0).
    local named_colors = {
        white = "255,255,255", black = "0,0,0", red = "255,0,0",
        green = "0,255,0", blue = "0,0,255", yellow = "255,255,0",
        gray = "128,128,128", grey = "128,128,128",
    }
    if type(color) == "string" then
        local key = color:lower():gsub("%s", "")
        if named_colors[key] then color = named_colors[key] end
    end
    local cr, cg, cb = 255, 255, 255
    if type(color) == "string" then
        local r0, g0, b0 = color:match("(%d+),%s*(%d+),%s*(%d+)")
        if r0 then
            cr = tonumber(r0) or 255
            cg = tonumber(g0) or 255
            cb = tonumber(b0) or 255
        end
    end
    -- The scroll size applies via the font state (pcall: headless
    -- test environments have no Render binding)
    pcall(backend.text_set_font, "default", size, color)

    -- Multi-line ED credits: split on newline and render each line
    -- with a line stride so the whole block scrolls as one unit
    -- (audit: the old loop rendered only the first line).
    local lines = {}
    local text_clean = text:gsub("\n+$", "")
    local text_with_nl = text_clean .. "\n"
    for line in text_with_nl:gmatch("(.-)\n") do
        lines[#lines + 1] = line
    end
    if #lines == 0 then lines = { text_clean } end
    local lineHeight = size + 10
    local blockHeight = #lines * lineHeight

    local y = height
    local lifetime = 0
    local duration = (height + blockHeight + 64) / math.max(1, speed)
    while lifetime < duration do
        local dt = (coroutine.yield() or 16) / 1000.0
        lifetime = lifetime + dt
        y = y - speed * dt
        for i, line in ipairs(lines) do
            backend.render_text(line, 32, y + (i - 1) * lineHeight,
                                cr, cg, cb, 255)
        end
    end
    backend.clear_text()
end

-- [flash time=200 r=255 g=255 b=255] — full-screen color flash
-- (KiriKiri classic white-flash / colored flash effect). Non-blocking:
-- VFX.flash runs its own coroutine-yield loop and returns.
function TransCommands.flash(ctx, params)
    local VFX = require("vfx")
    if VFX.flash then
        VFX.flash(ctx, params)
    end
end

-- [blur time=500 strength=2.0] — full-screen blur effect
-- (KAG3-adjacent visual effect; VFX.blur existed but had no command).
function TransCommands.blur(ctx, params)
    local VFX = require("vfx")
    if VFX.blur then
        VFX.blur(ctx, params)
    end
end

function TransCommands.trans(ctx, params)
    local dur = tonumber(params.time or params.duration) or 500
    local ResourceCommands = require("kag.commands.resource")
    if dur <= 0 then
        Transition.cancel_pending(ctx)
        Transition.cancel()
        ResourceCommands.promote_transition_slot()
        return
    end

    -- Own cancellation before the first capture: standalone startup can need a
    -- genuine render epoch even without the scheduler's adjacent-bg lease.
    local operation <close> = Operation.start(ctx)
    local ct = operation.token
    local captureOwner = {closed=false}
    local function close_capture_wait()
        captureOwner.closed = true
        Transition.clear_render_wait(ctx, captureOwner)
    end
    ct:register(close_capture_wait)
    local captureScope <close> = setmetatable({}, {__close=close_capture_wait})

    -- Acquire A before any preload yield can render the changed scene B.
    -- Snapshots are numeric native-owned leases, not pooled empty RTTs.
    local fromTex, fromFrame = Transition.take_pending(ctx)
    local prepared = fromTex ~= nil
    if not prepared then
        Transition.cancel()
        fromTex, fromFrame = Transition.capture_after_render(ctx, captureOwner)
        if not fromTex then return end
    end
    local toTex, tx
    local released = false
    local function release()
        if released then return end
        released = true
        if tx then Transition.clear_render_wait(ctx, tx) end
        require("rtt").destroy(fromTex)
        if toTex then require("rtt").destroy(toTex) end
    end
    local lease <close> = setmetatable({}, {__close = function()
        if tx and tx.status ~= Transition.Status.COMPLETED then Transition.cancel(tx) end
        release()
    end})
    local config = {method = resolve_method(params), duration = dur,
        direction = resolve_direction(params) or "left"}
    local rule = params.rule or params.rule_image
    if config.method == "rule" then
        config.rule_tex = Transition.preload_rule(rule)
        assert(config.rule_tex, "rule transition requires a loaded rule image")
    end
    tx = Transition.start(fromTex, fromTex, config)
    tx.release = release
    ct:register(function() Transition.cancel(tx) end)
    local function hold_and_yield()
        if ct.cancelled or not Transition.is_active(tx) then return false end
        Transition.tick(0, tx) -- hold A over the newly rendered scene (no B flash)
        Transition.wait_for_render(ctx, tx)
        return not ct.cancelled and Transition.is_active(tx)
    end
    local waited = 0
    while ResourceCommands.has_pending_transition() and waited < 5000 do
        if not hold_and_yield() then return end
        waited = waited + 16
    end
    ResourceCommands.promote_transition_slot()
    if params.wait_preload ~= false then
        while ctx._preloadPending do
            if not hold_and_yield() then return end
        end
    end
    -- Exactly a real frame boundary separates the two native scene copies.
    if not prepared and not hold_and_yield() then return end
    local toFrame
    toTex, toFrame = Transition.capture_screen()
    assert(toFrame > fromFrame, "transition destination has not been rendered")
    tx.to_tex = toTex
    Transition.tick(0, tx)
    while not ct.cancelled and Transition.is_active(tx) do
        local dt = Transition.wait_for_render(ctx, tx) or 16
        if ct.cancelled or not Transition.is_active(tx) then break end
        Transition.tick(dt, tx)
    end
    if not ct.cancelled and tx.status == Transition.Status.COMPLETED then
        operation:complete()
    end
end

-- ═══════════════════════════════════════════════════════════════════════════
--  [move layer="fg" x=200 y=100 time=300 path="(0,0)(100,50)(200,100)"]
--  Animate layer position using ease-in-out bezier.
--  Spec [10.2.41]: cubic bezier + easing.
--  Blocks coroutine until animation complete. Cancelable.
-- ═══════════════════════════════════════════════════════════════════════════

function TransCommands.move(ctx, params)
    local dur      = params.time or params.duration or 300
    local targetX  = params.x or 0
    local targetY  = params.y or 0
    local layerName= params.layer or "fg"
    local easing   = params.easing or "ease-in-out"

    local node = find_layer(layerName)
    if not node then
        print("[TransCmd] move: layer not found: " .. layerName)
        return
    end

    if dur <= 0 then
        layers.move_layer(node, targetX, targetY)
        return
    end

    local operation <close> = Operation.start(ctx)
    local ct = operation.token

    local startX = node.x or 0
    local startY = node.y or 0
    local elapsed = 0

    while elapsed < dur and not ct.cancelled do
        local dt = coroutine.yield() or 16
        if ct.cancelled then break end
        elapsed = elapsed + dt
        local t = math.min(elapsed / dur, 1.0)

        -- Apply easing
        if easing == "ease-in" then
            t = t * t
        elseif easing == "ease-out" then
            t = 1 - (1 - t) * (1 - t)
        elseif easing == "ease-in-out" then
            t = t < 0.5 and (2 * t * t) or (1 - (-2 * t + 2)^2 / 2)
        end

        local curX = startX + (targetX - startX) * t
        local curY = startY + (targetY - startY) * t
        layers.move_layer(node, curX, curY)
    end

    if not ct.cancelled then
        layers.move_layer(node, targetX, targetY)
        operation:complete()
    end
end

-- ═══════════════════════════════════════════════════════════════════════════
--  [quake time=500 intensity=5 count=10]
--  Screen shake effect via periodic random offsets.
--  Spec [10.2.42]: blocks during quake, click suppressed.
--  Blocks coroutine until complete. Cancelable.
-- ═══════════════════════════════════════════════════════════════════════════

-- [vib] -- message-layer vibration (KiriKiri text vib standard).
-- Shakes the message layer's offset like quake but scoped to the text
-- window; classic emphasis effect for dialogue.
schema.define("vib", {
    _meta = { category = "transition", blocking = true, desc = "KAG3-compatible vib command" },
    time = { type = "number", default = 300, min = 0, max = 30000 },
    intensity = { type = "number", default = 3, min = 0, max = 50 },
    amplitude = { type = "number", min = 0, max = 50 },
})

function TransCommands.vib(ctx, params)
    local dur = params.time or 300
    local amp = params.amplitude or params.intensity or 3
    if dur <= 0 then return end
    local layers = require("layers")
    local msg = layers.get_layer("message") or layers.find("message")
    if not msg then return end
    local operation <close> = require("kag.operation").start(ctx)
    local ct = operation.token
    ct:register(function()
        if msg then msg.x, msg.y = msg.base_x or 0, msg.base_y or 0 end
    end)
    local baseX, baseY = msg.x or 0, msg.y or 0
    msg.base_x, msg.base_y = baseX, baseY
    local elapsed = 0
    while elapsed < dur and not ct.cancelled do
        elapsed = elapsed + (coroutine.yield() or 16)
        local t = elapsed / math.max(1, dur)
        local wob = math.sin(elapsed * 0.05) * amp * (1 - t)
        msg.x = baseX + wob
        msg.y = baseY + (math.cos(elapsed * 0.07) * amp * 0.5) * (1 - t)
        layers.mark_dirty(msg)
    end
    if not ct.cancelled then
        msg.x, msg.y = baseX, baseY
        operation:complete()
    end
end

-- [camera] -- screen-offset pan (cinematic emphasis). KAG3 needed TJS
-- extension glue for a smooth camera drift; this animates the engine's
-- screen offset from (0,0) to (x,y) and back, cancel-safe.
schema.define("camera", {
    _meta = { category = "transition", blocking = false, desc = "KAG3-compatible camera command" },
    x = { type = "number", default = 0, min = 0, max = 2000 },  -- rect path pans right/down
    y = { type = "number", default = 0, min = 0, max = 2000 },
    time = { type = "number", default = 500, min = 0, max = 30000 },
    restore = { type = "boolean", default = true },
})

function TransCommands.camera(ctx, params)
    local toX, toY = params.x, params.y
    local dur = params.time
    if dur <= 0 then
        backend.set_screen_offset(toX, toY)
        return
    end
    local operation <close> = Operation.start(ctx)
    local ct = operation.token
    ct:register(function() backend.set_screen_offset(0, 0) end)
    local elapsed = 0
    while elapsed < dur and not ct.cancelled do
        local dt = coroutine.yield() or 16
        if ct.cancelled then break end
        elapsed = elapsed + dt
        local t = math.min(1, elapsed / dur)
        -- ease-in-out
        local e = t < 0.5 and 2 * t * t or 1 - (-2 * t + 2)^2 / 2
        backend.set_screen_offset(toX * e, toY * e)
    end
    if not ct.cancelled then
        if params.restore ~= false then
            backend.set_screen_offset(0, 0)  -- drift back after the beat
        else
            backend.set_screen_offset(toX, toY)  -- hold the offset
        end
        operation:complete()
    end
end

function TransCommands.quake(ctx, params)
    local dur       = params.time or params.duration or 300
    local intensity = params.intensity or params.amplitude or 5
    local freq      = tonumber(params.freq or params.frequency or 0.05)

    if dur <= 0 then
        backend.set_screen_offset(0, 0)
        return
    end

    local operation <close> = Operation.start(ctx)
    local ct = operation.token
    ct:register(function() backend.set_screen_offset(0, 0) end)

    local elapsed = 0
    while elapsed < dur and not ct.cancelled do
        local dt = coroutine.yield() or 16
        if ct.cancelled then break end
        elapsed = elapsed + dt

        -- Decaying amplitude
        local progress = elapsed / dur
        local decay = 1.0 - progress  -- linear decay
        local curIntensity = intensity * decay

        -- Random offset with current intensity
        local ox = (math.random() - 0.5) * 2 * curIntensity
        local oy = (math.random() - 0.5) * 2 * curIntensity
        backend.set_screen_offset(ox, oy)
    end

    if not ct.cancelled then
        backend.set_screen_offset(0, 0)
        operation:complete()
    end
end

-- ═══════════════════════════════════════════════════════════════════════════
--  [fade layer="bg" time=1000 from=255 to=0]
--  Fade a layer's opacity over time. Blocks coroutine. Cancelable.
--  Default: fade in (0→255). from/to control direction.
-- ═══════════════════════════════════════════════════════════════════════════

function TransCommands.fade(ctx, params)
    local dur       = tonumber(params.time or params.duration or 500)
    local layerName = params.layer or "fg"
    local from      = tonumber(params.from) or 0
    local to        = tonumber(params.to)   or 255

    local node = find_layer(layerName)
    if not node then
        print("[TransCmd] fade: layer not found: " .. layerName)
        return
    end

    if dur <= 0 then
        layers.set_layer_opacity(node, to)
        return
    end

    local operation <close> = Operation.start(ctx)
    local ct = operation.token
    layers.set_layer_opacity(node, from)

    local elapsed = 0
    while elapsed < dur and not ct.cancelled do
        local dt = coroutine.yield() or 16
        if ct.cancelled then break end
        elapsed = elapsed + dt
        local t = math.min(elapsed / dur, 1.0)
        local curOpacity = from + (to - from) * t
        layers.set_layer_opacity(node, curOpacity)
    end

    if not ct.cancelled then
        layers.set_layer_opacity(node, to)
        operation:complete()
    end
end

TransCommands.LUTCache = LUTCache
TransCommands.Bezier = Bezier

return TransCommands
