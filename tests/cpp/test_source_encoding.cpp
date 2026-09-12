#include "doctest.h"
#include "render/TextRenderer.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool isSourceFile(const std::filesystem::path& path) {
    const auto ext = path.extension().string();
    return ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx" ||
           ext == ".h" || ext == ".hh" || ext == ".hpp" || ext == ".hxx";
}

std::filesystem::path findRepoRoot() {
#ifdef CAESURA_SOURCE_DIR
    // Out-of-tree builds (e.g. WSL or CI) have no source tree on the cwd
    // chain; the CMake build injects the source root -- prefer it.
    const std::filesystem::path fromMacro(CAESURA_SOURCE_DIR);
    if (std::filesystem::exists(fromMacro / "src") &&
        std::filesystem::exists(fromMacro / "tests" / "cpp")) {
        return fromMacro;
    }
#endif
    auto path = std::filesystem::current_path();
    while (!path.empty()) {
        if (std::filesystem::exists(path / "src") &&
            std::filesystem::exists(path / "tests" / "cpp")) {
            return path;
        }
        const auto parent = path.parent_path();
        if (parent == path) break;
        path = parent;
    }
    return {};
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}

size_t countOccurrences(std::string_view text, std::string_view needle) {
    if (needle.empty()) return 0;

    size_t count = 0;
    size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string_view::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

size_t findMatchingBrace(std::string_view text, size_t openingBrace) {
    int depth = 0;
    for (size_t i = openingBrace; i < text.size(); ++i) {
        if (text[i] == '{') {
            ++depth;
        } else if (text[i] == '}' && --depth == 0) {
            return i;
        }
    }
    return std::string_view::npos;
}

bool containsInstanceAccessor(std::string_view text) {
    std::string compact;
    compact.reserve(text.size());
    for (const unsigned char c : text) {
        if (!std::isspace(c)) compact.push_back(static_cast<char>(c));
    }
    return compact.find("instance(") != std::string::npos;
}

bool isValidUtf8(std::string_view bytes) {
    size_t i = 0;
    while (i < bytes.size()) {
        const auto c = static_cast<unsigned char>(bytes[i]);
        if (c <= 0x7F) {
            ++i;
            continue;
        }

        size_t continuationCount = 0;
        uint32_t codePoint = 0;
        if ((c & 0xE0) == 0xC0) {
            continuationCount = 1;
            codePoint = c & 0x1F;
            if (codePoint == 0) return false;
        } else if ((c & 0xF0) == 0xE0) {
            continuationCount = 2;
            codePoint = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            continuationCount = 3;
            codePoint = c & 0x07;
        } else {
            return false;
        }

        if (i + continuationCount >= bytes.size()) return false;
        for (size_t j = 1; j <= continuationCount; ++j) {
            const auto next = static_cast<unsigned char>(bytes[i + j]);
            if ((next & 0xC0) != 0x80) return false;
            codePoint = (codePoint << 6) | (next & 0x3F);
        }

        if ((continuationCount == 1 && codePoint < 0x80) ||
            (continuationCount == 2 && codePoint < 0x800) ||
            (continuationCount == 3 && codePoint < 0x10000) ||
            codePoint > 0x10FFFF ||
            (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
            return false;
        }

        i += continuationCount + 1;
    }
    return true;
}

void collectInvalidUtf8Files(const std::filesystem::path& root,
                             std::vector<std::filesystem::path>& invalid) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file() || !isSourceFile(entry.path())) continue;
        if (!isValidUtf8(readFile(entry.path()))) {
            invalid.push_back(entry.path());
        }
    }
}

} // namespace

TEST_CASE("C++ source files are valid UTF-8") {
    const auto repoRoot = findRepoRoot();
    REQUIRE_FALSE(repoRoot.empty());

    std::vector<std::filesystem::path> invalid;
    collectInvalidUtf8Files(repoRoot / "src", invalid);
    collectInvalidUtf8Files(repoRoot / "tests" / "cpp", invalid);

    std::ostringstream message;
    message << "Invalid UTF-8 source files:";
    for (const auto& path : invalid) {
        message << "\n  " << std::filesystem::relative(path, repoRoot).generic_string();
    }
    INFO(message.str());
    CHECK(invalid.empty());
}

TEST_CASE("BackendRegistry depends only on module interfaces") {
    const auto repoRoot = findRepoRoot();
    REQUIRE_FALSE(repoRoot.empty());

    const std::string source = readFile(repoRoot / "src" / "di" / "BackendRegistry.cpp");
    CHECK(source.find("BgfxRenderDevice") == std::string::npos);
    CHECK(source.find("../render/BgfxRenderDevice.h") == std::string::npos);
    CHECK(source.find("class NullRenderDevice") == std::string::npos);
    CHECK(source.find("class NullPlatformBackend") == std::string::npos);

    const std::string header = readFile(repoRoot / "src" / "di" / "BackendRegistry.h");
    CHECK(header.find("../resource/ResourceHandle.h") == std::string::npos);
    CHECK(header.find("GenerationTracker  m_generations") == std::string::npos);
}

TEST_CASE("Render device interface does not expose bgfx handle types") {
    const auto repoRoot = findRepoRoot();
    REQUIRE_FALSE(repoRoot.empty());

    const std::string source = readFile(repoRoot / "src" / "render" / "api" / "IRenderDevice.h");
    CHECK(source.find("#include <bgfx/bgfx.h>") == std::string::npos);
    CHECK(source.find("../render/BgfxDebugCallback.h") == std::string::npos);
    CHECK(source.find("BgfxDebugCallback") == std::string::npos);
    CHECK(source.find("bgfx::") == std::string::npos);
}

TEST_CASE("Backend interfaces do not provide concrete default behavior") {
    const auto repoRoot = findRepoRoot();
    REQUIRE_FALSE(repoRoot.empty());

    const std::string render = readFile(repoRoot / "src" / "render" / "api" / "IRenderDevice.h");
    CHECK(render.find("virtual void beginShutdown() {") == std::string::npos);
    CHECK(render.find("virtual void flagDeviceLost() {") == std::string::npos);
    CHECK(render.find("virtual bool consumeDeviceLost() {") == std::string::npos);
    CHECK(render.find("virtual RenderUniformHandle getDefaultSampler() const {") == std::string::npos);
    CHECK(render.find("virtual RenderProgramHandle getFallbackProgram() const {") == std::string::npos);
    CHECK(render.find("virtual RenderRuntimeInfo getRuntimeInfo() const {") == std::string::npos);
    CHECK(render.find("virtual bool setPreferredBackend(const char*) {") == std::string::npos);

    const std::string saves = readFile(repoRoot / "src" / "storage" / "api" / "ISaveProvider.h");
    CHECK(saves.find("virtual bool pushToCloud(const std::string& slotPath) {") == std::string::npos);
    CHECK(saves.find("virtual bool pullFromCloud(const std::string& slotPath) {") == std::string::npos);
    CHECK(saves.find("virtual bool supportsCloudSync() const {") == std::string::npos);
}

TEST_CASE("Main entry point delegates script bootstrap helpers") {
    const auto repoRoot = findRepoRoot();
    REQUIRE_FALSE(repoRoot.empty());

    const auto helperPath = repoRoot / "src" / "entry" / "StartupScripts.cpp";
    const std::string source = readFile(repoRoot / "src" / "main.cpp");
    CHECK(source.find("fopen(\"scripts/kag/init.lua\", \"r\")") == std::string::npos);
    CHECK(source.find("lua_getglobal(L, \"package\")") == std::string::npos);
    CHECK(source.find("Caesura::discoverStartupScriptDir()") != std::string::npos);
    CHECK(source.find("Caesura::configureStartupLuaPath(L, scriptDir)") != std::string::npos);
    REQUIRE(std::filesystem::exists(helperPath));

    const std::string helper = readFile(helperPath);
    CHECK(countOccurrences(helper, "fopen(\"scripts/kag/init.lua\", \"r\")") == 1);
    CHECK(countOccurrences(helper, "lua_getglobal(L, \"package\")") == 1);
}

TEST_CASE("Main entry point leaves GPU monitor selection to Engine") {
    const auto repoRoot = findRepoRoot();
    REQUIRE_FALSE(repoRoot.empty());

    const std::string source = readFile(repoRoot / "src" / "main.cpp");
    CHECK(source.find("#include \"render/GpuMonitor.h\"") == std::string::npos);
    CHECK(source.find("config.gpuMonitor") == std::string::npos);
}

TEST_CASE("Main entry point avoids unused Steam concrete backend include") {
    const auto repoRoot = findRepoRoot();
    REQUIRE_FALSE(repoRoot.empty());

    const std::string source = readFile(repoRoot / "src" / "main.cpp");
    CHECK(source.find("#include \"steam/SteamBackend.h\"") == std::string::npos);
}

TEST_CASE("Main entry point leaves default optional backends to Engine") {
    const auto repoRoot = findRepoRoot();
    REQUIRE_FALSE(repoRoot.empty());

    const std::string source = readFile(repoRoot / "src" / "main.cpp");
    CHECK(source.find("#include \"audio/NullAudioBackend.h\"") == std::string::npos);
    CHECK(source.find("#include \"minigame/NullMiniGameBackend.h\"") == std::string::npos);
    CHECK(source.find("#include \"live2d/NullAnimationBackend.h\"") == std::string::npos);
    CHECK(source.find("#include \"steam/NullSteamBackend.h\"") == std::string::npos);
    CHECK(source.find("#include \"input/InputRouter.h\"") == std::string::npos);
    CHECK(source.find("#include \"render/VideoPlayer.h\"") == std::string::npos);
    CHECK(source.find("config.audio    = new Caesura::NullAudioBackend") == std::string::npos);
    CHECK(source.find("config.miniGame = new Caesura::NullMiniGameBackend") == std::string::npos);
    CHECK(source.find("config.animation") == std::string::npos);
    CHECK(source.find("config.steam") == std::string::npos);
    CHECK(source.find("config.lua         =") == std::string::npos);
    CHECK(source.find("config.inputRouter") == std::string::npos);
    CHECK(source.find("config.videoPlayer") == std::string::npos);
}

TEST_CASE("Main entry point uses texture manager interface") {
    const auto repoRoot = findRepoRoot();
    REQUIRE_FALSE(repoRoot.empty());

    const std::string source = readFile(repoRoot / "src" / "main.cpp");
    CHECK(source.find("#include \"render/TextureManager.h\"") == std::string::npos);
    CHECK(source.find("#include \"render/api/ITextureManager.h\"") == std::string::npos);
    CHECK(source.find("#include \"di/BackendRegistry.h\"") == std::string::npos);
    CHECK(source.find("BackendRegistry::instance().getTextureManager()") == std::string::npos);
    CHECK(source.find("setDevMode(devMode)") == std::string::npos);
    CHECK(source.find("Caesura::applyDevModeToTextureManager(L)") != std::string::npos);
}

TEST_CASE("Archive trust is not reselected after Lua startup configuration") {
    const auto repoRoot = findRepoRoot();
    REQUIRE_FALSE(repoRoot.empty());

    const auto helperPath = repoRoot / "src" / "entry" / "StartupValidation.cpp";
    const std::string source = readFile(repoRoot / "src" / "main.cpp");
    CHECK(source.find("#include \"archive/CARCReader.h\"") == std::string::npos);
    CHECK(source.find("carc::CARCReader") == std::string::npos);
    CHECK(source.find("verifySignature()") == std::string::npos);
    CHECK(source.find("validateCarcOnStartup") == std::string::npos);
    REQUIRE(std::filesystem::exists(helperPath));

    const std::string helper = readFile(helperPath);
    CHECK(countOccurrences(helper, "[main] CARC startup validation enabled.") == 0);
    CHECK(helper.find("carc::CARCReader") == std::string::npos);
    CHECK(helper.find("verifySignature()") == std::string::npos);
}

TEST_CASE("Install layout includes configured demo entry script") {
    const auto repoRoot = findRepoRoot();
    REQUIRE_FALSE(repoRoot.empty());

    const std::string config = readFile(repoRoot / "scripts" / "config.lua");
    CHECK(config.find("config.entry_script = \"../demo/entry.lua\"") != std::string::npos);
    CHECK(config.find("config.thumbnail_quality = 90") != std::string::npos);
    CHECK(config.find("config.thumbnail_format  = \"png\"") != std::string::npos);

    const std::string cmake = readFile(repoRoot / "CMakeLists.txt");
    CHECK(cmake.find("install(DIRECTORY scripts/ DESTINATION scripts)") != std::string::npos);
    CHECK(cmake.find("install(DIRECTORY demo/ DESTINATION demo)") != std::string::npos);
}

TEST_CASE("Engine input loop keeps wheel and save shortcuts reachable") {
    const auto repoRoot = findRepoRoot();
    REQUIRE_FALSE(repoRoot.empty());

    const std::string source = readFile(repoRoot / "src" / "entry" / "Engine.cpp");
    const auto pointerBranch = source.find(
        "if ((event.type == SDL_EVENT_MOUSE_MOTION ||");
    const auto pointerOpen = source.find('{', pointerBranch);
    const auto pointerClose = findMatchingBrace(source, pointerOpen);
    const auto wheelBranch = source.find("if (event.type == SDL_EVENT_MOUSE_WHEEL");

    REQUIRE(pointerBranch != std::string::npos);
    REQUIRE(pointerOpen != std::string::npos);
    REQUIRE(pointerClose != std::string::npos);
    REQUIRE(wheelBranch != std::string::npos);
    CHECK(wheelBranch > pointerClose);
    CHECK(source.find("if (!event.key.repeat && !isLuaExecutionPaused()) quicksave();") != std::string::npos);
    CHECK(source.find("if (!event.key.repeat && !isLuaExecutionPaused()) quickload();") != std::string::npos);
}

TEST_CASE("Install layout includes FFmpeg runtime DLLs when FFmpeg is bundled") {
    const auto repoRoot = findRepoRoot();
    REQUIRE_FALSE(repoRoot.empty());

    const std::string cmake = readFile(repoRoot / "CMakeLists.txt");
    // Only the five linked DLLs are copied/installed (avfilter/avdevice and
    // the CLI tools are unused); the list is explicit so a restored FFmpeg
    // bin/ cannot silently blow up the build output again.
    CHECK(cmake.find("avcodec-62.dll avformat-62.dll avutil-60.dll") != std::string::npos);
    CHECK(cmake.find("swscale-9.dll swresample-6.dll") != std::string::npos);
    CHECK(cmake.find("install(FILES") != std::string::npos);
}

TEST_CASE("Engine core avoids unused concrete adapter dependencies") {
    const auto repoRoot = findRepoRoot();
    REQUIRE_FALSE(repoRoot.empty());

    const std::string source = readFile(repoRoot / "src" / "entry" / "Engine.cpp");
    CHECK(source.find("../platform/SDL3PlatformBackend.h") == std::string::npos);
    CHECK(source.find("../audio/SoLoudAudioEngine.h") == std::string::npos);
    CHECK(source.find("SoLoudAudioEngine") == std::string::npos);
    CHECK(source.find("../render/BgfxRenderDevice.h") == std::string::npos);
    CHECK(source.find("../minigame/BgfxMiniGameBackend.h") == std::string::npos);
    CHECK(source.find("../rpc/RpcServer.h") == std::string::npos);
    CHECK(source.find("RpcServer::instance()") == std::string::npos);

    const std::string miniGame = readFile(
        repoRoot / "src" / "minigame" / "BgfxMiniGameBackend.cpp");
    CHECK(miniGame.find("../render/EmbeddedShaders.h") == std::string::npos);
    CHECK(miniGame.find("EmbeddedMiniGameShaders.h") != std::string::npos);

    const std::string editor = readFile(repoRoot / "src" / "rpc" / "EditorServer.cpp");
    CHECK(editor.find("../archive/CARCWriter.h") == std::string::npos);
    CHECK(editor.find("carc::CARCWriter") == std::string::npos);
    CHECK(editor.find("m_archiveWriterFactory") != std::string::npos);

    const std::string textures = readFile(repoRoot / "src" / "render" / "TextureManager.cpp");
    CHECK(textures.find("../di/TextureBudget.h") == std::string::npos);
    CHECK(textures.find("TextureBudget::instance()") == std::string::npos);
    CHECK(textures.find("getTextureBudget()") != std::string::npos);
}

TEST_CASE("Engine core delegates renderer runtime operations to render interface") {
    const auto repoRoot = findRepoRoot();
    REQUIRE_FALSE(repoRoot.empty());

    const std::string source = readFile(repoRoot / "src" / "entry" / "Engine.cpp");
    CHECK(source.find("#include <bgfx/bgfx.h>") == std::string::npos);
    CHECK(source.find("bgfx::") == std::string::npos);
    CHECK(source.find("BGFX_") == std::string::npos);
    CHECK(source.find("bgfx shutdown complete") == std::string::npos);
    CHECK(source.find("bgfx reinit") == std::string::npos);
}

TEST_CASE("Editor frame capture renders even when headless is also set") {
    const auto repoRoot = findRepoRoot();
    REQUIRE_FALSE(repoRoot.empty());

    const std::string source = readFile(repoRoot / "src" / "entry" / "Engine.cpp");
    const auto captureStart = source.find("std::string Engine::captureFrameForRpc");
    REQUIRE(captureStart != std::string::npos);
    const auto captureOpen = source.find('{', captureStart);
    REQUIRE(captureOpen != std::string::npos);
    const auto captureClose = findMatchingBrace(source, captureOpen);
    REQUIRE(captureClose != std::string::npos);

    const std::string_view captureBody(source.data() + captureOpen,
                                       captureClose - captureOpen + 1);
    CHECK(captureBody.find("if (!m_config.headless || m_config.editorMode)") !=
          std::string_view::npos);
}

TEST_CASE("Bgfx fallback does not shut down an initialization that failed") {
    const auto repoRoot = findRepoRoot();
    REQUIRE_FALSE(repoRoot.empty());

    const std::string source = readFile(
        repoRoot / "src" / "render" / "BgfxDeviceCore.cpp");
    CHECK(countOccurrences(source, "bgfx::shutdown();") == 1);
}

TEST_CASE("Engine core delegates CARC asset provider registration") {
    const auto repoRoot = findRepoRoot();
    REQUIRE_FALSE(repoRoot.empty());

    const std::string source = readFile(repoRoot / "src" / "entry" / "Engine.cpp");
    CHECK(source.find("../archive/CARCReader.h") == std::string::npos);
    CHECK(source.find("../archive/CarcAssetProvider.h") == std::string::npos);
    CHECK(source.find("carc::CARCReader") == std::string::npos);
    CHECK(source.find("carc::CarcAssetProvider") == std::string::npos);
}

TEST_CASE("Engine core delegates default backend construction") {
    const auto repoRoot = findRepoRoot();
    REQUIRE_FALSE(repoRoot.empty());

    const std::string source = readFile(repoRoot / "src" / "entry" / "Engine.cpp");
    CHECK(source.find("../audio/NullAudioBackend.h") == std::string::npos);
    CHECK(source.find("../minigame/NullMiniGameBackend.h") == std::string::npos);
    CHECK(source.find("../live2d/NullAnimationBackend.h") == std::string::npos);
    CHECK(source.find("../steam/SteamBackend.h") == std::string::npos);
    CHECK(source.find("../steam/NullSteamBackend.h") == std::string::npos);
    CHECK(source.find("../live2d/Live2D/Live2DBackend.h") == std::string::npos);
    CHECK(source.find("NullAudioBackend") == std::string::npos);
    CHECK(source.find("NullMiniGameBackend") == std::string::npos);
    CHECK(source.find("NullAnimationBackend") == std::string::npos);
    CHECK(source.find("std::make_unique<SteamBackend>") == std::string::npos);
    CHECK(source.find("std::make_unique<NullSteamBackend>") == std::string::npos);
    CHECK(source.find("Live2DBackend") == std::string::npos);
    CHECK(source.find("static_cast<Live2DBackend&>") == std::string::npos);

    const std::string live2dSource = readFile(
        repoRoot / "src" / "live2d" / "Live2D" / "Live2DBackend.cpp");
    const std::string live2dHeader = readFile(
        repoRoot / "src" / "live2d" / "Live2D" / "Live2DBackend.h");
    const std::string miniGameSource = readFile(
        repoRoot / "src" / "minigame" / "BgfxMiniGameBackend.cpp");
    const std::string miniGameHeader = readFile(
        repoRoot / "src" / "minigame" / "BgfxMiniGameBackend.h");
    REQUIRE_FALSE(live2dSource.empty());
    REQUIRE_FALSE(live2dHeader.empty());
    REQUIRE_FALSE(miniGameSource.empty());
    REQUIRE_FALSE(miniGameHeader.empty());
    CHECK(live2dSource.find("g_live2d") == std::string::npos);
    CHECK(live2dSource.find("registerLive2DBinding") == std::string::npos);
    CHECK(live2dSource.find("#include <lua.h>") == std::string::npos);
    CHECK(live2dHeader.find("Live2DBackend_setGlobal") == std::string::npos);
    CHECK(live2dHeader.find("registerLive2DBinding") == std::string::npos);
    CHECK(miniGameSource.find("g_mg") == std::string::npos);
    CHECK(miniGameSource.find("registerMiniGameBinding") == std::string::npos);
    CHECK(miniGameHeader.find("registerMiniGameBinding") == std::string::npos);
}

TEST_CASE("Engine composition root owns save system initialization") {
    const auto repoRoot = findRepoRoot();
    REQUIRE_FALSE(repoRoot.empty());

    const std::string engine = readFile(repoRoot / "src" / "entry" / "Engine.cpp");
    CHECK(engine.find("../storage/SaveManager.h") == std::string::npos);
    CHECK(engine.find("SaveManager::instance()") == std::string::npos);
    CHECK(engine.find("m_saveManager->init(\"saves/\")") != std::string::npos);
    CHECK(engine.find("setSaveManager(m_saveManager.get())") != std::string::npos);
    CHECK(engine.find("setSteamBackend(") != std::string::npos);

    const std::string factories = readFile(
        repoRoot / "src" / "entry" / "Engine_Backends.cpp");
    CHECK(factories.find("std::make_unique<SaveManager>()") != std::string::npos);
    CHECK(factories.find("std::make_unique<TextureBudget>()") != std::string::npos);
    CHECK(factories.find("std::make_unique<TextureManager>()") != std::string::npos);
    CHECK(factories.find("std::make_unique<AssetManager>()") != std::string::npos);
    CHECK(factories.find("std::make_unique<AsyncLoader>(assetManager)") != std::string::npos);
    CHECK(factories.find("std::make_unique<JobSystem>()") != std::string::npos);
    CHECK(factories.find("std::make_unique<carc::CryptoEngine>()") != std::string::npos);

    CHECK(engine.find("TextureBudget::instance()") == std::string::npos);
    CHECK(engine.find("TextureManager::instance()") == std::string::npos);
    CHECK(engine.find("AssetManager::instance()") == std::string::npos);
    CHECK(engine.find("AsyncLoader::instance()") == std::string::npos);
    CHECK(engine.find("JobSystem::instance()") == std::string::npos);
    CHECK(engine.find("carc::CryptoEngine::instance()") == std::string::npos);

    const std::string steamBinding = readFile(
        repoRoot / "src" / "script" / "bindings" / "SteamBinding.cpp");
    CHECK(steamBinding.find("static ISteamBackend*") == std::string::npos);
    CHECK(steamBinding.find("getSteamBackend()") != std::string::npos);

    const auto asyncShutdown = engine.find("m_asyncLoader->shutdown()");
    const auto assetShutdown = engine.find("m_assetManager->shutdown()");
    const auto jobShutdown = engine.find("m_jobSystem->shutdown()");
    const auto unregisterAsync = engine.find("setAsyncLoader(nullptr)");
    const auto unregisterJob = engine.find("setJobSystem(nullptr)");
    REQUIRE(asyncShutdown != std::string::npos);
    REQUIRE(assetShutdown != std::string::npos);
    REQUIRE(jobShutdown != std::string::npos);
    REQUIRE(unregisterAsync != std::string::npos);
    REQUIRE(unregisterJob != std::string::npos);
    CHECK(asyncShutdown < assetShutdown);
    // Workers and final callbacks finish while the asset owner is still alive.
    CHECK(jobShutdown < asyncShutdown);
    CHECK(jobShutdown < unregisterAsync);
    CHECK(jobShutdown < unregisterJob);

    const std::string engineHeader = readFile(
        repoRoot / "src" / "entry" / "Engine.h");
    const auto jobMember = engineHeader.find("std::unique_ptr<IJobSystem>");
    const auto assetMember = engineHeader.find("std::unique_ptr<AssetManager>");
    const auto asyncMember = engineHeader.find("std::unique_ptr<IAsyncLoader>");
    REQUIRE(jobMember != std::string::npos);
    REQUIRE(assetMember != std::string::npos);
    REQUIRE(asyncMember != std::string::npos);
    CHECK(jobMember < assetMember);
    CHECK(assetMember < asyncMember);

    const std::string binding = readFile(repoRoot / "src" / "script" / "bindings" / "SaveBinding.cpp");
    CHECK(binding.find("SaveManager::instance()") == std::string::npos);
    CHECK(binding.find("storage/api/ISaveManager.h") != std::string::npos);

    const std::string asyncLoader = readFile(
        repoRoot / "src" / "resource" / "AsyncLoader.cpp");
    CHECK(asyncLoader.find("AssetManager::instance()") == std::string::npos);
    CHECK(asyncLoader.find("m_assetManager->read(req.path)") != std::string::npos);

    const std::string assetManager = readFile(
        repoRoot / "src" / "resource" / "AssetManager.cpp");
    CHECK(assetManager.find("AssetManager::instance()") == std::string::npos);

    const std::string assetManagerHeader = readFile(
        repoRoot / "src" / "resource" / "AssetManager.h");
    CHECK(assetManagerHeader.find("static AssetManager& instance()") == std::string::npos);
}

TEST_CASE("Runtime services do not expose singleton accessors") {
    const auto repoRoot = findRepoRoot();
    REQUIRE_FALSE(repoRoot.empty());

    struct RuntimeService {
        const char* header;
        const char* implementation;
        const char* typeName;
    };
    const RuntimeService services[] = {
        {"src/di/TextureBudget.h", "src/di/TextureBudget.cpp", "TextureBudget"},
        {"src/render/TextureManager.h", "src/render/TextureManager.cpp", "TextureManager"},
        {"src/render/LayerManager.h", "src/render/LayerManager.cpp", "LayerManager"},
        {"src/di/SandboxQuota.h", "src/di/SandboxQuota.cpp", "SandboxQuotaService"},
        {"src/job/JobSystem.h", "src/job/JobSystem.cpp", "JobSystem"},
        {"src/resource/AssetManager.h", "src/resource/AssetManager.cpp", "AssetManager"},
        {"src/resource/AsyncLoader.h", "src/resource/AsyncLoader.cpp", "AsyncLoader"},
        {"src/script/vm/LuaManager.h", "src/script/vm/LuaManager.cpp", "LuaManager"},
        {"src/debug/HotReload.h", "src/debug/HotReload.cpp", "HotReload"},
        {"src/debug/DebugProtocol.h", "src/debug/DebugProtocol.cpp", "DebugProtocol"},
        {"src/storage/SaveManager.h", "src/storage/SaveManager.cpp", "SaveManager"},
        {"src/archive/CryptoEngine.h", "src/archive/CryptoEngine.cpp", "CryptoEngine"},
    };

    for (const auto& service : services) {
        CAPTURE(service.typeName);
        const std::string header = readFile(repoRoot / service.header);
        const std::string implementation = readFile(repoRoot / service.implementation);
        REQUIRE_FALSE(header.empty());
        REQUIRE_FALSE(implementation.empty());
        CHECK_FALSE(containsInstanceAccessor(header));
        CHECK(implementation.find(std::string(service.typeName) + "::instance") ==
              std::string::npos);
    }

    const std::string engine = readFile(repoRoot / "src" / "entry" / "Engine.cpp");
    const std::string engineHeader = readFile(repoRoot / "src" / "entry" / "Engine.h");
    CHECK(engine.find("HotReload::instance()") == std::string::npos);
    CHECK(engineHeader.find("std::unique_ptr<HotReload>") != std::string::npos);
}

TEST_CASE("TextRenderer owns FreeType without a global context") {
    const auto repoRoot = findRepoRoot();
    REQUIRE_FALSE(repoRoot.empty());

    CHECK_FALSE(std::filesystem::exists(
        repoRoot / "src" / "render" / "FreeTypeContext.h"));
    CHECK_FALSE(std::filesystem::exists(
        repoRoot / "src" / "render" / "FreeTypeContext.cpp"));

    const std::string engine = readFile(repoRoot / "src" / "entry" / "Engine.cpp");
    const std::string engineHeader = readFile(repoRoot / "src" / "entry" / "Engine.h");
    const std::string renderer = readFile(repoRoot / "src" / "render" / "TextRenderer.cpp");
    const std::string font = readFile(repoRoot / "src" / "render" / "TextRendererFont.cpp");
    const std::string modules = readFile(repoRoot / "cmake" / "CaesuraModules.cmake");

    CHECK(engine.find("FreeTypeContext") == std::string::npos);
    CHECK(engineHeader.find("freeTypeInitialized") == std::string::npos);
    CHECK(renderer.find("FreeTypeContext") == std::string::npos);
    CHECK(font.find("FreeTypeContext") == std::string::npos);
    // Exercise ownership instead of requiring a particular local variable's
    // dot/arrow spelling. Both real faces must outlive the caller's bytes;
    // destroying one atlas must not invalidate the other atlas's FT context.
    auto fontBytes = readFile(repoRoot / "assets" / "fonts" / "NotoSansCJKsc-Regular.otf");
    REQUIRE_FALSE(fontBytes.empty());
    using Atlas = Caesura::TextRenderer::GlyphAtlas;
    const auto* data = reinterpret_cast<const uint8_t*>(fontBytes.data());
    auto first = Atlas::create(data, fontBytes.size(), 28);
    auto second = Atlas::create(data, fontBytes.size(), 14);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    fontBytes.assign(fontBytes.size(), '\0');
    std::string{}.swap(fontBytes);
    const auto referenceBytes = readFile(repoRoot / "assets" / "fonts" / "NotoSansCJKsc-Regular.otf");
    const auto verifyNewGlyph = [&](Atlas& atlas, unsigned size, uint32_t codepoint) {
        struct Reference {
            FT_Library library = nullptr;
            FT_Face face = nullptr;
            ~Reference() {
                if (face) FT_Done_Face(face);
                if (library) FT_Done_FreeType(library);
            }
        } reference;
        REQUIRE(FT_Init_FreeType(&reference.library) == 0);
        REQUIRE(FT_New_Memory_Face(reference.library,
            reinterpret_cast<const uint8_t*>(referenceBytes.data()),
            static_cast<FT_Long>(referenceBytes.size()),0,&reference.face) == 0);
        REQUIRE(FT_Set_Pixel_Sizes(reference.face,0,size) == 0);
        REQUIRE(FT_Load_Char(reference.face,codepoint,FT_LOAD_RENDER) == 0);
        REQUIRE(atlas.prepareGlyph(codepoint) == Atlas::PrepareStatus::Added);
        const auto* glyph = atlas.find(codepoint);
        REQUIRE(glyph != nullptr);
        const auto& ft = *reference.face->glyph;
        REQUIRE(glyph->w == static_cast<int>(ft.bitmap.width));
        REQUIRE(glyph->h == static_cast<int>(ft.bitmap.rows));
        CHECK(glyph->advance == (ft.advance.x >> 6));
        CHECK(glyph->offsetX == ft.bitmap_left);
        CHECK(glyph->offsetY == ft.bitmap_top);
        REQUIRE(glyph->w > 0);
        REQUIRE(glyph->h > 0);
        bool pixelsMatch = true;
        for (int y=0;y<glyph->h;++y) for (int x=0;x<glyph->w;++x)
            pixelsMatch &= atlas.pixels()[(size_t(glyph->y+y)*atlas.width()+glyph->x+x)*4+3]
                == ft.bitmap.buffer[y*ft.bitmap.pitch+x];
        CHECK(pixelsMatch);
    };
    verifyNewGlyph(*first,28,0x6f22);
    verifyNewGlyph(*second,14,0x6f22);
    first.reset();
    verifyNewGlyph(*second,14,0x5b57);
    CHECK(modules.find("FreeTypeContext.cpp") == std::string::npos);
}

TEST_CASE("Engine core delegates Lua registry service injection") {
    const auto repoRoot = findRepoRoot();
    REQUIRE_FALSE(repoRoot.empty());

    const auto helperPath = repoRoot / "src" / "entry" / "Engine_LuaRegistry.cpp";
    const std::string source = readFile(repoRoot / "src" / "entry" / "Engine.cpp");

    CHECK(source.find("\"Caesura.RenderDevice\"") == std::string::npos);
    CHECK(source.find("\"Caesura.AudioBackend\"") == std::string::npos);
    CHECK(source.find("\"Caesura.PlatformBackend\"") == std::string::npos);
    CHECK(source.find("\"Caesura.InputRouter\"") == std::string::npos);
    CHECK(source.find("\"Caesura.VideoPlayer\"") == std::string::npos);
    CHECK(source.find("\"Caesura.TextureManager\"") == std::string::npos);
    CHECK(source.find("\"Caesura.AsyncLoader\"") == std::string::npos);
    CHECK(source.find("\"Caesura.DebugManager\"") == std::string::npos);
    CHECK(source.find("\"Caesura.MiniGameBackend\"") == std::string::npos);
    CHECK(source.find("registerEngineLuaRegistryServices(") != std::string::npos);
    CHECK(source.find("registerMiniGameLuaRegistryService(") != std::string::npos);
    REQUIRE(std::filesystem::exists(helperPath));

    const std::string helper = readFile(helperPath);
    CHECK(helper.find("TextureManager::instance()") == std::string::npos);
    CHECK(helper.find("AsyncLoader::instance()") == std::string::npos);
    CHECK(countOccurrences(helper, "\"Caesura.RenderDevice\"") == 1);
    CHECK(countOccurrences(helper, "\"Caesura.AudioBackend\"") == 1);
    CHECK(countOccurrences(helper, "\"Caesura.PlatformBackend\"") == 1);
    CHECK(countOccurrences(helper, "\"Caesura.InputRouter\"") == 1);
    CHECK(countOccurrences(helper, "\"Caesura.VideoPlayer\"") == 1);
    CHECK(countOccurrences(helper, "\"Caesura.TextureManager\"") == 1);
    CHECK(countOccurrences(helper, "\"Caesura.AsyncLoader\"") == 1);
    CHECK(countOccurrences(helper, "\"Caesura.DebugManager\"") == 1);
    CHECK(countOccurrences(helper, "\"Caesura.MiniGameBackend\"") == 1);
}
