#pragma once
#include "audio/api/IAudioBackend.h"
#include <array>
#include <cstdint>

namespace Caesura::TestSupport {

struct SoakVoiceAdmission {
    int64_t asyncId = 0;
    std::array<unsigned, 3> handles{};
    bool supported = false;
    bool playing = false;
    uint64_t completionsPending = 0;

    bool accepted() const {
        const bool distinct = handles[0] && handles[1] && handles[2]
            && handles[0] != handles[1] && handles[0] != handles[2]
            && handles[1] != handles[2];
        const bool voiceState = (playing && completionsPending == 0)
            || (!playing && completionsPending == 1);
        return asyncId > 0 && distinct && supported && voiceState;
    }
};

inline SoakVoiceAdmission observeSoakVoiceAdmission(
    IAudioBackend& audio, int64_t asyncId, const std::array<unsigned, 3>& handles) {
    SoakVoiceAdmission observation;
    observation.asyncId = asyncId;
    observation.handles = handles;
    // The production query culls naturally ended handles into the backend's
    // completion queue. Read that queue afterwards; never consume or update it.
    // These are owner-thread observations, not a global mixer-time barrier.
    observation.playing = audio.isVoicePlaying();
    const auto snapshot = audio.getSnapshot();
    observation.supported = snapshot.supported;
    observation.completionsPending = snapshot.voiceCompletionsPending;
    return observation;
}

} // namespace Caesura::TestSupport
