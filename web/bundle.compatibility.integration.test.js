// @vitest-environment jsdom
import {it,expect} from 'vitest'
import {readFileSync,existsSync} from 'node:fs'
import {dirname,join} from 'node:path'
import {fileURLToPath} from 'node:url'
import {createPlayer} from './bridge.js'
import {luaLiteralValue} from './lua-value.js'
const here=dirname(fileURLToPath(import.meta.url)),root=join(here,'..')
const index=JSON.parse(readFileSync(join(here,'scripts-index.json'),'utf8'))
async function playerForTest() {
  const slots=new Map()
  return createPlayer({scriptsBase:'http://local/scripts/',
    fetchImpl:async url=>{
      const path=join(root,new URL(url).pathname.slice(1)),ok=existsSync(path)
      return {ok,status:ok?200:404,text:async()=>ok?readFileSync(path,'utf8'):'',json:async()=>index}
    },storageBackend:{get:k=>slots.get(k)??null,set:(k,v)=>{slots.set(k,v);return true},del:k=>slots.delete(k)},
    wasmFile:join(here,'node_modules/wasmoon/dist/glue.wasm')})
}
async function bake(player,sources) {
  return player.lua.doString(`local sources=${luaLiteralValue(sources)}
    local compiler=require('kag.compiler')
    local result={version=1,scenes={},assets={}}
    for name,source in pairs(sources) do
      local tokens=require('tokenizer').parse(source);compiler.compile(tokens)
      result.scenes[name]=assert(compiler.serialize(tokens))
    end
    return result`)
}
const held='[set var="f.owner" value="original"]\n[ch text="ALPHA"]\n[ch text="BRAVO"]\n[end]'
async function hold(player) {
  expect((await player.runScene(held,'held.ks')).startsWith('WAIT:')).toBe(true)
  await player.lua.doString('__U14_OWNER=require("kag_runner").get_ctx();__U14_CO=__U14_OWNER.co')
}
async function intact(player) {
  expect(await player.lua.doString(`local c=require('kag_runner').get_ctx()
    return c==__U14_OWNER and c.co==__U14_CO and c.f.owner=='original' and c.waiting_input`)).toBe(true)
  expect((await player.runScene(held,'held.ks',{advance:true,advanceScene:'held.ks'})).startsWith('WAIT:')).toBe(true)
  expect(player.core.draws.map(d=>d.t??'').join('')).toContain('BRAVO')
}
it.each(['old-scene-format','future-semantics','future-container'])('rejects %s before replacing a held session',async kind=>{
  const player=await playerForTest()
  try {
    await hold(player)
    const sources={'new.ks':'[set var="f.owner" value="replaced"]\n[end]'}
    const bundle=await bake(player,sources)
    if(kind==='old-scene-format') bundle.scenes['new.ks'].version=1
    else if(kind==='future-semantics') bundle.scenes['new.ks'].compatibility.semantics='future-compiler'
    else bundle.version=999
    const status=await player.runFromBundle(bundle,'new.ks',{sceneSources:sources})
    expect(status).toMatch(/^ERR:.*(incompatible|mismatch)/)
    await intact(player)
  } finally {await player.dispose()}
})
it('preflights an incompatible callee even when entry is compatible and source fallback exists',async()=>{
  const player=await playerForTest()
  try {
    await hold(player)
    const sources={'new.ks':'[call callee.ks]\n[end]','callee.ks':'[set var="f.callee" value=1]\n[return]'}
    const bundle=await bake(player,sources)
    bundle.scenes['callee.ks'].compatibility.semantics='future-compiler'
    expect(await player.runFromBundle(bundle,'new.ks',{sceneSources:sources})).toMatch(/^ERR:.*callee.ks/)
    await intact(player)
  } finally {await player.dispose()}
})
it('rejects a changed schema default and still executes newly compiled source',async()=>{
  const player=await playerForTest()
  try {
    await player.lua.doString(`require('kag.schema').define('u14value',{value={type='number',default=7}})
      require('kag').u14value=function(ctx,p) ctx.f.value=p.value end`)
    const source='[u14value]\n[end]',bundle=await bake(player,{'value.ks':source})
    await player.lua.doString("require('kag.schema').specs('u14value').value.default=9")
    expect(await player.runFromBundle(bundle,'value.ks')).toMatch(/^ERR:.*command-contract-mismatch/)
    expect((await player.runScene(source,'value.ks')).startsWith('DONE:')).toBe(true)
    expect(await player.lua.doString("return require('kag_runner').get_ctx().f.value")).toBe(9)
  } finally {await player.dispose()}
})
