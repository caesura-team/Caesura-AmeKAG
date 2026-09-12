-- Host-backed runtime policy. Capture the provider, never its availability facts.
local json=require("capability_json")
local target=require("target_capabilities")
local raw_get,raw_set,raw_equal,raw_next=rawget,rawset,rawequal,next
local engine=raw_get(_G,"Engine")
local provider=type(engine)=="table" and raw_get(engine,"get_capability_profile") or nil
local host="native"
if type(provider)~="function" then
    provider=raw_get(_G,"__CAESURA_CAPABILITY_PROFILE_JSON");host="web"
end
if type(provider)~="function" then provider=nil;host="unknown" end
local project={required={},optional={},accept_approximate={}}
local M={}
local REASONS={supported=true,css_fixed_grade=true,backend_not_implemented=true,
    command_not_wired=true,backend_route_not_wired=true,sdk_disabled=true,
    backend_unavailable=true,unknown_feature=true,no_host=true,capability_provider_failed=true,
    capability_profile_invalid=true,resource_failed=true,approximation_not_accepted=true,
    invalid_decision=true,capability_failed=true}
local STATUSES={applied=true,approximate=true,unsupported=true,failed=true}
local CAP_STATUSES={supported=true,approximate=true,unsupported=true}
local function table_value(value) return type(value)=="table" and not raw_equal(value,json.null) end
local function field(value,key) return table_value(value) and raw_get(value,key) or nil end
local function feature_name(value) return target.query(value,nil).feature end
local function reason_name(value) return type(value)=="string" and REASONS[value] and value or "capability_failed" end
local function target_name(value) return (value=="native" or value=="web") and value or "unknown" end
local function scene_name(value)
    if type(value)~="string" or #value>256 or value:find("[%c?#]") or value:find("://",1,true) or not json.encode(value) then return nil end
    return value
end
local function command_name(value)
    if type(value)=="string" and #value<=64 and value:match("^[a-zA-Z_][a-zA-Z0-9_.-]*$") then return value end
end
local function copy_location(value)
    local where={scene=scene_name(field(value,"scene")),command=command_name(field(value,"command"))}
    local line=field(value,"line")
    if type(line)=="number" and line>=0 and line<math.huge and line%1==0 then where.line=line end
    return where
end
local function context_location(ctx,command)
    return copy_location({scene=field(ctx,"current_scene") or field(ctx,"currentScene"),
        line=field(ctx,"token_index"),command=command})
end
local function copy_capability(value)
    local status=field(value,"status")
    return {feature=feature_name(field(value,"feature")),target=target_name(field(value,"target")),
        scope=field(value,"scope")=="runtime" and "runtime" or "unknown",proven=field(value,"proven")==true,
        status=CAP_STATUSES[status] and status or nil,reason=reason_name(field(value,"reason"))}
end
local function read_profile()
    if not provider then return nil,"no_host" end
    local ok,profile=pcall(provider)
    if not ok then return nil,"capability_provider_failed" end
    if host=="web" then
        profile=json.decode(profile)
        if not profile then return nil,"capability_profile_invalid" end
    end
    if not target.validate_profile(profile) or field(profile,"scope")~="runtime" or field(profile,"target")~=host then
        return nil,"capability_profile_invalid"
    end
    return profile
end
local function query_profile(feature,profile,failure)
    local result=target.query(feature,profile)
    if failure then
        result.target=host;result.scope="runtime";result.proven=false;result.status=nil;result.reason=failure
    end
    return result
end
local function decide_profile(feature,profile,failure,where)
    local decision=target.decide(project,feature,profile,where)
    if failure then decision.capability=query_profile(feature,nil,failure);decision.decision="deny" end
    return decision
end
function M.has_host() return provider~=nil end
function M.query(feature)
    local profile,failure=read_profile()
    return query_profile(feature,profile,failure)
end
function M.configure_project_json(text)
    local document=json.decode(text)
    if not document then return nil,"invalid_project_json" end
    local candidate=target.validate_project(document)
    if not candidate then return nil,"invalid_project_declaration" end
    if provider then
        local profile,failure=read_profile()
        if not profile then return nil,failure end
        local report=target.check(candidate,profile,{}, {})
        if not report.passed then
            return nil,reason_name(field(report.errors[1],"reason")),M.finish(report.errors[1],false,nil)
        end
    end
    project=candidate
    return true
end
function M.configure_project_file(path,allow_missing)
    if type(path)~="string" or path=="" then return nil,"invalid_project_path" end
    -- Consult the active sandbox boundary, including a replacement IO proxy.
    local found,open=pcall(function()
        local current=raw_get(_G,"io")
        return current and current.open
    end)
    if not found or type(open)~="function" then return nil,"project_io_unavailable" end
    local opened,file,_,errno=pcall(open,path,"rb")
    if not opened then return nil,"project_open_failed" end
    if not file then
        if errno==2 and allow_missing==true then return M.configure_project_json("{}") end
        return nil,"project_open_failed"
    end
    local read_ok,contents=pcall(function()return file:read(json.MAX_BYTES+1)end)
    local close_ok,closed=pcall(function()return file:close()end)
    if not read_ok or type(contents)~="string" then return nil,"project_read_failed" end
    if not close_ok or closed~=true then return nil,"project_close_failed" end
    return M.configure_project_json(contents)
end
function M.evaluate(feature,ctx,command)
    local profile,failure=read_profile()
    return decide_profile(feature,profile,failure,context_location(ctx,command))
end
function M.guard_command(ctx,command,params)
    local features=target.command_features(command,params,false)
    if #features==0 or not provider then return "allow",nil end
    local profile,failure=read_profile()
    local first_allow,first_skip,first_deny
    local where=context_location(ctx,command)
    for _,feature in ipairs(features) do
        local decision=decide_profile(feature,profile,failure,where)
        if decision.decision=="deny" then first_deny=first_deny or decision
        elseif decision.decision=="skip" then first_skip=first_skip or decision
        else first_allow=first_allow or decision end
    end
    local decision=first_deny or first_skip or first_allow
    return decision.decision,decision
end
local record
function M.finish(decision,applied,ctx)
    local capability=copy_capability(field(decision,"capability"))
    local kind=field(decision,"decision")
    local result={status="failed",feature=capability.feature,target=capability.target,
        reason="invalid_decision",capability=capability,location=copy_location(field(decision,"location"))}
    if kind=="deny" or kind=="skip" then
        result.status="unsupported";result.reason=capability.reason
        if capability.status=="approximate" then result.reason="approximation_not_accepted" end
    elseif kind=="allow" and capability.proven and (capability.status=="supported" or capability.status=="approximate") then
        result.reason="resource_failed"
        if applied==true then
            result.status=capability.status=="approximate" and "approximate" or "applied"
            result.reason=capability.reason
        end
    end
    if table_value(ctx) then record(ctx,result) end
    return result
end
local function copy_result(value)
    local status=field(value,"status")
    return {status=STATUSES[status] and status or "failed",feature=feature_name(field(value,"feature")),
        target=target_name(field(value,"target")),reason=reason_name(field(value,"reason")),
        capability=copy_capability(field(value,"capability")),location=copy_location(field(value,"location"))}
end
function M.message(value)
    local result=copy_result(value)
    local where=result.location
    return string.format("[capability] status=%s target=%s feature=%s scene=%s token=%s command=%s reason=%s",
        result.status,result.target,result.feature,where.scene or "unknown",tostring(where.line or "unknown"),
        where.command or "unknown",result.reason)
end
local states=setmetatable({}, {__mode="k"})
local function publish(ctx,state)
    local entries={}
    for index,value in ipairs(state.entries) do entries[index]=copy_result(value) end
    raw_set(ctx,"capability_diagnostics",entries)
    raw_set(ctx,"capability_diagnostics_dropped",state.dropped)
end
record=function(ctx,value)
    if not table_value(ctx) then return false end
    local result=copy_result(value)
    if result.status=="applied" then return false end
    local state=states[ctx]
    if not state then state={entries={},seen={},dropped=0};states[ctx]=state end
    local key=assert(json.encode(json.array({result.status,result.target,result.feature,result.reason,
        result.location.scene or "",result.location.line or -1,result.location.command or ""})))
    if state.seen[key] then publish(ctx,state);return false end
    if #state.entries>=64 then
        state.dropped=state.dropped+1;publish(ctx,state);return false
    end
    state.seen[key]=true;state.entries[#state.entries+1]=result
    publish(ctx,state)
    local print_current=raw_get(_G,"print")
    if type(print_current)=="function" then pcall(print_current,M.message(result)) end
    return true
end
M.record=record

-- The public KAG table and scheduler share this boundary. Direct calls from
-- [iscript] must make the same decision before a handler changes scene state.
local guarded_handlers=setmetatable({}, {__mode="k"})
local command_wrappers=setmetatable({}, {__mode="kv"})
local function invoke(handler,ctx,params,command,...)
    local decision,detail=M.guard_command(ctx,command,params)
    if decision~="allow" then
        local result=M.finish(detail,false,ctx)
        if (command=="stopbgm" or command=="playstop")
            and field(field(detail,"capability"),"feature")=="audio.fade" then
            -- A rejected optional effect must not retain a playback owner.
            -- Preserve the caller's parameters and run only immediate cleanup.
            local cleanup={}
            if type(params)=="table" then for key,value in raw_next,params do cleanup[key]=value end end
            cleanup.fadeout=0;cleanup.time=0
            handler(ctx,cleanup,...)
        end
        if decision=="deny" then error(M.message(result),0) end
        return false,result
    end
    return handler(ctx,params,...)
end
function M.wrap_command(handler,command)
    if not target.command_has_capabilities(command) then return handler end
    local existing=command_wrappers[handler]
    if existing then
        local info=guarded_handlers[existing]
        if command<info.command then info.command=command end
        return existing
    end
    local info={handler=handler,command=command}
    local wrapped=function(ctx,params,...) return invoke(handler,ctx,params,info.command,...) end
    guarded_handlers[wrapped]=info
    command_wrappers[handler]=wrapped
    return wrapped
end
function M.invoke_command(handler,ctx,params,command)
    local info=guarded_handlers[handler]
    if info then handler=info.handler end
    return invoke(handler,ctx,params,command)
end
return M
