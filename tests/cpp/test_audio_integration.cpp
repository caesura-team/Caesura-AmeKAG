// test_audio_integration.cpp - audio tests not covered by test_audio.cpp
#include "doctest.h"
#include "audio/SoLoudAudioEngine.h"
#include <cstring>

using namespace Caesura;

TEST_CASE("Audio: shutdown then re-init succeeds") {
    SoLoudAudioEngine eng;
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }
    eng.shutdown();
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }
    eng.shutdown();
}

TEST_CASE("Audio: playSE before init returns 0") {
    SoLoudAudioEngine eng;
    unsigned int h = eng.playSE("tests/audio/silence.wav");
    CHECK(h == 0);
}

TEST_CASE("Audio: bus volume set/get includes SE bus") {
    SoLoudAudioEngine eng;
    if (!eng.init()) {
        MESSAGE("Audio device unavailable, skipping");
        return;
    }
    eng.setBusVolume("bgm", 0.75f);
    CHECK(eng.getBusVolume("bgm") == doctest::Approx(0.75f));
    eng.setBusVolume("voice", 0.5f);
    CHECK(eng.getBusVolume("voice") == doctest::Approx(0.5f));
    eng.setBusVolume("se", 0.9f);
    CHECK(eng.getBusVolume("se") == doctest::Approx(0.9f));
}

TEST_CASE("Audio: voice pool overlap survives explicit mixing (manual mix)") {
    SoLoudAudioEngine eng{SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(eng.init());
    REQUIRE(eng.soloud().getBackendId() == SoLoud::Soloud::NULLDRIVER);
    REQUIRE(eng.soloud().getBackendSamplerate() == 48000);
    float pcm[512 * 2]{};
    // Own the real mixer's clock: the 100 ms fixture cannot expire in a
    // device callback before looping is set. Two lines use separate pool slots.
    const unsigned int h1 = eng.playVoice("tests/audio/silence.wav");
    REQUIRE(h1 != 0);
    eng.soloud().setLooping(h1, true);
    eng.soloud().mix(pcm, 512);
    const unsigned int h2 = eng.playVoice("tests/audio/silence.wav");
    REQUIRE(h2 != 0);
    eng.soloud().setLooping(h2, true);
    eng.soloud().mix(pcm, 512);
    CHECK(h1 != h2);
    CHECK(eng.soloud().isValidVoiceHandle(h1));
    CHECK(eng.soloud().isValidVoiceHandle(h2));
    CHECK(eng.isVoicePlaying() == true);
    eng.stopVoice();
    CHECK(eng.isVoicePlaying() == false);
    eng.shutdown();
}
