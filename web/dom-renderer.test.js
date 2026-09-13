// @vitest-environment jsdom
// =============================================================================
//  Caesura (AmeKAG) — web/dom-renderer.test.js
//  Sprint 2 / t12: per-frame cost of the DOM render path.
//
//  Two properties are pinned here, both measured rather than asserted by
//  eyeball:
//    1. render() is serialized — a slow layer source (the wasmoon hop) must
//       not let two renders interleave.
//    2. a steady scene costs ZERO DOM writes per frame — style properties and
//       the <img> src are only written when the value actually changes.
//
//  Instrumentation: jsdom defines each CSS property as an accessor on
//  CSSStyleDeclaration.prototype, so the setters can be wrapped to count real
//  writes; Element.prototype.setAttribute / removeAttribute are wrapped the
//  same way. This counts what the renderer actually does to the DOM, not what
//  we believe it does.
// =============================================================================

import { describe, it, expect, beforeEach, afterEach } from 'vitest'
import { DomRenderer } from './dom-renderer.js'
import { AdapterCore } from './adapter-core.js'
import { installCanvasHost } from './test-support/canvas-host.js'

const COUNTED_STYLE_PROPS = [
  'transition', 'left', 'top', 'width', 'height', 'opacity', 'zIndex', 'filter',
]

function installCounters(styles = null) {
  const counts = { style: 0, setAttribute: 0, srcSet: 0, srcRemoved: 0, byProp: {} }
  const restore = []

  // jsdom 30 defines the CSS property accessors on CSSStyleProperties (a
  // subclass of CSSStyleDeclaration), and window.CSSStyleProperties is not
  // exposed — so derive the prototype from a real element's style object
  // instead of naming the class. Verified by probe: depth 0 =
  // CSSStyleProperties carries get/set for 'left', depth 1
  // (CSSStyleDeclaration) does not.
  const proto = Object.getPrototypeOf(document.createElement('div').style)
  for (const prop of COUNTED_STYLE_PROPS) {
    const desc = Object.getOwnPropertyDescriptor(proto, prop)
    if (!desc || typeof desc.set !== 'function') continue
    const originalSet = desc.set
    Object.defineProperty(proto, prop, {
      ...desc,
      set(value) {
        if (!styles || styles.has(this)) {
          counts.style += 1
          counts.byProp[prop] = (counts.byProp[prop] || 0) + 1
        }
        return originalSet.call(this, value)
      },
    })
    restore.push(() => Object.defineProperty(proto, prop, desc))
  }

  const elProto = window.Element.prototype
  const originalSetAttr = elProto.setAttribute
  elProto.setAttribute = function (name, value) {
    counts.setAttribute += 1
    if (name === 'src') counts.srcSet += 1
    return originalSetAttr.call(this, name, value)
  }
  restore.push(() => { elProto.setAttribute = originalSetAttr })

  const originalRemoveAttr = elProto.removeAttribute
  elProto.removeAttribute = function (name) {
    if (name === 'src') counts.srcRemoved += 1
    return originalRemoveAttr.call(this, name)
  }
  restore.push(() => { elProto.removeAttribute = originalRemoveAttr })

  return { counts, restore: () => { for (const fn of restore.reverse()) fn() } }
}

/** Minimal AdapterCore stand-in: a fixed layer list plus the fields the
 *  renderer reads (palette / draws / textBuffer). */
function makeCore(layers) {
  return {
    palette: { handle: null, intensity: 0 },
    draws: [],
    textBuffer: '',
    renderList: () => layers,
  }
}

const STEADY_LAYERS = [
  { name: 'bg', x: 0, y: 0, w: 1280, h: 720, z: 0, opacity: 255, texture: 1 },
  { name: '_char_Sakura', x: 440, y: 120, w: 400, h: 600, z: 10, opacity: 255, texture: 2 },
]

let harness = null
let root = null

beforeEach(() => {
  document.body.innerHTML = ''
  root = document.createElement('div')
  document.body.appendChild(root)
})

afterEach(() => {
  if (harness) { harness.restore(); harness = null }
})

describe('DomRenderer per-frame DOM cost (t12)', () => {
  it('a steady scene costs zero DOM writes after the first frame', async () => {
    const core = makeCore(STEADY_LAYERS)
    const renderer = new DomRenderer(core, root)
    renderer.setTextureUrl(1, '/assets/bg/classroom.png')
    renderer.setTextureUrl(2, '/assets/fg/girl_uniform.png')

    // Frame 1 builds the elements: writes are expected here.
    harness = installCounters()
    await renderer.render()
    const first = { ...harness.counts, byProp: { ...harness.counts.byProp } }

    // Frames 2..31 change nothing.
    const FRAMES = 30
    const before = { style: harness.counts.style, srcSet: harness.counts.srcSet }
    for (let i = 0; i < FRAMES; i++) await renderer.render()
    const steady = {
      style: harness.counts.style - before.style,
      srcSet: harness.counts.srcSet - before.srcSet,
    }

    // eslint-disable-next-line no-console
    console.log('[t12] frame1 writes=' + JSON.stringify(first)
      + ' | ' + FRAMES + ' steady frames: styleWrites=' + steady.style
      + ' srcSets=' + steady.srcSet)

    expect(steady.style).toBe(0)
    expect(steady.srcSet).toBe(0)
    // The DOM still reflects the scene (zero writes must not mean zero output).
    const bg = root.querySelector('img[data-layer="bg"]')
    expect(bg).toBeTruthy()
    expect(bg.getAttribute('src')).toBe('/assets/bg/classroom.png')
    expect(bg.style.left).toBe('0px')
    const ch = root.querySelector('img[data-layer="_char_Sakura"]')
    expect(ch.style.left).toBe('440px')
    expect(ch.style.zIndex).toBe('10')
  })

  it('a real change is still written (diffing must not swallow updates)', async () => {
    const layers = [{ name: 'bg', x: 0, y: 0, w: 1280, h: 720, z: 0, opacity: 255, texture: 1 }]
    const core = makeCore(layers)
    const renderer = new DomRenderer(core, root)
    renderer.setTextureUrl(1, '/assets/bg/a.png')
    renderer.setTextureUrl(2, '/assets/bg/b.png')
    await renderer.render()

    harness = installCounters()
    layers[0].x = 120
    layers[0].opacity = 128
    await renderer.render()
    const el = root.querySelector('img[data-layer="bg"]')
    expect(el.style.left).toBe('120px')
    expect(Number(el.style.opacity)).toBeCloseTo(128 / 255, 5)
    expect(harness.counts.style).toBeGreaterThan(0)

    // texture swap on the SAME layer name must re-point src
    layers[0].texture = 2
    await renderer.render()
    expect(el.getAttribute('src')).toBe('/assets/bg/b.png')

    // texture removal must drop the attribute
    layers[0].texture = null
    await renderer.render()
    expect(el.hasAttribute('src')).toBe(false)
  })

  it('recovers when the DOM is mutated behind the renderer', async () => {
    // Correctness red line: a stale internal cache would refuse to rewrite a
    // value that an external actor (devtools, another script, a CSS-clearing
    // widget) has changed. The renderer must restore it on the next frame.
    const core = makeCore(STEADY_LAYERS)
    const renderer = new DomRenderer(core, root)
    renderer.setTextureUrl(1, '/assets/bg/classroom.png')
    renderer.setTextureUrl(2, '/assets/fg/girl_uniform.png')
    await renderer.render()

    const bg = root.querySelector('img[data-layer="bg"]')
    bg.style.left = '999px'
    bg.removeAttribute('src')
    await renderer.render()
    expect(bg.style.left).toBe('0px')
    expect(bg.getAttribute('src')).toBe('/assets/bg/classroom.png')
  })

  it('serializes overlapping renders (no interleaving through the async layer source)', async () => {
    // getLayers models the wasmoon hop: slow and async. Without
    // serialization two unawaited render() calls interleave.
    let inside = 0
    let maxConcurrent = 0
    let calls = 0
    const core = makeCore([])
    const renderer = new DomRenderer(core, root, {
      getLayers: async () => {
        calls += 1
        inside += 1
        maxConcurrent = Math.max(maxConcurrent, inside)
        await new Promise((r) => setTimeout(r, 5))
        inside -= 1
        return STEADY_LAYERS
      },
    })
    renderer.setTextureUrl(1, '/assets/bg/classroom.png')
    renderer.setTextureUrl(2, '/assets/fg/girl_uniform.png')

    // 8 frames fired without awaiting, exactly like the rAF loop does.
    const promises = []
    for (let i = 0; i < 8; i++) promises.push(renderer.render())
    await Promise.all(promises)

    // eslint-disable-next-line no-console
    console.log('[t12] 8 unawaited renders -> getLayers calls=' + calls
      + ' maxConcurrent=' + maxConcurrent)

    expect(maxConcurrent).toBe(1)
    // Coalescing: the burst collapses to far fewer passes than calls, and the
    // final state is still rendered (no dropped last frame).
    expect(calls).toBeLessThan(8)
    expect(calls).toBeGreaterThanOrEqual(2)
    expect(root.querySelector('img[data-layer="bg"]')).toBeTruthy()
  })

  it('awaiting render() resolves only after the latest requested pass', async () => {
    // A caller that awaits must observe the state it asked for, even when its
    // request was coalesced into an in-flight pass.
    const layers = [{ name: 'bg', x: 0, y: 0, w: 100, h: 100, z: 0, opacity: 255, texture: 1 }]
    const core = makeCore(layers)
    const renderer = new DomRenderer(core, root, {
      getLayers: async () => {
        await new Promise((r) => setTimeout(r, 5))
        return layers
      },
    })
    renderer.setTextureUrl(1, '/assets/bg/a.png')

    const first = renderer.render()
    layers[0].x = 777
    await renderer.render()
    await first
    expect(root.querySelector('img[data-layer="bg"]').style.left).toBe('777px')
  })
})

describe('DomRenderer message overlay after graphics replacement (U21)', () => {
  // These positioned siblings share one stacking context. Compare their real
  // CSS stacking levels and document order; jsdom cannot prove glyph pixels.
  function expectMessageAboveGraphics(message) {
    expect(message.style.position).toBe('absolute')
    const messageZ = Number(getComputedStyle(message).zIndex) || 0
    for (const graphics of root.querySelectorAll('.caesura-layer')) {
      const graphicsZ = Number(getComputedStyle(graphics).zIndex) || 0
      const follows = Boolean(graphics.compareDocumentPosition(message) & Node.DOCUMENT_POSITION_FOLLOWING)
      expect(messageZ > graphicsZ || (messageZ === graphicsZ && follows),
        'message must paint above ' + graphics.tagName + ':' + graphics.dataset.layer).toBe(true)
    }
  }

  it.each(['structured', 'flat'])('keeps %s text above the same layer when IMG becomes a prepared CANVAS and back', async mode => {
    const restoreCanvas = installCanvasHost()
    const core = new AdapterCore()
    const renderer = new DomRenderer(core, root)
    try {
      const bg = core.ensureLayer('bg', {id:'background', z:0, w:1280, h:720})
      const original = core.loadTexture('assets/bg.png')
      core.setLayerImage(bg, original)
      renderer.setTextureUrl(original, '/assets/bg.png')
      if (mode === 'structured') core.setDraws([{t:'VISIBLE_DONE', x:40, y:50, r:255, g:255, b:255, s:1}])
      else core.setText('VISIBLE_DONE')
      await renderer.render()
      const image = root.querySelector('img[data-layer="bg"]')
      const message = root.querySelector('.caesura-message')
      expect(image?.getAttribute('src')).toBe('/assets/bg.png')
      expect(message?.textContent).toBe('VISIBLE_DONE')
      expectMessageAboveGraphics(message)

      const restored = core.registerPreparedTexture({kind:'asset', path:'assets/bg.png'}, {
        width:2, height:2,
        draw(context) { context.fillStyle = '#106010'; context.fillRect(0, 0, 2, 2) },
        dispose() {},
      })
      core.setLayerImage(bg, restored)
      await renderer.render()
      const canvas = root.querySelector('canvas[data-layer="bg"]')
      expect(canvas).not.toBeNull()
      expect(image.isConnected).toBe(false)
      expect(core.getLayer('bg').id).toBe('background')
      expect(root.querySelector('.caesura-message')).toBe(message)
      expect(message.textContent).toBe('VISIBLE_DONE')
      expect([...canvas.getContext('2d').getImageData(0, 0, 1, 1).data]).toEqual([16, 96, 16, 255])
      expectMessageAboveGraphics(message)

      core.setLayerImage(bg, original)
      await renderer.render()
      expect(root.querySelector('img[data-layer="bg"]')?.getAttribute('src')).toBe('/assets/bg.png')
      expect(canvas.isConnected).toBe(false)
      expect(root.querySelector('.caesura-message')).toBe(message)
      expectMessageAboveGraphics(message)
    } finally { renderer.destroy(); restoreCanvas() }
  })

  it('keeps the overlay above added and reordered graphics without steady container style or root-order writes', async () => {
    const core = new AdapterCore()
    const renderer = new DomRenderer(core, root)
    const texture = core.loadTexture('assets/foreground.png')
    core.setLayerImage(core.ensureLayer('bg', {z:0}), texture)
    core.setText('VISIBLE_DONE')
    await renderer.render()
    const message = root.querySelector('.caesura-message')
    const foreground = core.ensureLayer('fg', {z:10})
    core.setLayerImage(foreground, texture)
    await renderer.render()
    expectMessageAboveGraphics(message)
    core.setLayerProperty(foreground, 'z', 2147483647)
    await renderer.render()
    expectMessageAboveGraphics(message)
    expect(root.querySelector('.caesura-message')).toBe(message)

    // Text descendants are rebuilt by the existing renderer. Measure the
    // persistent containers affected by stacking, without claiming to remove
    // those unrelated text-layout writes.
    harness = installCounters(new Set([message, ...root.querySelectorAll('.caesura-layer')].map(node => node.style)))
    const changes = []
    const observer = new MutationObserver(records => changes.push(...records))
    observer.observe(root, {childList:true})
    try {
      for (let frame = 0; frame < 10; ++frame) await renderer.render()
      expect(harness.counts.style).toBe(0)
      expect(changes).toEqual([])
      expectMessageAboveGraphics(message)
    } finally { observer.disconnect(); renderer.destroy() }
  })
})
