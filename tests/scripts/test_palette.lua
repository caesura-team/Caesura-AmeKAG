-- test_palette.lua -- palette 3D-LUT wiring lock (t214)
-- Locks the real-name surface of scripts/palette.lua (load_texture +
-- is_valid_handle + set_postfx('lut3d'); the legacy phantom names
-- set_palette/load_image/is_valid are gone), the Lut3D param shape,
-- clear/unload/active semantics, the headless guard, and the [palette]
-- handler delegation through kag.
package.path = "scripts/?.lua;scripts/?/init.lua;scripts/kag/?.lua;scripts/kag/commands/?.lua;" .. package.path
local passed, failed = 0, 0
local function check(name, cond)
    if cond then print("PASS " .. name) passed = passed + 1
    else print("FAIL " .. name) failed = failed + 1 end
end

local calls = { set_postfx = {}, destroy = {} }
local loaded = {}
local texValid = {}
local nextId = 100
local postfx_result, postfx_detail = 1, nil
local failed_texture_path = nil
local stubBackend = {
    get_resolution = function() return 1920, 1080 end,
    load_texture = function(path)
        if path == "fail.png" or path == failed_texture_path then return 0 end
        -- The real texture boundary can reload an image after its old handle
        -- was destroyed; retained path metadata is not a live texture.
        if not loaded[path] or not texValid[tostring(loaded[path])] then
            nextId = nextId + 1
            loaded[path] = nextId
            texValid[tostring(nextId)] = true
        end
        return loaded[path]
    end,
    is_valid_handle = function(hType, h) return texValid[tostring(h)] == true end,
    set_postfx = function(kind, params)
        calls.set_postfx[#calls.set_postfx + 1] = { kind, params }
        return postfx_result, postfx_detail
    end,
    is_postfx_supported = function(kind) return kind == "lut3d" end,
    destroy_texture = function(h)
        calls.destroy[#calls.destroy + 1] = h
        texValid[tostring(h)] = nil
    end,
}
-- palette.lua's capability guard reads the GLOBAL backend; the module
-- require("backend") is separate (engine sets both to the same table).
_G.backend = stubBackend
package.loaded["backend"] = stubBackend
package.loaded["rtt"] = { acquire = function() return 0 end, release = function() end }

local palette = require("palette")

-- 1. load: real names, registry entry, error paths
do
    local ok = palette.load("vintage", "assets/lut/vintage.png")
    check("load ok", ok == true)
    check("load uses load_texture", loaded["assets/lut/vintage.png"] ~= nil)
    check("load without id rejected", palette.load("", "x.png") == nil)
    check("load without path rejected", palette.load("v2") == nil)
    local ok2 = palette.load("broken", "fail.png")
    check("load invalid texture rejected", ok2 == nil)
end

-- 2. apply: Lut3D param shape + intensity clamp
do
    local ok = palette.apply("vintage", 2.0)
    check("apply ok", ok == true)
    local last = calls.set_postfx[#calls.set_postfx]
    check("apply calls set_postfx kind lut3d", last and last[1] == "lut3d")
    local p = last and last[2] or {}
    local idVintage = loaded["assets/lut/vintage.png"]
    check("lut3d params lutId + clamped intensity/strength + lutSize",
          p.lutId == idVintage and p.intensity == 1.0 and p.strength == 1.0 and p.lutSize == 0)
    check("apply unknown id rejected", palette.apply("nope", 1.0) == nil)
    local n0 = #calls.set_postfx
    palette.apply("nope", 1.0)
    check("unknown apply does not call set_postfx", #calls.set_postfx == n0)
end

-- 3. clear: disables the stage (lutId=0)
do
    palette.clear()
    local last = calls.set_postfx[#calls.set_postfx]
    check("clear sends lutId=0", last and last[1] == "lut3d" and last[2].lutId == 0)
end

-- 4. unload: clears first when active, then destroys the texture
do
    palette.apply("vintage", 0.5)
    local ok = palette.unload("vintage")
    check("unload ok", ok == true)
    check("unload destroyed the texture", calls.destroy[#calls.destroy] == loaded["assets/lut/vintage.png"])
    check("unload cleared the active stage",
          calls.set_postfx[#calls.set_postfx] and calls.set_postfx[#calls.set_postfx][2].lutId == 0)
    check("unload unknown rejected", palette.unload("ghost") == nil)
end

-- 5. day/night/toggle via the real-name chain
do
    palette.set_day_mode()
    check("day mode clears", palette.get_mode() == "day"
          and calls.set_postfx[#calls.set_postfx][2].lutId == 0)
    local okNight = palette.set_night_mode()
    check("night mode loads + applies", okNight == true and palette.get_mode() == "night")
    local last = calls.set_postfx[#calls.set_postfx]
    check("night apply lutId is the night texture", last and last[1] == "lut3d" and last[2].lutId == loaded["assets/lut/night.png"])
    check("toggle switches back to day", palette.toggle_mode() == "day")
    check("toggle second time goes night", palette.toggle_mode() == "night")
end

-- 6. guard: missing set_postfx surface -> visible no-op, no crash
do
    local saved = package.loaded["backend"]
    local noPostfx = {}
    for k, v in pairs(saved) do noPostfx[k] = v end
    noPostfx.set_postfx = nil
    noPostfx.is_postfx_supported = nil
    -- palette.lua reads the GLOBAL backend for its capability guard.
    local gb = _G.backend
    _G.backend = noPostfx
    local ok = palette.load("g", "x.png")
    check("guard load returns false (no-op)", ok == false)
    local okA = palette.apply("g", 1.0)
    check("guard apply returns false", okA == false)
    _G.backend = gb
    package.loaded["backend"] = saved
end

-- 7. [palette] handler integration (VFXCommands.palette) + schema
do
    local KAG = require("kag")
    check("palette registered in kag", type(KAG.palette) == "function")
    local ctx = { f = {}, tf = {}, sf = {}, mp = {}, variables = {} }
    local okH = pcall(KAG.palette, ctx, { effect = "apply", id = "sd", path = "assets/lut/sd.png", intensity = 0.3 })
    check("palette handler runs ok", okH == true)
    local last = calls.set_postfx[#calls.set_postfx]
    check("handler applied via lut3d", last and last[1] == "lut3d" and last[2].intensity == 0.3)
    local S = require("kag.schema")
    local c = S.coerce("palette", { effect = "apply", intensity = "0.5" }, {})
    check("palette schema coerces", c.effect == "apply" and c.intensity == 0.5)
end

-- U19: Lua truthiness cannot establish a native handle or applied CSS ACK.
-- Observe active ownership through the real unload/clear path.
palette.unload_all()
local invalid_results = {
    { "nil" }, { "false", false }, { "zero", 0 }, { "negative", -1 },
    { "fractional", 1.5 }, { "nan", 0 / 0 }, { "infinity", math.huge },
    { "negative infinity", -math.huge }, { "uint32 overflow", 4294967296 },
    { "string", "1" }, { "table", {} },
}
for index, entry in ipairs(invalid_results) do
    local old_id, new_id = "u19-old-" .. index, "u19-new-" .. index
    postfx_result, postfx_detail = 1, nil
    assert(palette.load(old_id, "assets/lut/" .. old_id .. ".png"))
    assert(palette.load(new_id, "assets/lut/" .. new_id .. ".png"))
    assert(palette.apply(old_id))
    postfx_result, postfx_detail = entry[2], { status = "unsupported", reason = "test_backend_result" }
    local before = #calls.set_postfx
    local ok, detail = palette.apply(new_id)
    check("apply rejects " .. entry[1] .. " and preserves backend detail", ok == false and detail == postfx_detail)
    check("invalid apply is submitted once: " .. entry[1], #calls.set_postfx == before + 1)
    postfx_result, postfx_detail = 1, nil
    before = #calls.set_postfx
    assert(palette.unload(new_id))
    check("failed palette never becomes active: " .. entry[1], #calls.set_postfx == before)
    before = #calls.set_postfx
    assert(palette.unload(old_id))
    check("previous active palette remains owned: " .. entry[1],
        #calls.set_postfx == before + 1 and calls.set_postfx[#calls.set_postfx][2].lutId == 0)
end
for index, valid in ipairs({ 1, 4294967295, true }) do
    local id = "u19-valid-" .. index
    assert(palette.load(id, "assets/lut/" .. id .. ".png"))
    postfx_result, postfx_detail = valid, { status = "applied" }
    local ok, detail = palette.apply(id)
    check("positive handle or boolean ACK accepted: " .. tostring(valid), ok == true and detail == postfx_detail)
    assert(palette.unload(id))
end
postfx_result, postfx_detail = 1, nil

-- U19 public mode/cleanup helpers must reflect actual backend acceptance.
local function reset_day(active)
    postfx_result, postfx_detail, failed_texture_path = 1, nil, nil
    palette.unload_all()
    assert(palette.set_day_mode())
    if active then
        assert(palette.load(active, "assets/lut/" .. active .. ".png"))
        assert(palette.apply(active))
    end
end
local function check_active_retained(id, label)
    postfx_result, postfx_detail = 1, nil
    local before_clear, before_destroy = #calls.set_postfx, #calls.destroy
    local ok = palette.unload(id)
    check(label, ok == true and #calls.set_postfx == before_clear + 1
        and #calls.destroy == before_destroy + 1 and calls.set_postfx[#calls.set_postfx][2].lutId == 0)
end

for index, invalid in ipairs(invalid_results) do
    local id = "u19-clear-" .. index
    reset_day(id)
    postfx_result, postfx_detail = invalid[2], { status = "unsupported", reason = "clear_unavailable" }
    local ok, detail = palette.clear()
    check("clear rejects " .. invalid[1] .. " and returns its detail", ok == false and detail == postfx_detail)
    check_active_retained(id, "failed clear retains the active reference: " .. invalid[1])
end
for _, valid in ipairs({ 1, 4294967295, true }) do
    reset_day()
    postfx_result, postfx_detail = valid, { status = "applied" }
    local ok, detail = palette.clear()
    check("clear accepts native stage handle or Web ACK: " .. tostring(valid), ok == true and detail == postfx_detail)
end

reset_day()
assert(palette.set_night_mode())
postfx_result, postfx_detail = 0, { status = "unsupported", reason = "day_clear_failed" }
local ok, detail = palette.set_day_mode()
check("day mode requires successful clear and returns failure detail", ok == false and detail == postfx_detail)
check("failed day mode preserves actual night mode", palette.get_mode() == "night")
check_active_retained(palette._nightLutId, "failed day mode retains its referenced night texture")

reset_day("u19-night-apply-old")
postfx_result, postfx_detail = false, { status = "unsupported", reason = "night_apply_failed" }
ok, detail = palette.set_night_mode()
check("night mode returns the failed apply result", ok == false and detail == postfx_detail)
check("failed night apply preserves day mode", palette.get_mode() == "day")
check_active_retained("u19-night-apply-old", "failed night apply preserves the previous active palette")

reset_day("u19-night-load-old")
failed_texture_path = "assets/lut/night.png"
local before = #calls.set_postfx
ok, detail = palette.set_night_mode()
check("night load failure is reported instead of claiming neutral night", ok == false
    and type(detail) == "string" and detail:find("failed to load LUT image", 1, true) ~= nil)
check("night load failure preserves mode and does not clear prior effect",
    palette.get_mode() == "day" and #calls.set_postfx == before)
failed_texture_path = nil
check_active_retained("u19-night-load-old", "night load failure retains the previous active texture")

for _, old_mode in ipairs({ "day", "night" }) do
    local active = "u19-toggle-old"
    reset_day(old_mode == "day" and active or nil)
    if old_mode == "night" then assert(palette.set_night_mode()); active = palette._nightLutId end
    postfx_result, postfx_detail = 0, { status = "unsupported", reason = "toggle_failed" }
    local mode, result = palette.toggle_mode()
    check("failed toggle returns the actual old mode: " .. old_mode,
        mode == old_mode and palette.get_mode() == old_mode)
    check("failed toggle preserves detail: " .. old_mode, result == postfx_detail)
    check_active_retained(active, "failed toggle retains the active reference: " .. old_mode)
end

reset_day()
postfx_result, postfx_detail = true, { status = "approximate" }
ok, detail = palette.set_night_mode()
check("successful night mode reports application detail", ok == true and detail == postfx_detail and palette.get_mode() == "night")
postfx_result, postfx_detail = 7, { status = "applied" }
ok, detail = palette.set_day_mode()
check("successful day mode reports clear detail", ok == true and detail == postfx_detail and palette.get_mode() == "day")
postfx_result, postfx_detail = true, { status = "approximate" }
local mode, result = palette.toggle_mode()
check("successful toggle retains mode-string return and detail", mode == "night" and result == postfx_detail)
postfx_result, postfx_detail = 7, { status = "applied" }
mode, result = palette.toggle_mode()
check("successful reverse toggle retains mode-string return and detail", mode == "day" and result == postfx_detail)

reset_day("u19-unload-active")
postfx_result, postfx_detail = 0, { status = "failed", reason = "clear_refused" }
local before_destroy = #calls.destroy
ok, detail = palette.unload("u19-unload-active")
check("active unload returns clear failure", ok == false and detail == postfx_detail)
check("active unload cannot destroy a referenced texture after failed clear",
    #calls.destroy == before_destroy and texValid[tostring(loaded["assets/lut/u19-unload-active.png"])])
check_active_retained("u19-unload-active", "active unload can retry the retained registry entry")

reset_day("u19-unload-all-active")
assert(palette.load("u19-unload-all-inactive", "assets/lut/u19-unload-all-inactive.png"))
postfx_result, postfx_detail = false, { status = "failed", reason = "clear_refused" }
before_destroy = #calls.destroy
ok, detail = palette.unload_all()
check("unload_all returns active clear failure", ok == false and detail == postfx_detail)
check("unload_all preserves textures when its active clear fails", #calls.destroy == before_destroy
    and texValid[tostring(loaded["assets/lut/u19-unload-all-active.png"])])
postfx_result, postfx_detail = true, { status = "applied" }
before = #calls.set_postfx
ok, detail = palette.unload_all()
check("unload_all retry clears its retained active reference and frees both textures",
    ok == true and detail == postfx_detail and #calls.set_postfx == before + 1 and #calls.destroy == before_destroy + 2)
postfx_result, postfx_detail = 1, nil

if failed > 0 then os.exit(1) end
print("PALETTE TESTS DONE (" .. passed .. " passed)")
