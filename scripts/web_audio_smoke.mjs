#!/usr/bin/env node
// Real Chromium WebAudio PCM/lifecycle checks. Uses only a test-owned profile.
import { createServer } from 'node:http'
import { createServer as portServer } from 'node:net'
import { mkdirSync, readFileSync, writeFileSync, existsSync } from 'node:fs'
import { dirname, join, resolve, relative, isAbsolute } from 'node:path'
import { fileURLToPath } from 'node:url'
import { spawn, spawnSync } from 'node:child_process'
import { createHash } from 'node:crypto'

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const args = process.argv.slice(2)
const option = (name, fallback) => args.includes(name) ? args[args.indexOf(name) + 1] : fallback
const out = resolve(root, option('--out', 'artifacts/validation/web-audio-smoke'))
const inside = relative(root, out)
if (!inside || inside.startsWith('..') || isAbsolute(inside) || existsSync(out)) throw new Error('Use a new output directory inside the repository')
mkdirSync(out, {recursive: true})
const browserChoice = option('--browser', 'chrome')
const probe = spawnSync(process.execPath, [join(root, 'scripts/web_browser_smoke.mjs'), '--print-browser', '--browser', browserChoice], {cwd: root, encoding: 'utf8', windowsHide: true})
if (probe.status !== 0 || !existsSync(probe.stdout.trim())) throw new Error('Browser resolver failed: ' + probe.stderr)
const browser = probe.stdout.trim()
const freePort = () => new Promise(resolvePort => {
  const server = portServer(); server.listen(0, '127.0.0.1', () => { const port = server.address().port; server.close(() => resolvePort(port)) })
})
const delay = ms => new Promise(resolveDelay => setTimeout(resolveDelay, ms))
function wav() {
  const count = 72000, data = Buffer.alloc(44 + count * 2)
  data.write('RIFF'); data.writeUInt32LE(data.length - 8, 4); data.write('WAVEfmt ', 8)
  data.writeUInt32LE(16, 16); data.writeUInt16LE(1, 20); data.writeUInt16LE(1, 22)
  data.writeUInt32LE(48000, 24); data.writeUInt32LE(96000, 28)
  data.writeUInt16LE(2, 32); data.writeUInt16LE(16, 34); data.write('data', 36); data.writeUInt32LE(count * 2, 40)
  for (let index = 0; index < count; index++) data.writeInt16LE(8192, 44 + index * 2)
  return data
}
const files = new Map([
  ['/audio-engine.js', join(root, 'web/audio-engine.js')],
  ['/restore-assets.js', join(root, 'web/restore-assets.js')],
  ['/audio-pcm-probe.js', join(root, 'web/test-support/audio-pcm-probe.js')],
])
const hash = bytes => createHash('sha256').update(bytes).digest('hex')
const sourcePaths = [...files.values(), fileURLToPath(import.meta.url)]
const sourceIdentity = () => Object.fromEntries(sourcePaths.map(path => [relative(root, path).replaceAll('\\', '/'), hash(readFileSync(path))]))
const sourceBefore = sourceIdentity()
const html = '<!doctype html><meta charset="utf-8"><title>Caesura audio validation</title><button id="unlock" disabled>Unlock test audio graph</button><pre>PCM analysis uses a silent output sink.</pre><script type="module" src="/audio-pcm-probe.js"></script>'
const server = createServer((request, response) => {
  const path = new URL(request.url, 'http://local').pathname
  const body = path === '/' ? html : path === '/assets/constant.wav' ? wav() : files.has(path) ? readFileSync(files.get(path)) : null
  response.writeHead(body === null ? 404 : 200, {'content-type': path === '/' ? 'text/html; charset=utf-8' : path.endsWith('.wav') ? 'audio/wav' : 'text/javascript; charset=utf-8', 'cache-control': 'no-store'})
  response.end(body ?? '')
})
class Cdp {
  constructor(socket) {
    this.socket = socket; this.pending = new Map(); this.next = 1
    socket.addEventListener('message', event => {
      const value = JSON.parse(event.data), call = this.pending.get(value.id)
      if (!call) return
      this.pending.delete(value.id); clearTimeout(call.timer)
      if (value.error) call.reject(new Error(value.error.message)); else call.resolve(value.result)
    })
  }
  static async connect(url) {
    const socket = new WebSocket(url)
    await new Promise((yes, no) => { socket.onopen = yes; socket.onerror = no })
    return new Cdp(socket)
  }
  send(method, params = {}) {
    const id = this.next++
    return new Promise((resolveCall, reject) => {
      const timer = setTimeout(() => { this.pending.delete(id); reject(new Error(method + ' timed out')) }, 10000)
      this.pending.set(id, {resolve: resolveCall, reject, timer})
      this.socket.send(JSON.stringify({id, method, params}))
    })
  }
  async evaluate(expression) {
    const value = await this.send('Runtime.evaluate', {expression, returnByValue: true, awaitPromise: true})
    if (value.exceptionDetails) throw new Error(JSON.stringify(value.exceptionDetails))
    return value.result?.value
  }
}
let child, page, browserCdp, report
const port = await freePort()
let debugPort = await freePort()
while (debugPort === port) debugPort = await freePort()
try {
  await new Promise(resolveListen => server.listen(port, '127.0.0.1', resolveListen))
  const browserArgs = ['--headless=new', '--no-first-run', '--no-default-browser-check', '--disable-extensions',
    '--remote-debugging-port=' + debugPort, '--user-data-dir=' + join(out, 'profile'), '--window-size=800,600']
  if (process.env.CI) browserArgs.push('--no-sandbox', '--disable-dev-shm-usage')
  child = spawn(browser, [...browserArgs, 'about:blank'], {stdio: 'ignore', windowsHide: true})
  child.on('error', error => { report = {passed: false, error: String(error)} })
  let version
  const deadline = Date.now() + 60000
  while (!version) {
    try { version = await (await fetch(`http://127.0.0.1:${debugPort}/json/version`)).json() } catch { await delay(50) }
    if (Date.now() > deadline) throw new Error('Browser debugging endpoint unavailable')
  }
  browserCdp = await Cdp.connect(version.webSocketDebuggerUrl)
  const targets = await (await fetch(`http://127.0.0.1:${debugPort}/json/list`)).json()
  page = await Cdp.connect(targets.find(target => target.type === 'page').webSocketDebuggerUrl)
  await page.send('Page.enable'); await page.send('Runtime.enable')
  await page.send('Page.navigate', {url: `http://127.0.0.1:${port}/`})
  let state
  do {
    state = await page.evaluate('globalThis.__audioSmoke || null')
    if (Date.now() > deadline) throw new Error('Audio probe did not finish')
    if (!state?.liveReady && !state?.errors?.length) await delay(50)
  } while (!state?.liveReady && !state?.errors?.length)
  if (!state.errors.length) {
    if (state.initialLiveState !== 'suspended') throw new Error('Fresh browser did not require the tested unlock transition: ' + state.initialLiveState)
    const rectangle = await page.evaluate('(()=>{const r=document.getElementById("unlock").getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2}})()')
    await page.send('Input.dispatchMouseEvent', {type: 'mousePressed', ...rectangle, button: 'left', clickCount: 1})
    await page.send('Input.dispatchMouseEvent', {type: 'mouseReleased', ...rectangle, button: 'left', clickCount: 1})
    do {
      await delay(25); state = await page.evaluate('globalThis.__audioSmoke')
      if (Date.now() > deadline) throw new Error('Live audio probe did not finish')
    } while (!state.liveDone)
  }
  report = {passed: state.offlineDone && state.liveDone && !state.errors.length && state.checks.every(check => check.passed),
    browser: version.Browser, browserPath: browser, profile: join(out, 'profile'), ...state}
} catch (error) { report = {...report, passed: false, error: String(error.stack || error)} }
finally {
  // Chrome can relaunch from a short-lived Windows launcher. Close the actual
  // protocol instance, then use the owned launcher only as a fallback.
  try {
    if (!browserCdp) {
      const version = await (await fetch(`http://127.0.0.1:${debugPort}/json/version`)).json()
      browserCdp = await Cdp.connect(version.webSocketDebuggerUrl)
    }
    await browserCdp.send('Browser.close')
  } catch { child?.kill() }
  page?.socket.close(); browserCdp?.socket.close()
  let endpointClosed = false
  for (let attempt = 0; attempt < 60; attempt++) {
    try { await fetch(`http://127.0.0.1:${debugPort}/json/version`) }
    catch { endpointClosed = true; break }
    await delay(50)
  }
  await new Promise(resolveClose => server.close(resolveClose))
  const sourceAfter = sourceIdentity()
  report = {...report, node: process.version, fixture_sha256: hash(wav()), source_before: sourceBefore,
    source_after: sourceAfter, source_stable: JSON.stringify(sourceBefore) === JSON.stringify(sourceAfter), browser_endpoint_closed: endpointClosed}
  report.passed = report.passed && report.source_stable && endpointClosed
  writeFileSync(join(out, 'report.json'), JSON.stringify(report, null, 2) + '\n')
}
console.log(JSON.stringify(report, null, 2))
process.exitCode = report.passed ? 0 : 1
