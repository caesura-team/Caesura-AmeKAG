#pragma once
#include "api/IVideoPlayer.h"
#include "../di/api/IDeviceLostListener.h"
#include "../job/api/IJobSystem.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <mutex>
#include <bgfx/bgfx.h>

namespace Caesura {


// -- DecodedFrame: worker -> main-thread transfer buffer -------------------
struct DecodedFrame {
    std::vector<uint8_t> rgba;
    int width  = 0;
    int height = 0;
    bool valid = false;
};

// ============================================================================
// VideoPlayer -- MPEG1/FFmpeg video playback, implements IVideoPlayer
// ============================================================================

class VideoPlayer : public IVideoPlayer, public IDeviceLostListener {
public:
    VideoPlayer();
    ~VideoPlayer() override;

    // IDeviceLostListener: GPU loss invalidates every video texture; mark
    // them so the next frame re-uploads instead of submitting stale handles.
    void onDeviceLost() override;
    void onDeviceRestored() override {}

    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    void setJobSystem(IJobSystem& js) { m_jobSystem = &js; }

    VideoHandle open(const char* path) override;
    VideoHandle openMemory(std::vector<uint8_t> bytes) override;
    // Diagnostic counters for actual custom-I/O rejection regression; no URLs.
    unsigned lastMemorySecondaryIoRefusals() const { return m_memoryIoRefusals; }
    void close(VideoHandle handle) override;
    void closeAll() override;
    void setLoop(VideoHandle handle, bool loop) override;
    void setVolume(VideoHandle handle, float volume) override;
    bool update(VideoHandle handle, double dt) override;
    void updateAll(double dt) override;
    uint32_t getTexture(VideoHandle handle) const override;

    bool isPlaying(VideoHandle handle) const override;
    bool hasEnded(VideoHandle handle) const override;
    int  width(VideoHandle handle) const override;
    int  height(VideoHandle handle) const override;
    double duration(VideoHandle handle) const override;
    double currentTime(VideoHandle handle) const override;

    void pause(VideoHandle handle) override;
    void resume(VideoHandle handle) override;
    void seek(VideoHandle handle, double time) override;

    void shutdown() override;
    int  activeCount() const override { return static_cast<int>(m_videos.size()); }

    // pl_mpeg audio callback dispatch (worker thread). void* keeps pl_mpeg
    // C types out of this header; the .cpp casts.
    void onAudioDecoded(void* plm, void* samples);

    // -- drain-plan helper (pure, unit-testable) ---------------------------
    // Decides how many whole 1-second chunks fit in `chunkFloats` and whether
    // the remainder should be requeued (size-bound exit) or dropped (playback
    // failure). Returns { chunks, dropRemainder }.
    struct DrainPlan {
        size_t chunks = 0;         // whole chunks that fit
        bool   dropRemainder = false;  // true = playRawPCM failed
    };
    static DrainPlan planDrain(size_t chunkFloats, size_t framesPerChunk,
                               bool playFailed);
    // Clamp a seek time to [0, duration]. NaN/-Inf/huge finite values and
    // values beyond the bound resolve to 0; duration <= 0 (unknown) skips
    // the upper clamp. Pure, unit-testable.
    static double clampSeekTime(double time, double duration);

private:
    struct MemoryInput;
    struct VideoState {
        std::shared_ptr<MemoryInput> memory;
        std::shared_ptr<void> customIo;
        void*  plm = nullptr;
        bool   useFFmpeg = false;
        bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
        int    width  = 0;
        int    height = 0;
        double duration = 0.0;
        bool   playing = true;
        bool   ended   = false;
        bool   hasFrame = false;
        bool   closing  = false;  // RD-1: logically closed, erase deferred
        // Audio (pl_mpeg path): decoded float PCM queued by the audio
        // callback and drained into the audio backend during update().
        bool   audioEnabled = false;
        int    sampleRate = 0;
        std::vector<float> audioQueue;
        unsigned int audioHandle = 0;
        std::vector<unsigned int> audioHandles;  // all live chunks (stop all on close/seek)
        bool   audioStarted = false;
        float  volume = 1.0f;
        bool   loop = false;
        // Frame-rate pacing: playhead advances by dt each update; frames are
        // decoded until plm_get_time() catches up (bounded per call).
        double frameRate = 0.0;
        double playhead  = 0.0;
        double lastPlmTime = 0.0;  // loop detection: plm time regresses on rewind
        std::shared_ptr<DecodedFrame> m_readyFrame;
        std::shared_ptr<DecodedFrame> m_nextFrame;

#ifdef CAESURA_VIDEO_FFMPEG
        void*  avFormat = nullptr;
        void*  avCodec  = nullptr;
        void*  avFrame  = nullptr;
        void*  avFrameRGB = nullptr;
        void*  swsCtx   = nullptr;
        int    videoStreamIndex = -1;
        // FFmpeg audio: stream index, decoder context, frame and resampler.
        int    audioStreamIndex = -1;
        void*  avAudioCodec = nullptr;
        void*  avAudioFrame = nullptr;
        void*  swrCtx = nullptr;
        // Expected decoded-frame format captured when swr was configured;
        // frames are validated against these (NOT the live codec context,
        // which moves in lockstep with the frame on a mid-stream change).
        int    expectedSampleFmt = -1;
        int    expectedSampleRate = 0;
        std::shared_ptr<void> expectedChLayout; // owned AVChannelLayout copy
        std::vector<uint8_t> rgbaBuffer;
#endif
    };

    VideoHandle openImpl(const char* label, std::shared_ptr<MemoryInput> memory);
    void releaseState(VideoState& state);
    std::shared_ptr<VideoState> find(VideoHandle handle);
    void destroyTexture(VideoState& vs);
    // Drain queued decoded PCM into the audio backend (main thread).
    void drainAudio(VideoState& vs);
    // Flush any videos closed while a decoder worker could still be running
    // (RD-1): close() marks the state and defers the destructive erase to the
    // next updateAll() boundary, where no worker can still be touching it.
    void flushPendingClose();

    std::unordered_map<uint32_t, std::shared_ptr<VideoState>> m_videos;
    std::vector<uint32_t> m_pendingClose;  // ids closed, erase deferred (RD-1)
    uint32_t m_nextId = 1;
    unsigned m_memoryIoRefusals = 0;
    IJobSystem* m_jobSystem = nullptr;
    std::mutex m_audioMutex;  // guards audioQueue across worker/main threads
};

} // namespace Caesura
