-- =============================================================================
--  Caesura (AmeKAG) — sample_game_headless.lua
--  Headless driver for the sample game (demo/example_game/story.ks.new).
--
--  Boots kag_runner against a .ks scene with mocked C++ bindings, then
--  auto-clicks through the whole script (reveal, [p] waits, [select]/
--  [endselect] choices) until the coroutine dies at [end].
--
--  This is the engine-side "run to DONE with zero errors" verification.
--  It does NOT assert story content -- only that the script completes.
--
--  Usage (from repo root):
--    external/lua/lua.exe tests/scripts/sample_game_headless.lua
--
--  Env knobs:
--    SAMPLE_STORY   .ks path to drive (default demo/example_game/story.ks.new)
--    SAMPLE_ENDING  optional label to jump to at boot (e.g. "ending_zero",
--                    "ending_companion", "ending_promise") -- reachability
--                    probe for a specific ending branch.
--    SAMPLE_FRAMES  frame budget (default 200000)
--    SAMPLE_BRANCH_ROUTE / SAMPLE_BRANCH_TEXT  optional paired Golden proof:
--                    require the final route and a fully revealed branch line.
--
--  Exit: 0 = reached [end] (DONE), 1 = frame limit / fatal, 2 = target label
--  not found.
-- =============================================================================

package.path = "scripts/?.lua;scripts/?/init.lua;scripts/kag/?.lua;scripts/kag/commands/?.lua;" .. package.path

-- ---- Mock C++ bindings ----
-- backend.lua resolves via _CAESURA_BACKEND -> _G.KAG -> kag module. A
-- metatable that auto-creates no-op callables keeps the story runnable.
-- CRITICAL: is_voice_playing/is_bgm_playing MUST return false, otherwise
-- [playvoice]/[playbgm] wait loops spin forever (they poll the mock).
local function callable(t)
    return setmetatable(t or {}, {
        __index = function(self, key)
            if type(key) ~= "string" then return nil end
            rawset(self, key, function(...) return true end)
            return self[key]
        end,
    })
end

_G.KAG = callable({
    is_voice_playing  = function() return false end,
    is_bgm_playing    = function() return false end,
    get_active_voices = function() return 0 end,
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

local STORY  = os.getenv("SAMPLE_STORY")  or "demo/example_game/story.ks.new"
local ENDING = os.getenv("SAMPLE_ENDING") -- optional *ending_xxx label
local FMAX   = tonumber(os.getenv("SAMPLE_FRAMES")) or 200000
local BRANCH_ROUTE = os.getenv("SAMPLE_BRANCH_ROUTE")
local BRANCH_TEXT = os.getenv("SAMPLE_BRANCH_TEXT")
local branch_seen = false
if (BRANCH_ROUTE ~= nil or BRANCH_TEXT ~= nil)
    and (not BRANCH_ROUTE or BRANCH_ROUTE == "" or not BRANCH_TEXT or BRANCH_TEXT == "") then
    print("FATAL_BRANCH: both route and text expectations are required")
    os.exit(1)
end

local started, err = kag_runner.start(STORY)
if not started then
    print("FATAL_START: " .. tostring(err))
    os.exit(1)
end

-- Optional ending reachability probe: stage a jump to the target ending
-- label so we run JUST that branch to [end] (all endings funnel into
-- *credits -> [end]).
if ENDING and ENDING ~= "" then
    local ctx    = _G._CAESURA_CTX
    local target = ENDING:sub(1, 1) == "*" and ENDING or ("*" .. ENDING)
    if not kag_runner.stage_label_jump(ctx, target) then
        print("ENDING_NOT_FOUND: " .. target)
        os.exit(2)
    end
    -- t67: _pendingJump is what makes the runner's dead-coroutine branch
    -- re-spawn the scheduler at a "*label" (mirror golden_vn_headless.lua).
    -- Plain stop_flag alone ends the runner with "Script ended" and the
    -- probe passed vacuously (DONE:2 clicks=0 -- branch never ran, t52 C).
    ctx._pendingJump = target
    ctx.stop_flag = true -- end the current coroutine; update() re-spawns
    print("ENDING_JUMP: " .. target)
end

-- Auto-click: reveal a line, advance past [p], and pick the first rendered
-- option when a [select]/[endbutton] choice block is active.
local function drive_click()
    local ctx = _G._CAESURA_CTX
    if BRANCH_ROUTE and ctx and ctx.f and ctx.f.route == BRANCH_ROUTE then
        local state = require("kag.text_scene").get_state(ctx)
        local fully_revealed = not ctx.reveal or (state.reveal_chars or 0) >= ctx.reveal.total
        if fully_revealed then
            local words = {}
            for _, draw in ipairs(state.draws or {}) do words[#words + 1] = draw.text or "" end
            if table.concat(words):find(BRANCH_TEXT, 1, true) then branch_seen = true end
        end
    end
    if ctx and ctx._choiceMode then
        _G._GAME_MOUSE_X = 100
        local cbs = ctx._choiceButtonsActive or ctx._choiceButtons
        local ch  = cbs and cbs[1]
        _G._GAME_MOUSE_Y = (ch and tonumber(ch.y) or 450)
                         + (ch and tonumber(ch.h) or 24) / 2
        local kc = _G._KAG_onClick
        if type(kc) == "function" then kc() end
        -- Belt-and-suspenders: force the first option if the hit-test missed
        if ctx._choiceMode and type(cbs) == "table" and cbs[1] then
            ctx._selectedChoice   = cbs[1]
            ctx._choiceMode       = false
            ctx._choiceButtonsActive = nil
            ctx.waiting_input     = false
        end
        return
    end
    kag_runner.on_click()
end

local frames, clicks = 0, 0
local result = nil
while frames < FMAX do
    frames = frames + 1
    local ok, reason = kag_runner.update(0.016)
    local ctx = _G._CAESURA_CTX
    if reason == "ended" then
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
    end
end
if not result then result = "FRAME_LIMIT" end

local ctx = _G._CAESURA_CTX
local branch_ok = not BRANCH_ROUTE or (ctx.f and ctx.f.route == BRANCH_ROUTE and branch_seen)
if BRANCH_ROUTE then
    print("BRANCH_PROGRESS route=" .. tostring(ctx.f and ctx.f.route) .. " text=" .. tostring(branch_seen))
end
print("RESULT " .. result
      .. " token=" .. tostring(ctx.token_index)
      .. " clicks=" .. clicks
      .. " scene=" .. tostring(ctx.current_scene))

os.exit(result:sub(1, 4) == "DONE" and branch_ok and 0 or 1)
