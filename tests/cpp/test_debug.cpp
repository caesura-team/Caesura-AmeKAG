#include "doctest.h"
#include "debug/HotReload.h"
#include "debug/DebugProtocol.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <thread>

using namespace Caesura;

namespace {

std::string utf8SourcePath(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

constexpr const char* kStepScript =
    "local function inner()\n"
    "    local nested = 1\n"
    "    return nested\n"
    "end\n"
    "local value = 0\n"
    "value = inner()\n"
    "value = value + 1\n"
    "return value\n";

void checkStepCommand(DebugProtocol::Command command,
                      int breakpointLine,
                      int expectedStopLine) {
    lua_State* L = luaL_newstate();
    REQUIRE(L != nullptr);
    luaL_openlibs(L);

    HotReload reload;
    reload.init("__missing_hotreload_dir__", L);
    DebugProtocol protocol(reload);
    REQUIRE(protocol.init(L));
    protocol.setBreakpoint("debug_step.lua", breakpointLine);

    lua_State* coroutine = lua_newthread(L);
    REQUIRE(coroutine != nullptr);
    REQUIRE(luaL_loadbuffer(coroutine, kStepScript, std::strlen(kStepScript),
                            "debug_step.lua") == LUA_OK);

    int resultCount = 0;
    REQUIRE(lua_resume(coroutine, L, 0, &resultCount) == LUA_YIELD);
    REQUIRE(protocol.runState() == DebugProtocol::RunState::Paused);
    CHECK(protocol.currentLine() == breakpointLine);
    const auto firstPauseId = protocol.currentPauseId();
    REQUIRE(firstPauseId != DebugProtocol::NoPause);

    protocol.clearAllBreakpoints();
    auto commands = protocol.commandSink();
    REQUIRE(commands(firstPauseId, command));
    protocol.pumpCommands();
    REQUIRE(protocol.runState() == DebugProtocol::RunState::ResumePending);

    resultCount = 0;
    REQUIRE(protocol.resumePausedCoroutine(&resultCount) == LUA_YIELD);
    CHECK(protocol.runState() == DebugProtocol::RunState::Paused);
    CHECK(protocol.currentLine() == expectedStopLine);
    const auto secondPauseId = protocol.currentPauseId();
    REQUIRE(secondPauseId != DebugProtocol::NoPause);
    CHECK(secondPauseId != firstPauseId);
    CHECK_FALSE(commands(firstPauseId, DebugProtocol::Command::Continue));

    REQUIRE(commands(secondPauseId, DebugProtocol::Command::Continue));
    protocol.pumpCommands();
    resultCount = 0;
    CHECK(protocol.resumePausedCoroutine(&resultCount) == LUA_OK);
    CHECK(resultCount == 1);
    CHECK(lua_tointeger(coroutine, -1) == 2);

    protocol.shutdown();
    reload.shutdown();
    lua_close(L);
}

} // namespace

TEST_CASE("HotReload::default state") {
    HotReload hr;
    CHECK_FALSE(hr.initialized());
    CHECK(hr.scriptState() == ScriptState::IDLE);
}

TEST_CASE("HotReload::state transitions") {
    HotReload hr;
    hr.setScriptState(ScriptState::DEBUG_ACTIVE);
    CHECK(hr.scriptState() == ScriptState::DEBUG_ACTIVE);
    hr.setScriptState(ScriptState::IDLE);
    hr.setScriptState(ScriptState::RELOADING);
    CHECK(hr.scriptState() == ScriptState::RELOADING);
    hr.setScriptState(ScriptState::IDLE);
    CHECK(hr.scriptState() == ScriptState::IDLE);

    lua_State* first = luaL_newstate();
    REQUIRE(first != nullptr);
    hr.init("__missing_hotreload_dir__", first);
    CHECK(hr.initialized());
    hr.shutdown();
    CHECK_FALSE(hr.initialized());
    CHECK(hr.scriptState() == ScriptState::IDLE);
    lua_close(first);

    lua_State* second = luaL_newstate();
    REQUIRE(second != nullptr);
    hr.init("__missing_hotreload_dir__", second);
    CHECK(hr.initialized());
    hr.shutdown();
    CHECK_FALSE(hr.initialized());
    lua_close(second);
}

TEST_CASE("HotReload closes the active coroutine during a forced reload") {
    HotReload hr;
    lua_State* L = luaL_newstate();
    REQUIRE(L != nullptr);
    luaL_openlibs(L);

    const char* setup = R"lua(
        package.preload["kag"] = function()
            reload_count = (reload_count or 0) + 1
            return {}
        end
        require("kag")

        local co = coroutine.create(function()
            local cleanup <close> = setmetatable({}, {
                __close = function() reload_cleanup_finished = true end,
            })
            coroutine.yield("ready")
        end)
        assert(coroutine.resume(co))

        debug.getregistry().caesura_ctx = {
            co = co,
            active_operations = {},
        }

        close_argument_was_thread = false
        local original_close = coroutine.close
        coroutine.close = function(value)
            close_argument_was_thread = type(value) == "thread"
            return original_close(value)
        end
    )lua";

    const int setupStatus = luaL_dostring(L, setup);
    if (setupStatus != LUA_OK) {
        CAPTURE(lua_tostring(L, -1));
        hr.shutdown();
        lua_close(L);
        REQUIRE(setupStatus == LUA_OK);
    }

    hr.init("__missing_hotreload_dir__", L);
    int cancellationCalls = 0;
    hr.setBeforeReloadCallback([&] {
        ++cancellationCalls;
        lua_getglobal(L, "reload_cleanup_finished");
        CHECK(lua_toboolean(L, -1) != 0);
        lua_pop(L, 1);
        lua_getglobal(L, "reload_count");
        CHECK(lua_tointeger(L, -1) == 1);
        lua_pop(L, 1);
    });
    hr.requestReload();
    CHECK(hr.checkAndReload());
    CHECK(cancellationCalls == 1);
    CHECK(lua_gettop(L) == 0);

    lua_getglobal(L, "close_argument_was_thread");
    CHECK(lua_toboolean(L, -1) != 0);
    lua_pop(L, 1);

    lua_getglobal(L, "reload_count");
    CHECK(lua_tointeger(L, -1) == 2);
    lua_pop(L, 1);

    lua_getfield(L, LUA_REGISTRYINDEX, "caesura_ctx");
    const bool hasGameState = lua_istable(L, -1);
    CHECK(hasGameState);
    if (hasGameState) {
        lua_getfield(L, -1, "co");
        CHECK(lua_isnil(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    hr.shutdown();
    lua_close(L);
}

TEST_CASE("DebugProtocol instances are isolated per Lua state") {
    lua_State* firstL = luaL_newstate();
    lua_State* secondL = luaL_newstate();
    REQUIRE(firstL != nullptr);
    REQUIRE(secondL != nullptr);

    HotReload firstReload;
    HotReload secondReload;
    firstReload.init("__missing_hotreload_dir__", firstL);
    secondReload.init("__missing_hotreload_dir__", secondL);
    DebugProtocol first(firstReload);
    DebugProtocol second(secondReload);

    REQUIRE(first.init(firstL));
    REQUIRE(second.init(secondL));
    CHECK(first.init(firstL));
    CHECK(lua_gethook(firstL) != nullptr);
    CHECK(lua_gethook(secondL) != nullptr);

    first.setBreakpoint("first.lua", 11);
    second.setBreakpoint("second.lua", 22);
    CHECK(first.hasBreakpoint("first.lua", 11));
    CHECK_FALSE(first.hasBreakpoint("second.lua", 22));
    CHECK(second.hasBreakpoint("second.lua", 22));
    CHECK_FALSE(second.hasBreakpoint("first.lua", 11));

    first.shutdown();
    CHECK(lua_gethook(firstL) == nullptr);
    CHECK(lua_gethook(secondL) != nullptr);

    second.shutdown();
    firstReload.shutdown();
    secondReload.shutdown();
    lua_close(firstL);
    lua_close(secondL);
}

TEST_CASE("DebugProtocol::breakpoints with init") {
    HotReload hr;
    lua_State* L = luaL_newstate();
    REQUIRE(L != nullptr);
    luaL_openlibs(L);
    hr.init("__missing_hotreload_dir__", L);
    DebugProtocol dp(hr);
    REQUIRE(dp.init(L));
    
    CHECK_FALSE(dp.isDebugActive());
    dp.setBreakpoint("test.lua", 5);
    CHECK(dp.hasBreakpoint("test.lua", 5));
    CHECK_FALSE(dp.hasBreakpoint("test.lua", 99));
    dp.removeBreakpoint("test.lua", 5);
    CHECK_FALSE(dp.hasBreakpoint("test.lua", 5));
    
    dp.setBreakpoint("a.lua", 1);
    dp.setBreakpoint("b.lua", 2);
    dp.clearAllBreakpoints();
    CHECK_FALSE(dp.hasBreakpoint("a.lua", 1));

    const int tableStatus = luaL_dostring(L,
        "dangerous_table = setmetatable({ 1, 2 }, "
        "{ __len = function() error('must not run') end })");
    REQUIRE(tableStatus == LUA_OK);
    CHECK(dp.inspectGlobal("dangerous_table") == "table(2 entries)");

    const int globalsStatus = luaL_dostring(L,
        "global_index_hits = 0; "
        "setmetatable(_G, { __index = function() "
        "global_index_hits = global_index_hits + 1; return 'unsafe' end })");
    REQUIRE(globalsStatus == LUA_OK);
    const int globalsTop = lua_gettop(L);
    CHECK(dp.inspectGlobal("missing_global") == "nil");
    CHECK(dp.inspectGlobal("global_index_hits") == "0");
    CHECK(lua_gettop(L) == globalsTop);

    bool foreignShutdown = true;
    std::thread foreign([&dp, &foreignShutdown]() {
        foreignShutdown = dp.shutdown();
    });
    foreign.join();
    CHECK_FALSE(foreignShutdown);
    CHECK(dp.runState() == DebugProtocol::RunState::Running);
    CHECK(lua_gethook(L) != nullptr);

    CHECK(dp.shutdown());
    hr.shutdown();
    lua_close(L);
}

TEST_CASE("DebugProtocol canonicalizes source identifiers deterministically") {
    CHECK(DebugProtocol::canonicalSourceId(
              "@scripts\\chapter\\.\\scene\\..\\intro.lua") ==
          "scripts/chapter/intro.lua");
    CHECK(DebugProtocol::canonicalSourceId(
              "scripts/first/../../../shared/./intro.lua") ==
          "../shared/intro.lua");
    CHECK(DebugProtocol::canonicalSourceId("/scripts/../../intro.lua") ==
          "/intro.lua");
    CHECK(DebugProtocol::canonicalSourceId(
              "@C:\\Game\\Scripts\\Act1\\..\\Intro.LUA") ==
          "c:/game/scripts/intro.lua");
    CHECK(DebugProtocol::canonicalSourceId(
              "//Server/Share/Scripts/../Intro.lua") ==
          "//server/share/intro.lua");

#ifdef _WIN32
    CHECK(DebugProtocol::canonicalSourceId("Scripts/Intro.LUA") ==
          "scripts/intro.lua");
#else
    CHECK(DebugProtocol::canonicalSourceId("Scripts/Intro.LUA") ==
          "Scripts/Intro.LUA");
#endif
}

TEST_CASE("DebugProtocol matches absolute and relative breakpoint paths") {
    lua_State* L = luaL_newstate();
    REQUIRE(L != nullptr);

    HotReload reload;
    reload.init("__missing_hotreload_dir__", L);
    DebugProtocol protocol(reload);
    REQUIRE(protocol.init(L));

    const std::filesystem::path absolute =
        std::filesystem::current_path() / "scripts" / "chapter" / ".." /
        "source_identity.lua";
    const std::string relative = "./scripts/source_identity.lua";

    protocol.setBreakpoint(utf8SourcePath(absolute), 17);
    CHECK(protocol.hasBreakpoint(relative, 17));
    protocol.removeBreakpoint(relative, 17);
    CHECK_FALSE(protocol.hasBreakpoint(utf8SourcePath(absolute), 17));

    protocol.setBreakpoint("scripts/./source_identity.lua", 23);
    CHECK(protocol.hasBreakpoint(utf8SourcePath(absolute), 23));
    protocol.removeBreakpoint(utf8SourcePath(absolute), 23);
    CHECK_FALSE(protocol.hasBreakpoint(relative, 23));

    protocol.shutdown();
    reload.shutdown();
    lua_close(L);
}

TEST_CASE("DebugProtocol initializes and matches source paths in a Unicode working directory") {
    namespace fs = std::filesystem;
    const fs::path previous = fs::current_path();
    fs::path directory = fs::temp_directory_path() /
        ("caesura-debug-cwd-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    directory += fs::path(u8"-作品 输出 🎵𠀀");
    REQUIRE(fs::create_directory(directory));
    struct RestoreDirectory {
        fs::path previous;
        fs::path owned;
        ~RestoreDirectory() {
            std::error_code error;
            fs::current_path(previous, error);
            // This fixture creates only this empty directory; no recursive
            // removal can touch another test's files.
            fs::remove(owned, error);
        }
    } restore{previous, directory};
    fs::current_path(directory);
    std::unique_ptr<lua_State, decltype(&lua_close)> state(luaL_newstate(), lua_close);
    REQUIRE(state != nullptr);
    HotReload reload;
    DebugProtocol protocol(reload);
    REQUIRE(protocol.init(state.get()));

    // Match init()'s physical CWD when the host's temp root is a symlink.
    const std::string absolute = utf8SourcePath(fs::current_path() / "scripts/source.lua");
    protocol.setBreakpoint(absolute, 17);
    CHECK(protocol.hasBreakpoint("scripts/source.lua", 17));
    protocol.removeBreakpoint("./scripts/source.lua", 17);
    CHECK_FALSE(protocol.hasBreakpoint(absolute, 17));
    protocol.setBreakpoint("scripts/./source.lua", 23);
    CHECK(protocol.hasBreakpoint(absolute, 23));
    protocol.removeBreakpoint(absolute, 23);
    CHECK_FALSE(protocol.hasBreakpoint("scripts/source.lua", 23));
    CHECK(protocol.shutdown());
}

TEST_CASE("DebugProtocol yields a breakpoint without blocking the host") {
    lua_State* L = luaL_newstate();
    REQUIRE(L != nullptr);
    luaL_openlibs(L);

    HotReload reload;
    reload.init("__missing_hotreload_dir__", L);
    DebugProtocol protocol(reload);
    REQUIRE(protocol.init(L));
    const std::filesystem::path breakpointPath =
        std::filesystem::current_path() / "scripts" / "chapter" / ".." /
        "debug_nonblocking.lua";
    protocol.setBreakpoint(utf8SourcePath(breakpointPath), 2);
    auto commands = protocol.commandSink();
    CHECK_FALSE(commands(DebugProtocol::NoPause, DebugProtocol::Command::Continue));

    constexpr const char* script =
        "local value = 1\n"
        "value = value + 1\n"
        "return value\n";
    lua_State* coroutine = lua_newthread(L);
    REQUIRE(coroutine != nullptr);
    REQUIRE(luaL_loadbuffer(coroutine, script, std::strlen(script),
                            "@./scripts\\chapter\\..\\debug_nonblocking.lua") ==
            LUA_OK);

    int resultCount = 0;
    const auto started = std::chrono::steady_clock::now();
    const int pauseStatus = lua_resume(coroutine, L, 0, &resultCount);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE(pauseStatus == LUA_YIELD);
    CHECK(elapsed < std::chrono::milliseconds(250));
    CHECK(protocol.runState() == DebugProtocol::RunState::Paused);
    CHECK(protocol.isDebugActive());
    CHECK(protocol.currentSource() == "scripts/debug_nonblocking.lua");
    CHECK(protocol.currentLine() == 2);
    CHECK(reload.scriptState() == ScriptState::DEBUG_ACTIVE);
    const auto pauseId = protocol.currentPauseId();
    REQUIRE(pauseId != DebugProtocol::NoPause);

    {
        DebugProtocol competingProtocol(reload);
        CHECK_FALSE(competingProtocol.init(L));
    }
    CHECK(reload.scriptState() == ScriptState::DEBUG_ACTIVE);
    CHECK(protocol.runState() == DebugProtocol::RunState::Paused);

    int hostTicks = 0;
    for (int i = 0; i < 4; ++i) ++hostTicks;
    CHECK(hostTicks == 4);

    reload.requestReload();
    CHECK_FALSE(reload.checkAndReload());

    REQUIRE(commands(pauseId, DebugProtocol::Command::Continue));
    CHECK_FALSE(commands(pauseId, DebugProtocol::Command::StepInto));
    protocol.pumpCommands();
    CHECK(protocol.runState() == DebugProtocol::RunState::ResumePending);
    CHECK(protocol.isDebugActive());

    resultCount = 0;
    CHECK(protocol.resumePausedCoroutine(&resultCount) == LUA_OK);
    CHECK(resultCount == 1);
    CHECK(lua_tointeger(coroutine, -1) == 2);
    CHECK(protocol.runState() == DebugProtocol::RunState::Running);
    CHECK_FALSE(protocol.isDebugActive());
    CHECK(reload.scriptState() == ScriptState::IDLE);

    protocol.shutdown();
    reload.shutdown();
    lua_close(L);
}

TEST_CASE("DebugProtocol managed resume clears returned and yielded results") {
    SUBCASE("return results") {
        lua_State* L = luaL_newstate();
        REQUIRE(L != nullptr);
        luaL_openlibs(L);

        HotReload reload;
        reload.init("__missing_hotreload_dir__", L);
        DebugProtocol protocol(reload);
        REQUIRE(protocol.init(L));
        protocol.setBreakpoint("debug_managed_return.lua", 2);

        constexpr const char* script =
            "local value = 20\n"
            "value = value + 1\n"
            "return value, value + 1\n";
        lua_State* coroutine = lua_newthread(L);
        REQUIRE(coroutine != nullptr);
        REQUIRE(luaL_loadbuffer(coroutine, script, std::strlen(script),
                                "debug_managed_return.lua") == LUA_OK);

        int resultCount = 0;
        REQUIRE(lua_resume(coroutine, L, 0, &resultCount) == LUA_YIELD);
        REQUIRE(protocol.continue_());
        protocol.pumpCommands();

        const auto outcome = protocol.resumePausedCoroutineManaged();
        CHECK(outcome.status == LUA_OK);
        CHECK(outcome.resultCount == 2);
        CHECK(outcome.error.empty());
        CHECK(lua_gettop(coroutine) == 0);

        protocol.shutdown();
        reload.shutdown();
        lua_close(L);
    }

    SUBCASE("yield results") {
        lua_State* L = luaL_newstate();
        REQUIRE(L != nullptr);
        luaL_openlibs(L);

        HotReload reload;
        reload.init("__missing_hotreload_dir__", L);
        DebugProtocol protocol(reload);
        REQUIRE(protocol.init(L));
        protocol.setBreakpoint("debug_managed_yield.lua", 2);

        constexpr const char* script =
            "local value = 20\n"
            "value = value + 1\n"
            "coroutine.yield('ready', value)\n"
            "return value + 1\n";
        lua_State* coroutine = lua_newthread(L);
        REQUIRE(coroutine != nullptr);
        REQUIRE(luaL_loadbuffer(coroutine, script, std::strlen(script),
                                "debug_managed_yield.lua") == LUA_OK);

        int resultCount = 0;
        REQUIRE(lua_resume(coroutine, L, 0, &resultCount) == LUA_YIELD);
        REQUIRE(protocol.continue_());
        protocol.pumpCommands();

        const auto outcome = protocol.resumePausedCoroutineManaged();
        CHECK(outcome.status == LUA_YIELD);
        CHECK(outcome.resultCount == 2);
        CHECK(outcome.error.empty());
        CHECK(lua_gettop(coroutine) == 0);

        resultCount = 0;
        CHECK(lua_resume(coroutine, L, 0, &resultCount) == LUA_OK);
        CHECK(resultCount == 1);

        protocol.shutdown();
        reload.shutdown();
        lua_close(L);
    }
}

TEST_CASE("DebugProtocol managed resume preserves errors and clears the stack") {
    lua_State* L = luaL_newstate();
    REQUIRE(L != nullptr);
    luaL_openlibs(L);

    HotReload reload;
    reload.init("__missing_hotreload_dir__", L);
    DebugProtocol protocol(reload);
    REQUIRE(protocol.init(L));
    protocol.setBreakpoint("debug_managed_error.lua", 2);

    constexpr const char* script =
        "local value = 20\n"
        "value = value + 1\n"
        "error('managed resume failure')\n";
    lua_State* coroutine = lua_newthread(L);
    REQUIRE(coroutine != nullptr);
    REQUIRE(luaL_loadbuffer(coroutine, script, std::strlen(script),
                            "debug_managed_error.lua") == LUA_OK);

    int resultCount = 0;
    REQUIRE(lua_resume(coroutine, L, 0, &resultCount) == LUA_YIELD);
    REQUIRE(protocol.continue_());
    protocol.pumpCommands();

    const auto outcome = protocol.resumePausedCoroutineManaged();
    CHECK(outcome.status == LUA_ERRRUN);
    CHECK(outcome.resultCount > 0);
    CHECK(outcome.error.find("managed resume failure") != std::string::npos);
    CHECK(lua_gettop(coroutine) == 0);

    protocol.shutdown();
    reload.shutdown();
    lua_close(L);
}

TEST_CASE("DebugProtocol managed resume distinguishes no pending resume") {
    lua_State* L = luaL_newstate();
    REQUIRE(L != nullptr);

    HotReload reload;
    reload.init("__missing_hotreload_dir__", L);
    DebugProtocol protocol(reload);
    REQUIRE(protocol.init(L));

    const auto outcome = protocol.resumePausedCoroutineManaged();
    CHECK(outcome.status == DebugProtocol::NoResume);
    CHECK(outcome.resultCount == 0);
    CHECK(outcome.error.empty());

    protocol.shutdown();
    reload.shutdown();
    lua_close(L);
}

TEST_CASE("DebugProtocol step commands resume the paused coroutine") {
    SUBCASE("step into enters the called function") {
        checkStepCommand(DebugProtocol::Command::StepInto, 6, 2);
    }
    SUBCASE("step over stays in the caller") {
        checkStepCommand(DebugProtocol::Command::StepOver, 6, 7);
    }
    SUBCASE("step out returns to the caller") {
        checkStepCommand(DebugProtocol::Command::StepOut, 2, 7);
    }
}

TEST_CASE("DebugProtocol reports non-yieldable hits without failing lua_pcall") {
    lua_State* L = luaL_newstate();
    REQUIRE(L != nullptr);
    luaL_openlibs(L);

    HotReload reload;
    reload.init("__missing_hotreload_dir__", L);
    DebugProtocol protocol(reload);
    REQUIRE(protocol.init(L));
    protocol.setBreakpoint("debug_main.lua", 2);

    constexpr const char* script =
        "local value = 1\n"
        "value = value + 1\n"
        "return value\n";
    REQUIRE(luaL_loadbuffer(L, script, std::strlen(script), "debug_main.lua") == LUA_OK);
    const int status = lua_pcall(L, 0, 1, 0);

    REQUIRE(status == LUA_OK);
    CHECK(lua_tointeger(L, -1) == 2);
    lua_pop(L, 1);
    CHECK(protocol.nonYieldableHitCount() >= 1);
    CHECK(protocol.currentSource() == "debug_main.lua");
    CHECK(protocol.currentLine() == 2);
    CHECK(protocol.runState() == DebugProtocol::RunState::Running);
    CHECK_FALSE(protocol.isDebugActive());

    protocol.shutdown();
    reload.shutdown();
    lua_close(L);
}

TEST_CASE("DebugProtocol anchors a paused coroutine through garbage collection") {
    lua_State* L = luaL_newstate();
    REQUIRE(L != nullptr);
    luaL_openlibs(L);

    HotReload reload;
    reload.init("__missing_hotreload_dir__", L);
    DebugProtocol protocol(reload);
    REQUIRE(protocol.init(L));
    protocol.setBreakpoint("debug_anchor.lua", 2);

    constexpr const char* script =
        "local value = 41\n"
        "value = value + 1\n"
        "return value\n";
    lua_State* coroutine = lua_newthread(L);
    REQUIRE(coroutine != nullptr);
    REQUIRE(luaL_loadbuffer(coroutine, script, std::strlen(script),
                            "debug_anchor.lua") == LUA_OK);

    int resultCount = 0;
    REQUIRE(lua_resume(coroutine, L, 0, &resultCount) == LUA_YIELD);
    lua_settop(L, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);
    const int pausedTop = lua_gettop(coroutine);
    CHECK(protocol.inspectLocal(0, "value") == "41");
    CHECK(lua_gettop(coroutine) == pausedTop);

    auto commands = protocol.commandSink();
    REQUIRE(commands(protocol.currentPauseId(), DebugProtocol::Command::Continue));
    protocol.pumpCommands();
    resultCount = 0;
    CHECK(protocol.resumePausedCoroutine(&resultCount) == LUA_OK);
    CHECK(resultCount == 1);
    CHECK(lua_tointeger(coroutine, -1) == 42);

    protocol.shutdown();
    reload.shutdown();
    lua_close(L);
}

TEST_CASE("DebugProtocol shutdown detaches paused coroutines and closes command sinks") {
    lua_State* L = luaL_newstate();
    REQUIRE(L != nullptr);
    luaL_openlibs(L);

    HotReload reload;
    reload.init("__missing_hotreload_dir__", L);
    DebugProtocol protocol(reload);
    REQUIRE(protocol.init(L));
    protocol.setBreakpoint("debug_shutdown.lua", 2);

    constexpr const char* script =
        "local value = 1\n"
        "value = value + 1\n"
        "return value\n";
    lua_State* coroutine = lua_newthread(L);
    REQUIRE(coroutine != nullptr);
    REQUIRE(luaL_loadbuffer(coroutine, script, std::strlen(script),
                            "debug_shutdown.lua") == LUA_OK);

    int resultCount = 0;
    REQUIRE(lua_resume(coroutine, L, 0, &resultCount) == LUA_YIELD);
    auto commands = protocol.commandSink();
    const auto pauseId = protocol.currentPauseId();
    REQUIRE(pauseId != DebugProtocol::NoPause);

    std::atomic<bool> started{false};
    std::atomic<bool> stopProducer{false};
    std::thread producer([commands, pauseId, &started, &stopProducer]() {
        started.store(true, std::memory_order_release);
        while (!stopProducer.load(std::memory_order_acquire)) {
            commands(pauseId, DebugProtocol::Command::Continue);
        }
    });
    while (!started.load(std::memory_order_acquire)) std::this_thread::yield();

    protocol.shutdown();
    stopProducer.store(true, std::memory_order_release);
    producer.join();

    CHECK(protocol.runState() == DebugProtocol::RunState::Detached);
    CHECK_FALSE(commands(pauseId, DebugProtocol::Command::Continue));
    CHECK(reload.scriptState() == ScriptState::IDLE);
    CHECK(lua_gethook(L) == nullptr);

    resultCount = 0;
    CHECK(lua_resume(coroutine, L, 0, &resultCount) == LUA_OK);
    CHECK(lua_gethook(coroutine) == nullptr);

    reload.shutdown();
    lua_close(L);
}
