-- test_i18n.lua — Localization pipeline:
--   i18n.fnv1a content-addressed keys, i18n.localize precedence
--   (per-line translation > {key} token expansion > original),
--   inline-markup whitelist in expand, [ch]/[text] pipeline integration,
--   ks_i18n template generator (fresh + merge).
local results = {}
local function check(name, cond)
    if cond then print("PASS " .. name) else print("FAIL " .. name) end
    results[#results + 1] = cond
end

-- Mock the engine-side APIs (no GPU in unit tests)
_G.backend = _G.backend or {}
_G.backend = {
    create_solid_texture = function() return { _mock = true } end,
    render_text = function() end,
    set_input_focus = function() end,
    audio_play = function() end,
    create_viewport = function() return { _mock = true } end,
    destroy_viewport = function() end,
    clear_text = function() end,
    load_texture = function() return { _mock = true } end,
    submit_batch = function() end,
    text_set_state = function() end,
}

package.path = "scripts/?.lua;scripts/kag/?.lua;" .. package.path
local i18n = require("i18n")
local TextCommands = require("kag.commands.text")
local layers = require("layers")

-- Save i18n state (suite shares globals) and restore at the end.
local saved_current = i18n.current
local saved_strings = i18n.strings
local saved_lines = i18n.lines
local saved_fallback = i18n.fallback

-- ---------------------------------------------------------------------------
-- 1. fnv1a content addressing
-- ---------------------------------------------------------------------------
check("fnv1a known vector (matches ks_i18n template)",
      i18n.fnv1a("Welcome to Caesura AmeKAG Engine Demo.") == "21c6c4b9")
check("fnv1a is 8-hex", i18n.fnv1a("x"):match("^%x%x%x%x%x%x%x%x$") ~= nil)
check("fnv1a differs per message",
      i18n.fnv1a("a") ~= i18n.fnv1a("b"))
check("fnv1a deterministic", i18n.fnv1a("你好世界") == i18n.fnv1a("你好世界"))

-- ---------------------------------------------------------------------------
-- 2. localize precedence
-- ---------------------------------------------------------------------------
i18n.strings = { greeting = "你好", b = "BOLD" }
i18n.lines = {
    ["scene.ks:" .. i18n.fnv1a("hi")] = "bonjour",
    ["scene.ks:" .. i18n.fnv1a("{greeting} world")] = "世界你好",
}

check("localize: line translation wins",
      i18n.localize("hi", "scene.ks") == "bonjour")
check("localize: scene mismatch falls through",
      i18n.localize("hi", "other.ks") == "hi")
check("localize: token expansion",
      i18n.localize("say {greeting}", "x.ks") == "say 你好")
check("localize: translated line may carry its own key tokens",
      i18n.localize("{greeting} world", "scene.ks") == "世界你好")
check("localize: unknown key passes through",
      i18n.localize("no {such_key} here", "x.ks") == "no {such_key} here")
check("localize: empty text", i18n.localize("", "x.ks") == "")

-- Markup whitelist: even a defined string-table key must not eat markup.
check("expand: {b} whitelisted", i18n.expand("{b}bold{/b}") == "{b}bold{/b}")
check("expand: {i}/{s} whitelisted",
      i18n.expand("{i}x{/i}{s}y{/s}") == "{i}x{/i}{s}y{/s}")
check("expand: {color=...} untouched",
      i18n.expand("{color=#ff0000}red") == "{color=#ff0000}red")
check("expand: {size=36} untouched",
      i18n.expand("a{size=36}b") == "a{size=36}b")
check("expand: real key still expands",
      i18n.expand("{greeting}") == "你好")

-- ---------------------------------------------------------------------------
-- 3. [ch]/[text] pipeline integration
-- ---------------------------------------------------------------------------
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

local ctxC = fresh_ctx()
TextCommands.ch(ctxC, { name = "A", text = "hi" })
local found = false
for _, d in ipairs(ctxC.text_state.draws) do
    if d.text == "bonjour" then found = true end
end
check("ch: translated line drawn", found)
check("ch: backlog uses translated plain",
      ctxC.backlog[1] ~= nil and ctxC.backlog[1].text == "bonjour")

local ctxT = fresh_ctx()
TextCommands.text(ctxT, { text = "say {greeting}" })
local foundT = false
for _, d in ipairs(ctxT.text_state.draws) do
    if d.text == "say 你好" then foundT = true end
end
check("text: token expansion drawn", foundT)

local ctxM = fresh_ctx()
TextCommands.ch(ctxM, { text = "{b}bold{/b} and {greeting}" })
local boldDraw, tailDraw, literalB = false, false, false
for _, d in ipairs(ctxM.text_state.draws) do
    if d.text == "bold" and d.bold == true then boldDraw = true end
    if d.text == " and 你好" then tailDraw = true end
    if d.text:find("{b}", 1, true) then literalB = true end
end
check("ch: markup survives localization (bold span)",
      boldDraw and tailDraw and not literalB)

-- ---------------------------------------------------------------------------
-- 3c. [button]/[sel] choice labels
-- ---------------------------------------------------------------------------
local choiceKey = "scene.ks:" .. i18n.fnv1a("Choice A")
i18n.lines[choiceKey] = "選択肢A"
local savedClick = _G._KAG_onClick
local ctxB = fresh_ctx()
TextCommands.button(ctxB, { text = "Choice A", target = "*a" })
local choiceCoB = coroutine.create(function() TextCommands.endbutton(ctxB, {}) end)
assert(coroutine.resume(choiceCoB))
check("button: registered label localized",
      ctxB._choiceButtonsActive ~= nil
      and ctxB._choiceButtonsActive[1].text == "選択肢A")
check("button: draw uses localized label", ctxB.text_state.draws[1] ~= nil
      and ctxB.text_state.draws[1].text == "1. 選択肢A")
assert(coroutine.close(choiceCoB))
check("button: closing choice restores input and removes UI",
      _G._KAG_onClick == savedClick and ctxB._choiceMode == false
      and ctxB._choiceButtonsActive == nil and #ctxB.text_state.draws == 0)

-- [sel] alias shares the handler (KAG3 select syntax)
local ctxS = fresh_ctx()
TextCommands.sel(ctxS, { text = "Choice A", target = "*a" })
check("sel: alias localizes too",
      ctxS._choiceButtons ~= nil and ctxS._choiceButtons[1].text == "選択肢A")
i18n.lines[choiceKey] = nil

-- ---------------------------------------------------------------------------
-- 3d. language hot-switch full-page redraw (relocalize_page)
--  A [ch]/[text] records its pre-localize source; switching the string
--  table and calling relocalize_page re-draws the page in the new
--  language, re-localizes the backlog / active choices / cc_text and
--  seals the typewriter (fully revealed).
-- ---------------------------------------------------------------------------
local ctxR = fresh_ctx()
TextCommands.ch(ctxR, { name = "A", text = "hi" })
check("redraw: page source recorded", #ctxR.text_state.page_src == 1
      and ctxR.text_state.page_src[1].src == "hi")
check("redraw: backlog carries src", ctxR.backlog[1] ~= nil
      and ctxR.backlog[1].src == "hi"
      and ctxR.backlog[1].text == "bonjour")
local drawBefore = nil
for _, d in ipairs(ctxR.text_state.draws) do
    if d.text == "bonjour" then drawBefore = d end
end
check("redraw: draw marked as page source",
      drawBefore ~= nil and drawBefore._page_src == true)

-- Switch language: the same content-addressed key maps to a new text.
i18n.lines["scene.ks:" .. i18n.fnv1a("hi")] = "hallo"
TextCommands.relocalize_page(ctxR)
local foundNew = false
for _, d in ipairs(ctxR.text_state.draws) do
    if d.text == "hallo" then foundNew = true end
end
check("redraw: current line re-localized", foundNew)
local oldGone = true
for _, d in ipairs(ctxR.text_state.draws) do
    if d.text == "bonjour" then oldGone = false end
end
check("redraw: old draw dropped", oldGone)
check("redraw: backlog re-localized from src",
      ctxR.backlog[1] ~= nil and ctxR.backlog[1].text == "hallo")
check("redraw: typewriter sealed", (function()
    for _, d in ipairs(ctxR.text_state.draws) do
        if d.typewriter then return false end
    end
    return true
end)())
check("redraw: page source entry not mutated",
      (function()
          -- msgY follows the logical viewport (was hardcoded 580 for the
          -- 720-px layout; the engine default is now 1920x1080).
          local _, vh = require("viewport").wh()
          return ctxR.text_state.page_src[1].opts.msgY == math.max(0, vh - 140)
                 and ctxR.text_state.page_src[1].src == "hi"
      end)())

-- {key} tokens re-expand with the NEW string table on redraw.
local ctxK = fresh_ctx()
TextCommands.text(ctxK, { text = "say {greeting}" })
i18n.strings.greeting = "Hello"
TextCommands.relocalize_page(ctxK)
local foundK = false
for _, d in ipairs(ctxK.text_state.draws) do
    if d.text == "say Hello" then foundK = true end
end
check("redraw: {key} re-expands with new strings", foundK)
i18n.strings.greeting = "你好"

-- Translated strings may carry markup: the redraw re-parses it.
local mkKey = "scene.ks:" .. i18n.fnv1a("{b}bold{/b} hi")
i18n.lines[mkKey] = "{b}fett{/b} hallo"
local ctxM2 = fresh_ctx()
TextCommands.ch(ctxM2, { text = "{b}bold{/b} hi" })
TextCommands.relocalize_page(ctxM2)
local boldAfter, halloAfter = false, false
for _, d in ipairs(ctxM2.text_state.draws) do
    if d.text == "fett" and d.bold == true then boldAfter = true end
    -- The second span keeps its leading space (" hallo").
    if d.text == " hallo" then halloAfter = true end
end
check("redraw: translated markup re-parsed", boldAfter and halloAfter)
i18n.lines[mkKey] = nil

-- Untranslated lines fall through unchanged.
local ctxU = fresh_ctx()
TextCommands.ch(ctxU, { text = "untouched line" })
TextCommands.relocalize_page(ctxU)
local foundU = false
for _, d in ipairs(ctxU.text_state.draws) do
    if d.text == "untouched line" then foundU = true end
end
check("redraw: untranslated line unchanged", foundU)

-- NVL accumulated page: every line re-localizes; a translation that
-- wraps to more lines shifts the following lines down (y cascade).
local ctxN = fresh_ctx()
ctxN.nvl_mode = true
ctxN.nvl_prefix_fmt = "「%s」："
TextCommands.ch(ctxN, { name = "A", text = "hi" })
TextCommands.ch(ctxN, { name = "B", text = "hi" })
check("redraw: nvl page sources recorded", #ctxN.text_state.page_src == 2)
i18n.lines["scene.ks:" .. i18n.fnv1a("hi")] = string.rep("x", 200)
TextCommands.relocalize_page(ctxN)
local xCount, minY, maxY = 0, math.huge, -math.huge
for _, d in ipairs(ctxN.text_state.draws) do
    if d.text:find("^x+$") then
        xCount = xCount + 1
        if d.y < minY then minY = d.y end
        if d.y > maxY then maxY = d.y end
    end
end
check("redraw: nvl long translation wraps (>=4 lines)", xCount >= 4)
check("redraw: nvl second line shifted down (cascade)",
      maxY - minY >= 96)
check("redraw: nvl cursor follows last line",
      ctxN.textCursorY ~= nil and ctxN.textCursorY > 300)
i18n.lines["scene.ks:" .. i18n.fnv1a("hi")] = "hallo"

-- Active choice block: labels re-localize and the group re-renders.
i18n.lines[choiceKey] = "選択肢A"
local ctxCH = fresh_ctx()
TextCommands.button(ctxCH, { text = "Choice A", target = "*a" })
local choiceCoCH = coroutine.create(function() TextCommands.endbutton(ctxCH, {}) end)
assert(coroutine.resume(choiceCoCH))
i18n.lines[choiceKey] = "新選択"
TextCommands.relocalize_page(ctxCH)
check("redraw: active choice re-localized",
      ctxCH._choiceButtonsActive ~= nil
      and ctxCH._choiceButtonsActive[1].text == "新選択")
local choiceDraw = nil
for _, d in ipairs(ctxCH.text_state.draws) do
    if d.group == "choices" then choiceDraw = d end
end
check("redraw: choice group re-rendered",
      choiceDraw ~= nil and choiceDraw.text == "1. 新選択")
assert(coroutine.close(choiceCoCH))
check("redraw: closing relocalized choice releases input and UI",
      _G._KAG_onClick == savedClick and ctxCH._choiceMode == false
      and ctxCH._choiceButtonsActive == nil and #ctxCH.text_state.draws == 0)
i18n.lines[choiceKey] = nil

-- Closed captions (cc_mode) re-localize from their recorded source.
local ctxCC = fresh_ctx()
ctxCC.cc_mode = true
TextCommands.ch(ctxCC, { name = "A", text = "hi", voice = "v.wav" })
check("redraw: cc source recorded", ctxCC.cc_text ~= nil
      and ctxCC.cc_text.src == "hi")
TextCommands.relocalize_page(ctxCC)
check("redraw: cc re-localized", ctxCC.cc_text ~= nil
      and ctxCC.cc_text.text == "hallo")

-- Defense: bare ctx / empty page no-op.
check("redraw: nil ctx no-op", TextCommands.relocalize_page(nil) == false)
local ctxE = fresh_ctx()
TextCommands.relocalize_page(ctxE)
check("redraw: empty page no-op", #ctxE.text_state.draws == 0
      and #ctxE.text_state.page_src == 0)

-- Page source resets with the page (non-NVL [ch] clears both).
local ctxP = fresh_ctx()
TextCommands.ch(ctxP, { text = "hi" })
TextCommands.ch(ctxP, { text = "hi" })
check("redraw: page source reset per line",
      #ctxP.text_state.page_src == 1)

-- Backlog entries without a source (older saves) stay untouched.
local blX = { { text = "old text", src = nil } }
local ctxX = { backlog = blX, current_scene = "s.ks" }
TextCommands.relocalize_backlog(ctxX)
check("redraw: backlog without src untouched", blX[1].text == "old text")

-- ---------------------------------------------------------------------------
-- 3b. i18n.load reads a tool-generated lang file (comments + return shape)
-- ---------------------------------------------------------------------------
local saved2 = { current = i18n.current, strings = i18n.strings,
                 lines = i18n.lines, fallback = i18n.fallback }
i18n.load("ja")
check("load: generated lang file (comments+return) loads",
      i18n.current == "ja" and type(i18n.lines) == "table")
check("load: dialogue line key present",
      i18n.lines["galgame_demo.ks:21c6c4b9"] ~= nil)
check("load: settings keys preserved alongside lines",
      i18n.strings.fullscreen ~= nil)
check("load: empty placeholder falls back to original",
      i18n.localize("Welcome to Caesura AmeKAG Engine Demo.",
                    "galgame_demo.ks")
      == "Welcome to Caesura AmeKAG Engine Demo.")
i18n.current, i18n.strings = saved2.current, saved2.strings
i18n.lines, i18n.fallback = saved2.lines, saved2.fallback

-- ---------------------------------------------------------------------------
-- 3c. Runtime language API: fallback chain (missing lang -> default -> raw
--     key), translate() placeholder interpolation, set_language dictionary
--     swap (mid-scene) and reload() from-directory hot-reload behavior.
-- ---------------------------------------------------------------------------
local saved3 = { current = i18n.current, strings = i18n.strings,
                 lines = i18n.lines, fallback = i18n.fallback,
                 default_language = i18n.default_language }
i18n.current = "fr"                 -- a language with no lang file
i18n.default_language = "en"
i18n.strings = { greeting = "Bonjour", hello_key = "Salut {who}!" }
i18n.lines = {}
i18n.fallback = { greeting = "Hello", only_default = "DefaultValue" }

-- (a) fallback chain: current -> default language -> raw key
check("rt: current dictionary wins", i18n.t("greeting") == "Bonjour")
check("rt: default-language fallback used",
      i18n.t("only_default") == "DefaultValue")
check("rt: raw key returned when missing everywhere",
      i18n.t("absent_key") == "absent_key")
check("rt: current_language reflects selection",
      i18n.current_language() == "fr")
check("rt: per-line source misses -> original",
      i18n.localize("untouched line", "x.ks") == "untouched line")

-- (b) translate() {placeholder} interpolation
check("rt: translate interpolates {param}",
      i18n.translate("Hello, {name}!", { name = "Caesura" })
      == "Hello, Caesura!")
check("rt: translate many params + keeps markup",
      i18n.translate("{b}{name}{/b}: {n}x", { name = "A", n = 3 })
      == "{b}A{/b}: 3x")
check("rt: translate leaves unknown placeholders intact",
      i18n.translate("hi {who} {nope}", { who = "x" }) == "hi x {nope}")
check("rt: translate with no params returns template unchanged",
      i18n.translate("hi {who}") == "hi {who}")
check("rt: translate bare dict key uses its value as template",
      i18n.translate("hello_key", { who = "monde" }) == "Salut monde!")
check("rt: translate localizes {key} before interpolating params",
      i18n.translate("{greeting} {name}", { name = "Caesura" })
      == "Bonjour Caesura")

-- (c) set_language dictionary swap (mid-scene)
-- Switch from a *different* current code so set_language actually reloads.
i18n.current = "xx"                 -- dummy current, no lang file
i18n.set_language("fr")             -- no fr file -> built-in dictionary
check("rt: set_language seeds current", i18n.current == "fr")
check("rt: set_language selects a built-in dictionary",
      type(i18n.strings) == "table"
      and i18n.t("title_screen") == "标题画面")   -- builtin zh dict
i18n.strings = { greeting = "Bonjour" }   -- simulate the swapped dict
check("rt: t() reflects swapped dictionary immediately",
      i18n.t("greeting") == "Bonjour")
check("rt: localize picks up swapped dictionary tokens",
      i18n.localize("say {greeting}", "x.ks") == "say Bonjour")

-- same-code set_language is a no-op fast path keeping the active dict
i18n.current = "fr"; i18n.strings = { x = 1 }
i18n.set_language("fr")
check("rt: set_language same-code no-op keeps dictionary",
      i18n.strings.x == 1)
check("rt: set_language returns active strings table",
      type(i18n.set_language("fr")) == "table")

-- (d) reload() hot-reloads a language file, preserving current/default
i18n.default_language = "en"
local rld = i18n.reload("ja")       -- re-reads assets/lang/ja.lua
check("rt: reload returns a strings table", type(rld) == "table")
check("rt: reload switches current to reloaded code", i18n.current == "ja")
check("rt: reload preserves default_language",
      i18n.default_language == "en")
check("rt: t() resolves after reload",
      i18n.t("fullscreen") ~= nil and #i18n.t("fullscreen") > 0)

-- restore state so later sections (and the suite cleanup) see no drift
i18n.current, i18n.strings = saved3.current, saved3.strings
i18n.lines, i18n.fallback = saved3.lines, saved3.fallback
i18n.default_language = saved3.default_language



-- ---------------------------------------------------------------------------
-- 4. ks_i18n template generator
-- ---------------------------------------------------------------------------
local ks = require("ks_i18n")
-- Round 71: use a REPO-LOCAL tmp dir (tmp/test_i18n) instead of the OS temp
-- dir. The OS temp path is enumerated via popen(dir/ls) and can be
-- environment-sensitive on CI runners; a repo-local dir is guaranteed
-- writable and enumerable (same reasoning as test_carc_import reset_dir).
-- NOTE: scan_dir (fileutil) REJECTS backslash paths (C6 whitelist allows
-- only %w/_%.:-), so tmpdir must be FORWARD slashes on every platform.
local tmpdir = "tmp/test_i18n"
local function reset_tmp()
    pcall(os.execute, 'rm -rf "tmp/test_i18n"')
    -- Windows cmd has no -p: without the split a literal "-p" dir appears in
    -- the CWD (same split as test_ks_bake.lua / scripts/system.lua).
    if package.config:sub(1, 1) == "\\" then
        pcall(os.execute, 'mkdir "tmp\\test_i18n" 2>nul')
    else
        pcall(os.execute, 'mkdir -p "tmp/test_i18n"')
    end
end
reset_tmp()
local fixture = "demo/x.ks"
local f = io.open(tmpdir .. "/x.ks", "w")
f:write('[ch name="A" text="Hello world"]\n[p]\nplain line\n[text text="Second"]\n'
     .. '[button text="Yes" target="*y"]\n[endbutton]\n[sel text="No"]\n[endselect]\n')
f:close()

local body, total, scenes, kept = ks.build_template(tmpdir, nil)
check("ks_i18n: 5 messages extracted (incl. button/sel labels)", total == 5)
check("ks_i18n: scene counted", scenes == 1)
check("ks_i18n: hash keys present",
      body:find('x.ks:' .. i18n.fnv1a("Hello world"), 1, true) ~= nil)
check("ks_i18n: bare text token extracted",
      body:find('x.ks:' .. i18n.fnv1a("plain line"), 1, true) ~= nil)
check("ks_i18n: [text] extracted",
      body:find('x.ks:' .. i18n.fnv1a("Second"), 1, true) ~= nil)
check("ks_i18n: [button] label extracted",
      body:find('x.ks:' .. i18n.fnv1a("Yes"), 1, true) ~= nil)
check("ks_i18n: [sel] label extracted",
      body:find('x.ks:' .. i18n.fnv1a("No"), 1, true) ~= nil)
check("ks_i18n: original in comment",
      body:find("original: Hello world", 1, true) ~= nil)

-- Merge mode: existing translation preserved, new keys appended.
local existing = {
    lines = { ["x.ks:" .. i18n.fnv1a("Hello world")] = "Hallo Welt" },
    title_screen = "TITLE",
}
local merged, totalM = ks.build_template(tmpdir, existing)
check("ks_i18n merge: keeps translation",
      merged:find('"Hallo Welt"', 1, true) ~= nil)
check("ks_i18n merge: same key count", totalM == 5)
check("ks_i18n merge: preserves other fields",
      merged:find('title_screen = "TITLE"', 1, true) ~= nil)

-- load_lang must parse tool-generated files (comments + return) so a
-- real --update round-trip never drops hand-authored settings keys.
local genPath = tmpdir .. "/gen.lua"
local g = io.open(genPath, "w")
g:write(body)
g:close()
local reloaded = ks.load_lang(genPath)
check("ks_i18n load_lang: generated file parses",
      reloaded ~= nil and type(reloaded.lines) == "table")

-- Hand-written lang files are bare table literals ({...}, no return).
-- The value deliberately contains the word "return": a bare substring
-- detection would misfire here and silently drop the file.
local handPath = tmpdir .. "/hand.lua"
local h = io.open(handPath, "w")
h:write('{\n  title_screen = "手書き return テスト",\n  lines = { ["a.ks:1"] = "x" },\n}\n')
h:close()
local reloadedHand = ks.load_lang(handPath)
check("ks_i18n load_lang: hand-written literal parses",
      reloadedHand ~= nil and reloadedHand.title_screen == "手書き return テスト"
      and reloadedHand.lines["a.ks:1"] == "x")
os.remove(handPath)

-- ---------------------------------------------------------------------------
-- 5. find_missing — untranslated report (CI gate for translators)
-- ---------------------------------------------------------------------------
local report = ks.find_missing(tmpdir, existing)
check("find_missing: 4 of 5 missing (1 translated)",
      report.total == 5 and report.missing == 4)
local translatedMissing = false
for _, e in ipairs(report.entries) do
    if e.key == "x.ks:" .. i18n.fnv1a("Hello world") then
        translatedMissing = true
    end
end
check("find_missing: translated key excluded", not translatedMissing)
check("find_missing: original text carried for context",
      report.entries[1].original ~= nil and #report.entries[1].original > 0)
local noLang = ks.find_missing(tmpdir, nil)
check("find_missing: no lang data -> all missing", noLang.missing == 5)
local fullLang = { lines = {} }
for _, e in ipairs(ks.collect_entries(tmpdir)) do
    fullLang.lines[e.key] = "x"
end
check("find_missing: fully translated -> zero missing",
      ks.find_missing(tmpdir, fullLang).missing == 0)
os.remove(genPath)
os.remove(tmpdir .. "/x.ks")

-- ---------------------------------------------------------------------------
-- 6. CLI-level E2E (round 74): drive the REAL scripts/ks_i18n.lua entry
--    (arg parsing + file write + --update merge + --missing exit code)
--    against a real demo scene snippet in a repo-local tmp dir.
--    Success paths run in-process (dofile re-executes the module; safe
--    because the CLI only os.exit()s on --missing / cannot-write, which
--    we never trigger in-process). The --missing exit gate needs a real
--    subprocess because os.exit() would kill the whole suite.
-- ---------------------------------------------------------------------------
local SAVE_ARG = _G.arg
local cli_dir = "tmp/test_i18n/cli"       -- forward slashes (C6 whitelist)
local cli_lang = "tmp/test_i18n/cli/cli_e2e.lua"
pcall(os.execute, 'rm -rf "tmp/test_i18n/cli"')
if package.config:sub(1, 1) == "\\" then
    pcall(os.execute, 'mkdir "tmp\\test_i18n\\cli" 2>nul')
else
    pcall(os.execute, 'mkdir -p "tmp/test_i18n/cli"')
end

-- Real demo snippet (galgame_demo.ks dialogue + a bare text token).
local fcli = io.open(cli_dir .. "/galgame_demo.ks", "w")
fcli:write('[ch name="Narrator" text="Welcome to Caesura AmeKAG Engine Demo."]\n[p]\n'
         .. '[ch name="Narrator" text="Afternoon sunlight filters through the window."]\n'
         .. '[p]\nplain line\n[p]\n')
fcli:close()

-- Fresh generate: run scripts/ks_i18n.lua as a script with --dir/--out.
_G.arg = { [0] = "scripts/ks_i18n.lua", "--dir", cli_dir,
           "--lang", "cli_e2e", "--out", cli_lang }
local cli_ok = pcall(dofile, "scripts/ks_i18n.lua")
check("cli: script entry runs without error", cli_ok)
local cli_body = ""
local fcl = io.open(cli_lang, "r")
if fcl then cli_body = fcl:read("*a"); fcl:close() end
check("cli: fresh template written", #cli_body > 0)
local cli_keys = 0
for _ in cli_body:gmatch('%[%"[^%]]+%] = "') do cli_keys = cli_keys + 1 end
check("cli: 3 messages extracted from demo snippet", cli_keys == 3)
check("cli: bare text token extracted",
      cli_body:find('galgame_demo.ks:' .. i18n.fnv1a("plain line"),
                    1, true) ~= nil)

-- --update merge: pre-populate a translation + top-level settings key,
-- rerun the CLI with --update, and assert both survive regeneration.
local updKey = "galgame_demo.ks:" .. i18n.fnv1a(
    "Welcome to Caesura AmeKAG Engine Demo.")
local upd = io.open(cli_lang, "w")
upd:write(string.format('%s\n%s\n  greeting = "こんにちは！",\n  lines = {\n'
    .. '    ["%s"] = "ようこそ",\n  },\n}\n',
    "-- Auto-generated header", "return {", updKey))
upd:close()
_G.arg = { [0] = "scripts/ks_i18n.lua", "--dir", cli_dir,
           "--lang", "cli_e2e", "--out", cli_lang, "--update" }
pcall(dofile, "scripts/ks_i18n.lua")
local upd_body = ""
local fup = io.open(cli_lang, "r")
if fup then upd_body = fup:read("*a"); fup:close() end
check("cli --update: existing translation kept",
      upd_body:find('"ようこそ"', 1, true) ~= nil)
check("cli --update: top-level settings key preserved",
      upd_body:find('greeting = "こんにちは！"', 1, true) ~= nil)

-- --missing (subprocess): must report the untranslated remainder and
-- exit 1 (the os.exit() gate would kill the whole suite in-process, so
-- this one runs as a real subprocess). Locate the LUA interpreter like
-- test_carc_import finds its binary (vendored exe first, then bare).
local SEP2 = package.config:sub(1, 1)
local IS_WIN2 = SEP2 == "\\"
local LUA = nil
-- Interpreter candidates for the subprocess, in order: (1) the running
-- interpreter itself (arg[-1] -- the CI-invoked binary, e.g. brew lua on
-- macOS, lua5.4 on Linux); (2) the vendored source-tree lua (Windows
-- local builds keep lua.exe there); (3) the lua_cli built by CMake
-- (round 70); (4) bare "lua" via PATH (macOS/Linux CI). Without a real
-- binary the --missing gate cannot run -- POSIX popen("") yields an
-- EMPTY report that fails the assertions, so never fall through to "".
local cands = {}
-- (1) the interpreter this suite is RUNNING under. SAVE_ARG was captured
-- BEFORE the CLI e2e sections clobbered _G.arg, so it still carries the
-- runner's own argv: on CI that is exactly the Lua the job invoked
-- (brew "lua" on macOS, "lua5.4" on Linux, the built lua_cli path on
-- Windows). A bare name resolves via PATH in the popen shell.
if type(SAVE_ARG) == "table" and type(SAVE_ARG[-1]) == "string"
    and #SAVE_ARG[-1] > 0 then
    cands[#cands + 1] = SAVE_ARG[-1]
end
-- (2) vendored source-tree lua (Windows local builds keep lua.exe there)
for _, c in ipairs({ "external" .. SEP2 .. "lua" .. SEP2 .. "lua.exe",
                     "external" .. SEP2 .. "lua" .. SEP2 .. "lua" }) do
    cands[#cands + 1] = c
end
-- (3) the lua_cli interpreter built by CMake (round 70): multi-config
-- generators put it under Debug/ (Windows), Makefile generators directly
-- under build/lua/ (macOS/Linux CI).
for _, c in ipairs({ "build" .. SEP2 .. "lua" .. SEP2 .. "Debug"
                         .. SEP2 .. "lua.exe",
                     "build" .. SEP2 .. "lua" .. SEP2 .. "Debug"
                         .. SEP2 .. "lua",
                     "build" .. SEP2 .. "lua" .. SEP2 .. "lua",
                     "build" .. SEP2 .. "lua" .. SEP2 .. "lua.exe" }) do
    cands[#cands + 1] = c
end
-- (4) bare names via PATH (order matters: Linux CI installs lua5.4 only)
cands[#cands + 1] = "lua5.4"
cands[#cands + 1] = IS_WIN2 and "lua.exe" or "lua"
-- Resolve: path-like candidates must exist on disk; a bare name is
-- accepted only when no earlier candidate resolved (popen shells out,
-- so PATH lookup happens at spawn time).
for _, c in ipairs(cands) do
    -- path-like = contains a separator ONLY (a bare name like "lua5.4"
    -- carries a dot but resolves via PATH -- never treat it as a file).
    local pathLike = c:find(SEP2, 1, true) or c:find("/", 1, true)
    if not pathLike or io.open(c, "r") then LUA = c break end
end
-- The interpreter may be an absolute path containing spaces or
-- parentheses (local checkouts under "Caesura(AmeKAG)"); Windows cmd
-- splits on those, so wrap the binary in call "..." exactly like the
-- orphan runner does. POSIX shells quote paths natively -- wrapper is
-- Windows-only.
local cmdline = LUA
if IS_WIN2 and LUA and (LUA:find("[ %(%)]") or LUA:find(SEP2, 1, true)) then
    cmdline = 'call "' .. LUA .. '"'
end
local fff = io.popen(cmdline and (cmdline .. ' scripts/ks_i18n.lua --dir "'
    .. cli_dir .. '" --out "' .. cli_lang .. '" --missing') or "")
if fff then
    local report = fff:read("*a")
    fff:close()
    -- After --update merged "ようこそ" for the Welcome key, exactly the
    -- other two lines (Afternoon sunlight + plain line) stay untranslated.
    check("cli --missing: reports remaining untranslated",
          report:find("2/3 keys untranslated", 1, true) ~= nil
          and report:find("Afternoon sunlight", 1, true) ~= nil
          and report:find("plain line", 1, true) ~= nil)
else
    print("SKIP cli --missing: no LUA interpreter located")
end
-- The gate leaves the empty placeholders in place until a translator
-- fills them (CI-facing contract).
local fmiss = io.open(cli_lang, "r")
local miss_body = fmiss and fmiss:read("*a") or ""
if fmiss then fmiss:close() end
check("cli --missing: empty (untranslated) placeholders remain",
      miss_body:find('= ""', 1, true) ~= nil)

_G.arg = SAVE_ARG
pcall(os.execute, 'rm -rf "tmp/test_i18n/cli"')

-- ---------------------------------------------------------------------------
-- 7. Plural resolution + {n} numeric format (G9)
--  A string-table VALUE may be a plural variant table:
--      items = { one = "{n} item", other = "{n} items" }
--  translate() resolves the variant by i18n.plural_category(params.n)
--  (en: 1 -> "one", else "other"; zh/ja: always "other"/single form),
--  then interpolates {n} to the literal number.
-- ---------------------------------------------------------------------------
local saved_pl = { current = i18n.current, strings = i18n.strings,
                   lines = i18n.lines, fallback = i18n.fallback }

-- (a) en singular/plural by count
i18n.current = "en"
i18n.strings = { items = { one = "{n} item", other = "{n} items" } }
i18n.lines = {}
i18n.fallback = {}
check("plural: en count 1 -> one", 
      i18n.translate("items", { n = 1 }) == "1 item")
check("plural: en count 2 -> other",
      i18n.translate("items", { n = 2 }) == "2 items")
check("plural: en count 0 -> other",
      i18n.translate("items", { n = 0 }) == "0 items")
check("plural: en count as string resolves",
      i18n.translate("items", { n = "42" }) == "42 items")

-- (b) {n} numeric format + plural combined (placeholder inside the variant)
i18n.strings.items = { one = "You have {n} apple", other = "You have {n} apples" }
check("plural: placeholder inside one form",
      i18n.translate("items", { n = 1 }) == "You have 1 apple")
check("plural: placeholder inside other form",
      i18n.translate("items", { n = 5 }) == "You have 5 apples")

-- (c) missing-plural fallback: a plural table lacking the matched category
-- falls back to entry.other (generic); a table with only "one" decodes there.
i18n.strings.items = { one = "solo {n}" }          -- no "other"
check("plural: missing other falls back to one",
      i18n.translate("items", { n = 9 }) == "solo 9")
i18n.strings.items = { other = "only {n}" }        -- no "one"
check("plural: missing one falls back to other",
      i18n.translate("items", { n = 1 }) == "only 1")

-- (d) plural table without a count param -> generic form (no table leak),
-- and plain string entries are untouched by the plural path.
i18n.strings.items = { one = "{n} item", other = "{n} items" }
check("plural: no n param -> generic other resolved, no table leak",
      type(i18n.t("items")) == "string" and not i18n.t("items"):find("table:", 1, true))
check("plural: translate without n uses other form",
      i18n.translate("items", {}) == "{n} items")
i18n.strings.plain = "{n} widgets"
check("plural: plain string entry passes through",
      i18n.translate("plain", { n = 3 }) == "3 widgets")

-- (e) zh / ja: always the single ("other") form regardless of count.
-- zh has NO singular/plural distinction, so an en-authored {one=..,other=..}
-- entry still resolves to the single unmarked form for EVERY count (zh
-- plural_category is always "other", so the "other"/bare form wins).
i18n.current = "zh"
i18n.strings.items = { other = "共 {n} 个", one = "一个" }
check("plural: zh single form for 1",
      i18n.translate("items", { n = 1 }) == "共 1 个")
check("plural: zh single form for 5",
      i18n.translate("items", { n = 5 }) == "共 5 个")
check("plural: zh ignores en one/other split even on odd counts",
      i18n.translate("items", { n = 11 }) == "共 11 个")
i18n.current = "ja"
i18n.strings.items = { other = "{n}個" }
check("plural: ja single form count 1",
      i18n.translate("items", { n = 1 }) == "1個")
check("plural: ja single form count 10",
      i18n.translate("items", { n = 10 }) == "10個")
check("plural: ja category stays other",
      i18n.plural_category(1) == "other")

-- (f) plural_category per language
i18n.current = "en"
check("plural: en category one at 1",
      i18n.plural_category(1) == "one")
check("plural: en category other at 0/2/100",
      i18n.plural_category(0) == "other"
      and i18n.plural_category(2) == "other"
      and i18n.plural_category(100) == "other")
i18n.current = "zh"
check("plural: zh category always other",
      i18n.plural_category(1) == "other"
      and i18n.plural_category(2) == "other")

-- (g) plural variant survives ks_i18n --update roundtrip (serialize_field)
local plExisting = {
    lines = { ["x.ks:" .. i18n.fnv1a("Hello world")] = "Hallo Welt" },
    items = { one = "{n} item", other = "{n} items" },
    greeting = "hi",
}
local plBody = ks.build_template(tmpdir, plExisting)
check("ks_i18n plural: variants serialized on --update",
      plBody:find('items = { one = "{n} item", other = "{n} items" }', 1, true) ~= nil)
check("ks_i18n plural: string field still preserved",
      plBody:find('greeting = "hi"', 1, true) ~= nil)

i18n.current, i18n.strings = saved_pl.current, saved_pl.strings
i18n.lines, i18n.fallback = saved_pl.lines, saved_pl.fallback

-- ---------------------------------------------------------------------------
-- 7b. Plural usage THROUGH the KAG text pipeline (round 110 regression
--  guard). The [ch]/[text]/[button] pipeline localizes via i18n.localize
--  -> i18n.expand -> i18n.t, which NEVER receives a {n} count. A plural
--  VARIANT TABLE value ({ one=.., other=.. }) therefore resolves to its
--  generic ("other") form and LEAVES a literal {n} placeholder in the
--  rendered line. This is locked as the current (correct) semantics: plural
--  keys MUST be routed through i18n.translate(key, { n = <count> }) inside
--  [iscript]/[emb] (resolve the count first, then inject the translated
--  string). A bare {key} plural lookup is a content-authoring error and is
--  now also flagged by a one-shot [i18n] WARN from expand().
-- ---------------------------------------------------------------------------
local saved_pl2 = { current = i18n.current, strings = i18n.strings,
                    lines = i18n.lines, fallback = i18n.fallback }
i18n.current = "en"
i18n.strings = { items = { one = "{n} item", other = "{n} items" } }
i18n.lines = {}
i18n.fallback = {}

-- (a) bare {items} under expand/localize resolves "other" and KEEPS {n}.
check("plural-pipe: expand leaves {n} residual (no count in scope)",
      i18n.expand("count {items}") == "count {n} items")
check("plural-pipe: localize leaves {n} residual too",
      i18n.localize("count {items}", "x.ks") == "count {n} items")

-- (b) the engine now warns when a plural variant table is reached via bare
--     {key} (guides authors to translate()): the warning must mention the
--     key and point at i18n.translate. Capture printed output.
local warns = {}
local orig_print = print
_G.print = function(...)
    local parts = {}
    for i = 1, select("#", ...) do parts[i] = tostring(select(i, ...)) end
    local line = table.concat(parts, " ")
    if line:find("[i18n] WARN", 1, true) then warns[#warns + 1] = line end
    orig_print(...)
end
i18n.expand("count {items}")
_G.print = orig_print
check("plural-pipe: expand warns on plural-table {key}",
      #warns >= 1 and warns[1]:find("items", 1, true) ~= nil
      and warns[1]:find("translate", 1, true) ~= nil)

-- (c) [ch text="count {items}"] renders the residual {n} (locked), never a
--     raw table handle. Markup parsing keeps {n} as literal text (it is
--     not a markup tag), so the draw + backlog both carry "count {n} items".
local ctxPl = fresh_ctx()
TextCommands.ch(ctxPl, { name = "A", text = "count {items}" })
local plDrawn = false
for _, d in ipairs(ctxPl.text_state.draws) do
    if d.text == "count {n} items" then plDrawn = true end
end
check("plural-pipe: [ch] renders residual {n} (no plural count in pipeline)",
      plDrawn)
check("plural-pipe: [ch] never leaks a raw table handle",
      not (function()
          for _, d in ipairs(ctxPl.text_state.draws) do
              if d.text:find("table:", 1, true) then return true end
          end
          return false
      end)())
-- backlog records the localized plain (with residual {n}), not a table.
check("plural-pipe: backlog plain has residual {n}",
      ctxPl.backlog[1] ~= nil and ctxPl.backlog[1].text == "count {n} items")

-- (d) [text text=...] same residual behavior.
local ctxPlT = fresh_ctx()
TextCommands.text(ctxPlT, { text = "you have {items}" })
local plT = false
for _, d in ipairs(ctxPlT.text_state.draws) do
    if d.text == "you have {n} items" then plT = true end
end
check("plural-pipe: [text] renders residual {n}",
      plT and ctxPlT.backlog[1] ~= nil
      and ctxPlT.backlog[1].text == "you have {n} items")

-- (e) [button text="..{items}.."] label: localized at registration, which
--     expands the pair to the generic form with residual {n}.
local ctxPlB = fresh_ctx()
TextCommands.button(ctxPlB, { text = "take {items}", target = "*t" })
check("plural-pipe: [button] label resolves to residual {n}",
      ctxPlB._choiceButtons ~= nil
      and ctxPlB._choiceButtons[1].text == "take {n} items")

-- (f) the CORRECT plural authoring path still interpolates: resolve the
--     count in [iscript] and translate() with { n = <count> }.
check("plural-pipe: correct usage via translate({n=count})",
      i18n.translate("items", { n = 3 }) == "3 items"
      and i18n.translate("items", { n = 1 }) == "1 item")

i18n.current, i18n.strings = saved_pl2.current, saved_pl2.strings
i18n.lines, i18n.fallback = saved_pl2.lines, saved_pl2.fallback

-- ---------------------------------------------------------------------------
-- 8. [i18n language=] command (round 76) + deeper plural / interp /
--    serialization-roundtrip boundaries (round 80 +)
-- ---------------------------------------------------------------------------
local s8 = { current = i18n.current, strings = i18n.strings,
             lines = i18n.lines, fallback = i18n.fallback,
             default_language = i18n.default_language }
local SystemCommands = require("kag.commands.system")
local Schema = require("kag.schema")

-- (a) [i18n language=X] hot-switches the runtime dictionary and records the
-- selection into ctx.settingsValues (the settings-menu contract).
i18n.current = "xx"; i18n.strings = {}; i18n.fallback = {}; i18n.lines = {}
local ctxI = fresh_ctx()
local okI = pcall(SystemCommands.i18n, ctxI, { language = "en" })
check("i18n cmd: runs without error", okI)
check("i18n cmd: records settingsValues.language",
      ctxI.settingsValues ~= nil and ctxI.settingsValues.language == "en")
check("i18n cmd: switches i18n.current", i18n.current == "en")
check("i18n cmd: activated dictionary is the file-loaded en table",
      type(i18n.strings) == "table" and i18n.t("title_screen") == "Title Screen")

-- (b) missing / empty language= must degrade headlessly (never raise).
local ctxNo = fresh_ctx()
local okNo, retNo = pcall(SystemCommands.i18n, ctxNo, {})
check("i18n cmd: no language= degrades (returns true)", okNo and retNo == true)
local ctxE = fresh_ctx()
local okE, retE = pcall(SystemCommands.i18n, ctxE, { language = "" })
check("i18n cmd: empty language= degrades", okE and retE == true)

-- (c) the schema contract rejects a missing required language before the
-- handler runs (the scheduler coerce gate), and accepts a valid one.
local okC = pcall(Schema.coerce, "i18n", {}, fresh_ctx())
check("i18n schema: missing required language raises (coerce rejects)", not okC)
local okC2, coerced = pcall(Schema.coerce, "i18n", { language = "zh" }, fresh_ctx())
check("i18n schema: valid language coerces through",
      okC2 and type(coerced) == "table" and coerced.language == "zh")

-- (d) repeated [i18n] with the same language is idempotent: set_language's
-- same-code fast path keeps the active dictionary (no file re-read / drift).
i18n.current = "xx"; i18n.strings = { x = 1 }
local ctxD = fresh_ctx()
SystemCommands.i18n(ctxD, { language = "zh" })
local dictAfterFirst = i18n.strings
SystemCommands.i18n(ctxD, { language = "zh" })
check("i18n cmd: repeated same language idempotent (same dict table)",
      i18n.current == "zh" and rawequal(i18n.strings, dictAfterFirst))

-- (e) the command wires set_language + relocalize_page: a visible line is
-- re-localized against the NEW (file-loaded) dictionary after [i18n].
local cmdVec = "scene.ks:" .. i18n.fnv1a("hi")
i18n.current = "xx"; i18n.strings = {}; i18n.fallback = {}; i18n.lines = { [cmdVec] = "bonjour" }
local ctxR2 = fresh_ctx()
TextCommands.ch(ctxR2, { name = "A", text = "hi" })
local drewBonjour = false
for _, d in ipairs(ctxR2.text_state.draws) do
    if d.text == "bonjour" then drewBonjour = true end
end
check("i18n cmd: page drawn in pre-switch dictionary", drewBonjour)
local okR2 = pcall(SystemCommands.i18n, ctxR2, { language = "zh" })
check("i18n cmd: switch redraws without error", okR2 and i18n.current == "zh")
-- zh file carries no "hi" line, so the redraw falls through to raw "hi".
local drewHi = false
for _, d in ipairs(ctxR2.text_state.draws) do
    if d.text == "hi" then drewHi = true end
end
check("i18n cmd: line re-localized against new dictionary", drewHi)

-- (f) plural_category / {n} numeric-form edge counts (round 80).
i18n.current = "en"
i18n.strings.items = { one = "{n} item", other = "{n} items" }
check("plural: en negative count -> other (-1)",
      i18n.plural_category(-1) == "other"
      and i18n.translate("items", { n = -1 }) == "-1 items")
check("plural: en non-integer count -> other (1.5)",
      i18n.plural_category(1.5) == "other"
      and i18n.translate("items", { n = "1.5" }) == "1.5 items")
check("plural: en category nil / unparseable -> other",
      i18n.plural_category(nil) == "other"
      and i18n.plural_category("abc") == "other")
check("plural: zh zero count still single form",
      (function()
          i18n.current = "zh"
          i18n.strings.items = { other = "{n} 个" }
          return i18n.plural_category(0) == "other"
                 and i18n.translate("items", { n = 0 }) == "0 个"
      end)())

-- (g) interpolation boundaries: {n} mixed with ordinary / {key} placeholders.
i18n.current = "en"
i18n.strings.items = { other = "{n} {kind} items" }
check("interp: plural {n} + ordinary placeholder",
      i18n.translate("items", { n = 3, kind = "red" }) == "3 red items")
i18n.strings.items = { one = "{n} {thing}", other = "{n} {thing}s" }
i18n.strings.thing = "apple"
check("interp: plural variant resolves {key} token inside form",
      i18n.translate("items", { n = 1 }) == "1 apple"
      and i18n.translate("items", { n = 5 }) == "5 apples")

-- (h) serialize_field roundtrip survives quotes / backslash / newline
-- translations (--update regenerates + reloads losslessly).
if io.open(tmpdir .. "/x.ks", "r") then os.remove(tmpdir .. "/x.ks") end
local tricky = {
    greeting = 'say "hello"',
    winpath = "C:\\dir\\file",
    multi = "line one\nline two",
    items = { one = "{n} item", other = "{n} items" },
    lines = { ["x.ks:" .. i18n.fnv1a("Hello world")] = "Hallo" },
}
local tBody = ks.build_template(tmpdir, tricky)
local tPath = tmpdir .. "/tricky.lua"
local fw = io.open(tPath, "w"); fw:write(tBody); fw:close()
local back = ks.load_lang(tPath)
check("serialize r/t: double-quote translation roundtrips",
      back ~= nil and back.greeting == 'say "hello"')
check("serialize r/t: backslash path roundtrips", back.winpath == "C:\\dir\\file")
check("serialize r/t: newline translation roundtrips", back.multi == "line one\nline two")
check("serialize r/t: plural variants roundtrip intact",
      type(back.items) == "table" and back.items.one == "{n} item"
      and back.items.other == "{n} items")
os.remove(tPath)

-- (i) fallback-chain combo: translate() with no params is exactly localize()
-- (current dict wins, default lang second, raw key last).
i18n.current = "fr"; i18n.strings = { greeting = "Bonjour" }
i18n.fallback = { greeting = "Hello", hello_key = "Bonjour le monde" }
i18n.lines = {}
check("combo: translate() no-params == localize()",
      i18n.translate("say {greeting}") == i18n.localize("say {greeting}"))
check("combo: current missing a key -> default lang value",
      i18n.translate("hello_key", {}) == "Bonjour le monde")
check("combo: raw key returned when missing everywhere",
      i18n.translate("absent_key", {}) == "absent_key")
-- default-language key with a plural table resolves (en category picks one)
i18n.current = "en"
i18n.default_language = "en"
i18n.strings = {}
i18n.fallback = { count = { one = "one {n}", other = "{n} total" } }
check("combo: default-lang plural table resolves via translate",
      i18n.translate("count", { n = 1 }) == "one 1")

i18n.current, i18n.strings = s8.current, s8.strings
i18n.lines, i18n.fallback = s8.lines, s8.fallback
i18n.default_language = s8.default_language

-- Native missing-file classification must not accept Web/unknown error codes.
do
    local code = "caesura_missing_locale_regression_49ae"
    local file, _, errno = io.open("assets/lang/" .. code .. ".lua", "r")
    if file then file:close() end
    check("native locale probe is an actual missing file with ENOENT 2", file == nil and errno == 2)
    local current, strings, fallback = i18n.current, i18n.strings, i18n.fallback
    local prepared = i18n.prepare(code, code)
    check("native missing locale prepares builtin without committing", type(prepared.strings) == "table"
        and i18n.current == current and i18n.strings == strings and i18n.fallback == fallback)
    local original_open = io.open
    for _, denied_errno in ipairs({13, 44}) do
        -- Explicit I/O-boundary negative control; this is not an OS errno observation.
        io.open = function() return nil, "controlled dictionary read failure", denied_errno end
        local ok = pcall(i18n.prepare, code, code)
        io.open = original_open
        check("native non-ENOENT must reject error " .. denied_errno, not ok)
        check("rejected native locale preparation preserves active state " .. denied_errno,
            i18n.current == current and i18n.strings == strings and i18n.fallback == fallback)
    end
    io.open = function() return nil, "unclassified dictionary read failure" end
    local ok = pcall(i18n.prepare, code, code)
    io.open = original_open
    check("unknown dictionary read failure is not missing", not ok)
end

-- ---------------------------------------------------------------------------
-- cleanup: restore i18n state and the backend global
-- ---------------------------------------------------------------------------
i18n.current = saved_current
i18n.strings = saved_strings
i18n.lines = saved_lines
i18n.fallback = saved_fallback

local failed = 0
for _, ok in ipairs(results) do
    if not ok then failed = failed + 1 end
end
if failed > 0 then
    print(string.format("I18N TESTS: %d passed, %d FAILED",
        #results - failed, failed))
    os.exit(1)
end
print(string.format("I18N TESTS DONE (%d passed)", #results))
