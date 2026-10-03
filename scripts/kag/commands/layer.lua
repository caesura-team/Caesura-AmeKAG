-- =============================================================================
--  Caesura (AmeKAG) �� kag/commands/layer.lua
--  Phase 4: KAG layer tag handlers �� [bg], [fg], [cl], [image]
--  All calls route through layers.lua (spec [2.1]) + backend.lua.
-- =============================================================================

local backend = require("backend")
local layers  = require("layers")

-- Round 51 contract: [layfade] (audit: handler lacked a schema).
local _schema3 = require("kag.schema")
_schema3.define("layfade", {
    _meta = { category = "layer", blocking = true, desc = "fade one layer to an opacity" },
    layer = { type = "string", default = "bg", aliases = {"name"} },
    name = { type = "string" },
    to = { type = "number", default = 255, min = 0, max = 255 },
    opacity = { type = "number", min = 0, max = 255 },  -- legacy hybrid: <=1 fraction, >1 byte (handler converts)
    alpha   = { type = "number", min = 0, max = 255 },  -- alias (KAG3 name)
    time = { type = "number", default = 300, min = 0, max = 30000, aliases = {"duration"} },
    duration = { type = "number", default = 300, min = 0, max = 30000 },
})

local LayerCommands = {}

-- Internal: resolve file path (storage > path > file > positional)
local function resolve_file(params)
    -- string-only bare fallback (audit: pair table from named params
    -- must not reach the backend binding)
    local f = params.storage or params.path or params.file
    if type(f) ~= "string" and type(params[1]) == "string" then
        f = params[1]
    end
    -- Mod resolution: enabled mods may override base assets
    -- (mods/<name>/<path>); falls back to the base path.
    if type(f) == "string" and #f > 0 then
        f = require("mods").resolve(f)
    end
    return f
end

-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T
--  Internal: resolve layer node by name, or create if missing
-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T

local function get_or_create_layer(layerName, layerType)
    local node = layers.find(layerName)
    if not node then
        local root = layers.get_root()
        -- Full viewport size (logical resolution, default 1920x1080):
        -- scene layers must fill the render buffer at any resolution.
        -- Hardcoded 1280x720 left the scene as a top-left letterbox.
        local vw, vh = require("viewport").wh()
        node = layers.add_layer(root, {
            name = layerName, tag = layerName,
            z = (layerType == layers.Type.LAYER_BASE and 0 or 1),
            x = 0, y = 0, w = vw, h = vh, visible = true,
        })
    end
    return node
end

-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T
--  [bg storage="bg/school.png"]
--  Set background layer (z=0) texture.
-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T

-- Neo-Genesis contracts: typed + clamped via kag/schema.
local schema = require("kag.schema")
schema.define("cl", {
    _meta = { category = "layer", blocking = false, desc = "KAG3-compatible cl command" },
    layer = { type = "string", default = "all" },  -- unified clear entry
})
schema.define("bg", {
    _meta = { category = "layer", blocking = false, desc = "KAG3-compatible bg command" },
    storage = { type = "file" },
    file = { type = "file" },
    path = { type = "string" },
    layer = { type = "string", default = "bg" },
})
schema.define("fg", {
    _meta = { category = "layer", blocking = false, desc = "KAG3-compatible fg command" },
    storage = { type = "file" },
    file = { type = "file" },
    path = { type = "string" },
    layer = { type = "string", default = "fg" },
    clear = { type = "boolean", default = false },
})
schema.define("image", {
    _meta = { category = "layer", blocking = false, desc = "KAG3-compatible image command" },
    storage = { type = "file" },
    file = { type = "file" },
    layer = { type = "string", default = "fg" },
    x = { type = "number" },  -- no default: handler checks ~= nil
    y = { type = "number" },
    w = { type = "number", min = 0, max = 8192 },
    h = { type = "number", min = 0, max = 8192 },
})
schema.define("position", {
    _meta = { category = "layer", blocking = false, desc = "KAG3-compatible position command" },
    x = { type = "number", default = 0 },
    y = { type = "number", default = 0 },
    scale = { type = "number", default = 1.0, min = 0.01, max = 16 },
    layer = { type = "string", default = "fg", aliases = {"name"} },  -- KAG3 layer name
    name  = { type = "string", default = "" },
    pos   = { type = "string", default = "" },  -- left/center/right
})
schema.define("layopt", {
    _meta = { category = "layer", blocking = false, desc = "KAG3-compatible layopt command" },
    opacity = { type = "number", default = 1.0, min = 0, max = 1.0 },
    visible = { type = "boolean", default = true },
    layer   = { type = "string", default = "" },  -- KAG3 layer name
})
schema.define("fadeout", {
    _meta = { category = "layer", blocking = true, desc = "KAG3-compatible fadeout command" },
    layer = { type = "string", default = "bg", aliases = {"name"} },
    opacity = { type = "number", default = 0, min = 0, max = 1.0, aliases = {"alpha"} },
    alpha = { type = "number", default = 0, min = 0, max = 1.0 },
    time = { type = "number", default = 500, min = 0, max = 30000, aliases = {"duration"} },
    duration = { type = "number", default = 500, min = 0, max = 30000 },
})

function LayerCommands.bg(ctx, params)
    local file = resolve_file(params)
    if not file then
        print("[LayerCmd] bg: no file specified")
        return
    end
    -- Dedup: re-setting the SAME background reuses the loaded texture
    -- (common when scenes re-assert their bg after transitions) -- but
    -- visibility/z are ALWAYS re-applied so [layopt]/[ld] hiding the
    -- layer is still restored by the next [bg].
    local same = ctx.layers and ctx.layers.bg == file
    local tex
    if not same then
        tex = backend.load_texture(file)  -- (and/or would ALWAYS load)
    end
    if not same and not tex then
        print("[LayerCmd] bg: failed to load " .. file)
        return
    end

    local node = get_or_create_layer( "bg", layers.Type.LAYER_BASE)
    if tex then
        layers.set_layer_image(node, tex, nil, nil, nil, nil)
    end
    layers.set_layer_visible(node, true)
    layers.set_z( node, 0)

    ctx.layers = ctx.layers or {}
    ctx.layers.bg = file
end

-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T
--  [fg storage="chara/hero.png"]
--  Set foreground (character) layer (z=1) texture.
-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T

function LayerCommands.fg(ctx, params)
    local file = resolve_file(params)
    if not file then
        print("[LayerCmd] fg: no file specified")
        return
    end

    local tex = backend.load_texture(file)
    if not tex then
        print("[LayerCmd] fg: failed to load " .. file)
        return
    end

    local node = get_or_create_layer( "fg", layers.Type.LAYER_LAYER0)
    layers.set_layer_image(node, tex, nil, nil, nil, nil)
    layers.set_layer_visible(node, true)
    layers.set_z( node, 1)

    ctx.layers = ctx.layers or {}
    ctx.layers.fg = file
end

-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T
--  [cl layer="bg"|layer="fg"|layer="all"]
--  Clear specific layer(s). Default: clear all layers.
-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T

function LayerCommands.cl(ctx, params)
    local target = params.layer or "all"
    -- Neo-Genesis: re-apply the [textbox] style when the message window is
    -- cleared (the style persists across scenes; [cl] rebuilds it).
    if target == "all" or target == "message" then
        local style = ctx.textbox_style
        if style then
            local Text = require("kag.commands.text")
            if Text.textbox then Text.textbox(ctx, style) end
        end
    end

    if target == "all" or target == "bg" then
        local node = layers.find( "bg")
        if node then
            layers.set_layer_visible(node, false)
        end
        if ctx.layers then ctx.layers.bg = nil end
    end

    if target == "all" or target == "fg" then
        local node = layers.find( "fg")
        if node then
            layers.set_layer_visible(node, false)
        end
        if ctx.layers then ctx.layers.fg = nil end
    end
end

-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T
--  [image storage="chara/hero.png" layer="fg" x=200 y=100 opacity=255 blend="alpha"]
--  Display image on specified layer with optional position, opacity, blend.
-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T

function LayerCommands.image(ctx, params)
    local file      = resolve_file(params)
    local layerName = params.layer or "fg"

    if not file then
        print("[LayerCmd] image: no file specified")
        return
    end

    local layerType = layers.Type.LAYER_LAYER0
    if layerName == "bg" or layerName == "background" then
        layerType = layers.Type.LAYER_BASE
    elseif layerName == "fg" or layerName == "fore" then
        layerType = layers.Type.LAYER_LAYER0
    elseif layerName == "message" or layerName == "mes" then
        layerType = layers.Type.LAYER_MESSAGE
    end

    local tex = backend.load_texture(file)
    if not tex then
        print("[LayerCmd] image: failed to load " .. file)
        return
    end

    local node = get_or_create_layer( layerName, layerType)
    layers.set_layer_image(node, tex, nil, nil, nil, nil)
    layers.set_layer_visible(node, true)

    if params.x or params.y then
        local x = params.x or 0
        local y = params.y or 0
        layers.move_layer(node, x, y)
    end

    if params.opacity then
        local op = params.opacity
        if op then layers.set_layer_opacity(node, op) end
    end

    if params.blend then
        layers.set_layer_blend(node, params.blend)
    end

    ctx.layers = ctx.layers or {}
    ctx.layers[layerName] = file
end


-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T
--  [position layer="fg0" x=0.5 y=0.3 scale=1.0 unit="ndc"]
--  Set a layer's position. x,y in NDC [0-1] unless unit="px".
-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T

-- [moveto] -- KAG3 layer-move syntax (left/top/layer) mapped onto the
-- position command's x/y/layer semantics (Neo-Genesis alias).
-- Schema mirrors position (security LOW: the alias must clamp scale
-- the same way -- raw pass-through allowed unclamped render math).
-- NO defaults on left/top/x/y/layer (review blocking): coerce fills
-- defaults, and a filled "" layer or 0 left would SHADOW the handler's
-- `or`-fallbacks (layer-less moveto silently no-ops; x fallback dead).
-- Typed without default -- like image's x/y -- keeps nil pass-through.
schema.define("moveto", {
    _meta = { category = "layer", blocking = true, desc = "KAG3-compatible moveto command" },
    left  = { type = "number" },
    top   = { type = "number" },
    x     = { type = "number" },
    y     = { type = "number" },
    scale = { type = "number", default = 1.0, min = 0.01, max = 16 },
    layer = { type = "string" },
    unit  = { type = "string", default = "ndc" },
})
function LayerCommands.moveto(ctx, params)
    local layerName = params.layer or "fg"
    local x = params.left or params.x or 0
    local y = params.top or params.y or 0
    local scale = params.scale or 1.0
    local unit = params.unit or "ndc"
    layers.set_position(layerName, x, y, scale, unit)
end

function LayerCommands.position(ctx, params)
    local layerName = params.layer or "fg"
    local x = params.x or 0
    local y = params.y or 0
    local scale = params.scale or 1.0
    local unit = params.unit or "ndc"
    layers.set_position(layerName, x, y, scale, unit)
end

-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T
--  [layopt layer="fg0" opacity=0.8 visible=true blend="multiply"]
--  Batch-set layer visual options.
-- �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T

function LayerCommands.layopt(ctx, params)
    local layerName = params.layer or "fg"
    local opts = {}
    if params.opacity then
        opts.opacity = params.opacity
    end
    if params.visible ~= nil then
        if type(params.visible) == "string" then
            opts.visible = (params.visible == "true")
        else
            opts.visible = params.visible
        end
    end
    if params.blend then
        opts.blend = params.blend
    end
    layers.set_options(layerName, opts)
end

-- ═══════════════════════════════════════════════════════════════════════════
--  [layfade layer="bg" to=0 time=500]  (canonical, 0..255 byte)
--  [layfade layer="bg" opacity=0.5 time=500]  (legacy alias: <=1 fraction,
--  >1 passes through as 0..255)
--  Frame-stepped opacity transition for an existing layer (D2.6).
-- ═══════════════════════════════════════════════════════════════════════════

function LayerCommands.layfade(ctx, params)
    local layerName = params.layer or params.name
    if type(layerName) ~= "string" and type(params[1]) == "string" then
        layerName = params[1]
    end
    layerName = layerName or "bg"
    local node = layers.get(layerName)
    if not node then
        print("[LayerCmd] layfade: layer not found: " .. tostring(layerName))
        return
    end
    -- [M1-F] contract unification: the schema canonical target is "to"
    -- (0..255 byte, default 255 via coerce). opacity/alpha keep the
    -- legacy hybrid convention below; "to" needs no scaling -- its unit
    -- IS the byte range (to=1 stays 1, unlike opacity=1 -> 255).
    local target = tonumber(params.opacity or params.alpha)
    if target ~= nil then
        -- Scale ambiguity (audit): layopt's schema is 0..1 but fade_to /
        -- set_layer_opacity operate in 0..255. Accept BOTH: values <= 1
        -- are treated as 0..1 fractions (0.5 -> 128), larger values pass
        -- through as 0..255 (legacy [layfade opacity=128]). Non-breaking.
        -- tonumber FIRST: the tokenizer hands raw strings and Lua 5.4
        -- raises on string-vs-number compare (review blocking).
        if target <= 1 then
            target = math.floor(target * 255 + 0.5)
        end
    else
        target = tonumber(params.to)
    end
    if target == nil then
        print("[LayerCmd] layfade: opacity required")
        return
    end
    local duration = params.time or params.duration or 500
    layers.fade_to(node, target, duration)
end

return LayerCommands