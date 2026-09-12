#pragma once

#include "api/IRpcDispatcher.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace Caesura {

// Internal RPC queue, constructed by main's composition root. Only the
// constructing thread may pump; the executor stays on that owner thread.
class OwnerRpcQueue final {
public:
    enum class Phase { Accepted, Started, TimedOut, Completed, Cancelled };
    struct Event {
        std::uint64_t requestId = 0;
        const char* operation = "unknown";
        Phase phase = Phase::Accepted;
        RpcReplyStatus status = RpcReplyStatus::Ok;
        std::string code;
        std::chrono::milliseconds elapsed{0};
    };
    using Executor = std::function<RpcReply(const RpcRequest&)>;
    // Called without the queue mutex, possibly from different threads. The
    // observer must synchronize its own state. No request body enters Event.
    using Observer = std::function<void(const Event&)>;

    explicit OwnerRpcQueue(Executor executor, Observer observer = {});
    ~OwnerRpcQueue();
    OwnerRpcQueue(const OwnerRpcQueue&) = delete;
    OwnerRpcQueue& operator=(const OwnerRpcQueue&) = delete;

    RpcReply dispatch(const RpcRequest& request,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));
    void pump();
    // Stops intake and wakes queued callers; an executing owner operation is
    // never rolled back or executed from the closing thread.
    void close();

    // Nonzero inside the owner executor; nested dispatch restores its caller.
    // Foreign threads never read the owner-only execution context.
    std::uint64_t currentRequestId() const noexcept;

    static const char* phaseName(Phase phase) noexcept;

private:
    struct Pending;
    RpcReply invoke(const RpcRequest& request) const;
    void runPending(const std::shared_ptr<Pending>& pending);
    void complete(const std::shared_ptr<Pending>& pending, RpcReply reply,
                  Phase phase = Phase::Completed);
    void observe(const std::shared_ptr<Pending>& pending, Phase phase,
                 RpcReplyStatus status = RpcReplyStatus::Ok,
                 const std::string& code = {}) const noexcept;
    static RpcReply unavailable();

    Executor m_executor;
    Observer m_observer;
    std::thread::id m_ownerThread;
    std::mutex m_mutex;
    std::deque<std::shared_ptr<Pending>> m_pending;
    std::uint64_t m_nextRequestId = 1;
    bool m_accepting = true;
    bool m_pumping = false; // owner-only reentrancy guard
    std::uint64_t m_currentRequestId = 0; // owner only
};

} // namespace Caesura
