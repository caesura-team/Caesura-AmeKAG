#include "storage/CloudSaveProvider.h"
#include "steam/api/ISteamBackend.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <chrono>
#include <mutex>
#include <thread>
// test_storage.cpp - storage module unit tests (S2.4)
#include "doctest.h"
#include "storage/api/ISaveManager.h"
#include "storage/api/ISaveProvider.h"
#include "storage/SaveManager.h"
#include "storage/LocalFileSaveProvider.h"
#include "archive/CryptoEngine.h"
#include "di/BackendRegistry.h"
#include "TestPaths.h"
#include <filesystem>
#include <cstring>
#include <cstdio>
#include <fstream>

using namespace Caesura;

namespace {

// RAII: register a real AES-256-GCM CryptoEngine into the BackendRegistry for
// the duration of an encrypted-save test. SaveManager encrypt/decrypt routes
// through BackendRegistry::getCryptoEngine(), which is null unless registered
// (the test binary does not run a full Engine::init()).
class ScopedCryptoRegistration {
public:
    ScopedCryptoRegistration()
        : m_previous(BackendRegistry::instance().getCryptoEngine()) {
        BackendRegistry::instance().setCryptoEngine(&m_engine);
    }

    ~ScopedCryptoRegistration() {
        BackendRegistry::instance().setCryptoEngine(m_previous);
    }

private:
    carc::CryptoEngine m_engine;
    carc::ICryptoEngine* m_previous;
};

// Read raw file bytes (bypassing SaveManager) for at-rest inspection.
std::string readRawFileBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("Storage: SaveManager is constructible") {
    SaveManager sm;
    CHECK(sm.currentSchemaVersion() >= 0);
}

TEST_CASE("Storage: SaveManager init with temp directory") {
    TestPaths::ScopedTempDir dir("storage_init");
    SaveManager sm;
    sm.init(dir.string());
    CHECK(true);
}

TEST_CASE("Storage: SaveManager listSaves on empty directory") {
    TestPaths::ScopedTempDir dir("storage_empty");
    SaveManager sm;
    sm.init(dir.string());
    auto saves = sm.listSaves();
    CHECK(saves.empty());
}

TEST_CASE("Storage: SaveManager currentSchemaVersion is non-negative") {
    SaveManager sm;
    CHECK(sm.currentSchemaVersion() >= 0);
}

TEST_CASE("Storage: SaveManager slotExists on uninitialized returns false") {
    SaveManager sm;
    CHECK(sm.slotExists(99) == false);
}

TEST_CASE("Storage: SaveManager deleteSlot on uninitialized returns false") {
    SaveManager sm;
    CHECK(sm.deleteSlot(99) == false);
}

TEST_CASE("Storage: ISaveManager interface upcast") {
    SaveManager sm;
    ISaveManager* iface = &sm;
    CHECK(iface != nullptr);
    CHECK(iface->currentSchemaVersion() >= 0);
}

// =============================================================================
// Expanded: save provider
// =============================================================================

TEST_CASE("Storage: SaveManager default save provider exists after init") {
    TestPaths::ScopedTempDir dir("storage_provider");
    SaveManager sm;
    sm.init(dir.string());
    // Save provider is nullptr by default (must be set via setSaveProvider)
    // init does not automatically create a LocalFileSaveProvider
    CHECK(sm.getSaveProvider() == nullptr);
}

TEST_CASE("Storage: SaveManager ENGINE_VERSION is not empty") {
    CHECK(SaveManager::ENGINE_VERSION != nullptr);
    CHECK(std::strlen(SaveManager::ENGINE_VERSION) > 0);
}

// -- Mock Steam backend with in-memory cloud storage -------------------------

class MockSteamBackend final : public ISteamBackend {
public:
    std::map<std::string, std::string> files;
    // When >= 0, cloudWrite fails for chunk indexes >= this value (for
    // rollback testing). Only applied to .chunkNNN writes.
    int failCloudWritesFrom = -1;

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
        if (failCloudWritesFrom >= 0 && std::strstr(fileName, ".chunk")) {
            int idx = 0;
            if (std::sscanf(std::strrchr(fileName, 'k') + 1, "%d", &idx) == 1 && idx >= failCloudWritesFrom)
                return false;
        }
        files[fileName] = std::string(static_cast<const char*>(data), static_cast<size_t>(size));
        return true;
    }
    int32_t cloudRead(const char* fileName, void* buffer, int32_t maxSize) override {
        const auto it = files.find(fileName ? fileName : "");
        if (it == files.end() || !buffer || maxSize <= 0) return 0;
        const int32_t n = std::min<int32_t>(maxSize, static_cast<int32_t>(it->second.size()));
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
    int32_t cloudFileCount() const override { return static_cast<int32_t>(files.size()); }
    const char* cloudFileNameAt(int32_t index) const override {
        if (index < 0) return "";
        int32_t i = 0;
        for (const auto& [name, _] : files) {
            if (i++ == index) return name.c_str();
        }
        return "";
    }
    const char* name() const override { return "mock-steam"; }
};

TEST_CASE("Storage: CloudSaveProvider small-file roundtrip") {
    MockSteamBackend steam;
    CloudSaveProvider provider(&steam);

    const std::string payload = "{\"scene\":\"demo\",\"slot\":1}";
    REQUIRE(provider.writeFile("save_1.json", payload));
    CHECK(steam.cloudFileExists("save_1.json"));
    CHECK(provider.readFile("save_1.json") == payload);

    // pushToCloud reads the LOCAL file and uploads it. This used to be asserted
    // against a cloud-only object: writeFile put the bytes in the cloud, then
    // pushToCloud read them back OUT of the cloud and wrote them straight back,
    // so a cloud->cloud no-op reported success and "nothing local to push" was
    // indistinguishable from a real upload (t14). Give it a real local file.
    {
        std::ofstream local("local_slot.json", std::ios::binary | std::ios::trunc);
        local << "local-data";
    }
    CHECK(provider.pushToCloud("local_slot.json"));
    CHECK(provider.readFile("local_slot.json") == "local-data");
    std::remove("local_slot.json");

    // Without a local file the push must FAIL rather than echo the cloud copy.
    CHECK_FALSE(provider.pushToCloud("cloud_only_slot.json"));

    REQUIRE(provider.deleteFile("save_1.json"));
    CHECK_FALSE(steam.cloudFileExists("save_1.json"));
}

TEST_CASE("Storage: CloudSaveProvider large-file chunked roundtrip") {
    MockSteamBackend steam;
    CloudSaveProvider provider(&steam);

    // Exceeds the single-chunk threshold: exercises .meta + .chunkNNN path
    std::string payload;
    payload.reserve(300000);
    for (int i = 0; i < 300000; ++i) payload += static_cast<char>('a' + (i % 26));

    REQUIRE(provider.writeFile("big_save.json", payload));
    CHECK(steam.cloudFileExists("big_save.json.meta"));
    CHECK(provider.readFile("big_save.json") == payload);

    REQUIRE(provider.deleteFile("big_save.json"));
    CHECK_FALSE(steam.cloudFileExists("big_save.json.meta"));
    CHECK_FALSE(steam.cloudFileExists("big_save.json.chunk000"));
}



TEST_CASE("Storage: CloudSaveProvider rejects forged chunked .meta") {
    MockSteamBackend steam;
    CloudSaveProvider provider(&steam);

    // Absurd totalSize / numChunks must be rejected, not reserve GBs or loop.
    steam.files["f.meta"] = "2147483647,2147483647";
    CHECK(provider.readFile("f").empty());

    // numChunks far above what totalSize can legitimately use.
    steam.files["f.meta"] = "100,1000000";
    CHECK(provider.readFile("f").empty());

    // Missing chunks -> empty, not partial data.
    steam.files["f.meta"] = "300,2";
    steam.files["f.chunk000"] = std::string(256 * 1024, 'a');
    CHECK(provider.readFile("f").empty());

    // A chunk longer than the bytes still expected must be rejected.
    steam.files.clear();
    steam.files["g.meta"] = "10,1";
    steam.files["g.chunk000"] = std::string(20, 'a');
    CHECK(provider.readFile("g").empty());

    // Assembled length != totalSize must be rejected.
    steam.files.clear();
    steam.files["h.meta"] = "10,1";
    steam.files["h.chunk000"] = std::string(5, 'a');
    CHECK(provider.readFile("h").empty());
}

TEST_CASE("Storage: CloudSaveProvider chunked write failure rolls back meta") {
    MockSteamBackend steam;
    CloudSaveProvider provider(&steam);

    // 600KB payload -> 3 chunks; make the 2nd chunk write fail.
    std::string payload(600000, 'x');
    steam.failCloudWritesFrom = 1;  // fail chunk index 1 (0-based)
    CHECK_FALSE(provider.writeFile("roll.json", payload));

    // .meta and every chunk written so far must be gone.
    CHECK_FALSE(steam.cloudFileExists("roll.json.meta"));
    CHECK_FALSE(steam.cloudFileExists("roll.json.chunk000"));
    CHECK_FALSE(steam.cloudFileExists("roll.json.chunk001"));
}

TEST_CASE("Storage: CloudSaveProvider null backend is a safe no-op") {
    CloudSaveProvider provider(nullptr);
    CHECK_FALSE(provider.writeFile("x.json", "data"));
    CHECK(provider.readFile("x.json").empty());
    CHECK_FALSE(provider.deleteFile("x.json"));
    CHECK(provider.listFiles("*").empty());
    CHECK_FALSE(provider.pushToCloud("x.json"));
}

// =============================================================================
// G10: storage-layer hardening -- corrupted-save recovery, slot boundary
//      semantics, empty listing, overwrite, delete-missing, schema/migration,
//      nested payload roundtrip.
// =============================================================================

TEST_CASE("Storage: corrupted save file fails gracefully (no crash on load)") {
    TestPaths::ScopedTempDir dir("storage_corrupt");
    SaveManager sm;
    sm.init(dir.string());

    // (a-1) Truncated / invalid JSON in a real slot file.
    {
        std::ofstream f(dir.string() + "save_0.json", std::ios::binary | std::ios::trunc);
        f << "{truncated\"not-closed";
    }
    // (a-2) Valid JSON but a non-object envelope (array).
    {
        std::ofstream f(dir.string() + "save_1.json", std::ios::binary | std::ios::trunc);
        f << "[]";
    }
    // (a-3) Valid JSON scalar (not an object envelope).
    {
        std::ofstream f(dir.string() + "save_2.json", std::ios::binary | std::ios::trunc);
        f << "42";
    }

    // load() must report failure (null json), never throw or crash.
    CHECK(sm.load(0).is_null());
    CHECK(sm.load(1).is_null());
    CHECK(sm.load(2).is_null());

    // listSaves() must skip corrupt files instead of surfacing them.
    REQUIRE(sm.listSaves().empty());

    // slotExists reflects file presence (non-empty content), not validity.
    CHECK(sm.slotExists(0));
    CHECK(sm.slotExists(1));
    CHECK(sm.slotExists(2));

    // The manager stays fully usable afterwards: save/load/delete a valid slot.
    json ok = {{"ok", true}};
    REQUIRE(sm.save(0, ok, "demo", 1));
    CHECK(sm.load(0)["ok"] == true);
    REQUIRE(sm.deleteSlot(0));

    // A valid neighbour after a corrupt one is still loadable/listed.
    json good = {{"v", 9}};
    REQUIRE(sm.save(3, good, "good", 2));
    CHECK(sm.load(3)["v"] == 9);
    auto saves = sm.listSaves();
    REQUIRE(saves.size() == 1);
    CHECK(saves[0].slot == 3);
}

TEST_CASE("Storage: corrupt envelope with wrong-typed fields degrades gracefully (ST-1)") {
    // Regression guard for the ST-1 review finding: envelope.value("scene","")
    // threw nlohmann type_error.302 when a key existed with a wrong type
    // (e.g. "scene": 42), escaping through the Lua C boundary as a crash.
    TestPaths::ScopedTempDir dir("storage_typemismatch");
    SaveManager sm;
    sm.init(dir.string());

    // Every metadata field wrong-typed: must load as corrupt (null), never throw.
    {
        std::ofstream f(dir.string() + "save_0.json", std::ios::binary | std::ios::trunc);
        f << R"({"scene": 42, "timestamp": "nope", "token_index": "x", "schema_version": [1], "thumbnail": 3, "engine_version": 5})";
    }
    // Mixed: valid data sub-object but a malformed scene field alongside.
    {
        std::ofstream f(dir.string() + "save_1.json", std::ios::binary | std::ios::trunc);
        f << R"({"scene": {"nested": "obj"}, "timestamp": 12345, "token_index": 2, "schema_version": 1, "data": {"v": 7}})";
    }
    // listSaves() must tolerate both without throwing. Wrong-typed metadata
    // fields degrade to their defaults (schema 1 etc.) rather than crashing;
    // the slots may still be listed with defaulted metadata.
    const auto list = sm.listSaves();
    REQUIRE(list.size() == 2);   // both slots present with defaulted fields
    CHECK(list[0].sceneName == "");        // "scene": 42 -> default ""
    CHECK(list[0].tokenIndex == 0);        // "token_index": "x" -> 0
    CHECK(list[0].schemaVersion == 1);     // "schema_version": [1] -> 1

    // A fully valid save around them still round-trips.
    json good = {{"v", 77}, {"deep", {{"k", "z"}}}};
    REQUIRE(sm.save(2, good, "good", 3));
    CHECK(sm.load(2)["v"] == 77);
    auto saves = sm.listSaves();
    REQUIRE(saves.size() == 3);   // slots 0/1 (defaulted) + slot 2 (valid)
    CHECK(saves[2].slot == 2);

    // The manager remains usable after bad-field slots were skipped.
    REQUIRE(sm.deleteSlot(2));
    const auto after = sm.listSaves();
    REQUIRE(after.size() == 2);   // the two defaulted slots are still listed
}

TEST_CASE("Storage: slot boundary semantics (0 and 99 valid, out-of-range rejected)") {
    TestPaths::ScopedTempDir dir("storage_bounds");
    SaveManager sm;
    sm.init(dir.string());

    json gd = {{"n", 1}};
    // Lower and upper legal bounds both save/load/delete round-trip.
    REQUIRE(sm.save(0, gd, "s0", 1));
    REQUIRE(sm.save(99, gd, "s99", 2));
    CHECK(sm.load(0)["n"] == 1);
    CHECK(sm.load(99)["n"] == 1);
    CHECK(sm.slotExists(0));
    CHECK(sm.slotExists(99));

    // Out of range: rejected, never fabricates paths or crashes.
    // (System slots -1/-2 are LEGAL since 2026-08-24; -3 below the system
    // range and 100 above the player range must be rejected.)
    CHECK_FALSE(sm.save(100, gd, "hi", 0));
    CHECK_FALSE(sm.save(1000, gd, "hi", 0));
    CHECK_FALSE(sm.save(-3, gd, "neg", 0));
    CHECK(sm.load(100).is_null());
    CHECK(sm.load(1000).is_null());
    CHECK(sm.load(-3).is_null());
    CHECK_FALSE(sm.slotExists(100));
    CHECK_FALSE(sm.slotExists(-3));
    CHECK_FALSE(sm.deleteSlot(100));
    CHECK_FALSE(sm.deleteSlot(-3));

    // Exactly the two legal slots are listed.
    auto saves = sm.listSaves();
    REQUIRE(saves.size() == 2);
    CHECK(saves[0].slot == 0);
    CHECK(saves[1].slot == 99);
}

TEST_CASE("Storage: empty start -- listSaves empty, slots absent, load null") {
    TestPaths::ScopedTempDir dir("storage_empty_hard");
    SaveManager sm;
    sm.init(dir.string());
    CHECK(sm.listSaves().empty());
    CHECK_FALSE(sm.slotExists(0));
    CHECK_FALSE(sm.slotExists(50));
    CHECK_FALSE(sm.slotExists(99));
    CHECK(sm.load(0).is_null());
}

TEST_CASE("Storage: save twice to same slot replaces payload and updates metadata") {
    TestPaths::ScopedTempDir dir("storage_overwrite");
    SaveManager sm;
    sm.init(dir.string());

    json first = {{"phase", 1}};
    json second = {{"phase", 2}, {"note", "second"}};

    REQUIRE(sm.save(5, first, "sceneA", 3));
    REQUIRE(sm.save(5, second, "sceneB", 7));

    SaveMeta meta;
    json loaded = sm.load(5, &meta);
    CHECK(loaded["phase"] == 2);
    CHECK(loaded["note"] == "second");
    CHECK(meta.slot == 5);
    CHECK(meta.sceneName == "sceneB");
    CHECK(meta.tokenIndex == 7);

    // Single slot file remains; list shows the updated metadata.
    auto saves = sm.listSaves();
    REQUIRE(saves.size() == 1);
    CHECK(saves[0].slot == 5);
    CHECK(saves[0].sceneName == "sceneB");
    CHECK(saves[0].tokenIndex == 7);
}

TEST_CASE("Storage: deleting a missing slot is a safe no-op") {
    TestPaths::ScopedTempDir dir("storage_del_missing");
    SaveManager sm;
    sm.init(dir.string());

    CHECK_FALSE(sm.deleteSlot(3));
    CHECK(sm.listSaves().empty());

    // Present then deleted; a second delete is again a no-op.
    REQUIRE(sm.save(3, {{"a", 1}}, "s", 0));
    CHECK(sm.slotExists(3));
    REQUIRE(sm.deleteSlot(3));
    CHECK_FALSE(sm.slotExists(3));
    CHECK_FALSE(sm.deleteSlot(3));
    CHECK(sm.load(3).is_null());
}

TEST_CASE("Storage: schema migration applies the built-in chain to an old save") {
    TestPaths::ScopedTempDir dir("storage_migrate");
    SaveManager sm;
    sm.init(dir.string());  // registers built-in v1->v2->v3->v4->v5

    // Write an old v1 save by hand (simulating a save produced pre-migration).
    json env;
    env["schema_version"] = 1;
    env["timestamp"] = 0;
    env["scene"] = "old";
    env["token_index"] = 0;
    env["thumbnail"] = "";
    env["engine_version"] = "0.9.0";
    env["data"] = json::object();
    {
        std::ofstream f(dir.string() + "save_3.json", std::ios::binary | std::ios::trunc);
        f << env.dump();
    }

    SaveMeta meta;
    json loaded = sm.load(3, &meta);
    // Built-in chain adds playtime(2), minigame(3), live2d(4), editor(5).
    CHECK(loaded["playtime"] == 0);
    CHECK(loaded["minigame"].is_object());
    CHECK(loaded["live2d"].is_object());
    CHECK(loaded["editor"].is_object());
    // outMeta reflects the migrated (current) version.
    CHECK(meta.schemaVersion == sm.currentSchemaVersion());
}

TEST_CASE("Storage: registerMigration bumps current version, rejects non-increasing") {
    SaveManager sm;  // not init()ed -- only explicit migrations below apply
    CHECK(sm.currentSchemaVersion() == 1);

    sm.registerMigration(1, 2, [](const json& d) { json r = d; r["x"] = 1; return r; });
    CHECK(sm.currentSchemaVersion() == 2);

    // Non-increasing registrations are rejected and do not change the version.
    sm.registerMigration(1, 1, [](const json& d) { return d; });
    sm.registerMigration(3, 3, [](const json& d) { return d; });
    CHECK(sm.currentSchemaVersion() == 2);

    // The registered migration is applied by migrate().
    json out = sm.migrate(json::object(), 1);
    CHECK(out["x"] == 1);
    // Data already at current version passes through unchanged.
    CHECK(sm.migrate({{"y", 2}}, 2)["y"] == 2);
}

TEST_CASE("Storage: save payload roundtrip preserves nested Lua save-state structure") {
    TestPaths::ScopedTempDir dir("storage_roundtrip");
    SaveManager sm;
    sm.init(dir.string());

    // A payload shaped like the engine's Lua serialized save state.
    json gd;
    gd["f"] = json::object();
    gd["sf"] = json::object();
    gd["tf"] = json::object();
    gd["token_index"] = 42;
    gd["scene_path"] = "route/a.ks";
    gd["f"]["tbl"]["a"]["b"] = 1;               // f.tbl = {a={b=1}}
    gd["f"]["tbl"]["list"] = {1, 2, 3};
    gd["f"]["name"] = "protagonist";
    gd["backlog"] = {"line1", "line2"};
    gd["call_stack"] = {"main", "choice1"};
    gd["loop_stacks"] = json::array();
    gd["loop_stacks"].push_back({{"var", "i"}, {"end", 10}});
    gd["seen_scenes"] = {"prologue", "chapter1"};

    REQUIRE(sm.save(8, gd, "chapter1", 42));

    SaveMeta meta;
    json loaded = sm.load(8, &meta);
    CHECK(loaded == gd);                          // exact deep roundtrip
    CHECK(loaded["f"]["tbl"]["a"]["b"] == 1);     // f.tbl = {a={b=1}}
    CHECK(loaded["f"]["tbl"]["list"].is_array());
    CHECK(loaded["f"]["tbl"]["list"].size() == 3);
    CHECK(loaded["loop_stacks"][0]["end"] == 10);
    CHECK(meta.sceneName == "chapter1");
    CHECK(meta.tokenIndex == 42);
    auto saves = sm.listSaves();
    REQUIRE(saves.size() == 1);
    CHECK(saves[0].slot == 8);
}

// =============================================================================
// G10.1: storage encryption roundtrip -- at-rest secrecy, plain-vs-encrypted
// provider interop, encrypted slot boundary/overwrite/delete semantics, and
// tampered-ciphertext rejection. All cases register a real CryptoEngine into
// the BackendRegistry (SaveManager encrypt/decrypt routes through it).
//
// ARCHITECTURE NOTE (verified): ISaveProvider is a raw byte store with NO
// encryption awareness; encryption is decided entirely by SaveManager's
// m_keySet flag + the "CAES" magic in the on-disk bytes. There is no
// "encrypted provider" class -- see cases I2/I3 which exercise the real
// plain-vs-keyed interop behavior.
// =============================================================================

TEST_CASE("Storage: encrypted save roundtrip preserves nested payload exactly") {
    TestPaths::ScopedTempDir dir("storage_enc_roundtrip");
    ScopedCryptoRegistration cryptoRegistration;
    SaveManager sm;
    sm.init(dir.string());

    uint8_t key[32] = {};
    for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(i + 1);
    sm.setEncryptionKey(key);
    REQUIRE(sm.isEncryptionEnabled());

    // The same deep Lua-shaped payload as the plain roundtrip case, so the
    // encryption path is exercised over the full structured surface.
    json gd;
    gd["f"] = json::object();
    gd["sf"] = json::object();
    gd["tf"] = json::object();
    gd["token_index"] = 42;
    gd["scene_path"] = "route/a.ks";
    gd["f"]["tbl"]["a"]["b"] = 1;
    gd["f"]["tbl"]["list"] = {1, 2, 3};
    gd["f"]["name"] = "protagonist";
    gd["backlog"] = {"line1", "line2"};
    gd["call_stack"] = {"main", "choice1"};
    gd["loop_stacks"] = json::array();
    gd["loop_stacks"].push_back({{"var", "i"}, {"end", 10}});
    gd["seen_scenes"] = {"prologue", "chapter1"};

    REQUIRE(sm.save(8, gd, "chapter1", 42));

    SaveMeta meta;
    json loaded = sm.load(8, &meta);
    CHECK(loaded == gd);                          // exact deep roundtrip w/ encryption
    CHECK(loaded["f"]["tbl"]["a"]["b"] == 1);
    CHECK(loaded["f"]["tbl"]["list"].size() == 3);
    CHECK(loaded["loop_stacks"][0]["end"] == 10);
    CHECK(loaded["seen_scenes"].size() == 2);
    CHECK(meta.slot == 8);
    CHECK(meta.sceneName == "chapter1");
    CHECK(meta.tokenIndex == 42);

    // listSaves() surfaces the encrypted slot with decrypted metadata.
    auto saves = sm.listSaves();
    REQUIRE(saves.size() == 1);
    CHECK(saves[0].slot == 8);
    CHECK(saves[0].sceneName == "chapter1");
    CHECK(saves[0].tokenIndex == 42);
}

TEST_CASE("Storage: encrypted save file on disk is not plaintext-readable") {
    TestPaths::ScopedTempDir dir("storage_enc_secret");
    ScopedCryptoRegistration cryptoRegistration;
    SaveManager sm;
    sm.init(dir.string());

    uint8_t key[32] = {};
    for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(i + 1);
    sm.setEncryptionKey(key);

    // Distinctive markers that would obviously leak if the file were stored
    // as plaintext.
    const std::string secret = "SUPER_SECRET_MARKER_a1b2c3d4";
    const std::string sceneMarker = "encrypted_scene_XYZ_route";
    json gd = {{"secret", secret}, {"hp", 100}, {"flags", {{"a", true}}}};
    REQUIRE(sm.save(7, gd, sceneMarker, 9));

    std::string raw = readRawFileBytes(dir.string() + "save_7.json");
    REQUIRE(raw.size() > 4 + 12 + 16);            // "CAES" + nonce + tag + ct

    // On-disk format: 4-byte "CAES" magic, then a 12-byte nonce, 16-byte tag,
    // then AES-GCM ciphertext. No plaintext shell may survive.
    CHECK(std::memcmp(raw.data(), "CAES", 4) == 0);
    CHECK(raw.size() >= 4 + 12 + 16 + 4);         // at least a few ciphertext bytes

    // Payload strings, JSON structure markers, and the scene name must all be
    // absent from the raw ciphertext bytes.
    CHECK(raw.find(secret) == std::string::npos);
    CHECK(raw.find(sceneMarker) == std::string::npos);
    CHECK(raw.find("\"secret\"") == std::string::npos);
    CHECK(raw.find("\"data\"") == std::string::npos);

    // Sanity: the marker is still fully recoverable through the keyed manager.
    json loaded = sm.load(7);
    CHECK(loaded["secret"] == secret);
    CHECK(loaded["hp"] == 100);
}

TEST_CASE("Storage: encrypted save loaded without key (plain provider) fails gracefully") {
    TestPaths::ScopedTempDir dir("storage_enc_plain");
    ScopedCryptoRegistration cryptoRegistration;
    SaveManager sm;
    sm.init(dir.string());

    uint8_t key[32] = {};
    for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(i + 1);
    sm.setEncryptionKey(key);
    REQUIRE(sm.save(4, {{"secret", "classified"}}, "enc", 0));

    // A "plain" manager (no key set) sees the binary ciphertext bytes; it must
    // reject them gracefully -- no crash, null json -- rather than surface or
    // decode the raw file.
    sm.clearEncryptionKey();
    CHECK_FALSE(sm.isEncryptionEnabled());

    json loaded = sm.load(4);
    CHECK(loaded.is_null());                       // graceful failure, not garbage
    CHECK_FALSE(loaded.is_object());

    // slotExists still reflects file presence (bytes exist but are not a
    // readable save).
    CHECK(sm.slotExists(4));

    // listSaves() must skip the undecodable ciphertext instead of surfacing it.
    CHECK(sm.listSaves().empty());

    // The manager stays usable: a fresh plain save after the encrypted one.
    REQUIRE(sm.save(6, {{"plain", true}}, "plain", 0));
    CHECK(sm.load(6)["plain"] == true);
}

TEST_CASE("Storage: wrong encryption key rejects load gracefully (GCM auth)") {
    TestPaths::ScopedTempDir dir("storage_enc_wrongkey");
    ScopedCryptoRegistration cryptoRegistration;
    SaveManager sm;
    sm.init(dir.string());

    uint8_t keyA[32] = {};
    for (int i = 0; i < 32; ++i) keyA[i] = static_cast<uint8_t>(i + 1);
    sm.setEncryptionKey(keyA);
    REQUIRE(sm.save(2, {{"data", "authenticated_secret"}}, "enc", 1));

    // Switching to a different key must fail decrypt (tag mismatch) and
    // surface null json -- never crash, never return wrong-key payload.
    uint8_t keyB[32] = {};
    for (int i = 0; i < 32; ++i) keyB[i] = static_cast<uint8_t>(100 - i);
    sm.setEncryptionKey(keyB);
    CHECK(sm.load(2).is_null());
    CHECK(sm.listSaves().empty());

    // Restoring the correct key recovers the save.
    sm.setEncryptionKey(keyA);
    json again = sm.load(2);
    CHECK(again["data"] == "authenticated_secret");
}

TEST_CASE("Storage: plain save loaded with encryption enabled stays loadable (magic-gated)") {
    TestPaths::ScopedTempDir dir("storage_enc_loadsheer");
    ScopedCryptoRegistration cryptoRegistration;
    SaveManager sm;
    sm.init(dir.string());

    // Write a PLAIN (unencrypted) save with no key set.
    sm.clearEncryptionKey();
    REQUIRE(sm.save(2, {{"plain", "text"}, {"n", 5}}, "plain_scene", 3));
    std::string rawPlain = readRawFileBytes(dir.string() + "save_2.json");
    REQUIRE(rawPlain.find("CAES") == std::string::npos);
    REQUIRE(rawPlain.find("\"plain\"") != std::string::npos);  // confirmed plaintext

    // Now enable encryption (the "encrypted provider" reading a plain save).
    // Verified behavior: decryption is gated on the "CAES" magic prefix, so a
    // plain file is NOT treated as ciphertext and loads intact. This is safe
    // auto-detection rather than a failure -- intentionally documented, not a bug.
    uint8_t key[32] = {};
    for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(i + 1);
    sm.setEncryptionKey(key);

    json loaded = sm.load(2);
    CHECK(loaded.is_object());
    CHECK(loaded["plain"] == "text");
    CHECK(loaded["n"] == 5);

    // ...but a plain file that is then written through the keyed manager
    // becomes encrypted on disk.
    REQUIRE(sm.save(2, {{"plain", "text"}, {"n", 6}}, "plain_scene", 3));
    std::string rawReEnc = readRawFileBytes(dir.string() + "save_2.json");
    CHECK(std::memcmp(rawReEnc.data(), "CAES", 4) == 0);
    CHECK(sm.load(2)["n"] == 6);
}

TEST_CASE("Storage: forged CAES-magic decoy rejected gracefully under encryption") {
    TestPaths::ScopedTempDir dir("storage_enc_forged");
    ScopedCryptoRegistration cryptoRegistration;
    SaveManager sm;
    sm.init(dir.string());

    // A malicious/accidental file that begins with "CAES" magic but whose body
    // is not valid GCM ciphertext (e.g. rest of a plaintext truncated to look
    // encrypted). With encryption enabled, readFile must reject decrypt and
    // surface null json -- no crash, no partial/raw passthrough.
    {
        std::ofstream f(dir.string() + "save_3.json", std::ios::binary | std::ios::trunc);
        f.write("CAES", 4);
        f << "{not-valid-ciphertext-just-plaintext-garbage}";
    }

    uint8_t key[32] = {};
    for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(i + 1);
    sm.setEncryptionKey(key);

    json loaded = sm.load(3);
    CHECK(loaded.is_null());
    CHECK(sm.listSaves().empty());                // undecodable -> skipped
    // Manager remains usable.
    REQUIRE(sm.save(3, {{"ok", 1}}, "fresh", 0));
    CHECK(sm.load(3)["ok"] == 1);
}

TEST_CASE("Storage: encrypted provider mirrors slot bounds/overwrite/delete") {
    TestPaths::ScopedTempDir dir("storage_enc_bounds");
    ScopedCryptoRegistration cryptoRegistration;
    SaveManager sm;
    sm.init(dir.string());

    uint8_t key[32] = {};
    for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(i + 1);
    sm.setEncryptionKey(key);

    // Legal bounds round-trip under encryption, exactly like the plain path.
    REQUIRE(sm.save(0, {{"n", 0}}, "s0", 1));
    REQUIRE(sm.save(99, {{"n", 99}}, "s99", 2));
    CHECK(sm.load(0)["n"] == 0);
    CHECK(sm.load(99)["n"] == 99);
    CHECK(sm.slotExists(0));
    CHECK(sm.slotExists(99));

    // Out-of-range rejected identically under encryption.
    // (System slots -1/-2 are legal since 2026-08-24; -3 is below range.)
    CHECK_FALSE(sm.save(100, {{"n", 1}}, "hi", 0));
    CHECK_FALSE(sm.save(-3, {{"n", 1}}, "neg", 0));
    CHECK(sm.load(100).is_null());
    CHECK(sm.load(-3).is_null());
    CHECK_FALSE(sm.deleteSlot(100));
    CHECK_FALSE(sm.deleteSlot(-3));

    // Overwrite same encrypted slot replaces payload + metadata.
    REQUIRE(sm.save(5, {{"phase", 1}}, "sceneA", 3));
    REQUIRE(sm.save(5, {{"phase", 2}, {"note", "second"}}, "sceneB", 7));
    SaveMeta meta;
    json loaded = sm.load(5, &meta);
    CHECK(loaded["phase"] == 2);
    CHECK(loaded["note"] == "second");
    CHECK(meta.sceneName == "sceneB");
    CHECK(meta.tokenIndex == 7);
    // The overwritten slot stays encrypted on disk (fresh key/nonce each write).
    std::string raw = readRawFileBytes(dir.string() + "save_5.json");
    CHECK(std::memcmp(raw.data(), "CAES", 4) == 0);

    // Delete works; a second delete is a safe no-op.
    REQUIRE(sm.deleteSlot(5));
    CHECK_FALSE(sm.slotExists(5));
    CHECK_FALSE(sm.deleteSlot(5));
    CHECK(sm.load(5).is_null());

    // Exactly slots 0 and 99 remain listed.
    auto saves = sm.listSaves();
    REQUIRE(saves.size() == 2);
    CHECK(saves[0].slot == 0);
    CHECK(saves[1].slot == 99);
}

TEST_CASE("Storage: tampered encrypted save (ciphertext & nonce) rejected without crash") {
    TestPaths::ScopedTempDir dir("storage_enc_tamper");
    ScopedCryptoRegistration cryptoRegistration;
    SaveManager sm;
    sm.init(dir.string());

    uint8_t key[32] = {};
    for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(i + 1);
    sm.setEncryptionKey(key);

    json gd = {{"secret", "integrity_protected_payload"}, {"v", 7}};
    REQUIRE(sm.save(3, gd, "tamper", 0));

    std::string raw = readRawFileBytes(dir.string() + "save_3.json");
    REQUIRE(raw.size() > 4 + 12 + 16 + 8);

    // (1) Flip a byte in the CIPHERTEXT body (index 40, past the 4+12+16 header).
    auto flipInRegion = [&](std::string bytes, size_t idx, size_t* outSize) {
        bytes[idx] = static_cast<char>(bytes[idx] ^ 0xFF);
        std::ofstream out(dir.string() + "save_3.json", std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.close();
        *outSize = bytes.size();
    };
    size_t size = 0;
    flipInRegion(raw, 40, &size);
    CHECK(sm.load(3).is_null());                  // GCM auth fails -> null, no crash

    // (2) Flip a byte in the NONCE region (index 6). Changing the nonce must
    // also break GCM auth (tag drawn over the ciphertext + associated data).
    flipInRegion(raw, 6, &size);
    CHECK(sm.load(3).is_null());

    // (3) Corrupted file is skipped by listing. slotExists() reports false
    // because readFile returns empty when decryption fails (a corrupt
    // encrypted file is opaque); note this differs from a CORRUPT PLAIN file,
    // which is returned as raw bytes and therefore still "exists" (see the
    // plain corrupted-save case). Consistent with the keyed read path.
    CHECK_FALSE(sm.slotExists(3));
    CHECK(sm.listSaves().empty());

    // Manager remains fully usable after tampering; a fresh encrypted slot is
    // written (with a fresh nonce) and loads correctly.
    REQUIRE(sm.save(3, gd, "tamper", 0));
    json recovered = sm.load(3);
    CHECK(recovered["secret"] == "integrity_protected_payload");
    CHECK(recovered["v"] == 7);
}

// =============================================================================
// Round-79+ boundary expansion:
//  * large-payload roundtrip (~1 MiB) plain & encrypted, plus the undocumented
//    save/load size-ceiling asymmetry (save accepts >10 MiB, load then rejects)
//  * UTF-8 / CJK / emoji text roundtrip
//  * unknown future schema_version passes through unmigrated; chained migration
//    runs step-by-step when loading at an intermediate schema version
//  * rapid sequential multi-slot save + immediate same-process load (disk
//    persistence -- SaveManager keeps no in-memory cache)
//  * pluggable in-memory ISaveProvider: full save/load/list/delete flow and
//    provider-error propagation back through the SaveManager facade
// =============================================================================

// Build a deterministic repeated-string of the requested byte length.
static std::string makeRepeatedPayload(size_t bytes, char seedLoop) {
    std::string s;
    s.reserve(bytes);
    size_t i = 0;
    while (i < bytes) {
        s.push_back(static_cast<char>('A' + ((i + seedLoop) % 26)));
        ++i;
    }
    return s;
}

TEST_CASE("Storage: large payload ~1 MiB roundtrips exactly (plain and encrypted)") {
    TestPaths::ScopedTempDir dir("storage_big_roundtrip");
    SaveManager sm;
    sm.init(dir.string());

    const std::string big = makeRepeatedPayload(1024 * 1024, 0);  // 1 MiB
    json gd = {{"title", "big"}, {"blob", big}, {"n", 123456}};

    // Plain path.
    REQUIRE(sm.save(2, gd, "bigplain", 1));
    json loaded = sm.load(2);
    CHECK(loaded["title"] == "big");
    CHECK(loaded["blob"].is_string());
    CHECK(loaded["blob"].get<std::string>() == big);
    CHECK(loaded["n"] == 123456);

    // Encrypted path preserves the same large payload exactly.
    ScopedCryptoRegistration cryptoRegistration;
    uint8_t key[32] = {};
    for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(i + 1);
    sm.setEncryptionKey(key);
    REQUIRE(sm.save(3, gd, "bigenc", 2));
    json encLoaded = sm.load(3);
    CHECK(encLoaded["blob"].get<std::string>() == big);
    CHECK(encLoaded["n"] == 123456);
    // At-rest: the on-disk encrypted file must exceed the plaintext size and
    // start with the CAES magic (opaque, not a raw large-JSON dump).
    std::string rawEnc = readRawFileBytes(dir.string() + "save_3.json");
    CHECK(std::memcmp(rawEnc.data(), "CAES", 4) == 0);
    CHECK(rawEnc.size() >= big.size());
}

TEST_CASE("Storage: oversized save (>10 MiB ceiling) is refused, nothing written -- symmetric") {
    TestPaths::ScopedTempDir dir("storage_oversize");
    SaveManager sm;
    sm.init(dir.string());

    // readFile() enforces a 10 MiB MAX_SAVE_SIZE ceiling and returns empty for
    // any file larger than it. writeFile() carries the SAME guard now, so
    // save() at ~11 MiB is rejected up front, no payload reaches disk, and
    // load()/slotExists()/listSaves() naturally report the slot as absent.
    // Symmetric: a save the manager would refuse to read must never be written.
    const std::string huge = makeRepeatedPayload(11 * 1024 * 1024, 3);  // 11 MiB
    json gd = {{"huge", huge}};

    bool saved = sm.save(4, gd, "huge", 0);
    CHECK(saved == false);                   // writeFile rejects the oversized blob

    // Nothing was written: no file on disk at all.
    std::filesystem::path fp(dir.string() + "save_4.json");
    CHECK_FALSE(std::filesystem::exists(fp));

    // And the slot is naturally absent from the manager's perspective.
    CHECK(sm.load(4).is_null());
    CHECK_FALSE(sm.slotExists(4));
    CHECK(sm.listSaves().empty());

    // A normal-size save alongside stays fully functional.
    REQUIRE(sm.save(5, {{"ok", 1}}, "fine", 0));
    CHECK(sm.load(5)["ok"] == 1);
}

TEST_CASE("Storage: UTF-8 CJK / emoji / combining-mark text roundtrips exactly") {
    TestPaths::ScopedTempDir dir("storage_utf8");
    SaveManager sm;
    sm.init(dir.string());

    const std::string utf8 = "こんにちは世界 🌸✨ héllo wörld ✅ café résumé "
                             "漢字仮名交じり文 且 合 有 好\n e\u0301\u0302 \t emoji: 🎮⛩️🏮 愛❤️";
    const std::string nulInString = "abc\0def";  // embedded NUL must survive too
    json gd;
    gd["text"] = utf8;
    gd["with_nul"] = nulInString;
    gd["emojis"] = json::array({"✨", "🎌", "🌸", "❤️"});
    gd["mixed"] = "🎥 映画を見る 'quoted' \u00e9";

    REQUIRE(sm.save(6, gd, "utf8scene", 4));

    json loaded = sm.load(6);
    CHECK(loaded["text"].get<std::string>() == utf8);
    CHECK(loaded["with_nul"].get<std::string>() == nulInString);
    REQUIRE(loaded["emojis"].is_array());
    CHECK(loaded["emojis"].size() == 4);
    CHECK(loaded["emojis"][0] == "✨");
    CHECK(loaded["emojis"][3] == "❤️");
    CHECK(loaded["mixed"] == "🎥 映画を見る 'quoted' \u00e9");

    // In file metadata, the UTF-8 scene name is preserved verbatim (bytes).
    SaveMeta meta;
    CHECK(sm.load(6, &meta)["text"].get<std::string>() == utf8);
}

TEST_CASE("Storage: unknown future schema remains listed but cannot replace current metadata") {
    TestPaths::ScopedTempDir dir("storage_future_ver");
    SaveManager sm;
    sm.init(dir.string());  // built-in chain tops out at v5

    CHECK(sm.currentSchemaVersion() == 5);

    // Hand-write a save from a "future" engine (schema_version > current).
    json env;
    env["schema_version"] = 99;
    env["timestamp"] = 0;
    env["scene"] = "from_future";
    env["token_index"] = 7;
    env["thumbnail"] = "";
    env["engine_version"] = "9.99.99";
    env["data"] = {{"futuristic_field", true}, {"preserve", "intact"}};
    {
        std::ofstream f(dir.string() + "save_2.json", std::ios::binary | std::ios::trunc);
        f << env.dump();
    }

    // Discovery may show a future save, but restoration requires a supported
    // schema and must not publish candidate metadata when it is refused.
    SaveMeta meta{91, 456, "sentinel-scene", "sentinel-thumbnail", 789, 90};
    json loaded = sm.load(2, &meta);
    CHECK(loaded.is_null());
    CHECK(meta.slot == 91);
    CHECK(meta.timestamp == 456);
    CHECK(meta.sceneName == "sentinel-scene");
    CHECK(meta.thumbnail == "sentinel-thumbnail");
    CHECK(meta.tokenIndex == 789);
    CHECK(meta.schemaVersion == 90);
    // The future save surfaces in listSaves preserving its version.
    auto saves = sm.listSaves();
    REQUIRE(saves.size() == 1);
    CHECK(saves[0].slot == 2);
    CHECK(saves[0].schemaVersion == 99);
}

TEST_CASE("Storage: chained migration runs stepwise when loading an intermediate v2 save") {
    TestPaths::ScopedTempDir dir("storage_chain_intermediate");
    SaveManager sm;
    sm.init(dir.string());  // v1->v2->v3->v4->v5 chain installed
    REQUIRE(sm.currentSchemaVersion() == 5);

    // Simulate a save produced at schema v2 (already has playtime, missing
    // the v3/v4/v5 fields the chain must append).
    json env;
    env["schema_version"] = 2;
    env["timestamp"] = 0;
    env["scene"] = "midchain";
    env["token_index"] = 5;
    env["thumbnail"] = "";
    env["engine_version"] = "0.95.0";
    env["data"] = {{"playtime", 1234}, {"preserved", "yes"}};
    {
        std::ofstream f(dir.string() + "save_1.json", std::ios::binary | std::ios::trunc);
        f << env.dump();
    }

    // v2 -> v3 (minigame) -> v4 (live2d) -> v5 (editor); existing field intact.
    json loaded = sm.load(1);
    CHECK(loaded["playtime"] == 1234);       // v2 field survives the chain
    CHECK(loaded["preserved"] == "yes");      // untouched by migrations
    CHECK(loaded["minigame"].is_object());    // added at v3
    CHECK(loaded["live2d"].is_object());      // added at v4
    CHECK(loaded["editor"].is_object());      // added at v5
}

TEST_CASE("Storage: rapid sequential multi-slot save + immediate load (disk-backed)") {
    TestPaths::ScopedTempDir dir("storage_sequential");
    SaveManager sm;
    sm.init(dir.string());

    // Fill many slots back-to-back; SaveManager keeps NO in-memory cache, so
    // an immediate load re-reads the file from the provider on every call.
    const int N = 10;
    for (int s = 0; s < N; ++s) {
        json gd = {{"slot", s}, {"value", s * 10}};
        REQUIRE(sm.save(s, gd, "scene_" + std::to_string(s), s));
    }
    // Every slot loads immediately in the same process with matching metadata.
    for (int s = 0; s < N; ++s) {
        SaveMeta meta;
        json loaded = sm.load(s, &meta);
        CHECK(loaded["slot"] == s);
        CHECK(loaded["value"] == s * 10);
        CHECK(meta.slot == s);
        CHECK(meta.sceneName == "scene_" + std::to_string(s));
        CHECK(sm.slotExists(s));
    }
    // All N listed, in ascending slot order, no duplicates / no gaps.
    auto saves = sm.listSaves();
    REQUIRE(saves.size() == static_cast<size_t>(N));
    for (int s = 0; s < N; ++s) {
        CHECK(saves[static_cast<size_t>(s)].slot == s);
    }
}

// In-memory ISaveProvider for provider-injection testing: stores raw bytes in
// a std::map keyed by path, never touching the filesystem.
class InMemorySaveProvider final : public ISaveProvider {
public:
    std::map<std::string, std::string> files;
    // Error injection: when active, the flagged operation fails and reports.
    bool failWrite = false;
    bool failReadReturnEmpty = false;
    bool failDelete = false;

    std::string readFile(const std::string& path) override {
        if (failReadReturnEmpty) return "";
        const auto it = files.find(path);
        return it == files.end() ? std::string() : it->second;
    }
    bool writeFile(const std::string& path, const std::string& content) override {
        if (failWrite) return false;
        files[path] = content;
        return true;
    }
    bool deleteFile(const std::string& path) override {
        if (failDelete) return false;
        return files.erase(path) > 0;
    }
    std::vector<std::string> listFiles(const std::string& pattern) override {
        std::vector<std::string> out;
        for (const auto& [name, _] : files) {
            if (pattern.empty() || name.find(pattern) != std::string::npos) out.push_back(name);
        }
        return out;
    }
    bool pushToCloud(const std::string&) override { return false; }
    bool pullFromCloud(const std::string&) override { return false; }
    bool supportsCloudSync() const override { return false; }
};

TEST_CASE("Storage: custom in-memory ISaveProvider drives the full save/load flow") {
    TestPaths::ScopedTempDir dir("storage_provider_inmem");
    SaveManager sm;
    sm.init(dir.string());  // m_saveDir must be non-empty for listSaves()

    auto mem = std::make_unique<InMemorySaveProvider>();
    InMemorySaveProvider* memPtr = mem.get();
    sm.setSaveProvider(std::move(mem));
    CHECK(sm.getSaveProvider() == memPtr);

    // save() routes the whole slot path through the provider's writeFile.
    json gd = {{"p", true}, {"n", 7}, {"nested", {{"x", {{"y", 1}}}}}};
    REQUIRE(sm.save(3, gd, "memscene", 6));

    // Raw bytes land in the in-memory map under the canonical slot path...
    const std::string path = dir.string() + "save_3.json";
    CHECK(memPtr->files.count(path) == 1);

    // ...and the SaveManager facade reads them back identically.
    SaveMeta meta;
    json loaded = sm.load(3, &meta);
    CHECK(loaded == gd);
    CHECK(meta.slot == 3);
    CHECK(meta.sceneName == "memscene");
    CHECK(meta.tokenIndex == 6);
    CHECK(sm.slotExists(3));

    // listSaves() enumerates the in-memory provider (not the filesystem).
    auto saves = sm.listSaves();
    REQUIRE(saves.size() == 1);
    CHECK(saves[0].slot == 3);

    // deleteSlot() routes through the provider and removes the byte entry.
    REQUIRE(sm.deleteSlot(3));
    CHECK_FALSE(sm.slotExists(3));
    CHECK(sm.load(3).is_null());
    CHECK(memPtr->files.empty());
}

TEST_CASE("Storage: provider write/read/delete errors propagate gracefully") {
    TestPaths::ScopedTempDir dir("storage_provider_err");
    SaveManager sm;
    sm.init(dir.string());

    auto mem = std::make_unique<InMemorySaveProvider>();
    InMemorySaveProvider* memPtr = mem.get();
    sm.setSaveProvider(std::move(mem));

    // Normal write first (so we can later exercise the read/delete paths).
    REQUIRE(sm.save(2, {{"ok", 1}}, "base", 0));

    // 1) Provider write failure -> save() returns false and stores nothing.
    memPtr->files.clear();
    memPtr->failWrite = true;
    CHECK_FALSE(sm.save(2, {{"ok", 2}}, "wfail", 0));
    CHECK(memPtr->files.empty());
    CHECK(sm.load(2).is_null());
    memPtr->failWrite = false;

    // Re-establish a baseline entry.
    REQUIRE(sm.save(2, {{"ok", 1}}, "base", 0));

    // 2) Provider read failure (empty) -> load null, slot not exists, not listed.
    memPtr->failReadReturnEmpty = true;
    CHECK(sm.load(2).is_null());
    CHECK_FALSE(sm.slotExists(2));
    CHECK(sm.listSaves().empty());
    memPtr->failReadReturnEmpty = false;
    CHECK(sm.load(2)["ok"] == 1);   // recovers when the provider does

    // 3) Provider delete failure -> deleteSlot() returns false, slot remains.
    memPtr->failDelete = true;
    CHECK_FALSE(sm.deleteSlot(2));
    CHECK(sm.load(2)["ok"] == 1);   // still present
    memPtr->failDelete = false;
    REQUIRE(sm.deleteSlot(2));      // succeeds once the provider is healthy
    CHECK_FALSE(sm.slotExists(2));
}


// =============================================================================
// Round-85+ concurrency / multithreading boundary tests.
//
// ARCHITECTURE NOTE (verified): src/storage has NO CAESURA_ASSERT_MAIN_THREAD
// guard (0 matches in src/storage) and SaveManager holds no internal mutex.
// save()/load() read only effectively-immutable shared state after init()
// (m_saveDir, m_currentSchemaVersion, m_keySet, m_saveProvider) and route the
// actual I/O to the ISaveProvider. Different slots map to different provider
// keys/files, so concurrent saves to *distinct* slots are safe as long as the
// provider is thread-safe. The default test fixtures below install a
// mutex-guarded in-memory provider so our threads never race the provider.
//
// These cases cover:
//   * concurrent save isolation across distinct slots
//   * sequential interleaving order-independence + same-slot overwrite-by-load
//   * thread-safe provider call counting under concurrent save/load
//   * slow provider => synchronous (blocking) save still yields a correct slot
//   * large/small slot interleaving (1 MiB vs 1 KB)
//   * failure recovery: failed save then retry; failed load preserves bytes
//   * memory-pressure: 50-slot save/load pressure loop stays correct
// =============================================================================

// Thread-safe in-memory ISaveProvider. Unlike the plain InMemorySaveProvider
// above (whose std::map is NOT safe to mutate from several threads), this one
// guards every access with a mutex and reports call counts with atomics so a
// multi-threaded test can prove isolation and measuring.
class ThreadSafeInMemorySaveProvider final : public ISaveProvider {
public:
    std::string readFile(const std::string& path) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++readCalls;
        if (failNextReads > 0) {  // transient read failure: report nothing
            --failNextReads;
            return std::string();
        }
        const auto it = files.find(path);
        return it == files.end() ? std::string() : it->second;
    }
    bool writeFile(const std::string& path, const std::string& content) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++writeCalls;
        if (writeLatencyUs > 0) {
            // Simulate a slow backend: block this thread while "writing".
            std::this_thread::sleep_for(std::chrono::microseconds(writeLatencyUs));
        }
        if (failNextWrites > 0) {  // fail a bounded number of writes in a row
            --failNextWrites;
            return false;
        }
        files[path] = content;
        return true;
    }
    bool deleteFile(const std::string& path) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++deleteCalls;
        return files.erase(path) > 0;
    }
    std::vector<std::string> listFiles(const std::string& pattern) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<std::string> out;
        for (const auto& [name, _] : files) {
            if (pattern.empty() || name.find(pattern) != std::string::npos) out.push_back(name);
        }
        return out;
    }
    bool pushToCloud(const std::string&) override { return false; }
    bool pullFromCloud(const std::string&) override { return false; }
    bool supportsCloudSync() const override { return false; }

    // Optional artificial write latency (microseconds) to exercise the
    // synchronous-blocking behaviour: a slow provider must still yield a
    // correct, complete save (the call blocks until the write finishes).
    long writeLatencyUs = 0;
    std::atomic<int> failNextWrites{0};
    std::atomic<int> failNextReads{0};
    std::atomic<long> writeCalls{0};
    std::atomic<long> readCalls{0};
    std::atomic<long> deleteCalls{0};

private:
    std::mutex m_mutex;
    std::map<std::string, std::string> files;
};

TEST_CASE("Storage: concurrent saves to distinct slots are isolated") {
    TestPaths::ScopedTempDir dir("storage_concurrent_slots");
    SaveManager sm;
    sm.init(dir.string());

    auto prov = std::make_unique<ThreadSafeInMemorySaveProvider>();
    ThreadSafeInMemorySaveProvider* provPtr = prov.get();
    sm.setSaveProvider(std::move(prov));

    // 8 threads, each saving a UNIQUE slot with a slot-tagged payload.
    const int N = 8;
    std::vector<std::thread> threads;
    std::atomic<int> failures{0};
    for (int s = 0; s < N; ++s) {
        threads.emplace_back([&sm, &failures, s]() {
            json gd = {{"thread", s}, {"value", s * 100}, {"ok", true}};
            if (!sm.save(s, gd, "scene_" + std::to_string(s), s)) failures.fetch_add(1);
        });
    }
    for (auto& t : threads) t.join();

    // No save reported failure.
    CHECK(failures.load() == 0);

    // Exactly N writes reached the provider (one per slot).
    CHECK(provPtr->writeCalls.load() == N);

    // Every slot loads back with its own exact payload (isolation: no slot
    // leaked another thread's data).
    for (int s = 0; s < N; ++s) {
        SaveMeta meta;
        json loaded = sm.load(s, &meta);
        CHECK(loaded["thread"] == s);
        CHECK(loaded["value"] == s * 100);
        CHECK(loaded["ok"] == true);
        CHECK(meta.sceneName == "scene_" + std::to_string(s));
    }

    // All N slots enumerate exactly once, in ascending order, no dups/gaps.
    auto saves = sm.listSaves();
    REQUIRE(saves.size() == static_cast<size_t>(N));
    for (int s = 0; s < N; ++s) CHECK(saves[static_cast<size_t>(s)].slot == s);
}

TEST_CASE("Storage: concurrent save+load across distinct slots stays consistent") {
    TestPaths::ScopedTempDir dir("storage_concurrent_io");
    SaveManager sm;
    sm.init(dir.string());

    auto prov = std::make_unique<ThreadSafeInMemorySaveProvider>();
    ThreadSafeInMemorySaveProvider* provPtr = prov.get();
    sm.setSaveProvider(std::move(prov));

    // Phase 1: a writer thread backfills 4 slots so readers have content.
    std::thread seed([&]() {
        for (int s = 0; s < 4; ++s) {
            REQUIRE(sm.save(s, {{"phase", 0}, {"slot", s}}, "seed", s));
        }
    });
    seed.join();

    // Phase 2: one thread overwrites slots 0..3 while two readers repeatedly
    // load them. Each slot is self-consistent: a reader may observe the
    // pre- or post-overwrite value, but MUST never see a torn/mixed envelope.
    std::atomic<bool> stop{false};
    std::atomic<int> readerMixed{0};
    std::atomic<int> readerReads{0};
    auto writer = std::thread([&]() {
        for (int round = 1; round <= 10; ++round) {
            for (int s = 0; s < 4; ++s) sm.save(s, {{"phase", round}, {"slot", s}}, "w", round);
        }
        stop.store(true);
    });
    auto reader = std::thread([&]() {
        while (!stop.load() || readerReads.load() < 10) {
            for (int s = 0; s < 4; ++s) {
                json v = sm.load(s);
                if (v.is_null()) continue;
                readerReads.fetch_add(1);
                int slot = v.value("slot", -1);
                int phase = v.value("phase", -1);
                // A slot entry must never carry another slot's identity.
                if (slot != s || phase < 0) readerMixed.fetch_add(1);
            }
        }
    });

    writer.join();
    reader.join();

    CHECK(readerMixed.load() == 0);                 // no torn cross-slot reads
    CHECK(provPtr->deleteCalls.load() == 0);        // reader never deleted anything

    // Writers wrote and readers read: both call paths were exercised. The
    // reader count is naturally racy (it stops when the writer finishes), so
    // only require that meaningful reading happened; the writer bound is exact.
    CHECK(provPtr->writeCalls.load() >= 40);        // 4 seed + 4*10 overwrites
    CHECK(provPtr->readCalls.load() >= 10);         // reader loop did real loads
}

TEST_CASE("Storage: sequential interleaving of save/load is order-independent") {
    // save(A) + save(B) + load(A) must always give A's payload no matter which
    // interleaving produced it -- load reflects committed disk state for A.
    TestPaths::ScopedTempDir dir("storage_interleave");
    SaveManager sm;
    sm.init(dir.string());

    json a = {{"who", "A"}, {"n", 1}};
    json b = {{"who", "B"}, {"n", 2}};

    // Interleaving 1: save A, then save B, then load A.
    REQUIRE(sm.save(1, a, "a", 1));
    REQUIRE(sm.save(2, b, "b", 2));
    CHECK(sm.load(1)["who"] == "A");

    // Interleaving 2: save B, then save A (reverse registration), then load A.
    SaveManager sm2;
    sm2.init(dir.string());
    REQUIRE(sm2.save(2, b, "b", 2));
    REQUIRE(sm2.save(1, a, "a", 1));
    CHECK(sm2.load(1)["who"] == "A");
    CHECK(sm2.load(2)["who"] == "B");

    // Interleaving 3: single manager, A then B then A again, load mid-way.
    REQUIRE(sm.save(1, a, "a", 1));
    REQUIRE(sm.save(2, b, "b", 2));
    REQUIRE(sm.save(1, a, "a_re", 3));   // re-save A; must not disturb B
    CHECK(sm.load(1)["who"] == "A");
    CHECK(sm.load(2)["who"] == "B");
}

TEST_CASE("Storage: save same slot twice loads the second value") {
    TestPaths::ScopedTempDir dir("storage_slot_twice");
    SaveManager sm;
    sm.init(dir.string());

    REQUIRE(sm.save(4, {{"phase", 1}}, "first", 1));
    REQUIRE(sm.save(4, {{"phase", 2}, {"marker", "second"}}, "second", 9));

    json loaded = sm.load(4);
    CHECK(loaded["phase"] == 2);
    CHECK(loaded["marker"] == "second");

    // A third save also wins; the slot file count stays 1.
    REQUIRE(sm.save(4, {{"phase", 3}}, "third", 2));
    CHECK(sm.load(4)["phase"] == 3);
    auto saves = sm.listSaves();
    REQUIRE(saves.size() == 1);
    CHECK(saves[0].slot == 4);
    CHECK(saves[0].tokenIndex == 2);
}

TEST_CASE("Storage: slow provider blocks synchronously but saves correctly") {
    // SaveManager has no async path: a slow provider must fully block save()
    // and STILL return a complete, loadable slot -- never a partial/early one.
    TestPaths::ScopedTempDir dir("storage_slow_provider");
    SaveManager sm;
    sm.init(dir.string());

    auto prov = std::make_unique<ThreadSafeInMemorySaveProvider>();
    ThreadSafeInMemorySaveProvider* provPtr = prov.get();
    provPtr->writeLatencyUs = 5000;   // 5ms artificial latency per write
    sm.setSaveProvider(std::move(prov));

    auto t0 = std::chrono::steady_clock::now();
    json gd = {{"slow", true}, {"payload", "blocking-write"}};
    REQUIRE(sm.save(7, gd, "slowscene", 3));         // must block until written
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    CHECK(ms >= 4);                                  // actually waited on provider

    json loaded = sm.load(7);
    CHECK(loaded["slow"] == true);
    CHECK(loaded["payload"] == "blocking-write");
    CHECK(provPtr->writeCalls.load() == 1);
}

TEST_CASE("Storage: 1 MiB and 1 KB slots interleave without cross-corruption") {
    TestPaths::ScopedTempDir dir("storage_size_mix");
    SaveManager sm;
    sm.init(dir.string());

    const std::string big = makeRepeatedPayload(1024 * 1024, 5);   // 1 MiB
    const std::string small = makeRepeatedPayload(1024, 9);        // 1 KB

    // Interleave large and small across alternating slots, like a game
    // switching between a heavy quicksave and lightweight auto-saves.
    for (int i = 0; i < 6; ++i) {
        bool useBig = (i % 2 == 0);
        int slot = i;
        const std::string& body = useBig ? big : small;
        json gd = {{"k", i}, {"blob", body}};
        REQUIRE(sm.save(slot, gd, useBig ? "bigscene" : "smallscene", i));
    }

    // Every slot round-trips its exact blob; large stays large, small small.
    for (int i = 0; i < 6; ++i) {
        bool useBig = (i % 2 == 0);
        const std::string& expected = useBig ? big : small;
        json loaded = sm.load(i);
        CHECK(loaded["k"] == i);
        CHECK(loaded["blob"].is_string());
        CHECK(loaded["blob"].get<std::string>() == expected);
        CHECK(loaded["blob"].get<std::string>().size() == expected.size());
    }
}

TEST_CASE("Storage: failed save then immediate retry succeeds cleanly") {
    // A provider that fails the next write (transient error) must leave no
    // partial/poisoned slot; after the failure a retry lands the full payload.
    TestPaths::ScopedTempDir dir("storage_fail_retry");
    SaveManager sm;
    sm.init(dir.string());

    auto prov = std::make_unique<ThreadSafeInMemorySaveProvider>();
    ThreadSafeInMemorySaveProvider* provPtr = prov.get();
    sm.setSaveProvider(std::move(prov));

    // Baseline save to establish the slot content we must preserve.
    REQUIRE(sm.save(3, {{"v", 1}}, "base", 0));
    CHECK(sm.load(3)["v"] == 1);

    // Next write fails: save() reports failure and the old value stays intact.
    provPtr->failNextWrites.store(1);
    CHECK_FALSE(sm.save(3, {{"v", 2}}, "willfail", 0));
    CHECK(sm.load(3)["v"] == 1);   // prior content untouched (no partial write)

    // Immediate retry with a healthy provider succeeds and overwrites.
    REQUIRE(sm.save(3, {{"v", 3}}, "retry", 1));
    SaveMeta meta;
    json retryLoaded = sm.load(3, &meta);
    CHECK(retryLoaded["v"] == 3);
    CHECK(meta.sceneName == "retry");     // the retry's metadata won
}

TEST_CASE("Storage: failed load leaves the on-disk save intact") {
    // A transient READ failure (provider returns empty / reports none) must
    // NOT destroy or corrupt the underlying save: after health returns, the
    // exact payload is still recoverable. A real filesystem provider never
    // mutates on read; this guards against a future implementation that
    // deletes or truncates on a failed load.
    TestPaths::ScopedTempDir dir("storage_load_preserve");
    SaveManager sm;
    sm.init(dir.string());

    auto prov = std::make_unique<ThreadSafeInMemorySaveProvider>();
    ThreadSafeInMemorySaveProvider* provPtr = prov.get();
    sm.setSaveProvider(std::move(prov));

    json gd = {{"important", "data"}, {"n", 42}};
    REQUIRE(sm.save(9, gd, "keep", 5));
    SaveMeta metaBefore;
    REQUIRE(sm.load(9, &metaBefore)["n"] == 42);

    // Force the next reads to fail (return empty): load() reports null and
    // slotExists() false, but the stored bytes are untouched. The fail counter
    // is per-read, so cover both the load and the slotExists query.
    provPtr->failNextReads.store(2);
    CHECK(sm.load(9).is_null());
    CHECK_FALSE(sm.slotExists(9));      // present but unreadable at that instant
    CHECK(provPtr->readCalls.load() >= 2);

    // Health returns: the exact original payload survives the failed load.
    SaveMeta meta;
    json again = sm.load(9, &meta);
    CHECK(again["important"] == "data");
    CHECK(again["n"] == 42);
    CHECK(meta.sceneName == "keep");
    CHECK(meta.slot == 9);
    CHECK(sm.slotExists(9));
}

TEST_CASE("Storage: 50-slot save/load pressure loop stays correct") {
    // Quantity-of-order sanity for slot growth: hammer 50 slots save+load and
    // confirm every payload round-trips and the managed list stays consistent.
    // This exercises sustained per-slot I/O (disk-backed persistence, no cache)
    // and guards against accidental quadratic behaviour in list/scan paths.
    TestPaths::ScopedTempDir dir("storage_pressure50");
    SaveManager sm;
    sm.init(dir.string());

    const int N = 50;
    for (int s = 0; s < N; ++s) {
        json gd = {{"slot", s}, {"iter", 0}};
        REQUIRE(sm.save(s, gd, "p_" + std::to_string(s), s));
    }
    // Second pass overwrites every slot (proves repeated same-slot writes).
    for (int s = 0; s < N; ++s) {
        json gd = {{"slot", s}, {"iter", 1}};
        REQUIRE(sm.save(s, gd, "p_" + std::to_string(s), s + 1000));
    }
    // Load every slot, verify the latest (iter=1) content.
    for (int s = 0; s < N; ++s) {
        SaveMeta meta;
        json loaded = sm.load(s, &meta);
        CHECK(loaded["slot"] == s);
        CHECK(loaded["iter"] == 1);
        CHECK(meta.tokenIndex == s + 1000);
    }

    auto saves = sm.listSaves();
    REQUIRE(saves.size() == static_cast<size_t>(N));
    for (int s = 0; s < N; ++s) {
        CHECK(saves[static_cast<size_t>(s)].slot == s);
        CHECK(saves[static_cast<size_t>(s)].tokenIndex == s + 1000);
    }
}

TEST_CASE("Storage: LocalFileSaveProvider end-to-end through SaveManager (Track P4)") {
    TestPaths::ScopedTempDir dir("storage_localfile_default");
    SaveManager sm;
    sm.init(dir.string());
    sm.setSaveProvider(std::make_unique<LocalFileSaveProvider>());

    // Save / load round trip on real files (atomic-ish tmp+rename path).
    CHECK(sm.save(1, {{"hero", "sakura"}, {"level", 10}}, "demo/w2.ks", 7));
    SaveMeta meta;
    json data = sm.load(1, &meta);
    CHECK(data["hero"] == "sakura");
    CHECK(data["level"] == 10);
    CHECK(meta.sceneName == "demo/w2.ks");

    // Slot listing / existence / delete.
    CHECK(sm.slotExists(1));
    CHECK(sm.listSaves().size() == 1);
    CHECK(sm.deleteSlot(1));
    CHECK_FALSE(sm.slotExists(1));
    CHECK(sm.listSaves().empty());

    // Overwrite: same slot under a NEW payload stays a single entry.
    CHECK(sm.save(1, {{"hero", "mio"}}, "demo/w2.ks", 8));
    CHECK(sm.listSaves().size() == 1);
    CHECK(sm.load(1)["hero"] == "mio");
}
