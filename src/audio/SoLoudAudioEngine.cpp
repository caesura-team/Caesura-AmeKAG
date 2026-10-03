#include "SoLoudAudioEngine.h"
#include "VoiceMeter.h"
#include "di/api/ThreadAssert.h"
#include "di/BackendRegistry.h"
#include "../debug/api/DebugLog.h"
#include <soloud_wav.h>
#include <soloud_wavstream.h>
#include <cstdio>
#include <unordered_map>
#include <memory>
#include <algorithm>
#include <list>
#include <limits>
#include <array>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
namespace Caesura {

static bool validPlaybackOptions(const AudioPlaybackOptions& options) {
    return std::isfinite(options.volume) && options.volume >= 0 && options.volume <= 1.5f
        && std::isfinite(options.fadeIn) && options.fadeIn >= 0;
}

static bool hasPlaybackCapacity(SoLoud::Soloud& mixer) {
    AudioMutexLock lock(mixer);
    for (unsigned i = 0; i < VOICE_COUNT; ++i) {
        const auto* voice = mixer.mVoice[i];
        if (!voice || !(voice->mFlags & SoLoud::AudioSourceInstance::PROTECTED)) return true;
    }
    return false;
}

static bool ownsBusVoice(SoLoud::Soloud& mixer, SoLoud::handle handle,
                        SoLoud::handle bus, const SoLoud::AudioSource& source) {
    AudioMutexLock lock(mixer);
    const int index = mixer.getVoiceFromHandle_internal(handle);
    return index >= 0 && mixer.mVoice[index]->mBusHandle == bus
        && mixer.mVoice[index]->mAudioSourceID == source.mAudioSourceID;
}


// Detect extension and use WavStream for .ogg/.mp3, Wav for .wav
static bool isStreamFormat(const std::string& file) {
    size_t dot = file.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = file.substr(dot);
    // case-insensitive compare
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    return ext == ".ogg" || ext == ".mp3" || ext == ".flac";
}

std::shared_ptr<SoLoud::AudioSource> SoLoudAudioEngine::loadWave(const std::string& file) {
    auto it = m_waveCache.find(file);
    if (it != m_waveCache.end()) {
        auto mapIt = m_waveLRUMap.find(file);
        if (mapIt != m_waveLRUMap.end()) m_waveLRU.splice(m_waveLRU.begin(), m_waveLRU, mapIt->second);
        return it->second;
    }

    std::shared_ptr<SoLoud::AudioSource> src;

    if (isStreamFormat(file)) {
        auto stream = std::make_shared<SoLoud::WavStream>();
        if (stream->load(file.c_str()) != SoLoud::SO_NO_ERROR) {
            DEBUG_ERR(SubSys::Audio, ErrCode::Audio_FileLoadFailed, "[Audio] Failed to load stream: %s", file.c_str());
            return nullptr;
        }
        src = stream;
    } else {
        auto wav = std::make_shared<SoLoud::Wav>();
        if (wav->load(file.c_str()) != SoLoud::SO_NO_ERROR) {
            DEBUG_ERR(SubSys::Audio, ErrCode::Audio_FileLoadFailed, "[Audio] Failed to load: %s", file.c_str());
            return nullptr;
        }
        src = wav;
    }

    // LRU eviction: remove least recently used when >= 128 entries, but never
    // evict a source that SoLoud is still playing -- play() holds a raw
    // pointer into the shared_ptr, so deleting it would be a use-after-free
    // in the audio thread. Active sources are re-queued to the LRU tail
    // instead (they become evictable once playback finishes).
    if (m_waveCache.size() >= 128 && !m_waveLRU.empty()) {
        for (int attempts = 0; attempts < 4 && m_waveCache.size() >= 128
             && !m_waveLRU.empty(); ++attempts) {
            std::string lruFile = m_waveLRU.back();
            m_waveLRU.pop_back();
            m_waveLRUMap.erase(lruFile);
            auto it = m_waveCache.find(lruFile);
            if (it == m_waveCache.end()) continue;
            if (m_soloud.countAudioSource(*it->second) > 0) {
                // Still playing: keep it, requeue at the front (fresh LRU).
                m_waveCache[lruFile] = it->second;
                m_waveLRU.push_front(lruFile);
                m_waveLRUMap[lruFile] = m_waveLRU.begin();
                continue;
            }
            m_waveCache.erase(it);
            DEBUG_ERR(SubSys::Audio, ErrCode::Ok, "[Audio] Wave cache LRU evicted: %s", lruFile.c_str());
            break;
        }
    }
    m_waveCache[file] = src;
    m_waveLRU.push_front(file);
    m_waveLRUMap[file] = m_waveLRU.begin();
    return src;
}

// -- Lifecycle -------------------------------------------------------------

SoLoudAudioEngine::SoLoudAudioEngine(OutputMode outputMode)
    : m_outputMode(outputMode), m_voiceMeter(std::make_unique<VoiceMeter>()) {
    // Bus::setFilter dereferences an existing BusInstance. Install exactly
    // once here, before any instance exists; never reinstall during restart.
    m_voiceBus.setFilter(0, m_voiceMeter.get());
    ++m_voiceMeter->state.installations;
}

SoLoudAudioEngine::~SoLoudAudioEngine() {
    shutdown();
}

AudioBackendSnapshot SoLoudAudioEngine::getSnapshot() {
    CAESURA_ASSERT_MAIN_THREAD();
    AudioBackendSnapshot snapshot;
    snapshot.supported = true;
    snapshot.running = m_initialized;
    switch (m_outputMode) {
    case OutputMode::Device: snapshot.outputMode = AudioOutputMode::Device; break;
    case OutputMode::ManualMix: snapshot.outputMode = AudioOutputMode::ManualMix; break;
    case OutputMode::Software: snapshot.outputMode = AudioOutputMode::Software; break;
    }

    // Preserve owner records even after their mixer voices have finished.
    // Observation must not collect that debt or consume pending notifications.
    snapshot.retiringBGM = m_retiringBGM.size();
    snapshot.retiringVoice = m_retiringVoice.size();
    snapshot.sessionHandles = (m_currentBGM != 0 ? 1u : 0u)
        + m_activeSE.size() + snapshot.retiringBGM + snapshot.retiringVoice;
    for (const auto handle : m_voicePool)
        snapshot.sessionHandles += (handle != 0 ? 1u : 0u);
    snapshot.waveCacheEntries = m_waveCache.size();
    snapshot.rawCacheEntries = m_rawWaveCache.size();
    snapshot.voiceCompletionsPending = m_voiceCompletionsPending;
    snapshot.restoredSources = m_restoredBGMSource ? 1u : 0u;

    if (m_initialized) {
        // These getter calls only read under the mixer lock. Unlike
        // getActiveVoiceCount(), getVoiceCount() does not recalculate voices.
        const SoLoud::handle buses[] = {
            m_bgmBusHandle, m_voiceBusHandle, m_seBusHandle
        };
        for (const auto handle : buses)
            if (m_soloud.isValidVoiceHandle(handle)) ++snapshot.busVoices;
        // The protected buses cannot naturally end, and owner lifecycle must
        // not race this call. Read the total after observing those buses;
        // device playback may finish non-bus voices between these reads.
        snapshot.liveVoices = m_soloud.getVoiceCount() - snapshot.busVoices;
    }
    return snapshot;
}

VoiceLevelSnapshot SoLoudAudioEngine::getVoiceLevel() {
    CAESURA_ASSERT_MAIN_THREAD();
    VoiceLevelSnapshot snapshot;
    if (!m_initialized) {
        snapshot.generation = m_voiceMeter->state.generation;
        return snapshot;
    }
    AudioMutexLock lock(m_soloud);
    const auto& meter = m_voiceMeter->state;
    snapshot.generation = meter.generation;
    snapshot.supported = meter.available;
    for (const auto handle : m_voicePool)
        snapshot.playing |= m_soloud.getVoiceFromHandle_internal(handle) >= 0;
    const int bus = m_soloud.getVoiceFromHandle_internal(m_voiceBusHandle);
    if (!snapshot.supported || !snapshot.playing || m_softwareSuspended || bus < 0)
        return snapshot;
    const float gain = m_soloud.mVoice[bus]->mSetVolume;
    if (!std::isfinite(gain) || gain <= 0 ||
        !std::isfinite(m_globalVolume) || m_globalVolume <= 0 ||
        !meter.valid || meter.sampledGeneration != meter.generation)
        return snapshot;
    const double effective = meter.rawRms * double(gain) * m_globalVolume;
    if (!std::isfinite(effective)) return snapshot;
    snapshot.sampled = true;
    snapshot.rms = static_cast<float>(std::clamp(effective, 0.0, 1.0));
    return snapshot;
}

void SoLoudAudioEngine::invalidateVoiceMeter() {
    if (m_initialized) {
        AudioMutexLock lock(m_soloud);
        m_voiceMeter->invalidate();
    } else {
        m_voiceMeter->invalidate(); // No mixer, or after deinit has joined.
    }
}

void SoLoudAudioEngine::observeVoiceMuteLocked() {
    const int bus = m_soloud.getVoiceFromHandle_internal(m_voiceBusHandle);
    const bool muted = bus < 0 || !std::isfinite(m_soloud.mVoice[bus]->mSetVolume)
        || m_soloud.mVoice[bus]->mSetVolume <= 0;
    // Recovery was invalidated before scheduling the fade. A skipped filter
    // while its first gain is still zero is not another owner boundary.
    if (m_voiceFadeRecovering) {
        if (!muted) { m_voiceFadeRecovering = false; m_voiceGainMuted = false; }
        return;
    }
    if (muted != m_voiceGainMuted) {
        m_voiceMeter->invalidate();
        m_voiceGainMuted = muted;
    }
}

SoLoud::handle SoLoudAudioEngine::playBus(SoLoud::Bus& bus) {
    // Match Soloud::play: source creation is outside its audio mutex, then
    // playPrepared takes ownership, including its null-instance failure path.
    bus.mSoloud = &m_soloud;
    auto* instance = m_createBusInstance ? m_createBusInstance(bus) : bus.createInstance();
    return m_soloud.playPrepared(bus, instance, -1.0f, 0.0f, false, 0);
}

void SoLoudAudioEngine::rollbackInit() noexcept {
    // This path is required even when VOICE exists but m_initialized is false.
    m_soloud.stopAll();
    m_soloud.deinit(); // Backend teardown/join precedes direct state access.
    m_voiceMeter->invalidate();
    m_bgmBusHandle = m_voiceBusHandle = m_seBusHandle = 0;
    m_initialized = false;
    m_softwareSuspended = false;
    m_voiceFadeRecovering = false;
}

bool SoLoudAudioEngine::init(){
    CAESURA_ASSERT_MAIN_THREAD();
    if (m_initialized) return true;
    m_voiceMeter->invalidate();
    try {
    m_voiceCompletionsPending = 0;
    m_softwareMixStats = {};
    m_softwareFractionalFrames = 0;
    m_softwareSuspended = false;
    if (m_outputMode == OutputMode::Software) {
        printf("[Audio] Output mode: software; physical_device=NOT_RUN\n");
    } else if (m_outputMode == OutputMode::Device) {
        printf("[Audio] Output mode: device; physical_device=NOT_VERIFIED\n");
    }

    const bool manual = m_outputMode != OutputMode::Device;
    SoLoud::result res = m_soloud.init(
        SoLoud::Soloud::CLIP_ROUNDOFF,
        manual ? SoLoud::Soloud::NULLDRIVER : SoLoud::Soloud::AUTO,
        manual ? 48000 : SoLoud::Soloud::AUTO,
        // This argument is the device buffer size, not the stereo channel
        // count below. A two-frame WinMM buffer advances far slower than the
        // device clock; let each physical backend select its supported default.
        manual ? 2048 : SoLoud::Soloud::AUTO,
        2
    );
    if (res != SoLoud::SO_NO_ERROR) {
        DEBUG_ERR(SubSys::Audio, ErrCode::Audio_SoLoudInitFailed, "[Audio] SoLoud init failed: %d", res);
        rollbackInit();
        return false;
    }

    m_soloud.setGlobalVolume(m_globalVolume);

    // Buses produce the mixer's output rate; the default 44100 Hz bus would
    // otherwise introduce a second resampling stage on 48000 Hz devices.
    const float mixRate = static_cast<float>(m_soloud.getBackendSamplerate());
    m_bgmBus.mBaseSamplerate = mixRate;
    m_voiceBus.mBaseSamplerate = mixRate;
    m_seBus.mBaseSamplerate = mixRate;

    // Create and play audio buses -- all must succeed or init fails.
    // Apply any bus volume configured before init() (stored pending values),
    // matching the init-time application pattern of setGlobalVolume.
    m_bgmBus.setVolume(m_bgmVolume);
    m_bgmBusHandle = playBus(m_bgmBus);
    if (!m_soloud.isValidVoiceHandle(m_bgmBusHandle)) {
        DEBUG_ERR(SubSys::Audio, ErrCode::Audio_BusCreateFailed, "[Audio] BGM bus play() returned invalid handle 0");
        rollbackInit();
        return false;
    }

    m_voiceBus.setVolume(m_voiceVolume);
    m_voiceBusHandle = playBus(m_voiceBus);
    bool meterInstalled = false;
    {
        AudioMutexLock lock(m_soloud);
        const int bus = m_soloud.getVoiceFromHandle_internal(m_voiceBusHandle);
        meterInstalled = bus >= 0 && m_soloud.mVoice[bus]->mFilter[0] != nullptr;
    }
    if (!meterInstalled) {
        DEBUG_ERR(SubSys::Audio, ErrCode::Audio_BusCreateFailed, "[Audio] VOICE bus play() returned invalid handle 0");
        rollbackInit();
        return false;
    }

    m_seBus.setVolume(m_seVolume);
    m_seBusHandle = playBus(m_seBus);
    if (!m_soloud.isValidVoiceHandle(m_seBusHandle)) {
        DEBUG_ERR(SubSys::Audio, ErrCode::Audio_BusCreateFailed, "[Audio] SE bus play() returned invalid handle 0");
        rollbackInit();
        return false;
    }

    m_initialized = true;
    m_voiceGainMuted = !std::isfinite(m_voiceVolume) || m_voiceVolume <= 0;
    m_voiceFadeRecovering = false;
    printf("[Audio] SoLoud initialized: 3 buses (BGM, VOICE, SE) ready.\n");
    return true;
    } catch (...) {
        rollbackInit();
        return false;
    }
}

void SoLoudAudioEngine::shutdown(){
    CAESURA_ASSERT_MAIN_THREAD();
    if (!m_initialized) return;
    stopSessionAudio();

    std::size_t allocatedHandles = m_activeSE.size()
        + m_retiringBGM.size()
        + m_retiringVoice.size()
        + (m_currentBGM != 0 ? 1u : 0u);
    for (const unsigned int h : m_voicePool) allocatedHandles += (h != 0 ? 1u : 0u);
    m_soloud.stopAll();
    m_soloud.deinit();
    m_voiceMeter->invalidate(); // Joined: no callback can publish old PCM.
    m_waveCache.clear();
    m_waveLRU.clear();
    m_waveLRUMap.clear();
    m_activeSE.clear();
    m_retiringBGM.clear();
    m_retiringVoice.clear();
    m_initialized = false;
    m_currentBGM = 0;
    for (auto& h : m_voicePool) h = 0;
    m_bgmDucked = false;
    m_voiceCompletionsPending = 0;
    m_bgmBusHandle = 0;
    m_voiceBusHandle = 0;
    m_seBusHandle = 0;
    releaseAudioHandles(allocatedHandles);
    if (m_outputMode == OutputMode::Software) {
        // JSON numbers must stay locale-independent and finite. These counters
        // describe bytes that passed through the real mixer, not a device sink.
        std::ostringstream stats;
        stats.imbue(std::locale::classic());
        stats << std::setprecision(17)
              << "[Audio] Software mix stats: {\"frames\":" << m_softwareMixStats.frames
              << ",\"samples\":" << m_softwareMixStats.samples
              << ",\"nonzero_samples\":" << m_softwareMixStats.nonzeroSamples
              << ",\"nonfinite_samples\":" << m_softwareMixStats.nonfiniteSamples
              << ",\"peak\":" << m_softwareMixStats.peak
              << ",\"absolute_energy\":" << m_softwareMixStats.absoluteEnergy
              << ",\"saturated\":" << (m_softwareMixStats.saturated ? "true" : "false")
              << ",\"sample_rate\":48000,\"channels\":2,\"physical_device\":\"NOT_RUN\"}";
        printf("%s\n", stats.str().c_str());
    }
    printf("[Audio] SoLoud shut down.\n");

    m_rawWaveCache.clear();
}

void SoLoudAudioEngine::suspend(){
    CAESURA_ASSERT_MAIN_THREAD();
    if (!m_initialized || m_softwareSuspended) return;
    AudioMutexLock lock(m_soloud);
    m_voiceMeter->invalidate();
    m_softwareSuspended = true;
    for (unsigned i = 0; i < m_soloud.mHighestVoice; ++i)
        m_soloud.setVoicePause_internal(i, true);
}

void SoLoudAudioEngine::resume(){
    CAESURA_ASSERT_MAIN_THREAD();
    if (!m_initialized || !m_softwareSuspended) return;
    AudioMutexLock lock(m_soloud);
    m_voiceMeter->invalidate();
    for (unsigned i = 0; i < m_soloud.mHighestVoice; ++i)
        m_soloud.setVoicePause_internal(i, false);
    m_softwareSuspended = false;
}

void SoLoudAudioEngine::update(float deltaTime){
    CAESURA_ASSERT_MAIN_THREAD();
    if (!m_initialized) return;
    {
        AudioMutexLock lock(m_soloud);
        observeVoiceMuteLocked();
    }
    m_soloud.update3dAudio();
    if (m_outputMode == OutputMode::Software && !m_softwareSuspended &&
        std::isfinite(deltaTime) && deltaTime > 0) {
        // Match Engine's maximum simulation step. A public update caller must
        // not turn a large/invalid dt into an unbounded allocation or mix loop.
        const double frames = m_softwareFractionalFrames +
            (std::min)(static_cast<double>(deltaTime), 0.25) * 48000;
        auto remaining = static_cast<unsigned>(frames);
        m_softwareFractionalFrames = frames - remaining;
        constexpr unsigned blockFrames = 1024;
        std::array<float, blockFrames * 2> pcm{};
        const auto addCount = [this](uint64_t& counter, uint64_t amount) {
            const auto maximum = (std::numeric_limits<uint64_t>::max)();
            if (amount > maximum - counter) {
                counter = maximum;
                m_softwareMixStats.saturated = true;
            } else {
                counter += amount;
            }
        };
        while (remaining > 0) {
            const auto count = (std::min)(remaining, blockFrames);
            m_soloud.mix(pcm.data(), count);
            addCount(m_softwareMixStats.frames, count);
            addCount(m_softwareMixStats.samples, count * 2);
            for (unsigned i = 0; i < count * 2; ++i) {
                const auto sample = pcm[i];
                if (!std::isfinite(sample)) {
                    addCount(m_softwareMixStats.nonfiniteSamples, 1);
                    continue;
                }
                if (sample != 0) addCount(m_softwareMixStats.nonzeroSamples, 1);
                const auto magnitude = std::abs(sample);
                m_softwareMixStats.peak = (std::max)(m_softwareMixStats.peak, magnitude);
                const auto maximum = (std::numeric_limits<double>::max)();
                if (magnitude > maximum - m_softwareMixStats.absoluteEnergy) {
                    m_softwareMixStats.absoluteEnergy = maximum;
                    m_softwareMixStats.saturated = true;
                } else {
                    m_softwareMixStats.absoluteEnergy += magnitude;
                }
            }
            remaining -= count;
        }
    }
    {
        AudioMutexLock lock(m_soloud);
        observeVoiceMuteLocked();
    }
    cullFinishedHandles();
}

void SoLoudAudioEngine::releaseAudioHandles(std::size_t count) {
    auto& registry = BackendRegistry::instance();
    for (std::size_t i = 0; i < count; ++i) {
        registry.release("audio_handles");
    }
}

void SoLoudAudioEngine::retireHandle(
    SoLoud::handle handle,
    float fadeTime,
    std::vector<SoLoud::handle>& retiringHandles) {
    if (handle == 0) return;

    if (!m_soloud.isValidVoiceHandle(handle)) {
        releaseAudioHandles(1);
        return;
    }

    if (fadeTime <= 0.0f) {
        m_soloud.stop(handle);
        releaseAudioHandles(1);
        return;
    }

    m_soloud.fadeVolume(handle, 0.0f, fadeTime);
    m_soloud.scheduleStop(handle, fadeTime);
    retiringHandles.push_back(handle);
}

void SoLoudAudioEngine::stopRetiringHandles(
    std::vector<SoLoud::handle>& retiringHandles) {
    const std::size_t handleCount = retiringHandles.size();
    for (SoLoud::handle handle : retiringHandles) {
        if (m_soloud.isValidVoiceHandle(handle)) {
            m_soloud.stop(handle);
        }
    }
    retiringHandles.clear();
    releaseAudioHandles(handleCount);
}

void SoLoudAudioEngine::cullFinishedHandles() {
    std::size_t released = 0;

    if (m_currentBGM && m_soloud.hasConsumedVoiceTail(m_currentBGM, bgmSampleCount()))
        m_soloud.stop(m_currentBGM);
    if (m_currentBGM != 0 && !m_soloud.isValidVoiceHandle(m_currentBGM)) {
        m_currentBGM = 0;
        ++released;
    }
    bool hadActiveVoice = false;
    for (auto& h : m_voicePool) {
        if (h != 0) {
            if (m_soloud.isValidVoiceHandle(h)) {
                hadActiveVoice = true;
            } else {
                h = 0;
                if (m_voiceCompletionsPending <
                    std::numeric_limits<unsigned int>::max()) {
                    ++m_voiceCompletionsPending;
                }
                ++released;
            }
        }
    }
    // BGM ducking (VN standard): the moment the last voice finishes,
    // restore the BGM bus to its configured volume.
    if (!hadActiveVoice && m_bgmDucked) {
        m_bgmDucked = false;
        m_soloud.fadeVolume(m_bgmBusHandle, m_bgmVolume, 0.30f);
    }

    const auto firstFinished = std::remove_if(
        m_activeSE.begin(), m_activeSE.end(),
        [this, &released](SoLoud::handle handle) {
            if (m_soloud.isValidVoiceHandle(handle)) return false;
            ++released;
            m_rawWaveCache.erase(handle);  // finished raw-PCM Wav is dead
            return true;
        });
    m_activeSE.erase(firstFinished, m_activeSE.end());

    const auto cullRetiring = [this, &released](auto& retiringHandles) {
        const auto firstInvalid = std::remove_if(
            retiringHandles.begin(), retiringHandles.end(),
            [this, &released](SoLoud::handle handle) {
                if (m_soloud.isValidVoiceHandle(handle)) return false;
                ++released;
                return true;
            });
        retiringHandles.erase(firstInvalid, retiringHandles.end());
    };
    cullRetiring(m_retiringBGM);
    cullRetiring(m_retiringVoice);
    releaseAudioHandles(released);
    if (m_restoredBGMHandle && !m_soloud.isValidVoiceHandle(m_restoredBGMHandle)) {
        m_restoredBGMHandle = 0;
        m_restoredBGMSource.reset();
    }
}

// -- Global volume ---------------------------------------------------------

void SoLoudAudioEngine::setGlobalVolume(float volume){
    CAESURA_ASSERT_MAIN_THREAD();
    if (m_initialized) {
        AudioMutexLock lock(m_soloud);
        const bool wasMuted = !std::isfinite(m_globalVolume) || m_globalVolume <= 0;
        const bool muted = !std::isfinite(volume) || volume <= 0;
        if (wasMuted != muted) m_voiceMeter->invalidate();
        m_globalVolume = volume;
        // This vendor setter does not itself lock.
        m_soloud.setGlobalVolume(volume);
        return;
    }
    m_globalVolume = volume;
}

float SoLoudAudioEngine::getGlobalVolume() const {
    return m_globalVolume;
}

// -- Per-bus volume --------------------------------------------------------

void SoLoudAudioEngine::setBusVolume(const char* bus, float volume){
    CAESURA_ASSERT_MAIN_THREAD();
    // Store the pending value unconditionally, so a call before init() is
    // applied when init() starts the buses (same init-time application
    // pattern as setGlobalVolume). If already initialized, push it through
    // to the live SoLoud bus immediately.
    std::string b(bus);
    if (b == "bgm") {
        m_bgmVolume = volume;
        if (m_initialized) {
            m_bgmBus.setVolume(volume);
            m_soloud.setVolume(m_bgmBusHandle, m_bgmDucked ? volume * 0.35f : volume);
        }
    } else if (b == "voice") {
        m_voiceVolume = volume;
        if (m_initialized) {
            AudioMutexLock lock(m_soloud);
            observeVoiceMuteLocked();
            const bool muted = !std::isfinite(volume) || volume <= 0;
            if (muted != m_voiceGainMuted || (m_voiceFadeRecovering && muted))
                m_voiceMeter->invalidate();
            m_voiceGainMuted = muted;
            m_voiceFadeRecovering = false;
            m_voiceBus.setVolume(volume);
            const int index = m_soloud.getVoiceFromHandle_internal(m_voiceBusHandle);
            if (index >= 0) {
                m_soloud.mVoice[index]->mVolumeFader.mActive = 0;
                m_soloud.setVoiceVolume_internal(index, volume);
            }
        }
    } else if (b == "se") {
        m_seVolume = volume;
        if (m_initialized) {
            m_seBus.setVolume(volume);
            m_soloud.setVolume(m_seBusHandle, volume);
        }
    }
    printf("[Audio] Bus %s volume = %.2f\n", bus, volume);
}

float SoLoudAudioEngine::getBusVolume(const char* bus) const {
    std::string b(bus);
    if (b == "bgm")   return m_bgmVolume;
    if (b == "voice") return m_voiceVolume;
    if (b == "se")    return m_seVolume;
    return 1.0f;
}

void SoLoudAudioEngine::flushWaveCache() {
    // Only drop entries that are NOT currently playing: destroying a Wav
    // that SoLoud is still reading is a use-after-free. countAudioSource
    // reports live sources referencing the sample.
    for (auto it = m_waveCache.begin(); it != m_waveCache.end();) {
        if (!it->second) { it = m_waveCache.erase(it); continue; }
        if (m_soloud.countAudioSource(*it->second) > 0) {
            ++it;  // keep playing entries
        } else {
            it = m_waveCache.erase(it);
        }
    }
    // Rebuild the LRU from the surviving (playing) entries so subsequent
    // loadWave eviction never touches an empty list (UB fix: flush used to
    // clear the LRU index while leaving entries in the cache).
    m_waveLRU.clear();
    m_waveLRUMap.clear();
    for (auto& [file, wav] : m_waveCache) {
        if (wav) {
            m_waveLRU.push_front(file);
            m_waveLRUMap[file] = m_waveLRU.begin();
        }
    }
    printf("[Audio] Wave cache flushed (%zu playing entries kept).\n",
           m_waveCache.size());
}

// -- BGM: with cross-fade support (Spec [3.1][3.2]) ------------------------
// playBGM cross-fades out old BGM over fadeTime and fades in the new one.
// The fade-out uses fadeVolume() + scheduleStop() for a smooth transition.

unsigned int SoLoudAudioEngine::playBGM(const std::string& file, float fadeTime) {
    return playBGM(file, AudioPlaybackOptions{1.0f, false, fadeTime});
}

unsigned int SoLoudAudioEngine::playBGM(const std::string& file, const AudioPlaybackOptions& options) {
    if (!m_initialized || !validPlaybackOptions(options)) return 0;
    const float fadeTime = options.fadeIn;

    std::string playingPath = file;
    // Retirement bookkeeping must be allocated before creating a new voice.
    if (m_currentBGM) m_retiringBGM.reserve(m_retiringBGM.size() + 1);

    auto wav = loadWave(file);
    if (!wav) return 0;

    std::unique_ptr<SoLoud::AudioSourceInstance> instance(wav->createInstance());
    if (!instance) return 0;
    // Decoder EOF can precede the last audible source/bus interpolation frames.
    instance->mFlags |= SoLoud::AudioSourceInstance::DISABLE_AUTOSTOP;

    cullFinishedHandles();
    auto& registry = BackendRegistry::instance();
    if (!registry.tryAlloc("audio_handles")) return 0;

    // Start new BGM at volume 0, then fade in
    SoLoud::handle h = m_soloud.playPrepared(*wav, instance.release(), 0.0f, 0.0f, true, m_bgmBusHandle);
    if (h == 0 || !m_soloud.isValidVoiceHandle(h)) {
        registry.release("audio_handles");
        return 0;
    }

    // Retire the previous BGM only after replacement creation succeeds. Its
    // quota remains held until the physical SoLoud voice actually stops.
    if (m_currentBGM != 0) {
        retireHandle(m_currentBGM, fadeTime, m_retiringBGM);
        m_currentBGM = 0;
    }

    // Configure this paused instance only. Cached sources and persistent bus
    // gains must not inherit a previous clip's options.
    m_soloud.setLooping(h, options.loop);
    m_soloud.fadeVolume(h, options.volume, fadeTime);
    if (m_soloud.startBusVoice(h, m_bgmBus) != SoLoud::SO_NO_ERROR) {
        m_soloud.stop(h);
        registry.release("audio_handles");
        return 0;
    }
    m_currentBGM = h;
    m_currentBGMPath.swap(playingPath);

    printf("[Audio] BGM: %s (handle %u, fade %.1fs)\n", file.c_str(), h, fadeTime);
    return static_cast<unsigned int>(h);
}

void SoLoudAudioEngine::stopBGM(float fadeTime) {
    if (!m_initialized) return;
    stopRetiringHandles(m_retiringBGM);
    if (m_currentBGM == 0) return;

    const SoLoud::handle current = m_currentBGM;
    m_currentBGM = 0;
    retireHandle(current, fadeTime, m_retiringBGM);
}

// -- VOICE -----------------------------------------------------------------

unsigned int SoLoudAudioEngine::playVoice(const std::string& file){
    return playVoice(file, AudioPlaybackOptions{});
}

unsigned int SoLoudAudioEngine::playVoice(const std::string& file, const AudioPlaybackOptions& options) {
    CAESURA_ASSERT_MAIN_THREAD();
    if (!m_initialized || !validPlaybackOptions(options)) return 0;

    {
        AudioMutexLock lock(m_soloud);
        bool capacity = false;
        for (unsigned i = 0; i < VOICE_COUNT; ++i) {
            const auto* voice = m_soloud.mVoice[i];
            if (!voice || !(voice->mFlags & SoLoud::AudioSourceInstance::PROTECTED)) {
                capacity = true;
                break;
            }
        }
        // Vendor findFreeVoice_internal stops its selected victim even when
        // every slot is protected and no victim exists. Reject before that
        // path, without culling, quota allocation, or meter invalidation.
        if (!capacity) return 0;
    }
    // Playback admission is owner-thread only. Between this read and play,
    // the callback can release slots but cannot allocate/protect new voices.
    // Release the lock before play(), which acquires it itself.

    std::shared_ptr<SoLoud::AudioSource> wav;
    try {
        wav = loadWave(file);
        // Reserve retirement ownership before admitting a new mixer voice.
        m_retiringVoice.reserve(m_retiringVoice.size() + 1);
    } catch (...) { return 0; }
    if (!wav) return 0;

    cullFinishedHandles();
    auto& registry = BackendRegistry::instance();
    if (!registry.tryAlloc("audio_handles")) return 0;

    SoLoud::handle h = 0;
    try {
        // Admission is paused so a callback cannot publish a new session's
        // PCM before its epoch and owner slot have been committed together.
        h = m_voiceBus.play(*wav, options.fadeIn > 0 ? 0.0f : options.volume, 0.0f, true);
    } catch (...) {
        registry.release("audio_handles");
        return 0;
    }
    bool admitted = false;
    {
        AudioMutexLock lock(m_soloud);
        const int index = m_soloud.getVoiceFromHandle_internal(h);
        // Vendor failure returns UNKNOWN_ERROR (1), which can alias the
        // first bus handle. Handle validity alone does not prove admission.
        admitted = index >= 0 && m_soloud.mVoice[index]->mBusHandle == m_voiceBusHandle
            && m_soloud.mVoice[index]->mAudioSourceID == wav->mAudioSourceID;
    }
    if (!admitted) {
        registry.release("audio_handles");
        return 0;
    }

    m_soloud.setLooping(h, options.loop);
    if (options.fadeIn > 0) m_soloud.fadeVolume(h, options.volume, options.fadeIn);

    // Round-robin 4-slot voice pool: rapid character voice overlap (a VN
    // staple) no longer cuts the previous line; the displaced slot fades
    // out over 0.05s instead of being killed instantly.
    const unsigned int slot = m_voiceSlot % kVoicePoolSize;
    m_voiceSlot = (m_voiceSlot + 1) % kVoicePoolSize;
    if (m_voicePool[slot] != 0) {
        retireHandle(m_voicePool[slot], 0.05f, m_retiringVoice);
    }
    {
        AudioMutexLock lock(m_soloud);
        bool current = false;
        for (const auto handle : m_voicePool)
            current |= m_soloud.getVoiceFromHandle_internal(handle) >= 0;
        if (!current) m_voiceMeter->invalidate();
        m_voicePool[slot] = h;
        const int index = m_soloud.getVoiceFromHandle_internal(h);
        if (index >= 0) m_soloud.setVoicePause_internal(index, m_softwareSuspended);
    }

    // BGM ducking (VN standard): while a voice line plays, lower the BGM
    // bus to 35% over 0.15s; cullFinishedHandles restores it when the
    // last voice finishes. The configured bus volume (m_bgmVolume) is
    // untouched so restoration is exact.
    if (!m_bgmDucked && m_currentBGM != 0
        && m_soloud.isValidVoiceHandle(m_currentBGM)) {
        m_bgmDucked = true;
        m_soloud.fadeVolume(m_bgmBusHandle, m_bgmVolume * 0.35f, 0.15f);
    }

    printf("[Audio] Voice: %s (handle %u, slot %u)\n", file.c_str(), h, slot);
    return static_cast<unsigned int>(h);
}

void SoLoudAudioEngine::stopVoice(){
    CAESURA_ASSERT_MAIN_THREAD();
    if (!m_initialized) return;
    stopRetiringHandles(m_retiringVoice);
    bool any = false;
    for (auto& h : m_voicePool) {
        if (h != 0) {
            const SoLoud::handle cur = h;
            h = 0;
            retireHandle(cur, 0.05f, m_retiringVoice);
            any = true;
        }
    }
    if (any) {
        invalidateVoiceMeter();
        // Voice stop is also a ducking boundary: restore BGM immediately.
        if (m_bgmDucked) {
            m_bgmDucked = false;
            m_soloud.fadeVolume(m_bgmBusHandle, m_bgmVolume, 0.20f);
        }
    }
}

// -- SE --------------------------------------------------------------------
// SE handles are tracked in m_activeSE for mass-stop via stopSE().
// Dead handles are culled during update, state queries, and before playback.

void SoLoudAudioEngine::stopSE(){
    CAESURA_ASSERT_MAIN_THREAD();
    if (!m_initialized) return;
    const std::size_t handleCount = m_activeSE.size();
    for (auto h : m_activeSE) {
        if (m_soloud.isValidVoiceHandle(h)) {
            m_soloud.stop(h);
        }
    }
    m_activeSE.clear();
    m_rawWaveCache.clear();
    printf("[Audio] SE: all sound effects stopped.\n");
    releaseAudioHandles(handleCount);
}

void SoLoudAudioEngine::stopSE(float fadeTime) {
    CAESURA_ASSERT_MAIN_THREAD();
    if (!m_initialized || !std::isfinite(fadeTime) || fadeTime < 0) return;
    if (fadeTime == 0) { stopSE(); return; }
    // Retain owners, raw PCM sources and quotas until the scheduled physical
    // stop. cullFinishedHandles releases each one once the voice is invalid.
    for (auto h : m_activeSE) {
        if (m_soloud.isValidVoiceHandle(h)) {
            m_soloud.fadeVolume(h, 0.0f, fadeTime);
            m_soloud.scheduleStop(h, fadeTime);
        }
    }
}

unsigned int SoLoudAudioEngine::playSE(const std::string& file){
    return playSE(file, AudioPlaybackOptions{});
}

unsigned int SoLoudAudioEngine::playSE(const std::string& file, const AudioPlaybackOptions& options) {
    CAESURA_ASSERT_MAIN_THREAD();
    if (!m_initialized || !validPlaybackOptions(options)) return 0;
    // Same trusted owner-thread admission boundary as VOICE. With all voices
    // protected the vendor has no victim; do not enter its allocation path.
    if (!hasPlaybackCapacity(m_soloud)) return 0;
    auto wav = loadWave(file);
    if (!wav) return 0;

    cullFinishedHandles();
    m_activeSE.reserve(m_activeSE.size() + 1);
    auto& registry = BackendRegistry::instance();
    if (!registry.tryAlloc("audio_handles")) return 0;

    SoLoud::handle h = 0;
    try { h = m_seBus.play(*wav, options.fadeIn > 0 ? 0.0f : options.volume, 0.0f, true); }
    catch (...) { registry.release("audio_handles"); return 0; }
    if (!ownsBusVoice(m_soloud, h, m_seBusHandle, *wav)) {
        registry.release("audio_handles");
        return 0;
    }
    m_activeSE.push_back(h);
    m_soloud.setLooping(h, options.loop);
    if (options.fadeIn > 0) m_soloud.fadeVolume(h, options.volume, options.fadeIn);
    m_soloud.setPause(h, m_softwareSuspended);
    printf("[Audio] SE: %s (handle %u)\n", file.c_str(), h);
    return static_cast<unsigned int>(h);
}

unsigned int SoLoudAudioEngine::playRawPCM(const float* samples,
                                          unsigned int numFrames,
                                          unsigned int sampleRate,
                                          unsigned int channels) {
    CAESURA_ASSERT_MAIN_THREAD();
    if (!m_initialized || !samples || numFrames == 0 || sampleRate == 0 ||
        (channels != 1 && channels != 2)) {
        return 0;
    }

    cullFinishedHandles();
    auto& registry = BackendRegistry::instance();
    if (!registry.tryAlloc("audio_handles")) return 0;

    // loadRawWave takes raw interleaved float PCM directly (loadMem
    // expects a WAV file with header). Copy mode keeps our buffer valid.
    auto wav = std::make_shared<SoLoud::Wav>();
    // loadRawWave's length argument is the total sample (float) count, not
    // the frame count -- passing numFrames played back at half speed and
    // dropped half the PCM for stereo.
    if (wav->loadRawWave(const_cast<float*>(samples), numFrames * channels,
                         static_cast<float>(sampleRate), channels,
                         /*aCopy=*/true, /*aTakeOwnership=*/true) != SoLoud::SO_NO_ERROR) {
        registry.release("audio_handles");
        return 0;
    }

    SoLoud::handle h = m_seBus.play(*wav);
    if (h == 0 || !m_soloud.isValidVoiceHandle(h)) {
        registry.release("audio_handles");
        return 0;
    }
    m_activeSE.push_back(h);
    m_rawWaveCache[h] = wav;  // keep the Wav alive while playing
    printf("[Audio] Raw PCM: %u frames @ %u Hz (%u ch, handle %u)\n",
           numFrames, sampleRate, channels, h);
    return static_cast<unsigned int>(h);
}

unsigned int SoLoudAudioEngine::playSE3D(const std::string& file,
                                          float x, float y, float z) {
    return playSE3D(file, x, y, z, AudioPlaybackOptions{});
}

unsigned int SoLoudAudioEngine::playSE3D(const std::string& file, float x, float y, float z,
                                       const AudioPlaybackOptions& options) {
    CAESURA_ASSERT_MAIN_THREAD();
    if (!m_initialized || !validPlaybackOptions(options)
        || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return 0;
    if (!hasPlaybackCapacity(m_soloud)) return 0;
    auto wav = loadWave(file);
    if (!wav) return 0;

    cullFinishedHandles();
    m_activeSE.reserve(m_activeSE.size() + 1);
    auto& registry = BackendRegistry::instance();
    if (!registry.tryAlloc("audio_handles")) return 0;

    SoLoud::handle h = 0;
    try { h = m_seBus.play3d(*wav, x, y, z, 0, 0, 0,
                           options.fadeIn > 0 ? 0.0f : options.volume, true); }
    catch (...) { registry.release("audio_handles"); return 0; }
    // UNKNOWN_ERROR(1) must never be treated as an admitted unrelated bus.
    if (!ownsBusVoice(m_soloud, h, m_seBusHandle, *wav)) {
        registry.release("audio_handles");
        return 0;
    }
    m_activeSE.push_back(h);
    m_soloud.setLooping(h, options.loop);
    if (options.fadeIn > 0) m_soloud.fadeVolume(h, options.volume, options.fadeIn);
    m_soloud.setPause(h, m_softwareSuspended);
    printf("[Audio] SE 3D: %s at (%.1f,%.1f,%.1f) h=%u\n",
           file.c_str(), x, y, z, h);
    return static_cast<unsigned int>(h);
}

// -- 3D Audio --------------------------------------------------------------

void SoLoudAudioEngine::update3dListener(float posX, float posY, float posZ,
                                          float atX, float atY, float atZ,
                                          float upX, float upY, float upZ) {
    if (!m_initialized) return;
    m_soloud.set3dListenerParameters(posX, posY, posZ,
                                     atX, atY, atZ,
                                     upX, upY, upZ);
}

// -- State query -----------------------------------------------------------

unsigned int SoLoudAudioEngine::consumeVoiceCompletions() {
    const unsigned int completed = m_voiceCompletionsPending;
    m_voiceCompletionsPending = 0;
    return completed;
}

bool SoLoudAudioEngine::isVoicePlaying() {
    if (!m_initialized) return false;
    cullFinishedHandles();
    for (const auto h : m_voicePool) {
        if (h != 0) return true;
    }
    return false;
}

bool SoLoudAudioEngine::isBGMPlaying() {
    if (!m_initialized) return false;
    cullFinishedHandles();
    return m_currentBGM != 0;
}

bool SoLoudAudioEngine::isSEPlaying() {
    if (!m_initialized) return false;
    cullFinishedHandles();
    return !m_activeSE.empty();
}

int SoLoudAudioEngine::activeVoiceCount() {
    return m_initialized ? m_soloud.getActiveVoiceCount() : 0;
}

// -- Playback position (Spec [3.3]) -----------------------------------------

float SoLoudAudioEngine::getPosition(const char* bus) {
    if (!m_initialized) return 0.0f;
    std::string b(bus);
    SoLoud::handle h = 0;
    if (b == "voice") {
        for (const auto vh : m_voicePool) {
            if (vh != 0) { h = vh; break; }
        }
    } else if (b == "bgm")   h = m_currentBGM;
    if (h != 0 && m_soloud.isValidVoiceHandle(h))
        return (float)m_soloud.getStreamPosition(h);
    return 0.0f;
}

float SoLoudAudioEngine::getLength(const char* bus) {
    if (!m_initialized) return 0.0f;
    std::string b(bus);
    SoLoud::handle h = 0;
    if (b == "voice") {
        for (const auto vh : m_voicePool) {
            if (vh != 0) { h = vh; break; }
        }
    } else if (b == "bgm")   h = m_currentBGM;
    if (h != 0 && m_soloud.isValidVoiceHandle(h))
        return (float)m_soloud.getStreamTime(h);
    return 0.0f;
}

// -- Fade bus volume without stopping (Spec [3.2]) --------------------------

void SoLoudAudioEngine::fadeVolume(const char* bus, float targetVolume, float fadeTime){
    CAESURA_ASSERT_MAIN_THREAD();
    if (!m_initialized) return;
    std::string b(bus);
    if (b == "bgm") {
        m_bgmVolume = targetVolume;
        m_soloud.fadeVolume(m_bgmBusHandle, targetVolume, fadeTime);
    } else if (b == "voice") {
        m_voiceVolume = targetVolume;
        AudioMutexLock lock(m_soloud);
        observeVoiceMuteLocked();
        const int index = m_soloud.getVoiceFromHandle_internal(m_voiceBusHandle);
        if (index >= 0) {
            auto* voice = m_soloud.mVoice[index];
            const float from = voice->mSetVolume;
            const bool targetMuted = !std::isfinite(targetVolume) || targetVolume <= 0;
            if (fadeTime <= 0 || from == targetVolume) {
                if (targetMuted != m_voiceGainMuted || (m_voiceFadeRecovering && targetMuted))
                    m_voiceMeter->invalidate();
                m_voiceGainMuted = targetMuted;
                m_voiceFadeRecovering = false;
                voice->mVolumeFader.mActive = 0;
                m_soloud.setVoiceVolume_internal(index, targetVolume);
            } else {
                if (m_voiceGainMuted && !targetMuted && !m_voiceFadeRecovering) {
                    m_voiceMeter->invalidate();
                    m_voiceFadeRecovering = true;
                }
                if (targetMuted) m_voiceFadeRecovering = false;
                // Equivalent to SoLoud::fadeVolume, within the same lock as
                // recovery invalidation. The stored target is not live gain.
                voice->mVolumeFader.set(from, targetVolume, fadeTime, voice->mStreamTime);
            }
        }
    } else if (b == "se") {
        m_seVolume = targetVolume;
        m_soloud.fadeVolume(m_seBusHandle, targetVolume, fadeTime);
    }
    printf("[Audio] Bus %s fade to %.2f over %.2fs\n", bus, targetVolume, fadeTime);
}


// -- [10.2.27] Per-SE-handle volume control ---------------------------------

void SoLoudAudioEngine::setSEVolume(unsigned int handle, float volume){
    CAESURA_ASSERT_MAIN_THREAD();
    if (!m_initialized || handle == 0) return;
    m_soloud.setVolume(handle, volume);
}

float SoLoudAudioEngine::getSEVolume(unsigned int handle) {
    if (!m_initialized || handle == 0) return 0.0f;
    return m_soloud.getVolume(handle);
}

void SoLoudAudioEngine::stopSEHandle(unsigned int handle){
    CAESURA_ASSERT_MAIN_THREAD();
    if (!m_initialized || handle == 0) return;
    auto it = std::find(m_activeSE.begin(), m_activeSE.end(), handle);
    if (it == m_activeSE.end()) return;

    if (m_soloud.isValidVoiceHandle(handle)) {
        m_soloud.stop(handle);
    }
    m_activeSE.erase(it);
    BackendRegistry::instance().release("audio_handles");
}

} // namespace Caesura
