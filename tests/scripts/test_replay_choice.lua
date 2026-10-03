-- U21: real replay, tokenizer/compiler, runner and choice callbacks.
-- Registered in the isolated Lua suite. No fake handler,
-- backend playing predicate, scene loader or prefilled choice result is used.
package.path = "scripts/?.lua;scripts/?/init.lua;" .. package.path
local runner = require("kag_runner")
local replay = require("replay")
local passed, failed = 0, 0
local created = {}
local function cleanup()
    replay.set_mode("off")
    runner.stop()
    for _, path in ipairs(created) do os.remove(path) end
    created = {}
end
local scope <close> = setmetatable({}, {__close = cleanup})
local function check(name, value)
    print((value and "PASS " or "FAIL ") .. name)
    if value then passed = passed + 1 else failed = failed + 1 end
end
local function scene(name, content)
    local path = "tests/scripts/_u21_replay_" .. name .. ".ks"
    local old = io.open(path, "rb")
    if old then old:close(); error("Refusing to replace existing fixture " .. path) end
    local file = assert(io.open(path, "wb"))
    assert(file:write(content)); assert(file:close())
    created[#created + 1] = path
    return path
end
local menu = scene("menu", [[
[select]
[sel target="*a" text="Route A"]
[sel target="*b" text="Route B"]
[endselect]
[end]
*a
[set var="f.route" value="A"]
[jump *page]
*b
[set var="f.route" value="B"]
*page
[set var="f.at_page" value=1]
[p]
[set var="f.after_page" value=1]
[p]
[set var="f.beyond_second_page" value=1]
[end]
]])
local pages = scene("pages", [[
[p]
[set var="f.after_page" value=1]
[p]
[set var="f.beyond_second_page" value=1]
[end]
]])
local successor = scene("successor", [[
[p]
[set var="f.leaked" value=1]
[end]
]])
local function frames(count)
    for _ = 1, count do runner.update(0.016) end
end
local function begin(path)
    replay.set_mode("off")
    assert(runner.stop())
    -- This is the actual default page handler installed by the native game
    -- boot shim; the real choice command temporarily replaces it itself.
    _G._KAG_onClick = runner.on_click
    assert(runner.start(path))
    frames(16)
    return assert(runner.get_ctx())
end
local function schedule(x, y, count)
    replay.set_mode("record")
    for _ = 1, count or 1 do replay.record("click", x, y) end
    replay.set_mode("playback")
end
local function hit(choice)
    return 100, choice.y + choice.h / 2
end

for index, expected in ipairs({"A", "B"}) do
    -- Positive control follows Engine.cpp's real click dispatch: use the
    -- choice command's installed callback, then the ordinary runner frame.
    local direct = begin(menu)
    assert(direct._choiceMode and #direct._choiceButtonsActive == 2)
    local x, y = hit(direct._choiceButtonsActive[index])
    _G._GAME_MOUSE_X, _G._GAME_MOUSE_Y = x, y
    assert(type(_G._KAG_onClick) == "function")
    _G._KAG_onClick()
    frames(16)
    check("direct click selects " .. expected, direct.f.route == expected and direct.f.fallthrough == nil)
    check("direct choice stops at its following page " .. expected, direct.f.at_page == 1
        and direct.waiting_input and direct.f.after_page == nil)

    local played = begin(menu)
    local replay_x, replay_y = hit(played._choiceButtonsActive[index])
    schedule(replay_x, replay_y)
    runner.update(0)
    frames(16)
    check("replay consumes one coordinate event " .. expected, replay.clicks_fired() == 1)
    check("replay selects " .. expected, played.f.route == expected and played.f.fallthrough == nil)
    check("replay choice stops at its following page " .. expected, played.f.at_page == 1
        and played.waiting_input and played.f.after_page == nil)
end

do
    local current = begin(pages)
    schedule(100, 100)
    runner.update(0)
    frames(4)
    check("ordinary replay invokes the current page callback once", current.f.after_page == 1)
    check("ordinary replay does not pass the second page", current.waiting_input
        and current.f.beyond_second_page == nil and replay.clicks_fired() == 1)
end

do
    local old = begin(pages)
    local calls = 0
    _G._KAG_onClick = function()
        calls = calls + 1
        assert(runner.stop())
    end
    schedule(100, 100)
    local ok = pcall(runner.update, 0)
    check("external click callback can retire its owner without a stale frame", ok and calls == 1
        and runner.get_ctx() == nil and old.f.after_page == nil)
end

do
    local old = begin(pages)
    local calls = 0
    _G._KAG_onClick = function()
        calls = calls + 1
        assert(runner.start(successor, {replace=true}))
        _G._KAG_onClick = runner.on_click
    end
    schedule(100, 100, 2)
    local ok = pcall(runner.update, 0)
    local current = runner.get_ctx()
    check("callback replacement installs one new owner", ok and calls == 1 and current ~= old
        and current and current.current_scene == successor)
    check("remaining old replay clicks cannot advance the new owner's page", ok and current ~= old
        and current and current.waiting_input and current.f.leaked == nil and old.f.after_page == nil)
end

do
    -- The actual save/load commands, runner, scheduler and replay run here.
    -- Only the native storage boundary is a value-copy store in this Lua VM.
    -- A replay click can queue restore without immediately replacing ctx/co;
    -- that pending transition must survive the remainder of the same frame.
    local previous_kag = rawget(_G, "KAG")
    local copy = require("kag.save_state").copy
    local slots = {}
    _G.KAG = {
        save_game = function(slot, state) slots[slot] = copy(state); return true end,
        load_game = function(slot) return copy(slots[slot]), {slot=slot} end,
    }
    local saved_scene = scene("saved_page", [[
[set var="f.route" value="A"]
[save slot=31]
[p]
[set var="f.reward" value=1]
[p]
[set var="f.beyond_saved_page" value=1]
[end]
]])
    local load_scene = scene("load_from_page", [[
[set var="f.route" value="B"]
[set var="f.future_only" value=777]
[p]
[load slot=31]
[set var="f.abandoned" value=1]
[p]
[end]
]])
    begin(saved_scene)
    check("real save command captured route A", slots[31] and slots[31].f.route == "A")
    for _, input in ipairs({"direct", "replay"}) do
        local abandoned = begin(load_scene)
        local old_co = abandoned.co
        check(input .. " starts from route B at a page", abandoned.waiting_input
            and abandoned.f.route == "B" and abandoned.f.future_only == 777)
        if input == "direct" then
            _G._KAG_onClick()
        else
            -- Both events are due in one frame. The second may not enter a
            -- scheduler that is awaiting transactional replacement.
            schedule(100, 100, 2)
            runner.update(0)
        end
        frames(16)
        local restored = runner.get_ctx()
        check(input .. " load commits the saved owner", restored and restored ~= abandoned
            and restored.f.route == "A" and restored.f.future_only == nil
            and restored.tf.load_result == "ok")
        check(input .. " load retires the old coroutine", coroutine.status(old_co) == "dead"
            and abandoned.f.abandoned == nil)
        check(input .. " old click does not advance the restored page", restored
            and restored.waiting_input and restored.f.reward == nil
            and restored.f.beyond_saved_page == nil)
        if restored and restored ~= abandoned then
            replay.set_mode("off")
            runner.on_click()
            frames(4)
            check(input .. " new click continues saved route to the next page", restored.f.reward == 1
                and restored.waiting_input and restored.f.beyond_saved_page == nil)
        else
            check(input .. " new click continues saved route to the next page", false)
        end
    end
    slots[31] = nil
    local rejected = begin(load_scene)
    local rejected_co = rejected.co
    schedule(100, 100)
    runner.update(0)
    frames(4)
    check("rejected replay load retains the live owner", runner.get_ctx() == rejected
        and rejected.co == rejected_co and coroutine.status(rejected_co) == "suspended")
    check("rejected replay load continues the current route", rejected.f.route == "B"
        and rejected.f.future_only == 777 and rejected.f.abandoned == 1
        and rejected.waiting_input and rejected.tf.load_result == "error")
    _G.KAG = previous_kag
end

cleanup()
print(string.format("U21_REPLAY_CHOICE_RESULT: %d passed, %d failed", passed, failed))
if failed > 0 then os.exit(1) end
