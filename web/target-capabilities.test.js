// @vitest-environment jsdom
// U19: actual Wasmoon/backend/runner/DOM; no copied capability resolver.
import { afterEach, expect, it } from 'vitest'
import { existsSync, readFileSync, statSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { createPlayer } from './bridge.js'
import { DomRenderer } from './dom-renderer.js'

const here = dirname(fileURLToPath(import.meta.url))
const root = join(here, '..')
let player

afterEach(async () => {
  if (player) expect(await player.dispose()).toBe(true)
  player = null
})

async function boot(capabilities) {
  player = await createPlayer({
    capabilities,
    scriptsBase: 'http://local/scripts/',
    langBase: 'http://local/assets/lang/',
    wasmFile: join(here, 'node_modules/wasmoon/dist/glue.wasm'),
    fetchImpl: async url => {
      const path = join(root, new URL(url).pathname.slice(1))
      const available = existsSync(path) && statSync(path).isFile()
      return {
        ok: available, status: available ? 200 : 404,
        text: async () => available ? readFileSync(path, 'utf8') : '',
        json: async () => JSON.parse(readFileSync(join(here, 'scripts-index.json'), 'utf8')),
      }
    },
  })
  return player
}

it.each(['bloom', 'vignette', 'softblur', 'not-a-postfx'])(
  'Web never reports an unimplemented %s postfx as supported', async kind => {
    await boot()
    player.lua.global.set('__U19_KIND', kind)
    expect(await player.lua.doString('return backend.is_postfx_supported(__U19_KIND)')).toBe(false)
  })

it.each(['[particles action="create"]', '[vfx type="particle" action="create"]'])(
  'Web %s cannot publish a zero particle owner', async command => {
    await boot({optional: ['render.particles']})
    const result = await player.runScene(command + '\n[p]\n[end]', 'u19-particles.ks', { autoClick: false })
    expect.soft(result.startsWith('WAIT:')).toBe(true)
    expect(await player.lua.doString(`
      local c = require('kag_runner').get_ctx()
      return c ~= nil and (c._particleEmitters == nil or next(c._particleEmitters) == nil)
    `)).toBe(true)
  })

it('the existing CSS palette operation reports application and actually clears its effect', async () => {
  await boot({accept_approximate: ['render.postfx.lut3d']})
  const stage = document.createElement('div')
  const renderer = new DomRenderer(player.core, stage)
  const applied = await player.lua.doString(`
    local p = require('palette')
    assert(p.load('u19-tint', 'assets/lut/u19-night.png'))
    return p.apply('u19-tint', 0.8)
  `)
  expect.soft(applied).toBe(true)
  expect.soft(player.core.palette.intensity).toBe(0.8)
  await renderer.render()
  expect.soft(stage.style.filter).toContain('brightness')
  expect(await player.lua.doString("return require('palette').clear()")).toBe(true)
  await renderer.render()
  expect(stage.style.filter).toBe('')
})

it('uses the actual Web host profile and rejects required capabilities before author execution', async () => {
  await expect(boot({required: ['video.play']})).rejects.toThrow('capability configuration rejected')
  await boot()
  expect(await player.lua.doString(`
    local q=backend.get_capability('video.play')
    return q.proven and q.target=='web' and q.scope=='runtime' and q.status=='unsupported'
  `)).toBe(true)
})

it('an undeclared required command stops before allocating or advancing its successor', async () => {
  await boot()
  await player.runScene('[particles action="create"]\n[set var="f.advanced" value=1]\n[end]', 'u19-required.ks')
  expect(await player.lua.doString(`
    local c=require('kag_runner').get_ctx()
    return c._command_error==true and c.f.advanced==nil
      and (c._particleEmitters==nil or next(c._particleEmitters)==nil)
      and c.capability_diagnostics[1].status=='unsupported'
  `)).toBe(true)
})

it('direct dynamic calls return explicit unsupported and approximation results', async () => {
  await boot({accept_approximate: ['render.postfx.lut3d']})
  expect(await player.lua.doString(`
    local kind='bloom'
    local value,result=backend.set_postfx(kind,{})
    return value==0 and result.status=='unsupported' and result.target=='web'
  `)).toBe(true)
  expect(await player.lua.doString(`
    local p=require('palette'); assert(p.load('accepted','assets/lut/night.png'))
    local applied,result=p.apply('accepted',0.5)
    return applied==true and result.status=='approximate'
  `)).toBe(true)
})

it('the same approximation is refused without author acceptance', async () => {
  await boot()
  expect(await player.lua.doString(`
    local p=require('palette'); assert(p.load('unaccepted','assets/lut/night.png'))
    local applied,result=p.apply('unaccepted',0.5)
    return applied==false and result.status=='unsupported'
  `)).toBe(true)
  expect(player.core.palette.handle).toBe(null)
})

it('two actual players keep independent author policies and returned capability values', async () => {
  const accepted = await boot({accept_approximate: ['render.postfx.lut3d']})
  player = null
  try {
    const strict = await boot()
    expect(await accepted.lua.doString(`
      local p=require('palette'); assert(p.load('first','assets/lut/night.png'))
      local value,result=p.apply('first',0.4)
      return value==true and result.status=='approximate'
    `)).toBe(true)
    expect(await strict.lua.doString(`
      local q=backend.get_capability('render.postfx.lut3d'); q.status='supported'
      local p=require('palette'); assert(p.load('second','assets/lut/night.png'))
      local value,result=p.apply('second',0.4)
      return value==false and result.status=='unsupported'
        and backend.get_capability('render.postfx.lut3d').status=='approximate'
    `)).toBe(true)
    expect(accepted.core.palette.intensity).toBe(0.4)
    expect(strict.core.palette.handle).toBe(null)
  } finally {
    expect(await accepted.dispose()).toBe(true)
  }
})

it('unsupported video has no playback owner and a fresh ordinary scene remains usable', async () => {
  await boot()
  expect(await player.lua.doString("return backend.video_play('missing.mpg', {})")).toBe(0)
  expect(await player.lua.doString('return backend.video_is_playing(0)')).toBe(false)
  expect((await player.runScene('[set var="f.u19" value=1]\n[end]', 'u19-healthy.ks')).startsWith('DONE:')).toBe(true)
  expect(await player.lua.doString("return require('kag_runner').get_ctx().f.u19")).toBe(1)
})

it.each([
  ['live2d_motion', 'motion="idle"', 'motion="idle"'],
  ['live2d_expression', 'expression="smile"', 'expression="smile"'],
  ['live2d_lip_sync', 'value=0.8', 'value=0.8'],
])('dynamic kag.%s enforces the same capability as its tag before state changes', async (command, tag, params) => {
  await boot()
  await player.runScene(`[${command} model="m" ${tag}]\n[end]`, 'tag-capability.ks')
  expect(await player.lua.doString(`
    local c=require('kag_runner').get_ctx()
    return c._command_error==true and c.live2d==nil
  `)).toBe(true)
  const result = await player.runScene(`[iscript]
local ok,value=pcall(kag.${command},ctx,{model="m",${params}})
f.denied=not ok
f.detail=tostring(value)
[endscript]
[set var="f.continued" value=1]
[end]`, 'dynamic-capability.ks')
  expect(result.startsWith('DONE:')).toBe(true)
  expect(await player.lua.doString(`
    local c=require('kag_runner').get_ctx()
    return c.f.denied==true and c.f.continued==1 and c.live2d==nil
      and c.f.detail:find('kag.${command}',1,true)~=nil
      and c.capability_diagnostics[1].status=='unsupported'
  `)).toBe(true)
})

it('optional dynamic KAG calls return unsupported without pretending to update state', async () => {
  await boot({optional: ['kag.live2d_motion']})
  await player.runScene(`[iscript]
local value,result=kag.live2d_motion(ctx,{model="m",motion="idle"})
f.skipped=value==false and result and result.status=='unsupported'
[endscript]
[set var="f.continued" value=1]
[end]`, 'optional-dynamic-capability.ks')
  expect(await player.lua.doString(`
    local c=require('kag_runner').get_ctx()
    return c.f.skipped==true and c.live2d==nil and c.f.continued==1
  `)).toBe(true)
})

it.each(['0', 'nil'])('palette cleanup alias with %s still clears after author acceptance is withdrawn', async handle => {
  await boot({accept_approximate: ['render.postfx.lut3d']})
  const stage = document.createElement('div')
  const renderer = new DomRenderer(player.core, stage)
  expect(await player.lua.doString(`
    local texture=backend.load_texture('assets/lut/night.png')
    local applied,result=backend.set_palette(texture,0.6,16)
    return applied==true and result.status=='approximate'
  `)).toBe(true)
  await renderer.render()
  expect(stage.style.filter).toContain('brightness')
  expect(await player.lua.doString(`return require('capability_runtime').configure_project_json('{}')`)).toBe(true)
  expect(await player.lua.doString(`return backend.set_palette(${handle},0,0)`)).toBe(true)
  await renderer.render()
  expect(stage.style.filter).toBe('')
  expect(player.core.palette.handle).toBeNull()
})

it('night and toggle helpers retain their real mode when the approximation is refused', async () => {
  await boot()
  expect(await player.lua.doString(`
    local p=require('palette')
    local applied,result=p.set_night_mode()
    local mode,toggled=p.toggle_mode()
    return applied==false and result.status=='unsupported' and p.get_mode()=='day'
      and mode=='day' and toggled.status=='unsupported'
  `)).toBe(true)
  expect(player.core.palette.handle).toBeNull()
})
