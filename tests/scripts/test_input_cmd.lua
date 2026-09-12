-- test_input_cmd.lua — unit test suite for [input] / [edit] commands and IME integration

local results = {}
local check = function(name, cond)
    if cond then print("PASS " .. name) else print("FAIL " .. name) end
    results[#results + 1] = cond
end

package.path = "scripts/?.lua;scripts/kag/?.lua;" .. package.path

local Schema = require("kag.schema")
local TextCommands = require("kag.commands.text")
local TextScene = require("kag.text_scene")
local backend = require("backend")

-- 1. Schema Registration & Coercion
check("input schema defined", Schema.isMigrated("input"))
check("edit schema defined", Schema.isMigrated("edit"))

local coerced = Schema.coerce("input", { name = "f.hero", default = "Ame", maxlen = "16" })
check("schema coerces maxlen to number", type(coerced.maxlen) == "number" and coerced.maxlen == 16)
check("schema retains name", coerced.name == "f.hero")

-- 2. Mock Platform Backend Tracking
local ime_active = false
local ime_rect = nil
local original_start = backend.start_text_input
local original_stop = backend.stop_text_input
local original_set_rect = backend.set_text_input_rect

backend.start_text_input = function() ime_active = true return true end
backend.stop_text_input = function() ime_active = false return true end
backend.set_text_input_rect = function(x, y, w, h, c) ime_rect = { x = x, y = y, w = w, h = h, c = c } return true end

local ctx = {
    f = {}, sf = {}, tf = {}, mp = {},
    text_state = { line = 1, char_offset = 0, opacity = 255, cursor_x = 32, cursor_y = 580, draws = {} },
    textCursorX = 32, textCursorY = 580,
    backlog = {}, layers = {},
}

-- 3. Execution & UI Initialization
local co = coroutine.create(function()
    TextCommands.input(ctx, { name = "f.player_name", prompt = "Name:", default = "Hero", maxlen = 10, y = 800 })
end)
coroutine.resume(co)

check("input mode active", ctx._inputMode == true)
check("waiting_input flag set", ctx.waiting_input == true)
check("IME started", ime_active == true)
check("IME rect assigned", ime_rect ~= nil)
check("adaptive upper viewport positioning (y <= 0.45 * height)", ime_rect.y <= math.floor(720 * 0.45) or ime_rect.y <= math.floor(1080 * 0.45))

-- 4. Interactive Typing & Backspace Simulation
check("text input handler installed", type(_G._KAG_onTextInput) == "function")
check("key down handler installed", type(_G._KAG_onKeyDown) == "function")

-- Append text
_G._KAG_onTextInput("ine") -- "Heroine"
-- Backspace twice
_G._KAG_onKeyDown(8, "backspace") -- "Heroin"
_G._KAG_onKeyDown(8, "backspace") -- "Heroi"

-- 5. Commit via Enter Key
_G._KAG_onKeyDown(13, "return")

check("input mode cleared after enter", ctx._inputMode == false)
check("waiting_input cleared", ctx.waiting_input == false)
check("IME stopped after commit", ime_active == false)
check("variable assigned in f scope", ctx.f.player_name == "Heroi")
check("UI elements removed", #TextScene.get_state(ctx).draws == 0)

-- 6. Password Masking & Scope Validation
local ctx2 = {
    f = {}, sf = {}, tf = {}, mp = {},
    text_state = { line = 1, char_offset = 0, opacity = 255, cursor_x = 32, cursor_y = 580, draws = {} },
    textCursorX = 32, textCursorY = 580,
}

local co2 = coroutine.create(function()
    TextCommands.input(ctx2, { name = "tf.pwd", prompt = "PIN:", default = "1234", password = true })
end)
coroutine.resume(co2)

local draws = TextScene.get_state(ctx2).draws
local has_masked_display = false
for _, d in ipairs(draws) do
    if d.text and d.text:find("%*%*%*%*") then
        has_masked_display = true
        break
    end
end
check("password masked in UI display", has_masked_display)

_G._KAG_onKeyDown(13, "return")
check("plaintext stored in tf scope", ctx2.tf.pwd == "1234")

-- 7. Mouse click OK and Cancel simulation
local ctx3 = {
    f = {}, sf = {}, tf = {}, mp = {},
    text_state = { line = 1, char_offset = 0, opacity = 255, cursor_x = 32, cursor_y = 580, draws = {} },
    textCursorX = 32, textCursorY = 580,
}
local co3 = coroutine.create(function()
    TextCommands.input(ctx3, { name = "f.nick", prompt = "Nickname:", default = "Alice", btn_ok = "Confirm", btn_cancel = "Dismiss" })
end)
coroutine.resume(co3)
check("input mode active in ctx3", ctx3._inputMode == true)
_G._GAME_MOUSE_X = ime_rect.x + ime_rect.w - 100
_G._GAME_MOUSE_Y = ime_rect.y + ime_rect.h - 30
_G._KAG_onClick()
check("click OK saves result", ctx3.f.nick == "Alice")
check("input mode cleared after click OK", ctx3._inputMode == false)

-- Cancel button test
local ctx4 = {
    f = {}, sf = {}, tf = {}, mp = {},
    text_state = { line = 1, char_offset = 0, opacity = 255, cursor_x = 32, cursor_y = 580, draws = {} },
    textCursorX = 32, textCursorY = 580,
}
local co4 = coroutine.create(function()
    TextCommands.input(ctx4, { name = "f.canceled", default = "Initial", btn_ok = "OK", btn_cancel = "Cancel" })
end)
coroutine.resume(co4)
_G._KAG_onTextInput("_extra")
_G._GAME_MOUSE_X = ime_rect.x + ime_rect.w - 200
_G._GAME_MOUSE_Y = ime_rect.y + ime_rect.h - 30
_G._KAG_onClick()
check("click Cancel does not save result", ctx4.f.canceled == nil)
check("input mode cleared after click Cancel", ctx4._inputMode == false)

-- 8. Viewport Boundary Clamping Verification (720p & 1080p)
local original_viewport = package.loaded["viewport"]

-- 720p tests (vw = 1280, vh = 720, box_h = 180)
package.loaded["viewport"] = { wh = function() return 1280, 720 end }

-- 720p: Default y (omitted or <= 0) should clamp to math.floor(720 * 0.45 - 180) = 144
local ctx_vp1 = { f = {}, sf = {}, tf = {}, mp = {}, text_state = { draws = {} } }
local co_vp1 = coroutine.create(function()
    TextCommands.input(ctx_vp1, { name = "f.v720_def" })
end)
coroutine.resume(co_vp1)
check("720p default y clamped to 144", ime_rect.y == 144)
check("720p default y upper viewport bound satisfied (y + h <= 324)", ime_rect.y + ime_rect.h <= math.floor(720 * 0.45))
_G._KAG_onKeyDown(13, "return")

-- 720p: Explicit overflowing y = 500 should clamp to 144
local ctx_vp2 = { f = {}, sf = {}, tf = {}, mp = {}, text_state = { draws = {} } }
local co_vp2 = coroutine.create(function()
    TextCommands.input(ctx_vp2, { name = "f.v720_high", y = 500 })
end)
coroutine.resume(co_vp2)
check("720p explicit y=500 clamped to 144", ime_rect.y == 144)
check("720p explicit y=500 upper viewport bound satisfied", ime_rect.y + ime_rect.h <= math.floor(720 * 0.45))
_G._KAG_onKeyDown(13, "return")

-- 720p: Explicit valid y = 60 (< 144) should stay 60
local ctx_vp3 = { f = {}, sf = {}, tf = {}, mp = {}, text_state = { draws = {} } }
local co_vp3 = coroutine.create(function()
    TextCommands.input(ctx_vp3, { name = "f.v720_low", y = 60 })
end)
coroutine.resume(co_vp3)
check("720p explicit valid y=60 preserved", ime_rect.y == 60)
_G._KAG_onKeyDown(13, "return")

-- 1080p tests (vw = 1920, vh = 1080, box_h = 180)
package.loaded["viewport"] = { wh = function() return 1920, 1080 end }

-- 1080p: Default y = math.floor(1080 * 0.22) = 237 (since 237 <= math.floor(1080 * 0.45 - 180) = 306)
local ctx_vp4 = { f = {}, sf = {}, tf = {}, mp = {}, text_state = { draws = {} } }
local co_vp4 = coroutine.create(function()
    TextCommands.input(ctx_vp4, { name = "f.v1080_def" })
end)
coroutine.resume(co_vp4)
check("1080p default y is 237", ime_rect.y == 237)
check("1080p default y upper viewport bound satisfied (y + h <= 486)", ime_rect.y + ime_rect.h <= math.floor(1080 * 0.45))
_G._KAG_onKeyDown(13, "return")

-- 1080p: Explicit overflowing y = 800 should clamp to 306
local ctx_vp5 = { f = {}, sf = {}, tf = {}, mp = {}, text_state = { draws = {} } }
local co_vp5 = coroutine.create(function()
    TextCommands.input(ctx_vp5, { name = "f.v1080_high", y = 800 })
end)
coroutine.resume(co_vp5)
check("1080p explicit y=800 clamped to 306", ime_rect.y == 306)
check("1080p explicit y=800 upper viewport bound satisfied", ime_rect.y + ime_rect.h <= math.floor(1080 * 0.45))
_G._KAG_onKeyDown(13, "return")

-- 1080p: Explicit valid y = 100 (< 306) should stay 100
local ctx_vp6 = { f = {}, sf = {}, tf = {}, mp = {}, text_state = { draws = {} } }
local co_vp6 = coroutine.create(function()
    TextCommands.input(ctx_vp6, { name = "f.v1080_low", y = 100 })
end)
coroutine.resume(co_vp6)
check("1080p explicit valid y=100 preserved", ime_rect.y == 100)
_G._KAG_onKeyDown(13, "return")

package.loaded["viewport"] = original_viewport

-- 9. Strict Sandbox Mode Integration Verification (_SANDBOX_MODE == "strict")
local ok_sb, sandbox = pcall(require, "sandbox")
check("sandbox module loaded", ok_sb and type(sandbox) == "table")

-- Assert whitelisted globals can be assigned in strict mode
local ok_g_backspace = pcall(function() _G._GAME_KEY_BACKSPACE = true end)
check("sandbox allows _GAME_KEY_BACKSPACE", ok_g_backspace == true)

local ok_unauth = pcall(function() _G._UNAUTHORIZED_TEST_GLOBAL_XYZ = 42 end)
check("sandbox blocks non-whitelisted globals", ok_unauth == false)

-- Assert TextCommands.input executes cleanly under active sandbox metatable
local ctx_sandbox = {
    f = {}, sf = {}, tf = {}, mp = {},
    text_state = { line = 1, char_offset = 0, opacity = 255, cursor_x = 32, cursor_y = 580, draws = {} },
    textCursorX = 32, textCursorY = 580,
}
local co_sb = coroutine.create(function()
    TextCommands.input(ctx_sandbox, { name = "f.hero_name", default = "Strict", maxlen = 12 })
end)
local resume_ok, resume_err = coroutine.resume(co_sb)
check("strict sandbox coroutine resumes without error", resume_ok == true and resume_err == nil)
check("strict sandbox installed _KAG_onTextInput", type(_G._KAG_onTextInput) == "function")
check("strict sandbox installed _KAG_onTextEditing", type(_G._KAG_onTextEditing) == "function")
check("strict sandbox installed _KAG_onKeyDown", type(_G._KAG_onKeyDown) == "function")

-- Interact under strict sandbox
_G._KAG_onTextInput("Test")
_G._KAG_onTextEditing("comp", 0, 4)
_G._KAG_onKeyDown(8, "backspace")
-- Backspace belongs to the IME; the host reports preedit completion separately.
_G._KAG_onTextEditing("", 0, 0)
_G._KAG_onKeyDown(13, "return")

check("strict sandbox input completed", ctx_sandbox.f.hero_name == "StrictTest")
check("strict sandbox input mode cleared", ctx_sandbox._inputMode == false)
check("strict sandbox IME stopped", ime_active == false)

-- Restore backend mocks
backend.start_text_input = original_start
backend.stop_text_input = original_stop
backend.set_text_input_rect = original_set_rect

for _, r in ipairs(results) do
    if not r then
        error("[test_input_cmd] Verification failure detected.")
    end
end

print("[test_input_cmd] Done. All checks passed.")
