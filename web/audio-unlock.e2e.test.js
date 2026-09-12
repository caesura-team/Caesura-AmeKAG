// @vitest-environment jsdom
// Plan W1 — WebAudio autoplay lifecycle, DOM-level e2e for the REAL main.mjs.
//
// Drives main.mjs (which owns the W1 wiring: document-level gesture unlock +
// visibilitychange resume) against jsdom + wasmoon, with a FAKE AudioContext
// whose state starts 'suspended' and whose resume() flips it to 'running'.
// jsdom never grants trusted user gestures, so:
//   * before any dispatched input, the context stays 'suspended' even though
//     the auto-started scene already called play() — the real-browser
//     autoplay-policy symptom;
//   * dispatching pointerdown fires main.mjs's capture listener -> unlock();
//   * returning from 'hidden' fires the visibilitychange listener -> resume
//     again after a re-suspend (tab background recovery).
// This context controls autoplay state only; its decoder is a stub and does
// not prove successful playback. scripts/web_audio_smoke.mjs separately checks
// real decoding, source ownership and PCM with Chromium WebAudio.
import { describe, it, expect, beforeAll, vi } from 'vitest'
import { readFileSync, existsSync, statSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { dirname, join } from 'node:path'

const here = dirname(fileURLToPath(import.meta.url))
const root = join(here, '..')
const wasmFile = join(here, 'node_modules', 'wasmoon', 'dist', 'glue.wasm')

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
    const p = resolvePathFromUrl(url)
    if (p && existsSync(p) && statSync(p).isFile()) {
      const text = () => readFileSync(p, 'utf8')
      return {
        ok: true, status: 200, text,
        json: async () => JSON.parse(text()),
        arrayBuffer: async () => readFileSync(p).buffer,
      }
    }
    return { ok: false, status: 404, text: async () => '', json: async () => ({}), arrayBuffer: async () => new ArrayBuffer(0) }
  }
}

// ---- fake AudioContext (suspended until resume) ----
const fakeAudio = { resumeCalls: 0 }
function makeFakeAudioContext() {
  const ctx = {
    currentTime: 0,
    state: 'suspended',
    destination: { kind: 'dest' },
    createGain: () => ({ gain: { value: 1 }, connect: vi.fn(), disconnect: vi.fn() }),
    createBufferSource: () => ({
      buffer: null, loop: false, connect: vi.fn(), start: vi.fn(), stop: vi.fn(), disconnect: vi.fn(),
    }),
    decodeAudioData: vi.fn(async () => ({ duration: 5 })),
    suspend: vi.fn(async () => {}),
    resume: vi.fn(async () => { fakeAudio.resumeCalls++; ctx.state = 'running' }),
    close: vi.fn(async () => {}),
  }
  return ctx
}
let activeCtx = null

function setupDom() {
  document.head.innerHTML = '<title>caesura-audio-unlock</title>'
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

async function waitFor(predicate, label, timeout = 150000) {
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

beforeAll(async () => {
  // Pin the auto-started scene to the audio tutorial: the W1 contract needs
  // a scene that deterministically issues [playbgm]/[playse]/[playvoice] at
  // boot (the bundle's scene ordering is not part of the contract).
  try { window.history.replaceState(null, '', '?scene=tutorial/tutorial_04_audio.ks') } catch { /* jsdom fallback */ }
  globalThis.__CAESURA_WASM_FILE__ = wasmFile
  // CI checkouts never carry cache/story/story.lua (gitignored), so the boot
  // takes the bundle-missing path; the demo fallback is DEV_MODE-gated
  // (b6bfcd98 refuses to fake demo content in production). These suites drive
  // the RAW demo flow, so opt into dev mode explicitly.
  globalThis.__CAESURA_DEV__ = true
  globalThis.AudioContext = function () { activeCtx = makeFakeAudioContext(); return activeCtx }
  setupDom()
  globalThis.fetch = makeFetch()
  if (typeof globalThis.requestAnimationFrame !== 'function') {
    globalThis.requestAnimationFrame = (cb) => setTimeout(() => cb(Date.now()), 16)
  }
  await import('./main.mjs')
  await waitFor(() => /^parked: /i.test(statusText()), 'initial scene parks')
}, 200000)

describe('W1 WebAudio autoplay lifecycle (real main.mjs)', () => {
  it('the auto-started scene left the context suspended (no gesture yet)', async () => {
    expect(window.__caesuraAudio).toBeTruthy()
    // [playbgm] creates the context asynchronously after the scene starts;
    // wait for it, then assert it is still 'suspended' (no gesture yet).
    await waitFor(() => window.__caesuraAudio.state !== 'none', 'audio ctx created', 20000)
    expect(window.__caesuraAudio.state).toBe('suspended')
    expect(fakeAudio.resumeCalls).toBe(0)
  })

  it('a user gesture (pointerdown) unlocks: suspended -> running, resume once', async () => {
    document.dispatchEvent(new Event('pointerdown', { bubbles: true, cancelable: true }))
    await waitFor(() => window.__caesuraAudio.state === 'running', 'state running after gesture', 5000)
    expect(fakeAudio.resumeCalls).toBe(1)
  })

  it('repeated gestures do not re-resume a running context', async () => {
    document.dispatchEvent(new Event('pointerdown', { bubbles: true, cancelable: true }))
    document.dispatchEvent(new Event('keydown', { bubbles: true, cancelable: true, key: 'Enter' }))
    await new Promise((r) => setTimeout(r, 300))
    expect(window.__caesuraAudio.state).toBe('running')
    expect(fakeAudio.resumeCalls).toBe(1)
  })

  it('background (hidden) + re-suspend + visibilitychange returns -> resume again', async () => {
    // simulate the browser/OS re-suspending the context while in background
    activeCtx.state = 'suspended'
    Object.defineProperty(document, 'visibilityState', { value: 'hidden', configurable: true })
    document.dispatchEvent(new Event('visibilitychange'))
    await new Promise((r) => setTimeout(r, 200))
    expect(window.__caesuraAudio.state).toBe('suspended')
    expect(fakeAudio.resumeCalls).toBe(1)
    // tab returns to foreground
    Object.defineProperty(document, 'visibilityState', { value: 'visible', configurable: true })
    document.dispatchEvent(new Event('visibilitychange'))
    await waitFor(() => window.__caesuraAudio.state === 'running', 'state running after tab return', 5000)
    expect(fakeAudio.resumeCalls).toBe(2)
  })
})
