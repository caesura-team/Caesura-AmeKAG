#include "doctest.h"
#include "EntryLifecycleBackends.h"
#include "di/BackendRegistry.h"
#include "di/api/ISandboxQuota.h"
#include "render/ParticleSystem.h"
#include "script/bindings/EngineBinding.h"
#include "script/bindings/VFXBinding.h"

#include <memory>
#include <string>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

using namespace Caesura;

namespace {

class ParticleQuota final : public ISandboxQuota {
public:
    void setLuaState(lua_State*) override {}
    bool tryAlloc(const char* kind) override {
        CHECK(std::string(kind) == "particles_emitters");
        ++allocationCalls;
        ++live;
        return true;
    }
    void release(const char* kind) override {
        CHECK(std::string(kind) == "particles_emitters");
        ++releaseCalls;
        --live;
    }
    int count(const char*) override { return live; }
    int maxLimit(const char*) override { return 64; }

    int allocationCalls = 0, releaseCalls = 0, live = 0;
};

// Only readiness is supplied by the boundary. createEmitter/destroyEmitter
// remain the real ParticleSystem vector operations, including valid ID zero.
// This does not claim a GPU initialization or particle-rendering result.
class ReadyParticleSystem final : public ParticleSystem {
public:
    bool isInitialized() const override { return ready; }
    bool ready = true;
};

struct VfxCapabilityFixture {
    Test::LifecycleProbe renderProbe;
    Test::RenderDevice render{renderProbe};
    ParticleQuota quota;
    BackendRegistry& registry = BackendRegistry::instance();
    IRenderDevice* oldRender = registry.getRenderDevice();
    IParticleSystem* oldParticles = registry.getParticleSystem();
    ISandboxQuota* oldQuota = registry.getSandboxQuota();
    IAudioBackend* oldAudio = registry.getAudioBackend();
    IAnimationBackend* oldAnimation = registry.getAnimationBackend();
    ISteamBackend* oldSteam = registry.getSteamBackend();
    std::unique_ptr<lua_State, decltype(&lua_close)> state{luaL_newstate(), lua_close};

    explicit VfxCapabilityFixture(IParticleSystem& particles) {
        registry.setRenderDevice(&render);
        registry.setParticleSystem(&particles);
        registry.setSandboxQuota(&quota);
        registry.setAudioBackend(nullptr);
        registry.setAnimationBackend(nullptr);
        registry.setSteamBackend(nullptr);
        if (!state) return;
        lua_State* L = state.get();
        luaL_openlibs(L);
        lua_pushlightuserdata(L, static_cast<IRenderDevice*>(&render));
        lua_setfield(L, LUA_REGISTRYINDEX, "Caesura.RenderDevice");
        engine_binding::registerEngineBindings(L);
        registerVFXBinding(L);
    }
    ~VfxCapabilityFixture() {
        state.reset();
        registry.setRenderDevice(oldRender);
        registry.setParticleSystem(oldParticles);
        registry.setSandboxQuota(oldQuota);
        registry.setAudioBackend(oldAudio);
        registry.setAnimationBackend(oldAnimation);
        registry.setSteamBackend(oldSteam);
    }

    bool particlesAvailable() {
        lua_State* L = state.get();
        const int status = luaL_dostring(L, "return Engine.get_capability_profile().available.particles");
        REQUIRE_MESSAGE(status == LUA_OK, (status == LUA_OK ? "" : lua_tostring(L, -1)));
        REQUIRE(lua_isboolean(L, -1));
        const bool available = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return available;
    }
    lua_Integer create() {
        lua_State* L = state.get();
        lua_getglobal(L, "VFX");
        lua_getfield(L, -1, "particles_create_emitter");
        lua_remove(L, -2);
        lua_newtable(L);
        const int status = lua_pcall(L, 1, 1, 0);
        REQUIRE_MESSAGE(status == LUA_OK, (status == LUA_OK ? "" : lua_tostring(L, -1)));
        REQUIRE(lua_isinteger(L, -1));
        const auto id = lua_tointeger(L, -1);
        lua_pop(L, 1);
        return id;
    }
    bool destroy(lua_Integer id) {
        lua_State* L = state.get();
        lua_getglobal(L, "VFX");
        lua_getfield(L, -1, "particles_destroy_emitter");
        lua_remove(L, -2);
        lua_pushinteger(L, id);
        const int status = lua_pcall(L, 1, 1, 0);
        REQUIRE_MESSAGE(status == LUA_OK, (status == LUA_OK ? "" : lua_tostring(L, -1)));
        REQUIRE(lua_isboolean(L, -1));
        const bool destroyed = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return destroyed;
    }
};

} // namespace

TEST_CASE("U19 VFX capabilities: uninitialized real particles cannot allocate through the public binding") {
    ParticleSystem particles;
    VfxCapabilityFixture fixture(particles);
    REQUIRE(fixture.state != nullptr);
    SUBCASE("never initialized") {}
    SUBCASE("initialization failed at the render boundary") {
        REQUIRE_FALSE(particles.init());
    }
    SUBCASE("shutdown cleared an earlier emitter") {
        REQUIRE(particles.createEmitter(ParticleEmitterConfig{}) == 0);
        particles.shutdown();
    }
    REQUIRE_FALSE(particles.isInitialized());
    CHECK_FALSE(fixture.particlesAvailable());
    CHECK(fixture.create() == -1);
    CHECK(particles.activeEmitterCount() == 0);
    CHECK(fixture.quota.allocationCalls == 0);
    CHECK(fixture.quota.live == 0);
    CHECK(lua_gettop(fixture.state.get()) == 0);
}

TEST_CASE("U19 VFX capabilities: unavailable rendering rejects otherwise ready particle storage") {
    ReadyParticleSystem particles;
    VfxCapabilityFixture fixture(particles);
    REQUIRE(fixture.state != nullptr);
    fixture.renderProbe.initResult = false;
    REQUIRE(particles.isInitialized());
    CHECK_FALSE(fixture.particlesAvailable());
    CHECK(fixture.create() == -1);
    CHECK(particles.activeEmitterCount() == 0);
    CHECK(fixture.quota.allocationCalls == 0);
    CHECK(fixture.quota.live == 0);
}

TEST_CASE("U19 VFX capabilities: ready creation preserves ID zero and releases its real owner") {
    ReadyParticleSystem particles;
    VfxCapabilityFixture fixture(particles);
    REQUIRE(fixture.state != nullptr);
    CHECK(fixture.particlesAvailable());
    const auto id = fixture.create();
    CHECK(id == 0);
    CHECK(particles.activeEmitterCount() == 1);
    CHECK(fixture.quota.allocationCalls == 1);
    CHECK(fixture.quota.live == 1);
    CHECK(fixture.destroy(id));
    CHECK(particles.activeEmitterCount() == 0);
    CHECK(fixture.quota.releaseCalls == 1);
    CHECK(fixture.quota.live == 0);

    particles.ready = false;
    CHECK_FALSE(fixture.particlesAvailable());
    CHECK(fixture.create() == -1);
    CHECK(particles.activeEmitterCount() == 0);
    CHECK(fixture.quota.allocationCalls == 1);
    CHECK(fixture.quota.live == 0);
    CHECK(lua_gettop(fixture.state.get()) == 0);
}
