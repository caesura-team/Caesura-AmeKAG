// @vitest-environment node
// Exercise the actual Wasmoon resume adapter; only JS completion is controlled.
import { afterEach, expect, it } from 'vitest'
import { existsSync, readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { createPlayer } from './bridge.js'

const here = dirname(fileURLToPath(import.meta.url))
let player
afterEach(async () => { await player?.dispose(); player = null })

async function makePlayer() {
  player = await createPlayer({
    scriptsBase: 'http://local/scripts/',
    wasmFile: join(here, 'node_modules/wasmoon/dist/glue.wasm'),
    fetchImpl: async url => {
      const pathname = new URL(url).pathname
      const path = pathname === '/scripts/index.json'
        ? join(here, 'scripts-index.json') : join(here, '..', pathname.slice(1))
      return new Response(existsSync(path) ? readFileSync(path) : '', { status: existsSync(path) ? 200 : 404 })
    },
  })
  return player
}

// This observer calls the original JS predicate with the original value. It
// counts an existing boundary, never simulates its result or the resume helper.
const countPredicate = `
  local count, slot, original = 0, nil, nil
  for i=1,32 do
    local name,value=debug.getupvalue(__resume_web_scene,i)
    if not name then break end
    if name=='is_promise' then slot,original=i,value;break end
  end
  assert(slot and original,'real promise predicate must be captured')
  debug.setupvalue(__resume_web_scene,slot,function(value)
    count=count+1
    return original(value)
  end)
  local observer_guard <close> = setmetatable({}, {__close=function()
    debug.setupvalue(__resume_web_scene,slot,original)
  end})
`

it('zero-value and leading-nil yields preserve all results without crossing the Promise predicate', async () => {
  const p = await makePlayer()
  const result = await p.lua.doString(`${countPredicate}
    local received
    local co=coroutine.create(function()
      received=table.pack(coroutine.yield())
      coroutine.yield(nil,'tail',17)
      return false,nil,9
    end)
    local first=table.pack(__resume_web_scene(co))
    local second=table.pack(__resume_web_scene(co,42,nil,'reply'))
    local last=table.pack(__resume_web_scene(co))
    return {calls=count,first_n=first.n,first_ok=first[1],second_n=second.n,
      second_ok=second[1],second_nil=second[2]==nil,tail=second[3],number=second[4],
      received_n=received.n,received_first=received[1],received_nil=received[2]==nil,
      received_last=received[3],last_n=last.n,last_ok=last[1],last_false=last[2]==false,
      last_nil=last[3]==nil,last_value=last[4],status=coroutine.status(co)}
  `)
  expect(result).toEqual({ calls: 0, first_n: 1, first_ok: true, second_n: 4,
    second_ok: true, second_nil: true, tail: 'tail', number: 17,
    received_n: 3, received_first: 42, received_nil: true, received_last: 'reply',
    last_n: 4, last_ok: true, last_false: true, last_nil: true, last_value: 9, status: 'dead' })
})

it('non-nil scalar, table and function yields retain the original classifier boundary', async () => {
  const p = await makePlayer()
  expect(await p.lua.doString(`${countPredicate}
    local values={false,0,'text',{},function()return 1 end}
    local co=coroutine.create(function()
      for _,value in ipairs(values) do coroutine.yield(value) end
    end)
    for _,expected in ipairs(values) do
      local result=table.pack(__resume_web_scene(co))
      assert(result.n==2 and result[1] and result[2]==expected)
    end
    local done=table.pack(__resume_web_scene(co))
    return count==5 and done.n==1 and done[1] and coroutine.status(co)=='dead'
  `)).toBe(true)
})

async function startAwait() {
  const p = await makePlayer()
  const pending = Promise.withResolvers()
  const entered = Promise.withResolvers()
  const closed = []
  const continued = []
  await p.lua.doString("rawset(_G,'__U27_RESUME_HOST',false)")
  p.lua.global.set('__U27_RESUME_HOST', {
    pending: pending.promise,
    entered: () => entered.resolve(),
    closed: () => closed.push('closed'),
    continued: value => continued.push(value),
  })
  let settled = false
  const completion = p.lua.doString(`${countPredicate}
    local host=__U27_RESUME_HOST
    local co=coroutine.create(function()
      local guard <close> = setmetatable({}, {__close=function()host.closed()end})
      host.entered()
      local reply=host.pending:await()
      host.continued(reply)
      return reply,nil,'tail'
    end)
    rawset(_G,'__U27_RESUME_CHILD',co)
    local result=table.pack(__resume_web_scene(co))
    return {ok=result[1],value=tostring(result[2]),n=result.n,
      middle_nil=result[3]==nil,tail=result[4],calls=count,status=coroutine.status(co)}
  `).finally(() => { settled = true })
  await entered.promise
  return { p, pending, completion, closed, continued, isSettled: () => settled }
}

it('a real Promise remains pending until fulfillment resumes its actual continuation', async () => {
  const run = await startAwait()
  expect(run.isSettled()).toBe(false)
  expect(run.continued).toEqual([])
  run.pending.resolve('resolved')
  expect(await run.completion).toEqual({ ok: true, value: 'resolved', n: 4,
    middle_nil: true, tail: 'tail', calls: 1, status: 'dead' })
  expect(run.continued).toEqual(['resolved'])
  expect(run.closed).toEqual(['closed'])
})

it('a rejected real Promise preserves the error until its owner closes the child', async () => {
  const run = await startAwait()
  expect(run.isSettled()).toBe(false)
  run.pending.reject(new Error('resume rejection sentinel'))
  const result = await run.completion
  expect(result.ok).toBe(false)
  expect(result.value).toContain('resume rejection sentinel')
  expect(result.n).toBe(2)
  expect(result.calls).toBe(1)
  expect(result.status).toBe('dead')
  expect(run.continued).toEqual([])
  // Like coroutine.resume itself, this adapter returns the child error. The
  // runner owns closing an errored child; this test is that explicit owner.
  expect(run.closed).toEqual([])
  const closed = await run.p.lua.doString(`
    local ok,reason=coroutine.close(__U27_RESUME_CHILD)
    return {ok=ok,reason=tostring(reason)}
  `)
  expect(closed.ok).toBe(false)
  expect(closed.reason).toContain('resume rejection sentinel')
  expect(run.closed).toEqual(['closed'])
})

it('closing a child while its real Promise waits prevents late continuation', async () => {
  const run = await startAwait()
  expect(await run.p.lua.doString('return coroutine.close(__U27_RESUME_CHILD)')).toBe(true)
  expect(run.closed).toEqual(['closed'])
  run.pending.resolve('late')
  const result = await run.completion
  expect(result.ok).toBe(false)
  expect(result.value).toContain('dead coroutine')
  expect(result.calls).toBe(1)
  expect(run.continued).toEqual([])
  expect(run.closed).toEqual(['closed'])
})
