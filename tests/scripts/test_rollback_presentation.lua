-- U12 rollback uses the real layer tree and text renderer with ticket backends.
-- Submission equivalence is tested here; GPU pixels belong to native validation.
package.path = "scripts/?.lua;scripts/?/init.lua;" .. package.path
local image_tickets, font_tickets, sources, trace = {}, {}, {}, {}
local next_texture, next_rt = 100, 10
local failed_path, fail_font_prepare, fail_font_apply, fail_upload
local active_font = {version=1,active=true,font=2,path="assets/a.ttf",size=24}
local audio_stops, bgm_position, gpu_calls = {}, 91, 0
local copy = require("kag.save_state").copy
local encode = require("kag.compiler").encode_lua_literal
local function ticket(list, source)
    local value={source=copy(source),used=false,discards=0}
    list[#list+1]=value
    return value
end
package.loaded.backend = {
    get_resolution=function() return 1280,720 end,
    create_viewport=function() gpu_calls=gpu_calls+1; next_rt=next_rt+1; return next_rt end,
    destroy_viewport=function() gpu_calls=gpu_calls+1 end,
    destroy_texture=function(id) gpu_calls=gpu_calls+1; sources[id]=nil end,
    is_valid_handle=function(_,id) return sources[id]~=nil end,
    audio_stop=function(channel)
        audio_stops[#audio_stops+1]=channel
        if channel=="bgm" or channel=="all" then bgm_position=-1 end
    end,
    render_text=function(...) trace[#trace+1]={kind="text",values={...}} end,
    submit_batch=function(batch)
        for i=1,batch[1] do
            local base=1+(i-1)*16
            local record={kind="layer",source=copy(sources[batch[base+2]])}
            for offset=4,16 do record[offset-3]=batch[base+offset] end
            trace[#trace+1]=record
        end
    end,
}
_G.Restore = {
    describe_texture=function(id) return sources[id] end,
    prepare_image=function(path)
        if path==failed_path then return nil,"injected image prepare failure" end
        return ticket(image_tickets,{kind="asset",path=path})
    end,
    prepare_color=function(r,g,b,a) return ticket(image_tickets,{kind="color",r=r,g=g,b=b,a=a}) end,
    image_info=function() return 2,2 end,
    discard_image=function(value)
        value.discards=value.discards+1; assert(value.discards==1,"duplicate image discard")
        value.used=true
    end,
    materialize_image=function(value)
        assert(not value.used); value.used=true; gpu_calls=gpu_calls+1
        if fail_upload then return nil,"injected image apply failure" end
        next_texture=next_texture+1; sources[next_texture]=copy(value.source)
        return next_texture
    end,
    capture_font=function() return copy(active_font) end,
    prepare_font=function(value)
        if fail_font_prepare then return nil,"injected font prepare failure" end
        return ticket(font_tickets,value)
    end,
    discard_font=function(value)
        value.discards=value.discards+1; assert(value.discards==1,"duplicate font discard")
        value.used=true
    end,
    apply_font=function(value)
        assert(not value.used); value.used=true
        if fail_font_apply then return false,"injected font apply failure" end
        active_font=copy(value.source); return true
    end,
    clear_font=function() active_font={version=1,active=false}; return true end,
}
local Layers=require("layers")
local Text=require("kag.text_scene")
local Snapshot=require("kag.snapshot")
local passed,failed=0,0
local function check(name,condition)
    if condition then passed=passed+1 else failed=failed+1; print("FAIL: "..name) end
end
local function run(name,fn)
    local ok,err=pcall(fn)
    if not ok then failed=failed+1; print("FAIL: "..name..": "..tostring(err)) end
end
local function picture(ctx)
    trace={}; Layers.render(); Text.render(ctx)
    return encode({draws=trace,font=active_font})
end
local function context_a()
    failed_path,fail_font_prepare,fail_font_apply,fail_upload=nil,false,false,false
    Layers.clear_for_restore()
    active_font={version=1,active=true,font=2,path="assets/a.ttf",size=24}
    sources[1]={kind="asset",path="assets/a.bmp"}
    sources[2]={kind="color",r=12,g=34,b=56,a=255}
    sources[3]={kind="asset",path="assets/future.bmp"}
    local parent=Layers.add_layer(nil,{id="parent",x=10,y=20,z=1})
    local a=Layers.add_layer(parent,{id="a",x=3,y=4,w=120,h=80,z=2,opacity=180})
    a.tex=1; a.scaleX=-1; a.scaleY=2; a.rotation=30
    a.clipX=1; a.clipY=2; a.clipW=80; a.clipH=60
    local b=Layers.add_layer(parent,{id="b",w=20,h=30,z=2}); b.tex=2
    local ctx={current_scene="a.ks",token_index=4,_resume_index=5,call_stack={},
        f={point="A"},sf={},tf={temporary="A"},mp={},variables={},lf={},
        backlog={{text="A"}},seen_scenes={},text_speed=40,waiting_input=true,
        _undoStack={},co=coroutine.create(function() coroutine.yield() end)}
    Text.add_text(ctx,"A历史画面",32,580,{255,230,210,255})
    ctx.reveal={total=5,elapsed=200,last_shown=5}
    ctx.text_state.reveal_chars=5
    return ctx,Snapshot.capture(ctx)
end
local function future(ctx)
    Layers.remove_layer(Layers.get_layer("a"))
    Layers.get_layer("b").tex=3
    local future_node=Layers.add_layer(nil,{id="future",w=40,h=50,z=3}); future_node.tex=3
    active_font={version=1,active=true,font=2,path="assets/future.ttf",size=32}
    ctx.f={point="B"}; ctx.tf={temporary="B"}; ctx.token_index=10
    ctx.backlog[2]={text="B"}; Text.clear(ctx); Text.add_text(ctx,"B",32,580,{1,2,3,255})
    return future_node
end
local function unchanged(ctx,node,old_f,old_history,old_co,old_picture,old_gpu,old_audio)
    return Layers.get_layer("future")==node and ctx.f==old_f and ctx.token_index==10
        and ctx._undoStack==old_history and ctx.co==old_co and #ctx.backlog==2
        and picture(ctx)==old_picture and gpu_calls==old_gpu and #audio_stops==old_audio
end

run("actual rollback presentation roundtrip",function()
    local ctx,snap=context_a(); local original=picture(ctx)
    future(ctx)
    check("direct restore succeeds",Snapshot.restore(ctx,snap)==true)
    check("deleted tree and source values return in actual render submissions",picture(ctx)==original)
    check("future nodes removed and parent order restored",Layers.get_layer("future")==nil
        and Layers.get_layer("parent").children[1].id=="a"
        and Layers.get_layer("parent").children[2].id=="b")
    check("rollback preserves tf semantics",ctx.tf.temporary=="A")
    check("only old voice stops and BGM keeps its position",audio_stops[#audio_stops]=="voice"
        and bgm_position==91)
    local encoded=encode(snap.layers)
    check("history contains texture origins and no live texture handles",encoded:find("assets/a.bmp",1,true)
        and not encoded:find('"tex"',1,true) and not encoded:find('"ticket"',1,true))
end)

for _,kind in ipairs({"image","font"}) do
    run(kind.." prepare failure leaves live state intact",function()
        local ctx,snap=context_a(); local node=future(ctx)
        local old_picture,old_f,old_history,old_co=picture(ctx),ctx.f,ctx._undoStack,ctx.co
        local old_gpu,old_audio,old_tickets=gpu_calls,#audio_stops,#image_tickets
        if kind=="image" then
            -- The first image prepares successfully; the second must release it.
            snap.layers.nodes[4].source={kind="asset",path="assets/missing.bmp"}
            failed_path="assets/missing.bmp"
        else fail_font_prepare=true end
        local ok,err=pcall(Snapshot.restore,ctx,snap)
        check(kind.." failure propagates preparation error",not ok and tostring(err):find("prepare failure",1,true))
        check(kind.." failure preserves old values, history, coroutine and render",
            unchanged(ctx,node,old_f,old_history,old_co,old_picture,old_gpu,old_audio))
        check(kind.." failure releases each acquired image once",#image_tickets>old_tickets)
        for i=old_tickets+1,#image_tickets do check("prepared image discarded once",image_tickets[i].discards==1) end
    end)
end

run("prepare is independent and discard is idempotent",function()
    local ctx,snap=context_a(); local original=picture(ctx); local node=future(ctx)
    local old_gpu,old_audio=gpu_calls,#audio_stops
    local prepared=Snapshot.prepare(ctx,snap)
    check("prepare keeps current scene and audio",Layers.get_layer("future")==node
        and ctx.f.point=="B" and gpu_calls==old_gpu and #audio_stops==old_audio)
    check("another context cannot consume candidate",not pcall(Snapshot.apply,{},prepared)
        and Layers.get_layer("future")==node)
    snap.f.point="changed after prepare"; snap.text_state.draws[1].text="changed after prepare"
    snap.layers.nodes[3].x=999
    check("prepared candidate commits",Snapshot.apply(ctx,prepared)==true)
    check("commit uses independent prepared values",ctx.f.point=="A" and picture(ctx)==original)
    check("candidate can only commit once",not pcall(Snapshot.apply,ctx,prepared))
    local candidate=Snapshot.prepare(ctx,Snapshot.capture(ctx))
    local image_start,font_start=#image_tickets,#font_tickets
    check("discard succeeds",Snapshot.discard(candidate)==true)
    check("discard succeeds again",Snapshot.discard(candidate)==true)
    check("discard releases font once",font_tickets[font_start].discards==1)
    check("discard releases image once",image_tickets[image_start].discards==1)
    check("discarded candidate rejects commit",not pcall(Snapshot.apply,ctx,candidate))
end)

run("invalid text rejects before preparing resources",function()
    local ctx,snap=context_a(); local node=future(ctx)
    local old_picture,old_f,old_history,old_co=picture(ctx),ctx.f,ctx._undoStack,ctx.co
    local old_gpu,old_audio,old_images,old_fonts=gpu_calls,#audio_stops,#image_tickets,#font_tickets
    snap.text_state.draws[1].x=math.huge
    check("invalid text fails in preparation",not pcall(Snapshot.prepare,ctx,snap))
    check("invalid text has no live mutation",unchanged(ctx,node,old_f,old_history,old_co,
        old_picture,old_gpu,old_audio))
    check("invalid text acquires no tickets",#image_tickets==old_images and #font_tickets==old_fonts)
end)

run("one discard error does not strand other tickets",function()
    local ctx,snap=context_a(); future(ctx)
    local prepared=Snapshot.prepare(ctx,snap)
    local old_discard=Restore.discard_image
    local calls=0
    Restore.discard_image=function(value)
        calls=calls+1; old_discard(value)
        if calls==1 then error("injected discard failure") end
    end
    local ok,err=Snapshot.discard(prepared)
    Restore.discard_image=old_discard
    check("discard returns cleanup failure",ok==false and err:find("discard failure",1,true))
    check("cleanup continues through both images and font",calls==2 and font_tickets[#font_tickets].discards==1)
    check("retry cleanup does not release tickets twice",Snapshot.discard(prepared)==true)
end)

for _,kind in ipairs({"image","font"}) do
    run(kind.." commit failure is explicit and cleaned",function()
        local ctx,snap=context_a(); future(ctx)
        local prepared=Snapshot.prepare(ctx,snap)
        if kind=="image" then fail_upload=true else fail_font_apply=true end
        local ok,err=pcall(Snapshot.apply,ctx,prepared)
        check(kind.." commit failure propagates",not ok and tostring(err):find("apply failure",1,true))
        check(kind.." commit failure leaves no partial restored graph",Layers.count()==1
            and next(ctx._restoredTextures or {})==nil)
        check(kind.." commit failure releases all remaining candidates",Snapshot.discard(prepared)==true)
        check(kind.." commit failure clears font",active_font.active==false)
    end)
end
run("inactive historical font clears the future selection",function()
    local ctx=context_a()
    active_font={version=1,active=false}
    local snap=Snapshot.capture(ctx)
    future(ctx)
    check("inactive font restores",Snapshot.restore(ctx,snap)==true and active_font.active==false)
end)
check("all acquired image candidates are consumed or released",(function()
    for _,value in ipairs(image_tickets) do if not value.used or value.discards>1 then return false end end
    return true
end)())
check("all acquired font candidates are consumed or released",(function()
    for _,value in ipairs(font_tickets) do if not value.used or value.discards>1 then return false end end
    return true
end)())
print(string.format("U12 ROLLBACK PRESENTATION: %d passed, %d failed",passed,failed))
os.exit(failed==0 and 0 or 1)
