// @vitest-environment node
// Story-bundle path performance comparison (runFromBundle vs runScene).
//
// Question: does the ks_bake bundle path (compiled-token deserialize +
// Lua-literal scenes bridge) carry per-scene overhead vs the raw .ks source
// path (tokenizer.parse + full compile)? We bench BOTH paths on:
//   1. a tiny scene  -> isolates the FIXED per-run startup cost,
//   2. the real example_game/story.ks scene, and
//   3. a large synthetic scene (1000+ commands).
// Assertion: the bundle path is no more than 20% slower (>= 0.8x token
// dispatch throughput). Interleaved runs cancel warmup/drift bias.
import { describe, it, expect, beforeAll } from "vitest"
import { readFileSync, existsSync } from "node:fs"
import { fileURLToPath } from "node:url"
import { dirname, join } from "node:path"
import { createPlayer } from "./bridge.js"
const here = dirname(fileURLToPath(import.meta.url))
const rootDir = join(here, "..")
const scriptsDir = join(rootDir, "scripts")
const assetsDir = join(rootDir, "assets")
const index = JSON.parse(readFileSync(join(here, "scripts-index.json"), "utf8"))
const fileFetch = async (url) => {
  const u = new URL(url)
  if (u.pathname.startsWith("/assets/lang/")) {
    const rel = u.pathname.replace("/assets/lang/", "")
    const p = join(assetsDir, "lang", ...rel.split("/"))
    return { ok: existsSync(p), status: existsSync(p) ? 200 : 404,
      text: async () => (existsSync(p) ? readFileSync(p, "utf8") : ""), json: async () => index }
  }
  const rel = u.pathname.replace("/scripts/", "")
  const p = join(scriptsDir, ...rel.split("/"))
  const ok = existsSync(p)
  return { ok, status: ok ? 200 : 404, text: async () => (ok ? readFileSync(p, "utf8") : ""), json: async () => index }
}
let player = null
beforeAll(async () => {
  player = await createPlayer({
    scriptsBase: "http://local/scripts/", fetchImpl: fileFetch, langBase: "http://local/assets/lang/",
    wasmFile: join(here, "node_modules", "wasmoon", "dist", "glue.wasm"),
    capabilities: JSON.parse(readFileSync(join(here, '../demo/caesura.project.json'), 'utf8')).capabilities,
  })
})
const NLx = String.fromCharCode(10)
const Q = String.fromCharCode(34) // double-quote
async function bakeBundle(scenes) {
  const entries = []
  for (const key of Object.keys(scenes)) {
    const src = scenes[key]
    entries.push("  bundle_scenes[" + JSON.stringify(key) + "] = "
      + "(function() local t = tokenizer.parse(" + JSON.stringify(src) + ") "
      + "compiler.compile(t) return compiler.serialize(t) end)()")
  }
  const code = ["local tokenizer = require('tokenizer')", "local compiler = require('kag.compiler')",
    "local bundle_scenes = {}",
    entries.join(NLx),
    "return { version = 1, scenes = bundle_scenes, assets = {} }",
  ].join(NLx)
  return await player.lua.doString(code)
}
async function lastTokenIndex() {
  await player.lua.doString("_G.__LT = _G.__LAST_CTX and _G.__LAST_CTX.token_index or 0")
  return Number(player.lua.global.get("__LT")) || 0
}
/** Alternate fnA/fnB for N+ reps after one warmup rep each; return medians. */
function dualMedianMs(fnA, fnB, reps) {
  fnA(); fnB() // warmup
  const a = [], b = []
  for (let i = 0; i < reps; i++) {
    let t0 = process.hrtime.bigint(); fnA(); a.push(Number(process.hrtime.bigint() - t0) / 1e6)
    t0 = process.hrtime.bigint(); fnB(); b.push(Number(process.hrtime.bigint() - t0) / 1e6)
  }
  a.sort((x, y) => x - y); b.sort((x, y) => x - y)
  return { srcMs: a[Math.floor(a.length / 2)], bndMs: b[Math.floor(b.length / 2)] }
}
function syntheticScene(pages, cmdsPerPage) {
  const out = []
  for (let p = 0; p < pages; p++) {
    for (let c = 0; c < cmdsPerPage; c++) {
      out.push("[ch name=" + Q + "C" + Q + " text=" + Q + "line " + p + "-" + c + " padding padding padding padding padding" + Q + "]")
    }
    out.push("[p]")
  }
  out.push("[end]")
  return out.join(NLx)
}
function runBoth(sceneKey, src, bundle, maxFrames) {
  const srcFn = () => { player.core.backlog.length = 0; player.core.events.length = 0;
    void player.runScene(src, sceneKey, { maxFrames, autoClick: true }) }
  const bndFn = () => { player.core.backlog.length = 0; player.core.events.length = 0;
    void player.runFromBundle(bundle, sceneKey, { maxFrames, autoClick: true }) }
  return { srcFn, bndFn }
}

describe("bundle vs source performance (runFromBundle vs runScene)", () => {

  // ---- 0. tiny scene: isolates the fixed per-run startup cost ----
  it("tiny scene startup: bundle fixed cost must not be an order of magnitude worse", async () => {
    const key = "perf_tiny.ks"
    const src = "[ch name=\"C\" text=\"hi\"]\n[p]\n[ch name=\"C\" text=\"bye\"]\n[p]\n[end]"
    const bundle = await bakeBundle({ [key]: src })
    const reps = 10
    const { srcMs, bndMs } = dualMedianMs(
      (() => { player.core.backlog.length = 0; void player.runScene(src, key, { maxFrames: 100000, autoClick: true }) }),
      (() => { player.core.backlog.length = 0; void player.runFromBundle(bundle, key, { maxFrames: 100000, autoClick: true }) }),
      reps)
    // eslint-disable-next-line no-console
    console.log("[perf] tiny  source median=" + srcMs.toFixed(1) + "ms   bundle median=" + bndMs.toFixed(1) + "ms")
    // Fixed per-run cost should be the same order of magnitude (bundle within 3x).
    expect(bndMs, "bundle tiny-scene startup must be same order as source (\\3x)").toBeLessThan(srcMs * 3 + 50)
  }, 90000)

  // ---- 1. real story.ks scene, both paths ----
  it("real story.ks: bundle token throughput >= 0.8x source", async () => {
    const key = "story.ks"
    const src = readFileSync(join(rootDir, "demo", "example_game", key), "utf8")
    const bundle = await bakeBundle({ [key]: src })
    expect(bundle.scenes[key]).toBeTruthy()
    const reps = 6
    const { srcFn, bndFn } = runBoth(key, src, bundle, 1000000)
    const { srcMs, bndMs } = dualMedianMs(srcFn, bndFn, reps)
    // tokens are identical across paths (same scene); count from final ctx
    const tokens = await lastTokenIndex()
    const srcRate = tokens / Math.max(srcMs, 0.001)
    const bndRate = tokens / Math.max(bndMs, 0.001)
    // eslint-disable-next-line no-console
    console.log("[perf] story.ks  source median=" + srcMs.toFixed(1) + "ms  bundle median=" + bndMs.toFixed(1) + "ms  (" + tokens + " tok)  ratios " + srcRate.toFixed(0) + " vs " + bndRate.toFixed(0) + " tok/s")
    const ratio = bndRate / srcRate
    // eslint-disable-next-line no-console
    console.log("[perf] story.ks  bundle/source throughput ratio = " + ratio.toFixed(3) + " (must be >= 0.8)")
    expect(ratio, "bundle path must not be >20% slower than source").toBeGreaterThanOrEqual(0.8)
  }, 120000)

  // ---- 2. large synthetic scene (1000+ commands), both paths ----
  it("large synthetic scene (1000+ cmds): bundle token throughput >= 0.8x source", async () => {
    const key = "perf_big.ks"
    const src = syntheticScene(200, 6)
    const bundle = await bakeBundle({ [key]: src })
    expect(bundle.scenes[key]).toBeTruthy()
    const reps = 6
    const { srcFn, bndFn } = runBoth(key, src, bundle, 2000000)
    const { srcMs, bndMs } = dualMedianMs(srcFn, bndFn, reps)
    const tokens = await lastTokenIndex()
    expect(tokens, "synthetic scene should have 1000+ tokens").toBeGreaterThan(1000)
    const srcRate = tokens / Math.max(srcMs, 0.001)
    const bndRate = tokens / Math.max(bndMs, 0.001)
    // eslint-disable-next-line no-console
    console.log("[perf] big(1400+) source median=" + srcMs.toFixed(1) + "ms  bundle median=" + bndMs.toFixed(1) + "ms  (" + tokens + " tok)  ratios " + srcRate.toFixed(0) + " vs " + bndRate.toFixed(0) + " tok/s")
    const ratio = bndRate / srcRate
    // eslint-disable-next-line no-console
    console.log("[perf] big(1400+) bundle/source throughput ratio = " + ratio.toFixed(3) + " (must be >= 0.8)")
    expect(ratio, "bundle path must not be >20% slower than source").toBeGreaterThanOrEqual(0.8)
  }, 180000)
})
