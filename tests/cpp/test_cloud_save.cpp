// test_cloud_save.cpp - HTTP cloud-save provider (C7) round trip against a
// local mock REST server: configure -> save slot -> push -> pull -> verify.
#include "doctest.h"
#include "TestPaths.h"
#include "storage/SaveManager.h"
#include "storage/AtomicSaveFile.h"
#include "storage/CloudConflictStore.h"
#include <utility>
#include "storage/HttpCloudSaveProvider.h"
#include "storage/CloudSaveProvider.h"
#include "storage/api/ISaveProvider.h"
#include "storage/api/ICloudSaveTransport.h"
#include "storage/api/ICloudSaveSnapshotTransport.h"
#include "steam/api/ISteamBackend.h"
#include "steam/NullSteamBackend.h"
#include "di/BackendRegistry.h"
#include "archive/CryptoEngine.h"
#include <httplib.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <map>
#include <nlohmann_json.hpp>
#include <thread>
#include <map>
#include <mutex>
#include <string>
#include <fstream>
#include <iterator>
#include <cstdio>
#include <atomic>
#include <future>
#include <functional>


using namespace Caesura;

namespace {

// In-memory Steam Remote Storage stand-in. Kept in an anonymous namespace with
// its own name: test_storage.cpp defines a file-scope MockSteamBackend, and two
// different definitions of the same external-linkage class would be an ODR
// violation across translation units.
class CloudMockSteam final : public ISteamBackend {
public:
    std::map<std::string, std::string> files;
    // One-shot faults at the SDK I/O boundary. A rejected write does not
    // modify that object; a short read copies and reports fewer actual bytes.
    // Defaults leave every existing cloud-save test's transport unchanged.
    int writesBeforeFailure = -1;
    int writeFailureCount = 0;
    int readsBeforeShortRead = -1;
    int shortReadCount = 0;
    std::string rejectedWriteName;
    std::string rejectedDeleteName;
    int deleteFailureCount = 0;
    bool corruptNextWrite = false;

    bool init() override { return true; }
    void shutdown() override {}
    // The in-memory transport is ready at construction in these tests.
    bool isAvailable() const override { return true; }
    void runCallbacks() override {}
    bool isOverlayActive() const override { return false; }
    bool unlockAchievement(const char*) override { return true; }
    bool isAchievementUnlocked(const char*) const override { return false; }
    bool resetAchievement(const char*) override { return true; }
    bool resetAllAchievements() override { return true; }
    bool setStatInt(const char*, int32_t) override { return true; }
    int32_t getStatInt(const char*) const override { return 0; }
    bool setStatFloat(const char*, float) override { return true; }
    float getStatFloat(const char*) const override { return 0.0f; }
    bool storeStats() override { return true; }
    bool cloudWrite(const char* fileName, const void* data, int32_t size) override {
        if (!fileName || size < 0) return false;
        if (rejectedWriteName == fileName) {
            rejectedWriteName.clear();
            ++writeFailureCount;
            return false;
        }
        if (writesBeforeFailure == 0) {
            writesBeforeFailure = -1;
            ++writeFailureCount;
            return false;
        }
        if (writesBeforeFailure > 0) --writesBeforeFailure;
        files[fileName] = std::string(static_cast<const char*>(data),
                                      static_cast<size_t>(size));
        if (corruptNextWrite && size > 0) {
            corruptNextWrite = false;
            files[fileName][0] ^= 1;
        }
        return true;
    }
    int32_t cloudRead(const char* fileName, void* buffer, int32_t maxSize) override {
        const auto it = files.find(fileName ? fileName : "");
        if (it == files.end() || !buffer || maxSize <= 0) return 0;
        int32_t n = std::min<int32_t>(maxSize,
                                      static_cast<int32_t>(it->second.size()));
        if (readsBeforeShortRead == 0) {
            readsBeforeShortRead = -1;
            ++shortReadCount;
            if (n > 0) --n;
        } else if (readsBeforeShortRead > 0) {
            --readsBeforeShortRead;
        }
        std::memcpy(buffer, it->second.data(), static_cast<size_t>(n));
        return n;
    }
    int32_t cloudFileSize(const char* fileName) const override {
        const auto it = files.find(fileName ? fileName : "");
        return it == files.end() ? 0 : static_cast<int32_t>(it->second.size());
    }
    bool cloudFileExists(const char* fileName) const override {
        return files.count(fileName ? fileName : "") > 0;
    }
    bool cloudDelete(const char* fileName) override {
        if (rejectedDeleteName == (fileName ? fileName : "")) {
            ++deleteFailureCount;
            return false;
        }
        files.erase(fileName ? fileName : "");
        return true;
    }
    int32_t cloudQuotaTotal() const override { return 8 * 1024 * 1024; }
    int32_t cloudQuotaUsed() const override { return 0; }
    int32_t cloudFileCount() const override {
        return static_cast<int32_t>(files.size());
    }
    const char* cloudFileNameAt(int32_t index) const override {
        if (index < 0) return "";
        int32_t i = 0;
        for (const auto& entry : files) {
            if (i++ == index) return entry.first.c_str();
        }
        return "";
    }
    const char* name() const override { return "cloud-mock-steam"; }
};

class CloudCryptoRegistration {
public:
    CloudCryptoRegistration() : previous(BackendRegistry::instance().getCryptoEngine()) {
        BackendRegistry::instance().setCryptoEngine(&crypto);
    }
    ~CloudCryptoRegistration() { BackendRegistry::instance().setCryptoEngine(previous); }

private:
    carc::CryptoEngine crypto;
    carc::ICryptoEngine* previous;
};

class EncryptedCloudServer {
public:
    EncryptedCloudServer() {
        server.Put("/saves/save_3.json", [this](const httplib::Request& req, httplib::Response& res) {
            std::lock_guard<std::mutex> lock(mutex);
            stored = req.body;
            ++putCount;
            res.set_content("ok", "text/plain");
        });
        server.Get("/saves/save_3.json", [this](const httplib::Request&, httplib::Response& res) {
            std::lock_guard<std::mutex> lock(mutex);
            res.set_content(stored, "application/octet-stream");
        });
        port = server.bind_to_any_port("127.0.0.1");
        REQUIRE(port > 0);
        worker = std::thread([this]() { server.listen_after_bind(); });
        server.wait_until_ready();
    }
    ~EncryptedCloudServer() {
        server.stop();
        if (worker.joinable()) worker.join();
    }
    std::string endpoint() const { return "http://127.0.0.1:" + std::to_string(port) + "/saves"; }
    std::string bytes() {
        std::lock_guard<std::mutex> lock(mutex);
        return stored;
    }
    void replaceBytes(const std::string& bytes) {
        std::lock_guard<std::mutex> lock(mutex);
        stored = bytes;
    }
    int writes() {
        std::lock_guard<std::mutex> lock(mutex);
        return putCount;
    }

private:
    httplib::Server server;
    std::thread worker;
    std::mutex mutex;
    std::string stored;
    int port = 0;
    int putCount = 0;
};

std::string cloudFileBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void replaceCloudFileBytes(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file.good());
}

struct RejectedSyncInput {
    const char* name;
    std::string bytes;
    bool missingKey = false;
    bool wrongKey = false;
};

std::string encryptCloudTestBytes(const std::string& plain) {
    std::array<uint8_t, 32> key{};
    for (size_t i = 0; i < key.size(); ++i) key[i] = static_cast<uint8_t>(i + 1);
    auto* crypto = BackendRegistry::instance().getCryptoEngine();
    REQUIRE(crypto != nullptr);
    uint8_t nonce[12]{};
    uint8_t tag[16]{};
    crypto->generateNonce(nonce, sizeof(nonce));
    const auto cipher = crypto->encrypt(reinterpret_cast<const uint8_t*>(plain.data()), plain.size(),
                                         key.data(), key.size(), nonce, sizeof(nonce), tag, sizeof(tag));
    REQUIRE_FALSE(cipher.empty());
    std::string bytes = "CAES";
    bytes.append(reinterpret_cast<const char*>(nonce), sizeof(nonce));
    bytes.append(reinterpret_cast<const char*>(tag), sizeof(tag));
    bytes.append(reinterpret_cast<const char*>(cipher.data()), cipher.size());
    return bytes;
}

std::vector<RejectedSyncInput> rejectedSyncInputs(const std::string& encrypted) {
    REQUIRE(encrypted.size() > 32);
    auto badTag = encrypted;
    badTag[16] ^= 0x40;
    return {
        {"plaintext", R"({"schema_version":5,"data":{"route":"untrusted"}})"},
        {"bad-tag", badTag},
        {"wrong-key", encrypted, false, true},
        {"missing-key", encrypted, true, false},
        {"authenticated-null-data", encryptCloudTestBytes(R"({"schema_version":5,"data":null})")},
        {"authenticated-invalid-json", encryptCloudTestBytes("{truncated")},
    };
}

void setSyncTestKey(SaveManager& saves, const RejectedSyncInput& input) {
    std::array<uint8_t, 32> key{};
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<uint8_t>(i + (input.wrongKey ? 80 : 1));
    }
    saves.setEncryptionKey(key.data());
    if (input.missingKey) saves.clearEncryptionKey();
}

class SingleReadCloudProvider final : public ISaveProvider, public ICloudSaveTransport {
public:
    std::string validBytes;
    std::string uploaded;
    std::string committed;
    int localReads = 0;
    int cloudReads = 0;
    bool unstagedAccess = false;

    std::string readLocalFile(const std::string&) override {
        return ++localReads == 1 ? validBytes : "changed-after-first-read";
    }
    std::string readCloudFile(const std::string&) override {
        return ++cloudReads == 1 ? validBytes : "changed-after-first-read";
    }
    bool writeLocalFile(const std::string&, const std::string& bytes) override {
        committed = bytes;
        return true;
    }
    bool writeCloudFile(const std::string&, const std::string& bytes) override {
        uploaded = bytes;
        return true;
    }
    std::string readFile(const std::string&) override { unstagedAccess = true; return {}; }
    bool writeFile(const std::string&, const std::string&) override { unstagedAccess = true; return false; }
    bool deleteFile(const std::string&) override { return false; }
    std::vector<std::string> listFiles(const std::string&) override { return {}; }
    bool pushToCloud(const std::string&) override { unstagedAccess = true; return false; }
    bool pullFromCloud(const std::string&) override { unstagedAccess = true; return false; }
    bool supportsCloudSync() const override { return true; }
};

}  // namespace

TEST_CASE("U4: HTTP cloud transports the exact encrypted slot bytes") {
    TestPaths::ScopedTempDir sourceDir("cloud_encrypted_source");
    TestPaths::ScopedTempDir targetDir("cloud_encrypted_target");
    CloudCryptoRegistration crypto;
    EncryptedCloudServer server;
    std::array<uint8_t, 32> key{};
    for (size_t i = 0; i < key.size(); ++i) key[i] = static_cast<uint8_t>(i + 1);
    const json data = {{"secret", "U4_CLOUD_PRIVATE_PAYLOAD"}, {"route", "rain"}};

    SaveManager source;
    source.init(sourceDir.string());
    REQUIRE(source.configureCloudSync(server.endpoint()));
    source.setEncryptionKey(key.data());
    REQUIRE(source.save(3, data, "cloud-encrypted", 9));
    const auto diskBytes = cloudFileBytes(sourceDir.path() / "save_3.json");
    CHECK(diskBytes.substr(0, 4) == "CAES");
    CHECK(diskBytes.find("U4_CLOUD_PRIVATE_PAYLOAD") == std::string::npos);
    REQUIRE(source.pushSlotToCloud(3));
    CHECK(server.bytes() == diskBytes);

    SaveManager target;
    target.init(targetDir.string());
    REQUIRE(target.configureCloudSync(server.endpoint()));
    target.setEncryptionKey(key.data());
    REQUIRE(target.pullSlotFromCloud(3));
    CHECK(cloudFileBytes(targetDir.path() / "save_3.json") == diskBytes);
    CHECK(target.load(3) == data);
    REQUIRE(target.pushSlotToCloud(3));
    CHECK(server.bytes() == diskBytes); // Explicit sync never decrypts or adds another envelope.
    target.setEncryptionPolicy(SaveEncryptionPolicy::RequireEncrypted);
    target.clearEncryptionKey();
    CHECK(target.load(3).is_null());
    CHECK_FALSE(target.save(3, {{"secret", "must-not-replace"}}, "other", 1));
    CHECK(cloudFileBytes(targetDir.path() / "save_3.json") == diskBytes);
    CHECK(server.bytes() == diskBytes);
}

TEST_CASE("U4: Steam provider receives and returns encrypted raw bytes") {
    TestPaths::ScopedTempDir dir("steam_encrypted_bytes");
    CloudCryptoRegistration crypto;
    CloudMockSteam steam;
    std::array<uint8_t, 32> key{};
    for (size_t i = 0; i < key.size(); ++i) key[i] = static_cast<uint8_t>(i + 1);
    const json data = {{"secret", "U4_STEAM_PRIVATE_PAYLOAD"}};
    {
        SaveManager source;
        source.init(dir.string());
        source.setSaveProvider(std::make_unique<CloudSaveProvider>(&steam));
        source.setEncryptionKey(key.data());
        REQUIRE(source.save(3, data, "steam-encrypted", 7));
    }
    REQUIRE(steam.files.count("save_3.json") == 1);
    const auto bytes = steam.files.at("save_3.json");
    CHECK(bytes.substr(0, 4) == "CAES");
    CHECK(bytes.find("U4_STEAM_PRIVATE_PAYLOAD") == std::string::npos);
    SaveManager target;
    target.init(dir.string());
    target.setSaveProvider(std::make_unique<CloudSaveProvider>(&steam));
    CHECK(target.load(3).is_null());
    target.setEncryptionKey(key.data());
    CHECK(target.load(3) == data);
    CHECK(steam.files.at("save_3.json") == bytes);
    CHECK_FALSE(std::filesystem::exists(dir.path() / "save_3.json"));
    target.setEncryptionPolicy(SaveEncryptionPolicy::RequireEncrypted);
    target.clearEncryptionKey();
    CHECK_FALSE(target.save(3, {{"secret", "must-not-replace"}}, "other", 1));
    CHECK(steam.files.at("save_3.json") == bytes);
}

TEST_CASE("U4: strict HTTP sync validates staged bytes before changing either store") {
    TestPaths::ScopedTempDir dir("strict_http_sync_staging");
    CloudCryptoRegistration crypto;
    EncryptedCloudServer server;
    SaveManager saves;
    saves.init(dir.string());
    REQUIRE(saves.configureCloudSync(server.endpoint()));
    setSyncTestKey(saves, {"correct", ""});
    saves.setEncryptionPolicy(SaveEncryptionPolicy::RequireEncrypted);
    REQUIRE(saves.save(3, {{"route", "original"}}, "original", 3));
    const auto path = dir.path() / "save_3.json";
    const auto original = cloudFileBytes(path);
    for (const bool push : {true, false}) {
        CAPTURE(push);
        for (const auto& input : rejectedSyncInputs(original)) {
            CAPTURE(input.name);
            const auto localBefore = push ? input.bytes : original;
            const auto remoteBefore = push ? original : input.bytes;
            replaceCloudFileBytes(path, localBefore);
            server.replaceBytes(remoteBefore);
            setSyncTestKey(saves, input);
            const auto putsBefore = server.writes();
            const bool synced = push ? saves.pushSlotToCloud(3) : saves.pullSlotFromCloud(3);
            CHECK_FALSE(synced);
            CHECK(cloudFileBytes(path) == localBefore);
            CHECK(server.bytes() == remoteBefore);
            CHECK(server.writes() == putsBefore);
        }
    }
    // A real supported sync still works in strict mode when both bytes and key are valid.
    setSyncTestKey(saves, {"correct", ""});
    replaceCloudFileBytes(path, original);
    server.replaceBytes("");
    REQUIRE(saves.pushSlotToCloud(3));
    CHECK(server.bytes() == original);
    replaceCloudFileBytes(path, "old-local-content");
    REQUIRE(saves.pullSlotFromCloud(3));
    CHECK(cloudFileBytes(path) == original);
}

TEST_CASE("U4: strict Steam sync validates staged bytes before changing either store") {
    TestPaths::ScopedTempDir dir("strict_steam_sync_staging");
    CloudCryptoRegistration crypto;
    CloudMockSteam steam;
    SaveManager saves;
    saves.init(dir.string());
    setSyncTestKey(saves, {"correct", ""});
    REQUIRE(saves.save(3, {{"route", "original"}}, "original", 3));
    const auto path = dir.path() / "save_3.json";
    const auto original = cloudFileBytes(path);
    saves.setSaveProvider(std::make_unique<CloudSaveProvider>(&steam));
    saves.setEncryptionPolicy(SaveEncryptionPolicy::RequireEncrypted);
    for (const bool push : {true, false}) {
        CAPTURE(push);
        for (const auto& input : rejectedSyncInputs(original)) {
            CAPTURE(input.name);
            const auto localBefore = push ? input.bytes : original;
            const auto remoteBefore = push ? original : input.bytes;
            replaceCloudFileBytes(path, localBefore);
            steam.files["save_3.json"] = remoteBefore;
            setSyncTestKey(saves, input);
            const bool synced = push ? saves.pushSlotToCloud(3) : saves.pullSlotFromCloud(3);
            CHECK_FALSE(synced);
            CHECK(cloudFileBytes(path) == localBefore);
            CHECK(steam.files.at("save_3.json") == remoteBefore);
        }
    }
    setSyncTestKey(saves, {"correct", ""});
    replaceCloudFileBytes(path, original);
    steam.files.clear();
    REQUIRE(saves.pushSlotToCloud(3));
    CHECK(steam.files.at("save_3.json") == original);
    replaceCloudFileBytes(path, "old-local-content");
    REQUIRE(saves.pullSlotFromCloud(3));
    CHECK(cloudFileBytes(path) == original);
}

TEST_CASE("U4: cloud sync commits the exact validated buffer without rereading") {
    TestPaths::ScopedTempDir dir("cloud_single_read_staging");
    CloudCryptoRegistration crypto;
    auto provider = std::make_unique<SingleReadCloudProvider>();
    auto* probe = provider.get();
    const auto bytes = encryptCloudTestBytes(R"({"schema_version":5,"data":{"route":"verified"}})");
    probe->validBytes = bytes;
    SaveManager saves;
    saves.init(dir.string());
    saves.setSaveProvider(std::move(provider));
    saves.setEncryptionPolicy(SaveEncryptionPolicy::RequireEncrypted);
    setSyncTestKey(saves, {"correct", ""});
    REQUIRE(saves.pushSlotToCloud(3));
    CHECK(probe->localReads == 1);
    CHECK(probe->cloudReads == 0);
    CHECK(probe->uploaded == bytes);
    REQUIRE(saves.pullSlotFromCloud(3));
    CHECK(probe->localReads == 1);
    CHECK(probe->cloudReads == 1);
    CHECK(probe->committed == bytes);
    CHECK_FALSE(probe->unstagedAccess);
}

TEST_CASE("HttpCloudSaveProvider: push/pull round trip via mock server") {
    // Mock REST server: in-memory file store.
    httplib::Server srv;
    std::map<std::string, std::string> store;
    std::mutex storeMutex;
    srv.Put(R"(/saves/(.*))", [&](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(storeMutex);
        store[req.matches[1]] = req.body;
        res.set_content("{}", "application/json");
    });
    srv.Get(R"(/saves/(.*))", [&](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(storeMutex);
        auto it = store.find(req.matches[1]);
        if (it == store.end()) { res.status = 404; return; }
        res.set_content(it->second, "application/octet-stream");
    });
    srv.Delete(R"(/saves/(.*))", [&](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(storeMutex);
        store.erase(req.matches[1]);
        res.set_content("{}", "application/json");
    });
    int port = 0;
    for (int p = 18961; p <= 18970 && port == 0; ++p) {
        if (srv.bind_to_port("127.0.0.1", p)) port = p;
    }
    REQUIRE(port != 0);
    std::thread t([&]() { srv.listen_after_bind(); });

    const std::string endpoint = "http://127.0.0.1:" + std::to_string(port) + "/saves";
    SaveManager mgr;
    mgr.init("cloud_test_saves");
    REQUIRE(mgr.configureCloudSync(endpoint));

    // Save slot 2 locally, push, wipe local, pull, verify.
    nlohmann::json data = {{"scene", "chapter_3"}, {"hp", 42}};
    REQUIRE(mgr.save(2, data, "chapter_3", 120));
    REQUIRE(mgr.pushSlotToCloud(2));
    {
        std::lock_guard<std::mutex> lock(storeMutex);
        REQUIRE(store.find("save_2.json") != store.end());
        CHECK(store["save_2.json"].find("chapter_3") != std::string::npos);
    }
    // Simulate another machine: fresh manager pulls the slot.
    SaveManager other;
    other.init("cloud_test_saves");
    REQUIRE(other.configureCloudSync(endpoint));
    REQUIRE(other.pullSlotFromCloud(2));
    CHECK(other.slotExists(2));
    auto meta = other.listSaves();
    bool found = false;
    for (const auto& m : meta) {
        if (m.slot == 2) found = true;
    }
    CHECK(found);

    // Delete on the server; offline degrade: unreachable endpoint -> false.
    mgr.deleteSlot(2);
    mgr.pushSlotToCloud(2);  // re-push (slot file gone -> false is fine)
    REQUIRE(mgr.configureCloudSync(""));  // back to local-only
    CHECK_FALSE(mgr.pushSlotToCloud(2));  // local provider: no cloud sync

    srv.stop();
    t.join();
    // Cleanup test dirs.
    std::remove("cloud_test_saves/slot_2.json");
    std::remove("cloud_test_saves/save_2.meta");
    std::remove("cloud_test_saves");
    std::remove("cloud_test_saves/index.json");
}

TEST_CASE("HttpCloudSaveProvider: offline degrade never throws") {
    // A port nothing listens on: push/pull return false, no exception.
    // Use a port in the freed range; nothing listens there.
    const int port = 18999;

    HttpCloudSaveProvider provider(
        "http://127.0.0.1:" + std::to_string(port) + "/saves", 500);
    // No local file -> push false without touching the network.
    CHECK_FALSE(provider.pushToCloud("saves/nope.json"));
    CHECK_FALSE(provider.pullFromCloud("saves/nope.json"));
}
TEST_CASE("HttpCloudSaveProvider: oversized cloud payload rejected (ST-2)") {
    // Mock server returning a body larger than the 10 MiB cap: pull must
    // reject it instead of writing a multi-GB local file.
    httplib::Server srv;
    srv.Get(R"(/saves/(.*))", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(std::string(11u * 1024u * 1024u, 'x'), "application/octet-stream");
    });
    int port = 0;
    for (int p = 18941; p <= 18950 && port == 0; ++p) {
        if (srv.bind_to_port("127.0.0.1", p)) port = p;
    }
    REQUIRE(port != 0);
    std::thread t([&]() { srv.listen_after_bind(); });

    // Direct provider: pull must return false and leave no local file.
    {
        HttpCloudSaveProvider provider(
            "http://127.0.0.1:" + std::to_string(port) + "/saves", 8000);
        CHECK_FALSE(provider.pullFromCloud("save_0.json"));
        // No local artifact written by the pull itself.
        std::ifstream f("save_0.json");
        CHECK_FALSE(f.good());
    }
    srv.stop();
    t.join();
    std::remove("save_0.json");
}

TEST_CASE("HttpCloudSaveProvider: https endpoint fails closed without SSL (ST-2)") {
    // Without CPPHTTPLIB_OPENSSL_SUPPORT the client must reject an https
    // endpoint (return false) rather than silently downgrade to plaintext.
    HttpCloudSaveProvider provider("https://example.com/saves", 500);
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    // With SSL compiled we cannot reach example.com from CI; just ensure no crash.
    (void)provider;
    CHECK(true);
#else
    CHECK_FALSE(provider.pushToCloud("saves/nope.json"));
    // pull returns false: no local file, no exception, TLS not downgraded.
    CHECK_FALSE(provider.pullFromCloud("saves/nope.json"));
#endif
}

TEST_CASE("HttpCloudSaveProvider: bearer token sent as Authorization header (ST-2)") {
    httplib::Server srv;
    std::string gotAuth;
    std::mutex authMutex;
    srv.Get(R"(/saves/(.*))", [&](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(authMutex);
        gotAuth = req.get_header_value("Authorization");
        res.set_content("token-ok", "application/octet-stream");
    });
    int port = 0;
    for (int p = 18931; p <= 18940 && port == 0; ++p) {
        if (srv.bind_to_port("127.0.0.1", p)) port = p;
    }
    REQUIRE(port != 0);
    std::thread t([&]() { srv.listen_after_bind(); });

    {
        // Write a local file so pull has a target path to write into.
        HttpCloudSaveProvider provider(
            "http://127.0.0.1:" + std::to_string(port) + "/saves", 8000, "sekret");
        // Write a tiny local file first so push has content to send.
        // (pull reads the body regardless; the write target is the local file.)
        CHECK(provider.writeFile("st_ok.json", "seed"));
        const bool ok = provider.pullFromCloud("st_ok.json");
        CHECK(ok);
        std::lock_guard<std::mutex> lock(authMutex);
        CHECK(gotAuth == "Bearer sekret");
    }
    srv.stop();
    t.join();
    std::remove("st_ok.json");
}

TEST_CASE("SaveManager::configureCloudSync steam endpoint requires a backend") {
    // No Steam backend is registered in the test process, so the steam endpoint
    // must FAIL CLOSED and keep the existing provider. Installing a
    // CloudSaveProvider over a null backend would make load()/listSaves()
    // report every existing save as gone (readFile returns "" for all of them).
    TestPaths::ScopedTempDir dir("cloud_steam_nobackend");
    SaveManager mgr;
    mgr.init(dir.string());
    REQUIRE(mgr.configureCloudSync(""));  // local provider installed
    ISaveProvider* before = mgr.getSaveProvider();
    REQUIRE(before != nullptr);

    // A save made locally must still be visible after the refused switch.
    REQUIRE(mgr.save(3, nlohmann::json{{"hp", 7}}, "chapter_1", 11));
    REQUIRE(mgr.slotExists(3));

    for (const char* endpoint : {"steam", "steam://", "steamcloud"}) {
        CAPTURE(endpoint);
        CHECK_FALSE(mgr.configureCloudSync(endpoint));
        CHECK(mgr.getSaveProvider() == before);  // provider untouched
        CHECK(mgr.slotExists(3));                // save still reachable
    }

    // Local-only provider has no cloud end: push/pull refuse and say so
    // (t5 finding -- these used to be indistinguishable from a failed transfer).
    CHECK_FALSE(mgr.pushSlotToCloud(3));
    CHECK_FALSE(mgr.pullSlotFromCloud(3));
    CHECK(mgr.slotExists(3));  // a refused sync never touches the save
}

TEST_CASE("U26: unavailable registered Steam keeps the local save provider") {
    const char* endpoint = "steam";
    SUBCASE("steam") {}
    SUBCASE("steam scheme") { endpoint = "steam://"; }
    SUBCASE("steamcloud alias") { endpoint = "steamcloud"; }
    CAPTURE(endpoint);

    // The composition root registers a backend even when SDK initialization
    // fails. Use the actual Null implementation, not an absent registry entry.
    NullSteamBackend steam;
    REQUIRE_FALSE(steam.init());
    REQUIRE_FALSE(steam.isAvailable());
    auto& registry = BackendRegistry::instance();
    struct RestoreSteam {
        ISteamBackend* previous;
        ~RestoreSteam() { BackendRegistry::instance().setSteamBackend(previous); }
    } restore{registry.getSteamBackend()};
    registry.setSteamBackend(&steam);

    TestPaths::ScopedTempDir dir("cloud_steam_unavailable");
    SaveManager manager;
    manager.init(dir.string());
    REQUIRE(manager.configureCloudSync(""));
    ISaveProvider* const originalProvider = manager.getSaveProvider();
    REQUIRE(originalProvider != nullptr);
    const nlohmann::json data = {{"hp", 7}, {"route", "local-before-steam"}};
    REQUIRE(manager.save(3, data, "chapter_1", 11));
    REQUIRE(manager.load(3) == data);
    const auto slot = dir.path() / "save_3.json";
    const std::string originalBytes = cloudFileBytes(slot);
    REQUIRE_FALSE(originalBytes.empty());

    CHECK_FALSE(manager.configureCloudSync(endpoint));
    CHECK(manager.getSaveProvider() == originalProvider);
    CHECK(manager.slotExists(3));
    SaveMeta metadata;
    CHECK(manager.load(3, &metadata) == data);
    CHECK(metadata.sceneName == "chapter_1");
    CHECK(metadata.tokenIndex == 11);
    CHECK(cloudFileBytes(slot) == originalBytes);
}

// End-to-end save -> load round trip under the steam endpoint. This is the
// case t14 asks about: once CloudSaveProvider is installed, Steam Remote
// Storage IS the store, so the round trip must work without any local file.
TEST_CASE("SaveManager: steam endpoint save/load round trip goes through the cloud") {
    CloudMockSteam steam;
    BackendRegistry::instance().setSteamBackend(&steam);
    struct Restore {
        ~Restore() { BackendRegistry::instance().setSteamBackend(nullptr); }
    } restore;

    TestPaths::ScopedTempDir dir("cloud_steam_roundtrip");
    SaveManager mgr;
    mgr.init(dir.string());
    REQUIRE(mgr.configureCloudSync("steam"));
    REQUIRE(mgr.getSaveProvider() != nullptr);
    REQUIRE(mgr.getSaveProvider()->supportsCloudSync());

    const nlohmann::json data = {{"scene", "chapter_7"}, {"affinity", 88}};
    REQUIRE(mgr.save(5, data, "chapter_7", 314));

    // The bytes landed in Steam Remote Storage under the FLAT key, not on disk.
    CHECK(steam.files.count("save_5.json") == 1);
    CHECK_FALSE(std::filesystem::exists(dir.path() / "save_5.json"));

    // Round trip: load() reads back through the same provider.
    SaveMeta meta;
    const nlohmann::json loaded = mgr.load(5, &meta);
    REQUIRE(loaded.is_object());
    CHECK(loaded.value("affinity", 0) == 88);
    CHECK(meta.sceneName == "chapter_7");
    CHECK(meta.tokenIndex == 314);
    CHECK(mgr.slotExists(5));

    // listSaves() enumerates the cloud store too.
    bool listed = false;
    for (const auto& m : mgr.listSaves()) {
        if (m.slot == 5) listed = true;
    }
    CHECK(listed);

    // delete removes the cloud object.
    CHECK(mgr.deleteSlot(5));
    CHECK(steam.files.count("save_5.json") == 0);
    CHECK_FALSE(mgr.slotExists(5));
}

// Who wins when local and cloud disagree? Nobody implicitly: each direction is
// an explicit call, and the side named by the call wins for that call only.
TEST_CASE("CloudSaveProvider: push and pull are explicit one-way transfers") {
    CloudMockSteam steam;
    CloudSaveProvider provider(&steam);

    TestPaths::ScopedTempDir dir("cloud_conflict");
    const std::string localPath = (dir.path() / "save_9.json").string();

    // Local says "local-newer", cloud says "cloud-older".
    {
        std::ofstream f(localPath, std::ios::binary | std::ios::trunc);
        f << "local-newer";
    }
    steam.files["save_9.json"] = "cloud-older";

    // push: local wins, cloud replaced. The directory component is stripped, so
    // the cloud key stays flat.
    REQUIRE(provider.pushToCloud(localPath));
    CHECK(steam.files["save_9.json"] == "local-newer");

    // pull: cloud wins, local file replaced.
    steam.files["save_9.json"] = "cloud-authoritative";
    REQUIRE(provider.pullFromCloud(localPath));
    {
        std::ifstream f(localPath, std::ios::binary);
        std::string got((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
        CHECK(got == "cloud-authoritative");
    }

    // A missing cloud object must NOT wipe the local save: pull aborts before
    // opening the local file for writing.
    steam.files.erase("save_9.json");
    CHECK_FALSE(provider.pullFromCloud(localPath));
    {
        std::ifstream f(localPath, std::ios::binary);
        std::string got((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
        CHECK(got == "cloud-authoritative");  // untouched by the failed pull
    }

    // An empty cloud object is treated the same way (no silent truncation).
    steam.files["save_9.json"] = "";
    CHECK_FALSE(provider.pullFromCloud(localPath));
    {
        std::ifstream f(localPath, std::ios::binary);
        std::string got((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
        CHECK(got == "cloud-authoritative");
    }

    // push with no local file must not fabricate a cloud write from the cloud's
    // own copy (the earlier revision looped cloud -> cloud and reported true).
    steam.files["save_absent.json"] = "cloud-only";
    CHECK_FALSE(provider.pushToCloud((dir.path() / "save_absent.json").string()));
    CHECK(steam.files["save_absent.json"] == "cloud-only");  // unchanged
}

TEST_CASE("CloudSaveProvider: null backend refuses both transfer directions") {
    CloudSaveProvider provider(nullptr);
    TestPaths::ScopedTempDir dir("cloud_null_backend");
    const std::string localPath = (dir.path() / "save_0.json").string();
    {
        std::ofstream f(localPath, std::ios::binary | std::ios::trunc);
        f << "local-data";
    }
    CHECK_FALSE(provider.pushToCloud(localPath));
    CHECK_FALSE(provider.pullFromCloud(localPath));
    // The local file survives a refused pull.
    std::ifstream f(localPath, std::ios::binary);
    std::string got((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
    CHECK(got == "local-data");
}

TEST_CASE("U26 cloud chunks: failed overwrite preserves the complete previous save") {
    int successfulWritesBeforeFailure = 0;
    SUBCASE("first write rejected") {}
    SUBCASE("one write accepted before failure") { successfulWritesBeforeFailure = 1; }
    SUBCASE("two writes accepted before failure") { successfulWritesBeforeFailure = 2; }
    CAPTURE(successfulWritesBeforeFailure);

    CloudMockSteam steam;
    CloudSaveProvider provider(&steam);
    const std::string path = "saves/save_6.json";
    std::string previous(600000, 'A');
    previous[262144] = '\0';
    previous.back() = 'Z';
    const std::string replacement(650000, 'B');
    REQUIRE(provider.writeFile(path, previous));
    REQUIRE(bool(provider.readFile(path) == previous));

    steam.writesBeforeFailure = successfulWritesBeforeFailure;
    CHECK_FALSE(provider.writeFile(path, replacement));
    CHECK(steam.writeFailureCount == 1);  // The real provider reached the fault.
    CloudSaveProvider reopened(&steam);
    const auto restored = reopened.readFile(path);
    CHECK(restored.size() == previous.size());
    CHECK(bool(restored == previous));  // Full opaque bytes, including NUL.

    // A failed update must not poison a later explicit retry.
    REQUIRE(reopened.writeFile(path, replacement));
    CHECK(bool(CloudSaveProvider(&steam).readFile(path) == replacement));
}

TEST_CASE("U26 cloud chunks: replacing a large save with a small save reads the new version") {
    CloudMockSteam steam;
    CloudSaveProvider provider(&steam);
    const std::string path = "saves/save_7.json";
    const std::string previous(600000, 'L');
    const std::string replacement("new\0small", 9);
    REQUIRE(provider.writeFile(path, previous));
    REQUIRE(bool(provider.readFile(path) == previous));
    REQUIRE(provider.writeFile(path, replacement));

    // Reopen through the public interface: stale metadata/chunks must not
    // shadow the successful small write, regardless of physical key layout.
    CloudSaveProvider reopened(&steam);
    const auto current = reopened.readFile(path);
    CHECK(current.size() == replacement.size());
    CHECK(bool(current == replacement));
}

TEST_CASE("U26 cloud chunks: short SDK reads cannot publish partial save bytes") {
    size_t payloadSize = 101;
    int completeReadsBeforeFault = 0;
    SUBCASE("single file short read") {}
    SUBCASE("chunk metadata short read") { payloadSize = 600000; }
    SUBCASE("first chunk short read") {
        payloadSize = 600000;
        completeReadsBeforeFault = 1;
    }
    SUBCASE("last chunk short read") {
        payloadSize = 600000;
        completeReadsBeforeFault = 3;
    }
    CAPTURE(payloadSize);
    CAPTURE(completeReadsBeforeFault);

    CloudMockSteam steam;
    CloudSaveProvider provider(&steam);
    const std::string path = "saves/save_8.json";
    const std::string payload(payloadSize, 'R');
    REQUIRE(provider.writeFile(path, payload));
    REQUIRE(bool(provider.readFile(path) == payload));

    steam.readsBeforeShortRead = completeReadsBeforeFault;
    const auto incomplete = provider.readFile(path);
    CHECK(steam.shortReadCount == 1);
    CHECK(incomplete.empty());

    // The transport fault is transient, and reads must not mutate remote data.
    CHECK(bool(CloudSaveProvider(&steam).readFile(path) == payload));
}

TEST_CASE("U26 cloud publication: legacy chunks remain readable and can become a small save") {
    CloudMockSteam steam;
    const std::string previous(600000, 'L');
    steam.files["legacy.json.meta"] = "600000,3";
    steam.files["legacy.json.chunk000"] = previous.substr(0, 262144);
    steam.files["legacy.json.chunk001"] = previous.substr(262144, 262144);
    steam.files["legacy.json.chunk002"] = previous.substr(524288);
    steam.files["unrelated.json"] = "untouched";
    CloudSaveProvider provider(&steam);
    REQUIRE(bool(provider.readFile("legacy.json") == previous));
    REQUIRE(provider.writeFile("legacy.json", "small replacement"));
    CHECK(provider.readFile("legacy.json") == "small replacement");
    CHECK_FALSE(steam.cloudFileExists("legacy.json.chunk000"));
    CHECK_FALSE(steam.cloudFileExists("legacy.json.chunk001"));
    CHECK_FALSE(steam.cloudFileExists("legacy.json.chunk002"));
    CHECK(steam.files.at("unrelated.json") == "untouched");
}

TEST_CASE("U26 cloud publication: rejected head preserves the old generation and unrelated files") {
    CloudMockSteam steam;
    CloudSaveProvider provider(&steam);
    const std::string previous(600000, 'P');
    REQUIRE(provider.writeFile("save_10.json", previous));
    REQUIRE(bool(provider.readFile("save_10.json") == previous));
    steam.files["neighbor.json"] = "not ours";
    const auto before = steam.files;
    steam.rejectedWriteName = "save_10.json.meta";
    CHECK_FALSE(provider.writeFile("save_10.json", std::string(650000, 'N')));
    CHECK(steam.writeFailureCount == 1);
    CHECK(bool(steam.files == before));
    CHECK(bool(CloudSaveProvider(&steam).readFile("save_10.json") == previous));
}

TEST_CASE("U26 cloud publication: staged bytes must verify before the head changes") {
    CloudMockSteam steam;
    CloudSaveProvider provider(&steam);
    const std::string previous(600000, 'P');
    REQUIRE(provider.writeFile("verify.json", previous));
    const auto before = steam.files;
    steam.corruptNextWrite = true;
    CHECK_FALSE(provider.writeFile("verify.json", std::string(650000, 'N')));
    CHECK_FALSE(steam.corruptNextWrite);  // Fault reached the actual SDK write.
    CHECK(bool(steam.files == before));
    CHECK(bool(CloudSaveProvider(&steam).readFile("verify.json") == previous));
}

TEST_CASE("U26 cloud publication: cleanup failure cannot hide an already published replacement") {
    CloudMockSteam steam;
    CloudSaveProvider provider(&steam);
    const std::string previous(600000, 'P');
    const std::string replacement(650000, 'N');
    REQUIRE(provider.writeFile("cleanup.json", previous));
    for (const auto& file : steam.files) {
        if (file.first.find(".chunk") != std::string::npos) {
            steam.rejectedDeleteName = file.first;
            break;
        }
    }
    REQUIRE_FALSE(steam.rejectedDeleteName.empty());
    REQUIRE(provider.writeFile("cleanup.json", replacement));
    CHECK(steam.deleteFailureCount == 1);
    CHECK(steam.cloudFileExists(steam.rejectedDeleteName.c_str()));
    CHECK(bool(CloudSaveProvider(&steam).readFile("cleanup.json") == replacement));
    CHECK(provider.listFiles("*").empty());  // No staging keys exposed as slots.
}

TEST_CASE("U26 cloud publication: untrusted metadata never selects arbitrary objects for deletion") {
    const std::array<std::string, 8> invalid = {
        "v2,1,1,../victim", "v2,1,1,ffffffffffffffffffffffffffffffff,extra",
        "v2,1,1,FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF", "1,1,trailing",
        "-1,1", "300000,1", std::string("1,1\0junk", 8), std::string(1024, '9')
    };
    for (const auto& metadata : invalid) {
        CAPTURE(metadata.size());
        CloudMockSteam steam;
        steam.files["bad.json.meta"] = metadata;
        steam.files["bad.json.chunk000"] = "x";
        steam.files["bad.json"] = "stale direct bytes";
        steam.files["victim"] = "not ours";
        const auto before = steam.files;
        CloudSaveProvider provider(&steam);
        CHECK(provider.readFile("bad.json").empty());
        // Preserve the existing corrupt-slot recovery contract. Only the
        // explicitly requested direct key and its head may be removed; no
        // references from malformed metadata may drive chunk deletion.
        steam.rejectedDeleteName = "bad.json.meta";
        CHECK_FALSE(provider.deleteFile("bad.json"));
        CHECK(steam.deleteFailureCount == 1);
        CHECK(steam.files.at("bad.json.meta") == metadata);
        steam.rejectedDeleteName.clear();
        CHECK(provider.deleteFile("bad.json"));
        auto expected = before;
        expected.erase("bad.json.meta");
        expected.erase("bad.json");
        CHECK(bool(steam.files == expected));
        REQUIRE(provider.writeFile("bad.json", "recovered"));
        CHECK(provider.readFile("bad.json") == "recovered");
    }
}

TEST_CASE("U26 cloud publication: deleting a generation respects SDK failure and unrelated saves") {
    CloudMockSteam steam;
    CloudSaveProvider provider(&steam);
    REQUIRE(provider.writeFile("delete.json", std::string(600000, 'D')));
    steam.files["other.json"] = "keep";
    for (const auto& file : steam.files) {
        if (file.first.find(".chunk") != std::string::npos) {
            steam.rejectedDeleteName = file.first;
            break;
        }
    }
    REQUIRE_FALSE(steam.rejectedDeleteName.empty());
    CHECK_FALSE(provider.deleteFile("delete.json"));
    CHECK(steam.deleteFailureCount == 1);
    CHECK(steam.files.at("other.json") == "keep");
    steam.rejectedDeleteName.clear();
    CHECK(provider.deleteFile("delete.json"));
    CHECK(provider.readFile("delete.json").empty());
    CHECK(steam.files.size() == 1);
    CHECK(steam.files.at("other.json") == "keep");
}

TEST_CASE("Cloud sync: encrypted pull publication failures preserve the complete local envelope") {
    CloudCryptoRegistration crypto;
    EncryptedCloudServer server;
    TestPaths::ScopedTempDir directory("atomic_cloud_pull");
    const auto slot = directory.path() / "save_3.json";
    SaveManager manager;
    manager.init(directory.string());
    REQUIRE(manager.configureCloudSync(server.endpoint()));
    const std::array<uint8_t, 32> key{1, 2, 3, 4};
    manager.setEncryptionKey(key.data());
    const json previous = {{"chapter", 1}, {"text", "complete local state"}};
    const json replacement = {{"chapter", 2}, {"text", "complete cloud state"}};
    REQUIRE(manager.save(3, previous, "old", 3));
    const auto oldEnvelope = cloudFileBytes(slot);
    REQUIRE(manager.save(3, replacement, "new", 9));
    REQUIRE(manager.pushSlotToCloud(3));
    const auto cloudEnvelope = server.bytes();
    REQUIRE(cloudEnvelope.substr(0, 4) == "CAES");
    REQUIRE(cloudEnvelope != oldEnvelope);
    replaceCloudFileBytes(slot, oldEnvelope);
    const std::array<detail::SaveWriteStage, 6> stages = {
        detail::SaveWriteStage::CreateTemporary, detail::SaveWriteStage::Write,
        detail::SaveWriteStage::Flush, detail::SaveWriteStage::Close,
        detail::SaveWriteStage::Replace, detail::SaveWriteStage::WriteProgress
    };
    for (auto stage : stages) {
        INFO("cloud publication stage=", static_cast<int>(stage));
        detail::ScopedSaveWriteTestHook hook({
            [](detail::SaveWriteStage current, const std::filesystem::path&, void* context) {
                return current != *static_cast<detail::SaveWriteStage*>(context);
            }, &stage
        });
        CHECK_FALSE(manager.pullSlotFromCloud(3));
        CHECK(cloudFileBytes(slot) == oldEnvelope);
        CHECK(manager.load(3) == previous);
        CHECK(std::distance(std::filesystem::directory_iterator(directory.path()),
                            std::filesystem::directory_iterator()) == 1);
    }
    REQUIRE(manager.pullSlotFromCloud(3));
    CHECK(cloudFileBytes(slot) == cloudEnvelope);
    CHECK(manager.load(3) == replacement);
}

namespace {
constexpr size_t u26SnapshotLocalLimit = 10u * 1024u * 1024u;

std::string u26SnapshotPath(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return std::string(utf8.begin(), utf8.end());
}

void u26CheckUnversioned(const CloudSnapshot& snapshot) {
    CHECK(snapshot.revisionKind == CloudRevisionKind::None);
    CHECK(snapshot.revision.empty());
}

void u26CheckReadFailure(const CloudSnapshot& snapshot, CloudReadState state,
                         CloudReadError error) {
    CHECK(snapshot.state == state);
    CHECK(snapshot.error == error);
    CHECK(snapshot.bytes.empty());
    u26CheckUnversioned(snapshot);
}

// Actual loopback HTTP framing, including deliberately incomplete bodies.
// The only blocking handler is released before stop/join on every exit path.
class U26SnapshotHttpServer {
public:
    enum class Mode {
        Present, Empty, Missing, Denied, ServerError, Redirect,
        ShortFixed, ShortChunked, DeclaredTooLarge, StreamTooLarge, Timeout
    };
    explicit U26SnapshotHttpServer(Mode mode) : gate(release.get_future().share()) {
        server.Get("/saves/save_0.json", [this, mode](const httplib::Request&, httplib::Response& res) {
            ++gets;
            entered = true;
            res.set_header("ETag", "\"opaque-server-tag\"");
            res.set_header("Connection", "close");
            switch (mode) {
            case Mode::Present: res.set_content(payload, "application/octet-stream"); return;
            case Mode::Empty: res.set_content("", "application/octet-stream"); return;
            case Mode::Missing: res.status = 404; return;
            case Mode::Denied: res.status = 401; return;
            case Mode::ServerError: res.status = 500; return;
            case Mode::Redirect: res.set_redirect("/redirected"); return;
            case Mode::ShortFixed:
                res.set_content_provider(11, "application/octet-stream",
                    [](size_t, size_t, httplib::DataSink& sink) {
                        sink.write("abc", 3);
                        return false; // Close before the declared eleven bytes.
                    });
                return;
            case Mode::ShortChunked:
                res.set_chunked_content_provider("application/octet-stream",
                    [](size_t, httplib::DataSink& sink) {
                        sink.write("abc", 3);
                        return false; // No terminating zero-length chunk.
                    });
                return;
            case Mode::DeclaredTooLarge:
                res.set_content_provider(u26SnapshotLocalLimit + 1, "application/octet-stream",
                    [](size_t, size_t, httplib::DataSink&) { return false; });
                return;
            case Mode::StreamTooLarge:
                res.set_chunked_content_provider("application/octet-stream",
                    [block = std::string(64 * 1024, 'x')](size_t offset, httplib::DataSink& sink) {
                        const auto count = (std::min)(block.size(), u26SnapshotLocalLimit + 1 - offset);
                        if (!sink.write(block.data(), count)) return false;
                        if (offset + count == u26SnapshotLocalLimit + 1) sink.done();
                        return true;
                    });
                return;
            case Mode::Timeout:
                gate.wait(); // The actual client read deadline must fire first.
                res.set_content("released", "application/octet-stream");
                return;
            }
        });
        server.Get("/redirected", [this](const httplib::Request&, httplib::Response& res) {
            ++redirectGets;
            res.set_content("must not follow", "text/plain");
        });
        server.Put(R"(/saves/(.*))", [this](const httplib::Request&, httplib::Response& res) {
            ++writes;
            res.status = 200;
        });
        server.Delete(R"(/saves/(.*))", [this](const httplib::Request&, httplib::Response& res) {
            ++deletes;
            res.status = 200;
        });
        port = server.bind_to_any_port("127.0.0.1");
        REQUIRE(port > 0);
        worker = std::thread([this]() { server.listen_after_bind(); });
        server.wait_until_ready();
    }
    ~U26SnapshotHttpServer() {
        release.set_value();
        server.stop();
        if (worker.joinable()) worker.join();
    }
    std::string endpoint() const {
        return "http://127.0.0.1:" + std::to_string(port) + "/saves";
    }
    const std::string payload = std::string("raw\0snapshot", 12);
    std::atomic<int> gets{0}, redirectGets{0}, writes{0}, deletes{0};
    std::atomic<bool> entered{false};
private:
    std::promise<void> release;
    std::shared_future<void> gate;
    httplib::Server server;
    std::thread worker;
    int port = 0;
};

// Only the SDK boundary is replaced. Every snapshot is requested from the
// actual CloudSaveProvider; no result classifier is reproduced in this mock.
class U26SnapshotSteam final : public NullSteamBackend {
public:
    std::map<std::string, std::string> files;
    bool available = true;
    std::string shortReadName;
    std::function<void(const std::string&)> afterRead;
    int reads = 0, writes = 0, deletes = 0;
    mutable int probes = 0;
    bool isAvailable() const override { return available; }
    bool cloudFileExists(const char* name) const override {
        ++probes;
        return files.count(name) != 0;
    }
    int32_t cloudFileSize(const char* name) const override {
        ++probes;
        const auto found = files.find(name);
        return found == files.end() ? 0 : static_cast<int32_t>(found->second.size());
    }
    int32_t cloudRead(const char* name, void* buffer, int32_t maxSize) override {
        ++reads;
        const auto found = files.find(name);
        if (found == files.end() || !buffer || maxSize <= 0) return 0;
        auto size = (std::min)(maxSize, static_cast<int32_t>(found->second.size()));
        if (shortReadName == name && size > 0) --size;
        std::memcpy(buffer, found->second.data(), static_cast<size_t>(size));
        if (afterRead) afterRead(name);
        return size;
    }
    bool cloudWrite(const char* name, const void* bytes, int32_t size) override {
        ++writes;
        if (size < 0) return false;
        files[name] = std::string(static_cast<const char*>(bytes), static_cast<size_t>(size));
        return true;
    }
    bool cloudDelete(const char* name) override {
        ++deletes;
        files.erase(name);
        return true;
    }
    void resetCounts() { reads = writes = deletes = probes = 0; }
};
}

TEST_CASE("U26 cloud snapshot: local files distinguish complete empty missing and invalid") {
    TestPaths::ScopedTempDir temporary("typed_cloud_local");
    // macOS /var and Windows TEMP aliases are canonicalized in the fixture,
    // not accepted as evidence that a product link-traversal check succeeded.
    const auto root = std::filesystem::canonical(temporary.path());
    const auto path = root / "save_0.json";
    std::string payload("disk\0bytes", 10);
    CloudReadState expected = CloudReadState::Present;
    CloudReadError error = CloudReadError::None;
    bool createFile = true;
    SUBCASE("ordinary binary bytes") {}
    SUBCASE("ordinary empty file remains Present") { payload.clear(); }
    SUBCASE("missing leaf in an existing directory") {
        createFile = false;
        expected = CloudReadState::Missing;
    }
    SUBCASE("directory is not a save file") {
        createFile = false;
        std::filesystem::create_directory(path);
        expected = CloudReadState::Invalid;
        error = CloudReadError::NotRegularFile;
    }
    SUBCASE("oversized local file is invalid without returning partial bytes") {
        payload.assign(u26SnapshotLocalLimit + 1, 'x');
        expected = CloudReadState::Invalid;
        error = CloudReadError::TooLarge;
    }
    if (createFile) {
        replaceCloudFileBytes(path, payload);
        REQUIRE(cloudFileBytes(path) == payload);
    }
    CloudSaveProvider steam(nullptr); // Local does not require a Steam session.
    HttpCloudSaveProvider http("http://127.0.0.1:1/saves");
    for (ICloudSaveSnapshotTransport* transport :
         std::array<ICloudSaveSnapshotTransport*, 2>{&steam, &http}) {
        const auto snapshot = transport->readSnapshot(CloudSide::Local, u26SnapshotPath(path));
        CHECK(snapshot.state == expected);
        CHECK(snapshot.error == error);
        CHECK(snapshot.httpStatus == 0);
        u26CheckUnversioned(snapshot);
        if (expected == CloudReadState::Present) {
            CHECK(snapshot.bytes == payload);
            CHECK(snapshot.observedBytes == payload.size());
        } else {
            CHECK(snapshot.bytes.empty());
            CHECK(snapshot.observedBytes <= u26SnapshotLocalLimit);
        }
    }
    if (createFile) CHECK(cloudFileBytes(path) == payload);
    if (expected == CloudReadState::Missing) CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE("U26 cloud snapshot: invalid local paths and access failures never mean Missing") {
    TestPaths::ScopedTempDir temporary("typed_cloud_local_failure");
    const auto root = std::filesystem::canonical(temporary.path());
    const auto file = root / "save_0.json";
    replaceCloudFileBytes(file, "retained");
    CloudSaveProvider provider(nullptr);
    SUBCASE("embedded NUL is rejected before filesystem I/O") {
        auto invalid = u26SnapshotPath(file);
        invalid.append("\0suffix", 7);
        u26CheckReadFailure(provider.readSnapshot(CloudSide::Local, invalid),
                            CloudReadState::Invalid, CloudReadError::InvalidPath);
    }
    SUBCASE("a file used as a parent is an invalid path") {
        u26CheckReadFailure(provider.readSnapshot(CloudSide::Local, u26SnapshotPath(file / "child")),
                            CloudReadState::Invalid, CloudReadError::InvalidPath);
    }
#ifdef _WIN32
    SUBCASE("owned exclusive Windows handle proves a real denied open") {
        struct ExclusiveFile {
            HANDLE handle;
            explicit ExclusiveFile(const std::filesystem::path& path)
                : handle(CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {}
            ~ExclusiveFile() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
        } held(file);
        REQUIRE(held.handle != INVALID_HANDLE_VALUE);
        // A second real open confirms the OS sharing precondition independently.
        HANDLE control = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (control != INVALID_HANDLE_VALUE) CloseHandle(control);
        REQUIRE(control == INVALID_HANDLE_VALUE);
        u26CheckReadFailure(provider.readSnapshot(CloudSide::Local, u26SnapshotPath(file)),
                            CloudReadState::Failed, CloudReadError::ReadDenied);
    }
#endif
    CHECK(cloudFileBytes(file) == "retained");
}

TEST_CASE("U26 cloud snapshot: actual HTTP responses preserve typed status and bytes") {
    using Mode = U26SnapshotHttpServer::Mode;
    Mode mode = Mode::Present;
    CloudReadState expected = CloudReadState::Present;
    CloudReadError error = CloudReadError::None;
    int status = 200;
    SUBCASE("complete binary 200 is Present without a trusted revision") {}
    SUBCASE("complete empty 200 is Present rather than Missing") { mode = Mode::Empty; }
    SUBCASE("complete 404 is Missing") { mode = Mode::Missing; expected = CloudReadState::Missing; status = 404; }
    SUBCASE("401 is a rejected read rather than Missing") {
        mode = Mode::Denied; expected = CloudReadState::Failed; error = CloudReadError::HttpStatus; status = 401;
    }
    SUBCASE("500 is failure rather than Missing") {
        mode = Mode::ServerError; expected = CloudReadState::Failed; error = CloudReadError::HttpStatus; status = 500;
    }
    SUBCASE("redirect is not silently followed") {
        mode = Mode::Redirect; expected = CloudReadState::Failed; error = CloudReadError::HttpStatus; status = 302;
    }
    U26SnapshotHttpServer server(mode);
    HttpCloudSaveProvider provider(server.endpoint(), 500);
    const auto snapshot = provider.readSnapshot(CloudSide::Cloud, "save_0.json");
    CHECK(snapshot.state == expected);
    CHECK(snapshot.error == error);
    CHECK(snapshot.httpStatus == status);
    CHECK(snapshot.bytes == (mode == Mode::Present ? server.payload : std::string{}));
    if (expected == CloudReadState::Present) CHECK(snapshot.observedBytes == snapshot.bytes.size());
    u26CheckUnversioned(snapshot); // Even the real ETag does not establish CAS.
    CHECK(provider.conditionalWriteSupport(CloudSide::Cloud) == CloudConditionalWriteSupport::Unsupported);
    CHECK(server.gets.load() == 1);
    CHECK(server.redirectGets.load() == 0);
    CHECK(server.writes.load() == 0);
    CHECK(server.deletes.load() == 0);
}

TEST_CASE("U26 cloud snapshot: actual incomplete and oversized HTTP bodies are unusable") {
    using Mode = U26SnapshotHttpServer::Mode;
    Mode mode = Mode::ShortFixed;
    CloudReadState expected = CloudReadState::Failed;
    CloudReadError error = CloudReadError::Truncated;
    SUBCASE("fixed Content-Length is not satisfied") {}
    SUBCASE("chunked body has no terminator") { mode = Mode::ShortChunked; }
    SUBCASE("declared length exceeds the existing payload cap") {
        mode = Mode::DeclaredTooLarge; expected = CloudReadState::Invalid; error = CloudReadError::TooLarge;
    }
    SUBCASE("chunked payload exceeds the cap while receiving") {
        mode = Mode::StreamTooLarge; expected = CloudReadState::Invalid; error = CloudReadError::TooLarge;
    }
    U26SnapshotHttpServer server(mode);
    HttpCloudSaveProvider provider(server.endpoint(), 500);
    const auto snapshot = provider.readSnapshot(CloudSide::Cloud, "save_0.json");
    u26CheckReadFailure(snapshot, expected, error);
    CHECK(snapshot.httpStatus == 200);
    CHECK(snapshot.observedBytes <= u26SnapshotLocalLimit);
    CHECK(server.gets.load() == 1);
    CHECK(server.writes.load() == 0);
    CHECK(server.deletes.load() == 0);
}

TEST_CASE("U26 cloud snapshot: a controlled HTTP deadline is unavailable and never writes") {
    U26SnapshotHttpServer server(U26SnapshotHttpServer::Mode::Timeout);
    HttpCloudSaveProvider provider(server.endpoint(), 80);
    const auto snapshot = provider.readSnapshot(CloudSide::Cloud, "save_0.json");
    u26CheckReadFailure(snapshot, CloudReadState::Unavailable, CloudReadError::TransportUnavailable);
    CHECK(snapshot.httpStatus == 0);
    CHECK(snapshot.observedBytes == 0);
    CHECK(server.entered.load()); // Handler was actually waiting at its barrier.
    CHECK(server.gets.load() == 1);
    CHECK(server.writes.load() == 0);
    CHECK(server.deletes.load() == 0);
    // The fixture's destructor releases its handler before stop/join.
}

TEST_CASE("U26 cloud snapshot: Steam direct reads do not guess absence from zero") {
    U26SnapshotSteam steam;
    CloudSaveProvider provider(&steam);
    const std::string payload("sdk\0bytes", 9);
    steam.files["save_0.json"] = payload;
    CloudReadState expected = CloudReadState::Present;
    CloudReadError error = CloudReadError::None;
    bool backendUnavailable = false;
    SUBCASE("exact direct bytes") {}
    SUBCASE("SDK reports no file without authoritative absence") {
        steam.files.clear(); expected = CloudReadState::Unavailable; error = CloudReadError::IndeterminateMissing;
    }
    SUBCASE("zero size is not sufficient proof of a complete empty SDK read") {
        steam.files["save_0.json"].clear(); expected = CloudReadState::Failed; error = CloudReadError::IndeterminateSize;
    }
    SUBCASE("short direct read withholds all bytes") {
        steam.shortReadName = "save_0.json"; expected = CloudReadState::Failed; error = CloudReadError::Truncated;
    }
    SUBCASE("unavailable initialized interface cannot claim missing") {
        steam.available = false; backendUnavailable = true;
        expected = CloudReadState::Unavailable; error = CloudReadError::BackendUnavailable;
    }
    const auto original = steam.files;
    const auto snapshot = provider.readSnapshot(CloudSide::Cloud, "ignored-directory/save_0.json");
    CHECK(snapshot.state == expected);
    CHECK(snapshot.error == error);
    CHECK(snapshot.httpStatus == 0);
    u26CheckUnversioned(snapshot);
    if (expected == CloudReadState::Present) {
        CHECK(snapshot.bytes == payload);
        CHECK(snapshot.observedBytes == payload.size());
        CHECK(steam.reads == 1);
    } else {
        CHECK(snapshot.bytes.empty());
    }
    if (backendUnavailable) CHECK(steam.probes == 0);
    CHECK(steam.writes == 0);
    CHECK(steam.deletes == 0);
    CHECK(steam.files == original);
    CloudSaveProvider absent(nullptr);
    u26CheckReadFailure(absent.readSnapshot(CloudSide::Cloud, "save_0.json"),
                        CloudReadState::Unavailable, CloudReadError::BackendUnavailable);
}

TEST_CASE("U26 cloud snapshot: actual Steam generation reads reject incomplete or changed heads") {
    U26SnapshotSteam steam;
    CloudSaveProvider provider(&steam);
    const std::string payload(256 * 1024 + 19, 'g');
    REQUIRE(provider.writeFile("save_0.json", payload)); // Real production publication.
    REQUIRE(provider.readFile("save_0.json") == payload);
    CloudReadState expected = CloudReadState::Present;
    CloudReadError error = CloudReadError::None;
    std::string replacementHead;
    SUBCASE("published generation is complete and unversioned") {}
    SUBCASE("legacy metadata and chunks remain readable") {
        steam.files.clear();
        steam.files["save_0.json.meta"] = std::to_string(payload.size()) + ",2";
        steam.files["save_0.json.chunk000"] = payload.substr(0, 256 * 1024);
        steam.files["save_0.json.chunk001"] = payload.substr(256 * 1024);
    }
    SUBCASE("fully read malformed metadata is Invalid") {
        steam.files["save_0.json.meta"] = "v2,262163,2,../../foreign";
        expected = CloudReadState::Invalid; error = CloudReadError::MalformedMetadata;
    }
    SUBCASE("short metadata is not a parsed complete manifest") {
        steam.shortReadName = "save_0.json.meta";
        expected = CloudReadState::Failed; error = CloudReadError::Truncated;
    }
    SUBCASE("short generation chunk is not Present") {
        for (const auto& file : steam.files)
            if (file.first.find(".chunk000") != std::string::npos) steam.shortReadName = file.first;
        REQUIRE_FALSE(steam.shortReadName.empty());
        expected = CloudReadState::Failed; error = CloudReadError::Truncated;
    }
    SUBCASE("head changes at an actual SDK read boundary") {
        replacementHead = steam.files.at("save_0.json.meta");
        REQUIRE_FALSE(replacementHead.empty());
        replacementHead.back() = replacementHead.back() == '0' ? '1' : '0';
        steam.afterRead = [&](const std::string& name) {
            if (name.find(".chunk000") != std::string::npos)
                steam.files["save_0.json.meta"] = replacementHead;
        };
        expected = CloudReadState::Failed; error = CloudReadError::ChangedDuringRead;
    }
    const auto original = steam.files;
    steam.resetCounts();
    const auto snapshot = provider.readSnapshot(CloudSide::Cloud, "save_0.json");
    CHECK(snapshot.state == expected);
    CHECK(snapshot.error == error);
    u26CheckUnversioned(snapshot);
    if (expected == CloudReadState::Present) {
        CHECK(bool(snapshot.bytes == payload));
        CHECK(snapshot.observedBytes == payload.size());
    } else {
        CHECK(snapshot.bytes.empty());
    }
    CHECK(steam.writes == 0);
    CHECK(steam.deletes == 0);
    if (replacementHead.empty()) {
        CHECK(steam.files == original);
    } else {
        CHECK(steam.files["save_0.json.meta"] == replacementHead);
        CHECK(steam.files.size() == original.size());
    }
}

TEST_CASE("U26 cloud snapshot: optional legacy and conditional capabilities are explicit") {
    SingleReadCloudProvider legacy;
    ISaveProvider* old = &legacy;
    CHECK(dynamic_cast<ICloudSaveSnapshotTransport*>(old) == nullptr);
    CHECK(legacy.localReads == 0);
    CHECK(legacy.cloudReads == 0);
    CHECK_FALSE(legacy.unstagedAccess);
    U26SnapshotSteam steam;
    CloudSaveProvider cloud(&steam);
    U26SnapshotHttpServer server(U26SnapshotHttpServer::Mode::Present);
    HttpCloudSaveProvider http(server.endpoint(), 500);
    for (const auto side : {CloudSide::Local, CloudSide::Cloud}) {
        CHECK(cloud.conditionalWriteSupport(side) == CloudConditionalWriteSupport::Unsupported);
        CHECK(http.conditionalWriteSupport(side) == CloudConditionalWriteSupport::Unsupported);
    }
    CHECK(steam.probes == 0);
    CHECK(steam.reads == 0);
    CHECK(steam.writes == 0);
    CHECK(steam.deletes == 0);
    CHECK(server.gets.load() == 0);
    CHECK(server.writes.load() == 0);
    CHECK(server.deletes.load() == 0);
    const CloudSnapshot noCapability;
    CHECK(noCapability.state == CloudReadState::Unsupported);
    CHECK(noCapability.bytes.empty());
}

TEST_CASE("U26 cloud snapshot: cloud keys and endpoint configuration fail closed") {
    U26SnapshotSteam steam;
    CloudSaveProvider cloud(&steam);
    U26SnapshotHttpServer server(U26SnapshotHttpServer::Mode::Present);
    HttpCloudSaveProvider http(server.endpoint(), 500);
    for (const auto& key : std::array<std::string, 4>{"", ".", "..", std::string("save_0.json\0tail", 16)}) {
        for (ICloudSaveSnapshotTransport* transport :
             std::array<ICloudSaveSnapshotTransport*, 2>{&cloud, &http}) {
            u26CheckReadFailure(transport->readSnapshot(CloudSide::Cloud, key),
                                CloudReadState::Invalid, CloudReadError::InvalidPath);
        }
    }
    HttpCloudSaveProvider invalid("file:///not-an-http-endpoint");
    u26CheckReadFailure(invalid.readSnapshot(CloudSide::Cloud, "save_0.json"),
                        CloudReadState::Invalid, CloudReadError::InvalidEndpoint);
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
    auto endpoint = server.endpoint();
    endpoint.replace(0, 4, "https");
    HttpCloudSaveProvider unsupportedTls(endpoint, 500);
    u26CheckReadFailure(unsupportedTls.readSnapshot(CloudSide::Cloud, "save_0.json"),
                        CloudReadState::Unsupported, CloudReadError::UnsupportedTransport);
#endif
    CHECK(steam.probes == 0);
    CHECK(steam.reads == 0);
    CHECK(steam.writes == 0);
    CHECK(steam.deletes == 0);
    CHECK(server.gets.load() == 0);
    CHECK(server.redirectGets.load() == 0);
    CHECK(server.writes.load() == 0);
    CHECK(server.deletes.load() == 0);
}

// U26 conflict store: real opaque records; no provider or store verdict mocks.
namespace {
using U26Store = detail::CloudConflictStore;
using U26StoreCode = detail::ConflictStoreCode;
using U26RecordKind = detail::ConflictRecordKind;

bool u26ConflictSameBytes(const std::string& a, const std::string& b) {
    return a == b; // Keep raw payloads out of failed-assertion diagnostics.
}

bool u26ConflictHex(const std::string& value, size_t length) {
    return value.size() == length && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool u26ConflictSameRef(const detail::ConflictRecordRef& a,
                        const detail::ConflictRecordRef& b) {
    return a.id == b.id && a.manifestSha256 == b.manifestSha256;
}

const char* u26ConflictKindName(U26RecordKind kind) {
    switch (kind) {
        case U26RecordKind::EqualObserved: return "equal_observed";
        case U26RecordKind::LocalChanged: return "local_changed";
        case U26RecordKind::CloudChanged: return "cloud_changed";
        case U26RecordKind::Conflict: return "conflict";
        case U26RecordKind::DivergedWithoutBase: return "diverged_without_base";
    }
    return "invalid";
}

std::string u26ConflictSha(const std::string& bytes) {
    auto* crypto = BackendRegistry::instance().getCryptoEngine();
    REQUIRE(crypto != nullptr);
    std::array<uint8_t, 32> digest{};
    crypto->sha256(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(),
                   digest.data(), digest.size());
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : digest) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}

std::map<std::string, std::string> u26ConflictTree(const std::filesystem::path& root) {
    std::map<std::string, std::string> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        const auto relative = entry.path().lexically_relative(root).generic_string();
        const auto state = entry.symlink_status();
        if (std::filesystem::is_symlink(state))
            result[relative] = "L:" + std::filesystem::read_symlink(entry.path()).generic_string();
        else if (std::filesystem::is_directory(state)) result[relative] = "D:";
        else if (std::filesystem::is_regular_file(state))
            result[relative] = "F:" + cloudFileBytes(entry.path());
        else result[relative] = "S:";
    }
    return result;
}

struct U26ConflictFixture {
    CloudCryptoRegistration crypto;
    TestPaths::ScopedTempDir temporary{"u26_conflict_store"};
    std::filesystem::path root = std::filesystem::canonical(temporary.path());
    std::filesystem::path records = root / "preserved";
    std::filesystem::path slot = root / "save_3.json";
    EncryptedCloudServer server;
    HttpCloudSaveProvider transport{server.endpoint(), 1000};
    SaveManager manager;
    std::array<uint8_t, 32> key{};
    std::string a, b, c, currentLocal, currentCloud;

    U26ConflictFixture() {
        REQUIRE(std::filesystem::create_directory(records));
        manager.init(TestPaths::withTrailingSeparator(root));
        for (size_t i = 0; i < key.size(); ++i) key[i] = static_cast<uint8_t>(i + 1);
        manager.setEncryptionKey(key.data());
        manager.setEncryptionPolicy(SaveEncryptionPolicy::RequireEncrypted);
        a = makeEnvelope(1, 'A');
        b = makeEnvelope(2, 'B');
        c = makeEnvelope(3, 'C');
        REQUIRE_FALSE(u26ConflictSameBytes(a, b));
        REQUIRE_FALSE(u26ConflictSameBytes(a, c));
        REQUIRE_FALSE(u26ConflictSameBytes(b, c));
        setSides(a, a);
    }

    detail::ConflictStoreContext context() const {
        return {"0123456789abcdef0123456789abcdef", 3, 1};
    }

    std::string makeEnvelope(int chapter, char fill) {
        // More than one real AtomicSaveFile chunk; no guessed WriteProgress.
        const json data = {{"chapter", chapter}, {"opaque_test_data", std::string(70000, fill)}};
        REQUIRE(manager.save(3, data, "conflict-fixture", chapter));
        const auto bytes = cloudFileBytes(slot);
        REQUIRE(bytes.size() > 64u * 1024u);
        REQUIRE(bytes.substr(0, 4) == "CAES");
        CHECK(manager.load(3) == data);
        return bytes;
    }

    void setSides(const std::string& local, const std::string& cloud) {
        replaceCloudFileBytes(slot, local);
        server.replaceBytes(cloud); // Actual loopback server I/O boundary only.
        currentLocal = local;
        currentCloud = cloud;
    }

    std::pair<CloudSnapshot, CloudSnapshot> capture() {
        auto local = transport.readSnapshot(CloudSide::Local, u26SnapshotPath(slot));
        auto cloud = transport.readSnapshot(CloudSide::Cloud, u26SnapshotPath(slot));
        REQUIRE(local.state == CloudReadState::Present);
        REQUIRE(cloud.state == CloudReadState::Present);
        REQUIRE(local.error == CloudReadError::None);
        REQUIRE(cloud.error == CloudReadError::None);
        REQUIRE(u26ConflictSameBytes(local.bytes, currentLocal));
        REQUIRE(u26ConflictSameBytes(cloud.bytes, currentCloud));
        return {std::move(local), std::move(cloud)};
    }

    void checkSources() {
        CHECK(u26ConflictSameBytes(cloudFileBytes(slot), currentLocal));
        CHECK(u26ConflictSameBytes(server.bytes(), currentCloud));
        CHECK(server.writes() == 0);
        CHECK(transport.conditionalWriteSupport(CloudSide::Local) ==
              CloudConditionalWriteSupport::Unsupported);
        CHECK(transport.conditionalWriteSupport(CloudSide::Cloud) ==
              CloudConditionalWriteSupport::Unsupported);
    }
};

void u26ConflictCheckRecord(const detail::ConflictStoreResult& result,
                            U26StoreCode code, U26RecordKind kind,
                            const std::string& local, const std::string& cloud,
                            const std::optional<std::string>& base = std::nullopt) {
    CHECK(result.code == code);
    REQUIRE(result.record.has_value());
    const auto& record = *result.record;
    CHECK(record.kind == kind);
    CHECK(u26ConflictHex(record.ref.id, 32));
    CHECK(u26ConflictHex(record.ref.manifestSha256, 64));
    CHECK(u26ConflictSameBytes(record.localBytes, local));
    CHECK(u26ConflictSameBytes(record.cloudBytes, cloud));
    CHECK(record.baseBytes.has_value() == base.has_value());
    CHECK(record.baseRef.has_value() == base.has_value());
    if (base && record.baseBytes)
        CHECK(u26ConflictSameBytes(*record.baseBytes, *base));
    if (code == U26StoreCode::Preserved) {
        CHECK(result.operationId == record.ref.id);
        REQUIRE(result.candidateRef.has_value());
        CHECK(u26ConflictSameRef(*result.candidateRef, record.ref));
    }
}

detail::ConflictRecordRef u26ConflictSeedBase(U26ConflictFixture& fixture, U26Store& store) {
    fixture.setSides(fixture.a, fixture.a);
    const auto sides = fixture.capture();
    const auto result = store.preserve(fixture.context(), sides.first, sides.second);
    u26ConflictCheckRecord(result, U26StoreCode::Preserved, U26RecordKind::EqualObserved,
                           fixture.a, fixture.a);
    REQUIRE(result.record.has_value());
    return result.record->ref;
}

void u26ConflictCheckManifest(const std::filesystem::path& root,
                              const detail::PreservedConflictRecord& record) {
    const auto directory = root / record.ref.id;
    const auto raw = cloudFileBytes(directory / "manifest.json");
    CHECK(raw.size() <= 16u * 1024u);
    CHECK(u26ConflictSha(raw) == record.ref.manifestSha256);
    const auto manifest = json::parse(raw);
    REQUIRE(manifest.is_object());
    CHECK(manifest.size() == 8);
    for (const auto* key : {"schema_version", "id", "context", "kind", "envelope_validation",
                            "revision_kind", "base_ref", "payloads"})
        REQUIRE(manifest.contains(key));
    CHECK(manifest.at("schema_version") == 1);
    CHECK(manifest.at("id") == record.ref.id);
    CHECK(manifest.at("kind") == u26ConflictKindName(record.kind));
    CHECK(manifest.at("envelope_validation") == "NOT_CHECKED");
    CHECK(manifest.at("revision_kind") == "none");
    const auto& context = manifest.at("context");
    CHECK(context.size() == 3);
    CHECK(context.at("scope_id") == record.context.scopeId);
    CHECK(context.at("slot") == record.context.slot);
    CHECK(context.at("policy_epoch") == record.context.policyEpoch);
    CHECK(manifest.at("base_ref").is_null() == !record.baseRef.has_value());
    if (record.baseRef) {
        CHECK(manifest.at("base_ref").size() == 2);
        CHECK(manifest.at("base_ref").at("id") == record.baseRef->id);
        CHECK(manifest.at("base_ref").at("manifest_sha256") == record.baseRef->manifestSha256);
    }
    const auto& payloads = manifest.at("payloads");
    CHECK(payloads.size() == (record.baseBytes ? 3 : 2));
    for (const auto* role : {"local", "cloud", "base"}) {
        if (std::string(role) == "base" && !record.baseBytes) {
            CHECK_FALSE(payloads.contains(role));
            continue;
        }
        const auto& bytes = std::string(role) == "local" ? record.localBytes :
            std::string(role) == "cloud" ? record.cloudBytes : *record.baseBytes;
        const auto& payload = payloads.at(role);
        CHECK(payload.size() == 3);
        CHECK(payload.at("state") == "present");
        CHECK(payload.at("size") == bytes.size());
        CHECK(payload.at("sha256") == u26ConflictSha(bytes));
        CHECK(u26ConflictSameBytes(cloudFileBytes(directory / (std::string(role) + ".bin")), bytes));
    }
}

void u26ConflictCheckNoAttempt(const detail::ConflictStoreResult& result,
                               U26StoreCode code, const std::filesystem::path& root,
                               const std::map<std::string, std::string>& before) {
    CHECK(result.code == code);
    CHECK(result.operationId.empty());
    CHECK_FALSE(result.candidateRef.has_value());
    CHECK_FALSE(result.record.has_value());
    CHECK(u26ConflictTree(root) == before);
}

struct U26ConflictFailWrite {
    detail::SaveWriteStage target;
    size_t role = 0;
    size_t opened = 0;
    size_t fired = 0;
    std::filesystem::path storeRoot;
    bool ownedPath = true;
    std::string preparedManifestAtReplace{};

    static bool checkpoint(detail::SaveWriteStage stage, const std::filesystem::path& temporary,
                           void* context) {
        auto& self = *static_cast<U26ConflictFailWrite*>(context);
        self.ownedPath = self.ownedPath && temporary.parent_path().parent_path() == self.storeRoot;
        if (stage == detail::SaveWriteStage::CreateTemporary) ++self.opened;
        if (self.opened == self.role && stage == self.target) {
            if (self.ownedPath && self.role == 4 && stage == detail::SaveWriteStage::Replace)
                self.preparedManifestAtReplace = cloudFileBytes(temporary);
            ++self.fired;
            return false;
        }
        return true;
    }
};
} // namespace

TEST_CASE("U26 conflict store: equal observation reopens exact opaque bytes") {
    U26ConflictFixture fixture;
    const auto context = fixture.context();
    detail::ConflictRecordRef reference;
    {
        U26Store store(fixture.records);
        reference = u26ConflictSeedBase(fixture, store);
    }
    U26Store reopened(fixture.records);
    const auto record = reopened.readRecord(context, reference);
    u26ConflictCheckRecord(record, U26StoreCode::Complete, U26RecordKind::EqualObserved,
                           fixture.a, fixture.a);
    REQUIRE(record.record.has_value());
    u26ConflictCheckManifest(fixture.records, *record.record);
    const auto manifestPath = fixture.records / reference.id / "manifest.json";
    const auto rawManifest = cloudFileBytes(manifestPath);
    CHECK(u26ConflictSha(rawManifest) == reference.manifestSha256);
    const auto manifest = json::parse(rawManifest);
    CHECK(manifest.at("envelope_validation") == "NOT_CHECKED");
    CHECK(manifest.at("revision_kind") == "none");
    CHECK(manifest.at("context").at("scope_id") == context.scopeId);
    CHECK(manifest.at("context").at("slot") == context.slot);
    CHECK(manifest.at("context").at("policy_epoch") == context.policyEpoch);
    const auto listed = reopened.listRecords(context);
    CHECK(listed.code == U26StoreCode::Complete);
    REQUIRE(listed.completeRecords.size() == 1);
    CHECK(u26ConflictSameRef(listed.completeRecords.front(), reference));
    CHECK(listed.incompleteRecords == 0);
    CHECK(listed.invalidRecords == 0);
    fixture.checkSources();

    // A new store object is not a new process; this tests actual disk readback.
    // Complete empty opaque observations are retained without claiming CAES validity.
    fixture.setSides("", "");
    const auto empty = fixture.capture();
    const auto savedEmpty = reopened.preserve(context, empty.first, empty.second);
    u26ConflictCheckRecord(savedEmpty, U26StoreCode::Preserved, U26RecordKind::EqualObserved, "", "");
    fixture.checkSources();
}

TEST_CASE("U26 conflict store: explicit equal base preserves all fork bytes") {
    U26ConflictFixture fixture;
    U26Store store(fixture.records);
    const auto base = u26ConflictSeedBase(fixture, store);
    const auto baseTree = u26ConflictTree(fixture.records / base.id);
    struct Case { const std::string* local; const std::string* cloud; U26RecordKind kind; bool withBase; };
    const std::array<Case, 5> cases{{
        {&fixture.b, &fixture.c, U26RecordKind::Conflict, true},
        {&fixture.b, &fixture.a, U26RecordKind::LocalChanged, true},
        {&fixture.a, &fixture.c, U26RecordKind::CloudChanged, true},
        {&fixture.b, &fixture.b, U26RecordKind::EqualObserved, true},
        {&fixture.b, &fixture.c, U26RecordKind::DivergedWithoutBase, false}
    }};
    std::vector<std::string> ids{base.id};
    for (const auto& item : cases) {
        INFO("kind=", static_cast<int>(item.kind), " base=", item.withBase);
        fixture.setSides(*item.local, *item.cloud);
        const auto sides = fixture.capture();
        const auto expectedBase = item.withBase ? std::optional<detail::ConflictRecordRef>(base) : std::nullopt;
        const auto expectedBytes = item.withBase ? std::optional<std::string>(fixture.a) : std::nullopt;
        const auto result = store.preserve(fixture.context(), sides.first, sides.second, expectedBase);
        u26ConflictCheckRecord(result, U26StoreCode::Preserved, item.kind,
                               *item.local, *item.cloud, expectedBytes);
        REQUIRE(result.record.has_value());
        u26ConflictCheckManifest(fixture.records, *result.record);
        CHECK(std::find(ids.begin(), ids.end(), result.record->ref.id) == ids.end());
        ids.push_back(result.record->ref.id);
        U26Store reopened(fixture.records);
        const auto loaded = reopened.readRecord(fixture.context(), result.record->ref);
        u26ConflictCheckRecord(loaded, U26StoreCode::Complete, item.kind,
                               *item.local, *item.cloud, expectedBytes);
        CHECK(u26ConflictTree(fixture.records / base.id) == baseTree);
        fixture.checkSources();
    }
    // Same actual game data saved again is still a distinct raw observation.
    const auto sameGameData = fixture.makeEnvelope(2, 'B');
    REQUIRE_FALSE(u26ConflictSameBytes(sameGameData, fixture.b));
    fixture.setSides(fixture.b, sameGameData);
    const auto nonceSides = fixture.capture();
    const auto nonceRecord = store.preserve(fixture.context(), nonceSides.first, nonceSides.second, base);
    u26ConflictCheckRecord(nonceRecord, U26StoreCode::Preserved, U26RecordKind::Conflict,
                           fixture.b, sameGameData, fixture.a);
    fixture.checkSources();
    const auto listed = store.listRecords(fixture.context());
    CHECK(listed.code == U26StoreCode::Complete);
    CHECK(listed.completeRecords.size() == 7);
    CHECK(listed.incompleteRecords == 0);
    CHECK(listed.invalidRecords == 0);
}

TEST_CASE("U26 conflict store: incomplete reads cannot establish or replace a base") {
    U26ConflictFixture fixture;
    U26Store store(fixture.records);
    const auto sides = fixture.capture();
    const auto emptyTree = u26ConflictTree(fixture.records);
    // These three states come from the actual production HTTP reader.
    for (const auto mode : {U26SnapshotHttpServer::Mode::Missing,
                           U26SnapshotHttpServer::Mode::ServerError,
                           U26SnapshotHttpServer::Mode::ShortFixed}) {
        U26SnapshotHttpServer server(mode);
        HttpCloudSaveProvider transport(server.endpoint(), 500);
        const auto observed = transport.readSnapshot(CloudSide::Cloud, "save_0.json");
        REQUIRE(observed.state != CloudReadState::Present);
        REQUIRE(observed.bytes.empty());
        u26ConflictCheckNoAttempt(store.preserve(fixture.context(), sides.first, observed),
                                  U26StoreCode::InspectionIncomplete, fixture.records, emptyTree);
        CHECK(server.gets.load() == 1);
        CHECK(server.writes.load() == 0);
        CHECK(server.deletes.load() == 0);
    }
    // These are deliberately value-boundary controls, not extra HTTP evidence.
    for (auto state : {CloudReadState::Missing, CloudReadState::Unavailable,
                       CloudReadState::Failed, CloudReadState::Invalid, CloudReadState::Unsupported}) {
        CloudSnapshot absent;
        absent.state = state;
        if (state == CloudReadState::Unavailable) absent.error = CloudReadError::BackendUnavailable;
        if (state == CloudReadState::Failed) absent.error = CloudReadError::Io;
        if (state == CloudReadState::Invalid) absent.error = CloudReadError::InvalidPath;
        u26ConflictCheckNoAttempt(store.preserve(fixture.context(), sides.first, absent),
                                  U26StoreCode::InspectionIncomplete, fixture.records, emptyTree);
        absent.bytes = "unusable partial";
        u26ConflictCheckNoAttempt(store.preserve(fixture.context(), sides.first, absent),
                                  U26StoreCode::InvalidInput, fixture.records, emptyTree);
    }
    for (int variant = 0; variant < 4; ++variant) {
        auto context = fixture.context();
        if (variant == 0) context.scopeId = "../foreign";
        if (variant == 1) context.scopeId.clear();
        if (variant == 2) context.slot = 100;
        if (variant == 3) context.policyEpoch = 0;
        u26ConflictCheckNoAttempt(store.preserve(context, sides.first, sides.second),
                                  U26StoreCode::InvalidInput, fixture.records, emptyTree);
    }
    for (int variant = 0; variant < 3; ++variant) {
        auto invalid = sides.second;
        if (variant == 0) invalid.state = static_cast<CloudReadState>(999);
        if (variant == 1) invalid.error = CloudReadError::Io;
        if (variant == 2) {
            invalid.revisionKind = CloudRevisionKind::BackendOpaque;
            invalid.revision = "not-a-proven-conditional-write-capability";
        }
        u26ConflictCheckNoAttempt(store.preserve(fixture.context(), sides.first, invalid),
                                  U26StoreCode::InvalidInput, fixture.records, emptyTree);
    }
    const auto base = u26ConflictSeedBase(fixture, store);
    const auto before = u26ConflictTree(fixture.records);
    for (int variant = 0; variant < 4; ++variant) {
        auto context = fixture.context();
        auto reference = base;
        if (variant == 0) context.scopeId = "11111111111111111111111111111111";
        if (variant == 1) ++context.policyEpoch;
        if (variant == 2) context.slot = 4;
        if (variant == 3) reference.manifestSha256[0] = reference.manifestSha256[0] == 'a' ? 'b' : 'a';
        u26ConflictCheckNoAttempt(store.preserve(context, sides.first, sides.second, reference),
                                  U26StoreCode::InvalidInput, fixture.records, before);
    }
    const auto restored = store.readRecord(fixture.context(), base);
    u26ConflictCheckRecord(restored, U26StoreCode::Complete, U26RecordKind::EqualObserved, fixture.a, fixture.a);
    fixture.setSides(fixture.b, fixture.c);
    const auto fork = fixture.capture();
    const auto nonEqual = store.preserve(fixture.context(), fork.first, fork.second, base);
    u26ConflictCheckRecord(nonEqual, U26StoreCode::Preserved, U26RecordKind::Conflict,
                           fixture.b, fixture.c, fixture.a);
    REQUIRE(nonEqual.record.has_value());
    const auto withConflict = u26ConflictTree(fixture.records);
    u26ConflictCheckNoAttempt(store.preserve(fixture.context(), fork.first, fork.second, nonEqual.record->ref),
                              U26StoreCode::InvalidInput, fixture.records, withConflict);
    fixture.checkSources();
}

TEST_CASE("U26 conflict store: each precommit failure preserves previous records") {
    U26ConflictFixture fixture;
    U26Store store(fixture.records);
    const auto base = u26ConflictSeedBase(fixture, store);
    fixture.setSides(fixture.b, fixture.c);
    const auto sides = fixture.capture();
    const auto prior = store.preserve(fixture.context(), sides.first, sides.second, base);
    u26ConflictCheckRecord(prior, U26StoreCode::Preserved, U26RecordKind::Conflict,
                           fixture.b, fixture.c, fixture.a);
    REQUIRE(prior.record.has_value());
    const auto baseTree = u26ConflictTree(fixture.records / base.id);
    const auto priorTree = u26ConflictTree(fixture.records / prior.record->ref.id);
    const std::array<detail::SaveWriteStage, 6> stages{{
        detail::SaveWriteStage::CreateTemporary, detail::SaveWriteStage::Write,
        detail::SaveWriteStage::WriteProgress, detail::SaveWriteStage::Flush,
        detail::SaveWriteStage::Close, detail::SaveWriteStage::Replace
    }};
    uint64_t incomplete = 0;
    // Fixed role order is base, local, cloud, manifest; all are nonempty.
    for (size_t role = 1; role <= 4; ++role) {
        for (const auto stage : stages) {
            INFO("role=", role, " stage=", static_cast<int>(stage));
            U26ConflictFailWrite failure{stage, role, 0, 0, fixture.records, true};
            detail::ConflictStoreResult result;
            {
                detail::ScopedSaveWriteTestHook hook({&U26ConflictFailWrite::checkpoint, &failure});
                result = store.preserve(fixture.context(), sides.first, sides.second, base);
            }
            CHECK(failure.fired == 1);
            CHECK(failure.opened == role);
            CHECK(failure.ownedPath);
            CHECK(result.code == U26StoreCode::PublicationFailed);
            CHECK_FALSE(result.record.has_value());
            REQUIRE(u26ConflictHex(result.operationId, 32));
            CHECK_FALSE(std::filesystem::exists(fixture.records / result.operationId / "manifest.json"));
            CHECK(u26ConflictTree(fixture.records / base.id) == baseTree);
            CHECK(u26ConflictTree(fixture.records / prior.record->ref.id) == priorTree);
            U26Store reopened(fixture.records);
            const auto listed = reopened.listRecords(fixture.context());
            CHECK(listed.code == U26StoreCode::Complete);
            CHECK(listed.completeRecords.size() == 2);
            ++incomplete;
            CHECK(listed.incompleteRecords == incomplete);
            CHECK(listed.invalidRecords == 0);
            fixture.checkSources();
        }
    }
    CHECK(incomplete == 24);
    const auto next = store.preserve(fixture.context(), sides.first, sides.second, base);
    u26ConflictCheckRecord(next, U26StoreCode::Preserved, U26RecordKind::Conflict,
                           fixture.b, fixture.c, fixture.a);
    fixture.checkSources();
}

TEST_CASE("U26 conflict store: publication and reopen validate the full record closure") {
    U26ConflictFixture fixture;
    U26Store store(fixture.records);
    const auto base = u26ConflictSeedBase(fixture, store);
    fixture.setSides(fixture.b, fixture.c);
    const auto sides = fixture.capture();
    struct CorruptEarlierPayload {
        std::filesystem::path root;
        size_t opened = 0;
        bool changed = false;
        bool ownedPath = true;
        static bool checkpoint(detail::SaveWriteStage stage, const std::filesystem::path& temporary,
                               void* context) {
            auto& self = *static_cast<CorruptEarlierPayload*>(context);
            if (stage == detail::SaveWriteStage::CreateTemporary) ++self.opened;
            if (self.opened == 3 && stage == detail::SaveWriteStage::WriteProgress && !self.changed) {
                self.ownedPath = temporary.parent_path().parent_path() == self.root;
                if (!self.ownedPath) return false;
                const auto local = temporary.parent_path() / "local.bin";
                auto bytes = cloudFileBytes(local);
                if (bytes.empty()) return false;
                bytes.back() ^= 1;
                replaceCloudFileBytes(local, bytes);
                self.changed = true;
            }
            return true;
        }
    } corruption{fixture.records};
    detail::ConflictStoreResult refused;
    {
        detail::ScopedSaveWriteTestHook hook({&CorruptEarlierPayload::checkpoint, &corruption});
        refused = store.preserve(fixture.context(), sides.first, sides.second, base);
    }
    CHECK(corruption.changed);
    CHECK(corruption.ownedPath);
    CHECK(refused.code == U26StoreCode::PublicationFailed);
    CHECK_FALSE(refused.record.has_value());
    REQUIRE(u26ConflictHex(refused.operationId, 32));
    CHECK_FALSE(std::filesystem::exists(fixture.records / refused.operationId / "manifest.json"));

    const auto saved = store.preserve(fixture.context(), sides.first, sides.second, base);
    u26ConflictCheckRecord(saved, U26StoreCode::Preserved, U26RecordKind::Conflict,
                           fixture.b, fixture.c, fixture.a);
    REQUIRE(saved.record.has_value());
    u26ConflictCheckManifest(fixture.records, *saved.record);
    const auto ref = saved.record->ref;
    const auto directory = fixture.records / ref.id;
    const auto manifestPath = directory / "manifest.json";
    const auto manifestBytes = cloudFileBytes(manifestPath);
    REQUIRE(u26ConflictSha(manifestBytes) == ref.manifestSha256);
    for (const auto* name : {"base.bin", "local.bin", "cloud.bin"}) {
        const auto path = directory / name;
        const auto original = cloudFileBytes(path);
        REQUIRE_FALSE(original.empty());
        auto altered = original;
        altered.back() ^= 1;
        replaceCloudFileBytes(path, altered);
        U26Store reopened(fixture.records);
        const auto invalid = reopened.readRecord(fixture.context(), ref);
        CHECK(invalid.code == U26StoreCode::InvalidRecord);
        CHECK_FALSE(invalid.record.has_value());
        replaceCloudFileBytes(path, original);
        REQUIRE(std::filesystem::remove(path));
        const auto missing = reopened.readRecord(fixture.context(), ref);
        CHECK(missing.code == U26StoreCode::InvalidRecord);
        CHECK_FALSE(missing.record.has_value());
        REQUIRE(std::filesystem::create_directory(path));
        const auto nonRegular = reopened.readRecord(fixture.context(), ref);
        CHECK(nonRegular.code == U26StoreCode::InvalidRecord);
        CHECK_FALSE(nonRegular.record.has_value());
        REQUIRE(std::filesystem::remove(path));
        replaceCloudFileBytes(path, original);
    }
    for (int variant = 0; variant < 10; ++variant) {
        INFO("manifest mutation=", variant);
        auto manifest = json::parse(manifestBytes);
        if (variant == 0) manifest["schema_version"] = 999;
        if (variant == 1) manifest["extra_field"] = "refuse unknown schema fields";
        if (variant == 2) manifest["payloads"]["local"]["file"] = "../outside.bin";
        if (variant == 3) manifest["payloads"]["local"]["size"] = fixture.b.size() + 1;
        if (variant == 4) manifest["kind"] = "equal_observed"; // B != C.
        if (variant == 5) manifest["base_ref"]["id"] = "../foreign";
        if (variant == 7) manifest["payloads"]["local"]["size"] = -1;
        if (variant == 8) manifest["payloads"]["local"]["size"] = 1.5;
        if (variant == 9) manifest["context"]["policy_epoch"] = "1";
        auto altered = manifest.dump();
        if (variant == 6) altered.insert(1, "\"schema_version\":1,");
        replaceCloudFileBytes(manifestPath, altered);
        auto testReference = ref;
        testReference.manifestSha256 = u26ConflictSha(altered);
        U26Store reopened(fixture.records);
        const auto invalid = reopened.readRecord(fixture.context(), testReference);
        CHECK(invalid.code == U26StoreCode::InvalidRecord);
        CHECK_FALSE(invalid.record.has_value());
        // Original external hash must also continue to reject altered bytes.
        CHECK(reopened.readRecord(fixture.context(), ref).code == U26StoreCode::InvalidRecord);
        replaceCloudFileBytes(manifestPath, manifestBytes);
    }
    const auto restored = store.readRecord(fixture.context(), ref);
    u26ConflictCheckRecord(restored, U26StoreCode::Complete, U26RecordKind::Conflict,
                           fixture.b, fixture.c, fixture.a);
    fixture.checkSources();
}

TEST_CASE("U26 conflict store: capacity and incomplete recovery never evict originals") {
    U26ConflictFixture fixture;
    U26Store store(fixture.records);
    const auto sides = fixture.capture();
    const auto emptyTree = u26ConflictTree(fixture.records);
    {
        struct RestoreCrypto {
            carc::ICryptoEngine* previous = BackendRegistry::instance().getCryptoEngine();
            RestoreCrypto() { BackendRegistry::instance().setCryptoEngine(nullptr); }
            ~RestoreCrypto() { BackendRegistry::instance().setCryptoEngine(previous); }
        } removed;
        u26ConflictCheckNoAttempt(store.preserve(fixture.context(), sides.first, sides.second),
                                  U26StoreCode::CryptoUnavailable, fixture.records, emptyTree);
    }
    // Real production Steam snapshot larger than local store's explicit cap.
    U26SnapshotSteam sdk;
    sdk.files["save_0.json"] = std::string(10u * 1024u * 1024u + 1u, 'L');
    CloudSaveProvider cloud(&sdk);
    const auto large = cloud.readSnapshot(CloudSide::Cloud, "save_0.json");
    REQUIRE(large.state == CloudReadState::Present);
    REQUIRE(large.bytes.size() == 10u * 1024u * 1024u + 1u);
    u26ConflictCheckNoAttempt(store.preserve(fixture.context(), sides.first, large),
                              U26StoreCode::CapacityExceeded, fixture.records, emptyTree);
    CHECK(sdk.writes == 0);
    CHECK(sdk.deletes == 0);
    CHECK(cloud.conditionalWriteSupport(CloudSide::Cloud) == CloudConditionalWriteSupport::Unsupported);
    const auto base = u26ConflictSeedBase(fixture, store);
    const auto priorTree = u26ConflictTree(fixture.records);
    U26Store countLimited(fixture.records, {1, 256u * 1024u * 1024u});
    u26ConflictCheckNoAttempt(countLimited.preserve(fixture.context(), sides.first, sides.second),
                              U26StoreCode::CapacityExceeded, fixture.records, priorTree);
    U26Store bytesLimited(fixture.records, {128, 1});
    u26ConflictCheckNoAttempt(bytesLimited.preserve(fixture.context(), sides.first, sides.second),
                              U26StoreCode::CapacityExceeded, fixture.records, priorTree);
    fixture.setSides(fixture.b, fixture.c);
    const auto fork = fixture.capture();
    U26ConflictFailWrite failure{detail::SaveWriteStage::Replace, 4, 0, 0, fixture.records, true};
    detail::ConflictStoreResult pending;
    {
        detail::ScopedSaveWriteTestHook hook({&U26ConflictFailWrite::checkpoint, &failure});
        pending = store.preserve(fixture.context(), fork.first, fork.second, base);
    }
    CHECK(failure.fired == 1);
    CHECK(failure.ownedPath);
    CHECK(pending.code == U26StoreCode::PublicationFailed);
    REQUIRE(pending.candidateRef.has_value()); // Exact manifest prepared before Replace.
    CHECK(pending.candidateRef->id == pending.operationId);
    CHECK(u26ConflictHex(pending.candidateRef->manifestSha256, 64));
    REQUIRE_FALSE(failure.preparedManifestAtReplace.empty());
    CHECK(pending.candidateRef->manifestSha256 == u26ConflictSha(failure.preparedManifestAtReplace));
    U26Store reopened(fixture.records);
    const auto incomplete = reopened.readRecord(fixture.context(), *pending.candidateRef);
    CHECK(incomplete.code == U26StoreCode::Incomplete);
    CHECK_FALSE(incomplete.record.has_value());
    const auto beforeList = u26ConflictTree(fixture.records);
    const auto listed = reopened.listRecords(fixture.context());
    CHECK(listed.code == U26StoreCode::Complete);
    CHECK(listed.completeRecords.size() == 1);
    CHECK(listed.incompleteRecords == 1);
    CHECK(listed.invalidRecords == 0);
    CHECK(u26ConflictTree(fixture.records) == beforeList);
    U26Store incompleteCounts(fixture.records, {2, 256u * 1024u * 1024u});
    u26ConflictCheckNoAttempt(incompleteCounts.preserve(fixture.context(), fork.first, fork.second, base),
                              U26StoreCode::CapacityExceeded, fixture.records, beforeList);
    const auto restored = reopened.readRecord(fixture.context(), base);
    u26ConflictCheckRecord(restored, U26StoreCode::Complete, U26RecordKind::EqualObserved, fixture.a, fixture.a);
    fixture.checkSources();
}
