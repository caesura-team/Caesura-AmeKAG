#include "doctest.h"
#include "render/ScreenshotQueue.h"
#include "render/BgfxDebugCallback.h"
#include <stb/stb_image.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <future>
#include <thread>

using namespace Caesura;

namespace {
std::string pathUtf8(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return std::string(bytes.begin(), bytes.end());
}
const std::array<uint8_t, 24> paddedBgra = {
    0, 0, 255, 255, 0, 255, 0, 255, 71, 72, 73, 74,
    255, 0, 0, 255, 255, 255, 255, 255, 81, 82, 83, 84
};
void complete(ScreenshotQueue& queue, const ScreenshotQueue::Submission& submission) {
    queue.complete(submission.callbackName.c_str(), 2, 2, 12,
                   ScreenshotQueue::PixelFormat::BGRA8, paddedBgra.data(),
                   static_cast<uint32_t>(paddedBgra.size()), false);
}
std::vector<uint8_t> decode(const ScreenshotResult& result, int w, int h) {
    int actualW = 0, actualH = 0, components = 0;
    auto* pixels = stbi_load_from_memory(result.png.data(), static_cast<int>(result.png.size()),
                                         &actualW, &actualH, &components, 4);
    REQUIRE(pixels != nullptr);
    CHECK(actualW == w);
    CHECK(actualH == h);
    std::vector<uint8_t> copy(pixels, pixels + actualW * actualH * 4);
    stbi_image_free(pixels);
    return copy;
}
}

TEST_CASE("U15 screenshot queue validates admission and terminal ownership") {
    ScreenshotQueue queue;
    CHECK(queue.request({}, 2, 2).status == ScreenshotStatus::Failed);
    queue.open();
    for (const auto options : {ScreenshotOptions{0, 1}, ScreenshotOptions{1, 0},
                               ScreenshotOptions{8193, 1}, ScreenshotOptions{8192, 8192},
                               ScreenshotOptions{std::numeric_limits<uint32_t>::max(), 1}}) {
        const auto rejected = queue.request(options, 2, 2);
        CHECK_FALSE(static_cast<bool>(rejected.ticket));
        CHECK(rejected.status == ScreenshotStatus::Failed);
        CHECK_FALSE(rejected.error.empty());
    }
    CHECK(queue.retainedCount() == 0);
    const auto requested = queue.request({}, 2, 2);
    REQUIRE(static_cast<bool>(requested.ticket));
    CHECK(requested.status == ScreenshotStatus::Pending);
    CHECK(queue.take(requested.ticket).status == ScreenshotStatus::Pending);
    CHECK(queue.take(requested.ticket).status == ScreenshotStatus::Pending);
    const auto submissions = queue.submit(7);
    REQUIRE(submissions.size() == 1);
    CHECK(queue.submit(8).empty());
    complete(queue, submissions[0]);
    CHECK_FALSE(queue.cancel(requested.ticket));
    complete(queue, submissions[0]);
    auto done = queue.take(requested.ticket);
    CHECK(done.status == ScreenshotStatus::Completed);
    CHECK(done.frameId == 7);
    CHECK(done.width == 2);
    CHECK(done.height == 2);
    auto rgba = decode(done, 2, 2);
    CHECK(rgba == std::vector<uint8_t>{255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255});
    CHECK(queue.take(requested.ticket).status == ScreenshotStatus::Unknown);
    CHECK(queue.retainedBytes() == 0);
}

TEST_CASE("U15 screenshot cancellation generations and instance identity reject stale callbacks") {
    ScreenshotQueue queue;
    queue.open();
    auto before = queue.request({}, 2, 2);
    CHECK(queue.cancel(before.ticket));
    CHECK_FALSE(queue.cancel(before.ticket));
    CHECK(queue.submit(1).empty());
    CHECK(queue.take(before.ticket).status == ScreenshotStatus::Cancelled);
    const auto submitted = queue.request({}, 2, 2);
    const auto batch = queue.submit(2);
    REQUIRE(batch.size() == 1);
    queue.close("device lost");
    CHECK(queue.request({}, 2, 2).status == ScreenshotStatus::Failed);
    queue.open();
    const auto fresh = queue.request({}, 2, 2);
    CHECK(fresh.ticket.generation != submitted.ticket.generation);
    complete(queue, batch[0]);
    CHECK(queue.take(submitted.ticket).status == ScreenshotStatus::Cancelled);
    CHECK(queue.take(fresh.ticket).status == ScreenshotStatus::Pending);
    ScreenshotQueue replacement;
    replacement.open();
    const auto other = replacement.request({}, 2, 2);
    CHECK(other.ticket.requestId != fresh.ticket.requestId);
    CHECK(other.ticket.generation != fresh.ticket.generation);
    complete(replacement, batch[0]);
    CHECK(replacement.take(submitted.ticket).status == ScreenshotStatus::Unknown);
    CHECK(replacement.take(other.ticket).status == ScreenshotStatus::Pending);
    auto freshBatch = queue.submit(3);
    REQUIRE(freshBatch.size() == 1);
    CHECK(queue.cancel(fresh.ticket));
    complete(queue, freshBatch[0]);
    CHECK(queue.take(fresh.ticket).status == ScreenshotStatus::Cancelled);
    const auto positive = queue.request({}, 2, 2);
    auto positiveBatch = queue.submit(4);
    REQUIRE(positiveBatch.size() == 1);
    complete(queue, positiveBatch[0]);
    CHECK(queue.take(positive.ticket).status == ScreenshotStatus::Completed);
}

TEST_CASE("U15 screenshot callback rejects malformed buffers before reads") {
    ScreenshotQueue queue;
    queue.open();
    for (int fault = 0; fault != 7; ++fault) {
        const auto request = queue.request({}, 2, 2);
        const auto batch = queue.submit(1);
        REQUIRE(batch.size() == 1);
        queue.complete(batch[0].callbackName.c_str(), fault == 0 ? 0 : (fault == 1 ? 8193 : 2),
                       2, fault == 2 ? 7 : 12,
                       fault == 3 ? ScreenshotQueue::PixelFormat::Unsupported : ScreenshotQueue::PixelFormat::RGBA8,
                       fault == 4 ? nullptr : paddedBgra.data(), fault == 5 ? 19 : (fault == 6 ? 0 : 24), false);
        auto result = queue.take(request.ticket);
        CHECK(result.status == ScreenshotStatus::Failed);
        CHECK(result.png.empty());
        CHECK_FALSE(result.error.empty());
        CHECK(queue.retainedBytes() == 0);
    }
}

TEST_CASE("U15 screenshot row pitch flip and requested resize preserve exact colors") {
    ScreenshotQueue queue;
    queue.open();
    const auto request = queue.request({4, 4}, 2, 2);
    const auto batch = queue.submit(21);
    REQUIRE(batch.size() == 1);
    queue.complete(batch[0].callbackName.c_str(), 2, 2, 12,
                   ScreenshotQueue::PixelFormat::BGRA8, paddedBgra.data(), 24, true);
    const auto result = queue.take(request.ticket);
    REQUIRE(result.status == ScreenshotStatus::Completed);
    CHECK(result.width == 4);
    CHECK(result.height == 4);
    const auto rgba = decode(result, 4, 4);
    CHECK(std::vector<uint8_t>(rgba.begin(), rgba.begin() + 8) == std::vector<uint8_t>{0,0,255,255,0,0,255,255});
    CHECK(std::vector<uint8_t>(rgba.end() - 8, rgba.end()) == std::vector<uint8_t>{0,255,0,255,0,255,0,255});
    const auto plain = queue.request({1,1}, 2, 2);
    const auto plainBatch = queue.submit(22);
    REQUIRE(plainBatch.size() == 1);
    const uint8_t pixel[] = {11,22,33,44};
    queue.complete(plainBatch[0].callbackName.c_str(), 1, 1, 4,
                   ScreenshotQueue::PixelFormat::RGBA8, pixel, 4, false);
    CHECK(decode(queue.take(plain.ticket), 1, 1) == std::vector<uint8_t>{11,22,33,44});
}

TEST_CASE("U15 screenshot one backbuffer readback fans out independent same-frame tickets") {
    ScreenshotQueue queue;
    queue.open();
    const auto a = queue.request({2,2}, 2, 2);
    const auto b = queue.request({4,4}, 2, 2);
    const auto cancelled = queue.request({1,1}, 2, 2);
    const auto batch = queue.submit(71);
    REQUIRE(batch.size() == 1);
    CHECK(queue.cancel(cancelled.ticket));
    complete(queue, batch[0]);
    auto ar = queue.take(a.ticket);
    auto br = queue.take(b.ticket);
    CHECK(ar.frameId == 71);
    CHECK(br.frameId == 71);
    REQUIRE(ar.status == ScreenshotStatus::Completed);
    REQUIRE(br.status == ScreenshotStatus::Completed);
    CHECK(decode(ar, 2, 2).size() == 16);
    CHECK(decode(br, 4, 4).size() == 64);
    CHECK(queue.take(cancelled.ticket).status == ScreenshotStatus::Cancelled);
    complete(queue, batch[0]);
    CHECK(queue.retainedCount() == 0);
    CHECK(queue.retainedBytes() == 0);

    const auto first = queue.request({2,2}, 2, 2);
    const auto second = queue.request({2,2}, 2, 2);
    const auto anchored = queue.submit(72);
    REQUIRE(anchored.size() == 1);
    const auto anchor = anchored[0].ticket;
    const auto survivor = anchor.requestId == first.ticket.requestId ? second.ticket : first.ticket;
    REQUIRE(queue.cancel(anchor));
    CHECK(queue.take(anchor).status == ScreenshotStatus::Cancelled);
    complete(queue, anchored[0]);
    CHECK(queue.take(survivor).status == ScreenshotStatus::Completed);
    CHECK(queue.retainedCount() == 0);
}

TEST_CASE("U15 screenshot retained count byte budget and timeout reclaim capacity") {
    ScreenshotQueue queue;
    queue.open();
    std::vector<ScreenshotTicket> tickets;
    for (size_t i = 0; i < ScreenshotQueue::MaxRequests; ++i)
        tickets.push_back(queue.request({}, 2, 2).ticket);
    CHECK(queue.request({}, 2, 2).status == ScreenshotStatus::Failed);
    CHECK(queue.cancel(tickets.back()));
    CHECK(queue.request({}, 2, 2).status == ScreenshotStatus::Failed);
    CHECK(queue.take(tickets.back()).status == ScreenshotStatus::Cancelled);
    CHECK(queue.request({}, 2, 2).status == ScreenshotStatus::Pending);
    queue.close("shutdown");
    CHECK(queue.retainedBytes() == 0);
    ScreenshotQueue large;
    large.open();
    const auto big = large.request({4096, 2048}, 2, 2);
    REQUIRE(static_cast<bool>(big.ticket));
    CHECK(large.request({4096, 2048}, 2, 2).status == ScreenshotStatus::Failed);
    CHECK(large.cancel(big.ticket));
    CHECK(large.request({4096, 2048}, 2, 2).status == ScreenshotStatus::Pending);
    ScreenshotQueue timeout;
    timeout.open();
    const auto pending = timeout.request({}, 2, 2);
    auto batch = timeout.submit(100);
    REQUIRE(batch.size() == 1);
    timeout.submit(100 + ScreenshotQueue::MaxPendingFrames);
    complete(timeout, batch[0]);
    CHECK(timeout.take(pending.ticket).status == ScreenshotStatus::Failed);
    CHECK(timeout.retainedBytes() == 0);
}

TEST_CASE("U15 screenshot callback instance loss and owner cancellation synchronize") {
    auto queue = std::make_shared<ScreenshotQueue>();
    queue->open();
    BgfxDebugCallback callback(queue);
    BgfxDebugCallback unrelated;
    const auto request = queue->request({}, 2, 2);
    const auto batch = queue->submit(1);
    REQUIRE(batch.size() == 1);
    std::promise<void> permission;
    auto ready = permission.get_future();
    std::thread worker([&] {
        ready.wait();
        callback.screenShot(batch[0].callbackName.c_str(), 2, 2, 12,
                            bgfx::TextureFormat::BGRA8, paddedBgra.data(), 24, false);
    });
    CHECK(queue->cancel(request.ticket));
    CHECK(queue->take(request.ticket).status == ScreenshotStatus::Cancelled);
    permission.set_value();
    worker.join();
    CHECK(queue->take(request.ticket).status == ScreenshotStatus::Unknown);
    const auto lost = queue->request({}, 2, 2);
    callback.flagDeviceLost();
    CHECK(callback.deviceLost());
    CHECK(callback.consumeDeviceLost());
    CHECK_FALSE(callback.consumeDeviceLost());
    CHECK(callback.deviceLost()); // Acknowledge notification does not reopen rendering.
    CHECK_FALSE(unrelated.consumeDeviceLost());
    CHECK_FALSE(unrelated.deviceLost());
    CHECK(queue->take(lost.ticket).status == ScreenshotStatus::Cancelled);
    CHECK(queue->request({}, 2, 2).status == ScreenshotStatus::Failed);
    callback.reset();
    queue->open();
    const auto fresh = queue->request({}, 2, 2);
    const auto freshBatch = queue->submit(2);
    REQUIRE(freshBatch.size() == 1);
    std::thread positive([&] {
        callback.screenShot(freshBatch[0].callbackName.c_str(), 2, 2, 12,
                            bgfx::TextureFormat::BGRA8, paddedBgra.data(), 24, false);
    });
    positive.join();
    CHECK(queue->take(fresh.ticket).status == ScreenshotStatus::Completed);
}

TEST_CASE("U15 screenshot thumbnail PNG remains compressed and preserves pixels") {
    ScreenshotQueue queue;
    queue.open();
    const auto request = queue.request({320,180}, 1, 1);
    const auto batch = queue.submit(1);
    REQUIRE(batch.size() == 1);
    const uint8_t pixel[] = {27,53,91,255};
    queue.complete(batch[0].callbackName.c_str(), 1, 1, 4,
                   ScreenshotQueue::PixelFormat::RGBA8, pixel, 4, false);
    const auto result = queue.take(request.ticket);
    REQUIRE(result.status == ScreenshotStatus::Completed);
    CHECK(result.png.size() < 320 * 180);
    const auto rgba = decode(result, 320, 180);
    for (size_t i = 0; i < rgba.size(); ++i) {
        if (rgba[i] != pixel[i % 4]) { FAIL("resized thumbnail pixel mismatch"); break; }
    }
}

TEST_CASE("U15 screenshot legacy UTF8 output consumes terminal bytes and cannot reuse names") {
    ScreenshotQueue queue;
    queue.open();
    const auto identity = queue.request({}, 2, 2);
    REQUIRE(queue.cancel(identity.ticket));
    queue.take(identity.ticket);
    const auto directory = std::filesystem::current_path() / ("u15-screenshot-" + std::to_string(identity.ticket.requestId));
    REQUIRE(std::filesystem::create_directory(directory));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{directory};
    const auto path = directory / std::filesystem::u8path("截图.png");
    { std::ofstream old(path, std::ios::binary); old << "old screenshot"; }
    const auto request = queue.request({}, 2, 2, pathUtf8(path));
    REQUIRE(static_cast<bool>(request.ticket));
    auto batch = queue.submit(1);
    REQUIRE(batch.size() == 1);
    CHECK(batch[0].callbackName != pathUtf8(path));
    complete(queue, batch[0]);
    CHECK(queue.retainedCount() == 0);
    CHECK(queue.retainedBytes() == 0);
    CHECK(queue.take(request.ticket).status == ScreenshotStatus::Unknown);
    ScreenshotResult saved;
    { std::ifstream file(path, std::ios::binary); saved.png.assign(std::istreambuf_iterator<char>(file), {}); }
    CHECK(decode(saved, 2, 2).size() == 16);
    const auto missing = directory / "missing" / "image.png";
    REQUIRE(static_cast<bool>(queue.request({}, 2, 2, pathUtf8(missing)).ticket));
    auto failureBatch = queue.submit(2);
    REQUIRE(failureBatch.size() == 1);
    complete(queue, failureBatch[0]);
    CHECK(queue.retainedCount() == 0);
    CHECK(queue.retainedBytes() == 0);
    const auto second = queue.request({}, 2, 2, pathUtf8(path));
    REQUIRE(static_cast<bool>(second.ticket));
    auto secondBatch = queue.submit(3);
    REQUIRE(secondBatch.size() == 1);
    complete(queue, batch[0]);
    CHECK(queue.take(second.ticket).status == ScreenshotStatus::Pending);
    queue.close("shutdown");
    complete(queue, secondBatch[0]);
    CHECK(queue.retainedCount() == 0);
}

namespace {
RenderScreenshotCounts u27QueueCounts(const ScreenshotQueue& queue, uint64_t waiting,
                                     uint64_t submitted, uint64_t terminal) {
    const auto first = queue.getSnapshot();
    const auto second = queue.getSnapshot();
    CHECK(first.supported);
    CHECK(second.supported);
    CHECK(first.waiting == waiting);
    CHECK(first.submitted == submitted);
    CHECK(first.terminal == terminal);
    CHECK(first.waiting + first.submitted + first.terminal == queue.retainedCount());
    CHECK(first.reservedBytes == queue.retainedBytes());
    CHECK(second.waiting == first.waiting);
    CHECK(second.submitted == first.submitted);
    CHECK(second.terminal == first.terminal);
    CHECK(second.reservedBytes == first.reservedBytes);
    CHECK(second.pngBytes == first.pngBytes);
    return first;
}
}

TEST_CASE("U27 screenshot snapshot: real PNG publication retains budget and transfers logical bytes once") {
    ScreenshotQueue queue;
    u27QueueCounts(queue, 0, 0, 0);
    queue.open();
    const auto request = queue.request({320, 180}, 1, 1);
    REQUIRE(static_cast<bool>(request.ticket));
    const auto admitted = u27QueueCounts(queue, 1, 0, 0);
    CHECK(admitted.reservedBytes > 0);
    CHECK(admitted.pngBytes == 0);
    CHECK(queue.take(request.ticket).status == ScreenshotStatus::Pending);
    const auto batch = queue.submit(31);
    REQUIRE(batch.size() == 1);
    const auto submitted = u27QueueCounts(queue, 0, 1, 0);
    CHECK(submitted.reservedBytes == admitted.reservedBytes);
    CHECK(submitted.pngBytes == 0);
    const uint8_t pixel[] = {27, 53, 91, 255};
    queue.complete(batch[0].callbackName.c_str(), 1, 1, 4,
                   ScreenshotQueue::PixelFormat::RGBA8, pixel, 4, false);
    const auto completed = u27QueueCounts(queue, 0, 0, 1);
    CHECK(completed.reservedBytes == admitted.reservedBytes);
    CHECK(completed.pngBytes > 0);
    CHECK(completed.pngBytes < completed.reservedBytes);
    // Repeated callbacks and observations must not take or duplicate the result.
    queue.complete(batch[0].callbackName.c_str(), 1, 1, 4,
                   ScreenshotQueue::PixelFormat::RGBA8, pixel, 4, false);
    CHECK(u27QueueCounts(queue, 0, 0, 1).pngBytes == completed.pngBytes);
    const auto result = queue.take(request.ticket);
    REQUIRE(result.status == ScreenshotStatus::Completed);
    CHECK(result.frameId == 31);
    CHECK(result.png.size() == completed.pngBytes);
    const auto decoded = decode(result, 320, 180);
    REQUIRE(decoded.size() == 320 * 180 * 4);
    CHECK(std::vector<uint8_t>(decoded.begin(), decoded.begin() + 4)
          == std::vector<uint8_t>{27, 53, 91, 255});
    const auto transferred = u27QueueCounts(queue, 0, 0, 0);
    CHECK(transferred.reservedBytes == 0);
    CHECK(transferred.pngBytes == 0);
    CHECK_FALSE(result.png.empty()); // The caller, not the queue, now owns bytes.
    CHECK(queue.take(request.ticket).status == ScreenshotStatus::Unknown);
}

TEST_CASE("U27 screenshot snapshot: fanout tickets partition waiting submitted and terminal ownership") {
    ScreenshotQueue queue;
    queue.open();
    const auto a = queue.request({2, 2}, 2, 2);
    const auto b = queue.request({4, 4}, 2, 2);
    REQUIRE(static_cast<bool>(a.ticket));
    REQUIRE(static_cast<bool>(b.ticket));
    u27QueueCounts(queue, 2, 0, 0);
    const auto batch = queue.submit(41);
    REQUIRE(batch.size() == 1); // One actual readback, two submitted tickets.
    u27QueueCounts(queue, 0, 2, 0);
    const auto later = queue.request({1, 1}, 2, 2);
    REQUIRE(static_cast<bool>(later.ticket));
    REQUIRE(queue.cancel(a.ticket));
    CHECK(u27QueueCounts(queue, 1, 1, 1).pngBytes == 0);
    complete(queue, batch[0]);
    const auto terminal = u27QueueCounts(queue, 1, 0, 2);
    auto result = queue.take(b.ticket);
    REQUIRE(result.status == ScreenshotStatus::Completed);
    CHECK(result.png.size() == terminal.pngBytes);
    CHECK(decode(result, 4, 4).size() == 64);
    CHECK(u27QueueCounts(queue, 1, 0, 1).pngBytes == 0);
    CHECK(queue.take(a.ticket).status == ScreenshotStatus::Cancelled);
    CHECK(queue.take(later.ticket).status == ScreenshotStatus::Pending);
    u27QueueCounts(queue, 1, 0, 0);
    const auto fresh = queue.submit(42);
    REQUIRE(fresh.size() == 1);
    complete(queue, fresh[0]);
    const auto remaining = u27QueueCounts(queue, 0, 0, 1);
    auto final = queue.take(later.ticket);
    REQUIRE(final.status == ScreenshotStatus::Completed);
    CHECK(final.png.size() == remaining.pngBytes);
    u27QueueCounts(queue, 0, 0, 0);
}

TEST_CASE("U27 screenshot snapshot: malformed input releases reservation but retains terminal entry") {
    ScreenshotQueue queue;
    queue.open();
    for (int fault = 0; fault != 3; ++fault) {
        const auto request = queue.request({}, 2, 2);
        REQUIRE(static_cast<bool>(request.ticket));
        const auto batch = queue.submit(51 + fault);
        REQUIRE(batch.size() == 1);
        const auto pending = u27QueueCounts(queue, 0, 1, 0);
        CHECK(pending.reservedBytes > 0);
        queue.complete(batch[0].callbackName.c_str(), 2, 2, fault == 0 ? 7 : 12,
                       fault == 1 ? ScreenshotQueue::PixelFormat::Unsupported
                                  : ScreenshotQueue::PixelFormat::BGRA8,
                       paddedBgra.data(), fault == 2 ? 19 : 24, false);
        const auto failed = u27QueueCounts(queue, 0, 0, 1);
        CHECK(failed.reservedBytes == 0);
        CHECK(failed.pngBytes == 0);
        complete(queue, batch[0]);
        u27QueueCounts(queue, 0, 0, 1);
        const auto result = queue.take(request.ticket);
        REQUIRE(result.status == ScreenshotStatus::Failed);
        CHECK_FALSE(result.error.empty());
        CHECK(result.png.empty());
        u27QueueCounts(queue, 0, 0, 0);
    }
}

TEST_CASE("U27 screenshot snapshot: timeout close and reopen retain old terminal generations") {
    ScreenshotQueue queue;
    queue.open();
    const auto timed = queue.request({}, 2, 2);
    REQUIRE(static_cast<bool>(timed.ticket));
    const auto batch = queue.submit(61);
    REQUIRE(batch.size() == 1);
    CHECK(queue.submit(61 + ScreenshotQueue::MaxPendingFrames).empty());
    const auto timeout = u27QueueCounts(queue, 0, 0, 1);
    CHECK(timeout.reservedBytes == 0);
    CHECK(timeout.pngBytes == 0);
    const auto waiting = queue.request({}, 2, 2);
    REQUIRE(static_cast<bool>(waiting.ticket));
    u27QueueCounts(queue, 1, 0, 1);
    queue.close("U27 test shutdown");
    CHECK_FALSE(queue.ready());
    CHECK(u27QueueCounts(queue, 0, 0, 2).reservedBytes == 0);
    queue.open();
    const auto fresh = queue.request({}, 2, 2);
    REQUIRE(static_cast<bool>(fresh.ticket));
    REQUIRE(fresh.ticket.generation != waiting.ticket.generation);
    u27QueueCounts(queue, 1, 0, 2);
    complete(queue, batch[0]);
    u27QueueCounts(queue, 1, 0, 2);
    CHECK(queue.take(timed.ticket).status == ScreenshotStatus::Failed);
    CHECK(queue.take(waiting.ticket).status == ScreenshotStatus::Cancelled);
    CHECK(queue.take(fresh.ticket).status == ScreenshotStatus::Pending);
    const auto next = queue.submit(62 + ScreenshotQueue::MaxPendingFrames);
    REQUIRE(next.size() == 1);
    complete(queue, next[0]);
    u27QueueCounts(queue, 0, 0, 1);
    REQUIRE(queue.take(fresh.ticket).status == ScreenshotStatus::Completed);
    u27QueueCounts(queue, 0, 0, 0);
}

TEST_CASE("U27 screenshot snapshot: actual late callback cannot recreate cancelled queue ownership") {
    auto queue = std::make_shared<ScreenshotQueue>();
    queue->open();
    BgfxDebugCallback callback(queue);
    const auto request = queue->request({}, 2, 2);
    REQUIRE(static_cast<bool>(request.ticket));
    const auto batch = queue->submit(71);
    REQUIRE(batch.size() == 1);
    std::promise<void> permission;
    auto allowed = permission.get_future();
    std::thread worker([&] {
        allowed.wait();
        callback.screenShot(batch[0].callbackName.c_str(), 2, 2, 12,
                            bgfx::TextureFormat::BGRA8, paddedBgra.data(), 24, false);
    });
    struct ReleaseAndJoin {
        std::promise<void>& permission;
        std::thread& worker;
        bool released = false;
        void finish() {
            if (!released) { permission.set_value(); released = true; }
            if (worker.joinable()) worker.join();
        }
        ~ReleaseAndJoin() { finish(); }
    } releaseAndJoin{permission, worker};
    // No REQUIRE before releasing/joining the real callback worker.
    CHECK(queue->cancel(request.ticket));
    CHECK(queue->take(request.ticket).status == ScreenshotStatus::Cancelled);
    const auto unconsumedCallback = u27QueueCounts(*queue, 0, 0, 0);
    CHECK(unconsumedCallback.reservedBytes == 0);
    CHECK(unconsumedCallback.pngBytes == 0);
    // This observes ticket ownership only, deliberately NOT callback-idle.
    releaseAndJoin.finish();
    u27QueueCounts(*queue, 0, 0, 0);
    const auto positive = queue->request({}, 2, 2);
    REQUIRE(static_cast<bool>(positive.ticket));
    const auto next = queue->submit(72);
    REQUIRE(next.size() == 1);
    callback.screenShot(next[0].callbackName.c_str(), 2, 2, 12,
                        bgfx::TextureFormat::BGRA8, paddedBgra.data(), 24, false);
    const auto retained = u27QueueCounts(*queue, 0, 0, 1);
    auto result = queue->take(positive.ticket);
    REQUIRE(result.status == ScreenshotStatus::Completed);
    CHECK(result.png.size() == retained.pngBytes);
    CHECK(decode(result, 2, 2).size() == 16);
    u27QueueCounts(*queue, 0, 0, 0);
}

TEST_CASE("U27 screenshot snapshot: rejected admission and cancellation keep exact retained capacity") {
    ScreenshotQueue queue;
    queue.open();
    std::vector<ScreenshotTicket> tickets;
    for (size_t i = 0; i != ScreenshotQueue::MaxRequests; ++i) {
        const auto request = queue.request({}, 2, 2);
        REQUIRE(static_cast<bool>(request.ticket));
        tickets.push_back(request.ticket);
    }
    const auto full = u27QueueCounts(queue, ScreenshotQueue::MaxRequests, 0, 0);
    REQUIRE(queue.request({}, 2, 2).status == ScreenshotStatus::Failed);
    CHECK(u27QueueCounts(queue, ScreenshotQueue::MaxRequests, 0, 0).reservedBytes == full.reservedBytes);
    REQUIRE(queue.cancel(tickets.back()));
    const auto cancelled = u27QueueCounts(queue, ScreenshotQueue::MaxRequests - 1, 0, 1);
    CHECK(cancelled.reservedBytes < full.reservedBytes);
    REQUIRE(queue.request({}, 2, 2).status == ScreenshotStatus::Failed);
    CHECK(queue.take(tickets.back()).status == ScreenshotStatus::Cancelled);
    const auto replacement = queue.request({}, 2, 2);
    REQUIRE(static_cast<bool>(replacement.ticket));
    CHECK(u27QueueCounts(queue, ScreenshotQueue::MaxRequests, 0, 0).reservedBytes == full.reservedBytes);
    queue.close("U27 capacity cleanup");
    CHECK(u27QueueCounts(queue, 0, 0, ScreenshotQueue::MaxRequests).reservedBytes == 0);
    for (size_t i = 0; i + 1 < tickets.size(); ++i)
        CHECK(queue.take(tickets[i]).status == ScreenshotStatus::Cancelled);
    CHECK(queue.take(replacement.ticket).status == ScreenshotStatus::Cancelled);
    u27QueueCounts(queue, 0, 0, 0);
}

// U27 callback ownership: use production queue, callback and PNG paths. These
// tests bind logical context identities only; they do not create a GPU context.
#include <chrono>
#include <exception>

namespace {
void u27Deliver(BgfxDebugCallback& callback, const ScreenshotQueue::Submission& submission) {
    callback.screenShot(submission.callbackName.c_str(), 2, 2, 12,
                        bgfx::TextureFormat::BGRA8, paddedBgra.data(), 24, false);
}
void u27ReadbackCounts(const ScreenshotQueue& queue, uint64_t outstanding,
                       uint64_t context, bool active = true) {
    const auto first = queue.getReadbackSnapshot();
    const auto second = queue.getReadbackSnapshot();
    CHECK(first.supported);
    CHECK(first.ownershipComplete);
    CHECK(first.outstanding == outstanding);
    CHECK(first.contextGeneration == context);
    CHECK(first.contextActive == active);
    CHECK(second.supported == first.supported);
    CHECK(second.ownershipComplete == first.ownershipComplete);
    CHECK(second.outstanding == first.outstanding);
    CHECK(second.contextGeneration == first.contextGeneration);
    CHECK(second.contextActive == first.contextActive);
}
void u27CheckPixels(const ScreenshotResult& result, int width = 2, int height = 2) {
    REQUIRE(result.status == ScreenshotStatus::Completed);
    const auto pixels = decode(result, width, height);
    REQUIRE(pixels.size() == static_cast<size_t>(width * height * 4));
    CHECK(std::vector<uint8_t>(pixels.begin(), pixels.begin() + 4)
          == std::vector<uint8_t>{255, 0, 0, 255});
}
struct U27ReleaseAndJoin {
    std::promise<void>& permission;
    std::thread& worker;
    bool released = false;
    void finish() {
        if (!released) { permission.set_value(); released = true; }
        if (worker.joinable()) worker.join();
    }
    ~U27ReleaseAndJoin() { finish(); }
};
}

TEST_CASE("U27 readback ledger: context binding and fanout publish one obligation") {
    auto queue = std::make_shared<ScreenshotQueue>();
    BgfxDebugCallback callback(queue);
    CHECK_FALSE(queue->getReadbackSnapshot().supported);
    CHECK_FALSE(queue->getReadbackSnapshot().ownershipComplete);
    CHECK(queue->getReadbackSnapshot().outstanding == 0);
    CHECK_FALSE(callback.bindScreenshotContext(0));
    queue->open();
    const auto compatibility = queue->request({}, 2, 2);
    REQUIRE(static_cast<bool>(compatibility.ticket));
    const auto unbound = queue->submit(1);
    REQUIRE(unbound.size() == 1);
    u27Deliver(callback, unbound[0]);
    u27CheckPixels(queue->take(compatibility.ticket));
    CHECK_FALSE(queue->getReadbackSnapshot().supported);

    // A missing implementation fails here without pretending downstream paths ran.
    REQUIRE(callback.bindScreenshotContext(101));
    u27ReadbackCounts(*queue, 0, 101);
    CHECK_FALSE(callback.bindScreenshotContext(101));
    CHECK_FALSE(callback.bindScreenshotContext(102));
    queue->open();
    const auto first = queue->request({}, 2, 2);
    const auto second = queue->request({4, 4}, 2, 2);
    const auto third = queue->request({}, 2, 2);
    REQUIRE(static_cast<bool>(first.ticket));
    REQUIRE(static_cast<bool>(second.ticket));
    REQUIRE(static_cast<bool>(third.ticket));
    const auto batch = queue->submit(2);
    REQUIRE(batch.size() == 1);
    CHECK_FALSE(queue->registerReadback(batch[0], 102));
    auto fabricated = batch[0];
    fabricated.callbackName += "-other";
    CHECK_FALSE(queue->registerReadback(fabricated, 101));
    REQUIRE(queue->registerReadback(batch[0], 101));
    CHECK_FALSE(queue->registerReadback(batch[0], 101));
    u27ReadbackCounts(*queue, 1, 101);
    u27QueueCounts(*queue, 0, 3, 0);
    u27Deliver(callback, batch[0]);
    u27ReadbackCounts(*queue, 0, 101);
    u27QueueCounts(*queue, 0, 0, 3);
    u27CheckPixels(queue->take(first.ticket));
    u27CheckPixels(queue->take(second.ticket), 4, 4);
    u27CheckPixels(queue->take(third.ticket));
    u27Deliver(callback, batch[0]);
    u27ReadbackCounts(*queue, 0, 101);
    u27QueueCounts(*queue, 0, 0, 0);
}

TEST_CASE("U27 readback ledger: cancelled and taken entries retain a late callback obligation") {
    auto queue = std::make_shared<ScreenshotQueue>();
    BgfxDebugCallback callback(queue);
    REQUIRE(callback.bindScreenshotContext(201));
    queue->open();
    const auto first = queue->request({}, 2, 2);
    const auto second = queue->request({}, 2, 2);
    REQUIRE(static_cast<bool>(first.ticket));
    REQUIRE(static_cast<bool>(second.ticket));
    const auto batch = queue->submit(1);
    REQUIRE(batch.size() == 1);
    REQUIRE(queue->registerReadback(batch[0], 201));
    std::promise<void> permission;
    auto allowed = permission.get_future();
    std::thread worker([&] { allowed.wait(); u27Deliver(callback, batch[0]); });
    U27ReleaseAndJoin release{permission, worker};
    CHECK(queue->cancel(first.ticket));
    CHECK(queue->cancel(second.ticket));
    CHECK(queue->take(first.ticket).status == ScreenshotStatus::Cancelled);
    CHECK(queue->take(second.ticket).status == ScreenshotStatus::Cancelled);
    const auto emptyEntries = u27QueueCounts(*queue, 0, 0, 0);
    CHECK(emptyEntries.reservedBytes == 0);
    CHECK(emptyEntries.pngBytes == 0);
    u27ReadbackCounts(*queue, 1, 201);
    callback.beginShutdown();
    CHECK_FALSE(queue->ready());
    u27ReadbackCounts(*queue, 1, 201);
    release.finish(); // Actual callback now encounters complete()'s closed-queue exit.
    u27ReadbackCounts(*queue, 0, 201);
    u27QueueCounts(*queue, 0, 0, 0);
    callback.reset();
    queue->open();
    const auto fresh = queue->request({}, 2, 2);
    REQUIRE(static_cast<bool>(fresh.ticket));
    const auto next = queue->submit(2);
    REQUIRE(next.size() == 1);
    REQUIRE(queue->registerReadback(next[0], 201));
    u27ReadbackCounts(*queue, 1, 201);
    u27Deliver(callback, next[0]);
    u27CheckPixels(queue->take(fresh.ticket));
    u27ReadbackCounts(*queue, 0, 201);
}

TEST_CASE("U27 readback ledger: unanswered requests reach a bounded cap without forgetting debt") {
    auto queue = std::make_shared<ScreenshotQueue>();
    BgfxDebugCallback callback(queue);
    REQUIRE(callback.bindScreenshotContext(301));
    queue->open();
    CHECK(ScreenshotQueue::MaxOutstandingReadbacks == 8);
    std::vector<ScreenshotQueue::Submission> unanswered;
    for (size_t i = 0; i != ScreenshotQueue::MaxOutstandingReadbacks; ++i) {
        const auto request = queue->request({}, 2, 2);
        REQUIRE(static_cast<bool>(request.ticket));
        const uint64_t frame = 10 + i * (ScreenshotQueue::MaxPendingFrames + 2);
        const auto batch = queue->submit(frame);
        REQUIRE(batch.size() == 1);
        REQUIRE(queue->registerReadback(batch[0], 301));
        unanswered.push_back(batch[0]);
        if (i % 2 == 0) {
            CHECK(queue->submit(frame + ScreenshotQueue::MaxPendingFrames).empty());
            CHECK(queue->take(request.ticket).status == ScreenshotStatus::Failed);
        } else {
            CHECK(queue->cancel(request.ticket));
            CHECK(queue->take(request.ticket).status == ScreenshotStatus::Cancelled);
        }
        u27QueueCounts(*queue, 0, 0, 0);
        u27ReadbackCounts(*queue, i + 1, 301);
    }
    callback.beginShutdown();
    callback.reset();
    queue->open();
    u27ReadbackCounts(*queue, ScreenshotQueue::MaxOutstandingReadbacks, 301);
    const auto excess = queue->request({}, 2, 2);
    const auto excessFanout = queue->request({4, 4}, 2, 2);
    REQUIRE(static_cast<bool>(excess.ticket));
    REQUIRE(static_cast<bool>(excessFanout.ticket));
    const auto refused = queue->submit(2000);
    REQUIRE(refused.size() == 1);
    CHECK_FALSE(queue->registerReadback(refused[0], 301));
    // This tests the production queue's failure transition, not native-call ordering.
    queue->failUnissuedSubmission(refused[0], "screenshot readback capacity exceeded");
    const auto failure = queue->take(excess.ticket);
    CHECK(failure.status == ScreenshotStatus::Failed);
    CHECK_FALSE(failure.error.empty());
    const auto fanoutFailure = queue->take(excessFanout.ticket);
    CHECK(fanoutFailure.status == ScreenshotStatus::Failed);
    CHECK_FALSE(fanoutFailure.error.empty());
    u27QueueCounts(*queue, 0, 0, 0);
    u27ReadbackCounts(*queue, ScreenshotQueue::MaxOutstandingReadbacks, 301);
    u27Deliver(callback, unanswered.front());
    u27ReadbackCounts(*queue, ScreenshotQueue::MaxOutstandingReadbacks - 1, 301);
    const auto fresh = queue->request({}, 2, 2);
    REQUIRE(static_cast<bool>(fresh.ticket));
    const auto next = queue->submit(2001);
    REQUIRE(next.size() == 1);
    REQUIRE(queue->registerReadback(next[0], 301));
    u27ReadbackCounts(*queue, ScreenshotQueue::MaxOutstandingReadbacks, 301);
    u27Deliver(callback, next[0]);
    u27CheckPixels(queue->take(fresh.ticket));
    u27ReadbackCounts(*queue, ScreenshotQueue::MaxOutstandingReadbacks - 1, 301);
    // Explicit logical post-shutdown notification; actual Core shutdown is probe scope.
    CHECK(callback.screenshotContextShutdownComplete());
    u27ReadbackCounts(*queue, 0, 301, false);
}

TEST_CASE("U27 readback ledger: wrong stale and duplicate callbacks cannot retire another identity") {
    auto queue = std::make_shared<ScreenshotQueue>();
    BgfxDebugCallback callback(queue);
    REQUIRE(callback.bindScreenshotContext(401));
    queue->open();
    const auto old = queue->request({}, 2, 2);
    REQUIRE(static_cast<bool>(old.ticket));
    const auto oldBatch = queue->submit(1);
    REQUIRE(oldBatch.size() == 1);
    REQUIRE(queue->registerReadback(oldBatch[0], 401));
    queue->close("logical capture admission closes without context shutdown");
    queue->open();
    const auto current = queue->request({}, 2, 2);
    REQUIRE(static_cast<bool>(current.ticket));
    REQUIRE(current.ticket.generation != old.ticket.generation);
    const auto currentBatch = queue->submit(2);
    REQUIRE(currentBatch.size() == 1);
    REQUIRE(queue->registerReadback(currentBatch[0], 401));
    BgfxDebugCallback unrelated(queue); // Same queue, but not this context owner.
    u27Deliver(unrelated, currentBatch[0]);
    callback.screenShot(nullptr, 2, 2, 12, bgfx::TextureFormat::BGRA8, paddedBgra.data(), 24, false);
    auto wrong = currentBatch[0];
    wrong.callbackName += "-wrong";
    u27Deliver(callback, wrong);
    queue->finishReadbackCallback({402, currentBatch[0].ticket.requestId});
    CHECK_FALSE(queue->retireReadbacksAfterContextShutdown(402));
    u27ReadbackCounts(*queue, 2, 401);
    CHECK(queue->take(current.ticket).status == ScreenshotStatus::Pending);
    u27Deliver(callback, oldBatch[0]);
    u27ReadbackCounts(*queue, 1, 401);
    CHECK(queue->take(old.ticket).status == ScreenshotStatus::Cancelled);
    CHECK(queue->take(current.ticket).status == ScreenshotStatus::Pending);
    u27Deliver(callback, oldBatch[0]);
    u27ReadbackCounts(*queue, 1, 401);
    u27Deliver(callback, currentBatch[0]);
    u27CheckPixels(queue->take(current.ticket));
    u27ReadbackCounts(*queue, 0, 401);
    CHECK(callback.screenshotContextShutdownComplete());
    u27ReadbackCounts(*queue, 0, 401, false);
    CHECK_FALSE(callback.bindScreenshotContext(401));
    REQUIRE(callback.bindScreenshotContext(402));
    queue->open();
    const auto fresh = queue->request({}, 2, 2);
    REQUIRE(static_cast<bool>(fresh.ticket));
    const auto next = queue->submit(3);
    REQUIRE(next.size() == 1);
    REQUIRE(queue->registerReadback(next[0], 402));
    u27Deliver(callback, oldBatch[0]);
    u27Deliver(callback, currentBatch[0]);
    CHECK_FALSE(queue->retireReadbacksAfterContextShutdown(401));
    u27ReadbackCounts(*queue, 1, 402);
    CHECK(queue->take(fresh.ticket).status == ScreenshotStatus::Pending);
    u27Deliver(callback, next[0]);
    u27CheckPixels(queue->take(fresh.ticket));
    u27ReadbackCounts(*queue, 0, 402);
}

TEST_CASE("U27 readback ledger: malformed and failed callback output still retires its own obligation") {
    auto queue = std::make_shared<ScreenshotQueue>();
    BgfxDebugCallback callback(queue);
    REQUIRE(callback.bindScreenshotContext(501));
    queue->open();
    for (unsigned fault = 0; fault != 3; ++fault) {
        const auto request = queue->request({}, 2, 2);
        REQUIRE(static_cast<bool>(request.ticket));
        const auto batch = queue->submit(fault + 1);
        REQUIRE(batch.size() == 1);
        REQUIRE(queue->registerReadback(batch[0], 501));
        callback.screenShot(batch[0].callbackName.c_str(), 2, 2, fault == 0 ? 7 : 12,
                            fault == 1 ? bgfx::TextureFormat::Unknown : bgfx::TextureFormat::BGRA8,
                            paddedBgra.data(), fault == 2 ? 19 : 24, false);
        u27ReadbackCounts(*queue, 0, 501);
        const auto result = queue->take(request.ticket);
        CHECK(result.status == ScreenshotStatus::Failed);
        CHECK_FALSE(result.error.empty());
        CHECK(result.png.empty());
        u27QueueCounts(*queue, 0, 0, 0);
    }
    const auto identity = queue->request({}, 2, 2);
    REQUIRE(static_cast<bool>(identity.ticket));
    REQUIRE(queue->cancel(identity.ticket));
    CHECK(queue->take(identity.ticket).status == ScreenshotStatus::Cancelled);
    const auto directory = std::filesystem::current_path()
                         / ("u27-readback-" + std::to_string(identity.ticket.requestId));
    REQUIRE(std::filesystem::create_directory(directory));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{directory};
    // Opening a directory as an output stream fails on both supported desktop hosts.
    const auto legacy = queue->request({}, 2, 2, pathUtf8(directory));
    REQUIRE(static_cast<bool>(legacy.ticket));
    const auto failedOutput = queue->submit(4);
    REQUIRE(failedOutput.size() == 1);
    REQUIRE(queue->registerReadback(failedOutput[0], 501));
    u27Deliver(callback, failedOutput[0]);
    CHECK(std::filesystem::is_directory(directory));
    CHECK(std::filesystem::is_empty(directory));
    CHECK(queue->take(legacy.ticket).status == ScreenshotStatus::Unknown);
    u27QueueCounts(*queue, 0, 0, 0);
    u27ReadbackCounts(*queue, 0, 501);
    const auto positive = queue->request({}, 2, 2);
    REQUIRE(static_cast<bool>(positive.ticket));
    const auto next = queue->submit(5);
    REQUIRE(next.size() == 1);
    REQUIRE(queue->registerReadback(next[0], 501));
    u27Deliver(callback, next[0]);
    u27CheckPixels(queue->take(positive.ticket));
    u27ReadbackCounts(*queue, 0, 501);
}

TEST_CASE("U27 readback ledger: active callback blocks premature context retirement") {
    auto queue = std::make_shared<ScreenshotQueue>();
    BgfxDebugCallback callback(queue);
    REQUIRE(callback.bindScreenshotContext(601));
    queue->open();
    const auto request = queue->request({}, 2, 2);
    const auto retained = queue->request({}, 2, 2);
    REQUIRE(static_cast<bool>(request.ticket));
    REQUIRE(static_cast<bool>(retained.ticket));
    const auto batch = queue->submit(1);
    REQUIRE(batch.size() == 1);
    REQUIRE(queue->registerReadback(batch[0], 601));
    std::promise<void> claimed, permission, processed, finishPermission;
    auto reached = claimed.get_future();
    auto allowed = permission.get_future();
    auto afterComplete = processed.get_future();
    auto allowedToReturn = finishPermission.get_future();
    struct Barrier {
        std::promise<void>& claimed;
        std::future<void>& allowed;
        std::promise<void>& processed;
        std::future<void>& allowedToReturn;
    } barrier{claimed, allowed, processed, allowedToReturn};
    std::exception_ptr workerError;
    std::thread worker([&] {
        try {
            BgfxDebugCallback::ScopedScreenshotCheckpoint hook([](
                    BgfxDebugCallback::ScreenshotCheckpoint checkpoint, void* opaque) {
                auto& state = *static_cast<Barrier*>(opaque);
                if (checkpoint == BgfxDebugCallback::ScreenshotCheckpoint::AfterClaim) {
                    state.claimed.set_value();
                    state.allowed.wait();
                } else {
                    state.processed.set_value();
                    state.allowedToReturn.wait();
                }
            }, &barrier);
            u27Deliver(callback, batch[0]);
        } catch (...) { workerError = std::current_exception(); }
    });
    struct ReleaseBothAndJoin {
        std::promise<void>& permission;
        std::promise<void>& finishPermission;
        std::thread& worker;
        bool processingReleased = false;
        bool returnReleased = false;
        void allowProcessing() {
            if (!processingReleased) { permission.set_value(); processingReleased = true; }
        }
        void finish() {
            allowProcessing();
            if (!returnReleased) { finishPermission.set_value(); returnReleased = true; }
            if (worker.joinable()) worker.join();
        }
        ~ReleaseBothAndJoin() { finish(); }
    } release{permission, finishPermission, worker};
    const bool pausedAfterClaim = reached.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    CHECK(pausedAfterClaim);
    if (pausedAfterClaim) {
        u27ReadbackCounts(*queue, 1, 601);
        CHECK(queue->take(request.ticket).status == ScreenshotStatus::Pending);
        // This actual concurrent duplicate must neither encode nor retire the first call.
        u27Deliver(callback, batch[0]);
        u27ReadbackCounts(*queue, 1, 601);
        CHECK(queue->take(request.ticket).status == ScreenshotStatus::Pending);
        CHECK_FALSE(callback.screenshotContextShutdownComplete());
        CHECK_FALSE(queue->retireReadbacksAfterContextShutdown(601));
        CHECK_FALSE(callback.bindScreenshotContext(602));
        u27ReadbackCounts(*queue, 1, 601);
    }
    release.allowProcessing();
    const bool pausedAfterComplete = afterComplete.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    CHECK(pausedAfterComplete);
    ScreenshotResult transferred;
    if (pausedAfterComplete) {
        // Real encoding/publication has finished, but this actual callback has
        // not returned. Taking its PNG cannot retire the executing obligation.
        u27QueueCounts(*queue, 0, 0, 2);
        transferred = queue->take(request.ticket);
        CHECK(transferred.status == ScreenshotStatus::Completed);
        CHECK_FALSE(transferred.png.empty());
        u27QueueCounts(*queue, 0, 0, 1);
        u27ReadbackCounts(*queue, 1, 601);
        u27Deliver(callback, batch[0]);
        CHECK_FALSE(callback.screenshotContextShutdownComplete());
        CHECK_FALSE(queue->retireReadbacksAfterContextShutdown(601));
        u27ReadbackCounts(*queue, 1, 601);
    }
    release.finish();
    CHECK(workerError == nullptr);
    REQUIRE(pausedAfterClaim);
    REQUIRE(pausedAfterComplete);
    u27CheckPixels(transferred);
    u27ReadbackCounts(*queue, 0, 601);
    u27QueueCounts(*queue, 0, 0, 1);
    CHECK(callback.screenshotContextShutdownComplete());
    u27ReadbackCounts(*queue, 0, 601, false);
    REQUIRE(callback.bindScreenshotContext(602));
    queue->open();
    u27ReadbackCounts(*queue, 0, 602);
    // Context retirement/rebinding does not consume retained terminal PNG ownership.
    u27QueueCounts(*queue, 0, 0, 1);
    u27CheckPixels(queue->take(retained.ticket));
    const auto fresh = queue->request({}, 2, 2);
    REQUIRE(static_cast<bool>(fresh.ticket));
    const auto next = queue->submit(2);
    REQUIRE(next.size() == 1);
    REQUIRE(queue->registerReadback(next[0], 602));
    u27Deliver(callback, next[0]);
    u27CheckPixels(queue->take(fresh.ticket));
    u27ReadbackCounts(*queue, 0, 602);
}
