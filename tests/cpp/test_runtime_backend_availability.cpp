#include "doctest.h"

#include "audio/NullAudioBackend.h"
#include "audio/SoLoudAudioEngine.h"
#include "live2d/NullAnimationBackend.h"
#include "steam/NullSteamBackend.h"
#include "steam/SteamBackend.h"

#ifdef CAESURA_LIVE2D
#include "live2d/Live2D/Live2DBackend.h"
#endif

using namespace Caesura;

TEST_CASE("Runtime backend availability: silent audio stays unavailable after init") {
    NullAudioBackend audio;
    const IAudioBackend& query = audio;

    CHECK_FALSE(query.isPlaybackAvailable());
    REQUIRE(audio.init());
    CHECK_FALSE(query.isPlaybackAvailable());
    audio.suspend();
    audio.resume();
    CHECK_FALSE(query.isPlaybackAvailable());
    audio.shutdown();
    CHECK_FALSE(query.isPlaybackAvailable());
    audio.shutdown();
    CHECK_FALSE(query.isPlaybackAvailable());
}

TEST_CASE("Runtime backend availability: default SoLoud starts unavailable") {
    // Construction and shutdown must not require an audio device.
    SoLoudAudioEngine audio;
    const IAudioBackend& query = audio;

    CHECK_FALSE(query.isPlaybackAvailable());
    audio.shutdown();
    CHECK_FALSE(query.isPlaybackAvailable());
}

TEST_CASE("Runtime backend availability: real mixer follows its session lifecycle") {
    // Reuse the production ManualMix mode used by the audio-restore tests.
    // This proves mixer initialization, not physical device output or an asset.
    SoLoudAudioEngine audio{SoLoudAudioEngine::OutputMode::ManualMix};
    const IAudioBackend& query = audio;

    CHECK_FALSE(query.isPlaybackAvailable());
    REQUIRE(audio.init());
    REQUIRE(audio.soloud().getBackendId() == SoLoud::Soloud::NULLDRIVER);
    CHECK(query.isPlaybackAvailable());
    REQUIRE(audio.init());
    CHECK(query.isPlaybackAvailable());
    audio.suspend();
    CHECK(query.isPlaybackAvailable());
    audio.resume();
    CHECK(query.isPlaybackAvailable());
    audio.shutdown();
    CHECK_FALSE(query.isPlaybackAvailable());
    audio.shutdown();
    CHECK_FALSE(query.isPlaybackAvailable());
    REQUIRE(audio.init());
    CHECK(query.isPlaybackAvailable());
    audio.shutdown();
    CHECK_FALSE(query.isPlaybackAvailable());
}

TEST_CASE("Runtime backend availability: static image fallback never reports Cubism") {
    NullAnimationBackend animation;
    const IAnimationBackend& query = animation;

    CHECK_FALSE(query.isCubismAvailable());
    REQUIRE(animation.init());
    CHECK_FALSE(query.isCubismAvailable());
    REQUIRE(animation.init());
    CHECK_FALSE(query.isCubismAvailable());
    animation.shutdown();
    CHECK_FALSE(query.isCubismAvailable());
    animation.shutdown();
    CHECK_FALSE(query.isCubismAvailable());
}

#ifdef CAESURA_LIVE2D
TEST_CASE("Runtime backend availability: default Cubism starts unavailable") {
    // A real GPU and SDK session are deliberately not opened by this test.
    Live2DBackend animation;
    const IAnimationBackend& query = animation;

    CHECK_FALSE(query.isCubismAvailable());
    animation.shutdown();
    CHECK_FALSE(query.isCubismAvailable());
    animation.shutdown();
    CHECK_FALSE(query.isCubismAvailable());
}
#endif

TEST_CASE("Runtime backend availability: null Steam stays unavailable") {
    NullSteamBackend steam;
    const ISteamBackend& query = steam;

    CHECK_FALSE(query.isAvailable());
    CHECK_FALSE(steam.init());
    CHECK_FALSE(query.isAvailable());
    steam.shutdown();
    CHECK_FALSE(query.isAvailable());
    steam.shutdown();
    CHECK_FALSE(query.isAvailable());
}

TEST_CASE("Runtime backend availability: default Steam starts unavailable") {
    // No Steam client or account action is needed for an uninitialized query.
    SteamBackend steam;
    const ISteamBackend& query = steam;

    CHECK_FALSE(query.isAvailable());
    steam.shutdown();
    CHECK_FALSE(query.isAvailable());
#ifndef CAESURA_HAS_STEAM
    CHECK_FALSE(steam.init());
    CHECK_FALSE(query.isAvailable());
    steam.shutdown();
    CHECK_FALSE(query.isAvailable());
#endif
}
