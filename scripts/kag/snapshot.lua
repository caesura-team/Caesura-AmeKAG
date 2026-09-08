-- snapshot.lua — token-level rollback snapshots for the KAG runner.
--
-- On every click that advances the script, the runner pushes a snapshot of
-- the fields needed to *return* to that point: the scene, the token index
-- of the line the player just finished, the variable tables, the visible
-- text state, reconstructible layer declarations and the backlog length. Rolling back pops the
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
local DEEP_COPY_KEYS = { "f", "sf", "tf", "mp", "variables" }
local EMPTY_DEFAULT_KEYS = { "lf", "unlockedCG", "unlockedMusic", "seen_endings", "characters" }
local REF_KEYS = { "tokens", "macros", "backlog" }

-- Local deep copy (system.table_deep_copy is a module-local; keep this
-- module self-contained so the test harness can load it standalone).
local function deep_copy(orig, copies)
    if type(orig) ~= "table" then return orig end
    -- A top-level empty value has no graph to track. Recursive copies still
    -- register even empty tables so repeated references preserve identity.
    if not copies and next(orig) == nil then return {} end
    copies = copies or {}
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
        count = count + 1
        -- The private map contains only true entries; next never yields nil
        -- values. Equality proves both "already known" and "still true" in
        -- one comparison, while every changed/new value still validates.
        if cached_flags[index] ~= value then
            if value ~= true then return nil end
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

-- TextScene.commit and render mutate live draws (typewriter flags and reveal
-- caches). Histories share only detached semantic records, never those draws.
-- Reuse unchanged records across the 64 snapshots; copying every draw and
-- replay option on every click would retain an entire page per history entry.
-- Weak source keys let old live pages die while retained values remain usable.
local draw_cache = setmetatable({}, {__mode="k"})
local page_cache = setmetatable({}, {__mode="k"})
local options_cache = setmetatable({}, {__mode="k"})
local color_cache = setmetatable({}, {__mode="k"})
local DRAW_FIELDS = {"kind","text","ruby","x","y","r","g","b","a","group",
    "scale","bold","italic","strike","typewriter","layout_width","_page_src"}
local PAGE_FIELDS = {"kind","src","scene","speaker"}
local OPTION_FIELDS = {"nvl","pos","msgX","msgY","nameX","lineHeight","maxWidth","font_size"}
local COLOR_FIELDS = {1,2,3,4,"r","g","b","a"}
local STATE_FIELDS = {"line","char_offset","opacity","cursor_x","cursor_y","reveal_chars",
    "font_size","font_face","font_color","last_action"}

local function captured_fields(source, cache, fields, child_key, child)
    if type(source)~="table" then return source end
    local previous=cache[source]
    local equal=previous~=nil and (not child_key or previous[child_key]==child)
    if equal then
        for _,key in ipairs(fields) do
            if previous[key]~=source[key] then equal=false; break end
        end
    end
    if equal then return previous end
    local value={}
    for _,key in ipairs(fields) do value[key]=deep_copy(source[key]) end
    if child_key then value[child_key]=child end
    cache[source]=value
    return value
end

local function capture_page(source)
    if type(source)~="table" then return source end
    local opts=source.opts
    if type(opts)=="table" then
        local color=captured_fields(opts.color,color_cache,COLOR_FIELDS)
        opts=captured_fields(opts,options_cache,OPTION_FIELDS,"color",color)
    end
    return captured_fields(source,page_cache,PAGE_FIELDS,"opts",opts)
end

local function copy_text_state(state)
    if type(state) ~= "table" then return state end
    local out = {draws={},page_src={}}
    for _,key in ipairs(STATE_FIELDS) do out[key]=deep_copy(state[key]) end
    for i,draw in ipairs(state.draws or {}) do
        out.draws[i]=captured_fields(draw,draw_cache,DRAW_FIELDS)
    end
    for i,source in ipairs(state.page_src or {}) do
        out.page_src[i]=capture_page(source)
    end
    return out
end

--- snapshot.capture(ctx) → snap | nil
function snapshot.capture(ctx)
    if type(ctx) ~= "table" then return nil end
    for _, tween in ipairs(ctx.tweens or {}) do
        if not tween.done and not tween.cancelled then return nil, "tween-active" end
    end
    local text_state = require("kag.text_scene").get_state(ctx)
    local layers,font = require("kag.rollback_presentation").capture()
    local snap = {
        scene = ctx.current_scene or ctx.currentScene or "",
        token_index = ctx.token_index,
        -- Completed [ch]/[text] already advanced _resume_index. An explicit
        -- [p] is suspended inside its handler and must re-enter that wait so
        -- the next click still performs its page-clear continuation.
        resume_index = ctx._executing_index or ctx._resume_index or ctx.token_index,
        resume_page_wait = ctx._executing_command == "p",
        call_stack = deep_copy(ctx.call_stack),
        control = require("kag.save_state").capture_control(ctx),
        _seen_blocks = pack_seen(ctx.seen_scenes),
        backlog_len = type(ctx.backlog) == "table" and #ctx.backlog or 0,
        text_speed = ctx.text_speed,
        skip_mode = ctx.skip_mode,
        auto_mode = ctx.auto_mode,
        nvl_mode = ctx.nvl_mode,
        nvl_prefix_fmt = ctx.nvl_prefix_fmt,
        nvl_hidden_vis = deep_copy(ctx.nvl_hidden_vis),
        current_speaker = ctx.current_speaker,
        waiting_input = ctx.waiting_input,
        textbox_style = deep_copy(ctx.textbox_style),
        nameplate_style = deep_copy(ctx.nameplate_style),
        text_state = copy_text_state(text_state),
        reveal = (type(ctx.reveal) == "table") and {
            total = ctx.reveal.total,
            elapsed = (ctx.reveal.total or 0) * math.max(0, tonumber(ctx.text_speed) or 50),
            -- [typewriter sound] (t201): restore marks the whole line
            -- revealed (no typewriter replay); seal the SE boundary at
            -- total so a rollback cannot fire a burst of SEs.
            last_shown = ctx.reveal.total or 0,
        } or nil,
        layers = layers,
        font = font,
    }
    if not snap._seen_blocks then snap.seen_scenes = deep_copy(ctx.seen_scenes) end
    for _, k in ipairs(DEEP_COPY_KEYS) do
        snap[k] = deep_copy(ctx[k])
    end
    for _, k in ipairs(EMPTY_DEFAULT_KEYS) do
        snap[k] = deep_copy(ctx[k]) or {}
    end
    for _, k in ipairs(REF_KEYS) do
        snap[k] = ctx[k] -- macro stream edits establish a history barrier first
    end
    return snap
end

local function prepare_textbox_style(style)
    if style==nil then return nil end
    if type(style)~="table" then error("Invalid rollback textbox style",0) end
    local schema=require("kag.schema")
    if not schema.specs("textbox") then require("kag.commands.text") end
    local value=deep_copy(style)
    for key,spec in pairs(schema.specs("textbox")) do
        local field=value[key]
        if type(field)~=spec.type or (spec.type=="number" and (field~=field
            or math.abs(field)==math.huge or (spec.min and field<spec.min)
            or (spec.max and field>spec.max))) then
            error("Invalid rollback textbox style."..key,0)
        end
    end
    return value
end

-- A candidate owns all mutable restored values and temporary resource tickets.
-- Preparing it must finish before the runner retires its current coroutine.
function snapshot.prepare(ctx, snap)
    if type(ctx) ~= "table" or type(snap) ~= "table" then error("Invalid rollback snapshot",0) end
    local values={
        scene=snap.scene,token_index=snap.token_index or 1,
        resume_index=snap.resume_index or snap.token_index or 1,
        call_stack=deep_copy(snap.call_stack),control=deep_copy(snap.control) or {},
        seen_scenes=snap._seen_blocks and unpack_seen(snap._seen_blocks)
            or deep_copy(snap.seen_scenes) or {},
        text_speed=snap.text_speed,skip_mode=snap.skip_mode,auto_mode=snap.auto_mode,
        nvl_mode=snap.nvl_mode,nvl_prefix_fmt=snap.nvl_prefix_fmt,waiting_input=snap.waiting_input,
        nvl_hidden_vis=deep_copy(snap.nvl_hidden_vis),current_speaker=snap.current_speaker,
        textbox_style=prepare_textbox_style(snap.textbox_style),
        nameplate_style=deep_copy(snap.nameplate_style),
        reveal=deep_copy(snap.reveal),
    }
    for _,k in ipairs(DEEP_COPY_KEYS) do values[k]=deep_copy(snap[k] or ctx[k]) end
    for _,k in ipairs(EMPTY_DEFAULT_KEYS) do values[k]=deep_copy(snap[k]) or {} end
    for _,k in ipairs(REF_KEYS) do values[k]=snap[k] or ctx[k] end
    -- Truncation itself belongs to the commit. Copying this retained prefix
    -- here prevents a preparation failure from changing shared backlog data.
    if type(values.backlog)=="table" then
        local entries={}
        local count=math.min(#values.backlog,snap.backlog_len or #values.backlog)
        for i=1,count do entries[i]=deep_copy(values.backlog[i]) end
        values.backlog=entries
    end
    if values.reveal then
        values.reveal.elapsed=(values.reveal.total or 0)*math.max(0,tonumber(values.text_speed) or 50)
        values.reveal.last_shown=values.reveal.total or 0
    end
    local text
    if snap.text_state then
        text=require("kag.text_scene").prepare_restore({state=snap.text_state,
            reveal=values.reveal,text_speed=values.text_speed,waiting_input=values.waiting_input})
        if text.reveal then text.state.reveal_chars=text.reveal.total end
    end
    local prepared={owner=ctx,values=values,text=text,used=false}
    prepared.presentation=require("kag.rollback_presentation").prepare(snap.layers,snap.font)
    return prepared
end

function snapshot.discard(prepared)
    if type(prepared)~="table" then return true end
    prepared.used=true
    if prepared.presentation then return require("kag.rollback_presentation").discard(prepared.presentation) end
    return true
end

local function apply_values(ctx,snap,text)

    ctx.current_scene = snap.scene
    ctx.currentScene = snap.scene
    ctx.label_index = nil  -- security: a rollback across a [call] span must
    -- not reuse the callee's label index (stale cross-scene jump hazard)
    ctx.token_index = snap.token_index or 1
    ctx._resume_index = snap.resume_index or ctx.token_index
    ctx._executing_index, ctx._executing_command = nil, nil
    ctx.call_stack = snap.call_stack
    -- Use the same six control fields as SaveState.capture_control. A new
    -- scheduler must bind these historical tables, never the future frame or
    -- a stale load-resume marker. The retained snapshot remains independent.
    local control = snap.control
    ctx._forStack, ctx._whileStack = control.for_ or {}, control.while_ or {}
    ctx._ifStack, ctx._switchStack = control.if_ or {}, control.switch or {}
    ctx._forStackMarks, ctx._forRewound = control.for_marks or {}, control.for_rewound or {}
    ctx._resumeLoopStacks = nil
    ctx.seen_scenes = snap.seen_scenes
    ctx.text_speed = snap.text_speed
    ctx.skip_mode = snap.skip_mode
    ctx.auto_mode = snap.auto_mode
    ctx.nvl_mode = snap.nvl_mode
    ctx.nvl_prefix_fmt = snap.nvl_prefix_fmt
    ctx.nvl_hidden_vis = snap.nvl_hidden_vis
    ctx.current_speaker = snap.current_speaker
    ctx.waiting_input = snap.waiting_input
    ctx.textbox_style = snap.textbox_style -- absent history clears a future style
    ctx.nameplate_style = snap.nameplate_style

    for _, k in ipairs(DEEP_COPY_KEYS) do
        ctx[k] = snap[k]
    end
    for _, k in ipairs(EMPTY_DEFAULT_KEYS) do
        ctx[k] = snap[k]
    end
    for _, k in ipairs(REF_KEYS) do
        if snap[k] ~= nil then ctx[k] = snap[k] end
    end

    -- Text was validated and cloned before any resources or live values moved.
    ctx.reveal = snap.reveal
    if text then require("kag.text_scene").apply_restore(ctx,text) end
end

function snapshot.apply(ctx,prepared)
    if type(prepared)~="table" or prepared.owner~=ctx then error("Invalid rollback candidate owner",0) end
    if prepared.used then error("Rollback candidate already consumed",0) end
    prepared.used=true
    assert(require("kag.rollback_presentation").apply(prepared.presentation,ctx))
    apply_values(ctx,prepared.values,prepared.text)

    -- Audio: stop the voice line (SE/BGM cannot be un-played; documented).
    local backend = require("backend")
    if backend.audio_stop then
        local stopped, reason = backend.audio_stop("voice")
        if stopped == false then error(reason or "Rollback voice stop failed", 0) end
    end
    return true
end

--- Direct callers retain restore(ctx,snap); runner can supply a prepared candidate.
function snapshot.restore(ctx,snap,prepared)
    if type(ctx)~="table" or type(snap)~="table" then return false end
    return snapshot.apply(ctx,prepared or snapshot.prepare(ctx,snap))
end

return snapshot
