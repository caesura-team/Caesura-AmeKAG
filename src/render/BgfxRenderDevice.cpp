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
#include <atomic>
#include <limits>



namespace Caesura {

namespace {

// Context identity is separate from capture admission and survives shutdown in
// the instance snapshot. Zero permanently closes this process-wide allocator
// after UINT64_MAX has been assigned; an earlier context identity is never reused.
std::atomic<uint64_t> nextContextGeneration{1};
uint64_t allocateContextGeneration() {
    auto current = nextContextGeneration.load(std::memory_order_relaxed);
    while (current != 0) {
        const auto next = current == std::numeric_limits<uint64_t>::max() ? 0 : current + 1;
        if (nextContextGeneration.compare_exchange_weak(current, next, std::memory_order_relaxed))
            return current;
    }
    return 0;
}

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
    clearSceneSnapshots();
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
    m_contextGeneration = allocateContextGeneration();
    const bool screenshotsBound = m_deviceCore->bindScreenshotContext(m_contextGeneration);
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
    if (screenshotsBound && !m_shaders->coreProgramsBroken() && bgfx::getCaps()->rendererType != bgfx::RendererType::Noop
        && !m_deviceCore->deviceLost()) m_screenshots->open();
    return true;
}

void BgfxRenderDevice::beginShutdown() {
    clearSceneSnapshots();
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

bool BgfxRenderDevice::prepareSceneTargets() {
    const int w = m_deviceCore->getWidth(), h = m_deviceCore->getHeight();
    if (w < 1 || h < 1) return false;
    if (bgfx::isValid(m_sceneRtt) && bgfx::isValid(m_finalSceneRtt)
        && m_sceneRttW == w && m_sceneRttH == h) return true;
    destroyPostFxResources();
    auto makeTarget = [w, h]() -> bgfx::FrameBufferHandle {
        auto tex = bgfx::createTexture2D(uint16_t(w), uint16_t(h), false, 1,
            bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        if (!bgfx::isValid(tex)) return BGFX_INVALID_HANDLE;
        auto fb = bgfx::createFrameBuffer(1, &tex, true);
        if (!bgfx::isValid(fb)) bgfx::destroy(tex);
        return fb;
    };
    m_sceneRtt = makeTarget();
    m_finalSceneRtt = makeTarget();
    m_sceneRttW = w; m_sceneRttH = h;
    return bgfx::isValid(m_sceneRtt) && bgfx::isValid(m_finalSceneRtt);
}

void BgfxRenderDevice::beginFrame() {
    if (!canRender()) return;
    m_frameFinalized = false;
    m_deviceCore->beginFrame();
    // Keep the previous postprocessed scene until snapshot views have copied it.
    // bgfx orders these views before MAIN, irrespective of CPU submission order.
    m_chainRetargeted = prepareSceneTargets();
    bgfx::setViewFrameBuffer(BgfxDeviceCore::VIEW_MAIN,
        m_chainRetargeted ? m_sceneRtt : bgfx::FrameBufferHandle{bgfx::kInvalidHandle});
    if (m_chainRetargeted)
        bgfx::setViewRect(BgfxDeviceCore::VIEW_MAIN, 0, 0, uint16_t(m_sceneRttW), uint16_t(m_sceneRttH));
    // bgfx executes a view clear only when the view has a submission. Empty
    // scenes must clear too before finalScene can publish a new source frame.
    bgfx::touch(BgfxDeviceCore::VIEW_MAIN);
}


void BgfxRenderDevice::endFrame() {
    commit_frame();
    advanceFrame();
}


void BgfxRenderDevice::commit_frame() {
    if (!canRender() || m_frameFinalized) return;
    // The stage list can be cleared after beginFrame selected the scene RTT.
    // Such a frame still needs an identity composite to the backbuffer.
    m_sceneSubmitted = m_chainRetargeted && runPostFxChain();
    if (!m_sceneSubmitted) m_sceneReady = false;
    if (m_transitionDraw.pending && m_draw) {
        m_deviceCore->configurePresentationView(BgfxDeviceCore::VIEW_TRANSITION);
        m_draw->submitTransition(BgfxDeviceCore::VIEW_TRANSITION, toBgfx(m_transitionDraw.from),
            toBgfx(m_transitionDraw.to), toBgfx(m_transitionDraw.rule),
            m_transitionDraw.method, m_transitionDraw.progress);
        m_transitionDraw.pending = false;
    }
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
            // Publish debt before the void native call: an immediate callback
            // must find its slot, and a silently declined request stays counted.
            if (!m_screenshots->registerReadback(submission, m_contextGeneration)) {
                m_screenshots->failUnissuedSubmission(submission, "screenshot readback unavailable or capacity exceeded");
                continue;
            }
            bgfx::requestScreenShot(BGFX_INVALID_HANDLE, submission.callbackName.c_str());
        }
    }
    m_deviceCore->advanceFrame();
    if (m_sceneSubmitted) { ++m_sceneFrameId; m_sceneReady = true; }
    m_sceneSubmitted = false;
    std::erase_if(m_sceneSnapshots, [this](const SnapshotLease& lease) {
        if (!lease.releasePending) return false;
        m_deviceCore->destroyRenderTarget(lease.handle);
        return true;
    });
    m_snapshotCopiesThisFrame = 0;
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


SceneSnapshot BgfxRenderDevice::captureSceneSnapshot() {
    if (!canRender() || !m_sceneReady || !m_shaders || !bgfx::isValid(m_finalSceneRtt)
        || bgfx::getCaps()->rendererType == bgfx::RendererType::Noop
        || !bgfx::isValid(m_shaders->getTransitionProgram())
        || std::count_if(m_sceneSnapshots.begin(), m_sceneSnapshots.end(),
            [](const SnapshotLease& lease) { return !lease.releasePending; }) >= 2
        || m_snapshotCopiesThisFrame >= 2) return {};
    auto handle = m_deviceCore->createRenderTarget(m_sceneRttW, m_sceneRttH);
    if (!handle) return {};
    const auto view = uint16_t(BgfxDeviceCore::VIEW_SNAPSHOT_A + m_snapshotCopiesThisFrame);
    bgfx::setViewFrameBuffer(view, m_deviceCore->getRttFb(handle));
    bgfx::setViewRect(view, 0, 0, uint16_t(m_sceneRttW), uint16_t(m_sceneRttH));
    bgfx::setViewClear(view, BGFX_CLEAR_NONE);
    // Store a texture with the same UV convention as uploaded images. On GL
    // the unflipped RTT copy cancels destination storage's bottom-left origin.
    if (!submitFullscreenQuad(view, m_shaders->getFallbackProgram(),
            bgfx::getTexture(m_finalSceneRtt), m_shaders->getDefaultSampler(), false)) {
        m_deviceCore->destroyRenderTarget(handle);
        return {};
    }
    ++m_snapshotCopiesThisFrame;
    m_sceneSnapshots.push_back({handle, false});
    return {handle, m_sceneFrameId};
}

void BgfxRenderDevice::cancelTransition() { m_transitionDraw = {}; }

void BgfxRenderDevice::clearSceneSnapshots() {
    cancelTransition();
    if (m_bgfxInitialized && m_deviceCore)
        for (const auto& lease : m_sceneSnapshots) m_deviceCore->destroyRenderTarget(lease.handle);
    m_sceneSnapshots.clear();
    m_snapshotCopiesThisFrame = 0;
    m_sceneReady = m_sceneSubmitted = false;
}

void BgfxRenderDevice::destroyRenderTarget(ViewportHandle h) {
    for (auto& lease : m_sceneSnapshots) {
        if (lease.handle.id == h.id) {
            // The final transition submission still borrows it until advanceFrame.
            lease.releasePending = true;
            return;
        }
    }
    if (m_bgfxInitialized && m_deviceCore) m_deviceCore->destroyRenderTarget(h);
}


void BgfxRenderDevice::blitViewport(ViewportHandle handle, uint16_t targetView,
                                     float x, float y, float w, float h) {
    if (!canRender() || !m_draw) return;
    const auto* caps = bgfx::getCaps();
    // RTT storage follows the backend origin; uploaded image textures retain
    // their original top-to-bottom UV convention in the ordinary blit path.
    m_draw->blitTexture(targetView, m_deviceCore->getViewportTexture(handle),
        x, y, w, h, 255, caps && caps->originBottomLeft);
}

RenderTextureHandle BgfxRenderDevice::getViewportTexture(ViewportHandle h) {
    for (const auto& lease : m_sceneSnapshots)
        if (lease.handle.id == h.id && lease.releasePending) return {};
    return canRender() ? toRenderHandle(m_deviceCore->getViewportTexture(h)) : RenderTextureHandle{};
}

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
    m_contextGeneration = allocateContextGeneration();
    const bool screenshotsBound = m_deviceCore->bindScreenshotContext(m_contextGeneration);

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
    if (screenshotsBound) m_screenshots->open();
    return true;
}

void BgfxRenderDevice::flagDeviceLost() {
    clearSceneSnapshots();
    if (m_deviceCore) m_deviceCore->flagDeviceLost();
    else m_screenshots->close("device lost");
}

bool BgfxRenderDevice::consumeDeviceLost() { return m_deviceCore && m_deviceCore->consumeDeviceLost(); }

RenderSnapshot BgfxRenderDevice::getSnapshot() const {
    RenderSnapshot snapshot;
    snapshot.supported = true;
    snapshot.backendName = getBackendName();
    snapshot.contextInitialized = m_bgfxInitialized && m_deviceCore;
    snapshot.renderingAvailable = getRuntimeInfo().shaderReady;
    snapshot.contextGeneration = m_contextGeneration;
    snapshot.captureSubmissionFrame = m_frameId;
    if (snapshot.contextInitialized) {
        const auto* caps = bgfx::getCaps();
        if (caps && caps->rendererType == bgfx::RendererType::Noop)
            snapshot.backendKind = RenderBackendKind::Noop;
        else if (caps && caps->rendererType > bgfx::RendererType::Noop &&
                 caps->rendererType < bgfx::RendererType::Count)
            snapshot.backendKind = RenderBackendKind::GraphicsApi;

        // getStats owns a reused vendor buffer. Copy only these allocator
        // values immediately, before another bgfx call or queue observation.
        // Counts include deferred recycling; they do not establish GPU completion.
        if (m_contextGeneration != 0) {
            if (const auto* stats = bgfx::getStats()) {
                snapshot.resources.dynamicIndexBuffers = stats->numDynamicIndexBuffers;
                snapshot.resources.dynamicVertexBuffers = stats->numDynamicVertexBuffers;
                snapshot.resources.frameBuffers = stats->numFrameBuffers;
                snapshot.resources.indexBuffers = stats->numIndexBuffers;
                snapshot.resources.occlusionQueries = stats->numOcclusionQueries;
                snapshot.resources.programs = stats->numPrograms;
                snapshot.resources.shaders = stats->numShaders;
                snapshot.resources.textures = stats->numTextures;
                snapshot.resources.uniforms = stats->numUniforms;
                snapshot.resources.vertexBuffers = stats->numVertexBuffers;
                snapshot.resources.vertexLayouts = stats->numVertexLayouts;
                snapshot.resourceCountsAvailable = true;
            }
        }
    }
    if (m_screenshots) {
        snapshot.screenshots = m_screenshots->getSnapshot();
        const auto readbacks = m_screenshots->getReadbackSnapshot();
        // Queue and ledger reads are sequential observations. Preserve raw debt
        // even if binding failed; unavailable support must never imply idle.
        snapshot.screenshotReadbacksOutstanding = readbacks.outstanding;
        const bool contextMatches = m_contextGeneration != 0
            && readbacks.contextGeneration == m_contextGeneration
            && readbacks.contextActive == snapshot.contextInitialized;
        snapshot.screenshotReadbackTrackingSupported = readbacks.supported && contextMatches;
        snapshot.screenshotOwnershipComplete = readbacks.ownershipComplete
            && snapshot.screenshotReadbackTrackingSupported;
    }
    return snapshot;
}

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
void BgfxRenderDevice::submitTransition(uint16_t, RenderTextureHandle from, RenderTextureHandle to, RenderTextureHandle rule, int method, float progress) {
    if (canRender() && from.isValid() && to.isValid())
        m_transitionDraw = {from, to, rule, method, progress, true};
}


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
    unsigned passes = kind == PostFxKind::Bloom ? 4 : 1;
    for (const auto& existing : m_postFxStages)
        if (existing.enabled) passes += existing.kind == PostFxKind::Bloom ? 4 : 1;
    if (passes > BgfxDeviceCore::VIEW_POSTFX_LAST - BgfxDeviceCore::VIEW_POSTFX) return 0;
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

bool BgfxRenderDevice::submitFullscreenQuad(uint16_t viewId, bgfx::ProgramHandle program,
                                            bgfx::TextureHandle tex, bgfx::UniformHandle sampler, bool renderTargetOrigin,
                                            float offsetX, float offsetY) {
    if (!bgfx::isValid(program)) return false;
    if (!bgfx::isValid(tex)) return false;
    struct FsVertex { float x, y, u, v; };
    bgfx::TransientVertexBuffer tvb;
    bgfx::VertexLayout layout;
    layout.begin()
        .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();
    if (bgfx::getAvailTransientVertexBuffer(4, layout) < 4) return false;
    bgfx::allocTransientVertexBuffer(&tvb, 4, layout);
    auto* v = reinterpret_cast<FsVertex*>(tvb.data);
    // This helper samples an engine render target. Its texture origin differs
    // from CPU-uploaded images on bottom-left backends such as OpenGL.
    const bool bottomLeft = renderTargetOrigin && bgfx::getCaps()->originBottomLeft;
    const float topV = bottomLeft ? 1.0f : 0.0f, bottomV = 1.0f - topV;
    v[0] = { -1.0f + offsetX,  1.0f + offsetY,  0.0f, topV };
    v[1] = {  1.0f + offsetX,  1.0f + offsetY,  1.0f, topV };
    v[2] = {  1.0f + offsetX, -1.0f + offsetY,  1.0f, bottomV };
    v[3] = { -1.0f + offsetX, -1.0f + offsetY,  0.0f, bottomV };
    uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
    bgfx::TransientIndexBuffer tib;
    if (bgfx::getAvailTransientIndexBuffer(6) < 6) return false;
    bgfx::allocTransientIndexBuffer(&tib, 6);
    bx::memCopy(tib.data, indices, sizeof(indices));
    uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A;
    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setIndexBuffer(&tib);
    bgfx::setState(state);
    if (bgfx::isValid(tex) && bgfx::isValid(sampler))
        bgfx::setTexture(0, sampler, tex);
    bgfx::submit(viewId, program);
    return true;
}

// Helper: submit a full-screen quad without binding a source texture
// (used for the composite view when texture slots are set explicitly).
static bool submitFullscreenQuadNoTex(uint16_t viewId, bgfx::ProgramHandle program) {
    if (!bgfx::isValid(program)) return false;
    struct FsVertex { float x, y, u, v; };
    bgfx::TransientVertexBuffer tvb;
    bgfx::VertexLayout layout;
    layout.begin()
        .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();
    if (bgfx::getAvailTransientVertexBuffer(4, layout) < 4) return false;
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
    if (bgfx::getAvailTransientIndexBuffer(6) < 6) return false;
    bgfx::allocTransientIndexBuffer(&tib, 6);
    bx::memCopy(tib.data, indices, sizeof(indices));
    uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A;
    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setIndexBuffer(&tib);
    bgfx::setState(state);
    bgfx::submit(viewId, program);
    return true;
}

bool BgfxRenderDevice::runPostFxChain() {
    if (!canRender() || !m_chainRetargeted || !m_shaders
        || !bgfx::isValid(m_sceneRtt) || !bgfx::isValid(m_finalSceneRtt)) return false;
    const int w = m_sceneRttW, h = m_sceneRttH;
    auto result = bgfx::getTexture(m_sceneRtt);
    const auto sampler = m_shaders->getDefaultSampler();
    const auto uniform = m_shaders->getPostFxParams();
    uint16_t view = BgfxDeviceCore::VIEW_POSTFX;
    int outputSlot = 0;
    if (isPostFxActive()) {
        static bool logged = false;
        if (!logged) {
            printf("[RENDER][INFO] PostFx chain executing (stages=%zu)\n", m_postFxStages.size());
            logged = true;
        }
    }
    auto target = [&](bgfx::FrameBufferHandle fb, int tw, int th) {
        bgfx::setViewFrameBuffer(view, fb);
        bgfx::setViewRect(view, 0, 0, uint16_t(tw), uint16_t(th));
        bgfx::setViewClear(view, BGFX_CLEAR_NONE);
    };
    auto params = [&](const PostFxStage& st, float strength, int tw, int th) {
        const float values[16] = {strength, st.params.radius, st.params.amount, st.params.lutMix,
            st.params.r, st.params.g, st.params.b, 1,
            1.0f / float(tw), 1.0f / float(th), float(st.lutSize), 0, 0, 0, 0, 0};
        bgfx::setUniform(uniform, values, 4);
    };
    for (const auto& st : m_postFxStages) {
        if (!st.enabled || !isPostFxSupported(st.kind)
            || (st.kind == PostFxKind::Lut3D && (!bgfx::isValid(st.lutTex) || st.lutSize < 2))) continue;
        if (!bgfx::isValid(uniform)) return false;
        const unsigned passes = st.kind == PostFxKind::Bloom ? 4 : 1;
        // One final identity pass is always reserved. Every pass gets its own
        // view: framebuffer/rect are view state, not per-submit state in bgfx.
        if (view + passes > BgfxDeviceCore::VIEW_POSTFX_LAST) return false;
        auto prog = m_shaders->getPostFxProgram(static_cast<int>(st.kind));
        if (!bgfx::isValid(prog)) return false;
        auto out = getScratchRt(outputSlot, w, h);
        if (!bgfx::isValid(out.fb)) return false;
        if (st.kind == PostFxKind::Bloom) {
            const int hw = std::max(w / 2, 1), hh = std::max(h / 2, 1);
            const int qw = std::max(w / 4, 1), qh = std::max(h / 4, 1);
            auto e0 = getScratchRt(4, hw, hh);
            auto e1 = getScratchRt(5, qw, qh);
            auto e2 = getScratchRt(6, qw, qh);
            auto blur = m_shaders->getPostFxProgram(static_cast<int>(PostFxKind::SoftBlur));
            if (!bgfx::isValid(e0.fb) || !bgfx::isValid(e1.fb) || !bgfx::isValid(e2.fb)
                || !bgfx::isValid(blur)) return false;
            target(e0.fb, hw, hh); params(st, st.params.strength, hw, hh);
            bgfx::setTexture(1, m_shaders->getSampler1(), result);
            if (!submitFullscreenQuad(view++, prog, result, sampler)) return false;
            target(e1.fb, qw, qh); params(st, 1, qw, qh);
            if (!submitFullscreenQuad(view++, blur, bgfx::getTexture(e0.fb), sampler)) return false;
            target(e2.fb, qw, qh); params(st, 1, qw, qh);
            if (!submitFullscreenQuad(view++, blur, bgfx::getTexture(e1.fb), sampler)) return false;
            target(out.fb, w, h); params(st, st.params.strength, w, h);
            bgfx::setTexture(0, sampler, result);
            bgfx::setTexture(1, m_shaders->getSampler1(), bgfx::getTexture(e2.fb));
            if (!submitFullscreenQuadNoTex(view++, prog)) return false;
        } else {
            target(out.fb, w, h); params(st, st.params.strength, w, h);
            if (st.kind == PostFxKind::Lut3D) {
                static bool loggedLut = false;
                if (!loggedLut) {
                    printf("[RENDER][INFO] PostFx Lut3D stage executing (N=%u)\n", st.lutSize);
                    loggedLut = true;
                }
                bgfx::setTexture(0, sampler, result);
                bgfx::setTexture(1, m_shaders->getLutSampler(), st.lutTex);
                if (!submitFullscreenQuadNoTex(view++, prog)) return false;
            } else if (!submitFullscreenQuad(view++, prog, result, sampler)) return false;
        }
        result = bgfx::getTexture(out.fb);
        outputSlot = 1 - outputSlot;
    }
    target(m_finalSceneRtt, w, h);
    if (!submitFullscreenQuad(view, m_shaders->getFallbackProgram(), result, sampler)) return false;
    m_deviceCore->clearPresentationSurface();
    m_deviceCore->configurePresentationView(BgfxDeviceCore::VIEW_PRESENT);
    float offsetX = 0, offsetY = 0;
    m_deviceCore->presentationOffsetNdc(offsetX, offsetY);
    return submitFullscreenQuad(BgfxDeviceCore::VIEW_PRESENT, m_shaders->getFallbackProgram(),
        bgfx::getTexture(m_finalSceneRtt), sampler, true, offsetX, offsetY);
}

void BgfxRenderDevice::destroyPostFxResources() {
    clearSceneSnapshots();
    if (bgfx::isValid(m_finalSceneRtt)) bgfx::destroy(m_finalSceneRtt);
    m_finalSceneRtt = BGFX_INVALID_HANDLE;
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
