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
//    --assets <dir>    asset root to ship (default: repo assets/ shared pool)
//    --no-web-build    reuse an existing web/dist; do not (re)build it
//    --release         also print the CPack desktop-Release handoff (docs only)
//    --entry <scene>   nominate the entry scene (recorded in the manifest)
//    --zip <path>      also write a ZIP archive of the package (python zipfile)
//
//  Exit: 0 = packaged, 1 = any step failed.
// ==============================================================================

import { existsSync, readdirSync, statSync, copyFileSync, cpSync,
         mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs'
import { spawnSync } from 'node:child_process'
import { luaLiteralValue } from '../web/lua-value.js'
import { join, resolve, dirname, basename, relative, isAbsolute } from 'node:path'
import { tmpdir } from 'node:os'
import { fileURLToPath } from 'node:url'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = resolve(HERE, '..')
const DEFAULT_INPUT = 'demo/example_game'
const pkg = (...a) => console.log('[package]', ...a)

// ----------------------------------------------- helpers ---------------------
function fail(msg) { console.error('[package] FATAL: ' + msg); process.exit(1) }
// Relative CLI paths are resolved against ROOT (the former script cd'd to it).
const p2r = (p) => (isAbsolute(p) ? p : resolve(ROOT, p))
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
  --assets <dir>    asset root to ship (default: repo assets/ shared pool)
  --no-web-build    reuse an existing web/dist; do not (re)build it
  --release         also print the CPack desktop-Release handoff (docs only)
  --entry <scene>   nominate the entry scene (recorded in the manifest)
  --zip <path>      also write a ZIP archive of the package (python zipfile)

Exit: 0 = packaged, 1 = any step failed.`)
}

// ------------------------------------------- Lua interpreter probe ----------
// Same three levels as scripts/caesura_build.py::find_lua (keep in sync):
//   packaged external/lua/lua[.exe] (release-package artifact; gitignored in a
//   checkout; Windows-only presence) -> build-tree lua_cli product
//   (build/lua/<config>/lua[.exe], present after cmake --build) -> PATH
//   lua5.4 / lua. FATAL only when ALL levels miss, listing every location
//   probed (honest diagnostics; never a silent skip).
function probeLua() {
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
let OUT = '', ASSET_SRC = 'assets', NO_WEB_BUILD = false, RELEASE = false,
    ENTRY = '', ZIP_ARCHIVE = ''
const POSITIONAL = []
for (let i = 0; i < argv.length; i++) {
  const a = argv[i]
  if (a === '--out') { OUT = argv[++i]; if (OUT === undefined) fail('--out requires a value') }
  else if (a === '--assets') { ASSET_SRC = argv[++i]; if (ASSET_SRC === undefined) fail('--assets requires a value') }
  else if (a === '--entry') { ENTRY = argv[++i]; if (ENTRY === undefined) fail('--entry requires a value') }
  else if (a === '--zip') { ZIP_ARCHIVE = argv[++i]; if (ZIP_ARCHIVE === undefined) fail('--zip requires a value') }
  else if (a === '--no-web-build') { NO_WEB_BUILD = true }
  else if (a === '--release') { RELEASE = true }
  else if (a === '-h' || a === '--help') { printHelp(); process.exit(0) }
  else if (a.startsWith('-')) fail('unknown option: ' + a)
  else POSITIONAL.push(a)
}

// ------------------------------------------------- 1. resolve input ----------
if (POSITIONAL.length === 0) POSITIONAL.push(DEFAULT_INPUT)
const KAGS = []          // absolute paths (for spawn/fs)
const KAGS_DISPLAY = []  // args as the .sh would echo them (relative, forward slash)
for (const p0 of POSITIONAL) {
  const p = p2r(p0)
  const p0f = p0.replace(/\\/g, '/')
  if (p0.endsWith('.ks') && existsSync(p) && statSync(p).isFile()) {
    KAGS.push(p)
    KAGS_DISPLAY.push(p0f)
  } else if (existsSync(p) && statSync(p).isDirectory()) {
    const found = readdirSync(p).filter((f) => f.endsWith('.ks')).sort()
    for (const f of found) { KAGS.push(join(p, f)); KAGS_DISPLAY.push(p0f + '/' + f) }
  } else {
    fail('not a game dir or .ks file: ' + p0)
  }
}
if (KAGS.length === 0) fail('no .ks scenes found in input.')
pkg('input: ' + KAGS.length + ' scene(s) -> ' + KAGS_DISPLAY.join(' '))

const FIRST = KAGS[0]
let GAME_NAME = basename(dirname(FIRST))
if (GAME_NAME === '.' || GAME_NAME === '') GAME_NAME = basename(FIRST, '.ks')
if (!OUT) OUT = 'dist/' + GAME_NAME
const OUT_PATH = p2r(OUT)
// t186 A2: Node fs.rmSync has NO '..' refusal — it deletes exactly what the
// resolved path names (verified: rmSync on '../x' removes the sibling dir,
// unlike GNU coreutils rm which refuses; the old .sh leaned on that coreutils
// refusal + set -e). Guard so a CLI --out that escapes the repo root cannot
// recursively delete anything outside it. Allowed: any path strictly inside
// ROOT (e.g. dist/<game>). Denied: ROOT itself, '..' traversal, absolute
// paths outside ROOT.
{
  const rel = relative(ROOT, OUT_PATH)
  if (rel === '' || rel.startsWith('..') || isAbsolute(rel)) {
    fail('--out must stay inside the repo root (' + ROOT + '); resolved OUT_PATH=' + OUT_PATH + ' — allowed form: dist/<game> or another repo-relative subdirectory')
  }
}

// -------------------------------------------------- 2. ks_check (gate) ------
console.log()
pkg('Step 1/5: ks_check (contract gate)')
for (const [i, k] of KAGS.entries()) {
  // Same call face as the .sh: display (relative) path + cwd ROOT, so
  // ks_check output and bundle-embedded scene keys stay byte-identical.
  const rr = spawnSync(LUA_PATH, [join(ROOT, 'scripts', 'ks_check.lua'), KAGS_DISPLAY[i]], { cwd: ROOT, stdio: 'inherit' })
  if (rr.status !== 0) {
    pkg('FAIL: contract check failed for ' + KAGS_DISPLAY[i])
    process.exit(1)
  }
}
pkg('ks_check: all scenes pass contracts')

// --------------------------------------------------- 3. ks_bake --web ------
console.log()
pkg('Step 2/5: ks_bake --web (story bundle)')
const STAGE = mkdtempSync(join(tmpdir(), 'caesura-pkg-'))
// The stage persists until process exit (former `.sh trap ... EXIT`): BUNDLE
// is consumed by the assemble step below, and process.exit paths run the
// handler too.
process.once('exit', () => { try { rmSync(STAGE, { recursive: true, force: true }) } catch { /* temp cleanup */ } })
let BUNDLE = ''
let BAKED_SCENE_KEYS = []
try {
  let baked = [...KAGS_DISPLAY]
  if (ENTRY) {
    let es = null
    for (const k of KAGS_DISPLAY) {
      if (basename(k) === ENTRY || k === ENTRY) { es = k; break }
    }
    if (es === null) {
      es = ENTRY
      if (!existsSync(p2r(ENTRY))) { pkg('FAIL: --entry scene not found: ' + ENTRY); process.exit(1) }
      baked = [es, ...baked]
    }
  }
  BAKED_SCENE_KEYS = baked.map(path => basename(path))
  const rr = spawnSync(LUA_PATH, [join(ROOT, 'scripts', 'ks_bake.lua'), ...baked, '--web', STAGE], { cwd: ROOT, stdio: 'inherit' })
  if (rr.status !== 0) {
    pkg('FAIL: ks_bake web bundle failed')
    process.exit(1)
  }
  BUNDLE = join(STAGE, 'story.lua')
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

rmSync(OUT_PATH, { recursive: true, force: true })
mkdirSync(join(OUT_PATH, 'cache', 'story'), { recursive: true })
mkdirSync(join(OUT_PATH, 'demo', GAME_NAME), { recursive: true })
mkdirSync(join(OUT_PATH, 'web-assets'), { recursive: true })
mkdirSync(join(OUT_PATH, 'scripts'), { recursive: true })
mkdirSync(join(OUT_PATH, ASSET_SRC), { recursive: true })

copyFileSync(join(WEB_DIST, 'index.html'), join(OUT_PATH, 'index.html'))
if (existsSync(join(WEB_DIST, 'sw.js'))) copyFileSync(join(WEB_DIST, 'sw.js'), join(OUT_PATH, 'sw.js'))
else if (existsSync(join(ROOT, 'web', 'sw.js'))) copyFileSync(join(ROOT, 'web', 'sw.js'), join(OUT_PATH, 'sw.js'))
if (existsSync(join(WEB_DIST, 'manifest.webmanifest'))) copyFileSync(join(WEB_DIST, 'manifest.webmanifest'), join(OUT_PATH, 'manifest.webmanifest'))
else if (existsSync(join(ROOT, 'web', 'manifest.webmanifest'))) copyFileSync(join(ROOT, 'web', 'manifest.webmanifest'), join(OUT_PATH, 'manifest.webmanifest'))
if (existsSync(join(WEB_DIST, 'web-assets'))) cpSync(join(WEB_DIST, 'web-assets'), join(OUT_PATH, 'web-assets'), { recursive: true })
else pkg('WARN: ' + join(WEB_DIST, 'web-assets') + ' missing — packaged player may ship without wasm/chunks') // t186 NIT: loud WARN, skip semantics kept
if (existsSync(join(WEB_DIST, 'scripts'))) cpSync(join(WEB_DIST, 'scripts'), join(OUT_PATH, 'scripts'), { recursive: true })

// The web player bridge.js fetches scriptsBase + index.json -- regenerate it
// for the packaged script tree so a packaged game boots without manual
// bundle edits (Validation-Release task book §9).
if (existsSync(join(ROOT, 'web', 'gen-index.mjs'))) {
  const gr = spawnSync(process.execPath,
    [join(ROOT, 'web', 'gen-index.mjs'), join(OUT_PATH, 'scripts'), join(OUT_PATH, 'scripts', 'index.json')],
    { stdio: 'ignore' })
  if (gr.status !== 0) pkg('WARN: scripts index.json generation failed')
}

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

const ASSET_PATH = p2r(ASSET_SRC)
if (existsSync(ASSET_PATH)) {
  cpSync(ASSET_PATH, join(OUT_PATH, ASSET_SRC), { recursive: true })
} else {
  pkg('WARN: asset root [' + ASSET_SRC + '] not found — shipping without game assets')
}

for (const k of KAGS) {
  copyFileSync(k, join(OUT_PATH, 'demo', GAME_NAME, basename(k)))
}

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
local compatible,reason=require('kag.compiler').validateBundle(bundle,${luaLiteralValue(BAKED_SCENE_KEYS)})
if not compatible then io.stderr:write(tostring(reason));os.exit(1) end
print('[package] delivered bundle matches packaged runtime')
`], { cwd: OUT_PATH, stdio: 'inherit' })
if (runtimeCheck.status !== 0) {
  pkg('FATAL: delivered bundle/runtime compatibility failed')
  process.exit(1)
}

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
if (ENTRY) manifestLines.push('entry scene: ' + ENTRY)
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

console.log()
console.log('==================================================================')
console.log('  PACKAGE COMPLETE -> ' + OUT)
console.log('    scenes:  ' + KAGS.length + '  (pick one from the web scene dropdown)')
console.log('    bundle:  ' + OUT + '/cache/story/story.lua')
console.log('    assets:  ' + OUT + '/' + ASSET_SRC + '/')
if (ENTRY) console.log('    entry:   ' + ENTRY)
console.log('    manifest: ' + OUT + '/MANIFEST.txt')
console.log('------------------------------------------------------------------')
console.log('  Serve locally:  cd [' + OUT + '] && python -m http.server 8080')
console.log('  Or upload to itch.io / Netlify / GitHub Pages / S3.')
console.log('==================================================================')

// ---------------------------------------------------- 6. zip archive ---------
const ZIP_CODE = [
  'import os, zipfile, sys',
  'out_dir = sys.argv[1]',
  'zip_path = sys.argv[2]',
  "with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as zf:",
  '    for root, dirs, files in os.walk(out_dir):',
  '        for f in files:',
  '            full_p = os.path.join(root, f)',
  '            rel_p = os.path.relpath(full_p, out_dir)',
  '            zf.write(full_p, rel_p)',
  "print(f'  Created zip archive: {zip_path} ({os.path.getsize(zip_path)} bytes)')",
].join('\n')
if (ZIP_ARCHIVE) {
  console.log()
  pkg('Step 5/5: archive -> ' + ZIP_ARCHIVE)
  mkdirSync(dirname(p2r(ZIP_ARCHIVE)), { recursive: true })
  let zr
  try {
    zr = spawnSync('python', ['-c', ZIP_CODE, OUT_PATH, p2r(ZIP_ARCHIVE)], { stdio: 'inherit' })
  } catch (e) {
    pkg('FAIL: python not available for --zip: ' + e.message)
    process.exit(1)
  }
  if (zr.error) {
    try {
      zr = spawnSync('python3', ['-c', ZIP_CODE, OUT_PATH, p2r(ZIP_ARCHIVE)], { stdio: 'inherit' })
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

