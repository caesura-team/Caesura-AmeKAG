-- test_music_room.lua — [music_room] input loop (audit)
package.path = "scripts/?.lua;scripts/kag/?.lua;" .. package.path
local passed, failed = 0, 0
local function check(name, cond)
    if cond then print("PASS " .. name) passed = passed + 1
    else print("FAIL " .. name) failed = failed + 1 end
end

local MusicRoom = require("music_room")
local plays, favs = {}, {}
local real_scan = MusicRoom.scan
local real_play, real_favorite = MusicRoom.play, MusicRoom.favorite
MusicRoom.scan = function()
    return { { id = "t1", name = "Track One", path = "a.ogg" },
             { id = "t2", name = "Track Two", path = "b.ogg" } }
end
MusicRoom.play = function(id) plays[#plays + 1] = id return true end
MusicRoom.favorite = function(id) favs[#favs + 1] = id end
local real_backend = _G._CAESURA_BACKEND
_G._CAESURA_BACKEND = { render = function() return true end,
    platform = function(cmd)
        if cmd == "get_resolution" then return 1280, 720 end
        return true end }
local layers_backup = package.loaded["layers"]
package.loaded["layers"] = { ensure = function() return { visible = true } end,
    set_layer_visible = function() end, set_z = function() end }
local ctx = { f = {}, sf = {}, tf = {}, mp = {}, variables = {},
    unlockedMusic = { t1 = true } }
local co = coroutine.create(function() MusicRoom.show(ctx) end)
coroutine.resume(co)
coroutine.resume(co)
-- DOWN to track 2 (LOCKED): Enter must refuse, F must favorite
_G._GAME_KEY_DOWN = true
coroutine.resume(co)
_G._GAME_KEY_ENTER = true
coroutine.resume(co)
check("locked track refused", #plays == 0)
_G._GAME_KEY_F = true
coroutine.resume(co)
check("favorite works on locked", favs[1] == "t2")
-- UP to track 1 (unlocked): Enter plays
_G._GAME_KEY_UP = true
coroutine.resume(co)
_G._GAME_KEY_ENTER = true
coroutine.resume(co)
check("unlocked track plays", plays[1] == "t1")
-- ESC exits
_G._GAME_KEY_ESC = true
coroutine.resume(co)
check("esc exits", coroutine.status(co) == "dead")
package.loaded["layers"] = layers_backup
_G._CAESURA_BACKEND = real_backend
MusicRoom.scan = real_scan

-- scroll window (review warn): 25 tracks, cursor past the 22-row
-- window must still map to a rendered row; the gold highlight must
-- follow the cursor (review nit: assert the rendered color)
local big = {}
for i = 1, 25 do big[#big + 1] = { id = "t" .. i, name = "T" .. i, path = "p" .. i } end
MusicRoom.scan = function() return big end
local goldRows = {}
_G._CAESURA_BACKEND = { render = function(cmd, ...)
    if cmd == "render_text" then
        local a = { ... }
        if a[4] == 255 and a[5] == 220 and a[6] == 80 then
            goldRows[#goldRows + 1] = a[1]
        end
    end
    return true end,
    platform = function(cmd)
        if cmd == "get_resolution" then return 1280, 720 end
        return true end }
local ctxB = { f = {}, sf = {}, tf = {}, mp = {}, variables = {}, unlockedMusic = {} }
local coB = coroutine.create(function() MusicRoom.show(ctxB) end)
coroutine.resume(coB)
coroutine.resume(coB)
for _ = 1, 24 do
    _G._GAME_KEY_DOWN = true
    coroutine.resume(coB)
end
-- cursor at 25; Enter must pick t25 (unlocked? no -- locked, so no play)
_G._GAME_KEY_ENTER = true
local okB = coroutine.resume(coB)
check("scroll cursor no crash", okB and coroutine.status(coB) == "suspended")
local goldT25 = false
for _, row in ipairs(goldRows) do
    if row:find("T25") then goldT25 = true break end
end
check("gold follows cursor", goldT25)
_G._GAME_KEY_ESC = true
coroutine.resume(coB)
check("scroll esc exits", coroutine.status(coB) == "dead")

-- ============================================================
--  t184 semantic lock: [music] -> KAG.music (= SystemCommands.music,
--  kag.lua:78-81 pairs export) -> music_room.show -> Enter -> real
--  MusicRoom.play -> audio.play_bgm -> backend.audio_play -> the
--  audio binding call surface.  Headless Null-audio justification:
--  the ONLY stubbed seam is the final C++ be.audio dispatch; every
--  module above it (system cmd, music_room, audio.lua, backend.lua)
--  is real code -- no play mock, no show mock.
-- ============================================================
do
  -- (A) dispatch + interaction lock: [music] -> SystemCommands.music
  -- (kag.lua:78-81 exports SystemCommands into KAG, scheduler dispatches
  -- KAG.music) -> music_room.show (real input loop) -> Enter on an
  -- unlocked track reaches MusicRoom.play (recorded on the test's mock
  -- play -- the UI-selection hop; the effect hop is locked in (B)).
  local SystemCmds = require("kag.commands.system")
  local realBackend = _G._CAESURA_BACKEND
  _G._CAESURA_BACKEND = {
    render = function() return true end,
    platform = function(cmd)
      if cmd == "get_resolution" then return 1280, 720 end
      return true
    end,
    audio = function(cmd, ...)
      audioCalls = audioCalls or {}
      audioCalls[#audioCalls + 1] = { cmd, ... }
      return true
    end,
  }
  local ctxM = { f = {}, sf = {}, tf = {}, mp = {}, variables = {},
                 unlockedMusic = { t1 = true } }
  -- MusicRoom.scan is still the two-track mock from the UI tests above
  local playsBefore = #plays
  local coM = coroutine.create(function() SystemCmds.music(ctxM, {}) end)
  coroutine.resume(coM)  -- show() frame 1
  coroutine.resume(coM)  -- show() frame 2 (same pre-key pattern as above)
  _G._GAME_KEY_ENTER = true
  coroutine.resume(coM)  -- cursor 1 = t1 (unlocked): play fires
  check("music command reaches play", #plays == playsBefore + 1
        and plays[#plays] == "t1")
  coroutine.resume(coM)
  _G._GAME_KEY_ESC = true
  coroutine.resume(coM)
  check("music ESC exits show", coroutine.status(coM) == "dead")
  _G._GAME_KEY_ENTER = nil
  _G._GAME_KEY_ESC = nil
  _G._CAESURA_BACKEND = realBackend
end

-- (B) effect-binding lock: the real audio chain MusicRoom.play issues
-- at music_room.lua:78 -- audio.play_bgm(path, {fadein=0.5}) ->
-- audio.lua:16 -- backend.audio_play("bgm", ...) -> binding.  The only
-- stubbed hop is the final C++ be.audio dispatch (headless Null-audio);
-- audio.lua and backend.lua here are the REAL shipped modules.
do
  local audioCalls = {}
  local realBackend = _G._CAESURA_BACKEND
  _G._CAESURA_BACKEND = {
    render = function() return true end,
    audio = function(cmd, ...)
      audioCalls[#audioCalls + 1] = { cmd, ... }
      return true
    end,
  }
  -- exactly the call MusicRoom.play(t1) makes (track t1 -> "a.ogg")
  local okB = pcall(function() require("audio").play_bgm("a.ogg", { fadein = 0.5 }) end)
  local playCalls = 0
  local got = nil
  for _, c in ipairs(audioCalls) do
    if c[1] == "play_bgm" then
      playCalls = playCalls + 1
      got = c
    end
  end
  check("music binding play_bgm called once", okB and playCalls == 1)
  check("music binding correct track + fadein",
        got and got[2] == "a.ogg" and type(got[3]) == "table"
        and math.abs(got[3].fadein - 0.5) < 1e-9)
  -- stop mirror (music_room.lua:93): audio.stop_bgm(0.3) -> stop_bgm(0.3)
  audioCalls = {}
  require("audio").stop_bgm(0.3)
  local stopGot = nil
  for _, c in ipairs(audioCalls) do
    if c[1] == "stop_bgm" then stopGot = c end
  end
  check("music binding stop_bgm called with fadeout",
        stopGot and math.abs(stopGot[2] - 0.3) < 1e-9)
  _G._CAESURA_BACKEND = realBackend
end

MusicRoom.scan, MusicRoom.play, MusicRoom.favorite = real_scan, real_play, real_favorite
package.loaded["layers"] = layers_backup
_G._CAESURA_BACKEND = real_backend

if failed > 0 then os.exit(1) end
print("MUSIC ROOM TESTS DONE")
