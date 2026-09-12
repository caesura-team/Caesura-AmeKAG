// @vitest-environment node
// U20: actual Wasmoon, scene runner, KAG commands, bridge and AudioEngine.
// Only encoded asset delivery, decoder completion and the WebAudio clock/source
// boundary are controlled. These are lifecycle tests, not rendered PCM evidence.
import { afterEach, describe, expect, it, vi } from 'vitest'
import { existsSync, readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { createPlayer } from './bridge.js'

const here = dirname(fileURLToPath(import.meta.url))
const wav = readFileSync(join(here, '../assets/voice/line01.wav'))
const asset = 'assets/u20-wait.wav'
const duration = 0.25
const deferred = () => {
  let resolve
  const promise = new Promise(yes => { resolve = yes })
  return { promise, resolve }
}
let player, host
const pending = []

afterEach(async () => {
  host?.decodeRelease.resolve()
  await Promise.allSettled(pending.splice(0))
  if (player) expect(await player.dispose()).toBe(true)
  player = null; host = null
  vi.unstubAllGlobals()
})

describe('U20 additional audio wait review', () => {
  it('audio-clock progress during asynchronous host publication belongs to the next wait tick', async () => {
    const h = await boot()
    // Enter directly, avoiding any manual click budget in this separate case.
    const input = `[playbgm file="${asset}" loop=true]\n[waitbgm]\n[set var="f.after_audio" value=1]\n[p]\n[set var="f.after_page" value=1]\n[end]`
    expect(await player.runScene(input, 'clock-publication.ks', { autoClick: false, maxFrames: 64 })).toMatch(/^WAIT_AUDIO:/)
    expect(h.sources[0].loop).toBe(true)
    let clock = 59.9, firstRead = true
    Object.defineProperty(h.context, 'currentTime', {
      configurable: true,
      get() {
        const value = clock
        if (firstRead) {
          firstRead = false
          // The audio device continues while the real async JS/Lua publication
          // awaits. Only advance this host clock; do not wrap production code.
          queueMicrotask(() => { clock = 60.1 })
        }
        return value
      },
      set(value) { clock = value },
    })
    await tick()
    expect(await flags()).toMatchObject({ after: false, beyond: false })
    expect(clock).toBe(60.1)
    await nextPage()
    expect(player.audio.isPlaying('bgm')).toBe(true)
  })

  it('a manual page advance still skips voice_wait exactly once', async () => {
    await boot(); const source = await start('voice_wait')
    await held()
    expect(await player.runScene('', 'u20-wait.ks', { advance: true, advanceScene: 'u20-wait.ks', autoClick: false })).toMatch(/^WAIT:/)
    expect(source.stop).toHaveBeenCalledOnce()
    expect(player.audio.isPlaying('voice')).toBe(false)
    expect(await flags()).toEqual({ after: true, beyond: false, page: true })
    await tick()
    expect(await flags()).toEqual({ after: true, beyond: false, page: true })
  })

  it.each(['waitbgm', 'waitsound'])('%s spends its 60-second protection on the audio clock, never repeated ticks or wall time', async command => {
    const h = await boot(); const source = await start(command, true)
    h.advance(59.9)
    // Concurrent frame requests must not count one audio interval twice.
    await Promise.all([player.tickAudio(), player.tickAudio()])
    expect(await flags()).toMatchObject({ after: false, beyond: false })
    await h.context.suspend()
    vi.useFakeTimers({ toFake: ['Date', 'performance'] })
    try {
      const wallBefore = Date.now(), monotonicBefore = performance.now()
      vi.advanceTimersByTime(120000)
      expect(Date.now() - wallBefore).toBe(120000)
      expect(performance.now() - monotonicBefore).toBe(120000)
      h.advance(120)
      expect(h.context.currentTime).toBe(59.9)
      await held()
    } finally { vi.useRealTimers() }
    expect(await player.audio.unlock()).toBe(true)
    h.advance(0.099)
    await held()
    expect(h.context.currentTime).toBeCloseTo(59.999, 6)
    h.advance(0.002)
    await nextPage()
    // The guard releases only the wait; it does not pretend the loop ended.
    expect(source.stop).not.toHaveBeenCalled()
    expect(source.ended).toBe(false)
    expect(player.audio.isPlaying(busFor(command))).toBe(true)
  })

  it('a decoder finishing after a replacement scene exists cannot resurrect the old audio or advance the new page', async () => {
    const h = await boot(); const old = await start('voice_wait')
    await tick()
    const entered = deferred()
    h.context.decodeAudioData.mockImplementationOnce(async bytes => {
      expect(Buffer.from(bytes).subarray(0, 4).toString()).toBe('RIFF')
      expect(bytes.byteLength).toBe(wav.length)
      entered.resolve()
      await h.decodeRelease.promise
      return { duration, length: 12000, sampleRate: 48000, numberOfChannels: 1 }
    })
    // A different path avoids the successful clip's decoded-buffer cache.
    const late = player.lua.doString("return backend.audio_play('voice','assets/voice/line01.wav',{loop=false})")
    pending.push(late)
    await entered.promise
    expect(h.sources).toHaveLength(1)
    expect(await player.runScene('[set var="f.new_owner" value=23]\n[p]\n[set var="f.leaked" value=1]\n[end]', 'new-before-decode.ks', { autoClick: false })).toMatch(/^WAIT:/)
    expect(old.stop).toHaveBeenCalledOnce()
    expect(await player.lua.doString("local c=require('kag_runner').get_ctx();return c.f.new_owner==23 and c.f.leaked==nil")).toBe(true)
    h.decodeRelease.resolve()
    expect(await late).toBe(false)
    h.advance(1)
    await tick()
    expect(h.sources).toHaveLength(1)
    expect(player.audio.isPlaying('voice')).toBe(false)
    expect(await player.lua.doString("local c=require('kag_runner').get_ctx();return c.f.new_owner==23 and c.f.leaked==nil and c.f.after_audio==nil")).toBe(true)
  })
})

function audioHost({ gated = false } = {}) {
  const sources = [], decodeEntered = deferred(), decodeRelease = deferred()
  const context = { state: 'running', currentTime: 0, destination: {} }
  context.createGain = () => ({ gain: { value: 1 }, connect() {}, disconnect() {} })
  context.decodeAudioData = vi.fn(async bytes => {
    // A real, nonempty WAV reaches the actual AudioEngine decode boundary.
    expect(Buffer.from(bytes).subarray(0, 4).toString()).toBe('RIFF')
    expect(bytes.byteLength).toBe(wav.length)
    decodeEntered.resolve()
    if (gated) await decodeRelease.promise
    return { duration, length: 12000, sampleRate: 48000, numberOfChannels: 1 }
  })
  context.createBufferSource = () => {
    const source = { buffer: null, loop: false, ended: false, connect() {}, disconnect: vi.fn() }
    source.finish = () => { if (!source.ended) { source.ended = true; source.onended?.() } }
    source.start = vi.fn((when = 0, offset = 0) => {
      source.startedAt = Math.max(when, context.currentTime); source.offset = offset
    })
    source.stop = vi.fn((when = 0) => {
      source.stopAt = Math.max(when, context.currentTime)
      if (source.stopAt <= context.currentTime) source.finish()
    })
    sources.push(source)
    return source
  }
  context.resume = vi.fn(async () => { context.state = 'running' })
  context.suspend = vi.fn(async () => { context.state = 'suspended' })
  context.close = vi.fn(async () => { context.state = 'closed' })
  function advance(seconds) {
    if (context.state !== 'running') return
    context.currentTime += seconds
    for (const source of sources) {
      if (source.startedAt === undefined || source.ended) continue
      const end = source.loop ? Infinity : source.startedAt + source.buffer.duration - source.offset
      if (context.currentTime >= Math.min(end, source.stopAt ?? Infinity)) source.finish()
    }
  }
  async function fetchImpl(url) {
    const pathname = new URL(url).pathname
    if (pathname === '/' + asset) return new Response(wav)
    const path = pathname === '/scripts/index.json'
      ? join(here, 'scripts-index.json') : join(here, '..', pathname.slice(1))
    return new Response(existsSync(path) ? readFileSync(path) : '', { status: existsSync(path) ? 200 : 404 })
  }
  return { context, sources, advance, fetchImpl, decodeEntered, decodeRelease }
}

async function boot(options) {
  host = audioHost(options)
  vi.stubGlobal('FontFace', undefined)
  player = await createPlayer({
    scriptsBase: 'http://local/scripts/', langBase: 'http://local/assets/lang/',
    wasmFile: join(here, 'node_modules/wasmoon/dist/glue.wasm'),
    audioContext: host.context, audioAssetUrl: path => 'http://local/' + path,
    fetchImpl: host.fetchImpl,
  })
  return host
}

const busFor = command => command === 'waitbgm' ? 'bgm' : command === 'waitsound' ? 'se' : 'voice'
function scene(command) {
  const play = command === 'playvoice' ? `[playvoice file="${asset}"]` : `[${command}]`
  return `${play}\n[set var="f.after_audio" value=1]\n[p]\n[set var="f.after_page" value=1]\n[end]`
}
async function flags() {
  return player.lua.doString(`
    local c=require('kag_runner').get_ctx()
    return {after=c.f.after_audio==1, beyond=c.f.after_page==1, page=c.waiting_input==true}
  `)
}
async function start(command, loop = false) {
  let result
  if (command === 'playvoice') {
    result = await player.runScene(scene(command), 'u20-wait.ks', { autoClick: false, maxFrames: 4000 })
  } else {
    // Explicit waits consume already started audio. Use an actual first [p],
    // then the public Lua backend and one real page advance to enter the wait.
    // [iscript] deliberately cannot reach the backend or require globals.
    expect(await player.runScene('[p]\n' + scene(command), 'u20-wait.ks', { autoClick: false })).toMatch(/^WAIT:/)
    expect(await player.lua.doString(`return backend.audio_play(${JSON.stringify(busFor(command))},${JSON.stringify(asset)},{loop=${loop}})~=false`)).toBe(true)
    result = await player.runScene('', 'u20-wait.ks', { advance: true, advanceScene: 'u20-wait.ks', autoClick: false, maxFrames: 4000 })
  }
  expect(host.sources).toHaveLength(1)
  expect(host.sources[0].start).toHaveBeenCalledOnce()
  expect(player.audio.isPlaying(busFor(command))).toBe(true)
  // Check story semantics before the protocol label: an exhausted virtual
  // 60-second budget must not execute the next tag while real audio is held.
  expect(await flags(), `${command}: the host must not run past active audio`).toMatchObject({ after: false, beyond: false })
  expect(result).toMatch(/^WAIT_AUDIO:/)
  return host.sources[0]
}
async function tick() {
  expect(player.tickAudio, 'the Web host needs a non-click frame continuation for parked audio').toBeTypeOf('function')
  return player.tickAudio()
}
async function held() {
  for (let i = 0; i < 4; i++) await tick()
  expect(await flags()).toMatchObject({ after: false, beyond: false })
}
async function nextPage() {
  await tick()
  expect(await flags()).toEqual({ after: true, beyond: false, page: true })
  for (let i = 0; i < 4; i++) await tick()
  expect(await flags()).toEqual({ after: true, beyond: false, page: true })
}

describe('U20 actual Web audio waits', () => {
  it.each(['playvoice', 'voice_wait', 'waitsound', 'waitbgm'])('%s holds before the audio deadline and naturally reaches only the next page', async command => {
    const h = await boot(); const source = await start(command)
    await held()
    h.advance(duration - 1 / 48000)
    await held()
    expect(source.stop).not.toHaveBeenCalled()
    h.advance(2 / 48000)
    await nextPage()
    expect(source.ended).toBe(true)
    expect(source.stop).not.toHaveBeenCalled()
    expect(player.audio.isPlaying(busFor(command))).toBe(false)
  })

  it.each(['playvoice', 'voice_wait'])('%s is released by a real backend stop, without a click', async command => {
    await boot(); const source = await start(command)
    await held()
    expect(await player.lua.doString("return backend.audio_stop('voice')~=false")).toBe(true)
    await nextPage()
    expect(source.stop).toHaveBeenCalledOnce()
  })

  it.each(['voice_wait', 'waitsound'])('%s does not consume an audio deadline while suspended', async command => {
    const h = await boot(); const source = await start(command)
    h.advance(0.1)
    await h.context.suspend()
    h.advance(120) // Wall time may pass; suspended AudioContext time does not.
    expect(h.context.currentTime).toBe(0.1)
    await held()
    expect(source.ended).toBe(false)
    expect(player.audio.isPlaying(busFor(command))).toBe(true)
    expect(await player.audio.unlock()).toBe(true)
    h.advance(0.151)
    await nextPage()
  })

  it.each(['waitbgm', 'waitsound'])('%s remains held across several real audio loop periods until stop', async command => {
    const h = await boot(); const source = await start(command, true)
    for (let period = 0; period < 4; period++) { h.advance(duration); await held() }
    expect(source.loop).toBe(true)
    expect(source.ended).toBe(false)
    expect(source.stop).not.toHaveBeenCalled()
    expect(await player.lua.doString(`return backend.audio_stop(${JSON.stringify(busFor(command))})~=false`)).toBe(true)
    await nextPage()
    expect(source.stop).toHaveBeenCalledOnce()
  })

  it('actual decoder completion is awaited before playvoice can publish an audio wait', async () => {
    const h = await boot({ gated: true })
    let settled = false
    const run = player.runScene(scene('playvoice'), 'u20-decode.ks', { autoClick: false, maxFrames: 4000 }).finally(() => { settled = true })
    pending.push(run)
    await h.decodeEntered.promise
    expect(settled).toBe(false)
    expect(h.sources).toHaveLength(0)
    h.decodeRelease.resolve()
    expect(await run).toMatch(/^WAIT_AUDIO:/)
    expect(await flags()).toMatchObject({ after: false, beyond: false })
    expect(h.sources[0].start).toHaveBeenCalledOnce()
    h.advance(duration)
    await nextPage()
  })

  it('a parked audio tick does not lock out scene replacement or later advance the replacement page', async () => {
    const h = await boot(); const old = await start('voice_wait')
    await tick()
    expect(await player.runScene('[set var="f.new_owner" value=7]\n[p]\n[set var="f.leaked" value=1]\n[end]', 'replacement.ks', { autoClick: false })).toMatch(/^WAIT:/)
    h.advance(duration * 4)
    old.onended?.() // Late browser callback still belongs to its original source.
    await tick()
    expect(await player.lua.doString(`local c=require('kag_runner').get_ctx(); return c.f.new_owner==7 and c.f.leaked==nil and c.f.after_audio==nil`)).toBe(true)
  })

  it('a cancelled late decoder cannot start audio or advance a subsequently created scene', async () => {
    const h = await boot({ gated: true })
    const run = player.runScene(scene('playvoice'), 'cancelled-decode.ks', { autoClick: false, maxFrames: 4000 })
    pending.push(run)
    await h.decodeEntered.promise
    expect(await player.lua.doString("return backend.audio_stop('voice')~=false")).toBe(true)
    h.decodeRelease.resolve()
    expect(await run).toMatch(/^WAIT:/)
    expect(h.sources).toHaveLength(0)
    expect(await player.runScene('[set var="f.new_owner" value=9]\n[p]\n[set var="f.leaked" value=1]\n[end]', 'after-cancel.ks', { autoClick: false })).toMatch(/^WAIT:/)
    h.advance(duration * 8)
    await tick()
    expect(await player.lua.doString(`local c=require('kag_runner').get_ctx(); return c.f.new_owner==9 and c.f.leaked==nil`)).toBe(true)
    expect(h.sources).toHaveLength(0)
  })
})
