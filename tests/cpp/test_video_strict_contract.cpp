#include "doctest.h"
#include "EntryLifecycleBackends.h"
#include "di/BackendRegistry.h"
#include "render/api/IVideoPlayer.h"
#include "resource/api/IAssetReader.h"
#include <iterator>
#include <stdexcept>
#include <unordered_set>
#include "script/bindings/RenderBinding.h"
#include "script/bindings/KAGBinding.h"
#include "script/bindings/DevCoreBinding.h"
#include <filesystem>
#include <fstream>
#include <string>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
using namespace Caesura;
namespace {
class VideoRecorder : public IVideoPlayer {
public:
    VideoHandle open(const char* p) override {path=p;playing=true;++opens;live.insert(9);return {9};}
    VideoHandle openMemory(std::vector<uint8_t> bytes) override { if(bytes.empty()) return {}; const uint32_t id=9+memoryOpens++;live.insert(id);playing=true;return {id}; }
    void close(VideoHandle h) override {CHECK(live.erase(h.id)==1);playing=!live.empty();++closes;}
    void closeAll() override {live.clear();playing=false;}
    void setLoop(VideoHandle,bool v) override {loop=v;}
    void setVolume(VideoHandle,float v) override {volume=v;}
    bool update(VideoHandle,double) override {return playing;}
    void updateAll(double) override {}
    uint32_t getTexture(VideoHandle h) const override {return live.count(h.id)?142:0;}
    bool isPlaying(VideoHandle h) const override {return live.count(h.id)!=0;}
    bool hasEnded(VideoHandle h) const override {return !isPlaying(h);}
    int width(VideoHandle) const override {return 640;}
    int height(VideoHandle) const override {return 360;}
    double duration(VideoHandle) const override {return 10;}
    double currentTime(VideoHandle) const override {return 0;}
    void pause(VideoHandle) override {}
    void resume(VideoHandle) override {}
    void seek(VideoHandle,double) override {}
    void shutdown() override {closeAll();}
    int activeCount() const override {return static_cast<int>(live.size());}
    std::unordered_set<uint32_t> live;
    std::string path;bool playing=false,loop=false;float volume=1;int opens=0,closes=0,memoryOpens=0;
};
class VideoDrawRecorder : public Test::RenderDevice {
public:
    explicit VideoDrawRecorder(Test::LifecycleProbe& p):Test::RenderDevice(p){}
    void blitTexture(uint16_t v,uint32_t t,float x,float y,float w,float h,uint8_t a) override {
        if(t==142){++videoDraws;view=v;dx=x;dy=y;dw=w;dh=h;alpha=a;}
    }
    int getBackbufferWidth() const override {return 320;}
    int getBackbufferHeight() const override {return 180;}
    int videoDraws=0;uint16_t view=0;float dx=0,dy=0,dw=0,dh=0;uint8_t alpha=0;
};
}

namespace {
class StrictVideoAssets final : public IAssetReader {
public:
    Caesura::AssetDirectoryResult listDirectory(const std::string&, size_t, size_t) override { return {}; }
    std::vector<uint8_t> readAsset(const std::string& name, size_t limit) override {
        ++reads; lastPath=name; lastLimit=limit;
        if(throwRead) throw std::runtime_error("controlled reader failure");
        if(emptyRead) return {};
        if(name!="assets/video/clip.mpg") return {};
        std::ifstream input(std::filesystem::path(CAESURA_SOURCE_DIR)/"tests/audio/restore-video.mpg",std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
        return bytes.size()<=limit?bytes:std::vector<uint8_t>{};
    }
    int reads=0;std::string lastPath;size_t lastLimit=0;bool throwRead=false,emptyRead=false;
};
struct StrictVideoContext {
    BackendRegistry& registry = BackendRegistry::instance();
    IRenderDevice* priorRender = registry.getRenderDevice();
    IVideoPlayer* priorVideo = registry.getVideoPlayer();
    IAssetReader* priorAssets = registry.getAssetReader();
    StrictVideoAssets assets;
    Test::LifecycleProbe probe;
    VideoDrawRecorder render{probe};
    VideoRecorder video;
    lua_State* L = luaL_newstate();
    StrictVideoContext() {
        REQUIRE(L != nullptr);
        registry.setRenderDevice(&render); registry.setVideoPlayer(&video); registry.setAssetReader(&assets);
        luaL_openlibs(L); registerRenderBinding(L); registerKAGBinding(L); registerDevCoreBinding(L);
        const auto scripts = std::filesystem::path(CAESURA_SOURCE_DIR) / "scripts";
        const auto path = scripts.generic_string() + "/?.lua;";
        lua_pushlstring(L,path.data(),path.size()); lua_setglobal(L,"CONTRACT_PATH");
        run(R"(
            package.path=CONTRACT_PATH..package.path
            require('backend_factory').create()
            runner=require('kag_runner')
            require('replay') -- Real native preload in scripts/kag/init.lua, before sandbox.
            _CAESURA_CONFIG={dev_mode=false}
        )");
    }
    ~StrictVideoContext() {
        if(L){luaL_dostring(L,"if runner then pcall(runner.stop) end");lua_close(L);}
        registry.setRenderDevice(priorRender);registry.setVideoPlayer(priorVideo);registry.setAssetReader(priorAssets);
        std::error_code error;std::filesystem::remove("strict-video-native-contract.ks",error);
    }
    void run(const char* code) {
        const int result=luaL_dostring(L,code);
        const std::string error=result==LUA_OK?"":(lua_tostring(L,-1)?lua_tostring(L,-1):"non-string Lua error");
        REQUIRE_MESSAGE(result==LUA_OK,error);
    }
    void lockdown() {
        const auto path=(std::filesystem::path(CAESURA_SOURCE_DIR)/"scripts/sandbox.lua").string();
        const int result=luaL_dofile(L,path.c_str());
        const std::string error=result==LUA_OK?"":(lua_tostring(L,-1)?lua_tostring(L,-1):"non-string Lua error");
        REQUIRE_MESSAGE(result==LUA_OK,error);
        run("assert(_SANDBOX_MODE=='strict')");
    }
};
}
TEST_CASE("Strict video contract: raw video capability remains forbidden") {
    StrictVideoContext context;
    context.lockdown();
    context.run(R"(
        for _,name in ipairs({'video_play','video_draw','video_stop','video_update',
            'video_get_texture','video_is_playing','video_has_ended','video_pause','video_resume'}) do
            local ok,value=pcall(function() return Render[name] end)
            assert(not ok or value==nil,'raw video unexpectedly exposed: '..name)
        end
    )");
    CHECK(context.video.opens==0); CHECK(context.render.videoDraws==0);
}
TEST_CASE("Strict video contract: legal KAG scene reaches playback and draw after lockdown") {
    StrictVideoContext context;
    // Start reaches only the ordinary barrier. Prove no decoder/asset work
    // happened, then execute the video command after strict proxy installation.
    {std::ofstream f("strict-video-native-contract.ks");
     f<<"[set var=\"f.video_barrier\" value=1]\n[video storage=\"assets/video/clip.mpg\" x=11 y=17 w=100 h=60 volume=0.4]\n[end]\n";REQUIRE(f.good());}
    context.run("local ok,err=runner.start('strict-video-native-contract.ks');assert(ok,err);assert(runner.get_ctx().f.video_barrier==1)");
    CHECK(context.assets.reads==0); CHECK(context.video.memoryOpens==0); CHECK(context.video.opens==0);
    context.lockdown();
    context.run(R"(
        runner.update(16)
        local ctx=runner.get_ctx()
        assert(ctx and not ctx._command_error,'strict command errored')
        assert(ctx._videoPlayback,'legal asset video did not start')
        runner.render()
    )");
    CHECK(context.assets.reads==1); CHECK(context.assets.lastPath=="assets/video/clip.mpg");
    CHECK(context.assets.lastLimit>0); CHECK(context.assets.lastLimit<=64u*1024u*1024u);
    CHECK(context.video.opens==0); // Raw filename decoder must never authorize strict asset playback.
    CHECK(context.video.memoryOpens==1); CHECK(context.video.playing); CHECK(context.video.volume==doctest::Approx(0.4));
    CHECK(context.render.videoDraws==1); CHECK(context.render.dx==11); CHECK(context.render.dy==17);
    CHECK(context.render.dw==100); CHECK(context.render.dh==60);
    context.run("runner.stop();runner.render()");
    CHECK_FALSE(context.video.playing); CHECK(context.video.closes==1);
}
TEST_CASE("Strict video contract: hostile paths are explicit refusals without decoder effects") {
    StrictVideoContext context;
    context.run("contract_backend=require('backend')");
    context.lockdown();
    context.run(R"(
        for _,path in ipairs({'https://127.0.0.1/never-request.m3u8','file:///private/secret',
            '../outside.mpg','assets/../../outside.mpg','C:/outside.mpg',
            'assets\\video\\clip.mpg','assets/video/clip.mpg'..string.char(0)..'tail'}) do
            local ok,id,reason=pcall(contract_backend.video_play,path,{loop=false,volume=0.4})
            assert(ok,'asset-only boundary threw instead of returning a refusal')
            assert(not id or id==0,'hostile path accepted')
            assert(type(reason)=='string' and #reason>0,'missing refusal reason')
        end
    )");
    CHECK(context.assets.reads==0); CHECK(context.video.opens==0); CHECK(context.video.closes==0); CHECK(context.render.videoDraws==0);
}

TEST_CASE("Strict video contract: invalid options and rectangles have no effects") {
    StrictVideoContext context;context.lockdown();
    context.run(R"(
        for _,opts in ipairs({false,1,'bad',{loop=1},{volume='1'},{volume=0/0},{volume=math.huge},{volume=-1}}) do
            local id,err=KAG.video_asset_play('assets/video/clip.mpg',opts)
            assert(id==nil and type(err)=='string')
        end
    )");
    CHECK(context.assets.reads==0);CHECK(context.video.memoryOpens==0);CHECK(context.video.opens==0);
    context.run(R"(
        local h=assert(KAG.video_asset_play('assets/video/clip.mpg',{volume=1.5}))
        assert(KAG.video_asset_draw(h,0/0,0,10,10)==false)
        assert(KAG.video_asset_draw(h,0,0,math.huge,10)==false)
        assert(KAG.video_asset_draw(h,0,0,-1,10)==false)
        assert(KAG.video_asset_stop(h)==true)
        assert(KAG.video_asset_stop(h)==false)
        assert(KAG.video_asset_draw(h,0,0,10,10)==false)
        assert(KAG.video_asset_stop(9)==false) -- raw backend handle is not authority
    )");
    CHECK(context.video.volume==doctest::Approx(1.5));CHECK(context.video.closes==1);CHECK(context.render.videoDraws==0);
}
TEST_CASE("Strict video contract: backend generation rejects same pointer re-registration") {
    StrictVideoContext context;
    context.run("safe_video_token=assert(KAG.video_asset_play('assets/video/clip.mpg'))");
    context.lockdown();
    context.registry.setVideoPlayer(&context.video); // Actual registration boundary, not fake token metadata.
    context.run("assert(KAG.video_asset_is_playing(safe_video_token)==false);assert(KAG.video_asset_stop(safe_video_token)==false)");
    CHECK(context.video.closes==0);CHECK(context.video.playing);
    lua_close(context.L);context.L=nullptr;CHECK(context.video.closes==0); // GC must not dereference stale generation.
    context.video.closeAll(); // Old backend owns its own retirement, not the expired token.
}
TEST_CASE("Strict video contract: session cleanup is owned by its Lua state") {
    StrictVideoContext context;
    context.run("safe_video_token=assert(KAG.video_asset_play('assets/video/clip.mpg'))");
    lua_getglobal(context.L,"safe_video_token");const auto token=lua_tointeger(context.L,-1);lua_pop(context.L,1);
    lua_State* other=luaL_newstate();REQUIRE(other);luaL_openlibs(other);registerKAGBinding(other);
    lua_pushinteger(other,token);lua_setglobal(other,"foreign_token");
    CHECK(luaL_dostring(other,"assert(KAG.video_asset_stop(foreign_token)==false)")==LUA_OK);
    lua_close(other);CHECK(context.video.closes==0);
    lua_close(context.L);context.L=nullptr;CHECK(context.video.closes==1);CHECK_FALSE(context.video.playing);
}

TEST_CASE("Strict video contract: reader failures do not fall back to raw filenames") {
    StrictVideoContext context;context.lockdown();
    context.assets.throwRead=true;
    context.run("local h,e=KAG.video_asset_play('assets/video/clip.mpg');assert(h==nil and type(e)=='string')");
    context.assets.throwRead=false;context.assets.emptyRead=true;
    context.run("local h,e=KAG.video_asset_play('assets/video/clip.mpg');assert(h==nil and type(e)=='string')");
    CHECK(context.assets.reads==2);CHECK(context.video.opens==0);CHECK(context.video.memoryOpens==0);
    context.assets.emptyRead=false;
    context.run("local h=assert(KAG.video_asset_play('assets/video/clip.mpg'));assert(KAG.video_asset_stop(h))");
    CHECK(context.video.memoryOpens==1);CHECK(context.video.closes==1);
}
TEST_CASE("Strict video contract: session cap refuses before another asset read") {
    StrictVideoContext context;context.lockdown();
    context.run(R"(
        local handles={}
        for i=1,4 do handles[i]=assert(KAG.video_asset_play('assets/video/clip.mpg')) end
        local h,e=KAG.video_asset_play('assets/video/clip.mpg')
        assert(h==nil and type(e)=='string')
        for _,id in ipairs(handles) do assert(KAG.video_asset_stop(id)) end
    )");
    CHECK(context.assets.reads==4);CHECK(context.video.memoryOpens==4);CHECK(context.video.opens==0);CHECK(context.video.closes==4);
}
