-- Capability results at the Lua backend boundary. Existing first return values
-- remain the operation's handles/booleans; the second value describes the result.
local runtime = require("capability_runtime")
local policy = require("target_capabilities")
local M = {}
local installed = setmetatable({}, {__mode="k"})
local postfx = {
    bloom="render.postfx.bloom", vignette="render.postfx.vignette",
    lut="render.postfx.lut", softblur="render.postfx.softblur", lut3d="render.postfx.lut3d",
}
local function context()
    local value = rawget(_G, "_CAESURA_CTX")
    return type(value)=="table" and value or nil
end
local function integer(value, minimum, maximum)
    return type(value)=="number" and value>=minimum and value<=maximum and value%1==0
end
local function success(value, mode)
    if mode=="particle" then return integer(value,0,2147483647) end
    if mode=="handle" then return value==true or integer(value,1,4294967295) end
    if mode=="void" then return value==nil or value==true end
    return value==true
end
local function decision_for(features, ctx, command)
    local selected
    for _, feature in ipairs(features) do
        local decision=runtime.evaluate(feature,ctx,command)
        if decision.decision=="deny" then return decision end
        if not selected or decision.decision=="skip"
            or (selected.decision=="allow" and decision.capability.status=="approximate") then selected=decision end
    end
    return selected
end

function M.install(backend)
    if type(backend)~="table" or installed[backend] then return backend end
    installed[backend]=true
    backend.get_capability=function(feature) return runtime.query(feature) end
    local function wrap(name, features, mode, failure)
        local original=backend[name]
        if original==nil then return end
        backend[name]=function(...)
            if not runtime.has_host() then return original(...) end
            local ctx=context()
            local command=ctx and ctx._executing_command or name
            -- Disabling an existing LUT is cleanup, including after a target
            -- or policy change. It must not require permission to apply one.
            if name=="set_postfx" then
                local kind,params=...
                if kind=="lut3d" and type(params)=="table" and params.lutId==0 then return original(...) end
            elseif name=="set_palette" then
                local handle=...
                if handle==nil or handle==0 then return original(...) end
            end
            local required=type(features)=="function" and features(...) or {features}
            local decision=decision_for(required,ctx,command)
            if decision and decision.decision~="allow" then
                return failure,runtime.finish(decision,false,ctx)
            end
            local value=original(...)
            return value,decision and runtime.finish(decision,success(value,mode),ctx) or nil
        end
    end
    wrap("set_postfx",function(kind) return {postfx[kind] or "unknown_feature"} end,"handle",0)
    wrap("set_palette","render.postfx.lut3d","void",false)
    wrap("particles_create_emitter","render.particles","particle",-1)
    wrap("particles_emit","render.particles","void",false)
    wrap("video_play",function(file) return policy.command_features("video",{file=file},false) end,"handle",0)
    wrap("audio_play",function(bus,_,options)
        local features={"audio.play"}
        if bus=="bgm" and type(options)=="table" and (tonumber(options.fadein) or 0)>0 then
            features[#features+1]="audio.fade"
        end
        return features
    end,"handle",false)
    wrap("audio_fade_volume","audio.fade","void",false)
    wrap("audio_xfade","audio.crossfade","void",false)
    local stop=backend.audio_stop
    if stop then
        backend.audio_stop=function(bus,options,...)
            if not runtime.has_host() or bus~="bgm" or type(options)~="table"
                or (tonumber(options.fadeout) or 0)<=0 then return stop(bus,options,...) end
            local ctx=context()
            local decision=runtime.evaluate("audio.fade",ctx,ctx and ctx._executing_command or "audio_stop")
            local actual=options
            if decision.decision~="allow" then
                actual={}
                for key,value in next,options do actual[key]=value end
                actual.fadeout=0
            end
            -- Stop always reaches its owner. The second value describes the
            -- requested fade, which may have been rejected independently.
            local value=stop(bus,actual,...)
            return value,runtime.finish(decision,success(value,"void"),ctx)
        end
    end
    local probe=backend.is_postfx_supported
    if probe then
        backend.is_postfx_supported=function(kind)
            if not runtime.has_host() then return probe(kind) end
            local decision=runtime.evaluate(postfx[kind] or "unknown_feature",nil,"is_postfx_supported")
            return decision.decision=="allow" and probe(kind)==true,decision.capability
        end
    end
    return backend
end
return M
