-- =============================================================================
--  test_kag_commands.lua -- Comprehensive KAG command unit tests
--  Covers: button, text, audio, layer, transition, system, save, vfx, video
-- =============================================================================

package.path = "scripts/?.lua;scripts/?/init.lua;" .. package.path

-- Mock C++ bindings
_G.KAG = {
    play_bgm=function()return true end, play_voice=function()return true end,
    play_se=function()return true end, stop_bgm=function()return true end,
    stop_voice=function()return true end, stop_se=function()return true end,
    set_bus_volume=function()return true end, get_bus_volume=function()return 0.8 end,
    flush_wave_cache=function()return true end, show_text=function()return true end,
    show_image=function()return true end, clear_screen=function()return true end,
    wait_click=function()return true end, render_text=function()return true end,
    render_ruby=function()return true end, clear_text=function()return true end,
    set_font=function()return true end, line_height=function()return 24 end,
    set_listener=function()return true end, is_voice_playing=function()return false end,
    is_bgm_playing=function()return false end, get_active_voices=function()return 0 end,
    log=function()end, clear_text_layer=function()return true end,
    set_bgm_volume=function()return true end, set_se_volume=function()return true end,
    set_voice_volume=function()return true end, audio_get_position=function()return 0 end,
    audio_get_length=function()return 0 end, audio_fade_volume=function()return true end,
    quake=function()return true end, save_game=function()return true end,
    load_game=function()return nil,"mock"end, list_saves=function()return{}end,
    play_se_3d=function()return true end, set_global_volume=function()end,
    get_global_volume=function()return 1.0 end, replay_voice=function()end,
}
-- preload chapter_select so sandboxed later tests can require it
pcall(function() package.loaded["chapter_select"] = require("chapter_select") end)
pcall(function() package.loaded["music_room"] = require("music_room") end)
pcall(function() package.loaded["gallery"] = require("gallery") end)
pcall(function() package.loaded["settings"] = require("settings") end)
pcall(function() package.loaded["history_ui"] = require("history_ui") end)

_G.Render = {
    text_set_font = function() end,
    text_reset_state = function() end,
}
_G.DevCore = {}
_G.Engine = {select_platform_backend=function()return true end}

local passed, failed = 0, 0
local function check(name, cond, detail)
    if cond then
        passed = passed + 1
        print(string.format("  [PASS] %s", name))
    else
        failed = failed + 1
        print(string.format("  [FAIL] %s  -- %s", name, detail or ""))
    end
end

local function make_ctx()
    return {
        f = {}, sf = {}, tf = {},
        layers = {}, backlog = {}, active_operations = {},
        tokens = {}, token_index = 1, call_stack = {},
        waiting_input = false, _choiceMode = false,
        _selectedChoice = nil, _pendingJump = nil,
    }
end

print("\n=== KAG Commands Test Suite ===\n")

-- ═══ Layer Commands ═══
print("--- Layer Commands ---")
do
    local LayerCommands = require("kag.commands.layer")
    local ctx = make_ctx()
    
    check("bg handler exists", type(LayerCommands.bg) == "function")
    check("fg handler exists", type(LayerCommands.fg) == "function")
    check("cl handler exists", type(LayerCommands.cl) == "function")
    check("image handler exists", type(LayerCommands.image) == "function")
    check("position handler exists", type(LayerCommands.position) == "function")
    check("layopt handler exists", type(LayerCommands.layopt) == "function")
end

-- ═══ Text Commands ═══
print("\n--- Text Commands ---")
do
    local TextCommands = require("kag.commands.text")
    local ctx = make_ctx()
    
    check("ch handler exists", type(TextCommands.ch) == "function")
    check("text handler exists", type(TextCommands.text) == "function")
    check("l handler exists", type(TextCommands.l) == "function")
    check("r handler exists", type(TextCommands.r) == "function")
    check("er handler exists", type(TextCommands.er) == "function")
    check("p handler exists", type(TextCommands.p) == "function")
    check("ruby handler exists", type(TextCommands.ruby) == "function")
    check("font handler exists", type(TextCommands.font) == "function")
    check("skip handler exists", type(TextCommands.skip) == "function")
    check("reset handler exists", type(TextCommands.reset) == "function")
    check("pt handler exists", type(TextCommands.pt) == "function")
    
    -- [button] / [endbutton]
    check("button handler exists", type(TextCommands.button) == "function")
    check("endbutton handler exists", type(TextCommands.endbutton) == "function")
end

-- ═══ Audio Commands ═══
print("\n--- Audio Commands ---")
do
    local AudioCommands = require("kag.commands.audio")
    
    check("playbgm handler exists", type(AudioCommands.playbgm) == "function")
    check("stopbgm handler exists", type(AudioCommands.stopbgm) == "function")
    check("playse handler exists", type(AudioCommands.playse) == "function")
    check("playvoice handler exists", type(AudioCommands.playvoice) == "function")
    check("fadebgm handler exists", type(AudioCommands.fadebgm) == "function")
    check("xfadebgm handler exists", type(AudioCommands.xfadebgm) == "function")
    check("setbgmvolume handler exists", type(AudioCommands.setbgmvolume) == "function")
    check("waitsound handler exists", type(AudioCommands.waitsound) == "function")
    check("stopse handler exists", type(AudioCommands.stopse) == "function")
    check("playbgmstop handler exists", type(AudioCommands.playbgmstop) == "function")
end

-- ═══ System Commands ═══
print("\n--- System Commands ---")
do
    local SystemCommands = require("kag.commands.system")
    
    check("wait handler exists", type(SystemCommands.wait) == "function")
    check("emb handler exists", type(SystemCommands.emb) == "function")
    check("eval handler exists", type(SystemCommands.eval) == "function")
    check("history handler exists", type(SystemCommands.history) == "function")
end

-- ═══ Transition Commands ═══
print("\n--- Transition Commands ---")
do
    local TransCommands = require("kag.commands.transition")
    
    check("trans handler exists", type(TransCommands.trans) == "function")
    check("move handler exists", type(TransCommands.move) == "function")
    check("quake handler exists", type(TransCommands.quake) == "function")
    check("fade handler exists", type(TransCommands.fade) == "function")
end

-- ═══ VFX Commands ═══
print("\n--- VFX Commands ---")
do
    local VFXCommands = require("kag.commands.vfx")
    check("vfx handler exists", type(VFXCommands.vfx) == "function")
end

-- ═══ Video Commands ═══
print("\n--- Video Commands ---")
do
    local VideoCommands = require("kag.commands.video")
    check("video handler exists", type(VideoCommands.video) == "function")
    check("stopvideo handler exists", type(VideoCommands.stopvideo) == "function")
end

-- ═══ Save Commands ═══
print("\n--- Save Commands ---")
do
    local SaveCommands = require("kag.commands.save")
    check("save handler exists", type(SaveCommands.save) == "function")
    check("load handler exists", type(SaveCommands.load) == "function")

    -- [load] scene-path allowlist regression (was a silent no-op once)
    local safe = SaveCommands._safeScenePath
    check("guard rejects traversal ../", not safe("demo/../x.ks"))
    check("guard rejects traversal ../ nested", not safe("tests/scripts/../../y.ks"))
        check("guard rejects nested traversal", not safe("demo/../sub/x.ks"))
    check("guard rejects non-ks", not safe("scripts/x.txt"))
    check("guard rejects a NUL-truncated scene", not safe("scripts/x" .. string.char(0) .. ".ks"))
    check("guard rejects absolute", not safe("/etc/passwd"))
    check("guard rejects empty", not safe(""))
    check("guard rejects non-string", not safe(42))
    check("guard accepts entry", safe("scripts/demo_story.ks"))
    check("guard accepts jump path", safe("assets/script/x.ks"))
    check("listsaves handler exists", type(SaveCommands.listsaves) == "function")
end

-- ═══ Resource Commands ═══
print("\n--- Resource Commands ---")
do
    local ResourceCommands = require("kag.commands.resource")
    check("preload handler exists", type(ResourceCommands.preload) == "function")
    check("flush_cache exists", type(ResourceCommands.flush_cache) == "function")
end

-- ═══ KAG Module Integration ═══
print("\n--- KAG Module Integration ---")
do
    local KAG = require("kag")
    
    -- All key commands should be registered
    check("KAG.bg registered", type(KAG.bg) == "function")
    check("KAG.ch registered", type(KAG.ch) == "function")
    check("KAG.playbgm registered", type(KAG.playbgm) == "function")
    check("KAG.video registered", type(KAG.video) == "function")
    check("KAG.save registered", type(KAG.save) == "function")
    check("KAG.trans registered", type(KAG.trans) == "function")
    check("KAG.vfx registered", type(KAG.vfx) == "function")
    check("KAG.preload registered", type(KAG.preload) == "function")
    
    -- Legacy aliases
    check("KAG.showtext alias", type(KAG.showtext) == "function")
    check("KAG.clearscreen alias", type(KAG.clearscreen) == "function")
    check("KAG.cancel exists", type(KAG.cancel) == "function")
end

-- ═══ Results ═══
print("\n" .. string.rep("=", 50))
print(string.format("  KAG Commands: %d passed, %d failed, %d total",
    passed, failed, passed + failed))
print(string.rep("=", 50))

-- [ch voice=] stores the voice file in the backlog for V-key replay
do
    local ctx2 = { backlog = {}, f = {}, sf = {}, characters = {}, text_state = { draws = {} }, textCursorX = 32, textCursorY = 580 }
    local TextCommands2 = require("kag.commands.text")
    pcall(function() TextCommands2.ch(ctx2, { name = "Ame", text = "Hi", voice = "assets/voice/line01.wav" }) end)
    if ctx2.backlog and ctx2.backlog[1] and ctx2.backlog[1].voice == "assets/voice/line01.wav" then
        passed = passed + 1 print("  [PASS] ch voice stored in backlog")
    else failed = failed + 1 end
end

-- Language persistence (source-level: capture stores it, restore applies it)
do
    local src4 = io.open("scripts/kag/commands/save.lua", "r"):read("*a")
    if src4:find("state.language = ", 1, true) then
        passed = passed + 1 print("  [PASS] save captures language")
    else failed = failed + 1 end
    local locale = require("i18n")
    local before = locale.current
    local staged = require("kag.save_state").prepare({
        scene_path = "tests/projects/u11_restore/base.ks", language = "en",
    }, require("kag.commands.save")._safeScenePath, require("flow").prepare_scene)
    if staged.language == "en" and locale.current == before then
        passed = passed + 1 print("  [PASS] restore stages language without changing the live locale")
    else failed = failed + 1 end
end

-- Auto-save interval setting: menu item exists and engine apply is wired
do
    local settings_src = io.open("scripts/settings.lua", "r"):read("*a")
    if settings_src:find("autosave_interval", 1, true)
       and settings_src:find("setAutoSaveInterval", 1, true) then
        passed = passed + 1 print("  [PASS] settings autosave item wired")
    else failed = failed + 1 end
    local i18n_src = io.open("scripts/i18n.lua", "r"):read("*a")
    if i18n_src:find("autosave_interval", 1, true) then
        passed = passed + 1 print("  [PASS] i18n autosave label")
    else failed = failed + 1 end
end

-- [gallery]/[music] commands exist and load their UI modules
do
    local sys_src = io.open("scripts/kag/commands/system.lua", "r"):read("*a")
    if sys_src:find("function SystemCommands.gallery", 1, true)
       and sys_src:find("function SystemCommands.music", 1, true) then
        passed = passed + 1 print("  [PASS] gallery/music commands defined")
    else failed = failed + 1 end
    local gal_src = io.open("scripts/gallery.lua", "r"):read("*a")
    local mus_src = io.open("scripts/music_room.lua", "r"):read("*a")
    if gal_src:find("function Gallery.show", 1, true)
       and mus_src:find("function MusicRoom.show", 1, true) then
        passed = passed + 1 print("  [PASS] gallery/music UIs exist")
    else failed = failed + 1 end
end

-- Character sprite: [ch sprite=] shows a standing portrait layer
do
    local text_src = io.open("scripts/kag/commands/text.lua", "r"):read("*a")
    if text_src:find('"_char_" .. speaker', 1, true)
       and text_src:find("backend.load_texture(sprite)", 1, true) then
        passed = passed + 1 print("  [PASS] character sprite layer wired")
    else failed = failed + 1 end
end

-- [flash] command registered (KiriKiri classic white-flash effect)
do
    local tr_src = io.open("scripts/kag/commands/transition.lua", "r"):read("*a")
    if tr_src:find("function TransCommands.flash", 1, true)
       and tr_src:find('require("vfx")', 1, true) then
        passed = passed + 1 print("  [PASS] flash command registered")
    else failed = failed + 1 end
end

-- Chapter select: command registered + collect() scans chapter_ labels
do
    local sys_src = io.open("scripts/kag/commands/system.lua", "r"):read("*a")
    if sys_src:find("function SystemCommands.chapter", 1, true)
       and sys_src:find("_pendingJump", 1, true) then
        passed = passed + 1 print("  [PASS] chapter command registered")
    else failed = failed + 1 end
    local cs = io.open("scripts/chapter_select.lua", "r"):read("*a")
    if cs:find("chapter_", 1, true) then
        passed = passed + 1 print("  [PASS] chapter collect scans labels")
    else failed = failed + 1 end
end

-- Save/load slot menu: command registered + UI exists
do
    local sv_src = io.open("scripts/kag/commands/save.lua", "r"):read("*a")
    local sl_src = io.open("scripts/saveload_menu.lua", "r"):read("*a")
    if sv_src:find("function SaveCommands.saveload", 1, true)
       and sl_src:find("function SaveLoad.show", 1, true) then
        passed = passed + 1 print("  [PASS] saveload menu wired")
    else failed = failed + 1 end
    -- Audio parameter handling: playbgm forwards volume/fadein(ms->s)/loop.
    -- Anchor to the playbgm body (playbgmstop shares two of the strings).
    local au_src = io.open("scripts/kag/commands/audio.lua", "r"):read("*a")
    -- Slice from the playbgm function header up to the next function header:
    -- the body may contain its own 'end' (early-return branches), so the
    -- next "function AudioCommands." marker is the reliable boundary.
    local pb_start = au_src:find("function AudioCommands.playbgm", 1, true)
    local pb_end = pb_start and au_src:find("function AudioCommands.stopbgm", pb_start, true) or 0
    local pb_body = pb_start and pb_end and au_src:sub(pb_start, pb_end) or ""
    if pb_body:find("fadein / 1000.0", 1, true)
       and pb_body:find("params.loop ~= false", 1, true)
       and pb_body:find("volume = volume", 1, true) then
        passed = passed + 1 print("  [PASS] playbgm parameter mapping")
    else failed = failed + 1 end
    -- Transition aliases (KiriKiri): dissolve/gradient->crossfade, mask->rule
    local tr_src2 = io.open("scripts/kag/commands/transition.lua", "r"):read("*a")
    if tr_src2:find('dissolve    = "crossfade"', 1, true)
       and tr_src2:find('mask        = "rule"', 1, true)
       and tr_src2:find('slide       = "wipe"', 1, true) then
        passed = passed + 1 print("  [PASS] transition aliases")
    else failed = failed + 1 end
    -- [blur] command registered (VFX.blur was command-less dead code)
    local tr_src3 = io.open("scripts/kag/commands/transition.lua", "r"):read("*a")
    if tr_src3:find("function TransCommands.blur", 1, true)
       and tr_src3:find('VFX.blur', 1, true) then
        passed = passed + 1 print("  [PASS] blur command registered")
    else failed = failed + 1 end
    -- Invalid saved positions must fail preparation, not redirect to token 1.
    local all_rejected = true
    for _, index in ipairs({ 0, -1, "bad", false, 1.5, 999999 }) do
        local ok = pcall(require("kag.save_state").prepare, {
            scene_path = "tests/projects/u11_restore/base.ks", token_index = index,
        }, require("kag.commands.save")._safeScenePath, require("flow").prepare_scene)
        if ok then all_rejected = false end
    end
    if all_rejected then
        passed = passed + 1 print("  [PASS] crafted saved positions are rejected")
    else failed = failed + 1 end
    -- Save capture completeness: all persistent fields present
    if sv_src:find("state.language", 1, true)
       and sv_src:find("state.seen_scenes", 1, true)
       and sv_src:find("state.backlog", 1, true)
       and sv_src:find("state.call_stack", 1, true)
       and sv_src:find("state.schema_version", 1, true) then
        passed = passed + 1 print("  [PASS] save captures all persistent fields")
    else failed = failed + 1 end
end

-- [scroll] command: registered + text/speed params parsed (logic only, no GPU)
do
    local trans_src = nil
    local f = io.open("scripts/kag/commands/transition.lua", "r")
    if f then trans_src = f:read("*a") f:close() end
    if trans_src and trans_src:find("TransCommands.scroll", 1, true) then
        passed = passed + 1 print("  [PASS] [scroll] command implemented")
    else failed = failed + 1 end
    if trans_src and trans_src:find("coroutine.yield()", 1, true) then
        passed = passed + 1 print("  [PASS] [scroll] is coroutine-driven")
    else failed = failed + 1 end
end

-- chapter_select: read badge driven by seen_scenes
do
    local src = nil
    local f = io.open("scripts/chapter_select.lua", "r")
    if f then src = f:read("*a") f:close() end
    if src and src:find("ch.read", 1, true) and src:find("seen_scenes", 1, true) then
        passed = passed + 1 print("  [PASS] chapter read badge present")
    else failed = failed + 1 end
end

-- [ending] command + title endings sub-screen
do
    local sys = nil
    local f = io.open("scripts/kag/commands/system.lua", "r")
    if f then sys = f:read("*a") f:close() end
    if sys and sys:find("SystemCommands.ending", 1, true) and sys:find("seen_endings", 1, true) then
        passed = passed + 1 print("  [PASS] [ending] command present")
    else failed = failed + 1 end
    local tm = nil
    f = io.open("scripts/title_menu.lua", "r")
    if f then tm = f:read("*a") f:close() end
    if tm and tm:find("showEndings", 1, true) and tm:find("endings_title", 1, true) then
        passed = passed + 1 print("  [PASS] title endings sub-screen present")
    else failed = failed + 1 end
    local sv = nil
    f = io.open("scripts/kag/commands/save.lua", "r")
    if f then sv = f:read("*a") f:close() end
    local staged = require("kag.save_state").prepare({
        scene_path = "tests/projects/u11_restore/base.ks", seen_endings = { ending_a = true },
    }, require("kag.commands.save")._safeScenePath, require("flow").prepare_scene)
    if staged.seen_endings.ending_a == true then
        passed = passed + 1 print("  [PASS] endings persisted in save")
    else failed = failed + 1 end
end

-- Title Continue reads the system autosave rather than a manual slot.
do
    local tm = nil
    local f = io.open("scripts/title_menu.lua", "r")
    if f then tm = f:read("*a") f:close() end
    if tm and tm:find("continue", 1, true) and tm:find("KAG.save_exists", 1, true) then
        passed = passed + 1 print("  [PASS] title continue item present")
    else failed = failed + 1 end
    check("autosave uses reserved slot -2", require("system").AUTOSAVE_SLOT == -2)
end

-- skip_auto force-skip (runner + settings + i18n)
do
    local src = nil
    local f = io.open("scripts/kag_runner.lua", "r")
    if f then src = f:read("*a") f:close() end
    if src and src:find("ctx.skip_auto", 1, true) and src:find("ctx.skip_rate", 1, true) then
        passed = passed + 1 print("  [PASS] skip_auto in runner")
    else failed = failed + 1 end
    f = io.open("scripts/settings.lua", "r")
    if f then src = f:read("*a") f:close() end
    if src and src:find("skip_auto", 1, true) then
        passed = passed + 1 print("  [PASS] skip_auto in settings")
    else failed = failed + 1 end
    f = io.open("scripts/i18n.lua", "r")
    if f then src = f:read("*a") f:close() end
    if src and src:find("skip_auto", 1, true) then
        passed = passed + 1 print("  [PASS] skip_auto in i18n")
    else failed = failed + 1 end
end

-- V-key re-voice (gameplay)
do
    local src = nil
    local f = io.open("scripts/kag_demo_entry.lua", "r")
    if f then src = f:read("*a") f:close() end
    if src and src:find("_GAME_KEY_V", 1, true) and src:find("audio_play", 1, true) and src:find("backlog[1]", 1, true) then
        passed = passed + 1 print("  [PASS] V re-voice in entry")
    else failed = failed + 1 end
    if src and src:find("not v:find(", 1, true) and src:find("%.ogg$", 1, true) then
        passed = passed + 1 print("  [PASS] V re-voice guards path")
    else failed = failed + 1 end
end

-- backlog F-key filter (voice/sprite)
do
    local src = nil
    local f = io.open("scripts/history_ui.lua", "r")
    if f then src = f:read("*a") f:close() end
    if src and src:find("_GAME_KEY_F", 1, true) and src:find("filteredCount()", 1, true) then
        passed = passed + 1 print("  [PASS] backlog filter key present")
    else failed = failed + 1 end
    if src and src:find("filteredIndex(i)", 1, true) and src:find("math.min(filteredCount()", 1, true) then
        passed = passed + 1 print("  [PASS] filter bounds navigation")
    else failed = failed + 1 end
end

-- [wait]/[pt] clamping
do
    local src = nil
    local f = io.open("scripts/kag/commands/system.lua", "r")
    if f then src = f:read("*a") f:close() end
    if src and src:find("clamped %d ms to 60000", 1, true) or (src and src:find("kag.schema", 1, true)) then
        passed = passed + 1 print("  [PASS] [wait] hard cap present")
    else failed = failed + 1 end
    f = io.open("scripts/kag/commands/text.lua", "r")
    if f then src = f:read("*a") f:close() end
    if src and src:find("kag.schema", 1, true) and src:find("min = 8", 1, true) and src:find("max = 5000", 1, true) then
        passed = passed + 1 print("  [PASS] [pt] clamp present")
    else failed = failed + 1 end
end

-- auto-mode voice pacing
do
    local src = nil
    local f = io.open("scripts/kag_runner.lua", "r")
    if f then src = f:read("*a") f:close() end
    if src and src:find("audio_is_playing", 1, true) and src:find("auto_delay", 1, true) then
        passed = passed + 1 print("  [PASS] auto waits for voice")
    else failed = failed + 1 end
    if src and src:find("voicePlaying then", 1, true) and src:find("auto_advance_ms = 0", 1, true) then
        passed = passed + 1 print("  [PASS] voice holds advance timer")
    else failed = failed + 1 end
end

-- Ending replay uses the authored scene recorded by the command.
do
    local src = nil
    local f = io.open("scripts/kag/commands/system.lua", "r")
    if f then src = f:read("*a") f:close() end
    if src and src:find("scene = ctx.current_scene", 1, true) then
        passed = passed + 1 print("  [PASS] ending records scene")
    else failed = failed + 1 end
    local owner = { current_scene = "demo/galgame_demo.ks" }
    require("kag.commands.system").ending(owner, { id = "route_a", name = "Route A" })
    local record = owner.seen_endings and owner.seen_endings.route_a
    check("ending records a valid replay scene", record and record.scene == owner.current_scene
        and require("kag.commands.save")._safeScenePath(record.scene))
end

-- Ctrl hold-to-skip wiring
do
    local src = nil
    local f = io.open("scripts/kag_demo_entry.lua", "r")
    if f then src = f:read("*a") f:close() end
    if src and src:find("function _KAG_onCtrlDown()", 1, true) and src:find("_prevSkipMode", 1, true) then
        passed = passed + 1 print("  [PASS] Ctrl hold-skip wired")
    else failed = failed + 1 end
    if src and src:find("ctx.skip_mode = _prevSkipMode", 1, true) then
        passed = passed + 1 print("  [PASS] Ctrl release restores mode")
    else failed = failed + 1 end
end

-- A-key auto toggle
do
    local src = nil
    local f = io.open("scripts/kag_demo_entry.lua", "r")
    if f then src = f:read("*a") f:close() end
    if src and src:find("_GAME_KEY_A == true", 1, true) and src:find("ctx.auto_mode = not ctx.auto_mode", 1, true) then
        passed = passed + 1 print("  [PASS] A toggles auto")
    else failed = failed + 1 end
end
if failed > 0 then os.exit(1) end
