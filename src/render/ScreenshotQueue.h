#pragma once

#include "api/IRenderDevice.h"
#include <array>
#include <mutex>
#include <unordered_map>

namespace Caesura {

// Private renderer implementation, shared by the owner and its bgfx callback.
// All transitions (including PNG publication) are serialized by this queue.
class ScreenshotQueue {
public:
    enum class PixelFormat { RGBA8, BGRA8, Unsupported };
    struct Submission { ScreenshotTicket ticket; std::string callbackName; };
    static constexpr size_t MaxRequests = 8;
    static constexpr uint32_t MaxDimension = 8192;
    static constexpr uint64_t MaxPixels = 8ull * 1024 * 1024;
    static constexpr size_t MaxPngBytes = 36ull * 1024 * 1024;
    static constexpr size_t MaxRetainedBytes = 64ull * 1024 * 1024;
    static constexpr uint64_t MaxPendingFrames = 120;

    // Opens a distinct generation after successful initialization/recovery.
    void open();
    void close(const char* reason);
    bool ready() const;
    ScreenshotResult request(const ScreenshotOptions& options,
                             uint32_t nativeWidth, uint32_t nativeHeight,
                             const std::string& legacyPath = {});
    ScreenshotResult take(const ScreenshotTicket& ticket);
    bool cancel(const ScreenshotTicket& ticket);
    // bgfx allows one screenshot per framebuffer per frame. One returned readback
    // fans out to all newly submitted tickets, each with its own PNG dimensions.
    std::vector<Submission> submit(uint64_t frameId);
    void complete(const char* callbackName, uint32_t width, uint32_t height,
                  uint32_t pitch, PixelFormat format, const void* data,
                  uint32_t dataSize, bool flipY);
    // Read retained entry classification, budget and logical PNG bytes under
    // one lock. This does not observe independently outstanding bgfx callbacks.
    RenderScreenshotCounts getSnapshot() const;
    size_t retainedCount() const;
    size_t retainedBytes() const;

    // Readbacks have a context lifetime independent of retained tickets. These
    // internal transitions are used at actual native publication/callback/shutdown
    // edges; callers cannot set the observed count directly.
    static constexpr size_t MaxOutstandingReadbacks = 8;
    struct ReadbackToken {
        uint64_t contextGeneration = 0;
        uint64_t requestId = 0;
    };
    struct ReadbackObservation {
        bool supported = false;
        bool ownershipComplete = false;
        uint64_t outstanding = 0;
        uint64_t contextGeneration = 0; // Last successfully bound context.
        bool contextActive = false;
    };
    enum class CallbackDisposition { Rejected, Unbound, Claimed };
    struct CallbackClaim {
        CallbackDisposition disposition = CallbackDisposition::Rejected;
        ReadbackToken token;
    };
    // Fresh nonzero, increasing identity only, after exact old-context retirement.
    bool beginReadbackContext(uint64_t contextGeneration);
    // Reserve before the void native request. Fanout names consume one slot.
    bool registerReadback(const Submission& submission, uint64_t contextGeneration);
    void failUnissuedSubmission(const Submission& submission, const char* reason);
    CallbackClaim claimReadbackCallback(const char* exactName, uint64_t callbackContextGeneration);
    void finishReadbackCallback(ReadbackToken token) noexcept;
    // Caller must have completed actual context shutdown; refuses active callbacks.
    bool retireReadbacksAfterContextShutdown(uint64_t contextGeneration);
    ReadbackObservation getReadbackSnapshot() const;

private:
    struct Entry {
        ScreenshotResult result;
        std::string callbackName;
        std::string legacyPath;
        size_t reservedBytes = 0;
    };
    using Entries = std::unordered_map<uint64_t, Entry>;
    enum class ReadbackState { Empty, Published, Executing };
    struct ReadbackSlot {
        ReadbackToken token;
        // "caesura-capture-" and two decimal uint64 values fit, including NUL.
        std::array<char, 64> callbackName{};
        ReadbackState state = ReadbackState::Empty;
    };
    void finish(Entry& entry, ScreenshotStatus status, const char* error);
    mutable std::mutex m_mutex;
    Entries m_entries;
    std::array<ReadbackSlot, MaxOutstandingReadbacks> m_readbacks{};
    uint64_t m_readbackContext = 0;
    bool m_readbackContextActive = false;
    uint64_t m_generation = 0;
    size_t m_reservedBytes = 0;
    bool m_ready = false;
};

} // namespace Caesura
