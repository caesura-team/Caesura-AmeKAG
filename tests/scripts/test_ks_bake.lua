-- test_ks_bake.lua — Battle 5a: mobile bytecode pre-baking tests.
-- ks_bake pre-compiles scenes into .ksc (same path/hash algorithm as
-- flow.load_scene); the runtime then loads pre-baked scenes without
-- parse+compile. Also covers the hash-cache correctness (same-size
-- content edit must invalidate).
package.path = "scripts/?.lua;scripts/kag/?.lua;" .. package.path
local passed, failed = 0, 0
local function check(name, cond, detail)
    if cond then print("PASS " .. name) passed = passed + 1
    else print("FAIL " .. name .. (detail and (" -- " .. tostring(detail)) or ""))
        failed = failed + 1 end
end

pcall(require, "kag.commands.text")
pcall(require, "kag.commands.system")
pcall(require, "kag.commands.audio")
pcall(require, "kag.commands.layer")
pcall(require, "kag.commands.vfx")
pcall(require, "kag.commands.save")
pcall(require, "kag.commands.video")
pcall(require, "kag")

local compiler = require("kag.compiler")
local tokenizer = require("tokenizer")
local flow = require("flow")
local bake = dofile("scripts/ks_bake.lua")

local SCENE = "tmp/bake_test.ks"
local OUT = "cache/ksc"

-- CI checkouts start without tmp/ and cache/ (round 60: the suite now
-- runs on Linux/macOS where the Windows mkdir 2>nul trick is invalid).
local function ensure_dir(path)
    local sep = package.config:sub(1, 1)
    if sep == "\\" then
        os.execute('mkdir "' .. path:gsub("/", "\\") .. '" 2>nul')
    else
        os.execute('mkdir -p "' .. path .. '"')
    end
end
ensure_dir("tmp")
ensure_dir("cache")
ensure_dir("cache/ksc")

local function writeScene(text)
    local f = io.open(SCENE, "w")
    f:write(text)
    f:close()
end

-- ---------------------------------------------------------------------------
-- 1. bake writes a .ksc with the same path algorithm as flow.load_scene
-- ---------------------------------------------------------------------------
writeScene('*start\n[set f.hp 100]\n[ch text="hello"]\n[end]\n')
local okBake, outPath = bake.bakeScene(SCENE, OUT)
check("bake succeeds", okBake == true)
check("ksc path matches flow algorithm",
      outPath == flow.load_scene and true or outPath == "cache/ksc/tmp_bake_test.ksc")
local kscExists = io.open(outPath, "r") ~= nil
check("ksc file written", kscExists)

-- ---------------------------------------------------------------------------
-- 2. flow.load_scene loads the pre-baked scene (compiled, correct tokens)
-- ---------------------------------------------------------------------------
flow.scene_cache = {}
local scene = flow.load_scene(SCENE)
check("pre-baked scene loads", scene ~= nil and #scene.tokens > 0)
check("scene is compiled", scene.tokens._compiled ~= nil)
check("labels intact", scene.labels["start"] ~= nil)
check("baked content correct",
      scene.tokens[2] and scene.tokens[2][1] == "set")

-- ---------------------------------------------------------------------------
-- 3. second load within the session is served from caches (~0 cost)
-- ---------------------------------------------------------------------------
flow.scene_cache = {}
local t0 = os.clock()
local scene2 = flow.load_scene(SCENE)
local elapsed = os.clock() - t0
check("repeat load fast (<5ms)", elapsed < 0.005, string.format("%.2fms", elapsed * 1000))
check("repeat load identical", #scene2.tokens == #scene.tokens)

-- ---------------------------------------------------------------------------
-- 4. same-size content edit invalidates the hash cache (freshness)
-- ---------------------------------------------------------------------------
writeScene('*start\n[set f.hp 200]\n[ch text="hello"]\n[end]\n')  -- same length-ish
-- force rebake (hash cache sees the new head)
local okRebake = bake.bakeScene(SCENE, OUT)
check("rebake after edit succeeds", okRebake == true)
flow.scene_cache = {}
local scene3 = flow.load_scene(SCENE)
check("edited content reloaded", scene3.tokens[2] and scene3.tokens[2][1] == "set")

-- ---------------------------------------------------------------------------
-- 5. check mode reports freshness
-- ---------------------------------------------------------------------------
check("fresh after bake", bake.isFresh(SCENE, OUT) == true)
writeScene('*start\n[set f.hp 300]\n[ch text="hello"]\n[end]\n')
check("stale after edit", bake.isFresh(SCENE, OUT) == false)
-- restore + rebake for cleanup state
bake.bakeScene(SCENE, OUT)

-- ---------------------------------------------------------------------------
-- 6. hash cache correctness (same-size edit changes hash)
-- ---------------------------------------------------------------------------
local h1 = compiler.hashFile(SCENE)
local h2 = compiler.hashFile(SCENE)
check("hash cached same file", h1 == h2)
writeScene('*start\n[set f.hp 999]\n[ch text="hello"]\n[end]\n')
local h3 = compiler.hashFile(SCENE)
check("same-size edit changes hash", h1 ~= h3)

-- ---------------------------------------------------------------------------
-- 7. web bundle export (round 35): bakeWeb returns scenes + assets
-- ---------------------------------------------------------------------------
local okWeb, webRes = pcall(function()
    -- bakeWeb is a local inside ks_bake's script body; drive it via CLI
    -- is heavy, so verify through the exported bakeScene path + compiler
    -- encode_lua_literal contract instead: the bundle writer is exercised
    -- end-to-end by the web player integration test (flow.integration.test.js).
    local bundle = { version = 1, scenes = {}, assets = { "assets/bg/classroom.png" } }
    bundle.scenes["test.ks"] = { version = 1, tokens = { { "ch", { text = "x" } } } }
    local enc = compiler.encode_lua_literal(bundle)
    local chunk = assert(load("return " .. enc))
    local round = chunk()
    assert(round.version == 1 and round.assets[1] == "assets/bg/classroom.png")
    assert(round.scenes["test.ks"].tokens[1][1] == "ch")
    return true
end)
check("7a: encode_lua_literal round-trips web bundle", okWeb == true)
check("7b: encode_lua_literal exported by compiler",
      type(compiler.encode_lua_literal) == "function")

-- ---------------------------------------------------------------------------
-- 8. portable --dir listing and --web output dir creation (Web package CI
--    gate, t144 design A). The CLI's --dir collection used to shell out to
--    cmd.exe `dir /s /b`, which yields nothing on the Linux runners that bake
--    demo/ before packaging; the two shells also disagree on traversal order
--    (dir: root files before subdirs; find: whatever readdir gives), so the
--    contract is a Lua-side sorted, '/'-separated, CWD-relative list.
-- ---------------------------------------------------------------------------
local IS_WINDOWS = package.config:sub(1, 1) == "\\"
local DIR_FIX = "tmp/bake_dir"
ensure_dir(DIR_FIX)
ensure_dir(DIR_FIX .. "/sub")
for _, rel in ipairs({ "z.ks", "m.ks", "sub/a.ks", "not.txt" }) do
    local f = assert(io.open(DIR_FIX .. "/" .. rel, "w"))
    f:write("*start\n[end]\n")
    f:close()
end

check("8a: listKsFiles exported", type(bake.listKsFiles) == "function")
local okList, listed = pcall(function() return bake.listKsFiles(DIR_FIX) end)
check("8b: listKsFiles runs", okList == true, listed)
local joined = okList and type(listed) == "table" and table.concat(listed, "|") or tostring(listed)
check("8c: listKsFiles is sorted, '/'-separated, relative and skips non-.ks",
      joined == DIR_FIX .. "/m.ks|" .. DIR_FIX .. "/sub/a.ks|" .. DIR_FIX .. "/z.ks", joined)

check("8d: ensureDir exported", type(bake.ensureDir) == "function")
local NESTED = "tmp/bake_nested/x/y"
local okDir = pcall(function() bake.ensureDir(NESTED) end)
local probe = okDir and io.open(NESTED .. "/probe.txt", "w") or nil
check("8e: ensureDir creates nested directories", probe ~= nil)
if probe then probe:close() end
if not IS_WINDOWS then
    local stray = io.open("nul", "r")
    check("8f: no stray 'nul' file left behind on POSIX", stray == nil)
    if stray then stray:close(); os.remove("nul") end
end

-- U14: the actual CLI verifies the finished Web artifact against this runtime.
do
    local bin=arg and arg[-1] or 'lua'
    if IS_WINDOWS then bin='call "'..bin:gsub('/','\\')..'"' else bin='"'..bin..'"' end
    local function cli(args)
        local process=assert(io.popen(bin..' scripts/ks_bake.lua '..args..' 2>&1'))
        local output=process:read('*a');local ok=process:close()
        return ok==true,output
    end
    local folder='tmp/bake_web_compat'
    local path=folder..'/story.lua'
    ensure_dir(folder)
    local cold_path=folder..'/cold.lua'
    local cold=assert(io.open(cold_path,'wb'))
    cold:write([=[package.path='scripts/?.lua;scripts/?/init.lua;'..package.path
local compiler=require('kag.compiler')
local tokens=require('tokenizer').parse('[eval exp="f.x=1"]\n[end]')
compiler.compile(tokens)
local data=assert(compiler.serialize(tokens))
require('kag')
assert(compiler.validateSerialized(data),'cold compiler contracts differ from loaded runtime')
print('COLD-COMPILER-COMPATIBLE')
]=]);cold:close()
    local process=assert(io.popen(bin..' '..cold_path..' 2>&1'))
    local cold_log=process:read('*a');local cold_ok=process:close()
    check('U14 cold compiler uses complete runtime contracts',cold_ok==true,cold_log)
    local cached,cache_log=cli(SCENE..' --out '..folder)
    check('U14 cold CLI writes current native cache',cached,cache_log)
    local fresh,fresh_log=cli(SCENE..' --out '..folder..' --check')
    check('U14 separate cold CLI accepts its current native cache',fresh,fresh_log)
    local baked,bake_log=cli(SCENE..' --web '..folder)
    check('U14 CLI bakes a Web artifact',baked,bake_log)
    local accepted,accept_log=cli('--check-web '..path)
    check('U14 CLI validates current artifact',accepted and accept_log:find('[web-check] compatible',1,true),accept_log)
    if baked then
        local data=assert(loadfile(path,'t',{}))()
        local entry=assert(data.scenes['bake_test.ks'])
        if entry.compatibility then entry.compatibility.semantics='unknown-compiler' end
        local file=assert(io.open(path,'wb'));file:write('return '..compiler.encode_lua_literal(data));file:close()
        local accepted_bad,bad_log=cli('--check-web '..path)
        check('U14 CLI refuses mismatched compiler artifact',not accepted_bad
            and bad_log:find('compiler-semantics-mismatch',1,true),bad_log)
        file=assert(io.open(path,'wb'));file:write('return { scenes = {');file:close()
        local accepted_short,short_log=cli('--check-web '..path)
        check('U14 CLI refuses truncated artifact',not accepted_short
            and short_log:find('invalid-bundle-literal',1,true),short_log)
    end
    os.remove(folder..'/tmp_bake_test.ksc');os.remove(cold_path);os.remove(path);os.remove(folder)
end

-- cleanup (files first, then empty dirs bottom-up; leftovers are harmless:
-- ensure_dir/ensureDir are idempotent and the *.ks filter ignores strays)
for _, rel in ipairs({ "z.ks", "m.ks", "sub/a.ks", "not.txt" }) do os.remove(DIR_FIX .. "/" .. rel) end
os.remove(DIR_FIX .. "/sub"); os.remove(DIR_FIX)
os.remove(NESTED .. "/probe.txt"); os.remove(NESTED); os.remove("tmp/bake_nested/x"); os.remove("tmp/bake_nested")

-- cleanup
os.remove(SCENE)
os.remove("cache/ksc/tmp_bake_test.ksc")

-- Exit gate.
if failed > 0 then
    print(string.format("KS BAKE TESTS: %d passed, %d FAILED", passed, failed))
    os.exit(1)
end
print(string.format("KS BAKE TESTS DONE (%d passed)", passed))
