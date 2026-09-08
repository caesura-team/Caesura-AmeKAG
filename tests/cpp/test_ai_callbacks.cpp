#include "doctest.h"
#include "di/BackendRegistry.h"
#include "job/api/IJobSystem.h"
#include "script/bindings/AIBinding.h"
#include "mocks/NullJobSystem.h"
#include "entry/Engine.h"
#include "entry/EngineConfig.h"
#include "EntryLifecycleBackends.h"
#include "U10SessionFixture.h"
#include <httplib.h>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

namespace {

// Execute the real HTTP worker and its completion separately, so cancellation
// on either side of worker completion never depends on timing or sleeps.
class ControlledAiJobs final : public Caesura::IJobSystem {
public:
    struct Pending {
        Caesura::JobFn work;
        Caesura::MainThreadFn complete;
    };
    ControlledAiJobs()
        : previous(Caesura::BackendRegistry::instance().getJobSystem()) {
        Caesura::BackendRegistry::instance().setJobSystem(this);
    }
    ~ControlledAiJobs() override {
        Caesura::BackendRegistry::instance().setJobSystem(previous);
    }
    void init() override { running = true; }
    void shutdown() override { running = false; }
    uint64_t submit(Caesura::JobFn work, Caesura::JobPriority,
                    Caesura::MainThreadFn complete) override {
        if (!running || !work) return 0;
        pending.push_back({std::move(work), std::move(complete)});
        return pending.size();
    }
    void pollMainThreadJobs() override {}
    void waitIdle() override {}
    int workerCount() const override { return 0; }
    int pendingJobs() const override { return static_cast<int>(pending.size()); }
    bool isRunning() const override { return running; }
    void work(size_t index) {
        REQUIRE(index < pending.size());
        const auto callback = pending[index].work;
        callback();
    }
    void complete(size_t index) {
        REQUIRE(index < pending.size());
        // Copy before calling: callback code can append another job.
        const auto callback = pending[index].complete;
        REQUIRE(static_cast<bool>(callback));
        callback();
    }

    std::vector<Pending> pending;
    bool running = true;
    Caesura::IJobSystem* previous;
};

using LuaState = std::unique_ptr<lua_State, decltype(&lua_close)>;

LuaState makeAiLua() {
    LuaState state(luaL_newstate(), lua_close);
    REQUIRE(state != nullptr);
    luaL_openlibs(state.get());
    Caesura::registerAIBinding(state.get());
    return state;
}

void run(lua_State* state, const char* source) {
    const int top = lua_gettop(state);
    int result = luaL_loadstring(state, source);
    if (result == LUA_OK) result = lua_pcall(state, 0, 0, 0);
    const char* message = result == LUA_OK ? nullptr : lua_tostring(state, -1);
    const std::string error = message ? message : "none";
    INFO("Lua error: " << error);
    INFO("Lua source: " << source);
    REQUIRE(result == LUA_OK);
    REQUIRE(lua_gettop(state) == top);
}

class AiLoopback {
public:
    AiLoopback() {
        server.Post("/v1/chat/completions", [](const httplib::Request&,
                                                httplib::Response& response) {
            response.set_content(
                R"({"choices":[{"message":{"content":"fixture reply"}}]})",
                "application/json");
        });
        port = server.bind_to_any_port("127.0.0.1");
        REQUIRE(port > 0);
        listener = std::thread([this]() { server.listen_after_bind(); });
        server.wait_until_ready();
    }
    ~AiLoopback() {
        server.stop();
        if (listener.joinable()) listener.join();
    }
    void configure(lua_State* state) const {
        const auto source = "config = {ai = {endpoint = 'http://127.0.0.1:"
            + std::to_string(port) + "/v1', model = 'fixture', timeout_ms = 3000}}";
        run(state, source.c_str());
    }

private:
    httplib::Server server;
    std::thread listener;
    int port = 0;
};

// The first real HTTP response can straddle rollback. The barrier observes
// server admission and withholds that response; no guessed sleep races the
// runner against the worker. Later requests always receive a fresh reply.
class RollbackAiLoopback {
public:
    RollbackAiLoopback(bool holdOld, bool oldSuccess)
        : released(!holdOld), oldSucceeds(oldSuccess) {
        server.Post("/v1/chat/completions", [this](const httplib::Request&,
                                                  httplib::Response& response) {
            bool obsolete = false;
            {
                std::unique_lock<std::mutex> lock(mutex);
                obsolete = ++requests == 1;
                changed.notify_all();
                if (obsolete && !changed.wait_for(lock, std::chrono::seconds(10),
                                                  [this] { return released; }))
                    barrierTimedOut = true;
            }
            if (obsolete && !oldSucceeds) {
                response.status = 503;
                response.set_content("obsolete failure", "text/plain");
            } else {
                const std::string reply = obsolete ? "obsolete reply" : "current reply";
                response.set_content("{\"choices\":[{\"message\":{\"content\":\""
                    + reply + "\"}}]}", "application/json");
            }
        });
        port = server.bind_to_any_port("127.0.0.1");
        REQUIRE(port > 0);
        listener = std::thread([this] { server.listen_after_bind(); });
        server.wait_until_ready();
    }
    ~RollbackAiLoopback() {
        releaseOld();
        server.stop();
        if (listener.joinable()) listener.join();
    }
    bool waitForRequests(int count) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(5),
                                [this, count] { return requests >= count; });
    }
    void releaseOld() {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        changed.notify_all();
    }
    bool barrierFailed() {
        std::lock_guard<std::mutex> lock(mutex);
        return barrierTimedOut;
    }
    void configure(lua_State* state) const {
        // Runner startup requires config for accessibility. Initialize that real
        // module first so its defaults cannot overwrite the loopback endpoint.
        const auto source = "package.path = 'scripts/?.lua;scripts/?/init.lua;' .. package.path; "
            "config = require('config'); u12_ai_endpoint = 'http://127.0.0.1:"
            + std::to_string(port) + "/v1'; config.ai = {endpoint = u12_ai_endpoint, "
            "model = 'fixture', timeout_ms = 10000}";
        run(state, source.c_str());
    }

private:
    httplib::Server server;
    std::thread listener;
    std::mutex mutex;
    std::condition_variable changed;
    int port = 0;
    int requests = 0;
    bool released;
    const bool oldSucceeds;
    bool barrierTimedOut = false;
};

bool aiWorkersFinished(Caesura::IJobSystem& jobs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (jobs.pendingJobs() != 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    return jobs.pendingJobs() == 0;
}

void startRollbackAiScene(lua_State* state) {
    run(state, R"lua(
        package.path = 'scripts/?.lua;scripts/?/init.lua;' .. package.path
        package.loaded.flow = {load_scene = function(path)
            return {path = path, labels = {}, tokens = require('tokenizer').parse(
                '[p][ai_dialog prompt="request" name="fixture" '
                .. 'fallback="obsolete fallback" max_wait_ms=60000][p][end]')}
        end}
        require('kag')
        runner = require('kag_runner')
        runner.set_resume_adapter({
            is_paused = function() return false end,
            resume = function(_, co, value)
                u12_last_co = co
                return coroutine.resume(co, value)
            end,
        })
        -- Observe the real callback entry without replacing native AI I/O.
        local native_query = AI.query_async
        u12_ai_requests, u12_ai_calls = 0, {}
        u12_ai_weak = setmetatable({}, {__mode = 'v'})
        AI.query_async = function(prompt, opts, callback)
            assert(config.ai.endpoint == u12_ai_endpoint,
                'runner startup must preserve the loopback endpoint')
            u12_ai_requests = u12_ai_requests + 1
            local request = u12_ai_requests
            u12_ai_calls[request] = 0
            local observed = function(...)
                u12_ai_calls[request] = u12_ai_calls[request] + 1
                return callback(...)
            end
            u12_ai_weak[request] = observed
            local submitted = native_query(prompt, opts, observed)
            assert(submitted, 'the real JobSystem must accept the AI request')
            return submitted
        end
        assert(runner.start('ai-rollback.ks'))
        runner.get_ctx().f.marker = 'historical'
        assert(runner.on_click())
        assert(#runner.get_ctx()._undoStack == 1, 'history must come from a public click')
        assert(u12_ai_requests == 1)
        u12_old_co = u12_last_co
        assert(coroutine.status(u12_old_co) == 'suspended')
        assert(not runner.get_ctx().waiting_input and next(_AI_CALLBACKS) ~= nil,
            'the scene must be suspended inside the real AI wait')
        runner.get_ctx().f.marker = 'future'
    )lua");
}

void checkRollbackAiResponse(bool buffered, bool oldSuccess) {
    Caesura::Test::LifecycleProbe render;
    Caesura::EngineConfig config;
    config.headless = true;
    config.render = new Caesura::Test::RenderDevice(render);
    Caesura::Engine engine(std::move(config));
    REQUIRE(engine.init());
    // Destroy the barrier before Engine joins workers if an assertion fails.
    RollbackAiLoopback server(!buffered, oldSuccess);
    auto* state = engine.lua().state();
    server.configure(state);
    startRollbackAiScene(state);
    auto& jobs = engine.jobSystem();
    REQUIRE(server.waitForRequests(1));
    if (buffered) REQUIRE(aiWorkersFinished(jobs));
    else CHECK(jobs.pendingJobs() > 0);
    run(state, R"lua(
        assert(u12_ai_calls[1] == 0 and #runner.get_ctx().backlog == 0)
        assert(runner.rollback())
        assert(coroutine.status(u12_old_co) == 'dead')
        assert(#runner.get_ctx()._undoStack == 0)
        assert(runner.get_ctx().f.marker == 'historical')
        assert(next(_AI_CALLBACKS) == nil)
        collectgarbage('collect')
        assert(u12_ai_weak[1] == nil, 'rollback must release the old AI closure')
        local progressed, reason = runner.update(0.1)
        assert(not progressed and reason == 'waiting-input')
        assert(u12_ai_requests == 1 and #runner.get_ctx().backlog == 0)
        assert(runner.on_click(), 'the restored click must start a fresh AI request')
        assert(u12_ai_requests == 2)
    )lua");
    REQUIRE(server.waitForRequests(2));
    server.releaseOld();
    REQUIRE(aiWorkersFinished(jobs));
    CHECK_FALSE(server.barrierFailed());
    jobs.pollMainThreadJobs();
    run(state, R"lua(
        assert(u12_ai_calls[1] == 0 and u12_ai_calls[2] == 1)
        runner.update(0.016)
        local ctx = runner.get_ctx()
        assert(rawequal(ctx, _CAESURA_CTX) and ctx.f.marker == 'historical')
        assert(#ctx.backlog == 1 and ctx.backlog[1].text == 'current reply')
        local page = {}
        for _, draw in ipairs(require('kag.text_scene').get_state(ctx).draws) do
            page[#page + 1] = draw.text or ''
        end
        local visible = table.concat(page)
        assert(visible:find('current reply', 1, true), 'fresh AI reply must be presented')
        assert(not visible:find('obsolete', 1, true), 'old response/fallback must stay absent')
        assert(ctx.waiting_input and not ctx._rollback_waiting)
        assert(next(_AI_CALLBACKS) == nil)
        collectgarbage('collect')
        assert(u12_ai_weak[1] == nil and u12_ai_weak[2] == nil)
    )lua");
    engine.shutdown();
}

int retainSentinel(lua_State* state) {
    lua_newtable(state);
    lua_pushstring(state, "unrelated registry owner");
    lua_setfield(state, -2, "value");
    return luaL_ref(state, LUA_REGISTRYINDEX);
}

void checkSentinel(lua_State* state, int reference) {
    lua_rawgeti(state, LUA_REGISTRYINDEX, reference);
    CHECK(lua_istable(state, -1));
    if (lua_istable(state, -1)) {
        lua_getfield(state, -1, "value");
        const char* value = lua_tostring(state, -1);
        CHECK(value != nullptr);
        if (value) CHECK(std::string(value) == "unrelated registry owner");
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
}

} // namespace

TEST_CASE("AI callback completion releases its reference exactly once") {
    ControlledAiJobs jobs;
    auto lua = makeAiLua();
    run(lua.get(), R"lua(
        calls = 0
        assert(AI.query_async('no endpoint', function(text, err)
            calls = calls + 1
            assert(text == nil and err == 'no-endpoint')
        end))
    )lua");
    jobs.work(0);
    jobs.complete(0);
    run(lua.get(), "assert(calls == 1)");

    const int sentinel = retainSentinel(lua.get());
    SUBCASE("cancel after completion leaves a reused registry reference alive") {
        run(lua.get(), "assert(AI.cancel())");
    }
    SUBCASE("redelivered completion leaves a reused registry reference alive") {
        jobs.complete(0);
    }
    checkSentinel(lua.get(), sentinel);
    run(lua.get(), "assert(calls == 1)");
    luaL_unref(lua.get(), LUA_REGISTRYINDEX, sentinel);
}

TEST_CASE("AI U12: rollback suppresses obsolete real HTTP replies and presents a fresh reply") {
    bool buffered = false;
    bool oldSuccess = true;
    SUBCASE("old success arrives after rollback") {}
    SUBCASE("old HTTP error arrives after rollback") { oldSuccess = false; }
    SUBCASE("old success completes before rollback without consumption") { buffered = true; }
    SUBCASE("old HTTP error completes before rollback without consumption") {
        buffered = true;
        oldSuccess = false;
    }
    checkRollbackAiResponse(buffered, oldSuccess);
}

TEST_CASE("AI callback can cancel and enqueue a successor exactly once") {
    ControlledAiJobs jobs;
    AiLoopback server;
    auto lua = makeAiLua();
    server.configure(lua.get());
    run(lua.get(), R"lua(
        first, successor, reply, failure = 0, 0, nil, nil
        assert(AI.query_async('first', function()
            first = first + 1
            assert(AI.cancel())
            assert(AI.query_async('successor', function(text, err)
                successor = successor + 1
                reply, failure = text, err
            end))
        end))
    )lua");
    jobs.work(0);
    jobs.complete(0);
    REQUIRE(jobs.pending.size() == 2);
    jobs.work(1);
    jobs.complete(1);
    run(lua.get(), R"lua(
        assert(first == 1 and successor == 1, 'both callbacks must run once')
        assert(reply == 'fixture reply' and failure == nil,
               'successor must keep its successful response: ' .. tostring(failure))
    )lua");
    const int sentinel = retainSentinel(lua.get());
    jobs.complete(0);
    jobs.complete(1);
    checkSentinel(lua.get(), sentinel);
    run(lua.get(), "assert(first == 1 and successor == 1)");
    luaL_unref(lua.get(), LUA_REGISTRYINDEX, sentinel);
}

TEST_CASE("AI cancel suppresses stale success and error completions") {
    ControlledAiJobs jobs;
    AiLoopback server;
    auto lua = makeAiLua();
    bool finishBeforeCancel = false;
    SUBCASE("successful worker completes before cancel") {
        server.configure(lua.get());
        finishBeforeCancel = true;
    }
    SUBCASE("successful worker completes after cancel") {
        server.configure(lua.get());
    }
    SUBCASE("failed worker completes before cancel") {
        finishBeforeCancel = true;
    }
    SUBCASE("failed worker completes after cancel") {}
    run(lua.get(), R"lua(
        obsolete = 0
        assert(AI.query_async('obsolete', function() obsolete = obsolete + 1 end))
    )lua");
    if (finishBeforeCancel) jobs.work(0);
    run(lua.get(), "assert(AI.cancel())");
    server.configure(lua.get());
    run(lua.get(), R"lua(
        current, reply, failure = 0, nil, nil
        assert(AI.query_async('current', function(text, err)
            current = current + 1
            reply, failure = text, err
        end))
    )lua");
    if (!finishBeforeCancel) jobs.work(0);
    jobs.complete(0);
    run(lua.get(), "assert(obsolete == 0 and current == 0)");
    jobs.work(1);
    jobs.complete(1);
    run(lua.get(), R"lua(
        assert(obsolete == 0 and current == 1, 'cancel must suppress only obsolete callback')
        assert(reply == 'fixture reply' and failure == nil,
               'new request must keep its successful response: ' .. tostring(failure))
    )lua");
}

TEST_CASE("AI cancellation in one VM leaves another VM request unchanged") {
    ControlledAiJobs jobs;
    AiLoopback server;
    auto first = makeAiLua();
    auto second = makeAiLua();
    server.configure(second.get());
    run(second.get(), R"lua(
        calls, reply, failure = 0, nil, nil
        assert(AI.query_async('another VM', function(text, err)
            calls = calls + 1
            reply, failure = text, err
        end))
    )lua");
    run(first.get(), "assert(AI.cancel())");
    jobs.work(0);
    jobs.complete(0);
    run(second.get(), R"lua(
        assert(calls == 1, 'other VM callback must run once')
        assert(reply == 'fixture reply' and failure == nil,
               'other VM must keep its successful response: ' .. tostring(failure))
    )lua");
}

TEST_CASE("AI callback submitted by a coroutine executes on its main Lua thread") {
    ControlledAiJobs jobs;
    Caesura::NullJobSystem inlineJobs;
    inlineJobs.init();
    bool inlineCompletion = false;
    SUBCASE("queued completion after submitter collection") {}
    SUBCASE("inline completion") { inlineCompletion = true; }
    auto lua = makeAiLua();
    struct RestoreJobBinding {
        Caesura::IJobSystem* previous;
        ~RestoreJobBinding() { Caesura::BackendRegistry::instance().setJobSystem(previous); }
    } restore{Caesura::BackendRegistry::instance().getJobSystem()};
    if (inlineCompletion) Caesura::BackendRegistry::instance().setJobSystem(&inlineJobs);
    run(lua.get(), R"lua(
        calls, onMain = 0, false
        weak_submitter = setmetatable({}, {__mode = 'v'})
        submitter = coroutine.create(function()
            assert(AI.query_async('no endpoint', function()
                calls = calls + 1
                local thread
                thread, onMain = coroutine.running()
            end))
        end)
        assert(coroutine.resume(submitter))
        assert(coroutine.status(submitter) == 'dead')
        weak_submitter[1], submitter = submitter, nil
        collectgarbage('collect')
        assert(weak_submitter[1] == nil, 'completed submitter must be collectible')
    )lua");
    if (!inlineCompletion) {
        jobs.work(0);
        jobs.complete(0);
    }
    run(lua.get(), "assert(calls == 1 and onMain == true, 'callback must execute on main Lua thread')");
    const int sentinel = retainSentinel(lua.get());
    run(lua.get(), "assert(AI.cancel())");
    checkSentinel(lua.get(), sentinel);
    luaL_unref(lua.get(), LUA_REGISTRYINDEX, sentinel);
}

TEST_CASE("AI rejected job submission releases the callback and reports failure") {
    ControlledAiJobs jobs;
    auto lua = makeAiLua();
    jobs.shutdown();
    run(lua.get(), R"lua(
        weak = setmetatable({}, {__mode = 'v'})
        do
            local callback = function() error('rejected callback invoked') end
            weak[1] = callback
            accepted = AI.query_async('rejected', callback)
        end
        collectgarbage('collect')
        assert(accepted == false, 'closed job system must reject admission')
        assert(weak[1] == nil, 'rejected callback must not stay rooted')
    )lua");
    CHECK(jobs.pending.empty());
}
