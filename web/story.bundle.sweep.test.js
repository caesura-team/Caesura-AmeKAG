// @vitest-environment jsdom
// Story-bundle consistency sweep for the WEB player.
//
// Chain under test (ks_bake -> bundle -> play):
//   scripts/ks_bake.lua --dir demo --web cache/story  emits cache/story/story.lua
//   (a Lua-literal bundle { version, scenes = { <key> = serialized }, assets }).
//   main.mjs loads it (loadStoryBundle) and main.runScene/advance call
//   bridge.runFromBundle when the requested scene key is present.
//
// This file makes the "bundle == source" guarantee explicit:
//   1. bundle shape: version=1 and every .ks under demo/ has a scene key
//      (incl. tutorial_14_flow_timing.ks / tutorial_15_expr_deep.ks);
//   2. formal sweep: EVERY bundled scene is driven runFromBundle -> DONE with
//      zero error events (round 90's zz_sweep was a probe; this is the real one);
//   3. bundle-vs-source parity: 3 scenes run through runScene (raw .ks source
//      path) and runFromBundle (bundle path) must converge on the SAME backlog;
//   4. a missing scene key returns 'ERR:scene-not-in-bundle:<key>'.
//
// cache/story is gitignored (generated locally). On CI or a fresh clone the
// file may be absent: the whole suite is skipped via describe.skipIf so a
// missing bundle never fails the run (web tests are not part of CI anyway).
import { describe, it, expect, beforeAll } from 'vitest'
import { readFileSync, existsSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { dirname, join } from 'node:path'
import { createPlayer } from './bridge.js'

const here = dirname(fileURLToPath(import.meta.url))
const rootDir = join(here, '..')
const scriptsDir = join(rootDir, 'scripts')
const assetsDir = join(rootDir, 'assets')
const index = JSON.parse(readFileSync(join(here, 'scripts-index.json'), 'utf8'))
const storyPath = join(rootDir, 'cache', 'story', 'story.lua')
// Set at module scope so describe.skipIf can branch on it before beforeAll.
const bundleExists = existsSync(storyPath)

// In-memory file fetch: serve real scripts and assets/lang dictionaries so
// the bridge (wasmoon) can boot offline just like flow.integration.test.js.
const fileFetch = async (url) => {
  const u = new URL(url)
  if (u.pathname.startsWith('/assets/lang/')) {
    const rel = u.pathname.replace('/assets/lang/', '')
    const p = join(assetsDir, 'lang', ...rel.split('/'))
    return {
      ok: existsSync(p),
      status: existsSync(p) ? 200 : 404,
      text: async () => (existsSync(p) ? readFileSync(p, 'utf8') : ''),
      json: async () => index,
    }
  }
  const rel = u.pathname.replace('/scripts/', '')
  const p = join(scriptsDir, ...rel.split('/'))
  const ok = existsSync(p)
  return { ok, status: ok ? 200 : 404, text: async () => (ok ? readFileSync(p, 'utf8') : ''), json: async () => index }
}

// The complete scene-key set the bundle is expected to carry. It mirrors the
// .ks files live under demo/ at the time of baking (ks_bake --dir demo
// recurses top-level + demo/tutorial/ + demo/example_game/). Keeping it
// explicit here means the sweep FAILS loudly the moment a demo scene is added
// but the bundle is not regenerated — exactly the consistency gap this suite
// guards. Update this list in lockstep with demo/ when scene files change.
const EXPECTED_KEYS = [
  // Round 94: full_pipeline_demo / galgame_demo were excluded while the
  // bundle-path [save]/[load] self-referential deadlock was open; round 95
  // fixed the load-resume cursor in bridge.js, so they are back in the sweep.
  'full_pipeline_demo.ks',
  'galgame_demo.ks',
  'showcase.ks',
  'sma_demo.ks',
  'story.ks',
  'tutorial_01_hello.ks',
  'tutorial_02_text.ks',
  'tutorial_03_layers.ks',
  'tutorial_04_audio.ks',
  'tutorial_05_branching.ks',
  'tutorial_06_effects.ks',
  // Round 94: tutorial_07_saveload was excluded for the same bundle [load]
  // resume deadlock; restored in round 95 with the bridge fix.
  'tutorial_07_saveload.ks',
  'tutorial_08_system_ui.ks',
  'tutorial_09_interpolation.ks',
  'tutorial_10_loops.ks',
  'tutorial_11_switch.ks',
  'tutorial_12_expr_combo.ks',
  'tutorial_13_commands.ks',
  'tutorial_14_flow_timing.ks',
  'tutorial_15_expr_deep.ks',
]

// Resolve a scene key back to its raw .ks source on disk (top-level demo/,
// demo/tutorial/ or demo/example_game/ — wherever it actually lives).
function sourceFor(key) {
  for (const dir of ['', 'tutorial/', 'example_game/']) {
    const p = join(rootDir, 'demo', dir, key)
    if (existsSync(p)) return readFileSync(p, 'utf8')
  }
  return null
}

// A deterministic text signature of the committed VN history (backlog) —
// what the DOM history screen would show. Used to prove bundle-vs-source
// parity on a per-page basis, ignoring the non-deterministic draw metadata.
const backlogSignature = (core) =>
  core.backlog.map((b) => (typeof b.text === 'string' ? b.text : '')).join('\n')

describe.skipIf(!bundleExists)('story bundle sweep (ks_bake -> bundle -> play)', () => {
  let player = null
  let bundle = null

  beforeAll(async () => {
    player = await createPlayer({
      scriptsBase: 'http://local/scripts/',
      fetchImpl: fileFetch,
      langBase: 'http://local/assets/lang/',
      wasmFile: join(here, 'node_modules', 'wasmoon', 'dist', 'glue.wasm'),
    })
    // Load the same bundle main.mjs loadStoryBundle() produces at boot.
    const story = readFileSync(storyPath, 'utf8')
    player.lua.global.set('STORY_SRC', story)
    const SQ = String.fromCharCode(39)
    const code = '  local chunk = assert(load(STORY_SRC, ' + SQ + '@story.lua' + SQ + ', ' + SQ + 't' + SQ + ', _ENV))'
      + String.fromCharCode(10) + '  return chunk()'
    bundle = await player.lua.doString(code)
  })

  it('bundle loads: version=1 and all demo scene keys are present', () => {
    expect(bundle, 'cache/story/story.lua should load (suite was not skipped)').toBeTruthy()
    expect(bundle.version).toBe(1)
    expect(bundle.scenes).toBeTruthy()
    // Container version remains 1; compiled scene format is independently 2.
    for (const key of EXPECTED_KEYS) {
      expect(bundle.scenes[key], 'missing bundle scene: ' + key).toBeTruthy()
      expect(bundle.scenes[key].version, key + ' should use compiled scene format 2').toBe(2)
    }
    // the required tutorial 14/15 scenes are explicitly present
    expect(bundle.scenes['tutorial_14_flow_timing.ks']).toBeTruthy()
    expect(bundle.scenes['tutorial_15_expr_deep.ks']).toBeTruthy()
    // bundle must not have fewer keys than the expected set (no stale bundle)
    expect(Object.keys(bundle.scenes).length).toBeGreaterThanOrEqual(EXPECTED_KEYS.length)
    expect(bundle.assets).toBeTruthy()
  }, 60000)

  // ---- formal full-scene sweep (round 90 probe upgraded to a real guard) ----
  it.each(EXPECTED_KEYS)('bundle sweep: %s runs to DONE with zero error events', async (key) => {
    player.core.events.length = 0
    player.core.backlog.length = 0
    const out = await player.runFromBundle(bundle, key, { maxFrames: 300000, autoClick: true })
    expect(out.startsWith('DONE:'), key + ' should complete via bundle: ' + out).toBe(true)
    // round 89 regression: a bundle scene must not short-circuit to DONE:1:0
    // (wasmoon userdata-proxy deserialize failure). Guard the "actually ran"
    // signal so a silent bake failure re-surfaces.
    expect(out, key + ' must not short-circuit (DONE:1:0)').not.toBe('DONE:1:0')
    const errs = player.core.events.filter((e) => String(e.kind).includes('error'))
    expect(errs, key + ' should have no error events').toEqual([])
    // a real scene produces at least one committed backlog page
    expect(player.core.backlog.length, key + ' should commit backlog pages').toBeGreaterThan(0)
  }, 300000)

  // ---- bundle-vs-source parity: same scene, both paths, same backlog ----
  it.each([
    ['tutorial_06_effects.ks'],
    ['tutorial_14_flow_timing.ks'],
    ['tutorial_15_expr_deep.ks'],
  ])('bundle backlog matches source-path backlog for %s', async (key) => {
    const src = sourceFor(key)
    expect(src, 'source .ks should exist for ' + key).toBeTruthy()
    // drive the RAW source path to completion
    player.core.events.length = 0
    player.core.backlog.length = 0
    const srcOut = await player.runScene(src, key, { maxFrames: 300000, autoClick: true })
    expect(srcOut.startsWith('DONE:'), key + ' source should complete: ' + srcOut).toBe(true)
    const srcSig = backlogSignature(player.core)

    // drive the BUNDLE path to completion
    player.core.events.length = 0
    player.core.backlog.length = 0
    const bndOut = await player.runFromBundle(bundle, key, { maxFrames: 300000, autoClick: true })
    expect(bndOut.startsWith('DONE:'), key + ' bundle should complete: ' + bndOut).toBe(true)
    const bndSig = backlogSignature(player.core)

    // the rendered story text must be identical — bundle deserialize +
    // Lua-literal bridge must not alter how commands produce [p] pages
    expect(bndSig, key + ' bundle backlog should equal source backlog').toBe(srcSig)
  }, 300000)

  it('a missing scene key returns ERR:scene-not-in-bundle', async () => {
    const out = await player.runFromBundle(bundle, 'no_such_scene.ks', { maxFrames: 5000, autoClick: true })
    expect(out).toBe('ERR:scene-not-in-bundle:no_such_scene.ks')
  }, 60000)
})

// When the bundle is absent (fresh clone / CI) the sweep is skipIf'd; surface
// a clear regenerating hint so a dev knows the guard is alive but unbeatable.
if (!bundleExists) {
  describe('story bundle sweep (skipped: bundle missing)', () => {
    it('reports the generation step (informational, does not fail)', () => {
      expect(true).toBe(true)
      // eslint-disable-next-line no-console
      // checkout-executable intepreter (build/lua/Debug/lua.exe); the released
      // package ships the interpreter at external/lua/lua.exe (gitignored).
      console.warn('[story.bundle.sweep] cache/story/story.lua not found — sweep skipped. '
        + 'Regenerate with: build/lua/Debug/lua.exe scripts/ks_bake.lua --dir demo --web cache/story')
    })
  })
}
