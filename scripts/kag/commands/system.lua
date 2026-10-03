-- =============================================================================
--  Caesura (AmeKAG) — kag/commands/system.lua
--  Phase 4/9: KAG system tag handlers — [wait], [emb], [eval]
--  [wait]  — time-based blocking with CancelToken (spec [10.2.33])
--  [emb]   — embedded Lua execution within sandbox (spec [10.2.47])
--  [eval]  — expression evaluation with result storage
--  Phase 9: Integrated sandbox.lua for strict Release enforcement
-- =============================================================================

local Operation = require("kag.operation")
local WaitState = require("kag.wait_state")
-- Sandbox is loaded by C++ lockdownScriptEnv() AFTER all modules are preloaded.
-- Do NOT require("sandbox") here ? it would activate lockdown too early.
local _sandbox_cache = nil
local function getSandbox()
    if not _sandbox_cache then
        _sandbox_cache = rawget(package.loaded, "sandbox")
    end
    return _sandbox_cache
end

local SystemCommands = {}

-- ═══════════════════════════════════════════════════════════════════════════
--  [wait time=1500]
--  Block for specified milliseconds. Cancelable via CancelToken.
--  Spec [10.2.33]: registers cancel callback, uses coroutine.yield.
-- ═══════════════════════════════════════════════════════════════════════════

-- Neo-Genesis contract: number + 60s cap (replaces the inline clamp).
-- NOTE: ms/duration deliberately carry NO default (security fix): coerce
-- injects defaults for absent fields, so time's default (1000) would
-- shadow an explicit [delay ms=500]; the handler prefers ms, and only
-- time carries the fallback default.
require("kag.schema").define("wait", {
    _meta = { category = "system", blocking = true, desc = "KAG3-compatible wait command" },
    time     = { type = "number", default = 1000, min = 0, max = 60000 },
    ms       = { type = "number", min = 0, max = 60000 },
    duration = { type = "number", min = 0, max = 60000 },
})
-- [delay ms=500] -- KAG3 duplicate of [wait]; its OWN schema so the ms
-- string from the tokenizer coerces to a number before the wait loop's
-- ms<=0 comparison (audit: without this, "500" <= 0 raised, pcall'd).
require("kag.schema").define("delay", {
    _meta = { category = "system", blocking = true, desc = "KAG3-compatible delay command" },
    time     = { type = "number", default = 1000, min = 0, max = 60000 },
    ms       = { type = "number", min = 0, max = 60000 },
    duration = { type = "number", min = 0, max = 60000 },
})

function SystemCommands.wait(ctx, params)
    -- Explicit aliases (ms/duration) first; time only carries the
    -- default (so [delay ms=500] waits 500ms, not the injected 1000ms
    -- default -- and [delay duration=2000] gets 2000, review warn)
    -- bare positional [wait 200] -> params[1] (tokenizer bare-value)
    local ms=WaitState.duration(params)
    -- clamp here too: bare positional args bypass schema's 0..60000
    -- (security minor: [wait 999999] must not block for 16 minutes)
    local scene=ctx.current_scene or ctx.currentScene or ''
    local index=ctx._executing_index or ctx.token_index
    if ctx._restoredWait then
        local restored=WaitState.prepare(ctx._restoredWait,scene,ctx.tokens,index)
        ctx._restoredWait=nil
        ms=restored.remaining_ms
    end
    if ms<=0 then return end

    local operation <close> = Operation.start(ctx)
    local ct = operation.token
    local state={scene=scene,token_index=index,remaining_ms=ms}
    ctx._waitState=state
    ct:register(function() if ctx._waitState==state then ctx._waitState=nil end end)

    local elapsed = 0
    local frameTime = 16  -- ~60fps default dt

    -- Honor the scheduler's flow controls exactly like [until] (round
    -- 87): a scene abort (ctx.stop_flag) or a Lua-initiated jump
    -- (ctx._next_index) must end the wait immediately instead of keeping
    -- the coroutine parked until ms elapses or the operation is cancelled.
    while elapsed < ms and not ct.cancelled
          and not ctx.stop_flag and not ctx._next_index do
        -- Yield each frame; scheduler feeds actual dt
        local dt = coroutine.yield() or frameTime
        if ct.cancelled then break end
        elapsed = elapsed + dt
        state.remaining_ms=math.max(0,ms-elapsed)
    end

    if not ct.cancelled then
        operation:complete()
    end
    if ctx._waitState==state then ctx._waitState=nil end
end

-- ═══════════════════════════════════════════════════════════════════════════
--  [emb exp="ctx.f.score = ctx.f.score + 1"]
--  Execute embedded Lua within sandboxed environment.
--  Spec [10.2.47]: _ENV whitelist, Dev/Release dual mode.
--  Phase 9: Uses sandbox.execute() when sandbox.is_strict() is true.
-- ═══════════════════════════════════════════════════════════════════════════

function SystemCommands.emb(ctx, params)
    local exp = params.exp or params.code
    if type(exp) ~= "string" and type(params[1]) == "string" then
        exp = params[1]
    end
    exp = exp or ""
    if #exp == 0 then return end

    -- Check if sandbox enforcement is active
    if getSandbox().is_strict() then
        -- Strict mode: use sandbox.execute()
        local env = {
            ctx  = ctx,
            tf   = ctx.tf or {},
            f    = ctx.f or {},
            sf   = ctx.sf or {},
            mp   = ctx.mp or {},
        }

        local ok, result, envOut = getSandbox().execute(exp, env)

        -- Sync back any mutations to the environment. Type-guarded
        -- (security LOW): a script REPLACING tf with a non-table would
        -- flow it into ctx.tf and the emb_result write below would raise
        -- OUTSIDE the sandbox pcall.
        if envOut then
            if type(envOut.tf) == "table" then ctx.tf = envOut.tf end
            if type(envOut.f) == "table" then ctx.f = envOut.f end
            if type(envOut.sf) == "table" then ctx.sf = envOut.sf end
            if type(envOut.mp) == "table" then ctx.mp = envOut.mp end
        end

        if ok then
            if type(ctx.tf) ~= "table" then ctx.tf = {} end
            rawset(ctx.tf, "emb_result", result)  -- security LOW: no __newindex trap
        else
            if type(ctx.tf) ~= "table" then ctx.tf = {} end
            rawset(ctx.tf, "emb_result", nil)
        end
        return
    end

    -- Dev mode (lax): original behavior — build sandbox with basic whitelist
    local sandbox = {
        ctx     = ctx,
        math    = math,
        string  = string,
        table   = table,
        os      = { clock = os.clock, date = os.date, time = os.time },
        tostring = tostring,
        tonumber = tonumber,
        type    = type,
        pairs   = pairs,
        ipairs  = ipairs,
        next    = next,
        print   = print,
        pcall   = pcall,
        select  = select,
        unpack  = unpack or table.unpack,
        error   = error,
    }

    -- Compile + execute in sandbox
    local fn, compileErr = load(exp, "=emb", "t", sandbox)
    if not fn then
        print("[SystemCmd] emb compile error: " .. tostring(compileErr))
        return
    end

    local ok, result = pcall(fn)
    if ok then
        ctx.tf = ctx.tf or {}
        rawset(ctx.tf, "emb_result", result)  -- uniform no-trap invariant
    else
        print("[SystemCmd] emb runtime error: " .. tostring(result))
        ctx.tf = ctx.tf or {}
        rawset(ctx.tf, "emb_result", nil)
    end
end

-- ═══════════════════════════════════════════════════════════════════════════
--  [eval exp="1+2*3"]
--  Evaluate a Lua expression and return the result in tf.eval_result.
--  Live path: scheduler.lua treats [eval] as flow-control
--  (flow_commands["eval"]=true, scheduler.lua:30) and evaluates it INLINE
--  (scheduler.lua:1099 -- TJS operator parity + whitelist env). This
--  handler is therefore a REGISTRY STUB, same class as KAG.macro/endmacro
--  (kag.lua:241-243): the old strict-mode sandbox fallback body was dead
--  (the inline branch intercepts first -- its own comment said so, and the
--  t192/t195 eval-path audit found no direct KAG.eval/SystemCommands.eval
--  callers anywhere in scripts/tests/demo) and was removed. The key stays
--  exported so the dispatch census / direct-API surface remain intact
--  (removing it would leave [eval] looking UNDISPATCHED to the scanner).
-- ═══════════════════════════════════════════════════════════════════════════

function SystemCommands.eval(ctx, params) end

-- ═══════════════════════════════════════════════════════════════════════════
--  [history] — open backlog overlay UI
--  U3: Backlog display. Opens scrollable overlay with jump + voice replay.
-- ═══════════════════════════════════════════════════════════════════════════

function SystemCommands.history(ctx, params)
    local HistoryUI = require("history_ui")
    local result = HistoryUI.show(ctx)
    if type(result) == "table" and result.jump then
        -- The scheduler discards handler return values; signal the jump
        -- through ctx so the runner's dead-coroutine branch consumes it.
        ctx._pendingJump = { scene = result.scene, index = result.index }
        ctx.stop_flag = true
    end
    return result
end


-- ═══════════════════════════════════════════════════════════════════════════
--  [unlock type="cg" id="scene01"] — unlock a gallery/music item
--  Writes to ctx.unlockedCG (or ctx.unlockedMusic) for persistent tracking.
-- ═══════════════════════════════════════════════════════════════════════════

-- [rollback] — pop the newest token-level snapshot and re-run from there.
-- Returns false (no error) when there is nothing to roll back; the runner
-- surfaces that as a click with no effect.
-- [gallery] — open the CG gallery (browse unlocked art; [unlock cg=] adds)
-- Neo-Genesis contracts: flow/UI commands typed + validated.
local _schema = require("kag.schema")
_schema.define("eval", {
    _meta = { category = "system", blocking = false, desc = "KAG3-compatible eval command" },
    exp = { type = "string" },   -- no default: "" is truthy and would shadow
    code = { type = "string" }, -- the handler's positional/code fallbacks
})
_schema.define("emb", {
    _meta = { category = "system", blocking = false, desc = "KAG3-compatible emb command" },
    exp = { type = "string" },
    code = { type = "string" },
})
_schema.define("chapter", {
    _meta = { category = "system", blocking = false, desc = "KAG3-compatible chapter command" },
    label = { type = "string" },
    id = { type = "string" },
})
_schema.define("gallery", {
    _meta = { category = "system", blocking = false, desc = "KAG3-compatible gallery command" },
    id = { type = "string" },  -- no default: handler's positional fallback
})
_schema.define("sma_play", {
    _meta = { category = "system", blocking = false,
              desc = "SMA skeletal-mesh actor spawn (Battle 4d S3; loop/rate/on_done round 18)" },
    name = { type = "string" },   -- actor id
    asset = { type = "string" },  -- registered SMA asset (sma.register)
    anim = { type = "string" },   -- animation name (default "idle")
    x = { type = "number" }, y = { type = "number" },
    scale = { type = "number" }, opacity = { type = "number" },
    tex = { type = "number" },    -- engine render-texture id
    view = { type = "number" },   -- target bgfx view id
    loop = { type = "boolean", default = true },  -- loop the clip (design doc §4)
    rate = { type = "number", default = 1 },      -- playback speed multiplier
    on_done_anim = { type = "string" },  -- non-loop completion fallback
})
_schema.define("sma_anim", {
    _meta = { category = "system", blocking = false,
              desc = "SMA runtime animation switch (round 18; optional blend_time crossfade)" },
    name = { type = "string" },
    anim = { type = "string" },
    loop = { type = "boolean", default = true },
    rate = { type = "number" },
    on_done_anim = { type = "string" },
    blend_time = { type = "number", min = 0, max = 60 },  -- crossfade seconds
})
_schema.define("sma_ik", {
    _meta = { category = "system", blocking = false,
              desc = "SMA 2-bone IK constraint (round 18): chain reaches (tx, ty)" },
    name = { type = "string" },
    bone0 = { type = "number" },  -- root bone id
    bone1 = { type = "number" },  -- mid bone id
    tx = { type = "number" }, ty = { type = "number" },  -- world target
    l1 = { type = "number" },     -- optional root->mid length (default: current)
    l2 = { type = "number" },     -- mid->end length
})
_schema.define("sma_variant", {
    _meta = { category = "system", blocking = false,
              desc = "SMA part variant switch (round 18; E-mote style)" },
    name = { type = "string" },
    part = { type = "string" },     -- part id from the asset's parts list
    variant = { type = "string" },  -- variant name
    tex = { type = "number" },      -- optional per-part texture id override
})
_schema.define("sma_stop", {
    _meta = { category = "system", blocking = false,
              desc = "SMA skeletal-mesh actor despawn (Battle 4d S3)" },
    name = { type = "string" },
})
_schema.define("music", {
    _meta = { category = "system", blocking = false, desc = "KAG3-compatible music command" },
})
_schema.define("ending", {
    _meta = { category = "system", blocking = false, desc = "KAG3-compatible ending command" },
    id = { type = "string" },   -- handler falls back to "end" / params[1]
    name = { type = "string" }, -- handler builds "Ending <id>" when absent
})
_schema.define("history", {
    _meta = { category = "system", blocking = false, desc = "KAG3-compatible history command" },
})

function SystemCommands.gallery(ctx, params)
    local Gallery = require("gallery")
    local startId = params.id
    if type(startId) ~= "string" and type(params[1]) == "string" then
        startId = params[1]
    end
    Gallery.show(ctx, startId)
end

-- [music] — open the music room (preview unlocked BGM tracks)
function SystemCommands.music(ctx, params)
    require("music_room").show(ctx)
end

-- [chapter] — chapter selection overlay; jumps to the chosen *chapter_*
-- label via _pendingJump (the runner's dead-coroutine branch consumes it).
function SystemCommands.chapter(ctx, params)
    local ChapterSelect = require("chapter_select")
    local chosen = ChapterSelect.show(ctx)
    if chosen then
        -- Same signal as history/choice jumps: the scheduler discards
        -- handler returns, so route through ctx.
        ctx._pendingJump = { scene = ctx.current_scene or ctx.currentScene,
                             index = ctx.labelMap and ctx.labelMap[chosen] or 1,
                             target = chosen }
        ctx.stop_flag = true
    end
    return chosen
end

-- [ending id=end01 name="Good End"] — record an unlocked ending.
-- Persisted in ctx.seen_endings (captured by save capture + restored).
function SystemCommands.ending(ctx, params)
    local id = params.id
    if type(id) ~= "string" and type(params[1]) == "string" then
        id = params[1]
    end
    id = id or "end"
    local name = params.name or params[2] or ("Ending " .. id)
    if type(ctx.seen_endings) ~= "table" then ctx.seen_endings = {} end
    ctx.seen_endings[id] = { name = name, at = os.time(), scene = ctx.current_scene or ctx.currentScene or "" }
    print(string.format("[ending] unlocked %s (%s)", id, name))
end

-- Round 51 contracts: [unlock]/[rollback] (audit: handlers lacked
-- schema contracts — no type checks or editor validation).
_schema.define("unlock", {
    _meta = { category = "system", blocking = false, desc = "unlock a gallery CG or music-room track" },
    type = { type = "string", default = "cg" },
    id = { type = "string" },
    name = { type = "string" },
})
_schema.define("rollback", {
    _meta = { category = "system", blocking = true, desc = "pop the newest token-level snapshot and re-run from there" },
})

function SystemCommands.rollback(ctx, params)
    if not ctx then return false end
    local ok, reason = require("kag_runner").rollback()
    if not ok then
        print("[rollback] unavailable: " .. tostring(reason))
        return false
    end
    return true
end

function SystemCommands.unlock(ctx, params)
    -- bare [unlock cg1] -> params[1] as the id (KAG3-style positional);
    -- string-only guard matches the jump/call/link pattern (a raw pair
    -- table at params[1] from a direct caller must not become a key)
    local kind = params.type or "cg"
    local id   = params.id or params.name
    if type(id) ~= "string" and type(params[1]) == "string" then
        id = params[1]
    end
    if type(id) ~= "string" or id == "" then return end

    if kind == "cg" then
        ctx.unlockedCG = ctx.unlockedCG or {}
        ctx.unlockedCG[id] = true
    elseif kind == "music" then
        ctx.unlockedMusic = ctx.unlockedMusic or {}
        ctx.unlockedMusic[id] = true
    end
end

-- =============================================================================
--  Modern utility commands (KAG Neo-Genesis additions)
-- =============================================================================

-- Resolve "f.x" / "sf.x" / "tf.x" / "mp.x" / "lf.x" / bare "x" (-> f.x)
-- to { table = ctx.f, key = "x" }; returns nil for unknown scopes.
local function resolve_var(ctx, var)
    if type(var) ~= "string" or var == "" then return nil end
    local scope, key
    local tname, k = var:match("^([%a_]+)%.([%w_]+)$")
    if tname then
        scope = ({ f = "f", sf = "sf", tf = "tf", mp = "mp", lf = "lf" })[tname]
        key = k
    else
        scope, key = "f", var
    end
    local t = scope and ctx[scope]
    if type(t) ~= "table" then return nil end
    return t, key
end

-- Type inference for [set]/[inc]: "true"/"false" -> boolean, integer ->
-- number, decimal -> number, else string.
local function infer_value(value)
    if type(value) ~= "string" then return value end
    if value == "true" then return true end
    if value == "false" then return false end
    if value:match("^%-?%d+$") then return tonumber(value) end
    if value:match("^%-?%d+%.%d+$") then return tonumber(value) end
    return value
end

--- [set var="f.hp" value="30"] / [set f.hp 30] — typed variable assignment.
--  KAG3 needed [eval tf.x = ...]; [set] is the declarative modern form.
function SystemCommands.set(ctx, params)
    local var = params.var
    if type(var) ~= "string" and type(params[1]) == "string" then
        var = params[1]
    end
    local value = params.value
    if value == nil and type(params[2]) == "string" then value = params[2] end
    if value == nil and params[2] ~= nil then value = tostring(params[2]) end
    local t, key = resolve_var(ctx, var)
    if not t then
        print("[WARN] [set] unknown variable scope: " .. tostring(var))
        return
    end
    t[key] = infer_value(value)
end

--- [inc var="f.count"] / [inc var="f.count" by=2] — increment (KAG3 staple).
--  Also handles decrement with by=-1; nil-safe (missing var starts at 0).
function SystemCommands.inc(ctx, params)
    local var = params.var
    if type(var) ~= "string" and type(params[1]) == "string" then
        var = params[1]
    end
    local by = tonumber(params.by or params[2] or 1) or 1
    local t, key = resolve_var(ctx, var)
    if not t then
        print("[WARN] [inc] unknown variable scope: " .. tostring(var))
        return
    end
    t[key] = (tonumber(t[key]) or 0) + by
end

--- [assert exp="f.hp > 0" msg="hp must be positive"] — development-time
--  assertion: on failure prints a scene:line diagnostic and raises through
--  the engine error path (ctx.handle_error when present) so dev builds
--  catch bad state; release scripts can omit asserts wholesale.
function SystemCommands.assert(ctx, params)
    local exp = params.exp
    if type(exp) ~= "string" and type(params[1]) == "string" then
        exp = params[1]
    end
    if type(exp) ~= "string" or exp == "" then
        print("[WARN] [assert] missing exp=")
        return
    end
    local exprLang = require("kag.expr")
    local ok, value = exprLang.evaluate(ctx, exp)
    if not (ok and value) then
        local where = (ctx.current_scene or ctx.currentScene or "?")
            .. ":" .. tostring(ctx.token_index or ctx.tokenIndex or "?")
        local msg = params.msg or ("assertion failed: " .. exp)
        local full = string.format("[KAG] [assert] %s at %s", msg, where)
        -- Raise only: scheduler.run's pcall reports once with scene:line and
        -- invokes ctx.handle_error exactly once (no duplicate diagnostics).
        error(full, 0)
    end
end

function SystemCommands.random(ctx, params)
    local var = params.var
    if type(var) ~= "string" and type(params[1]) == "string" then
        var = params[1]
    end
    -- Integer-only range (math.random(N, M) requires integers on LuaJIT).
    local min = math.floor(tonumber(params.min or params[2] or 0) or 0)
    local max = math.floor(tonumber(params.max or params[3] or 100) or 100)
    local t, key = resolve_var(ctx, var)
    if not t then
        print("[WARN] [random] unknown variable scope: " .. tostring(var))
        return
    end
    if min > max then min, max = max, min end
    t[key] = math.random(min, max)
end


-- Modern utility contracts (Neo-Genesis additions)
_schema.define("set", {
    _meta = { category = "system", blocking = false, desc = "typed variable assignment (f.x/sf.x/tf.x/mp.x/lf.x)" },
    var   = { type = "string", required = true, positional_index = 1 },
    value = { type = "string", required = true, positional_index = 2 },
})
_schema.define("inc", {
    _meta = { category = "system", blocking = false, desc = "increment a numeric variable (by default 1)" },
    var = { type = "string", required = true, positional_index = 1 },
    by  = { type = "number", default = 1, positional_index = 2 },
})
_schema.define("random", {
    _meta = { category = "system", blocking = false, desc = "write a random integer into a variable" },
    var = { type = "string", required = true, positional_index = 1 },
    min = { type = "number", default = 0, positional_index = 2 },
    max = { type = "number", default = 100, positional_index = 3 },
})
_schema.define("assert", {
    _meta = { category = "system", blocking = false, desc = "development-time assertion on an expression" },
    exp = { type = "string", required = true, positional_index = 1 },
    msg = { type = "string" },
})

-- Input recording / playback control (Neo-Genesis):
--   [replay mode="record"] ... [replay mode="save" file="demo.json"]
--   [replay mode="load" file="demo.json"] [replay mode="playback"]
--   [replay mode="off"]
_schema.define("replay", {
    _meta = { category = "system", blocking = false, desc = "input recording/playback control" },
    mode = { type = "string", required = true, positional_index = 1 },
    file = { type = "string" },
})

function SystemCommands.replay(ctx, params)
    local replay = require("replay")
    local mode = params.mode
    if type(mode) ~= "string" and type(params[1]) == "string" then
        mode = params[1]
    end
    local file = params.file
    if mode == "save" then
        local ok, err = replay.save(file)
        if not ok then
            print("[replay] save failed: " .. tostring(err))
        else
            print("[replay] saved " .. tostring(replay.event_count()) .. " events -> " .. file)
        end
        return ok
    end
    if mode == "load" then
        local n, err = replay.load(file)
        if not n then
            print("[replay] load failed: " .. tostring(err))
        else
            print("[replay] loaded " .. tostring(n) .. " events <- " .. file)
        end
        return n ~= nil
    end
    if mode ~= "off" and mode ~= "record" and mode ~= "playback" then
        print("[replay] unknown mode: " .. tostring(mode))
        return false
    end
    replay.set_mode(mode, file)
    print("[replay] mode -> " .. mode .. (file and (" (" .. file .. ")") or ""))
    return true
end

-- AI-driven dialogue line (Neo-Genesis, distinctive):
--   [ai_dialog name="Aoi" prompt="回应主角的告白" system="你是傲娇青梅竹马" fallback="…"]
-- Queries config.ai endpoint (OpenAI-compatible / Ollama) ASYNCHRONOUSLY
-- (the render loop keeps running while the LLM thinks), waits up to
-- max_wait_ms, then shows the reply as a normal [ch] line. Unavailable
-- service / timeout -> fallback text (or a visible placeholder), never
-- a script error.
_schema.define("ai_dialog", {
    _meta = { category = "system", blocking = true, desc = "AI-driven dialogue line (LLM, async)" },
    name       = { type = "string", default = "" },
    prompt     = { type = "string", required = true, positional_index = 1 },
    system     = { type = "string", default = "" },
    model      = { type = "string", default = "" },
    fallback   = { type = "string", default = "" },
    max_wait_ms = { type = "number", default = 15000, min = 100, max = 120000 },
})

-- --------------------------------------------------------------------------
--  [notify msg="存档完成" time=2500] — author-facing toast notification.
--  Routes to the toast module (scripts/toast.lua) whose public API is
--  Toast.show(msg, ttlSeconds). time is expressed in MILLISECONDS in the
--  KAG contract (toast expects seconds, so the handler divides by 1000).
--  Toast.show itself only mutates module state (no backends/layers touched),
--  so it is headless-safe; a require/show failure degrades to a printed
--  notice and never blocks or breaks the scene (blocking = false).
--  kind is editor-validated metadata for future theming; the current toast
--  API has no kind/position support, so it is accepted but not applied.
_schema.define("notify", {
    _meta = { category = "system", blocking = false,
              desc = "show a brief corner toast notification (author feedback)" },
    msg  = { type = "string", required = true, positional_index = 1 },
    time = { type = "number", default = 2500, min = 0, max = 30000 },  -- ms
    kind = { type = "string", enum = { "info", "warn", "error" } },     -- reserved
})

function SystemCommands.notify(ctx, params)
    local msg = params.msg
    if type(msg) ~= "string" and type(params[1]) == "string" then
        msg = params[1]
    end
    if type(msg) ~= "string" or msg == "" then
        print("[notify] missing msg= (ignored)")
        return true
    end

    -- Clamp ttl to the schema's 0..30000ms range (bare positional args
    -- bypass schema defaults, mirroring the [wait] guard).
    local ttlMs = tonumber(params.time) or 2500
    if ttlMs < 0 then ttlMs = 0 end
    if ttlMs > 30000 then ttlMs = 30000 end

    -- Headless/tests may not have toast (or its backend/layers deps).
    -- pcall both the require and the show call so a missing or degraded
    -- toast module never raises into the scheduler.
    local ok, toast = pcall(require, "toast")
    if ok and type(toast) == "table" and type(toast.show) == "function" then
        local okShow, err = pcall(toast.show, msg, ttlMs / 1000)
        if not okShow then
            print("[notify] toast.show degraded: " .. tostring(err))
        end
    else
        print("[notify] toast unavailable; headless degrade: " .. tostring(msg))
    end
    return true
end

-- [i18n language="zh"] — hot-switch the UI language mid-scene (round 76).
-- Uses the round-75 runtime API (i18n.set_language with the fallback chain
-- current -> default_language -> raw key) and re-localizes the visible
-- page, backlog, active choices and closed captions — the same path the
-- settings menu uses after its language cycle. Headless-safe: a missing
-- dictionary degrades to the printed notice, never raises.
_schema.define("i18n", {
    _meta = { category = "system", blocking = false,
              desc = "hot-switch the UI language mid-scene (language=xx)" },
    language = { type = "string", required = true },
})

function SystemCommands.i18n(ctx, params)
    local lang = params.language
    if type(lang) ~= "string" or lang == "" then
        print("[i18n] missing language= (e.g. [i18n language=\"zh\"])")
        return true
    end
    local ok, i18n = pcall(require, "i18n")
    if not (ok and type(i18n) == "table" and type(i18n.set_language) == "function") then
        print("[i18n] runtime unavailable; degraded: " .. tostring(lang))
        return true
    end
    i18n.set_language(lang)
    ctx.settingsValues = ctx.settingsValues or {}
    ctx.settingsValues.language = lang
    -- Full-page redraw: page/backlog/choices/cc re-localize (the exact
    -- relocalize_page the settings menu uses after its language cycle).
    pcall(function()
        local kt = require("kag.commands.text")
        if kt and kt.relocalize_page then kt.relocalize_page(ctx) end
    end)
    return true
end

function SystemCommands.ai_dialog(ctx, params)
    local backend = require("backend")
    local prompt = params.prompt
    local text = nil
    local err = nil

    if backend.ai_available() then
        local done, result, aerr = false, nil, nil
        local ok = backend.ai_query_async(prompt, {
            system = params.system,
            model = params.model,
        }, function(r, e)
            result, aerr = r, e
            done = true
        end)
        if ok then
            local waited = 0
            while not done do
                local dt = coroutine.yield() or 16
                waited = waited + (tonumber(dt) or 16)
                if waited >= (params.max_wait_ms or 15000) then
                    backend.ai_cancel()
                    break
                end
            end
            if done and result and #result > 0 then
                text = result
            else
                err = aerr or "timeout"
            end
        else
            err = "async-unavailable"
        end
    else
        err = "ai-unavailable"
    end

    if not text then
        text = params.fallback
        if not text or #text == 0 then
            text = err and ("(" .. err .. ")") or "(AI unavailable)"
        end
        print("[ai_dialog] fallback used: " .. tostring(err))
    end

    -- Present as a normal dialogue line (nameplate + textbox + backlog)
    -- through the SAME dispatch table the scheduler uses, so mocks,
    -- hot-reload hooks, and command layering all see one path.
    local kag = require("kag")
    local tc = require("kag.commands.text")
    if kag and type(kag.ch) == "function" then
        kag.ch(ctx, { name = params.name, text = text })
    else
        tc.ch(ctx, { name = params.name, text = text })
    end
    return true
end

-- ═══════════════════════════════════════════════════════════════════════════
--  [steam_achievement id="ACH_01" cond="f.flag" silent=true]
--  Unlock a Steamworks achievement through steam.set_achievement(id) /
--  steam.unlock_achievement(id).
--
--  DEGRADATION IS NOT SILENT (t14). Three outcomes, each distinguishable:
--    * unlocked   -- the binding returned truthy; one "[steam_achievement]
--                    unlocked <id>" line.
--    * refused    -- the binding returned falsy (Null backend: no Steam SDK, no
--                    running client, or an id the app does not declare). A
--                    WARN line names the id, because a missing achievement is
--                    a shipping bug the developer must see.
--    * unavailable-- no steam global at all (engine built without the binding,
--                    or a bare Lua host). WARN naming the id.
--  The command still returns true in every case: a story must not stall or
--  crash because Steam is absent. Pass silent=true to suppress the two WARN
--  lines when a title deliberately runs achievements as best-effort.
-- ═══════════════════════════════════════════════════════════════════════════

function SystemCommands.steam_achievement(ctx, params)
    params = params or {}

    -- Optional [if]-style guard, same shape as [input cond=...]: an achievement
    -- fires only when the expression is truthy.
    if type(params.cond) == "string" and params.cond ~= "" then
        local okExpr, exprLang = pcall(require, "kag.expr")
        if okExpr and exprLang then
            local ok, v
            if params.cond:find("[&|!?]") then
                ok, v = exprLang.evaluate(ctx, params.cond)
            else
                ok, v = exprLang.evaluateTranslated(ctx, params.cond, params.cond)
            end
            if not (ok and v) then return true end
        end
    end

    local id = params.id or params.name or params[1]
    if not id or id == "" then
        print("[WARN] [steam_achievement] ignored: no achievement id given")
        return true
    end
    id = tostring(id)

    local silent = params.silent == true or params.silent == "true"

    -- rawget avoids the sandbox __index panic on an absent global; the
    -- package.loaded fallback is what the Lua-only test host injects.
    local steam = rawget(_G, "steam")
        or (package and package.loaded and package.loaded["steam"])

    if not steam then
        if not silent then
            print(string.format(
                "[WARN] [steam_achievement] %s NOT unlocked: no Steam binding in "
                .. "this build/host", id))
        end
        -- Record the attempt so a debug overlay / test can see what was tried
        -- even when Steam is absent (ctx.tf is the documented no-trap scratch).
        if ctx and type(ctx.tf) == "table" then
            ctx.tf.steam_achievement_result = "unavailable"
        end
        return true
    end

    local fn = steam.set_achievement
    if type(fn) ~= "function" then fn = steam.unlock_achievement end
    if type(fn) ~= "function" then
        if not silent then
            print(string.format(
                "[WARN] [steam_achievement] %s NOT unlocked: steam binding has no "
                .. "set_achievement/unlock_achievement", id))
        end
        if ctx and type(ctx.tf) == "table" then
            ctx.tf.steam_achievement_result = "unavailable"
        end
        return true
    end

    -- pcall: a backend that raises must not take the scene down with it.
    local called, unlocked = pcall(fn, id)
    if called and unlocked then
        print(string.format("[steam_achievement] unlocked %s", id))
        if ctx and type(ctx.tf) == "table" then
            ctx.tf.steam_achievement_result = "unlocked"
        end
        return true
    end

    if not silent then
        print(string.format(
            "[WARN] [steam_achievement] %s NOT unlocked: Steam refused it "
            .. "(no SDK/client, or the app does not declare this id)", id))
    end
    if ctx and type(ctx.tf) == "table" then
        ctx.tf.steam_achievement_result = "refused"
    end
    return true
end

return SystemCommands
