-- Wiring-only observer: real modules/entry, no renderer or handler replacements.
package.path = 'scripts/?.lua;scripts/?/init.lua;' .. package.path
return function(entry, expected_scene)
assert(type(entry)=='string' and type(expected_scene)=='string')
local report = {scope='ORIGINAL_ENTRY_LUAMANAGER_C_BINDINGS_WIRING_ONLY_NO_GPU', entry=entry,
    expected_scene=expected_scene, status='INCONCLUSIVE', calls={start=0,update=0,render=0},
    test_backend_replacements=false, story_completion_measured=false, pixels_measured=false}
local runner, originals, codec
local function normalized(p) return type(p)=='string' and p:gsub('\\','/'):gsub('^%./','') or '' end
local function snapshot()
    local c=runner and runner.get_ctx()
    return {exists=type(c)=='table', published=c~=nil and c==rawget(_G,'_CAESURA_CTX'),
      scene=c and tostring(c.current_scene or '') or '', tokens=c and #(c.tokens or {}) or 0,
      command_error=c and c._command_error==true or false,
      active=c and c._session_active==true or false, token_index=c and c.token_index or -1}
end
local ok, err=xpcall(function()
    codec=require('capability_json')
    require('config')
    require('kag.init')
    runner=require('kag_runner')
    require('layers')
    assert(type(KAG)=='table' and type(Render)=='table' and type(DevCore)=='table',
      'Real LuaManager C bindings required')
    originals={start=runner.start,update=runner.update,render=runner.render}
    for _,name in ipairs({'start','update','render'}) do
        local fn=assert(originals[name])
        runner[name]=function(...)
            report.calls[name]=report.calls[name]+1
            if name=='start' then report.start_argument=tostring(select(1,...)) end
            local values=table.pack(fn(...)) -- unchanged arguments and returns; exceptions propagate
            if name=='start' then report.start_return_true=values[1]==true end
            return table.unpack(values,1,values.n)
        end
    end
    assert(rawget(_G,'engine_update')==nil and rawget(_G,'engine_render')==nil,
      'Unexpected preexisting entry hooks')
    dofile(entry) -- original bytes; no callback replacement/fallback
    report.after_entry=snapshot()
    local c=report.after_entry
    report.start_context_confirmed=report.calls.start==1 and report.start_return_true==true
      and c.exists and c.published and c.tokens>0 and c.active and not c.command_error
      and normalized(c.scene)==normalized(expected_scene)
    if not report.start_context_confirmed then report.reason='real-start-or-context-precondition-not-established'; return end
    report.hooks={engine_update=type(_G.engine_update),engine_render=type(_G.engine_render),click=type(_G._KAG_onClick)}
    report.missing_hooks=codec.array({})
    for _,name in ipairs({'engine_update','engine_render','_KAG_onClick'}) do
        if type(rawget(_G,name))~='function' then report.missing_hooks[#report.missing_hooks+1]=name end
    end
    -- Exactly one update and one render; no click, scene pumping, sleep, synthetic input, or retry.
    if type(_G.engine_update)=='function' then
        local before=report.calls.update
        report.update_ok,report.update_error=pcall(_G.engine_update,0.016)
        report.update_forwarded=report.calls.update-before==1
        if report.update_ok then report.update_error=nil else report.update_error=tostring(report.update_error) end
    end
    if type(_G.engine_render)=='function' then
        local before=report.calls.render
        report.render_ok,report.render_error=pcall(_G.engine_render)
        report.render_forwarded=report.calls.render-before==1
        if report.render_ok then report.render_error=nil else report.render_error=tostring(report.render_error) end
    end
    report.after_callbacks=snapshot()
    if #report.missing_hooks>0 then
        report.status='FAIL_ENTRY_WIRING'; report.reason='required-entry-hooks-missing'
    elseif report.update_ok and report.render_ok and not report.after_callbacks.command_error then
        if report.update_forwarded and report.render_forwarded then report.status='PASS_WIRING_ONLY'
        else report.status='FAIL_ENTRY_WIRING'; report.reason='entry-does-not-forward-runner-update-or-render' end
    else report.reason='real-callback-execution-precondition-not-established' end
end,debug.traceback)
if not ok then report.reason='real-module-entry-or-host-precondition-error'; report.error=tostring(err) end
if runner then
    local call_ok, stop_result, stop_reason=pcall(runner.stop)
    report.stop_call_ok=call_ok
    report.stop_return_true=call_ok and stop_result==true
    if not report.stop_return_true then
        report.stop_error=tostring(call_ok and stop_reason or stop_result)
        report.status='INCONCLUSIVE'
    end
    if originals then for name,fn in pairs(originals) do runner[name]=fn end end
    report.context_retired=runner.get_ctx()==nil
    if not report.context_retired then report.status='INCONCLUSIVE';report.cleanup_error='context not retired' end
end
if codec then print('ENTRY_WIRING_REPORT='..assert(codec.encode(report)))
else print('ENTRY_WIRING_INCONCLUSIVE='..tostring(report.error)) end
io.stdout:flush();io.stderr:flush()
return report
end
