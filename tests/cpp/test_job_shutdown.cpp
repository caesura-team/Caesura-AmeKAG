#include "doctest.h"
#include "job/JobSystem.h"
#include "mocks/NullJobSystem.h"
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>

using namespace Caesura;

namespace {
// pendingJobs reaches zero only after workers have queued their completions.
// Do not poll here: the test controls exactly when a main-thread batch begins.
bool workersFinished(IJobSystem& jobs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (jobs.pendingJobs() != 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    return jobs.pendingJobs() == 0;
}
}

TEST_CASE("Job U6: shutdown delivers already queued completions exactly once") {
    int completions = 0;
    JobSystem jobs;
    jobs.init();
    REQUIRE(jobs.submit([] {}, JobPriority::Normal, [&] { ++completions; }) > 0);
    REQUIRE(workersFinished(jobs));
    jobs.waitIdle();
    CHECK(completions == 0);
    jobs.shutdown();
    CHECK(completions == 1);
    jobs.pollMainThreadJobs();
    jobs.shutdown();
    CHECK(completions == 1);
    CHECK(jobs.workerCount() == 0);
}

TEST_CASE("Job U6: final drain isolates exceptions and rejects reentrant admission") {
    int first = 0;
    int second = 0;
    JobSystem jobs;
    jobs.init();
    REQUIRE(jobs.submit([] {}, JobPriority::Normal, [&] {
        ++first;
        CHECK(jobs.workerCount() == 0);
        CHECK_FALSE(jobs.isRunning());
        CHECK(jobs.submit([] {}) == 0);
        jobs.init();
        CHECK_FALSE(jobs.isRunning());
        throw std::runtime_error("deliberate completion failure");
    }) > 0);
    REQUIRE(workersFinished(jobs));
    REQUIRE(jobs.submit([] {}, JobPriority::Normal, [&] { ++second; }) > 0);
    REQUIRE(workersFinished(jobs));
    CHECK_NOTHROW(jobs.shutdown());
    CHECK(first == 1);
    CHECK(second == 1);
    CHECK_FALSE(jobs.isRunning());
    CHECK(jobs.workerCount() == 0);
}

TEST_CASE("Job U6: recursive polling leaves new completions for the next batch") {
    int first = 0;
    int second = 0;
    JobSystem jobs;
    jobs.init();
    REQUIRE(jobs.submit([] {}, JobPriority::Normal, [&] {
        ++first;
        CHECK(jobs.submit([] {}, JobPriority::Normal, [&] { ++second; }) > 0);
        CHECK(workersFinished(jobs));
        jobs.waitIdle();
        jobs.pollMainThreadJobs();
        CHECK(second == 0);
    }) > 0);
    REQUIRE(workersFinished(jobs));
    jobs.pollMainThreadJobs();
    CHECK(first == 1);
    CHECK(second == 0);
    jobs.pollMainThreadJobs();
    CHECK(second == 1);
    jobs.shutdown();
}

TEST_CASE("Job U6: nested shutdown cancels the rest of the final snapshot") {
    int first = 0;
    int second = 0;
    JobSystem jobs;
    jobs.init();
    REQUIRE(jobs.submit([] {}, JobPriority::Normal, [&] {
        ++first;
        jobs.shutdown();
    }) > 0);
    REQUIRE(workersFinished(jobs));
    REQUIRE(jobs.submit([] {}, JobPriority::Normal, [&] { ++second; }) > 0);
    REQUIRE(workersFinished(jobs));
    jobs.shutdown();
    CHECK(first == 1);
    CHECK(second == 0);
    CHECK_FALSE(jobs.isRunning());
}

TEST_CASE("Job U6: shutdown inside a callback invalidates its remaining batch") {
    bool restart = false;
    SUBCASE("stop") {}
    SUBCASE("restart with a new request") { restart = true; }
    int oldCallbacks = 0;
    int freshCallbacks = 0;
    JobSystem jobs;
    jobs.init();
    REQUIRE(jobs.submit([] {}, JobPriority::Normal, [&] {
        jobs.shutdown();
        if (restart) {
            jobs.init();
            CHECK(jobs.submit([] {}, JobPriority::Normal, [&] { ++freshCallbacks; }) > 0);
        }
    }) > 0);
    REQUIRE(workersFinished(jobs));
    REQUIRE(jobs.submit([] {}, JobPriority::Normal, [&] { ++oldCallbacks; }) > 0);
    REQUIRE(workersFinished(jobs));
    jobs.pollMainThreadJobs();
    CHECK(oldCallbacks == 0);
    REQUIRE(workersFinished(jobs));
    jobs.pollMainThreadJobs();
    CHECK(freshCallbacks == (restart ? 1 : 0));
    jobs.shutdown();
}

TEST_CASE("Job U6: real and Null reject an empty work function") {
    int callbacks = 0;
    JobSystem real;
    NullJobSystem synchronous;
    real.init();
    synchronous.init();
    for (IJobSystem* jobs : {static_cast<IJobSystem*>(&real),
                            static_cast<IJobSystem*>(&synchronous)}) {
        CHECK(jobs->submit({}, JobPriority::Normal, [&] { ++callbacks; }) == 0);
        jobs->waitIdle();
        jobs->pollMainThreadJobs();
        CHECK(callbacks == 0);
        jobs->shutdown();
    }
}

TEST_CASE("Job U6: Null inline work cannot complete into a restarted owner") {
    bool restart = false;
    SUBCASE("stop during work") {}
    SUBCASE("stop and restart during work") { restart = true; }
    int callbacks = 0;
    NullJobSystem jobs;
    jobs.init();
    REQUIRE(jobs.submit([&] {
        jobs.shutdown();
        if (restart) jobs.init();
    }, JobPriority::Normal, [&] { ++callbacks; }) > 0);
    CHECK(callbacks == 0);
    if (restart) {
        REQUIRE(jobs.submit([] {}, JobPriority::Normal, [&] { ++callbacks; }) > 0);
        CHECK(callbacks == 1);
    }
    jobs.shutdown();
}

namespace {
void checkJobSnapshot(const JobSystemSnapshot& state, bool running,
                      uint64_t pending, uint64_t queued, uint64_t dispatching) {
    CHECK(state.supported);
    CHECK(state.running == running);
    CHECK(state.workerPending == pending);
    CHECK(state.queuedCompletions == queued);
    CHECK(state.dispatchingCompletions == dispatching);
}

// Release even if a REQUIRE aborts: JobSystem's destructor must be able to join
// the real worker. The promise/state outlives the JobSystem, and this guard is
// declared after it so release always precedes that join.
struct ReleaseSnapshotWorker {
    std::promise<void>& signal;
    bool released = false;
    void release() {
        if (!released) {
            signal.set_value();
            released = true;
        }
    }
    ~ReleaseSnapshotWorker() { release(); }
};
}

TEST_CASE("Job U27 snapshot: worker completion is not callback quiescence") {
    std::promise<void> entered;
    auto started = entered.get_future();
    std::promise<void> release;
    auto released = release.get_future().share();
    int callbacks = 0;
    JobSystemSnapshot duringCallback;
    JobSystem jobs;
    ReleaseSnapshotWorker releaseOnExit{release};
    IJobSystem& api = jobs;
    checkJobSnapshot(api.getSnapshot(), false, 0, 0, 0);
    jobs.init();
    REQUIRE(jobs.submit([&entered, released] {
        entered.set_value();
        released.wait();
    }, JobPriority::Normal, [&] {
        duringCallback = api.getSnapshot();
        ++callbacks;
    }) > 0);
    REQUIRE(started.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    checkJobSnapshot(api.getSnapshot(), true, 1, 0, 0);
    CHECK(jobs.pendingJobs() == 1);

    releaseOnExit.release();
    REQUIRE(workersFinished(jobs));
    jobs.waitIdle();
    CHECK(jobs.pendingJobs() == 0);  // Existing API alone would misidentify idle.
    CHECK(callbacks == 0);
    checkJobSnapshot(api.getSnapshot(), true, 0, 1, 0);
    jobs.pollMainThreadJobs();
    CHECK(callbacks == 1);
    checkJobSnapshot(duringCallback, true, 0, 0, 1);
    checkJobSnapshot(api.getSnapshot(), true, 0, 0, 0);

    REQUIRE(jobs.submit([] {}) > 0);  // Work without a completion also drains.
    REQUIRE(workersFinished(jobs));
    checkJobSnapshot(api.getSnapshot(), true, 0, 0, 0);
    jobs.shutdown();
    checkJobSnapshot(api.getSnapshot(), false, 0, 0, 0);
}

TEST_CASE("Job U27 snapshot: nested polls retain the current batch and newly queued work") {
    int callbacks = 0;
    JobSystemSnapshot first, nestedBefore, nestedAfter, second, fresh;
    JobSystem jobs;
    jobs.init();
    REQUIRE(jobs.submit([] {}, JobPriority::Normal, [&] {
        first = jobs.getSnapshot();
        ++callbacks;
        CHECK(jobs.submit([] {}, JobPriority::Normal, [&] {
            fresh = jobs.getSnapshot();
            ++callbacks;
        }) > 0);
        CHECK(workersFinished(jobs));
        nestedBefore = jobs.getSnapshot();
        jobs.pollMainThreadJobs();
        nestedAfter = jobs.getSnapshot();
        CHECK(callbacks == 1);
    }) > 0);
    REQUIRE(workersFinished(jobs)); // Publish in a controlled order.
    REQUIRE(jobs.submit([] {}, JobPriority::Normal, [&] {
        second = jobs.getSnapshot();
        ++callbacks;
    }) > 0);
    REQUIRE(workersFinished(jobs));
    checkJobSnapshot(jobs.getSnapshot(), true, 0, 2, 0);
    jobs.pollMainThreadJobs();
    CHECK(callbacks == 2);
    checkJobSnapshot(first, true, 0, 0, 2);
    checkJobSnapshot(nestedBefore, true, 0, 1, 2);
    checkJobSnapshot(nestedAfter, true, 0, 1, 2);
    checkJobSnapshot(second, true, 0, 1, 1);
    checkJobSnapshot(jobs.getSnapshot(), true, 0, 1, 0);
    jobs.pollMainThreadJobs();
    CHECK(callbacks == 3);
    checkJobSnapshot(fresh, true, 0, 0, 1);
    checkJobSnapshot(jobs.getSnapshot(), true, 0, 0, 0);
    jobs.shutdown();
}

TEST_CASE("Job U27 snapshot: worker and callback exceptions leave no dispatch debt") {
    int callbacks = 0;
    JobSystemSnapshot first, second;
    JobSystem jobs;
    jobs.init();
    REQUIRE(jobs.submit([] { throw std::runtime_error("U27 worker fault"); },
        JobPriority::Normal, [&] {
            first = jobs.getSnapshot();
            ++callbacks;
            throw std::runtime_error("U27 callback fault");
        }) > 0);
    REQUIRE(workersFinished(jobs));
    REQUIRE(jobs.submit([] {}, JobPriority::Normal, [&] {
        second = jobs.getSnapshot();
        ++callbacks;
        throw 27; // Exercise the non-std exception cleanup path too.
    }) > 0);
    REQUIRE(workersFinished(jobs));
    checkJobSnapshot(jobs.getSnapshot(), true, 0, 2, 0);
    CHECK_NOTHROW(jobs.pollMainThreadJobs());
    CHECK(callbacks == 2);
    checkJobSnapshot(first, true, 0, 0, 2);
    checkJobSnapshot(second, true, 0, 0, 1);
    checkJobSnapshot(jobs.getSnapshot(), true, 0, 0, 0);
    REQUIRE(jobs.submit([] {}, JobPriority::Normal, [&] { ++callbacks; }) > 0);
    REQUIRE(workersFinished(jobs));
    jobs.pollMainThreadJobs();
    CHECK(callbacks == 3);
    jobs.shutdown();
    checkJobSnapshot(jobs.getSnapshot(), false, 0, 0, 0);
}

TEST_CASE("Job U27 snapshot: shutdown cancellation remains visible until outer dispatch returns") {
    bool finalDrain = false;
    SUBCASE("shutdown from ordinary poll cancels active and newly queued callbacks") {}
    SUBCASE("nested shutdown cancels the final shutdown batch") { finalDrain = true; }
    int first = 0, cancelled = 0, fresh = 0;
    JobSystemSnapshot beforeShutdown, insideShutdown;
    JobSystem jobs;
    jobs.init();
    REQUIRE(jobs.submit([] {}, JobPriority::Normal, [&] {
        ++first;
        if (!finalDrain) {
            CHECK(jobs.submit([] {}, JobPriority::Normal, [&] { ++cancelled; }) > 0);
            CHECK(workersFinished(jobs));
        }
        beforeShutdown = jobs.getSnapshot();
        jobs.shutdown();
        insideShutdown = jobs.getSnapshot();
        jobs.pollMainThreadJobs(); // Still inside the same active callback.
        checkJobSnapshot(jobs.getSnapshot(), false, 0, 0, 2);
    }) > 0);
    REQUIRE(workersFinished(jobs));
    REQUIRE(jobs.submit([] {}, JobPriority::Normal, [&] { ++cancelled; }) > 0);
    REQUIRE(workersFinished(jobs));
    if (finalDrain) jobs.shutdown();
    else jobs.pollMainThreadJobs();
    CHECK(first == 1);
    CHECK(cancelled == 0);
    checkJobSnapshot(beforeShutdown, !finalDrain, 0, finalDrain ? 0 : 1, 2);
    checkJobSnapshot(insideShutdown, false, 0, 0, 2);
    checkJobSnapshot(jobs.getSnapshot(), false, 0, 0, 0);
    jobs.init();
    REQUIRE(jobs.submit([] {}, JobPriority::Normal, [&] { ++fresh; }) > 0);
    REQUIRE(workersFinished(jobs));
    checkJobSnapshot(jobs.getSnapshot(), true, 0, 1, 0);
    jobs.pollMainThreadJobs();
    CHECK(fresh == 1);
    CHECK(cancelled == 0);
    checkJobSnapshot(jobs.getSnapshot(), true, 0, 0, 0);
    jobs.shutdown();
}

TEST_CASE("Job U27 snapshot: Null does not claim measured worker or callback quiescence") {
    NullJobSystem jobs;
    IJobSystem& api = jobs;
    CHECK_FALSE(api.getSnapshot().supported);
    CHECK_FALSE(api.getSnapshot().running);
    jobs.init();
    CHECK_FALSE(api.getSnapshot().supported);
    CHECK(api.getSnapshot().running);
    int callbacks = 0;
    REQUIRE(jobs.submit([] {}, JobPriority::Normal, [&] {
        ++callbacks;
        CHECK_FALSE(api.getSnapshot().supported);
        CHECK(api.getSnapshot().running);
    }) > 0);
    CHECK(callbacks == 1);
    jobs.shutdown();
    CHECK_FALSE(api.getSnapshot().supported);
    CHECK_FALSE(api.getSnapshot().running);
}

TEST_CASE("Job U27 snapshot: callback restart retains its old active stack until unwind") {
    int cancelled = 0, fresh = 0;
    JobSystemSnapshot stopped, restarted, nested, freshDispatch;
    JobSystem jobs;
    jobs.init();
    REQUIRE(jobs.submit([] {}, JobPriority::Normal, [&] {
        jobs.shutdown();
        stopped = jobs.getSnapshot();
        jobs.init(); // Same callback stack, new admission epoch.
        CHECK(jobs.submit([] {}, JobPriority::Normal, [&] {
            freshDispatch = jobs.getSnapshot();
            ++fresh;
        }) > 0);
        CHECK(workersFinished(jobs));
        restarted = jobs.getSnapshot();
        jobs.pollMainThreadJobs();
        nested = jobs.getSnapshot();
        CHECK(fresh == 0);
    }) > 0);
    REQUIRE(workersFinished(jobs));
    REQUIRE(jobs.submit([] {}, JobPriority::Normal, [&] { ++cancelled; }) > 0);
    REQUIRE(workersFinished(jobs));
    jobs.pollMainThreadJobs();
    CHECK(cancelled == 0);
    checkJobSnapshot(stopped, false, 0, 0, 2);
    checkJobSnapshot(restarted, true, 0, 1, 2);
    checkJobSnapshot(nested, true, 0, 1, 2);
    checkJobSnapshot(jobs.getSnapshot(), true, 0, 1, 0);
    jobs.pollMainThreadJobs();
    CHECK(fresh == 1);
    CHECK(cancelled == 0);
    checkJobSnapshot(freshDispatch, true, 0, 0, 1);
    checkJobSnapshot(jobs.getSnapshot(), true, 0, 0, 0);
    jobs.shutdown();
}

TEST_CASE("Job U27 snapshot: cancelled capture destruction can observe its outstanding debt") {
    std::promise<void> entered;
    auto started = entered.get_future();
    std::promise<void> release;
    auto released = release.get_future().share();
    const auto ownerThread = std::this_thread::get_id();
    std::thread::id workerThread, destructorThread;
    int destructors = 0, cancelledCallbacks = 0, outerCallbacks = 0;
    JobSystemSnapshot duringDestruction, afterShutdown;
    JobSystem jobs;
    ReleaseSnapshotWorker releaseOnExit{release};

    struct SnapshotOnDestroy {
        JobSystem& jobs;
        JobSystemSnapshot& observed;
        std::thread::id& thread;
        int& destroyed;
        SnapshotOnDestroy(JobSystem& owner, JobSystemSnapshot& snapshot,
                          std::thread::id& observerThread, int& count)
            : jobs(owner), observed(snapshot), thread(observerThread), destroyed(count) {}
        ~SnapshotOnDestroy() {
            ++destroyed;
            thread = std::this_thread::get_id();
            std::fprintf(stderr, "[U27] cancellation destructor entering getSnapshot\n");
            std::fflush(stderr);
            observed = jobs.getSnapshot();
            std::fprintf(stderr, "[U27] cancellation destructor returned from getSnapshot\n");
            std::fflush(stderr);
        }
    };

    jobs.init();
    REQUIRE(jobs.submit([] {}, JobPriority::Normal, [&] {
        ++outerCallbacks;
        auto probe = std::make_shared<SnapshotOnDestroy>(
            jobs, duringDestruction, destructorThread, destructors);
        REQUIRE(jobs.submit([&entered, &workerThread, released] {
            workerThread = std::this_thread::get_id();
            entered.set_value();
            released.wait();
        }, JobPriority::Normal, [probe, &cancelledCallbacks] {
            ++cancelledCallbacks;
        }) > 0);
        REQUIRE(started.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        CHECK(workerThread != ownerThread);
        checkJobSnapshot(jobs.getSnapshot(), true, 1, 0, 1);
        releaseOnExit.release();
        REQUIRE(workersFinished(jobs));
        jobs.waitIdle(); // The real worker has published this completion, not run it.
        checkJobSnapshot(jobs.getSnapshot(), true, 0, 1, 1);
        REQUIRE(probe.use_count() == 2); // Caller and the queued completion only.
        probe.reset();
        CHECK(destructors == 0);

        // Nested poll is suppressed. shutdown cancels the newly queued capture;
        // its destructor re-enters getSnapshot on this same owner thread.
        jobs.shutdown();
        afterShutdown = jobs.getSnapshot();
        CHECK(destructors == 1);
        CHECK(cancelledCallbacks == 0);
    }) > 0);
    REQUIRE(workersFinished(jobs));
    jobs.pollMainThreadJobs();

    CHECK(outerCallbacks == 1);
    CHECK(destructors == 1);
    CHECK(destructorThread == ownerThread);
    CHECK(cancelledCallbacks == 0);
    CHECK(duringDestruction.supported);
    CHECK_FALSE(duringDestruction.running);
    CHECK(duringDestruction.workerPending == 0);
    CHECK(duringDestruction.dispatchingCompletions >= 1);
    // With no workers outstanding, the queued/dispatching categories must
    // still include both the outer callback and this unfinished cancellation.
    CHECK(duringDestruction.queuedCompletions +
          duringDestruction.dispatchingCompletions >= 2);
    checkJobSnapshot(afterShutdown, false, 0, 0, 1);
    checkJobSnapshot(jobs.getSnapshot(), false, 0, 0, 0);
    jobs.pollMainThreadJobs();
    jobs.shutdown();
    CHECK(destructors == 1);
    CHECK(cancelledCallbacks == 0);
    checkJobSnapshot(jobs.getSnapshot(), false, 0, 0, 0);
}
