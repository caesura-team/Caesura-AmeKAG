// Real browser WebAudio graph and decoded PCM. No mock AudioParam/source.
import { AudioEngine } from '/audio-engine.js'

const result = { checks: [], offlineDone: false, liveDone: false, errors: [] }
globalThis.__audioSmoke = result
addEventListener('error', event => result.errors.push(String(event.message)))
addEventListener('unhandledrejection', event => result.errors.push(String(event.reason)))
const assetUrl = path => new URL(path, location.href).href
const pause = ms => new Promise(resolve => setTimeout(resolve, ms))
function check(name, value, details) {
  result.checks.push({ name, passed: !!value, details })
  if (!value) throw new Error(name + ': ' + JSON.stringify(details))
}
function sample(buffer, time) {
  const data = buffer.getChannelData(0), radius = 128
  const center = Math.floor(time * buffer.sampleRate)
  let sum = 0
  for (let index = center - radius; index < center + radius; index++) sum += data[index]
  return sum / (2 * radius)
}
function closeTo(name, actual, expected, tolerance = 0.002) {
  check(name, Math.abs(actual - expected) <= tolerance, { actual, expected, tolerance })
}
async function offline(name, setup, verify, atPause) {
  const context = new OfflineAudioContext(1, 57600, 48000)
  const audio = new AudioEngine({ctx: context, fetchImpl: fetch})
  audio.setBusVolume('bgm', 0.6)
  try {
    await setup(audio, context)
    const held = atPause ? context.suspend(0.25) : null
    const rendering = context.startRendering()
    if (held) { await held; await atPause(audio, context); await context.resume() }
    const buffer = await rendering
    verify(buffer, audio)
    check(name + ': finite PCM', buffer.getChannelData(0).every(Number.isFinite))
  } finally { audio.destroy() }
}
async function play(audio, options = {}) {
  check('real WAV decode/source start', await audio.play('bgm', 'assets/constant.wav', {
    volume: 0.8, loop: true, assetUrl, ...options,
  }))
}
async function runOffline() {
  await offline('clip fadein', async audio => play(audio, {fadein: 0.4}), (buffer, audio) => {
    closeTo('fadein PCM at 0.1s', sample(buffer, 0.1), 0.03)
    closeTo('fadein PCM at 0.3s', sample(buffer, 0.3), 0.09)
    closeTo('fadein PCM final clip x bus', sample(buffer, 0.6), 0.12)
    closeTo('fadein preserves bus target', audio._busVolumes.get('bgm'), 0.6)
  })
  await offline('clip fadeout', async audio => {
    await play(audio)
    check('fadeout scheduled', audio.stop('bgm', {fadeout: 0.4}))
    check('fadeout owns a retiring source', audio._retiring.size === 1)
  }, (buffer, audio) => {
    closeTo('fadeout PCM at 0.1s', sample(buffer, 0.1), 0.09)
    closeTo('fadeout PCM at 0.3s', sample(buffer, 0.3), 0.03)
    closeTo('fadeout PCM silent after deadline', sample(buffer, 0.6), 0)
    check('fadeout onended releases owner', audio._retiring.size === 0)
    closeTo('fadeout preserves bus target', audio._busVolumes.get('bgm'), 0.6)
  })
  await offline('bus fade', async audio => {
    await play(audio)
    check('bus fade scheduled', audio.fadeVolume('bgm', 0.3, 0.4))
  }, buffer => {
    closeTo('bus fade PCM at 0.1s', sample(buffer, 0.1), 0.105)
    closeTo('bus fade PCM final', sample(buffer, 0.6), 0.06)
  })
  await offline('immediate override', async audio => {
    await play(audio)
    check('initial bus fade scheduled', audio.fadeVolume('bgm', 0.2, 1))
  }, buffer => closeTo('setter replaces held ramp in PCM', sample(buffer, 0.7), 0.18), audio => {
    audio.setBusVolume('bgm', 0.9)
    closeTo('setter target is latest', audio._busVolumes.get('bgm'), 0.9)
  })
  let expectedCaptureGain
  await offline('restore during clip fade', async audio => play(audio, {fadein: 1}), buffer => {
    closeTo('restore keeps instantaneous clip gain as static PCM', sample(buffer, 0.7), 0.25 * expectedCaptureGain * 0.9)
  }, async (audio, context) => {
    const state = audio.captureBgm()
    expectedCaptureGain = 0.8 * context.currentTime // Known 0 -> 0.8 envelope over one second from time zero.
    closeTo('capture matches independent audio-clock envelope', state.gain, expectedCaptureGain, 1e-6)
    const bytes = new Uint8Array(await (await fetch(assetUrl('assets/constant.wav'))).arrayBuffer())
    const packet = await audio.decodePrepared(bytes)
    audio.setBusVolume('bgm', 0.9)
    check('restore applied', audio.applyPreparedBgm(packet, state))
    check('restore clears retiring owners', audio._retiring.size === 0)
  })
  await offline('retiring replacement', async audio => {
    await play(audio)
    check('old clip fadeout scheduled', audio.stop('bgm', {fadeout: 0.4}))
    await play(audio, {volume: 0.4})
  }, (buffer, audio) => {
    closeTo('new clip survives old stop in PCM', sample(buffer, 0.7), 0.06)
    check('old stop cleaned its retiring owner', audio._retiring.size === 0)
  })
}
async function clockAt(context, time) {
  const deadline = performance.now() + 3000
  while (context.currentTime < time) {
    if (performance.now() > deadline) throw new Error('Audio clock did not reach deadline')
    await pause(5)
  }
}
async function prepareLive() {
  const context = new AudioContext({sampleRate: 48000})
  const audio = new AudioEngine({ctx: context, fetchImpl: fetch})
  const analyser = context.createAnalyser()
  analyser.fftSize = 2048
  const mute = context.createGain()
  mute.gain.value = 0
  analyser.connect(mute); mute.connect(context.destination)
  // Observe the production buses before a silent output sink; no speaker output.
  for (const gain of audio._busGains.values()) { gain.disconnect(); gain.connect(analyser) }
  audio.setBusVolume('bgm', 0.6)
  await play(audio)
  result.initialLiveState = context.state
  const button = document.getElementById('unlock')
  button.disabled = false
  button.onclick = async () => {
    button.disabled = true
    try {
      check('trusted gesture unlock', await audio.unlock() && context.state === 'running', context.state)
      check('live fade scheduled', audio.fadeVolume('bgm', 0.3, 0.25))
      await clockAt(context, context.currentTime + 0.32)
      const data = new Float32Array(analyser.fftSize)
      analyser.getFloatTimeDomainData(data)
      closeTo('live AudioContext PCM after bus fade', data.reduce((sum, value) => sum + value, 0) / data.length, 0.06, 0.012)
      const stopAt = context.currentTime + 0.25
      check('live fadeout scheduled', audio.stop('bgm', {fadeout: 0.25}))
      await context.suspend()
      const held = context.currentTime
      await pause(400) // Exceed the entire 250ms fade; wall-clock cleanup must still not fire.
      closeTo('suspension holds the audio clock', context.currentTime, held, 1 / context.sampleRate)
      check('suspension retains the fading owner', audio._retiring.size === 1)
      check('same context resumes', await audio.unlock())
      await clockAt(context, stopAt + 0.05)
      await pause(20)
      check('resumed audio deadline releases the owner', audio._retiring.size === 0)
    } catch (error) { result.errors.push(String(error.stack || error)) }
    finally { audio.destroy(); result.liveDone = true }
  }
}
try {
  await runOffline()
  result.offlineDone = true
  await prepareLive()
  result.liveReady = true
} catch (error) {
  result.errors.push(String(error.stack || error))
  result.offlineDone = true; result.liveDone = true
}
