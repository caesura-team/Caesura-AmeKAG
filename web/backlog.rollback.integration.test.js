// @vitest-environment jsdom
// Shared-runner parity: rollback with real history restores a click point;
// an unconditional inline rollback keeps returning to that point.
//
// Deep backlog / history / rollback coverage beyond the round-82 baseline:
//  1. backlog STRUCTURE  — per-page draws / nameplate / src fields; multi-[p]
//     page order; the [ch name=] nameplate-prefix convention.
//  2. backlog CAP        — web backlog grows UNBOUNDED (no backlog_max on the
//     bridge/AdapterCore side), a 500-page probe runs clean (memory no crash).
//  3. rollback semantics — inline rollback restores real history and stops at
//     its click boundary; public rollback restores a [p] wait and then follows
//     the normal continuation. Lua and JS history discard future entries.
//  4. replay/review      — re-running the same scene re-commits identical pages
//     FIFO on top of the accumulated history; a deep read preserves order.
//  5. clear semantics    — a NEW runScene KEEPS the accumulated backlog (session
//     accumulation; core.backlog is only cleared on construction / manually);
//     [load] restores its own backlog without dropping or interleaving pages.
//  6. i18n x [p] stop    — switching language AFTER a [p] stop re-localizes the
//     committed backlog entry (round 78/91 extend the [p]-stop edge).
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
  return {
    ok,
    status: ok ? 200 : 404,
    text: async () => (ok ? readFileSync(p, 'utf8') : ''),
    json: async () => index,
  }
}

let player = null
const NL = String.fromCharCode(10)

beforeAll(async () => {
  player = await createPlayer({
    scriptsBase: 'http://local/scripts/',
    fetchImpl: fileFetch,
    langBase: 'http://local/assets/lang/',
    wasmFile: join(here, 'node_modules', 'wasmoon', 'dist', 'glue.wasm'),
  })
})

// Read a field off the last run's ctx (the exporter sets __LAST_CTX after
// every runScene). Same pattern as the round-78/91 tests.
const readCtx = async (name) => {
  await player.lua.doString('_G.__R = _G.__LAST_CTX and _G.__LAST_CTX[' + JSON.stringify(name) + '] or nil')
  return player.lua.global.get('__R')
}
const backlogTexts = () => (player.core.backlog || []).map((b) => b.text || '').join(' | ')
const pageText = () => (player.core.draws || []).map((d) => d.t || '').join('')

describe('web backlog / history / rollback (round-87 parity deep)', () => {
  // ---------------------------------------------------------------- task 1 --
  it('backlog page structure: draws carry documented fields; nameplate is [ch] prefix span', async () => {
    player.core.backlog = []
    const ks = [
      '[ch name="N" text="Hello"]', '[p]',
      '[ch name="N" text="World"]', '[p]',
      '[end]',
    ].join(NL)
    const out = await player.runScene(ks, 'bl_struct.ks', { maxFrames: 200000, autoClick: true })
    expect(out.startsWith('DONE:'), out).toBe(true)
    expect(player.core.backlog.length).toBe(2)
    const p0 = player.core.backlog[0]
    const t0 = p0.draws.map((d) => d.t).join('')
    expect(t0).toContain('[N]')
    expect(t0).toContain('Hello')
    // the derived entry.text is the concatenation of its draw texts
    expect(p0.text).toBe(t0)
    for (const pg of player.core.backlog) {
      expect(Array.isArray(pg.draws)).toBe(true)
      for (const d of pg.draws) {
        expect(typeof d.t).toBe('string')
        expect(typeof d.x === 'number' || typeof d.x === 'undefined').toBe(true)
        expect(typeof d.y === 'number' || typeof d.y === 'undefined').toBe(true)
      }
    }
    expect(backlogTexts()).toContain('[N]Hello')
    expect(backlogTexts()).toContain('[N]World')
  }, 120000)

  it('multi-[p] page order and a [ch name=] nameplate prefix are preserved FIFO', async () => {
    player.core.backlog = []
    const ks = []
    for (let i = 1; i <= 5; i++) ks.push('[ch name="Char' + i + '" text="MSG' + i + '"]', '[p]')
    ks.push('[end]')
    const out = await player.runScene(ks.join(NL), 'bl_multi.ks', { maxFrames: 200000, autoClick: true })
    expect(out.startsWith('DONE:'), out).toBe(true)
    expect(player.core.backlog.length).toBe(5)
    const texts = player.core.backlog.map((b) => b.text)
    for (let i = 1; i <= 5; i++) {
      const t = texts[i - 1]
      expect(t.startsWith('[Char' + i + ']'), 'page ' + i + ' prefix: ' + t).toBe(true)
      expect(t).toContain('MSG' + i)
    }
    expect(texts[0]).toBe('[Char1]MSG1')
    expect(texts[4]).toBe('[Char5]MSG5')
  }, 120000)

  // ---------------------------------------------------------------- task 2 --
  it('backlog grows UNBOUNDED across pages; a 500-page probe runs clean (no cap, no crash)', async () => {
    player.core.backlog = []
    player.core.events.length = 0
    const ks = []
    for (let i = 1; i <= 500; i++) ks.push('[ch name="N" text="L' + i + '"]', '[p]')
    ks.push('[end]')
    // autoClick caps at 100 clicks per run (engine runner guard), so a single
    // run commits the first 100 [p] pages; drive the parked cursor to DONE with
    // the round-81 advance loop (each advance pops one parked [p] page). This
    // is the ONLY practical way to commit 500 pages in one scene session.
    let out = await player.runScene(ks.join(NL), 'bl_500.ks', { maxFrames: 2000000, autoClick: true })
    // first run commits a bounded prefix of pages (autoClick guard) — already
    // proof the web backlog holds far more than a manual-slot history list
    expect(player.core.backlog.length).toBeGreaterThan(0)
    let guard = 0
    while (!String(out).startsWith('DONE:') && guard < 700) {
      out = await player.runScene(ks.join(NL), 'bl_500.ks', { maxFrames: 2000000, advance: true, advanceScene: 'bl_500.ks' })
      guard++
    }
    expect(out.startsWith('DONE:'), '500-page probe must complete, got ' + out).toBe(true)
    // NO backlog_max on the web side: all 500 pages landed, nothing trimmed
    expect(player.core.backlog.length).toBe(500)
    const errs = player.core.events.filter((e) => String(e.kind).includes('error'))
    expect(errs, '500-page probe should have no error events').toEqual([])
    expect(player.core.backlog[0].text).toContain('L1')
    expect(player.core.backlog[499].text).toContain('L500')
  }, 300000)

  // ---------------------------------------------------------------- task 3 --
  it('inline [rollback] restores a real checkpoint and never falls through its script loop', async () => {
    player.core.backlog = []
    player.core.events.length = 0
    const ks = [
      '[set var="f.reward" value=0]', '[set var="lf.local_value" value=10]',
      '[ch name="N" text="P1"]',
      '[set var="f.reward" value=1]', '[set var="lf.local_value" value=20]',
      '[ch name="N" text="P2"]',
      '[set var="f.reward" value=2]', '[set var="lf.local_value" value=30]',
      '[rollback]',
      '[set var="f.future_reward" value=1]', '[ch name="N" text="P3"]',
      '[end]',
    ].join(NL)
    const advance = (autoClick = false) => player.runScene(ks, 'rb_mid.ks', {
      maxFrames: 200, advance: true, advanceScene: 'rb_mid.ks', autoClick,
    })
    let out = await player.runScene(ks, 'rb_mid.ks', { maxFrames: 200 })
    expect(out.startsWith('WAIT:'), out).toBe(true)
    expect(await player.lua.doString("return #require('kag_runner').get_ctx()._undoStack")).toBe(0)
    out = await advance()
    expect(out.startsWith('WAIT:'), out).toBe(true)
    expect(pageText()).toBe('[N]P2')
    expect(await player.lua.doString("return #require('kag_runner').get_ctx()._undoStack")).toBe(1)

    // Even automatic host clicks stop at the restored boundary. A bounded
    // driver must not silently skip the repeated rollback to manufacture DONE.
    out = await advance(true)
    expect(out.startsWith('WAIT:'), out).toBe(true)
    const restored = await player.lua.doString(`
      local c=require('kag_runner').get_ctx()
      return {reward=c.f.reward, local_value=c.lf.local_value, future=c.f.future_reward or false,
        undo=#c._undoStack, waiting=c.waiting_input, pending=c._pendingRollback~=nil,
        shown=c.text_state.reveal_chars, total=c.reveal.total}
    `)
    expect(restored).toMatchObject({ reward: 1, local_value: 20, future: false, undo: 1, waiting: true, pending: false })
    expect(restored.shown).toBe(restored.total)
    expect(pageText()).toBe('[N]P2')
    expect((await readCtx('backlog')).map(entry => `[${entry.name}]${entry.text}`)).toEqual(['[N]P1', '[N]P2'])
    expect(player.core.backlog.map(entry => entry.text)).toEqual(['[N]P1', '[N]P2'])
    out = await advance()
    expect(out.startsWith('WAIT:'), out).toBe(true)
    expect(pageText()).toBe('[N]P2')
    expect(player.core.backlog.map(entry => entry.text)).toEqual(['[N]P1', '[N]P2'])
    expect(await player.lua.doString("return require('kag_runner').get_ctx().f.future_reward or false")).toBe(false)
    const errs = player.core.events.filter((e) => String(e.kind).includes('error'))
    expect(errs, 'rollback must surface no error event').toEqual([])
  }, 120000)

  it('public rollback restores an explicit p wait and then follows the baseline continuation', async () => {
    player.core.backlog = []
    const ks = [
      '[set var="f.reward" value=0]', '[set var="lf.local_value" value=10]',
      '[ch name="A" text="R1"]', '[p]',
      '[set var="f.reward" value=1]', '[set var="lf.local_value" value=20]',
      '[ch name="A" text="R2"]', '[p]',
      '[set var="f.reward" value=2]', '[ch name="A" text="R3"]', '[p]',
      '[end]',
    ].join(NL)
    const normal = await player.runScene(ks, 'rb_p.ks', { maxFrames: 2000, autoClick: true })
    expect(normal.startsWith('DONE:'), normal).toBe(true)
    const baselineHistory = player.core.backlog.map(entry => entry.text)
    const baselineReward = (await readCtx('f')).reward
    expect(baselineHistory).toEqual(['[A]R1', '[A]R2', '[A]R3'])

    player.core.backlog = []
    let out = await player.runScene(ks, 'rb_p.ks', { maxFrames: 50000 })
    expect(out.startsWith('WAIT:'), out).toBe(true)
    expect(pageText()).toContain('R1')

    out = await player.runScene(ks, 'rb_p.ks', { maxFrames: 50000, advance: true, advanceScene: 'rb_p.ks' })
    expect(out.startsWith('WAIT:'), out).toBe(true)
    // The common runner respects both the ch wait and the explicit p wait.
    expect(pageText()).toContain('R1')
    out = await player.runScene(ks, 'rb_p.ks', { maxFrames: 50000, advance: true, advanceScene: 'rb_p.ks' })
    expect(pageText()).toBe('[A]R2')
    expect(await player.lua.doString("return #require('kag_runner').get_ctx()._undoStack")).toBe(2)
    // Commit R3 to the JS backlog too, so restoration must actually remove
    // future JS content rather than merely catch up with the Lua current page.
    for (let clicks = 0; clicks < 3; clicks++) {
      out = await player.runScene(ks, 'rb_p.ks', { maxFrames: 50000, advance: true, advanceScene: 'rb_p.ks' })
      expect(out.startsWith('WAIT:'), out).toBe(true)
    }
    expect(pageText()).toBe('[A]R3')
    expect(player.core.backlog.map(entry => entry.text)).toEqual(baselineHistory)
    expect(await player.lua.doString("return #require('kag_runner').get_ctx()._undoStack")).toBe(5)
    const restored = await player.lua.doString(`
      local r=require('kag_runner')
      local ok,reason
      for _=1,4 do
        ok,reason=r.rollback()
        if not ok then break end
      end
      local c=r.get_ctx()
      local draws={}
      require('kag.text_scene').render(c,{render_text=function(text) draws[#draws+1]=text end})
      return {ok=ok,reason=reason,page=table.concat(draws),reward=c.f.reward,local_value=c.lf.local_value,
        waiting=c.waiting_input,undo=#c._undoStack}
    `)
    expect(restored).toMatchObject({ ok: true, page: '[A]R1', reward: 0, local_value: 10, waiting: true, undo: 1 })
    expect((await readCtx('backlog')).map(entry => entry.text)).toEqual(['R1'])
    out = await player.runScene(ks, 'rb_p.ks', { maxFrames: 50000, advance: true, advanceScene: 'rb_p.ks' })
    expect(out.startsWith('WAIT:'), out).toBe(true)
    expect(pageText()).toBe('[A]R2')
    expect(player.core.backlog.map(entry => entry.text)).toEqual(['[A]R1', '[A]R2'])
    expect((await readCtx('backlog')).map(entry => `[${entry.name}]${entry.text}`)).toEqual(['[A]R1', '[A]R2'])
    out = await player.runScene(ks, 'rb_p.ks', { maxFrames: 50000, advance: true, advanceScene: 'rb_p.ks', autoClick: true })
    expect(out.startsWith('DONE:'), out).toBe(true)
    expect(player.core.backlog.map(entry => entry.text)).toEqual(baselineHistory)
    expect((await readCtx('f')).reward).toBe(baselineReward)
    expect((await readCtx('backlog')).map(entry => `[${entry.name}]${entry.text}`)).toEqual(baselineHistory)
  }, 120000)

  it.each(['source', 'bundle'])('manual advance with persistent skip resumes a restored %s checkpoint', async (mode) => {
    player.core.backlog = []
    const name = 'rb_manual_skip.ks'
    const ks = [
      '[set var="f.reward" value=0]', '[ch name="N" text="ALPHA"]',
      '[set var="f.reward" value=1]', '[ch name="N" text="BRAVO"]',
      '[set var="f.reward" value=2]', '[end]',
    ].join(NL)
    let bundle
    if (mode === 'bundle') {
      player.lua.global.set('__ROLLBACK_SKIP_SOURCE', ks)
      const serialized = await player.lua.doString(`
        local compiler=require('kag.compiler')
        local tokens=require('tokenizer').parse(__ROLLBACK_SKIP_SOURCE)
        compiler.compile(tokens)
        return compiler.serialize(tokens)
      `)
      bundle = { version: 1, scenes: { [name]: serialized }, assets: [] }
    }
    const run = (opts = {}) => mode === 'bundle'
      ? player.runFromBundle(bundle, name, { maxFrames: 200, ...opts })
      : player.runScene(ks, name, { maxFrames: 200, ...opts })
    expect(await run()).toMatch(/^WAIT:/)
    expect(await run({ advance: true, advanceScene: name })).toMatch(/^WAIT:/)
    expect(pageText()).toBe('[N]BRAVO')
    const restored = await player.lua.doString(`
      local r=require('kag_runner')
      local ok=r.rollback()
      local c=r.get_ctx()
      return {ok=ok,waiting=c._rollback_waiting,reward=c.f.reward,undo=#c._undoStack}
    `)
    expect(restored).toMatchObject({ ok: true, waiting: true, reward: 0, undo: 0 })
    const out = await run({ advance: true, advanceScene: name, skip: true })
    expect(out.startsWith('DONE:'), out).toBe(true)
    expect((await readCtx('f')).reward).toBe(2)
    expect(player.core.backlog.map(entry => entry.text)).toEqual(['[N]ALPHA', '[N]BRAVO'])
    expect(await player.lua.doString("return require('kag_runner').get_ctx()._rollback_waiting or false")).toBe(false)
  }, 120000)

  // ---------------------------------------------------------------- task 4 --
  it('replay/review: re-running the same scene re-commits identical pages FIFO; deep read order preserved', async () => {
    player.core.backlog = []
    const ks = [
      '[ch name="N" text="A"]', '[p]',
      '[ch name="N" text="B"]', '[p]',
      '[ch name="N" text="C"]', '[p]',
      '[end]',
    ].join(NL)
    const out = await player.runScene(ks, 'bl_replay.ks', { maxFrames: 200000, autoClick: true })
    expect(out.startsWith('DONE:'), out).toBe(true)
    expect(player.core.backlog.length).toBe(3)

    const out2 = await player.runScene(ks, 'bl_replay.ks', { maxFrames: 200000, autoClick: true })
    expect(out2.startsWith('DONE:'), out2).toBe(true)
    expect(player.core.backlog.length).toBe(6)
    const texts = player.core.backlog.map((b) => b.text)
    expect(texts.slice(0, 3)).toEqual(['[N]A', '[N]B', '[N]C'])
    expect(texts.slice(3, 6)).toEqual(['[N]A', '[N]B', '[N]C'])
    expect(player.core.backlog[4].text).toContain('B')
  }, 120000)

  // ---------------------------------------------------------------- task 5 --
  it('a NEW runScene KEEPS the accumulated backlog (session accumulation)', async () => {
    player.core.backlog = []
    const out1 = await player.runScene(['[ch name="N" text="ONE"]', '[p]', '[end]'].join(NL), 'keep_one.ks', { maxFrames: 200000, autoClick: true })
    expect(out1.startsWith('DONE:'), out1).toBe(true)
    expect(player.core.backlog.length).toBe(1)

    const out2 = await player.runScene(['[ch name="N" text="TWO"]', '[p]', '[end]'].join(NL), 'keep_two.ks', { maxFrames: 200000, autoClick: true })
    expect(out2.startsWith('DONE:'), out2).toBe(true)
    expect(player.core.backlog.length).toBe(2)
    expect(backlogTexts()).toContain('ONE')
    expect(backlogTexts()).toContain('TWO')
  }, 120000)

  it('[load] restores slot-owned history and discards future loader pages', async () => {
    for (const k of [...Object.keys(localStorage)]) if (k.startsWith('caesura.save.')) localStorage.removeItem(k)
    // Scene A commits 2 pages before the [save] and 1 page after; the
    // snapshot owns the first two pages. SA3 is replayed once after loading.
    const sceneA = [
      '[ch name="N" text="SA1"]', '[p]',
      '[ch name="N" text="SA2"]', '[p]',
      '[save slot=3]',
      '[ch name="N" text="SA3"]', '[p]',
      '[end]',
    ].join(NL)
    player.core.backlog = []
    const outA = await player.runScene(sceneA, 'bl_save_a.ks', { maxFrames: 200000, autoClick: true })
    expect(outA.startsWith('DONE:'), outA).toBe(true)
    const beforeLoad = player.core.backlog.length
    expect(beforeLoad).toBe(3) // SA1, SA2, SA3

    // loadSlot builds its own loader scene ([System]Loading slot N... [p]
    // [load slot=N]) and resumes from the saved token (SA3). The loader is
    // future state and must disappear when the slot replaces the session.
    const outL = await player.loadSlot(3, { sceneSources: { 'bl_save_a.ks': sceneA } })
    expect(outL.startsWith('DONE:'), outL).toBe(true)
    const texts = player.core.backlog.map((b) => b.text)
    // original session pages intact (head of the list not dropped)
    expect(texts[0]).toContain('SA1')
    expect(texts[1]).toContain('SA2')
    // the loader page and the resumed SA3 appear AFTER the pre-load history
    expect(texts.some((t) => t.includes('Loading slot 3'))).toBe(false)
    expect(texts).toEqual(['[N]SA1', '[N]SA2', '[N]SA3'])
    expect(player.core.backlog.length).toBe(beforeLoad)
    const errs = player.core.events.filter((e) => String(e.kind).includes('error'))
    expect(errs).toEqual([])
    player.deleteSlot(3)
  }, 120000)

  // ---------------------------------------------------------------- task 6 --
  it('[i18n] switch after a [p] stop re-localizes the committed backlog entry via relocalize_backlog', async () => {
    player.core.backlog = []
    const ks = [
      '[i18n language="zh"]',
      '[ch name="N" text="s={settings}"]', '[p]',
      '[end]',
    ].join(NL)
    const out = await player.runScene(ks, 'i18n_stop.ks', { maxFrames: 50000 })
    expect(out.startsWith('WAIT:'), out).toBe(true)
    await player.lua.doString('(require("i18n")).set_language("en")')
    await player.lua.doString([
      'local okR, errR = pcall(function()',
      "  local kt = require('kag.commands.text')",
      "  local ctx = _G.__CTXREF",
      "  if type(kt) == 'table' and kt.relocalize_backlog and type(ctx) == 'table' then kt.relocalize_backlog(ctx) end",
      'end)',
      'return okR and "ok" or tostring(errR)',
    ].join(NL))
    const bl = await readCtx('backlog')
    expect(Array.isArray(bl) && bl.length).toBeGreaterThanOrEqual(1)
    expect(bl[0].text).toContain('s=Settings')
    expect(bl[0].src).toBe('s={settings}')
  }, 120000)
})
