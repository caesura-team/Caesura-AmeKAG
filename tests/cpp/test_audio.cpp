#include "doctest.h"
#include <algorithm>
#include <cstring>
#include "audio/SoLoudAudioEngine.h"
#include "audio/NullAudioBackend.h"
#include "audio/AudioFocusService.h"
#include "audio/api/IAudioFocusService.h"
#include "di/BackendRegistry.h"
#include "di/api/ISandboxQuota.h"
#include "job/JobSystem.h"
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>
#include <soloud_file.h>
#include <soloud_wav.h>
#include <soloud_wavstream.h>

using namespace Caesura;

namespace {

// Business-contract checks own the real mixer's clock. Device callbacks may
// consume the 100 ms fixture or a 50 ms retirement between two assertions.
// Each block advances 512 / 48000 seconds; it is not physical-device evidence.
void mixAudioContractBlock(SoLoudAudioEngine& audio) {
    REQUIRE(audio.soloud().getBackendId() == SoLoud::Soloud::NULLDRIVER);
    REQUIRE(audio.soloud().getBackendSamplerate() == 48000);
    std::array<float, 512 * 2> pcm{};
    audio.soloud().mix(pcm.data(), 512);
    CHECK(std::all_of(pcm.begin(), pcm.end(), [](float sample) { return std::isfinite(sample); }));
}

class AudioQuota final : public ISandboxQuota {
public:
    explicit AudioQuota(int limit) : m_limit(limit) {}

    void setLuaState(lua_State*) override {}

    bool tryAlloc(const char* kind) override {
        ++tryCalls;
        if (std::strcmp(kind, "audio_handles") != 0) {
            ++unexpectedKinds;
            return false;
        }
        if (activeCount >= m_limit) return false;
        ++activeCount;
        peakCount = std::max(peakCount, activeCount);
        return true;
    }

    void release(const char* kind) override {
        ++releaseCalls;
        if (std::strcmp(kind, "audio_handles") != 0) {
            ++unexpectedKinds;
            return;
        }
        if (activeCount == 0) {
            ++releaseUnderflows;
            return;
        }
        --activeCount;
    }

    int count(const char*) override { return activeCount; }
    int maxLimit(const char*) override { return m_limit; }

    int activeCount = 0;
    int peakCount = 0;
    int tryCalls = 0;
    int releaseCalls = 0;
    int releaseUnderflows = 0;
    int unexpectedKinds = 0;

private:
    int m_limit;
};

class ScopedAudioQuota final {
public:
    explicit ScopedAudioQuota(ISandboxQuota& quota)
        : m_registry(BackendRegistry::instance()),
          m_previous(m_registry.getSandboxQuota()) {
        m_registry.setSandboxQuota(&quota);
    }

    ~ScopedAudioQuota() { m_registry.setSandboxQuota(m_previous); }

    ScopedAudioQuota(const ScopedAudioQuota&) = delete;
    ScopedAudioQuota& operator=(const ScopedAudioQuota&) = delete;

private:
    BackendRegistry& m_registry;
    ISandboxQuota* m_previous;
};

class AudioPathFiles final {
public:
    AudioPathFiles() {
        static std::atomic<unsigned> sequence{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt != 16; ++attempt) {
            m_root = std::filesystem::path("tests/audio") /
                ("u21-paths-" + std::to_string(stamp) + "-" + std::to_string(sequence++));
            if (std::filesystem::create_directory(m_root)) return;
        }
        throw std::runtime_error("Cannot create exclusive audio path fixture directory");
    }

    ~AudioPathFiles() {
        std::error_code ignored;
        for (const auto& file : m_files) std::filesystem::remove(file, ignored);
        for (auto directory = m_directories.rbegin(); directory != m_directories.rend(); ++directory)
            std::filesystem::remove(*directory, ignored);
        std::filesystem::remove(m_root, ignored);
    }

    std::string path(const std::filesystem::path& relative) const {
        const auto utf8 = (m_root / relative).generic_u8string();
        return std::string(utf8.begin(), utf8.end());
    }

    std::string copyWave(const std::filesystem::path& relative) {
        const auto destination = m_root / relative;
        if (destination.parent_path() != m_root &&
            std::filesystem::create_directory(destination.parent_path())) {
            m_directories.push_back(destination.parent_path());
        }
        std::filesystem::copy_file("tests/audio/silence.wav", destination);
        m_files.push_back(destination);
        return path(relative);
    }

    std::string corruptWave() {
        const auto destination = m_root / "corrupt.wav";
        std::ofstream output(destination, std::ios::binary);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        m_files.push_back(destination);
        output << "not a WAV file\n";
        output.close();
        return path("corrupt.wav");
    }

    AudioPathFiles(const AudioPathFiles&) = delete;
    AudioPathFiles& operator=(const AudioPathFiles&) = delete;

private:
    std::filesystem::path m_root;
    std::vector<std::filesystem::path> m_files;
    std::vector<std::filesystem::path> m_directories;
};

} // namespace

TEST_CASE("U22 software audio: update mixes real stereo PCM against the manual mixer") {
    AudioQuota quota(4);
    ScopedAudioQuota scopedQuota(quota);
    SoLoudAudioEngine software{SoLoudAudioEngine::OutputMode::Software};
    SoLoudAudioEngine manual{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(software.init());
    REQUIRE(manual.init());
    CHECK(software.soloud().getBackendId() == SoLoud::Soloud::NULLDRIVER);
    CHECK(software.soloud().getBackendSamplerate() == 48000);
    CHECK(software.soloud().getBackendChannels() == 2);
    std::vector<float> source(48000 * 2);
    for (unsigned i = 0; i < 48000; ++i) {
        source[i * 2] = float(int(i % 97) - 48) / 500.0f;
        source[i * 2 + 1] = float(int(i % 71) - 35) / 700.0f;
    }
    const auto softwareHandle = software.playRawPCM(source.data(), 48000, 48000, 2);
    const auto manualHandle = manual.playRawPCM(source.data(), 48000, 48000, 2);
    REQUIRE(softwareHandle != 0);
    REQUIRE(manualHandle != 0);
    std::vector<float> reference(375 * 2);
    double energy = 0;
    float peak = 0;
    uint64_t nonzero = 0;
    for (unsigned block = 0; block != 32; ++block) {
        // Exactly representable dt: 48 kHz / 128 = 375 sample frames.
        software.update(1.0f / 128);
        manual.soloud().mix(reference.data(), 375);
        for (const auto sample : reference) {
            REQUIRE(std::isfinite(sample));
            energy += std::abs(sample);
            peak = std::max(peak, std::abs(sample));
            nonzero += sample != 0;
        }
    }
    const auto stats = software.softwareMixStats();
    CHECK(stats.frames == 12000);
    CHECK(stats.samples == 24000);
    CHECK(stats.nonfiniteSamples == 0);
    CHECK_FALSE(stats.saturated);
    CHECK(stats.nonzeroSamples == nonzero);
    CHECK(stats.nonzeroSamples > 0);
    CHECK(stats.absoluteEnergy == doctest::Approx(energy).epsilon(0.000001));
    CHECK(stats.peak == doctest::Approx(peak));
    CHECK(energy > 1);
    CHECK(software.soloud().getStreamPosition(softwareHandle) ==
          doctest::Approx(manual.soloud().getStreamPosition(manualHandle)));
    CHECK(software.soloud().getStreamPosition(softwareHandle) > 0);
}

TEST_CASE("U22 software audio: suspension does not advance or accrue catch-up time") {
    AudioQuota quota(2);
    ScopedAudioQuota scopedQuota(quota);
    SoLoudAudioEngine audio{SoLoudAudioEngine::OutputMode::Software};
    REQUIRE(audio.init());
    std::vector<float> source(48000 * 2, 0.1f);
    const auto handle = audio.playRawPCM(source.data(), 48000, 48000, 2);
    REQUIRE(handle != 0);
    audio.update(1.0f / 64);
    CHECK(audio.softwareMixStats().frames == 750);
    const auto position = audio.soloud().getStreamPosition(handle);
    audio.suspend();
    audio.update(0.25f);
    audio.update(0.25f);
    CHECK(audio.softwareMixStats().frames == 750);
    CHECK(audio.soloud().getStreamPosition(handle) == position);
    audio.resume();
    audio.update(1.0f / 64);
    CHECK(audio.softwareMixStats().frames == 1500);
    CHECK(audio.soloud().getStreamPosition(handle) > position);
}

TEST_CASE("U22 software audio: natural completion reclaims SE and voice quotas once") {
    AudioQuota quota(2);
    ScopedAudioQuota scopedQuota(quota);
    SoLoudAudioEngine audio{SoLoudAudioEngine::OutputMode::Software};
    REQUIRE(audio.init());
    std::vector<float> source(128 * 2, 0.1f);
    REQUIRE(audio.playRawPCM(source.data(), 128, 48000, 2) != 0);
    REQUIRE(audio.playVoice("tests/audio/silence.wav") != 0);
    REQUIRE(quota.activeCount == 2);
    audio.update(0.25f);
    audio.update(0.25f);
    CHECK_FALSE(audio.isSEPlaying());
    CHECK_FALSE(audio.isVoicePlaying());
    CHECK(quota.activeCount == 0);
    CHECK(quota.releaseCalls == 2);
    CHECK(quota.releaseUnderflows == 0);
    CHECK(audio.consumeVoiceCompletions() == 1);
    audio.update(0.25f);
    CHECK(audio.consumeVoiceCompletions() == 0);
    CHECK(quota.releaseCalls == 2);
}

TEST_CASE("U22 software audio: fractional and invalid time inputs remain bounded") {
    SoLoudAudioEngine audio{SoLoudAudioEngine::OutputMode::Software};
    REQUIRE(audio.init());
    const float fractional = 1.0f / 131072;
    audio.update(fractional); // 0.3662109375 sample frames, retained precisely.
    for (const float invalid : {0.0f, -1.0f, std::numeric_limits<float>::infinity(),
             -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        audio.update(invalid);
    }
    CHECK(audio.softwareMixStats().frames == 0);
    for (unsigned i = 1; i < 512; ++i) audio.update(fractional);
    CHECK(audio.softwareMixStats().frames == 187); // 187.5 frames, not 512 rounded samples.
    audio.update(std::numeric_limits<float>::max());
    CHECK(audio.softwareMixStats().frames == 12187); // At most 0.25 seconds per update.
    CHECK(audio.softwareMixStats().samples == 24374);
    CHECK(audio.softwareMixStats().nonfiniteSamples == 0);
    CHECK_FALSE(audio.softwareMixStats().saturated);
    CHECK(audio.softwareMixStats().nonzeroSamples == 0); // Actual initialized mixer silence.
}

TEST_CASE("U22 software audio: ManualMix update preserves its explicit host clock") {
    AudioQuota quota(1);
    ScopedAudioQuota scopedQuota(quota);
    SoLoudAudioEngine audio{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(audio.init());
    std::vector<float> source(48000 * 2, 0.1f);
    const auto handle = audio.playRawPCM(source.data(), 48000, 48000, 2);
    REQUIRE(handle != 0);
    audio.update(0.25f);
    CHECK(audio.soloud().getStreamPosition(handle) == 0);
    CHECK(audio.softwareMixStats().frames == 0);
    std::vector<float> output(512 * 2);
    audio.soloud().mix(output.data(), 512);
    CHECK(audio.soloud().getStreamPosition(handle) > 0);
    CHECK(std::any_of(output.begin(), output.end(), [](float sample) { return sample != 0; }));
}

TEST_CASE("U22 software audio: shutdown and reinit reset the session clock") {
    SoLoudAudioEngine audio{SoLoudAudioEngine::OutputMode::Software};
    REQUIRE(audio.init());
    audio.update(1.0f / 131072); // Leave a fractional sample before shutdown.
    audio.update(1.0f / 64);
    CHECK(audio.softwareMixStats().frames == 750);
    audio.suspend();
    audio.shutdown();
    audio.update(0.25f);
    CHECK(audio.softwareMixStats().frames == 750);
    REQUIRE(audio.init());
    CHECK(audio.softwareMixStats().frames == 0);
    CHECK(audio.softwareMixStats().absoluteEnergy == 0);
    audio.update(1.0f / 131072);
    audio.update(1.0f / 131072);
    CHECK(audio.softwareMixStats().frames == 0); // Old fractional remainder is gone.
    audio.update(1.0f / 131072);
    CHECK(audio.softwareMixStats().frames == 1); // Old suspension is also gone.
}

TEST_CASE("U21 audio paths: WAV playback accepts UTF-8 directories and filenames") {
    REQUIRE(std::filesystem::is_regular_file("tests/audio/silence.wav"));
    std::filesystem::path relative;
    SUBCASE("ASCII") { relative = "ascii.wav"; }
    SUBCASE("UTF-8 directory") { relative = u8"\u65c5\u7a0b/loop.wav"; }
    SUBCASE("UTF-8 filename") { relative = u8"\u5faa\u73af.wav"; }
    AudioPathFiles files;
    const auto file = files.copyWave(relative);
    CAPTURE(file);
    AudioQuota quota(4);
    ScopedAudioQuota scopedQuota(quota);
    SoLoudAudioEngine audio{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(audio.init());
    REQUIRE(audio.playBGM(file, 0) != 0);
    CHECK(audio.isBGMPlaying());
    std::vector<float> pcm(512 * 2, 1.0f);
    audio.soloud().mix(pcm.data(), 512);
    CHECK(std::all_of(pcm.begin(), pcm.end(), [](float sample) { return sample == 0.0f; }));
    CHECK(audio.getPosition("bgm") > 0);
    audio.stopBGM(0);
    CHECK(quota.activeCount == 0);
}

TEST_CASE("U21 audio paths: WavStream reopens UTF-8 files for each decoder instance") {
    REQUIRE(std::filesystem::is_regular_file("tests/audio/silence.wav"));
    std::filesystem::path relative;
    SUBCASE("ASCII") { relative = "stream.wav"; }
    SUBCASE("UTF-8 directory") { relative = u8"\u65c5\u7a0b/stream.wav"; }
    SUBCASE("UTF-8 filename") { relative = u8"\u6d41\u5f0f.wav"; }
    AudioPathFiles files;
    const auto file = files.copyWave(relative);
    CAPTURE(file);
    SoLoud::WavStream stream;
    REQUIRE(stream.load(file.c_str()) == SoLoud::SO_NO_ERROR);
    REQUIRE(stream.mMemFile == nullptr);
    REQUIRE(stream.mStreamFile == nullptr);
    REQUIRE(stream.mFilename != nullptr);
    CHECK(std::string(stream.mFilename) == file);
    REQUIRE(stream.mChannels > 0);
    REQUIRE(stream.mSampleCount > 128);
    for (int generation = 0; generation != 2; ++generation) {
        CAPTURE(generation);
        // Each constructor reopens mFilename through the production DiskFile;
        // no memory/file override can make this a load-only success.
        std::unique_ptr<SoLoud::AudioSourceInstance> instance(stream.createInstance());
        REQUIRE(instance != nullptr);
        instance->init(stream, 0);
        std::vector<float> pcm(64 * stream.mChannels, 1.0f);
        REQUIRE(instance->getAudio(pcm.data(), 64, 64) == 64);
        CHECK(std::all_of(pcm.begin(), pcm.end(), [](float sample) { return sample == 0.0f; }));
        REQUIRE(instance->seekFrame(32) == SoLoud::SO_NO_ERROR);
        REQUIRE(instance->getAudio(pcm.data(), 64, 64) == 64);
        CHECK(std::all_of(pcm.begin(), pcm.end(), [](float sample) { return sample == 0.0f; }));
    }
}

TEST_CASE("U21 audio paths: missing and corrupt inputs fail without poisoning later playback") {
    REQUIRE(std::filesystem::is_regular_file("tests/audio/silence.wav"));
    AudioPathFiles files;
    const auto missing = files.path("missing.wav");
    const auto corrupt = files.corruptWave();
    const auto valid = files.copyWave("recovery.wav");
    AudioQuota quota(4);
    ScopedAudioQuota scopedQuota(quota);
    SoLoudAudioEngine audio{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(audio.init());
    CHECK(audio.playBGM(missing, 0) == 0);
    CHECK(audio.playBGM(corrupt, 0) == 0);
    CHECK(quota.activeCount == 0);
    REQUIRE(audio.playBGM(valid, 0) != 0);
    CHECK(audio.isBGMPlaying());
    audio.stopBGM(0);
    CHECK(quota.activeCount == 0);
    SoLoud::WavStream stream;
    CHECK(stream.load(missing.c_str()) != SoLoud::SO_NO_ERROR);
    CHECK(stream.load(corrupt.c_str()) != SoLoud::SO_NO_ERROR);
    REQUIRE(stream.load(valid.c_str()) == SoLoud::SO_NO_ERROR);
    std::unique_ptr<SoLoud::AudioSourceInstance> instance(stream.createInstance());
    REQUIRE(instance != nullptr);
    instance->init(stream, 0);
    std::vector<float> pcm(64 * stream.mChannels);
    CHECK(instance->getAudio(pcm.data(), 64, 64) == 64);
}

#ifdef _WIN32
TEST_CASE("U21 audio paths: Windows rejects invalid UTF-8 before opening a file") {
    AudioPathFiles files;
    const std::string invalid[] = {"\xc0\xaf", "\xed\xa0\x80", "\xff", "\xe4\xb8"};
    for (const auto& sequence : invalid) {
        const std::string path = files.path("invalid-") + sequence + ".wav";
        SoLoud::DiskFile file;
        CHECK(file.open(path.c_str()) == SoLoud::INVALID_PARAMETER);
        CHECK(file.getFilePtr() == nullptr);
    }
    const auto valid = files.copyWave("valid-after-invalid.wav");
    SoLoud::DiskFile file;
    CHECK(file.open(valid.c_str()) == SoLoud::SO_NO_ERROR);
    CHECK(file.length() == std::filesystem::file_size(std::filesystem::path(valid)));
}
#endif

TEST_CASE("SoLoudAudioEngine::name") {
    SoLoudAudioEngine eng;
    CHECK(strcmp(eng.getBackendName(), "SoLoud") == 0);
}

TEST_CASE("SoLoudAudioEngine::init succeeds") {
    SoLoudAudioEngine eng;
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }
    CHECK(eng.isBGMPlaying() == false);
    CHECK(eng.isVoicePlaying() == false);
    CHECK(eng.activeVoiceCount() >= 0);
}

TEST_CASE("SoLoudAudioEngine::global volume") {
    SoLoudAudioEngine eng;
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }
    eng.setGlobalVolume(0.5f);
    CHECK(eng.getGlobalVolume() == doctest::Approx(0.5f));
    eng.setGlobalVolume(1.0f);
    CHECK(eng.getGlobalVolume() == doctest::Approx(1.0f));
}

TEST_CASE("SoLoudAudioEngine::bus volume persistence") {
    SoLoudAudioEngine eng;
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }
    eng.setBusVolume("bgm", 0.8f);
    CHECK(eng.getBusVolume("bgm") == doctest::Approx(0.8f));
    eng.setBusVolume("voice", 0.6f);
    CHECK(eng.getBusVolume("voice") == doctest::Approx(0.6f));
}

TEST_CASE("SoLoudAudioEngine::fade volume does not crash") {
    SoLoudAudioEngine eng;
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }
    eng.fadeVolume("bgm", 0.0f, 0.5f);
    eng.fadeVolume("voice", 0.5f, 1.0f);
    eng.fadeVolume("se", 1.0f, 0.3f);
}

TEST_CASE("SoLoudAudioEngine::shutdown idempotent") {
    SoLoudAudioEngine eng;
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }
    eng.shutdown();
    eng.shutdown();
    CHECK(eng.activeVoiceCount() == 0);
}

TEST_CASE("SoLoudAudioEngine::playSE returns handle") {
    SoLoudAudioEngine eng;
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }
    // Play non-existent file returns 0, doesn't crash
    unsigned int h = eng.playSE("nonexistent.wav");
    CHECK(h == 0);
}

TEST_CASE("SoLoudAudioEngine::LRU cache survives multiple plays") {
    SoLoudAudioEngine eng;
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }
    for (int i = 0; i < 10; i++) {
        eng.playSE("nonexistent.wav");  // each call attempts load
    }
    // Cache operations should not crash
}


TEST_CASE("SoLoudAudioEngine::load WAV format") {
    SoLoudAudioEngine eng;
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }
    unsigned int h = eng.playSE("tests/audio/silence.wav");
    CHECK(h > 0);
    eng.stopSE();
}

TEST_CASE("SoLoudAudioEngine::load FLAC format") {
    SoLoudAudioEngine eng;
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }
    unsigned int h = eng.playSE("tests/audio/silence.flac");
    CHECK(h > 0);
    eng.stopSE();
}

TEST_CASE("SoLoudAudioEngine::unsupported format returns 0 no crash") {
    SoLoudAudioEngine eng;
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }
    unsigned int h = eng.playSE("CMakeLists.txt");
    CHECK(h == 0);
    unsigned int h2 = eng.playSE("");
    CHECK(h2 == 0);
}

// =============================================================================
// Expanded: BGM, Voice, SE3D, SE control, 3D, position, flush
// =============================================================================

TEST_CASE("SoLoudAudioEngine::playBGM and stopBGM with silence") {
    SoLoudAudioEngine eng{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(eng.init());
    unsigned int h = eng.playBGM("tests/audio/silence.wav", 0.0f);
    CHECK(h > 0);
    mixAudioContractBlock(eng);
    CHECK(eng.isBGMPlaying());
    eng.stopBGM(0.0f);
}

TEST_CASE("SoLoudAudioEngine::playVoice and stopVoice with silence") {
    SoLoudAudioEngine eng{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(eng.init());
    const unsigned int h = eng.playVoice("tests/audio/silence.wav");
    REQUIRE(h > 0);
    // Advance 512 / 48000 seconds of the real mixer, below the 100 ms fixture.
    // The helper requires NULLDRIVER and 48 kHz; no device callback owns time.
    mixAudioContractBlock(eng);
    CHECK(eng.soloud().isValidVoiceHandle(h));
    CHECK(eng.isVoicePlaying());
    CHECK(eng.consumeVoiceCompletions() == 0);
    eng.stopVoice();
    CHECK_FALSE(eng.isVoicePlaying());
    CHECK(eng.consumeVoiceCompletions() == 0);

    eng.shutdown();
    CHECK(eng.activeVoiceCount() == 0);
    CHECK_FALSE(eng.isVoicePlaying());
    CHECK(eng.consumeVoiceCompletions() == 0);
}

TEST_CASE("SoLoudAudioEngine reports each naturally finished current voice once") {
    SoLoudAudioEngine eng;
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }
    const unsigned int handle = eng.playVoice("tests/audio/silence.wav");
    REQUIRE(handle != 0);
    eng.soloud().setLooping(handle, true);
    CHECK(eng.consumeVoiceCompletions() == 0);

    eng.soloud().stop(handle);
    REQUIRE_FALSE(eng.soloud().isValidVoiceHandle(handle));
    eng.update(0.0f);

    CHECK(eng.consumeVoiceCompletions() == 1);
    CHECK(eng.consumeVoiceCompletions() == 0);
}

TEST_CASE("SoLoudAudioEngine::playSE3D with silence") {
    SoLoudAudioEngine eng;
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }
    unsigned int h = eng.playSE3D("tests/audio/silence.wav", 0, 0, -5);
    CHECK(h > 0);
    eng.stopSE();
}

TEST_CASE("SoLoudAudioEngine::setSEVolume and stopSEHandle") {
    SoLoudAudioEngine eng{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(eng.init());
    unsigned int h = eng.playSE("tests/audio/silence.wav");
    REQUIRE(h > 0);
    eng.setSEVolume(h, 0.5f);
    mixAudioContractBlock(eng);
    CHECK(eng.getSEVolume(h) == doctest::Approx(0.5f));
    eng.stopSEHandle(h);
    // stopSE handle 0 should not crash
    eng.stopSEHandle(0);
}

TEST_CASE("SoLoudAudioEngine::update3dListener does not crash") {
    SoLoudAudioEngine eng;
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }
    eng.update3dListener(0, 0, 0, 1, 0, 0);
    eng.update3dListener(10, 5, -3, 0, 1, 0, 0, 1, 0);
}

TEST_CASE("SoLoudAudioEngine::isSEPlaying returns false initially") {
    SoLoudAudioEngine eng;
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }
    CHECK_FALSE(eng.isSEPlaying());
}

TEST_CASE("SoLoudAudioEngine::getPosition and getLength return zero initially") {
    SoLoudAudioEngine eng;
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }
    CHECK(eng.getPosition("bgm") == 0.0f);
    CHECK(eng.getLength("bgm") == 0.0f);
    CHECK(eng.getPosition("voice") == 0.0f);
    CHECK(eng.getPosition("se") == 0.0f);
}

TEST_CASE("SoLoudAudioEngine::update does not crash") {
    SoLoudAudioEngine eng;
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }
    eng.update(0.016f);
    eng.update(0.0f);
}

TEST_CASE("SoLoudAudioEngine::flushWaveCache does not crash") {
    SoLoudAudioEngine eng;
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }
    eng.playSE("tests/audio/silence.wav");
    eng.flushWaveCache();
    eng.flushWaveCache();  // idempotent
}

TEST_CASE("SoLoudAudioEngine rejects playback when audio handle quota is exhausted") {
    AudioQuota quota(0);
    ScopedAudioQuota scopedQuota(quota);
    SoLoudAudioEngine eng;
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }
    const int baselineVoices = eng.activeVoiceCount();

    CHECK(eng.playBGM("tests/audio/silence.wav", 0.0f) == 0);
    CHECK(eng.playVoice("tests/audio/silence.wav") == 0);
    CHECK(eng.playSE("tests/audio/silence.wav") == 0);
    CHECK(eng.playSE3D("tests/audio/silence.wav", 0.0f, 0.0f, 0.0f) == 0);

    CHECK(quota.tryCalls == 4);
    CHECK(quota.activeCount == 0);
    CHECK(quota.releaseCalls == 0);
    CHECK(quota.unexpectedKinds == 0);
    CHECK(eng.activeVoiceCount() == baselineVoices);
}

TEST_CASE("SoLoudAudioEngine releases reserved quota when SoLoud creation fails") {
    AudioQuota quota(1);
    ScopedAudioQuota scopedQuota(quota);
    SoLoudAudioEngine eng;
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }

    SoLoud::BusInstance* instance = eng.seBus().mInstance;
    REQUIRE(instance != nullptr);
    eng.seBus().mInstance = nullptr;
    CHECK(eng.playSE("tests/audio/silence.wav") == 0);
    eng.seBus().mInstance = instance;

    CHECK(quota.tryCalls == 1);
    CHECK(quota.releaseCalls == 1);
    CHECK(quota.activeCount == 0);
    CHECK(quota.releaseUnderflows == 0);
}

TEST_CASE("SoLoudAudioEngine keeps current BGM when replacement quota is denied") {
    AudioQuota quota(1);
    ScopedAudioQuota scopedQuota(quota);
    SoLoudAudioEngine eng;
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }

    const unsigned int current = eng.playBGM("tests/audio/silence.wav", 0.0f);
    REQUIRE(current != 0);
    eng.soloud().setLooping(current, true);
    const int activeVoices = eng.activeVoiceCount();

    CHECK(eng.playBGM("tests/audio/silence.wav", 0.0f) == 0);
    CHECK(eng.isBGMPlaying());
    CHECK(eng.activeVoiceCount() == activeVoices);
    CHECK(quota.activeCount == 1);
    CHECK(quota.tryCalls == 2);
    CHECK(quota.releaseCalls == 0);

    eng.stopBGM(0.0f);
    CHECK(quota.activeCount == 0);
    CHECK(quota.releaseCalls == 1);
}

TEST_CASE("SoLoudAudioEngine stopSE releases every tracked handle exactly once") {
    AudioQuota quota(8);
    ScopedAudioQuota scopedQuota(quota);
    SoLoudAudioEngine eng{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(eng.init());

    REQUIRE(eng.playSE("tests/audio/silence.wav") != 0);
    mixAudioContractBlock(eng);
    REQUIRE(eng.playSE3D("tests/audio/silence.wav", 0.0f, 0.0f, -1.0f) != 0);
    mixAudioContractBlock(eng);
    REQUIRE(quota.activeCount == 2);

    eng.stopSE();
    CHECK(quota.activeCount == 0);
    CHECK(quota.releaseCalls == 2);
    CHECK(quota.releaseUnderflows == 0);

    eng.stopSE();
    CHECK(quota.releaseCalls == 2);
}

TEST_CASE("SoLoudAudioEngine stopSEHandle releases only a tracked handle") {
    AudioQuota quota(8);
    ScopedAudioQuota scopedQuota(quota);
    SoLoudAudioEngine eng;
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }

    const unsigned int handle = eng.playSE("tests/audio/silence.wav");
    REQUIRE(handle != 0);
    REQUIRE(quota.activeCount == 1);

    eng.stopSEHandle(handle);
    CHECK(quota.activeCount == 0);
    CHECK(quota.releaseCalls == 1);

    eng.stopSEHandle(handle);
    eng.stopSEHandle(0);
    CHECK(quota.releaseCalls == 1);
    CHECK(quota.releaseUnderflows == 0);
}

TEST_CASE("SoLoudAudioEngine update releases naturally finished SE handles") {
    AudioQuota quota(8);
    ScopedAudioQuota scopedQuota(quota);
    SoLoudAudioEngine eng;
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }

    const unsigned int handle = eng.playSE("tests/audio/silence.wav");
    REQUIRE(handle != 0);
    REQUIRE(quota.activeCount == 1);

    eng.soloud().stop(handle);
    REQUIRE_FALSE(eng.soloud().isValidVoiceHandle(handle));
    eng.update(0.0f);

    CHECK(quota.activeCount == 0);
    CHECK(quota.releaseCalls == 1);
    CHECK_FALSE(eng.isSEPlaying());
}

TEST_CASE("SoLoudAudioEngine BGM and voice replacement keep quota counts symmetric") {
    AudioQuota quota(8);
    ScopedAudioQuota scopedQuota(quota);
    SoLoudAudioEngine eng{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(eng.init());

    REQUIRE(eng.playBGM("tests/audio/silence.wav", 0.0f) != 0);
    mixAudioContractBlock(eng);
    REQUIRE(quota.activeCount == 1);
    REQUIRE(eng.playBGM("tests/audio/silence.wav", 0.0f) != 0);
    mixAudioContractBlock(eng);
    CHECK(quota.activeCount == 1);
    CHECK(quota.releaseCalls == 1);
    CHECK(quota.peakCount == 2);
    eng.stopBGM(0.0f);
    CHECK(quota.activeCount == 0);
    CHECK(quota.releaseCalls == 2);

    const unsigned int firstVoice = eng.playVoice("tests/audio/silence.wav");
    REQUIRE(firstVoice != 0);
    eng.soloud().setLooping(firstVoice, true);
    REQUIRE(quota.activeCount == 1);
    const unsigned int secondVoice = eng.playVoice("tests/audio/silence.wav");
    REQUIRE(secondVoice != 0);
    eng.soloud().setLooping(secondVoice, true);
    // Both voices loop before mixing; do not advance the later 50 ms retirement.
    mixAudioContractBlock(eng);
    CHECK(quota.activeCount == 2);
    CHECK(quota.releaseCalls == 2);

    eng.soloud().stop(firstVoice);
    eng.update(0.0f);
    CHECK(quota.activeCount == 1);
    CHECK(quota.releaseCalls == 3);

    eng.stopVoice();
    CHECK(quota.activeCount == 1);
    CHECK(quota.releaseCalls == 3);
    eng.stopVoice();
    CHECK(quota.activeCount == 0);
    CHECK(quota.releaseCalls == 4);
    CHECK(quota.releaseUnderflows == 0);
}

TEST_CASE("SoLoudAudioEngine retiring BGM releases only after physical voice ends") {
    AudioQuota quota(4);
    ScopedAudioQuota scopedQuota(quota);
    SoLoudAudioEngine eng;
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }

    const unsigned int first = eng.playBGM("tests/audio/silence.wav", 0.0f);
    REQUIRE(first != 0);
    eng.soloud().setLooping(first, true);
    const unsigned int second = eng.playBGM("tests/audio/silence.wav", 10.0f);
    REQUIRE(second != 0);
    eng.soloud().setLooping(second, true);

    CHECK(quota.activeCount == 2);
    CHECK(quota.peakCount == 2);
    CHECK(quota.releaseCalls == 0);

    eng.soloud().stop(first);
    eng.update(0.0f);
    CHECK(quota.activeCount == 1);
    CHECK(quota.releaseCalls == 1);

    eng.stopBGM(0.0f);
    CHECK(quota.activeCount == 0);
    CHECK(quota.releaseCalls == 2);
    CHECK(quota.releaseUnderflows == 0);
}

TEST_CASE("SoLoudAudioEngine rapid BGM replacement is capped including retiring voices") {
    AudioQuota quota(3);
    ScopedAudioQuota scopedQuota(quota);
    SoLoudAudioEngine eng;
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }

    const unsigned int first = eng.playBGM("tests/audio/silence.wav", 0.0f);
    REQUIRE(first != 0);
    eng.soloud().setLooping(first, true);
    const unsigned int second = eng.playBGM("tests/audio/silence.wav", 10.0f);
    REQUIRE(second != 0);
    eng.soloud().setLooping(second, true);
    const unsigned int current = eng.playBGM("tests/audio/silence.wav", 10.0f);
    REQUIRE(current != 0);
    eng.soloud().setLooping(current, true);

    REQUIRE(quota.activeCount == 3);
    const int activeVoices = eng.activeVoiceCount();
    CHECK(eng.playBGM("tests/audio/silence.wav", 10.0f) == 0);
    CHECK(eng.isBGMPlaying());
    CHECK(eng.activeVoiceCount() == activeVoices);
    CHECK(quota.activeCount == 3);
    CHECK(quota.tryCalls == 4);
    CHECK(quota.releaseCalls == 0);

    eng.stopBGM(10.0f);
    CHECK_FALSE(eng.soloud().isValidVoiceHandle(first));
    CHECK_FALSE(eng.soloud().isValidVoiceHandle(second));
    CHECK(eng.soloud().isValidVoiceHandle(current));
    CHECK(quota.activeCount == 1);
    CHECK(quota.releaseCalls == 2);

    eng.stopBGM(10.0f);
    CHECK_FALSE(eng.soloud().isValidVoiceHandle(current));
    CHECK(quota.activeCount == 0);
    CHECK(quota.releaseCalls == 3);
    CHECK(quota.releaseUnderflows == 0);
}

TEST_CASE("SoLoudAudioEngine shutdown releases all remaining handle quotas once") {
    AudioQuota quota(8);
    ScopedAudioQuota scopedQuota(quota);
    SoLoudAudioEngine eng{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(eng.init());

    const unsigned int firstBGM = eng.playBGM("tests/audio/silence.wav", 0.0f);
    REQUIRE(firstBGM != 0);
    eng.soloud().setLooping(firstBGM, true);
    const unsigned int currentBGM = eng.playBGM("tests/audio/silence.wav", 10.0f);
    REQUIRE(currentBGM != 0);
    eng.soloud().setLooping(currentBGM, true);

    const unsigned int firstVoice = eng.playVoice("tests/audio/silence.wav");
    REQUIRE(firstVoice != 0);
    eng.soloud().setLooping(firstVoice, true);
    const unsigned int currentVoice = eng.playVoice("tests/audio/silence.wav");
    REQUIRE(currentVoice != 0);
    eng.soloud().setLooping(currentVoice, true);

    REQUIRE(eng.playSE("tests/audio/silence.wav") != 0);
    REQUIRE(eng.playSE3D("tests/audio/silence.wav", 0.0f, 0.0f, -1.0f) != 0);
    // One shared block preserves all six owners and the retiring BGM.
    mixAudioContractBlock(eng);
    REQUIRE(quota.activeCount == 6);

    eng.shutdown();
    CHECK(quota.activeCount == 0);
    CHECK(quota.releaseCalls == 6);
    CHECK(quota.releaseUnderflows == 0);

    eng.shutdown();
    CHECK(quota.releaseCalls == 6);
}


TEST_CASE("SoLoudAudioEngine playRawPCM plays and stops cleanly") {
    SoLoudAudioEngine eng{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(eng.init());
    // init() starts the three bus voices; baseline counts them.
    const int baseline = static_cast<int>(eng.activeVoiceCount());
    CHECK(baseline >= 3);

    // 0.25s of 440Hz sine at 44.1kHz stereo (interleaved float)
    const unsigned int sr = 44100;
    const unsigned int frames = sr / 4;
    std::vector<float> pcm(frames * 2);
    for (unsigned int i = 0; i < frames; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(sr);
        const float v = 0.25f * std::sin(2.0f * 3.14159265f * 440.0f * t);
        pcm[i * 2] = v;
        pcm[i * 2 + 1] = v;
    }

    const unsigned int h = eng.playRawPCM(pcm.data(), frames, sr, 2);
    REQUIRE(h != 0);
    mixAudioContractBlock(eng);
    CHECK(static_cast<int>(eng.activeVoiceCount()) > baseline);

    // Invalid parameters are rejected without crashing.
    CHECK(eng.playRawPCM(nullptr, 100, sr, 2) == 0);
    CHECK(eng.playRawPCM(pcm.data(), 0, sr, 2) == 0);
    CHECK(eng.playRawPCM(pcm.data(), 100, 0, 2) == 0);
    CHECK(eng.playRawPCM(pcm.data(), 100, sr, 3) == 0);

    eng.stopSEHandle(h);
    eng.shutdown();
    CHECK(eng.activeVoiceCount() == 0);
}

TEST_CASE("Audio: voice pool API safe before init") {
    SoLoudAudioEngine eng;
    // Uninitialized paths must not crash and return empty results.
    CHECK(eng.playVoice("tests/audio/silence.wav") == 0);
    eng.stopVoice();  // no-op, no crash
    CHECK(eng.isVoicePlaying() == false);
    eng.shutdown();   // idempotent
}

TEST_CASE("Audio: suspend/resume lifecycle contract (round 29)") {
    SoLoudAudioEngine eng;
    // Before init: suspend/resume must be safe no-ops.
    eng.suspend();
    eng.resume();
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }
    // After init: suspend/resume must not crash and must be repeatable.
    eng.suspend();
    eng.suspend();
    eng.resume();
    eng.resume();
    eng.update(0.0f);
    eng.shutdown();
    // After shutdown: still safe.
    eng.suspend();
    eng.resume();
}

// =============================================================================
// G10 audio module boundary tests
// =============================================================================

TEST_CASE("SoLoudAudioEngine BGM and SE buses are independent") {
    SoLoudAudioEngine eng{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(eng.init());

    const unsigned int bgm = eng.playBGM("tests/audio/silence.wav", 0.0f);
    REQUIRE(bgm != 0);
    eng.soloud().setLooping(bgm, true);
    REQUIRE(eng.isBGMPlaying());

    // Playing an SE must not disturb the BGM bus.
    const unsigned int se = eng.playSE("tests/audio/silence.wav");
    REQUIRE(se != 0);
    mixAudioContractBlock(eng);
    CHECK(eng.isSEPlaying());
    CHECK(eng.isBGMPlaying());          // BGM still playing after SE starts

    // Stopping the SE must not stop the BGM.
    eng.stopSE();
    CHECK_FALSE(eng.isSEPlaying());
    CHECK(eng.isBGMPlaying());

    // Reverse: play SE, then BGM; stopping the BGM leaves the SE untouched.
    const unsigned int se2 = eng.playSE("tests/audio/silence.wav");
    REQUIRE(se2 != 0);
    const unsigned int bgm2 = eng.playBGM("tests/audio/silence.wav", 0.0f);
    REQUIRE(bgm2 != 0);
    eng.soloud().setLooping(bgm2, true);
    mixAudioContractBlock(eng);
    CHECK(eng.isSEPlaying());
    eng.stopBGM(0.0f);
    CHECK_FALSE(eng.isBGMPlaying());
    CHECK(eng.isSEPlaying());           // SE survived the BGM stop

    eng.stopSE();
}

TEST_CASE("SoLoudAudioEngine voice keeps overlapping characters (pool, no single-slot kill)") {
    SoLoudAudioEngine eng{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(eng.init());

    // The engine uses a round-robin 4-slot voice pool: a new voice does NOT
    // hard-stop the previous one (single-slot semantics were removed). Both
    // handles coexist in the pool and stay valid independently.
    const unsigned int v1 = eng.playVoice("tests/audio/silence.wav");
    REQUIRE(v1 != 0);
    // Set looping before advancing the mixer so the 100 ms fixture cannot
    // naturally finish between playVoice() and setLooping().
    eng.soloud().setLooping(v1, true);
    mixAudioContractBlock(eng);
    CHECK(eng.isVoicePlaying());

    const unsigned int v2 = eng.playVoice("tests/audio/silence.wav");
    REQUIRE(v2 != 0);
    eng.soloud().setLooping(v2, true);
    mixAudioContractBlock(eng);
    CHECK(eng.isVoicePlaying());
    CHECK(eng.soloud().isValidVoiceHandle(v1));  // first voice still alive
    CHECK(eng.soloud().isValidVoiceHandle(v2));

    // stopVoice() clears the whole pool; only NATURAL ends count as completions.
    eng.stopVoice();
    CHECK_FALSE(eng.isVoicePlaying());
    CHECK(eng.consumeVoiceCompletions() == 0);
}

TEST_CASE("SoLoudAudioEngine bus volume applies to the bus (documented approximation)") {
    SoLoudAudioEngine eng{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(eng.init());

    // The engine exposes the configured bus volume as the authoritative truth;
    // a per-handle getSEVolume() does NOT include the bus multiplier, so the
    // effective (bus x handle) volume is not directly readable through
    // IAudioBackend. We assert the documented contract: the bus volume is
    // applied and readable via getBusVolume(), and samples still play.
    const float original = eng.getBusVolume("se");
    eng.setBusVolume("se", 0.35f);
    CHECK(eng.getBusVolume("se") == doctest::Approx(0.35f));

    const unsigned int h = eng.playSE("tests/audio/silence.wav");
    REQUIRE(h != 0);
    mixAudioContractBlock(eng);
    CHECK(eng.isSEPlaying());

    // fadeVolume() keeps the persisted bus-volume state consistent.
    eng.fadeVolume("se", 0.7f, 0.1f);
    CHECK(eng.getBusVolume("se") == doctest::Approx(0.7f));

    eng.stopSEHandle(h);
    eng.setBusVolume("se", original);
    CHECK(eng.getBusVolume("se") == doctest::Approx(original));
}

TEST_CASE("SoLoudAudioEngine empty/invalid handles are graceful no-ops") {
    SoLoudAudioEngine eng;
    if (!eng.init()) { MESSAGE("Audio device unavailable, skipping"); return; }

    eng.setSEVolume(0, 0.5f);
    CHECK(eng.getSEVolume(0) == 0.0f);
    eng.setSEVolume(99999u, 0.5f);   // unknown handle: no-op, must not crash
    eng.stopSEHandle(0);
    eng.stopSEHandle(1u << 30);      // not a tracked handle

    // Playing an empty file yields handle 0 and leaves SE idle.
    CHECK(eng.playSE("") == 0);
    CHECK_FALSE(eng.isSEPlaying());

    // stopVoice with nothing playing is a safe no-op.
    eng.stopVoice();
    CHECK_FALSE(eng.isVoicePlaying());
    CHECK(eng.consumeVoiceCompletions() == 0);
}

TEST_CASE("SoLoudAudioEngine stopping all three buses clears every bus") {
    SoLoudAudioEngine eng{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(eng.init());

    const unsigned int bgm = eng.playBGM("tests/audio/silence.wav", 0.0f);
    REQUIRE(bgm != 0);
    eng.soloud().setLooping(bgm, true);
    const unsigned int voice = eng.playVoice("tests/audio/silence.wav");
    REQUIRE(voice != 0);
    eng.soloud().setLooping(voice, true);
    REQUIRE(eng.playSE("tests/audio/silence.wav") != 0);
    mixAudioContractBlock(eng);

    CHECK(eng.isBGMPlaying());
    CHECK(eng.isVoicePlaying());
    CHECK(eng.isSEPlaying());

    // IAudioBackend has no single stopAll(); stopping each bus is the public
    // contract for clearing all playback.
    eng.stopBGM(0.0f);
    eng.stopVoice();
    eng.stopSE();

    CHECK_FALSE(eng.isBGMPlaying());
    CHECK_FALSE(eng.isVoicePlaying());
    CHECK_FALSE(eng.isSEPlaying());
    CHECK(eng.consumeVoiceCompletions() == 0);  // explicit stop != natural end
}

TEST_CASE("SoLoudAudioEngine suspend pauses without dropping handles (app pause)") {
    SoLoudAudioEngine eng{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(eng.init());

    const unsigned int bgm = eng.playBGM("tests/audio/silence.wav", 0.0f);
    REQUIRE(bgm != 0);
    eng.soloud().setLooping(bgm, true);
    const unsigned int se = eng.playSE("tests/audio/silence.wav");
    REQUIRE(se != 0);
    // Three blocks total 32 ms; the middle block exercises the paused mixer.
    mixAudioContractBlock(eng);

    // suspend() pauses the whole mixer; tracked handles stay alive so a
    // subsequent resume() continues them.
    eng.suspend();
    mixAudioContractBlock(eng);
    CHECK(eng.isBGMPlaying());
    CHECK(eng.isSEPlaying());
    CHECK(eng.soloud().isValidVoiceHandle(bgm));
    CHECK(eng.soloud().isValidVoiceHandle(se));

    eng.resume();
    eng.update(0.0f);
    mixAudioContractBlock(eng);
    CHECK(eng.isBGMPlaying());
    CHECK(eng.isSEPlaying());

    eng.stopBGM(0.0f);
    eng.stopSE();
}

TEST_CASE("SoLoudAudioEngine 3D position routing left/center/right yields valid handles") {
    SoLoudAudioEngine eng{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(eng.init());

    // IAudioBackend exposes no direct pan (left/center/right) control; the
    // spatial routing it does expose is 3D positioning via playSE3D().
    const unsigned int left   = eng.playSE3D("tests/audio/silence.wav", -10.0f, 0.0f, 0.0f);
    const unsigned int center = eng.playSE3D("tests/audio/silence.wav",   0.0f, 0.0f, 0.0f);
    const unsigned int right  = eng.playSE3D("tests/audio/silence.wav",  10.0f, 0.0f, 0.0f);
    CHECK(left != 0);
    CHECK(center != 0);
    CHECK(right != 0);
    mixAudioContractBlock(eng);
    CHECK(eng.isSEPlaying());

    // Listener at origin facing +x; the 3D mix update must run without crashing.
    eng.update3dListener(0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
    eng.update(0.0f);

    eng.stopSE();
}

TEST_CASE("SoLoudAudioEngine has no mute toggle; bus volume 0 persists as mute approximation") {
    SoLoudAudioEngine eng;
    if (!eng.init()) { MESSAGE("Audio device unavailable, skipping"); return; }

    // IAudioBackend exposes no mute()/unmute(); the closest documented
    // mechanism is setting a global/bus volume to 0, which persists.
    eng.setGlobalVolume(0.0f);
    CHECK(eng.getGlobalVolume() == doctest::Approx(0.0f));
    eng.setGlobalVolume(1.0f);
    CHECK(eng.getGlobalVolume() == doctest::Approx(1.0f));

    eng.setBusVolume("voice", 0.0f);
    CHECK(eng.getBusVolume("voice") == doctest::Approx(0.0f));
    eng.setBusVolume("voice", 1.0f);
    CHECK(eng.getBusVolume("voice") == doctest::Approx(1.0f));
}

// -----------------------------------------------------------------------------
// G10 follow-up: bus volume pre-init consistency (round-77 audit)
// setBusVolume must store its value before init() and apply it at init(),
// matching setGlobalVolume's documented init-time application pattern.
// -----------------------------------------------------------------------------

TEST_CASE("SoLoudAudioEngine setBusVolume before init is applied at init") {
    SoLoudAudioEngine eng;
    // Configure a bus BEFORE the backend is initialized. Historically this was
    // silently dropped (setBusVolume early-returned when !m_initialized); it
    // must now be stored and applied when init() starts the buses.
    eng.setBusVolume("bgm", 0.42f);
    CHECK(eng.getBusVolume("bgm") == doctest::Approx(0.42f));  // stored pending

    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        eng.shutdown();
        return;
    }

    // The SoLoud BGM bus must actually carry the pre-init value (not 1.0f).
    // AudioSource::mVolume is the default volume init() applied via setVolume.
    CHECK(eng.bgmBus().mVolume == doctest::Approx(0.42f));
    // And the persisted getter agrees.
    CHECK(eng.getBusVolume("bgm") == doctest::Approx(0.42f));

    eng.shutdown();
}

TEST_CASE("SoLoudAudioEngine setGlobalVolume before init still applies at init (regression)") {
    SoLoudAudioEngine eng;
    // setGlobalVolume is the established init-time-application pattern: it
    // stores pre-init and applies the stored value inside init(). This
    // regression guard ensures it keeps working unchanged.
    eng.setGlobalVolume(0.25f);
    CHECK(!eng.isBGMPlaying());  // still pre-init
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        eng.shutdown();
        return;
    }
    CHECK(eng.getGlobalVolume() == doctest::Approx(0.25f));

    // Verify SoLoud's global volume was actually applied at init.
    CHECK(eng.soloud().getGlobalVolume() == doctest::Approx(0.25f));
    eng.shutdown();
}

TEST_CASE("SoLoudAudioEngine setBusVolume after init applies immediately (regression)") {
    SoLoudAudioEngine eng;
    if (!eng.init()) { MESSAGE("Audio device unavailable, skipping"); return; }

    // Live-path semantics unchanged: an after-init call must push the volume
    // straight through to the SoLoud bus and update the persisted getter.
    const float original = eng.getBusVolume("voice");
    eng.setBusVolume("voice", 0.63f);
    CHECK(eng.getBusVolume("voice") == doctest::Approx(0.63f));
    CHECK(eng.voiceBus().mVolume == doctest::Approx(0.63f));

    eng.setBusVolume("voice", original);
    CHECK(eng.getBusVolume("voice") == doctest::Approx(original));
    eng.shutdown();
}

TEST_CASE("SoLoudAudioEngine setBusVolume(0) mutes that bus") {
    SoLoudAudioEngine eng{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(eng.init());

    // A zero bus volume is the documented mute approximation: it persists and
    // drives the SoLoud bus to silence.
    eng.setBusVolume("se", 0.0f);
    CHECK(eng.getBusVolume("se") == doctest::Approx(0.0f));
    CHECK(eng.seBus().mVolume == doctest::Approx(0.0f));

    // Sanity: the SE bus still plays samples (muted bus does not block play).
    const unsigned int h = eng.playSE("tests/audio/silence.wav");
    REQUIRE(h != 0);
    mixAudioContractBlock(eng);
    CHECK(eng.isSEPlaying());
    eng.stopSEHandle(h);

    eng.setBusVolume("se", 1.0f);
    CHECK(eng.getBusVolume("se") == doctest::Approx(1.0f));
    eng.shutdown();
}

// =============================================================================
// Round-2 audio boundary tests (round 77/78 follow-up)
// Covers: SE concurrency (no built-in rotation), voice pool rotation cap,
// stop-then-replay of the same handle, same-file dedup semantics, fade
// interruption, the global x bus x handle volume chain, global-volume after
// playback, and bus-volume persistence across suspend/resume.
// Semantics below are locked against SoLoudAudioEngine.cpp as implemented.
// =============================================================================

TEST_CASE("SoLoudAudioEngine multiple SE on one bus coexist (no built-in rotation)") {
    SoLoudAudioEngine eng{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(eng.init());

    // SE has NO fixed pool: playSE() appends every live handle to m_activeSE,
    // so concurrent SE instances on the same bus overlap instead of evicting.
    // Playing the SAME file three times must yield three distinct live voices
    // (the wave-cache dedups the SOURCE, not the voices).
    const unsigned int s1 = eng.playSE("tests/audio/silence.wav");
    REQUIRE(s1 != 0);
    const unsigned int s2 = eng.playSE("tests/audio/silence.wav");
    REQUIRE(s2 != 0);
    const unsigned int s3 = eng.playSE("tests/audio/silence.wav");
    REQUIRE(s3 != 0);
    mixAudioContractBlock(eng);

    // All three handles are independently live on the same bus.
    CHECK(eng.soloud().isValidVoiceHandle(s1));
    CHECK(eng.soloud().isValidVoiceHandle(s2));
    CHECK(eng.soloud().isValidVoiceHandle(s3));
    CHECK(eng.isSEPlaying());

    // Stopping ONE handle leaves the other two playing.
    eng.stopSEHandle(s2);
    CHECK_FALSE(eng.soloud().isValidVoiceHandle(s2));
    CHECK(eng.isSEPlaying());
    CHECK(eng.soloud().isValidVoiceHandle(s1));
    CHECK(eng.soloud().isValidVoiceHandle(s3));

    eng.stopSE();
    CHECK_FALSE(eng.isSEPlaying());
}

TEST_CASE("SoLoudAudioEngine voice pool rotation caps at pool+one retiring slot") {
    // The VN voice pool is a round-robin 4-slot array; each slot displaced by a
    // new voice is retired (fade 0.05s) into m_retiringVoice. So 4 pool slots +
    // up to 1 in-flight retiring voice == 5 concurrent allocations in the
    // steady rotation; a 6th must be rejected by the handle quota.
    AudioQuota quota(5);
    ScopedAudioQuota scopedQuota(quota);
    SoLoudAudioEngine eng{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(eng.init());

    const unsigned int v0 = eng.playVoice("tests/audio/silence.wav");
    REQUIRE(v0 != 0);
    eng.soloud().setLooping(v0, true);
    const unsigned int v1 = eng.playVoice("tests/audio/silence.wav");
    REQUIRE(v1 != 0);
    eng.soloud().setLooping(v1, true);
    const unsigned int v2 = eng.playVoice("tests/audio/silence.wav");
    REQUIRE(v2 != 0);
    eng.soloud().setLooping(v2, true);
    const unsigned int v3 = eng.playVoice("tests/audio/silence.wav");
    REQUIRE(v3 != 0);
    eng.soloud().setLooping(v3, true);
    // Pool full (4 slots). The 5th play displaces the oldest slot into the
    // retiring list, so allocations rise to 5 (4 pool + 1 retiring).
    const unsigned int v4 = eng.playVoice("tests/audio/silence.wav");
    REQUIRE(v4 != 0);
    eng.soloud().setLooping(v4, true);
    CHECK(quota.activeCount == 5);
    CHECK(quota.tryCalls == 5);

    // 6th play: quota exhausted -> rejected, nothing new plays.
    CHECK(eng.playVoice("tests/audio/silence.wav") == 0);
    CHECK(quota.activeCount == 5);

    // The displaced first handle is no longer in the live pool but the rest stay.
    eng.soloud().setLooping(v4, true);
    CHECK(eng.isVoicePlaying());

    // Only the explicitly advanced mixer clock may complete the 50 ms
    // retirement. 4096 frames at 48 kHz exceed that fade; all live pool
    // voices loop, so only v0 is reclaimed and a new admission must succeed.
    for (unsigned block = 0; block != 8; ++block) mixAudioContractBlock(eng);
    eng.update(0.0f);
    CHECK_FALSE(eng.soloud().isValidVoiceHandle(v0));
    CHECK(eng.soloud().isValidVoiceHandle(v1));
    CHECK(eng.soloud().isValidVoiceHandle(v2));
    CHECK(eng.soloud().isValidVoiceHandle(v3));
    CHECK(eng.soloud().isValidVoiceHandle(v4));
    CHECK(quota.activeCount == 4);
    CHECK(quota.releaseCalls == 1);
    const unsigned int retried = eng.playVoice("tests/audio/silence.wav");
    REQUIRE(retried != 0);
    eng.soloud().setLooping(retried, true);
    CHECK(eng.soloud().isValidVoiceHandle(retried));
    CHECK(quota.activeCount == 5);

    eng.stopVoice();
    eng.update(0.0f);
    eng.shutdown();
    CHECK(quota.activeCount == 0);
    CHECK(quota.releaseUnderflows == 0);
}

TEST_CASE("SoLoudAudioEngine stop then immediately replay same handle is clean") {
    AudioQuota quota(8);
    ScopedAudioQuota scopedQuota(quota);
    SoLoudAudioEngine eng{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(eng.init());

    const unsigned int first = eng.playSE("tests/audio/silence.wav");
    REQUIRE(first != 0);
    mixAudioContractBlock(eng);
    REQUIRE(quota.activeCount == 1);

    // stopSEHandle frees the slot and its quota immediately.
    eng.stopSEHandle(first);
    CHECK_FALSE(eng.soloud().isValidVoiceHandle(first));
    CHECK(quota.activeCount == 0);

    // Replaying the same FILE (same handle value pool) right after must give a
    // fresh, live handle and re-take the quota exactly once.
    const unsigned int again = eng.playSE("tests/audio/silence.wav");
    REQUIRE(again != 0);
    mixAudioContractBlock(eng);
    CHECK(eng.soloud().isValidVoiceHandle(again));
    CHECK(quota.activeCount == 1);
    // Old handle remains dead.
    CHECK_FALSE(eng.soloud().isValidVoiceHandle(first));

    eng.stopSE();
    CHECK(quota.activeCount == 0);
    CHECK(quota.releaseUnderflows == 0);
}

TEST_CASE("SoLoudAudioEngine same-file SE plays overlay rather than dedupe") {
    // The wave cache dedupes the underlying source (one load, one shared_ptr),
    // but each playSE() creates a NEW SoLoud voice. Contract locked: repeating
    // the same file plays concurrently (no single-instance dedup/overwrite).
    SoLoudAudioEngine eng{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(eng.init());

    const unsigned int a = eng.playSE("tests/audio/silence.wav");
    REQUIRE(a != 0);
    const unsigned int b = eng.playSE("tests/audio/silence.wav");
    REQUIRE(b != 0);
    mixAudioContractBlock(eng);

    CHECK(eng.soloud().isValidVoiceHandle(a));
    CHECK(eng.soloud().isValidVoiceHandle(b));
    CHECK_NE(a, b);  // distinct live voices for the same file

    eng.stopSE();
}

TEST_CASE("SoLoudAudioEngine fade interruption: later fade replaces the target") {
    SoLoudAudioEngine eng;
    if (!eng.init()) { MESSAGE("Audio device unavailable, skipping"); return; }

    // fadeVolume() records the target in the persisted bus state and starts a
    // SoLoud fade. A second fadeVolume() before the first completes must
    // override the target (the persisted state always matches the latest call)
    // without crashing or leaving the bus in a stale pending state.
    eng.setBusVolume("se", 0.9f);
    eng.fadeVolume("se", 0.3f, 5.0f);   // long fade, interrupted immediately
    CHECK(eng.getBusVolume("se") == doctest::Approx(0.3f));

    eng.fadeVolume("se", 0.7f, 0.0f);   // override; 0s jump
    CHECK(eng.getBusVolume("se") == doctest::Approx(0.7f));

    // Interrupt a fade with a direct setBusVolume: set wins immediately.
    eng.fadeVolume("se", 0.1f, 3.0f);
    eng.setBusVolume("se", 0.55f);
    CHECK(eng.getBusVolume("se") == doctest::Approx(0.55f));
    CHECK(eng.seBus().mVolume == doctest::Approx(0.55f));

    // A fade on one bus leaves the other bus volumes untouched.
    eng.setBusVolume("bgm", 0.9f);
    eng.fadeVolume("voice", 0.4f, 2.0f);
    CHECK(eng.getBusVolume("bgm") == doctest::Approx(0.9f));
    CHECK(eng.getBusVolume("voice") == doctest::Approx(0.4f));

    eng.shutdown();
}

TEST_CASE("SoLoudAudioEngine global x bus x handle volume chain applies per level") {
    SoLoudAudioEngine eng;
    if (!eng.init()) { MESSAGE("Audio device unavailable, skipping"); return; }

    // The engine applies volume at three independent levels (global -> bus ->
    // handle); IAudioBackend does not expose the composed product, so we read
    // the SoLoud-level state of each stage and lock that the chain holds.
    eng.setGlobalVolume(0.8f);
    eng.setBusVolume("bgm", 0.5f);
    const unsigned int h = eng.playBGM("tests/audio/silence.wav", 0.0f);  // 0s fade => handle at 1.0
    REQUIRE(h != 0);
    if (eng.soloud().isValidVoiceHandle(h))
        CHECK(eng.soloud().getVolume(h) == doctest::Approx(1.0f));

    CHECK(eng.soloud().getGlobalVolume() == doctest::Approx(0.8f));
    CHECK(eng.bgmBus().mVolume == doctest::Approx(0.5f));
    // Composed chain value (readable stages): 0.8 * 0.5 * 1.0.
    CHECK(eng.soloud().getGlobalVolume() * eng.bgmBus().mVolume
          == doctest::Approx(0.4f));

    eng.shutdown();
}

TEST_CASE("SoLoudAudioEngine setGlobalVolume takes effect after playback") {
    AudioQuota quota(8);
    ScopedAudioQuota scopedQuota(quota);
    SoLoudAudioEngine eng{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(eng.init());

    const unsigned int se = eng.playSE("tests/audio/silence.wav");
    REQUIRE(se != 0);
    REQUIRE(quota.activeCount == 1);
    mixAudioContractBlock(eng);
    const int voicesAfterPlay = eng.activeVoiceCount();

    // Lowering the global volume AFTER a voice is live must apply live (SoLoud
    // global volume scales every active voice) and must not kill the handle.
    eng.setGlobalVolume(0.25f);
    CHECK(eng.soloud().getGlobalVolume() == doctest::Approx(0.25f));
    CHECK(eng.soloud().isValidVoiceHandle(se));
    CHECK(eng.isSEPlaying());
    CHECK(eng.activeVoiceCount() == voicesAfterPlay);

    // Further lowering to mute keeps the voice alive (mute approximation).
    eng.setGlobalVolume(0.0f);
    CHECK(eng.soloud().getGlobalVolume() == doctest::Approx(0.0f));
    CHECK(eng.soloud().isValidVoiceHandle(se));
    CHECK(eng.isSEPlaying());

    eng.stopSE();
    eng.setGlobalVolume(1.0f);
    eng.shutdown();
    CHECK(quota.activeCount == 0);
}

TEST_CASE("SoLoudAudioEngine setBusVolume during suspend persists across resume") {
    SoLoudAudioEngine eng;
    if (!eng.init()) { MESSAGE("Audio device unavailable, skipping"); return; }

    const unsigned int bgm = eng.playBGM("tests/audio/silence.wav", 0.0f);
    REQUIRE(bgm != 0);
    eng.soloud().setLooping(bgm, true);
    const float before = eng.getBusVolume("bgm");

    // suspend() pauses the mixer but does NOT tear down volume state; a
    // setBusVolume() made while suspended must persist and survive resume().
    eng.suspend();
    CHECK(eng.soloud().isValidVoiceHandle(bgm));  // handle kept across suspend

    eng.setBusVolume("bgm", 0.22f);
    CHECK(eng.getBusVolume("bgm") == doctest::Approx(0.22f));
    CHECK(eng.bgmBus().mVolume == doctest::Approx(0.22f));

    eng.resume();
    eng.update(0.0f);
    CHECK(eng.getBusVolume("bgm") == doctest::Approx(0.22f));   // value survived resume
    CHECK(eng.bgmBus().mVolume == doctest::Approx(0.22f));
    CHECK(eng.soloud().isValidVoiceHandle(bgm));
    CHECK(eng.isBGMPlaying());

    eng.setBusVolume("bgm", before);
    eng.stopBGM(0.0f);
    eng.shutdown();
}

// =============================================================================
// Mixed audio + job (round-2)
// IAudioBackend methods are guarded by CAESURA_ASSERT_MAIN_THREAD(): they may
// only be called from the main thread. The JobSystem onComplete callback runs
// on the main thread (via pollMainThreadJobs), so it is a valid, safe place to
// drive audio. Driving audio directly from a job WORKER is out of contract
// (main-thread assertion) and is documented, not exercised here.
// =============================================================================

TEST_CASE("Mixed: audio playable from a job onComplete (main thread)") {
    AudioQuota quota(8);
    ScopedAudioQuota scopedQuota(quota);
    SoLoudAudioEngine eng;
    if (!eng.init()) { MESSAGE("Audio device unavailable, skipping"); return; }

    JobSystem js;
    js.init();
    REQUIRE(js.isRunning());

    std::atomic<bool> workerDone{false};
    std::atomic<bool> sePlayed{false};
    std::atomic<bool> seDone{false};

    // The job's onComplete runs on the main thread while draining the callback
    // queue, so playing an SE here is contract-safe and must allocate a handle.
    js.submit(
        [&]() { workerDone.store(true); },
        JobPriority::Normal,
        [&]() {
            const unsigned int h = eng.playSE("tests/audio/silence.wav");
            if (h != 0) {
                sePlayed.store(true);
                eng.stopSEHandle(h);  // release cleanly on the main thread
                seDone.store(true);
            }
        });

    for (int i = 0; i < 200 && !sePlayed.load(); ++i) {
        js.waitIdle();
        js.pollMainThreadJobs();  // drain the onComplete (main thread)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    CHECK(workerDone.load());
    CHECK(sePlayed.load());     // an SE was actually started from onComplete
    CHECK(seDone.load());
    CHECK(quota.activeCount == 0);  // handle released within the callback

    js.shutdown();
    eng.shutdown();
}


TEST_CASE("SoLoudAudioEngine survives rapid init/play/stop/shutdown cycles") {
    // Regression for the deinit() flag race: deinit asserted
    // !mInsideAudioThreadMutex spuriously when the audio thread acquired
    // the mix lock between deinit's transient lock-drain and the backend
    // cleanup join (SIGABRT on headless ALSA-null). 40 cycles with a real
    // play/stop pair keep the audio thread hot without being slow.
    for (int iter = 0; iter < 40; ++iter) {
        SoLoudAudioEngine eng;
        if (!eng.init()) {
            MESSAGE("Audio device unavailable, skipping");
            return;
        }
        const unsigned int h = eng.playVoice("tests/audio/silence.wav");
        if (h) {
            eng.soloud().setLooping(h, true);
            eng.soloud().stop(h);
        }
        eng.update(0.0f);
        eng.shutdown();
    }
    CHECK(true);
}

// =============================================================================
// Track P5 — Audio Focus Service (IAudioFocusService)
// =============================================================================

namespace {
struct FocusProbe : IAudioFocusListener {
    AudioFocusEvent events[8] = {};
    int count = 0;
    void onAudioFocusEvent(AudioFocusEvent event) override {
        if (count < 8) events[count++] = event;
    }
};
}

TEST_CASE("AudioFocus: state machine transitions on post") {
    AudioFocusService svc;
    CHECK(svc.currentState() == AudioFocusState::Normal);
    svc.post(AudioFocusEvent::InterruptionBegin);
    CHECK(svc.currentState() == AudioFocusState::Interrupted);
    svc.post(AudioFocusEvent::InterruptionEnd);
    CHECK(svc.currentState() == AudioFocusState::Normal);
    svc.post(AudioFocusEvent::FocusLost);
    CHECK(svc.currentState() == AudioFocusState::Lost);
    svc.post(AudioFocusEvent::FocusGained);
    CHECK(svc.currentState() == AudioFocusState::Normal);
}

TEST_CASE("AudioFocus: interruption end without begin keeps state") {
    AudioFocusService svc;
    svc.post(AudioFocusEvent::InterruptionEnd);
    CHECK(svc.currentState() == AudioFocusState::Normal);
    svc.post(AudioFocusEvent::FocusGained);
    CHECK(svc.currentState() == AudioFocusState::Normal);
}

TEST_CASE("AudioFocus: listeners receive events in order; duplicates ignored") {
    AudioFocusService svc;
    FocusProbe a, b;
    svc.addListener(&a);
    svc.addListener(&b);
    svc.addListener(&a);
    svc.post(AudioFocusEvent::InterruptionBegin);
    svc.post(AudioFocusEvent::InterruptionEnd);
    CHECK(a.count == 2);
    CHECK(b.count == 2);
    CHECK(a.events[0] == AudioFocusEvent::InterruptionBegin);
    CHECK(b.events[1] == AudioFocusEvent::InterruptionEnd);
}

TEST_CASE("AudioFocus: remove stops delivery; null-safe hub") {
    AudioFocusService svc;
    FocusProbe a;
    svc.addListener(&a);
    svc.post(AudioFocusEvent::FocusLost);
    CHECK(a.count == 1);
    svc.removeListener(&a);
    svc.post(AudioFocusEvent::FocusGained);
    CHECK(a.count == 1);
    svc.removeListener(nullptr);
    CHECK_NOTHROW(svc.post(AudioFocusEvent::FocusLost));
    CHECK(svc.currentState() == AudioFocusState::Lost);
}

TEST_CASE("AudioFocus: interface upcast") {
    AudioFocusService svc;
    IAudioFocusService* iface = &svc;
    REQUIRE(iface != nullptr);
    FocusProbe probe;
    iface->addListener(&probe);
    iface->post(AudioFocusEvent::InterruptionBegin);
    CHECK(probe.count == 1);
    CHECK(iface->currentState() == AudioFocusState::Interrupted);
}


TEST_CASE("U17 AudioFocus: independent lost and interrupted reasons survive interleaving") {
    Caesura::AudioFocusService focus;
    using Event = Caesura::AudioFocusEvent;
    using State = Caesura::AudioFocusState;
    focus.post(Event::FocusLost);
    CHECK(focus.currentState() == State::Lost);
    focus.post(Event::InterruptionBegin);
    CHECK(focus.currentState() == State::Interrupted);
    focus.post(Event::FocusGained);
    CHECK(focus.currentState() == State::Interrupted);
    focus.post(Event::InterruptionEnd);
    CHECK(focus.currentState() == State::Normal);

    focus.post(Event::InterruptionBegin);
    focus.post(Event::FocusLost);
    CHECK(focus.currentState() == State::Interrupted);
    focus.post(Event::InterruptionEnd);
    CHECK(focus.currentState() == State::Lost);
    focus.post(Event::InterruptionEnd);
    CHECK(focus.currentState() == State::Lost);
    focus.post(Event::FocusGained);
    CHECK(focus.currentState() == State::Normal);
}

namespace {
void u27CheckAudioIdle(const AudioBackendSnapshot& state, bool running,
                       uint64_t buses, uint64_t waves, AudioOutputMode mode) {
    CHECK(state.supported);
    CHECK(state.running == running);
    CHECK(state.outputMode == mode);
    CHECK(state.liveVoices == 0);
    CHECK(state.busVoices == buses);
    CHECK(state.sessionHandles == 0);
    CHECK(state.retiringBGM == 0);
    CHECK(state.retiringVoice == 0);
    CHECK(state.waveCacheEntries == waves);
    CHECK(state.rawCacheEntries == 0);
    CHECK(state.voiceCompletionsPending == 0);
    CHECK(state.restoredSources == 0);
}

void u27CheckSameAudioSnapshot(const AudioBackendSnapshot& first,
                              const AudioBackendSnapshot& second) {
    CHECK(first.supported == second.supported);
    CHECK(first.running == second.running);
    CHECK(first.outputMode == second.outputMode);
    CHECK(first.liveVoices == second.liveVoices);
    CHECK(first.busVoices == second.busVoices);
    CHECK(first.sessionHandles == second.sessionHandles);
    CHECK(first.retiringBGM == second.retiringBGM);
    CHECK(first.retiringVoice == second.retiringVoice);
    CHECK(first.waveCacheEntries == second.waveCacheEntries);
    CHECK(first.rawCacheEntries == second.rawCacheEntries);
    CHECK(first.voiceCompletionsPending == second.voiceCompletionsPending);
    CHECK(first.restoredSources == second.restoredSources);
}

// A fixed PCM clock, independent of device scheduling and wall time. This
// intentionally does not call update/cull or any mutating playback query.
double u27MixAudioFrames(SoLoudAudioEngine& audio, unsigned frames) {
    std::array<float, 512 * 2> pcm{};
    double energy = 0;
    bool finite = true;
    while (frames != 0) {
        const auto count = (std::min)(512u, frames);
        audio.soloud().mix(pcm.data(), count);
        for (unsigned i = 0; i != count * 2; ++i) {
            finite = finite && std::isfinite(pcm[i]);
            energy += std::abs(double(pcm[i]));
        }
        frames -= count;
    }
    CHECK(finite);
    return energy;
}
}

TEST_CASE("Audio U27 snapshot: lifecycle distinguishes real mixers from unsupported backends") {
    SoLoudAudioEngine device;
    SoLoudAudioEngine software{SoLoudAudioEngine::OutputMode::Software};
    SoLoudAudioEngine audio{SoLoudAudioEngine::OutputMode::ManualMix};
    IAudioBackend& api = audio;
    // Device and Software are observed before init; no physical device opens.
    u27CheckAudioIdle(device.getSnapshot(), false, 0, 0, AudioOutputMode::Device);
    u27CheckAudioIdle(software.getSnapshot(), false, 0, 0, AudioOutputMode::Software);
    u27CheckAudioIdle(api.getSnapshot(), false, 0, 0, AudioOutputMode::ManualMix);
    REQUIRE(audio.init());
    REQUIRE(audio.soloud().getBackendId() == SoLoud::Soloud::NULLDRIVER);
    REQUIRE(audio.soloud().getBackendSamplerate() == 48000);
    REQUIRE(audio.soloud().getBackendChannels() == 2);
    CHECK(audio.soloud().getVoiceCount() == 3); // Infrastructure buses only.
    u27CheckAudioIdle(api.getSnapshot(), true, 3, 0, AudioOutputMode::ManualMix);
    u27CheckSameAudioSnapshot(api.getSnapshot(), api.getSnapshot());
    CHECK(audio.softwareMixStats().frames == 0);
    audio.shutdown();
    u27CheckAudioIdle(api.getSnapshot(), false, 0, 0, AudioOutputMode::ManualMix);
    REQUIRE(audio.init());
    u27CheckAudioIdle(api.getSnapshot(), true, 3, 0, AudioOutputMode::ManualMix);
    audio.shutdown();

    NullAudioBackend silent;
    CHECK_FALSE(silent.getSnapshot().supported);
    REQUIRE(silent.init());
    CHECK_FALSE(silent.getSnapshot().supported);
    CHECK_FALSE(silent.getSnapshot().running);
    CHECK(silent.getSnapshot().outputMode == AudioOutputMode::Unknown);
    silent.shutdown();
    CHECK_FALSE(silent.getSnapshot().supported);
}

TEST_CASE("Audio U27 snapshot: finished raw PCM retains cleanup debt until owner update") {
    AudioQuota quota(2);
    ScopedAudioQuota scopedQuota(quota);
    SoLoudAudioEngine audio{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(audio.init());
    REQUIRE(audio.soloud().getBackendId() == SoLoud::Soloud::NULLDRIVER);
    std::vector<float> pcm(128 * 2, 0.125f);
    const auto handle = audio.playRawPCM(pcm.data(), 128, 48000, 2);
    REQUIRE(handle != 0);
    CHECK(quota.activeCount == 1);
    const auto playing = audio.getSnapshot();
    CHECK(playing.supported);
    CHECK(playing.liveVoices == 1);
    CHECK(playing.busVoices == 3);
    CHECK(playing.sessionHandles == 1);
    CHECK(playing.rawCacheEntries == 1);
    CHECK(playing.waveCacheEntries == 0);
    const auto position = audio.soloud().getStreamPosition(handle);
    u27CheckSameAudioSnapshot(playing, audio.getSnapshot());
    CHECK(audio.soloud().getStreamPosition(handle) == position);
    CHECK(u27MixAudioFrames(audio, 48000) > 0);
    REQUIRE_FALSE(audio.soloud().isValidVoiceHandle(handle));
    const auto finished = audio.getSnapshot();
    CHECK(finished.liveVoices == 0);
    CHECK(finished.sessionHandles == 1);
    CHECK(finished.rawCacheEntries == 1);
    CHECK(quota.activeCount == 1);
    CHECK(quota.releaseCalls == 0);
    u27CheckSameAudioSnapshot(finished, audio.getSnapshot());
    CHECK(quota.releaseCalls == 0);
    audio.update(0);
    u27CheckAudioIdle(audio.getSnapshot(), true, 3, 0, AudioOutputMode::ManualMix);
    CHECK(quota.activeCount == 0);
    CHECK(quota.releaseCalls == 1);
    CHECK(quota.releaseUnderflows == 0);
    audio.shutdown();
    u27CheckAudioIdle(audio.getSnapshot(), false, 0, 0, AudioOutputMode::ManualMix);
}

TEST_CASE("Audio U27 snapshot: natural voice notifications survive repeated observations") {
    AudioQuota quota(2);
    ScopedAudioQuota scopedQuota(quota);
    SoLoudAudioEngine audio{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(audio.init());
    REQUIRE(audio.soloud().getBackendId() == SoLoud::Soloud::NULLDRIVER);
    // The retained fixture is 4410 frames at 44100 Hz (0.1 seconds).
    const auto handle = audio.playVoice("tests/audio/silence.wav");
    REQUIRE(handle != 0);
    const auto playing = audio.getSnapshot();
    CHECK(playing.liveVoices == 1);
    CHECK(playing.sessionHandles == 1);
    CHECK(playing.voiceCompletionsPending == 0);
    u27MixAudioFrames(audio, 48000);
    REQUIRE_FALSE(audio.soloud().isValidVoiceHandle(handle));
    const auto uncollected = audio.getSnapshot();
    CHECK(uncollected.liveVoices == 0);
    CHECK(uncollected.sessionHandles == 1);
    CHECK(uncollected.voiceCompletionsPending == 0);
    CHECK(quota.releaseCalls == 0);
    audio.update(0);
    const auto completed = audio.getSnapshot();
    CHECK(completed.supported);
    CHECK(completed.sessionHandles == 0);
    CHECK(completed.voiceCompletionsPending == 1);
    CHECK(completed.waveCacheEntries == 1);
    CHECK(quota.releaseCalls == 1);
    for (unsigned i = 0; i != 3; ++i)
        u27CheckSameAudioSnapshot(completed, audio.getSnapshot());
    CHECK(audio.consumeVoiceCompletions() == 1);
    CHECK(audio.consumeVoiceCompletions() == 0);
    u27CheckAudioIdle(audio.getSnapshot(), true, 3, 1, AudioOutputMode::ManualMix);
    audio.flushWaveCache();
    u27CheckAudioIdle(audio.getSnapshot(), true, 3, 0, AudioOutputMode::ManualMix);
    CHECK(quota.releaseUnderflows == 0);
}

TEST_CASE("Audio U27 snapshot: retiring BGM and voice owners remain visible until culled") {
    AudioQuota quota(8);
    ScopedAudioQuota scopedQuota(quota);
    SoLoudAudioEngine audio{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(audio.init());
    REQUIRE(audio.soloud().getBackendId() == SoLoud::Soloud::NULLDRIVER);
    SUBCASE("BGM replacement retains the retired source until update") {
        const auto first = audio.playBGM("tests/audio/silence.wav", 0);
        REQUIRE(first != 0);
        audio.soloud().setLooping(first, true);
        const auto second = audio.playBGM("tests/audio/silence.wav", 0.05f);
        REQUIRE(second != 0);
        audio.soloud().setLooping(second, true);
        const auto retiring = audio.getSnapshot();
        CHECK(retiring.liveVoices == 2);
        CHECK(retiring.sessionHandles == 2);
        CHECK(retiring.retiringBGM == 1);
        CHECK(retiring.retiringVoice == 0);
        CHECK(retiring.waveCacheEntries == 1);
        u27CheckSameAudioSnapshot(retiring, audio.getSnapshot());
        u27MixAudioFrames(audio, 48000);
        REQUIRE_FALSE(audio.soloud().isValidVoiceHandle(first));
        REQUIRE(audio.soloud().isValidVoiceHandle(second));
        const auto uncollected = audio.getSnapshot();
        CHECK(uncollected.liveVoices == 1);
        CHECK(uncollected.sessionHandles == 2);
        CHECK(uncollected.retiringBGM == 1);
        CHECK(quota.activeCount == 2);
        audio.update(0);
        const auto culled = audio.getSnapshot();
        CHECK(culled.liveVoices == 1);
        CHECK(culled.sessionHandles == 1);
        CHECK(culled.retiringBGM == 0);
        CHECK(quota.activeCount == 1);
        audio.stopBGM(0);
    }
    SUBCASE("Voice rotation and explicit stop do not manufacture completions") {
        std::array<unsigned, 5> handles{};
        for (auto& handle : handles) {
            handle = audio.playVoice("tests/audio/silence.wav");
            REQUIRE(handle != 0);
            audio.soloud().setLooping(handle, true);
        }
        const auto retiring = audio.getSnapshot();
        CHECK(retiring.liveVoices == 5);
        CHECK(retiring.sessionHandles == 5);
        CHECK(retiring.retiringBGM == 0);
        CHECK(retiring.retiringVoice == 1);
        CHECK(retiring.waveCacheEntries == 1);
        u27CheckSameAudioSnapshot(retiring, audio.getSnapshot());
        u27MixAudioFrames(audio, 48000);
        REQUIRE_FALSE(audio.soloud().isValidVoiceHandle(handles[0]));
        const auto uncollected = audio.getSnapshot();
        CHECK(uncollected.liveVoices == 4);
        CHECK(uncollected.sessionHandles == 5);
        CHECK(uncollected.retiringVoice == 1);
        CHECK(quota.activeCount == 5);
        audio.update(0);
        CHECK(audio.getSnapshot().sessionHandles == 4);
        CHECK(audio.getSnapshot().retiringVoice == 0);
        CHECK(quota.activeCount == 4);
        audio.stopVoice();
        CHECK(audio.getSnapshot().retiringVoice == 4);
        CHECK(audio.getSnapshot().sessionHandles == 4);
        CHECK(audio.getSnapshot().voiceCompletionsPending == 0);
        u27MixAudioFrames(audio, 48000);
        CHECK(audio.getSnapshot().liveVoices == 0);
        CHECK(audio.getSnapshot().retiringVoice == 4);
        CHECK(quota.activeCount == 4);
        audio.update(0);
        CHECK(audio.consumeVoiceCompletions() == 0);
    }
    u27CheckAudioIdle(audio.getSnapshot(), true, 3, 1, AudioOutputMode::ManualMix);
    CHECK(quota.activeCount == 0);
    CHECK(quota.releaseUnderflows == 0);
    audio.flushWaveCache();
    audio.shutdown();
    u27CheckAudioIdle(audio.getSnapshot(), false, 0, 0, AudioOutputMode::ManualMix);
}
