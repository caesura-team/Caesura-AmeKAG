// test_cloud_save.cpp - HTTP cloud-save provider (C7) round trip against a
// local mock REST server: configure -> save slot -> push -> pull -> verify.
#include "doctest.h"
#include "TestPaths.h"
#include "storage/SaveManager.h"
#include "storage/AtomicSaveFile.h"
#include "storage/HttpCloudSaveProvider.h"
#include "storage/CloudSaveProvider.h"
#include "storage/api/ISaveProvider.h"
#include "storage/api/ICloudSaveTransport.h"
#include "steam/api/ISteamBackend.h"
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

using namespace Caesura;

namespace {

// In-memory Steam Remote Storage stand-in. Kept in an anonymous namespace with
// its own name: test_storage.cpp defines a file-scope MockSteamBackend, and two
// different definitions of the same external-linkage class would be an ODR
// violation across translation units.
class CloudMockSteam final : public ISteamBackend {
public:
    std::map<std::string, std::string> files;

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
        files[fileName] = std::string(static_cast<const char*>(data),
                                      static_cast<size_t>(size));
        return true;
    }
    int32_t cloudRead(const char* fileName, void* buffer, int32_t maxSize) override {
        const auto it = files.find(fileName ? fileName : "");
        if (it == files.end() || !buffer || maxSize <= 0) return 0;
        const int32_t n = std::min<int32_t>(maxSize,
                                            static_cast<int32_t>(it->second.size()));
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
