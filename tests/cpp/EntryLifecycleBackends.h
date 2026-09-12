#pragma once

#include "audio/api/IAudioBackend.h"
#include "live2d/api/IAnimationBackend.h"
#include "minigame/api/IMiniGameBackend.h"
#include "platform/api/IPlatformBackend.h"
#include "render/api/IRenderDevice.h"
#include "render/api/ILayerManager.h"
#include "di/api/ISandboxQuota.h"

#include <functional>

namespace Caesura::Test {

struct LifecycleProbe {
    bool initResult = true;
    int initCalls = 0;
    int shutdownCalls = 0;
    int destructorCalls = 0;
    int beginShutdownCalls = 0;
    int flushCalls = 0;
    int advanceCalls = 0;
    void* providedNativeHandle = nullptr;
    void* observedNativeHandle = reinterpret_cast<void*>(1);
    bool voicePlaying = false;
    unsigned int voiceCompletions = 0;
    int audioUpdateCalls = 0;
    int audioSuspendCalls = 0;
    int audioResumeCalls = 0;
    int isVoicePlayingCalls = 0;
    int animationRenderCalls = 0;
    float lastAnimationDt = -1.0f;
    std::function<void()> onShutdown;
};

struct ServiceProbe {
    int initCalls = 0;
    int shutdownCalls = 0;
    int destructorCalls = 0;
    int setLuaStateCalls = 0;
    int tryAllocCalls = 0;
    int releaseCalls = 0;
    lua_State* lastLuaState = nullptr;
    std::function<void(lua_State*)> onSetLuaState;
};

class LayerManagerBackend final : public ILayerManager {
public:
    explicit LayerManagerBackend(ServiceProbe& probe) : m_probe(probe) {}
    ~LayerManagerBackend() override { ++m_probe.destructorCalls; }

    void init() override { ++m_probe.initCalls; }
    void shutdown() override { ++m_probe.shutdownCalls; }
    bool configureLayers(const LayerConfig*, uint32_t) override { return true; }
    uint32_t getLayerCount() const override { return 3; }
    const char* getLayerName(uint32_t) const override { return "mock"; }
    int32_t findLayer(const char*) const override { return -1; }
    bool reorderLayer(uint32_t, uint32_t) override { return true; }
    void setTexture(uint32_t, uint32_t) override {}
    void setVisible(uint32_t, bool) override {}
    void setOpacity(uint32_t, float) override {}
    void setPosition(uint32_t, float, float) override {}
    void setScale(uint32_t, float, float) override {}
    void setBlendMode(uint32_t, int) override {}
    void clear(uint32_t) override {}
    void clearAll() override {}
    void markAllDirty() override {}
    void markDirty(uint32_t, uint16_t, uint16_t, uint16_t, uint16_t) override {}
    void markDirtyWithTransparency(
        uint32_t, uint16_t, uint16_t, uint16_t, uint16_t) override {}
    void updateDirtyRegions(uint16_t, uint16_t) override {}
    void clearDirtyRects() override {}
    void render(uint16_t, int, int, uint32_t) override {}

private:
    ServiceProbe& m_probe;
};

class SandboxQuotaBackend final : public ISandboxQuota {
public:
    explicit SandboxQuotaBackend(ServiceProbe& probe) : m_probe(probe) {}
    ~SandboxQuotaBackend() override { ++m_probe.destructorCalls; }

    void setLuaState(lua_State* L) override {
        ++m_probe.setLuaStateCalls;
        m_probe.lastLuaState = L;
        if (m_probe.onSetLuaState) m_probe.onSetLuaState(L);
    }
    bool tryAlloc(const char*) override {
        ++m_probe.tryAllocCalls;
        return true;
    }
    void release(const char*) override { ++m_probe.releaseCalls; }
    int count(const char*) override { return 0; }
    int maxLimit(const char*) override { return 0; }

private:
    ServiceProbe& m_probe;
};

class PlatformBackend final : public IPlatformBackend {
public:
    explicit PlatformBackend(LifecycleProbe& probe) : m_probe(probe) {}
    ~PlatformBackend() override { ++m_probe.destructorCalls; }

    bool init(const char*, int width, int height) override {
        ++m_probe.initCalls;
        m_width = width;
        m_height = height;
        return m_probe.initResult;
    }
    void shutdown() override { ++m_probe.shutdownCalls; }
    bool pollEvent() override { return false; }
    MouseState getMouseState() const override { return {}; }
    uint64_t getTicksMs() const override { return 0; }
    void* getNativeWindowHandle() const override { return m_probe.providedNativeHandle; }
    int getWindowWidth() const override { return m_width; }
    int getWindowHeight() const override { return m_height; }
    void setFullscreen(bool) override {}
    void resizeWindow(int width, int height) override {
        m_width = width;
        m_height = height;
    }
    const char* getBackendName() const override { return "TestPlatform"; }

    bool startTextInput() override { return true; }
    bool stopTextInput() override { return true; }
    bool setTextInputRect(int, int, int, int, int = 0) override { return true; }
    bool isTextInputActive() const override { return false; }

private:
    LifecycleProbe& m_probe;
    int m_width = 0;
    int m_height = 0;
};

class RenderDevice : public IRenderDevice {
public:
    explicit RenderDevice(LifecycleProbe& probe) : m_probe(probe) {}
    ~RenderDevice() override { ++m_probe.destructorCalls; }

    void setPresentSize(uint32_t, uint32_t) override {}
    bool init(void* nativeWindowHandle, int width, int height) override {
        ++m_probe.initCalls;
        m_probe.observedNativeHandle = nativeWindowHandle;
        m_width = width;
        m_height = height;
        return m_probe.initResult;
    }
    bool isInitialized() const override { return m_probe.initResult; }
    void beginShutdown() override { ++m_probe.beginShutdownCalls; }
    void shutdown() override {
        ++m_probe.shutdownCalls;
        if (m_probe.onShutdown) m_probe.onShutdown();
    }
    void flushAllRTT() override { ++m_probe.flushCalls; }
    void beginFrame() override {}
    void endFrame() override {}
    void commit_frame() override {}
    void advanceFrame() override { ++m_probe.advanceCalls; }
    void setScreenOffset(int, int) override {}
    void setViewRect(uint16_t, uint16_t, uint16_t, uint16_t, uint16_t) override {}
    void setViewClear(uint16_t, uint16_t, uint32_t, float, uint8_t) override {}
    void touch(uint16_t) override {}
    ViewportHandle createRenderTarget(int, int) override { return {}; }
    void destroyRenderTarget(ViewportHandle) override {}
    void blitViewport(ViewportHandle, uint16_t, float, float, float, float) override {}
    RenderTextureHandle getViewportTexture(ViewportHandle) override { return {}; }
    int getBackbufferWidth() const override { return m_width; }
    int getBackbufferHeight() const override { return m_height; }
    void resize(int width, int height) override {
        m_width = width;
        m_height = height;
    }
    void blitTexture(uint16_t, uint32_t, float, float, float, float, uint8_t) override {}
    void stretchBlt(uint16_t, uint32_t, float, float, float, float,
                    uint32_t, float, float, float, float, int) override {}
    void affineBlt(uint16_t, uint32_t, float, float, float, float,
                   uint32_t, float, float, float, float, const float[6]) override {}
    void beginBatch() override {}
    void flushBatch() override {}
    void setDebugName(uint16_t, const std::string&) override {}
    void drawDebugOverlay(const std::string&) override {}
    bool requestScreenshot(const std::string&) override { return false; }
    Caesura::ScreenshotResult requestScreenshot(const Caesura::ScreenshotOptions&) override {
        Caesura::ScreenshotResult result;
        result.status = Caesura::ScreenshotStatus::Failed;
        result.error = "screenshots unsupported by test renderer";
        return result;
    }
    Caesura::ScreenshotResult takeScreenshot(const Caesura::ScreenshotTicket&) override { return {}; }
    bool cancelScreenshot(const Caesura::ScreenshotTicket&) override { return false; }
    bool recoverDevice(void*, int width, int height) override {
        m_width = width;
        m_height = height;
        return true;
    }
    void flagDeviceLost() override {}
    bool consumeDeviceLost() override { return false; }
    void renderText(uint16_t, const std::string&, float, float,
                    uint8_t, uint8_t, uint8_t, uint8_t,
                    float, bool, bool, bool) override {}
    void renderRuby(uint16_t, const std::string&, const std::string&, float, float,
                    uint8_t, uint8_t, uint8_t, uint8_t) override {}
    void setFont(int) override {}
    bool loadTTF(const char*, float) override { return false; }
    FontRestoreState captureFontState() const override { return {}; }
    FontRestoreState defaultFontState() const override { return {}; }
    std::unique_ptr<IPreparedFontState> prepareFontState(const FontRestoreState&, const uint8_t*, size_t) override { return {}; }
    bool applyFontState(std::unique_ptr<IPreparedFontState>) override { return false; }
    void clearFontState() override {}
    float textLineHeight() const override { return 0.0f; }
    void submitBlend(uint16_t, RenderTextureHandle, RenderTextureHandle, int,
                     float, float, float) override {}
    void submitTransition(uint16_t, RenderTextureHandle, RenderTextureHandle,
                          RenderTextureHandle, int, float) override {}
    void submitVFX(uint16_t, RenderTextureHandle, int, float, float, float,
                   float, float, float, float) override {}
    void fillViewport(ViewportHandle, uint8_t, uint8_t, uint8_t, uint8_t) override {}
    bool setColorFilter(ColorFilterPreset) override { return true; }

    // -- post-processing chain (round 102) -- test double no-op (unsupported)
    bool isPostFxSupported(PostFxKind) const override { return false; }
    PostFxHandle createPostFx(PostFxKind, const PostFxParams&) override { return 0; }
    void setPostFxParams(PostFxHandle, const PostFxParams&) override {}
    void destroyPostFx(PostFxHandle) override {}
    void clearPostFx() override {}
    bool isPostFxActive() const override { return false; }
    RenderUniformHandle getDefaultSampler() const override { return {}; }
    RenderProgramHandle getFallbackProgram() const override { return {}; }
    RenderProgramHandle getModulatedTextureProgram() const override { return {}; }
    const char* getBackendName() const override { return "TestRender"; }
    RenderRuntimeInfo getRuntimeInfo() const override {
        return {"TestRender", m_width, m_height, 0, true};
    }
    bool setPreferredBackend(const char*) override { return false; }

private:
    LifecycleProbe& m_probe;
    int m_width = 0;
    int m_height = 0;
};

class AudioBackend final : public IAudioBackend {
public:
    explicit AudioBackend(LifecycleProbe& probe) : m_probe(probe) {}
    ~AudioBackend() override { ++m_probe.destructorCalls; }

    bool init() override {
        ++m_probe.initCalls;
        m_initialized = m_probe.initResult;
        return m_initialized;
    }
    void shutdown() override {
        ++m_probe.shutdownCalls;
        m_initialized = false;
        if (m_probe.onShutdown) m_probe.onShutdown();
    }
    bool isPlaybackAvailable() const override { return m_initialized; }
    void update(float) override { ++m_probe.audioUpdateCalls; }
    void suspend() override { ++m_probe.audioSuspendCalls; }
    void resume() override { ++m_probe.audioResumeCalls; }
    unsigned int playBGM(const std::string&, float) override { return 0; }
    void stopBGM(float) override {}
    unsigned int playVoice(const std::string&) override { return 0; }
    void stopVoice() override {}
    unsigned int playSE(const std::string&) override { return 0; }
    unsigned int playRawPCM(const float*, unsigned int, unsigned int, unsigned int) override { return 0; }
    unsigned int playSE3D(const std::string&, float, float, float) override { return 0; }
    void stopSE() override {}
    void setSEVolume(unsigned int, float) override {}
    float getSEVolume(unsigned int) override { return 0.0f; }
    void stopSEHandle(unsigned int) override {}
    void update3dListener(float, float, float, float, float, float,
                          float, float, float) override {}
    void setGlobalVolume(float) override {}
    float getGlobalVolume() const override { return 1.0f; }
    void setBusVolume(const char*, float) override {}
    float getBusVolume(const char*) const override { return 1.0f; }
    void flushWaveCache() override {}
    unsigned int consumeVoiceCompletions() override {
        const unsigned int completions = m_probe.voiceCompletions;
        m_probe.voiceCompletions = 0;
        return completions;
    }
    bool isVoicePlaying() override {
        ++m_probe.isVoicePlayingCalls;
        return m_probe.voicePlaying;
    }
    bool isBGMPlaying() override { return false; }
    bool isSEPlaying() override { return false; }
    int activeVoiceCount() override { return 0; }
    float getPosition(const char*) override { return 0.0f; }
    float getLength(const char*) override { return 0.0f; }
    void fadeVolume(const char*, float, float) override {}
    const char* getBackendName() const override { return "TestAudio"; }

private:
    LifecycleProbe& m_probe;
    bool m_initialized = false;
};

class MiniGameBackend final : public IMiniGameBackend {
public:
    explicit MiniGameBackend(LifecycleProbe& probe) : m_probe(probe) {}
    ~MiniGameBackend() override { ++m_probe.destructorCalls; }

    bool init() override { ++m_probe.initCalls; return m_probe.initResult; }
    void shutdown() override { ++m_probe.shutdownCalls; }
    uint32_t loadScene(const std::string&) override { return 0; }
    void unloadScene(uint32_t) override {}
    void enter(uint32_t) override {}
    void leave() override {}
    bool isActive() const override { return false; }
    bool update(float) override { return true; }
    void render() override {}
    bool processEvent(const void*) override { return false; }
    int luaCall(lua_State*, const char*) override { return 0; }
    void setRenderDevice(IRenderDevice*) override {}
    const char* getBackendName() const override { return "TestMiniGame"; }

private:
    LifecycleProbe& m_probe;
};

class AnimationBackend final : public IAnimationBackend {
public:
    explicit AnimationBackend(LifecycleProbe& probe) : m_probe(probe) {}
    ~AnimationBackend() override { ++m_probe.destructorCalls; }

    bool init() override { ++m_probe.initCalls; return m_probe.initResult; }
    void shutdown() override {
        ++m_probe.shutdownCalls;
        if (m_probe.onShutdown) m_probe.onShutdown();
    }
    // The lifecycle fixture has no Cubism implementation.
    bool isCubismAvailable() const override { return false; }
    int loadModel(const std::string&, const std::string&) override { return 0; }
    void unloadModel(int) override {}
    bool isLoaded(int) const override { return false; }
    std::size_t loadedModelCount() const override { return 0; }
    void clearModels() override {}
    void showModel(int, float, float, float) override {}
    void hideModel(int) override {}
    void setOpacity(int, float) override {}
    void render(float dt) override {
        ++m_probe.animationRenderCalls;
        m_probe.lastAnimationDt = dt;
    }
    bool playMotion(int, const std::string&) override { return false; }
    void setExpression(int, const std::string&) override {}
    void setParameter(int, const std::string&, float) override {}
    const char* name() const override { return "TestAnimation"; }

private:
    LifecycleProbe& m_probe;
};

} // namespace Caesura::Test
