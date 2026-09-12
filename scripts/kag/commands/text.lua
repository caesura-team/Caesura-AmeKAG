-- =============================================================================
--  Caesura (AmeKAG) ?? kag/commands/text.lua
--  Phase 4: KAG text tag handlers ?? [ch], [text], [l], [r], [er], [p]
--  Manages character dialog display, backlog, and text cursor state.
--  All rendering delegates to backend.render_text / backend.clear_text.
-- =============================================================================

local backend = require("backend")
local layers  = require("layers")
local Operation = require("kag.operation")
local TextScene = require("kag.text_scene")
local TextLayout = require("kag.text_layout")

-- NVL mode (Ren'Py parity): full-screen accumulated text block. Text
-- lines append below the previous one instead of replacing the message
-- window; [nvl clear] / [p] break the page. The NVL cursor reuses
-- textCursorX/Y (persisted in text_state -> save/rollback for free).
local NVL_X = 48
local NVL_Y0 = 160
local NVL_MAX_WIDTH = 1184

-- Reset the NVL cursor to the top of a fresh page. The cursor lives in
-- text_state.cursor_x/y (persisted by snapshot/rollback) AND the ctx
-- textCursorX/Y mirror (read by the next [ch]/[text]).
local function nvl_reset_cursor(ctx)
    ctx.textCursorX = NVL_X
    ctx.textCursorY = NVL_Y0
    local state = TextScene.get_state(ctx)
    state.cursor_x = NVL_X
    state.cursor_y = NVL_Y0
end

-- =============================================================================
--  Internal: update text_state for save/load position tracking
-- =============================================================================

local function update_text_state(ctx, action, char_count)
    ctx.text_state = ctx.text_state or {}
    ctx.text_state.line = ctx.text_state.line or 1
    ctx.text_state.char_offset = ctx.text_state.char_offset or 0
    ctx.text_state.last_action = action

    if action == "l" or action == "r" then
        ctx.text_state.line = (ctx.text_state.line or 1) + 1
        ctx.text_state.char_offset = 0
    elseif action == "p" or action == "er" then
        ctx.text_state.line = 1
        ctx.text_state.char_offset = 0
    elseif action == "ch" or action == "text" then
        ctx.text_state.char_offset =
            ctx.text_state.char_offset + (tonumber(char_count) or 1)
    end
end

-- Floor then clamp: float components (e.g. "128.9") must yield bytes
-- (review nit: max/min alone drops the floor for in-range floats).
local function clamp_byte(value)
    value = math.floor(tonumber(value) or 0)
    return math.max(0, math.min(255, value))
end

local function parse_hex_color(value)
    if type(value) ~= "string" then return nil end
    local hex = value:match("^#?(%x%x%x%x%x%x)$")
    if not hex then return nil end
    return {
        r = tonumber(hex:sub(1, 2), 16),
        g = tonumber(hex:sub(3, 4), 16),
        b = tonumber(hex:sub(5, 6), 16),
        a = 255,
    }
end

local function resolve_color(ctx, params)
    local state = TextScene.get_state(ctx)
    local color = parse_hex_color(params.color)
        or parse_hex_color(state.font_color)
        or { r = 255, g = 255, b = 255, a = 255 }
    color.r = clamp_byte(params.r or color.r)
    color.g = clamp_byte(params.g or color.g)
    color.b = clamp_byte(params.b or color.b)
    color.a = clamp_byte(params.a or color.a)
    return color
end

local function resolve_line_height(ctx)
    local state = TextScene.get_state(ctx)
    -- line_height() may return extra values; wrap in parens to keep only
    -- the first, otherwise tonumber gets >1 args and errors.
    local line_height = tonumber((backend.line_height()))
        or tonumber(state.font_size)
        or 24
    if line_height <= 0 then return 24 end
    return line_height
end

local function resolve_max_width(ctx, params, x)
    local explicit = params.max_width
    if explicit and explicit > 0 then return explicit end

    local chars_per_line = params.chars_per_line
    if chars_per_line and chars_per_line > 0 then
        local state = TextScene.get_state(ctx)
        return chars_per_line * (tonumber(state.font_size) or 24)
    end
    local vw = require("viewport").logical(1280, 720)
    return math.max(1, vw - x - 48)
end

local function animate_text_opacity(ctx, params)
    local duration = params.fade_time or params.fade or 0
    local target = clamp_byte(params.opacity or params.fade_to or 255)
    if duration <= 0 then
        TextScene.set_opacity(ctx, target)
        return
    end

    local from = clamp_byte(params.fade_from or 0)
    TextScene.set_opacity(ctx, from)

    local operation <close> = Operation.start(ctx)
    local elapsed = 0
    while elapsed < duration and not operation.token.cancelled do
        local delta_ms = tonumber(coroutine.yield()) or 16
        if operation.token.cancelled then break end
        elapsed = math.min(duration, elapsed + math.max(delta_ms, 0))
        local progress = elapsed / duration
        TextScene.set_opacity(ctx, from + (target - from) * progress)
    end

    if not operation.token.cancelled then
        TextScene.set_opacity(ctx, target)
        operation:complete()
    end
end

-- =============================================================================
--  Internal: replay a recorded page source through the display pipeline.
--  Localizes (current language), parses markup, builds the NVL speaker
--  prefix span, then wraps into persistent text_scene draws. Shared by
--  the [ch]/[text] handlers (display path) and relocalize_page (language
--  hot-switch full-page redraw). yOverride shifts the line for the
--  redraw layout cascade (translations may wrap to a different line
--  count); returns the new end y and the stripped plain text.
-- =============================================================================
local function _drawMessage(ctx, entry, yOverride)
    local opts = entry.opts or {}
    local nvl = opts.nvl == true
    local msgX = opts.msgX
    local msgY = yOverride or opts.msgY or 0
    local color = opts.color
    local lineHeight = opts.lineHeight
    -- Index of the first draw this call will append: every draw below is
    -- marked as a page-source draw so a language hot-switch redraw can
    -- drop exactly the old page and replay it (foreign draws like [ruby]
    -- stay untouched).
    local firstIdx = #TextScene.get_state(ctx).draws + 1

    -- Localization pipeline (same as the handlers): per-line translation
    -- then {key} token expansion, before markup parsing.
    local message = require("i18n").localize(entry.src, entry.scene)
    local spans, plain = nil, message
    if message:find("{", 1, true) then
        local parsed = TextLayout.parse_markup(message)
        spans, plain = parsed.spans, parsed.plain
    end

    local speaker = entry.speaker or ""
    if nvl and #speaker > 0 and #message > 0 then
        -- Inline prefix (styled span; format from ctx.nvl_prefix_fmt).
        -- The prefix is an "instant" draw (no typewriter truncation).
        local tr, tg, tb
        if ctx.nameplate_style and ctx.nameplate_style.text_color then
            -- Direct call: an and-chain would truncate the match's
            -- multi-return to one value (Lua binary-op adjustment).
            tr, tg, tb = ctx.nameplate_style.text_color:match(
                "(%d+),%s*(%d+),%s*(%d+)")
        end
        local prefix_span = {
            -- Format string: "%s" = speaker name. gsub substitution keeps
            -- stray "%" in names harmless -- both the pattern and the
            -- replacement string interpret "%" (escape the name first).
            text = (ctx.nvl_prefix_fmt or "「%s」：")
                -- Parentheses truncate the inner gsub's second return
                -- (match count) -- otherwise it lands in the outer gsub's
                -- limit slot and blocks all replacements.
                :gsub("%%s", (speaker:gsub("%%", "%%%%"))),
            color = (tr and { r = clamp_byte(tr), g = clamp_byte(tg),
                              b = clamp_byte(tb) }) or nil,
            size = nil,
            bold = false,
            italic = false,
            instant = true,
        }
        if not spans then
            spans = { { text = message, color = nil, size = nil,
                        bold = false, italic = false } }
        end
        table.insert(spans, 1, prefix_span)
    elseif #speaker > 0 then
        if nvl then
            -- Inline speaker label above the line (no fixed-position plate).
            local tr, tg, tb
            if ctx.nameplate_style and ctx.nameplate_style.text_color then
                tr, tg, tb = ctx.nameplate_style.text_color:match(
                    "(%d+),%s*(%d+),%s*(%d+)")
            end
            backend.render_text(speaker, msgX, msgY - lineHeight,
                clamp_byte(tr or 255), clamp_byte(tg or 255),
                clamp_byte(tb or 255), 255)
        else
            TextScene.add_text(
                ctx, "[" .. speaker .. "]", opts.nameX or 540, 540, color,
                nil, 1, false, false, true)
        end
    end

    if #message > 0 then
        if spans then
            msgY = TextScene.add_wrapped_spans(ctx, spans, {
                x = msgX,
                y = msgY,
                max_width = opts.maxWidth,
                line_height = lineHeight,
                font_size = opts.font_size or lineHeight,
                color = color,
            })
        else
            msgY = TextScene.add_wrapped(ctx, message, {
                x = msgX,
                y = msgY,
                max_width = opts.maxWidth,
                line_height = lineHeight,
                font_size = opts.font_size or lineHeight,
                color = color,
            })
        end
    end

    -- Mark the draws this call produced as page-source draws (see above).
    local st = TextScene.get_state(ctx)
    for i = firstIdx, #st.draws do
        st.draws[i]._page_src = true
    end
    return msgY, plain
end

-- =============================================================================
--  Internal: draw the choice button group (shared by [endbutton] and the
--  language hot-switch redraw). Buttons keep their y/h regions so
--  hit-testing and the active-block state survive re-rendering.
-- =============================================================================
local function _renderChoices(ctx, buttons)
    TextScene.remove_group(ctx, "choices")
    local startY = 450
    local lineHeight = resolve_line_height(ctx)
    for idx, choice in ipairs(buttons) do
        local y = startY + (idx - 1) * (lineHeight + 8)
        TextScene.add_text(
            ctx, idx .. ". " .. choice.text, 32, y,
            { r = 255, g = 255, b = 0, a = 255 }, "choices")
        -- Store button region for hit testing
        choice.y = y
        choice.h = lineHeight
        choice.index = idx
    end
end

local TextCommands = {}

-- =============================================================================
--  Internal: push a message entry to the ctx.backlog (spec [4.1])
--  [R5-FIX] Exported for system.lua delegation
-- =============================================================================

-- Neo-Genesis contracts: typed + clamped via kag/schema.
local schema = require("kag.schema")
schema.define("ch", {
    _meta = { category = "text", blocking = true, desc = "KAG3-compatible ch command" },
    name   = { type = "string", default = "" },
    text   = { type = "string", default = "", interpolate = true },
    voice  = { type = "string", default = "" },
    sprite = { type = "string" },  -- no default: "" is truthy and would shadow storage/file
    max_width = { type = "number", default = 0, min = 0, max = 4096 },
    chars_per_line = { type = "number", default = 0, min = 0, max = 512 },
})
schema.define("text", {
    _meta = { category = "text", blocking = false, desc = "KAG3-compatible text command" },
    text = { type = "string", default = "", interpolate = true },
    fade_time = { type = "number", default = 0, min = 0, max = 30000 },
    fade = { type = "number", default = 0, min = 0, max = 30000 },
})
schema.define("ruby", {
    _meta = { category = "text", blocking = false, desc = "KAG3-compatible ruby command" },
    text = { type = "string", default = "" },
    ruby = { type = "string", default = "" },
    x = { type = "number", default = 0 },
    y = { type = "number", default = 0 },
    ruby_scale = { type = "number", default = 0.5, min = 0.1, max = 2.0 },
})
schema.define("font", {
    _meta = { category = "text", blocking = false, desc = "KAG3-compatible font command" },
    face = { type = "string", default = "default" },
    size = { type = "number", default = 22, min = 4, max = 256 },
    color = { type = "string", default = "white" },  -- KAG3 color param
})
-- Text-flow family (Neo-Genesis: typed + validated like every command).
schema.define("l", {
    _meta = { category = "text", blocking = false, desc = "line break" },
})          -- line break (no params)
schema.define("r", {
    _meta = { category = "text", blocking = false, desc = "carriage return" },
})          -- carriage return (no params)
schema.define("er", {
    _meta = { category = "text", blocking = false, desc = "erase line" },
})         -- erase line (no params)
schema.define("br", {
    _meta = { category = "text", blocking = false, desc = "KAG3 line-break alias" },
})         -- KAG3 line-break alias (no params)
schema.define("hr", {
    _meta = { category = "text", blocking = false, desc = "horizontal rule" },
})         -- horizontal rule (decorative)
schema.define("p", {
    _meta = { category = "text", blocking = true, desc = "click-to-advance" },
})          -- click-to-advance (no params)
schema.define("reset", {
    _meta = { category = "text", blocking = false, desc = "reset text state" },
})      -- reset text state (no params)
schema.define("s", {
    _meta = { category = "text", blocking = true, desc = "KAG3 short-wait" },            -- KAG3 short-wait
    ms = { type = "number", default = 250, min = 0, max = 60000 },
})

function TextCommands.push_backlog(ctx, speaker, text, voiceFile, src)
    ctx.backlog = ctx.backlog or {}
    local entry = {
        name        = speaker or "",
        text        = text or "",
        voice       = voiceFile or "",
        time        = os.date("%H:%M:%S"),
        timestamp   = os.time(),
        scene       = ctx.current_scene or ctx.currentScene or "",
        token_index = ctx.token_index or 1,
        -- Pre-localize message: the language hot-switch redraw re-localizes
        -- the backlog from this source (entries without src keep their text).
        src         = src,
    }
    table.insert(ctx.backlog, entry)

    -- Trim if over max
    local maxEntries = ctx.backlog_max or 500
    while #ctx.backlog > maxEntries do
        table.remove(ctx.backlog, 1)
    end

    -- [R7-FIX] Seen-marking moved to the click handler (kag_runner.on_click):
    -- marking here made every line "seen" the moment it was displayed, so
    -- read-skip could never distinguish unread text.
end

-- =============================================================================
--  [ch name="Hero" text="Hello, world!"]
--  Display character dialog: renders name + text on message layer,
--  appends to backlog, and blocks until click (via [p] semantics).
-- =============================================================================

-- [textbox] -- Neo-Genesis message-window styling (KAG3 needed TJS ext).
-- Configures the message layer: position, size, background color and
-- opacity. State persists in ctx.textbox_style and is re-applied on
-- [cl] (clearscreen rebuilds the window).
schema.define("textbox", {
    _meta = { category = "text", blocking = false, desc = "KAG3-compatible textbox command" },
    x       = { type = "number", default = 0 },
    y       = { type = "number", default = 520 },
    w       = { type = "number", default = 1280, min = 64, max = 4096 },
    h       = { type = "number", default = 200, min = 32, max = 1024 },
    color   = { type = "string", default = "0,0,0" },
    opacity = { type = "number", default = 200, min = 0, max = 255 },
    visible = { type = "boolean", default = true },
})

function TextCommands.textbox(ctx, params)
    ctx.textbox_style = {
        x = params.x, y = params.y, w = params.w, h = params.h,
        color = params.color, opacity = params.opacity, visible = params.visible,
    }
    local layers = require("layers")
    local bg = layers.ensure(ctx, "_textbox", 2)  -- below message text
    bg.visible = params.visible
    bg.x, bg.y = params.x, params.y
    bg.w, bg.h = params.w, params.h
    -- color "r,g,b" -> solid texture (backend.create_solid_texture)
    local r, g, b = params.color:match("(%d+),%s*(%d+),%s*(%d+)")
    if r then
        bg.texture = backend.create_solid_texture(
            clamp_byte(r), clamp_byte(g), clamp_byte(b),
            math.floor(params.opacity))
    else
        bg.texture = nil  -- unparseable color: clear stale texture
    end
    layers.mark_dirty(bg)
end

-- [nameplate] -- Neo-Genesis character-name plate (KAG3 needed TJS ext).
-- Styles the speaker name display above the message window. When
-- configured, [ch name=X] shows the plate with the character's name;
-- the style persists in ctx.nameplate_style.
schema.define("nameplate", {
    _meta = { category = "text", blocking = false, desc = "KAG3-compatible nameplate command" },
    x       = { type = "number", default = 32 },
    y       = { type = "number", default = 480 },
    w       = { type = "number", default = 220, min = 32, max = 1024 },
    h       = { type = "number", default = 36, min = 16, max = 256 },
    color   = { type = "string", default = "0,0,0" },
    opacity = { type = "number", default = 220, min = 0, max = 255 },
    text_color = { type = "string", default = "255,255,255" },
})

function TextCommands.nameplate(ctx, params)
    ctx.nameplate_style = {
        x = params.x, y = params.y, w = params.w, h = params.h,
        color = params.color, opacity = params.opacity,
        text_color = params.text_color,
    }
    -- Re-render the current speaker's plate immediately.
    if ctx and ctx.current_speaker and #ctx.current_speaker > 0 then
        TextCommands._renderNameplate(ctx, ctx.current_speaker)
    end
end

-- Render the nameplate layer for a speaker (called by [ch] and [nameplate]).
function TextCommands._renderNameplate(ctx, speaker)
    local layers = require("layers")
    local bg = layers.ensure(ctx, "_nameplate", 3)  -- above textbox, below text
    local st = ctx.nameplate_style or {
        x = 32, y = 480, w = 220, h = 36,
        color = "0,0,0", opacity = 220,
        text_color = "255,255,255",
    }
    bg.visible = true
    bg.x, bg.y, bg.w, bg.h = st.x, st.y, st.w, st.h
    local r, g, b = st.color:match("(%d+),%s*(%d+),%s*(%d+)")
    if r then
        bg.texture = backend.create_solid_texture(
            clamp_byte(r), clamp_byte(g), clamp_byte(b),
            math.floor(st.opacity))
    else
        bg.texture = nil  -- unparseable color: clear stale plate
    end
    layers.mark_dirty(bg)
    -- Speaker name text: backend.render_text(text, x, y, r, g, b, a).
    local tr, tg, tb = st.text_color:match("(%d+),%s*(%d+),%s*(%d+)")
    backend.render_text(speaker, st.x + 8, st.y + 6,
        clamp_byte(tr or 255), clamp_byte(tg or 255), clamp_byte(tb or 255), 255)
end

-- [sprite_fade] -- character-sprite fade in/out (performance idiom:
-- KAG3 needed layeredit + tween glue for this). Animates the
-- _char_<speaker> layer opacity 0..255 via an operation yield loop.
schema.define("sprite_fade", {
    _meta = { category = "text", blocking = true, desc = "KAG3-compatible sprite_fade command" },
    speaker = { type = "string", required = true },
    to = { type = "number", default = 255, min = 0, max = 255 },
    time = { type = "number", default = 300, min = 0, max = 30000 },
})

function TextCommands.sprite_fade(ctx, params)
    local layers = require("layers")
    local name = "_char_" .. (params.speaker or "")
    local node = layers.get(name) or layers.find(name)
    if not node then
        print("[sprite_fade] no sprite layer: " .. name)
        return
    end
    local from = node.opacity or 255
    local to = params.to
    local dur = params.time
    if dur <= 0 then
        layers.set_layer_opacity(node, to)
        return
    end
    local operation <close> = require("kag.operation").start(ctx)
    local ct = operation.token
    local elapsed = 0
    while elapsed < dur and not ct.cancelled do
        elapsed = elapsed + (coroutine.yield() or 16)
        local t = math.min(1, elapsed / dur)
        local o = math.floor(from + (to - from) * t)
        layers.set_layer_opacity(node, o)
    end
    if not ct.cancelled then
        layers.set_layer_opacity(node, to)
        operation:complete()
    end
end

-- [sprite_move] -- character-sprite slide (entrance/exit performance).
-- Animates the _char_<speaker> layer x/y toward a target position.
schema.define("sprite_move", {
    _meta = { category = "text", blocking = true, desc = "KAG3-compatible sprite_move command" },
    speaker = { type = "string", required = true },
    x = { type = "number", default = 440 },
    y = { type = "number", default = 200 },
    time = { type = "number", default = 400, min = 0, max = 30000 },
})

function TextCommands.sprite_move(ctx, params)
    local layers = require("layers")
    local name = "_char_" .. (params.speaker or "")
    local node = layers.get(name) or layers.find(name)
    if not node then
        print("[sprite_move] no sprite layer: " .. name)
        return
    end
    local fromX, fromY = node.x or 0, node.y or 0
    local toX, toY = params.x, params.y
    local dur = params.time
    if dur <= 0 or (fromX == toX and fromY == toY) then
        layers.move_layer(node, toX, toY)
        return
    end
    local operation <close> = require("kag.operation").start(ctx)
    local ct = operation.token
    local elapsed = 0
    while elapsed < dur and not ct.cancelled do
        elapsed = elapsed + (coroutine.yield() or 16)
        local t = math.min(1, elapsed / dur)
        layers.move_layer(node, fromX + (toX - fromX) * t,
                               fromY + (toY - fromY) * t)
    end
    if not ct.cancelled then
        layers.move_layer(node, toX, toY)
        operation:complete()
    end
end

-- [sprite_scale] -- character-sprite zoom (performance emphasis).
-- Animates the _char_<speaker> layer scaleX/scaleY toward a target.
schema.define("sprite_scale", {
    _meta = { category = "text", blocking = true, desc = "KAG3-compatible sprite_scale command" },
    speaker = { type = "string", required = true },
    scale = { type = "number", default = 1.0, min = 0.1, max = 4.0 },
    time = { type = "number", default = 300, min = 0, max = 30000 },
})

function TextCommands.sprite_scale(ctx, params)
    local layers = require("layers")
    local name = "_char_" .. (params.speaker or "")
    local node = layers.get(name) or layers.find(name)
    if not node then
        print("[sprite_scale] no sprite layer: " .. name)
        return
    end
    local from = node.scaleX or node.scale or 1.0
    local to = params.scale
    local dur = params.time
    if dur <= 0 or math.abs(from - to) < 0.001 then
        node.scaleX, node.scaleY = to, to
        layers.mark_dirty(node)
        return
    end
    local operation <close> = require("kag.operation").start(ctx)
    local ct = operation.token
    local elapsed = 0
    while elapsed < dur and not ct.cancelled do
        elapsed = elapsed + (coroutine.yield() or 16)
        local t = math.min(1, elapsed / dur)
        local sc = from + (to - from) * t
        node.scaleX, node.scaleY = sc, sc
        layers.mark_dirty(node)
    end
    if not ct.cancelled then
        node.scaleX, node.scaleY = to, to
        layers.mark_dirty(node)
        operation:complete()
    end
end

-- [sprite_swap] -- character re-dress / expression swap (performance
-- idiom: KAG3 needed layeredit + reload glue). Swaps the standing
-- portrait's texture and re-registers the sprite for future [ch].
schema.define("sprite_swap", {
    _meta = { category = "text", blocking = true, desc = "KAG3-compatible sprite_swap command" },
    speaker = { type = "string", required = true },
    sprite = { type = "string", required = true },
})

function TextCommands.sprite_swap(ctx, params)
    local layers = require("layers")
    local name = "_char_" .. (params.speaker or "")
    local node = layers.get(name) or layers.find(name)
    if not node then
        print("[sprite_swap] no sprite layer: " .. name)
        return
    end
    local tex = backend.load_texture(params.sprite)
    if not tex or tex == 0 then
        print("[sprite_swap] failed to load: " .. params.sprite)
        return  -- keep the current outfit visible
    end
    node.texture = tex
    layers.mark_dirty(node)
    -- Re-register so later [ch name=<speaker>] keeps the new outfit.
    ctx.characters = ctx.characters or {}
    if ctx.characters[params.speaker] then
        ctx.characters[params.speaker].sprite = params.sprite
    end
end

function TextCommands.ch(ctx, params)
    local speaker = params.name or params.character or ""
    local message = params.text or params.message or ""
    -- Pre-localize source (post-interpolation): the language hot-switch
    -- redraw re-localizes the page from this text with the current
    -- language (see relocalize_page / _drawMessage).
    local src_message = message

    -- Localization pipeline: per-line translation (lines[scene:hash]) first,
    -- then {key} token expansion — applied BEFORE markup parsing so the
    -- translated string may itself carry {color}/{size}/{b}/{i} markup.
    if #message > 0 then
        message = require("i18n").localize(message, ctx.current_scene)
    end

    local nvl = ctx.nvl_mode == true

    -- Neo-Genesis inline markup: {color=#rrggbb}...{/color} spans. The
    -- backlog / closed captions / reveal counters use the stripped plain
    -- text (visible characters only); drawing uses the colored spans.
    local spans, plain = nil, message
    if message:find("{", 1, true) then
        local parsed = TextLayout.parse_markup(message)
        spans, plain = parsed.spans, parsed.plain
    end

    -- U1.3: pos parameter (left/center/right) for text alignment
    local pos = params.pos or "center"
    if pos ~= "left" and pos ~= "center" and pos ~= "right" then
        pos = "center"
    end

    -- Neo-Genesis: show the nameplate when a speaker is present. NVL mode
    -- uses an inline label instead (see the draw section below), so the
    -- fixed-position plate is skipped.
    ctx.current_speaker = speaker
    if not nvl and ctx.nameplate_style and #speaker > 0 then
        TextCommands._renderNameplate(ctx, speaker)
    end

    -- U1.3: ctx.characters registry -- track active on-screen characters
    ctx.characters = ctx.characters or {}
    if #speaker > 0 then
        if not ctx.characters[speaker] then
            ctx.characters[speaker] = { pos = pos }
        else
            -- Inherit stored position unless explicitly overridden
            if params.pos then
                ctx.characters[speaker].pos = pos
            else
                pos = ctx.characters[speaker].pos or "center"
            end
        end
        if params.layer then
            ctx.characters[speaker].layer = params.layer
        end
        -- Guard must admit sprite-only lines (storage/file are nil then)
        -- and reject the contract's "" sprite default (truthy -> would
        -- shadow storage/file and create a bogus empty layer).
        if (params.sprite and params.sprite ~= "") or params.storage or params.file then
            -- sprite= is the contract-advertised param; storage/file are the
            -- KAG3 aliases. All three register the standing portrait.
            ctx.characters[speaker].sprite =
                (params.sprite ~= "" and params.sprite) or params.storage or params.file
        end
    end

    -- Voice: [ch voice="assets/voice/x.wav"] plays the line and stores the
    -- file in the backlog entry so the history overlay's V key can replay it.
    -- Accessibility: with cc_mode enabled the CURRENT line becomes the
    -- closed-caption text (drawn at a fixed bottom position by
    -- kag_runner.render) while a voice line is on screen.
    local voiceFile = params.voice or params.voicefile or ""
    if #voiceFile > 0 then
        -- Closed captions: with cc_mode enabled the voiced line becomes
        -- the standing caption until the next voiced (or voiceless) line.
        -- cc_mode lives on ctx (set by Settings._applyAll, game scripts,
        -- or the runner's startup sync from config) -- ch NEVER loads
        -- config itself (its require chain is unavailable in degraded
        -- contexts and would disturb suite ordering).
        if ctx.cc_mode == true then
            ctx.cc_text = {
                speaker = speaker, text = plain,
                -- Pre-localize source for the hot-switch redraw.
                src = src_message,
                scene = ctx.current_scene or ctx.currentScene or "",
            }
        end
        -- Non-blocking voice: playvoice() yields until the clip ends, which
        -- would stall the script (and swallow clicks) for the whole line.
        -- Fire-and-forget matches VN behavior: the line's voice plays while
        -- the text is on screen; clicking continues immediately.
        pcall(function()
            local audio = require("kag.commands.audio")
            local file = voiceFile
            -- resolve_file lives on the audio module; reuse its resolution
            -- via a direct play with the raw path when valid.
            backend.audio_play("voice", file, {})
        end)
    else
        -- No voice on this line: clear any standing caption (CC shows only
        -- lines that are actually voiced; the textbox itself stays).
        ctx.cc_text = nil
    end

    -- Character sprite (KAG-style standing portrait): if the speaker has a
    -- registered sprite (via [ch sprite=] or a previous [ch storage=]), show
    -- it on a dedicated layer, positioned by the speaker's registered pos.
    if #speaker > 0 and ctx.characters and ctx.characters[speaker]
       and ctx.characters[speaker].sprite then
        local sprite = ctx.characters[speaker].sprite
        local spritePos = ctx.characters[speaker].pos or "center"
        local charLayerName = "_char_" .. speaker
        local node = layers.get(charLayerName)
        if not node then
            node = layers.add_layer(nil, {
                name = charLayerName, layer_type = 0,
                x = 0, y = 200, w = 400, h = 520, visible = true,
            })
            layers.set_z(node, 1)
        end
        if spritePos == "left" then
            node.x, node.y = 40, 200
        elseif spritePos == "right" then
            node.x, node.y = 840, 200
        else
            node.x, node.y = 440, 200
        end
        node.texture = backend.load_texture(sprite)
        node.visible = true
    end

    -- Store in backlog (src = pre-localize message for hot-switch redraw)
    TextCommands.push_backlog(ctx, speaker, plain, voiceFile, src_message)

    -- Set up message layer if needed
    local msgNode = layers.get("message")
    if not msgNode then
        -- Bottom dialog box sized to the logical viewport: full width,
        -- 200px tall at y = H - 200 (was 1280x200 at y=520, a 720-px layout
        -- that floated mid-screen at 1920x1080).
        local vw, vh = require("viewport").wh()
        msgNode = layers.add_layer(nil, {
            name = "message",
            layer_type = layers.Type.LAYER_MESSAGE,
            x = 0, y = math.max(0, vh - 200), visible = true,
        })
        -- TextScene submits the persistent glyphs directly. This node only
        -- carries message geometry/visibility and needs no empty GPU target.
        msgNode.w, msgNode.h = vw, 200
        layers.set_z(msgNode, 2)
    end

    -- Replace the persistent message draw list. The render loop submits it
    -- every frame, so text remains visible after the command returns.
    -- NVL mode: text accumulates into a full-screen page instead of
    -- replacing the message window (Ren'Py NVL parity).
    if not nvl then
        backend.clear_text()
        TextScene.clear(ctx)
    else
        -- Seal the page: prior lines are already fully revealed, so the
        -- typewriter only animates the line being appended.
        TextScene.commit(ctx)
    end
    -- [hr] (t202): the standing rule belongs to the finished page.
    TextCommands._hideHr(ctx)

    -- Calculate X positions based on "pos" (normal mode) or the fixed NVL
    -- column (full-screen page).
    local nameX, msgX
    local vw, vh = require("viewport").wh()
    if nvl then
        msgX = NVL_X
    elseif pos == "left" then
        nameX = 48
        msgX  = 48
    elseif pos == "right" then
        nameX = vw - 248
        msgX  = vw - 496    -- right-side text starts earlier for readability
    else  -- center (default)
        nameX = math.floor(vw / 2) - 100   -- speaker name centered
        msgX  = 48     -- dialogue text left-aligned (standard galgame convention)
    end

    local color = resolve_color(ctx, params)
    local lineHeight = resolve_line_height(ctx)

    -- Calculate y-position for message: normal mode draws below the fixed
    -- speaker line; NVL mode continues at the accumulated cursor (reset by
    -- [nvl]/[nvl clear]/[p] to the top of the page). 580 was the 720-px
    -- baseline; at H it becomes H - 140 so the box hugs the bottom.
    local msgY = nvl and (ctx.textCursorY or NVL_Y0) or math.max(0, vh - 140)
    local maxWidth = nvl and math.max(1, vw - 96)
        or resolve_max_width(ctx, params, msgX)

    -- Record the page source (pre-localize message + the exact layout
    -- values used) so a language hot-switch can replay this line in the
    -- new language (see relocalize_page). page_src lives in text_state
    -- and is cleared wherever the draws are cleared (TextScene.clear).
    local state = TextScene.get_state(ctx)
    state.page_src = state.page_src or {}
    local entry = {
        kind = "ch",
        src = src_message,
        scene = ctx.current_scene or ctx.currentScene or "",
        speaker = speaker,
        opts = {
            nvl = nvl, pos = pos, nameX = nameX,
            color = color, lineHeight = lineHeight,
            msgX = msgX, msgY = msgY, maxWidth = maxWidth,
            font_size = tonumber(state.font_size) or lineHeight,
        },
    }
    state.page_src[#state.page_src + 1] = entry
    -- Draw the line through the shared replay path (also used by the
    -- language hot-switch redraw): speaker prefix/label, markup spans,
    -- wrap and persistent draws.
    msgY = _drawMessage(ctx, entry)

    ctx.textCursorX = msgX
    ctx.textCursorY = msgY
    animate_text_opacity(ctx, params)
    ctx.waiting_input = true
    update_text_state(ctx, "ch", utf8.len(plain) or #plain)
    -- Typewriter reveal: animate chars in over text_speed ms/char.
    ctx.reveal = { total = TextScene.reveal_length(ctx), elapsed = 0, last_shown = 0 }
    TextScene.get_state(ctx).reveal_chars = 0
end

-- =============================================================================
--  [text text="Plain narration text."]
--  Display narration (no speaker name). Appends to backlog.
-- =============================================================================

function TextCommands.text(ctx, params)
    local message = params.text or params.message or params.content or ""
    if #message == 0 then return end
    -- Pre-localize source (post-interpolation; see [ch]).
    local src_message = message

    -- Localization pipeline (same as [ch]): per-line translation first,
    -- then {key} token expansion, before markup parsing.
    message = require("i18n").localize(message, ctx.current_scene)

    -- Neo-Genesis inline markup (see [ch]): spans for drawing, plain for
    -- backlog / reveal.
    local spans, plain = nil, message
    if message:find("{", 1, true) then
        local parsed = TextLayout.parse_markup(message)
        spans, plain = parsed.spans, parsed.plain
    end

    TextCommands.push_backlog(ctx, "", plain, src_message)

    local nvl = ctx.nvl_mode == true
    if not nvl then
        backend.clear_text()
        TextScene.clear(ctx)
    else
        -- NVL accumulation: seal prior lines, append below the cursor.
        TextScene.commit(ctx)
    end
    -- [hr] (t202): the standing rule belongs to the finished page.
    TextCommands._hideHr(ctx)

    local lineHeight = resolve_line_height(ctx)
    local x = nvl and NVL_X or 32
    local y = nvl and (ctx.textCursorY or NVL_Y0) or 580
    local maxWidth = nvl and NVL_MAX_WIDTH or resolve_max_width(ctx, params, x)

    -- Record the page source for the language hot-switch redraw, then
    -- draw through the shared replay path (same as [ch]).
    local state = TextScene.get_state(ctx)
    state.page_src = state.page_src or {}
    local entry = {
        kind = "text",
        src = src_message,
        scene = ctx.current_scene or ctx.currentScene or "",
        speaker = "",
        opts = {
            nvl = nvl,
            color = resolve_color(ctx, params),
            lineHeight = lineHeight,
            msgX = x, msgY = y, maxWidth = maxWidth,
            font_size = tonumber(state.font_size) or lineHeight,
        },
    }
    state.page_src[#state.page_src + 1] = entry
    y = _drawMessage(ctx, entry)

    ctx.textCursorX = x
    ctx.textCursorY = y
    animate_text_opacity(ctx, params)
    ctx.waiting_input = true
    update_text_state(ctx, "text", utf8.len(plain) or #plain)
    ctx.reveal = { total = TextScene.reveal_length(ctx), elapsed = 0, last_shown = 0 }
    TextScene.get_state(ctx).reveal_chars = 0
end

-- =============================================================================
--  [l] ?? line break: advance text cursor to next line
-- =============================================================================

function TextCommands.l(ctx, params)
    local lineHeight = resolve_line_height(ctx)
    ctx.textCursorY = (ctx.textCursorY or 600) + lineHeight
    ctx.textCursorX = 32
    local state = TextScene.get_state(ctx)
    state.cursor_x = ctx.textCursorX
    state.cursor_y = ctx.textCursorY
    update_text_state(ctx, "l")
end

-- =============================================================================
--  [hr] -- horizontal rule (decorative separator, KAG3 compat)
--  Draws a thin full-width rule at the current text cursor position and
--  advances the cursor one line so following text continues below it.
--  The rule lives on the _hr layer (above _textbox, below text) and is
--  hidden by the next [ch]/[text]/[er]/[reset] - it belongs to the page.
--  Zero-parameter contract (decorative; no style surface yet - honest).
-- =============================================================================

function TextCommands.hr(ctx, params)
    local vw = require("viewport").logical(1920, 1080)
    local lineHeight = tonumber(resolve_line_height(ctx)) or 24
    local y = tonumber(ctx.textCursorY) or 580
    local layers = require("layers")
    local node = layers.ensure(ctx, "_hr", 2)
    node.visible = true
    node.x, node.y = 32, y
    node.w = math.max(8, vw - 64)
    node.h = 2
    node.texture = backend.create_solid_texture(160, 160, 160, 200)
    layers.mark_dirty(node)
    -- The rule occupies one row: advance so following text sits below it.
    ctx.textCursorY = y + lineHeight
    ctx.textCursorX = 32
end

-- Hide the standing rule (called by [ch]/[text]/[er]/[reset]).
function TextCommands._hideHr(ctx)
    local ok, node = pcall(require("layers").get, "_hr")
    if ok and node and node.visible ~= false then
        node.visible = false
    end
end

-- =============================================================================
--  [r] ?? carriage return: reset cursor to start of current line
-- =============================================================================

function TextCommands.r(ctx, params)
    ctx.textCursorX = 32
    TextScene.get_state(ctx).cursor_x = ctx.textCursorX
    update_text_state(ctx, "r")
end

-- =============================================================================
--  [er] ?? erase: clear all text from message layer (backlog preserved)
-- =============================================================================

function TextCommands.er(ctx, params)
    backend.clear_text()
    TextScene.clear(ctx)
    -- [hr] (t202): page cleared -> the standing rule goes with it.
    TextCommands._hideHr(ctx)
    if ctx.nvl_mode then
        -- Erasing the NVL page also resets the accumulation cursor.
        nvl_reset_cursor(ctx)
    end
    ctx.waiting_input = false
    update_text_state(ctx, "er")
end

-- =============================================================================
--  [p] ?? page break / click-to-advance
--  Blocks the coroutine until user clicks or presses Enter/Space.
--  The scheduler detects ctx.waiting_input and handles resume on input.
-- =============================================================================

function TextCommands.p(ctx, params)
    ctx.waiting_input = true
    -- Reached the actual handler, beyond either Lua/KAG debugger pause.
    -- A restored scheduler no longer needs to reconstruct this suspension.
    ctx._resumePageWait = nil

    -- Keep the current page visible while waiting, then clear it only after
    -- the scheduler resumes this coroutine for the accepted click.
    coroutine.yield()
    backend.clear_text()
    TextScene.clear(ctx)
    -- [hr] (t202): page cleared -> the standing rule goes with it.
    TextCommands._hideHr(ctx)
    if ctx.nvl_mode then
        -- NVL page break: the next line starts a fresh page at the top.
        nvl_reset_cursor(ctx)
    end
    update_text_state(ctx, "p")
end

-- =============================================================================
--  [nvl] / [nvl clear] / [nvl off] ?? NVL mode (Ren'Py parity)
--  Full-screen accumulated text block. In NVL mode each [ch]/[text] line
--  APPENDS below the previous one (instead of replacing the message
--  window); [nvl clear] (or [p]) breaks the page, [nvl off] returns to the
--  normal message window. The speaker name renders as an inline label.
-- =============================================================================

schema.define("nvl", {
    _meta = { category = "text", blocking = false, desc = "NVL mode: full-screen accumulated text (Ren'Py parity); [nvl clear] page break, [nvl off] exit; [nvl prefix=\"「%s」：\"] customizes the speaker prefix format (%s = name)" },
    mode = { type = "string" },  -- "clear"/"off"/omitted = enter (bare [nvl clear] passes via params[1])
    prefix = { type = "string" },  -- speaker prefix format for this NVL session ("%s" = speaker name)
})

function TextCommands.nvl(ctx, params)
    -- Bare positional form: [nvl clear] -> params[1] == "clear".
    local mode = params.mode or params[1]
    if mode ~= "clear" and mode ~= "off" then mode = "enter" end

    if mode == "off" then
        ctx.nvl_mode = false
        -- Drop the accumulated page so the normal window resumes clean.
        backend.clear_text()
        TextScene.clear(ctx)
        -- REVIEW-FIX (phase D): NVL and the fixed message window are
        -- mutually exclusive (Ren'Py NVL parity). [nvl off] restores the
        -- pre-NVL visibility of the _textbox / _nameplate layers that
        -- [nvl] hid on entry (saved in ctx.nvl_hidden_vis).
        local saved = ctx.nvl_hidden_vis
        ctx.nvl_hidden_vis = nil
        for _, ln in ipairs({ "_textbox", "_nameplate" }) do
            local node = layers.get(ln)
            if node then
                node.visible = (saved and saved[ln] ~= nil)
                    and saved[ln] or node.visible
            end
        end
        return
    end

    -- enter / clear both start a fresh page at the top of the screen.
    ctx.nvl_mode = true
    if params.prefix ~= nil then
        ctx.nvl_prefix_fmt = params.prefix  -- persists until changed
    end
    backend.clear_text()
    TextScene.clear(ctx)
    -- REVIEW-FIX (phase D): hide the fixed message-window / nameplate
    -- layers while in full-screen NVL mode (the textbox would otherwise
    -- box the accumulated block). Save their prior visibility so [nvl
    -- off] can restore it. Layers that never existed stay untouched.
    if not ctx.nvl_hidden_vis then
        ctx.nvl_hidden_vis = {}
        for _, ln in ipairs({ "_textbox", "_nameplate" }) do
            local node = layers.get(ln)
            if node then
                ctx.nvl_hidden_vis[ln] = node.visible == true
                if node.visible then node.visible = false end
            end
        end
    end
    nvl_reset_cursor(ctx)
    update_text_state(ctx, "nvl")
end


-- =============================================================================
--  [ruby text="?h??" ruby="????"]
--  Render base text with ruby (furigana) annotation above it.
--  Delegates to TextScene.add_ruby -- glyph layout via TextLayout.measure_ruby
--  and rendering via backend.render_ruby (KAGBinding luaL_Reg "render_ruby").
-- =============================================================================

function TextCommands.ruby(ctx, params)
    local text = params.text or ""
    local ruby_text = params.ruby or ""
    if text == "" then return end

    local lineHeight = resolve_line_height(ctx)
    local startX = params.start_x or 32
    -- REVIEW-FIX (phase D): schema.coerce fills x=0/y=0 defaults, and Lua
    -- treats 0 as truthy, so a bare [ruby ...] (no explicit position) used
    -- to pin the draw at absolute (0,0) instead of following the current
    -- text cursor. Only a strictly positive x/y overrides cursor-follow;
    -- nil lets add_ruby fall back to state.cursor_x / cursor_y.
    local rx = (params.x and params.x > 0) and params.x or nil
    local ry = (params.y and params.y > 0) and params.y or nil
    TextScene.add_ruby(ctx, text, ruby_text, {
        x = rx,
        y = ry,
        start_x = startX,
        max_width = resolve_max_width(ctx, params, startX),
        line_height = lineHeight,
        font_size = tonumber(TextScene.get_state(ctx).font_size)
            or lineHeight,
        ruby_scale = params.ruby_scale or 0.5,
        color = resolve_color(ctx, params),
    })
end

-- =============================================================================
--  [font face="Noto Serif" size=28 color="#333333"]
--  Set font face, size, and/or color for subsequent text rendering.
--  Only specified params are updated; existing values are preserved.
-- =============================================================================

function TextCommands.font(ctx, params)
    ctx.text_state = ctx.text_state or {}
    if params.face then ctx.text_state.font_face = params.face end
    if params.size then ctx.text_state.font_size = params.size end
    if params.color then ctx.text_state.font_color = params.color end
    local backend = require("backend")
    backend.text_set_font(ctx.text_state.font_face, ctx.text_state.font_size, ctx.text_state.font_color)
end

-- =============================================================================
--  [skip] ?? toggle skip mode
--  When active, scheduler auto-advances without waiting for user click.
-- =============================================================================

function TextCommands.skip(ctx, params)
    local mode = params.mode
    if mode == "seen" then
        -- Skip only already-seen text ([skip mode=seen]); a second
        -- [skip mode=seen] turns it OFF. (Audit fix: the old
        -- `cond and false or "seen"` is a value-selector, NOT a branch --
        -- it ALWAYS yielded "seen", so seen-skip could never be turned
        -- off.)
        if ctx.skip_mode == "seen" then
            ctx.skip_mode = false
        else
            ctx.skip_mode = "seen"
        end
    else
        ctx.skip_mode = not ctx.skip_mode
    end
end

-- =============================================================================
--  [auto] ?? toggle auto-advance mode
--  In auto mode the runner advances past click-waits ([p]) after ~1.5s,
--  like a visual-novel auto-play button. State is persisted by [save].
-- =============================================================================

-- Neo-Genesis: explicit mode param (on/off/toggle) beyond KAG3's bare toggle.
schema.define("auto", {
    _meta = { category = "text", blocking = false, desc = "KAG3-compatible auto command" },
    mode = { type = "string", choices = { ["on"] = true, ["off"] = true, ["toggle"] = true } },
})

function TextCommands.auto(ctx, params)
    local m = params.mode or "toggle"
    if m == "on" then ctx.auto_mode = true
    elseif m == "off" then ctx.auto_mode = false
    else ctx.auto_mode = not ctx.auto_mode end
end

-- [voice_off] -- mute/unmute voice without stopping the engine bus
-- (Neo-Genesis convenience: KAG3 needed stopvoice + a saved setting to mute).
schema.define("voice_off", {
    _meta = { category = "text", blocking = false, desc = "KAG3-compatible voice_off command" },
    on = { type = "boolean", default = true },
})

function TextCommands.voice_off(ctx, params)
    ctx.voice_muted = params.on ~= false
end

-- =============================================================================
--  [reset] ?? reset text state
--  Clears line/char_offset tracking and resets backend text renderer.
--  Registered as KAG.reset via auto-iteration in kag.lua.
-- =============================================================================

function TextCommands.reset(ctx, params)
    TextScene.reset(ctx)
    backend.text_reset_state()
    ctx.reveal = nil
    -- [hr] (t202): reset also drops the standing rule.
    TextCommands._hideHr(ctx)
end

-- =============================================================================
--  [pt speed=50] ?? typewriter speed (ms per character)
--  Controls the delay between each character appearing in [ch] / [text].
-- =============================================================================

-- Neo-Genesis contract: typed + clamped (replaces the inline clamps).
require("kag.schema").define("pt", {
    _meta = { category = "text", blocking = false, desc = "point text at position" },
    speed = { type = "number", default = 50, min = 8, max = 5000 },
})

function TextCommands.pt(ctx, params)
    ctx.text_speed = params.speed
end

-- =============================================================================
--  [textspeed cps=50] / [cps 50] ?? KAG3-compatible typewriter speed
--  (chars per second). Overrides the per-character delay that [pt speed=...]
--  sets via ctx.text_speed (ms/char). kag_runner.lua reads ctx.text_speed
--  every frame to advance the typewriter reveal, so this command takes effect
--  for the next [ch]/[text] reveal.
--
--  Unit conversion (KAG3 semantics): cps -> ms-per-char = floor(1000 / cps).
--  KAG3's default message speed is 50 cps (== 20 ms/char); the task-specified
--  valid range is 1..120 cps. floor(1000/120) == 8, the same 8 ms/char floor
--  [pt] enforces, so the two commands stay consistent at the fast end.
--
--  [cps] and [textspeed] are separate contracts (like [delay]/[wait], each
--  independently coerced+clamped by kag/schema); both accept the bare
--  positional form [cps 50] via positional_index = 1.
-- =============================================================================
local DEFAULT_TEXT_CPS = 50   -- KAG3 default (50 chars/sec == 20 ms/char)
local function apply_text_cps(ctx, params, cmd)
    local raw = params.cps
    if raw == nil or raw == "" then
        -- Bare positional ([cps 50]): schema.coerce leaves params.cps nil
        -- when positional_index fills the slot, so fall back to params[1].
        raw = params[1]
    end
    local provided = raw ~= nil and raw ~= ""
    local cps = provided and tonumber(raw) or DEFAULT_TEXT_CPS
    if provided and cps == nil then
        -- A named non-numeric cps errors inside schema.coerce (visible);
        -- a bare positional skips that coercion, so surface it here and fall
        -- back to the default instead of silently zero-ing the speed.
        print(string.format("[kag] [%s]: cps not a number (%s), using default %d",
            cmd, tostring(raw), DEFAULT_TEXT_CPS))
        cps = DEFAULT_TEXT_CPS
    end
    if cps < 1 or cps > 120 then
        local cl = math.max(1, math.min(120, math.floor(cps)))
        print(string.format("[kag] [%s]: cps clamped %s -> %s",
            cmd, tostring(cps), tostring(cl)))
        cps = cl
    end
    ctx.text_speed = math.floor(1000 / cps)  -- real read point (kag_runner)
    ctx.cps = cps                             -- observable KAG3 cps state
end

schema.define("textspeed", {
    _meta = { category = "text", blocking = false,
              desc = "KAG3 typewriter speed: chars per second (overrides [pt] ms/char)" },
    cps = { type = "number", default = DEFAULT_TEXT_CPS, min = 1, max = 120,
            positional_index = 1 },
})

schema.define("cps", {
    _meta = { category = "text", blocking = false,
              desc = "KAG3 [textspeed] alias: chars per second ([cps 50])" },
    cps = { type = "number", default = DEFAULT_TEXT_CPS, min = 1, max = 120,
            positional_index = 1 },
})

function TextCommands.textspeed(ctx, params)
    apply_text_cps(ctx, params, "textspeed")
end

function TextCommands.cps(ctx, params)
    apply_text_cps(ctx, params, "cps")
end

-- =============================================================================
--  [typewriter sound="assets/se/type.wav" interval=1 volume=1.0]
--  Configure typewriter sound effects on character reveal.
--
--  WIRING STATUS: WRITE-ONLY / NOT WIRED. The handler below stores
--  ctx.typewriter_sound / _interval / _volume, and a repo-wide grep for those
--  three keys finds NO reader outside this file -- nothing plays a sound when a
--  character is revealed. The command therefore configures a capability the
--  engine does not have yet. It is kept rather than deleted because the schema
--  and parameter surface are the agreed contract, but it must not be described
--  as working.
--
--  Who should read them, and where -- two points, both outside this batch:
--    1. The reveal advance in scripts/kag_runner.lua update() (:446-455) is the
--       only place that knows a NEW character just became visible: it computes
--       shown = floor(reveal.elapsed / speed) and writes text_scene's
--       reveal_chars. A per-character SE must fire exactly when
--       shown > previous_shown, honoring _interval (once every N chars), and
--       must NOT fire on the instant-reveal paths (skip_mode at kag_runner:484,
--       the first click in on_click() at :692) or a skipped line would
--       machine-gun dozens of SE in a single frame.
--    2. Playback is backend.audio_play("se", file) (scripts/backend.lua:66).
--       That call carries no per-SE volume today (only the bus-level
--       backend.audio_set_bus_volume), so honoring _volume needs either a volume
--       argument on the SE path or an explicit bus-volume ride -- a separate
--       decision, not a detail.
--  Not done in this batch: the scope here is closing out the typography markup;
--  a frame-driven audio trigger in kag_runner is new runtime behavior and needs
--  its own tests for the skip / auto / click instant-reveal interactions.
-- =============================================================================
schema.define("typewriter", {
    _meta = { category = "text", blocking = false, desc = "Configure typewriter sound effects on character reveal" },
    sound    = { type = "string", default = "" },
    file     = { type = "string", default = "" },
    interval = { type = "number", default = 1, min = 1, max = 100 },
    volume   = { type = "number", default = 1.0, min = 0.0, max = 2.0 },
    action   = { type = "enum", values = { ["set"] = true, ["stop"] = true, ["clear"] = true, ["off"] = true }, default = "set" },
    enabled  = { type = "boolean", default = true },
})

schema.define("typewriter_sound", {
    _meta = { category = "text", blocking = false, desc = "Alias for typewriter sound configuration" },
    sound    = { type = "string", default = "" },
    file     = { type = "string", default = "" },
    interval = { type = "number", default = 1, min = 1, max = 100 },
    volume   = { type = "number", default = 1.0, min = 0.0, max = 2.0 },
    action   = { type = "enum", values = { ["set"] = true, ["stop"] = true, ["clear"] = true, ["off"] = true }, default = "set" },
    enabled  = { type = "boolean", default = true },
})

function TextCommands.typewriter(ctx, params)
    params = params or {}
    local action = params.action or "set"
    if action == "stop" or action == "clear" or action == "off" or params.enabled == false then
        ctx.typewriter_sound = ""
        ctx.typewriter_sound_interval = 1
        ctx.typewriter_sound_volume = 1.0
        return
    end
    local sound = params.sound or params.file or ""
    ctx.typewriter_sound = sound
    ctx.typewriter_sound_interval = math.max(1, tonumber(params.interval) or 1)
    ctx.typewriter_sound_volume = tonumber(params.volume) or 1.0
end
TextCommands.typewriter_sound = TextCommands.typewriter


-- =============================================================================
--  [button text="Choice 1" target="*label_a"]
--  [button text="Choice 2" target="*label_b"]
--  [endbutton]
--  Interactive choice buttons. Blocks coroutine until user selects.
--  Each [button] renders a clickable choice. [endbutton] executes the block.
--  On selection, jumps to the target label within current scene.
--
--[[
[R4-FIX] Choice/Branch System Design Note:
The choice system is implemented purely in Lua (no C++ ChoiceController class).
This is intentional - the visual novel choice system is a UI concern that benefits
from Lua's flexibility for layout, styling, and animation.
Architecture:
  1. [button text="..." target="*label"] registers choices into ctx._choiceButtons[]
  2. [endbutton] renders all buttons, blocks the coroutine via coroutine.yield(),
     and jumps to ctx._selectedChoice.target on resume
  3. Intermediate state fields: ctx._choiceButtons (staging), ctx._choiceButtonsActive (active),
     ctx._choiceMode (bool), ctx._selectedChoice (result)
  4. _KAG_onClick is temporarily overridden for hit-testing during choice mode
Future enhancement: extract to a standalone ChoiceController Lua class if complexity grows.
--]]
-- =============================================================================

-- Round 51 contracts: choice/button + skip commands (audit: handlers
-- lacked schema contracts).
schema.define("button", {
    _meta = { category = "text", blocking = false, desc = "register a choice button label ([endbutton] draws it)" },
    text = { type = "string", default = "", interpolate = true },
    caption = { type = "string" },
    target = { type = "string" },
    cond = { type = "string" },
    -- KAG3 [sel x=...]: variable to store this option chosen target
    -- into (e.g. x="tf.result"). Bare key -> f scope ([set] parity).
    x = { type = "string" },
})
schema.define("endbutton", {
    _meta = { category = "text", blocking = true, desc = "draw the registered choice buttons and wait for a pick" },
    target = { type = "string" },
})
schema.define("endselect", {
    _meta = { category = "text", blocking = true, desc = "KAG3 alias of [endbutton]" },
})
schema.define("select", {
    _meta = { category = "text", blocking = false, desc = "begin a choice block ([sel] alias)" },
})
schema.define("skip", {
    _meta = { category = "text", blocking = false, desc = "toggle skip mode (mode=seen skips read text only)" },
    mode = { type = "string" },
})

function TextCommands.button(ctx, params)
    ctx._choiceButtons = ctx._choiceButtons or {}
    local text = params.text or params.caption or ""
    -- Pre-localize label source for the language hot-switch redraw.
    local src_text = text
    -- Localization pipeline (same as [ch]): per-line translation then
    -- {key} token expansion, at registration time so [endbutton] draws
    -- and hit-testing use the localized label. [sel] (KAG3 alias) shares
    -- this handler.
    if #text > 0 then
        text = require("i18n").localize(text, ctx.current_scene)
    end
    -- bare [button *route_a text="..."] -> params[1] as target
    -- (consistency with jump/call/link -- audit)
    local target = params.target or params.storage
    if target == nil and type(params[1]) == "string" then
        target = params[1]
    end
    -- Neo-Genesis: [button cond="f.x > 1"] — conditional choice (Ren'Py
    -- menu `if` parity). Evaluated when [endbutton] renders the block;
    -- false choices are hidden. TJS syntax, runtime-translated.
    -- KAG3 [sel x="tf.result"]: the chosen option target label is
    -- stored into x when [endbutton] completes (engine choice result).
    -- [sel] without x falls back to the jump-only behavior (the rich
    -- result also lives in ctx._selectedChoice for handlers/UI).
    table.insert(ctx._choiceButtons, {
        text = text, target = target or "", cond = params.cond,
        -- Pre-localize label source for the hot-switch redraw.
        src = src_text,
        scene = ctx.current_scene or ctx.currentScene or "",
        x = params.x,
    })
end

function TextCommands.endbutton(ctx, params)
    if not ctx._choiceButtons or #ctx._choiceButtons == 0 then
        ctx._choiceButtons = nil
        return
    end

    -- Neo-Genesis: [button cond=...] — drop choices whose condition is
    -- false (Ren'Py menu `if` parity). All-hidden blocks just dissolve.
    -- cond is compile-time translated (TJS->Lua) in compiled streams;
    -- evaluateTranslated then skips the runtime translate. Hand-built /
    -- uncompiled contexts carry raw TJS (&& || ! != ?:) — detect and
    -- translate on the fly there.
    local exprLang = require("kag.expr")
    local filtered = {}
    for _, choice in ipairs(ctx._choiceButtons) do
        local cond = choice.cond
        if type(cond) == "string" and cond ~= "" then
            local ok, v
            if cond:find("[&|!?]") then
                ok, v = exprLang.evaluate(ctx, cond)
            else
                ok, v = exprLang.evaluateTranslated(ctx, cond, cond)
            end
            if ok and v then filtered[#filtered + 1] = choice end
        else
            filtered[#filtered + 1] = choice
        end
    end
    ctx._choiceButtons = filtered
    if #filtered == 0 then
        ctx._choiceButtons = nil
        return
    end

    -- A visible menu commits a branch boundary. Its suspended operation and
    -- later deferred jump are not part of token snapshots. Empty/all-hidden
    -- blocks above remain ordinary linear flow and keep their history.
    ctx._undoStack = {}

    local operation <close> = Operation.start(ctx)
    local oldClick = _G._KAG_onClick
    local clickHandler
    local cleaned = false
    local function cleanup()
        if cleaned then return end
        cleaned = true
        if clickHandler and _G._KAG_onClick == clickHandler then
            _G._KAG_onClick = oldClick
        end
        ctx._choiceMode = false
        ctx.waiting_input = false
        ctx._choiceButtons = nil
        ctx._choiceButtonsActive = nil
        ctx._selectedChoice = nil
        TextScene.remove_group(ctx, "choices")
    end
    operation.token:register(cleanup)

    -- Store choice draws in the persistent text scene (shared render
    -- path with the language hot-switch redraw).
    _renderChoices(ctx, ctx._choiceButtons)

    -- Install click handler for choice mode
    ctx._choiceButtonsActive = ctx._choiceButtons
    ctx._choiceButtons = nil
    ctx._choiceMode = true
    ctx.waiting_input = true

    -- Override the global click callback for choice detection. The engine
    -- dispatches clicks with no arguments (coalesced); read the mouse
    -- position from the per-frame globals instead.
    clickHandler = function()
        if cleaned or operation.token.cancelled or ctx._session_active == false
            or not ctx._choiceMode then
            return
        end
        local x, y = _G._GAME_MOUSE_X, _G._GAME_MOUSE_Y
        -- Hit-test against button regions
        local buttons = ctx._choiceButtonsActive
        if not buttons then return end
        local vw, _ = require("viewport").wh()
        for _, choice in ipairs(buttons) do
            if y >= (choice.y - 6) and y <= (choice.y + choice.h + 10)
               and x >= 0 and x <= (vw or 1920) then
                ctx._selectedChoice = choice
                ctx._choiceMode = false
                ctx._choiceButtonsActive = nil
                ctx.waiting_input = false
                if _G._KAG_onClick == clickHandler then
                    _G._KAG_onClick = oldClick
                end
                return
            end
        end
    end
    _G._KAG_onClick = clickHandler

    -- Block until user selects
    coroutine.yield()
    
    -- After selection, jump to target label
    local selected = ctx._selectedChoice
    cleanup()
    if operation.token.cancelled or ctx._session_active == false then return end
    
    if selected then
        -- KAG3 [sel x="tf.result"]: write the chosen option target label
        -- into the declared variable (tf./sf./f./mp./lf. scope or bare
        -- key -> f, matching [set]). Guards mirror resolve_var in [set]:
        -- unknown/non-table scope degrades silently.
        if type(selected.x) == "string" and selected.x ~= "" then
            local scopeName, key = selected.x:match("^([%a_]+)%.([%w_]+)$")
            local targetTbl
            if scopeName and ctx[scopeName] then
                targetTbl, key = ctx[scopeName], key
            else
                scopeName, key = "f", selected.x
                targetTbl = ctx[scopeName]
            end
            if type(targetTbl) == "table" and key and key ~= "" then
                targetTbl[key] = selected.target or ""
            end
        end
        if selected.target then
            ctx._pendingJump = selected.target
        end
    end
    operation:complete()
end

-- KAG3 select syntax: [select] opens the block (no-op), [sel] registers
-- an option (same fields as [button]), [endselect] renders + blocks
-- (same as [endbutton]). The choice engine is shared; no schema is
-- needed (button itself is unmigrated -- raw params pass through).
function TextCommands.select(ctx, params)
    ctx._choiceButtons = ctx._choiceButtons or {}
end

TextCommands.sel = TextCommands.button

function TextCommands.endselect(ctx, params)
    return TextCommands.endbutton(ctx, params)
end

-- =============================================================================
--  Language hot-switch full-page redraw (round 16).
--
--  Settings language cycle calls i18n.load then relocalize_page: the
--  already-displayed page (message window / NVL accumulated block), the
--  backlog, active choice labels and closed captions are re-localized
--  with the NEW language and re-rendered in place. This exceeds Ren'Py,
--  which keeps already-displayed lines in the original language.
--
--  Replay source: every [ch]/[text] records its pre-localize message
--  plus the exact layout values into text_state.page_src (parallel to
--  draws; cleared by TextScene.clear). Draws produced by the display
--  path carry the _page_src marker so a redraw drops exactly the old
--  page (foreign draws like [ruby] survive) and replays it. Redrawn
--  lines are sealed (fully revealed, like TextScene.commit).
-- =============================================================================

-- Re-localize backlog entries in place (entries without a src -- e.g.
-- older saves -- keep their stored text).
function TextCommands.relocalize_backlog(ctx)
    local bl = ctx.backlog
    if type(bl) ~= "table" then return false end
    local i18nMod = require("i18n")
    for _, entry in ipairs(bl) do
        if type(entry.src) == "string" and #entry.src > 0 then
            entry.text = i18nMod.localize(
                entry.src, entry.scene or ctx.current_scene)
        end
    end
    return true
end

-- Re-localize choice labels: the staging list ([button]... before
-- [endbutton]) and the active block. The active group is re-rendered
-- (button y/h regions are preserved, so hit-testing stays valid).
function TextCommands._relocalizeChoices(ctx)
    local i18nMod = require("i18n")
    local staging = ctx._choiceButtons
    if type(staging) == "table" then
        for _, choice in ipairs(staging) do
            if type(choice.src) == "string" and #choice.src > 0 then
                choice.text = i18nMod.localize(
                    choice.src, choice.scene or ctx.current_scene)
            end
        end
    end
    local active = ctx._choiceButtonsActive
    if type(active) == "table" and #active > 0 then
        for _, choice in ipairs(active) do
            if type(choice.src) == "string" and #choice.src > 0 then
                choice.text = i18nMod.localize(
                    choice.src, choice.scene or ctx.current_scene)
            end
        end
        _renderChoices(ctx, active)
    end
end

-- Re-localize the standing closed caption (cc_mode), stripping markup
-- from the new text the same way [ch] strips the plain form.
function TextCommands._relocalizeCC(ctx)
    local cc = ctx.cc_text
    if type(cc) ~= "table" or type(cc.src) ~= "string" or #cc.src == 0 then
        return
    end
    local localized = require("i18n").localize(
        cc.src, cc.scene or ctx.current_scene)
    local plain = localized
    if localized:find("{", 1, true) then
        plain = TextLayout.parse_markup(localized).plain
    end
    cc.text = plain
end

--- TextCommands.relocalize_page(ctx) — full-page redraw after a
--  language hot-switch. Returns true when anything was re-rendered;
--  safe on bare/degraded contexts (callers wrap in pcall).
function TextCommands.relocalize_page(ctx)
    if type(ctx) ~= "table" then return false end
    local state = TextScene.get_state(ctx)
    local sources = state.page_src or {}

    if #sources > 0 then
        -- Drop the old page draws (marked at display time) and replay
        -- every recorded source with the new language. A translation may
        -- wrap to a different line count than the original: keep a
        -- cumulative y-delta and shift each later line by it (layout
        -- cascade). Entries are never mutated, so snapshots of the page
        -- sources stay valid across redraws.
        local kept = {}
        for _, draw in ipairs(state.draws) do
            if not draw._page_src then kept[#kept + 1] = draw end
        end
        state.draws = kept

        local cascade = 0
        local lastEnd, lastMsgX = nil, nil
        for _, entry in ipairs(sources) do
            local opts = entry.opts or {}
            local origEnd = opts.msgY
            lastMsgX = opts.msgX
            local newEnd = _drawMessage(ctx, entry, (origEnd or 0) + cascade)
            if origEnd and newEnd then
                cascade = cascade + (newEnd - origEnd)
            end
            lastEnd = newEnd
        end

        -- Seal every redrawn line: no typewriter replay after the switch
        -- (the player returned from the settings menu; the page is shown
        -- in full, like TextScene.commit).
        for _, draw in ipairs(state.draws) do
            draw.typewriter = false
            draw._shown = nil
            draw._shown_len = nil
        end

        -- Cursor mirrors: the next [ch]/[text] (NVL accumulation) must
        -- continue below the re-laid-out page.
        if lastEnd then
            ctx.textCursorY = lastEnd
            state.cursor_y = lastEnd
            if lastMsgX then ctx.textCursorX = lastMsgX end
        end
    end

    TextCommands.relocalize_backlog(ctx)
    TextCommands._relocalizeChoices(ctx)
    TextCommands._relocalizeCC(ctx)
    return true
end

-- =============================================================================
-- [input] — Interactive Text Input with Virtual Keyboard & IME Support
-- =============================================================================

function TextCommands.input(ctx, params)
    -- 1. Condition check: skip if condition evaluates to false
    if params.cond and type(params.cond) == "string" and params.cond ~= "" then
        local exprLang = require("kag.expr")
        local ok, v
        if params.cond:find("[&|!?]") then
            ok, v = exprLang.evaluate(ctx, params.cond)
        else
            ok, v = exprLang.evaluateTranslated(ctx, params.cond, params.cond)
        end
        if not (ok and v) then
            return
        end
    end

    local var_name = params.name
    if not var_name or var_name == "" then
        return
    end

    local max_len = tonumber(params.maxlen or params.max_length) or 32
    local buffer = tostring(params.default or "")
    local comp_text = ""
    local prompt_text = tostring(params.prompt or "")
    local is_password = params.password == true
    local btn_ok_label = params.btn_ok or "OK"
    local btn_cancel_label = params.btn_cancel or ""

    -- 2. Viewport & Dimensions with Adaptive Upper Placement
    local vw, vh = 1280, 720
    local ok_vp, vp = pcall(require, "viewport")
    if ok_vp and vp and vp.wh then
        vw, vh = vp.wh()
    end
    local box_w = tonumber(params.width) or 640
    local box_h = tonumber(params.height) or 180
    local box_x = tonumber(params.x) or 0
    if box_x <= 0 then
        box_x = math.floor((vw - box_w) / 2)
    end
    local box_y = tonumber(params.y) or 0
    if box_y <= 0 then
        box_y = math.floor(vh * 0.22)
    end
    -- Virtual keyboard occlusion prevention: upper viewport bound y + box_h <= 0.45 * vh
    local max_allowed_y = math.floor(vh * 0.45 - box_h)
    if max_allowed_y < 20 then max_allowed_y = 20 end
    box_y = math.max(0, math.min(box_y, max_allowed_y))

    -- Own callbacks and native input before the first fallible UI/backend call.
    local operation <close> = Operation.start(ctx)
    local previousHandlers = {
        _KAG_onTextInput = _G._KAG_onTextInput,
        _KAG_onTextEditing = _G._KAG_onTextEditing,
        _KAG_onKeyDown = _G._KAG_onKeyDown,
        _KAG_onClick = _G._KAG_onClick,
    }
    local inputHandlers = {}
    local cleaned = false
    local nativeInputStarted = false
    local function cleanup()
        if cleaned then return end
        cleaned = true
        for name, handler in pairs(inputHandlers) do
            if _G[name] == handler then _G[name] = previousHandlers[name] end
        end
        ctx._inputMode = false
        ctx.waiting_input = false
        TextScene.remove_group(ctx, "text_input")
        if nativeInputStarted then
            nativeInputStarted = false
            backend.stop_text_input()
        end
    end
    operation.token:register(cleanup)

    -- 3. Notify platform backend
    backend.set_text_input_rect(box_x, box_y, box_w, box_h, 0)
    nativeInputStarted = true
    backend.start_text_input()

    -- UTF-8 Helpers
    local function utf8_length(s)
        if not s or #s == 0 then return 0 end
        if type(utf8) == "table" and utf8.len then
            local l = utf8.len(s)
            if l then return l end
        end
        local count = 0
        for i = 1, #s do
            local b = s:byte(i)
            if b < 0x80 or b >= 0xC0 then count = count + 1 end
        end
        return count
    end

    local function utf8_pop(s)
        if not s or #s == 0 then return "" end
        if type(utf8) == "table" and utf8.offset then
            local p = utf8.offset(s, -1)
            if p then return s:sub(1, p - 1) end
        end
        local i = #s
        while i > 1 and s:byte(i) >= 0x80 and s:byte(i) < 0xC0 do
            i = i - 1
        end
        return s:sub(1, i - 1)
    end

    -- 4. UI Layout & Button Rectangles
    local btn_ok_rect = {
        x = box_x + box_w - 120,
        y = box_y + box_h - 44,
        w = 100,
        h = 36,
    }
    local btn_cancel_rect = {
        x = box_x + box_w - 240,
        y = box_y + box_h - 44,
        w = 100,
        h = 36,
    }

    local function redraw_ui()
        TextScene.remove_group(ctx, "text_input")
        if #prompt_text > 0 then
            TextScene.add_text(ctx, prompt_text, box_x + 16, box_y + 16,
                { r = 220, g = 220, b = 220, a = 255 }, "text_input", 1, true, false, true)
        end
        local display_buf = is_password and string.rep("*", utf8_length(buffer)) or buffer
        local full_line = display_buf
        if #comp_text > 0 then
            full_line = full_line .. "[" .. comp_text .. "]"
        end
        full_line = full_line .. "|"
        TextScene.add_text(ctx, full_line, box_x + 20, box_y + 64,
            { r = 255, g = 255, b = 255, a = 255 }, "text_input", 1, false, false, true)
        TextScene.add_text(ctx, "[" .. btn_ok_label .. "]", btn_ok_rect.x, btn_ok_rect.y,
            { r = 100, g = 255, b = 100, a = 255 }, "text_input", 1, true, false, true)
        if #btn_cancel_label > 0 then
            TextScene.add_text(ctx, "[" .. btn_cancel_label .. "]", btn_cancel_rect.x, btn_cancel_rect.y,
                { r = 255, g = 100, b = 100, a = 255 }, "text_input", 1, true, false, true)
        end
    end

    redraw_ui()

    ctx._inputMode = true
    ctx.waiting_input = true

    local function cleanup_and_finish(save_result)
        if cleaned or operation.token.cancelled or ctx._session_active == false then return end
        cleanup()

        if save_result then
            local scopeName, key = var_name:match("^([%a_]+)%.([%w_]+)$")
            local targetTbl
            if scopeName and ctx[scopeName] then
                targetTbl, key = ctx[scopeName], key
            else
                scopeName, key = "f", var_name
                targetTbl = ctx[scopeName]
            end
            if type(targetTbl) == "table" and key and key ~= "" then
                targetTbl[key] = buffer
            end
        end
    end

    inputHandlers._KAG_onTextInput = function(text)
        if cleaned or operation.token.cancelled or ctx._session_active == false
            or not ctx._inputMode then
            return
        end
        if type(text) == "string" and #text > 0 then
            local current_len = utf8_length(buffer)
            local append_len = utf8_length(text)
            if current_len + append_len <= max_len then
                buffer = buffer .. text
            else
                local allowed = max_len - current_len
                if allowed > 0 then
                    if type(utf8) == "table" and utf8.codes and utf8.char then
                        local added = 0
                        for _, cp in utf8.codes(text) do
                            if added < allowed then
                                buffer = buffer .. utf8.char(cp)
                                added = added + 1
                            else
                                break
                            end
                        end
                    else
                        local count = 0
                        local byte_idx = #text
                        for i = 1, #text do
                            local b = text:byte(i)
                            if b < 0x80 or b >= 0xC0 then
                                count = count + 1
                                if count > allowed then
                                    byte_idx = i - 1
                                    break
                                end
                            end
                        end
                        buffer = buffer .. text:sub(1, byte_idx)
                    end
                end
            end
            comp_text = ""
            redraw_ui()
        end
    end

    inputHandlers._KAG_onTextEditing = function(text, start, length)
        if cleaned or operation.token.cancelled or ctx._session_active == false
            or not ctx._inputMode then
            return
        end
        comp_text = tostring(text or "")
        redraw_ui()
    end

    inputHandlers._KAG_onKeyDown = function(keyCode, keyName)
        if cleaned or operation.token.cancelled or ctx._session_active == false
            or not ctx._inputMode then
            return
        end
        if keyName == "backspace" or keyCode == 8 or keyCode == 0x08 then
            -- The IME owns edits to active preedit. Only its subsequent
            -- TextEditing notification changes that preview; repeated keys
            -- before the notification must not delete committed characters.
            if #comp_text == 0 then
                buffer = utf8_pop(buffer)
                redraw_ui()
            end
        elseif keyName == "return" or keyCode == 13 or keyCode == 0x0D then
            -- Candidate confirmation is not submission of the whole form.
            if #comp_text == 0 then cleanup_and_finish(true) end
        elseif keyName == "escape" or keyCode == 27 or keyCode == 0x1B then
            if #comp_text > 0 then
                comp_text = ""
                redraw_ui()
            else
                cleanup_and_finish(false)
            end
        end
    end

    inputHandlers._KAG_onClick = function()
        if cleaned or operation.token.cancelled or ctx._session_active == false
            or not ctx._inputMode then
            return
        end
        local mx = _G._GAME_MOUSE_X or 0
        local my = _G._GAME_MOUSE_Y or 0
        if mx >= btn_ok_rect.x and mx <= (btn_ok_rect.x + btn_ok_rect.w) and
           my >= (btn_ok_rect.y - 6) and my <= (btn_ok_rect.y + btn_ok_rect.h + 6) then
            cleanup_and_finish(true)
            return
        end
        if #btn_cancel_label > 0 and
           mx >= btn_cancel_rect.x and mx <= (btn_cancel_rect.x + btn_cancel_rect.w) and
           my >= (btn_cancel_rect.y - 6) and my <= (btn_cancel_rect.y + btn_cancel_rect.h + 6) then
            cleanup_and_finish(false)
            return
        end
    end
    for name, handler in pairs(inputHandlers) do _G[name] = handler end

    coroutine.yield()

    if operation.token.cancelled or ctx._session_active == false then return end
    if not cleaned then
        cleanup_and_finish(true)
    end
    operation:complete()
end

TextCommands.edit = TextCommands.input

return TextCommands
