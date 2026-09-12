 #include "BgfxRenderDevice.h"
#include "BgfxDebugCallback.h"
#include "ShaderCache.h"
#include "ColorFilterMath.h"
#include "../di/api/ThreadAssert.h"
#include <bgfx/bgfx.h>
// #include <bgfx/embedded_shader.h> -- using bgfx::createShader with raw bytecode instead
#include <bx/math.h>
#include <bgfx/platform.h>
#include <bimg/decode.h>
#include <bx/bx.h>
#include <bx/readerwriter.h>
#include <bx/error.h>
#include <cstdio>
#include <cstring>
#include <algorithm>



namespace Caesura {

namespace {

RenderTextureHandle toRenderHandle(bgfx::TextureHandle handle) {
    return bgfx::isValid(handle) ? RenderTextureHandle{handle.idx} : RenderTextureHandle{};
}

RenderProgramHandle toRenderHandle(bgfx::ProgramHandle handle) {
    return bgfx::isValid(handle) ? RenderProgramHandle{handle.idx} : RenderProgramHandle{};
}

RenderUniformHandle toRenderHandle(bgfx::UniformHandle handle) {
    return bgfx::isValid(handle) ? RenderUniformHandle{handle.idx} : RenderUniformHandle{};
}

bgfx::TextureHandle toBgfx(RenderTextureHandle handle) {
    if (!handle.isValid()) return BGFX_INVALID_HANDLE;
    bgfx::TextureHandle result;
    result.idx = handle.idx;
    return result;
}

} // namespace

BgfxRenderDevice::~BgfxRenderDevice() {
    shutdown();
}

bool BgfxRenderDevice::setShaderTestFault(ShaderTestFault fault) {
    if (m_bgfxInitialized || m_deviceCore || !BgfxShaderManager::supportsTestFault(fault)) return false;
    m_shaderTestFault = fault;
    return true;
}

ShaderBuildReport BgfxRenderDevice::shaderBuildReport() const {
    return m_shaders ? m_shaders->buildReport() : ShaderBuildReport{};
}


void BgfxRenderDevice::flushAllRTT() {
    if (m_bgfxInitialized && m_deviceCore) m_deviceCore->flushAllRTT();
}


// ===========================================================================
//  Batch protocol (spec [0.3]): beginBatch / flushBatch
// ===========================================================================

// ===========================================================================
//  Batch protocol (spec [0.3]): beginBatch / flushBatch
//  Defers GPU submission to batch many draw calls into one vertex buffer.
// ===========================================================================


void BgfxRenderDevice::beginBatch() { if (canRender() && m_draw) m_draw->beginBatch(); }


void BgfxRenderDevice::flushBatch() { if (canRender() && m_draw) m_draw->flushBatch(); }


// ===========================================================================
// Backend preference helpers
// These correspond to the original objective's requirement for explicit
// DX12 / Metal / WebGPU backend stubs. bgfx handles the actual backend
// internally; these helpers expose the selection API.

// (P2) backend preference is owned by BgfxDeviceCore; the copy here was dead.

// setPreferredBackend extracted to BgfxDeviceCore


// getBackendName extracted to BgfxDeviceCore


#if defined(__ANDROID__)
extern "C" void* caesuraAndroidGLContext();  // set by Engine from SDL3PlatformBackend
#endif

bool BgfxRenderDevice::init(void* nativeWindowHandle, int width, int height) {
    if (m_bgfxInitialized || width <= 0 || height <= 0) return false;
    m_screenshots->close("renderer initializing");
    m_stopping = false;
    m_recovering = false;
    m_recoveryFailed = false;
    m_frameFinalized = false;
    m_shaderRenderingDisabled = false;
#if defined(__ANDROID__)
    BgfxDeviceCore::setOverrideGLContext(caesuraAndroidGLContext());
#endif
    m_bgfxInitialized = false;
    m_shutdownComplete = false;
    m_shaders = std::make_unique<BgfxShaderManager>();
    if (!m_shaders->setTestFault(m_shaderTestFault)) return false;
    m_deviceCore = std::make_unique<BgfxDeviceCore>(m_screenshots);
    if (!m_deviceCore->init(nativeWindowHandle, width, height)) {
        m_deviceCore.reset();
        m_shaders.reset();
        return false;
    }
    m_bgfxInitialized = true;
    m_shaders->initEmbeddedShaders();
    // t73 (b): never submit a half-broken program. When any core embedded
    // shader failed to build (manager already printed the one-shot ERROR),
    // switch bgfx to IFH so the frame loop keeps running with rendering
    // skipped -- verify probes distinguish "alive, rendering disabled"
    // from a hard crash.
    if (m_shaders->coreProgramsBroken()) {
        // t75: only CORE-program failures disable rendering. Non-core failures
        // (vfx/transition/stretch/affine/postfx) are counted + logged loudly by
        // the manager; their draw sites already guard + skip, so rendering must
        // continue (a single broken VFX shader must not black the screen).
        printf("[RENDER][ERROR] [BgfxRenderDevice] CORE shader programs broken; "
               "rendering disabled (BGFX_DEBUG_IFH) - frame loop continues.\n");
        bgfx::setDebug(BGFX_DEBUG_IFH);
        m_shaderRenderingDisabled = true;
    }
    m_drawState.shaders = m_shaders.get();
    m_drawState.device  = m_deviceCore.get();
    m_draw = std::make_unique<BgfxDraw>();
    m_draw->init(&m_drawState);
    m_textRenderer = std::make_unique<TextRenderer>();
    if (!m_textRenderer->init(this)) { m_textRenderer.reset(); }
    if (!m_shaders->coreProgramsBroken() && bgfx::getCaps()->rendererType != bgfx::RendererType::Noop
        && !m_deviceCore->deviceLost()) m_screenshots->open();
    return true;
}

void BgfxRenderDevice::beginShutdown() {
    m_stopping = true;
    m_screenshots->close("renderer shutting down");
    if (m_deviceCore) m_deviceCore->beginShutdown();
}

void BgfxRenderDevice::setPresentSize(uint32_t width, uint32_t height) {
    if (canRender() && width > 0 && height > 0 && width <= UINT16_MAX && height <= UINT16_MAX)
        m_deviceCore->setPresentSize(uint16_t(width), uint16_t(height));
}

void BgfxRenderDevice::resize(int width, int height) {
    if (!canRender()) return;
    m_deviceCore->resize(width, height);
    if (m_textRenderer) m_textRenderer->setScreenSize(m_deviceCore->getWidth(), m_deviceCore->getHeight());
    // Scene RTT + chain scratch targets are size-matched to the backbuffer;
    // rebuild them lazily on the next chain frame (beginFrame/runPostFxChain
    // detect the size change and recreate). Garbage-collect the old ones now.
    destroyPostFxResources();
}


void BgfxRenderDevice::shutdown() {
    if (m_shutdownComplete) return;
    beginShutdown();
    m_shutdownComplete = true;
    if (m_bgfxInitialized) {
        // Release chain RTTs while the GPU context is still alive.
        destroyPostFxResources();
    }
    if (m_textRenderer) m_textRenderer.reset();
    m_draw.reset();
    m_shaders.reset();
    if (m_deviceCore) m_deviceCore->shutdown();
    m_bgfxInitialized = false;
}




// Helper: construct a valid bgfx shader binary (version 11) from raw
// DXBC / SPIR-V bytecode. bgfx encodes its own header in front of the
// platform-specific code so the runtime can reflect on uniforms and
// input attributes without invoking the platform compiler.
//
// bgfx binary format (shader version >= 10):
//   uint32_t  magic          VSH/FSH/CSH + version byte (11)
//   uint32_t  hashIn
//   uint32_t  hashOut
//   uint16_t  uniformCount
//   ...       uniforms       (omitted when count == 0)
//   uint32_t  codeSize
//   uint8_t   code[codeSize]
//   uint8_t   padding        (1 byte)
//   uint8_t   numAttrs
//   uint16_t  attrIds[numAttrs]
//   uint16_t  cbSize         constant-buffer size, 0 when none




// initEmbeddedShaders
// Picks the correct embedded bytecode (SPIR-V for Vulkan, DXBC for
// D3D11/D3D12), wraps it in a proper bgfx binary header via
// buildBgfxShader(), and registers the resulting program as the
// engine-wide fallback for 2-D quad rendering and RTT blits.




//setupDefaultViews      configure the three View layers
//   T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T

// setupDefaultViews extracted to BgfxDeviceCore



//   T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T
// Frame-management pass-throughs
//   T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T

void BgfxRenderDevice::beginFrame() {
    if (canRender()) {
        m_frameFinalized = false;
        m_deviceCore->beginFrame();
        // View state applies to the entire submitted frame in bgfx. Select its
        // destination here and leave it intact until the owner advances it.
        bgfx::setViewFrameBuffer(BgfxDeviceCore::VIEW_MAIN, BGFX_INVALID_HANDLE);
        m_chainRetargeted = false;
        // Round-102 post-process chain: while active the whole frame's
        // VIEW_MAIN draws are redirected to the internal scene RTT. This must
        // be set before any VIEW_MAIN submit this frame, so it lives here at
        // frame start (not in commit_frame, which runs after the scene draws).
        if (isPostFxActive()) {
            const int W = m_deviceCore->getWidth();
            const int H = m_deviceCore->getHeight();
            if (!bgfx::isValid(m_sceneRtt) || m_sceneRttW != W || m_sceneRttH != H) {
                if (bgfx::isValid(m_sceneRtt)) {
                    bgfx::destroy(m_sceneRtt);
                    m_sceneRtt = BGFX_INVALID_HANDLE;
                }
                bgfx::TextureHandle tex = bgfx::createTexture2D(
                    static_cast<uint16_t>(W), static_cast<uint16_t>(H), false, 1,
                    bgfx::TextureFormat::RGBA8,
                    BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
                if (bgfx::isValid(tex)) {
                    m_sceneRtt = bgfx::createFrameBuffer(1, &tex, true);
                    m_sceneRttW = W; m_sceneRttH = H;
                }
            }
            if (bgfx::isValid(m_sceneRtt)) {
                bgfx::setViewFrameBuffer(BgfxDeviceCore::VIEW_MAIN, m_sceneRtt);
                bgfx::setViewRect(BgfxDeviceCore::VIEW_MAIN, 0, 0,
                    static_cast<uint16_t>(W), static_cast<uint16_t>(H));
                m_chainRetargeted = true;
            }
        }
    }
}


void BgfxRenderDevice::endFrame() {
    commit_frame();
    advanceFrame();
}


void BgfxRenderDevice::commit_frame() {
    if (!canRender() || m_frameFinalized) return;
    // The stage list can be cleared after beginFrame selected the scene RTT.
    // Such a frame still needs an identity composite to the backbuffer.
    if (m_chainRetargeted) runPostFxChain();
    m_frameFinalized = true;
}


void BgfxRenderDevice::advanceFrame() {
    if (!m_bgfxInitialized || !m_deviceCore || m_recovering || (m_recoveryFailed && !m_stopping)
        || m_deviceCore->deviceLost()) return;
    // beginShutdown closes capture admission but allows the Engine's two explicit
    // resource-destruction drains while the context is still alive, including
    // when core recreation succeeded but font/program restoration failed.
    // A missing context, active recovery, or latched device loss still rejects.
    if (!m_stopping) {
        for (const auto& submission : m_screenshots->submit(++m_frameId)) {
            if (!canRender()) break;
            bgfx::requestScreenShot(BGFX_INVALID_HANDLE, submission.callbackName.c_str());
        }
    }
    m_deviceCore->advanceFrame();
    m_frameFinalized = false;
}



//   T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T
// View-management pass-throughs
//   T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T

void BgfxRenderDevice::setScreenOffset(int dx, int dy) {
    if (m_deviceCore) m_deviceCore->setScreenOffset(dx, dy);
}

void BgfxRenderDevice::setViewRect(uint16_t v, uint16_t x, uint16_t y, uint16_t w, uint16_t h) { if (canRender()) m_deviceCore->setViewRect(v, x, y, w, h); }


void BgfxRenderDevice::setViewClear(uint16_t v, uint16_t f, uint32_t c, float d, uint8_t s) { if (canRender()) m_deviceCore->setViewClear(v, f, c, d, s); }


void BgfxRenderDevice::touch(uint16_t v) { if (canRender()) m_deviceCore->touch(v); }



//   T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T
// createRenderTarget / destroyRenderTarget / blitViewport
//   T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T  T

ViewportHandle BgfxRenderDevice::createRenderTarget(int w, int h) { return canRender() ? m_deviceCore->createRenderTarget(w, h) : ViewportHandle{}; }


void BgfxRenderDevice::destroyRenderTarget(ViewportHandle h) { if (m_bgfxInitialized && m_deviceCore) m_deviceCore->destroyRenderTarget(h); }


void BgfxRenderDevice::blitViewport(ViewportHandle handle, uint16_t targetView,
                                     float x, float y, float w, float h) {
    if (!canRender() || !m_draw) return;
    const auto* caps = bgfx::getCaps();
    // RTT storage follows the backend origin; uploaded image textures retain
    // their original top-to-bottom UV convention in the ordinary blit path.
    m_draw->blitTexture(targetView, m_deviceCore->getViewportTexture(handle),
        x, y, w, h, 255, caps && caps->originBottomLeft);
}

RenderTextureHandle BgfxRenderDevice::getViewportTexture(ViewportHandle h) { return canRender() ? toRenderHandle(m_deviceCore->getViewportTexture(h)) : RenderTextureHandle{}; }

RenderProgramHandle BgfxRenderDevice::getFallbackProgram() const { return m_shaders ? toRenderHandle(m_shaders->getFallbackProgram()) : RenderProgramHandle{}; }
RenderProgramHandle BgfxRenderDevice::getModulatedTextureProgram() const { return m_shaders ? toRenderHandle(m_shaders->getModulatedTextureProgram()) : RenderProgramHandle{}; }

RenderUniformHandle BgfxRenderDevice::getDefaultSampler() const { return m_shaders ? toRenderHandle(m_shaders->getDefaultSampler()) : RenderUniformHandle{}; }




void BgfxRenderDevice::blitTexture(uint16_t v, uint32_t tid, float x, float y, float w, float h, uint8_t o) { if (canRender() && m_draw) m_draw->blitTexture(v,tid,x,y,w,h,o); }
void BgfxRenderDevice::blitTexture(uint16_t v, bgfx::TextureHandle t, float x, float y, float w, float h, uint8_t o) { if (canRender() && m_draw) m_draw->blitTexture(v,t,x,y,w,h,o); }



// blitTexture(handle) old body removed



void BgfxRenderDevice::renderText(uint16_t viewId, const std::string& text,
                                     float x, float y,
                                     uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                                     float scale, bool bold, bool italic,
                                     bool strike) {
    // Cached path: static text (same text/view/position every frame) reuses
    // its glyph geometry with zero rebuild; the full key guarantees a cache
    // hit is only ever served for identical parameters (see matches()).
    // Scaled/bold/italic/struck text ({size}/{b}/{i}/{s} markup) bypasses
    // the cache (the geometry differs per scale/shear/strike) and goes
    // straight to the direct path.
    if (!canRender() || !m_textRenderer) return;
    if (scale != 1.0f || bold || italic || strike) {
        m_textRenderer->renderText(viewId, text, x, y, TextColor{r,g,b,a},
                                   scale, bold, italic, strike);
    } else {
        m_textRenderer->renderTextCached(viewId, text, x, y, TextColor{r,g,b,a});
    }
}

void BgfxRenderDevice::renderRuby(uint16_t viewId, const std::string& text,
                                     const std::string& ruby,
                                     float x, float y,
                                     uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (canRender() && m_textRenderer)
        m_textRenderer->renderRuby(viewId, text, ruby, x, y, TextColor{r,g,b,a});
}

void BgfxRenderDevice::setFont(int fontId) {
    if (canRender() && m_textRenderer)
        m_textRenderer->setFont(static_cast<FontId>(fontId));
}

bool BgfxRenderDevice::loadTTF(const char* path, float fontSize) {
    return canRender() && m_textRenderer && m_textRenderer->loadTTF(path, fontSize);
}

float BgfxRenderDevice::textLineHeight() const {
    return m_textRenderer ? m_textRenderer->lineHeight() : 16.0f;
}

void BgfxRenderDevice::setDebugName(uint16_t v, const std::string& n) { if (canRender()) m_deviceCore->setDebugName(v, n.c_str()); }

void BgfxRenderDevice::drawDebugOverlay(const std::string& title) {
    if (!canRender()) return;
    const bgfx::Caps* caps = bgfx::getCaps();
    if (!caps) return;

    bgfx::dbgTextClear();
    bgfx::dbgTextPrintf(0, 0, 0x0F, "%s", title.c_str());
    bgfx::dbgTextPrintf(0, 1, 0x0F, "Renderer: %s  %dx%d",
                        bgfx::getRendererName(caps->rendererType),
                        getBackbufferWidth(), getBackbufferHeight());
}

bool BgfxRenderDevice::requestScreenshot(const std::string& path) {
    if (path.empty() || !canRender()) return false;
    return static_cast<bool>(m_screenshots->request({}, m_deviceCore->presentWidth(),
                                                  m_deviceCore->presentHeight(), path).ticket);
}

ScreenshotResult BgfxRenderDevice::requestScreenshot(const ScreenshotOptions& options) {
    // No bgfx introspection at admission: readiness is renderer-owned.
    if (!canRender()) {
        ScreenshotResult result;
        result.status = ScreenshotStatus::Failed;
        result.error = "renderer unavailable";
        return result;
    }
    return m_screenshots->request(options, m_deviceCore->presentWidth(), m_deviceCore->presentHeight());
}
ScreenshotResult BgfxRenderDevice::takeScreenshot(const ScreenshotTicket& ticket) {
    return m_screenshots->take(ticket);
}
bool BgfxRenderDevice::cancelScreenshot(const ScreenshotTicket& ticket) {
    return m_screenshots->cancel(ticket);
}

bool BgfxRenderDevice::recoverDevice(void* nativeWindowHandle, int width, int height) {
    if (!m_bgfxInitialized || !m_deviceCore || m_stopping || m_recovering) return false;
    const bool hadPresentSize = m_deviceCore->hasExplicitPresentSize();
    const auto presentWidth = m_deviceCore->presentWidth();
    const auto presentHeight = m_deviceCore->presentHeight();
    m_recovering = true;
    m_recoveryFailed = true;
    m_frameFinalized = false;
    m_screenshots->close("renderer recovering");

    std::unique_ptr<IPreparedFontState> savedFont;
    if (m_textRenderer) savedFont=m_textRenderer->takeFontForDeviceRecovery();

    destroyPostFxResources();

    if (m_textRenderer) {
        m_textRenderer->shutdown();
        m_textRenderer.reset();
    }
    m_draw.reset();
    m_shaders.reset();
    m_bgfxInitialized = false;
    m_deviceCore->shutdown();

    if (!m_deviceCore->init(nativeWindowHandle, width, height)) {
        m_recovering = false;
        return false;
    }
    m_bgfxInitialized = true;

    if (hadPresentSize)
        m_deviceCore->setPresentSize(static_cast<uint16_t>(presentWidth), static_cast<uint16_t>(presentHeight));

    m_shaders = std::make_unique<BgfxShaderManager>();
    if (!m_shaders->setTestFault(m_shaderTestFault)) {
        m_recovering = false;
        return false;
    }
    m_shaderRenderingDisabled = false;
    m_shaders->initEmbeddedShaders();
    // t73 (b): same degrade gate as the primary init path.
    if (m_shaders->coreProgramsBroken()) {
        // t75: only CORE-program failures disable rendering. Non-core failures
        // (vfx/transition/stretch/affine/postfx) are counted + logged loudly by
        // the manager; their draw sites already guard + skip, so rendering must
        // continue (a single broken VFX shader must not black the screen).
        printf("[RENDER][ERROR] [BgfxRenderDevice] CORE shader programs broken; "
               "rendering disabled (BGFX_DEBUG_IFH) - frame loop continues.\n");
        bgfx::setDebug(BGFX_DEBUG_IFH);
        m_shaderRenderingDisabled = true;
    }
    m_drawState.shaders = m_shaders.get();
    m_drawState.device  = m_deviceCore.get();
    m_draw = std::make_unique<BgfxDraw>();
    m_draw->init(&m_drawState);
    m_textRenderer = std::make_unique<TextRenderer>();
    if (!m_textRenderer->init(this,false)
        || (savedFont && !m_textRenderer->applyFontState(std::move(savedFont)))) {
        m_textRenderer.reset();
        m_recovering = false;
        return false;
    }
    m_recovering = false;
    m_recoveryFailed = false;
    if (m_shaders->coreProgramsBroken() || m_deviceCore->deviceLost()
        || bgfx::getCaps()->rendererType == bgfx::RendererType::Noop) {
        m_recoveryFailed = true;
        return false;
    }
    m_screenshots->open();
    return true;
}

void BgfxRenderDevice::flagDeviceLost() {
    if (m_deviceCore) m_deviceCore->flagDeviceLost();
    else m_screenshots->close("device lost");
}

bool BgfxRenderDevice::consumeDeviceLost() { return m_deviceCore && m_deviceCore->consumeDeviceLost(); }

RenderRuntimeInfo BgfxRenderDevice::getRuntimeInfo() const {
    RenderRuntimeInfo info;
    info.backendName = getBackendName();
    info.width = getBackbufferWidth();
    info.height = getBackbufferHeight();
    info.viewCount = 3;
    info.shaderReady = canRender() && m_shaders && !m_shaders->coreProgramsBroken()
        && !m_shaderRenderingDisabled && bgfx::getCaps()->rendererType != bgfx::RendererType::Noop;
    return info;
}



// ===========================================================================
//  GPU Effect: Blend -- two-texture blend with selectable mode

// ===========================================================================
//  fillViewport -- render solid-color quad into a viewport RTT framebuffer
// ===========================================================================

void BgfxRenderDevice::fillViewport(ViewportHandle h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) { if (canRender() && m_draw) m_draw->fillViewport(h,r,g,b,a); }

// Accessibility color filter presets: shared pure table (ColorFilterMath.h).
bool BgfxRenderDevice::setColorFilter(ColorFilterPreset preset) {
    if (!m_deviceCore) return false;
    m_deviceCore->setColorFilterMatrix(colorFilterPresetMatrix(preset));
    return true;
}



// ===========================================================================


// ===========================================================================
//  submitFullscreenQuad  helper for GPU effects
// ===========================================================================

// x,y reserved for future 3D RTT offset rendering
// submitFullscreenQuad → BgfxDraw


void BgfxRenderDevice::submitBlend(uint16_t v, RenderTextureHandle base, RenderTextureHandle blend, int mode, float ba, float bla, float ga) { if (canRender() && m_draw) m_draw->submitBlend(v,toBgfx(base),toBgfx(blend),mode,ba,bla,ga); }


// ===========================================================================
//  GPU Effect: Transition — crossfade / rule / wipe between two textures
// ===========================================================================

// Spec [10.2.25]: @Beta 闂?Pre-bake rule images into a LUT texture atlas for batch
// transition rendering. Currently each transition passes its rule texture
// individually via texture slot 2. A pre-baked atlas would reduce draw calls.
void BgfxRenderDevice::submitTransition(uint16_t v, RenderTextureHandle from, RenderTextureHandle to, RenderTextureHandle rule, int method, float progress) { if (canRender() && m_draw) m_draw->submitTransition(v,toBgfx(from),toBgfx(to),toBgfx(rule),method,progress); }


// ===========================================================================
//  GPU Effect: VFX — fade / blur / quake post-processing
// ===========================================================================

void BgfxRenderDevice::submitVFX(uint16_t v, RenderTextureHandle src, int e, float fa, float fr, float fg, float fb, float br, float qx, float qy) { if (canRender() && m_draw) m_draw->submitVFX(v,toBgfx(src),e,fa,fr,fg,fb,br,qx,qy); }


// ===========================================================================
//  GPU Transform: Stretch Blit (filtered copy with src/dst rects)
// ===========================================================================

void BgfxRenderDevice::stretchBlt(uint16_t v, uint32_t d, float dx, float dy, float dw, float dh, uint32_t s, float sx, float sy, float sw, float sh, int f) { if (canRender() && m_draw) m_draw->stretchBlt(v,d,dx,dy,dw,dh,s,sx,sy,sw,sh,f); }


// ===========================================================================
//  GPU Transform: Affine Blit (2D affine matrix transform)
// ===========================================================================

void BgfxRenderDevice::affineBlt(uint16_t v, uint32_t d, float dx, float dy, float dw, float dh, uint32_t s, float sx, float sy, float sw, float sh, const float m[6]) { if (canRender() && m_draw) m_draw->affineBlt(v,d,dx,dy,dw,dh,s,sx,sy,sw,sh,m); }


// ===========================================================================
//  Post-processing chain (round 102): API skeleton -- pass logic filled by
//  the render subagent (vignette/LUT/blur/bloom full-screen passes using the
//  embedded-shader pool + scratch RTTs + VIEW_MAIN retarget to m_sceneRtt).
// ===========================================================================

bool BgfxRenderDevice::isPostFxSupported(PostFxKind kind) const {
    if (!canRender() || !m_shaders || m_shaderRenderingDisabled
        || !bgfx::isValid(m_shaders->getPostFxProgram(static_cast<int>(kind)))) return false;
    // Bloom's multi-pass pipeline also depends on the blur shader.
    return kind != PostFxKind::Bloom
        || bgfx::isValid(m_shaders->getPostFxProgram(static_cast<int>(PostFxKind::SoftBlur)));
}

bool BgfxRenderDevice::isPostFxActive() const {
    if (!canRender()) return false;
    return std::any_of(m_postFxStages.begin(), m_postFxStages.end(), [this](const PostFxStage& stage) {
        return stage.enabled && isPostFxSupported(stage.kind)
            && (stage.kind != PostFxKind::Lut3D || (bgfx::isValid(stage.lutTex) && stage.lutSize >= 2));
    });
}

BgfxRenderDevice::PostFxHandle BgfxRenderDevice::createPostFx(PostFxKind kind, const PostFxParams& params) {
    if (!isPostFxSupported(kind)) return 0;
    if (kind == PostFxKind::Lut3D && (!params.lutTexture.isValid() || params.lutSize < 2)) return 0;
    PostFxStage stage;
    stage.kind = kind;
    stage.params = params;
    stage.lutTex = toBgfx(params.lutTexture);
    stage.lutSize = params.lutSize;
    if (kind == PostFxKind::Lut3D) {
        printf("[RENDER][INFO] PostFx Lut3D create: tex=%u size=%u intensity=%.2f\n",
               stage.lutTex.idx, stage.lutSize, stage.params.strength);
    }
    m_postFxStages.push_back(stage);
    return static_cast<PostFxHandle>(m_postFxStages.size()); // stable handle = index+1
}

void BgfxRenderDevice::setPostFxParams(PostFxHandle handle, const PostFxParams& params) {
    if (handle == 0 || handle > m_postFxStages.size()) return;
    PostFxStage& st = m_postFxStages[handle - 1];
    st.params = params;
    // Lut3D (t214): refresh the borrowed texture/size (palette apply/clear).
    st.lutTex = toBgfx(params.lutTexture);
    st.lutSize = params.lutSize;
    if (st.kind == PostFxKind::Lut3D) {
        printf("[RENDER][INFO] PostFx Lut3D set: tex=%u size=%u intensity=%.2f\n",
               st.lutTex.idx, st.lutSize, st.params.strength);
    }
}

void BgfxRenderDevice::destroyPostFx(PostFxHandle handle) {
    // Stable-handle contract: disable the stage in place instead of erasing,
    // so every other live handle keeps pointing at the same effect (RD-2).
    // The chain skips disabled stages; clearPostFx() reclaims all slots.
    if (handle == 0 || handle > m_postFxStages.size()) return;
    m_postFxStages[handle - 1].enabled = false;
}

void BgfxRenderDevice::clearPostFx() {
    m_postFxStages.clear();
}

BgfxRenderDevice::PostFxRt BgfxRenderDevice::getScratchRt(int slot, int w, int h) {
    // Returns a size-matched RGBA8 scratch RTT, growing the pool on demand.
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (static_cast<int>(m_postFxRtPool.size()) <= slot) {
        m_postFxRtPool.resize(static_cast<size_t>(slot) + 1u);
    }
    PostFxRt& rt = m_postFxRtPool[static_cast<size_t>(slot)];
    if (bgfx::isValid(rt.fb) && rt.w == w && rt.h == h) return rt;
    if (bgfx::isValid(rt.fb)) { bgfx::destroy(rt.fb); rt.fb = BGFX_INVALID_HANDLE; }
    bgfx::TextureHandle tex = bgfx::createTexture2D(
        static_cast<uint16_t>(w), static_cast<uint16_t>(h), false, 1,
        bgfx::TextureFormat::RGBA8,
        BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    if (!bgfx::isValid(tex)) return rt;
    rt.fb = bgfx::createFrameBuffer(1, &tex, true); // fb owns/destroys tex
    rt.w = w; rt.h = h;
    return rt;
}

void BgfxRenderDevice::submitFullscreenQuad(uint16_t viewId, bgfx::ProgramHandle program,
                                            bgfx::TextureHandle tex, bgfx::UniformHandle sampler) {
    if (!bgfx::isValid(program)) return;
    if (!bgfx::isValid(tex)) return;
    struct FsVertex { float x, y, u, v; };
    bgfx::TransientVertexBuffer tvb;
    bgfx::VertexLayout layout;
    layout.begin()
        .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();
    if (bgfx::getAvailTransientVertexBuffer(4, layout) < 4) return;
    bgfx::allocTransientVertexBuffer(&tvb, 4, layout);
    auto* v = reinterpret_cast<FsVertex*>(tvb.data);
    // This helper samples an engine render target. Its texture origin differs
    // from CPU-uploaded images on bottom-left backends such as OpenGL.
    const bool bottomLeft = bgfx::getCaps()->originBottomLeft;
    const float topV = bottomLeft ? 1.0f : 0.0f, bottomV = 1.0f - topV;
    v[0] = { -1.0f,  1.0f,  0.0f, topV };
    v[1] = {  1.0f,  1.0f,  1.0f, topV };
    v[2] = {  1.0f, -1.0f,  1.0f, bottomV };
    v[3] = { -1.0f, -1.0f,  0.0f, bottomV };
    uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
    bgfx::TransientIndexBuffer tib;
    if (bgfx::getAvailTransientIndexBuffer(6) < 6) return;
    bgfx::allocTransientIndexBuffer(&tib, 6);
    bx::memCopy(tib.data, indices, sizeof(indices));
    uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                   | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA,
                                           BGFX_STATE_BLEND_INV_SRC_ALPHA);
    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setIndexBuffer(&tib);
    bgfx::setState(state);
    if (bgfx::isValid(tex) && bgfx::isValid(sampler))
        bgfx::setTexture(0, sampler, tex);
    bgfx::submit(viewId, program);
}

// Helper: submit a full-screen quad without binding a source texture
// (used for the composite view when texture slots are set explicitly).
static void submitFullscreenQuadNoTex(uint16_t viewId, bgfx::ProgramHandle program) {
    if (!bgfx::isValid(program)) return;
    struct FsVertex { float x, y, u, v; };
    bgfx::TransientVertexBuffer tvb;
    bgfx::VertexLayout layout;
    layout.begin()
        .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();
    if (bgfx::getAvailTransientVertexBuffer(4, layout) < 4) return;
    bgfx::allocTransientVertexBuffer(&tvb, 4, layout);
    auto* v = reinterpret_cast<FsVertex*>(tvb.data);
    const bool bottomLeft = bgfx::getCaps()->originBottomLeft;
    const float topV = bottomLeft ? 1.0f : 0.0f, bottomV = 1.0f - topV;
    v[0] = { -1.0f,  1.0f,  0.0f, topV };
    v[1] = {  1.0f,  1.0f,  1.0f, topV };
    v[2] = {  1.0f, -1.0f,  1.0f, bottomV };
    v[3] = { -1.0f, -1.0f,  0.0f, bottomV };
    uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
    bgfx::TransientIndexBuffer tib;
    if (bgfx::getAvailTransientIndexBuffer(6) < 6) return;
    bgfx::allocTransientIndexBuffer(&tib, 6);
    bx::memCopy(tib.data, indices, sizeof(indices));
    uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                   | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA,
                                           BGFX_STATE_BLEND_INV_SRC_ALPHA);
    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setIndexBuffer(&tib);
    bgfx::setState(state);
    bgfx::submit(viewId, program);
}

void BgfxRenderDevice::runPostFxChain() {
    if (!m_bgfxInitialized || !m_deviceCore || !m_chainRetargeted) return;
    if (!m_shaders || !bgfx::isValid(m_shaders->getPostFxParams())) return;
    {
        static bool s_chainRan = false;
        if (!s_chainRan) {
            printf("[RENDER][INFO] PostFx chain executing (stages=%zu)\n",
                   m_postFxStages.size());
            s_chainRan = true;
        }
    }

    const int W = m_deviceCore->getWidth();
    const int H = m_deviceCore->getHeight();
    if (W < 1 || H < 1) return;

    static const uint16_t kPostFxView = BgfxDeviceCore::VIEW_POSTFX;
    bgfx::setViewRect(kPostFxView, 0, 0, static_cast<uint16_t>(W), static_cast<uint16_t>(H));
    bgfx::setViewClear(kPostFxView, BGFX_CLEAR_NONE, 0x00000000, 1.0f, 0);

    // Only composite the scene actually selected at beginFrame. Recreating an
    // empty target here would discard the scene that has already been drawn.
    if (!bgfx::isValid(m_sceneRtt)) return;
    const bgfx::TextureHandle sceneTex = bgfx::getTexture(m_sceneRtt, 0);
    if (!bgfx::isValid(sceneTex)) return;

    bgfx::UniformHandle uParams = m_shaders->getPostFxParams();
    bgfx::UniformHandle uS0     = m_shaders->getDefaultSampler();
    bgfx::UniformHandle uS1     = m_shaders->getSampler1();

    // Scene is sampled; if the chain has only one stage we composite straight
    // to the backbuffer, otherwise we ping-pong through scratch RTTs.
    bgfx::TextureHandle resultTex = sceneTex;
    const size_t stageCount = m_postFxStages.size();
    const bool single = stageCount == 1;

    const auto executable = [this](const PostFxStage& stage) {
        return stage.enabled && isPostFxSupported(stage.kind)
            && (stage.kind != PostFxKind::Lut3D || (bgfx::isValid(stage.lutTex) && stage.lutSize >= 2));
    };
    // A disabled or invalid tail must not divert the preceding valid result.
    size_t lastEnabledIdx = 0;
    bool hasEnabled = false;
    for (size_t i = 0; i < stageCount; ++i) {
        if (executable(m_postFxStages[i])) { lastEnabledIdx = i; hasEnabled = true; }
    }
    if (!hasEnabled) {
        bgfx::setViewFrameBuffer(kPostFxView, BGFX_INVALID_HANDLE);
        bgfx::setViewRect(kPostFxView, 0, 0, static_cast<uint16_t>(m_deviceCore->presentWidth()),
            static_cast<uint16_t>(m_deviceCore->presentHeight()));
        submitFullscreenQuad(kPostFxView, m_shaders->getFallbackProgram(), sceneTex, uS0);
        return;
    }

    for (size_t i = 0; i < stageCount; ++i) {
        const PostFxStage& st = m_postFxStages[i];
        if (!executable(st)) continue;
        const bool last = (i == lastEnabledIdx);
        if (last) bgfx::setViewRect(kPostFxView, 0, 0, static_cast<uint16_t>(m_deviceCore->presentWidth()),
            static_cast<uint16_t>(m_deviceCore->presentHeight()));
        PostFxKind kind = st.kind;
        bgfx::ProgramHandle prog = m_shaders->getPostFxProgram(static_cast<int>(kind));
        if (!bgfx::isValid(prog)) prog = m_shaders->getFallbackProgram();
        if (!bgfx::isValid(prog)) continue;

        auto setParams = [&](float s, float r, float a, float lm,
                             float tintR, float tintG, float tintB,
                             float texelW, float texelH) {
            float params[16] = {
                s, r, a, lm,
                tintR, tintG, tintB, 1.0f,
                texelW, texelH, 0.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 0.0f
            };
            bgfx::setUniform(uParams, params, 4);
        };

        if (kind == PostFxKind::Lut3D) {
            // 3D LUT (t214): scene t0 + 2D-packed LUT t1, single pass.
            // Borrowed texture: guard each frame (palette.unload can destroy
            // it out from under the stage -- then the stage is skipped).
            if (!bgfx::isValid(st.lutTex) || st.lutSize < 2) continue;
            {
                static bool s_lut3dRan = false; // one-shot diagnostics (t214)
                if (!s_lut3dRan) {
                    printf("[RENDER][INFO] PostFx Lut3D stage executing (N=%u)\n",
                           st.lutSize);
                    s_lut3dRan = true;
                }
            }
            const int slot = static_cast<int>(i % 2);
            PostFxRt out{};
            if (last || single) {
                bgfx::setViewFrameBuffer(kPostFxView, BGFX_INVALID_HANDLE);
            } else {
                out = getScratchRt(slot, W, H);
                if (!bgfx::isValid(out.fb)) continue;
                bgfx::setViewFrameBuffer(kPostFxView, out.fb);
            }
            float params[16] = {
                st.params.strength, 0.0f, 0.0f, 0.0f,
                1.0f, 1.0f, 1.0f, 1.0f,
                1.0f / float(W), 1.0f / float(H), float(st.lutSize), 0.0f,
                0.0f, 0.0f, 0.0f, 0.0f
            };
            bgfx::setUniform(uParams, params, 4);
            bgfx::setTexture(0, uS0, resultTex);
            bgfx::setTexture(1, m_shaders->getLutSampler(), st.lutTex);
            submitFullscreenQuadNoTex(kPostFxView, prog);
            resultTex = (last || single)
                ? bgfx::TextureHandle{}
                : bgfx::getTexture(out.fb, 0);
        } else if (kind == PostFxKind::Bloom) {
            // Internal multi-pass: bright-extract (½ res) -> blur ¼ -> blur ¼ -> add.
            static const int slot = 4; // bloom scratch slots (0..3 used by single-pass chain)
            const int hw = std::max(W / 2, 1), hh = std::max(H / 2, 1);
            const int qw = std::max(W / 4, 1), qh = std::max(H / 4, 1);
            PostFxRt e0 = getScratchRt(slot,     hw, hh);
            PostFxRt e1 = getScratchRt(slot + 1, qw, qh);
            PostFxRt e2 = getScratchRt(slot + 2, qw, qh);
            if (!bgfx::isValid(e0.fb) || !bgfx::isValid(e1.fb) || !bgfx::isValid(e2.fb)) {
                continue; // cannot run bloom; skip stage (graceful)
            }
            // Pass 1: bright-pass extract + downsample -> e0 (bloom FS, bloom slot unbound).
            bgfx::setViewFrameBuffer(kPostFxView, e0.fb);
            setParams(st.params.strength, st.params.radius, st.params.amount, st.params.lutMix,
                      st.params.r, st.params.g, st.params.b,
                      1.0f / float(hw), 1.0f / float(hh));
            submitFullscreenQuad(kPostFxView, prog, resultTex, uS0);
            // Pass 2-3: soft blur e0 -> e1 -> e2 (uses SoftBlur FS at ¼ res).
            bgfx::ProgramHandle blurProg = m_shaders->getPostFxProgram((int)PostFxKind::SoftBlur);
            if (!bgfx::isValid(blurProg)) blurProg = m_shaders->getFallbackProgram();
            {
                bgfx::setViewFrameBuffer(kPostFxView, e1.fb);
                setParams(1.0f, st.params.radius, st.params.amount, 0.0f,
                          st.params.r, st.params.g, st.params.b,
                          1.0f / float(qw), 1.0f / float(qh));
                submitFullscreenQuad(kPostFxView, blurProg, bgfx::getTexture(e0.fb, 0), uS0);
            }
            {
                bgfx::setViewFrameBuffer(kPostFxView, e2.fb);
                setParams(1.0f, st.params.radius, st.params.amount, 0.0f,
                          st.params.r, st.params.g, st.params.b,
                          1.0f / float(qw), 1.0f / float(qh));
                submitFullscreenQuad(kPostFxView, blurProg, bgfx::getTexture(e1.fb, 0), uS0);
            }
            // Pass 4: composite scene + blurred bloom (additive) to backbuffer.
            bgfx::setViewFrameBuffer(kPostFxView, BGFX_INVALID_HANDLE);
            setParams(st.params.strength, st.params.radius, st.params.amount, st.params.lutMix,
                      st.params.r, st.params.g, st.params.b,
                      1.0f / float(W), 1.0f / float(H));
            // bloom FS: t0 = scene, t1 = bloom texture (or scene if absent).
            bgfx::TextureHandle bloomTex = bgfx::getTexture(e2.fb, 0);
            bgfx::setTexture(0, uS0, resultTex);
            bgfx::setTexture(1, uS1, bgfx::isValid(bloomTex) ? bloomTex : resultTex);
            submitFullscreenQuadNoTex(kPostFxView, prog);
            resultTex = {}; // backbuffer output; nothing more to read
            // Any subsequent stage has nowhere to read from a real chain;
            // bloom is typically last. Keep going (next reads invalid -> guard).
        } else {
            // Single-stage full-screen pass.
            if (last || single) {
                bgfx::setViewFrameBuffer(kPostFxView, BGFX_INVALID_HANDLE);
                setParams(st.params.strength, st.params.radius, st.params.amount, st.params.lutMix,
                          st.params.r, st.params.g, st.params.b,
                          1.0f / float(W), 1.0f / float(H));
                submitFullscreenQuad(kPostFxView, prog, resultTex, uS0);
                resultTex = {}; // wrote to backbuffer
            } else {
                // Intermediate stage: ping-pong between scratch slots 0 and 1
                // so the stage never reads the same RTT it writes (avoids a
                // framebuffer feedback loop). The next stage consumes resultTex.
                const int slot = static_cast<int>(i % 2);
                PostFxRt out = getScratchRt(slot, W, H);
                if (!bgfx::isValid(out.fb)) continue;
                bgfx::setViewFrameBuffer(kPostFxView, out.fb);
                setParams(st.params.strength, st.params.radius, st.params.amount, st.params.lutMix,
                          st.params.r, st.params.g, st.params.b,
                          1.0f / float(W), 1.0f / float(H));
                submitFullscreenQuad(kPostFxView, prog, resultTex, uS0);
                resultTex = bgfx::getTexture(out.fb, 0);
            }
        }
    }
}

void BgfxRenderDevice::destroyPostFxResources() {
    for (auto& rt : m_postFxRtPool) {
        if (bgfx::isValid(rt.fb)) bgfx::destroy(rt.fb);
    }
    m_postFxRtPool.clear();
    if (bgfx::isValid(m_sceneRtt)) {
        bgfx::destroy(m_sceneRtt);
        m_sceneRtt = BGFX_INVALID_HANDLE;
    }
    m_sceneRttW = m_sceneRttH = 0;
    m_chainRetargeted = false;
}
} // namespace Caesura
