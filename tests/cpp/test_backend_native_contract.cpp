#include "doctest.h"
#include "EntryLifecycleBackends.h"
#include "di/BackendRegistry.h"
#include "script/bindings/DevCoreBinding.h"
#include "script/bindings/KAGBinding.h"
#include "script/bindings/RenderBinding.h"

#include <filesystem>
#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

using namespace Caesura;

namespace {
// Replace only the GPU boundary. Lua modules, factory dispatch, argument
// decoding and RTT ID resolution all remain the real production path.
class ContractRender final : public Test::RenderDevice {
public:
    explicit ContractRender(Test::LifecycleProbe& probe) : Test::RenderDevice(probe) {}

    struct TransitionCall {
        uint16_t view;
        RenderTextureHandle from, to, rule;
        int method;
        float progress;
    };
    struct BlendCall {
        uint16_t view;
        RenderTextureHandle base, blend;
        int mode;
        float baseAlpha, blendAlpha, globalAlpha;
    };

    ViewportHandle createRenderTarget(int width, int height) override {
        CHECK(width == 320);
        CHECK(height == 180);
        // IDs deliberately differ from VIEW_MAIN and from resolved texture IDs.
        const auto id = nextViewport++;
        viewports.push_back(id);
        return ViewportHandle{id};
    }
    RenderTextureHandle getViewportTexture(ViewportHandle viewport) override {
        for (const auto id : viewports) {
            if (id == viewport.id) return RenderTextureHandle{uint16_t(id + 100)};
        }
        return {};
    }
    void destroyRenderTarget(ViewportHandle viewport) override {
        destroyed.push_back(viewport.id);
        std::erase(viewports, viewport.id);
    }
    SceneSnapshot captureSceneSnapshot() override {
        if (!sceneAvailable) return {};
        capturedFrames.push_back(sceneFrame);
        return {createRenderTarget(320, 180), sceneFrame};
    }
    void cancelTransition() override { ++cancelCount; }
    void advanceFrame() override { ++sceneFrame; }
    int getBackbufferWidth() const override { return 320; }
    int getBackbufferHeight() const override { return 180; }
    void submitTransition(uint16_t view, RenderTextureHandle from, RenderTextureHandle to,
                          RenderTextureHandle rule, int method, float progress) override {
        transitions.push_back({view, from, to, rule, method, progress});
    }
    void submitBlend(uint16_t view, RenderTextureHandle base, RenderTextureHandle blend,
                     int mode, float baseAlpha, float blendAlpha, float globalAlpha) override {
        blends.push_back({view, base, blend, mode, baseAlpha, blendAlpha, globalAlpha});
    }

    std::vector<uint32_t> viewports, destroyed;
    std::vector<TransitionCall> transitions;
    std::vector<BlendCall> blends;
    uint32_t nextViewport = 41;
    uint64_t sceneFrame = 1;
    bool sceneAvailable = true;
    int cancelCount = 0;
    std::vector<uint64_t> capturedFrames;
};

struct NativeContractFixture {
    Test::LifecycleProbe probe;
    ContractRender render{probe};
    BackendRegistry& registry = BackendRegistry::instance();
    IRenderDevice* oldRender = registry.getRenderDevice();
    ITextureManager* oldTextures = registry.getTextureManager();
    std::unique_ptr<lua_State, decltype(&lua_close)> lua{luaL_newstate(), lua_close};
    std::string error;

    NativeContractFixture() {
        registry.setRenderDevice(&render);
        registry.setTextureManager(nullptr);
        if (!lua) return;
        auto* L = lua.get();
        luaL_openlibs(L);
        registerKAGBinding(L);
        registerRenderBinding(L);
        registerDevCoreBinding(L);
        const auto root = std::filesystem::path(CAESURA_SOURCE_DIR).generic_string();
        const auto search = root + "/scripts/?.lua;" + root + "/scripts/?/init.lua;";
        lua_pushlstring(L, search.data(), search.size());
        lua_setglobal(L, "CONTRACT_PACKAGE_PATH");
    }
    ~NativeContractFixture() {
        registry.setRenderDevice(oldRender);
        registry.setTextureManager(oldTextures);
        if (lua) {
            // Clear the binding pointer caches before this hardware boundary dies.
            registerRenderBinding(lua.get());
            registerKAGBinding(lua.get());
        }
    }
    bool run(const char* script) {
        lua_settop(lua.get(), 0);
        const int status = luaL_dostring(lua.get(), script);
        const char* detail = status == LUA_OK ? nullptr : lua_tostring(lua.get(), -1);
        error = status == LUA_OK ? "" : (detail ? detail : "non-string Lua error");
        return status == LUA_OK;
    }
    bool boot() {
        return run(R"lua(
            package.path = CONTRACT_PACKAGE_PATH .. package.path
            require('backend_factory').create()
            backend = require('backend')
            rtt = require('rtt')
            transition = require('transition')
        )lua");
    }
};
} // namespace

TEST_CASE("Native backend contract: real C entry points accept numeric RTT handles") {
    NativeContractFixture f;
    REQUIRE(f.lua != nullptr);
    REQUIRE_MESSAGE(f.boot(), f.error);
    REQUIRE_MESSAGE(f.run(R"lua(
        local a,b=rtt.create(320,180),rtt.create(320,180)
        assert(type(a)=='number' and type(b)=='number')
        assert(Render.submit_transition(a,b,0,2,0.25))
        assert(Render.submit_blend(a,b,1,1.0,0.25,0.6))
    )lua"), f.error);
    REQUIRE(f.render.transitions.size() == 1);
    REQUIRE(f.render.blends.size() == 1);
    CHECK(f.render.transitions[0].from.idx == 141);
    CHECK(f.render.transitions[0].to.idx == 142);
    CHECK(f.render.transitions[0].progress == doctest::Approx(0.25f));
    CHECK(f.render.blends[0].base.idx == 141);
    CHECK(f.render.blends[0].blend.idx == 142);
    CHECK(f.render.blends[0].blendAlpha == doctest::Approx(0.25f));
}

TEST_CASE("Native backend contract: actual RTT numbers reach transition binding without a view prefix") {
    NativeContractFixture f;
    REQUIRE(f.lua != nullptr);
    REQUIRE_MESSAGE(f.boot(), f.error);
    REQUIRE_MESSAGE(f.run(R"lua(
        a, b, rule = rtt.create(320,180), rtt.create(320,180), rtt.create(320,180)
        assert(type(a)=='number' and type(b)=='number' and type(rule)=='number')
    )lua"), f.error);
    int method = 0;
    SUBCASE("crossfade") {
        CHECK_MESSAGE(f.run("transition.start(a,b,{duration=1000});transition.tick(250)"), f.error);
    }
    SUBCASE("directional wipe") {
        method = 5;
        CHECK_MESSAGE(f.run("transition.start(a,b,{method='wipe',direction='bottom',duration=1000});transition.tick(250)"), f.error);
    }
    SUBCASE("rule texture") {
        method = 1;
        CHECK_MESSAGE(f.run("transition.start(a,b,{method='rule',rule_tex=rule,duration=1000});transition.tick(250)"), f.error);
    }
    REQUIRE(f.render.transitions.size() == 1);
    const auto& call = f.render.transitions.front();
    CHECK(call.view == VIEW_TRANSITION);
    CHECK(call.from.idx == 141);
    CHECK(call.to.idx == 142);
    CHECK(call.method == method);
    CHECK(call.progress == doctest::Approx(0.25f));
    CHECK(call.rule.isValid() == (method == 1));
    if (method == 1) CHECK(call.rule.idx == 143);
}

TEST_CASE("Native backend contract: blend Lua wrapper preserves texture mode and alpha slots") {
    NativeContractFixture f;
    REQUIRE(f.lua != nullptr);
    REQUIRE_MESSAGE(f.boot(), f.error);
    CHECK_MESSAGE(f.run(R"lua(
        local a,b=rtt.create(320,180),rtt.create(320,180)
        require('blend').blend_textures(a,b,'multiply',0.25,0.6,7)
    )lua"), f.error);
    REQUIRE(f.render.blends.size() == 1);
    const auto& call = f.render.blends.front();
    CHECK(call.view == VIEW_MAIN);
    CHECK(call.base.idx == 141);
    CHECK(call.blend.idx == 142);
    CHECK(call.mode == 1); // Native fs_blend/BlendMode::Multiply, not Lua's legacy enum.
    CHECK(call.baseAlpha == doctest::Approx(1.0f));
    CHECK(call.blendAlpha == doctest::Approx(0.25f));
    CHECK(call.globalAlpha == doctest::Approx(0.6f));
}

TEST_CASE("Native backend contract: real trans handler must not feed nil to the C binding") {
    NativeContractFixture f;
    REQUIRE(f.lua != nullptr);
    REQUIRE_MESSAGE(f.boot(), f.error);
    // No invented Transition.capture_screen, start or tick implementation.
    // This exposes the actual missing snapshot source independently of the
    // valid-RTT argument-order cases above. The full visual capture lifecycle
    // requires the real renderer implementation and a separate GPU test.
    REQUIRE_MESSAGE(f.run(R"lua(
        command=require('kag.commands.transition')
        ctx={f={},tf={},sf={},mp={},variables={},viewport={width=320,height=180}}
        co=coroutine.create(function() command.trans(ctx,{time=32,method='crossfade'}) end)
        local ok,err=coroutine.resume(co,16); assert(ok,err)
    )lua"), f.error);
    REQUIRE(f.render.transitions.size() == 1);
    CHECK(f.render.transitions.front().from.idx == f.render.transitions.front().to.idx);
    CHECK(f.render.transitions.front().progress == 0);
    for (int frame = 0; frame < 3; ++frame) {
        f.render.advanceFrame();
        REQUIRE_MESSAGE(f.run("local ok,err=coroutine.resume(co,16); assert(ok,err)"), f.error);
    }
    CHECK_MESSAGE(f.run(R"lua(
        assert(coroutine.status(co)=='dead','bounded transition did not finish')
        assert(#(ctx.active_operations or {})==0,'transition operation leaked')
    )lua"), f.error);
    REQUIRE(f.render.capturedFrames.size() == 2);
    CHECK(f.render.capturedFrames[0] == 1);
    CHECK(f.render.capturedFrames[1] == 2);
    CHECK(f.render.transitions.back().from.idx == 141);
    CHECK(f.render.transitions.back().to.idx == 142);
    CHECK(f.render.transitions.back().progress == 1);
    CHECK(f.render.destroyed == std::vector<uint32_t>{41, 42});
    CHECK(f.render.viewports.empty());
}

TEST_CASE("Native backend contract: all ten Lua GPU blend modes map to native shader modes") {
    NativeContractFixture f;
    REQUIRE_MESSAGE(f.boot(), f.error);
    REQUIRE_MESSAGE(f.run(R"lua(
        local a,b=rtt.create(320,180),rtt.create(320,180)
        for mode=0,9 do assert(backend.submit_blend(7,a,b,mode,0.25,0.6)) end
        assert(not pcall(backend.submit_blend,7,a,b,10,0.25,0.6))
    )lua"), f.error);
    // ShaderCache::BlendMode and shaders/glsl/fs_blend.sc use this order.
    const std::array<int,10> expected{0,16,17,1,2,3,4,5,10,9};
    REQUIRE(f.render.blends.size() == expected.size());
    for (size_t i=0; i<expected.size(); ++i) {
        const auto& call=f.render.blends[i];
        CHECK(call.mode == expected[i]);
        CHECK(call.base.idx == 141);
        CHECK(call.blend.idx == 142);
        CHECK(call.baseAlpha == doctest::Approx(1));
        CHECK(call.blendAlpha == doctest::Approx(0.25));
        CHECK(call.globalAlpha == doctest::Approx(0.6));
    }
}

TEST_CASE("Native backend contract: trans explicit aliases survive schema and reach the binding") {
    struct Case { const char* fields; int method; };
    const std::array<Case,8> cases{{
        {"method='wipe'",2}, {"type='wipe'",2}, {"kind='wipe'",2},
        {"type='fade'",0}, {"method='crossfade',type='wipe',kind='rule'",0},
        {"type='crossfade',kind='wipe'",0},
        {"method='wipe',type='fade',kind='crossfade'",2}, {"",0}
    }};
    for (const auto& item : cases) {
        NativeContractFixture f;
        REQUIRE_MESSAGE(f.boot(), f.error);
        const std::string script = std::string("local raw={time=32,") + item.fields + R"lua(}
            local ctx={f={},tf={},sf={},mp={},variables={}}
            local command=require('kag.commands.transition')
            local params=require('kag.schema').coerce('trans',raw,ctx)
            local co=coroutine.create(function() command.trans(ctx,params) end)
            local ok,err=coroutine.resume(co);assert(ok,err)
            assert(coroutine.close(co))
        )lua";
        REQUIRE_MESSAGE(f.run(script.c_str()), f.error);
        REQUIRE(f.render.transitions.size() == 1);
        CHECK_MESSAGE(f.render.transitions[0].method == item.method, item.fields);
        CHECK(f.render.transitions[0].from.idx == f.render.transitions[0].to.idx);
        CHECK(f.render.transitions[0].progress == 0);
        CHECK(f.render.viewports.empty());
    }
}

TEST_CASE("Native backend contract: trans capture cancellation and failure release only owned snapshots") {
    NativeContractFixture f;
    REQUIRE_MESSAGE(f.boot(), f.error);
    REQUIRE_MESSAGE(f.run(R"lua(
        command=require('kag.commands.transition')
        ctx={f={},tf={},sf={},mp={},variables={}}
        co=coroutine.create(function() command.trans(ctx,{time=32}) end)
        local ok,err=coroutine.resume(co); assert(ok,err)
    )lua"), f.error);
    SUBCASE("cancel during from hold") {
        REQUIRE_MESSAGE(f.run("require('kag.operation').cancel_all(ctx); assert(coroutine.resume(co)); assert(coroutine.status(co)=='dead')"), f.error);
        CHECK(f.render.destroyed == std::vector<uint32_t>{41});
    }
    SUBCASE("coroutine close") {
        REQUIRE_MESSAGE(f.run("assert(coroutine.close(co))"), f.error);
        CHECK(f.render.destroyed == std::vector<uint32_t>{41});
    }
    SUBCASE("renderer loses capture source") {
        f.render.sceneAvailable = false;
        f.render.advanceFrame();
        REQUIRE_MESSAGE(f.run("local ok,err=coroutine.resume(co); assert(not ok and err:find('no rendered scene')); coroutine.close(co)"), f.error);
        CHECK(f.render.destroyed == std::vector<uint32_t>{41});
    }
    SUBCASE("same frame is not a destination") {
        REQUIRE_MESSAGE(f.run("local ok,err=coroutine.resume(co); assert(not ok and err:find('not been rendered')); coroutine.close(co)"), f.error);
        CHECK(f.render.destroyed == std::vector<uint32_t>{41, 42});
    }
    CHECK(f.render.viewports.empty());
    CHECK_MESSAGE(f.run("assert(not transition.is_active()); assert(#(ctx.active_operations or {})==0)"), f.error);
}

TEST_CASE("Native backend contract: replacement cannot be cancelled by the previous coroutine") {
    NativeContractFixture f;
    REQUIRE_MESSAGE(f.boot(), f.error);
    REQUIRE_MESSAGE(f.run(R"lua(
        command=require('kag.commands.transition')
        oldctx={f={},tf={},sf={},mp={},variables={}}
        newctx={f={},tf={},sf={},mp={},variables={}}
        oldco=coroutine.create(function() command.trans(oldctx,{time=32}) end)
        assert(coroutine.resume(oldco))
    )lua"), f.error);
    f.render.advanceFrame();
    REQUIRE_MESSAGE(f.run(R"lua(
        newco=coroutine.create(function() command.trans(newctx,{time=32}) end)
        assert(coroutine.resume(newco))
        assert(coroutine.close(oldco))
        assert(transition.is_active(), 'old owner cancelled replacement')
    )lua"), f.error);
    CHECK(f.render.destroyed == std::vector<uint32_t>{41});
    for (int frame = 0; frame < 3; ++frame) {
        f.render.advanceFrame();
        REQUIRE_MESSAGE(f.run("local ok,err=coroutine.resume(newco,16); assert(ok,err)"), f.error);
    }
    CHECK(f.render.destroyed == std::vector<uint32_t>{41, 42, 43});
    CHECK(f.render.viewports.empty());
    CHECK_MESSAGE(f.run("assert(coroutine.status(newco)=='dead'); assert(#oldctx.active_operations==0 and #newctx.active_operations==0)"), f.error);
}

TEST_CASE("Native backend contract: an unavailable real scene is rejected before allocating a transition") {
    NativeContractFixture f;
    REQUIRE_MESSAGE(f.boot(), f.error);
    f.render.sceneAvailable = false;
    REQUIRE_MESSAGE(f.run(R"lua(
        ctx={f={},tf={},sf={},mp={},_render_epoch=0}
        co=coroutine.create(function()
            require('kag.commands.transition').trans(ctx,{time=32})
        end)
        local ok,signal=coroutine.resume(co)
        assert(ok and signal=='__kag_render_pending', tostring(signal))
        assert(coroutine.status(co)=='suspended')
        assert(ctx._transition_render_wait.epoch==1)
        assert(#ctx.active_operations==1)
        token=ctx.active_operations[1]
        -- An update/debug resume cannot stand in for a render boundary.
        ok,signal=coroutine.resume(co)
        assert(ok and signal=='__kag_render_pending', tostring(signal))
        assert(coroutine.status(co)=='suspended')
        assert(not transition.is_active())
    )lua"), f.error);
    CHECK(f.render.nextViewport == 41);
    CHECK(f.render.viewports.empty());
    CHECK(f.render.transitions.empty());
    // Explicit test-host render boundary only: no actual GPU rendering is
    // claimed, and the native renderer still reports no scene after this epoch.
    f.render.advanceFrame();
    REQUIRE_MESSAGE(f.run(R"lua(
        ctx._render_epoch=ctx._render_epoch+1
        local ok,err=coroutine.resume(co)
        assert(not ok and type(err)=='string' and
            err:find('transition preparation requires a real rendered scene',1,true), tostring(err))
        assert(coroutine.status(co)=='dead')
        -- Lua 5.4 closes failed coroutine scopes explicitly and reports the
        -- original error. Check cleanup before the fixture VM is destroyed.
        local closed,close_error=coroutine.close(co)
        assert(not closed and close_error==err, tostring(close_error))
        assert(ctx._transition_render_wait==nil)
        assert(#ctx.active_operations==0)
        assert(token.cancelled and #token.callbacks==0)
        assert(not transition.is_active())
    )lua"), f.error);
    CHECK(f.render.nextViewport == 41);
    CHECK(f.render.capturedFrames.empty());
    CHECK(f.render.destroyed.empty());
    CHECK(f.render.viewports.empty());
    CHECK(f.render.transitions.empty());
}
