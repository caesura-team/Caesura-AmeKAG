-- Prepared real tokenizer/compiler/scheduler regression. Only native host
-- boundaries are recorded; no command implementation or registry is replaced.
package.path="scripts/?.lua;scripts/?/init.lua;"..package.path
local lpeg=require("lpeg")
local passed,failed=0,0
local function check(name,ok,detail)
    if ok then passed=passed+1;print("PASS "..name)
    else failed=failed+1;print("FAIL "..name.." -- "..tostring(detail)) end
end
local function fixture()
    local env=setmetatable({},{__index=_G});env._G=env
    local modules={lpeg=lpeg}
    env.package={loaded=modules,path=package.path,cpath=package.cpath,config=package.config}
    env.Restore={capture_font=function()return {version=1,active=true,font=0,path="",size=16}end,
        prepare_font=function(v)return v end,apply_font=function()return true end,discard_font=function()end}
    local calls={}
    env._CAESURA_BACKEND={
        render=function(method,...)
            calls[#calls+1]={method=method,args={...}}
            if method=="create_solid_texture" or method=="load_texture" then return 41 end
            if method=="create_viewport" then return 501 end
            if method=="line_height" then return 24 end
            if method=="measure_text" then return 20,24 end
            return true
        end,
        platform=function(method,...)
            if method=="get_resolution" then return 1280,720 end
            if method=="get_input_focus" then return "KAG" end
            return true
        end,
        audio=function() return false end,
    }
    env.require=function(name)
        if modules[name]==nil then
            local path="scripts/"..name:gsub("%.","/")..".lua"
            local f=assert(io.open(path,"rb"));local bytes=f:read("*a");f:close()
            modules[name]=assert(load(bytes:gsub("^\239\187\191",""),"@"..path,"t",env))()
        end
        return modules[name]
    end
    local tokenizer=env.require("tokenizer")
    local compiler=env.require("kag.compiler")
    local scheduler=env.require("scheduler")
    local kag=env.require("kag")
    local resource=env.require("kag.commands.resource")
    local function run(name, retained_handler)
        resource._audioCache.contract_owned=true
        local tokens=tokenizer.parse("["..name.."]\n[set f.sentinel 1]\n[end]")
        compiler.compile(tokens)
        if retained_handler then tokens._compiled.handlers[1]=kag[name] end
        local handler=tokens._compiled.handlers[1]
        local ctx={f={},sf={},tf={},mp={},lf={},variables={},tokens=tokens,token_index=1,
            current_scene="internal-dispatch.ks",call_stack={},macro_args={},_session_active=true}
        local co=coroutine.create(function()scheduler.run(ctx,tokens,1)end)
        local err
        for _=1,32 do
            if coroutine.status(co)=="dead" then break end
            local ok,msg=coroutine.resume(co,16)
            if not ok then err=tostring(msg);break end
            if ctx._command_error then err=ctx.error_command;break end
        end
        local ended=coroutine.status(co)=="dead";local closed,cerr=coroutine.close(co)
        return {ctx=ctx,handler=handler,error=err,ended=ended,closed=closed,close_error=cerr,
            cache=resource._audioCache.contract_owned,kag=kag,resource=resource}
    end
    return run,kag,resource
end
local names={"_hideHr","_relocalizeCC","_relocalizeChoices","_renderNameplate","flush_cache","get_texture","has_pending_transition","is_loaded","is_pending","preload_transition","promote_transition_slot","push_backlog","relocalize_backlog","relocalize_page","render","return_to_caller","update","wait_click","Bezier","LUTCache","_audioCache","_pendingAudio","_pendingTextures","_textureCache","_transitionSlot","gesture_defaults"}
for _,name in ipairs(names) do
    local run=fixture();local result=run(name)
    -- Compatibility policy: keep existing unknown-tag warning/text handling,
    -- but never bind an internal helper or a module table as a command.
    check(name.." is not a compiled DSL handler",result.handler==nil,type(result.handler))
    check(name.." follows unknown-tag handling without side effects",
        result.ended and result.closed and not result.error and not result.ctx._command_error
        and result.ctx.f.sentinel==1 and next(result.ctx._warned_cmds or {})~=nil
        and result.cache==true,
        "ended="..tostring(result.ended).." error="..tostring(result.error).." cache="..tostring(result.cache))
end
do
    local run=fixture();local result=run("flush_cache",true)
    check("retained compiled internal helper cannot bypass current DSL policy",
        result.ended and result.closed and not result.error and result.cache==true
        and next(result.ctx._warned_cmds or {})~=nil)
end
do
    local _,kag,resource=fixture()
    resource._audioCache.contract_owned=true
    check("Lua helper API still exported",type(kag.flush_cache)=="function")
    kag.flush_cache()
    check("direct Lua flush_cache retains behavior",resource._audioCache.contract_owned==nil)
end
print(string.format("%d passed, %d failed",passed,failed))
assert(failed==0,"internal dispatch boundary regressions: "..failed)
