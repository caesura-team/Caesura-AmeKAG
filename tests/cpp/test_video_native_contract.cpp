#include "doctest.h"
#include "EntryLifecycleBackends.h"
#include "di/BackendRegistry.h"
#include "render/api/IVideoPlayer.h"
#include "resource/api/IAssetReader.h"
#include <iterator>
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
    VideoHandle open(const char* p) override {path=p;playing=true;++opens;return {9};}
    VideoHandle openMemory(std::vector<uint8_t> bytes) override { if(bytes.empty()) return {}; ++memoryOpens;playing=true;return {9}; }
    void close(VideoHandle h) override {CHECK(h.id==9);playing=false;++closes;}
    void closeAll() override {playing=false;}
    void setLoop(VideoHandle,bool v) override {loop=v;}
    void setVolume(VideoHandle,float v) override {volume=v;}
    bool update(VideoHandle,double) override {return playing;}
    void updateAll(double) override {}
    uint32_t getTexture(VideoHandle h) const override {return h.id==9&&playing?142:0;}
    bool isPlaying(VideoHandle h) const override {return h.id==9&&playing;}
    bool hasEnded(VideoHandle) const override {return !playing;}
    int width(VideoHandle) const override {return 640;}
    int height(VideoHandle) const override {return 360;}
    double duration(VideoHandle) const override {return 10;}
    double currentTime(VideoHandle) const override {return 0;}
    void pause(VideoHandle) override {}
    void resume(VideoHandle) override {}
    void seek(VideoHandle,double) override {}
    void shutdown() override {playing=false;}
    int activeCount() const override {return playing?1:0;}
    std::string path;bool playing=false,loop=false;float volume=1;int opens=0,closes=0,memoryOpens=0;
};
class VideoAssetReader : public IAssetReader {
public:
    Caesura::AssetDirectoryResult listDirectory(const std::string&, size_t, size_t) override { return {}; }
    std::vector<uint8_t> readAsset(const std::string& path,size_t limit) override {
        if(path!="clip.mpg") return {};
        std::ifstream input(std::filesystem::path(CAESURA_SOURCE_DIR)/"tests/audio/restore-video.mpg",std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
        return bytes.size()<=limit?bytes:std::vector<uint8_t>{};
    }
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
TEST_CASE("Native video contract: live runner renders decoded video in requested rectangle") {
    BackendRegistry& reg=BackendRegistry::instance();
    auto* oldRender=reg.getRenderDevice();auto* oldVideo=reg.getVideoPlayer();auto* oldAssets=reg.getAssetReader();
    Test::LifecycleProbe probe;VideoDrawRecorder render(probe);VideoRecorder video;VideoAssetReader assets;
    reg.setRenderDevice(&render);reg.setVideoPlayer(&video);reg.setAssetReader(&assets);
    lua_State* L=luaL_newstate();
    struct Cleanup {
        BackendRegistry& reg;IRenderDevice* render;IVideoPlayer* video;IAssetReader* assets;lua_State* L;
        ~Cleanup(){
            if(L){luaL_dostring(L,"if runner then runner.stop() end");lua_close(L);}
            reg.setRenderDevice(render);reg.setVideoPlayer(video);reg.setAssetReader(assets);
            std::error_code ec;std::filesystem::remove("video-native-contract.ks",ec);
        }
    } cleanup{reg,oldRender,oldVideo,oldAssets,L};
    REQUIRE(L);luaL_openlibs(L);registerRenderBinding(L);registerKAGBinding(L);registerDevCoreBinding(L);
    const auto path=std::filesystem::path(CAESURA_SOURCE_DIR).generic_string()+"/scripts/?.lua;";
    lua_pushlstring(L,path.data(),path.size());lua_setglobal(L,"CONTRACT_PATH");
    {std::ofstream f("video-native-contract.ks");f<<"[video storage=\"clip.mpg\" x=11 y=17 w=100 h=60 volume=0.4]\n[end]\n";REQUIRE(f.good());}
    const int result=luaL_dostring(L,R"(
        package.path=CONTRACT_PATH..package.path
        require('backend_factory').create()
        runner=require('kag_runner')
        local ok,err=runner.start('video-native-contract.ks');assert(ok,err)
        runner.update(16)
        assert(runner.get_ctx()._videoPlayback,'video must actually be active')
        runner.render()
    )");
    const std::string error=result==LUA_OK?"":lua_tostring(L,-1);
    REQUIRE_MESSAGE(result==LUA_OK,error);
    CHECK(video.opens==0);CHECK(video.memoryOpens==1);CHECK(video.volume==doctest::Approx(0.4));
    CHECK(render.videoDraws==1);CHECK(render.view==VIEW_MAIN);
    CHECK(render.dx==11);CHECK(render.dy==17);CHECK(render.dw==100);CHECK(render.dh==60);CHECK(render.alpha==255);
    REQUIRE(luaL_dostring(L,"runner.stop();runner.render()") == LUA_OK);
    CHECK(video.closes==1);CHECK(render.videoDraws==1);
}
