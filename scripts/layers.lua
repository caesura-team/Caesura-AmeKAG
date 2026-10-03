-- ═══════════════════════════════════════════════════════════════════════════
--  Caesura (AmeKAG) — layers.lua
--  Layer tree manager. Full implementation per spec [2.1].
--  Each layer supports: z-order, blend mode, visibility, opacity, position,
--  size, clipping, hit testing, dirty tracking, per-layer RTT.
--  Render pipeline: DFS traversal → z-order sort → blend child RTs →
--  submit via backend.submit_batch → flush to screen.
--  Batch submission uses one persistent positional array (zero per-frame
--  table allocation); see the submit_batch wire format above Layers.render.
--  Krkrz reference: LayerIntf.h, LayerManager.h, drawable.h (28 blend types)
-- ═══════════════════════════════════════════════════════════════════════════

local backend   = require("backend")
local rtt       = require("rtt")
local blend_lib = require("blend")

-- [R11-FIX] This Lua layer tree (7 types) operates ABOVE the C++ ILayerManager (3 slots).
-- Each Lua layer node gets its own RTT and bgfx viewport ID.
-- Final composition happens via backend.submit_batch() -> C++ IRenderDevice.
-- The C++ BG/FG/MSG slots are bypassed for the tree-based pipeline.
-- The tree-based approach provides independent per-layer RTTs, Z-sorting,
-- and dirty-rectangle propagation across child-parent boundaries.

local Layers = {}

-- ═══════════════════════════════════════════════════════════════════════════
--  LayerType — scene-layer role enum (spec [2.1])
-- ═══════════════════════════════════════════════════════════════════════════

Layers.Type = {
    LAYER_BASE    = 1,  -- 背景层
    LAYER_LAYER0  = 2,  -- 立绘层0
    LAYER_LAYER1  = 3,  -- 立绘层1
    LAYER_FORE    = 4,  -- 前景层
    LAYER_UI      = 5,  -- UI 层
    LAYER_MESSAGE = 6,  -- 消息层
    LAYER_EFFECT  = 7,  -- 特效层
}

Layers.TypeName = {
    [1] = "base", [2] = "layer0", [3] = "layer1",
    [4] = "fore", [5] = "ui", [6] = "message", [7] = "effect",
}

-- ═══════════════════════════════════════════════════════════════════════════
--  Internal state
-- ═══════════════════════════════════════════════════════════════════════════

local layerMap  = {}   -- id → LayerNode lookup
local rootNode  = nil  -- scene root
local nextView  = 1    -- bgfx View ID allocator
local freeViews = {}  -- recycled view IDs (returned on remove)
local maxView   = 256  -- bgfx View limit (bgfx caps at 256)

-- ═══════════════════════════════════════════════════════════════════════════
--  Node constructor
-- ═══════════════════════════════════════════════════════════════════════════

local function newLayerNode(id, config)
    config = config or {}
    return {
        id          = id,
        name        = config.name or config.tag or ("layer_" .. id),
        parent      = nil,
        children    = {},             -- sorted by z-order ascending
        layer_type  = config.layer_type or Layers.Type.LAYER_FORE,
        x           = config.x or 0,
        y           = config.y or 0,
        w           = config.w or 0,
        h           = config.h or 0,
        visible     = (config.visible == nil) or config.visible,
        opacity     = config.opacity or 255,
        blend_mode  = config.blend_mode or "alpha",
        rt          = config.rt or nil,      -- RTT handle
        view_id     = nil,                   -- bgfx View ID
        dirty       = true,
        dirty_rect  = nil,                   -- {x,y,w,h} sub-region
        z           = config.z or 0,
        -- visual state
        scaleX      = 1.0,  scaleY = 1.0,
        rotation    = 0,
        originX     = 0,    originY = 0,
        alpha       = (config.opacity or 255) / 255.0,
        -- clipping (pixels in parent space)
        clipX       = config.clipX, clipY = config.clipY,
        clipW       = config.clipW, clipH = config.clipH,
        -- image sub-rect
        imgX        = nil, imgY = nil, imgW = nil, imgH = nil,
        -- texture tracking for blend
        _prev_rt    = nil,
        -- vfx state
        quake       = { active = false, amplitude_x = 0, amplitude_y = 0 },
        shake       = { active = false, offset_x = 0, offset_y = 0 },
        fade        = { active = false, alpha = 255 },
        -- application data
        name        = config.name or nil,
        tag         = config.tag or nil,
        userdata    = config.userdata or nil,
    }
end

-- ═══════════════════════════════════════════════════════════════════════════
--  z-order helpers
-- ═══════════════════════════════════════════════════════════════════════════

local function sortChildrenByZ(node)
    if not node or not node.children then return end
    table.sort(node.children, function(a, b) return a.z < b.z end)
end

local function insertSortedByZ(children, node)
    local idx = 1
    while idx <= #children and (children[idx].z or 0) <= (node.z or 0) do
        idx = idx + 1
    end
    table.insert(children, idx, node)
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Layers.init() — reset the tree
-- ═══════════════════════════════════════════════════════════════════════════

function Layers.init()
    layerMap = {}
    rootNode = nil
    nextView = 1
    freeViews = {}  -- clear recycled view ids on reset (duplicate-handout guard)
    Layers.get_root()
end

-- A restore commit retires the entire old tree. Clear lookup ownership first,
-- then try every distinct RTT even if a backend cleanup fails. Path-cached
-- textures remain TextureManager-owned; the restore module tracks its own IDs.
function Layers.clear_for_restore()
    local previous = layerMap
    Layers.init()
    local released, errors = {}, {}
    for _, node in pairs(previous) do
        for _, field in ipairs({"rt", "_prev_rt"}) do
            local handle = node[field]
            node[field] = nil
            if type(handle) == "number" and handle > 0 and not released[handle] then
                released[handle] = true
                local ok, err = pcall(rtt.destroy, handle)
                if not ok then errors[#errors + 1] = tostring(err) end
            end
        end
        node.tex, node.texture, node.view_id = nil, nil, nil
    end
    return #errors == 0, table.concat(errors, "; ")
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Layers.get_root() → root LayerNode (lazy init)
-- ═══════════════════════════════════════════════════════════════════════════

function Layers.get_root()
    if not rootNode then
        rootNode = {
            id = "_root", parent = nil, children = {},
            layer_type = Layers.Type.LAYER_BASE,
            x = 0, y = 0, w = 0, h = 0,
            visible = true, opacity = 255,
            blend_mode = "opaque", z = -9999,
            rt = nil, view_id = 0,
            dirty = true, dirty_rect = nil,
            scaleX = 1.0, scaleY = 1.0, rotation = 0,
            alpha = 1.0, quake = {}, shake = {},
            tex = 0,
        }
        layerMap["_root"] = rootNode
    end
    return rootNode
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Layers.count() → integer
-- ═══════════════════════════════════════════════════════════════════════════

function Layers.count()
    local n = 0
    for _ in pairs(layerMap) do n = n + 1 end
    return n
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Layers.get_layer(id) → LayerNode | nil
-- ═══════════════════════════════════════════════════════════════════════════

function Layers.get_layer(id)
    return layerMap[id]
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Layers.find(pred) → LayerNode | nil
--    pred: function(node)→boolean, or string tag
-- ═══════════════════════════════════════════════════════════════════════════

function Layers.find(pred)
    if type(pred) == "string" then
        local tag = pred
        pred = function(n) return n.tag == tag end
    end
    for _, node in pairs(layerMap) do
        if pred(node) then return node end
    end
    return nil
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Layers.get(layer_name) → LayerNode | nil
--    Look up a layer by its name field (set at construction).
-- ═══════════════════════════════════════════════════════════════════════════

--  Layers.ensure(ctx, name, z) → LayerNode
--    Return the layer with the given name (or id), creating it under the root
--    if missing. Used by settings/gallery/music_room overlays; the caller then
--    sets size/position/texture.
function Layers.ensure(ctx, name, z)
    local node = Layers.get(name) or Layers.find(name) or Layers.get_layer(name)
    if node then
        if z ~= nil then node.z = z end
        return node
    end
    -- tag mirrors name so Layers.find(name) (which matches tags) works for
    -- hide(); without it the settings/gallery/music_room overlays could
    -- never be found, hidden, or destroyed (and reopen would leak view ids).
    return Layers.add_layer(Layers.get_root(), { name = name, id = name, tag = name, z = z or 0 })
end

function Layers.get(layer_name)
    if not layer_name then return nil end
    for _, node in pairs(layerMap) do
        if node.name == layer_name then return node end
    end
    return nil
end


-- ═══════════════════════════════════════════════════════════════════════════
--  Layers.forEach(fn) — iterate all layers
-- ═══════════════════════════════════════════════════════════════════════════

function Layers.forEach(fn)
    for id, node in pairs(layerMap) do
        fn(id, node)
    end
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Layers.pick(px, py) → hits[]
--    IDE preview-frame hit test (round 23 /api/pick). DFS over the node
--    tree; returns every visible node whose bounds contain (px, py),
--    ordered bottom-to-top by z. Approximation: scale/rotation/clip are
--    ignored (preview-picking tolerance); x/y/w/h are engine-space pixels.
-- ═══════════════════════════════════════════════════════════════════════════

function Layers.pick(px, py)
    local hits = {}
    local function walk(node, depth)
        if not node then return end
        if node.visible == false then return end
        if node.opacity ~= nil and node.opacity <= 0 then return end
        local x, y = node.x or 0, node.y or 0
        local w, h = node.w or 0, node.h or 0
        if w > 0 and h > 0 and px >= x and px <= x + w and py >= y and py <= y + h then
            hits[#hits + 1] = {
                id = node.id, name = node.name, z = node.z or 0, depth = depth,
                opacity = node.opacity or 255, x = x, y = y, w = w, h = h,
            }
        end
        for _, child in ipairs(node.children or {}) do
            walk(child, depth + 1)
        end
    end
    walk(Layers.get_root(), 0)
    table.sort(hits, function(a, b) return (a.z or 0) < (b.z or 0) end)
    return hits
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Layers.add_layer(parent, config) → LayerNode
--    Creates a new layer node under parent (defaults to root), allocates
--    an RTT, assigns a bgfx View ID, inserts in z-order, marks dirty.
-- ═══════════════════════════════════════════════════════════════════════════

function Layers.add_layer(parent, config)
    config = config or {}
    local id = config.id or ("layer_" .. tostring(Layers.count() + 1))
    parent = parent or Layers.get_root()

    local node = newLayerNode(id, config)
    node.parent = parent

    -- allocate RTT from the pool if size is known (lazy: visible-only
    -- allocation happens in render; a pooled handle is reused across
    -- add/remove cycles so GPU viewports are not churned)
    local nw = node.w or 0
    local nh = node.h or 0
    if nw > 0 and nh > 0 then
        node.rt = rtt.acquire(nw, nh)
    end

    -- assign bgfx View ID (recycled from the free-list; monotonic
    -- allocation would exhaust 256 views on repeated add/remove)
    if #freeViews > 0 then
        node.view_id = table.remove(freeViews)
    else
        if nextView >= maxView then
            error("[layers] bgfx View ID exhausted (max=" .. tostring(maxView) .. ")")
        end
        node.view_id = nextView
        nextView = nextView + 1
    end

    -- insert in z-order
    parent.children = parent.children or {}
    insertSortedByZ(parent.children, node)

    layerMap[id] = node
    Layers.mark_dirty(node)
    return node
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Layers.remove_layer(node) — recursive tree-aware removal
--    Also frees the RTT and clears the layerMap entry.
-- ═══════════════════════════════════════════════════════════════════════════

function Layers.remove_layer(node)
    if not node then return false end
    -- The root node has view_id 0 (truthy in Lua): recycling it would
    -- hand view 0 out twice. Compare by reference (id is "_root").
    if node == Layers.get_root() or node.view_id == 0 then
        print("[layers] refusing to remove the root layer")
        return false
    end

    -- detach from parent
    if node.parent and node.parent.children then
        for i, child in ipairs(node.parent.children) do
            if child == node then
                table.remove(node.parent.children, i)
                break
            end
        end
        Layers.mark_dirty(node.parent)
    end

    -- recursively remove children (copy list to avoid mutation during iteration)
    if node.children then
        local children = {}
        for _, c in ipairs(node.children) do table.insert(children, c) end
        for _, c in ipairs(children) do
            Layers.remove_layer(c)
        end
    end

    -- free RTT (pooled; release destroys when the bucket is full)
    if node.rt then
        rtt.release(node.rt, node.w or 0, node.h or 0)
        node.rt = nil
    end
    -- recycle the bgfx view id ALWAYS (rt-less layers never rendered
    -- must not leak view ids -- ~256 add/remove cycles would exhaust)
    if node.view_id then
        freeViews[#freeViews + 1] = node.view_id
        node.view_id = nil
    end

    layerMap[node.id] = nil
    return true
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Layers.set_z(node, z) — reorder and mark dirty
-- ═══════════════════════════════════════════════════════════════════════════

function Layers.set_z(node, z)
    if not node then return end
    node.z = z
    if node.parent and node.parent.children then
        sortChildrenByZ(node.parent)
    end
    Layers.mark_dirty(node)
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Layers.mark_dirty(node, rect) — mark node + all ancestors dirty
--    Called by transform, blend, vfx, and external mutations.
-- ═══════════════════════════════════════════════════════════════════════════

function Layers.mark_dirty(node, rect)
    if not node then return end
    node.dirty = true
    if rect then
        node.dirty_rect = { x = rect.x, y = rect.y, w = rect.w, h = rect.h }
    end
    -- propagate upward to root
    local p = node.parent
    while p do
        p.dirty = true
        p = p.parent
    end
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Internal: clear dirty flags recursively (post-render)
-- ═══════════════════════════════════════════════════════════════════════════

local function clearDirtyRecursive(node)
    if not node then return end
    node.dirty = false
    node.dirty_rect = nil
    -- `node.children or {}` would allocate a throwaway empty table for every
    -- childless node on every frame (this runs post-render across the whole
    -- tree). Guard instead of defaulting.
    local kids = node.children
    if kids then
        for i = 1, #kids do
            clearDirtyRecursive(kids[i])
        end
    end
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Layers.get_world_pos(node) → x, y — absolute screen coordinates
-- ═══════════════════════════════════════════════════════════════════════════

function Layers.get_world_pos(node)
    local x, y = node.x or 0, node.y or 0
    local p = node.parent
    while p do
        x = x + (p.x or 0)
        y = y + (p.y or 0)
        p = p.parent
    end
    return x, y
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Layers.get_world_rect(node) → {x,y,w,h} — absolute screen rect
-- ═══════════════════════════════════════════════════════════════════════════

function Layers.get_world_rect(node)
    local wx, wy = Layers.get_world_pos(node)
    return { x = wx, y = wy, w = node.w or 0, h = node.h or 0 }
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Layers.hit_test(x, y) → LayerNode
--    Walks the tree depth-first from the root, returning the deepest
--    visible leaf node whose bounding rect contains (x, y).
--    Higher z-order children are checked last (topmost → first hit).
-- ═══════════════════════════════════════════════════════════════════════════

function Layers.hit_test(x, y)
    local function hitRecursive(node, sx, sy)
        if not node or not node.visible then return nil end
        local nx = sx + (node.x or 0)
        local ny = sy + (node.y or 0)
        local nw = node.w or 0
        local nh = node.h or 0

        -- check children from topmost (highest z) to bottommost
        local children = node.children or {}
        for i = #children, 1, -1 do
            local hit = hitRecursive(children[i], nx, ny)
            if hit then return hit end
        end

        -- self hit: point must be inside bounding rect
        if nw > 0 and nh > 0 then
            if x >= nx and x < nx + nw and y >= ny and y < ny + nh then
                return node
            end
        end
        return nil
    end

    return hitRecursive(Layers.get_root(), 0, 0)
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Layers.emit_text(layer, content) — render text via backend
-- ═══════════════════════════════════════════════════════════════════════════

function Layers.emit_text(layer, content)
    if not layer or not content then return end
    local wr = Layers.get_world_rect(layer)
    backend.render_text(content, wr.x, wr.y)
    Layers.mark_dirty(layer)
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Layers.capture_snapshot() → table — deep-copy state (backlog / save)
-- ═══════════════════════════════════════════════════════════════════════════

function Layers.capture_snapshot()
    local snap = { layers = {} }
    for id, node in pairs(layerMap) do
        if id ~= "_root" then
            snap.layers[id] = {
                id         = node.id,
                x          = node.x,
                y          = node.y,
                w          = node.w,
                h          = node.h,
                visible    = node.visible,
                opacity    = node.opacity,
                alpha      = node.alpha,
                blend_mode = node.blend_mode,
                z          = node.z,
                scaleX     = node.scaleX,
                scaleY     = node.scaleY,
                rotation   = node.rotation,
                clipX      = node.clipX,
                clipY      = node.clipY,
                clipW      = node.clipW,
                clipH      = node.clipH,
                layer_type = node.layer_type,
                tag        = node.tag,
            }
        end
    end
    return snap
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Layers.restore_snapshot(snap) — restore from snapshot, mark dirty
-- ═══════════════════════════════════════════════════════════════════════════

function Layers.restore_snapshot(snap)
    if not snap or not snap.layers then return end
    for id, data in pairs(snap.layers) do
        local node = layerMap[id]
        if node then
            for k, v in pairs(data) do node[k] = v end
            node.dirty = true
        end
    end
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Layers.render() — main render pipeline (spec [2.1])
--
--  1. DFS traversal of the scene tree starting at root.
--  2. For each visible node with dirty flag:
--     a. Render children first (bottom-up composition).
--     b. Compose child RTs onto current node RT via blend mode.
--     c. Collect batch commands.
--  3. Submit the unified batch through backend.submit_batch.
--  4. Clear all dirty flags.
-- ═══════════════════════════════════════════════════════════════════════════

-- ═══════════════════════════════════════════════════════════════════════════
--  submit_batch wire format (t11) — POSITIONAL FLAT ARRAY
--
--  batch[1]            = command count N (integer)
--  batch[1 + (i-1)*16 + k] = field k of command i (1 <= i <= N, 1 <= k <= 16)
--
--    k= 1 view_id     k= 2 tex        k= 3 rt (0 = none)  k= 4 x
--    k= 5 y           k= 6 w          k= 7 h              k= 8 opacity 0..255
--    k= 9 blend mode  (NUMERIC id via blend.resolve, not a string)
--    k=10 scaleX      k=11 scaleY     k=12 rotation
--    k=13 clipX       k=14 clipY      k=15 clipW          k=16 clipH
--                                     (clipW/clipH <= 0 means "no clip")
--
--  Every slot is a number — never nil — so the array part stays contiguous and
--  the C++ reader uses lua_rawgeti (integer index, no string hashing) instead
--  of ~11 lua_getfield calls per command.
--
--  RESIDUE SAFETY: the array is module-scoped and reused every frame, so a
--  frame that emits fewer commands than its predecessor leaves stale numbers
--  in the tail. batch[1] is the ONLY authority on how much is live; the reader
--  must never use lua_rawlen. Tests assert this (test_layers_alloc.lua).
-- ═══════════════════════════════════════════════════════════════════════════

local BATCH_STRIDE = 16
local batchArray   = {}   -- persistent flat command array (zero per-frame alloc)

-- config is optional: layers.lua must stay loadable in sandboxes/benches where
-- the config module was never preloaded. Reading package.loaded directly is one
-- table index (the sandboxed require resolves nothing else anyway) — the old
-- code paid a pcall + require on EVERY frame, and requiring config at module
-- scope would run config.apply() before kag/init.lua is ready for it.
local function peekConfig()
    return package.loaded["config"]
end

function Layers.render()
    local root = Layers.get_root()
    if not root then return end

    local cmds  = batchArray
    local count = 0
    cmds[1] = 0

    local function renderNode(node, parent_wx, parent_wy)
        if not node or not node.visible then return end

        local use_x = (node.pos_x ~= nil) and node.pos_x or node.x
        local use_y = (node.pos_y ~= nil) and node.pos_y or node.y
        local wx = parent_wx + (use_x or 0)
        local wy = parent_wy + (use_y or 0)

        -- apply quake / shake offsets
        local qx, qy = 0, 0
        if node.quake and node.quake.active then
            qx = node.quake.offset_x or 0
            qy = node.quake.offset_y or 0
        end
        if node.shake and node.shake.active then
            qx = qx + (node.shake.offset_x or 0)
            qy = qy + (node.shake.offset_y or 0)
        end
        wx = wx + qx
        wy = wy + qy

        -- render children first (bottom → top). Guarded rather than
        -- `or {}`: leaf nodes are the majority and that idiom allocates one
        -- empty table per leaf per frame.
        local kids = node.children
        if kids then
            for i = 1, #kids do
                renderNode(kids[i], wx, wy)
            end
        end

        -- emit commands for visible nodes with tex/rt and view_id.
        -- Root-level nodes may have tex but no rt; include them.
        -- NOTE: emitted EVERY frame (not only when dirty): the backbuffer
        -- does not preserve content between presents, so a static layer
        -- that submitted once would vanish from frame 2 onward (blank
        -- scene / flicker). dirty stays in charge of lazy RTT allocation
        -- and per-frame re-composition work only.
        local nodeTex = node.tex or node.texture or 0
        -- Lazy RTT: a layer with content but no pooled rt (e.g. added with
        -- size 0 then resized at runtime, or texture-only until now) gets
        -- its viewport allocated here, once, from the pool -- invisible
        -- layers cost zero GPU memory until they actually render.
        if node.dirty and node.view_id and not node.rt and nodeTex and nodeTex ~= 0
            and (node.w or 0) > 0 and (node.h or 0) > 0 then
            node.rt = rtt.acquire(node.w, node.h)
        end
        if node.view_id and (node.rt or (nodeTex and nodeTex ~= 0)) then
            -- Positional write into the persistent flat array (see the wire
            -- format above). No table is constructed per command per frame.
            local base = 1 + count * BATCH_STRIDE
            cmds[base + 1]  = node.view_id
            cmds[base + 2]  = nodeTex
            cmds[base + 3]  = node.rt or 0
            cmds[base + 4]  = wx
            cmds[base + 5]  = wy
            cmds[base + 6]  = node.w or 0
            cmds[base + 7]  = node.h or 0
            cmds[base + 8]  = node.opacity or 255
            cmds[base + 9]  = blend_lib.resolve(node.blend_mode or "alpha")
            cmds[base + 10] = node.scale or node.scaleX or 1.0
            cmds[base + 11] = node.scale or node.scaleY or 1.0
            cmds[base + 12] = node.rotation or 0
            cmds[base + 13] = node.clipX or 0
            cmds[base + 14] = node.clipY or 0
            cmds[base + 15] = node.clipW or 0
            cmds[base + 16] = node.clipH or 0
            count = count + 1
        end
    end

    renderNode(root, 0, 0)

    -- Header LAST: the count is the only live-length authority, so the tail of
    -- the reused array (stale numbers from a busier previous frame) is never
    -- read. Writing it after traversal also makes a partial traversal harmless.
    cmds[1] = count

    if count > 0 then
        backend.submit_batch(cmds)
    end

    -- Keep the old presented scene during the scheduler's normal token yield
    -- (and debugger pauses), until the exact adjacent [trans] takes ownership.
    local transition = package.loaded["transition"]
    if transition and transition.render_pending then transition.render_pending() end

    -- Accessibility color filter (Neo-Genesis): config.accessibility
    -- color_filter = "deuteranopia"|"protanopia"|"tritanopia"|"grayscale"
    -- |"high_contrast" applies a full-screen matrix pass (VFX effect 4)
    -- over the composited scene each frame. The preset matrix is synced
    -- to C++ on every frame (cheap table write) so config changes apply
    -- immediately; UI text drawn later via render_text is unaffected.
    -- peekConfig (module scope) replaced a per-frame pcall(require, "config"):
    -- layers.render must stay usable in sandboxes/benches where the config
    -- module was never preloaded (the filter simply stays off).
    local config = peekConfig()
    local cf = config and config.accessibility and config.accessibility.color_filter
    if cf and cf ~= "none" then
        local target = root and root.rt
        if (not target or target == 0) then
            -- fallback: topmost visible leaf RT (scene content)
            local bestZ, bestRt = -1, 0
            local function walk(n)
                if not n or n.visible == false then return end
                if n.rt and n.rt > 0 and (n.z or 0) > bestZ
                   and not n.children then
                    bestZ, bestRt = n.z or 0, n.rt
                end
                for _, c in ipairs(n.children or {}) do walk(c) end
            end
            walk(root)
            target = bestRt
        end
        if target and target > 0 then
            pcall(backend.set_color_filter, cf)
            pcall(backend.submit_vfx, target, 4, 1.0, 0, 0, 0, 0, 0, 0)
        end
    end

    clearDirtyRecursive(root)
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Layers.compose_children(node) — blend child RTs into node RT
--    Called internally after children are rendered.
-- ═══════════════════════════════════════════════════════════════════════════

function Layers.compose_children(node)
    if not node or not node.children then return end
    for _, child in ipairs(node.children) do
        if child.dirty and child.rt and node.rt then
            blend_lib.blend_into(
                node.rt,
                child.rt,
                child.blend_mode or "alpha",
                child.opacity or 255,
                child.x or 0,
                child.y or 0,
                child.w or 0,
                child.h or 0,
                child.view_id
            )
        end
    end
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Layers.set_layer_image(node, tex_id, ix, iy, iw, ih)
--    Assign a texture to a layer. Auto-set size from image rect.
-- ═══════════════════════════════════════════════════════════════════════════

function Layers.set_layer_image(node, tex_id, ix, iy, iw, ih)
    if not node then return end
    node.tex = tex_id  -- texture id (separate from RTT handle)
    node.imgX = ix; node.imgY = iy; node.imgW = iw; node.imgH = ih
    if iw and ih then
        node.w = iw
        node.h = ih
    end
    Layers.mark_dirty(node)
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Layers.set_layer_visible(node, visible)
-- ═══════════════════════════════════════════════════════════════════════════

function Layers.set_layer_visible(node, visible)
    if not node then return end
    if node.visible ~= visible then
        node.visible = visible
        Layers.mark_dirty(node.parent or node)
    end
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Layers.set_layer_opacity(node, opacity_0_255)
-- ═══════════════════════════════════════════════════════════════════════════

function Layers.set_layer_opacity(node, opacity)
    if not node then return end
    node.opacity = math.max(0, math.min(255, tonumber(opacity) or 255))
    node.alpha = node.opacity / 255.0
    Layers.mark_dirty(node)
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Layers.fade_to(node, target_opacity, duration_ms) — frame-stepped opacity
--  transition (D2.6). In coroutine context each yield consumes the scheduler
--  frame delta (ms); outside coroutines it steps at a 16 ms default.
-- ═══════════════════════════════════════════════════════════════════════════

function Layers.fade_to(node, target_opacity, duration_ms)
    if not node then return end
    duration_ms = tonumber(duration_ms)
    if not duration_ms or duration_ms <= 0 then
        Layers.set_layer_opacity(node, target_opacity)
        return
    end

    local from = tonumber(node.opacity) or 255
    local to = math.max(0, math.min(255, tonumber(target_opacity) or 255))
    local elapsed_ms = 0
    local is_coroutine = coroutine.isyieldable()
    while elapsed_ms < duration_ms do
        local delta_ms = 16
        if is_coroutine then delta_ms = tonumber(coroutine.yield()) or 16 end
        elapsed_ms = elapsed_ms + math.max(delta_ms, 0)
        local t = math.min(1, elapsed_ms / duration_ms)
        Layers.set_layer_opacity(node, from + (to - from) * t)
    end
    Layers.set_layer_opacity(node, to)
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Layers.set_layer_blend(node, blend_mode)
-- ═══════════════════════════════════════════════════════════════════════════

function Layers.set_layer_blend(node, blend_mode)
    if not node then return end
    node.blend_mode = blend_mode
    Layers.mark_dirty(node)
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Layers.move_layer(node, x, y) — absolute position; dirties the node
-- ═══════════════════════════════════════════════════════════════════════════

function Layers.move_layer(node, x, y)
    if not node then return end
    if x ~= nil then node.x = x end
    if y ~= nil then node.y = y end
    Layers.mark_dirty(node)
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Layers.resize_layer(node, w, h) — resize and reallocate RTT
-- ═══════════════════════════════════════════════════════════════════════════


-- ═══════════════════════════════════════════════════════════════════════════
--  Layers.set_position(layer_name, x, y, scale, unit)
--    Set layer position. x,y are NDC [0-1] by default, or pixel when
--    unit="px" (assumes 1280×720 reference resolution).
-- ═══════════════════════════════════════════════════════════════════════════

function Layers.set_position(layer_name, x, y, scale, unit)
    if unit == "px" then
        x = x / 1280
        y = y / 720
    end
    local l = Layers.get(layer_name)
    if l then
        l.pos_x = x; l.pos_y = y; l.scale = scale or 1.0
        Layers.mark_dirty(l)
    end
end

-- ═══════════════════════════════════════════════════════════════════════════
--  Layers.set_options(layer_name, opts)
--    Batch-set layer options: opacity (0.0-1.0), visible (bool), blend (string).
-- ═══════════════════════════════════════════════════════════════════════════

function Layers.set_options(layer_name, opts)
    local l = Layers.get(layer_name)
    if not l then return end
    if opts.opacity then
        local op = tonumber(opts.opacity)
        if op then
            -- opacity in [0.0, 1.0] range from KAG
            local a = math.max(0.0, math.min(1.0, op))
            l.opacity = math.floor(a * 255)
            l.alpha = a
        end
    end
    if opts.visible ~= nil then
        if l.visible ~= opts.visible then
            l.visible = opts.visible
        end
    end
    if opts.blend then
        l.blend_mode = opts.blend
    end
    Layers.mark_dirty(l)
end

-- =============================================================================
--  Layers.restore_text_state(text_state)
--  Restore text rendering position after save/load.
-- =============================================================================

function Layers.restore_text_state(text_state)
    if not text_state then return end
    if backend.text_set_state then
        backend.text_set_state(text_state.line or 1, text_state.char_offset or 0)
    end
end

function Layers.resize_layer(node, w, h)
    if not node then return end
    local ow, oh = node.w or 0, node.h or 0  -- old size for the pool key
    if w ~= nil then node.w = w end
    if h ~= nil then node.h = h end
    if node.rt then rtt.release(node.rt, ow, oh) end
    node.rt = rtt.acquire(node.w, node.h)
    Layers.mark_dirty(node)
end


-- ═══════════════════════════════════════════════════════════════════════════
--  [10.2.53] mark_dirty_with_transparency — recursive dirty marking
--  When a layer has a blend mode with transparency, lower layers
--  underneath may become visible. This marks the layer AND all
--  layers beneath it in z-order as dirty for the affected region.
-- ═══════════════════════════════════════════════════════════════════════════

function Layers.mark_dirty_with_transparency(layer, rect)
    if not layer then return end
    rect = rect or { x = layer.x, y = layer.y, w = layer.w, h = layer.h }
    layer.dirty = true
    layer.dirty_rect = rect

    -- Walk up to find all layers below this one in z-order
    local parent = layer.parent
    if not parent then return end

    for _, sibling in ipairs(parent.children) do
        if sibling == layer then break end  -- stop at/after current layer
        if not sibling.visible then goto continue_sib end
        -- Mark sibling as dirty in overlapping region
        sibling.dirty = true
        if not sibling.dirty_rect then
            sibling.dirty_rect = { x = rect.x, y = rect.y, w = rect.w, h = rect.h }
        else
            -- Union the dirty rects
            local dr = sibling.dirty_rect
            local nx1 = math.min(dr.x, rect.x)
            local ny1 = math.min(dr.y, rect.y)
            local nx2 = math.max(dr.x + dr.w, rect.x + rect.w)
            local ny2 = math.max(dr.y + dr.h, rect.y + rect.h)
            dr.x, dr.y = nx1, ny1
            dr.w, dr.h = nx2 - nx1, ny2 - ny1
        end
        ::continue_sib::
    end
end

--- Snapshot the layer tree for the Web player renderer (round 34).
--  Pure read: returns { name, tag, x, y, w, h, visible, opacity, z,
--  texture } for every registered layer (no binding calls).
function Layers.snapshot()
    local out = {}
    for _, node in pairs(layerMap) do
        out[#out + 1] = {
            name = node.name, tag = node.tag,
            x = node.x or 0, y = node.y or 0,
            w = node.w or 1280, h = node.h or 720,
            visible = node.visible ~= false,
            opacity = node.opacity or 1,
            z = node.z or 0,
            texture = node.texture,
        }
    end
    return out
end

return Layers
