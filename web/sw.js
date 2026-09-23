// Caesura Web Player Service Worker (PWA Offline Support & IndexedDB Asset Persistence)
//
// CLASSIC SCRIPT — DO NOT ADD ESM SYNTAX (`export`, or a top-level `import`).
//
// web/main.mjs registers this file with
//   navigator.serviceWorker.register('./sw.js')
// deliberately WITHOUT { type: 'module' }, so the browser parses it with the
// classic script goal. A single `export` statement here is a SyntaxError that
// aborts service-worker registration outright and silently disables the whole
// PWA offline cache. That regression already shipped once: ESM exports were added
// purely so vitest could `import` this file, and the unit suite stayed green
// while the runtime feature was dead.
//
// Consequences for how this file is structured:
//   * The only imported script is offline-assets.js, generated from the final
//     build/package tree. Production refuses to install without that manifest.
//     IndexedDB helpers remain here and are used for optional dynamic assets.
//   * web/test/sw.test.mjs therefore loads this file the way a browser does —
//     evaluated as a classic script with the worker globals stubbed — instead of
//     importing it as a module. That test also asserts the parse goal itself, so
//     re-adding `export` here now fails the suite instead of only the runtime.
// Vite and the packager change this marker in their emitted copy only.
const REQUIRE_OFFLINE_MANIFEST = false;
let OFFLINE_MANIFEST = null;
if (typeof importScripts === 'function') {
  try {
    importScripts('./offline-assets.js');
    OFFLINE_MANIFEST = self.__CAESURA_OFFLINE_MANIFEST__;
  } catch (error) {
    if (REQUIRE_OFFLINE_MANIFEST) throw error;
  }
}
if (REQUIRE_OFFLINE_MANIFEST && !OFFLINE_MANIFEST) throw new Error('Missing production offline manifest');
const WORKER_SCOPE = typeof self !== 'undefined' && self.registration
  ? self.registration.scope : 'http://localhost/';
const CACHE_PREFIX = 'caesura-offline-v2:' + encodeURIComponent(WORKER_SCOPE) + ':';
const CACHE_NAME = CACHE_PREFIX + (OFFLINE_MANIFEST ? OFFLINE_MANIFEST.revision : 'development');

const STATIC_ASSETS = [
  './',
  './index.html',
  './main.mjs',
  './bridge.js',
  './adapter-core.js',
  './audio-engine.js',
  './dom-renderer.js',
  './player-settings.js',
  './scene-options.js',
  './touch-gestures.js',
  './scripts-index.json',
  './manifest.webmanifest',
  './web-assets/glue.wasm',
  './scripts/index.json',
  './cache/story/story.lua',
  './assets/fonts/NotoSansCJKsc-Regular.otf',
  './assets/icon-192.png',
  './assets/icon-512.png'
];

function offlineResources() {
  const manifest = OFFLINE_MANIFEST;
  if (!manifest || manifest.schema !== 1 || !/^[a-f0-9]{64}$/.test(manifest.revision)
      || !Array.isArray(manifest.resources) || !manifest.resources.length) {
    throw new Error('Invalid production offline manifest');
  }
  const urls = new Set();
  let total = 0;
  for (const resource of manifest.resources) {
    if (typeof resource.url !== 'string' || !resource.url.startsWith('./')
        || /[\\?#]/.test(resource.url) || !/^[a-f0-9]{64}$/.test(resource.sha256)
        || !Number.isSafeInteger(resource.bytes) || resource.bytes < 0) throw new Error('Invalid offline resource');
    for (const part of resource.url.slice(2).split('/')) {
      const decoded = decodeURIComponent(part);
      if (!decoded || decoded === '.' || decoded === '..' || /[\\/:\0]/.test(decoded)) throw new Error('Escaping offline resource');
    }
    const url = new URL(resource.url, WORKER_SCOPE);
    if (!url.href.startsWith(WORKER_SCOPE) || urls.has(url.href)) throw new Error('Duplicate or escaping offline resource');
    urls.add(url.href);
    total += resource.bytes;
  }
  if (total !== manifest.total_bytes) throw new Error('Offline manifest size mismatch');
  if (!urls.has(new URL('./index.html', WORKER_SCOPE).href)) throw new Error('Offline manifest has no entry page');
  return manifest.resources;
}

async function installOfflineCache() {
  const cache = await caches.open(CACHE_NAME);
  if (!OFFLINE_MANIFEST) {
    // Source development has no frozen Vite output; it makes no production
    // offline guarantee. The final package path below never tolerates misses.
    await Promise.all(STATIC_ASSETS.map(async url => {
      try { const response = await fetch(url); if (response.ok) await cache.put(url, response); } catch {}
    }));
    return;
  }
  const completionKey = new URL('./offline-assets.js?complete=' + OFFLINE_MANIFEST.revision, WORKER_SCOPE).href;
  const wasComplete = await cache.match(completionKey);
  try {
    const resources = offlineResources();
    // Bound simultaneous body buffers (notably CJK fonts and author audio).
    let next = 0;
    const workers = Array.from({ length: Math.min(4, resources.length) }, async () => {
      while (next < resources.length) {
        const resource = resources[next++];
        const url = new URL(resource.url, WORKER_SCOPE).href;
        const response = await fetch(url, { cache: 'reload' });
        if (!response.ok) throw new Error('Required offline resource failed: ' + resource.url);
        const bytes = await response.clone().arrayBuffer();
        const digest = Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', bytes)))
          .map(byte => byte.toString(16).padStart(2, '0')).join('');
        if (bytes.byteLength !== resource.bytes || digest !== resource.sha256) throw new Error('Offline resource changed: ' + resource.url);
        await cache.put(url, response);
      }
    });
    // Wait for all cache writes before propagating failure; never activate a
    // partially written revision or erase a previously successful revision.
    const results = await Promise.allSettled(workers);
    const failed = results.find(result => result.status === 'rejected');
    if (failed) throw failed.reason;
    await cache.put(completionKey, new Response(OFFLINE_MANIFEST.revision));
  } catch (error) {
    if (!wasComplete) await caches.delete(CACHE_NAME);
    throw error;
  }
}

const IDB_DB_NAME = 'caesura-asset-cache:' + encodeURIComponent(WORKER_SCOPE);
const IDB_DB_VERSION = 1;
const IDB_STORE_NAME = 'assets';

/**
 * Categorize asset URL based on its file extension.
 * Supports audio (.ogg, .mp3, .wav), textures (.png, .webp, .jpg, .jpeg),
 * bytecode/script (.ksc, story.lua) and fonts (.otf, .ttf, .woff, .woff2).
 */
function getAssetCategory(url) {
  const cleanUrl = String(url || '').split(/[?#]/)[0].toLowerCase();
  if (/\.(ogg|mp3|wav)$/.test(cleanUrl)) return 'audio';
  if (/\.(png|webp|jpg|jpeg|gif)$/.test(cleanUrl)) return 'texture';
  if (/\.(ksc)$/.test(cleanUrl) || cleanUrl.endsWith('story.lua')) return 'bytecode';
  if (/\.(otf|ttf|woff|woff2)$/.test(cleanUrl)) return 'font';
  return null;
}

function isIDBCachedAsset(url) {
  return getAssetCategory(url) !== null;
}

/**
 * Open or upgrade the IndexedDB asset database.
 */
function openAssetDatabase() {
  return new Promise((resolve, reject) => {
    if (typeof indexedDB === 'undefined') {
      return reject(new Error('IndexedDB is not available in current environment'));
    }
    const request = indexedDB.open(IDB_DB_NAME, IDB_DB_VERSION);
    request.onupgradeneeded = (event) => {
      const db = event.target.result;
      if (!db.objectStoreNames.contains(IDB_STORE_NAME)) {
        const store = db.createObjectStore(IDB_STORE_NAME, { keyPath: 'url' });
        store.createIndex('category', 'category', { unique: false });
        store.createIndex('timestamp', 'timestamp', { unique: false });
      }
    };
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
  });
}

/**
 * Retrieve an asset record from IndexedDB by its URL.
 */
async function getAssetFromIDB(url) {
  const db = await openAssetDatabase();
  return new Promise((resolve, reject) => {
    const tx = db.transaction(IDB_STORE_NAME, 'readonly');
    const store = tx.objectStore(IDB_STORE_NAME);
    const req = store.get(url);
    req.onsuccess = () => resolve(req.result || null);
    req.onerror = () => reject(req.error);
  });
}

/**
 * Store an asset into IndexedDB.
 */
async function putAssetToIDB(url, blob, mimeType, category) {
  const db = await openAssetDatabase();
  return new Promise((resolve, reject) => {
    const tx = db.transaction(IDB_STORE_NAME, 'readwrite');
    const store = tx.objectStore(IDB_STORE_NAME);
    const record = {
      url,
      blob,
      mimeType: mimeType || (blob && blob.type) || 'application/octet-stream',
      size: (blob && blob.size) || 0,
      timestamp: Date.now(),
      category: category || getAssetCategory(url) || 'misc'
    };
    const req = store.put(record);
    req.onsuccess = () => resolve(record);
    req.onerror = () => reject(req.error);
  });
}

/**
 * Delete a specific asset from IndexedDB.
 */
async function deleteAssetFromIDB(url) {
  url = assetCacheKey(url);
  const db = await openAssetDatabase();
  return new Promise((resolve, reject) => {
    const tx = db.transaction(IDB_STORE_NAME, 'readwrite');
    const store = tx.objectStore(IDB_STORE_NAME);
    const req = store.delete(url);
    req.onsuccess = () => resolve(true);
    req.onerror = () => reject(req.error);
  });
}

/**
 * Clear all records in the IndexedDB asset store.
 */
async function clearAssetCacheIDB() {
  const db = await openAssetDatabase();
  return new Promise((resolve, reject) => {
    const tx = db.transaction(IDB_STORE_NAME, 'readwrite');
    const store = tx.objectStore(IDB_STORE_NAME);
    const req = store.clear();
    req.onsuccess = () => resolve(true);
    req.onerror = () => reject(req.error);
  });
}

/**
 * Compute aggregate statistics of cached assets.
 */
async function getCacheStatsIDB() {
  const db = await openAssetDatabase();
  return new Promise((resolve, reject) => {
    const tx = db.transaction(IDB_STORE_NAME, 'readonly');
    const store = tx.objectStore(IDB_STORE_NAME);
    const req = store.getAll();
    req.onsuccess = () => {
      const all = req.result || [];
      let totalBytes = 0;
      const byCategory = { audio: 0, texture: 0, bytecode: 0, font: 0, misc: 0 };
      for (const item of all) {
        totalBytes += Number(item.size || (item.blob && item.blob.size) || 0);
        const cat = item.category || 'misc';
        byCategory[cat] = (byCategory[cat] || 0) + 1;
      }
      resolve({
        count: all.length,
        totalBytes,
        byCategory,
        assets: all.map((a) => ({
          url: a.url,
          category: a.category,
          size: a.size,
          mimeType: a.mimeType,
          timestamp: a.timestamp
        }))
      });
    };
    req.onerror = () => reject(req.error);
  });
}

/**
 * Explicitly download and store an asset into IndexedDB.
 */
function assetCacheKey(url) {
  if (OFFLINE_MANIFEST) {
    url = new URL(url, WORKER_SCOPE).href;
    if (!url.startsWith(WORKER_SCOPE)) throw new Error('Asset URL is outside this game scope');
  }
  return url;
}

async function cacheAssetUrl(url) {
  url = assetCacheKey(url);
  const res = await fetch(url);
  if (!res.ok) throw new Error(`HTTP ${res.status} fetching ${url}`);
  const blob = await res.blob();
  const mimeType = res.headers.get('content-type') || (blob && blob.type) || 'application/octet-stream';
  const category = getAssetCategory(url);
  return putAssetToIDB(url, blob, mimeType, category);
}

// ---------------------------------------------------------------------------
// Service Worker Lifecycle Events
// ---------------------------------------------------------------------------

if (typeof self !== 'undefined' && typeof self.addEventListener === 'function') {
  self.addEventListener('install', event => {
    event.waitUntil(installOfflineCache().then(() => self.skipWaiting()));
  });

  self.addEventListener('activate', event => {
    event.waitUntil(caches.keys().then(keys => Promise.all(keys
      .filter(key => key.startsWith(CACHE_PREFIX) && key !== CACHE_NAME)
      .map(key => caches.delete(key)))).then(() => self.clients.claim()));
  });

  self.addEventListener('fetch', event => {
    const request = event.request;
    if (request.method !== 'GET') return;
    const url = new URL(request.url);
    if (!url.href.startsWith(WORKER_SCOPE)) return;
    event.respondWith((async () => {
      const cache = await caches.open(CACHE_NAME);
      // First-install resources all use the same cache for both writes and
      // reads, including icons/fonts that otherwise qualify for IndexedDB.
      const lookup = new URL(request.url);
      lookup.search = ''; lookup.hash = '';
      const cached = await cache.match(lookup.href);
      if (cached) return cached;
      const isAsset = isIDBCachedAsset(url.pathname);
      const declared = OFFLINE_MANIFEST && OFFLINE_MANIFEST.resources.some(resource =>
        new URL(resource.url, WORKER_SCOPE).href === lookup.href);
      if (isAsset && !declared) {
        const record = await getAssetFromIDB(request.url).catch(() => null);
        if (record && record.blob) return new Response(record.blob, {
          headers: { 'Content-Type': record.mimeType || 'application/octet-stream' }
        });
      }
      try {
        const response = await fetch(request);
        if (response.ok && (!OFFLINE_MANIFEST || (isAsset && !declared))) {
          const copy = response.clone();
          const pending = isAsset
            ? copy.blob().then(blob => putAssetToIDB(request.url, blob, response.headers.get('content-type'), getAssetCategory(url.pathname)))
            : cache.put(request, copy);
          event.waitUntil(pending.catch(error => console.warn('[SW] Dynamic cache failed:', error)));
        }
        return response;
      } catch {
        if (request.destination === 'document' || request.mode === 'navigate') {
          const entry = await cache.match(new URL('./index.html', WORKER_SCOPE).href);
          if (entry) return entry;
        }
        return new Response('Resource unavailable offline', { status: 503 });
      }
    })());
  });

  // Client messaging for asset cache operations
  self.addEventListener('message', async (event) => {
    const data = event.data || {};
    const replyPort = event.ports && event.ports[0];
    const respond = (msg) => {
      if (replyPort) {
        replyPort.postMessage(msg);
      } else if (event.source && typeof event.source.postMessage === 'function') {
        event.source.postMessage(msg);
      }
    };

    try {
      switch (data.type) {
        case 'CACHE_ASSET': {
          const url = data.url;
          if (!url) throw new Error('Missing url in CACHE_ASSET');
          const record = await cacheAssetUrl(url);
          respond({ type: 'CACHE_ASSET_SUCCESS', url, size: record.size, success: true });
          break;
        }
        case 'CLEAR_ASSET_CACHE': {
          await clearAssetCacheIDB();
          respond({ type: 'CLEAR_ASSET_CACHE_SUCCESS', success: true });
          break;
        }
        case 'GET_CACHE_STATS': {
          const stats = await getCacheStatsIDB();
          respond({ type: 'GET_CACHE_STATS_SUCCESS', success: true, ...stats });
          break;
        }
        case 'DELETE_ASSET': {
          const url = data.url;
          await deleteAssetFromIDB(url);
          respond({ type: 'DELETE_ASSET_SUCCESS', url, success: true });
          break;
        }
        case 'SKIP_WAITING': {
          if (typeof self.skipWaiting === 'function') self.skipWaiting();
          respond({ type: 'SKIP_WAITING_SUCCESS', success: true });
          break;
        }
        default:
          break;
      }
    } catch (err) {
      respond({ type: 'ERROR', error: String(err.message || err), success: false });
    }
  });
}
