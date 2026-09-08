-- test_scene_preload.lua — [preload type="scene"] parses the target now
-- so a later [jump] hits flow.scene_cache (no parse stall mid-transition).
package.path = "scripts/?.lua;scripts/kag/?.lua;" .. package.path
local results = {}
local function check(name, cond, detail)
    if cond then print("PASS " .. name) else print("FAIL " .. name .. " -- " .. tostring(detail)) end
    results[#results + 1] = cond
end

local flow = require("flow")
local resource = require("kag.commands.resource")
-- The normal main suite is already locked here. Keep the standalone entry on
-- that same production path, after loading the runtime modules it requires.
require("kag")
require("mods")
require("tokenizer")
local compiler = require("kag.compiler")
require("sandbox")
check("scene preload runs after sandbox lockdown", dofile == nil)

-- parse_file call counter: proves the cache is hit on the second load.
local real_parse_file = nil
local parse_calls = 0
do
    local tokenizer = require("tokenizer")
    real_parse_file = tokenizer.parse_file
    tokenizer.parse_file = function(path)
        parse_calls = parse_calls + 1
        return real_parse_file(path)
    end
end

-- ---- preload scene then jump: single parse -------------------------------
do
    local ctx = { f = {}, tf = {}, sf = {}, mp = {}, lf = {},
                  current_scene = "p.ks", token_index = 1 }
    parse_calls = 0
    flow.scene_cache["tests/scripts/smoke_test.ks"] = nil
    check("locked source hash is available",
        compiler.hashFile("tests/scripts/smoke_test.ks") ~= nil)
    local co = coroutine.create(function()
        resource.preload(ctx, { type = "scene", path = "tests/scripts/smoke_test.ks" })
    end)
    while coroutine.status(co) ~= "dead" do
        local ok, err = coroutine.resume(co)
        check("preload coroutine resumes", ok, err)
        if not ok then break end
    end
    check("preload scene parses once", parse_calls == 1, tostring(parse_calls))
    check("preload scene fills cache",
        flow.scene_cache["tests/scripts/smoke_test.ks"] ~= nil)

    -- A second load (as a [jump] would trigger) must hit the cache.
    local scene2 = flow.load_scene("tests/scripts/smoke_test.ks")
    check("jump after preload hits cache",
        scene2 ~= nil and parse_calls == 1, tostring(parse_calls))
end

-- ---- preload unknown scene: graceful failure ------------------------------
do
    local ctx = { f = {}, tf = {}, sf = {}, mp = {}, lf = {},
                  current_scene = "p.ks", token_index = 1 }
    local printed = {}
    local realPrint = print
    print = function(...) printed[#printed + 1] = table.concat({...}, " ") end
    local co = coroutine.create(function()
        resource.preload(ctx, { type = "scene", path = "assets/script/__nope__.ks" })
    end)
    while coroutine.status(co) ~= "dead" do coroutine.resume(co) end
    print = realPrint
    local failed = false
    for _, p in ipairs(printed) do
        if p:find("preload scene FAILED", 1, true) then failed = true end
    end
    check("preload missing scene reports failure",
        failed, table.concat(printed, "|"))
end

-- ---- restore parse_file ---------------------------------------------------
do
    local tokenizer = require("tokenizer")
    tokenizer.parse_file = real_parse_file
end

local failed = 0
for _, ok in ipairs(results) do if not ok then failed = failed + 1 end end
if failed > 0 then os.exit(1) end
print("SCENE PRELOAD TESTS DONE")
