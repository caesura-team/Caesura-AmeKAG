// @vitest-environment jsdom
//
// Unit tests for the PWA Service Worker (web/sw.js).
//
// HOW THIS FILE LOADS THE WORKER, AND WHY (P0 regression context):
// web/main.mjs registers the worker with
//   navigator.serviceWorker.register('./sw.js')
// and NO { type: 'module' }, so the browser parses sw.js with the CLASSIC script
// goal. A previous revision added `export` to sw.js purely so this test could
// `import` from it — the browser then rejected the worker with a SyntaxError,
// the whole PWA offline cache was dead, and this suite stayed green because
// vitest happily loaded it as ESM. "Testable" had been traded for "works".
//
// So this file now loads sw.js exactly the way a browser does: the source is
// evaluated as a classic script in a context that stands in for the worker
// global (self), with the SW lifecycle APIs stubbed, and the functions are read
// off that global afterwards. The first describe block additionally asserts the
// parse goal itself, so re-introducing ESM syntax fails the test instead of only
// failing silently at runtime.
import { describe, it, expect, beforeEach } from 'vitest'
import { readFileSync } from 'node:fs'
import { createContext, Script } from 'node:vm'
import { fileURLToPath } from 'node:url'
import { dirname, resolve } from 'node:path'

const WEB_DIR = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const SW_PATH = resolve(WEB_DIR, 'sw.js')
const SW_SOURCE = readFileSync(SW_PATH, 'utf8')

// Simple in-memory mock of IndexedDB for testing environments where IDB is unavailable or incomplete
class MockIDBRequest {
  constructor() {
    this.result = null
    this.error = null
    this.onsuccess = null
    this.onerror = null
  }
}

class MockIDBOpenDBRequest extends MockIDBRequest {
  constructor(db) {
    super()
    this.result = db
    this.onupgradeneeded = null
    setTimeout(() => {
      if (this.onupgradeneeded) {
        this.onupgradeneeded({ target: this })
      }
      if (this.onsuccess) {
        this.onsuccess({ target: this })
      }
    }, 0)
  }
}

class MockIDBObjectStore {
  constructor(name, map) {
    this.name = name
    this.map = map
  }

  createIndex(name, keyPath, options) {}

  get(key) {
    const req = new MockIDBRequest()
    setTimeout(() => {
      req.result = this.map.get(key) || null
      if (req.onsuccess) req.onsuccess({ target: req })
    }, 0)
    return req
  }

  put(val) {
    const req = new MockIDBRequest()
    setTimeout(() => {
      this.map.set(val.url, val)
      req.result = val.url
      if (req.onsuccess) req.onsuccess({ target: req })
    }, 0)
    return req
  }

  delete(key) {
    const req = new MockIDBRequest()
    setTimeout(() => {
      this.map.delete(key)
      req.result = undefined
      if (req.onsuccess) req.onsuccess({ target: req })
    }, 0)
    return req
  }

  clear() {
    const req = new MockIDBRequest()
    setTimeout(() => {
      this.map.clear()
      req.result = undefined
      if (req.onsuccess) req.onsuccess({ target: req })
    }, 0)
    return req
  }

  getAll() {
    const req = new MockIDBRequest()
    setTimeout(() => {
      req.result = Array.from(this.map.values())
      if (req.onsuccess) req.onsuccess({ target: req })
    }, 0)
    return req
  }
}

class MockIDBTransaction {
  constructor(db, mode) {
    this.db = db
    this.mode = mode
  }

  objectStore(name) {
    return new MockIDBObjectStore(name, this.db._data)
  }
}

class MockIDBDatabase {
  constructor(name, version) {
    this.name = name
    this.version = version
    this.objectStoreNames = {
      contains: (n) => this._stores.has(n)
    }
    this._stores = new Set(['assets'])
    this._data = new Map()
  }

  createObjectStore(name, options) {
    this._stores.add(name)
    return new MockIDBObjectStore(name, this._data)
  }

  transaction(names, mode) {
    return new MockIDBTransaction(this, mode)
  }
}

const mockDatabaseInstance = new MockIDBDatabase('caesura-asset-cache', 1)

/**
 * Load web/sw.js as a CLASSIC script, the way the browser does for
 * register('./sw.js') without { type: 'module' }.
 *
 * Returns { sw, listeners, context }: `sw` exposes the worker's top-level
 * functions and constants, `listeners` collects the registered SW event
 * handlers, `context` is the stand-in worker global.
 */
function loadClassicServiceWorker(overrides = {}) {
  const listeners = new Map()
  const sandbox = {
    console,
    Blob,
    Headers,
    Response,
    Request: globalThis.Request,
    URL,
    Date,
    Promise,
    setTimeout,
    clearTimeout,
    Number,
    String,
    Array,
    Object,
    Error,
    JSON,
    indexedDB: {
      open: () => new MockIDBOpenDBRequest(mockDatabaseInstance)
    },
    fetch: globalThis.fetch,
    caches: {
      open: async () => ({ put: async () => {}, match: async () => undefined }),
      keys: async () => [],
      delete: async () => true,
      match: async () => undefined
    },
    ...overrides
  }
  const context = createContext(sandbox)
  context.globalThis = context
  // The worker global: sw.js only installs its lifecycle handlers when
  // self.addEventListener exists, mirroring a real ServiceWorkerGlobalScope.
  context.self = Object.assign(context, {
    addEventListener: (type, handler) => { listeners.set(type, handler) },
    skipWaiting: () => {},
    clients: { claim: async () => {} }
  })

  new Script(SW_SOURCE, { filename: 'web/sw.js' }).runInContext(context)

  // Top-level `const` declarations (CACHE_NAME, STATIC_ASSETS) live in the
  // script's global LEXICAL scope, not on the global object, so they are read by
  // evaluating an expression inside the same context. Values are copied out as
  // plain JSON so host-side matchers are not confused by cross-realm objects.
  const readConst = (expr) => JSON.parse(new Script(`JSON.stringify(${expr})`).runInContext(context))

  return { sw: context, listeners, context, readConst }
}

const {
  getAssetCategory,
  isIDBCachedAsset,
  getAssetFromIDB,
  putAssetToIDB,
  deleteAssetFromIDB,
  clearAssetCacheIDB,
  getCacheStatsIDB
} = loadClassicServiceWorker().sw

describe('Service Worker classic-script contract (P0 regression lock)', () => {
  it('sw.js parses with the CLASSIC script goal', () => {
    // This is exactly what the browser does for register('./sw.js') with no
    // { type: 'module' }. An `export` statement throws right here.
    expect(() => new Script(SW_SOURCE, { filename: 'web/sw.js' })).not.toThrow()
  })

  it('sw.js contains no ESM syntax at all', () => {
    expect(/^\s*export[\s{]/m.test(SW_SOURCE)).toBe(false)
    // Static `import x from` / `import './x'`; dynamic import() stays allowed.
    expect(/^\s*import\s+(?![(])/m.test(SW_SOURCE)).toBe(false)
  })

  it('sw.js imports only the generated classic offline inventory', () => {
    const includes = [...SW_SOURCE.matchAll(/\bimportScripts\s*\(['"]([^'"]+)['"]\)/g)].map(match => match[1])
    expect(includes).toEqual(['./offline-assets.js'])
    // Behavioral first-install, missing-manifest and final-package inventory
    // tests live in offline-manifest.test.js and resources.test.js.
  })

  it('registers the service worker lifecycle handlers when loaded in a worker scope', () => {
    const { listeners } = loadClassicServiceWorker()
    expect([...listeners.keys()].sort()).toEqual(['activate', 'fetch', 'install', 'message'])
  })

  it('the message handler is wired to the IndexedDB helpers in the same scope', async () => {
    // Proves the lifecycle handlers really resolve the helper functions after the
    // refactor (a scoping mistake would only surface as a runtime ReferenceError
    // inside the worker, which no static check catches).
    const { listeners } = loadClassicServiceWorker()
    const onMessage = listeners.get('message')
    const replies = []
    await onMessage({
      data: { type: 'GET_CACHE_STATS' },
      ports: [{ postMessage: (m) => replies.push(m) }]
    })
    expect(replies).toHaveLength(1)
    expect(replies[0].type).toBe('GET_CACHE_STATS_SUCCESS')
    expect(replies[0].success).toBe(true)
    expect(typeof replies[0].count).toBe('number')
  })

  it('main.mjs registers sw.js as a classic worker (no { type: "module" })', () => {
    const mainSource = readFileSync(resolve(WEB_DIR, 'main.mjs'), 'utf8')
    const m = mainSource.match(/serviceWorker\.register\((.*?)\)/s)
    expect(m, 'main.mjs must still register ./sw.js').not.toBeNull()
    expect(m[1]).toContain("'./sw.js'")
    // If a module worker is ever wanted, sw.js must gain { type: 'module' } here
    // AND the ESM assertions above must be flipped in the same change.
    expect(m[1]).not.toContain('module')
  })

  it('every STATIC_ASSETS entry is a relative path the deployed root actually serves', () => {
    const { readConst } = loadClassicServiceWorker()
    const staticAssets = readConst('STATIC_ASSETS')
    expect(Array.isArray(staticAssets)).toBe(true)
    expect(staticAssets.length).toBeGreaterThan(5)
    for (const entry of staticAssets) {
      expect(entry.startsWith('./')).toBe(true)
    }
    // The worker script itself is fetched by the browser, never precached.
    expect(staticAssets).not.toContain('./sw.js')
    // Every top-level web/*.js|mjs module the player imports must be listed,
    // otherwise the "offline" shell cannot boot from cache.
    for (const shellFile of [
      './index.html', './main.mjs', './bridge.js', './adapter-core.js',
      './audio-engine.js', './dom-renderer.js', './player-settings.js',
      './scene-options.js', './touch-gestures.js'
    ]) {
      expect(staticAssets).toContain(shellFile)
    }
  })
})

describe('Service Worker IndexedDB Asset Cache (M5)', () => {
  beforeEach(async () => {
    mockDatabaseInstance._data.clear()
  })

  describe('Asset Category Identification', () => {
    it('identifies audio formats correctly', () => {
      expect(getAssetCategory('assets/bgm/theme.ogg')).toBe('audio')
      expect(getAssetCategory('https://example.com/sound.mp3?version=2')).toBe('audio')
      expect(getAssetCategory('./voice/line01.wav#t=10')).toBe('audio')
    })

    it('identifies texture formats correctly', () => {
      expect(getAssetCategory('assets/bg/school.png')).toBe('texture')
      expect(getAssetCategory('assets/fg/heroine.webp')).toBe('texture')
      expect(getAssetCategory('cg/event.jpg')).toBe('texture')
      expect(getAssetCategory('cg/photo.jpeg')).toBe('texture')
    })

    it('identifies bytecode and story bundles', () => {
      expect(getAssetCategory('cache/story/main.ksc')).toBe('bytecode')
      expect(getAssetCategory('./cache/story/story.lua')).toBe('bytecode')
    })

    it('identifies font formats', () => {
      expect(getAssetCategory('assets/fonts/NotoSans.otf')).toBe('font')
      expect(getAssetCategory('assets/fonts/Main.ttf')).toBe('font')
    })

    it('returns null for web application shell files', () => {
      expect(getAssetCategory('index.html')).toBeNull()
      expect(getAssetCategory('main.mjs')).toBeNull()
      expect(getAssetCategory('style.css')).toBeNull()
      expect(getAssetCategory('sw.js')).toBeNull()
      expect(isIDBCachedAsset('main.mjs')).toBe(false)
      expect(isIDBCachedAsset('assets/bgm/op.ogg')).toBe(true)
    })
  })

  describe('IndexedDB CRUD Operations', () => {
    it('opens database and writes/reads binary assets', async () => {
      const blob = new Blob(['sample-ogg-binary-data'], { type: 'audio/ogg' })
      const record = await putAssetToIDB('assets/bgm/test.ogg', blob, 'audio/ogg', 'audio')

      expect(record.url).toBe('assets/bgm/test.ogg')
      expect(record.mimeType).toBe('audio/ogg')
      expect(record.size).toBe(blob.size)
      expect(record.category).toBe('audio')

      const fetched = await getAssetFromIDB('assets/bgm/test.ogg')
      expect(fetched).not.toBeNull()
      expect(fetched.url).toBe('assets/bgm/test.ogg')
      expect(fetched.category).toBe('audio')
      expect(fetched.size).toBe(blob.size)
    })

    it('updates cache statistics accurately', async () => {
      const blob1 = new Blob(['audio-data-123'], { type: 'audio/ogg' })
      const blob2 = new Blob(['texture-png-45678'], { type: 'image/png' })
      const blob3 = new Blob(['ksc-bytecode-99'], { type: 'application/octet-stream' })

      await putAssetToIDB('bgm/1.ogg', blob1, 'audio/ogg', 'audio')
      await putAssetToIDB('fg/1.png', blob2, 'image/png', 'texture')
      await putAssetToIDB('scenes/1.ksc', blob3, 'application/octet-stream', 'bytecode')

      const stats = await getCacheStatsIDB()
      expect(stats.count).toBe(3)
      expect(stats.totalBytes).toBe(blob1.size + blob2.size + blob3.size)
      expect(stats.byCategory.audio).toBe(1)
      expect(stats.byCategory.texture).toBe(1)
      expect(stats.byCategory.bytecode).toBe(1)
      expect(stats.assets.length).toBe(3)
    })

    it('deletes specific assets and clears entire store', async () => {
      const blob = new Blob(['data'], { type: 'image/webp' })
      await putAssetToIDB('cg/1.webp', blob, 'image/webp', 'texture')
      await putAssetToIDB('cg/2.webp', blob, 'image/webp', 'texture')

      await deleteAssetFromIDB('cg/1.webp')
      expect(await getAssetFromIDB('cg/1.webp')).toBeNull()
      expect(await getAssetFromIDB('cg/2.webp')).not.toBeNull()

      await clearAssetCacheIDB()
      expect(await getAssetFromIDB('cg/2.webp')).toBeNull()
      const stats = await getCacheStatsIDB()
      expect(stats.count).toBe(0)
      expect(stats.totalBytes).toBe(0)
    })
  })

  describe('Asset URL Caching & Network Fallback', () => {
    it('downloads and caches an asset URL via cacheAssetUrl', async () => {
      const fakeData = 'PNG_HEADER_DATA_12345'
      // sw.js reads `fetch` from its own worker global, so the stub is injected
      // into the loaded context rather than onto globalThis.
      const { sw } = loadClassicServiceWorker({
        fetch: async () => ({
          ok: true,
          status: 200,
          blob: async () => new Blob([fakeData], { type: 'image/png' }),
          headers: new Headers({ 'content-type': 'image/png' })
        })
      })

      const record = await sw.cacheAssetUrl('assets/bg/room.png')
      expect(record.url).toBe('assets/bg/room.png')
      expect(record.category).toBe('texture')

      const cached = await sw.getAssetFromIDB('assets/bg/room.png')
      expect(cached).not.toBeNull()
      expect(cached.size).toBe(fakeData.length)
    })

    it('rejects when the network refuses the asset', async () => {
      const { sw } = loadClassicServiceWorker({
        fetch: async () => ({ ok: false, status: 404, headers: new Headers() })
      })
      await expect(sw.cacheAssetUrl('assets/bg/missing.png')).rejects.toThrow(/HTTP 404/)
    })
  })
})
