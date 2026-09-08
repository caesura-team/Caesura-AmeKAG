#include "doctest.h"
#include "entry/Engine.h"
#include "entry/EngineConfig.h"
#include "di/BackendRegistry.h"
#include "di/api/ISandboxQuota.h"
#include "job/api/IJobSystem.h"
#include "resource/api/IAsyncLoader.h"
#include "EntryLifecycleBackends.h"
#include "TestPaths.h"
#include "U10SessionFixture.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace Caesura;
using u10_test::runLua;

namespace {

// A valid CPU-decodable 1x1 TGA, in the Engine's existing loose-asset root.
// The unique relative name keeps production path validation and CWD intact.
class SessionImage {
public:
    SessionImage()
        : name(TestPaths::uniqueTempDir("u10_session").filename().string() + ".tga") {
        constexpr unsigned char pixel[] = {
            0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 24, 0x20, 0, 0, 255
        };
        REQUIRE_FALSE(std::filesystem::exists(name));
        std::ofstream output(name, std::ios::binary);
        output.write(reinterpret_cast<const char*>(pixel), sizeof(pixel));
        REQUIRE(output.good());
    }
    ~SessionImage() {
        std::error_code ignored;
        std::filesystem::remove(name, ignored);
    }
    const std::string name;
};

// Count only texture reservations, excluding all other subsystem quotas.
// loadTextureFromRGBA reserves before its headless guard; the guard rejects
// before bgfx allocation. These counts observe materialization attempts.
class TextureQuotaProbe final : public ISandboxQuota {
public:
    explicit TextureQuotaProbe(Test::ServiceProbe& probe) : m_probe(probe) {}
    ~TextureQuotaProbe() override { ++m_probe.destructorCalls; }
    void setLuaState(lua_State* state) override {
        ++m_probe.setLuaStateCalls;
        m_probe.lastLuaState = state;
        if (m_probe.onSetLuaState) m_probe.onSetLuaState(state);
    }
    bool tryAlloc(const char* category) override {
        if (std::strcmp(category, "textures") == 0) ++m_probe.tryAllocCalls;
        return true;
    }
    void release(const char* category) override {
        if (std::strcmp(category, "textures") == 0) ++m_probe.releaseCalls;
    }
    int count(const char*) override { return 0; }
    int maxLimit(const char*) override { return 0; }
private:
    Test::ServiceProbe& m_probe;
};

// pendingJobs becomes zero after workers have queued their completions.
// Unlike waitIdle(), this barrier does not run main-thread completions.
bool sessionWorkersFinished(IJobSystem& jobs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (jobs.pendingJobs() != 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    return jobs.pendingJobs() == 0;
}

void initNativeRunner(lua_State* state, const SessionImage& image) {
    lua_pushlstring(state, image.name.data(), image.name.size());
    lua_setglobal(state, "u10_image");
    runLua(state, R"lua(
        package.path = 'scripts/?.lua;scripts/?/init.lua;' .. package.path
        -- Scene contents are deterministic; all backend bindings, native
        -- async I/O, decoder, workers, runner and Operation remain real.
        package.loaded.flow = {load_scene = function(path)
            return {path = path, labels = {},
                tokens = require('tokenizer').parse('[wait time=60000][end]')}
        end}
        require('kag')
        runner = require('kag_runner')
        runner.set_resume_adapter({
            is_paused = function() return false end,
            resume = function(_, co, value)
                u10_last_co = co
                return coroutine.resume(co, value)
            end,
        })
        u10_weak_callbacks = setmetatable({}, {__mode = 'v'})
        u10_a_callbacks, u10_b_callbacks = 0, 0
        local operation = require('kag.operation')
        local probe = operation.start({active_operations = {}})
        probe:complete()
        probe:__close()
        local scope = getmetatable(probe)
        local original_close = scope.__close
        u10_close_calls = {}
        scope.__close = function(self, error_value)
            u10_close_calls[self.token] = (u10_close_calls[self.token] or 0) + 1
            if u10_native_event then u10_native_event(1) end
            return original_close(self, error_value)
        end
    )lua");
}

void checkReplacedSessionLoad(bool buffered, bool oldSuccess) {
    SessionImage image;
    Test::LifecycleProbe render;
    Test::ServiceProbe quota;
    EngineConfig config;
    config.headless = true;
    config.render = new Test::RenderDevice(render);
    config.sandboxQuota = new TextureQuotaProbe(quota);
    Engine engine(std::move(config));
    REQUIRE(engine.init());
    auto* state = engine.lua().state();
    initNativeRunner(state, image);
    lua_pushboolean(state, oldSuccess);
    lua_setglobal(state, "u10_old_success");
    runLua(state, R"lua(
        assert(runner.start('A.ks'))
        u10_a, u10_a_co = runner.get_ctx(), u10_last_co
        u10_a_token = assert(u10_a.active_operations[1])
        local callback = function() u10_a_callbacks = u10_a_callbacks + 1 end
        u10_weak_callbacks[1] = callback
        local path = u10_old_success and u10_image or (u10_image .. '.missing')
        assert(Render.load_texture_async(path, callback) > 0)
    )lua");
    auto& jobs = engine.jobSystem();
    auto* loader = BackendRegistry::instance().getAsyncLoader();
    REQUIRE(loader != nullptr);
    REQUIRE(sessionWorkersFinished(jobs));
    if (buffered) jobs.pollMainThreadJobs();
    REQUIRE(loader->pendingCount() == 1);
    const int allocations = quota.tryAllocCalls;
    const int releases = quota.releaseCalls;
    runLua(state, R"lua(
        assert(u10_a_callbacks == 0)
        assert(runner.stop())
        assert(coroutine.status(u10_a_co) == 'dead' and u10_a_token.cancelled)
        assert(u10_close_calls[u10_a_token] == 1 and #u10_a.active_operations == 0)
        assert(next(_ASYNC_CALLBACKS) == nil)
        collectgarbage('collect')
        assert(u10_weak_callbacks[1] == nil, 'A callback registry reference leaked')
        assert(runner.start('B.ks'))
        u10_b, u10_b_co = runner.get_ctx(), u10_last_co
        local callback = function(ok, path, texture)
            u10_b_callbacks = u10_b_callbacks + 1
            u10_b_result = {ok = ok, path = path, texture = texture}
        end
        u10_weak_callbacks[2] = callback
        assert(Render.load_texture_async(u10_image, callback) > 0)
    )lua");
    REQUIRE(sessionWorkersFinished(jobs));
    jobs.pollMainThreadJobs(); // Deliver late A and fresh B completion closures.
    REQUIRE(loader->pendingCount() == 1);
    CHECK(quota.tryAllocCalls == allocations);
    int ticks = 0;
    engine.run([&] { if (++ticks == 3) engine.quit(); });
    CHECK(ticks == 3);
    runLua(state, R"lua(
        assert(u10_a_callbacks == 0 and u10_b_callbacks == 1)
        assert(u10_b_result.path == u10_image)
        -- Real headless TextureManager rejects a valid decode before GPU I/O.
        assert(u10_b_result.ok == false and u10_b_result.texture == 0)
        assert(rawequal(runner.get_ctx(), u10_b) and rawequal(_CAESURA_CTX, u10_b))
        assert(coroutine.status(u10_b_co) == 'suspended')
        assert(#u10_b.active_operations == 1 and not u10_b.active_operations[1].cancelled)
        assert(next(_ASYNC_CALLBACKS) == nil)
        collectgarbage('collect')
        assert(u10_weak_callbacks[1] == nil and u10_weak_callbacks[2] == nil)
    )lua");
    CHECK(loader->pendingCount() == 0);
    // B is a positive control: exactly one valid decode reaches the texture
    // materialization entry and releases its rejected headless reservation.
    CHECK(quota.tryAllocCalls == allocations + 1);
    CHECK(quota.releaseCalls == releases + 1);
    engine.shutdown();
}

void checkRollbackSessionLoad(bool buffered, bool oldSuccess) {
    SessionImage image;
    Test::LifecycleProbe render;
    Test::ServiceProbe quota;
    EngineConfig config;
    config.headless = true;
    config.render = new Test::RenderDevice(render);
    config.sandboxQuota = new TextureQuotaProbe(quota);
    Engine engine(std::move(config));
    REQUIRE(engine.init());
    auto* state = engine.lua().state();
    initNativeRunner(state, image);
    lua_pushboolean(state, oldSuccess);
    lua_setglobal(state, "u12_old_success");
    runLua(state, R"lua(
        package.loaded.flow.load_scene = function(path)
            return {path = path, labels = {}, tokens = require('tokenizer').parse(
                '[p][wait time=60000][end]')}
        end
        assert(runner.start('rollback.ks'))
        runner.get_ctx().f.marker = 'historical'
        assert(runner.on_click())
        u12_owner, u12_old_co = runner.get_ctx(), u10_last_co
        assert(#u12_owner._undoStack == 1, 'must create history through public click')
        u12_old_token = assert(u12_owner.active_operations[1])
        u12_owner.f.marker = 'future'
        u12_old_calls, u12_new_calls = 0, 0
        local callback = function()
            u12_old_calls = u12_old_calls + 1
            runner.get_ctx().f.marker = 'obsolete completion'
        end
        u10_weak_callbacks[1] = callback
        local path = u12_old_success and u10_image or (u10_image .. '.missing')
        assert(Render.load_texture_async(path, callback) > 0)
    )lua");
    auto& jobs = engine.jobSystem();
    auto* loader = BackendRegistry::instance().getAsyncLoader();
    REQUIRE(loader != nullptr);
    // Both modes use real worker I/O/decode. One leaves the completion in the
    // JobSystem; the other transfers it to the loader before rollback.
    REQUIRE(sessionWorkersFinished(jobs));
    if (buffered) jobs.pollMainThreadJobs();
    REQUIRE(loader->pendingCount() == 1);
    const int allocations = quota.tryAllocCalls;
    const int releases = quota.releaseCalls;
    runLua(state, R"lua(
        assert(u12_old_calls == 0)
        assert(runner.rollback())
        assert(coroutine.status(u12_old_co) == 'dead' and u12_old_token.cancelled)
        assert(u10_close_calls[u12_old_token] == 1)
        assert(#runner.get_ctx()._undoStack == 0)
        assert(runner.get_ctx().f.marker == 'historical')
        assert(runner.get_ctx().waiting_input and runner.get_ctx()._rollback_waiting)
        assert(next(_ASYNC_CALLBACKS) == nil)
        collectgarbage('collect')
        assert(u10_weak_callbacks[1] == nil, 'rollback must release obsolete closure')
        local callback = function(ok, path, texture)
            u12_new_calls = u12_new_calls + 1
            u12_new_result = {ok = ok, path = path, texture = texture}
        end
        u10_weak_callbacks[2] = callback
        assert(Render.load_texture_async(u10_image, callback) > 0)
    )lua");
    REQUIRE(sessionWorkersFinished(jobs));
    jobs.pollMainThreadJobs();
    CHECK(loader->pendingCount() == 1);
    CHECK(quota.tryAllocCalls == allocations);
    int ticks = 0;
    engine.run([&] { if (++ticks == 3) engine.quit(); });
    CHECK(ticks == 3);
    runLua(state, R"lua(
        assert(u12_old_calls == 0 and u12_new_calls == 1)
        assert(u12_new_result.path == u10_image)
        assert(u12_new_result.ok == false and u12_new_result.texture == 0)
        assert(rawequal(runner.get_ctx(), _CAESURA_CTX))
        assert(runner.get_ctx().f.marker == 'historical')
        assert(runner.get_ctx().waiting_input and runner.get_ctx()._rollback_waiting)
        assert(next(_ASYNC_CALLBACKS) == nil)
        collectgarbage('collect')
        assert(u10_weak_callbacks[1] == nil and u10_weak_callbacks[2] == nil)
        assert(runner.on_click(), 'manual continuation remains available')
        assert(runner.get_ctx().f.marker == 'historical')
        assert(not runner.get_ctx()._rollback_waiting)
    )lua");
    CHECK(loader->pendingCount() == 0);
    CHECK(quota.tryAllocCalls == allocations + 1);
    CHECK(quota.releaseCalls == releases + 1);
    engine.shutdown();
}

struct ShutdownObservation {
    Test::LifecycleProbe& render;
    Test::ServiceProbe& layers;
    IRenderDevice* renderer = nullptr;
    ILayerManager* layerManager = nullptr;
    IJobSystem* jobs = nullptr;
    std::vector<int> events;
    bool allCleanupsBeforeTeardown = true;
    bool cancelledScopeObserved = false;
    int cleanupRequest = 0;
    int callbacks = 0;

    bool backendsAreLive() const {
        auto& registry = BackendRegistry::instance();
        return render.beginShutdownCalls == 0 && render.shutdownCalls == 0
            && render.destructorCalls == 0 && layers.shutdownCalls == 0
            && layers.destructorCalls == 0 && registry.getRenderDevice() == renderer
            && registry.getLayerManager() == layerManager && registry.getJobSystem() == jobs
            && jobs && jobs->isRunning() && jobs->workerCount() > 0;
    }
    static int record(lua_State* state) {
        auto& probe = *static_cast<ShutdownObservation*>(
            lua_touserdata(state, lua_upvalueindex(1)));
        const int event = static_cast<int>(lua_tointeger(state, 1));
        probe.events.push_back(event);
        if (event == 3) { ++probe.callbacks; return 0; }
        probe.allCleanupsBeforeTeardown &= probe.backendsAreLive();
        if (event == 2) {
            probe.cleanupRequest = static_cast<int>(lua_tointeger(state, 2));
            probe.cancelledScopeObserved = lua_toboolean(state, 3) != 0;
        }
        return 0;
    }
};

} // namespace

TEST_CASE("Entry U10: stop rejects old worker completions after a successor starts") {
    bool oldSuccess = true;
    SUBCASE("old successful decode") {}
    SUBCASE("old missing asset") { oldSuccess = false; }
    checkReplacedSessionLoad(false, oldSuccess);
}

TEST_CASE("Entry U10: stop discards loader results buffered before Engine consumption") {
    bool oldSuccess = true;
    SUBCASE("old successful decode") {}
    SUBCASE("old missing asset") { oldSuccess = false; }
    checkReplacedSessionLoad(true, oldSuccess);
}

TEST_CASE("Entry U12: rollback rejects obsolete native loads and accepts a fresh load") {
    bool buffered = false;
    bool oldSuccess = true;
    SUBCASE("successful decode delivered after rollback") {}
    SUBCASE("missing asset delivered after rollback") { oldSuccess = false; }
    SUBCASE("successful decode buffered before rollback") { buffered = true; }
    SUBCASE("missing asset buffered before rollback") { buffered = true; oldSuccess = false; }
    checkRollbackSessionLoad(buffered, oldSuccess);
}

TEST_CASE("Entry U10: shutdown closes runner scopes before backends and cancels cleanup loads") {
    SessionImage image;
    Test::LifecycleProbe render;
    Test::ServiceProbe layers, quota;
    ShutdownObservation observation{render, layers};
    bool stateVerifiedBeforeClose = false;
    std::string stateError;
    lua_State* liveState = nullptr;
    quota.onSetLuaState = [&](lua_State* state) {
        if (state) { liveState = state; return; }
        const int top = lua_gettop(liveState);
        const int result = luaL_dostring(liveState, R"lua(
            assert(runner.get_ctx() == nil and _CAESURA_CTX == nil)
            assert(coroutine.status(u10_a_co) == 'dead' and u10_a_token.cancelled)
            assert(#u10_a.active_operations == 0 and #u10_a_token.callbacks == 0)
            assert(u10_close_calls[u10_a_token] == 1)
            assert(next(_ASYNC_CALLBACKS) == nil)
            collectgarbage('collect')
            assert(u10_weak_callbacks[1] == nil and u10_weak_callbacks[2] == nil)
        )lua");
        stateVerifiedBeforeClose = result == LUA_OK;
        if (result != LUA_OK) {
            const char* message = lua_tostring(liveState, -1);
            stateError = message ? message : "non-string Lua error";
        }
        lua_settop(liveState, top);
    };
    EngineConfig config;
    config.headless = true;
    config.render = new Test::RenderDevice(render);
    config.layerManager = new Test::LayerManagerBackend(layers);
    config.sandboxQuota = new TextureQuotaProbe(quota);
    Engine engine(std::move(config));
    REQUIRE(engine.init());
    auto* state = engine.lua().state();
    initNativeRunner(state, image);
    observation.renderer = BackendRegistry::instance().getRenderDevice();
    observation.layerManager = BackendRegistry::instance().getLayerManager();
    observation.jobs = &engine.jobSystem();
    lua_pushlightuserdata(state, &observation);
    lua_pushcclosure(state, &ShutdownObservation::record, 1);
    lua_setglobal(state, "u10_native_event");
    runLua(state, R"lua(
        assert(runner.start('A.ks'))
        u10_a, u10_a_co = runner.get_ctx(), u10_last_co
        u10_a_token = assert(u10_a.active_operations[1])
        local callback = function() u10_native_event(3) end
        u10_weak_callbacks[1] = callback
        assert(Render.load_texture_async(u10_image, callback) > 0)
        u10_a_token:register(function()
            local late_callback = function() u10_native_event(3) end
            u10_weak_callbacks[2] = late_callback
            local request = Render.load_texture_async(u10_image, late_callback)
            u10_native_event(2, request, u10_a_token.cancelled
                and #u10_a.active_operations == 0 and u10_close_calls[u10_a_token] == 1)
        end)
    )lua");
    REQUIRE(sessionWorkersFinished(*observation.jobs));
    const int allocations = quota.tryAllocCalls;
    const int releases = quota.releaseCalls;
    engine.shutdown();
    CHECK(observation.events == std::vector<int>{1, 2});
    CHECK(observation.allCleanupsBeforeTeardown);
    CHECK(observation.cancelledScopeObserved);
    CHECK(observation.cleanupRequest > 0);
    CHECK(observation.callbacks == 0);
    CHECK_MESSAGE(stateVerifiedBeforeClose, stateError);
    CHECK(quota.tryAllocCalls == allocations);
    CHECK(quota.releaseCalls == releases);
    CHECK(render.beginShutdownCalls == 1);
    CHECK(render.shutdownCalls == 1);
    CHECK(layers.shutdownCalls == 1);
    CHECK_FALSE(observation.jobs->isRunning());
    CHECK(observation.jobs->workerCount() == 0);
    CHECK(observation.jobs->pendingJobs() == 0);
    CHECK(BackendRegistry::instance().getJobSystem() == nullptr);
    engine.shutdown();
    CHECK(observation.events == std::vector<int>{1, 2});
    CHECK(render.shutdownCalls == 1);
    CHECK(layers.shutdownCalls == 1);
}
