#pragma once
#include "api/IDeviceLostListener.h"
#include <typeindex>
#include <unordered_map>
#include <vector>
#include <string>
#include <functional>
#include <cstdint>
#include <type_traits>
#include <stdexcept>
#include <limits>

namespace Caesura {

// Forward declarations only — no I*.h includes needed for consumers
class IRenderDevice;
class IAudioBackend;
class IAudioRestore;
class IPlatformBackend;
class IInputRouter;
class IVideoPlayer;
class ITextureManager;
class ILayerManager;
class IParticleSystem;
class IDebugManager;
class IAsyncLoader;
class IMiniGameBackend;
class IAnimationBackend;
class ILuaManager;
class IJobSystem;
class ISandboxQuota;
class ITextureBudget;
class ISaveManager;
class IResourceGenerationTracker;
class IAssetReader;
class IImageDecoder;
class ISteamBackend;
class IMobileAdapter;
class IDisplayService;
class ILifecycleService;
class IAudioFocusService;
class IMeshRenderer;
namespace carc { class ICryptoEngine; }

class BackendRegistry {
public:
    static BackendRegistry& instance();
    BackendRegistry(const BackendRegistry&) = delete;
    BackendRegistry& operator=(const BackendRegistry&) = delete;

    // -- Type-erased storage (header-only for template, needs complete type at call site)
    template<typename I>
    void setService(I* impl) {
        if constexpr (std::is_same_v<I, IVideoPlayer>) {
            if (m_videoGeneration == std::numeric_limits<uint64_t>::max())
                throw std::overflow_error("Video backend generation exhausted");
            ++m_videoGeneration; // Every registration, including same-address ABA.
        }
        m_services[std::type_index(typeid(I))] = static_cast<void*>(impl);
    }
    uint64_t videoPlayerGeneration() const noexcept { return m_videoGeneration; }

    template<typename I>
    I* getService() const {
        auto it = m_services.find(std::type_index(typeid(I)));
        return (it != m_services.end()) ? static_cast<I*>(it->second) : nullptr;
    }

    // -- Setters (out-of-line — need complete types in .cpp) --
    void setRenderDevice(IRenderDevice* device);
    void setAudioBackend(IAudioBackend* backend);
    void setAudioRestore(IAudioRestore* restore);
    void setPlatformBackend(IPlatformBackend* backend);
    void setInputRouter(IInputRouter* router);
    void setMiniGameBackend(IMiniGameBackend* backend);
    void setAnimationBackend(IAnimationBackend* be);
    void setCryptoEngine(carc::ICryptoEngine* engine);
    void setLuaManager(ILuaManager* mgr);
    void setJobSystem(IJobSystem* js);
    void setSandboxQuota(ISandboxQuota* sq);
    void setVideoPlayer(IVideoPlayer* player);
    void setTextureManager(ITextureManager* mgr);
    void setParticleSystem(IParticleSystem* ps);
    void setDebugManager(IDebugManager* dm);
    void setAsyncLoader(IAsyncLoader* al);
    void setLayerManager(ILayerManager* mgr);
    void setTextureBudget(ITextureBudget* tb);
    void setSaveManager(ISaveManager* manager);
    void setResourceGenerationTracker(IResourceGenerationTracker* tracker);
    void setAssetReader(IAssetReader* reader);
    void setImageDecoder(IImageDecoder* decoder);
    void setSteamBackend(ISteamBackend* backend);
    void setMobileAdapter(IMobileAdapter* adapter);
    void setDisplayService(IDisplayService* service);
    void setLifecycleService(ILifecycleService* service);
    void setAudioFocusService(IAudioFocusService* service);
    void setMeshRenderer(IMeshRenderer* renderer);

    // -- SandboxQuota wrappers (delegate to the registered interface) --
    bool tryAlloc(const char* kind);
    void release(const char* kind);
    // Current sandbox-quota count for `kind` (0 when no quota is installed).
    int count(const char* kind);

    // -- Getters (out-of-line — need complete types in .cpp) --
    IRenderDevice*    getRenderDevice();
    IAudioBackend*    getAudioBackend();
    IAudioRestore*    getAudioRestore();
    IPlatformBackend* getPlatformBackend();
    IInputRouter*     getInputRouter();
    IVideoPlayer*     getVideoPlayer();
    ITextureManager*  getTextureManager();
    ILayerManager*    getLayerManager();
    IParticleSystem*  getParticleSystem();
    IDebugManager*    getDebugManager();
    IAsyncLoader*     getAsyncLoader();
    IMiniGameBackend* getMiniGameBackend();
    IAnimationBackend* getAnimationBackend();
    carc::ICryptoEngine* getCryptoEngine();
    ILuaManager*      getLuaManager();
    IJobSystem*       getJobSystem();
    ISandboxQuota*    getSandboxQuota();
    ITextureBudget*   getTextureBudget();
    ISaveManager*     getSaveManager();
    IResourceGenerationTracker* getResourceGenerationTracker();
    IAssetReader* getAssetReader();
    IImageDecoder* getImageDecoder();
    ISteamBackend*    getSteamBackend();
    IMobileAdapter*   getMobileAdapter();
    IDisplayService*  getDisplayService();
    ILifecycleService* getLifecycleService();
    IAudioFocusService* getAudioFocusService();
    IMeshRenderer*    getMeshRenderer();

    // -- Factories --
    IAudioBackend*    createAudioBackend(const char* name);
    IRenderDevice*    createRenderDevice(const char* name);
    IPlatformBackend* createPlatformBackend(const char* name);

    // -- Device loss recovery listeners --
    void registerDeviceLostListener(IDeviceLostListener* listener);
    void unregisterDeviceLostListener(IDeviceLostListener* listener);
    void notifyDeviceLost();
    void notifyDeviceRestored();

    // -- Error reporter (script runtime error -> ErrorUI bridge). di is the
    //    all-knowing module and std::function carries no concrete backend
    //    dependency; the Engine (composition root) installs the real sink. --
    using ErrorReporter = std::function<void(const std::string& command,
                                             const std::string& error,
                                             const std::string& scene,
                                             int line)>;
    void setErrorReporter(ErrorReporter reporter);
    const ErrorReporter& getErrorReporter() const;

private:
    BackendRegistry() = default;
    std::unordered_map<std::type_index, void*> m_services;
    uint64_t m_videoGeneration = 0;
    std::vector<IDeviceLostListener*> m_deviceLostListeners;
    ErrorReporter m_errorReporter;
};

} // namespace Caesura
