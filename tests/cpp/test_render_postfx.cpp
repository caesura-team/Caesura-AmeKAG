// =============================================================================
// test_render_postfx.cpp - GPU-free contract tests for the round-102
// post-processing chain API (IRenderDevice::PostFx*).
//
// The postfx chain lives behind IRenderDevice (full-screen passes applied to
// the scene before backbuffer composite). This file pins the CONTRACT:
//   (a) PostFxKind enum values / PostFxParams defaults / PostFxHandle type;
//   (b) NullRenderDevice graceful-degradation semantics (unsupported ->
//       isPostFxSupported=false, createPostFx returns 0, everything else
//       is a safe no-op);
//   (c) BgfxRenderDevice gate: isPostFxSupported requires a GPU-initialized
//       device, so on a freshly default-constructed device createPostFx
//       returns 0 (graceful). The *real* handle lifecycle (stable handles =
//       index+1, destroy/clear reordering) is only reachable after
//       init(nativeWindow,...) succeeds, which needs a GPU - it is therefore
//       NOT covered here and must be validated on a real device unless a
//       headless bgfx software/passthrough renderer is introduced.
//
// See docs/solutions/deferred-gpu-tests.md for the engine-wide convention on
// GPU-dependent test coverage.
// =============================================================================

#include "doctest.h"

#include "render/api/IRenderDevice.h"
#include "render/BgfxRenderDevice.h"
#include "render/NullRenderDevice.h"
#include "script/bindings/RenderBinding.h"
#include "EntryLifecycleBackends.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <cstdint>
#include <map>
#include <string>
#include <type_traits>

using namespace Caesura;

// -----------------------------------------------------------------------------
// (a) Interface existence / signature (compile-time contract pins).
// -----------------------------------------------------------------------------

TEST_CASE("postfx: PostFxKind enum values are fixed") {
    CHECK(static_cast<int>(IRenderDevice::PostFxKind::Vignette) == 0);
    CHECK(static_cast<int>(IRenderDevice::PostFxKind::LutColorGrade) == 1);
    CHECK(static_cast<int>(IRenderDevice::PostFxKind::SoftBlur) == 2);
    CHECK(static_cast<int>(IRenderDevice::PostFxKind::Bloom) == 3);
    CHECK(static_cast<int>(IRenderDevice::PostFxKind::Lut3D) == 4); // t214
}

TEST_CASE("postfx: PostFxHandle is a uint32_t") {
    static_assert(std::is_same<IRenderDevice::PostFxHandle, uint32_t>::value,
                  "PostFxHandle must be uint32_t");
    // 0 is the documented "invalid/unsupported" sentinel.
    CHECK(IRenderDevice::PostFxHandle(0) == 0);
}

TEST_CASE("postfx: PostFxParams defaults match the contract") {
    IRenderDevice::PostFxParams p;
    CHECK(p.strength == doctest::Approx(1.0f));
    CHECK(p.radius == doctest::Approx(0.0f));
    CHECK(p.amount == doctest::Approx(0.0f));
    CHECK(p.r == doctest::Approx(1.0f));
    CHECK(p.g == doctest::Approx(1.0f));
    CHECK(p.b == doctest::Approx(1.0f));
    CHECK(p.lutMix == doctest::Approx(0.0f));
    // t214: Lut3D fields default to "no texture" (borrowed semantics: the
    // TextureManager stays the owner; the stage only references it).
    CHECK_FALSE(p.lutTexture.isValid());
    CHECK(p.lutSize == 0);
}

TEST_CASE("postfx: every PostFx method surface exists (compile-time)") {
    // Pin each virtual signature through the abstract interface so a
    // broken signature (arg order / const-ness / return type) fails to build.
    IRenderDevice::PostFxParams p;
    (void)static_cast<bool (IRenderDevice::*)(IRenderDevice::PostFxKind) const>(&IRenderDevice::isPostFxSupported);
    (void)static_cast<IRenderDevice::PostFxHandle (IRenderDevice::*)(IRenderDevice::PostFxKind, const IRenderDevice::PostFxParams&)>(&IRenderDevice::createPostFx);
    (void)static_cast<void (IRenderDevice::*)(IRenderDevice::PostFxHandle, const IRenderDevice::PostFxParams&)>(&IRenderDevice::setPostFxParams);
    (void)static_cast<void (IRenderDevice::*)(IRenderDevice::PostFxHandle)>(&IRenderDevice::destroyPostFx);
    (void)static_cast<void (IRenderDevice::*)()>(&IRenderDevice::clearPostFx);
    (void)static_cast<bool (IRenderDevice::*)() const>(&IRenderDevice::isPostFxActive);
    (void)p;
}

// -----------------------------------------------------------------------------
// (b) NullRenderDevice graceful degradation (fully headless / contract-faithful).
// -----------------------------------------------------------------------------

TEST_CASE("Null postfx: all kinds unsupported") {
    NullRenderDevice dev;
    for (int k = 0; k <= 4; ++k) { // t214: Lut3D=4 degrades like the rest
        auto kind = static_cast<IRenderDevice::PostFxKind>(k);
        CHECK_FALSE(dev.isPostFxSupported(kind));
    }
}

TEST_CASE("Null postfx: createPostFx returns invalid handle (0)") {
    NullRenderDevice dev;
    IRenderDevice::PostFxParams p;
    for (int k = 0; k <= 4; ++k) { // t214: Lut3D=4 included
        auto kind = static_cast<IRenderDevice::PostFxKind>(k);
        CHECK(dev.createPostFx(kind, p) == 0);
    }
}

TEST_CASE("Null postfx: isPostFxActive always false") {
    NullRenderDevice dev;
    CHECK_FALSE(dev.isPostFxActive());
}

TEST_CASE("Null postfx: all methods are safe no-ops (no crash)") {
    NullRenderDevice dev;
    IRenderDevice::PostFxParams p;
    // Set params on an invalid (0) handle.
    CHECK_NOTHROW(dev.setPostFxParams(0, p));
    // Set params on a non-zero handle that was never created.
    CHECK_NOTHROW(dev.setPostFxParams(42, p));
    // Destroy invalid / unknown handles.
    CHECK_NOTHROW(dev.destroyPostFx(0));
    CHECK_NOTHROW(dev.destroyPostFx(1));
    CHECK_NOTHROW(dev.destroyPostFx(0xFFFFFFFFu));
    // Clear the (empty) chain.
    CHECK_NOTHROW(dev.clearPostFx());
    CHECK_NOTHROW(dev.clearPostFx());
    // Repeatable / order-independent - a Null device must stay inert.
    CHECK_FALSE(dev.isPostFxActive());
}

// -----------------------------------------------------------------------------
// (c) BgfxRenderDevice gate (documented limitation - no GPU in CI).
// -----------------------------------------------------------------------------
// isPostFxSupported returns `m_bgfxInitialized && m_shaders != nullptr`, both
// of which are unset until init(nativeWindow,...) succeeds on a real GPU. On a
// default-constructed device createPostFx is therefore gated to 0. The actual
// handle lifecycle (stable handle = index+1, destroy/clear renumbering) is
// exercised only after a successful GPU init and is out of headless scope.

TEST_CASE("Bgfx postfx: unsupported without GPU init (graceful 0)") {
    BgfxRenderDevice dev;
    IRenderDevice::PostFxParams p;
    CHECK_FALSE(dev.isPostFxSupported(IRenderDevice::PostFxKind::Vignette));
    CHECK_FALSE(dev.isPostFxSupported(IRenderDevice::PostFxKind::Bloom));
    CHECK(dev.createPostFx(IRenderDevice::PostFxKind::Vignette, p) == 0);
    CHECK_FALSE(dev.isPostFxActive());
    // The full method surface stays safe even with an empty chain.
    CHECK_NOTHROW(dev.setPostFxParams(0, p));
    CHECK_NOTHROW(dev.clearPostFx());
}

// NOTE (deferred to GPU): handle lifecycle contract -
//   createPostFx -> handle = m_postFxStages.size() (1-based sequence)
//   setPostFxParams(handle>size|0) is a no-op
//   destroyPostFx(handle>size|0) is a no-op; valid destroys renumber
//   clearPostFx() empties the chain; isPostFxActive() reflects non-empty
// This behavior depends on a live initialized BgfxRenderDevice.

namespace {

// Reuse the existing headless interface fixture; only the device boundary is
// recorded. Name parsing and the per-kind cache remain the actual Lua binding.
class ReadyPostFxDevice final : public Test::RenderDevice {
public:
    explicit ReadyPostFxDevice(Test::LifecycleProbe& probe) : Test::RenderDevice(probe) {}

    bool isPostFxSupported(PostFxKind) const override {
        ++supportQueries;
        return true;
    }
    PostFxHandle createPostFx(PostFxKind kind, const PostFxParams&) override {
        ++creates;
        const auto handle = ++nextHandle;
        live.emplace(handle, kind);
        return handle;
    }
    void setPostFxParams(PostFxHandle handle, const PostFxParams&) override {
        ++updates;
        updatedHandle = handle;
    }
    void destroyPostFx(PostFxHandle handle) override {
        ++destroys;
        live.erase(handle);
    }
    void clearPostFx() override { live.clear(); }
    bool isPostFxActive() const override { return !live.empty(); }

    mutable unsigned supportQueries = 0;
    unsigned creates = 0, updates = 0, destroys = 0;
    PostFxHandle updatedHandle = 0;
    std::map<PostFxHandle, PostFxKind> live;

private:
    PostFxHandle nextHandle = 40;
};

struct PostFxLuaReply {
    int status = LUA_OK;
    int type = LUA_TNONE;
    lua_Integer integer = 0;
    bool boolean = false;
    std::string error;
};

struct PostFxLuaFixture {
    Test::LifecycleProbe probe;
    ReadyPostFxDevice device{probe};
    lua_State* state = luaL_newstate();

    PostFxLuaFixture() {
        if (!state) return;
        lua_pushlightuserdata(state, static_cast<IRenderDevice*>(&device));
        lua_setfield(state, LUA_REGISTRYINDEX, "Caesura.RenderDevice");
        registerRenderBinding(state);
    }
    ~PostFxLuaFixture() {
        if (!state) return;
        // Reset the binding's process-local pointer caches while this fixture
        // is still alive; no later Lua state can retain the recording device.
        registerRenderBinding(state);
        lua_close(state);
    }

    PostFxLuaReply call(const char* function, const char* kind, bool params = false) {
        lua_getglobal(state, "Render");
        lua_getfield(state, -1, function);
        lua_remove(state, -2);
        lua_pushstring(state, kind); // nullptr intentionally supplies Lua nil.
        if (params) lua_newtable(state);
        PostFxLuaReply reply;
        reply.status = lua_pcall(state, params ? 2 : 1, 1, 0);
        reply.type = lua_type(state, -1);
        if (reply.type == LUA_TNUMBER) reply.integer = lua_tointeger(state, -1);
        if (reply.type == LUA_TBOOLEAN) reply.boolean = lua_toboolean(state, -1) != 0;
        if (reply.status != LUA_OK && lua_tostring(state, -1)) reply.error = lua_tostring(state, -1);
        lua_pop(state, 1);
        return reply;
    }
};

} // namespace

TEST_CASE("U19 Render postfx: registered Bloom binding keeps its real handle lifecycle") {
    PostFxLuaFixture fixture;
    REQUIRE(fixture.state != nullptr);

    const auto supported = fixture.call("is_postfx_supported", "bloom");
    REQUIRE(supported.status == LUA_OK);
    CHECK(supported.type == LUA_TBOOLEAN);
    CHECK(supported.boolean);
    const auto created = fixture.call("set_postfx", "bloom", true);
    REQUIRE(created.status == LUA_OK);
    REQUIRE(created.type == LUA_TNUMBER);
    REQUIRE(created.integer > 0);
    const auto handle = static_cast<IRenderDevice::PostFxHandle>(created.integer);
    REQUIRE(fixture.device.live.count(handle) == 1);
    CHECK(fixture.device.live.at(handle) == IRenderDevice::PostFxKind::Bloom);
    CHECK(fixture.device.creates == 1);

    const auto updated = fixture.call("set_postfx", "bloom", true);
    CHECK(updated.status == LUA_OK);
    CHECK(updated.integer == created.integer);
    CHECK(fixture.device.creates == 1);
    CHECK(fixture.device.updates == 1);
    CHECK(fixture.device.updatedHandle == handle);
    const auto destroyed = fixture.call("destroy_postfx", "bloom");
    CHECK(destroyed.status == LUA_OK);
    CHECK(destroyed.type == LUA_TBOOLEAN);
    CHECK(destroyed.boolean);
    CHECK(fixture.device.destroys == 1);
    CHECK(fixture.device.live.empty());
    CHECK(lua_gettop(fixture.state) == 0);
}

TEST_CASE("U19 Render postfx: unknown and empty names cannot claim or mutate Bloom") {
    for (const char* kind : { "typo", "" }) {
        CAPTURE(kind);
        PostFxLuaFixture fixture;
        REQUIRE(fixture.state != nullptr);
        const auto bloom = fixture.call("set_postfx", "bloom", true);
        REQUIRE(bloom.status == LUA_OK);
        REQUIRE(bloom.integer > 0);
        const auto handle = static_cast<IRenderDevice::PostFxHandle>(bloom.integer);
        const auto queries = fixture.device.supportQueries;

        const auto supported = fixture.call("is_postfx_supported", kind);
        CHECK(supported.status == LUA_OK);
        CHECK(supported.type == LUA_TBOOLEAN);
        CHECK_FALSE(supported.boolean);
        CHECK(fixture.device.supportQueries == queries);

        const auto rejected = fixture.call("set_postfx", kind, true);
        CHECK(rejected.status == LUA_OK);
        CHECK(rejected.type == LUA_TNUMBER);
        CHECK(rejected.integer == 0);
        CHECK(fixture.device.creates == 1);
        CHECK(fixture.device.updates == 0);
        CHECK(fixture.device.live.size() == 1);

        const auto destroyed = fixture.call("destroy_postfx", kind);
        CHECK(destroyed.status == LUA_OK);
        CHECK(destroyed.type == LUA_TBOOLEAN);
        CHECK_FALSE(destroyed.boolean);
        CHECK(fixture.device.destroys == 0);
        CHECK(fixture.device.live.size() == 1);
        CHECK(fixture.device.live.count(handle) == 1);
        CHECK(lua_gettop(fixture.state) == 0);
    }
}

TEST_CASE("U19 Render postfx: nil names retain Lua argument errors") {
    PostFxLuaFixture fixture;
    REQUIRE(fixture.state != nullptr);
    for (const char* function : { "is_postfx_supported", "set_postfx", "destroy_postfx" }) {
        CAPTURE(function);
        const auto reply = fixture.call(function, nullptr, std::string(function) == "set_postfx");
        CHECK(reply.status == LUA_ERRRUN);
        CHECK(reply.error.find("string expected") != std::string::npos);
    }
    CHECK(fixture.device.supportQueries == 0);
    CHECK(fixture.device.creates == 0);
    CHECK(fixture.device.destroys == 0);
    CHECK(fixture.device.live.empty());
    CHECK(lua_gettop(fixture.state) == 0);
}
