// Build-time identity for the actual Web player and its Lua runtime.
import { createHash } from 'node:crypto'
import { readFileSync, readdirSync, existsSync, lstatSync } from 'node:fs'
import { dirname, isAbsolute, join, relative, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { catalogSha256 } from '../web/capability-catalog.js'

const hash = file => createHash('sha256').update(readFileSync(file)).digest('hex')
const hashBytes = bytes => createHash('sha256').update(bytes).digest('hex')
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const sorted = value => Object.fromEntries(Object.entries(value).sort(([a], [b]) => a < b ? -1 : a > b ? 1 : 0))
const verifiedProfiles = new WeakSet()
const excludedWebDirectories = new Set(['dist', 'node_modules', 'test', 'tests', 'test-support',
  'test-results', 'playwright-report', 'coverage', 'artifacts', '__pycache__'])
const buildHelpers = ['scripts/copy_tree.mjs', 'scripts/web_capability_profile.mjs', 'config/runtime-capabilities.json']
const requiredSources = ['web/index.html', 'web/vite.config.js', 'web/gen-index.mjs', 'web/package.json',
  'scripts/capability_catalog.lua', 'scripts/target_capabilities.lua', 'scripts/capability_runtime.lua',
  'scripts/backend.lua', ...buildHelpers]
const pathKey = (root, file) => relative(root, file).split(/[\\/]/).join('/')
function regularFile(path) {
  if (!lstatSync(path).isFile()) throw new Error('Web profile requires an ordinary file: ' + path)
}
function ordinaryDirectory(path) {
  if (!lstatSync(path).isDirectory()) throw new Error('Web profile requires an ordinary directory: ' + path)
}
function hashMap(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return false
  const entries = Object.entries(value)
  return entries.length > 0 && entries.every(([key, digest]) =>
    key && !isAbsolute(key) && !key.includes('\\') && !key.includes(':')
    && key.split('/').every(part => part && part !== '.' && part !== '..')
    && typeof digest === 'string' && /^[a-f0-9]{64}$/.test(digest))
}
function equalFiles(expected, actual, label) {
  if (!hashMap(expected) || Object.keys(expected).length !== Object.keys(actual).length) {
    throw new Error(label + ' has an incomplete file inventory')
  }
  for (const [file, digest] of Object.entries(actual)) {
    if (!Object.hasOwn(expected, file) || expected[file] !== digest) throw new Error(label + ' changed: ' + file)
  }
}
export function collectWebSourceFiles(sourceRoot = ROOT) {
  const files = {}
  function add(file) { regularFile(file); files[pathKey(sourceRoot, file)] = hash(file) }
  function walk(directory, web) {
    ordinaryDirectory(directory)
    for (const entry of readdirSync(directory, { withFileTypes: true })) {
      if (entry.name.startsWith('.')) continue
      if (web && (excludedWebDirectories.has(entry.name) || /\.(?:test|spec)\./.test(entry.name))) continue
      const file = join(directory, entry.name)
      if (entry.isSymbolicLink()) throw new Error('Web source profile cannot follow linked inputs')
      if (entry.isDirectory()) walk(file, web)
      else if (entry.isFile() && (web || entry.name.endsWith('.lua'))) add(file)
    }
  }
  walk(join(sourceRoot, 'web'), true)
  walk(join(sourceRoot, 'scripts'), false)
  for (const file of buildHelpers) add(join(sourceRoot, file))
  for (const file of ['package.json', 'package-lock.json', 'npm-shrinkwrap.json']) {
    if (existsSync(join(sourceRoot, file))) add(join(sourceRoot, file))
  }
  for (const file of requiredSources) {
    if (!files[file]) throw new Error('Web source profile is missing a required input: ' + file)
  }
  return Object.freeze(sorted(files))
}
export function verifyWebSourceFiles(sourceFiles, sourceRoot = ROOT) {
  equalFiles(sourceFiles, collectWebSourceFiles(sourceRoot), 'Web player build source')
  return true
}
// Rollup's actual project modules must belong to the deterministic source set.
// Third-party implementation bytes are bound by the final compiled player files;
// package manifests/locks are included above, rather than an entire node_modules tree.
export function verifyWebSourceModules(sourceFiles, moduleIds, sourceRoot = ROOT) {
  for (const id of moduleIds) {
    if (typeof id !== 'string' || id.startsWith('\0') || id === '__vite-browser-external') continue
    const file = id.split('?')[0]
    if (file.split(/[\\/]/).includes('node_modules')) continue
    if (!isAbsolute(file) || !Object.hasOwn(sourceFiles, pathKey(sourceRoot, file))) {
      throw new Error('Built Web module is outside the captured source inventory: ' + id)
    }
  }
  return true
}
export function webCatalogSha256() {
  const source = new URL('../config/runtime-capabilities.json', import.meta.url)
  if (existsSync(source)) {
    const actual = createHash('sha256').update(readFileSync(source, 'utf8').replace(/\r\n/g, '\n')).digest('hex')
    if (actual !== catalogSha256) throw new Error('Runtime capability catalog is stale; regenerate its Lua/JS data')
  }
  return catalogSha256
}
function playerFiles(root) {
  regularFile(join(root, 'index.html'))
  const files = { 'index.html': hash(join(root, 'index.html')) }
  function walk(directory) {
    ordinaryDirectory(directory)
    for (const entry of readdirSync(directory, { withFileTypes: true })) {
      if (entry.isSymbolicLink()) throw new Error('Web capability profile cannot follow linked runtime files')
      const file = join(directory, entry.name)
      if (entry.isDirectory()) walk(file)
      else if (entry.isFile()) {
        const key = relative(root, file).split(/[\\/]/).join('/')
        if (key.startsWith('web-assets/') || key.endsWith('.lua') || key === 'scripts/index.json') {
          files[key] = hash(file)
        }
      }
    }
  }
  walk(join(root, 'web-assets'))
  walk(join(root, 'scripts'))
  for (const file of ['sw.js', 'manifest.webmanifest']) {
    if (existsSync(join(root, file))) { regularFile(join(root, file)); files[file] = hash(join(root, file)) }
  }
  for (const required of ['web-assets/glue.wasm', 'scripts/index.json', 'scripts/capability_catalog.lua',
    'scripts/capability_json.lua', 'scripts/target_capabilities.lua', 'scripts/capability_runtime.lua',
    'scripts/capability_backend.lua', 'scripts/backend.lua']) {
    if (!files[required]) throw new Error('Web capability profile is missing a required runtime file: ' + required)
  }
  if (!Object.keys(files).some(key => key.startsWith('web-assets/') && key.endsWith('.js'))) {
    throw new Error('Web capability profile is missing the built player module')
  }
  return sorted(files)
}

export function createWebCapabilityProfile(root, { sourceFiles, sourceRoot = ROOT } = {}) {
  verifyWebSourceFiles(sourceFiles, sourceRoot)
  return { schema: 1, target: 'web', platform: 'browser', scope: 'build',
    catalog_sha256: webCatalogSha256(), compiled: {}, source_files: sorted(sourceFiles), bundle_files: playerFiles(root) }
}

export function verifyWebCapabilityProfile(root, profile, { sourceRoot = ROOT } = {}) {
  if (profile?.schema !== 1 || profile.target !== 'web' || profile.platform !== 'browser'
    || profile.scope !== 'build' || profile.catalog_sha256 !== webCatalogSha256()
    || !profile.compiled || typeof profile.compiled !== 'object' || Array.isArray(profile.compiled)
    || Object.keys(profile.compiled).length !== 0) {
    throw new Error('Web player capability profile does not match this runtime catalog; rebuild the player')
  }
  verifyWebSourceFiles(profile.source_files, sourceRoot)
  equalFiles(profile.bundle_files, playerFiles(root), 'Web player after capability profiling')
  const verified = Object.freeze({ schema: 1, target: 'web', platform: 'browser', scope: 'build',
    catalog_sha256: profile.catalog_sha256, compiled: Object.freeze({}),
    source_files: Object.freeze(sorted(profile.source_files)), bundle_files: Object.freeze(sorted(profile.bundle_files)) })
  verifiedProfiles.add(verified)
  return verified
}

function checkedCopyFiles(root, verifiedBuildProfile, { indexHtml, scriptsIndex, sourceRoot = ROOT } = {}) {
  if (!verifiedProfiles.has(verifiedBuildProfile)) throw new Error('A verified Web build profile is required')
  verifyWebSourceFiles(verifiedBuildProfile.source_files, sourceRoot)
  const expected = { ...verifiedBuildProfile.bundle_files }
  for (const [file, bytes] of [['index.html', indexHtml], ['scripts/index.json', scriptsIndex]]) {
    if (bytes !== undefined) {
      if (typeof bytes !== 'string' && !Buffer.isBuffer(bytes)) throw new Error('Expected transformed player bytes are invalid')
      expected[file] = hashBytes(bytes)
    }
  }
  const actual = playerFiles(root)
  equalFiles(expected, actual, 'Copied Web player')
  return actual
}

export function verifyCopiedPlayer(root, verifiedBuildProfile, options = {}) {
  checkedCopyFiles(root, verifiedBuildProfile, options)
  return true
}

export function createPackagedWebCapabilityProfile(root, verifiedBuildProfile, options = {}) {
  const copiedFiles = checkedCopyFiles(root, verifiedBuildProfile, options)
  return { schema: 1, target: 'web', platform: 'browser', scope: 'build',
    catalog_sha256: verifiedBuildProfile.catalog_sha256, compiled: {},
    source_files: sorted(verifiedBuildProfile.source_files), bundle_files: copiedFiles }
}
