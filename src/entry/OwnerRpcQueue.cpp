#include "OwnerRpcQueue.h"

#include <algorithm>
#include <condition_variable>
#include <optional>
#include <exception>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace Caesura {
namespace {
const char* operationName(const RpcRequest& request) {
    return std::visit([](const auto& operation) -> const char* {
        using T = std::decay_t<decltype(operation)>;
        if constexpr (std::is_same_v<T, RpcStatusRequest>) return "status";
        else if constexpr (std::is_same_v<T, RpcRunScriptRequest>) return "run";
        else if constexpr (std::is_same_v<T, RpcStopRequest>) return "stop";
        else if constexpr (std::is_same_v<T, RpcEvaluateRequest>) return "eval";
        else if constexpr (std::is_same_v<T, RpcGetStateRequest>) return "get_state";
        else if constexpr (std::is_same_v<T, RpcSmaValidateRequest>) return "sma_validate";
        else if constexpr (std::is_same_v<T, RpcPickRequest>) return "pick";
        else if constexpr (std::is_same_v<T, RpcSmaSaveRequest>) return "sma_save";
        else if constexpr (std::is_same_v<T, RpcStatsRequest>) return "stats";
        else if constexpr (std::is_same_v<T, RpcCaptureFrameRequest>) return "capture_frame";
        else if constexpr (std::is_same_v<T, RpcReloadScriptsRequest>) return "reload";
        else if constexpr (std::is_same_v<T, RpcLoadAnimationRequest>) return "load_animation";
        else if constexpr (std::is_same_v<T, RpcSetBreakpointRequest>) return "set_breakpoint";
        else if constexpr (std::is_same_v<T, RpcRemoveBreakpointRequest>) return "remove_breakpoint";
        else if constexpr (std::is_same_v<T, RpcClearBreakpointsRequest>) return "clear_breakpoints";
        else if constexpr (std::is_same_v<T, RpcDebugResumeRequest>) return "debug_resume";
        else if constexpr (std::is_same_v<T, RpcInspectLocalRequest>) return "inspect_local";
        else if constexpr (std::is_same_v<T, RpcInspectGlobalRequest>) return "inspect_global";
        else if constexpr (std::is_same_v<T, RpcGetDebugStateRequest>) return "debug_state";
        else return "kag_debug";
    }, request.payload);
}

// A result code is a diagnostic label, never an arbitrary error or script.
std::string diagnosticCode(const std::string& code) {
    if (code.size() > 64) return "redacted_code";
    for (const unsigned char c : code) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return "redacted_code";
    }
    return code;
}
} // namespace

static_assert(std::is_nothrow_move_constructible_v<RpcRequest>,
              "Taking an accepted RPC payload must not fail after Running is published");

struct OwnerRpcQueue::Pending {
    enum class State { Queued, Running, Completed, Cancelled };
    Pending(const RpcRequest& value, std::uint64_t valueId)
        : request(value), id(valueId), operation(operationName(value)) {}

    std::optional<RpcRequest> request;
    const std::uint64_t id;
    const char* const operation;
    const std::chrono::steady_clock::time_point acceptedAt = std::chrono::steady_clock::now();
    std::condition_variable ready;
    State state = State::Queued; // every read/write uses OwnerRpcQueue::m_mutex
    RpcReply reply;
};

OwnerRpcQueue::OwnerRpcQueue(Executor executor, Observer observer)
    : m_executor(std::move(executor)), m_observer(std::move(observer)),
      m_ownerThread(std::this_thread::get_id()) {}

OwnerRpcQueue::~OwnerRpcQueue() { close(); }

std::uint64_t OwnerRpcQueue::currentRequestId() const noexcept {
    if (std::this_thread::get_id() != m_ownerThread) return 0;
    return m_currentRequestId;
}

RpcReply OwnerRpcQueue::unavailable() {
    return {RpcReplyStatus::Unavailable, "dispatcher_closed",
            "Engine RPC dispatcher is closed", {}};
}

RpcReply OwnerRpcQueue::dispatch(const RpcRequest& request, std::chrono::milliseconds timeout) {
    const bool owner = std::this_thread::get_id() == m_ownerThread;
    std::shared_ptr<Pending> pending;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_accepting) return unavailable();
        pending = std::make_shared<Pending>(request, m_nextRequestId++);
        if (!owner) m_pending.push_back(pending);
    }
    observe(pending, Phase::Accepted);
    if (owner) {
        runPending(pending);
        std::lock_guard<std::mutex> lock(m_mutex);
        return pending->reply;
    }

    std::unique_lock<std::mutex> lock(m_mutex);
    if (pending->ready.wait_for(lock, timeout, [&] {
            return pending->state == Pending::State::Completed ||
                   pending->state == Pending::State::Cancelled;
        })) return pending->reply;

    if (pending->state == Pending::State::Queued) {
        pending->reply = {RpcReplyStatus::Busy, "request_cancelled",
            "Engine request cancelled before execution (request_id=" +
                std::to_string(pending->id) + ")", {}};
        pending->state = Pending::State::Cancelled;
        pending->request.reset(); // local batch references cannot retain the request body
        m_pending.erase(std::remove(m_pending.begin(), m_pending.end(), pending), m_pending.end());
        lock.unlock();
        observe(pending, Phase::TimedOut, pending->reply.status, pending->reply.code);
        observe(pending, Phase::Cancelled, pending->reply.status, pending->reply.code);
        pending->ready.notify_one();
        return pending->reply;
    }

    // Promotion won the same mutex: this operation has started. The timeout
    // abandons only the waiting reply, not the owner operation or its terminal.
    lock.unlock();
    observe(pending, Phase::TimedOut, RpcReplyStatus::Busy, "result_unknown");
    return {RpcReplyStatus::Busy, "result_unknown",
        "Engine request started; its result is unknown after the wait timeout. "
        "Do not automatically retry mutations (request_id=" +
            std::to_string(pending->id) + ")", {}};
}

void OwnerRpcQueue::pump() {
    if (std::this_thread::get_id() != m_ownerThread)
        throw std::logic_error("OwnerRpcQueue::pump requires the owner thread");
    if (m_pumping) return;
    m_pumping = true;
    struct ResetFlag { bool& flag; ~ResetFlag() { flag = false; } } reset{m_pumping};
    std::uint64_t lastId = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_pending.empty()) lastId = m_pending.back()->id;
    }
    // Snapshot the accepted prefix, not ownership of each queued request.
    // Its unstarted tail stays visible to close/timeout; newly admitted IDs
    // wait for the next pump, even when the executor admits more work.
    for (;;) {
        std::shared_ptr<Pending> pending;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_pending.empty() || m_pending.front()->id > lastId) return;
            pending = m_pending.front();
        }
        runPending(pending);
    }
}

void OwnerRpcQueue::runPending(const std::shared_ptr<Pending>& pending) {
    std::optional<RpcRequest> request;
    bool cancelled = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (pending->state != Pending::State::Queued) return;
        if (!m_accepting) {
            pending->reply = unavailable();
            pending->state = Pending::State::Cancelled;
            pending->request.reset();
            cancelled = true;
        } else {
            pending->state = Pending::State::Running;
            request.emplace(std::move(*pending->request));
            pending->request.reset();
        }
        if (!m_pending.empty() && m_pending.front() == pending) m_pending.pop_front();
        else m_pending.erase(std::remove(m_pending.begin(), m_pending.end(), pending), m_pending.end());
    }
    if (cancelled) {
        observe(pending, Phase::Cancelled, pending->reply.status, pending->reply.code);
        pending->ready.notify_one();
        return;
    }
    observe(pending, Phase::Started);
    RpcReply reply;
    {
        struct RestoreContext {
            std::uint64_t& current;
            std::uint64_t previous;
            ~RestoreContext() { current = previous; }
        } restore{m_currentRequestId, m_currentRequestId};
        m_currentRequestId = pending->id;
        reply = invoke(*request);
    }
    complete(pending, std::move(reply));
}

void OwnerRpcQueue::close() {
    std::deque<std::shared_ptr<Pending>> cancelled;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_accepting) return;
        m_accepting = false;
        cancelled.swap(m_pending);
        for (const auto& pending : cancelled) {
            pending->reply = unavailable();
            pending->state = Pending::State::Cancelled;
            pending->request.reset();
        }
    }
    for (const auto& pending : cancelled) {
        observe(pending, Phase::Cancelled, pending->reply.status, pending->reply.code);
        pending->ready.notify_one();
    }
}

void OwnerRpcQueue::complete(const std::shared_ptr<Pending>& pending, RpcReply reply, Phase phase) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (pending->state != Pending::State::Running) return;
        pending->reply = std::move(reply);
        pending->state = Pending::State::Completed;
    }
    observe(pending, phase, pending->reply.status, pending->reply.code);
    pending->ready.notify_one();
}
RpcReply OwnerRpcQueue::invoke(const RpcRequest& request) const {
    try {
        return m_executor(request);
    } catch (const std::exception& error) {
        return {RpcReplyStatus::Failed, "owner_dispatch_exception", error.what(), {}};
    } catch (...) {
        return {RpcReplyStatus::Failed, "owner_dispatch_exception",
                "Owner dispatcher threw an unknown exception", {}};
    }
}

void OwnerRpcQueue::observe(const std::shared_ptr<Pending>& pending, Phase phase,
                           RpcReplyStatus status, const std::string& code) const noexcept {
    if (!m_observer) return;
    try {
        m_observer({pending->id, pending->operation, phase, status, diagnosticCode(code),
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - pending->acceptedAt)});
    } catch (...) {
        // Diagnostics must never change execution, wakeup or lifetime semantics.
    }
}

const char* OwnerRpcQueue::phaseName(Phase phase) noexcept {
    switch (phase) {
    case Phase::Accepted: return "accepted";
    case Phase::Started: return "started";
    case Phase::TimedOut: return "timed_out";
    case Phase::Completed: return "completed";
    case Phase::Cancelled: return "cancelled";
    }
    return "unknown";
}

} // namespace Caesura
