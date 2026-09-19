// @vitest-environment node
import { afterEach, describe, expect, it } from 'vitest'
import { mkdtempSync, mkdirSync, readFileSync, writeFileSync, rmSync, symlinkSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join, resolve } from 'node:path'
import { createHash, webcrypto } from 'node:crypto'
import { createContext, Script } from 'node:vm'

const source = readFileSync(new URL('./sw.js', import.meta.url), 'utf8')
const hash = value => createHash('sha256').update(value).digest('hex')
const roots = []
afterEach(() => { for (const root of roots.splice(0)) rmSync(root, { recursive: true, force: true }) })
function fixture() {
  const root = mkdtempSync(join(tmpdir(), 'caesura-offline-test-'))
  roots.push(root)
  const files = {
    'index.html': '<head><link rel="manifest" href="./manifest.webmanifest"></head><script type="module" src="./web-assets/index-hashed.js"></script>',
    'web-assets/index-hashed.js': 'import "./lazy-chunk.js";',
    'web-assets/lazy-chunk.js': 'export const ready = true;',
    'web-assets/glue.wasm': 'wasm-fixture',
    'scripts/index.json': JSON.stringify({ kag: true, 'kag.compiler': true }),
    'scripts/kag.lua': 'return {}', 'scripts/kag/compiler.lua': 'return {}',
    'cache/story/story.lua': 'return {entry="作者入口.ks"}',
    'assets/章节 一/图 片.png': 'project image bytes',
    'assets/icon-192.png': 'icon192', 'assets/icon-512.png': 'icon512',
    'manifest.webmanifest': JSON.stringify({ start_url: './index.html', icons: [{ src: './assets/icon-192.png' }, { src: './assets/icon-512.png' }] }),
    'sw.js': source,
  }
  for (const [name, bytes] of Object.entries(files)) {
    mkdirSync(join(root, name, '..'), { recursive: true }); writeFileSync(join(root, name), bytes)
  }
  return { root, files }
}
async function generator() { return import('../scripts/offline_manifest.mjs') }
function manifestFor(files) {
  const resources = Object.entries(files).filter(([path]) => path !== 'sw.js').sort(([a], [b]) => a.localeCompare(b))
    .map(([path, bytes]) => ({ url: './' + path.split('/').map(encodeURIComponent).join('/'), bytes: Buffer.byteLength(bytes), sha256: hash(bytes) }))
  return { schema: 1, revision: hash(JSON.stringify(resources)), resources,
    total_bytes: resources.reduce((n, item) => n + item.bytes, 0) }
}
// Only browser storage/network APIs are substituted. The actual classic SW
// executes install, validates real response bytes, then serves a cold offline
// fetch: no online fetch event is sent to warm any runtime cache.
function worker(files, { scope = 'https://offline.invalid/', manifest = manifestFor(files), fail, quota, caches: shared, databases = new Map() } = {}) {
  const stores = shared || new Map(), listeners = new Map(), requests = []
  let idbRecords = new Map()
  const idbRequest = result => {
    const request = { result }
    queueMicrotask(() => request.onsuccess?.({ target: request }))
    return request
  }
  const openDatabase = name => {
    if (!databases.has(name)) databases.set(name, new Map())
    const records = databases.get(name)
    idbRecords = records
    const store = {
      put: record => { records.set(record.url, record); return idbRequest(record.url) },
      get: url => idbRequest(records.get(url)),
      getAll: () => idbRequest([...records.values()]),
      delete: url => { records.delete(url); return idbRequest(undefined) },
      clear: () => { records.clear(); return idbRequest(undefined) },
    }
    return idbRequest({ transaction: () => ({ objectStore: () => store }) })
  }
  let offline = false, skipped = false
  const key = input => new URL(typeof input === 'string' ? input : input.url, scope).href
  const caches = {
    keys: async () => [...stores.keys()], delete: async name => stores.delete(name),
    open: async name => {
      if (!stores.has(name)) stores.set(name, new Map())
      const data = stores.get(name)
      return { match: async input => data.get(key(input))?.clone(), put: async (input, response) => {
        if (quota) throw new Error('QuotaExceededError')
        data.set(key(input), response.clone())
      } }
    },
    match: async input => { for (const data of stores.values()) if (data.has(key(input))) return data.get(key(input)).clone() },
  }
  const sandbox = { console, URL, Request, Response, Headers, Blob, TextEncoder, crypto: webcrypto, caches,
    indexedDB: { open: openDatabase },
    registration: { scope }, location: new URL('sw.js', scope),
    addEventListener: (type, callback) => listeners.set(type, callback),
    clients: { claim: async () => {} }, skipWaiting: async () => { skipped = true },
    fetch: async input => {
      const url = key(input); requests.push(url)
      if (offline) throw new Error('offline')
      const relative = decodeURIComponent(new URL(url).pathname.slice(new URL(scope).pathname.length)) || 'index.html'
      if (fail === relative) return new Response('missing', { status: 404 })
      return new Response(files[relative] ?? 'missing', { status: Object.hasOwn(files, relative) ? 200 : 404 })
    },
  }
  sandbox.self = sandbox
  sandbox.importScripts = () => { if (!manifest) throw new Error('missing manifest'); sandbox.__CAESURA_OFFLINE_MANIFEST__ = manifest }
  new Script(source.replace('const REQUIRE_OFFLINE_MANIFEST = false;', 'const REQUIRE_OFFLINE_MANIFEST = true;')).runInContext(createContext(sandbox))
  const event = async name => { let done; listeners.get(name)({ waitUntil: p => { done = p } }); return done }
  return { stores, requests, get idbRecords() { return idbRecords }, install: () => event('install'), activate: () => event('activate'), skipped: () => skipped,
    message: async data => {
      const replies = []
      await listeners.get('message')({ data, ports: [{ postMessage: reply => replies.push(reply) }] })
      return replies
    },
    offline: () => { offline = true }, fetch: async relative => {
      let response; const pending = []
      listeners.get('fetch')({ request: new Request(new URL(relative, scope)), respondWith: p => { response = p }, waitUntil: p => pending.push(p) })
      const result = await response; await Promise.all(pending); return result
    } }
}

describe('final package offline resources', () => {
  it.each(['/', '/games/作品/'])('first install alone serves hashed chunks, full Lua and binary assets offline at %s', async path => {
    const { files } = fixture(), sw = worker(files, { scope: 'https://offline.invalid' + encodeURI(path) })
    await sw.install(); await sw.activate(); sw.offline()
    for (const file of ['index.html', 'web-assets/index-hashed.js', 'web-assets/lazy-chunk.js', 'web-assets/glue.wasm', 'scripts/kag/compiler.lua', 'assets/icon-192.png', 'assets/章节 一/图 片.png']) {
      const response = await sw.fetch(file.split('/').map(encodeURIComponent).join('/'))
      expect(response?.status, file).toBe(200); expect(await response.text()).toBe(files[file])
    }
    expect(sw.skipped()).toBe(true)
  })
  it.each(['web-assets/index-hashed.js', 'scripts/kag/compiler.lua', 'assets/icon-192.png'])('missing required %s rejects install', async fail => {
    const sw = worker(fixture().files, { fail })
    await expect(sw.install()).rejects.toThrow(); expect(sw.skipped()).toBe(false)
  })
  it('changed bytes and storage quota both reject installation', async () => {
    const { files } = fixture(), manifest = manifestFor(files)
    files['web-assets/index-hashed.js'] = 'changed bytes'
    for (const options of [{ manifest }, { quota: true }]) {
      const sw = worker(files, options)
      await expect(sw.install()).rejects.toThrow(); expect(sw.skipped()).toBe(false)
    }
  })
  it('new revision never accepts old cached resources and activation preserves another scope', async () => {
    const { files } = fixture(), first = worker(files)
    await first.install(); const firstKeys = [...first.stores.keys()]
    const other = worker(files, { scope: 'https://offline.invalid/other/', caches: first.stores })
    await other.install(); await other.activate()
    expect(firstKeys.every(key => first.stores.has(key))).toBe(true)
    const changed = { ...files, 'web-assets/index-hashed.js': 'new build' }
    const broken = worker(changed, { caches: first.stores, fail: 'web-assets/index-hashed.js' })
    await expect(broken.install()).rejects.toThrow()
    expect(firstKeys.every(key => first.stores.has(key))).toBe(true)
  })
  it('explicitly cached optional media remains readable offline in a production package', async () => {
    const { files } = fixture(), manifest = manifestFor(files)
    files['assets/optional.ogg'] = 'downloaded optional author audio'
    const sw = worker(files, { manifest })
    await sw.install(); await sw.activate()
    const url = 'https://offline.invalid/assets/optional.ogg'
    const replies = await sw.message({ type: 'CACHE_ASSET', url })
    expect(replies).toEqual([{ type: 'CACHE_ASSET_SUCCESS', url,
      size: Buffer.byteLength(files['assets/optional.ogg']), success: true }])
    expect(sw.idbRecords.has(url)).toBe(true)
    sw.offline()
    const response = await sw.fetch('assets/optional.ogg')
    expect(response.status).toBe(200)
    expect(await response.text()).toBe(files['assets/optional.ogg'])
  })
  it('deletes explicitly cached optional media using the same relative URL', async () => {
    const { files } = fixture(), manifest = manifestFor(files)
    files['assets/optional.ogg'] = 'downloaded optional author audio'
    const sw = worker(files, { manifest })
    await sw.install(); await sw.activate()
    const url = './assets/optional.ogg'
    const cached = await sw.message({ type: 'CACHE_ASSET', url })
    expect(cached[0].success).toBe(true)
    expect(sw.idbRecords.size).toBe(1)
    const deleted = await sw.message({ type: 'DELETE_ASSET', url })
    expect(deleted[0].success).toBe(true)
    expect(sw.idbRecords.size).toBe(0)
    sw.offline()
    expect((await sw.fetch('assets/optional.ogg')).status).toBe(503)
  })
  it('never substitutes prior IDB data for a missing declared production resource', async () => {
    const { files } = fixture(), sw = worker(files)
    await sw.install(); await sw.activate()
    const url = 'https://offline.invalid/assets/icon-192.png'
    const cached = await sw.message({ type: 'CACHE_ASSET', url })
    expect(cached[0].success).toBe(true)
    sw.idbRecords.get(url).blob = new Blob(['stale prior version icon'])
    for (const data of sw.stores.values()) data.delete(url)
    sw.offline()
    expect((await sw.fetch('assets/icon-192.png')).status).toBe(503)
  })
  it('clearing optional assets in one game preserves another game on the same origin', async () => {
    const { files } = fixture(), manifest = manifestFor(files), databases = new Map()
    const first = worker({ ...files, 'assets/optional.ogg': 'first game audio' },
      { manifest, scope: 'https://offline.invalid/first/', databases })
    const second = worker({ ...files, 'assets/optional.ogg': 'second game audio' },
      { manifest, scope: 'https://offline.invalid/second/', databases })
    await first.install(); await first.activate()
    await second.install(); await second.activate()
    for (const sw of [first, second]) {
      expect((await sw.message({ type: 'CACHE_ASSET', url: './assets/optional.ogg' }))[0].success).toBe(true)
    }
    expect((await first.message({ type: 'CLEAR_ASSET_CACHE' }))[0].success).toBe(true)
    expect(first.idbRecords.size).toBe(0)
    second.offline()
    const response = await second.fetch('assets/optional.ogg')
    expect(response.status).toBe(200)
    expect(await response.text()).toBe('second game audio')
  })
  it('failed revisions discard their partial cache without accumulating storage', async () => {
    const { files } = fixture(), stable = worker(files)
    await stable.install(); await stable.activate()
    const originalKeys = [...stable.stores.keys()]
    for (const revision of ['a', 'b', 'c']) {
      const changed = { ...files, 'web-assets/index-hashed.js': 'new revision ' + revision }
      const broken = worker(changed, { caches: stable.stores, fail: 'web-assets/index-hashed.js' })
      await expect(broken.install()).rejects.toThrow(/Required offline resource failed/)
    }
    expect([...stable.stores.keys()]).toEqual(originalKeys)
    stable.offline()
    expect(await (await stable.fetch('web-assets/index-hashed.js')).text()).toBe(files['web-assets/index-hashed.js'])
  })
  it('failed reinstall of an already complete revision preserves its valid offline cache', async () => {
    const { files } = fixture(), stable = worker(files)
    await stable.install(); await stable.activate()
    const originalKeys = [...stable.stores.keys()]
    const reinstall = worker(files, { caches: stable.stores, fail: 'web-assets/index-hashed.js' })
    await expect(reinstall.install()).rejects.toThrow(/Required offline resource failed/)
    expect([...stable.stores.keys()]).toEqual(originalKeys)
    stable.offline()
    const response = await stable.fetch('web-assets/index-hashed.js')
    expect(response.status).toBe(200)
    expect(await response.text()).toBe(files['web-assets/index-hashed.js'])
  })
  it('production requires a manifest and refuses escaping manifest entries', async () => {
    const { files } = fixture()
    expect(() => worker(files, { manifest: null })).toThrow()
    for (const url of ['../foreign', './%2e%2e/foreign', 'https://elsewhere.invalid/file', './x%2fy']) {
      const manifest = manifestFor(files); manifest.resources[0].url = url
      let failed = false
      try { await worker(files, { manifest }).install() } catch { failed = true }
      expect(failed, url).toBe(true)
    }
  })
  it('generates deterministic contained inventory after final story and author asset changes', async () => {
    const { root } = fixture(), { writeOfflineManifest } = await generator()
    const initial = writeOfflineManifest(root)
    writeFileSync(join(root, 'cache/story/story.lua'), 'return {entry="new-author.ks"}')
    writeFileSync(join(root, 'assets/章节 一/图 片.png'), 'author override')
    const result = writeOfflineManifest(root)
    expect(result.revision).not.toBe(initial.revision)
    expect(writeOfflineManifest(root)).toEqual(result)
    expect(result.resources.some(item => item.url.includes('offline-assets'))).toBe(false)
    for (const item of result.resources) {
      const bytes = readFileSync(resolve(root, decodeURIComponent(item.url)))
      expect(hash(bytes)).toBe(item.sha256); expect(bytes.length).toBe(item.bytes)
    }
    const context = createContext({ self: {} })
    new Script(readFileSync(join(root, 'offline-assets.js'), 'utf8')).runInContext(context)
    expect(context.self.__CAESURA_OFFLINE_MANIFEST__.revision).toBe(result.revision)
  })
  it('refuses missing HTML chunks, indexed Lua, icons, required media and symlink escapes', async () => {
    const { writeOfflineManifest } = await generator()
    for (const removed of ['web-assets/index-hashed.js', 'scripts/kag/compiler.lua', 'assets/icon-192.png']) {
      const { root } = fixture(); rmSync(join(root, removed))
      expect(() => writeOfflineManifest(root)).toThrow()
    }
    const { root } = fixture()
    expect(() => writeOfflineManifest(root, { requiredAssets: ['assets/missing.png'] })).toThrow()
    symlinkSync(tmpdir(), join(root, 'assets/escaped'), process.platform === 'win32' ? 'junction' : 'dir')
    expect(() => writeOfflineManifest(root)).toThrow(/link|ordinary|regular/i)
  })
})
