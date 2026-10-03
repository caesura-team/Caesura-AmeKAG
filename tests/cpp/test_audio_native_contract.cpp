#include "doctest.h"
#include "audio/NullAudioBackend.h"
#include "audio/SoLoudAudioEngine.h"
#include "di/BackendRegistry.h"
#include "di/api/ISandboxQuota.h"
#include "script/bindings/DevCoreBinding.h"
#include "script/bindings/EngineBinding.h"
#include "script/bindings/KAGBinding.h"
#include "script/bindings/RenderBinding.h"
#include <filesystem>
#include <memory>
#include <string>
#include <array>
#include <limits>
#include <vector>
#include <cstdio>
#include <soloud_wav.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

namespace {
// Real Lua modules and registered C bindings call this hardware boundary.
// No replacement backend/factory/handler or nonexistent Lua method is injected.
class AudioBoundaryRecorder final : public Caesura::NullAudioBackend {
public:
    bool isPlaybackAvailable() const override { return true; }
    unsigned int playBGM(const std::string& value, float seconds) override {
        ++bgmCalls; file = value; bgmFade = seconds;
        bgmVolume = bgmBus; return admitted ? 101 : 0;
    }
    unsigned int playVoice(const std::string& value) override {
        ++voiceCalls; file = value; voiceVolume = voiceBus;
        return admitted ? 102 : 0;
    }
    unsigned int playSE(const std::string& value) override {
        ++seCalls; file = value; seVolume = seBus;
        return admitted ? 103 : 0;
    }
    unsigned int playSE3D(const std::string& value, float, float, float) override {
        return playSE(value);
    }
    // After the real interface gained typed clip options, observe those actual
    // arguments rather than estimating clip gain from a persistent bus. The
    // requested audible semantics are unchanged from the original RED.
    unsigned int playBGM(const std::string& value, const Caesura::AudioPlaybackOptions& options) override {
        const auto result = playBGM(value, options.fadeIn);
        bgmOptions = options; bgmVolume = options.volume; return result;
    }
    unsigned int playVoice(const std::string& value, const Caesura::AudioPlaybackOptions& options) override {
        const auto result = playVoice(value);
        voiceOptions = options; voiceVolume = options.volume; return result;
    }
    unsigned int playSE(const std::string& value, const Caesura::AudioPlaybackOptions& options) override {
        const auto result = playSE(value);
        seOptions = options; seVolume = options.volume; return result;
    }
    unsigned int playSE3D(const std::string& value, float x, float y, float z,
                         const Caesura::AudioPlaybackOptions& options) override {
        spatial = {x,y,z}; return playSE(value, options);
    }
    void stopBGM(float seconds) override { ++stopBgmCalls; stopBgmFade = seconds; }
    void stopSE() override { ++stopSeCalls; }
    void stopSE(float seconds) override { stopSeFade = seconds; stopSE(); }
    void setBusVolume(const char* bus, float volume) override {
        const std::string name = bus;
        if (name == "bgm") bgmVolume = bgmBus = volume;
        if (name == "voice") voiceVolume = voiceBus = volume;
        if (name == "se") seVolume = seBus = volume;
    }
    float getBusVolume(const char* bus) const override {
        const std::string name = bus;
        return name == "bgm" ? bgmBus : name == "voice" ? voiceBus : seBus;
    }
    void setSEVolume(unsigned int handle, float volume) override {
        if (handle == 103) seVolume = volume;
    }
    float getSEVolume(unsigned int handle) override { return handle == 103 ? seVolume : 0; }
    bool isVoicePlaying() override { return false; }
    bool isBGMPlaying() override { return false; }
    bool isSEPlaying() override { return false; }
    std::string file;
    bool admitted = true;
    unsigned bgmCalls = 0, voiceCalls = 0, seCalls = 0;
    unsigned stopBgmCalls = 0, stopSeCalls = 0;
    float bgmFade = -1, stopBgmFade = -1;
    float bgmBus = 1, voiceBus = 1, seBus = 1;
    float bgmVolume = 1, voiceVolume = 1, seVolume = 1;
    float stopSeFade = -1;
    Caesura::AudioPlaybackOptions bgmOptions, voiceOptions, seOptions;
    std::array<float,3> spatial{};
};

struct AudioNativeFixture {
    AudioBoundaryRecorder audio;
    Caesura::BackendRegistry& registry = Caesura::BackendRegistry::instance();
    Caesura::IAudioBackend* previous = registry.getAudioBackend();
    std::unique_ptr<lua_State, decltype(&lua_close)> lua{luaL_newstate(), lua_close};
    std::string error;
    AudioNativeFixture() {
        registry.setAudioBackend(&audio);
        if (!lua) return;
        luaL_openlibs(lua.get());
        Caesura::engine_binding::registerEngineBindings(lua.get());
        Caesura::registerKAGBinding(lua.get());
        Caesura::registerRenderBinding(lua.get());
        Caesura::registerDevCoreBinding(lua.get());
        const auto root = std::filesystem::path(CAESURA_SOURCE_DIR).generic_string();
        const auto search = root + "/scripts/?.lua;" + root + "/scripts/?/init.lua;";
        lua_pushlstring(lua.get(), search.data(), search.size());
        lua_setglobal(lua.get(), "AUDIO_CONTRACT_PACKAGE_PATH");
    }
    ~AudioNativeFixture() {
        registry.setAudioBackend(previous);
        if (lua) Caesura::registerKAGBinding(lua.get()); // Clear cached backend pointer.
    }
    bool run(const char* source) {
        lua_settop(lua.get(), 0);
        const int status = luaL_dostring(lua.get(), source);
        const char* message = status == LUA_OK ? nullptr : lua_tostring(lua.get(), -1);
        error = status == LUA_OK ? "" : message ? message : "non-string Lua error";
        return status == LUA_OK;
    }
    bool boot() {
        return run(R"lua(
            package.path = AUDIO_CONTRACT_PACKAGE_PATH .. package.path
            require('backend_factory').create()
            local tokenizer = require('tokenizer')
            local scheduler = require('scheduler')
            require('kag')
            function audio_contract_scene(source)
                local ctx = {f={},sf={},tf={},lf={},mp={},call_stack={},
                    current_scene='audio-contract.ks',token_index=1,stop_flag=false}
                local tokens = tokenizer.parse(source)
                ctx.tokens = tokens
                local failure
                ctx.handle_error = function(_, message)
                    failure = message
                    ctx.stop_flag, ctx._command_error = true, true
                end
                local co = coroutine.create(function() scheduler.run(ctx,tokens,1) end)
                for _=1,16 do
                    if coroutine.status(co)=='dead' then break end
                    local ok, message = coroutine.resume(co,16)
                    assert(ok,message)
                end
                assert(coroutine.status(co)=='dead','audio command unexpectedly blocked')
                assert(not ctx._command_error,failure or 'audio command failed')
            end
        )lua");
    }
};
}

TEST_CASE("Audio native contract: canonical BGM and explicit bus setters remain positive controls") {
    AudioNativeFixture f;
    REQUIRE(f.lua != nullptr);
    REQUIRE_MESSAGE(f.boot(), f.error);
    REQUIRE_MESSAGE(f.run("audio_contract_scene('[playbgm storage=\"clip.wav\" fadein=250]')"), f.error);
    CHECK(f.audio.bgmCalls == 1);
    CHECK(f.audio.file == "clip.wav");
    CHECK(f.audio.bgmFade == doctest::Approx(0.25));
    REQUIRE_MESSAGE(f.run("audio_contract_scene('[setvoicevolume volume=0.3][setsevolume volume=0.4]')"), f.error);
    CHECK(f.audio.voiceBus == doctest::Approx(0.3));
    CHECK(f.audio.seBus == doctest::Approx(0.4));
}

TEST_CASE("Audio native contract: play and bgm aliases supply canonical fade defaults") {
    AudioNativeFixture f;
    REQUIRE(f.lua != nullptr);
    REQUIRE_MESSAGE(f.boot(), f.error);
    SUBCASE("play default bus") {
        REQUIRE_MESSAGE(f.run("audio_contract_scene('[play file=\"clip.wav\"]')"), f.error);
    }
    SUBCASE("bgm alias") {
        REQUIRE_MESSAGE(f.run("audio_contract_scene('[bgm storage=\"clip.wav\"]')"), f.error);
    }
    CHECK(f.audio.bgmCalls == 1);
    CHECK(f.audio.bgmFade == doctest::Approx(0));
}

TEST_CASE("Audio native contract: stopbgm time alias survives schema defaults") {
    AudioNativeFixture f;
    REQUIRE(f.lua != nullptr);
    REQUIRE_MESSAGE(f.boot(), f.error);
    SUBCASE("time alias") {
        REQUIRE_MESSAGE(f.run("audio_contract_scene('[stopbgm time=1250]')"), f.error);
    }
    SUBCASE("canonical fadeout") {
        REQUIRE_MESSAGE(f.run("audio_contract_scene('[stopbgm fadeout=1250]')"), f.error);
    }
    CHECK(f.audio.stopBgmCalls == 1);
    CHECK(f.audio.stopBgmFade == doctest::Approx(1.25));
}

TEST_CASE("Audio native contract: playse volume reaches the audio boundary") {
    AudioNativeFixture f;
    REQUIRE(f.lua != nullptr);
    REQUIRE_MESSAGE(f.boot(), f.error);
    REQUIRE_MESSAGE(f.run("audio_contract_scene('[playse storage=\"clip.wav\" volume=0.25]')"), f.error);
    CHECK(f.audio.seCalls == 1);
    CHECK(f.audio.seVolume == doctest::Approx(0.25));
    CHECK(f.audio.seBus == 1); // Clip gain must not mutate the persistent bus.
}

TEST_CASE("Audio native contract: playvoice volume reaches the audio boundary") {
    AudioNativeFixture f;
    REQUIRE(f.lua != nullptr);
    REQUIRE_MESSAGE(f.boot(), f.error);
    REQUIRE_MESSAGE(f.run("audio_contract_scene('[playvoice storage=\"clip.wav\" volume=0.35]')"), f.error);
    CHECK(f.audio.voiceCalls == 1);
    CHECK(f.audio.voiceVolume == doctest::Approx(0.35));
    CHECK(f.audio.voiceBus == 1);
}

TEST_CASE("Audio native contract: backend options preserve BGM gain and creation refusal") {
    AudioNativeFixture f;
    REQUIRE(f.lua != nullptr);
    REQUIRE_MESSAGE(f.boot(), f.error);
    SUBCASE("requested gain") {
        REQUIRE_MESSAGE(f.run("assert(require('backend').audio_play('bgm','clip.wav',{fadein=0,volume=0.45}))"), f.error);
        CHECK(f.audio.bgmCalls == 1);
        CHECK(f.audio.bgmFade == 0);
        CHECK(f.audio.bgmVolume == doctest::Approx(0.45));
        CHECK(f.audio.bgmBus == 1);
    }
    SUBCASE("backend refusal stays false") {
        f.audio.admitted = false;
        REQUIRE_MESSAGE(f.run("assert(require('backend').audio_play('bgm','clip.wav',{fadein=0}) == false)"), f.error);
        CHECK(f.audio.bgmCalls == 1);
    }
}

TEST_CASE("Audio native contract: SE fades and loop options reach typed C++ inputs") {
    AudioNativeFixture f;
    REQUIRE(f.lua != nullptr);
    REQUIRE_MESSAGE(f.boot(), f.error);
    REQUIRE_MESSAGE(f.run("audio_contract_scene('[playse storage=\"clip.wav\" volume=0.4 fadein=750][stopse time=1250]')"), f.error);
    CHECK(f.audio.seCalls == 1);
    CHECK(f.audio.seOptions.fadeIn == doctest::Approx(0.75));
    CHECK(f.audio.stopSeCalls == 1);
    CHECK(f.audio.stopSeFade == doctest::Approx(1.25));
    CHECK(f.audio.seBus == 1);
    REQUIRE_MESSAGE(f.run("assert(require('backend').audio_play('bgm','clip.wav',{volume=0.2,loop=true,fadein=0.5}))"), f.error);
    CHECK(f.audio.bgmOptions.loop);
    CHECK(f.audio.bgmOptions.fadeIn == doctest::Approx(0.5));
    CHECK(f.audio.bgmOptions.volume == doctest::Approx(0.2));
    CHECK(f.audio.bgmBus == 1);
    REQUIRE_MESSAGE(f.run("assert(require('backend').audio_play('se','clip.wav',{x=1,y=2,z=3,volume=0.3,loop=false,fadein=0.1}))"), f.error);
    CHECK_FALSE(f.audio.seOptions.loop);
    CHECK(f.audio.spatial[0] == 1);
    CHECK(f.audio.spatial[1] == 2);
    CHECK(f.audio.spatial[2] == 3);
    CHECK(f.audio.seOptions.volume == doctest::Approx(0.3));
}

TEST_CASE("Audio native contract: invalid typed fields reject before any backend call") {
    AudioNativeFixture f;
    REQUIRE(f.lua != nullptr);
    REQUIRE_MESSAGE(f.boot(), f.error);
    REQUIRE_MESSAGE(f.run(R"lua(
        local backend=require('backend')
        for _, options in ipairs({{volume='bad'},{volume=0/0},{volume=1/0},
                                   {volume=-1},{volume=2},{loop='false'},
                                   {fadein=-1},{fadein=1/0}}) do
            assert(backend.audio_play('bgm','clip.wav',options)==false)
            assert(backend.audio_play('se','clip.wav',options)==false)
            assert(backend.audio_play('voice','clip.wav',options)==false)
        end
        assert(KAG.stop_se(-1)==false)
        assert(KAG.stop_se(1/0)==false)
    )lua"), f.error);
    CHECK(f.audio.bgmCalls == 0);
    CHECK(f.audio.voiceCalls == 0);
    CHECK(f.audio.seCalls == 0);
    CHECK(f.audio.stopSeCalls == 0);
}

namespace {
class ContractAudioQuota final : public Caesura::ISandboxQuota {
public:
    ContractAudioQuota() : previous(Caesura::BackendRegistry::instance().getSandboxQuota()) {
        Caesura::BackendRegistry::instance().setSandboxQuota(this);
    }
    ~ContractAudioQuota() override { Caesura::BackendRegistry::instance().setSandboxQuota(previous); }
    void setLuaState(lua_State*) override {}
    bool tryAlloc(const char*) override { ++tryCalls; if (reject) return false; ++live; return true; }
    void release(const char*) override { ++releaseCalls; --live; }
    int count(const char*) override { return live; }
    int maxLimit(const char*) override { return 100; }
    Caesura::ISandboxQuota* previous;
    int live = 0;
    unsigned tryCalls = 0, releaseCalls = 0;
    bool reject = false;
};
}

TEST_CASE("Audio native contract: real SoLoud clip gain looping fades and retirement") {
    ContractAudioQuota quota;
    Caesura::SoLoudAudioEngine audio{Caesura::SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(audio.init());
    const int baseline = quota.live;
    audio.setBusVolume("bgm",0.7f);
    audio.setBusVolume("voice",0.6f);
    audio.setBusVolume("se",0.8f);
    Caesura::AudioPlaybackOptions options{0.25f,true,0.0f};
    const auto bgm = audio.playBGM("tests/audio/silence.wav",options);
    REQUIRE(bgm != 0);
    CHECK(audio.soloud().getVolume(bgm) == doctest::Approx(0.25));
    CHECK(audio.soloud().getLooping(bgm));
    CHECK(audio.getBusVolume("bgm") == doctest::Approx(0.7));
    const auto voice = audio.playVoice("tests/audio/silence.wav",options);
    REQUIRE(voice != 0);
    CHECK(audio.soloud().getVolume(voice) == doctest::Approx(0.25));
    CHECK(audio.soloud().getLooping(voice));
    CHECK(audio.getBusVolume("voice") == doctest::Approx(0.6));
    options.fadeIn = 0.1f;
    const auto se = audio.playSE("tests/audio/silence.wav",options);
    REQUIRE(se != 0);
    CHECK(audio.soloud().getVolume(se) == doctest::Approx(0));
    CHECK(audio.getBusVolume("se") == doctest::Approx(0.8));
    std::array<float,1024> pcm{};
    for (int i=0;i<32;++i) { audio.soloud().mix(pcm.data(),512); audio.update(0); }
    CHECK(audio.soloud().isValidVoiceHandle(bgm)); // Beyond the 100 ms fixture.
    CHECK(audio.soloud().isValidVoiceHandle(voice));
    CHECK(audio.soloud().isValidVoiceHandle(se));
    CHECK(audio.soloud().getVolume(se) == doctest::Approx(0.25));
    CHECK(quota.live == baseline+3);
    audio.stopSE(0.1f);
    CHECK(audio.soloud().isValidVoiceHandle(se));
    CHECK(quota.live == baseline+3); // No early quota/raw source retirement.
    for (int i=0;i<32;++i) { audio.soloud().mix(pcm.data(),512); audio.update(0); }
    CHECK_FALSE(audio.soloud().isValidVoiceHandle(se));
    CHECK_FALSE(audio.isSEPlaying());
    CHECK(quota.live == baseline+2);
    audio.stopBGM(0);
    audio.stopVoice();
    for (int i=0;i<32;++i) { audio.soloud().mix(pcm.data(),512); audio.update(0); }
    CHECK(quota.live == baseline);
    options.loop = false; options.fadeIn = 0;
    const auto once = audio.playSE("tests/audio/silence.wav",options);
    REQUIRE(once != 0);
    CHECK_FALSE(audio.soloud().getLooping(once));
    for (int i=0;i<32;++i) { audio.soloud().mix(pcm.data(),512); audio.update(0); }
    CHECK_FALSE(audio.soloud().isValidVoiceHandle(once));
    CHECK(quota.live == baseline);
    options.volume = std::numeric_limits<float>::quiet_NaN();
    CHECK(audio.playBGM("tests/audio/silence.wav",options) == 0);
    CHECK(audio.playVoice("tests/audio/silence.wav",options) == 0);
    CHECK(audio.playSE("tests/audio/silence.wav",options) == 0);
    CHECK(quota.live == baseline);
    options.volume = 0.5f;
    quota.reject = true;
    CHECK(audio.playSE("tests/audio/silence.wav",options) == 0);
    CHECK(quota.live == baseline);
    quota.reject = false;
    audio.shutdown();
    CHECK(quota.live == baseline);
}

TEST_CASE("Audio review negative: malformed BGM stop cannot become a valid stop") {
    AudioNativeFixture f;
    REQUIRE(f.lua != nullptr);
    REQUIRE_MESSAGE(f.boot(), f.error);
    CHECK_MESSAGE(f.run(R"lua(
        local backend=require('backend')
        for _, value in ipairs({'bad',false,{}}) do
            local ok,result=pcall(backend.audio_stop,'bgm',{fadeout=value})
            assert(ok and result==false,'malformed nonnil stop must return false')
        end
    )lua"), f.error);
    CHECK(f.audio.stopBgmCalls == 0);
}

namespace {
void checkRealSECapacityRefusal(bool spatial) {
    ContractAudioQuota quota;
    SoLoud::Wav filler;
    REQUIRE(filler.load("tests/audio/silence.wav") == SoLoud::SO_NO_ERROR);
    Caesura::SoLoudAudioEngine audio{Caesura::SoLoudAudioEngine::OutputMode::ManualMix};
    REQUIRE(audio.init());
    REQUIRE(audio.soloud().getBackendId() == SoLoud::Soloud::NULLDRIVER);
    const auto original=audio.playSE("tests/audio/silence.wav",
        Caesura::AudioPlaybackOptions{0.25f,true,0});
    REQUIRE(original != 0);
    audio.soloud().setProtectVoice(original,true);
    std::vector<unsigned> extras;
    for (unsigned i=audio.soloud().getVoiceCount(); i<VOICE_COUNT; ++i) {
        const auto handle=audio.soloud().play(filler,1,0,true);
        REQUIRE(audio.soloud().isValidVoiceHandle(handle));
        audio.soloud().setProtectVoice(handle,true);
        REQUIRE(audio.soloud().getProtectVoice(handle));
        extras.push_back(handle);
    }
    REQUIRE(audio.soloud().getVoiceCount() == VOICE_COUNT);
    const auto before=audio.getSnapshot();
    REQUIRE(before.busVoices == 3);
    REQUIRE(extras.size()+1+before.busVoices == VOICE_COUNT);
    REQUIRE(audio.soloud().getProtectVoice(original));
    const auto allocations=quota.tryCalls, releases=quota.releaseCalls;
    const auto live=quota.live;
    std::printf("AUDIO_CAPACITY_READY kind=%s voices=%u protected_external=%zu buses=%llu original=%u quota=%d\n",
        spatial?"se3d":"se",audio.soloud().getVoiceCount(),extras.size()+1,
        static_cast<unsigned long long>(before.busVoices),original,live);
    std::fflush(stdout);
    const Caesura::AudioPlaybackOptions options{0.5f,false,0.1f};
    const auto refused=spatial
        ? audio.playSE3D("tests/audio/silence.wav",1,2,3,options)
        : audio.playSE("tests/audio/silence.wav",options);
    CHECK(refused == 0);
    CHECK(quota.tryCalls == allocations);
    CHECK(quota.releaseCalls == releases);
    CHECK(quota.live == live);
    CHECK(audio.soloud().getVoiceCount() == VOICE_COUNT);
    CHECK(audio.soloud().isValidVoiceHandle(original));
    CHECK(audio.soloud().getVolume(original) == doctest::Approx(0.25));
    const auto after=audio.getSnapshot();
    CHECK(after.busVoices == before.busVoices);
    CHECK(after.liveVoices == before.liveVoices);
    CHECK(after.sessionHandles == before.sessionHandles);
    for (const auto h:extras) CHECK(audio.soloud().isValidVoiceHandle(h));
    audio.shutdown();
    CHECK(quota.live == 0);
}
}

// Separate processes/filters for RED: the unfixed vendor path can abort before
// another subcase runs. Do not conceal that outcome with an in-process mock.
TEST_CASE("Audio review negative: SE rejects a real all protected mixer") {
    checkRealSECapacityRefusal(false);
}
TEST_CASE("Audio review negative: SE3D rejects a real all protected mixer") {
    checkRealSECapacityRefusal(true);
}
