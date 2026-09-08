-- Real runner click history, layer tree and text submissions. Only native
-- bindings and scene IO are replaced; this isolated test owns its whole VM.
package.path = 'scripts/?.lua;scripts/?/init.lua;' .. package.path
local function callable(values)
    return setmetatable(values or {}, {__index=function(self,key)
        local fn=function() return true end
        rawset(self,key,fn)
        return fn
    end})
end
_G.KAG=callable({is_voice_playing=function() return false end,
    is_bgm_playing=function() return false end,get_active_voices=function() return 0 end})
_G.Engine,_G.Render,_G.DevCore=callable(),callable(),callable()
local copy=require('kag.save_state').copy
local encode=require('kag.compiler').encode_lua_literal
local backend=require('backend')
local image_tickets,font_tickets,texture_sources,trace={},{},{},{}
local next_texture,next_rt,gpu_calls=100,10,0
local image_failure,font_prepare_failure,font_apply_failure,upload_failure
local voice_stop_failure
local prepare_hook,prepare_hook_path,cleanup_failure
local active_font={version=1,active=true,font=2,path='assets/a.ttf',size=24}
local cancels,ai_cancels,voice_stops,cleanups,completions=0,0,0,0,0
local old_token
local function ticket(list,source)
    local value={source=copy(source),used=false,discards=0,applications=0}
    list[#list+1]=value
    return value
end
local function discard(value)
    value.discards=value.discards+1
    assert(value.discards==1,'duplicate ticket discard')
    value.used=true
end
backend.get_resolution=function() return 1280,720 end
backend.create_viewport=function() gpu_calls=gpu_calls+1; next_rt=next_rt+1; return next_rt end
backend.destroy_viewport=function() gpu_calls=gpu_calls+1 end
backend.destroy_texture=function(id) gpu_calls=gpu_calls+1; texture_sources[id]=nil end
backend.is_valid_handle=function(_,id) return texture_sources[id]~=nil end
backend.cancel_async_loads=function() cancels=cancels+1 end
backend.ai_cancel=function() ai_cancels=ai_cancels+1 end
backend.audio_stop=function(channel)
    if channel=='voice' then
        voice_stops=voice_stops+1
        if voice_stop_failure=='throw' then error('injected voice stop failure') end
        if voice_stop_failure=='false' then return false end
    end
end
backend.render_text=function(...) trace[#trace+1]={kind='text',values={...}} end
backend.submit_batch=function(batch)
    for i=1,batch[1] do
        local base=1+(i-1)*16
        local record={kind='layer',source=copy(texture_sources[batch[base+2]])}
        for offset=4,16 do record[offset-3]=batch[base+offset] end
        trace[#trace+1]=record
    end
end
_G.Restore={
    describe_texture=function(id) return texture_sources[id] end,
    prepare_image=function(path)
        if prepare_hook and (not prepare_hook_path or prepare_hook_path==path) then
            local hook=prepare_hook; prepare_hook=nil; hook()
        end
        if path==image_failure then return nil,'injected image prepare failure' end
        return ticket(image_tickets,{kind='asset',path=path})
    end,
    prepare_color=function(r,g,b,a) return ticket(image_tickets,{kind='color',r=r,g=g,b=b,a=a}) end,
    image_info=function() return 2,2 end,
    discard_image=discard,
    materialize_image=function(value)
        assert(not value.used,'image ticket reused')
        value.used=true; value.applications=value.applications+1; gpu_calls=gpu_calls+1
        if upload_failure then return nil,'injected image apply failure' end
        next_texture=next_texture+1; texture_sources[next_texture]=copy(value.source)
        return next_texture
    end,
    capture_font=function() return copy(active_font) end,
    default_font=function() return {version=1,active=true,font=0,path='',size=16} end,
    prepare_font=function(value)
        if font_prepare_failure then return nil,'injected font prepare failure' end
        return ticket(font_tickets,value)
    end,
    discard_font=discard,
    apply_font=function(value)
        assert(not value.used,'font ticket reused')
        value.used=true; value.applications=value.applications+1
        if font_apply_failure then return false,'injected font apply failure' end
        active_font=copy(value.source); return true
    end,
    clear_font=function() active_font={version=1,active=false}; return true end,
}
local Layers=require('layers')
local Text=require('kag.text_scene')
local flow=require('flow')
local scene_sources={
    ['transaction-scope.ks']=[[
[u12_transaction_picture phase="A"]
[set var="f.value" value=10]
[ch text="ALPHA"]
[set var="f.value" value=20]
[u12_transaction_scope]
[set var="f.continued" value=1]
[ch text="CHARLIE"]
]],
    ['transaction-inline.ks']=[[
[u12_transaction_picture phase="A"]
[set var="f.value" value=10]
[ch text="ALPHA"]
[u12_transaction_picture phase="B"]
[set var="f.value" value=20]
[rollback]
[set var="f.leaked" value=1]
[ch text="UNREACHABLE"]
]],
    ['transaction-new.ks']=[[
[u12_transaction_picture phase="NEW"]
[set var="f.value" value=99]
[ch text="NEW OWNER"]
]],
}
flow.load_scene=function(path)
    local tokens=require('tokenizer').parse(assert(scene_sources[path],'unknown fixture scene'))
    require('kag.compiler').compile(tokens)
    return {tokens=tokens,labels=tokens._compiled.labels,path=path,base_path=path}
end
flow.reload_scene=flow.load_scene
local runner=require('kag_runner')
local Operation=require('kag.operation')
local commands=require('kag')
local function install_picture(phase)
    assert(Layers.clear_for_restore())
    local parent=Layers.add_layer(nil,{id='parent',x=10,y=20,z=1})
    for i=1,2 do
        local node=Layers.add_layer(parent,{id='image'..i,x=i*10,y=i*5,w=120,h=80,z=i})
        next_texture=next_texture+1
        texture_sources[next_texture]={kind='asset',path='assets/'..phase..i..'.bmp'}
        node.tex=next_texture
    end
    active_font={version=1,active=true,font=2,path='assets/'..phase..'.ttf',size=24}
end
commands.u12_transaction_picture=function(_,params) install_picture(params.phase) end
commands.u12_transaction_scope=function(owner)
    local scope <close> = Operation.start(owner)
    local cleanup_guard <close> = setmetatable({}, {__close=function()
        if cleanup_failure then error('injected old cleanup failure') end
    end})
    old_token=scope.token
    scope.token:register(function() cleanups=cleanups+1 end)
    install_picture('B')
    Text.clear(owner); Text.add_text(owner,'BRAVO',32,580,{255,255,255,255})
    owner.reveal=nil
    owner.waiting_input=true
    coroutine.yield()
    completions=completions+1
    scope:complete()
end
local passed,failed=0,0
local function check(label,ok)
    if ok then passed=passed+1 else failed=failed+1 end
    print((ok and 'PASS ' or 'FAIL ')..label)
    return ok
end
local function run(label,fn)
    print('CASE '..label)
    local ok,err=pcall(fn)
    if not ok then failed=failed+1; print('FAIL '..label..': '..tostring(err)) end
    image_failure,font_prepare_failure,font_apply_failure,upload_failure=nil,false,false,false
    voice_stop_failure=nil
    prepare_hook,prepare_hook_path,cleanup_failure=nil,nil,false
    runner.stop()
end
local function picture(owner)
    trace={}; Layers.render(); Text.render(owner)
    return encode({draws=trace,font=active_font})
end
local function page(owner)
    local texts={}
    Text.render(owner,{render_text=function(text) texts[#texts+1]=text end})
    return table.concat(texts,'|')
end
local function reach_wait()
    for _=1,32 do
        local owner=runner.get_ctx()
        if owner and owner.waiting_input and not owner._pendingRollback then return true end
        runner.update(0)
    end
    return false
end
local function reveal()
    local owner=runner.get_ctx()
    if owner and owner.reveal and Text.get_state(owner).reveal_chars<owner.reveal.total then
        return runner.on_click()
    end
    return true
end
local function start(name)
    assert(runner.start('transaction-'..name..'.ks'))
    assert(reach_wait(),'fixture did not reach first wait')
    reveal()
    return runner.get_ctx()
end
local function advance()
    reveal()
    local ok=runner.on_click()
    return ok and reach_wait()
end
local function historical_scope()
    local owner=start('scope')
    local previous=picture(owner)
    check('ALPHA has no synthetic history',page(owner)=='ALPHA' and #owner._undoStack==0)
    check('real click creates one historical checkpoint',advance() and #owner._undoStack==1)
    check('BRAVO suspends a live Operation',page(owner)=='BRAVO' and #owner.active_operations==1
        and not old_token.cancelled and coroutine.status(owner.co)=='suspended')
    return owner,previous
end
local function all_tickets_settled()
    for _,list in ipairs({image_tickets,font_tickets}) do
        for _,value in ipairs(list) do
            if not value.used or value.discards>1 or value.applications>1 then return false end
        end
    end
    return true
end

run('instant click reveal remains complete on later frames',function()
    assert(runner.start('transaction-new.ks'))
    assert(reach_wait())
    local owner=runner.get_ctx()
    check('fresh line begins with an incomplete typewriter',owner.reveal~=nil
        and Text.get_state(owner).reveal_chars<owner.reveal.total)
    local ok,reason=runner.on_click()
    check('first click seals both displayed text and reveal clock',ok==true and reason=='revealed'
        and page(owner)=='NEW OWNER' and owner.reveal.elapsed>=owner.reveal.total*(owner.text_speed or 50)
        and #owner._undoStack==0)
    local co=owner.co
    runner.update(0)
    check('zero-delta frame cannot hide instantly revealed text',page(owner)=='NEW OWNER'
        and owner.waiting_input and owner.co==co and #owner._undoStack==0)
    runner.update(0.016)
    check('positive-delta frame preserves full text and click wait',page(owner)=='NEW OWNER'
        and owner.waiting_input and owner.co==co and #owner._undoStack==0)
end)

for _,kind in ipairs({'image','font'}) do
    run(kind..' preparation preserves original execution',function()
        local owner=historical_scope()
        local co,values,history,operations=owner.co,owner.f,owner._undoStack,owner.active_operations
        local top,token,node=history[#history],old_token,Layers.get_layer('image1')
        local before,gpu,voice=picture(owner),gpu_calls,voice_stops
        local cancel,ai,clean,completed=cancels,ai_cancels,cleanups,completions
        local images=#image_tickets
        if kind=='image' then image_failure='assets/A2.bmp' else font_prepare_failure=true end
        local called,ok,reason=pcall(runner.rollback)
        check(kind..' reports preparation failure',called and ok==false
            and tostring(reason):find('prepare failure',1,true))
        check(kind..' retains owner and coroutine identity',runner.get_ctx()==owner
            and owner.co==co and coroutine.status(co)=='suspended')
        check(kind..' retains live values and history identity',owner.f==values and owner.f.value==20
            and owner._undoStack==history and #history==1 and history[1]==top)
        check(kind..' retains actual render and live node',picture(owner)==before
            and Layers.get_layer('image1')==node and gpu_calls==gpu and voice_stops==voice)
        check(kind..' leaves operations and cancellation untouched',owner.active_operations==operations
            and operations[1]==token and not token.cancelled and cancels==cancel and ai_cancels==ai
            and cleanups==clean and completions==completed and owner.waiting_input and not owner.stop_flag)
        check(kind..' releases prepared image tickets',#image_tickets>images and all_tickets_settled())
        image_failure,font_prepare_failure=nil,false
        check(kind..' original coroutine continues on next click',advance() and reveal()
            and page(owner)=='CHARLIE' and owner.f.continued==1 and completions==completed+1
            and cleanups==clean and not token.cancelled)
    end)
end

for _,kind in ipairs({'image','font'}) do
    run(kind..' application failure retires execution safely',function()
        local owner=historical_scope()
        local co,token,depth=owner.co,old_token,#owner._undoStack
        if kind=='image' then upload_failure=true else font_apply_failure=true end
        local called,ok,reason=pcall(runner.rollback)
        check(kind..' reports application failure',called and ok==false
            and tostring(reason):find('apply failure',1,true))
        check(kind..' old execution cannot revive',coroutine.status(co)=='dead'
            and token.cancelled and owner.co==nil and owner._session_active==false)
        check(kind..' failed commit consumes no checkpoint',#owner._undoStack==depth)
        check(kind..' failed commit has no partial picture or active font',Layers.count()==1
            and next(owner._restoredTextures or {})==nil and active_font.active==false)
        check(kind..' failed commit releases all tickets',all_tickets_settled())
        runner.update(1)
        check(kind..' failed commit cannot continue old script',owner.f.continued==nil
            and owner._pendingRollback==nil)
        upload_failure,font_apply_failure=false,false
        local fresh=start('new')
        check(kind..' explicit start recovers a fresh session',fresh~=owner and page(fresh)=='NEW OWNER'
            and fresh.f.value==99 and active_font.active and all_tickets_settled())
    end)
end

for _,kind in ipairs({'throw','false'}) do
    run('voice stop '..kind..' fails the rollback commit safely',function()
        local owner=historical_scope()
        local co,token,depth=owner.co,old_token,#owner._undoStack
        voice_stop_failure=kind
        local called,ok,reason=pcall(runner.rollback)
        check(kind..' voice stop rejects rollback with an explicit error',called and ok==false
            and type(reason)=='string' and #reason>0)
        check(kind..' voice stop failure closes old coroutine and Operation',coroutine.status(co)=='dead'
            and token.cancelled and owner.co==nil and owner._session_active==false)
        check(kind..' voice stop failure does not consume a checkpoint',#owner._undoStack==depth)
        local published=runner.get_ctx()
        check(kind..' voice stop failure publishes no active mixed session',published==nil
            or (published._session_active==false and published.co==nil))
        check(kind..' voice stop failure clears partial presentation and candidates',Layers.count()==1
            and next(owner._restoredTextures or {})==nil and active_font.active==false
            and all_tickets_settled())
        runner.update(1)
        check(kind..' voice stop failure cannot execute the old continuation',owner.f.continued==nil
            and owner._pendingRollback==nil)
        voice_stop_failure=nil
        local started=runner.start('transaction-new.ks')
        if started then reach_wait(); reveal() end
        local fresh=runner.get_ctx()
        check(kind..' explicit start recovers after voice stop failure',started and fresh~=owner and fresh.f.value==99
            and page(fresh)=='NEW OWNER' and all_tickets_settled())
    end)
end

run('successful rollback clears an already staged reload',function()
    local owner,previous=historical_scope()
    local co=owner.co
    check('public reload stages a new continuation',runner.reload_scene()==true
        and owner._pendingSceneReload==true and coroutine.status(co)=='dead')
    check('rollback accepts the earlier real click point',runner.rollback()==true)
    check('rollback restores exact historical submissions',picture(owner)==previous and owner.f.value==10)
    check('success clears every stale continuation',owner._pendingSceneReload==nil
        and not owner._scene_changed and owner._pendingJump==nil and owner._pendingLoadScene==nil
        and owner._pendingLoadToken==nil and owner._pendingRollback==nil)
    check('success consumes exactly one checkpoint',#owner._undoStack==0)
    runner.update(1)
    check('next frame remains on the historical page',picture(owner)==previous and page(owner)=='ALPHA'
        and owner.f.value==10 and owner.waiting_input)
    check('same checkpoint cannot be consumed twice',runner.rollback()==false and #owner._undoStack==0)
    check('manual continuation executes normally after reload rollback',advance() and page(owner)=='BRAVO'
        and owner.f.value==20 and #owner._undoStack==1)
end)

run('reentrant preparation rejects a replaced owner',function()
    local owner=historical_scope()
    local co,top=owner.co,owner._undoStack[1]
    local fresh,new_picture,stopped,started
    local cancel_after_start,ai_after_start
    prepare_hook=function()
        stopped=runner.stop()
        started=runner.start('transaction-new.ks')
        if stopped and started then
            reach_wait(); reveal()
            fresh=runner.get_ctx(); new_picture=picture(fresh)
            cancel_after_start,ai_after_start=cancels,ai_cancels
        end
    end
    local called,ok,reason=pcall(runner.rollback)
    check('prepare hook can legally stop and start',stopped==true and started==true and fresh~=owner)
    check('replaced owner rejects outer rollback',called and ok==false
        and tostring(reason):find('expired',1,true))
    check('replacement does not consume old history',#owner._undoStack==1 and owner._undoStack[1]==top)
    check('replacement retires old execution',coroutine.status(co)=='dead')
    check('expired candidate cannot mutate or cancel fresh owner',fresh~=nil and runner.get_ctx()==fresh
        and page(fresh)=='NEW OWNER' and fresh.f.value==99 and picture(fresh)==new_picture
        and cancels==cancel_after_start and ai_cancels==ai_after_start)
    check('expired owner candidate releases acquired resources',all_tickets_settled())
end)

run('yielded preparation rejects a replaced owner after resumption',function()
    local owner=historical_scope()
    local images=#image_tickets
    prepare_hook_path='assets/A2.bmp'
    prepare_hook=function() coroutine.yield('image-prepare-barrier') end
    local worker=coroutine.create(function() return runner.rollback() end)
    local yielded,barrier=coroutine.resume(worker)
    check('prepare yields after owning one candidate image',yielded and barrier=='image-prepare-barrier'
        and #image_tickets==images+1 and not image_tickets[images+1].used)
    local stopped=runner.stop()
    local started=runner.start('transaction-new.ks')
    check('host can replace a session while prepare is suspended',stopped==true and started==true)
    local fresh=runner.get_ctx()
    reach_wait(); reveal()
    local before=picture(fresh)
    local resumed,ok,reason=coroutine.resume(worker)
    check('resumed preparation rejects expired owner',resumed and ok==false
        and tostring(reason):find('expired',1,true) and coroutine.status(worker)=='dead')
    check('yielded candidate cannot install into replacement',fresh~=owner and runner.get_ctx()==fresh
        and fresh.f.value==99 and page(fresh)=='NEW OWNER' and picture(fresh)==before)
    check('yielded expired candidate releases earlier and later tickets',all_tickets_settled())
end)

run('reentrant click expires a historical top without replacing its owner',function()
    local owner=historical_scope()
    local co,top=owner.co,owner._undoStack[1]
    local advanced,current_picture,original_cancels,original_ai
    prepare_hook=function()
        advanced=advance()
        reveal()
        current_picture=picture(owner)
        original_cancels,original_ai=cancels,ai_cancels
    end
    local called,ok,reason=pcall(runner.rollback)
    check('prepare hook advances the same real scheduler',advanced==true and page(owner)=='CHARLIE')
    check('expired historical top rejects outer rollback',called and ok==false
        and tostring(reason):find('expired',1,true))
    check('top expiry keeps both real click points',#owner._undoStack==2 and owner._undoStack[1]==top
        and owner._undoStack[2]~=top)
    check('top expiry preserves current execution and presentation',runner.get_ctx()==owner
        and owner.co==co and coroutine.status(co)=='suspended' and owner.f.continued==1
        and picture(owner)==current_picture and cancels==original_cancels and ai_cancels==original_ai)
    check('top expiry releases all prepared resources',all_tickets_settled())
end)

local function inline_pending()
    local owner=start('inline')
    local images,fonts=#image_tickets,#font_tickets
    check('inline begins with no synthetic checkpoint',#owner._undoStack==0 and page(owner)=='ALPHA')
    check('inline rollback stages at the host boundary',runner.on_click()==true
        and owner._pendingRollback~=nil and owner.f.value==20 and #owner._undoStack==1)
    check('pending inline rollback owns prepared resources',#image_tickets==images+2
        and #font_tickets==fonts+1 and not image_tickets[images+1].used
        and not font_tickets[fonts+1].used)
    return owner,images,fonts
end
local function pending_discarded(images,fonts)
    for i=images+1,images+2 do
        local value=image_tickets[i]
        if not value or value.discards~=1 or value.applications~=0 then return false end
    end
    local font=font_tickets[fonts+1]
    return font and font.discards==1 and font.applications==0
end
run('stopping inline pending rollback releases its candidate',function()
    local owner,images,fonts=inline_pending()
    local co=owner.co
    check('stop succeeds while inline candidate is pending',runner.stop()==true
        and owner._pendingRollback==nil and coroutine.status(co)=='dead')
    check('stop releases candidate tickets without application',pending_discarded(images,fonts))
    local fresh=start('new')
    local before=picture(fresh)
    runner.update(1)
    check('stopped inline candidate never submits on the next owner',picture(fresh)==before
        and fresh.f.value==99 and owner.f.leaked==nil and all_tickets_settled())
end)

run('save restoration supersedes inline pending rollback',function()
    local saved_owner=start('new')
    local saved=copy(require('kag.commands.save').capture_state(saved_owner))
    local saved_picture=picture(saved_owner)
    assert(runner.stop())
    local owner,images,fonts=inline_pending()
    local co=owner.co
    local prepared=require('kag.save_state').prepare(saved,function(path)
        return scene_sources[path]~=nil
    end,flow.load_scene)
    check('public restore replaces inline pending owner',runner.restore_candidate(owner,prepared)==true
        and runner.get_ctx()~=owner and coroutine.status(co)=='dead')
    check('save restoration discards old rollback tickets',pending_discarded(images,fonts)
        and owner._pendingRollback==nil)
    local fresh=runner.get_ctx()
    check('save restoration installs its own visible checkpoint',fresh.f.value==99
        and page(fresh)=='NEW OWNER' and picture(fresh)==saved_picture and all_tickets_settled())
    runner.update(0)
    if page(fresh)~='NEW OWNER' then
        print('RESTORE DIAGNOSTIC '..encode({page=page(fresh),same_owner=runner.get_ctx()==fresh,
            value=fresh.f.value,waiting=fresh.waiting_input,reveal=fresh.reveal,
            text_reveal=Text.get_state(fresh).reveal_chars,leaked=owner.f.leaked}))
    end
    check('superseded rollback cannot overwrite restored owner',runner.get_ctx()==fresh
        and fresh.f.value==99 and page(fresh)=='NEW OWNER' and owner.f.leaked==nil)
end)

run('throwing old cleanup cannot revive its coroutine',function()
    local owner=historical_scope()
    local co,token,top,clean=owner.co,old_token,owner._undoStack[1],cleanups
    cleanup_failure=true
    local called,ok,reason=pcall(runner.rollback)
    check('old cleanup failure returns a recoverable error',called and ok==false
        and tostring(reason):find('old cleanup failure',1,true))
    check('throwing cleanup still retires old operation once',coroutine.status(co)=='dead'
        and owner.co==nil and owner._session_active==false and token.cancelled and cleanups==clean+1)
    check('throwing cleanup retains history and discards candidate',#owner._undoStack==1
        and owner._undoStack[1]==top and all_tickets_settled())
    runner.update(1)
    check('throwing cleanup cannot resume old continuation',owner.f.continued==nil
        and owner._pendingRollback==nil and cleanups==clean+1)
    cleanup_failure=false
    local fresh=start('new')
    check('explicit start recovers after throwing cleanup',fresh~=owner and fresh.f.value==99
        and page(fresh)=='NEW OWNER' and all_tickets_settled())
end)

for _,terminal in ipairs({'stop','commit'}) do
    run('nested inline preparation preserves pending ownership through '..terminal,function()
        local owner=start('inline')
        local previous=picture(owner)
        local images,fonts=#image_tickets,#font_tickets
        local accepted,reason,inner_request
        prepare_hook=function()
            accepted,reason=runner.rollback()
            inner_request=owner._pendingRollback
        end
        check(terminal..' nested rollback publishes a real pending candidate',runner.on_click()==true
            and accepted==true and reason=='rollback-pending' and inner_request~=nil)
        check(terminal..' outer prepare retains the already published candidate',owner._pendingRollback==inner_request
            and #image_tickets==images+4 and #font_tickets==fonts+2)
        local a,b=image_tickets[images+3],image_tickets[images+4]
        local font=font_tickets[fonts+2]
        check(terminal..' outer candidate discards without applying',a and b and font
            and a.discards==1 and b.discards==1 and font.discards==1
            and a.applications==0 and b.applications==0 and font.applications==0)
        check(terminal..' pending inner resources remain reserved',image_tickets[images+1]
            and not image_tickets[images+1].used and not image_tickets[images+2].used
            and not font_tickets[fonts+1].used)
        local co=owner.co
        if terminal=='stop' then
            check('stop releases the retained inner candidate',runner.stop()==true
                and pending_discarded(images,fonts) and coroutine.status(co)=='dead')
        else
            check('host commits its own retained pending request',runner.update(0)==true
                and picture(owner)==previous and page(owner)=='ALPHA' and owner.f.value==10
                and owner._pendingRollback==nil and #owner._undoStack==0 and coroutine.status(co)=='dead')
            check('retained inner tickets apply once',image_tickets[images+1].applications==1
                and image_tickets[images+2].applications==1 and font_tickets[fonts+1].applications==1)
            runner.update(0)
            check('nested commit cannot replay or consume twice',picture(owner)==previous
                and owner.f.leaked==nil and runner.rollback()==false)
        end
        check(terminal..' all nested candidates are settled',all_tickets_settled())
    end)
end

run('restore staged during rollback preparation retains ownership',function()
    local saved_owner=start('new')
    local saved=copy(require('kag.commands.save').capture_state(saved_owner))
    local previous=picture(saved_owner)
    assert(runner.stop())
    local owner=start('inline')
    local prepared=require('kag.save_state').prepare(saved,function(path)
        return scene_sources[path]~=nil
    end,flow.load_scene)
    local images,fonts=#image_tickets,#font_tickets
    local accepted,reason
    prepare_hook=function() accepted,reason=runner.restore_candidate(owner,prepared) end
    check('nested restore publishes its candidate during inline prepare',runner.on_click()==true
        and accepted==true and reason=='restore-pending' and owner._pendingRestore==prepared)
    local co=owner.co
    check('host commits staged restore before inline rollback',runner.update(0)==true
        and runner.get_ctx()~=owner and coroutine.status(co)=='dead')
    local fresh=runner.get_ctx()
    check('nested restore releases every unused rollback ticket',pending_discarded(images,fonts)
        and all_tickets_settled() and owner._pendingRollback==nil and owner._pendingRestore==nil)
    check('nested restore remains the authoritative visible owner',fresh.f.value==99
        and page(fresh)=='NEW OWNER' and picture(fresh)==previous and owner.f.leaked==nil)
end)

check('all acquired tickets were consumed or released',all_tickets_settled())
print(string.format('U12 ROLLBACK TRANSACTION: %d passed, %d failed',passed,failed))
assert(failed==0,'rollback transaction regression')
