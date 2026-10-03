#include "doctest.h"
#include "render/VideoPlayer.h"
#include "render/api/IVideoPlayer.h"
#include <fstream>
#include <cstdio>
#include <iterator>

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
    JobSystemSnapshot getSnapshot() const override {
        return {false, true, 0, 0, 0};
    }
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

TEST_CASE("VideoPlayer asset memory decodes real MPG and releases its owned input") {
    constexpr wchar_t childEnv[]=L"CAESURA_VIDEO_ASSET_MEMORY_CHILD";
    constexpr wchar_t testName[]=L"VideoPlayer asset memory decodes real MPG and releases its owned input";
    if(!CaesuraTest::isGpuChildProcess(childEnv)) {
        CHECK(CaesuraTest::runGpuChildProcess(childEnv,testName)==ERROR_SUCCESS);return;
    }
    CaesuraTest::HiddenSdlWindow window(64,64);REQUIRE(window);
    BgfxRenderDevice device;REQUIRE(device.setPreferredBackend("dx11"));REQUIRE(device.init(window.nativeHandle(),64,64));
    {
        std::ifstream file(kVideoFixture,std::ios::binary);REQUIRE(file.good());
        const std::vector<uint8_t> original((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
        REQUIRE(original.size()>1024);
        ImmediateVideoJobs jobs;VideoAudioCapture audio;VideoPlayer player;player.setJobSystem(jobs);
        auto callerBytes=original;
        const auto first=player.openMemory(std::move(callerBytes));REQUIRE(static_cast<bool>(first));
        callerBytes.assign(4096,0xFF); // Decoder must own its original bytes, not this later caller buffer.
        player.setLoop(first,true);player.setVolume(first,0.4f);
        decodeVideoUntilAudio(player,first,audio);
        CHECK(player.lastMemorySecondaryIoRefusals()==0);
        player.close(first);CHECK_FALSE(player.isPlaying(first));player.updateAll(0);
        CHECK(player.activeCount()==0);CHECK(audio.live.empty());
        const auto second=player.openMemory(original);REQUIRE(static_cast<bool>(second));CHECK_FALSE(first==second);
        decodeVideoUntilAudio(player,second,audio);
        player.closeAll();player.updateAll(0);CHECK(player.activeCount()==0);CHECK(audio.live.empty());
        player.shutdown();CHECK(player.activeCount()==0);
    }
    bgfx::frame();device.shutdown();
}
#endif

TEST_CASE("VideoPlayer asset memory rejects missing oversized and corrupt input") {
    VideoPlayer player;
    CHECK_FALSE(static_cast<bool>(player.openMemory({})));
    CHECK_FALSE(static_cast<bool>(player.openMemory(std::vector<uint8_t>(64u*1024u*1024u+1,0))));
    CHECK_FALSE(static_cast<bool>(player.openMemory({'n','o','t','-','a','-','v','i','d','e','o'})));
    CHECK(player.activeCount()==0);
    player.shutdown();CHECK(player.activeCount()==0);
}
#ifdef CAESURA_VIDEO_FFMPEG
TEST_CASE("VideoPlayer asset memory FFmpeg refuses external URL playlist input") {
    // Current FFmpeg rejects HLS memory input at sniffing because it has no
    // trusted filename/MIME. This is early refusal, not proof that secondary
    // io_open was called. Other FFmpeg versions may reach the same deny layer.
    const std::string playlist="#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n"
        "#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:1,\nfile:///__caesura_forbidden_secondary__/segment.ts\n#EXT-X-ENDLIST\n";
    VideoPlayer player;
    const auto result=player.openMemory(std::vector<uint8_t>(playlist.begin(),playlist.end()));
    CHECK_FALSE(static_cast<bool>(result));
    INFO("secondary refusals=" << player.lastMemorySecondaryIoRefusals()
         << "; zero denotes pre-I/O rejection, not callback coverage");
    CHECK(player.activeCount()==0);
    player.shutdown();CHECK(player.activeCount()==0);
}
TEST_CASE("VideoPlayer asset memory FFmpeg rejects concat secondary protocol") {
    // Self-identifying input reaches the real concat demuxer. Current FFmpeg
    // rejects its external segment at the empty protocol whitelist BEFORE
    // the custom io_open callback; that earlier defense is correct behavior.
    // The root acceptance pairs this receipt with exact captured FFmpeg stderr.
    // Callback execution is unverified here and is never inferred from refusal.
    const std::string playlist="ffconcat version 1.0\nfile '__caesura_forbidden_secondary__.ts'\n";
    VideoPlayer player;
    const auto result=player.openMemory(std::vector<uint8_t>(playlist.begin(),playlist.end()));
    CHECK_FALSE(static_cast<bool>(result));
    CHECK(player.activeCount()==0);
    std::printf("VIDEO_MEMORY_SECONDARY_DIAGNOSTIC:{\"kind\":\"concat\",\"opened\":%s,\"active\":%d,\"io_open_refusals\":%u}\n",
        result ? "true" : "false",player.activeCount(),player.lastMemorySecondaryIoRefusals());
    player.shutdown();CHECK(player.activeCount()==0);
}

#endif
