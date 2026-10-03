// @vitest-environment jsdom
// Real Wasmoon modules + real DOM rendering. No transition/RTT implementation
// or capture result is injected by the test. The canvas host is an actual Skia
// pixel surface for jsdom; browser-pixel acceptance remains a separate run.
import {afterEach, expect, it} from 'vitest'
import {readFileSync, existsSync, statSync} from 'node:fs'
import {fileURLToPath} from 'node:url'
import {dirname, join, resolve, relative} from 'node:path'
import {createPlayer} from './bridge.js'
import {createCanvas,loadImage} from '@napi-rs/canvas'
import {ReadableStream} from 'node:stream/web'
import {DomRenderer} from './dom-renderer.js'
import {installCanvasHost} from './test-support/canvas-host.js'

const here=dirname(fileURLToPath(import.meta.url))
const root=process.env.CAESURA_SOURCE_ROOT?resolve(process.env.CAESURA_SOURCE_ROOT):resolve(here,'..')
const index=JSON.parse(readFileSync(join(here,'scripts-index.json'),'utf8'))
const fixtureImages=new Map()
const fetchFiles=async url=>{
  const pathname=decodeURIComponent(new URL(url,'http://contract/').pathname).replace(/^\/+/, '')
  if(pathname==='scripts/index.json') return {ok:true,status:200,json:async()=>index,text:async()=>JSON.stringify(index)}
  if(fixtureImages.has(pathname)){
    const bytes=fixtureImages.get(pathname)
    return {ok:true,status:200,headers:{get:name=>name.toLowerCase()==='content-length'?String(bytes.byteLength):null},
      body:new ReadableStream({start(controller){controller.enqueue(new Uint8Array(bytes));controller.close()}}),
      arrayBuffer:async()=>bytes.buffer.slice(bytes.byteOffset,bytes.byteOffset+bytes.byteLength)}
  }
  const file=resolve(root,pathname)
  if(relative(root,file).startsWith('..')) throw new Error('Fixture path escaped')
  const ok=existsSync(file)&&statSync(file).isFile()
  const bytes=ok?readFileSync(file):Buffer.alloc(0)
  return {ok,status:ok?200:404,text:async()=>bytes.toString('utf8'),
    arrayBuffer:async()=>bytes.buffer.slice(bytes.byteOffset,bytes.byteOffset+bytes.byteLength)}
}
let player,renderer,stage,restoreCanvas
async function setup(){
  fixtureImages.clear()
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


// Geometry is checked against the native shader's four threshold quadrants.
// jsdom does not rasterize CSS clipping: these are real source pixels plus DOM
// clip geometry, not an assertion that Chrome compositor pixels were measured.
const wipes=[
  {direction:'left',method:2,clip:'inset(0 75% 0 0)'},
  {direction:'right',method:3,clip:'inset(0 0 0 75%)'},
  {direction:'top',method:4,clip:'inset(75% 0 0 0)'},
  {direction:'bottom',method:5,clip:'inset(0 0 75% 0)'},
]
async function pair(){
  await scene([255,0,0,255],'A RED');await renderer.render();const a=await capture('method_a')
  await scene([0,0,255,255],'B BLUE');await renderer.render();const b=await capture('method_b')
  expect(b[1]).toBeGreaterThan(a[1]);return {a:a[0],b:b[0]}
}
async function start(method,extra=''){
  await player.lua.doString(`
    methods_tx=require('transition').start(method_a,method_b,{method='${method}',duration=100,${extra}})
    methods_release_count=0
    methods_tx.release=function()
      methods_release_count=methods_release_count+1
      require('rtt').destroy(method_a);require('rtt').destroy(method_b)
    end
  `)
}
function sourcePixels(){
  const overlay=stage.querySelector('.caesura-transition');expect(overlay).not.toBeNull()
  const from=overlay.querySelector('.caesura-transition-from'),to=overlay.querySelector('.caesura-transition-to')
  expect([...from.querySelector('canvas').getContext('2d').getImageData(0,0,1,1).data]).toEqual([255,0,0,255])
  expect([...to.querySelector('canvas').getContext('2d').getImageData(0,0,1,1).data]).toEqual([0,0,255,255])
  return {overlay,from,to}
}
it.each(wipes)('Web method contract: $direction wipe retains distinct rendered source/destination',async ({direction,method,clip})=>{
  await setup();await pair();await start('wipe',`direction='${direction}',`)
  await player.lua.doString("require('transition').tick(25,methods_tx)")
  const {to}=sourcePixels()
  expect(player.core.transitionOverlay.method).toBe(method)
  expect(player.core.transitionOverlay.progress).toBeCloseTo(.25)
  expect(to.style.clipPath).toBe(clip)
  expect(to.style.opacity).toBe('1')
  // An additional real presentation must retain the hold, not flash base B.
  await renderer.render();expect(stage.querySelector('.caesura-transition')).not.toBeNull()
  await player.lua.doString("assert(require('transition').tick(75,methods_tx)==require('transition').Status.COMPLETED)")
  expect(player.core.transitionOverlay.progress).toBe(1)
  await player.lua.doString("require('transition').cancel(methods_tx);require('transition').cancel(methods_tx);assert(methods_release_count==1)")
  expect(player.core.sceneSnapshots.size).toBe(0)
  expect(stage.querySelector('.caesura-transition')).toBeNull()
},20000)

it('Web method contract: decoded rule mask reveals actual red-channel thresholds',async()=>{
  await setup();await pair()
  const image=createCanvas(4,1),ctx=image.getContext('2d')
  for(const [x,grey] of [0,64,192,255].entries()){ctx.fillStyle=`rgb(${grey},${grey},${grey})`;ctx.fillRect(x,0,1,1)}
  fixtureImages.set('assets/contracts/threshold-rule.png',image.toBuffer('image/png'))
  const id=await player.lua.doString("rule_id=assert(require('transition').preload_rule('assets/contracts/threshold-rule.png'));return rule_id")
  await start('rule','rule_tex=rule_id,')
  await player.lua.doString("require('transition').tick(50,methods_tx)")
  const {to}=sourcePixels()
  expect(player.core.textures.get(id).prepared).toBeTruthy()
  const match=to.style.maskImage.match(/data:image\/png;base64,([A-Za-z0-9+/=]+)/)
  expect(match).not.toBeNull()
  const mask=await loadImage(Buffer.from(match[1],'base64'))
  const decoded=createCanvas(4,1),out=decoded.getContext('2d');out.drawImage(mask,0,0)
  const pixels=out.getImageData(0,0,4,1).data
  expect([pixels[3],pixels[7],pixels[11],pixels[15]]).toEqual([255,255,0,0])
  await player.lua.doString("require('transition').cancel(methods_tx);require('transition').free_rule(rule_id)")
  expect(player.core.sceneSnapshots.size).toBe(0);expect(player.core.textures.has(id)).toBe(false)
  expect(stage.querySelector('.caesura-transition')).toBeNull()
},20000)

it('Web method contract: missing rule decoding cancels leases without a completion receipt',async()=>{
  await setup();await pair();await start('rule',"rule_tex=assert(require('transition').preload_rule('assets/contracts/missing-rule.png')),")
  expect(await player.lua.doString("return pcall(function() require('transition').tick(25,methods_tx) end)")).toBe(false)
  // Direct low-level caller owns its release callback even on submission error.
  await player.lua.doString("require('transition').cancel(methods_tx);require('transition').clear_rule_cache();assert(methods_release_count==1)")
  expect(player.core.sceneSnapshots.size).toBe(0)
  expect(player.core.events.some(e=>e.kind==='transition.submit'&&e.detail.progress>=1)).toBe(false)
},20000)

it('Web method contract: real KAG zero-duration trans uses no snapshot or presentation wait',async()=>{
  await setup();await scene([255,0,0,255],'UNCHANGED');await renderer.render()
  const before=player.core.events.length
  const result=await player.runScene('[trans duration=0][eval exp="f.zero_done=1"][end]','zero-contract.ks',{maxFrames:30,autoClick:false})
  expect(result).not.toMatch(/^ERR:/)
  expect(await player.lua.doString('return require("kag_runner").get_ctx().f.zero_done')).toBe(1)
  const events=player.core.events.slice(before)
  expect(events.filter(e=>e.kind==='transition.capture'||e.kind==='transition.submit'||e.kind==='scene.presented')).toHaveLength(0)
  expect(player.core.sceneSnapshots.size).toBe(0)
  expect(await player.lua.doString('return require("kag_runner").get_ctx()._transition_render_wait==nil')).toBe(true)
},20000)

it('Web method contract: replacement owner survives a stale cancel and all leases retire once',async()=>{
  await setup();await pair();await start('crossfade')
  await player.lua.doString("require('transition').tick(25,methods_tx)")
  await scene([0,255,0,255],'C GREEN');await renderer.render();await capture('replacement_a')
  await scene([255,255,0,255],'D YELLOW');await renderer.render();await capture('replacement_b')
  await player.lua.doString(`
    replacement_tx=require('transition').start(replacement_a,replacement_b,{method='wipe',direction='right',duration=100})
    replacement_released=0
    replacement_tx.release=function()
      replacement_released=replacement_released+1
      require('rtt').destroy(replacement_a);require('rtt').destroy(replacement_b)
    end
    assert(methods_release_count==1)
    require('transition').tick(25,replacement_tx)
    require('transition').cancel(methods_tx)
    assert(require('transition').is_active(replacement_tx))
  `)
  expect(player.core.sceneSnapshots.size).toBe(2)
  expect(player.core.transitionOverlay.method).toBe(3)
  expect(stage.querySelector('.caesura-transition-to').textContent).toContain('D YELLOW')
  await player.lua.doString("require('transition').cancel(replacement_tx);require('transition').cancel(replacement_tx);assert(replacement_released==1)")
  expect(player.core.sceneSnapshots.size).toBe(0);expect(stage.querySelector('.caesura-transition')).toBeNull()
},20000)

it('Web epoch contract: replacing a real presenter rejects its old pending receipt',async()=>{
  await setup()
  const pending=player.core.presentScene()
  const nextStage=document.createElement('div');document.body.appendChild(nextStage)
  const nextRenderer=new DomRenderer(player.core,nextStage,{width:320,height:180})
  try{
    await expect(pending).rejects.toThrow(/owner or frame changed/)
    expect(player.core.events.filter(e=>e.kind==='scene.presented')).toHaveLength(0)
  }finally{nextRenderer.destroy()}
},20000)

it('Web epoch contract: initial capture cannot publish a retired presenter frame',async()=>{
  await setup();await scene([255,0,0,255],'OLD PRESENTER')
  // Real pending rAF; no forged frame counter or fake capture implementation.
  const pending=player.core.captureScene(true)
  const nextStage=document.createElement('div');document.body.appendChild(nextStage)
  const nextRenderer=new DomRenderer(player.core,nextStage,{width:320,height:180})
  try{
    const outcome=await pending.then(value=>({ok:true,value}),()=>({ok:false}))
    if(outcome.ok) expect(outcome.value).toBe('0:0')
    expect(player.core.sceneSnapshots.size).toBe(0)
  }finally{nextRenderer.destroy()}
},20000)

it('Web scheduler contract: real bg mutation yields distinct A and B with pending ownership consumed',async()=>{
  await setup()
  // This host resolves registered image URLs as the browser page does. jsdom
  // does not decode IMG CSS composition; this case proves snapshot/source
  // identity and scheduling, while color/mask pixels are measured above.
  const originalGet=renderer.textureUrls.get.bind(renderer.textureUrls)
  renderer.textureUrls.get=id=>originalGet(id)??(player.core.textures.get(id)?.path
    ?new URL(player.core.textures.get(id).path,'http://contract/').href:null)
  await player.runScene('[bg storage="assets/bg/classroom.png"][text text="A scene"][p]',
    'source-a.ks',{maxFrames:100,autoClick:false})
  await renderer.render()
  const observed=[]
  const observer=new MutationObserver(()=>{
    const overlay=stage.querySelector('.caesura-transition')
    if(!overlay)return
    const a=overlay.querySelector('.caesura-transition-from img[data-layer="bg"]')
    const b=overlay.querySelector('.caesura-transition-to img[data-layer="bg"]')
    observed.push({a:a?.getAttribute('src'),b:b?.getAttribute('src'),progress:player.core.transitionOverlay?.progress})
  })
  observer.observe(stage,{subtree:true,childList:true,attributes:true,attributeFilter:['style','src']})
  try{
    const result=await player.runScene('[bg storage="assets/fg/girl_uniform.png"][trans method="crossfade" time=64][eval exp="f.pending_done=1"][end]',
      'destination-b.ks',{maxFrames:100,autoClick:false})
    expect(result).not.toMatch(/^ERR:/)
    expect(observed.some(v=>v.a?.endsWith('/assets/bg/classroom.png')&&v.b?.endsWith('/assets/fg/girl_uniform.png')&&v.progress>0&&v.progress<1)).toBe(true)
    expect(await player.lua.doString('local c=require("kag_runner").get_ctx();return c.f.pending_done==1 and c._pending_transition==nil and c._transition_render_wait==nil and #c.active_operations==0')).toBe(true)
    expect(player.core.sceneSnapshots.size).toBe(0)
  }finally{observer.disconnect()}
},20000)
