import { luaLiteralValue } from './lua-value.js'
// G5 path-B web player — wasmoon bridge.
// Loads the pure-Lua KAG stack + the REAL kag command table, wires the
// AdapterCore as the binding surface, and runs .ks scenes.
import { Lua } from 'wasmoon'
import { AdapterCore, LAYER_TYPE } from './adapter-core.js'
import { AudioEngine } from './audio-engine.js'
import { installLayerBridge } from './layer-bridge.js'
import { installRunnerBridge } from './runner-bridge.js'
import { createAssetRestore } from './restore-assets.js'
import { installSaveValueBridge } from './save-value-bridge.js'
import { createFontRestore } from './restore-font.js'
import { createAudioRestore } from './restore-audio.js'
import { catalogSha256 } from './capability-catalog.js'

// modules whose Lua source lives in scripts/ (NOT bindings)
// layers/audio/etc are C++-binding modules stubbed in JS below (the real
// scripts/layers.lua pulls rtt/view_id/RTT pools — too heavy for the web
// adapter; the stub honors its Type enum and node contract).
function makeDefaultStorage() {
  if (typeof localStorage !== 'undefined' && localStorage) {
    return {
      get: (k) => { try { return localStorage.getItem(k) } catch { return null } },
      set: (k, v) => { try { localStorage.setItem(k, v); return true } catch { return false } },
      del: (k) => { try { localStorage.removeItem(k) } catch { /* noop */ } },
    }
  }
  // Node/jsdom fallback: process-lifetime memory store.
  const mem = new Map()
  return {
    get: (k) => (mem.has(k) ? mem.get(k) : null),
    set: (k, v) => { mem.set(k, v); return true },
    del: (k) => { mem.delete(k) },
  }
}


const BINDING_MODULES = new Set([
  'backend', 'layers', 'audio', 'rtt', 'blend', 'transition', 'transform',
  'vfx', 'flow', 'replay', 'pool', 'config', 'system',
  'settings', 'gallery', 'music_room', 'title_menu', 'saveload_menu',
  'chapter_select', 'dev_hud', 'history_ui', 'toast', 'ks_i18n',
  'fileutil', 'sandbox',
])

/* Derive the assets/lang URL from the scripts URL: both live at the same
 * site root, so a scripts base like '/scripts/' or 'http://local/scripts/'
 * maps to '/assets/lang/' / 'http://local/assets/lang/'. Callers may pin
 * langBase explicitly (tests) to override this default. */
const langBaseFromScripts = (sb) => (typeof sb === 'string' && sb.length > 0
  ? String(sb).replace(/scripts\/?$/, '') + 'assets/lang/'
  : undefined)

// Engine audio paths are repo-relative ('assets/bgm/x.wav'); the web
// player serves them under the site root, so normalize to '/<path>' when
// the path already carries the 'assets/' prefix (W0 image/audio P0 fix:
// the previous '/assets/' + p produced '/assets/assets/...' = 404 and
// silently kept WebAudio silent while the core state machine reported it).
// W7: resolve against the page base (subpath hosting); lazily computed so
// node/jsdom test environments without a document keep functioning.
const pageBase = () => {
  try {
    if (typeof document !== 'undefined' && document && document.baseURI) {
      return String(document.baseURI).split(/[?#]/)[0].replace(/[^/]*$/, '')
    }
  } catch { /* noop */ }
  return ''
}
const defaultAudioAssetUrl = (p) => {
  const s = String(p)
  if (/^(https?:|\/)/.test(s)) return s
  if (s.startsWith('assets/')) return pageBase() + s
  return pageBase() + 'assets/' + s
}

function syncRestoredHistory(lua, core) {
  const saved = lua.global.get('__RESTORED_WEB_BACKLOG')
  if (saved == null) return
  const entries = JSON.parse(JSON.stringify(saved))
  core.backlog = (Array.isArray(entries) ? entries : []).map((entry) => {
    const text = (entry.name ? '[' + entry.name + ']' : '') + (entry.text ?? '')
    return { ...entry, text, draws: [{ t: text }] }
  })
  core._lastBacklog = core.backlog.at(-1)?.text ?? ''
  core.endings = []
  core._endingKeys.clear()
  lua.global.set('__RESTORED_WEB_BACKLOG', null)
}

export async function createPlayer({ scriptsBase, fetchImpl = fetch, wasmFile, audioContext, audioAssetUrl = defaultAudioAssetUrl, assetUrl = defaultAudioAssetUrl, decodeImage, storageBackend, capabilities, langBase = langBaseFromScripts(scriptsBase), langs = ['en', 'zh', 'ja'] }) {
  if (capabilities !== undefined && (!capabilities || typeof capabilities !== 'object' || Array.isArray(capabilities))) {
    throw new TypeError('Project capabilities must be a JSON object')
  }
  const projectCapabilityJson = JSON.stringify(capabilities === undefined ? {} : {capabilities})
  const factory = await Lua.load(wasmFile ? { wasmFile } : undefined)
  const lua = factory.createState()
  const core = new AdapterCore()
  const audio = new AudioEngine({ctx:audioContext,fetchImpl})
  lua.global.set('__CAESURA_CAPABILITY_PROFILE_JSON', () => JSON.stringify({
    schema: 1, target: 'web', platform: 'browser', scope: 'runtime', catalog_sha256: catalogSha256,
    compiled: {}, available: { audio: audio.isPlaybackAvailable() },
  }))
  const audioRestore = createAudioRestore({audio,fetchImpl,assetUrl:audioAssetUrl})
  const imageRestore = createAssetRestore({core, fetchImpl, assetUrl, decodeImage})
  const fontRestore = typeof globalThis.FontFace === 'function' && globalThis.document?.fonts
    ? createFontRestore({core, fetchImpl, assetUrl}) : null
  function solidTexture(...rgba) {
    const key = rgba.join(',')
    for (const [id, texture] of core.textures) if (texture.ordinaryColor === key) return id
    const id = imageRestore.materialize_image(imageRestore.prepare_color(...rgba))
    core.textures.get(id).ordinaryColor = key
    return id
  }
  function setCssPalette(handle, intensity, size) {
    const id = Number(handle ?? 0)
    if (id === 0) { core.setPalette(null, 0, 0); return true }
    if (!Number.isSafeInteger(id) || !core.textures.has(id) || !Number.isFinite(Number(intensity))) return false
    core.setPalette(id, Math.max(0, Math.min(1, Number(intensity))), Number(size) || 0)
    return true
  }

  // ---- lang dictionaries (round 91 i18n web parity) ------------------
  // The REAL scripts/i18n.lua loads assets/lang/<code>.lua through
  // io.open. Under wasmoon/browser that filesystem cannot reach served
  // assets, so i18n.load has always degraded to the built-in dictionary
  // stubs on web — losing the genuine dictionaries (plural `items` keys,
  // per-lang lines, fallback dict). We fetch each dictionary from
  // langBase (e.g. assets/lang/) and mount it into wasmoon's virtual FS,
  // so the unchanged i18n.load/set_language code resolves the REAL files:
  // unit strings, plural variant tables, full zh->en->raw fallback chain,
  // and graceful degradation to built-ins when a file is absent (a missing
  // lang file must still return the built-in table, not throw).
  if (langBase) {
    const base = String(langBase).replace(/[\/\s]*$/, '/')
    for (const code of langs) {
      if (!/^[A-Za-z0-9-]{1,16}$/.test(String(code))) continue
      try {
        const resp = await fetchImpl(base + code + '.lua')
        const text = await resp.text()
        if (text && text.trim().length > 0) {
          factory.mountFile('assets/lang/' + code + '.lua', text)
        }
      } catch {
        // File absent/unreadable: i18n.load falls back to built-ins.
      }
    }
  }

  // ---- save storage backend (round 46) ----
  // Desktop engine persists saves through C++ SaveManager; the web player
  // bridges [save]/[load] to a KV store. Default: browser localStorage;
  // tests inject an in-memory backend (jsdom lacks a shared origin).
  const storage = storageBackend ?? makeDefaultStorage()
  const saveKey = (slot) => 'caesura.save.' + String(slot)

  // ---- collect scripts/*.lua via HTTP ----
  const entries = await (await fetchImpl(scriptsBase + 'index.json')).json()
  const preloads = {}
  for (const name of Object.keys(entries)) {
    if (BINDING_MODULES.has(name)) continue
    let src = await (await fetchImpl(scriptsBase + name.replaceAll('.', '/') + '.lua')).text()
    if (src.charCodeAt(0) === 0xfeff) src = src.slice(1)
    preloads[name] = src
  }
  lua.global.set('__PRELOAD_MAP', Object.keys(preloads))
  for (const [name, src] of Object.entries(preloads)) {
    lua.global.set('__PRELOAD_' + name.replaceAll('.', '_'), src)
  }
  await lua.doString(`
    for _, name in ipairs(__PRELOAD_MAP) do
      local safe = name:gsub('%.', '_')
      package.preload[name] = function()
        local src = _G['__PRELOAD_' .. safe]
        return assert(load(src, '@' .. name .. '.lua', 't', _ENV))()
      end
    end
  `)

  // ---- JS binding adapter (backend.* + layers.*) ----
  // Type enum mirrors scripts/layers.lua (LAYER_BASE=1 ... LAYER_MESSAGE=6).
  const jsLayers = {
    Type: { LAYER_BASE: 1, LAYER_LAYER0: 2, LAYER_LAYER1: 3, LAYER_FORE: 4, LAYER_UI: 5, LAYER_MESSAGE: 6, LAYER_EFFECT: 7 },
    get: (id) => core.getLayer(id) ?? null,
    find: (id) => core.getLayer(id) ?? null,
    // Engine contract: Layers.ensure(ctx, name, z) - command handlers call
    // layers.ensure(ctx, "_textbox", 2). The real layers.lua ignores ctx
    // and creates/fetches the layer named `name` at z-order `z`. The web
    // stub must mirror that (and set tag=name like Layers.add_layer does,
    // so Layers.find(name) tag-matching + DomRenderer tag heuristics work).
    // (Round 74: previously (id, opts) - the Lua handlers passed (ctx,
    // _textbox, 2) which silently created a layer named after the ctx
    // table, so [textbox]/[nameplate]/[nvl] never found their layers.)
    ensure: (ctx, name, z) =>
      core.ensureLayer(String(name == null ? '' : name), {
        z: typeof z === 'number' ? z : (typeof z === 'string' ? Number(z) : undefined),
        tag: String(name == null ? '' : name),
      }),
    get_root: () => 0,
    add_layer: (parent, opts) => {
      const name = (opts && opts.name) || ('layer' + (core._seq + 1))
      return core.ensureLayer(name, opts)
    },
    bg: (name) => core.ensureLayer(name ?? 'bg', { layer_type: 1, z: 0 }),
    fg: (name) => core.ensureLayer(name ?? 'fg', { layer_type: 4, z: 1 }),
    set_layer_image: (node, tex) => core.setLayerImage(node, tex),
    set_layer_visible: (node, v) => core.setLayerVisible(node, v),
    set_layer_opacity: (node, v) => core.setLayerOpacity(node, v),
    set_z: (node, z) => core.setLayerZ(node, z),
    move_layer: (node, x, y) => core.moveLayer(node, x, y),
    set_layer_blend: () => {}, set_position: (n, x, y) => core._log('layer.position', { name: String(n), x, y }), set_options: () => {},
    fade_to: () => {}, mark_dirty: () => {}, get_layer: (id) => core.getLayer(id) ?? null,
  }

  // ---- KAG binding surface (C++ bindings in the real engine) ----
  // Save/load go through KAG.save_game / KAG.load_game in the desktop
  // engine; the web player has no SaveManager, so expose honest stubs
  // that return nil (engine-side handlers degrade to "error" results
  // instead of crashing on a nil call).
  const jsKAG = {
    // Serialize the engine state table (Lua -> JS object, JSON-safe) into
    // the storage backend. Returns truthy on success (engine sets
    // tf.save_result = "ok"), falsy on failure ("error").
    save_game: (slot, state, sceneName, tokenIdx, thumbnail) => {
      try {
        // Slot contract: integer 0..99 (engine SaveManager manual range);
        // anything else is an invalid slot -> honest error result.
        const sv = Number(slot)
        if (!Number.isInteger(sv) || sv < 0 || sv > 99) {
          core._log('save.error', { slot: Number(slot), error: 'invalid slot' })
          return null
        }
        // Keep logical scene identities intact, including caller frames and
        // localization keys. Restore is limited to the host's trusted map.
        const st = state && typeof state === 'object' ? { ...state } : {}
        const scene = String(sceneName ?? '')
        const payload = JSON.stringify({
          state: st,
          scene,
          token: Number(tokenIdx) || 1,
          thumbnail: String(thumbnail ?? ''),
          savedAt: Date.now(),
        })
        const ok = storage.set(saveKey(slot), payload)
        core._log('save.write', { slot: Number(slot), ok: !!ok })
        return ok
      } catch (e) {
        core._log('save.error', { slot: Number(slot), error: String(e) })
        return null
      }
    },
    // Raw read used by the Lua-side load_game wrapper. Returns the
    // stored state as a Lua literal string (the wrapper parses it with
    // load('return ' .. s) into a real table).
    _load_raw: (slot) => {
      const sv = Number(slot)
      if (!Number.isInteger(sv) || sv < 0 || sv > 99) return null
      const raw = storage.get(saveKey(slot))
      core._log('save.read', { slot: Number(slot), found: raw != null })
      if (raw == null) return null
      try {
        const payload = JSON.parse(raw)
        return luaLiteralValue(payload.state ?? {})
      } catch {
        return null
      }
    },
    capture_thumbnail: () => null,
  }
  const jsBackend = {
    // Logical resolution for the Lua layout stack (scripts/viewport.lua):
    // the web player's #stage is the render target, so viewport-following
    // layout defaults (bg/fg layer sizes, message box, dialogue positions)
    // must resolve to the STAGE size, not the desktop 1920x1080 default.
    // Call-time lookup (the stage element exists only after the DOM is
    // mounted); jsdom tests report 0x0 and fall back to the classic
    // 1280x720 stage (matching the hardcoded values the desktop engine
    // used before the resolution change).
    get_resolution: () => {
      // NOTE: wasmoon marshals a JS ARRAY return as a single opaque value
      // (neither string nor indexable table). Return a STRING in the
      // engine's "<w>x<h>" shape; the Lua wrapper in the bootstrap parses
      // it back into two numeric returns for viewport.lua / rtt.lua.
      try {
        const el = document.getElementById('stage')
        if (el) {
          const w = el.clientWidth, h = el.clientHeight
          if (w > 0 && h > 0) {
            if (typeof window !== 'undefined') window.__caesuraLayoutRes = [w, h]
            return String(w + 'x' + h)
          }
        }
      } catch { /* jsdom / pre-DOM */ }
      if (typeof window !== 'undefined') window.__caesuraLayoutRes = [1280, 720]
      return '1280x720'
    },
    // audio_play routes through the real WebAudio engine when available;
    // the core state machine always records the call (telemetry + fallback).
    audio_play: async (kind, file, opts) => {
      const k = String(kind), f = String(file)
      core.audioPlay(k, f, opts && opts.volume)
      return await audio.play(k, f, {volume:opts && opts.volume,loop:opts?.loop ?? k==='bgm',assetUrl:audioAssetUrl})
    },
    audio_stop: (kind) => { const k = String(kind); core.audioStop(k); audio.stop(k) },
    audio_xfade: () => {},
    audio_is_playing: (kind) => {
      const key=String(kind),playing=audio.isPlaying(key)
      if (!playing && core.audioBus[key]?.playing) core.audioEnded(key)
      return playing
    },
    audio_set_bus_volume: (kind, v) => { core.audioSetBusVolume(String(kind), v); audio.setBusVolume(String(kind), v) },
    audio_fade_volume: () => {},
    load_texture: (f) => {
      // resolve via mods.resolve-like identity; fetch metadata in browser
      const id = core.loadTexture(f)
      return id
    },
    load_texture_async: (f) => core.loadTexture(f),
    create_solid_texture: solidTexture, destroy_texture: (id) => core.destroyTexture(id),
    // -- palette / LUT bridge (round 77) -------------------------------
    // scripts/palette.lua drives LUT color grading through these four
    // backend.* entries (load_image / is_valid / set_palette /
    // destroy_texture). Earlier load_image+set_palette were missing, so
    // [palette effect=night] crashed at palette.lua:39 (field load_image)
    // once lut_available() returned true. Web grading is a fixed CSS
    // approximation: the LUT pixels are not sampled by the DOM renderer.
    load_image: (f) => core.loadTexture(String(f ?? '')),
    is_valid: (h) => core.textures.has(Number(h)),
    is_valid_handle: (type, h) => core.textures.has(Number(h)), // t214: real-name alias (palette.lua); HandleType::TEXTURE=0
    set_palette: setCssPalette,
    font_render_text: () => 1, font_clear: () => {}, line_height: () => 22,
    // Lua contract: render_text(text, x, y, r, g, b, a) — the text is the
    // first arg; x/y/r/g/b/a are render coordinates we ignore in the DOM
    // overlay (TextScene/add_wrapped_spans drive the real layout).
    render_text: (...args) => { const t = args[0]; if (typeof t === 'string') core.appendText(t); return 1 }, clear_text: () => core.clearText(),
    text_render_ruby: () => {}, text_set_font: () => {}, text_reset_state: () => {},
    create_lut_texture: () => 0, render_frame: () => {}, set_screen_offset: () => {},
    is_postfx_supported: (kind) => kind === 'lut3d',
    // The Lua boundary additionally requires the author to accept this
    // approximate grade, and returns its status alongside the legacy ACK.
    set_postfx: (kind, pf) => {
      if (kind === 'lut3d') {
        return setCssPalette(pf?.lutId ?? 0, pf?.intensity ?? pf?.strength ?? 1, pf?.lutSize ?? 0)
      }
      return false
    },
    destroy_postfx: kind => kind === 'lut3d' ? setCssPalette(0, 0, 0) : false,
    clear_postfx: () => setCssPalette(0, 0, 0),
    particles_create_emitter: () => -1, particles_emit: () => false, particles_destroy_emitter: () => false, clear_particles: () => {},
    video_stop: () => {}, video_play: () => 0, video_is_playing: () => false,
    ai_available: () => false, ai_query_async: () => {}, ai_cancel: () => {},
  }
  const jsStubs = {
    audio: { lua: () => {} }, rtt: { create: () => 0, destroy: () => {}, bind: () => {} },
    blend: { lua: () => {} }, transition: { lua: () => {}, start: () => {}, is_active: () => false },
    transform: { lua: () => {} }, vfx: { lua: () => {}, flash: () => {}, shake: () => {}, quake: () => {} },
    flow: { scene_cache: () => {}, load_scene: () => {} },
    replay: { get_mode: () => 'off', save: () => {}, event_count: () => 0, load: () => {}, set_mode: () => {} },
    pool: {}, config: { ai: () => {} }, system: { lua: () => {} },
    settings: {},
    // System-UI modules: the desktop engine opens overlay screens; the web
    // player has no overlay runtime, so every entry point is a safe no-op
    // (engine handlers call show(ctx)/_hideAll(ctx) — nil would crash).
    gallery: { show: () => {}, _hideAll: () => {} },
    music_room: { show: () => {}, _hideAll: () => {} },
    history_ui: { show: () => {}, _hideAll: () => {} },
    title_menu: { show: () => {}, _hideAll: () => {} },
    saveload_menu: { show: () => {}, _hideAll: () => {} },
    chapter_select: { show: () => {}, _hideAll: () => {} },
    dev_hud: { show: () => {}, _hideAll: () => {} },
    toast: { show: () => {}, _hideAll: () => {} },
    ks_i18n: {}, fileutil: {}, sandbox: {},
    mods: { resolve: (p) => p, register: () => {}, list: () => ({}) },
    // i18n is intentionally NOT stubbed: scripts/i18n.lua is the real
    // pure-Lua module and is loaded through wasmoon (round 77 web parity),
    // so the [i18n language=] handler sees a genuine Lua table with
    // set_language and records ctx.settingsValues.language.
  }
  for (const [k, v] of Object.entries(jsStubs)) lua.global.set(k, v)
  lua.global.set('backend', jsBackend)
  lua.global.set('layers', jsLayers)
  lua.global.set('KAG', jsKAG)
  lua.global.set('__IS_WEB_PROMISE', (value) => value instanceof Promise)
  await lua.doString(`
    local is_promise = __IS_WEB_PROMISE
    function __copy_web_scenes(scenes)
      local result = {}
      for key, value in pairs(scenes or {}) do result[key] = value end
      return result
    end
    function __find_web_scene(scenes, name)
      if not scenes or type(name) ~= 'string' then return nil end
      if scenes[name] ~= nil then return scenes[name] end
      for _, prefix in ipairs({'assets/script/', 'assets/scripts/', 'demo/'}) do
        if name:sub(1, #prefix) == prefix then return scenes[name:sub(#prefix + 1)] end
      end
    end
    function __resume_web_scene(co, ...)
      local result = table.pack(coroutine.resume(co, ...))
      -- Ordinary frame waits yield no value. Nil cannot be a JS Promise;
      -- retain the original classifier for every non-nil value.
      while result[1] and coroutine.status(co) ~= 'dead' and result[2] ~= nil
        and is_promise(result[2]) do
        -- Pass the child :await() yield to Wasmoon's async outer thread.
        -- Its continuation owns the result; a frame dt is not a reply.
        coroutine.yield(result[2])
        result = table.pack(coroutine.resume(co))
      end
      return table.unpack(result, 1, result.n)
    end
    backend = _G['backend']
    layers = _G['layers']
    -- Round 77: wasmoon marshals a JS binding object as userdata, but
    -- scripts/palette.lua gates every LUT op on a type(backend) == table
    -- (lut_available). Copy the JS backend fields into a real Lua table so
    -- [palette] sees a wired backend instead of degrading to a no-op.
    do
      local jsb = _G.backend
      local t = {}
      for k, v in pairs(jsb) do t[k] = v end
      -- wasmoon marshals a JS-array return as ONE value (observed: a
      -- "1280,720" string), but the engine contract (DevCore.get_resolution,
      -- scripts/viewport.lua / rtt.lua) is TWO numeric returns. Wrap the
      -- JS call in real Lua so viewport-following layout resolves to the
      -- stage size ((1280,720)) instead of falling back to the desktop
      -- 1920x1080 default.
      local _jsRes = t.get_resolution
      local play_audio=t.audio_play
      t.audio_play=function(...) return play_audio(...):await() end
      if _jsRes ~= nil then
        t.get_resolution = function()
          local ok, r = pcall(_jsRes)
          if ok and type(r) == 'string' then
            local w, h = r:match('^(%d+)x(%d+)$')
            if w and h then return tonumber(w), tonumber(h) end
          end
          return nil
        end
      end
      _G.backend = t
    end
    do
      local original = _G.flow
      local prepared_flow = {}
      for key, value in pairs(original) do prepared_flow[key] = value end
      function prepared_flow.prepare_scene(path)
        local loader = rawget(_G, '__PREPARE_SCENE_TOKENS')
        if type(path) ~= 'string' or type(loader) ~= 'function' then
          return nil, 'No trusted scene provider'
        end
        local loaded, tokens = pcall(loader, path)
        if not loaded then return nil, tostring(tokens) end
        if type(tokens) ~= 'table' then return nil, 'Scene is unavailable: ' .. path end
        local compiled, reason = pcall(require('kag.compiler').compile, tokens)
        if not compiled then return nil, tostring(reason) end
        return {tokens=tokens, labels=tokens._compiled and tokens._compiled.labels or {}, path=path}
      end
      function prepared_flow.is_restore_scene(path)
        if type(path) ~= 'string' or #path == 0 or #path > 4096
          or path:sub(1,1) == '/' or path:find('..',1,true) or not path:match('%.ks$') then return false end
        for index = 1, #path do
          local byte = path:byte(index)
          if byte < 32 or byte == 58 or byte == 92 then return false end
        end
        local available = rawget(_G, '__HAS_RESTORE_SCENE')
        return type(available) == 'function' and available(path) == true
      end
      prepared_flow.load_scene = prepared_flow.prepare_scene
      _G.flow = prepared_flow
    end
    for _, name in ipairs({'backend','layers','audio','rtt','blend','transition','transform','vfx','flow','replay','pool','config','system','settings','gallery','music_room','title_menu','saveload_menu','chapter_select','dev_hud','history_ui','toast','ks_i18n','fileutil','sandbox','mods'}) do
      package.loaded[name] = _G[name]
    end

    -- Web parity (round 77): load the REAL pure-Lua i18n module (scripts/
    -- i18n.lua has set_language; a JS-object stub would report a non-table
    -- type and the [i18n language=] handler would degrade before recording
    -- ctx.settingsValues.language). Headless-safe: i18n.load pcall's the
    -- io.open of assets/lang/*.lua and falls back to built-in dictionaries.
    pcall(function() _G.i18n = require('i18n') end)
  `)

  // Policy is instance-local and checked before author commands can run.
  // Rejected boot configuration releases the resources already acquired here.
  lua.global.set('__CAESURA_PROJECT_CAPABILITY_JSON', projectCapabilityJson)
  try {
    await lua.doString(`
      local runtime = require('capability_runtime')
      local ok, reason, detail = runtime.configure_project_json(__CAESURA_PROJECT_CAPABILITY_JSON)
      __CAESURA_PROJECT_CAPABILITY_JSON = nil
      if not ok then error('Project capability configuration rejected: ' .. (detail and runtime.message(detail) or reason), 0) end
      require('capability_backend').install(backend)
    `)
  } catch (error) {
    for (const cleanup of [()=>imageRestore.dispose(),()=>fontRestore?.dispose(),()=>audioRestore.dispose(),()=>audio.destroy(),()=>lua.global.close()]) {
      try { cleanup() } catch { /* Preserve the original configuration error. */ }
    }
    throw error
  }

  // ---- load the real kag command table ----
  await installLayerBridge(lua, core)
  lua.global.set('__TRANSIENT_RESTORE', {
    capture:()=>({videos:0,models:0,particles:0,emitters:0,postfx:core.palette.handle==null?0:1}),
    stop:()=>{if(core.palette.handle!=null) core.setPalette(null,0,0);return true},
  })
  lua.global.set('__IMAGE_RESTORE', imageRestore)
  await lua.doString(`
    local image = __IMAGE_RESTORE
    local transient = __TRANSIENT_RESTORE
    Restore = Restore or {}
    Restore.capture_transients=function()
      local result={}
      for key,value in pairs(transient.capture()) do result[key]=value end
      return result
    end
    Restore.stop_transients=transient.stop
    Restore.prepare_image = function(path) return image.prepare_image(path):await() end
    Restore.prepare_color = image.prepare_color
    Restore.materialize_image = image.materialize_image
    Restore.discard_image = image.discard_image
    Restore.image_info = function(ticket)
      local size = image.image_info(ticket)
      return size[1], size[2]
    end
    Restore.describe_texture = function(id)
      local source = image.describe_texture(id)
      if source == nil then return nil end
      local value = {}
      for key, item in pairs(source) do value[key] = item end
      return value
    end
    __IMAGE_RESTORE = nil
    __TRANSIENT_RESTORE = nil
    local kag = require('kag')
  `)
  await installRunnerBridge(lua)



  // ---- web save/load bridge (round 46) ----
  // [save] already works through jsKAG.save_game. [load] needs a real
  // Lua table back, but wasmoon returns JS objects as userdata proxies
  // (type(v) ~= 'table'), which the engine's load handler rejects. So
  // wrap load_game in Lua: read the raw JSON via the JS stub, then
  // parse it with load() (JSON is valid Lua literal syntax) into a real
  // table. This mirrors ks_bake's encode_lua_literal round-trip.
  await lua.doString([
    // Wrap KAG in a pure-Lua table: kag_binding reads _G.KAG[name], and a
    // function stored on the JS proxy would have its return values marshaled
    // back through the bridge as userdata (type ~= 'table' breaks load).
    // A Lua-side table holding the JS functions + our load_game wrapper
    // keeps the load_game return value a genuine Lua table.
    'local _kag_js = _G.KAG',
    'local _kag_json_to_table = function(s)',
    "  local f, err = load('return ' .. s, '@save.json', 't', {})",
    '  if not f then return nil, err end',
    '  local ok, t = pcall(f)',
    "  if not ok or type(t) ~= 'table' then return nil, 'corrupt save' end",
    '  return t',
    'end',
    'local _kag = {}',
    'for k, v in pairs(_kag_js) do _kag[k] = v end',
    '_kag.load_game = function(slot)',
    '  local raw = _kag._load_raw(slot)',
    "  if raw == nil then return nil, 'no save in slot' end",
    "  if type(raw) ~= 'string' then return nil, 'bad payload' end",
    '  local t, err = _kag_json_to_table(raw)',
    "  if not t then return nil, err or 'corrupt' end",
    "  return t, 'ok'",
    'end',
    '_G.KAG = _kag',
  ].join(String.fromCharCode(10)))
  await installSaveValueBridge(lua, jsKAG.save_game)
  lua.global.set('__AUDIO_RESTORE', {
    ...audioRestore,
    stop_audio: () => {
      const result=audioRestore.stop_audio()
      for (const kind of ['bgm','se','voice']) core.audioStop(kind)
      return result
    },
    apply_audio: ticket => {
      const result=audioRestore.apply_audio(ticket)
      for (const kind of ['bgm','se','voice']) core.audioStop(kind)
      const {bgm}=audioRestore.capture_audio()
      if (bgm!==false) core.audioPlay('bgm',bgm.path,bgm.gain)
      return result
    },
  })
  await lua.doString(`
    local audio=__AUDIO_RESTORE
    Restore.capture_audio=function()
      local source=audio.capture_audio()
      local value={version=source.version,bgm=false}
      if source.bgm~=false then
        value.bgm={}
        for key,item in pairs(source.bgm) do value.bgm[key]=item end
      end
      return value
    end
    Restore.prepare_audio=function(state) return audio.prepare_audio(state):await() end
    Restore.apply_audio=audio.apply_audio
    Restore.discard_audio=audio.discard_audio
    Restore.stop_audio=audio.stop_audio
    __AUDIO_RESTORE=nil
  `)
  if (fontRestore) {
    try {
      fontRestore.apply_font(await fontRestore.prepare_font(fontRestore.default_font()))
      lua.global.set('__FONT_RESTORE', fontRestore)
      await lua.doString(`
        local font=__FONT_RESTORE
        local function value(description)
          local result={}
          for key,item in pairs(description) do result[key]=item end
          return result
        end
        Restore.capture_font=function() return value(font.capture_font()) end
        Restore.default_font=function() return value(font.default_font()) end
        Restore.prepare_font=function(state) return font.prepare_font(state):await() end
        Restore.apply_font=font.apply_font
        Restore.discard_font=font.discard_font
        Restore.clear_font=font.clear_font
        backend.text_set_font=function(face,size) return font.select_font(face or 'default',size or 24):await() end
        KAG.set_font=function(id)
          if id~=0 and id~=1 then return false end
          local ticket=font.prepare_font({version=1,active=true,font=id,path='',size=id==0 and 16 or 32}):await()
          return font.apply_font(ticket)
        end
        __FONT_RESTORE=nil
      `)
    } catch (error) {
      fontRestore.dispose()
      imageRestore.dispose()
      lua.global.close()
      throw error
    }
  }

  let driving = false
  let disposed = false
  async function drive(player, mode, name, opts, sources, bundle) {
    if (disposed) return 'ERR:player-closed'
    if (driving) return 'ERR:player-busy'
    driving = true
    try {
      lua.global.set('__WEB_REQUEST', luaLiteralValue({mode, name, opts, sources, bundle}))
      const out = await lua.doString(`
        local request = assert(load('return '..__WEB_REQUEST, '@web.request', 't', {}))()
        __WEB_REQUEST = nil
        return __DRIVE_WEB_RUNNER(request.mode, request.name, request.opts, request.sources, request.bundle)
      `)
      const draws = lua.global.get('__SCENE_DRAWS_TABLE')
      core.setDraws(draws ? JSON.parse(JSON.stringify(draws)) : [])
      if (lua.global.get('__WEB_NEW_SESSION') === true) core._lastBacklog = ''
      lua.global.set('__WEB_NEW_SESSION', null)
      syncRestoredHistory(lua, core)
      const pages = lua.global.get('__SCENE_BACKLOG')
      if (Array.isArray(pages)) {
        for (const page of pages) if (Array.isArray(page) && page.length) core.pushBacklog(JSON.parse(JSON.stringify(page)))
      }
      const endings = lua.global.get('__SCENE_ENDINGS')
      if (Array.isArray(endings)) core.recordEndings(JSON.parse(JSON.stringify(endings)))
      lua.global.set('__SCENE_BACKLOG', null)
      player._ctx = lua.global.get('__CTXREF')
      player._co = await lua.doString("return __CO and coroutine.status(__CO) or 'nil'")
      return out
    } finally { driving = false }
  }

  return {
    lua,
    core,
    audio,
    async dispose() {
      if (disposed) return true
      if (driving) return false
      disposed=true
      const errors=[]
      try {await lua.doString("local ok,err=require('kag_runner').stop(); if not ok then error(err) end")} catch(error) {errors.push(error)}
      for (const cleanup of [()=>imageRestore.dispose(),()=>fontRestore?.dispose(),()=>audioRestore.dispose(),()=>audio.destroy(),
        ...[...core.textures.keys()].map(id=>()=>core.destroyTexture(id)),()=>lua.global.close()]) {
        try {cleanup()} catch(error) {errors.push(error)}
      }
      return errors.length===0
    },
    /** Set the active UI/story language via the real pure-Lua i18n module
     *  (loaded at boot). Falls back silently when i18n is unavailable.
     *  @param {string} lang BCP-47-ish code, e.g. 'en', 'zh', 'ja-JP'. */
    async setLanguage(lang) {
      if (driving || disposed) return false
      const s = String(lang ?? '').replace(/[^A-Za-z0-9-]/g, '')
      if (!s) return false
      try {
        const cur = await lua.doString([
          "local i18n = require('i18n')",
          "if type(i18n) ~= 'table' then return false end",
          "if i18n.set_language then i18n.set_language('" + s + "') end",
          // Desktop parity (round 91): the settings menu drives the SAME
          // relocalize path as [i18n language=] — the already-displayed
          // page, backlog, active choices and cc re-localize with the new
          // dictionary. Use the live scene ctx when one is parked.
          "local okR, errR = pcall(function()",
          "  local kt = require('kag.commands.text')",
          "  local ctx = _G.__CTXREF",
          "  if type(kt) == 'table' and kt.relocalize_page and type(ctx) == 'table' then",
          "    kt.relocalize_page(ctx)",
          "  end",
          "end)",
          "if not okR then print('[setLanguage] relocalize degraded: ' .. tostring(errR)) end",
          // Publish through the runner's TextScene renderer so opacity,
          // reveal clipping, ruby and inline styles match every scene run.
          "__PUBLISH_WEB_TEXT()",
          "return i18n.current",
        ].join(String.fromCharCode(10)))
        const drawsTable = lua.global.get('__SCENE_DRAWS_TABLE')
        core.setDraws(drawsTable ? JSON.parse(JSON.stringify(drawsTable)) : [])
        return cur
      } catch { return false }
    },
    /** Run a .ks source; resolves with final ctx.token_index. */
    /** Current scene coroutine state (persisted across advances). */
    _co: null,
    _ctx: null,
    /** Start a scene; resolves DONE:n/m or WAIT:idx (parked at [p]). */
    /** Source and bundle entry points share the native runner lifecycle. */
    async runScene(ksSrc, sceneName = 'scene.ks', opts = {}) {
      return drive(this, 'source', sceneName, opts, { ...opts.sceneSources, [sceneName]: ksSrc })
    },
    async runFromBundle(bundle, sceneKey, opts = {}) {
      if (bundle?.version !== 1) return 'ERR:bundle-format-mismatch'
      if (!bundle?.scenes?.[sceneKey]) return 'ERR:scene-not-in-bundle:' + String(sceneKey)
      return drive(this, 'bundle', sceneKey, opts, opts.sceneSources ?? {}, bundle.scenes)
    },
    /** Raise the click signal for the next runScene. */
    async click() { lua.global.set('__CLICK', true) },

    // -- save-slot management (round 49) ----------------------------------
    // Storage is the same backend [save]/[load] use (localStorage default).
    /** List saved slots with metadata: [{ slot, scene, token, savedAt }]. */
    listSlots() {
      const out = []
      // storage keys are opaque; scan a sane range like the engine's
      // SaveManager (slots 0..99 are manual slots).
      for (let s = 0; s < 100; s++) {
        const raw = storage.get(saveKey(s))
        if (raw == null) continue
        try {
          const payload = JSON.parse(raw)
          out.push({
            slot: s,
            scene: String(payload.scene ?? ''),
            token: Number(payload.token) || 1,
            savedAt: Number(payload.savedAt) || 0,
          })
        } catch { /* corrupt entry: skip */ }
      }
      return out
    },

    /** Storage pressure summary for the UI: { slots, bytesUsed }.
     *  Scans the manual slot range (0..99) like listSlots, summing the
     *  stored payload bytes so the player can warn before quota limits
     *  (W2: quota failure must be observable, not silent). */
    storageStats() {
      let slots = 0
      let bytesUsed = 0
      for (let sl = 0; sl < 100; sl++) {
        const raw = storage.get(saveKey(sl))
        if (raw == null) continue
        slots++
        bytesUsed += String(raw).length
      }
      return { slots, bytesUsed }
    },

    /** Delete a saved slot. Returns true when it existed. */
    deleteSlot(slot) {
      const sv = Number(slot)
      if (!Number.isInteger(sv) || sv < 0 || sv > 99) return false
      const key = saveKey(sv)
      const existed = storage.get(key) != null
      storage.del(key)
      core._log('save.delete', { slot: Number(slot), existed })
      return existed
    },

    /** Save the CURRENT scene position (last run's ctx) into a slot.
     *  Drives the engine's own SaveCommands.save so f/sf/unlocks/backlog
     *  all persist exactly as [save slot=N] would in a running scene. */
    async saveCurrent(slot) {
      if (driving || disposed) return false
      const sv = Number(slot)
      if (!Number.isInteger(sv) || sv < 0 || sv > 99) return false
      const ctx = lua.global.get('__CTXREF')
      if (!ctx) return false
      try {
        const ok = await lua.doString([
          "local Save = require('kag.commands.save')",
          "local c = require('kag_runner').get_ctx()",
          "if type(c) ~= 'table' then return false end",
          'Save.save(c, { slot = ' + sv + ' })',
          // Success/failure is reported via ctx.tf.save_result ('ok'/'error').
          "return not not (c.tf and c.tf.save_result == 'ok')",
        ].join(String.fromCharCode(10)))
        return !!ok
      } catch (e) {
        // Lua-side failure must degrade to an honest false, never throw into
        // the UI (W2: quota/error observability at the call surface).
        core._log('save.error', { slot: sv, error: String(e) })
        return false
      }
    },

    /** Prepare a slot before retiring the current session. */
    async loadSlot(slot, opts = {}) {
      const number = Number(slot)
      if (!Number.isInteger(number) || number < 0 || number > 99) return 'ERR:invalid-slot'
      return drive(this, 'load', '', { ...opts, slot: number, autoClick: opts.autoClick ?? true }, opts.sceneSources, opts.bundle?.scenes)
    },
    /** Snapshot the real Lua layer tree (Layers.snapshot) for rendering. */
    async snapshotLayers() {
      const out = await lua.doString(`
        local layers = require('layers')
        local snaps = layers.snapshot()
        local out = {}
        for i, s in ipairs(snaps) do
          out[i] = { id = s.id, name = s.name, tag = s.tag, x = s.x, y = s.y, w = s.w, h = s.h, visible = s.visible, opacity = s.opacity, z = s.z, texture = s.texture }
        end
        return out
      `)
      return out
    },
    /** Pre-resolve texture ids to URLs (browser: asset URLs). */
    async linkTextures(assetUrl) {
      for (const [id, t] of core.textures) {
        if (t.prepared) continue
        const u = assetUrl(t.path)
        core.textures.set(id, { ...t, url: u })
      }
    },
    LAYER_TYPE,
  }
}
