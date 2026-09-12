// WebAudio owns playback truth; UI telemetry must not impersonate a source.
import { readAssetBytes } from './restore-assets.js'

export const MAX_AUDIO_BYTES = 64 * 1024 * 1024
const MAX_DECODED_AUDIO_BYTES = 256 * 1024 * 1024
const BUSES = ['bgm', 'se', 'voice']

const validDuration = value => Number.isFinite(value) && value >= 0
const validGain = value => Number.isFinite(value) && value >= 0 && value <= 16
const validTime = value => Number.isFinite(value) && value >= 0

// Only the current linear segment is retained; WebAudio renders the curve.
function gainAt(segment, fallback, time) {
  if (!segment) return fallback
  if (time >= segment.end) return segment.to
  if (time <= segment.start) return segment.from
  return segment.from + (segment.to - segment.from) * (time - segment.start) / (segment.end - segment.start)
}

function canRamp(param) {
  return typeof param?.setValueAtTime === 'function'
    && typeof param.linearRampToValueAtTime === 'function'
    && (typeof param.cancelAndHoldAtTime === 'function' || typeof param.cancelScheduledValues === 'function')
}

function cancelGain(param, time) {
  if (typeof param.cancelAndHoldAtTime === 'function') param.cancelAndHoldAtTime(time)
  else if (typeof param.cancelScheduledValues === 'function') param.cancelScheduledValues(time)
  else throw new Error('AudioParam cancellation is unavailable')
}

function rampGain(param, from, to, time, duration) {
  const end = time + duration
  if (!canRamp(param) || !validTime(time) || !validTime(end) || !validGain(from) || !validGain(to)) {
    throw new Error('AudioParam automation is unavailable or invalid')
  }
  cancelGain(param, time)
  param.setValueAtTime(from, time)
  param.linearRampToValueAtTime(to, end)
  return { from, to, start: time, end }
}

function validateBuffer(buffer) {
  if (!buffer || !Number.isFinite(buffer.duration) || buffer.duration <= 0 || buffer.duration > 86400
      || !Number.isSafeInteger(buffer.length) || buffer.length <= 0
      || !Number.isInteger(buffer.numberOfChannels) || buffer.numberOfChannels <= 0
      || !Number.isFinite(buffer.sampleRate) || buffer.sampleRate <= 0
      || buffer.length * buffer.numberOfChannels * 4 > MAX_DECODED_AUDIO_BYTES
      || Math.abs(buffer.duration - buffer.length / buffer.sampleRate) > 1 / buffer.sampleRate) {
    throw new Error('Invalid decoded audio or decoded size limit exceeded')
  }
}

export class AudioEngine {
  constructor(opts = {}) {
    this._ctx = opts.ctx ?? null
    this._fetchImpl = opts.fetchImpl
    this._buffers = new Map()
    this._sources = new Map()
    this._retiring = new Set()
    this._busGains = new Map()
    this._busFades = new Map()
    this._busVolumes = new Map(BUSES.map(kind => [kind, 1]))
    this._generations = new Map()
    this._pending = new Map()
    this._revision = 0
    this.ready = false
    this._init()
  }

  _init() {
    if (!this._ctx) return
    for (const kind of BUSES) {
      const gain = this._ctx.createGain()
      gain.gain.value = this._busVolumes.get(kind)
      gain.connect(this._ctx.destination)
      this._busGains.set(kind, gain)
    }
    this.ready = true
  }

  ensureContext() {
    if (this._ctx) return this._ctx.state === 'closed' ? null : this._ctx
    const Ctor = globalThis.AudioContext || globalThis.webkitAudioContext
    if (!Ctor) return null
    this._ctx = new Ctor()
    this._init()
    return this._ctx
  }

  _nextGeneration(kind) {
    const generation = (this._generations.get(kind) ?? 0) + 1
    this._generations.set(kind, generation)
    return generation
  }

  // Cancellation generations and committed source identities are distinct.
  isCurrentPlayback(kind, owner) {
    return owner != null && this._sources.get(kind)?.identity === owner && this.isPlaying(kind)
  }

  /** Decode owned bytes without starting or stopping any source. The packet is
   * internal to audio restoration and tied to this context's lifetime. */
  async decodePrepared(bytes, context = this.ensureContext()) {
    if (!context) throw new Error('AudioContext is unavailable')
    if (context !== this._ctx || context.state === 'closed') throw new Error('Prepared audio context expired')
    if (!(bytes instanceof Uint8Array) || bytes.byteLength === 0 || bytes.byteLength > MAX_AUDIO_BYTES) {
      throw new Error('Invalid encoded audio size limit')
    }
    const revision = this._revision
    const owned = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength)
    const buffer = await context.decodeAudioData(owned)
    if (context !== this._ctx || revision !== this._revision || context.state === 'closed') {
      throw new Error('Prepared audio context expired')
    }
    validateBuffer(buffer)
    return Object.freeze({ buffer, context, revision })
  }

  async _load(path, assetUrl, context) {
    if (this._buffers.has(path)) return this._buffers.get(path)
    const revision = this._revision
    const resolveUrl = typeof assetUrl === 'string' ? value => assetUrl + value : assetUrl
    const promise = (async () => {
      const bytes = await readAssetBytes(path, {
        fetchImpl: this._fetchImpl ?? globalThis.fetch,
        assetUrl: resolveUrl ?? (value => value), maxBytes: MAX_AUDIO_BYTES,
      })
      if (context !== this._ctx || revision !== this._revision || context.state === 'closed') return null
      return this.decodePrepared(bytes, context)
    })().catch(() => {
      if (this._buffers.get(path) === promise) this._buffers.delete(path)
      return null
    })
    this._buffers.set(path, promise)
    return promise
  }

  _validatePrepared(packet, position, gain) {
    if (!packet || packet.context !== this._ctx || packet.revision !== this._revision
        || !this._ctx || this._ctx.state === 'closed') throw new Error('Prepared audio context expired')
    validateBuffer(packet.buffer)
    if (!Number.isFinite(position) || position < 0 || position >= packet.buffer.duration) {
      throw new Error('Audio position is outside the decoded clip')
    }
    if (!Number.isFinite(gain) || gain < 0 || gain > 16) throw new Error('Invalid clip gain')
  }

  _release(record, stop) {
    if (record.released) return
    record.released = true
    this._retiring.delete(record)
    record.envelope = null
    if (stop) { try { record.source.stop() } catch { /* Already stopped or not started. */ } }
    try { record.source.disconnect() } catch { /* Detached source. */ }
    try { record.clipGain?.disconnect() } catch { /* Detached gain. */ }
  }

  _stopSource(kind) {
    const record = this._sources.get(kind)
    if (!record) return
    this._sources.delete(kind)
    this._release(record, true)
  }

  _start(kind, packet, { path, position = 0, gain = 1, looping = false, fadein = 0 }) {
    this._validatePrepared(packet, position, gain)
    const context = this._ctx
    if (!validDuration(fadein) || !validTime(context.currentTime)) throw new Error('Invalid audio envelope time')
    const source = context.createBufferSource()
    const record = { source, kind, context, identity: Symbol('audio-source'), clipGain: null, gain: this._busGains.get(kind), path, envelope: null,
      started: context.currentTime, offset: position, duration: packet.buffer.duration, released: false }
    try {
      record.clipGain = context.createGain()
      record.clipGain.gain.value = fadein > 0 ? 0 : gain
      if (fadein > 0) record.envelope = rampGain(record.clipGain.gain, 0, gain, record.started, fadein)
      source.buffer = packet.buffer
      source.loop = looping
      source.connect(record.clipGain)
      record.clipGain.connect(record.gain)
      source.onended = () => {
        if (this._sources.get(kind) === record) this._sources.delete(kind)
        this._release(record, false)
      }
      this._stopSource(kind)
      this._sources.set(kind, record)
      source.start(0, position)
      return record
    } catch (error) {
      if (this._sources.get(kind) === record) this._sources.delete(kind)
      this._release(record, true)
      throw error
    }
  }

  async play(kind, path, opts = {}) {
    return (await this.playWithReceipt(kind, path, opts)).played
  }

  async playWithReceipt(kind, path, opts = {}) {
    const failed = {played: false, owner: null}
    if (!BUSES.includes(kind)) return failed
    let generation
    try {
      const fadein = opts.fadein ?? 0
      if (!validDuration(fadein)) return failed
      generation = this._nextGeneration(kind)
      const context = this.ensureContext()
      if (!context) return failed
      this._pending.set(kind, generation)
      const packet = await this._load(path, opts.assetUrl, context)
      if (!packet || this._generations.get(kind) !== generation || context !== this._ctx) return failed
      const record = this._start(kind, packet, { path, position: opts.position ?? 0,
        gain: opts.volume == null ? 1 : Number(opts.volume), looping: !!opts.loop, fadein })
      return {played: true, owner: record.identity}
    } catch { return failed }
    finally { if (generation !== undefined && this._pending.get(kind) === generation) this._pending.delete(kind) }
  }

  /** Apply a fully decoded candidate synchronously, preserving user bus gains. */
  applyPreparedBgm(packet, state) {
    this._validatePrepared(packet, state.position, state.gain)
    this.stopAll()
    this._start('bgm', packet, { ...state, fadein: 0 })
    return true
  }

  stop(kind, opts = {}) {
    if (!BUSES.includes(kind)) return false
    const fadeout = opts?.fadeout ?? 0
    if (!validDuration(fadeout)) return false
    this._nextGeneration(kind)
    this._pending.delete(kind)
    if (fadeout === 0) {
      this._stopSource(kind)
      for (const record of this._retiring) if (record.kind === kind) this._release(record, true)
      return true
    }
    const record = this._sources.get(kind)
    if (!record) return false // Pending work is cancelled, but no fade ran.
    this._sources.delete(kind)
    this._retiring.add(record)
    try {
      const context = this._ctx
      if (!context || context !== record.context || context.state === 'closed' || !this.ready) {
        throw new Error('Audio fade-out context is unavailable')
      }
      const now = context.currentTime
      const from = gainAt(record.envelope, record.clipGain.gain.value, now)
      record.envelope = rampGain(record.clipGain.gain, from, 0, now, fadeout)
      record.source.stop(record.envelope.end)
      return true
    } catch {
      // A failed requested envelope still performs immediate cleanup.
      this._release(record, true)
      return false
    }
  }

  isPlaying(kind) {
    const record = this._sources.get(kind)
    if (!record || record.released || !this._ctx || this._ctx.state === 'closed') return false
    return record.source.loop || record.offset + Math.max(0, this._ctx.currentTime - record.started) < record.duration
  }

  captureBgm() {
    if (this._pending.has('bgm')) throw new Error('BGM preparation is pending')
    if (!this.isPlaying('bgm')) return false
    const record = this._sources.get('bgm')
    const looping = record.source.loop === true
    const elapsed = record.offset + Math.max(0, this._ctx.currentTime - record.started)
    const position = looping ? elapsed % record.duration : elapsed
    const gain = gainAt(record.envelope, record.clipGain.gain.value, this._ctx.currentTime)
    if (!Number.isFinite(position) || !Number.isFinite(gain) || gain < 0 || gain > 16) {
      throw new Error('Invalid active BGM state')
    }
    return { path: record.path, position, gain, looping }
  }

  setBusVolume(kind, value) {
    if (!BUSES.includes(kind)) return
    const number = Number(value)
    const volume = Number.isFinite(number) ? Math.max(0, Math.min(16, number)) : 1
    const gain = this._busGains.get(kind)
    if (gain) {
      if (this._busFades.has(kind)) cancelGain(gain.gain, this._ctx.currentTime)
      gain.gain.value = volume
    }
    this._busFades.delete(kind)
    this._busVolumes.set(kind, volume)
  }

  fadeVolume(kind, target, duration) {
    if (!BUSES.includes(kind) || !validGain(target) || !validDuration(duration)) return false
    const context = this._ctx
    const param = this._busGains.get(kind)?.gain
    if (!context || context.state === 'closed' || !this.ready || !param || !validTime(context.currentTime)) return false
    const now = context.currentTime
    if (!validTime(now + duration) || (duration > 0 && !canRamp(param))) return false
    const from = gainAt(this._busFades.get(kind), param.value, now)
    if (!validGain(from)) return false
    try {
      if (duration > 0) {
        const segment = rampGain(param, from, target, now, duration)
        this._busFades.set(kind, segment)
      } else {
        if (this._busFades.has(kind)) cancelGain(param, now)
        param.value = target
        this._busFades.delete(kind)
      }
      this._busVolumes.set(kind, target)
      return true
    } catch {
      // Host scheduling can fail after cancellation. Do not publish the new
      // target; best-effort hold the previous instantaneous value instead.
      this._busFades.delete(kind)
      try {
        cancelGain(param, now)
        param.setValueAtTime(from, now)
      } catch {
        try { param.cancelScheduledValues?.(now) } catch { /* Broken host API. */ }
        try { param.value = from } catch { /* No success is reported. */ }
      }
      return false
    }
  }

  stopAll() { for (const kind of BUSES) this.stop(kind) }
  suspend() { return this._ctx?.suspend?.() }
  resume() { return this._ctx?.resume?.() }
  get state() { return this._ctx ? (this._ctx.state ?? 'running') : 'none' }
  get currentTime() { return this._ctx?.currentTime ?? 0 }

  // Eligibility only: a user gesture, a valid asset and successful decoding
  // are still required. Querying this never creates/unlocks an AudioContext.
  isPlaybackAvailable() {
    if (this._ctx) return this.ready && this._ctx.state !== 'closed'
    return typeof globalThis.AudioContext === 'function' || typeof globalThis.webkitAudioContext === 'function'
  }

  async unlock() {
    let context
    try { context = this.ensureContext() }
    catch { return false } // A later gesture may retry context construction.
    if (!context) return false
    const revision = this._revision
    try { if (context.state === 'suspended') await context.resume?.() }
    catch { /* A later trusted gesture can retry. */ }
    // A delayed resume belongs to its original context lifetime, even when
    // a replacement context has already started playing successfully.
    return context === this._ctx && revision === this._revision && this.state === 'running'
  }

  destroy() {
    this._revision++
    this.stopAll()
    try { this._ctx?.close?.()?.catch?.(() => {}) } catch { /* Already closed. */ }
    this._ctx = null
    this.ready = false
    this._busGains.clear()
    this._busFades.clear()
    this._buffers.clear()
  }
}
