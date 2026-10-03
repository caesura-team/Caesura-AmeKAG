local saved_KAG = rawget(_G, "KAG")
local fixture_KAG = {}
if type(saved_KAG)=="table" then for k,v in pairs(saved_KAG) do fixture_KAG[k]=v end end
fixture_KAG.list_assets=function(dir,max_entries,max_bytes)
    assert(dir=="assets/lang" and max_entries==4096 and max_bytes==1024*1024)
    return {"en.lua","ja.lua","zh.lua"},nil
end
rawset(_G,"KAG",fixture_KAG)
local fixture_ok, fixture_error=pcall(function()
-- test_accessibility.lua — closed captions (CC) + TTS interface probe.
package.path = "scripts/?.lua;scripts/kag/?.lua;" .. package.path
local results = {}
local function check(name, cond, detail)
    if cond then print("PASS " .. name) else print("FAIL " .. name .. " -- " .. tostring(detail)) end
    results[#results + 1] = cond
end

-- No config stubbing: CC mode is driven through ctx.cc_mode (the
-- context-flag path, which Settings._applyAll uses). config is only a
-- fallback and never loaded here -- loading it would initialize the
-- backend factory and break every later test in the suite chain.

local tokenizer = require("tokenizer")
local scheduler = require("scheduler")

-- ---- voiced [ch] sets cc_text only in cc_mode ----------------------------
do
    local backend = require("backend")
    local realPlay = backend.audio_play
    local played = {}
    backend.audio_play = function(bus, file, opts)
        played[#played + 1] = { bus, file }
        return true
    end
    local kag_orig = package.loaded["kag"]
    package.loaded["kag"] = { ch = function(c2, p2)
        require("kag.commands.text").ch(c2, p2) end }

    -- cc_mode OFF (context flag): no caption state
    local ctx = { f = {}, tf = {}, sf = {}, mp = {}, lf = {},
                  current_scene = "cc.ks", token_index = 1, cc_mode = false }
    local co = coroutine.create(function()
        scheduler.run(ctx, tokenizer.parse([[
[ch name="Aoi" text="hello" voice="assets/voice/a.ogg"]
]]), 1)
    end)
    while coroutine.status(co) ~= "dead" do coroutine.resume(co) end
    check("voice plays without cc_mode", played[1] ~= nil, tostring(played[1]))
    check("no caption state without cc_mode",
        ctx.cc_text == nil, tostring(ctx.cc_text))

    -- cc_mode ON (context flag): voiced line becomes the caption
    -- (separate run: a voiceless ch in the same stream would clear it
    -- before the check)
    local ctx2 = { f = {}, tf = {}, sf = {}, mp = {}, lf = {},
                   current_scene = "cc.ks", token_index = 1, cc_mode = true }
    local co2 = coroutine.create(function()
        scheduler.run(ctx2, tokenizer.parse([[
[ch name="Aoi" text="hello" voice="assets/voice/a.ogg"]
]]), 1)
    end)
    while coroutine.status(co2) ~= "dead" do coroutine.resume(co2) end
    check("caption set for voiced line",
        ctx2.cc_text and ctx2.cc_text.text == "hello"
            and ctx2.cc_text.speaker == "Aoi",
        ctx2.cc_text and tostring(ctx2.cc_text.text))
    -- voiceless line clears the standing caption
    local co3 = coroutine.create(function()
        scheduler.run(ctx2, tokenizer.parse([[
[ch text="no voice here"]
]]), 1)
    end)
    while coroutine.status(co3) ~= "dead" do coroutine.resume(co3) end
    check("voiceless line clears caption",
        ctx2.cc_text == nil, tostring(ctx2.cc_text))
    backend.audio_play = realPlay
    package.loaded["kag"] = kag_orig
end

-- ---- TTS interface reports unavailable gracefully -------------------------
do
    local backend = require("backend")
    check("tts_available false (no backend)", backend.tts_available() == false)
    check("tts_speak false when unavailable", backend.tts_speak("hi") == false)
end

-- ---- settings menu exposes cc_mode ---------------------------------------
do
    local settings = require("settings")
    local ctx = { settingsValues = {} }
    -- _buildMenu requires ctx.settingsValues + i18n; build and find the item
    local items = settings._buildMenu(ctx)
    local found = false
    for _, item in ipairs(items) do
        if item.key == "cc_mode" then found = true end
    end
    check("settings menu has cc_mode item", found)
    -- applyAll mirrors the toggle into ctx.cc_mode (minimal
    -- settingsValues: volume/fullscreen keys would hit the backend
    -- factory which is unavailable in tests)
    ctx.settingsValues = { cc_mode = true }
    settings._applyAll(ctx)
    check("_applyAll mirrors cc_mode into ctx",
        ctx.cc_mode == true, tostring(ctx.cc_mode))
end

local failed = 0
for _, ok in ipairs(results) do if not ok then failed = failed + 1 end end
if failed > 0 then os.exit(1) end
print("ACCESSIBILITY TESTS DONE")

end)
rawset(_G,"KAG",saved_KAG)
assert(fixture_ok,fixture_error)
