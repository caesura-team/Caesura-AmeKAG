-- Caesura (AmeKAG) - Palette / LUT Management (Lua layer)
-- Provides load/apply/clear for 3D color lookup tables (LUTs).
-- All GPU operations go through the backend table (no direct bgfx calls).
-- t214: real-name surface -- load_texture/is_valid_handle + set_postfx
-- ("lut3d") replace the legacy phantom names (set_palette/load_image/
-- is_valid). The 2D-packed LUT (256x16 = 16^3, 4096x64 = 64^3) is sampled
-- by the Lut3D postfx stage; the texture stays TextureManager-owned.
-- Backend resolution: the GLOBAL backend (set by the web boot bridge)
-- wins; the native engine falls back to the backend module (which routes
-- through the _CAESURA_BACKEND unified proxy).

local palette = {}

-- Internal registry: lut_id -> { handle, path, size }
local luts = {}

-- Active LUT id (so unload can clear the stage pointing at a destroyed
-- texture before releasing it; the chain also isValid-guards each frame).
local active_id = nil

-- Native PostFxHandle is uint32 with zero invalid; a Web effect may instead
-- return true as an applied ACK. Lua truthiness also accepts invalid zero.
local function valid_postfx_result(value)
    return value == true or (type(value) == "number" and value > 0
        and value <= 4294967295 and value % 1 == 0)
end

-- Resolve the backend surface (global first, module fallback).
local function get_backend()
    local g = rawget(_G, "backend")
    if type(g) == "table" then return g end
    local ok, b = pcall(require, "backend")
    if ok and type(b) == "table" then return b end
    return nil
end

-- t214: capability guard -- set_postfx is the LUT postfx surface on both
-- the native module (shim -> Proxy.render -> RenderBinding) and the web
-- global (jsBackend). The binding's isPostFxSupported decides the rest.
local function lut_available()
    local b = get_backend()
    return type(b) == "table" and type(b.set_postfx) == "function"
end
local function lut_noop(what)
    print(string.format(
        "[palette] %s: LUT postfx not wired (set_postfx missing) -- no-op",
        what))
end

--- Load a LUT texture from file and register with a string ID.
-- @param lut_id  string identifier for this LUT (e.g. "vintage", "noir")
-- @param path    file path to the 256x16 or 4096x64 LUT image (.png)
-- @return true on success, nil+error on failure
function palette.load(lut_id, path)
    if not lut_available() then lut_noop("load") return false end
    if not lut_id or #lut_id == 0 then
        return nil, "palette.load: lut_id required"
    end
    if not path or #path == 0 then
        return nil, "palette.load: path required"
    end

    -- Load texture through backend (TextureManager); real names (t214).
    local b = get_backend()
    local handle = b.load_texture(path)
    if not handle or not b.is_valid_handle or not b.is_valid_handle(0, handle) then -- HandleType::TEXTURE = 0
        return nil, "palette.load: failed to load LUT image: " .. path
    end

    luts[lut_id] = {
        handle = handle,
        path   = path,
        size   = 0,  -- 0 = derive at apply (N = texture height; width must be N*N)
    }

    return true
end

--- Apply a loaded LUT to the current rendering output.
-- Intensity controls the blend: 0 = no effect, 1 = full LUT.
-- @param lut_id     string ID from palette.load()
-- @param intensity  float 0.0-1.0 (default 1.0)
-- @return true on success, nil+error on failure
function palette.apply(lut_id, intensity)
    if not lut_available() then lut_noop("apply") return false end
    intensity = intensity or 1.0
    intensity = math.max(0.0, math.min(1.0, intensity))

    local entry = luts[lut_id]
    if not entry then
        return nil, "palette.apply: LUT not loaded: " .. tostring(lut_id)
    end

    -- t214: real surface -- the Lut3D postfx stage binds the borrowed
    -- texture at sampler t1 and blends by intensity. 0/false return =
    -- not supported on this device (visible degradation).
    local b = get_backend()
    -- strength = native PostFxParams field; intensity = web jsBackend field.
    local applied, result = b.set_postfx("lut3d", {
        lutId = entry.handle,
        strength = intensity, intensity = intensity,
        lutSize = entry.size,
    })
    if not valid_postfx_result(applied) then
        if type(result)=="table" and type(result.status)=="string" then
            print(require("capability_runtime").message(result))
        else
            print("[palette] apply: backend did not apply the effect")
        end
        return false, result
    end
    active_id = lut_id

    return true, result
end

--- Clear/disable the active palette.
-- @return true on success, false plus the backend result on failure
function palette.clear()
    if not lut_available() then lut_noop("clear") return false end
    local b = get_backend()
    local cleared, result = b.set_postfx("lut3d", { lutId = 0, intensity = 0, lutSize = 0 })
    if not valid_postfx_result(cleared) then return false, result end
    active_id = nil
    return true, result
end

--- Unload a specific LUT from memory.
-- @param lut_id  string ID
-- @return true on success, nil+error if not found
function palette.unload(lut_id)
    if not lut_available() then lut_noop("unload") return end
    local entry = luts[lut_id]
    if not entry then
        return nil, "palette.unload: LUT not found: " .. tostring(lut_id)
    end

    -- If the unloaded LUT is the active one, clear the stage first so a
    -- destroyed texture is never referenced (chain also guards isValid).
    local result
    if active_id == lut_id then
        local cleared
        cleared, result = palette.clear()
        if not cleared then return false, result end
    end
    local b = get_backend()
    if entry.handle and b.is_valid_handle and b.is_valid_handle(0, entry.handle) then
        b.destroy_texture(entry.handle)
    end
    luts[lut_id] = nil
    return true, result
end

--- Unload all LUTs.
function palette.unload_all()
    local result
    if active_id then
        local cleared
        cleared, result = palette.clear()
        if not cleared then return false, result end
    end
    local b = get_backend()
    for id, entry in pairs(luts) do
        if entry.handle and b.is_valid_handle and b.is_valid_handle(0, entry.handle) then
            b.destroy_texture(entry.handle)
        end
    end
    luts = {}
    return true, result
end

--- Day/night mode state
palette._mode = "day"  -- "day" or "night"
palette._nightLutId = "__night_preset"

--- Activate day mode (neutral/no color grading).
-- Clears any active LUT. Safe to call repeatedly.
function palette.set_day_mode()
    local cleared, result = palette.clear()
    if not cleared then return false, result end
    palette._mode = "day"
    return true, result
end

--- Activate night mode (blue-dark color grading).
-- Tries to load assets/lut/night.png LUT. Failure preserves the current mode.
-- The LUT is cached after first load so subsequent calls are cheap.
function palette.set_night_mode()
    -- Try loading the night LUT if not already loaded
    if not luts[palette._nightLutId] then
        local nightPath = "assets/lut/night.png"
        local loaded, result = palette.load(palette._nightLutId, nightPath)
        if not loaded then return false, result end
    end

    local applied, result = palette.apply(palette._nightLutId, 1.0)
    if not applied then return false, result end
    palette._mode = "night"
    return true, result
end

--- Toggle between day and night mode.
-- @return actual "day" or "night" mode, plus the attempted operation result
function palette.toggle_mode()
    local _, result
    if palette._mode == "day" then
        _, result = palette.set_night_mode()
    else
        _, result = palette.set_day_mode()
    end
    return palette._mode, result
end

--- Get the current mode.
-- @return "day" or "night"
function palette.get_mode()
    return palette._mode
end

return palette
