-- =============================================================================
-- test_vfx_postfx.lua - round 102 post-processing chain (Lua/kag contract).
--
-- Headless test for the [vfx postfx=...] contract. Uses the SAME mock
-- kag/backend pattern as test_vfx_clamp.lua:
--   * backend  : recorder with a no-op __index fallback
--   * layers   : stub
--
-- The round-102 [vfx] schema is VALIDATION-ONLY: postfx is an enum
-- {bloom,vignette,lut,softblur,none} with NO default, so legacy [vfx]
-- effects (fade/blur/...) are unaffected. When postfx= is present the
-- command routes to the C++ PostFx chain via backend.clear_postfx /
-- is_postfx_supported / set_postfx; postfx=none clears the chain.
--
-- Covers:
--   (1) postfx=none closes safely (clear_postfx, still returns cleanly);
--   (2) invalid postfx value rejected by schema (pcall false);
--   (3) missing-param defaults (strength 1.0, radius 0, amount 0, lutMix 0);
--   (4) fade and timed scene blur route to their own effects without postfx=.
--
-- =============================================================================

package.path = 'scripts/?.lua;scripts/kag/?.lua;' .. package.path
local passed, failed = 0, 0
local function check(name, cond)
    if cond then print('PASS ' .. name) passed = passed + 1
    else print('FAIL ' .. name) failed = failed + 1 end
end

-- -----------------------------------------------------------------------------
-- Mocks (mirror test_vfx_clamp.lua)
-- -----------------------------------------------------------------------------
local calls = {}
local backendMock = setmetatable({
    submit_vfx = function(...)
        calls[#calls + 1] = { 'submit_vfx', ... }
    end,
    set_scene_blur = function(radius)
        calls[#calls + 1] = { 'set_scene_blur', radius }
        return true
    end,
    clear_postfx = function()
        calls[#calls + 1] = { 'clear_postfx' }
    end,
    set_postfx = function(kind, pf)
        calls[#calls + 1] = { 'set_postfx', kind, pf }
    end,
    is_postfx_supported = function(kind)
        calls[#calls + 1] = { 'is_postfx_supported', kind }
        return false  -- headless: Null device reports unsupported (no-op)
    end,
}, { __index = function() return function() end end })

local savedBackend = package.loaded['backend']
local savedLayers = package.loaded['layers']
package.loaded['backend'] = backendMock
_G.backend = backendMock
package.loaded['layers'] = { forEach = function() end, get_layer = function() return nil end, find = function() return nil end }

local schemaMod = require('kag.schema')
local VFXCommands = require('kag.commands.vfx')

check('[vfx] schema is migrated (round 102)', schemaMod.isMigrated('vfx'))
local vfxSpec = schemaMod.specs('vfx') or {}
check('[vfx] schema declares postfx enum', type(vfxSpec.postfx) == 'table' and vfxSpec.postfx.type == 'enum')

-- -----------------------------------------------------------------------------
-- (4) no postfx parameter: direct fade and temporary full-screen blur.
-- -----------------------------------------------------------------------------
do
    calls = {}
    local ctx = { _bgTexture = 9 }
    local p = schemaMod.coerce('vfx', { type = 'fade', time = '10', r = '255', g = '0', b = '0' }, ctx)
    VFXCommands.vfx(ctx, p)
    local ef, tx = nil, nil
    for _, c in ipairs(calls) do
        if c[1] == 'submit_vfx' then tx, ef = c[2], c[3] end
    end
    check('fade (no postfx) routes to submit_vfx effect 1', ef == 1)
    check('fade (no postfx) keeps target texture', tx == 9)
end

do
    calls = {}
    local ctx = {}
    local p = schemaMod.coerce('vfx', { type = 'blur', time = '10', strength = '4' }, ctx)
    local co = coroutine.create(function() VFXCommands.vfx(ctx, p) end)
    assert(coroutine.resume(co))
    assert(coroutine.resume(co, 16))
    check('blur (no postfx) applies the requested radius through a render epoch',
        calls[1] and calls[1][1] == 'set_scene_blur' and calls[1][2] == 4
        and coroutine.status(co) == 'suspended')
    assert(coroutine.resume(co, 16))
    check('blur clears only the temporary scene effect', #calls == 2
        and calls[2][1] == 'set_scene_blur' and calls[2][2] == 0
        and coroutine.status(co) == 'dead')
end

-- -----------------------------------------------------------------------------
-- (3) missing-param defaults (postfx path).
-- -----------------------------------------------------------------------------
-- (3a) headless (Null) device: postfx unsupported -> no-op, never sets the chain.
do
    calls = {}
    local ctx = {}
    local p = schemaMod.coerce('vfx', { postfx = 'softblur' }, ctx)
    VFXCommands.vfx(ctx, p)
    local probed, setTo = false, false
    for _, c in ipairs(calls) do
        if c[1] == 'is_postfx_supported' then probed = true end
        if c[1] == 'set_postfx' then setTo = true end
    end
    check('unsupported device probes is_postfx_supported', probed)
    check('unsupported device does NOT set_postfx (graceful no-op)', not setTo)
end

-- (3b) supported device: postfx routes to set_postfx with defaults for missing params.
do
    calls = {}
    local prev = backendMock.is_postfx_supported
    backendMock.is_postfx_supported = function() return true end
    local ctx = {}
    local p = schemaMod.coerce('vfx', { postfx = 'softblur' }, ctx)  -- only kind
    VFXCommands.vfx(ctx, p)
    backendMock.is_postfx_supported = prev
    local kind, pf = nil, nil
    for _, c in ipairs(calls) do if c[1] == 'set_postfx' then kind, pf = c[2], c[3] end end
    check('supported: postfx routes to set_postfx with kind', kind == 'softblur')
    check('supported: default strength = 1.0', pf and pf.strength == 1.0)
    check('supported: default radius = 0', pf and pf.radius == 0)
    check('supported: default amount = 0', pf and pf.amount == 0)
    check('supported: default lutMix = 0', pf and pf.lutMix == 0)
end

do
    calls = {}
    local prev = backendMock.is_postfx_supported
    backendMock.is_postfx_supported = function() return true end
    local ctx = {}
    local p = schemaMod.coerce('vfx', { postfx = 'bloom', strength = '0.4', amount = '0.75' }, ctx)
    VFXCommands.vfx(ctx, p)
    backendMock.is_postfx_supported = prev
    local kind, pf = nil, nil
    for _, c in ipairs(calls) do if c[1] == 'set_postfx' then kind, pf = c[2], c[3] end end
    check('supported: explicit strength survives coerce', pf and pf.strength == 0.4)
    check('supported: explicit amount survives coerce', pf and pf.amount == 0.75)
end

-- -----------------------------------------------------------------------------
-- (1) postfx=none closes safely (clear_postfx).
-- -----------------------------------------------------------------------------
do
    calls = {}
    local ok, err = pcall(function()
        local p = schemaMod.coerce('vfx', { postfx = 'none' }, {})
        VFXCommands.vfx({}, p)
    end)
    check('[vfx postfx=none] does not raise', ok)
    if not ok then print('    err: ' .. tostring(err)) end
    local cleared = false
    for _, c in ipairs(calls) do if c[1] == 'clear_postfx' then cleared = true end end
    check('[vfx postfx=none] calls clear_postfx', cleared)
end

-- -----------------------------------------------------------------------------
-- (2) invalid postfx value rejected by schema (pcall false).
-- -----------------------------------------------------------------------------
do
    local ok, err = pcall(schemaMod.coerce, 'vfx', { postfx = 'GARBAGE_NOT_A_KIND' }, {})
    check('schema rejects invalid postfx value (pcall false)', not ok)
end

-- valid enum choices accepted (frame the contract surface)
do
    local ok1 = pcall(schemaMod.coerce, 'vfx', { postfx = 'vignette' }, {})
    local ok2 = pcall(schemaMod.coerce, 'vfx', { postfx = 'lut' }, {})
    local ok3 = pcall(schemaMod.coerce, 'vfx', { postfx = 'bloom' }, {})
    check('schema accepts vignette/lut/bloom/softblur', ok1 and ok2 and ok3)
end

-- restore resolution
package.loaded['backend'] = savedBackend
_G.backend = nil
package.loaded['layers'] = savedLayers

print(string.format('VFX POSTFX TESTS DONE (%d pass, %d fail)', passed, failed))
if failed > 0 then os.exit(1) end
