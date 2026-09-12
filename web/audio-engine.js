// WebAudio owns playback truth; UI telemetry must not impersonate a source.
import { readAssetBytes } from './restore-assets.js'

export const MAX_AUDIO_BYTES = 64 * 1024 * 1024
const MAX_DECODED_AUDIO_BYTES = 256 * 1024 * 1024
const BUSES = ['bgm', 'se', 'voice']

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
    this._busGains = new Map()
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

  _start(kind, packet, { path, position = 0, gain = 1, looping = false }) {
    this._validatePrepared(packet, position, gain)
    const context = this._ctx
    const source = context.createBufferSource()
    const record = { source, clipGain: null, gain: this._busGains.get(kind), path,
      started: context.currentTime, offset: position, duration: packet.buffer.duration, released: false }
    try {
      record.clipGain = context.createGain()
      record.clipGain.gain.value = gain
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
      return true
    } catch (error) {
      if (this._sources.get(kind) === record) this._sources.delete(kind)
      this._release(record, true)
      throw error
    }
  }

  async play(kind, path, opts = {}) {
    if (!BUSES.includes(kind)) return false
    const generation = this._nextGeneration(kind)
    try {
      const context = this.ensureContext()
      if (!context) return false
      this._pending.set(kind, generation)
      const packet = await this._load(path, opts.assetUrl, context)
      if (!packet || this._generations.get(kind) !== generation || context !== this._ctx) return false
      return this._start(kind, packet, { path, position: opts.position ?? 0,
        gain: opts.volume == null ? 1 : Number(opts.volume), looping: !!opts.loop })
    } catch { return false }
    finally { if (this._pending.get(kind) === generation) this._pending.delete(kind) }
  }

  /** Apply a fully decoded candidate synchronously, preserving user bus gains. */
  applyPreparedBgm(packet, state) {
    this._validatePrepared(packet, state.position, state.gain)
    this.stopAll()
    return this._start('bgm', packet, state)
  }

  stop(kind) {
    this._nextGeneration(kind)
    this._pending.delete(kind)
    this._stopSource(kind)
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
    const gain = record.clipGain.gain.value
    if (!Number.isFinite(position) || !Number.isFinite(gain) || gain < 0 || gain > 16) {
      throw new Error('Invalid active BGM state')
    }
    return { path: record.path, position, gain, looping }
  }

  setBusVolume(kind, value) {
    if (!BUSES.includes(kind)) return
    const number = Number(value)
    const volume = Number.isFinite(number) ? Math.max(0, Math.min(16, number)) : 1
    this._busVolumes.set(kind, volume)
    const gain = this._busGains.get(kind)
    if (gain) gain.gain.value = volume
  }

  stopAll() { for (const kind of BUSES) this.stop(kind) }
  suspend() { return this._ctx?.suspend?.() }
  resume() { return this._ctx?.resume?.() }
  get state() { return this._ctx ? (this._ctx.state ?? 'running') : 'none' }

  // Eligibility only: a user gesture, a valid asset and successful decoding
  // are still required. Querying this never creates/unlocks an AudioContext.
  isPlaybackAvailable() {
    if (this._ctx) return this.ready && this._ctx.state !== 'closed'
    return typeof globalThis.AudioContext === 'function' || typeof globalThis.webkitAudioContext === 'function'
  }

  async unlock() {
    const context = this.ensureContext()
    if (!context) return false
    try { if (context.state === 'suspended') await context.resume?.() }
    catch { /* A later trusted gesture can retry. */ }
    return this.state === 'running'
  }

  destroy() {
    this._revision++
    this.stopAll()
    try { this._ctx?.close?.()?.catch?.(() => {}) } catch { /* Already closed. */ }
    this._ctx = null
    this.ready = false
    this._busGains.clear()
    this._buffers.clear()
  }
}
