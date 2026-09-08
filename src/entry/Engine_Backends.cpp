#include "../audio/NullAudioBackend.h"
#include "../live2d/NullAnimationBackend.h"
#include "../minigame/NullMiniGameBackend.h"
#include "../steam/NullSteamBackend.h"
#include "../render/ParticleSystem.h"
#include "../render/NullRenderDevice.h"
#include "../render/BgfxRenderDevice.h"
#include "../platform/NullPlatformBackend.h"
#include "../resource/ResourceHandle.h"
#include "../di/TextureBudget.h"
#include "../render/TextureManager.h"
#include "../render/LayerManager.h"
#include "../di/SandboxQuota.h"
#include "../resource/AssetManager.h"
#include "../resource/ImageDecoder.h"
#include "../resource/AsyncLoader.h"
#include "../job/JobSystem.h"
#include "../storage/SaveManager.h"
#include "../archive/CryptoEngine.h"

#ifdef CAESURA_LIVE2D
#include "../live2d/Live2D/Live2DBackend.h"
#endif

#ifdef CAESURA_HAS_STEAM
#include "../steam/SteamBackend.h"
#endif

#include <memory>

namespace Caesura {

class IRenderDevice;

std::unique_ptr<ISteamBackend> createDefaultSteamIntegration() {
#ifdef CAESURA_HAS_STEAM
    return std::make_unique<SteamBackend>();
#else
    return std::make_unique<NullSteamBackend>();
#endif
}

std::unique_ptr<IAudioBackend> createHeadlessAudioBackend() {
    return std::make_unique<NullAudioBackend>();
}

std::unique_ptr<IRenderDevice> createHeadlessRenderDevice() {
    return std::make_unique<NullRenderDevice>();
}

std::unique_ptr<IPlatformBackend> createHeadlessPlatformBackend() {
    return std::make_unique<NullPlatformBackend>();
}

std::unique_ptr<IMiniGameBackend> createFallbackMiniGameBackend() {
    return std::make_unique<NullMiniGameBackend>();
}

std::unique_ptr<IParticleSystem> createParticleSystem() {
    return std::make_unique<ParticleSystem>();
}

std::unique_ptr<IResourceGenerationTracker> createResourceGenerationTracker() {
    return std::make_unique<GenerationTracker>();
}

std::unique_ptr<ITextureBudget> createTextureBudget() {
    return std::make_unique<TextureBudget>();
}

std::unique_ptr<ITextureManager> createTextureManager() {
    return std::make_unique<TextureManager>();
}

bool initializeTextureManager(ITextureManager& textures, IRenderDevice* renderer,
                              bool gpuMode) {
    // This concrete texture service owns bgfx handles. An injected renderer can
    // render editor frames without initializing bgfx, just as with LayerManager.
    return textures.initialize(gpuMode && dynamic_cast<BgfxRenderDevice*>(renderer) != nullptr);
}

std::unique_ptr<ILayerManager> createLayerManager(IRenderDevice* renderDevice) {
    const bool gpuEnabled = dynamic_cast<BgfxRenderDevice*>(renderDevice) != nullptr;
    return std::make_unique<LayerManager>(gpuEnabled);
}

std::unique_ptr<ISandboxQuota> createSandboxQuota() {
    return std::make_unique<SandboxQuotaService>();
}

std::unique_ptr<AssetManager> createAssetManager() {
    return std::make_unique<AssetManager>();
}

std::unique_ptr<IImageDecoder> createImageDecoder() {
    return std::make_unique<CpuImageDecoder>();
}

std::unique_ptr<IAsyncLoader> createAsyncLoader(AssetManager* assetManager) {
    return std::make_unique<AsyncLoader>(assetManager);
}

std::unique_ptr<IJobSystem> createJobSystem() {
    return std::make_unique<JobSystem>();
}

std::unique_ptr<ISaveManager> createSaveManager() {
    return std::make_unique<SaveManager>();
}

std::unique_ptr<carc::ICryptoEngine> createCryptoEngine() {
    return std::make_unique<carc::CryptoEngine>();
}

std::unique_ptr<IAnimationBackend> createDefaultAnimationBackend() {
#ifdef CAESURA_LIVE2D
    return std::make_unique<Live2DBackend>();
#else
    return std::make_unique<NullAnimationBackend>();
#endif
}

std::unique_ptr<IAnimationBackend> createFallbackAnimationBackend() {
    return std::make_unique<NullAnimationBackend>();
}

void attachRenderDeviceToAnimationBackend(IAnimationBackend* animationBackend,
                                          IRenderDevice* renderDevice) {
#ifdef CAESURA_LIVE2D
    if (auto* live2d = dynamic_cast<Live2DBackend*>(animationBackend)) {
        live2d->setRenderDevice(renderDevice);
    }
#else
    (void)animationBackend;
    (void)renderDevice;
#endif
}

} // namespace Caesura
