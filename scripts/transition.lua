-- ═══════════════════════════════════════════════════════════════════════════
--  Caesura (AmeKAG) — transition.lua
--  GPU-backed transitions: Crossfade, Rule Image, and directional Wipe.
--  Spec [2.3]: fullscreen quad shader → C++ submitTransition via backend.
--  Supports both coroutine-based (blocking) and tick-based (non-blocking) APIs.
--  Krkrz reference: visual/TransitionHandler.cpp, RuleImage transition engine.
-- ═══════════════════════════════════════════════════════════════════════════

local backend = require("backend")
local rtt     = require("rtt")

local Transition = {}

-- ═══════════════════════════════════════════════════════════════════════════
--  Transition methods (GPU shader modes for submit_transition)
-- ═══════════════════════════════════════════════════════════════════════════

Transition.Method = {
    CROSSFADE    = 0,  -- direct lerp: mix(from, to, t)
    RULE         = 1,  -- Rule Image: rule_tex pixel value → threshold
    WIPE_LEFT    = 2,  -- wipe from left edge
    WIPE_RIGHT   = 3,  -- wipe from right edge
    WIPE_TOP     = 4,  -- wipe from top edge
    WIPE_BOTTOM  = 5,  -- wipe from bottom edge
}

-- ═══════════════════════════════════════════════════════════════════════════
--  Transition status for tick-based API
-- ═══════════════════════════════════════════════════════════════════════════

Transition.Status = {
    IDLE      = 0,
    RUNNING   = 1,
    COMPLETED = 2,
    CANCELLED = 3,
}

-- ═══════════════════════════════════════════════════════════════════════════
--  Internal: active transition state (tick-based path)
-- ═══════════════════════════════════════════════════════════════════════════

local activeTransition = nil
local activePending = nil

-- ═══════════════════════════════════════════════════════════════════════════
--  Internal: preloaded rule textures
-- ═══════════════════════════════════════════════════════════════════════════

local ruleCache = {}

function Transition.capture_screen()
    return rtt.capture_scene()
end

function Transition.clear_render_wait(ctx, owner)
    local wait = ctx and ctx._transition_render_wait
    if wait and wait.owner == owner then ctx._transition_render_wait = nil end
end

function Transition.defer_until_render(ctx, owner)
    -- Direct scheduler callers own their explicit frame pump. Real runner
    -- contexts provide an epoch which advances only at their render boundary.
    if ctx._render_epoch == nil then return nil end
    local wait = {owner=owner, epoch=ctx._render_epoch+1}
    ctx._transition_render_wait = wait
    return wait
end

function Transition.wait_for_render(ctx, owner)
    local wait = Transition.defer_until_render(ctx, owner)
    if not wait then return coroutine.yield() end
    local value
    repeat
        value = coroutine.yield("__kag_render_pending")
        -- DebugProtocol can directly resume the anchored coroutine. Such a
        -- resume must still not turn an update/input event into a render.
        if owner.closed or owner.status == Transition.Status.CANCELLED or ctx._session_active == false then
            Transition.clear_render_wait(ctx, owner)
            return nil
        end
    until ctx._render_epoch >= wait.epoch
    Transition.clear_render_wait(ctx, owner)
    return value
end

function Transition.capture_after_render(ctx, owner)
    -- Keep an existing presented A. Only the explicit no-frame result may
    -- request a first actual render; unsupported remains an error after one try.
    local id, frame = backend.capture_scene()
    local function cancelled()
        return owner.closed or owner.status == Transition.Status.CANCELLED
            or ctx._session_active == false
    end
    if id == 0 and frame == 0 and not cancelled() then
        Transition.wait_for_render(ctx, owner)
        if not cancelled() then id, frame = backend.capture_scene() end
    end
    if cancelled() then
        if type(id) == "number" and id > 0 then rtt.destroy(id) end
        return nil
    end
    assert(type(id)=="number" and id>0 and type(frame)=="number" and frame>0,
        "transition preparation requires a real rendered scene")
    return id, frame
end

-- Scheduler-owned bridge between an immediate visual mutation and the exact
-- next [trans]. The normal token yield is retained: B renders offscreen while
-- this lease holds the already presented A. No future command is executed.
function Transition.cancel_pending(ctx, owner)
    local p = ctx and ctx._pending_transition
    if not p or (owner and p.owner ~= owner) then return end
    ctx._pending_transition = nil
    if p.closed then return end
    p.closed = true
    Transition.clear_render_wait(ctx, p)
    if activePending == p then
        activePending = nil
        backend.cancel_transition()
    end
    if p.from then rtt.destroy(p.from) end
    p.operation:__close("pending transition cancelled")
end

function Transition.prepare_pending(ctx, stream, index, owner)
    Transition.cancel_pending(ctx)
    Transition.cancel()
    local p = {ctx=ctx, stream=stream, index=index, owner=owner, scene=ctx.current_scene,
        operation=require("kag.operation").start(ctx)}
    ctx._pending_transition = p
    activePending = p
    p.operation.token:register(function() Transition.cancel_pending(ctx, owner) end)
    local published = false
    local guard <close> = setmetatable({}, {__close=function()
        if not published then Transition.cancel_pending(ctx, owner) end
    end})
    local id, frame = Transition.capture_after_render(ctx, p)
    if not id then return false end
    p.from, p.frame = id, frame
    assert(backend.submit_transition(1, id, id, 0, Transition.Method.CROSSFADE, 0) ~= false,
        "transition source hold unavailable")
    Transition.defer_until_render(ctx, p)
    published = true
    return true
end

function Transition.take_pending(ctx)
    local p = ctx and ctx._pending_transition
    if not p then return nil end
    local wait = ctx._transition_render_wait
    if wait and wait.owner == p and ctx._render_epoch < wait.epoch then
        -- Direct debug resumption can also bypass the runner while suspended
        -- at the scheduler's ordinary post-bg yield, not just inside capture.
        Transition.wait_for_render(ctx, p)
    end
    assert(not p.closed and not p.operation.token.cancelled, "pending transition cancelled")
    ctx._pending_transition = nil
    Transition.clear_render_wait(ctx, p)
    if activePending == p then activePending = nil end
    p.closed = true
    p.operation:complete()
    p.operation:__close()
    return p.from, p.frame
end

function Transition.render_pending()
    local p = activePending
    if not p or not p.from or p.closed then return end
    if p.operation.token.cancelled then Transition.cancel_pending(p.ctx, p.owner); return end
    if backend.submit_transition(1, p.from, p.from, 0, Transition.Method.CROSSFADE, 0) == false then
        Transition.cancel_pending(p.ctx, p.owner)
        error("pending transition source no longer available")
    end
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Transition.preload_rule(storage) → rule_tex_id
--    Preloads a rule image as a GPU texture. Returns a tex id usable
--    by rule() and start_rule(). The texture is cached for reuse.
-- ═══════════════════════════════════════════════════════════════════════════

function Transition.preload_rule(storage)
    if not storage then return nil end

    -- check cache
    local key = tostring(storage)
    if ruleCache[key] then
        return ruleCache[key]
    end

    local ok, texId = pcall(function()
        return backend.load_texture(storage)
    end)

    if ok and texId and type(texId) == "number" and texId > 0 then
        ruleCache[key] = texId
        return texId
    end

    return nil
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Transition.free_rule(rule_tex_id) — release a preloaded rule texture
-- ═══════════════════════════════════════════════════════════════════════════

function Transition.free_rule(rule_tex_id)
    if not rule_tex_id then return end
    backend.destroy_texture(rule_tex_id)
    -- remove from cache
    for key, id in pairs(ruleCache) do
        if id == rule_tex_id then
            ruleCache[key] = nil
            break
        end
    end
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Transition.clear_rule_cache() — release all preloaded rule textures
-- ═══════════════════════════════════════════════════════════════════════════

function Transition.clear_rule_cache()
    for _, id in pairs(ruleCache) do
        backend.destroy_texture(id)
    end
    ruleCache = {}
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Transition.load_rule(storage) → rule_tex_id | rule_table
--    Legacy-compatible: loads a rule image, falls back to a grayscale table.
-- ═══════════════════════════════════════════════════════════════════════════

function Transition.load_rule(storage)
    local texId = Transition.preload_rule(storage)
    if texId then return texId end

    -- fallback: build a 256-entry grayscale rule table
    local tbl = {}
    for i = 0, 255 do
        tbl[i] = i / 255.0
    end
    return tbl
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Transition.crossfade(ctx, fromTex, toTex, duration)
--    BLOCKING (coroutine): crossfades from fromTex to toTex over duration ms.
--    Yields each frame until complete.
-- ═══════════════════════════════════════════════════════════════════════════

function Transition.crossfade(ctx, fromTex, toTex, duration)
    duration = tonumber(duration) or 1000
    local elapsed = 0

    while elapsed < duration do
        local dt = coroutine.yield() or 0
        elapsed = elapsed + dt
        local t = math.min(1.0, elapsed / duration)

        backend.submit_transition(1, fromTex, toTex, 0,
            Transition.Method.CROSSFADE, t)
    end

    backend.submit_transition(1, fromTex, toTex, 0,
        Transition.Method.CROSSFADE, 1.0)
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Transition.rule(ctx, fromTex, toTex, ruleTex, duration)
--    BLOCKING (coroutine): Rule Image transition.
--    ruleTex can be a preloaded texture id or a rule table from load_rule.
-- ═══════════════════════════════════════════════════════════════════════════

function Transition.rule(ctx, fromTex, toTex, ruleTex, duration)
    duration = tonumber(duration) or 1000
    local elapsed = 0
    local ruleId = type(ruleTex) == "number" and ruleTex or 0

    while elapsed < duration do
        local dt = coroutine.yield() or 0
        elapsed = elapsed + dt
        local t = math.min(1.0, elapsed / duration)

        backend.submit_transition(1, fromTex, toTex, ruleId,
            Transition.Method.RULE, t)
    end

    backend.submit_transition(1, fromTex, toTex, ruleId,
        Transition.Method.RULE, 1.0)
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Transition.wipe(ctx, fromTex, toTex, duration, direction)
--    BLOCKING (coroutine): directional wipe.
--    direction: "left" | "right" | "top" | "bottom"
-- ═══════════════════════════════════════════════════════════════════════════

function Transition.wipe(ctx, fromTex, toTex, duration, direction)
    duration = tonumber(duration) or 800
    local dirMap = {
        left   = Transition.Method.WIPE_LEFT,
        right  = Transition.Method.WIPE_RIGHT,
        top    = Transition.Method.WIPE_TOP,
        bottom = Transition.Method.WIPE_BOTTOM,
    }
    local method = dirMap[direction] or Transition.Method.WIPE_LEFT
    local elapsed = 0

    while elapsed < duration do
        local dt = coroutine.yield() or 0
        elapsed = elapsed + dt
        local t = math.min(1.0, elapsed / duration)

        backend.submit_transition(1, fromTex, toTex, 0, method, t)
    end

    backend.submit_transition(1, fromTex, toTex, 0, method, 1.0)
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Non-blocking (tick-based) transition API
--    Use these when you need to drive transitions from the main loop
--    without blocking a coroutine context.
-- ═══════════════════════════════════════════════════════════════════════════

-- ═══════════════════════════════════════════════════════════════════════════
--  Transition.start(fromTex, toTex, params) → transition_id
--    params: { method="crossfade"|"rule"|"wipe", duration=ms,
--              rule_tex=id, direction="left"|..., on_complete=fn }
--    Returns a transition state table.
-- ═══════════════════════════════════════════════════════════════════════════

function Transition.start(fromTex, toTex, params)
    params = params or {}
    assert(type(fromTex) == "number" and fromTex > 0
        and type(toTex) == "number" and toTex > 0, "transition requires numeric texture handles")
    Transition.cancel()

    local tx = {
        from_tex    = fromTex,
        to_tex      = toTex,
        method      = params.method or "crossfade",
        duration    = tonumber(params.duration) or 1000,
        elapsed     = 0,
        status      = Transition.Status.RUNNING,
        rule_tex    = params.rule_tex or 0,
        direction   = params.direction or "left",
        on_complete = params.on_complete,
        view_id     = tonumber(params.view_id) or 1,
    }

    -- resolve method id
    local methodMap = {
        crossfade   = Transition.Method.CROSSFADE,
        rule        = Transition.Method.RULE,
        wipe        = Transition.Method.WIPE_LEFT,
    }
    local dirMap = {
        left   = Transition.Method.WIPE_LEFT,
        right  = Transition.Method.WIPE_RIGHT,
        top    = Transition.Method.WIPE_TOP,
        bottom = Transition.Method.WIPE_BOTTOM,
    }
    if tx.method == "wipe" then
        tx.method_id = dirMap[tx.direction] or Transition.Method.WIPE_LEFT
    else
        tx.method_id = methodMap[tx.method] or Transition.Method.CROSSFADE
    end

    activeTransition = tx
    return tx
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Transition.tick(dt) — advance active transition by dt milliseconds
--    Call once per frame from the main loop. Submits the current frame
--    to the GPU and triggers the on_complete callback when done.
-- ═══════════════════════════════════════════════════════════════════════════

function Transition.tick(dt, expected)
    local tx = activeTransition
    if not tx or (expected and tx ~= expected) or tx.status ~= Transition.Status.RUNNING then
        return Transition.Status.IDLE
    end

    tx.elapsed = tx.elapsed + math.max(0, tonumber(dt) or 0)
    local t = tx.duration <= 0 and 1 or math.min(1.0, tx.elapsed / tx.duration)

    if tx.method == "rule" then
        assert(backend.submit_transition(tx.view_id, tx.from_tex, tx.to_tex,
            tx.rule_tex, Transition.Method.RULE, t) ~= false, "transition textures no longer available")
    else
        assert(backend.submit_transition(tx.view_id, tx.from_tex, tx.to_tex,
            0, tx.method_id, t) ~= false, "transition textures no longer available")
    end

    if t >= 1.0 then
        tx.status = Transition.Status.COMPLETED
        if tx.on_complete then
            tx.on_complete(tx)
        end
        return Transition.Status.COMPLETED
    end

    return Transition.Status.RUNNING
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Transition.cancel() — cancel the currently active transition
-- ═══════════════════════════════════════════════════════════════════════════

function Transition.cancel(expected)
    if expected and activeTransition ~= expected then return end
    if activePending then Transition.cancel_pending(activePending.ctx, activePending.owner) end
    if activeTransition then
        activeTransition.status = Transition.Status.CANCELLED
        if activeTransition.release then activeTransition.release() end
    end
    activeTransition = nil
    backend.cancel_transition()
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Transition.get_status() → status enum, progress (0-1)
-- ═══════════════════════════════════════════════════════════════════════════

function Transition.get_status()
    if not activeTransition then
        return Transition.Status.IDLE, 0
    end
    local t = 0
    if activeTransition.duration > 0 then
        t = math.min(1.0, activeTransition.elapsed / activeTransition.duration)
    end
    return activeTransition.status, t
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Transition.is_active() → boolean
-- ═══════════════════════════════════════════════════════════════════════════

function Transition.is_active(expected)
    return activeTransition ~= nil
        and (not expected or activeTransition == expected)
        and activeTransition.status == Transition.Status.RUNNING
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Transition.clear(ctx) — reset transition state (compatibility)
-- ═══════════════════════════════════════════════════════════════════════════

function Transition.clear(ctx)
    if ctx then ctx._transition = nil end
    Transition.cancel()
end

return Transition
