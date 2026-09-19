import test from 'node:test'
import assert from 'node:assert/strict'
import { spawn } from 'node:child_process'
import { createServer } from 'node:http'
import { createHash } from 'node:crypto'
import { mkdtempSync, mkdirSync, readFileSync, writeFileSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

// Protocol fixtures exercise the real CLI. No Chrome, engine, game runtime,
// package HTTP server, or browser-equivalent success is claimed here.
const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..')
const script = join(root, 'scripts/web_package_probe.mjs')
const invoke = args => new Promise((done, reject) => {
  const child = spawn(process.execPath, [script, ...args], { windowsHide: true })
  let stdout = '', stderr = ''
  child.stdout.on('data', x => { stdout += x })
  child.stderr.on('data', x => { stderr += x })
  child.on('error', reject)
  const timer = setTimeout(() => { child.kill(); reject(new Error('Probe CLI did not terminate')) }, 12000)
  child.on('close', code => { clearTimeout(timer); done({ code, stdout, stderr }) })
})
const directory = t => { const path = mkdtempSync(join(tmpdir(), 'caesura-web-probe-')); t.after(() => rmSync(path, { recursive: true, force: true })); return path }
const argumentsFor = out => ['--url', 'http://127.0.0.1:12345/games/example/', '--cdp-url', 'http://127.0.0.1:12346', '--browser-pid', '4242', '--output', out]

function packet(value) {
  const payload = Buffer.from(JSON.stringify(value))
  if (payload.length < 126) return Buffer.concat([Buffer.from([0x81, payload.length]), payload])
  const head = Buffer.alloc(4); head[0] = 0x81; head[1] = 126; head.writeUInt16BE(payload.length, 2)
  return Buffer.concat([head, payload])
}
async function protocolFixture(t, responder, versionOverride, ports = [0]) {
  const commands = [], requests = [], sockets = new Set()
  const server = createServer((req, res) => {
    requests.push(req.url)
    if (typeof versionOverride === 'function') { versionOverride(req, res); return }
    res.setHeader('Content-Type', 'application/json')
    res.end(JSON.stringify(versionOverride || { Browser: 'ProtocolFixture/0', webSocketDebuggerUrl: `ws://127.0.0.1:${server.address().port}/devtools/browser/fixture` }))
  })
  server.on('connection', socket => { sockets.add(socket); socket.on('close', () => sockets.delete(socket)) })
  server.on('upgrade', (req, socket) => {
    const accept = createHash('sha1').update(req.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64')
    socket.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`)
    let buffer = Buffer.alloc(0)
    socket.on('data', input => {
      buffer = Buffer.concat([buffer, input])
      while (buffer.length >= 2) {
        let size = buffer[1] & 127, offset = 2
        if (size === 126) { if (buffer.length < 4) return; size = buffer.readUInt16BE(2); offset = 4 }
        if (size === 127) { socket.destroy(); return }
        const masked = !!(buffer[1] & 128), maskBytes = masked ? 4 : 0
        if (buffer.length < offset + maskBytes + size) return
        const opcode = buffer[0] & 15, mask = buffer.subarray(offset, offset + maskBytes)
        const data = Buffer.from(buffer.subarray(offset + maskBytes, offset + maskBytes + size))
        buffer = buffer.subarray(offset + maskBytes + size)
        if (masked) for (let i = 0; i < data.length; i++) data[i] ^= mask[i % 4]
        if (opcode === 8) { socket.end(Buffer.from([0x88, 0])); return }
        if (opcode !== 1) continue
        const call = JSON.parse(data.toString()); commands.push(call)
        const response = responder(call)
        if (response === 'close') { socket.destroy(); return }
        if (response) {
          for (const event of response.events || []) socket.write(packet(event))
          const { events, ...answer } = response
          socket.write(packet({ id: call.id, ...answer }))
        }
      }
    })
  })
  const bindAttempts = []
  for (const port of ports) {
    try {
      await new Promise((done, reject) => {
        const failed = error => { server.off('listening', ready); reject(error) }
        const ready = () => { server.off('error', failed); done() }
        server.once('error', failed); server.once('listening', ready)
        server.listen({ port, host: '127.0.0.1', exclusive: true })
      })
      bindAttempts.push({ requested: port, bound: server.address().port }); break
    } catch (error) {
      bindAttempts.push({ requested: port, error: error.code })
      if (!['EADDRINUSE', 'EACCES'].includes(error.code)) throw error
    }
  }
  assert.ok(server.listening, 'No configured fixture port could be owned; existing listeners were untouched: ' + JSON.stringify(bindAttempts))
  t.after(async () => { for (const socket of sockets) socket.destroy(); await new Promise(done => server.close(done)) })
  return { cdp: `http://127.0.0.1:${server.address().port}`, commands, requests, bindAttempts }
}

function fixtureArgs(out, fixture) { const args = argumentsFor(out); args[3] = fixture.cdp; return args }
function readReport(out, invocation = null) {
  try { return JSON.parse(readFileSync(join(out, 'report.json'), 'utf8')) }
  catch (error) { throw new Error('Missing/invalid probe report: ' + String(error) + '; CLI=' + JSON.stringify(invocation), { cause: error }) }
}

function actionsFile(base, value = { schema: 1, steps: [{ click: '#route-a' }] }, name = 'actions.json') {
  const path = join(base, name), bytes = Buffer.from(typeof value === 'string' ? value : JSON.stringify(value))
  writeFileSync(path, bytes)
  return { path, sha256: createHash('sha256').update(bytes).digest('hex'), args: ['--actions', path, '--actions-sha256', createHash('sha256').update(bytes).digest('hex')] }
}

test('real CLI refuses missing/duplicate/unknown options', async t => {
  const base = directory(t)
  for (const args of [[], [...argumentsFor(join(base, 'a')), '--browser-pid', '4242'], [...argumentsFor(join(base, 'b')), '--browser', 'chrome']]) {
    const result = await invoke(args); assert.notEqual(result.code, 0); assert.match(result.stdout + result.stderr, /argument|option|Missing/i)
  }
})
test('real CLI refuses nonloopback or ambiguous deployment/CDP URLs and invalid PID', async t => {
  const base = directory(t)
  for (const [at, value] of [[1, 'https://example.invalid/'], [1, 'http://127.0.0.1:12345/games/../'], [1, 'http://user@127.0.0.1/'], [3, 'http://127.0.0.1:12346/json/list'], [5, '0'], [5, '42.2']]) {
    const args = argumentsFor(join(base, String(at) + '-' + createHash('sha1').update(value).digest('hex'))); args[at] = value
    const result = await invoke(args); assert.notEqual(result.code, 0); assert.match(result.stdout + result.stderr, /URL|loopback|PID|path/i)
  }
})
test('existing output evidence is preserved before contacting CDP', async t => {
  const base = directory(t), out = join(base, 'existing'); mkdirSync(out); writeFileSync(join(out, 'original'), 'keep')
  const result = await invoke(argumentsFor(out)); assert.notEqual(result.code, 0); assert.equal(readFileSync(join(out, 'original'), 'utf8'), 'keep'); assert.match(result.stdout + result.stderr, /exist|new output/i)
})
test('offline requires the exact target and prior successful boot record', async t => {
  const base = directory(t), out = join(base, 'offline')
  const result = await invoke([...argumentsFor(out), '--phase', 'offline']); assert.notEqual(result.code, 0); assert.match(result.stdout + result.stderr, /target|previous/i)
})

test('offline reload disables network and HTTP cache while preserving service worker handling', async t => {
  const base = directory(t), out = join(base, 'offline-controls'), target = 'owned-player'
  const url = argumentsFor(out)[1]
  let reloaded = false
  const f = await protocolFixture(t, call => {
    if (call.method === 'SystemInfo.getProcessInfo') return { result: { processInfo: [{ type: 'browser', id: 4242 }] } }
    if (call.method === 'Target.getTargetInfo') return { result: { targetInfo: { targetId: target, type: 'page', url } } }
    if (call.method === 'Target.attachToTarget') return { result: { sessionId: 'owned-session' } }
    if (call.method === 'Page.getFrameTree') return { result: { frameTree: { frame: { id: 'frame', loaderId: reloaded ? 'after' : 'before' } } } }
    if (call.method === 'Page.reload') { reloaded = true; return { result: {} } }
    if (call.method === 'Runtime.evaluate') return { error: { message: 'fixture stops after reload; no game acceptance' } }
    return { result: {} }
  })
  const prior = join(base, 'prior.json')
  writeFileSync(prior, JSON.stringify({ schema: 'caesura.web-package-probe.v1', status: 'BOOT_READY', phase: 'boot',
    target_id: target, browser_pid: 4242, browser_pid_verified: true, url, cdp_url: f.cdp,
    browser_ws_url: f.cdp.replace('http:', 'ws:') + '/devtools/browser/fixture', slot: { sha256: 'a'.repeat(64) } }))
  const result = await invoke([...fixtureArgs(out, f), '--phase', 'offline', '--target-id', target, '--previous', prior])
  assert.notEqual(result.code, 0)
  const reload = f.commands.find(x => x.method === 'Page.reload')
  assert.ok(reload, JSON.stringify(readReport(out, result)))
  assert.equal(reload.params.ignoreCache, false, 'hard reload bypasses the service worker entirely')
  assert.equal(reload.params.loaderId, 'before')
  assert.equal(f.commands.find(x => x.method === 'Network.setCacheDisabled').params.cacheDisabled, true)
  assert.equal(f.commands.find(x => x.method === 'Network.setBypassServiceWorker').params.bypass, false)
  assert.equal(f.commands.find(x => x.method === 'Network.emulateNetworkConditions').params.offline, true)
})
test('returned remote browser websocket cannot redirect the explicit endpoint', async t => {
  const base = directory(t), out = join(base, 'endpoint')
  const f = await protocolFixture(t, () => null, { webSocketDebuggerUrl: 'ws://example.invalid/devtools/browser/no' })
  const result = await invoke(fixtureArgs(out, f)); assert.notEqual(result.code, 0); assert.equal(f.commands.length, 0); assert.match(JSON.stringify(readReport(out)), /endpoint|WebSocket/i)
})
test('wrong browser PID is rejected before any Target or Runtime mutation', async t => {
  const base = directory(t), out = join(base, 'wrong-pid')
  const f = await protocolFixture(t, call => call.method === 'SystemInfo.getProcessInfo' ? { result: { processInfo: [{ type: 'browser', id: 9999 }] } } : { error: { message: 'Unexpected command' } })
  const result = await invoke(fixtureArgs(out, f)); assert.notEqual(result.code, 0)
  assert.deepEqual(f.commands.map(x => x.method), ['SystemInfo.getProcessInfo']); assert.equal(readReport(out).browser_pid_verified, false)
})
test('ambiguous browser process response is refused without page fallback', async t => {
  const base = directory(t), out = join(base, 'multiple-pids')
  const f = await protocolFixture(t, () => ({ result: { processInfo: [{ type: 'browser', id: 4242 }, { type: 'browser', id: 4243 }] } }))
  const result = await invoke(fixtureArgs(out, f)); assert.notEqual(result.code, 0); assert.equal(f.commands.length, 1); assert.deepEqual(f.requests, ['/json/version'])
})
test('unsupported PID protocol fails closed and never enumerates old pages', async t => {
  const base = directory(t), out = join(base, 'unsupported')
  const f = await protocolFixture(t, () => ({ error: { code: -32601, message: 'SystemInfo unsupported' } }))
  const result = await invoke(fixtureArgs(out, f)); assert.notEqual(result.code, 0); assert.equal(f.commands.length, 1, JSON.stringify(readReport(out))); assert.equal(readReport(out).status, 'PROBE_FAIL')
})
test('connection lost after verified PID preserves the failed target creation attempt', async t => {
  const base = directory(t), out = join(base, 'closed')
  const f = await protocolFixture(t, call => call.method === 'SystemInfo.getProcessInfo' ? { result: { processInfo: [{ type: 'browser', id: 4242 }] } } : 'close')
  const result = await invoke(fixtureArgs(out, f)); assert.notEqual(result.code, 0); assert.deepEqual(f.commands.map(x => x.method), ['SystemInfo.getProcessInfo', 'Target.createTarget'], JSON.stringify(readReport(out))); assert.equal(readReport(out).browser_pid_verified, true)
})

test('raw network callback errors cannot disappear from the failed probe receipt', async t => {
  const base = directory(t), out = join(base, 'callback-error')
  const f = await protocolFixture(t, call => {
    if (call.method === 'SystemInfo.getProcessInfo') return { result: { processInfo: [{ type: 'browser', id: 4242 }] } }
    if (call.method === 'Target.createTarget') return { result: { targetId: 'new-only' } }
    if (call.method === 'Target.attachToTarget') return { result: { sessionId: 'owned-session' } }
    if (call.method === 'Network.enable') return { result: {}, events: [{ sessionId: 'owned-session', method: 'Network.loadingFailed', params: { requestId: 'missing-start', errorText: 'fixture-request-failed', canceled: false } }] }
    if (call.method === 'Page.navigate') return { result: { frameId: 'frame' } }
    return { result: {} }
  })
  const result = await invoke(fixtureArgs(out, f)); assert.notEqual(result.code, 0)
  const report = readReport(out)
  assert.equal(report.status, 'PROBE_FAIL'); assert.ok(report.callback_errors.length > 0, JSON.stringify(report))
  assert.match(readFileSync(join(out, 'cdp.jsonl'), 'utf8'), /fixture-request-failed/)
  assert.equal(report.probe_session_detached, true)
  assert.equal(f.commands.some(x => x.method === 'Target.getTargets' || x.method === 'Browser.close' || x.method === 'Target.closeTarget'), false)
})
test('offline cannot borrow a boot record for another exact target', async t => {
  const base = directory(t), out = join(base, 'wrong-target'), previous = join(base, 'previous.json')
  writeFileSync(previous, JSON.stringify({ schema: 'caesura.web-package-probe.v1', status: 'BOOT_READY', phase: 'boot', target_id: 'different', browser_pid: 4242, browser_pid_verified: true, url: 'http://127.0.0.1:12345/games/example/', cdp_url: 'http://127.0.0.1:12346', browser_ws_url: 'ws://127.0.0.1:12346/devtools/browser/id', slot: { sha256: '0'.repeat(64) } }))
  const result = await invoke([...argumentsFor(out), '--phase', 'offline', '--target-id', 'requested', '--previous', previous])
  assert.notEqual(result.code, 0); assert.match(result.stdout + result.stderr, /boot record.*target/i)
})

test('actions require paired explicit file and locked SHA before any CDP request', async t => {
  const base = directory(t), action = actionsFile(base)
  const f = await protocolFixture(t, () => ({ error: { message: 'must not contact CDP' } }))
  for (const [i, extra] of [['file', ['--actions', action.path]], ['sha', ['--actions-sha256', action.sha256]],
    ['bad-sha', ['--actions', action.path, '--actions-sha256', 'invalid']],
    ['mismatch', ['--actions', action.path, '--actions-sha256', '0'.repeat(64)]]]) {
    const result = await invoke([...fixtureArgs(join(base, i), f), ...extra])
    assert.notEqual(result.code, 0); assert.match(result.stdout + result.stderr, /actions.*(?:pair|SHA|hash)|both.*actions/i)
  }
  assert.equal(f.requests.length, 0)
})

test('actions reject unbounded or unknown executable configuration', async t => {
  const base = directory(t), f = await protocolFixture(t, () => ({ error: { message: 'must not contact CDP' } }))
  const invalid = [
    '{', [], { schema: true, steps: [{ click: '#a' }] }, { schema: 2, steps: [{ click: '#a' }] },
    { schema: 1, steps: [] }, { schema: 1, steps: Array.from({ length: 21 }, () => ({ click: '#a' })) },
    { schema: 1, steps: [{ click: '#a' }], eval: 'anything' },
    { schema: 1, steps: [{ click: '' }] }, { schema: 1, steps: [{ click: 'x'.repeat(513) }] },
    { schema: 1, steps: [{ click: '#a', wait: { selector: '#b', text: 'b' } }] },
    { schema: 1, steps: [{ click: { selector: '#a' } }] },
    { schema: 1, steps: [{ click: { selector: '#a', surface: '#stage', force: true } }] },
    { schema: 1, steps: [{ eval: 'localStorage.setItem("x","y")' }] },
    { schema: 1, steps: [{ wait: { selector: '#b' } }] },
    { schema: 1, steps: [{ wait: { selector: '#b', text: '', timeout: 30000 } }] },
    { schema: 1, steps: [{ wait: { selector: '#b', text: 'x'.repeat(2049) } }] },
    ' '.repeat(65537),
  ]
  for (const [i, value] of invalid.entries()) {
    const action = actionsFile(base, value, `invalid-${i}.json`)
    const result = await invoke([...fixtureArgs(join(base, `invalid-out-${i}`), f), ...action.args])
    assert.notEqual(result.code, 0); assert.match(result.stdout + result.stderr, /actions/i)
    assert.doesNotMatch(result.stdout + result.stderr, /unique supported --option/)
  }
  assert.equal(f.requests.length, 0)
})

test('actions preserve their initially locked identity and fail on actual input replacement during the probe', async t => {
  const base = directory(t), out = join(base, 'changed-actions'), action = actionsFile(base)
  const f = await protocolFixture(t, call => {
    writeFileSync(action.path, JSON.stringify({ schema: 1, steps: [{ click: '#other-route' }] }))
    return { result: { processInfo: [{ type: 'browser', id: 9999 }] } }
  })
  const result = await invoke([...fixtureArgs(out, f), ...action.args])
  assert.notEqual(result.code, 0)
  const report = readReport(out)
  assert.equal(report.actions.sha256, action.sha256)
  assert.equal(report.actions.input_stable, false)
  assert.match(JSON.stringify(report.errors), /actions.*changed/i)
  assert.equal(report.status, 'PROBE_FAIL')
  assert.deepEqual(f.commands.map(x => x.method), ['SystemInfo.getProcessInfo'])
})

test('actions are bound to the previous offline session instead of being silently substituted', async t => {
  const base = directory(t), previous = join(base, 'previous-actions.json'), action = actionsFile(base)
  const f = await protocolFixture(t, () => ({ error: { message: 'must not contact CDP' } }))
  const prior = { schema: 'caesura.web-package-probe.v1', status: 'BOOT_READY', phase: 'boot', target_id: 'owned-target', browser_pid: 4242, browser_pid_verified: true,
    url: 'http://127.0.0.1:12345/games/example/', cdp_url: f.cdp, browser_ws_url: f.cdp.replace('http:', 'ws:') + '/devtools/browser/fixture', slot: { sha256: '0'.repeat(64) },
    actions: { path: action.path, sha256: action.sha256 } }
  writeFileSync(previous, JSON.stringify(prior))
  for (const [i, extra] of [['omitted', []], ['other', actionsFile(base, { schema: 1, steps: [{ click: '#other' }] }, 'other.json').args]]) {
    const result = await invoke([...fixtureArgs(join(base, i), f), '--phase', 'offline', '--target-id', 'owned-target', '--previous', previous, ...extra])
    assert.notEqual(result.code, 0); assert.match(result.stdout + result.stderr, /actions.*(?:prior|previous|boot)|(?:prior|previous|boot).*actions/i)
  }
  assert.equal(f.requests.length, 0)
})

async function actionProtocol(t, base, steps, failure) {
  const action = actionsFile(base, { schema: 1, steps }), out = join(base, 'action-sequence'), clicks = []
  let selected = null
  const f = await protocolFixture(t, call => {
    if (call.method === 'SystemInfo.getProcessInfo') return { result: { processInfo: [{ type: 'browser', id: 4242 }] } }
    if (call.method === 'Target.createTarget') return { result: { targetId: 'new-only' } }
    if (call.method === 'Target.attachToTarget') return { result: { sessionId: 'owned-session' } }
    if (call.method === 'Page.navigate') return { result: { frameId: 'frame' } }
    if (call.method === 'DOM.getDocument') return { result: { root: { nodeId: 1 } } }
    if (call.method === 'DOM.querySelector') {
      selected = call.params.selector
      if (selected === '#save-now') return { error: { message: 'fixture intentionally stops before product save' } }
      if (failure === 'stale-span' && selected.startsWith('#stage')) return { result: { nodeId: selected === '#stage' ? 10 : 20 } }
      return { result: { nodeId: selected === failure ? 0 : 2 } }
    }
    if (call.method === 'DOM.scrollIntoViewIfNeeded' && failure === 'stale-span' && call.params.nodeId === 20) {
      return { error: { code: -32000, message: 'Could not find node with given id' } }
    }
    if (call.method === 'Input.dispatchMouseEvent' && call.params.type === 'mouseReleased') clicks.push(selected)
    if (call.method === 'Runtime.evaluate') {
      const expression = call.params.expression
      if (expression.includes('document.elementFromPoint')) {
        // Input uses the freshly observed target geometry, which can differ
        // from the stable ancestor selected only for scrolling.
        const target = expression.match(/querySelectorAll\(("(?:\\.|[^"\\])*")\)/)
        if (target) selected = JSON.parse(target[1])
        return { result: { result: { value: { x: 50, y: 50, w: 20, h: 20, visible: true,
        inside: selected === '#refresh-slots' || failure !== 'surface-obscured', matches: 1, vw: 1280, vh: 900,
        surface_matches: failure === 'surface-ambiguous' ? 2 : 1, surface_contains: failure !== 'surface-outside' } } } }
      }
      if (expression.includes('actionSelector')) return { result: { result: { value: { visible: failure !== 'wait-timeout', contains: failure !== 'wait-timeout', matches: 1, text_excerpt: 'Route A 存档点 arrived' } } } }
      return { result: { result: { value: { url: 'http://127.0.0.1:12345/games/example/', ready: 'complete', status: 'parked: select', scene: 'story',
        errors: [], coreErrors: [], log: '', slot: null, slotInput: '1', wasmPin: 'http://127.0.0.1:12345/games/example/web-assets/glue.wasm',
        audio: { hook: true, state: 'running' }, viewport: { width: 1280, height: 900 },
        presentation: [{ tag: 'DIV', text: 'Visible fixture', rect: { x: 0, y: 0, width: 100, height: 50, display: 'block', visibility: 'visible', opacity: '1' } }] } } } }
    }
    return { result: {} }
  })
  const result = await invoke([...fixtureArgs(out, f), ...action.args, '--timeout-ms', '1500'])
  assert.notEqual(result.code, 0, 'fixture never simulates product-save success')
  return { result, report: readReport(out, result), commands: f.commands, clicks, action }
}

test('actions execute only the explicit trusted UI sequence after audio and before saving', async t => {
  const base = directory(t), steps = [{ click: '#route-a' }, { wait: { selector: '#stage .message', text: '存档点' } }, { click: '#continue-to-save' }]
  const result = await actionProtocol(t, base, steps)
  assert.deepEqual(result.clicks, ['#refresh-slots', '#route-a', '#continue-to-save'])
  assert.equal(result.report.actions.status, 'ACTIONS_PASS')
  assert.equal(result.report.actions.input_stable, true)
  assert.deepEqual(result.report.actions.steps.map(x => x.status), ['ACTION_PASS', 'ACTION_PASS', 'ACTION_PASS'])
  assert.match(JSON.stringify(result.report.errors), /intentionally stops before product save/)
  assert.equal(result.commands.filter(x => x.method === 'Input.dispatchMouseEvent').length, 9)
  const waitRead = result.commands.find(x => x.method === 'Runtime.evaluate' && x.params.expression.includes('actionSelector'))
  assert.ok(waitRead); assert.match(waitRead.params.expression, /#stage \.message/)
})

test('actions fail on missing controls or unsatisfied text without attempting a save', async t => {
  const base = directory(t)
  for (const [name, steps, failure] of [['missing', [{ click: '#missing' }], '#missing'],
    ['timeout', [{ wait: { selector: '#stage', text: 'not present' } }], 'wait-timeout']]) {
    const folder = join(base, name); mkdirSync(folder)
    const result = await actionProtocol(t, folder, steps, failure)
    assert.equal(result.report.actions.status, 'ACTIONS_FAIL')
    assert.equal(result.report.actions.steps[0].status, 'ACTION_FAIL')
    assert.equal(result.report.actions.input_stable, true)
    assert.equal(result.commands.some(x => x.method === 'DOM.querySelector' && x.params.selector === '#save-now'), false)
    assert.deepEqual(result.clicks, ['#refresh-slots'])
  }
})

test('actions surface clicks require the explicit unique containing surface and reject outside overlays', async t => {
  const base = directory(t), selector = '#stage .caesura-message span:nth-child(1)'
  const steps = [{ click: { selector, surface: '#stage' } }]
  const okay = join(base, 'okay'); mkdirSync(okay)
  const positive = await actionProtocol(t, okay, steps)
  assert.deepEqual(positive.clicks, ['#refresh-slots', selector])
  assert.equal(positive.report.actions.status, 'ACTIONS_PASS')
  const point = positive.commands.find(x => x.method === 'Runtime.evaluate' && x.params.expression.includes('surfaceSelector="#stage"'))
  assert.ok(point, 'the declared surface must be observed during hit testing')
  for (const failure of ['surface-outside', 'surface-obscured', 'surface-ambiguous']) {
    const folder = join(base, failure); mkdirSync(folder)
    const negative = await actionProtocol(t, folder, steps, failure)
    assert.equal(negative.report.actions.status, 'ACTIONS_FAIL')
    assert.deepEqual(negative.clicks, ['#refresh-slots'])
    assert.equal(negative.commands.some(x => x.method === 'DOM.querySelector' && x.params.selector === '#save-now'), false)
  }
})

test('actions scroll the explicit stable surface before freshly observing a replaced message span', async t => {
  const base = directory(t), selector = '#stage .caesura-message span:nth-child(1)'
  const stable = join(base, 'stable'); mkdirSync(stable)
  const result = await actionProtocol(t, stable, [{ click: { selector, surface: '#stage' } }], 'stale-span')
  assert.equal(result.report.actions.status, 'ACTIONS_PASS', JSON.stringify(result.report))
  assert.deepEqual(result.clicks, ['#refresh-slots', selector])
  assert.equal(result.commands.some(x => x.method === 'DOM.scrollIntoViewIfNeeded' && x.params.nodeId === 20), false)
  const scroll = result.commands.findIndex(x => x.method === 'DOM.scrollIntoViewIfNeeded' && x.params.nodeId === 10)
  const geometry = result.commands.findIndex(x => x.method === 'Runtime.evaluate' && x.params.expression.includes('surfaceSelector="#stage"'))
  assert.ok(scroll >= 0 && geometry > scroll, 'current span geometry is sampled only after scrolling the declared stable surface')
  assert.match(JSON.stringify(result.report.errors), /intentionally stops before product save/)
  const legacy = join(base, 'no-declared-surface'); mkdirSync(legacy)
  const refusal = await actionProtocol(t, legacy, [{ click: selector }], 'stale-span')
  assert.equal(refusal.report.actions.status, 'ACTIONS_FAIL')
  assert.match(JSON.stringify(refusal.report.errors), /Could not find node with given id/)
  assert.deepEqual(refusal.clicks, ['#refresh-slots'], 'no surface is guessed or selected without explicit configuration')
})

test('actions keep a failed final receipt when the real locked input is deleted during CDP', async t => {
  const base = directory(t), out = join(base, 'deleted-actions'), action = actionsFile(base)
  const f = await protocolFixture(t, () => {
    rmSync(action.path)
    return { result: { processInfo: [{ type: 'browser', id: 9999 }] } }
  })
  const result = await invoke([...fixtureArgs(out, f), ...action.args])
  const report = readReport(out, result)
  assert.notEqual(result.code, 0); assert.equal(report.status, 'PROBE_FAIL')
  assert.equal(report.actions.sha256, action.sha256); assert.equal(report.actions.input_stable, false)
  assert.match(JSON.stringify(report.errors), /actions final identity.*ENOENT/)
})

test('actions offline validates the same locked file without replaying its boot UI sequence', async t => {
  const base = directory(t), out = join(base, 'offline-actions'), action = actionsFile(base), previous = join(base, 'boot-session.json')
  const slot = JSON.stringify({ scene: 'story', token: 1 }), clicks = []
  let selected = null, frames = 0
  const f = await protocolFixture(t, call => {
    if (call.method === 'SystemInfo.getProcessInfo') return { result: { processInfo: [{ type: 'browser', id: 4242 }] } }
    if (call.method === 'Target.getTargetInfo') return { result: { targetInfo: { targetId: 'owned-target', type: 'page', url: 'http://127.0.0.1:12345/games/example/' } } }
    if (call.method === 'Target.attachToTarget') return { result: { sessionId: 'owned-session' } }
    if (call.method === 'Page.getFrameTree') return { result: { frameTree: { frame: { id: 'frame', loaderId: ++frames === 1 ? 'old' : 'fresh' } } } }
    if (call.method === 'DOM.getDocument') return { result: { root: { nodeId: 1 } } }
    if (call.method === 'DOM.querySelector') { selected = call.params.selector; return { result: { nodeId: 2 } } }
    if (call.method === 'Input.dispatchMouseEvent' && call.params.type === 'mouseReleased') clicks.push(selected)
    if (call.method === 'Runtime.evaluate') {
      const expression = call.params.expression
      if (expression.includes('navigator.serviceWorker.ready')) return { error: { message: 'fixture ends after offline slot verification' } }
      if (expression.includes('document.elementFromPoint')) return { result: { result: { value: { x: 50, y: 50, w: 20, h: 20, visible: true, inside: true, matches: 1, vw: 1280, vh: 900 } } } }
      return { result: { result: { value: { url: 'http://127.0.0.1:12345/games/example/', ready: 'complete', status: 'parked: l', scene: 'story',
        errors: [], coreErrors: [], log: '', slot, slotInput: '1', online: false, wasmPin: 'http://127.0.0.1:12345/games/example/web-assets/glue.wasm',
        audio: { hook: true, state: 'running' }, viewport: { width: 1280, height: 900 },
        presentation: [{ tag: 'DIV', text: 'Visible fixture', rect: { x: 0, y: 0, width: 100, height: 50, display: 'block', visibility: 'visible', opacity: '1' } }] } } } }
    }
    return { result: {} }
  })
  writeFileSync(previous, JSON.stringify({ schema: 'caesura.web-package-probe.v1', status: 'BOOT_READY', phase: 'boot', target_id: 'owned-target', browser_pid: 4242, browser_pid_verified: true,
    url: 'http://127.0.0.1:12345/games/example/', cdp_url: f.cdp, browser_ws_url: f.cdp.replace('http:', 'ws:') + '/devtools/browser/fixture',
    slot: { sha256: createHash('sha256').update(slot).digest('hex') }, actions: { path: action.path, sha256: action.sha256 } }))
  const result = await invoke([...fixtureArgs(out, f), '--phase', 'offline', '--target-id', 'owned-target', '--previous', previous, ...action.args])
  const report = readReport(out, result)
  assert.notEqual(result.code, 0, 'fixture does not simulate service worker/game acceptance')
  assert.match(JSON.stringify(report.errors), /fixture ends after offline slot verification/)
  assert.deepEqual(clicks, ['#refresh-slots'])
  assert.equal(report.actions.status, 'NOT_RUN'); assert.deepEqual(report.actions.steps, [])
  assert.equal(report.actions.input_stable, true); assert.equal(report.actions.sha256, action.sha256)
  assert.equal(f.commands.some(x => x.method === 'Target.createTarget'), false)
})

test('CDP control HTTP reaches Fetch-restricted ports and retains the separate WebSocket failure cause', async t => {
  const base = directory(t), out = join(base, 'restricted-port')
  const f = await protocolFixture(t, () => ({ error: { message: 'WebSocket should remain restricted' } }), undefined,
    [6665, 6666, 6667, 6668, 6669, 6697])
  await assert.rejects(fetch(f.cdp + '/json/version'), error => {
    t.diagnostic(JSON.stringify({ endpoint: f.cdp, bind_attempts: f.bindAttempts, fetch_error: error.message, fetch_cause: error.cause?.message }))
    return error.cause?.message === 'bad port'
  })
  assert.equal(f.requests.length, 0, 'Fetch rejects the port before any TCP HTTP request')
  const result = await invoke(fixtureArgs(out, f)), report = readReport(out, result)
  assert.notEqual(result.code, 0, 'the native WebSocket restriction must not be reported as a passing probe')
  assert.deepEqual(f.requests, ['/json/version'], JSON.stringify(report))
  assert.equal(report.browser_ws_url, f.cdp.replace('http:', 'ws:') + '/devtools/browser/fixture')
  assert.match(JSON.stringify(report.errors), /CDP WebSocket connection failed/)
  assert.match(JSON.stringify(report.errors), /Caused by:.*Received network error or non-101 status code/)
  assert.equal(report.status, 'PROBE_FAIL'); assert.equal(report.browser_pid_verified, false)
  assert.equal(f.commands.length, 0)
})

test('CDP control HTTP rejects redirects and bounds declared or streamed response bodies', async t => {
  const base = directory(t)
  for (const [name, reply, expected] of [
    ['redirect', (request, response) => { response.writeHead(302, { Location: 'http://outside.invalid/must-not-follow' }); response.end() }, /CDP endpoint HTTP 302/],
    ['declared', (request, response) => { response.writeHead(200, { 'Content-Length': 1048577 }); response.flushHeaders() }, /CDP version response.*(?:large|limit|exceed)/],
    ['streamed', (request, response) => { response.writeHead(200); response.end('x'.repeat(1048577)) }, /CDP version response.*(?:large|limit|exceed)/],
  ]) {
    const out = join(base, name), f = await protocolFixture(t, () => null, reply)
    const result = await invoke(fixtureArgs(out, f)), report = readReport(out, result)
    assert.notEqual(result.code, 0); assert.match(JSON.stringify(report.errors), expected)
    assert.deepEqual(f.requests, ['/json/version']); assert.equal(f.commands.length, 0)
  }
})

test('CDP control HTTP deadline also bounds a response that begins but never ends', async t => {
  const base = directory(t), out = join(base, 'stalled')
  const f = await protocolFixture(t, () => null, (request, response) => { response.writeHead(200); response.write('{') })
  const start = performance.now()
  const result = await invoke([...fixtureArgs(out, f), '--timeout-ms', '1000']), report = readReport(out, result)
  assert.notEqual(result.code, 0); assert.match(JSON.stringify(report.errors), /CDP.*timed out/)
  assert.ok(performance.now() - start < 5000, 'a started body must not evade the explicit total deadline')
  assert.equal(f.commands.length, 0)
})

test('CDP control HTTP errors retain the original socket failure cause', async t => {
  const base = directory(t), out = join(base, 'reset')
  const f = await protocolFixture(t, () => null, request => request.socket.destroy())
  const result = await invoke(fixtureArgs(out, f)), report = readReport(out, result)
  assert.notEqual(result.code, 0); assert.equal(report.status, 'PROBE_FAIL')
  assert.match(JSON.stringify(report.errors), /CDP control request failed/)
  assert.match(JSON.stringify(report.errors), /Caused by:.*socket hang up/)
  assert.deepEqual(f.requests, ['/json/version']); assert.equal(f.commands.length, 0)
})
