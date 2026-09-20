#pragma once
#include "../../src/job/api/IJobSystem.h"
#include <cstdio>
#include <cstdint>
#include <exception>

namespace Caesura {

// NullJobSystem -- synchronous mock for unit testing
// All submitted work runs immediately on the calling thread.
// No worker threads, no queues, no waiting.
// Use in tests to isolate modules from real JobSystem dependency.

class NullJobSystem : public IJobSystem {
public:
    NullJobSystem() = default;
    ~NullJobSystem() override = default;

    void init() override {
        if (m_running) return;
        ++m_epoch;
        m_running = true;
    }

    void shutdown() override {
        if (!m_running) return;
        ++m_epoch;
        m_running = false;
        m_jobId = 1;
    }

    uint64_t submit(JobFn work,
                    JobPriority priority = JobPriority::Normal,
                    MainThreadFn onComplete = nullptr) override {
        (void)priority;
        if (!m_running || !work) return 0;
        const uint64_t epoch = m_epoch;
        uint64_t id = m_jobId++;
        // Exception isolation mirrors the real JobSystem: a throwing task or
        // completion callback is caught, reported, and swallowed so it can
        // neither escape the submit() caller nor corrupt the queue. The
        // completion callback still runs, and later submissions are unaffected.
        if (work) {
            try {
                work();
            } catch (const std::exception& e) {
                fprintf(stderr,
                        "[NullJobSystem] submit work() threw -- isolated and swallowed: %s\n",
                        e.what());
            } catch (...) {
                fprintf(stderr,
                        "[NullJobSystem] submit work() threw unknown exception -- "
                        "isolated and swallowed\n");
            }
        }
        if (onComplete && m_running && epoch == m_epoch) {
            try {
                onComplete();
            } catch (const std::exception& e) {
                fprintf(stderr,
                        "[NullJobSystem] submit onComplete() threw -- isolated and "
                        "swallowed: %s\n",
                        e.what());
            } catch (...) {
                fprintf(stderr,
                        "[NullJobSystem] submit onComplete() threw unknown exception -- "
                        "isolated and swallowed\n");
            }
        }
        return id;
    }

    void pollMainThreadJobs() override {
        // No-op: all jobs already completed synchronously
    }

    void waitIdle() override {
        // No-op: synchronous mode, nothing pending
    }

    int workerCount() const override { return 0; }
    int pendingJobs() const override { return 0; }
    bool isRunning() const override { return m_running; }
    JobSystemSnapshot getSnapshot() const override {
        return {false, m_running, 0, 0, 0};
    }

private:
    bool m_running = false;
    uint64_t m_epoch = 0;
    uint64_t m_jobId = 1;
};

} // namespace Caesura
