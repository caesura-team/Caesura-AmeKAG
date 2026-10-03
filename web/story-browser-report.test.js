import {describe,it,expect} from 'vitest'
import {validateStoryBrowserReport,requiredSources,allowFreshViteDependency,viteFsPath,browserFailureSummary,waitForBrowserStartup} from './test-support/run-story-browser-benchmark.mjs'
import {mkdtempSync,mkdirSync,writeFileSync,symlinkSync,rmSync} from 'node:fs'
import {tmpdir} from 'node:os'
import {join,posix} from 'node:path'
import {createHash} from 'node:crypto'
import {createServer} from 'node:http'

function report() {
  const sample={out:'DONE:339:193',wallMs:3000,frames:4000,framesPerMs:4000/3000,
    tokens:339,tokensPerMs:339/3000,memGrowthKB:20,renderedFrames:150,errors:[],audioBefore:'closed',audioAfter:'closed',
    audioContextRetained:true,audioAvailable:false}
  const sources=Object.fromEntries(requiredSources.map(path=>[path,'a'.repeat(64)]))
  return {passed:true,sourceStable:true,browserExited:true,endpointClosed:true,launcherExit:0,
    browser:process.execPath,browserVersion:'Chrome/154.0.0.0',browserSha256:'b'.repeat(64),browserPid:123,node:process.version,
    sourceBefore:sources,sourceAfter:{...sources},sourceManifestSha256:createHash('sha256').update(JSON.stringify(sources)).digest('hex'),
    owner:{actualExit:0,cleanupComplete:true,timedOut:false,python:process.execPath},
    result:{visibility:'visible',disposed:true,audioAvailable:false,audioProfile:'unavailable-real-closed-context',
      warmup:{...sample},samples:Array.from({length:3},()=>({...sample}))}}
}
describe('required real-browser story report',()=>{
  it('retains all three measured samples',()=>{
    const value=report();expect(validateStoryBrowserReport(value)).toBe(value.result.samples)
  })
  it.each([
    ['missing browser',value=>{value.passed=false}],
    ['changed source',value=>{value.sourceStable=false}],
    ['live browser',value=>{value.browserExited=false}],
    ['open endpoint',value=>{value.endpointClosed=false}],
    ['nonzero launcher',value=>{value.launcherExit=1}],
    ['forced cleanup',value=>{value.cleanupFallback=true}],
    ['missing browser path',value=>{delete value.browser}],
    ['missing browser version',value=>{delete value.browserVersion}],
    ['missing browser digest',value=>{delete value.browserSha256}],
    ['missing browser pid',value=>{delete value.browserPid}],
    ['missing node identity',value=>{delete value.node}],
    ['missing source manifest',value=>{delete value.sourceBefore}],
    ['empty source manifest',value=>{value.sourceBefore={};value.sourceAfter={}}],
    ['changed raw source',value=>{value.sourceAfter['web/bridge.js']='c'.repeat(64)}],
    ['wrong source digest',value=>{value.sourceManifestSha256='d'.repeat(64)}],
    ['missing VM JavaScript',value=>{delete value.sourceBefore['web/node_modules/wasmoon/dist/index.js'];delete value.sourceAfter['web/node_modules/wasmoon/dist/index.js']}],
    ['malformed source digest',value=>{value.sourceBefore['web/bridge.js']='bad';value.sourceAfter['web/bridge.js']='bad'}],
    ['missing tree owner',value=>{delete value.owner}],
    ['failed tree owner',value=>{value.owner.actualExit=1}],
    ['incomplete tree cleanup',value=>{value.owner.cleanupComplete=false}],
    ['owner timeout',value=>{value.owner.timedOut=true}],
    ['hidden page',value=>{value.result.visibility='hidden'}],
    ['missing teardown',value=>{value.result.disposed=false}],
    ['changed audio capability',value=>{value.result.audioAvailable=true}],
    ['missing audio profile',value=>{delete value.result.audioProfile}],
    ['running audio before',value=>{value.result.samples[0].audioBefore='running'}],
    ['resumed audio afterward',value=>{value.result.samples[0].audioAfter='running'}],
    ['replaced audio context',value=>{value.result.samples[0].audioContextRetained=false}],
    ['enabled audio sample',value=>{value.result.samples[0].audioAvailable=true}],
    ['missing sample',value=>{value.result.samples.pop()}],
    ['invalid duration',value=>{value.result.samples[0].wallMs=NaN}],
    ['zero duration',value=>{value.result.samples[0].wallMs=0}],
    ['unfinished story',value=>{value.result.samples[0].out='WAIT:42'}],
    ['token mismatch',value=>{value.result.samples[0].tokens=1}],
    ['no scheduler progress',value=>{value.result.samples[0].frames=0}],
    ['no real render progress',value=>{value.result.samples[0].renderedFrames=0}],
    ['command failure',value=>{value.result.samples[0].errors=[{kind:'error'}]}],
    ['invented throughput',value=>{value.result.samples[0].tokensPerMs=1}],
  ])('rejects %s',(_name,mutate)=>{
    const value=report();mutate(value);expect(()=>validateStoryBrowserReport(value)).toThrow()
  })
})

describe('owned Vite dependency publication boundary',()=>{
  it.each([
    ['/tmp/owned/cache/deps/wasmoon.js','/tmp/owned/cache/deps/wasmoon.js'],
    ['E:/owned/cache/deps/wasmoon.js','E:/owned/cache/deps/wasmoon.js'],
    ['/tmp/owned/cache/deps/../wasmoon.js','/tmp/owned/cache/wasmoon.js'],
  ])('decodes Vite generated URL for %s independently of the host OS',(file,expected)=>{
    expect(viteFsPath(posix.join('/@fs/',file))).toBe(expected)
  })
  it('rejects non-Vite and drive-relative paths',()=>{
    expect(viteFsPath('/tmp/file.js')).toBeNull()
    expect(viteFsPath('/@fs/E:relative.js')).toBeNull()
    expect(viteFsPath('/@fs/E:\\escape.js')).toBeNull()
  })
  it('permits existing and pending dependencies only inside the owned cache',()=>{
    const owned=mkdtempSync(join(tmpdir(),'caesura-vite-path-'))
    const cache=join(owned,'cache'),outside=join(owned,'outside')
    mkdirSync(cache);mkdirSync(outside)
    const url=file=>posix.join('/@fs/',file.replaceAll('\\','/'))
    try {
      expect(allowFreshViteDependency(url(join(cache,'deps','future.js')),cache)).toBe(true)
      mkdirSync(join(cache,'deps'));writeFileSync(join(cache,'deps','ready.js'),'export {}')
      expect(allowFreshViteDependency(url(join(cache,'deps','ready.js')),cache)).toBe(true)
      expect(allowFreshViteDependency(url(join(cache,'deps','future.js.map')),cache)).toBe(true)
      expect(allowFreshViteDependency(url(join(outside,'escape.js')),cache)).toBe(false)
      expect(allowFreshViteDependency(url(join(cache,'..','outside','escape.js')),cache)).toBe(false)
      expect(allowFreshViteDependency(url(join(cache,'deps','secret.json')),cache)).toBe(false)
      symlinkSync(outside,join(cache,'linked'),process.platform==='win32'?'junction':'dir')
      expect(allowFreshViteDependency(url(join(cache,'linked','future.js')),cache)).toBe(false)
      expect(allowFreshViteDependency('/bridge.js',cache)).toBe(false)
    } finally {rmSync(owned,{recursive:true,force:true})}
  })
})

describe('bounded browser failure diagnostics',()=>{
  it('retains original error, request failure and cleanup fields',()=>{
    const value={passed:false,error:'Script HTTP 404: /@fs/tmp/owned/wasmoon.js',
      diagnostics:[{kind:'http-denied',detail:'/@fs/tmp/owned/wasmoon.js'}],
      sourceStable:true,endpointClosed:true,browserExited:true,launcherExit:0,cleanupFallback:false}
    const summary=browserFailureSummary(value)
    expect(summary.error).toBe(value.error)
    expect(summary.diagnostics).toEqual(value.diagnostics)
    expect(summary.endpointClosed).toBe(true)
    expect(summary.browserExited).toBe(true)
    expect(summary.launcherExit).toBe(0)
  })
  it('bounds diagnostics while reporting omitted entries',()=>{
    const summary=browserFailureSummary({error:'e'.repeat(10000),
      diagnostics:Array.from({length:64},()=>({kind:'network',detail:'d'.repeat(5000)}))})
    expect(summary.error).toHaveLength(4096)
    expect(summary.diagnostics).toHaveLength(16)
    expect(summary.diagnosticCount).toBe(64)
    expect(JSON.stringify(summary).length).toBeLessThan(23000)
  })
})

describe('owned browser protocol readiness before the measured workload',()=>{
  const endpoint='http://127.0.0.1:9222'
  const version={Browser:'Chrome/154.0.0.0',webSocketDebuggerUrl:'ws://127.0.0.1:9222/devtools/browser/owned'}
  const targets=[{id:'page',type:'page',url:'about:blank',webSocketDebuggerUrl:'ws://127.0.0.1:9222/devtools/page/owned'}]
  const response=(value,status=200)=>({status,ok:status===200,json:async()=>value,body:{cancel:async()=>{}}})
  function fixture(queue) {
    let now=0,exited=false,launchError=null
    const urls=[], pauses=[], diagnostics=[], startup={}
    const options={endpoint,deadline:500,now:()=>now,pause:async ms=>{pauses.push(ms);now+=ms},
      getChildState:()=>({exited,launchError}),startup,diagnostic:(kind,value)=>diagnostics.push({kind,value}),
      fetchImpl:async url=>{urls.push(url);const item=queue.shift();if(item instanceof Error)throw item
        if(typeof item==='function')return item();if(!item)throw Error('Unexpected readiness fetch');return item}}
    return {options,urls,pauses,diagnostics,startup,setExited:()=>{exited=true},setLaunchError:error=>{launchError=error},setNow:value=>{now=value}}
  }
  it('waits for HTTP after the owned port file exists, without relaunching a browser',async()=>{
    const timedOut=new Error('version not ready');timedOut.name='TimeoutError'
    const refused=new TypeError('fetch failed');refused.cause={code:'ECONNREFUSED'}
    const test=fixture([timedOut,refused,response(version),response(targets)])
    expect(await waitForBrowserStartup(test.options)).toEqual({version,targets})
    expect(test.urls).toEqual([endpoint+'/json/version',endpoint+'/json/version',endpoint+'/json/version',endpoint+'/json/list'])
    expect(test.pauses).toHaveLength(2)
    expect(test.startup.attempts).toBe(4)
    expect(test.startup.transientFailures).toBe(2)
  })
  it('waits through declared transient HTTP 503 and a well-formed empty target list',async()=>{
    const test=fixture([response({},503),response(version),response([]),response(targets)])
    expect(await waitForBrowserStartup(test.options)).toEqual({version,targets})
    expect(test.urls.filter(url=>url.endsWith('/json/version'))).toHaveLength(2)
    expect(test.urls.filter(url=>url.endsWith('/json/list'))).toHaveLength(2)
    expect(test.startup.emptyTargetLists).toBe(1)
  })
  it('fails when the retained browser exits during startup, before another request',async()=>{
    const test=fixture([])
    test.options.fetchImpl=async()=>{test.urls.push('attempt');test.setExited();const error=new Error('slow startup');error.name='TimeoutError';throw error}
    await expect(waitForBrowserStartup(test.options)).rejects.toThrow(/exited/)
    expect(test.urls).toHaveLength(1)
  })
  it('preserves a real launch error before issuing a readiness request',async()=>{
    const test=fixture([response(version),response(targets)])
    test.setLaunchError(new Error('declared launch failure'))
    await expect(waitForBrowserStartup(test.options)).rejects.toThrow('declared launch failure')
    expect(test.urls).toEqual([])
  })
  it('does not restart its overall deadline after a transient failure',async()=>{
    const test=fixture([])
    test.options.fetchImpl=async()=>{test.urls.push('attempt');const error=new Error('not ready');error.name='TimeoutError';throw error}
    await expect(waitForBrowserStartup(test.options)).rejects.toThrow(/startup deadline/)
    expect(test.urls.length).toBeGreaterThan(0)
    expect(test.urls.length).toBeLessThanOrEqual(10)
    expect(test.pauses.reduce((sum,ms)=>sum+ms,0)).toBe(500)
  })
  it('rejects an already expired deadline without starting HTTP',async()=>{
    const test=fixture([response(version),response(targets)]);test.setNow(501)
    await expect(waitForBrowserStartup(test.options)).rejects.toThrow(/startup deadline/)
    expect(test.urls).toEqual([])
  })
  it('fails immediately on malformed JSON instead of retrying it',async()=>{
    const bad=response(null);bad.json=async()=>{throw new SyntaxError('bad DevTools JSON')}
    const test=fixture([bad,response(version),response(targets)])
    await expect(waitForBrowserStartup(test.options)).rejects.toThrow('bad DevTools JSON')
    expect(test.urls).toHaveLength(1);expect(test.pauses).toEqual([])
  })
  it.each([
    ['wrong version shape',{},targets],
    ['wrong endpoint identity',{...version,webSocketDebuggerUrl:'ws://127.0.0.1:9333/devtools/browser/other'},targets],
    ['wrong target shape',version,{page:targets[0]}],
    ['invalid target entry',version,[null]],
  ])('rejects %s without retry',async(_name,versionValue,targetValue)=>{
    const test=fixture([response(versionValue),response(targetValue)])
    await expect(waitForBrowserStartup(test.options)).rejects.toThrow(/Invalid/)
    expect(test.pauses).toEqual([])
  })
  it('does not retry a permanent HTTP response',async()=>{
    const test=fixture([response({},404),response(version),response(targets)])
    await expect(waitForBrowserStartup(test.options)).rejects.toThrow(/HTTP 404/)
    expect(test.urls).toHaveLength(1);expect(test.pauses).toEqual([])
  })
  it('retries a real loopback response-body timeout with native fetch and the same absolute deadline',async()=>{
    const sockets=new Set(),timers=new Set(),requests=[]
    let origin,firstHeadersSent=false,versionRequests=0
    const server=createServer((request,response)=>{
      requests.push(request.url)
      const value=request.url==='/json/version'
        ? {Browser:'Chrome/154.0.0.0',webSocketDebuggerUrl:origin.replace('http:','ws:')+'/devtools/browser/owned'}
        : [{id:'owned',type:'page',url:'about:blank',webSocketDebuggerUrl:origin.replace('http:','ws:')+'/devtools/page/owned'}]
      response.writeHead(200,{'content-type':'application/json'})
      if(request.url==='/json/version' && ++versionRequests===1){
        response.flushHeaders();firstHeadersSent=true
        const timer=setTimeout(()=>{timers.delete(timer);if(!response.destroyed)response.end(JSON.stringify(value))},1300)
        timers.add(timer)
      }else response.end(JSON.stringify(value))
    })
    server.on('connection',socket=>{sockets.add(socket);socket.on('close',()=>sockets.delete(socket))})
    try {
      await new Promise((yes,no)=>{server.once('error',no);server.listen(0,'127.0.0.1',yes)})
      origin='http://127.0.0.1:'+server.address().port
      const startup={},deadline=Date.now()+4000
      const result=await waitForBrowserStartup({endpoint:origin,deadline,startup})
      expect(firstHeadersSent).toBe(true)
      expect(requests).toEqual(['/json/version','/json/version','/json/list'])
      expect(startup.attempts).toBe(3)
      expect(startup.transientFailures).toBe(1)
      expect(startup.lastFailure).toMatch(/abort|timeout/i)
      expect(startup.ready).toBe(true)
      expect(Date.now()).toBeLessThan(deadline)
      expect(result.targets[0].webSocketDebuggerUrl).toBe(origin.replace('http:','ws:')+'/devtools/page/owned')
    }finally{
      for(const timer of timers)clearTimeout(timer)
      timers.clear()
      const closed=new Promise((yes,no)=>server.close(error=>error?no(error):yes()))
      for(const socket of sockets)socket.destroy()
      await closed
      expect(server.listening).toBe(false)
      expect(await new Promise((yes,no)=>server.getConnections((error,count)=>error?no(error):yes(count)))).toBe(0)
    }
  },10000)
})
