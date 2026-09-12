// @vitest-environment jsdom
// Regression: dialogue/choice text must survive the [save] command
// (device-open: on the Android device the text vanished AFTER the
// [save slot=7] + [notify] in first_vn; the same Lua command stack runs
// here, so a green result points at the C++ capture-thumbnail frame, a
// red result at the Lua scene/text state).
import { describe, it, expect, beforeAll } from 'vitest'
import { readFileSync, existsSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { dirname, join } from 'node:path'
import { createPlayer } from './bridge.js'
import { DomRenderer } from './dom-renderer.js'

const here = dirname(fileURLToPath(import.meta.url))
const rootDir = join(here, '..')
const scriptsDir = join(rootDir, 'scripts')
const assetsDir = join(rootDir, 'assets')
const index = JSON.parse(readFileSync(join(here, 'scripts-index.json'), 'utf8'))
const fileFetch = async (url) => {
  const u = new URL(url)
  if (u.pathname.startsWith('/assets/lang/')) {
    const rel = u.pathname.replace('/assets/lang/', '')
    const p = join(assetsDir, 'lang', ...rel.split('/'))
    return { ok: existsSync(p), status: existsSync(p) ? 200 : 404, text: async () => (existsSync(p) ? readFileSync(p, 'utf8') : ''), json: async () => index }
  }
  const rel = u.pathname.replace('/scripts/', '')
  const p = join(scriptsDir, ...rel.split('/'))
  return { text: async () => readFileSync(p, 'utf8'), json: async () => index, status: 200, ok: true }
}

let player = null
let renderer = null
let stage = null

beforeAll(async () => {
  player = await createPlayer({
    scriptsBase: 'http://local/scripts/',
    fetchImpl: fileFetch,
    langBase: 'http://local/assets/lang/',
    capabilities: JSON.parse(readFileSync(join(rootDir, 'tests/projects/first_vn/caesura.project.json'), 'utf8')).capabilities,
    wasmFile: join(here, 'node_modules', 'wasmoon', 'dist', 'glue.wasm'),
  })
  stage = document.createElement('div')
  document.body.appendChild(stage)
  renderer = new DomRenderer(player.core, stage)
})

describe('first_vn save -> choice text regression', () => {
  it('message text persists through [save] and the choice renders', async () => {
    const ks = readFileSync(join(rootDir, 'tests', 'projects', 'first_vn', 'story.ks'), 'utf8')
    const SCENE = 'first_vn/story.ks'
    let out = await player.runScene(ks, SCENE, { maxFrames: 60000 })
    expect(out.startsWith('WAIT:')).toBe(true)

    const texts = []
    for (let i = 0; i < 14; i++) {
      await renderer.render()
      const msg = stage.querySelector('.caesura-message')
      const t = msg ? String(msg.textContent || '') : ''
      texts.push(t.trim())
      out = await player.runScene(ks, SCENE, {
        advance: true, advanceScene: SCENE, maxFrames: 60000,
      })
      if (out.startsWith('DONE:')) break
    }
    // After the save the page text (choice intro) must not be empty.
    const nonEmpty = texts.filter((s) => s.length > 0)
    expect(nonEmpty.length).toBeGreaterThan(5)
    expect(texts[texts.length - 1].length).toBeGreaterThan(0)
  }, 120000)
})
