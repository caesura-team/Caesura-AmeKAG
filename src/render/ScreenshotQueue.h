#pragma once

#include "api/IRenderDevice.h"
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
    size_t retainedCount() const;
    size_t retainedBytes() const;

private:
    struct Entry {
        ScreenshotResult result;
        std::string callbackName;
        std::string legacyPath;
        size_t reservedBytes = 0;
    };
    using Entries = std::unordered_map<uint64_t, Entry>;
    void finish(Entry& entry, ScreenshotStatus status, const char* error);
    mutable std::mutex m_mutex;
    Entries m_entries;
    uint64_t m_generation = 0;
    size_t m_reservedBytes = 0;
    bool m_ready = false;
};

} // namespace Caesura
