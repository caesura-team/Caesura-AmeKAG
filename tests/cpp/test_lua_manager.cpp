#include "doctest.h"
#include "debug/DebugProtocol.h"
#include "debug/HotReload.h"
#include "script/vm/LuaManager.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <cstring>
#include <cstdio>

using namespace Caesura;

TEST_CASE("LuaManager::init creates valid state") {
    LuaManager lm;
    CHECK(lm.init());
    CHECK(lm.state() != nullptr);
}

TEST_CASE("LuaManager::shutdown idempotent") {
    LuaManager lm;
    lm.init();
    lm.shutdown();
    lm.shutdown();
    CHECK(lm.state() == nullptr);
}

TEST_CASE("LuaManager::lockdownScriptEnv removes dangerous globals") {
    LuaManager lm;
    lm.init();
    lm.lockdownScriptEnv();
    lua_State* L = lm.state();
    REQUIRE(L != nullptr);

    // loadfile should be removed
    lua_getglobal(L, "loadfile");
    CHECK(lua_isnil(L, -1));
    lua_pop(L, 1);

    // dofile should be removed
    lua_getglobal(L, "dofile");
    CHECK(lua_isnil(L, -1));
    lua_pop(L, 1);

    // debug should be restricted (read-only subset)
    lua_getglobal(L, "debug");
    CHECK(lua_istable(L, -1));
    lua_pop(L, 1);

    // require should be overridden (safe wrapper)
    lua_getglobal(L, "require");
    CHECK(lua_isfunction(L, -1));
    lua_pop(L, 1);

    // io.input/output/lines bypass io.open internally and can still access
    // arbitrary files unless each entry point is removed explicitly.
    lua_getglobal(L, "io");
    REQUIRE(lua_istable(L, -1));
    const char* dangerousIoFunctions[] = {"input", "output", "lines", "read", "tmpfile"};
    for (const char* name : dangerousIoFunctions) {
        lua_getfield(L, -1, name);
        CAPTURE(name);
        CHECK(lua_isnil(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    // Environment variables commonly contain credentials and build secrets.
    lua_getglobal(L, "os");
    REQUIRE(lua_istable(L, -1));
    lua_getfield(L, -1, "getenv");
    CHECK(lua_isnil(L, -1));
    lua_pop(L, 2);
}

TEST_CASE("LuaManager::loadScript fails on nonexistent file") {
    LuaManager lm;
    lm.init();
    // [galgame-flake-H1] A truly missing file is a persistent LUA_ERRFILE: the
    // one-shot retry cannot rescue it and loadScript must still return false.
    CHECK_FALSE(lm.loadScript("nonexistent_script.lua"));
}

// [galgame-flake-H1] Retry boundary: the success path must load a real script
// without any retry interfering (an extra retry would double-execute the file's
// side effects). A valid file with a side effect is executed exactly once.
TEST_CASE("LuaManager::loadScript loads valid file exactly once on success path") {
    LuaManager lm;
    REQUIRE(lm.init());
    lua_State* L = lm.state();
    REQUIRE(L != nullptr);

    const char* tmpFile = "loadscript_success_tmp.lua";
    const char* body = "loaded_count = (loaded_count or 0) + 1\n";
    std::FILE* f = std::fopen(tmpFile, "wb");
    REQUIRE(f != nullptr);
    std::fwrite(body, 1, std::strlen(body), f);
    std::fclose(f);

    CHECK(lm.loadScript(tmpFile));   // first load executes once
    CHECK(lm.loadScript(tmpFile));   // second load executes again (it is not cached)

    lua_getglobal(L, "loaded_count");
    CHECK(lua_tointeger(L, -1) == 2);
    lua_pop(L, 1);
    std::remove(tmpFile);
}

// [galgame-flake-H1] Only LUA_ERRFILE is retried. A real syntax error returns a
// non-ERRFILE code and must fail immediately (and stay failed after the retry
// path is skipped) — the fix must not turn a genuine script bug into flakiness.
TEST_CASE("LuaManager::loadScript does not retry genuine syntax errors") {
    LuaManager lm;
    REQUIRE(lm.init());
    lua_State* L = lm.state();
    REQUIRE(L != nullptr);

    const char* tmpFile = "loadscript_syntax_tmp.lua";
    const char* body = "this is not valid lua !!!\n";
    std::FILE* f = std::fopen(tmpFile, "wb");
    REQUIRE(f != nullptr);
    std::fwrite(body, 1, std::strlen(body), f);
    std::fclose(f);

    // A syntax error is not a file-open failure; loadScript must return false
    // and leave the VM in a state where no partial result was produced.
    CHECK_FALSE(lm.loadScript(tmpFile));
    std::remove(tmpFile);
}

TEST_CASE("LuaManager::double init is safe") {
    LuaManager lm;
    CHECK(lm.init());
    CHECK(lm.init());  // second call
    CHECK(lm.state() != nullptr);
}

// =============================================================================
// Expanded: instruction budget, update, resumeKAG
// =============================================================================

TEST_CASE("LuaManager::instruction budget set/get round-trip") {
    LuaManager lm;
    lm.setInstructionBudget(200000);
    CHECK(lm.getInstructionBudget() == 200000);
    lm.resetInstructionBudget();
    CHECK_FALSE(lm.isInstructionBudgetExceeded());
}

TEST_CASE("LuaManager::instruction budget interrupts infinite loop") {
    LuaManager independentManager;
    independentManager.resetInstructionBudget();
    independentManager.setInstructionBudget(1);

    LuaManager manager;
    manager.setInstructionBudget(100);
    REQUIRE(manager.init());
    lua_State* L = manager.state();
    REQUIRE(L != nullptr);

    int result = luaL_dostring(L, "while true do end");
    CHECK(result != LUA_OK);
    CHECK(manager.isInstructionBudgetExceeded());
    CHECK_FALSE(independentManager.isInstructionBudgetExceeded());
}

TEST_CASE("DebugProtocol preserves LuaManager instruction budget") {
    LuaManager manager;
    manager.setInstructionBudget(100);
    REQUIRE(manager.init());
    lua_State* L = manager.state();
    REQUIRE(L != nullptr);

    const lua_Hook budgetHook = lua_gethook(L);
    const int budgetMask = lua_gethookmask(L);
    const int budgetCount = lua_gethookcount(L);

    HotReload reload;
    reload.init("__missing_hotreload_dir__", L);
    DebugProtocol protocol(reload);
    REQUIRE(protocol.init(L));
    CHECK((lua_gethookmask(L) & LUA_MASKCOUNT) != 0);
    CHECK(lua_gethookcount(L) == budgetCount);

    const int result = luaL_dostring(
        L, "local total = 0; for i = 1, 100000 do total = total + i end");
    if (result != LUA_OK) {
        CAPTURE(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    CHECK(result != LUA_OK);
    CHECK(manager.isInstructionBudgetExceeded());

    protocol.shutdown();
    CHECK(lua_gethook(L) == budgetHook);
    CHECK(lua_gethookmask(L) == budgetMask);
    CHECK(lua_gethookcount(L) == budgetCount);
    reload.shutdown();
    manager.shutdown();
}

TEST_CASE("Coroutine budget survives DebugProtocol destruction") {
    LuaManager manager;
    manager.setInstructionBudget(100);
    REQUIRE(manager.init());
    lua_State* L = manager.state();
    REQUIRE(L != nullptr);

    const lua_Hook budgetHook = lua_gethook(L);
    const int budgetMask = lua_gethookmask(L);
    const int budgetCount = lua_gethookcount(L);
    lua_State* coroutine = nullptr;
    lua_Hook coroutineBridge = nullptr;

    HotReload reload;
    reload.init("__missing_hotreload_dir__", L);
    {
        DebugProtocol protocol(reload);
        REQUIRE(protocol.init(L));
        coroutine = lua_newthread(L);
        REQUIRE(coroutine != nullptr);
        coroutineBridge = lua_gethook(coroutine);
        CHECK(coroutineBridge != nullptr);
        CHECK(coroutineBridge != budgetHook);
        CHECK((lua_gethookmask(coroutine) & LUA_MASKCOUNT) != 0);
        REQUIRE(luaL_loadstring(coroutine,
            "local total = 0; for i = 1, 100000 do total = total + i end") == LUA_OK);
    }

    CHECK(lua_gethook(L) == budgetHook);
    CHECK(lua_gethookmask(L) == budgetMask);
    CHECK(lua_gethookcount(L) == budgetCount);
    CHECK(lua_gethook(coroutine) == coroutineBridge);

    reload.shutdown();
    manager.resetInstructionBudget();
    int resultCount = 0;
    const int result = lua_resume(coroutine, L, 0, &resultCount);
    if (result != LUA_OK) {
        CAPTURE(lua_tostring(coroutine, -1));
    }
    CHECK(result != LUA_OK);
    CHECK(manager.isInstructionBudgetExceeded());
    manager.shutdown();
}

TEST_CASE("LuaManager instruction budget survives debugger pause and resume") {
    LuaManager manager;
    manager.setInstructionBudget(2000);
    REQUIRE(manager.init());
    lua_State* L = manager.state();
    REQUIRE(L != nullptr);

    HotReload reload;
    reload.init("__missing_hotreload_dir__", L);
    DebugProtocol protocol(reload);
    REQUIRE(protocol.init(L));
    protocol.setBreakpoint("debug_budget.lua", 2);

    constexpr const char* script =
        "local total = 0\n"
        "for i = 1, 100000 do\n"
        "    total = total + i\n"
        "end\n"
        "return total\n";
    lua_State* coroutine = lua_newthread(L);
    REQUIRE(coroutine != nullptr);
    REQUIRE(luaL_loadbuffer(coroutine, script, std::strlen(script),
                            "debug_budget.lua") == LUA_OK);

    int resultCount = 0;
    REQUIRE(lua_resume(coroutine, L, 0, &resultCount) == LUA_YIELD);
    CHECK(protocol.runState() == DebugProtocol::RunState::Paused);
    CHECK_FALSE(manager.isInstructionBudgetExceeded());

    protocol.clearAllBreakpoints();
    auto commands = protocol.commandSink();
    REQUIRE(commands(protocol.currentPauseId(), DebugProtocol::Command::Continue));
    protocol.pumpCommands();
    resultCount = 0;
    const int status = protocol.resumePausedCoroutine(&resultCount);
    CHECK(status != LUA_OK);
    CHECK(status != LUA_YIELD);
    CHECK(manager.isInstructionBudgetExceeded());

    protocol.shutdown();
    reload.shutdown();
    manager.shutdown();
}

TEST_CASE("LuaManager::update does not crash") {
    LuaManager lm;
    lm.init();
    lm.update(0.016f);
    lm.update(0.0f);
}

TEST_CASE("LuaManager::resumeKAGCoroutine does not crash without coroutine") {
    LuaManager lm;
    lm.init();
    // No KAG coroutine running — should be a safe no-op
    lm.resumeKAGCoroutine();
}

TEST_CASE("Lua BackendFactory name commands report actual engine backend info") {
    LuaManager lm;
    REQUIRE(lm.init());
    lua_State* L = lm.state();
    REQUIRE(L != nullptr);

    const char* script =
        "package.path = 'scripts/?.lua;scripts/?/init.lua;' .. package.path\n"
        "package.loaded['backend_factory'] = nil\n"
        "KAG = {}\n"
        "Render = {}\n"
        "DevCore = {}\n"
        "Engine = {\n"
        "  select_platform_backend = function() return true end,\n"
        "  get_backend_info = function()\n"
        "    return { render = 'NullRender', audio = 'NullAudio', platform = 'NullPlatform' }\n"
        "  end,\n"
        "}\n"
        "local BackendFactory = require('backend_factory')\n"
        "local backend = BackendFactory.create({ render = 'bgfx', audio = 'soloud', platform = 'sdl3' })\n"
        "assert(backend.render('name') == 'NullRender', backend.render('name'))\n"
        "assert(backend.audio('name') == 'NullAudio', backend.audio('name'))\n"
        "assert(backend.platform('name') == 'NullPlatform', backend.platform('name'))\n";

    int luaStatus = luaL_loadstring(L, script);
    if (luaStatus == LUA_OK) {
        luaStatus = lua_pcall(L, 0, 0, 0);
    }
    if (luaStatus != LUA_OK) {
        CAPTURE(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    CHECK(luaStatus == LUA_OK);
}

TEST_CASE("Lua config ready log reports actual engine backend info") {
    LuaManager lm;
    REQUIRE(lm.init());
    lua_State* L = lm.state();
    REQUIRE(L != nullptr);

    const char* script =
        "package.path = 'scripts/?.lua;scripts/?/init.lua;' .. package.path\n"
        "package.loaded['backend_factory'] = nil\n"
        "local captured = {}\n"
        "local oldPrint = print\n"
        "print = function(msg) captured[#captured + 1] = tostring(msg) end\n"
        "KAG = { set_bus_volume = function() return true end }\n"
        "Render = {}\n"
        "DevCore = {\n"
        "  set_resolution = function() return true end,\n"
        "  set_fullscreen = function() return true end,\n"
        "}\n"
        "Engine = {\n"
        "  select_platform_backend = function() return true end,\n"
        "  get_backend_info = function()\n"
        "    return { render = 'NullRender', audio = 'NullAudio', platform = 'NullPlatform' }\n"
        "  end,\n"
        "}\n"
        "local ok, err = pcall(dofile, 'scripts/config.lua')\n"
        "print = oldPrint\n"
        "assert(ok, err)\n"
        "local expected = '[config] Backends ready: render=NullRender audio=NullAudio platform=NullPlatform'\n"
        "for _, line in ipairs(captured) do\n"
        "  if line == expected then return end\n"
        "end\n"
        "error('missing ready log: ' .. table.concat(captured, '\\n'))\n";

    int luaStatus = luaL_loadstring(L, script);
    if (luaStatus == LUA_OK) {
        luaStatus = lua_pcall(L, 0, 0, 0);
    }
    if (luaStatus != LUA_OK) {
        CAPTURE(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    CHECK(luaStatus == LUA_OK);
}

TEST_CASE("Lua backend audio helpers prefer unified backend proxy") {
    LuaManager lm;
    REQUIRE(lm.init());
    lua_State* L = lm.state();
    REQUIRE(L != nullptr);

    const char* script =
        "package.path = 'scripts/?.lua;scripts/?/init.lua;' .. package.path\n"
        "package.loaded['backend'] = nil\n"
        // This VM observes Lua proxy routing and string payloads, not playback.
        // Select its no-host boundary before the runtime captures a provider.
        "assert(package.loaded['capability_runtime'] == nil)\n"
        "rawset(Engine, 'get_capability_profile', nil)\n"
        "local runtime = require('capability_runtime')\n"
        "assert(not runtime.has_host())\n"
        "assert(runtime.query('audio.play').proven == false)\n"
        "local calls = {}\n"
        "_CAESURA_BACKEND = {\n"
        "  audio = function(cmd, ...)\n"
        "    calls[#calls + 1] = cmd\n"
        "    if cmd == 'is_voice_playing' then return true end\n"
        "    if cmd == 'is_bgm_playing' then return true end\n"
        "    if cmd == 'is_playing' then return true end\n"
        "    return 'proxy:' .. cmd\n"
        "  end,\n"
        "}\n"
        "KAG = {\n"
        "  play_bgm = function() error('fallback play_bgm used') end,\n"
        "  play_voice = function() error('fallback play_voice used') end,\n"
        "  play_se = function() error('fallback play_se used') end,\n"
        "  stop_bgm = function() error('fallback stop_bgm used') end,\n"
        "  stop_voice = function() error('fallback stop_voice used') end,\n"
        "  stop_se = function() error('fallback stop_se used') end,\n"
        "  is_voice_playing = function() error('fallback is_voice_playing used') end,\n"
        "  is_bgm_playing = function() error('fallback is_bgm_playing used') end,\n"
        "  is_se_playing = function() error('fallback is_se_playing used') end,\n"
        "}\n"
        "local backend = require('backend')\n"
        "assert(backend.audio_play('bgm', 'song.ogg') == 'proxy:play_bgm')\n"
        "assert(backend.audio_play('voice', 'voice.ogg') == 'proxy:play_voice')\n"
        "assert(backend.audio_play('se', 'click.wav') == 'proxy:play_se')\n"
        "assert(backend.audio_stop('bgm') == 'proxy:stop_bgm')\n"
        "assert(backend.audio_stop('voice') == 'proxy:stop_voice')\n"
        "assert(backend.audio_stop('se') == 'proxy:stop_se')\n"
        "assert(backend.audio_is_playing('voice') == true)\n"
        "assert(backend.audio_is_playing('bgm') == true)\n"
        "assert(backend.audio_is_playing('se') == true)\n"
        "assert(#calls == 9, 'calls=' .. tostring(#calls))\n";

    int luaStatus = luaL_loadstring(L, script);
    if (luaStatus == LUA_OK) {
        luaStatus = lua_pcall(L, 0, 0, 0);
    }
    if (luaStatus != LUA_OK) {
        CAPTURE(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    CHECK(luaStatus == LUA_OK);
}

TEST_CASE("Lua backend platform helpers prefer unified backend proxy") {
    LuaManager lm;
    REQUIRE(lm.init());
    lua_State* L = lm.state();
    REQUIRE(L != nullptr);

    const char* script =
        "package.path = 'scripts/?.lua;scripts/?/init.lua;' .. package.path\n"
        "package.loaded['backend'] = nil\n"
        "local calls = {}\n"
        "_CAESURA_BACKEND = {\n"
        "  platform = function(cmd, ...)\n"
        "    calls[#calls + 1] = cmd\n"
        "    if cmd == 'get_resolution' then return 1280, 720 end\n"
        "    if cmd == 'get_input_focus' then return 'game' end\n"
        "    return 'proxy:' .. cmd\n"
        "  end,\n"
        "}\n"
        "DevCore = {\n"
        "  set_resolution = function() error('fallback set_resolution used') end,\n"
        "  get_resolution = function() error('fallback get_resolution used') end,\n"
        "  set_input_focus = function() error('fallback set_input_focus used') end,\n"
        "  get_input_focus = function() error('fallback get_input_focus used') end,\n"
        "  set_fullscreen = function() error('fallback set_fullscreen used') end,\n"
        "}\n"
        "local backend = require('backend')\n"
        "assert(backend.set_resolution(1920, 1080) == 'proxy:set_resolution')\n"
        "local w, h = backend.get_resolution()\n"
        "assert(w == 1280 and h == 720, tostring(w) .. 'x' .. tostring(h))\n"
        "assert(backend.set_input_focus('ui') == 'proxy:set_input_focus')\n"
        "assert(backend.get_input_focus() == 'game')\n"
        "assert(backend.set_fullscreen(true) == 'proxy:set_fullscreen')\n"
        "assert(#calls == 5, 'calls=' .. tostring(#calls))\n";

    int luaStatus = luaL_loadstring(L, script);
    if (luaStatus == LUA_OK) {
        luaStatus = lua_pcall(L, 0, 0, 0);
    }
    if (luaStatus != LUA_OK) {
        CAPTURE(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    CHECK(luaStatus == LUA_OK);
}
