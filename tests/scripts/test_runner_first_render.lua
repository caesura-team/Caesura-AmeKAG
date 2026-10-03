-- Actual runner.start/update/render order; host rendering alone is recorded.
package.path="scripts/?.lua;scripts/?/init.lua;"..package.path
local boot={volumes={},volumeCalls=0,width=320,height=180}
_G.KAG={
 set_bus_volume=function(bus,value)
  assert(bus=='bgm' or bus=='se' or bus=='voice');assert(type(value)=='number')
  boot.volumes[bus]=value;boot.volumeCalls=boot.volumeCalls+1;return true
 end,
 get_bus_volume=function(bus)return boot.volumes[bus] or 1 end,
}
_G.Render={}
_G.DevCore={
 set_resolution=function(w,h)assert(w>0 and h>0);boot.width,boot.height=w,h;return true end,
 get_resolution=function()return boot.width,boot.height end,
 set_fullscreen=function(value)boot.fullscreen=value;return true end,
}

local config=require('config') -- Actual bootstrap, before installing this hardware boundary.
assert(boot.volumeCalls==3 and boot.width==config.window_width and boot.height==config.window_height,'actual bootstrap boundary incomplete')
print('BOOTSTRAP_REAL_CONFIG_PASS volumes='..boot.volumeCalls..' size='..boot.width..'x'..boot.height)
local hw={frame=0,scene='BLACK',next=1000,leases={},queries=0,captures={},submits={},destroyed={}}
_G._CAESURA_BACKEND={
 render=function(cmd,...)
  local a={...}
  if cmd=='capture_scene' then
   hw.queries=hw.queries+1
   if hw.frame==0 or hw.unsupported then return 0,0 end
   hw.next=hw.next+1;hw.leases[hw.next]={scene=hw.scene,frame=hw.frame}
   hw.captures[#hw.captures+1]=hw.leases[hw.next]
   print(string.format('CAPTURE frame=%d scene=%s',hw.frame,hw.scene))
   return hw.next,hw.frame
  elseif cmd=='load_texture' then assert(a[1]=='scene-B.png');return 102
  elseif cmd=='create_viewport' then hw.next=hw.next+1;return hw.next
  elseif cmd=='destroy_viewport' then hw.destroyed[#hw.destroyed+1]=a[1];return true
  elseif cmd=='submit_batch' then
   for i=1,a[1][1] do if a[1][1+(i-1)*16+2]==102 then hw.pending='B' end end
   return true
  elseif cmd=='submit_transition' then
   local from,to=assert(hw.leases[a[2]]),assert(hw.leases[a[3]])
   hw.submits[#hw.submits+1]={from=from.scene,to=to.scene,t=a[6]};return true
  elseif cmd=='get_resolution' then return 320,180
  elseif cmd=='cancel_transition' or cmd=='cancel_async_loads' or cmd=='set_view_name'
   or cmd=='text_reset_state' or cmd=='clear_text' then return true
  else error('Unexpected render boundary: '..cmd) end
 end,
 audio=function(cmd) if cmd:find('is_') then return false end;if cmd=='get_position' or cmd=='get_length' or cmd=='active_voice_count' then return 0 end;if cmd=='get_bus_volume' then return 1 end;return true end,
 platform=function(cmd) if cmd=='get_resolution' or cmd=='get_window_size' then return 320,180 end;return true end,
}
local runner=require('kag_runner');local layers=require('layers')
local failures=0
local function check(name,ok) print((ok and 'PASS ' or 'FAIL ')..name);if not ok then failures=failures+1 end end
local path=arg and arg[1] or 'tests/projects/transition_startup/scene.ks'
assert(runner.start(path))
local ctx=runner.get_ctx()
check('start suspended before first visual mutation',not ctx._command_error and not layers.find('bg') and hw.frame==0)
-- Native order: start has resumed; update and input can resume before render.
runner.update(.016);runner.update(.016);runner.on_click();runner.continue_scene_debugger()
runner.update(.016)
check('pre-render update/click/debug cannot consume capture wait',not ctx._command_error and hw.frame==0 and #hw.captures==0 and not layers.find('bg'))
-- DebugProtocol can directly resume the anchored coroutine before notifying
-- runner: the wait inside that coroutine must defend the same render boundary.
if ctx.co and coroutine.status(ctx.co)=='suspended' then
 local ok,err=coroutine.resume(ctx.co,16);check('direct debug resume remains a wait',ok and not ctx._command_error)
else check('direct debug resume remains a wait',false) end
local function render()
 hw.pending='BLACK';layers.render();runner.render()
 hw.frame=hw.frame+1;hw.scene=hw.pending
 print(string.format('REAL_HOST_RENDER frame=%d scene=%s',hw.frame,hw.scene))
end
render()
for i=1,10 do
 runner.update(.016)
 if i==1 and ctx.co and coroutine.status(ctx.co)=='suspended' then
  local before=#hw.captures
  local ok=coroutine.resume(ctx.co,16)
  check('direct debug at post-bg yield waits for destination render',ok and not ctx._command_error and #hw.captures==before)
 end
 -- A second owner update in the same render epoch cannot capture B early.
 runner.update(.016)
 render()
 if ctx.f.sentinel==1 then break end
end
check('real runner reaches sentinel without ErrorUI',ctx.f.sentinel==1 and not ctx._command_error)
check('source is real initial rendered BLACK',#hw.captures==2 and hw.captures[1].scene=='BLACK' and hw.captures[1].frame>=1)
check('destination is a later rendered B',#hw.captures==2 and hw.captures[2].scene=='B' and hw.captures[2].frame>hw.captures[1].frame)
check('all source leases and operations retire',ctx._pending_transition==nil and #ctx.active_operations==0)
assert(runner.stop())

-- A failed capability is reported after one actual render attempt, not retried
-- forever. Replacing/stopping a pending session must clear its epoch waiter.
hw.frame,hw.scene,hw.queries=0,'BLACK',0;hw.unsupported=true
assert(runner.start(path));local blocked=runner.get_ctx()
runner.update(.016)
check('unsupported has not been guessed before render',not blocked._command_error and hw.queries==1)
render();runner.update(.016)
check('unsupported fails after actual render without repeated capture',blocked._command_error==true and hw.queries==2
 and blocked._pending_transition==nil and blocked._transition_render_wait==nil and #blocked.active_operations==0)
assert(runner.stop())
hw.frame,hw.scene,hw.queries=0,'BLACK',0;hw.unsupported=false
assert(runner.start(path));local replaced=runner.get_ctx()
assert(runner.start(path,{replace=true}));local replacement=runner.get_ctx()
check('replacement retires old render waiter',replacement~=replaced and replaced._transition_render_wait==nil
 and replaced._pending_transition==nil and #replaced.active_operations==0)
assert(runner.stop())
check('stop retires current render waiter',replacement._transition_render_wait==nil
 and replacement._pending_transition==nil and #replacement.active_operations==0)
print('RUNNER_FIRST_RENDER_RESULT failures='..failures)
if failures>0 then os.exit(1) end
