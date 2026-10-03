-- Unsupported running effects are rejected at capture; existing effects are
-- retired only after preparation succeeds and the session commits its load.
local M={}
local function nonempty(value) return type(value)=='table' and next(value)~=nil end
local function reject(reason) error('Cannot save '..reason,0) end

function M.assert_saveable(ctx)
    if type(ctx)~='table' then reject('without a context') end
    if ctx._choiceMode or nonempty(ctx._choiceButtons) or nonempty(ctx._choiceButtonsActive)
        or (ctx._selectedChoice~=nil and ctx._selectedChoice~=false) or ctx._inputMode then
        reject('an unfinished choice or text input')
    end
    if ctx._settingsActive or (ctx.galleryState and ctx.galleryState.active)
        or (ctx.input_focus and ctx.input_focus~='' and ctx.input_focus~='kag') then
        reject('while a runtime menu owns input')
    end
    if ctx._gesture_history_co and coroutine.status(ctx._gesture_history_co)~='dead' then
        reject('an active history overlay')
    end
    if ctx._videoPlayback and not ctx._videoPlayback.closed then reject('active video playback') end
    if nonempty(ctx._particleEmitters) or nonempty(ctx._weatherEmitters) then reject('active particle declarations') end
    if nonempty(ctx.live2d) or nonempty(ctx._live2dHandles) then reject('unrestorable animation declarations') end
    if nonempty(ctx.macros) or nonempty(ctx._macroStack) or nonempty(ctx.macro_args) then
        reject('runtime macro state')
    end
    if ctx.tokens and ctx.tokens._runtime_rewritten then reject('a rewritten macro stream') end
    for _,frame in ipairs(ctx.call_stack or {}) do
        if frame.tokens and frame.tokens._runtime_rewritten then reject('a rewritten caller stream') end
    end
    for _,tween in ipairs(ctx.tweens or {}) do
        if not tween.done and not tween.cancelled then reject('an unfinished tween') end
    end
    for _,token in ipairs(ctx.active_operations or {}) do
        if not token.cancelled and ctx._executing_command~='wait' and ctx._executing_command~='delay' then
            reject('an unfinished '..tostring(ctx._executing_command or 'operation'))
        end
    end
    local api=rawget(_G,'Restore')
    if api and type(api.capture_transients)=='function' then
        local counts=assert(api.capture_transients())
        for _,field in ipairs({'videos','models','particles','emitters','postfx'}) do
            local count=type(counts)=='table' and counts[field]
            if type(count)~='number' or count<0 or count%1~=0 then reject('with invalid '..field..' status') end
            if count>0 then reject('active '..field..' without a restore contract') end
        end
    elseif nonempty(ctx._particleEmitters) or nonempty(ctx._weatherEmitters) then
        reject('active particle effects')
    end
    return true
end

function M.stop(ctx)
    local ok,result,reason=true,true,nil
    local api=rawget(_G,'Restore')
    if api and type(api.stop_transients)=='function' then
        ok,result,reason=pcall(api.stop_transients)
    else
        local backend=package.loaded.backend
        if backend and type(backend.clear_particles)=='function'
            and (nonempty(ctx._particleEmitters) or nonempty(ctx._weatherEmitters)) then
            ok,result,reason=pcall(backend.clear_particles)
        end
    end
    local vfx=package.loaded['kag.commands.vfx']
    if vfx and type(vfx._clear_runtime_state)=='function' then vfx._clear_runtime_state(ctx) end
    ctx._particleEmitters,ctx._weatherEmitters={},{}
    if not ok then return false,tostring(result) end
    if result==false then return false,tostring(reason or 'Transient cleanup failed') end
    -- Native Restore.stop_transients already clears the models. Direct Lua
    -- hosts instead retire this ctx's handles through the same backend proxy.
    if not (api and type(api.stop_transients)=='function') and nonempty(ctx._live2dHandles) then
        local backend=require('backend')
        for name,handle in pairs(ctx._live2dHandles) do
            local called,unloaded,why=pcall(backend.live2d_unload,handle)
            if not called or unloaded~=true then
                return false,tostring(called and (why or 'Model cleanup failed') or unloaded)
            end
            ctx._live2dHandles[name]=nil
            if ctx.live2d then ctx.live2d[name]=nil end
        end
    end
    ctx._live2dHandles,ctx.live2d={},{}
    return true
end
return M
