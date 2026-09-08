-- Rollback reuses save/load resource ownership, without rewinding BGM or SE.
local Layers=require("kag.layer_state")
local Font=require("kag.font_state")
local M={}

local function cleanup(errors,fn,...)
    local ok,result,err=pcall(fn,...)
    if not ok then errors[#errors+1]=tostring(result)
    elseif result==false then errors[#errors+1]=tostring(err or "rollback cleanup failed") end
end

function M.capture()
    return Layers.capture(),Font.capture()
end

function M.discard(prepared)
    prepared.used=true
    local errors={}
    if prepared.layers then cleanup(errors,Layers.discard,prepared.layers) end
    if prepared.font then cleanup(errors,Font.discard,prepared.font) end
    return #errors==0,table.concat(errors,"; ")
end

function M.prepare(layers,font)
    local prepared={used=false}
    local ok,err=pcall(function()
        prepared.layers=Layers.prepare(layers)
        prepared.font=Font.prepare(font)
    end)
    if not ok then
        local discarded,reason=M.discard(prepared)
        error(tostring(err)..(discarded and "" or "; discard: "..reason),0)
    end
    return prepared
end

function M.apply(prepared,owner)
    if prepared.used then error("Rollback presentation candidate already consumed",0) end
    prepared.used=true
    local ok,err=pcall(function()
        assert(Layers.apply(prepared.layers,owner))
        assert(Font.apply(prepared.font))
    end)
    if not ok then
        local discarded,discard_error=M.discard(prepared)
        local errors={}
        cleanup(errors,Layers.stop,owner)
        cleanup(errors,Font.clear)
        error(tostring(err)..(discarded and "" or "; discard: "..discard_error)
            ..(#errors==0 and "" or "; cleanup: "..table.concat(errors,"; ")),0)
    end
    return true
end

return M
