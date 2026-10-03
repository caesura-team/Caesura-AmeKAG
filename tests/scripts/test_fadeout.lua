-- test_fadeout.lua — [fadeout] layfade delegation (audit)
package.path = "scripts/?.lua;scripts/kag/?.lua;" .. package.path
local passed, failed = 0, 0
local function check(name, cond)
    if cond then print("PASS " .. name) passed = passed + 1
    else print("FAIL " .. name) failed = failed + 1 end
end

local KAG = require("kag")
local tokenizer = require("tokenizer")
check("fadeout registered", type(KAG.fadeout) == "function")
local t = tokenizer.parse('[fadeout layer="bg" time=500]')
check("fadeout parses", t[1].cmd == "fadeout" and t[1].params[1][2] == "bg")

-- behavior: delegates to layfade with opacity 0 (default fade-out)
local Layer = require("kag.commands.layer")
local calls = {}
local real_layfade = Layer.layfade
Layer.layfade = function(ctx, p)
    calls[#calls + 1] = p
end
local ctx = { f = {}, tf = {}, sf = {}, mp = {}, variables = {} }
local ok = pcall(KAG.fadeout, ctx, { layer = "bg", time = 700 })
Layer.layfade = real_layfade
check("fadeout delegates", ok and calls[1]
      and calls[1].layer == "bg" and calls[1].to == 0 and calls[1].time == 700)

-- explicit opacity converts 0..1 -> 0..255 (review should-fix: the
-- schema is 0..1 but layfade/fade_to operate in 0..255 -- passing 0.3
-- straight through would fade to ~transparent)
calls = {}
Layer.layfade = function(ctx, p) calls[#calls + 1] = p end
pcall(KAG.fadeout, ctx, { layer = "fg", opacity = 0.3 })
Layer.layfade = real_layfade
check("fadeout explicit opacity scaled", calls[1] and calls[1].to == 76)
-- default layer aligned with layfade (bg)
calls = {}
Layer.layfade = function(ctx, p) calls[#calls + 1] = p end
pcall(KAG.fadeout, ctx, { time = 500 })
Layer.layfade = real_layfade
check("fadeout default layer bg", calls[1] and calls[1].layer == "bg")
-- schema accepts layer (review nit)
local schema = require("kag.schema")
local c4 = schema.coerce("fadeout", { layer = "bg", opacity = "0.5" }, {})
check("fadeout layer in schema", c4.layer == "bg")

-- schema contract intact (opacity clamp)
local schema = require("kag.schema")
local c = schema.coerce("fadeout", { opacity = "9", time = "1" }, {})
check("fadeout opacity clamped", c.opacity == 1.0)

-- schema-vs-handler audit: fadevol + particles now have handlers
local Audio = require("kag.commands.audio")
check("fadevol registered", type(KAG.fadevol) == "function")
local fade_calls = {}
local be_backup = _G._CAESURA_BACKEND
_G._CAESURA_BACKEND = { render = function() return true end,
    audio = function(cmd, ...)
        if cmd == "fade_volume" then fade_calls[#fade_calls + 1] = { ... } end
        return true end }
pcall(Audio.fadevol, ctx, { volume = 0.5, time = 2000 })
check("fadevol delegates", fade_calls[1] and fade_calls[1][2] == 0.5
      and fade_calls[1][3] == 2.0)
_G._CAESURA_BACKEND = be_backup

check("particles registered", type(KAG.particles) == "function")
local p_calls = {}
-- the backend forwards the emitter trio to the GLOBAL VFX binding
local vfx_backup = _G.VFX
_G.VFX = {
    particles_create_emitter = function(cfg) p_calls[#p_calls + 1] = cfg return 7 end,
    particles_emit = function(e, n) p_calls[#p_calls + 1] = { "emit", e, n } end,
    particles_destroy_emitter = function() end,
    particles_clear = function() end,
}
local ctxP = { f = {}, tf = {}, sf = {}, mp = {}, variables = {} }
local okP = pcall(KAG.particles, ctxP, { action = "create", x = 10, rate = 20 })
check("particles create", okP and p_calls[1] and p_calls[1].rate == 20
      and ctxP._particleEmitters and ctxP._particleEmitters[7] == true)
pcall(KAG.particles, ctxP, { action = "emit", emitter = 7, count = 5 })
check("particles emit forwards", #p_calls == 2 and p_calls[2][1] == "emit"
      and p_calls[2][2] == 7 and p_calls[2][3] == 5)
pcall(KAG.particles, ctxP, { action = "destroy", emitter = 7 })
check("particles destroy tracked", ctxP._particleEmitters[7] == nil)
-- recreate an emitter so the clear reset actually has work to do
-- (review nit: destroy already emptied the table, making the empty
-- half vacuous)
pcall(KAG.particles, ctxP, { action = "create", rate = 5 })
local okClear = pcall(KAG.particles, ctxP, { action = "clear" })
check("particles clear ok", okClear and ctxP._particleEmitters ~= nil
      and next(ctxP._particleEmitters) == nil)
_G.VFX = vfx_backup

-- layfade scale (audit): <=1 treated as 0..1 fraction, >1 as 0..255.
-- Drive through the REAL layer tree (the module's local layers
-- binding cannot be mocked -- a mock get() returned nothing, so
-- layfade bailed with 'layer not found').
local Layer2 = require("kag.commands.layer")
local real_layers = require("layers")
local node1 = real_layers.add_layer(nil, {
    name = "_lf1", layer_type = 0, x = 0, y = 0, w = 10, h = 10,
    visible = true, opacity = 255 })
pcall(Layer2.layfade, { f = {}, tf = {}, sf = {}, mp = {}, variables = {} },
      { layer = "_lf1", opacity = 0.5, time = 1 })
check("layfade fraction scaled", node1.opacity == 128)
local node2 = real_layers.add_layer(nil, {
    name = "_lf2", layer_type = 0, x = 0, y = 0, w = 10, h = 10,
    visible = true, opacity = 255 })
pcall(Layer2.layfade, { f = {}, tf = {}, sf = {}, mp = {}, variables = {} },
      { layer = "_lf2", opacity = 128, time = 1 })
check("layfade 0-255 passthrough", node2.opacity == 128)
-- STRING params (tokenizer/scheduler shape -- review should-fix: the
-- raw string used to raise on the <= compare)
local node3 = real_layers.add_layer(nil, {
    name = "_lf3", layer_type = 0, x = 0, y = 0, w = 10, h = 10,
    visible = true, opacity = 255 })
local okStr = pcall(Layer2.layfade, { f = {}, tf = {}, sf = {}, mp = {}, variables = {} },
      { layer = "_lf3", opacity = "0.5", time = 1 })
check("layfade string param safe", okStr and node3.opacity == 128)

if failed > 0 then os.exit(1) end
print("FADEOUT TESTS DONE")
