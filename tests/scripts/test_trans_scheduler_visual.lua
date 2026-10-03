-- Real tokenizer/compiler/scheduler/commands/layers. Only render hardware is recorded.
-- This is a scheduling/source-identity regression, not GPU pixel evidence.
package.path = "scripts/?.lua;scripts/?/init.lua;" .. package.path
local previous_backend = _G._CAESURA_BACKEND
local hw = {frame=0, scene="BLACK", pending="BLACK", overlay=nil, next=1000,
    leases={}, captures={}, submits={}, frames={}, destroyed={}}
local textures = {[101]="A", [102]="B"}
local function allocate() local id=hw.next;hw.next=id+1;return id end
_G._CAESURA_BACKEND = {
    render = function(cmd,...)
        local a={...}
        if cmd=="load_texture" then
            assert(a[1]=="scene-A.png" or a[1]=="scene-B.png", 'unexpected asset')
            if hw.failLoad and a[1]=='scene-B.png' then error('controlled asset failure') end
            return a[1]=="scene-A.png" and 101 or 102
        elseif cmd=="create_viewport" then return allocate()
        elseif cmd=="destroy_viewport" then hw.destroyed[#hw.destroyed+1]=a[1];return true
        elseif cmd=="set_view_name" then return true
        elseif cmd=="get_resolution" then return 320,180
        elseif cmd=="submit_batch" then
            local batch=a[1]
            for i=1,batch[1] do
                local texture=batch[1+(i-1)*16+2]
                if textures[texture] then hw.pending=textures[texture] end
            end
            return true
        elseif cmd=="capture_scene" then
            if hw.frame==0 then return 0,0 end
            local id=allocate();hw.leases[id]={scene=hw.scene,frame=hw.frame}
            hw.captures[#hw.captures+1]={id=id,scene=hw.scene,frame=hw.frame}
            print(string.format('CAPTURE id=%d frame=%d scene=%s',id,hw.frame,hw.scene))
            return id,hw.frame
        elseif cmd=="cancel_transition" then hw.overlay=nil;return true
        elseif cmd=="submit_transition" then
            assert(type(a[2])=='number' and type(a[3])=='number','numeric leases required')
            local from,to=assert(hw.leases[a[2]]),assert(hw.leases[a[3]])
            hw.overlay={from=from.scene,to=to.scene,t=a[6],method=a[5]}
            hw.submits[#hw.submits+1]=hw.overlay
            print(string.format('SUBMIT frame=%d from=%s to=%s t=%.3f',hw.frame,from.scene,to.scene,a[6]))
            return true
        else error('unexpected render boundary: '..cmd) end
    end,
    platform=function(cmd,...)
        if cmd=='get_resolution' or cmd=='get_window_size' then return 320,180 end
        if cmd=='get_input_focus' then return 'kag' end
        if cmd=='log' then return end
        error('unexpected platform boundary: '..cmd)
    end,
}
local tokenizer=require('tokenizer')
local scheduler=require('scheduler')
local layers=require('layers')
layers.clear_for_restore()
local tokens=tokenizer.parse('[bg storage="scene-A.png"][bg storage="scene-B.png"][trans method="crossfade" time="32"][eval exp="f.sentinel=1"]')
local ctx={f={},sf={},tf={},mp={},variables={},layers={},backlog={},active_operations={},
    macros={},call_stack={},tokens=tokens,token_index=1,stop_flag=false}
local co=coroutine.create(function() scheduler.run(ctx,tokens) end)
local flash=false
for step=1,16 do
    if coroutine.status(co)=='dead' then break end
    hw.pending='BLACK'
    local ok,err=coroutine.resume(co,16)
    assert(ok,err)
    layers.render()
    hw.frame=hw.frame+1
    hw.scene=hw.pending
    local display=hw.scene
    if hw.overlay then
        display=hw.overlay.t<=0 and hw.overlay.from or
            (hw.overlay.t>=1 and hw.overlay.to or ('MIX:'..hw.overlay.from..':'..hw.overlay.to))
    end
    if display=='B' and #hw.submits==0 then flash=true end
    hw.frames[#hw.frames+1]={scene=hw.scene,display=display,index=ctx.token_index}
    print(string.format('FRAME frame=%d token=%d scene=%s display=%s',hw.frame,ctx.token_index,hw.scene,display))
    hw.overlay=nil
end
local failures=0
local function check(name,value)
    print((value and 'PASS ' or 'FAIL ')..name)
    if not value then failures=failures+1 end
end
check('actual scheduler reaches sentinel',coroutine.status(co)=='dead' and ctx.f.sentinel==1)
check('source is actual previous A',#hw.captures==2 and hw.captures[1].scene=='A')
check('destination is newer actual B',#hw.captures==2 and hw.captures[2].scene=='B' and hw.captures[2].frame>hw.captures[1].frame)
check('no B presentation before first transition submission',not flash)
check('source and destination differ',#hw.captures==2 and hw.captures[1].scene~=hw.captures[2].scene)
local last=hw.submits[#hw.submits]
check('final transition submission is A to B at one',last and last.from=='A' and last.to=='B' and last.t==1)
check('no operation remains',#ctx.active_operations==0)
require('transition').clear(ctx)
print(string.format('SCHEDULER_TRANS_RESULT frames=%d captures=%d failures=%d',#hw.frames,#hw.captures,failures))

local function reset(script)
    require('transition').clear()
    layers.clear_for_restore()
    hw.frame,hw.scene,hw.pending,hw.overlay=0,'BLACK','BLACK',nil
    hw.leases,hw.captures,hw.submits,hw.frames,hw.destroyed={},{},{},{},{}
    hw.failLoad=false
    local ts=tokenizer.parse(script)
    local c={f={},sf={},tf={},mp={},variables={},layers={},backlog={},active_operations={},
        macros={},call_stack={},tokens=ts,token_index=1,stop_flag=false}
    return c,coroutine.create(function() scheduler.run(c,ts) end)
end
local function frame(c,thread)
    hw.pending='BLACK'
    local ok,err=coroutine.resume(thread,16);assert(ok,err)
    layers.render()
    hw.frame=hw.frame+1;hw.scene=hw.pending
    local visible=hw.overlay and hw.overlay.t==0 and hw.overlay.from or hw.scene
    print(string.format('CONTROL_FRAME frame=%d token=%d scene=%s display=%s',hw.frame,c.token_index,hw.scene,visible))
    hw.overlay=nil
    return visible
end
local pair='[bg storage="scene-A.png"][bg storage="scene-B.png"][trans time="32"][eval exp="f.sentinel=1"]'
for _,mode in ipairs({'cancel','close','scene-replacement'}) do
    local c,thread=reset(pair)
    frame(c,thread);frame(c,thread)
    local p=c._pending_transition
    check(mode..' starts with an owned A lease',p~=nil and #hw.captures==1 and hw.captures[1].scene=='A')
    local id=p and p.from
    if mode=='cancel' then require('kag.operation').cancel_all(c);coroutine.close(thread)
    elseif mode=='close' then coroutine.close(thread)
    else c.current_scene='replacement.ks';c.stop_flag=true;frame(c,thread) end
    local count=0;for _,destroyed in ipairs(hw.destroyed) do if destroyed==id then count=count+1 end end
    check(mode..' releases once and removes operation',id and count==1 and c._pending_transition==nil and #c.active_operations==0)
    check(mode..' cannot capture destination',#hw.captures==1)
end
-- A normal visual command without an adjacent trans must remain immediate.
local c,thread=reset('[bg storage="scene-A.png"][bg storage="scene-B.png"][eval exp="f.sentinel=1"]')
frame(c,thread);local ordinary=frame(c,thread)
for _=1,4 do if coroutine.status(thread)=='dead' then break end;frame(c,thread) end
check('no-trans route has no hidden lease or hold',#hw.captures==0 and ordinary=='B' and c.f.sentinel==1)
-- Before the first-ever mutation, render a real initial frame rather than an
-- allocated empty texture; the initial source is genuinely BLACK here.
c,thread=reset('[bg storage="scene-B.png"][trans time="32"][eval exp="f.sentinel=1"]')
local initial=frame(c,thread)
check('first frame waits before mutating B',initial=='BLACK' and layers.find('bg')==nil and #hw.captures==0)
for _=1,10 do if coroutine.status(thread)=='dead' then break end;frame(c,thread) end
check('first-frame transition captures real prior BLACK then B',#hw.captures==2 and hw.captures[1].scene=='BLACK' and hw.captures[1].frame==1 and hw.captures[2].scene=='B' and c.f.sentinel==1)
-- Actual bg handler failure after preparation must retire the pending source.
c,thread=reset(pair);frame(c,thread);hw.failLoad=true;frame(c,thread)
check('mutation error is visible and leaves no pending owner',c._command_error==true and c._pending_transition==nil and #c.active_operations==0 and coroutine.status(thread)=='dead')
local id=hw.captures[1] and hw.captures[1].id
local count=0;for _,destroyed in ipairs(hw.destroyed) do if destroyed==id then count=count+1 end end
check('mutation error releases exact captured lease',id and count==1)
-- Exercise the published spellings through the real compiler/schema/scheduler.
for _,case in ipairs({
    {'method="wipe"',2}, {'type="wipe"',2}, {'kind="wipe"',2}, {'type="fade"',0},
    {'method="crossfade" type="wipe" kind="rule"',0}, {'type="crossfade" kind="wipe"',0},
    {'method="wipe" type="fade" kind="crossfade"',2}, {'',0},
}) do
    c,thread=reset('[bg storage="scene-A.png"][bg storage="scene-B.png"][trans time="32" '..case[1]..'][eval exp="f.sentinel=1"]')
    for _=1,12 do if coroutine.status(thread)=='dead' then break end;frame(c,thread) end
    local submitted=hw.submits[#hw.submits]
    check('scheduler method precedence '..case[1],submitted and submitted.method==case[2]
        and submitted.from=='A' and submitted.to=='B' and submitted.t==1 and c.f.sentinel==1)
end
require('transition').clear(c)
layers.clear_for_restore()
-- Explicit alias duration must survive schema defaults. Derive elapsed frames
-- from real submitted progress, not from a replacement handler or loop budget.
for _,case in ipairs({{'time="48"',48},{'duration="96"',96},
    {'time="64" duration="128"',64},{'',500},{'duration="0"',0},{'time="0" duration="96"',0}}) do
    c,thread=reset('[bg storage="scene-A.png"][bg storage="scene-B.png"][trans '..case[1]..'][eval exp="f.sentinel=1"]')
    for _=1,80 do if coroutine.status(thread)=='dead' then break end;frame(c,thread) end
    local first,steps
    steps=0
    for _,submit in ipairs(hw.submits) do
        if submit.from=='A' and submit.to=='B' and submit.t>0 then
            first=first or submit.t;steps=steps+1
        end
    end
    if case[2]==0 then
        check('scheduler duration contract '..case[1],first==nil and steps==0
            and #hw.captures==0 and c.f.sentinel==1)
    else
        check('scheduler duration contract '..case[1],first and math.abs(first-16/case[2])<1e-9
            and steps==math.ceil(case[2]/16) and c.f.sentinel==1)
    end
end
require('transition').clear(c)
layers.clear_for_restore()
_G._CAESURA_BACKEND=previous_backend
print(string.format('SCHEDULER_TRANS_ALL_RESULT failures=%d',failures))
if failures>0 then os.exit(1) end
