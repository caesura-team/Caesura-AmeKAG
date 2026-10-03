// Required real-browser provider for the story throughput case. No test clock,
// synthetic canvas host, or animation-frame replacement is installed here.
import { createPlayer } from '../bridge.js'
import { DomRenderer } from '../dom-renderer.js'

const ensure = (value, message) => { if (!value) throw new Error(message) }
async function heapKB(player) {
  return Number(await player.lua.doString('collectgarbage("collect"); collectgarbage("collect"); collectgarbage("collect"); return tostring(collectgarbage("count"))'))
}
async function benchmarkRun(player, renderer, source) {
  player.core.events.length = 0
  player.core.backlog.length = 0
  player.lua.global.set('__PERF_TRACE', true)
  player.lua.global.set('__FRAME_COUNT', 0)
  const memBefore = await heapKB(player)
  const audioBefore = player.audio._ctx?.state ?? 'uncreated'
  ensure(player.audio._ctx === audioContext && audioBefore === 'closed' && player.audio.isPlaybackAvailable() === false,
    'Story baseline requires the original unavailable-audio profile')
  const presentedBefore = renderer._sceneFrame
  const start = performance.now()
  const out = String(await player.runScene(source, 'story.ks', {maxFrames:1000000, autoClick:true}))
  const wallMs = performance.now() - start
  const frames = Number((await player.lua.global.get('__FRAME_COUNT')) || 0)
  const memAfter = await heapKB(player)
  const match = /^DONE:(\d+):(\d+)$/.exec(out)
  const errors = player.core.events.filter(event => String(event.kind).includes('error'))
  const renderedFrames = renderer._sceneFrame - presentedBefore
  ensure(match, 'Story did not complete: ' + out)
  ensure(frames > 100, 'Real scheduler tick count missing')
  ensure(Number.isFinite(wallMs) && wallMs > 0, 'Positive measured duration required')
  ensure(errors.length === 0, 'Story errors: ' + JSON.stringify(errors))
  ensure(out === 'DONE:339:193', 'Story route/workload changed: ' + out)
  ensure(renderedFrames >= 150, 'Story omitted required real presentations')
  ensure(player.audio._ctx === audioContext && player.audio.state === 'closed' && player.audio.isPlaybackAvailable() === false,
    'Story audio profile changed during measurement')
  return {out, wallMs, frames, framesPerMs:frames / wallMs, tokens:Number(match[1]),
    tokensPerMs:Number(match[1]) / wallMs, memGrowthKB:memAfter - memBefore,
    renderedFrames, errors, audioBefore, audioAfter:player.audio._ctx?.state ?? 'uncreated',
    audioContextRetained:player.audio._ctx === audioContext, audioAvailable:player.audio.isPlaybackAvailable()}
}

globalThis.__storyBenchmark = {done:false}
let player, renderer, stage, audioContext
try {
  const source = await (await fetch('/__story__/story.ks')).text()
  const project = await (await fetch('/__story__/project.json')).json()
  const assetUrl = path => new URL(String(path).startsWith('assets/') ? String(path) : 'assets/' + path, location.origin).href
  // The original jsdom workload has no playback backend and completes in one
  // runScene call. Preserve that explicit capability profile using the public
  // injection point and a genuinely closed browser context. Do not synthesize
  // audio ticks or combine several WAIT_AUDIO resumes into one measured run.
  const AudioContextClass = globalThis.AudioContext || globalThis.webkitAudioContext
  ensure(typeof AudioContextClass === 'function', 'Real browser AudioContext required')
  audioContext = new AudioContextClass()
  await audioContext.close()
  ensure(audioContext.state === 'closed', 'Audio baseline context did not close')
  player = await createPlayer({scriptsBase:location.origin + '/scripts/', assetUrl,
    audioAssetUrl:assetUrl, langBase:location.origin + '/assets/lang/',
    wasmFile:location.origin + '/__story__/glue.wasm', capabilities:project.capabilities, audioContext})
  stage = document.createElement('div')
  Object.assign(stage.style, {position:'relative', width:'1280px', height:'720px'})
  document.body.appendChild(stage)
  renderer = new DomRenderer(player.core, stage)
  const warmup = await benchmarkRun(player, renderer, source)
  const samples = []
  for (let index = 0; index < 3; index++) samples.push(await benchmarkRun(player, renderer, source))
  globalThis.__storyBenchmark = {done:true, warmup, samples, visibility:document.visibilityState,
    audioAvailable:player.audio.isPlaybackAvailable(), audioProfile:'unavailable-real-closed-context',
    declaredCapabilities:project.capabilities}
} catch (error) {
  globalThis.__storyBenchmark = {done:true, error:String(error.stack || error)}
} finally {
  try { await player?.dispose() }
  catch(error) { globalThis.__storyBenchmark.error = String(error.stack || error) }
  if(audioContext && audioContext.state !== 'closed')await audioContext.close()
  renderer?.destroy(); stage?.remove()
  globalThis.__storyBenchmark.disposed = true
}
