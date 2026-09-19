#!/usr/bin/env node
// One explicitly owned browser/page probe. This script never starts/stops a
// browser or HTTP server and never searches existing pages. The Python caller
// owns processes, ports, package digests, final acceptance and full cleanup.
import { createHash } from 'node:crypto'
import { get as httpGet } from 'node:http'
import { appendFileSync, closeSync, existsSync, fstatSync, lstatSync, mkdirSync, openSync, readFileSync, readSync, realpathSync, writeFileSync } from 'node:fs'
import { basename, dirname, join, resolve } from 'node:path'

const SCHEMA = 'caesura.web-package-probe.v1'
const sleep = ms => new Promise(done => setTimeout(done, ms))
const sha = bytes => createHash('sha256').update(bytes).digest('hex')
const requireThat = (condition, message) => { if (!condition) throw new Error(message) }
const writeJson = (path, value) => writeFileSync(path, JSON.stringify(value, null, 2) + '\n', { encoding: 'utf8', flag: 'wx' })
function errorText(error) {
  const parts = [], seen = new Set()
  for (let current = error; current && !seen.has(current) && parts.length < 8; current = current.cause) {
    seen.add(current); parts.push(String(current.stack || current))
  }
  return parts.join('\nCaused by: ')
}

function actionBytes(path) {
  const signature = info => [info.dev, info.ino, info.size, info.mtimeNs, info.ctimeNs].join(':')
  const before = lstatSync(path, { bigint: true })
  requireThat(before.isFile() && !before.isSymbolicLink() && before.size <= 65536n, 'actions file must be a plain file at most 64 KiB')
  const fd = openSync(path, 'r')
  try {
    requireThat(signature(fstatSync(fd, { bigint: true })) === signature(before), 'actions file changed before opening')
    const bytes = Buffer.alloc(65537)
    let length = 0, received
    do { received = readSync(fd, bytes, length, bytes.length - length, null); length += received } while (received && length < bytes.length)
    requireThat(length <= 65536, 'actions file exceeds 64 KiB')
    requireThat(signature(fstatSync(fd, { bigint: true })) === signature(before)
      && signature(lstatSync(path, { bigint: true })) === signature(before), 'actions file changed while reading')
    return bytes.subarray(0, length)
  } finally { closeSync(fd) }
}
function readActions(file, expected) {
  requireThat(/^[0-9a-f]{64}$/.test(expected), 'actions SHA must be an explicit lowercase SHA-256')
  const path = realpathSync.native(resolve(file)), bytes = actionBytes(path)
  requireThat(sha(bytes) === expected, 'actions file differs from its locked SHA-256')
  let value
  try { value = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(bytes)) }
  catch (error) { throw new Error('actions must contain valid UTF-8 JSON: ' + String(error)) }
  const keys = (object, expectedKeys) => object && typeof object === 'object' && !Array.isArray(object)
    && Object.keys(object).sort().join(',') === [...expectedKeys].sort().join(',')
  const string = (value, limit) => typeof value === 'string' && value.length > 0 && value.length <= limit && !/[\x00-\x1f\x7f]/.test(value)
  requireThat(keys(value, ['schema', 'steps']) && value.schema === 1 && Array.isArray(value.steps)
    && value.steps.length >= 1 && value.steps.length <= 20, 'actions require schema 1 and 1..20 explicit steps with no extra fields')
  for (const [index, step] of value.steps.entries()) {
    requireThat(keys(step, ['click']) && (string(step.click, 512)
      || keys(step.click, ['selector', 'surface']) && string(step.click.selector, 512) && string(step.click.surface, 512))
      || keys(step, ['wait']) && keys(step.wait, ['selector', 'text']) && string(step.wait.selector, 512) && string(step.wait.text, 2048),
    'actions step ' + (index + 1) + ' must be one bounded click selector/explicit surface or wait selector/text object')
  }
  return { path, sha256: expected, bytes: bytes.length, configuration: value }
}

function localUrl(text, kind) {
  requireThat(typeof text === 'string' && /^http:\/\/(127\.0\.0\.1|\[::1\]):[1-9][0-9]{0,4}(?:\/[^?#]*)?$/.test(text), kind + ' URL must use an explicit HTTP loopback IP/port without credentials/query/fragment')
  requireThat(!text.includes('\\'), kind + ' URL contains a backslash')
  const url = new URL(text)
  requireThat(Number(url.port) > 0 && Number(url.port) <= 65535, kind + ' URL has invalid port')
  const rawPath = text.replace(/^http:\/\/[^/]+/, '') || '/'
  const decoded = decodeURIComponent(rawPath)
  requireThat(!decoded.includes('\\') && !decoded.split('/').some((part, i, all) => part === '.' || part === '..' || (!part && i !== 0 && i !== all.length - 1)), kind + ' URL has an ambiguous path')
  if (kind === 'CDP') requireThat(url.pathname === '/', 'CDP URL must identify its root endpoint, not a page/list path')
  else requireThat(url.pathname.endsWith('/') || url.pathname.endsWith('/index.html'), 'Deployment URL must identify its directory or index.html')
  return url
}
function readCdpVersion(cdp, deadline) {
  // This is an explicitly owned loopback control endpoint, not page networking.
  // Fetch blocks some valid OS-assigned CDP ports before making a TCP request.
  const endpoint = new URL('/json/version', localUrl(cdp.origin, 'CDP'))
  const limit = 1024 * 1024
  return new Promise((resolveBody, reject) => {
    let response, request, timer, finished = false
    const fail = error => {
      if (finished) return
      finished = true; clearTimeout(timer)
      response?.destroy(); request?.destroy(); reject(error)
    }
    request = httpGet(endpoint, { agent: false, maxHeaderSize: 16384, headers: { Accept: 'application/json' } }, incoming => {
      response = incoming
      if (incoming.statusCode < 200 || incoming.statusCode >= 300) {
        fail(new Error('CDP endpoint HTTP ' + incoming.statusCode + '; redirects are not followed')); return
      }
      const declared = incoming.headers['content-length']
      if (declared !== undefined && (!/^[0-9]+$/.test(declared) || Number(declared) > limit)) {
        fail(new Error('CDP version response exceeds the 1 MiB limit')); return
      }
      const chunks = []; let size = 0
      incoming.on('data', chunk => {
        if (finished) return
        size += chunk.length
        if (size > limit) { fail(new Error('CDP version response exceeds the 1 MiB limit')); return }
        chunks.push(chunk)
      })
      incoming.once('aborted', () => fail(new Error('CDP version response was aborted')))
      incoming.once('error', error => fail(new Error('CDP version response read failed', { cause: error })))
      incoming.once('end', () => {
        if (finished) return
        try {
          requireThat(incoming.complete, 'CDP version response ended before completion')
          const text = new TextDecoder('utf-8', { fatal: true }).decode(Buffer.concat(chunks, size))
          finished = true; clearTimeout(timer); resolveBody(text)
        } catch (error) { fail(error) }
      })
    })
    request.once('error', error => fail(new Error('CDP control request failed', { cause: error })))
    timer = setTimeout(() => fail(new Error('CDP endpoint request timed out')),
      Math.max(1, Math.min(8000, deadline - Date.now())))
  })
}
function options(argv) {
  const allowed = new Set(['--url', '--cdp-url', '--browser-pid', '--output', '--phase', '--target-id', '--previous', '--timeout-ms', '--actions', '--actions-sha256'])
  const values = new Map()
  for (let i = 0; i < argv.length; i += 2) {
    requireThat(allowed.has(argv[i]) && !values.has(argv[i]) && argv[i + 1] && !argv[i + 1].startsWith('--'), 'Use unique supported --option value arguments')
    values.set(argv[i], argv[i + 1])
  }
  const required = name => { requireThat(values.get(name), 'Missing argument ' + name); return values.get(name) }
  const url = localUrl(required('--url'), 'Deployment'), cdp = localUrl(required('--cdp-url'), 'CDP')
  const pidText = required('--browser-pid'), pid = Number(pidText)
  requireThat(/^[1-9][0-9]*$/.test(pidText) && Number.isSafeInteger(pid), 'Invalid expected browser PID')
  const phase = values.get('--phase') || 'boot'
  requireThat(['boot', 'offline'].includes(phase), 'Unsupported phase option')
  const timeout = Number(values.get('--timeout-ms') || 90000)
  requireThat(Number.isSafeInteger(timeout) && timeout >= 1000 && timeout <= 180000, 'Invalid timeout option')
  const requested = resolve(required('--output')), output = join(realpathSync.native(dirname(requested)), basename(requested))
  requireThat(!existsSync(output), 'Use a new output evidence directory; existing output is preserved')
  requireThat(values.has('--actions') === values.has('--actions-sha256'), 'actions file and SHA arguments must be supplied as a pair')
  const actions = values.has('--actions') ? readActions(required('--actions'), required('--actions-sha256')) : null
  let previous = null, previousBytes = null, previousPath = null, target = values.get('--target-id')
  if (phase === 'offline') {
    requireThat(target && /^[A-Za-z0-9_-]+$/.test(target), 'Offline phase requires the exact --target-id')
    previousPath = realpathSync.native(resolve(required('--previous')))
    previousBytes = readFileSync(previousPath)
    requireThat(previousBytes.length < 1024 * 1024, 'Previous boot record exceeds size limit')
    previous = JSON.parse(previousBytes.toString('utf8'))
    requireThat(previous.schema === SCHEMA && previous.status === 'BOOT_READY' && previous.phase === 'boot'
      && previous.target_id === target && previous.browser_pid === pid && previous.url === url.href
      && previous.cdp_url === cdp.origin && previous.browser_pid_verified === true
      && typeof previous.browser_ws_url === 'string' && typeof previous.slot?.sha256 === 'string', 'Previous boot record does not match this explicit URL/PID/target')
    const priorActions = previous.actions ?? null
    requireThat(actions ? priorActions && priorActions.path === actions.path && priorActions.sha256 === actions.sha256
      : priorActions === null, 'actions must match the exact previous boot path and SHA-256')
  } else requireThat(!target && !values.has('--previous'), 'Boot phase creates its own new target; target/previous options are offline-only')
  requireThat(Number(process.versions.node.split('.')[0]) >= 22 && typeof WebSocket === 'function', 'Explicit Node 22 or later with WebSocket is required')
  return { url, cdp, pid, phase, timeout, output, target, previous, previousBytes, previousPath, actions, mount: new URL('.', url).pathname }
}

class Cdp {
  constructor(socket, logPath, deadline) {
    this.socket = socket; this.logPath = logPath; this.deadline = deadline; this.next = 1; this.pending = new Map(); this.handlers = []
    this.errors = []; this.closed = false
    socket.addEventListener('message', event => {
      try {
        requireThat(typeof event.data === 'string' && event.data.length <= 40 * 1024 * 1024, 'Invalid or excessive CDP message')
        const message = JSON.parse(event.data)
        if (message.id !== undefined) {
          const call = this.pending.get(message.id)
          if (!call) return
          this.pending.delete(message.id); clearTimeout(call.timer)
          if (message.error) call.reject(new Error(call.method + ': ' + JSON.stringify(message.error)))
          else call.resolve(message.result || {})
        } else {
          appendFileSync(logPath, JSON.stringify({ at: new Date().toISOString(), ...message }) + '\n')
          for (const handler of this.handlers) handler(message)
        }
      } catch (error) { this.fail(error); this.socket.close() }
    })
    socket.addEventListener('error', event => { this.fail(new Error('CDP socket error', { cause: event.error })) })
    socket.addEventListener('close', () => { this.closed = true; this.fail(new Error('CDP socket closed'), false) })
  }
  fail(error, record = true) {
    if (record) this.errors.push(errorText(error))
    for (const call of this.pending.values()) { clearTimeout(call.timer); call.reject(error) }
    this.pending.clear()
  }
  static async connect(url, logPath, deadline) {
    const socket = new WebSocket(url)
    await new Promise((yes, no) => {
      const timer = setTimeout(() => { socket.close(); no(new Error('CDP connection timeout')) }, Math.max(1, Math.min(8000, deadline - Date.now())))
      socket.addEventListener('open', () => { clearTimeout(timer); yes() }, { once: true })
      socket.addEventListener('error', event => { clearTimeout(timer); no(new Error('CDP WebSocket connection failed', { cause: event.error })) }, { once: true })
    })
    return new Cdp(socket, logPath, deadline)
  }
  send(method, params = {}, sessionId) {
    requireThat(!this.closed && this.socket.readyState === WebSocket.OPEN && !this.errors.length, 'CDP connection is not healthy')
    const id = this.next++
    appendFileSync(this.logPath, JSON.stringify({ at: new Date().toISOString(), direction: 'send', id, method, params, sessionId }) + '\n')
    return new Promise((resolveCall, reject) => {
      const timer = setTimeout(() => { this.pending.delete(id); reject(new Error(method + ' timeout')) }, Math.max(1, Math.min(12000, this.deadline - Date.now())))
      this.pending.set(id, { method, resolve: resolveCall, reject, timer })
      try { this.socket.send(JSON.stringify({ id, method, params, ...(sessionId ? { sessionId } : {}) })) }
      catch (error) { clearTimeout(timer); this.pending.delete(id); reject(error) }
    })
  }
  async read(session, expression) {
    const result = await this.send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true }, session)
    requireThat(!result.exceptionDetails, 'Read-only page diagnostic threw: ' + JSON.stringify(result.exceptionDetails))
    return result.result?.value
  }
  async close() {
    if (this.socket.readyState === WebSocket.CLOSED) return
    await new Promise(done => {
      const timer = setTimeout(done, 1000)
      this.socket.addEventListener('close', () => { clearTimeout(timer); done() }, { once: true })
      this.socket.close()
    })
  }
}

const STATE = `(()=>{
 const rect=e=>{if(!e)return null;const r=e.getBoundingClientRect(),s=getComputedStyle(e);return{x:r.x,y:r.y,width:r.width,height:r.height,display:s.display,visibility:s.visibility,opacity:s.opacity}};
 const stage=document.getElementById('stage');
 return {url:location.href,ready:document.readyState,status:document.getElementById('status')?.textContent||'',
  errors:window.__caesuraErrors??null,coreErrors:window.__caesuraCore?.events?.filter(e=>/error|failed/i.test(e.kind))||[],
  log:document.getElementById('log')?.textContent||'',scene:document.getElementById('scene')?.value,
  stage:rect(stage),stageText:stage?.textContent||'',viewport:{width:innerWidth,height:innerHeight},
  presentation:[...document.querySelectorAll('#stage canvas,#stage .caesura-message,#stage img')].map(e=>({tag:e.tagName,rect:rect(e),text:(e.textContent||'').slice(0,500),naturalWidth:e.naturalWidth||0,width:e.width||0,height:e.height||0})),
  audio:{hook:!!window.__caesuraAudio,state:window.__caesuraAudio?.state,clock:window.__caesuraAudio?.currentTime},
  slotInput:document.getElementById('save-slot')?.value,slot:localStorage.getItem('caesura.save.1'),
  online:navigator.onLine,serviceWorker:navigator.serviceWorker?.controller?.scriptURL||null,
  wasmPin:self.__CAESURA_WASM_FILE__||null};
})()`

function samePackage(url, opt) {
  try { const value = new URL(url); return value.origin === opt.url.origin && value.pathname.startsWith(opt.mount) }
  catch { return false }
}
async function probe(opt) {
  mkdirSync(opt.output)
  const report = { schema: SCHEMA, status: 'RUNNING', phase: opt.phase, package_acceptance: 'NOT_EVALUATED',
    process_cleanup: 'CALLER_OWNED', browser_runtime: 'NOT_YET_OBSERVED', started_at: new Date().toISOString(),
    url: opt.url.href, cdp_url: opt.cdp.origin, browser_pid: opt.pid, browser_pid_verified: false,
    target_id: opt.target || null, checks: [], errors: [], callback_errors: [], page_exceptions: [],
    console_errors: [], outside_requests: [], unknown_media_requests: [], network: [], pending_requests: [], resources: [],
    actions: opt.actions ? { ...opt.actions, status: 'NOT_RUN', execution_phase: 'BOOT_ONLY', input_stable: null, steps: [] } : null,
    boundaries: { author_branch_journey: 'NOT_RUN', new_process_restore: 'NOT_RUN', pixel_equivalence: 'NOT_VERIFIED',
      speaker_output: 'NOT_VERIFIED', dynamic_media_completeness: 'NOT_PROVEN', package_bytes: 'CALLER_VERIFIES_RESPONSE_HASHES' } }
  let client, session, deliberateDetach = false, deadline = Date.now() + opt.timeout
  const requests = new Map(), jobs = new Set()
  let networkId = 0, mainFrame, previousLoader
  const check = (name, okay, detail) => { report.checks.push({ name, passed: !!okay, detail }); requireThat(okay, name + ': ' + JSON.stringify(detail)) }
  const fatal = () => {
    requireThat(Date.now() < deadline, 'Overall probe deadline exceeded')
    requireThat(!client?.errors.length && !report.callback_errors.length, 'CDP callback failure: ' + JSON.stringify([...(client?.errors || []), ...report.callback_errors]))
    requireThat(!report.page_exceptions.length && !report.console_errors.length, 'Page exception/console error observed')
  }
  async function until(description, read, predicate, milliseconds = 30000) {
    const end = Math.min(deadline, Date.now() + milliseconds)
    let latest
    do {
      fatal()
      try { latest = await read(); if (predicate(latest)) return latest }
      catch (error) {
        if (!/Execution context was destroyed|Cannot find context|Cannot find default execution context|Inspected target navigated/.test(String(error))) throw error
        // Navigation destroys its previous context; retain the observation.
        report.network.push({ kind: 'navigation_context_replaced', message: String(error) })
      }
      await sleep(50)
    } while (Date.now() < end)
    throw new Error(description + ' timed out; last=' + JSON.stringify(latest).slice(0, 2000))
  }
  async function readState(label) {
    const value = await client.read(session, STATE)
    writeJson(join(opt.output, label + '.json'), value)
    return value
  }
  function visible(state) {
    const screen = state.viewport
    return state.presentation.some(item => {
      const r = item.rect
      return r && r.width > 0 && r.height > 0 && r.x < screen.width && r.y < screen.height && r.x + r.width > 0 && r.y + r.height > 0
        && r.display !== 'none' && r.visibility !== 'hidden' && Number(r.opacity) > 0
        && (item.tag === 'CANVAS' && item.width > 0 && item.height > 0 || item.text.trim() || item.tag === 'IMG' && item.naturalWidth > 0)
    })
  }
  async function trustedClick(selector, unique = false, surface = null) {
    // Message spans can be replaced every frame. Scroll the explicitly declared
    // stable surface when present, then sample the current target below without
    // retaining its nodeId. Geometry remains read-only; input remains trusted.
    const document = await client.send('DOM.getDocument', {}, session)
    const scrollSelector = surface || selector
    const selected = await client.send('DOM.querySelector', { nodeId: document.root.nodeId, selector: scrollSelector }, session)
    requireThat(selected.nodeId > 0, 'Missing player UI control: ' + scrollSelector)
    await client.send('DOM.scrollIntoViewIfNeeded', { nodeId: selected.nodeId }, session)
    const point = await client.read(session, `(()=>{const matches=document.querySelectorAll(${JSON.stringify(selector)}),e=matches[0],surfaceSelector=${JSON.stringify(surface)},surfaces=surfaceSelector?document.querySelectorAll(surfaceSelector):[],surface=surfaces[0];if(!e||e.disabled)return null;const r=e.getBoundingClientRect(),s=getComputedStyle(e);const x=r.x+r.width/2,y=r.y+r.height/2,top=document.elementFromPoint(x,y);return{x,y,w:r.width,h:r.height,visible:s.display!=='none'&&s.visibility!=='hidden'&&Number(s.opacity)>0,inside:!!top&&(surface?top===surface||surface.contains(top):top===e||e.contains(top)),matches:matches.length,surface_matches:surfaces.length,surface_contains:!!surface&&surface.contains(e),vw:innerWidth,vh:innerHeight}})()`)
    requireThat(point && point.w > 0 && point.h > 0 && point.visible && point.inside && point.x >= 0 && point.y >= 0 && point.x < point.vw && point.y < point.vh, 'Player control is not visibly clickable: ' + selector)
    requireThat(!unique || point.matches === 1, 'actions click must identify exactly one visible player control: ' + selector)
    requireThat(!surface || point.surface_matches === 1 && point.surface_contains,
      'actions click requires its explicit unique surface to contain the target: ' + surface)
    for (const type of ['mouseMoved', 'mousePressed', 'mouseReleased']) await client.send('Input.dispatchMouseEvent', { type, x: point.x, y: point.y, ...(type === 'mouseMoved' ? {} : { button: 'left', clickCount: 1 }) }, session)
  }
  async function runActions() {
    if (!opt.actions || opt.phase !== 'boot') return
    report.actions.status = 'ACTIONS_RUNNING'
    try {
      for (const [index, step] of opt.actions.configuration.steps.entries()) {
        const result = { index: index + 1, action: step, status: 'ACTION_RUNNING', started_at: new Date().toISOString() }
        report.actions.steps.push(result)
        try {
          fatal()
          requireThat(sha(actionBytes(opt.actions.path)) === opt.actions.sha256, 'actions input changed before executing step ' + (index + 1))
          if (step.click !== undefined) {
            const click = typeof step.click === 'string' ? { selector: step.click, surface: null } : step.click
            await trustedClick(click.selector, true, click.surface)
          }
          else {
            const expression = `(()=>{const actionSelector=${JSON.stringify(step.wait.selector)},matches=document.querySelectorAll(actionSelector),e=matches[0];if(!e)return null;const r=e.getBoundingClientRect(),s=getComputedStyle(e),text=e.textContent||'';return{matches:matches.length,visible:r.width>0&&r.height>0&&r.x<innerWidth&&r.y<innerHeight&&r.x+r.width>0&&r.y+r.height>0&&s.display!=='none'&&s.visibility!=='hidden'&&Number(s.opacity)>0,contains:text.includes(${JSON.stringify(step.wait.text)}),text_excerpt:text.slice(0,4096)}})()`
            result.observed = await until('actions wait step ' + (index + 1), () => client.read(session, expression),
              value => value?.matches === 1 && value.visible === true && value.contains === true)
          }
          result.status = 'ACTION_PASS'
        } catch (error) { result.status = 'ACTION_FAIL'; result.error = String(error); throw error }
        finally { result.finished_at = new Date().toISOString() }
      }
      report.actions.status = 'ACTIONS_PASS'
    } catch (error) { report.actions.status = 'ACTIONS_FAIL'; throw error }
  }
  async function booted() {
    return until('Real packaged player startup', () => client.read(session, STATE), value => {
      requireThat(Array.isArray(value?.errors) || !value?.status, 'Player error diagnostic hook is unavailable')
      requireThat(!value?.errors?.length && !value?.coreErrors?.length && !/^FAILED|ERR:/i.test(value?.status || ''), 'Player reported boot/runtime errors: ' + JSON.stringify(value).slice(0, 4000))
      return value?.ready === 'complete' && /^parked:/.test(value.status) && value.scene && visible(value)
    }, Math.min(60000, opt.timeout))
  }
  try {
    const versionText = await readCdpVersion(opt.cdp, deadline)
    const version = JSON.parse(versionText), websocket = new URL(version.webSocketDebuggerUrl)
    requireThat(websocket.protocol === 'ws:' && websocket.hostname === opt.cdp.hostname && websocket.port === opt.cdp.port
      && !websocket.username && !websocket.password && !websocket.search && !websocket.hash && /^\/devtools\/browser\/[A-Za-z0-9_-]+$/.test(websocket.pathname), 'Returned WebSocket must stay on the explicit browser endpoint')
    if (opt.previous) check('same browser endpoint as successful boot', websocket.href === opt.previous.browser_ws_url, websocket.href)
    report.browser_ws_url = websocket.href; report.browser_version = version.Browser
    client = await Cdp.connect(websocket.href, join(opt.output, 'cdp.jsonl'), deadline)
    const browserInfo = await client.send('SystemInfo.getProcessInfo')
    const browsers = browserInfo.processInfo?.filter(item => item.type === 'browser') || []
    check('CDP identifies the expected unique browser PID', browsers.length === 1 && Number(browsers[0].id) === opt.pid, browsers)
    report.browser_pid_verified = true
    if (opt.phase === 'boot') {
      const target = await client.send('Target.createTarget', { url: 'about:blank' })
      requireThat(typeof target.targetId === 'string' && target.targetId.length > 0, 'Target.createTarget returned no target ID')
      report.target_id = target.targetId
    } else {
      const info = await client.send('Target.getTargetInfo', { targetId: opt.target })
      check('offline target is exactly the prior owned player page', info.targetInfo?.targetId === opt.target && info.targetInfo?.type === 'page' && info.targetInfo?.url === opt.url.href, info.targetInfo)
      report.previous = { path: opt.previousPath, sha256: sha(opt.previousBytes) }
    }
    const attached = await client.send('Target.attachToTarget', { targetId: report.target_id, flatten: true })
    session = attached.sessionId; requireThat(typeof session === 'string' && session, 'Missing attached CDP session')
    client.handlers.push(message => {
      if (message.sessionId !== session) return
      const event = message.params || {}
      try {
        if (message.method === 'Runtime.exceptionThrown') report.page_exceptions.push(event)
        if (message.method === 'Runtime.consoleAPICalled' && event.type === 'error') report.console_errors.push(event)
        if (message.method === 'Inspector.detached' && !deliberateDetach) throw new Error('Explicit page detached unexpectedly: ' + JSON.stringify(event))
        if (message.method === 'Network.requestWillBeSent') {
          if (requests.has(event.requestId) && event.redirectResponse) report.network.push({ kind: 'redirect', ...event.redirectResponse })
          const record = { sequence: ++networkId, request_id: event.requestId, url: event.request.url, type: event.type,
            frame_id: event.frameId, loader_id: event.loaderId, method: event.request.method, response: null, finished: false, failed: null }
          requests.set(event.requestId, record); report.network.push(record)
          if (!samePackage(record.url, opt)) report.outside_requests.push(record)
          if (/\.(?:png|jpe?g|webp|gif|avif|wav|ogg|mp3|flac|m4a|mp4|webm)(?:[?#]|$)/i.test(record.url)) {
            report.unknown_media_requests.push({ url: record.url, request_id: record.request_id,
              classification: 'STATIC_OR_DYNAMIC_NOT_DETERMINED_BY_PROBE' })
          }
        }
        if (message.method === 'Network.responseReceived') {
          const record = requests.get(event.requestId)
          requireThat(record, 'Network response has no observed request: ' + event.requestId)
          record.response = { url: event.response.url, status: event.response.status, mime_type: event.response.mimeType,
            from_service_worker: !!event.response.fromServiceWorker, from_disk_cache: !!event.response.fromDiskCache, protocol: event.response.protocol }
        }
        if (message.method === 'Network.loadingFailed') {
          const record = requests.get(event.requestId)
          requireThat(record, 'Failed network request has no observed start: ' + event.requestId)
          record.failed = event; record.finished = true
        }
        if (message.method === 'Network.loadingFinished') {
          const record = requests.get(event.requestId)
          requireThat(record, 'Completed network request has no observed start: ' + event.requestId)
          record.finished = true; record.encoded_bytes = event.encodedDataLength
          if (!samePackage(record.url, opt)) return
          const job = client.send('Network.getResponseBody', { requestId: event.requestId }, session).then(body => {
            const bytes = Buffer.from(body.body, body.base64Encoded ? 'base64' : 'utf8')
            record.body = { bytes: bytes.length, sha256: sha(bytes), body_encoding: body.base64Encoded ? 'base64' : 'utf8' }
            report.resources.push({ url: record.url, ...record.body, type: record.type, from_service_worker: record.response?.from_service_worker || false })
          }).catch(error => { record.body_error = String(error); report.callback_errors.push('Response body capture: ' + String(error)) }).finally(() => jobs.delete(job))
          jobs.add(job)
        }
      } catch (error) { report.callback_errors.push(String(error)) }
    })
    for (const method of ['Page.enable', 'Runtime.enable', 'DOM.enable']) await client.send(method, {}, session)
    await client.send('Network.enable', { maxTotalBufferSize: 100000000, maxResourceBufferSize: 30000000 }, session)
    await client.send('Network.setCacheDisabled', { cacheDisabled: true }, session)
    if (opt.phase === 'boot') {
      const navigation = await client.send('Page.navigate', { url: opt.url.href }, session)
      requireThat(!navigation.errorText && navigation.frameId, 'Player navigation failed: ' + JSON.stringify(navigation))
      mainFrame = navigation.frameId
    } else {
      const frame = await client.send('Page.getFrameTree', {}, session)
      mainFrame = frame.frameTree?.frame?.id; previousLoader = frame.frameTree?.frame?.loaderId
      requireThat(mainFrame && previousLoader, 'Offline page has no current frame/loader identity')
      await client.send('Network.emulateNetworkConditions', { offline: true, latency: 0, downloadThroughput: 0, uploadThroughput: 0 }, session)
      await client.send('Network.setBypassServiceWorker', { bypass: false }, session)
      // ignoreCache:true is Shift+reload, which also bypasses service workers.
      // HTTP caching stays disabled above and the owned server is already down.
      // Require an ordinary reload served by the worker and a fresh loader.
      await client.send('Page.reload', { ignoreCache: false, loaderId: previousLoader }, session)
      await until('Fresh offline document loader', () => client.send('Page.getFrameTree', {}, session), value => value.frameTree?.frame?.loaderId && value.frameTree.frame.loaderId !== previousLoader)
    }
    const initial = await booted()
    check('default player URL remains the explicit deployment', initial.url === opt.url.href, initial.url)
    check('packaged WASM pin stays at the explicit mount', initial.wasmPin === new URL('web-assets/glue.wasm', new URL('.', opt.url)).href, initial.wasmPin)
    report.browser_runtime = 'OBSERVED'; report.initial = await readState('initial')
    check('existing audio state diagnostic is available', initial.audio.hook && ['none', 'suspended', 'running'].includes(initial.audio.state), initial.audio)
    await trustedClick('#refresh-slots')
    const unlocked = await until('AudioContext unlock from trusted player UI gesture', () => client.read(session, STATE), value => value?.audio?.state === 'running')
    report.audio = { before: initial.audio, after: unlocked.audio, gesture: 'Input.dispatchMouseEvent on #refresh-slots', pcm: 'NOT_MEASURED' }
    await runActions()
    if (opt.phase === 'boot') {
      check('fresh profile slot 1 is empty before this UI save', initial.slot === null && initial.slotInput === '1', { present: initial.slot !== null, input: initial.slotInput })
      await trustedClick('#save-now')
      const saved = await until('Save Current writes a real slot', () => client.read(session, STATE), value => {
        requireThat(!value.log.includes('save FAILED'), 'Player UI reports save failure')
        return typeof value.slot === 'string' && value.slot.length > 0 && value.log.includes('saved current position to slot 1')
      })
      requireThat(saved.slot.length <= 1024 * 1024, 'Saved slot exceeds diagnostic limit')
      const payload = JSON.parse(saved.slot)
      check('real saved slot identifies a scene and token', typeof payload.scene === 'string' && Number.isFinite(payload.token), { scene: payload.scene, token: payload.token })
      report.slot = { slot: 1, bytes: Buffer.byteLength(saved.slot), sha256: sha(Buffer.from(saved.slot)), scene: payload.scene, token: payload.token }
      const loadSelector = await client.read(session, `(()=>{const rows=[...document.querySelectorAll('#saves .save-entry')],i=rows.findIndex(r=>r.querySelector('.slot-num')?.textContent==='01');if(i<0)return null;const buttons=[...rows[i].querySelectorAll('button')],b=buttons.findIndex(x=>x.textContent==='Load');return b<0?null:'#saves .save-entry:nth-child('+(i+1)+') button:nth-of-type('+(b+1)+')'})()`)
      requireThat(loadSelector, 'Saved slot has no exact Load UI button')
      await trustedClick(loadSelector)
      const loaded = await until('Real UI Load completion', () => client.read(session, STATE), value => {
        requireThat(!/^load: ERR|^FAILED/.test(value.status), 'Player UI load failed: ' + value.status)
        return /^load:/.test(value.status) && value.log.includes('load result:')
      })
      check('UI Load keeps original stored slot bytes', sha(Buffer.from(loaded.slot || '')) === report.slot.sha256, loaded.status)
      report.ui_save_load = { status: 'OBSERVED', restored_status: loaded.status, cross_process_restore: 'NOT_RUN' }
    } else {
      check('offline reload retained the original saved bytes', sha(Buffer.from(initial.slot || '')) === opt.previous.slot.sha256, { slot: 1 })
      report.slot = opt.previous.slot
      check('offline document reports network disabled', initial.online === false, initial.online)
    }
    const sw = await until('Active service worker controls this player', async () => {
      return client.read(session, `(async()=>{if(!navigator.serviceWorker?.controller)return null;const r=await navigator.serviceWorker.ready;return{controller:navigator.serviceWorker.controller.scriptURL,scope:r.scope,active:r.active?.scriptURL,state:r.active?.state}})()`)
    }, value => value && value.controller === new URL('sw.js', new URL('.', opt.url)).href && value.active === value.controller && value.state === 'activated' && opt.url.href.startsWith(value.scope))
    report.service_worker = sw
    const document = await client.send('DOM.getDocument', {}, session)
    const stageNode = await client.send('DOM.querySelector', { nodeId: document.root.nodeId, selector: '#stage' }, session)
    await client.send('DOM.scrollIntoViewIfNeeded', { nodeId: stageNode.nodeId }, session)
    const final = await readState('final')
    check('final presentation has an on-screen message/canvas/image', visible(final), final.presentation)
    check('player diagnostics contain no errors', Array.isArray(final.errors) && final.errors.length === 0 && final.coreErrors.length === 0, { errors: final.errors, core: final.coreErrors })
    await until('All observed package requests finished', async () => { await Promise.all([...jobs]); return [...requests.values()] }, values => values.every(value => value.finished))
    fatal()
    const bad = [...requests.values()].filter(record => record.failed || !record.response || record.response.status < 200 || record.response.status >= 400 || record.body_error)
    check('observed requests have successful completed responses', bad.length === 0, bad)
    check('no request left the explicit package mount', report.outside_requests.length === 0, report.outside_requests)
    for (const path of ['web-assets/glue.wasm', 'scripts/index.json', 'cache/story/story.lua']) {
      const expected = new URL(path, new URL('.', opt.url)).href
      check('required player resource completed: ' + path, report.resources.some(value => value.url === expected && value.bytes > 0), expected)
    }
    check('runtime loaded actual packaged Lua modules', report.resources.some(value => value.url.startsWith(new URL('scripts/', new URL('.', opt.url)).href) && /\.lua$/.test(new URL(value.url).pathname) && value.bytes > 0), report.resources.length)
    if (opt.phase === 'offline') check('offline document came through the package service worker', [...requests.values()].some(record => record.type === 'Document' && record.frame_id === mainFrame && record.response?.from_service_worker === true && record.finished), mainFrame)
    const shot = await client.send('Page.captureScreenshot', { format: 'png' }, session)
    const pixels = Buffer.from(shot.data, 'base64'); requireThat(pixels.length > 8, 'Empty screenshot response')
    writeFileSync(join(opt.output, 'player.png'), pixels, { flag: 'wx' }); report.screenshot = { file: 'player.png', sha256: sha(pixels), pixel_equivalence: 'NOT_VERIFIED' }
    const finalPids = await client.send('SystemInfo.getProcessInfo')
    const lastBrowsers = finalPids.processInfo?.filter(item => item.type === 'browser') || []
    check('same unique browser PID after probe', lastBrowsers.length === 1 && Number(lastBrowsers[0].id) === opt.pid, lastBrowsers)
    report.status = 'PROBE_PASS'
  } catch (error) { report.status = 'PROBE_FAIL'; report.errors.push(errorText(error)) }
  finally {
    await Promise.allSettled([...jobs])
    report.pending_requests = [...requests.values()].filter(value => !value.finished)
    if (client && session) {
      client.deadline = Date.now() + 5000 // bounded detach still runs after the probe deadline
      deliberateDetach = true
      try { await client.send('Target.detachFromTarget', { sessionId: session }); report.probe_session_detached = true }
      catch (error) { report.probe_session_detached = false; report.status = 'PROBE_FAIL'; report.errors.push('Probe session detach failed: ' + String(error)) }
    }
    if (client) await client.close()
    // Re-evaluate after detach/socket closure: late network callbacks must not
    // turn into discarded evidence after the last ordinary success assertion.
    await Promise.allSettled([...jobs])
    report.pending_requests = [...requests.values()].filter(value => !value.finished)
    const failed = [...requests.values()].filter(value => value.failed || value.body_error)
    if (report.pending_requests.length || failed.length || report.page_exceptions.length
        || report.console_errors.length || report.outside_requests.length || report.callback_errors.length || client?.errors.length) {
      report.status = 'PROBE_FAIL'
      report.errors.push(...(client?.errors || []), ...report.callback_errors,
        'Final event guard: ' + JSON.stringify({ pending_requests: report.pending_requests.length,
          failed_requests: failed.length, page_exceptions: report.page_exceptions.length,
          console_errors: report.console_errors.length, outside_requests: report.outside_requests.length }))
    }
    report.target_disposition = 'LEFT_FOR_CALLER'; report.finished_at = new Date().toISOString()
    if (opt.actions) {
      try {
        report.actions.input_stable = sha(actionBytes(opt.actions.path)) === opt.actions.sha256
        requireThat(report.actions.input_stable, 'actions input changed during the probe')
      } catch (error) {
        report.actions.input_stable = false; report.status = 'PROBE_FAIL'; report.errors.push('actions final identity: ' + String(error))
      }
    }
    if (opt.phase === 'boot' && report.status === 'PROBE_PASS') {
      const record = { schema: SCHEMA, status: 'BOOT_READY', phase: 'boot', url: report.url, cdp_url: report.cdp_url,
        browser_ws_url: report.browser_ws_url, browser_pid: opt.pid, browser_pid_verified: true, target_id: report.target_id,
        slot: report.slot, service_worker: report.service_worker,
        actions: opt.actions ? { path: opt.actions.path, sha256: opt.actions.sha256 } : null }
      writeJson(join(opt.output, 'session.json'), record)
    }
    writeJson(join(opt.output, 'report.json'), report)
  }
  return report
}

try {
  const opt = options(process.argv.slice(2)), result = await probe(opt)
  console.log(JSON.stringify({ status: result.status, package_acceptance: 'NOT_EVALUATED', report: join(opt.output, 'report.json'), target_id: result.target_id }))
  process.exitCode = result.status === 'PROBE_PASS' ? 0 : 1
} catch (error) {
  console.error(JSON.stringify({ status: 'PROBE_FAIL', package_acceptance: 'NOT_EVALUATED', error: errorText(error) }))
  process.exitCode = 2
}
