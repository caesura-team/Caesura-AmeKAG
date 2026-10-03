-- =============================================================================
--  Caesura (AmeKAG) — kag/commands/character.lua
--  Phase 4: KAG3 character convenience tags — [csp], [csd], [csl]
--  Character show / delete / move commands layered on top of the layer tree
--  (a KAG3-compat veneer over the underlying [image]/[position]/[moveto]
--  layer pipeline, per spec [2.1]).
--
--  Semantics (KAG3 compatibility):
--    [csp name=chara layer=0 x=320 y=240]  show a character: ensure the layer
--        exists and is visible, assign assets/char/<name>.png (or an explicit
--        storage/file/path override, mirroring [image] path resolution) as the
--        layer image, and set its position.
--    [csd name=chara layer=0]  hide/clear the character on the layer (mirrors
--        [cl]): the layer is made invisible and its texture is dropped.
--    [csl name=chara layer=0 x=340 y=240]  move a character layer to a new
--        position without changing its visibility (mirrors [moveto]/[position]
--        positioning via Layers.move_layer).
--
--  All calls route through layers.lua + backend.lua, exactly like the layer
--  commands. Layer names are normalized to strings (both numeric and named
--  forms are accepted: "0" vs 0 vs "fg0").
-- =============================================================================

local backend = require("backend")
local layers  = require("layers")

-- -----------------------------------------------------------------------------
--  Neo-Genesis contracts: typed + clamped via kag/schema. Category "layer"
--  groups these with the underlying layer commands; blocking=false (none of
--  these wait on completion).
-- -----------------------------------------------------------------------------

local schema = require("kag.schema")
schema.define("csp", {
    _meta = { category = "layer", blocking = false,
        desc = "KAG3-compatible csp command: show a character image on a layer (default assets/char/<name>.png at 0,0)" },
    name    = { type = "string", required = true, positional_index = 1 },  -- character id / asset stem
    layer   = { type = "string", default = "0" },
    x       = { type = "number", default = 0 },
    y       = { type = "number", default = 0 },
    storage = { type = "file" },   -- optional image-style path override
    file    = { type = "file" },
    path    = { type = "string" },
})
schema.define("csd", {
    _meta = { category = "layer", blocking = false,
        desc = "KAG3-compatible csd command: hide/remove a character on a layer" },
    name    = { type = "string", required = true, positional_index = 1 },
    layer   = { type = "string", default = "0" },
})
schema.define("csl", {
    _meta = { category = "layer", blocking = false,
        desc = "KAG3-compatible csl command: move a character layer (no visibility change)" },
    name    = { type = "string", required = true, positional_index = 1 },
    layer   = { type = "string", default = "0" },
    x       = { type = "number", default = 0 },
    y       = { type = "number", default = 0 },
})
schema.define("live2d_motion", {
    _meta = { category = "character", blocking = false,
        desc = "Play a Live2D motion animation on a model with optional fade times" },
    model   = { type = "string", required = true, positional_index = 1 },
    motion  = { type = "string", required = true, positional_index = 2 },
    fadein  = { type = "number", default = 500, min = 0, max = 10000 },
    fadeout = { type = "number", default = 500, min = 0, max = 10000 },
})
schema.define("live2d_expression", {
    _meta = { category = "character", blocking = false,
        desc = "Set a Live2D facial expression on a model" },
    model      = { type = "string", required = true, positional_index = 1 },
    expression = { type = "string", required = true, positional_index = 2 },
    weight     = { type = "number", default = 1.0, min = 0.0, max = 1.0 },
})
schema.define("live2d_lip_sync", {
    _meta = { category = "character", blocking = false,
        desc = "Control a loaded Live2D mouth manually, from VOICE PCM, or turn automatic control off" },
    model   = { type = "string", required = true, positional_index = 1 },
    source  = { type = "enum", values = {"manual", "voice", "off"}, default = "manual" },
    -- Omission must remain distinguishable from explicit value=0 for voice/off.
    value   = { type = "number", positional_index = 2, min = 0.0, max = 1.0 },
})
schema.define("live2d_load", {
    _meta = { category = "character", blocking = false, desc = "Load a Live2D model owned by the current scene context" },
    model = { type = "string", required = true, positional_index = 1 },
    storage = { type = "file", required = true, positional_index = 2 },
})
schema.define("live2d_show", {
    _meta = { category = "character", blocking = false, desc = "Show a loaded Live2D model" },
    model = { type = "string", required = true, positional_index = 1 },
    x = { type = "number", default = 0 },
    y = { type = "number", default = 0 },
    scale = { type = "number", default = 1, min = 0.001 },
})
for _,command in ipairs({"live2d_hide", "live2d_unload"}) do
    schema.define(command, {
        _meta = { category = "character", blocking = false, desc = "Hide or unload a context-owned Live2D model" },
        model = { type = "string", required = true, positional_index = 1 },
    })
end

local CharacterCommands = {}

-- Internal: resolve the character image path. Mirrors [image]'s resolve_file
-- (storage > path > file override) plus mod resolution; when no explicit
-- path is given, falls back to the KAG3 convention assets/char/<name>.png.
local function resolve_character_file(params, chara)
    local f = params.storage or params.path or params.file
    if type(f) ~= "string" or #f == 0 then
        f = "assets/char/" .. tostring(chara) .. ".png"
    end
    -- Mod resolution: enabled mods may override base assets
    -- (mods/<name>/<path>); falls back to the base path.
    if type(f) == "string" and #f > 0 then
        f = require("mods").resolve(f)
    end
    return f
end

-- Internal: resolve a layer node by name (numeric or string form), creating
-- it under the root if missing — same "ensure" semantics as layer.lua's
-- get_or_create_layer, with id=name so both Layers.get and Layers.find(tag)
-- resolve it. Character layers default to size 0 (the RTT/layout is driven
-- by the image once assigned).
local function get_or_create_layer(layerName)
    local node = layers.find(layerName) or layers.get(layerName)
    if not node then
        local root = layers.get_root()
        node = layers.add_layer(root, {
            name = layerName, id = layerName, tag = layerName,
            z = 1, x = 0, y = 0, w = 0, h = 0, visible = true,
        })
    end
    return node
end

-- [csp name=chara layer=0 x=320 y=240]
-- Show a character: ensure the layer exists and is visible, assign the
-- resolved image, and place it at (x, y). Re-showing the same layer with a
-- different name updates the image.
function CharacterCommands.csp(ctx, params)
    local chara = params.name or params[1]
    if not chara then
        print("[CharCmd] csp: chara name required")
        return
    end
    local layerName = tostring(params.layer)
    local file     = resolve_character_file(params, chara)

    local tex = backend.load_texture(file)
    if not tex then
        print("[CharCmd] csp: failed to load " .. file)
        return
    end

    local node = get_or_create_layer(layerName)
    layers.set_layer_image(node, tex, nil, nil, nil, nil)
    layers.set_layer_visible(node, true)
    layers.move_layer(node, params.x or 0, params.y or 0)

    ctx.layers = ctx.layers or {}
    ctx.layers[layerName] = file
    ctx.characters = ctx.characters or {}
    ctx.characters[layerName] = { chara = chara, file = file }
end

-- [csd name=chara layer=0]
-- Hide/remove the character on the layer. Mirrors [cl] (hide) plus [ld]'s
-- texture drop so the layer no longer carries a stale image; the dedup state
-- is cleared so the next [csp] re-asserts cleanly.
function CharacterCommands.csd(ctx, params)
    local layerName = tostring(params.layer)
    local node = layers.find(layerName) or layers.get(layerName)
    if node then
        layers.set_layer_visible(node, false)
        node.tex = nil
        node.texture = nil
    end
    if ctx.layers then ctx.layers[layerName] = nil end
    if ctx.characters then ctx.characters[layerName] = nil end
end

-- [csl name=chara layer=0 x=340 y=240]
-- Move a character layer to (x, y) WITHOUT changing its visibility — mirrors
-- [moveto]/[position] positioning via Layers.move_layer. No-op (with a
-- diagnostic) when the layer has not been shown yet.
function CharacterCommands.csl(ctx, params)
    local layerName = tostring(params.layer)
    local node = layers.find(layerName) or layers.get(layerName)
    if not node then
        print("[CharCmd] csl: layer not shown: " .. layerName)
        return
    end
    layers.move_layer(node, params.x or 0, params.y or 0)
end

-- [live2d_motion model=name motion=tap_body fadein=500 fadeout=500]
function CharacterCommands.live2d_motion(ctx, params)
    local model = params.model or params[1]
    local motion = params.motion or params[2]
    if not model or not motion then return end
    ctx.live2d = ctx.live2d or {}
    ctx.live2d[model] = ctx.live2d[model] or {}
    ctx.live2d[model].current_motion = motion
    ctx.live2d[model].fadein = params.fadein or 500
    ctx.live2d[model].fadeout = params.fadeout or 500
end

-- [live2d_expression model=name expression=smile weight=1.0]
function CharacterCommands.live2d_expression(ctx, params)
    local model = params.model or params[1]
    local expr = params.expression or params[2]
    if not model or not expr then return end
    ctx.live2d = ctx.live2d or {}
    ctx.live2d[model] = ctx.live2d[model] or {}
    ctx.live2d[model].expression = expr
    ctx.live2d[model].expression_weight = params.weight or 1.0
end

-- Native handles are private transient ownership, never persistent scene values.
local function model_name(params)
    local name = params.model or params[1]
    if type(name) ~= "string" or name == "" then error("Live2D model name is required", 0) end
    return name
end

local function model_handle(ctx, params)
    local name = model_name(params)
    local handle = ctx._live2dHandles and ctx._live2dHandles[name]
    if not handle then error("Live2D model is not loaded in this context: " .. name, 0) end
    return handle, name
end

local function applied(ok, reason)
    if ok ~= true then error("Live2D operation failed: " .. tostring(reason or "unsupported model or backend"), 0) end
end

function CharacterCommands.live2d_load(ctx, params)
    local name = model_name(params)
    if ctx._live2dHandles and ctx._live2dHandles[name] then
        error("Live2D model name is already loaded: " .. name, 0)
    end
    local handle, reason = backend.live2d_load(params.storage or params[2], name)
    if type(handle) ~= "number" or handle <= 0 or handle % 1 ~= 0 then
        error("Live2D load failed: " .. tostring(reason or "invalid handle"), 0)
    end
    ctx._live2dHandles = ctx._live2dHandles or {}
    ctx._live2dHandles[name] = handle
    ctx.live2d = ctx.live2d or {}
    ctx.live2d[name] = {storage = params.storage or params[2]}
end

function CharacterCommands.live2d_show(ctx, params)
    local handle = model_handle(ctx, params)
    applied(backend.live2d_show(handle, params.x or 0, params.y or 0, params.scale or 1))
end

function CharacterCommands.live2d_hide(ctx, params)
    local handle = model_handle(ctx, params)
    applied(backend.live2d_hide(handle))
end

function CharacterCommands.live2d_unload(ctx, params)
    local handle, name = model_handle(ctx, params)
    applied(backend.live2d_unload(handle))
    ctx._live2dHandles[name] = nil
    if ctx.live2d then ctx.live2d[name] = nil end
end

function CharacterCommands.live2d_lip_sync(ctx, params)
    local source = params.source or "manual"
    local value = params.value or params[2]
    local value_provided = params.value ~= nil or params[2] ~= nil
    -- The shared schema treats an empty string as omitted. The scheduler's
    -- original normalized token still records explicit value="", which must
    -- also be rejected for voice/off instead of silently changing its meaning.
    if ctx._executing_command == "live2d_lip_sync" and ctx.tokens then
        local token = ctx.tokens[ctx._executing_index]
        local raw = token and token[2]
        if raw then value_provided = value_provided or raw.value ~= nil or raw[2] ~= nil end
    end
    if source ~= "manual" and source ~= "voice" and source ~= "off" then
        error("Invalid Live2D lip sync source", 0)
    end
    if source ~= "manual" and value_provided then
        error("Live2D voice/off source cannot include a value", 0)
    end
    local handle, name = model_handle(ctx, params)
    if source == "manual" then
        -- The native operation disables automatic control before writing mouth.
        applied(backend.live2d_set_mouth(handle, value or 0))
    else
        applied(backend.live2d_set_voice_lipsync(handle, source == "voice"))
    end
    ctx.live2d = ctx.live2d or {}
    ctx.live2d[name] = ctx.live2d[name] or {}
    ctx.live2d[name].lip_sync_source = source
    ctx.live2d[name].lip_sync = source == "manual" and (value or 0) or nil
end

return CharacterCommands
