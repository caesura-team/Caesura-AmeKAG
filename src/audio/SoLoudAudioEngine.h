#pragma once
#include "../audio/api/IAudioBackend.h"
#include "api/IAudioRestore.h"
#include <soloud.h>
#include <soloud_bus.h>
#include <soloud_wav.h>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <list>
#include <memory>
#include <vector>

namespace Caesura {

// ---------------------------------------------------------------------------
// SoLoudAudioEngine   Concrete IAudioBackend using SoLoud
// ---------------------------------------------------------------------------
// Three audio buses: BGM, VOICE, SE.
// Owned by the Engine composition root and registered through BackendRegistry.
// Waveform cache uses LRU eviction (spec [10.2.69]) with O(1) touch via
// unordered_map + list.  Cache is a class member, not a global static.

class SoLoudAudioEngine : public IAudioBackend, public IAudioRestore {
public:
    enum class OutputMode { Device, ManualMix };
    // ManualMix uses the actual SoLoud mixer, advanced explicitly by its host.
    explicit SoLoudAudioEngine(OutputMode outputMode = OutputMode::Device)
        : m_outputMode(outputMode) {}
    ~SoLoudAudioEngine() override;

    SoLoudAudioEngine(const SoLoudAudioEngine&) = delete;
    SoLoudAudioEngine& operator=(const SoLoudAudioEngine&) = delete;

    // -- IAudioBackend interface -------------------------------------------
    bool init() override;
    void shutdown() override;
    void update(float deltaTime) override;
    bool isPlaybackAvailable() const override { return m_initialized; }
    void suspend() override;
    void resume() override;

    unsigned int playBGM(const std::string& file, float fadeTime = 1.0f) override;
    void stopBGM(float fadeTime = 1.0f) override;

    unsigned int playVoice(const std::string& file) override;
    void stopVoice() override;

    unsigned int playSE(const std::string& file) override;
    unsigned int playRawPCM(const float* samples, unsigned int numFrames,
                            unsigned int sampleRate, unsigned int channels) override;
    unsigned int playSE3D(const std::string& file, float x, float y, float z) override;
    void stopSE() override;
    void setSEVolume(unsigned int handle, float volume) override;
    float getSEVolume(unsigned int handle) override;
    void stopSEHandle(unsigned int handle) override;

    void update3dListener(float posX, float posY, float posZ,
                          float atX, float atY, float atZ,
                          float upX = 0.0f, float upY = 1.0f,
                          float upZ = 0.0f) override;

    void setGlobalVolume(float volume) override;
    float getGlobalVolume() const override;

    void setBusVolume(const char* bus, float volume) override;
    float getBusVolume(const char* bus) const override;

    void flushWaveCache() override;

    unsigned int consumeVoiceCompletions() override;
    bool isVoicePlaying() override;
    bool isBGMPlaying() override;
    bool isSEPlaying() override;
    int activeVoiceCount() override;

    float getPosition(const char* bus) override;
    float getLength(const char* bus) override;
    void fadeVolume(const char* bus, float targetVolume, float fadeTime) override;

    const char* getBackendName() const override { return "SoLoud"; }

    AudioRestoreState captureAudioState() override;
    std::unique_ptr<IPreparedAudioState> prepareAudioState(
        const AudioRestoreState& state, const uint8_t* bytes, size_t size) override;
    bool applyAudioState(std::unique_ptr<IPreparedAudioState> prepared) override;
    void stopSessionAudio() override;

    // -- Direct SoLoud access (for advanced use) ---------------------------
    SoLoud::Soloud& soloud() { return m_soloud; }
    SoLoud::Bus& bgmBus()    { return m_bgmBus; }
    SoLoud::Bus& voiceBus()  { return m_voiceBus; }
    SoLoud::Bus& seBus()     { return m_seBus; }

private:
    // -- Internal helpers -------------------------------------------------
    std::shared_ptr<SoLoud::AudioSource> loadWave(const std::string& file);
    void cullFinishedHandles();
    uint64_t bgmSampleCount() const;
    void releaseAudioHandles(std::size_t count);
    void retireHandle(SoLoud::handle handle, float fadeTime,
                      std::vector<SoLoud::handle>& retiringHandles);
    void stopRetiringHandles(std::vector<SoLoud::handle>& retiringHandles);

    const OutputMode m_outputMode;
    SoLoud::Soloud m_soloud;
    SoLoud::Bus    m_bgmBus;
    SoLoud::Bus    m_voiceBus;
    SoLoud::Bus    m_seBus;
    // Raw-PCM Wavs kept alive while their voice plays (playRawPCM).
    std::unordered_map<unsigned int, std::shared_ptr<SoLoud::Wav>> m_rawWaveCache;
    SoLoud::handle m_bgmBusHandle   = 0;
    SoLoud::handle m_voiceBusHandle = 0;
    SoLoud::handle m_seBusHandle    = 0;

    static constexpr unsigned int kVoicePoolSize = 4;  // VN voice pool
    unsigned int m_voicePool[kVoicePoolSize] = { 0 };  // round-robin voice slots
    unsigned int m_voiceSlot    = 0;
    bool         m_bgmDucked    = false;  // ducking state (voice lowers BGM)
    unsigned int m_currentBGM   = 0;
    std::string m_currentBGMPath;
    std::shared_ptr<SoLoud::AudioSource> m_restoredBGMSource;
    unsigned int m_restoredBGMHandle = 0;
    unsigned int m_voiceCompletionsPending = 0;
    std::vector<SoLoud::handle> m_retiringBGM;
    std::vector<SoLoud::handle> m_retiringVoice;

    // SE handles: tracked per-play for mass-stop via stopSE()
    std::vector<SoLoud::handle> m_activeSE;

    // Waveform LRU cache (spec [3.1], [10.2.69]) -- class member, not global static
    std::unordered_map<std::string, std::shared_ptr<SoLoud::AudioSource>> m_waveCache;
    std::list<std::string> m_waveLRU;
    std::unordered_map<std::string, std::list<std::string>::iterator> m_waveLRUMap;

    bool m_initialized = false;
    float m_globalVolume = 1.0f;

    // Per-bus volume state for persistence
    float m_bgmVolume   = 1.0f;
    float m_voiceVolume = 1.0f;
    float m_seVolume    = 1.0f;
};

} // namespace Caesura
