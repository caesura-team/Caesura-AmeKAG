// test_backend_registry.cpp - BackendRegistry / DI module tests
#include "doctest.h"
#include "di/BackendRegistry.h"
#include "script/bindings/EngineBinding.h"
#include "render/BgfxRenderDevice.h"
#include "render/NullRenderDevice.h"
#include "render/ParticleSystem.h"
#include "platform/NullPlatformBackend.h"
#include "resource/ResourceHandle.h"
#include "audio/NullAudioBackend.h"
#include "audio/SoLoudAudioEngine.h"
#include "live2d/NullAnimationBackend.h"
#include "steam/NullSteamBackend.h"
#include "EntryLifecycleBackends.h"
#include <memory>
#include <stdexcept>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

using namespace Caesura;

TEST_CASE("BackendRegistry::singleton access") {
    auto& reg = BackendRegistry::instance();
    (void)reg;
}

TEST_CASE("BackendRegistry::setRenderDevice/getRenderDevice round-trip") {
    auto& reg = BackendRegistry::instance();
    BgfxRenderDevice rd;
    IRenderDevice* oldRd = reg.getRenderDevice();
    reg.setRenderDevice(&rd);
    CHECK(reg.getRenderDevice() == &rd);
    reg.setRenderDevice(oldRd);  // Restore
}

TEST_CASE("BackendRegistry::ResourceHandle invalid handle") {
    auto& reg = BackendRegistry::instance();
    GenerationTracker tracker;
    auto* old = reg.getResourceGenerationTracker();
    reg.setResourceGenerationTracker(&tracker);
    ResourceHandle invalid;
    invalid.id = 0;
    invalid.generation = 0;
    const bool invalidIsCurrent =
        invalid.id != 0 && reg.getResourceGenerationTracker()->isCurrent(invalid);
    CHECK_FALSE(invalidIsCurrent);
    reg.setResourceGenerationTracker(old);
}

TEST_CASE("BackendRegistry::invalidateHandles verifies effect") {
    auto& reg = BackendRegistry::instance();
    GenerationTracker tracker;
    auto* old = reg.getResourceGenerationTracker();
    reg.setResourceGenerationTracker(&tracker);
    // Create a valid handle, invalidate its type, verify it becomes invalid
    ResourceHandle h = tracker.makeHandle(HandleType::TEXTURE, 42);
    const bool handleIsCurrent = h.id != 0 && tracker.isCurrent(h);
    CHECK(handleIsCurrent);
    tracker.invalidate(HandleType::TEXTURE);
    CHECK_FALSE(tracker.isCurrent(h));
    reg.setResourceGenerationTracker(old);
}

TEST_CASE("BackendRegistry::setJobSystem/getJobSystem") {
    auto& reg = BackendRegistry::instance();
    IJobSystem* old = reg.getJobSystem();
    IJobSystem* sentinel = reinterpret_cast<IJobSystem*>(0x1);
    reg.setJobSystem(sentinel);
    CHECK(reg.getJobSystem() == sentinel);
    reg.setJobSystem(old);  // Restore
}

TEST_CASE("BackendRegistry::setCryptoEngine/getCryptoEngine") {
    auto& reg = BackendRegistry::instance();
    carc::ICryptoEngine* old = reg.getCryptoEngine();
    carc::ICryptoEngine* sentinel = reinterpret_cast<carc::ICryptoEngine*>(0x1);
    reg.setCryptoEngine(sentinel);
    CHECK(reg.getCryptoEngine() == sentinel);
    reg.setCryptoEngine(old);  // Restore
}

TEST_CASE("BackendRegistry::setLuaManager/getLuaManager") {
    auto& reg = BackendRegistry::instance();
    ILuaManager* old = reg.getLuaManager();
    ILuaManager* sentinel = reinterpret_cast<ILuaManager*>(0x1);
    reg.setLuaManager(sentinel);
    CHECK(reg.getLuaManager() == sentinel);
    reg.setLuaManager(old);  // Restore
}

// =============================================================================
// Expanded coverage
// =============================================================================

TEST_CASE("BackendRegistry accepts externally owned null backends") {
    NullRenderDevice render;
    NullPlatformBackend platform;
    auto& reg = BackendRegistry::instance();
    auto* oldRender = reg.getRenderDevice();
    auto* oldPlatform = reg.getPlatformBackend();
    reg.setRenderDevice(&render);
    reg.setPlatformBackend(&platform);
    CHECK(reg.getRenderDevice() == &render);
    CHECK(reg.getPlatformBackend() == &platform);
    reg.setRenderDevice(oldRender);
    reg.setPlatformBackend(oldPlatform);
}

TEST_CASE("BackendRegistry::createBackend factory rejects unknown names") {
    auto& reg = BackendRegistry::instance();
    CHECK(reg.createRenderDevice("vulkan") == nullptr);
    CHECK(reg.createAudioBackend("fmod") == nullptr);
    CHECK(reg.createPlatformBackend("glfw") == nullptr);
    CHECK(reg.createRenderDevice("nonexistent") == nullptr);
}

TEST_CASE("EngineBinding::registerEngineBindings works (P1-5 move)") {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    REQUIRE(L != nullptr);
    Caesura::engine_binding::registerEngineBindings(L);
    lua_getglobal(L, "Engine");
    CHECK(lua_istable(L, -1));
    lua_pop(L, 1);
    lua_close(L);
}

TEST_CASE("Engine capability query reads current providers and returns isolated values") {
    auto& reg = BackendRegistry::instance();
    struct RestoreRegistry {
        BackendRegistry& registry;
        IRenderDevice* render;
        IAudioBackend* audio;
        IAnimationBackend* animation;
        ISteamBackend* steam;
        IParticleSystem* particles;
        ~RestoreRegistry() {
            registry.setRenderDevice(render);
            registry.setAudioBackend(audio);
            registry.setAnimationBackend(animation);
            registry.setSteamBackend(steam);
            registry.setParticleSystem(particles);
        }
    };
    NullRenderDevice render;
    Test::LifecycleProbe renderProbe;
    renderProbe.initResult = true;
    Test::RenderDevice readyRender(renderProbe);
    ParticleSystem particles;
    NullAudioBackend silent;
    NullAnimationBackend animation;
    NullSteamBackend steam;
    SoLoudAudioEngine mixer{SoLoudAudioEngine::OutputMode::ManualMix};
    class ThrowingAudio final : public NullAudioBackend {
        bool isPlaybackAvailable() const override { throw std::runtime_error("private-provider-detail"); }
    } throwing;
    RestoreRegistry restore{reg, reg.getRenderDevice(), reg.getAudioBackend(),
                            reg.getAnimationBackend(), reg.getSteamBackend(), reg.getParticleSystem()};
    reg.setRenderDevice(&render);
    reg.setAudioBackend(&silent);
    reg.setAnimationBackend(&animation);
    reg.setSteamBackend(&steam);
    std::unique_ptr<lua_State, decltype(&lua_close)> state(luaL_newstate(), lua_close);
    REQUIRE(state != nullptr);
    lua_State* L = state.get();
    luaL_openlibs(L);
    engine_binding::registerEngineBindings(L);
    const auto run = [&](const char* code) {
        const int result = luaL_dostring(L, code);
        const std::string diagnostic = result == LUA_OK ? "capability query succeeded" : lua_tostring(L, -1);
        INFO(diagnostic);
        CHECK(result == LUA_OK);
        lua_settop(L, 0);
    };
    run("local p=Engine.get_capability_profile(); assert(p.target=='native' and p.scope=='runtime'); "
        "assert(not p.available.audio and not p.available.cubism and not p.available.steam); "
        "assert(not p.available.video and not p.available.particles and not p.available['postfx.bloom']); "
        "p.available.audio=true; p.compiled.ffmpeg='tampered'; "
        "local fresh=Engine.get_capability_profile(); assert(not fresh.available.audio); "
        "assert(type(fresh.compiled.ffmpeg)=='boolean')");
    REQUIRE(mixer.init());
    reg.setAudioBackend(&mixer);
    run("assert(Engine.get_capability_profile().available.audio)");
    mixer.shutdown();
    run("assert(not Engine.get_capability_profile().available.audio)");
    reg.setAudioBackend(&silent);
    run("assert(not Engine.get_capability_profile().available.audio)");

    reg.setRenderDevice(&readyRender);
    reg.setParticleSystem(&particles);
    run("assert(not Engine.get_capability_profile().available.particles)");
    // The boundary render reports ready but supplies no real GPU handles.
    // Failed particle initialization must not become availability by registration.
    REQUIRE_FALSE(particles.init());
    run("assert(not Engine.get_capability_profile().available.particles)");
    particles.shutdown();
    run("assert(not Engine.get_capability_profile().available.particles)");

    reg.setAudioBackend(&throwing);
    run("local p,err=Engine.get_capability_profile(); assert(p==nil and err=='capability_query_failed')");
}
