// @vitest-environment jsdom
// Round 109: WEB player (wasmoon) large-scene performance baseline.
//
// Goal: establish a Web-player performance baseline for large scenes —
// frame throughput (scheduler ticks/ms), token throughput (tokens/ms),
// and Lua-heap growth under collectgarbage — so engine/script hot-path
// changes that would regress the browser player surface detectably.
//
// Measurement surface (jsdom-headless, wasmoon): wall time around runScene
// includes parsing, scheduling, bridge work and final state publication.
// Throughput uses one representative warmup and the median of three runs
// in the same VM; it does not measure first-player startup. Every run still
// checks completion, frame/token counts and error events.
// bridge.js round-109 hook writes _G.__FRAME_COUNT under __PERF_TRACE so
// we can read the exact frame (tick) count — gated off on the normal path.
// Memory uses the round-101 technique: collectgarbage("collect") x3 then
// collectgarbage("count") (KB) before/after — reflects Lua-managed heap
// (tables + strings) inside wasmoon emscripten linear memory.
//
// The round-109 682 ms synthetic baseline used a separate driver without
// rollback history. The real U11 runner maintains 64 checkpoints and 2000
// read marks here; hosted Windows measured about 1533–1580 ms. Its budget
// is 2000 ms (3000 tokens / 1.5 tokens/ms), with every run checking that work.
// This is a workload-budget recalibration, not a measured engine speedup.
// Driver ticks/ms are not rendered FPS. Runs in CI; run locally via
// `cd web && npx vitest run perf-baseline.test.js`.
import { describe, it, expect, beforeAll, afterAll } from 'vitest'
import { readFileSync, existsSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { dirname, join } from 'node:path'
import { createPlayer } from './bridge.js'

const here = dirname(fileURLToPath(import.meta.url))
const rootDir = join(here, '..')
const scriptsDir = join(rootDir, 'scripts')
const assetsDir = join(rootDir, 'assets')
const index = JSON.parse(readFileSync(join(here, 'scripts-index.json'), 'utf8'))
const syntheticMinFrames = 2.0
const syntheticMinTokens = 1.5

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
  return { ok, status: ok ? 200 : 404, text: async () => (ok ? readFileSync(p, 'utf8') : ''), json: async () => index }
}

function sourceFor(key) {
  for (const dir of ['../demo/', '../demo/tutorial/', '../demo/example_game/']) {
    const p = join(here, dir, key)
    if (existsSync(p)) return readFileSync(p, 'utf8')
  }
  return null
}

function makeSynthetic(lines) {
  let ks = ''
  for (let i = 0; i < lines; i++) {
    ks += '[ch name="Nar' + (i % 4) + '"]synthetic stress line ' + i + ' of the large scene\n[p]\n'
  }
  return ks
}

async function heapKB(player) {
  const s = await player.lua.doString(
    'collectgarbage("collect"); collectgarbage("collect"); collectgarbage("collect"); return tostring(collectgarbage("count"))'
  )
  return parseFloat(String(s))
}

async function benchmarkRun(player, src, name, expectedTokens) {
  player.core.events.length = 0
  player.core.backlog.length = 0
  player.lua.global.set('__PERF_TRACE', true)
  player.lua.global.set('__FRAME_COUNT', 0)
  const memBefore = await heapKB(player)
  const t0 = performance.now()
  const out = await player.runScene(src, name, { maxFrames: 1000000, autoClick: true })
  const t1 = performance.now()
  const frames = Number((await player.lua.global.get('__FRAME_COUNT')) || 0)
  const memAfter = await heapKB(player)
  const wall = t1 - t0
  const m = /^DONE:(\d+):(\d+)$/.exec(String(out))
  expect(m, 'scene should complete: ' + out).not.toBeNull()
  expect(frames, 'real scheduler tick count should be reported').toBeGreaterThan(100)
  expect(Number.isFinite(wall) && wall > 0, 'positive measured duration').toBe(true)
  if (expectedTokens !== undefined) {
    expect(Number(m[1])).toBe(expectedTokens)
    const history = await player.lua.doString(`
      local count=0
      for _,flags in pairs(__CTXREF.seen_scenes)do
        for _,read in pairs(flags)do if read==true then count=count+1 end end
      end
      return #__CTXREF._undoStack..':'..count
    `)
    expect(history, 'complete bounded history and read marks').toBe('64:' + (expectedTokens * 2 / 3))
  }
  expect(player.core.events.filter(event => String(event.kind).includes('error'))).toEqual([])
  return {
    out: String(out),
    wallMs: wall,
    frames,
    framesPerMs: frames / wall,
    tokens: m ? Number(m[1]) : NaN,
    tokensPerMs: m ? Number(m[1]) / wall : NaN,
    memGrowthKB: memAfter - memBefore,
  }
}

function medianRun(samples, name) {
  expect(samples).toHaveLength(3)
  const median = [...samples].sort((left, right) => left.wallMs - right.wallMs)[1]
  if (name) process.stdout.write(`[perf] ${name}: samples=${samples.map(run => run.wallMs.toFixed(1)).join(',')}ms; median=${median.wallMs.toFixed(1)}ms; tokens/ms=${median.tokensPerMs.toFixed(3)}\n`)
  return median
}

function assertThroughput(run, name, minFrames, minTokens) {
  expect(run.framesPerMs, name + ' frame throughput >= ' + minFrames).toBeGreaterThan(minFrames)
  expect(run.tokensPerMs, name + ' token throughput >= ' + minTokens).toBeGreaterThan(minTokens)
}

async function steadyRun(player, source, name, expectedTokens) {
  await benchmarkRun(player, source, name, expectedTokens)
  const samples = []
  for (let sample = 0; sample < 3; sample++) {
    samples.push(await benchmarkRun(player, source, name, expectedTokens))
  }
  return medianRun(samples, name)
}

describe('performance measurement statistics', () => {
  const sample = (wallMs) => ({ wallMs, framesPerMs: 4001 / wallMs, tokensPerMs: 3000 / wallMs })

  it('selects an observed median sample without changing the measurements', () => {
    const samples = [sample(1200), sample(8000), sample(1000)]
    const original = [...samples]
    const median = medianRun(samples)
    expect(median).toBe(samples[0])
    expect(samples).toEqual(original)
    expect(() => assertThroughput(median, 'synthetic statistic fixture', syntheticMinFrames, syntheticMinTokens)).not.toThrow()
  })

  it('keeps sustained slowdown failing even when one sample is fast', () => {
    for (const times of [[2100, 2200, 2300], [1000, 2200, 2300]]) {
      const median = medianRun(times.map(sample))
      expect(() => assertThroughput(median, 'synthetic statistic fixture', syntheticMinFrames, syntheticMinTokens))
        .toThrow(/frame throughput/)
    }
    const tokenLimited = medianRun([2100, 2200, 2300].map(wall => ({...sample(wall), framesPerMs:3})))
    expect(() => assertThroughput(tokenLimited, 'synthetic statistic fixture', syntheticMinFrames, syntheticMinTokens))
      .toThrow(/token throughput/)
  })
})

describe('web player performance baseline (round 109)', () => {
  let player = null
  beforeAll(async () => {
    player = await createPlayer({
      scriptsBase: 'http://local/scripts/',
      fetchImpl: fileFetch,
      langBase: 'http://local/assets/lang/',
      wasmFile: join(here, 'node_modules', 'wasmoon', 'dist', 'glue.wasm'),
    })
  }, 60000)
  afterAll(async () => { await player?.dispose() })

  it('story.ks main path: frame throughput + completes clean', async () => {
    const src = sourceFor('story.ks')
    expect(src, 'demo/example_game/story.ks should exist').toBeTruthy()
    const r = await steadyRun(player, src, 'story.ks')
    assertThroughput(r, 'story.ks', 0.8, 0.08)
  }, 120000)

  it('story.ks Lua heap growth stays bounded (< 1024 KB)', async () => {
    const r = await benchmarkRun(player, sourceFor('story.ks'), 'story.ks-mem')
    expect(r.memGrowthKB, 'story.ks heap growth < 1024 KB (got ' + r.memGrowthKB.toFixed(1) + ' KB)').toBeLessThan(1024)
  }, 120000)

  it('synthetic 1000-line scene: frame throughput + correctness (3000 tokens)', async () => {
    const r = await steadyRun(player, makeSynthetic(1000), 'synthetic1000.ks', 3000)
    assertThroughput(r, 'synthetic 1000-line with history', syntheticMinFrames, syntheticMinTokens)
  }, 120000)

  it('synthetic 1000-line Lua heap growth stays bounded (< 2048 KB)', async () => {
    const r = await benchmarkRun(player, makeSynthetic(1000), 'synthetic1000-mem.ks', 3000)
    expect(r.memGrowthKB, 'synthetic 1000-line heap growth < 2048 KB (got ' + r.memGrowthKB.toFixed(1) + ' KB)').toBeLessThan(2048)
  }, 120000)

  it('2000-line scene runs within 2.5x of the 1000-line scene (linear-ish)', async () => {
    const small = makeSynthetic(1000), large = makeSynthetic(2000)
    const smallRun = () => benchmarkRun(player, small, 'synth1000-scale.ks', 3000)
    const largeRun = () => benchmarkRun(player, large, 'synth2000-scale.ks', 6000)
    await smallRun()
    await largeRun()
    const smallSamples = [], largeSamples = []
    for (let sample = 0; sample < 3; sample++) {
      if (sample % 2 === 0) {
        smallSamples.push(await smallRun())
        largeSamples.push(await largeRun())
      } else {
        largeSamples.push(await largeRun())
        smallSamples.push(await smallRun())
      }
    }
    const r1 = medianRun(smallSamples, 'scale 1000'), r2 = medianRun(largeSamples, 'scale 2000')
    expect(r2.wallMs, 'doubling scene size should not blow up (wall2000 < 2.5x wall1000)').toBeLessThan(r1.wallMs * 2.5)
  }, 120000)

  it('wasmoon limitation observability: heap window + single-thread coroutines', async () => {
    const hk = await heapKB(player)
    expect(hk, 'collectgarbage("count") should report Lua heap in KB').toBeGreaterThan(0)
    const syncProbe = await player.lua.doString('return tostring(coroutine.running())')
    expect(syncProbe, 'scene frame loop drives coroutines cooperatively on the main thread').toBeTruthy()
  }, 60000)
})
