#include "doctest.h"
#include "entry/OwnerRpcQueue.h"

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
