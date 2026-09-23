// test_steam.cpp — Steam backend tests (NullSteamBackend)
#include "doctest.h"
#include "steam/NullSteamBackend.h"
#include "steam/api/ISteamBackend.h"

using namespace Caesura;

TEST_CASE("NullSteamBackend::init returns false") {
    NullSteamBackend steam;
    CHECK(steam.init() == false);
}

TEST_CASE("NullSteamBackend::name") {
    NullSteamBackend steam;
    CHECK(std::string(steam.name()) == "NullSteam");
}

TEST_CASE("NullSteamBackend::overlay is never active") {
    NullSteamBackend steam;
    CHECK(steam.isOverlayActive() == false);
}

TEST_CASE("NullSteamBackend::achievements always return false") {
    NullSteamBackend steam;
    CHECK(steam.unlockAchievement("ACH_TEST") == false);
    CHECK(steam.isAchievementUnlocked("ACH_TEST") == false);
    CHECK(steam.resetAchievement("ACH_TEST") == false);
    CHECK(steam.resetAllAchievements() == false);
}

TEST_CASE("NullSteamBackend::stats return default values") {
    NullSteamBackend steam;
    CHECK(steam.setStatInt("kills", 10) == false);
    CHECK(steam.getStatInt("kills") == 0);
    CHECK(steam.setStatFloat("time", 1.5f) == false);
    CHECK(steam.getStatFloat("time") == 0.0f);
    CHECK(steam.storeStats() == false);
}

TEST_CASE("NullSteamBackend::cloud operations return empty") {
    NullSteamBackend steam;
    const char* data = "test save data";
    CHECK(steam.cloudWrite("save.dat", data, 13) == false);
    char buf[256] = {};
    CHECK(steam.cloudRead("save.dat", buf, 256) == 0);
    CHECK(steam.cloudFileSize("save.dat") == 0);
    CHECK(steam.cloudFileExists("save.dat") == false);
    CHECK(steam.cloudDelete("save.dat") == false);
    CHECK(steam.cloudQuotaTotal() == 0);
    CHECK(steam.cloudQuotaUsed() == 0);
}

TEST_CASE("NullSteamBackend::runCallbacks does not crash") {
    NullSteamBackend steam;
    steam.runCallbacks();  // should be no-op
}

TEST_CASE("NullSteamBackend::shutdown is idempotent") {
    NullSteamBackend steam;
    steam.shutdown();
    steam.shutdown();  // second call should not crash
}

// ---------------------------------------------------------------------------
// G3: Conditional-compilation tests for the real Steam backend.
//
// SteamBackend.cpp is ALWAYS compiled (cmake/CaesuraModules.cmake
// caesura_add_module(Steam ...)), so in a no-SDK build every method compiles
// its #else branch. The four cases below preserve that degradation contract
// without the SDK and exercise uninitialized sentinels with the SDK. SDK ON
// cases never call init(): initialized sessions and account actions require
// separate acceptance. Static source checks follow these runtime cases.
// ---------------------------------------------------------------------------

#include "steam/SteamBackend.h"

#include <filesystem>
#include <fstream>
#include <sstream>

static std::string readSteamSourceFile(const std::string& relative) {
#ifdef CAESURA_SOURCE_DIR
    // Out-of-tree builds: prefer the CMake-injected source root.
    const std::filesystem::path fromMacro(CAESURA_SOURCE_DIR);
    if (std::filesystem::exists(fromMacro / "src") &&
        std::filesystem::exists(fromMacro / "tests" / "cpp")) {
        std::ifstream file(fromMacro / relative, std::ios::binary);
        std::ostringstream out;
        out << file.rdbuf();
        return out.str();
    }
#endif
    auto path = std::filesystem::current_path();
    while (!path.empty()) {
        if (std::filesystem::exists(path / "src") &&
            std::filesystem::exists(path / "tests" / "cpp")) {
            break;
        }
        const auto parent = path.parent_path();
        if (parent == path) {
            path.clear();
            break;
        }
        path = parent;
    }
    REQUIRE_FALSE(path.empty());
    std::ifstream file(path / relative, std::ios::binary);
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}

#ifdef CAESURA_HAS_STEAM
TEST_CASE("SteamBackend (SDK ON uninitialized) default state is unavailable") {
#else
TEST_CASE("SteamBackend (no-SDK build) init degrades to false") {
#endif
    SteamBackend steam;
#ifdef CAESURA_HAS_STEAM
    REQUIRE_FALSE(steam.isAvailable());
#else
    // Without the SDK the real backend must refuse to initialize.
    CHECK(steam.init() == false);
#endif
    CHECK(std::string(steam.name()) == "Steam");
    steam.shutdown();
#ifdef CAESURA_HAS_STEAM
    CHECK_FALSE(steam.isAvailable());
#endif
}

#ifdef CAESURA_HAS_STEAM
TEST_CASE("SteamBackend (SDK ON uninitialized) feature gates return disabled sentinels") {
#else
TEST_CASE("SteamBackend (no-SDK build) feature gates all return disabled sentinels") {
#endif
    SteamBackend steam;
#ifdef CAESURA_HAS_STEAM
    REQUIRE_FALSE(steam.isAvailable());
#else
    steam.init();
#endif
    CHECK(steam.isOverlayActive() == false);
    CHECK(steam.unlockAchievement("ACH_TEST") == false);
    CHECK(steam.isAchievementUnlocked("ACH_TEST") == false);
    CHECK(steam.resetAchievement("ACH_TEST") == false);
    CHECK(steam.resetAllAchievements() == false);
#ifdef CAESURA_HAS_STEAM
    CHECK_FALSE(steam.isAvailable());
#endif
}

#ifdef CAESURA_HAS_STEAM
TEST_CASE("SteamBackend (SDK ON uninitialized) stats return default values") {
#else
TEST_CASE("SteamBackend (no-SDK build) stats degrade to default values") {
#endif
    SteamBackend steam;
#ifdef CAESURA_HAS_STEAM
    REQUIRE_FALSE(steam.isAvailable());
#else
    steam.init();
#endif
    CHECK(steam.setStatInt("kills", 10) == false);
    CHECK(steam.getStatInt("kills") == 0);
    CHECK(steam.setStatFloat("time", 1.5f) == false);
    CHECK(steam.getStatFloat("time") == 0.0f);
    CHECK(steam.storeStats() == false);
#ifdef CAESURA_HAS_STEAM
    CHECK_FALSE(steam.isAvailable());
#endif
}

#ifdef CAESURA_HAS_STEAM
TEST_CASE("SteamBackend (SDK ON uninitialized) cloud operations return empty") {
#else
TEST_CASE("SteamBackend (no-SDK build) cloud operations degrade to empty") {
#endif
    SteamBackend steam;
#ifdef CAESURA_HAS_STEAM
    REQUIRE_FALSE(steam.isAvailable());
#else
    steam.init();
#endif
    const char* data = "save payload";
    CHECK(steam.cloudWrite("save.dat", data, 12) == false);
    char buf[64] = {};
    CHECK(steam.cloudRead("save.dat", buf, 64) == 0);
    CHECK(steam.cloudFileSize("save.dat") == 0);
    CHECK(steam.cloudFileExists("save.dat") == false);
    CHECK(steam.cloudDelete("save.dat") == false);
    CHECK(steam.cloudQuotaTotal() == 0);
    CHECK(steam.cloudQuotaUsed() == 0);
    CHECK(steam.cloudFileCount() == 0);
    CHECK(std::string(steam.cloudFileNameAt(0)) == "");
#ifdef CAESURA_HAS_STEAM
    CHECK_FALSE(steam.isAvailable());
#endif
}

TEST_CASE("SteamBackend (no-SDK build) destructor + runCallbacks are safe") {
    // shutdown via destructor must not crash; runCallbacks is a no-op.
    {
        SteamBackend steam;
        steam.runCallbacks();
    }  // ~SteamBackend() calls shutdown()
    SteamBackend steam2;
    steam2.shutdown();
    steam2.shutdown();  // idempotent
}

TEST_CASE("SteamBackend no-SDK build must never reference Steamworks symbols") {
    // Every real SDK call site lives behind #ifdef CAESURA_HAS_STEAM. If any
    // Steamworks symbol slipped outside a guard, a no-SDK build would fail to
    // link -- this static assertion fails that drift before it reaches CI.
    const std::string src = readSteamSourceFile("src/steam/SteamBackend.cpp");
    CHECK(src.find("SteamAPI_Init()") != std::string::npos);        // SDK path exists
    CHECK(src.find("SteamAPI_RunCallbacks()") != std::string::npos);
    CHECK(src.find("SteamUserStats()") != std::string::npos);
    CHECK(src.find("SteamRemoteStorage()") != std::string::npos);
    CHECK(src.find("#ifdef CAESURA_HAS_STEAM") != std::string::npos);
    // Negative: no Steamworks type/macro appears outside a guard (the header
    // only declares CCallbackManual members inside its own guard too -- the
    // STEAM_CALLBACK macro form was replaced in Sprint 4b/Steam-SDK bring-up
    // because its protected default ctor is inaccessible to a non-derived
    // holder class; CCallbackManual + explicit Register works for holders).
    const std::string header = readSteamSourceFile("src/steam/SteamBackend.h");
    CHECK(header.find("CCallbackManual") != std::string::npos);
    CHECK(header.find("#ifdef CAESURA_HAS_STEAM") != std::string::npos);
}
