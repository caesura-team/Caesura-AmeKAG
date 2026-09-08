// @vitest-environment jsdom
// Real native runs and ks_bake output feed the actual Wasmoon Web player.
import { it, expect } from 'vitest'
import { readFileSync, writeFileSync, existsSync, mkdirSync, mkdtempSync } from 'node:fs'
import { join, dirname, relative } from 'node:path'
import { fileURLToPath } from 'node:url'
import { spawnSync } from 'node:child_process'
import { createPlayer } from './bridge.js'
import { luaLiteralValue } from './lua-value.js'

const here=dirname(fileURLToPath(import.meta.url)), root=join(here,'..')
const corpusDir=join(root,'tests/projects/runtime_contracts')
const read=(path)=>readFileSync(path,'utf8')
const index=JSON.parse(read(join(here,'scripts-index.json')))
const fetchImpl=async (url)=>{
  const pathname=new URL(url).pathname
  const path=join(root,pathname.slice(1)), ok=existsSync(path)
  return {ok,status:ok?200:404,text:async()=>ok?read(path):'',json:async()=>index}
}
function nativeLua() {
  const candidates=[process.env.CAESURA_LUA_BIN,
    join(root,'build/lua/Debug/lua.exe'),join(root,'build/lua/Release/lua.exe'),
    join(root,'build/lua/lua'),process.platform==='linux'?'lua5.4':'lua']
  for (const command of candidates.filter(Boolean)) {
    const result=spawnSync(command,['-e','assert(_VERSION == "Lua 5.4")'],{cwd:root,encoding:'utf8'})
    if (result.status===0) return command
  }
  throw new Error('U13 requires a real Lua 5.4 interpreter; build it or set CAESURA_LUA_BIN')
}
function createCorpusPlayer() {
  const slots=new Map()
  return createPlayer({scriptsBase:'http://local/scripts/',fetchImpl,
    storageBackend:{get:key=>slots.get(key)??null,set:(key,value)=>{slots.set(key,value);return true},
      del:key=>slots.delete(key)},
    wasmFile:join(here,'node_modules/wasmoon/dist/glue.wasm')})
}
it('preserves corpus events across native source/AST/cache and actual Web source/baked bundle',async()=>{
  mkdirSync(join(root,'artifacts/validation'),{recursive:true})
  const output=mkdtempSync(join(root,'artifacts/validation/runtime-contracts-'))
  const luaBin=nativeLua()
  const native=spawnSync(luaBin,['tests/scripts/test_runtime_contracts.lua'],
    {cwd:root,encoding:'utf8',timeout:60000,maxBuffer:8*1024*1024})
  writeFileSync(join(output,'native.log'),(native.stdout??'')+(native.stderr??''))
  expect(native.status,native.stderr+'\n'+native.stdout).toBe(0)
  const marker=native.stdout.split(/\r?\n/).find(line=>line.startsWith('RUNTIME_CONTRACTS_JSON:'))
  const nativeResult=JSON.parse(marker.slice('RUNTIME_CONTRACTS_JSON:'.length))
  writeFileSync(join(output,'native.json'),JSON.stringify(nativeResult,null,2))

  const bootstrap=await createCorpusPlayer()
  let corpus
  try {corpus=JSON.parse(await bootstrap.lua.doString(
    `local observe=(function() ${read(join(corpusDir,'observe.lua'))} end)()\n`
    +`return observe.json((function() ${read(join(corpusDir,'corpus.lua'))} end)())`))}
  finally {await bootstrap.dispose()}
  const files=[...new Set(corpus.flatMap(c=>c.scenes))]
  const bake=spawnSync(luaBin,['scripts/ks_bake.lua',...files.map(f=>'tests/projects/runtime_contracts/'+f),
    '--web',relative(root,output).replaceAll('\\','/')],{cwd:root,encoding:'utf8',timeout:30000})
  writeFileSync(join(output,'bake.log'),(bake.stdout??'')+(bake.stderr??''))
  expect(bake.status,bake.stdout+'\n'+bake.stderr).toBe(0)
  const bakedSource=read(join(output,'story.lua')), runs=[]
  for (const item of corpus) {
    const expected=nativeResult.runs.find(r=>r.case===item.name&&r.lane==='source'&&r.dt===0.007)
    expect(expected,item.name+' missing native baseline').toBeTruthy()
    for (const lane of ['source','bundle']) {
      const player=await createCorpusPlayer()
      try {
        const scenes=Object.fromEntries(item.scenes.map(f=>[f,read(join(corpusDir,f))]))
        const bundle=await player.lua.doString(bakedSource)
        await player.lua.doString(`__U13_OBSERVE=(function() ${read(join(corpusDir,'observe.lua'))} end)()`)
        // Bundle runs have no source fallback. A missing scene must fail.
        const drive=(entry,advance=false)=>lane==='bundle'
          ?player.runFromBundle(bundle,entry,{maxFrames:10000,advance,advanceScene:entry,choiceIndex:item.choice})
          :player.runScene(scenes[entry],entry,{maxFrames:10000,advance,advanceScene:entry,
            choiceIndex:item.choice,sceneSources:scenes})
        const run=async(entry,replay)=>{
          const trace=[]
          let status=await drive(entry)
          for(let step=0;step<100;step++) {
            expect(status.startsWith('WAIT:')||status.startsWith('DONE:'),status).toBe(true)
            const terminal=status.startsWith('DONE:')
            trace.push(JSON.parse(await player.lua.doString(
              `return __U13_OBSERVE.json(__U13_OBSERVE.capture(require('kag_runner').get_ctx(),${terminal}))`)))
            if(terminal) {
              await player.lua.doString(`__U13_OBSERVE.check(${luaLiteralValue(item)},${luaLiteralValue(trace)},${replay})`)
              return trace
            }
            status=await drive(entry,true)
          }
          throw new Error('Web corpus did not end: '+item.name)
        }
        const trace=await run(item.entry,false)
        const replay=item.replay?await run(item.replay,true):undefined
        runs.push({case:item.name,lane,dt:0.016,trace,replay})
        // Persist measured results before comparing so failures stay inspectable.
        writeFileSync(join(output,'web.json'),JSON.stringify({version:1,runtime:'web',runs},null,2))
        expect(trace,item.name+' '+lane+' events').toEqual(expected.trace)
        expect(replay,item.name+' '+lane+' replay').toEqual(expected.replay)
      } finally {expect(await player.dispose()).toBe(true)}
    }
  }
  expect(runs).toHaveLength(corpus.length*2)
  const compare=spawnSync(process.env.PYTHON??(process.platform==='win32'?'python':'python3'),
    ['scripts/compare_runtime_contracts.py',join(output,'native.json'),join(output,'web.json')],
    {cwd:root,encoding:'utf8',timeout:30000})
  writeFileSync(join(output,'comparison.log'),(compare.stdout??'')+(compare.stderr??''))
  expect(compare.status,compare.stdout+'\n'+compare.stderr).toBe(0)
  const controls=spawnSync(process.env.PYTHON??(process.platform==='win32'?'python':'python3'),
    ['tests/scripts/check_runtime_trace_controls.py',join(output,'native.json'),join(output,'web.json')],
    {cwd:root,encoding:'utf8',timeout:30000})
  writeFileSync(join(output,'negative-controls.log'),(controls.stdout??'')+(controls.stderr??''))
  expect(controls.status,controls.stdout+'\n'+controls.stderr).toBe(0)
},60000)

it('cannot replay a previous lane save when the current lane save fails',async()=>{
  const scenes={'saved.ks':'[set var="f.proof" value=77]\n[save slot=41]\n[end]',
    'loader.ks':'[load slot=41]\n[end]'}
  const first=await createCorpusPlayer()
  try {
    expect((await first.runScene(scenes['saved.ks'],'saved.ks',{sceneSources:scenes})).startsWith('DONE:')).toBe(true)
    expect(await first.lua.doString("return require('kag_runner').get_ctx().tf.save_result")).toBe('ok')
  } finally {await first.dispose()}
  const second=await createCorpusPlayer()
  try {
    await second.lua.doString('KAG.save_game=function() return false end')
    await second.runScene(scenes['saved.ks'],'saved.ks',{sceneSources:scenes})
    expect(await second.lua.doString("return require('kag_runner').get_ctx().tf.save_result")).toBe('error')
    await second.runScene(scenes['loader.ks'],'loader.ks',{sceneSources:scenes})
    expect(await second.lua.doString("return require('kag_runner').get_ctx().tf.load_result")).toBe('error')
  } finally {await second.dispose()}
})
