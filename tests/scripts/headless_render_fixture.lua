-- Symbolic host boundary for the Golden drivers. This executes the real Lua
-- render/transition pump, but records submissions: it is NOT GPU evidence.
local M = {}

function M.install()
    -- config installs BackendFactory's proxy on its first require. Complete
    -- that real bootstrap before taking ownership of the host boundary;
    -- otherwise runner.start lazily overwrites this recorder and Render's
    -- old generic mock returns boolean handles to the layer tree.
    local config = require('config')
    local hw = {frame=0, next_id=1000, handles={}, completed={}, pending={},
        captures=0, transitions=0, width=config.window_width, height=config.window_height}
    local function allocate(kind, value)
        hw.next_id = hw.next_id + 1
        hw.handles[hw.next_id] = {kind=kind, value=value}
        return hw.next_id
    end
    local function record(method, ...)
        -- Layer batches are reused in-place by Lua; a completed symbolic frame
        -- owns its values rather than retaining the next frame's mutable table.
        local function copy(value)
            if type(value)~='table' then return value end
            local result={}
            for k,v in pairs(value) do result[k]=copy(v) end
            return result
        end
        hw.pending[#hw.pending+1] = copy({method, ...})
        return true
    end
    local noops = {cancel_transition=true, cancel_async_loads=true,
        set_view_name=true, text_reset_state=true, clear_text=true,
        set_font=true, text_set_font=true, set_screen_offset=true,
        set_color_filter=true, clear_postfx=true, destroy_postfx=true}
    local draws = {submit_batch=true, draw_viewport=true, fill_viewport=true,
        render_text=true, render_ruby=true, submit_blend=true, submit_vfx=true}
    local function render(method, ...)
        local a = {...}
        if method == 'capture_scene' then
            if hw.frame == 0 or os.getenv('CAESURA_HEADLESS_CAPTURE_FAIL') == '1' then return 0,0 end
            hw.captures = hw.captures + 1
            return allocate('snapshot', {frame=hw.frame, submissions=hw.completed}), hw.frame
        elseif method == 'submit_transition' then
            local from, to = hw.handles[a[2]], hw.handles[a[3]]
            assert(from and from.kind=='snapshot' and to and to.kind=='snapshot',
                'headless transition requires live captured snapshots')
            assert(type(a[6])=='number' and a[6]>=0 and a[6]<=1, 'invalid transition progress')
            if a[2] ~= a[3] then
                assert(to.value.frame > from.value.frame, 'transition destination must be a later frame')
            end
            hw.transitions = hw.transitions + 1
            return record(method, ...)
        elseif method == 'create_viewport' or method == 'load_texture'
            or method == 'create_solid_texture' then
            if method=='load_texture' and os.getenv('CAESURA_HEADLESS_COMMAND_THROW')=='1' then
                error('injected headless texture command failure')
            end
            return allocate(method, a)
        elseif method == 'destroy_viewport' or method == 'destroy_texture' then
            hw.handles[a[1]] = nil
            return true
        elseif method == 'is_valid_handle' then return hw.handles[a[2]] ~= nil
        elseif method == 'line_height' then return 24
        elseif method == 'get_resolution' then return hw.width, hw.height
        elseif method == 'is_postfx_active' or method == 'is_postfx_supported' then return false
        elseif method == 'set_postfx' then return 0
        elseif noops[method] then return true
        elseif draws[method] then return record(method, ...)
        end
        error('unsupported headless render boundary: '..tostring(method))
    end
    local adapter = {
        render=render,
        -- Preserve the drivers' existing save/voice mocks. Direct convenience
        -- functions still resolve through KAG; audio dispatch reaches that same table.
        audio=function(method, ...)
            if method=='get_bus_volume' then return 1 end
            if method=='get_length' or method=='get_position' then return 0 end
            if method=='is_playing' then return false end
            return _G.KAG[method](...)
        end,
        platform=function(method, ...)
            local a={...}
            if method=='get_resolution' then return hw.width,hw.height end
            if method=='set_resolution' then hw.width,hw.height=a[1],a[2]; return true end
            if method=='log' then print(a[1]);return true end
            return _G.DevCore[method](...)
        end,
    }
    _G._CAESURA_BACKEND = adapter
    function hw.check(runner, ok, reason)
        assert(rawget(_G, '_CAESURA_BACKEND') == adapter, 'headless renderer ownership replaced')
        local ctx = runner.get_ctx()
        if (ctx and ctx._command_error) or reason=='command-error' then
            error('headless command-error: '..tostring(ctx and ctx.error_command))
        end
        if ok == false and reason ~= 'ended' and reason ~= 'render-pending'
            and reason ~= 'waiting-input' and reason ~= 'kag-paused'
            and reason ~= 'debug-paused' and reason ~= 'dead' then
            error('headless runner failure: '..tostring(reason))
        end
    end
    function hw.pump(runner)
        hw.pending = {}
        require('layers').render()
        local ok, reason = runner.render()
        assert(ok, 'headless render failed: '..tostring(reason))
        -- Completion belongs to the host frame boundary, never capture_scene.
        hw.frame = hw.frame + 1
        hw.completed = hw.pending
        hw.check(runner)
    end
    print('HEADLESS_RENDERER symbolic submissions; not GPU evidence')
    return hw
end
return M
