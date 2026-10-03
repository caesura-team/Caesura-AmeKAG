// G5 path-B web player — DOM renderer for AdapterCore.
// Renders the layer tree into a container element: each visible layer
// with a texture becomes a positioned <img>; the message layer shows
// text. jsdom-testable (no real browser APIs beyond DOM).

// Layer tag -> element kind: image-bearing layers render as <img>;
// message/ui layers as overlay <div>.
const IMAGE_LAYER_TAGS = new Set(['bg', 'fg', 'layer0', 'layer1', 'fore', '_char_', 'image'])

// Web-side LUT color-grading: map the active palette (core.palette) to a
// CSS filter on the render container. This is the DOM render path analog of
// the desktop backend.set_palette binding (s_lutTex applied to future
// submits). handle==null (day/neutral) -> none; otherwise a blue-dark
// "night" grade whose strength follows intensity 0..1.
function paletteFilter(palette) {
  if (!palette || palette.handle == null || !(Number(palette.intensity) > 0)) return ''
  const t = Math.min(1, Math.max(0, Number(palette.intensity) || 0))
  // night = cool, dim blue cast (brightness down, blue/sepia grade up
  // with t). Linear in intensity so day<->night toggle visibly shifts.
  const b = Math.round(16 * t)
  const h = Math.round(198 * t)
  const sat = 1 + 0.25 * t
  return 'brightness(' + (1 - 0.18 * t).toFixed(3) + ') sepia(' + b + '%) hue-rotate(' + h + 'deg) saturate(' + sat.toFixed(3) + ')'
}

// Write a style property only when the DOM value actually differs (t12).
//
// Why compare against the ELEMENT and not a cached "last written" map: a cache
// goes stale the moment anything else touches the node (devtools, another
// script, a CSS-clearing widget, or an element reused for a different layer),
// and a stale cache makes the renderer REFUSE to repair the difference. Reading
// el.style.<prop> is an inline-style read — it parses the style attribute and
// does NOT force layout or style recalc (unlike getComputedStyle), so the
// comparison is cheap and the renderer stays self-healing by construction.
function setStyle(el, prop, value) {
  if (el.style[prop] !== value) el.style[prop] = value
}

export class DomRenderer {
  constructor(core, rootEl, opts = {}) {
    this.core = core
    this.root = rootEl
    this.width = opts.width ?? 1280
    this.height = opts.height ?? 720
    this.textureUrls = new Map() // id -> src string
    this._els = new Map() // stable layer id (legacy fallback: name) -> element
    this._drawnPrepared = new WeakMap() // canvas -> immutable prepared resource
    this._textEl = null
    /** Optional external layer source (Lua Layers.snapshot()); when set it
     *  takes precedence over core.renderList(). */
    this.getLayers = opts.getLayers ?? null
    // ---- render serialization (t12) ------------------------------------
    // render() crosses the wasm boundary via getLayers(); the rAF loop calls
    // it without awaiting, so a slow pass used to overlap with the next one
    // (both walking the same layer list and writing the same nodes).
    // _inFlight holds the running pass; while it runs, further calls do not
    // start a second walk — they set _pending and share _pendingPromise, so a
    // burst of N frames collapses to "the current pass + exactly one more".
    //
    // Why this cannot make the picture lag: the coalesced pass runs
    // immediately after the current one and reads core state FRESH at that
    // moment, so it renders the newest state, not a queued stale frame. The
    // work that is dropped is only the redundant intermediate walks whose
    // output would have been overwritten in the same rAF burst anyway. The
    // last requested frame is always executed — never skipped.
    this._inFlight = null
    this._pending = false
    this._pendingPromise = null
    this._pendingResolve = null
    this._sceneFrame = 0
    this._presentedFilter = 'none'
    this._destroyed = false
    this._transitionElement = null
    this._transitionIdentity = null
    this._transitionHidden = new Map()
    this._frameWaits = new Set()
    this._presenter = {
      get frame() { return this.renderer._sceneFrame },
      get content() { return this.renderer._presentedContent },
      get textures() { return this.renderer._presentedTextures },
      get urls() { return this.renderer._presentedUrls },
      resolveTextureUrl:id=>this._resolveTextureUrl(id),
      renderer:this,
      capture:()=>this._captureScene(),
      nextFrame:()=>this._nextSceneFrame(),
      clearOverlay:()=>this._clearTransition(),
    }
    this.core.attachScenePresenter?.(this._presenter)
    this._subscribe()
  }

  _nextSceneFrame() {
    if (this._destroyed) return Promise.reject(new Error('Scene renderer is closed'))
    if(typeof requestAnimationFrame!=='function') return Promise.reject(new Error('Animation frame presenter is unavailable'))
    return new Promise((resolve,reject)=>{
      const wait={frame:null,timer:null,reject}
      const finish=()=>{clearTimeout(wait.timer);this._frameWaits.delete(wait)}
      wait.timer=setTimeout(()=>{
        if(wait.frame!==null) cancelAnimationFrame(wait.frame)
        finish();reject(new Error('Transition presentation deadline exceeded'))
      },2000)
      wait.frame=requestAnimationFrame(()=>{
        if(this._destroyed){finish();reject(new Error('Scene renderer is closed'));return}
        // Canonical JS layer nodes already contain the Lua changes. Do not
        // reenter Wasmoon while it is suspended on this presentation promise.
        try {this._renderWithList(this.core.renderList());finish();resolve(this._sceneFrame)}
        catch(error){finish();reject(error)}
      })
      this._frameWaits.add(wait)
    })
  }

  _resolveTextureUrl(id) {
    return id && (!this.core.textures || this.core.textures.get(id))
      ? this.textureUrls.get(id) : null
  }

  _copySceneNode(source, budget = {pixels:0}) {
    const copy=source.cloneNode(true)
    if(this._transitionHidden.has(source)) copy.style.visibility=this._transitionHidden.get(source)
    const originals=[source,...source.querySelectorAll('canvas')].filter(n=>n.tagName==='CANVAS')
    const copies=[copy,...copy.querySelectorAll('canvas')].filter(n=>n.tagName==='CANVAS')
    for(let i=0;i<originals.length;++i){
      const a=originals[i],b=copies[i]
      budget.pixels+=a.width*a.height
      if(!Number.isSafeInteger(budget.pixels)||budget.pixels>16*1024*1024) throw new Error('Scene snapshot pixel budget exceeded')
      b.width=a.width;b.height=a.height
      const input=a.getContext('2d'),output=b.getContext('2d')
      if(!input||!output) throw new Error('Scene canvas capture is unavailable')
      if(a.width&&a.height) output.putImageData(input.getImageData(0,0,a.width,a.height),0,0)
    }
    return copy
  }

  _captureScene() {
    if(!this._sceneFrame || this._destroyed) return null
    const node=document.createElement('div')
    Object.assign(node.style,{position:'absolute',inset:'0',width:'100%',height:'100%',pointerEvents:'none',overflow:'hidden'})
    const owned=new Set([...this._els.values(),this._textEl].filter(Boolean))
    const budget={pixels:0}
    let count=0
    for(const child of this.root.children){
      if(!owned.has(child)) continue
      count+=1+child.querySelectorAll('*').length
      if(count>4096) throw new Error('Scene snapshot node budget exceeded')
      node.appendChild(this._copySceneNode(child,budget))
    }
    node.style.filter=this._presentedFilter
    node.style.backgroundColor=getComputedStyle(this.root).backgroundColor
    const frame=this._sceneFrame
    return {frame,node,dispose(){for(const c of node.querySelectorAll('canvas')){c.width=0;c.height=0}node.replaceChildren()}}
  }

  _clearTransition() {
    for(const canvas of this._transitionElement?.querySelectorAll('canvas')??[]){canvas.width=0;canvas.height=0}
    this._transitionElement?.remove()
    this._transitionElement=null
    this._transitionIdentity=null
    for(const [element,visibility] of this._transitionHidden)element.style.visibility=visibility
    this._transitionHidden.clear()
    setStyle(this.root,'filter',paletteFilter(this.core.palette))
  }

  _renderTransition() {
    const state=this.core.transitionOverlay
    if(!state){if(this._transitionElement)this._clearTransition();return}
    const a=this.core.sceneSnapshots?.get(state.from),b=this.core.sceneSnapshots?.get(state.to)
    if(!a||!b) throw new Error('Transition snapshot is no longer owned')
    const identity=`${state.from}:${state.to}:${state.method}:${state.rule}`
    if(identity!==this._transitionIdentity){
      this._clearTransition()
      const overlay=document.createElement('div')
      overlay.className='caesura-transition';overlay.setAttribute('aria-hidden','true')
      Object.assign(overlay.style,{position:'absolute',inset:'0',zIndex:'2147483647',pointerEvents:'none',overflow:'hidden'})
      overlay.style.isolation='isolate'
      const from=this._copySceneNode(a.node),to=this._copySceneNode(b.node)
      from.classList.add('caesura-transition-from');to.classList.add('caesura-transition-to')
      overlay.append(from,to);this.root.appendChild(overlay)
      this._transitionElement=overlay;this._transitionIdentity=identity
    }
    // Filter is part of each captured scene. Applying today's stage grade to
    // both old and new snapshots would double-grade or relabel the old scene.
    setStyle(this.root,'filter','none')
    for(const element of [...this._els.values(),this._textEl].filter(Boolean)){
      if(!this._transitionHidden.has(element))this._transitionHidden.set(element,element.style.visibility)
      setStyle(element,'visibility','hidden')
    }
    const from=this._transitionElement.querySelector('.caesura-transition-from')
    const to=this._transitionElement.querySelector('.caesura-transition-to')
    const percent=state.progress*100
    setStyle(to,'opacity',state.method===0?String(state.progress):'1')
    setStyle(from,'opacity',state.method===0?String(1-state.progress):'1')
    setStyle(from,'mixBlendMode',state.method===0?'plus-lighter':'normal')
    setStyle(to,'mixBlendMode',state.method===0?'plus-lighter':'normal')
    let clip='none'
    if(state.method===2)clip=`inset(0 ${100-percent}% 0 0)`
    if(state.method===3)clip=`inset(0 0 0 ${100-percent}%)`
    if(state.method===4)clip=`inset(${100-percent}% 0 0 0)`
    if(state.method===5)clip=`inset(0 0 ${100-percent}% 0)`
    setStyle(to,'clipPath',clip)
    if(state.method===1){
      const texture=this.core.textures.get(state.rule)?.prepared
      if(!texture)throw new Error('Rule texture is unavailable')
      const mask=document.createElement('canvas');mask.width=texture.width;mask.height=texture.height
      const ctx=mask.getContext('2d');if(!ctx)throw new Error('Rule mask canvas unavailable')
      texture.draw(ctx)
      const image=ctx.getImageData(0,0,mask.width,mask.height)
      for(let i=0;i<image.data.length;i+=4)image.data[i+3]=image.data[i]/255<=state.progress?255:0
      ctx.putImageData(image,0,0)
      const url=ctx.canvas.toDataURL('image/png')
      setStyle(to,'maskImage',`url("${url}")`);setStyle(to,'maskSize','100% 100%');setStyle(to,'maskMode','alpha')
    }
  }

  _subscribe() {
    const render = () => this.render()
    // The core is driven synchronously by Lua; we re-render on every
    // call via a trailing microtask (batches multi-step commands).
    this._flush = render
  }

  /** Set the URL for a texture id (img src resolution). */
  setTextureUrl(id, url) { this.textureUrls.set(id, url) }

  /** Sync DOM to core state.
   *
   *  Two shapes, picked by whether a layer source can suspend:
   *   - no getLayers (core.renderList, plain JS): the whole pass is
   *     SYNCHRONOUS. It cannot overlap with anything, so it is not queued —
   *     callers observe the finished DOM immediately after render() returns,
   *     which is the long-standing contract several suites rely on
   *     (adapter.test.js calls render() without awaiting and asserts at once).
   *   - getLayers set (the wasmoon hop): serialized. An in-flight pass absorbs
   *     further calls into ONE trailing pass; the returned promise resolves
   *     after a pass that started at or after this call, so an awaiting caller
   *     always observes the state it asked for.
   *  Always returns a promise, so `await renderer.render()` is valid on both. */
  render() {
    if (!this.getLayers) {
      this._renderWithList(this.core.renderList())
      return Promise.resolve()
    }
    if (this._inFlight) {
      // A pass is running: ask for one more and hand every waiter the same
      // promise (one trailing pass, no matter how many frames pile up).
      this._pending = true
      if (!this._pendingPromise) {
        this._pendingPromise = new Promise((resolve) => { this._pendingResolve = resolve })
      }
      return this._pendingPromise
    }
    this._inFlight = this._runPasses()
    return this._inFlight
  }

  /** Drive passes until no further render was requested while one ran.
   *  Each iteration takes ownership of the waiters that queued BEFORE it
   *  started, so a request arriving mid-pass gets a fresh promise and its own
   *  later pass — an awaiting caller is never resolved by a pass that began
   *  before its request. */
  async _runPasses() {
    try {
      this._renderWithList(await this.getLayers())
      while (this._pending) {
        this._pending = false
        const resolve = this._pendingResolve
        this._pendingPromise = null
        this._pendingResolve = null
        this._renderWithList(await this.getLayers())
        if (resolve) resolve()
      }
    } finally {
      this._inFlight = null
    }
  }

  /** One full pass over an already-resolved layer list: synchronous, so the
   *  DOM is never observed half-updated (the only await in the render path is
   *  the layer fetch above). */
  _renderWithList(list) {
    const presentedUrls=[]
    const alive = new Set()
    let messageZ = '0'
    // Web-side color grading: the active LUT (backend.set_palette ->
    // core.palette) tints the whole render output via a CSS filter, the
    // DOM analog of the desktop s_lutTex/u_paletteParams binding. day/
    // neutral (handle null) applies nothing; night applies a blue-dark
    // grade scaled by intensity.
    setStyle(this.root, 'filter', paletteFilter(this.core.palette))
    // text layer rendered separately (overlay) if it has content
    for (const n of list) {
      const identity = n.id ?? n.name
      let el = this._els.get(identity)
      const texture = this.core.textures?.get(n.texture)
      const prepared = texture?.prepared
      const isImage = IMAGE_LAYER_TAGS.has(n.name) || (n.tag && IMAGE_LAYER_TAGS.has(n.tag)) ||
        n.name.startsWith('_char_') || (n.tag && n.tag.startsWith('_char_'))
      const tagName = prepared ? 'CANVAS' : isImage ? 'IMG' : 'DIV'
      if (!el || el.tagName !== tagName) {
        if (el) el.remove()
        el = document.createElement(tagName.toLowerCase())
        el.className = 'caesura-layer'
        el.dataset.layer = n.name
        el.style.position = 'absolute'
        this.root.appendChild(el)
        this._els.set(identity, el)
      }
      // CSS transitions animate engine-driven moves/fades (sprite_move /
      // sprite_fade yield per frame; the DOM sees the endpoint).
      //
      // t12: every write below goes through setStyle, which compares against
      // the element's current inline value first. A steady scene therefore
      // costs ZERO style writes per frame instead of 7 per layer, and the
      // renderer still repairs any value changed behind its back (the
      // comparison reads the DOM, not a cache).
      setStyle(el, 'transition', 'left 300ms linear, top 300ms linear, opacity 300ms linear')
      setStyle(el, 'left', n.x + 'px')
      setStyle(el, 'top', n.y + 'px')
      setStyle(el, 'width', n.w + 'px')
      setStyle(el, 'height', n.h + 'px')
      // engine opacity is 0..255; DOM wants 0..1
      setStyle(el, 'opacity', String(Number(n.opacity ?? 255) / 255))
      setStyle(el, 'zIndex', String(n.z))
      // Keep the accepted CSS value, including its integer representation.
      if (Number(el.style.zIndex) > Number(messageZ)) messageZ = el.style.zIndex
      const url = this._resolveTextureUrl(n.texture)
      presentedUrls.push([n.texture,url])
      if (prepared) {
        if (this._drawnPrepared.get(el) !== prepared) {
          el.width = prepared.width
          el.height = prepared.height
          const context = el.getContext('2d')
          if (!context) throw new Error('Prepared image canvas unavailable')
          prepared.draw(context)
          this._drawnPrepared.set(el, prepared)
        }
      } else if (el.tagName === 'IMG') {
        // Re-setting src to the SAME URL makes the browser re-run its image
        // load path (cache revalidation, and a decode on some engines), so
        // compare with the attribute actually on the node. getAttribute
        // returns the literal string that was set (el.src would return an
        // absolutized URL and never match a relative one).
        if (url) {
          if (el.getAttribute('src') !== url) el.setAttribute('src', url)
        } else if (el.hasAttribute('src')) {
          el.removeAttribute('src')
        }
      } else {
        el.textContent = url ? '' : ''
      }
      alive.add(identity)
    }
    // remove stale layer elements
    for (const [name, el] of this._els) {
      if (!alive.has(name) && name !== '_message') {
        el.remove()
        this._els.delete(name)
      }
    }
    // message overlay — structured draws (x/y/rgb/scale) when available,
    // else the flat textBuffer (legacy fallback).
    const draws = this.core.draws ?? []
    const hasText = this.core.font?.active !== false && (draws.length > 0 || this.core.textBuffer.length > 0)
    if (hasText && !this._textEl) {
      this._textEl = document.createElement('div')
      this._textEl.className = 'caesura-message'
      this._textEl.style.position = 'absolute'
      this._textEl.style.left = '0'
      this._textEl.style.top = '0'
      this._textEl.style.width = '100%'
      this._textEl.style.height = '100%'
      this._textEl.style.pointerEvents = 'none'
      this.root.appendChild(this._textEl)
    }
    if (this._textEl) {
      // Text is drawn after ordinary graphics, even when restoring a texture
      // replaces an existing IMG with a newly appended CANVAS. Equal highest
      // z plus last sibling also avoids overflowing the CSS integer limit.
      setStyle(this._textEl, 'zIndex', messageZ)
      if (this.root.lastElementChild !== this._textEl) this.root.appendChild(this._textEl)
      this._textEl.textContent = ''
      if (draws.length > 0) {
        for (const d of draws) {
          const span = document.createElement('span')
          span.textContent = d.t
          span.style.position = 'absolute'
          span.style.left = d.x + 'px'
          span.style.top = d.y + 'px'
          span.style.color = 'rgb(' + d.r + ',' + d.g + ',' + d.b + ')'
          span.style.fontSize = Math.round((this.core.font?.size ?? 20) * (d.s || 1)) + 'px'
          if (this.core.font?.family) span.style.fontFamily = this.core.font.family
          if (d.bd) span.style.fontWeight = '700'
          if (d.it) span.style.fontStyle = 'italic'
          if (d.st) span.style.textDecoration = 'line-through'
          if (d.ruby) {
            const ruby = document.createElement('ruby')
            ruby.textContent = d.t
            const annotation = document.createElement('rt')
            annotation.textContent = d.ruby
            ruby.appendChild(annotation)
            span.replaceChildren(ruby)
          }
          this._textEl.appendChild(span)
        }
      } else {
        // flat fallback: bottom text box
        const box = document.createElement('div')
        box.textContent = this.core.textBuffer
        box.style.position = 'absolute'
        box.style.left = '0'
        box.style.right = '0'
        box.style.bottom = '0'
        box.style.padding = '16px 24px'
        box.style.background = 'rgba(0,0,0,0.55)'
        box.style.color = '#fff'
        // inherit the player font stack (@font-face CaesuraNoto + fallbacks);
        // a hard-coded system-ui here would override the W3 CJK font.
        box.style.fontFamily = ''
        box.style.fontSize = '20px'
        box.style.whiteSpace = 'pre-wrap'
        this._textEl.appendChild(box)
      }
    }
    if (!hasText && this._textEl) { this._textEl.remove(); this._textEl = null }
    this._renderTransition()
    // This ID records an actual completed DOM scene pass, not a Lua tick.
    this._presentedFilter=paletteFilter(this.core.palette)
    // Bind completion to the model actually consumed in this synchronous pass,
    // before Promise continuations can mutate the core again.
    // Lightweight render models remain supported. Without a content witness
    // they render normally but cannot certify a reusable transition receipt.
    this._presentedContent=typeof this.core._presentationContent==='function'
      ?this.core._presentationContent(list):null
    this._presentedTextures=this.core.textures
      ?[...this.core.textures].map(([id,texture])=>[id,texture,texture.path,texture.prepared]):[]
    this._presentedUrls=presentedUrls
    ++this._sceneFrame
    if(!Number.isSafeInteger(this._sceneFrame))throw new Error('Scene frame identity exhausted')
  }

  destroy() {
    this._destroyed=true
    for(const wait of this._frameWaits){clearTimeout(wait.timer);cancelAnimationFrame(wait.frame);wait.reject(new Error('Scene renderer closed'))}
    this._frameWaits.clear()
    if(this.core._scenePresenter===this._presenter){this.core.clearSceneSnapshots();this.core._scenePresenter=null}
    for (const el of this._els.values()) el.remove()
    this._els.clear()
    if (this._textEl) { this._textEl.remove(); this._textEl = null }
  }
}
