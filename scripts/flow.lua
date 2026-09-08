-- =============================================================================
--  Caesura (AmeKAG) — flow.lua
--  Script flow utilities: scene loading, label map building, skip helpers.
--  Used by scheduler.lua for [jump]/[call]/[link] cross-scene navigation.
-- =============================================================================

local flow = {}

-- ── Scene cache ─────────────────────────────────────────────────────────────

flow.scene_cache = {}

-- Public cache entries retain the latest loaded scene for preload/hot-reload
-- callers. Their token streams belong to a runner and may be rewritten by
-- dynamic macros. Only these private, detached payloads can seed another load.
local scene_templates = setmetatable({}, { __mode = "k" })

local function restore_template(compiler, data)
    local ok, tokens = pcall(compiler.deserialize, data)
    if ok then return tokens end
end

local function remember_template(compiler, tokens)
    local ok, data = pcall(compiler.serialize, tokens)
    if not ok or not data then return nil end
    -- serialize may share nested tables with its input. Deserialize first so
    -- the stored payload never points back into the live scene's token graph.
    local detached = restore_template(compiler, data)
    if not detached then return nil end
    local saved, payload = pcall(compiler.serialize, detached)
    if saved then return payload end
end

-- ── flow.load_scene(path) → {tokens, labels} ────────────────────────────────

-- Both memory and .ksc caches must match the current resolved source content
-- and compiler/schema contract. Every load owns an independent token graph.
-- Cache failures degrade to parse+compile; source parsing remains authoritative.

function flow.load_scene(path, prepare_only)
    -- Check cache (cache keyed by the RESOLVED path so a mod override
    -- that appears after a base scene was cached still wins).
    -- Mod resolution: enabled mods may override base scenes
    -- (mods/<name>/<path>); the resolved path is cached independently.
    local mods = require("mods")
    local resolved = mods.resolve_scene(path)
    local tokenizer = require("tokenizer")
    local compiler = require("kag.compiler")

    -- Cache path: compiled bytecode lives in cache/ksc/ (never next to
    -- the source, so scene asset directories stay clean). The cache key
    -- is the resolved path with separators and extension sanitized.
    local kscPath = "cache/ksc/" .. resolved:gsub("[/\\]+", "_"):gsub("%.ks$", ".ksc")
    local tokens = nil
    local template_data = nil
    local srcHash = nil
    if not prepare_only then
        local hashed, hash = pcall(compiler.hashFile, resolved)
        if hashed then srcHash = hash end
    end

    -- A caller may clear/replace scene_cache directly. Respect that observable
    -- cache invalidation instead of silently retaining an unrelated template.
    local previous = not prepare_only and scene_templates[flow.scene_cache[resolved]]
    if previous and srcHash and previous.hash == srcHash then
        -- deserialize checks compatibility against the CURRENT compiler and
        -- command schemas, including in-place edits to a schema default.
        tokens = restore_template(compiler, previous.data)
        if tokens then template_data = previous.data end
    end

    -- 1) Try persisted bytecode when no compatible memory template remains.
    if not tokens and not prepare_only and srcHash then
        local read, cached = pcall(compiler.readCache, kscPath)
        if read and cached and #cached > 0 and cached._compiled then
            local checked, compatible = pcall(compiler.isCompatible, cached)
            if checked and compatible and cached._compiled._srcHash == srcHash then
                tokens = cached
            end
        end
    end

    -- 2) Cache miss: parse + compile, then persist.
    if not tokens then
        local ok, tokens_or_err = pcall(tokenizer.parse_file, resolved)
        if not ok then
            print("[Flow] Failed to load scene: " .. resolved .. " - " .. tostring(tokens_or_err))
            return nil, tokens_or_err
        end
        tokens = tokens_or_err
        local compiled, reason=pcall(compiler.compile, tokens)
        if prepare_only and not compiled then return nil,reason end
        if not prepare_only then
            tokens._srcHash = srcHash
            pcall(compiler.writeCache, tokens, kscPath)
        end
    end

    -- Label map from the compiled index (fixes the legacy dead-code path
    -- that scanned record-format tokens with array-format checks).
    local labels = {}
    if tokens._compiled and tokens._compiled.labels then
        for k, v in pairs(tokens._compiled.labels) do labels[k] = v end
    end

    local scene = {tokens = tokens, labels = labels, path = resolved,
                   base_path = path}
    if not prepare_only then
        local payload = template_data or remember_template(compiler, tokens)
        scene_templates[scene] = payload and { data = payload, hash = srcHash } or nil
        flow.scene_cache[resolved] = scene
    end
    return scene
end

-- Restore preparation must not reuse a token stream changed by live macros or
-- replace the cache entry used by the still-running session.
function flow.prepare_scene(path)
    return flow.load_scene(path, true)
end

-- ── flow.reload_scene(path) — force reload (for hot reload) ──────────────────

function flow.reload_scene(path)
    local resolved = require("mods").resolve_scene(path)
    flow.scene_cache[resolved] = nil
    flow.scene_cache[path] = nil
    return flow.load_scene(path)
end

-- ── flow.clear_cache() — hot reload support ──────────────────────────────────

function flow.clear_cache()
    flow.scene_cache = {}
    scene_templates = setmetatable({}, { __mode = "k" })
end

-- ── flow.skip_to(tokens, start, targets) → index ────────────────────────────

function flow.skip_to(tokens, start_idx, target_cmds)
    local depth = 1
    local opens = target_cmds.opens or {}
    local targets = {}
    for _, t in ipairs(target_cmds) do targets[t] = true end

    for i = start_idx + 1, #tokens do
        local cmd = tokens[i][1]
        if targets[cmd] and depth == 1 then
            return i
        elseif opens[cmd] then
            depth = depth + 1
        elseif targets[cmd] then
            depth = depth - 1
        end
    end
    return #tokens
end

-- ── flow.find_label(tokens, name) → index ────────────────────────────────────

function flow.find_label(tokens, name)
    for i, tok in ipairs(tokens) do
        if tok[1] == "label" and tok[2] and tok[2].name == name then
            return i
        end
    end
    return nil
end

return flow
