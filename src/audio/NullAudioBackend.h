#pragma once
#include "../audio/api/IAudioBackend.h"
#include "api/IAudioRestore.h"

namespace Caesura {

// ---------------------------------------------------------------------------
// NullAudioBackend   No-op audio fallback (headless / test / CI)
// ---------------------------------------------------------------------------
// All 24 pure virtual methods return safe defaults: false, 0, or no-ops.
// Used when no actual audio device is available.

class NullAudioBackend : public IAudioBackend, public IAudioRestore {
public:
    NullAudioBackend();
    ~NullAudioBackend() override = default;

    // -- Lifecycle ---------------------------------------------------------
    bool init() override;
    void shutdown() override;
    void update(float deltaTime) override;
    bool isPlaybackAvailable() const override { return false; }
    void suspend() override;
    void resume() override;

    // -- BGM bus -----------------------------------------------------------
    unsigned int playBGM(const std::string& file, float fadeTime = 1.0f) override;
    void stopBGM(float fadeTime = 1.0f) override;

    // -- VOICE bus ---------------------------------------------------------
    unsigned int playVoice(const std::string& file) override;
    void stopVoice() override;

    // -- SE bus (2D + 3D) --------------------------------------------------
    unsigned int playSE(const std::string& file) override;
    unsigned int playRawPCM(const float* samples, unsigned int numFrames,
                            unsigned int sampleRate, unsigned int channels) override;
    unsigned int playSE3D(const std::string& file,
                          float x, float y, float z) override;
    void stopSE() override;
    void setSEVolume(unsigned int handle, float volume) override;
    float getSEVolume(unsigned int handle) override;
    void stopSEHandle(unsigned int handle) override;


    // -- 3D Audio ----------------------------------------------------------
    void update3dListener(float posX, float posY, float posZ,
                          float atX, float atY, float atZ,
                          float upX = 0.0f, float upY = 1.0f,
                          float upZ = 0.0f) override;

    // -- Global volume -----------------------------------------------------
    void setGlobalVolume(float volume) override;
    float getGlobalVolume() const override;

    // -- Per-bus volume ----------------------------------------------------
    void setBusVolume(const char* bus, float volume) override;
    float getBusVolume(const char* bus) const override;

    // -- Wave cache ---------------------------------------------------------
    void flushWaveCache() override;

    // -- State query -------------------------------------------------------
    unsigned int consumeVoiceCompletions() override;
    bool isVoicePlaying() override;
    bool isBGMPlaying() override;
    bool isSEPlaying() override;
    int activeVoiceCount() override;

    // -- Playback position / length / fade (Spec [3.2][3.3]) ---------------
    float getPosition(const char* bus) override;
    float getLength(const char* bus) override;
    void fadeVolume(const char* bus, float targetVolume, float fadeTime) override;

    // -- Backend identification --------------------------------------------
    const char* getBackendName() const override;
    AudioRestoreState captureAudioState() override;
    std::unique_ptr<IPreparedAudioState> prepareAudioState(
        const AudioRestoreState& state, const uint8_t* bytes, size_t size) override;
    bool applyAudioState(std::unique_ptr<IPreparedAudioState> prepared) override;
    void stopSessionAudio() override;
};

} // namespace Caesura
