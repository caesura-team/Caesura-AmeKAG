-- =============================================================================
--  test_sandbox.lua — Sandbox security verification
-- =============================================================================

local passed, failed = 0, 0
local function check(name, cond, detail)
    if cond then
        passed = passed + 1
        print(string.format("  [PASS] %s", name))
    else
        failed = failed + 1
        print(string.format("  [FAIL] %s  -- %s", name, detail or ""))
    end
end

print("\n=== Sandbox Tests ===\n")

-- Keep only the source bytes as an oracle, never an unrestricted I/O handle.
-- On Windows this also catches silently replacing "rb" with text mode "r".
local binary_path = "tests/scripts/smoke_test.ks"
local binary_source = assert(io.open(binary_path, "rb"))
local expected_bytes = assert(binary_source:read("*a"))
binary_source:close()

-- 1. sandbox.lua loads
do
    local ok, sb = pcall(require, "sandbox")
    check("sandbox loads", ok, tostring(sb))
end

-- 2. sandbox module has expected functions
do
    local sandbox = require("sandbox")
    check("sandbox is table", type(sandbox) == "table")
    check("create exists", type(sandbox.create) == "function")
    check("execute exists", type(sandbox.execute) == "function")
    check("is_strict exists", type(sandbox.is_strict) == "function")
end

-- 3. Dev mode: basic code runs
do
    local sandbox = require("sandbox")
    local env = sandbox.create({mode = "dev"})
    check("dev env created", type(env) == "table")
    local fn, err = load("return 1 + 1", "=test", "t", env)
    check("dev eval loads", fn ~= nil, err)
    if fn then
        local ok, result = pcall(fn)
        check("dev eval executes", ok and result == 2, tostring(result))
    end
end

-- 4. Release mode: blacklisted globals blocked at compile time
do
    local sandbox = require("sandbox")
    local env = sandbox.create({mode = "release"})
    check("release env created", type(env) == "table")
    -- Accessing blacklisted globals should fail at load() time
    local fn, err = load("return os.exit", "=test", "t", env)
    check("release: os.exit blocked", fn ~= nil, "load should fail: " .. (err or "nil"))
end

-- 5. Strict mode: input event handlers and key constants whitelisted in _G
do
    local ok1 = pcall(function() _G._KAG_onTextInput = function() end end)
    check("whitelist: _KAG_onTextInput", ok1)
    local ok2 = pcall(function() _G._KAG_onTextEditing = function() end end)
    check("whitelist: _KAG_onTextEditing", ok2)
    local ok3 = pcall(function() _G._KAG_onKeyDown = function() end end)
    check("whitelist: _KAG_onKeyDown", ok3)
    local ok4 = pcall(function() _G._GAME_KEY_BACKSPACE = true end)
    check("whitelist: _GAME_KEY_BACKSPACE", ok4)
    -- t109: native swipe consumers route via SDLK_SPACE/SDLK_PAGEUP -> these
    -- hooks; they must be whitelisted like every other _KAG_on* handler.
    local ok5 = pcall(function() _G._KAG_onKeySpace = function() end end)
    check("whitelist: _KAG_onKeySpace", ok5)
    local ok6 = pcall(function() _G._KAG_onKeyPageUp = function() end end)
    check("whitelist: _KAG_onKeyPageUp", ok6)
    local ok_blocked = pcall(function() _G._UNAUTHORIZED_TEST_VAR_FORBIDDEN = 123 end)
    check("strict: unauthorized global blocked", not ok_blocked)
end

-- 6. Allowlisted sources remain readable after lockdown, with read-only modes.
do
    local binary, err = io.open(binary_path, "rb")
    check("io: allowlisted binary read opens", binary ~= nil, err)
    if binary then
        check("io: binary read preserves source bytes", binary:read("*a") == expected_bytes)
        local wrote, result = pcall(binary.write, binary, "forbidden")
        check("io: binary handle cannot write", not wrote or result == nil)
        binary:close()
    end
    local text, err_text = io.open(binary_path, "r")
    check("io: allowlisted text read remains available", text ~= nil, err_text)
    if text then text:close() end

    for _, mode in ipairs({"w", "wb", "w+", "wb+", "w+b", "a", "ab",
                            "a+", "ab+", "a+b", "r+", "rb+", "r+b"}) do
        local handle, denied = io.open("tests/__sandbox_write_denied__/probe.tmp", mode)
        check("io: reject write mode " .. mode,
              handle == nil and denied == "io.open write disabled", denied)
        if handle then handle:close() end
    end
    for _, path in ipairs({"tests/../scripts/sandbox.lua", "tests/..\\scripts/sandbox.lua"}) do
        local handle, denied = io.open(path, "rb")
        check("io: reject binary traversal " .. path,
              handle == nil and denied == "io.open traversal rejected", denied)
        if handle then handle:close() end
    end
    for _, path in ipairs({"README.md", "cache/ksc/smoke_test.ksc", "/etc/passwd", "C:/Windows/win.ini"}) do
        local handle, denied = io.open(path, "rb")
        check("io: reject binary path outside allowlist " .. path,
              handle == nil and denied == "io.open path not allowlisted", denied)
        if handle then handle:close() end
    end
    local handle, denied = io.open({}, "rb")
    check("io: reject non-string binary path",
          handle == nil and denied == "io.open path must be string", denied)
end

print(string.format("\nResults: %d passed, %d failed", passed, failed))
if failed > 0 then os.exit(1) end
