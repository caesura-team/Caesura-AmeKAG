// Inventory the final delivered runtime, after author overrides and bundling.
// This file is a build helper; the emitted file is a classic worker script.
import { createHash } from 'node:crypto'
import { lstatSync, readFileSync, readdirSync, writeFileSync } from 'node:fs'
import { join, resolve } from 'node:path'

const hash = bytes => createHash('sha256').update(bytes).digest('hex')
export function writeOfflineManifest(directory, { requiredAssets = [] } = {}) {
  const root = resolve(directory), files = new Map()
  function add(path) {
    const absolute = join(root, path)
    if (!lstatSync(absolute).isFile()) throw new Error('Offline resource must be an ordinary file: ' + path)
    files.set(path, readFileSync(absolute))
  }
  function walk(path, include = () => true) {
    if (!lstatSync(join(root, path)).isDirectory()) throw new Error('Offline directory must be ordinary: ' + path)
    for (const entry of readdirSync(join(root, path), { withFileTypes: true })) {
      const name = path + '/' + entry.name
      if (entry.isSymbolicLink()) throw new Error('Offline inventory cannot follow links: ' + name)
      if (entry.isDirectory()) walk(name, include)
      else if (!entry.isFile()) throw new Error('Offline inventory requires regular files: ' + name)
      else if (include(name)) add(name)
    }
  }
  add('index.html'); add('manifest.webmanifest')
  walk('web-assets'); walk('assets'); walk('cache')
  walk('scripts', name => name.endsWith('.lua') || name === 'scripts/index.json')
  const localPath = reference => {
    if (typeof reference !== 'string' || /[\\?#]/.test(reference)) throw new Error('Unsafe offline reference: ' + reference)
    const path = reference.replace(/^\.\//, '').split('/').map(part => {
      const decoded = decodeURIComponent(part)
      if (!decoded || decoded === '.' || decoded === '..' || /[\\/:\0]/.test(decoded)) throw new Error('Unsafe offline reference: ' + reference)
      return decoded
    }).join('/')
    return path
  }
  function requireFile(reference) {
    const path = localPath(reference)
    if (!files.has(path)) throw new Error('Missing required offline resource: ' + path)
  }
  for (const file of ['web-assets/glue.wasm', 'scripts/index.json', 'cache/story/story.lua', ...requiredAssets]) requireFile(file)
  for (const match of files.get('index.html').toString('utf8').matchAll(/<(?:script|link)\b[^>]*?\b(?:src|href)=["']([^"']+)["']/gi)) requireFile(match[1])
  const app = JSON.parse(files.get('manifest.webmanifest').toString('utf8'))
  requireFile(app.start_url)
  for (const icon of app.icons || []) requireFile(icon.src)
  const modules = JSON.parse(files.get('scripts/index.json').toString('utf8'))
  for (const [name, enabled] of Object.entries(modules)) {
    if (enabled !== true || !/^[\w.-]+$/.test(name)) throw new Error('Invalid offline Lua module index')
    requireFile('scripts/' + name.replaceAll('.', '/') + '.lua')
  }
  const worker = join(root, 'sw.js')
  if (!lstatSync(worker).isFile()) throw new Error('Offline worker must be an ordinary file')
  const source = readFileSync(worker, 'utf8')
  const production = source.replace('const REQUIRE_OFFLINE_MANIFEST = false;', 'const REQUIRE_OFFLINE_MANIFEST = true;')
  if (!production.includes('const REQUIRE_OFFLINE_MANIFEST = true;')) throw new Error('Offline worker has no production manifest contract')
  const resources = [...files].sort(([a], [b]) => a < b ? -1 : a > b ? 1 : 0).map(([path, bytes]) => ({
    url: './' + path.split('/').map(encodeURIComponent).join('/'), bytes: bytes.length, sha256: hash(bytes),
  }))
  const worker_sha256 = hash(production)
  const manifest = { schema: 1, revision: hash(JSON.stringify({ worker_sha256, resources })), worker_sha256,
    resources, total_bytes: resources.reduce((sum, item) => sum + item.bytes, 0) }
  // Exclude both generated inventories and the worker itself from resources:
  // no recursive hash cycle, but the revision still binds the worker bytes.
  writeFileSync(worker, production)
  writeFileSync(join(root, 'offline-assets.js'), 'self.__CAESURA_OFFLINE_MANIFEST__ = ' + JSON.stringify(manifest) + ';\n')
  return manifest
}
