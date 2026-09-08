-- Persisted layer declarations and short-lived preparation ownership.
-- Only apply/stop touch the live tree or GPU; prepare reads immutable pixels.
local Layers = require("layers")
local backend = require("backend")
local rtt = require("rtt")
local copy = require("kag.save_state").copy
local M = {}
local active_owner
local scalar_fields = {"x","y","w","h","z","opacity","scale","scaleX","scaleY",
    "rotation","originX","originY","alpha","pos_x","pos_y","clipX","clipY","clipW","clipH",
    "imgX","imgY","imgW","imgH","layer_type"}

local function number(value, low, high, integer)
    return type(value)=="number" and value==value and value>=low and value<=high
        and (not integer or value==math.floor(value))
end

local function valid_path(path)
    return type(path)=="string" and #path>0 and #path<=4096
        and path:sub(1,1)~="/" and not path:find("..",1,true)
        and not path:find("[%z\1-\31\\:]")
end

local function source_value(source)
    if source==nil or source==false then return false end
    if type(source)~="table" then error("Invalid saved texture source",0) end
    if source.kind=="asset" and valid_path(source.path) then
        return {kind="asset",path=source.path}
    end
    if source.kind=="color" then
        for _,key in ipairs({"r","g","b","a"}) do
            if not number(source[key],0,255,true) then error("Invalid saved color",0) end
        end
        return {kind="color",r=source.r,g=source.g,b=source.b,a=source.a}
    end
    error("Invalid saved texture origin",0)
end

local function node_value(node)
    if type(node)~="table" or type(node.id)~="string" or #node.id==0 or #node.id>128 then
        error("Invalid saved layer identity",0)
    end
    local result = {id=node.id,parent=node.parent,source=source_value(node.source)}
    for _,key in ipairs(scalar_fields) do
        local value=node[key]
        if value~=nil then
            local low,high,integral=-1000000,1000000,false
            if key=="w" or key=="h" or key=="clipW" or key=="clipH" or key=="imgW" or key=="imgH" then
                low,high,integral=0,16384,true
            elseif key=="opacity" then low,high,integral=0,255,true
            elseif key=="alpha" then low,high=0,1
            elseif key=="layer_type" then low,high,integral=0,7,true
            elseif key=="scale" or key=="scaleX" or key=="scaleY" then low,high=-16,16 end
            if not number(value,low,high,integral) then error("Invalid saved layer "..key,0) end
            result[key]=value
        end
    end
    if node.visible~=nil and type(node.visible)~="boolean" then error("Invalid saved visibility",0) end
    result.visible=node.visible~=false
    for _,key in ipairs({"name","tag","blend_mode"}) do
        local value=node[key]
        if value~=nil then
            local valid=type(value)=="string" and #value<=128
            if key=="blend_mode" then valid=valid or number(value,0,28,true) end
            if not valid then error("Invalid saved layer "..key,0) end
            result[key]=value
        end
    end
    return result
end

function M.capture()
    local nodes,seen={},{}
    local api=rawget(_G,"Restore")
    local function walk(node,parent,depth)
        if depth>64 or #nodes>=256 or seen[node] then error("Layer tree exceeds restore limits",0) end
        seen[node]=true
        for _,key in ipairs({"quake","shake","fade"}) do
            if node[key] and node[key].active then error("Cannot save an active layer effect",0) end
        end
        if node.userdata~=nil then error("Cannot save an opaque layer payload",0) end
        local record={id=node.id,parent=parent,visible=node.visible,name=node.name,
            tag=node.tag,blend_mode=node.blend_mode}
        for _,key in ipairs(scalar_fields) do record[key]=node[key] end
        local texture=node.tex or node.texture or 0
        if texture~=0 then
            if not api or type(api.describe_texture)~="function" then error("Texture capture unavailable",0) end
            record.source=assert(api.describe_texture(texture))
        elseif node.rt and node.rt~=0 then
            error("Cannot save an RTT without a reconstructible image",0)
        end
        nodes[#nodes+1]=node_value(record)
        for _,child in ipairs(node.children or {}) do walk(child,node.id,depth+1) end
    end
    -- Web can transfer the complete tree in one bridge call. The resulting
    -- detached Lua values still pass the same source/shape/limit validation.
    local root=type(Layers.capture_restore_tree)=="function"
        and Layers.capture_restore_tree(scalar_fields) or Layers.get_root()
    walk(root,nil,0)
    return {version=1,nodes=nodes}
end

function M.discard(prepared)
    prepared.used=true
    local errors={}
    for _,image in ipairs(prepared.images or {}) do
        if image.ticket then
            local ticket=image.ticket
            image.ticket=nil
            local ok,err=pcall(prepared.api.discard_image,ticket)
            if not ok then errors[#errors+1]=tostring(err) end
        end
    end
    return #errors==0,table.concat(errors,"; ")
end

function M.prepare(description)
    if description==nil then description={version=1,nodes={{id="_root"}}} end
    description=copy(description)
    if type(description)~="table" or description.version~=1 or type(description.nodes)~="table" or #description.nodes==0
        or #description.nodes>256 then error("Invalid saved layer tree",0) end
    local prepared={nodes={},images={},api=rawget(_G,"Restore"),used=false}
    local known,by_source={},{}
    for i,node in ipairs(description.nodes) do
        local record=node_value(node)
        if known[record.id] or (i==1 and (record.id~="_root" or record.parent~=nil))
            or (i>1 and not known[record.parent]) then error("Invalid saved layer parent/order",0) end
        local depth=i==1 and 0 or known[record.parent]+1
        if depth>64 then error("Saved layer tree is too deep",0) end
        known[record.id]=depth
        if record.source then
            -- Render.create_viewport rejects dimensions above 4096. A known
            -- invalid target is a preparation failure, before retiring A.
            if (record.w or 0)>4096 or (record.h or 0)>4096 then
                error("Saved layer target exceeds viewport limits",0)
            end
            local s=record.source
            local key=s.kind=="asset" and "asset:"..s.path
                or string.format("color:%d:%d:%d:%d",s.r,s.g,s.b,s.a)
            local image=by_source[key]
            if not image then
                image={source=s,id=0}
                prepared.images[#prepared.images+1]=image
                by_source[key]=image
            end
            record.image=image
        end
        prepared.nodes[i]=record
    end
    local ok,err=pcall(function()
        local bytes=0
        for _,image in ipairs(prepared.images) do
            local api,s=prepared.api,image.source
            if not api then error("Image preparation unavailable",0) end
            if s.kind=="asset" then image.ticket=assert(api.prepare_image(s.path))
            else image.ticket=assert(api.prepare_color(s.r,s.g,s.b,s.a)) end
            local w,h=api.image_info(image.ticket)
            if not number(w,1,16384,true) or not number(h,1,16384,true) then
                error("Invalid prepared image dimensions",0)
            end
            bytes=bytes+w*h*4
            if bytes>256*1024*1024 then error("Prepared layer pixels exceed 256 MiB",0) end
        end
    end)
    if not ok then
        local discarded,discard_error=M.discard(prepared)
        error(tostring(err)..(discarded and "" or "; cleanup: "..discard_error),0)
    end
    return prepared
end

function M.stop(owner)
    local errors={}
    if active_owner==nil or active_owner==owner then
        active_owner=nil
        local ok,err=Layers.clear_for_restore()
        if not ok then errors[#errors+1]=err end
    end
    for _,image in ipairs(owner._restoredTextures or {}) do
        local id=image.id
        image.id=0
        if id~=0 then
            local ok,err=pcall(backend.destroy_texture,id)
            if not ok then errors[#errors+1]=tostring(err) end
        end
    end
    owner._restoredTextures=nil
    return #errors==0,table.concat(errors,"; ")
end

function M.apply(prepared,owner)
    if prepared.used then error("Layer candidate already consumed",0) end
    prepared.used=true
    local ok,err=pcall(function()
        if active_owner then assert(M.stop(active_owner)) end
        assert(Layers.clear_for_restore())
        active_owner=owner
        -- Reserve ownership records before GPU work. Each image already has
        -- an id field, so publishing a returned ID needs no new Lua table.
        owner._restoredTextures=prepared.images
        for _,image in ipairs(prepared.images) do
            image.id=assert(prepared.api.materialize_image(image.ticket))
            image.ticket=nil
        end
        -- A later upload can evict an earlier one when the whole scene does
        -- not fit the texture budget. Never publish such a partial picture.
        for _,image in ipairs(prepared.images) do
            if backend.is_valid_handle(0,image.id)~=true then
                error("A required restored texture is no longer available",0)
            end
        end
        if type(Layers.install_prepared)=="function" then
            assert(Layers.install_prepared(prepared.nodes))
            return
        end
        local installed={}
        for i,record in ipairs(prepared.nodes) do
            local node=i==1 and Layers.get_root()
                or Layers.add_layer(installed[record.parent],{id=record.id,z=record.z})
            for _,key in ipairs(scalar_fields) do if record[key]~=nil then node[key]=record[key] end end
            node.visible,node.name,node.tag=record.visible,record.name,record.tag
            node.blend_mode=record.blend_mode or "alpha"
            node.tex=record.image and record.image.id or 0
            if record.image and (node.w or 0)>0 and (node.h or 0)>0 then
                node.rt=rtt.acquire(node.w,node.h)
                if not number(node.rt,1,4294967295,true) then error("Required layer RTT creation failed",0) end
            end
            installed[record.id]=node
        end
    end)
    if not ok then
        local discarded,discard_error=M.discard(prepared)
        local stopped,stop_error=M.stop(owner)
        error(tostring(err)..(discarded and "" or "; discard: "..discard_error)
            ..(stopped and "" or "; cleanup: "..stop_error),0)
    end
    return true
end

return M
