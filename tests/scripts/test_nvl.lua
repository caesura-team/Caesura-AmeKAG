-- test_nvl.lua — NVL mode (full-screen accumulated text, Ren'Py parity):
--   [nvl] enter / [nvl clear] page break / [nvl off] exit; [ch]/[text]
--   accumulate lines below the cursor instead of replacing the message
--   window; [p] resets the page; nvl_mode persists via save + snapshot.
local results = {}
local function check(name, cond)
    if cond then print("PASS " .. name) else print("FAIL " .. name) end
    results[#results + 1] = cond
end

-- Intercept the engine backend so render/clear calls are observable.
-- Save + restore the global: the suite runs every file in one process,
-- so a leaked mock would change later tests' backend resolution.
local saved_backend = rawget(_G, "_CAESURA_BACKEND")
local backend_guard <close> = setmetatable({}, { __close = function()
    rawset(_G, "_CAESURA_BACKEND", saved_backend)
end })
local calls = { render_text = 0, clear_text = 0 }
_G._CAESURA_BACKEND = {
    render = function(method, ...)
        if method == "render_text" then
            calls.render_text = calls.render_text + 1
        elseif method == "clear_text" then
            calls.clear_text = calls.clear_text + 1
        elseif method == "line_height" then
            return 24
        elseif method == "create_viewport" or method == "create_solid_texture" then
            -- This text-only host owns no GPU resources. A boolean is not a
            -- render handle and cannot participate in presentation capture.
            return 0
        end
        return true
    end,
    platform = function(...) return true end,
    audio = function(...) return true end,
}

package.path = "scripts/?.lua;scripts/kag/?.lua;" .. package.path
local TextScene = require("kag.text_scene")
local TextCommands = require("kag.commands.text")
local Schema = require("kag.schema")
local snapshot = require("kag.snapshot")
local layers = require("layers")
-- Shared-suite predecessors own different synthetic textures. This text test
-- starts with its own empty scene, just like a new runner session.
assert(layers.clear_for_restore())

-- Seed the message layer so [ch] does not allocate one during the test.
layers.add_layer(nil, {
    name = "message", layer_type = 0,
    x = 0, y = 520, w = 1280, h = 200, visible = true,
})

local function fresh_ctx()
    return {
        backlog = {},
        f = {}, sf = {}, tf = {}, mp = {}, variables = {},
        characters = {},
        current_scene = "scene.ks", currentScene = "scene.ks", token_index = 1,
        text_state = { line = 1, char_offset = 0, opacity = 255,
                       cursor_x = 32, cursor_y = 580, draws = {} },
        textCursorX = 32, textCursorY = 580,
    }
end

-- ---------------------------------------------------------------------------
-- 1. [nvl] contract + mode toggling
-- ---------------------------------------------------------------------------
check("nvl migrated", Schema.isMigrated("nvl"))
local p0 = Schema.coerce("nvl", {}, {})
check("nvl bare (no mode)", p0.mode == nil)
local pc = Schema.coerce("nvl", { mode = "clear" }, {})
check("nvl mode=clear coerced", pc.mode == "clear")

local ctx = fresh_ctx()
TextCommands.nvl(ctx, {})
check("nvl enter sets mode", ctx.nvl_mode == true)
check("nvl enter resets cursor",
      ctx.textCursorY == 160 and ctx.text_state.cursor_y == 160)

TextCommands.nvl(ctx, { mode = "off" })
check("nvl off clears mode", ctx.nvl_mode == false)

-- bare positional [nvl clear] (params[1])
local ctxP = fresh_ctx()
TextCommands.nvl(ctxP, { [1] = "clear" })
check("nvl bare clear enters + resets",
      ctxP.nvl_mode == true and ctxP.textCursorY == 160)

-- ---------------------------------------------------------------------------
-- 2. accumulation (narration) vs replacement
-- ---------------------------------------------------------------------------
calls.clear_text = 0
local ctxA = fresh_ctx()
TextCommands.nvl(ctxA, {})
TextCommands.text(ctxA, { text = "first" })
TextCommands.text(ctxA, { text = "second" })
check("nvl accumulates two lines", #ctxA.text_state.draws == 2)
check("nvl cursor advanced two lines",
      ctxA.textCursorY == 160 + 24 + 24)

-- clear_text happens once (the [nvl] enter), never per appended line
check("nvl append does not clear", calls.clear_text == 1)

calls.clear_text = 0
local ctxN = fresh_ctx()
TextCommands.text(ctxN, { text = "first" })
TextCommands.text(ctxN, { text = "second" })
check("normal mode replaces (one line)", #ctxN.text_state.draws == 1)
check("normal mode clears each line", calls.clear_text == 2)

-- ---------------------------------------------------------------------------
-- 3. [nvl clear] page break
-- ---------------------------------------------------------------------------
local ctxK = fresh_ctx()
TextCommands.nvl(ctxK, {})
TextCommands.text(ctxK, { text = "first" })
TextCommands.nvl(ctxK, { mode = "clear" })
check("nvl clear empties the page", #ctxK.text_state.draws == 0)
check("nvl clear resets cursor", ctxK.textCursorY == 160
      and ctxK.text_state.cursor_y == 160)

-- ---------------------------------------------------------------------------
-- 4. [p] page break resets the NVL cursor (blocking command needs a coro)
-- ---------------------------------------------------------------------------
local ctxB = fresh_ctx()
TextCommands.nvl(ctxB, {})
TextCommands.text(ctxB, { text = "first" })
local co = coroutine.create(function() TextCommands.p(ctxB, {}) end)
coroutine.resume(co)  -- runs to the yield (waiting for the click)
check("p sets waiting_input", ctxB.waiting_input == true)
coroutine.resume(co)  -- resumes past the yield: clear + page reset
check("p resets nvl cursor", ctxB.textCursorY == 160
      and ctxB.text_state.cursor_y == 160)

-- ---------------------------------------------------------------------------
-- 5. [ch] in NVL: speaker as 「Name」： inline prefix span (Ren'Py NVL
--    style). The prefix is an instant draw prepended to the message spans
--    (wraps with the line; no direct render_text during ch — submitted by
--    TextScene.render). Empty-message [ch] keeps the standalone label.
-- ---------------------------------------------------------------------------
calls.render_text = 0
local ctxC = fresh_ctx()
ctxC.nameplate_style = { text_color = "255,255,255" }
TextCommands.nvl(ctxC, {})
TextCommands.ch(ctxC, { name = "Ame", text = "hi" })
check("nvl ch no direct render_text", calls.render_text == 0)
check("nvl ch prefix + message draws", #ctxC.text_state.draws == 2)
local dP, dM = ctxC.text_state.draws[1], ctxC.text_state.draws[2]
check("nvl prefix draw instant + styled",
      dP.text == "「Ame」：" and dP.typewriter == false
      and dP.r == 255 and dP.g == 255 and dP.b == 255)
check("nvl message draw typewriter",
      dM.text == "hi" and dM.typewriter == true)
check("nvl backlog plain excludes prefix",
      ctxC.backlog[1] ~= nil and ctxC.backlog[1].text == "hi")

calls.render_text = 0
local ctxE = fresh_ctx()
ctxE.nameplate_style = { text_color = "255,255,255" }
TextCommands.nvl(ctxE, {})
TextCommands.ch(ctxE, { name = "Ame" })
check("nvl empty message keeps standalone label", calls.render_text == 1)

-- 5b. [nvl prefix="..."] customizes the speaker prefix format
local ctxP = fresh_ctx()
ctxP.nameplate_style = { text_color = "255,255,255" }
TextCommands.nvl(ctxP, { prefix = "%s: " })
TextCommands.ch(ctxP, { name = "Ame", text = "hi" })
check("nvl custom prefix format", ctxP.text_state.draws[1] ~= nil
      and ctxP.text_state.draws[1].text == "Ame: ")

-- custom brackets
local ctxQ = fresh_ctx()
TextCommands.nvl(ctxQ, { prefix = "（%s）" })
TextCommands.ch(ctxQ, { name = "Ame", text = "hi" })
check("nvl custom brackets", ctxQ.text_state.draws[1] ~= nil
      and ctxQ.text_state.draws[1].text == "（Ame）")

-- stray % in the speaker name must not break the substitution
local ctxR = fresh_ctx()
TextCommands.nvl(ctxR, {})
TextCommands.ch(ctxR, { name = "100% Girl", text = "hi" })
check("nvl prefix survives % in name", ctxR.text_state.draws[1] ~= nil
      and ctxR.text_state.draws[1].text == "「100% Girl」：")

-- prefix persists across lines until changed
local ctxS2 = fresh_ctx()
TextCommands.nvl(ctxS2, { prefix = "%s: " })
TextCommands.ch(ctxS2, { name = "Ame", text = "one" })
TextCommands.ch(ctxS2, { name = "B", text = "two" })
check("nvl prefix persists", ctxS2.text_state.draws[3] ~= nil
      and ctxS2.text_state.draws[3].text == "B: ")

-- ---------------------------------------------------------------------------
-- 6. commit seals prior lines (typewriter only animates the appended line)
-- ---------------------------------------------------------------------------
local ctxM = fresh_ctx()
TextCommands.nvl(ctxM, {})
TextCommands.text(ctxM, { text = "first" })
TextCommands.text(ctxM, { text = "second" })
check("commit seals prior line", ctxM.text_state.draws[1].typewriter == false)
check("appended line still typewriter", ctxM.text_state.draws[2].typewriter == true)

-- ---------------------------------------------------------------------------
-- 7. nvl_mode persists across snapshot (rollback) round-trip
-- ---------------------------------------------------------------------------
local ctxS = {
    current_scene = "demo/x.ks", currentScene = "demo/x.ks",
    token_index = 1,
    f = {}, sf = {}, tf = {}, mp = {}, variables = {},
    backlog = {}, seen_scenes = {}, call_stack = {},
    text_speed = 40, skip_mode = false, auto_mode = false,
    nvl_mode = true, waiting_input = false,
    reveal = { total = 3, elapsed = 3 },
    text_state = { line = 1, char_offset = 0, cursor_x = 48, cursor_y = 160, draws = {} },
}
local snap = snapshot.capture(ctxS)
ctxS.nvl_mode = false
local okRestore = snapshot.restore(ctxS, snap)
check("snapshot restores nvl_mode", okRestore == true and ctxS.nvl_mode == true)

-- ---------------------------------------------------------------------------
-- 8. [nvl] / [textbox] mutual exclusion (phase D): full-screen NVL text has
--    no fixed message window, so entering NVL must hide the _textbox /_
--    _nameplate layers and [nvl off] must restore their prior visibility.
-- ---------------------------------------------------------------------------
local ctxT = fresh_ctx()
local tbp = Schema.coerce("textbox", {}, ctxT)
TextCommands.textbox(ctxT, tbp)
local tbNode = layers.get("_textbox")
check("textbox layer visible before nvl", tbNode ~= nil and tbNode.visible == true)

TextCommands.nvl(ctxT, {})
tbNode = layers.get("_textbox")
check("nvl hides the textbox layer", tbNode ~= nil and tbNode.visible == false)

-- [nvl clear] re-enters and keeps it hidden
TextCommands.text(ctxT, { text = "line" })
TextCommands.nvl(ctxT, { mode = "clear" })
tbNode = layers.get("_textbox")
check("nvl clear keeps textbox hidden", tbNode ~= nil and tbNode.visible == false)

TextCommands.nvl(ctxT, { mode = "off" })
tbNode = layers.get("_textbox")
check("nvl off restores the textbox", tbNode ~= nil and tbNode.visible == true)

-- textbox configured invisible beforehand stays hidden after nvl off
local ctxT2 = fresh_ctx()
TextCommands.textbox(ctxT2, Schema.coerce("textbox", { visible = false }, ctxT2))
TextCommands.nvl(ctxT2, {})
TextCommands.nvl(ctxT2, { mode = "off" })
check("nvl off keeps a pre-hidden textbox hidden",
      layers.get("_textbox") ~= nil and layers.get("_textbox").visible == false)

-- ---------------------------------------------------------------------------
-- cleanup: restore the backend global the way we found it
-- ---------------------------------------------------------------------------
rawset(_G, "_CAESURA_BACKEND", saved_backend)

local failed = 0
for _, ok in ipairs(results) do
    if not ok then failed = failed + 1 end
end
if failed > 0 then
    print(string.format("NVL TESTS: %d passed, %d FAILED",
        #results - failed, failed))
    os.exit(1)
end
print(string.format("NVL TESTS DONE (%d passed)", #results))
