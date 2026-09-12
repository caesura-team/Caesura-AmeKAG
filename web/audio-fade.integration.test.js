// @vitest-environment node
// Actual Wasmoon, capability resolver, KAG handlers, bridge and AudioEngine.
// Only WebAudio/asset host boundaries are controlled; scheduling records are
// not evidence of a rendered waveform or audible browser/device playback.
import { afterEach, describe, expect, it, vi } from 'vitest'
import { existsSync, readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { createPlayer } from './bridge.js'

const here = dirname(fileURLToPath(import.meta.url))
const asset = name => `assets/u20-audio/${name}.wav`
const deferred = () => {
  let resolve, reject
  const promise = new Promise((yes, no) => { resolve = yes; reject = no })
  return { promise, resolve, reject }
}
let player, host
const pendingLua = []

afterEach(async () => {
  for (const release of host?.releases ?? []) release()
  await Promise.allSettled(pendingLua.splice(0))
  if (player) expect(await player.dispose()).toBe(true)
  player = null
  host = null
  vi.unstubAllGlobals()
})

function audioHost() {
  const sources = [], gains = [], releases = [], routes = new Map(), decodeGates = new Map()
  let nextCode = 1
  const buffer = { duration: 4, length: 192000, sampleRate: 48000, numberOfChannels: 1 }
  const ctx = { currentTime: 10, state: 'running', destination: {} }
  // Minimal AudioParam host timeline. Production scheduling is never replaced.
  function parameter() {
    let initial = 1, sequence = 0, events = []
    const calls = []
    function valueAt(time) {
      let value = initial, previousTime = 0
      for (const event of [...events].sort((a, b) => a.time - b.time || a.sequence - b.sequence)) {
        if (time < event.time) {
          if (event.method === 'linearRampToValueAtTime' && event.time > previousTime) {
            return value + (event.value - value) * Math.max(0, (time - previousTime) / (event.time - previousTime))
          }
          return value
        }
        value = event.value; previousTime = event.time
      }
      return value
    }
    function schedule(method, value, time) {
      if (!Number.isFinite(value) || !Number.isFinite(time) || time < 0) throw new Error('Invalid host automation')
      const event = { method, value, time, sequence: sequence++ }
      calls.push(event); events.push(event)
      return param
    }
    const param = {
      calls,
      get value() { return valueAt(ctx.currentTime) },
      set value(value) { initial = value; schedule('setValueAtTime', value, ctx.currentTime) },
      setValueAtTime: (value, time) => schedule('setValueAtTime', value, time),
      linearRampToValueAtTime: (value, time) => schedule('linearRampToValueAtTime', value, time),
      cancelScheduledValues(time) {
        calls.push({ method: 'cancelScheduledValues', time })
        events = events.filter(event => event.time < time)
        return param
      },
      cancelAndHoldAtTime(time) {
        const value = valueAt(time)
        calls.push({ method: 'cancelAndHoldAtTime', time })
        events = events.filter(event => event.time < time)
        return schedule('setValueAtTime', value, time)
      },
    }
    return param
  }
  ctx.createGain = vi.fn(() => {
    const node = { gain: parameter(), connect: vi.fn(), disconnect: vi.fn() }
    gains.push(node)
    return node
  })
  ctx.createBufferSource = vi.fn(() => {
    const source = { buffer: null, loop: false, connect: vi.fn(), disconnect: vi.fn(), ended: false }
    source.start = vi.fn((when = 0, offset = 0) => {
      source.startedAt = Math.max(when, ctx.currentTime); source.offset = offset
    })
    source.stop = vi.fn((when = 0) => { source.stopAt = Math.max(when, ctx.currentTime) })
    source.finish = () => {
      if (source.ended) return
      source.ended = true
      source.onended?.()
    }
    sources.push(source)
    return source
  })
  ctx.decodeAudioData = vi.fn(async bytes => {
    expect(bytes.byteLength).toBe(4)
    const code = new Uint8Array(bytes)[0]
    if (code === 255) throw new Error('controlled decoder rejection')
    const gate = decodeGates.get(code)
    if (gate) { gate.entered.resolve(); await gate.release.promise }
    return buffer
  })
  ctx.resume = vi.fn(async () => { ctx.state = 'running' })
  ctx.suspend = vi.fn(async () => { ctx.state = 'suspended' })
  ctx.close = vi.fn(async () => { ctx.state = 'closed' })
  const response = code => new Response(new Uint8Array([code, 2, 3, 4]))
  function add(path, mode = 'ok') {
    const code = mode === 'decode-failure' ? 255 : nextCode++
    const entered = deferred(), release = deferred()
    const route = { code, mode, entered, release }
    routes.set('/' + path, route)
    if (mode === 'defer-decode') decodeGates.set(code, route)
    releases.push(() => release.resolve())
    return route
  }
  async function fetchImpl(url) {
    const pathname = new URL(url).pathname
    const route = routes.get(pathname)
    if (route) {
      if (route.mode === 'fetch-failure') return new Response('', { status: 404 })
      if (route.mode === 'defer-fetch') { route.entered.resolve(); await route.release.promise }
      return response(route.code)
    }
    const path = pathname === '/scripts/index.json'
      ? join(here, 'scripts-index.json') : join(here, '..', pathname.slice(1))
    return new Response(existsSync(path) ? readFileSync(path) : '', { status: existsSync(path) ? 200 : 404 })
  }
  function advance(time) {
    expect(time).toBeGreaterThanOrEqual(ctx.currentTime)
    ctx.currentTime = time
    for (const source of sources) {
      if (source.startedAt === undefined || source.ended) continue
      const naturalEnd = source.loop ? Infinity : source.startedAt + source.buffer.duration - source.offset
      if (time >= Math.min(source.stopAt ?? Infinity, naturalEnd)) source.finish()
    }
  }
  return { ctx, sources, gains, releases, add, fetchImpl, advance }
}

async function boot() {
  host = audioHost()
  vi.stubGlobal('FontFace', undefined)
  player = await createPlayer({
    scriptsBase: 'http://local/scripts/', langBase: 'http://local/assets/lang/',
    wasmFile: join(here, 'node_modules/wasmoon/dist/glue.wasm'),
    audioContext: host.ctx, audioAssetUrl: path => 'http://local/' + path,
    fetchImpl: host.fetchImpl,
  })
  player.audio.setBusVolume('bgm', 0.6)
  player.audio.setBusVolume('se', 0.4)
  player.audio.setBusVolume('voice', 0.8)
  return host
}
function clipGain(source) { return source.connect.mock.calls[0][0].gain }
function ramps(param) { return param.calls.filter(call => call.method === 'linearRampToValueAtTime') }
function unchangedBuses() {
  expect(player.audio._busVolumes.get('bgm')).toBe(0.6)
  expect(player.audio._busVolumes.get('se')).toBe(0.4)
  expect(player.audio._busVolumes.get('voice')).toBe(0.8)
  for (const bus of ['bgm', 'se', 'voice']) expect(ramps(player.audio._busGains.get(bus).gain)).toEqual([])
}
async function playConfirmed(path, loop = true) {
  return player.lua.doString(`
    local value,result=backend.audio_play('bgm',${JSON.stringify(path)},{fadein=0,volume=0.4,loop=${loop}})
    return value==true and result.status=='applied'
  `)
}

describe('U20 actual Lua gain fade routes', () => {
  it('declares audio.fade supported only through the actual runtime profile', async () => {
    await boot()
    expect(await player.lua.doString(`
      local q=backend.get_capability('audio.fade')
      return q.proven and q.target=='web' and q.scope=='runtime' and q.status=='supported'
    `)).toBe(true)
  })

  it('playbgm converts fadein milliseconds to a clip ramp without changing user buses', async () => {
    const h = await boot(); h.add(asset('fade-in'))
    expect(await player.runScene(`[playbgm file="${asset('fade-in')}" fadein=2000 volume=0.4 loop=true]\n[end]`, 'u20-fade-in.ks')).toMatch(/^DONE:/)
    expect(h.sources).toHaveLength(1)
    const gain = clipGain(h.sources[0])
    expect(gain.value).toBe(0)
    expect(ramps(gain)).toEqual([expect.objectContaining({ value: 0.4, time: 12 })])
    unchangedBuses()
    h.advance(11)
    expect(gain.value).toBeCloseTo(0.2)
    h.advance(12)
    expect(gain.value).toBeCloseTo(0.4)
    expect(player.audio.isPlaying('bgm')).toBe(true)
  })

  it.each(['fadebgm', 'fadevol'])('%s fades only the bus and direct fade returns typed applied', async command => {
    const h = await boot(); h.add(asset('bus-fade'))
    expect(await player.runScene(`[playbgm file="${asset('bus-fade')}" volume=0.4]\n[${command} volume=0.3 time=1250]\n[end]`, 'u20-bus-fade.ks')).toMatch(/^DONE:/)
    expect(h.sources).toHaveLength(1)
    expect(clipGain(h.sources[0]).value).toBe(0.4)
    const bus = player.audio._busGains.get('bgm').gain
    expect(ramps(bus)).toEqual([expect.objectContaining({ value: 0.3, time: 11.25 })])
    expect(player.audio._busVolumes.get('bgm')).toBe(0.3)
    expect(player.audio._busVolumes.get('se')).toBe(0.4)
    expect(player.audio._busVolumes.get('voice')).toBe(0.8)
    expect(h.sources[0].stop).not.toHaveBeenCalled()
    expect(await player.lua.doString(`
      local value,result=backend.audio_fade_volume('bgm',0.25,0.5)
      return value~=false and result and result.status=='applied' and result.feature=='audio.fade'
    `)).toBe(true)
    expect(ramps(bus).at(-1)).toMatchObject({ value: 0.25, time: 10.5 })
    expect(clipGain(h.sources[0]).value).toBe(0.4)
  })

  it('stopbgm fades its old clip for exactly the requested time and late ended cannot erase a new source', async () => {
    const h = await boot(); h.add(asset('stop-old')); h.add(asset('stop-new'))
    expect(await player.runScene(`[playbgm file="${asset('stop-old')}" volume=0.4]\n[stopbgm fadeout=750]\n[end]`, 'u20-stop.ks')).toMatch(/^DONE:/)
    const old = h.sources[0], oldEnded = old.onended
    expect(old.stop).toHaveBeenCalledOnce()
    expect(old.stop).toHaveBeenCalledWith(10.75)
    expect(ramps(clipGain(old))).toEqual([expect.objectContaining({ value: 0, time: 10.75 })])
    expect(old.disconnect).not.toHaveBeenCalled()
    unchangedBuses()
    expect(await playConfirmed(asset('stop-new'))).toBe(true)
    const current = h.sources.at(-1)
    expect(clipGain(current).value * player.audio._busGains.get('bgm').gain.value).toBeCloseTo(0.24)
    oldEnded()
    expect(player.audio.isPlaying('bgm')).toBe(true)
    expect(player.audio.captureBgm().path).toBe(asset('stop-new'))
    expect(current.stop).not.toHaveBeenCalled()
    expect(await player.lua.doString("return backend.audio_is_playing('bgm')")).toBe(true)
    expect(player.core.audioBus.bgm).toMatchObject({ path: asset('stop-new'), playing: true })
  })

  it('playbgmstop routes precise clip stop then fadein while retaining a nonzero bus', async () => {
    const h = await boot(); h.add(asset('swap-old')); h.add(asset('swap-new'))
    expect(await player.runScene(`[playbgm file="${asset('swap-old')}" volume=0.4]\n[playbgmstop file="${asset('swap-new')}" fadeout=600 fadein=1200 volume=0.2]\n[end]`, 'u20-swap.ks')).toMatch(/^DONE:/)
    expect(h.sources).toHaveLength(2)
    expect(h.sources[0].stop).toHaveBeenCalledOnce()
    expect(h.sources[0].stop).toHaveBeenCalledWith(10.6)
    expect(ramps(clipGain(h.sources[0]))).toEqual([expect.objectContaining({ value: 0, time: 10.6 })])
    expect(ramps(clipGain(h.sources[1]))).toEqual([expect.objectContaining({ value: 0.2, time: 11.2 })])
    unchangedBuses()
    h.advance(11.2)
    expect(player.audio.captureBgm().path).toBe(asset('swap-new'))
    expect(clipGain(h.sources[1]).value * player.audio._busGains.get('bgm').gain.value).toBeCloseTo(0.12)
  })
})

describe('U20 bridge publishes confirmed playback truth', () => {
  it.each([false, true])('a started result publishes only the current playback state (stop before publish: %s)', async stopBeforePublish => {
    const h = await boot(); h.add(asset('publish-window'))
    const createSource = h.ctx.createBufferSource.getMockImplementation()
    h.ctx.createBufferSource.mockImplementationOnce(() => {
      const source = createSource()
      const start = source.start.getMockImplementation()
      source.start.mockImplementation((...args) => {
        start(...args)
        if (stopBeforePublish) queueMicrotask(() => player.audio.stop('bgm'))
      })
      return source
    })
    // The operation really started, so its first return and applied result
    // stay successful even if a later microtask stops it before UI publication.
    expect(await playConfirmed(asset('publish-window'))).toBe(true)
    expect(h.sources).toHaveLength(1)
    expect(h.sources[0].start).toHaveBeenCalledOnce()
    expect(h.sources[0].stop).toHaveBeenCalledTimes(stopBeforePublish ? 1 : 0)
    expect(player.audio.isPlaying('bgm')).toBe(!stopBeforePublish)
    // Do not call backend.audio_is_playing here: its separate UI reconciliation
    // would conceal this actual start-to-publish ordering defect.
    expect(player.core.audioBus.bgm?.playing ?? false).toBe(!stopBeforePublish)
  })

  it('a started result cannot replace a newer restored owner in the UI', async () => {
    const h = await boot(); h.add(asset('requested-owner')); h.add(asset('restored-owner'))
    await player.lua.doString(`
      __U20_PUBLISH_TICKET=Restore.prepare_audio({version=1,bgm={
        path=${JSON.stringify(asset('restored-owner'))},position=0,gain=0.7,looping=true}})
    `)
    const createSource = h.ctx.createBufferSource.getMockImplementation()
    let restored
    h.ctx.createBufferSource.mockImplementationOnce(() => {
      const source = createSource()
      const start = source.start.getMockImplementation()
      source.start.mockImplementation((...args) => {
        start(...args)
        queueMicrotask(() => {
          restored = player.lua.doString('return Restore.apply_audio(__U20_PUBLISH_TICKET)')
          pendingLua.push(restored)
        })
      })
      return source
    })
    expect(await playConfirmed(asset('requested-owner'))).toBe(true)
    expect(await restored).toBe(true)
    expect(h.sources).toHaveLength(2)
    expect(player.audio.captureBgm()).toMatchObject({ path: asset('restored-owner'), gain: 0.7 })
    expect(player.audio.isPlaying('bgm')).toBe(true)
    // Both requests really started. The earlier await continuation must not
    // overwrite the confirmed restored owner's path or gain merely because
    // some source on this bus is still playing.
    expect(player.core.audioBus.bgm).toMatchObject({ path: asset('restored-owner'), volume: 0.7, playing: true })
  })

  it('a newer failed request cannot hide the successfully committed current owner', async () => {
    const h = await boot()
    h.add(asset('previous-confirmed')); h.add(asset('current-confirmed'))
    h.add(asset('newer-missing'), 'fetch-failure')
    expect(await playConfirmed(asset('previous-confirmed'))).toBe(true)
    const previous = h.sources[0]
    const createSource = h.ctx.createBufferSource.getMockImplementation()
    let failedRequest
    h.ctx.createBufferSource.mockImplementationOnce(() => {
      const source = createSource()
      const start = source.start.getMockImplementation()
      source.start.mockImplementation((...args) => {
        start(...args)
        queueMicrotask(() => {
          failedRequest = player.lua.doString(`
            local value,result=backend.audio_play('bgm',${JSON.stringify(asset('newer-missing'))},{fadein=0,loop=true})
            return value==false and result.status=='failed'
          `)
          pendingLua.push(failedRequest)
        })
      })
      return source
    })
    expect(await playConfirmed(asset('current-confirmed'))).toBe(true)
    expect(await failedRequest).toBe(true)
    expect(h.sources).toHaveLength(2)
    expect(previous.stop).toHaveBeenCalledOnce()
    expect(h.sources[1].stop).not.toHaveBeenCalled()
    expect(player.audio.captureBgm().path).toBe(asset('current-confirmed'))
    expect(player.audio.isPlaying('bgm')).toBe(true)
    // The failed request changes request cancellation state, but never changes
    // the committed source. It must not suppress that source's confirmed UI.
    expect(player.core.audioBus.bgm).toMatchObject({ path: asset('current-confirmed'), volume: 0.4, playing: true })
    expect(player.core.events.filter(event => event.kind === 'audio.play' && event.detail.path === asset('newer-missing'))).toHaveLength(0)
  })

  it.each(['fetch-failure', 'decode-failure'])('%s keeps the previous real source and confirmed UI path', async mode => {
    const h = await boot(); h.add(asset('confirmed')); h.add(asset(mode), mode)
    expect(await playConfirmed(asset('confirmed'))).toBe(true)
    const old = h.sources[0]
    expect(await player.lua.doString(`
      local value,result=backend.audio_play('bgm',${JSON.stringify(asset(mode))},{fadein=0,loop=true})
      return value==false and result.status=='failed'
    `)).toBe(true)
    expect.soft(player.core.audioBus.bgm).toMatchObject({ path: asset('confirmed'), playing: true })
    expect(player.audio.captureBgm().path).toBe(asset('confirmed'))
    expect(old.stop).not.toHaveBeenCalled()
    expect(h.sources).toHaveLength(1)
    expect(player.core.events.filter(event => event.kind === 'audio.play' && event.detail.path === asset(mode))).toHaveLength(0)
  })

  it.each(['defer-fetch', 'defer-decode'])('%s cannot announce a pending path or resurrect it after stop', async mode => {
    const h = await boot(); h.add(asset('pending-old')); const gate = h.add(asset(mode), mode)
    expect(await playConfirmed(asset('pending-old'))).toBe(true)
    const old = h.sources[0]
    const request = player.lua.doString(`
      local value,result=backend.audio_play('bgm',${JSON.stringify(asset(mode))},{fadein=0,loop=true})
      return value==false and result.status=='failed'
    `)
    pendingLua.push(request)
    await gate.entered.promise
    expect.soft(player.core.audioBus.bgm).toMatchObject({ path: asset('pending-old'), playing: true })
    expect.soft(old.stop).not.toHaveBeenCalled()
    // Cancellation uses the actual engine while this Lua coroutine awaits its
    // host Promise. Query after settlement uses the existing bridge UI sync.
    player.audio.stop('bgm')
    gate.release.resolve()
    expect(await request).toBe(true)
    expect(h.sources).toHaveLength(1)
    expect(await player.lua.doString("return backend.audio_is_playing('bgm')")).toBe(false)
    expect(player.core.audioBus.bgm?.playing).not.toBe(true)
    expect(player.core.events.filter(event => event.kind === 'audio.play' && event.detail.path === asset(mode))).toHaveLength(0)
  })

  it('natural ended synchronizes confirmed UI state through the existing real playback query', async () => {
    const h = await boot(); h.add(asset('natural'))
    expect(await playConfirmed(asset('natural'), false)).toBe(true)
    expect(player.core.audioBus.bgm).toMatchObject({ path: asset('natural'), playing: true })
    h.advance(14)
    expect(player.audio.isPlaying('bgm')).toBe(false)
    expect(await player.lua.doString("return backend.audio_is_playing('bgm')")).toBe(false)
    expect(player.core.audioBus.bgm.playing).toBe(false)
    expect(h.sources[0].disconnect).toHaveBeenCalledOnce()
  })
})
