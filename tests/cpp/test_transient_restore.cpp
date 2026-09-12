#include "doctest.h"
#include "di/BackendRegistry.h"
#include "di/SandboxQuota.h"
#include "di/api/ISandboxQuota.h"
#include "live2d/api/IAnimationBackend.h"
#include "render/ParticleSystem.h"
#include "render/api/IRenderDevice.h"
#include "render/api/IVideoPlayer.h"
#include "script/bindings/RestoreBinding.h"
#include "script/bindings/TransientRestoreBinding.h"
#include <limits>
#include <stdexcept>
#include <string>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

using namespace Caesura;

namespace {
class TransientVideo final : public IVideoPlayer {
public:
    VideoHandle open(const char*) override { ++active; return {1}; }
    void close(VideoHandle) override { active = 0; }
    void closeAll() override {
        ++closes;
        if (failClose) throw std::runtime_error("injected video close failure");
        active = 0;
    }
    void setLoop(VideoHandle, bool) override {}
    void setVolume(VideoHandle, float) override {}
    bool update(VideoHandle, double) override { return false; }
    void updateAll(double) override {}
    uint32_t getTexture(VideoHandle) const override { return 0; }
    bool isPlaying(VideoHandle) const override { return false; }
    bool hasEnded(VideoHandle) const override { return true; }
    int width(VideoHandle) const override { return 0; }
    int height(VideoHandle) const override { return 0; }
    double duration(VideoHandle) const override { return 0; }
    double currentTime(VideoHandle) const override { return 0; }
    void pause(VideoHandle) override {}
    void resume(VideoHandle) override {}
    void seek(VideoHandle, double) override {}
    void shutdown() override { ++shutdowns; }
    int activeCount() const override {
        if (failCapture) throw std::runtime_error("injected video capture failure");
        return active;
    }
    int active = 2, closes = 0, shutdowns = 0;
    bool failClose = false, failCapture = false;
};

class TransientAnimation final : public IAnimationBackend {
public:
    bool init() override { ++initializations; return true; }
    void shutdown() override { ++shutdowns; }
    int loadModel(const std::string&, const std::string&) override { ++models; return 1; }
    void unloadModel(int) override {}
    bool isLoaded(int) const override { return false; }
    size_t loadedModelCount() const override { return models; }
    void clearModels() override {
        ++clears;
        if (failClear) throw std::runtime_error("injected model clear failure");
        models = 0;
    }
    void showModel(int, float, float, float) override {}
    void hideModel(int) override {}
    void setOpacity(int, float) override {}
    void render(float) override {}
    bool playMotion(int, const std::string&) override { return false; }
    void setExpression(int, const std::string&) override {}
    void setParameter(int, const std::string&, float) override {}
    const char* name() const override { return "TransientAnimation"; }
    size_t models = 3;
    int clears = 0, shutdowns = 0, initializations = 0;
    bool failClear = false;
};

class TransientRender final : public IRenderDevice {
public:
    bool init(void*, int, int) override { return true; }
    void setPresentSize(uint32_t, uint32_t) override {}
    bool isInitialized() const override { return true; }
    void beginShutdown() override {}
    void shutdown() override { ++shutdowns; }
    void flushAllRTT() override {}
    void beginFrame() override {}
    void endFrame() override {}
    void commit_frame() override {}
    void advanceFrame() override {}
    void setScreenOffset(int, int) override {}
    void setViewRect(uint16_t, uint16_t, uint16_t, uint16_t, uint16_t) override {}
    void setViewClear(uint16_t, uint16_t, uint32_t, float, uint8_t) override {}
    void touch(uint16_t) override {}
    ViewportHandle createRenderTarget(int, int) override { return {}; }
    void destroyRenderTarget(ViewportHandle) override {}
    void blitViewport(ViewportHandle, uint16_t, float, float, float, float) override {}
    RenderTextureHandle getViewportTexture(ViewportHandle) override { return {}; }
    int getBackbufferWidth() const override { return 1280; }
    int getBackbufferHeight() const override { return 720; }
    void resize(int, int) override {}
    void blitTexture(uint16_t, uint32_t, float, float, float, float, uint8_t) override {}
    void stretchBlt(uint16_t, uint32_t, float, float, float, float,
                    uint32_t, float, float, float, float, int) override {}
    void affineBlt(uint16_t, uint32_t, float, float, float, float,
                   uint32_t, float, float, float, float, const float[6]) override {}
    void beginBatch() override {}
    void flushBatch() override {}
    void setDebugName(uint16_t, const std::string&) override {}
    void drawDebugOverlay(const std::string&) override {}
    bool requestScreenshot(const std::string&) override { return false; }
    Caesura::ScreenshotResult requestScreenshot(const Caesura::ScreenshotOptions&) override {
        Caesura::ScreenshotResult result;
        result.status = Caesura::ScreenshotStatus::Failed;
        result.error = "screenshots unsupported by test renderer";
        return result;
    }
    Caesura::ScreenshotResult takeScreenshot(const Caesura::ScreenshotTicket&) override { return {}; }
    bool cancelScreenshot(const Caesura::ScreenshotTicket&) override { return false; }
    bool recoverDevice(void*, int, int) override { return true; }
    void flagDeviceLost() override {}
    bool consumeDeviceLost() override { return false; }
    void renderText(uint16_t, const std::string&, float, float,
                    uint8_t, uint8_t, uint8_t, uint8_t, float, bool, bool, bool) override {}
    void renderRuby(uint16_t, const std::string&, const std::string&, float, float,
                    uint8_t, uint8_t, uint8_t, uint8_t) override {}
    void setFont(int) override {}
    bool loadTTF(const char*, float) override { return false; }
    FontRestoreState captureFontState() const override { return {}; }
    FontRestoreState defaultFontState() const override { return {}; }
    std::unique_ptr<IPreparedFontState> prepareFontState(
        const FontRestoreState&, const uint8_t*, size_t) override { return {}; }
    bool applyFontState(std::unique_ptr<IPreparedFontState>) override { return false; }
    void clearFontState() override {}
    float textLineHeight() const override { return 0; }
    void submitBlend(uint16_t, RenderTextureHandle, RenderTextureHandle, int,
                     float, float, float) override {}
    void submitTransition(uint16_t, RenderTextureHandle, RenderTextureHandle,
                          RenderTextureHandle, int, float) override {}
    void submitVFX(uint16_t, RenderTextureHandle, int, float, float, float,
                   float, float, float, float) override {}
    void fillViewport(ViewportHandle, uint8_t, uint8_t, uint8_t, uint8_t) override {}
    bool setColorFilter(ColorFilterPreset) override { return true; }
    bool isPostFxSupported(PostFxKind) const override { return true; }
    PostFxHandle createPostFx(PostFxKind, const PostFxParams&) override { return 1; }
    void setPostFxParams(PostFxHandle, const PostFxParams&) override {}
    void destroyPostFx(PostFxHandle) override {}
    void clearPostFx() override { ++clears; active = false; }
    bool isPostFxActive() const override { return active; }
    RenderUniformHandle getDefaultSampler() const override { return {}; }
    RenderProgramHandle getFallbackProgram() const override { return {}; }
    RenderProgramHandle getModulatedTextureProgram() const override { return {}; }
    const char* getBackendName() const override { return "TransientRender"; }
    RenderRuntimeInfo getRuntimeInfo() const override { return {}; }
    bool setPreferredBackend(const char*) override { return false; }
    bool active = true;
    int clears = 0, shutdowns = 0;
};

class TransientParticles final : public IParticleSystem {
public:
    bool init() override { ++initializations; initialized = initSucceeds; return initialized; }
    void shutdown() override { ++shutdowns; emitters = alive = 0; initialized = false; }
    int createEmitter(const ParticleEmitterConfig&) override { return ++emitters; }
    bool destroyEmitter(int) override { return false; }
    void emit(int, int) override {}
    void update(float, uint32_t, uint32_t) override {}
    void render(uint16_t) override {}
    int aliveCount() const override { return alive; }
    int activeEmitterCount() const override { return emitters; }
    bool isInitialized() const override { return initialized; }
    bool initialized = true, initSucceeds = true;
    int emitters = 1, alive = 7, shutdowns = 0, initializations = 0;
};

class TransientQuota final : public ISandboxQuota {
public:
    void setLuaState(lua_State*) override {}
    bool tryAlloc(const char*) override { ++emitters; return true; }
    void release(const char* kind) override {
        if (failRelease) {
            if (state) {
                lua_pushstring(state, luaErrorText);
                lua_error(state);
            }
            throw std::runtime_error("injected quota C++ error");
        }
        CHECK(std::string(kind) == "particles_emitters");
        REQUIRE(emitters > 0);
        --emitters;
    }
    int count(const char* kind) override {
        CHECK(std::string(kind) == "particles_emitters");
        return emitters;
    }
    int maxLimit(const char*) override { return 64; }
    int emitters = 2;
    lua_State* state = nullptr;
    const char* luaErrorText = "injected quota Lua error";
    bool failRelease = false;
};

struct TransientFixture {
    BackendRegistry& registry = BackendRegistry::instance();
    IVideoPlayer* oldVideo = registry.getVideoPlayer();
    IParticleSystem* oldParticles = registry.getParticleSystem();
    IAnimationBackend* oldAnimation = registry.getAnimationBackend();
    IRenderDevice* oldRender = registry.getRenderDevice();
    ISandboxQuota* oldQuota = registry.getSandboxQuota();
    lua_State* state = luaL_newstate();

    TransientFixture() {
        REQUIRE(state != nullptr);
        registry.setVideoPlayer(nullptr);
        registry.setParticleSystem(nullptr);
        registry.setAnimationBackend(nullptr);
        registry.setRenderDevice(nullptr);
        registry.setSandboxQuota(nullptr);
        luaL_openlibs(state);
        registerRestoreBinding(state);
        registerTransientRestoreBinding(state);
    }
    ~TransientFixture() {
        lua_close(state);
        registry.setVideoPlayer(oldVideo);
        registry.setParticleSystem(oldParticles);
        registry.setAnimationBackend(oldAnimation);
        registry.setRenderDevice(oldRender);
        registry.setSandboxQuota(oldQuota);
    }
    void run(const char* source) {
        const int result = luaL_dostring(state, source);
        INFO((result == LUA_OK ? "Lua completed" : lua_tostring(state, -1)));
        REQUIRE(result == LUA_OK);
        lua_settop(state, 0);
    }
};
}

TEST_CASE("U11 transient restore: absent backends produce an empty snapshot and repeatable stop") {
    TransientFixture fixture;
    fixture.run(R"(
        assert(type(Restore.prepare_image) == 'function')
        local state = assert(Restore.capture_transients())
        assert(type(state) == 'table' and getmetatable(state) == nil)
        assert(state.videos == 0 and state.particles == 0 and state.emitters == 0)
        assert(state.models == 0 and state.postfx == 0)
        assert(Restore.stop_transients() == true)
        assert(Restore.stop_transients() == true)
    )");
}

TEST_CASE("U11 transient restore: actual uninitialized particle emitters are counted and released") {
    ParticleSystem particles;
    TransientQuota quota;
    TransientFixture fixture;
    fixture.registry.setParticleSystem(&particles);
    fixture.registry.setSandboxQuota(&quota);
    const int first = particles.createEmitter(ParticleEmitterConfig{});
    particles.createEmitter(ParticleEmitterConfig{});
    fixture.run(R"(
        local state = assert(Restore.capture_transients())
        assert(state.emitters == 2 and state.particles == 0)
    )");
    CHECK(particles.activeEmitterCount() == 2);
    CHECK(quota.emitters == 2);
    fixture.run("assert(Restore.stop_transients() == true)");
    CHECK(particles.activeEmitterCount() == 0);
    CHECK_FALSE(particles.isInitialized());
    CHECK_FALSE(particles.destroyEmitter(first));
    CHECK(quota.emitters == 0);
    fixture.run("assert(Restore.stop_transients() == true)");
}

TEST_CASE("U11 transient restore: captures all backends without mutation and preserves them after stop") {
    TransientVideo video;
    TransientParticles particles;
    TransientAnimation animation;
    TransientRender renderer;
    TransientQuota quota;
    TransientFixture fixture;
    fixture.registry.setVideoPlayer(&video);
    fixture.registry.setParticleSystem(&particles);
    fixture.registry.setAnimationBackend(&animation);
    fixture.registry.setRenderDevice(&renderer);
    fixture.registry.setSandboxQuota(&quota);
    fixture.run(R"(
        local state = assert(Restore.capture_transients())
        assert(state.videos == 2 and state.particles == 7 and state.emitters == 1)
        assert(state.models == 3 and state.postfx == 1)
    )");
    CHECK(video.closes == 0);
    CHECK(particles.shutdowns == 0);
    CHECK(animation.clears == 0);
    CHECK(renderer.clears == 0);
    fixture.run(R"(
        assert(Restore.stop_transients() == true)
        local state = assert(Restore.capture_transients())
        for _, count in pairs(state) do assert(count == 0) end
        assert(Restore.stop_transients() == true)
    )");
    CHECK(video.shutdowns == 0);
    CHECK(particles.shutdowns == 1);
    CHECK(particles.initializations == 1);
    CHECK(particles.initialized);
    CHECK(animation.clears == 1);
    CHECK(animation.shutdowns == 0);
    CHECK(animation.initializations == 0);
    CHECK(renderer.clears == 1);
    CHECK(renderer.shutdowns == 0);
    CHECK(quota.emitters == 0);
    CHECK(video.open("after-restore").id == 1);
    CHECK(animation.loadModel("after-restore", "new") == 1);
}

TEST_CASE("U11 transient restore: one clear failure does not prevent other backend cleanup") {
    TransientVideo video;
    TransientParticles particles;
    TransientAnimation animation;
    TransientRender renderer;
    TransientQuota quota;
    TransientFixture fixture;
    fixture.registry.setVideoPlayer(&video);
    fixture.registry.setParticleSystem(&particles);
    fixture.registry.setAnimationBackend(&animation);
    fixture.registry.setRenderDevice(&renderer);
    fixture.registry.setSandboxQuota(&quota);
    SUBCASE("video failure") { video.failClose = true; }
    SUBCASE("animation failure") { animation.failClear = true; }
    SUBCASE("particle reinitialization failure") { particles.initSucceeds = false; }
    fixture.run(R"(
        local ok, err = Restore.stop_transients()
        assert(ok == false and type(err) == 'string' and #err > 0)
    )");
    CHECK(video.closes == 1);
    CHECK(video.shutdowns == 0);
    CHECK(particles.shutdowns == 1);
    CHECK(particles.initializations == 1);
    CHECK(animation.clears == 1);
    CHECK(renderer.clears == 1);
    CHECK(quota.emitters == 0);
}

TEST_CASE("U11 transient restore: capture exceptions return a Lua error value without mutation") {
    TransientVideo video;
    TransientFixture fixture;
    fixture.registry.setVideoPlayer(&video);
    video.failCapture = true;
    fixture.run(R"(
        local state, err = Restore.capture_transients()
        assert(state == nil and type(err) == 'string' and #err > 0)
    )");
    CHECK(video.active == 2);
    CHECK(video.closes == 0);
    CHECK(video.shutdowns == 0);
}

TEST_CASE("U11 transient restore: invalid backend counters reject capture") {
    TransientVideo video;
    TransientParticles particles;
    TransientAnimation animation;
    TransientFixture fixture;
    fixture.registry.setVideoPlayer(&video);
    fixture.registry.setParticleSystem(&particles);
    fixture.registry.setAnimationBackend(&animation);
    SUBCASE("negative video count") { video.active = -3; }
    SUBCASE("negative emitter count") { particles.emitters = -2; }
    SUBCASE("negative particle count") { particles.alive = -7; }
    if constexpr (sizeof(size_t) >= sizeof(lua_Integer)) {
        SUBCASE("model count does not fit a Lua integer") {
            animation.models = std::numeric_limits<size_t>::max();
        }
    }
    fixture.run(R"(
        local state, err = Restore.capture_transients()
        assert(state == nil and type(err) == 'string' and #err > 0)
    )");
    CHECK(video.closes == 0);
    CHECK(particles.shutdowns == 0);
    CHECK(animation.clears == 0);
}

TEST_CASE("U11 transient restore: quota errors preserve subsequent cleanup and particle initialization") {
    TransientParticles particles;
    TransientAnimation animation;
    TransientRender renderer;
    TransientQuota quota;
    TransientFixture fixture;
    fixture.registry.setParticleSystem(&particles);
    fixture.registry.setAnimationBackend(&animation);
    fixture.registry.setRenderDevice(&renderer);
    fixture.registry.setSandboxQuota(&quota);
    quota.failRelease = true;
    SUBCASE("C++ exception from quota release") {}
    SUBCASE("Lua error from quota release") { quota.state = fixture.state; }
    SUBCASE("empty Lua error from quota release") {
        quota.state = fixture.state;
        quota.luaErrorText = "";
    }
    SUBCASE("non-string Lua error from quota release") {
        quota.state = fixture.state;
        quota.luaErrorText = nullptr;
    }
    fixture.run(R"(
        local ok, err = Restore.stop_transients()
        assert(ok == false and type(err) == 'string' and #err > 0)
    )");
    CHECK(particles.shutdowns == 1);
    CHECK(particles.initializations == 1);
    CHECK(particles.initialized);
    CHECK(animation.clears == 1);
    CHECK(renderer.clears == 1);
    CHECK(quota.emitters == 2);
    quota.failRelease = false;
    fixture.run("assert(Restore.stop_transients() == true)");
    CHECK(quota.emitters == 0);
    CHECK(particles.shutdowns == 1);
    CHECK(particles.initializations == 1);
    CHECK(animation.clears == 1);
    CHECK(renderer.clears == 1);
}

TEST_CASE("U11 transient restore: particle lifecycle depends on remaining work") {
    TransientParticles particles;
    TransientFixture fixture;
    fixture.registry.setParticleSystem(&particles);
    particles.emitters = 0;
    SUBCASE("remaining particles without emitters") {
        fixture.run("assert(Restore.stop_transients() == true)");
        CHECK(particles.shutdowns == 1);
        CHECK(particles.initializations == 1);
        CHECK(particles.alive == 0);
    }
    SUBCASE("initialized but already idle") {
        particles.alive = 0;
        fixture.run("assert(Restore.stop_transients() == true)");
        CHECK(particles.shutdowns == 0);
        CHECK(particles.initializations == 0);
        CHECK(particles.initialized);
    }
}

TEST_CASE("U11 transient restore: coroutine cleanup releases quota owned by the main Lua state") {
    TransientParticles particles;
    SandboxQuotaService quota;
    TransientFixture fixture;
    fixture.registry.setParticleSystem(&particles);
    fixture.registry.setSandboxQuota(&quota);
    quota.setLuaState(fixture.state);
    fixture.run(R"(
        _SANDBOX_RESOURCES = { particles_emitters_loaded = 2 }
        local co = coroutine.create(function()
            assert(Restore.stop_transients() == true)
        end)
        local resumed, err = coroutine.resume(co)
        assert(resumed, err)
        assert(coroutine.status(co) == 'dead')
        assert(_SANDBOX_RESOURCES.particles_emitters_loaded == 0)
    )");
    CHECK(particles.shutdowns == 1);
    CHECK(particles.initializations == 1);
    CHECK(particles.initialized);
    CHECK(quota.count("particles_emitters") == 0);
}

TEST_CASE("U11 transient restore: main-state quota errors stay inside coroutine cleanup") {
    TransientParticles particles;
    TransientAnimation animation;
    TransientRender renderer;
    SandboxQuotaService quota;
    TransientFixture fixture;
    fixture.registry.setParticleSystem(&particles);
    fixture.registry.setAnimationBackend(&animation);
    fixture.registry.setRenderDevice(&renderer);
    fixture.registry.setSandboxQuota(&quota);
    quota.setLuaState(fixture.state);
    SUBCASE("count metamethod raises on the main Lua state") {
        fixture.run(R"(
            _SANDBOX_RESOURCES = setmetatable({}, {
                __index = function() error('injected real quota count error') end
            })
        )");
    }
    SUBCASE("release metamethod raises on the main Lua state") {
        fixture.run(R"(
            _SANDBOX_RESOURCES = setmetatable({}, {
                __index = function() return 2 end,
                __newindex = function() error('injected real quota release error') end
            })
        )");
    }
    lua_pushliteral(fixture.state, "preserved main stack value");
    const int stackTop = lua_gettop(fixture.state);
    const int result = luaL_dostring(fixture.state, R"(
        local co = coroutine.create(function()
            local ok, err = Restore.stop_transients()
            assert(ok == false and type(err) == 'string')
            assert(err:find('injected real quota', 1, true))
        end)
        local resumed, err = coroutine.resume(co)
        assert(resumed, err)
        assert(coroutine.status(co) == 'dead')
    )");
    INFO((result == LUA_OK ? "Lua completed" : lua_tostring(fixture.state, -1)));
    CHECK(result == LUA_OK);
    CHECK(lua_gettop(fixture.state) == stackTop);
    CHECK(std::string(lua_tostring(fixture.state, 1)) == "preserved main stack value");
    lua_settop(fixture.state, 0);
    CHECK(particles.shutdowns == 1);
    CHECK(particles.initializations == 1);
    CHECK(particles.initialized);
    CHECK(animation.clears == 1);
    CHECK(renderer.clears == 1);
}
