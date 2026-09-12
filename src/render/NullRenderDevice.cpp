#include "NullRenderDevice.h"

#include <cstdio>

namespace Caesura {

NullRenderDevice::NullRenderDevice() {
    std::printf("[Render] Using NullRenderDevice.\n");
}

bool NullRenderDevice::init(void*, int width, int height) {
    m_width = width;
    m_height = height;
    return true;
}

void NullRenderDevice::beginShutdown() {}
void NullRenderDevice::shutdown() {}
void NullRenderDevice::flushAllRTT() {}
void NullRenderDevice::beginFrame() {}
void NullRenderDevice::endFrame() {}
void NullRenderDevice::commit_frame() {}
void NullRenderDevice::advanceFrame() {}
void NullRenderDevice::setViewRect(uint16_t, uint16_t, uint16_t, uint16_t, uint16_t) {}
void NullRenderDevice::setViewClear(uint16_t, uint16_t, uint32_t, float, uint8_t) {}
void NullRenderDevice::touch(uint16_t) {}
ViewportHandle NullRenderDevice::createRenderTarget(int, int) { return {}; }
void NullRenderDevice::destroyRenderTarget(ViewportHandle) {}
RenderTextureHandle NullRenderDevice::getViewportTexture(ViewportHandle) { return {}; }
void NullRenderDevice::blitViewport(ViewportHandle, uint16_t, float, float, float, float) {}
int NullRenderDevice::getBackbufferWidth() const { return m_width; }
int NullRenderDevice::getBackbufferHeight() const { return m_height; }
void NullRenderDevice::resize(int width, int height) {
    m_width = width;
    m_height = height;
}
void NullRenderDevice::blitTexture(uint16_t, uint32_t, float, float, float, float, uint8_t) {}
void NullRenderDevice::setDebugName(uint16_t, const std::string&) {}
void NullRenderDevice::drawDebugOverlay(const std::string&) {}
bool NullRenderDevice::requestScreenshot(const std::string&) { return false; }
ScreenshotResult NullRenderDevice::requestScreenshot(const ScreenshotOptions&) {
    ScreenshotResult result;
    result.status = ScreenshotStatus::Failed;
    result.error = "screenshots unsupported by headless renderer";
    return result;
}
ScreenshotResult NullRenderDevice::takeScreenshot(const ScreenshotTicket&) { return {}; }
bool NullRenderDevice::cancelScreenshot(const ScreenshotTicket&) { return false; }
bool NullRenderDevice::recoverDevice(void*, int width, int height) {
    m_width = width;
    m_height = height;
    return true;
}
void NullRenderDevice::flagDeviceLost() {}
bool NullRenderDevice::consumeDeviceLost() { return false; }
void NullRenderDevice::renderText(uint16_t, const std::string&, float, float,
                                  uint8_t, uint8_t, uint8_t, uint8_t,
                                  float, bool, bool, bool) {}
void NullRenderDevice::renderRuby(uint16_t, const std::string&, const std::string&,
                                  float, float, uint8_t, uint8_t, uint8_t, uint8_t) {}
void NullRenderDevice::setFont(int) {}
bool NullRenderDevice::loadTTF(const char*, float) { return false; }
void NullRenderDevice::stretchBlt(uint16_t, uint32_t, float, float, float, float,
                                  uint32_t, float, float, float, float, int) {}
void NullRenderDevice::affineBlt(uint16_t, uint32_t, float, float, float, float,
                                 uint32_t, float, float, float, float,
                                 const float*) {}
void NullRenderDevice::beginBatch() {}
void NullRenderDevice::submitBlend(uint16_t, RenderTextureHandle,
                                   RenderTextureHandle, int, float, float, float) {}
void NullRenderDevice::submitTransition(uint16_t, RenderTextureHandle,
                                        RenderTextureHandle, RenderTextureHandle,
                                        int, float) {}
void NullRenderDevice::submitVFX(uint16_t, RenderTextureHandle, int, float,
                                 float, float, float, float, float, float) {}
void NullRenderDevice::fillViewport(ViewportHandle, uint8_t, uint8_t, uint8_t, uint8_t) {}
void NullRenderDevice::flushBatch() {}
float NullRenderDevice::textLineHeight() const { return 0.0f; }
RenderUniformHandle NullRenderDevice::getDefaultSampler() const { return {}; }
RenderProgramHandle NullRenderDevice::getFallbackProgram() const { return {}; }
RenderProgramHandle NullRenderDevice::getModulatedTextureProgram() const { return {}; }
const char* NullRenderDevice::getBackendName() const { return "NullRender"; }

RenderRuntimeInfo NullRenderDevice::getRuntimeInfo() const {
    RenderRuntimeInfo info;
    info.backendName = getBackendName();
    info.width = m_width;
    info.height = m_height;
    return info;
}

bool NullRenderDevice::setPreferredBackend(const char*) { return false; }

} // namespace Caesura
