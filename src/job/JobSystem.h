#pragma once
#include "api/IJobSystem.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace Caesura {


// Fiber-free work-stealing job system (Beta Part 8 subset).
// - Workers execute JobFn (green zone: I/O, decode, CPU math).
// - MainThreadFn callbacks are queued and run via pollMainThreadJobs() (red zone).
class JobSystem : public IJobSystem {
public:
    JobSystem() = default;
    ~JobSystem() override;
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    // Lifecycle (main thread only)
    void init() override;
    void shutdown() override;

    // Submit CPU work. Returns opaque job id (0 on failure).
    // onComplete, if set, runs on main thread after work finishes.
    uint64_t submit(JobFn work,
                    JobPriority priority = JobPriority::Normal,
                    MainThreadFn onComplete = nullptr) override;

    // Run one callback snapshot; nested polls defer to the next outer call.
    void pollMainThreadJobs() override;

    // Wait for worker bodies and callback publication, without invoking callbacks.
    void waitIdle() override;

    int  workerCount() const override { return m_workerCount; }
    int  pendingJobs() const override { return m_pendingJobs.load(); }
    bool isRunning()   const override { return m_running.load(); }
    JobSystemSnapshot getSnapshot() const override;
    bool isWorkerThread() const;

private:
    struct Job {
        JobFn          work;
        MainThreadFn   onComplete;
        JobPriority    priority = JobPriority::Normal;
    };

    struct WorkQueue {
        std::deque<Job> jobs;
        mutable std::mutex mutex;

        void push(Job job);
        bool pop(Job& out);    // owner: take the highest-priority front item
        bool steal(Job& out);  // thief: take from front
        bool empty() const;
    };

    static int computeWorkerCount();

    void workerLoop(int workerIndex);
    bool tryDequeueJob(int workerIndex, Job& out);
    void enqueueMainThreadJob(MainThreadFn fn);
    void notifyWorkers();

    std::atomic<bool> m_running{false};
    // Owner-thread state; workers never inspect callback dispatch state.
    uint64_t m_dispatchEpoch = 0;
    uint64_t m_dispatchingCompletions = 0;
    bool m_polling = false;
    bool m_shuttingDown = false;
    int m_workerCount = 0;

    std::vector<std::unique_ptr<WorkQueue>> m_queues;
    std::vector<std::thread>            m_workers;

    mutable std::mutex      m_mainMutex;
    std::deque<MainThreadFn> m_mainJobs;

    std::mutex              m_waitMutex;
    std::condition_variable m_cv;

    std::atomic<uint64_t> m_nextJobId{1};
    std::atomic<int>      m_pendingJobs{0};
    std::atomic<uint32_t> m_roundRobin{0};
};

} // namespace Caesura
