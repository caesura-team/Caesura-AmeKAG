#include "SoLoudAudioEngine.h"
#include "NullAudioBackend.h"
#include "../di/BackendRegistry.h"
#include "../di/api/ThreadAssert.h"
#include <soloud_wavstream.h>
#include <soloud_wav.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Caesura {
namespace {
constexpr uint64_t kSamplePhase = uint64_t{1} << 20;
class PreparedAudio final : public IPreparedAudioState {
public:
    PreparedAudio(AudioRestoreState state, std::shared_ptr<SoLoud::AudioSource> source = {})
        : state(std::move(state)), source(std::move(source)) {}
    const AudioRestoreState& description() const override { return state; }
    const AudioRestoreState state;
    const std::shared_ptr<SoLoud::AudioSource> source;
    std::unique_ptr<SoLoud::AudioSourceInstance> instance;
    unsigned int preRollFrames = 0;
    unsigned int initialFraction = 0;
};

bool validState(const AudioRestoreState& state) {
    if (!std::isfinite(state.position) || state.position < 0 || state.position > 86400
        || !std::isfinite(state.gain) || state.gain < 0 || state.gain > 16) return false;
    if (state.frameFraction >= kSamplePhase || state.framePosition > (UINT64_MAX >> 20)) return false;
    if (!state.sourceRate && (state.framePosition || state.frameFraction || state.outputRate)) return false;
    if (state.sourceRate && !state.outputRate) return false;
    if (state.bgmPath.empty()) return state.position == 0 && state.gain == 1 && !state.looping && !state.sourceRate;
    if (state.bgmPath.size() > 4096 || state.bgmPath[0] == '/'
        || state.bgmPath.find("..") != std::string::npos) return false;
    for (const auto ch : state.bgmPath) {
        if (static_cast<unsigned char>(ch) < 32 || ch == '\\' || ch == ':') return false;
    }
    return true;
}

bool completeWaveHeader(const uint8_t* bytes, size_t size) {
    if (size < 4 || std::memcmp(bytes, "RIFF", 4) != 0) return true;
    if (size < 12 || std::memcmp(bytes + 8, "WAVE", 4) != 0) return false;
    const auto readSize = [&](size_t offset) -> uint64_t {
        return uint32_t(bytes[offset]) | (uint32_t(bytes[offset+1]) << 8)
            | (uint32_t(bytes[offset+2]) << 16) | (uint32_t(bytes[offset+3]) << 24);
    };
    const uint64_t end = readSize(4) + 8;
    if (end > size || end < 12) return false;
    uint64_t offset = 12;
    bool format = false, data = false;
    while (offset < end) {
        if (end - offset < 8) return false;
        const uint64_t length = readSize(static_cast<size_t>(offset + 4));
        if (length + (length & 1) > end - offset - 8) return false;
        if (std::memcmp(bytes + offset, "fmt ", 4) == 0) format = length >= 16;
        if (std::memcmp(bytes + offset, "data", 4) == 0) data = length > 0;
        offset += 8 + length + (length & 1);
    }
    return format && data;
}
}

uint64_t SoLoudAudioEngine::bgmSampleCount() const {
    auto source = m_restoredBGMHandle == m_currentBGM ? m_restoredBGMSource : nullptr;
    if (!source) {
        const auto found = m_waveCache.find(m_currentBGMPath);
        if (found != m_waveCache.end()) source = found->second;
    }
    if (const auto* stream = dynamic_cast<const SoLoud::WavStream*>(source.get())) return stream->mSampleCount;
    if (const auto* wave = dynamic_cast<const SoLoud::Wav*>(source.get())) return wave->mSampleCount;
    return 0;
}

AudioRestoreState SoLoudAudioEngine::captureAudioState() {
    CAESURA_ASSERT_MAIN_THREAD();
    if (!m_initialized || !isBGMPlaying()) return {};
    SoLoud::VoiceFrameCursor cursor{};
    if (!m_soloud.getVoiceFrameCursor(m_currentBGM, cursor))
        throw std::runtime_error("BGM has no exact resumable sample clock");
    const uint64_t length = bgmSampleCount();
    if (!length || !cursor.sourceRate) throw std::runtime_error("BGM sample range unavailable");
    const auto frame = cursor.looping ? cursor.frame % length : cursor.frame;
    const double position = (double(frame) + double(cursor.fraction) / kSamplePhase) / cursor.sourceRate;
    AudioRestoreState value{m_currentBGMPath,
        cursor.looping ? position : (std::min)(position, double(length) / cursor.sourceRate),
        cursor.volume, cursor.looping};
    value.framePosition = cursor.frame;
    value.frameFraction = cursor.fraction;
    value.sourceRate = cursor.sourceRate;
    value.outputRate = cursor.outputRate;
    return value;
}

std::unique_ptr<IPreparedAudioState> SoLoudAudioEngine::prepareAudioState(
    const AudioRestoreState& state, const uint8_t* bytes, size_t size) {
    CAESURA_ASSERT_MAIN_THREAD();
    if (!validState(state)) return {};
    try {
        if (state.bgmPath.empty()) return size == 0 ? std::make_unique<PreparedAudio>(state) : nullptr;
        if (!bytes || size == 0 || size > 64 * 1024 * 1024 || !completeWaveHeader(bytes, size)) return {};
        auto stream = std::make_shared<SoLoud::WavStream>();
        // The stream owns a copy; commit never rereads a mutable file or cache.
        if (stream->loadMem(bytes, static_cast<unsigned int>(size), true, false) != SoLoud::SO_NO_ERROR)
            return {};
        if (!(stream->mBaseSamplerate >= 1) || double(stream->mBaseSamplerate) > UINT32_MAX) return {};
        // WavStream::getLength currently divides in float before returning
        // double, which can reject a valid fractional frame just before EOF.
        const double length = double(stream->mSampleCount) / double(stream->mBaseSamplerate);
        if (!std::isfinite(length) || length <= 0 || length > 86400
            || (state.sourceRate ? state.position > length : state.position >= length)) return {};
        const auto sourceRate = static_cast<uint32_t>(stream->mBaseSamplerate);
        const auto outputRate = m_initialized ? m_soloud.getBackendSamplerate()
            : (state.outputRate ? state.outputRate : 48000);
        if (!sourceRate || !outputRate || !stream->mSampleCount
            || (state.sourceRate && state.sourceRate != sourceRate)) return {};
        const float ratio = float(sourceRate) / float(outputRate);
        if (!(ratio > 0) || ratio >= 4096) return {};
        const uint64_t step = static_cast<uint64_t>(std::floor(ratio * kSamplePhase));
        if (!step) return {};
        const uint64_t history = 2 + (kSamplePhase + step - 1) / step;
        if (history > 4096) return {};
        AudioRestoreState target = state;
        target.sourceRate = sourceRate;
        target.outputRate = outputRate;
        if (!state.sourceRate) {
            target.framePosition = static_cast<uint64_t>(std::floor(state.position * sourceRate));
            target.frameFraction = 0;
        }
        const uint64_t fixed = target.framePosition * kSamplePhase + target.frameFraction;
        // New exact cursors include the finite linear source/bus tail after
        // decoder EOF. Legacy seconds-only snapshots keep their old range.
        const uint64_t tailEnd = (uint64_t(stream->mSampleCount) + 1) * kSamplePhase + step;
        if (!target.looping && fixed >= tailEnd) return {};
        const unsigned preRoll = static_cast<unsigned>((std::min)(history, fixed / step));
        const uint64_t anchor = fixed - uint64_t(preRoll) * step;
        auto result = std::make_unique<PreparedAudio>(target, stream);
        result->instance.reset(stream->createInstance());
        if (!result->instance) return {};
        result->instance->init(*stream, 0);
        result->instance->mFlags |= SoLoud::AudioSourceInstance::DISABLE_AUTOSTOP;
        if (target.looping) result->instance->mFlags |= SoLoud::AudioSourceInstance::LOOPING;
        if (result->instance->seekFrame((anchor >> 20) % stream->mSampleCount) != SoLoud::SO_NO_ERROR) return {};
        result->preRollFrames = preRoll;
        result->initialFraction = static_cast<unsigned>(anchor & (kSamplePhase - 1));
        return result;
    } catch (...) {
        return {};
    }
}

bool SoLoudAudioEngine::applyAudioState(std::unique_ptr<IPreparedAudioState> prepared) {
    CAESURA_ASSERT_MAIN_THREAD();
    stopSessionAudio();
    auto* value = dynamic_cast<PreparedAudio*>(prepared.get());
    if (!m_initialized || !value) return false;
    if (value->state.bgmPath.empty()) return true;
    if (!value->source || !value->instance || value->state.outputRate != m_soloud.getBackendSamplerate()) return false;
    bool allocated = false;
    auto& registry = BackendRegistry::instance();
    try {
        std::string path = value->state.bgmPath;
        allocated = registry.tryAlloc("audio_handles");
        if (!allocated) return false;
        const auto handle = m_soloud.playPrepared(*value->source, value->instance.release(),
            value->state.gain, 0.0f, true, m_bgmBusHandle);
        if (!handle || !m_soloud.isValidVoiceHandle(handle)) {
            registry.release("audio_handles");
            return false;
        }
        m_currentBGM = handle;
        m_currentBGMPath.swap(path);
        m_restoredBGMHandle = handle;
        m_restoredBGMSource = value->source;
        m_soloud.setLooping(handle, value->state.looping);
        const SoLoud::VoiceFrameCursor target{value->state.framePosition, value->state.frameFraction,
            value->state.sourceRate, value->state.outputRate, value->state.gain, value->state.looping};
        if (m_soloud.primeRestoredVoice(handle, m_bgmBus, value->preRollFrames,
                value->initialFraction, target) != SoLoud::SO_NO_ERROR) {
            stopSessionAudio();
            return false;
        }
        return true;
    } catch (...) {
        if (allocated && !m_currentBGM) registry.release("audio_handles");
        stopSessionAudio();
        return false;
    }
}

void SoLoudAudioEngine::stopSessionAudio() {
    CAESURA_ASSERT_MAIN_THREAD();
    if (!m_initialized) return;
    stopRetiringHandles(m_retiringBGM);
    stopRetiringHandles(m_retiringVoice);
    std::size_t released = m_activeSE.size() + (m_currentBGM != 0 ? 1 : 0);
    if (m_currentBGM) m_soloud.stop(m_currentBGM);
    for (auto& handle : m_voicePool) {
        if (handle) { m_soloud.stop(handle); ++released; handle = 0; }
    }
    for (const auto handle : m_activeSE) m_soloud.stop(handle);
    m_activeSE.clear();
    m_rawWaveCache.clear();
    m_currentBGM = 0;
    m_currentBGMPath.clear();
    m_restoredBGMHandle = 0;
    m_restoredBGMSource.reset();
    m_voiceSlot = 0;
    m_voiceCompletionsPending = 0;
    m_bgmDucked = false;
    m_soloud.setVolume(m_bgmBusHandle, m_bgmVolume);
    m_soloud.clearResamplerBuffers(m_bgmBusHandle);
    m_soloud.clearResamplerBuffers(m_voiceBusHandle);
    m_soloud.clearResamplerBuffers(m_seBusHandle);
    invalidateVoiceMeter();
    releaseAudioHandles(released);
}

AudioRestoreState NullAudioBackend::captureAudioState() { return {}; }
std::unique_ptr<IPreparedAudioState> NullAudioBackend::prepareAudioState(
    const AudioRestoreState& state, const uint8_t*, size_t size) {
    if (!validState(state) || !state.bgmPath.empty() || size != 0) return {};
    return std::make_unique<PreparedAudio>(state);
}
bool NullAudioBackend::applyAudioState(std::unique_ptr<IPreparedAudioState> prepared) {
    const auto* value = dynamic_cast<const PreparedAudio*>(prepared.get());
    return value && value->state.bgmPath.empty() && !value->source;
}
void NullAudioBackend::stopSessionAudio() {}
}
