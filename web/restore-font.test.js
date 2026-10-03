// @vitest-environment node
import {expect,it} from 'vitest'
import {createFontRestore} from './restore-font.js'

function setup(options={}) {
  let reads=0
  const fonts=new Set(), core={}
  class Face {
    constructor(family,bytes) {this.family=family;this.bytes=new Uint8Array(bytes);this.status='unloaded'}
    async load() {if(this.bytes[0]!==42) throw new Error('invalid font');this.status='loaded';return this}
  }
  const api=createFontRestore({core,fontSet:fonts,FontFaceClass:Face,assetUrl:path=>path,
    fetchImpl:async path=>{reads++;return new Response(new Uint8Array([path.includes('bad')?0:42]))},...options})
  return {api,core,fonts,reads:()=>reads}
}
const font={version:1,active:true,font:2,path:'assets/fonts/test.otf',size:24}

it('prepares decoded font bytes before publication and applies without reading again',async()=>{
  const {api,core,fonts,reads}=setup()
  const prepared=await api.prepare_font(font)
  expect(fonts.size).toBe(0)
  expect(core.font).toBeUndefined()
  expect(api.apply_font(prepared)).toBe(true)
  expect(reads()).toBe(1)
  expect(fonts.size).toBe(1)
  expect(core.font.size).toBe(24)
  expect(api.capture_font()).toEqual(font)
  expect(()=>api.apply_font(prepared)).toThrow(/consumed/)
  api.discard_font(prepared)
  expect(fonts.size).toBe(1)
})

it('rejects decode failure and invalid paths without replacing the active font',async()=>{
  const {api,core,fonts}=setup()
  api.apply_font(await api.prepare_font(font))
  const original=core.font
  await expect(api.prepare_font({...font,path:'assets/fonts/bad.otf'})).rejects.toThrow('invalid font')
  for(const patch of [{path:'../escape.otf'},{size:257},{size:NaN},{font:9}]) {
    await expect(api.prepare_font({...font,...patch})).rejects.toThrow()
  }
  expect(core.font).toBe(original)
  expect(fonts.size).toBe(1)
})

it('discards preparation, replaces only its own face and clears idempotently',async()=>{
  const {api,fonts}=setup()
  const unrelated={family:'Other'};fonts.add(unrelated)
  const discarded=await api.prepare_font(font)
  api.discard_font(discarded);api.discard_font(discarded)
  expect(()=>api.apply_font(discarded)).toThrow(/consumed/)
  api.apply_font(await api.prepare_font(font))
  api.apply_font(await api.prepare_font({...font,size:30}))
  expect(fonts.size).toBe(2)
  expect(api.clear_font()).toBe(true)
  expect(api.clear_font()).toBe(true)
  expect([...fonts]).toEqual([unrelated])
  expect(api.capture_font()).toEqual({version:1,active:false})
})

it('bitmap presets and explicit inactive snapshots require no encoded asset',async()=>{
  const {api,reads,core}=setup()
  for(const index of [0,1]) {
    const state={version:1,active:true,font:index,path:'',size:index===0?16:32}
    api.apply_font(await api.prepare_font(state))
    expect(api.capture_font()).toEqual(state)
  }
  api.apply_font(await api.prepare_font({version:1,active:false}))
  expect(core.font.active).toBe(false)
  expect(reads()).toBe(0)
})

it('selection validates before mutation and a disposed host rejects new work',async()=>{
  const {api,core}=setup()
  expect(await api.select_font('assets/fonts/test.otf',24)).toBe(true)
  const original=core.font
  expect(await api.select_font('assets/fonts/bad.otf',24)).toBe(false)
  expect(core.font).toBe(original)
  api.dispose()
  await expect(api.prepare_font(font)).rejects.toThrow(/closed/)
})

it('a newer font selection supersedes an older request before either finishes',async()=>{
  const waiting=new Map()
  let signal
  const entered=new Promise(resolve=>{signal=resolve})
  const {api}=setup({fetchImpl:path=>new Promise(resolve=>{waiting.set(path,resolve);if(waiting.size===2) signal()})})
  const first=api.select_font('first.otf',20)
  const second=api.select_font('second.otf',30)
  await entered
  waiting.get('first.otf')(new Response(new Uint8Array([42])))
  const firstResult=await first
  waiting.get('second.otf')(new Response(new Uint8Array([42])))
  const secondResult=await second
  expect(firstResult).toBe(false)
  expect(secondResult).toBe(true)
  expect(api.capture_font().path).toBe('second.otf')
})

it('failed face deletion hides the font immediately and retains ownership for retry',async()=>{
  const {api,core,fonts}=setup()
  api.apply_font(await api.prepare_font(font))
  const remove=fonts.delete.bind(fonts)
  fonts.delete=()=>{throw new Error('font deletion failed')}
  expect(()=>api.clear_font()).toThrow('font deletion failed')
  expect(core.font.active).toBe(false)
  expect(api.capture_font()).toEqual({version:1,active:false})
  expect(fonts.size).toBe(1)
  fonts.delete=remove
  expect(api.clear_font()).toBe(true)
  expect(fonts.size).toBe(0)
})

function resourceSetup() {
  const core={},fonts=new Set(),faces=[],reads=[]
  let bytes=new Uint8Array([42,1]), missing=false
  class Face {
    constructor(family,buffer){this.family=family;this.bytes=new Uint8Array(buffer);this.status='unloaded';faces.push(this)}
    async load(){if(this.bytes[0]!==42)throw new Error('invalid changed font');this.status='loaded';return this}
  }
  const api=createFontRestore({core,fontSet:fonts,FontFaceClass:Face,assetUrl:path=>path,
    fetchImpl:async path=>{reads.push(path);return missing?new Response(null,{status:404}):new Response(bytes)}})
  return {api,core,fonts,faces,reads,setBytes:value=>{bytes=new Uint8Array(value)},setMissing:value=>{missing=value}}
}

it('reuses the active decoded face only after re-reading identical bytes, including size and path changes',async()=>{
  const {api,core,fonts,faces,reads}=resourceSetup()
  api.apply_font(await api.prepare_font(font))
  const original=[...fonts][0], deleted=[]
  const remove=fonts.delete.bind(fonts);fonts.delete=face=>{deleted.push(face);return remove(face)}
  api.apply_font(await api.prepare_font({...font,size:30}))
  api.apply_font(await api.prepare_font({...font,path:'assets/fonts/alias.otf',size:36}))
  expect(reads).toEqual([font.path,font.path,'assets/fonts/alias.otf'])
  expect(faces).toHaveLength(1)
  expect([...fonts]).toEqual([original])
  expect(deleted).toEqual([])
  expect(core.font.size).toBe(36)
  expect(api.capture_font().path).toBe('assets/fonts/alias.otf')
})

it('same-path changed or missing bytes cannot hide behind a loaded face',async()=>{
  const {api,core,fonts,faces,reads,setBytes,setMissing}=resourceSetup()
  api.apply_font(await api.prepare_font(font))
  const first=[...fonts][0]
  setBytes([42,2]);api.apply_font(await api.prepare_font(font))
  const second=[...fonts][0],selected=core.font
  expect(second).not.toBe(first)
  expect(faces).toHaveLength(2)
  setBytes([0,2]);await expect(api.prepare_font(font)).rejects.toThrow('invalid changed font')
  expect(core.font).toBe(selected);expect([...fonts]).toEqual([second])
  setMissing(true);await expect(api.prepare_font(font)).rejects.toThrow(/404/)
  expect(reads).toHaveLength(4)
  expect(core.font).toBe(selected);expect([...fonts]).toEqual([second])
})

it('concurrent tickets retain their exact decoded resource across later font publication',async()=>{
  const {api,fonts,faces,setBytes}=resourceSetup()
  api.apply_font(await api.prepare_font(font))
  const first=[...fonts][0]
  const saved=await api.prepare_font({...font,size:31})
  const discarded=await api.prepare_font({...font,size:32})
  setBytes([42,2]);const changed=await api.prepare_font({...font,size:40})
  api.discard_font(discarded)
  expect([...fonts]).toEqual([first])
  expect(()=>api.apply_font(discarded)).toThrow(/consumed/)
  api.apply_font(changed)
  expect([...fonts][0]).not.toBe(first)
  api.apply_font(saved)
  expect([...fonts]).toEqual([first])
  expect(api.capture_font().size).toBe(31)
  expect(faces).toHaveLength(2)
  expect(()=>api.apply_font(saved)).toThrow(/consumed/)
})

it('re-adds an externally removed active face without decoding or deleting it',async()=>{
  const {api,fonts,faces}=resourceSetup()
  api.apply_font(await api.prepare_font(font))
  const face=[...fonts][0];fonts.delete(face)
  const ticket=await api.prepare_font({...font,size:29})
  api.apply_font(ticket)
  expect(faces).toHaveLength(1)
  expect([...fonts]).toEqual([face])
})

it('a failed same-face publication does not remove the still-active face',async()=>{
  const {api,core,fonts}=resourceSetup()
  api.apply_font(await api.prepare_font(font))
  const face=[...fonts][0],previous=core.font
  const ticket=await api.prepare_font({...font,size:29})
  const add=fonts.add.bind(fonts);fonts.add=()=>{throw new Error('font add rejected')}
  expect(()=>api.apply_font(ticket)).toThrow('font add rejected')
  expect([...fonts]).toEqual([face]);expect(core.font).toBe(previous)
  fonts.add=add
  api.apply_font(await api.prepare_font({...font,size:30}))
  expect([...fonts]).toEqual([face])
})

it('clear releases the reusable active resource while existing tickets remain independently owned',async()=>{
  const {api,fonts,faces}=resourceSetup()
  api.apply_font(await api.prepare_font(font))
  const face=[...fonts][0],prepared=await api.prepare_font({...font,size:25})
  api.clear_font();expect(fonts.size).toBe(0)
  api.apply_font(prepared);expect([...fonts]).toEqual([face])
  api.clear_font()
  api.apply_font(await api.prepare_font(font))
  expect([...fonts][0]).not.toBe(face)
  expect(faces).toHaveLength(2)
})

it('unloaded active faces are not reused and disposal invalidates pending shared tickets',async()=>{
  const {api,fonts,faces}=resourceSetup()
  api.apply_font(await api.prepare_font(font))
  const first=[...fonts][0];first.status='error'
  api.apply_font(await api.prepare_font(font))
  expect(faces).toHaveLength(2);expect([...fonts][0]).not.toBe(first)
  const shared=await api.prepare_font(font)
  expect(faces).toHaveLength(2)
  api.dispose();expect(fonts.size).toBe(0)
  expect(()=>api.apply_font(shared)).toThrow(/consumed/)
  await expect(api.prepare_font(font)).rejects.toThrow(/closed/)
})
