// @vitest-environment jsdom
import { beforeAll, expect, it } from 'vitest'
import { readFileSync, existsSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { createPlayer } from './bridge.js'

const here = dirname(fileURLToPath(import.meta.url))
const store = new Map()
const storageBackend = {
  get: key => store.get(key) ?? null,
  set: (key, value) => { store.set(key, value); return true },
  del: key => store.delete(key),
}
const options = {
  scriptsBase: 'http://local/scripts/',
  langBase: 'http://local/assets/lang/',
  wasmFile: join(here, 'node_modules/wasmoon/dist/glue.wasm'),
  storageBackend,
  fetchImpl: async url => {
    const pathname = new URL(url).pathname
    const path = pathname === '/scripts/index.json'
      ? join(here, 'scripts-index.json') : join(here, '..', pathname.slice(1))
    return {
      ok: existsSync(path), status: existsSync(path) ? 200 : 404,
      text: async () => existsSync(path) ? readFileSync(path, 'utf8') : '',
      json: async () => JSON.parse(readFileSync(path, 'utf8')),
    }
  },
}
let player
beforeAll(async () => { player = await createPlayer(options) })

it('failed external load preserves owner, coroutine, presentation and trusted provider', async () => {
  const source = '[ch text="KEEP"]\n[jump storage="next.ks"]'
  const scenes = { 'old.ks': source, 'next.ks': '[set f.continued = 7]\n[end]' }
  expect(await player.runScene(source, 'old.ks', { sceneSources: scenes })).toMatch(/^WAIT:/)
  await player.lua.doString('SAVED_OWNER=__CTXREF; SAVED_CO=__CO')
  const draws = structuredClone(player.core.draws)
  store.set('caesura.save.82', '{invalid')
  expect(await player.loadSlot(82, { sceneSources: { 'next.ks': '[set f.continued = 99]' } })).toMatch(/^ERR:/)
  expect(await player.lua.doString('return __CTXREF==SAVED_OWNER and __CO==SAVED_CO')).toBe(true)
  expect(player.core.draws).toEqual(draws)
  expect(await player.runScene('', 'old.ks', { advance: true, advanceScene: 'old.ks', autoClick: true })).toMatch(/^DONE:/)
  expect(await player.lua.doString('return __CTXREF.f.continued')).toBe(7)
})

it('inline load closes the old coroutine and publishes a distinct runner owner', async () => {
  const source = '[set f.answer = 4]\n[save slot=83]\n[ch text="SAVED"]\n[end]'
  await player.runScene(source, 'saved.ks', { autoClick: true })
  const loader = '[ch text="OLD"]\n[load slot=83]\n[set f.unreachable = 1]'
  await player.runScene(loader, 'loader.ks', { sceneSources: { 'saved.ks': source } })
  await player.lua.doString('OLD_OWNER=__CTXREF; OLD_CO=__CO')
  expect(await player.runScene(loader, 'loader.ks', { advance: true, advanceScene: 'loader.ks', autoClick: true })).toMatch(/^DONE:/)
  expect(await player.lua.doString(`return __CTXREF ~= OLD_OWNER and coroutine.status(OLD_CO)=='dead'
    and require('kag_runner').get_ctx()==__CTXREF and __CTXREF.f.answer==4 and __CTXREF.f.unreachable==nil`)).toBe(true)
})

it('cold load creates no synthetic scene and retains control bytes before digits', async () => {
  const source = '[ch text="SLOT"]\n[end]'
  await player.runScene(source, 'cold.ks')
  await player.lua.doString(' __CTXREF.f.value=string.char(1).."123"..string.char(0).."456" ')
  expect(await player.saveCurrent(84)).toBe(true)
  expect(JSON.parse(store.get('caesura.save.84')).state.f.value).toBe('\u0001123\u0000456')
  const cold = await createPlayer(options)
  expect(await cold.loadSlot(84, { sceneSources: { 'cold.ks': source }, autoClick: false })).toMatch(/^WAIT:/)
  expect(await cold.lua.doString('return __CTXREF.f.value==string.char(1).."123"..string.char(0).."456"')).toBe(true)
  expect(await cold.lua.doString('return require("kag_runner").get_ctx()==__CTXREF')).toBe(true)
  expect(cold.core.events.some(event => JSON.stringify(event).includes('Loading slot'))).toBe(false)
})

it('scene identifiers are passed as values including quote characters', async () => {
  expect(await player.runScene('[set f.valid = 1]\n[end]', "quoted'1.ks")).toMatch(/^DONE:/)
  expect(await player.lua.doString('return __CTXREF.f.valid')).toBe(1)
})

it('restores materialized color resources through the real shared layer transaction', async () => {
  const source = '[ch text="COLOR"]\n[end]'
  await player.runScene(source, 'color.ks')
  const previous = await player.lua.doString(`
    local L=require('layers')
    local node=L.ensure({},'paint',3)
    local id=backend.create_solid_texture(12,23,34,45)
    L.set_layer_image(node,id); node.w=40; node.h=20
    return id
  `)
  expect(await player.saveCurrent(85)).toBe(true)
  await player.lua.doString("require('layers').get('paint').x=999")
  expect(await player.loadSlot(85, { sceneSources: { 'color.ks': source }, autoClick: false })).toMatch(/^WAIT:/)
  const restored = player.core.getLayer('paint')
  expect(restored.x).toBe(0)
  expect(restored.texture).not.toBe(previous)
  expect(player.core.textures.get(restored.texture)).toMatchObject({
    loaded: true, width: 1, height: 1, source: { kind: 'color', r: 12, g: 23, b: 34, a: 45 },
  })
  let resolved = 0
  await player.linkTextures(() => { resolved++; return '/unused' })
  expect(player.core.textures.get(restored.texture).url).toBeUndefined()
  expect(resolved).toBeLessThan(player.core.textures.size)
})

it('rejects a competing drive while an asynchronous scheduler operation owns the VM', async () => {
  let release, entered
  const gate = new Promise(resolve => { release = resolve })
  const began = new Promise(resolve => { entered = resolve })
  player.lua.global.set('__WAIT_FOR_OWNER', () => { entered(); return gate })
  await player.lua.doString("require('kag').u11_wait_owner=function(ctx) __WAIT_FOR_OWNER():await(); ctx.f.done=1 end")
  const running = player.runScene('[u11_wait_owner]\n[end]', 'pending.ks')
  try {
    await began
    expect(await player.runScene('[set f.bad = 1]', 'competitor.ks')).toBe('ERR:player-busy')
    expect(await player.loadSlot(85)).toBe('ERR:player-busy')
    expect(await player.saveCurrent(86)).toBe(false)
  } finally { release() }
  expect(await running).toMatch(/^DONE:/)
  expect(await player.lua.doString('return __CTXREF.f.done==1 and __CTXREF.f.bad==nil')).toBe(true)
})

it('rejects unsafe JSON numbers and malformed table/string values without replacing a slot', async () => {
  await player.runScene('[ch text="VALID"]\n[end]', 'values.ks')
  expect(await player.saveCurrent(87)).toBe(true)
  const original = store.get('caesura.save.87')
  for (const value of ['9007199254740993', 'math.mininteger', '{[1]="kept",named="lost"}', '{[1]=1,[3]=3}', '0/0', 'string.char(255)']) {
    expect(await player.lua.doString(`return KAG.save_game(87,{f={value=${value}}},'values.ks',1,'')`)).toBe(false)
    expect(store.get('caesura.save.87')).toBe(original)
  }
  await player.lua.doString(' __CTXREF.f.value=9007199254740993 ')
  expect(await player.saveCurrent(87)).toBe(false)
  expect(store.get('caesura.save.87')).toBe(original)
})

it('preserves saved seen-skip when the load caller does not override it', async () => {
  const source = '[ch text="SKIP"]\n[end]'
  await player.runScene(source, 'skip.ks')
  await player.lua.doString("__CTXREF.skip_mode='seen'")
  expect(await player.saveCurrent(88)).toBe(true)
  expect(await player.loadSlot(88, {sceneSources: {'skip.ks':source}, autoClick:false, maxFrames:100})).toMatch(/^WAIT:/)
  expect(await player.lua.doString('return __CTXREF.skip_mode')).toBe('seen')
})

it('exports distinct same-name layer identities to the DOM-facing snapshot', async () => {
  await player.runScene('[end]', 'identity.ks')
  await player.lua.doString(`
    local L=require('layers')
    for _,id in ipairs({'a','b'}) do
      local node=L.add_layer(L.get_root(),{id=id,name='same',w=1,h=1})
      L.set_layer_image(node,backend.create_solid_texture(1,2,3,255))
    end
  `)
  const nodes=(await player.snapshotLayers()).filter(n=>n.name==='same')
  expect(nodes.map(n=>n.id).sort()).toEqual(['a','b'])
})

it('reuses ordinary solid colors without aliasing restored ownership', async () => {
  const ids=[]
  for (let count=0;count<5;count++) {
    await player.runScene('[ch text="CACHE"]', 'cache.ks')
    ids.push(await player.lua.doString('return backend.create_solid_texture(2,4,6,8)'))
  }
  expect(new Set(ids).size).toBe(1)
  const restored=await player.lua.doString('return Restore.materialize_image(Restore.prepare_color(2,4,6,8))')
  expect(restored).not.toBe(ids[0])
  player.core.destroyTexture(ids[0])
  expect(await player.lua.doString('return backend.create_solid_texture(2,4,6,8)')).not.toBe(ids[0])
  expect(player.core.textures.has(restored)).toBe(true)
})

it('rejects an uncaptured post effect and clears future effects when restoring a supported slot',async()=>{
  const source='[ch text="BEFORE-EFFECT"]\n[end]'
  await player.runScene(source,'effect.ks')
  expect(await player.saveCurrent(93)).toBe(true)
  const original=store.get('caesura.save.93')
  player.core.setPalette(player.core.loadTexture('assets/lut/night.png'),1,16)
  expect(await player.saveCurrent(93)).toBe(false)
  expect(store.get('caesura.save.93')).toBe(original)
  expect(await player.loadSlot(93,{sceneSources:{'effect.ks':source},autoClick:false})).toMatch(/^WAIT:/)
  expect(player.core.palette.handle).toBeNull()
})

it('save/load then cl keeps an absent textbox style absent without a handler error', async () => {
  const source = '[ch text="BEFORE"]\n[cl]\n[ch text="AFTER"]'
  await player.runScene(source, 'textbox-absent.ks')
  expect(await player.saveCurrent(94)).toBe(true)
  expect(JSON.parse(store.get('caesura.save.94')).state.textbox_style).toBeUndefined()
  expect(await player.loadSlot(94, {sceneSources:{'textbox-absent.ks':source}, autoClick:false})).toMatch(/^WAIT:/)
  await player.lua.doString(`
    local ctx=__CTXREF
    ctx.handle_error=function(command, reason) ctx.f.handler_error=command..':'..reason end
  `)
  expect(await player.runScene('', 'textbox-absent.ks', {advance:true, advanceScene:'textbox-absent.ks'})).toMatch(/^WAIT:/)
  expect(await player.lua.doString('return __CTXREF.f.handler_error==nil')).toBe(true)
  expect(await player.lua.doString('return __CTXREF.textbox_style==nil')).toBe(true)
  expect(player.core.draws.some(draw => draw.t === 'AFTER')).toBe(true)
})

it('save/load then cl preserves complete textbox style including zero and false', async () => {
  const source = '[textbox x=-12 y=42 w=640 h=96 color="12,34,56" opacity=0 visible=false]\n[ch text="STYLE"]\n[cl]\n[ch text="AFTER"]'
  await player.runScene(source, 'textbox-complete.ks')
  expect(await player.saveCurrent(95)).toBe(true)
  const expected = {x:-12,y:42,w:640,h:96,color:'12,34,56',opacity:0,visible:false}
  expect(JSON.parse(store.get('caesura.save.95')).state.textbox_style).toEqual(expected)
  expect(await player.loadSlot(95, {sceneSources:{'textbox-complete.ks':source}, autoClick:false})).toMatch(/^WAIT:/)
  expect(await player.runScene('', 'textbox-complete.ks', {advance:true, advanceScene:'textbox-complete.ks'})).toMatch(/^WAIT:/)
  expect(JSON.parse(JSON.stringify(await player.lua.doString('return __CTXREF.textbox_style')))).toEqual(expected)
  expect(player.core.getLayer('_textbox')).toMatchObject({x:-12,y:42,w:640,h:96,visible:false})
})

it('malformed textbox styles reject before replacing the live session', async () => {
  const source = '[ch text="KEEP"]\n[cl]\n[end]'
  await player.runScene(source, 'textbox-reject.ks')
  expect(await player.saveCurrent(96)).toBe(true)
  const valid = store.get('caesura.save.96')
  await player.lua.doString('TEXTBOX_OWNER=__CTXREF; TEXTBOX_CO=__CO')
  const draws = structuredClone(player.core.draws)
  const style = {x:0,y:0,w:640,h:96,color:'0,0,0',opacity:200,visible:true}
  for (const bad of [{}, {...style,color:null}, {...style,w:63}, {...style,opacity:256}, {...style,visible:'false'}]) {
    const payload = JSON.parse(valid)
    payload.state.textbox_style = bad
    store.set('caesura.save.96', JSON.stringify(payload))
    expect(await player.loadSlot(96, {sceneSources:{'textbox-reject.ks':source},autoClick:false})).toMatch(/^ERR:.*textbox_style/)
    expect(await player.lua.doString("return __CTXREF==TEXTBOX_OWNER and __CO==TEXTBOX_CO and coroutine.status(__CO)=='suspended'")).toBe(true)
    expect(player.core.draws).toEqual(draws)
  }
})
