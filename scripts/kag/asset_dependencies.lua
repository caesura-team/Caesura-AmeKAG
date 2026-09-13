-- Build/check-time media dependency collection. No command dispatch, author Lua
-- execution, interpolation evaluation, filesystem lookup, or extension guessing.
local tokenizer = require("tokenizer")
local compiler = require("kag.compiler")
local schema = require("kag.schema")
local capabilities = require("target_capabilities")
local json = require("capability_json")
local M = {}

-- These handlers share storage > path > file > first positional resolution
-- (commands/layer.lua, commands/audio.lua and commands/video.lua).
local file_commands = {bg=true,fg=true,image=true,playbgm=true,playbgmstop=true,playse=true,
    playvoice=true,xfadebgm=true,video=true}
-- kag.lua wrappers deliberately forward storage/file/positional, not path.
local audio_aliases = {play=true,bgm=true,se=true,voice=true}

function M.new_report()
    return {schema=1,analysis_scope="known_static_media_commands",
        media=json.array(),dynamic=json.array(),invalid=json.array()}
end

local function command_params(token)
    if token.type and token.type~="command" then return nil end
    local command=token.cmd or token[1]
    if type(command)~="string" then return nil end
    return command,compiler.normalize_params(command,token.params or token[2])
end

local function first(params, names)
    for _,name in ipairs(names) do
        if params[name]~=nil then return params[name],name end
    end
end

local function dynamic(path)
    -- Match the capability checker's conservative static boundary. These are
    -- recorded as unproved strings, never executed or expanded by this module.
    return path:find("$",1,true) or path:find("%",1,true) or path:match("^%s*&")
end

local function unsafe(path)
    return path:find("..",1,true) or path:find(":",1,true)
        or path:match("^/") or path:find("[%z\1-\31]")
end

function M.collect(tokens, scene)
    -- Register only the engine's own contracts. Never load the author's entry
    -- script or call schema.coerce, which can evaluate runtime interpolation.
    require("kag")
    local result=M.new_report()
    local local_macros={}
    local possible_macros={}
    for _,token in ipairs(tokens) do
        local command,params=command_params(token)
        if command=="macro" then
            local name=params.name or params[1]
            if type(name)=="string" then possible_macros[name]=true end
        end
    end
    local flow_depth,macro_depth,transfer_unknown=0,0,false
    local opens={ ["if"]=true,["while"]=true,["for"]=true,["switch"]=true }
    local closes={endif=true,endwhile=true,endfor=true,endswitch=true}
    local transfers={jump=true,["goto"]=true,call=true,["return"]=true}
    for index,token in ipairs(tokens) do
        local command,params=command_params(token)
        if token.type=="iscript" then transfer_unknown=true end
        if opens[command] then flow_depth=flow_depth+1 end
        -- Definitions only shadow subsequent calls; erasure restores the
        -- builtin. A future definition cannot hide an earlier media command.
        if command=="macro" or command=="erasemacro" then
            local name=params.name or params[1]
            if type(name)=="string" then
                if flow_depth>0 or macro_depth>0 or transfer_unknown then
                    local_macros[name]="uncertain"
                else
                    local_macros[name]=command=="macro" or nil
                end
            end
        end
        if command=="macro" then macro_depth=macro_depth+1 end
        if command=="endmacro" then macro_depth=math.max(0,macro_depth-1) end
        local dispatch_unproven=command and (local_macros[command]=="uncertain"
            or (transfer_unknown and possible_macros[command]))
        if command and (not local_macros[command] or dispatch_unproven) then
            local specs=schema.specs(command) or schema.specs(audio_aliases[command] and "play" or "")
            local function add(path,field,optional_empty)
                if path==nil or (optional_empty and path=="") then return end
                local features=capabilities.command_features(command,params,true)
                local record={scene=scene,token=index,command=command,field=tostring(field),features=features}
                if type(path)~="string" then
                    record.path="<non-string>";record.reason="non_string_media_path"
                    result.invalid[#result.invalid+1]=record
                    return
                end
                path=path:gsub("\\","/")
                record.path=path
                if dispatch_unproven then
                    record.reason="macro_dispatch_unproven"
                    result.dynamic[#result.dynamic+1]=record
                elseif dynamic(path) then
                    record.reason="dynamic_media_path_unproven"
                    result.dynamic[#result.dynamic+1]=record
                elseif path=="" or unsafe(path) then
                    record.reason="unsafe_or_empty_media_path"
                    result.invalid[#result.invalid+1]=record
                else
                    result.media[#result.media+1]=record
                end
            end
            if specs and file_commands[command] then
                local path,field=first(params,{"storage","path","file",1})
                add(path,field)
            elseif specs and audio_aliases[command] then
                local path,field=first(params,{"storage","file",1})
                add(path,field)
            elseif command=="ch" and specs then
                -- ch registers a sprite and independently plays line voice.
                local sprite=params.sprite
                if sprite==nil or sprite=="" then
                    local path,field=first(params,{"storage","file"})
                    add(path,field,true)
                else add(sprite,"sprite",true) end
                add(params.voice,"voice",true)
            elseif command=="sprite_swap" and specs then
                add(params.sprite,"sprite")
            elseif command=="csp" and specs then
                local path,field=first(params,{"storage","path","file"})
                if path==nil or path=="" then
                    local name=params.name or params[1]
                    if type(name)=="string" and name~="" then
                        path="assets/char/"..name..".png";field="name"
                    end
                end
                add(path,field)
            elseif command=="preload" and specs then
                local kind=params.type or specs.type.default
                -- Read the same declared defaults that dispatch applies; an
                -- empty path default must not invent a storage alias fallback.
                local paths=params.path
                if paths==nil then paths=specs.path.default or params.storage end
                if kind=="texture" or kind=="audio" then
                    if type(paths)=="string" then
                        for path in paths:gmatch("[^,]+") do
                            add(path:match("^%s*(.-)%s*$"),"path",true)
                        end
                    end
                elseif type(kind)=="string" and dynamic(kind) and type(paths)=="string" and paths~="" then
                    result.dynamic[#result.dynamic+1]={scene=scene,token=index,command=command,
                        field="path",path=paths,reason="dynamic_preload_kind_unproven"}
                end
            end
            -- call/jump/link/selection and preload type=scene are navigation,
            -- not media; file-looking text/iscript bodies are never inspected.
        end
        if closes[command] then flow_depth=math.max(0,flow_depth-1) end
        -- No CFG or author expression execution: after a transfer or a macro
        -- invocation, later macro-sensitive dispatch is explicitly unproved.
        if transfers[command] or (command and local_macros[command] and command~="macro") then
            transfer_unknown=true
        end
    end
    return result
end

function M.extend(destination, source)
    for _,field in ipairs({"media","dynamic","invalid"}) do
        for _,record in ipairs(source[field]) do
            -- Literal bundles lose private JSON array tags. Copy the record
            -- and its feature array; reporting must not retag caller input.
            local copy={}
            for key,value in pairs(record) do copy[key]=value end
            if type(record.features)=="table" then
                copy.features=json.array()
                for _,feature in ipairs(record.features) do
                    copy.features[#copy.features+1]=feature
                end
            end
            destination[field][#destination[field]+1]=copy
        end
    end
    return destination
end

function M.scan_files(files)
    local result=M.new_report()
    for _,file in ipairs(files) do
        M.extend(result,M.collect(tokenizer.parse_file(file.path),file.name))
    end
    return result
end

function M.apply_capabilities(report, checked)
    assert(type(report)=="table" and report.schema==1,"missing media dependency report")
    local skipped_features={}
    if checked then
        assert(checked.passed==true,"media dependencies require a passed capability report")
        for _,requirement in ipairs(checked.requirements or {}) do
            if requirement.decision=="skip" then skipped_features[requirement.feature]=true end
        end
    end
    local result=M.new_report()
    M.extend(result,report)
    result.static=json.array()
    result.skipped=json.array()
    local seen={}
    for _,record in ipairs(result.media) do
        local skip=false
        for _,feature in ipairs(record.features or {}) do
            if skipped_features[feature] then skip=true end
        end
        if skip then
            local copy={}
            for key,value in pairs(record) do copy[key]=value end
            copy.reason="capability_skip_not_verified"
            result.skipped[#result.skipped+1]=copy
        elseif not seen[record.path] then
            seen[record.path]=true
            result.static[#result.static+1]=record.path
        end
    end
    return result
end

return M
