-- One owner and one failure boundary for all restored presentation resources.
local Layers=require("kag.layer_state")
local Audio=require("kag.audio_state")
local Font=require("kag.font_state")
local Transients=require("kag.transient_state")
local Models=require("kag.live2d_state")
local M={}
local active_owner

function M.adopt(owner) active_owner=owner end

function M.capture()
    return {layers=Layers.capture(),audio=Audio.capture(),font=Font.capture()}
end

local function cleanup(errors,fn,...)
    local ok,result,err=pcall(fn,...)
    if not ok then errors[#errors+1]=tostring(result)
    elseif result==false then errors[#errors+1]=tostring(err or "presentation cleanup failed") end
end

function M.discard(prepared)
    prepared.used=true
    local errors={}
    if prepared.layers then cleanup(errors,Layers.discard,prepared.layers) end
    if prepared.audio then cleanup(errors,Audio.discard,prepared.audio) end
    if prepared.font then cleanup(errors,Font.discard,prepared.font) end
    if prepared.models then cleanup(errors,Models.discard,prepared.models) end
    return #errors==0,table.concat(errors,"; ")
end

function M.prepare(state)
    local prepared={used=false}
    local ok,err=pcall(function()
        prepared.layers=Layers.prepare(state.layer_snapshot)
        prepared.audio=Audio.prepare(state.audio_snapshot)
        prepared.font=Font.prepare(state.font_snapshot)
        prepared.models=Models.prepare(state.live2d_snapshot)
    end)
    if not ok then
        local discarded,reason=M.discard(prepared)
        error(tostring(err)..(discarded and "" or "; discard: "..reason),0)
    end
    return prepared
end

function M.stop(owner,clear_font,keep_models)
    if active_owner~=nil and active_owner~=owner then return true end
    local errors={}
    cleanup(errors,Transients.stop,owner,keep_models)
    cleanup(errors,Layers.stop,owner)
    cleanup(errors,Audio.stop)
    if clear_font then cleanup(errors,Font.clear) end
    return #errors==0,table.concat(errors,"; ")
end

function M.apply(prepared,owner)
    if prepared.used then error("Presentation candidate already consumed",0) end
    prepared.used=true
    local keep_models = prepared.models and prepared.models.handles
    local ok,err=pcall(function()
        if active_owner and active_owner~=owner then assert(M.stop(active_owner,nil,keep_models)) end
        active_owner=owner
        assert(Transients.stop(owner,keep_models))
        assert(Audio.stop())
        Font.apply(prepared.font)
        Layers.apply(prepared.layers,owner)
        Audio.apply(prepared.audio)
        Models.apply(prepared.models,owner)
    end)
    if not ok then
        local discarded,discard_error=M.discard(prepared)
        local stopped,stop_error=M.stop(owner,true)
        error(tostring(err)..(discarded and "" or "; discard: "..discard_error)
            ..(stopped and "" or "; cleanup: "..stop_error),0)
    end
    return true
end

-- Ordinary stop may retain the renderer-owned font cache. An explicit new
-- start after failure must reactivate the stable default, without reviving a
-- font during the failure cleanup itself.
function M.prepare_start()
    if not Font.capture().active then return Font.prepare(nil) end
end
function M.apply_start(prepared)
    if prepared then return Font.apply(prepared) end
    return true
end
function M.discard_start(prepared)
    if prepared then Font.discard(prepared) end
end

return M
