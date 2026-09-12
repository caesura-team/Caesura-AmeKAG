#include "BgfxDebugCallback.h"

void BgfxDebugCallback::traceVargs(const char* path, uint16_t line, const char* format, va_list arguments) {
    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer), format, arguments);
#if defined(CAESURA_RENDER_TEST_FAULTS)
    // The explicit render-test configuration preserves native diagnostics even
    // when normal debug logging is compiled out. No effect on callback state.
    std::fprintf(stderr, "[bgfx trace] %s(%u): %s", path, unsigned(line), buffer);
#else
    DEBUG_DBG(Caesura::SubSys::Render, Caesura::ErrCode::Ok, "[bgfx] %s(%d): %s", path, int(line), buffer);
#endif
}

void BgfxDebugCallback::screenShot(const char* name, uint32_t width, uint32_t height,
                                  uint32_t pitch, bgfx::TextureFormat::Enum format,
                                  const void* data, uint32_t size, bool flipY) {
#if defined(CAESURA_RENDER_TEST_FAULTS)
    std::fprintf(stderr, "[capture input] name=%s native=%ux%u pitch=%u flipY=%d\n",
        name ? name : "<null>", width, height, pitch, int(flipY));
#endif
    using Format = Caesura::ScreenshotQueue::PixelFormat;
    const auto pixelFormat = format == bgfx::TextureFormat::BGRA8 ? Format::BGRA8
                           : format == bgfx::TextureFormat::RGBA8 ? Format::RGBA8
                           : Format::Unsupported;
    m_screenshots->complete(name, width, height, pitch, pixelFormat, data, size, flipY);
}
