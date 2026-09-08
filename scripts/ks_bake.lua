-- =============================================================================
--  Caesura (AmeKAG) — ks_bake.lua
--  Mobile bytecode pre-baking (Battle 5a): pre-compile every .ks scene
--  into its .ksc cache file BEFORE shipping, so the runtime (especially
--  mobile) loads pre-compiled bytecode with zero parse/compile at launch.
--
--  Path + freshness algorithms are IDENTICAL to flow.load_scene:
--    cache_root/<resolved path with / and \ -> _>.ksc
--    _srcHash = FNV-1a of the source file
--  A baked scene is a cache hit on first load.
--
--  Usage:
--    lua scripts/ks_bake.lua <scene.ks> [more.ks ...]
--    lua scripts/ks_bake.lua --dir scripts --dir demo --out cache/ksc
--    lua scripts/ks_bake.lua --check   (verify existing .ksc is fresh)
--    lua scripts/ks_bake.lua --check-web cache/story/story.lua
--
--  Exit codes: 0 = all baked, 1 = errors.
-- =============================================================================

local BS = string.char(92)
local here = (arg and arg[0] and arg[0]:match("(.*[/" .. BS .. "])")) or "scripts/"
package.path = here .. "?.lua;" .. here .. "kag/?.lua;" .. package.path

local tokenizer = require("tokenizer")
local compiler = require("kag.compiler")

local OUT_ROOT = "cache/ksc"
local IS_WINDOWS = package.config:sub(1, 1) == BS

-- mkdir -p equivalent. cmd.exe mkdir is recursive but rejects '/' and needs
-- 2>nul to stay quiet when the dir exists; on POSIX that redirection would
-- create a literal file named "nul", so the two branches must not share text.
local function ensureDir(path)
    if IS_WINDOWS then
        os.execute('mkdir "' .. path:gsub("/", BS) .. '" 2>nul')
    else
        os.execute('mkdir -p "' .. path .. '"')
    end
end

-- Every *.ks under dir (recursive) as sorted, '/'-separated, CWD-relative
-- paths. The listing shells differ (cmd.exe `dir /s /b` lists a directory's
-- own files before descending; find follows readdir order), so the order
-- contract is enforced here rather than trusted from either shell.
local function listKsFiles(dir)
    local out = {}
    if IS_WINDOWS then
        local pf = io.popen('dir /s /b "' .. dir .. BS .. '*.ks" 2>nul')
        if pf then
            -- dir /b on absolute inputs yields full paths; strip the CWD
            -- prefix so io.open works even when the repo path is non-ASCII
            -- (Lua 5.4 io.open is byte-oriented on Windows).
            local cwd = io.popen("cd"):read("*a") or ""
            cwd = cwd:gsub("^%s*(.-)%s*$", "%1"):gsub(BS, "/")
            for raw_line in pf:lines() do
                local line = raw_line:gsub(BS, "/")
                if cwd ~= "" and line:sub(1, #cwd) == cwd then
                    line = line:sub(#cwd + 2)
                end
                out[#out + 1] = line
            end
            pf:close()
        end
    else
        local pf = io.popen('find "' .. dir .. '" -type f -name "*.ks"')
        if pf then
            for line in pf:lines() do out[#out + 1] = line end
            pf:close()
        end
    end
    table.sort(out)
    return out
end

-- Same sanitization as flow.load_scene: resolved path -> flat file name.
local function kscPathFor(resolved, outRoot)
    return (outRoot or OUT_ROOT) .. "/"
        .. resolved:gsub("[/\\]+", "_"):gsub("%.ks$", ".ksc")
end

--- bake one scene: parse + compile + write .ksc. Returns true/false, err.
local function bakeScene(path, outRoot)
    local f = io.open(path, "r")
    if not f then return false, "cannot open: " .. path end
    local src = f:read("*a")
    f:close()

    local ok, tokens = pcall(tokenizer.parse, src)
    if not ok or not tokens then
        return false, "tokenize failed: " .. path
    end
    compiler.compile(tokens)
    tokens._srcHash = compiler.hashFile(path)
    local outPath = kscPathFor(path, outRoot)
    if not compiler.writeCache(tokens, outPath) then
        return false, "write failed: " .. outPath
    end
    return true, outPath
end

--- Web export (round 35): bake every scene into a story.json bundle for
--  the web player: { version, scenes: { path = serialized }, assets: [...] }.
--  Assets are discovered from [bg]/[fg]/[playbgm]/[playse]/[playvoice]
--  storage/file params (best-effort; the player falls back to the .ksc
--  stream when an asset is missing).
local function bakeWeb(scenes, outDir)
    local bundle = { version = 1, scenes = {}, assets = {} }
    local seen = {}
    local assetCount = 0
    local function addAsset(p)
        if type(p) ~= "string" or #p == 0 or seen[p] then return end
        seen[p] = true
        assetCount = assetCount + 1
        bundle.assets[assetCount] = p
    end
    local expected,key_error=compiler.bundleSceneKeys(scenes)
    if not expected then return nil,key_error end
    for index, path in ipairs(scenes) do
        local f = io.open(path, "r")
        if not f then return nil, "cannot open: " .. path end
        local src = f:read("*a")
        f:close()
        local ok, tokens = pcall(tokenizer.parse, src)
        if not ok or not tokens then return nil, "tokenize failed: " .. path end
        compiler.compile(tokens)
        local serialized = compiler.serialize(tokens)
        if not serialized then return nil, "serialize failed: " .. path end
        local key = expected[index]
        bundle.scenes[key] = serialized
        -- asset discovery: scan token params for storage/file values
        for _, tok in ipairs(tokens) do
            local p2 = tok[2]
            if type(p2) == "table" then
                for _, k in ipairs({ "storage", "file" }) do
                    local v = p2[k]
                    if type(v) == "string" and v:find("%.%a%a%a?$") then
                        addAsset(v)
                    end
                end
            end
        end
    end
    local compatible,reason=compiler.validateBundle(bundle,expected)
    if not compatible then return nil,reason end
    return bundle, assetCount
end

local function checkWeb(path,expected)
    -- A separate CLI process must register the same built-in contracts before
    -- comparing them with the baked scene identities.
    local loaded,reason=pcall(require,'kag')
    if not loaded then return false,'runtime-contracts-unavailable:'..tostring(reason) end
    local chunk,err=loadfile(path,'t',{})
    if not chunk then return false,'invalid-bundle-literal:'..tostring(err) end
    local decoded,bundle=pcall(chunk)
    if not decoded then return false,'invalid-bundle-literal:'..tostring(bundle) end
    return compiler.validateBundle(bundle,expected)
end

--- check whether an existing .ksc matches the source (fresh).
local function isFresh(path, outRoot)
    local outPath = kscPathFor(path, outRoot)
    local cached = compiler.readCache(outPath)
    if not cached or not cached._compiled then return false end
    local h = compiler.hashFile(path)
    return h ~= nil and cached._compiled._srcHash == h
end

-- ---------------------------------------------------------------------------
-- CLI
-- ---------------------------------------------------------------------------
local is_script = arg and arg[0]
    and arg[0]:match("([^/\\]+)$") == "ks_bake.lua"

if is_script then
    local inputs, dirs, outRoot, checkOnly, webOut, webCheck = {}, {}, OUT_ROOT, false, nil, nil
    local i = 1
    while i <= #arg do
        local a = arg[i]
        if a == "--dir" then
            i = i + 1
            dirs[#dirs + 1] = arg[i]
        elseif a == "--out" then
            i = i + 1
            outRoot = arg[i]
        elseif a == "--web" then
            i = i + 1
            webOut = arg[i]
        elseif a == "--check" then
            checkOnly = true
        elseif a == '--check-web' then
            i=i+1
            webCheck=arg[i]
            if not webCheck then print('error: --check-web requires a bundle path');os.exit(1) end
        elseif a == "-h" or a == "--help" then
            print("Usage: lua scripts/ks_bake.lua <scene.ks> [more.ks ...]")
            print("       lua scripts/ks_bake.lua --dir <dir> [--dir <dir2>] [--out cache/ksc] [--check]")
            print("       lua scripts/ks_bake.lua --dir demo --web cache/story  (web player bundle)")
            print("       lua scripts/ks_bake.lua --check-web cache/story/story.lua")
            print("  bakes scenes into pre-compiled .ksc (mobile zero-parse launch)")
            print("  --out: cache root (default cache/ksc, same as flow.load_scene)")
            print("  --web <dir>: emit cache/story.lua bundle (scenes + assets) for the web player")
            print("  --check: verify existing .ksc freshness without rewriting")
            os.exit(0)
        else
            inputs[#inputs + 1] = a
        end
        i = i + 1
    end

    if webCheck then
        local compatible,reason=checkWeb(webCheck)
        print(compatible and '[web-check] compatible' or '[web-check] '..tostring(reason))
        os.exit(compatible and 0 or 1)
    end

    -- gather .ks from --dir args (recursive)
    local collected = {}
    local function add(p)
        if p:match("%.ks$") then collected[#collected + 1] = p end
    end
    for _, p in ipairs(inputs) do add(p) end
    for _, d in ipairs(dirs) do
        for _, p in ipairs(listKsFiles(d)) do add(p) end
    end

    if #collected == 0 then
        print("error: no .ks scenes given")
        os.exit(1)
    end

    -- Web bundle mode (round 35): one Lua-literal file the web player loads
    -- with zero parse/compile (scenes serialized via compiler.serialize).
    if webOut then
        local bundle, assetCount = bakeWeb(collected)
        if not bundle then
            print("[error] " .. tostring(assetCount))
            os.exit(1)
        end
        local encoded = compiler.encode_lua_literal(bundle)
        local outPath = webOut .. "/story.lua"
        ensureDir(webOut)
        local w = io.open(outPath, "w")
        if not w then
            print("[error] cannot write " .. outPath)
            os.exit(1)
        end
        w:write("return " .. encoded .. "\n")
        w:close()
        local expected=assert(compiler.bundleSceneKeys(collected))
        local compatible,reason=checkWeb(outPath,expected)
        if not compatible then
            print('[error] finished bundle failed runtime compatibility: '..tostring(reason))
            os.exit(1)
        end
        print(string.format("[web] %s: %d scenes, %d assets", outPath,
            #collected, assetCount or 0))
        os.exit(0)
    end

    local errors = 0
    local baked = 0
    local fresh = 0
    for _, path in ipairs(collected) do
        if checkOnly then
            if isFresh(path, outRoot) then
                fresh = fresh + 1
            else
                print("[stale] " .. path)
                errors = errors + 1
            end
        else
            local ok, res = bakeScene(path, outRoot)
            if ok then
                baked = baked + 1
                print("[baked] " .. res)
            else
                print("[error] " .. tostring(res))
                errors = errors + 1
            end
        end
    end
    print(string.format(
        "%s: %d baked, %d fresh, %d errors",
        checkOnly and "CHECK" or "BAKE", baked, fresh, errors))
    if errors > 0 then os.exit(1) end
    os.exit(0)
end

return {
    bakeScene = bakeScene,
    isFresh = isFresh,
    kscPathFor = kscPathFor,
    listKsFiles = listKsFiles,
    ensureDir = ensureDir,
    checkWeb = checkWeb,
}
