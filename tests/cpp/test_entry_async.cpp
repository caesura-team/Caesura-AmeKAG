#include "doctest.h"
#include "entry/Engine.h"
#include "entry/EngineConfig.h"
#include "debug/DebugProtocol.h"
#include "job/api/IJobSystem.h"
#include "di/BackendRegistry.h"
#include "resource/api/IAsyncLoader.h"
#include "render/NullRenderDevice.h"
#include "render/NullGpuMonitor.h"
#include "audio/NullAudioBackend.h"
#include "platform/NullPlatformBackend.h"
#include <vector>
#include "script/vm/LuaManager.h"
#include <cstring>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

using namespace Caesura;

namespace {

EngineConfig asyncEngineConfig() {
    EngineConfig config;
    config.headless = true;
    config.enableDebugger = true;
    return config;
}

void settleAsyncJobs(Engine& engine) {
    engine.jobSystem().waitIdle();
    engine.jobSystem().pollMainThreadJobs();
}

int luaCount(lua_State* L, const char* name) {
    lua_getglobal(L, name);
    const int count = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    return count;
}

DebugProtocol::PauseId pauseAsyncEngine(Engine& engine) {
    auto* protocol = engine.debugProtocol();
    REQUIRE(protocol != nullptr);
    lua_State* L = engine.lua().state();
    constexpr const char* code = "local value = 1\nvalue = value + 1\nreturn value\n";
    protocol->setBreakpoint("async_debug.lua", 2);
    lua_State* coroutine = lua_newthread(L);
    REQUIRE(luaL_loadbuffer(coroutine, code, std::strlen(code), "async_debug.lua") == LUA_OK);
    int results = 0;
    REQUIRE(lua_resume(coroutine, L, 0, &results) == LUA_YIELD);
    const auto pause = protocol->currentPauseId();
    lua_pop(L, 1); // DebugProtocol anchors the paused coroutine.
    REQUIRE(pause != DebugProtocol::NoPause);
    return pause;
}

} // namespace

TEST_CASE("Entry async: paused completions wait for resume and honour cancellation") {
    bool cancel = false;
    SUBCASE("resume delivers exactly once") {}
    SUBCASE("cancel while paused discards old completion") { cancel = true; }
    Engine engine(asyncEngineConfig());
    REQUIRE(engine.init());
    lua_State* L = engine.lua().state();
    REQUIRE(luaL_dostring(L,
        "async_count = 0; "
        "assert(Render.load_texture_async('__missing_async_pause__.png', function(ok) "
        "assert(not ok); async_count = async_count + 1 end) > 0)") == LUA_OK);
    settleAsyncJobs(engine);
    const auto pause = pauseAsyncEngine(engine);
    const auto commands = engine.debugProtocol()->commandSink();
    int tick = 0;
    engine.run([&] {
        ++tick;
        if (tick == 2) {
            CHECK(luaCount(L, "async_count") == 0);
            if (cancel) REQUIRE(luaL_dostring(L, "Render.cancel_async_loads()") == LUA_OK);
            REQUIRE(commands(pause, DebugProtocol::Command::Continue));
        } else if (tick == 3) {
            CHECK(luaCount(L, "async_count") == 0);
        } else if (tick == 5) {
            CHECK(luaCount(L, "async_count") == (cancel ? 0 : 1));
            engine.quit();
        }
    });
    CHECK(tick == 5);
    CHECK(luaL_dostring(L, "assert(next(_ASYNC_CALLBACKS) == nil)") == LUA_OK);
    engine.shutdown();
}

TEST_CASE("Entry async: full script reload discards old buffered closures") {
    Engine engine(asyncEngineConfig());
    REQUIRE(engine.init());
    lua_State* L = engine.lua().state();
    REQUIRE(luaL_dostring(L,
        "package.path = 'scripts/?.lua;scripts/?/init.lua;' .. package.path; "
        "old_async_count = 0; new_async_count = 0; "
        "assert(Render.load_texture_async('__missing_async_reload__.png', function() "
        "old_async_count = old_async_count + 1 end) > 0)") == LUA_OK);
    settleAsyncJobs(engine);
    REQUIRE(engine.reloadScriptsNow());
    CHECK(luaL_dostring(L, "assert(next(_ASYNC_CALLBACKS) == nil)") == LUA_OK);
    REQUIRE(luaL_dostring(L,
        "assert(Render.load_texture_async('__missing_async_reload__.png', function() "
        "new_async_count = new_async_count + 1 end) > 0)") == LUA_OK);
    settleAsyncJobs(engine);
    int ticks = 0;
    engine.run([&] { if (++ticks == 3) engine.quit(); });
    CHECK(luaCount(L, "old_async_count") == 0);
    CHECK(luaCount(L, "new_async_count") == 1);
    engine.shutdown();
}

TEST_CASE("Entry async: callback cancellation and reenqueue preserve the new closure") {
    Engine engine(asyncEngineConfig());
    REQUIRE(engine.init());
    lua_State* L = engine.lua().state();
    REQUIRE(luaL_dostring(L,
        "first_async_count = 0; stale_async_count = 0; next_async_count = 0; last_async_count = 0; "
        "assert(Render.load_texture_async('__missing_async_reentry__.png', function() "
        " first_async_count = first_async_count + 1; Render.cancel_async_loads(); "
        " assert(Render.load_texture_async('__missing_async_reentry__.png', function() "
        "  next_async_count = next_async_count + 1 end) > 0); "
        " assert(Render.load_texture_async('__missing_async_reentry__.png', function() "
        "  last_async_count = last_async_count + 1 end) > 0) end) > 0); "
        "assert(Render.load_texture_async('__missing_async_reentry__.png', function() "
        " stale_async_count = stale_async_count + 1 end) > 0)") == LUA_OK);
    settleAsyncJobs(engine);
    int ticks = 0;
    engine.run([&] {
        if (++ticks == 2) settleAsyncJobs(engine);
        if (ticks == 4) engine.quit();
    });
    CHECK(luaCount(L, "first_async_count") == 1);
    CHECK(luaCount(L, "stale_async_count") == 0);
    CHECK(luaCount(L, "next_async_count") == 1);
    CHECK(luaCount(L, "last_async_count") == 1);
    CHECK(luaL_dostring(L, "assert(next(_ASYNC_CALLBACKS) == nil)") == LUA_OK);
    engine.shutdown();
}


namespace {
struct EntryHostObservation {
    int marker = 0;
    EngineHostSnapshot host;
    AsyncLoaderSnapshot loader;
    JobSystemSnapshot jobs;
};
struct EntryHostProbe {
    Engine* engine = nullptr;
    std::vector<EntryHostObservation> observations;
};

void installEntryHostProbe(Engine& engine, EntryHostProbe& probe) {
    probe.engine = &engine;
    lua_State* L = engine.lua().state();
    lua_pushlightuserdata(L, &probe);
    lua_pushcclosure(L, [](lua_State* state) {
        auto& capture = *static_cast<EntryHostProbe*>(lua_touserdata(state, lua_upvalueindex(1)));
        auto* loader = BackendRegistry::instance().getAsyncLoader();
        auto* jobs = BackendRegistry::instance().getJobSystem();
        // Record inside the real Lua callback; assertions run after Lua returns.
        capture.observations.push_back({static_cast<int>(luaL_checkinteger(state, 1)),
            capture.engine->getHostSnapshot(),
            loader ? loader->getSnapshot() : AsyncLoaderSnapshot{},
            jobs ? jobs->getSnapshot() : JobSystemSnapshot{}});
        return 0;
    }, 1);
    lua_setglobal(L, "record_entry_host_snapshot");
}

void checkHostPayloads(const EngineHostSnapshot& state, uint64_t deferred,
                       uint64_t draining, uint64_t dispatching) {
    CHECK(state.supported);
    CHECK(state.deferredAsyncPayloads == deferred);
    CHECK(state.drainingAsyncPayloads == draining);
    CHECK(state.dispatchingAsyncPayloads == dispatching);
}

void checkDirectHostDispatch(const EntryHostObservation& observed,
                            int marker, uint64_t remaining) {
    CHECK(observed.marker == marker);
    CHECK(observed.host.initialized);
    CHECK(observed.host.running);
    CHECK_FALSE(observed.host.luaPaused);
    CHECK(observed.host.delivery == AsyncHostDelivery::DirectDrain);
    CHECK(observed.host.asyncOwnershipComplete);
    checkHostPayloads(observed.host, 0, remaining, 1);
    CHECK(observed.loader.supported);
    CHECK(observed.loader.pendingWaiters == 0);
    CHECK(observed.loader.inflightKeys == 0);
    CHECK(observed.loader.completedBuffered == 0);
    CHECK(observed.jobs.supported);
    CHECK(observed.jobs.workerPending == 0);
    CHECK(observed.jobs.queuedCompletions == 0);
    CHECK(observed.jobs.dispatchingCompletions == 0);
}
}

TEST_CASE("Host U27 snapshot: drained batches retain current and remaining payloads through Lua errors") {
    bool firstThrows = false;
    bool unregisteredFirst = false;
    SUBCASE("both callbacks return") {}
    SUBCASE("first callback reports a Lua error") { firstThrows = true; }
    SUBCASE("public enqueue without a Lua callback returns early before the batch") { unregisteredFirst = true; }
    EntryHostProbe probe; // Outlives Engine and every registered Lua closure.
    Engine engine(asyncEngineConfig());
    REQUIRE(engine.init());
    installEntryHostProbe(engine, probe);
    lua_State* L = engine.lua().state();
    lua_pushboolean(L, firstThrows);
    lua_setglobal(L, "host_first_throws");
    auto* loader = BackendRegistry::instance().getAsyncLoader();
    REQUIRE(loader != nullptr);
    if (unregisteredFirst)
        REQUIRE(loader->enqueue("__missing_host_batch__.png", "texture") > 0);
    REQUIRE(luaL_dostring(L,
        "host_first = 0; host_second = 0; "
        "assert(Render.load_texture_async('__missing_host_batch__.png', function(ok) "
        " assert(not ok); host_first = host_first + 1; record_entry_host_snapshot(1); "
        " if host_first_throws then error('U27 deliberate async callback error') end end) > 0); "
        "assert(Render.load_texture_async('__missing_host_batch__.png', function(ok) "
        " assert(not ok); host_second = host_second + 1; record_entry_host_snapshot(2) end) > 0)") == LUA_OK);
    settleAsyncJobs(engine);
    CHECK(loader->getSnapshot().completedBuffered == (unregisteredFirst ? 3u : 2u));
    CHECK(engine.jobSystem().getSnapshot().workerPending == 0);
    checkHostPayloads(engine.getHostSnapshot(), 0, 0, 0);
    int ticks = 0;
    engine.run([&] { if (++ticks == 2) engine.quit(); });
    REQUIRE(probe.observations.size() == 2);
    checkDirectHostDispatch(probe.observations[0], 1, 2);
    checkDirectHostDispatch(probe.observations[1], 2, 1);
    CHECK(luaCount(L, "host_first") == 1);
    CHECK(luaCount(L, "host_second") == 1);
    CHECK(luaL_dostring(L, "assert(next(_ASYNC_CALLBACKS) == nil)") == LUA_OK);
    checkHostPayloads(engine.getHostSnapshot(), 0, 0, 0);
    engine.shutdown();
    checkHostPayloads(engine.getHostSnapshot(), 0, 0, 0);
}

TEST_CASE("Host U27 snapshot: paused and cancelled payloads remain owned until disposal") {
    bool cancel = false;
    SUBCASE("resume dispatches the moved deferred batch") {}
    SUBCASE("cancel invalidates delivery but does not hide retained bytes") { cancel = true; }
    EntryHostProbe probe;
    Engine engine(asyncEngineConfig());
    REQUIRE(engine.init());
    installEntryHostProbe(engine, probe);
    lua_State* L = engine.lua().state();
    REQUIRE(luaL_dostring(L,
        "host_paused_callbacks = 0; "
        "for i = 1, 2 do local marker = i; "
        " assert(Render.load_texture_async('__missing_host_pause__.png', function(ok) "
        " assert(not ok); host_paused_callbacks = host_paused_callbacks + 1; "
        " record_entry_host_snapshot(marker) end) > 0) end") == LUA_OK);
    settleAsyncJobs(engine);
    const auto pause = pauseAsyncEngine(engine);
    const auto commands = engine.debugProtocol()->commandSink();
    int tick = 0;
    engine.run([&] {
        ++tick;
        if (tick == 2) {
            const auto paused = engine.getHostSnapshot();
            CHECK(paused.initialized);
            CHECK(paused.running);
            CHECK(paused.luaPaused);
            CHECK(paused.asyncOwnershipComplete);
            checkHostPayloads(paused, 2, 0, 0);
            auto* loader = BackendRegistry::instance().getAsyncLoader();
            REQUIRE(loader != nullptr);
            CHECK(loader->getSnapshot().completedBuffered == 0);
            CHECK(loader->getSnapshot().pendingWaiters == 0);
            CHECK(probe.observations.empty());
            if (cancel) {
                REQUIRE(luaL_dostring(L, "Render.cancel_async_loads()") == LUA_OK);
                // cancelAll invalidates generation; the Engine still owns these two objects.
                checkHostPayloads(engine.getHostSnapshot(), 2, 0, 0);
            }
            REQUIRE(commands(pause, DebugProtocol::Command::Continue));
        } else if (tick == 5) {
            engine.quit();
        }
    });
    CHECK(tick == 5);
    CHECK(luaCount(L, "host_paused_callbacks") == (cancel ? 0 : 2));
    REQUIRE(probe.observations.size() == (cancel ? 0u : 2u));
    if (!cancel) {
        checkDirectHostDispatch(probe.observations[0], 1, 2);
        checkDirectHostDispatch(probe.observations[1], 2, 1);
    }
    checkHostPayloads(engine.getHostSnapshot(), 0, 0, 0);
    CHECK(luaL_dostring(L, "assert(next(_ASYNC_CALLBACKS) == nil)") == LUA_OK);
    engine.shutdown();
    checkHostPayloads(engine.getHostSnapshot(), 0, 0, 0);
}

TEST_CASE("Host U27 snapshot: callback cancellation cannot hide its old batch or new generation") {
    EntryHostProbe probe;
    Engine engine(asyncEngineConfig());
    REQUIRE(engine.init());
    installEntryHostProbe(engine, probe);
    lua_State* L = engine.lua().state();
    REQUIRE(luaL_dostring(L,
        "host_stale_callbacks = 0; host_fresh_callbacks = 0; "
        "assert(Render.load_texture_async('__missing_host_reentry__.png', function() "
        " record_entry_host_snapshot(1); Render.cancel_async_loads(); "
        " record_entry_host_snapshot(2); "
        " assert(Render.load_texture_async('__missing_host_reentry__.png', function() "
        "  host_fresh_callbacks = host_fresh_callbacks + 1; record_entry_host_snapshot(3) end) > 0) end) > 0); "
        "assert(Render.load_texture_async('__missing_host_reentry__.png', function() "
        " host_stale_callbacks = host_stale_callbacks + 1 end) > 0)") == LUA_OK);
    settleAsyncJobs(engine);
    int ticks = 0;
    engine.run([&] {
        if (++ticks == 2) settleAsyncJobs(engine);
        if (ticks == 4) engine.quit();
    });
    REQUIRE(probe.observations.size() == 3);
    checkDirectHostDispatch(probe.observations[0], 1, 2);
    checkDirectHostDispatch(probe.observations[1], 2, 2);
    checkDirectHostDispatch(probe.observations[2], 3, 1);
    CHECK(luaCount(L, "host_stale_callbacks") == 0);
    CHECK(luaCount(L, "host_fresh_callbacks") == 1);
    checkHostPayloads(engine.getHostSnapshot(), 0, 0, 0);
    engine.shutdown();
}

TEST_CASE("Host U27 snapshot: reload and safe owner shutdown release deferred payloads") {
    bool reload = false;
    SUBCASE("owner shutdown while paused") {}
    SUBCASE("reload after resume before deferred dispatch") { reload = true; }
    EntryHostProbe probe;
    Engine engine(asyncEngineConfig());
    REQUIRE(engine.init());
    installEntryHostProbe(engine, probe);
    lua_State* L = engine.lua().state();
    REQUIRE(luaL_dostring(L,
        "package.path = 'scripts/?.lua;scripts/?/init.lua;' .. package.path; "
        "for i = 1, 2 do assert(Render.load_texture_async('__missing_host_clear__.png', "
        " function() record_entry_host_snapshot(99) end) > 0) end") == LUA_OK);
    settleAsyncJobs(engine);
    const auto pause = pauseAsyncEngine(engine);
    const auto commands = engine.debugProtocol()->commandSink();
    int ticks = 0;
    engine.run([&] {
        ++ticks;
        if (ticks == 2) {
            checkHostPayloads(engine.getHostSnapshot(), 2, 0, 0);
            CHECK(probe.observations.empty());
            if (reload) {
                REQUIRE(commands(pause, DebugProtocol::Command::Continue));
            } else {
                // Owner pump is outside a Lua call: closing the Lua state is safe.
                engine.shutdown();
                checkHostPayloads(engine.getHostSnapshot(), 0, 0, 0);
            }
        } else if (ticks == 3 && reload) {
            CHECK_FALSE(engine.getHostSnapshot().luaPaused);
            checkHostPayloads(engine.getHostSnapshot(), 2, 0, 0);
            REQUIRE(engine.reloadScriptsNow());
            checkHostPayloads(engine.getHostSnapshot(), 0, 0, 0);
            engine.quit();
        } else if (ticks > 3) {
            engine.quit(); // Deterministic fixture bound, never a readiness signal.
        }
    });
    CHECK(ticks == (reload ? 3 : 2));
    CHECK(probe.observations.empty());
    engine.shutdown();
    const auto stopped = engine.getHostSnapshot();
    CHECK_FALSE(stopped.initialized);
    CHECK_FALSE(stopped.running);
    checkHostPayloads(stopped, 0, 0, 0);
}

TEST_CASE("Host U27 snapshot: ordinary SDL publication is explicitly incomplete ownership") {
    struct Events {
        Events() { REQUIRE(SDL_InitSubSystem(SDL_INIT_EVENTS)); }
        ~Events() { SDL_QuitSubSystem(SDL_INIT_EVENTS); }
    } events;
    EngineConfig config;
    config.headless = false;
    config.editorMode = false;
    config.render = new NullRenderDevice();
    config.audio = new NullAudioBackend();
    config.platform = new NullPlatformBackend();
    config.gpuMonitor = new NullGpuMonitor();
    Engine engine(std::move(config));
    REQUIRE(engine.init());
    auto* loader = BackendRegistry::instance().getAsyncLoader();
    REQUIRE(loader != nullptr);
    REQUIRE(loader->enqueue("__missing_host_sdl__.png", "texture") > 0);
    settleAsyncJobs(engine);
    REQUIRE(loader->getSnapshot().completedBuffered == 1);
    REQUIRE(loader->poll()); // Actual production SDL publication transfers the payload.
    CHECK(loader->getSnapshot().pendingWaiters == 0);
    CHECK(loader->getSnapshot().completedBuffered == 0);
    struct QueuedOwner { IAsyncLoader* owner; int count = 0; } queued{loader};
    const auto inspect = [](void* userdata, SDL_Event* event) -> bool {
        auto& own = *static_cast<QueuedOwner*>(userdata);
        if (event->type == CAESURA_EVENT_ASYNC_LOAD && event->user.data2 == own.owner)
            ++own.count;
        return true; // Observe only; preserve every owner's original queued event.
    };
    SDL_FilterEvents(inspect, &queued);
    REQUIRE(queued.count == 1);
    const auto state = engine.getHostSnapshot();
    CHECK(state.initialized);
    CHECK(state.delivery == AsyncHostDelivery::SdlEvents);
    CHECK_FALSE(state.asyncOwnershipComplete);
    checkHostPayloads(state, 0, 0, 0); // Visible zeroes cannot certify the SDL queue idle.
    engine.shutdown(); // Production cancellation reclaims its still-queued payload.
    queued.count = 0;
    SDL_FilterEvents(inspect, &queued);
    CHECK(queued.count == 0);
    CHECK_FALSE(engine.getHostSnapshot().asyncOwnershipComplete);
}
