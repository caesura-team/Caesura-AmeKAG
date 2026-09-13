#include "doctest.h"
#include "resource/ResourceHandle.h"
#include "resource/DirAssetProvider.h"
#include "resource/ProviderChain.h"
#include "resource/XP3Archive.h"
#include "resource/AssetManager.h"
#include "resource/AsyncLoader.h"
#include "di/BackendRegistry.h"
#include "di/api/ThreadAssert.h"
#include "mocks/NullJobSystem.h"
#include "job/JobSystem.h"   // real multi-threaded JobSystem (concurrency dedup)
#include "TestPaths.h"
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <memory>

using namespace Caesura;    // ResourceHandle, GenerationTracker, HandleType::TEXTURE
using namespace Caesura;    // DirAssetProvider, ProviderChain

TEST_CASE("ResourceHandle::default") {
    ResourceHandle h;
    CHECK(h.id == 0);
    CHECK_FALSE(h);
}

TEST_CASE("GenerationTracker::invalidate") {
    GenerationTracker gt;
    auto h = gt.makeHandle(HandleType::TEXTURE, 42);
    CHECK(h.id == 42);
    CHECK(gt.isCurrent(h));
    gt.invalidate(HandleType::TEXTURE);
    CHECK_FALSE(gt.isCurrent(h));
}

TEST_CASE("GenerationTracker::independent types") {
    GenerationTracker gt;
    auto h1 = gt.makeHandle(HandleType::TEXTURE, 1);
    auto h2 = gt.makeHandle(HandleType::AUDIO, 1);
    gt.invalidate(HandleType::TEXTURE);
    CHECK_FALSE(gt.isCurrent(h1));
    CHECK(gt.isCurrent(h2));
}

TEST_CASE("DirAssetProvider::exists") {
    namespace fs = std::filesystem;
    fs::create_directories("test_assets");
    std::ofstream("test_assets/hello.txt") << "world";
    DirAssetProvider provider("test_assets");
    CHECK(provider.exists("hello.txt"));
    CHECK_FALSE(provider.exists("ghost.txt"));
    fs::remove_all("test_assets");
}

TEST_CASE("ProviderChain::add and check") {
    namespace fs = std::filesystem;
    fs::create_directories("test_pc");
    std::ofstream f("test_pc/data.bin", std::ios::binary);
    f.write("binary", 6);
    f.close();
    ProviderChain chain;
    chain.addProvider(std::make_unique<DirAssetProvider>("test_pc"));
    CHECK(chain.exists("data.bin"));
    CHECK_FALSE(chain.exists("nope.bin"));
    fs::remove_all("test_pc");
}

// =============================================================================
// Expanded: DirAssetProvider read + ProviderChain priority fallback
// =============================================================================

TEST_CASE("DirAssetProvider::read returns file content") {
    namespace fs = std::filesystem;
    fs::create_directories("test_read");
    { std::ofstream out("test_read/data.txt"); out << "hello world"; }
    DirAssetProvider provider("test_read");
    auto data = provider.read("data.txt");
    REQUIRE_FALSE(data.empty());
    std::string content(data.begin(), data.end());
    CHECK(content == "hello world");
    fs::remove_all("test_read");
}

TEST_CASE("DirAssetProvider::getSource and priority") {
    DirAssetProvider provider("/some/root");
    CHECK(provider.getSource().find("Dir:") != std::string::npos);
    CHECK(provider.getSource().find("/some/root") != std::string::npos);
    CHECK(provider.priority() == 5);
    CHECK(provider.verify());
}

TEST_CASE("ProviderChain::read falls back to lower priority") {
    namespace fs = std::filesystem;
    fs::create_directories("test_high");
    fs::create_directories("test_low");
    // Only write file in low-priority dir
    { std::ofstream out("test_low/fallback.txt"); out << "fallback"; }
    ProviderChain chain;
    // Both providers have default priority 5. File only in test_low —
    // tests same-priority chaining (not priority ordering).
    chain.addProvider(std::make_unique<DirAssetProvider>("test_high"));
    chain.addProvider(std::make_unique<DirAssetProvider>("test_low"));
    CHECK(chain.exists("fallback.txt"));
    auto data = chain.read("fallback.txt");
    REQUIRE_FALSE(data.empty());
    std::string content(data.begin(), data.end());
    CHECK(content == "fallback");
    fs::remove_all("test_high");
    fs::remove_all("test_low");
}

TEST_CASE("ProviderChain::providers accessor") {
    ProviderChain chain;
    CHECK(chain.providers().empty());
    chain.addProvider(std::make_unique<DirAssetProvider>("some_dir"));
    CHECK(chain.providers().size() == 1);
    chain.clear();
    CHECK(chain.providers().empty());
    CHECK_NOTHROW(chain.clear());
}

TEST_CASE("DirAssetProvider::read nonexistent returns empty") {
    namespace fs = std::filesystem;
    fs::create_directories("test_empty");
    DirAssetProvider provider("test_empty");
    auto data = provider.read("nonexistent.txt");
    CHECK(data.empty());
    fs::remove_all("test_empty");
}

TEST_CASE("DirAssetProvider confines reads to its root") {
    namespace fs = std::filesystem;
    TestPaths::ScopedTempDir temp("asset_provider_confinement");
    const fs::path root = temp.path() / "assets";
    const fs::path outside = temp.path() / "secret.txt";
    fs::create_directories(root);
    { std::ofstream secret(outside); secret << "not an asset"; }

    DirAssetProvider rooted(root.string());
    CHECK_FALSE(rooted.exists("../secret.txt"));
    CHECK(rooted.read("../secret.txt").empty());

    DirAssetProvider workingDirectoryRoot("");
    CHECK_FALSE(workingDirectoryRoot.exists(fs::absolute(outside).string()));
    CHECK(workingDirectoryRoot.read(fs::absolute(outside).string()).empty());
}

namespace {
std::string resourceUtf8(const std::filesystem::path& path) {
    const auto bytes = path.generic_u8string();
    return std::string(bytes.begin(), bytes.end());
}

void writeResourceBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    REQUIRE(output.is_open());
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    REQUIRE(output.good());
}
}

TEST_CASE("U21 resource paths: directory provider reads UTF-8 names and roots") {
    namespace fs = std::filesystem;
    TestPaths::ScopedTempDir temp("u21_provider_utf8");
    fs::path rootName = "root";
    fs::path relative = "data.bin";
    SUBCASE("ASCII control") {}
    SUBCASE("UTF-8 directory") { relative = u8"\u65c5\u7a0b/data.bin"; }
    SUBCASE("UTF-8 basename outside the ANSI repertoire") { relative = u8"\u56fe\u50cf-\U0001f3b5.bin"; }
    SUBCASE("UTF-8 provider root") { rootName = u8"\u8d44\u6e90\u6839-\U0001f3b5"; }
    const fs::path root = temp.path() / rootName;
    const std::vector<uint8_t> expected{0, 1, 2, 127, 128, 255};
    writeResourceBytes(root / relative, expected);
    const fs::path outside = temp.path() / "outside.bin";
    writeResourceBytes(outside, std::vector<uint8_t>{99});
    DirAssetProvider provider(resourceUtf8(root));
    const std::string requested = resourceUtf8(relative);
    CAPTURE(requested);
    bool exists = false;
    std::vector<uint8_t> actual;
    CHECK_NOTHROW(exists = provider.exists(requested));
    CHECK(exists);
    CHECK_NOTHROW(actual = provider.read(requested));
    CHECK(actual == expected);
    CHECK_FALSE(provider.exists("missing.bin"));
    CHECK(provider.read("missing.bin").empty());
    CHECK_FALSE(provider.exists("../outside.bin"));
    CHECK(provider.read("../outside.bin").empty());
    CHECK_FALSE(provider.exists(resourceUtf8(outside)));
    CHECK(provider.read(resourceUtf8(outside)).empty());
    CHECK(provider.read(requested) == expected);
}

TEST_CASE("U21 resource paths: asset reader preserves UTF-8 bytes and size guards") {
    namespace fs = std::filesystem;
    TestPaths::ScopedTempDir temp("u21_reader_utf8");
    fs::path rootName = "root";
    fs::path relative = "u21-reader/data.bin";
    SUBCASE("ASCII control") {}
    SUBCASE("UTF-8 directory") { relative = u8"u21-reader/\u65c5\u7a0b/data.bin"; }
    SUBCASE("UTF-8 basename") { relative = u8"u21-reader/\u8d44\u6e90-\U0001f3b5.bin"; }
    SUBCASE("UTF-8 provider root") { rootName = u8"\u8bfb\u53d6\u6839-\U0001f3b5"; }
    const fs::path root = temp.path() / rootName;
    const std::vector<uint8_t> expected{0, 1, 2, 127, 128, 255};
    writeResourceBytes(root / relative, expected);
    const std::string requested = resourceUtf8(relative);
    CAPTURE(requested);
    // Ensure no ambient default provider can satisfy this positive control.
    REQUIRE_FALSE(fs::exists(relative));
    REQUIRE_FALSE(fs::exists(fs::path("assets") / relative));
    AssetManager assets;
    assets.init();
    assets.addProvider(std::make_unique<DirAssetProvider>(resourceUtf8(root)));
    IAssetReader& reader = assets;
    CHECK(reader.readAsset(requested, expected.size()) == expected);
    CHECK(reader.readAsset(requested, expected.size() - 1).empty());
    CHECK(reader.readAsset(requested, 0).empty());
    CHECK(reader.readAsset("u21-reader/missing.bin", 64).empty());
    CHECK(reader.readAsset("../outside.bin", 64).empty());
    CHECK(reader.readAsset(resourceUtf8(root / relative), 64).empty());
    CHECK(reader.readAsset(std::string("u21-reader/\0data.bin", 20), 64).empty());
    CHECK(reader.readAsset(requested, 64) == expected);
}

#ifdef _WIN32
TEST_CASE("U21 resource paths: invalid Windows UTF-8 fails without throwing") {
    namespace fs = std::filesystem;
    TestPaths::ScopedTempDir temp("u21_provider_invalid_utf8");
    const fs::path root = temp.path() / "root";
    const std::vector<uint8_t> expected{7, 8, 9};
    writeResourceBytes(root / "valid.bin", expected);
    DirAssetProvider provider(resourceUtf8(root));
    const std::string invalid[] = {"\xc0\xaf", "\xed\xa0\x80", "\xff", "\xe4\xb8"};
    for (const auto& bytes : invalid) {
        const std::string requested = "bad-" + bytes + ".bin";
        bool exists = false;
        std::vector<uint8_t> data;
        CHECK_NOTHROW(exists = provider.exists(requested));
        CHECK_FALSE(exists);
        CHECK_NOTHROW(data = provider.read(requested));
        CHECK(data.empty());
        DirAssetProvider badRoot(resourceUtf8(root) + "/" + bytes);
        CHECK_NOTHROW(exists = badRoot.exists("valid.bin"));
        CHECK_FALSE(exists);
        CHECK_NOTHROW(data = badRoot.read("valid.bin"));
        CHECK(data.empty());
    }
    CHECK(provider.read("valid.bin") == expected);
}
#endif

TEST_CASE("XP3Archive rejects raw segment size mismatch") {
    namespace fs = std::filesystem;
    TestPaths::ScopedTempDir temp("xp3_raw_size_mismatch");
    const fs::path archivePath = temp.path() / "malformed.xp3";
    const fs::path outputDir = temp.path() / "output";

    std::vector<uint8_t> bytes;
    auto appendU32 = [&bytes](uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<uint8_t>(value >> shift));
    };
    auto appendU64 = [&bytes](uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8)
            bytes.push_back(static_cast<uint8_t>(value >> shift));
    };

    const char magic[] = "XP3\r\n";
    bytes.insert(bytes.end(), magic, magic + 5);
    appendU64(14); // one data byte starts at offset 13; index starts at 14
    bytes.push_back('X');

    appendU32(0); // file flags
    appendU64(4); // declared original file size
    appendU64(1); // declared archived file size
    bytes.push_back('a'); bytes.push_back(0); // UTF-16LE filename
    bytes.push_back(0);   bytes.push_back(0); // terminator
    appendU32(1); // one segment
    appendU32(XP3Archive::XP3_ENC_RAW);
    appendU64(13); // data offset
    appendU64(4);  // original segment size
    appendU64(1);  // archived segment size

    std::ofstream out(archivePath, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();

    CHECK_FALSE(XP3Archive::unpack(archivePath.string(), outputDir.string()));
    CHECK_FALSE(fs::exists(outputDir / "a"));
}

TEST_CASE("XP3Archive pack list and unpack round-trip") {
    namespace fs = std::filesystem;
    TestPaths::ScopedTempDir temp("xp3_roundtrip");
    const fs::path inputDir = temp.path() / "input";
    const fs::path outputDir = temp.path() / "output";
    const fs::path archivePath = temp.path() / "roundtrip.xp3";
    fs::create_directories(inputDir / "nested");
    {
        std::ofstream source(inputDir / "nested" / "story.txt", std::ios::binary);
        source << "Caesura XP3 round-trip payload";
    }

    REQUIRE(XP3Archive::pack(inputDir.string(), archivePath.string()));
    const auto entries = XP3Archive::list(archivePath.string());
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].orgSize == 30);

    REQUIRE(XP3Archive::unpack(archivePath.string(), outputDir.string()));
    std::ifstream extracted(outputDir / "nested" / "story.txt", std::ios::binary);
    REQUIRE(extracted.is_open());
    const std::string contents((std::istreambuf_iterator<char>(extracted)),
                               std::istreambuf_iterator<char>());
    CHECK(contents == "Caesura XP3 round-trip payload");
}

// =============================================================================
// G10: resource module boundary tests (provider chain + async loader)
// =============================================================================

namespace {

// In-memory IAssetProvider for testing the ProviderChain boundary without
// touching the filesystem. exists()/read() serve a small map; a
// provider can be forced to report exists()==true while read() stays empty to
// exercise the chain's "empty read falls through" contract.
class MemProvider : public IAssetProvider {
public:
    MemProvider(int prio, std::string source)
        : m_prio(prio), m_source(std::move(source)) {}

    void put(std::string path, std::vector<uint8_t> data) {
        m_files[std::move(path)] = std::move(data);
    }
    void clear() { m_files.clear(); }
    void forceExists(bool v) { m_forceExists = v; }
    std::string source() const { return m_source; }

    bool exists(const std::string& path) override {
        return m_forceExists || m_files.find(path) != m_files.end();
    }
    std::vector<uint8_t> read(const std::string& path) override {
        auto it = m_files.find(path);
        if (it == m_files.end()) return {};
        return it->second;
    }
    std::string getSource() const override { return m_source; }
    int priority() const override { return m_prio; }
    bool verify() override { return true; }

private:
    int m_prio;
    std::string m_source;
    bool m_forceExists = false;
    std::map<std::string, std::vector<uint8_t>> m_files;
};

// Synchronous (NullJobSystem) AsyncLoader fixture. All loads complete
// deterministically on the calling thread, so payload/failure assertions are
// exact. Registers the job system in BackendRegistry and always restores it.
class AsyncFixtures {
public:
    AsyncFixtures()
        : loader(&assets) {
        detail::g_mainThreadId = std::this_thread::get_id();
        jobs.init();
        BackendRegistry::instance().setJobSystem(&jobs);
        assets.init();
        loader.init();
    }
    ~AsyncFixtures() {
        loader.shutdown();
        assets.shutdown();
        BackendRegistry::instance().setJobSystem(nullptr);
        jobs.shutdown();
    }

    NullJobSystem jobs;
    AssetManager assets;
    AsyncLoader loader;
};

} // namespace

TEST_CASE("ProviderChain::fallback first missing second serves") {
    auto high = std::make_unique<MemProvider>(10, "high");
    auto low  = std::make_unique<MemProvider>(5, "low");
    low->put("asset.dat", {'h', 'i'});
    // high has nothing for asset.dat yet the chain must still resolve it
    // from low via fallback.
    ProviderChain chain;
    chain.addProvider(std::move(high));
    chain.addProvider(std::move(low));

    CHECK(chain.exists("asset.dat"));
    const auto data = chain.read("asset.dat");
    REQUIRE(data.size() == 2);
    CHECK(data[0] == 'h');
    CHECK(data[1] == 'i');
}

TEST_CASE("ProviderChain::fallback empty read falls through") {
    // First provider wrongly claims exists() but read() returns empty; the
    // chain must treat that as "not servable" and fall through to the next.
    auto high = std::make_unique<MemProvider>(10, "high");
    high->forceExists(true);          // exists()==true, read() empty
    auto low = std::make_unique<MemProvider>(5, "low");
    low->put("x.bin", {'a'});

    ProviderChain chain;
    chain.addProvider(std::move(high));
    chain.addProvider(std::move(low));

    const auto data = chain.read("x.bin");
    REQUIRE(data.size() == 1);
    CHECK(data[0] == 'a');
}

TEST_CASE("ProviderChain::priority resolves highest first") {
    auto top = std::make_unique<MemProvider>(10, "top");
    top->put("k", {'1'});
    auto mid = std::make_unique<MemProvider>(5, "mid");
    mid->put("k", {'2'});
    auto low = std::make_unique<MemProvider>(1, "low");
    low->put("k", {'3'});

    ProviderChain chain;
    // Insert out of priority order: sortByPriority() must fix ordering.
    chain.addProvider(std::move(low));
    chain.addProvider(std::move(mid));
    chain.addProvider(std::move(top));

    const auto data = chain.read("k");
    REQUIRE(data.size() == 1);
    CHECK(data[0] == '1');   // highest priority wins

    // When the top provider is dropped, the same chain resolves from mid.
    // (Rebuild with only mid+low to avoid stale pointers.)
    ProviderChain chain2;
    auto mid2 = std::make_unique<MemProvider>(5, "mid");
    mid2->put("k", {'2'});
    auto low2 = std::make_unique<MemProvider>(1, "low");
    low2->put("k", {'3'});
    chain2.addProvider(std::move(mid2));
    chain2.addProvider(std::move(low2));
    REQUIRE(chain2.read("k").size() == 1);
    CHECK(chain2.read("k")[0] == '2');
}

TEST_CASE("ProviderChain no provider serves -> empty, no crash") {
    ProviderChain chain;
    auto p = std::make_unique<MemProvider>(5, "only");
    p->put("present.bin", {'z'});
    chain.addProvider(std::move(p));

    CHECK(chain.read("missing.bin").empty());
    CHECK_FALSE(chain.exists("missing.bin"));
    CHECK_NOTHROW(chain.read("present.bin"));
}

// ---- Async loader: completion, error path, cancellation, dedup -------------

TEST_CASE("AsyncLoader completion delivers loaded payload") {
    const std::string path = "res_g10_payload.bin";
    { std::ofstream out(path, std::ios::binary); out << "hello async bytes"; }
    {
        AsyncFixtures fx;
        int id = fx.loader.enqueue(path, "text");
        REQUIRE(id > 0);
        auto done = fx.loader.drainCompleted();
        REQUIRE_FALSE(done.empty());
        CHECK(done[0].id == id);
        CHECK(done[0].success);
        CHECK_FALSE(done[0].data.empty());
        std::string s(done[0].data.begin(), done[0].data.end());
        CHECK(s == "hello async bytes");
        CHECK(fx.loader.pendingCount() == 0);
    }
    std::remove(path.c_str());
}

TEST_CASE("AsyncLoader missing asset reports failure") {
    const std::string path = "res_g10_does_not_exist_9f3.bin";
    { // ensure the missing path really is absent
        std::remove(path.c_str());
    }
    {
        AsyncFixtures fx;
        int id = fx.loader.enqueue(path, "text");
        REQUIRE(id > 0);
        auto done = fx.loader.drainCompleted();
        REQUIRE_FALSE(done.empty());
        CHECK(done[0].id == id);
        CHECK_FALSE(done[0].success);  // error path surfaces, no crash
        CHECK(done[0].data.empty());
        CHECK(fx.loader.pendingCount() == 0);
    }
}

TEST_CASE("AsyncLoader cancelAll is safe/idempotent and resets") {
    {
        const std::string path = "res_g10_cancel.bin";
        { std::ofstream out(path, std::ios::binary); out << "cancel me"; }
        {
            AsyncFixtures fx;
            int id = fx.loader.enqueue(path, "text");
            REQUIRE(id > 0);
            fx.loader.cancelAll();
            fx.loader.cancelAll();               // idempotent
            // drain is safe after cancel; no crash, counters stay sane.
            auto done = fx.loader.drainCompleted();
            (void)done;
            CHECK(fx.loader.pendingCount() == 0);

            // New enqueue after cancel delivers normally (flag is reset).
            int id2 = fx.loader.enqueue(path, "text");
            REQUIRE(id2 > 0);
            auto done2 = fx.loader.drainCompleted();
            REQUIRE_FALSE(done2.empty());
            CHECK(done2[0].success);
        }
        std::remove(path.c_str());
    }
}

// Minimal validated 1x1 red PNG (same bytes used by test_async).
static const uint8_t kResG10RedPng[] = {
    0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,
    0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
    0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,
    0x08,0x02,0x00,0x00,0x00,0x90,0x77,0x53,
    0xDE,0x00,0x00,0x00,0x0D,0x49,0x44,0x41,
    0x54,0x78,0x9C,0x63,0xF8,0xCF,0xC0,0xF0,
    0x1F,0x00,0x05,0x00,0x01,0xFF,0x89,0x99,
    0x3D,0x1D,0x00,0x00,0x00,0x00,0x49,0x45,
    0x4E,0x44,0xAE,0x42,0x60,0x82
};

TEST_CASE("AsyncLoader dedup: same asset twice shares one load") {
    const std::string path = "res_g10_dedup.png";
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(kResG10RedPng),
                  static_cast<std::streamsize>(sizeof(kResG10RedPng)));
    }
    {
        AsyncFixtures fx;
        // First request decodes and caches the texture.
        int id1 = fx.loader.enqueue(path, "texture");
        REQUIRE(id1 > 0);
        auto first = fx.loader.drainCompleted();
        REQUIRE_FALSE(first.empty());
        REQUIRE(first[0].success);
        REQUIRE_FALSE(first[0].rgba.empty());

        // Identical (path,type) again: served from the decode cache, a fresh
        // id but a shared identical payload (one load, not two).
        int id2 = fx.loader.enqueue(path, "texture");
        REQUIRE(id2 > 0);
        auto second = fx.loader.drainCompleted();
        REQUIRE_FALSE(second.empty());
        CHECK(second[0].id == id2);
        CHECK(second[0].success);
        CHECK(second[0].rgba == first[0].rgba);
        CHECK(second[0].width  == first[0].width);
        CHECK(second[0].height == first[0].height);
    }
    std::remove(path.c_str());
}

// =============================================================================
// Round 78 second wave: boundary tests for the provider chain + AsyncLoader.
// Focus: chain depth / priority override / exception behavior, queue capacity,
// cache semantics (invalidation, no-reload, same-content distinct paths),
// error retry, lifecycle, and path normalization (traversal rejection).
// =============================================================================

namespace {

// Spy provider: counts how many times exists()/read() were invoked, so tests
// can assert priority-override (a low-priority provider must never be touched
// when a higher-priority one serves) with exact call accounting.
class SpyProvider : public IAssetProvider {
public:
    SpyProvider(int prio, std::string source, bool present, std::vector<uint8_t> payload = {})
        : m_prio(prio), m_source(std::move(source)), m_present(present), m_payload(std::move(payload)) {}

    int existsCalls() const { return m_existsCalls; }
    int readCalls() const { return m_readCalls; }

    bool exists(const std::string&) override {
        ++m_existsCalls;
        return m_present;
    }
    std::vector<uint8_t> read(const std::string&) override {
        ++m_readCalls;
        return m_payload;
    }
    std::string getSource() const override { return m_source; }
    int priority() const override { return m_prio; }
    bool verify() override { return true; }

private:
    int m_prio;
    std::string m_source;
    bool m_present;
    std::vector<uint8_t> m_payload;
    int m_existsCalls = 0;
    int m_readCalls = 0;
};

// Provider whose read()/exists() throws, to exercise ProviderChain behavior
// when a member provider faults.
class ThrowingProvider : public IAssetProvider {
public:
    explicit ThrowingProvider(int prio, std::string source, bool throwOnExists = false)
        : m_prio(prio), m_source(std::move(source)), m_throwOnExists(throwOnExists) {}

    bool exists(const std::string&) override {
        if (m_throwOnExists) throw std::runtime_error("exists() fault");
        return true;
    }
    std::vector<uint8_t> read(const std::string&) override {
        throw std::runtime_error("read() fault");
    }
    std::string getSource() const override { return m_source; }
    int priority() const override { return m_prio; }
    bool verify() override { return true; }

private:
    int m_prio;
    std::string m_source;
    bool m_throwOnExists;
};

} // namespace

// ---- Provider chain depth & priority override -------------------------------

TEST_CASE("ProviderChain deep 5-provider fallback honors priority order") {
    // A(10),B(8),C(6),D(4),E(2). Only E (lowest priority) holds the file, so
    // the chain must walk through A,B,C,D (each a miss) and resolve from E.
    // Inserted out of order to prove addProvider() re-sorts by priority.
    ProviderChain chain;
    auto e = std::make_unique<MemProvider>(2, "E");
    e->put("deep.bin", {'E'});
    auto a = std::make_unique<MemProvider>(10, "A");
    auto d = std::make_unique<MemProvider>(4, "D");
    auto c = std::make_unique<MemProvider>(6, "C");
    auto b = std::make_unique<MemProvider>(8, "B");
    chain.addProvider(std::move(e));
    chain.addProvider(std::move(a));
    chain.addProvider(std::move(d));
    chain.addProvider(std::move(c));
    chain.addProvider(std::move(b));

    REQUIRE(chain.providers().size() == 5);
    // Sorted order must be A(10),B(8),C(6),D(4),E(2).
    CHECK(chain.providers()[0]->priority() == 10);
    CHECK(chain.providers()[1]->priority() == 8);
    CHECK(chain.providers()[2]->priority() == 6);
    CHECK(chain.providers()[3]->priority() == 4);
    CHECK(chain.providers()[4]->priority() == 2);

    CHECK(chain.exists("deep.bin"));
    const auto data = chain.read("deep.bin");
    REQUIRE(data.size() == 1);
    CHECK(data[0] == 'E');
}

TEST_CASE("ProviderChain priority override: low provider not called when high serves") {
    ProviderChain chain;
    auto high = std::make_unique<SpyProvider>(10, "high", /*present*/ true, std::vector<uint8_t>{'H'});
    auto low  = std::make_unique<SpyProvider>(1, "low",  /*present*/ true, std::vector<uint8_t>{'L'});
    auto* lowRaw  = low.get();
    auto* highRaw = high.get();
    chain.addProvider(std::move(high));
    chain.addProvider(std::move(low));

    // read() checks exists() then read() on the highest provider that serves.
    const auto data = chain.read("asset.bin");
    REQUIRE(data.size() == 1);
    CHECK(data[0] == 'H');
    CHECK(highRaw->readCalls() == 1);
    // The low-priority provider must never be consulted at all (exists or read).
    CHECK(lowRaw->existsCalls() == 0);
    CHECK(lowRaw->readCalls() == 0);
}

TEST_CASE("ProviderChain exists() short-circuits at highest hit") {
    ProviderChain chain;
    auto high = std::make_unique<SpyProvider>(10, "high", /*present*/ true, std::vector<uint8_t>{'H'});
    auto mid  = std::make_unique<SpyProvider>(5,  "mid",  /*present*/ true, std::vector<uint8_t>{'M'});
    auto* midRaw = mid.get();
    chain.addProvider(std::move(high));
    chain.addProvider(std::move(mid));

    // exists() returns true immediately on the first provider that has it --
    // the lower provider is never queried.
    CHECK(chain.exists("x.bin"));
    CHECK(midRaw->existsCalls() == 0);
}

TEST_CASE("ProviderChain throwing provider is isolated and read falls back") {
    // A member that throws is treated as "not servable": the exception is
    // swallowed (matching AsyncLoader/NullJobSystem isolation) and the chain
    // falls through to the next provider. The throwing provider reports
    // exists()==true but its read() throws -- that fault must not escape.
    ProviderChain chain;
    chain.addProvider(std::make_unique<ThrowingProvider>(10, "bad"));
    auto low = std::make_unique<MemProvider>(5, "good");
    low->put("x.bin", {'a'});
    chain.addProvider(std::move(low));

    CHECK(chain.exists("x.bin"));   // resolved by the low provider
    const auto data = chain.read("x.bin");
    REQUIRE(data.size() == 1);
    CHECK(data[0] == 'a');
    CHECK_NOTHROW(chain.read("x.bin"));
}

TEST_CASE("ProviderChain high-priority throwing provider still falls back") {
    // Even when the HIGHEST-priority provider faults, the chain must not
    // propagate and must still serve from the lower-priority provider.
    ProviderChain chain;
    chain.addProvider(std::make_unique<ThrowingProvider>(10, "bad"));
    auto low = std::make_unique<MemProvider>(1, "good");
    low->put("asset.bin", {'L'});
    chain.addProvider(std::move(low));

    CHECK(chain.exists("asset.bin"));
    const auto data = chain.read("asset.bin");
    REQUIRE(data.size() == 1);
    CHECK(data[0] == 'L');
}

TEST_CASE("ProviderChain all providers throwing fails gracefully") {
    // When every provider faults, read()/exists() swallow and return the
    // "no result" contract (empty / false) -- never propagate. throwOnExists
    // makes exists() fault too, so both paths are exercised.
    ProviderChain chain;
    chain.addProvider(std::make_unique<ThrowingProvider>(10, "bad1", /*throwOnExists*/ true));
    chain.addProvider(std::make_unique<ThrowingProvider>(1, "bad2", /*throwOnExists*/ true));

    CHECK_FALSE(chain.exists("x.bin"));
    CHECK(chain.read("x.bin").empty());
    CHECK_NOTHROW(chain.read("x.bin"));
    CHECK_NOTHROW(chain.exists("x.bin"));
}

TEST_CASE("ProviderChain exceptions in lower-priority provider stay masked when high serves") {
    // High-priority provider serves; the throwing provider sits at lower
    // priority and is never consulted, so no exception escapes.
    ProviderChain chain;
    auto high = std::make_unique<MemProvider>(10, "good");
    high->put("safe.bin", {'s'});
    chain.addProvider(std::make_unique<ThrowingProvider>(1, "bad"));
    chain.addProvider(std::move(high));

    const auto data = chain.read("safe.bin");
    REQUIRE(data.size() == 1);
    CHECK(data[0] == 's');
}

// ---- AsyncLoader: queue capacity, cache semantics, lifecycle ----------------

TEST_CASE("AsyncLoader queue capacity: 16 pending max, excess rejected") {
    namespace fs = std::filesystem;
    fs::create_directories("res_cap");
    for (int i = 0; i < 20; ++i) {
        std::ofstream out("res_cap/f" + std::to_string(i) + ".txt");
        out << "payload-" << i;
    }
    {
        AsyncFixtures fx;
        int accepted = 0, rejected = 0;
        for (int i = 0; i < 20; ++i) {
            const int id = fx.loader.enqueue("res_cap/f" + std::to_string(i) + ".txt", "text");
            if (id > 0) ++accepted;
            else ++rejected;
        }
        // Everything completed synchronously but is still "pending" until
        // drained, so the 16-entry cap is what gates the 17th..20th enqueues.
        CHECK(accepted == 16);
        CHECK(rejected == 4);
        CHECK(fx.loader.pendingCount() <= 16);

        auto done = fx.loader.drainCompleted();
        REQUIRE(done.size() == static_cast<size_t>(accepted));
        for (const auto& c : done) {
            CHECK(c.success);
            CHECK_FALSE(c.data.empty());
        }
        CHECK(fx.loader.pendingCount() == 0);
    }
    fs::remove_all("res_cap");
}

TEST_CASE("AsyncLoader cache hit does not re-read source file") {
    // After a texture is cached, delete it from disk; a re-enqueue of the same
    // (path,type) must still succeed from the resident cache -- proving the
    // loader does NOT re-open the (now missing) source file.
    const std::string path = "res_cachereload.png";
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(kResG10RedPng),
                  static_cast<std::streamsize>(sizeof(kResG10RedPng)));
    }
    {
        AsyncFixtures fx;
        int id1 = fx.loader.enqueue(path, "texture");
        REQUIRE(id1 > 0);
        auto first = fx.loader.drainCompleted();
        REQUIRE_FALSE(first.empty());
        REQUIRE(first[0].success);
        REQUIRE_FALSE(first[0].rgba.empty());

        // Source disappears while the decoded payload stays resident.
        std::remove(path.c_str());

        int id2 = fx.loader.enqueue(path, "texture");
        REQUIRE(id2 > 0);
        auto second = fx.loader.drainCompleted();
        REQUIRE_FALSE(second.empty());
        CHECK(second[0].success);               // served from cache, not disk
        CHECK(second[0].rgba == first[0].rgba);
        CHECK(second[0].width  == first[0].width);
        CHECK(second[0].height == first[0].height);
    }
    // File already removed above; tolerate absence.
    std::remove(path.c_str());
}

TEST_CASE("AsyncLoader cache: reload after cancelAll-invalidate") {
    const std::string path = "res_invalidate.png";
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(kResG10RedPng),
                  static_cast<std::streamsize>(sizeof(kResG10RedPng)));
    }
    {
        AsyncFixtures fx;
        int id1 = fx.loader.enqueue(path, "texture");
        REQUIRE(id1 > 0);
        auto first = fx.loader.drainCompleted();
        REQUIRE_FALSE(first.empty());
        REQUIRE(first[0].success);

        // cancelAll clears the decode cache (contract: invalidate everything).
        fx.loader.cancelAll();

        // Re-enqueue same (path,type): must re-load from source (cache gone).
        int id2 = fx.loader.enqueue(path, "texture");
        REQUIRE(id2 > 0);
        auto second = fx.loader.drainCompleted();
        REQUIRE_FALSE(second.empty());
        CHECK(second[0].success);
        CHECK(second[0].id != id1);
        // Distinct load id proves the source (not the cache) served it.
        CHECK(second[0].id == id2);
    }
    std::remove(path.c_str());
}

TEST_CASE("AsyncLoader different paths with same content are distinct cache entries") {
    const std::string pA = "res_same_a.png";
    const std::string pB = "res_same_b.png";
    {
        std::ofstream outA(pA, std::ios::binary);
        outA.write(reinterpret_cast<const char*>(kResG10RedPng),
                   static_cast<std::streamsize>(sizeof(kResG10RedPng)));
        std::ofstream outB(pB, std::ios::binary);
        outB.write(reinterpret_cast<const char*>(kResG10RedPng),
                   static_cast<std::streamsize>(sizeof(kResG10RedPng)));
    }
    {
        AsyncFixtures fx;
        int idA = fx.loader.enqueue(pA, "texture");
        REQUIRE(idA > 0);
        auto a = fx.loader.drainCompleted();
        REQUIRE(a.size() == 1);
        REQUIRE(a[0].success);

        int idB = fx.loader.enqueue(pB, "texture");
        REQUIRE(idB > 0);
        auto b = fx.loader.drainCompleted();
        REQUIRE(b.size() == 1);
        CHECK(b[0].success);

        // Keys are (path,type) pairs, so identical bytes under two names are
        // cached separately and both load successfully.
        CHECK(a[0].id == idA);
        CHECK(b[0].id == idB);
        CHECK(a[0].path != b[0].path);
        CHECK(b[0].rgba == a[0].rgba);   // same decoded content
    }
    std::remove(pA.c_str());
    std::remove(pB.c_str());
}

TEST_CASE("AsyncLoader error then retry with file present succeeds") {
    const std::string path = "res_retry.txt";
    std::remove(path.c_str());
    {
        AsyncFixtures fx;
        // First attempt: asset missing -> failure surfaces.
        int id1 = fx.loader.enqueue(path, "text");
        REQUIRE(id1 > 0);
        auto fail = fx.loader.drainCompleted();
        REQUIRE(fail.size() == 1);
        CHECK_FALSE(fail[0].success);

        // Create the file, then a fresh request must succeed (retry recovers).
        { std::ofstream out(path, std::ios::binary); out << "now it exists"; }
        int id2 = fx.loader.enqueue(path, "text");
        REQUIRE(id2 > 0);
        auto ok = fx.loader.drainCompleted();
        REQUIRE(ok.size() == 1);
        CHECK(ok[0].success);
        std::string s(ok[0].data.begin(), ok[0].data.end());
        CHECK(s == "now it exists");
    }
    std::remove(path.c_str());
}

TEST_CASE("AsyncLoader lifecycle: enqueue before init returns -1") {
    // A loader that was never init()'d must reject work. Not running => -1.
    AssetManager assets;
    AsyncLoader loader(&assets);
    // Do NOT call loader.init() -- exercise the not-running guard.
    CHECK(loader.enqueue("whatever.bin", "text") == -1);
    CHECK(loader.pendingCount() == 0);
    // shutdown() on a never-started loader is a safe no-op.
    CHECK_NOTHROW(loader.shutdown());
}

TEST_CASE("AsyncLoader twice back-to-back same path: first loads, second cache-hit id") {
    const std::string path = "res_back2back.png";
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(kResG10RedPng),
                  static_cast<std::streamsize>(sizeof(kResG10RedPng)));
    }
    {
        AsyncFixtures fx;
        int id1 = fx.loader.enqueue(path, "texture");
        REQUIRE(id1 > 0);
        int id2 = fx.loader.enqueue(path, "texture");
        REQUIRE(id2 > 0);
        // IDs are strictly increasing and distinct.
        CHECK(id2 > id1);
        auto done = fx.loader.drainCompleted();
        REQUIRE(done.size() == 2);
        // One entry is the real load, the other the cache hit; both succeed.
        CHECK(done[0].success);
        CHECK(done[1].success);
        CHECK_FALSE(done[0].rgba.empty());
        CHECK(done[1].rgba == done[0].rgba);
    }
    std::remove(path.c_str());
}

// ---- Path normalization / traversal rejection --------------------------------

TEST_CASE("AsyncLoader rejects parent-traversal paths") {
    AsyncFixtures fx;
    // isPathSafe() rejects any path containing ".." -- both "/" and "\"
    // separators, and embedded forms.
    CHECK(fx.loader.enqueue("../escape.bin", "text") == -1);
    CHECK(fx.loader.enqueue("a/../b.bin", "text") == -1);
    CHECK(fx.loader.enqueue("..\\escape.bin", "text") == -1);
    CHECK(fx.loader.enqueue("..", "text") == -1);
    // Leading-dot relative paths with no ".." are NOT a traversal and pass.
    const std::string flat = "res_dot_ok.txt";
    { std::ofstream out(flat, std::ios::binary); out << "dot ok"; }
    CHECK(fx.loader.enqueue("./" + flat, "text") > 0);
    // (Note: the provider reads with the literal key "./res_dot_ok.txt", which
    //  the filesystem normalizes to the same file on POSIX-style stat())

    std::remove(flat.c_str());
}

// =============================================================================
// Round 90 gap: TRUE concurrency dedup. The completion cache cannot collapse
// two requests that overlap in flight -- only a per-(path,type) in-flight table
// can. This test therefore drives a REAL (multi-threaded) JobSystem with a
// provider that holds the first read open, so the second enqueue is guaranteed
// to arrive while the first load is still running. It asserts the source is
// read exactly once.
// =============================================================================

namespace {

// Provider that counts source reads and can hold one specific read open until
// a barrier is released. Lets a test pin a load "in flight" while a second
// concurrent enqueue fires.
class BlockingCountProvider : public IAssetProvider {
public:
    BlockingCountProvider(std::string path, std::vector<uint8_t> payload)
        : m_pathToBlock(std::move(path)), m_payload(std::move(payload)) {}

    int readCount() const { return m_readCount.load(); }

    // Main-thread + workers: block until read() has been entered >= target
    // times. The counter is monotonic (never reset), so callers can wait for a
    // later read (e.g. a retry after release) by passing an absolute count.
    void waitForReads(int target, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cvStart.wait_for(lock, timeout, [this, target] {
            return m_readCount.load() >= target;
        });
    }
    void release() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_released = true;
        }
        m_cv.notify_all();
    }

    bool exists(const std::string& path) override { return path == m_pathToBlock; }
    std::vector<uint8_t> read(const std::string& path) override {
        m_readCount.fetch_add(1);
        m_cvStart.notify_all();
        if (path == m_pathToBlock) {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return m_released; });
        }
        return m_payload;
    }
    std::string getSource() const override { return "BlockingCountProvider"; }
    int priority() const override { return 100; }
    bool verify() override { return true; }

private:
    std::string m_pathToBlock;
    std::vector<uint8_t> m_payload;
    std::atomic<int> m_readCount{0};
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::condition_variable m_cvStart;
    bool m_released = false;
};

// Real-JobSystem AsyncLoader fixture: true off-thread workers. Completion
// callbacks are discharged on the main thread via pollMainThreadJobs() (the
// test has no SDL event loop, so drainCompleted() is the delivery channel).
class RealAsyncFixtures {
public:
    RealAsyncFixtures()
        : loader(&assets) {
        detail::g_mainThreadId = std::this_thread::get_id();
        jobs.init();
        BackendRegistry::instance().setJobSystem(&jobs);
        assets.init();
        loader.init();
    }
    ~RealAsyncFixtures() {
        loader.shutdown();
        assets.shutdown();
        BackendRegistry::instance().setJobSystem(nullptr);
        jobs.shutdown();
    }

    JobSystem jobs;
    AssetManager assets;
    AsyncLoader loader;
};

} // namespace

TEST_CASE("AsyncLoader in-flight dedup: concurrent same-path enqueues read source once") {
    const std::string path = "dedup_concurrent.dat";
    std::vector<uint8_t> payload = { 'c', 'o', 'n', 'c', 'u', 'r' };
    {
        RealAsyncFixtures fx;
        // Counting+blocking provider sits at priority 100, above the default
        // DirAssetProviders, so the loader's read goes through THIS provider.
        auto prov = std::make_unique<BlockingCountProvider>(path, payload);
        BlockingCountProvider* raw = prov.get();
        fx.assets.addProvider(std::move(prov));

        // First enqueue: a worker picks it up and blocks inside read().
        int id1 = fx.loader.enqueue(path, "text");
        REQUIRE(id1 > 0);
        raw->waitForReads(1, std::chrono::seconds(5));
        REQUIRE(raw->readCount() == 1);   // first job is genuinely reading the source

        // Second enqueue while the first is in flight: must share that load
        // (per-(path,type) in-flight dedup), never submit a second job.
        int id2 = fx.loader.enqueue(path, "text");
        REQUIRE(id2 > 0);
        CHECK(id2 != id1);
        // Source is still held open -> a duplicate job would have read it a
        // second time; dedup means it was NOT read again.
        CHECK(raw->readCount() == 1);
        CHECK(fx.loader.pendingCount() == 2);  // two not-yet-drained enqueues

        // Release the barrier: the single load reads once and completes for both.
        raw->release();
        fx.jobs.waitIdle();
        fx.jobs.pollMainThreadJobs();

        auto done = fx.loader.drainCompleted();
        REQUIRE(done.size() == 2);
        CHECK(done[0].success);
        CHECK(done[1].success);
        CHECK(raw->readCount() == 1);   // THE assertion: source read exactly once
        CHECK(fx.loader.pendingCount() == 0);
    }
}

// In-flight dedup must not leak: once a shared load completes, a fresh enqueue
// of the same key (with the asset now served by the source, not a cache) starts
// a real new load -- the in-flight entry was released on completion.
TEST_CASE("AsyncLoader in-flight dedup: released after completion; retry loads again") {
    const std::string path = "dedup_release.dat";
    std::vector<uint8_t> payload = { 'r', 'e', 'l', 'e', 'a', 's', 'e' };
    {
        RealAsyncFixtures fx;
        auto prov = std::make_unique<BlockingCountProvider>(path, payload);
        BlockingCountProvider* raw = prov.get();
        fx.assets.addProvider(std::move(prov));

        int id1 = fx.loader.enqueue(path, "text");
        REQUIRE(id1 > 0);
        raw->waitForReads(1, std::chrono::seconds(5));
        int id2 = fx.loader.enqueue(path, "text");
        REQUIRE(id2 > 0);
        CHECK(raw->readCount() == 1);

        raw->release();
        fx.jobs.waitIdle();
        fx.jobs.pollMainThreadJobs();
        auto first = fx.loader.drainCompleted();
        REQUIRE(first.size() == 2);
        CHECK(fx.loader.pendingCount() == 0);

        // In-flight entry was released -> a new enqueue starts a fresh load
        // and reads the source again (no stale dedup).
        int id3 = fx.loader.enqueue(path, "text");
        REQUIRE(id3 > 0);
        raw->waitForReads(2, std::chrono::seconds(5));
        CHECK(raw->readCount() == 2);
        raw->release();
        fx.jobs.waitIdle();
        fx.jobs.pollMainThreadJobs();
        auto second = fx.loader.drainCompleted();
        REQUIRE(second.size() == 1);
        CHECK(second[0].success);
        CHECK(fx.loader.pendingCount() == 0);
    }
}

TEST_CASE("U11 asset reader: preparation binds the selected source and returned size") {
    AssetManager assets;
    assets.init();
    auto low = std::make_unique<SpyProvider>(100, "base", true, std::vector<uint8_t>{1,2,3,4});
    auto* lowSpy = low.get();
    assets.addProvider(std::move(low));
    IAssetReader& reader = assets;
    SUBCASE("selected data is returned and survives later provider changes") {
        auto bytes = reader.readAsset("u11/asset.bin", 4);
        CHECK(bytes == std::vector<uint8_t>{1,2,3,4});
        CHECK(lowSpy->readCalls() == 1);
        bytes.clear();
        CHECK(reader.readAsset("u11/asset.bin", 4).size() == 4);
    }
    SUBCASE("oversize result is not exposed") {
        CHECK(reader.readAsset("u11/asset.bin", 3).empty());
    }
    SUBCASE("an empty selected layer cannot expose the lower layer") {
        auto high = std::make_unique<SpyProvider>(200, "patch", true);
        auto* highSpy = high.get();
        assets.addProvider(std::move(high));
        CHECK(reader.readAsset("u11/asset.bin", 64).empty());
        CHECK(highSpy->readCalls() == 1);
        CHECK(lowSpy->readCalls() == 0);
    }
    SUBCASE("a faulting selected layer cannot expose the lower layer") {
        assets.addProvider(std::make_unique<ThrowingProvider>(200,"broken-patch"));
        CHECK(reader.readAsset("u11/asset.bin",64).empty());
        CHECK(lowSpy->readCalls() == 0);
    }
    SUBCASE("a genuine miss permits the next source") {
        assets.addProvider(std::make_unique<SpyProvider>(200,"missing-patch",false));
        CHECK(reader.readAsset("u11/asset.bin",4).size() == 4);
    }
    SUBCASE("nonportable paths reject before querying providers") {
        for (const std::string path : {"../outside", "/absolute", "C:/absolute", "u11\\file"}) {
            CHECK(reader.readAsset(path,64).empty());
        }
        CHECK(reader.readAsset(std::string("u11/\0file",9),64).empty());
        CHECK(lowSpy->existsCalls() == 0);
    }
}
