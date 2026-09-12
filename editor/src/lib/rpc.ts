// Caesura Editor — engine HTTP RPC client.
// Mirrors the 36 routes of src/rpc/EditorServer.cpp — 34 /api/* + 2 static (docs/api/editor-api-reference.md).
// All calls go through /api/* (Vite dev proxy → 127.0.0.1:9876, or same-origin
// in production). Every request carries the optional bearer token from the
// engine's CAESURA_EDITOR_TOKEN env var (set via the connection panel).

export interface PingReply {
  status: string
  engine: string
}

export interface StatusReply {
  status: string
  engine: string
  lua: boolean
  port: number
}

export interface AssetEntry {
  path: string
  name: string
  /** Coarse category: "image" | "audio" | "script". */
  type: string
  /** Per-directory slot: "bg" | "fg" | "char" | "ui" | "bgm" | "voice" | "se" | "scripts". */
  kind?: string
}

export interface LogEntry {
  level: 'info' | 'warn' | 'error'
  message: string
  time: string
}

export interface DebugStateReply {
  status: string
  scene?: string
  paused?: boolean
  token_index?: number
  /** Current execution element, e.g. "[ch]", "*start", "text" (round 28). */
  current_cmd?: string
  [key: string]: unknown
}

export interface BreakpointSpec {
  source: string
  line: number
}

export interface FrameReply {
  status: string
  width?: number
  height?: number
  png?: string // base64
  error?: string
}

// Engine runtime state for the preview panel (GET /api/state, round 18).
export interface StateReply {
  status: string
  scene?: string
  token_index?: number
  nvl_mode?: boolean
  language?: string
  backlog_count?: number
  layer_count?: number
  /** Current execution element, e.g. "[ch]", "*start", "text" (round 28). */
  current_cmd?: string
  error?: string
}

// Engine stats (GET /api/stats). Render/asset/job/Lua introspection.
export interface StatsReply {
  status: string
  texture_budget_mb?: number
  texture_tier?: number
  texture_tier_name?: string
  mesh_count?: number
  job_workers?: number
  job_pending?: number
  lua_kb?: number
  error?: string
}

// Preview-frame hit test (GET /api/pick, round 23).
export interface PickReply {
  status: string
  hits: string // JSON array text: [{id,name,z,depth,opacity,x,y,w,h}]
  error?: string
}

export interface PickHit {
  id: string
  name: string
  z: number
  depth: number
  opacity: number
  x: number
  y: number
  w: number
  h: number
}

// SMA asset validation (GET /api/sma/validate, round 19).
export interface SmaValidateReply {
  status: string
  ok: boolean
  errors: string[]
  /** JSON object text: {bones, anims, parts, verts, tris} (parse client-side). */
  meta: string
  error?: string
}

export interface SmaMeta {
  bones: number
  anims: string[]
  parts: number
  verts: number
  tris: number
}
export interface SmaSaveReply {
  status: string
  ok: boolean
  errors: string[]
  error?: string
}


// Reply of POST /api/build (one-click CARC packaging, R1.3).
export interface BuildReply {
  status: string
  /** Produced archive path (as confined under build/). */
  path?: string
  /** Archive size in bytes. */
  size?: number
  /** Number of packaged files. */
  files?: number
  error?: string
}

// Reply of POST /api/package/web (one-click web-site packaging, Sprint 6).
export interface WebPackageReply {
  ok: boolean
  /** Confined output directory, e.g. "dist/example_game". */
  outputDir?: string
  /** Last lines of the packaging script output (for troubleshooting). */
  logTail?: string
  error?: string
}

export interface Live2DModel {
  path: string
  name: string
}

// A project template discoverable via GET /api/project/templates.
export interface ProjectTemplate {
  id: string
  name: string
  description: string
  /** Directory name under tools/project_templates (== id in practice). */
  dir: string
}

// A managed project under ./projects/ (GET /api/project/list).
export interface ProjectInfo {
  /** Directory path relative to the engine cwd, e.g. "projects/demo". */
  path: string
  name: string
  template: string
  /** File-system mtime as a raw integer string (opaque; for sorting). */
  modified?: string
}

// Shared reply for POST /api/project/create, /duplicate and /import.
export interface ProjectOpReply {
  ok: boolean
  path?: string
  error?: string
}

// Project metadata persisted as projects/<name>/caesura.project.json
// (GET/POST /api/project/meta, task book §6.3 PM settings). `name` mirrors
// the directory name and is not editable; created/version/modified are
// engine-owned stamps.
export interface ProjectMeta {
  name: string
  template: string
  version: string
  /** UI language hint: 'zh' | 'en' | 'ja'. */
  language: string
  description: string
  /** ISO-8601 UTC timestamps stamped by the engine. */
  created: string
  modified: string
}

// Reply of GET/POST /api/project/meta. inferred=true means no metadata
// file existed yet — every field came from engine defaults.
export interface ProjectMetaReply {
  ok: boolean
  path?: string
  inferred?: boolean
  meta?: ProjectMeta
  error?: string
}

export interface ApiError {
  status: string
  code?: string
  message?: string
  error?: string
}

export class RpcError extends Error {
  constructor(
    message: string,
    public readonly status?: number,
    public readonly body?: unknown,
  ) {
    super(message)
    this.name = 'RpcError'
  }
}

async function responseError(res: Response, path: string): Promise<RpcError> {
  // Consume once: json() also consumes malformed text, losing a later text fallback.
  let body: unknown = null
  try {
    const text = await res.text()
    try { body = JSON.parse(text) } catch { body = text }
  } catch { /* Retain the HTTP status when reading the response body fails. */ }
  const fields = body && typeof body === 'object'
    ? body as Record<string, unknown> : {}
  const code = typeof fields.code === 'string' ? fields.code : ''
  const detail = typeof fields.message === 'string' ? fields.message
    : typeof fields.error === 'string' ? fields.error
    : typeof body === 'string' ? body : ''
  const diagnostic = [code, detail].filter(Boolean).join(': ')
  return new RpcError('HTTP ' + res.status + ' on ' + path + (diagnostic ? ': ' + diagnostic : ''), res.status, body)
}

export class EngineClient {
  private token = ''

  constructor(
    private base = '/api',
    private fetchImpl: typeof fetch = fetch,
  ) {}

  setToken(token: string) {
    this.token = token
  }

  /** Configure a different engine base URL (production: same origin). */
  setBase(base: string) {
    this.base = base
  }

  private async request<T>(path: string, init?: RequestInit): Promise<T> {
    const headers: Record<string, string> = {
      Accept: 'application/json',
    }
    if (this.token) headers['Authorization'] = `Bearer ${this.token}`
    if (init?.body) headers['Content-Type'] = 'application/json'

    const res = await this.fetchImpl(this.base + path, { ...init, headers })
    if (!res.ok) {
      throw await responseError(res, path)
    }
    return (await res.json()) as T
  }

  // -- engine control -----------------------------------------------------

  ping(): Promise<PingReply> {
    return this.request<PingReply>('/ping')
  }

  /** Engine runtime state (scene/token/language/backlog/layers). */
  state(): Promise<StateReply> {
    return this.request<StateReply>('/state')
  }

  /** Engine stats (texture budget/tier, meshes, job workers/pending, Lua heap). */
  stats(): Promise<StatsReply> {
    return this.request<StatsReply>('/stats')
  }

  /** Hit-test the preview frame at a window pixel (1280x720 space). */
  pick(x: number, y: number): Promise<PickReply> {
    return this.request<PickReply>(
      `/pick?x=${x}&y=${y}`,
    )
  }

  /** Validate an SMA asset through the engine's shared checker. */
  smaValidate(path: string): Promise<SmaValidateReply> {
    return this.request<SmaValidateReply>(
      `/sma/validate?path=${encodeURIComponent(path)}`,
    )
  }

  /** Save an SMA asset through the engine shared checker (round 26). */
  smaSave(path: string, content: string): Promise<SmaSaveReply> {
    return this.request<SmaSaveReply>('/sma/save', {
      method: 'POST',
      body: JSON.stringify({ path, content }),
    })
  }

  status(): Promise<StatusReply> {
    return this.request<StatusReply>('/status')
  }

  run(script: string): Promise<{ status: string }> {
    return this.request<{ status: string }>('/run', {
      method: 'POST',
      body: JSON.stringify({ script }),
    })
  }

  /** Execute a Lua expression via /api/eval; returns the raw result
   *  string (the engine JSON-escapes it as "result":"..." — parse twice
   *  when the Lua side returns JSON). The engine reads the request body
   *  as raw Lua code (not JSON-wrapped). */
  async evalRaw(code: string): Promise<string> {
    const res = await this.fetchImpl(this.base + '/eval', {
      method: 'POST',
      headers: {
        Accept: 'application/json',
        'Content-Type': 'text/plain',
        ...(this.token ? { Authorization: `Bearer ${this.token}` } : {}),
      },
      body: code,
    })
    if (!res.ok) {
      throw await responseError(res, '/eval')
    }
    const body = (await res.json()) as { result?: string; error?: string }
    if (body.error) throw new RpcError(body.error, res.status, body)
    return body.result ?? ''
  }

  stop(): Promise<{ status: string }> {
    return this.request<{ status: string }>('/stop', { method: 'POST' })
  }

  reload(): Promise<{ status: string }> {
    return this.request<{ status: string }>('/reload', { method: 'POST' })
  }

  // -- introspection ------------------------------------------------------

  /**
   * List project assets. The optional filter accepts either the coarse
   * category ("image" | "audio" | "script") or a per-directory slot
   * ("bg" | "fg" | "bgm" | "se" | "voice" | ...) so Scene Builder can pull
   * exactly the assets for a slot it is painting.
   */
  assets(
    type?:
      | 'image'
      | 'audio'
      | 'script'
      | 'bg'
      | 'fg'
      | 'char'
      | 'ui'
      | 'bgm'
      | 'voice'
      | 'se'
      | 'scripts',
  ): Promise<AssetEntry[]> {
    const q = type ? `?type=${type}` : ''
    return this.request<AssetEntry[]>(`/assets${q}`)
  }

  logs(): Promise<LogEntry[]> {
    return this.request<LogEntry[]>('/logs')
  }

  live2dModels(): Promise<Live2DModel[]> {
    return this.request<Live2DModel[]>('/live2d/models')
  }

  /** One-click CARC packaging (POST /api/build): packs scripts/ + assets/
   *  into an encrypted archive. Both paths are optional — the engine defaults
   *  to build/game.carc and build/game.key and confines them under build/
   *  (rejecting "..", drive letters and absolute paths). */
  buildCarc(outputPath?: string, keyPath?: string): Promise<BuildReply> {
    return this.request<BuildReply>('/build', {
      method: 'POST',
      body: JSON.stringify({ outputPath, keyPath }),
    })
  }

  /** One-click web-site packaging (POST /api/package/web): the engine
   *  validates a whitelisted story path (assets/ demo/ tests/projects/
   *  projects/), sanitizes outName to [A-Za-z0-9_-] and runs
   *  scripts/package_game.sh into dist/<outName>. Both args optional —
   *  the engine defaults to demo/example_game/story.ks + example_game. */
  packageWeb(storyPath?: string, outName?: string): Promise<WebPackageReply> {
    return this.request<WebPackageReply>('/package/web', {
      method: 'POST',
      body: JSON.stringify({ storyPath, outName }),
    })
  }

  // -- KAG scene debugger -------------------------------------------------

  debugState(): Promise<DebugStateReply> {
    return this.request<DebugStateReply>('/debug/getState')
  }

  frame(w = 640, h = 360): Promise<FrameReply> {
    return this.request<FrameReply>(`/debug/getFrame?w=${w}&h=${h}`)
  }

  setBreakpoint(source: string, line: number): Promise<{ status: string }> {
    return this.request<{ status: string }>('/debug/setBreakpoint', {
      method: 'POST',
      body: JSON.stringify({ source, line }),
    })
  }

  removeBreakpoint(source: string, line: number): Promise<{ status: string }> {
    return this.request<{ status: string }>('/debug/removeBreakpoint', {
      method: 'POST',
      body: JSON.stringify({ source, line }),
    })
  }

  clearBreakpoints(): Promise<{ status: string }> {
    return this.request<{ status: string }>('/debug/clearBreakpoints', {
      method: 'POST',
    })
  }

  debugContinue(): Promise<{ status: string }> {
    return this.request<{ status: string }>('/debug/continue', {
      method: 'POST',
    })
  }

  /** Single-step controls (Sprint 4b): same contract as /continue. */
  debugStep(mode: 'into' | 'over' | 'out'): Promise<{ status: string }> {
    const cap = mode === 'into' ? 'Into' : mode === 'over' ? 'Over' : 'Out'
    return this.request<{ status: string }>(`/debug/step${cap}`, {
      method: 'POST',
    })
  }

  inspect(name: string, frame = 0, global = false): Promise<unknown> {
    const g = global ? '&global=1' : ''
    return this.request<unknown>(
      `/debug/inspect?name=${encodeURIComponent(name)}&frame=${frame}${g}`,
    )
  }

  // -- Live2D -------------------------------------------------------------

  live2dLoad(path: string): Promise<{ status: string; modelId?: number }> {
    return this.request<{ status: string; modelId?: number }>('/live2d/load', {
      method: 'POST',
      body: JSON.stringify({ path }),
    })
  }

  // -- Project Manager ----------------------------------------------------

  /** List discoverable project templates (GET /api/project/templates). */
  projectTemplates(): Promise<ProjectTemplate[]> {
    return this.request<ProjectTemplate[]>('/project/templates')
  }

  /** List managed projects under ./projects/ (GET /api/project/list). */
  projectList(): Promise<ProjectInfo[]> {
    return this.request<ProjectInfo[]>('/project/list')
  }

  /** Create a new project from a template (POST /api/project/create). */
  projectCreate(template: string, name: string): Promise<ProjectOpReply> {
    return this.request<ProjectOpReply>('/project/create', {
      method: 'POST',
      body: JSON.stringify({ template, name }),
    })
  }

  /** Duplicate an existing source project under a new name
   *  (POST /api/project/duplicate). srcPath is the ProjectInfo.path. */
  projectDuplicate(srcPath: string, name: string): Promise<ProjectOpReply> {
    return this.request<ProjectOpReply>('/project/duplicate', {
      method: 'POST',
      body: JSON.stringify({ srcPath, name }),
    })
  }

  /** Import an existing on-disk project directory into the manager
   *  (POST /api/project/import). Unlike duplicate, srcPath may be ANY
   *  directory containing story.ks or entry.lua; it is copied under
   *  ./projects/<name>. */
  projectImport(srcPath: string, name: string): Promise<ProjectOpReply> {
    return this.request<ProjectOpReply>('/project/import', {
      method: 'POST',
      body: JSON.stringify({ srcPath, name }),
    })
  }

  /** Read one project's metadata (GET /api/project/meta). Missing files
   *  come back with inferred=true and engine-default values. */
  projectMeta(path: string): Promise<ProjectMetaReply> {
    return this.request<ProjectMetaReply>(
      `/project/meta?path=${encodeURIComponent(path)}`,
    )
  }

  /** Save editable project metadata (POST /api/project/meta): language,
   *  description and template. name/created/version are engine-owned. */
  projectSaveMeta(
    path: string,
    meta: Partial<Pick<ProjectMeta, 'template' | 'language' | 'description'>>,
  ): Promise<ProjectMetaReply> {
    return this.request<ProjectMetaReply>('/project/meta', {
      method: 'POST',
      body: JSON.stringify({ path, meta }),
    })
  }
}
