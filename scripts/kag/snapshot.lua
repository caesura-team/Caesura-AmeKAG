-- snapshot.lua — token-level rollback snapshots for the KAG runner.
--
-- On every click that advances the script, the runner pushes a snapshot of
-- the fields needed to *return* to that point: the scene, the token index
-- of the line the player just finished, the variable tables, the visible
-- text state, layer transforms and the backlog length. Rolling back pops the
-- newest snapshot, restores those fields, and resumes at the saved next
-- execution position only after another click.
--
-- Constraint set (documented in docs/plans/rollback):
--   * snapshots are pushed only on click-advance, never across a [choice]
--     branch or a macro-expansion point (the undo stack is cleared there)
--   * audio does NOT roll back: voice is stopped on restore, BGM is not
--     rewound (a [playbgm] inside the replayed span re-applies naturally)
--   * restore marks the whole line revealed (no typewriter replay)

local snapshot = {}

-- Field whitelist. emb/eval in strict mode replace the f/sf/tf/mp/variables
-- tables by reference, so restore MUST swap the whole table, never merge.
local DEEP_COPY_KEYS = { "f", "sf", "tf", "lf", "mp", "variables" }
local REF_KEYS = { "tokens", "macros", "characters", "backlog" }

-- Local deep copy (system.table_deep_copy is a module-local; keep this
-- module self-contained so the test harness can load it standalone).
local function deep_copy(orig, copies)
    copies = copies or {}
    if type(orig) ~= "table" then return orig end
    if copies[orig] then return copies[orig] end
    local copy = {}
    copies[orig] = copy
    for k, v in next, orig do
        copy[deep_copy(k, copies)] = deep_copy(v, copies)
    end
    return copy
end

-- Private, weakly keyed work buffers never become part of a snapshot and
-- never change the live maps. Each capture still scans every live entry:
-- existing-value edits and removals must be observed, not just appends.
local seen_cache = setmetatable({}, { __mode = "k" })

local function set_seen_bit(cache, index, present)
    local block, mask = (index - 1) // 64, 1 << ((index - 1) % 64)
    local old = cache.words[block] or 0
    local bits = present and (old | mask) or (old & ~mask)
    local slot = cache.slots[block]
    cache.flags[index] = present or nil
    if bits == 0 then
        local last = #cache.chunks
        if slot ~= last then
            local moved = cache.blocks[last]
            cache.chunks[slot], cache.blocks[slot] = cache.chunks[last], moved
            cache.slots[moved] = slot
        end
        cache.chunks[last], cache.blocks[last] = nil, nil
        cache.words[block], cache.slots[block] = nil, nil
    else
        slot = slot or (#cache.chunks + 1)
        cache.words[block], cache.slots[block], cache.blocks[slot] = bits, slot, block
        cache.chunks[slot] = string.pack("<I4I8", block, bits)
    end
end

local function pack_seen_flags(flags)
    local cache = seen_cache[flags]
        or { flags = {}, count = 0, words = {}, slots = {}, blocks = {}, chunks = {}, packed = "" }
    local cached_flags=cache.flags
    local added, count = {}, 0
    for index, value in next, flags do
        if value ~= true then return nil end
        count = count + 1
        if not cached_flags[index] then
            if type(index) ~= "number" or index % 1 ~= 0
                or index < 1 or index > 2147483647 then return nil end
            added[#added + 1] = index
        end
    end
    local removed = count ~= cache.count + #added
    -- Updates may allocate (pack/concat/table growth). A failed capture must
    -- discard its work buffer; only a fully encoded version is cached again.
    if removed or #added > 0 then seen_cache[flags] = nil end
    if removed then
        for index in next, cached_flags do
            if rawget(flags, index) ~= true then set_seen_bit(cache, index, false) end
        end
    end
    for _, index in ipairs(added) do set_seen_bit(cache, index, true) end
    if removed or #added > 0 then cache.packed = table.concat(cache.chunks) end
    cache.count = count
    seen_cache[flags] = cache
    return cache.packed
end

-- Standard maps use immutable sparse 64-bit block strings. Unchanged scenes
-- share that immutable encoding across snapshots. Nonstandard graphs retain
-- the original deep-copy behavior, including aliases and arbitrary values.
local function pack_seen(seen)
    if type(seen) ~= "table" then return nil end
    local packed, visited = {}, {}
    for scene, flags in next, seen do
        if type(scene) ~= "string" or type(flags) ~= "table" or visited[flags] then return nil end
        visited[flags] = true
        local bytes = pack_seen_flags(flags)
        if not bytes then return nil end
        packed[scene] = bytes
    end
    return packed
end

local function unpack_seen(packed)
    local seen = {}
    for scene, bytes in next, packed do
        local flags, offset = {}, 1
        while offset <= #bytes do
            local block, bits
            block, bits, offset = string.unpack("<I4I8", bytes, offset)
            for bit = 0, 63 do
                if (bits & (1 << bit)) ~= 0 then flags[block * 64 + bit + 1] = true end
            end
        end
        seen[scene] = flags
    end
    return seen
end

-- Shallow-copy the text_state: the draws array is APPEND-ONLY in normal
-- play (each [ch]/[text] appends a draw; only remove_group replaces the
-- array wholesale, which never mutates a previously snapshotted array).
-- The draw entries are immutable value tables, so sharing them between
-- the live state and every snapshot is safe -- restore truncates to the
-- snapshot length (replay appends fresh draws, never mutating old ones).
-- Measured: 2000-draw state deep copy 517 KB vs shallow 32 KB (-93.8%);
-- with a 64-snapshot undo stack this is the dominant rollback memory cost.
local function copy_text_state(state)
    if type(state) ~= "table" then return state end
    local out = {}
    for k, v in pairs(state) do
        if k == "draws" or k == "page_src" then
            -- copy the ARRAY (new table), share the entries. page_src is
            -- the language hot-switch replay source (parallel to draws:
            -- append-only in play, replaced wholesale by redraws).
            local arr = {}
            if type(v) == "table" then
                for i = 1, #v do arr[i] = v[i] end
            end
            out[k] = arr
        else
            out[k] = v
        end
    end
    return out
end

--- snapshot.capture(ctx) → snap | nil
function snapshot.capture(ctx)
    if type(ctx) ~= "table" then return nil end
    local text_state = require("kag.text_scene").get_state(ctx)
    local snap = {
        scene = ctx.current_scene or ctx.currentScene or "",
        token_index = ctx.token_index,
        -- Completed [ch]/[text] already advanced _resume_index. An explicit
        -- [p] is suspended inside its handler and must re-enter that wait so
        -- the next click still performs its page-clear continuation.
        resume_index = ctx._executing_index or ctx._resume_index or ctx.token_index,
        resume_page_wait = ctx._executing_command == "p",
        call_stack = deep_copy(ctx.call_stack),
        _seen_blocks = pack_seen(ctx.seen_scenes),
        backlog_len = type(ctx.backlog) == "table" and #ctx.backlog or 0,
        text_speed = ctx.text_speed,
        skip_mode = ctx.skip_mode,
        auto_mode = ctx.auto_mode,
        nvl_mode = ctx.nvl_mode,
        waiting_input = ctx.waiting_input,
        text_state = copy_text_state(text_state),
        reveal = (type(ctx.reveal) == "table") and {
            total = ctx.reveal.total,
            elapsed = (ctx.reveal.total or 0) * math.max(0, tonumber(ctx.text_speed) or 50),
            -- [typewriter sound] (t201): restore marks the whole line
            -- revealed (no typewriter replay); seal the SE boundary at
            -- total so a rollback cannot fire a burst of SEs.
            last_shown = ctx.reveal.total or 0,
        } or nil,
        layers = require("layers").capture_snapshot(),
    }
    if not snap._seen_blocks then snap.seen_scenes = deep_copy(ctx.seen_scenes) end
    for _, k in ipairs(DEEP_COPY_KEYS) do
        snap[k] = deep_copy(ctx[k])
    end
    for _, k in ipairs(REF_KEYS) do
        snap[k] = ctx[k] -- shared reference: tokens/macros never mutate
    end
    return snap
end

--- snapshot.restore(ctx, snap) — swap state back; returns true on success.
function snapshot.restore(ctx, snap)
    if type(ctx) ~= "table" or type(snap) ~= "table" then return false end
    local text_scene = require("kag.text_scene")

    ctx.current_scene = snap.scene
    ctx.currentScene = snap.scene
    ctx.label_index = nil  -- security: a rollback across a [call] span must
    -- not reuse the callee's label index (stale cross-scene jump hazard)
    ctx.token_index = snap.token_index or 1
    ctx._resume_index = snap.resume_index or ctx.token_index
    ctx._executing_index, ctx._executing_command = nil, nil
    ctx.call_stack = deep_copy(snap.call_stack)
    ctx.seen_scenes = snap._seen_blocks and unpack_seen(snap._seen_blocks)
        or deep_copy(snap.seen_scenes) or {}
    if type(ctx.backlog) == "table" and type(snap.backlog_len) == "number" then
        -- Truncate replay duplicates: the replayed [ch] pushes again.
        for i = #ctx.backlog, snap.backlog_len + 1, -1 do
            ctx.backlog[i] = nil
        end
    end
    ctx.text_speed = snap.text_speed
    ctx.skip_mode = snap.skip_mode
    ctx.auto_mode = snap.auto_mode
    ctx.nvl_mode = snap.nvl_mode
    ctx.waiting_input = snap.waiting_input

    for _, k in ipairs(DEEP_COPY_KEYS) do
        ctx[k] = deep_copy(snap[k]) or (k == "lf" and {} or ctx[k])
    end
    for _, k in ipairs(REF_KEYS) do
        if snap[k] ~= nil then ctx[k] = snap[k] end
    end

    -- Text state: swap the whole table; reveal forced complete (no replay).
    if snap.text_state then
        ctx.text_state = copy_text_state(snap.text_state)
        ctx.textCursorX = snap.text_state.cursor_x
        ctx.textCursorY = snap.text_state.cursor_y
    end
    ctx.reveal = deep_copy(snap.reveal)
    if ctx.reveal then
        ctx.reveal.elapsed = (ctx.reveal.total or 0) * math.max(0, tonumber(ctx.text_speed) or 50)
        ctx.reveal.last_shown = ctx.reveal.total or 0
        text_scene.get_state(ctx).reveal_chars = ctx.reveal.total or 0
    end

    if snap.layers then require("layers").restore_snapshot(snap.layers) end

    -- Audio: stop the voice line (SE/BGM cannot be un-played; documented).
    local backend = require("backend")
    if backend.audio_stop then
        pcall(function() backend.audio_stop("voice") end)
    end
    return true
end

return snapshot
