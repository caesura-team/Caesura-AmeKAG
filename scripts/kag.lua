-- =============================================================================
--  Caesura (AmeKAG) �� kag.lua
--  KAG command handler table. The scheduler dispatches kag[cmd](ctx, params)
--  for every non-flow-control tag in the token stream.
--  Flow commands (if/jump/call/return/label/end/macro/eval/wait/stop)
--  are handled inline by scheduler.lua and never reach this module.
--
--  Phase 4: Loads all command sub-modules (layer, text, audio, system,
--  transition, video) and merges them into the unified KAG table.
-- =============================================================================

local KAG = {}
local flow = require("flow")

-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T
--  Layer commands �� [bg], [fg], [cl], [image]
--  Loaded from kag/commands/layer.lua, wired to layers.lua + backend.lua.
-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T

local layer_cmds = require("kag.commands.layer")
-- Standalone effect aliases ([flash]/[shake]/[quake]) route through the
-- vfx handler table (registered before layer_cmds so the plain names
-- dispatch without a [vfx] wrapper).
-- NOTE: "particles" is ALSO a schema name; VFX.particles (added in
-- the schema-vs-handler audit) is a REAL handler key, so the pairs
-- loop registers [particles] as a command -- no filter needed.
local vfx_cmds = require("kag.commands.vfx")
for name, handler in pairs(vfx_cmds) do
    if name:sub(1,1)~='_' then KAG[name] = handler end
end

for name, handler in pairs(layer_cmds) do
    KAG[name] = handler
end

-- [tween] declarative tween commands (round 106): merged after layer so
-- attr reads/writes resolve through the same layer/speaker path.
local tween_cmds = require("kag.commands.tween")
for name, handler in pairs(tween_cmds) do
    KAG[name] = handler
end

-- [layout] declarative layout containers (round 107): pure coordinate
-- calculators writing existing layer x/y; no new render path.
local layout_cmds = require("kag.commands.layout")
for name, handler in pairs(layout_cmds) do
    KAG[name] = handler
end

-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T
--  Text commands �� [ch], [text], [l], [r], [er], [p]
--  Loaded from kag/commands/text.lua, delegates to backend font rendering.
-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T

local text_cmds = require("kag.commands.text")
for name, handler in pairs(text_cmds) do
    KAG[name] = handler
end

-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T
--  Audio commands �� [playbgm], [stopbgm], [playse], [playvoice], [fadebgm],
--  [xfadebgm], [stopse], [stopvoice], [waitsound], [waitbgm],
--  [setbgmvolume], [setsevolume], [setvoicevolume]
--  Loaded from kag/commands/audio.lua, wired to backend audio proxy.
-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T

local audio_cmds = require("kag.commands.audio")
for name, handler in pairs(audio_cmds) do
    KAG[name] = handler
end

-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T
--  System commands �� [wait], [emb]
--  Loaded from kag/commands/system.lua. CancelToken-integrated blocking.
--  Note: [eval] is handled inline by scheduler.lua, not here.
-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T

local system_cmds = require("kag.commands.system")
for name, handler in pairs(system_cmds) do
    KAG[name] = handler
end
-- KAG3 compatibility aliases on the system table:
--   [end]  -> ending (KAG3's standard end-of-game tag; the demo scenes use
--            bare [end] and it must not render as text)
--   NOTE: "end" is a Lua keyword, so bracket syntax is required.
KAG["end"] = system_cmds.ending

-- ===========================================================================
--  Math commands -- [add]/[sub]/[mul]/[div]/[mod]/[dec] (KAG3 compat)
--  Loaded from kag/commands/math.lua (round 71). Registered by pairs so the
--  table keys become dispatchable commands.
-- ===========================================================================
do
    local math_cmds = require("kag.commands.math")
    for name, handler in pairs(math_cmds) do
        KAG[name] = handler
    end
end

-- ===========================================================================
--  Character commands -- [csp]/[csd]/[csl] (KAG3 compat)
--  Loaded from kag/commands/character.lua (round 71).
-- ===========================================================================
do
    local character_cmds = require("kag.commands.character")
    for name, handler in pairs(character_cmds) do
        KAG[name] = handler
    end
end

-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T
--  Saveplace / Loadplace �� in-memory scene bookmarks
--  Spec [10.2.38]: independent of save system, no disk writes.
--  Wired to system.lua System.saveplace/loadplace.
-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T

do
    local System = require("system")
    KAG.saveplace = function(ctx, params) System.saveplace(ctx) end
    KAG.loadplace = function(ctx, params) System.loadplace(ctx) end
end

-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T
--  Save/Load commands �� [save], [load], [listsaves]
--  Loaded from kag/commands/save.lua, wired to C++ SaveManager.
-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T

do
    local save_cmds = require("kag.commands.save")
    KAG.save = save_cmds.save
    KAG.load = save_cmds.load
    KAG.listsaves = save_cmds.listsaves
    -- Audit fix: saveload/saveplace/loadplace existed in SaveCommands but
    -- were NEVER registered -- [saveload] etc. in a .ks script hit a nil
    -- handler (silent no-op at best, error at worst).
    KAG.saveload = save_cmds.saveload
    KAG.saveplace = save_cmds.saveplace
    KAG.loadplace = save_cmds.loadplace
end

-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T
--  Transition commands �� [trans], [move], [quake], [fade]
--  Loaded from kag/commands/transition.lua, wired to GPU transition engine.
-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T

local trans_cmds = require("kag.commands.transition")
for name, handler in pairs(trans_cmds) do
    -- Skip-list (review nit): flash/quake have standalone aliases bound
    -- explicitly later (vfx routing) -- the transition handlers would
    -- silently override them here if their names ever collide again.
    if name ~= "flash" and name ~= "quake" then
        KAG[name] = handler
    end
end
-- Fallback invariant (review nit): if vfx.lua ever drops the flash
-- export, keep [flash] registered from the transition table rather than
-- silently unregistered. (shake/quake are bound by explicit wrappers.)
if not KAG.flash then
    KAG.flash = trans_cmds.flash
end

-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T
--  Video commands �� [video], [stopvideo]
--  Loaded from kag/commands/video.lua, wired to pl_mpeg backend.
-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T

local video_cmds = require("kag.commands.video")
for name, handler in pairs(video_cmds) do
    KAG[name] = handler
end

-- ===========================================================================
--  Skeletal mesh animation commands — [sma_play] / [sma_stop]
--  Loaded from kag/sma.lua (SMA Battle 4d S3 driver).
-- ===========================================================================
local sma_cmds = require("kag.sma")
for name, handler in pairs(sma_cmds.commands) do
    KAG[name] = handler
end

-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T

-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T
--  Resource commands �� [preload]
--  Spec [10.2.32]: async asset preloading with placeholder textures.
--  Loaded from kag/commands/resource.lua.
-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T

local resource_cmds = require("kag.commands.resource")
for name, handler in pairs(resource_cmds) do
    KAG[name] = handler
end


-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T
--  VFX commands �� [vfx type="particle|quake|shake|flash|fade|blur|stop"]
--  Loaded from kag/commands/vfx.lua, wired to vfx.lua + particle system.
-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T

-- (vfx commands registered at the top with the standalone aliases --
-- this second loop was redundant; folded per review nit)
--  Legacy aliases �� backward compatibility
-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T

-- [showtext] �� alias for [text]
KAG.showtext = KAG.text

-- [clearscreen] �� alias for [cl]
KAG.clearscreen = KAG.cl

-- [br] �� line break (decorative, same as [l])
function KAG.br(ctx, params)
    if KAG.l then KAG.l(ctx, params) end
end

-- [hr] -- horizontal rule (decorative separator; TextCommands.hr draws it)
function KAG.hr(ctx, params)
    if text_cmds and text_cmds.hr then return text_cmds.hr(ctx, params) end
end

-- [cancel] �� cancel current voice/transition (backward compat)
function KAG.cancel(ctx, params)
    local backend = require("backend")
    if backend.audio_stop then backend.audio_stop("voice") end
    require("kag.operation").cancel_all(ctx)
    ctx.waiting_input = false
end

-- [close] �� close active scene, return to menu (backward compat)
function KAG.close(ctx, params)
    local backend = require("backend")
    if backend.audio_stop then
        backend.audio_stop("bgm")
        backend.audio_stop("voice")
        backend.audio_stop("se")
    end
    require("kag.operation").cancel_all(ctx)
    return "stop"
end

-- [macro] / [endmacro] / [erasemacro] �� handled inline by scheduler
-- These stubs exist for documentation only
function KAG.macro(ctx, params) end
function KAG.endmacro(ctx, params) end
function KAG.erasemacro(ctx, params) end

-- ===========================================================================
--  KAG 3.0 compatibility aliases
--  Lets scripts written for the Japanese KiriKiri/KAG3 tag set run mostly
--  unchanged on Caesura (differentiator vs Ren'Py/Tyrano).
-- ===========================================================================

-- Neo-Genesis contracts: KAG3-compat commands typed + validated.
local _schema = require("kag.schema")
_schema.define("saveplace", {
    _meta = { category = "save", blocking = false, desc = "KAG3-compatible saveplace command" },
})
_schema.define("loadplace", {
    _meta = { category = "save", blocking = false, desc = "KAG3-compatible loadplace command" },
})
_schema.define("listsaves", {
    _meta = { category = "save", blocking = false, desc = "KAG3-compatible listsaves command" },
})
_schema.define("br", {})
_schema.define("hr", {})
_schema.define("cancel", {
    _meta = { category = "system", blocking = false, desc = "cancel current voice/transition (KAG3 compat)" },
    layer = { type = "string", default = "" },
    all = { type = "boolean", default = false },
})
_schema.define("close", {
    _meta = { category = "system", blocking = false, desc = "close active scene, return to menu (KAG3 compat)" },
})
_schema.define("ld", {
    _meta = { category = "layer", blocking = false, desc = "delete a layer (KAG3 compat)" },
    layer = { type = "string" },
    name = { type = "string" },
})
-- (no shake define here: vfx.lua owns the [shake] contract -- a
-- duplicate would silently override its frequency clamp)
_schema.define("playstop", {
    _meta = { category = "audio", blocking = false, desc = "stop BGM playback (KAG3 compat)" },
    fadeout = { type = "number", default = 0, min = 0, max = 30000 },
})
_schema.define("waitforclick", {
    _meta = { category = "text", blocking = true, desc = "block until the player clicks" },
})

-- [r] -- line break (KAG3); same as [l]
KAG.r = KAG.l or KAG.br

-- [voice_wait] -- wait for voice with click-to-skip (Neo-Genesis).
-- No timeout param: the waiting_input token isn't frame-resumed in this
-- runner, so a deadline check can never fire. Promise nothing we can't
-- keep (see the review chain).
require("kag.schema").define("voice_wait", {
    _meta = { category = "text", blocking = true, desc = "wait for a voice line with click-to-skip" },
})
KAG.voice_wait = function(ctx, params)
    return require("kag.commands.audio").voice_wait(ctx, params)
end

-- [s] -- KAG3 short-wait control char; unified through [wait]
-- (default 250ms matches KAG3's s-char pacing).
KAG.s = function(ctx, params)
    params = params or {}
    return require("kag.commands.system").wait(ctx, {
        ms = tonumber(params.ms) or 250,
    })
end
-- [waitclick] -- wait for a click WITHOUT clearing the page (KAG3
-- semantics; [p] clears). Same runner contract: waiting_input blocks
-- the scheduler until the click resumes it.
function KAG.waitclick(ctx, params)
    if ctx then ctx.waiting_input = true end
    if coroutine.running() then
        coroutine.yield()
    end
end

-- KAG.wait_click() -- the backend.wait_click() forwarding target (was
-- undefined: the demo's direct-API path called into nil). Coroutine
-- context suspends like [waitclick]; a direct-API call without a
-- coroutine errors loudly instead of hanging the frame loop.
function KAG.wait_click()
    if coroutine.running() then
        local ctx = rawget(_G, "_CAESURA_CTX")
        if ctx then ctx.waiting_input = true end
        coroutine.yield()
        return true
    end
    error("wait_click() outside a coroutine: use it from a KAG scene "
        .. "or a frame callback (it cannot block the direct API)", 0)
end

-- [delay ms=N] -- KAG3 duplicate of [wait]; unified through the wait
-- command + its schema contract (Neo-Genesis: one implementation, aliases
-- share it). delay's ms param maps onto wait's ms field.
KAG.delay = function(ctx, params)
    -- bare positional [delay 500] -> params[1] (tokenizer bare-value
    -- support). STRING-only guard: with [delay ms=500] the scheduler's
    -- key normalization leaves params[1] as the raw pair table
    -- ({"ms","500"}) -- tonumber of that is nil and must NOT clobber
    -- the coerced ms (review blocking: it silently fell to 1000ms).
    if type(params[1]) == "string" and params.ms == nil then
        params.ms = tonumber(params[1])
    end
    return require("kag.commands.system").wait(ctx, params)
end
-- [clear] -- clear text layer (KAG3); alias for [cl]
KAG.clear = KAG.cl

-- [ct] -- clear text/message (KAG3); same as [clear]
KAG.ct = KAG.cl

-- [endtag] -- generic block end (KAG3 legacy; no-op -- the Neo-Genesis
-- grammar already closes iscript/macro blocks explicitly)
KAG.endtag = function() end

-- [endform] -- KAG3 form-system end (forms are not implemented; safe
-- no-op so legacy scripts do not error)
KAG.endform = function() end

-- [g storage=x] -- KAG3 graphic display; same as [bg]
KAG.g = function(ctx, params)
    return KAG.bg(ctx, params)
end

-- [fadeout layer="bg" opacity=0 time=500] -- fade a layer out (KAG3);
-- schema existed but NO handler was registered, so the scheduler's
-- fallback rendered 'fadeout' as dialogue (audit: same class as the
-- [delay] gap). Delegates to layfade with opacity defaulting to 0.
KAG.fadeout = function(ctx, params)
    local Layer = require("kag.commands.layer")
    -- the fadeout SCHEMA is 0..1, but layfade/fade_to operate in
    -- 0..255 -- pass an explicit opacity straight through would fade
    -- [fadeout opacity=0.5] to 0.5/255 ~= transparent (review
    -- should-fix). Convert here, like layopt's set_options does.
    -- tonumber first: a non-numeric opacity string would raise in the
    -- multiplication (audit: string-param crash sweep)
    local opacity255 = math.floor((tonumber(params.opacity or params.alpha) or 0) * 255)
    return Layer.layfade(ctx, {
        layer = params.layer or params.name or "bg",
        opacity = opacity255,
        time = params.time or params.duration or 500,
    })
end

-- [waitforclick] -- block until the player clicks (KAG3 control flow)
function KAG.waitforclick(ctx, params)
    if not ctx then return end
    ctx.waiting_input = true
    while ctx.waiting_input do
        coroutine.yield()
    end
end

-- [ld] -- delete layer (KAG3 layer delete)
function KAG.ld(ctx, params)
    local layers = require("layers")
    local name = params.layer or params.name or params[1]
    if name then
        local node = layers.get_layer(name)
        if node then
            node.visible = false
            node.texture = nil
        end
        -- Clear the dedup state so the next [bg] reloads (review nit:
        -- a hidden-then-reasserted bg must not reuse a nil texture).
        if name == "bg" and ctx and ctx.layers then
            ctx.layers.bg = nil
        end
    end
end

-- [shake] / [quake] -- screen shake (KAG3 classic effect).
-- quake routes through the vfx handler (review should-fix: binding it
-- to KAG.shake made standalone [quake] run a shake).
function KAG.shake(ctx, params)
    local vfx = require("kag.commands.vfx")
    if vfx.shake then vfx.shake(ctx, params) end
end
function KAG.quake(ctx, params)
    local vfx = require("kag.commands.vfx")
    if vfx.quake then vfx.quake(ctx, params) end
end

-- [playstop] -- stop BGM (KAG3)
function KAG.playstop(ctx, params)
    local audio = require("kag.commands.audio")
    audio.stopbgm(ctx, params)
end

-- [voice file=X] -- play voice (KAG3)
function KAG.voice(ctx, params)
    local schema = require("kag.schema")
    if schema.isMigrated("play") then
        params = schema.coerce("play", params, ctx)
    end
    local audio = require("kag.commands.audio")
    audio.playvoice(ctx, { file = params.file or params[1], storage = params.storage })
end

-- [se file=X] -- KAG3 alternate; unified through [play bus=se]
function KAG.se(ctx, params)
    -- Route through the play contract so volume is clamped here too
    -- (the alias bypasses the [play] dispatch coerce).
    local schema = require("kag.schema")
    if schema.isMigrated("play") then
        params = schema.coerce("play", params, ctx)
    end
    local audio = require("kag.commands.audio")
    audio.playse(ctx, { file = params.file or params[1], storage = params.storage, volume = params.volume })
end

-- [play bus=bgm|se|voice file=X volume=...] -- Neo-Genesis unified audio
-- command: one entry for all three buses (KAG3 needed play/bgm/se/voice
-- as separate commands with duplicated param handling).
-- Neo-Genesis contract: unified audio entry gets typed params (bus choices
-- replace the manual string compare; volume clamped like the audio cmds).
require("kag.schema").define("play", {
    _meta = { category = "audio", blocking = false, desc = "play audio on bus=bgm|se|voice (Neo-Genesis unified)" },
    bus     = { type = "string", choices = { ["bgm"] = true, ["se"] = true, ["voice"] = true } },
    file    = { type = "string" },
    storage = { type = "string" },  -- KAG3 alias
    volume  = { type = "number", min = 0, max = 1.5 },  -- no default: positional
})

function KAG.play(ctx, params)
    local audio = require("kag.commands.audio")
    local bus = params.bus or "bgm"
    if bus == "bgm" then
        return audio.playbgm(ctx, { file = params.file or params[1], storage = params.storage, volume = params.volume })
    elseif bus == "se" then
        return audio.playse(ctx, { file = params.file or params[1], storage = params.storage, volume = params.volume })
    elseif bus == "voice" then
        return audio.playvoice(ctx, { file = params.file or params[1], storage = params.storage })
    end
    print("[play] unknown bus: " .. tostring(bus))
end

-- [bgm file=X] -- KAG3 alternate; unified through [play bus=bgm]
-- (defined AFTER the unified play so it binds the bus-aware handler)
-- The "bgm" command name itself is contract-migrated so the scheduler
-- coerces [bgm volume=9] BEFORE dispatch (security: no amplification).
require("kag.schema").define("bgm", {
    _meta = { category = "audio", blocking = false, desc = "play BGM (KAG3 alternate for [play bus=bgm])" },
    file   = { type = "string" },
    storage = { type = "string" },
    volume = { type = "number", min = 0, max = 1.5 },  -- no default: positional
})
KAG.bgm = KAG.play

-- ===========================================================================
--  Lua → KAG flow-control API
--  Called from [iscript] blocks, [emb] expressions, or external Lua scripts.
--  These set ctx._next_index so the scheduler takes the jump on the next
--  coroutine resume.  Code after these calls still executes until the next
--  yield — use `return` to stop immediately after scheduling a jump.
-- ===========================================================================

--- kag.jump(ctx, target) — intra-scene label jump
function KAG.jump(ctx, target)
    if not ctx or not target then return end
    local idx = flow.find_label(ctx.tokens, target:gsub("^*", ""))
    if idx then
        ctx._next_index = idx
    else
        print("[kag.jump] Label not found: " .. tostring(target))
    end
end

--- kag.call(ctx, target) — subroutine call (push call stack, jump to label)
function KAG.call(ctx, target)
    if not ctx or not target then return end
    local idx = flow.find_label(ctx.tokens, target:gsub("^*", ""))
    if idx then
        ctx.call_stack = ctx.call_stack or {}
        table.insert(ctx.call_stack, {
            tokens = ctx.tokens,
            index  = ctx.token_index,
        })
        ctx._next_index = idx
    else
        print("[kag.call] Label not found: " .. tostring(target))
    end
end

--- kag.return_to_caller(ctx) — return from subroutine
function KAG.return_to_caller(ctx)
    if not ctx then return end
    if ctx.call_stack and #ctx.call_stack > 0 then
        local frame = table.remove(ctx.call_stack)
        ctx._next_index = (frame.index or 1) + 1
    end
end

-- t125: DEFAULT gesture hooks for the native SwipeDown/SwipeUp consumers.
-- Engine.cpp routes SDLK_SPACE -> _KAG_onKeySpace and SDLK_PAGEUP ->
-- _KAG_onKeyPageUp (same guard style as _KAG_onCtrlDown). The web player
-- layer serves these gestures to every game (web/main.mjs:634-647); the
-- runtime default gives every native game the same parity. Semantics
-- mirror kag_demo_entry.lua's override:
--   SwipeDown -> toggle message-layer visibility
--   SwipeUp   -> open the backlog/history overlay (input_focus guard +
--                single-flight coroutine + kag.commands.system.history)
-- Override rule: an entry that defines the same globals AFTER
-- require("kag") wins (the demo entry is the reference override -- its
-- engine_update drives the overlay frames; see the driver caveat at
-- kag_demo_entry.lua).
-- Headless/defensive: ctx or message layer may be absent -> no-op.
-- NOTE: single-flight state lives on ctx (_gesture_history_co) so tests
-- can drive the real overlay coroutine (drift-proof) and per-context
-- games stay isolated.
local function defaultKeySpace()
    local ctx = _G._CAESURA_CTX
    if not ctx then return end
    -- t130 (M-F1): bind the layers module like the :398 precedent (function-
    -- local require); the bare global `layers` does NOT exist at kag.lua
    -- scope (only the demo entry's closure sees an entry-local `layers`),
    -- so a non-override entry pressing SPACE/SwipeDown would nil-call.
    -- pcall keeps the headless contract: layers module absent -> no-op.
    local ok, layers = pcall(require, "layers")
    if not ok or not layers or type(layers.get) ~= "function" then return end
    local msg = layers.get("message")
    if msg then
        msg.visible = not msg.visible
    end
end

local function defaultKeyPageUp()
    local ctx = _G._CAESURA_CTX
    if ctx and ctx.input_focus ~= "history" and not ctx._gesture_history_co then
        ctx._gesture_history_co = coroutine.create(function()
            require("kag.commands.system").history(ctx, {})
        end)
    end
end

-- Public contract (t127 suggest 2): KAG.gesture_defaults are the canonical
-- runtime defaults for the native swipe gestures. Entries override by
-- defining _G._KAG_onKeySpace / _G._KAG_onKeyPageUp AFTER require("kag")
-- (first-definition-wins at install time). Both defaults are headless-safe
-- (no ctx / no message layer / layers module absent -> no-op). PAGEUP state
-- lives on ctx._gesture_history_co (single-flight, per-context); the overlay
-- coroutine needs a frame driver (see kag_demo_entry.lua override).
-- Exported for tests and future entries: the canonical runtime defaults.
KAG.gesture_defaults = {
    space = defaultKeySpace,
    page_up = defaultKeyPageUp,
}

_G._KAG_onKeySpace = _G._KAG_onKeySpace or defaultKeySpace
_G._KAG_onKeyPageUp = _G._KAG_onKeyPageUp or defaultKeyPageUp

-- NOTE: KAG.save_game / KAG.load_game are C bindings registered by
-- SaveBinding (src/script/bindings/SaveBinding.cpp). Do not redefine them
-- here: a Lua-level redefinition shadows the C functions and breaks the
-- [save]/[load] command path.

local capabilityRuntime = require("capability_runtime")
for name, handler in pairs(KAG) do
    if type(handler) == "function" then
        KAG[name] = capabilityRuntime.wrap_command(handler, name)
    end
end
return KAG
