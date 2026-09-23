#pragma once
#include <cstdint>
#include <string>

namespace Caesura {

// Selected output path, not evidence that a physical device is audible.
enum class AudioOutputMode { Unknown, Device, ManualMix, Software };

struct AudioBackendSnapshot {
    bool supported = false;
    bool running = false; // An initialized playback implementation.
    AudioOutputMode outputMode = AudioOutputMode::Unknown;
    // Actual non-bus mixer voices, including paused/virtual/retiring voices.
    uint64_t liveVoices = 0;
    uint64_t busVoices = 0;
    // Owner records still held, including invalid handles awaiting culling:
    // current BGM + occupied voice slots + SE + both retiring collections.
    uint64_t sessionHandles = 0;
    uint64_t retiringBGM = 0;   // Subset of sessionHandles.
    uint64_t retiringVoice = 0; // Subset of sessionHandles.
    uint64_t waveCacheEntries = 0; // Reusable sources may remain at idle.
    uint64_t rawCacheEntries = 0;  // Session PCM owners, not reusable cache.
    uint64_t voiceCompletionsPending = 0;
    // Restored source outside the wave cache; its handle aliases a session
    // record and must not be counted as another voice or quota allocation.
    uint64_t restoredSources = 0;
};

// ---------------------------------------------------------------------------
// IAudioBackend   Abstract audio backend interface
// ---------------------------------------------------------------------------
// Concrete implementations: SoLoudAudioEngine, (future) OpenAL, FMOD, etc.
// All Lua-side audio calls dispatch through this interface.
// Backend instances are managed by BackendRegistry and accessed via handles.

class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;

    // -- Lifecycle ---------------------------------------------------------
    virtual bool init() = 0;
    virtual void shutdown() = 0;
    virtual void update(float deltaTime) = 0;

    // Owner-thread session query; do not race init/shutdown. True means an
    // initialized playback implementation (including a host-driven mixer),
    // not proof that a particular asset plays or a physical device is audible.
    // Silent fallbacks return false even when their init() succeeds.
    virtual bool isPlaybackAvailable() const = 0;

    // Owner/main thread only; do not race init/shutdown. Observation must not
    // cull handles, consume completions, mix/update, flush, or release quotas.
    // Counts are meaningful only when supported. Owner records and mixer
    // voices describe different phases and must not be summed as resources.
    // Device playback may finish between the mixer's individually locked
    // reads; this is not a globally atomic or global-quiescence assertion.
    virtual AudioBackendSnapshot getSnapshot() = 0;

    // -- App-lifecycle audio suspend/resume (mobile backgrounding) ----------
    // Suspends all playback (mixer paused) without releasing loaded assets;
    // resume() continues from the suspended position. Used by the engine's
    // SDL app-lifecycle watcher (WILL_ENTER_BACKGROUND/DID_ENTER_FOREGROUND).
    virtual void suspend() = 0;
    virtual void resume() = 0;

    // -- BGM bus: background music with cross-fade support -----------------
    virtual unsigned int playBGM(const std::string& file, float fadeTime = 1.0f) = 0;
    virtual void stopBGM(float fadeTime = 1.0f) = 0;

    // -- VOICE bus: voice playback and explicit session interruption ------
    virtual unsigned int playVoice(const std::string& file) = 0;
    virtual void stopVoice() = 0;

    // -- SE bus: sound effects (2D and 3D spatial) ------------------------
    virtual unsigned int playSE(const std::string& file) = 0;

    // -- Raw PCM playback (video audio etc.) ------------------------------
    // Plays interleaved float PCM [-1,1] on the SE bus; the engine copies the
    // samples, so the caller may free them after the call. Returns a handle
    // usable with stopSEHandle/setSEVolume, or 0 on failure.
    virtual unsigned int playRawPCM(const float* samples, unsigned int numFrames,
                                    unsigned int sampleRate, unsigned int channels) = 0;
    virtual unsigned int playSE3D(const std::string& file,
                                   float x, float y, float z) = 0;
    virtual void stopSE() = 0;
    // [10.2.27] Per-SE-handle volume control
    virtual void setSEVolume(unsigned int handle, float volume) = 0;
    virtual float getSEVolume(unsigned int handle) = 0;
    virtual void stopSEHandle(unsigned int handle) = 0;


    // -- 3D Audio ----------------------------------------------------------
    virtual void update3dListener(float posX, float posY, float posZ,
                                   float atX, float atY, float atZ,
                                   float upX = 0.0f, float upY = 1.0f,
                                   float upZ = 0.0f) = 0;

    // -- Global volume -----------------------------------------------------
    virtual void setGlobalVolume(float volume) = 0;
    virtual float getGlobalVolume() const = 0;

    // -- Per-bus volume ----------------------------------------------------
    virtual void setBusVolume(const char* bus, float volume) = 0;
    virtual float getBusVolume(const char* bus) const = 0;

    // -- Wave cache ---------------------------------------------------------
    virtual void flushWaveCache() = 0;

    // -- State query -------------------------------------------------------
    // Returns and clears the number of voice lines that ended naturally
    // since the previous call. Explicit stop/replacement does not count.
    virtual unsigned int consumeVoiceCompletions() = 0;
    virtual bool isVoicePlaying() = 0;
    virtual bool isBGMPlaying() = 0;
    virtual bool isSEPlaying() = 0;
    virtual int activeVoiceCount() = 0;

    // -- Playback position / length / fade (Spec [3.2][3.3]) -----------
    virtual float getPosition(const char* bus) = 0;
    virtual float getLength(const char* bus) = 0;
    virtual void fadeVolume(const char* bus, float targetVolume, float fadeTime) = 0;


    // -- Backend identification --------------------------------------------
    virtual const char* getBackendName() const = 0;
};

} // namespace Caesura
