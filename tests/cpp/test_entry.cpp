// test_entry.cpp - entry module unit tests (R4.2, extended S3)
#include "doctest.h"
#include "entry/Engine.h"
#include "entry/EngineConfig.h"
#include "debug/DebugProtocol.h"
#include "di/BackendRegistry.h"
#include "input/InputRouter.h"
#include "render/api/IGpuMonitor.h"
#include "platform/api/IDisplayService.h"
#include "platform/SDL3PlatformBackend.h"
#include "script/vm/LuaManager.h"
#include "EntryLifecycleBackends.h"
#include "audio/SoLoudAudioEngine.h"
#include "audio/AudioFocusService.h"
#include "audio/api/IAudioFocusService.h"
#include "platform/api/ILifecycleService.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include "PublisherArchives.h"
#include <SDL3/SDL.h>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <utility>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

using namespace Caesura;

namespace Caesura {
std::unique_ptr<IDisplayService> createDisplayService(
    const EngineConfig& config, const IPlatformBackend* platformBackend);
SDL_Window* getSDLWindow(const IPlatformBackend* platformBackend);
}

namespace {

class TestGpuMonitor : public IGpuMonitor {
public:
    explicit TestGpuMonitor(int* destructorCalls = nullptr)
        : m_destructorCalls(destructorCalls) {}
    ~TestGpuMonitor() override {
        if (m_destructorCalls) ++*m_destructorCalls;
    }

    GpuQuality update(double) override { return GpuQuality::HIGH; }
    const FrameMetrics& metrics() override { return m_metrics; }
    GpuQuality currentQuality() const override { return GpuQuality::HIGH; }
    bool isDegraded() const override { return false; }
    float resolutionScale() const override { return 1.0f; }
    bool vfxEnabled() const override { return true; }
    void reset() override {}
    void setGpuAvailable(bool) override {}

private:
    int* m_destructorCalls = nullptr;
    FrameMetrics m_metrics;
};

class TestDisplayService final : public IDisplayService {
public:
    explicit TestDisplayService(int& destructorCalls)
        : m_destructorCalls(destructorCalls) {}
    ~TestDisplayService() override { ++m_destructorCalls; }

    DisplayMetrics currentMetrics() const override { return {}; }

private:
    int& m_destructorCalls;
};

void checkEngineRegistryCleared() {
    auto& registry = BackendRegistry::instance();
    CHECK(registry.getPlatformBackend() == nullptr);
    CHECK(registry.getRenderDevice() == nullptr);
    CHECK(registry.getAudioBackend() == nullptr);
    CHECK(registry.getInputRouter() == nullptr);
    CHECK(registry.getVideoPlayer() == nullptr);
    CHECK(registry.getTextureManager() == nullptr);
    CHECK(registry.getLayerManager() == nullptr);
    CHECK(registry.getSandboxQuota() == nullptr);
    CHECK(registry.getTextureBudget() == nullptr);
    CHECK(registry.getDebugManager() == nullptr);
    CHECK(registry.getAsyncLoader() == nullptr);
    CHECK(registry.getJobSystem() == nullptr);
    CHECK(registry.getCryptoEngine() == nullptr);
    CHECK(registry.getSaveManager() == nullptr);
    CHECK(registry.getParticleSystem() == nullptr);
    CHECK(registry.getResourceGenerationTracker() == nullptr);
    CHECK(registry.getMiniGameBackend() == nullptr);
    CHECK(registry.getAnimationBackend() == nullptr);
    CHECK(registry.getSteamBackend() == nullptr);
    CHECK(registry.getLuaManager() == nullptr);
    CHECK(registry.getDisplayService() == nullptr);
    CHECK(registry.getLifecycleService() == nullptr);
    CHECK(registry.getAudioFocusService() == nullptr);
    CHECK(registry.getMeshRenderer() == nullptr);
}

} // namespace

TEST_CASE("Entry: EngineConfig default values") {
    EngineConfig cfg;
    CHECK(cfg.width == 1280);
    CHECK(cfg.height == 720);
    CHECK(cfg.title != nullptr);
    CHECK(std::strcmp(cfg.title, "Caesura (AmeKAG)") == 0);
    CHECK(cfg.headless == false);
    CHECK(cfg.editorMode == false);
    CHECK(cfg.enableDebugger == false);
}

TEST_CASE("Entry: EngineConfig pointer fields default nullptr") {
    CHECK_FALSE(std::is_copy_constructible_v<EngineConfig>);
    CHECK(std::is_move_constructible_v<EngineConfig>);
    EngineConfig cfg;
    CHECK(cfg.platform == nullptr);
    CHECK(cfg.render == nullptr);
    CHECK(cfg.audio == nullptr);
    CHECK(cfg.lua == nullptr);
    CHECK(cfg.inputRouter == nullptr);
    CHECK(cfg.gpuMonitor == nullptr);
    CHECK(cfg.videoPlayer == nullptr);
    CHECK(cfg.layerManager == nullptr);
    CHECK(cfg.sandboxQuota == nullptr);
    CHECK(cfg.miniGame == nullptr);
    CHECK(cfg.animation == nullptr);
    CHECK(cfg.steam == nullptr);
    CHECK(cfg.displayService == nullptr);
}

TEST_CASE("Entry: EngineConfig headless mode flag") {
    EngineConfig cfg;
    cfg.headless = true;
    CHECK(cfg.headless == true);
    CHECK(cfg.editorMode == false);
}

TEST_CASE("Entry: EngineConfig editor mode flag") {
    EngineConfig cfg;
    cfg.editorMode = true;
    CHECK(cfg.editorMode == true);
    CHECK(cfg.headless == false);
}

TEST_CASE("Entry: EngineConfig preserves debugger policy when moved") {
    EngineConfig cfg;
    cfg.enableDebugger = true;

    EngineConfig moved(std::move(cfg));
    CHECK(moved.enableDebugger);
}

TEST_CASE("Entry: EngineConfig frameLimit defaults to 0 (unlimited)") {
    EngineConfig cfg;
    CHECK(cfg.frameLimit == 0);
    CHECK(cfg.renderBackend == nullptr);
}

TEST_CASE("Entry: EngineConfig preserves frameLimit when moved") {
    EngineConfig cfg;
    cfg.frameLimit = 300;

    EngineConfig moved(std::move(cfg));
    CHECK(moved.frameLimit == 300);
}

TEST_CASE("Entry: EngineConfig rejects frameLimit via sane constructor path") {
    // --frames parsing lives in main.cpp; EngineConfig only carries the
    // validated value. Ensure a positive value survives the move and a
    // zero (CLI absence) means "unlimited".
    EngineConfig cfg;
    cfg.frameLimit = 1;
    EngineConfig moved(std::move(cfg));
    CHECK(moved.frameLimit == 1);
}

TEST_CASE("Entry: EngineConfig custom title") {
    EngineConfig cfg;
    CHECK(std::strlen(cfg.title) > 0);
}

TEST_CASE("Entry: EngineConfig width/height range") {
    EngineConfig cfg;
    cfg.width = 640;
    cfg.height = 480;
    CHECK(cfg.width == 640);
    CHECK(cfg.height == 480);

    cfg.width = 1920;
    cfg.height = 1080;
    CHECK(cfg.width == 1920);
    CHECK(cfg.height == 1080);
}

// S3 — Engine construction and lifecycle tests

TEST_CASE("Entry: Engine constructs in headless mode without crash") {
    EngineConfig cfg;
    cfg.headless = true;
    Engine engine(std::move(cfg));
    CHECK(true);
}

TEST_CASE("U4: Engine applies host save encryption policy after configuration move") {
    EngineConfig config;
    config.headless = true;
    config.saveEncryptionPolicy = SaveEncryptionPolicy::RequireEncrypted;
    EngineConfig moved(std::move(config));
    Engine engine(std::move(moved));
    REQUIRE(engine.init());
    auto* saves = BackendRegistry::instance().getSaveManager();
    REQUIRE(saves != nullptr);
    CHECK(saves->getEncryptionPolicy() == SaveEncryptionPolicy::RequireEncrypted);
    CHECK_FALSE(saves->isEncryptionEnabled());
    engine.shutdown();
}

TEST_CASE("Entry: Engine default construct then destruct without init") {
    Caesura::Test::LifecycleProbe platform;
    Caesura::Test::LifecycleProbe render;
    Caesura::Test::LifecycleProbe audio;
    Caesura::Test::LifecycleProbe miniGame;
    Caesura::Test::LifecycleProbe animation;
    Caesura::Test::ServiceProbe layer;
    Caesura::Test::ServiceProbe sandbox;
    int gpuMonitorDestructors = 0;

    EngineConfig cfg;
    cfg.headless = true;
    cfg.platform = new Caesura::Test::PlatformBackend(platform);
    cfg.render = new Caesura::Test::RenderDevice(render);
    cfg.audio = new Caesura::Test::AudioBackend(audio);
    cfg.miniGame = new Caesura::Test::MiniGameBackend(miniGame);
    cfg.animation = new Caesura::Test::AnimationBackend(animation);
    cfg.layerManager = new Caesura::Test::LayerManagerBackend(layer);
    cfg.sandboxQuota = new Caesura::Test::SandboxQuotaBackend(sandbox);
    cfg.gpuMonitor = new TestGpuMonitor(&gpuMonitorDestructors);
    {
        Engine engine(std::move(cfg));
        CHECK(cfg.platform == nullptr);
        CHECK(cfg.render == nullptr);
        CHECK(cfg.audio == nullptr);
        CHECK(cfg.miniGame == nullptr);
        CHECK(cfg.animation == nullptr);
        CHECK(cfg.layerManager == nullptr);
        CHECK(cfg.sandboxQuota == nullptr);
        CHECK(cfg.gpuMonitor == nullptr);
    }

    CHECK(platform.destructorCalls == 1);
    CHECK(render.destructorCalls == 1);
    CHECK(audio.destructorCalls == 1);
    CHECK(miniGame.destructorCalls == 1);
    CHECK(animation.destructorCalls == 1);
    CHECK(layer.destructorCalls == 1);
    CHECK(sandbox.destructorCalls == 1);
    CHECK(gpuMonitorDestructors == 1);
    CHECK(platform.initCalls == 0);
    CHECK(render.initCalls == 0);
    CHECK(audio.initCalls == 0);
    CHECK(platform.shutdownCalls == 0);
    CHECK(render.shutdownCalls == 0);
    CHECK(audio.shutdownCalls == 0);

    // Construct again to verify BackendRegistry state is clean
    EngineConfig cleanCfg;
    cleanCfg.headless = true;
    Engine engine2(std::move(cleanCfg));
    CHECK(true);
}

TEST_CASE("Entry: unconsumed EngineConfig releases injected ownership") {
    Caesura::Test::LifecycleProbe platform;
    Caesura::Test::LifecycleProbe render;
    Caesura::Test::LifecycleProbe audio;
    Caesura::Test::LifecycleProbe miniGame;
    Caesura::Test::LifecycleProbe animation;
    Caesura::Test::ServiceProbe layer;
    Caesura::Test::ServiceProbe sandbox;
    int gpuMonitorDestructors = 0;

    {
        EngineConfig cfg;
        cfg.platform = new Caesura::Test::PlatformBackend(platform);
        cfg.render = new Caesura::Test::RenderDevice(render);
        cfg.audio = new Caesura::Test::AudioBackend(audio);
        cfg.miniGame = new Caesura::Test::MiniGameBackend(miniGame);
        cfg.animation = new Caesura::Test::AnimationBackend(animation);
        cfg.layerManager = new Caesura::Test::LayerManagerBackend(layer);
        cfg.sandboxQuota = new Caesura::Test::SandboxQuotaBackend(sandbox);
        cfg.gpuMonitor = new TestGpuMonitor(&gpuMonitorDestructors);
    }

    CHECK(platform.destructorCalls == 1);
    CHECK(render.destructorCalls == 1);
    CHECK(audio.destructorCalls == 1);
    CHECK(miniGame.destructorCalls == 1);
    CHECK(animation.destructorCalls == 1);
    CHECK(layer.destructorCalls == 1);
    CHECK(sandbox.destructorCalls == 1);
    CHECK(gpuMonitorDestructors == 1);
    CHECK(platform.shutdownCalls == 0);
    CHECK(render.shutdownCalls == 0);
    CHECK(audio.shutdownCalls == 0);
}

TEST_CASE("Entry: injected display service transfers ownership and unregisters") {
    int displayDestructorCalls = 0;
    auto* display = new TestDisplayService(displayDestructorCalls);

    {
        EngineConfig cfg;
        cfg.headless = true;
        cfg.displayService = display;

        Engine engine(std::move(cfg));
        CHECK(engine.config().displayService == nullptr);
        REQUIRE(engine.init());
        CHECK(BackendRegistry::instance().getDisplayService() == display);

        engine.shutdown();
        CHECK(BackendRegistry::instance().getDisplayService() == nullptr);
        CHECK(displayDestructorCalls == 1);
    }

    CHECK(displayDestructorCalls == 1);
    checkEngineRegistryCleared();
}

TEST_CASE("Entry: default display service uses only SDL3 platform windows") {
    Caesura::Test::LifecycleProbe platform;
    EngineConfig cfg;
    cfg.width = 321;
    cfg.height = 123;
    cfg.headless = false;
    auto platformBackend = std::make_unique<Caesura::Test::PlatformBackend>(platform);

    // A non-SDL platform must not expose its native OS handle as SDL_Window*.
    auto display = Caesura::createDisplayService(cfg, platformBackend.get());
    REQUIRE(display != nullptr);
    DisplayMetrics metrics = display->currentMetrics();
    CHECK(metrics.logicalWidth == 321);
    CHECK(metrics.logicalHeight == 123);

    // The concrete SDL backend provides the actual SDL window accessor. An
    // uninitialized backend has no window, so metrics remain unavailable but
    // the call is safe and never casts the bgfx-native OS handle.
    SDL3PlatformBackend sdlPlatform;
    display = Caesura::createDisplayService(cfg, &sdlPlatform);
    REQUIRE(display != nullptr);
    metrics = display->currentMetrics();
    CHECK(metrics.logicalWidth == 0);
    CHECK(metrics.logicalHeight == 0);
}

TEST_CASE("Entry: Engine default construct then destruct") {
    // Test that default-constructed Engine (no init) destructs safely
    CHECK_NOTHROW({
        EngineConfig cfg;
        cfg.headless = true;
        Engine engine(std::move(cfg));
        // Destructor runs here — must not crash
    });
}

TEST_CASE("Entry: shutdown before init permanently rejects initialization") {
    Caesura::Test::LifecycleProbe platform;
    Caesura::Test::LifecycleProbe render;
    Caesura::Test::LifecycleProbe audio;

    {
        EngineConfig cfg;
        cfg.headless = true;
        cfg.platform = new Caesura::Test::PlatformBackend(platform);
        cfg.render = new Caesura::Test::RenderDevice(render);
        cfg.audio = new Caesura::Test::AudioBackend(audio);

        Engine engine(std::move(cfg));
        engine.shutdown();

        CHECK_FALSE(engine.init());
        CHECK(platform.initCalls == 0);
        CHECK(render.initCalls == 0);
        CHECK(audio.initCalls == 0);
        checkEngineRegistryCleared();
    }

    CHECK(platform.destructorCalls == 1);
    CHECK(render.destructorCalls == 1);
    CHECK(audio.destructorCalls == 1);
}

TEST_CASE("Entry: custom platform native handle is never treated as SDL window") {
    Caesura::Test::LifecycleProbe platform;
    platform.providedNativeHandle = reinterpret_cast<void*>(0x1234);
    Caesura::Test::PlatformBackend platformBackend(platform);

    CHECK(platformBackend.getNativeWindowHandle() == reinterpret_cast<void*>(0x1234));
    CHECK(Caesura::getSDLWindow(&platformBackend) == nullptr);
}

TEST_CASE("Entry: debugger attaches after Lua safety hook and detaches before VM shutdown") {
    Caesura::Test::ServiceProbe sandbox;
    lua_State* observedState = nullptr;
    lua_Hook safetyHook = nullptr;
    bool safetyHookRestoredBeforeUnbind = false;

    sandbox.onSetLuaState = [&](lua_State* state) {
        if (state) {
            observedState = state;
            safetyHook = lua_gethook(state);
        } else if (observedState) {
            safetyHookRestoredBeforeUnbind =
                lua_gethook(observedState) == safetyHook;
        }
    };

    EngineConfig cfg;
    cfg.headless = true;
    cfg.enableDebugger = true;
    cfg.sandboxQuota = new Caesura::Test::SandboxQuotaBackend(sandbox);

    Engine engine(std::move(cfg));
    REQUIRE(engine.init());
    REQUIRE(engine.debugProtocol() != nullptr);
    CHECK(engine.debugProtocol()->runState() == DebugProtocol::RunState::Running);
    CHECK(lua_gethook(engine.lua().state()) != safetyHook);

    lua_getglobal(engine.lua().state(), "_CAESURA_DEBUG_IS_PAUSED");
    REQUIRE(lua_isfunction(engine.lua().state(), -1));
    REQUIRE(lua_pcall(engine.lua().state(), 0, 1, 0) == LUA_OK);
    CHECK(lua_toboolean(engine.lua().state(), -1) == 0);
    lua_pop(engine.lua().state(), 1);

    constexpr const char* debugScript =
        "local value = 1\n"
        "value = value + 1\n"
        "return value\n";
    engine.debugProtocol()->setBreakpoint("entry_debug.lua", 2);
    lua_State* coroutine = lua_newthread(engine.lua().state());
    REQUIRE(luaL_loadbuffer(coroutine, debugScript, std::strlen(debugScript),
                            "entry_debug.lua") == LUA_OK);
    int resultCount = 0;
    REQUIRE(lua_resume(coroutine, engine.lua().state(), 0, &resultCount) == LUA_YIELD);

    lua_getglobal(engine.lua().state(), "_CAESURA_DEBUG_IS_PAUSED");
    REQUIRE(lua_pcall(engine.lua().state(), 0, 1, 0) == LUA_OK);
    CHECK(lua_toboolean(engine.lua().state(), -1) != 0);
    lua_pop(engine.lua().state(), 1);

    auto commands = engine.debugProtocol()->commandSink();
    REQUIRE(commands(engine.debugProtocol()->currentPauseId(),
                     DebugProtocol::Command::Continue));
    int ownerTicks = 0;
    engine.run([&]() {
        if (++ownerTicks >= 2) engine.quit();
    });
    CHECK(engine.debugProtocol()->runState() == DebugProtocol::RunState::Running);

    engine.shutdown();
    CHECK(engine.debugProtocol() == nullptr);
    CHECK(safetyHookRestoredBeforeUnbind);
    CHECK(sandbox.setLuaStateCalls == 2);
    checkEngineRegistryCleared();
}

TEST_CASE("Entry: voice completions are deferred by debugger pause and delivered once each") {
    Caesura::Test::LifecycleProbe audio;
    audio.voicePlaying = true;

    EngineConfig cfg;
    cfg.headless = true;
    cfg.enableDebugger = true;
    cfg.audio = new Caesura::Test::AudioBackend(audio);

    Engine engine(std::move(cfg));
    REQUIRE(engine.init());
    lua_State* L = engine.lua().state();
    REQUIRE(L != nullptr);
    REQUIRE(luaL_dostring(L,
        "voice_complete_count = 0; "
        "function _onVoiceComplete() "
        "voice_complete_count = voice_complete_count + 1 end") == LUA_OK);

    DebugProtocol* protocol = engine.debugProtocol();
    REQUIRE(protocol != nullptr);
    DebugProtocol::CommandSink commands;
    DebugProtocol::PauseId pauseId = DebugProtocol::NoPause;
    int ownerTick = 0;

    engine.run([&]() {
        ++ownerTick;
        if (ownerTick == 2) {
            constexpr const char* debugScript =
                "local value = 1\n"
                "value = value + 1\n"
                "return value\n";
            protocol->setBreakpoint("voice_debug.lua", 2);
            lua_State* coroutine = lua_newthread(L);
            REQUIRE(luaL_loadbuffer(coroutine, debugScript,
                                    std::strlen(debugScript),
                                    "voice_debug.lua") == LUA_OK);
            int resultCount = 0;
            REQUIRE(lua_resume(coroutine, L, 0, &resultCount) == LUA_YIELD);
            pauseId = protocol->currentPauseId();
            commands = protocol->commandSink();
            audio.voicePlaying = false;
            audio.voiceCompletions = 1;
        } else if (ownerTick == 3) {
            lua_getglobal(L, "voice_complete_count");
            CHECK(lua_tointeger(L, -1) == 0);
            lua_pop(L, 1);
            REQUIRE(pauseId != DebugProtocol::NoPause);
            REQUIRE(commands(pauseId, DebugProtocol::Command::Continue));
        } else if (ownerTick == 4) {
            lua_getglobal(L, "voice_complete_count");
            CHECK(lua_tointeger(L, -1) == 0);
            lua_pop(L, 1);
            audio.voicePlaying = true;
        } else if (ownerTick == 5) {
            lua_getglobal(L, "voice_complete_count");
            CHECK(lua_tointeger(L, -1) == 1);
            lua_pop(L, 1);
            lua_getglobal(L, "_CAESURA_VOICE_COMPLETE");
            CHECK(lua_toboolean(L, -1) == 0);
            lua_pop(L, 1);
            audio.voicePlaying = false;
            audio.voiceCompletions = 1;
        } else if (ownerTick >= 6) {
            engine.quit();
        }
    });

    lua_getglobal(L, "voice_complete_count");
    CHECK(lua_tointeger(L, -1) == 2);
    lua_pop(L, 1);
    lua_getglobal(L, "_CAESURA_VOICE_COMPLETE");
    CHECK(lua_toboolean(L, -1) != 0);
    lua_pop(L, 1);
    CHECK(audio.isVoicePlayingCalls >= 3);
    CHECK(audio.audioUpdateCalls >= 6);

    engine.shutdown();
    checkEngineRegistryCleared();
}

TEST_CASE("Entry: non-string voice callback errors are contained") {
    Caesura::Test::LifecycleProbe audio;
    EngineConfig cfg;
    cfg.headless = true;
    cfg.audio = new Caesura::Test::AudioBackend(audio);

    Engine engine(std::move(cfg));
    REQUIRE(engine.init());
    lua_State* L = engine.lua().state();
    REQUIRE(L != nullptr);
    REQUIRE(luaL_dostring(L,
        "voice_callback_entered = false; "
        "function _onVoiceComplete() "
        "voice_callback_entered = true; error({reason='test'}) end") == LUA_OK);

    audio.voiceCompletions = 1;
    int ownerTick = 0;
    CHECK_NOTHROW(engine.run([&]() {
        if (++ownerTick >= 2) engine.quit();
    }));

    lua_getglobal(L, "voice_callback_entered");
    CHECK(lua_toboolean(L, -1) != 0);
    lua_pop(L, 1);
    CHECK(audio.voiceCompletions == 0);

    engine.shutdown();
    checkEngineRegistryCleared();
}

TEST_CASE("Entry: consecutive engines own independent HotReload state") {
    {
        EngineConfig cfg;
        cfg.headless = true;
        Engine engine(std::move(cfg));
        REQUIRE(engine.init());

        lua_State* L = engine.lua().state();
        REQUIRE(L != nullptr);
        const int firstReloadSetup = luaL_dostring(L,
            "engine_reload_marker = 0; package.loaded['kag'] = {}; "
            "package.preload['kag'] = function() engine_reload_marker = 1; return {} end");
        REQUIRE(firstReloadSetup == LUA_OK);
        CHECK(engine.reloadScriptsNow());
        lua_getglobal(L, "engine_reload_marker");
        CHECK(lua_tointeger(L, -1) == 1);
        lua_pop(L, 1);
        CHECK_NOTHROW(engine.quit());
    }

    {
        EngineConfig cfg;
        cfg.headless = true;
        Engine engine(std::move(cfg));
        REQUIRE(engine.init());

        lua_State* L = engine.lua().state();
        REQUIRE(L != nullptr);
        const int secondReloadSetup = luaL_dostring(L,
            "engine_reload_marker = 0; package.loaded['kag'] = {}; "
            "package.preload['kag'] = function() engine_reload_marker = 2; return {} end");
        REQUIRE(secondReloadSetup == LUA_OK);
        CHECK(engine.reloadScriptsNow());
        lua_getglobal(L, "engine_reload_marker");
        CHECK(lua_tointeger(L, -1) == 2);
        lua_pop(L, 1);
    }
}

TEST_CASE("Entry: Engine headless init uses safe default backends") {
    SUBCASE("default adapters initialize and register") {
        EngineConfig cfg;
        cfg.headless = true;

        Engine engine(std::move(cfg));

        REQUIRE(engine.init());
        CHECK(BackendRegistry::instance().getRenderDevice() != nullptr);
        CHECK(BackendRegistry::instance().getAudioBackend() != nullptr);
        CHECK(BackendRegistry::instance().getPlatformBackend() != nullptr);
        CHECK(BackendRegistry::instance().getMiniGameBackend() != nullptr);
        CHECK(BackendRegistry::instance().getLayerManager() != nullptr);
        CHECK(BackendRegistry::instance().getSandboxQuota() != nullptr);
    }

    SUBCASE("injected adapters initialize once and shut down once") {
        Caesura::Test::LifecycleProbe platform;
        Caesura::Test::LifecycleProbe render;
        Caesura::Test::LifecycleProbe audio;
        bool meshReleasedBeforeRenderShutdown = false;
        render.onShutdown = [&] {
            meshReleasedBeforeRenderShutdown =
                BackendRegistry::instance().getMeshRenderer() == nullptr;
        };
        platform.providedNativeHandle = reinterpret_cast<void*>(0x1234);
        {
            EngineConfig cfg;
            cfg.headless = true;
            cfg.platform = new Caesura::Test::PlatformBackend(platform);
            cfg.render = new Caesura::Test::RenderDevice(render);
            cfg.audio = new Caesura::Test::AudioBackend(audio);

            Engine engine(std::move(cfg));
            REQUIRE(engine.init());
            CHECK_FALSE(engine.init());
            CHECK(platform.initCalls == 1);
            CHECK(render.initCalls == 1);
            CHECK(audio.initCalls == 1);
            CHECK(render.observedNativeHandle == nullptr);
            CHECK(engine.config().platform == nullptr);
            CHECK(engine.config().render == nullptr);
            CHECK(engine.config().audio == nullptr);
        }

        CHECK(platform.shutdownCalls == 1);
        CHECK(render.beginShutdownCalls == 1);
        CHECK(render.flushCalls == 1);
        CHECK(render.advanceCalls == 2);
        CHECK(render.shutdownCalls == 1);
        CHECK(meshReleasedBeforeRenderShutdown);
        CHECK(audio.shutdownCalls == 1);
        CHECK(platform.destructorCalls == 1);
        CHECK(render.destructorCalls == 1);
        CHECK(audio.destructorCalls == 1);
    }

    SUBCASE("owned layer and sandbox services register and unbind") {
        Caesura::Test::LifecycleProbe audio;
        Caesura::Test::LifecycleProbe animation;
        Caesura::Test::ServiceProbe layer;
        Caesura::Test::ServiceProbe sandbox;
        bool layerUnregisteredBeforeSandboxUnbind = false;
        bool audioReleasedBeforeSandboxUnbind = false;
        bool animationReleasedBeforeTextureAndSandbox = false;
        ISandboxQuota* configuredSandbox = nullptr;
        audio.onShutdown = [&] {
            audioReleasedBeforeSandboxUnbind =
                BackendRegistry::instance().getSandboxQuota() == configuredSandbox &&
                sandbox.lastLuaState != nullptr;
        };
        animation.onShutdown = [&] {
            auto& registry = BackendRegistry::instance();
            const bool quotaAvailable = registry.tryAlloc("textures");
            if (quotaAvailable) registry.release("textures");
            animationReleasedBeforeTextureAndSandbox =
                quotaAvailable &&
                registry.getTextureManager() != nullptr &&
                registry.getSandboxQuota() == configuredSandbox &&
                sandbox.lastLuaState != nullptr;
        };
        sandbox.onSetLuaState = [&](lua_State* L) {
            if (!L) {
                layerUnregisteredBeforeSandboxUnbind =
                    BackendRegistry::instance().getLayerManager() == nullptr &&
                    BackendRegistry::instance().getAnimationBackend() == nullptr &&
                    BackendRegistry::instance().getTextureManager() == nullptr &&
                    audio.shutdownCalls == 1 &&
                    animation.shutdownCalls == 1;
            }
        };
        {
            EngineConfig cfg;
            cfg.headless = true;
            auto* configuredLayer = new Caesura::Test::LayerManagerBackend(layer);
            configuredSandbox = new Caesura::Test::SandboxQuotaBackend(sandbox);
            cfg.audio = new Caesura::Test::AudioBackend(audio);
            cfg.animation = new Caesura::Test::AnimationBackend(animation);
            cfg.layerManager = configuredLayer;
            cfg.sandboxQuota = configuredSandbox;

            Engine engine(std::move(cfg));
            REQUIRE(engine.init());
            CHECK(cfg.layerManager == nullptr);
            CHECK(cfg.sandboxQuota == nullptr);
            CHECK(BackendRegistry::instance().getLayerManager() == configuredLayer);
            CHECK(BackendRegistry::instance().getSandboxQuota() == configuredSandbox);
            CHECK(layer.initCalls == 1);
            CHECK(sandbox.setLuaStateCalls == 1);
            CHECK(sandbox.lastLuaState == engine.lua().state());
        }

        CHECK(layer.shutdownCalls == 1);
        CHECK(layer.destructorCalls == 1);
        CHECK(sandbox.setLuaStateCalls == 2);
        CHECK(sandbox.lastLuaState == nullptr);
        CHECK(sandbox.destructorCalls == 1);
        CHECK(audio.shutdownCalls == 1);
        CHECK(animation.initCalls == 1);
        CHECK(animation.shutdownCalls == 1);
        CHECK(animation.destructorCalls == 1);
        CHECK(sandbox.tryAllocCalls == 1);
        CHECK(sandbox.releaseCalls == 1);
        CHECK(audioReleasedBeforeSandboxUnbind);
        CHECK(animationReleasedBeforeTextureAndSandbox);
        CHECK(layerUnregisteredBeforeSandboxUnbind);
        checkEngineRegistryCleared();
    }

    SUBCASE("platform failure rolls back without touching uninitialized adapters") {
        Caesura::Test::LifecycleProbe platform;
        Caesura::Test::LifecycleProbe render;
        Caesura::Test::LifecycleProbe audio;
        Caesura::Test::ServiceProbe layer;
        Caesura::Test::ServiceProbe sandbox;
        platform.initResult = false;
        {
            EngineConfig cfg;
            cfg.headless = true;
            cfg.platform = new Caesura::Test::PlatformBackend(platform);
            cfg.render = new Caesura::Test::RenderDevice(render);
            cfg.audio = new Caesura::Test::AudioBackend(audio);
            cfg.layerManager = new Caesura::Test::LayerManagerBackend(layer);
            cfg.sandboxQuota = new Caesura::Test::SandboxQuotaBackend(sandbox);

            Engine engine(std::move(cfg));
            CHECK_FALSE(engine.init());
            CHECK_FALSE(engine.init());
            CHECK(platform.initCalls == 1);
            CHECK(render.initCalls == 0);
            CHECK(audio.initCalls == 0);
            CHECK(platform.shutdownCalls == 0);
            CHECK(render.beginShutdownCalls == 0);
            CHECK(render.shutdownCalls == 0);
            CHECK(layer.initCalls == 0);
            CHECK(layer.shutdownCalls == 0);
            CHECK(sandbox.setLuaStateCalls == 0);
            CHECK_THROWS_AS(engine.renderDevice(), std::logic_error);
            CHECK_THROWS_AS(engine.audio(), std::logic_error);
            CHECK_THROWS_AS(engine.platform(), std::logic_error);
            CHECK_NOTHROW(engine.renderOneFrame());
            CHECK(engine.captureFrameForRpc(1, 1).empty());
            checkEngineRegistryCleared();
        }

        CHECK(platform.destructorCalls == 1);
        CHECK(render.destructorCalls == 1);
        CHECK(audio.destructorCalls == 1);
        CHECK(layer.initCalls == 0);
        CHECK(layer.shutdownCalls == 0);
        CHECK(layer.destructorCalls == 1);
        CHECK(sandbox.setLuaStateCalls == 0);
        CHECK(sandbox.destructorCalls == 1);
        checkEngineRegistryCleared();
    }

    SUBCASE("render failure shuts down only the initialized platform") {
        Caesura::Test::LifecycleProbe platform;
        Caesura::Test::LifecycleProbe render;
        Caesura::Test::LifecycleProbe audio;
        render.initResult = false;
        {
            EngineConfig cfg;
            cfg.headless = true;
            cfg.platform = new Caesura::Test::PlatformBackend(platform);
            cfg.render = new Caesura::Test::RenderDevice(render);
            cfg.audio = new Caesura::Test::AudioBackend(audio);

            Engine engine(std::move(cfg));
            CHECK_FALSE(engine.init());
            CHECK(platform.shutdownCalls == 1);
            CHECK(render.initCalls == 1);
            CHECK(render.beginShutdownCalls == 0);
            CHECK(render.flushCalls == 0);
            CHECK(render.advanceCalls == 0);
            CHECK(render.shutdownCalls == 0);
            CHECK(audio.initCalls == 0);
            checkEngineRegistryCleared();
        }
    }

    SUBCASE("audio failure degrades to the silent backend and keeps running (t56)") {
        Caesura::Test::LifecycleProbe platform;
        Caesura::Test::LifecycleProbe render;
        Caesura::Test::LifecycleProbe audio;
        audio.initResult = false;
        {
            EngineConfig cfg;
            cfg.headless = true;
            cfg.platform = new Caesura::Test::PlatformBackend(platform);
            cfg.render = new Caesura::Test::RenderDevice(render);
            cfg.audio = new Caesura::Test::AudioBackend(audio);

            Engine engine(std::move(cfg));
            // t56: a failing real audio backend must NOT kill startup -- the
            // engine degrades to the silent backend and keeps the run alive
            // (macOS vendored-SoLoud no-backend regression).
            CHECK(engine.init());
            CHECK(platform.initCalls == 1);
            CHECK(render.initCalls == 1);
            CHECK(audio.initCalls == 1);
            CHECK(audio.shutdownCalls == 0);   // failed backend replaced, not shutdown
            CHECK(BackendRegistry::instance().getAudioBackend() != nullptr);
            engine.shutdown();
            checkEngineRegistryCleared();      // shutdown still drains everything
        }
    }

    SUBCASE("editor mode without injected GPU backends is rejected") {
        EngineConfig cfg;
        cfg.headless = true;
        cfg.editorMode = true;

        Engine engine(std::move(cfg));
        CHECK_FALSE(engine.init());
        checkEngineRegistryCleared();
    }

    SUBCASE("failed optional adapters are cleaned before fallback replacement") {
        Caesura::Test::LifecycleProbe miniGame;
        Caesura::Test::LifecycleProbe animation;
        miniGame.initResult = false;
        animation.initResult = false;

        EngineConfig cfg;
        cfg.headless = true;
        cfg.miniGame = new Caesura::Test::MiniGameBackend(miniGame);
        cfg.animation = new Caesura::Test::AnimationBackend(animation);

        Engine engine(std::move(cfg));
        REQUIRE(engine.init());
        CHECK(miniGame.initCalls == 1);
        CHECK(miniGame.shutdownCalls == 1);
        CHECK(miniGame.destructorCalls == 1);
        CHECK(animation.initCalls == 1);
        CHECK(animation.shutdownCalls == 1);
        CHECK(animation.destructorCalls == 1);
        CHECK(engine.config().miniGame == nullptr);
        CHECK(engine.config().animation == nullptr);
    }
}

TEST_CASE("Entry: Engine uses configured GPU monitor") {
    EngineConfig cfg;
    cfg.headless = true;
    auto* configured = new TestGpuMonitor();
    cfg.gpuMonitor = configured;

    Engine engine(std::move(cfg));

    REQUIRE(engine.init());
    CHECK(&engine.gpuMonitor() == configured);
}

TEST_CASE("Entry: Engine headless backend info reports actual null adapters") {
    EngineConfig cfg;
    cfg.headless = true;

    Engine engine(std::move(cfg));

    REQUIRE(engine.init());

    lua_State* L = engine.lua().state();
    REQUIRE(L != nullptr);

    const char* script =
        "local info = Engine.get_backend_info()\n"
        "assert(info.render == 'NullRender', 'render=' .. tostring(info.render))\n"
        "assert(info.audio == 'NullAudio', 'audio=' .. tostring(info.audio))\n"
        "assert(info.platform == 'NullPlatform', 'platform=' .. tostring(info.platform))\n";

    int luaStatus = luaL_loadstring(L, script);
    if (luaStatus == LUA_OK) {
        luaStatus = lua_pcall(L, 0, 0, 0);
    }
    if (luaStatus != LUA_OK) {
        CAPTURE(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    CHECK(luaStatus == LUA_OK);
}

TEST_CASE("Entry: Engine headless Lua registry services back KAG bindings") {
    EngineConfig cfg;
    cfg.headless = true;

    Engine engine(std::move(cfg));
    REQUIRE(engine.init());

    lua_State* L = engine.lua().state();
    REQUIRE(L != nullptr);

    const char* registryKeys[] = {
        "Caesura.RenderDevice",
        "Caesura.AudioBackend",
        "Caesura.PlatformBackend",
        "Caesura.InputRouter",
        "Caesura.VideoPlayer",
        "Caesura.TextureManager",
        "Caesura.AsyncLoader",
        "Caesura.DebugManager",
        "Caesura.MiniGameBackend",
    };

    for (const char* key : registryKeys) {
        lua_getfield(L, LUA_REGISTRYINDEX, key);
        CAPTURE(key);
        CHECK(lua_islightuserdata(L, -1));
        lua_pop(L, 1);
    }

    auto runLuaBoolean = [&](const char* script) {
        INFO(script);
        int luaStatus = luaL_loadstring(L, script);
        if (luaStatus == LUA_OK) {
            luaStatus = lua_pcall(L, 0, 1, 0);
        }
        if (luaStatus != LUA_OK) {
            CAPTURE(lua_tostring(L, -1));
            lua_pop(L, 1);
            return false;
        }
        const bool ok = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return ok;
    };

    CHECK(runLuaBoolean("return KAG.set_bus_volume('bgm', 0.5)"));
    CHECK(runLuaBoolean("return type(KAG.get_bus_volume('bgm')) == 'number'"));
    CHECK(runLuaBoolean("return KAG.render_text('registry probe', 1, 2, 255, 255, 255, 255)"));
    CHECK(runLuaBoolean("return type(KAG.save_game) == 'function'"));
    CHECK(runLuaBoolean("return type(KAG.load_game) == 'function'"));
    CHECK(runLuaBoolean("return type(KAG.list_saves) == 'function'"));
}

TEST_CASE("Entry: mobile adapter registered, lifecycle callbacks work, unregistered on shutdown") {
    EngineConfig cfg;
    cfg.headless = true;
    Engine engine(std::move(cfg));
    REQUIRE(engine.init());

    // Adapter is registered via BackendRegistry (interface access).
    auto* adapter = BackendRegistry::instance().getMobileAdapter();
    REQUIRE(adapter != nullptr);
    CHECK_FALSE(adapter->isPaused());

    // Lifecycle callbacks through the interface drive the pause state.
    // (SDL app-lifecycle events are delivered via event watches and are
    // not pushable from tests; the Engine::appLifecycleWatch bridge is
    // verified by review.)
    adapter->onPause(nullptr);
    CHECK(adapter->isPaused());
    adapter->onResume(nullptr);
    CHECK_FALSE(adapter->isPaused());

    engine.shutdown();
    CHECK(BackendRegistry::instance().getMobileAdapter() == nullptr);
}

TEST_CASE("Entry: Engine::handleAppLifecycle maps SDL app events to adapter callbacks") {
    EngineConfig cfg;
    cfg.headless = true;
    Engine engine(std::move(cfg));
    REQUIRE(engine.init());
    auto* adapter = BackendRegistry::instance().getMobileAdapter();
    REQUIRE(adapter != nullptr);

    // Pure static mapping — no SDL event push needed (app events are
    // watch-delivered only and cannot be queued from tests).
    CHECK_FALSE(adapter->isPaused());
    Engine::handleAppLifecycle(adapter, nullptr, SDL_EVENT_WILL_ENTER_BACKGROUND);
    CHECK(adapter->isPaused());
    Engine::handleAppLifecycle(adapter, nullptr, SDL_EVENT_DID_ENTER_FOREGROUND);
    CHECK_FALSE(adapter->isPaused());

    // Unrelated event types and null adapter are safe no-ops.
    Engine::handleAppLifecycle(adapter, nullptr, SDL_EVENT_KEY_DOWN);
    CHECK_FALSE(adapter->isPaused());
    Engine::handleAppLifecycle(nullptr, nullptr, SDL_EVENT_WILL_ENTER_BACKGROUND);

    engine.shutdown();
}

TEST_CASE("U8: EngineConfig preserves publisher policy and value-owned key across moves") {
    Caesura::Test::PublisherArchives files;
    EngineConfig config;
    config.archiveTrustMode = ArchiveTrustMode::PinnedPublisher;
    config.archivePublisherKey = files.trustedKey;
    EngineConfig moved(std::move(config));
    CHECK(moved.archiveTrustMode == ArchiveTrustMode::PinnedPublisher);
    REQUIRE(moved.archivePublisherKey.has_value());
    CHECK(*moved.archivePublisherKey == files.trustedKey);
    config.archivePublisherKey = files.attackerKey;
    CHECK(*moved.archivePublisherKey == files.trustedKey);
    Engine engine(std::move(moved));
    CHECK(engine.config().archiveTrustMode == ArchiveTrustMode::PinnedPublisher);
    CHECK(engine.config().archivePublisherKey == files.trustedKey);
}

TEST_CASE("U8: strict Engine init fails closed and rolls back every registered service") {
    Caesura::Test::PublisherArchives files;
    bool provideKey = true;
    SUBCASE("missing host key even with no archives") { provideKey = false; }
    SUBCASE("untrusted patch blocks base and loose fallback") {
        files.mount("base.carc", true);
        files.mount("patch.carc", false);
        std::ofstream(files.root / "u8_publisher_payload.txt") << "loose fallback";
    }
    SUBCASE("corrupt package blocks a previously staged trusted patch") {
        files.mount("patch.carc", true);
        std::ofstream(files.root / "base.carc", std::ios::binary) << "corrupt";
    }
    Caesura::Test::ScopedWorkingDirectory workingDirectory(files.root);
    EngineConfig config;
    config.headless = true;
    config.archiveTrustMode = ArchiveTrustMode::PinnedPublisher;
    if (provideKey) config.archivePublisherKey = files.trustedKey;
    Engine engine(std::move(config));
    CHECK_FALSE(engine.init());
    CHECK_FALSE(engine.init());
    CHECK_THROWS_AS(engine.lua(), std::logic_error);
    CHECK_THROWS_AS(engine.jobSystem(), std::logic_error);
    checkEngineRegistryCleared();
    CHECK_NOTHROW(engine.shutdown());
}

TEST_CASE("U8: strict Engine init accepts trusted archives before Lua startup configuration") {
    Caesura::Test::PublisherArchives files;
    files.mount("base.carc", true);
    files.mount("patch.carc", true);
    Caesura::Test::ScopedWorkingDirectory workingDirectory(files.root);
    EngineConfig config;
    config.headless = true;
    config.archiveTrustMode = ArchiveTrustMode::PinnedPublisher;
    config.archivePublisherKey = files.trustedKey;
    Engine engine(std::move(config));
    REQUIRE(engine.init());
    REQUIRE(luaL_dostring(engine.lua().state(),
        "config = { carc_verify_on_startup = false }") == LUA_OK);
    CHECK(engine.config().archiveTrustMode == ArchiveTrustMode::PinnedPublisher);
    CHECK(engine.config().archivePublisherKey == files.trustedKey);
    engine.shutdown();
    checkEngineRegistryCleared();
}

TEST_CASE("U17 Entry: overlapping pause reasons resume audio only when all clear") {
    Caesura::Test::LifecycleProbe audio;
    EngineConfig cfg;
    cfg.headless = true;
    cfg.audio = new Caesura::Test::AudioBackend(audio);
    Engine engine(std::move(cfg));
    REQUIRE(engine.init());
    auto* L = engine.lua().state();
    REQUIRE(luaL_dostring(L,
        "pauses=0; resumes=0; function onPause() pauses=pauses+1 end; "
        "function onResume() resumes=resumes+1 end") == LUA_OK);

    engine.onLifecycleEvent(LifecycleEvent::Background);
    CHECK(audio.audioSuspendCalls == 1);
    engine.onAudioFocusEvent(AudioFocusEvent::FocusLost);
    CHECK(audio.audioSuspendCalls == 1);
    engine.onLifecycleEvent(LifecycleEvent::Foreground);
    CHECK(audio.audioResumeCalls == 0);
    engine.onAudioFocusEvent(AudioFocusEvent::InterruptionBegin);
    CHECK(audio.audioSuspendCalls == 1);
    engine.onAudioFocusEvent(AudioFocusEvent::FocusGained);
    CHECK(audio.audioResumeCalls == 0);
    engine.onAudioFocusEvent(AudioFocusEvent::InterruptionEnd);
    CHECK(audio.audioResumeCalls == 1);
    engine.onLifecycleEvent(LifecycleEvent::Foreground);
    engine.onAudioFocusEvent(AudioFocusEvent::InterruptionEnd);
    CHECK(audio.audioResumeCalls == 1);

    engine.onLifecycleEvent(LifecycleEvent::Pause);
    engine.onLifecycleEvent(LifecycleEvent::Background);
    CHECK(audio.audioSuspendCalls == 2);
    engine.onLifecycleEvent(LifecycleEvent::Resume);
    CHECK(audio.audioResumeCalls == 1);
    engine.onLifecycleEvent(LifecycleEvent::Foreground);
    CHECK(audio.audioResumeCalls == 2);
    REQUIRE(luaL_dostring(L, "assert(pauses == 2 and resumes == 2)") == LUA_OK);
    engine.shutdown();
    checkEngineRegistryCleared();
}

namespace {
class U17ScopedSDLHint {
public:
    U17ScopedSDLHint(const char* name, const char* value) : m_name(name) {
        if (const char* old = SDL_GetHint(name)) { m_hadValue = true; m_value = old; }
        REQUIRE(SDL_SetHintWithPriority(name, value, SDL_HINT_OVERRIDE));
    }
    ~U17ScopedSDLHint() {
        if (m_hadValue) SDL_SetHintWithPriority(m_name, m_value.c_str(), SDL_HINT_OVERRIDE);
        else SDL_ResetHint(m_name);
    }
private:
    const char* m_name;
    bool m_hadValue = false;
    std::string m_value;
};

class U17LogicalRender final : public Caesura::Test::RenderDevice {
public:
    using Caesura::Test::RenderDevice::RenderDevice;
    int getBackbufferWidth() const override { return 200; }
    int getBackbufferHeight() const override { return 100; }
};
}

TEST_CASE("U17 Platform: SDL window dimensions follow external resize without backend polling") {
    REQUIRE((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0);
    U17ScopedSDLHint video(SDL_HINT_VIDEO_DRIVER, "dummy");
    U17ScopedSDLHint audio(SDL_HINT_AUDIO_DRIVER, "dummy");
    SDL3PlatformBackend backend;
    REQUIRE(backend.init("U17 dummy SDL dimensions", 400, 200));
    REQUIRE(std::strcmp(SDL_GetCurrentVideoDriver(), "dummy") == 0);
    REQUIRE(SDL_SetWindowSize(backend.window(), 800, 400));
    REQUIRE(SDL_SyncWindow(backend.window()));
    int actualWidth = 0, actualHeight = 0;
    REQUIRE(SDL_GetWindowSize(backend.window(), &actualWidth, &actualHeight));
    REQUIRE(actualWidth == 800);
    REQUIRE(actualHeight == 400);
    // Engine, not this backend's pollEvent(), owns the event pump.
    CHECK(backend.getWindowWidth() == actualWidth);
    CHECK(backend.getWindowHeight() == actualHeight);
    backend.shutdown();
}

TEST_CASE("U17 Entry: queued click keeps its event coordinates after resize and later motion") {
    REQUIRE((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0);
    U17ScopedSDLHint video(SDL_HINT_VIDEO_DRIVER, "dummy");
    U17ScopedSDLHint audioDriver(SDL_HINT_AUDIO_DRIVER, "dummy");
    Caesura::Test::LifecycleProbe render, audio;
    EngineConfig cfg;
    cfg.width = 400; cfg.height = 200; cfg.frameLimit = 2;
    auto* platform = new SDL3PlatformBackend;
    cfg.platform = platform;
    cfg.render = new U17LogicalRender(render);
    cfg.audio = new Caesura::Test::AudioBackend(audio);
    cfg.gpuMonitor = new TestGpuMonitor;
    Engine engine(std::move(cfg));
    REQUIRE(engine.init());
    REQUIRE(std::strcmp(SDL_GetCurrentVideoDriver(), "dummy") == 0);
    lua_State* L = engine.lua().state();
    REQUIRE(luaL_dostring(L,
        "input_calls=0; function _KAG_onClick() input_calls=input_calls+1; "
        "click_x=_GAME_MOUSE_X; click_y=_GAME_MOUSE_Y end") == LUA_OK);
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    int ticks = 0;
    engine.run([&] {
        if (++ticks != 1) { engine.quit(); return; }
        REQUIRE(SDL_SetWindowSize(platform->window(), 800, 400));
        REQUIRE(SDL_SyncWindow(platform->window()));
        const SDL_WindowID window = SDL_GetWindowID(platform->window());
        SDL_Event event{};
        event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        event.button.windowID = window;
        event.button.button = SDL_BUTTON_LEFT;
        event.button.down = true;
        event.button.x = 400; event.button.y = 200;
        REQUIRE(SDL_PushEvent(&event));
        event = {};
        event.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.windowID = window;
        event.motion.state = SDL_BUTTON_LMASK;
        event.motion.x = 600; event.motion.y = 300;
        REQUIRE(SDL_PushEvent(&event));
        event = {};
        event.type = SDL_EVENT_MOUSE_BUTTON_UP;
        event.button.windowID = window;
        event.button.button = SDL_BUTTON_LEFT;
        event.button.down = false;
        event.button.x = 600; event.button.y = 300;
        REQUIRE(SDL_PushEvent(&event));
    });
    const auto number = [&](const char* name) {
        lua_getglobal(L, name);
        const double result = lua_tonumber(L, -1);
        lua_pop(L, 1);
        return result;
    };
    CHECK(number("input_calls") == 1);
    CHECK(number("click_x") == 100);
    CHECK(number("click_y") == 50);
    CHECK(number("_GAME_MOUSE_X") == 150);
    CHECK(number("_GAME_MOUSE_Y") == 75);
    lua_getglobal(L, "_GAME_MOUSE_DOWN");
    CHECK_FALSE(lua_toboolean(L, -1));
    lua_pop(L, 1);
    engine.shutdown();
    checkEngineRegistryCleared();
}

namespace {
// Real SDL queue and Engine event loop; only the GPU/audio devices are replaced.
struct U17InputHarness {
    U17ScopedSDLHint video{SDL_HINT_VIDEO_DRIVER, "dummy"};
    U17ScopedSDLHint audioDriver{SDL_HINT_AUDIO_DRIVER, "dummy"};
    Caesura::Test::LifecycleProbe renderProbe, audioProbe;
    SDL3PlatformBackend* platform = nullptr;
    std::unique_ptr<Engine> engine;
    lua_State* L = nullptr;
    U17InputHarness() {
        REQUIRE((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0);
        EngineConfig cfg;
        cfg.width = 400; cfg.height = 200; cfg.frameLimit = 16;
        cfg.platform = platform = new SDL3PlatformBackend;
        cfg.render = new U17LogicalRender(renderProbe);
        cfg.audio = new Caesura::Test::AudioBackend(audioProbe);
        cfg.gpuMonitor = new TestGpuMonitor;
        engine = std::make_unique<Engine>(std::move(cfg));
        REQUIRE(engine->init());
        REQUIRE(std::strcmp(SDL_GetCurrentVideoDriver(), "dummy") == 0);
        L = engine->lua().state();
        REQUIRE(luaL_dostring(L,
            "clicks=0; rights=0; updates=0; "
            "function _KAG_onClick() clicks=clicks+1; cx=_GAME_MOUSE_X; cy=_GAME_MOUSE_Y end; "
            "function _KAG_onRightClick() rights=rights+1 end; "
            "function engine_update() updates=updates+1 end") == LUA_OK);
        SDL_FlushEvents(SDL_EVENT_WINDOW_FIRST, SDL_EVENT_WINDOW_LAST);
        SDL_FlushEvents(SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_WHEEL);
        SDL_FlushEvents(SDL_EVENT_FINGER_DOWN, SDL_EVENT_FINGER_CANCELED);
    }
    ~U17InputHarness() { engine->shutdown(); }
    double number(const char* key) {
        lua_getglobal(L, key); const auto result = lua_tonumber(L, -1); lua_pop(L, 1); return result;
    }
    void finger(Uint32 type, SDL_TouchID touch, SDL_FingerID id, float x=.25f, float y=.5f) {
        SDL_Event event{}; event.type = type;
        event.tfinger.windowID = SDL_GetWindowID(platform->window());
        event.tfinger.touchID = touch; event.tfinger.fingerID = id;
        event.tfinger.x=x; event.tfinger.y=y;
        REQUIRE(SDL_PushEvent(&event));
    }
    void mouse(SDL_MouseID which=0) {
        SDL_Event event{}; event.type=SDL_EVENT_MOUSE_BUTTON_DOWN;
        event.button.windowID=SDL_GetWindowID(platform->window());
        event.button.which=which; event.button.button=SDL_BUTTON_LEFT;
        event.button.down=true; event.button.x=100; event.button.y=100;
        REQUIRE(SDL_PushEvent(&event));
    }
    void windowEvent(Uint32 type, SDL_WindowID id=0) {
        SDL_Event event{}; event.type=type;
        event.window.windowID = id ? id : SDL_GetWindowID(platform->window());
        REQUIRE(SDL_PushEvent(&event));
    }
};
}

TEST_CASE("U17 SDL route: full touch identifiers retain a short tap until release") {
    U17InputHarness h;
    constexpr SDL_TouchID device = (SDL_TouchID(1) << 42) + 13;
    constexpr SDL_FingerID finger = (SDL_FingerID(1) << 41) + 79;
    int tick=0;
    h.engine->run([&] {
        switch (++tick) {
        case 1: h.finger(SDL_EVENT_FINGER_DOWN, device, finger); break;
        case 2:
            CHECK(h.number("clicks") == 0);
            CHECK(BackendRegistry::instance().getMobileAdapter()->activeTouchCount() == 1);
            h.finger(SDL_EVENT_FINGER_UP, device, finger, .275f, .5f); break;
        case 3:
            CHECK(h.number("clicks") == 1); CHECK(h.number("cx") == doctest::Approx(55));
            CHECK(h.number("cy") == doctest::Approx(50));
            CHECK(BackendRegistry::instance().getMobileAdapter()->activeTouchCount() == 0);
            h.engine->quit(); break;
        }
    });
}

TEST_CASE("U17 SDL route: equal finger IDs from different devices cannot collapse into a tap") {
    U17InputHarness h;
    int tick=0;
    h.engine->run([&] {
        switch (++tick) {
        case 1:
            h.finger(SDL_EVENT_FINGER_DOWN, 3, 0); h.finger(SDL_EVENT_FINGER_DOWN, 4, 0); break;
        case 2:
            CHECK(h.number("clicks") == 0);
            CHECK(BackendRegistry::instance().getMobileAdapter()->activeTouchCount() == 2);
            h.finger(SDL_EVENT_FINGER_CANCELED, 9, 0); // Unknown device cannot cancel either owner.
            break;
        case 3:
            CHECK(BackendRegistry::instance().getMobileAdapter()->activeTouchCount() == 2);
            h.finger(SDL_EVENT_FINGER_UP, 3, 0); h.finger(SDL_EVENT_FINGER_UP, 4, 0); break;
        case 4:
            CHECK(h.number("clicks") == 0); CHECK(h.number("rights") == 1);
            CHECK(BackendRegistry::instance().getMobileAdapter()->activeTouchCount() == 0);
            h.engine->quit(); break;
        }
    });
}

TEST_CASE("U17 SDL route: canceled contact and automatic touch mouse never advance KAG") {
    U17InputHarness h;
    int tick=0;
    h.engine->run([&] {
        switch (++tick) {
        case 1:
            h.finger(SDL_EVENT_FINGER_DOWN, 3, 0);
            h.finger(SDL_EVENT_FINGER_CANCELED, 3, 0);
            h.mouse(SDL_TOUCH_MOUSEID); break;
        case 2:
            CHECK(h.number("clicks") == 0);
            CHECK(BackendRegistry::instance().getMobileAdapter()->activeTouchCount() == 0);
            h.finger(SDL_EVENT_FINGER_UP, 3, 0);
            h.finger(SDL_EVENT_FINGER_DOWN, 3, 0); h.finger(SDL_EVENT_FINGER_UP, 3, 0); break;
        case 3: CHECK(h.number("clicks") == 1); h.engine->quit(); break;
        }
    });
}

TEST_CASE("U17 SDL route: elapsed long press is classified before its queued release") {
    U17InputHarness h;
    int tick=0;
    h.engine->run([&] {
        switch (++tick) {
        case 1: h.finger(SDL_EVENT_FINGER_DOWN, 3, 0); break;
        case 2:
            CHECK(h.number("clicks") == 0);
            // Exercise the real duration clock, with a margin above the specified
            // threshold. This is a timed gesture, not an async race assumption.
            SDL_Delay(Uint32(GestureDetector::kLongPressMs + 25));
            h.finger(SDL_EVENT_FINGER_UP, 3, 0); break;
        case 3:
            CHECK(h.number("clicks") == 0); CHECK(h.number("rights") == 1);
            h.engine->quit(); break;
        }
    });
}

TEST_CASE("U17 SDL route: focus round trip invalidates the accepted click and held pointer") {
    U17InputHarness h;
    bool changed=false;
    h.engine->input().registerKAGCallback([&](const SDL_Event& e) {
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !changed) {
            changed=true;
            h.engine->input().setFocus(InputFocus::GAME);
            h.engine->input().setFocus(InputFocus::KAG);
        }
    });
    int tick=0;
    h.engine->run([&] {
        switch (++tick) {
        case 1: h.mouse(); break;
        case 2:
            CHECK(changed); CHECK(h.number("clicks") == 0);
            lua_getglobal(h.L, "_GAME_MOUSE_DOWN"); CHECK_FALSE(lua_toboolean(h.L,-1)); lua_pop(h.L,1);
            h.mouse(); break;
        case 3: CHECK(h.number("clicks") == 1); h.engine->quit(); break;
        }
    });
}

TEST_CASE("U17 SDL route: window loss and lifecycle reasons pause updates without replaying inputs") {
    U17InputHarness h;
    int tick=0; double pausedUpdates=0;
    h.engine->run([&] {
        switch (++tick) {
        case 1:
            h.windowEvent(SDL_EVENT_WINDOW_FOCUS_LOST, SDL_GetWindowID(h.platform->window())+100);
            break; // Foreign window is irrelevant.
        case 2:
            CHECK(h.number("updates") == 1);
            pausedUpdates=h.number("updates");
            h.mouse(); h.windowEvent(SDL_EVENT_WINDOW_FOCUS_LOST); break;
        case 3:
            CHECK(h.number("updates") == pausedUpdates); CHECK(h.number("clicks") == 0);
            CHECK(h.audioProbe.audioSuspendCalls == 1);
            h.engine->onLifecycleEvent(LifecycleEvent::Pause);
            h.engine->onLifecycleEvent(LifecycleEvent::Background);
            h.windowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED); break;
        case 4:
            CHECK(h.number("updates") == pausedUpdates);
            CHECK(h.audioProbe.audioResumeCalls == 0);
            h.engine->onLifecycleEvent(LifecycleEvent::Resume); break;
        case 5:
            CHECK(h.number("updates") == pausedUpdates);
            h.engine->onLifecycleEvent(LifecycleEvent::Foreground); break;
        case 6:
            CHECK(h.number("updates") == pausedUpdates+1); CHECK(h.number("clicks") == 0);
            CHECK(h.audioProbe.audioResumeCalls == 1);
            h.engine->onAudioFocusEvent(AudioFocusEvent::FocusLost); break;
        case 7:
            CHECK(h.number("updates") == pausedUpdates+2); // Audio focus alone leaves game logic running.
            h.engine->onAudioFocusEvent(AudioFocusEvent::FocusGained);
            h.engine->quit(); break;
        }
    });
}

TEST_CASE("U17 SDL route: GAME preserves immediate touch drag and raw contact delivery") {
    U17InputHarness h;
    h.engine->input().setFocus(InputFocus::GAME);
    int rawDown=0, rawMove=0, rawUp=0, mouseDown=0, mouseMove=0, mouseUp=0;
    h.engine->input().registerGameCallback([&](const SDL_Event& event) {
        if (event.type == SDL_EVENT_FINGER_DOWN) ++rawDown;
        if (event.type == SDL_EVENT_FINGER_MOTION) ++rawMove;
        if (event.type == SDL_EVENT_FINGER_UP) ++rawUp;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) ++mouseDown;
        if (event.type == SDL_EVENT_MOUSE_MOTION) {
            ++mouseMove; CHECK((event.motion.state & SDL_BUTTON_LMASK) != 0);
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) ++mouseUp;
    });
    int tick=0;
    h.engine->run([&] {
        switch (++tick) {
        case 1: h.finger(SDL_EVENT_FINGER_DOWN, 3, (SDL_FingerID(1)<<39)+100); break;
        case 2:
            CHECK(mouseDown == 1); CHECK(rawDown == 1); CHECK(h.number("clicks") == 0);
            h.finger(SDL_EVENT_FINGER_MOTION, 3, (SDL_FingerID(1)<<39)+100, .5f, .5f); break;
        case 3:
            CHECK(mouseMove == 1); CHECK(rawMove == 1);
            lua_getglobal(h.L,"_GAME_MOUSE_DOWN"); CHECK(lua_toboolean(h.L,-1)); lua_pop(h.L,1);
            h.finger(SDL_EVENT_FINGER_UP, 3, (SDL_FingerID(1)<<39)+100, .5f, .5f); break;
        case 4:
            CHECK(mouseUp == 1); CHECK(rawUp == 1); CHECK(h.number("clicks") == 0);
            h.engine->input().setFocus(InputFocus::KAG);
            h.finger(SDL_EVENT_FINGER_DOWN, 3, 0); h.finger(SDL_EVENT_FINGER_UP, 3, 0); break;
        case 5: CHECK(h.number("clicks") == 1); h.engine->quit(); break;
        }
    });
}

TEST_CASE("U17 SDL route: text and shortcut callbacks follow KAG focus and window ownership") {
    U17InputHarness h;
    REQUIRE(h.platform->startTextInput());
    REQUIRE(luaL_dostring(h.L,
        "texts=0; edits=0; keys=0; shortcuts=0; "
        "function _KAG_onTextInput(t) texts=texts+1; committed=t end; "
        "function _KAG_onTextEditing(t,s,n) edits=edits+1; composed=t; cs=s; cn=n end; "
        "function _KAG_onKeyDown() keys=keys+1 end; "
        "function _KAG_onCtrlDown() shortcuts=shortcuts+1 end; "
        "function _KAG_onKeySpace() shortcuts=shortcuts+1 end; "
        "function _KAG_onKeyPageUp() shortcuts=shortcuts+1 end") == LUA_OK);
    const auto send = [&](SDL_WindowID window) {
        SDL_Event event{};
        event.type=SDL_EVENT_TEXT_EDITING; event.edit.windowID=window;
        event.edit.text="漢字"; event.edit.start=1; event.edit.length=1;
        REQUIRE(SDL_PushEvent(&event));
        event={}; event.type=SDL_EVENT_TEXT_INPUT; event.text.windowID=window;
        event.text.text="名字"; REQUIRE(SDL_PushEvent(&event));
        for (const auto key : {SDLK_BACKSPACE, SDLK_RETURN, SDLK_ESCAPE, SDLK_LCTRL, SDLK_SPACE, SDLK_PAGEUP}) {
            event={}; event.type=SDL_EVENT_KEY_DOWN; event.key.windowID=window;
            event.key.key=key; event.key.down=true; REQUIRE(SDL_PushEvent(&event));
        }
    };
    const auto checkCounts = [&](int expected) {
        CHECK(h.number("texts") == expected); CHECK(h.number("edits") == expected);
        CHECK(h.number("keys") == 3*expected); CHECK(h.number("shortcuts") == 3*expected);
    };
    int tick=0;
    h.engine->run([&] {
        switch (++tick) {
        case 1: send(SDL_GetWindowID(h.platform->window())); break;
        case 2:
            checkCounts(1);
            CHECK(h.number("cs") == 1); CHECK(h.number("cn") == 1);
            REQUIRE(luaL_dostring(h.L,"assert(committed=='名字' and composed=='漢字')") == LUA_OK);
            h.engine->input().setFocus(InputFocus::GAME);
            send(SDL_GetWindowID(h.platform->window())); break;
        case 3:
            checkCounts(1);
            h.engine->input().setFocus(InputFocus::KAG);
            send(SDL_GetWindowID(h.platform->window())+100); break;
        case 4:
            checkCounts(1);
            send(SDL_GetWindowID(h.platform->window())); break;
        case 5: checkCounts(2); h.platform->stopTextInput(); h.engine->quit(); break;
        }
    });
}

TEST_CASE("U17 IME command: SDL composition keys do not prematurely finish the real input command") {
    bool cancel=false;
    SUBCASE("composition commit then independent form commit") { cancel=false; }
    SUBCASE("composition cancel then independent form cancel") { cancel=true; }
    U17InputHarness h;
    const int startResult = luaL_dostring(h.L,
        "package.path='scripts/?.lua;scripts/?/init.lua;'..package.path; "
        "ime_ctx={f={name='unchanged'},sf={},tf={},mp={},_session_active=true}; "
        "ime_co=coroutine.create(function() require('kag.commands.text').input(ime_ctx, "
        "{name='f.name',default='初',maxlen=4,btn_cancel='Cancel'}) end); "
        "local ok,err=coroutine.resume(ime_co); assert(ok,err); assert(ime_ctx._inputMode)");
    INFO("input startup: " << (startResult == LUA_OK ? "ok" : lua_tostring(h.L,-1)));
    if (startResult != LUA_OK) std::fprintf(stderr,"U17 input startup error: %s\n", lua_tostring(h.L,-1));
    REQUIRE(startResult == LUA_OK);
    REQUIRE(h.platform->isTextInputActive());
    const auto key = [&](SDL_Keycode code) {
        SDL_Event event{}; event.type=SDL_EVENT_KEY_DOWN;
        event.key.windowID=SDL_GetWindowID(h.platform->window());
        event.key.key=code; event.key.down=true; REQUIRE(SDL_PushEvent(&event));
    };
    int tick=0;
    h.engine->run([&] {
        switch (++tick) {
        case 1: {
            SDL_Event event{}; event.type=SDL_EVENT_TEXT_EDITING;
            event.edit.windowID=SDL_GetWindowID(h.platform->window());
            event.edit.text="候选"; event.edit.start=0; event.edit.length=2;
            REQUIRE(SDL_PushEvent(&event)); key(cancel ? SDLK_ESCAPE : SDLK_RETURN); break;
        }
        case 2: {
            CHECK(luaL_dostring(h.L,"assert(ime_ctx._inputMode and ime_ctx.waiting_input and ime_ctx.f.name=='unchanged')") == LUA_OK);
            if (lua_gettop(h.L) > 0) lua_settop(h.L,0);
            CHECK(h.platform->isTextInputActive());
            if (!cancel) {
                SDL_Event event{}; event.type=SDL_EVENT_TEXT_INPUT;
                event.text.windowID=SDL_GetWindowID(h.platform->window());
                event.text.text="中文"; REQUIRE(SDL_PushEvent(&event));
            }
            key(cancel ? SDLK_ESCAPE : SDLK_RETURN); break;
        }
        case 3:
            CHECK(luaL_dostring(h.L, cancel
                ? "assert(not ime_ctx._inputMode and not ime_ctx.waiting_input and ime_ctx.f.name=='unchanged')"
                : "assert(not ime_ctx._inputMode and not ime_ctx.waiting_input and ime_ctx.f.name=='初中文')") == LUA_OK);
            if (lua_gettop(h.L) > 0) lua_settop(h.L,0);
            CHECK_FALSE(h.platform->isTextInputActive());
            REQUIRE(luaL_dostring(h.L,"assert(coroutine.resume(ime_co)); assert(coroutine.status(ime_co)=='dead')") == LUA_OK);
            h.engine->quit(); break;
        }
    });
}

TEST_CASE("U17 review regression: Android transient focus loss and gain resume their own reason") {
    Caesura::Test::LifecycleProbe audio;
    EngineConfig cfg; cfg.headless=true;
    cfg.audio=new Caesura::Test::AudioBackend(audio);
    Engine engine(std::move(cfg)); REQUIRE(engine.init());
    auto* service=BackendRegistry::instance().getAudioFocusService(); REQUIRE(service);
    int cycles=0;
    for (const int lost : {-1,-2,-3}) {
        service->post(audioFocusEventForAndroidChange(lost));
        CHECK(audio.audioSuspendCalls == cycles+1);
        service->post(audioFocusEventForAndroidChange(1));
        ++cycles;
        CHECK(audio.audioResumeCalls == cycles);
        CHECK(service->currentState() == AudioFocusState::Normal);
    }
    service->post(AudioFocusEvent::InterruptionBegin);
    service->post(audioFocusEventForAndroidChange(-2));
    service->post(audioFocusEventForAndroidChange(1));
    CHECK(audio.audioResumeCalls == cycles);
    CHECK(service->currentState() == AudioFocusState::Interrupted);
    service->post(AudioFocusEvent::InterruptionEnd);
    CHECK(audio.audioResumeCalls == cycles+1);
    engine.shutdown();
}

TEST_CASE("U17 review regression: GAME receives balancing release during lifecycle cancellation") {
    U17InputHarness h;
    h.engine->input().setFocus(InputFocus::GAME);
    int downs=0, ups=0; bool held=false;
    h.engine->input().registerGameCallback([&](const SDL_Event& event) {
        if (event.type==SDL_EVENT_MOUSE_BUTTON_DOWN) { ++downs; held=true; }
        if (event.type==SDL_EVENT_MOUSE_BUTTON_UP) { ++ups; held=false; }
    });
    int tick=0;
    h.engine->run([&] {
        switch(++tick) {
        case 1: h.finger(SDL_EVENT_FINGER_DOWN, 3, 7); break;
        case 2:
            CHECK(downs==1); CHECK(held);
            h.engine->onLifecycleEvent(LifecycleEvent::Pause);
            CHECK(ups==1); CHECK_FALSE(held);
            h.finger(SDL_EVENT_FINGER_DOWN, 3, 8); break; // Background input is still blocked.
        case 3:
            CHECK(downs==1);
            h.engine->onLifecycleEvent(LifecycleEvent::Resume);
            h.finger(SDL_EVENT_FINGER_UP, 3, 7);
            h.finger(SDL_EVENT_FINGER_DOWN, 3, 9); h.finger(SDL_EVENT_FINGER_UP, 3, 9); break;
        case 4:
            CHECK(downs==2); CHECK(ups==2); CHECK_FALSE(held); h.engine->quit(); break;
        }
    });
}

TEST_CASE("U17 review regression: multi-contact hold never decays to one-finger long press") {
    int fingers=2;
    SUBCASE("two contact hold") { fingers=2; }
    SUBCASE("three contact hold") { fingers=3; }
    U17InputHarness h;
    REQUIRE(luaL_dostring(h.L,"skips=0; function _KAG_onCtrlDown() skips=skips+1 end") == LUA_OK);
    int tick=0;
    h.engine->run([&] {
        switch(++tick) {
        case 1:
            for(int i=0;i<fingers;++i) h.finger(SDL_EVENT_FINGER_DOWN, 3, i, .2f+.1f*i, .5f);
            break;
        case 2:
            SDL_Delay(Uint32(GestureDetector::kLongPressMs+25));
            for(int i=0;i<fingers;++i) h.finger(SDL_EVENT_FINGER_UP, 3, i, .2f+.1f*i, .5f);
            break;
        case 3:
            CHECK(h.number("clicks")==0); CHECK(h.number("rights")==0);
            CHECK(h.number("skips")== (fingers==3 ? 1 : 0));
            h.finger(SDL_EVENT_FINGER_DOWN, 3, 0); h.finger(SDL_EVENT_FINGER_UP, 3, 0); break;
        case 4: CHECK(h.number("clicks")==1); h.engine->quit(); break;
        }
    });
}

TEST_CASE("U17 review reentry: GAME release callback resume cannot publish a stale lifecycle pause") {
    U17InputHarness h;
    h.engine->input().setFocus(InputFocus::GAME);
    REQUIRE(luaL_dostring(h.L,"pauses=0; resumes=0; function onPause() pauses=pauses+1 end; function onResume() resumes=resumes+1 end") == LUA_OK);
    int releases=0;
    h.engine->input().registerGameCallback([&](const SDL_Event& event) {
        if(event.type==SDL_EVENT_MOUSE_BUTTON_UP) {
            ++releases;
            h.engine->onLifecycleEvent(LifecycleEvent::Resume);
        }
    });
    int tick=0;
    h.engine->run([&] {
        switch(++tick) {
        case 1: h.finger(SDL_EVENT_FINGER_DOWN, 3, 0); break;
        case 2:
            h.engine->onLifecycleEvent(LifecycleEvent::Pause);
            CHECK(releases==1);
            CHECK_FALSE(BackendRegistry::instance().getMobileAdapter()->isPaused());
            CHECK(h.number("pauses")==0); CHECK(h.number("resumes")==0);
            CHECK(h.audioProbe.audioSuspendCalls==0); CHECK(h.audioProbe.audioResumeCalls==0);
            break;
        case 3:
            CHECK(h.number("updates")==2);
            h.engine->onLifecycleEvent(LifecycleEvent::Pause);
            CHECK(h.number("pauses")==1);
            h.engine->onLifecycleEvent(LifecycleEvent::Resume);
            CHECK(h.number("resumes")==1);
            CHECK_FALSE(BackendRegistry::instance().getMobileAdapter()->isPaused());
            h.engine->quit(); break;
        }
    });
}

// Real SoLoud NULLDRIVER mix, no window/GPU/output-device or speaker evidence.

namespace {
constexpr unsigned u17MixRate = 48000;
constexpr unsigned u17MixChannels = 2;
constexpr unsigned u17MixBlock = 1024;
constexpr unsigned u17MixFrameLimit = 640;

// Observation only: each override forwards exactly once to the real backend.
// The Lua KAG.play_voice binding intentionally returns bool, not a voice handle.
// Record the actual handle here without adding a production getter or replacing
// KAGBinding, the mixer, culling, or Engine's completion consumer.
class U17ObservedManualMix final : public SoLoudAudioEngine {
public:
    U17ObservedManualMix() : SoLoudAudioEngine(OutputMode::ManualMix) {}
    unsigned int playVoice(const std::string& path) override {
        const auto handle = SoLoudAudioEngine::playVoice(path);
        lastVoice = handle;
        if (handle != 0) ++successfulPlays;
        return handle;
    }
    void stopVoice() override {
        ++voiceStops;
        SoLoudAudioEngine::stopVoice();
    }
    unsigned int consumeVoiceCompletions() override {
        const auto result = SoLoudAudioEngine::consumeVoiceCompletions();
        consumedNaturalCompletions += result;
        return result;
    }
    unsigned lastVoice = 0, successfulPlays = 0, voiceStops = 0;
    unsigned consumedNaturalCompletions = 0;
};

void u17MixLua(Engine& engine, const char* code) {
    auto& vm = engine.lua();
    vm.resetInstructionBudget();
    lua_State* state = vm.state();
    REQUIRE(state != nullptr);
    const int top = lua_gettop(state);
    const int status = luaL_dostring(state, code);
    std::string error;
    if (status != LUA_OK) {
        const char* message = lua_tostring(state, -1);
        error = message ? message : "non-string Lua error";
    }
    lua_settop(state, top);
    INFO(error);
    REQUIRE(status == LUA_OK);
}

lua_Integer u17MixInteger(lua_State* state, const char* name) {
    lua_getglobal(state, name);
    const bool integer = lua_isinteger(state, -1);
    const lua_Integer value = lua_tointeger(state, -1);
    lua_pop(state, 1);
    INFO(name);
    REQUIRE(integer);
    return value;
}

struct U17PcmObservation {
    unsigned blocks = 0;
    double energy = 0, peak = 0, maxPositionDrift = 0;
    bool finite = true, handleAlive = true, runnerHeld = true;
    void add(const std::array<float, u17MixBlock * u17MixChannels>& samples) {
        ++blocks;
        for (float sample : samples) {
            finite = finite && std::isfinite(sample);
            energy += double(sample) * sample;
            peak = (std::max)(peak, double(std::abs(sample)));
        }
    }
    void print(const char* phase) const {
        const double count = double(blocks) * u17MixBlock * u17MixChannels;
        std::printf("U17 ManualMix phase=%s frames=%u peak=%.9g rms=%.9g max_voice_position_drift=%.12g\n",
            phase, blocks * u17MixBlock, peak, count ? std::sqrt(energy / count) : 0,
            maxPositionDrift);
    }
};
} // namespace

TEST_CASE("U17 Entry: real ManualMix voice_wait and audio lifecycle (NULLDRIVER)") {
    bool explicitStop = false;
    SUBCASE("all overlapping reasons hold actual PCM and voice progress") {}
    SUBCASE("explicit stop has no completion; successor naturally completes") { explicitStop = true; }

    EngineConfig config;
    config.headless = true;
    config.frameLimit = u17MixFrameLimit;
    auto* audio = new U17ObservedManualMix;
    config.audio = audio; // Engine owns this real backend.
    Engine engine(std::move(config));
    REQUIRE(engine.init());
    auto& registry = BackendRegistry::instance();
    REQUIRE(registry.getAudioBackend() == audio); // Reject silent fallback.
    REQUIRE(registry.getAudioRestore() == audio);
    auto* lifecycle = registry.getLifecycleService();
    auto* focus = registry.getAudioFocusService();
    REQUIRE(lifecycle != nullptr);
    REQUIRE(focus != nullptr);
    REQUIRE(audio->soloud().getBackendId() == SoLoud::Soloud::NULLDRIVER);
    REQUIRE(audio->soloud().getBackendString() != nullptr);
    REQUIRE(audio->soloud().getBackendSamplerate() == u17MixRate);
    REQUIRE(audio->soloud().getBackendChannels() == u17MixChannels);
    std::printf("U17 ManualMix host=Engine audio_backend_id=%u audio_backend=%s rate=%u channels=%u block=%u headless=1 speaker_evidence=0\n",
        audio->soloud().getBackendId(), audio->soloud().getBackendString(),
        audio->soloud().getBackendSamplerate(), audio->soloud().getBackendChannels(), u17MixBlock);
    audio->setGlobalVolume(1);
    audio->setBusVolume("voice", 1);
    u17MixLua(engine, R"lua(
        package.path = 'scripts/?.lua;scripts/?/init.lua;' .. package.path
        local runner = require('kag_runner')
        u17_done, u17_beyond, u17_waiting, u17_updates = 0, 0, 0, 0
        u17_completions, u17_pause_callbacks, u17_resume_callbacks = 0, 0, 0
        function _onVoiceComplete() u17_completions = u17_completions + 1 end
        function onPause() u17_pause_callbacks = u17_pause_callbacks + 1 end
        function onResume() u17_resume_callbacks = u17_resume_callbacks + 1 end
        function u17_start_voice(replace)
            assert(runner.start('assets/script/tests/u17_voice_wait_manualmix.ks', {replace=replace}))
            local ctx = assert(runner.get_ctx())
            assert(ctx.f.u17_entered == 1 and ctx.f.u17_done == nil)
            ctx.auto_mode, ctx.skip_mode = false, nil
            u17_done, u17_beyond, u17_waiting = 0, 0, 0
            -- Session creation must precede play: session cleanup can stop audio.
            assert(KAG.play_voice('assets/voice/line01.wav'))
        end
        function engine_update(dt)
            u17_updates = u17_updates + 1
            runner.update(dt) -- Actual Engine callback; no test-side wait loop.
            local ctx = runner.get_ctx()
            u17_done = ctx and ctx.f.u17_done or 0
            u17_beyond = ctx and ctx.f.u17_beyond_page or 0
            u17_waiting = ctx and ctx.waiting_input and 1 or 0
        end
        u17_start_voice(false)
    )lua");
    lua_State* state = engine.lua().state();
    const unsigned firstVoice = audio->lastVoice;
    REQUIRE(firstVoice != 0);
    REQUIRE(audio->soloud().isValidVoiceHandle(firstVoice));
    REQUIRE(audio->isVoicePlaying());
    const unsigned initialStops = audio->voiceStops;

    unsigned ticks = 0, nativeEndTick = 0, doneTick = 0, heldAfterDone = 0;
    double pausedPosition = 0, resumedPosition = 0;
    bool stopHadNoCompletion = true, stoppedWaitReleased = false, oldHandleRetired = false;
    U17PcmObservation active, successor;
    std::array<U17PcmObservation, 4> held;
    std::array<float, u17MixBlock * u17MixChannels> samples{};

    engine.run([&] {
        ++ticks;
        if (ticks == 25) {
            CHECK(u17MixInteger(state, "u17_done") == 0);
            CHECK(u17MixInteger(state, "u17_waiting") == 1);
            REQUIRE(audio->soloud().isValidVoiceHandle(firstVoice));
            pausedPosition = audio->soloud().getStreamPosition(firstVoice);
            CHECK(pausedPosition > 0);
            if (explicitStop) {
                u17MixLua(engine, "KAG.stop_voice()");
                CHECK(audio->voiceStops == initialStops + 1);
            } else {
                lifecycle->post(LifecycleEvent::Background);
                lifecycle->post(LifecycleEvent::Pause);
                focus->post(AudioFocusEvent::FocusLost);
            }
        }
        if (!explicitStop) {
            if (ticks == 33) lifecycle->post(LifecycleEvent::Foreground);
            if (ticks == 41) lifecycle->post(LifecycleEvent::Resume);
            if (ticks == 49) {
                focus->post(AudioFocusEvent::InterruptionBegin);
                focus->post(AudioFocusEvent::FocusGained);
            }
            if (ticks == 57) focus->post(AudioFocusEvent::InterruptionEnd);
        } else {
            if (ticks >= 25 && ticks < 200) {
                stopHadNoCompletion = stopHadNoCompletion
                    && audio->consumedNaturalCompletions == 0
                    && u17MixInteger(state, "u17_completions") == 0;
            }
            if (ticks == 40) {
                stoppedWaitReleased = u17MixInteger(state, "u17_done") == 1
                    && u17MixInteger(state, "u17_waiting") == 1
                    && u17MixInteger(state, "u17_beyond") == 0;
                oldHandleRetired = !audio->soloud().isValidVoiceHandle(firstVoice);
            }
            if (ticks == 200) {
                CHECK(stopHadNoCompletion);
                CHECK(audio->consumedNaturalCompletions == 0);
                CHECK(u17MixInteger(state, "u17_completions") == 0);
                u17MixLua(engine, "u17_start_voice(true)");
                REQUIRE(audio->lastVoice != 0);
                REQUIRE(audio->lastVoice != firstVoice);
                nativeEndTick = doneTick = heldAfterDone = 0;
            }
        }

        // Only this owner-pump call advances the actual NULLDRIVER mixer.
        // Engine subsequently runs Lua, audio.update/culling and its real
        // completion delivery. Never call consumeVoiceCompletions here.
        audio->soloud().mix(samples.data(), u17MixBlock);
        if (!explicitStop && ticks >= 25 && ticks <= 56) {
            auto& phase = held[(ticks - 25) / 8];
            phase.add(samples);
            phase.maxPositionDrift = (std::max)(phase.maxPositionDrift,
                std::abs(audio->soloud().getStreamPosition(firstVoice) - pausedPosition));
            phase.handleAlive = phase.handleAlive && audio->soloud().isValidVoiceHandle(firstVoice);
            phase.runnerHeld = phase.runnerHeld && u17MixInteger(state, "u17_done") == 0
                && u17MixInteger(state, "u17_completions") == 0;
        } else active.add(samples);
        if (explicitStop && ticks >= 200) successor.add(samples);
        if (!explicitStop && ticks == 70)
            resumedPosition = audio->soloud().getStreamPosition(firstVoice);

        const bool finalVoice = !explicitStop || ticks >= 200;
        if (finalVoice && !audio->soloud().isValidVoiceHandle(audio->lastVoice) && nativeEndTick == 0)
            nativeEndTick = ticks;
        if (finalVoice && u17MixInteger(state, "u17_done") == 1) {
            if (doneTick == 0) doneTick = ticks;
            if (++heldAfterDone >= 4) engine.quit();
        }
    });

    active.print(explicitStop ? "active-and-stopped-tail" : "active-before-and-after-resume");
    CHECK(active.finite);
    CHECK(active.peak > 0.00001);
    CHECK(active.energy > 0.00000001);
    CHECK(ticks < u17MixFrameLimit);
    CHECK(nativeEndTick != 0);
    CHECK(doneTick >= nativeEndTick);
    CHECK(doneTick - nativeEndTick <= 3); // Scheduler return + following [set].
    CHECK(u17MixInteger(state, "u17_done") == 1);
    CHECK(u17MixInteger(state, "u17_waiting") == 1);
    CHECK(u17MixInteger(state, "u17_beyond") == 0);
    CHECK(u17MixInteger(state, "u17_completions") == 1);
    CHECK(audio->consumedNaturalCompletions == 1);
    CHECK_FALSE(audio->isVoicePlaying());
    if (explicitStop) {
        successor.print("successor-active");
        CHECK(successor.finite);
        CHECK(successor.peak > 0.00001);
        CHECK(successor.energy > 0.00000001);
        CHECK(stopHadNoCompletion);
        CHECK(stoppedWaitReleased);
        CHECK(oldHandleRetired);
        CHECK(audio->successfulPlays == 2);
    } else {
        const char* labels[] = {"background-pause-focus", "pause-focus", "focus-only", "interruption-only"};
        for (size_t i = 0; i < held.size(); ++i) {
            INFO(labels[i]);
            held[i].print(labels[i]);
            CHECK(held[i].blocks == 8);
            CHECK(held[i].finite);
            CHECK(held[i].peak <= 0.0000001);
            CHECK(held[i].maxPositionDrift <= 0.000000001);
            CHECK(held[i].handleAlive);
            CHECK(held[i].runnerHeld);
        }
        CHECK(resumedPosition > pausedPosition);
        CHECK(audio->successfulPlays == 1);
        CHECK(audio->voiceStops == initialStops);
        CHECK(u17MixInteger(state, "u17_pause_callbacks") == 1);
        CHECK(u17MixInteger(state, "u17_resume_callbacks") == 1);
    }
    std::printf("U17 ManualMix outcome explicit_stop=%d ticks=%u native_end_tick=%u command_done_tick=%u natural_completions=%u prefix_position=%.9g resumed_position=%.9g\n",
        int(explicitStop), ticks, nativeEndTick, doneTick, audio->consumedNaturalCompletions,
        pausedPosition, resumedPosition);
    engine.shutdown();
    checkEngineRegistryCleared();
}

TEST_CASE("U17 Entry: cached voice completions cannot cross a replaced runner owner") {
    EngineConfig cfg;
    cfg.headless = true;
    cfg.enableDebugger = true;
    auto* audio = new U17ObservedManualMix;
    cfg.audio = audio;
    Engine engine(std::move(cfg));
    REQUIRE(engine.init());
    REQUIRE(BackendRegistry::instance().getAudioBackend() == audio);
    REQUIRE(audio->soloud().getBackendId() == SoLoud::Soloud::NULLDRIVER);
    lua_State* L = engine.lua().state();
    u17MixLua(engine, R"lua(
        package.path = 'scripts/?.lua;scripts/?/init.lua;' .. package.path
        local runner = require('kag_runner')
        assert(runner.start('assets/script/tests/u17_voice_wait_manualmix.ks'))
        assert(KAG.play_voice('assets/voice/line01.wav'))
        u17_old_owner = runner.get_ctx()
        u17_old_owner.f.callbacks = 0
        function engine_update(dt) runner.update(dt) end
        function _onVoiceComplete()
            local owner = assert(runner.get_ctx())
            owner.f.callbacks = (owner.f.callbacks or 0) + 1
        end
    )lua");
    const auto finishCurrentVoice = [&] {
        std::array<float, u17MixBlock * u17MixChannels> samples{};
        REQUIRE(audio->soloud().isValidVoiceHandle(audio->lastVoice));
        for (unsigned block = 0; block < 120; ++block)
            audio->soloud().mix(samples.data(), u17MixBlock);
        REQUIRE_FALSE(audio->soloud().isValidVoiceHandle(audio->lastVoice));
    };
    auto* protocol = engine.debugProtocol();
    REQUIRE(protocol != nullptr);
    DebugProtocol::CommandSink commands;
    DebugProtocol::PauseId pause = DebugProtocol::NoPause;
    const auto callbacks = [&](const char* owner) {
        lua_getglobal(L, owner);
        REQUIRE(lua_istable(L, -1));
        lua_getfield(L, -1, "f");
        lua_getfield(L, -1, "callbacks");
        const auto count = lua_tointeger(L, -1);
        lua_pop(L, 3);
        return count;
    };
    unsigned tick = 0;
    engine.run([&] {
        ++tick;
        if (tick == 2) {
            constexpr const char* source = "local value=1\nvalue=value+1\nreturn value\n";
            protocol->setBreakpoint("u17_voice_owner_debug.lua", 2);
            lua_State* paused = lua_newthread(L);
            REQUIRE(luaL_loadbuffer(paused, source, std::strlen(source), "u17_voice_owner_debug.lua") == LUA_OK);
            int results = 0;
            REQUIRE(lua_resume(paused, L, 0, &results) == LUA_YIELD);
            pause = protocol->currentPauseId();
            commands = protocol->commandSink();
            finishCurrentVoice(); // Actual natural completion collected while paused.
        } else if (tick == 3) {
            CHECK(callbacks("u17_old_owner") == 0);
            CHECK(audio->consumedNaturalCompletions == 1); // Already consumed by Engine.
            u17MixLua(engine, R"lua(
                local runner = require('kag_runner')
                assert(runner.stop())
            )lua");
        } else if (tick == 4) {
            REQUIRE(pause != DebugProtocol::NoPause);
            REQUIRE(commands(pause, DebugProtocol::Command::Continue));
        } else if (tick == 5) {
            // A start during debugger pause is correctly rejected. The owner
            // may start B after resume, before this frame's completion drain.
            u17MixLua(engine, R"lua(
                local runner = require('kag_runner')
                assert(runner.start('assets/script/tests/u17_voice_wait_manualmix.ks'))
                assert(KAG.play_voice('assets/voice/line01.wav'))
                u17_new_owner = runner.get_ctx()
                assert(u17_new_owner ~= u17_old_owner)
                u17_new_owner.f.callbacks = 0
            )lua");
        } else if (tick == 6) {
            CHECK(callbacks("u17_new_owner") == 0);
            finishCurrentVoice(); // A legal new-owner natural completion.
        } else if (tick >= 7) {
            engine.quit();
        }
    });
    CHECK(callbacks("u17_old_owner") == 0);
    CHECK(callbacks("u17_new_owner") == 1);
    CHECK(audio->consumedNaturalCompletions == 2);
    engine.shutdown();
    checkEngineRegistryCleared();
}
