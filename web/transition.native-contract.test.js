// @vitest-environment jsdom
// Real Wasmoon modules + real DOM rendering. No transition/RTT implementation
// or capture result is injected by the test. The canvas host is an actual Skia
// pixel surface for jsdom; browser-pixel acceptance remains a separate run.
import {afterEach, expect, it} from 'vitest'
import {readFileSync, existsSync, statSync} from 'node:fs'
import {fileURLToPath} from 'node:url'
import {dirname, join, resolve, relative} from 'node:path'
import {createPlayer} from './bridge.js'
import {DomRenderer} from './dom-renderer.js'
import {installCanvasHost} from './test-support/canvas-host.js'

const here=dirname(fileURLToPath(import.meta.url))
const root=process.env.CAESURA_SOURCE_ROOT?resolve(process.env.CAESURA_SOURCE_ROOT):resolve(here,'..')
const index=JSON.parse(readFileSync(join(here,'scripts-index.json'),'utf8'))
const fetchFiles=async url=>{
  const pathname=decodeURIComponent(new URL(url,'http://contract/').pathname).replace(/^\/+/, '')
  if(pathname==='scripts/index.json') return {ok:true,status:200,json:async()=>index,text:async()=>JSON.stringify(index)}
  const file=resolve(root,pathname)
  if(relative(root,file).startsWith('..')) throw new Error('Fixture path escaped')
  const ok=existsSync(file)&&statSync(file).isFile()
  const bytes=ok?readFileSync(file):Buffer.alloc(0)
  return {ok,status:ok?200:404,text:async()=>bytes.toString('utf8'),
    arrayBuffer:async()=>bytes.buffer.slice(bytes.byteOffset,bytes.byteOffset+bytes.byteLength)}
}
let player,renderer,stage,restoreCanvas
async function setup(){
  restoreCanvas=installCanvasHost()
  document.body.innerHTML='<div id="stage"></div>'
  stage=document.getElementById('stage')
  player=await createPlayer({scriptsBase:'http://contract/scripts/',fetchImpl:fetchFiles,
    langBase:'',wasmFile:process.env.CAESURA_WASMOON_WASM??join(here,'node_modules/wasmoon/dist/glue.wasm')})
  renderer=new DomRenderer(player.core,stage,{width:320,height:180})
}
afterEach(async()=>{
  if(player){await player.dispose();player=null}
  renderer?.destroy();renderer=null
  restoreCanvas?.();restoreCanvas=null
})
async function scene(color,text){
  const id=await player.lua.doString(`return require('backend').create_solid_texture(${color.join(',')})`)
  const node=player.core.ensureLayer('bg',{w:320,h:180,z:0,layer_type:4})
  player.core.setLayerImage(node,id)
  player.core.setDraws([{t:text,x:12,y:20,r:255,g:255,b:255,a:255,s:1}])
}
async function capture(name){
  const value=await player.lua.doString(`
    ${name},${name}_frame=require('rtt').capture_scene()
    assert(type(${name})=='number' and ${name}>0)
    assert(type(${name}_frame)=='number' and ${name}_frame>0)
    return tostring(${name})..':'..tostring(${name}_frame)
  `)
  return value.split(':').map(Number)
}

it('Web native contract: real Lua captures two rendered scenes and visibly blends them',async()=>{
  await setup()
  await scene([255,0,0,255],'BEFORE')
  await renderer.render()
  // Mutate model state without presenting it: capture must retain BEFORE,
  // not silently substitute the new unrendered model for a real prior frame.
  await scene([0,0,255,255],'AFTER')
  const [from,frameA]=await capture('capture_a')
  await renderer.render()
  const [to,frameB]=await capture('capture_b')
  expect(from).not.toBe(to)
  expect(frameB).toBeGreaterThan(frameA)
  await player.lua.doString(`
    local transition=require('transition')
    tx=transition.start(capture_a,capture_b,{method='crossfade',duration=100})
    assert(transition.is_active(tx))
    tx.release=function()
      if not tx_released then
        tx_released=true
        require('rtt').destroy(capture_a)
        require('rtt').destroy(capture_b)
      end
    end
    transition.tick(50,tx)
  `)
  await renderer.render()
  const overlay=stage.querySelector('.caesura-transition')
  expect(overlay).not.toBeNull()
  expect(overlay.querySelector('.caesura-transition-from')?.textContent).toContain('BEFORE')
  expect(overlay.querySelector('.caesura-transition-to')?.textContent).toContain('AFTER')
  expect(Number(overlay.querySelector('.caesura-transition-to').style.opacity)).toBeCloseTo(0.5)
  expect(overlay.querySelectorAll('canvas').length).toBeGreaterThanOrEqual(2)
  const oldPixels=overlay.querySelector('.caesura-transition-from canvas').getContext('2d').getImageData(0,0,1,1).data
  const newPixels=overlay.querySelector('.caesura-transition-to canvas').getContext('2d').getImageData(0,0,1,1).data
  expect([...oldPixels]).toEqual([255,0,0,255])
  expect([...newPixels]).toEqual([0,0,255,255])
  await player.lua.doString(`
    local transition=require('transition')
    assert(transition.tick(50,tx)==transition.Status.COMPLETED)
    assert(not transition.is_active(tx))
    transition.cancel(tx)
    assert(tx_released)
  `)
  await renderer.render()
  expect(stage.querySelector('.caesura-transition')).toBeNull()
},20000)

it('Web native contract: capture without presentation rejects and cancellation clears owned overlay',async()=>{
  await setup()
  expect(await player.lua.doString(`local ok=pcall(function() require('rtt').capture_scene() end);return ok`)).toBe(false)
  await scene([255,0,0,255],'OWNER')
  await renderer.render()
  await capture('owner_a')
  await renderer.render()
  await capture('owner_b')
  await player.lua.doString(`
    local transition=require('transition')
    owner=transition.start(owner_a,owner_b,{duration=100})
    released=0
    owner.release=function()
      released=released+1
      require('rtt').destroy(owner_a)
      require('rtt').destroy(owner_b)
    end
    transition.tick(25,owner)
    transition.cancel({}) -- stale owner must not cancel the active operation
    assert(transition.is_active(owner))
  `)
  await renderer.render()
  expect(stage.querySelector('.caesura-transition')).not.toBeNull()
  await player.lua.doString(`
    local transition=require('transition')
    transition.cancel(owner)
    transition.cancel(owner)
    assert(released==1 and not transition.is_active(owner))
    assert(require('backend').submit_transition(1,owner_a,owner_b,0,0,0.5)==false)
  `)
  await renderer.render()
  expect(stage.querySelector('.caesura-transition')).toBeNull()
},20000)

it('Web native contract: actual KAG trans crosses a real presentation boundary',async()=>{
  await setup()
  await scene([255,0,0,255],'OLD FRAME')
  await renderer.render()
  const observed=[]
  const mutation=new MutationObserver(()=>{
    const overlay=stage.querySelector('.caesura-transition')
    if(overlay){
      const next=overlay.querySelector('.caesura-transition-to')
      observed.push(next?Number(next.style.opacity):0)
    }
  })
  mutation.observe(stage,{subtree:true,childList:true,attributes:true,attributeFilter:['style']})
  try{
    const result=await player.runScene('[trans method="crossfade" time=64][eval exp="f.transition_done=1"][end]',
      'transition-contract.ks',{maxFrames:100,autoClick:false})
    expect(result).not.toMatch(/^ERR:/)
    expect(await player.lua.doString('return require("kag_runner").get_ctx().f.transition_done')).toBe(1)
    expect(observed.some(progress=>progress>0&&progress<1)).toBe(true)
    const rendered=player.core.events.filter(event=>event.kind==='scene.presented')
    expect(rendered.length).toBeGreaterThan(0)
    const epoch=await player.lua.doString('return require("kag_runner").get_ctx()._render_epoch')
    expect(epoch).toBeGreaterThan(0)
    expect(epoch).toBe(rendered.at(-1).detail.frame)
    expect(epoch).toBeLessThanOrEqual(renderer._sceneFrame)
    expect(await player.lua.doString('return #(require("kag_runner").get_ctx().active_operations or {})')).toBe(0)
    await renderer.render()
    expect(stage.querySelector('.caesura-transition')).toBeNull()
  }finally{mutation.disconnect()}
},20000)

it('Web native contract: a missing presenter cannot release the runner render wait',async()=>{
  await setup()
  renderer.destroy()
  const result=await player.runScene('[bg storage="assets/bg/classroom.png"][trans time=32][end]',
    'transition-no-presenter.ks',{maxFrames:20,autoClick:false})
  expect(result).toMatch(/^ERR:render-presentation-failed:/)
  expect(player.core.events.filter(event=>event.kind==='scene.presented')).toHaveLength(0)
  expect(player.core.sceneSnapshots.size).toBe(0)
  expect(await player.lua.doString('return require("kag_runner").get_ctx()==nil')).toBe(true)
},20000)

it('Web native contract: closing the renderer rejects an in-flight real frame receipt',async()=>{
  await setup()
  const completion=player.core.presentScene()
  renderer.destroy()
  await expect(completion).rejects.toThrow(/closed/)
  expect(player.core.events.filter(event=>event.kind==='scene.presented')).toHaveLength(0)
},20000)

async function runStandalone(mode,source,name){
  if(mode==='source')return player.runScene(source,name,{maxFrames:100,autoClick:false})
  // Same real tokenizer/compiler/serialize route as a baked Web bundle.
  player.lua.global.set('__standalone_source',source)
  player.lua.global.set('__standalone_name',name)
  const bundle=await player.lua.doString(`
    local tokens=require('tokenizer').parse(__standalone_source)
    require('kag.compiler').compile(tokens)
    return {version=1,scenes={[__standalone_name]=require('kag.compiler').serialize(tokens)},assets={}}
  `)
  return player.runFromBundle(bundle,name,{maxFrames:100,autoClick:false})
}

it.each(['source','bundle'])('Web native contract: standalone trans obtains its first actual frame in %s',async(mode)=>{
  await setup()
  expect(renderer._sceneFrame).toBe(0)
  expect(player.core.events.filter(event=>event.kind==='scene.presented')).toHaveLength(0)
  // bg is deliberately NOT adjacent to trans. The normal earlier token yield
  // and flag update must not be mistaken for a prior rendered frame.
  const source='[bg storage="assets/bg/classroom.png"][eval exp="f.before_standalone=1"][trans time=64][eval exp="f.standalone_done=1"][end]'
  const result=await runStandalone(mode,source,`standalone-first-${mode}.ks`)
  expect(result).toMatch(/^DONE:/)
  expect(await player.lua.doString('local c=require("kag_runner").get_ctx();return c.f.before_standalone==1 and c.f.standalone_done==1')).toBe(true)
  const captures=player.core.events.filter(event=>event.kind==='transition.capture')
  expect(captures).toHaveLength(2)
  expect(captures[0].detail.frame).toBeGreaterThan(0)
  expect(captures[1].detail.frame).toBeGreaterThan(captures[0].detail.frame)
  const firstPresent=player.core.events.findIndex(event=>event.kind==='scene.presented')
  const firstCapture=player.core.events.findIndex(event=>event.kind==='transition.capture')
  expect(firstPresent).toBeGreaterThanOrEqual(0)
  expect(firstCapture).toBeGreaterThan(firstPresent)
  const releases=player.core.events.filter(event=>event.kind==='transition.release').map(event=>event.detail.id)
  expect(releases.sort()).toEqual(captures.map(event=>event.detail.id).sort())
  expect(player.core.sceneSnapshots.size).toBe(0)
  expect(player.core.transitionOverlay).toBeNull()
  expect(await player.lua.doString('local c=require("kag_runner").get_ctx();return #(c.active_operations or {})==0 and c._transition_render_wait==nil')).toBe(true)
},20000)

it('Web native contract: standalone first-frame wait cannot fabricate an unsupported presentation',async()=>{
  await setup()
  renderer.destroy()
  const result=await runStandalone('source','[eval exp="f.before_standalone=1"][trans time=64][eval exp="f.must_not_run=1"][end]',
    'standalone-unsupported.ks')
  expect(result).toMatch(/^ERR:render-presentation-failed:/)
  expect(player.core.events.filter(event=>event.kind==='scene.presented')).toHaveLength(0)
  expect(player.core.events.filter(event=>event.kind==='transition.capture')).toHaveLength(0)
  expect(player.core.sceneSnapshots.size).toBe(0)
  expect(player.core.transitionOverlay).toBeNull()
  expect(await player.lua.doString('return require("kag_runner").get_ctx()==nil')).toBe(true)
},20000)
