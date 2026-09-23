#include "BgfxDebugCallback.h"

namespace {
thread_local BgfxDebugCallback::ScopedScreenshotCheckpoint::Hook screenshotCheckpoint = nullptr;
thread_local void* screenshotCheckpointContext = nullptr;

void checkpoint(BgfxDebugCallback::ScreenshotCheckpoint stage) {
    if (screenshotCheckpoint) screenshotCheckpoint(stage, screenshotCheckpointContext);
}

struct ReadbackCompletion {
    std::shared_ptr<Caesura::ScreenshotQueue> queue;
    Caesura::ScreenshotQueue::ReadbackToken token;
    ~ReadbackCompletion() noexcept {
        if (token.contextGeneration != 0) queue->finishReadbackCallback(token);
    }
};
}

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
                                  const void* data, uint32_t size, bool flipY) noexcept {
    using Queue = Caesura::ScreenshotQueue;
    const auto queue = m_screenshots;
    const auto claim = queue->claimReadbackCallback(name, m_screenshotContext.load(std::memory_order_acquire));
    if (claim.disposition == Queue::CallbackDisposition::Rejected) return;
    // shared_ptr copying and the fixed token cannot throw between claim and
    // guard construction. The guard also spans diagnostics and both checkpoints.
    ReadbackCompletion completion{queue, claim.token};
    try {
        if (claim.disposition == Queue::CallbackDisposition::Claimed)
            checkpoint(ScreenshotCheckpoint::AfterClaim);
#if defined(CAESURA_RENDER_TEST_FAULTS)
        std::fprintf(stderr, "[capture input] name=%s native=%ux%u pitch=%u flipY=%d\n",
            name ? name : "<null>", width, height, pitch, int(flipY));
#endif
        using Format = Queue::PixelFormat;
        const auto pixelFormat = format == bgfx::TextureFormat::BGRA8 ? Format::BGRA8
                               : format == bgfx::TextureFormat::RGBA8 ? Format::RGBA8
                               : Format::Unsupported;
        queue->complete(name, width, height, pitch, pixelFormat, data, size, flipY);
        if (claim.disposition == Queue::CallbackDisposition::Claimed)
            checkpoint(ScreenshotCheckpoint::AfterComplete);
    } catch (...) {
        // Do not let an unexpected processing/test-hook exception cross bgfx's
        // callback boundary. complete() handles normal encode/write failures;
        // any still-Pending ticket remains observable until cancellation/timeout.
        std::fputs("[capture error] unexpected screenshot callback processing failure\n", stderr);
    }
}

bool BgfxDebugCallback::bindScreenshotContext(uint64_t contextGeneration) {
    if (m_screenshotContext.load(std::memory_order_acquire) != 0
        || !m_screenshots->beginReadbackContext(contextGeneration)) return false;
    m_screenshotContext.store(contextGeneration, std::memory_order_release);
    return true;
}

bool BgfxDebugCallback::screenshotContextShutdownComplete() {
    const auto context = m_screenshotContext.load(std::memory_order_acquire);
    if (context == 0 || !m_screenshots->retireReadbacksAfterContextShutdown(context)) return false;
    m_screenshotContext.store(0, std::memory_order_release);
    return true;
}

BgfxDebugCallback::ScopedScreenshotCheckpoint::ScopedScreenshotCheckpoint(Hook hook, void* context) noexcept
    : m_previousHook(screenshotCheckpoint), m_previousContext(screenshotCheckpointContext) {
    screenshotCheckpoint = hook;
    screenshotCheckpointContext = context;
}

BgfxDebugCallback::ScopedScreenshotCheckpoint::~ScopedScreenshotCheckpoint() noexcept {
    screenshotCheckpoint = m_previousHook;
    screenshotCheckpointContext = m_previousContext;
}
