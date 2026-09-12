#pragma once

#include "api/IRenderDevice.h"
#include <cstdint>  // fixed-width types (GCC strict)

namespace Caesura {

class NullRenderDevice final : public IRenderDevice {public:
    NullRenderDevice();

    bool init(void* nativeWindowHandle, int width, int height) override;
    void setPresentSize(uint32_t, uint32_t) override {}
    bool isInitialized() const override { return false; }
    void beginShutdown() override;
    void shutdown() override;
    void flushAllRTT() override;
    void beginFrame() override;
    void endFrame() override;
    void commit_frame() override;
    void advanceFrame() override;
    void setScreenOffset(int, int) override {}
    void setViewRect(uint16_t viewId, uint16_t x, uint16_t y,
                     uint16_t width, uint16_t height) override;
    void setViewClear(uint16_t viewId, uint16_t flags, uint32_t rgba,
                      float depth, uint8_t stencil) override;
    void touch(uint16_t viewId) override;
    ViewportHandle createRenderTarget(int width, int height) override;
    void destroyRenderTarget(ViewportHandle handle) override;
    RenderTextureHandle getViewportTexture(ViewportHandle handle) override;
    void blitViewport(ViewportHandle handle, uint16_t targetView,
                      float x, float y, float width, float height) override;
    int getBackbufferWidth() const override;
    int getBackbufferHeight() const override;
    void resize(int width, int height) override;
    void blitTexture(uint16_t viewId, uint32_t textureId,
                     float x, float y, float width, float height,
                     uint8_t opacity) override;
    void setDebugName(uint16_t handle, const std::string& name) override;
    void drawDebugOverlay(const std::string& text) override;
    bool requestScreenshot(const std::string& path) override;
    ScreenshotResult requestScreenshot(const ScreenshotOptions& options) override;
    ScreenshotResult takeScreenshot(const ScreenshotTicket& ticket) override;
    bool cancelScreenshot(const ScreenshotTicket& ticket) override;
    bool recoverDevice(void* nativeWindowHandle, int width, int height) override;
    void flagDeviceLost() override;
    bool consumeDeviceLost() override;
    void renderText(uint16_t viewId, const std::string& text,
                    float x, float y, uint8_t r, uint8_t g,
                    uint8_t b, uint8_t a,
                    float scale = 1.0f, bool bold = false,
                    bool italic = false, bool strike = false) override;
    void renderRuby(uint16_t viewId, const std::string& base,
                    const std::string& ruby, float x, float y,
                    uint8_t r, uint8_t g, uint8_t b, uint8_t a) override;
    void setFont(int fontId) override;
    bool loadTTF(const char* path, float fontSize) override;
    FontRestoreState captureFontState() const override;
    FontRestoreState defaultFontState() const override;
    std::unique_ptr<IPreparedFontState> prepareFontState(
        const FontRestoreState& state, const uint8_t* bytes, size_t size) override;
    bool applyFontState(std::unique_ptr<IPreparedFontState> prepared) override;
    void clearFontState() override;
    void stretchBlt(uint16_t viewId, uint32_t srcTexture,
                    float srcX, float srcY, float srcWidth, float srcHeight,
                    uint32_t dstTexture, float dstX, float dstY,
                    float dstWidth, float dstHeight, int blendMode) override;
    void affineBlt(uint16_t viewId, uint32_t srcTexture,
                   float srcX, float srcY, float srcWidth, float srcHeight,
                   uint32_t dstTexture, float dstX, float dstY,
                   float dstWidth, float dstHeight,
                   const float* transform) override;
    void beginBatch() override;
    void submitBlend(uint16_t viewId, RenderTextureHandle src,
                     RenderTextureHandle dst, int mode, float opacity,
                     float x, float y) override;
    void submitTransition(uint16_t viewId, RenderTextureHandle from,
                          RenderTextureHandle to, RenderTextureHandle mask,
                          int type, float progress) override;
    void submitVFX(uint16_t viewId, RenderTextureHandle source,
                   int effect, float p0, float p1, float p2,
                   float p3, float p4, float p5, float p6) override;
    void fillViewport(ViewportHandle handle, uint8_t r, uint8_t g,
                      uint8_t b, uint8_t a) override;
    bool setColorFilter(ColorFilterPreset preset) override { return false; }
    bool isPostFxSupported(PostFxKind kind) const override { return false; }
    PostFxHandle createPostFx(PostFxKind kind, const PostFxParams& params) override { return 0; }
    void setPostFxParams(PostFxHandle handle, const PostFxParams& params) override {}
    void destroyPostFx(PostFxHandle handle) override {}
    void clearPostFx() override {}
    bool isPostFxActive() const override { return false; }
    void flushBatch() override;
    float textLineHeight() const override;
    RenderUniformHandle getDefaultSampler() const override;
    RenderProgramHandle getFallbackProgram() const override;
    RenderProgramHandle getModulatedTextureProgram() const override;
    const char* getBackendName() const override;
    RenderRuntimeInfo getRuntimeInfo() const override;
    bool setPreferredBackend(const char* backendName) override;

private:
    int m_width = 0;
    int m_height = 0;
};

} // namespace Caesura
