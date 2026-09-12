import { describe, it, expect, vi } from 'vitest'
import { EngineClient, RpcError } from './rpc'
import type { OutlineSection } from '../ide/sceneOutlineTypes'
import {
  buildPositionProbeSnippet,
  parsePositionProbe,
  sceneMatchesDoc,
  tokenToOutlineLine,
} from './enginePosition'
import { buildLabelJumpSnippet, parseJumpResult, escapeLuaString } from './engineJump'
import {
  buildLayerSnapshotSnippet,
  parseLayerSnapshot,
  layerSlot,
} from './layerSnapshot'

/** Build a mock fetch returning a canned Response-like object. */
type FetchFn = (input: string | URL | Request, init?: RequestInit) => Promise<Response>

function mockFetch(status: number, body: unknown, _headers?: Record<string, string>) {
  return vi.fn<FetchFn>(async (_input: string | URL | Request, _init?: RequestInit) => {
    return {
      ok: status >= 200 && status < 300,
      status,
      json: async () => body,
      text: async () => JSON.stringify(body),
    } as Response
  })
}

describe('EngineClient', () => {
  it.each(['evalRaw', 'run', 'reload', 'stop'] as const)(
    '%s preserves an unknown outcome and never automatically resubmits the mutation',
    async method => {
      const payload = {
        status: 'busy', code: 'result_unknown',
        message: 'Request 27 started; result unknown. Do not automatically retry.',
      }
      const fetchMock = vi.fn<FetchFn>()
        .mockResolvedValueOnce(new Response(JSON.stringify(payload), { status: 503 }))
        .mockResolvedValueOnce(new Response(JSON.stringify({ status: 'ok', result: 'fresh' })))
      const client = new EngineClient('/api', fetchMock)
      const invoke = () => method === 'evalRaw' || method === 'run'
        ? client[method]('return 1') : client[method]()
      await expect(invoke()).rejects.toMatchObject({
        name: 'RpcError', status: 503, body: payload,
        message: expect.stringContaining('result_unknown'),
      })
      await Promise.resolve()
      expect(fetchMock).toHaveBeenCalledTimes(1)
      await expect(invoke()).resolves.toBeDefined()
      expect(fetchMock).toHaveBeenCalledTimes(2)
    },
  )

  it('evalRaw preserves a real non-JSON failure body after one fetch', async () => {
    const fetchMock = vi.fn<FetchFn>().mockResolvedValue(new Response('transport unavailable', { status: 502 }))
    const client = new EngineClient('/api', fetchMock)
    await expect(client.evalRaw('return 1')).rejects.toMatchObject({
      name: 'RpcError', status: 502, body: 'transport unavailable',
      message: expect.stringContaining('transport unavailable'),
    })
    expect(fetchMock).toHaveBeenCalledTimes(1)
  })

  it('GET request hits base + path with Accept header', async () => {
    const fetchMock = mockFetch(200, { status: 'ok' })
    const client = new EngineClient('/api', fetchMock)
    await client.ping()
    expect(fetchMock).toHaveBeenCalledWith(
      '/api/ping',
      expect.objectContaining({
        headers: expect.objectContaining({ Accept: 'application/json' }),
      }),
    )
  })

  it('includes Bearer token when set', async () => {
    const fetchMock = mockFetch(200, { status: 'ok' })
    const client = new EngineClient('/api', fetchMock)
    client.setToken('secret-token')
    await client.ping()
    const [, init] = fetchMock.mock.calls[0] as [string, RequestInit]
    expect((init.headers as Record<string, string>).Authorization).toBe(
      'Bearer secret-token',
    )
  })

  it('sets Content-Type when a body is sent', async () => {
    const fetchMock = mockFetch(200, { status: 'ok' })
    const client = new EngineClient('/api', fetchMock)
    await client.smaSave('demo/x.ks', 'hello')
    const [, init] = fetchMock.mock.calls[0] as [string, RequestInit]
    expect((init.headers as Record<string, string>)['Content-Type']).toBe(
      'application/json',
    )
    // body is the serialized {path, content}
    expect(JSON.parse(init.body as string)).toEqual({
      path: 'demo/x.ks',
      content: 'hello',
    })
  })

  it('encodes query params in pick()', async () => {
    const fetchMock = mockFetch(200, { hits: '[]' })
    const client = new EngineClient('/api', fetchMock)
    await client.pick(320, 180)
    expect(fetchMock.mock.calls[0][0]).toBe('/api/pick?x=320&y=180')
  })

  it('parses JSON replies into typed objects', async () => {
    const fetchMock = mockFetch(200, { status: 'ok', engine: 'caesura', lua: true, port: 9876 })
    const client = new EngineClient('/api', fetchMock)
    const reply = await client.status()
    expect(reply).toEqual({ status: 'ok', engine: 'caesura', lua: true, port: 9876 })
  })

  it('throws RpcError with status and parsed body on HTTP error', async () => {
    const fetchMock = mockFetch(500, { status: 'error', message: 'boom' })
    const client = new EngineClient('/api', fetchMock)
    await expect(client.ping()).rejects.toMatchObject({
      name: 'RpcError',
      status: 500,
      body: { status: 'error', message: 'boom' },
    })
  })

  it('RpcError is an instanceof Error with HTTP detail in message', async () => {
    const fetchMock = mockFetch(404, { error: 'not found' })
    const client = new EngineClient('/api', fetchMock)
    try {
      await client.ping()
      expect.unreachable('should have thrown')
    } catch (e) {
      expect(e).toBeInstanceOf(RpcError)
      expect((e as Error).message).toContain('404')
      expect((e as Error).message).toContain('/ping')
    }
  })

  it('setBase redirects all requests', async () => {
    const fetchMock = mockFetch(200, { status: 'ok' })
    const client = new EngineClient('/api', fetchMock)
    client.setBase('/engine')
    await client.ping()
    expect(fetchMock.mock.calls[0][0]).toBe('/engine/ping')
  })

  // ---------------------------------------------------------------------
  // Task 1 — request construction across every route
  // ---------------------------------------------------------------------

  it('evalRaw posts raw Lua code as text/plain to /eval and returns the raw result', async () => {
    const fetchMock = mockFetch(200, { result: 'some-raw-result' })
    const client = new EngineClient('/api', fetchMock)
    const result = await client.evalRaw('return 1 + 2')
    expect(result).toBe('some-raw-result')
    const [url, init] = fetchMock.mock.calls[0] as [string, RequestInit]
    expect(url).toBe('/api/eval')
    expect(init.method).toBe('POST')
    expect(init.body).toBe('return 1 + 2') // raw Lua, NOT JSON-wrapped
    const headers = init.headers as Record<string, string>
    expect(headers['Content-Type']).toBe('text/plain')
    expect(headers.Accept).toBe('application/json')
    expect(headers.Authorization).toBeUndefined()
  })

  it('evalRaw includes the bearer token when set', async () => {
    const fetchMock = mockFetch(200, { result: 'x' })
    const client = new EngineClient('/api', fetchMock)
    client.setToken('tk')
    await client.evalRaw('return 1')
    const [, init] = fetchMock.mock.calls[0] as [string, RequestInit]
    expect((init.headers as Record<string, string>).Authorization).toBe('Bearer tk')
  })

  it('evalRaw throws RpcError on non-2xx', async () => {
    // text() returns a non-JSON string here because we force the body to a string
    const fetchMock = vi.fn<FetchFn>(async () => ({
      ok: false,
      status: 500,
      json: async () => {
        throw new Error('not json')
      },
      text: async () => 'engine exploded',
    }) as unknown as Response)
    const client = new EngineClient('/api', fetchMock)
    await expect(client.evalRaw('boom')).rejects.toMatchObject({
      name: 'RpcError',
      status: 500,
    })
  })

  it('evalRaw throws RpcError carrying the engine error payload when body.error is set', async () => {
    const fetchMock = mockFetch(200, { error: 'syntax error near X' })
    const client = new EngineClient('/api', fetchMock)
    await expect(client.evalRaw('not lua')).rejects.toMatchObject({
      name: 'RpcError',
      message: 'syntax error near X',
      body: { error: 'syntax error near X' },
    })
  })

  it('evalRaw returns empty string when result is absent', async () => {
    const fetchMock = mockFetch(200, {})
    const client = new EngineClient('/api', fetchMock)
    expect(await client.evalRaw('return nil')).toBe('')
  })

  it('run() POSTs a JSON {script} to /run', async () => {
    const fetchMock = mockFetch(200, { status: 'ok' })
    const client = new EngineClient('/api', fetchMock)
    await client.run('print("hi")')
    const [url, init] = fetchMock.mock.calls[0] as [string, RequestInit]
    expect(url).toBe('/api/run')
    expect(init.method).toBe('POST')
    expect(JSON.parse(init.body as string)).toEqual({ script: 'print("hi")' })
  })

  it('smaValidate URL-encodes the path query param', async () => {
    const fetchMock = mockFetch(200, { status: 'ok', ok: true, errors: [], meta: '{}' })
    const client = new EngineClient('/api', fetchMock)
    await client.smaValidate('demo/my model/model.psd')
    expect(fetchMock.mock.calls[0][0]).toBe(
      '/api/sma/validate?path=' + encodeURIComponent('demo/my model/model.psd'),
    )
  })

  it('buildCarc() POSTs JSON {outputPath, keyPath} to /api/build', async () => {
    const fetchMock = mockFetch(200, {
      status: 'ok',
      path: 'build/game.carc',
      size: 2048,
      files: 12,
    })
    const client = new EngineClient('/api', fetchMock)
    const reply = await client.buildCarc('out', 'key')
    const [url, init] = fetchMock.mock.calls[0] as [string, RequestInit]
    expect(url).toBe('/api/build')
    expect(init.method).toBe('POST')
    expect(JSON.parse(init.body as string)).toEqual({
      outputPath: 'out',
      keyPath: 'key',
    })
    // Success reply carries the produced archive path/size/file count.
    expect(reply.status).toBe('ok')
    expect(reply.path).toBe('build/game.carc')
    expect(reply.size).toBe(2048)
    expect(reply.files).toBe(12)
  })

  it('buildCarc() with no args omits outputPath/keyPath (server defaults apply)', async () => {
    const fetchMock = mockFetch(200, { status: 'ok' })
    const client = new EngineClient('/api', fetchMock)
    await client.buildCarc()
    const [url, init] = fetchMock.mock.calls[0] as [string, RequestInit]
    expect(url).toBe('/api/build')
    expect(JSON.parse(init.body as string)).toEqual({
      outputPath: undefined,
      keyPath: undefined,
    })
  })

  it('buildCarc() surfaces RpcError carrying the engine error on 400', async () => {
    // The engine rejects paths escaping build/ with a plain {error} body.
    const client = new EngineClient(
      '/api',
      mockFetch(400, { error: 'outputPath/keyPath must be relative paths under build/' }),
    )
    const err = await client.buildCarc('../evil.carc').catch((e) => e)
    expect(err).toBeInstanceOf(RpcError)
    expect(err.status).toBe(400)
    expect(err.body).toEqual({
      error: 'outputPath/keyPath must be relative paths under build/',
    })
  })

  it('packageWeb() POSTs JSON {storyPath,outName} to /api/package/web', async () => {
    const fetchMock = mockFetch(200, {
      ok: true,
      outputDir: 'dist/example_game',
      logTail: '  PACKAGE COMPLETE -> dist/example_game',
    })
    const client = new EngineClient('/api', fetchMock)
    const reply = await client.packageWeb('demo/example_game/story.ks', 'example_game')
    const [url, init] = fetchMock.mock.calls[0] as [string, RequestInit]
    expect(url).toBe('/api/package/web')
    expect(init.method).toBe('POST')
    expect(JSON.parse(init.body as string)).toEqual({
      storyPath: 'demo/example_game/story.ks',
      outName: 'example_game',
    })
    expect(reply.ok).toBe(true)
    expect(reply.outputDir).toBe('dist/example_game')
    expect(reply.logTail).toContain('PACKAGE COMPLETE')
  })

  it('packageWeb() with no args sends undefined fields (server defaults apply)', async () => {
    const fetchMock = mockFetch(200, { ok: true, outputDir: 'dist/example_game' })
    const client = new EngineClient('/api', fetchMock)
    await client.packageWeb()
    const [url, init] = fetchMock.mock.calls[0] as [string, RequestInit]
    expect(url).toBe('/api/package/web')
    expect(JSON.parse(init.body as string)).toEqual({
      storyPath: undefined,
      outName: undefined,
    })
  })

  it('packageWeb() surfaces RpcError carrying logTail when the script fails', async () => {
    // The engine answers 500 with the packaging script's log tail so the
    // IDE can show which step failed (ks_check / ks_bake / assemble).
    const client = new EngineClient(
      '/api',
      mockFetch(500, {
        ok: false,
        error: 'package_game.sh failed with exit code 1',
        outputDir: 'dist/example_game',
        logTail: '[package] FAIL: ks_check contract gate',
      }),
    )
    const err = await client.packageWeb('demo/x.ks').catch((e) => e)
    expect(err).toBeInstanceOf(RpcError)
    expect(err.status).toBe(500)
    expect(err.body.error).toContain('exit code 1')
    expect(err.body.logTail).toContain('ks_check')
  })

  it('stop() / reload() POST with no body', async () => {
    const fetchMock = mockFetch(200, { status: 'ok' })
    const client = new EngineClient('/api', fetchMock)
    await client.stop()
    await client.reload()
    expect(fetchMock.mock.calls[0][0]).toBe('/api/stop')
    expect(fetchMock.mock.calls[1][0]).toBe('/api/reload')
    expect((fetchMock.mock.calls[0][1] as RequestInit).method).toBe('POST')
    expect((fetchMock.mock.calls[0][1] as RequestInit).body).toBeUndefined()
  })

  it('setBreakpoint / removeBreakpoint / clearBreakpoints hit debug routes', async () => {
    const fetchMock = mockFetch(200, { status: 'ok' })
    const client = new EngineClient('/api', fetchMock)
    await client.setBreakpoint('main.ks', 12)
    await client.removeBreakpoint('main.ks', 12)
    await client.clearBreakpoints()
    const [u1, i1] = fetchMock.mock.calls[0] as [string, RequestInit]
    const [u2, i2] = fetchMock.mock.calls[1] as [string, RequestInit]
    const [u3] = fetchMock.mock.calls[2] as [string, RequestInit]
    expect(u1).toBe('/api/debug/setBreakpoint')
    expect(JSON.parse(i1.body as string)).toEqual({ source: 'main.ks', line: 12 })
    expect(i1.method).toBe('POST')
    expect(u2).toBe('/api/debug/removeBreakpoint')
    expect(JSON.parse(i2.body as string)).toEqual({ source: 'main.ks', line: 12 })
    expect(u3).toBe('/api/debug/clearBreakpoints')
  })

  it('debugContinue POSTs to /debug/continue', async () => {
    const fetchMock = mockFetch(200, { status: 'ok' })
    const client = new EngineClient('/api', fetchMock)
    await client.debugContinue()
    const [url, init] = fetchMock.mock.calls[0] as [string, RequestInit]
    expect(url).toBe('/api/debug/continue')
    expect(init.method).toBe('POST')
  })

  it('debugStep POSTs to /debug/step<Mode> (Sprint 4b)', async () => {
    const fetchMock = mockFetch(200, { status: 'ok' })
    const client = new EngineClient('/api', fetchMock)
    await client.debugStep('into')
    await client.debugStep('over')
    await client.debugStep('out')
    const urls = fetchMock.mock.calls.map((c) => c[0])
    expect(urls).toEqual([
      '/api/debug/stepInto',
      '/api/debug/stepOver',
      '/api/debug/stepOut',
    ])
  })

  it('inspect() encodes name, frame and global flag', async () => {
    const fetchMock = mockFetch(200, { value: 1 })
    const client = new EngineClient('/api', fetchMock)
    await client.inspect('a b', 3, true)
    expect(fetchMock.mock.calls[0][0]).toBe(
      '/api/debug/inspect?name=' + encodeURIComponent('a b') + '&frame=3&global=1',
    )
  })

  it('inspect() omits =1 global postfix when global is false', async () => {
    const fetchMock = mockFetch(200, { value: 1 })
    const client = new EngineClient('/api', fetchMock)
    await client.inspect('x', 0, false)
    expect(fetchMock.mock.calls[0][0]).toBe('/api/debug/inspect?name=x&frame=0')
  })

  it('frame() defaults to 640x360 and encodes query', async () => {
    const fetchMock = mockFetch(200, { status: 'ok' })
    const client = new EngineClient('/api', fetchMock)
    await client.frame()
    await client.frame(1280, 720)
    expect(fetchMock.mock.calls[0][0]).toBe('/api/debug/getFrame?w=640&h=360')
    expect(fetchMock.mock.calls[1][0]).toBe('/api/debug/getFrame?w=1280&h=720')
  })

  it('assets() hits /assets and appends ?type= filter', async () => {
    const fetchMock = mockFetch(200, [])
    const client = new EngineClient('/api', fetchMock)
    await client.assets()
    await client.assets('audio')
    expect(fetchMock.mock.calls[0][0]).toBe('/api/assets')
    expect(fetchMock.mock.calls[1][0]).toBe('/api/assets?type=audio')
  })

  it('logs() / live2dModels() / debugState() hit their GET routes', async () => {
    const fetchMock = mockFetch(200, [])
    const client = new EngineClient('/api', fetchMock)
    await client.logs()
    await client.live2dModels()
    await client.debugState()
    expect(fetchMock.mock.calls[0][0]).toBe('/api/logs')
    expect(fetchMock.mock.calls[1][0]).toBe('/api/live2d/models')
    expect(fetchMock.mock.calls[2][0]).toBe('/api/debug/getState')
  })

  it('live2dLoad POSTs JSON {path}', async () => {
    const fetchMock = mockFetch(200, { status: 'ok', modelId: 7 })
    const client = new EngineClient('/api', fetchMock)
    const reply = await client.live2dLoad('models/a.model3.json')
    expect(reply.modelId).toBe(7)
    const [url, init] = fetchMock.mock.calls[0] as [string, RequestInit]
    expect(url).toBe('/api/live2d/load')
    expect(init.method).toBe('POST')
    expect(JSON.parse(init.body as string)).toEqual({
      path: 'models/a.model3.json',
    })
  })

  // ---------------------------------------------------------------------
  // Task 2 — response parsing & error handling
  // ---------------------------------------------------------------------

  it('state() returns the full engine runtime state payload', async () => {
    const payload = {
      status: 'running',
      scene: 'assets/script/main.ks',
      token_index: 42,
      nvl_mode: false,
      language: 'zh',
      backlog_count: 3,
      layer_count: 5,
      current_cmd: 'text',
    }
    const client = new EngineClient('/api', mockFetch(200, payload))
    expect(await client.state()).toEqual(payload)
  })

  it('stats() returns the full stats payload', async () => {
    const payload = {
      status: 'ok',
      texture_budget_mb: 512,
      texture_tier: 3,
      texture_tier_name: 'high',
      mesh_count: 10,
      job_workers: 4,
      job_pending: 2,
      lua_kb: 1024,
    }
    const client = new EngineClient('/api', mockFetch(200, payload))
    expect(await client.stats()).toEqual(payload)
  })

  it('pick() passes the raw JSON-array-text hits through unmodified', async () => {
    const rawHits = '[{"id":"1","name":"bg","z":0,"depth":1,"opacity":1,"x":0,"y":0,"w":100,"h":100}]'
    const client = new EngineClient('/api', mockFetch(200, { status: 'ok', hits: rawHits }))
    const reply = await client.pick(1, 2)
    expect(reply.status).toBe('ok')
    // The client does not parse the JSON array text — that is left to consumers.
    expect(reply.hits).toBe(rawHits)
  })

  it('frame() passes the base64 png through', async () => {
    const client = new EngineClient('/api', mockFetch(200, {
      status: 'ok', width: 640, height: 360, png: 'aGVsbG8=',
    }))
    const reply = await client.frame()
    expect(reply.png).toBe('aGVsbG8=')
    expect(reply.width).toBe(640)
  })

  it('debugState() passes paused/token_index/current_cmd through', async () => {
    const client = new EngineClient('/api', mockFetch(200, {
      status: 'paused', scene: 'main.ks', paused: true, token_index: 9, current_cmd: '[ch]',
    }))
    const reply = await client.debugState()
    expect(reply.paused).toBe(true)
    expect(reply.token_index).toBe(9)
    expect(reply.current_cmd).toBe('[ch]')
  })

  it('parses an HTTP error whose body is non-JSON by falling back to text', async () => {
    const fetchMock = vi.fn<FetchFn>(async () => ({
      ok: false,
      status: 503,
      json: async () => {
        throw new SyntaxError('bad json')
      },
      text: async () => 'plain text 503',
    }) as unknown as Response)
    const client = new EngineClient('/api', fetchMock)
    const err = await client.ping().catch((e) => e)
    expect(err).toBeInstanceOf(RpcError)
    expect(err.status).toBe(503)
    // body is the fallback text when JSON parsing failed
    expect(err.body).toBe('plain text 503')
    expect((err as Error).message).toContain('/ping')
  })

  it('RpcError message mentions the HTTP status even when body is absent', async () => {
    const fetchMock = vi.fn<FetchFn>(async () => ({
      ok: false,
      status: 400,
      json: async () => null,
      text: async () => '',
    }) as Response)
    const client = new EngineClient('/api', fetchMock)
    const err = await client.smaSave('x', 'y').catch((e) => e)
    expect(err).toBeInstanceOf(RpcError)
    expect((err as Error).message).toContain('400')
    expect((err as Error).message).toContain('/sma/save')
  })

  it('propagates network errors (fetch rejection) as-is', async () => {
    const fetchMock = vi.fn<FetchFn>(async () => {
      throw new TypeError('Failed to fetch')
    })
    const client = new EngineClient('/api', fetchMock)
    await expect(client.ping()).rejects.toThrow('Failed to fetch')
  })

  it('treats a 204-style empty JSON body gracefully (parses as-is)', async () => {
    const fetchMock = vi.fn<FetchFn>(async () => ({
      ok: true,
      status: 204,
      json: async () => ({}),
      text: async () => '',
    }) as Response)
    const client = new EngineClient('/api', fetchMock)
    expect(await client.ping()).toEqual({})
  })

  // ---------------------------------------------------------------------
  // Task 5 — evalRaw → pure-function parsing chains
  // ---------------------------------------------------------------------

  it('integration: evalRaw -> parsePositionProbe resolves a running scene', async () => {
    const fetchMock = mockFetch(200, { result: '{"scene":"assets/script/main.ks","token":7}' })
    const client = new EngineClient('/api', fetchMock)
    const raw = await client.evalRaw(buildPositionProbeSnippet())
    const pos = parsePositionProbe(raw)
    expect(pos).toEqual({ scene: 'assets/script/main.ks', token: 7 })
  })

  it('integration: evalRaw -> parseJumpResult classifies ok/missing/no-ctx', async () => {
    const fetchMock = vi.fn<FetchFn>(async () => ({ ok: true, status: 200, json: async () => ({ result: 'ok' }), text: async () => '' }) as Response)
    const client = new EngineClient('/api', fetchMock)
    const ok = parseJumpResult(await client.evalRaw(buildLabelJumpSnippet('*start')))
    expect(ok).toBe('ok')

    const fetchMissing = vi.fn<FetchFn>(async () => ({ ok: true, status: 200, json: async () => ({ result: 'missing' }), text: async () => '' }) as Response)
    expect(parseJumpResult(await new EngineClient('/api', fetchMissing).evalRaw(buildLabelJumpSnippet('*nope')))).toBe('missing')
  })

  it('integration: evalRaw -> parseLayerSnapshot resolves a structured tree', async () => {
    const layerJson = JSON.stringify([
      { id: '1', name: 'msg', z: 10, visible: true, handle: 5, opacity: 1 },
      { id: '2', name: 'bg', z: 1, visible: true, handle: 3, opacity: 0.5 },
    ])
    const fetchMock = mockFetch(200, { result: layerJson })
    const client = new EngineClient('/api', fetchMock)
    const snaps = parseLayerSnapshot(await client.evalRaw(buildLayerSnapshotSnippet()))
    // parseLayerSnapshot sorts bg slots before msg
    expect(snaps.map((s) => s.name)).toEqual(['bg', 'msg'])
    expect(snaps[0].slot).toBe('bg')
    expect(snaps[1].slot).toBe('msg')
  })

  it('integration: evalRaw no-ctx pushes null through parsePositionProbe', async () => {
    const fetchMock = mockFetch(200, { result: 'no-ctx' })
    const client = new EngineClient('/api', fetchMock)
    expect(parsePositionProbe(await client.evalRaw(buildPositionProbeSnippet()))).toBeNull()
  })

  it('helpers compose: sceneMatchesDoc and tokenToOutlineLine are consistent with probes', () => {
    // The snippet + parser round-trip must agree with the outline mapping helper.
    expect(sceneMatchesDoc('assets/script/main.ks', 'assets/script/main.ks')).toBe(true)
    expect(sceneMatchesDoc('assets/script/main.ks', 'main.ks')).toBe(true)
    expect(sceneMatchesDoc('a.ks', 'b.ks')).toBe(false)

    const sections: OutlineSection[] = [
      {
        label: 'start',
        line: 3,
        items: [{ kind: 'label', line: 3, name: 'start' }],
      },
      {
        label: null,
        line: 1,
        items: [{ kind: 'text', line: 1, content: 'prologue' }],
      },
    ]
    // rows are flattened heading-then-items in document order: [3,3,1,1].
    expect(tokenToOutlineLine(sections, 1)).toBe(3)
    expect(tokenToOutlineLine(sections, 2)).toBe(3)
    expect(tokenToOutlineLine(sections, 3)).toBe(1)
    expect(tokenToOutlineLine(sections, 5)).toBeNull()
  })
})

describe('EngineClient evalRaw contract (pure fragments)', () => {
  it('buildLabelJumpSnippet escapes label quotes/backslashes and parseJumpResult round-trips', () => {
    const safe = escapeLuaString('a"b\\c')
    expect(safe).toBe('a\\\"b\\\\c')
    expect(parseJumpResult('ok')).toBe('ok')
    expect(parseJumpResult('missing')).toBe('missing')
    expect(parseJumpResult('no-ctx')).toBe('no-ctx')
    expect(parseJumpResult('weird')).toBe('weird')
  })

  it('layerSlot groups bg/fg/msg names', () => {
    expect(layerSlot('bg')).toBe('bg')
    expect(layerSlot('fgLayer')).toBe('fg')
    expect(layerSlot('msgLayer')).toBe('msg')
    expect(layerSlot('_gallery')).toBe('other')
    expect(layerSlot('foreground')).toBe('other')
    expect(layerSlot('')).toBe('other')
  })
})

describe('EngineClient project routes (Project Manager, Sprint 2)', () => {
  it('projectTemplates() GETs /project/templates and parses the array', async () => {
    const payload = [
      { id: 'basic', name: 'basic', description: 'Minimal', dir: 'basic' },
      { id: 'live2d', name: 'live2d', description: 'Live2D', dir: 'live2d' },
    ]
    const client = new EngineClient('/api', mockFetch(200, payload))
    const templates = await client.projectTemplates()
    expect(templates).toHaveLength(2)
    expect(templates[0].id).toBe('basic')
    expect(templates[0].dir).toBe('basic')
    expect(client['request']).toBeDefined()
  })

  it('projectTemplates() hits the exact /api/project/templates path', async () => {
    const fetchMock = mockFetch(200, [])
    const client = new EngineClient('/api', fetchMock)
    await client.projectTemplates()
    expect(fetchMock.mock.calls[0][0]).toBe('/api/project/templates')
  })

  it('projectList() GETs /project/list and parses ProjectInfo[]', async () => {
    const payload = [
      { path: 'projects/demo', name: 'demo', template: 'basic', modified: '1234' },
    ]
    const client = new EngineClient('/api', mockFetch(200, payload))
    const list = await client.projectList()
    expect(list).toHaveLength(1)
    expect(list[0].path).toBe('projects/demo')
    expect(list[0].template).toBe('basic')
  })

  it('projectList() hits the exact /api/project/list path', async () => {
    const fetchMock = mockFetch(200, [])
    const client = new EngineClient('/api', fetchMock)
    await client.projectList()
    expect(fetchMock.mock.calls[0][0]).toBe('/api/project/list')
  })

  it('projectCreate() POSTs JSON {template,name} to /project/create', async () => {
    const fetchMock = mockFetch(200, { ok: true, path: 'projects/demo' })
    const client = new EngineClient('/api', fetchMock)
    const reply = await client.projectCreate('basic', 'demo')
    expect(reply.ok).toBe(true)
    expect(reply.path).toBe('projects/demo')
    const [url, init] = fetchMock.mock.calls[0] as [string, RequestInit]
    expect(url).toBe('/api/project/create')
    expect(init.method).toBe('POST')
    expect(JSON.parse(init.body as string)).toEqual({ template: 'basic', name: 'demo' })
  })

  it('projectCreate() surfaces a {error} reply object when not ok', async () => {
    const client = new EngineClient('/api', mockFetch(200, { ok: false, error: 'Project already exists' }))
    const reply = await client.projectCreate('basic', 'demo')
    expect(reply.ok).toBe(false)
    expect(reply.error).toBe('Project already exists')
  })

  it('projectDuplicate() POSTs JSON {srcPath,name} to /project/duplicate', async () => {
    const fetchMock = mockFetch(200, { ok: true, path: 'projects/demo_copy' })
    const client = new EngineClient('/api', fetchMock)
    const reply = await client.projectDuplicate('projects/demo', 'demo_copy')
    expect(reply.ok).toBe(true)
    const [url, init] = fetchMock.mock.calls[0] as [string, RequestInit]
    expect(url).toBe('/api/project/duplicate')
    expect(init.method).toBe('POST')
    expect(JSON.parse(init.body as string)).toEqual({
      srcPath: 'projects/demo',
      name: 'demo_copy',
    })
  })

  it('projectImport() POSTs JSON {srcPath,name} to /project/import', async () => {
    const fetchMock = mockFetch(200, { ok: true, path: 'projects/imported' })
    const client = new EngineClient('/api', fetchMock)
    const reply = await client.projectImport('/tmp/mygame', 'imported')
    expect(reply.ok).toBe(true)
    expect(reply.path).toBe('projects/imported')
    const [url, init] = fetchMock.mock.calls[0] as [string, RequestInit]
    expect(url).toBe('/api/project/import')
    expect(init.method).toBe('POST')
    expect(JSON.parse(init.body as string)).toEqual({
      srcPath: '/tmp/mygame',
      name: 'imported',
    })
  })

  it('projectImport() throws RpcError carrying the backend error on 404', async () => {
    const client = new EngineClient(
      '/api',
      mockFetch(404, { error: 'Source directory not found' }),
    )
    const err = await client.projectImport('/nope', 'x').catch((e) => e)
    expect(err).toBeInstanceOf(RpcError)
    expect(err.status).toBe(404)
    expect(err.body).toEqual({ error: 'Source directory not found' })
  })

  it('projectImport() surfaces a {error} reply object when not ok', async () => {
    const client = new EngineClient(
      '/api',
      mockFetch(200, { ok: false, error: 'Not a Caesura project (missing story.ks/entry.lua)' }),
    )
    const reply = await client.projectImport('/tmp/plain', 'plain')
    expect(reply.ok).toBe(false)
    expect(reply.error).toContain('story.ks')
  })

  it('projectCreate() throws RpcError carrying the backend error on 400', async () => {
    const client = new EngineClient('/api', mockFetch(400, { error: 'Invalid project name' }))
    const err = await client.projectCreate('basic', 'bad name!').catch((e) => e)
    expect(err).toBeInstanceOf(RpcError)
    expect(err.status).toBe(400)
    expect(err.body).toEqual({ error: 'Invalid project name' })
  })
})
