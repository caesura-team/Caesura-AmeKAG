-- U26: real tokenizer -> compiler/schema -> scheduler -> backend.lua.
-- Only the native Live2D table is a fake. This is not Cubism/SDK evidence.
package.path = "scripts/?.lua;scripts/?/init.lua;" .. package.path
local passed, failed = 0, 0
local function check(name, value)
    print((value and "PASS " or "FAIL ") .. name)
    if value then passed = passed + 1 else failed = failed + 1 end
end
local calls, models, next_handle = {}, {}, 100
local reject_load, reject_voice = false, false
local function record(...) calls[#calls + 1] = {...} end
local native = {
    load = function(path, name)
        record("load", path, name)
        if reject_load then return nil, "fixture model load failed" end
        next_handle = next_handle + 1; models[next_handle] = {}
        return next_handle
    end,
    show = function(handle, x, y, scale)
        record("show", handle, x, y, scale); return models[handle] ~= nil
    end,
    hide = function(handle) record("hide", handle); return models[handle] ~= nil end,
    unload = function(handle)
        record("unload", handle)
        if not models[handle] then return false, "unknown handle" end
        models[handle] = nil; return true
    end,
    set_mouth = function(handle, value)
        record("mouth", handle, value); return models[handle] ~= nil
    end,
    set_voice_lipsync = function(handle, enabled)
        record("voice", handle, enabled)
        if reject_voice then return false, "fixture unsupported mouth" end
        return models[handle] ~= nil
    end,
    play_motion = function(handle, name, fadein, fadeout)
        record("motion", handle, name, fadein, fadeout)
        if not models[handle] or name ~= "TapBody/0" then return false, "motion not found" end
        return true
    end,
    set_expression = function(handle, name, weight)
        record("expression", handle, name, weight)
        if not models[handle] or name ~= "F01" then return false, "expression not found" end
        return true
    end,
}
_G.Live2D = native
local tokenizer, scheduler = require("tokenizer"), require("scheduler")
local schema, transients = require("kag.schema"), require("kag.transient_state")
local function context()
    return {f={},sf={},tf={},variables={},call_stack={},active_operations={},current_scene="u26-live2d.ks"}
end
local function execute(ctx, text)
    ctx.stop_flag, ctx._command_error = false, nil
    local parsed, tokens = pcall(tokenizer.parse, text)
    if not parsed then return false, tokens end
    ctx.tokens = tokens
    local co = coroutine.create(function() scheduler.run(ctx, tokens, 1) end)
    for _ = 1, 128 do
        local ok, err = coroutine.resume(co, 0.016)
        if not ok then return false, err end
        if coroutine.status(co) == "dead" then return not ctx._command_error, ctx.error_command end
    end
    coroutine.close(co)
    return false, "bounded scheduler did not finish"
end
local ctx = context()
local ok = execute(ctx, [[
[live2d_load model=haru storage="assets/live2d/Haru/Haru.model3.json"]
[live2d_show model=haru x=12 y=34 scale=1.5]
[live2d_lip_sync model=haru source=voice]
[live2d_lip_sync model=haru source=off]
[live2d_lip_sync model=haru value=0.8]
[live2d_hide model=haru]
]])
check("actual command pipeline reaches six native calls", ok and #calls == 6)
local handle = ctx._live2dHandles and ctx._live2dHandles.haru
check("load publishes a positive handle owned by this ctx", type(handle)=="number" and models[handle]~=nil)
check("typed show values reach native boundary", calls[2] and calls[2][1]=="show" and calls[2][2]==handle
    and calls[2][3]==12 and calls[2][4]==34 and calls[2][5]==1.5)
check("voice and off preserve boolean mode", calls[3] and calls[3][1]=="voice" and calls[3][3]==true
    and calls[4] and calls[4][1]=="voice" and calls[4][3]==false)
check("legacy default source routes manual mouth", calls[5] and calls[5][1]=="mouth" and calls[5][3]==0.8)
check("a fully declared Live2D model is saveable", pcall(transients.assert_saveable, ctx))
local before = #calls
ok = execute(ctx, '[live2d_motion model=haru motion="TapBody/0" fadein=250 fadeout=750]')
local motion = calls[before + 1]
check("motion reaches the context-owned model with second-based fades",
    ok and motion and motion[1]=="motion" and motion[2]==handle and motion[3]=="TapBody/0"
        and motion[4]==0.25 and motion[5]==0.75)
check("motion metadata reflects actual admission", ctx.live2d.haru.current_motion=="TapBody/0")
before = #calls
ok = execute(context(), '[live2d_motion model=haru motion="TapBody/0"]')
check("motion cannot borrow a handle from another context", not ok and #calls==before)
ok = execute(ctx, '[live2d_motion model=haru motion=unknown]')
check("rejected motion leaves the last admitted motion unchanged",
    not ok and #calls==before+1 and ctx.live2d.haru.current_motion=="TapBody/0")
before = #calls
ok = execute(ctx, '[live2d_expression model=haru expression=F01 weight=0.25]')
local expression = calls[before + 1]
check("expression reaches the current model with its authored weight",
    ok and expression and expression[1]=="expression" and expression[2]==handle
        and expression[3]=="F01" and expression[4]==0.25)
before = #calls
ok = execute(context(), '[live2d_expression model=haru expression=F01]')
check("expression cannot borrow another context's model", not ok and #calls==before)
ok = execute(ctx, '[live2d_expression model=haru expression=unknown]')
check("rejected expression preserves the last admitted state", not ok and #calls==before+1
    and ctx.live2d.haru.expression=="F01" and ctx.live2d.haru.expression_weight==0.25)
ok = execute(ctx, '[live2d_expression model=haru expression=F01 weight=0]')
check("zero weight reaches the native boundary", ok and #calls==before+2 and calls[#calls][4]==0)
local saved, snapshot = pcall(function()
    return require("kag.live2d_state").capture(ctx)
end)
check("capture preserves hidden placement and excludes live handles", saved and snapshot.haru.visible==false
    and snapshot.haru.x==12 and snapshot.haru.y==34 and snapshot.haru.scale==1.5
    and snapshot.haru.handle==nil and snapshot.haru.expression_weight==0
    and snapshot.haru.current_motion=="TapBody/0" and snapshot.haru.lip_sync==0.8)
local model_state = require("kag.live2d_state")
local invalid = require("kag.save_state").copy(snapshot)
invalid.haru.expression = "unknown"
local failed_handle = next_handle + 1
check("failed model preparation releases the candidate and preserves the live model",
    not pcall(model_state.prepare, invalid) and models[failed_handle]==nil and models[handle]~=nil)
local prepared = model_state.prepare(snapshot)
local prepared_handle = prepared.handles[1]
check("prepared hidden model has a distinct handle", prepared_handle~=handle and models[prepared_handle]~=nil)
_G.Restore = {stop_transients=function(keep)
    for loaded in pairs(models) do
        if not keep or loaded ~= keep[1] then models[loaded]=nil end
    end
    return true
end}
local retired = {_live2dHandles={haru=handle},live2d={haru={}}}
check("retiring the previous scene preserves the prepared model",
    transients.stop(retired, prepared.handles) and models[handle]==nil and models[prepared_handle]~=nil)
model_state.apply(prepared, ctx)
check("commit adopts the restored model and keeps it hidden",
    ctx._live2dHandles.haru==prepared_handle and ctx.live2d.haru.visible==false
    and next(prepared.handles)==nil and calls[#calls][1]=="mouth")
handle = prepared_handle
_G.Restore = nil
do
    local presentation = require("kag.presentation")
    presentation.adopt(ctx)
    local candidate = {models=model_state.prepare(snapshot)}
    local candidate_handle = candidate.models.handles[1]
    _G.Restore = {stop_transients=function() return false, "old cleanup failed" end}
    check("failed owner retirement discards the prepared models",
        not pcall(presentation.apply,candidate,{}) and models[candidate_handle]==nil and models[handle]~=nil)
    presentation.adopt(nil)
    _G.Restore = nil
end
do
    local rollback = require("kag.snapshot")
    local point = rollback.capture(ctx)
    local backend = require("backend")
    local audio_stop = backend.audio_stop
    backend.audio_stop = function() return false, "fixture voice cleanup failed" end
    local candidate_handle = next_handle + 1
    check("direct rollback releases a model if a later commit step fails",
        not pcall(rollback.restore,ctx,point) and models[candidate_handle]==nil
        and models[handle]==nil and next(ctx._live2dHandles)==nil)
    backend.audio_stop = audio_stop
    model_state.apply(model_state.prepare(snapshot), ctx)
    handle = ctx._live2dHandles.haru
end
before = #calls
ok = execute(ctx, '[live2d_load model=haru storage="different.model3.json"]')
check("duplicate name fails before calling native load", not ok and #calls==before
    and ctx._live2dHandles and ctx._live2dHandles.haru==handle)
for _,source in ipairs({"voice","off"}) do
    before=#calls
    ok=execute(ctx, '[live2d_lip_sync model=haru source='..source..' value=0]')
    check("explicit value conflicts with "..source, not ok and #calls==before)
end
before=#calls
ok=execute(context(), '[live2d_lip_sync model=haru source=voice]')
check("another ctx cannot borrow a global model name", not ok and #calls==before)
reject_load=true
ok=execute(ctx, '[live2d_load model=other storage="missing.model3.json"]')
check("load failure keeps previous mapping and does not publish another", not ok and ctx._live2dHandles
    and ctx._live2dHandles.haru==handle and ctx._live2dHandles.other==nil)
reject_load=false; reject_voice=true
ok=execute(ctx, '[live2d_lip_sync model=haru source=voice]')
check("unsupported mouth is a runtime error with model retained", not ok and ctx._live2dHandles
    and ctx._live2dHandles.haru==handle and models[handle]~=nil)
reject_voice=false
_G.Live2D=nil; before=#calls
ok=execute(ctx, '[live2d_show model=haru]')
check("missing native backend fails without destroying ownership", not ok and #calls==before
    and ctx._live2dHandles and ctx._live2dHandles.haru==handle)
_G.Live2D=native
_G.KAG={load_game=function() return nil,"fixture preparation failed" end}
local saved_map=ctx._live2dHandles
local loaded=require("kag.commands.save").load(ctx,{slot=31})
check("failed save-load preparation preserves loaded model and mapping", loaded==false
    and ctx._live2dHandles==saved_map and handle~=nil and models[handle]~=nil)
ok=execute(ctx, '[live2d_unload model=haru]')
check("successful unload removes handle and declaration", ok and ctx._live2dHandles
    and ctx._live2dHandles.haru==nil and (not ctx.live2d or ctx.live2d.haru==nil)
    and handle~=nil and models[handle]==nil)
before=#calls
ok=execute(ctx, '[live2d_hide model=haru]')
check("use after unload is a runtime error before native call", not ok and #calls==before)
ok=execute(ctx, '[live2d_load model=haru storage="Haru.model3.json"]')
local replacement=ctx._live2dHandles and ctx._live2dHandles.haru
check("reload gets a new handle", ok and replacement~=nil and replacement~=handle)
_G.Restore={stop_transients=function() return false,"fixture cleanup failed" end}
local stopped=transients.stop(ctx)
check("failed transient cleanup retains ownership mapping", stopped==false and ctx._live2dHandles
    and ctx._live2dHandles.haru==replacement)
_G.Restore.stop_transients=function() models={}; return true end
check("successful transient cleanup clears mapping and declarations", transients.stop(ctx)==true
    and ctx._live2dHandles and next(ctx._live2dHandles)==nil and ctx.live2d and next(ctx.live2d)==nil)
_G.Restore=nil
check("private handles also prevent save even without declarations",
    not pcall(transients.assert_saveable,{_live2dHandles={haru=501}}))
-- The capture guard above is the boundary; no persistence format includes handles.
check("source enum is registered", schema.specs("live2d_lip_sync").source~=nil)
ok=execute(ctx, '[live2d_load model=cleanup storage="Haru.model3.json"]')
local cleanup_handle=ctx._live2dHandles and ctx._live2dHandles.cleanup
before=#calls
check("direct host cleanup releases the ctx-owned native handle", ok and transients.stop(ctx)==true
    and #calls==before+1 and calls[#calls][1]=="unload" and calls[#calls][2]==cleanup_handle
    and models[cleanup_handle]==nil and next(ctx._live2dHandles)==nil)
ok=execute(ctx, '[live2d_load model=defaults storage="Haru.model3.json"][live2d_lip_sync model=defaults]')
check("omitted manual value is zero", ok and calls[#calls][1]=="mouth" and calls[#calls][3]==0)
for _,invalid in ipairs({"source=unknown", "source=voice 0", "source=off 0",
    'source=voice value=""', 'source=off value=""'}) do
    before=#calls
    ok=execute(ctx,'[live2d_lip_sync defaults '..invalid..']')
    check("invalid or ambiguous positional mode fails before native: "..invalid, not ok and #calls==before)
end
assert(transients.stop(ctx))
-- Retain the current public unsupported Web declaration while the root is
-- still validating the native bridge; this does not upgrade the catalog.
local capabilities=require("target_capabilities")
local web_profile={schema=1,target="web",scope="runtime",platform="browser",
    catalog_sha256=capabilities.catalog_sha256,compiled={},available={}}
local catalog=assert(require("capability_json").decode(require("capability_catalog").json))
for _,feature in pairs(catalog.features) do
    local fact=feature.web.available
    if fact then web_profile.available[fact]=true end
end
assert(capabilities.validate_profile(web_profile))
local support=capabilities.query("kag.live2d_lip_sync",web_profile)
check("Web lip sync remains explicitly unsupported", support.status=="unsupported")
print(string.format("U26 Live2D pipeline: %d passed, %d failed; native_boundary=FAKE; Cubism=NOT_RUN",passed,failed))
if failed>0 then os.exit(1) end
