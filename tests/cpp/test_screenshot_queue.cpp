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
