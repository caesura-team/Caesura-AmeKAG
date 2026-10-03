-- =============================================================================
--  Caesura (AmeKAG) — golden_vn_headless.lua
--  Headless driver for the Golden Project v1 fixture (tests/projects/golden_vn/),
--  modeled on first_vn_headless.lua with three v1 additions:
--    1. GOLDEN_ROUTE env — 1-based index of the [select] option the
--       auto-clicker picks (route A = forest / route B = city), so BOTH
--       branches are driven end-to-end.
--    2. Cross-scene [jump] seam for GOLDEN_CROSS=1 — the scheduler resolves a
--       non-* storage target as assets/script/<target>.ks (scripts/scheduler.lua
--       is_safe_scene_path); the engine loads scenes from that logical runtime
--       location, but this fixture keeps its sources under
--       tests/projects/golden_vn/. So the driver remaps the logical name to the
--       fixture file BEFORE any jump executes (the ctx.load_tokens callback is
--       read at jump time). The remap is the ONLY seam: the scheduler's
--       cross-scene machinery (path building, safety check, switch budget,
--       token/label swap, fresh local frame) runs for real. Cross-scene runs
--       use the dedicated golden_cross.ks starter so the scheduler's deferred
--       choice pending-jump (consumed at the next coroutine death) never
--       overlaps the scene switch.
--    3. Feature flags after the run: EVAL_OK / LOAD_MISS_OK / MACRO_OK /
--       XSCENE_OK plus a FLAGS line with f.energy, f.after_load, f.sceneB,
--       f.crossFallback, f.savedByStory, f.macroUsed — gates grep these.
--
--  This is a DRIVER, not a unit test: run_lua_tests.lua does not register
--  it. Mock semantics are identical to sample_game_headless.lua (in
--  particular KAG.load_game returns a non-table, so [load slot=99] takes its
--  graceful-miss path and the story continues past it).
--
--  Usage (from repo root):
--    GOLDEN_ROUTE=1 build/lua/Debug/lua.exe tests/scripts/golden_vn_headless.lua
--    GOLDEN_ROUTE=2 build/lua/Debug/lua.exe tests/scripts/golden_vn_headless.lua
--    GOLDEN_CROSS=1   build/lua/Debug/lua.exe tests/scripts/golden_vn_headless.lua
--    GOLDEN_RB=1       build/lua/Debug/lua.exe tests/scripts/golden_vn_headless.lua     (v2)
--    GOLDEN_HISTORY=1  build/lua/Debug/lua.exe tests/scripts/golden_vn_headless.lua     (v2)
--    GOLDEN_ROUNDTRIP=1 build/lua/Debug/lua.exe tests/scripts/golden_vn_headless.lua    (v2)
--    GOLDEN_NVL=1      build/lua/Debug/lua.exe tests/scripts/golden_vn_headless.lua     (v3)
--    GOLDEN_VOICE=1    build/lua/Debug/lua.exe tests/scripts/golden_vn_headless.lua     (v3)
--
--  v2 modes:
--    GOLDEN_RB       stages a jump to *rollback_check and drives
--                    kag_runner.rollback() twice while the run pauses at the
--                    [wait]: #1 pops the {f.rb=2} snapshot (token rewound,
--                    f.rb still 2), #2 pops the {f.rb=1} snapshot (the
--                    "only via snapshot.restore" value). Prints RB_FORWARD_* /
--                    RB_POP1_* / RB_POP2_A / RB_REPLAY_END.
--    GOLDEN_HISTORY  stages a jump to *history_check, verifies ctx.backlog
--                    content while the [history] overlay is open (count +
--                    text/name/scene/token_index per entry), closes it with
--                    Esc and asserts the story continues. Prints
--                    HISTORY_OPEN / BACKLOG_ENTRY1 / BACKLOG_OK /
--                    HISTORY_OK.
--    GOLDEN_ROUNDTRIP drives tests/scripts/golden_rt.ks (allowlisted scene
--                    path — see the file header), issues the load through
--                    SaveCommands.load(ctx,{slot=9}) — the exact [load] tag
--                    handler — while the story pauses at the [wait], asserts
--                    the restore synchronously (f.rtMarker back to PRE_SAVE),
--                    then drives the resume-from-save replay to [end].
--                    Prints RT_FORWARD_* / RT_RESUME_ARMED / ROUNDTRIP_OK /
--                    RT_REPLAY_END.
--    Engine-defect note (v2, reported not fixed): a same-scene [load] TAG
--    re-enters the saved resume point and re-executes the downstream [load]
--    token, looping on the native runner; the cursor+1 self-reference guard
--    exists only in web/bridge.js. The driver therefore drives the same
--    handler instead of an in-story [load] tag.
--
--  v3 modes (2026-08-30):
--    GOLDEN_NVL       stages a jump to *nvl_check and asserts the NVL
--                     accumulation semantics: two [ch] lines on ONE page
--                     (ctx.nvl_mode true, page_src[].opts.nvl, draws
--                     appended, textCursorY advanced, TextScene.commit
--                     seal = prior draws typewriter=false / appended
--                     draw true, backlog carries both lines), the
--                     [nvl clear] page break (page_src==1 fresh page,
--                     cursor back toward the NVL top), [save slot=7]
--                     (state.nvl_mode==true, save.lua:165) and [nvl off]
--                     (mode false, page dropped, nvl_hidden_vis cleared).
--                     Prints NVL_ACCUM_OK / NVL_PAGE_OK / NVL_SAVE_OK /
--                     NVL_OFF_OK.
--    GOLDEN_VOICE     stages a jump to *voice_check and asserts the voice
--                     routing: [ch voice=] stores the file in the backlog
--                     entry (text.lua push_backlog voice field) AND in the
--                     saved state (save.lua:87 backlog[].voice), and
--                     [playvoice storage=] dispatches backend.audio_play
--                     ("voice") -- the mock's play_voice spy records each
--                     call (the C++ binding contract) while
--                     is_voice_playing=false makes the wait loop return
--                     immediately, so the story continues past it to
--                     [end]. Prints VOICE_BL_OK / VOICE_SAVE_OK /
--                     VOICE_DISPATCH_OK.
--
--  Env knobs:
--    SAMPLE_STORY      .ks path (default story.ks, or golden_cross.ks with
--                      GOLDEN_CROSS=1, or golden_rt.ks with GOLDEN_ROUNDTRIP=1)
--    SAMPLE_ENDING     optional label jump at boot (reachability probe;
--                      defaulted to rollback_check / history_check in v2 modes)
--    SAMPLE_FRAMES     frame budget (default 200000)
--    GOLDEN_ROUTE      1-based select-option index (default: first option)
--    GOLDEN_CROSS      1 = drive tests/projects/golden_vn/golden_cross.ks
--    GOLDEN_RB / GOLDEN_HISTORY / GOLDEN_ROUNDTRIP   1 = enable v2 mode (see above)
--
--  v1 note (unchanged): KAG.load_game returns a non-table so [load slot=99]
--  takes the graceful-miss path; the v2 roundtrip mode overrides the mock
--  with an in-memory slot store (KAG.save_game/load_game).
--
--  Exit: 0 = reached [end] (DONE), 1 = frame limit / fatal, 2 = target label
--  not found.
-- =============================================================================

package.path = "scripts/?.lua;scripts/?/init.lua;scripts/kag/?.lua;scripts/kag/commands/?.lua;" .. package.path

-- ---- Mock C++ bindings (identical to sample_game_headless.lua) ----
local function callable(t)
    return setmetatable(t or {}, {
        __index = function(self, key)
            if type(key) ~= "string" then return nil end
            rawset(self, key, function(...) return true end)
            return self[key]
        end,
    })
end

-- v2 roundtrip: in-memory save store. v1 mock semantics (load_game returns a
-- non-table -> graceful miss) are the DEFAULT; GOLDEN_ROUNDTRIP overrides
-- save_game/load_game with the store so [save slot=9] really persists and the
-- load handler sees the captured state table.
local _savedSlots = {}
-- v3 voice spy: the C++ binding contract is KAG.play_voice(file)
-- (backend.lua audio_play "voice" -> call_resolved("play_voice") when
-- _CAESURA_BACKEND is absent, as in this headless mock). Every voice
-- dispatch ([ch voice=] and [playvoice]) lands here, in story order.
local _voicePlays = {}
_G.KAG = callable({
    is_voice_playing  = function() return false end,
    is_bgm_playing    = function() return false end,
    get_active_voices = function() return 0 end,
    play_voice        = function(file)
        _voicePlays[#_voicePlays + 1] = file
        return true
    end,
    save_game = function(slot, state)
        _savedSlots[tonumber(slot) or 0] = state
        return true
    end,
    load_game = function(slot)
        local s = _savedSlots[tonumber(slot) or 0]
        return s, nil
    end,
})
_G.Render  = callable({})
_G.DevCore = callable({})
_G.Engine  = callable({})
_G.backend = callable({
    is_voice_playing  = function() return false end,
    is_bgm_playing    = function() return false end,
    get_active_voices = function() return 0 end,
})

local kag_runner = require("kag_runner")
local headless = dofile("tests/scripts/headless_render_fixture.lua").install()

local CROSS  = os.getenv("GOLDEN_CROSS") == "1"
local RB     = os.getenv("GOLDEN_RB") == "1"
local HIST   = os.getenv("GOLDEN_HISTORY") == "1"
local RT     = os.getenv("GOLDEN_ROUNDTRIP") == "1"
local NVL    = os.getenv("GOLDEN_NVL") == "1"
local VOICE  = os.getenv("GOLDEN_VOICE") == "1"
local STORY  = os.getenv("SAMPLE_STORY")
    or (RT and "tests/scripts/golden_rt.ks")
    or (CROSS and "tests/projects/golden_vn/golden_cross.ks"
                  or "tests/projects/golden_vn/story.ks")
local ENDING = os.getenv("SAMPLE_ENDING")
    or (RB and "rollback_check")
    or (HIST and "history_check")
    or (NVL and "nvl_check")
    or (VOICE and "voice_check")
local FMAX   = tonumber(os.getenv("SAMPLE_FRAMES")) or 200000
local CHOICE = tonumber(os.getenv("GOLDEN_ROUTE") or "")

local started, err = kag_runner.start(STORY)
if not started then
    print("FATAL_START: " .. tostring(err))
    os.exit(1)
end

-- ---- Cross-scene [jump] seam (see header comment) --------------------------
-- Read by scheduler at jump time; the logical runtime name of the v1 scene B
-- is assets/script/golden_scene_b.ks (golden_cross.ks jumps there), which
-- maps to the fixture source file.
local ctx0 = _G._CAESURA_CTX
if CROSS then
    local real_load = ctx0.load_tokens
    ctx0.load_tokens = function(path)
        if path == "assets/script/golden_scene_b.ks" then
            print("XSCENE_REMAP " .. path .. " -> tests/projects/golden_vn/scene_b.ks")
            return real_load("tests/projects/golden_vn/scene_b.ks")
        end
        return real_load(path)
    end
end

if ENDING and ENDING ~= "" then
    local ctx    = _G._CAESURA_CTX
    local target = ENDING:sub(1, 1) == "*" and ENDING or ("*" .. ENDING)
    if not kag_runner.stage_label_jump(ctx, target) then
        print("ENDING_NOT_FOUND: " .. target)
        os.exit(2)
    end
    -- The staged token restart requires a dead-branch signal: the choice-path
    -- _pendingJump is the one that re-spawns the scheduler at a "*label".
    -- (Plain stop_flag alone ends the runner with "Script ended" -- the v1
    -- sample driver's ending probe shares this; a staged jump without the
    -- pending signal is hollow.)
    ctx._pendingJump = target
    ctx.stop_flag = true
    print("ENDING_JUMP: " .. target)
end

-- Auto-click; when GOLDEN_ROUTE is set, pick that option (1-based).
local function drive_click()
    local ctx = _G._CAESURA_CTX
    if ctx and ctx._choiceMode then
        _G._GAME_MOUSE_X = 100
        local cbs = ctx._choiceButtonsActive or ctx._choiceButtons
        local idx = 1
        if CHOICE and cbs and #cbs > 0 then
            idx = CHOICE < 1 and 1 or (CHOICE > #cbs and #cbs or CHOICE)
        end
        local ch = cbs and cbs[idx]
        _G._GAME_MOUSE_Y = (ch and tonumber(ch.y) or 450)
                         + (ch and tonumber(ch.h) or 24) / 2
        local kc = _G._KAG_onClick
        if type(kc) == "function" then kc() end
        -- Belt-and-suspenders: force the chosen option if the hit-test missed
        if ctx._choiceMode and type(cbs) == "table" and cbs[idx] then
            ctx._selectedChoice      = cbs[idx]
            ctx._choiceMode          = false
            ctx._choiceButtonsActive = nil
            ctx.waiting_input        = false
        end
        return
    end
    kag_runner.on_click()
end

local frames, clicks = 0, 0
local result = nil

-- ---- v2 mode state ----------------------------------------------------------
local rbDone    = false   -- GOLDEN_RB: rollback pair executed
local histEsc   = false   -- GOLDEN_HISTORY: overlay Esc sent
local rtLoaded  = false   -- GOLDEN_ROUNDTRIP: load() issued + restore checked

-- ---- v3 mode state ----------------------------------------------------------
local nvlAccumed = false   -- GOLDEN_NVL: accumulation inspected
local nvlPaged   = false   -- GOLDEN_NVL: [nvl clear] page break inspected
local nvlSaved   = false   -- GOLDEN_NVL: save-state nvl_mode inspected
local nvlOffed   = false   -- GOLDEN_NVL: [nvl off] inspected
local nvlCursor1 = nil     -- cursor Y at accumulation (page-reset baseline)
local voiceCh    = false   -- GOLDEN_VOICE: backlog voice inspected
local voiceSave  = false   -- GOLDEN_VOICE: save-state backlog voice inspected
local voicePlay  = false   -- GOLDEN_VOICE: playvoice dispatch inspected

while frames < FMAX do
    frames = frames + 1
    local ok, reason = kag_runner.update(0.016)
    headless.check(kag_runner, ok, reason)
    local ctx = _G._CAESURA_CTX

    -- v2 rollback: the run pauses at the [wait] (f.rbReady==1); drive the two
    -- snapshot pops synchronously (both restores are immediate).
    if RB and not rbDone and ctx and ctx.f and ctx.f.rbReady == 1 then
        rbDone = true
        local preIdx = (type(ctx.token_index) == "number") and ctx.token_index or nil
        print("RB_FORWARD rb=" .. tostring(ctx.f.rb)
              .. " observedB=" .. tostring(ctx.f.rbObsB == 1))
        local ok1 = kag_runner.rollback()
        print("RB_POP1 ok=" .. tostring(ok1)
              .. " rb=" .. tostring(ctx.f.rb)
              .. " rewind=" .. tostring(preIdx ~= nil and type(ctx.token_index) == "number"
                                          and ctx.token_index < preIdx))
        local ok2 = kag_runner.rollback()
        print("RB_POP2 ok=" .. tostring(ok2) .. " rb=" .. tostring(ctx.f.rb))
        if ok1 and ok2 and ctx.f.rb == 1 then print("RB_POP2_A") end
    end

    -- v2 history: while the [history] overlay is open, verify backlog content
    -- (count + per-entry fields), then close it with Esc.
    if HIST and not histEsc and ctx and ctx.input_focus == "history" then
        histEsc = true
        local bl = ctx.backlog or {}
        local e1 = bl[1]
        print("HISTORY_OPEN backlog=" .. tostring(#bl))
        print("BACKLOG_ENTRY1 text=" .. tostring(e1 and e1.text or "?")
              .. " name=" .. tostring(e1 and e1.name or "?")
              .. " scene=" .. tostring(e1 and e1.scene or "?")
              .. " token=" .. tostring(e1 and e1.token_index or "?"))
        local okCount    = (#bl >= 2)
        local okContent  = (e1 and type(e1.text) == "string" and #e1.text > 0
                            and type(e1.name) == "string" and #e1.name > 0
                            and type(e1.scene) == "string" and #e1.scene > 0
                            and type(e1.token_index) == "number" and e1.token_index >= 1)
        if okCount and okContent then print("BACKLOG_OK") end
        _G._GAME_KEY_ESC = true
    end

    -- v2 roundtrip: the run pauses at the [wait] (f.rtReady==1); issue the
    -- load through the exact [load] tag handler and assert the restore
    -- synchronously, before any replay can re-run.
    if RT and not rtLoaded and ctx and ctx.f and ctx.f.rtReady == 1 then
        rtLoaded = true
        print("RT_FORWARD marker=" .. tostring(ctx.f.rtMarker)
              .. " counter=" .. tostring(ctx.f.rtCounter))
        local okL, SaveCmds = pcall(require, "kag.commands.save")
        local okH, herr = false, nil
        local previous, previous_co = ctx, ctx.co
        if okL then okH, herr = pcall(SaveCmds.load, ctx, { slot = 9 }) end
        ctx = kag_runner.get_ctx()
        print("RT_LOAD_CALL ok=" .. tostring(okH) .. (herr and (" " .. tostring(herr)) or ""))
        -- Prove the committed session has its own live coroutine and that the
        -- previous coroutine was closed, rather than trusting a pending flag.
        local pls = ctx and ctx.current_scene
        if okH and herr == true and ctx ~= previous and ctx._session_active
            and ctx.co and coroutine.status(ctx.co) == "suspended"
            and (not previous_co or coroutine.status(previous_co) == "dead")
            and pls == "tests/scripts/golden_rt.ks" then
            print("RT_RESUME_ARMED scene=" .. tostring(pls))
        else
            print("RT_RESUME_MISSING scene=" .. tostring(pls))
        end
        if okH and herr == true and ctx.f.rtMarker == "PRE_SAVE" and ctx.f.rtCounter == 1 then
            print("ROUNDTRIP_OK")
        else
            print("RT_RESTORE_BAD marker=" .. tostring(ctx.f.rtMarker)
                  .. " counter=" .. tostring(ctx.f.rtCounter))
        end
    end

    -- v3 NVL: the run parks at the [wait] after two accumulated [ch] lines
    -- (no [p] between them). Assert the full-screen accumulation semantics:
    -- mode on, one page_src entry per line with opts.nvl, draws appended,
    -- cursor advanced below NVL_Y0 (and the ctx/state mirrors consistent),
    -- the TextScene.commit seal (prior sealed, appended typewriter-live),
    -- and the backlog carrying both lines without the inline prefix.
    if NVL and not nvlAccumed and ctx and ctx.f and ctx.f.nvlAccumReady == 1 then
        nvlAccumed = true
        local st  = require("kag.text_scene").get_state(ctx)
        local ps  = st.page_src or {}
        local dr  = st.draws or {}
        local bl  = ctx.backlog or {}
        nvlCursor1 = ctx.textCursorY
        print("NVL_ACCUM mode=" .. tostring(ctx.nvl_mode)
              .. " page_src=" .. tostring(#ps) .. " draws=" .. tostring(#dr)
              .. " cursorY=" .. tostring(ctx.textCursorY)
              .. " stateY=" .. tostring(st.cursor_y)
              .. " backlog=" .. tostring(#bl))
        local okMode  = (ctx.nvl_mode == true)
        local okPage  = (#ps == 2)
                        and (ps[1] and ps[1].opts and ps[1].opts.nvl == true)
                        and (ps[2] and ps[2].opts and ps[2].opts.nvl == true)
        local okDraws = (#dr >= 2)
        local okCur   = (type(ctx.textCursorY) == "number" and ctx.textCursorY > 160)
                        and (ctx.textCursorY == st.cursor_y)
        local okSeal  = (#dr >= 2) and (dr[1].typewriter == false)
                        and (dr[#dr].typewriter == true)
        local okBl    = (#bl == 2) and (bl[1] and bl[1].name == "A")
                        and (bl[2] and bl[2].name == "B")
                        and (type(bl[1].text) == "string"
                             and bl[1].text:find("「", 1, true) == nil)
        if okMode and okPage and okDraws and okCur and okSeal and okBl then
            print("NVL_ACCUM_OK")
        else
            print("NVL_ACCUM_BAD mode=" .. tostring(okMode)
                  .. " page=" .. tostring(okPage) .. " draws=" .. tostring(okDraws)
                  .. " cursor=" .. tostring(okCur) .. " seal=" .. tostring(okSeal)
                  .. " backlog=" .. tostring(okBl))
        end
    end

    -- v3 NVL: after [nvl clear] + the fresh-page [ch], assert the page break
    -- semantics: the old page is gone (page_src==1), the cursor was reset
    -- toward the NVL top (strictly above the accumulation cursor), mode stays
    -- on.
    if NVL and not nvlPaged and ctx and ctx.f and ctx.f.nvlPageReady == 1 then
        nvlPaged = true
        local st  = require("kag.text_scene").get_state(ctx)
        local ps  = st.page_src or {}
        local dr  = st.draws or {}
        print("NVL_PAGE mode=" .. tostring(ctx.nvl_mode)
              .. " page_src=" .. tostring(#ps) .. " draws=" .. tostring(#dr)
              .. " cursorY=" .. tostring(ctx.textCursorY)
              .. " prevCursorY=" .. tostring(nvlCursor1))
        local okMode = (ctx.nvl_mode == true)
        local okPage = (#ps == 1)
                       and (ps[1] and ps[1].opts and ps[1].opts.nvl == true)
        local okCur  = (type(ctx.textCursorY) == "number"
                        and ctx.textCursorY > 160
                        and nvlCursor1 ~= nil
                        and ctx.textCursorY < nvlCursor1)
        if okMode and okPage and (#dr >= 1) and okCur then
            print("NVL_PAGE_OK")
        else
            print("NVL_PAGE_BAD mode=" .. tostring(okMode)
                  .. " page=" .. tostring(okPage)
                  .. " cursor=" .. tostring(okCur))
        end
    end

    -- v3 NVL: while still in NVL mode, [save slot=7] must persist nvl_mode
    -- into the saved state (save.lua capture_state line 165) together with
    -- the accumulated backlog entries.
    if NVL and not nvlSaved and ctx and ctx.f and ctx.f.nvlSaveReady == 1 then
        nvlSaved = true
        local s7  = _savedSlots[7]
        local sbl = s7 and s7.backlog or {}
        print("NVL_SAVE nvl_mode=" .. tostring(s7 and s7.nvl_mode)
              .. " backlog=" .. tostring(#sbl))
        if s7 and s7.nvl_mode == true and #sbl >= 2 then
            print("NVL_SAVE_OK")
        else
            print("NVL_SAVE_BAD")
        end
    end

    -- v3 NVL: after [nvl off], assert the exit semantics: mode false, the
    -- full-screen page dropped, the hidden-visibility bookkeeping cleared.
    if NVL and not nvlOffed and ctx and ctx.f and ctx.f.nvlOffReady == 1 then
        nvlOffed = true
        local st = require("kag.text_scene").get_state(ctx)
        local ps = st.page_src or {}
        print("NVL_OFF mode=" .. tostring(ctx.nvl_mode)
              .. " page_src=" .. tostring(#ps)
              .. " hidden_vis_cleared=" .. tostring(ctx.nvl_hidden_vis == nil))
        if ctx.nvl_mode == false and #ps == 0 and ctx.nvl_hidden_vis == nil then
            print("NVL_OFF_OK")
        else
            print("NVL_OFF_BAD")
        end
    end

    -- v3 voice: after [ch voice=], the backlog entry must carry the voice
    -- file (text.lua push_backlog voice field, V-key replay contract).
    if VOICE and not voiceCh and ctx and ctx.f and ctx.f.voiceChReady == 1 then
        voiceCh = true
        local bl = ctx.backlog or {}
        local e  = bl[#bl]
        print("VOICE_CH backlog=" .. tostring(#bl)
              .. " voice=" .. tostring(e and e.voice or "?")
              .. " name=" .. tostring(e and e.name or "?"))
        if e and e.voice == "assets/voice/line01.wav"
           and e.name == "A"
           and type(e.text) == "string" and #e.text > 0 then
            print("VOICE_BL_OK")
        else
            print("VOICE_BL_BAD")
        end
    end

    -- v3 voice: after [save slot=8], the SAVED state must carry the voice
    -- field per backlog entry (save.lua capture_state line 87).
    if VOICE and not voiceSave and ctx and ctx.f and ctx.f.voiceSaveReady == 1 then
        voiceSave = true
        local s8  = _savedSlots[8]
        local sbl = s8 and s8.backlog or {}
        local e   = sbl[#sbl]
        print("VOICE_SAVE backlogs=" .. tostring(#sbl)
              .. " voice=" .. tostring(e and e.voice or "?")
              .. " name=" .. tostring(e and e.name or "?"))
        if e and e.voice == "assets/voice/line01.wav"
           and e.name == "A"
           and type(e.text) == "string" and #e.text > 0 then
            print("VOICE_SAVE_OK")
        else
            print("VOICE_SAVE_BAD")
        end
    end

    -- v3 voice: after [playvoice storage=], the mock's play_voice spy must
    -- have recorded BOTH dispatches in story order ([ch voice=] then the
    -- playvoice tag) -- the exact backend.audio_play("voice") contract --
    -- and, with is_voice_playing=false, the wait loop must not stall the
    -- runner (the story reaches the following markers/[end]).
    if VOICE and not voicePlay and ctx and ctx.f and ctx.f.voicePlayReady == 1 then
        voicePlay = true
        print("VOICE_PLAY dispatches=" .. tostring(#_voicePlays)
              .. " first=" .. tostring(_voicePlays[1] or "?")
              .. " second=" .. tostring(_voicePlays[2] or "?"))
        if #_voicePlays >= 2
           and _voicePlays[1] == "assets/voice/line01.wav"
           and _voicePlays[2] == "assets/voice/line02.wav" then
            print("VOICE_DISPATCH_OK")
        else
            print("VOICE_DISPATCH_BAD")
        end
    end

    headless.check(kag_runner) -- Mode probes above may resume the coroutine too.
    if reason == "ended" then
        if RB and rbDone then print("RB_REPLAY_END") end
        if HIST and ctx and ctx.f and ctx.f.historyClosed == 1 then print("HISTORY_OK") end
        if RT and rtLoaded then
            print("RT_REPLAY_END")
            local current = kag_runner.get_ctx()
            if current and current.f.rtMarker == "POST_SAVE_MUTATED"
                and current.f.rtCounter == 2 and current.f.rtReady == 1 then
                print("RT_CONTINUATION_OK")
            end
        end
        if NVL and nvlOffed then print("NVL_REPLAY_END") end
        if VOICE and voicePlay then print("VOICE_REPLAY_END") end
        result = "DONE:" .. frames
        break
    end
    if ctx and ctx.tokens and ctx.token_index
       and ctx.token_index > #ctx.tokens then
        result = "DONE(exhausted):" .. frames
        break
    end
    if ctx and (ctx.waiting_input or ctx._choiceMode) then
        clicks = clicks + 1
        drive_click()
        headless.check(kag_runner)
    end
    headless.pump(kag_runner)
end
if not result then result = "FRAME_LIMIT" end

local ctx = _G._CAESURA_CTX
print("RESULT " .. result
      .. " token=" .. tostring(ctx.token_index)
      .. " clicks=" .. clicks
      .. " scene=" .. tostring(ctx.current_scene))

-- Which branch executed? f.route is set by [set var="f.route"] per branch.
local route = (ctx and ctx.f and ctx.f.route) or "none"
print("ROUTE " .. tostring(route))

-- ---- v1 feature flags ------------------------------------------------------
local f = ctx and ctx.f or {}
print("FLAGS energy=" .. tostring(f.energy)
      .. " after_load=" .. tostring(f.after_load)
      .. " sceneB=" .. tostring(f.sceneB)
      .. " crossFallback=" .. tostring(f.crossFallback)
      .. " savedByStory=" .. tostring(f.savedByStory)
      .. " macroUsed=" .. tostring(f.macroUsed))
if f.energy and f.energy >= 15 then print("EVAL_OK") end
if f.after_load == 1 then print("LOAD_MISS_OK") end
if f.macroUsed == 1 then print("MACRO_OK") end
if f.sceneB == 1 then print("XSCENE_OK") end

os.exit(result:sub(1, 4) == "DONE" and 0 or 1)
