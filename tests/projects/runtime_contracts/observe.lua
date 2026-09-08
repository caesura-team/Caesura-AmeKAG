-- Shared by the native Lua and actual Web player drivers. Read only: no
-- tokenizer, scheduler, input, persistence or runner behavior is replaced here.
local copy=require('kag.save_state').copy
local Text=require('kag.text_scene')
local M={}
local function scene(path) return (path or ''):gsub('^assets/script/','') end
local function callers(ctx)
    local result={}
    for _,frame in ipairs(ctx.call_stack or {}) do
        result[#result+1]={scene=scene(frame.scene),locals=copy(frame.lf or {})}
    end
    return result
end
function M.capture(ctx,terminal)
    local event={kind=terminal and 'end' or (ctx._choiceMode and 'choice'
            or (ctx._executing_command=='p' and 'page' or 'dialog')),
        scene=scene(ctx.current_scene),variables=copy(ctx.f),locals=copy(ctx.lf or {}),
        callers=callers(ctx)}
    if not terminal then
        if ctx._choiceMode then
            event.options={}
            for _,button in ipairs(ctx._choiceButtonsActive or {}) do
                event.options[#event.options+1]={text=button.text,target=button.target}
            end
        else
            local draws={}
            Text.render(ctx,{render_text=function(text) draws[#draws+1]=text end,
                render_ruby=function(base) draws[#draws+1]=base end})
            event.text=table.concat(draws)
        end
    end
    return event
end
function M.check(case,trace,replay)
    local dialogs,menus,pages=0,0,0
    for _,event in ipairs(trace) do
        if event.kind=='dialog' then dialogs=dialogs+1 end
        if event.kind=='page' then pages=pages+1 end
        if event.kind=='choice' then
            menus=menus+1
            assert(#event.options==2 and event.options[1].text=='左路'
                and event.options[2].text=='右路','conditional choices differ')
        end
    end
    assert(dialogs==(replay and case.replay_texts or case.texts),
        case.name..' dialogue count: '..dialogs)
    assert(menus==(replay and 0 or case.menus or 0),case.name..' menu count: '..menus)
    assert(pages==(replay and 0 or case.pages or 0),case.name..' page count: '..pages)
    local last=assert(trace[#trace],'empty trace')
    assert(last.kind=='end' and #last.callers==0,'runner did not unwind and end')
    for key,value in pairs(case.variables) do
        assert(last.variables[key]==value,case.name..' variable '..key..': '..tostring(last.variables[key]))
    end
    if case.message_x then
        local layer=require('layers').get('message')
        assert(layer and layer.x==case.message_x,'blocking tween must reach exact endpoint')
    end
end
-- Small JSON writer for measured traces sent to the Python comparator and JS.
-- Empty tables are objects; numeric-key sequences are arrays on both hosts.
function M.json(value)
    local kind=type(value)
    if kind=='nil' then return 'null' end
    if kind=='boolean' then return value and 'true' or 'false' end
    if kind=='number' then
        assert(value==value and math.abs(value)<math.huge,'non-finite trace number')
        return tostring(value)
    end
    if kind=='string' then
        return '"'..value:gsub('[%z\1-\31\\"]',function(c)
            local escaped={['"']='\\"',['\\']='\\\\',['\n']='\\n',['\r']='\\r',['\t']='\\t'}
            return escaped[c] or string.format('\\u%04x',c:byte())
        end)..'"'
    end
    assert(kind=='table','trace contains unsupported value: '..kind)
    local result={}
    if #value>0 then
        for i=1,#value do result[i]=M.json(value[i]) end
        return '['..table.concat(result,',')..']'
    end
    local keys={}; for key in pairs(value) do assert(type(key)=='string');keys[#keys+1]=key end
    table.sort(keys)
    for _,key in ipairs(keys) do result[#result+1]=M.json(key)..':'..M.json(value[key]) end
    return '{'..table.concat(result,',')..'}'
end
return M
