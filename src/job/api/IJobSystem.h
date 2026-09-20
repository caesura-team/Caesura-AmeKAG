// IJobSystem - pure virtual interface for parallel job execution
// Concrete: JobSystem. Pattern: module api/ directory.
#pragma once
#include <functional>
#include <cstdint>

namespace Caesura {

enum class JobPriority : uint8_t {
    High   = 0,
    Normal = 1,
    Low    = 2,
};

using JobFn        = std::function<void()>;
using MainThreadFn = std::function<void()>;

struct JobSystemSnapshot {
    // Unsupported backends must not present placeholder zeroes as measured idle.
    bool supported = false;
    bool running = false;
    uint64_t workerPending = 0;
    uint64_t queuedCompletions = 0;
    uint64_t dispatchingCompletions = 0;
};

class IJobSystem {
public:
    virtual ~IJobSystem() = default;

    virtual void init() = 0;
    // Stop admission, join accepted workers, then deliver one final completion
    // snapshot while backend owners are still alive. Reentrant shutdown from
    // a callback cancels remaining callbacks; init is ignored during shutdown.
    virtual void shutdown() = 0;
    virtual uint64_t submit(JobFn work,
                            JobPriority priority = JobPriority::Normal,
                            MainThreadFn onComplete = nullptr) = 0;
    // Main thread only. Process one ready snapshot; callbacks may submit work
    // for a later poll. Nested polls do not recursively drain more callbacks.
    virtual void pollMainThreadJobs() = 0;
    // Main thread only. Wait for accepted worker bodies and publication of
    // their completion callbacks, but do not execute those callbacks. Workers
    // must not synchronously wait for a main-thread callback to finish.
    virtual void waitIdle() = 0;
    virtual int  workerCount() const = 0;
    virtual int  pendingJobs() const = 0;
    virtual bool isRunning() const = 0;
    // Owner/main thread only, including from a completion callback. Counts are
    // valid only when supported. workerPending retains pendingJobs() semantics:
    // accepted work through completion publication, not callback execution.
    // queuedCompletions await a future poll; dispatchingCompletions includes
    // the current callback and the unfinished remainder of the active batch.
    // Nested polling/shutdown must not hide that still-active callback stack.
    // Publication can briefly overlap workerPending and queuedCompletions;
    // these are lifecycle observations, not disjoint quantities to sum.
    virtual JobSystemSnapshot getSnapshot() const = 0;
};

} // namespace Caesura
