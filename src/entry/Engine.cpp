extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#include "Engine.h"
#include "../input/InputRouter.h"
#include "../di/BackendRegistry.h"
#include "../debug/DebugManager.h"
#include "ErrorUI.h"
#include "../audio/api/IAudioBackend.h"
#include "../audio/api/IAudioRestore.h"
#include "../platform/api/IDisplayService.h"
#include "../platform/LifecycleService.h"
#include "../audio/AudioFocusService.h"
#include "../di/api/ITextureBudget.h"
#include "../di/api/ISandboxQuota.h"
#include "../render/api/IRenderDevice.h"
#include "../render/api/ITextureManager.h"
#include "../render/api/ILayerManager.h"
#include "../render/VideoPlayer.h"
#include "../render/api/IGpuMonitor.h"
#include "../render/api/IParticleSystem.h"
#include "../render/api/IMeshRenderer.h"
#include "../render/NullMeshRenderer.h"
#include "../render/SmaMeshRenderer.h"
#include "../minigame/api/IMiniGameBackend.h"
#include "../live2d/api/IAnimationBackend.h"
#include "../script/vm/LuaManager.h"
#include "../script/state/GameState.h"
#include "../debug/HotReload.h"
#include "../debug/DebugProtocol.h"
#include "../job/api/IJobSystem.h"
#include "../resource/api/IAsyncLoader.h"
#include "../resource/AssetManager.h"
#include "../resource/api/IImageDecoder.h"
#include "../resource/api/IResourceGenerationTracker.h"
#include "../steam/api/ISteamBackend.h"
#include "../script/bindings/SteamBinding.h"
#include "../script/bindings/RenderBinding.h"
#include "../script/bindings/VFXBinding.h"
#include "../storage/api/ISaveManager.h"
#include "../storage/LocalFileSaveProvider.h"
#include "../archive/api/ICryptoEngine.h"
#include <SDL3/SDL.h>
#include <thread>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <vector>
#include <cmath>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>

#if defined(__ANDROID__)
#include <functional>
namespace Caesura {
void setMobileNativeAudioFocusSink(std::function<void(int)> sink);
void mobileNativeDrainAudioFocus();
} // namespace Caesura
#endif


#if defined(__ANDROID__)
static void* g_androidGLContext = nullptr;
extern "C" void* caesuraAndroidGLContext() { return g_androidGLContext; }
#endif

namespace Caesura {

namespace {

// Wait for this already-submitted readback; never manufacture another bgfx
// frame or read a filename that could belong to an earlier request.
ScreenshotResult awaitScreenshot(IRenderDevice& renderer, const ScreenshotTicket& ticket) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (;;) {
        auto result = renderer.takeScreenshot(ticket);
        if (result.status != ScreenshotStatus::Pending) return result;
        if (std::chrono::steady_clock::now() >= deadline) {
            renderer.cancelScreenshot(ticket);
            result = renderer.takeScreenshot(ticket);
            // Completion may have won the cancellation race.
            if (result.status != ScreenshotStatus::Completed)
                result.error = "Screenshot completion timed out";
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

constexpr const char* kDebugPausedGlobal = "_CAESURA_DEBUG_PAUSED";
constexpr const char* kDebugPauseProbeGlobal = "_CAESURA_DEBUG_IS_PAUSED";

int debugPauseProbe(lua_State* L) {
    auto* protocol = static_cast<DebugProtocol*>(
        lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushboolean(L, protocol && protocol->isDebugActive() ? 1 : 0);
    return 1;
}

// Lua-visible engine handle: _CAESURA_ENGINE = { setAutoSaveInterval = fn }.
static int lua_engine_setAutoSaveInterval(lua_State* L) {
    Engine* engine = static_cast<Engine*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!engine) return 0;
    engine->setAutoSaveInterval(static_cast<double>(luaL_checknumber(L, 1)));
    return 0;
}

void installDebugPauseProbe(lua_State* L, DebugProtocol* protocol) {
    if (!L) return;
    lua_pushlightuserdata(L, protocol);
    lua_pushcclosure(L, debugPauseProbe, 1);
    lua_setglobal(L, kDebugPauseProbeGlobal);
    lua_pushboolean(L, 0);
    lua_setglobal(L, kDebugPausedGlobal);
}

void removeDebugPauseProbe(lua_State* L) {
    if (!L) return;
    lua_pushnil(L);
    lua_setglobal(L, kDebugPauseProbeGlobal);
    lua_pushboolean(L, 0);
    lua_setglobal(L, kDebugPausedGlobal);
}

} // namespace

// Factory for GpuMonitor (defined in Engine_Gpu.cpp — F1)
std::unique_ptr<IGpuMonitor> createGpuMonitor(bool headless);
std::unique_ptr<IDisplayService> createDisplayService(
    const EngineConfig& config, const IPlatformBackend* platformBackend);
SDL_Window* getSDLWindow(const IPlatformBackend* platformBackend);
std::unique_ptr<ISteamBackend> createDefaultSteamIntegration();
std::unique_ptr<IAudioBackend> createHeadlessAudioBackend();
std::unique_ptr<IRenderDevice> createHeadlessRenderDevice();
std::unique_ptr<IPlatformBackend> createHeadlessPlatformBackend();
std::unique_ptr<IMiniGameBackend> createFallbackMiniGameBackend();
std::unique_ptr<IParticleSystem> createParticleSystem();
std::unique_ptr<IResourceGenerationTracker> createResourceGenerationTracker();
std::unique_ptr<ITextureBudget> createTextureBudget();
std::unique_ptr<ITextureManager> createTextureManager();
bool initializeTextureManager(ITextureManager& textures, IRenderDevice* renderer, bool gpuMode);
std::unique_ptr<ILayerManager> createLayerManager(IRenderDevice* renderDevice);
std::unique_ptr<ISandboxQuota> createSandboxQuota();
std::unique_ptr<AssetManager> createAssetManager();
std::unique_ptr<IImageDecoder> createImageDecoder();
std::unique_ptr<IAsyncLoader> createAsyncLoader(AssetManager* assetManager);
std::unique_ptr<IJobSystem> createJobSystem();
std::unique_ptr<ISaveManager> createSaveManager();
std::unique_ptr<carc::ICryptoEngine> createCryptoEngine();
std::unique_ptr<IAnimationBackend> createDefaultAnimationBackend();
std::unique_ptr<IAnimationBackend> createFallbackAnimationBackend();
void attachRenderDeviceToAnimationBackend(IAnimationBackend* animationBackend,
                                           IRenderDevice* renderDevice);
bool registerDefaultAssetProviders(AssetManager& assetManager,
                                   const EngineConfig& config, const std::string& root);
void registerEngineLuaRegistryServices(lua_State* L,
                                       IInputRouter* inputRouter,
                                       IVideoPlayer* videoPlayer,
                                       IParticleSystem* particleSystem,
                                       ITextureManager* textureManager,
                                       IAsyncLoader* asyncLoader);
void registerMiniGameLuaRegistryService(lua_State* L,
                                         IMiniGameBackend* miniGameBackend);

EngineConfig::~EngineConfig() {
    delete displayService;
    delete steam;
    delete animation;
    delete miniGame;
    delete sandboxQuota;
    delete layerManager;
    delete videoPlayer;
    delete gpuMonitor;
    delete inputRouter;
    delete lua;
    delete platform;
    delete audio;
    delete render;
}


// -- Phase G8-U1: lua_Alloc hook for per-allocation memory check ---------------
// Lua 5.4 forbids calling lua_gc inside the allocator (stack overflow).
// Memory budget is enforced per-frame in Engine::run() instead.
static void* s_luaAllocFn(void* ud, void* ptr, size_t osize, size_t nsize) {
    // Lua 5.4 forbids calling lua_gc inside the allocator (C stack overflow).
    if (nsize == 0) { free(ptr); return nullptr; }
    return realloc(ptr, nsize);
}

Engine::Engine(EngineConfig&& config)
    : m_config(std::move(config))
    , m_renderDevice(std::exchange(m_config.render, nullptr))
    , m_audioBackend(std::exchange(m_config.audio, nullptr))
    , m_platformBackend(std::exchange(m_config.platform, nullptr))
    , m_displayService(std::exchange(m_config.displayService, nullptr))
    , m_lua(std::exchange(m_config.lua, nullptr))
    , m_hotReload(std::make_unique<HotReload>())
    , m_inputRouter(std::exchange(m_config.inputRouter, nullptr))
    , m_gpuMonitor(std::exchange(m_config.gpuMonitor, nullptr))
    , m_miniGameBackend(std::exchange(m_config.miniGame, nullptr))
    , m_animationBackend(std::exchange(m_config.animation, nullptr))
    , m_steamBackend(std::exchange(m_config.steam, nullptr))
    , m_videoPlayer(std::exchange(m_config.videoPlayer, nullptr))
    , m_layerManager(std::exchange(m_config.layerManager, nullptr))
    , m_sandboxQuota(std::exchange(m_config.sandboxQuota, nullptr))
{
    // Acquire every injected pointer before any default allocation can throw.
    if (!m_lua) m_lua = std::make_unique<LuaManager>();
    if (!m_inputRouter) m_inputRouter = std::make_unique<InputRouter>();
    if (!m_videoPlayer) m_videoPlayer = std::make_unique<VideoPlayer>();
    if (!m_steamBackend) m_steamBackend = createDefaultSteamIntegration();
    if (!m_sandboxQuota) m_sandboxQuota = createSandboxQuota();
}

Engine::~Engine() {
    if (!m_shutdownComplete) shutdown();
}

void Engine::requireInitialized() const {
    if (!m_initialized) {
        throw std::logic_error("Engine service access requires a successful init()");
    }
}

IRenderDevice& Engine::renderDevice() { requireInitialized(); return *m_renderDevice; }
IAudioBackend& Engine::audio() { requireInitialized(); return *m_audioBackend; }
IPlatformBackend& Engine::platform() { requireInitialized(); return *m_platformBackend; }

bool Engine::init() {
    if (m_initAttempted || m_shutdownComplete) {
        fprintf(stderr, "[Engine] init() may only be called once per Engine instance.\n");
        return false;
    }
    m_initAttempted = true;

#if defined(__ANDROID__)
    // Android audio-focus bridge (t211): install the drain sink up front;
    // per-frame drain is a no-op until focus events arrive on the UI thread.
    Caesura::setMobileNativeAudioFocusSink([](int code) {
        AudioFocusEvent ev = (code == -1) ? AudioFocusEvent::FocusLost
                          : ((code == -2 || code == -3) ? AudioFocusEvent::InterruptionBegin
                                                        : AudioFocusEvent::FocusGained);
        if (auto* s = BackendRegistry::instance().getAudioFocusService()) {
            s->post(ev);
        }
    });
#endif

    const bool useHeadlessDefaults = m_config.headless && !m_config.editorMode;
    if (useHeadlessDefaults && !m_audioBackend) {
        m_audioBackend = createHeadlessAudioBackend();
    }
    if (useHeadlessDefaults && !m_renderDevice) {
        m_renderDevice = createHeadlessRenderDevice();
    }
    if (useHeadlessDefaults && !m_platformBackend) {
        m_platformBackend = createHeadlessPlatformBackend();
    }
    if (useHeadlessDefaults && !m_miniGameBackend) {
        m_miniGameBackend = createFallbackMiniGameBackend();
    }
    if (!m_layerManager) m_layerManager = createLayerManager(m_renderDevice.get());
    if (!m_textureBudget) m_textureBudget = createTextureBudget();
    if (!m_textureManager) m_textureManager = createTextureManager();
    if (!m_assetManager) m_assetManager = createAssetManager();
    if (!m_asyncLoader) m_asyncLoader = createAsyncLoader(m_assetManager.get());
    if (!m_jobSystem) m_jobSystem = createJobSystem();
    if (!m_saveManager) m_saveManager = createSaveManager();
    // Track P4: install the desktop default provider from the composition
    // root (SaveManager itself stays platform-agnostic — Android/iOS swap
    // this line for their AppStorage provider later).
    if (!m_saveManager->getSaveProvider()) {
        m_saveManager->setSaveProvider(std::make_unique<LocalFileSaveProvider>());
    }
    if (!m_cryptoEngine) m_cryptoEngine = createCryptoEngine();

    detail::g_mainThreadId = std::this_thread::get_id();

    if (m_config.editorMode) {
        fprintf(stderr, "[Engine] Running in EDITOR mode (hidden window + GPU)\n");
    }
    if (m_config.headless) {
        fprintf(stderr, "[Engine] Running in HEADLESS mode (no window, no GPU)\n");
    }

    m_debugInitialized = DebugManager::instance().init("logs");
    if (!m_debugInitialized) {
        fprintf(stderr, "[Engine] DebugManager init failed - continuing.\n");
    }

    // T2: Phase-split initialization
    if (!initPlatformPhase()) { shutdown(); return false; }
    if (!initScriptingPhase()) { shutdown(); return false; }
    if (!initAssetPhase()) { shutdown(); return false; }
    if (!initOptionalPhase()) {
        fprintf(stderr, "[Engine] Optional subsystems init had issues (non-fatal).\n");
    }

    // t212 ①: runtime script command errors surface through ErrorUI. The
    // handle path is re-entrancy safe (diag assembly never throws); scene/line
    // are re-read from ctx inside handleFatalError (G2).
    BackendRegistry::instance().setErrorReporter(
        [this](const std::string&, const std::string& error, const std::string&, int) {
            handleFatalError("Script runtime error", error.c_str());
        });

    DEBUG_INFO(SubSys::Engine, ErrCode::Ok, "All subsystems initialized.");
    m_initialized = true;
    return true;
}

// ============================================================================
// T2.1: Platform + Render + Audio initialization
// ============================================================================

bool Engine::initPlatformPhase() {
    int width  = m_config.width;
    int height = m_config.height;
    const char* title = m_config.title;
    const bool gpuMode = !m_config.headless || m_config.editorMode;

    if (!m_platformBackend || !m_renderDevice || !m_audioBackend) {
        DEBUG_ERROR(SubSys::Engine, ErrCode::Engine_PlatformInitFailed,
                    "Platform, render, and audio backends are required.");
        return false;
    }
    if (!m_platformBackend->init(title, width, height)) {
        DEBUG_ERROR(SubSys::Engine, ErrCode::Engine_PlatformInitFailed,
                    "Platform backend init failed.");
        return false;
    }
    m_platformInitialized = true;
    BackendRegistry::instance().setPlatformBackend(m_platformBackend.get());

    // Track M device-day: hand an OS GL/EGL context to the render device
    // (bgfx on Android needs the SDL-owned EGL context; desktop no-op).
    if (!m_platformBackend->createGLContext()) {
        DEBUG_ERROR(SubSys::Engine, ErrCode::Engine_PlatformInitFailed,
                    "Platform GL context creation failed.");
        return false;
    }
#if defined(__ANDROID__)
    g_androidGLContext = m_platformBackend->getGLContext();
#endif

    // Display metrics service (Track P1): injected from main.cpp when a
    // platform impl is available; Null default otherwise. Registered so
    // script/engine bindings can query live metrics without platform ifdefs.
    if (!m_displayService) {
        m_displayService = createDisplayService(m_config, m_platformBackend.get());
    }
    BackendRegistry::instance().setDisplayService(m_displayService.get());

    // Unified lifecycle hub (Track P2): desktop source is the SDL event
    // watch registered just below; Android/iOS post from native hooks.
    m_lifecycleService = std::make_unique<LifecycleService>();
    m_lifecycleService->addListener(this);
    BackendRegistry::instance().setLifecycleService(m_lifecycleService.get());

    // Audio focus arbitration (Track P5): desktop has no OS arbitration, so
    // the default hub only waits for native interruption callbacks (Android
    // JNI / iOS audio session notifications post here). Engine reacts by
    // pausing/resuming the audio backend; game layers may listen too.
    m_audioFocusService = std::make_unique<AudioFocusService>();
    m_audioFocusService->addListener(this);
    BackendRegistry::instance().setAudioFocusService(m_audioFocusService.get());

    // Mobile adapter: touch/lifecycle mapping (registered for editor/RPC
    // and future mobile ports; wired to SDL background/foreground events).
    m_mobileAdapter = std::make_unique<MobileAdapter>();
    BackendRegistry::instance().setMobileAdapter(m_mobileAdapter.get());
    m_gestureDetector = std::make_unique<GestureDetector>();
    // SDL app-lifecycle events are delivered only via event watches
    // (they are never queued); register once here.
    SDL_AddEventWatch(&Engine::appLifecycleWatch, this);

    SDL_Window* sdlWindow = getSDLWindow(m_platformBackend.get());
    if (m_config.editorMode && sdlWindow) {
        SDL_HideWindow(sdlWindow);
    }

    void* nwh = gpuMode ? m_platformBackend->getNativeWindowHandle() : nullptr;
    DEBUG_INFO(SubSys::Engine, ErrCode::Ok, "Native window handle: %p", nwh);

    // Optional explicit GPU backend selection (e.g. "opengl", "vulkan").
    if (m_config.renderBackend) {
        if (!m_renderDevice->setPreferredBackend(m_config.renderBackend)) {
            DEBUG_ERROR(SubSys::Engine, ErrCode::Engine_RenderInitFailed,
                        "Unknown render backend requested: %s", m_config.renderBackend);
            return false;
        }
    }

    if (!m_renderDevice->init(nwh, width, height)) {
        DEBUG_ERROR(SubSys::Engine, ErrCode::Engine_RenderInitFailed,
                    "Render device init failed.");
        return false;
    }
    m_renderInitialized = true;
    // Present surface: the drawable size differs from the configured engine
    // resolution on Android (OS-imposed window), so the renderer maps the
    // logical scene (1920x1080) to the real surface (e.g. 2320x956) and the
    // game fills the display instead of clipping/letterboxing.
    if (sdlWindow) {
        int pw = 0, ph = 0;
        if (SDL_GetWindowSizeInPixels(sdlWindow, &pw, &ph) && pw > 0 && ph > 0) {
            m_renderDevice->setPresentSize(uint32_t(pw), uint32_t(ph));
        }
    }
    BackendRegistry::instance().setRenderDevice(m_renderDevice.get());
    // GPU monitor may now query the render stats safely (round 39).
    if (m_gpuMonitor) m_gpuMonitor->setGpuAvailable(true);

    // Wire up default CJK font if available in GPU mode (P0 text visibility)
    if (gpuMode) {
        const char* defaultFont = "assets/fonts/NotoSansCJKsc-Regular.otf";
        if (m_renderDevice->loadTTF(defaultFont, 22.0f)) {
            DEBUG_INFO(SubSys::Engine, ErrCode::Ok, "Loaded default font: %s (22px)", defaultFont);
        } else {
            DEBUG_WARN(SubSys::Engine, ErrCode::Ok, "Default font not loaded: %s (using bitmap fallback)", defaultFont);
        }
    }

    // Render info (GPU caps)
    if (gpuMode) {
        const RenderRuntimeInfo renderInfo = m_renderDevice->getRuntimeInfo();
        DebugManager::RenderInfo ri;
        ri.backendName = renderInfo.backendName;
        ri.width = renderInfo.width;
        ri.height = renderInfo.height;
        ri.viewCount = renderInfo.viewCount;
        ri.shaderReady = renderInfo.shaderReady;
        DebugManager::instance().setRenderInfo(ri);
    }

    if (!m_audioBackend->init()) {
        DEBUG_ERROR(SubSys::Engine, ErrCode::Engine_AudioInitFailed,
                    "Audio backend init failed.");
        // t56: a machine without a usable audio device (CI runner, server,
        // macOS where the vendored SoLoud has no backend compiled) must not
        // keep the engine from starting. Keep the ERROR record above as the
        // auditable trace, then continue loudly on the silent backend
        // supplied by the default-construction helper. (The default headless
        // path never enters this branch: the silent backend's init() is
        // unconditional true, so headless semantics are unchanged.)
        DEBUG_WARN(SubSys::Engine, ErrCode::Engine_AudioInitFailed,
                   "audio unavailable -- continuing with silent NullAudio");
        m_audioBackend = createHeadlessAudioBackend();
        if (!m_audioBackend->init()) {
            DEBUG_ERROR(SubSys::Engine, ErrCode::Engine_AudioInitFailed,
                        "NullAudio fallback failed to initialize.");
            return false;
        }
    }
    m_audioInitialized = true;
    BackendRegistry::instance().setAudioBackend(m_audioBackend.get());
    BackendRegistry::instance().setAudioRestore(dynamic_cast<IAudioRestore*>(m_audioBackend.get()));

    DebugManager::AudioInfo ai;
    ai.initialized = true; ai.bgmBusReady = true;
    ai.voiceBusReady = true; ai.seBusReady = true; ai.globalVolume = 1.0f;
    DebugManager::instance().setAudioInfo(ai);

    // Input router
    BackendRegistry::instance().setInputRouter(m_inputRouter.get());
    DebugManager::InputInfo ii;
    ii.currentFocus = "KAG";
    DebugManager::instance().setInputInfo(ii);

    if (!m_gpuMonitor) {
        m_gpuMonitor = createGpuMonitor(m_config.headless);
    }

    // Texture budget + shared backend registrations
    m_textureBudget->detect();
    BackendRegistry::instance().setTextureBudget(m_textureBudget.get());
    if (!initializeTextureManager(*m_textureManager, m_renderDevice.get(), gpuMode)) {
        DEBUG_ERROR(SubSys::Engine, ErrCode::Engine_RenderInitFailed,
                    "Texture manager init failed.");
        return false;
    }
    m_textureManagerInitialized = true;
    BackendRegistry::instance().setTextureManager(m_textureManager.get());
    BackendRegistry::instance().setDebugManager(&DebugManager::instance());
    BackendRegistry::instance().setAsyncLoader(m_asyncLoader.get());
    m_saveManager->init("saves/");
    m_saveManager->setEncryptionPolicy(m_config.saveEncryptionPolicy);
    BackendRegistry::instance().setSaveManager(m_saveManager.get());
    if (!m_particleSystem) {
        m_particleSystem = createParticleSystem();
    }
    BackendRegistry::instance().setParticleSystem(m_particleSystem.get());

    BackendRegistry::instance().setLayerManager(m_layerManager.get());
    m_layerManager->init();
    m_layerManagerInitialized = true;

    return true;
}

// ============================================================================
// T2.2: Lua scripting + bindings + HotReload
// ============================================================================

bool Engine::initScriptingPhase() {
    if (!m_lua->init()) {
        fprintf(stderr, "[Engine] Lua VM init failed.\n");
        return false;
    }
    m_luaInitialized = true;

    // Phase G8-U1: install lua_Alloc hook for memory monitoring
    lua_setallocf(m_lua->state(), s_luaAllocFn, m_lua->state());
    // (Round 21 P1-5) BackendRegistry no longer stores a lua_State; the
    // Lua surface lives in script/bindings (EngineBinding + registries).
    BackendRegistry::instance().setLuaManager(m_lua.get());
    m_sandboxQuota->setLuaState(m_lua->state());
    m_sandboxQuotaBound = true;
    BackendRegistry::instance().setSandboxQuota(m_sandboxQuota.get());
    BackendRegistry::instance().setVideoPlayer(m_videoPlayer.get());

    // Phase R2-U1: Push all backend pointers into Lua registry so binding
    // files (KAG, Render, DevCore, Unified, VFX) can resolve them without
    // including BackendRegistry.h. Keys are the runtime contract with bindings.
    registerEngineLuaRegistryServices(m_lua->state(), m_inputRouter.get(),
                                      m_videoPlayer.get(), m_particleSystem.get(),
                                      m_textureManager.get(), m_asyncLoader.get());

    // HotReload for scripts/ directory
    m_hotReload->init("scripts/", m_lua->state());
    m_hotReload->setBeforeReloadCallback([this] {
        cancelRenderAsyncLoads(m_lua->state());
        m_deferredAsyncLoads.clear();
    });
    // Scene (.ks) and mod content live outside scripts/: watch them too so
    // editing a running scene hot-reloads surgically (kag_runner.reload_scene).
    m_hotReload->addWatchRoot("assets/script/");
    m_hotReload->addWatchRoot("mods/");

    if (m_config.enableDebugger) {
        m_debugProtocol = std::make_unique<DebugProtocol>(*m_hotReload);
        if (!m_debugProtocol->init(m_lua->state())) {
            fprintf(stderr, "[Engine] DebugProtocol init failed.\n");
            m_debugProtocol.reset();
            return false;
        }
        m_debugProtocolInitialized = true;
    }
    installDebugPauseProbe(m_lua->state(), m_debugProtocol.get());
    lua_pushboolean(m_lua->state(), 0);
    lua_setglobal(m_lua->state(), "_CAESURA_VOICE_COMPLETE");

    // Expose the engine handle to Lua (System.setAutoSaveInterval uses it).
    lua_State* engineL = m_lua->state();
    if (engineL) {
        lua_newtable(engineL);
        lua_pushlightuserdata(engineL, this);
        lua_pushcclosure(engineL, lua_engine_setAutoSaveInterval, 1);
        lua_setfield(engineL, -2, "setAutoSaveInterval");
        lua_setglobal(engineL, "_CAESURA_ENGINE");
    }

    return true;
}

// ============================================================================
// T2.3: Job system + Asset pipeline
// ============================================================================

bool Engine::initAssetPhase() {
    if (!m_imageDecoder) m_imageDecoder=createImageDecoder();
    BackendRegistry::instance().setImageDecoder(m_imageDecoder.get());
    if (!m_resourceGenerationTracker) {
        m_resourceGenerationTracker = createResourceGenerationTracker();
    }
    BackendRegistry::instance().setResourceGenerationTracker(
        m_resourceGenerationTracker.get());

    // Parallel task system
    m_jobSystem->init();
    m_jobSystemInitialized = true;
    BackendRegistry::instance().setJobSystem(m_jobSystem.get());
    m_videoPlayer->setJobSystem(*m_jobSystem);

    // Asset management
    m_assetManager->init();
    m_assetManagerInitialized = true;
    // Inject CARC providers (moved from AssetManager to break resource→archive cycle)
    if (!registerDefaultAssetProviders(*m_assetManager, m_config, ".")) {
        return false;
    }
    BackendRegistry::instance().setAssetReader(m_assetManager.get());
    m_asyncLoader->init();
    if (!m_asyncLoader->isRunning()) {
        DEBUG_ERR(SubSys::Resource, ErrCode::Ok, "Async loader initialization failed");
        return false;
    }
    m_asyncLoaderInitialized = true;

    return true;
}

// ============================================================================
// T2.4: Optional subsystems (Steam, Crypto, MiniGame, Animation)
// ============================================================================

bool Engine::initOptionalPhase() {
    bool allOk = true;

    // Steam init (optional, no-op if SDK not present). The binding is
    // registered UNCONDITIONALLY with the (possibly Null) backend in the
    // registry, so scripts can always call steam.* -- without the SDK every
    // call returns a safe default instead of a nil-function error.
    m_steamInitialized = m_steamBackend && m_steamBackend->init();
    BackendRegistry::instance().setSteamBackend(m_steamBackend.get());
    // (P2) registerSteamBinding moved to LuaManager::registerModules

    // Crypto engine registration (moved OUT of Steam if-block - bug fix)
    BackendRegistry::instance().setCryptoEngine(m_cryptoEngine.get());

    if (!m_miniGameBackend) {
        m_miniGameBackend = createFallbackMiniGameBackend();
    }

    // 3D mini-game backend (bgfx or null)
    m_miniGameBackend->setRenderDevice(BackendRegistry::instance().getRenderDevice());
    m_miniGameInitialized = m_miniGameBackend->init();
    if (!m_miniGameInitialized) {
        m_miniGameBackend->shutdown();
        m_miniGameBackend = createFallbackMiniGameBackend();
        m_miniGameBackend->setRenderDevice(BackendRegistry::instance().getRenderDevice());
        m_miniGameInitialized = m_miniGameBackend->init();
        allOk = false;
    }
    if (m_miniGameInitialized) {
        BackendRegistry::instance().setMiniGameBackend(m_miniGameBackend.get());
        registerMiniGameLuaRegistryService(m_lua->state(), m_miniGameBackend.get());
        // P1-5 (round 35): wire the mini-game into the GAME-focus input path.
        // InputRouter dispatches events to registered game callbacks while
        // focus == GAME (set by BgfxMiniGameBackend::enter/leave); without
        // this registration the backend's processEvent is never called.
        IMiniGameBackend* mg = m_miniGameBackend.get();
        m_inputRouter->registerGameCallback([mg](const SDL_Event& ev) {
            mg->processEvent(&ev);
        });
    }

    // Animation backend
    if (!m_animationBackend) m_animationBackend = createDefaultAnimationBackend();
    m_animationInitialized = m_animationBackend->init();
    if (!m_animationInitialized) {
        m_animationBackend->shutdown();
        m_animationBackend = createFallbackAnimationBackend();
        m_animationInitialized = m_animationBackend->init();
        allOk = false;
    }
    if (m_animationInitialized) {
        BackendRegistry::instance().setAnimationBackend(m_animationBackend.get());
        attachRenderDeviceToAnimationBackend(m_animationBackend.get(), m_renderDevice.get());
    }

    // Skeletal mesh renderer (SMA, Battle 4d S1+S2): the real bgfx
    // implementation when a GPU render device is active (lazy-init inside;
    // safe no-op without GPU), Null backend otherwise (headless/CI).
    if (!m_meshRenderer) {
        if (m_renderInitialized && !m_config.headless) {
            m_meshRenderer = std::make_unique<SmaMeshRenderer>();
        } else {
            m_meshRenderer = std::make_unique<NullMeshRenderer>();
        }
    }
    BackendRegistry::instance().setMeshRenderer(m_meshRenderer.get());

    return allOk;
}
void Engine::run(const OwnerPump& ownerPump) {
    if (!m_initialized) {
        fprintf(stderr, "[Engine] run() requires a successful init().\n");
        return;
    }
    m_running = true;
    if (m_config.headless) {
        m_lastTick = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    } else {
        m_lastTick = m_platformBackend->getTicksMs();
    }
    DEBUG_INFO(SubSys::Engine, ErrCode::Ok, "Entering main loop.");

    while (m_running) {
        PROFILE_SCOPE("frame");
        m_skipLuaCallbacksThisFrame = false;
#if defined(__ANDROID__)
        Caesura::mobileNativeDrainAudioFocus();
#endif
        if (ownerPump) ownerPump();
        if (m_shutdownComplete) break;
        m_skipLuaCallbacksThisFrame = pumpDebugger();
        publishDebugPauseState();
        processEvents();
        if (m_shutdownComplete) break;

        // --- GPU device loss recovery check (before any per-frame rendering) ---
        if (m_renderDevice && m_renderDevice->consumeDeviceLost()) {
            recoverFromDeviceLoss();
            continue;
        }

        uint64_t now;
        if (m_config.headless) {
            now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        } else {
            now = m_platformBackend->getTicksMs();
        }
        float dt = (float)(now - m_lastTick) / 1000.0f;
        m_lastTick = now;
        if (dt < 0.0f) dt = 0.0f;
        if (dt > 0.25f) dt = 0.25f;
        (void)dt; // reserved for frame-time tracking

        // Keep reload and Lua GC stopped while a coroutine is suspended.
        // Rendering and transport pumping remain active.
        if (!isLuaExecutionPaused()) {
            m_hotReload->checkAndReload();
        }

        // JobSystem main-thread callbacks + async load SDL events
        m_jobSystem->pollMainThreadJobs();
        if (m_shutdownComplete) break;
        if (!(m_config.headless || m_config.editorMode)) {
            m_asyncLoader->poll();
        }

        // -- Phase G8-U1: Lua memory budget check \(every frame\) ---------------
        lua_State* Lgc = m_lua->state();
        if (Lgc && !isLuaExecutionPaused()) {
            int memKB = lua_gc(Lgc, LUA_GCCOUNT, 0);
            if (memKB > 204 * 1024) {  // 80% = 204MB
                lua_gc(Lgc, LUA_GCSTEP, 50);
            }
            if (memKB > 244 * 1024) {  // 95% = 244MB
                lua_gc(Lgc, LUA_GCCOLLECT, 0);
                lua_getglobal(Lgc, "System");
                if (lua_istable(Lgc, -1)) {
                    lua_getfield(Lgc, -1, "collect_full");
                    if (lua_isfunction(Lgc, -1)) {
                        if (lua_pcall(Lgc, 0, 0, 0) != LUA_OK) {
                            fprintf(stderr, "System.collect_full: %s\n",
                                    lua_tostring(Lgc, -1) ? lua_tostring(Lgc, -1) : "unknown");
                            lua_pop(Lgc, 1);
                        }
                    }
                    else lua_pop(Lgc, 1);
                }
                lua_pop(Lgc, 1);
            }
            if (memKB > 256 * 1024) {  // 100% = 256MB
                fprintf(stderr, "[Engine] FATAL: Lua memory exceeded 256MB\n");
                handleFatalError("Lua OOM", "Memory exceeded 256MB limit");
            }
            // 300-frame periodic GC step
            m_gcFrameCounter++;
            if (m_gcFrameCounter >= 300) {
                m_gcFrameCounter = 0;
                lua_gc(Lgc, LUA_GCSTEP, 10);
            }
        }

        GpuQuality gpuQ = m_gpuMonitor->update(static_cast<double>(dt));

        // Gesture polling: long-press fires on a held still finger; pinch
        // pulses on two-finger distance changes. Dispatching at most one
        // event per frame keeps the SDL event queue from being flooded.
        if (m_mobileAdapter && m_gestureDetector) {
            const GestureEvent ge = m_gestureDetector->tick(
                static_cast<double>(SDL_GetTicks()));
            switch (ge.kind) {
            case GestureEvent::Kind::LongPress:
                m_mobileAdapter->onLongPress(ge.x, ge.y);
                break;
            case GestureEvent::Kind::Pinch:
                m_mobileAdapter->onPinch(ge.x, ge.y, ge.scale);
                break;
            // Multi-finger gestures (audit fix): the detector and the adapter
            // both grew TwoFingerTap/ThreeFingerHold/SwipeDown/SwipeUp, but
            // this dispatch forwarded only the first two kinds, so the new
            // gestures were unreachable at runtime and only unit tests ever
            // saw them. A swipe reports its END point in x/y and its travel
            // in deltaX/deltaY, hence the start reconstruction below.
            case GestureEvent::Kind::TwoFingerTap:
                m_mobileAdapter->onTwoFingerTap(ge.x, ge.y);
                break;
            case GestureEvent::Kind::ThreeFingerHold:
                m_mobileAdapter->onThreeFingerHold(ge.x, ge.y);
                break;
            case GestureEvent::Kind::SwipeDown:
                m_mobileAdapter->onSwipeDown(ge.x - ge.deltaX, ge.y - ge.deltaY,
                                             ge.x, ge.y);
                break;
            case GestureEvent::Kind::SwipeUp:
                m_mobileAdapter->onSwipeUp(ge.x - ge.deltaX, ge.y - ge.deltaY,
                                           ge.x, ge.y);
                break;
            case GestureEvent::Kind::None:
                break;
            }
        }

        // Auto-save timer (0 disables; Lua System.setAutoSaveInterval).
        if (m_autoSaveIntervalSec > 0.0 && dt > 0.0 && !isLuaExecutionPaused()) {
            m_autoSaveAccum += static_cast<double>(dt);
            if (m_autoSaveAccum >= m_autoSaveIntervalSec) {
                m_autoSaveAccum = 0.0;
                triggerAutoSave();
            }
        } else {
            m_autoSaveAccum = 0.0;
        }

        lua_State* L = m_lua->state();
        if (L) {
            // Dispatch at most one coalesced click per frame, before the
            // Lua update (mirrors the pre-existing _KAG_onClick behavior).
            if (m_clickPending && !isLuaExecutionPaused()) {
                m_clickPending = false;
                lua_getglobal(L, "_KAG_onClick");
                if (lua_isfunction(L, -1)) {
                    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                        const char* err = lua_tostring(L, -1);
                        fprintf(stderr, "_KAG_onClick: %s\n", err ? err : "unknown");
                        lua_pop(L, 1);
                    }
                } else { lua_pop(L, 1); }
            }
            if (!isLuaExecutionPaused()) {
                lua_getglobal(L, "engine_update");
                if (lua_isfunction(L, -1)) {
                    lua_pushnumber(L, dt);
                    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                        // S6: a per-frame script error is NOT fatal -- log it
                        // and keep the frame loop alive (the game continues;
                        // the KAG runner/scheduler has its own error recovery).
                        // Only OOM / render-loop failures stay fatal.
                        const char* err = lua_tostring(L, -1);
                        fprintf(stderr, "engine_update (recoverable): %s\n",
                                err ? err : "unknown");
                        lua_pop(L, 1);
                    }
                } else { lua_pop(L, 1); }
            }
            publishDebugPauseState();

            const int gpuQv = static_cast<int>(gpuQ);
            if (gpuQv != m_lastGpuQuality) {
                m_lastGpuQuality = gpuQv;
                lua_pushinteger(L, gpuQv);
                lua_setglobal(L, "_CAESURA_GPU_QUALITY");
            }
            const bool vfxOn = m_gpuMonitor->vfxEnabled();
            if (vfxOn != m_lastVfxEnabled) {
                m_lastVfxEnabled = vfxOn;
                lua_pushboolean(L, vfxOn ? 1 : 0);
                lua_setglobal(L, "_CAESURA_VFX_ENABLED");
            }
            const float gpuTime = m_gpuMonitor->metrics().gpuTimeMs;
            if (gpuTime != m_lastGpuTimeMs) {
                m_lastGpuTimeMs = gpuTime;
                lua_pushnumber(L, static_cast<lua_Number>(gpuTime));
                lua_setglobal(L, "_CAESURA_GPU_TIME_MS");
            }
            const float gpuAvg = m_gpuMonitor->metrics().rollingAvgMs;
            if (gpuAvg != m_lastGpuAvgMs) {
                m_lastGpuAvgMs = gpuAvg;
                lua_pushnumber(L, static_cast<lua_Number>(gpuAvg));
                lua_setglobal(L, "_CAESURA_GPU_AVG_MS");
            }
            const bool degraded = m_gpuMonitor->metrics().degraded;
            if (degraded != m_lastGpuDegraded) {
                m_lastGpuDegraded = degraded;
                lua_pushboolean(L, degraded ? 1 : 0);
                lua_setglobal(L, "_CAESURA_GPU_DEGRADED");
            }
        }

        // D4.6: Consume backend-owned voice completion events. A counter keeps
        // multiple natural completions lossless while Lua is debugger-paused.
        if (m_audioBackend) {
            m_audioBackend->update(static_cast<float>(dt));
            const unsigned int completed =
                m_audioBackend->consumeVoiceCompletions();
            const unsigned int capacity =
                std::numeric_limits<unsigned int>::max() -
                m_audioVoiceCompletionsPending;
            m_audioVoiceCompletionsPending +=
                completed > capacity ? capacity : completed;

            if (m_audioBackend->isVoicePlaying() && L) {
                lua_pushboolean(L, 0);
                lua_setglobal(L, "_CAESURA_VOICE_COMPLETE");
            }

            while (m_audioVoiceCompletionsPending > 0 &&
                   !isLuaExecutionPaused() && L) {
                lua_pushboolean(L, 1);
                lua_setglobal(L, "_CAESURA_VOICE_COMPLETE");
                lua_getglobal(L, "_onVoiceComplete");
                if (lua_isfunction(L, -1)) {
                    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                        const char* error = lua_tostring(L, -1);
                        fprintf(stderr, "_onVoiceComplete: %s\n",
                                error ? error : "non-string Lua error");
                        lua_pop(L, 1);
                    }
                } else { lua_pop(L, 1); }
                --m_audioVoiceCompletionsPending;
            }

            // A completion callback may start the next line immediately.
            if (m_audioBackend->isVoicePlaying() && L) {
                lua_pushboolean(L, 0);
                lua_setglobal(L, "_CAESURA_VOICE_COMPLETE");
            }
        }

        render(static_cast<float>(dt));

        {
            ScreenshotTicket exportTicket;
            if (!m_config.exportReplayFile.empty() && m_renderDevice) {
                auto request = m_renderDevice->requestScreenshot(ScreenshotOptions{});
                exportTicket = request.ticket;
                if (request.status != ScreenshotStatus::Pending || !exportTicket) {
                    fprintf(stderr, "[Engine] Export capture rejected: %s\n", request.error.c_str());
                    m_renderFailed = true;
                    m_running = false;
                }
            }

            presentFrame();

            if (exportTicket) {
                const auto image = awaitScreenshot(*m_renderDevice, exportTicket);
                bool written = false;
                if (image.status == ScreenshotStatus::Completed && !image.png.empty()) {
                    char filename[32];
                    snprintf(filename, sizeof(filename), "frame_%05u.png", m_frameCount);
                    const auto path = std::filesystem::u8path(m_config.exportDir) / filename;
                    std::ofstream output(path, std::ios::binary | std::ios::trunc);
                    if (output) {
                        output.write(reinterpret_cast<const char*>(image.png.data()),
                                     static_cast<std::streamsize>(image.png.size()));
                        output.close();
                        written = output.good();
                    }
                }
                if (!written) {
                    fprintf(stderr, "[Engine] Export capture failed for frame %u: %s\n",
                            m_frameCount, image.error.empty() ? "PNG write failed" : image.error.c_str());
                    m_renderFailed = true;
                    m_running = false;
                }
            }
        }
        if (m_renderFailed) break;

        // -- Reserved: 3D mini-game update hook (CPU work, future JobSystem target) --
        if (m_miniGameBackend && m_miniGameBackend->isActive() &&
            !isLuaExecutionPaused()) {
            m_miniGameBackend->update(static_cast<float>(dt));
            // U3.3: invoke Lua-side per-frame input callback if defined
            if (L) {
                lua_getglobal(L, "_minigame_update");
                if (lua_isfunction(L, -1)) {
                    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                        fprintf(stderr, "_minigame_update: %s\n",
                                lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown");
                        lua_pop(L, 1);
                    }
                } else {
                    lua_pop(L, 1);
                }
            }
        }

        // -- Deterministic frame limit (--frames N): lets CI drive a real
        // GPU window for N frames, then exits cleanly with code 0. --
        if (m_config.frameLimit > 0 &&
            ++m_frameCount >= m_config.frameLimit) {
            DEBUG_INFO(SubSys::Engine, ErrCode::Ok,
                       "Frame limit reached (%u frames); exiting.",
                       m_config.frameLimit);
            m_running = false;
        }
    }

}

bool Engine::pumpDebugger() {
    if (!m_debugProtocolInitialized || !m_debugProtocol) return false;

    m_debugProtocol->pumpCommands();
    const DebugProtocol::ResumeOutcome outcome =
        m_debugProtocol->resumePausedCoroutineManaged();
    if (outcome.status == DebugProtocol::NoResume) return false;

    if (!outcome.error.empty()) {
        fprintf(stderr, "[Debug] Coroutine resume failed: %s\n",
                outcome.error.c_str());
    }
    notifyKagDebugResume();
    return true;
}

bool Engine::isLuaExecutionPaused() const {
    return m_deviceRecoveryPaused || m_skipLuaCallbacksThisFrame ||
           (m_debugProtocol && m_debugProtocol->isDebugActive());
}

void Engine::publishDebugPauseState() {
    if (!m_luaInitialized || !m_lua || !m_lua->state()) return;
    lua_pushboolean(m_lua->state(), isLuaExecutionPaused() ? 1 : 0);
    lua_setglobal(m_lua->state(), kDebugPausedGlobal);
}

void Engine::notifyKagDebugResume() {
    lua_State* L = m_lua ? m_lua->state() : nullptr;
    if (!L) return;

    const int stackTop = lua_gettop(L);
    lua_getglobal(L, "package");
    if (!lua_istable(L, -1)) {
        lua_settop(L, stackTop);
        return;
    }
    lua_getfield(L, -1, "loaded");
    if (!lua_istable(L, -1)) {
        lua_settop(L, stackTop);
        return;
    }
    lua_getfield(L, -1, "kag_runner");
    if (!lua_istable(L, -1)) {
        lua_settop(L, stackTop);
        return;
    }
    lua_getfield(L, -1, "debug_resume");
    if (lua_isfunction(L, -1) && lua_pcall(L, 0, 0, 0) != LUA_OK) {
        fprintf(stderr, "[Debug] kag_runner.debug_resume: %s\n",
                lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown");
    }
    lua_settop(L, stackTop);
}

void Engine::quit() {
    m_running = false;
}


void Engine::dispatchAsyncLoad(std::unique_ptr<CompletedLoad> completed) {
    if (!completed || !m_asyncLoader->isCurrent(*completed)) return;
    if (isLuaExecutionPaused()) {
        m_deferredAsyncLoads.push_back(std::move(completed));
        return;
    }
    lua_State* L = m_lua ? m_lua->state() : nullptr;
    if (!L) return;

    uint32_t texId = 0;
    if (completed->success && completed->type == "texture" &&
        !completed->rgba.empty() && completed->width > 0) {
        CAESURA_ASSERT_MAIN_THREAD();
        texId = m_textureManager->loadTextureFromRGBA(
            completed->rgba.data(), completed->width, completed->height,
            completed->path);
        if (texId == 0) completed->success = false;
    }

    lua_getglobal(L, "_ASYNC_CALLBACKS");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    lua_rawgeti(L, -1, completed->id);
    const int cbRef = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    lua_pushnil(L);
    lua_rawseti(L, -2, completed->id);
    lua_pop(L, 1);
    if (cbRef <= 0) return;

    // Retain the function on the Lua stack, then release its reference before
    // calling it. Reentrant cancellation/reload may reuse that reference for
    // a new request, which must never be unreferenced by this old completion.
    lua_rawgeti(L, LUA_REGISTRYINDEX, cbRef);
    luaL_unref(L, LUA_REGISTRYINDEX, cbRef);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }
    lua_pushboolean(L, completed->success ? 1 : 0);
    lua_pushstring(L, completed->path.c_str());
    lua_pushinteger(L, static_cast<lua_Integer>(texId));
    if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
        fprintf(stderr, "[AsyncLoader] callback error: %s\n",
                lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown");
        lua_pop(L, 1);
    }
}

void Engine::processEvents() {
    // Steam callbacks every frame
    m_steamBackend->runCallbacks();
    if (m_shutdownComplete) return;

    lua_State* L = m_lua->state();
    if (L) {
        lua_getglobal(L, "_CAESURA_QUIT");
        const bool quitRequested =
            lua_isboolean(L, -1) && lua_toboolean(L, -1);
        lua_pop(L, 1);
        if (quitRequested) {
            m_running = false;
            return;
        }
    }

    // Pause input while Steam overlay is active
    if (m_steamBackend->isOverlayActive()) return;
    // Track 3: Reset Lua instruction budget each frame
    m_lua->resetInstructionBudget();

    // Move the deferred batch out before invoking Lua: a callback may reload,
    // cancel, or pause again. Every result is rechecked at its dispatch point.
    if (!isLuaExecutionPaused() && !m_deferredAsyncLoads.empty()) {
        auto deferred = std::move(m_deferredAsyncLoads);
        m_deferredAsyncLoads.clear();
        for (auto& completed : deferred) {
            dispatchAsyncLoad(std::move(completed));
            if (m_shutdownComplete) return;
        }
    }

    // Headless/Editor mode: no SDL event loop -- deliver completed async
    // loads directly (texture upload + Lua callback) instead of queueing
    // SDL events that nothing would ever consume.
    if (m_config.headless || m_config.editorMode) {
        for (auto& c : m_asyncLoader->drainCompleted()) {
            dispatchAsyncLoad(std::make_unique<CompletedLoad>(std::move(c)));
            if (m_shutdownComplete) return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        return;
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (m_renderDevice && (event.type == SDL_EVENT_WINDOW_RESIZED
                               || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)) {
            if (auto* window = SDL_GetWindowFromID(event.window.windowID)) {
                int pixelWidth = 0, pixelHeight = 0;
                if (SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight)
                    && pixelWidth > 0 && pixelHeight > 0)
                    m_renderDevice->setPresentSize(uint32_t(pixelWidth), uint32_t(pixelHeight));
            }
        }
        // -- Window resize: propagate to registered callbacks (layer tree
        // rebuild + dirty marking). Was documented-but-unwired: the
        // InputRouter callback list never fired because no event handler
        // existed here.
        if (event.type == SDL_EVENT_WINDOW_RESIZED) {
            static int s_resizeAll = 0;
            if (s_resizeAll++ < 24) {
                fprintf(stderr, "[RESIZE-ALL] %ux%u\n",
                        (unsigned)event.window.data1, (unsigned)event.window.data2);
            }
            if (m_inputRouter) {
                m_inputRouter->notifyResize(event.window.data1,
                                            event.window.data2);
            }
            // Track M device-day: the device window is portrait (e.g.
            // 1080x2276) while the engine initializes desktop-sized
            // (1280x720); without this the scene renders into a 720-high
            // band and appears squeezed to the bottom of the screen.
            // Only propagate when the size really changed (Android can
            // resend identical resize events every frame; that storms RTT
            // rebuilds and flickers).
            if (m_renderDevice && (uint32_t(event.window.data1) != m_renderLastW
                                || uint32_t(event.window.data2) != m_renderLastH)) {
                static int s_resizeLog = 0;
                if (s_resizeLog++ < 12) {
                    fprintf(stderr, "[RESIZE] %ux%u\n",
                            (unsigned)event.window.data1, (unsigned)event.window.data2);
                }
                m_renderDevice->resize(event.window.data1, event.window.data2);
                m_renderLastW = uint32_t(event.window.data1);
                m_renderLastH = uint32_t(event.window.data2);
            }
        }
        // -- GPU device reset from SDL (triggers recovery at next loop iteration) --
        if (event.type == SDL_EVENT_RENDER_DEVICE_RESET) {
            if (m_renderDevice) m_renderDevice->flagDeviceLost();
        }
        // -- Mobile touch (P7): SDL finger events -> MobileAdapter (which
        // injects mouse events and tracks multi-touch). Finger coords are
        // normalized 0..1; the adapter expects window pixels.
        if (m_mobileAdapter) {
            // SDL finger coordinates are normalized 0..1; scale to window
            // pixels for the adapter's touch -> mouse injection.
            const float winW = m_platformBackend
                ? static_cast<float>(m_platformBackend->getWindowWidth()) : 1280.0f;
            const float winH = m_platformBackend
                ? static_cast<float>(m_platformBackend->getWindowHeight()) : 720.0f;
            switch (event.type) {
                case SDL_EVENT_FINGER_DOWN: {
                    const float fx = event.tfinger.x * winW;
                    const float fy = event.tfinger.y * winH;
                    if (m_gestureDetector) {
                        m_gestureDetector->onFingerDown(
                            static_cast<int>(event.tfinger.fingerID), fx, fy,
                            static_cast<double>(SDL_GetTicks()));
                    }
                    m_mobileAdapter->onFingerDown(
                        fx, fy, static_cast<int>(event.tfinger.fingerID));
                    break;
                }
                case SDL_EVENT_FINGER_MOTION: {
                    const float fx = event.tfinger.x * winW;
                    const float fy = event.tfinger.y * winH;
                    if (m_gestureDetector) {
                        m_gestureDetector->onFingerMove(
                            static_cast<int>(event.tfinger.fingerID), fx, fy,
                            static_cast<double>(SDL_GetTicks()));
                    }
                    m_mobileAdapter->onFingerMotion(
                        fx, fy, static_cast<int>(event.tfinger.fingerID));
                    break;
                }
                case SDL_EVENT_FINGER_UP: {
                    const float fx = event.tfinger.x * winW;
                    const float fy = event.tfinger.y * winH;
                    if (m_gestureDetector) {
                        m_gestureDetector->onFingerUp(
                            static_cast<int>(event.tfinger.fingerID),
                            static_cast<double>(SDL_GetTicks()));
                    }
                    m_mobileAdapter->onFingerUp(
                        fx, fy, static_cast<int>(event.tfinger.fingerID));
                    break;
                }
                default:
                    break;
            }
        }

        // -- G8-U3: Async load completion (custom SDL event from AsyncLoader) --
        if (event.type == CAESURA_EVENT_ASYNC_LOAD &&
            event.user.data2 == m_asyncLoader.get()) {
            dispatchAsyncLoad(std::unique_ptr<CompletedLoad>(
                static_cast<CompletedLoad*>(event.user.data1)));
            continue;
        }

        if ((event.type == SDL_EVENT_MOUSE_MOTION ||
             event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
             event.type == SDL_EVENT_MOUSE_BUTTON_UP) && L) {
            float mx = 0, my = 0;
            SDL_GetMouseState(&mx, &my);

            int winW = m_platformBackend ? m_platformBackend->getWindowWidth() : 0;
            int winH = m_platformBackend ? m_platformBackend->getWindowHeight() : 0;
            int logW = m_renderDevice ? m_renderDevice->getBackbufferWidth() : m_config.width;
            int logH = m_renderDevice ? m_renderDevice->getBackbufferHeight() : m_config.height;
            if (winW > 0 && winH > 0 && logW > 0 && logH > 0) {
                mx = mx * ((float)logW / (float)winW);
                my = my * ((float)logH / (float)winH);
            }

            IPlatformBackend::MouseState mouse;
            mouse.x = mx; mouse.y = my;
            mouse.leftDown = (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) != 0;
            lua_pushnumber(L, mouse.x); lua_setglobal(L, "_GAME_MOUSE_X");
            lua_pushnumber(L, mouse.y); lua_setglobal(L, "_GAME_MOUSE_Y");
            lua_pushboolean(L, mouse.leftDown ? 1 : 0);
            lua_setglobal(L, "_GAME_MOUSE_DOWN");


            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                m_inputRouter->getFocus() == InputFocus::KAG &&
                !isLuaExecutionPaused()) {
                if (event.button.button == SDL_BUTTON_RIGHT) {
                    // Right-click dispatches immediately (not the advance path).
                    lua_getglobal(L, "_KAG_onRightClick");
                    if (lua_isfunction(L, -1)) {
                        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                            const char* err = lua_tostring(L, -1);
                            fprintf(stderr, "_KAG_onRightClick: %s\n", err ? err : "unknown");
                            lua_pop(L, 1);
                        }
                    } else { lua_pop(L, 1); }
                } else {
                    // Coalesce left clicks: the frame loop dispatches at
                    // most one _KAG_onClick per frame (event storms must
                    // not batch-resume the scheduler or deep-copy rollback
                    // snapshots per event).
                    m_clickPending = true;
                }
            }

        }

        // D9.7: Mouse wheel is a distinct SDL event, not a mouse-button event.
        if (event.type == SDL_EVENT_MOUSE_WHEEL && L &&
            m_inputRouter->getFocus() == InputFocus::KAG &&
            !isLuaExecutionPaused()) {
            lua_pushnumber(L, event.wheel.y); lua_setglobal(L, "_KAG_MOUSE_WHEEL_Y");
            lua_getglobal(L, "_KAG_onMouseWheel");
            if (lua_isfunction(L, -1)) {
                if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                    const char* err = lua_tostring(L, -1);
                    fprintf(stderr, "_KAG_onMouseWheel: %s\n", err ? err : "unknown");
                    lua_pop(L, 1);
                }
            } else { lua_pop(L, 1); }
        }

        // -- IME / Virtual Keyboard Text Input & Editing --------------------
        if (event.type == SDL_EVENT_TEXT_INPUT && L && !isLuaExecutionPaused()) {
            lua_getglobal(L, "_KAG_onTextInput");
            if (lua_isfunction(L, -1)) {
                lua_pushstring(L, event.text.text ? event.text.text : "");
                if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                    const char* err = lua_tostring(L, -1);
                    fprintf(stderr, "_KAG_onTextInput error: %s\n", err ? err : "unknown");
                    lua_pop(L, 1);
                }
            } else { lua_pop(L, 1); }
        }

        if (event.type == SDL_EVENT_TEXT_EDITING && L && !isLuaExecutionPaused()) {
            lua_getglobal(L, "_KAG_onTextEditing");
            if (lua_isfunction(L, -1)) {
                lua_pushstring(L, event.edit.text ? event.edit.text : "");
                lua_pushinteger(L, event.edit.start);
                lua_pushinteger(L, event.edit.length);
                if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
                    const char* err = lua_tostring(L, -1);
                    fprintf(stderr, "_KAG_onTextEditing error: %s\n", err ? err : "unknown");
                    lua_pop(L, 1);
                }
            } else { lua_pop(L, 1); }
        }

        if ((event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) && L) {
            if (event.type == SDL_EVENT_KEY_DOWN) {
                if (event.key.key == SDLK_BACKSPACE) {
                    lua_pushboolean(L, 1); lua_setglobal(L, "_GAME_KEY_BACKSPACE");
                    if (!isLuaExecutionPaused()) {
                        lua_getglobal(L, "_KAG_onKeyDown");
                        if (lua_isfunction(L, -1)) {
                            lua_pushinteger(L, event.key.key);
                            lua_pushstring(L, "backspace");
                            if (lua_pcall(L, 2, 0, 0) != LUA_OK) { lua_pop(L, 1); }
                        } else { lua_pop(L, 1); }
                    }
                }
                if (event.key.key == SDLK_F4) {
                    lua_pushboolean(L, 1); lua_setglobal(L, "_GAME_KEY_F4");
                    if (!event.key.repeat && !isLuaExecutionPaused()) {
                        lua_getglobal(L, "_KAG_onF4");
                        if (lua_isfunction(L, -1)) {
                            if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                                const char* err = lua_tostring(L, -1);
                                fprintf(stderr, "_KAG_onF4: %s\n", err ? err : "unknown");
                                lua_pop(L, 1);
                            }
                        } else { lua_pop(L, 1); }
                    }
                }
                if (event.key.key == SDLK_F5) {
                    lua_pushboolean(L, 1); lua_setglobal(L, "_GAME_KEY_F5");
                    if (!event.key.repeat && !isLuaExecutionPaused()) quicksave();
                }
                if (event.key.key == SDLK_F6) {
                    lua_pushboolean(L, 1); lua_setglobal(L, "_GAME_KEY_F6");
                    if (!event.key.repeat && !isLuaExecutionPaused()) quickload();
                }
                if (event.key.key == SDLK_W)    { lua_pushboolean(L, 1); lua_setglobal(L, "_GAME_KEY_W"); }
                // A: auto-mode toggle -- key-repeat guarded like V/Ctrl so a
                // held A doesn't flap auto_mode ~30x/sec via SDL auto-repeat.
                if (event.key.key == SDLK_A && !event.key.repeat) {
                    lua_pushboolean(L, 1); lua_setglobal(L, "_GAME_KEY_A");
                }
                if (event.key.key == SDLK_S)    { lua_pushboolean(L, 1); lua_setglobal(L, "_GAME_KEY_S"); }
                if (event.key.key == SDLK_D)    { lua_pushboolean(L, 1); lua_setglobal(L, "_GAME_KEY_D"); }
                if (event.key.key == SDLK_UP)   { lua_pushboolean(L, 1); lua_setglobal(L, "_GAME_KEY_UP"); }
                if (event.key.key == SDLK_DOWN) { lua_pushboolean(L, 1); lua_setglobal(L, "_GAME_KEY_DOWN"); }
                if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
                    lua_pushboolean(L, 1); lua_setglobal(L, "_GAME_KEY_ENTER");
                    if (!isLuaExecutionPaused()) {
                        lua_getglobal(L, "_KAG_onKeyDown");
                        if (lua_isfunction(L, -1)) {
                            lua_pushinteger(L, event.key.key);
                            lua_pushstring(L, "return");
                            if (lua_pcall(L, 2, 0, 0) != LUA_OK) { lua_pop(L, 1); }
                        } else { lua_pop(L, 1); }
                    }
                }
                if (event.key.key == SDLK_ESCAPE) {
                    lua_pushboolean(L, 1); lua_setglobal(L, "_GAME_KEY_ESC");
                    if (!isLuaExecutionPaused()) {
                        lua_getglobal(L, "_KAG_onKeyDown");
                        if (lua_isfunction(L, -1)) {
                            lua_pushinteger(L, event.key.key);
                            lua_pushstring(L, "escape");
                            if (lua_pcall(L, 2, 0, 0) != LUA_OK) { lua_pop(L, 1); }
                        } else { lua_pop(L, 1); }
                    }
                }
                if (event.key.key == SDLK_H)    { lua_pushboolean(L, 1); lua_setglobal(L, "_GAME_KEY_H"); }
                if (event.key.key == SDLK_F)    { lua_pushboolean(L, 1); lua_setglobal(L, "_GAME_KEY_F"); }
                if (event.key.key == SDLK_LEFT)  { lua_pushboolean(L, 1); lua_setglobal(L, "_GAME_KEY_LEFT"); }
                if (event.key.key == SDLK_RIGHT) { lua_pushboolean(L, 1); lua_setglobal(L, "_GAME_KEY_RIGHT"); }
                if (event.key.key == SDLK_V && !event.key.repeat) {
                    lua_pushboolean(L, 1); lua_setglobal(L, "_GAME_KEY_V");
                }
                // D9.6: Ctrl triggers skip mode via Lua (only on the initial
                // press -- key-repeat events would re-toggle skip ~30x/sec)
                if ((event.key.key == SDLK_LCTRL || event.key.key == SDLK_RCTRL) &&
                    !event.key.repeat && !isLuaExecutionPaused()) {
                    lua_getglobal(L, "_KAG_onCtrlDown");
                    if (lua_isfunction(L, -1)) {
                        if (lua_pcall(L, 0, 0, 0) != LUA_OK) { lua_pop(L, 1); }
                    } else { lua_pop(L, 1); }
                }
                // t109: native SwipeDown/SwipeUp consumers (MobileAdapter maps
                // them to SDLK_SPACE / SDLK_PAGEUP, MobileAdapter.cpp:305-330);
                // mirror the WEB gesture semantics (web/main.mjs:634-647) --
                // SPACE toggles the message-layer UI overlay, PAGEUP opens the
                // backlog view. Same guard style as _KAG_onCtrlDown above.
                if (event.key.key == SDLK_SPACE && !event.key.repeat && !isLuaExecutionPaused()) {
                    lua_getglobal(L, "_KAG_onKeySpace");
                    if (lua_isfunction(L, -1)) {
                        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                            const char* err = lua_tostring(L, -1);
                            fprintf(stderr, "_KAG_onKeySpace: %s\n", err ? err : "unknown");
                            lua_pop(L, 1);
                        }
                    } else { lua_pop(L, 1); }
                }
                if (event.key.key == SDLK_PAGEUP && !event.key.repeat && !isLuaExecutionPaused()) {
                    lua_getglobal(L, "_KAG_onKeyPageUp");
                    if (lua_isfunction(L, -1)) {
                        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                            const char* err = lua_tostring(L, -1);
                            fprintf(stderr, "_KAG_onKeyPageUp: %s\n", err ? err : "unknown");
                            lua_pop(L, 1);
                        }
                    } else { lua_pop(L, 1); }
                }
            }
            if (event.type == SDL_EVENT_KEY_UP) {
                if (event.key.key == SDLK_BACKSPACE) { lua_pushboolean(L, 0); lua_setglobal(L, "_GAME_KEY_BACKSPACE"); }
                if (event.key.key == SDLK_F4) { lua_pushboolean(L, 0); lua_setglobal(L, "_GAME_KEY_F4"); }
                if (event.key.key == SDLK_F5) { lua_pushboolean(L, 0); lua_setglobal(L, "_GAME_KEY_F5"); }
                if (event.key.key == SDLK_F6) { lua_pushboolean(L, 0); lua_setglobal(L, "_GAME_KEY_F6"); }
                if (event.key.key == SDLK_W)    { lua_pushboolean(L, 0); lua_setglobal(L, "_GAME_KEY_W"); }
                if (event.key.key == SDLK_A)    { lua_pushboolean(L, 0); lua_setglobal(L, "_GAME_KEY_A"); }
                if (event.key.key == SDLK_S)    { lua_pushboolean(L, 0); lua_setglobal(L, "_GAME_KEY_S"); }
                if (event.key.key == SDLK_D)    { lua_pushboolean(L, 0); lua_setglobal(L, "_GAME_KEY_D"); }
                if (event.key.key == SDLK_UP)   { lua_pushboolean(L, 0); lua_setglobal(L, "_GAME_KEY_UP"); }
                if (event.key.key == SDLK_DOWN) { lua_pushboolean(L, 0); lua_setglobal(L, "_GAME_KEY_DOWN"); }
                if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
                    lua_pushboolean(L, 0); lua_setglobal(L, "_GAME_KEY_ENTER");
                }
                if (event.key.key == SDLK_ESCAPE) { lua_pushboolean(L, 0); lua_setglobal(L, "_GAME_KEY_ESC"); }
                if (event.key.key == SDLK_H)    { lua_pushboolean(L, 0); lua_setglobal(L, "_GAME_KEY_H"); }
                if (event.key.key == SDLK_F)    { lua_pushboolean(L, 0); lua_setglobal(L, "_GAME_KEY_F"); }
                if (event.key.key == SDLK_LEFT)  { lua_pushboolean(L, 0); lua_setglobal(L, "_GAME_KEY_LEFT"); }
                if (event.key.key == SDLK_RIGHT) { lua_pushboolean(L, 0); lua_setglobal(L, "_GAME_KEY_RIGHT"); }
                if (event.key.key == SDLK_V)    { lua_pushboolean(L, 0); lua_setglobal(L, "_GAME_KEY_V"); }
                // D9.6: Ctrl skip mode toggle release
                if ((event.key.key == SDLK_LCTRL || event.key.key == SDLK_RCTRL) &&
                    !isLuaExecutionPaused()) {
                    lua_getglobal(L, "_KAG_onCtrlUp");
                    if (lua_isfunction(L, -1)) {
                        if (lua_pcall(L, 0, 0, 0) != LUA_OK) { lua_pop(L, 1); }
                    } else { lua_pop(L, 1); }
                }
            }
        }
        m_inputRouter->processEvent(event);
    }
}

void Engine::render(float dt) {
    if (m_shutdownComplete || !m_renderInitialized || m_deviceRecoveryPaused || m_renderFailed) return;
    // Headless mode (not editor): no GPU rendering
    if (m_config.headless && !m_config.editorMode) return;

    // t214-followup: postfx chain frame hook (beginFrame prepares chain state;
    // commit_frame at the end runs runPostFxChain before the core swap).
    if (m_renderDevice) m_renderDevice->beginFrame();

    // NOTE: instruction budget was reset once in processEvents this frame;
    // resetting again here would double the per-frame allowance.
    // Drive active videos by real frame time (frame-rate pacing inside).
    if (m_videoPlayer) m_videoPlayer->updateAll(dt);
    lua_State* L = m_lua->state();
    if (L && !isLuaExecutionPaused()) {
        lua_getglobal(L, "engine_render");
        if (lua_isfunction(L, -1)) {
            if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                const char* err = lua_tostring(L, -1);
                fprintf(stderr, "engine_render: %s\n", err ? err : "unknown");
                lua_pop(L, 1);
            }
        } else { lua_pop(L, 1); }
    }

    if (m_animationInitialized && m_animationBackend) {
        DebugManager::instance().beginFrameProfile();
        m_animationBackend->render(isLuaExecutionPaused() ? 0.0f : dt);
        DebugManager::instance().endFrameProfile();
    }

    if (m_renderDevice) {
        m_renderDevice->drawDebugOverlay("Caesura (AmeKAG) v1.0.0");
    }

    // -- Reserved: 3D mini-game render hook (main thread, after KAG pass) --
    if (m_miniGameBackend && m_miniGameBackend->isActive()) {
        m_miniGameBackend->render();
    }

    // Finalize postfx submissions. Presentation is a separate, single step.
    if (m_renderDevice) m_renderDevice->commit_frame();

}

void Engine::presentFrame() {
    if (m_shutdownComplete || !m_renderInitialized || m_deviceRecoveryPaused || m_renderFailed) return;
    if (m_renderDevice) m_renderDevice->advanceFrame();
    // The external Android GL surface is swapped only after bgfx submits the
    // complete draw pass, including mini-game and postfx work.
    if (m_platformBackend) m_platformBackend->postFrame();
}


// ============================================================================
//  Save helpers (SU-3)
// ============================================================================
void Engine::quicksave() {
    if (isLuaExecutionPaused()) return;
    lua_State* L = m_lua->state(); if (!L) return;
    lua_getglobal(L, "quicksave");
    if (lua_isfunction(L, -1)) { if (lua_pcall(L, 0, 0, 0) != LUA_OK) { fprintf(stderr, "quicksave: %s\n", lua_tostring(L, -1)); lua_pop(L, 1); } }
    else { lua_pop(L, 1); }
}
void Engine::quickload() {
    if (isLuaExecutionPaused()) return;
    lua_State* L = m_lua->state(); if (!L) return;
    lua_getglobal(L, "quickload");
    if (lua_isfunction(L, -1)) { if (lua_pcall(L, 0, 0, 0) != LUA_OK) { fprintf(stderr, "quickload: %s\n", lua_tostring(L, -1)); lua_pop(L, 1); } }
    else { lua_pop(L, 1); }
}
void Engine::triggerAutoSave() {
    if (isLuaExecutionPaused()) return;
    lua_State* L = m_lua->state(); if (!L) return;
    lua_getglobal(L, "autosave");
    if (lua_isfunction(L, -1)) { if (lua_pcall(L, 0, 0, 0) != LUA_OK) { fprintf(stderr, "autosave: %s\n", lua_tostring(L, -1)); lua_pop(L, 1); } }
    else { lua_pop(L, 1); }
}

void Engine::handleFatalError(const char* context, const char* luaError) {
    std::string msg = "A fatal error occurred in the engine.\n\n";
    msg += "Context: ";
    msg += context ? context : "unknown";
    msg += "\n\n";
    msg += luaError ? luaError : "No error details available.";
    msg += "\n\nPlease choose an action below.";

    // §15 Crash/Diagnostics: assemble the player-friendly crash context.
    // Composition-root data only; every field degrades gracefully to empty.
    DiagnosticInfo diag;
    diag.engineVersion = CAESURA_VERSION;  // CMake project version (see CaesuraModules.cmake)
    diag.platform = SDL_GetPlatform();
    if (m_renderDevice) diag.gpuBackend = m_renderDevice->getBackendName();
    diag.logDir = "logs";
    // t212 G2: failing command/line captured from the runner ctx below
    // (declared at function scope: ErrorUI::show is called outside the block).
    std::string ctxCmd;
    int ctxLine = 0;
    {
        // Currently running KAG scene from the runner ctx (same authoritative
        // source the debugger uses). Any failure leaves it empty -- building
        // diagnostics must never throw while handling a fatal error.
        lua_State* L = m_lua ? m_lua->state() : nullptr;
        if (L) {
            const int stackTop = lua_gettop(L);
            // t212 G2: fetch scene + failing command/line from the runner
            // ctx (scheduler stashes ctx.error_command/error_token_line on a
            // runtime command error). Empty when not a command error (OOM/GPU
            // recovery paths) -- building diagnostics must never throw while
            // handling a fatal error.
            const char* snippet =
                "local ok, ctx = pcall(function() "
                "return require('kag_runner').get_ctx() end); "
                "if ok and ctx then return tostring(ctx.current_scene or ''), "
                "tostring(ctx.error_command or ''), ctx.error_token_line or 0 end";
            if (luaL_loadstring(L, snippet) == LUA_OK
                && lua_pcall(L, 0, 3, 0) == LUA_OK) {
                if (lua_isstring(L, -3)) diag.scenePath = lua_tostring(L, -3);
                if (lua_isstring(L, -2)) ctxCmd = lua_tostring(L, -2);
                if (lua_isnumber(L, -1)) ctxLine = (int)lua_tointeger(L, -1);
            }
            lua_settop(L, stackTop);
        }
    }

    ErrorAction action = ErrorUI::show(
        "Engine Runtime Error",
        msg,
        "",  // scriptTrace: the runtime traceback is carried inside msg (t212 G4)
        ctxLine,
        m_renderInitialized && m_renderDevice && m_renderDevice->isInitialized()
            && !m_deviceRecoveryPaused && !m_renderFailed,
        ctxCmd,  // t212 G2: failing KAG command (empty when not a command error)
        diag
    );

    if (m_renderFailed) {
        m_running = false;
        return;
    }
    switch (action) {
        case ErrorAction::Retry:
            DEBUG_INFO(SubSys::Engine, ErrCode::Ok, "ErrorUI: Retry requested");
            // Request hot reload retry
            m_hotReload->requestReload();
            break;
        case ErrorAction::Title:
            DEBUG_INFO(SubSys::Engine, ErrCode::Ok, "ErrorUI: Title requested");
            m_running = false;
            break;
        case ErrorAction::Quit:
        default:
            DEBUG_INFO(SubSys::Engine, ErrCode::Ok, "ErrorUI: Quit requested");
            m_running = false;
            break;
    }
}


void Engine::renderOneFrame() {
    if (!m_initialized || m_shutdownComplete || !m_renderInitialized || m_deviceRecoveryPaused || m_renderFailed) return;
    if (m_config.headless && !m_config.editorMode) return;
    lua_State* L = m_lua->state();
    if (L && !isLuaExecutionPaused()) {
        lua_getglobal(L, "engine_update");
        if (lua_isfunction(L, -1)) {
            lua_pushnumber(L, 0.016);
            if (lua_pcall(L, 1, 0, 0) != LUA_OK) { lua_pop(L, 1); }
        } else { lua_pop(L, 1); }
    }
    render(0.016f);
    presentFrame();
}

bool Engine::reloadScriptsNow() {
    requireInitialized();
    if (isLuaExecutionPaused()) return false;
    m_hotReload->requestReload();
    return m_hotReload->checkAndReload();
}

static std::string encodeFrameBase64(const std::vector<uint8_t>& buffer) {
    const auto fileSize = buffer.size();
    static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((fileSize + 2) / 3) * 4);
    for (size_t i = 0; i < fileSize; i += 3) {
        unsigned char a = buffer[i];
        unsigned char b = (i + 1 < fileSize) ? buffer[i + 1] : 0;
        unsigned char c = (i + 2 < fileSize) ? buffer[i + 2] : 0;
        result += b64[a >> 2];
        result += b64[((a & 3) << 4) | (b >> 4)];
        result += (i + 1 < fileSize) ? b64[((b & 15) << 2) | (c >> 6)] : '=';
        result += (i + 2 < fileSize) ? b64[c & 63] : '=';
    }
    return result;
}

std::string Engine::captureFrameForRpc(int w, int h) {
    if (!m_initialized || m_shutdownComplete || !m_renderInitialized || !m_renderDevice
        || m_deviceRecoveryPaused || m_renderFailed) return "";
    if (m_config.headless && !m_config.editorMode) return "";
    if (w < 0 || h < 0 || ((w == 0) != (h == 0))) return "";
    // Keep the existing managed-frame update, then submit its own ticket
    // before the single presentation. Callback completion is checked explicitly.
    if (!m_config.headless || m_config.editorMode) {
        lua_State* L = m_lua->state();
        if (L && !isLuaExecutionPaused()) {
            lua_getglobal(L, "engine_update");
            if (lua_isfunction(L, -1)) {
                lua_pushnumber(L, 0.016);
                if (lua_pcall(L, 1, 0, 0) != LUA_OK) { lua_pop(L, 1); }
            } else { lua_pop(L, 1); }
        }
        render(0.016f);
    }
    auto request = m_renderDevice->requestScreenshot(
        ScreenshotOptions{static_cast<uint32_t>(w), static_cast<uint32_t>(h)});
    presentFrame();
    if (request.status != ScreenshotStatus::Pending || !request.ticket) return "";
    auto result = awaitScreenshot(*m_renderDevice, request.ticket);
    return result.status == ScreenshotStatus::Completed ? encodeFrameBase64(result.png) : "";
}

// -- App lifecycle watcher -------------------------------------------------
// SDL delivers WILL_ENTER_BACKGROUND/DID_ENTER_FOREGROUND exclusively to
// event watches (they never enter the poll queue). Contract: the platform
// layer must deliver these on the engine/main thread (true on iOS/Android);
// when a native mobile layer lands on a platform that dispatches elsewhere,
// events must be marshalled to the engine thread before touching Lua.
//
// Pure decision logic lives in handleAppLifecycle (unit-testable without
// SDL); the watcher only extracts Engine state and forwards the event type.
//
// Re-entrancy note (handoff 008 §4.1): SDL3 invokes event watchers from the
// push path too. A user Lua onPause/onResume callback that itself pushes an
// SDL event would re-enter this watcher synchronously inside the event-queue
// lock -> theoretical self-deadlock (LOW, handoff 004 (d)). The engine
// currently exposes no Lua binding that pushes SDL events, so the path is
// unreachable; re-audit when a mobile input/marshalling layer lands.
void Engine::handleAppLifecycle(IMobileAdapter* adapter, lua_State* L, Uint32 eventType) {
    if (!adapter) return;
    switch (eventType) {
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
            adapter->onPause(L);
            break;
        case SDL_EVENT_DID_ENTER_FOREGROUND:
            adapter->onResume(L);
            break;
        case SDL_EVENT_DISPLAY_ORIENTATION: {
            // P7: report orientation changes to scripts (portrait/landscape
            // etc.) so game UIs can reflow without a native plugin.
            const char* name = "unknown";
            switch (SDL_GetCurrentDisplayOrientation(0)) {
                case SDL_ORIENTATION_LANDSCAPE:          name = "landscape"; break;
                case SDL_ORIENTATION_LANDSCAPE_FLIPPED:  name = "landscape_flipped"; break;
                case SDL_ORIENTATION_PORTRAIT:           name = "portrait"; break;
                case SDL_ORIENTATION_PORTRAIT_FLIPPED:   name = "portrait_flipped"; break;
                default: break;
            }
            adapter->onOrientationChanged(L, name);
            break;
        }
        default:
            break;
    }
}

bool Engine::appLifecycleWatch(void* userdata, SDL_Event* event) {
    auto* engine = static_cast<Engine*>(userdata);
    if (!engine) return true;
    // Track P2: uniform lifecycle events go through LifecycleService.
    // Orientation is engine-specific today (no LifecycleEvent slot), so it
    // keeps its direct MobileAdapter path.
    switch (event->type) {
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
            if (engine->m_lifecycleService) engine->m_lifecycleService->post(LifecycleEvent::Background);
            break;
        case SDL_EVENT_DID_ENTER_FOREGROUND:
            if (engine->m_lifecycleService) engine->m_lifecycleService->post(LifecycleEvent::Foreground);
            break;
        case SDL_EVENT_LOW_MEMORY:
            if (engine->m_lifecycleService) engine->m_lifecycleService->post(LifecycleEvent::LowMemory);
            break;
        case SDL_EVENT_TERMINATING:
            if (engine->m_lifecycleService) engine->m_lifecycleService->post(LifecycleEvent::Terminate);
            break;
        case SDL_EVENT_DISPLAY_ORIENTATION:
            handleAppLifecycle(engine->m_mobileAdapter.get(),
                               engine->m_lua ? engine->m_lua->state() : nullptr,
                               event->type);
            break;
        default:
            break;
    }
    return true; // allow other watchers / the queue to see the event
}

void Engine::onLifecycleEvent(LifecycleEvent event) {
    auto* L = m_lua ? m_lua->state() : nullptr;
    switch (event) {
        case LifecycleEvent::Background:
        case LifecycleEvent::Pause:
            if (m_mobileAdapter) m_mobileAdapter->onPause(L);
            // Mobile backgrounding must silence audio without unloading
            // assets (composition-root concern, unchanged since round 29).
            if (m_audioBackend) m_audioBackend->suspend();
            break;
        case LifecycleEvent::Foreground:
        case LifecycleEvent::Resume:
            if (m_mobileAdapter) m_mobileAdapter->onResume(L);
            if (m_audioBackend) m_audioBackend->resume();
            break;
        case LifecycleEvent::LowMemory:
            if (m_mobileAdapter) m_mobileAdapter->onLowMemory(L);
            break;
        case LifecycleEvent::Terminate:
            // Notification only: teardown order is unchanged (the SDL watch
            // is unregistered first in Engine::shutdown).
            if (m_mobileAdapter) m_mobileAdapter->onTerminate(L);
            break;
    }
}

void Engine::onAudioFocusEvent(AudioFocusEvent event) {
    // Pause/resume the whole audio engine on focus loss / interruptions;
    // SoLoud suspend is idempotent (safe with the lifecycle background
    // suspend). An interruption end that never began is a resume no-op.
    switch (event) {
        case AudioFocusEvent::FocusLost:
        case AudioFocusEvent::InterruptionBegin:
            if (m_audioBackend) m_audioBackend->suspend();
            break;
        case AudioFocusEvent::FocusGained:
        case AudioFocusEvent::InterruptionEnd:
            if (m_audioBackend) m_audioBackend->resume();
            break;
    }
}

void Engine::shutdown() {
    if (m_shutdownComplete) return;
    m_shutdownComplete = true;
    m_initialized = false;
    m_running = false;

    // Unregister the SDL app-lifecycle watch first: a background event
    // delivered after this point must not reach Lua state being torn down.
    SDL_RemoveEventWatch(&Engine::appLifecycleWatch, this);
    // Remove the engine listener first too: a native mobile lifecycle event
    // delivered after this point must not reach Lua state being torn down.
    if (m_lifecycleService) m_lifecycleService->removeListener(this);
    if (m_audioFocusService) m_audioFocusService->removeListener(this);

    // A never-initialized Engine owns its injected objects but no global state.
    if (!m_initAttempted) return;

    auto& registry = BackendRegistry::instance();

    // Invalidate script-owned loads before draining generic completions. Stop
    // admission and join workers while every backend is still alive; final
    // legal callbacks must precede VFX, render, layer and mini-game teardown.
    if (m_luaInitialized && !GameState::stopRunner(m_lua->state())) {
        fprintf(stderr, "[Engine] Runner cleanup failed during shutdown.\n");
    }
    if (m_asyncLoaderInitialized) cancelRenderAsyncLoads(m_lua->state());
    m_deferredAsyncLoads.clear();
    if (m_jobSystemInitialized) m_jobSystem->shutdown();
    if (m_asyncLoaderInitialized) m_asyncLoader->shutdown();

    VFXBinding_Shutdown(m_particleSystem.get());

    if (m_renderInitialized) {
        m_renderDevice->beginShutdown();
    }

    if (m_layerManagerInitialized) {
        m_layerManager->shutdown();
        m_layerManagerInitialized = false;
        registry.setLayerManager(nullptr);
    }

    // Reset error crash counters on clean shutdown
    ErrorUI::resetCounters();

    if (m_miniGameInitialized) m_miniGameBackend->shutdown();
    m_miniGameBackend.reset();
    registry.setMiniGameBackend(nullptr);

    registry.setAssetReader(nullptr);
    registry.setImageDecoder(nullptr);
    if (m_assetManagerInitialized) m_assetManager->shutdown();
    if (m_steamInitialized) {
        m_steamBackend->shutdown();
    }
    registry.setSteamBackend(nullptr);
    if (m_videoPlayer) m_videoPlayer->shutdown();
    if (m_luaInitialized) {
        removeDebugPauseProbe(m_lua->state());
    }
    if (m_debugProtocolInitialized && m_debugProtocol) {
        if (!m_debugProtocol->shutdown()) {
            fprintf(stderr, "[Engine] DebugProtocol shutdown rejected off owner thread.\n");
        }
        m_debugProtocolInitialized = false;
    }
    m_debugProtocol.reset();
    if (m_luaInitialized) m_hotReload->shutdown();
    if (m_audioInitialized) m_audioBackend->shutdown();

    // Flush renderer pipeline before touching shared animation resources.
    if (m_renderInitialized) { m_renderDevice->flushAllRTT(); }

    // Process one final renderer frame to drain pending GPU commands.
    if (m_renderInitialized) { m_renderDevice->advanceFrame(); }

    // Animation shuts down after renderer pipeline is drained.
    if (m_animationInitialized) {
        m_animationBackend->shutdown();
        m_animationInitialized = false;
    }
    m_animationBackend.reset();
    registry.setAnimationBackend(nullptr);

    // SmaMeshRenderer owns bgfx programs and per-mesh GPU buffers. Destroy it
    // while the renderer context is still alive; member destruction after
    // m_renderDevice->shutdown() would release GPU handles too late.
    registry.setMeshRenderer(nullptr);
    m_meshRenderer.reset();

    // Animation can release textures through ITextureManager during shutdown.
    if (m_textureManagerInitialized) {
        m_textureManager->shutdown();
        m_textureManagerInitialized = false;
        registry.setTextureManager(nullptr);
    }

    // Drain texture destruction before unregistering quota/Lua services.
    if (m_renderInitialized) { m_renderDevice->advanceFrame(); }

    if (m_sandboxQuotaBound) {
        registry.setSandboxQuota(nullptr);
        m_sandboxQuota->setLuaState(nullptr);
        m_sandboxQuotaBound = false;
    }
    if (m_luaInitialized) m_lua->shutdown();
    m_deferredAsyncLoads.clear();

    if (m_renderInitialized) { m_renderDevice->shutdown(); }
    registry.setDisplayService(nullptr);
    m_displayService.reset();
    if (m_platformInitialized) m_platformBackend->shutdown();

    registry.setVideoPlayer(nullptr);
    registry.setInputRouter(nullptr);
    registry.setTextureManager(nullptr);
    registry.setLayerManager(nullptr);
    registry.setSandboxQuota(nullptr);
    registry.setTextureBudget(nullptr);
    registry.setDebugManager(nullptr);
    registry.setAsyncLoader(nullptr);
    registry.setJobSystem(nullptr);
    registry.setCryptoEngine(nullptr);
    registry.setParticleSystem(nullptr);
    registry.setSaveManager(nullptr);
    registry.setResourceGenerationTracker(nullptr);
    registry.setSteamBackend(nullptr);
    registry.setMobileAdapter(nullptr);
    registry.setLifecycleService(nullptr);
    registry.setAudioFocusService(nullptr);
    registry.setRenderDevice(nullptr);
    registry.setAudioBackend(nullptr);
    registry.setAudioRestore(nullptr);
    registry.setPlatformBackend(nullptr);
    registry.setLuaManager(nullptr);

    if (m_debugInitialized) DebugManager::instance().shutdown();
}

void Engine::recoverFromDeviceLoss() {
    fprintf(stderr, "[Engine] === GPU device lost — starting recovery ===\n");
    m_deviceRecoveryPaused = true;

    // Phase 1: Notify all listeners to release GPU resources
    BackendRegistry::instance().notifyDeviceLost();

    // Phase 2: Reinitialize renderer.
    void* nwh = nullptr;
    if (m_platformBackend) {
        nwh = m_platformBackend->getNativeWindowHandle();
    }
    int w = m_platformBackend ? m_platformBackend->getWindowWidth() : m_config.width;
    int h = m_platformBackend ? m_platformBackend->getWindowHeight() : m_config.height;

    if (!m_renderDevice || !m_renderDevice->recoverDevice(nwh, w, h)) {
        fprintf(stderr, "[Engine] FATAL: renderer recovery failed after device loss\n");
        m_renderFailed = true;
        m_running = false;
        // Keep teardown ownership: a failed font/resource restoration may
        // still leave a physical GPU context that shutdown must drain/release.
        if (!m_config.headless)
            handleFatalError("GPU Recovery", "Renderer recovery failed after device loss");
        return;
    }
    fprintf(stderr, "[Engine] renderer recovery success (%dx%d)\n", w, h);

    // Phase 3: Notify all listeners to recreate GPU resources.
    BackendRegistry::instance().notifyDeviceRestored();

    // Phase 4: Notify Lua scripts to reload textures.
    lua_State* L = m_lua->state();
    if (L) {
        lua_pushboolean(L, 1);
        lua_setglobal(L, "_CAESURA_DEVICE_RESTORED");
    }

    m_deviceRecoveryPaused = false;
    fprintf(stderr, "[Engine] === GPU device recovery complete ===\n");
}

} // namespace Caesura
