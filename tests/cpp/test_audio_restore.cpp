#include "doctest.h"
#include "audio/SoLoudAudioEngine.h"
#include "audio/NullAudioBackend.h"
#include "di/BackendRegistry.h"
#include "di/api/ISandboxQuota.h"
#include "entry/Engine.h"
#include "entry/EngineConfig.h"
#include "resource/api/IAssetReader.h"
#include "script/api/ILuaManager.h"
#include "storage/api/ISaveManager.h"
#include "TestPaths.h"
#include <cmath>
#include <algorithm>
#include <limits>
#include <vector>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <soloud_wavstream.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

using namespace Caesura;

namespace {
std::vector<uint8_t> silentWave() {
    std::vector<uint8_t> bytes(2044, 0);
    const auto word = [&](size_t offset, uint32_t value, size_t count) {
        for (size_t i = 0; i < count; ++i) bytes[offset + i] = uint8_t(value >> (i * 8));
    };
    const char* riff = "RIFF";
    const char* wave = "WAVEfmt ";
    const char* data = "data";
    for (size_t i = 0; i < 4; ++i) { bytes[i] = riff[i]; bytes[36+i] = data[i]; }
    for (size_t i = 0; i < 8; ++i) bytes[8+i] = wave[i];
    word(4, 2036, 4); word(16, 16, 4); word(20, 1, 2); word(22, 1, 2);
    word(24, 1000, 4); word(28, 2000, 4); word(32, 2, 2); word(34, 16, 2); word(40, 2000, 4);
    return bytes;
}

std::vector<uint8_t> patternWave(unsigned sampleRate = 48000) {
    constexpr size_t frames = 32768;
    auto bytes = silentWave();
    bytes.resize(44 + frames * 4);
    const auto word = [&](size_t offset, uint32_t value, size_t count) {
        for (size_t i = 0; i < count; ++i) bytes[offset + i] = uint8_t(value >> (i * 8));
    };
    word(4, static_cast<uint32_t>(bytes.size() - 8), 4);
    word(22, 2, 2); word(24, sampleRate, 4); word(28, sampleRate * 4, 4); word(32, 4, 2);
    word(40, frames * 4, 4);
    for (size_t i = 0; i < frames; ++i) {
        word(44 + i * 4, static_cast<uint16_t>((int(i % 97) - 48) * 120), 2);
        word(46 + i * 4, static_cast<uint16_t>((int(i % 71) - 35) * 140), 2);
    }
    return bytes;
}

struct AudioWaveFile {
    const std::string path = TestPaths::uniqueTempDir("audio_wave").filename().string() + ".wav";
    explicit AudioWaveFile(const std::vector<uint8_t>& bytes) {
        std::ofstream output(path, std::ios::binary);
        REQUIRE(output.good());
        output.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        REQUIRE(output.good());
    }
    ~AudioWaveFile() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
};

double pcmDifference(const std::vector<float>& first, const std::vector<float>& second) {
    REQUIRE(first.size() == second.size());
    double difference = 0;
    for (size_t i = 0; i < first.size(); ++i)
        difference = (std::max)(difference, std::abs(double(first[i] - second[i])));
    return difference;
}

class RestoreAudioQuota final : public ISandboxQuota {
public:
    RestoreAudioQuota() : previous(BackendRegistry::instance().getSandboxQuota()) {
        BackendRegistry::instance().setSandboxQuota(this);
    }
    ~RestoreAudioQuota() override { BackendRegistry::instance().setSandboxQuota(previous); }
    void setLuaState(lua_State*) override {}
    bool tryAlloc(const char*) override { if (reject) return false; ++live; return true; }
    void release(const char*) override { --live; }
    int count(const char*) override { return live; }
    int maxLimit(const char*) override { return 100; }
    ISandboxQuota* previous;
    int live = 0;
    bool reject = false;
};

class AudioAssetFixture final : public IAssetReader {
public:
    AudioAssetFixture() : previous(BackendRegistry::instance().getAssetReader()) {
        BackendRegistry::instance().setAssetReader(this);
    }
    ~AudioAssetFixture() override { BackendRegistry::instance().setAssetReader(previous); }
    std::vector<uint8_t> readAsset(const std::string& path, size_t limit) override {
        ++reads;
        return path == "assets/bgm.wav" && bytes.size() <= limit ? bytes : std::vector<uint8_t>{};
    }
    IAssetReader* previous;
    std::vector<uint8_t> bytes = silentWave();
    int reads = 0;
};
}

TEST_CASE("U11 audio restore: CPU preparation owns bytes and rejects invalid state") {
    RestoreAudioQuota quota;
    SoLoudAudioEngine audio;
    auto bytes = silentWave();
    const AudioRestoreState state{"assets/bgm.wav", 0.25, 0.75f, true};
    auto prepared = audio.prepareAudioState(state, bytes.data(), bytes.size());
    REQUIRE(prepared != nullptr);
    CHECK(prepared->description().bgmPath == state.bgmPath);
    CHECK(prepared->description().position == state.position);
    CHECK(quota.live == 0);
    CHECK_FALSE(audio.isBGMPlaying());
    for (const double position : {-1.0, 2.0, std::numeric_limits<double>::infinity()}) {
        auto invalid = state;
        invalid.position = position;
        CHECK_FALSE(audio.prepareAudioState(invalid, bytes.data(), bytes.size()));
    }
    CHECK_FALSE(audio.prepareAudioState(state, bytes.data(), 12));
    auto shortContainer = bytes;
    shortContainer[4] = 120;
    shortContainer[5] = shortContainer[6] = shortContainer[7] = 0;
    CHECK_FALSE(audio.prepareAudioState(state, shortContainer.data(), shortContainer.size()));
    CHECK_FALSE(audio.prepareAudioState(state, nullptr, 0));
    CHECK_FALSE(audio.prepareAudioState({"../bgm.wav"}, bytes.data(), bytes.size()));
}

TEST_CASE("U11 audio restore: preparation rejects decoder channels beyond the mixer limit") {
    RestoreAudioQuota quota;
    SoLoudAudioEngine audio;
    for (const unsigned channels : {unsigned(MAX_CHANNELS), unsigned(MAX_CHANNELS + 1)}) {
        CAPTURE(channels);
        constexpr unsigned frames = 512;
        auto bytes = silentWave();
        bytes.resize(44 + frames * channels * 2, 0);
        const auto word = [&](size_t offset, uint32_t value, size_t count) {
            for (size_t i = 0; i < count; ++i) bytes[offset + i] = uint8_t(value >> (i * 8));
        };
        word(4, unsigned(bytes.size() - 8), 4);
        word(22, channels, 2);
        word(24, 48000, 4);
        word(28, 48000 * channels * 2, 4);
        word(32, channels * 2, 2);
        word(40, frames * channels * 2, 4);
        // Safety detector: never apply or mix the malformed channel count.
        auto prepared = audio.prepareAudioState({"assets/channel-limit.wav"}, bytes.data(), bytes.size());
        if (channels == MAX_CHANNELS) CHECK(prepared != nullptr);
        else CHECK_FALSE(prepared);
        // The matching non-streaming loader guard is now in place before its
        // fixed scratch decode. This call was not part of the unsafe RED run.
        SoLoud::Wav wave;
        const auto loaded = wave.loadMem(bytes.data(), unsigned(bytes.size()), true, false);
        if (channels == MAX_CHANNELS) CHECK(loaded == SoLoud::SO_NO_ERROR);
        else CHECK(loaded != SoLoud::SO_NO_ERROR);
        CHECK(quota.live == 0);
        CHECK_FALSE(audio.isBGMPlaying());
    }
}

TEST_CASE("U11 audio restore: real mixer resumes BGM and hard-stops old session voices") {
    RestoreAudioQuota quota;
    SoLoudAudioEngine audio{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(audio.init());
    REQUIRE(audio.soloud().getBackendId() == SoLoud::Soloud::NULLDRIVER);
    audio.setGlobalVolume(0.7f);
    audio.setBusVolume("bgm", 0.6f);
    REQUIRE(audio.playBGM("tests/audio/silence.wav", 0) != 0);
    const auto original = audio.captureAudioState();
    CHECK(original.bgmPath == "tests/audio/silence.wav");
    REQUIRE(audio.playVoice("tests/audio/silence.wav") != 0);
    REQUIRE(audio.playSE("tests/audio/silence.wav") != 0);
    audio.suspend(); // freeze mixer time while comparing the restored cursor
    const int active = quota.live;
    auto bytes = silentWave();
    auto prepared = audio.prepareAudioState({"assets/bgm.wav",0.25,0.75f,true}, bytes.data(), bytes.size());
    REQUIRE(prepared != nullptr);
    CHECK(quota.live == active);
    CHECK(audio.captureAudioState().bgmPath == original.bgmPath);
    bytes.clear(); bytes.shrink_to_fit();
    REQUIRE(audio.applyAudioState(std::move(prepared)));
    const auto restored = audio.captureAudioState();
    CHECK(restored.bgmPath == "assets/bgm.wav");
    CHECK(restored.position == doctest::Approx(0.25).epsilon(0.001));
    CHECK(restored.gain == doctest::Approx(0.75));
    CHECK(restored.looping);
    CHECK_FALSE(audio.isVoicePlaying());
    CHECK_FALSE(audio.isSEPlaying());
    CHECK(audio.consumeVoiceCompletions() == 0);
    CHECK(quota.live == 1);
    CHECK(audio.getGlobalVolume() == doctest::Approx(0.7));
    CHECK(audio.getBusVolume("bgm") == doctest::Approx(0.6));
    audio.stopSessionAudio();
    audio.stopSessionAudio();
    CHECK_FALSE(audio.isBGMPlaying());
    CHECK(quota.live == 0);
    bytes = silentWave();
    prepared = audio.prepareAudioState({"assets/bgm.wav"}, bytes.data(), bytes.size());
    REQUIRE(prepared != nullptr);
    quota.reject = true;
    CHECK_FALSE(audio.applyAudioState(std::move(prepared)));
    CHECK_FALSE(audio.isBGMPlaying());
    CHECK(quota.live == 0);
    quota.reject = false;
    for (const auto* path : {"assets/bgm/daily.ogg", "tests/audio/silence.flac"}) {
        std::ifstream input(path, std::ios::binary);
        REQUIRE_MESSAGE(input.good(), path);
        const std::vector<uint8_t> encoded{std::istreambuf_iterator<char>(input), {}};
        auto compressed = audio.prepareAudioState({path}, encoded.data(), encoded.size());
        REQUIRE_MESSAGE(compressed != nullptr, path);
        REQUIRE(audio.applyAudioState(std::move(compressed)));
        CHECK(audio.captureAudioState().bgmPath == path);
        // Vorbis reports its actual sample boundary (the shipped Ogg begins
        // two samples after zero). Keep the allowed codec offset below 1 ms.
        CHECK(audio.captureAudioState().position >= 0.0);
        CHECK(audio.captureAudioState().position <= 0.001);
        audio.stopSessionAudio();
        CHECK(quota.live == 0);
    }
    const auto ogg = audio.playBGM("assets/bgm/daily.ogg", 0);
    REQUIRE(ogg != 0);
    CHECK(audio.soloud().seek(ogg, -1.0) != SoLoud::SO_NO_ERROR);
    CHECK(audio.soloud().seek(ogg, 1e12) != SoLoud::SO_NO_ERROR);
    CHECK(audio.soloud().seek(ogg, 0.25) == SoLoud::SO_NO_ERROR);
    CHECK(std::abs(audio.captureAudioState().position - 0.25) < 0.001);
    audio.stopSessionAudio();
}

TEST_CASE("U11 audio restore: silent backend accepts only a silent snapshot") {
    NullAudioBackend audio;
    REQUIRE(audio.init());
    auto prepared = audio.prepareAudioState({}, nullptr, 0);
    REQUIRE(prepared != nullptr);
    CHECK(audio.applyAudioState(std::move(prepared)));
    const auto bytes = silentWave();
    CHECK_FALSE(audio.prepareAudioState({"assets/bgm.wav"}, bytes.data(), bytes.size()));
    CHECK(audio.captureAudioState().bgmPath.empty());
    audio.stopSessionAudio();
    CHECK_FALSE(audio.isBGMPlaying());
}

TEST_CASE("U11 audio restore: Ogg seek emits the same PCM samples as continuous decoding") {
    SoLoud::Wav reference;
    SoLoud::WavStream stream;
    REQUIRE(reference.load("assets/bgm/daily.ogg") == SoLoud::SO_NO_ERROR);
    REQUIRE(stream.load("assets/bgm/daily.ogg") == SoLoud::SO_NO_ERROR);
    constexpr unsigned int frames = 256;
    constexpr unsigned int target = 12345;
    REQUIRE(reference.mSampleCount > target + frames);
    std::unique_ptr<SoLoud::AudioSourceInstance> instance(stream.createInstance());
    REQUIRE(instance != nullptr);
    instance->init(stream, 0);
    std::vector<float> samples(frames * reference.mChannels);
    // Leave a partial decoder frame before seeking: old buffered PCM must not
    // be emitted when playback resumes at the new position.
    REQUIRE(instance->getAudio(samples.data(), frames, frames) == frames);
    const double position = double(target) / reference.mBaseSamplerate;
    REQUIRE(instance->seek(position, samples.data(), unsigned(samples.size())) == SoLoud::SO_NO_ERROR);
    REQUIRE(instance->getAudio(samples.data(), frames, frames) == frames);
    double maximumError = 0, signal = 0;
    for (unsigned int channel = 0; channel < reference.mChannels; ++channel) {
        for (unsigned int i = 0; i < frames; ++i) {
            const float expected = reference.mData[channel * reference.mSampleCount + target + i];
            signal += std::abs(expected);
            maximumError = std::max(maximumError, double(std::abs(samples[channel * frames + i] - expected)));
        }
    }
    CHECK(signal > 0.001);
    CHECK(maximumError < 0.00001);
}

TEST_CASE("U11 audio restore: Lua binding transfers prepared ownership into the real backend") {
    auto* audio = new SoLoudAudioEngine{SoLoudAudioEngine::OutputMode::ManualMix};
    EngineConfig config;
    config.headless = true;
    config.audio = audio;
    Engine engine{std::move(config)};
    REQUIRE(engine.init());
    REQUIRE(BackendRegistry::instance().getAudioBackend() == audio);
    REQUIRE(audio->soloud().getBackendId() == SoLoud::Soloud::NULLDRIVER);
    audio->suspend();
    AudioAssetFixture assets;
    auto* vm = BackendRegistry::instance().getLuaManager();
    REQUIRE(vm != nullptr);
    const auto run = [&](const char* code) {
        vm->resetInstructionBudget();
        lua_State* state = vm->state();
        const int result = luaL_dostring(state, code);
        const std::string error = result == LUA_OK ? "" : lua_tostring(state, -1);
        lua_settop(state, 0);
        REQUIRE_MESSAGE(result == LUA_OK, error);
    };
    run(R"lua(
        assert(Restore.capture_audio().bgm == false)
        audio_ticket = assert(Restore.prepare_audio({version=1,bgm={
            path='assets/bgm.wav',position=0.25,gain=0.5,looping=true}}))
        assert(type(audio_ticket) == 'userdata')
        assert(Restore.capture_audio().bgm == false)
    )lua");
    CHECK(assets.reads == 1);
    assets.bytes.clear();
    run(R"lua(
        assert(Restore.apply_audio(audio_ticket))
        local snapshot=Restore.capture_audio()
        assert(snapshot.version == 1 and snapshot.bgm.path == 'assets/bgm.wav')
        assert(math.abs(snapshot.bgm.position-0.25)<0.001)
        assert(snapshot.bgm.gain == 0.5 and snapshot.bgm.looping == true)
        Restore.discard_audio(audio_ticket)
        Restore.discard_audio(audio_ticket)
        assert(Restore.apply_audio(audio_ticket) == false)
        assert(Restore.capture_audio().bgm.path == 'assets/bgm.wav')
        assert(Restore.stop_audio())
        assert(Restore.capture_audio().bgm == false)
        assert(Restore.prepare_audio({version=1,bgm={path='assets/bgm.wav',
            position=-1,gain=1,looping=false}}) == nil)
    )lua");
    CHECK(assets.reads == 1);
}

TEST_CASE("U11 audio restore: manual mixer uses real SoLoud without a device callback") {
    RestoreAudioQuota quota;
    SoLoudAudioEngine audio{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(audio.init());
    CHECK(audio.soloud().getBackendId() == SoLoud::Soloud::NULLDRIVER);
    CHECK(audio.soloud().getBackendSamplerate() == 48000);
    CHECK(audio.soloud().getBackendChannels() == 2);
    audio.bgmBus().findBusHandle();
    audio.voiceBus().findBusHandle();
    audio.seBus().findBusHandle();
    REQUIRE(audio.bgmBus().mChannelHandle != 0);
    REQUIRE(audio.voiceBus().mChannelHandle != 0);
    REQUIRE(audio.seBus().mChannelHandle != 0);
    audio.setBusVolume("bgm", 0.8f);
    audio.setBusVolume("voice", 0.7f);
    audio.setBusVolume("se", 0.6f);
    CHECK(audio.soloud().getVolume(audio.bgmBus().mChannelHandle) == doctest::Approx(0.8));
    CHECK(audio.soloud().getVolume(audio.voiceBus().mChannelHandle) == doctest::Approx(0.7));
    CHECK(audio.soloud().getVolume(audio.seBus().mChannelHandle) == doctest::Approx(0.6));
    constexpr unsigned frames = 8192;
    std::vector<float> source(frames * 2);
    for (unsigned i = 0; i < frames; ++i) {
        source[i * 2] = float(int(i % 97) - 48) / 500.0f;
        source[i * 2 + 1] = float(int(i % 71) - 35) / 700.0f;
    }
    const auto handle = audio.playRawPCM(source.data(), frames, 48000, 2);
    REQUIRE(handle != 0);
    CHECK(audio.soloud().getStreamPosition(handle) == 0);
    // Fixed, explicit mixing is the only clock; no sleep or audio hardware.
    std::vector<float> output(2048 * 2);
    audio.soloud().mix(output.data(), 2048);
    double energy = 0, channelDifference = 0;
    for (size_t i = 0; i < output.size(); i += 2) {
        REQUIRE(std::isfinite(output[i]));
        REQUIRE(std::isfinite(output[i + 1]));
        energy += std::abs(output[i]) + std::abs(output[i + 1]);
        channelDifference += std::abs(output[i] - output[i + 1]);
    }
    CHECK(energy > 1);
    CHECK(channelDifference > 1);
    CHECK(audio.soloud().getStreamPosition(handle) > 0);
    audio.stopSessionAudio();
    CHECK_FALSE(audio.soloud().isValidVoiceHandle(handle));
    CHECK(quota.live == 0);
    // The next mix must not reuse a previous session's bus buffers.
    audio.soloud().mix(output.data(), 2048);
    for (const float sample : output) CHECK(sample == 0);
}

TEST_CASE("U11 audio restore: restored mixer output follows continuous BGM samples") {
    std::vector<unsigned> prefixChunks{8192};
    unsigned sourceRate = 48000;
    SUBCASE("aligned source and bus buffers") {}
    SUBCASE("nonaligned source and bus buffers") { prefixChunks = {8193}; }
    SUBCASE("eighteen real callback blocks") { prefixChunks.assign(18, 512); }
    SUBCASE("same sample frontier with different callback blocks") { prefixChunks.assign(72, 128); }
    SUBCASE("start retains initial linear zero history") { prefixChunks.clear(); }
    SUBCASE("first source frame") { prefixChunks = {1}; }
    SUBCASE("second source frame") { prefixChunks = {2}; }
    SUBCASE("before a resample block boundary") { prefixChunks = {511}; }
    SUBCASE("after a resample block boundary") { prefixChunks = {513}; }
    SUBCASE("fractional rate and partial bus buffer") { sourceRate = 44100; prefixChunks = {8193}; }
    SUBCASE("low source rate needs a longer history window") { sourceRate = 1000; prefixChunks = {8193}; }
    RestoreAudioQuota quota;
    SoLoudAudioEngine audio{SoLoudAudioEngine::OutputMode::ManualMix};
    audio.setGlobalVolume(0.6f);
    audio.setBusVolume("bgm", 0.7f);
    REQUIRE(audio.init());
    const auto bytes = patternWave(sourceRate);
    auto start = audio.prepareAudioState({"assets/pattern.wav", 0, 0.625f, true}, bytes.data(), bytes.size());
    REQUIRE(start != nullptr);
    REQUIRE(audio.applyAudioState(std::move(start)));
    unsigned prefixFrames = 0;
    for (const auto frames : prefixChunks) {
        std::vector<float> prefix(frames * 2);
        audio.soloud().mix(prefix.data(), frames);
        prefixFrames += frames;
    }
    const auto saved = audio.captureAudioState();
    CHECK(saved.position == doctest::Approx(double(prefixFrames) / 48000).epsilon(0.00001));
    auto prepared = audio.prepareAudioState(saved, bytes.data(), bytes.size());
    REQUIRE(prepared != nullptr);
    constexpr unsigned comparisonFrames = 4097;
    std::vector<float> continuous(comparisonFrames * 2), restored(comparisonFrames * 2);
    audio.soloud().mix(continuous.data(), comparisonFrames);
    REQUIRE(audio.applyAudioState(std::move(prepared)));
    CHECK(audio.captureAudioState().position == doctest::Approx(saved.position).epsilon(0.00001));
    audio.soloud().mix(restored.data(), comparisonFrames);
    double maximumError = 0, signal = 0;
    for (size_t i = 0; i < restored.size(); ++i) {
        maximumError = std::max(maximumError, std::abs(double(restored[i] - continuous[i])));
        signal += std::abs(continuous[i]);
    }
    CAPTURE(maximumError);
    CAPTURE(saved.position);
    CAPTURE(prefixFrames);
    CAPTURE(sourceRate);
    if (maximumError >= 0.00001) {
        std::ofstream before("u11-mix-continuous.f32", std::ios::binary);
        before.write(reinterpret_cast<const char*>(continuous.data()), continuous.size() * sizeof(float));
        std::ofstream after("u11-mix-restored.f32", std::ios::binary);
        after.write(reinterpret_cast<const char*>(restored.data()), restored.size() * sizeof(float));
    }
    CHECK(signal > 1);
    CHECK(maximumError < 0.00001);
    audio.stopSessionAudio();
    audio.soloud().mix(restored.data(), comparisonFrames);
    double stoppedEnergy = 0;
    for (const float sample : restored) stoppedEnergy += std::abs(sample);
    CHECK(stoppedEnergy == 0);
    CHECK(quota.live == 0);
}

TEST_CASE("U11 audio restore: exact decoder seek respects partial stereo channel stride") {
    const auto bytes = patternWave();
    SoLoud::Wav reference;
    SoLoud::WavStream stream;
    REQUIRE(reference.loadMem(bytes.data(), unsigned(bytes.size()), true, false) == SoLoud::SO_NO_ERROR);
    REQUIRE(stream.loadMem(bytes.data(), unsigned(bytes.size()), true, false) == SoLoud::SO_NO_ERROR);
    std::unique_ptr<SoLoud::AudioSourceInstance> decoder(stream.createInstance());
    REQUIRE(decoder != nullptr);
    decoder->init(stream, 0);
    REQUIRE(decoder->seekFrame(stream.mSampleCount - 3) == SoLoud::SO_NO_ERROR);
    std::vector<float> output(32, 99.0f);
    REQUIRE(decoder->getAudio(output.data(), 9, 16) == 3);
    for (unsigned channel = 0; channel < 2; ++channel) {
        for (unsigned frame = 0; frame < 3; ++frame)
            CHECK(output[channel * 16 + frame] == reference.mData[channel * reference.mSampleCount + reference.mSampleCount - 3 + frame]);
        for (unsigned frame = 3; frame < 16; ++frame) CHECK(output[channel * 16 + frame] == 99.0f);
    }
}

TEST_CASE("U11 audio restore: linear resampling retains fractional phase across source blocks") {
    SoLoud::Soloud mixer;
    // The bounded ramp cannot clip. Disable the optional soft-clip transfer
    // curve so the reference below isolates resampling phase, not distortion.
    REQUIRE(mixer.init(0, SoLoud::Soloud::NULLDRIVER,
        48000, 2048, 2) == SoLoud::SO_NO_ERROR);
    mixer.setPostClipScaler(1.0f);
    SoLoud::Wav wave;
    std::vector<float> source(4096);
    for (unsigned i = 0; i < source.size(); ++i) source[i] = float(i) / 16384;
    REQUIRE(wave.loadRawWave(source.data(), unsigned(source.size()), 44100, 1, true, false) == SoLoud::SO_NO_ERROR);
    REQUIRE(mixer.play(wave) != 0);
    constexpr unsigned frames = 2049;
    std::vector<float> output(frames * 2);
    mixer.mix(output.data(), frames);
    const uint64_t phaseUnit = uint64_t{1} << 20;
    const uint64_t step = uint64_t(std::floor((44100.0f / 48000.0f) * phaseUnit));
    double maximumError = 0;
    for (unsigned i = 0; i < frames; ++i) {
        const uint64_t phase = uint64_t(i) * step;
        const unsigned frame = unsigned(phase >> 20);
        const float fraction = float(phase & (phaseUnit - 1)) / phaseUnit;
        const float previous = frame ? source[frame - 1] : 0;
        const float expected = (previous + (source[frame] - previous) * fraction) * std::sqrt(0.5f);
        maximumError = (std::max)(maximumError, std::abs(double(output[i * 2] - expected)));
    }
    CAPTURE(maximumError);
    CHECK(maximumError < 0.000001);
}

TEST_CASE("U11 audio restore: sample cursor survives real Lua JSON and rejects invalid fields") {
    TestPaths::ScopedTempDir directory{"audio_cursor"};
    auto* audio = new SoLoudAudioEngine{SoLoudAudioEngine::OutputMode::ManualMix};
    EngineConfig config;
    config.headless = true;
    config.audio = audio;
    Engine engine{std::move(config)};
    REQUIRE(engine.init());
    auto& registry = BackendRegistry::instance();
    auto* saves = registry.getSaveManager();
    auto* vm = registry.getLuaManager();
    REQUIRE(saves != nullptr);
    REQUIRE(vm != nullptr);
    saves->init(directory.string());
    saves->clearEncryptionKey();
    AudioAssetFixture assets;
    assets.bytes = patternWave(44100);
    auto start = audio->prepareAudioState({"assets/bgm.wav", 0, 0.625f, true},
        assets.bytes.data(), assets.bytes.size());
    REQUIRE(start != nullptr);
    REQUIRE(audio->applyAudioState(std::move(start)));
    std::vector<float> block(512 * 2);
    for (unsigned i = 0; i < 18; ++i) audio->soloud().mix(block.data(), 512);
    const auto expected = audio->captureAudioState();
    REQUIRE(expected.framePosition > 0);
    REQUIRE(expected.frameFraction > 0);
    const auto run = [&](const char* code) {
        vm->resetInstructionBudget();
        auto* state = vm->state();
        const int result = luaL_dostring(state, code);
        const char* message = result == LUA_OK ? "" : lua_tostring(state, -1);
        const std::string error = message ? message : "non-string Lua error";
        lua_settop(state, 0);
        REQUIRE_MESSAGE(result == LUA_OK, error);
    };
    run(R"lua(
        package.path = 'scripts/?.lua;scripts/?/init.lua;' .. package.path
        local Audio = require('kag.audio_state')
        audio_cursor_snapshot = Audio.capture()
        local cursor = assert(audio_cursor_snapshot.bgm.cursor)
        assert(math.type(cursor.frame) == 'integer' and math.type(cursor.fraction) == 'integer')
        assert(cursor.fraction > 0 and cursor.source_rate == 44100 and cursor.output_rate == 48000)
        assert(KAG.save_game(17, {audio_snapshot=audio_cursor_snapshot}, 'audio-cursor.ks', 1))
        local loaded = assert(KAG.load_game(17))
        for key,value in pairs(cursor) do assert(loaded.audio_snapshot.bgm.cursor[key] == value) end
        assert(math.type(loaded.audio_snapshot.bgm.cursor.frame) == 'integer')
        Audio.apply(Audio.prepare(loaded.audio_snapshot))
        local restored = Audio.capture()
        for key,value in pairs(cursor) do assert(restored.bgm.cursor[key] == value) end
    )lua");
    CHECK(assets.reads == 1);
    const auto disk = saves->load(17);
    const auto& cursor = disk.at("audio_snapshot").at("bgm").at("cursor");
    CHECK(cursor.at("frame").is_number_integer());
    CHECK(cursor.at("frame").get<uint64_t>() == expected.framePosition);
    CHECK(cursor.at("fraction").get<uint32_t>() == expected.frameFraction);
    CHECK(cursor.at("source_rate").get<uint32_t>() == expected.sourceRate);
    CHECK(cursor.at("output_rate").get<uint32_t>() == expected.outputRate);

    run(R"lua(
        local Audio = require('kag.audio_state')
        local copy = require('kag.save_state').copy
        local function prepare(label, state)
            local ok,ticket = pcall(Audio.prepare, state)
            assert(ok, label .. ': ' .. tostring(ticket))
            return ticket
        end
        local legacy = copy(audio_cursor_snapshot)
        legacy.bgm.cursor = nil
        Audio.discard(prepare('legacy cursor', legacy))
        local boundary = copy(audio_cursor_snapshot)
        boundary.bgm.cursor.frame = 17592186044415
        boundary.bgm.cursor.fraction = 1048575
        boundary.bgm.position = (boundary.bgm.cursor.frame % 32768 + 1048575/1048576)/44100
        Audio.discard(prepare('maximum valid cursor near EOF', boundary))
        assert(KAG.save_game(18, {audio_snapshot=boundary}, 'audio-cursor.ks', 1))
        local loaded = assert(KAG.load_game(18)).audio_snapshot.bgm.cursor
        assert(loaded.frame == 17592186044415 and math.type(loaded.frame) == 'integer')
        assert(loaded.fraction == 1048575)
    )lua");
    CHECK(assets.reads == 3);
    const int readsBeforeRejection = assets.reads;
    run(R"lua(
        local Audio = require('kag.audio_state')
        local copy = require('kag.save_state').copy
        local function unchanged()
            local live = assert(Restore.capture_audio()).bgm
            assert(live.path == audio_cursor_snapshot.bgm.path)
            for key,value in pairs(audio_cursor_snapshot.bgm.cursor) do assert(live.cursor[key] == value) end
        end
        local invalid = {
            function(s) s.bgm.cursor = false end,
            function(s) s.bgm.cursor = {} end,
            function(s) s.bgm.cursor = {1,0,44100,48000} end,
        }
        for _,key in ipairs({'frame','fraction','source_rate','output_rate'}) do
            for _,value in ipairs({false, '1', -1, 1.5, math.huge, 0/0}) do
                local field, bad = key, value
                invalid[#invalid+1] = function(s) s.bgm.cursor[field] = bad end
            end
            local field = key
            invalid[#invalid+1] = function(s) s.bgm.cursor[field] = nil end
        end
        invalid[#invalid+1] = function(s) s.bgm.cursor.frame = 17592186044416 end
        invalid[#invalid+1] = function(s) s.bgm.cursor.fraction = 1048576 end
        invalid[#invalid+1] = function(s) s.bgm.cursor.source_rate = 4294967296 end
        invalid[#invalid+1] = function(s) s.bgm.cursor.output_rate = 4294967296 end
        invalid[#invalid+1] = function(s) s.bgm.cursor.source_rate = 0 end
        invalid[#invalid+1] = function(s) s.bgm.cursor.output_rate = 0 end
        for index,mutate in ipairs(invalid) do
            local state = copy(audio_cursor_snapshot)
            mutate(state)
            local native,reason = Restore.prepare_audio(state)
            assert(native == nil and type(reason) == 'string' and #reason > 0, 'native accepted invalid cursor '..index)
            assert(not pcall(Audio.prepare, state), 'Lua accepted invalid cursor '..index)
            unchanged()
        end
    )lua");
    CHECK(assets.reads == readsBeforeRejection);
    CHECK(audio->isBGMPlaying());
    const auto actual = audio->captureAudioState();
    CHECK(actual.framePosition == expected.framePosition);
    CHECK(actual.frameFraction == expected.frameFraction);
    CHECK(actual.gain == expected.gain);
    audio->stopSessionAudio();
}

TEST_CASE("U11 audio restore: ordinary BGM after idle bus frames has an exact playback cursor") {
    unsigned idleFrames = 1;
    SUBCASE("one idle frame") {}
    SUBCASE("partial idle block") { idleFrames = 128; }
    for (const unsigned prefixFrames : {1u, 128u, 511u, 512u}) {
    const auto bytes = patternWave();
    AudioWaveFile file(bytes);
    RestoreAudioQuota quota;
    SoLoudAudioEngine audio{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(audio.init());
    std::vector<float> idle(idleFrames * 2);
    audio.soloud().mix(idle.data(), idleFrames);
    REQUIRE(audio.playBGM(file.path, 0) != 0);
    std::vector<float> prefix(prefixFrames * 2);
    audio.soloud().mix(prefix.data(), prefixFrames);
    const auto saved = audio.captureAudioState();
    REQUIRE(saved.bgmPath == file.path);
    auto prepared = audio.prepareAudioState(saved, bytes.data(), bytes.size());
    REQUIRE(prepared != nullptr);
    std::vector<float> continuous(4097 * 2), restored(4097 * 2);
    audio.soloud().mix(continuous.data(), 4097);
    REQUIRE(audio.applyAudioState(std::move(prepared)));
    audio.soloud().mix(restored.data(), 4097);
    const auto maximumError = pcmDifference(continuous, restored);
    CAPTURE(idleFrames);
    CAPTURE(prefixFrames);
    CAPTURE(saved.framePosition);
    CAPTURE(maximumError);
    CHECK(maximumError < 0.00001);
    audio.stopSessionAudio();
    CHECK(quota.live == 0);
    }
}

TEST_CASE("U11 audio restore: nonloop BGM keeps its buffered tail until it is consumed") {
    unsigned prefixFrames = 32257;
    SUBCASE("early in final source block") {}
    SUBCASE("last source frame pending") { prefixFrames = 32767; }
    SUBCASE("source length reached with pipeline tail") { prefixFrames = 32768; }
    SUBCASE("last linear history frame pending") { prefixFrames = 32769; }
    const auto bytes = patternWave();
    AudioWaveFile file(bytes);
    RestoreAudioQuota quota;
    SoLoudAudioEngine audio{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(audio.init());
    const auto handle = audio.playBGM(file.path, 0);
    REQUIRE(handle != 0);
    audio.soloud().setLooping(handle, false);
    for (unsigned consumed = 0; consumed < prefixFrames;) {
        const unsigned frames = (std::min)(128u, prefixFrames - consumed);
        std::vector<float> block(frames * 2);
        audio.soloud().mix(block.data(), frames);
        consumed += frames;
    }
    const auto saved = audio.captureAudioState();
    CAPTURE(prefixFrames);
    CHECK(saved.bgmPath == file.path);
    CHECK_FALSE(saved.looping);
    auto prepared = audio.prepareAudioState(saved, bytes.data(), bytes.size());
    REQUIRE(prepared != nullptr);
    std::vector<float> continuous(4097 * 2), restored(4097 * 2);
    audio.soloud().mix(continuous.data(), 4097);
    double signal = 0;
    for (const float sample : continuous) signal += std::abs(sample);
    CHECK(signal > 0.00001);
    REQUIRE(audio.applyAudioState(std::move(prepared)));
    audio.soloud().mix(restored.data(), 4097);
    const auto maximumError = pcmDifference(continuous, restored);
    CAPTURE(maximumError);
    CHECK(maximumError < 0.00001);
    CHECK_FALSE(audio.isBGMPlaying());
    CHECK(audio.captureAudioState().bgmPath.empty());
    CHECK(quota.live == 0);
}

TEST_CASE("U11 audio restore: virtualized BGM cannot claim an exact sample cursor") {
    const auto bytes = patternWave();
    AudioWaveFile file(bytes);
    RestoreAudioQuota quota;
    SoLoudAudioEngine audio{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(audio.init());
    REQUIRE(audio.playBGM(file.path, 0) != 0);
    std::vector<float> prefix(256 * 2);
    audio.soloud().mix(prefix.data(), 256);
    CHECK(audio.captureAudioState().framePosition == 256);
    // The three protected/ticking buses consume this budget. The BGM is no
    // longer in the active mix list; its old buffer pointer is not evidence of
    // participation after setMaxActiveVoiceCount reallocated that storage.
    REQUIRE(audio.soloud().setMaxActiveVoiceCount(3) == SoLoud::SO_NO_ERROR);
    CHECK(audio.soloud().getActiveVoiceCount() == 3);
    std::vector<float> output(512 * 2);
    audio.soloud().mix(output.data(), 512);
    CHECK_THROWS_AS(audio.captureAudioState(), std::runtime_error);
    CHECK(audio.isBGMPlaying());
    audio.stopSessionAudio();
    CHECK(quota.live == 0);
}

TEST_CASE("U11 audio restore: public voice budgets map their complete supported range") {
    const auto bytes = patternWave();
    AudioWaveFile file(bytes);
    RestoreAudioQuota quota;
    SoLoudAudioEngine audio{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(audio.init());
    for (const unsigned budget : {256u, 1023u}) {
        REQUIRE(audio.soloud().setMaxActiveVoiceCount(budget) == SoLoud::SO_NO_ERROR);
        REQUIRE(audio.playBGM(file.path, 0) != 0);
        CHECK(audio.soloud().getActiveVoiceCount() == 4);
        std::vector<float> output(512 * 2);
        audio.soloud().mix(output.data(), 512);
        CHECK(audio.captureAudioState().bgmPath == file.path);
        CHECK(std::any_of(output.begin(), output.end(), [](float value) { return std::abs(value) > 0.001f; }));
        audio.stopSessionAudio();
        CHECK(quota.live == 0);
    }
}


TEST_CASE("Audio U27 snapshot: reusable waves and restored session sources have separate lifetimes") {
    RestoreAudioQuota quota;
    const auto bytes = silentWave();
    AudioWaveFile file(bytes);
    SoLoudAudioEngine audio{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(audio.init());
    REQUIRE(audio.soloud().getBackendId() == SoLoud::Soloud::NULLDRIVER);
    const auto checkIdle = [&](bool running, uint64_t buses, uint64_t waves) {
        const auto state = audio.getSnapshot();
        CHECK(state.supported);
        CHECK(state.running == running);
        CHECK(state.outputMode == AudioOutputMode::ManualMix);
        CHECK(state.busVoices == buses);
        CHECK(state.liveVoices == 0);
        CHECK(state.sessionHandles == 0);
        CHECK(state.retiringBGM == 0);
        CHECK(state.retiringVoice == 0);
        CHECK(state.rawCacheEntries == 0);
        CHECK(state.voiceCompletionsPending == 0);
        CHECK(state.restoredSources == 0);
        CHECK(state.waveCacheEntries == waves);
    };
    checkIdle(true, 3, 0);
    for (unsigned cycle = 0; cycle != 4; ++cycle) {
        CAPTURE(cycle);
        const auto handle = audio.playSE(file.path);
        REQUIRE(handle != 0);
        CHECK(audio.getSnapshot().waveCacheEntries == 1);
        CHECK(audio.getSnapshot().liveVoices == 1);
        CHECK(audio.getSnapshot().sessionHandles == 1);
        audio.flushWaveCache(); // A playing source must remain owned.
        CHECK(audio.getSnapshot().waveCacheEntries == 1);
        CHECK(audio.soloud().isValidVoiceHandle(handle));
        CHECK(quota.live == 1);
        audio.stopSessionAudio();
        checkIdle(true, 3, 1); // Fixed-asset cache plateau, not a live handle leak.
        CHECK(quota.live == 0);
    }
    CHECK(audio.playSE(file.path + ".missing") == 0);
    checkIdle(true, 3, 1);

    const AudioRestoreState state{"assets/u27-restored.wav", 0.25, 0.75f, true};
    auto prepared = audio.prepareAudioState(state, bytes.data(), bytes.size());
    REQUIRE(prepared != nullptr);
    checkIdle(true, 3, 1); // A caller-owned preparation is outside backend counts.
    REQUIRE(audio.applyAudioState(std::move(prepared)));
    const auto restored = audio.getSnapshot();
    CHECK(restored.liveVoices == 1);
    CHECK(restored.sessionHandles == 1); // Restored handle is an alias, not +1.
    CHECK(restored.restoredSources == 1);
    CHECK(restored.waveCacheEntries == 1); // Restore did not enter the file cache.
    CHECK(restored.rawCacheEntries == 0);
    CHECK(quota.live == 1);
    audio.stopBGM(0);
    CHECK(quota.live == 0);
    const auto awaitingCull = audio.getSnapshot();
    CHECK(awaitingCull.liveVoices == 0);
    CHECK(awaitingCull.sessionHandles == 0);
    CHECK(awaitingCull.restoredSources == 1);
    CHECK(audio.getSnapshot().restoredSources == 1); // Observation cannot release it.
    audio.update(0);
    checkIdle(true, 3, 1);

    prepared = audio.prepareAudioState(state, bytes.data(), bytes.size());
    REQUIRE(prepared != nullptr);
    REQUIRE(audio.applyAudioState(std::move(prepared)));
    CHECK(audio.getSnapshot().restoredSources == 1);
    audio.stopSessionAudio(); // Hard session boundary clears restored owners too.
    checkIdle(true, 3, 1);
    CHECK(quota.live == 0);
    audio.flushWaveCache();
    checkIdle(true, 3, 0);
    audio.shutdown();
    checkIdle(false, 0, 0);
    REQUIRE(audio.init());
    checkIdle(true, 3, 0);
    audio.shutdown();
    checkIdle(false, 0, 0);
    CHECK(quota.live == 0);
}
