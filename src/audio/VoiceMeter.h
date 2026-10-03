#pragma once

#include <soloud.h>
#include <cstdint>

namespace Caesura {

// Audio-private observer. All access while the mixer lives uses its audio
// mutex; after deinit/join the owner may access it directly. The callback
// never allocates, locks, logs, or touches another subsystem.
class VoiceMeter final : public SoLoud::Filter {
public:
    struct State {
        double rawRms = 0;
        bool valid = false;
        bool available = false;
        uint64_t generation = 0;
        uint64_t sampledGeneration = 0;
        uint64_t blockSerial = 0;
        uint64_t invalidBlocks = 0;
        uint64_t instancesCreated = 0;
        uint64_t instancesDestroyed = 0;
        unsigned installations = 0;
    };

    SoLoud::FilterInstance* createInstance() override;
    void invalidate() noexcept; // Owner only: obtain a process-unique epoch.
    void observe(const float* buffer, unsigned samples, unsigned stride,
                 unsigned channels) noexcept;
    State state;

private:
    class Instance;
};

// Do not call lock-taking public SoLoud APIs within this scope.
class AudioMutexLock final {
public:
    explicit AudioMutexLock(SoLoud::Soloud& soloud) : m_soloud(soloud) {
        m_soloud.lockAudioMutex_internal();
    }
    ~AudioMutexLock() { m_soloud.unlockAudioMutex_internal(); }
    AudioMutexLock(const AudioMutexLock&) = delete;
    AudioMutexLock& operator=(const AudioMutexLock&) = delete;
private:
    SoLoud::Soloud& m_soloud;
};

} // namespace Caesura
