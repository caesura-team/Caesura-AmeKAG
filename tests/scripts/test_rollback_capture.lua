-- Snapshot capture through real text commands, TextScene and layer declarations.
-- Host pixels/audio are controlled here; this does not claim a GPU visual test.
package.path = "scripts/?.lua;scripts/?/init.lua;" .. package.path

local original_backend, original_restore = package.loaded.backend, rawget(_G,"Restore")
local sources, next_texture, trace, voice_stops = {}, 100, {}, 0
local copy = require("kag.save_state").copy
package.loaded.backend = {
    get_resolution=function() return 1280,720 end,
    line_height=function() return 24 end,
    measure_text=function(text) return (utf8.len(text) or #text)*12 end,
    clear_text=function() end,
    audio_stop=function(bus) assert(bus=="voice"); voice_stops=voice_stops+1 end,
    render_text=function(text,x,y,r,g,b,a,scale,bold,italic,strike)
        trace[#trace+1]={text,x,y,r,g,b,a,scale,bold,italic,strike}
    end,
    render_ruby=function(text,ruby,x,y,r,g,b,a) trace[#trace+1]={text,ruby,x,y,r,g,b,a} end,
    create_solid_texture=function(r,g,b,a)
        next_texture=next_texture+1
        sources[next_texture]={kind="color",r=r,g=g,b=b,a=a}
        return next_texture
    end,
    create_viewport=function() return 1 end,
    destroy_viewport=function() end,
    destroy_texture=function(id) sources[id]=nil end,
    is_valid_handle=function(_,id) return sources[id]~=nil end,
}
_G.Restore={
    describe_texture=function(id) return copy(sources[id]) end,
    prepare_color=function(r,g,b,a) return {kind="color",r=r,g=g,b=b,a=a} end,
    image_info=function() return 1,1 end,
    discard_image=function() end,
    materialize_image=function(source)
        return package.loaded.backend.create_solid_texture(source.r,source.g,source.b,source.a)
    end,
}
local Text=require("kag.text_scene")
local Commands=require("kag.commands.text")
local LayerCommands=require("kag.commands.layer")
local Layers=require("layers")
local Snapshot=require("kag.snapshot")
local encode=require("kag.compiler").encode_lua_literal
local passed,failed=0,0
local function check(name,condition)
    if condition then passed=passed+1; print("PASS "..name)
    else failed=failed+1; print("FAIL "..name) end
end
local function scenario(name,fn)
    Layers.clear_for_restore()
    local ok,err=pcall(fn)
    if not ok then failed=failed+1; print("FAIL "..name..": "..tostring(err)) end
end
local function ctx()
    return {current_scene="capture.ks",token_index=2,_resume_index=3,
        f={},sf={},tf={},mp={},lf={},variables={},call_stack={},backlog={},seen_scenes={},
        tokens={},macros={},characters={},text_speed=50,waiting_input=true}
end
local function picture(context)
    trace={}; Text.render(context)
    return encode(trace)
end
local function reveal(context)
    context.reveal.elapsed=context.reveal.total*context.text_speed
    context.reveal.last_shown=context.reveal.total
    context.text_state.reveal_chars=context.reveal.total
end

scenario("NVL history and later continuation",function()
    local c=ctx()
    Commands.nvl(c,{})
    Commands.text(c,{text="{b}甲乙{/b}{color=#aabbcc}丙丁{/color}"})
    reveal(c)
    local first_picture=picture(c)
    local live=c.text_state.draws[1]
    local snap=Snapshot.capture(c)
    check("history owns its semantic draw value",snap.text_state.draws[1]~=live)
    check("capture excludes transient reveal caches",snap.text_state.draws[1]._shown==nil
        and snap.text_state.draws[1]._shown_len==nil)
    local captured_typewriter=snap.text_state.draws[1].typewriter
    Commands.text(c,{text="后续文字"}) -- real NVL path commits earlier draws in place
    check("NVL commit actually seals the live prior line",live.typewriter==false)
    check("NVL commit cannot change retained semantic flags",
        snap.text_state.draws[1].typewriter==captured_typewriter)
    local next_snap=Snapshot.capture(c)
    check("capture observes an in-place commit as a new historical value",
        next_snap.text_state.draws[1].typewriter==false
        and next_snap.text_state.draws[1]~=snap.text_state.draws[1])
    local same_snap=Snapshot.capture(c)
    check("unchanged captures reuse detached values across histories",
        same_snap.text_state.draws[1]==next_snap.text_state.draws[1]
        and same_snap.text_state.page_src[1]==next_snap.text_state.page_src[1])
    reveal(c); picture(c)
    check("snapshot restores after the later NVL commit",Snapshot.restore(c,snap)==true)
    check("restored NVL line submits the original full text and markup",picture(c)==first_picture)
    Commands.text(c,{text="重放文字"})
    c.text_state.reveal_chars=1
    trace={}; Text.render(c)
    check("new NVL reveal keeps earlier line fully shown",trace[1][1]=="甲乙" and trace[2][1]=="丙丁"
        and trace[#trace][1]=="重")
    check("new NVL line continues at the historical cursor",trace[#trace][3]>trace[1][3])
    check("restoring and replaying cannot mutate retained draw flags",
        snap.text_state.draws[1].typewriter==captured_typewriter)
end)

scenario("page replay source ownership",function()
    local c=ctx()
    Commands.text(c,{text="原始文本",color="#112233"}); reveal(c)
    local old_picture=picture(c)
    local source=c.text_state.page_src[1]
    local saved=Snapshot.capture(c)
    source.opts.msgX=999; source.opts.color.r=250
    check("history isolates nested page replay options",saved.text_state.page_src[1].opts.msgX~=999
        and saved.text_state.page_src[1].opts.color.r==17)
    Snapshot.restore(c,saved)
    Commands.relocalize_page(c)
    check("language page replay after restore uses captured layout and color",picture(c)==old_picture)
end)

scenario("textbox style survives the next real clear command",function()
    local c=ctx()
    local historical={x=17,y=500,w=1100,h=180,color="12,34,56",opacity=170,visible=true}
    Commands.textbox(c,historical)
    local saved=Snapshot.capture(c)
    Commands.textbox(c,{x=200,y=400,w=700,h=120,color="90,80,70",opacity=90,visible=false})
    Snapshot.restore(c,saved)
    LayerCommands.cl(c,{layer="message"})
    local node=Layers.get("_textbox")
    local color=sources[node.texture]
    check("clear after restore uses historical textbox geometry",node.x==17 and node.y==500
        and node.w==1100 and node.h==180 and node.visible==true)
    check("clear after restore uses historical textbox color",color and color.r==12 and color.g==34
        and color.b==56 and color.a==170)
    c.textbox_style.x=333
    check("restored textbox style is independent from history",saved.textbox_style
        and saved.textbox_style.x==17)
    Snapshot.restore(c,saved)
    check("repeated restore retains historical style",c.textbox_style.x==17)
end)

scenario("absent historical style clears a future style",function()
    local c=ctx(); local saved=Snapshot.capture(c)
    Commands.textbox(c,{x=200,y=400,w=700,h=120,color="90,80,70",opacity=90,visible=false})
    Snapshot.restore(c,saved)
    LayerCommands.cl(c,{layer="message"})
    check("missing style cannot recreate a future textbox after clear",c.textbox_style==nil
        and Layers.get("_textbox")==nil)
end)

scenario("textbox preparation isolates values and rejects invalid styles",function()
    local c=ctx()
    Commands.textbox(c,{x=17,y=500,w=1100,h=180,color="12,34,56",opacity=170,visible=true})
    local saved=Snapshot.capture(c)
    local prepared=Snapshot.prepare(c,saved)
    saved.textbox_style.x=999
    Snapshot.apply(c,prepared)
    check("prepared textbox does not read later snapshot mutations",c.textbox_style.x==17)
    for _,value in ipairs({false,{}, {x=0/0,y=500,w=1100,h=180,color="12,34,56",opacity=170,visible=true}}) do
        saved.textbox_style=value
        local old_style,old_node=c.textbox_style,Layers.get("_textbox")
        local ok=pcall(Snapshot.prepare,c,saved)
        check("malformed textbox rejects without touching live context or tree",
            not ok and c.textbox_style==old_style and Layers.get("_textbox")==old_node)
    end
end)

check("rollback only stops voice at the host boundary",voice_stops>0)
Layers.clear_for_restore()
package.loaded.backend, _G.Restore=original_backend, original_restore
print(string.format("ROLLBACK CAPTURE TESTS: %d passed, %d failed",passed,failed))
assert(failed==0,"rollback capture failures")
