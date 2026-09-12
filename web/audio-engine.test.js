// @vitest-environment node
import { describe, it, expect, vi } from 'vitest'
import { AudioEngine } from './audio-engine.js'
import { createAudioRestore } from './restore-audio.js'

// minimal fake WebAudio context for engine tests
function fakeContext() {
  const destinations = []
  const sources = []
  const gains = []
  const ctx = {
    currentTime: 0,
    destination: { kind: 'dest' },
    createGain: () => {
      const g = { gain: { value: 1 }, connect: vi.fn(), disconnect: vi.fn() }
      gains.push(g)
      return g
    },
    createBufferSource: () => {
      const s = { buffer: null, loop: false, connect: vi.fn(), start: vi.fn(), stop: vi.fn(), disconnect: vi.fn() }
      sources.push(s)
      return s
    },
    decodeAudioData: vi.fn(async () => ({ duration: 5, length: 240000, sampleRate: 48000, numberOfChannels: 2 })),
    suspend: vi.fn(async () => {}),
    resume: vi.fn(async () => {}),
    close: vi.fn(async () => {}),
  }
  return { ctx, sources, destinations, gains }
}

// engine wired to a fake context + a fetch that returns a decodable blob
function mkEngine() {
  const { ctx, sources, gains } = fakeContext()
  const eng = new AudioEngine({ ctx })
  globalThis.fetch = vi.fn(async () => new Response(new Uint8Array(8)))
  return { ctx, sources, gains, eng }
}

describe('AudioEngine', () => {
  it('builds the three SoLoud buses on init', () => {
    const { ctx } = fakeContext()
    const eng = new AudioEngine({ ctx })
    expect(eng.ready).toBe(true)
    expect(eng._busGains.has('bgm')).toBe(true)
    expect(eng._busGains.has('se')).toBe(true)
    expect(eng._busGains.has('voice')).toBe(true)
  })

  it('loads, plays and reports isPlaying on a bus', async () => {
    const { ctx, sources } = fakeContext()
    const eng = new AudioEngine({ ctx })
    globalThis.fetch = vi.fn(async () => new Response(new Uint8Array(8)))
    const ok = await eng.play('bgm', 'assets/bgm/daily.wav', { assetUrl: (p) => 'http://x/' + p })
    expect(ok).toBe(true)
    expect(sources[0].buffer).toBeTruthy()
    expect(eng.isPlaying('bgm')).toBe(true)
    ctx.currentTime = 10; // past the 5s clip
    expect(eng.isPlaying('bgm')).toBe(false)
    eng.stopAll()
    expect(eng.isPlaying('bgm')).toBe(false)
  })

  it('stop cuts the source and disconnects', async () => {
    const { ctx, sources } = fakeContext()
    const eng = new AudioEngine({ ctx })
    globalThis.fetch = vi.fn(async () => new Response(new Uint8Array(8)))
    await eng.play('voice', 'v.wav', { assetUrl: 'http://x/' })
    eng.stop('voice')
    expect(eng._sources.size).toBe(0)
    expect(eng.isPlaying('voice')).toBe(false)
  })

  it('caches decoded buffers per path', async () => {
    const { ctx } = fakeContext()
    const eng = new AudioEngine({ ctx })
    globalThis.fetch = vi.fn(async () => new Response(new Uint8Array(8)))
    await eng.play('bgm', 'a.wav', { assetUrl: 'http://x/' })
    await eng.play('se', 'a.wav', { assetUrl: 'http://x/' })
    expect(globalThis.fetch).toHaveBeenCalledTimes(1)
  })

  it('degrades without an AudioContext (jsdom/headless)', async () => {
    const eng = new AudioEngine({});
    expect(eng.ready).toBe(false)
    const ok = await eng.play('bgm', 'x.wav', { assetUrl: 'http://x/' })
    expect(ok).toBe(false)
    expect(eng.isPlaying('bgm')).toBe(false)
  })

// =============================================================
// round 83 deep coverage — bus routing
// =============================================================
describe('AudioEngine · bus routing', () => {
  it('routes a source onto its own bus gain (not the destination)', async () => {
    const { ctx, eng } = mkEngine()
    await eng.play('bgm', 'a.wav', { assetUrl: 'http://x/' })
    const rec = eng._sources.get('bgm')
    expect(rec.gain).toBe(eng._busGains.get('bgm'))
    expect(rec.gain).not.toBe(ctx.destination)
    expect(rec.source.connect).toHaveBeenCalledWith(rec.clipGain)
    expect(rec.clipGain.connect).toHaveBeenCalledWith(eng._busGains.get('bgm'))
  })

  it('plays all three kinds concurrently on independent buses', async () => {
    const eng = mkEngine().eng
    await eng.play('bgm', 'bgm.wav', { assetUrl: 'http://x/' })
    await eng.play('se', 'se.wav', { assetUrl: 'http://x/' })
    await eng.play('voice', 'v.wav', { assetUrl: 'http://x/' })
    expect(eng._sources.size).toBe(3)
    expect(eng._sources.get('bgm').gain).toBe(eng._busGains.get('bgm'))
    expect(eng._sources.get('se').gain).toBe(eng._busGains.get('se'))
    expect(eng._sources.get('voice').gain).toBe(eng._busGains.get('voice'))
    for (const k of ['bgm', 'se', 'voice']) expect(eng.isPlaying(k)).toBe(true)
  })

  it('same-kind play cuts the previous source (one live source per kind)', async () => {
    const { sources, eng } = mkEngine()
    await eng.play('bgm', 'a.wav', { assetUrl: 'http://x/' })
    const first = eng._sources.get('bgm').source
    await eng.play('bgm', 'b.wav', { assetUrl: 'http://x/' })
    const second = eng._sources.get('bgm').source
    expect(first).not.toBe(second)
    expect(first.stop).toHaveBeenCalled()
    expect(eng._sources.size).toBe(1)
    expect(sources.length).toBe(2)
    // the replacement source is the live one
    expect(eng._sources.get('bgm').source).toBe(second)
  })

  it('muting one bus does not affect the others', () => {
    const eng = mkEngine().eng
    eng.setBusVolume('se', 0)
    expect(eng._busGains.get('se').gain.value).toBe(0)
    expect(eng._busGains.get('bgm').gain.value).toBe(1)
    expect(eng._busGains.get('voice').gain.value).toBe(1)
  })
})

// =============================================================
// round 83 deep coverage — volume control
// =============================================================
describe('AudioEngine · volume control', () => {
  it('setBusVolume is per-bus independent', () => {
    const eng = mkEngine().eng
    eng.setBusVolume('bgm', 0.5)
    eng.setBusVolume('se', 0.25)
    eng.setBusVolume('voice', 0.8)
    expect(eng._busGains.get('bgm').gain.value).toBe(0.5)
    expect(eng._busGains.get('se').gain.value).toBe(0.25)
    expect(eng._busGains.get('voice').gain.value).toBe(0.8)
  })

  it('accepts 0, 1 and mid values', () => {
    const eng = mkEngine().eng
    eng.setBusVolume('bgm', 0)
    expect(eng._busGains.get('bgm').gain.value).toBe(0)
    eng.setBusVolume('bgm', 1)
    expect(eng._busGains.get('bgm').gain.value).toBe(1)
    eng.setBusVolume('bgm', 0.35)
    expect(eng._busGains.get('bgm').gain.value).toBe(0.35)
  })

  it('global mute mutes all three buses', () => {
    const eng = mkEngine().eng
    for (const k of ['bgm', 'se', 'voice']) eng.setBusVolume(k, 0)
    for (const k of ['bgm', 'se', 'voice']) expect(eng._busGains.get(k).gain.value).toBe(0)
  })

  it('user bus volume persists while explicit play volume controls only the clip', async () => {
    const eng = mkEngine().eng
    eng.setBusVolume('bgm', 0.6)
    await eng.play('bgm', 'a.wav', { assetUrl: 'http://x/' })
    expect(eng._busGains.get('bgm').gain.value).toBe(0.6)
    await eng.play('bgm', 'b.wav', { assetUrl: 'http://x/', volume: 0.2 })
    expect(eng._busGains.get('bgm').gain.value).toBe(0.6)
    expect(eng._sources.get('bgm').clipGain.gain.value).toBe(0.2)
  })

  it('play accepts an explicit muted (0) volume', async () => {
    const eng = mkEngine().eng
    await eng.play('bgm', 'a.wav', { assetUrl: 'http://x/', volume: 0 })
    expect(eng._busGains.get('bgm').gain.value).toBe(1)
    expect(eng._sources.get('bgm').clipGain.gain.value).toBe(0)
  })
})

// =============================================================
// round 83 deep coverage — lifecycle
// =============================================================
describe('AudioEngine · lifecycle', () => {
  it('suspend/resume delegate to the AudioContext', () => {
    const { ctx, eng } = mkEngine()
    eng.suspend()
    eng.resume()
    expect(ctx.suspend).toHaveBeenCalledTimes(1)
    expect(ctx.resume).toHaveBeenCalledTimes(1)
  })

  it('destroy stops all sources, closes the context and clears state', async () => {
    const { ctx, eng } = mkEngine()
    await eng.play('bgm', 'a.wav', { assetUrl: 'http://x/' })
    await eng.play('se', 'b.wav', { assetUrl: 'http://x/' })
    eng.destroy()
    expect(ctx.close).toHaveBeenCalledTimes(1)
    expect(eng.ready).toBe(false)
    expect(eng._ctx).toBeNull()
    expect(eng._sources.size).toBe(0)
    expect(eng._busGains.size).toBe(0)
    expect(eng._buffers.size).toBe(0)
  })

  it('calls after destroy degrade safely without throwing', async () => {
    const eng = mkEngine().eng
    eng.destroy()
    await expect(eng.play('bgm', 'a.wav', { assetUrl: 'http://x/' })).resolves.toBe(false)
    expect(() => eng.stop('bgm')).not.toThrow()
    expect(eng.isPlaying('bgm')).toBe(false)
    expect(() => eng.setBusVolume('bgm', 0.5)).not.toThrow()
    expect(() => eng.stopAll()).not.toThrow()
    expect(() => eng.suspend()).not.toThrow()
    expect(() => eng.resume()).not.toThrow()
  })

  it('double destroy is safe', () => {
    const eng = mkEngine().eng
    eng.destroy()
    expect(() => eng.destroy()).not.toThrow()
  })

  it('ensureContext re-initializes the three buses after destroy', async () => {
    const eng = mkEngine().eng
    eng.destroy()
    let created = 0
    const fakeCtor = function () { created++; return fakeContext().ctx }
    const prev = globalThis.AudioContext
    globalThis.AudioContext = fakeCtor
    try {
      const ctx2 = eng.ensureContext()
      expect(created).toBe(1)
      expect(ctx2).toBeTruthy()
      expect(eng.ready).toBe(true)
      expect(eng._busGains.has('bgm')).toBe(true)
      expect(eng._busGains.has('se')).toBe(true)
      expect(eng._busGains.has('voice')).toBe(true)
      await expect(eng.play('bgm', 'a.wav', { assetUrl: 'http://x/' })).resolves.toBe(true)
    } finally {
      if (prev === undefined) delete globalThis.AudioContext
      else globalThis.AudioContext = prev
    }
  })
})

// =============================================================
// round 83 deep coverage — resource tolerance
// =============================================================
describe('AudioEngine · resource tolerance', () => {
  it('fetch rejection degrades to false without throwing', async () => {
    const eng = mkEngine().eng
    globalThis.fetch = vi.fn(async () => { throw new Error('network down') })
    await expect(eng.play('bgm', 'a.wav', { assetUrl: 'http://x/' })).resolves.toBe(false)
    expect(eng.isPlaying('bgm')).toBe(false)
    expect(eng._sources.size).toBe(0)
  })

  it('a non-ok HTTP response degrades to false', async () => {
    const eng = mkEngine().eng
    globalThis.fetch = vi.fn(async () => ({ ok: false, status: 404 }))
    await expect(eng.play('se', 'a.wav', { assetUrl: 'http://x/' })).resolves.toBe(false)
    expect(eng.isPlaying('se')).toBe(false)
  })

  it('a failed load is not cached, so a later retry can succeed', async () => {
    const eng = mkEngine().eng
    globalThis.fetch = vi.fn()
      .mockRejectedValueOnce(new Error('first load fails'))
      .mockResolvedValueOnce(new Response(new Uint8Array(8)))
    await expect(eng.play('bgm', 'a.wav', { assetUrl: 'http://x/' })).resolves.toBe(false)
    await expect(eng.play('bgm', 'a.wav', { assetUrl: 'http://x/' })).resolves.toBe(true)
    expect(eng._buffers.has('a.wav')).toBe(true)
    expect(globalThis.fetch).toHaveBeenCalledTimes(2)
  })

  it('decodeAudioData rejection degrades to false', async () => {
    const eng = mkEngine().eng
    eng._ctx.decodeAudioData = vi.fn(async () => { throw new Error('decode fail') })
    await expect(eng.play('voice', 'a.wav', { assetUrl: 'http://x/' })).resolves.toBe(false)
  })

  it('an empty (null) decoded buffer is treated as unplayable', async () => {
    const eng = mkEngine().eng
    eng._ctx.decodeAudioData = vi.fn(async () => null)
    await expect(eng.play('bgm', 'a.wav', { assetUrl: 'http://x/' })).resolves.toBe(false)
  })

  it('stop on a kind with no active source is a safe no-op', () => {
    const eng = mkEngine().eng
    expect(() => eng.stop('se')).not.toThrow()
    expect(eng._sources.size).toBe(0)
    expect(() => eng.stopAll()).not.toThrow()
  })
})

// =============================================================
// round 83 deep coverage — state queries & events
// =============================================================
describe('AudioEngine · state queries', () => {
  it('isPlaying is kind-independent and clears after the clip ends', async () => {
    const { ctx, eng } = mkEngine()
    await eng.play('bgm', 'a.wav', { assetUrl: 'http://x/' })
    await eng.play('voice', 'v.wav', { assetUrl: 'http://x/' })
    expect(eng.isPlaying('bgm')).toBe(true)
    expect(eng.isPlaying('voice')).toBe(true)
    ctx.currentTime = 6 // past the 5s clip
    expect(eng.isPlaying('bgm')).toBe(false)
    expect(eng.isPlaying('voice')).toBe(false)
  })

  it('stop(kind) only halts that kind, leaving others playing', async () => {
    const eng = mkEngine().eng
    await eng.play('bgm', 'a.wav', { assetUrl: 'http://x/' })
    await eng.play('se', 'b.wav', { assetUrl: 'http://x/' })
    eng.stop('se')
    expect(eng.isPlaying('se')).toBe(false)
    expect(eng.isPlaying('bgm')).toBe(true)
  })

  it('stopAll clears every kind', async () => {
    const eng = mkEngine().eng
    await eng.play('bgm', 'a.wav', { assetUrl: 'http://x/' })
    await eng.play('se', 'b.wav', { assetUrl: 'http://x/' })
    await eng.play('voice', 'c.wav', { assetUrl: 'http://x/' })
    eng.stopAll()
    expect(eng._sources.size).toBe(0)
    for (const k of ['bgm', 'se', 'voice']) expect(eng.isPlaying(k)).toBe(false)
  })

  it('the onended event drops the finished source from the active map', async () => {
    const eng = mkEngine().eng
    await eng.play('bgm', 'a.wav', { assetUrl: 'http://x/' })
    const src = eng._sources.get('bgm').source
    expect(typeof src.onended).toBe('function')
    src.onended()
    expect(eng.isPlaying('bgm')).toBe(false)
    expect(eng._sources.get('bgm')).toBeUndefined()
    // a stale onended from a superseded source must not clear a newer one
  })

  it('stale onended from a replaced source does not clear the newer source', async () => {
    const eng = mkEngine().eng
    await eng.play('bgm', 'a.wav', { assetUrl: 'http://x/' })
    const first = eng._sources.get('bgm').source
    await eng.play('bgm', 'b.wav', { assetUrl: 'http://x/' })
    const second = eng._sources.get('bgm').source
    first.onended() // late end from the replaced source
    expect(eng.isPlaying('bgm')).toBe(true)
    expect(eng._sources.get('bgm').source).toBe(second)
  })
})

// =============================================================
// round 83 — bridge integration contract
// =============================================================
describe('AudioEngine · bridge contract', () => {
  it('exposes the method surface createPlayer/backend relies on', () => {
    const eng = new AudioEngine({})
    for (const m of ['play', 'stop', 'isPlaying', 'setBusVolume', 'stopAll', 'suspend', 'resume', 'destroy']) {
      expect(typeof eng[m]).toBe('function')
    }
  })

  it('the backend.audio_play call shape (kind, file, { volume, assetUrl }) routes correctly', async () => {
    // bridge.js: backend.audio_play(kind, file, opts) -> audio.play(kind, file, { volume: opts.volume, assetUrl })
    const { eng } = mkEngine()
    const kind = 'bgm', file = 'music/track1.wav'
    await eng.play(kind, file, { volume: 0.4, assetUrl: (p) => 'http://cdn/' + p })
    expect(eng.isPlaying('bgm')).toBe(true)
    expect(eng._busGains.get('bgm').gain.value).toBe(1)
    expect(eng._sources.get('bgm').clipGain.gain.value).toBe(0.4)
    // backend.audio_stop(kind) -> audio.stop(kind)
    eng.stop(kind)
    expect(eng.isPlaying(kind)).toBe(false)
    // backend.audio_set_bus_volume(kind, v) -> audio.setBusVolume(kind, v)
    eng.setBusVolume('voice', 0.9)
    expect(eng._busGains.get('voice').gain.value).toBe(0.9)
    // backend.audio_is_playing(kind) -> audio.isPlaying(kind)
    await eng.play('se', 'sfx.wav', { assetUrl: 'http://cdn/' })
    expect(eng.isPlaying('se')).toBe(true)
  })

  it('exposes unlock/state for the W1 lifecycle contract', () => {
    const eng = new AudioEngine({})
    expect(typeof eng.unlock).toBe('function')
    expect(typeof eng.state).toBe('string')
  })
})

// =============================================================
// plan W1 — WebAudio autoplay lifecycle (user-gesture unlock)
// =============================================================
describe('AudioEngine · autoplay unlock (W1)', () => {
  // fake ctx pinned to 'suspended' with resume() that flips it to running
  function suspendedEngine() {
    const { ctx, eng } = mkEngine()
    ctx.state = 'suspended'
    ctx.resume = vi.fn(async () => { ctx.state = 'running' })
    return { ctx, eng }
  }

  it('reports ctx state through the state getter', async () => {
    const { ctx, eng } = suspendedEngine()
    expect(eng.state).toBe('suspended')
    ctx.state = 'running'
    expect(eng.state).toBe('running')
    eng.destroy()
    expect(eng.state).toBe('none')
  })

  it('unlock resumes a suspended context (initial suspended -> running)', async () => {
    const { ctx, eng } = suspendedEngine()
    expect(eng.state).toBe('suspended')
    await expect(eng.unlock()).resolves.toBe(true)
    expect(eng.state).toBe('running')
    expect(ctx.resume).toHaveBeenCalledTimes(1)
  })

  it('repeated unlock is safe and does not resume a running context twice', async () => {
    const { ctx, eng } = suspendedEngine()
    await eng.unlock()
    await eng.unlock()
    await eng.unlock()
    expect(ctx.resume).toHaveBeenCalledTimes(1)
    expect(eng.state).toBe('running')
  })

  it('repeat resume calls (engine.resume) are safe', async () => {
    const { ctx, eng } = suspendedEngine()
    await expect(Promise.resolve(eng.resume())).resolves.toBeUndefined()
    ctx.state = 'running'
    await expect(Promise.resolve(eng.resume())).resolves.toBeUndefined()
    expect(eng.state).toBe('running')
  })

  it('background recovery: a re-suspended context unlocks again on tab return', async () => {
    const { ctx, eng } = suspendedEngine()
    await eng.unlock()
    // browser/OS suspend puts the context back to suspended
    ctx.state = 'suspended'
    ctx.resume.mockClear()
    await expect(eng.unlock()).resolves.toBe(true)
    expect(ctx.resume).toHaveBeenCalledTimes(1)
    expect(eng.state).toBe('running')
  })

  it('unlock creates the context lazily on first gesture and resumes it', async () => {
    const { ctx } = suspendedEngine()
    let created = 0
    const fakeCtor = function () { created++; return ctx }
    const prev = globalThis.AudioContext
    globalThis.AudioContext = fakeCtor
    try {
      const eng = new AudioEngine({})
      expect(eng.state).toBe('none')
      await expect(eng.unlock()).resolves.toBe(true)
      expect(created).toBe(1)
      expect(eng.state).toBe('running')
    } finally {
      if (prev === undefined) delete globalThis.AudioContext
      else globalThis.AudioContext = prev
    }
  })

  it('unlock degrades to false without an AudioContext (headless)', async () => {
    const eng = new AudioEngine({})
    await expect(eng.unlock()).resolves.toBe(false)
    // and it never throws even after destroy
    eng.destroy()
    await expect(eng.unlock()).resolves.toBe(false)
  })

  it('unlock ignores a resume() rejection (transient gesture failure)', async () => {
    const { ctx, eng } = suspendedEngine()
    ctx.resume = vi.fn(async () => { throw new Error('not allowed yet') })
    await expect(eng.unlock()).resolves.toBe(false)
    expect(eng.state).toBe('suspended')
    // a later attempt succeeds once the gesture is trusted
    ctx.resume = vi.fn(async () => { ctx.state = 'running' })
    await expect(eng.unlock()).resolves.toBe(true)
  })

  it.each(['resolve', 'reject'])('U20: late old-context resume %s cannot borrow a replacement context', async completion => {
    const first = fakeContext()
    first.ctx.state = 'suspended'
    let resolveResume, rejectResume
    const resume = new Promise((resolve, reject) => { resolveResume = resolve; rejectResume = reject })
    first.ctx.resume = vi.fn(() => resume)
    first.ctx.close = vi.fn(async () => { first.ctx.state = 'closed' })
    const engine = new AudioEngine({ ctx: first.ctx,
      fetchImpl: async () => new Response(new Uint8Array(8)) })
    const replacement = fakeContext()
    replacement.ctx.state = 'running'
    const previous = globalThis.AudioContext
    globalThis.AudioContext = function () { return replacement.ctx }
    try {
      const oldUnlock = engine.unlock()
      expect(first.ctx.resume).toHaveBeenCalledOnce()
      engine.destroy()
      expect(engine.ensureContext()).toBe(replacement.ctx)
      await expect(engine.unlock()).resolves.toBe(true)
      await expect(engine.play('bgm', 'replacement.wav', { loop: true })).resolves.toBe(true)
      const current = engine._sources.get('bgm')

      if (completion === 'resolve') resolveResume()
      else rejectResume(new Error('old context closed during resume'))
      await expect(oldUnlock).resolves.toBe(false)
      expect(engine._ctx).toBe(replacement.ctx)
      expect(engine._sources.get('bgm')).toBe(current)
      replacement.ctx.currentTime = 20
      expect(engine.isPlaying('bgm')).toBe(true)
      expect(current.source.stop).not.toHaveBeenCalled()
      expect(replacement.ctx.close).not.toHaveBeenCalled()
      expect(first.ctx.close).toHaveBeenCalledOnce()
    } finally {
      resolveResume()
      engine.destroy()
      if (previous === undefined) delete globalThis.AudioContext
      else globalThis.AudioContext = previous
    }
  })

  it('U20: unlock contains AudioContext construction failure and allows a later gesture', async () => {
    const engine = new AudioEngine()
    const previous = globalThis.AudioContext
    const rejected = vi.fn(function () { throw new Error('AudioContext construction refused') })
    globalThis.AudioContext = rejected
    try {
      await expect(engine.unlock()).resolves.toBe(false)
      expect(rejected).toHaveBeenCalledOnce()
      expect(engine.state).toBe('none')
      expect(engine.ready).toBe(false)
      expect(engine._sources.size).toBe(0)

      const recovered = fakeContext()
      recovered.ctx.state = 'suspended'
      recovered.ctx.resume = vi.fn(async () => { recovered.ctx.state = 'running' })
      globalThis.AudioContext = function () { return recovered.ctx }
      await expect(engine.unlock()).resolves.toBe(true)
      expect(recovered.ctx.resume).toHaveBeenCalledOnce()
      expect(engine._ctx).toBe(recovered.ctx)
      expect(engine._busGains.size).toBe(3)
      expect(engine.ready).toBe(true)
    } finally {
      engine.destroy()
      if (previous === undefined) delete globalThis.AudioContext
      else globalThis.AudioContext = previous
    }
  })
})

describe('AudioEngine · U20 gain lifecycle', () => {
  function fixture({ hold = true } = {}) {
    const f = fakeContext()
    f.ctx.state = 'running'
    f.ctx.createGain = vi.fn(() => {
      const param = {
        value: 1,
        setValueAtTime: vi.fn(value => { param.value = value; return param }),
        linearRampToValueAtTime: vi.fn(() => param),
        cancelScheduledValues: vi.fn(() => param),
      }
      if (hold) param.cancelAndHoldAtTime = vi.fn(() => param)
      const node = { gain: param, connect: vi.fn(), disconnect: vi.fn() }
      f.gains.push(node)
      return node
    })
    const fetchImpl = vi.fn(async () => new Response(new Uint8Array(8)))
    const engine = new AudioEngine({ ctx: f.ctx, fetchImpl })
    return { ...f, engine, fetchImpl }
  }
  const defer = () => {
    let resolve, reject
    const promise = new Promise((yes, no) => { resolve = yes; reject = no })
    return { promise, resolve, reject }
  }

  it('fades the clip from zero on the audio clock without changing its user bus', async () => {
    const { engine, ctx } = fixture()
    ctx.currentTime = 2
    engine.setBusVolume('bgm', 0.6)
    expect(await engine.play('bgm', 'fade-in.wav', { volume: 0.2, fadein: 2, loop: true })).toBe(true)
    const clip = engine._sources.get('bgm')
    expect(clip.clipGain.gain.setValueAtTime).toHaveBeenCalledWith(0, 2)
    expect(clip.clipGain.gain.linearRampToValueAtTime).toHaveBeenCalledWith(0.2, 4)
    expect(engine.captureBgm().gain).toBe(0)
    ctx.currentTime = 3
    expect(engine.captureBgm().gain).toBeCloseTo(0.1)
    ctx.currentTime = 4
    expect(engine.captureBgm().gain).toBeCloseTo(0.2)
    ctx.currentTime = 20
    expect(engine.captureBgm().gain).toBeCloseTo(0.2)
    expect(engine.isPlaying('bgm')).toBe(true)
    expect(engine._busVolumes.get('bgm')).toBe(0.6)
    expect(engine._busGains.get('bgm').gain.value).toBe(0.6)
    expect(engine._busGains.get('voice').gain.linearRampToValueAtTime).not.toHaveBeenCalled()
  })

  it('zero duration applies clip gain and stops immediately without a retiring owner', async () => {
    const { engine } = fixture()
    expect(await engine.play('bgm', 'immediate.wav', { volume: 0.4, fadein: 0 })).toBe(true)
    const clip = engine._sources.get('bgm')
    expect(engine.captureBgm().gain).toBe(0.4)
    expect(clip.clipGain.gain.linearRampToValueAtTime).not.toHaveBeenCalled()
    expect(engine.stop('bgm', { fadeout: 0 })).toBe(true)
    expect(engine.isPlaying('bgm')).toBe(false)
    expect(engine._retiring.size).toBe(0)
    expect(clip.source.stop).toHaveBeenCalledOnce()
    expect(clip.source.disconnect).toHaveBeenCalledOnce()
    expect(clip.clipGain.disconnect).toHaveBeenCalledOnce()
    expect(engine.stop('bgm')).toBe(true)
    expect(clip.source.stop).toHaveBeenCalledOnce()
  })

  it.each([-1, NaN, Infinity, -Infinity, '2'])('rejects invalid duration %s before changing an owner or bus target', async duration => {
    const { engine } = fixture()
    expect(await engine.play('bgm', 'kept.wav', { volume: 0.4, loop: true })).toBe(true)
    engine.setBusVolume('bgm', 0.6)
    const clip = engine._sources.get('bgm')
    expect(await engine.play('bgm', 'rejected.wav', { fadein: duration })).toBe(false)
    expect(engine.stop('bgm', { fadeout: duration })).toBe(false)
    expect(engine.fadeVolume('bgm', 0.2, duration)).toBe(false)
    expect(engine._sources.get('bgm')).toBe(clip)
    expect(clip.source.stop).not.toHaveBeenCalled()
    expect(engine._busVolumes.get('bgm')).toBe(0.6)
    expect(engine._retiring.size).toBe(0)
  })

  it('unknown buses and invalid fade targets do not publish a request', async () => {
    const { engine } = fixture()
    const bus = engine._busGains.get('bgm').gain
    expect(await engine.play('unknown', 'x.wav', { fadein: 1 })).toBe(false)
    expect(engine.stop('unknown', { fadeout: 1 })).toBe(false)
    expect(engine.fadeVolume('unknown', 0.5, 1)).toBe(false)
    for (const value of [-1, 17, NaN, Infinity, '0.5']) {
      expect(engine.fadeVolume('bgm', value, 1)).toBe(false)
    }
    expect(bus.cancelAndHoldAtTime).not.toHaveBeenCalled()
    expect(bus.linearRampToValueAtTime).not.toHaveBeenCalled()
    expect(engine._busVolumes.get('bgm')).toBe(1)
    expect(engine._sources.size).toBe(0)
  })

  it('bus fades re-anchor at the held value and preserve clip gain and other buses', async () => {
    const { engine, ctx } = fixture()
    engine.setBusVolume('bgm', 0.6)
    expect(await engine.play('bgm', 'bus.wav', { volume: 0.2, loop: true })).toBe(true)
    const bus = engine._busGains.get('bgm').gain
    expect(engine.fadeVolume('bgm', 0.3, 4)).toBe(true)
    expect(engine._busVolumes.get('bgm')).toBe(0.3)
    expect(bus.setValueAtTime).toHaveBeenLastCalledWith(0.6, 0)
    expect(bus.linearRampToValueAtTime).toHaveBeenLastCalledWith(0.3, 4)
    ctx.currentTime = 2
    expect(engine.fadeVolume('bgm', 0.9, 2)).toBe(true)
    expect(bus.cancelAndHoldAtTime).toHaveBeenLastCalledWith(2)
    expect(bus.setValueAtTime).toHaveBeenLastCalledWith(expect.closeTo(0.45), 2)
    expect(bus.linearRampToValueAtTime).toHaveBeenLastCalledWith(0.9, 4)
    expect(engine._busVolumes.get('bgm')).toBe(0.9)
    expect(engine.captureBgm().gain).toBe(0.2)
    expect(engine._busGains.get('se').gain.value).toBe(1)
    expect(engine._busGains.get('se').gain.linearRampToValueAtTime).not.toHaveBeenCalled()
  })

  it('fallback cancellation re-anchors the current ramp without cancelAndHoldAtTime', () => {
    const { engine, ctx } = fixture({ hold: false })
    engine.setBusVolume('bgm', 0.6)
    const bus = engine._busGains.get('bgm').gain
    expect(engine.fadeVolume('bgm', 0.2, 4)).toBe(true)
    ctx.currentTime = 1
    expect(engine.fadeVolume('bgm', 0.8, 2)).toBe(true)
    expect(bus.cancelScheduledValues).toHaveBeenLastCalledWith(1)
    expect(bus.setValueAtTime).toHaveBeenLastCalledWith(expect.closeTo(0.5), 1)
    expect(bus.linearRampToValueAtTime).toHaveBeenLastCalledWith(0.8, 3)
  })

  it('a direct user setter cancels a bus fade and becomes the next ramp origin', () => {
    const { engine, ctx } = fixture()
    const bus = engine._busGains.get('bgm').gain
    expect(engine.fadeVolume('bgm', 0, 4)).toBe(true)
    ctx.currentTime = 2
    engine.setBusVolume('bgm', 0.8)
    expect(bus.cancelAndHoldAtTime).toHaveBeenLastCalledWith(2)
    expect(bus.value).toBe(0.8)
    expect(engine._busVolumes.get('bgm')).toBe(0.8)
    ctx.currentTime = 3
    expect(engine.fadeVolume('bgm', 0.4, 2)).toBe(true)
    expect(bus.setValueAtTime).toHaveBeenLastCalledWith(0.8, 3)
    expect(bus.linearRampToValueAtTime).toHaveBeenLastCalledWith(0.4, 5)
    expect(engine.fadeVolume('bgm', 0.7, 0)).toBe(true)
    expect(bus.value).toBe(0.7)
    expect(engine._busVolumes.get('bgm')).toBe(0.7)
  })

  it('a timed fade needs a live context and is not a pending preference update', () => {
    const engine = new AudioEngine()
    engine.setBusVolume('bgm', 0.25)
    const previous = globalThis.AudioContext
    const construct = vi.fn(function () { return fakeContext().ctx })
    globalThis.AudioContext = construct
    try {
      expect(engine.fadeVolume('bgm', 0.8, 2)).toBe(false)
      expect(construct).not.toHaveBeenCalled()
      expect(engine._busVolumes.get('bgm')).toBe(0.25)
      expect(engine.state).toBe('none')
    } finally {
      if (previous === undefined) delete globalThis.AudioContext
      else globalThis.AudioContext = previous
    }
    const f = fixture()
    f.ctx.state = 'closed'
    expect(f.engine.fadeVolume('bgm', 0.5, 1)).toBe(false)
    expect(f.engine._busVolumes.get('bgm')).toBe(1)
  })

  it('a fade-out logically stops but retains its physical tail on the audio clock', async () => {
    const { engine, ctx } = fixture()
    engine.setBusVolume('bgm', 0.6)
    expect(await engine.play('bgm', 'tail.wav', { volume: 0.8, fadein: 2, loop: true })).toBe(true)
    const clip = engine._sources.get('bgm')
    ctx.currentTime = 1
    expect(engine.stop('bgm', { fadeout: 2 })).toBe(true)
    expect(engine._sources.has('bgm')).toBe(false)
    expect(engine.captureBgm()).toBe(false)
    expect(engine._retiring.has(clip)).toBe(true)
    expect(clip.clipGain.gain.setValueAtTime).toHaveBeenLastCalledWith(0.4, 1)
    expect(clip.clipGain.gain.linearRampToValueAtTime).toHaveBeenLastCalledWith(0, 3)
    expect(clip.source.stop).toHaveBeenCalledWith(3)
    expect(clip.source.disconnect).not.toHaveBeenCalled()
    expect(clip.clipGain.disconnect).not.toHaveBeenCalled()
    vi.useFakeTimers()
    try {
      ctx.state = 'suspended'
      await vi.advanceTimersByTimeAsync(30000)
      expect(engine._retiring.has(clip)).toBe(true)
      expect(clip.source.disconnect).not.toHaveBeenCalled()
    } finally { vi.useRealTimers() }
    ctx.state = 'running'
    ctx.currentTime = 3
    clip.source.onended()
    expect(engine._retiring.size).toBe(0)
    expect(clip.source.disconnect).toHaveBeenCalledOnce()
    expect(clip.clipGain.disconnect).toHaveBeenCalledOnce()
    expect(engine._busVolumes.get('bgm')).toBe(0.6)
  })

  it('repeated fade stop cannot extend an old tail and a hard stop releases it once', async () => {
    const { engine, ctx } = fixture()
    await engine.play('bgm', 'repeat.wav', { loop: true })
    const clip = engine._sources.get('bgm')
    expect(engine.stop('bgm', { fadeout: 2 })).toBe(true)
    ctx.currentTime = 1
    expect(engine.stop('bgm', { fadeout: 8 })).toBe(false)
    expect(clip.source.stop).toHaveBeenCalledTimes(1)
    expect(clip.source.stop).toHaveBeenLastCalledWith(2)
    expect(engine.stop('bgm')).toBe(true)
    expect(engine.stop('bgm')).toBe(true)
    expect(engine._retiring.size).toBe(0)
    expect(clip.source.stop).toHaveBeenCalledTimes(2)
    expect(clip.source.disconnect).toHaveBeenCalledOnce()
  })

  it('a retired source ending never clears or stops its replacement', async () => {
    const { engine } = fixture()
    await engine.play('bgm', 'old.wav', { loop: true })
    const old = engine._sources.get('bgm')
    expect(engine.stop('bgm', { fadeout: 2 })).toBe(true)
    await engine.play('bgm', 'new.wav', { loop: true })
    const current = engine._sources.get('bgm')
    old.source.onended()
    old.source.onended()
    expect(engine._retiring.size).toBe(0)
    expect(engine._sources.get('bgm')).toBe(current)
    expect(current.source.stop).not.toHaveBeenCalled()
    expect(old.source.disconnect).toHaveBeenCalledOnce()
  })

  it.each(['stopAll', 'destroy'])('%s releases active and retiring clips, including stale end callbacks', async action => {
    const { engine } = fixture()
    await engine.play('bgm', 'old.wav', { loop: true })
    const retired = engine._sources.get('bgm')
    engine.stop('bgm', { fadeout: 10 })
    await engine.play('bgm', 'new.wav', { loop: true })
    const active = engine._sources.get('bgm')
    await engine.play('voice', 'voice.wav')
    const voice = engine._sources.get('voice')
    engine[action]()
    expect(engine._sources.size).toBe(0)
    expect(engine._retiring.size).toBe(0)
    for (const record of [retired, active, voice]) {
      record.source.onended()
      expect(record.source.disconnect).toHaveBeenCalledOnce()
      expect(record.clipGain.disconnect).toHaveBeenCalledOnce()
    }
    expect(retired.source.stop).toHaveBeenCalledTimes(2)
  })

  it.each(['live bus ramp', 'new user volume'])('v1 restore freezes clip gain while preserving %s', async mode => {
    const f = fixture()
    const { engine, ctx } = f
    const restore = createAudioRestore({ audio: engine, fetchImpl: f.fetchImpl })
    engine.setBusVolume('bgm', 0.7)
    await engine.play('bgm', 'restore.wav', { volume: 0.8, fadein: 4, loop: true })
    const old = engine._sources.get('bgm')
    expect(engine.fadeVolume('bgm', 0.3, 8)).toBe(true)
    ctx.currentTime = 1
    const snapshot = restore.capture_audio()
    expect(snapshot.bgm.gain).toBeCloseTo(0.2)
    const ticket = await restore.prepare_audio(snapshot)
    expect(old.source.stop).not.toHaveBeenCalled()
    engine.stop('bgm', { fadeout: 4 })
    const bus = engine._busGains.get('bgm').gain
    if (mode === 'new user volume') engine.setBusVolume('bgm', 0.9)
    const cancellations = bus.cancelAndHoldAtTime.mock.calls.length
    const ramps = bus.linearRampToValueAtTime.mock.calls.length
    expect(restore.apply_audio(ticket)).toBe(true)
    const current = engine._sources.get('bgm')
    expect(current).not.toBe(old)
    expect(engine._busGains.get('bgm').gain).toBe(bus)
    expect(engine._retiring.size).toBe(0)
    expect(current.clipGain.gain.value).toBeCloseTo(0.2)
    expect(current.clipGain.gain.linearRampToValueAtTime).not.toHaveBeenCalled()
    expect(bus.cancelAndHoldAtTime).toHaveBeenCalledTimes(cancellations)
    expect(bus.linearRampToValueAtTime).toHaveBeenCalledTimes(ramps)
    expect(engine._busVolumes.get('bgm')).toBe(mode === 'new user volume' ? 0.9 : 0.3)
    ctx.currentTime = 3
    expect(restore.capture_audio().bgm.gain).toBeCloseTo(0.2)
    old.source.onended()
    expect(engine._sources.get('bgm')).toBe(current)
    expect(current.source.stop).not.toHaveBeenCalled()
    expect(old.source.disconnect).toHaveBeenCalledOnce()
  })

  it('a restore invalidates an older pending fade-in decode without publishing its envelope', async () => {
    const f = fixture()
    const pending = defer()
    const entered = defer()
    f.ctx.decodeAudioData.mockImplementationOnce(() => { entered.resolve(); return pending.promise })
    const play = f.engine.play('bgm', 'late.wav', { fadein: 2 })
    await entered.promise
    const packet = await f.engine.decodePrepared(new Uint8Array(8))
    let applied
    try {
      applied = f.engine.applyPreparedBgm(packet, { path: 'saved.wav', position: 1, gain: 0.7, looping: true })
    } finally {
      pending.resolve({ duration: 5, length: 240000, sampleRate: 48000, numberOfChannels: 2 })
    }
    const current = f.engine._sources.get('bgm')
    expect(applied).toBe(true)
    expect(await play).toBe(false)
    expect(f.sources).toHaveLength(1)
    expect(f.engine._sources.get('bgm')).toBe(current)
    expect(current.clipGain.gain.linearRampToValueAtTime).not.toHaveBeenCalled()
  })

  it('positive stop cancels a pending decode without pretending a fade was applied', async () => {
    const f = fixture()
    const pending = defer(), entered = defer()
    f.ctx.decodeAudioData.mockImplementationOnce(() => { entered.resolve(); return pending.promise })
    const play = f.engine.play('bgm', 'pending.wav', { fadein: 2 })
    await entered.promise
    const stopped = f.engine.stop('bgm', { fadeout: 1 })
    pending.resolve({ duration: 5, length: 240000, sampleRate: 48000, numberOfChannels: 2 })
    const played = await play
    expect(stopped).toBe(false)
    expect(played).toBe(false)
    expect(f.sources).toHaveLength(0)
    expect(f.engine._pending.size).toBe(0)
    expect(f.engine._retiring.size).toBe(0)
  })

  it.each(['missing ramp', 'ramp throws'])('failed fade-in (%s) preserves the previous source and discards its candidate', async failure => {
    const f = fixture()
    await f.engine.play('bgm', 'kept.wav', { loop: true })
    const old = f.engine._sources.get('bgm')
    const createGain = f.ctx.createGain.getMockImplementation()
    f.ctx.createGain.mockImplementationOnce(() => {
      const node = createGain()
      if (failure === 'missing ramp') delete node.gain.linearRampToValueAtTime
      else node.gain.linearRampToValueAtTime.mockImplementation(() => { throw new Error('schedule failed') })
      return node
    })
    expect(await f.engine.play('bgm', 'rejected.wav', { fadein: 2 })).toBe(false)
    expect(f.engine._sources.get('bgm')).toBe(old)
    expect(old.source.stop).not.toHaveBeenCalled()
    expect(f.sources[1].start).not.toHaveBeenCalled()
    expect(f.sources[1].disconnect).toHaveBeenCalledOnce()
    expect(f.gains.at(-1).disconnect).toHaveBeenCalledOnce()
  })

  it('a source start failure after preparation never leaves a new owner', async () => {
    const f = fixture()
    const createSource = f.ctx.createBufferSource
    f.ctx.createBufferSource = () => {
      const source = createSource()
      source.start.mockImplementation(() => { throw new Error('start failed') })
      return source
    }
    expect(await f.engine.play('bgm', 'start-failed.wav', { fadein: 2 })).toBe(false)
    expect(f.engine._sources.size).toBe(0)
    expect(f.engine._retiring.size).toBe(0)
    expect(f.sources[0].disconnect).toHaveBeenCalledOnce()
  })

  it.each(['ramp', 'scheduled stop'])('fade-out %s failure performs immediate cleanup and returns false', async failure => {
    const { engine } = fixture()
    await engine.play('bgm', 'stop-failed.wav', { loop: true })
    const clip = engine._sources.get('bgm')
    if (failure === 'ramp') clip.clipGain.gain.linearRampToValueAtTime.mockImplementation(() => { throw new Error('ramp failed') })
    else clip.source.stop.mockImplementationOnce(() => { throw new Error('stop scheduling failed') })
    expect(engine.stop('bgm', { fadeout: 2 })).toBe(false)
    expect(engine._sources.size).toBe(0)
    expect(engine._retiring.size).toBe(0)
    expect(clip.source.disconnect).toHaveBeenCalledOnce()
    expect(clip.clipGain.disconnect).toHaveBeenCalledOnce()
    clip.source.onended()
    expect(clip.source.disconnect).toHaveBeenCalledOnce()
  })

  it('bus scheduling failure does not publish a new target and holds the previous value', () => {
    const { engine } = fixture()
    engine.setBusVolume('bgm', 0.6)
    const bus = engine._busGains.get('bgm').gain
    bus.linearRampToValueAtTime.mockImplementationOnce(() => { throw new Error('bus scheduling failed') })
    expect(engine.fadeVolume('bgm', 0.2, 2)).toBe(false)
    expect(engine._busVolumes.get('bgm')).toBe(0.6)
    expect(bus.value).toBe(0.6)
    expect(engine.fadeVolume('bgm', 0.8, 1)).toBe(true)
    expect(bus.setValueAtTime).toHaveBeenLastCalledWith(0.6, 0)
    expect(engine._busVolumes.get('bgm')).toBe(0.8)
  })

  it('missing bus automation support refuses a positive fade without changing state', () => {
    const { engine } = fixture()
    engine.setBusVolume('bgm', 0.6)
    const bus = engine._busGains.get('bgm').gain
    delete bus.linearRampToValueAtTime
    const cancellations = bus.cancelAndHoldAtTime.mock.calls.length
    expect(engine.fadeVolume('bgm', 0.2, 1)).toBe(false)
    expect(bus.cancelAndHoldAtTime).toHaveBeenCalledTimes(cancellations)
    expect(bus.value).toBe(0.6)
    expect(engine._busVolumes.get('bgm')).toBe(0.6)
  })
})

})
