#include "doctest.h"
#include "EntryLifecycleBackends.h"
#include "di/BackendRegistry.h"
#include "script/bindings/RenderBinding.h"
#include "script/bindings/KAGBinding.h"
#include "script/bindings/DevCoreBinding.h"
#include <array>
#include <filesystem>
#include <string>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
using namespace Caesura;
namespace {
class BltRecorder : public Test::RenderDevice {
public:
    explicit BltRecorder(Test::LifecycleProbe& p):Test::RenderDevice(p) {}
    ViewportHandle createRenderTarget(int w,int h) override {rtw=w;rth=h;return {91};}
    RenderTextureHandle getViewportTexture(ViewportHandle h) override {
        return h.id == 42 ? RenderTextureHandle{142} : RenderTextureHandle{};
    }
    void stretchBlt(uint16_t v, uint32_t d, float x,float y,float w,float h,
                    uint32_t s,float sx,float sy,float sw,float sh,int f) override {
        ++stretch; view=v;dst=d;src=s;rects={x,y,w,h,sx,sy,sw,sh};filter=f;
    }
    void affineBlt(uint16_t v,uint32_t d,float x,float y,float w,float h,
                   uint32_t s,float sx,float sy,float sw,float sh,const float m[6]) override {
        ++affine;view=v;dst=d;src=s;rects={x,y,w,h,sx,sy,sw,sh};
        for(int i=0;i<6;++i) matrix[i]=m[i];
    }
    int stretch=0,affine=0,filter=-1,rtw=0,rth=0;
    uint16_t view=65535;uint32_t dst=0,src=0;
    std::array<float,8> rects{};std::array<float,6> matrix{};
};
struct BltFixture {
    Test::LifecycleProbe probe;BltRecorder render{probe};
    BackendRegistry& reg=BackendRegistry::instance();
    IRenderDevice* oldRender=reg.getRenderDevice();
    ITextureManager* oldTextures=reg.getTextureManager();
    lua_State* L=luaL_newstate();std::string error;
    BltFixture(){
        reg.setRenderDevice(&render);reg.setTextureManager(nullptr);
        if(!L)return;
        luaL_openlibs(L);registerRenderBinding(L);registerKAGBinding(L);registerDevCoreBinding(L);
        const auto search=std::filesystem::path(CAESURA_SOURCE_DIR).generic_string()+"/scripts/?.lua;";
        lua_pushlstring(L,search.data(),search.size());lua_setglobal(L,"CONTRACT_PATH");
    }
    ~BltFixture(){
        reg.setRenderDevice(oldRender);reg.setTextureManager(oldTextures);
        if(L){registerRenderBinding(L);registerKAGBinding(L);lua_close(L);}
    }
    bool run(const char* s){int rc=luaL_dostring(L,s);error=rc==LUA_OK?"":lua_tostring(L,-1);return rc==LUA_OK;}
};
}
TEST_CASE("Native blt contract: actual factory expands stretch rectangles") {
    BltFixture f;REQUIRE(f.L);
    REQUIRE_MESSAGE(f.run(R"(
        package.path=CONTRACT_PATH..package.path
        require('backend_factory').create()
        assert(require('backend').submit_stretch_blt(41,{x=1.25,y=2,w=30,h=40},42,{x=5,y=6,w=7,h=8},0))
    )"),f.error);
    CHECK(f.render.stretch==1);CHECK(f.render.view==VIEW_MAIN);
    CHECK(f.render.dst==41);CHECK(f.render.src==142);CHECK(f.render.filter==0);
    CHECK(f.render.rects==std::array<float,8>{1.25f,2,30,40,5,6,7,8});
}
TEST_CASE("Native blt contract: actual factory preserves affine matrix") {
    BltFixture f;REQUIRE(f.L);
    REQUIRE_MESSAGE(f.run(R"(
        package.path=CONTRACT_PATH..package.path
        require('backend_factory').create()
        assert(require('backend').submit_affine_blt(41,{x=1,y=2,w=30,h=40},42,{x=5,y=6,w=7,h=8},{a=2,b=3,c=4,d=5,tx=6,ty=7},1))
    )"),f.error);
    CHECK(f.render.affine==1);CHECK(f.render.view==VIEW_MAIN);
    CHECK(f.render.dst==41);CHECK(f.render.src==142);
    CHECK(f.render.rects==std::array<float,8>{1,2,30,40,5,6,7,8});
    CHECK(f.render.matrix==std::array<float,6>{2,3,4,5,6,7});
}
TEST_CASE("Native blt contract: logical fractional RTT size covers integer texture pixels") {
    BltFixture f;REQUIRE(f.L);
    REQUIRE_MESSAGE(f.run(R"(
        package.path=CONTRACT_PATH..package.path
        require('backend_factory').create()
        assert(require('rtt').create(12.5,8.5)==91)
    )"),f.error);
    CHECK(f.render.rtw==13);CHECK(f.render.rth==9);
}
