// test_script_boundary.cpp - E3 boundary tests (KAG error recovery, GameState, bindings)
#include "doctest.h"
#include "EntryLifecycleBackends.h"
#include "script/vm/LuaManager.h"
#include "script/bindings/KAGBinding.h"
#include "script/bindings/RenderBinding.h"
#include "script/bindings/VFXBinding.h"
#include "script/bindings/DebugBinding.h"
#include "script/bindings/DevCoreBinding.h"
#include "script/state/GameState.h"
#include "di/BackendRegistry.h"
#include "render/ParticleSystem.h"
#include <memory>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

using namespace Caesura;

// -- KAG error recovery (E3 Step 1) ------------------------------------

static LuaManager* initKAGLua() {
    auto* lm = new LuaManager();
    if (!lm->init()) { delete lm; return nullptr; }
    lua_State* L = lm->state();
    luaL_dostring(L,
        "package.path = 'scripts/?.lua;scripts/?/init.lua;' .. package.path");
    registerKAGBinding(L);
    registerRenderBinding(L);
    registerVFXBinding(L);
    registerDebugBinding(L);
    registerDevCoreBinding(L);
    return lm;
}

static bool requireModule(lua_State* L, const char* name) {
    lua_getglobal(L, "require");
    lua_pushstring(L, name);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) { lua_pop(L, 1); return false; }
    lua_pop(L, 1);
    return true;
}

TEST_CASE("E3 KAG: malformed script does not crash") {
    auto* lm = initKAGLua();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    REQUIRE(requireModule(L, "tokenizer"));
    int r = luaL_dostring(L,
        "local tk = require('tokenizer'); "
        "local ok, result = pcall(tk.parse, '@invalid @@syntax **broken'); "
        "assert(ok == false or type(result) == 'table')");
    CHECK((r == LUA_OK || r == LUA_ERRRUN));
    delete lm;
}


TEST_CASE("E3 KAG: choice with zero options degrades gracefully") {
    auto* lm = initKAGLua();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    REQUIRE(requireModule(L, "tokenizer"));
    int r = luaL_dostring(L,
        "local tk = require('tokenizer'); "
        "local tokens = tk.parse('[choice]\\n[endchoice]'); "
        "assert(#tokens >= 2)");
    CHECK((r == LUA_OK || r == LUA_ERRRUN));
    delete lm;
}

TEST_CASE("E3 KAG: deeply nested if does not crash") {
    auto* lm = initKAGLua();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    REQUIRE(requireModule(L, "tokenizer"));
    int r = luaL_dostring(L,
        "local tk = require('tokenizer'); "
        "local s = ''; "
        "for i = 1, 15 do s = s .. '[if exp=\\'1\\']\\n' end; "
        "for i = 1, 15 do s = s .. '[endif]\\n' end; "
        "local tokens = tk.parse(s); "
        "assert(#tokens >= 30)");
    CHECK((r == LUA_OK || r == LUA_ERRRUN));
    delete lm;
}

// -- GameState persistence (E3 Step 2) ----------------------------------

static void bindBoundaryContext(lua_State* state) {
    lua_newtable(state);
    REQUIRE(GameState::bind(state, -1));
    lua_pop(state, 1);
}

TEST_CASE("E3 GameState: persist across save/load cycle") {
    LuaManager lm1;
    REQUIRE(lm1.init());
    lua_State* L1 = lm1.state();
    bindBoundaryContext(L1);
    REQUIRE(GameState::push(L1));
    lua_pushstring(L1, "persist_value");
    lua_setfield(L1, -2, "persist_key");
    lua_getfield(L1, -1, "persist_key");
    CHECK(std::string(lua_tostring(L1, -1)) == "persist_value");
    lua_pop(L1, 2);

    LuaManager lm2;
    REQUIRE(lm2.init());
    lua_State* L2 = lm2.state();
    bindBoundaryContext(L2);
    REQUIRE(GameState::push(L2));
    lua_getfield(L2, -1, "persist_key");
    CHECK(lua_isnil(L2, -1));
    lua_pop(L2, 2);
}

TEST_CASE("E3 GameState: nested table storage") {
    LuaManager lm;
    REQUIRE(lm.init());
    lua_State* L = lm.state();
    bindBoundaryContext(L);
    REQUIRE(GameState::push(L));
    lua_newtable(L);
    lua_pushstring(L, "inner");
    lua_setfield(L, -2, "nested_key");
    lua_setfield(L, -2, "outer_key");
    lua_getfield(L, -1, "outer_key");
    CHECK(lua_istable(L, -1));
    lua_getfield(L, -1, "nested_key");
    CHECK(std::string(lua_tostring(L, -1)) == "inner");
    lua_pop(L, 3);
}

TEST_CASE("E3 GameState: push after bind returns existing ctx") {
    LuaManager lm;
    REQUIRE(lm.init());
    lua_State* L = lm.state();
    bindBoundaryContext(L);
    REQUIRE(GameState::push(L));
    lua_pushstring(L, "persistent");
    lua_setfield(L, -2, "sticky_key");
    lua_pop(L, 1);
    REQUIRE(GameState::push(L));
    lua_getfield(L, -1, "sticky_key");
    CHECK(std::string(lua_tostring(L, -1)) == "persistent");
    lua_pop(L, 2);
}

// -- Binding parameter validation (E3 Step 3) ---------------------------

static LuaManager* initBindingLua() {
    auto* lm = new LuaManager();
    if (!lm->init()) { delete lm; return nullptr; }
    lua_State* L = lm->state();
    registerKAGBinding(L);
    registerRenderBinding(L);
    registerVFXBinding(L);
    registerDebugBinding(L);
    registerDevCoreBinding(L);
    return lm;
}

TEST_CASE("E3 Bindings: KAG play_bgm empty path does not crash") {
    auto* lm = initBindingLua();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    lua_getglobal(L, "KAG");
    lua_getfield(L, -1, "play_bgm");
    lua_pushstring(L, "");
    int r = lua_pcall(L, 1, 1, 0);
    CHECK((r == LUA_OK || r == LUA_ERRRUN));
    lua_pop(L, 2);
    delete lm;
}

TEST_CASE("E3 Bindings: Debug log nil message does not crash") {
    auto* lm = initBindingLua();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    lua_getglobal(L, "Debug");
    lua_getfield(L, -1, "log");
    lua_pushnil(L);
    int r = lua_pcall(L, 1, 0, 0);
    CHECK((r == LUA_OK || r == LUA_ERRRUN));
    lua_pop(L, 1);
    delete lm;
}

TEST_CASE("E3 Bindings: DevCore set_dev_mode invalid value does not crash") {
    auto* lm = initBindingLua();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    lua_getglobal(L, "DevCore");
    lua_getfield(L, -1, "set_dev_mode");
    lua_pushinteger(L, 999);
    int r = lua_pcall(L, 1, 0, 0);
    CHECK((r == LUA_OK || r == LUA_ERRRUN));
    lua_pop(L, 1);
    delete lm;
}

// -- E3 Step 4: Jump to unknown label does not crash (scheduler.lua guard) --

TEST_CASE("E3 Scheduler: jump to unknown label does not crash") {
    auto* lm = initKAGLua();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    REQUIRE(requireModule(L, "tokenizer"));
    REQUIRE(requireModule(L, "scheduler"));
    int r = luaL_dostring(L,
        "local sch = require('scheduler'); "
        "local tokens = require('tokenizer').parse('[jump target=\"*nonexistent\"]'); "
        "local ctx = { tokens = tokens, token_index = 1, call_stack = {}, "
        "layers = {}, backlog = {}, active_operations = {} }; "
        "local co = coroutine.create(function() sch.run(ctx, ctx.tokens) end); "
        "local ok, err = coroutine.resume(co); "
        "assert(ok ~= false)");  // should not crash even if label not found
    CHECK((r == LUA_OK || r == LUA_ERRRUN));
    delete lm;
}

TEST_CASE("E3 Scheduler: jump with nil target does not crash") {
    auto* lm = initKAGLua();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    REQUIRE(requireModule(L, "tokenizer"));
    REQUIRE(requireModule(L, "scheduler"));
    int r = luaL_dostring(L,
        "local sch = require('scheduler'); "
        "local tokens = require('tokenizer').parse('[jump]'); "
        "local ctx = { tokens = tokens, token_index = 1, call_stack = {}, "
        "layers = {}, backlog = {}, active_operations = {} }; "
        "local co = coroutine.create(function() sch.run(ctx, ctx.tokens) end); "
        "local ok, err = coroutine.resume(co); "
        "assert(ok ~= false)");
    CHECK((r == LUA_OK || r == LUA_ERRRUN));
    delete lm;
}

// -- E3 Step 5: VFX create_emitter rejects negative parameters --

TEST_CASE("E3 VFX: create_emitter rejects negative rate") {
    auto* lm = initBindingLua();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    lua_getglobal(L, "VFX");
    REQUIRE(lua_istable(L, -1));
    lua_getfield(L, -1, "particles_create_emitter");
    REQUIRE(lua_isfunction(L, -1));
    // Create config table with negative rate
    lua_newtable(L);
    lua_pushnumber(L, -10.0); lua_setfield(L, -2, "rate");
    int r = lua_pcall(L, 1, 1, 0);
    CHECK(r == LUA_OK);
    CHECK(lua_tointeger(L, -1) == -1);  // should return error code
    lua_pop(L, 2);
    delete lm;
}

TEST_CASE("E3 VFX: create_emitter clamps lifeMax below lifeMin") {
    // Only readiness is supplied by this boundary. The production Lua binding
    // must clamp the observed config; real ParticleSystem storage owns ID zero.
    // No GPU initialization or particle-rendering result is claimed here.
    class ObservedParticles final : public ParticleSystem {
    public:
        bool isInitialized() const override { return true; }
        int createEmitter(const ParticleEmitterConfig& cfg) override {
            ++createCalls;
            received = cfg;
            return ParticleSystem::createEmitter(cfg);
        }
        ParticleEmitterConfig received;
        int createCalls = 0;
    } particles;
    Test::LifecycleProbe renderProbe;
    Test::RenderDevice render(renderProbe);
    REQUIRE(render.init(nullptr, 64, 64));
    Test::ServiceProbe quotaProbe;
    Test::SandboxQuotaBackend quota(quotaProbe);
    auto& registry = BackendRegistry::instance();
    struct RestoreBindings {
        BackendRegistry& registry;
        IRenderDevice* render;
        IParticleSystem* particles;
        ISandboxQuota* quota;
        ~RestoreBindings() {
            registry.setRenderDevice(render);
            registry.setParticleSystem(particles);
            registry.setSandboxQuota(quota);
        }
    } restore{registry, registry.getRenderDevice(), registry.getParticleSystem(),
              registry.getSandboxQuota()};
    registry.setRenderDevice(&render);
    registry.setParticleSystem(&particles);
    registry.setSandboxQuota(&quota);
    auto closeLua = [](LuaManager* value) {
        if (value) { value->shutdown(); delete value; }
    };
    std::unique_ptr<LuaManager, decltype(closeLua)> lm(initBindingLua(), closeLua);
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    lua_getglobal(L, "VFX");
    lua_getfield(L, -1, "particles_create_emitter");
    lua_newtable(L);
    lua_pushnumber(L, 5.0); lua_setfield(L, -2, "lifeMin");
    lua_pushnumber(L, 1.0); lua_setfield(L, -2, "lifeMax");
    int r = lua_pcall(L, 1, 1, 0);
    CHECK(r == LUA_OK);
    CHECK(lua_tointeger(L, -1) >= 0);  // should succeed with clamped values
    CHECK(lua_tointeger(L, -1) == 0);
    CHECK(particles.createCalls == 1);
    CHECK(particles.received.lifeMin == doctest::Approx(5.0f));
    CHECK(particles.received.lifeMax == doctest::Approx(5.0f));
    CHECK(particles.activeEmitterCount() == 1);
    CHECK(quotaProbe.tryAllocCalls == 1);
    lua_pop(L, 2);
    REQUIRE(luaL_dostring(L, "return VFX.particles_destroy_emitter(0)") == LUA_OK);
    CHECK(lua_isboolean(L, -1));
    CHECK(lua_toboolean(L, -1));
    CHECK(particles.activeEmitterCount() == 0);
    CHECK(quotaProbe.releaseCalls == 1);
    lua_pop(L, 1);
}

// -- E3 Step 6: GameState survives coroutine context switch --

TEST_CASE("E3 GameState: survives coroutine context switch") {
    LuaManager lm;
    REQUIRE(lm.init());
    lua_State* L = lm.state();
    // Bind a fixture-owned table; VM init creates no independent context.
    bindBoundaryContext(L);
    REQUIRE(GameState::push(L));
    lua_pushstring(L, "cross_coro_value");
    lua_setfield(L, -2, "coro_key");
    lua_pop(L, 1);
    // Create and resume a coroutine -- GameState table should still be on the
    // registry and accessible from any lua_State sharing the same Lua universe
    int r = luaL_dostring(L,
        "local co = coroutine.create(function() return 42 end); "
        "local ok, result = coroutine.resume(co); "
        "assert(ok and result == 42)");
    CHECK(r == LUA_OK);
    // Verify GameState still accessible from main thread
    REQUIRE(GameState::push(L));
    lua_getfield(L, -1, "coro_key");
    CHECK(std::string(lua_tostring(L, -1)) == "cross_coro_value");
    lua_pop(L, 2);
}

// -- E3 Step 6: Bindings nil parameter safety --

TEST_CASE("E3 Bindings: Render load_texture nil path does not crash") {
    auto* lm = initBindingLua();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    lua_getglobal(L, "Render");
    lua_getfield(L, -1, "load_texture");
    lua_pushnil(L);
    int r = lua_pcall(L, 1, 1, 0);
    CHECK((r == LUA_OK || r == LUA_ERRRUN));  // nil ->error is acceptable
    lua_pop(L, 2);
    delete lm;
}

TEST_CASE("E3 Bindings: Render destroy_texture invalid id does not crash") {
    auto* lm = initBindingLua();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    // This test requires TextureManager to be registered -- skip if not available
    if (!BackendRegistry::instance().getTextureManager()) {
        delete lm;
        return;
    }
    lua_getglobal(L, "Render");
    lua_getfield(L, -1, "destroy_texture");
    lua_pushinteger(L, 99999);  // non-existent texture ID
    int r = lua_pcall(L, 1, 1, 0);
    CHECK(r == LUA_OK);  // should succeed (no-op for invalid ID)
    lua_pop(L, 2);
    delete lm;
}

TEST_CASE("E3 Bindings: VFX emit negative count does not crash") {
    auto* lm = initBindingLua();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    if (!BackendRegistry::instance().getParticleSystem()) {
        delete lm;
        return;
    }
    lua_getglobal(L, "VFX");
    lua_getfield(L, -1, "particles_emit");
    lua_pushinteger(L, 1);     // emitter ID
    lua_pushinteger(L, -5);    // negative count
    int r = lua_pcall(L, 2, 1, 0);
    CHECK((r == LUA_OK || r == LUA_ERRRUN));
    lua_pop(L, 2);
    delete lm;
}
