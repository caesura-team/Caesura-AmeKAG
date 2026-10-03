// File-backed test fetch: real repository bytes and real byte streams.
// No external network, generated image, permissive fallback, or fake presenter.
import {readFileSync,existsSync,statSync,realpathSync} from 'node:fs'
import {resolve,relative,isAbsolute} from 'node:path'
import {ReadableStream} from 'node:stream/web'

export const repositoryAssetUrl=path=>new URL(String(path).startsWith('assets/')
  ?String(path):'assets/'+String(path),'http://local/').href

export function createRepositoryFetch(rootDir,index) {
  const root=realpathSync(rootDir)
  const inside=path=>{const rel=relative(root,path);return rel!==''&&!rel.startsWith('..')&&!isAbsolute(rel)}
  return async (input,{signal}={})=>{
    if(signal?.aborted) throw signal.reason??new Error('Fixture fetch aborted')
    const url=new URL(input,'http://local/')
    if(url.origin!=='http://local') throw new Error('Fixture fetch origin is not local')
    const name=decodeURIComponent(url.pathname).replace(/^\/+/, '')
    if(!name.startsWith('scripts/')&&!name.startsWith('assets/')) throw new Error('Fixture fetch path is outside declared trees')
    let bytes,ok
    if(name==='scripts/index.json') {bytes=Buffer.from(JSON.stringify(index));ok=true}
    else {
      const file=resolve(root,name)
      if(!inside(file)) throw new Error('Fixture fetch path escaped source root')
      ok=existsSync(file)&&statSync(file).isFile()
      if(ok&&!inside(realpathSync(file))) throw new Error('Fixture fetch target escaped source root')
      bytes=ok?readFileSync(file):Buffer.alloc(0)
    }
    let offset=0
    return {ok,status:ok?200:404,url:url.href,
      headers:{get:name=>name.toLowerCase()==='content-length'?String(bytes.byteLength):null},
      body:new ReadableStream({pull(controller){
        if(signal?.aborted){controller.error(signal.reason??new Error('Fixture fetch aborted'));return}
        if(offset>=bytes.byteLength){controller.close();return}
        const end=Math.min(offset+65536,bytes.byteLength)
        controller.enqueue(new Uint8Array(bytes.subarray(offset,end)));offset=end
      }}),
      text:async()=>bytes.toString('utf8'),json:async()=>JSON.parse(bytes.toString('utf8')),
      arrayBuffer:async()=>bytes.buffer.slice(bytes.byteOffset,bytes.byteOffset+bytes.byteLength)}
  }
}
