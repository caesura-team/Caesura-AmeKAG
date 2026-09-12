#include "doctest.h"
#include <algorithm>
#include <cstring>
#include "audio/SoLoudAudioEngine.h"
#include "audio/AudioFocusService.h"
#include "audio/api/IAudioFocusService.h"
#include "di/BackendRegistry.h"
#include "di/api/ISandboxQuota.h"
#include "job/JobSystem.h"
#include <atomic>
#include <thread>

using namespace Caesura;

namespace {

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

} // namespace

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
    SoLoudAudioEngine eng;
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }
    unsigned int h = eng.playBGM("tests/audio/silence.wav", 0.0f);
    CHECK(h > 0);
    CHECK(eng.isBGMPlaying());
    eng.stopBGM(0.0f);
}

TEST_CASE("SoLoudAudioEngine::playVoice and stopVoice with silence") {
    SoLoudAudioEngine eng;
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }
    unsigned int h = eng.playVoice("tests/audio/silence.wav");
    CHECK(h > 0);
    CHECK(eng.isVoicePlaying());
    eng.stopVoice();
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
    SoLoudAudioEngine eng;
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }
    unsigned int h = eng.playSE("tests/audio/silence.wav");
    REQUIRE(h > 0);
    eng.setSEVolume(h, 0.5f);
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
    SoLoudAudioEngine eng;
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }

    REQUIRE(eng.playSE("tests/audio/silence.wav") != 0);
    REQUIRE(eng.playSE3D("tests/audio/silence.wav", 0.0f, 0.0f, -1.0f) != 0);
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
    SoLoudAudioEngine eng;
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }

    REQUIRE(eng.playBGM("tests/audio/silence.wav", 0.0f) != 0);
    REQUIRE(quota.activeCount == 1);
    REQUIRE(eng.playBGM("tests/audio/silence.wav", 0.0f) != 0);
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
    SoLoudAudioEngine eng;
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }

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
    REQUIRE(quota.activeCount == 6);

    eng.shutdown();
    CHECK(quota.activeCount == 0);
    CHECK(quota.releaseCalls == 6);
    CHECK(quota.releaseUnderflows == 0);

    eng.shutdown();
    CHECK(quota.releaseCalls == 6);
}


TEST_CASE("SoLoudAudioEngine playRawPCM plays and stops cleanly") {
    SoLoudAudioEngine eng;
    if (!eng.init()) return;  // no audio device (headless CI): skip
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
    SoLoudAudioEngine eng;
    if (!eng.init()) { MESSAGE("Audio device unavailable, skipping"); return; }

    const unsigned int bgm = eng.playBGM("tests/audio/silence.wav", 0.0f);
    REQUIRE(bgm != 0);
    eng.soloud().setLooping(bgm, true);
    REQUIRE(eng.isBGMPlaying());

    // Playing an SE must not disturb the BGM bus.
    const unsigned int se = eng.playSE("tests/audio/silence.wav");
    REQUIRE(se != 0);
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
    CHECK(eng.isSEPlaying());
    eng.stopBGM(0.0f);
    CHECK_FALSE(eng.isBGMPlaying());
    CHECK(eng.isSEPlaying());           // SE survived the BGM stop

    eng.stopSE();
}

TEST_CASE("SoLoudAudioEngine voice keeps overlapping characters (pool, no single-slot kill)") {
    SoLoudAudioEngine eng;
    if (!eng.init()) { MESSAGE("Audio device unavailable, skipping"); return; }

    // The engine uses a round-robin 4-slot voice pool: a new voice does NOT
    // hard-stop the previous one (single-slot semantics were removed). Both
    // handles coexist in the pool and stay valid independently.
    const unsigned int v1 = eng.playVoice("tests/audio/silence.wav");
    REQUIRE(v1 != 0);
    eng.soloud().setLooping(v1, true);
    CHECK(eng.isVoicePlaying());

    const unsigned int v2 = eng.playVoice("tests/audio/silence.wav");
    REQUIRE(v2 != 0);
    eng.soloud().setLooping(v2, true);
    CHECK(eng.isVoicePlaying());
    CHECK(eng.soloud().isValidVoiceHandle(v1));  // first voice still alive
    CHECK(eng.soloud().isValidVoiceHandle(v2));

    // stopVoice() clears the whole pool; only NATURAL ends count as completions.
    eng.stopVoice();
    CHECK_FALSE(eng.isVoicePlaying());
    CHECK(eng.consumeVoiceCompletions() == 0);
}

TEST_CASE("SoLoudAudioEngine bus volume applies to the bus (documented approximation)") {
    SoLoudAudioEngine eng;
    if (!eng.init()) { MESSAGE("Audio device unavailable, skipping"); return; }

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
    SoLoudAudioEngine eng;
    if (!eng.init()) { MESSAGE("Audio device unavailable, skipping"); return; }

    const unsigned int bgm = eng.playBGM("tests/audio/silence.wav", 0.0f);
    REQUIRE(bgm != 0);
    eng.soloud().setLooping(bgm, true);
    const unsigned int voice = eng.playVoice("tests/audio/silence.wav");
    REQUIRE(voice != 0);
    eng.soloud().setLooping(voice, true);
    REQUIRE(eng.playSE("tests/audio/silence.wav") != 0);

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
    SoLoudAudioEngine eng;
    if (!eng.init()) { MESSAGE("Audio device unavailable, skipping"); return; }

    const unsigned int bgm = eng.playBGM("tests/audio/silence.wav", 0.0f);
    REQUIRE(bgm != 0);
    eng.soloud().setLooping(bgm, true);
    const unsigned int se = eng.playSE("tests/audio/silence.wav");
    REQUIRE(se != 0);

    // suspend() pauses the whole mixer; tracked handles stay alive so a
    // subsequent resume() continues them.
    eng.suspend();
    CHECK(eng.isBGMPlaying());
    CHECK(eng.isSEPlaying());
    CHECK(eng.soloud().isValidVoiceHandle(bgm));
    CHECK(eng.soloud().isValidVoiceHandle(se));

    eng.resume();
    eng.update(0.0f);
    CHECK(eng.isBGMPlaying());
    CHECK(eng.isSEPlaying());

    eng.stopBGM(0.0f);
    eng.stopSE();
}

TEST_CASE("SoLoudAudioEngine 3D position routing left/center/right yields valid handles") {
    SoLoudAudioEngine eng;
    if (!eng.init()) { MESSAGE("Audio device unavailable, skipping"); return; }

    // IAudioBackend exposes no direct pan (left/center/right) control; the
    // spatial routing it does expose is 3D positioning via playSE3D().
    const unsigned int left   = eng.playSE3D("tests/audio/silence.wav", -10.0f, 0.0f, 0.0f);
    const unsigned int center = eng.playSE3D("tests/audio/silence.wav",   0.0f, 0.0f, 0.0f);
    const unsigned int right  = eng.playSE3D("tests/audio/silence.wav",  10.0f, 0.0f, 0.0f);
    CHECK(left != 0);
    CHECK(center != 0);
    CHECK(right != 0);
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
    SoLoudAudioEngine eng;
    if (!eng.init()) { MESSAGE("Audio device unavailable, skipping"); return; }

    // A zero bus volume is the documented mute approximation: it persists and
    // drives the SoLoud bus to silence.
    eng.setBusVolume("se", 0.0f);
    CHECK(eng.getBusVolume("se") == doctest::Approx(0.0f));
    CHECK(eng.seBus().mVolume == doctest::Approx(0.0f));

    // Sanity: the SE bus still plays samples (muted bus does not block play).
    const unsigned int h = eng.playSE("tests/audio/silence.wav");
    REQUIRE(h != 0);
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
    SoLoudAudioEngine eng;
    if (!eng.init()) { MESSAGE("Audio device unavailable, skipping"); return; }

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
    SoLoudAudioEngine eng;
    if (!eng.init()) { MESSAGE("Audio device unavailable, skipping"); return; }

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

    eng.stopVoice();
    eng.update(0.0f);
    eng.shutdown();
    CHECK(quota.activeCount == 0);
    CHECK(quota.releaseUnderflows == 0);
}

TEST_CASE("SoLoudAudioEngine stop then immediately replay same handle is clean") {
    AudioQuota quota(8);
    ScopedAudioQuota scopedQuota(quota);
    SoLoudAudioEngine eng;
    if (!eng.init()) { MESSAGE("Audio device unavailable, skipping"); return; }

    const unsigned int first = eng.playSE("tests/audio/silence.wav");
    REQUIRE(first != 0);
    REQUIRE(quota.activeCount == 1);

    // stopSEHandle frees the slot and its quota immediately.
    eng.stopSEHandle(first);
    CHECK_FALSE(eng.soloud().isValidVoiceHandle(first));
    CHECK(quota.activeCount == 0);

    // Replaying the same FILE (same handle value pool) right after must give a
    // fresh, live handle and re-take the quota exactly once.
    const unsigned int again = eng.playSE("tests/audio/silence.wav");
    REQUIRE(again != 0);
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
    SoLoudAudioEngine eng;
    if (!eng.init()) { MESSAGE("Audio device unavailable, skipping"); return; }

    const unsigned int a = eng.playSE("tests/audio/silence.wav");
    REQUIRE(a != 0);
    const unsigned int b = eng.playSE("tests/audio/silence.wav");
    REQUIRE(b != 0);

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
    SoLoudAudioEngine eng;
    if (!eng.init()) { MESSAGE("Audio device unavailable, skipping"); return; }

    const unsigned int se = eng.playSE("tests/audio/silence.wav");
    REQUIRE(se != 0);
    REQUIRE(quota.activeCount == 1);
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
