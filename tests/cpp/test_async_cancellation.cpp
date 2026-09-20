#include "doctest.h"
#include "resource/AssetManager.h"
#include "resource/AsyncLoader.h"
#include "di/BackendRegistry.h"
#include "di/api/ThreadAssert.h"
#include "mocks/NullJobSystem.h"
#include "TestPaths.h"
#include <SDL3/SDL.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace Caesura;

namespace {
constexpr uint8_t kRedTga[] = {0,0,2,0,0,0,0,0,0,0,0,0,1,0,1,0,24,0x20,0,0,255};
class MemProvider : public IAssetProvider {
public:
    MemProvider(int priority, std::string source) : m_priority(priority), m_source(std::move(source)) {}
    void put(std::string path, std::vector<uint8_t> bytes) { m_files[std::move(path)] = std::move(bytes); }
    bool exists(const std::string& path) override { return m_files.count(path) != 0; }
    std::vector<uint8_t> read(const std::string& path) override {
        auto found=m_files.find(path);
        return found == m_files.end() ? std::vector<uint8_t>{} : found->second;
    }
    std::string getSource() const override { return m_source; }
    int priority() const override { return m_priority; }
    bool verify() override { return true; }
private:
    int m_priority;
    std::string m_source;
    std::map<std::string,std::vector<uint8_t>> m_files;
};
class AsyncFixtures {
public:
    AsyncFixtures() : loader(&assets), previous(BackendRegistry::instance().getJobSystem()) {
        detail::g_mainThreadId=std::this_thread::get_id();
        jobs.init();
        BackendRegistry::instance().setJobSystem(&jobs);
        assets.init();
        loader.init();
    }
    ~AsyncFixtures() {
        loader.shutdown();
        assets.shutdown();
        BackendRegistry::instance().setJobSystem(previous);
        jobs.shutdown();
    }
    NullJobSystem jobs;
    AssetManager assets;
    AsyncLoader loader;
    IJobSystem* previous;
};
}


// U5: cancellation invalidates a request generation, including work or
// completions that are still owned by the job system.
namespace {

class SteppedAsyncJobs : public NullJobSystem {
public:
    uint64_t submit(JobFn work, JobPriority = JobPriority::Normal,
                    MainThreadFn onComplete = nullptr) override {
        if (!isRunning()) return 0;
        m_work.push_back(std::move(work));
        m_completions.push_back(std::move(onComplete));
        return m_work.size();
    }
    JobFn takeWork(size_t index) { return std::move(m_work.at(index)); }
    void runWork(size_t index) {
        auto work = takeWork(index);
        if (work) work();
    }
    void complete(size_t index) {
        auto completion = std::move(m_completions.at(index));
        if (completion) completion();
    }
    void waitIdle() override {
        for (size_t i = 0; i < m_work.size(); ++i) runWork(i);
    }
    void pollMainThreadJobs() override {
        for (size_t i = 0; i < m_completions.size(); ++i) complete(i);
    }
    size_t submittedCount() const { return m_work.size(); }

private:
    std::vector<JobFn> m_work;
    std::vector<MainThreadFn> m_completions;
};

class SteppedAsyncFixture {
public:
    SteppedAsyncFixture() : loader(&assets) {
        detail::g_mainThreadId = std::this_thread::get_id();
        previousJobs = BackendRegistry::instance().getJobSystem();
        jobs.init();
        BackendRegistry::instance().setJobSystem(&jobs);
        assets.init();
        loader.init();
    }
    ~SteppedAsyncFixture() {
        loader.shutdown();
        assets.shutdown();
        BackendRegistry::instance().setJobSystem(previousJobs);
        jobs.shutdown();
    }

    SteppedAsyncJobs jobs;
    AssetManager assets;
    AsyncLoader loader;
    IJobSystem* previousJobs = nullptr;
};

// The first read owns a byte snapshot before it signals the barrier. Rewriting
// the backing file cannot alter that old worker's bytes.
class SnapshotReadProvider : public IAssetProvider {
public:
    explicit SnapshotReadProvider(std::filesystem::path file)
        : m_file(std::move(file)) {}
    bool exists(const std::string& path) override { return path == "generation.tga"; }
    std::vector<uint8_t> read(const std::string&) override {
        std::ifstream input(m_file, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                  std::istreambuf_iterator<char>());
        input.close();
        std::unique_lock<std::mutex> lock(m_mutex);
        ++m_reads;
        m_started.notify_all();
        if (m_reads == 1) m_release.wait(lock, [this] { return m_released; });
        return bytes;
    }
    bool waitForFirstRead() {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_started.wait_for(lock, std::chrono::seconds(5),
                                  [this] { return m_reads > 0; });
    }
    void release() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_released = true;
        m_release.notify_all();
    }
    int reads() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_reads;
    }
    std::string getSource() const override { return "SnapshotReadProvider"; }
    int priority() const override { return 100; }
    bool verify() override { return true; }

private:
    std::filesystem::path m_file;
    std::mutex m_mutex;
    std::condition_variable m_started;
    std::condition_variable m_release;
    int m_reads = 0;
    bool m_released = false;
};

class ScopedSnapshotWorker {
public:
    ScopedSnapshotWorker(SnapshotReadProvider& provider, JobFn work)
        : m_provider(provider), m_thread(std::move(work)) {}
    ~ScopedSnapshotWorker() { finish(); }
    void finish() {
        m_provider.release();
        if (m_thread.joinable()) m_thread.join();
    }

private:
    SnapshotReadProvider& m_provider;
    std::thread m_thread;
};

void writeGenerationPixel(const std::filesystem::path& path, bool blue) {
    // Uncompressed top-left 1x1 24-bit TGA, whose last three bytes are BGR.
    std::vector<uint8_t> bytes(21, 0);
    bytes[2] = 2;
    bytes[12] = 1;
    bytes[14] = 1;
    bytes[16] = 24;
    bytes[17] = 0x20;
    bytes[blue ? 18 : 20] = 255;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    REQUIRE(output.good());
}

} // namespace

TEST_CASE("AsyncLoader U5: new enqueue cannot revive cancelled queued waiters") {
    SteppedAsyncFixture fx;
    auto provider = std::make_unique<MemProvider>(100, "generation");
    provider->put("generation.bin", {'n', 'e', 'w'});
    fx.assets.addProvider(std::move(provider));

    const int oldId = fx.loader.enqueue("generation.bin", "text");
    const int waiterId = fx.loader.enqueue("generation.bin", "text");
    REQUIRE(oldId > 0);
    REQUIRE(waiterId > oldId);
    REQUIRE(fx.jobs.submittedCount() == 1);
    CHECK(fx.loader.pendingCount() == 2);
    fx.loader.cancelAll();
    fx.loader.cancelAll();
    CHECK(fx.loader.pendingCount() == 0);

    const int newId = fx.loader.enqueue("generation.bin", "text");
    REQUIRE(newId > waiterId);
    REQUIRE(fx.jobs.submittedCount() == 2);
    fx.jobs.runWork(0);
    fx.jobs.complete(0);
    CHECK(fx.loader.drainCompleted().empty());
    CHECK(fx.loader.pendingCount() == 1);
    fx.jobs.runWork(1);
    fx.jobs.complete(1);
    const auto done = fx.loader.drainCompleted();
    REQUIRE(done.size() == 1);
    CHECK(done[0].id == newId);
    CHECK(done[0].success);
    CHECK(fx.loader.isCurrent(done[0]));
    fx.loader.cancelAll();
    CHECK_FALSE(fx.loader.isCurrent(done[0]));
    CHECK(fx.loader.pendingCount() == 0);
}

TEST_CASE("AsyncLoader U5: cancelled completed workers never publish late results") {
    SteppedAsyncFixture fx;
    auto provider = std::make_unique<MemProvider>(100, "generation");
    bool success = true;
    SUBCASE("late success") {}
    SUBCASE("late failure") { success = false; }
    // An empty asset deterministically produces a failed load without invoking
    // the independent malformed-image parser assertion in bimg Debug builds.
    provider->put("generation.tga", success
        ? std::vector<uint8_t>(kRedTga, kRedTga + sizeof(kRedTga))
        : std::vector<uint8_t>{});
    fx.assets.addProvider(std::move(provider));
    REQUIRE(fx.loader.enqueue("generation.tga", "texture") > 0);
    fx.jobs.runWork(0);
    fx.loader.cancelAll();
    const int currentId = fx.loader.enqueue("generation.tga", "texture");
    REQUIRE(currentId > 0);
    fx.jobs.complete(0);
    CHECK(fx.loader.drainCompleted().empty());
    CHECK(fx.loader.pendingCount() == 1);
    fx.jobs.runWork(1);
    fx.jobs.complete(1);
    const auto current = fx.loader.drainCompleted();
    REQUIRE(current.size() == 1);
    CHECK(current[0].id == currentId);
    CHECK(current[0].success == success);
    CHECK(fx.loader.pendingCount() == 0);
}

TEST_CASE("AsyncLoader U5: cancelled running snapshot cannot refill the new cache") {
    TestPaths::ScopedTempDir temp("async_generation");
    const auto file = temp.path() / "generation.tga";
    writeGenerationPixel(file, false);
    SteppedAsyncFixture fx;
    auto provider = std::make_unique<SnapshotReadProvider>(file);
    auto* snapshot = provider.get();
    fx.assets.addProvider(std::move(provider));
    const int oldId = fx.loader.enqueue("generation.tga", "texture");
    REQUIRE(oldId > 0);
    ScopedSnapshotWorker worker(*snapshot, fx.jobs.takeWork(0));
    REQUIRE(snapshot->waitForFirstRead());
    const int oldWaiter = fx.loader.enqueue("generation.tga", "texture");
    REQUIRE(oldWaiter > oldId);
    fx.loader.cancelAll();
    fx.loader.cancelAll();
    CHECK(fx.loader.pendingCount() == 0);
    writeGenerationPixel(file, true);

    const int freshId = fx.loader.enqueue("generation.tga", "texture");
    REQUIRE(freshId > oldWaiter);
    REQUIRE(fx.jobs.submittedCount() == 2);
    fx.jobs.runWork(1);
    REQUIRE(snapshot->reads() == 2);
    // New bytes are already read, but the old callback arrives first. It must
    // neither publish its old waiters nor win the cache-insertion race.
    worker.finish();
    fx.jobs.complete(0);
    CHECK(fx.loader.drainCompleted().empty());
    CHECK(fx.loader.pendingCount() == 1);
    // A late old callback must not remove the fresh in-flight association or
    // expose its red snapshot as an intermediate cache hit.
    const int freshWaiter = fx.loader.enqueue("generation.tga", "texture");
    REQUIRE(freshWaiter > freshId);
    CHECK(fx.jobs.submittedCount() == 2);
    CHECK(fx.loader.drainCompleted().empty());
    CHECK(fx.loader.pendingCount() == 2);
    fx.jobs.complete(1);
    const auto fresh = fx.loader.drainCompleted();
    REQUIRE(fresh.size() == 2);
    CHECK(fresh[0].id == freshId);
    CHECK(fresh[1].id == freshWaiter);
    REQUIRE(fresh[0].success);
    CHECK(fresh[1].success);
    CHECK(fresh[0].rgba == std::vector<uint8_t>{0, 0, 255, 255});
    CHECK(fresh[1].rgba == fresh[0].rgba);

    const int cachedId = fx.loader.enqueue("generation.tga", "texture");
    REQUIRE(cachedId > freshId);
    const auto cached = fx.loader.drainCompleted();
    REQUIRE(cached.size() == 1);
    CHECK(cached[0].rgba == fresh[0].rgba);
    CHECK(snapshot->reads() == 2);
    CHECK(fx.jobs.submittedCount() == 2);
    CHECK(fx.loader.pendingCount() == 0);
}

TEST_CASE("AsyncLoader U5: cancellation removes buffered success failure and cache hits") {
    AsyncFixtures fx;
    auto provider = std::make_unique<MemProvider>(100, "generation");
    provider->put("generation.tga",
        std::vector<uint8_t>(std::begin(kRedTga), std::end(kRedTga)));
    auto* source = provider.get();
    fx.assets.addProvider(std::move(provider));

    SUBCASE("buffered success and cache hit") {
        REQUIRE(fx.loader.enqueue("generation.tga", "texture") > 0);
        REQUIRE(fx.loader.enqueue("generation.tga", "texture") > 0);
    }
    SUBCASE("buffered failure") {
        source->put("generation.tga", {});
        REQUIRE(fx.loader.enqueue("generation.tga", "texture") > 0);
    }
    fx.loader.cancelAll();
    CHECK(fx.loader.pendingCount() == 0);
    CHECK(fx.loader.drainCompleted().empty());
    CHECK_FALSE(fx.loader.poll());
    source->put("generation.tga", {});
    REQUIRE(fx.loader.enqueue("generation.tga", "texture") > 0);
    const auto fresh = fx.loader.drainCompleted();
    REQUIRE(fresh.size() == 1);
    CHECK_FALSE(fresh[0].success);
    CHECK(fx.loader.pendingCount() == 0);
}

TEST_CASE("AsyncLoader U5: shutdown invalidates cached and late worker results") {
    SteppedAsyncFixture fx;
    auto provider = std::make_unique<MemProvider>(100, "generation");
    provider->put("generation.tga",
        std::vector<uint8_t>(std::begin(kRedTga), std::end(kRedTga)));
    auto* source = provider.get();
    fx.assets.addProvider(std::move(provider));
    REQUIRE(fx.loader.enqueue("generation.tga", "texture") > 0);
    fx.jobs.runWork(0);
    std::vector<CompletedLoad> transferred;
    SUBCASE("callback already delivered") { fx.jobs.complete(0); }
    SUBCASE("callback delayed until shutdown") {}
    SUBCASE("result already transferred to host") {
        fx.jobs.complete(0);
        transferred = fx.loader.drainCompleted();
        REQUIRE(transferred.size() == 1);
        CHECK(fx.loader.isCurrent(transferred[0]));
    }
    fx.loader.shutdown();
    CHECK(fx.loader.pendingCount() == 0);
    CHECK(fx.loader.drainCompleted().empty());
    if (!transferred.empty()) CHECK_FALSE(fx.loader.isCurrent(transferred[0]));
    source->put("generation.tga", {});
    fx.loader.init();
    const int restartedId = fx.loader.enqueue("generation.tga", "texture");
    REQUIRE(restartedId > 0);
    fx.jobs.waitIdle();
    fx.jobs.pollMainThreadJobs();
    const auto restarted = fx.loader.drainCompleted();
    REQUIRE(restarted.size() == 1);
    CHECK(restarted[0].id == restartedId);
    CHECK_FALSE(restarted[0].success);
    CHECK(fx.loader.isCurrent(restarted[0]));
    if (!transferred.empty()) CHECK_FALSE(fx.loader.isCurrent(transferred[0]));
    CHECK(fx.loader.pendingCount() == 0);
}

namespace {
void checkAsyncSnapshot(const AsyncLoaderSnapshot& state, bool running,
                        uint64_t pending, uint64_t inflight, uint64_t buffered,
                        uint64_t entries, uint64_t bytes) {
    CHECK(state.supported);
    CHECK(state.running == running);
    CHECK(state.pendingWaiters == pending);
    CHECK(state.inflightKeys == inflight);
    CHECK(state.completedBuffered == buffered);
    CHECK(state.cacheEntries == entries);
    CHECK(state.cacheBytes == bytes);
}

struct SnapshotAsyncEvents {
    bool ready = SDL_InitSubSystem(SDL_INIT_EVENTS);
    ~SnapshotAsyncEvents() { if (ready) SDL_QuitSubSystem(SDL_INIT_EVENTS); }
};
}

TEST_CASE("AsyncLoader U27 snapshot: waiters keys buffered results and cache stay distinct") {
    SteppedAsyncFixture fx;
    IAsyncLoader& api = fx.loader;
    auto provider = std::make_unique<MemProvider>(100, "snapshot");
    provider->put("snapshot.tga", std::vector<uint8_t>(std::begin(kRedTga), std::end(kRedTga)));
    provider->put("snapshot.bin", {'r', 'a', 'w'});
    auto* source = provider.get();
    fx.assets.addProvider(std::move(provider));
    checkAsyncSnapshot(api.getSnapshot(), true, 0, 0, 0, 0, 0);
    const int first = fx.loader.enqueue("snapshot.tga", "texture");
    const int shared = fx.loader.enqueue("snapshot.tga", "texture");
    const int raw = fx.loader.enqueue("snapshot.bin", "bytes");
    REQUIRE(first > 0);
    REQUIRE(shared > first);
    REQUIRE(raw > shared);
    REQUIRE(fx.jobs.submittedCount() == 2);
    checkAsyncSnapshot(api.getSnapshot(), true, 3, 2, 0, 0, 0);

    fx.jobs.runWork(0); // Real read/decode finished, private result not delivered.
    checkAsyncSnapshot(api.getSnapshot(), true, 3, 2, 0, 0, 0);
    fx.jobs.complete(0);
    checkAsyncSnapshot(api.getSnapshot(), true, 3, 1, 2, 1, 4);
    fx.jobs.runWork(1);
    fx.jobs.complete(1);
    checkAsyncSnapshot(api.getSnapshot(), true, 3, 0, 3, 1, 4);
    CHECK(fx.loader.pendingCount() == 3);
    const auto transferred = fx.loader.drainCompleted();
    REQUIRE(transferred.size() == 3);
    CHECK(transferred[0].id == first);
    CHECK(transferred[1].id == shared);
    CHECK(transferred[2].id == raw);
    CHECK(transferred[0].rgba == std::vector<uint8_t>{255, 0, 0, 255});
    CHECK(transferred[1].rgba == transferred[0].rgba);
    CHECK(transferred[2].data == std::vector<uint8_t>{'r', 'a', 'w'});
    checkAsyncSnapshot(api.getSnapshot(), true, 0, 0, 0, 1, 4);

    source->put("snapshot.tga", {}); // A cache hit must not reread these bytes.
    const int hit = fx.loader.enqueue("snapshot.tga", "texture");
    REQUIRE(hit > raw);
    CHECK(fx.jobs.submittedCount() == 2);
    checkAsyncSnapshot(api.getSnapshot(), true, 1, 0, 1, 1, 4);
    const auto cached = fx.loader.drainCompleted();
    REQUIRE(cached.size() == 1);
    CHECK(cached[0].id == hit);
    CHECK(cached[0].rgba == transferred[0].rgba);
    checkAsyncSnapshot(api.getSnapshot(), true, 0, 0, 0, 1, 4);
    fx.loader.cancelAll();
    checkAsyncSnapshot(api.getSnapshot(), true, 0, 0, 0, 0, 0);
    CHECK_FALSE(fx.loader.isCurrent(transferred[0]));
    CHECK(transferred[0].rgba == std::vector<uint8_t>{255, 0, 0, 255}); // Host still owns bytes.

    REQUIRE(fx.loader.enqueue("snapshot.tga", "texture") > hit);
    REQUIRE(fx.jobs.submittedCount() == 3);
    fx.jobs.runWork(2);
    fx.jobs.complete(2);
    checkAsyncSnapshot(api.getSnapshot(), true, 1, 0, 1, 0, 0);
    const auto failed = fx.loader.drainCompleted();
    REQUIRE(failed.size() == 1);
    CHECK_FALSE(failed[0].success);
    checkAsyncSnapshot(api.getSnapshot(), true, 0, 0, 0, 0, 0);
    fx.loader.shutdown();
    checkAsyncSnapshot(api.getSnapshot(), false, 0, 0, 0, 0, 0);
}

TEST_CASE("AsyncLoader U27 snapshot: cancelled containers do not claim old workers have stopped") {
    TestPaths::ScopedTempDir temp("async_snapshot_running");
    const auto file = temp.path() / "generation.tga";
    writeGenerationPixel(file, false);
    std::atomic<bool> oldWorkerFinished{false};
    SteppedAsyncFixture fx;
    auto provider = std::make_unique<SnapshotReadProvider>(file);
    auto* source = provider.get();
    fx.assets.addProvider(std::move(provider));
    const int oldId = fx.loader.enqueue("generation.tga", "texture");
    REQUIRE(oldId > 0);
    REQUIRE(fx.loader.enqueue("generation.tga", "texture") > oldId);
    auto oldWork = fx.jobs.takeWork(0);
    ScopedSnapshotWorker worker(*source, [work = std::move(oldWork), &oldWorkerFinished] {
        work();
        oldWorkerFinished = true;
    });
    REQUIRE(source->waitForFirstRead());
    checkAsyncSnapshot(fx.loader.getSnapshot(), true, 2, 1, 0, 0, 0);
    fx.loader.cancelAll();
    checkAsyncSnapshot(fx.loader.getSnapshot(), true, 0, 0, 0, 0, 0);
    CHECK_FALSE(oldWorkerFinished.load()); // Real thread remains held at the read barrier.
    CHECK_FALSE(fx.jobs.getSnapshot().supported); // Stepped scheduler is not real Job telemetry.

    writeGenerationPixel(file, true);
    const int freshId = fx.loader.enqueue("generation.tga", "texture");
    REQUIRE(freshId > oldId);
    REQUIRE(fx.jobs.submittedCount() == 2);
    fx.jobs.runWork(1);
    checkAsyncSnapshot(fx.loader.getSnapshot(), true, 1, 1, 0, 0, 0);
    worker.finish();
    CHECK(oldWorkerFinished.load());
    fx.jobs.complete(0); // Old result must not erase the fresh key or refill cache.
    checkAsyncSnapshot(fx.loader.getSnapshot(), true, 1, 1, 0, 0, 0);
    const int freshWaiter = fx.loader.enqueue("generation.tga", "texture");
    REQUIRE(freshWaiter > freshId);
    CHECK(fx.jobs.submittedCount() == 2);
    checkAsyncSnapshot(fx.loader.getSnapshot(), true, 2, 1, 0, 0, 0);
    fx.jobs.complete(1);
    checkAsyncSnapshot(fx.loader.getSnapshot(), true, 2, 0, 2, 1, 4);
    const auto fresh = fx.loader.drainCompleted();
    REQUIRE(fresh.size() == 2);
    CHECK(fresh[0].id == freshId);
    CHECK(fresh[1].id == freshWaiter);
    CHECK(fresh[0].rgba == std::vector<uint8_t>{0, 0, 255, 255});
    CHECK(fresh[1].rgba == fresh[0].rgba);
    CHECK(source->reads() == 2);
    checkAsyncSnapshot(fx.loader.getSnapshot(), true, 0, 0, 0, 1, 4);
    fx.loader.shutdown();
    checkAsyncSnapshot(fx.loader.getSnapshot(), false, 0, 0, 0, 0, 0);
}

TEST_CASE("AsyncLoader U27 snapshot: SDL and host owned payloads are outside loader containers") {
    SnapshotAsyncEvents events;
    REQUIRE_MESSAGE(events.ready, "SDL event initialization failed: ", SDL_GetError());
    SteppedAsyncFixture fx;
    auto provider = std::make_unique<MemProvider>(100, "snapshot SDL");
    provider->put("snapshot-event.bin", {'e', 'v', 't'});
    fx.assets.addProvider(std::move(provider));
    const int id = fx.loader.enqueue("snapshot-event.bin", "bytes");
    REQUIRE(id > 0);
    fx.jobs.runWork(0);
    fx.jobs.complete(0);
    checkAsyncSnapshot(fx.loader.getSnapshot(), true, 1, 0, 1, 0, 0);
    REQUIRE(fx.loader.poll());
    checkAsyncSnapshot(fx.loader.getSnapshot(), true, 0, 0, 0, 0, 0);

    struct CapturedEvent {
        AsyncLoader* owner;
        std::unique_ptr<CompletedLoad> payload;
        int count = 0;
    } captured{&fx.loader, nullptr};
    // Take only this loader's actual event; preserve every unrelated event.
    SDL_FilterEvents([](void* userdata, SDL_Event* event) -> bool {
        auto& own = *static_cast<CapturedEvent*>(userdata);
        if (event->type != CAESURA_EVENT_ASYNC_LOAD || event->user.data2 != own.owner)
            return true;
        ++own.count;
        own.payload.reset(static_cast<CompletedLoad*>(event->user.data1));
        return false;
    }, &captured);
    REQUIRE(captured.count == 1);
    REQUIRE(captured.payload != nullptr);
    CHECK(captured.payload->id == id);
    CHECK(captured.payload->success);
    CHECK(captured.payload->data == std::vector<uint8_t>{'e', 'v', 't'});
    fx.loader.shutdown();
    checkAsyncSnapshot(fx.loader.getSnapshot(), false, 0, 0, 0, 0, 0);
    CHECK_FALSE(fx.loader.isCurrent(*captured.payload));
    CHECK(captured.payload->data == std::vector<uint8_t>{'e', 'v', 't'});
}

TEST_CASE("AsyncLoader U27 snapshot: shutdown clears buffered or delayed generations") {
    bool completeBeforeShutdown = false;
    SUBCASE("private worker result awaiting completion callback") {}
    SUBCASE("completed result and cache still owned by loader") { completeBeforeShutdown = true; }
    SteppedAsyncFixture fx;
    auto provider = std::make_unique<MemProvider>(100, "snapshot shutdown");
    provider->put("snapshot-stop.tga", std::vector<uint8_t>(std::begin(kRedTga), std::end(kRedTga)));
    auto* source = provider.get();
    fx.assets.addProvider(std::move(provider));
    REQUIRE(fx.loader.enqueue("snapshot-stop.tga", "texture") > 0);
    fx.jobs.runWork(0);
    if (completeBeforeShutdown) fx.jobs.complete(0);
    checkAsyncSnapshot(fx.loader.getSnapshot(), true, 1,
                       completeBeforeShutdown ? 0 : 1,
                       completeBeforeShutdown ? 1 : 0,
                       completeBeforeShutdown ? 1 : 0,
                       completeBeforeShutdown ? 4 : 0);
    fx.loader.shutdown();
    checkAsyncSnapshot(fx.loader.getSnapshot(), false, 0, 0, 0, 0, 0);
    CHECK(fx.loader.drainCompleted().empty());
    source->put("snapshot-stop.tga", {});
    fx.loader.init();
    checkAsyncSnapshot(fx.loader.getSnapshot(), true, 0, 0, 0, 0, 0);
    REQUIRE(fx.loader.enqueue("snapshot-stop.tga", "texture") > 0);
    REQUIRE(fx.jobs.submittedCount() == 2);
    fx.jobs.runWork(1);
    fx.jobs.complete(1);
    checkAsyncSnapshot(fx.loader.getSnapshot(), true, 1, 0, 1, 0, 0);
    const auto restarted = fx.loader.drainCompleted();
    REQUIRE(restarted.size() == 1);
    CHECK_FALSE(restarted[0].success); // No stale cached success survived shutdown.
    checkAsyncSnapshot(fx.loader.getSnapshot(), true, 0, 0, 0, 0, 0);
}
