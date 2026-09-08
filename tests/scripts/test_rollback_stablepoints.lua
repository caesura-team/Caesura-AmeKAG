-- Real script journeys for recoverable text/character state and tween barriers.
-- The only doubles are native host bindings and scene file reads.
package.path='scripts/?.lua;scripts/?/init.lua;'..package.path
local function callable(fields)
    return setmetatable(fields or {},{__index=function(self,key)
        if type(key)~='string' then return nil end
        local fn=function() return true end
        rawset(self,key,fn); return fn
    end})
end
_G.KAG=callable({is_voice_playing=function() return false end,
    is_bgm_playing=function() return false end,get_active_voices=function() return 0 end})
_G.Engine,_G.Render,_G.DevCore=callable(),callable(),callable()
local plate_calls,texture_sources,next_texture={}, {},100
local copy=require('kag.save_state').copy
Render.create_viewport=function() return 1 end
Render.create_solid_texture=function(r,g,b,a)
    next_texture=next_texture+1
    texture_sources[next_texture]={kind='color',r=r,g=g,b=b,a=a}
    return next_texture
end
Render.render_text=function(...) plate_calls[#plate_calls+1]={...} end
KAG.render_text=Render.render_text
_G.Restore={
    describe_texture=function(id) return copy(assert(texture_sources[id])) end,
    prepare_color=function(r,g,b,a) return {r=r,g=g,b=b,a=a} end,
    image_info=function() return 1,1 end,
    discard_image=function() end,
    materialize_image=function(s) return Render.create_solid_texture(s.r,s.g,s.b,s.a) end,
}
local sources={
    ['stablepoints-nameplate.ks']=[[
[nameplate x=32 text_color="255,0,0"]
[ch name="Alice" text="ALPHA"]
[ch name="Alice" text="BRAVO"]
[nameplate x=300 text_color="0,255,0"]
[ch name="Bob" text="CHARLIE"]
[ch text="END"]
]],
    ['stablepoints-speaker.ks']=[[
[ch name="Alice" text="ALPHA"]
[nameplate x=32]
[p]
[ch name="Bob" text="BRAVO"]
[ch text="END"]
]],
    ['stablepoints-nvl-visible.ks']=[[
[textbox]
[nvl]
[ch name="Alice" text="ALPHA"]
[ch name="Alice" text="BRAVO"]
[nvl off]
[ch name="Alice" text="CHARLIE"]
[ch text="END"]
]],
    ['stablepoints-character.ks']=[[
[ch name="Alice" pos="left" text="ALPHA"]
[ch name="Alice" text="BRAVO"]
[ch name="Alice" pos="right" text="CHARLIE"]
[ch text="END"]
]],
    ['stablepoints-nvl.ks']=[[
[nvl prefix="OLD:%s"]
[ch name="Alice" text="ALPHA"]
[ch name="Alice" text="BRAVO"]
[nvl prefix="NEW:%s"]
[ch name="Alice" text="CHARLIE"]
[ch text="END"]
]],
    ['stablepoints-tween.ks']=[[
[ch text="BEFORE"]
[tween target="message" attr="x" to="100" dur="1000" wait="false"]
[ch text="ALPHA"]
[ch text="BRAVO"]
[ch text="END"]
]],
    ['stablepoints-empty-tween.ks']=[[
[ch text="ALPHA"]
[ch text="BRAVO"]
[ch text="END"]
]],
}
local flow=require('flow')
flow.load_scene=function(path)
    local tokens=require('tokenizer').parse(assert(sources[path],'unknown stablepoint fixture'))
    require('kag.compiler').compile(tokens)
    return {tokens=tokens,labels=tokens._compiled.labels,path=path,base_path=path}
end
local runner=require('kag_runner')
local Text=require('kag.text_scene')
local Layers=require('layers')
local Snapshot=require('kag.snapshot')
local encode=require('kag.compiler').encode_lua_literal
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
    runner.stop()
end
local function render_trace(owner)
    local result={}
    Text.render(owner,{render_text=function(...) result[#result+1]={...} end})
    return encode(result)
end
local function shown(owner)
    local result={}
    Text.render(owner,{render_text=function(value) result[#result+1]=value end})
    return table.concat(result)
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
    if owner.reveal and Text.get_state(owner).reveal_chars<owner.reveal.total then
        return runner.on_click()
    end
    return true
end
local function start(name)
    assert(runner.start('stablepoints-'..name..'.ks'))
    assert(reach_wait(),'fixture did not reach first text wait')
    assert(reveal())
    return runner.get_ctx()
end
local function advance()
    reveal()
    local ok=runner.on_click()
    return ok and reach_wait() and reveal()
end
local function message_x() return Layers.get('message').x end

run('character registry replays the original inherited position',function()
    local owner=start('character')
    local alpha=render_trace(owner)
    check('ALPHA establishes Alice on the left with empty history',owner.characters.Alice.pos=='left'
        and #owner._undoStack==0 and shown(owner):find('ALPHA',1,true))
    check('original BRAVO inherits the left position',advance() and owner.characters.Alice.pos=='left'
        and #owner._undoStack==1 and shown(owner):find('BRAVO',1,true))
    local bravo=render_trace(owner)
    local historical=owner._undoStack[1]
    local advanced=advance()
    check('future CHARLIE explicitly moves Alice right',advanced and owner.characters.Alice.pos=='right'
        and #owner._undoStack==2 and shown(owner):find('CHARLIE',1,true))
    check('historical registry remains independent of future character updates',historical.characters.Alice.pos=='left')
    check('first public rollback restores the actual BRAVO checkpoint',runner.rollback()==true
        and render_trace(owner)==bravo and owner.characters.Alice.pos=='left' and #owner._undoStack==1)
    check('second public rollback restores the ALPHA checkpoint',runner.rollback()==true
        and render_trace(owner)==alpha and owner.characters.Alice.pos=='left' and #owner._undoStack==0)
    check('replayed BRAVO has the exact original render submissions',advance() and render_trace(owner)==bravo
        and owner.characters.Alice.pos=='left')
    check('replayed explicit CHARLIE can still change the position',advance()
        and owner.characters.Alice.pos=='right' and shown(owner):find('CHARLIE',1,true))
end)

run('NVL continuation restores the historical prefix and layout',function()
    local owner=start('nvl')
    local alpha=render_trace(owner)
    check('NVL ALPHA visibly uses OLD prefix',owner.nvl_prefix_fmt=='OLD:%s'
        and shown(owner):find('OLD:Alice',1,true))
    check('original NVL BRAVO appends under OLD prefix',advance()
        and shown(owner):find('BRAVO',1,true) and #owner._undoStack==1)
    local bravo=render_trace(owner)
    check('future NVL CHARLIE changes the prefix',advance() and owner.nvl_prefix_fmt=='NEW:%s'
        and shown(owner):find('NEW:Alice',1,true) and #owner._undoStack==2)
    check('first NVL rollback restores BRAVO and its rendering policy',runner.rollback()==true
        and owner.nvl_prefix_fmt=='OLD:%s' and render_trace(owner)==bravo)
    check('second NVL rollback restores ALPHA and OLD prefix',runner.rollback()==true
        and owner.nvl_prefix_fmt=='OLD:%s' and render_trace(owner)==alpha)
    check('NVL BRAVO replay preserves original prefix and draw geometry',advance()
        and owner.nvl_prefix_fmt=='OLD:%s' and render_trace(owner)==bravo)
    check('replayed NVL command can explicitly choose NEW again',advance()
        and owner.nvl_prefix_fmt=='NEW:%s' and shown(owner):find('NEW:Alice',1,true))
end)

run('active tween skips its click point while preserving an earlier stable point',function()
    local owner=start('tween')
    local before=render_trace(owner)
    check('ordinary initial point is capturable',Snapshot.capture(owner)~=nil)
    local ok,reason=runner.rollback()
    check('ordinary empty history has its usual explanation',ok==false and reason=='nothing-to-rollback')
    check('BEFORE click starts real nonblocking tween and reaches ALPHA',advance()
        and shown(owner)=='ALPHA' and #owner._undoStack==1 and #owner.tweens==1 and message_x()==0)
    local tween=owner.tweens[1]
    local stable=owner._undoStack[1]
    local captured,unavailable=Snapshot.capture(owner)
    check('active tween capture reports an unsupported point without changing the tween',captured==nil
        and unavailable=='tween-active' and not tween.done and not tween.cancelled and tween.t==0)
    check('active ALPHA click continues to BRAVO but keeps only BEFORE',advance()
        and shown(owner)=='BRAVO' and #owner._undoStack==1 and owner._undoStack[1]==stable)
    check('public rollback skips active points and returns to BEFORE',runner.rollback()==true
        and render_trace(owner)==before and shown(owner)=='BEFORE' and #owner._undoStack==0)
    check('stable rollback cancels the original tween',tween.cancelled and #(owner.tweens or {})==0
        and message_x()==0 and Snapshot.capture(owner)~=nil)
    check('BEFORE replay rebuilds the real ALPHA tween',advance() and shown(owner)=='ALPHA'
        and #owner.tweens==1 and owner.tweens[1]~=tween)
    check('replay reaches the original BRAVO endpoint',advance() and shown(owner)=='BRAVO')
    runner.update(1)
    check('replayed tween reaches x100 as the original timeline does',message_x()==100 and #owner.tweens==0)
end)

run('finished tween points are captured and preserve the original endpoint',function()
    local owner=start('tween')
    check('original timeline reaches active ALPHA',advance() and shown(owner)=='ALPHA' and #owner.tweens==1)
    local tween=owner.tweens[1]
    check('original timeline reaches BRAVO without running frames',advance() and shown(owner)=='BRAVO')
    runner.update(1)
    local original=render_trace(owner)
    check('original BRAVO ends its real tween at x100',tween.done and not tween.cancelled
        and message_x()==100 and #owner.tweens==0)
    local captured,reason=Snapshot.capture(owner)
    check('finished tween point is capturable without an unavailable reason',captured~=nil and reason==nil)
    local depth=#owner._undoStack
    check('finished BRAVO creates exactly one usable checkpoint',advance() and shown(owner)=='END'
        and #owner._undoStack==depth+1)
    check('rollback accepts the finished BRAVO point',runner.rollback()==true and shown(owner)=='BRAVO'
        and render_trace(owner)==original and message_x()==100 and #owner._undoStack==depth)
    runner.update(1)
    check('completed historical tween does not restart',message_x()==100 and #(owner.tweens or {})==0)
end)

run('empty history explains an unsupported active tween and recovers after completion',function()
    local owner=start('empty-tween')
    -- Invoke the real registered tween command while the initial ALPHA wait is
    -- displayed. No click has occurred, so there is no synthetic BEFORE point.
    require('kag').tween(owner,{target='message',attr='x',to='100',dur=1000,wait=false})
    local tween=owner.tweens[1]
    check('empty-history fixture owns a real active tween',tween~=nil and #owner._undoStack==0)
    local captured,reason=Snapshot.capture(owner)
    check('initial active point explains capture refusal',captured==nil and reason=='tween-active')
    check('initial active click still reaches BRAVO with empty history',advance()
        and shown(owner)=='BRAVO' and #owner._undoStack==0)
    local ok,unavailable=runner.rollback()
    check('empty-history rollback explains the omitted active point',ok==false and unavailable=='tween-active'
        and shown(owner)=='BRAVO' and not tween.cancelled)
    runner.update(1)
    check('a refused rollback leaves the original tween able to finish',tween.done and message_x()==100)
    local stable,stable_reason=Snapshot.capture(owner)
    check('completed initial tween clears the capture refusal',stable~=nil and stable_reason==nil)
    local depth=#owner._undoStack
    check('next stable click starts collecting real history',advance() and shown(owner)=='END'
        and #owner._undoStack==depth+1)
    check('new stable checkpoint rolls back normally after earlier refusal',runner.rollback()==true
        and shown(owner)=='BRAVO' and message_x()==100)
end)

run('nameplate style and speaker restore before their next consumers',function()
    local owner=start('nameplate')
    plate_calls={}
    check('original BRAVO uses an existing historical nameplate',advance() and shown(owner):find('BRAVO',1,true))
    local original=encode(plate_calls)
    local historical=owner._undoStack[1]
    check('future nameplate explicitly changes position and color',advance()
        and owner.nameplate_style.x==300 and owner.current_speaker=='Bob')
    check('two rollbacks return to the original named ALPHA',runner.rollback()==true
        and runner.rollback()==true and owner.current_speaker=='Alice'
        and owner.nameplate_style.x==32)
    plate_calls={}
    check('replayed BRAVO nameplate has identical render submissions',advance() and encode(plate_calls)==original)
    owner.nameplate_style.x=900
    check('historical nameplate style is detached from the live style',historical.nameplate_style~=nil
        and historical.nameplate_style.x==32)
end)

run('a nameplate after rollback reads the historical speaker and clears absent style',function()
    local owner=start('speaker')
    check('initial Alice has no configured nameplate',owner.current_speaker=='Alice' and owner.nameplate_style==nil)
    plate_calls={}
    check('original continuation reaches the page wait',advance() and owner._executing_command=='p')
    local original=encode(plate_calls)
    check('original continuation renders Alice',plate_calls[1] and plate_calls[1][1]=='Alice')
    check('next page changes the current speaker to Bob',advance() and owner.current_speaker=='Bob')
    check('rollback restores Alice and the absent earlier nameplate style',runner.rollback()==true
        and runner.rollback()==true and owner.current_speaker=='Alice' and owner.nameplate_style==nil)
    plate_calls={}
    check('replayed nameplate renders the same historical speaker',advance() and encode(plate_calls)==original)
end)

run('NVL rollback preserves the visibility needed by a later exit',function()
    local owner=start('nvl-visible')
    check('NVL retains the pre-entry visible textbox',owner.nvl_hidden_vis._textbox==true
        and Layers.get('_textbox').visible==false)
    check('original NVL reaches BRAVO',advance() and shown(owner):find('BRAVO',1,true))
    check('original NVL exit reveals the textbox',advance() and Layers.get('_textbox').visible==true
        and owner.nvl_hidden_vis==nil)
    check('two rollbacks restore NVL with its original visibility policy',runner.rollback()==true
        and runner.rollback()==true and owner.nvl_mode and owner.nvl_hidden_vis~=nil
        and owner.nvl_hidden_vis._textbox==true and Layers.get('_textbox').visible==false)
    check('replayed NVL exit reveals the textbox again',advance() and advance()
        and Layers.get('_textbox').visible==true and owner.nvl_hidden_vis==nil)
end)

run('unsupported presentation capture preserves normal input and prior history',function()
    local owner=start('empty-tween')
    check('ordinary ALPHA produces an earlier stable checkpoint',advance()
        and shown(owner)=='BRAVO' and #owner._undoStack==1)
    local stable=owner._undoStack[1]
    -- A live custom payload has no reconstruction contract. Exercise the real
    -- layer-state refusal, without replacing Snapshot.capture with a stub.
    Layers.get('message').userdata={opaque=true}
    local ok,advanced=pcall(advance)
    check('unsupported layer does not throw or prevent click advancement',ok and advanced
        and shown(owner)=='END' and owner._session_active)
    check('unsupported capture retains the last restorable history',#owner._undoStack==1
        and owner._undoStack[1]==stable)
    check('unsupported capture publishes a specific explanation',type(owner._rollback_capture_reason)=='string'
        and owner._rollback_capture_reason:find('opaque layer payload',1,true))
    check('earlier stable point remains usable after rejected capture',runner.rollback()==true
        and shown(owner)=='ALPHA' and #owner._undoStack==0
        and Layers.get('message').userdata==nil)
end)

print(string.format('U12 ROLLBACK STABLE POINTS: %d passed, %d failed',passed,failed))
assert(failed==0,'rollback stable-point regression')
