#!/usr/bin/env node
// ==============================================================================
//  Caesura (AmeKAG) — package_game.mjs
//
//  Platform-independent web packaging CLI (t179). Full behavior/port parity
//  with the former scripts/package_game.sh (275+ lines of bash) — same CLI,
//  same stdout text, same exit codes — implemented purely with Node builtins
//  (fs/path/child_process). Node is already an implicit hard dependency of web
//  packaging (vite/wasmoon toolchain); this file removes the only unnecessary
//  runtime, Git Bash. scripts/package_game.sh is now a thin wrapper that
//  exec's this file.
//
//  One-click packaging: turn a KAG .ks game into a distributable web bundle.
//  Content-author focus:  node scripts/package_game.mjs demo/example_game
//  produces dist/<game>/ — a self-contained static site the web player serves
//  (works on any static host: GitHub Pages / itch.io / Netlify / S3).
//
//  Pipeline (fail-fast)
//    1. resolve input    — a demo dir (all its .ks) or explicit .ks paths
//    2. ks_check         — static contract gate (zero errors required)
//    3. ks_bake --web    — bake the game scenes into cache/story/story.lua
//    4. assemble         — copy the built web player + runtime dirs + assets
//    5. manifest         — MANIFEST.txt (file tree + sizes)
//    6. (--zip)          — optional ZIP archive (python zipfile, as before)
//    7. (--release)      — print the CPack desktop-Release handoff (docs only)
//
//  Usage (from repo root)
//    node scripts/package_game.mjs                       # default demo/example_game
//    node scripts/package_game.mjs demo/example_game      # a whole game dir
//    node scripts/package_game.mjs path/to/game.ks        # a single scene
//    node scripts/package_game.mjs demo/tutorial          # or any dir of .ks
//    node scripts/package_game.mjs --release demo/example_game
//    node scripts/package_game.mjs --no-web-build demo/example_game
//
//  Options
//    --out <dir>       package destination (default dist/<game-name>)
//    --assets <dir>    author asset source over the shared pool, shipped as assets/
//    --no-web-build    reuse an existing web/dist; do not (re)build it
//    --skip-check      skip ordinary lint, while retaining required capability checks
//    --release         also print the CPack desktop-Release handoff (docs only)
//    --entry <scene>   select the bundle's default startup scene
//    --zip <path>      also write a ZIP archive of the package (python zipfile)
//
//  Exit: 0 = packaged, 1 = any step failed.
// ==============================================================================

import { existsSync, readdirSync, statSync, copyFileSync, readFileSync,
         mkdirSync, mkdtempSync, rmSync, writeFileSync, lstatSync, renameSync, rmdirSync, realpathSync } from 'node:fs'
import { spawnSync } from 'node:child_process'
import { createHash } from 'node:crypto'
import { luaLiteralValue } from '../web/lua-value.js'
import { copyDirectorySync } from './copy_tree.mjs'
import { join, resolve, dirname, basename, relative, isAbsolute } from 'node:path'
import { tmpdir } from 'node:os'
import { fileURLToPath } from 'node:url'
import { webCatalogSha256, verifyWebCapabilityProfile, createPackagedWebCapabilityProfile } from './web_capability_profile.mjs'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = resolve(HERE, '..')
const DEFAULT_INPUT = 'demo/example_game'
const pkg = (...a) => console.log('[package]', ...a)

// ----------------------------------------------- helpers ---------------------
function fail(msg) { console.error('[package] FATAL: ' + msg); process.exit(1) }
// Relative CLI paths are resolved against ROOT (the former script cd'd to it).
const p2r = (p) => (isAbsolute(p) ? p : resolve(ROOT, p))
function canonicalOutputPath(path) {
  // Resolve only existing parents. Never dereference the named output leaf:
  // its link/ownership checks still run before replacing anything.
  try {
    const absolute = resolve(path), tail = [basename(absolute)]
    let parent = dirname(absolute)
    while (!outputExists(parent)) {
      tail.unshift(basename(parent))
      const next = dirname(parent)
      if (next === parent) throw new Error('output has no existing parent')
      parent = next
    }
    if (!statSync(parent).isDirectory()) throw new Error('output parent is not a directory: ' + parent)
    return join(realpathSync(parent), ...tail)
  } catch (error) { fail('cannot resolve output parent: ' + error.message) }
}
// Manifest rows use '/' separators regardless of host platform (.sh find %P).
const toPosix = (s) => s.split(/[\\/]/).join('/')

function printHelp() {
  console.log(`Caesura (AmeKAG) — package_game.mjs  (port of package_game.sh)

One-click packaging: turn a KAG .ks game into a distributable web bundle.
  node scripts/package_game.mjs demo/example_game
produces dist/<game>/ — a self-contained static site the web player serves
(works on any static host: GitHub Pages / itch.io / Netlify / S3).

Pipeline (fail-fast)
  1. resolve input    — a demo dir (all its .ks) or explicit .ks paths
  2. ks_check         — static contract gate (zero errors required)
  3. ks_bake --web    — bake the game scenes into cache/story/story.lua
  4. assemble         — copy the built web player + runtime dirs + assets
  5. manifest         — MANIFEST.txt (file tree + sizes)
  6. (--zip)          — optional ZIP archive (python zipfile)
  7. (--release)      — print the CPack desktop-Release handoff (docs only)

Usage (from repo root)
  node scripts/package_game.mjs                       # default demo/example_game
  node scripts/package_game.mjs demo/example_game      # a whole game dir
  node scripts/package_game.mjs path/to/game.ks        # a single scene
  node scripts/package_game.mjs demo/tutorial          # or any dir of .ks
  node scripts/package_game.mjs --release demo/example_game
  node scripts/package_game.mjs --no-web-build demo/example_game

Options
  --out <dir>       package destination (default dist/<game-name>)
  --assets <dir>    author asset source (default: project assets/ over shared pool)
  --no-web-build    reuse an existing web/dist; do not (re)build it
  --skip-check      skip ordinary lint; required capability checks still run
  --release         also print the CPack desktop-Release handoff (docs only)
  --entry <scene>   select the bundle's default startup scene
  --zip <path>      also write a ZIP archive of the package (python zipfile)

Exit: 0 = packaged, 1 = any step failed.`)
}

// ------------------------------------------- Lua interpreter probe ----------
// Same precedence as scripts/caesura_build.py::find_lua (keep in sync):
//   explicit CAESURA_LUA (authoritative, invalid values fail without fallback) ->
//   packaged external/lua/lua[.exe] (release-package artifact; gitignored in a
//   checkout; Windows-only presence) -> build-tree lua_cli product
//   (build/lua/<config>/lua[.exe], present after cmake --build) -> PATH
//   lua5.4 / lua. FATAL only when ALL levels miss, listing every location
//   probed (honest diagnostics; never a silent skip).
function probeLua() {
  const configured = process.env.CAESURA_LUA
  if (configured !== undefined) {
    // Like Path.resolve() in caesura_build.py, a relative environment override
    // is relative to the caller's CWD. spawnSync receives the intact absolute
    // filename, including spaces/Unicode, without a shell or command splitting.
    let selected = null
    try {
      const path = resolve(configured)
      if (configured && statSync(path).isFile()) selected = path
    } catch { /* Missing/inaccessible paths are an explicit configuration error. */ }
    if (!selected) fail('CAESURA_LUA does not point at a Lua interpreter: ' + (configured || '<empty>'))
    return selected
  }
  const candidates = [
    'external/lua/lua.exe', 'external/lua/lua',
    'build/lua/Release/lua.exe', 'build/lua/Release/lua',
    'build/lua/Debug/lua.exe', 'build/lua/Debug/lua',
    'build/lua/RelWithDebInfo/lua.exe', 'build/lua/RelWithDebInfo/lua',
    'build/lua/MinSizeRel/lua.exe', 'build/lua/MinSizeRel/lua',
    'build/lua/lua.exe', 'build/lua/lua',
  ]
  for (const c of candidates) {
    const p = join(ROOT, c)
    if (existsSync(p)) return p
  }
  // PATH lua5.4 / lua (mirrors `command -v` without a shell layer).
  const win = process.platform === 'win32'
  const dirs = (process.env.PATH || '').split(win ? ';' : ':')
  const names = win ? ['lua5.4.exe', 'lua5.4', 'lua.exe', 'lua'] : ['lua5.4', 'lua']
  for (const d of dirs) {
    if (!d) continue
    for (const n of names) {
      const p = join(d.trim(), n)
      if (existsSync(p)) return p
    }
  }
  return null
}

const LUA_PATH = probeLua()
if (!LUA_PATH) {
  fail('no Lua interpreter (probed: external/lua/lua[.exe], build/lua/{Release,Debug,RelWithDebInfo,MinSizeRel}/lua[.exe], build/lua/lua[.exe], PATH lua5.4/lua)')
}

// ------------------------------------------------------------- options ------
const argv = process.argv.slice(2)
let OUT = '', ASSET_SRC = '', NO_WEB_BUILD = false, RELEASE = false, SKIP_CHECK = false, DEFER_COMPLETE = false,
    ENTRY = '', ZIP_ARCHIVE = ''
const POSITIONAL = []
for (let i = 0; i < argv.length; i++) {
  const a = argv[i]
  if (a === '--out') { OUT = argv[++i]; if (OUT === undefined) fail('--out requires a value') }
  else if (a === '--assets') { ASSET_SRC = argv[++i]; if (ASSET_SRC === undefined) fail('--assets requires a value') }
  else if (a === '--entry') { ENTRY = argv[++i]; if (ENTRY === undefined) fail('--entry requires a value') }
  else if (a === '--zip') { ZIP_ARCHIVE = argv[++i]; if (ZIP_ARCHIVE === undefined) fail('--zip requires a value') }
  else if (a === '--no-web-build') { NO_WEB_BUILD = true }
  else if (a === '--skip-check') { SKIP_CHECK = true }
  // The creator wrapper reports completion after all requested targets commit.
  else if (a === '--defer-complete') { DEFER_COMPLETE = true }
  else if (a === '--release') { RELEASE = true }
  else if (a === '-h' || a === '--help') { printHelp(); process.exit(0) }
  else if (a.startsWith('-')) fail('unknown option: ' + a)
  else POSITIONAL.push(a)
}

// ------------------------------------------------- 1. resolve input ----------
if (POSITIONAL.length === 0) POSITIONAL.push(DEFAULT_INPUT)
const KAGS = []          // absolute paths (for spawn/fs)
const KAGS_DISPLAY = []  // args as the .sh would echo them (relative, forward slash)
const excluded = new Set(['node_modules', '.git', '.svn', '__pycache__'])
function sceneFiles(directory, prefix = '') {
  const files = []
  for (const entry of readdirSync(directory, { withFileTypes: true })) {
    if (excluded.has(entry.name)) continue
    const name = prefix + entry.name
    if (entry.isDirectory()) files.push(...sceneFiles(join(directory, entry.name), name + '/'))
    else if (entry.isFile() && entry.name.endsWith('.ks')) files.push(name)
  }
  return files.sort()
}
for (const p0 of POSITIONAL) {
  const p = p2r(p0)
  const p0f = p0.replace(/\\/g, '/')
  if (p0.endsWith('.ks') && existsSync(p) && statSync(p).isFile()) {
    KAGS.push(p)
    KAGS_DISPLAY.push(p0f)
  } else if (existsSync(p) && statSync(p).isDirectory()) {
    const found = sceneFiles(p)
    for (const f of found) { KAGS.push(join(p, f)); KAGS_DISPLAY.push(p0f + '/' + f) }
  } else {
    fail('not a game dir or .ks file: ' + p0)
  }
}
const firstInput = p2r(POSITIONAL[0])
const INPUT_ROOT = statSync(firstInput).isDirectory() ? firstInput : dirname(firstInput)
const pathIdentity = path => process.platform === 'win32' ? resolve(path).toLowerCase() : resolve(path)
const includedPath = path => KAGS.find(file => pathIdentity(file) === pathIdentity(path))
let ENTRY_PATH
if (ENTRY) {
  const requested = toPosix(ENTRY)
  const projectPath = isAbsolute(requested) ? resolve(requested) : resolve(INPUT_ROOT, requested)
  const rootPath = p2r(requested)
  ENTRY_PATH = includedPath(projectPath) || includedPath(rootPath)
  if (!ENTRY_PATH && !isAbsolute(requested) && !requested.includes('/')) {
    const matches = KAGS.filter(file => basename(file) === requested)
    if (matches.length > 1) fail('--entry basename is ambiguous: ' + ENTRY + '; choose a project-relative path: '
      + matches.map(file => toPosix(relative(INPUT_ROOT, file))).join(', '))
    if (matches.length === 1) ENTRY_PATH = matches[0]
  }
  if (!ENTRY_PATH) {
    ENTRY_PATH = [projectPath, rootPath].find(file => file.endsWith('.ks') && existsSync(file) && statSync(file).isFile())
    if (!ENTRY_PATH) fail('--entry scene not found: ' + ENTRY)
    // An explicit scene outside a selected file list still receives the same
    // declaration, capability and source-identity checks as every other scene.
    KAGS.push(ENTRY_PATH)
    KAGS_DISPLAY.push(toPosix(ENTRY_PATH))
  }
}
if (KAGS.length === 0) fail('no .ks scenes found in input.')
ENTRY_PATH ||= includedPath(join(INPUT_ROOT, 'story.ks')) || KAGS[0]
const ENTRY_INDEX = KAGS.indexOf(ENTRY_PATH)
pkg('input: ' + KAGS.length + ' scene(s) -> ' + KAGS_DISPLAY.join(' '))

const FIRST = KAGS[0]
let GAME_NAME = basename(statSync(firstInput).isDirectory() ? firstInput : dirname(FIRST))
if (GAME_NAME === '.' || GAME_NAME === '') GAME_NAME = basename(FIRST, '.ks')
if (!OUT) OUT = 'dist/' + GAME_NAME
const FINAL_OUT = canonicalOutputPath(p2r(OUT))
const FINAL_ZIP = ZIP_ARCHIVE ? canonicalOutputPath(p2r(ZIP_ARCHIVE)) : null
let OUT_PATH = FINAL_OUT
// t186 A2: Node fs.rmSync has NO '..' refusal — it deletes exactly what the
// resolved path names (verified: rmSync on '../x' removes the sibling dir,
// unlike GNU coreutils rm which refuses; the old .sh leaned on that coreutils
// refusal + set -e). Guard so a CLI --out that escapes the repo root cannot
// recursively delete anything outside it. Allowed: any path strictly inside
// ROOT (e.g. dist/<game>). Denied: ROOT itself, '..' traversal, absolute
// paths outside ROOT.
{
  const rel = relative(realpathSync(ROOT), OUT_PATH)
  if (rel === '' || rel.startsWith('..') || isAbsolute(rel)) {
    fail('--out must stay inside the repo root (' + ROOT + '); resolved OUT_PATH=' + OUT_PATH + ' — allowed form: dist/<game> or another repo-relative subdirectory')
  }
}

const STAGE = mkdtempSync(join(tmpdir(), 'caesura-pkg-'))
process.once('exit', () => { try { rmSync(STAGE, { recursive: true, force: true }) } catch { /* owned temp cleanup */ } })
const sha256 = file => createHash('sha256').update(readFileSync(file)).digest('hex')
const OUTPUT_LEDGER = '.caesura-output.json'
const ownedStages = []
process.once('exit', () => {
  for (const directory of ownedStages) {
    try { rmSync(directory, { recursive: true, force: true }) } catch { /* exclusive scratch only */ }
  }
})
function noOutputLinks(path) {
  for (let current = resolve(path);; current = dirname(current)) {
    try {
      if (lstatSync(current).isSymbolicLink()) throw new Error('refusing linked output path: ' + current)
    } catch (error) { if (error.code !== 'ENOENT') throw error }
    if (dirname(current) === current) break
  }
}
function outputTree(directory, omitLedger = false) {
  // Every filesystem name is data, including Object.prototype property names.
  const files = Object.create(null), directories = []
  function walk(current) {
    for (const name of readdirSync(current).sort()) {
      const path = join(current, name), value = lstatSync(path), key = toPosix(relative(directory, path))
      if (value.isSymbolicLink()) throw new Error('refusing linked output entry: ' + path)
      if (value.isDirectory()) { directories.push(key); walk(path) }
      else if (value.isFile()) { if (!(omitLedger && key === OUTPUT_LEDGER)) files[key] = sha256(path) }
      else throw new Error('refusing non-regular output entry: ' + path)
    }
  }
  walk(directory)
  return { files, directories: directories.sort() }
}
function outputState(path) {
  noOutputLinks(path)
  if (!existsSync(path)) return null
  if (statSync(path).isDirectory()) return outputTree(path)
  if (statSync(path).isFile()) return sha256(path)
  throw new Error('refusing non-regular output: ' + path)
}
const sameState = (a, b) => JSON.stringify(a) === JSON.stringify(b)
function outputExists(path) {
  try { lstatSync(path); return true }
  catch (error) { if (error.code === 'ENOENT') return false; throw error }
}
function prepareOutput(directory) {
  const state = outputState(directory)
  if (state === null || sameState(state, { files: {}, directories: [] })) return state
  if (!statSync(directory).isDirectory()) throw new Error('output is not a directory: ' + directory)
  let ledger
  try { ledger = JSON.parse(readFileSync(join(directory, OUTPUT_LEDGER), 'utf8')) }
  catch { throw new Error('refusing to replace a non-empty output without verifiable Caesura content ownership: ' + directory) }
  if (!sameState(ledger, { kind: 'caesura-web-output', schema: 1, ...outputTree(directory, true) })) {
    throw new Error('refusing to replace modified output (added files, saves, or changed generated content): ' + directory)
  }
  return state
}
const zipReceipt = path => join(dirname(path), '.' + basename(path) + '.caesura.json')
function prepareArchive(path) {
  const state = outputState(path), receiptState = outputState(zipReceipt(path))
  if (state === null && receiptState === null) return [state, receiptState]
  try {
    const ledger = JSON.parse(readFileSync(zipReceipt(path), 'utf8'))
    if (typeof state !== 'string' || !sameState(ledger,
      { kind: 'caesura-archive', schema: 1, file: basename(path), sha256: state })) throw new Error('changed archive')
  } catch { throw new Error('refusing to replace unowned or modified archive: ' + path) }
  return [state, receiptState]
}
function privateStage(destination) {
  // Keep incomplete work outside the delivery parent, even after a hard kill.
  const parent = dirname(dirname(destination))
  noOutputLinks(destination)
  mkdirSync(parent, { recursive: true })
  const directory = mkdtempSync(join(parent, '.' + basename(destination) + '.staging-'))
  ownedStages.push(directory)
  return directory
}
function publishOutputs(items) {
  // A set of directory/file renames is not power-loss atomic. Preserve each
  // old object under a unique sibling name until all promotions have succeeded.
  const backups = [], promoted = [], newStates = items.map(([source]) => outputState(source))
  try {
    for (const [, destination, expected] of items) {
      if (!sameState(outputState(destination), expected)) throw new Error('output changed during packaging: ' + destination)
    }
    for (const [index, [source, destination, expected]] of items.entries()) {
      mkdirSync(dirname(destination), { recursive: true })
      if (expected !== null) {
        const container = mkdtempSync(join(dirname(destination), '.' + basename(destination) + '.previous-'))
        const backup = join(container, 'previous')
        try { renameSync(destination, backup) }
        catch (error) { rmdirSync(container); throw error }
        backups.push([destination, backup, expected])
        if (!sameState(outputState(backup), expected)) throw new Error('output changed while acquiring replacement: ' + destination)
      } else if (outputExists(destination)) throw new Error('output appeared during packaging: ' + destination)
      renameSync(source, destination)
      promoted.push([index, source, destination])
    }
    for (const [index, [, destination]] of items.entries()) {
      if (!sameState(outputState(destination), newStates[index])) throw new Error('published output changed before completion: ' + destination)
    }
  } catch (error) {
    for (const [index, source, destination] of promoted.reverse()) {
      try { if (sameState(outputState(destination), newStates[index])) renameSync(destination, source) }
      catch { /* preserve both names for recovery */ }
    }
    for (const [destination, backup] of backups.reverse()) {
      try {
        if (!outputExists(destination)) { renameSync(backup, destination); rmdirSync(dirname(backup)); continue }
      } catch { /* never erase a backup on failed rollback */ }
      console.error('[package] previous output retained for recovery: ' + backup)
    }
    throw error
  }
  for (const [, backup, expected] of backups) {
    try {
      if (!sameState(outputState(backup), expected)) throw new Error('previous output changed')
      rmSync(backup, { recursive: true }); rmdirSync(dirname(backup))
    } catch { console.error('[package] previous output retained for recovery: ' + backup) }
  }
}
function metadataFor(scene) {
  let directory = dirname(scene)
  for (;;) {
    const metadata = join(directory, 'caesura.project.json')
    if (existsSync(metadata)) return metadata
    const parent = dirname(directory)
    if (parent === directory) return null
    directory = parent
  }
}
const declarations = new Set(KAGS.map(metadataFor))
if (declarations.size > 1) fail('input scenes use different project declarations; package each project separately')
let PROJECT_METADATA = [...declarations][0]
const projectRoot = PROJECT_METADATA ? dirname(PROJECT_METADATA) : INPUT_ROOT
const authorAssets = ASSET_SRC ? p2r(ASSET_SRC) : join(projectRoot, 'assets')
if (ASSET_SRC && (!existsSync(authorAssets) || !statSync(authorAssets).isDirectory())) {
  fail('--assets source is not a directory: ' + ASSET_SRC)
}
const ASSET_ROOTS = []
for (const source of [join(ROOT, 'assets'), authorAssets]) {
  if (!existsSync(source)) continue
  if (!statSync(source).isDirectory()) fail('asset source is not a directory: ' + source)
  if (!ASSET_ROOTS.some(previous => pathIdentity(previous) === pathIdentity(source))) ASSET_ROOTS.push(source)
}
if (!PROJECT_METADATA) {
  PROJECT_METADATA = join(STAGE, 'legacy-project.json')
  writeFileSync(PROJECT_METADATA, '{}\n')
}
const CHECKED_METADATA = sha256(PROJECT_METADATA)
const CHECKED_SCENES = KAGS.map(sha256)
const PROFILE_PATH = join(STAGE, 'profile.json')
const REPORT_PATH = join(STAGE, 'capabilities.json')
const SOURCE_PROFILE = { schema: 1, target: 'web', platform: 'browser', scope: 'build',
  catalog_sha256: webCatalogSha256(), compiled: {} }
writeFileSync(PROFILE_PATH, JSON.stringify(SOURCE_PROFILE))
function inputsUnchanged() {
  if (sha256(PROJECT_METADATA) !== CHECKED_METADATA || KAGS.some((file, index) => sha256(file) !== CHECKED_SCENES[index])) {
    fail('project inputs changed after capability checking')
  }
  for (const input of POSITIONAL) {
    const directory = p2r(input)
    if (statSync(directory).isDirectory()) {
      const current = sceneFiles(directory).map(file => join(directory, file))
      if (current.some(file => !KAGS.includes(file))) fail('project scene set changed after capability checking')
    }
  }
}

// -------------------------------------------------- 2. ks_check (gate) ------
console.log()
pkg('Step 1/5: ks_check (contract gate)')
const checkArgs = [join(ROOT, 'scripts', 'ks_check.lua'), '--target', 'web', '--profile', PROFILE_PATH,
  '--project', PROJECT_METADATA, '--json-output', REPORT_PATH,
  ...(SKIP_CHECK ? ['--capabilities-only'] : []), ...KAGS_DISPLAY]
const checked = spawnSync(LUA_PATH, checkArgs, { cwd: ROOT, stdio: 'inherit' })
if (checked.status !== 0 || !existsSync(REPORT_PATH)) fail('required Web capabilities or scene contracts are not satisfied')
const CAPABILITY_REPORT = JSON.parse(readFileSync(REPORT_PATH, 'utf8'))
if (CAPABILITY_REPORT.passed !== true) fail('Web capability report did not pass')
inputsUnchanged()
pkg('ks_check: all scenes pass contracts')

// --------------------------------------------------- 3. ks_bake --web ------
console.log()
pkg('Step 2/5: ks_bake --web (story bundle)')
// The stage persists until process exit (former `.sh trap ... EXIT`): BUNDLE
// is consumed by the assemble step below, and process.exit paths run the
// handler too.
let BUNDLE = ''
let BAKED_SCENE_PATHS = []
let BUNDLE_SHA256 = ''
let ASSET_DEPENDENCIES
const DEPENDENCIES_PATH = join(STAGE, 'asset-dependencies.json')
try {
  const baked = [...KAGS_DISPLAY]
  BAKED_SCENE_PATHS = baked
  const rr = spawnSync(LUA_PATH, [join(ROOT, 'scripts', 'ks_bake.lua'), ...baked, '--web', STAGE], { cwd: ROOT, stdio: 'inherit' })
  if (rr.status !== 0) {
    pkg('FAIL: ks_bake web bundle failed')
    process.exit(1)
  }
  BUNDLE = join(STAGE, 'story.lua')
  // Derive the entry key using the real compiler's mapping, including duplicate
  // basenames. The player consumes this field; Lua table order is not a contract.
  const selected = spawnSync(LUA_PATH, ['-e', `
package.path='scripts/?.lua;scripts/?/init.lua;'..package.path
require('kag')
local compiler=require('kag.compiler')
local path=${luaLiteralValue(BUNDLE)}
local bundle=assert(loadfile(path,'t',{}))()
local keys=assert(compiler.bundleSceneKeys(${luaLiteralValue(BAKED_SCENE_PATHS)}))
assert(compiler.validateBundle(bundle,keys))
bundle.entry=assert(keys[${ENTRY_INDEX + 1}])
assert(bundle.scenes[bundle.entry],'selected-entry-not-in-bundle')
local json=require('capability_json')
local dependency_collector=require('kag.asset_dependencies')
local checked_file=assert(io.open(${luaLiteralValue(REPORT_PATH)},'rb'))
local checked=assert(json.decode(checked_file:read('*a')))
assert(checked_file:close())
bundle.asset_dependencies=dependency_collector.apply_capabilities(bundle.asset_dependencies,checked)
bundle.assets=bundle.asset_dependencies.static
local dependency_json=assert(json.encode(bundle.asset_dependencies))
local dependency_file=assert(io.open(${luaLiteralValue(DEPENDENCIES_PATH)},'wb'))
assert(dependency_file:write(dependency_json))
assert(dependency_file:close())
local file=assert(io.open(path,'wb'))
assert(file:write('return '..compiler.encode_lua_literal(bundle)..'\\n'))
assert(file:close())
`], { cwd: ROOT, encoding: 'utf8' })
  if (selected.status !== 0) fail('cannot set bundle entry: ' + (selected.stderr || selected.stdout || selected.error?.message || 'Lua failed'))
  ASSET_DEPENDENCIES = JSON.parse(readFileSync(DEPENDENCIES_PATH, 'utf8'))
  if (ASSET_DEPENDENCIES.schema !== 1 || !['static', 'dynamic', 'skipped', 'invalid'].every(key => Array.isArray(ASSET_DEPENDENCIES[key]))) {
    fail('invalid media dependency report')
  }
  if (ASSET_DEPENDENCIES.invalid.length) fail('invalid static media paths: ' + ASSET_DEPENDENCIES.invalid.map(item => item.path).join(', '))
  BUNDLE_SHA256 = sha256(BUNDLE)
} catch (e) {
  rmSync(STAGE, { recursive: true, force: true })
  throw e
}
// ---------------------------------------------------- 4. assemble -----------
console.log()
pkg('Step 3/5: assemble web player + runtime')
const WEB_DIST = join(ROOT, 'web', 'dist')
if (!NO_WEB_BUILD) {
  // Guard (round 5, shared-state coupling): vite closeBundle copies repo-root
  // cache/story into web/dist UNCONDITIONALLY (web/vite.config.js
  // RUNTIME_DIRS). When the demo bundle is missing -- cache/story/story.lua is
  // a gitignored generated artifact that several flows clean/rewrite -- the
  // rebuilt web/dist ships without story.lua and resources.test.js's
  // "dist exists but bundle missing = bad build must FAIL" sentinel fires by
  // design. Bake the demo bundle first; a bake failure is FATAL -- never
  // continue to produce a broken dist.
  const ROOT_STORY = join(ROOT, 'cache', 'story', 'story.lua')
  if (!existsSync(ROOT_STORY)) {
    pkg('  cache/story/story.lua missing -- baking demo bundle first')
    const br = spawnSync(LUA_PATH, [join(ROOT, 'scripts', 'ks_bake.lua'), '--dir', 'demo', '--web', 'cache/story'], { cwd: ROOT, stdio: 'inherit' })
    if (br.status !== 0) {
      pkg('FATAL: demo bundle bake failed (cache/story/story.lua); aborting instead of rebuilding web/dist without it')
      process.exit(1)
    }
    if (!existsSync(ROOT_STORY)) {
      pkg('FATAL: bake reported success but ' + ROOT_STORY + ' does not exist')
      process.exit(1)
    }
  }
  const hasViteWeb = existsSync(join(ROOT, 'web', 'node_modules', 'vite'))
  const hasViteRoot = existsSync(join(ROOT, 'node_modules', 'vite'))
  if (hasViteWeb || hasViteRoot) {
    pkg('  (re)building web player -> ' + WEB_DIST)
    const viteBin = existsSync(join(ROOT, 'web', 'node_modules', 'vite', 'bin', 'vite.js'))
      ? join(ROOT, 'web', 'node_modules', 'vite', 'bin', 'vite.js')
      : join(ROOT, 'node_modules', 'vite', 'bin', 'vite.js')
    const vr = spawnSync(process.execPath, [viteBin, 'build'], { cwd: join(ROOT, 'web'), encoding: 'utf8' })
    if (vr.status !== 0) {
      // t186 A1: the former .sh was set -e (abort on failure); silently
      // continuing risked packaging a stale web player. Align: print the
      // failure tail, then abort. --no-web-build never reaches this branch.
      const tail = ((vr.stdout || '') + (vr.stderr || '')).split('\n').filter(Boolean).slice(-20).join('\n')
      console.error('[package]   vite build FAILED (tail):\n' + tail)
      fail('vite build failed; aborting instead of packaging a stale web player')
    }
  } else {
    pkg('  node_modules missing — reusing existing ' + WEB_DIST)
    pkg('  (to rebuild: cd web && npm install && node_modules/.bin/vite build)')
  }
}
if (!existsSync(join(WEB_DIST, 'index.html'))) {
  console.log()
  pkg('FAIL: web player not built (missing ' + WEB_DIST + '/index.html).')
  pkg('  Build it once with:  (cd web && npm install && node_modules/.bin/vite build)')
  process.exit(1)
}
let BUILT_PROFILE
try {
  BUILT_PROFILE = verifyWebCapabilityProfile(WEB_DIST, JSON.parse(readFileSync(join(WEB_DIST, 'capabilities-build.json'), 'utf8')))
} catch (error) { fail(String(error.message || error)) }
inputsUnchanged()
// Complete every declaration/HTML check while the previous delivery still exists.
const projectDocument = JSON.parse(readFileSync(PROJECT_METADATA, 'utf8'))
const declarationJson = JSON.stringify(projectDocument.capabilities || {}).replace(/</g, '\\u003c')
const declarationScript = '<script>self.__CAESURA_PROJECT_CAPABILITIES__=' + declarationJson + ';</' + 'script>'
const playerHtml = readFileSync(join(WEB_DIST, 'index.html'), 'utf8')
if (!playerHtml.includes('</head>')) fail('built player has no metadata insertion point')
const configuredHtml = playerHtml.replace('</head>', declarationScript + '\n</head>')
let previousOutput, previousZip, previousReceipt
try {
  previousOutput = prepareOutput(FINAL_OUT)
  if (FINAL_ZIP) {
    const archiveRelative = relative(FINAL_OUT, FINAL_ZIP)
    const outputRelative = relative(FINAL_ZIP, FINAL_OUT)
    if ((!archiveRelative.startsWith('..') && !isAbsolute(archiveRelative)) ||
        (!outputRelative.startsWith('..') && !isAbsolute(outputRelative))) {
      throw new Error('--zip must not overlap the output directory')
    }
    ;[previousZip, previousReceipt] = prepareArchive(FINAL_ZIP)
  }
  OUT_PATH = join(privateStage(FINAL_OUT), 'game')
} catch (error) { fail(error.message) }

mkdirSync(join(OUT_PATH, 'cache', 'story'), { recursive: true })
mkdirSync(join(OUT_PATH, 'demo', GAME_NAME), { recursive: true })
mkdirSync(join(OUT_PATH, 'web-assets'), { recursive: true })
mkdirSync(join(OUT_PATH, 'scripts'), { recursive: true })
mkdirSync(join(OUT_PATH, 'assets'), { recursive: true })

writeFileSync(join(OUT_PATH, 'index.html'), configuredHtml)
if (existsSync(join(WEB_DIST, 'sw.js'))) copyFileSync(join(WEB_DIST, 'sw.js'), join(OUT_PATH, 'sw.js'))
if (existsSync(join(WEB_DIST, 'manifest.webmanifest'))) copyFileSync(join(WEB_DIST, 'manifest.webmanifest'), join(OUT_PATH, 'manifest.webmanifest'))
if (existsSync(join(WEB_DIST, 'web-assets'))) copyDirectorySync(join(WEB_DIST, 'web-assets'), join(OUT_PATH, 'web-assets'))
else pkg('WARN: ' + join(WEB_DIST, 'web-assets') + ' missing — packaged player may ship without wasm/chunks') // t186 NIT: loud WARN, skip semantics kept
if (existsSync(join(WEB_DIST, 'scripts'))) copyDirectorySync(join(WEB_DIST, 'scripts'), join(OUT_PATH, 'scripts'))

// Preserve the verified build's script index along with its matching Lua tree.

// prune dev-only artifacts from the packaged script tree
function pruneTree(dir) {
  if (!existsSync(dir)) return
  for (const ent of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, ent.name)
    if (ent.isDirectory()) {
      if (ent.name === '__pycache__') { rmSync(p, { recursive: true, force: true }); continue }
      pruneTree(p)
    } else if (ent.name.endsWith('.pyc')) {
      rmSync(p, { force: true })
    }
  }
}
pruneTree(join(OUT_PATH, 'scripts'))

// Shared template resources remain available. Author files are copied last,
// including same-name overrides, and always land under the package's assets/.
for (const source of ASSET_ROOTS) {
  copyDirectorySync(source, join(OUT_PATH, 'assets'))
  pkg('asset source: ' + toPosix(source) + ' -> assets/')
}
if (!ASSET_ROOTS.length) pkg('WARN: no shared or project assets found')

const missingMedia = []
for (const reference of ASSET_DEPENDENCIES.static) {
  const path = resolve(OUT_PATH, reference)
  const rel = relative(OUT_PATH, path)
  if (!rel || rel === '..' || rel.startsWith('..' + (process.platform === 'win32' ? '\\' : '/')) || isAbsolute(rel)) {
    fail('media dependency escapes the package: ' + reference)
  }
  if (!existsSync(path) || !statSync(path).isFile()) missingMedia.push(reference)
}
if (missingMedia.length) fail('missing required static media: ' + missingMedia.join(', '))
pkg('media dependencies: ' + ASSET_DEPENDENCIES.static.length + ' static; '
  + ASSET_DEPENDENCIES.dynamic.length + ' dynamic (not proven); '
  + ASSET_DEPENDENCIES.skipped.length + ' capability-skipped (not verified)')

copyFileSync(BUNDLE, join(OUT_PATH, 'cache', 'story', 'story.lua'))

// Validate the delivered copy with the delivered Lua modules. Staging success
// cannot bless a stale/damaged copy or a different packaged compiler contract.
const runtimeCheck = spawnSync(LUA_PATH, ['-e', `
package.path='scripts/?.lua;scripts/?/init.lua;'..package.path
require('kag')
local chunk,err=loadfile('cache/story/story.lua','t',{})
if not chunk then io.stderr:write('invalid-bundle-literal: '..tostring(err));os.exit(1) end
local decoded,bundle=pcall(chunk)
if not decoded then io.stderr:write('invalid-bundle-literal: '..tostring(bundle));os.exit(1) end
local compiler=require('kag.compiler')
local keys,key_error=compiler.bundleSceneKeys(${luaLiteralValue(BAKED_SCENE_PATHS)})
if not keys then io.stderr:write(tostring(key_error));os.exit(1) end
local compatible,reason=compiler.validateBundle(bundle,keys)
if not compatible then io.stderr:write(tostring(reason));os.exit(1) end
if type(bundle.entry)~='string' or bundle.entry~=keys[${ENTRY_INDEX + 1}] or bundle.scenes[bundle.entry]==nil then
  io.stderr:write('delivered-bundle-entry-mismatch');os.exit(1)
end
io.write('PACKAGE-SCENE-KEYS:',table.concat(keys,string.char(0)))
`], { cwd: OUT_PATH, encoding: 'utf8' })
function reportRuntimeCheckFailure() {
  console.error('[package] runtime verifier failed: ' +
    `lua=${LUA_PATH}; cwd=${OUT_PATH}; node=${process.version}; ` +
    `status=${runtimeCheck.status}; signal=${runtimeCheck.signal || 'none'}; ` +
    `error=${runtimeCheck.error?.message || 'none'}`)
}
if (runtimeCheck.status !== 0) {
  if (runtimeCheck.stdout) process.stdout.write(runtimeCheck.stdout)
  if (runtimeCheck.stderr) process.stderr.write(runtimeCheck.stderr)
  reportRuntimeCheckFailure()
  pkg('FATAL: delivered bundle/runtime compatibility failed')
  process.exit(1)
}
const keyPrefix = 'PACKAGE-SCENE-KEYS:'
if (!runtimeCheck.stdout.startsWith(keyPrefix)) {
  if (runtimeCheck.stdout) process.stdout.write(runtimeCheck.stdout)
  if (runtimeCheck.stderr) process.stderr.write(runtimeCheck.stderr)
  reportRuntimeCheckFailure()
  fail('packaged runtime did not return scene keys')
}
const sceneKeys = runtimeCheck.stdout.slice(keyPrefix.length).split('\0')
if (sceneKeys.length !== BAKED_SCENE_PATHS.length) fail('packaged runtime returned incomplete scene keys')
if (sha256(join(OUT_PATH, 'cache/story/story.lua')) !== BUNDLE_SHA256) fail('copied story bundle differs from checked entry bundle')
for (const [index, key] of sceneKeys.entries()) {
  // Consume the same runtime-derived mapping for the editable source copies.
  // Never flatten two distinct story.ks inputs onto the same destination.
  if (!key || isAbsolute(key) || key.includes(':') || key.split(/[\\/]/).some(p => p === '..' || p === '.')) {
    fail('packaged runtime returned an unsafe scene key')
  }
  const destination = join(OUT_PATH, 'demo', GAME_NAME, key)
  mkdirSync(dirname(destination), { recursive: true })
  copyFileSync(p2r(BAKED_SCENE_PATHS[index]), destination)
  const checkedIndex = KAGS_DISPLAY.indexOf(BAKED_SCENE_PATHS[index])
  if (checkedIndex < 0 || sha256(destination) !== CHECKED_SCENES[checkedIndex]) fail('copied scene differs from capability-checked input')
}
pkg('delivered bundle matches packaged runtime')
const packagedNames = new Map(BAKED_SCENE_PATHS.map((file, index) => [file.replace(/\\/g, '/'), sceneKeys[index]]))
function packageLocations(value) {
  if (Array.isArray(value)) { value.forEach(packageLocations); return }
  if (!value || typeof value !== 'object') return
  if (typeof value.scene === 'string') {
    const key = value.scene.replace(/\\/g, '/')
    value.scene = packagedNames.get(key) || (isAbsolute(value.scene) ? 'project' : value.scene)
  }
  Object.values(value).forEach(packageLocations)
}
packageLocations(CAPABILITY_REPORT)
CAPABILITY_REPORT.profile = createPackagedWebCapabilityProfile(OUT_PATH, BUILT_PROFILE, { indexHtml: configuredHtml })
CAPABILITY_REPORT.checked_inputs = Object.fromEntries(BAKED_SCENE_PATHS.map((file, index) =>
  [sceneKeys[index], CHECKED_SCENES[KAGS_DISPLAY.indexOf(file)]]))
writeFileSync(join(OUT_PATH, 'capabilities-build.json'), JSON.stringify(CAPABILITY_REPORT.profile, null, 2) + '\n')
writeFileSync(join(OUT_PATH, 'CAPABILITIES.json'), JSON.stringify(CAPABILITY_REPORT, null, 2) + '\n')

// ---------------------------------------------------- 5. manifest -----------
console.log()
pkg('Step 4/5: manifest')
const allFiles = []
function collect(dir) {
  for (const ent of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, ent.name)
    if (ent.isDirectory()) collect(p)
    else allFiles.push(p)
  }
}
collect(OUT_PATH)
allFiles.sort((a, b) => {
  const ra = toPosix(relative(OUT_PATH, a))
  const rb = toPosix(relative(OUT_PATH, b))
  return ra < rb ? -1 : ra > rb ? 1 : 0
})
let totalBytes = 0
const manifestLines = []
manifestLines.push('Caesura (AmeKAG) web package: ' + GAME_NAME)
manifestLines.push('built: ' + new Date().toISOString()
  .replace('T', 'T').slice(0, 19) + 'Z')
manifestLines.push('scenes: ' + KAGS.length)
manifestLines.push('entry scene: ' + sceneKeys[ENTRY_INDEX])
manifestLines.push('static media dependencies: ' + ASSET_DEPENDENCIES.static.length)
manifestLines.push('dynamic media dependencies (not proven): ' + ASSET_DEPENDENCIES.dynamic.length)
manifestLines.push('capability-skipped media dependencies (not verified): ' + ASSET_DEPENDENCIES.skipped.length)
manifestLines.push('---')
manifestLines.push('files (size bytes, path):')
for (const f of allFiles) {
  const st = statSync(f)
  totalBytes += st.size
  manifestLines.push(String(st.size) + '\t' + toPosix(relative(OUT_PATH, f)))
}
manifestLines.push('---')
manifestLines.push('total KB: ' + Math.floor(totalBytes / 1024))
writeFileSync(join(OUT_PATH, 'MANIFEST.txt'), manifestLines.join('\n') + '\n', 'utf8')
writeFileSync(join(OUT_PATH, OUTPUT_LEDGER), JSON.stringify({
  kind: 'caesura-web-output', schema: 1, ...outputTree(OUT_PATH, true),
}, null, 2) + '\n')
prepareOutput(OUT_PATH)

// ---------------------------------------------------- 6. zip archive ---------
const ZIP_CODE = [
  'import os, zipfile, sys, hashlib, json',
  'out_dir = sys.argv[1]',
  'zip_path = sys.argv[2]',
  "with zipfile.ZipFile(zip_path, 'x', zipfile.ZIP_DEFLATED) as zf:",
  '    for root, dirs, files in os.walk(out_dir):',
  '        for f in files:',
  '            full_p = os.path.join(root, f)',
  '            rel_p = os.path.relpath(full_p, out_dir)',
  '            zf.write(full_p, rel_p)',
  'with zipfile.ZipFile(zip_path) as zf:',
  '    if zf.testzip() is not None: raise ValueError("archive verification failed")',
  '    for root, dirs, files in os.walk(out_dir):',
  '        for name in files:',
  '            full_p = os.path.join(root, name)',
  '            rel_p = os.path.relpath(full_p, out_dir).replace("\\\\", "/")',
  '            with open(full_p, "rb") as source:',
  '                if hashlib.sha256(zf.read(rel_p)).digest() != hashlib.sha256(source.read()).digest():',
  '                    raise ValueError("archive content differs: " + rel_p)',
].join('\n')
const promotions = []
if (ZIP_ARCHIVE) {
  console.log()
  pkg('Step 5/5: archive -> ' + ZIP_ARCHIVE)
  const stagedZip = join(privateStage(FINAL_ZIP), basename(FINAL_ZIP))
  let zr
  try {
    zr = spawnSync('python', ['-c', ZIP_CODE, OUT_PATH, stagedZip], { stdio: 'inherit' })
  } catch (e) {
    pkg('FAIL: python not available for --zip: ' + e.message)
    process.exit(1)
  }
  if (zr.error) {
    try {
      zr = spawnSync('python3', ['-c', ZIP_CODE, OUT_PATH, stagedZip], { stdio: 'inherit' })
    } catch (e) {
      pkg('FAIL: python not available for --zip: ' + e.message)
      process.exit(1)
    }
  }
  if (!zr.error && zr.status !== 0) process.exit(1)
  if (zr.error) {
    pkg('FAIL: python not available for --zip: ' + zr.error.message)
    process.exit(1)
  }
  writeFileSync(zipReceipt(stagedZip), JSON.stringify({
    kind: 'caesura-archive', schema: 1, file: basename(FINAL_ZIP), sha256: sha256(stagedZip),
  }) + '\n')
  promotions.push([stagedZip, FINAL_ZIP, previousZip], [zipReceipt(stagedZip), zipReceipt(FINAL_ZIP), previousReceipt])
}
try {
  prepareOutput(OUT_PATH)
  promotions.push([OUT_PATH, FINAL_OUT, previousOutput])
  publishOutputs(promotions)
} catch (error) { fail(error.message) }

if (!DEFER_COMPLETE) {
  console.log()
  console.log('==================================================================')
  console.log('  PACKAGE COMPLETE -> ' + OUT)
  console.log('    scenes:  ' + KAGS.length + '  (pick one from the web scene dropdown)')
  console.log('    bundle:  ' + OUT + '/cache/story/story.lua')
  console.log('    assets:  ' + OUT + '/assets/')
  console.log('    entry:   ' + sceneKeys[ENTRY_INDEX])
  console.log('    manifest: ' + OUT + '/MANIFEST.txt')
  if (FINAL_ZIP) console.log('    archive: ' + ZIP_ARCHIVE + ' (' + statSync(FINAL_ZIP).size + ' bytes)')
  console.log('------------------------------------------------------------------')
  console.log('  Serve locally:  cd [' + OUT + '] && python -m http.server 8080')
  console.log('  Or upload to itch.io / Netlify / GitHub Pages / S3.')
  console.log('==================================================================')
}

// --------------------------------------------------- 7. --release -----------
if (RELEASE) {
  console.log()
  pkg('--release: desktop CPack handoff (see docs/guides/release-process.md)')
  console.log('    cmake --build build --config Release --parallel')
  console.log('    cd build && cpack -C Release -G ZIP && cd ..')
  console.log('    git tag -a vX.Y.Z -m [Caesura (AmeKAG) vX.Y.Z] && git push origin vX.Y.Z')
  console.log('    gh release create vX.Y.Z build/CaesuraAmeKAG-*-Windows-AMD64.zip --title [TITLE] --notes-file CHANGELOG.md --draft')
}

process.exit(0)

