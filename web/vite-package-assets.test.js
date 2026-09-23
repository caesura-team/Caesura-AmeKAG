// @vitest-environment node
import { afterEach, describe, expect, it } from 'vitest'
import { spawnSync } from 'node:child_process'
import { existsSync, mkdirSync, mkdtempSync, readdirSync, readFileSync, realpathSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, isAbsolute, join, relative, resolve } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'
import { copyDirectorySync } from '../scripts/copy_tree.mjs'

const here = dirname(fileURLToPath(import.meta.url))
const temporary = []
const files = root => readdirSync(root, { withFileTypes: true }).flatMap(entry =>
  entry.isDirectory() ? files(join(root, entry.name)) : [join(root, entry.name)])

function fixture() {
  const root = realpathSync.native(mkdtempSync(join(tmpdir(), 'caesura-vite-package-')))
  temporary.push(root)
  const web = join(root, 'web')
  mkdirSync(web)
  return { root, web }
}

function runVite(command, root, web) {
  // Keep the compiler/dev-server lifecycle in a separate Node process;
  // Vitest observes the actual emitted files and the HTTP response.
  const program = `
    import { build, createServer, loadConfigFromFile } from ${JSON.stringify(pathToFileURL(resolve(here, 'node_modules/vite/dist/node/index.js')).href)};
    import { dirname, join } from 'node:path';
    const [configFile, command, root, web] = process.argv.slice(1);
    const loaded = await loadConfigFromFile({ command, mode: command === 'build' ? 'production' : 'development' }, configFile, dirname(configFile), 'silent');
    if (!loaded) throw new Error('Actual project config did not load');
    const config = { ...loaded.config, configFile: false, root: web, plugins: [], logLevel: 'error', cacheDir: join(root, 'vite-cache') };
    if (command === 'build') {
      await build({ ...config, build: { ...config.build, outDir: join(root, 'dist'), emptyOutDir: false } });
    } else {
      const server = await createServer({ ...config, server: { host: '127.0.0.1', port: 0, strictPort: true } });
      try {
        await server.listen();
        const response = await fetch('http://127.0.0.1:' + server.httpServer.address().port + '/assets/fixture.txt', { redirect: 'error' });
        console.log('VITE_FIXTURE_RESULT:' + JSON.stringify({ status: response.status, body: await response.text() }));
      } finally { await server.close(); }
    }
  `
  const env = Object.fromEntries(Object.entries(process.env).filter(([key]) => !/^(VITEST|VITE_NODE|TINYPOOL)/.test(key)))
  const result = spawnSync(process.execPath, ['--input-type=module', '-e', program, join(here, 'vite.config.js'), command, root, web],
    { cwd: web, env, encoding: 'utf8', timeout: 45000, windowsHide: true })
  expect(result.error, result.stderr).toBeUndefined()
  expect(result.status, result.stdout + result.stderr).toBe(0)
  return result.stdout
}

afterEach(() => {
  for (const directory of temporary.splice(0)) rmSync(directory, { recursive: true, force: true })
})

describe('Vite package asset paths', () => {
  it('emits resolvable Wasmoon WASM references with ordinary in-checkout dependencies', async () => {
    const { root, web } = fixture()
    // A physical copy models npm ci. A worktree's node_modules junction can
    // move Wasmoon outside publicDir and hide this production build defect.
    mkdirSync(join(web, 'node_modules'))
    copyDirectorySync(realpathSync.native(join(here, 'node_modules/wasmoon')), join(web, 'node_modules/wasmoon'))
    writeFileSync(join(web, 'index.html'), '<script type="module" src="./main.js"></script>\n')
    writeFileSync(join(web, 'main.js'), "import { Lua } from 'wasmoon'; globalThis.fixtureLuaClass = Lua;\n")
    const output = join(root, 'dist')
    // Exercise real Vite and the real dependency with the production config's
    // asset settings. Project game-copy plugins need the full game tree and
    // are covered by final-package validation; this fixture never runs a VM.
    runVite('build', root, web)
    const references = []
    for (const script of files(output).filter(path => path.endsWith('.js'))) {
      for (const match of readFileSync(script, 'utf8').matchAll(/new URL\(\s*(["'])([^"']+\.wasm)\1\s*,\s*import\.meta\.url\s*\)/g)) {
        const target = fileURLToPath(new URL(match[2], pathToFileURL(script)))
        const inside = relative(output, target)
        references.push(match[2])
        expect(!inside.startsWith('..') && !isAbsolute(inside), 'WASM reference stays inside output').toBe(true)
        expect(existsSync(target), `built WASM reference must exist: ${match[2]}`).toBe(true)
        expect(readFileSync(target).equals(readFileSync(join(web, 'node_modules/wasmoon/dist/glue.wasm'))),
          'emitted WASM bytes match the selected real dependency').toBe(true)
      }
    }
    expect(references.length, 'inspect an actual Wasmoon WASM URL in the built JavaScript').toBeGreaterThan(0)
  }, 60000)

  it('still serves repo-relative public resources in development', async () => {
    const { root, web } = fixture()
    mkdirSync(join(root, 'assets'))
    writeFileSync(join(root, 'assets/fixture.txt'), 'owned development asset\n')
    const stdout = runVite('serve', root, web)
    const results = stdout.split('\n').filter(line => line.startsWith('VITE_FIXTURE_RESULT:'))
    expect(results).toHaveLength(1)
    expect(JSON.parse(results[0].slice('VITE_FIXTURE_RESULT:'.length))).toEqual({ status: 200, body: 'owned development asset\n' })
  }, 60000)
})
