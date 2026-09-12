-- =============================================================================
--  Caesura (AmeKAG) — kag/commands/vfx.lua
--  VFX command handler: [vfx type="..." ...]
--  Routes to particle system, quake/shake/flash/fade/blur via vfx.lua module.
-- =============================================================================

local VFX = require("vfx")
local backend = require("backend")

local VFXCommands = {}

local function valid_postfx_result(value)
    -- Native PostFxHandle: positive uint32; Web may report an applied ACK.
    return value == true or (type(value) == "number" and value > 0
        and value <= 4294967295 and value % 1 == 0)
end

local function valid_particle_id(value)
    -- Native emitters use an int vector index, so zero is a valid owner.
    return type(value) == "number" and value >= 0 and value <= 2147483647
        and value % 1 == 0
end

-- ═══════════════════════════════════════════════════════════════════════════
--  [vfx type="particle" action="create" x=0 y=0 rate=10 lifeMin=0.5 lifeMax=2.0]
--  [vfx type="particle" action="emit" emitter=0 count=10]
--  [vfx type="particle" action="destroy" emitter=0]
--  [vfx type="particle" action="clear"]
--  [vfx type="quake" time=500 amplitudex=10 amplitudey=5]
--  [vfx type="shake" time=500 frequency=20 amplitude=6]
--  [vfx type="flash" time=200 r=255 g=255 b=255]
--  [vfx type="fade" time=500 r=0 g=0 b=0]
--  [vfx type="blur" time=500 strength=4]
--  [palette effect="apply|clear|day|night|toggle|unload" ...]  -- LUT color grading
--  [vibrate time=300 intensity=3 ...]  -- KAG3 alias for [vib]
-- ═══════════════════════════════════════════════════════════════════════════

-- Neo-Genesis contracts: typed + clamped via kag/schema.
local schema = require("kag.schema")
-- Standalone [flash]/[shake]/[quake] (KAG3 names; routed to VFX).
schema.define("flash", {
    _meta = { category = "vfx", blocking = true, desc = "KAG3-compatible flash command" },
    r = { type = "number", default = 255, min = 0, max = 255 },
    g = { type = "number", default = 255, min = 0, max = 255 },
    b = { type = "number", default = 255, min = 0, max = 255 },
    time = { type = "number", default = 200, min = 0, max = 10000 },
})
schema.define("shake", {
    _meta = { category = "vfx", blocking = true, desc = "KAG3-compatible shake command" },
    time = { type = "number", default = 500, min = 0, max = 10000 },
    frequency = { type = "number", default = 20, min = 1, max = 120 },
    amplitude = { type = "number", default = 6, min = 0, max = 100 },
})
-- NOTE: [quake] has NO schema here -- transition.lua owns the quake
-- contract (time/duration/intensity/amplitude); re-defining it would
-- silently override that richer contract (schema.define overwrites).
schema.define("particles", {
    _meta = { category = "vfx", blocking = false, desc = "KAG3-compatible particles command" },
    x = { type = "number", default = 0 },
    y = { type = "number", default = 0 },
    rate = { type = "number", default = 10, min = 0, max = 1000 },
    lifeMin = { type = "number", default = 0.5, min = 0, max = 60 },
    life_max = { type = "number", default = 0.5, min = 0, max = 60 },
    lifeMax = { type = "number", default = 2.0, min = 0, max = 60 },
    speedMin = { type = "number", default = 10, min = 0, max = 10000 },
    speed_max = { type = "number", default = 10, min = 0, max = 10000 },
    speedMax = { type = "number", default = 50, min = 0, max = 10000 },
    angleMin = { type = "number", default = 0 },
    angle_max = { type = "number", default = 0 },
    angleMax = { type = "number", default = 6.283 },
    sizeMin = { type = "number", default = 2, min = 0, max = 512 },
    size_max = { type = "number", default = 2, min = 0, max = 512 },
    sizeMax = { type = "number", default = 8, min = 0, max = 512 },
    r = { type = "number", default = 1, min = 0, max = 255 },
    red = { type = "number", default = 1, min = 0, max = 255 },
    g = { type = "number", default = 1, min = 0, max = 255 },
    green = { type = "number", default = 1, min = 0, max = 255 },
    b = { type = "number", default = 1, min = 0, max = 255 },
    blue = { type = "number", default = 1, min = 0, max = 255 },
    a = { type = "number", default = 1, min = 0, max = 255 },
    alpha = { type = "number", default = 1, min = 0, max = 255 },
})

schema.define("postprocess", {
    _meta = { category = "vfx", blocking = false, desc = "Apply post-processing full-screen shader effect" },
    effect    = { type = "enum", values = { ["bloom"] = true, ["vignette"] = true, ["lut"] = true, ["softblur"] = true, ["off"] = true, ["none"] = true }, required = true },
    -- intensity/strength are ALIASES for the same PostFx knob. Neither carries
    -- a schema default: filling one in makes the other unreachable, because the
    -- handler cannot then tell "author omitted it" from "schema supplied it".
    -- The WIP had default=1.0 on intensity, so [postprocess effect="bloom"
    -- strength=0.25] silently rendered at 1.0. The 1.0 fallback now lives in
    -- apply_postfx, the single place that builds PostFxParams -- same reasoning
    -- as the validation-only [vfx] schema documented above.
    intensity = { type = "number", min = 0.0, max = 1.0 },
    strength  = { type = "number", min = 0.0, max = 1.0 },
    radius    = { type = "number", default = 0.0, min = 0.0, max = 64.0 },
    amount    = { type = "number", default = 0.0, min = 0.0, max = 1.0 },
    -- lutMix default 0 (NOT 1.0): it must agree with the ONE fallback in
    -- apply_postfx, with the legacy [vfx postfx=lut] path (validation-only
    -- schema -> no default -> 0) and with the C++ side
    -- (RenderBinding.cpp resolvePostFxParams lutMix default 0.0f, pinned by
    -- tests/cpp/test_render_postfx.cpp:60). The WIP declared 1.0 here while
    -- [vfx] resolved 0, so the SAME effect graded fully through one command
    -- and not at all through the other. Consequence of picking 0: a bare
    -- [postprocess effect="lut"] is a visual no-op -- lut needs an explicit
    -- lutMix, exactly as [vfx postfx="lut" lutMix=0.8] already does.
    lutMix    = { type = "number", default = 0.0, min = 0.0, max = 1.0 },
    r         = { type = "number", default = 255, min = 0, max = 255 },
    g         = { type = "number", default = 255, min = 0, max = 255 },
    b         = { type = "number", default = 255, min = 0, max = 255 },
    rgb       = { type = "string" },
})

schema.define("postprocess_off", {
    _meta = { category = "vfx", blocking = false,
        desc = "Disable post-processing: the whole chain, or one stage with effect=" },
    -- Bare [postprocess_off] clears everything (backend.clear_postfx).
    -- effect=<kind> tears down one stage (backend.destroy_postfx) so a scene
    -- can drop its vignette while keeping bloom. "all" is spelled explicitly
    -- for symmetry with [particle_weather type="all"].
    effect = { type = "enum", values = {
        ["bloom"] = true, ["vignette"] = true, ["lut"] = true,
        ["softblur"] = true, ["all"] = true,
    } },
})

schema.define("particle_weather", {
    _meta = { category = "vfx", blocking = false, desc = "Declarative weather atmosphere particle system" },
    -- NO default on type: the handler resolves a missing type to "rain" for
    -- action="start", but for action="stop" a missing type means "stop every
    -- weather emitter". A schema default of "rain" made that unreachable --
    -- [particle_weather action="stop"] arrived at the handler as
    -- type="rain" and only ever stopped the rain, silently leaving snow and
    -- sakura running. (The handler's stop-all branch was already written and
    -- was dead code through the real KAG dispatch path; only direct
    -- VFXCommands calls in tests reached it.)
    type      = { type = "enum", values = { ["rain"] = true, ["snow"] = true, ["sakura"] = true, ["dust"] = true, ["all"] = true } },
    action    = { type = "enum", values = { ["start"] = true, ["stop"] = true, ["clear"] = true }, default = "start" },
    count     = { type = "number", default = 10, min = 0, max = 500 },
    wind      = { type = "number", default = 0, min = -10, max = 10 },
    speed     = { type = "number", default = 1.0, min = 0.1, max = 10.0 },
    intensity = { type = "number", default = 1.0, min = 0.0, max = 5.0 },
})


-- [vfx] wrapper contract (round 102). NOTE: this schema is VALIDATION-ONLY
-- (no defaults on shared fields) so migrating [vfx] never changes legacy
-- effect behavior: the handlers resolve <name> or <alias> or <literal>
-- fallbacks from nil params, and filling in a default for one shared name
-- (e.g. strength used by both blur and the postfx chain, amplitude vs
-- intensity) would silently alter existing scripts. postfx is an enum
-- (choices validated by ks_check/LSP) and the numeric clamps apply.
schema.define("vfx", {
    _meta = { category = "vfx", blocking = false,
        desc = "GPU visual effects: particles, quake/shake/flash/fade/blur, and PostFx chain (postfx=)" },
    type       = { type = "string" },
    effect     = { type = "string" }, -- legacy alias of type
    action     = { type = "string" },
    direction  = { type = "string" },
    layer      = { type = "string" },
    color      = { type = "string" },
    rgb        = { type = "string" },  -- "r,g,b" (0..255 each)
    -- PostFx chain (round 102): kind choices + numeric clamps.
    postfx   = { type = "enum", values = {
        ["bloom"] = true, ["vignette"] = true, ["lut"] = true,
        ["softblur"] = true, ["none"] = true,
    } },
    strength = { type = "number", min = 0, max = 255 }, -- postfx 0..1 / blur px
    radius   = { type = "number", min = 0, max = 64 },
    amount   = { type = "number", min = 0, max = 1 },
    lutMix   = { type = "number", min = 0, max = 1 },
    -- Legacy effect params (typed + clamped; no defaults -- see above)
    time       = { type = "number", min = 0, max = 30000 },
    intensity  = { type = "number", min = 0, max = 50 },
    frequency  = { type = "number", min = 1, max = 120 },
    amplitude  = { type = "number", min = 0, max = 100 },
    amplitudex = { type = "number", min = 0, max = 200 },
    amplitudey = { type = "number", min = 0, max = 200 },
    power      = { type = "number", min = 0, max = 200 },
    decay      = { type = "boolean" },
    opacity    = { type = "number", min = 0, max = 1 },
    r = { type = "number", min = 0, max = 255 },
    g = { type = "number", min = 0, max = 255 },
    b = { type = "number", min = 0, max = 255 },
    red  = { type = "number", min = 0, max = 255 },
    green= { type = "number", min = 0, max = 255 },
    blue = { type = "number", min = 0, max = 255 },
    blurlevel = { type = "number", min = 0, max = 64 },
    x = { type = "number" },
    y = { type = "number" },
    rate    = { type = "number", min = 0, max = 1000 },
    speed   = { type = "number", min = 0, max = 10000 },
    emitter = { type = "number" },
    count   = { type = "number", min = 0, max = 100000 },
    lifeMin  = { type = "number", min = 0, max = 60 },
    life_max = { type = "number", min = 0, max = 60 },
    lifeMax  = { type = "number", min = 0, max = 60 },
    speedMin = { type = "number", min = 0, max = 10000 },
    speed_max= { type = "number", min = 0, max = 10000 },
    speedMax = { type = "number", min = 0, max = 10000 },
    sizeMin  = { type = "number", min = 0, max = 512 },
    size_max = { type = "number", min = 0, max = 512 },
    sizeMax  = { type = "number", min = 0, max = 512 },
    angleMin = { type = "number" },
    angle_max= { type = "number" },
    angleMax = { type = "number" },
    gravityX = { type = "number" },
    gravityY = { type = "number" },
    gravity_x = { type = "number" },
    gravity_y = { type = "number" },
});

-- ═══════════════════════════════════════════════════════════════════════
--  PostFx chain — ONE implementation, two entry points
-- ═══════════════════════════════════════════════════════════════════════
--  Entry points:
--    [vfx postfx=<kind>]  legacy round-102 form, routed by VFXCommands.vfx
--    [postprocess effect=<kind>] / [postprocess_off]  declarative form
--  Both funnel into apply_postfx below. They previously had near-identical
--  copies of this logic and DISAGREED on the lutMix fallback (0 vs 1.0),
--  which is exactly how a chain that "works in one command and not the
--  other" gets shipped.
--
--  Fallback literals live HERE and nowhere else, and they match the C++
--  defaults in RenderBinding.cpp resolvePostFxParams (strength 1.0,
--  radius 0, amount 0, lutMix 0) so a missing field means the same thing on
--  both sides of the binding.
--
--  The one value the two entries legitimately resolve differently is
--  strength, so it is an ARGUMENT rather than a branch inside the helper:
--    * [vfx] has a VALIDATION-ONLY schema (no defaults, see the comment on
--      schema.define("vfx")) -- filling one in would silently change legacy
--      scenes, so its caller passes params.strength or the literal.
--    * [postprocess] declares intensity with default 1.0, so its caller
--      resolves the author-facing alias pair before calling in.
--
--  Headless / Null devices report is_postfx_supported=false -> no-op
--  (graceful degradation, matching the legacy submitVFX path).
--  kind "none"/"off"/"" clears the WHOLE chain (backend.clear_postfx);
--  per-effect teardown is backend.destroy_postfx, reached through
--  [postprocess_off effect=<kind>].
local function apply_postfx(ctx, kind, strength, params, label)
    if kind == "none" or kind == "off" or kind == "" or kind == nil then
        backend.clear_postfx()
        if ctx then ctx._postfx = nil end
        return
    end
    if not backend.is_postfx_supported(kind) then
        print("[" .. (label or "vfx") .. "] postfx '" .. tostring(kind)
              .. "' unsupported on this device -- no-op")
        return
    end
    -- rgb: an explicit "r,g,b" string wins; otherwise synthesise one from
    -- discrete r/g/b when the author supplied any of them (C++ parses both
    -- forms, and an empty string means "leave the tint at white").
    local rgbStr = params.rgb
    if not rgbStr and (params.r ~= nil or params.g ~= nil or params.b ~= nil) then
        rgbStr = string.format("%d,%d,%d",
            math.floor(params.r or 255),
            math.floor(params.g or 255),
            math.floor(params.b or 255))
    end
    local pf = {
        strength = strength or 1.0,
        radius   = params.radius or 0,
        amount   = params.amount or 0,
        lutMix   = params.lutMix or 0,
        rgb      = rgbStr or "",
    }
    local handle, result = backend.set_postfx(kind, pf)
    if not valid_postfx_result(handle) then return false, result end
    if ctx then
        -- Bookkeeping only. NOT WIRED (audit): nothing reads ctx._postfx --
        -- not kag/snapshot.lua, not the save/load path, not scene reload --
        -- so an active chain is NOT restored after [load] or a scene switch.
        -- Left in place because it is the natural anchor for that restore and
        -- the tests pin its shape; whoever implements postfx persistence
        -- (storage/snapshot owner) should consume it rather than invent a
        -- second registry.
        ctx._postfx = ctx._postfx or {}
        ctx._postfx[kind] = { handle = handle, params = pf }
    end
    return handle, result
end

-- [vfx postfx=...] entry point.
function VFXCommands._postfx(ctx, params)
    return apply_postfx(ctx, params.postfx or "",
                        params.strength or 1.0, params, "vfx")
end

function VFXCommands.vfx(ctx, params)
    -- PostFx chain takes precedence when postfx= is present.
    if params.postfx and params.postfx ~= "" then
        return VFXCommands._postfx(ctx, params)
    end
    local vtype = params.type or params.effect or "particle"

    -- ── Particle effects ────────────────────────────────────────────────
    if vtype == "particle" then
        local action = params.action or "create"

        if action == "create" then
            local cfg = {
                x        = params.x or 0,
                y        = params.y or 0,
                rate     = params.rate or 10,
                lifeMin  = params.lifeMin or params.life_min or 0.5,
                lifeMax  = params.lifeMax or params.life_max or 2.0,
                speedMin = params.speedMin or params.speed_min or 10,
                speedMax = params.speedMax or params.speed_max or 50,
                angleMin = params.angleMin or params.angle_min or 0,
                angleMax = params.angleMax or params.angle_max or 6.283,
                sizeMin  = params.sizeMin or params.size_min or 2,
                sizeMax  = params.sizeMax or params.size_max or 8,
                r = params.r or params.red or 1,
                g = params.g or params.green or 1,
                b = params.b or params.blue or 1,
                a = params.a or params.alpha or 1,
                gravityX = tonumber(params.gravityX or params.gravity_x) or 0,
                gravityY = tonumber(params.gravityY or params.gravity_y) or 0,
            }
            local id, result = backend.particles_create_emitter(cfg)
            if not valid_particle_id(id) then return false, result end
            ctx._particleEmitters = ctx._particleEmitters or {}
            ctx._particleEmitters[id] = true
            return id, result

        elseif action == "emit" then
            local emitter = tonumber(params.emitter or params.id) or 0
            local count   = tonumber(params.count) or 1
            backend.particles_emit(emitter, count)

        elseif action == "destroy" then
            local emitter = tonumber(params.emitter or params.id) or 0
            backend.particles_destroy_emitter(emitter)
            if ctx._particleEmitters then
                ctx._particleEmitters[emitter] = nil
            end

        elseif action == "clear" then
            backend.clear_particles()
            VFXCommands._clear_runtime_state(ctx)
        end

    -- ── Quake ───────────────────────────────────────────────────────────
    elseif vtype == "quake" then
        VFX.quake(ctx, params)

    -- ── Shake ───────────────────────────────────────────────────────────
    elseif vtype == "shake" then
        VFX.shake(ctx, params)

    -- ── Flash ───────────────────────────────────────────────────────────
    elseif vtype == "flash" then
        VFX.flash(ctx, params)

    -- ── Fade ────────────────────────────────────────────────────────────
    elseif vtype == "fade" then
        VFX.fade(ctx, params)

    -- ── Blur ────────────────────────────────────────────────────────────
    elseif vtype == "blur" then
        VFX.blur(ctx, params)

    -- ── Stop all ────────────────────────────────────────────────────────
    elseif vtype == "stop" then
        VFX.stop_all()
        -- Also clear particles
        if ctx._particleEmitters then
            for id, _ in pairs(ctx._particleEmitters) do
                pcall(backend.particles_destroy_emitter, id)
            end
            ctx._particleEmitters = {}
        end
    end
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Standalone command aliases (KAG3 names, Neo-Genesis contracts).
-- ═══════════════════════════════════════════════════════════════════════════
function VFXCommands.flash(ctx, params)
    VFX.flash(ctx, params)
end

function VFXCommands.shake(ctx, params)
    VFX.shake(ctx, params)
end

function VFXCommands.quake(ctx, params)
    VFX.quake(ctx, params)
end

-- [particles action="create" x=0 y=0 rate=10 ...] -- standalone form
-- of [vfx type="particle" ...] (schema-vs-handler audit: the schema
-- existed with no handler key; the scheduler fallback rendered
-- 'particles' as dialogue). Same dispatch as the vfx wrapper.
function VFXCommands.particles(ctx, params)
    local action = params.action or "create"
    if action == "create" then
        local cfg = {
            x = tonumber(params.x) or 0,
            y = tonumber(params.y) or 0,
            rate = tonumber(params.rate) or 10,
            lifeMin = tonumber(params.lifeMin or params.life_min) or 0.5,
            lifeMax = tonumber(params.lifeMax or params.life_max) or 2.0,
            speedMin = tonumber(params.speedMin or params.speed_min) or 10,
            speedMax = tonumber(params.speedMax or params.speed_max) or 50,
            sizeMin = tonumber(params.sizeMin or params.size_min) or 2,
            sizeMax = tonumber(params.sizeMax or params.size_max) or 8,
            angleMin = tonumber(params.angleMin or params.angle_min) or 0,
            angleMax = tonumber(params.angleMax or params.angle_max) or 6.283,
            r = tonumber(params.r or params.red) or 1,
            g = tonumber(params.g or params.green) or 1,
            b = tonumber(params.b or params.blue) or 1,
            a = tonumber(params.a or params.alpha) or 1,
            gravityX = tonumber(params.gravityX or params.gravity_x) or 0,
            gravityY = tonumber(params.gravityY or params.gravity_y) or 0,
        }
        local id, result = backend.particles_create_emitter(cfg)
        if not valid_particle_id(id) then return false, result end
        ctx._particleEmitters = ctx._particleEmitters or {}
        ctx._particleEmitters[id] = true
        return id, result
    elseif action == "emit" then
        local emitter = tonumber(params.emitter or params.id) or 0
        local count = tonumber(params.count) or 1
        backend.particles_emit(emitter, count)
    elseif action == "destroy" then
        local emitter = tonumber(params.emitter or params.id) or 0
        backend.particles_destroy_emitter(emitter)
        if ctx._particleEmitters then ctx._particleEmitters[emitter] = nil end
    elseif action == "clear" then
        -- the backend name is clear_particles (not particles_clear)
        backend.clear_particles()
        VFXCommands._clear_runtime_state(ctx)
    end
end


-- ═══════════════════════════════════════════════════════════════════════
--  [palette effect="apply|clear|day|night|toggle|unload" ...]
--  Standalone LUT color-grading command routing to scripts/palette.lua.
--  The palette module drives the C++ backend through the GLOBAL backend
--  (backend.load_image / backend.is_valid / backend.set_palette /
--  backend.destroy_texture) -- it is not require()-bound.
--    effect="apply" id="<lut>" path="assets/lut/<name>.png" intensity=0..1
--        -> loads (when path given) then applies the LUT; without a path
--           re-applies a previously loaded id
--    effect="clear"     -> disable the active palette
--    effect="day"       -> neutral grading (clears any active LUT)
--    effect="night"     -> blue-dark grading (loads assets/lut/night.png)
--    effect="toggle"    -> day <-> night
--    effect="unload" id="<lut>" -> unload a cached LUT from memory
-- ═══════════════════════════════════════════════════════════════════════
schema.define("palette", {
    _meta = { category = "vfx", blocking = false, desc = "KAG3-compatible palette/LUT color-grading command" },
    effect = { type = "enum", values = {
        ["apply"] = true, ["clear"] = true, ["day"] = true,
        ["night"] = true, ["toggle"] = true, ["unload"] = true,
    }, default = "apply" },
    id        = { type = "string", default = "" },
    path      = { type = "string", default = "" },
    intensity = { type = "number", default = 1.0, min = 0.0, max = 1.0 },
})

function VFXCommands.palette(ctx, params)
    local effect = params.effect or "apply"
    local paletteModule = require("palette")
    local id = params.id or ""
    if effect == "apply" then
        -- Load-then-apply: a path means (id, path) is a fresh LUT.
        if params.path and #params.path > 0 then
            local ok, err = paletteModule.load(id, params.path)
            if not ok then
                print("[palette] " .. tostring(err))
                return
            end
        end
        -- Apply whatever is loaded under id (no-op if the id is empty).
        if id and #id > 0 then
            paletteModule.apply(id, params.intensity)
        end
    elseif effect == "clear" then
        paletteModule.clear()
    elseif effect == "day" then
        paletteModule.set_day_mode()
    elseif effect == "night" then
        paletteModule.set_night_mode()
    elseif effect == "toggle" then
        paletteModule.toggle_mode()
    elseif effect == "unload" then
        if id and #id > 0 then
            paletteModule.unload(id)
        end
    end
end

-- [vibrate] -- KAG3-compatible alias for [vib] (message-layer vibration).
-- [vib] is owned by transition.lua; this alias carries the SAME schema
-- contract and forwards verbatim so KAG3 scripts written with either tag
-- name behave identically. Lazy require keeps kag.lua's module load order
-- independent of the transition table.
schema.define("vibrate", {
    _meta = { category = "vfx", blocking = true, desc = "KAG3-compatible alias for [vib] (message-layer vibration)" },
    time      = { type = "number", default = 300, min = 0, max = 30000 },
    intensity = { type = "number", default = 3, min = 0, max = 50 },
    amplitude = { type = "number", min = 0, max = 50 },
})

function VFXCommands.vibrate(ctx, params)
    local trans = require("kag.commands.transition")
    if trans.vib then
        trans.vib(ctx, params)
    end
end

-- ═══════════════════════════════════════════════════════════════════════
--  [postprocess] & [postprocess_off] — declarative entry points
-- ═══════════════════════════════════════════════════════════════════════
--  Both delegate to apply_postfx (defined next to VFXCommands._postfx) so
--  there is exactly one place that builds a PostFxParams table.
function VFXCommands.postprocess(ctx, params)
    -- intensity is the author-facing name, strength the [vfx]-compatible
    -- alias; neither has a schema default (see the schema comment), so a nil
    -- here really means "the author omitted it" and apply_postfx supplies the
    -- single 1.0 fallback. When both are given the declared name wins.
    return apply_postfx(ctx, params.effect or "bloom",
                        params.intensity or params.strength,
                        params, "postprocess")
end

-- [postprocess_off]           -> clear the whole chain
-- [postprocess_off effect=x]  -> tear down one stage (backend.destroy_postfx)
function VFXCommands.postprocess_off(ctx, params)
    params = params or {}
    local effect = params.effect
    if effect and effect ~= "" and effect ~= "all"
       and type(backend.destroy_postfx) == "function" then
        backend.destroy_postfx(effect)
        if ctx and ctx._postfx then
            ctx._postfx[effect] = nil
            if next(ctx._postfx) == nil then ctx._postfx = nil end
        end
        return
    end
    backend.clear_postfx()
    if ctx then ctx._postfx = nil end
end

-- ═══════════════════════════════════════════════════════════════════════
--  [particle_weather]
-- ═══════════════════════════════════════════════════════════════════════
local WEATHER_PRESETS = {
    rain = {
        x = 640, y = -20,
        rate = 80,
        lifeMin = 0.8, lifeMax = 1.6,
        speedMin = 600, speedMax = 1000,
        angleMin = 1.45, angleMax = 1.69,
        sizeMin = 1, sizeMax = 3,
        r = 0.75, g = 0.85, b = 1.0, a = 0.65,
        gravityX = 0, gravityY = 600,
        windMultX = 40,
    },
    snow = {
        x = 640, y = -20,
        rate = 25,
        lifeMin = 3.0, lifeMax = 6.0,
        speedMin = 40, speedMax = 100,
        angleMin = 1.20, angleMax = 1.94,
        sizeMin = 2, sizeMax = 6,
        r = 1.0, g = 1.0, b = 1.0, a = 0.9,
        gravityX = 0, gravityY = 25,
        windMultX = 35,
    },
    sakura = {
        x = 200, y = -20,
        rate = 15,
        lifeMin = 4.0, lifeMax = 8.0,
        speedMin = 30, speedMax = 70,
        angleMin = 1.0, angleMax = 2.1,
        sizeMin = 4, sizeMax = 8,
        r = 1.0, g = 0.72, b = 0.82, a = 0.85,
        gravityX = 30, gravityY = 18,
        windMultX = 45,
    },
    dust = {
        x = 640, y = 360,
        rate = 10,
        lifeMin = 2.0, lifeMax = 5.0,
        speedMin = 5, speedMax = 25,
        angleMin = 0, angleMax = 6.283,
        sizeMin = 1, sizeMax = 4,
        r = 1.0, g = 0.95, b = 0.8, a = 0.35,
        gravityX = 0, gravityY = 0,
        windMultX = 10,
    },
}

-- Process-wide fallback registry, used when a caller passes no ctx (engine
-- scripts, editor eval). ctx._weatherEmitters is authoritative when present.
local _activeWeatherEmitters = {}

function VFXCommands._clear_runtime_state(ctx)
    _activeWeatherEmitters={}
    if ctx then ctx._weatherEmitters={};ctx._particleEmitters={} end
end

function VFXCommands.particle_weather(ctx, params)
    local action    = params.action or "start"
    -- A missing type means "rain" when starting, but "every type" when
    -- stopping -- so resolve the start default HERE rather than in the schema
    -- (see the schema.define comment above).
    local wtype     = params.type or "rain"
    local count     = tonumber(params.count) or 10
    local wind      = tonumber(params.wind) or 0
    local speed     = tonumber(params.speed) or 1.0
    local intensity = tonumber(params.intensity) or 1.0

    if action == "start" then
        -- type="all" is a stop-only selector; starting it would silently
        -- create a rain emitter through the fallback below, so say so instead.
        if wtype == "all" then
            print("[particle_weather] type=\"all\" is only valid with action=\"stop\""
                  .. " -- ignoring start")
            return
        end
        local preset = WEATHER_PRESETS[wtype] or WEATHER_PRESETS.rain
        local rate = preset.rate * intensity * (count > 0 and (count / 10) or 1.0)
        local cfg = {
            x = preset.x,
            y = preset.y,
            rate = rate,
            lifeMin = preset.lifeMin,
            lifeMax = preset.lifeMax,
            speedMin = preset.speedMin * speed,
            speedMax = preset.speedMax * speed,
            angleMin = preset.angleMin,
            angleMax = preset.angleMax,
            sizeMin = preset.sizeMin,
            sizeMax = preset.sizeMax,
            r = preset.r,
            g = preset.g,
            b = preset.b,
            a = preset.a,
            gravityX = (preset.gravityX or 0) + (wind * (preset.windMultX or 10)),
            gravityY = preset.gravityY or 0,
        }

        -- Prepare the replacement before changing either owner index. If the
        -- backend is at its quota, keep the existing weather unchanged.
        local oldId = (ctx and ctx._weatherEmitters and ctx._weatherEmitters[wtype]) or _activeWeatherEmitters[wtype]
        local id, result = backend.particles_create_emitter(cfg)
        if not valid_particle_id(id) then return false, result end
        if oldId and oldId ~= id then
            pcall(backend.particles_destroy_emitter, oldId)
            if ctx and ctx._particleEmitters then ctx._particleEmitters[oldId] = nil end
        end

        _activeWeatherEmitters[wtype] = id
        if ctx then
            ctx._weatherEmitters = ctx._weatherEmitters or {}
            ctx._weatherEmitters[wtype] = id
            ctx._particleEmitters = ctx._particleEmitters or {}
            ctx._particleEmitters[id] = true
        end
        if count > 0 then
            pcall(backend.particles_emit, id, math.min(count, 50))
        end
        return id, result

    elseif action == "stop" then
        local specificType = params.type
        if specificType and specificType ~= "all" and specificType ~= "" then
            local id = (ctx and ctx._weatherEmitters and ctx._weatherEmitters[specificType]) or _activeWeatherEmitters[specificType]
            if id then
                pcall(backend.particles_destroy_emitter, id)
                if ctx and ctx._particleEmitters then ctx._particleEmitters[id] = nil end
                if ctx and ctx._weatherEmitters then ctx._weatherEmitters[specificType] = nil end
                _activeWeatherEmitters[specificType] = nil
            end
        else
            if ctx and ctx._weatherEmitters then
                for _, eid in pairs(ctx._weatherEmitters) do
                    pcall(backend.particles_destroy_emitter, eid)
                    if ctx._particleEmitters then ctx._particleEmitters[eid] = nil end
                end
                ctx._weatherEmitters = {}
            end
            for _, eid in pairs(_activeWeatherEmitters) do
                pcall(backend.particles_destroy_emitter, eid)
            end
            _activeWeatherEmitters = {}
        end

    elseif action == "clear" then
        local ok=backend.clear_particles()
        VFXCommands._clear_runtime_state(ctx)
        return ok
    end
end

return VFXCommands
