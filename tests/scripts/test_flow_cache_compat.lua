-- U14: real file -> tokenizer/compiler -> flow -> runner cache contracts.
-- Run in isolation: this suite supplies native host bindings and redirects only
-- its own source/mod/cache file IO into artifacts/validation/u14-flow/.
package.path = "scripts/?.lua;scripts/?/init.lua;" .. package.path

local ROOT = "artifacts/validation/u14-flow/"
local SCENE = ROOT .. "source.ks"
local MOD = "u14_flow_cache"
local MOD_SCENE = "mods/" .. MOD .. "/" .. SCENE
local MOD_FILE = ROOT .. "mod-source.ks"
local function mkdir(path)
    if package.config:sub(1, 1) == "\\" then
        os.execute('if not exist "' .. path .. '" mkdir "' .. path .. '"')
    else
        os.execute("mkdir -p '" .. path .. "'")
    end
end
mkdir(ROOT .. "cache")

local real_open, real_execute = io.open, os.execute
local deny_cache_write, deny_cache_read = false, false
local io_proof = { cache_reads = 0, cache_writes = 0, denied = 0 }
local function cache_file(path)
    if type(path) == "string" and path:match("^cache/ksc/.*u14%-flow") then
        return ROOT .. "cache/" .. path:match("[^/]+$")
    end
end
io.open = function(path, mode)
    local redirected = cache_file(path)
    if redirected then
        local writing = mode and mode:find("[wa+]") ~= nil
        if writing then
            io_proof.cache_writes = io_proof.cache_writes + 1
            if deny_cache_write then
                io_proof.denied = io_proof.denied + 1
                return nil, "permission denied: U14 read-only cache fixture"
            end
        else
            io_proof.cache_reads = io_proof.cache_reads + 1
            if deny_cache_read then
                error("permission denied: U14 unreadable cache fixture")
            end
        end
        path = redirected
    elseif path == MOD_SCENE then
        path = MOD_FILE
    end
    return real_open(path, mode)
end
os.execute = function(command)
    -- compiler.writeCache's directory request belongs to the redirected cache.
    if command:find("cache/ksc", 1, true) then return true end
    return real_execute(command)
end

local function callable(fields)
    return setmetatable(fields or {}, { __index = function(self, key)
        local fn = function() return true end
        rawset(self, key, fn)
        return fn
    end })
end
_G.KAG = callable({ is_voice_playing = function() return false end,
    is_bgm_playing = function() return false end,
    get_active_voices = function() return 0 end })
_G.Render, _G.Engine, _G.DevCore = callable(), callable(), callable()
Render.create_viewport = function() return 0 end
Render.create_solid_texture = function() return 0 end

local runner = require("kag_runner")
local flow = require("flow")
local compiler = require("kag.compiler")
local tokenizer = require("tokenizer")
local schema = require("kag.schema")
local mods = require("mods")
require("kag") -- Load command contracts before taking a cache signature.
mods.register(MOD, 100)

-- Forwarding probes observe production calls; they never supply tokens or
-- replace tokenizer/compiler behavior.
local parse_calls, compile_calls = 0, 0
local real_parse, real_compile = tokenizer.parse_file, compiler.compile
tokenizer.parse_file = function(...)
    parse_calls = parse_calls + 1
    return real_parse(...)
end
compiler.compile = function(...)
    compile_calls = compile_calls + 1
    return real_compile(...)
end

local function write(path, content)
    local f = assert(real_open(path, "wb"))
    assert(f:write(content))
    assert(f:close())
end
local function ksc_path(path)
    return "cache/ksc/" .. path:gsub("[/\\]+", "_"):gsub("%.ks$", ".ksc")
end
local function reset(source)
    runner.stop()
    flow.clear_cache()
    mods.disable(MOD)
    deny_cache_write, deny_cache_read = false, false
    os.remove(assert(cache_file(ksc_path(SCENE))))
    os.remove(assert(cache_file(ksc_path(MOD_SCENE))))
    os.remove(MOD_FILE)
    write(SCENE, source)
    parse_calls, compile_calls = 0, 0
    io_proof.cache_reads, io_proof.cache_writes, io_proof.denied = 0, 0, 0
end
local function execute(path)
    assert(runner.start(path or SCENE, { replace = true }))
    for _ = 1, 500 do
        local ctx = assert(runner.get_ctx())
        assert(not ctx.tf.load_error, tostring(ctx.tf.load_error))
        if not ctx._session_active then return ctx end
        local ok, reason = runner.update(0.016)
        assert(ok or reason == "ended" or reason == "dead", tostring(reason))
    end
    error("runner did not finish within 500 deterministic frames")
end
local function command_count(tokens, command)
    local n = 0
    for _, token in ipairs(tokens) do if token[1] == command then n = n + 1 end end
    return n
end

local passed, failed = 0, 0
local function case(name, fn)
    local ok, reason = pcall(fn)
    if ok then
        passed = passed + 1
        print("PASS " .. name)
    else
        failed = failed + 1
        print("FAIL " .. name .. " -- " .. tostring(reason))
    end
    deny_cache_write, deny_cache_read = false, false
end

case("same-length source edits invalidate the live scene entry", function()
    local old = '[eval exp="f.value = 11"]\n[end]\n'
    local new = '[eval exp="f.value = 22"]\n[end]\n'
    assert(#old == #new)
    reset(old)
    assert(execute().f.value == 11)
    local first = flow.scene_cache[SCENE]
    local parsed = parse_calls
    write(SCENE, new)
    assert(execute().f.value == 22, "runner executed the previous source")
    assert(parse_calls == parsed + 1, "edited source was not reparsed")
    assert(flow.scene_cache[SCENE] ~= first, "live scene entry was returned again")
end)

case("schema default mutation invalidates memory and disk caches", function()
    reset("[pt]\n[end]\n")
    local speed = assert(schema.specs("pt")).speed
    local previous = speed.default
    local restore <close> = setmetatable({}, { __close = function() speed.default = previous end })
    assert(execute().text_speed == previous)
    local parsed = parse_calls
    speed.default = previous + 7
    assert(execute().text_speed == previous + 7)
    assert(parse_calls == parsed + 1, "schema mutation reused the in-memory compiled scene")
    flow.clear_cache()
    parsed = parse_calls
    speed.default = previous + 9
    assert(execute().text_speed == previous + 9)
    assert(parse_calls == parsed + 1, "schema mutation reused the on-disk compiled scene")
end)

case("enabling a same-name mod uses the currently resolved scene", function()
    reset('[eval exp="f.value = 31"]\n[end]\n')
    write(MOD_FILE, '[eval exp="f.value = 42"]\n[end]\n')
    assert(execute().f.value == 31)
    mods.enable(MOD)
    assert(execute().f.value == 42, "enabled mod did not override the cached base scene")
    local scene = assert(flow.scene_cache[MOD_SCENE])
    assert(scene.path == MOD_SCENE and scene.base_path == SCENE)
    mods.disable(MOD)
    assert(execute().f.value == 31, "disabling mod did not recover the current base scene")
end)

case("cache write denial still executes source and supports reuse", function()
    reset('[eval exp="f.value = 53"]\n[end]\n')
    deny_cache_write = true
    assert(execute().f.value == 53)
    assert(io_proof.denied > 0, "read-only cache boundary was not reached")
    local parsed = parse_calls
    assert(execute().f.value == 53)
    assert(parse_calls == parsed, "read-only cache discarded the valid in-memory template")
end)

case("cache read exception degrades to real source parsing", function()
    reset('[eval exp="f.value = 64"]\n[end]\n')
    deny_cache_read = true
    assert(execute().f.value == 64, "cache exception escaped source fallback")
    assert(parse_calls > 0 and io_proof.cache_reads > 0)
end)

case("warm memory and persisted disk hits avoid parsing and compilation", function()
    reset('[eval exp="f.value = 75"]\n[end]\n')
    local first = assert(flow.load_scene(SCENE))
    local parsed, compiled = parse_calls, compile_calls
    assert(parsed == 1 and compiled > 0 and io_proof.cache_writes > 0)
    local second = assert(flow.load_scene(SCENE))
    assert(parse_calls == parsed and compile_calls == compiled, "memory hit reran front-end")
    assert(second ~= first and second.tokens ~= first.tokens, "memory hit shares mutable scene state")
    local cache_reads = io_proof.cache_reads
    flow.clear_cache()
    local third = assert(flow.load_scene(SCENE))
    assert(io_proof.cache_reads > cache_reads, "disk hit did not read the cache file")
    assert(parse_calls == parsed and compile_calls == compiled, "disk hit reran front-end")
    assert(third.tokens ~= second.tokens and third.tokens[1][2] ~= second.tokens[1][2])
    assert(execute().f.value == 75)
    print(string.format("PROOF warm paths: parses=%d compiles=%d disk_reads=%d writes=%d",
        parsed, compiled, io_proof.cache_reads, io_proof.cache_writes))
end)

case("runtime macro rewrites cannot contaminate later scene loads", function()
    reset('[eval exp="f.count = 0"]\n'
        .. '[macro name="bump"]\n[eval exp="f.count = f.count + 1"]\n[endmacro]\n[bump]\n'
        .. '[macro name="bump"]\n[eval exp="f.count = f.count + 2"]\n[endmacro]\n[bump]\n[end]\n')
    local before = assert(flow.load_scene(SCENE))
    assert(command_count(before.tokens, "bump") == 2, "fixture was statically inlined")
    local first_run = execute()
    assert(first_run.f.count == 3 and first_run.tokens._runtime_rewritten,
        "fixture did not execute the runtime macro splice path")
    local next_scene = assert(flow.load_scene(SCENE))
    assert(next_scene.tokens ~= first_run.tokens, "later load shares the rewritten stream")
    assert(command_count(next_scene.tokens, "bump") == 2, "later load contains earlier macro expansion")
    assert(command_count(before.tokens, "bump") == 2, "another preloaded scene was modified by execution")
    assert(not next_scene.tokens._runtime_rewritten)
    assert(execute().f.count == 3)
end)

case("restore preparation leaves the active cache and tokens untouched", function()
    reset('[eval exp="f.value = 86"]\n[end]\n')
    local live = assert(flow.load_scene(SCENE))
    local writes = io_proof.cache_writes
    local prepared = assert(flow.prepare_scene(SCENE))
    assert(flow.scene_cache[SCENE] == live and prepared ~= live)
    assert(prepared.tokens ~= live.tokens and prepared.tokens[1][2] ~= live.tokens[1][2])
    assert(io_proof.cache_writes == writes, "restore preparation persisted a cache entry")
end)

case("explicit public cache replacement invalidates private reuse", function()
    reset('[eval exp="f.value = 97"]\n[end]\n')
    local live = assert(flow.load_scene(SCENE))
    local reads = io_proof.cache_reads
    flow.scene_cache = {}
    local next_scene = assert(flow.load_scene(SCENE))
    assert(next_scene ~= live and io_proof.cache_reads > reads,
        "public cache replacement kept the private template active")
    assert(execute().f.value == 97)
end)

case("reload invalidates the currently resolved mod entry", function()
    reset('[eval exp="f.value = 18"]\n[end]\n')
    write(MOD_FILE, '[eval exp="f.value = 29"]\n[end]\n')
    mods.enable(MOD)
    assert(execute().f.value == 29)
    local reads = io_proof.cache_reads
    local reloaded = assert(flow.reload_scene(SCENE))
    assert(reloaded.path == MOD_SCENE and io_proof.cache_reads > reads,
        "reload invalidated only the base path and reused the resolved mod entry")
end)

runner.stop()
mods.disable(MOD)
flow.clear_cache()
tokenizer.parse_file, compiler.compile = real_parse, real_compile
io.open, os.execute = real_open, real_execute
print(string.format("FLOW CACHE COMPAT TESTS: %d passed, %d failed", passed, failed))
if failed > 0 then os.exit(1) end
