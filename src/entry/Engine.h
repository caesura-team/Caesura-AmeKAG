#pragma once

#include "entry/EngineConfig.h"
#include <cstdint>  // fixed-width types (GCC strict)
#include "platform/api/IPlatformBackend.h"
#include "platform/api/ILifecycleService.h"
#include "audio/api/IAudioFocusService.h"
#include "platform/MobileAdapter.h"
#include "platform/GestureDetector.h"
#include "di/api/ThreadAssert.h"
#include <SDL3/SDL.h>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace Caesura {

class IRenderDevice;
class IAudioBackend;
class LuaManager;
class HotReload;
class DebugProtocol;
class InputRouter;
class IGpuMonitor;
class IDisplayService;
class VideoPlayer;
class DebugManager;
class IMiniGameBackend;
class IAnimationBackend;
class IMeshRenderer;
class ISteamBackend;
class IParticleSystem;
class IResourceGenerationTracker;
class ITextureBudget;
class ITextureManager;
class ILayerManager;
class ISandboxQuota;
class AssetManager;
class IImageDecoder;
class IAsyncLoader;
class IJobSystem;
class ISaveManager;
struct CompletedLoad;
namespace carc { class ICryptoEngine; }

// The Lua KAG runner owns its session table. GameState references that exact
// table in the VM registry for native queries and hot reload; VM initialization
// creates no independent state. Engine stops the runner before native teardown.
class Engine : public ILifecycleListener, public IAudioFocusListener {
public:
    using OwnerPump = std::function<void()>;

    explicit Engine(EngineConfig&& config);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    bool init();
    void run(const OwnerPump& ownerPump = {});
    void shutdown();

    // Auto-save interval (seconds; 0 disables). Called from Lua via the
    // _CAESURA_ENGINE table (System.setAutoSaveInterval).
    void setAutoSaveInterval(double seconds) { m_autoSaveIntervalSec = seconds > 0 ? seconds : 0.0; }

    bool isHeadless() const { return m_config.headless; }
    bool isEditorMode() const { return m_config.editorMode; }
    bool hasRenderFailure() const noexcept { return m_renderFailed; }
    void renderOneFrame();
    std::string captureFrameForRpc(int w, int h);
    bool reloadScriptsNow();
    void quit();
    DebugProtocol* debugProtocol() noexcept { return m_debugProtocol.get(); }
    const DebugProtocol* debugProtocol() const noexcept { return m_debugProtocol.get(); }

    IRenderDevice& renderDevice();
    IAudioBackend& audio();
    IPlatformBackend& platform();
    IMiniGameBackend& miniGame() { requireInitialized(); return *m_miniGameBackend; }
    IAnimationBackend& animation() { requireInitialized(); return *m_animationBackend; }
    IMeshRenderer& meshRenderer() { requireInitialized(); return *m_meshRenderer; }
    // Pure mapping from an SDL app event type to the mobile adapter callback
    // (onPause/onResume), extracted so it can be unit-tested without SDL.
    static void handleAppLifecycle(IMobileAdapter* adapter, lua_State* L, Uint32 eventType);
    // Unified lifecycle (Track P2): receives LifecycleService events from
    // Engine::appLifecycleWatch (desktop SDL source) / native mobile sources.
    void onLifecycleEvent(LifecycleEvent event) override;
    void onAudioFocusEvent(AudioFocusEvent event) override;

    LuaManager&   lua()           { requireInitialized(); return *m_lua; }
    InputRouter&  input()         { requireInitialized(); return *m_inputRouter; }
    IGpuMonitor&  gpuMonitor()    { requireInitialized(); return *m_gpuMonitor; }
    VideoPlayer&  videoPlayer()   { requireInitialized(); return *m_videoPlayer; }
    ITextureBudget& textureBudget() { requireInitialized(); return *m_textureBudget; }
    IJobSystem&   jobSystem()     { requireInitialized(); return *m_jobSystem; }

    const EngineConfig& config() const { return m_config; }

private:
    void requireInitialized() const;
    void processEvents();
    void dispatchPlatformEvent(const SDL_Event& event, bool internalTouch = false);
    void dispatchTouchEvent(const SDL_Event& event);
    void pumpGestures(double nowMs);
    void cancelPointerInput();
    bool isLifecyclePaused() const;
    void updateLifecyclePause(bool wasPaused);
    void render(float dt);
    void presentFrame();

    void triggerAutoSave();
    void quicksave();
    void quickload();

    void handleFatalError(const char* context, const char* luaError);
    void recoverFromDeviceLoss();
    bool pumpDebugger();
    bool isLuaExecutionPaused() const;
    void publishDebugPauseState();
    void notifyKagDebugResume();
    void updateAudioSuspension();
    bool pendingVoiceOwnerMatches(lua_State* L) const;
    void clearPendingVoiceCompletions(lua_State* L);

    // T2: Init phase methods
    bool initPlatformPhase();
    bool initScriptingPhase();
    bool initAssetPhase();
    bool initOptionalPhase();

    bool         m_running  = false;
    uint64_t     m_lastTick = 0;
    bool         m_initAttempted = false;
    bool         m_initialized = false;
    bool         m_shutdownComplete = false;
    bool         m_debugInitialized = false;
    bool         m_debugProtocolInitialized = false;
    // Coalesce mouse clicks: at most one _KAG_onClick dispatch per frame
    // (event storms from auto-clickers/touch ghosts must not batch-resume).
    bool         m_clickPending = false;
    float        m_clickX = 0.0f, m_clickY = 0.0f;
    float        m_pointerX = 0.0f, m_pointerY = 0.0f;
    bool         m_pointerLeftDown = false;
    struct TouchContact {
        SDL_TouchID device = 0;
        SDL_FingerID finger = 0;
        bool active = false;
    };
    TouchContact m_touchContacts[GestureDetector::kMaxFingers];
    uint64_t m_touchGeneration = 0;
    // Auto-save timer: triggerAutoSave() fires every m_autoSaveIntervalSec
    // of accumulated frame time (0 disables; Lua System.setAutoSaveInterval).
    double       m_autoSaveAccum = 0.0;
    double       m_autoSaveIntervalSec = 60.0;
    bool         m_platformInitialized = false;
    bool         m_renderInitialized = false;
    bool         m_renderFailed = false;
    bool         m_audioInitialized = false;
    // Independently paired reasons; one resume event cannot clear another
    // source's pause. The backend sees only aggregate state transitions.
    bool         m_lifecycleBackground = false;
    bool         m_lifecyclePaused = false;
    bool         m_windowFocusLost = false;
    bool         m_audioFocusLost = false;
    bool         m_audioInterrupted = false;
    bool         m_audioSuspended = false;
    bool         m_textureManagerInitialized = false;
    bool         m_layerManagerInitialized = false;
    bool         m_sandboxQuotaBound = false;
    bool         m_luaInitialized = false;
    bool         m_jobSystemInitialized = false;
    bool         m_assetManagerInitialized = false;
    bool         m_asyncLoaderInitialized = false;
    bool         m_steamInitialized = false;
    uint32_t     m_renderLastW = 0;
    uint32_t     m_renderLastH = 0;
    bool         m_miniGameInitialized = false;
    bool         m_animationInitialized = false;
    unsigned int m_audioVoiceCompletionsPending = 0;
    // Positive Lua registry reference pins the batch's runner table. Zero
    // denotes engine-owned audio outside a runner, not a raw table address.
    int m_audioCompletionOwnerRef = 0;
    int  m_gcFrameCounter = 0;
    // Rendered-frame counter; when m_config.frameLimit > 0 the main loop
    // stops deterministically after that many frames (--frames N).
    uint32_t m_frameCount = 0;
    bool         m_deviceRecoveryPaused = false;
    bool         m_skipLuaCallbacksThisFrame = false;
    static void* luaAllocHook(void* ud, void* ptr, size_t osize, size_t nsize);

    EngineConfig m_config;
    std::unique_ptr<IRenderDevice>     m_renderDevice;
    std::unique_ptr<IAudioBackend>     m_audioBackend;
    std::unique_ptr<IPlatformBackend>  m_platformBackend;
    std::unique_ptr<IDisplayService>   m_displayService;
    std::unique_ptr<MobileAdapter>     m_mobileAdapter;
    // Owner-thread gesture classification shares the bounded contact slots.
    std::unique_ptr<GestureDetector>   m_gestureDetector;
    std::unique_ptr<ILifecycleService>  m_lifecycleService;
    std::unique_ptr<IAudioFocusService>  m_audioFocusService;
    std::unique_ptr<LuaManager>        m_lua;

    // App-lifecycle watcher: SDL app events (WILL_ENTER_BACKGROUND etc.) are
    // delivered exclusively via SDL_AddEventWatch — they never enter the
    // poll queue. Registered on init, removed on shutdown.
    static bool SDLCALL appLifecycleWatch(void* userdata, SDL_Event* event);
    std::unique_ptr<HotReload>         m_hotReload;
    std::unique_ptr<DebugProtocol>      m_debugProtocol;
    std::unique_ptr<InputRouter>       m_inputRouter;
    std::unique_ptr<IGpuMonitor>        m_gpuMonitor;
    std::unique_ptr<IMiniGameBackend>  m_miniGameBackend;
    std::unique_ptr<IAnimationBackend>  m_animationBackend;
    std::unique_ptr<IMeshRenderer>      m_meshRenderer;
    std::unique_ptr<ISteamBackend>      m_steamBackend;
    std::unique_ptr<VideoPlayer>       m_videoPlayer;
    std::unique_ptr<IParticleSystem>   m_particleSystem;
    std::unique_ptr<IResourceGenerationTracker> m_resourceGenerationTracker;
    std::unique_ptr<ITextureBudget>    m_textureBudget;
    std::unique_ptr<ITextureManager>   m_textureManager;
    std::unique_ptr<ILayerManager>     m_layerManager;
    std::unique_ptr<ISandboxQuota>     m_sandboxQuota;
    std::unique_ptr<IJobSystem>        m_jobSystem;
    std::unique_ptr<AssetManager>      m_assetManager;
    std::unique_ptr<IImageDecoder>     m_imageDecoder;
    std::unique_ptr<IAsyncLoader>      m_asyncLoader;
    std::unique_ptr<ISaveManager>      m_saveManager;
    std::unique_ptr<carc::ICryptoEngine> m_cryptoEngine;
    // Dispatch one completed async load (texture upload + Lua callback).
    // Used by both the SDL event path and the headless/editor drain path.
    void dispatchAsyncLoad(std::unique_ptr<CompletedLoad> completed);

    std::vector<std::unique_ptr<CompletedLoad>> m_deferredAsyncLoads;
    // Cached last-written GPU-state globals: written to Lua only when the
    // value changes (avoid per-frame global-table hashing for near-constant
    // values).
    int      m_lastGpuQuality = -1;
    bool     m_lastVfxEnabled = false;
    bool     m_lastGpuDegraded = false;
    float    m_lastGpuTimeMs = -1.0f;
    float    m_lastGpuAvgMs  = -1.0f;
};

} // namespace Caesura
