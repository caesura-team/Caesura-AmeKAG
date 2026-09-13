import { readFileSync, existsSync } from 'node:fs'
import { dirname, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { vi } from 'vitest'
import { createPlayer } from '../bridge.js'
import { luaLiteralValue } from '../lua-value.js'
import { installCanvasHost } from './canvas-host.js'

const web = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const root = resolve(web, '..')
export const entryKey = 'nested/入口.ks'

// One real main.mjs session per isolated test file. The fixture itself is
// compiled by the production tokenizer/compiler in Wasmoon, not handwritten
// token metadata. The numeric-string scene key has deterministic JS key order.
export async function bootEntryPage({ entry = entryKey, legacy = false, sceneSources, savedSlot } = {}) {
  installCanvasHost()
  const frames = new Set()
  vi.stubGlobal('requestAnimationFrame', callback => {
    const id = setTimeout(() => { frames.delete(id); callback(performance.now()) }, 16)
    frames.add(id); return id
  })
  vi.stubGlobal('cancelAnimationFrame', id => { clearTimeout(id); frames.delete(id) })
  vi.stubGlobal('FontFace', undefined)
  vi.stubGlobal('AudioContext', undefined)
  localStorage.clear()
  const wasmFile = resolve(web, 'node_modules/wasmoon/dist/glue.wasm')
  async function fileFetch(url) {
    const path = new URL(url).pathname
    const file = path === '/scripts/index.json' ? resolve(web, 'scripts-index.json') : resolve(root, '.' + path)
    return new Response(existsSync(file) ? readFileSync(file) : '', {status: existsSync(file) ? 200 : 404})
  }
  const compiler = await createPlayer({scriptsBase:'http://local/scripts/', fetchImpl:fileFetch, wasmFile})
  let scenes
  try {
    const sources = sceneSources ?? {'0':'[ch text="U21_LEGACY_FIRST"]\n[end]',
      [entryKey]:'[ch text="U21_SELECTED_ENTRY"]\n[end]'}
    scenes = await compiler.lua.doString(`
      local tokenizer,compiler=require('tokenizer'),require('kag.compiler')
      local scenes={}
      for name,source in pairs(${luaLiteralValue(sources)}) do
        local tokens=tokenizer.parse(source)
        compiler.compile(tokens);scenes[name]=compiler.serialize(tokens)
      end
      return scenes
    `)
    if (savedSlot !== undefined) {
      const parked = await compiler.runFromBundle({version:1, assets:[], scenes}, entryKey, {autoClick:false})
      if (!parked.startsWith('WAIT:') || !await compiler.saveCurrent(savedSlot)) {
        throw new Error('The previous real player did not produce a saved slot')
      }
    }
  } finally { await compiler.dispose() }
  const bundle = {version:1, assets:[], scenes, ...(legacy ? {} : {entry})}
  const bundleSource = 'return ' + luaLiteralValue(bundle)
  const html = readFileSync(resolve(web, 'index.html'), 'utf8')
  document.body.innerHTML = html.match(/<body[^>]*>([\s\S]*?)<\/body>/i)[1].replace(/<script\b[^>]*>[\s\S]*?<\/script>/gi, '')
  vi.stubGlobal('__CAESURA_WASM_FILE__', wasmFile)
  vi.stubGlobal('__CAESURA_DEV__', false)
  vi.stubGlobal('fetch', async input => {
    const url = typeof input === 'string' ? input : input.url
    if (new URL(url).pathname === '/cache/story/story.lua') return new Response(bundleSource)
    return fileFetch(url)
  })
  await import('../main.mjs')
  return () => {
    for (const id of frames) clearTimeout(id)
    frames.clear()
    window.__caesuraAudio?.destroy()
    window.__caesuraGestures?.detach()
    vi.unstubAllGlobals()
  }
}
