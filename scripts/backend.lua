-- ===========================================================================
--  Caesura (AmeKAG) -- backend.lua
--  Spec [0.4]: Unified C++ backend proxy.
--  Resolution order: 1. _CAESURA_BACKEND  2. direct KAG/Render/DevCore
-- ===========================================================================

local Backend = {}

local function get_backend()
    return rawget(_G, "_CAESURA_BACKEND")
end


local function render_or_guard(method, ...)
    local be = get_backend()
    if be then return be.render(method, ...) end
    if Render and Render[method] then return Render[method](...) end
    return false
end

local function devcore_or_guard(method, ...)
    local be = get_backend()
    if be then return be.platform(method, ...) end
    if DevCore and DevCore[method] then return DevCore[method](...) end
    return false
end


-- Resolution chain for backend convenience calls (audit: backend.lua
-- called a bare global KAG.* 18 times -- the ENGINE sets _G.KAG (a C++
-- binding table) so it worked there, but direct-API contexts had nil).
-- Order: _CAESURA_BACKEND (UnifiedBinding) -> global KAG -> kag module.
local function resolve(fn_name)
    local be = get_backend()
    local v = be and be[fn_name]
    if type(v) == "function" then return v end
    local g = rawget(_G, "KAG")
    v = g and g[fn_name]
    if type(v) == "function" then return v end
    local k = package.loaded["kag"]
    v = k and k[fn_name]
    if type(v) == "function" then return v end
    return nil
end

-- Guarded call (review should-fix): a missing binding must return nil
-- (direct-API contexts) instead of throwing "attempt to call a nil".
local function call_resolved(name, ...)
    local fn = resolve(name)
    if not fn then
        print("[backend] '" .. name .. "' unavailable (no KAG binding)")
        return nil
    end
    return fn(...)
end

-- =========================================================================
-- Audio
-- =========================================================================

-- Screen-offset pan (camera/quakes): shifts the main view rect.
function Backend.set_screen_offset(dx, dy)
    return render_or_guard("set_screen_offset", dx, dy)
end

function Backend.audio_play(bus, file, opts)
    opts = opts or {}
    local be = get_backend()
    if be then
        if bus == "bgm" then return be.audio("play_bgm", file, tonumber(opts.fadein) or 1.0)
        elseif bus == "voice" then return be.audio("play_voice", file)
        elseif bus == "se" then
            if opts.x and opts.y then return be.audio("play_se_3d", file, opts.x, opts.y, opts.z or 0)
            else return be.audio("play_se", file) end
        end
    else
        if bus == "bgm" then return call_resolved("play_bgm", file, tonumber(opts.fadein) or 1.0)
        elseif bus == "voice" then return call_resolved("play_voice", file)
        elseif bus == "se" then
            if opts.x and opts.y then return call_resolved("play_se_3d", file, opts.x, opts.y, opts.z or 0)
            else return call_resolved("play_se", file) end
        end
    end
    return false
end

function Backend.audio_stop(bus, opts)
    opts = opts or {}
    local be = get_backend()
    if be then
        if bus == "bgm" then return be.audio("stop_bgm", tonumber(opts.fadeout) or 1.0)
        elseif bus == "voice" then return be.audio("stop_voice")
        elseif bus == "se" then return be.audio("stop_se") end
    else
        if bus == "bgm" then return call_resolved("stop_bgm", tonumber(opts.fadeout) or 1.0)
        elseif bus == "voice" then return call_resolved("stop_voice")
        elseif bus == "se" then call_resolved("stop_se") end
    end
    return false
end

function Backend.audio_is_playing(bus)
    local be = get_backend()
    if be then
        if bus == "voice" then return be.audio("is_voice_playing") end
        if bus == "bgm"   then return be.audio("is_bgm_playing") end
        if bus == "se"    then return be.audio("is_playing", "se") end
    else
        if bus == "voice" then return call_resolved("is_voice_playing") end
        if bus == "bgm"   then return call_resolved("is_bgm_playing") end
        if bus == "se"    then return call_resolved("is_se_playing") end
    end
    return false
end

function Backend.audio_set_listener(px, py, pz, ax, ay, az)
    local be = get_backend()
    if be then return be.audio("set_listener", px or 0, py or 0, pz or 0, ax or 0, ay or 1, az or 0)
    else return call_resolved("set_listener", px, py, pz, ax, ay, az) end
end

function Backend.audio_set_bus_volume(bus, vol)
    local be = get_backend()
    if be then return be.audio("set_bus_volume", bus, vol)
    else return call_resolved("set_bus_volume", bus, vol) end
end

function Backend.audio_get_bus_volume(bus)
    local be = get_backend()
    if be then return be.audio("get_bus_volume", bus)
    else return call_resolved("get_bus_volume", bus) end
end

function Backend.audio_fade_volume(bus, target_vol, fade_time)
    local be = get_backend()
    if be then return be.audio("fade_volume", bus, target_vol, fade_time)
    else return false end
end

function Backend.audio_get_length(bus)
    local be = get_backend()
    if be then return be.audio("get_length", bus)
    else return 0 end
end

function Backend.audio_get_position(bus)
    local be = get_backend()
    if be then return be.audio("get_position", bus)
    else return 0 end
end

function Backend.stop_se()
    local be = get_backend()
    if be then return be.audio("stop_se")
    else return call_resolved("stop_se") end
end

function Backend.flush_wave_cache()
    local be = get_backend()
    if be then return be.audio("flush_wave_cache")
    else return call_resolved("flush_wave_cache") end
end

-- =========================================================================
-- Render
-- =========================================================================

function Backend.create_viewport(w, h)
    return render_or_guard("create_viewport", w, h)
end

function Backend.destroy_viewport(vpId)
    return render_or_guard("destroy_viewport", vpId)
end

function Backend.draw_viewport(vpId, x, y, w, h)
    return render_or_guard("draw_viewport", vpId, x, y, w, h)
end

function Backend.fill_viewport(handleId, r, g, b, a)
    return render_or_guard("fill_viewport", handleId, r, g, b, a)
end

function Backend.load_texture(file)
    return render_or_guard("load_texture", file)
end

function Backend.is_valid_handle(handleType, handle)
    -- HandleType::TEXTURE = 0 (engine ResourceHandle validator: type, id)
    return render_or_guard("is_valid_handle", handleType or 0, handle)
end

function Backend.destroy_texture(id)
    return render_or_guard("destroy_texture", id)
end

function Backend.create_solid_texture(r, g, b, a)
    return render_or_guard("create_solid_texture", r, g, b, a)
end

function Backend.submit_batch(batch)
    local be = get_backend()
    if be then return be.render("submit_batch", batch)
    else return false end
end

function Backend.submit_blend(viewId, texA, texB, mode, progress, alpha)
    local be = get_backend()
    if be then return be.render("submit_blend", viewId, texA, texB, mode, progress, alpha)
    else return false end
end

function Backend.submit_transition(viewId, fromId, toId, ruleId, method, progress)
    local be = get_backend()
    if be then return be.render("submit_transition", viewId, fromId, toId, ruleId, method, progress)
    else return false end
end

function Backend.submit_vfx(texId, effect, fadeAlpha, fadeR, fadeG, fadeB, blurRadius, quakeX, quakeY)
    local be = get_backend()
    if be then return be.render("submit_vfx", texId, effect, fadeAlpha, fadeR, fadeG, fadeB, blurRadius, quakeX, quakeY)
    else return false end
end

-- Accessibility color filter (Neo-Genesis): preset name -> C++ preset
-- matrix (deuteranopia/protanopia/tritanopia/grayscale/high_contrast/
-- none), applied by subsequent effect-4 VFX passes.
function Backend.set_color_filter(preset)
    local be = get_backend()
    if be then return be.render("set_color_filter", preset)
    else return false end
end


-- Post-expression chain (round 102): full-screen PostFx effects.
-- kind: "bloom" | "vignette" | "lut" | "softblur"; params table carries
--   strength / radius / amount / rgb / lutMix (defaults filled in C++).
-- Returns the effect handle (0 when unsupported / headless no-op).
function Backend.set_postfx(kind, params)
    local be = get_backend()
    params = params or {}
    if be then return be.render("set_postfx", kind, params)
    else return 0 end
end

function Backend.destroy_postfx(kind)
    local be = get_backend()
    if be then return be.render("destroy_postfx", kind)
    else return false end
end

function Backend.clear_postfx()
    local be = get_backend()
    if be then return be.render("clear_postfx")
    else return false end
end

function Backend.is_postfx_supported(kind)
    local be = get_backend()
    if be then return be.render("is_postfx_supported", kind)
    else return false end
end

function Backend.is_postfx_active()
    local be = get_backend()
    if be then return be.render("is_postfx_active")
    else return false end
end
function Backend.submit_stretch_blt(dst_rt, dst_rect, src_rt, src_rect, filter_id)
    local be = get_backend()
    if be then return be.render("submit_stretch_blt", dst_rt, dst_rect, src_rt, src_rect, filter_id)
    else return false end
end

function Backend.submit_affine_blt(dst_rt, dst_rect, src_rt, src_rect, matrix, filter_id)
    local be = get_backend()
    if be then return be.render("submit_affine_blt", dst_rt, dst_rect, src_rt, src_rect, matrix, filter_id)
    else return false end
end

-- =========================================================================
-- Text / UI
-- =========================================================================

function Backend.render_text(text, x, y, r, g, b, a, scale, bold, italic, strike)
    local be = get_backend()
    if be then
        return be.render("render_text", text, x, y, r, g, b, a, scale, bold, italic, strike)
    else
        return call_resolved("render_text", text, x, y, r, g, b, a, scale, bold, italic, strike)
    end
end

-- Text-to-speech (accessibility): engine-level TTS is not wired yet --
-- these entry points let game scripts probe and request speech synthesis
-- without crashing when the backend is absent. A future TTS backend
-- (Windows SAPI / external RPC) implements tts_available() and
-- tts_speak(); until then both report the feature as unavailable.
function Backend.tts_available()
    local be = get_backend()
    if be and be.tts_available then
        local ok, r = pcall(be.tts_available)
        return ok and r == true
    end
    return false
end

function Backend.tts_speak(text, rate)
    if not Backend.tts_available() then
        return false
    end
    local be = get_backend()
    if be and be.tts_speak then
        local ok, r = pcall(be.tts_speak, text, rate or 1.0)
        return ok and r ~= false
    end
    return false
end

-- AI dialogue (Neo-Genesis): LLM query through the `AI` C++ binding.
-- Degrades to false/nil when the binding or service is unavailable --
-- the [ai_dialog] command falls back to its fallback= text.
function Backend.ai_available()
    local ai = rawget(_G, "AI")
    if not ai or type(ai.available) ~= "function" then return false end
    local ok, r = pcall(ai.available)
    return ok and r == true
end

function Backend.ai_query(prompt, opts)
    local ai = rawget(_G, "AI")
    if not ai or type(ai.query) ~= "function" then return nil, "no-binding" end
    return ai.query(prompt, opts or {})
end

function Backend.ai_query_async(prompt, opts, callback)
    local ai = rawget(_G, "AI")
    if not ai or type(ai.query_async) ~= "function" then return false end
    if type(callback) ~= "function" then return false end
    if type(opts) == "function" then opts = { } end
    local ok, r = pcall(ai.query_async, prompt, opts or {}, callback)
    return ok and r == true
end

function Backend.ai_cancel()
    local ai = rawget(_G, "AI")
    if not ai or type(ai.cancel) ~= "function" then return false end
    local ok, r = pcall(ai.cancel)
    return ok and r ~= false
end

function Backend.show_text(text)
    return call_resolved("show_text", text)
end

function Backend.show_image(file, x, y)
    return call_resolved("show_image", file, x or 0, y or 0)
end

function Backend.clear_screen()
    return call_resolved("clear_screen")
end

function Backend.wait_click()
    -- Explicit require: the global KAG table is never set (review
    -- should-fix) -- the demo's direct-API path called into nil here.
    -- package.loaded direct: sandbox-safe (audit, same as audio_xfade)
    local kag_mod = package.loaded["kag"] or require("kag")
    if kag_mod and kag_mod.wait_click then
        return kag_mod.wait_click()
    end
    return false
end

-- =========================================================================
-- System / Platform
-- =========================================================================

function Backend.set_resolution(w, h)
    return devcore_or_guard("set_resolution", w, h)
end

function Backend.get_resolution()
    return devcore_or_guard("get_resolution")
end

function Backend.set_input_focus(mode)
    return devcore_or_guard("set_input_focus", mode)
end

function Backend.get_input_focus()
    return devcore_or_guard("get_input_focus")
end

-- Video trio: the Render binding owns these; the commands' `and`
-- guards masked their absence (video silently no-op'd) -- audit.
function Backend.video_play(file, opts)
    return render_or_guard("video_play", file, opts)
end

function Backend.video_stop(handle)
    return render_or_guard("video_stop", handle)
end

function Backend.video_is_playing(handle)
    return render_or_guard("video_is_playing", handle)
end

function Backend.set_fullscreen(enabled)
    return devcore_or_guard("set_fullscreen", enabled)
end


-- =========================================================================
-- Audio crossfade helper
-- =========================================================================

function Backend.audio_xfade(bus, new_file, fade_time)
    local be = get_backend()
    if be then return be.audio("xfade", bus, new_file, fade_time)
    else
        -- package.loaded direct: require() is sandbox-wrapped in the
        -- suite and 'audio' may not be preloaded -- audit
        local Audio = package.loaded["audio"]
        if Audio and Audio.crossfade_bgm then
            return Audio.crossfade_bgm(new_file, fade_time / 1000)
        end
        return false
    end
end

-- =========================================================================
-- Texture management (name-based)
-- =========================================================================



-- =========================================================================
-- Text rendering helpers
-- =========================================================================

function Backend.clear_text()
    local be = get_backend()
    if be then return be.render("clear_text")
    else return call_resolved("clear_text") end
end

function Backend.render_ruby(text, ruby, x, y, r, g, b, a)
    local be = get_backend()
    if be then
        return be.render("render_ruby", text, ruby, x, y, r, g, b, a)
    else
        return call_resolved("render_ruby", text, ruby, x, y, r, g, b, a)
    end
end

function Backend.set_font(id)
    local be = get_backend()
    if be then return be.render("set_font", id)
    else return call_resolved("set_font", id) end
end

function Backend.line_height()
    local be = get_backend()
    if be then return be.render("line_height")
    else return 24 end
end

-- =========================================================================
-- Logging
-- =========================================================================

function Backend.log(msg)
    local be = get_backend()
    if be then return be.platform("log", msg)
    else print("[Caesura] " .. tostring(msg)) end
end


-- =========================================================================
-- Particle effects (stubs for pure Lua fallback)
-- =========================================================================

-- clear_particles maps to the VFX binding (the render factory has no
-- "clear_particles" route; the dead begin/draw/end wrappers were removed).
function Backend.clear_particles()
    if VFX and VFX.particles_clear then
        return VFX.particles_clear()
    end
    return false
end

-- The emitter trio was missing -- [vfx type="particle" action="create"]
-- (and the new [particles] command) called backend.particles_create_emitter
-- and CRASHED with 'attempt to call a nil value' (audit: pre-existing).
function Backend.particles_create_emitter(cfg)
    if VFX and VFX.particles_create_emitter then
        return VFX.particles_create_emitter(cfg)
    end
    return false
end

function Backend.particles_emit(emitter, count)
    if VFX and VFX.particles_emit then
        return VFX.particles_emit(emitter, count)
    end
    return false
end

function Backend.particles_destroy_emitter(emitter)
    if VFX and VFX.particles_destroy_emitter then
        return VFX.particles_destroy_emitter(emitter)
    end
    return false
end


-- ══════════════════════════════════════════════════════
-- 异步加载 API (G11-U3)
-- ══════════════════════════════════════════════════════

--- 异步加载纹理 (立即返回 request_id; 可选 callback(success, path, texId))
function Backend.load_texture_async(path, callback)
    local be = get_backend()
    if be then return be.render("load_texture_async", path, callback) end
end

--- 取消所有异步加载
function Backend.cancel_async_loads()
    local be = get_backend()
    if be then return be.render("cancel_async_loads") end
end

-- =============================================================================
-- Text rendering state
-- =============================================================================

function Backend.text_set_font(face, size, color)
    return render_or_guard("text_set_font", face, size, color)
end

function Backend.text_reset_state()
    return render_or_guard("text_reset_state")
end

-- =========================================================================
-- Platform / IME / Text Input
-- =========================================================================

function Backend.start_text_input()
    return devcore_or_guard("start_text_input")
end

function Backend.stop_text_input()
    return devcore_or_guard("stop_text_input")
end

function Backend.set_text_input_rect(x, y, w, h, cursor)
    return devcore_or_guard("set_text_input_rect", x, y, w, h, cursor)
end

function Backend.is_text_input_active()
    return devcore_or_guard("is_text_input_active")
end

return require("capability_backend").install(Backend)
