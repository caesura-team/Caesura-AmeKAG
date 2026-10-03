-- ===========================================================================
--  Caesura (AmeKAG) -- backend_factory.lua
--  Spec [0.4]: Factory pattern for backend creation.
--  Called once during engine init. Creates render/audio/platform backends,
--  stores handle to Lua registry, returns unified backend proxy.
-- ===========================================================================

local BackendFactory = {}

-- Lua Blend.GPUMode is the historical ten-mode enum. The native unified
-- fs_blend uses ShaderCache::BlendMode (28 modes); these IDs are not aliases.
local native_blend_mode = {[0]=0, [1]=16, [2]=17, [3]=1, [4]=2,
    [5]=3, [6]=4, [7]=5, [8]=10, [9]=9}

function BackendFactory.create(opts)
    opts = opts or {}

    local render_name   = opts.render   or "bgfx"
    local audio_name    = opts.audio    or "soloud"
    local platform_name = opts.platform or "sdl3"
    local backend_info  = {}

    if Engine and Engine.select_platform_backend then
        Engine.select_platform_backend(platform_name)
    end
    if Engine and Engine.get_backend_info then
        local ok, info = pcall(Engine.get_backend_info)
        if ok and type(info) == "table" then
            backend_info = info
        end
    end

    local actual_render_name   = backend_info.render   or render_name
    local actual_audio_name    = backend_info.audio    or audio_name
    local actual_platform_name = backend_info.platform or platform_name

    if not KAG then error("[BackendFactory] KAG C binding not available.") end
    if not Render then error("[BackendFactory] Render C binding not available.") end
    if not DevCore then error("[BackendFactory] DevCore C binding not available.") end

    local backend = {}

    -- Render subsystem --------------------------------------------------
    backend.render = function(cmd, ...)
        if cmd == "ping" then return true
        elseif cmd == "name" then return actual_render_name
        elseif cmd == "submit_batch" then return Render.submit_batch(...)
        elseif cmd == "submit_blend" then
            local _view, base, blend, mode, progress, alpha = ...
            local native_mode = assert(native_blend_mode[mode], "unknown Lua GPU blend mode")
            return Render.submit_blend(base, blend, native_mode, 1.0, progress, alpha)
        elseif cmd == "submit_transition" then
            local _view, from, to, rule, method, progress = ...
            return Render.submit_transition(from, to, rule, method, progress)
        elseif cmd == "capture_scene" then return Render.capture_scene()
        elseif cmd == "cancel_transition" then return Render.cancel_transition()
        elseif cmd == "submit_vfx" then return Render.submit_vfx(...)
        elseif cmd == "set_color_filter" then return Render.set_color_filter(...)
        elseif cmd == "set_postfx" then return Render.set_postfx(...)
        elseif cmd == "destroy_postfx" then return Render.destroy_postfx(...)
        elseif cmd == "clear_postfx" then return Render.clear_postfx()
        elseif cmd == "is_postfx_supported" then return Render.is_postfx_supported(...)
        elseif cmd == "is_postfx_active" then return Render.is_postfx_active()
        elseif cmd == "create_viewport" then return Render.create_viewport(...)
        elseif cmd == "destroy_viewport" then return Render.destroy_viewport(...)
        elseif cmd == "draw_viewport" then return Render.draw_viewport(...)
        elseif cmd == "fill_viewport" then return Render.fill_viewport(...)
        elseif cmd == "load_texture" then return Render.load_texture(...)
        elseif cmd == "load_texture_async" then return Render.load_texture_async(...)
        elseif cmd == "cancel_async_loads" then return Render.cancel_async_loads()
        elseif cmd == "is_valid_handle" then return Render.is_valid_handle(...)
        elseif cmd == "destroy_texture" then return Render.destroy_texture(...)
        elseif cmd == "create_solid_texture" then
            return Render.create_solid_texture(...)
        elseif cmd == "get_resolution" then return Render.get_resolution()
        elseif cmd == "submit_stretch_blt" then
            local dst, dst_rect, src, src_rect, filter = ...
            return Render.stretch_blt(dst,
                dst_rect.x, dst_rect.y, dst_rect.w, dst_rect.h,
                src, src_rect.x, src_rect.y, src_rect.w, src_rect.h, filter)
        elseif cmd == "submit_affine_blt" then
            local dst, dst_rect, src, src_rect, matrix = ...
            return Render.affine_blt(dst,
                dst_rect.x, dst_rect.y, dst_rect.w, dst_rect.h,
                src, src_rect.x, src_rect.y, src_rect.w, src_rect.h, matrix)
        elseif cmd == "set_view_name" then return Render.set_view_name(...)
        elseif cmd == "set_screen_offset" then return Render.set_screen_offset(...)
        elseif cmd == "render_text" then return KAG.render_text(...)
        elseif cmd == "render_ruby" then return KAG.render_ruby(...)
        elseif cmd == "clear_text" then return KAG.clear_text(...)
        elseif cmd == "set_font" then return KAG.set_font(...)
        elseif cmd == "line_height" then return KAG.line_height(...)
        elseif cmd == "text_set_font" then return Render.text_set_font(...)
        elseif cmd == "text_reset_state" then return Render.text_reset_state(...)
        elseif cmd == "video_play" then return KAG.video_asset_play(...)
        elseif cmd == "video_stop" then return KAG.video_asset_stop(...)
        elseif cmd == "video_draw" then return KAG.video_asset_draw(...)
        elseif cmd == "video_is_playing" then return KAG.video_asset_is_playing(...)
        else error("[BackendFactory] Unknown render: " .. tostring(cmd)) end
    end

    -- Audio subsystem ----------------------------------------------------
    backend.audio = function(cmd, ...)
        if cmd == "ping" then return true
        elseif cmd == "name" then return actual_audio_name
        elseif cmd == "play_bgm" then return KAG.play_bgm(...)
        elseif cmd == "stop_bgm" then return KAG.stop_bgm(...)
        elseif cmd == "play_voice" then return KAG.play_voice(...)
        elseif cmd == "stop_voice" then return KAG.stop_voice(...)
        elseif cmd == "play_se" then return KAG.play_se(...)
        elseif cmd == "play_se_3d" then return KAG.play_se_3d(...)
        elseif cmd == "set_listener" then return KAG.set_listener(...)
        elseif cmd == "is_voice_playing" then return KAG.is_voice_playing()
        elseif cmd == "is_bgm_playing" then return KAG.is_bgm_playing()
        elseif cmd == "is_playing" then return KAG.is_se_playing()
        elseif cmd == "stop_se" then return KAG.stop_se(...)
        elseif cmd == "set_bus_volume" then return KAG.set_bus_volume(...)
        elseif cmd == "get_bus_volume" then return KAG.get_bus_volume(...)
        elseif cmd == "flush_wave_cache" then return KAG.flush_wave_cache()
        elseif cmd == "fade_volume" then return KAG.audio_fade_volume(...)
        elseif cmd == "get_position" then return KAG.audio_get_position(...)
        elseif cmd == "get_length" then return KAG.audio_get_length(...)
        elseif cmd == "active_voice_count" then return 0
        else error("[BackendFactory] Unknown audio: " .. tostring(cmd)) end
    end

    -- Platform subsystem -------------------------------------------------
    backend.platform = function(cmd, ...)
        if cmd == "ping" then return true
        elseif cmd == "name" then return actual_platform_name
        elseif cmd == "set_input_focus" then return DevCore.set_input_focus(...)
        elseif cmd == "get_input_focus" then return DevCore.get_input_focus()
        elseif cmd == "set_resolution" then return DevCore.set_resolution(...)
        elseif cmd == "get_resolution" then return DevCore.get_resolution()
        elseif cmd == "set_fullscreen" then return DevCore.set_fullscreen(...)
        elseif cmd == "log" then return DevCore.log(...)
        elseif cmd == "start_text_input" then return DevCore.start_text_input()
        elseif cmd == "stop_text_input" then return DevCore.stop_text_input()
        elseif cmd == "set_text_input_rect" then return DevCore.set_text_input_rect(...)
        elseif cmd == "is_text_input_active" then return DevCore.is_text_input_active()
        else error("[BackendFactory] Unknown platform: " .. tostring(cmd)) end
    end

    -- Text / UI shortcuts ------------------------------------------------
    -- U3 de-duplication: convenience methods are now served by
    -- UnifiedBinding C++ layer (_CAESURA_BACKEND). No KAG delegation needed.
    -- Lua aliases (KAG.show_text = _CAESURA_BACKEND.show_text) are set in
    -- kag.lua for backward compatibility.

    rawset(_G, "_CAESURA_BACKEND", backend)
    print(string.format("[BackendFactory] Created: render=%s audio=%s platform=%s",
        actual_render_name, actual_audio_name, actual_platform_name))
    return backend
end

return BackendFactory
