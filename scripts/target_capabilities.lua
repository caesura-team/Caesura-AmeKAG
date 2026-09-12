-- Pure capability policy over generated data. No files, host configuration or
-- command execution here; callers own profile provenance and runtime guards.
local json=require("capability_json")
local generated=require("capability_catalog")
local raw_get,raw_next,raw_equal=rawget,next,rawequal
local catalog=assert(json.decode(raw_get(generated,"json")),"invalid_capability_catalog")
local FEATURES=raw_get(catalog,"features")
local DIGEST=raw_get(generated,"sha256")
local M={catalog_sha256=DIGEST}
local UNKNOWN="unknown_feature"
local FIELDS={"required","optional","accept_approximate"}
local FIELD_SET={required=true,optional=true,accept_approximate=true}
local COMPILED={ffmpeg=true,live2d=true,steam=true}
local PLATFORMS={windows=true,linux=true,macos=true,ios=true,android=true}
local PROFILE_KEYS={schema=true,target=true,scope=true,platform=true,catalog_sha256=true,
    compiled=true,available=true,binary_sha256=true,binary=true,bundle_files=true,source_files=true}
local GUARDED_COMMANDS={postprocess=true,vfx=true,particles=true,particle_weather=true,palette=true,
    video=true,xfadebgm=true,fadebgm=true,fadevol=true,stopbgm=true,playstop=true,playbgmstop=true,
    play=true,bgm=true,playbgm=true,playse=true,playvoice=true,se=true,voice=true,
    live2d_motion=true,live2d_expression=true,live2d_lip_sync=true,steam_achievement=true}
function M.command_has_capabilities(command) return GUARDED_COMMANDS[command]==true end
local REQUIRED_FACTS={native={},web={}}
local FACTS={}
for _,feature in raw_next,FEATURES do
    for _,target in ipairs({"native","web"}) do
        local fact=raw_get(raw_get(feature,target),"available")
        if fact then REQUIRED_FACTS[target][fact]=true;FACTS[fact]=true end
    end
end

local function object(value)
    if type(value)~="table" or raw_equal(value,json.null) or json.is_array(value) then return false end
    for key in raw_next,value do if type(key)~="string" then return false end end
    return true
end
local function source_path(path)
    if #path==0 or #path>4096 or path:find("[%z\1-\31\127\\:]")
        or path:sub(1,1)=="/" or path:sub(-1)=="/" or path:find("//",1,true)
        or not json.encode(path) then return false end
    for part in path:gmatch("[^/]+") do if part=="." or part==".." then return false end end
    return true
end
local function known(feature) return type(feature)=="string" and raw_get(FEATURES,feature)~=nil end
local function length(value)
    if type(value)~="table" or raw_equal(value,json.null) or json.is_object(value) then return nil end
    local count,maximum=0,0
    for key in raw_next,value do
        if type(key)~="number" or key<1 or key>json.MAX_NODES or key%1~=0 then return nil end
        count=count+1;maximum=math.max(maximum,key)
    end
    if count~=maximum then return nil end
    return count
end
local function valid_policy(policy)
    if not object(policy) then return false end
    for key in raw_next,policy do if not FIELD_SET[key] then return false end end
    for _,field in ipairs(FIELDS) do
        local values=raw_get(policy,field)
        if not object(values) then return false end
        for feature,enabled in raw_next,values do
            if not known(feature) or enabled~=true then return false end
        end
    end
    for feature in raw_next,raw_get(policy,"required") do
        if raw_get(raw_get(policy,"optional"),feature) then return false end
    end
    return true
end

function M.validate_project(document)
    if not json.is_object(document) or not object(document) then return nil,"invalid_project_object" end
    local policy={required={},optional={},accept_approximate={}}
    local declaration=raw_get(document,"capabilities")
    if declaration==nil then return policy end
    if not json.is_object(declaration) or not object(declaration) then return nil,"invalid_capabilities_object" end
    for key in raw_next,declaration do if not FIELD_SET[key] then return nil,"unknown_declaration_field" end end
    for _,field in ipairs(FIELDS) do
        local values=raw_get(declaration,field)
        if values~=nil then
            if not json.is_array(values) then return nil,"invalid_declaration_array" end
            local count=length(values)
            if not count then return nil,"invalid_declaration_array" end
            for index=1,count do
                local feature=raw_get(values,index)
                if not known(feature) then return nil,"unknown_feature" end
                if policy[field][feature] then return nil,"duplicate_declaration_feature" end
                policy[field][feature]=true
            end
        end
    end
    for feature in raw_next,policy.required do
        if policy.optional[feature] then return nil,"required_optional_overlap" end
    end
    return policy
end

function M.validate_profile(profile)
    if not object(profile) then return nil,"invalid_profile_object" end
    for key in raw_next,profile do if not PROFILE_KEYS[key] then return nil,"unknown_profile_field" end end
    if raw_get(profile,"schema")~=1 then return nil,"invalid_profile_schema" end
    local target,scope=raw_get(profile,"target"),raw_get(profile,"scope")
    if target~="native" and target~="web" then return nil,"invalid_profile_target" end
    if scope~="build" and scope~="runtime" then return nil,"invalid_profile_scope" end
    local platform=raw_get(profile,"platform")
    if (target=="native" and not PLATFORMS[platform]) or (target=="web" and platform~="browser") then
        return nil,"invalid_profile_platform"
    end
    if raw_get(profile,"catalog_sha256")~=DIGEST then return nil,"catalog_identity_mismatch" end
    local compiled=raw_get(profile,"compiled")
    if not object(compiled) then return nil,"invalid_compiled_facts" end
    if target=="web" then
        if raw_next(compiled)~=nil then return nil,"invalid_web_compiled_facts" end
    else
        for key in raw_next,compiled do if not COMPILED[key] then return nil,"unknown_compiled_fact" end end
        for key in raw_next,COMPILED do if type(raw_get(compiled,key))~="boolean" then return nil,"missing_compiled_fact" end end
    end
    local available=raw_get(profile,"available")
    if scope=="runtime" or available~=nil then
        if not object(available) then return nil,"invalid_available_facts" end
        for key,value in raw_next,available do
            if not FACTS[key] or type(value)~="boolean" then return nil,"invalid_available_fact" end
        end
        if scope=="runtime" then
            for key in raw_next,REQUIRED_FACTS[target] do
                if type(raw_get(available,key))~="boolean" then return nil,"missing_available_fact" end
            end
        end
    end
    local sources=raw_get(profile,"source_files")
    if sources~=nil then
        if not object(sources) then return nil,"invalid_source_files" end
        for path,digest in raw_next,sources do
            if not source_path(path)
                or type(digest)~="string" or #digest~=64 or digest:find("[^0-9a-f]") then
                return nil,"invalid_source_files"
            end
        end
    end
    -- Provenance shape is checked here; filesystem attestation belongs to the packager.
    return true
end

local function identity(profile)
    local target,scope="unknown","unknown"
    if type(profile)=="table" then
        local t,s=raw_get(profile,"target"),raw_get(profile,"scope")
        if t=="native" or t=="web" then target=t end
        if s=="build" or s=="runtime" then scope=s end
    end
    return target,scope
end
function M.query(feature,profile)
    local target,scope=identity(profile)
    local result={feature=known(feature) and feature or UNKNOWN,target=target,scope=scope,proven=true}
    if not known(feature) then result.status="unsupported";result.reason="unknown_feature";return result end
    local valid,reason=M.validate_profile(profile)
    if not valid then result.proven=false;result.reason=reason;return result end
    local entry=raw_get(raw_get(FEATURES,feature),target)
    result.status=raw_get(entry,"status")
    result.reason=raw_get(entry,"reason") or "supported"
    if result.status=="unsupported" then return result end
    local compiled=raw_get(entry,"compiled")
    if compiled and raw_get(raw_get(profile,"compiled"),compiled)~=true then
        result.status="unsupported";result.reason="sdk_disabled";return result
    end
    local available=raw_get(entry,"available")
    if scope=="runtime" and available and raw_get(raw_get(profile,"available"),available)~=true then
        result.status="unsupported";result.reason="backend_unavailable"
    end
    return result
end

local function location(value)
    local result={}
    if type(value)~="table" then return result end
    for _,key in ipairs({"scene","command"}) do
        local text=raw_get(value,key)
        if type(text)=="string" and json.encode(text) then result[key]=text end
    end
    local line=raw_get(value,"line")
    if type(line)=="number" and line>=0 and line<math.huge and line%1==0 then result.line=line end
    return result
end
function M.decide(policy,feature,profile,where)
    local capability=M.query(feature,profile)
    local result={decision="deny",capability=capability,location=location(where)}
    if capability.reason=="unknown_feature" or not capability.proven or not valid_policy(policy) then return result end
    local optional=raw_get(raw_get(policy,"optional"),feature)==true
    if capability.status=="supported" then result.decision="allow"
    elseif capability.status=="approximate" then
        if raw_get(raw_get(policy,"accept_approximate"),feature)==true then result.decision="allow"
        elseif optional then result.decision="skip" end
    elseif optional then result.decision="skip" end
    return result
end

function M.check(policy,profile,calls,unproven)
    local target,scope=identity(profile)
    local report={passed=true,target=target,scope=scope,catalog_sha256=DIGEST,
        requirements=json.array(),errors=json.array(),warnings=json.array(),not_proven=json.array(),
        analysis_scope="declared_and_catalogued_static_calls"}
    local function input_error(reason)
        report.passed=false
        report.errors[#report.errors+1]={decision="deny",origin="validation",reason=reason,
            capability={target=target,scope=scope,proven=false,reason=reason},location={}}
    end
    if not valid_policy(policy) then input_error("invalid_policy");return report end
    local valid,reason=M.validate_profile(profile)
    if not valid then input_error(reason);return report end
    if calls==nil then calls={} end
    if unproven==nil then unproven={} end
    local call_count,unproven_count=length(calls),length(unproven)
    if not call_count or not unproven_count then input_error("invalid_analysis_list");return report end
    for index=1,unproven_count do
        local input=raw_get(unproven,index)
        local note=location(input)
        local code=type(input)=="table" and raw_get(input,"reason") or input
        if type(code)~="string" or #code>64 or not code:match("^[a-z][a-z0-9_]*$") then code="dynamic_path_unproven" end
        note.reason=code;report.not_proven[#report.not_proven+1]=note
    end
    local seen={}
    for index=1,call_count do
        local call=raw_get(calls,index)
        if type(call)~="table" then input_error("invalid_static_call");return report end
        local feature=raw_get(call,"feature")
        if known(feature) then seen[feature]=true end
    end
    local function add(feature,where,origin)
        local result=M.decide(policy,feature,profile,where)
        result.feature=result.capability.feature;result.origin=origin
        result.reason=result.capability.reason
        if result.capability.status=="approximate" and result.decision~="allow" then result.reason="approximation_not_accepted" end
        report.requirements[#report.requirements+1]=result
        if result.decision=="deny" then report.errors[#report.errors+1]=result;report.passed=false
        elseif result.decision=="skip" or result.capability.status=="approximate" then report.warnings[#report.warnings+1]=result end
    end
    local declared={}
    for feature in raw_next,raw_get(policy,"required") do if not seen[feature] then declared[#declared+1]=feature end end
    table.sort(declared)
    for _,feature in ipairs(declared) do add(feature,nil,"declared_required") end
    for index=1,call_count do
        local call=raw_get(calls,index)
        add(raw_get(call,"feature"),call,"static_call")
    end
    return report
end

local POSTFX={bloom="render.postfx.bloom",vignette="render.postfx.vignette",
    lut="render.postfx.lut",softblur="render.postfx.softblur"}
local FFMPEG_EXT={mp4=true,m4v=true,mkv=true,mov=true,avi=true,webm=true,flv=true,
    wmv=true,ogv=true,["3gp"]=true,ts=true,m2ts=true}
local function dynamic(value)
    return type(value)=="string" and (value:find("$",1,true) or value:find("%",1,true) or value:match("^%s*&"))~=nil
end
local function file_parameter(params)
    local file=raw_get(params,"storage") or raw_get(params,"path") or raw_get(params,"file")
    if type(file)~="string" and type(raw_get(params,1))=="string" then file=raw_get(params,1) end
    return file
end

function M.command_features(command,params,staticMode)
    local features,reasons=json.array(),json.array()
    local have,noted={},{}
    local function add(feature) if not have[feature] then have[feature]=true;features[#features+1]=feature end end
    local function note(reason) if not noted[reason] then noted[reason]=true;reasons[#reasons+1]=reason end end
    if params==nil then params={} end
    if type(command)~="string" or type(params)~="table" or raw_equal(params,json.null) then
        add(UNKNOWN);return features,reasons
    end
    if staticMode then
        for _,value in raw_next,params do
            if type(value)=="string" and value:find("${",1,true) then note("dynamic_interpolation") end
        end
    end
    local function selector(value)
        if staticMode and dynamic(value) then note("dynamic_selector");return true end
        return false
    end
    local function fade(value)
        if selector(value) then return end
        if (type(value)=="number" or type(value)=="string") and (tonumber(value) or 0)>0 then add("audio.fade") end
    end
    local function postfx(kind)
        if selector(kind) then return end
        if kind=="none" or kind=="off" or kind=="" then return end
        add(POSTFX[kind] or UNKNOWN)
    end
    local function particle(action,default)
        action=action or default
        if selector(action) then return end
        if action=="create" or action=="start" or action=="emit" then add("render.particles")
        elseif action~="clear" and action~="destroy" and action~="stop" then add(UNKNOWN) end
    end
    if command=="eval" or command=="iscript" or command=="emb" then note("dynamic_lua")
    elseif command=="postprocess_off" or command=="stopvideo" then -- teardown is always permitted
    elseif command=="postprocess" then postfx(raw_get(params,"effect") or "bloom")
    elseif command=="vfx" then
        local fx=raw_get(params,"postfx")
        if fx and fx~="" then postfx(fx)
        else
            local kind=raw_get(params,"type") or raw_get(params,"effect") or "particle"
            if not selector(kind) then
                if kind=="particle" then particle(raw_get(params,"action"),"create")
                elseif kind=="blur" then add("render.blur")
                elseif kind~="quake" and kind~="shake" and kind~="flash" and kind~="fade" and kind~="stop" then add(UNKNOWN) end
            end
        end
    elseif command=="particles" then particle(raw_get(params,"action"),"create")
    elseif command=="particle_weather" then
        local action=raw_get(params,"action") or "start"
        if not (action=="start" and raw_get(params,"type")=="all") then particle(action,"start") end
    elseif command=="palette" then
        local effect=raw_get(params,"effect") or "apply"
        if not selector(effect) then
            if effect=="night" or effect=="toggle" then add("render.postfx.lut3d")
            elseif effect=="apply" then
                local id,path=raw_get(params,"id"),raw_get(params,"path")
                if (type(id)=="string" and #id>0) or (type(path)=="string" and #path>0) then add("render.postfx.lut3d") end
            elseif effect~="clear" and effect~="day" and effect~="unload" then add(UNKNOWN) end
        end
    elseif command=="video" then
        add("video.play")
        local file=file_parameter(params)
        if staticMode and dynamic(file) then note("dynamic_video_format")
        elseif type(file)~="string" then note("video_format_unproven")
        else
            local extension=file:match("%.([a-zA-Z0-9]+)$")
            extension=extension and extension:lower()
            if extension~="mpg" and extension~="mpeg" then
                if FFMPEG_EXT[extension] then add("video.ffmpeg") else note("video_format_unproven") end
            end
        end
    elseif command=="xfadebgm" then add("audio.crossfade")
    elseif command=="fadebgm" or command=="fadevol" then add("audio.fade")
    elseif command=="stopbgm" or command=="playstop" then
        -- Both schemas supply fadeout=0. It shadows time= in the real handler.
        fade(raw_get(params,"fadeout"))
    elseif command=="playbgmstop" then
        local file=file_parameter(params)
        if file then add("audio.play") end
        fade(raw_get(params,"fadeout"))
        if file then fade(raw_get(params,"fadein")) end
    elseif command=="play" or command=="bgm" then
        local bus=raw_get(params,"bus") or "bgm"
        if not selector(bus) then
            if bus=="bgm" or bus=="se" or bus=="voice" then add("audio.play") else add(UNKNOWN) end
        end
        -- The alias forwards file/storage/volume, not fadein.
    elseif command=="playbgm" then add("audio.play");fade(raw_get(params,"fadein"))
    elseif command=="playse" or command=="playvoice" or command=="se" or command=="voice" then add("audio.play")
    elseif command=="live2d_motion" or command=="live2d_expression" or command=="live2d_lip_sync" then add("kag."..command)
    elseif command=="steam_achievement" then add("steam.achievements") end
    return features,reasons
end
return M
