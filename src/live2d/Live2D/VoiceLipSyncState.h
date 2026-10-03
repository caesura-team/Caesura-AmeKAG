#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Caesura::Detail {

// Model-owned state only. The caller obtains the current model's real Core
// parameter reference on every call; no model/audio pointers survive a call.
// Kept independent of the SDK so the actual envelope/lifecycle logic can be
// tested without constructing GPU resources or a substitute Cubism model.
class VoiceLipSyncState {
public:
    bool enabled() const { return m_enabled; }
    int parameterIndex() const { return m_parameterIndex; }

    // parameterIndex must come from the real Core parameter array, not
    // CubismModel::GetParameterIndex (which can create a virtual parameter).
    bool enable(int parameterIndex, float minimum, float maximum, float& mouth) {
        if (parameterIndex < 0 || !std::isfinite(minimum) ||
            !std::isfinite(maximum) || minimum > 0.0f || maximum < 1.0f) {
            return false;
        }
        if (m_enabled) return m_parameterIndex == parameterIndex;
        m_enabled = true;
        m_parameterIndex = parameterIndex;
        m_envelope = 0.0f;
        m_lastSeenGeneration = 0;
        mouth = 0.0f;
        return true;
    }

    void disable(float& mouth) {
        if (!m_enabled) return;
        mouth = 0.0f;
        *this = VoiceLipSyncState{};
    }

    void hide(float& mouth) {
        if (!m_enabled) return;
        m_envelope = 0.0f;
        mouth = 0.0f;
    }

    // hasCurrentSample is the conjunction of the public supported, playing
    // and sampled flags. Same-generation silent PCM is a valid release input.
    // Call after motion/expression/pose and before CubismModel::Update.
    void apply(bool hasCurrentSample, float rms, uint64_t generation,
               float dt, float& mouth) {
        if (!m_enabled) return;
        if (generation != m_lastSeenGeneration) {
            m_envelope = 0.0f;
            mouth = 0.0f;
            m_lastSeenGeneration = generation;
        }
        if (!hasCurrentSample || generation == 0 || !std::isfinite(rms) ||
            rms < 0.0f || rms > 1.0f) {
            m_envelope = 0.0f;
            mouth = 0.0f;
            return;
        }

        // A frozen/invalid frame cannot advance smoothing, but cannot prevent
        // the generation or unavailable-input resets above either.
        dt = std::isfinite(dt) ? std::clamp(dt, 0.0f, 0.25f) : 0.0f;
        const float target = std::clamp((rms - 0.01f) / 0.24f, 0.0f, 1.0f);
        const float tau = target > m_envelope ? 0.03f : 0.08f;
        m_envelope += (target - m_envelope) * -std::expm1(-dt / tau);
        m_envelope = std::clamp(m_envelope, 0.0f, 1.0f);
        mouth = m_envelope;
    }

private:
    bool m_enabled = false;
    int m_parameterIndex = -1;
    float m_envelope = 0.0f;
    uint64_t m_lastSeenGeneration = 0;
};

} // namespace Caesura::Detail
