package.path="scripts/?.lua;scripts/?/init.lua;"..package.path
local T=require('kag.transient_state')
local passed,failed=0,0
local function check(name,value)
    if value then passed=passed+1;print('PASS '..name)
    else failed=failed+1;print('FAIL '..name) end
end
local function accepts(ctx) return pcall(T.assert_saveable,ctx) end
local stable={f={value=1},call_stack={{tokens={}}},_forStack={{}},waiting_input=true,reveal={total=3}}
check('stable text and call/control state stay supported',accepts(stable))
check('capture preserves values',stable.f.value==1)
check('wait within a caller remains supported',accepts({_executing_command='wait',active_operations={{cancelled=false}}}))
for _,value in ipairs({
    {_choiceMode=true},{_choiceButtons={{}}},{_selectedChoice=1},{_inputMode=true},
    {_settingsActive=true},{galleryState={active=true}},{input_focus='history'},
    {_videoPlayback={handle=1,closed=false}},{live2d={model={}}},{macros={m={}}},
    {tokens={_runtime_rewritten=true}},{call_stack={{tokens={_runtime_rewritten=true}}}},
    {tweens={{done=false,cancelled=false}}},{_executing_command='trans',active_operations={{cancelled=false}}},
}) do check('unfinished state is rejected',not accepts(value)) end
check('finished effects are not rejected',accepts({tweens={{done=true}},_videoPlayback={closed=true}}))
local counts={videos=0,models=0,particles=0,emitters=0,postfx=0}
local stops=0
Restore={capture_transients=function() return counts end,stop_transients=function() stops=stops+1;return true end}
for _,field in ipairs({'videos','models','particles','emitters','postfx'}) do
    counts[field]=1
    check('backend '..field..' prevents lossy capture',not accepts({}))
    counts[field]=0
end
check('capture never stops resources',stops==0)
check('empty backends permit capture',accepts({}))
counts.models='unknown'
check('invalid backend status rejected',not accepts({}))
counts.models=0
local indexes=0
package.loaded['kag.commands.vfx']={_clear_runtime_state=function(ctx) indexes=indexes+1;ctx._weatherEmitters={} end}
local owner={_weatherEmitters={rain=3}}
check('stop clears backend state',T.stop(owner)==true and stops==1)
check('stop clears private weather indexes',indexes==1 and next(owner._weatherEmitters)==nil)
Restore.stop_transients=function() error('backend cleanup failed') end
local ok,reason=T.stop(owner)
check('cleanup errors are observable',ok==false and tostring(reason):find('backend cleanup failed',1,true))
check('index cleanup still runs after backend failure',indexes==2)

-- Exercise the real command paths: fabricated ctx flags miss stale indexes
-- and effects whose coroutine never registered an Operation.
local emitters,destroyed,next_emitter={}, {},0
local submitted=0
local scene_blur=0
package.loaded.backend={
    particles_create_emitter=function()
        next_emitter=next_emitter+1;emitters[next_emitter]=true;return next_emitter
    end,
    particles_emit=function() end,
    particles_destroy_emitter=function(id) destroyed[#destroyed+1]=id;emitters[id]=nil;return true end,
    clear_particles=function() emitters={};return true end,
    create_solid_texture=function() return 7 end,
    submit_vfx=function() submitted=submitted+1 end,
    set_scene_blur=function(radius) scene_blur=radius;return true end,
}
package.loaded['kag.commands.vfx']=nil
local commands=require('kag.commands.vfx')
local VFX=require('vfx')
local Operations=require('kag.operation')
local Save=require('kag.commands.save')
local Layers=require('layers')
local targets,next_target={},0
package.loaded.rtt={
    alloc=function() next_target=next_target+1;targets[next_target]=true;return next_target end,
    free=function(id) targets[id]=nil end,
}
Restore.stop_transients=function() return true end
for _,clear in ipairs({'particles','vfx','particle_weather'}) do
    local ctx={}
    commands.particle_weather(ctx,{action='start',type='rain',count=0})
    if clear=='vfx' then commands.vfx(ctx,{type='particle',action='clear'})
    else commands[clear](ctx,{action='clear'}) end
    check(clear..' clear permits saving after weather',next(emitters)==nil and accepts(ctx))
    local before=#destroyed
    commands.particle_weather({}, {action='stop',type='rain'})
    check(clear..' clear removes private weather index',#destroyed==before)
end

local function context(command)
    return {_executing_command=command,_executing_index=2,current_scene='effect.ks',
        f={},sf={},tf={},lf={},mp={},variables={},_bgTexture=9}
end
local effect_cases={
    {'flash',function(ctx) commands.flash(ctx,{time=200}) end},
    {'quake',function(ctx) commands.quake(ctx,{time=200}) end},
    {'shake',function(ctx) commands.shake(ctx,{time=200}) end},
    {'fade',function(ctx) commands.vfx(ctx,{type='fade',time=200}) end},
    {'blur',function(ctx) commands.vfx(ctx,{type='blur',time=200}) end},
    {'fade_layer',function(ctx) VFX.fade_layer(ctx,ctx.effect_layer,{time=200}) end},
    {'blur_layer',function(ctx) VFX.blur_layer(ctx.effect_layer,{time=200},coroutine) end},
}
local function begin_effect(ctx,run)
    _G._CAESURA_CTX=ctx
    ctx.effect_layer=ctx.effect_layer or {alpha=1,texture=9}
    local co=coroutine.create(function() run(ctx) end)
    assert(coroutine.resume(co));assert(coroutine.resume(co,50))
    assert(coroutine.status(co)=='suspended')
    return co
end
local function visible_state(ctx)
    local root=Layers.get_root()
    local values={submitted,scene_blur,ctx._flashLayer and ctx._flashLayer.alpha or -1,
        ctx.effect_layer.alpha,tostring(root.quake.active),root.quake.offset_x or 0,
        tostring(root.shake.active),root.shake.offset_x or 0}
    return table.concat(values,':')
end
for _,effect in ipairs(effect_cases) do
    local name,run=effect[1],effect[2]
    local ctx=context(name)
    local co=begin_effect(ctx,run)
    local captured,err=pcall(Save.capture_state,ctx)
    check(name..' real capture rejects an unfinished effect',not captured
        and tostring(err):find('unfinished',1,true)~=nil)
    for _=1,3 do assert(coroutine.resume(co,50)) end
    if name=='blur' then
        check('blur renders the maximum radius before cleanup',scene_blur==4 and coroutine.status(co)=='suspended')
        assert(coroutine.resume(co,50))
    end
    check(name..' completion preserves duration and permits capture',coroutine.status(co)=='dead'
        and pcall(Save.capture_state,ctx))
    check(name..' completion releases temporary targets',next(targets)==nil)

    local cancelled=context(name)
    local old_co=begin_effect(cancelled,run)
    Operations.cancel_all(cancelled)
    check(name..' cancellation permits capture',pcall(Save.capture_state,cancelled))
    check(name..' cancellation releases temporary targets',next(targets)==nil)
    local new_co=begin_effect(cancelled,run)
    local before=visible_state(cancelled)
    assert(coroutine.resume(old_co,200));assert(coroutine.close(old_co))
    check(name..' cancelled coroutine cannot change the next effect',visible_state(cancelled)==before)
    assert(coroutine.close(new_co))
    check(name..' closing a coroutine permits capture and releases targets',next(targets)==nil
        and pcall(Save.capture_state,cancelled))

    local defaults=context(name)
    defaults.effect_layer={alpha=1,texture=9}
    _G._CAESURA_CTX=defaults
    local default_co=coroutine.create(function() run(defaults) end)
    local resumed=coroutine.resume(default_co)
    for _=1,(name=='blur' and 14 or 13) do
        if resumed and coroutine.status(default_co)~='dead' then resumed=coroutine.resume(default_co) end
    end
    check(name..' omitted frame delta retains the 16ms default',resumed
        and coroutine.status(default_co)=='dead')
    coroutine.close(default_co)
end

for _,index in ipairs({1,2,3,6}) do
    local name,run=effect_cases[index][1],effect_cases[index][2]
    local ctx=context(name)
    local old_co=begin_effect(ctx,run)
    local new_co=begin_effect(ctx,run)
    local before=visible_state(ctx)
    assert(coroutine.close(old_co))
    check(name..' closing the previous owner preserves a newer effect',visible_state(ctx)==before)
    assert(coroutine.close(new_co))
    check(name..' final owner cleanup permits capture',pcall(Save.capture_state,ctx))
end

do
    local backend=package.loaded.backend
    local set_scene_blur=backend.set_scene_blur
    local calls,radius=0,0
    backend.set_scene_blur=function(value)
        calls=calls+1
        if calls==2 then return false end
        radius=value
        return true
    end
    local ctx=context('blur')
    local co=coroutine.create(function() VFX.blur(ctx,{time=200}) end)
    assert(coroutine.resume(co))
    assert(coroutine.resume(co,16))
    check('active blur prevents a lossy save',radius>0 and not pcall(Save.capture_state,ctx))
    local resumed,err=coroutine.resume(co,16)
    check('scene blur propagates an application error',not resumed
        and tostring(err):find('Scene blur could not be applied',1,true)~=nil)
    coroutine.close(co)
    check('failed scene blur clears the already applied effect',radius==0 and next(targets)==nil
        and pcall(Save.capture_state,ctx))
    backend.set_scene_blur=set_scene_blur
end
_G._CAESURA_CTX=nil
print(string.format('TRANSIENT RESTORE: %d passed, %d failed',passed,failed))
os.exit(failed==0 and 0 or 1)
