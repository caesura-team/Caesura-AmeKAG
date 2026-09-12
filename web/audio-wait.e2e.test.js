// @vitest-environment jsdom
// Actual main.mjs RAF, DOM, Wasmoon, KAG runner and AudioEngine. The only
// simulated audio pieces are the decoder result and AudioContext clock/source.
import { afterAll, beforeAll, expect, it, vi } from 'vitest'
import { existsSync, readFileSync, statSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { installCanvasHost } from './test-support/canvas-host.js'

const here = dirname(fileURLToPath(import.meta.url))
const root = join(here, '..')
const wav = readFileSync(join(root, 'assets/voice/line01.wav'))
const sceneName = 'u20-voice-wait.ks'
const marker = 'U20_VOICE_WAIT_CONTINUED'
const sourceText = `[p]\n[voice_wait]\n[ch text="${marker}"]\n[p]\n[end]`
let context, sources = []
const frames = new Set()
const $ = id => document.getElementById(id)

function makeContext() {
  const ctx = {
    currentTime: 0, state: 'running', destination: {},
    createGain: () => ({ gain: { value: 1 }, connect() {}, disconnect() {} }),
    async decodeAudioData(bytes) {
      expect(Buffer.from(bytes).subarray(0, 4).toString()).toBe('RIFF')
      expect(bytes.byteLength).toBe(wav.length)
      return { duration: 0.25, length: 12000, sampleRate: 48000, numberOfChannels: 1 }
    },
    createBufferSource() {
      const source = { buffer: null, loop: false, ended: false, connect() {}, disconnect() {} }
      source.finish = () => { if (!source.ended) { source.ended = true; source.onended?.() } }
      source.start = vi.fn((when = 0, offset = 0) => { source.startedAt = Math.max(when, ctx.currentTime); source.offset = offset })
      source.stop = vi.fn(() => source.finish())
      sources.push(source)
      return source
    },
    async suspend() { ctx.state = 'suspended' },
    async resume() { ctx.state = 'running' },
    async close() { ctx.state = 'closed' },
  }
  return ctx
}
function advanceAudio(seconds) {
  if (context.state !== 'running') return
  context.currentTime += seconds
  for (const source of sources) {
    if (!source.loop && source.startedAt !== undefined && context.currentTime >= source.startedAt + source.buffer.duration - source.offset) source.finish()
  }
}
async function renderedFrames(count) {
  for (let i = 0; i < count; i++) await new Promise(resolve => requestAnimationFrame(resolve))
}

beforeAll(async () => {
  installCanvasHost()
  localStorage.clear()
  const html = readFileSync(join(here, 'index.html'), 'utf8')
  document.body.innerHTML = html.match(/<body[^>]*>([\s\S]*?)<\/body>/i)[1].replace(/<script\b[^>]*>[\s\S]*?<\/script>/gi, '')
  vi.stubGlobal('FontFace', undefined)
  vi.stubGlobal('AudioContext', function () { context = makeContext(); return context })
  vi.stubGlobal('__CAESURA_WASM_FILE__', join(here, 'node_modules/wasmoon/dist/glue.wasm'))
  vi.stubGlobal('__CAESURA_PROJECT_CAPABILITIES__', JSON.parse(readFileSync(join(root, 'demo/caesura.project.json'), 'utf8')).capabilities)
  vi.stubGlobal('__CAESURA_DEV__', true)
  vi.stubGlobal('fetch', async input => {
    const pathname = new URL(typeof input === 'string' ? input : input.url, 'http://local').pathname
    if (pathname === '/cache/story/story.lua') return new Response('', { status: 404 })
    if (pathname === '/demo/galgame_demo.ks') return new Response('[p]\n[end]')
    if (pathname === '/demo/' + sceneName) return new Response(sourceText)
    if (pathname === '/assets/u20-wait.wav') return new Response(wav)
    const path = pathname === '/scripts/index.json' ? join(here, 'scripts-index.json') : join(root, pathname.slice(1))
    const present = existsSync(path) && statSync(path).isFile()
    return new Response(present ? readFileSync(path) : '', { status: present ? 200 : 404 })
  })
  // Retain actual jsdom RAF timing and callbacks; record IDs only for teardown.
  const nativeRaf = globalThis.requestAnimationFrame.bind(globalThis)
  vi.stubGlobal('requestAnimationFrame', callback => {
    const id = nativeRaf(time => { frames.delete(id); callback(time) })
    frames.add(id)
    return id
  })
  await import('./main.mjs')
  await vi.waitFor(() => expect($('status').textContent).toMatch(/^parked: WAIT:/), { timeout: 15000, interval: 10 })
}, 20000)

afterAll(() => {
  for (const id of frames) cancelAnimationFrame(id)
  frames.clear()
  window.__caesuraAudio?.destroy()
  vi.unstubAllGlobals()
})

it('the real RAF continues voice_wait after audio resumes and ends, without an Advance click', async () => {
  const option = document.createElement('option')
  option.value = sceneName; option.textContent = sceneName
  $('scene').appendChild(option)
  $('scene').value = sceneName
  $('run').click()
  await vi.waitFor(() => expect($('log').textContent).toContain('running ' + sceneName), { timeout: 5000, interval: 10 })
  await vi.waitFor(() => expect($('status').textContent).toMatch(/^parked: WAIT:/), { timeout: 5000, interval: 10 })
  // A host-started voice is a valid voice_wait input. This is the production
  // AudioEngine exposed by main, not a replacement of its playback query.
  expect(await window.__caesuraAudio.play('voice', 'assets/u20-wait.wav', { loop: false, assetUrl: path => 'http://local/' + path })).toBe(true)
  $('advance').click() // Consume only the initial [p], entering voice_wait.
  await vi.waitFor(() => {
    expect(sources).toHaveLength(1)
    expect($('log').textContent).toContain('advance: WAIT')
  }, { timeout: 5000, interval: 10 })
  const advancesBeforeEnd = $('log').textContent.split('\n').filter(line => line.startsWith('advance:')).length
  expect(window.__caesuraAudio.isPlaying('voice')).toBe(true)
  expect($('stage').textContent).not.toContain(marker)
  await context.suspend()
  advanceAudio(120)
  await renderedFrames(3)
  expect(context.currentTime).toBe(0)
  expect(sources[0].ended).toBe(false)
  expect($('stage').textContent).not.toContain(marker)
  await context.resume()
  advanceAudio(0.251)
  await vi.waitFor(() => expect($('stage').textContent).toContain(marker), { timeout: 1000, interval: 10 })
  expect(sources[0].ended).toBe(true)
  expect(sources[0].stop).not.toHaveBeenCalled()
  expect(window.__caesuraAudio.isPlaying('voice')).toBe(false)
  expect($('log').textContent.split('\n').filter(line => line.startsWith('advance:'))).toHaveLength(advancesBeforeEnd)
  expect($('status').textContent).toMatch(/^parked: WAIT:/)
})

it('U20 additional: the actual Auto timer does not skip an active voice_wait', async () => {
  if (!$('scene').querySelector(`option[value="${sceneName}"]`)) {
    const option = document.createElement('option')
    option.value = sceneName; option.textContent = sceneName
    $('scene').appendChild(option)
  }
  $('scene').value = sceneName
  const completedRuns = $('log').textContent.split('\n').filter(line => line.startsWith('result:')).length
  $('run').click()
  await vi.waitFor(() => {
    expect($('log').textContent.split('\n').filter(line => line.startsWith('result:')).length).toBeGreaterThan(completedRuns)
    expect($('status').textContent).toMatch(/^parked: WAIT:/)
  }, { timeout: 5000, interval: 10 })
  expect(await window.__caesuraAudio.play('voice', 'assets/u20-wait.wav', { loop: false, assetUrl: path => 'http://local/' + path })).toBe(true)
  const source = sources.at(-1)
  const beforeEntry = $('log').textContent.split('\n').filter(line => line.startsWith('advance:')).length
  $('advance').click()
  await vi.waitFor(() => expect($('log').textContent.split('\n').filter(line => line.startsWith('advance:')).length).toBeGreaterThan(beforeEntry), { timeout: 1000, interval: 10 })
  expect(window.__caesuraAudio.isPlaying('voice')).toBe(true)
  expect($('stage').textContent).not.toContain(marker)
  const beforeAuto = $('log').textContent.split('\n').filter(line => line.startsWith('advance:')).length
  $('auto').click()
  try {
    // One actual production 1200-ms Auto deadline, plus margin. The audio
    // clock stays held; no fake command, click callback or playing predicate.
    await new Promise(resolve => setTimeout(resolve, 1300))
    expect(source.stop).not.toHaveBeenCalled()
    expect(source.ended).toBe(false)
    expect(window.__caesuraAudio.isPlaying('voice')).toBe(true)
    expect($('stage').textContent).not.toContain(marker)
    expect($('log').textContent.split('\n').filter(line => line.startsWith('advance:'))).toHaveLength(beforeAuto)
  } finally { $('auto').click() }
  advanceAudio(0.251)
  await vi.waitFor(() => expect($('stage').textContent).toContain(marker), { timeout: 1000, interval: 10 })
  expect(source.stop).not.toHaveBeenCalled()
  expect(window.__caesuraAudio.isPlaying('voice')).toBe(false)
})

