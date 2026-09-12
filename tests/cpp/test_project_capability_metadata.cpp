#include "doctest.h"
#include "rpc/ProjectContext.h"
#include "rpc/services/ProjectService.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {
namespace metadata_fs = std::filesystem;
using MetadataJson = nlohmann::json;

// Exercise the production service against an owned package-shaped directory.
// The explicit anchor wins over CAESURA_SOURCE_DIR without changing the CWD.
class CapabilityMetadataFixture {
public:
    CapabilityMetadataFixture() : m_root(createOwnedRoot()) {}
    CapabilityMetadataFixture(const CapabilityMetadataFixture&) = delete;
    CapabilityMetadataFixture& operator=(const CapabilityMetadataFixture&) = delete;

    ~CapabilityMetadataFixture() {
        std::error_code error;
        const auto resolved = metadata_fs::weakly_canonical(m_root, error);
        if (error || resolved != m_root
            || resolved.filename().string().rfind("caesura-u19-metadata-", 0) != 0) return;
        metadata_fs::remove_all(resolved, error);
    }

    Caesura::rpc::service::ProjectService service() const {
        metadata_fs::create_directories(m_root / "tools" / "project_templates");
        metadata_fs::create_directories(m_root / "scripts");
        metadata_fs::create_directories(m_root / "demo");
        metadata_fs::create_directories(m_root / "projects" / "game");
        const auto context = Caesura::ProjectContext::fromEnvironment(m_root);
        if (context.sourceRoot() != m_root)
            throw std::runtime_error("metadata fixture did not own the ProjectContext root");
        return Caesura::rpc::service::ProjectService(context);
    }

    void write(const MetadataJson& metadata) const { writeRaw(metadata.dump(2)); }

    void writeRaw(const std::string& contents) const {
        std::ofstream output(metadataPath(), std::ios::binary | std::ios::trunc);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output << contents;
    }

    std::string readRaw() const {
        std::ifstream input(metadataPath(), std::ios::binary);
        if (!input) throw std::runtime_error("metadata fixture could not read its file");
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    MetadataJson read() const { return MetadataJson::parse(readRaw()); }

private:
    static metadata_fs::path createOwnedRoot() {
        static std::atomic<unsigned long long> serial{0};
        const auto parent = metadata_fs::canonical(metadata_fs::temp_directory_path());
        for (int attempt = 0; attempt < 32; ++attempt) {
            const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
            const auto candidate = parent / ("caesura-u19-metadata-" + std::to_string(ticks)
                + "-" + std::to_string(serial.fetch_add(1)));
            std::error_code error;
            if (metadata_fs::create_directory(candidate, error)) return candidate;
            if (error) throw metadata_fs::filesystem_error("create metadata fixture", candidate, error);
        }
        throw std::runtime_error("could not create an exclusive metadata fixture directory");
    }

    metadata_fs::path metadataPath() const {
        return m_root / "projects" / "game" / "caesura.project.json";
    }

    const metadata_fs::path m_root;
};

MetadataJson authoredCapabilities() {
    return {
        {"required", MetadataJson::array({"video.play"})},
        {"optional", MetadataJson::array({"render.postfx.lut3d"})},
        {"accept_approximate", MetadataJson::array({"render.postfx.lut3d"})},
    };
}

void checkDeclaration(const MetadataJson& metadata, const MetadataJson& expected) {
    CHECK(metadata.contains("capabilities"));
    if (metadata.contains("capabilities")) CHECK(metadata.at("capabilities") == expected);
}
}

TEST_CASE("U19 ProjectService metadata: exposes existing nested capability declarations") {
    CapabilityMetadataFixture fixture;
    auto projects = fixture.service();
    const auto declaration = authoredCapabilities();
    fixture.write({{"name", "Authored name"}, {"capabilities", declaration}});
    const auto original = fixture.readRaw();

    const auto result = projects.metaGet("projects/game");
    REQUIRE(result.status == 200);
    CHECK_FALSE(result.body.at("inferred").get<bool>());
    CHECK(result.body.at("meta").at("name") == "Authored name");
    checkDeclaration(result.body.at("meta"), declaration);
    CHECK(fixture.readRaw() == original);
}

TEST_CASE("U19 ProjectService metadata: legacy string updates preserve the disk declaration") {
    CapabilityMetadataFixture fixture;
    auto projects = fixture.service();
    const auto declaration = authoredCapabilities();
    fixture.write({{"name", "Old name"}, {"language", "en"}, {"capabilities", declaration}});

    // An old client knows only the string metadata and omits capabilities.
    const auto result = projects.metaSave("projects/game", {
        {"name", "Updated name"}, {"language", "ja"},
        {"description", "Updated description"}, {"custom_string", "still accepted"},
    });
    REQUIRE(result.status == 200);
    checkDeclaration(result.body.at("meta"), declaration);
    const auto stored = fixture.read();
    checkDeclaration(stored, declaration);
    CHECK(stored.at("name") == "Updated name");
    CHECK(stored.at("language") == "ja");
    CHECK(stored.at("description") == "Updated description");
    CHECK(stored.at("custom_string") == "still accepted");
    CHECK(stored.at("modified").is_string());

    const auto reread = projects.metaGet("projects/game");
    REQUIRE(reread.status == 200);
    checkDeclaration(reread.body.at("meta"), declaration);
}

TEST_CASE("U19 ProjectService metadata: invalid declarations remain available for CLI rejection") {
    MetadataJson declaration;
    SUBCASE("unknown feature") {
        declaration = {{"required", MetadataJson::array({"video.paly"})}};
    }
    SUBCASE("misspelled namespace key") {
        declaration = {{"requred", MetadataJson::array({"video.play"})}};
    }
    SUBCASE("wrong required field type") { declaration = {{"required", "video.play"}}; }
    SUBCASE("nested invalid requirement") {
        declaration = {{"required", MetadataJson::array({MetadataJson{{"feature", "video.play"}}})}};
    }
    SUBCASE("null declaration") { declaration = nullptr; }
    SUBCASE("array declaration") { declaration = MetadataJson::array({"video.play"}); }
    SUBCASE("string declaration") { declaration = "video.play is required"; }

    CapabilityMetadataFixture fixture;
    auto projects = fixture.service();
    fixture.write({{"name", "Invalid declaration must survive"}, {"capabilities", declaration}});
    const auto read = projects.metaGet("projects/game");
    REQUIRE(read.status == 200);
    checkDeclaration(read.body.at("meta"), declaration);

    const auto saved = projects.metaSave("projects/game", {{"description", "A normal metadata edit"}});
    REQUIRE(saved.status == 200);
    checkDeclaration(saved.body.at("meta"), declaration);
    checkDeclaration(fixture.read(), declaration);
    CHECK(fixture.read().at("description") == "A normal metadata edit");
}

TEST_CASE("U19 ProjectService metadata: old projects without declarations keep working") {
    CapabilityMetadataFixture fixture;
    auto projects = fixture.service();
    bool inferred = true;
    SUBCASE("no metadata file") {}
    SUBCASE("existing metadata without capabilities") {
        fixture.write({{"name", "Legacy"}, {"language", "en"}});
        inferred = false;
    }

    const auto read = projects.metaGet("projects/game");
    REQUIRE(read.status == 200);
    CHECK(read.body.at("inferred") == inferred);
    CHECK_FALSE(read.body.at("meta").contains("capabilities"));
    const auto saved = projects.metaSave("projects/game", {{"name", "Updated legacy"}, {"language", "zh"}});
    REQUIRE(saved.status == 200);
    CHECK_FALSE(saved.body.at("meta").contains("capabilities"));
    CHECK_FALSE(fixture.read().contains("capabilities"));
    CHECK(fixture.read().at("name") == "Updated legacy");
}

TEST_CASE("U19 ProjectService metadata: request fields cannot replace or introduce declarations") {
    bool existingDeclaration = true;
    SUBCASE("existing disk declaration wins") {}
    SUBCASE("metadata endpoint cannot introduce a declaration") { existingDeclaration = false; }
    const auto declaration = authoredCapabilities();
    const std::vector<MetadataJson> attemptedReplacements{
        MetadataJson{{"required", MetadataJson::array()}},
        MetadataJson("claim every host supports everything"),
        MetadataJson(nullptr),
    };
    for (const auto& replacement : attemptedReplacements) {
        const std::string replacementText = replacement.dump();
        CAPTURE(existingDeclaration);
        CAPTURE(replacementText);
        CapabilityMetadataFixture fixture;
        auto projects = fixture.service();
        MetadataJson original{{"name", "Original"}};
        if (existingDeclaration) original["capabilities"] = declaration;
        fixture.write(original);
        const auto result = projects.metaSave("projects/game", {
            {"description", "Only this string changes"}, {"capabilities", replacement},
        });
        REQUIRE(result.status == 200);
        const auto stored = fixture.read();
        CHECK(stored.at("description") == "Only this string changes");
        if (existingDeclaration) {
            checkDeclaration(result.body.at("meta"), declaration);
            checkDeclaration(stored, declaration);
        } else {
            CHECK_FALSE(result.body.at("meta").contains("capabilities"));
            CHECK_FALSE(stored.contains("capabilities"));
        }
    }
}

TEST_CASE("U19 ProjectService metadata: unreadable documents cannot erase pending requirements") {
    CapabilityMetadataFixture fixture;
    auto projects = fixture.service();
    std::string original;
    SUBCASE("truncated document") {
        original = R"({"name":"Old","capabilities":{"required":["video.play"]})";
    }
    SUBCASE("non-object metadata document") {
        original = R"([{"capabilities":{"required":["video.play"]}}])";
    }
    fixture.writeRaw(original);
    const auto result = projects.metaSave("projects/game", {{"description", "Do not clear requirements"}});
    CHECK(result.status >= 400);
    CHECK(fixture.readRaw() == original);
}
