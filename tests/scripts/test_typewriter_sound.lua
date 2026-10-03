-- test_typewriter_sound.lua — [typewriter sound] per-character SE wiring lock (t201)
-- Locks the CONSUMED path: kag_runner update() reveal advance fires
-- backend.audio_play('se', sound) when the reveal crosses >= interval NEW
-- characters since the LAST FIRE (boundary semantics: interval=3 fires on
-- chars 3, 6, ...). Skip mode / click-instant / empty-sound never fire; a
-- new line resets the boundary; the click-instant marks the SE boundary at
-- total so the elapsed-driven follow-through cannot fire a burst either.
-- Call form has NO volume parameter (v1 honesty: per-SE volume has no
-- consumption surface yet — t200 2.3). Harness mirrors test_textspeed.lua
-- Part 2 (fresh kag_runner module per launch; recorder wraps the required
-- backend module audio_play, the exact seam the wiring uses).
package.path = "scripts/?.lua;scripts/?/init.lua;scripts/kag/?.lua;scripts/kag/commands/?.lua;" .. package.path
local passed, failed = 0, 0
local function check(name, cond)
    if cond then print("PASS " .. name) passed = passed + 1
    else print("FAIL " .. name) failed = failed + 1 end
end

local seq = 0
local function launch(src)
    seq = seq + 1
    local path = "tests/scripts/_tw_s" .. seq .. ".ks"
    local f = assert(io.open(path, "w"))
    f:write(src)
    f:close()
    local krf = assert(io.open("scripts/kag_runner.lua", "r"))
    local krsrc = krf:read("*a")
    krf:close()
    local kr = assert(load(krsrc, "=kag_runner"))()
    local ok = kr.start(path)
    os.remove(path)
    return kr, kr.get_ctx(), require("kag.text_scene"), ok
end

-- The scheduler batches non-blocking tokens per resume; how many updates
-- a scene needs to reach [ch] depends on the token flow, so each case
-- pumps until ctx.reveal exists (0.001ms frames: negligible elapsed).
local function pump_until_reveal(kr, ctx, maxN)
    for _ = 1, (maxN or 8) do
        if ctx.reveal then return true end
        kr.update(0.001)
    end
    return ctx.reveal ~= nil
end

-- Observe the production backend module consumed by kag_runner. Restore its
-- original function after every block; no nonexistent global backend is added.
local function with_recorder(fn)
    local backend = require("backend")
    local calls = {}
    local realPlay = backend.audio_play
    backend.audio_play = function(bus, file, ...)
        calls[#calls + 1] = { bus, file, ... }
        if realPlay then return realPlay(bus, file, ...) end
        return true
    end
    local ok, err = xpcall(function() return fn(calls) end, function(e)
        return debug and debug.traceback and debug.traceback(e, 2) or tostring(e)
    end)
    backend.audio_play = realPlay
    return ok, err, calls
end

local function se_calls(calls, file)
    -- recorded at backend.audio_play(bus, file) -> { "se", file }
    local n = 0
    for _, c in ipairs(calls) do
        if c[1] == "se" and (file == nil or c[2] == file) then n = n + 1 end
    end
    return n
end

-- 1. interval=1: every new character -> one play_se with the right file
do
    local ok, err, calls = with_recorder(function(calls)
        local kr, ctx, ts, okS = launch(
            '[pt speed=16]\n[typewriter sound="assets/se/type.wav" interval=1]\n[ch name="A" text="ABCDEFGHIJ"]\n')
        check("start ok", okS == true)
        check("pump reaches reveal", pump_until_reveal(kr, ctx) == true)
        check("reveal total 10", ctx.reveal.total == 10)
        check("reveal starts empty (no SE)", se_calls(calls) == 0)
        kr.update(0.016); kr.update(0.016); kr.update(0.016)
        check("3x16ms reveals 3 chars", ts.get_state(ctx).reveal_chars == 3)
        check("interval=1 fires 3 SE", se_calls(calls) == 3)
        check("SE carries the configured file", se_calls(calls, "assets/se/type.wav") == 3)
        check("call form has NO volume param (v1 honesty)",
            (function()
                for _, c in ipairs(calls) do
                    if c[1] == "se" and #c ~= 2 then return false end
                end
                return true
            end)() == true)
    end)
    check("interval=1 block ran", ok == true)
    if not ok then print("   " .. tostring(err)) end
end

-- 2. interval=3: fires once per 3-char boundary (since the LAST FIRE, not
--    per frame — a 1-char-per-frame reveal must still hit every 3rd char)
do
    local ok, err, calls = with_recorder(function(calls)
        local kr, ctx, ts, okS = launch(
            '[pt speed=16]\n[typewriter sound="a.ogg" interval=3]\n[ch name="A" text="ABCDEFGHIJ"]\n')
        pump_until_reveal(kr, ctx)
        kr.update(0.016); kr.update(0.016); kr.update(0.016)
        check("interval=3: 3 chars -> 1 SE", se_calls(calls) == 1)
        check("interval=3: boundary at 3", ctx.reveal.last_shown == 3)
        kr.update(0.016); kr.update(0.016); kr.update(0.016)
        check("interval=3: 6 chars -> 2 SE total", se_calls(calls) == 2)
        check("interval=3: boundary at 6", ctx.reveal.last_shown == 6)
    end)
    check("interval=3 block ran", ok == true)
    if not ok then print("   " .. tostring(err)) end
end

-- 3. skip_mode: instant reveal never reaches the SE block
do
    local ok, err, calls = with_recorder(function(calls)
        local kr, ctx, ts, okS = launch(
            '[typewriter sound="a.ogg" interval=1]\n[ch name="A" text="ABCDEFGHIJ"]\n')
        pump_until_reveal(kr, ctx)
        ctx.skip_mode = true
        kr.update(0.5)
        check("skip instant: reveal total", ts.get_state(ctx).reveal_chars == 10)
        check("skip instant: 0 SE", se_calls(calls) == 0)
    end)
    check("skip block ran", ok == true)
    if not ok then print("   " .. tostring(err)) end
end

-- 4. click instant-reveal: on_click sets total outside the block -> 0 SE,
--    AND the elapsed-driven follow-through stays silent (boundary sealed
--    at total by the click). Speed=16 proves it is not a speed artifact.
do
    local ok, err, calls = with_recorder(function(calls)
        local kr, ctx, ts, okS = launch(
            '[pt speed=16]\n[typewriter sound="a.ogg" interval=1]\n[ch name="A" text="ABCDEFGHIJ"]\n')
        pump_until_reveal(kr, ctx)
        local r, why = kr.on_click()
        check("click reveals instantly", r == true and why == "revealed")
        check("click seals SE boundary", ctx.reveal.last_shown == ctx.reveal.total)
        kr.update(0.016); kr.update(0.016); kr.update(0.016)
        check("click instant + follow-through: 0 SE", se_calls(calls) == 0)
    end)
    check("click block ran", ok == true)
    if not ok then print("   " .. tostring(err)) end
end

-- 5. empty sound / action=off: configured nothing -> 0 SE
do
    local ok, err, calls = with_recorder(function(calls)
        local kr, ctx, ts, okS = launch(
            '[typewriter action=off]\n[ch name="A" text="ABCDEFGHIJ"]\n')
        pump_until_reveal(kr, ctx)
        check("action=off clears sound", ctx.typewriter_sound == "")
        kr.update(0.016); kr.update(0.016); kr.update(0.016)
        check("action=off: 0 SE", se_calls(calls) == 0)
    end)
    check("off block ran", ok == true)
    if not ok then print("   " .. tostring(err)) end
end

-- 6. new-line reset: line 1 boundary is 3 after 3 chars; clicking to
--    advance starts [ch] #2 with a FRESH reveal (last_shown = 0), so the
--    new line's first char fires its own SE.
do
    local ok, err, calls = with_recorder(function(calls)
        local kr, ctx, ts, okS = launch(
            '[pt speed=16]\n[typewriter sound="a.ogg" interval=1]\n[ch name="A" text="ABC"]\n[ch name="B" text="XY"]\n')
        pump_until_reveal(kr, ctx)
        kr.update(0.016); kr.update(0.016); kr.update(0.016)
        check("line1: 3 chars -> 3 SE", se_calls(calls) == 3)
        check("line1: boundary at 3", ctx.reveal.last_shown == 3)
        local r, _ = kr.on_click()
        check("advance click ok", r == true)
        check("new [ch] reveal created", ctx.reveal.total == 2)
        check("new line reset last_shown", ctx.reveal.last_shown == 0)
        kr.update(0.016)
        check("second line first char -> +1 SE (total 4)", se_calls(calls) == 4)
        kr.update(0.016)
        check("second line 2 chars -> +2 SE (total 5)", se_calls(calls) == 5)
    end)
    check("reset block ran", ok == true)
    if not ok then print("   " .. tostring(err)) end
end

-- 7. alias + schema shape (typewriter_sound shares the handler)
do
    local TextCmds = require("kag.commands.text")
    check("typewriter_sound alias == typewriter", TextCmds.typewriter_sound == TextCmds.typewriter)
    local S = require("kag.schema")
    local c = S.coerce("typewriter_sound", { sound = "s.ogg", interval = "3" }, {})
    check("typewriter_sound schema coerce", c.sound == "s.ogg" and c.interval == 3)
end

-- 8. alias scene usage: [typewriter_sound] dispatches to the same handler
--    and fires SEs through the same reveal consume (Runtime evidence for
--    the typewriter_sound row in the closure matrix).
do
    local ok, err, calls = with_recorder(function(calls)
        local kr, ctx, ts, okS = launch(
            '[pt speed=16]\n[typewriter_sound sound="s.ogg" interval=1]\n[ch name="A" text="AB"]\n')
        pump_until_reveal(kr, ctx)
        kr.update(0.016); kr.update(0.016)
        check("[typewriter_sound] alias fires SE", se_calls(calls) == 2)
        check("[typewriter_sound] carries the configured file", se_calls(calls, "s.ogg") == 2)
        check("[typewriter_sound] sets the same ctx field", ctx.typewriter_sound == "s.ogg")
    end)
    check("[typewriter_sound] block ran", ok == true)
    if not ok then print("   " .. tostring(err)) end
end

if failed > 0 then os.exit(1) end
print("TYPEWRITER SOUND TESTS DONE (" .. passed .. " passed)")
