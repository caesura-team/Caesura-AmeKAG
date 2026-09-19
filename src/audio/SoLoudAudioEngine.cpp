#include "SoLoudAudioEngine.h"
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

SoLoudAudioEngine::~SoLoudAudioEngine() {
    shutdown();
}

bool SoLoudAudioEngine::init(){
    CAESURA_ASSERT_MAIN_THREAD();
    if (m_initialized) return true;
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
        manual ? 2048 : 2,
        2
    );
    if (res != SoLoud::SO_NO_ERROR) {
        DEBUG_ERR(SubSys::Audio, ErrCode::Audio_SoLoudInitFailed, "[Audio] SoLoud init failed: %d", res);
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
    m_bgmBusHandle = m_soloud.play(m_bgmBus);
    if (!m_soloud.isValidVoiceHandle(m_bgmBusHandle)) {
        DEBUG_ERR(SubSys::Audio, ErrCode::Audio_BusCreateFailed, "[Audio] BGM bus play() returned invalid handle 0");
        m_soloud.deinit();
        return false;
    }

    m_voiceBus.setVolume(m_voiceVolume);
    m_voiceBusHandle = m_soloud.play(m_voiceBus);
    if (!m_soloud.isValidVoiceHandle(m_voiceBusHandle)) {
        DEBUG_ERR(SubSys::Audio, ErrCode::Audio_BusCreateFailed, "[Audio] VOICE bus play() returned invalid handle 0");
        m_soloud.deinit();
        return false;
    }

    m_seBus.setVolume(m_seVolume);
    m_seBusHandle = m_soloud.play(m_seBus);
    if (!m_soloud.isValidVoiceHandle(m_seBusHandle)) {
        DEBUG_ERR(SubSys::Audio, ErrCode::Audio_BusCreateFailed, "[Audio] SE bus play() returned invalid handle 0");
        m_soloud.deinit();
        return false;
    }

    m_initialized = true;
    printf("[Audio] SoLoud initialized: 3 buses (BGM, VOICE, SE) ready.\n");
    return true;
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
    if (!m_initialized) return;
    m_softwareSuspended = true;
    m_soloud.setPauseAll(true);
}

void SoLoudAudioEngine::resume(){
    CAESURA_ASSERT_MAIN_THREAD();
    if (!m_initialized) return;
    m_soloud.setPauseAll(false);
    m_softwareSuspended = false;
}

void SoLoudAudioEngine::update(float deltaTime){
    CAESURA_ASSERT_MAIN_THREAD();
    if (!m_initialized) return;
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
    m_globalVolume = volume;
    if (m_initialized) m_soloud.setGlobalVolume(volume);
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
            m_voiceBus.setVolume(volume);
            m_soloud.setVolume(m_voiceBusHandle, volume);
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
    if (!m_initialized) return 0;

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

    m_soloud.fadeVolume(h, 1.0f, fadeTime);
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
    CAESURA_ASSERT_MAIN_THREAD();
    if (!m_initialized) return 0;

    auto wav = loadWave(file);
    if (!wav) return 0;

    cullFinishedHandles();
    auto& registry = BackendRegistry::instance();
    if (!registry.tryAlloc("audio_handles")) return 0;

    SoLoud::handle h = m_voiceBus.play(*wav);
    if (h == 0 || !m_soloud.isValidVoiceHandle(h)) {
        registry.release("audio_handles");
        return 0;
    }

    // Round-robin 4-slot voice pool: rapid character voice overlap (a VN
    // staple) no longer cuts the previous line; the displaced slot fades
    // out over 0.05s instead of being killed instantly.
    const unsigned int slot = m_voiceSlot % kVoicePoolSize;
    m_voiceSlot = (m_voiceSlot + 1) % kVoicePoolSize;
    if (m_voicePool[slot] != 0) {
        retireHandle(m_voicePool[slot], 0.05f, m_retiringVoice);
    }
    m_voicePool[slot] = h;

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

unsigned int SoLoudAudioEngine::playSE(const std::string& file){
    CAESURA_ASSERT_MAIN_THREAD();
    if (!m_initialized) return 0;
    auto wav = loadWave(file);
    if (!wav) return 0;

    cullFinishedHandles();
    auto& registry = BackendRegistry::instance();
    if (!registry.tryAlloc("audio_handles")) return 0;

    SoLoud::handle h = m_seBus.play(*wav);
    if (h == 0 || !m_soloud.isValidVoiceHandle(h)) {
        registry.release("audio_handles");
        return 0;
    }
    m_activeSE.push_back(h);
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
    CAESURA_ASSERT_MAIN_THREAD();
    if (!m_initialized) return 0;
    auto wav = loadWave(file);
    if (!wav) return 0;

    cullFinishedHandles();
    auto& registry = BackendRegistry::instance();
    if (!registry.tryAlloc("audio_handles")) return 0;

    SoLoud::handle h = m_seBus.play3d(*wav, x, y, z);
    if (h == 0 || !m_soloud.isValidVoiceHandle(h)) {
        registry.release("audio_handles");
        return 0;
    }
    m_activeSE.push_back(h);
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
        m_soloud.fadeVolume(m_voiceBusHandle, targetVolume, fadeTime);
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
