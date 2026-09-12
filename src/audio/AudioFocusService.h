#pragma once

#include "api/IAudioFocusService.h"

#include <algorithm>
#include <mutex>
#include <vector>

namespace Caesura {

// Android's JNI source passes AudioManager's integer focus-change code.
// This is the same conversion used by the composition root's native sink.
inline AudioFocusEvent audioFocusEventForAndroidChange(int code) {
    // Permanent, transient and can-duck losses share Android's later gain.
    // Keep the existing pause-on-duck policy, but do not open an independent
    // InterruptionBegin reason that this native source can never close.
    return (code == -1 || code == -2 || code == -3)
        ? AudioFocusEvent::FocusLost : AudioFocusEvent::FocusGained;
}

// Default focus hub: state machine + ordered listener dispatch on the
// posting thread (same contract as LifecycleService). Never touches audio
// itself — consumers decide what to pause.
class AudioFocusService final : public IAudioFocusService {
public:
    AudioFocusService() = default;

    void addListener(IAudioFocusListener* listener) override {
        if (!listener) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (std::find(m_listeners.begin(), m_listeners.end(), listener) != m_listeners.end()) {
            return;
        }
        m_listeners.push_back(listener);
    }

    void removeListener(IAudioFocusListener* listener) override {
        if (!listener) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_listeners.erase(
            std::remove(m_listeners.begin(), m_listeners.end(), listener),
            m_listeners.end());
    }

    void post(AudioFocusEvent event) override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            transitionLocked(event);
        }
        std::vector<IAudioFocusListener*> snapshot;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            snapshot = m_listeners;
        }
        for (IAudioFocusListener* l : snapshot) {
            if (l) l->onAudioFocusEvent(event);
        }
    }

    AudioFocusState currentState() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_state;
    }

private:
    void transitionLocked(AudioFocusEvent event) {
        switch (event) {
            case AudioFocusEvent::FocusLost: m_focusLost = true; break;
            case AudioFocusEvent::FocusGained: m_focusLost = false; break;
            case AudioFocusEvent::InterruptionBegin: m_interrupted = true; break;
            case AudioFocusEvent::InterruptionEnd: m_interrupted = false; break;
        }
        // Ending an interruption does not grant focus, and gaining focus does
        // not end an active interruption. The enum reports the strongest
        // currently active reason while both reasons retain their ownership.
        m_state = m_interrupted ? AudioFocusState::Interrupted
            : m_focusLost ? AudioFocusState::Lost : AudioFocusState::Normal;
    }

    mutable std::mutex m_mutex;
    AudioFocusState m_state = AudioFocusState::Normal;
    bool m_focusLost = false;
    bool m_interrupted = false;
    std::vector<IAudioFocusListener*> m_listeners;
};

} // namespace Caesura
