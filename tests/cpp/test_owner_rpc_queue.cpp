#include "doctest.h"
#include "rpc/OwnerRpcQueue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

using namespace Caesura;
using namespace std::chrono_literals;

namespace {
using Phase = OwnerRpcQueue::Phase;

class Gate {
public:
    void open() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_open = true;
        m_changed.notify_all();
    }
    bool wait() {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, 3s, [&] { return m_open; });
    }
private:
    std::mutex m_mutex;
    std::condition_variable m_changed;
    bool m_open = false;
};

class Events {
public:
    void add(const OwnerRpcQueue::Event& event) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_events.push_back(event);
        m_changed.notify_all();
    }
    bool wait(Phase phase, const char* operation) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, 3s, [&] {
            return std::any_of(m_events.begin(), m_events.end(), [&](const auto& event) {
                return event.phase == phase && std::strcmp(event.operation, operation) == 0;
            });
        });
    }
    std::vector<OwnerRpcQueue::Event> snapshot() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_events;
    }
private:
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::vector<OwnerRpcQueue::Event> m_events;
};

RpcReply ok() { return {RpcReplyStatus::Ok, {}, {}, {}}; }

// The worker calls the real production queue; only the host executor and
// diagnostic observer are controlled. Cleanup closes intake before joining.
class Caller {
public:
    Caller(OwnerRpcQueue& queue, RpcRequest request,
           std::chrono::milliseconds timeout = 2s, Gate* start = nullptr)
        : m_queue(queue), m_start(start),
          m_task([&queue, request = std::move(request), timeout, start]() {
              if (start && !start->wait())
                  return RpcReply{RpcReplyStatus::Failed, "test_start_timeout", {}, {}};
              return queue.dispatch(request, timeout);
          }),
          m_reply(m_task.get_future()), m_thread(std::ref(m_task)) {}
    ~Caller() {
        if (m_thread.joinable()) {
            if (m_start) m_start->open();
            m_queue.close();
            m_thread.join();
        }
    }
    RpcReply finish() {
        RpcReply result = m_reply.get();
        m_thread.join();
        return result;
    }
private:
    OwnerRpcQueue& m_queue;
    Gate* m_start;
    std::packaged_task<RpcReply()> m_task;
    std::future<RpcReply> m_reply;
    std::thread m_thread;
};

int count(const std::vector<OwnerRpcQueue::Event>& events, Phase phase, const char* operation) {
    return static_cast<int>(std::count_if(events.begin(), events.end(), [&](const auto& event) {
        return event.phase == phase && std::strcmp(event.operation, operation) == 0;
    }));
}
} // namespace

TEST_CASE("U18 OwnerRpcQueue: worker execution stays on owner with a direct-owner positive control") {
    Events events;
    const auto owner = std::this_thread::get_id();
    int calls = 0;
    bool ownerOnly = true;
    OwnerRpcQueue queue([&](const RpcRequest&) {
        ++calls;
        ownerOnly = ownerOnly && std::this_thread::get_id() == owner;
        return ok();
    }, [&](const auto& event) { events.add(event); });
    CHECK(queue.dispatch(RpcRequest{RpcStatusRequest{}}).status == RpcReplyStatus::Ok);
    Caller caller(queue, RpcRequest{RpcEvaluateRequest{"return 1"}});
    REQUIRE(events.wait(Phase::Accepted, "eval"));
    CHECK(calls == 1);
    queue.pump();
    CHECK(caller.finish().status == RpcReplyStatus::Ok);
    CHECK(calls == 2);
    CHECK(ownerOnly);
}

TEST_CASE("U18 OwnerRpcQueue: queued timeout cancels before execution and permits a fresh request") {
    Events events;
    int calls = 0;
    OwnerRpcQueue queue([&](const RpcRequest&) { ++calls; return ok(); },
                        [&](const auto& event) { events.add(event); });
    Caller caller(queue, RpcRequest{RpcEvaluateRequest{"queued-side-effect"}}, 40ms);
    REQUIRE(events.wait(Phase::Accepted, "eval"));
    const auto reply = caller.finish(); // Owner has not pumped: deterministically Queued.
    CHECK(reply.status == RpcReplyStatus::Busy);
    CHECK(reply.code == "request_cancelled");
    queue.pump();
    CHECK(calls == 0);
    const auto trace = events.snapshot();
    CHECK(count(trace, Phase::Started, "eval") == 0);
    CHECK(count(trace, Phase::Cancelled, "eval") == 1);
    CHECK(count(trace, Phase::Completed, "eval") == 0);
    CHECK(queue.dispatch(RpcRequest{RpcStatusRequest{}}).status == RpcReplyStatus::Ok);
    CHECK(calls == 1);
}

TEST_CASE("U18 OwnerRpcQueue: a queued tail can time out after the owner snapshots its batch") {
    Events events;
    Gate firstStarted, tailReturned;
    std::atomic<bool> tailObservedStart{false};
    int tailCalls = 0;
    bool firstWaited = false;
    OwnerRpcQueue queue([&](const RpcRequest& request) {
        if (std::holds_alternative<RpcStatusRequest>(request.payload)) {
            firstStarted.open();
            firstWaited = tailReturned.wait();
        } else ++tailCalls;
        return ok();
    }, [&](const auto& event) {
        events.add(event);
        if (event.phase == Phase::Accepted && std::strcmp(event.operation, "eval") == 0)
            tailObservedStart = firstStarted.wait(); // Timeout starts only inside the owner batch.
    });
    Caller first(queue, RpcRequest{RpcStatusRequest{}});
    REQUIRE(events.wait(Phase::Accepted, "status"));
    Caller tail(queue, RpcRequest{RpcEvaluateRequest{"must-not-run"}}, 40ms);
    REQUIRE(events.wait(Phase::Accepted, "eval"));
    RpcReply tailReply;
    std::thread release([&] { tailReply = tail.finish(); tailReturned.open(); });
    queue.pump();
    release.join();
    CHECK(first.finish().status == RpcReplyStatus::Ok);
    CHECK(firstWaited);
    CHECK(tailObservedStart.load());
    CHECK(tailReply.code == "request_cancelled");
    CHECK(tailCalls == 0);
    CHECK(count(events.snapshot(), Phase::Cancelled, "eval") == 1);
}

TEST_CASE("U18 OwnerRpcQueue: Running timeout is unknown and has one correlated late terminal") {
    bool fail = false;
    SUBCASE("late success") { fail = false; }
    SUBCASE("late failure") { fail = true; }
    Events events;
    Gate started, callerReturned;
    std::atomic<bool> acceptedWaited{false};
    int sideEffects = 0;
    bool executorWaited = false;
    OwnerRpcQueue queue([&](const RpcRequest& request) {
        if (std::holds_alternative<RpcEvaluateRequest>(request.payload)) {
            executorWaited = callerReturned.wait();
            ++sideEffects;
            if (fail) throw std::runtime_error("auth=secret-sentinel error body");
        }
        return ok();
    }, [&](const auto& event) {
        events.add(event);
        if (std::strcmp(event.operation, "eval") != 0) return;
        if (event.phase == Phase::Accepted) acceptedWaited = started.wait();
        if (event.phase == Phase::Started) started.open();
    });
    Caller caller(queue, RpcRequest{RpcEvaluateRequest{"script=secret-sentinel"}}, 40ms);
    REQUIRE(events.wait(Phase::Accepted, "eval"));
    RpcReply unknown;
    std::thread release([&] { unknown = caller.finish(); callerReturned.open(); });
    queue.pump();
    release.join();
    CHECK(acceptedWaited.load());
    CHECK(executorWaited);
    CHECK(unknown.status == RpcReplyStatus::Busy);
    CHECK(unknown.code == "result_unknown");
    CHECK(unknown.message.find("unknown") != std::string::npos);
    CHECK(sideEffects == 1); // No rollback promise and no automatic re-execution.
    const auto trace = events.snapshot();
    CHECK(count(trace, Phase::Accepted, "eval") == 1);
    CHECK(count(trace, Phase::Started, "eval") == 1);
    CHECK(count(trace, Phase::TimedOut, "eval") == 1);
    CHECK(count(trace, Phase::Completed, "eval") == 1);
    CHECK(count(trace, Phase::Cancelled, "eval") == 0);
    REQUIRE_FALSE(trace.empty());
    const auto id = trace.front().requestId;
    CHECK(id != 0);
    for (const auto& event : trace) {
        CHECK(event.requestId == id);
        CHECK(event.code.find("secret-sentinel") == std::string::npos);
        CHECK(std::strcmp(event.operation, "eval") == 0);
        if (event.phase == Phase::Completed)
            CHECK(event.status == (fail ? RpcReplyStatus::Failed : RpcReplyStatus::Ok));
    }
    CHECK(queue.dispatch(RpcRequest{RpcStatusRequest{}}).status == RpcReplyStatus::Ok);
}

TEST_CASE("U18 OwnerRpcQueue: close wakes queued waiters and rejects subsequent work") {
    Events events;
    int calls = 0;
    OwnerRpcQueue queue([&](const RpcRequest&) { ++calls; return ok(); },
                        [&](const auto& event) { events.add(event); });
    Caller caller(queue, RpcRequest{RpcEvaluateRequest{"must-not-run"}});
    REQUIRE(events.wait(Phase::Accepted, "eval"));
    queue.close();
    queue.close();
    const auto reply = caller.finish();
    CHECK(reply.status == RpcReplyStatus::Unavailable);
    CHECK(reply.code == "dispatcher_closed");
    queue.pump();
    CHECK(calls == 0);
    CHECK(count(events.snapshot(), Phase::Cancelled, "eval") == 1);
    CHECK(queue.dispatch(RpcRequest{RpcStatusRequest{}}).status == RpcReplyStatus::Unavailable);
}

TEST_CASE("U18 OwnerRpcQueue: Stop closes the accepted batch before the next mutation") {
    Events events;
    int mutations = 0;
    OwnerRpcQueue* ownerQueue = nullptr;
    OwnerRpcQueue queue([&](const RpcRequest& request) {
        if (std::holds_alternative<RpcStopRequest>(request.payload)) ownerQueue->close();
        else ++mutations;
        return ok();
    }, [&](const auto& event) { events.add(event); });
    ownerQueue = &queue;
    Caller stop(queue, RpcRequest{RpcStopRequest{}});
    REQUIRE(events.wait(Phase::Accepted, "stop"));
    Caller tail(queue, RpcRequest{RpcEvaluateRequest{"mutation-after-stop"}});
    REQUIRE(events.wait(Phase::Accepted, "eval"));
    queue.pump();
    CHECK(stop.finish().status == RpcReplyStatus::Ok);
    CHECK(tail.finish().status == RpcReplyStatus::Unavailable);
    CHECK(mutations == 0);
    CHECK(count(events.snapshot(), Phase::Started, "eval") == 0);
    CHECK(count(events.snapshot(), Phase::Cancelled, "eval") == 1);
    CHECK(queue.dispatch(RpcRequest{RpcStatusRequest{}}).status == RpcReplyStatus::Unavailable);
}

TEST_CASE("U18 OwnerRpcQueue: a pump processes only its initial batch") {
    Events events;
    Gate submitTail;
    int tailCalls = 0;
    bool tailAccepted = false;
    OwnerRpcQueue queue([&](const RpcRequest& request) {
        if (std::holds_alternative<RpcStatusRequest>(request.payload)) {
            submitTail.open();
            tailAccepted = events.wait(Phase::Accepted, "eval");
        } else ++tailCalls;
        return ok();
    }, [&](const auto& event) { events.add(event); });
    Caller tail(queue, RpcRequest{RpcEvaluateRequest{"next-pump"}}, 2s, &submitTail);
    Caller first(queue, RpcRequest{RpcStatusRequest{}});
    REQUIRE(events.wait(Phase::Accepted, "status"));
    queue.pump();
    CHECK(tailAccepted);
    CHECK(tailCalls == 0);
    CHECK(first.finish().status == RpcReplyStatus::Ok);
    queue.pump();
    CHECK(tail.finish().status == RpcReplyStatus::Ok);
    CHECK(tailCalls == 1);
}

TEST_CASE("U18 OwnerRpcQueue: diagnostics cannot expose bodies or alter execution") {
    Events events;
    OwnerRpcQueue queue([](const RpcRequest&) {
        return RpcReply{RpcReplyStatus::Failed, "auth=secret-sentinel", "body=secret-sentinel", {}};
    }, [&](const auto& event) { events.add(event); });
    const auto reply = queue.dispatch(RpcRequest{RpcEvaluateRequest{"eval=secret-sentinel"}});
    CHECK(reply.code == "auth=secret-sentinel"); // The response contract itself remains intact.
    const auto trace = events.snapshot();
    REQUIRE(trace.size() == 3);
    CHECK(trace.back().code == "redacted_code");
    for (const auto& event : trace) CHECK(event.code.find("secret-sentinel") == std::string::npos);

    int calls = 0;
    OwnerRpcQueue throwingObserver([&](const RpcRequest&) { ++calls; return ok(); },
                                  [](const auto&) { throw std::runtime_error("observer failed"); });
    CHECK(throwingObserver.dispatch(RpcRequest{RpcStatusRequest{}}).status == RpcReplyStatus::Ok);
    CHECK(calls == 1);
}

TEST_CASE("U18 OwnerRpcQueue: request context restores after nested owner dispatch") {
    Events events;
    OwnerRpcQueue* activeQueue = nullptr;
    std::uint64_t outer = 0, inner = 0, restored = 0, foreign = 99;
    bool nestedOk = false;
    OwnerRpcQueue queue([&](const RpcRequest& request) {
        if (std::holds_alternative<RpcStatusRequest>(request.payload)) {
            outer = activeQueue->currentRequestId();
            std::thread observer([&] { foreign = activeQueue->currentRequestId(); });
            observer.join();
            nestedOk = activeQueue->dispatch(RpcRequest{RpcEvaluateRequest{"nested"}}).status == RpcReplyStatus::Ok;
            restored = activeQueue->currentRequestId();
        } else inner = activeQueue->currentRequestId();
        return ok();
    }, [&](const auto& event) { events.add(event); });
    activeQueue = &queue;
    CHECK(queue.currentRequestId() == 0);
    CHECK(queue.dispatch(RpcRequest{RpcStatusRequest{}}).status == RpcReplyStatus::Ok);
    CHECK(nestedOk);
    CHECK(outer != 0);
    CHECK(inner != 0);
    CHECK(inner != outer);
    CHECK(restored == outer);
    CHECK(foreign == 0);
    CHECK(queue.currentRequestId() == 0);
    for (const auto& event : events.snapshot()) {
        if (std::strcmp(event.operation, "status") == 0) CHECK(event.requestId == outer);
        if (std::strcmp(event.operation, "eval") == 0) CHECK(event.requestId == inner);
    }
}


// U27 owner stats observation. These tests exercise the actual queue and one
// actual Engine/Job callback path; the local executor is not main.cpp's mapper.
#include "entry/Engine.h"
#include "di/BackendRegistry.h"
#include "job/api/IJobSystem.h"

TEST_CASE("U27 OwnerRpc stats: foreign request reads only at owner pump and returns a value copy") {
    Events events;
    const auto owner = std::this_thread::get_id();
    std::atomic<int> calls{0};
    std::atomic<bool> ownerOnly{true};
    RpcStatsResult current;
    current.host.supported = true;
    current.host.completedOwnerFrames = 9007199254740993ULL;
    current.jobs.supported = true;
    current.jobs.queuedCompletions = 4294967301ULL;
    std::mutex currentMutex;
    OwnerRpcQueue queue([&](const RpcRequest& request) {
        ++calls;
        if (std::this_thread::get_id() != owner) ownerOnly = false;
        if (!std::holds_alternative<RpcStatsRequest>(request.payload))
            return RpcReply{RpcReplyStatus::Failed, "unexpected_fixture_request", {}, {}};
        std::lock_guard<std::mutex> lock(currentMutex);
        return RpcReply{RpcReplyStatus::Ok, {}, {}, current};
    }, [&](const auto& event) { events.add(event); });
    Caller caller(queue, RpcRequest{RpcStatsRequest{}});
    REQUIRE(events.wait(Phase::Accepted, "stats"));
    CHECK(calls.load() == 0);
    CHECK(count(events.snapshot(), Phase::Started, "stats") == 0);
    queue.pump();
    const auto reply = caller.finish();
    REQUIRE(reply.status == RpcReplyStatus::Ok);
    const auto* first = std::get_if<RpcStatsResult>(&reply.payload);
    REQUIRE(first != nullptr);
    CHECK(first->host.completedOwnerFrames == 9007199254740993ULL);
    CHECK(first->jobs.queuedCompletions == 4294967301ULL);
    {
        std::lock_guard<std::mutex> lock(currentMutex);
        current.host.completedOwnerFrames = 18446744073709551615ULL;
        current.jobs.queuedCompletions = 0;
    }
    const auto next = queue.dispatch(RpcRequest{RpcStatsRequest{}});
    REQUIRE(std::holds_alternative<RpcStatsResult>(next.payload));
    CHECK(std::get<RpcStatsResult>(next.payload).host.completedOwnerFrames == 18446744073709551615ULL);
    CHECK(first->host.completedOwnerFrames == 9007199254740993ULL);
    CHECK(first->jobs.queuedCompletions == 4294967301ULL);
    CHECK(ownerOnly.load());
    CHECK(calls.load() == 2);
    const auto trace = events.snapshot();
    CHECK(count(trace, Phase::Started, "stats") == 2);
    CHECK(count(trace, Phase::Completed, "stats") == 2);
}

TEST_CASE("U27 OwnerRpc stats: inline callback observes real Job debt until unwind") {
    EngineConfig config;
    config.headless = true;
    Engine engine(std::move(config));
    REQUIRE(engine.init());
    auto* jobs = BackendRegistry::instance().getJobSystem();
    REQUIRE(jobs != nullptr);
    REQUIRE(jobs->getSnapshot().supported);
    const auto owner = std::this_thread::get_id();
    bool ownerOnly = true;
    int reads = 0;
    OwnerRpcQueue queue([&](const RpcRequest& request) {
        ownerOnly = ownerOnly && std::this_thread::get_id() == owner;
        ++reads;
        if (!std::holds_alternative<RpcStatsRequest>(request.payload))
            return RpcReply{RpcReplyStatus::Failed, "unexpected_fixture_request", {}, {}};
        const auto job = jobs->getSnapshot();
        const auto host = engine.getHostSnapshot();
        RpcStatsResult observed;
        observed.jobs = {job.supported, job.running, job.workerPending,
            job.queuedCompletions, job.dispatchingCompletions};
        observed.host.supported = host.supported;
        observed.host.initialized = host.initialized;
        observed.host.completedOwnerFrames = host.completedOwnerFrames;
        observed.host.audioCompletionTrackingSupported = host.audioCompletionTrackingSupported;
        observed.host.audioCompletionsPending = host.audioCompletionsPending;
        observed.host.audioCompletionsActive = host.audioCompletionsActive;
        observed.host.audioCompletionOwnerRefs = host.audioCompletionOwnerRefs;
        return RpcReply{RpcReplyStatus::Ok, {}, {}, observed};
    });
    std::atomic<bool> bodyRan{false};
    bool callbackRan = false;
    RpcReply during;
    REQUIRE(jobs->submit([&] { bodyRan = true; }, JobPriority::Normal, [&] {
        callbackRan = true;
        during = queue.dispatch(RpcRequest{RpcStatsRequest{}}); // direct owner path
    }) != 0);
    jobs->waitIdle(); // actual worker completion publication, not a sleep
    CHECK(bodyRan.load());
    CHECK_FALSE(callbackRan);
    CHECK(reads == 0);
    REQUIRE(jobs->getSnapshot().queuedCompletions == 1);
    jobs->pollMainThreadJobs();
    REQUIRE(callbackRan);
    REQUIRE(during.status == RpcReplyStatus::Ok);
    REQUIRE(std::holds_alternative<RpcStatsResult>(during.payload));
    const auto saved = std::get<RpcStatsResult>(during.payload);
    CHECK(saved.jobs.workerPending == 0);
    CHECK(saved.jobs.queuedCompletions == 0);
    CHECK(saved.jobs.dispatchingCompletions == 1);
    CHECK(saved.host.supported);
    CHECK(saved.host.initialized);
    CHECK(saved.host.audioCompletionTrackingSupported);
    CHECK(saved.host.completedOwnerFrames == 0); // no main-loop frame was pumped
    const auto after = queue.dispatch(RpcRequest{RpcStatsRequest{}});
    REQUIRE(std::holds_alternative<RpcStatsResult>(after.payload));
    CHECK(std::get<RpcStatsResult>(after.payload).jobs.dispatchingCompletions == 0);
    CHECK(saved.jobs.dispatchingCompletions == 1); // immutable returned value
    CHECK(ownerOnly);
    CHECK(reads == 2);
    queue.close();
    engine.shutdown(); // only after the callback and queue dispatch unwind
}

TEST_CASE("U27 OwnerRpc stats: timeout and closed intake never manufacture zero snapshots") {
    SUBCASE("unstarted timeout cancels without a read then fresh stats succeeds") {
        Events events;
        int reads = 0;
        OwnerRpcQueue queue([&](const RpcRequest&) {
            ++reads;
            RpcStatsResult s; s.jobs.supported = true; s.jobs.workerPending = 4294967301ULL;
            return RpcReply{RpcReplyStatus::Ok, {}, {}, s};
        }, [&](const auto& event) { events.add(event); });
        Caller caller(queue, RpcRequest{RpcStatsRequest{}}, 40ms);
        REQUIRE(events.wait(Phase::Accepted, "stats"));
        const auto reply = caller.finish();
        CHECK(reply.status == RpcReplyStatus::Busy);
        CHECK(reply.code == "request_cancelled");
        CHECK(std::holds_alternative<std::monostate>(reply.payload));
        queue.pump();
        CHECK(reads == 0);
        CHECK(count(events.snapshot(), Phase::Started, "stats") == 0);
        const auto fresh = queue.dispatch(RpcRequest{RpcStatsRequest{}});
        REQUIRE(fresh.status == RpcReplyStatus::Ok);
        REQUIRE(std::holds_alternative<RpcStatsResult>(fresh.payload));
        CHECK(std::get<RpcStatsResult>(fresh.payload).jobs.workerPending == 4294967301ULL);
        CHECK(reads == 1);
    }
    SUBCASE("started timeout returns unknown even though one late read completes") {
        Events events;
        Gate started, callerReturned;
        std::atomic<bool> acceptedWaited{false};
        bool executorWaited = false;
        int reads = 0;
        OwnerRpcQueue queue([&](const RpcRequest&) {
            executorWaited = callerReturned.wait();
            ++reads;
            RpcStatsResult s; s.host.supported = true; s.host.completedOwnerFrames = 9007199254740993ULL;
            return RpcReply{RpcReplyStatus::Ok, {}, {}, s};
        }, [&](const auto& event) {
            events.add(event);
            if (event.phase == Phase::Accepted) acceptedWaited = started.wait();
            if (event.phase == Phase::Started) started.open();
        });
        Caller caller(queue, RpcRequest{RpcStatsRequest{}}, 40ms);
        REQUIRE(events.wait(Phase::Accepted, "stats"));
        RpcReply reply;
        std::thread release([&] { reply = caller.finish(); callerReturned.open(); });
        queue.pump();
        release.join();
        CHECK(acceptedWaited.load());
        CHECK(executorWaited);
        CHECK(reply.status == RpcReplyStatus::Busy);
        CHECK(reply.code == "result_unknown");
        CHECK(std::holds_alternative<std::monostate>(reply.payload));
        CHECK(reads == 1);
        const auto trace = events.snapshot();
        CHECK(count(trace, Phase::TimedOut, "stats") == 1);
        CHECK(count(trace, Phase::Completed, "stats") == 1);
        CHECK(count(trace, Phase::Cancelled, "stats") == 0);
    }
    SUBCASE("close wakes a queued stats caller without reading the backend") {
        Events events;
        int reads = 0;
        OwnerRpcQueue queue([&](const RpcRequest&) {
            ++reads; return RpcReply{RpcReplyStatus::Ok, {}, {}, RpcStatsResult{}};
        }, [&](const auto& event) { events.add(event); });
        Caller caller(queue, RpcRequest{RpcStatsRequest{}});
        REQUIRE(events.wait(Phase::Accepted, "stats"));
        queue.close();
        const auto reply = caller.finish();
        CHECK(reply.status == RpcReplyStatus::Unavailable);
        CHECK(reply.code == "dispatcher_closed");
        CHECK(std::holds_alternative<std::monostate>(reply.payload));
        queue.pump();
        CHECK(reads == 0);
        const auto later = queue.dispatch(RpcRequest{RpcStatsRequest{}});
        CHECK(later.status == RpcReplyStatus::Unavailable);
        CHECK(std::holds_alternative<std::monostate>(later.payload));
        CHECK(reads == 0);
    }
}

// Actual entry collector wiring. Controlled interfaces carry sentinel values;
// they are not proof of a real audio/device workload or global idle.
#include "entry/RuntimeStats.h"
#include "audio/NullAudioBackend.h"
#include "mocks/NullJobSystem.h"
#include <limits>
#include <type_traits>

namespace {
struct RuntimeStatsReads {
    const std::thread::id owner = std::this_thread::get_id();
    mutable std::atomic<int> reads{0};
    mutable std::atomic<bool> ownerOnly{true};
    std::atomic<int> mutations{0};
    void read() const {
        ++reads;
        if (std::this_thread::get_id() != owner) ownerOnly = false;
    }
};

class RuntimeStatsJobProbe final : public NullJobSystem {
public:
    JobSystemSnapshot current;
    RuntimeStatsReads calls;
    JobSystemSnapshot getSnapshot() const override { calls.read(); return current; }
    void init() override { ++calls.mutations; }
    void shutdown() override { ++calls.mutations; }
    void pollMainThreadJobs() override { ++calls.mutations; }
    void waitIdle() override { ++calls.mutations; }
};

class RuntimeStatsLoaderProbe final : public IAsyncLoader {
public:
    AsyncLoaderSnapshot current;
    RuntimeStatsReads calls;
    void init() override { ++calls.mutations; }
    void shutdown() override { ++calls.mutations; }
    int enqueue(const std::string&, const std::string&) override { ++calls.mutations; return 0; }
    void cancelAll() override { ++calls.mutations; }
    bool poll() override { ++calls.mutations; return false; }
    std::vector<CompletedLoad> drainCompleted() override { ++calls.mutations; return {}; }
    bool isCurrent(const CompletedLoad&) const override { return false; }
    int pendingCount() const override { return 0; }
    bool isRunning() const override { return false; }
    AsyncLoaderSnapshot getSnapshot() const override { calls.read(); return current; }
};

class RuntimeStatsAudioProbe final : public NullAudioBackend {
public:
    AudioBackendSnapshot current;
    RuntimeStatsReads calls;
    AudioBackendSnapshot getSnapshot() override { calls.read(); return current; }
    bool init() override { ++calls.mutations; return false; }
    void shutdown() override { ++calls.mutations; }
    void update(float) override { ++calls.mutations; }
    void flushWaveCache() override { ++calls.mutations; }
    unsigned int consumeVoiceCompletions() override { ++calls.mutations; return 0; }
};

class RuntimeStatsHostProbe final : public IEngineHostSnapshot {
public:
    EngineHostSnapshot current;
    RuntimeStatsReads calls;
    EngineHostSnapshot getHostSnapshot() const override { calls.read(); return current; }
};

class RuntimeStatsRegistryScope {
public:
    RuntimeStatsRegistryScope(IJobSystem* jobs, IAsyncLoader* loader, IAudioBackend* audio)
        : registry(BackendRegistry::instance()), oldJobs(registry.getJobSystem()),
          oldLoader(registry.getAsyncLoader()), oldAudio(registry.getAudioBackend()) {
        registry.setJobSystem(jobs);
        registry.setAsyncLoader(loader);
        registry.setAudioBackend(audio);
    }
    ~RuntimeStatsRegistryScope() {
        registry.setJobSystem(oldJobs);
        registry.setAsyncLoader(oldLoader);
        registry.setAudioBackend(oldAudio);
    }
private:
    BackendRegistry& registry;
    IJobSystem* oldJobs;
    IAsyncLoader* oldLoader;
    IAudioBackend* oldAudio;
};
} // namespace

TEST_CASE("U27 Entry stats: registered interfaces are copied once without consuming state") {
    RuntimeStatsJobProbe jobs;
    RuntimeStatsLoaderProbe loader;
    RuntimeStatsAudioProbe audio;
    RuntimeStatsHostProbe host;
    RuntimeStatsRegistryScope registration(&jobs, &loader, &audio);
    constexpr uint64_t big = 9007199254740993ULL;
    constexpr uint64_t maximum = std::numeric_limits<uint64_t>::max();
    jobs.current = {true, false, big, big + 1, maximum};
    loader.current = {true, true, big + 2, big + 3, big + 4, big + 5, maximum};
    host.current = {true, true, true, false, static_cast<AsyncHostDelivery>(777), true,
        maximum, big + 6, big + 7, big + 8, true, big + 9, big + 10, 1};
    audio.current = {true, true, static_cast<AudioOutputMode>(777), big + 11, big + 12,
        big + 13, big + 14, big + 15, big + 16, big + 17, big + 18, maximum};
    const auto first = captureRuntimeStats(host);
    static_assert(std::is_same_v<decltype(first.jobs.workerPending), uint64_t>);
    static_assert(std::is_same_v<decltype(first.asyncLoader.cacheBytes), uint64_t>);
    static_assert(std::is_same_v<decltype(first.host.completedOwnerFrames), uint64_t>);
    static_assert(std::is_same_v<decltype(first.audio.restoredSources), uint64_t>);
    CHECK(first.jobs.supported);
    CHECK_FALSE(first.jobs.running);
    CHECK(first.jobs.workerPending == big);
    CHECK(first.jobs.queuedCompletions == big + 1);
    CHECK(first.jobs.dispatchingCompletions == maximum);
    CHECK(first.asyncLoader.supported);
    CHECK(first.asyncLoader.running);
    CHECK(first.asyncLoader.pendingWaiters == big + 2);
    CHECK(first.asyncLoader.inflightKeys == big + 3);
    CHECK(first.asyncLoader.completedBuffered == big + 4);
    CHECK(first.asyncLoader.cacheEntries == big + 5);
    CHECK(first.asyncLoader.cacheBytes == maximum);
    CHECK(first.host.supported);
    CHECK(first.host.initialized);
    CHECK(first.host.running);
    CHECK_FALSE(first.host.luaPaused);
    CHECK(static_cast<int>(first.host.delivery) == 777);
    CHECK(first.host.asyncOwnershipComplete); // Raw copy; main's existing mapping decides completeness.
    CHECK(first.host.completedOwnerFrames == maximum);
    CHECK(first.host.deferredAsyncPayloads == big + 6);
    CHECK(first.host.drainingAsyncPayloads == big + 7);
    CHECK(first.host.dispatchingAsyncPayloads == big + 8);
    CHECK(first.host.audioCompletionTrackingSupported);
    CHECK(first.host.audioCompletionsPending == big + 9);
    CHECK(first.host.audioCompletionsActive == big + 10);
    CHECK(first.host.audioCompletionOwnerRefs == 1);
    CHECK(first.audio.supported);
    CHECK(first.audio.running);
    CHECK(static_cast<int>(first.audio.outputMode) == 777);
    CHECK(first.audio.liveVoices == big + 11);
    CHECK(first.audio.busVoices == big + 12);
    CHECK(first.audio.sessionHandles == big + 13);
    CHECK(first.audio.retiringBGM == big + 14);
    CHECK(first.audio.retiringVoice == big + 15);
    CHECK(first.audio.waveCacheEntries == big + 16);
    CHECK(first.audio.rawCacheEntries == big + 17);
    CHECK(first.audio.voiceCompletionsPending == big + 18);
    CHECK(first.audio.restoredSources == maximum);
    for (const auto* calls : {&jobs.calls, &loader.calls, &host.calls, &audio.calls}) {
        CHECK(calls->reads.load() == 1);
        CHECK(calls->ownerOnly.load());
        CHECK(calls->mutations.load() == 0);
    }
    jobs.current = {};
    loader.current = {};
    host.current = {};
    audio.current = {};
    const auto second = captureRuntimeStats(host);
    CHECK_FALSE(second.jobs.supported);
    CHECK_FALSE(second.asyncLoader.supported);
    CHECK_FALSE(second.host.supported);
    CHECK_FALSE(second.audio.supported);
    CHECK(first.jobs.workerPending == big);
    CHECK(first.asyncLoader.cacheBytes == maximum);
    CHECK(first.host.completedOwnerFrames == maximum);
    CHECK(first.audio.restoredSources == maximum);
    for (const auto* calls : {&jobs.calls, &loader.calls, &host.calls, &audio.calls})
        CHECK(calls->reads.load() == 2);
}

TEST_CASE("U27 Entry stats: missing backends stay unsupported while host is still observed") {
    RuntimeStatsHostProbe host;
    host.current.supported = true;
    host.current.completedOwnerFrames = 4294967301ULL;
    RuntimeStatsRegistryScope registration(nullptr, nullptr, nullptr);
    const auto result = captureRuntimeStats(host);
    CHECK_FALSE(result.jobs.supported);
    CHECK_FALSE(result.asyncLoader.supported);
    CHECK_FALSE(result.audio.supported);
    CHECK(result.audio.outputMode == AudioOutputMode::Unknown);
    CHECK(result.host.supported);
    CHECK(result.host.completedOwnerFrames == 4294967301ULL);
    CHECK(host.calls.reads.load() == 1);
}

TEST_CASE("U27 Entry stats: actual owner queue invokes the collector only after owner pump") {
    RuntimeStatsJobProbe jobs;
    RuntimeStatsLoaderProbe loader;
    RuntimeStatsAudioProbe audio;
    RuntimeStatsHostProbe host;
    RuntimeStatsRegistryScope registration(&jobs, &loader, &audio);
    Events events;
    std::vector<RuntimeStatsSnapshot> captured;
    OwnerRpcQueue queue([&](const RpcRequest&) {
        captured.push_back(captureRuntimeStats(host));
        return ok();
    }, [&](const auto& event) { events.add(event); });
    Caller caller(queue, RpcRequest{RpcStatsRequest{}});
    REQUIRE(events.wait(Phase::Accepted, "stats"));
    for (const auto* calls : {&jobs.calls, &loader.calls, &host.calls, &audio.calls})
        CHECK(calls->reads.load() == 0);
    queue.pump();
    CHECK(caller.finish().status == RpcReplyStatus::Ok);
    REQUIRE(captured.size() == 1);
    for (const auto* calls : {&jobs.calls, &loader.calls, &host.calls, &audio.calls}) {
        CHECK(calls->reads.load() == 1);
        CHECK(calls->ownerOnly.load());
        CHECK(calls->mutations.load() == 0);
    }
    queue.close();
}

TEST_CASE("U27 Entry stats: real Job callback debt remains in returned copy until unwind") {
    EngineConfig config;
    config.headless = true;
    Engine engine(std::move(config));
    REQUIRE(engine.init());
    auto* jobs = BackendRegistry::instance().getJobSystem();
    REQUIRE(jobs != nullptr);
    REQUIRE(jobs->getSnapshot().supported);
    std::atomic<bool> bodyRan{false};
    bool callbackRan = false;
    RuntimeStatsSnapshot during;
    REQUIRE(jobs->submit([&] { bodyRan = true; }, JobPriority::Normal, [&] {
        during = captureRuntimeStats(engine);
        callbackRan = true;
    }) != 0);
    jobs->waitIdle();
    CHECK(bodyRan.load());
    CHECK_FALSE(callbackRan);
    const auto before = captureRuntimeStats(engine);
    CHECK(before.jobs.queuedCompletions == 1);
    jobs->pollMainThreadJobs();
    REQUIRE(callbackRan);
    CHECK(during.jobs.dispatchingCompletions == 1);
    CHECK(during.host.supported);
    CHECK(during.host.initialized);
    CHECK_FALSE(during.audio.supported); // Headless Null audio is not real audio evidence.
    const auto after = captureRuntimeStats(engine);
    CHECK(after.jobs.dispatchingCompletions == 0);
    CHECK(during.jobs.dispatchingCompletions == 1);
    engine.shutdown();
}

// U27 render collection uses the production collector and OwnerRpcQueue. The
// interface boundary supplies values only; this is not a GPU workload or a test
// of main.cpp's separate transport mapping.
#include "EntryLifecycleBackends.h"
#include "render/NullRenderDevice.h"

namespace {
class RuntimeStatsRenderProbe final : public Test::RenderDevice {
public:
    explicit RuntimeStatsRenderProbe(Test::LifecycleProbe& lifecycle)
        : Test::RenderDevice(lifecycle) {}
    RenderSnapshot current;
    RuntimeStatsReads calls;
    mutable std::atomic<int> otherReads{0};
    RenderSnapshot getSnapshot() const override { calls.read(); return current; }
    bool isInitialized() const override { ++otherReads; return false; }
    RenderRuntimeInfo getRuntimeInfo() const override { ++otherReads; return {}; }
    const char* getBackendName() const override { ++otherReads; return "unexpected separate read"; }
    bool init(void*, int, int) override { ++calls.mutations; return false; }
    void setPresentSize(uint32_t, uint32_t) override { ++calls.mutations; }
    void beginShutdown() override { ++calls.mutations; }
    void shutdown() override { ++calls.mutations; }
    void flushAllRTT() override { ++calls.mutations; }
    void beginFrame() override { ++calls.mutations; }
    void endFrame() override { ++calls.mutations; }
    void commit_frame() override { ++calls.mutations; }
    void advanceFrame() override { ++calls.mutations; }
    void beginBatch() override { ++calls.mutations; }
    void flushBatch() override { ++calls.mutations; }
    void setScreenOffset(int, int) override { ++calls.mutations; }
    void setViewRect(uint16_t, uint16_t, uint16_t, uint16_t, uint16_t) override { ++calls.mutations; }
    void setViewClear(uint16_t, uint16_t, uint32_t, float, uint8_t) override { ++calls.mutations; }
    void touch(uint16_t) override { ++calls.mutations; }
    ViewportHandle createRenderTarget(int, int) override { ++calls.mutations; return {}; }
    void destroyRenderTarget(ViewportHandle) override { ++calls.mutations; }
    RenderTextureHandle getViewportTexture(ViewportHandle) override { ++otherReads; return {}; }
    int getBackbufferWidth() const override { ++otherReads; return 0; }
    int getBackbufferHeight() const override { ++otherReads; return 0; }
    void blitViewport(ViewportHandle, uint16_t, float, float, float, float) override { ++calls.mutations; }
    void resize(int, int) override { ++calls.mutations; }
    void blitTexture(uint16_t, uint32_t, float, float, float, float, uint8_t) override { ++calls.mutations; }
    void stretchBlt(uint16_t, uint32_t, float, float, float, float,
        uint32_t, float, float, float, float, int) override { ++calls.mutations; }
    void affineBlt(uint16_t, uint32_t, float, float, float, float,
        uint32_t, float, float, float, float, const float[6]) override { ++calls.mutations; }
    void setDebugName(uint16_t, const std::string&) override { ++calls.mutations; }
    void drawDebugOverlay(const std::string&) override { ++calls.mutations; }
    bool requestScreenshot(const std::string&) override { ++calls.mutations; return false; }
    ScreenshotResult requestScreenshot(const ScreenshotOptions&) override { ++calls.mutations; return {}; }
    ScreenshotResult takeScreenshot(const ScreenshotTicket&) override { ++calls.mutations; return {}; }
    bool cancelScreenshot(const ScreenshotTicket&) override { ++calls.mutations; return false; }
    bool recoverDevice(void*, int, int) override { ++calls.mutations; return false; }
    void flagDeviceLost() override { ++calls.mutations; }
    bool consumeDeviceLost() override { ++calls.mutations; return false; }
    void renderText(uint16_t, const std::string&, float, float,
        uint8_t, uint8_t, uint8_t, uint8_t, float, bool, bool, bool) override { ++calls.mutations; }
    void renderRuby(uint16_t, const std::string&, const std::string&, float, float,
        uint8_t, uint8_t, uint8_t, uint8_t) override { ++calls.mutations; }
    void setFont(int) override { ++calls.mutations; }
    bool loadTTF(const char*, float) override { ++calls.mutations; return false; }
    FontRestoreState captureFontState() const override { ++otherReads; return {}; }
    FontRestoreState defaultFontState() const override { ++otherReads; return {}; }
    std::unique_ptr<IPreparedFontState> prepareFontState(
        const FontRestoreState&, const uint8_t*, size_t) override { ++calls.mutations; return {}; }
    bool applyFontState(std::unique_ptr<IPreparedFontState>) override { ++calls.mutations; return false; }
    void clearFontState() override { ++calls.mutations; }
    float textLineHeight() const override { ++otherReads; return 0.0f; }
    void submitBlend(uint16_t, RenderTextureHandle, RenderTextureHandle, int,
        float, float, float) override { ++calls.mutations; }
    void submitTransition(uint16_t, RenderTextureHandle, RenderTextureHandle,
        RenderTextureHandle, int, float) override { ++calls.mutations; }
    void submitVFX(uint16_t, RenderTextureHandle, int,
        float, float, float, float, float, float, float) override { ++calls.mutations; }
    void fillViewport(ViewportHandle, uint8_t, uint8_t, uint8_t, uint8_t) override { ++calls.mutations; }
    bool setColorFilter(ColorFilterPreset) override { ++calls.mutations; return false; }
    bool isPostFxSupported(PostFxKind) const override { ++otherReads; return false; }
    PostFxHandle createPostFx(PostFxKind, const PostFxParams&) override { ++calls.mutations; return 0; }
    void setPostFxParams(PostFxHandle, const PostFxParams&) override { ++calls.mutations; }
    void destroyPostFx(PostFxHandle) override { ++calls.mutations; }
    void clearPostFx() override { ++calls.mutations; }
    bool isPostFxActive() const override { ++otherReads; return false; }
    RenderUniformHandle getDefaultSampler() const override { ++otherReads; return {}; }
    RenderProgramHandle getFallbackProgram() const override { ++otherReads; return {}; }
    RenderProgramHandle getModulatedTextureProgram() const override { ++otherReads; return {}; }
    bool setPreferredBackend(const char*) override { ++calls.mutations; return false; }
};

class RuntimeStatsRenderScope {
public:
    explicit RuntimeStatsRenderScope(IRenderDevice* renderer)
        : registry(BackendRegistry::instance()), previous(registry.getRenderDevice()) {
        registry.setRenderDevice(renderer);
    }
    ~RuntimeStatsRenderScope() { registry.setRenderDevice(previous); }
private:
    BackendRegistry& registry;
    IRenderDevice* previous;
};

RenderSnapshot u27WideNativeRender() {
    RenderSnapshot s;
    s.supported = true;
    s.contextInitialized = false;
    s.renderingAvailable = true;
    s.resourceCountsAvailable = false;
    s.backendKind = static_cast<RenderBackendKind>(777); // Entry preserves raw future enums.
    s.backendName = "GPU \"quoted\"\nline\\tail";
    s.contextGeneration = 4294967296ULL;
    s.captureSubmissionFrame = 9007199254740993ULL;
    s.resources = {18446744073709551615ULL, 4294967297ULL, 9007199254740995ULL,
        18446744073709551614ULL, 4294967298ULL, 9007199254740997ULL,
        18446744073709551613ULL, 4294967299ULL, 9007199254740999ULL,
        18446744073709551612ULL, 4294967300ULL};
    s.screenshots = {true, 9007199254741001ULL, 18446744073709551611ULL,
        4294967301ULL, 9007199254741003ULL, 18446744073709551610ULL};
    s.screenshotOwnershipComplete = false;
    s.screenshotReadbackTrackingSupported = true;
    s.screenshotReadbacksOutstanding = 4294967302ULL;
    return s;
}

void u27CheckNativeRender(const RenderSnapshot& actual, const RenderSnapshot& expected) {
    CHECK(actual.supported == expected.supported);
    CHECK(actual.contextInitialized == expected.contextInitialized);
    CHECK(actual.renderingAvailable == expected.renderingAvailable);
    CHECK(actual.resourceCountsAvailable == expected.resourceCountsAvailable);
    CHECK(actual.backendKind == expected.backendKind);
    CHECK(actual.backendName == expected.backendName);
    CHECK(actual.contextGeneration == expected.contextGeneration);
    CHECK(actual.captureSubmissionFrame == expected.captureSubmissionFrame);
    CHECK(actual.resources.dynamicIndexBuffers == expected.resources.dynamicIndexBuffers);
    CHECK(actual.resources.dynamicVertexBuffers == expected.resources.dynamicVertexBuffers);
    CHECK(actual.resources.frameBuffers == expected.resources.frameBuffers);
    CHECK(actual.resources.indexBuffers == expected.resources.indexBuffers);
    CHECK(actual.resources.occlusionQueries == expected.resources.occlusionQueries);
    CHECK(actual.resources.programs == expected.resources.programs);
    CHECK(actual.resources.shaders == expected.resources.shaders);
    CHECK(actual.resources.textures == expected.resources.textures);
    CHECK(actual.resources.uniforms == expected.resources.uniforms);
    CHECK(actual.resources.vertexBuffers == expected.resources.vertexBuffers);
    CHECK(actual.resources.vertexLayouts == expected.resources.vertexLayouts);
    CHECK(actual.screenshots.supported == expected.screenshots.supported);
    CHECK(actual.screenshots.waiting == expected.screenshots.waiting);
    CHECK(actual.screenshots.submitted == expected.screenshots.submitted);
    CHECK(actual.screenshots.terminal == expected.screenshots.terminal);
    CHECK(actual.screenshots.reservedBytes == expected.screenshots.reservedBytes);
    CHECK(actual.screenshots.pngBytes == expected.screenshots.pngBytes);
    CHECK(actual.screenshotOwnershipComplete == expected.screenshotOwnershipComplete);
    CHECK(actual.screenshotReadbackTrackingSupported == expected.screenshotReadbackTrackingSupported);
    CHECK(actual.screenshotReadbacksOutstanding == expected.screenshotReadbacksOutstanding);
}

void u27CheckUnsupportedNativeRender(const RenderSnapshot& value) {
    CHECK_FALSE(value.supported);
    CHECK_FALSE(value.contextInitialized);
    CHECK_FALSE(value.renderingAvailable);
    CHECK_FALSE(value.resourceCountsAvailable);
    CHECK(value.backendKind == RenderBackendKind::Unknown);
    CHECK(value.backendName.empty());
    CHECK(value.contextGeneration == 0);
    CHECK(value.captureSubmissionFrame == 0);
    CHECK(value.resources.dynamicIndexBuffers == 0);
    CHECK(value.resources.dynamicVertexBuffers == 0);
    CHECK(value.resources.frameBuffers == 0);
    CHECK(value.resources.indexBuffers == 0);
    CHECK(value.resources.occlusionQueries == 0);
    CHECK(value.resources.programs == 0);
    CHECK(value.resources.shaders == 0);
    CHECK(value.resources.textures == 0);
    CHECK(value.resources.uniforms == 0);
    CHECK(value.resources.vertexBuffers == 0);
    CHECK(value.resources.vertexLayouts == 0);
    CHECK_FALSE(value.screenshots.supported);
    CHECK(value.screenshots.waiting == 0);
    CHECK(value.screenshots.submitted == 0);
    CHECK(value.screenshots.terminal == 0);
    CHECK(value.screenshots.reservedBytes == 0);
    CHECK(value.screenshots.pngBytes == 0);
    CHECK_FALSE(value.screenshotOwnershipComplete);
    CHECK_FALSE(value.screenshotReadbackTrackingSupported);
    CHECK(value.screenshotReadbacksOutstanding == 0);
}

void u27CheckRenderReadOnly(const RuntimeStatsRenderProbe& renderer, int reads) {
    CHECK(renderer.calls.reads.load() == reads);
    CHECK(renderer.calls.ownerOnly.load());
    CHECK(renderer.calls.mutations.load() == 0);
    CHECK(renderer.otherReads.load() == 0);
}
} // namespace

TEST_CASE("U27 Render entry stats: collector preserves every native value and owns its copy") {
    Test::LifecycleProbe lifecycle;
    RuntimeStatsRenderProbe renderer(lifecycle);
    RuntimeStatsHostProbe host;
    RuntimeStatsRegistryScope otherBackends(nullptr, nullptr, nullptr);
    RuntimeStatsRenderScope registration(&renderer);
    const auto expected = u27WideNativeRender();
    renderer.current = expected;
    const auto first = captureRuntimeStats(host);
    u27CheckNativeRender(first.render, expected);
    u27CheckRenderReadOnly(renderer, 1);
    CHECK(host.calls.reads.load() == 1);
    renderer.current = {};
    renderer.current.backendKind = RenderBackendKind::Noop;
    renderer.current.backendName = "new context owns another string";
    renderer.current.contextInitialized = true;
    renderer.current.resourceCountsAvailable = true;
    renderer.current.screenshotOwnershipComplete = true;
    renderer.current.screenshotReadbacksOutstanding = 18446744073709551615ULL;
    const auto second = captureRuntimeStats(host);
    u27CheckNativeRender(second.render, renderer.current);
    u27CheckNativeRender(first.render, expected);
    u27CheckRenderReadOnly(renderer, 2);
    CHECK(host.calls.reads.load() == 2);
    renderer.current.backendName.assign(256, 'X');
    CHECK(first.render.backendName == "GPU \"quoted\"\nline\\tail");
    CHECK(second.render.backendName == "new context owns another string");
}

TEST_CASE("U27 Render entry stats: missing and actual Null backends keep all defaults unsupported") {
    RuntimeStatsHostProbe host;
    host.current.supported = true;
    host.current.completedOwnerFrames = 9007199254740993ULL;
    RuntimeStatsRegistryScope otherBackends(nullptr, nullptr, nullptr);
    {
        RuntimeStatsRenderScope missing(nullptr);
        const auto observed = captureRuntimeStats(host);
        u27CheckUnsupportedNativeRender(observed.render);
        CHECK(observed.host.supported);
        CHECK(observed.host.completedOwnerFrames == 9007199254740993ULL);
        CHECK(host.calls.reads.load() == 1);
    }
    {
        NullRenderDevice renderer;
        RuntimeStatsRenderScope registeredNull(&renderer);
        const auto observed = captureRuntimeStats(host);
        u27CheckUnsupportedNativeRender(observed.render);
        CHECK(observed.host.supported);
        CHECK(observed.host.completedOwnerFrames == 9007199254740993ULL);
        CHECK(host.calls.reads.load() == 2);
    }
}

TEST_CASE("U27 Render entry stats: owner queue reads once per request without lifecycle or ticket consumption") {
    Test::LifecycleProbe lifecycle;
    RuntimeStatsRenderProbe renderer(lifecycle);
    RuntimeStatsHostProbe host;
    RuntimeStatsRegistryScope otherBackends(nullptr, nullptr, nullptr);
    RuntimeStatsRenderScope registration(&renderer);
    auto expected = u27WideNativeRender();
    expected.backendKind = RenderBackendKind::GraphicsApi;
    renderer.current = expected;
    Events events;
    std::vector<RuntimeStatsSnapshot> captured;
    OwnerRpcQueue queue([&](const RpcRequest& request) {
        if (!std::holds_alternative<RpcStatsRequest>(request.payload))
            return RpcReply{RpcReplyStatus::Failed, "unexpected_fixture_request", {}, {}};
        captured.push_back(captureRuntimeStats(host));
        return ok();
    }, [&](const auto& event) { events.add(event); });
    Caller caller(queue, RpcRequest{RpcStatsRequest{}});
    REQUIRE(events.wait(Phase::Accepted, "stats"));
    u27CheckRenderReadOnly(renderer, 0);
    CHECK(host.calls.reads.load() == 0);
    CHECK(count(events.snapshot(), Phase::Started, "stats") == 0);
    queue.pump();
    REQUIRE(caller.finish().status == RpcReplyStatus::Ok);
    REQUIRE(captured.size() == 1);
    u27CheckRenderReadOnly(renderer, 1);
    CHECK(host.calls.reads.load() == 1);
    u27CheckNativeRender(captured.front().render, expected);
    u27CheckNativeRender(renderer.current, expected); // Observation did not consume the supplied state.
    renderer.current = {};
    renderer.current.contextGeneration = 18446744073709551615ULL;
    CHECK(queue.dispatch(RpcRequest{RpcStatsRequest{}}).status == RpcReplyStatus::Ok);
    REQUIRE(captured.size() == 2);
    u27CheckRenderReadOnly(renderer, 2);
    u27CheckNativeRender(captured.back().render, renderer.current);
    u27CheckNativeRender(captured.front().render, expected);
    queue.pump(); // An empty pump must not manufacture another observation.
    u27CheckRenderReadOnly(renderer, 2);
    const auto trace = events.snapshot();
    CHECK(count(trace, Phase::Started, "stats") == 2);
    CHECK(count(trace, Phase::Completed, "stats") == 2);
    queue.close();
}
