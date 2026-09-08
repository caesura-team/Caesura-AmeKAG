#include "doctest.h"
#include "render/BgfxRenderDevice.h"
#include "render/NullRenderDevice.h"
#include "entry/Engine.h"
#include "entry/EngineConfig.h"
#include "di/BackendRegistry.h"
#include "script/api/ILuaManager.h"
#include "storage/api/ISaveManager.h"
#include "TestPaths.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

using namespace Caesura;

namespace {
namespace fs = std::filesystem;

#if defined(_WIN32)

constexpr char staleThumbnail[] = "U15 stale thumbnail from another renderer lifetime";

void writeBytes(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary);
    REQUIRE(output.good());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    REQUIRE(output.good());
}

std::string readBytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

constexpr wchar_t childScenarioEnv[] = L"CAESURA_U15_SCREENSHOT_CHILD";
constexpr wchar_t childDirectoryEnv[] = L"CAESURA_U15_SCREENSHOT_DIRECTORY";

std::wstring environmentValue(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) return {};
    std::wstring value(size, L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, value.data(), size);
    if (copied == 0 || copied >= size) return {};
    value.resize(copied);
    return value;
}

class ScopedChildEnvironment {
public:
    ScopedChildEnvironment(const wchar_t* name, const std::wstring& value)
        : name_(name), previous_(environmentValue(name)) {
        installed_ = SetEnvironmentVariableW(name, value.c_str()) != FALSE;
    }
    ~ScopedChildEnvironment() {
        SetEnvironmentVariableW(name_, previous_.empty() ? nullptr : previous_.c_str());
    }
    bool installed() const { return installed_; }

private:
    const wchar_t* name_;
    std::wstring previous_;
    bool installed_ = false;
};

struct ChildResult {
    DWORD createError = ERROR_SUCCESS;
    DWORD waitResult = WAIT_FAILED;
    DWORD exitCode = ERROR_PROCESS_ABORTED;
};

// Same-executable isolation follows the existing HiddenGpuContext/U7 helpers,
// without creating a window or calling bgfx::init. A regression may terminate
// this child at bgfx's assertion boundary; the doctest parent must survive.
ChildResult runScreenshotChild(const wchar_t* testName, const wchar_t* scenario,
                               const fs::path& directory) {
    ChildResult result;
    std::array<wchar_t, 32768> executable{};
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                           static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) {
        result.createError = ERROR_INSUFFICIENT_BUFFER;
        return result;
    }
    const ScopedChildEnvironment scenarioValue(childScenarioEnv, scenario);
    const ScopedChildEnvironment directoryValue(childDirectoryEnv, directory.wstring());
    if (!scenarioValue.installed() || !directoryValue.installed()) {
        result.createError = GetLastError();
        if (result.createError == ERROR_SUCCESS) result.createError = ERROR_INVALID_ENVIRONMENT;
        return result;
    }

    std::wstring command = L"\"" + std::wstring(executable.data(), length) +
        L"\" --test-case=\"" + testName + L"\" --no-version --no-colors --no-breaks";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.data(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        result.createError = GetLastError();
        return result;
    }
    CloseHandle(process.hThread);
    result.waitResult = WaitForSingleObject(process.hProcess, 30000);
    if (result.waitResult == WAIT_OBJECT_0) {
        if (!GetExitCodeProcess(process.hProcess, &result.exitCode))
            result.exitCode = ERROR_PROCESS_ABORTED;
    } else {
        // Terminate only the process created above and reap it before the
        // parent removes its own fixture. Never kill by executable name.
        TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        WaitForSingleObject(process.hProcess, 5000);
        result.exitCode = ERROR_TIMEOUT;
    }
    CloseHandle(process.hProcess);
    return result;
}

void checkChild(const ChildResult& result, const fs::path& directory) {
    CHECK_MESSAGE(result.createError == ERROR_SUCCESS,
                  "Child creation failed with Windows error ", result.createError);
    CHECK_MESSAGE(result.waitResult == WAIT_OBJECT_0,
                  "Child did not finish before its timeout; wait result ", result.waitResult);
    // This marker is written only after setup/Engine::init succeeds. Missing
    // assets or a child-launch problem must not masquerade as screenshot RED.
    CHECK_MESSAGE(fs::is_regular_file(directory / "entered-capture.txt"),
                  "Child never reached the production screenshot call");
    CHECK_MESSAGE(result.exitCode == ERROR_SUCCESS,
                  "Screenshot child exit code ", result.exitCode);
    CHECK_MESSAGE(fs::is_regular_file(directory / "returned-from-capture.txt"),
                  "Production screenshot call did not return safely");
}

fs::path childDirectory() {
    const auto value = environmentValue(childDirectoryEnv);
    REQUIRE_FALSE(value.empty());
    const fs::path directory(value);
    REQUIRE(directory.is_absolute());
    REQUIRE(fs::is_directory(directory));
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    return directory;
}

class ScopedCurrentDirectory {
public:
    explicit ScopedCurrentDirectory(const fs::path& directory) : previous_(fs::current_path()) {
        fs::current_path(directory);
    }
    ~ScopedCurrentDirectory() {
        std::error_code ignored;
        fs::current_path(previous_, ignored);
    }
private:
    fs::path previous_;
};

EngineConfig headlessConfig() {
    EngineConfig config;
    config.headless = true;
    return config;
}

void headlessCapture(bool throughLua) {
    const auto directory = childDirectory();
    Engine engine(headlessConfig());
    // Keep the normal test binary CWD for Engine script/asset initialization.
    REQUIRE(engine.init());
    auto& registry = BackendRegistry::instance();
    auto* renderer = registry.getRenderDevice();
    auto* saves = registry.getSaveManager();
    auto* vm = registry.getLuaManager();
    REQUIRE(renderer != nullptr);
    REQUIRE_FALSE(renderer->isInitialized());
    REQUIRE(saves != nullptr);
    REQUIRE(vm != nullptr);
    REQUIRE(vm->state() != nullptr);
    saves->init((directory / "saves").string());
    saves->clearEncryptionKey();

    // Only the child changes CWD, after startup. The parent owns and inspects
    // the planted file even when old code deletes it and then aborts in bgfx.
    const ScopedCurrentDirectory captureCwd(directory);
    REQUIRE(readBytes(directory / "save_thumb.png") == staleThumbnail);
    writeBytes(directory / "entered-capture.txt", "entered");
    if (throughLua) {
        vm->resetInstructionBudget();
        lua_State* state = vm->state();
        const int top = lua_gettop(state);
        const int status = luaL_dostring(state,
            "local thumbnail = KAG.capture_thumbnail(); "
            "assert(thumbnail == nil or thumbnail == '', "
            "'headless capture returned a stale thumbnail')");
        const std::string error = status != LUA_OK && lua_isstring(state, -1)
            ? lua_tostring(state, -1) : "KAG.capture_thumbnail failed";
        lua_settop(state, top);
        CHECK_MESSAGE(status == LUA_OK, error);
    } else {
        // Storage now accepts data only; capture belongs to the renderer and
        // its Lua binding. A state-only save must never inspect a stale PNG.
        const json plain{{"marker", "without-thumbnail"}};
        CHECK(saves->save(8, plain, "u15.ks", 1, ""));
        SaveMeta meta;
        CHECK(saves->load(8, &meta) == plain);
        CHECK(meta.thumbnail.empty());
    }
    writeBytes(directory / "returned-from-capture.txt", "returned");
    CHECK(readBytes(directory / "save_thumb.png") == staleThumbnail);
    // Ordinary explicit-thumbnail saves remain usable in headless mode.
    const json expected{{"marker", "headless-control"}};
    CHECK(saves->save(9, expected, "u15.ks", 1, "explicit-thumbnail"));
    SaveMeta meta;
    CHECK(saves->load(9, &meta) == expected);
    CHECK(meta.thumbnail == "explicit-thumbnail");
}

#endif
} // namespace

#if defined(_WIN32)

TEST_CASE("U15 screenshot: Bgfx rejects requests outside initialized lifetime") {
    constexpr wchar_t name[] = L"U15 screenshot: Bgfx rejects requests outside initialized lifetime";
    const auto scenario = environmentValue(childScenarioEnv);
    if (scenario == L"before-init" || scenario == L"after-shutdown") {
        const auto directory = childDirectory();
        BgfxRenderDevice renderer;
        if (scenario == L"after-shutdown") {
            renderer.beginShutdown();
            renderer.shutdown();
        }
        REQUIRE_FALSE(renderer.isInitialized());
        writeBytes(directory / "entered-capture.txt", "entered");
        const auto output = directory / "requested.png";
        CHECK_FALSE(renderer.requestScreenshot(output.string()));
        writeBytes(directory / "returned-from-capture.txt", "returned");
        CHECK_FALSE(fs::exists(output));
        return;
    }
    REQUIRE(scenario.empty());
    for (const auto* state : {L"before-init", L"after-shutdown"}) {
        const bool beforeInit = std::wstring(state) == L"before-init";
        INFO("renderer state: ", beforeInit ? "before init" : "after shutdown");
        TestPaths::ScopedTempDir directory("u15_screenshot_lifetime");
        const auto result = runScreenshotChild(name, state, directory.path());
        checkChild(result, directory.path());
        CHECK_FALSE(fs::exists(directory.path() / "requested.png"));
    }
}

TEST_CASE("U15 screenshot: headless SaveManager leaves stale thumbnail untouched") {
    constexpr wchar_t name[] = L"U15 screenshot: headless SaveManager leaves stale thumbnail untouched";
    const auto scenario = environmentValue(childScenarioEnv);
    if (scenario == L"headless-manager") {
        headlessCapture(false);
        return;
    }
    REQUIRE(scenario.empty());
    TestPaths::ScopedTempDir directory("u15_headless_manager");
    writeBytes(directory.path() / "save_thumb.png", staleThumbnail);
    const auto result = runScreenshotChild(name, L"headless-manager", directory.path());
    checkChild(result, directory.path());
    CHECK(readBytes(directory.path() / "save_thumb.png") == staleThumbnail);
}

TEST_CASE("U15 screenshot: headless KAG binding leaves stale thumbnail untouched") {
    constexpr wchar_t name[] = L"U15 screenshot: headless KAG binding leaves stale thumbnail untouched";
    const auto scenario = environmentValue(childScenarioEnv);
    if (scenario == L"headless-lua") {
        headlessCapture(true);
        return;
    }
    REQUIRE(scenario.empty());
    TestPaths::ScopedTempDir directory("u15_headless_lua");
    writeBytes(directory.path() / "save_thumb.png", staleThumbnail);
    const auto result = runScreenshotChild(name, L"headless-lua", directory.path());
    checkChild(result, directory.path());
    CHECK(readBytes(directory.path() / "save_thumb.png") == staleThumbnail);
}

#endif

TEST_CASE("U15 screenshot: Null renderer consistently rejects capture") {
    TestPaths::ScopedTempDir directory("u15_null_screenshot");
    const auto output = directory.path() / "requested.png";
    NullRenderDevice renderer;
    CHECK_FALSE(renderer.requestScreenshot(output.string()));
    REQUIRE(renderer.init(nullptr, 320, 180));
    CHECK_FALSE(renderer.isInitialized());
    CHECK_FALSE(renderer.requestScreenshot(output.string()));
    renderer.beginShutdown();
    CHECK_FALSE(renderer.requestScreenshot(output.string()));
    renderer.shutdown();
    CHECK_FALSE(renderer.requestScreenshot(output.string()));
    CHECK_FALSE(fs::exists(output));
}
