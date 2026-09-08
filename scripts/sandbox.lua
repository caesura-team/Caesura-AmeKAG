-- ===========================================================================
--  Caesura (AmeKAG) — Sandbox Rules v2 (Track 3)
--  ===========================================================================
--  Loaded once at engine startup via LuaManager::lockdownScriptEnv().
--  All rules here are readable by external AI assistants.
--
--  Design principle: DEFAULT DENY, EXPLICIT ALLOW.
--  Any capability not explicitly permitted here is blocked.
--  ===========================================================================

-- ---------------------------------------------------------------------------
--  0. TRUSTED LOCAL CAPTURES (must run FIRST)
-- ---------------------------------------------------------------------------
--  The hardened replacements installed below need the ORIGINAL primitives.
--  Capturing them in file-local variables here means:
--    * the internal implementation keeps full power (rawset on _G, real
--      load for engine modules, real set/getmetatable),
--    * sandboxed code can never reach them: they are upvalues of closures
--      in this chunk, and section 2 removes debug.getupvalue /
--      debug.getuservalue / debug.getmetatable — the only Lua-level routes
--      to a closure's upvalues or to a protected metatable.
-- ---------------------------------------------------------------------------

local real_rawset       = rawset
local real_rawget       = rawget
local real_setmetatable = setmetatable
local real_getmetatable = getmetatable
local real_load         = load
local real_getinfo      = _G.debug and _G.debug.getinfo or nil

-- This chunk's own source ("@scripts/sandbox.lua" or "@.../scripts/sandbox.lua"),
-- used to skip our own frames when attributing a caller (section 2b).
local SELF_SOURCE = real_getinfo and real_getinfo(1, "S").source or nil

-- Forward declaration: the caller-attribution predicate used by the load guard
-- (section 2b). Declaring it local here keeps it OUT of _G — a global helper
-- would be readable (and, being non-whitelisted, unwritable but still
-- discoverable) from sandboxed code.
local caller_is_trusted

-- Protected read-only globals (section 7). Values published here are visible
-- as ordinary globals through _G's __index, but they are NOT raw fields of _G,
-- so an assignment to them hits __newindex and is subject to the whitelist.
-- (Lua only consults __newindex for keys that are ABSENT from the table: any
-- value stored raw in _G stays freely overwritable. _SANDBOX_MODE must not be,
-- since "dev" disables the section 9 Render/DevCore whitelist proxies.)
local _G_protected = {}

-- ---------------------------------------------------------------------------
--  1. GLOBAL DANGEROUS FUNCTIONS — REMOVED
-- ---------------------------------------------------------------------------
--  These are the most dangerous entry points: arbitrary file loading,
--  script execution, and process spawning.
--  All asset loading goes through the C++ IAssetProvider chain instead.
--  loadstring (Lua 5.1 name, absent in 5.4) is nil'd for hosts that ship a
--  compat shim: it is a second door to the same dynamic-compile capability.
-- ---------------------------------------------------------------------------

_G.loadfile   = nil
_G.dofile     = nil
_G.loadstring = nil

-- ---------------------------------------------------------------------------
--  2. DEBUG LIBRARY — READ-ONLY SUBSET
-- ---------------------------------------------------------------------------
--  Kept (inspection only):
--    getinfo, traceback
--  Removed (mutation capable OR protection-bypassing):
--    setupvalue, sethook, setlocal, setmetatable, getregistry,
--    upvaluejoin, setuservalue, debug (raw), gethook
--    getmetatable  -- ESCAPE (fixed): __metatable = "protected" only stops
--                     the Lua-level getmetatable; debug.getmetatable handed
--                     out the REAL _G metatable, and rewriting its
--                     __newindex disabled the section 7 write protection.
--    getupvalue / getuservalue
--                  -- ESCAPE (fixed): every hardened wrapper in this file
--                     keeps the original primitive as an upvalue (section 0).
--                     getupvalue on any closure defined here (e.g. the
--                     global _SANDBOX_CHECK) leaked rawset / load / the _G
--                     metatable, defeating the whole lockdown.
--    getlocal      -- unused repo-wide; default-deny (it can read the locals
--                     of trusted frames further up the stack).
--  Repo-wide consumers before removal: only debug.getinfo (demo entry.lua,
--  tests/scripts/test_integration.lua) and debug.traceback are used.
-- ---------------------------------------------------------------------------

if _G.debug then
    local raw_debug = _G.debug
    _G.debug = {
        getinfo      = raw_debug.getinfo,
        traceback    = raw_debug.traceback,
    }
    -- package.loaded keeps its OWN reference to every stdlib table, and the
    -- require wrapper in section 4 resolves names from package.loaded alone --
    -- so narrowing _G.debug without narrowing this handed the untouched library
    -- straight back through require("debug"). That was a full escape, not a
    -- cosmetic gap: getupvalue reaches the real_rawset / real_load / _G_mt
    -- upvalues this file captured in section 0, i.e. the whole hardening below
    -- is only as strong as this line.
    if _G.package and _G.package.loaded then
        _G.package.loaded.debug = _G.debug
    end
    raw_debug = nil  -- release the local: no closure below can capture it
end

-- ---------------------------------------------------------------------------
--  2b. DYNAMIC COMPILATION (load) — TRUSTED HOST CHANNEL ONLY
-- ---------------------------------------------------------------------------
--  load() used to be fully open: any script running with _ENV = _G (an
--  AI-authored engine script, a project entry.lua, an [iscript] body that got
--  hold of it) could compile and run arbitrary Lua at runtime.
--
--  It cannot simply be nil'd: ~30 TRUSTED engine call sites compile source at
--  runtime. Full list (grep over this repo, build artifacts excluded):
--    scripts/config.lua:141,201,272          settings files
--    scripts/system.lua:243,489              backlog + config restore
--    scripts/i18n.lua:78,79,105,369,370      language packs
--    scripts/ks_i18n.lua:93,94               scene i18n tables
--    scripts/music_room.lua:117              favourites file
--    scripts/kag/compiler.lua:610,632        AOT expression dump
--    scripts/kag/expr.lua:567,585            [if] expression cache (mode "b")
--    scripts/kag/schema.lua:128,556          brace interpolation
--    scripts/kag/lsp.lua:325-336             expression diagnostics
--    scripts/ks_check.lua:249-260            linter
--    scripts/scheduler.lua:1051,1111,1121    [iscript]/[eval] host side
--    scripts/sandbox.lua Sandbox.execute     [emb]/[eval] strict path
--  Rewriting those call sites is outside this change's file scope, so load
--  becomes a HOST-ONLY channel instead: the wrapper attributes its CALLER via
--  debug.getinfo (captured in section 0, before the debug subset is narrowed)
--  and only code loaded from a real file under scripts/ or tests/ may compile.
--
--  Concretely:
--    * scripts/*.lua and tests/scripts/*.lua  -> allowed (trusted host code)
--    * a dynamically loaded chunk (=iscript, =eval, =emb, =sandbox, =ksc,
--      =kag_expr, a mod/project chunk — anything whose source is not an @file
--      under scripts/ or tests/) -> refused with nil + message. That is load's
--      documented failure contract, so trusted callers which already handle a
--      compile error degrade instead of crashing.
--    * The refusal message distinguishes binary payloads (mode "b" or the
--      "\27Lua" bytecode signature): Lua does not validate bytecode, so an
--      untrusted binary chunk is a memory-safety hole, not just a sandbox one.
--  Sandbox table environments never expose load at all (SANDBOX_WHITELIST and
--  the [iscript] / [eval] envs are load-free), so this wrapper is the second
--  line of defence, not the only one.
-- ---------------------------------------------------------------------------

local LUA_BINARY_SIGNATURE = "\27Lua"

-- Attribute the CALLER of a guarded primitive. Frames belonging to this file
-- and C frames are skipped: pcall(load, ...) (kag/expr.lua, kag/compiler.lua)
-- inserts a C frame, and the guards themselves live here. The first remaining
-- Lua frame decides. Default deny: an unattributable frame is untrusted.
local function caller_source()
    if not real_getinfo then return nil end
    for level = 2, 16 do
        local info = real_getinfo(level, "S")
        if not info then return nil end
        local src = info.source
        if src and src ~= "=[C]" and src ~= SELF_SOURCE then return src end
    end
    return nil
end

-- Normalise a chunk path: backslashes to "/", "." dropped, ".." resolved.
-- The Lua test runner puts "tests/scripts/../../scripts/?.lua" on package.path,
-- so a trusted engine module legitimately arrives as
-- "@tests/scripts/../../scripts/i18n.lua"; rejecting ".." outright would have
-- broken i18n/config under the suite. Resolving it keeps the check honest
-- without letting a path climb out of the repo unnoticed.
-- Engine root, derived from THIS file's own source: "@D:/repo/scripts/sandbox.lua"
-- yields "D:/repo/", while a relative "@scripts/sandbox.lua" yields "". Trust
-- anchors are built on it so a directory merely NAMED scripts/ somewhere else on
-- the filesystem cannot be mistaken for engine code.
local ROOT_PREFIX = ""
if type(SELF_SOURCE) == "string" and SELF_SOURCE:sub(1, 1) == "@" then
    local self_path = SELF_SOURCE:sub(2):gsub("\\", "/")
    ROOT_PREFIX = self_path:match("^(.*/)scripts/[^/]+$") or ""
end

local function normalise_path(path)
    local parts = {}
    for seg in path:gmatch("[^/]+") do
        if seg == ".." then
            if #parts > 0 and parts[#parts] ~= ".." then
                parts[#parts] = nil
            else
                parts[#parts + 1] = ".."
            end
        elseif seg ~= "." then
            parts[#parts + 1] = seg
        end
    end
    return table.concat(parts, "/")
end

-- Trusted == loaded from a real FILE (source "@path") under scripts/ or tests/.
-- A dynamically compiled chunk carries "=name" or the chunk text as its source
-- and can never satisfy this, so [iscript] / [eval] / [emb] / mod and project
-- chunks are untrusted by construction.
function caller_is_trusted()
    -- A host built without the debug library cannot attribute callers; keeping
    -- the engine functional wins there. Unreachable in this engine: section 2
    -- keeps debug.getinfo and section 0 captured it before narrowing.
    if not real_getinfo then return true end
    local src = caller_source()
    if type(src) ~= "string" then return false end
    if src:sub(1, 1) ~= "@" then return false end   -- dynamic chunk: untrusted
    local path = normalise_path(src:sub(2):gsub("\\", "/"))
    if path:find("%.%.") then return false end     -- unresolved climb: untrusted
    -- Anchored at THIS engine's root, not matched as a substring: "/scripts/"
    -- anywhere in the path used to trust any file under any directory that
    -- happened to be called scripts, e.g. D:/tmp/scripts/evil.lua. ROOT_PREFIX
    -- comes from this file's own source, so a relative source ("@scripts/
    -- sandbox.lua") yields "" and the anchors below stay relative.
    return path == ROOT_PREFIX .. "scripts" or path == ROOT_PREFIX .. "tests"
        or path:sub(1, #ROOT_PREFIX + 8) == ROOT_PREFIX .. "scripts/"
        or path:sub(1, #ROOT_PREFIX + 6) == ROOT_PREFIX .. "tests/"
end

-- The forwarding is varargs-exact on PURPOSE. Lua distinguishes an ABSENT 4th
-- argument (chunk gets _ENV = the globals) from an explicit nil (chunk gets
-- _ENV = nil, and the first global access raises "attempt to index a nil value
-- (upvalue '_ENV')"). Forwarding a fixed 4-tuple turned every env-less call —
-- tests/scripts/test_label_jump.lua, scripts/music_room.lua:117 — into the
-- second case.
_G.load = function(chunk, chunkname, mode, ...)
    -- chunkname is DATA supplied by the compiling code, but caller_is_trusted()
    -- reads it back as provenance -- so load(src, "@scripts/evil.lua") used to
    -- mint a chunk that counted as engine code and could compile further code
    -- forever. A chunk built from a string never came from a file, so strip the
    -- "@" claim: real file provenance can only come from Lua's own file loader
    -- (loadfile/require), never from here. Verified: the forged chunk still
    -- RUNS, it just cannot compile more code.
    if type(chunkname) == "string" and chunkname:sub(1, 1) == "@" then
        chunkname = "=" .. chunkname:sub(2)
    end
    if not caller_is_trusted() then
        local binary = (type(mode) == "string" and mode:find("b", 1, true) ~= nil)
            or (type(chunk) == "string"
                and chunk:sub(1, #LUA_BINARY_SIGNATURE) == LUA_BINARY_SIGNATURE)
        if binary then
            return nil, "Sandbox: load() binary chunks are host-only"
        end
        return nil, "Sandbox: load() is restricted to trusted engine modules"
    end
    if select("#", ...) > 0 then
        return real_load(chunk, chunkname, mode, (select(1, ...)))
    end
    return real_load(chunk, chunkname, mode)
end

-- ---------------------------------------------------------------------------
--  3. PACKAGE SYSTEM — SEARCH DISABLED
-- ---------------------------------------------------------------------------
--  Clears package.searchers/loaders so require() cannot search the filesystem.
--  Only modules preloaded in package.loaded (at startup via config.lua +
--  kag/init.lua) are accessible.
-- ---------------------------------------------------------------------------

package.loadlib    = nil
package.searchpath = nil

if package.searchers then
    package.searchers = {}
elseif package.loaders then
    package.loaders = {}
end

-- ---------------------------------------------------------------------------
--  4. REQUIRE — SAFE WRAPPER
-- ---------------------------------------------------------------------------
--  Only returns preloaded modules. Un-preloaded module = hard error.
-- ---------------------------------------------------------------------------

_G.require = function(name)
    local loaded = package.loaded[name]
    if loaded ~= nil then
        return loaded
    end
    error('Sandbox: module "' .. name .. '" not preloaded. Add to config.lua.', 2)
end

-- ---------------------------------------------------------------------------
--  5. OS MODULE — FILESYSTEM OPERATIONS DISABLED
-- ---------------------------------------------------------------------------
--  Kept: os.clock, os.date, os.time, os.difftime (non-I/O)
--  Replaced with no-ops or sandboxed stubs.
-- ---------------------------------------------------------------------------

if _G.os then
    _G.os.execute = function(cmd) return -1 end
    _G.os.remove  = function(path) return nil, "sandboxed" end
    _G.os.rename  = function() return nil, "sandboxed" end
    _G.os.exit    = function() error("os.exit disabled", 2) end
    _G.os.getenv  = nil
end

-- ---------------------------------------------------------------------------
--  6. IO MODULE — FILESYSTEM READ/WRITE DISABLED
-- ---------------------------------------------------------------------------
--  All file I/O through C++ IAssetProvider chain (read) or SaveManager (write).
-- ---------------------------------------------------------------------------

if _G.io then
    -- Read-only allowlisted io.open: the runtime legitimately reads .ks
    -- scenes (tokenizer/flow), language packs (i18n) and UI config from
    -- scripts/, assets/, tests/ AFTER the sandbox locks. A hard disable
    -- broke cross-scene jumps and load-game restore. Binary reads preserve the
    -- source bytes used by cache freshness hashes. Writes stay blocked.
    local real_open = _G.io.open
    _G.io.open = function(fn, mode)
        if mode ~= "r" and mode ~= "rb" then return nil, "io.open write disabled" end
        if type(fn) ~= "string" then return nil, "io.open path must be string" end
        if fn:find("..", 1, true) then return nil, "io.open traversal rejected" end
        if fn:find("^scripts/") == 1 or fn:find("^assets/") == 1
           or fn:find("^tests/") == 1 or fn:find("^demo/") == 1
           or fn:find("^projects/") == 1 then
            local f, err = real_open(fn, mode)
            if f then return f, nil end
            return nil, err or "io.open open failed"
        end
        return nil, "io.open path not allowlisted"
    end
    _G.io.popen = function() return nil, "io.popen disabled" end
    _G.io.input = nil
    _G.io.output = nil
    _G.io.lines = nil
    _G.io.read = nil
    _G.io.tmpfile = nil
end

-- ===========================================================================
--  TRACK 3: Extreme Security Additions
-- ===========================================================================

-- ---------------------------------------------------------------------------
--  7. _G WRITE PROTECTION (Track 3)
-- ---------------------------------------------------------------------------
--  Prevents AI-generated scripts from poisoning the global environment.
--  Whitelisted globals can still be assigned; everything else is read-only.
--  Uses __newindex metamethod on the global table.
-- ---------------------------------------------------------------------------

local _G_whitelist = {
    -- Standard Lua globals (safe subset)
    assert     = true, error      = true, ipairs     = true,
    next       = true, pairs      = true, pcall      = true,
    select     = true, tonumber   = true, tostring   = true,
    type       = true, xpcall     = true,
    -- Coroutine (safe: no C boundary cross)
    coroutine  = true,
    -- Math (pure computation)
    math       = true,
    -- String (pure computation)
    string     = true,
    -- Table (pure computation)
    table      = true,
    -- Engine API tables (read-only — their internals are protected by C)
    KAG        = true,
    Engine     = true,
    Render     = true,
    VFX        = true,
    mini_game  = true,
    sma        = true, -- skeletal mesh animation binding (SmaBinding)
    DevCore    = true,
    steam      = true,  -- Steamworks binding global (SteamBinding.cpp); tests
                        -- install/remove a mock through rawset (section 7b)
    _CAESURA_BACKEND = true,
    _CAESURA_CONFIG  = true,
    _SANDBOX_RESOURCES = true,
    _SANDBOX_CHECK     = true,
    -- Engine callback globals (game scripts define these)
    engine_update       = true,
    engine_render       = true,
    _KAG_onClick        = true,
    _KAG_onKey          = true,
    _KAG_onScroll       = true,
    _KAG_onTextInput    = true,
    _KAG_onTextEditing  = true,
    _KAG_onKeyDown      = true,
    _KAG_onKeySpace     = true,  -- t109: native SwipeDown -> message-layer toggle
    _KAG_onKeyPageUp    = true,  -- t109: native SwipeUp -> open backlog view
    -- Engine runtime state globals (set by C++ main loop).
    -- These MUST all be listed: lua_setglobal honours __newindex, so a C++
    -- write to an unlisted key that is not already a raw field raises
    -- lua_error, and the engine-side writers are not inside a protected call
    -- -- i.e. a panic, not a silent no-op. DevCoreBinding's _CAESURA_QUIT is
    -- the live example: it is only written when DevCore.quit() runs, which can
    -- happen after lockdown.
    _CAESURA_QUIT = true,
    _CAESURA_DEVICE_RESTORED = true,
    _CAESURA_DEBUG_PAUSED = true,
    _CAESURA_DEBUG_IS_PAUSED = true,
    _CAESURA_GPU_QUALITY  = true,
    _CAESURA_VFX_ENABLED  = true,
    _CAESURA_GPU_TIME_MS  = true,
    _CAESURA_GPU_AVG_MS   = true,
    _CAESURA_GPU_DEGRADED = true,
    _CAESURA_VOICE_COMPLETE = true,
    _CAESURA_CTX = true,
    _CAESURA_ENGINE = true,
    _ASYNC_CALLBACKS = true,  -- async load callback registry (RenderBinding)
    _AI_CALLBACKS    = true,  -- async AI callback registry (AIBinding)
    AI               = true,  -- LLM query binding (config.ai endpoint)
    _GAME_MOUSE_X   = true,
    _GAME_MOUSE_Y   = true,
    _GAME_MOUSE_DOWN = true,
    _GAME_KEY_F     = true,
    _GAME_KEY_F4    = true,
    _GAME_KEY_F5    = true,
    _GAME_KEY_F6    = true,
    _GAME_KEY_W     = true,
    _GAME_KEY_A     = true,
    _GAME_KEY_S     = true,
    _GAME_KEY_D     = true,
    _GAME_KEY_UP    = true,
    _GAME_KEY_LEFT  = true,
    _GAME_KEY_RIGHT = true,
    _GAME_KEY_DOWN  = true,
    _GAME_KEY_ENTER = true,
    _GAME_KEY_ESC   = true,
    _GAME_KEY_H     = true,
    _GAME_KEY_V     = true,
    _GAME_KEY_BACKSPACE = true,
    _KAG_MOUSE_WHEEL_Y = true,
    _CAESURA_AUDIO_EVENT = true,
    _CAESURA_VOICE_COMPLETE = true,
    quicksave = true,
    quickload = true,
    autosave  = true,
    debug      = true,  -- already read-only from section 2
    -- Read-only globals
    package    = true,
    os         = true,  -- I/O already disabled in section 5
}

do
    -- _G_mt lives in a do-block so the table itself is not even a file-local
    -- upvalue of the module-level functions below: the only reference kept is
    -- __newindex's closure. Retrieval routes that used to exist:
    --   getmetatable(_G)        -> "protected" (the __metatable marker)
    --   debug.getmetatable(_G)  -> REAL table  (ESCAPE, removed in section 2)
    --   debug.getupvalue(f)     -> upvalues of any closure here (removed too)
    local _G_mt = {
        -- Table (not function) __index: a missing-global read stays a C-level
        -- table lookup, so the hot path keeps its cost.
        __index = _G_protected,
        __newindex = function(t, k, v)
            if _G_whitelist[k] then
                -- real_rawset (section 0), NOT the global: the section 7b
                -- wrapper would re-enter this check for _G writes.
                real_rawset(t, k, v)  -- allow whitelisted writes
            else
                error("Sandbox: cannot create global '" .. tostring(k) .. "'", 2)
            end
        end,
        __metatable = "protected",
    }
    real_setmetatable(_G, _G_mt)
end

-- ---------------------------------------------------------------------------
--  7b. rawset — __newindex BYPASS CLOSED
-- ---------------------------------------------------------------------------
--  rawset ignores __newindex by definition, so section 7's write protection
--  was decorative: one line,
--      rawset(_G, "evil", 1)
--  created any global it liked. Closed on two independent fronts:
--    * rawset / rawget are GONE from every sandbox environment
--      (SANDBOX_WHITELIST, section 10; the [iscript] and [eval] envs built in
--      scheduler.lua never had them).
--    * the GLOBAL rawset is wrapped here to enforce the SAME key whitelist as
--      __newindex when the target table is _G, with the SAME error text — so a
--      raw write and a plain assignment are now indistinguishable, whoever the
--      caller is. This is deliberately NOT caller-attributed: a key-based rule
--      cannot be spoofed by stack shape, and every legitimate rawset(_G, ...)
--      in the engine already writes a whitelisted key
--      (scripts/system.lua quicksave/quickload/autosave, backend_factory.lua
--      and config.lua _CAESURA_BACKEND, kag_runner.lua _CAESURA_CTX, the test
--      mocks steam / _CAESURA_BACKEND / _CAESURA_CTX).
--  rawset on any OTHER table is untouched: the documented "no __newindex trap"
--  ctx.tf writes (kag/commands/system.lua, scheduler.lua) keep working, and so
--  do the mock-table rawsets in tests/scripts/.
-- ---------------------------------------------------------------------------

_G.rawset = function(t, k, v)
    if t == _G and not _G_whitelist[k] then
        error("Sandbox: cannot create global '" .. tostring(k) .. "'", 2)
    end
    return real_rawset(t, k, v)
end

-- ---------------------------------------------------------------------------
--  8. RESOURCE BUDGET TRACKING HOOKS (Track 3)
-- ---------------------------------------------------------------------------
--  These counters are incremented by the C++ side when resources are
--  allocated through KAG API functions. AI scripts can query them.
-- ---------------------------------------------------------------------------

_G._SANDBOX_RESOURCES = {
    textures_loaded   = 0,
    textures_max      = 256,   -- hard cap for AI scripts
    audio_handles_loaded = 0,
    audio_handles_max    = 64, -- hard cap for AI scripts
    rtt_canvases_loaded  = 0,
    rtt_canvases_max     = 8,
    particles_emitters_loaded = 0,
    particles_emitters_max    = 16,
}

-- Check function: call before allocating to avoid resource exhaustion
function _G._SANDBOX_CHECK(kind)
    local r = _SANDBOX_RESOURCES
    local cur = r[kind .. "_loaded"]
    local max = r[kind .. "_max"]
    if cur and max and cur >= max then
        error("Sandbox: " .. kind .. " limit reached (" .. max .. ")", 2)
    end
end

-- ===========================================================================
-- ---------------------------------------------------------------------------
--  9. RENDER OPERATION WHITELIST (Track 3) -- "DEFAULT DENY, EXPLICIT ALLOW"
-- ---------------------------------------------------------------------------
--  In strict mode, engine API tables (Render, DevCore) are wrapped with
--  __index proxies. Calls to non-whitelisted functions are blocked.
--  Set _SANDBOX_MODE = "dev" (or omit) to disable whitelist enforcement.
-- ---------------------------------------------------------------------------

-- Whitelist: Render module -- allowed functions for AI scripts
local RENDER_WHITELIST = {
    load_texture        = true,
    destroy_texture     = true,
    create_solid_texture = true,
    get_resolution      = true,
    set_view_name       = true,
    set_screen_offset   = true,
    submit_batch        = true,
    submit_blend        = true,
    load_texture_async  = true,
    cancel_async_loads  = true,
    -- Viewport operations (for RTT layer compositing)
    create_viewport     = true,
    destroy_viewport    = true,
    draw_viewport       = true,
    fill_viewport       = true,
    -- Transition and VFX rendering
    submit_transition   = true,
    submit_vfx          = true,
    submit_stretch_blt  = true,
    submit_affine_blt   = true,
    set_color_filter    = true,  -- accessibility filter presets
    set_postfx          = true,  -- PostFx chain (bloom/vignette/lut/softblur)
    destroy_postfx      = true,
    clear_postfx        = true,
    is_postfx_supported = true,
    is_postfx_active    = true,
    -- Text rendering state (font face/size/color)
    text_set_font       = true,
    text_reset_state    = true,
    -- NOTE: video_play/video_stop/video_is_playing are deliberately NOT
    -- whitelisted -- AI scripts may not drive video; the BackendFactory
    -- closure captures the real Render, so engine scripts are unaffected.
}

-- Whitelist: DevCore module -- allowed functions for AI scripts
local DEVCORE_WHITELIST = {
    set_input_focus     = true,
    get_input_focus     = true,
    log                 = true,
    get_resolution      = true,
    get_window_size     = true,
    start_text_input    = true,
    stop_text_input     = true,
    set_text_input_rect = true,
    is_text_input_active = true,
}

-- Whitelist: Debug module -- read-only inspection only
local DEBUG_WHITELIST = {
    get_last_error      = true,
    get_error_count     = true,
    get_subsystem_stats = true,
    dump_report         = true,
    get_render_info     = true,
    get_audio_info      = true,
    get_input_info      = true,
    get_log_path        = true,
    log                 = true,
    get_stats           = true,
}

-- Build a call-proxy metatable for a module table
local function make_whitelist_proxy(original, whitelist, module_name)
    local proxy = {}
    local mt = {
        __index = function(t, k)
            local v = original[k]
            if v == nil then return nil end
            if type(v) == "function" then
                if whitelist[k] then
                    return v  -- allowed: return original function
                else
                    error("Sandbox: " .. module_name .. "." .. k .. " is blocked in strict mode", 2)
                end
            end
            return v  -- non-function fields: always readable
        end,
        __newindex = function(t, k, v)
            error("Sandbox: cannot modify " .. module_name .. " table", 2)
        end,
        __metatable = "protected",
    }
    real_setmetatable(proxy, mt)
    return proxy
end

-- Apply whitelist proxies in strict mode
-- Default mode is "strict" -- AI scripts get maximum protection.
-- Developers can set _SANDBOX_MODE = "dev" in config.lua for full access.
-- Check if _CAESURA_CONFIG.dev_mode is set (by main.cpp before lockdown)
local function is_dev_mode()
    local cfg = real_rawget(_G, "_CAESURA_CONFIG")
    if type(cfg) == "table" and cfg.dev_mode == true then
        return true
    end
    return false
end

-- _SANDBOX_MODE is published into the PROTECTED slot table (section 7), not as
-- a raw field of _G: __newindex only guards keys that are absent, so a raw
-- _SANDBOX_MODE would have stayed freely assignable and any script could
-- downgrade the sandbox to "dev" (which disables the section 9 whitelist
-- proxies). Reads are unchanged — bare _SANDBOX_MODE and _G._SANDBOX_MODE both
-- resolve through __index. A pre-lockdown value (config.lua dev_mode workflow)
-- is honoured and then migrated out of raw storage.
do
    local mode = real_rawget(_G, "_SANDBOX_MODE")
    if mode == nil then
        mode = is_dev_mode() and "dev" or "strict"
    end
    real_rawset(_G, "_SANDBOX_MODE", nil)  -- drop the raw (writable) field
    _G_protected._SANDBOX_MODE = mode
end

if _SANDBOX_MODE == "strict" then
    -- KAG: full access (safe high-level API, all ops go through BackendRegistry)
    -- VFX: full access (already quota-controlled by C++ side)
    -- Unified (_CAESURA_BACKEND): delegates through whitelisted modules, inherits restrictions

    if Render then
        _G.Render = make_whitelist_proxy(Render, RENDER_WHITELIST, "Render")
    end
    if DevCore then
        _G.DevCore = make_whitelist_proxy(DevCore, DEVCORE_WHITELIST, "DevCore")
    end
    if Debug then
        _G.Debug = make_whitelist_proxy(Debug, DEBUG_WHITELIST, "Debug")
    end

    print("[Sandbox] Strict mode: render operation whitelist active")
else
    print("[Sandbox] Dev mode: whitelist disabled, full API access")
end


-- ===========================================================================
--  10. SANDBOX MODULE API -- for require("sandbox")
-- ===========================================================================
--  Provides create/execute/is_strict for standalone Lua testing,
--  AI tool integration, and IDE code assistants.
-- ===========================================================================

local Sandbox = {}

-- Restricted whitelist for the release-mode fallback lookup (audit
-- should-fix): the old `__index = _G` let loadfile/dofile/require/debug
-- fall through -- env.loadfile = nil was shadowed by _G.loadfile, so
-- the strict sandbox path (emb/eval via sandbox.execute) was open.
--
-- REMOVED from this whitelist (escape chain, this change):
--   rawset / rawget -- rawset ignores __newindex, so `rawset(_G, "x", 1)`
--       walked straight through the section 7 write protection. Nothing in
--       the engine needs raw table access from INSIDE a sandbox: the trusted
--       ctx.tf writes ([emb]/[eval] "no __newindex trap" invariant) all run in
--       host modules (kag/commands/system.lua, scheduler.lua), never in
--       sandboxed chunks.
--   load / loadstring -- never were in here, and must not be: they are the
--       generic "compile anything" capability (also gated host-side, 2b).
-- SAFE-ISED (kept, but wrapped):
--   setmetatable / getmetatable -- some KAG3-era author scripts build small
--       OO helpers inside [iscript], so removing them outright would be a
--       behaviour break. The wrappers below refuse to touch _G, refuse to
--       REPLACE an existing metatable, and never hand a real metatable table
--       back (only the __metatable marker), so they cannot be used to reach
--       or rewrite _G_mt / the env metatable / an engine proxy metatable.

-- getmetatable that never leaks a live metatable TABLE. A protected table
-- returns its __metatable marker (a string here, e.g. "protected"/"sandbox");
-- anything else reports nil rather than a mutable table.
local function sandbox_getmetatable(t)
    local mt = real_getmetatable(t)   -- honours __metatable already
    if type(mt) == "table" then return nil end
    return mt
end

-- setmetatable restricted to FRESH tables: attaching behaviour to a table the
-- sandboxed code just built is fine, hijacking an existing one is not.
local function sandbox_setmetatable(t, mt)
    if type(t) ~= "table" then
        error("Sandbox: setmetatable expects a table", 2)
    end
    if t == _G then
        error("Sandbox: cannot set a metatable on _G", 2)
    end
    if real_getmetatable(t) ~= nil then
        error("Sandbox: cannot replace an existing metatable", 2)
    end
    if type(mt) == "table" and (mt.__index == _G or mt.__newindex == _G) then
        error("Sandbox: cannot alias _G through a metatable", 2)
    end
    return real_setmetatable(t, mt)
end

local SANDBOX_WHITELIST = {
    math = math, string = string, table = table,
    tostring = tostring, tonumber = tonumber, type = type,
    pairs = pairs, ipairs = ipairs, next = next, print = print,
    pcall = pcall, xpcall = xpcall, assert = assert,
    select = select, unpack = unpack or table.unpack,
    error = error, coroutine = coroutine,
    setmetatable = sandbox_setmetatable, getmetatable = sandbox_getmetatable,
    os = { clock = os.clock, date = os.date, time = os.time, difftime = os.difftime },
    io = { write = io.write },
}

function Sandbox.create(opts)
    opts = opts or {}
    local mode = opts.mode or "release"
    local env = {}
    -- Read-only proxy: the shared whitelist must not be mutable through
    -- getmetatable(env) + rawset from sandboxed code (review LOW).
    local whitelist = {}
    for k, v in pairs(SANDBOX_WHITELIST) do
        -- Copy the restricted os/io tables too: dev-mode envs resolve
        -- them through the metatable and a sandboxed rawset(nil) would
        -- corrupt the shared tables for later sandboxes (review LOW).
        if type(v) == "table" then
            -- Shallow-copy ALL table members (math/string/table/coroutine
            -- included): sandboxed rawset on a member REPLACES it only in
            -- this env's copy -- the shared globals stay intact (info
            -- item; member FUNCTIONS are shared closures, still safe).
            local t = {}
            for k2, v2 in pairs(v) do t[k2] = v2 end
            whitelist[k] = t
        else
            whitelist[k] = v
        end
    end
    real_setmetatable(env, { __index = whitelist, __metatable = "sandbox" })
    if mode == "release" then
        env.os = { clock = os.clock, date = os.date, time = os.time, difftime = os.difftime }
        env.io = { write = io.write }
    end
    return env
end

function Sandbox.execute(code, env)
    env = env or Sandbox.create()
    -- real_load, mode "t": this IS the trusted host compile channel for
    -- [emb]/[eval] (reachable only through require("sandbox"), which no sandbox
    -- environment provides). Text mode only -- a caller must never be able to
    -- feed unvalidated bytecode in here.
    local fn, err = real_load(code, "=sandbox", "t", env)
    if not fn then return false, err end
    local ok, result = pcall(fn)
    -- Return the ENV as the third value: [emb]'s strict path syncs
    -- envOut.tf/f/sf/mp back -- without it that sync was dead code and
    -- a script REPLACING tf (rather than writing f.x) lost the change
    -- (audit fix).
    return ok, result, env
end

function Sandbox.is_strict()
    return _SANDBOX_MODE == "strict"
end

package.loaded["sandbox"] = Sandbox
return Sandbox

-- ===========================================================================
--  End of sandbox rules.
-- ===========================================================================
