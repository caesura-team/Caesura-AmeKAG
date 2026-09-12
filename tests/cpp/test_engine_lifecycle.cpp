#include "doctest.h"
#include "../src/di/BackendRegistry.h"
#include "../src/debug/DebugManager.h"
#include "../src/job/JobSystem.h"
#include "../src/entry/Engine.h"
#include "../src/entry/EngineConfig.h"
#include "../src/render/NullRenderDevice.h"
#include "../src/audio/NullAudioBackend.h"
#include "../src/platform/NullPlatformBackend.h"
#include "../src/minigame/NullMiniGameBackend.h"
#include <memory>
#include <utility>

using namespace Caesura;

// Engine lifecycle integration tests

TEST_CASE("BackendRegistry::singleton") {
    auto& a = Caesura::BackendRegistry::instance();
    auto& b = Caesura::BackendRegistry::instance();
    CHECK(&a == &b);
}

TEST_CASE("BackendRegistry::null backends at startup") {
    auto& reg = Caesura::BackendRegistry::instance();
    CHECK(reg.getRenderDevice() == nullptr);
    CHECK(reg.getAudioBackend() == nullptr);
}

TEST_CASE("BackendRegistry::miniGame null initially") {
    auto& reg = Caesura::BackendRegistry::instance();
    CHECK(reg.getMiniGameBackend() == nullptr);
}

TEST_CASE("DebugManager::singleton") {
    auto& a = Caesura::DebugManager::instance();
    auto& b = Caesura::DebugManager::instance();
    CHECK(&a == &b);
}

TEST_CASE("JobSystem instances are independently owned") {
    Caesura::JobSystem a;
    Caesura::JobSystem b;
    CHECK(&a != &b);
    CHECK_FALSE(a.isRunning());
    CHECK_FALSE(b.isRunning());
}

// Engine lifecycle guards (audit): double-init, shutdown-without-init,
// and double-shutdown must be safe no-ops, and service access before
// init must throw (requireInitialized).

static EngineConfig makeConfig() {
    EngineConfig cfg;
    cfg.headless = true;
    return cfg;
}

TEST_CASE("Engine: double init is rejected") {
    Engine engine(makeConfig());
    CHECK(engine.init());
    CHECK_FALSE(engine.init());  // second call refused
    engine.shutdown();
}

TEST_CASE("Engine: shutdown without init is safe + double shutdown idempotent") {
    Engine engine(makeConfig());
    engine.shutdown();          // never initialized
    CHECK_NOTHROW(engine.shutdown());  // again -- must not throw/crash
}

TEST_CASE("Engine: service access before init throws") {
    Engine engine(makeConfig());
    bool threw = false;
    try {
        (void)engine.renderDevice();
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw);
}

// t56: a real audio backend whose init() fails must degrade to the silent
// Null backend instead of killing engine startup (macOS vendored-SoLoud
// no-backend regression: 'Audio backend init failed.' -> hard exit).
// Exercise the failure through the same EngineConfig::audio seam main.cpp
// uses for SoLoudAudioEngine; headless defaults supply platform/render
// nulls, so no GPU or display is needed.
namespace {
struct FailingAudioBackend : IAudioBackend {
    bool init() override { return false; }
    void shutdown() override {}
    bool isPlaybackAvailable() const override { return false; }
    void update(float) override {}
    void suspend() override {}
    void resume() override {}
    unsigned int playBGM(const std::string&, float) override { return 0; }
    void stopBGM(float) override {}
    unsigned int playVoice(const std::string&) override { return 0; }
    void stopVoice() override {}
    unsigned int playSE(const std::string&) override { return 0; }
    unsigned int playRawPCM(const float*, unsigned int, unsigned int,
                            unsigned int) override { return 0; }
    unsigned int playSE3D(const std::string&, float, float, float) override { return 0; }
    void stopSE() override {}
    void setSEVolume(unsigned int, float) override {}
    float getSEVolume(unsigned int) override { return 0.0f; }
    void stopSEHandle(unsigned int) override {}
    void update3dListener(float, float, float, float, float, float,
                          float, float, float) override {}
    void setGlobalVolume(float) override {}
    float getGlobalVolume() const override { return 1.0f; }
    void setBusVolume(const char*, float) override {}
    float getBusVolume(const char*) const override { return 0.0f; }
    void flushWaveCache() override {}
    unsigned int consumeVoiceCompletions() override { return 0; }
    bool isVoicePlaying() override { return false; }
    bool isBGMPlaying() override { return false; }
    bool isSEPlaying() override { return false; }
    int activeVoiceCount() override { return 0; }
    float getPosition(const char*) override { return 0.0f; }
    float getLength(const char*) override { return 0.0f; }
    void fadeVolume(const char*, float, float) override {}
    const char* getBackendName() const override { return "FailingAudio"; }
};
} // namespace

TEST_CASE("Engine: failing real audio init degrades to NullAudio (t56)") {
    auto& reg = BackendRegistry::instance();
    EngineConfig cfg = makeConfig();          // headless defaults: Null platform/render
    cfg.audio = new FailingAudioBackend();    // real-backend failure seam
    Engine engine(std::move(cfg));
    CHECK(engine.init());                     // must NOT be killed by the audio failure
    CHECK(reg.getAudioBackend() != nullptr);
    CHECK(dynamic_cast<NullAudioBackend*>(reg.getAudioBackend()) != nullptr);
    // the muted engine remains fully serviceable
    CHECK(engine.audio().getBackendName() != nullptr);
    engine.shutdown();
}

// ============================================================================
// Engine lifecycle boundary tests (task): clean round-trip, defaults,
// explicit backend overrides, and destroy-after-shutdown safety.
// ============================================================================

// (a) construct + init + shutdown completes cleanly with no crash / no
// double-free. shutdown() is idempotent after a successful init.
TEST_CASE("Engine: clean construct+init+shutdown roundtrip") {
    Engine engine(makeConfig());
    REQUIRE(engine.init());
    engine.shutdown();
    CHECK_NOTHROW(engine.shutdown());  // second shutdown must be a no-op
}

// (f) destroy after shutdown -- the documented lifecycle order -- must be
// safe. The destructor observes m_shutdownComplete and must NOT re-enter
// shutdown (no double-free of the owned backends).
TEST_CASE("Engine: destroy after shutdown is safe") {
    auto engine = std::make_unique<Engine>(makeConfig());
    REQUIRE(engine->init());
    engine->shutdown();
    engine.reset();  // destructor runs with m_shutdownComplete already set
    CHECK(true);     // reaching here means destroy-after-shutdown is safe
}

// (f-alt) destroying an initialized but never-shutdown Engine is also safe:
// the destructor must auto-shutdown (idempotent), freeing all owned backends.
TEST_CASE("Engine: destroy after init (no explicit shutdown) is safe") {
    auto engine = std::make_unique<Engine>(makeConfig());
    REQUIRE(engine->init());
    engine.reset();  // destructor triggers shutdown()
    CHECK(true);
}

// (d-i) EngineConfig carries sane defaults and leaves all backend pointers
// null, so the Engine's composition root fills in placeholder backends.
TEST_CASE("EngineConfig: defaults yield a working minimal config") {
    EngineConfig cfg;
    CHECK(cfg.title == std::string("Caesura (AmeKAG)"));
    CHECK(cfg.width == 1280);
    CHECK(cfg.height == 720);
    CHECK_FALSE(cfg.headless);
    CHECK_FALSE(cfg.editorMode);
    CHECK(cfg.frameLimit == 0);
    CHECK(cfg.render == nullptr);
    CHECK(cfg.audio == nullptr);
    CHECK(cfg.platform == nullptr);
    CHECK(cfg.gpuMonitor == nullptr);
    CHECK(cfg.miniGame == nullptr);
    CHECK(cfg.animation == nullptr);

    // With no explicit backends the engine still builds a minimal, working
    // config: init succeeds and the registry is populated with placeholder
    // (Null) backends.
    Engine engine(makeConfig());
    REQUIRE(engine.init());
    auto& reg = BackendRegistry::instance();
    CHECK(reg.getRenderDevice() != nullptr);
    CHECK(reg.getAudioBackend() != nullptr);
    CHECK(reg.getPlatformBackend() != nullptr);
    CHECK(reg.getMiniGameBackend() != nullptr);
    CHECK(reg.getAnimationBackend() != nullptr);
    CHECK(reg.getJobSystem() != nullptr);
    engine.shutdown();
    // After shutdown the registry must be unregistered again.
    CHECK(reg.getRenderDevice() == nullptr);
    CHECK(reg.getAudioBackend() == nullptr);
}

// (e) Explicit backend overrides injected through EngineConfig are wired
// through to the registry (verified by pointer equality). Engine takes
// ownership of the injected objects, so the test must NOT free them.
TEST_CASE("Engine: explicit backend overrides wire through to registry") {
    auto* injectedRender   = new NullRenderDevice();
    auto* injectedAudio    = new NullAudioBackend();
    auto* injectedPlatform = new NullPlatformBackend();
    auto* injectedMiniGame = new NullMiniGameBackend();

    EngineConfig cfg;
    cfg.headless   = true;
    cfg.render     = injectedRender;
    cfg.audio      = injectedAudio;
    cfg.platform   = injectedPlatform;
    cfg.miniGame   = injectedMiniGame;

    {
        Engine engine(std::move(cfg));  // ownership transfers to Engine
        REQUIRE(engine.init());
        BackendRegistry& reg = BackendRegistry::instance();
        CHECK(reg.getRenderDevice() == injectedRender);
        CHECK(reg.getAudioBackend() == injectedAudio);
        CHECK(reg.getPlatformBackend() == injectedPlatform);
        CHECK(reg.getMiniGameBackend() == injectedMiniGame);
        engine.shutdown();
    }  // ~Engine frees the injected backends it now owns (no double-free)
    CHECK(BackendRegistry::instance().getRenderDevice() == nullptr);
}

// ============================================================================
// EngineConfig / Engine launch-parameter boundary tests (round 81+5 follow-up).
// Coverage: restart round-trip, registry-cleared integrity, headless-vs-explicit
// backend precedence, partial/all-Null backend injection, invalid-dimension
// acceptance, and end-of-init field passthrough.
// ============================================================================

// (4a) init -> shutdown -> init again is REFUSED: init() is once-per-instance
// (m_initAttempted guards re-entry). A restart therefore requires a fresh
// Engine. After the refused re-init the registry must still be empty and
// service access must still throw.
TEST_CASE("Engine: init->shutdown->re-init refused (once per instance)") {
    Engine engine(makeConfig());
    REQUIRE(engine.init());
    engine.shutdown();

    auto& reg = BackendRegistry::instance();
    CHECK(reg.getRenderDevice() == nullptr);
    CHECK(reg.getAudioBackend() == nullptr);

    // Re-init on the same instance is refused after shutdown; service access
    // is still guarded (m_initialized is false).
    bool threw = false;
    try {
        (void)engine.renderDevice();
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw);               // access still guarded
    CHECK_FALSE(engine.init()); // restart round-trip not supported in-place

    // Registry untouched (still cleared) and shutdown remains idempotent.
    CHECK(reg.getRenderDevice() == nullptr);
    CHECK_NOTHROW(engine.shutdown());
}

// (4b) Dedicated full-registry integrity check: after shutdown every backend
// slot the engine owns is unregistered, not just the headline render/audio
// pair that round 81 verified inline. Locks the full set.
TEST_CASE("Engine: shutdown clears the entire owned registry") {
    {
        Engine engine(makeConfig());
        REQUIRE(engine.init());
        BackendRegistry& reg = BackendRegistry::instance();
        CHECK(reg.getRenderDevice() != nullptr);
        CHECK(reg.getAudioBackend() != nullptr);
        CHECK(reg.getAudioRestore() != nullptr);
        CHECK(reg.getPlatformBackend() != nullptr);
        CHECK(reg.getMiniGameBackend() != nullptr);
        CHECK(reg.getAnimationBackend() != nullptr);
        CHECK(reg.getJobSystem() != nullptr);
        CHECK(reg.getCryptoEngine() != nullptr);
        CHECK(reg.getAssetReader() != nullptr);
        CHECK(reg.getImageDecoder() != nullptr);
        engine.shutdown();
    }
    BackendRegistry& reg = BackendRegistry::instance();
    CHECK(reg.getRenderDevice() == nullptr);
    CHECK(reg.getAudioBackend() == nullptr);
    CHECK(reg.getAudioRestore() == nullptr);
    CHECK(reg.getPlatformBackend() == nullptr);
    CHECK(reg.getMiniGameBackend() == nullptr);
    CHECK(reg.getAnimationBackend() == nullptr);
    CHECK(reg.getJobSystem() == nullptr);
    CHECK(reg.getCryptoEngine() == nullptr);
    CHECK(reg.getAssetReader() == nullptr);
    CHECK(reg.getImageDecoder() == nullptr);
}

// (2a) headless=true selects Null (no-GPU) adapters in the registry. The
// registry pointers are non-null but the concrete types report the Null
// backend name -- locked via the interface (no GPU resources in CI).
TEST_CASE("Engine: headless mode registers Null (no-GPU) backends") {
    Engine engine(makeConfig());  // headless=true
    REQUIRE(engine.init());
    BackendRegistry& reg = BackendRegistry::instance();
    REQUIRE(reg.getRenderDevice() != nullptr);
    REQUIRE(reg.getAudioBackend() != nullptr);
    REQUIRE(reg.getPlatformBackend() != nullptr);
    CHECK(std::string(reg.getRenderDevice()->getBackendName()) == "NullRender");
    CHECK(std::string(reg.getAudioBackend()->getBackendName())   == "NullAudio");
    CHECK(std::string(reg.getPlatformBackend()->getBackendName())== "NullPlatform");
    engine.shutdown();
}

// (2b) headless + explicit backend conflict: the explicitly injected backend
// WINS. In init(), the headless Null defaults are created only when the slot
// is still empty, so an injected render device is never replaced.
TEST_CASE("Engine: explicit backend wins over headless null default") {
    auto* injectedRender = new NullRenderDevice();
    EngineConfig cfg;
    cfg.headless = true;              // conflicting signal on purpose
    cfg.render   = injectedRender;    // explicit backend intent
    {
        Engine engine(std::move(cfg));
        REQUIRE(engine.init());
        CHECK(BackendRegistry::instance().getRenderDevice() == injectedRender);
        engine.shutdown();
    }
    CHECK(BackendRegistry::instance().getRenderDevice() == nullptr);
}

// (3a) Partial backend injection: supply only a render device (and platform)
// and leave the rest null. In headless mode the audio slot is auto-filled with
// a Null default, while the injected pointers pass straight through.
TEST_CASE("Engine: partial backend injection fills missing with null defaults") {
    auto* injectedRender   = new NullRenderDevice();
    auto* injectedPlatform = new NullPlatformBackend();
    EngineConfig cfg;
    cfg.headless = true;
    cfg.render   = injectedRender;
    cfg.platform = injectedPlatform;  // audio + minigame intentionally omitted
    {
        Engine engine(std::move(cfg));
        REQUIRE(engine.init());
        BackendRegistry& reg = BackendRegistry::instance();
        CHECK(reg.getRenderDevice() == injectedRender);
        CHECK(reg.getPlatformBackend() == injectedPlatform);
        // Missing slot auto-filled by headless Null default (not dangling).
        REQUIRE(reg.getAudioBackend() != nullptr);
        CHECK(std::string(reg.getAudioBackend()->getBackendName()) == "NullAudio");
        // miniGame is default-filled by initOptionalPhase.
        REQUIRE(reg.getMiniGameBackend() != nullptr);
        engine.shutdown();
    }
    CHECK(BackendRegistry::instance().getRenderDevice() == nullptr);
}

// (3b) All-Null injection: every core slot explicitly supplied as a Null
// backend registers by pointer equality (no double-ownership, no leak).
TEST_CASE("Engine: all-Null injection wires through to registry") {
    auto* injectedRender   = new NullRenderDevice();
    auto* injectedAudio    = new NullAudioBackend();
    auto* injectedPlatform = new NullPlatformBackend();
    auto* injectedMiniGame = new NullMiniGameBackend();
    EngineConfig cfg;
    cfg.headless = true;
    cfg.render   = injectedRender;
    cfg.audio    = injectedAudio;
    cfg.platform = injectedPlatform;
    cfg.miniGame = injectedMiniGame;
    {
        Engine engine(std::move(cfg));
        REQUIRE(engine.init());
        BackendRegistry& reg = BackendRegistry::instance();
        CHECK(reg.getRenderDevice()  == injectedRender);
        CHECK(reg.getAudioBackend()  == injectedAudio);
        CHECK(reg.getPlatformBackend()== injectedPlatform);
        CHECK(reg.getMiniGameBackend()== injectedMiniGame);
        engine.shutdown();
    }
    CHECK(BackendRegistry::instance().getRenderDevice() == nullptr);
}

// (1a) Launch parameters pass through to config() after a full init -- the
// engine never rewrites the caller's width/height/title/frameLimit.
TEST_CASE("Engine: field values pass through to config() after init") {
    EngineConfig cfg;
    cfg.title       = "Boundary Test Title";
    cfg.width       = 960;
    cfg.height      = 540;
    cfg.headless    = true;
    cfg.editorMode  = false;
    cfg.frameLimit  = 5;
    Engine engine(std::move(cfg));
    REQUIRE(engine.init());
    const EngineConfig& c = engine.config();
    CHECK(std::string(c.title) == "Boundary Test Title");
    CHECK(c.width == 960);
    CHECK(c.height == 540);
    CHECK(c.headless);
    CHECK_FALSE(c.editorMode);
    CHECK(c.frameLimit == 5);
    engine.shutdown();
}

// (1b) Negative window dimensions on the config are ACCEPTED (not clamped or
// rejected): in headless mode the NullPlatformBackend stores and reports the
// raw value verbatim. This locks the current no-clamp behavior; a future clamp
// would need to update this test. (Record-only note: the engine does NOT
// validate dimensions today -- main.cpp is the actual guard for real windows.)
TEST_CASE("Engine: negative dimensions accepted with no clamp (headless)") {
    EngineConfig cfg;
    cfg.headless = true;
    cfg.width  = -1;
    cfg.height = -2;
    Engine engine(std::move(cfg));
    REQUIRE(engine.init());  // must not throw / crash on negative dims
    CHECK(engine.platform().getWindowWidth()  == -1);
    CHECK(engine.platform().getWindowHeight() == -2);
    engine.shutdown();
}
