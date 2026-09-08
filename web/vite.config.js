import { defineConfig } from 'vite'
import { copyFileSync, mkdirSync, writeFileSync } from 'node:fs'
import { resolve } from 'node:path'
import { buildIndex, serialize } from './gen-index.mjs'
import { readFileSync, existsSync } from 'node:fs'
import { copyDirectorySync } from '../scripts/copy_tree.mjs'

// The web player serves the whole repo root as static content so
// /scripts/, /demo/, /assets/ and /cache/story/story.lua resolve
// (dev server: publicDir '..'). For a production build we do NOT copy
// the whole repo into dist (node_modules/.git/build would bloat it);
// instead copyPublicDir is off and a plugin copies exactly the runtime
// directories the player needs.
const REPO_ROOT = resolve(process.cwd(), '..')
const RUNTIME_DIRS = ['scripts', 'demo', 'assets', 'cache/story']

function copyRuntimeDirs() {
  return {
    name: 'caesura-copy-runtime-dirs',
    closeBundle() {
      for (const dir of RUNTIME_DIRS) {
        const from = resolve(REPO_ROOT, dir)
        const to = resolve(process.cwd(), 'dist', dir)
        mkdirSync(to, { recursive: true })
        copyDirectorySync(from, to)
      }
      // W7: vendor the Lua VM wasm into web-assets so the packaged player is
      // offline-capable (wasmoon defaults to fetching glue.wasm from unpkg).
      const localWasm = resolve(process.cwd(), 'node_modules', 'wasmoon', 'dist', 'glue.wasm')
      if (existsSync(localWasm)) {
        copyFileSync(localWasm, resolve(process.cwd(), 'dist', 'web-assets', 'glue.wasm'))
      } else {
        console.warn('[vite] WARN: wasmoon glue.wasm not found under node_modules — packaged player would fetch it from unpkg.')
      }
      // Copy PWA Service Worker and Web App Manifest into dist/
      const swFile = resolve(process.cwd(), 'sw.js')
      if (existsSync(swFile)) {
        copyFileSync(swFile, resolve(process.cwd(), 'dist', 'sw.js'))
      }
      const manifestFile = resolve(process.cwd(), 'manifest.webmanifest')
      if (existsSync(manifestFile)) {
        copyFileSync(manifestFile, resolve(process.cwd(), 'dist', 'manifest.webmanifest'))
      }

      // bridge.js hard-depends on scriptsBase + 'index.json'; generate it for
      // the copied scripts tree so web/dist is self-contained (W0: the bare
      // dist used to boot-fail on /scripts/index.json 404).
      try {
        writeFileSync(resolve(process.cwd(), 'dist', 'scripts', 'index.json'), serialize(buildIndex(resolve(REPO_ROOT, 'scripts'))))
      } catch (e) {
        console.error('[vite] WARN: dist scripts/index.json generation failed:', String(e))
      }
    },
  }
}

// W7: build-time only — pin the Lua VM wasm to the vendored copy before the
// module script loads (wasmoon defaults to unpkg when __CAESURA_WASM_FILE__ is
// unset). Dev is left untouched so the dev server keeps its default behavior.
function w7WasmPin() {
  return {
    name: 'caesura-wasm-pin',
    apply: 'build',
    transformIndexHtml(html) {
      const pin = '<script>self.__CAESURA_WASM_FILE__ = new URL("web-assets/glue.wasm", document.baseURI || location.href).href</' + 'script>'
      return html.includes('__CAESURA_WASM_FILE__') ? html : html.replace('</head>', pin + String.fromCharCode(10) + '</head>')
    },
  }
}

// Dev server: the player boots from the served repo root, so /scripts/
// index.json must exist there — it is generated on the fly from scripts/
// (gitignored repo root has no committed copy; only web/scripts-index.json
// is the committed CI artifact).
function devScriptsIndex() {
  return {
    name: 'caesura-dev-scripts-index',
    configureServer(server) {
      server.middlewares.use((req, res, next) => {
        try {
          const pathname = new URL(req.url ?? '/', 'http://x').pathname
          if (pathname === '/scripts/index.json') {
            res.setHeader('content-type', 'application/json; charset=utf-8')
            res.end(serialize(buildIndex(resolve(REPO_ROOT, 'scripts'))))
            return
          }
        } catch (e) {
          res.statusCode = 500; res.end('scripts index error: ' + String(e)); return
        }
        next()
      })
    },
  }
}

export default defineConfig({
  publicDir: '..',
  server: { port: 5174, host: '127.0.0.1' },
  base: './',
  build: {
    outDir: 'dist',
    copyPublicDir: false,
    target: 'es2022',
    assetsDir: 'web-assets',
  },
  plugins: [copyRuntimeDirs(), devScriptsIndex(), w7WasmPin()],
})
