// @vitest-environment jsdom
// Observe actual production DOM/rAF completion; never replace the scheduler,
// transition, clock or frame counter. Chrome compositor pixels are separate.
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
async function fetchFiles(url){
  const name=decodeURIComponent(new URL(url,'http://contract/').pathname).replace(/^\/+/, '')
  if(name==='scripts/index.json')return {ok:true,status:200,json:async()=>index,text:async()=>JSON.stringify(index)}
  const file=resolve(root,name)
  if(relative(root,file).startsWith('..'))throw new Error('Fixture path escaped')
  const ok=existsSync(file)&&statSync(file).isFile(), bytes=ok?readFileSync(file):Buffer.alloc(0)
  return {ok,status:ok?200:404,text:async()=>bytes.toString('utf8'),arrayBuffer:async()=>bytes.buffer.slice(bytes.byteOffset,bytes.byteOffset+bytes.byteLength)}
}
let player,renderer,stage,restoreCanvas
async function setup(){
  restoreCanvas=installCanvasHost()
  document.body.innerHTML='<div id="stage"></div>';stage=document.getElementById('stage')
  player=await createPlayer({scriptsBase:'http://contract/scripts/',fetchImpl:fetchFiles,langBase:'',
    wasmFile:process.env.CAESURA_WASMOON_WASM??join(here,'node_modules/wasmoon/dist/glue.wasm')})
  renderer=new DomRenderer(player.core,stage,{width:320,height:180})
  // Same URL resolution host used by the existing real A→B scheduler contract.
  const get=renderer.textureUrls.get.bind(renderer.textureUrls)
  renderer.textureUrls.get=id=>get(id)??(player.core.textures.get(id)?.path
    ?new URL(player.core.textures.get(id).path,'http://contract/').href:null)
}
afterEach(async()=>{
  try{if(player)expect(await player.dispose()).toBe(true)}
  finally{renderer?.destroy();restoreCanvas?.();player=renderer=stage=restoreCanvas=null}
})
async function run(mode,source,name){
  if(mode==='source')return player.runScene(source,name,{maxFrames:100,autoClick:false})
  player.lua.global.set('__presentation_source',source);player.lua.global.set('__presentation_name',name)
  const bundle=await player.lua.doString(`
    local tokens=require('tokenizer').parse(__presentation_source)
    require('kag.compiler').compile(tokens)
    return {version=1,scenes={[__presentation_name]=require('kag.compiler').serialize(tokens)},assets={}}
  `)
  return player.runFromBundle(bundle,name,{maxFrames:100,autoClick:false})
}
function observeFrames(afterFrame){
  const frames=[],original=renderer._renderWithList
  renderer._renderWithList=function(...args){
    const result=original.apply(this,args)
    const state=player.core.transitionOverlay, overlay=stage.querySelector('.caesura-transition')
    if(state&&overlay){
      frames.push({frame:this._sceneFrame,from:state.from,to:state.to,revision:player.core._transitionRevision,
        progress:state.progress,opacity:Number(overlay.querySelector('.caesura-transition-to').style.opacity),
        a:overlay.querySelector('.caesura-transition-from img[data-layer="bg"]')?.getAttribute('src'),
        b:overlay.querySelector('.caesura-transition-to img[data-layer="bg"]')?.getAttribute('src')})
      afterFrame?.(frames.at(-1))
    }
    return result
  }
  return frames
}

it.each(['source','bundle'])('Web presentation contract: %s renders each transition progress once across the runner acknowledgement',async mode=>{
  await setup()
  await player.runScene('[bg storage="assets/bg/classroom.png"][text text="A scene"][p]',
    'presentation-a.ks',{maxFrames:100,autoClick:false})
  await renderer.render()
  const frames=observeFrames()
  const result=await run(mode,'[bg storage="assets/fg/girl_uniform.png"][trans time=64][eval exp="f.presentation_done=1"][end]',`presentation-${mode}.ks`)
  expect(result).toMatch(/^DONE:/)
  expect(await player.lua.doString('local c=require("kag_runner").get_ctx();return c.f.presentation_done==1 and c._pending_transition==nil and c._transition_render_wait==nil and #c.active_operations==0')).toBe(true)
  expect(player.core.sceneSnapshots.size).toBe(0)
  expect(player.core.transitionOverlay).toBeNull()
  const middle=frames.filter(f=>f.progress>0&&f.progress<1)
  expect(middle.length).toBeGreaterThan(0)
  expect(middle.every(f=>f.a?.endsWith('/assets/bg/classroom.png')&&f.b?.endsWith('/assets/fg/girl_uniform.png'))).toBe(true)
  expect(new Set(middle.map(f=>`${f.from}:${f.to}:${f.revision}`)).size).toBe(1)
  expect(frames.every((f,i)=>i===0||f.frame>frames[i-1].frame)).toBe(true)
  expect(middle.every(f=>Math.abs(f.opacity-f.progress)<1e-6)).toBe(true)
  expect(frames.some(f=>f.progress===1&&f.opacity===1)).toBe(true)
  const grouped=Object.groupBy(middle,f=>`${f.from}:${f.to}:${f.revision}:${f.progress}`)
  console.log('TRANSITION_PRESENTATION_FRAMES:'+JSON.stringify({mode,frames}))
  // Exact logical progress identity is the oracle, not wall time. A hold at
  // progress0 can legitimately span asset loading; middle progress cannot.
  for(const repeated of Object.values(grouped))expect(repeated,'same owned progress was presented again solely for runner acknowledgement').toHaveLength(1)
},20000)

it('Web presentation contract: zero duration does not invent capture or intermediate frames',async()=>{
  await setup()
  const frames=observeFrames()
  expect(await run('source','[trans time=0][eval exp="f.zero_done=1"][end]','presentation-zero.ks')).toMatch(/^DONE:/)
  expect(await player.lua.doString('return require("kag_runner").get_ctx().f.zero_done')).toBe(1)
  expect(frames).toHaveLength(0)
  expect(player.core.events.filter(e=>e.kind==='transition.capture')).toHaveLength(0)
  expect(player.core.sceneSnapshots.size).toBe(0)
},20000)

it('Web presentation contract: cancelling at an actual rendered frame cannot acknowledge stale work',async()=>{
  await setup()
  await player.runScene('[bg storage="assets/bg/classroom.png"][p]','presentation-cancel-a.ks',{maxFrames:100,autoClick:false})
  await renderer.render()
  let cancelled=false
  const frames=observeFrames(frame=>{
    if(frame.progress>0&&frame.progress<1&&!cancelled){cancelled=true;player.core.cancelTransition()}
  })
  let result,error
  try{result=await run('source','[trans time=64][eval exp="f.must_not_complete=1"][end]','presentation-cancel.ks')}
  catch(e){error=e}
  expect(cancelled).toBe(true)
  expect(frames.some(f=>f.progress>0&&f.progress<1)).toBe(true)
  expect(Boolean(error)||String(result).startsWith('ERR:')).toBe(true)
  expect(await player.lua.doString('local c=require("kag_runner").get_ctx();return c==nil or c.f.must_not_complete==nil')).toBe(true)
  expect(player.core.sceneSnapshots.size).toBe(0)
  expect(player.core.transitionOverlay).toBeNull()
},20000)

it('Web presentation contract: receipts are single-use and changed content or wrong owner needs a new real frame',async()=>{
  await setup()
  await player.runScene('[bg storage="assets/bg/classroom.png"][p]','receipt-a.ks',{maxFrames:100,autoClick:false})
  await renderer.render()
  const capture=async()=>Number((await player.core.captureScene()).split(':')[0])
  const from=await capture();await renderer.render();const to=await capture()
  expect(from).toBeGreaterThan(0);expect(to).toBeGreaterThan(from)
  expect(await player.core.submitTransition(1,from,to,0,0,.25)).toBe(true)
  const completed=renderer._sceneFrame
  expect(await player.core.presentScene({from,to,epoch:completed})).toBe(completed)
  expect(renderer._sceneFrame).toBe(completed)
  expect(await player.core.presentScene({from,to,epoch:completed})).toBeGreaterThan(completed)

  expect(await player.core.submitTransition(1,from,to,0,0,.5)).toBe(true)
  const beforeText=renderer._sceneFrame
  player.core.setDraws([{t:'NEW CONTENT MUST RENDER',x:12,y:20,r:255,g:255,b:255,a:255,s:1}])
  expect(await player.core.presentScene({from,to,epoch:beforeText})).toBeGreaterThan(beforeText)
  expect(stage.textContent).toContain('NEW CONTENT MUST RENDER')

  expect(await player.core.submitTransition(1,from,to,0,0,.75)).toBe(true)
  const beforeWrongOwner=renderer._sceneFrame
  expect(await player.core.presentScene({from:to,to:from,epoch:beforeWrongOwner})).toBeGreaterThan(beforeWrongOwner)
  player.core.cancelTransition();player.core.destroySceneSnapshot(from);player.core.destroySceneSnapshot(to)
  expect(player.core.sceneSnapshots.size).toBe(0)
},20000)

it.each(['size','active','family'])('Web presentation contract: font %s mutation invalidates a completed frame receipt',async change=>{
  await setup()
  const {createFontRestore}=await import('./restore-font.js')
  // Real font transaction for builtin presets needs no mocked decoder or font
  // download. Family-only control observes the actual DOM style input directly;
  // it is not a claim that a browser FontFace was decoded.
  const fonts=createFontRestore({core:player.core})
  const preset=font=>({version:1,active:true,font,path:'',size:font===0?16:32})
  fonts.apply_font(await fonts.prepare_font(preset(0)))
  player.core.setDraws([{t:'FONT CONTENT',x:12,y:20,r:255,g:255,b:255,a:255,s:1}])
  await renderer.render()
  expect(renderer._textEl.querySelector('span').style.fontSize).toBe('16px')
  expect(renderer._textEl.querySelector('span').style.fontFamily).toBe('monospace')
  const from=Number((await player.core.captureScene()).split(':')[0])
  await renderer.render()
  const to=Number((await player.core.captureScene()).split(':')[0])
  try{
    expect(await player.core.submitTransition(1,from,to,0,0,.5)).toBe(true)
    const before=renderer._sceneFrame
    // No drawings, layers, palette, texture owner, transition revision or
    // progress changes here. Only the real font input changes after completion.
    if(change==='size')fonts.apply_font(await fonts.prepare_font(preset(1)))
    else if(change==='active')fonts.clear_font()
    else player.core.font=Object.freeze({...player.core.font,family:'serif'})
    const acknowledged=await player.core.presentScene({from,to,epoch:before})
    const observed={change,before,acknowledged,actualFrame:renderer._sceneFrame,
      font:player.core.font,domTextPresent:renderer._textEl!==null,
      domSize:renderer._textEl?.querySelector('span')?.style.fontSize??null,
      domFamily:renderer._textEl?.querySelector('span')?.style.fontFamily??null}
    console.log('TRANSITION_FONT_RECEIPT:'+JSON.stringify(observed))
    expect.soft(acknowledged).toBeGreaterThan(before)
    expect.soft(renderer._sceneFrame).toBe(acknowledged)
    if(change==='size')expect.soft(observed.domSize).toBe('32px')
    else if(change==='active')expect.soft(observed.domTextPresent).toBe(false)
    else expect.soft(observed.domFamily).toBe('serif')
  }finally{
    player.core.cancelTransition()
    player.core.destroySceneSnapshot(from);player.core.destroySceneSnapshot(to)
    fonts.dispose()
  }
},20000)

// Append to the existing transition.presentation-contract.test.js only after
// the current run retires. Reuses its real setup/teardown; no fake presenter.
it('Web presentation contract: a resolved texture URL change requires a new actual frame',async()=>{
  await setup()
  const id=player.core.loadTexture('assets/bg/classroom.png')
  const node=player.core.ensureLayer('bg',{w:320,h:180,z:0,layer_type:4})
  player.core.setLayerImage(node,id)
  renderer.setTextureUrl(id,'http://contract/original-mapped.png')
  await renderer.render()
  const from=Number((await player.core.captureScene()).split(':')[0])
  await renderer.render()
  const to=Number((await player.core.captureScene()).split(':')[0])
  expect(from).toBeGreaterThan(0);expect(to).toBeGreaterThan(from)
  expect(await player.core.submitTransition(1,from,to,0,0,.25)).toBe(true)
  const before=renderer._sceneFrame
  const texture=player.core.textures.get(id)
  const image=renderer._els.get('bg')
  expect(image.getAttribute('src')).toBe('http://contract/original-mapped.png')
  // This is the maintained URL resolver API also used by main.mjs; core
  // texture identity/path and layer content intentionally remain unchanged.
  renderer.setTextureUrl(id,'http://contract/new-mapped.png')
  expect(player.core.textures.get(id)).toBe(texture)
  expect(texture.path).toBe('assets/bg/classroom.png')
  const actual=await player.core.presentScene({from,to,epoch:before})
  expect(actual).toBeGreaterThan(before)
  expect(renderer._els.get('bg').getAttribute('src')).toBe('http://contract/new-mapped.png')
  player.core.cancelTransition();player.core.destroySceneSnapshot(from);player.core.destroySceneSnapshot(to)
  expect(player.core.sceneSnapshots.size).toBe(0)
},20000)
