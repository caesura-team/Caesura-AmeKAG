#include "BgfxDebugCallback.h"

void BgfxDebugCallback::screenShot(const char* name, uint32_t width, uint32_t height,
                                  uint32_t pitch, bgfx::TextureFormat::Enum format,
                                  const void* data, uint32_t size, bool flipY) {
    using Format = Caesura::ScreenshotQueue::PixelFormat;
    const auto pixelFormat = format == bgfx::TextureFormat::BGRA8 ? Format::BGRA8
                           : format == bgfx::TextureFormat::RGBA8 ? Format::RGBA8
                           : Format::Unsupported;
    m_screenshots->complete(name, width, height, pitch, pixelFormat, data, size, flipY);
}
