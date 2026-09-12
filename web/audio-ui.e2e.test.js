// @vitest-environment jsdom
// Audio UI integration test for the Caesura web player (main.mjs).
// Drives the REAL main.mjs against a real jsdom DOM + a real wasmoon engine,
// using local asset bytes and a controlled AudioContext boundary. The real
// AudioEngine must own playback; this proves UI wiring, not decoding or sound.
// Covers the audio settings UI wiring that the
// unit suites cover only in isolation:
//   * 1. BGM/SE/Voice slider drags -> settings.volumes update -> mirrored
//          slider position stays in sync, three busses independent.
//   * 2. Volume 0 mutes a bus (settings + engine forward); restoring 1 clears.
//   * 3. syncAudioStatus: shows the playing BGM path, and the placeholder
//          when nothing is playing (after the scene reaches [stopbgm]/[stopse]).
//   * 4. Reset walks sliders back to their 1.0 defaults (settings restored).
//   * 5. Scene commands: [playbgm] drives the status display, and a
//          [setbgmvolume] engine mutation does NOT write back into the
//          settings/UI (round 93 one-way lock) -- asserted at the DOM layer.
//
// One page load per file (a single browser session), ordered cases so later
// ones build on the parked scene, exactly like a user would.
import { describe, it, expect, beforeAll } from 'vitest'
import { installCanvasHost } from './test-support/canvas-host.js'
import { readFileSync, existsSync, statSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { dirname, join } from 'node:path'

const here = dirname(fileURLToPath(import.meta.url))
const root = join(here, '..')
const wasmFile = join(here, 'node_modules', 'wasmoon', 'dist', 'glue.wasm')
const AUDIO_TUTORIAL = 'tutorial/tutorial_04_audio.ks'
const NATURAL_END_SCENE = 'u20-natural-voice.ks'
let holdNaturalEnds = false
const EM = String.fromCharCode(0x2014) // — (no-audio placeholder)
beforeAll(()=>{installCanvasHost()})

// Map web/player fetch URLs to real files so the player boots without a
// server (copied 1:1 from e2e.main.test.js to stay in the same harness).
function resolvePathFromUrl(urlStr) {
  let pathname
  try { pathname = new URL(urlStr, 'http://local').pathname } catch { pathname = urlStr }
  if (pathname === '/scripts/index.json') return join(here, 'scripts-index.json')
  for (const dir of ['scripts', 'demo', 'cache', 'assets']) {
    const prefix = '/' + dir + '/'
    if (pathname.startsWith(prefix)) return join(root, dir, pathname.slice(prefix.length))
  }
  return null
}

function makeFetch() {
  return async (input) => {
    const url = typeof input === 'string' ? input : String(input?.url ?? '')
    if (new URL(url, 'http://local').pathname === '/demo/' + NATURAL_END_SCENE) {
      return new Response('[stopbgm fadeout=0]\n[playvoice file="assets/voice/line01.wav"]\n[p]\n[end]')
    }
    const p = resolvePathFromUrl(url)
    if (p && existsSync(p) && statSync(p).isFile()) {
      return new Response(readFileSync(p))
    }
    return { ok: false, status: 404, text: async () => '', json: async () => ({}), arrayBuffer: async () => new ArrayBuffer(0) }
  }
}

function uiAudioContext() {
  const parameter = () => ({
    value: 1,
    setValueAtTime(value) { this.value = value; return this },
    linearRampToValueAtTime(value) { this.value = value; return this },
    cancelScheduledValues() { return this },
    cancelAndHoldAtTime() { return this },
  })
  const context = {
    state: 'running', currentTime: 0, destination: {},
    createGain: () => ({ gain: parameter(), connect() {}, disconnect() {} }),
    async decodeAudioData(bytes) {
      if (!bytes.byteLength) throw new Error('empty audio fixture')
      return { duration: 0.04, length: 1920, sampleRate: 48000, numberOfChannels: 1 }
    },
    createBufferSource() {
      let timer
      return {
        buffer: null, loop: false, onended: null, connect() {}, disconnect() {},
        start() { if (!this.loop && !holdNaturalEnds) timer = setTimeout(() => this.onended?.(), this.buffer.duration * 1000) },
        stop() { clearTimeout(timer); this.onended?.() },
      }
    },
    async resume() { context.state = 'running' },
    async suspend() { context.state = 'suspended' },
    async close() { context.state = 'closed' },
  }
  return context
}

// DOM fixture mirrors web/index.html body (minus the module <script>).
function setupDom() {
  document.head.innerHTML = '<title>caesura-audio-e2e</title>'
  document.body.innerHTML = [
    '<div class="controls">',
    '  <select id="scene"></select>',
    '  <button id="run">▶ Run Scene</button>',
    '  <button id="advance">Click to Advance</button>',
    '  <button id="auto">⏩ Auto</button>',
    '  <span id="status"></span>',
    '</div>',
    '<div class="controls settings-bar">',
    '  <label>Lang <select id="settings-lang"><option value="en">English</option><option value="zh">中文</option><option value="zh-TW">繁體中文</option><option value="ja">日本語</option></select></label>',
    '  <label>Auto <input id="settings-auto" type="checkbox" /></label>',
    '  <label>Speed <input id="settings-speed" type="range" min="1" max="80" value="20" /></label>',
    '  <label>BGM <input id="settings-bgm" type="range" min="0" max="1" step="0.05" value="1" /></label>',
    '  <label>SE <input id="settings-se" type="range" min="0" max="1" step="0.05" value="1" /></label>',
    '  <label>Voice <input id="settings-voice" type="range" min="0" max="1" step="0.05" value="1" /></label>',
    '  <button id="settings-reset" type="button">↺ Reset</button>',
    '</div>',
    '<div id="stage"></div>',
    '<div class="status-line">Audio: <span id="audio-status">—</span></div>',
    '<div class="endings-wrap"><div class="backlog-title">Endings (<span id="endings-count">0</span>)</div><div id="endings"></div></div>',
    '<div class="saves-wrap">',
    '  <div class="backlog-title">Save Slots (<span id="saves-count">0</span>)</div>',
    '  <div class="saves-actions"><input id="save-slot" type="number" min="0" max="99" value="1" /><button id="save-now">💾 Save Current</button><button id="refresh-slots">⟳ Refresh</button></div>',
    '  <div id="saves"></div>',
    '</div>',
    '<div class="backlog-wrap"><div class="backlog-title">Backlog (<span id="backlog-count">0</span>)</div><div id="backlog"></div></div>',
    '<div id="log"></div>',
  ].join(String.fromCharCode(10))
}

async function waitFor(predicate, label, timeout = 15000) {
  const start = Date.now()
  while (Date.now() - start < timeout) {
    const v = predicate()
    if (v) return v
    await new Promise((r) => setTimeout(r, 40))
  }
  throw new Error('waitFor timed out: ' + label)
}

const $ = (id) => document.getElementById(id)
const statusText = () => ($('status') ? $('status').textContent : '')
const audioStatus = () => ($('audio-status') ? $('audio-status').textContent : '')

async function waitStatus(fragment, label, timeout = 60000) {
  return waitFor(() => statusText().includes(fragment), label, timeout)
}

// Read what main.mjs persisted into localStorage (the settings source of truth
// the UI mirrors). Returns parsed settings or null.
function readPersistedSettings() {
  const raw = localStorage.getItem('caesura.player-settings')
  if (!raw) return null
  try { return JSON.parse(raw) } catch { return null }
}

// Set a slider to `value` and dispatch input (the real handler listens for
// 'input'), mirroring how main.mjs coerces the slider's string value.
function setSlider(id, value) {
  const el = $(id)
  el.value = String(value)
  el.dispatchEvent(new Event('input', { bubbles: true }))
  return el
}

function sliderValue(id) { return Number($(id).value) }

// Resolve a scene to the actual <option> value the picker offers. The book
// bundle keys scenes by bare name (e.g. 'tutorial_04_audio.ks'); when the
// bundle is unavailable the fallback list uses 'tutorial/<name>'. Pick
// whichever form exists so the select actually selects it.
function pickSceneValue(name) {
  const sel = $('scene')
  const bare = name.split('/').pop()
  for (const opt of sel.options) {
    if (opt.value === name || opt.value === bare) return opt.value
  }
  // Neither form is an option (defensive): keep a stable value by appending one.
  return 'tutorial_04_audio.ks'
}

// Pick a scene and Run it, waiting for it to park on a [p] (WAIT:).
async function runScene(name) {
  const sel = $('scene')
  sel.value = pickSceneValue(name)
  sel.dispatchEvent(new Event('change', { bubbles: true }))
  $('run').click()
  return waitStatus('parked: ', 'run parks: ' + name)
}

// Every completed click writes a log entry, including [ch] -> explicit [p]
// boundaries that keep the same display-token status and visible page.
// An audio boundary finishes later through RAF and publishes "parked:";
// require that settled page as well as this click's successful receipt.
async function advanceOnce() {
  const before = $('log').textContent.split('\n').filter(line => line.startsWith('advance: ')).length
  $('advance').click()
  return waitFor(() => {
    const completed = $('log').textContent.split('\n').filter(line => line.startsWith('advance: '))
    return completed.length > before && /^advance: (WAIT:|WAIT_AUDIO:|DONE:)/.test(completed.at(-1))
      && /^(advance|parked): (WAIT:|DONE:)/.test(statusText())
  }, 'advance completes its input boundary', 60000)
}

beforeAll(async () => {
  globalThis.AudioContext = function () { return uiAudioContext() }
  globalThis.__CAESURA_WASM_FILE__ = wasmFile
  globalThis.__CAESURA_PROJECT_CAPABILITIES__ = JSON.parse(readFileSync(join(root, 'demo/caesura.project.json'), 'utf8')).capabilities
  // CI checkouts never carry cache/story/story.lua (gitignored), so the
  // boot takes the bundle-missing path; the demo fallback is DEV_MODE-gated
  // (b6bfcd98 refuses to fake demo content in production). These suites drive
  // the RAW demo flow, so opt into dev mode explicitly.
  globalThis.__CAESURA_DEV__ = true
  setupDom()
  globalThis.fetch = makeFetch()
  if (typeof globalThis.requestAnimationFrame !== 'function') {
    globalThis.requestAnimationFrame = (cb) => setTimeout(() => cb(Date.now()), 16)
  }
  await import('./main.mjs')
  // The module auto-runs galgame_demo.ks; wait for it to park on a [p].
  await waitFor(() => /^parked: /i.test(statusText()), 'initial scene parks', 150000)
  // Anchor sliders at defaults so the per-bus tests start from a known point.
  setSlider('settings-bgm', 1)
  setSlider('settings-se', 1)
  setSlider('settings-voice', 1)
}, 180000)

describe('audio UI · volume sliders (integration with settings + engine wiring)', () => {
  it('drags each slider independently: settings.volumes persists + mirror stays in sync', () => {
    setSlider('settings-bgm', 0.4)
    setSlider('settings-se', 0.6)
    setSlider('settings-voice', 0.8)

    const vols = readPersistedSettings().volumes
    expect(vols.bgm).toBe(0.4)
    expect(vols.se).toBe(0.6)
    expect(vols.voice).toBe(0.8)

    // The settings mirror (syncSettingsControls) keeps DOM sliders in sync
    // with the persisted, sanitized value.
    expect(sliderValue('settings-bgm')).toBe(0.4)
    expect(sliderValue('settings-se')).toBe(0.6)
    expect(sliderValue('settings-voice')).toBe(0.8)

    // Independent again: only bgm changes.
    setSlider('settings-bgm', 0.25)
    const vols2 = readPersistedSettings().volumes
    expect(vols2.bgm).toBe(0.25)
    expect(vols2.se).toBe(0.6)
    expect(vols2.voice).toBe(0.8)
    expect(sliderValue('settings-bgm')).toBe(0.25)
  })

  it('muting a bus (volume 0) persists + is restored; other buses unaffected', () => {
    setSlider('settings-se', 0)
    let vols = readPersistedSettings().volumes
    expect(vols.se).toBe(0)
    expect(sliderValue('settings-se')).toBe(0)
    expect(vols.bgm).toBe(0.25)
    expect(vols.voice).toBe(0.8)

    setSlider('settings-se', 1)
    vols = readPersistedSettings().volumes
    expect(vols.se).toBe(1)
    expect(sliderValue('settings-se')).toBe(1)
  })
})

describe('audio UI · reset restores the default 1.0 slider positions', () => {
  it('Reset after manual drags returns all busses to 1.0', () => {
    setSlider('settings-bgm', 0.3)
    setSlider('settings-se', 0.3)
    setSlider('settings-voice', 0.3)
    expect(sliderValue('settings-bgm')).toBe(0.3)

    $('settings-reset').click()

    // The reset notifies subscribers -> syncSettingsControls mirrors the
    // defaults back into the DOM sliders (the observable UI contract).
    expect(sliderValue('settings-bgm')).toBe(1)
    expect(sliderValue('settings-se')).toBe(1)
    expect(sliderValue('settings-voice')).toBe(1)
  })
})

describe('audio UI · scene audio status display + one-way lock', () => {
  it('runs the audio tutorial: [playbgm] shows the playing path, [setbgmvolume] does NOT write back to the slider', async () => {
    await runScene(AUDIO_TUTORIAL)
    await waitFor(() => audioStatus().includes('BGM'), 'audio status shows BGM', 15000)
    expect(audioStatus()).toContain('BGM: daily.wav')
    expect(window.__caesuraAudio.isPlaying('bgm')).toBe(true)

    // Anchor the settings/UI at 0.7 (a user pick). [setbgmvolume] happens on
    // the NEXT page and is engine-side only (round 93 one-way lock): it must
    // NOT echo back into the slider or settings.volumes.
    setSlider('settings-bgm', 0.7)
    expect(sliderValue('settings-bgm')).toBe(0.7)

    // Complete [ch] and its explicit [p] before page 2's volume command.
    const before = window.__caesuraCore.events.length
    await advanceOnce()
    await advanceOnce()
    expect(statusText()).toMatch(/^advance: (WAIT:|DONE:)/)
    expect(window.__caesuraCore.events.slice(before).some(event => event.kind === 'audio.volume'
      && event.detail.kind === 'bgm' && event.detail.v === 0.3)).toBe(true)

    expect(sliderValue('settings-bgm')).toBe(0.7)
    expect(readPersistedSettings().volumes.bgm).toBe(0.7)
  }, 120000)

  it('[stopbgm]/[stopse] drop those buses from the status display', async () => {
    // Advance until the scene clears every bus. The tutorial's final audio
    // pages run [playse]/voice then [stopse] + [stopbgm]; after that all
    // buses report not-playing and syncAudioStatus falls back to the "—"
    // placeholder. Advance repeatedly (bounded) so the count is robust to
    // exactly where the previous test left the parked cursor.
    for (let i = 0; i < 10; i++) {
      if (audioStatus() === EM) break
      await advanceOnce()
      if (audioStatus() !== EM) await advanceOnce()
    }
    await waitFor(() => audioStatus() === EM, 'status reaches the no-audio placeholder', 15000)
    expect(audioStatus()).toBe(EM)
    expect(['bgm', 'se', 'voice'].some(bus => window.__caesuraAudio.isPlaying(bus))).toBe(false)
  }, 120000)

  it('natural source completion updates the parked UI without an explicit playback query', async () => {
    const option = document.createElement('option')
    option.value = NATURAL_END_SCENE
    option.textContent = NATURAL_END_SCENE
    $('scene').appendChild(option)
    holdNaturalEnds = true
    try {
      await runScene(NATURAL_END_SCENE)
      await waitFor(() => audioStatus().includes('VOICE:'), 'voice source is displayed')
      const source = window.__caesuraAudio._sources.get('voice').source
      source.onended()
      // Only the normal requestAnimationFrame UI path may observe completion.
      await waitFor(() => audioStatus() === EM, 'natural completion clears parked audio status', 2000)
      expect(window.__caesuraCore.audioBus.voice.playing).toBe(false)
    } finally {
      holdNaturalEnds = false
    }
  }, 20000)
})
