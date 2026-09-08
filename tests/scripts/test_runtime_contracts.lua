-- U13: one corpus through the real runner and source/AST/on-disk cache paths.
-- Only native host bindings and the scene-content provider are test doubles.
package.path='scripts/?.lua;scripts/?/init.lua;'..package.path
local corpus_root='tests/projects/runtime_contracts/'
local corpus=dofile(corpus_root..'corpus.lua')
local function callable(fields)
    return setmetatable(fields or {},{__index=function(self,key)
        local fn=function() return true end;rawset(self,key,fn);return fn
    end})
end
local copy=require('kag.save_state').copy
local slots={}
_G.KAG=callable({save_game=function(slot,state) slots[slot]=copy(state);return true end,
    load_game=function(slot) return copy(slots[slot]),{slot=slot} end,
    is_voice_playing=function() return false end,is_bgm_playing=function() return false end,
    get_active_voices=function() return 0 end})
_G.Render,_G.Engine,_G.DevCore=callable(),callable(),callable()
Render.create_viewport=function() return 0 end
Render.create_solid_texture=function() return 0 end
local runner=require('kag_runner')
local flow=require('flow')
local compiler=require('kag.compiler')
local tokenizer=require('tokenizer')
local semantic=require('kag.semantic')
local Text=require('kag.text_scene')
local Layers=require('layers')
local observe=dofile(corpus_root..'observe.lua')
local function read(path)
    local file=assert(io.open(path,'rb'));local source=file:read('*a');file:close();return source
end
local function setup(case,lane)
    local sources={}
    for _,name in ipairs(case.scenes) do sources[name]=read(corpus_root..name) end
    local proof={loads=0,cache_reads=0,ast_compiles=0,source_compiles=0}
    flow.load_scene=function(path)
        local source=assert(sources[path:gsub('^assets/script/','')],'unknown corpus scene '..path)
        proof.loads=proof.loads+1
        local tokens
        if lane=='ast' then
            tokens=compiler.compile_from_ast(semantic.parse(source,path))
            proof.ast_compiles=proof.ast_compiles+1
        else
            tokens=tokenizer.parse(source);compiler.compile(tokens)
            proof.source_compiles=proof.source_compiles+1
            if lane=='cache' then
                local cache=os.tmpname()
                local cache_cleanup <close> = setmetatable({},{__close=function() os.remove(cache) end})
                assert(compiler.writeCache(tokens,cache),'cache write failed')
                tokens=assert(compiler.readCache(cache),'cache read failed')
                proof.cache_reads=proof.cache_reads+1
            end
        end
        return {tokens=tokens,labels=tokens._compiled.labels,path=path,base_path=path}
    end
    return proof
end
local function execute(entry,case,dt)
    assert(runner.start('assets/script/'..entry,{replace=true}))
    local trace={}
    for _=1,10000 do
        local ctx=assert(runner.get_ctx())
        assert(not (ctx.tf and ctx.tf.load_error),ctx.tf and ctx.tf.load_error)
        if not ctx._session_active then
            trace[#trace+1]=observe.capture(ctx,true);return trace
        end
        if ctx.waiting_input then
            if ctx.reveal and Text.get_state(ctx).reveal_chars<ctx.reveal.total then
                assert(runner.on_click())
            end
            trace[#trace+1]=observe.capture(ctx,false)
            if ctx._choiceMode then
                local button=assert(ctx._choiceButtonsActive[case.choice or 1])
                _GAME_MOUSE_X,_GAME_MOUSE_Y=100,button.y+button.h/2
                assert(type(_KAG_onClick)=='function');_KAG_onClick()
            end
            runner.on_click()
        else
            local ok,reason=runner.update(dt)
            assert(ok or reason=='waiting-input' or reason=='ended' or reason=='dead',tostring(reason))
        end
    end
    error('corpus runner frame limit: '..entry)
end
local runs,passed,failed={},0,0
for _,case in ipairs(corpus) do
    local canonical
    for _,lane in ipairs({'source','ast','cache'}) do
        for _,dt in ipairs({0.007,0.031}) do
            local ok,result=pcall(function()
                runner.stop();Layers.clear_for_restore();slots={}
                local proof=setup(case,lane)
                local trace=execute(case.entry,case,dt)
                if arg[1]=='--debug' and lane=='source' and dt==0.007 then
                    print('DEBUG '..case.name..' '..observe.json(trace))
                end
                observe.check(case,trace,false)
                local replay
                if case.replay then
                    replay=execute(case.replay,case,dt)
                    if arg[1]=='--debug' and lane=='source' and dt==0.007 then
                        print('DEBUG replay '..case.name..' '..observe.json(replay))
                    end
                    observe.check(case,replay,true)
                    -- Replay must reproduce the suffix after the actual saved point.
                    local suffix={};for i=#trace-#replay+1,#trace do suffix[#suffix+1]=trace[i] end
                    assert(observe.json(replay)==observe.json(suffix),'save replay differs from original suffix')
                end
                if lane=='cache' then assert(proof.cache_reads==proof.loads and proof.cache_reads>0) end
                if lane=='ast' then assert(proof.ast_compiles==proof.loads and proof.ast_compiles>0) end
                local normalized=observe.json({trace=trace,replay=replay})
                canonical=canonical or normalized
                assert(normalized==canonical,'events differ from source/7ms baseline')
                return {case=case.name,lane=lane,dt=dt,trace=trace,replay=replay,proof=proof}
            end)
            runner.stop()
            if ok then
                passed=passed+1;runs[#runs+1]=result
                print('PASS runtime corpus '..case.name..' '..lane..' dt='..dt)
            else
                failed=failed+1;print('FAIL runtime corpus '..case.name..' '..lane..' dt='..dt..': '..tostring(result))
            end
        end
    end
end
print('RUNTIME_CONTRACTS_JSON:'..observe.json({version=1,runtime='native',runs=runs}))
print(string.format('Runtime corpus: %d passed, %d failed',passed,failed))
assert(failed==0,'runtime corpus differences')
