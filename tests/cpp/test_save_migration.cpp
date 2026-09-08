#include "doctest.h"
#include "storage/SaveManager.h"
#include "TestPaths.h"
#include <filesystem>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>

using namespace Caesura;

namespace {
std::string writeMigrationEnvelope(const std::filesystem::path& path,
                                  const json& version, const json& data) {
    // Synthetic bytes exercise the actual file/parse/migration path. They are
    // deliberately not represented as a historical engine-produced save.
    const json envelope = {{"schema_version", version}, {"timestamp", 123},
        {"scene", "candidate-scene"}, {"token_index", 2},
        {"thumbnail", "candidate-thumbnail"}, {"data", data}};
    const std::string bytes = envelope.dump();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(out.good());
    return bytes;
}

void checkRejectedMigration(SaveManager& saves, const std::filesystem::path& path,
                            const std::string& original, bool legacy = false) {
    SaveMeta meta{91, 456, "sentinel-scene", "sentinel-thumbnail", 789, 90};
    const json loaded = legacy ? saves.loadLegacyPlaintext(1, &meta) : saves.load(1, &meta);
    CHECK(loaded.is_null());
    CHECK(meta.slot == 91);
    CHECK(meta.timestamp == 456);
    CHECK(meta.sceneName == "sentinel-scene");
    CHECK(meta.thumbnail == "sentinel-thumbnail");
    CHECK(meta.tokenIndex == 789);
    CHECK(meta.schemaVersion == 90);
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.is_open());
    const std::string after((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    CHECK(after == original);
}
} // namespace

TEST_CASE("U11: unsupported future envelope schemas reject without publishing metadata") {
    TestPaths::ScopedTempDir dir("u11_future_schema");
    SaveManager saves;
    saves.init(dir.string());
    json version = saves.currentSchemaVersion() + 1;
    SUBCASE("next unknown schema") {}
    SUBCASE("unknown schema must not wrap through int conversion") {
        version = (std::numeric_limits<uint64_t>::max)();
    }
    const auto path = dir.path() / "save_1.json";
    const std::string bytes = writeMigrationEnvelope(path, version, {{"original", true}});
    checkRejectedMigration(saves, path, bytes);
    checkRejectedMigration(saves, path, bytes, true);
}

TEST_CASE("U11: an incomplete migration chain never reports the newest schema") {
    TestPaths::ScopedTempDir dir("u11_migration_gap");
    SaveManager saves;
    saves.init(dir.string());
    const int previousHead = saves.currentSchemaVersion();
    unsigned finalStepCalls = 0;
    // Advertise a newer schema while deliberately leaving its first link absent.
    saves.registerMigration(previousHead + 1, previousHead + 2, [&](json data) {
        ++finalStepCalls;
        data["final_step"] = true;
        return data;
    });
    const json original = {{"original", true}};
    const auto path = dir.path() / "save_1.json";
    const std::string bytes = writeMigrationEnvelope(path, 1, original);
    CHECK(saves.migrate(original, 1).is_null());
    checkRejectedMigration(saves, path, bytes);
    CHECK(finalStepCalls == 0);

    saves.registerMigration(previousHead, previousHead + 1, [](json data) {
        data["missing_link"] = true;
        return data;
    });
    SaveMeta meta;
    const json restored = saves.load(1, &meta);
    REQUIRE(restored.is_object());
    CHECK(restored["original"] == true);
    CHECK(restored["missing_link"] == true);
    CHECK(restored["final_step"] == true);
    CHECK(finalStepCalls == 1);
    CHECK(meta.schemaVersion == previousHead + 2);
}

TEST_CASE("U11: the migration step budget rejects partial results at its boundary") {
    TestPaths::ScopedTempDir dir("u11_migration_budget");
    SaveManager saves;
    saves.init(dir.string());
    // v1 -> v66 needs 65 links; v2 -> v66 needs exactly the supported 64.
    for (int version = saves.currentSchemaVersion(); version < 66; ++version) {
        saves.registerMigration(version, version + 1, [](json data) { return data; });
    }
    const auto path = dir.path() / "save_1.json";
    const json original = {{"original", true}};
    const std::string bytes = writeMigrationEnvelope(path, 1, original);
    checkRejectedMigration(saves, path, bytes);
    writeMigrationEnvelope(path, 2, original);
    SaveMeta meta;
    const json restored = saves.load(1, &meta);
    REQUIRE(restored.is_object());
    CHECK(restored["original"] == true);
    CHECK(meta.schemaVersion == 66);
}

TEST_CASE("SaveManager::migration chain — v1 to v2 adds playtime") {
    TestPaths::ScopedTempDir dir("migration_chain");
    SaveManager sm;
    sm.init(dir.string());

    json data = {{"val", 1}};
    CHECK(sm.save(1, data, "s1", 0));

    json loaded = sm.load(1);
    CHECK_FALSE(loaded.empty());
    // v1->v2 migration adds playtime field
    CHECK(loaded.contains("val"));
    CHECK(loaded["val"] == 1);
}

TEST_CASE("SaveManager::json nil round-trip") {
    TestPaths::ScopedTempDir dir("migration_nil");
    SaveManager sm;
    sm.init(dir.string());

    json data = {{"nil_val", nullptr}, {"str_val", "hello"}};
    sm.save(1, data, "nil_test", 0);

    json loaded = sm.load(1);
    CHECK(loaded["nil_val"] == nullptr);
    CHECK(loaded["str_val"] == "hello");
}

TEST_CASE("SaveManager::schema version is tracked") {
    TestPaths::ScopedTempDir dir("migration_schema");
    SaveManager sm;
    sm.init(dir.string());
    CHECK(sm.currentSchemaVersion() >= 2);
}

TEST_CASE("SaveManager::JSON parse error returns empty") {
    TestPaths::ScopedTempDir dir("migration_parse_error");
    SaveManager sm;
    sm.init(dir.string());

    // Write invalid JSON manually
    std::ofstream out(dir.path() / "save_0.json");
    out << "this is not json {{{";
    out.close();

    json data = sm.load(0);
    CHECK(data.empty());
}

TEST_CASE("SaveManager stores supplied thumbnails headlessly without touching stale screenshot files") {
    TestPaths::ScopedTempDir dir("headless_thumbnail");
    // SaveManager no longer owns readiness or screenshot requests. Its real
    // headless persistence path must leave any old screenshot bytes alone.
    const auto stale = dir.path() / "save_thumb.png";
    { std::ofstream out(stale, std::ios::binary); out << "old-screenshot-sentinel"; }
    SaveManager saves;
    saves.init(dir.string());
    REQUIRE(saves.save(1, {{"route", "without-image"}}, "headless.ks", 7));
    SaveMeta meta;
    CHECK(saves.load(1, &meta).at("route") == "without-image");
    CHECK(meta.thumbnail.empty());
    REQUIRE(saves.save(2, {{"route", "with-image"}}, "headless.ks", 8, "provided-base64"));
    CHECK(saves.load(2, &meta).at("route") == "with-image");
    CHECK(meta.thumbnail == "provided-base64");
    std::ifstream input(stale, std::ios::binary);
    REQUIRE(input.good());
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    CHECK(bytes == "old-screenshot-sentinel");
}
