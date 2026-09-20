#include "JobSystem.h"
#include "../di/api/ThreadAssert.h"
#include <cstdio>
#include <chrono>
#include <exception>

namespace Caesura {

JobSystem::~JobSystem() {
    shutdown();
}

JobSystemSnapshot JobSystem::getSnapshot() const {
    CAESURA_ASSERT_MAIN_THREAD();
    // Publication holds this mutex before pending is decremented. Read both
    // under the same lock so a completed worker cannot appear in neither phase.
    std::lock_guard<std::mutex> lock(m_mainMutex);
    return {true, m_running.load(), static_cast<uint64_t>(m_pendingJobs.load()),
            static_cast<uint64_t>(m_mainJobs.size()), m_dispatchingCompletions};
}

int JobSystem::computeWorkerCount() {
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    if (hw < 4) return 1;
    return static_cast<int>(hw) - 1;
}

void JobSystem::WorkQueue::push(Job job) {
    std::lock_guard<std::mutex> lock(mutex);
    if (job.priority == JobPriority::High)
        jobs.push_front(std::move(job));
    else
        jobs.push_back(std::move(job));
}

bool JobSystem::WorkQueue::pop(Job& out) {
    std::lock_guard<std::mutex> lock(mutex);
    if (jobs.empty()) return false;
    // FIFO from the front: push places High jobs at the front, so popping
    // the front honors priority. Popping the back (LIFO) inverted it --
    // High jobs were drained LAST despite being pushed first.
    out = std::move(jobs.front());
    jobs.pop_front();
    return true;
}

bool JobSystem::WorkQueue::steal(Job& out) {
    std::lock_guard<std::mutex> lock(mutex);
    if (jobs.empty()) return false;
    out = std::move(jobs.front());
    jobs.pop_front();
    return true;
}

bool JobSystem::WorkQueue::empty() const {
    std::lock_guard<std::mutex> lock(mutex);
    return jobs.empty();
}

void JobSystem::init() {
    CAESURA_ASSERT_MAIN_THREAD();
    if (m_shuttingDown || m_running.exchange(true)) return;
    ++m_dispatchEpoch;

    m_workerCount = computeWorkerCount();
    m_queues.reserve(static_cast<size_t>(m_workerCount));
    for (int i = 0; i < m_workerCount; ++i) m_queues.emplace_back(std::make_unique<WorkQueue>());
    m_workers.reserve(static_cast<size_t>(m_workerCount));

    for (int i = 0; i < m_workerCount; ++i) {
        m_workers.emplace_back(&JobSystem::workerLoop, this, i);
    }

    printf("[JobSystem] Initialized: %d worker(s) (hw=%u)\n",
           m_workerCount, std::thread::hardware_concurrency());
}

void JobSystem::shutdown() {
    CAESURA_ASSERT_MAIN_THREAD();
    if (m_shuttingDown) {
        // A final callback may be shutting down its owner. Do not deliver
        // further callbacks from this snapshot after that nested boundary.
        if (m_polling) ++m_dispatchEpoch;
        return;
    }
    if (!m_running.exchange(false)) return;
    m_shuttingDown = true;
    ++m_dispatchEpoch;

    waitIdle();
    notifyWorkers();

    for (auto& t : m_workers) {
        if (t.joinable()) t.join();
    }
    m_workers.clear();
    m_queues.clear();
    m_workerCount = 0;

    // Workers have joined and admission is closed. If shutdown was invoked
    // inside an earlier poll, its remaining callbacks are cancelled instead.
    pollMainThreadJobs();
    {
        struct CancellationGuard {
            uint64_t& dispatching;
            const uint64_t previous;
            ~CancellationGuard() { dispatching = previous; }
        } guard{m_dispatchingCompletions, m_dispatchingCompletions};
        // Declared after the guard: capture destructors may inspect the owner,
        // so retain their cancellation debt until every capture is released.
        std::deque<MainThreadFn> cancelled;
        {
            std::lock_guard<std::mutex> lock(m_mainMutex);
            cancelled.swap(m_mainJobs);
            m_dispatchingCompletions += static_cast<uint64_t>(cancelled.size());
        }
        // Destroy outside m_mainMutex. Admission stays closed throughout;
        // nested shutdown/init cannot revive or deliver cancelled callbacks.
    }
    m_shuttingDown = false;

    printf("[JobSystem] Shutdown complete.\n");
}

uint64_t JobSystem::submit(JobFn work, JobPriority priority, MainThreadFn onComplete) {
    CAESURA_ASSERT_MAIN_THREAD();
    if (!m_running || !work) return 0;

    uint64_t id = m_nextJobId.fetch_add(1);
    m_pendingJobs++;

    Job job;
    job.work       = std::move(work);
    job.onComplete = std::move(onComplete);
    job.priority   = priority;

    uint32_t idx = m_roundRobin.fetch_add(1) % static_cast<uint32_t>(m_workerCount);
    m_queues[idx]->push(std::move(job));
    notifyWorkers();
    return id;
}

void JobSystem::enqueueMainThreadJob(MainThreadFn fn) {
    if (!fn) return;
    std::lock_guard<std::mutex> lock(m_mainMutex);
    m_mainJobs.push_back(std::move(fn));
}

void JobSystem::pollMainThreadJobs() {
    CAESURA_ASSERT_MAIN_THREAD();
    if (m_polling) return;
    struct PollGuard {
        bool& polling;
        uint64_t& dispatching;
        ~PollGuard() {
            // Constructed before batch: cancelled callbacks are destroyed
            // before their outstanding observation is cleared.
            dispatching = 0;
            polling = false;
        }
    } guard{m_polling, m_dispatchingCompletions};
    m_polling = true;
    const uint64_t epoch = m_dispatchEpoch;

    std::deque<MainThreadFn> batch;
    {
        std::lock_guard<std::mutex> lock(m_mainMutex);
        batch.swap(m_mainJobs);
        m_dispatchingCompletions = static_cast<uint64_t>(batch.size());
    }

    for (auto& fn : batch) {
        if (epoch != m_dispatchEpoch) break;
        if (!fn) {
            --m_dispatchingCompletions;
            continue;
        }
        try {
            fn();
        } catch (const std::exception& e) {
            fprintf(stderr,
                    "[JobSystem] Main-thread callback threw -- isolated and swallowed: %s\n",
                    e.what());
        } catch (...) {
            fprintf(stderr,
                    "[JobSystem] Main-thread callback threw unknown exception -- "
                    "isolated and swallowed\n");
        }
        --m_dispatchingCompletions;
    }
}

void JobSystem::waitIdle() {
    CAESURA_ASSERT_MAIN_THREAD();
    while (m_pendingJobs.load() > 0) {
        std::unique_lock<std::mutex> lock(m_waitMutex);
        m_cv.wait_for(lock, std::chrono::milliseconds(10), [this] {
            return m_pendingJobs.load() == 0;
        });
    }
}

// Thread-local worker flag: set once when a worker loop starts. The old
// implementation scanned m_workerThreadIds (O(n)) on every call -- this
// query runs on the hot path (task bodies checking thread affinity).
static thread_local bool t_isWorkerThread = false;

bool JobSystem::isWorkerThread() const {
    return t_isWorkerThread;
}

void JobSystem::notifyWorkers() {
    m_cv.notify_all();
}

bool JobSystem::tryDequeueJob(int workerIndex, Job& out) {
    if (m_queues[static_cast<size_t>(workerIndex)]->pop(out))
        return true;

    for (int i = 0; i < m_workerCount; ++i) {
        if (i == workerIndex) continue;
        if (m_queues[static_cast<size_t>(i)]->steal(out))
            return true;
    }
    return false;
}

void JobSystem::workerLoop(int workerIndex) {
    t_isWorkerThread = true;
    printf("[JobSystem] Worker %d started.\n", workerIndex);

    while (m_running.load() || m_pendingJobs.load() > 0) {
        Job job;
        if (!tryDequeueJob(workerIndex, job)) {
            std::unique_lock<std::mutex> lock(m_waitMutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(10), [this] {
                if (!m_running.load() && m_pendingJobs.load() == 0) return true;
                for (const auto& q : m_queues) {
                    if (!q->empty()) return true;
                }
                return false;
            });
            continue;
        }

        if (job.work) {
            try {
                job.work();
            } catch (const std::exception& e) {
                fprintf(stderr,
                        "[JobSystem] Worker %d: task work() threw -- isolated and "
                        "swallowed: %s\n",
                        workerIndex, e.what());
            } catch (...) {
                fprintf(stderr,
                        "[JobSystem] Worker %d: task work() threw unknown exception "
                        "-- isolated and swallowed\n",
                        workerIndex);
            }
        }

        if (job.onComplete) {
            enqueueMainThreadJob(std::move(job.onComplete));
        }

        m_pendingJobs--;
        if (m_pendingJobs.load() == 0) {
            notifyWorkers();
        }
    }

    printf("[JobSystem] Worker %d stopped.\n", workerIndex);
}

} // namespace Caesura
