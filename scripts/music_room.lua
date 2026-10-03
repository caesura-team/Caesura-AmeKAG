-- ===========================================================================
--  Caesura (AmeKAG) — music_room.lua  [P1.2]
--  Music Room: browse BGM tracks, preview playback, mark favorites.
--  Favorite state persisted in independent config (config/music_room.lua).
-- ===========================================================================

local MusicRoom = {}
local backend = require("backend")
local audio   = require("audio")
local fileutil = require("fileutil")
local Operation = require("kag.operation")

local trackCache = nil
local favorites = {}
local currentPreview = nil
local activeRoom = nil

-- [P1-2] Capture os.execute before potential sandboxing
local _os_execute = os.execute

-- Helper: safe solid texture
local function solid(r, g, b, a)
    return backend.create_solid_texture(math.floor(r), math.floor(g), math.floor(b), math.floor(a or 255))
end

-- Module-owned textures (cleaned on hide)
local ownedTextures = {}

-- ===========================================================================
-- MusicRoom.scan() -> { {id, path, name}, ... }
-- Cross-platform scan (W7).
-- ===========================================================================
function MusicRoom.scan()
    if trackCache then return trackCache end
    local candidate = {} -- publish only after complete enumeration and sorting
    local dirs = {"assets/bgm/", "assets/bgm", "data/bgm/", "data/bgm"}
    -- Lua patterns have no alternation operator. Filter extensions explicitly
    -- after the directory scan so valid tracks are not silently omitted.
    local pattern = "%.%w+$"
    for _, dir in ipairs(dirs) do
        local files = fileutil.scan_dir(dir, pattern)
        if #files > 0 then
            for _, fname in ipairs(files) do
                local extension = fname:lower():match("%.([^.]+)$")
                if extension == "ogg" or extension == "mp3" or extension == "wav" then
                local id = fname:match("^(.-)%.[^.]+$") or fname
                table.insert(candidate, {id = id, path = dir .. "/" .. fname, name = id})
                end
            end
            if #candidate > 0 then break end
        end
    end
    table.sort(candidate, function(a, b) return a.id < b.id end)
    trackCache = candidate
    return trackCache
end

-- ===========================================================================
-- MusicRoom.list(ctx) -> {id, name, unlocked, favorited}
-- ===========================================================================
function MusicRoom.list(ctx)
    local tracks = MusicRoom.scan()
    MusicRoom._loadFavorites()
    local unlocked = (ctx and ctx.unlockedMusic) or {}
    local result = {}
    for _, t in ipairs(tracks) do
        table.insert(result, {
            id = t.id,
            name = t.name,
            unlocked = unlocked[t.id] == true,
            favorited = favorites[t.id] == true,
            path = t.path,
        })
    end
    return result
end

-- ===========================================================================
-- MusicRoom.play(id) — preview a track (W5: fixed opts table)
-- ===========================================================================
function MusicRoom.play(id)
    MusicRoom.stop()
    local tracks = MusicRoom.scan()
    for _, t in ipairs(tracks) do
        if t.id == id then
            audio.play_bgm(t.path, { fadein = 0.5 })
            currentPreview = id
            print("[MusicRoom] Playing: " .. id)
            return true
        end
    end
    print("[MusicRoom] Track not found: " .. id)
    return false
end

-- ===========================================================================
-- MusicRoom.stop()
-- ===========================================================================
function MusicRoom.stop()
    if currentPreview then
        audio.stop_bgm(0.3)
        currentPreview = nil
    end
end

-- ===========================================================================
-- MusicRoom.favorite(id) — toggle favorite mark
-- ===========================================================================
function MusicRoom.favorite(id)
    favorites[id] = not favorites[id]
    MusicRoom._saveFavorites()
    print("[MusicRoom] " .. id .. " favorited: " .. tostring(favorites[id]))
    return favorites[id]
end

-- ===========================================================================
-- MusicRoom._loadFavorites()
-- ===========================================================================
function MusicRoom._loadFavorites()
    local ok, data = pcall(function()
        local f = io.open("config/music_room.lua", "r")
        if not f then return {} end
        local txt = f:read("*a")
        f:close()
        return load("return " .. txt)()
    end)
    if ok and type(data) == "table" then
        favorites = data
    end
end

-- ===========================================================================
-- MusicRoom._saveFavorites()
-- ===========================================================================
function MusicRoom._saveFavorites()
    pcall(function()
        -- Cross-platform mkdir (W4)
    local isWindows = (package.config:sub(1,1) == "\\")
    if isWindows then
        _os_execute('mkdir "config" 2>nul')
    else
        _os_execute('mkdir -p "config" 2>/dev/null')
    end
        local f = io.open("config/music_room.lua", "w")
        if not f then return end
        f:write("{\n")
        local first = true
        for k, v in pairs(favorites) do
            if not first then f:write(",\n") end
            f:write('  ["' .. k .. '"] = ' .. tostring(v))
            first = false
        end
        f:write("}\n")
        f:close()
    end)
end

-- ===========================================================================
-- MusicRoom._cleanupTextures() — destroy all module-owned textures
-- ===========================================================================
function MusicRoom._cleanupTextures()
    local textures = ownedTextures
    ownedTextures = {}
    for _, texId in pairs(textures) do
        if type(texId) == "number" and texId > 0 then
            pcall(function() backend.destroy_texture(texId) end)
        end
    end
end

local function cleanup(ctx, cancelled)
    local room = activeRoom
    if not room or room.ctx ~= ctx then return end
    activeRoom = nil
    if room.overlay then
        room.overlay.visible, room.overlay.texture = false, nil
    end
    MusicRoom._cleanupTextures()
    if cancelled and currentPreview then
        currentPreview = nil
        pcall(audio.stop_bgm, 0)
    end
    if ctx.input_focus == "music_room" then
        ctx.input_focus = room.previousFocus
        for _, key in ipairs({"UP", "DOWN", "ENTER", "F", "ESC"}) do
            _G["_GAME_KEY_" .. key] = false
        end
        pcall(backend.set_input_focus, room.previousBackendFocus)
    end
end

-- ===========================================================================
-- MusicRoom.show(ctx)
-- ===========================================================================
function MusicRoom.show(ctx)
    if activeRoom then return end
    local tracks = MusicRoom.list(ctx)
    if #tracks == 0 then
        backend.render_text("[Music Room] No tracks found.", 32, 100)
        return
    end

    local scope <close> = Operation.start(ctx)
    local previousFocus = ctx.input_focus or "kag"
    local ok, focus = pcall(backend.get_input_focus)
    local room = {
        ctx = ctx,
        previousFocus = previousFocus,
        previousBackendFocus = ok and type(focus) == "string" and focus
            or (previousFocus == "kag" and "KAG" or "GAME"),
    }
    activeRoom = room
    scope.token:register(function()
        if activeRoom == room then cleanup(ctx, true) end
    end)
    ctx.input_focus = "music_room"
    backend.set_input_focus("GAME")

    local w, h = backend.get_resolution()
    local vw, vh = require("viewport").wh()  -- viewported from 1280x720
    w = w or vw
    h = h or vh

    -- Dark overlay background layer (semi-transparent full-screen)
    local overlayTex = solid(0, 0, 0, 180)
    ownedTextures["_music_overlay"] = overlayTex
    pcall(function()
        local layers = require("layers")
        local overlay = layers.ensure(ctx, "_music_overlay", 95)
        room.overlay = overlay
        overlay.visible = true
        overlay.x, overlay.y = 0, 0
        overlay.w, overlay.h = w, h
        overlay.texture = overlayTex
    end)

    -- INPUT LOOP (audit: show() rendered once and returned -- no way to
    -- select/play/favorite; play()/favorite() had no callers). Same
    -- per-frame pattern as ChapterSelect: the scheduler coroutine
    -- drives coroutine.yield, and the engine routes _G._GAME_KEY_*.
    local cursor, scroll, ITEMS = 1, 0, 10
    local _printed = false
    while true do
    -- Title header bar background
    -- Title: "Music Room / 音楽室"
    backend.render_text("Music Room / 音楽室", 32, 18, 255, 200, 60, 255)

    -- Currently playing info
    if currentPreview then
        local npColor = {r=100, g=220, b=255}
        backend.render_text("▶ Now Playing: " .. currentPreview, 32, 44, npColor.r, npColor.g, npColor.b, 255)
    else
        backend.render_text("  No track playing", 32, 44, 140, 140, 160, 255)
    end

    -- Separator line
    backend.render_text(string.rep("-", 80), 32, 58, 80, 80, 120, 220)

    -- Track list with alternating backgrounds
    local startY = 70
    local lineH = 26
    local maxVisible = math.min(#tracks, 22)  -- Fit ~22 tracks on screen
    for i = 1, maxVisible do
        -- scroll is APPLIED here (review warn: the cursor moved past
        -- the visible window with >22 tracks -- marker vanished)
        local t = tracks[scroll + i]
        if not t then break end
        local y = startY + (i - 1) * lineH

        -- Build status indicators
        local favMark = t.favorited and "★ " or "  "
        local lockStatus = t.unlocked and "" or " [Locked]"

        -- Track line: "> ★ 03. track_name [Locked]"
        local prefix = (scroll + i == cursor) and "> " or "  "
        local idxStr = string.format("%2d.", i)
        local line = prefix .. favMark .. idxStr .. " " .. t.name .. lockStatus
        local r, g2, bVal = 220, 220, 255  -- default white-blue
        if scroll + i == cursor then
            r, g2, bVal = 255, 220, 80    -- cursor gold
        elseif not t.unlocked then
            r, g2, bVal = 100, 100, 120  -- dim for locked
        elseif currentPreview == t.id then
            r, g2, bVal = 100, 255, 255  -- cyan highlight for playing
        elseif t.favorited then
            r, g2, bVal = 255, 220, 80   -- gold for favorited
        elseif i % 2 == 0 then
            r, g2, bVal = 160, 160, 200  -- alternating row: slightly dimmer
        end
        backend.render_text(line, 40, y, r, g2, bVal, 255)
    end

    -- Footer: navigation hints
    local footerY = startY + maxVisible * lineH + 20
    backend.render_text(string.rep("-", 80), 32, footerY, 80, 80, 120, 220)
    backend.render_text("Up/Down: Navigate | Enter: Play/Stop | F: Favorite | ESC: Close", 32, footerY + 14, 160, 160, 200, 255)
    backend.render_text(tracks.favoritedCount or "Total: " .. #tracks .. " tracks", w - 220, footerY + 14, 160, 160, 200, 255)

    -- Favorite summary
    local favCount = 0
    for _, t in ipairs(tracks) do if t.favorited then favCount = favCount + 1 end end
    backend.render_text("★ " .. favCount .. " favorites", 40, footerY + 14, 255, 220, 80, 255)

    if not _printed then
        print("[MusicRoom] Displayed " .. #tracks .. " tracks.")
        _printed = true
    end
        coroutine.yield()
        if scope.token.cancelled or activeRoom ~= room then return end

        if _G._GAME_KEY_UP == true then
            _G._GAME_KEY_UP = false
            cursor = math.max(1, cursor - 1)
        elseif _G._GAME_KEY_DOWN == true then
            _G._GAME_KEY_DOWN = false
            cursor = math.min(#tracks, cursor + 1)
        elseif _G._GAME_KEY_ENTER == true then
            _G._GAME_KEY_ENTER = false
            local t = tracks[cursor]
            if t and not t.unlocked then
                print("[MusicRoom] Track locked: " .. t.name)
            elseif t then
                MusicRoom.play(t.id)
            end
        elseif _G._GAME_KEY_F == true then
            _G._GAME_KEY_F = false
            local t = tracks[cursor]
            if t then MusicRoom.favorite(t.id) end
        elseif _G._GAME_KEY_ESC == true then
            _G._GAME_KEY_ESC = false
            break
        end
        if cursor <= scroll then scroll = cursor - 1
        elseif cursor >= scroll + ITEMS then scroll = cursor - ITEMS + 1 end
        scroll = math.max(0, math.min(#tracks - ITEMS, scroll))
    end
    MusicRoom.hide(ctx)
    scope:complete()
end

-- ===========================================================================
-- MusicRoom.hide(ctx) — cleanup and restore input focus
-- ===========================================================================
function MusicRoom.hide(ctx)
    cleanup(ctx, false)
    print("[MusicRoom] Closed.")
end

return MusicRoom
