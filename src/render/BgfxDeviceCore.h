#pragma once

#include <bgfx/bgfx.h>
#include "api/IRenderDevice.h"
#include <unordered_map>
#include <cstdint>
#include "BgfxDebugCallback.h"

namespace Caesura {

class BgfxDeviceCore {
public:
#if defined(__ANDROID__)
    static void setOverrideGLContext(void* ctx);
#endif
public:
    explicit BgfxDeviceCore(std::shared_ptr<ScreenshotQueue> screenshots = std::make_shared<ScreenshotQueue>())
        : m_callback(std::move(screenshots)) {}
    ~BgfxDeviceCore();

    BgfxDeviceCore(const BgfxDeviceCore&) = delete;
    BgfxDeviceCore& operator=(const BgfxDeviceCore&) = delete;

    static constexpr uint16_t VIEW_RTT        = 0;
    static constexpr uint16_t VIEW_MAIN       = 1;
    static constexpr uint16_t VIEW_DEBUG      = 2;
    static constexpr uint16_t VIEW_TRANSITION = 3;
    // Round-102 post-processing chain composite view (order: RTT -> MAIN
    // -> POSTFX -> DEBUG -> TRANSITION). Scene renders to the internal
    // scene RTT under VIEW_MAIN; POSTFX composites sceneRtt -> backbuffer.
    static constexpr uint16_t VIEW_POSTFX    = 40;

    static bool setPreferredBackend(const char* name);
    // t92: per-platform DEFAULT backend used when no --backend override is
    // given (_WIN32 -> Direct3D11, __APPLE__ -> Metal, else -> OpenGL).
    // setPreferredBackend (--backend) still takes precedence; bgfx's own
    // auto-select retry (RendererType::Count) remains a fallback only.
    static bgfx::RendererType::Enum platformDefaultBackend();
    const char* getBackendName() const;

    bool init(void* nativeWindowHandle, int width, int height);
    // Screen-offset pan (camera/quakes): shifts VIEW_MAIN's rect each frame.
    void setScreenOffset(int dx, int dy) { m_screenOffsetX = dx; m_screenOffsetY = dy; }
    void resize(int width, int height);
    void shutdown();
    void beginFrame();
    void endFrame();
    void commit_frame();
    void advanceFrame();
    void beginShutdown() { m_callback.beginShutdown(); }
    void flagDeviceLost() { m_callback.flagDeviceLost(); }
    bool consumeDeviceLost() { return m_callback.consumeDeviceLost(); }
    bool deviceLost() const { return m_callback.deviceLost(); }
    uint32_t presentWidth() const { return m_backbufferW ? m_backbufferW : static_cast<uint32_t>(m_width); }
    uint32_t presentHeight() const { return m_backbufferH ? m_backbufferH : static_cast<uint32_t>(m_height); }
    bool hasExplicitPresentSize() const { return m_presentSizeExplicit; }
    // Present surface size (see IRenderDevice::setPresentSize).
    void setPresentSize(uint16_t w, uint16_t h);
    void setViewRect(uint16_t v, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
    void setViewClear(uint16_t v, uint16_t f, uint32_t c, float d, uint8_t s);
    void touch(uint16_t v);
    void setDebugName(uint16_t v, const std::string& n);
    ViewportHandle createRenderTarget(int w, int h);
    void destroyRenderTarget(ViewportHandle h);
    bgfx::TextureHandle getViewportTexture(ViewportHandle h);
    bgfx::FrameBufferHandle getRttFb(ViewportHandle h);
    void flushAllRTT();
    // Cached 1x1 solid-color texture (fillViewport hot path): created once
    // per color, destroyed with the other RTT resources. fillViewport used
    // to create a texture EVERY call (per-frame GPU churn).
    bgfx::TextureHandle getSolidPixel(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    int getWidth()  const { return m_width; }
    int getHeight() const { return m_height; }

    // Accessibility color filter (Neo-Genesis): the active preset matrix
    // is filled into effect-4 full-screen VFX passes. nullptr = no filter.
    void setColorFilterMatrix(const float* m) {
        if (m) {
            for (int i = 0; i < 9; ++i) m_colorFilter[i] = m[i];
            m_colorFilterActive = true;
        } else {
            m_colorFilterActive = false;
        }
    }
    const float* getColorFilterMatrix() const {
        return m_colorFilterActive ? m_colorFilter : nullptr;
    }

private:
    // Owned through bgfx::shutdown(), including its final callback drain.
    BgfxDebugCallback m_callback;
    float  m_colorFilter[9] = { 0.0f };
    bool   m_colorFilterActive = false;

private:
    int m_screenOffsetX = 0;
    int m_screenOffsetY = 0;
    void setupDefaultViews();
    void updateBackbufferSize();
    int m_width  = 1280;
    int m_height = 720;
    // Actual present surface (bgfx backbuffer) pixels. On desktop it equals
    // the logical size; on Android the OS surface (e.g. 2320x956) differs
    // from the configured engine resolution (1920x1080), so the MAIN/DEBUG
    // view rects use this size while every projection stays in LOGICAL
    // coordinates -- the game content stretches to fill the display.
    uint16_t m_backbufferW = 0;
    uint16_t m_backbufferH = 0;
    bool m_presentSizeExplicit = false;
    bool m_bgfxInitialized = false;
    std::string m_backendName = "bgfx";
    bool m_shutdownComplete = false;
    struct RTTEntry { bgfx::FrameBufferHandle fb = BGFX_INVALID_HANDLE; bgfx::TextureHandle tex = BGFX_INVALID_HANDLE; uint16_t viewId = VIEW_RTT; };
    uint32_t m_nextHandle = 1;
    std::unordered_map<uint32_t, RTTEntry> m_rttMap;
    bgfx::TextureHandle m_solidPixel = BGFX_INVALID_HANDLE;
    uint32_t            m_solidPixelKey = 0;
};

} // namespace Caesura
