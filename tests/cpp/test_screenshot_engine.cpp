#include "doctest.h"
#include "EntryLifecycleBackends.h"
#include "TestPaths.h"
#include "entry/Engine.h"
#include "render/ScreenshotQueue.h"
#include "script/vm/LuaManager.h"
#include <stb/stb_image.h>
#include <array>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

using namespace Caesura;

namespace {
struct Frames {
    std::vector<std::string> events;
    Test::LifecycleProbe audio;
    ScreenshotQueue captures;
    std::vector<uint8_t> lastPng;
    bool stopping = false;
    bool lost = false;
    bool rejectCapture = false;
    bool failReadback = false;
    unsigned presented = 0;
    unsigned recovered = 0;
    uint64_t capturedFrame = 0;
};

class FramePlatform final : public IPlatformBackend {
public:
    explicit FramePlatform(Frames& frames) : f(frames) {}
    bool init(const char*, int w, int h) override { width=w; height=h; return true; }
    void shutdown() override {}
    bool pollEvent() override { return false; }
    MouseState getMouseState() const override { return {}; }
    uint64_t getTicksMs() const override { return 0; }
    void* getNativeWindowHandle() const override { return nullptr; }
    int getWindowWidth() const override { return width; }
    int getWindowHeight() const override { return height; }
    void setFullscreen(bool) override {}
    void resizeWindow(int w, int h) override { width=w; height=h; }
    const char* getBackendName() const override { return "FrameProbe"; }
    void postFrame() override { f.events.emplace_back("post"); }
    bool startTextInput() override { return true; }
    bool stopTextInput() override { return true; }
    bool setTextInputRect(int,int,int,int,int=0) override { return true; }
    bool isTextInputActive() const override { return false; }
private:
    Frames& f;
    int width=2, height=2;
};

class FrameRenderer final : public Test::RenderDevice {
public:
    FrameRenderer(Test::LifecycleProbe& probe, Frames& frames)
        : Test::RenderDevice(probe), f(frames) {}
    bool init(void* window, int w, int h) override {
        const bool ok=Test::RenderDevice::init(window,w,h);
        if(ok) f.captures.open();
        return ok;
    }
    void beginFrame() override { f.events.emplace_back("begin"); }
    void drawDebugOverlay(const std::string&) override { f.events.emplace_back("overlay"); }
    void commit_frame() override { f.events.emplace_back("commit"); }
    void advanceFrame() override {
        Test::RenderDevice::advanceFrame();
        if(f.stopping) return; // Ordered destruction drains are not new game frames.
        f.events.emplace_back("advance");
        static constexpr std::array<uint8_t,16> pixels={
            255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255};
        for(const auto& batch:f.captures.submit(++f.presented)) {
            f.captures.complete(batch.callbackName.c_str(),2,2,8,
                f.failReadback ? ScreenshotQueue::PixelFormat::Unsupported : ScreenshotQueue::PixelFormat::RGBA8,
                pixels.data(),static_cast<uint32_t>(pixels.size()),false);
        }
    }
    using Test::RenderDevice::requestScreenshot;
    ScreenshotResult requestScreenshot(const ScreenshotOptions& options) override {
        f.events.emplace_back("request");
        if(f.rejectCapture) {
            ScreenshotResult result;
            result.status=ScreenshotStatus::Failed;
            result.error="injected rejection";
            return result;
        }
        return f.captures.request(options,2,2);
    }
    ScreenshotResult takeScreenshot(const ScreenshotTicket& ticket) override {
        f.events.emplace_back("take");
        auto result=f.captures.take(ticket);
        if(result.status==ScreenshotStatus::Completed) {
            f.lastPng=result.png;
            f.capturedFrame=result.frameId;
        }
        return result;
    }
    bool cancelScreenshot(const ScreenshotTicket& ticket) override { return f.captures.cancel(ticket); }
    void beginShutdown() override {
        f.stopping=true;
        f.captures.close("shutdown");
        Test::RenderDevice::beginShutdown();
    }
    bool consumeDeviceLost() override { const bool lost=f.lost; f.lost=false; return lost; }
    bool recoverDevice(void*,int,int) override { ++f.recovered; f.captures.close("recovery failed"); return false; }
private:
    Frames& f;
};

EngineConfig configFor(Frames& frames, Test::LifecycleProbe& render,
                       Test::LifecycleProbe& animation) {
    EngineConfig config;
    config.headless=true;
    config.editorMode=true;
    config.width=2;
    config.height=2;
    config.render=new FrameRenderer(render,frames);
    config.audio=new Test::AudioBackend(frames.audio);
    config.platform=new FramePlatform(frames);
    config.animation=new Test::AnimationBackend(animation);
    return config;
}

void installFrameScript(Engine& engine, Frames& frames) {
    auto* L=engine.lua().state();
    lua_pushlightuserdata(L,&frames);
    lua_pushcclosure(L,[](lua_State* state) {
        auto* f=static_cast<Frames*>(lua_touserdata(state,lua_upvalueindex(1)));
        f->events.emplace_back(lua_tostring(state,1));
        return 0;
    },1);
    lua_setglobal(L,"record_frame_probe");
    const int status=luaL_dostring(L,
        "engine_update=function() record_frame_probe('update') end; "
        "engine_render=function() record_frame_probe('draw') end");
    REQUIRE(status == LUA_OK);
    frames.events.clear();
}

std::vector<uint8_t> decodeBase64(const std::string& text) {
    const std::string alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint32_t value=0;
    unsigned bits=0;
    std::vector<uint8_t> bytes;
    for(char c:text) {
        if(c=='=') break;
        const auto digit=alphabet.find(c);
        REQUIRE(digit!=std::string::npos);
        value=(value<<6)|static_cast<uint32_t>(digit);
        bits+=6;
        if(bits>=8) { bits-=8; bytes.push_back(static_cast<uint8_t>(value>>bits)); }
    }
    return bytes;
}
}

TEST_CASE("U15 engine presents a complete managed frame once") {
    Frames frames;
    Test::LifecycleProbe renderer,animation;
    Engine engine(configFor(frames,renderer,animation));
    REQUIRE(engine.init());
    installFrameScript(engine,frames);
    engine.renderOneFrame();
    CHECK(frames.events==std::vector<std::string>{"update","begin","draw","overlay","commit","advance","post"});
    CHECK(frames.presented==1);
    CHECK(animation.animationRenderCalls==1);
    engine.shutdown();
    CHECK(renderer.advanceCalls==3); // One game frame plus the existing two destruction drains.
    const auto closedEvents=frames.events;
    engine.renderOneFrame();
    CHECK(engine.captureFrameForRpc(1,1).empty());
    CHECK(frames.events==closedEvents);
}

TEST_CASE("U15 engine RPC capture consumes the PNG from its own frame") {
    Frames frames;
    Test::LifecycleProbe renderer,animation;
    Engine engine(configFor(frames,renderer,animation));
    REQUIRE(engine.init());
    installFrameScript(engine,frames);
    const auto encoded=engine.captureFrameForRpc(1,1);
    REQUIRE_FALSE(encoded.empty());
    CHECK(frames.events==std::vector<std::string>{"update","begin","draw","overlay","commit","request","advance","post","take"});
    const auto png=decodeBase64(encoded);
    CHECK(png==frames.lastPng);
    int w=0,h=0,channels=0;
    auto* pixels=stbi_load_from_memory(png.data(),static_cast<int>(png.size()),&w,&h,&channels,4);
    REQUIRE(pixels!=nullptr);
    CHECK(w==1);
    CHECK(h==1);
    CHECK(pixels[0]==255);
    // Center-sampled nearest resize maps 2x2 -> 1x1 to the white bottom-right pixel.
    CHECK(pixels[1]==255);
    CHECK(pixels[2]==255);
    stbi_image_free(pixels);
    CHECK(frames.capturedFrame==1);
    CHECK(frames.captures.retainedCount()==0);
}

TEST_CASE("U15 engine capture rejection does not strand a rendered frame") {
    Frames frames;
    frames.rejectCapture=true;
    Test::LifecycleProbe renderer,animation;
    Engine engine(configFor(frames,renderer,animation));
    REQUIRE(engine.init());
    installFrameScript(engine,frames);
    CHECK(engine.captureFrameForRpc(1,1).empty());
    CHECK(frames.presented==1);
    CHECK(frames.events.back()=="post");
    CHECK(frames.captures.retainedCount()==0);
}

TEST_CASE("U15 engine stops rendering after failed GPU recovery and retains teardown") {
    Frames frames;
    Test::LifecycleProbe renderer,animation;
    auto config=configFor(frames,renderer,animation);
    config.frameLimit=2;
    Engine engine(std::move(config));
    REQUIRE(engine.init());
    installFrameScript(engine,frames);
    frames.lost=true;
    unsigned pumps=0;
    engine.run([&] { if(++pumps>2) engine.quit(); });
    CHECK(engine.hasRenderFailure());
    CHECK(frames.recovered==1);
    CHECK(frames.presented==0);
    CHECK(frames.events.empty());
    engine.renderOneFrame();
    CHECK(engine.captureFrameForRpc(1,1).empty());
    CHECK(frames.events.empty());
    CHECK(animation.animationRenderCalls==0);
    engine.shutdown();
    CHECK(renderer.beginShutdownCalls==1);
    CHECK(renderer.advanceCalls==2);
    CHECK(renderer.shutdownCalls==1);
}

TEST_CASE("U15 engine export writes matched completed PNGs and reports readback failure") {
    Frames frames;
    bool failure=false;
    SUBCASE("complete") {}
    SUBCASE("readback failure") { failure=true; }
    frames.failReadback=failure;
    TestPaths::ScopedTempDir output("u15_export");
    Test::LifecycleProbe renderer,animation;
    auto config=configFor(frames,renderer,animation);
    config.frameLimit=2;
    config.exportReplayFile="controlled-export";
    config.exportDir=output.path().string();
    Engine engine(std::move(config));
    REQUIRE(engine.init());
    installFrameScript(engine,frames);
    engine.run();
    CHECK(engine.hasRenderFailure()==failure);
    CHECK(frames.presented==(failure ? 1u : 2u));
    CHECK(frames.captures.retainedCount()==0);
    for(unsigned i=0;i<2;++i) {
        const auto path=output.path()/(i==0 ? "frame_00000.png" : "frame_00001.png");
        CHECK(std::filesystem::exists(path)==!failure);
        if(!failure) {
            std::ifstream input(path,std::ios::binary);
            const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
            CHECK(bytes==frames.lastPng);
        }
    }
}
