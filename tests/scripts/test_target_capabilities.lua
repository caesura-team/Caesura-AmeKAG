-- Standalone policy/resolver test; no Engine, IO-driven policy or global host setup.
local script=(arg and arg[0] or ""):gsub("\\", "/")
local root=(arg and arg[1]) or script:match("^(.*)/tests/scripts/[^/]+$") or "."
package.path=root.."/scripts/?.lua"
package.cpath=""
package.loaded.target_capabilities=nil
local passed,failed=0,0
local function check(name,condition)
    if condition then passed=passed+1 else failed=failed+1;print("[FAIL] "..name) end
end
local loaded,caps=pcall(require,"target_capabilities")
check("resolver module loads",loaded and type(caps)=="table")
if not loaded then
    print(string.format("Target Capability Tests: %d passed, %d failed",passed,failed));os.exit(1)
end
local json=require("capability_json")
local generated=require("capability_catalog")
local catalog=assert(json.decode(generated.json))
local function document(text) return assert(json.decode(text)) end
local function policy(text)
    local result,reason=caps.validate_project(document(text or "{}"))
    check("valid project policy",result~=nil and reason==nil)
    return result
end
local function profile(target,scope)
    local result={schema=1,target=target,scope=scope or "build",
        platform=target=="native" and "windows" or "browser",catalog_sha256=caps.catalog_sha256,
        compiled=target=="native" and {ffmpeg=false,live2d=false,steam=false} or {}}
    if result.scope=="runtime" then
        result.available={}
        for _,feature in pairs(catalog.features) do
            local fact=feature[target].available
            if fact then result.available[fact]=true end
        end
    end
    return result
end
local function invalid_project(value,name)
    local result,reason=caps.validate_project(value)
    check(name,result==nil and type(reason)=="string")
    return reason
end
local function invalid_profile(value,name)
    local result,reason=caps.validate_profile(value)
    check(name,result==nil and type(reason)=="string")
    return reason
end

check("catalog identity matches generated data",caps.catalog_sha256==generated.sha256)
local legacy=policy()
check("legacy policy has empty sets",next(legacy.required)==nil and next(legacy.optional)==nil and next(legacy.accept_approximate)==nil)
local decoded=document('{"name":"game","custom":42,"capabilities":{"required":["video.play"],"optional":["render.postfx.lut3d"],"accept_approximate":["render.postfx.lut3d"]}}')
local authored=assert(caps.validate_project(decoded))
check("declarations become feature sets",authored.required["video.play"]==true and authored.optional["render.postfx.lut3d"]==true and authored.accept_approximate["render.postfx.lut3d"]==true)
decoded.capabilities.required[1]="audio.play"
check("policy does not alias decoded declaration arrays",authored.required["video.play"] and not authored.required["audio.play"])
local independent=policy()
legacy.required["video.play"]=true
check("new policies have independent maps",not independent.required["video.play"])
legacy=policy()
local acceptance=policy('{"capabilities":{"accept_approximate":["render.postfx.lut3d"]}}')
check("approximation acceptance may stand alone",acceptance.accept_approximate["render.postfx.lut3d"] and next(acceptance.required)==nil)
invalid_project({},"untagged project root rejected")
for index,text in ipairs({
    "[]","null","true",'{"capabilities":[]}','{"capabilities":null}',
    '{"capabilities":{"requred":[]}}','{"capabilities":{"required":{}}}',
    '{"capabilities":{"required":null}}','{"capabilities":{"required":[1]}}',
    '{"capabilities":{"required":["video.paly"]}}',
    '{"capabilities":{"required":["video.play","video.play"]}}',
    '{"capabilities":{"optional":["audio.play","audio.play"]}}',
    '{"capabilities":{"accept_approximate":["render.postfx.lut3d","render.postfx.lut3d"]}}',
    '{"capabilities":{"required":["audio.play"],"optional":["audio.play"]}}',
}) do invalid_project(document(text),"bad declaration rejected "..index) end
local sparse=document('{"capabilities":{"required":["video.play"]}}')
sparse.capabilities.required[3]="audio.play"
invalid_project(sparse,"mutated tagged array holes are rejected")
local bad_declaration_reason=invalid_project(document('{"capabilities":{"required":["PRIVATE_BODY_TOKEN"]}}'),"private unknown feature rejected")
check("declaration error is redacted",not bad_declaration_reason:find("PRIVATE_BODY_TOKEN",1,true))

local native=profile("native")
local web=profile("web")
check("native build profile accepted",caps.validate_profile(native)==true)
check("web build profile accepted",caps.validate_profile(web)==true)
local decoded_profile=document(assert(json.encode({schema=1,target="native",scope="build",platform="linux",catalog_sha256=caps.catalog_sha256,compiled={ffmpeg=false,live2d=false,steam=false}})))
check("decoded profile object is accepted",caps.validate_profile(decoded_profile)==true)
for _,platform in ipairs({"windows","linux","macos","ios","android"}) do
    local p=profile("native");p.platform=platform
    check("native platform accepted "..platform,caps.validate_profile(p)==true)
end
for _,field in ipairs({"schema","target","scope","platform","catalog_sha256","compiled"}) do
    local p=profile("native");p[field]=nil
    invalid_profile(p,"missing profile field "..field)
end
local p=profile("native");p.catalog_sha256=string.rep("0",64)
invalid_profile(p,"wrong catalog identity rejected")
p=profile("web");p.platform="windows";invalid_profile(p,"web platform must be browser")
p=profile("native");p.platform="browser";invalid_profile(p,"native platform cannot be browser")
p=profile("native");p.compiled.ffmpeg="false";invalid_profile(p,"compiled facts must be actual booleans")
p=profile("native");p.compiled.live2d=nil;invalid_profile(p,"all native compiled facts are mandatory")
p=profile("native");p.compiled.extra=true;invalid_profile(p,"unknown compiled fact rejected")
p=profile("web");p.compiled.ffmpeg=true;invalid_profile(p,"web compiled map must be empty")
p=profile("web");p.compiled=json.array();invalid_profile(p,"empty array is not compiled object")
p=profile("native");p.supported=true;invalid_profile(p,"unsupported profile assertion field rejected")
invalid_profile(json.array(),"profile array rejected")
invalid_profile(json.null,"profile null rejected")
for _,target in ipairs({"native","web"}) do
    local runtime=profile(target,"runtime")
    check("complete runtime profile accepted "..target,caps.validate_profile(runtime)==true)
    for fact in pairs(runtime.available) do
        local missing=profile(target,"runtime");missing.available[fact]=nil
        invalid_profile(missing,"missing runtime fact "..target..":"..fact)
    end
end
p=profile("web","runtime");p.available.audio=1;invalid_profile(p,"runtime fact number is not boolean")
p=profile("native","runtime");p.available=nil;invalid_profile(p,"runtime availability is mandatory")
p=profile("native");p.binary="UNVERIFIED_BINARY";p.binary_sha256=string.rep("a",64);p.bundle_files=json.array()
check("provenance fields are allowed without attestation",caps.validate_profile(p)==true)
check("provenance cannot enable a disabled SDK",caps.query("video.ffmpeg",p).reason=="sdk_disabled")
p=profile("web");p.source_files={['scripts/runtime.lua']=string.rep('a',64),['web/目录 空格.js']=string.rep('b',64)}
check("source provenance shape preserves relative Unicode filenames",caps.validate_profile(p)==true)
for _,path in ipairs({'','/absolute.js','C:/private.js','../private.js','scripts/../private.js','scripts/./runtime.lua','scripts//runtime.lua','scripts/runtime.lua/','scripts\\runtime.lua','x\0y'}) do
    p=profile("web");p.source_files={[path]=string.rep('a',64)}
    local valid,why=caps.validate_profile(p)
    check("unsafe source provenance path refused "..path,valid==nil and why=='invalid_source_files')
end
for _,bad in ipairs({json.null,json.array(),true,"sources",{['scripts/runtime.lua']=false},{['scripts/runtime.lua']=string.rep('A',64)},{['scripts/runtime.lua']='abc'}}) do
    p=profile("web");p.source_files=bad
    local valid,why=caps.validate_profile(p)
    check("invalid source provenance refused",valid==nil and why=='invalid_source_files')
end
local hooks=0
p=setmetatable(profile("native"),{__pairs=function()hooks=hooks+1;error("PRIVATE_PROFILE")end,__index=function()hooks=hooks+1;error("PRIVATE_PROFILE")end,__eq=function()hooks=hooks+1;return false end})
check("ordinary host table is read without callbacks",caps.validate_profile(p)==true)
local q=caps.query("video.play",p)
check("query is callback independent",q.status=="supported" and hooks==0)
local old_digest=caps.catalog_sha256
caps.catalog_sha256="tampered"
p=profile("native");p.catalog_sha256="tampered"
invalid_profile(p,"export mutation cannot change private expected identity")
caps.catalog_sha256=old_digest

check("native MPEG base remains supported with SDKs off",caps.query("video.play",native).status=="supported")
q=caps.query("video.ffmpeg",native)
check("FFmpeg off reports proven SDK-disabled",q.status=="unsupported" and q.reason=="sdk_disabled" and q.proven==true)
for _,feature in ipairs({"live2d.cubism","steam.achievements","steam.stats","steam.cloud"}) do
    check("SDK-off feature rejected "..feature,caps.query(feature,native).reason=="sdk_disabled")
end
p=profile("native","runtime");p.compiled.ffmpeg=true;p.available.video=false
check("runtime unavailable cannot inherit build support",caps.query("video.ffmpeg",p).reason=="backend_unavailable")
p=profile("native");p.compiled.ffmpeg=true;p.compiled.live2d=true;p.compiled.steam=true
for _,feature in ipairs({"audio.crossfade","render.blur","kag.live2d_motion","kag.live2d_expression","kag.live2d_lip_sync"}) do
    check("fixed unsupported cannot be upgraded "..feature,caps.query(feature,p).status=="unsupported")
end
check("web bloom is unsupported",caps.query("render.postfx.bloom",web).status=="unsupported")
q=caps.query("render.postfx.lut3d",web)
check("web LUT reports approximation",q.status=="approximate" and q.reason=="css_fixed_grade")
q.status="supported";q.reason="tampered"
check("query results are fresh",caps.query("render.postfx.lut3d",web).status=="approximate")
p=profile("native","runtime");p.available.audio=nil
q=caps.query("audio.play",p)
check("invalid profile has no proven status",q.status==nil and q.proven==false)
q=caps.query("PRIVATE_BODY_TOKEN",native)
check("unknown feature denial is explicit",q.status=="unsupported" and q.reason=="unknown_feature")
check("query error does not echo a payload",not assert(json.encode(q)):find("PRIVATE_BODY_TOKEN",1,true))

local optional=policy('{"capabilities":{"optional":["video.play","render.postfx.lut3d"]}}')
check("implicit exact requirement allows support",caps.decide(legacy,"video.play",native).decision=="allow")
check("implicit unsupported requirement denies",caps.decide(legacy,"video.play",web).decision=="deny")
check("optional unsupported skips",caps.decide(optional,"video.play",web).decision=="skip")
check("implicit approximation requires acceptance",caps.decide(legacy,"render.postfx.lut3d",web).decision=="deny")
check("optional unaccepted approximation skips",caps.decide(optional,"render.postfx.lut3d",web).decision=="skip")
check("explicit approximation acceptance allows",caps.decide(acceptance,"render.postfx.lut3d",web).decision=="allow")
local invalid_optional=policy();invalid_optional.optional.unknown_feature=true
check("unknown feature always denies",caps.decide(invalid_optional,"unknown_feature",web).decision=="deny")
local location={scene="story.ks",line=12,command="video",body="PRIVATE_LOCATION_BODY"}
local decision=caps.decide(legacy,"video.play",native,location)
decision.location.scene="changed"
check("decision location is an isolated value",location.scene=="story.ks" and caps.decide(legacy,"video.play",native,location).location.scene=="story.ks")
check("locations never copy a command body",decision.location.body==nil)

local report=caps.check(policy('{"capabilities":{"required":["video.ffmpeg"]}}'),native,{}, {})
check("uncalled declared requirements are checked",not report.passed and #report.requirements==1 and #report.errors==1)
report=caps.check(legacy,web,{{feature="video.play",scene="branch.ks",line=8,command="video"}}, {})
check("static calls are implicit requirements",not report.passed and report.errors[1].feature=="video.play")
report=caps.check(optional,web,{{feature="video.play",scene="branch.ks",line=8,command="video"}}, {})
check("optional static call skips with warning",report.passed and #report.errors==0 and #report.warnings==1 and report.requirements[1].decision=="skip")
report=caps.check(acceptance,web,{{feature="render.postfx.lut3d",scene="story.ks",line=2,command="palette"}}, {})
check("accepted approximation remains visible",report.passed and #report.warnings==1 and report.requirements[1].capability.status=="approximate")
local unproven={{reason="dynamic_lua",scene="dynamic.ks",line=4,command="iscript",body="PRIVATE_BODY"}}
report=caps.check(legacy,web,{},unproven)
check("dynamic Lua does not globally fail a project",report.passed and #report.not_proven==1 and #report.errors==0)
check("unproved location and reason survive",report.not_proven[1].reason=="dynamic_lua" and report.not_proven[1].scene=="dynamic.ks")
check("unproved output does not copy bodies",report.not_proven[1].body==nil)
report.not_proven[1].scene="changed"
check("unproved records are copied",unproven[1].scene=="dynamic.ks")
check("analysis scope is bounded",report.analysis_scope=="declared_and_catalogued_static_calls")
local serialized=assert(json.encode(report));local serialized_report=document(serialized)
check("all report lists retain JSON array shape",json.is_array(serialized_report.requirements) and json.is_array(serialized_report.errors) and json.is_array(serialized_report.warnings) and json.is_array(serialized_report.not_proven))
p=profile("native","runtime");p.available.audio=nil
check("invalid profile cannot pass an empty analysis",not caps.check(legacy,p,{},{}).passed)
check("false call list is not an empty analysis",not caps.check(legacy,native,false,{}).passed)
check("false unproved list cannot disappear",not caps.check(legacy,native,{},false).passed)
report=caps.check(policy('{"capabilities":{"required":["video.play"]}}'),native,{{feature="video.play",scene="story.ks",line=1,command="video"}}, {})
check("called requirement is not duplicated without a location",report.passed and #report.requirements==1 and report.requirements[1].location.line==1)

local function mapping(command,params,expected,reason,static)
    local features,reasons=caps.command_features(command,params or {},static~=false)
    check("mapping "..command.." -> "..table.concat(expected,","),json.encode(features)==json.encode(json.array(expected)))
    if reason then
        local found=false;for _,r in ipairs(reasons) do if r==reason then found=true end end
        check("mapping reason "..command..":"..reason,found)
    else check("mapping is statically resolved "..command,#reasons==0) end
    return features,reasons
end
mapping("vfx",{}, {"render.particles"})
mapping("vfx",{postfx=""}, {"render.particles"})
mapping("vfx",{type="blur"}, {"render.blur"})
mapping("vfx",{type="stop",postfx="bloom"}, {"render.postfx.bloom"})
mapping("vfx",{type="stop"}, {})
mapping("vfx",{postfx="none"}, {})
mapping("postprocess",{}, {"render.postfx.bloom"})
mapping("postprocess",{effect="softblur"}, {"render.postfx.softblur"})
mapping("postprocess",{effect="off"}, {})
mapping("postprocess_off",{effect="bloom"}, {})
mapping("postprocess",{effect="PRIVATE_UNKNOWN_EFFECT"}, {"unknown_feature"})
mapping("postprocess",{effect="$tf.kind"}, {},"dynamic_selector")
mapping("vfx",{type="%effect%"}, {},"dynamic_selector")
mapping("vfx",{type="&f.effect"}, {},"dynamic_selector")
mapping("vfx",{type="$tf.kind"}, {"unknown_feature"},nil,false)
mapping("text",{text="${arbitrary_function()}"}, {},"dynamic_interpolation")
mapping("palette",{effect="night"}, {"render.postfx.lut3d"})
mapping("palette",{id="night"}, {"render.postfx.lut3d"})
mapping("palette",{}, {})
mapping("palette",{effect="clear"}, {})
mapping("palette",{effect="day"}, {})
mapping("palette",{effect="unload",id="night"}, {})
mapping("particles",{}, {"render.particles"})
mapping("particles",{action="emit"}, {"render.particles"})
mapping("particles",{action="destroy"}, {})
mapping("particle_weather",{}, {"render.particles"})
mapping("particle_weather",{action="stop"}, {})
mapping("video",{file="opening.MPG"}, {"video.play"})
mapping("video",{storage="opening.mpeg",file="ignored.mp4"}, {"video.play"})
mapping("video",{file="opening.mp4"}, {"video.play","video.ffmpeg"})
mapping("video",{file="%movie%"}, {"video.play"},"dynamic_video_format")
mapping("video",{file="extensionless"}, {"video.play"},"video_format_unproven")
mapping("stopvideo",{}, {})
mapping("playbgm",{file="bgm.ogg"}, {"audio.play"})
mapping("playbgm",{file="bgm.ogg",fadein="200"}, {"audio.play","audio.fade"})
mapping("playbgm",{file="bgm.ogg",fadein="$tf.fade"}, {"audio.play"},"dynamic_selector")
mapping("playbgmstop",{file="bgm.ogg",fadeout=100}, {"audio.play","audio.fade"})
mapping("playbgmstop",{fadeout=0}, {})
mapping("playbgmstop",{fadeout=100}, {"audio.fade"})
mapping("xfadebgm",{file="bgm.ogg"}, {"audio.crossfade"})
mapping("fadebgm",{time=0}, {"audio.fade"})
mapping("fadevol",{}, {"audio.fade"})
mapping("stopbgm",{fadeout=100}, {"audio.fade"})
mapping("playstop",{fadeout=100}, {"audio.fade"})
mapping("stopbgm",{time=100}, {}) -- schema's fadeout=0 shadows this legacy alias
mapping("stopse",{fadeout=100}, {}) -- actual handler ignores the schema field
mapping("playse",{file="a.wav",fadein=100}, {"audio.play"})
mapping("play",{file="a.ogg",fadein=100}, {"audio.play"}) -- alias drops fadein
mapping("play",{file="a.ogg",bus="$tf.bus"}, {},"dynamic_selector")
mapping("voice",{file="a.ogg"}, {"audio.play"})
mapping("stopvoice",{}, {})
mapping("waitsound",{}, {})
for _,command in ipairs({"live2d_motion","live2d_expression","live2d_lip_sync"}) do mapping(command,{}, {"kag."..command}) end
mapping("steam_achievement",{id="ACH",silent=true}, {"steam.achievements"})
for _,command in ipairs({"eval","iscript","emb"}) do mapping(command,{exp="PRIVATE_SCRIPT_BODY"}, {},"dynamic_lua") end

-- Resolve real command handlers with only their host backend dependencies mocked.
-- The resolver/schema/handlers themselves remain production code.
local calls={}
package.loaded.backend={
    particles_create_emitter=function()calls[#calls+1]="particles";return 7 end,
    clear_particles=function()calls[#calls+1]="clear";return true end,
    particles_destroy_emitter=function()end,
    is_postfx_supported=function()return true end,
    set_postfx=function(kind)calls[#calls+1]="postfx:"..kind;return 2 end,
    clear_postfx=function()calls[#calls+1]="postfx:clear" end,
    audio_play=function()calls[#calls+1]="audio:play" end,
    audio_stop=function()calls[#calls+1]="audio:stop" end,
    audio_fade_volume=function()calls[#calls+1]="audio:fade" end,
    audio_xfade=function()calls[#calls+1]="audio:crossfade" end,
}
package.loaded.vfx={blur=function()calls[#calls+1]="blur"end,stop_all=function()end}
package.loaded.mods={resolve=function(path)return path end}
package.loaded["kag.commands.vfx"]=nil;package.loaded["kag.commands.audio"]=nil
local actual_vfx=require("kag.commands.vfx")
local actual_audio=require("kag.commands.audio")
local schema=require("kag.schema")
actual_vfx.vfx({},{});actual_vfx.vfx({},{postfx=""});actual_vfx.postprocess({},{})
check("real vfx and postprocess defaults agree",table.concat(calls,",")=="particles,particles,postfx:bloom")
calls={};actual_audio.stopbgm({},schema.coerce("stopbgm",{time=100},{}))
check("actual coerced stopbgm time is shadowed",table.concat(calls,",")=="audio:stop")
calls={};actual_audio.playbgmstop({},schema.coerce("playbgmstop",{file="a.ogg",fadeout=100},{}))
check("actual playbgmstop stops then plays without bus fade or crossfade",table.concat(calls,",")=="audio:stop,audio:play")
calls={};actual_audio.stopse({},schema.coerce("stopse",{fadeout=100},{}))
check("actual stopse ignores its fade field",table.concat(calls,",")=="audio:stop")

print(string.format("Target Capability Tests: %d passed, %d failed",passed,failed))
if failed==0 then print("ALL TARGET CAPABILITY TESTS PASSED") end
os.exit(failed==0 and 0 or 1)
