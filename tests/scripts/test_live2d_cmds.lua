-- =============================================================================
--  Caesura (AmeKAG) — test_live2d_cmds.lua
--  Schema/declaration tests; model admission is covered by the pipeline suite.
-- =============================================================================

local BS = string.char(92)
local here = (arg and arg[0] and arg[0]:match("(.*[/" .. BS .. "])")) or "scripts/"
if not package.path:find(here, 1, true) then
    package.path = here .. "?.lua;" .. here .. "?/init.lua;" .. "scripts/?.lua;scripts/?/init.lua;" .. package.path
end

local schema = require("kag.schema")
local charCmds = require("kag.commands.character")

local passed = 0
local failed = 0

local function check(cond, msg)
    if cond then
        passed = passed + 1
        print("  [PASS] " .. msg)
    else
        failed = failed + 1
        print("  [FAIL] " .. msg)
    end
end

print("=== Running Live2D Command Tests ===")

-- 1. Schema Registration
check(schema.specs("live2d_motion") ~= nil, "live2d_motion registered in schema")
check(schema.specs("live2d_expression") ~= nil, "live2d_expression registered in schema")
check(schema.specs("live2d_lip_sync") ~= nil, "live2d_lip_sync registered in schema")

-- 2. Clamping and validation
local ctx = {}
local val_motion = schema.coerce("live2d_motion", { model = "aoi", motion = "wave", fadein = 15000, fadeout = -100 }, ctx, 1)
check(val_motion.fadein == 10000, "fadein clamped to max 10000")
check(val_motion.fadeout == 0, "fadeout clamped to min 0")

check(not pcall(charCmds.live2d_motion, ctx, val_motion), "Motion requires a loaded model")
check(ctx.live2d == nil, "Rejected motion does not fabricate model state")

-- 3. Expression
local val_expr = schema.coerce("live2d_expression", { model = "aoi", expression = "smile", weight = 1.5 }, ctx, 2)
check(val_expr.weight == 1.0, "Expression weight clamped to 1.0")
check(not pcall(charCmds.live2d_expression, ctx, val_expr), "Expression requires a loaded model")
check(ctx.live2d == nil, "Rejected expression does not fabricate model state")

-- 4. Lip Sync schema/declaration compatibility. The proxy below is a fake:
-- this retained ctx assertion proves metadata only, never a native model.
local val_lip = schema.coerce("live2d_lip_sync", { model = "aoi", value = 0.85 }, ctx, 3)
check(val_lip.value == 0.85, "Lip sync value valid")
local backend = require("backend")
local previous_mouth = backend.live2d_set_mouth
backend.live2d_set_mouth = function() return true end
ctx._live2dHandles = {aoi = 1}
charCmds.live2d_lip_sync(ctx, val_lip)
backend.live2d_set_mouth = previous_mouth
check(ctx.live2d["aoi"].lip_sync == 0.85, "Lip sync stored in ctx (fake proxy; metadata only)")
check(val_lip.source == "manual", "Legacy lip sync defaults to manual source (schema only)")
check(schema.coerce("live2d_lip_sync", {model="aoi",source="voice"},ctx,3).value == nil,
    "Voice omission remains distinct from explicit value zero")
check(not pcall(schema.coerce,"live2d_lip_sync",{model="aoi",source="microphone"},ctx,3),
    "Invalid lip sync source is rejected")

print(string.format("\nLive2D Command Tests: %d passed, %d failed.", passed, failed))
if failed > 0 then os.exit(1) end
