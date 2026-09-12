#include "doctest.h"
#include "render/VideoPlayer.h"
#include "render/api/IVideoPlayer.h"

#if defined(_WIN32)
#include "HiddenGpuContext.h"
#include "audio/NullAudioBackend.h"
#include "di/BackendRegistry.h"
#include "job/api/IJobSystem.h"
#include "render/BgfxRenderDevice.h"
#include <algorithm>
#include <cmath>
#include <unordered_set>
#endif

using namespace Caesura;

TEST_CASE("VideoPlayer closeAll is safe for an empty player through its interface") {
    VideoPlayer player;
    IVideoPlayer& video = player;
    video.closeAll();
    video.closeAll();
    CHECK(video.activeCount() == 0);
    CHECK_FALSE(video.isPlaying(VideoHandle{}));
    video.updateAll(0.0);
    CHECK(video.activeCount() == 0);
    video.shutdown();
    video.closeAll();
    CHECK(video.activeCount() == 0);
}

TEST_CASE("VideoPlayer closeAll leaves failed opens and unknown handles inert") {
    VideoPlayer player;
    IVideoPlayer& video = player;
    const VideoHandle missing = video.open("__u11_missing_video__.mpg");
    REQUIRE_FALSE(static_cast<bool>(missing));
    video.close(missing);
    video.close(VideoHandle{99});
    video.closeAll();
    video.resume(VideoHandle{99});
    CHECK_FALSE(video.update(VideoHandle{99}, 0.016));
    CHECK_FALSE(video.isPlaying(VideoHandle{99}));
    CHECK(video.hasEnded(VideoHandle{99}));
    video.updateAll(0.016);
    CHECK(video.activeCount() == 0);
}

#if defined(_WIN32)
namespace {

// Runs the production decoder body synchronously; this fixture verifies real
// decoding and close/flush behavior, not overlap with an active worker thread.
class ImmediateVideoJobs final : public IJobSystem {
public:
    void init() override {}
    void shutdown() override {}
    uint64_t submit(JobFn work, JobPriority, MainThreadFn complete) override {
        ++submitted;
        work();
        if (complete) complete();
        return submitted;
    }
    void pollMainThreadJobs() override {}
    void waitIdle() override {}
    int workerCount() const override { return 0; }
    int pendingJobs() const override { return 0; }
    bool isRunning() const override { return true; }
    uint64_t submitted = 0;
};

// Records the PCM that the real decoder submits, without opening an audio
// device. The close contract must stop every submitted voice exactly once.
class VideoAudioCapture final : public NullAudioBackend {
public:
    // PCM submission is simulated by this fixture without a device session.
    bool isPlaybackAvailable() const override { return true; }
    VideoAudioCapture()
        : previous(BackendRegistry::instance().getAudioBackend()) {
        BackendRegistry::instance().setAudioBackend(this);
    }
    ~VideoAudioCapture() override {
        BackendRegistry::instance().setAudioBackend(previous);
    }
    unsigned int playRawPCM(const float* samples, unsigned int frames,
                           unsigned int sampleRate, unsigned int channels) override {
        CHECK(sampleRate == 44100);
        CHECK(channels == 2);
        lastPeak = 0.0f;
        bool allFinite = true;
        for (size_t i = 0; i < static_cast<size_t>(frames) * channels; ++i) {
            allFinite = allFinite && std::isfinite(samples[i]);
            lastPeak = std::max(lastPeak, std::fabs(samples[i]));
        }
        CHECK(allFinite);
        live.insert(++submitted);
        return submitted;
    }
    void stopSEHandle(unsigned int handle) override {
        CHECK(live.erase(handle) == 1);
        ++stopped;
    }
    std::unordered_set<unsigned int> live;
    unsigned int submitted = 0;
    unsigned int stopped = 0;
    float lastPeak = 0.0f;
private:
    IAudioBackend* previous;
};

constexpr const char* kVideoFixture = "tests/audio/restore-video.mpg";

void decodeVideoUntilAudio(IVideoPlayer& video, VideoHandle handle,
                           VideoAudioCapture& audio) {
    const auto previousSubmissions = audio.submitted;
    REQUIRE(video.width(handle) == 32);
    REQUIRE(video.height(handle) == 32);
    REQUIRE(video.duration(handle) > 1.0);
    for (int frame = 0; frame < 60 && audio.submitted == previousSubmissions; ++frame) {
        video.update(handle, 1.0 / 25.0);
        bgfx::frame();
    }
    REQUIRE(audio.submitted > previousSubmissions);
    CHECK(audio.lastPeak > 0.001f);
    CHECK(video.getTexture(handle) != 0);
    CHECK(video.isPlaying(handle));
}

void verifyClosedVideosStaySilent(IVideoPlayer& video, VideoHandle first,
                                  VideoHandle second, VideoAudioCapture& audio,
                                  ImmediateVideoJobs& jobs) {
    const auto submittedAudio = audio.submitted;
    const auto submittedJobs = jobs.submitted;
    REQUIRE(audio.live.size() == 2);
    video.closeAll();
    CHECK(audio.live.empty());
    CHECK(audio.stopped == submittedAudio);
    CHECK(video.activeCount() == 2); // physical release remains deferred
    for (const auto handle : {first, second}) {
        CHECK_FALSE(video.isPlaying(handle));
        CHECK(video.hasEnded(handle));
        video.resume(handle);
        CHECK_FALSE(video.isPlaying(handle));
        CHECK_FALSE(video.update(handle, 0.04));
    }
    video.closeAll();
    CHECK(audio.submitted == submittedAudio);
    CHECK(audio.stopped == submittedAudio);
    CHECK(jobs.submitted == submittedJobs);
}

void verifyVideoReopensBeforeFlush(IVideoPlayer& video, VideoHandle first,
                                  VideoHandle second, VideoAudioCapture& audio) {
    const VideoHandle fresh = video.open(kVideoFixture);
    REQUIRE(static_cast<bool>(fresh));
    CHECK_FALSE(fresh == first);
    CHECK_FALSE(fresh == second);
    CHECK(video.activeCount() == 3); // two pending plus the new video
    video.updateAll(0.0);
    CHECK(video.activeCount() == 1);
    CHECK(video.width(first) == 0);
    CHECK(video.width(second) == 0);
    video.close(first);
    video.close(second);
    decodeVideoUntilAudio(video, fresh, audio);
    video.closeAll();
    CHECK(audio.live.empty());
    video.updateAll(0.0);
    CHECK(video.activeCount() == 0);
    CHECK(audio.stopped == audio.submitted);
}

} // namespace

TEST_CASE("VideoPlayer closeAll stops decoded videos and permits reopening before deferred release") {
    constexpr wchar_t childEnv[] = L"CAESURA_VIDEO_CLOSE_ALL_CHILD";
    constexpr wchar_t testName[] =
        L"VideoPlayer closeAll stops decoded videos and permits reopening before deferred release";
    if (!CaesuraTest::isGpuChildProcess(childEnv)) {
        CHECK(CaesuraTest::runGpuChildProcess(childEnv, testName) == ERROR_SUCCESS);
        return;
    }
    CaesuraTest::HiddenSdlWindow window(64, 64);
    REQUIRE(window);
    BgfxRenderDevice device;
    REQUIRE(device.setPreferredBackend("dx11"));
    REQUIRE(device.init(window.nativeHandle(), 64, 64));
    {
        ImmediateVideoJobs jobs;
        VideoAudioCapture audio;
        VideoPlayer player;
        player.setJobSystem(jobs);
        IVideoPlayer& video = player;
        const VideoHandle first = video.open(kVideoFixture);
        const VideoHandle second = video.open(kVideoFixture);
        REQUIRE(static_cast<bool>(first));
        REQUIRE(static_cast<bool>(second));
        REQUIRE_FALSE(first == second);
        decodeVideoUntilAudio(video, first, audio);
        decodeVideoUntilAudio(video, second, audio);
        verifyClosedVideosStaySilent(video, first, second, audio, jobs);
        verifyVideoReopensBeforeFlush(video, first, second, audio);
    }
    bgfx::frame();
    device.shutdown();
}
#endif
