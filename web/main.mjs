// Caesura Web Player entry — loads scripts via fetch, runs scenes in
// wasmoon. Prefers the pre-baked story bundle (ks_bake --web) for
// zero-parse scene starts; falls back to raw .ks sources.
import { createPlayer } from './bridge.js'
import { DomRenderer } from './dom-renderer.js'
import { buildSceneOptions } from './scene-options.js'
import { createPlayerSettings, VOLUME_BUSSES } from './player-settings.js'
import { TouchGestureDetector } from './touch-gestures.js'

const stage = document.getElementById('stage')
const logEl = document.getElementById('log')
const statusEl = document.getElementById('status')
const endingsEl = document.getElementById('endings')
const endingsCount = document.getElementById('endings-count')
const audioStatusEl = document.getElementById('audio-status')
const savesEl = document.getElementById('saves')
const savesCount = document.getElementById('saves-count')
const log = (s) => { logEl.textContent += s + '\n'; logEl.scrollTop = logEl.scrollHeight }

// W7: a packaged player may be served from a subpath (GitHub Pages / itch.io
// / Netlify). Resolve runtime bases RELATIVE to the page URL instead of
// hard-coding '/...' so the same dist works at any mount point.
const baseHref = String(document.baseURI || location.href).split(/[?#]/)[0].replace(/[^/]*$/, '')
const SCRIPTS_BASE = baseHref + 'scripts/'
const DEMO_BASE = baseHref + 'demo/'
const STORY_BASE = baseHref + 'cache/story/story.lua'
const ASSET_BASE = baseHref + 'assets/'

// wasmFile stays undefined in production (wasmoon fetches its CDN
// default); a test host may pin a local copy via self.__CAESURA_WASM_FILE__
// so fake-DOM runs can load the engine offline without changing browser
// behavior (behavior unchanged when the global is unset).
const wasmFile = (typeof self !== 'undefined' && self.__CAESURA_WASM_FILE__)
  ? self.__CAESURA_WASM_FILE__
  : undefined
const projectCapabilities = typeof self !== 'undefined' ? self.__CAESURA_PROJECT_CAPABILITIES__ : undefined
const player = await createPlayer({ scriptsBase: SCRIPTS_BASE, capabilities: projectCapabilities,
  ...(wasmFile ? { wasmFile } : {}) })
log('engine loaded; kag table ready')

// ---- WebAudio lifecycle & Mobile orientation lock (plan W1/R3) -------
// Autoplay policy: a fresh AudioContext stays 'suspended' until a trusted
// user gesture. Capture pointerdown/keydown/touchstart (document-level,
// covers the Run/Advance buttons + canvas taps) to unlock — idempotent,
// so every later gesture is a safe no-op. Returning to the tab after a
// browser/OS suspend resumes it again.
const requestOrientationLock = () => {
  try {
    if (typeof screen !== 'undefined' && screen.orientation && typeof screen.orientation.lock === 'function') {
      screen.orientation.lock('landscape').catch(() => {})
    }
  } catch { /* orientation lock restricted or unsupported */ }
}
const unlockAudio = () => {
  void player.audio.unlock()
  requestOrientationLock()
}
for (const type of ['pointerdown', 'keydown', 'touchstart']) {
  document.addEventListener(type, unlockAudio, { capture: true, passive: true })
}
document.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'visible') unlockAudio()
})
// Verification hook for the real-browser smoke (W1 asserts the context
// state before/after a trusted click); also the live audio debug surface.
window.__caesuraAudio = player.audio

// W4 verification hooks (smoke --stress): texture/dem leak probes + a
// page-level error net (uncaught errors / rejected promises, incl. WASM
// runtime failures) so the stress run can tell "stable" from "swallowed".
window.__caesuraCore = player.core
window.__caesuraErrors = []
window.addEventListener('error', (e) => { window.__caesuraErrors.push(String(e.message || e.type)) })
window.addEventListener('unhandledrejection', (e) => {
  const r = e.reason || {}
  const stack = String(r && r.stack || '')
  window.__caesuraErrors.push('rejection: ' + String(r && r.message || r).slice(0, 160) + (stack ? ' @ ' + stack.split(String.fromCharCode(10)).slice(0, 3).join(' | ') : ''))
})

const renderer = new DomRenderer(player.core, stage)
let storyBundle = null
// Non-null when the story bundle failed to load: the reason string. Read by
// the boot guard below (and by any host inspecting the failure) so a broken
// package can never be mistaken for a working one.
let storyBundleError = null

// ---- player settings (round 86) -------------------------------------
// Centralized, persistent settings (language / auto / text speed / skip /
// per-bus volume). Reading here so the player boots with saved preferences
// and any UI write propagates to the engine (i18n + audio buses) live.
const settings = createPlayerSettings()
settings.load()
// Push the persisted volume / language into the engine at boot.
applyVolumes(settings.get('volumes'))
void applyLanguage(settings.get('language'))

/**
 * Mirror the current settings controller state back into the settings UI
 * controls wherever they actually mirror a persisted field. Safe against
 * echo loops: assigning el.value / el.checked programmatically does NOT
 * fire change/input events, so this never re-triggers a settings set.
 * Called after load, on any settings notification (so an external write
 * such as a reset or a programmatic set updates the sliders/checkbox), and
 * after reset. Text-speed/volume sliders show the sanitized in-range value.
 */
function syncSettingsControls() {
  const langEl = document.getElementById('settings-lang')
  const autoEl = document.getElementById('settings-auto')
  const speedEl = document.getElementById('settings-speed')
  const bgmEl = document.getElementById('settings-bgm')
  const seEl = document.getElementById('settings-se')
  const voiceEl = document.getElementById('settings-voice')
  if (!langEl) return // settings UI absent (headless) — nothing to mirror
  langEl.value = settings.get('language') in { en: 1, zh: 1, 'zh-TW': 1, ja: 1 } ? settings.get('language') : 'en'
  autoEl.checked = settings.get('autoClick')
  speedEl.value = String(settings.get('textSpeed'))
  bgmEl.value = String(settings.get('volumes').bgm)
  seEl.value = String(settings.get('volumes').se)
  voiceEl.value = String(settings.get('volumes').voice)
}

// Keep the engine (volume buses + i18n) live behind any later settings
// change, and keep the settings UI mirrors in sync (bidirectional, no cycle:
// control writes -> settings.set -> notify -> mirror back without an event).
settings.subscribe(({ field, settings: s }) => {
  if (field === '*') {
    // Reset carried defaults: re-apply every engine-facing setting now.
    applyVolumes(s.volumes)
    void applyLanguage(s.language)
  } else {
    if (field === 'volumes') applyVolumes(s.volumes)
    if (field === 'language') void applyLanguage(s.language)
  }
  syncSettingsControls()
})

/**
 * Reset all settings to their defaults and apply them to the engine +
 * UI immediately (round 87 reset): language resets to default (en), volume
 * buses return to 1.0, auto/skip off, text speed back to DEFAULT_TEXT_SPEED.
 * settings.reset() notifies subscribers ('*'), which re-applies volumes +
 * language and mirrors the controls.
 */
function resetPlayerSettings() {
  settings.reset()
  const s = settings.getAll()
  autoMode = s.autoClick
  document.getElementById('auto').textContent = autoMode ? '⏸ Auto' : '⏩ Auto'
  if (autoMode) scheduleAuto()
  else if (autoTimer) clearTimeout(autoTimer)
  syncSettingsControls()
}

/** Forward volumes to both the core state machine and the WebAudio engine. */
function applyVolumes(vols) {
  for (const b of VOLUME_BUSSES) {
    const v = Number.isFinite(vols?.[b]) ? vols[b] : 1
    try { player.core.audioSetBusVolume(b, v) } catch { /* noop */ }
    try { player.audio.setBusVolume(b, v) } catch { /* noop */ }
  }
}

/** Forward the active language to the pure-Lua i18n module. */
async function applyLanguage(lang) {
  try { await player.setLanguage(lang) } catch { /* noop */ }
}

/** Sync once from the persisted settings into the settings UI and bind the
 *  controls so any change writes straight back to the settings controller
 *  (which notifies -> engine wiring). Auto toggle also mirrors the #auto
 *  play/advance button (round 86). */
function bindSettingsControls() {
  const langEl = document.getElementById('settings-lang')
  const autoEl = document.getElementById('settings-auto')
  const speedEl = document.getElementById('settings-speed')
  const bgmEl = document.getElementById('settings-bgm')
  const seEl = document.getElementById('settings-se')
  const voiceEl = document.getElementById('settings-voice')
  // seed control values from the settings controller (kept in sync with the
  // subscriber notify path — see syncSettingsControls).
  syncSettingsControls()
  // writes: control operation -> settings controller (which notifies -> engine
  // wiring + control mirror). No echo loop: programmatic .value assignment
  // never fires change/input events.
  langEl.addEventListener('change', () => settings.set('language', langEl.value))
  autoEl.addEventListener('change', () => {
    settings.set('autoClick', autoEl.checked)
    const cur = settings.get('autoClick')
    autoMode = cur
    document.getElementById('auto').textContent = cur ? '⏸ Auto' : '⏩ Auto'
    if (cur) scheduleAuto()
    else if (autoTimer) clearTimeout(autoTimer)
  })
  speedEl.addEventListener('input', () => settings.set('textSpeed', Number(speedEl.value)))
  bgmEl.addEventListener('input', () => settings.setVolume('bgm', Number(bgmEl.value)))
  seEl.addEventListener('input', () => settings.setVolume('se', Number(seEl.value)))
  voiceEl.addEventListener('input', () => settings.setVolume('voice', Number(voiceEl.value)))
  const resetBtn = document.getElementById('settings-reset')
  if (resetBtn) resetBtn.addEventListener('click', () => resetPlayerSettings())
}

const syncTextures = () => {
  for (const [id, t] of player.core.textures) {
    // Engine texture paths are repo-relative ('assets/bg/x.png'); serve them
    // under the site root. The previous ASSET_BASE + t.path produced
    // '/assets/assets/...' (404 in real browsers; jsdom never fetches <img>,
    // so only the packaged browser run catches it — W0 修正).
    const p = String(t.path || '')
    renderer.setTextureUrl(id, /^(https?:|\/)/.test(p) ? p : (p.startsWith('assets/') ? baseHref + p : ASSET_BASE + p))
  }
}

// ---- boot-failure visibility (Sprint 1 / t3) --------------------------
// A missing or empty story bundle means the PACKAGING chain failed
// (scripts/package_game.sh -> ks_bake --web -> cache/story/story.lua).
// Historically that was swallowed: one log line plus a silent switch to the
// raw demo scenes, so a broken package still looked like a working game and
// the browser smoke stayed green. Failure is now explicit on three surfaces:
//   (a) a visible page banner (#boot-error, built here so no index.html
//       change is needed and every DOM fixture gets it too),
//   (b) window.__caesuraErrors — the machine-readable array that
//       scripts/web_browser_smoke.mjs asserts is empty,
//   (c) the demo fallback content only in an EXPLICIT dev mode.
//
// DEV_MODE is deliberately opt-in ONLY (?dev=1, or self.__CAESURA_DEV__ for
// a fixture). Host-based detection was rejected: both the vite dev server
// (127.0.0.1:5174) and the packaged-site verification harness
// (scripts/web_browser_smoke.mjs serves dist/ over 127.0.0.1) live on
// loopback, so "localhost means dev" would re-hide the failure in exactly
// the environment whose job is to catch it. An explicit flag cannot be
// inherited by accident, and a dev who really wants the raw .ks list adds
// ?dev=1 to the URL.
const DEV_MODE = (() => {
  try {
    if (typeof self !== 'undefined' && self.__CAESURA_DEV__ === true) return true
    return new URLSearchParams(location.search).get('dev') === '1'
  } catch { return false }
})()

/** Render (or update) the top-of-page error banner. Text only — the detail
 *  string comes from the network/Lua side, so it is never interpolated as
 *  HTML. Idempotent: repeated failures reuse the same element. */
function showBootError(title, detail, hint) {
  if (typeof document === 'undefined' || !document.body) return
  let el = document.getElementById('boot-error')
  if (!el) {
    el = document.createElement('div')
    el.id = 'boot-error'
    el.setAttribute('role', 'alert')
    el.style.cssText = 'margin:0;padding:12px 16px;background:#3a0f0f;color:#ffb3b3;'
      + 'border-bottom:2px solid #f66;font:14px/1.5 monospace;white-space:pre-wrap;'
    document.body.insertBefore(el, document.body.firstChild)
  }
  el.textContent = ''
  const mk = (text, css) => {
    const d = document.createElement('div')
    d.textContent = text
    if (css) d.style.cssText = css
    el.appendChild(d)
  }
  mk('\u26A0 ' + title, 'font-weight:bold;color:#ff8080')
  mk(detail)
  if (hint) mk(hint, 'color:#ffd9a0')
}

/** Single funnel for a fatal boot problem: page banner + smoke-visible error
 *  array + log + status line. Never throws (a reporting failure must not
 *  mask the failure being reported). */
function reportBootFailure(title, detail, hint) {
  const line = title + ': ' + detail
  try { window.__caesuraErrors.push(line) } catch { /* array not installed yet */ }
  try { log('FATAL ' + line) } catch { /* log element absent */ }
  try { if (statusEl) statusEl.textContent = 'FAILED: ' + title } catch { /* noop */ }
  try { showBootError(title, detail, hint) } catch { /* DOM unavailable */ }
}

const BAKE_HINT = 'Rebuild the package: bash scripts/package_game.sh <game>'
  + ' (step 2 bakes cache/story/story.lua via scripts/ks_bake.lua --web).'
  + ' Add ?dev=1 to run the raw demo scenes instead.'

async function loadStoryBundle() {
  try {
    const res = await fetch(STORY_BASE)
    if (!res.ok) throw new Error('status ' + res.status)
    const src = await res.text()
    if (!src || src.trim().length === 0) throw new Error('empty bundle')
    player.lua.global.set('STORY_SRC', src)
    const SQ = String.fromCharCode(39)
    const code = '  local chunk = assert(load(STORY_SRC, ' + SQ + '@story.lua' + SQ + ', ' + SQ + 't' + SQ + ', _ENV))' + String.fromCharCode(10) + '  return chunk()'
    const bundle = await player.lua.doString(code)
    // A bundle that parses but carries no scenes is a packaging failure too:
    // the old code fell through to populateFallbackScenes() and the player
    // silently offered demo content in place of the game.
    const sceneCount = bundle && bundle.scenes ? Object.keys(bundle.scenes).length : 0
    if (sceneCount === 0) throw new Error('bundle carries 0 scenes')
    storyBundle = bundle
    log('story bundle: ' + sceneCount + ' scenes, ' + bundle.assets.length + ' assets')
    populateScenePicker(bundle.scenes)
  } catch (e) {
    storyBundleError = String((e && e.message) || e).slice(0, 160)
    reportBootFailure(
      'story bundle unavailable (packaging failed)',
      storyBundleError + ' @ ' + STORY_BASE,
      BAKE_HINT,
    )
    if (DEV_MODE) {
      log('dev mode (?dev=1): offering the raw demo scenes as a fallback')
      populateFallbackScenes()
    } else {
      // Production: refuse to substitute content. The picker stays empty and
      // the initial auto-run is suppressed below, so nobody can mistake demo
      // scenes for the packaged game.
      log('production: demo fallback disabled — the packaged story is missing')
    }
  }
}

function populateScenePicker(scenes) {
  const sel = document.getElementById('scene')
  sel.textContent = ''
  for (const group of buildSceneOptions(scenes)) {
    const og = document.createElement('optgroup')
    og.label = group.label
    for (const opt of group.options) {
      const o = document.createElement('option')
      o.value = opt.value
      o.textContent = opt.label
      og.appendChild(o)
    }
    sel.appendChild(og)
  }
}

// When the bundle is unavailable, offer raw .ks files from /demo (flat list).
const FALLBACK_SCENES = [
  'galgame_demo.ks', 'full_pipeline_demo.ks', 'sma_demo.ks', 'showcase.ks',
  'tutorial/tutorial_01_hello.ks', 'tutorial/tutorial_02_text.ks',
  'tutorial/tutorial_03_layers.ks', 'tutorial/tutorial_04_audio.ks',
  'tutorial/tutorial_05_branching.ks', 'tutorial/tutorial_06_effects.ks',
]

function populateFallbackScenes() {
  if (document.getElementById('scene').options.length > 0) return
  const sel = document.getElementById('scene')
  sel.textContent = ''
  for (const name of FALLBACK_SCENES) {
    const o = document.createElement('option')
    o.value = name
    o.textContent = name
    sel.appendChild(o)
  }
}

let lastEndingsLen = -1
const syncEndings = () => {
  const list = player.core.endings || []
  if (list.length === lastEndingsLen) return
  lastEndingsLen = list.length
  endingsCount.textContent = String(list.length)
  endingsEl.textContent = ''
  for (const e of list) {
    const row = document.createElement('div')
    row.className = 'ending-entry'
    row.textContent = (e.id || '?') + ' — ' + (e.name || '')
    endingsEl.appendChild(row)
  }
}

const syncAudioStatus = () => {
  const bus = player.core.audioBus
  const parts = []
  if (bus.bgm && bus.bgm.playing) parts.push('BGM: ' + bus.bgm.path.split('/').pop())
  // audioPlay(kind, ...) stores the played entry directly on audioBus[kind],
  // so after any [se ...] the SE bus is the *last* entry (an object), not the
  // initial empty array — normalize both shapes so this never throws.
  const seList = Array.isArray(bus.se) ? bus.se : (bus.se ? [bus.se] : [])
  const sePlaying = seList.filter((x) => x && x.playing)
  if (sePlaying.length > 0) parts.push('SE: ' + sePlaying.length)
  if (bus.voice && bus.voice.playing) parts.push('VOICE: ' + bus.voice.path.split('/').pop())
  const txt = parts.length > 0 ? parts.join(' · ') : '—'
  if (audioStatusEl.textContent !== txt) audioStatusEl.textContent = txt
}

async function runScene(name) {
  log('running ' + name)
  statusEl.textContent = 'running…'
  const runOpts = {
    autoClick: settings.get('autoClick'),
    textSpeed: settings.get('textSpeed'),
    skip: settings.get('skipMode'),
  }
  let out
  if (storyBundle && storyBundle.scenes[name]) {
    out = await player.runFromBundle(storyBundle, name, runOpts)
  } else if (storyBundleError && !DEV_MODE) {
    // No bundle + production: the raw-.ks path would quietly play demo
    // content in place of the packaged game. Refuse, visibly.
    reportBootFailure('cannot run scene without a story bundle', name, BAKE_HINT)
    statusEl.textContent = 'FAILED: story bundle missing'
    return
  } else {
    const ks = await (await fetch(DEMO_BASE + name)).text()
    out = await player.runScene(ks, name, runOpts)
  }
  syncTextures()
  await renderer.render()
  syncBacklog()
  syncEndings()
  syncAudioStatus()
  statusEl.textContent = 'parked: ' + out
  log('result: ' + out)
}

async function advance() {
  const sel = document.getElementById('scene').value
  // VN semantics: advance resumes the previously parked scene cursor one page
  // (desktop on_click parity) instead of re-running the whole scene from token 1.
  // textSpeed/skip ride along so a mid-run setting toggle takes effect on the
  // next advanced page (the bridge re-applies them to the live/wrapped ctx).
  const advOpts = {
    autoClick: false, maxFrames: 5000, advance: true, advanceScene: sel,
    textSpeed: settings.get('textSpeed'),
    skip: settings.get('skipMode'),
  }
  let out
  if (storyBundle && storyBundle.scenes[sel]) {
    out = await player.runFromBundle(storyBundle, sel, advOpts)
  } else if (storyBundleError && !DEV_MODE) {
    reportBootFailure('cannot advance without a story bundle', String(sel), BAKE_HINT)
    statusEl.textContent = 'FAILED: story bundle missing'
    return
  } else {
    const ks = await (await fetch(DEMO_BASE + sel)).text()
    out = await player.runScene(ks, sel, advOpts)
  }
  syncTextures()
  await renderer.render()
  syncBacklog()
  syncEndings()
  syncAudioStatus()
  statusEl.textContent = 'advance: ' + out
  log('advance: ' + out)
}

await loadStoryBundle()
renderSlots()
bindSettingsControls()

document.getElementById('run').addEventListener('click', () => {
  void runScene(document.getElementById('scene').value)
})
document.getElementById('advance').addEventListener('click', () => {
  void advance()
})
document.getElementById('auto').addEventListener('click', () => {
  // persist the toggle through player settings (round 86); autoMode + UI
  // labels stay as the single source of truth derived from settings.
  settings.set('autoClick', !settings.get('autoClick'))
  autoMode = settings.get('autoClick')
  document.getElementById('auto').textContent = autoMode ? '⏸ Auto' : '⏩ Auto'
  if (autoMode) scheduleAuto()
  else if (autoTimer) clearTimeout(autoTimer)
})

// --- backlog (VN history) ---
const backlogEl = document.getElementById('backlog')
const backlogCount = document.getElementById('backlog-count')
let lastBacklogLen = -1
const syncBacklog = () => {
  const bl = player.core.backlog
  if (bl.length === lastBacklogLen) return
  lastBacklogLen = bl.length
  backlogCount.textContent = String(bl.length)
  backlogEl.textContent = ''
  bl.forEach((entry, i) => {
    const row = document.createElement('div')
    row.className = 'backlog-entry'
    row.innerHTML = '<span class="bl-line">' + (i + 1) + '</span>' + entry.text.replace(/</g, '&lt;')
    row.title = 'backlog ' + (i + 1)
    backlogEl.appendChild(row)
  })
  backlogEl.scrollTop = backlogEl.scrollHeight
}

// --- save slots (round 49) ---
const fmtTime = (ts) => (ts ? new Date(ts).toLocaleTimeString() : '—')

async function renderSlots() {
  const slots = player.listSlots()
  savesCount.textContent = String(slots.length)
  // W2: surface storage pressure (guarded span; absent in headless fixtures)
  const statEl = document.getElementById('saves-storage')
  if (statEl) {
    try {
      const st = player.storageStats()
      const kb = (st.bytesUsed / 1024).toFixed(1)
      statEl.textContent = st.slots > 0 ? String(st.slots + ' slot(s) · ' + kb + ' KB') : ''
    } catch { /* stats unavailable: leave blank */ }
  }
  savesEl.textContent = ''
  if (slots.length === 0) {
    const row = document.createElement('div')
    row.className = 'save-entry'
    row.innerHTML = '<span class="slot-meta">(no saves yet — run a scene, then Save Current)</span>'
    savesEl.appendChild(row)
    return
  }
  for (const s of slots) {
    const row = document.createElement('div')
    row.className = 'save-entry'
    const meta = document.createElement('span')
    meta.className = 'slot-meta'
    meta.textContent = 'scene ' + s.scene + ' · token ' + s.token + ' · ' + fmtTime(s.savedAt)
    const loadBtn = document.createElement('button')
    loadBtn.textContent = 'Load'
    loadBtn.addEventListener('click', async () => {
      log('loading slot ' + s.slot + '…')
      const out = await player.loadSlot(s.slot)
      syncTextures()
      await renderer.render()
      syncBacklog()
      syncEndings()
      syncAudioStatus()
      statusEl.textContent = 'load: ' + out
      log('load result: ' + out)
      renderSlots()
    })
    const delBtn = document.createElement('button')
    delBtn.textContent = 'Delete'
    delBtn.addEventListener('click', () => {
      player.deleteSlot(s.slot)
      log('deleted slot ' + s.slot)
      renderSlots()
    })
    const num = document.createElement('span')
    num.className = 'slot-num'
    num.textContent = String(s.slot).padStart(2, '0')
    row.appendChild(num)
    row.appendChild(meta)
    row.appendChild(loadBtn)
    row.appendChild(delBtn)
    savesEl.appendChild(row)
  }
}

document.getElementById('save-now').addEventListener('click', async () => {
  const slot = Number(document.getElementById('save-slot').value)
  if (!Number.isInteger(slot) || slot < 0 || slot > 99) { log('slot must be 0..99'); return }
  const ok = await player.saveCurrent(slot)
  const statEl = document.getElementById('saves-storage')
  if (ok && statEl) statEl.style.color = ''
  if (!ok) {
    // W2: quota / backend failure must be observable, not a silent no-op
    log('save FAILED (storage full or backend error) — not written to slot ' + slot)
    if (statEl) { statEl.textContent = '⚠ save failed — storage full?'; statEl.style.color = '#f66' }
  } else {
    log('saved current position to slot ' + slot)
  }
  renderSlots()
})
document.getElementById('refresh-slots').addEventListener('click', () => renderSlots())

// --- auto-advance ---
let autoMode = settings.get('autoClick') // derived from persisted settings (round 86)
let autoTimer = null
document.getElementById('auto').textContent = autoMode ? '⏸ Auto' : '⏩ Auto'
const scheduleAuto = () => {
  if (!autoMode) return
  autoTimer = setTimeout(() => { void advance(); scheduleAuto() }, 1200)
}

// render loop: sync core state to DOM every frame (CSS transitions
// interpolate sprite moves/fades between renders).
//
// t12: render() returns a promise and crosses the wasm boundary (getLayers),
// so firing it per frame without awaiting used to let slow passes overlap.
// Serialization now lives INSIDE DomRenderer.render() — an in-flight pass
// absorbs further calls into a single trailing pass that reads fresh state —
// which is the right place for it: every caller (this loop, runScene, advance,
// the save/load handlers) gets the guarantee, not just this one. The loop
// therefore stays synchronous: awaiting here would stall the backlog/endings/
// audio sync behind a wasm hop. The promise is explicitly voided, and a
// rejection is reported instead of becoming an unhandled rejection (main.mjs
// counts those as page errors via window.__caesuraErrors).
const frame = () => {
  void renderer.render().catch((e) => {
    log('render error: ' + String((e && e.message) || e).slice(0, 120))
  })
  syncBacklog()
  syncEndings()
  syncAudioStatus()
  requestAnimationFrame(frame)
}
requestAnimationFrame(frame)

// Default scene: URL ?scene=<name> wins, else the FIRST scene of the
// loaded story bundle, else the demo fallback -- a packaged game must boot
// without any manual bundle edits (Validation-Release task book §9).
const requestedScene = new URLSearchParams(location.search).get('scene')
const initialScene =
    requestedScene
    ?? (storyBundle ? Object.keys(storyBundle.scenes)[0] : null)
    ?? (DEV_MODE ? 'galgame_demo.ks' : null)
if (initialScene) {
  void runScene(initialScene)
} else {
  // Bundle load failed and this is not an explicit dev session: booting a
  // demo scene here is exactly the silent substitution t3 removes. Leave the
  // banner + __caesuraErrors entry as the only outcome.
  statusEl.textContent = 'FAILED: no story bundle — nothing to run'
  log('boot aborted: story bundle missing (see the error banner above)')
}

// Mobile Touch Gestures (M5): 2-finger tap (menu/history), 3-finger hold (skip), swipe-down (hide UI), swipe-up (backlog)
const gestureDetector = new TouchGestureDetector({
  onTwoFingerTap: () => {
    // 2-finger tap: toggle settings/system menu and backlog
    const settingsLang = document.getElementById('settings-lang')
    if (settingsLang) {
      const panel = settingsLang.closest('.panel-row') || settingsLang.parentElement
      if (panel) {
        panel.style.display = (panel.style.display === 'none') ? '' : 'none'
      }
    }
    if (backlogEl) {
      backlogEl.style.display = (backlogEl.style.display === 'none') ? '' : 'none'
    }
  },
  onThreeFingerHold: ({ active }) => {
    // 3-finger hold: fast-forward skip mode
    settings.set('skipMode', Boolean(active))
    if (player && player.core) {
      player.core.skip_mode = Boolean(active)
    }
  },
  onSwipeDown: () => {
    // Swipe-down: hide dialogue box / UI overlay
    const msgEl = document.querySelector('.caesura-message') || document.querySelector('.message-layer')
    if (msgEl) {
      msgEl.style.display = (msgEl.style.display === 'none') ? '' : 'none'
    }
  },
  onSwipeUp: () => {
    // Swipe-up: open backlog view
    if (backlogEl) {
      backlogEl.style.display = ''
      backlogEl.scrollTop = backlogEl.scrollHeight
    }
  }
})
gestureDetector.attach(stage || (typeof document !== 'undefined' ? document.body : null))
if (typeof window !== 'undefined') {
  window.__caesuraGestures = gestureDetector
}

// Register PWA Service Worker for offline asset caching.
// Registered as a CLASSIC worker on purpose (no { type: 'module' }): web/sw.js is
// written as a classic script and must stay that way. If a module worker is ever
// wanted, sw.js has to be converted in the SAME change — adding ESM syntax there
// while this call stays classic is a SyntaxError that kills the whole PWA offline
// cache silently (web/test/sw.test.mjs locks both halves of this contract).
if (typeof navigator !== 'undefined' && 'serviceWorker' in navigator && location.protocol.startsWith('http')) {
  window.addEventListener('load', () => {
    navigator.serviceWorker.register('./sw.js').catch((err) => {
      console.warn('[PWA] ServiceWorker registration failed:', err)
    })
  })
}
