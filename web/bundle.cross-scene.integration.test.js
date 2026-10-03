// @vitest-environment jsdom
// Cross-scene [jump]/[call]/[return]/[advance]/[save]/[load]/i18n coverage for
// the WEB player's BUNDLE path (runFromBundle + __BUNDLE_SCENES lookup).
//
// This is the complement of flow.integration.test.js round-89/95 bundle
// regressions: those proved a single baked scene deserializes and executes and
// that the bundle path does not deadlock on self-referential [save]/[load].
// This file adds the CROSS-SCENE combinations that were only ever proven on
// the raw-source path (runScene + __SCENE_SOURCES):
//
//   1. bundle cross-scene [jump]  : A [jump scene_b.ks] -> B runs -> DONE
//   2. bundle [jump] fallback     : target absent from bundle but present in
//                                   sceneSources resolves via source parse
//   3. bundle [jump] missing      : [jump nonexistent.ks] (neither bundle nor
//                                   sources) -> WARN, no crash, execution
//                                   continues past the jump (locked behaviour)
//   4. bundle cross-scene [call]/[return] : A call B -> B return -> A resumes;
//                                   nested 3-scene call stack survives
//   5. bundle [p] park + advance across a jump : advance drives A park, the
//                                   jump to B fires, subsequent advance runs B
//   6. bundle cross-scene save/load : A saves (scene name matches bundle key),
//                                   a loader scene in B [load]s -> resumes A's
//                                   saved token via __BUNDLE_SCENES (round 47)
//   7. bundle cross-scene i18n   : [i18n language=en] then [jump B]; B's text
//                                   renders in en and i18n.current survives
//
// All scenes are BAKED (tokenizer.parse + compiler.compile + serialize) into
// a bundle {version=1, scenes={key=serialized}} exactly as ks_bake --web and
// runFromBundle expect, so every lookup goes through __BUNDLE_SCENES — not
// __SCENE_SOURCES. Bundled-scene resolution is the surface under test.
import { describe, it, expect, beforeAll } from 'vitest'
import { readFileSync, existsSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { dirname, join } from 'node:path'
import { createPlayer } from './bridge.js'
import { DomRenderer } from './dom-renderer.js'

const here = dirname(fileURLToPath(import.meta.url))
const rootDir = join(here, '..')
const scriptsDir = join(rootDir, 'scripts')
const assetsDir = join(rootDir, 'assets')
const index = JSON.parse(readFileSync(join(here, 'scripts-index.json'), 'utf8'))
const fileFetch = async (url) => {
  const u = new URL(url)
  if (u.pathname.startsWith('/assets/lang/')) {
    const rel = u.pathname.replace('/assets/lang/', '')
    const p = join(assetsDir, 'lang', ...rel.split('/'))
    return {
      ok: existsSync(p), status: existsSync(p) ? 200 : 404,
      text: async () => (existsSync(p) ? readFileSync(p, 'utf8') : ''),
      json: async () => index,
    }
  }
  const rel = u.pathname.replace('/scripts/', '')
  const p = join(scriptsDir, ...rel.split('/'))
  const ok = existsSync(p)
  return {
    ok,
    status: ok ? 200 : 404,
    text: async () => (ok ? readFileSync(p, 'utf8') : ''),
    json: async () => index,
  }
}

let player = null
let renderer = null
let stage = null

beforeAll(async () => {
  player = await createPlayer({
    scriptsBase: 'http://local/scripts/',
    fetchImpl: fileFetch,
    langBase: 'http://local/assets/lang/',
    wasmFile: join(here, 'node_modules', 'wasmoon', 'dist', 'glue.wasm'),
  })
  stage = document.createElement('div')
  document.body.appendChild(stage)
  renderer = new DomRenderer(player.core, stage)
})

const NL = String.fromCharCode(10)

/** Bake a map { key -> .ks source } into a runFromBundle-ready bundle object
 *  using the real tokenizer/compiler/serialize pipeline (ks_bake --web path). */
async function bakeBundle(scenes) {
  const entries = []
  // JSON.stringify produces a valid Lua double-quoted string literal for the
  // source, handling embedded quotes / newlines / backslashes losslessly.
  for (const key of Object.keys(scenes)) {
    const src = scenes[key]
    entries.push('  bundle_scenes[' + JSON.stringify(key) + '] = '
      + '(function() local t = tokenizer.parse(' + JSON.stringify(src) + ') '
      + 'compiler.compile(t) return compiler.serialize(t) end)()')
  }
  const code = [
    "local tokenizer = require('tokenizer')",
    "local compiler = require('kag.compiler')",
    'local bundle_scenes = {}',
    entries.join(NL),
    'return { version = 1, scenes = bundle_scenes, assets = {} }',
  ].join(NL)
  return await player.lua.doString(code)
}

/** Backlog plain-text accumulator (the same drill flow.integration uses). */
function backlogText(marker) {
  const pages = player.core.backlog
  const texts = []
  for (const pg of pages) {
    if (Array.isArray(pg)) {
      for (const d of pg) if (d && d.t) texts.push(d.t)
    } else if (pg && pg.draws) {
      for (const d of pg.draws) if (d && d.t) texts.push(d.t)
    } else if (pg && pg.text) {
      texts.push(pg.text)
    }
  }
  return texts.join(' | ')
}

async function currentI18n() {
  await player.lua.doString('_G.__I18N = require("i18n").current_language() or ""')
  return player.lua.global.get('__I18N')
}

describe('bundle cross-scene flow (runFromBundle + __BUNDLE_SCENES)', () => {
  it('uses the same exact scene key for ordinary jumps and restore preparation', async () => {
    const bundle = await bakeBundle({
      'alias_entry.ks': '[jump alias_target.ks]',
      'alias_target.ks': '[save slot=19]\n[set var="f.identity" value=1]\n[end]',
      'assets/script/alias_target.ks': '[save slot=19]\n[set var="f.identity" value=2]\n[end]',
      'alias_loader.ks': '[load slot=19]\n[end]',
    })
    expect((await player.runFromBundle(bundle, 'alias_entry.ks', { maxFrames: 100 })).startsWith('DONE:')).toBe(true)
    const original = await player.lua.doString('return __CTXREF.f.identity')
    expect((await player.runFromBundle(bundle, 'alias_loader.ks', { maxFrames: 100 })).startsWith('DONE:')).toBe(true)
    const restored = await player.lua.doString('return __CTXREF.f.identity')
    expect(restored).toBe(original)
    expect(original).toBe(2)
  })

  it.each(['source', 'bundle'])('restores scheduler scene identities in the %s path', async (mode) => {
    const scenes = {
      'identity_a.ks': '[jump identity_b.ks]',
      'identity_b.ks': '[save slot=18]\n[set var="f.restored_b" value=1]\n[end]',
      'identity_loader.ks': '[load slot=18]\n[end]',
    }
    const bundle = mode === 'bundle' ? await bakeBundle(scenes) : null
    const run = (key) => mode === 'bundle'
      ? player.runFromBundle(bundle, key, { maxFrames: 100 })
      : player.runScene(scenes[key], key, { maxFrames: 100, sceneSources: scenes })
    expect((await run('identity_a.ks')).startsWith('DONE:')).toBe(true)
    expect((await run('identity_loader.ks')).startsWith('DONE:')).toBe(true)
    expect(await player.lua.doString("return __CTXREF.tf.load_error or ''")).toBe('')
    expect(await player.lua.doString("return __CTXREF.tf.load_result == 'ok' and __CTXREF.f.restored_b == 1")).toBe(true)
  })

  it.each(['source', 'bundle'])('restores a bare-key local caller frame in the %s path', async (mode) => {
    const scenes = {
      'identity_call.ks': '[set var="lf.owner" value="caller"]\n[call target="*sub"]\n[eval exp="f.returned = lf.owner"]\n[end]\n*sub\n[set var="lf.owner" value="callee"]\n[save slot=18]\n[return]',
      'identity_loader.ks': '[load slot=18]\n[end]',
    }
    const bundle = mode === 'bundle' ? await bakeBundle(scenes) : null
    const run = (key) => mode === 'bundle'
      ? player.runFromBundle(bundle, key, { maxFrames: 100 })
      : player.runScene(scenes[key], key, { maxFrames: 100, sceneSources: scenes })
    expect((await run('identity_call.ks')).startsWith('DONE:')).toBe(true)
    expect((await run('identity_loader.ks')).startsWith('DONE:')).toBe(true)
    expect(await player.lua.doString("return __CTXREF.tf.load_error or ''")).toBe('')
    expect(await player.lua.doString("return __CTXREF.tf.load_result == 'ok' and __CTXREF.f.returned == 'caller'")).toBe(true)
  })

  it.each([['source', false], ['bundle', false], ['source', true], ['bundle', true]])(
    'awaits a nested asynchronous operation in the %s driver (reject=%s)', async (mode, reject) => {
    const events = []
    player.lua.global.set('__u11_prepare_async', async () => {
      events.push('prepare')
      await new Promise((resolve) => setTimeout(resolve, 1))
      events.push('prepared')
      if (reject) throw new Error('fixture preparation failed')
      return 42
    })
    player.lua.global.set('__u11_expect_rejection', reject)
    await player.lua.doString(`
      -- Register in this same Wasmoon VM before either source or bundle compilation.
      require('kag.schema').define('u11_async_prepare', {})
      require('kag').u11_async_prepare = function(ctx)
        local ok, value = pcall(function() return __u11_prepare_async():await() end)
        if __u11_expect_rejection then
          assert(not ok and tostring(value):find('fixture preparation failed', 1, true))
          ctx.f.prepared = -1
        else
          assert(ok and value == 42, 'driver resumed before the preparation settled')
          ctx.f.prepared = value
        end
      end
    `)
    const source = '[u11_async_prepare]\n[set var="f.after_prepare" value=1]\n[end]'
    const out = mode === 'source'
      ? await player.runScene(source, 'async_prepare.ks', { maxFrames: 100 })
      : await player.runFromBundle(await bakeBundle({ 'async_prepare.ks': source }), 'async_prepare.ks', { maxFrames: 100 })
    expect(out.startsWith('DONE:'), out).toBe(true)
    expect(events).toEqual(['prepare', 'prepared'])
    expect(await player.lua.doString(`return __CTXREF.f.prepared == ${reject ? -1 : 42} and __CTXREF.f.after_prepare == 1`)).toBe(true)
  })

  it('prepares source scenes without publishing or sharing mutable tokens', async () => {
    const source = '*entry\n[set var="f.marker" value=1]\n[end]'
    await player.runScene(source, 'prepare_source.ks', {
      maxFrames: 100, sceneSources: { '../owned_bad.ks': '[end]', '/owned_absolute.ks': '[end]' },
    })
    const prepared = await player.lua.doString(`
      local flow = require('flow')
      assert(flow.is_restore_scene('prepare_source.ks') and flow.is_restore_scene('assets/script/prepare_source.ks'))
      assert(not flow.is_restore_scene('demo/no_such_scene.ks'))
      assert(not flow.is_restore_scene('../owned_bad.ks') and not flow.is_restore_scene('/owned_absolute.ks'))
      local current, tokens = __CTXREF, __CTXREF.tokens
      local index = current.token_index
      local first = assert(flow.prepare_scene('demo/prepare_source.ks'))
      assert(type(first) == 'table' and type(first.tokens) == 'table')
      assert(first.tokens ~= tokens and first.labels.entry == 1)
      first.tokens[2] = {'end', {}}
      local second = assert(flow.prepare_scene('demo/prepare_source.ks'))
      assert(second.tokens[2].cmd == 'set' or second.tokens[2][1] == 'set')
      assert(__CTXREF == current and current.tokens == tokens and current.token_index == index)
      assert(flow.prepare_scene('demo/no_such_scene.ks') == nil)
      return true
    `)
    expect(prepared).toBe(true)
  })

  it('prepares bundled scenes through the trusted compiler without changing the live scene', async () => {
    const bundle = await bakeBundle({
      'prepare_bundle.ks': '*entry\n[set var="f.marker" value=2]\n[end]',
    })
    await player.runFromBundle(bundle, 'prepare_bundle.ks', { maxFrames: 100 })
    const prepared = await player.lua.doString(`
      local current, tokens = __CTXREF, __CTXREF.tokens
      local first = assert(require('flow').prepare_scene('demo/prepare_bundle.ks'))
      assert(type(first) == 'table' and first.tokens ~= tokens and first.labels.entry == 1)
      assert(__CTXREF == current and current.tokens == tokens)
      return true
    `)
    expect(prepared).toBe(true)
  })

  it('jump: A [jump scene_b.ks] -> B executes -> DONE (both in bundle)', async () => {
    const bundle = await bakeBundle({
      'scene_a.ks': [
        "[ch name='Hero' text='A-ONE']",
        '[p]',
        '[jump scene_b.ks]',
        "[ch name='Hero' text='A-NOT-REACHED']",
        '[p]',
        '[end]',
      ].join(NL),
      'scene_b.ks': [
        "[ch name='Hero' text='B-DONE-BODY']",
        '[p]',
        '[end]',
      ].join(NL),
    })
    expect(bundle.scenes['scene_a.ks']).toBeTruthy()
    expect(bundle.scenes['scene_b.ks']).toBeTruthy()
    player.core.backlog.length = 0
    player.core.events.length = 0
    const out = await player.runFromBundle(bundle, 'scene_a.ks', { maxFrames: 50000, autoClick: true })
    expect(out.startsWith('DONE:'), 'bundle cross-scene jump should complete: ' + out).toBe(true)
    expect(out, 'bundle cross-scene jump must not short-circuit').not.toBe('DONE:1:0')
    const t = backlogText()
    expect(t).toContain('A-ONE')
    expect(t).toContain('B-DONE-BODY')
    expect(t, 'jump must NOT fall through to a post-jump line in A').not.toContain('A-NOT-REACHED')
    const errs = player.core.events.filter((e) => String(e.kind).includes('error'))
    expect(errs).toEqual([])
    await player.lua.doString('_G.__SR = _G.__LAST_CTX and _G.__LAST_CTX.current_scene or ""')
    const cur = player.lua.global.get('__SR')
    expect(String(cur), 'after jump the live scene should be scene_b').toContain('scene_b.ks')
  }, 60000)

  it('jump fallback: target not in bundle resolves via __SCENE_SOURCES source parse', async () => {
    const bundle = await bakeBundle({
      'scene_a.ks': [
        "[ch name='Hero' text='A-FB-1']",
        '[p]',
        '[jump not_in_bundle.ks]',
        "[ch name='Hero' text='A-FB-AFTER']",
        '[p]',
        '[end]',
      ].join(NL),
    })
    // scene_a is bundled, but the jump target only exists as raw source:
    const fallbackSrc = [
      "[ch name='Hero' text='FB-SOURCE-BODY']",
      '[p]',
      '[end]',
    ].join(NL)
    player.core.backlog.length = 0
    player.core.events.length = 0
    const out = await player.runFromBundle(bundle, 'scene_a.ks', {
      maxFrames: 50000, autoClick: true,
      sceneSources: { 'not_in_bundle.ks': fallbackSrc },
    })
    expect(out.startsWith('DONE:'), out).toBe(true)
    const t = backlogText()
    expect(t).toContain('A-FB-1')
    expect(t, 'jump target resolved from sceneSources should execute').toContain('FB-SOURCE-BODY')
    expect(t, 'execution should NOT continue in A after the cross-scene jump').not.toContain('A-FB-AFTER')
    const errs = player.core.events.filter((e) => String(e.kind).includes('error'))
    expect(errs).toEqual([])
  }, 60000)

  it('jump missing target: [jump nonexistent.ks] (neither bundle nor sources) WARNs and continues, no crash', async () => {
    const bundle = await bakeBundle({
      'scene_a.ks': [
        "[ch name='Hero' text='A-MISS-1']",
        '[p]',
        '[jump nonexistent.ks]',
        "[ch name='Hero' text='A-MISS-AFTER']",
        '[p]',
        '[end]',
      ].join(NL),
    })
    player.core.backlog.length = 0
    player.core.events.length = 0
    const out = await player.runFromBundle(bundle, 'scene_a.ks', { maxFrames: 50000, autoClick: true })
    // Locked behaviour: a bundled jump to an unresolvable scene must NOT
    // crash or enter an infinite loop — the scheduler WARNs and falls through.
    expect(out.startsWith('DONE:'), 'missing jump target must complete cleanly: ' + out).toBe(true)
    const t = backlogText()
    expect(t).toContain('A-MISS-1')
    expect(t, 'execution continues past the unresolvable jump').toContain('A-MISS-AFTER')
    const errs = player.core.events.filter((e) => String(e.kind).includes('error'))
    expect(errs, 'unresolvable jump must not surface an error event').toEqual([])
  }, 60000)

  it('call/return: A [call scene_c.ks] -> C returns -> A resumes; nested 3-scene stack survives', async () => {
    const bundle = await bakeBundle({
      'scene_a.ks': [
        "[ch name='Hero' text='A-CALL-1']",
        '[p]',
        '[call scene_b.ks]',
        "[ch name='Hero' text='A-CALL-RETURNS']",
        '[p]',
        '[end]',
      ].join(NL),
      'scene_b.ks': [
        "[ch name='Hero' text='B-NESTED-1']",
        '[p]',
        '[call scene_c.ks]',
        "[ch name='Hero' text='B-NESTED-RETURNS']",
        '[p]',
        '[return]',
      ].join(NL),
      'scene_c.ks': [
        "[ch name='Hero' text='C-LEAF-1']",
        '[p]',
        '[return]',
      ].join(NL),
    })
    player.core.backlog.length = 0
    player.core.events.length = 0
    const out = await player.runFromBundle(bundle, 'scene_a.ks', { maxFrames: 100000, autoClick: true })
    expect(out.startsWith('DONE:'), 'cross-scene nested call should complete: ' + out).toBe(true)
    const t = backlogText()
    // call descends A -> B -> C, returns C -> B, then B return -> A
    const ia = t.indexOf('A-CALL-1'), ib = t.indexOf('B-NESTED-1'), ic = t.indexOf('C-LEAF-1')
    const ibR = t.indexOf('B-NESTED-RETURNS'), iaR = t.indexOf('A-CALL-RETURNS')
    expect(ia).toBeGreaterThanOrEqual(0)
    expect(ib).toBeGreaterThan(ia)
    expect(ic).toBeGreaterThan(ib)
    expect(ibR).toBeGreaterThan(ic)
    expect(iaR).toBeGreaterThan(ibR)
    const errs = player.core.events.filter((e) => String(e.kind).includes('error'))
    expect(errs).toEqual([])
  }, 60000)

  it('advance: A parks at [p], advance fires the jump to B, then advance runs B', async () => {
    const bundle = await bakeBundle({
      'scene_a.ks': [
        "[ch name='Hero' text='A-PARK']",
        '[p]',
        '[jump scene_b.ks]',
        "[ch name='Hero' text='A-AFTER-B']",
        '[p]',
        '[end]',
      ].join(NL),
      'scene_b.ks': [
        "[ch name='Hero' text='B-ADV-1']",
        '[p]',
        "[ch name='Hero' text='B-ADV-2']",
        '[p]',
        '[end]',
      ].join(NL),
    })
    player.core.backlog.length = 0
    player.core.events.length = 0
    // 1. park scene_a at its [p] (no autoscroll)
    let out = await player.runFromBundle(bundle, 'scene_a.ks', { maxFrames: 30000 })
    expect(out.startsWith('WAIT:'), 'A should park on its [p]: ' + out).toBe(true)

    // A ch and an explicit p each have a wait in the shared native runner.
    out = await player.runFromBundle(bundle, 'scene_a.ks', { maxFrames: 30000, advance: true, advanceScene: 'scene_a.ks' })
    expect(out.startsWith('WAIT:'), out).toBe(true)
    out = await player.runFromBundle(bundle, 'scene_a.ks', { maxFrames: 30000, advance: true, advanceScene: 'scene_a.ks' })
    expect(out.startsWith('WAIT:'), 'A jumps to the first B line: ' + out).toBe(true)

    // 3. advance continues B: B-ADV-1 -> B-ADV-2 parks
    out = await player.runFromBundle(bundle, 'scene_b.ks', { maxFrames: 30000, advance: true, advanceScene: 'scene_b.ks' })
    // the live ctx/coroutine is already inside scene_b after the jump
    expect(out.startsWith('WAIT:'), '2nd advance should run B and park at B-ADV-2: ' + out).toBe(true)

    // The next click reaches B's second ch, followed by its explicit p.
    out = await player.runFromBundle(bundle, 'scene_b.ks', { maxFrames: 30000, advance: true, advanceScene: 'scene_b.ks' })
    expect(out.startsWith('WAIT:'), out).toBe(true)
    out = await player.runFromBundle(bundle, 'scene_b.ks', { maxFrames: 30000, advance: true, advanceScene: 'scene_b.ks' })
    expect(out.startsWith('WAIT:'), out).toBe(true)
    out = await player.runFromBundle(bundle, 'scene_b.ks', { maxFrames: 30000, advance: true, advanceScene: 'scene_b.ks' })
    expect(out.startsWith('DONE:'), 'B should finish: ' + out).toBe(true)

    const t = backlogText()
    // all four parks committed: A-PARK, B-ADV-1, B-ADV-2 (A-AFTER-B never runs
    // because the jump left A before that line; B-ADV-2 is the final park).
    expect(t).toContain('A-PARK')
    expect(t).toContain('B-ADV-1')
    expect(t).toContain('B-ADV-2')
    expect(t, 'once jumped to B, A post-jump lines never execute').not.toContain('A-AFTER-B')
    const errs = player.core.events.filter((e) => String(e.kind).includes('error'))
    expect(errs).toEqual([])
  }, 60000)

  it('save/load: A saves then a loader scene in B [load]s and resumes A via __BUNDLE_SCENES (round 47 bundle)', async () => {
    for (const k of [...Object.keys(localStorage)]) if (k.startsWith('caesura.save.')) localStorage.removeItem(k)
    const bundle = await bakeBundle({
      'scene_a.ks': [
        "[ch name='Hero' text='A-SL-1']",
        '[p]',
        "[ch name='Hero' text='A-SL-2']",
        '[p]',
        '[set f.marker = 42]',
        '[save slot=1]',
        "[ch name='Hero' text='A-SL-AFTER-SAVE']",
        '[p]',
        '[end]',
      ].join(NL),
      'scene_loader_b.ks': [
        "[ch name='Hero' text='LOADER-B']",
        '[p]',
        '[load slot=1]',
        '[end]',
      ].join(NL),
    })
    // save in bundle scene A: scene name stored must match the bundle key
    player.core.backlog.length = 0
    player.core.events.length = 0
    const outA = await player.runFromBundle(bundle, 'scene_a.ks', { maxFrames: 50000, autoClick: true })
    expect(outA.startsWith('DONE:'), outA).toBe(true)
    expect(player.core.events.some((e) => e.kind === 'save.write'),
      'save failed: ' + String(player._ctx?.tf?.save_error)).toBe(true)
    const slots = player.listSlots()
    const s1 = slots.find((s) => s.slot === 1)
    expect(s1, 'bundle [save] must create slot 1').toBeTruthy()
    expect(String(s1.scene), 'saved scene name must match the bundle key').toContain('scene_a.ks')

    // load from loader scene B (bundle path): __load_scene_tokens resolves the
    // saved scene_a.ks from __BUNDLE_SCENES and resumes at the saved token
    player.core.backlog.length = 0
    const outB = await player.runFromBundle(bundle, 'scene_loader_b.ks', { maxFrames: 50000, autoClick: true })
    expect(outB.startsWith('DONE:'), outB).toBe(true)
    expect(player.core.events.some((e) => e.kind === 'save.read')).toBe(true)
    const t = backlogText()
    expect(t).not.toContain('LOADER-B')
    expect(t).toContain('A-SL-1')
    expect(t).toContain('A-SL-2')
    expect(t, 'bundle [load] must resume scene A at the saved token').toContain('A-SL-AFTER-SAVE')
    const errs = player.core.events.filter((e) => String(e.kind).includes('error'))
    expect(errs, 'bundle cross-scene load must not surface errors').toEqual([])
  }, 60000)

  it('i18n: [i18n language=en] then [jump scene_b.ks]; i18n.current survives and B renders en', async () => {
    const bundle = await bakeBundle({
      'scene_a.ks': [
        '[i18n language="en"]',
        "[ch name='Hero' text='A-i18n-{language}']",
        '[p]',
        '[jump scene_b.ks]',
      ].join(NL),
      'scene_b.ks': [
        "[ch name='Hero' text='B-i18n-{language}']",
        '[p]',
        '[end]',
      ].join(NL),
    })
    player.core.backlog.length = 0
    const out = await player.runFromBundle(bundle, 'scene_a.ks', { maxFrames: 50000, autoClick: true })
    expect(out.startsWith('DONE:'), 'bundle i18n jump should complete: ' + out).toBe(true)
    const lang = await currentI18n()
    expect(lang, 'i18n.current must persist across the bundle cross-scene jump').toBe('en')
    const t = backlogText()
    expect(t).toContain('A-i18n-Language')
    expect(t, 'scene B must render in the en dictionary after the jump').toContain('B-i18n-Language')
    const errs = player.core.events.filter((e) => String(e.kind).includes('error'))
    expect(errs).toEqual([])
  }, 60000)
})
