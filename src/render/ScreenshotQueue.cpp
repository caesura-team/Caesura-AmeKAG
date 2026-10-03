#include "ScreenshotQueue.h"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <limits>
#include <stb/stb_image_write.h>

namespace Caesura {
namespace {
// Identity allocators only: image data and lifecycle flags are instance-owned.
std::atomic<uint64_t> nextRequest{1};
std::atomic<uint64_t> nextGeneration{1};
uint64_t allocateIdentity(std::atomic<uint64_t>& counter) {
    auto value = counter.load(std::memory_order_relaxed);
    while (value != std::numeric_limits<uint64_t>::max()) {
        if (counter.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) return value;
    }
    return 0; // Exhaustion never wraps into a ticket from an earlier lifetime.
}

bool geometry(uint32_t w, uint32_t h) {
    return w && h && w <= ScreenshotQueue::MaxDimension && h <= ScreenshotQueue::MaxDimension
        && uint64_t(w) * h <= ScreenshotQueue::MaxPixels;
}
size_t pngSize(uint32_t w, uint32_t h) {
    const size_t filtered = (size_t(w) * 4 + 1) * h;
    // Vendored stb falls back to 32767-byte stored blocks when compression is
    // worse. Reserve its worst-case PNG size up front, then enforce the limit.
    return 63 + filtered + ((filtered + 32766) / 32767) * 5;
}
struct PngSink {
    std::vector<uint8_t> bytes;
    size_t limit = 0;
    bool failed = false;
};
void appendPng(void* context, void* data, int length) noexcept {
    auto& sink = *static_cast<PngSink*>(context);
    if (sink.failed || length <= 0 || !data || static_cast<size_t>(length) > sink.limit - sink.bytes.size()) {
        sink.failed = true;
        return;
    }
    try {
        const auto* bytes = static_cast<const uint8_t*>(data);
        sink.bytes.insert(sink.bytes.end(), bytes, bytes + length);
    } catch (...) { sink.failed = true; }
}

// Prepare oriented, tightly-packed RGBA locally, then use the existing PNG
// encoder. No global stb flip or compression settings are changed. Nearest-
// neighbor resize samples pixel centers, preserving diagnostic colors.
std::vector<uint8_t> encode(uint32_t sw, uint32_t sh, uint32_t pitch,
                            ScreenshotQueue::PixelFormat format, const uint8_t* src,
                            bool flip, uint32_t w, uint32_t h) {
    std::vector<uint8_t> rgba(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        uint32_t sy = static_cast<uint32_t>((uint64_t(y) * 2 + 1) * sh / (uint64_t(h) * 2));
        if (flip) sy = sh - 1 - sy;
        auto* dst = rgba.data() + size_t(w) * 4 * y;
        for (uint32_t x = 0; x < w; ++x) {
            const auto sx = static_cast<uint32_t>((uint64_t(x) * 2 + 1) * sw / (uint64_t(w) * 2));
            const auto* pixel = src + size_t(sy) * pitch + size_t(sx) * 4;
            *dst++ = pixel[format == ScreenshotQueue::PixelFormat::BGRA8 ? 2 : 0];
            *dst++ = pixel[1];
            *dst++ = pixel[format == ScreenshotQueue::PixelFormat::BGRA8 ? 0 : 2];
            *dst++ = pixel[3];
        }
    }
    PngSink sink;
    sink.limit = std::min(pngSize(w, h), ScreenshotQueue::MaxPngBytes);
    const int ok = stbi_write_png_to_func(appendPng, &sink, static_cast<int>(w), static_cast<int>(h),
                                         4, rgba.data(), static_cast<int>(w * 4));
    if (!ok || sink.failed || sink.bytes.empty()) throw std::runtime_error("PNG encoding failed");
    return std::move(sink.bytes);
}
ScreenshotResult rejected(const char* error) {
    ScreenshotResult result;
    result.status = ScreenshotStatus::Failed;
    result.error = error;
    return result;
}
bool validSubmissionName(const ScreenshotQueue::Submission& submission) {
    if (!submission.ticket) return false;
    std::array<char, 64> expected{};
    constexpr char prefix[] = "caesura-capture-";
    auto* next = std::copy(prefix, prefix + sizeof(prefix) - 1, expected.data());
    auto* end = expected.data() + expected.size() - 1;
    const auto generation = std::to_chars(next, end, submission.ticket.generation);
    if (generation.ec != std::errc{} || generation.ptr == end) return false;
    *generation.ptr = '-';
    const auto request = std::to_chars(generation.ptr + 1, end, submission.ticket.requestId);
    if (request.ec != std::errc{}) return false;
    const auto length = static_cast<size_t>(request.ptr - expected.data());
    return submission.callbackName.size() == length
        && std::memcmp(submission.callbackName.data(), expected.data(), length) == 0;
}
}

void ScreenshotQueue::finish(Entry& entry, ScreenshotStatus status, const char* error) {
    entry.result.status = status;
    // Capacity is reserved before admission, so callback failure publication does
    // not allocate even if the encoder failed due to memory pressure.
    entry.result.error.assign(error, std::min<size_t>(std::strlen(error), 95));
    entry.result.png.clear();
    m_reservedBytes -= entry.reservedBytes;
    entry.reservedBytes = 0;
}
void ScreenshotQueue::open() {
    close("renderer generation changed");
    std::lock_guard<std::mutex> lock(m_mutex);
    m_generation = allocateIdentity(nextGeneration);
    m_ready = m_generation != 0;
}
void ScreenshotQueue::close(const char* reason) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_ready = false;
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (it->second.result.status == ScreenshotStatus::Pending)
            finish(it->second, ScreenshotStatus::Cancelled, reason);
        if (!it->second.legacyPath.empty()) {
            m_reservedBytes -= it->second.reservedBytes;
            it = m_entries.erase(it);
        } else ++it;
    }
}
bool ScreenshotQueue::ready() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_ready;
}
ScreenshotResult ScreenshotQueue::request(const ScreenshotOptions& options,
                                           uint32_t nativeWidth, uint32_t nativeHeight,
                                           const std::string& legacyPath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ready) return rejected("renderer unavailable");
    if ((options.width == 0) != (options.height == 0)) return rejected("invalid screenshot dimensions");
    const auto w = options.width ? options.width : nativeWidth;
    const auto h = options.height ? options.height : nativeHeight;
    if (!geometry(w, h)) return rejected("screenshot dimensions exceed limit");
    if (legacyPath.size() > 32768 || legacyPath.find('\0') != std::string::npos)
        return rejected("invalid screenshot path");
    const auto reserve = pngSize(w, h);
    if (m_entries.size() >= MaxRequests || reserve > MaxPngBytes
        || reserve > MaxRetainedBytes - m_reservedBytes) return rejected("screenshot queue capacity exceeded");
    try {
        Entry entry;
        entry.result.ticket = {allocateIdentity(nextRequest), m_generation};
        if (!entry.result.ticket) return rejected("screenshot identity exhausted");
        entry.result.status = ScreenshotStatus::Pending;
        entry.result.error.reserve(96);
        entry.result.width = w; entry.result.height = h;
        entry.callbackName = "caesura-capture-" + std::to_string(m_generation) + "-"
                           + std::to_string(entry.result.ticket.requestId);
        entry.callbackName.reserve(64); // All generation/request IDs fit without fanout allocation.
        entry.legacyPath = legacyPath;
        entry.reservedBytes = reserve;
        const auto result = entry.result;
        m_entries.emplace(result.ticket.requestId, std::move(entry));
        m_reservedBytes += reserve;
        return result;
    } catch (const std::exception&) { return rejected("screenshot allocation failed"); }
}
ScreenshotResult ScreenshotQueue::take(const ScreenshotTicket& ticket) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_entries.find(ticket.requestId);
    if (it == m_entries.end() || it->second.result.ticket.generation != ticket.generation) return {};
    if (it->second.result.status == ScreenshotStatus::Pending) return it->second.result;
    auto result = std::move(it->second.result);
    m_reservedBytes -= it->second.reservedBytes;
    m_entries.erase(it);
    return result;
}
bool ScreenshotQueue::cancel(const ScreenshotTicket& ticket) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_entries.find(ticket.requestId);
    if (it == m_entries.end() || it->second.result.ticket.generation != ticket.generation
        || it->second.result.status != ScreenshotStatus::Pending) return false;
    finish(it->second, ScreenshotStatus::Cancelled, "screenshot cancelled");
    return true;
}
std::vector<ScreenshotQueue::Submission> ScreenshotQueue::submit(uint64_t frameId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<Submission> batch;
    if (!m_ready || frameId == 0) return batch;
    // Allocate the single dispatch before changing any entry's submission frame.
    // If it fails, every unsubmitted request receives an observable failure.
    try {
        batch.reserve(1);
        for (const auto& item : m_entries) {
            if (item.second.result.status == ScreenshotStatus::Pending && !item.second.result.frameId) {
                batch.push_back({item.second.result.ticket, item.second.callbackName});
                break;
            }
        }
    } catch (const std::exception&) {
        for (auto& item : m_entries) {
            if (item.second.result.status == ScreenshotStatus::Pending && !item.second.result.frameId)
                finish(item.second, ScreenshotStatus::Failed, "screenshot dispatch allocation failed");
        }
        batch.clear();
    }
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        auto& entry = it->second;
        if (entry.result.status == ScreenshotStatus::Pending) {
            if (!entry.result.frameId) {
                entry.callbackName = batch.front().callbackName;
                entry.result.frameId = frameId;
            } else if (frameId >= entry.result.frameId
                       && frameId - entry.result.frameId >= MaxPendingFrames) {
                finish(entry, ScreenshotStatus::Failed, "screenshot callback timeout");
            }
        }
        if (!entry.legacyPath.empty() && entry.result.status != ScreenshotStatus::Pending) {
            m_reservedBytes -= entry.reservedBytes;
            it = m_entries.erase(it);
        } else ++it;
    }
    return batch;
}
void ScreenshotQueue::complete(const char* name, uint32_t w, uint32_t h, uint32_t pitch,
                                PixelFormat format, const void* data, uint32_t size, bool flip) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ready || !name) return;
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        auto& entry = it->second;
        if (entry.callbackName != name || entry.result.status != ScreenshotStatus::Pending
            || !entry.result.frameId || entry.result.ticket.generation != m_generation) {
            ++it;
            continue;
        }
        if (!geometry(w, h) || !data || (format != PixelFormat::RGBA8 && format != PixelFormat::BGRA8)
            || uint64_t(pitch) < uint64_t(w) * 4
            || uint64_t(pitch) * (h - 1) + uint64_t(w) * 4 > size) {
            finish(entry, ScreenshotStatus::Failed, "invalid screenshot pixel buffer");
        } else {
            try {
                auto png = encode(w, h, pitch, format, static_cast<const uint8_t*>(data), flip,
                                  entry.result.width, entry.result.height);
                if (png.empty() || png.size() > entry.reservedBytes)
                    throw std::runtime_error("PNG budget exceeded");
                if (!entry.legacyPath.empty()) {
                    std::ofstream file(std::filesystem::u8path(entry.legacyPath), std::ios::binary | std::ios::trunc);
                    file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
                    file.close();
                    if (!file) throw std::runtime_error("PNG file write failed");
                }
                entry.result.png = std::move(png);
                entry.result.status = ScreenshotStatus::Completed;
            } catch (const std::exception&) { finish(entry, ScreenshotStatus::Failed, "screenshot encoding or write failed"); }
        }
        // Fire-and-forget compatibility requests have no consumer. Reclaim their
        // terminal storage here, and on timeout/close. Filenames are never identity.
        if (!entry.legacyPath.empty()) {
            m_reservedBytes -= entry.reservedBytes;
            it = m_entries.erase(it);
        } else ++it;
    }
}
RenderScreenshotCounts ScreenshotQueue::getSnapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    RenderScreenshotCounts snapshot;
    snapshot.supported = true;
    snapshot.reservedBytes = m_reservedBytes;
    for (const auto& item : m_entries) {
        const auto& result = item.second.result;
        if (result.status == ScreenshotStatus::Pending) {
            if (result.frameId == 0) ++snapshot.waiting;
            else ++snapshot.submitted;
        } else {
            ++snapshot.terminal;
        }
        snapshot.pngBytes += static_cast<uint64_t>(result.png.size());
    }
    return snapshot;
}

size_t ScreenshotQueue::retainedCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries.size();
}
size_t ScreenshotQueue::retainedBytes() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_reservedBytes;
}

bool ScreenshotQueue::beginReadbackContext(uint64_t contextGeneration) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (contextGeneration == 0 || contextGeneration <= m_readbackContext
        || m_readbackContextActive) return false;
    for (const auto& slot : m_readbacks)
        if (slot.state != ReadbackState::Empty) return false;
    m_readbackContext = contextGeneration;
    m_readbackContextActive = true;
    return true;
}

bool ScreenshotQueue::registerReadback(const Submission& submission, uint64_t contextGeneration) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ready || !m_readbackContextActive || contextGeneration == 0
        || contextGeneration != m_readbackContext || submission.ticket.generation != m_generation
        || !validSubmissionName(submission)) return false;
    // A cancelled/taken fanout anchor need not remain, but a matching submitted
    // Pending recipient must still exist before this publication is attempted.
    bool hasRecipient = false;
    for (const auto& item : m_entries) {
        const auto& entry = item.second;
        if (entry.result.status == ScreenshotStatus::Pending && entry.result.frameId != 0
            && entry.result.ticket.generation == submission.ticket.generation
            && entry.callbackName == submission.callbackName) {
            hasRecipient = true;
            break;
        }
    }
    if (!hasRecipient) return false;
    ReadbackSlot* vacant = nullptr;
    for (auto& slot : m_readbacks) {
        if (slot.state == ReadbackState::Empty) {
            if (!vacant) vacant = &slot;
        } else if (slot.token.requestId == submission.ticket.requestId
                   || submission.callbackName == slot.callbackName.data()) {
            return false;
        }
    }
    if (!vacant) return false; // Never evict an unanswered native publication.
    vacant->token = {contextGeneration, submission.ticket.requestId};
    std::copy(submission.callbackName.begin(), submission.callbackName.end(), vacant->callbackName.begin());
    vacant->callbackName[submission.callbackName.size()] = '\0';
    vacant->state = ReadbackState::Published;
    return true;
}

void ScreenshotQueue::failUnissuedSubmission(const Submission& submission, const char* reason) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!validSubmissionName(submission)) return;
    // A duplicate registration must not turn an already published fanout into
    // an unissued one. Its callback still owns the corresponding obligation.
    for (const auto& slot : m_readbacks)
        if (slot.state != ReadbackState::Empty
            && slot.token.requestId == submission.ticket.requestId
            && submission.callbackName == slot.callbackName.data()) return;
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        auto& entry = it->second;
        if (entry.result.status != ScreenshotStatus::Pending || entry.result.frameId == 0
            || entry.result.ticket.generation != submission.ticket.generation
            || entry.callbackName != submission.callbackName) {
            ++it;
            continue;
        }
        finish(entry, ScreenshotStatus::Failed, reason ? reason : "screenshot readback unavailable");
        if (!entry.legacyPath.empty()) it = m_entries.erase(it);
        else ++it;
    }
}

ScreenshotQueue::CallbackClaim ScreenshotQueue::claimReadbackCallback(
        const char* exactName, uint64_t callbackContextGeneration) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!exactName) return {};
    if (m_readbackContext == 0 && callbackContextGeneration == 0)
        return {CallbackDisposition::Unbound, {}};
    if (!m_readbackContextActive || callbackContextGeneration == 0
        || callbackContextGeneration != m_readbackContext) return {};
    for (auto& slot : m_readbacks) {
        if (slot.state != ReadbackState::Empty && slot.token.contextGeneration == callbackContextGeneration
            && std::strcmp(slot.callbackName.data(), exactName) == 0) {
            if (slot.state != ReadbackState::Published) return {}; // Duplicate Executing callback.
            slot.state = ReadbackState::Executing;
            return {CallbackDisposition::Claimed, slot.token};
        }
    }
    return {};
}

void ScreenshotQueue::finishReadbackCallback(ReadbackToken token) noexcept {
    // No allocation/destructor work beyond fixed value storage. A mutex failure
    // is fatal through noexcept; it must never be reported as a fabricated idle.
    std::lock_guard<std::mutex> lock(m_mutex);
    if (token.contextGeneration == 0 || token.requestId == 0) return;
    for (auto& slot : m_readbacks) {
        if (slot.state == ReadbackState::Executing
            && slot.token.contextGeneration == token.contextGeneration
            && slot.token.requestId == token.requestId) {
            slot = {};
            return;
        }
    }
}

bool ScreenshotQueue::retireReadbacksAfterContextShutdown(uint64_t contextGeneration) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_readbackContextActive || contextGeneration == 0 || contextGeneration != m_readbackContext)
        return false;
    for (const auto& slot : m_readbacks) {
        if (slot.state != ReadbackState::Empty
            && (slot.token.contextGeneration != contextGeneration || slot.state == ReadbackState::Executing))
            return false;
    }
    for (auto& slot : m_readbacks) slot = {};
    m_readbackContextActive = false;
    return true;
}

ScreenshotQueue::ReadbackObservation ScreenshotQueue::getReadbackSnapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    ReadbackObservation observation;
    observation.supported = m_readbackContext != 0;
    observation.ownershipComplete = observation.supported;
    observation.contextGeneration = m_readbackContext;
    observation.contextActive = m_readbackContextActive;
    for (const auto& slot : m_readbacks)
        if (slot.state != ReadbackState::Empty) ++observation.outstanding;
    return observation;
}
} // namespace Caesura
