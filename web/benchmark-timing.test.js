// @vitest-environment node
// Deterministic timing/lifecycle contracts. No Wasmoon VM or performance workload.
import { describe, it, expect } from 'vitest'
import * as fs from 'node:fs'
import * as path from 'node:path'
import * as url from 'node:url'
import { dualMedianMs } from './benchmark-timing.mjs'

function deferred() {
  let resolve, reject
  const promise = new Promise((ok, fail) => { resolve = ok; reject = fail })
  // A broken non-awaiting caller must not leave an unhandled fixture rejection.
  promise.catch(() => {})
  return { promise, resolve, reject }
}

async function flushMicrotasks() {
  for (let i = 0; i < 6; i++) await Promise.resolve()
}

function controlledCalls() {
  let time = 0n, active = 0, maxActive = 0
  const calls = []
  function call(kind) {
    const gate = deferred()
    active++
    maxActive = Math.max(active, maxActive)
    const result = gate.promise.then(ms => { time += BigInt(ms) * 1000000n })
      .finally(() => { active-- })
    result.catch(() => {})
    calls.push({ kind, gate })
    return result
  }
  return {
    calls, nowNs: () => time, call,
    get active() { return active }, get maxActive() { return maxActive },
    releaseAll() { for (const { gate } of calls) gate.resolve(0) },
  }
}

describe('completed async benchmark timing', () => {
  it('waits at each Promise barrier, including both warmups, and times completion', async () => {
    const state = controlledCalls()
    const completed = Promise.resolve(dualMedianMs(() => state.call('A'), () => state.call('B'), 3, state.nowNs))
    const countsBeforeRelease = []
    try {
      // Warmup durations are deliberately larger and must not enter the median.
      const durations = [100, 200, 4, 7, 9, 2, 5, 6]
      for (let i = 0; i < durations.length; i++) {
        await flushMicrotasks()
        countsBeforeRelease.push(state.calls.length)
        state.calls[i]?.gate.resolve(durations[i])
      }
      const actual = await completed
      expect({ countsBeforeRelease, maxActive: state.maxActive, actual }).toEqual({
        countsBeforeRelease: [1, 2, 3, 4, 5, 6, 7, 8],
        maxActive: 1,
        actual: { srcMs: 5, bndMs: 6 },
      })
      expect(state.calls.map(call => call.kind)).toEqual(['A', 'B', 'A', 'B', 'A', 'B', 'A', 'B'])
      expect(state.active).toBe(0)
    } finally {
      state.releaseAll()
      await completed
    }
  })

  it('keeps the existing upper-middle selection for even sample counts', async () => {
    let time = 0n
    const values = [100, 200, 4, 7, 9, 2]
    const next = () => { time += BigInt(values.shift()) * 1000000n }
    expect(await dualMedianMs(next, next, 2, () => time)).toEqual({ srcMs: 9, bndMs: 7 })
    expect(values).toEqual([])
  })

  for (const [stage, failingCall] of [['warmup A', 0], ['warmup B', 1], ['sample A', 2], ['sample B', 3]]) {
    it(`propagates ${stage} rejection without starting the next operation`, async () => {
      const state = controlledCalls()
      const failure = new Error(`controlled ${stage} rejection`)
      const outcome = Promise.resolve(dualMedianMs(() => state.call('A'), () => state.call('B'), 2, state.nowNs))
        .then(value => ({ value }), error => ({ error }))
      try {
        for (let i = 0; i <= failingCall; i++) {
          await flushMicrotasks()
          if (i === failingCall) state.calls[i]?.gate.reject(failure)
          else state.calls[i]?.gate.resolve(1)
        }
        await flushMicrotasks()
        expect({ outcome: await outcome, started: state.calls.length, active: state.active }).toEqual({
          outcome: { error: failure }, started: failingCall + 1, active: 0,
        })
      } finally {
        state.releaseAll()
        await outcome
      }
    })
  }
})

// Execute the maintained benchmark's real registration and callbacks. Only its
// module imports, Vitest runner hooks, player host, and clock are substituted;
// no copy of its warmup/sample/callback/disposal implementation lives here.
function loadBenchmarkFixture({ failAt = null } = {}) {
  const entry = new URL('./perf-bundle.test.js', import.meta.url)
  const specs = [], before = [], after = [], calls = []
  const disposeGate = deferred()
  let time = 0n, active = 0, maxActive = 0, disposeCalls = 0, disposed = false
  const failure = new Error('controlled benchmark player rejection')
  function run(kind, key) {
    calls.push({ kind, key })
    const callIndex = calls.length
    active++
    maxActive = Math.max(active, maxActive)
    const result = Promise.resolve().then(() => {
      time += 2000000n
      if (callIndex === failAt) throw failure
      return 'DONE:2000:2000'
    }).finally(() => { active-- })
    result.catch(() => {})
    return result
  }
  const player = {
    core: { backlog: [], events: [] },
    lua: {
      doString: async code => code.includes('bundle_scenes')
        ? { version: 1, scenes: { 'perf_tiny.ks': {}, 'story.ks': {}, 'perf_big.ks': {} } }
        : undefined,
      global: { get: () => 2000 },
    },
    runScene: (_source, key) => run('source', key),
    runFromBundle: (_bundle, key) => run('bundle', key),
    dispose: () => {
      disposeCalls++
      return disposeGate.promise.then(() => { disposed = true })
    },
  }
  const imports = {
    vitest: {
      describe: (_name, register) => register(),
      it: (name, execute) => specs.push({ name, execute }),
      expect,
      beforeAll: hook => before.push(hook),
      afterAll: hook => after.push(hook),
    },
    'node:fs': fs, 'node:path': path, 'node:url': url,
    './bridge.js': { createPlayer: async () => player },
    './benchmark-timing.mjs': { dualMedianMs: (a, b, reps) => dualMedianMs(a, b, reps, () => time) },
  }
  let source = fs.readFileSync(entry, 'utf8').replace(/\r\n/g, '\n')
  source = source.replace(/^import (.+) from (["'])(.+?)\2;?$/gm, (_line, bindings, _quote, specifier) => {
    if (!Object.hasOwn(imports, specifier)) throw new Error(`unmapped benchmark import: ${specifier}`)
    return `const ${bindings} = imports[${JSON.stringify(specifier)}]`
  }).replaceAll('import.meta.url', JSON.stringify(entry.href))
  // The controlled host does not log synthetic timings as performance evidence.
  new Function('imports', 'console', source)(imports, { log() {} })
  return {
    specs, before, after, calls, disposeGate, failure,
    get active() { return active }, get maxActive() { return maxActive },
    get disposeCalls() { return disposeCalls }, get disposed() { return disposed },
  }
}

describe('maintained bundle benchmark callback and disposal contracts', () => {
  for (const [index, operationCount] of [[0, 22], [1, 14], [2, 14]]) {
    it(`awaits every actual benchmark callback for case ${index + 1} and disposes after completion`, async () => {
      const fixture = loadBenchmarkFixture()
      try {
        for (const hook of fixture.before) await hook()
        expect(fixture.specs).toHaveLength(3)
        await fixture.specs[index].execute()
        await flushMicrotasks()
        expect({ count: fixture.calls.length, maxActive: fixture.maxActive, active: fixture.active }).toEqual({
          count: operationCount, maxActive: 1, active: 0,
        })
        expect(fixture.calls.map(call => call.kind)).toEqual(
          Array.from({ length: operationCount }, (_, i) => i % 2 === 0 ? 'source' : 'bundle'))
      } finally {
        let cleanupSettled = false
        const cleanup = Promise.all(fixture.after.map(hook => hook())).then(() => { cleanupSettled = true })
        await flushMicrotasks()
        const beforeRelease = { calls: fixture.disposeCalls, disposed: fixture.disposed, cleanupSettled }
        fixture.disposeGate.resolve()
        await cleanup
        expect.soft(beforeRelease).toEqual({ calls: 1, disposed: false, cleanupSettled: false })
        expect.soft(fixture.disposed).toBe(true)
      }
    })
  }

  it('propagates a real callback rejection, stops samples, and still awaits the registered disposal hook', async () => {
    const fixture = loadBenchmarkFixture({ failAt: 3 })
    try {
      for (const hook of fixture.before) await hook()
      await expect(fixture.specs[1].execute()).rejects.toBe(fixture.failure)
      expect(fixture.calls).toHaveLength(3)
      expect(fixture.active).toBe(0)
    } finally {
      fixture.disposeGate.resolve()
      for (const hook of fixture.after) await hook()
      expect.soft({ calls: fixture.disposeCalls, disposed: fixture.disposed }).toEqual({ calls: 1, disposed: true })
    }
  })
})
