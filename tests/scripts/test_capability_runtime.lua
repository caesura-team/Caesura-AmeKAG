-- Isolated adapter test: actual policy/codec, only host-provider and IO boundaries replaced.
local script=(arg and arg[0] or ""):gsub("\\","/")
local root=(arg and arg[1]) or script:match("^(.*)/tests/scripts/[^/]+$") or "."
package.path=root.."/scripts/?.lua";package.cpath=""
local passed,failed=0,0
local function check(name,condition)
    if condition then passed=passed+1 else failed=failed+1;print("[FAIL] "..name) end
end
local chunk=loadfile(root.."/scripts/capability_runtime.lua")
check("runtime adapter module exists",type(chunk)=="function")
if not chunk then
    -- Native corpus proved standalone blur bypassed the guard while vfx blur did not.
-- Exercise the actual adapter; the effect sink records whether a denial leaked.
do
    local facts=profile("native")
    local runtime=instance("native",function()return facts end)
    local calls=0
    local sink=function()calls=calls+1;return true end
    check("standalone blur optional policy commits",runtime.configure_project_json('{"capabilities":{"optional":["render.blur"]}}')==true)
    local owner={current_scene="blur.ks",token_index=1}
    local applied,result=runtime.invoke_command(sink,owner,{amount=4,time=32},"blur")
    check("standalone blur optional refusal has exact diagnostic and no effect",applied==false and calls==0
        and result.status=="unsupported" and result.feature=="render.blur" and result.reason=="command_not_wired"
        and #owner.capability_diagnostics==1 and owner.capability_diagnostics[1].location.command=="blur")
    check("standalone blur ordinary policy commits",runtime.configure_project_json('{}')==true)
    owner={current_scene="blur.ks",token_index=2}
    local ok=pcall(runtime.wrap_command(sink,"blur"),owner,{amount=4})
    check("standalone blur required-use rejection cannot reach effect",not ok and calls==0
        and owner.capability_diagnostics[1].status=="unsupported")
end

print(string.format("Capability Runtime Tests: %d passed, %d failed",passed,failed));os.exit(1)
end
local json=require("capability_json")
local policy=require("target_capabilities")
local data=assert(json.decode(require("capability_catalog").json))
local function profile(target)
    local p={schema=1,target=target,scope="runtime",platform=target=="native" and "windows" or "browser",
        catalog_sha256=policy.catalog_sha256,compiled=target=="native" and {ffmpeg=false,live2d=false,steam=false} or {},available={}}
    for _,feature in pairs(data.features) do
        if feature[target].available then p.available[feature[target].available]=true end
    end
    return p
end
local function instance(kind,provider)
    local old_engine,old_web=rawget(_G,"Engine"),rawget(_G,"__CAESURA_CAPABILITY_PROFILE_JSON")
    rawset(_G,"Engine",kind=="native" and {get_capability_profile=provider} or nil)
    rawset(_G,"__CAESURA_CAPABILITY_PROFILE_JSON",kind=="web" and provider or nil)
    local ok,value=pcall(assert(loadfile(root.."/scripts/capability_runtime.lua")))
    rawset(_G,"Engine",old_engine);rawset(_G,"__CAESURA_CAPABILITY_PROFILE_JSON",old_web)
    check("isolated module instance loads",ok and type(value)=="table")
    return value
end
local native_facts=profile("native")
local native_calls=0
local native=instance("native",function()native_calls=native_calls+1;return native_facts end)
check("provider is captured without querying at load",native_calls==0 and native.has_host())
local status,decision=native.guard_command({},"stopvideo",{})
check("cleanup has no capability query",status=="allow" and decision==nil and native_calls==0)
native.guard_command({},"iscript",{})
check("dynamic command itself does not query a capability",native_calls==0)
check("native base video available",native.query("video.play").status=="supported")
check("native SDK off stays unsupported",native.query("video.ffmpeg").reason=="sdk_disabled")
for fact in pairs(native_facts.available) do native_facts.available[fact]=false end
check("null or unavailable native provider facts are current",native.query("video.play").reason=="backend_unavailable")
native_facts.available.video=true
check("provider is queried fresh after availability changes",native.query("video.play").status=="supported")
local changed=native.query("video.play");changed.status="unsupported";changed.reason="tampered"
check("returned query mutation does not change facts",native.query("video.play").status=="supported")
local replacement_calls=0
local old_engine=rawget(_G,"Engine")
rawset(_G,"Engine",{get_capability_profile=function()replacement_calls=replacement_calls+1;return nil end})
check("later Engine replacement cannot replace captured provider",native.query("video.play").status=="supported" and replacement_calls==0)
rawset(_G,"Engine",old_engine)
local ctx={current_scene="chapter/story.ks",token_index=7}
local evaluated=native.evaluate("video.play",ctx,"video")
check("evaluate has no effect and captures token location",evaluated.decision=="allow" and evaluated.location.scene=="chapter/story.ks" and evaluated.location.line==7 and evaluated.location.command=="video")
ctx.token_index=8
local exact=native.finish(evaluated,true)
check("explicit successful exact result is applied",exact.status=="applied" and exact.location.line==7)
exact.capability.status="tampered";exact.location.scene="changed"
check("finish result is independent of decision",evaluated.capability.status=="supported" and evaluated.location.scene=="chapter/story.ks")
for _,applied in ipairs({false,0,1,"ok",{handle=3}}) do
    local result=native.finish(evaluated,applied)
    check("non-boolean success is not inferred",result.status=="failed" and result.reason=="resource_failed")
end
check("nil is not inferred as success",native.finish(evaluated,nil).status=="failed")
check("missing decision cannot be called applied",native.finish(nil,true).status~="applied")

check("optional SDK policy can commit",native.configure_project_json('{"capabilities":{"optional":["video.ffmpeg"]}}')==true)
check("optional SDK operation skips",native.evaluate("video.ffmpeg",ctx,"video").decision=="skip")
local configured,reason=native.configure_project_json('{"capabilities":{"required":["video.ffmpeg"]}}')
check("unavailable required SDK rejects configuration",configured==nil and type(reason)=="string")
check("failed configuration preserves previous policy",native.evaluate("video.ffmpeg",ctx,"video").decision=="skip")
configured,reason=native.configure_project_json('{"capabilities":{"required":["PRIVATE_UNKNOWN_FEATURE"]}}')
check("invalid declaration is rejected without a payload echo",configured==nil and type(reason)=="string" and not reason:find("PRIVATE_UNKNOWN_FEATURE",1,true))
check("bad declaration preserves policy",native.evaluate("video.ffmpeg",ctx,"video").decision=="skip")
configured,reason=native.configure_project_json('{"capabilities": PRIVATE_JSON_BODY}')
check("bad JSON is redacted",configured==nil and type(reason)=="string" and not reason:find("PRIVATE_JSON_BODY",1,true))
check("bad JSON preserves policy",native.evaluate("video.ffmpeg",ctx,"video").decision=="skip")
native_facts.scope="build"
check("runtime adapter rejects build-only proof",native.query("video.play").proven==false and native.query("video.play").status==nil)
native_facts.scope="runtime"

local web_facts=profile("web")
local web_calls=0
local provider_failed=false
local web=instance("web",function()
    web_calls=web_calls+1
    if provider_failed then error("PRIVATE_PROVIDER_BODY") end
    return assert(json.encode(web_facts))
end)
local other_web=instance("web",function()return assert(json.encode(web_facts))end)
check("default Web video denies",web.evaluate("video.play",ctx,"video").decision=="deny")
check("valid Web policy commits",web.configure_project_json('{"capabilities":{"optional":["video.play"],"required":["render.postfx.lut3d"],"accept_approximate":["render.postfx.lut3d"]}}')==true)
check("separate instances do not share project policy",web.evaluate("video.play",ctx,"video").decision=="skip" and other_web.evaluate("video.play",ctx,"video").decision=="deny")
evaluated=web.evaluate("render.postfx.lut3d",ctx,"palette")
check("accepted approximation allows the operation",evaluated.decision=="allow" and evaluated.capability.status=="approximate")
local approximate=web.finish(evaluated,true)
check("successful approximation is not exact applied",approximate.status=="approximate" and approximate.reason=="css_fixed_grade")
configured,reason=web.configure_project_json('{"capabilities":{"required":["render.postfx.lut3d"]}}')
check("unaccepted required approximation rejects policy",configured==nil and type(reason)=="string")
check("approximation rejection preserves accepted policy",web.evaluate("render.postfx.lut3d",ctx,"palette").decision=="allow")
local before=web_calls
status,decision=web.guard_command(ctx,"video",{file="opening.mp4"})
check("required denial outweighs an earlier optional skip",status=="deny" and decision.capability.feature=="video.ffmpeg")
check("one command uses one provider snapshot",web_calls==before+1)
check("all-optional policy commits",web.configure_project_json('{"capabilities":{"optional":["video.play","video.ffmpeg"]}}')==true)
status,decision=web.guard_command(ctx,"video",{file="opening.mp4"})
check("all optional command skips with first relevant decision",status=="skip" and decision.capability.feature=="video.play")
check("denied or skipped effect never reports applied",web.finish(decision,true).status=="unsupported")
provider_failed=true
local unavailable=web.query("video.play")
check("provider failure returns unproved data",unavailable.proven==false and unavailable.status==nil)
check("provider failure message has no private body",not unavailable.reason:find("PRIVATE_PROVIDER_BODY",1,true))
configured,reason=web.configure_project_json('{}')
check("provider failure cannot replace policy",configured==nil and type(reason)=="string")
provider_failed=false
check("provider failure retained optional policy",web.evaluate("video.play",ctx,"video").decision=="skip")
local bad_web=instance("web",function()return '{PRIVATE_HOST_JSON_BODY}'end)
unavailable=bad_web.query("audio.play")
check("invalid host JSON is unproved and redacted",unavailable.proven==false and unavailable.status==nil and not unavailable.reason:find("PRIVATE_HOST_JSON_BODY",1,true))
local wrong_web=instance("web",function()return assert(json.encode(profile("native")))end)
check("provider target mismatch is not runtime proof",wrong_web.query("video.play").proven==false)

local no_host=instance(nil,nil)
check("legacy component has no captured host",not no_host.has_host())
check("legacy query makes no supported claim",no_host.query("video.play").proven==false and no_host.query("video.play").status==nil)
status,decision=no_host.guard_command(ctx,"video",{file="opening.mp4"})
check("legacy command remains runnable without fabricated decision",status=="allow" and decision==nil)
check("legacy evaluate remains unproved",no_host.evaluate("video.play",ctx,"video").capability.proven==false)
check("legacy declarations can be retained without host proof",no_host.configure_project_json('{"capabilities":{"required":["video.play"]}}')==true)
rawset(_G,"Engine",{get_capability_profile=function()return profile("native")end})
check("provider is not acquired after module load",not no_host.has_host() and no_host.query("video.play").proven==false)
rawset(_G,"Engine",old_engine)

-- Read through CURRENT IO boundary; do not retain the pre-sandbox io.open.
local original_open=io.open
local read_limit,closed,opened=0,0,0
io.open=function(path,mode)
    opened=opened+1;check("configuration uses read-only binary IO",path=="project.json" and mode=="rb")
    return {read=function(_,limit)read_limit=limit;return '{"capabilities":{"optional":["video.play"]}}'end,
        close=function()closed=closed+1;return true end}
end
configured,reason=other_web.configure_project_file("project.json",false)
io.open=original_open
check("current IO replacement is used",configured==true and opened==1 and closed==1 and read_limit==json.MAX_BYTES+1)
check("file policy actually commits",other_web.evaluate("video.play",ctx,"video").decision=="skip")
local original_io=rawget(_G,"io")
local proxy_lookups=0
rawset(_G,"io",setmetatable({}, {__index=function(_,key)
    if key=="open" then
        proxy_lookups=proxy_lookups+1
        return function()return {read=function()return '{}'end,close=function()return true end}end
    end
end}))
configured,reason=other_web.configure_project_file("project.json",false)
rawset(_G,"io",original_io)
check("whole sandbox IO proxy replacement is consulted",configured==true and proxy_lookups==1)
check("proxy policy actually commits",other_web.evaluate("video.play",ctx,"video").decision=="deny")
check("restore optional declaration after proxy test",other_web.configure_project_json('{"capabilities":{"optional":["video.play"]}}')==true)
io.open=function()return nil,"PRIVATE_ACCESS_DENIED",13 end
configured,reason=other_web.configure_project_file("project.json",true)
io.open=original_open
check("access denied is not missing legacy metadata",configured==nil and type(reason)=="string" and not reason:find("PRIVATE_ACCESS_DENIED",1,true))
check("read failure leaves old policy",other_web.evaluate("video.play",ctx,"video").decision=="skip")
local failed_closes=0
io.open=function()return {read=function()error("PRIVATE_READ_ERROR")end,close=function()failed_closes=failed_closes+1;return true end}end
configured,reason=other_web.configure_project_file("project.json",false)
io.open=original_open
check("read exception still closes the file",configured==nil and failed_closes==1 and not reason:find("PRIVATE_READ_ERROR",1,true))
io.open=function()return {read=function()return '{}'end,close=function()return nil,"PRIVATE_CLOSE_ERROR"end}end
configured,reason=other_web.configure_project_file("project.json",false)
io.open=original_open
check("close failure prevents commit",configured==nil and not reason:find("PRIVATE_CLOSE_ERROR",1,true) and other_web.evaluate("video.play",ctx,"video").decision=="skip")
io.open=function()return {read=function()return string.rep(" ",json.MAX_BYTES+1)end,close=function()return true end}end
configured,reason=other_web.configure_project_file("project.json",false)
io.open=original_open
check("oversize metadata cannot commit",configured==nil and other_web.evaluate("video.play",ctx,"video").decision=="skip")
io.open=function()return nil,"not found",2 end
configured,reason=other_web.configure_project_file("missing.json",false)
io.open=original_open
check("missing metadata needs explicit permission",configured==nil and other_web.evaluate("video.play",ctx,"video").decision=="skip")
io.open=function()return nil,"not found",2 end
configured,reason=other_web.configure_project_file("missing.json",true)
io.open=original_open
check("explicitly allowed missing metadata commits legacy policy",configured==true and other_web.evaluate("video.play",ctx,"video").decision=="deny")
io.open=nil
configured,reason=other_web.configure_project_file("missing.json",true)
io.open=original_open
check("unavailable sandbox IO is not treated as missing metadata",configured==nil and type(reason)=="string")

local lines={}
local original_print=print
print=function(line)lines[#lines+1]=line end
local record_ctx={current_scene="diagnostics.ks",token_index=1}
local denied=other_web.evaluate("video.play",record_ctx,"video")
local result=other_web.finish(denied,false,record_ctx)
other_web.record(record_ctx,result)
local recorded_once=#lines==1 and #record_ctx.capability_diagnostics==1
local plain=native.evaluate("video.play",record_ctx,"video")
native.finish(plain,true,record_ctx)
local exact_silent=#lines==1
for index=2,70 do
    record_ctx.token_index=index
    other_web.finish(other_web.evaluate("video.play",record_ctx,"video"),false,record_ctx)
end
print=original_print
check("finish records once and explicit record deduplicates",recorded_once)
check("normal exact applied has no warning",exact_silent)
check("diagnostics are capped at 64",#record_ctx.capability_diagnostics==64 and record_ctx.capability_diagnostics_dropped==6 and #lines==64)
local isolated_ctx={current_scene="diagnostics.ks",token_index=1}
print=function()end
other_web.finish(other_web.evaluate("video.play",isolated_ctx,"video"),false,isolated_ctx)
print=original_print
check("diagnostic state is scoped to context",#isolated_ctx.capability_diagnostics==1 and isolated_ctx.capability_diagnostics_dropped==0)
result.feature="PRIVATE_RESOURCE_TOKEN";result.reason="PRIVATE_ERROR_BODY";result.location.scene="https://private.test/?token=PRIVATE_URL"
local message=other_web.message(result)
check("diagnostic does not echo a resource URL or unknown payload",not message:find("PRIVATE_",1,true) and not message:find("https://",1,true))
check("recorded result did not alias caller mutation",record_ctx.capability_diagnostics[1].feature=="video.play" and record_ctx.capability_diagnostics[1].location.scene=="diagnostics.ks")

-- U26 policy integration unit: real schema/runtime/character handler/backend.lua;
-- only the host profile and native Live2D boundary are controlled. No SDK claim.
local lip_facts=profile("native");lip_facts.compiled.live2d=true
lip_facts.available.cubism=true;lip_facts.available.audio=false
local lip_profile_reads=0
local lip_runtime=instance("native",function()lip_profile_reads=lip_profile_reads+1;return lip_facts end)
check("Live2D unit uses actual runtime policy",lip_runtime.configure_project_json("{}")==true)
local actual_character=require("kag.commands.character")
local actual_schema=require("kag.schema")
local actual_tokenizer=require("tokenizer")
local previous_live2d=rawget(_G,"Live2D")
local boundary_calls={}
local function boundary(name,...)
    boundary_calls[#boundary_calls+1]={name,...}
    return name=="load" and 41 or true
end
rawset(_G,"Live2D",{
    load=function(...)return boundary("load",...)end,
    show=function(...)return boundary("show",...)end,
    hide=function(...)return boundary("hide",...)end,
    unload=function(...)return boundary("unload",...)end,
    set_mouth=function(...)return boundary("set_mouth",...)end,
    set_voice_lipsync=function(...)return boundary("set_voice_lipsync",...)end,
})
local function lip_context(command)
    return {current_scene="capability-live2d.ks",token_index=1,f={},sf={},tf={},
        _live2dHandles=command=="live2d_load" and {} or {haru=41},live2d={}}
end
local function dispatch_live2d(command,raw,public_wrapper)
    local ctx=lip_context(command)
    local coerced=actual_schema.coerce(command,raw,ctx)
    local called,value
    if public_wrapper then
        called,value=pcall(lip_runtime.wrap_command(actual_character[command],command),ctx,coerced)
    else
        called,value=pcall(lip_runtime.invoke_command,actual_character[command],ctx,coerced,command)
    end
    return called,value,ctx
end
local function denied_as(ctx,feature,reason)
    local entries=ctx.capability_diagnostics
    return type(entries)=="table" and #entries==1 and entries[1].status=="unsupported"
        and entries[1].feature==feature and entries[1].reason==reason
end
for _,command in ipairs({"live2d_load","live2d_show","live2d_hide","live2d_unload"}) do
    local raw={model="haru"};if command=="live2d_load" then raw.storage="Haru.model3.json" end
    lip_facts.available.cubism=true;local before=#boundary_calls
    local ok=dispatch_live2d(command,raw,true)
    check(command.." reaches real handler/native boundary with Cubism and no audio",ok and #boundary_calls==before+1)
    lip_facts.available.cubism=false;before=#boundary_calls
    local blocked,_,blocked_ctx=dispatch_live2d(command,raw,true)
    check(command.." public wrapper denies unavailable Cubism before native effect",not blocked and #boundary_calls==before
        and denied_as(blocked_ctx,"live2d.cubism","backend_unavailable"))
    lip_facts.available.cubism=true;lip_facts.compiled.live2d=false;before=#boundary_calls
    blocked,_,blocked_ctx=dispatch_live2d(command,raw,false)
    check(command.." scheduler invocation denies SDK-off before native effect",not blocked and #boundary_calls==before
        and denied_as(blocked_ctx,"live2d.cubism","sdk_disabled"))
    lip_facts.compiled.live2d=true
end
for _,source in ipairs({"manual","off"}) do
    local before=#boundary_calls;local ok=dispatch_live2d("live2d_lip_sync",{model="haru",source=source},false)
    local last=boundary_calls[#boundary_calls]
    check(source.." remains usable with NullAudio-equivalent availability",ok and #boundary_calls==before+1
        and last[1]==(source=="manual" and "set_mouth" or "set_voice_lipsync") and last[3]==(source=="manual" and 0 or false))
end
local parsed=actual_tokenizer.parse('[live2d_lip_sync model=haru value=0.4]')
check("real tokenizer retains default-source tag",parsed[1] and parsed[1].type=="command" and parsed[1].cmd=="live2d_lip_sync" and #parsed[1].params==2)
local tag_params={}
for _,pair in ipairs(parsed[1].params or {}) do tag_params[pair[1]]=pair[2] end
local before=#boundary_calls
local ok=dispatch_live2d("live2d_lip_sync",tag_params,false)
check("real schema default manual reaches native mouth despite unavailable audio",ok and #boundary_calls==before+1
    and boundary_calls[#boundary_calls][1]=="set_mouth" and boundary_calls[#boundary_calls][3]==.4)
before=#boundary_calls
local blocked,_,blocked_ctx=dispatch_live2d("live2d_lip_sync",{model="haru",source="voice"},false)
check("Cubism plus NullAudio-equivalent facts denies voice specifically on audio.play",not blocked and #boundary_calls==before
    and denied_as(blocked_ctx,"audio.play","backend_unavailable"))
lip_facts.available.audio=true;before=#boundary_calls;local reads=lip_profile_reads
ok=dispatch_live2d("live2d_lip_sync",{model="haru",source="voice"},true)
check("fresh playable-audio fact admits voice through real handler",ok and #boundary_calls==before+1 and lip_profile_reads>reads
    and boundary_calls[#boundary_calls][1]=="set_voice_lipsync" and boundary_calls[#boundary_calls][3]==true)
lip_facts.available.audio=false;before=#boundary_calls
blocked,_,blocked_ctx=dispatch_live2d("live2d_lip_sync",{model="haru",source="voice"},true)
check("losing audio availability is rechecked without a cached allow",not blocked and #boundary_calls==before
    and denied_as(blocked_ctx,"audio.play","backend_unavailable"))
for _,source in ipairs({"manual","voice","off"}) do
    lip_facts.available.audio=true;lip_facts.available.cubism=false;before=#boundary_calls
    blocked,_,blocked_ctx=dispatch_live2d("live2d_lip_sync",{model="haru",source=source},false)
    check(source.." denies unavailable Cubism before native mouth change",not blocked and #boundary_calls==before
        and denied_as(blocked_ctx,"kag.live2d_lip_sync","backend_unavailable"))
end
-- source is an enum, not an interpolated string: do not add new syntax while
-- synchronizing capability policy. Macros must already have expanded to a literal.
for _,unresolved in ipairs({"$tf.mode","${f.mode}","%mode%"}) do
    before=#boundary_calls
    local accepted=pcall(actual_schema.coerce,"live2d_lip_sync",{model="haru",source=unresolved},{f={mode="voice"},tf={mode="voice"}})
    check("unresolved enum source remains a real schema error "..unresolved,not accepted and #boundary_calls==before)
end
rawset(_G,"Live2D",previous_live2d)

print(string.format("Capability Runtime Tests: %d passed, %d failed",passed,failed))
if failed==0 then print("ALL CAPABILITY RUNTIME TESTS PASSED") end
os.exit(failed==0 and 0 or 1)
