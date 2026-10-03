// G5 path-B web player — binding adapter CORE (pure state machine, no DOM).
// backend.* / layers.* semantics as observed by the Lua command handlers
// (spike4 contract): add_layer returns a node table, Type is a constant
// table, get returns null when absent, load_texture returns an id.

export class AdapterCore {
  constructor() {
    /** stable layer id -> canonical node (ordinary ids default to the name) */
    this.layers = new Map()
    this.textures = new Map() // id -> {path, loaded, width, height}
    this.textBuffer = ''
    this.draws = []
    this.backlog = []
    this._lastBacklog = ''
    this.endings = []
    this._endingKeys = new Set()
    this.audioBus = { bgm: null, se: [], voice: null }
    this.events = [] // call log for tests/telemetry
    this._seq = 0
    this._layerInstances = new WeakSet()
    this._childArrays = new WeakSet()
    this._root = this._makeLayer('_root', '_root', { w: 0, h: 0, z: -9999, layer_type: 1 })
    /** Active color-grading LUT state (round 77 web palette bridge).
     *  Mirrors the desktop s_lutTex/u_paletteParams binding: handle is the
     *  registered LUT texture id (nil = no LUT), intensity 0..1, size 16/64. */
    this.palette = { handle: null, intensity: 0, size: 0 }
    this.sceneSnapshots = new Map()
    this.transitionOverlay = null
    this._scenePresenter = null
    this._transitionRevision = 0
    this._transitionPresentation = null
  }

  // Owned copies of actually presented DOM frames. A newer model is never
  // substituted for the renderer's last presentation at capture time.
  attachScenePresenter(presenter) {
    if (this._scenePresenter && this._scenePresenter !== presenter) this.clearSceneSnapshots()
    this._scenePresenter = presenter
  }

  _presentationContent(list = this.renderList()) {
    return JSON.stringify([list,this.draws,this.textBuffer,this.palette,this.font??null])
  }

  async presentScene(completedTransition = null) {
    const presenter = this._scenePresenter
    if (!presenter) throw new Error('Scene presentation is unavailable')
    const before = presenter.frame
    if (!Number.isSafeInteger(before) || before < 0) throw new Error('Scene presentation frame is invalid')
    const receipt = this._transitionPresentation
    this._transitionPresentation = null // one completed frame can satisfy one wait
    if (completedTransition && receipt && receipt.presenter === presenter
      && receipt.revision === this._transitionRevision && receipt.overlay === this.transitionOverlay
      && receipt.frame === before && receipt.frame >= completedTransition.epoch
      && receipt.overlay.from === completedTransition.from && receipt.overlay.to === completedTransition.to
      && receipt.content === this._presentationContent()
      && receipt.textures.every(([id,texture,path,prepared])=>this.textures.get(id)===texture
        && texture.path===path && texture.prepared===prepared)
      && typeof presenter.resolveTextureUrl === 'function'
      && receipt.urls.every(([id,url])=>presenter.resolveTextureUrl(id)===url)) {
      this._log('scene.presented', {frame:receipt.frame})
      return receipt.frame
    }
    const frame = await presenter.nextFrame()
    if (this._scenePresenter !== presenter || !Number.isSafeInteger(frame)
      || frame <= before || presenter.frame < frame) {
      throw new Error('Scene presentation owner or frame changed')
    }
    this._log('scene.presented', {frame})
    return frame
  }

  async captureScene(allowInitialPresentation = false) {
    const presenter = this._scenePresenter
    if (!presenter) return '0:0'
    if (!presenter.frame && allowInitialPresentation) await presenter.nextFrame()
    // A replacement may attach while the old renderer's genuine rAF is pending.
    // Its completed frame still belongs to the retired presenter, not this core.
    if (this._scenePresenter !== presenter || !presenter.frame) return '0:0'
    if (this.sceneSnapshots.size >= 16) throw new Error('Transition snapshot budget exceeded')
    const snapshot = presenter.capture()
    if (this._scenePresenter !== presenter) { snapshot?.dispose(); return '0:0' }
    if (!snapshot || !Number.isSafeInteger(snapshot.frame) || snapshot.frame <= 0) return '0:0'
    const id = ++this._seq
    if (!Number.isSafeInteger(id)) { snapshot.dispose(); throw new Error('Snapshot identity exhausted') }
    this.sceneSnapshots.set(id, snapshot)
    this._log('transition.capture', {id, frame:snapshot.frame})
    return `${id}:${snapshot.frame}`
  }

  destroySceneSnapshot(id) {
    const snapshot = this.sceneSnapshots.get(id)
    if (!snapshot) return false
    if (this.transitionOverlay?.from === id || this.transitionOverlay?.to === id) this.cancelTransition()
    this.sceneSnapshots.delete(id)
    snapshot.dispose()
    this._log('transition.release', {id})
    return true
  }

  async submitTransition(view, from, to, rule, method, progress) {
    const presenter = this._scenePresenter
    if (!presenter || !Number.isSafeInteger(view) || view < 0
      || !this.sceneSnapshots.has(from) || !this.sceneSnapshots.has(to)
      || !Number.isInteger(method) || method < 0 || method > 5
      || !Number.isFinite(progress) || progress < 0 || progress > 1
      || (method === 1 && !this.textures.get(rule)?.prepared)) return false
    const revision = this._transitionRevision
    const overlay = {view,from,to,rule,method,progress}
    this._transitionPresentation = null
    this.transitionOverlay = overlay
    this._log('transition.submit', {from,to,method,progress})
    // A real render/rAF separates Lua ticks. Ordinary synchronous pump yields
    // alone cannot present a frame and must not manufacture a frame counter.
    const before = presenter.frame
    const frame = await presenter.nextFrame()
    if (this._scenePresenter !== presenter || revision !== this._transitionRevision
      || this.transitionOverlay !== overlay || !Number.isSafeInteger(frame)
      || frame <= before || presenter.frame !== frame) return false
    // This is the actual completed DOM/rAF frame, not a synthetic epoch. The
    // runner may acknowledge it after tick() returns, provided no scene changed.
    if (typeof presenter.content === 'string' && Array.isArray(presenter.textures)
      && Array.isArray(presenter.urls) && typeof presenter.resolveTextureUrl === 'function') {
      this._transitionPresentation = {presenter,revision,overlay,frame,
        content:presenter.content,textures:presenter.textures,urls:presenter.urls}
    }
    return true
  }

  cancelTransition() {
    ++this._transitionRevision
    this._transitionPresentation = null
    this.transitionOverlay = null
    this._scenePresenter?.clearOverlay()
    return true
  }

  clearSceneSnapshots() {
    this.cancelTransition()
    for (const id of [...this.sceneSnapshots.keys()]) this.destroySceneSnapshot(id)
  }

  // -- layers -----------------------------------------------------------
  // Mirrors the engine's Layers.ensure(ctx, name, z): creating an
  // existing layer is a no-op except for an explicit z/tag update (the
  // engine overwrites z on a found node). tag defaults to the layer name
  // (so Layers.find(name) tag-matching and the DOM tag heuristics work).
  ensureLayer(name, opts = {}) {
    this.getRoot()
    let n = this.getLayer(name)
    if (!n) {
      const id = opts.id ?? String(name)
      if (typeof id !== 'string' || !id || id === '_root' || this.layers.has(id)) {
        throw new Error('Invalid or duplicate layer identity')
      }
      n = this._makeLayer(id, String(name), opts)
      this.layers.set(id, n)
      this._attachLayer(n, this._root)
      ++this._seq
      this._log('layer.create', { name, z: n.z })
    } else {
      if (opts.z !== undefined) this.setLayerProperty(n, 'z', opts.z)
      if (opts.tag !== undefined) this.setLayerProperty(n, 'tag', opts.tag)
    }
    return n
  }

  _makeLayer(id, name, opts = {}) {
    const node = {
      id, name, parent: null, children: [],
      x: opts.x ?? 0, y: opts.y ?? 0, w: opts.w ?? 1280, h: opts.h ?? 720,
      visible: opts.visible ?? true, opacity: opts.opacity ?? 255, z: opts.z ?? 0,
      texture: opts.texture ?? opts.tex ?? null, tag: opts.tag ?? null,
      layerType: opts.layer_type ?? opts.layerType ?? 0,
      scaleX: 1, scaleY: 1, rotation: 0, originX: 0, originY: 0,
      alpha: (opts.opacity ?? 255) / 255, blend_mode: opts.blend_mode ?? 'alpha',
      dirty: true, quake: {}, shake: {}, fade: {},
    }
    for (const key of ['scale', 'scaleX', 'scaleY', 'rotation', 'originX', 'originY', 'alpha',
      'pos_x', 'pos_y', 'clipX', 'clipY', 'clipW', 'clipH', 'imgX', 'imgY', 'imgW', 'imgH']) {
      if (opts[key] !== undefined) node[key] = opts[key]
    }
    this._layerInstances.add(node)
    this._childArrays.add(node.children)
    return node
  }

  getRoot() {
    // A few existing hosts clear the public Map directly between scenes.
    if (this.layers.size === 0 && this._root.children.length > 0) {
      this._root = this._makeLayer('_root', '_root', { w: 0, h: 0, z: -9999, layer_type: 1 })
    }
    return this._root
  }

  getLayer(name) {
    if (name == null) return null
    if (name === '_root') return this.getRoot()
    return this.layers.get(name) ?? [...this.layers.values()].find(node => node.name === name) ?? null
  }

  addLayer(parent, opts = {}) {
    this.getRoot()
    const target = parent == null ? this._root : this._canon(parent)
    if (!target) throw new Error('Layer parent is no longer active')
    let name = opts.name ?? opts.id
    if (name == null) {
      let candidate = this._seq + 1
      do { name = 'layer' + candidate++ } while (this.getLayer(name))
    }
    const id = opts.id ?? String(name)
    if (typeof id !== 'string' || !id || id === '_root' || this.layers.has(id)) {
      throw new Error('Invalid or duplicate layer identity')
    }
    const node = this._makeLayer(id, String(name), opts)
    this.layers.set(id, node)
    this._attachLayer(node, target)
    ++this._seq
    this._log('layer.create', { name: node.name, z: node.z })
    return node
  }

  _attachLayer(node, parent) {
    node.parent = parent
    parent.children.push(node)
    parent.children.sort((a, b) => a.z - b.z)
  }

  reparentLayer(node, parent) {
    const current = this._canon(node)
    const target = parent == null ? this.getRoot() : this._canon(parent)
    if (!current) return false
    if (!target || current === this._root) throw new Error('Invalid layer parent')
    for (let ancestor = target; ancestor; ancestor = ancestor.parent) {
      if (ancestor === current) throw new Error('Cyclic layer parent')
    }
    if (current.parent === target) return true
    const siblings = current.parent?.children
    if (siblings) siblings.splice(siblings.indexOf(current), 1)
    this._attachLayer(current, target)
    return true
  }

  clearLayers() {
    this.layers.clear()
    this._root = this._makeLayer('_root', '_root', { w: 0, h: 0, z: -9999, layer_type: 1 })
    this._log('layer.clear', {})
    return true
  }

  removeLayer(name) {
    const node = typeof name === 'object' ? this._canon(name) : this.getLayer(name)
    if (!node || node === this._root) return false
    for (const child of [...node.children]) this.removeLayer(child)
    const siblings = node.parent?.children
    if (siblings) siblings.splice(siblings.indexOf(node), 1)
    this.layers.delete(node.id)
    this._log('layer.remove', { name: node.name })
    return true
  }

  /** Resolve the canonical node: Lua-side proxies may wrap the same layer
   *  under a different JS reference, so mutators always write the node
   *  stored in the Map (keyed by name). */
  _canon(node) {
    if (!node) return null
    if (node === this._root) return node
    if (this._layerInstances.has(node)) return this.layers.get(node.id) === node ? node : null
    return this.getLayer(node.name ?? node.id)
  }

  isLayerNode(value) { return this._layerInstances.has(value) }
  isLayerChildren(value) { return this._childArrays.has(value) }

  setLayerProperty(node, field, value) {
    const n = this._canon(node)
    if (!n) return false
    if (field === 'id') {
      if (value !== n.id) throw new Error('Layer identity is immutable')
      return true
    }
    if (field === 'children') throw new Error('Use layer APIs to change children')
    if (field === 'parent') return this.reparentLayer(n, value)
    const key = field === 'tex' ? 'texture' : field === 'layer_type' ? 'layerType' : field
    if (key === '__proto__' || key === 'constructor' || key === 'prototype') throw new Error('Invalid layer field')
    n[key] = value
    if (key === 'z') n.parent?.children.sort((a, b) => a.z - b.z)
    if (key === 'opacity') n.alpha = value / 255
    n._lastMutateAt = this._frame ?? 0
    return true
  }

  installPreparedLayers(records) {
    if (!Array.isArray(records) || records.length === 0) throw new Error('Expected prepared layer records')
    const next = new Map()
    let root = null
    for (const [index, record] of records.entries()) {
      if (!record || typeof record.id !== 'string' || !record.id || next.has(record.id)) {
        throw new Error('Invalid or duplicate prepared layer identity')
      }
      if ((index === 0 && (record.id !== '_root' || record.parent != null))
          || (index > 0 && (record.id === '_root' || !next.has(record.parent)))) {
        throw new Error('Invalid prepared layer parent/order')
      }
      const texture = record.image ? record.image.id : null
      if (texture != null && (!Number.isInteger(texture) || texture <= 0)) throw new Error('Invalid materialized texture ID')
      const node = this._makeLayer(record.id, record.name ?? record.id, {
        ...record, texture, w: record.w ?? 0, h: record.h ?? 0,
      })
      if (index === 0) root = node
      else this._attachLayer(node, next.get(record.parent))
      next.set(record.id, node)
    }
    next.delete('_root')
    this.layers = next
    this._root = root
    this._log('layer.install', { nodes: records.length })
    return true
  }

  setLayerImage(node, texId) {
    const n = this._canon(node)
    if (!n) return
    this.setLayerProperty(n, 'texture', texId ?? null)
    this._log('layer.image', { name: n.name, tex: texId })
  }

  setLayerVisible(node, v) { const n = this._canon(node); if (n) { this.setLayerProperty(n, 'visible', !!v); this._log('layer.visible', { name: n.name, v: !!v }) } }
  setLayerOpacity(node, v) { const n = this._canon(node); if (n) { this.setLayerProperty(n, 'opacity', Number(v)); this._log('layer.opacity', { name: n.name, v: n.opacity }) } }
  setLayerZ(node, z) { const n = this._canon(node); if (n) { this.setLayerProperty(n, 'z', Number(z)); this._log('layer.z', { name: n.name, z: n.z }) } }
  moveLayer(node, x, y) {
    const n = this._canon(node)
    if (!n) return
    if (x != null) this.setLayerProperty(n, 'x', Number(x))
    if (y != null) this.setLayerProperty(n, 'y', Number(y))
    this._log('layer.move', { name: n.name, x: n.x, y: n.y })
  }

  // -- textures ---------------------------------------------------------
  loadTexture(path) {
    if (!path) return 0
    let id = 0
    for (const [k, v] of this.textures) if (!v.prepared && v.path === path) { id = k; break }
    if (!id) { id = ++this._seq; this.textures.set(id, { path, loaded: false, width: 0, height: 0 }) }
    this._log('texture.load', { path, id })
    return id
  }
  markTextureLoaded(id, width, height) {
    const t = this.textures.get(id)
    if (t) { t.loaded = true; t.width = width ?? 0; t.height = height ?? 0; this._log('texture.ready', { id, w: t.width, h: t.height }) }
  }
  /** Adopt an already decoded image without adding it to the path cache. */
  registerPreparedTexture(source, prepared) {
    const id = this._seq + 1
    const texture = {
      path: source.kind === 'asset' ? source.path : '', source: { ...source }, prepared,
      loaded: true, width: prepared.width, height: prepared.height,
    }
    this.textures.set(id, texture)
    try { this._log('texture.restore', { id, width: texture.width, height: texture.height }) }
    catch (error) { this.textures.delete(id); throw error }
    this._seq = id
    return id
  }
  destroyTexture(id) {
    const texture = this.textures.get(id)
    this.textures.delete(id)
    texture?.prepared?.dispose()
    this._log('texture.destroy', { id })
  }

  // -- palette / LUT (round 77 web bridge) -----------------------------
  /** Apply the active color-grading LUT. handle is a texture id registered
   *  via load_image/loadTexture (null clears -> neutral). intensity 0..1,
   *  size 16 (16^3) or 64 (64^3). The DOM renderer consumes core.palette to
   *  tint the render output (a real web-side analog of the desktop LUT). */
  setPalette(handle, intensity, size) {
    const h = handle == null ? null : Number(handle)
    this.palette = {
      handle: h && this.textures.has(h) ? h : null,
      intensity: Number(intensity) || 0,
      size: Number(size) || 0,
    }
    this._log('palette.set', { handle: this.palette.handle, intensity: this.palette.intensity, size: this.palette.size })
  }

  // -- text -------------------------------------------------------------
  clearText() { this.textBuffer = ''; this.draws = []; this._log('text.clear') }
  appendText(s) { if (s) { this.textBuffer += s; this._log('text.append', { len: s.length }) } }
  setText(s) { this.textBuffer = String(s ?? ''); this._log('text.set', { len: this.textBuffer.length }) }
  /** Structured text draws from the Lua TextScene (x/y/rgb/scale/bold). */
  setDraws(draws) {
    this.draws = Array.isArray(draws) ? draws : []
    this._log('text.draws', { n: this.draws.length })
  }

  /** Commit a historical page WITHOUT changing the current view (VN
   *  backlog: clicking pushes the visible page into history; the display
   *  stays until the next [p] clears it). */
  pushBacklog(draws) {
    const list = Array.isArray(draws) ? draws : []
    const text = list.map((d) => d.t).join('')
    if (text.trim().length === 0 || text === this._lastBacklog) return
    this.backlog.push({ draws: list.map((d) => ({ ...d })), text })
    this._lastBacklog = text
    this._log('backlog.add', { n: this.backlog.length })
  }

  /** Record endings unlocked by [ending] during a run (engine writes
   *  ctx.seen_endings; bridge exports them per-run). Dedup by id. */
  recordEndings(list) {
    const arr = Array.isArray(list) ? list : []
    for (const e of arr) {
      if (!e || typeof e.id !== 'string' || this._endingKeys.has(e.id)) continue
      this._endingKeys.add(e.id)
      this.endings.push({ id: e.id, name: String(e.name ?? '') })
      this._log('ending.unlock', { id: e.id, name: String(e.name ?? '') })
    }
    return this.endings.length
  }

  // -- audio ------------------------------------------------------------
  audioPlay(kind, path, volume = 1) {
    this.audioBus[kind] = { path, volume: Number(volume) ?? 1, playing: true }
    this._log('audio.play', { kind, path, volume })
  }
  audioStop(kind) { if (this.audioBus[kind]) this.audioBus[kind].playing = false; this._log('audio.stop', { kind }) }
  audioSetBusVolume(kind, v) { this._log('audio.volume', { kind, v: Number(v) ?? 1 }) }
  /** Host callback: a clip actually ended (AudioContext onended). */
  audioEnded(kind) { if (this.audioBus[kind]) this.audioBus[kind].playing = false; this._log('audio.ended', { kind }) }
  audioIsPlaying(kind) {
    const e = this.audioBus[kind]
    if (!e || !e.playing) return false
    // Web player: a clip is treated as finished after its simulated
    // duration (real impl uses AudioContext currentTime).
    const elapsed = (this._frame ?? 0) - (e._startFrame ?? 0)
    return elapsed < (e._durationFrames ?? 120)
  }

  // -- z-sorted render list (canvas/DOM both consume this) --------------
  renderList() {
    const result = []
    const visit = (node, parentX, parentY) => {
      if (!this._canon(node) || !node.visible) return
      const x = parentX + (node.pos_x ?? node.x ?? 0)
      const y = parentY + (node.pos_y ?? node.y ?? 0)
      for (const child of node.children) visit(child, x, y)
      if (node.texture) {
        const { parent, children, ...fields } = node
        result.push({ ...fields, x, y })
      }
    }
    visit(this.getRoot(), 0, 0)
    return result.sort((a, b) => a.z - b.z)
  }

  _log(kind, detail) { this.events.push({ kind, detail, t: this.events.length }) }
  /** Host frame tick (browser rAF / test driver). */
  tick() { this._frame = (this._frame ?? 0) + 1 }
}

// Lua-facing constants mirroring the C++ layer type enum (subset).
export const LAYER_TYPE = { LAYER_NORMAL: 0, LAYER_MESSAGE: 1, LAYER_BG: 2, LAYER_FG: 3 }
