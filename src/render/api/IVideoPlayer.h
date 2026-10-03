#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace Caesura {

// -- VideoHandle -----------------------------------------------------------
struct VideoHandle {
    uint32_t id = 0;
    explicit operator bool() const { return id != 0; }
    bool operator==(const VideoHandle& o) const { return id == o.id; }
};

// ============================================================================
// IVideoPlayer — pure virtual interface for video playback
// ============================================================================
// VideoPlayer implements this interface. BackendRegistry stores IVideoPlayer*
// so it does not depend on the concrete implementation.

class IVideoPlayer {
public:
    virtual ~IVideoPlayer() = default;

    virtual VideoHandle open(const char* path) = 0;
    // Owned, bounded asset bytes only: no filename/URL fallback or secondary I/O.
    virtual VideoHandle openMemory(std::vector<uint8_t> bytes) = 0;
    virtual void close(VideoHandle handle) = 0;
    // Stop every video immediately, retaining the backend for subsequent open().
    // Physical decoder/GPU release follows close() at the updateAll() boundary.
    virtual void closeAll() = 0;
    // Loop mode (pl_mpeg: plm_set_loop; FFmpeg: rewind on end).
    virtual void setLoop(VideoHandle handle, bool loop) = 0;
    // Playback volume [0..1.5] applied to the video audio handles.
    virtual void setVolume(VideoHandle handle, float volume) = 0;
    virtual bool update(VideoHandle handle, double dt) = 0;
    // Advance every playing video by dt (called by the engine frame loop).
    virtual void updateAll(double dt) = 0;
    // Returns raw bgfx TextureHandle.idx, or 0 if no frame available.
    virtual uint32_t getTexture(VideoHandle handle) const = 0;

    virtual bool isPlaying(VideoHandle handle) const = 0;
    virtual bool hasEnded(VideoHandle handle) const = 0;
    virtual int  width(VideoHandle handle) const = 0;
    virtual int  height(VideoHandle handle) const = 0;
    virtual double duration(VideoHandle handle) const = 0;
    virtual double currentTime(VideoHandle handle) const = 0;

    virtual void pause(VideoHandle handle) = 0;
    virtual void resume(VideoHandle handle) = 0;
    virtual void seek(VideoHandle handle, double time) = 0;

    virtual void shutdown() = 0;
    virtual int  activeCount() const = 0;
};

} // namespace Caesura
