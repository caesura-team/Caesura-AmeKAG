#include "doctest.h"
#include "render/TextRenderer.h"
#include "render/NullRenderDevice.h"
#include "render/BgfxRenderDevice.h"
#include "HiddenGpuContext.h"
#include "di/BackendRegistry.h"
#include "resource/api/IAssetReader.h"
#include "script/vm/LuaManager.h"
#include <fstream>
#include <iterator>
#include <limits>
#include <vector>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

using namespace Caesura;

TEST_CASE("U11 font restore: CPU preparation retains a real font independently of the renderer") {
    std::ifstream input("assets/fonts/NotoSansCJKsc-Regular.otf",std::ios::binary);
    REQUIRE(input.good());
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),{}};
    TextRenderer renderer;
    CHECK_FALSE(renderer.captureFontState().active);
    auto prepared=TextRenderer::prepareFontState({true,FontId::TTF,"assets/fonts/NotoSansCJKsc-Regular.otf",24},bytes.data(),bytes.size());
    REQUIRE(prepared != nullptr);
    bytes.clear(); bytes.shrink_to_fit();
    CHECK(prepared->description().active);
    CHECK(prepared->description().font==FontId::TTF);
    CHECK(prepared->description().pixelSize==24);
    CHECK_FALSE(renderer.captureFontState().active);
    CHECK_FALSE(renderer.applyFontState(std::move(prepared))); // no GPU
    CHECK_FALSE(renderer.captureFontState().active);
}

TEST_CASE("U11 font restore: invalid font input cannot become a preparation") {
    const uint8_t invalid[]={1,2,3,4};
    CHECK_FALSE(TextRenderer::prepareFontState({true,FontId::TTF,"assets/a.ttf",24},invalid,sizeof(invalid)));
    CHECK_FALSE(TextRenderer::prepareFontState({true,FontId::TTF,"assets/a.ttf",24},nullptr,0));
    for (const float size : {0.0f,0.5f,257.0f,std::numeric_limits<float>::infinity()}) {
        CHECK_FALSE(TextRenderer::prepareFontState({true,FontId::TTF,"assets/a.ttf",size},invalid,sizeof(invalid)));
    }
    CHECK_FALSE(TextRenderer::prepareFontState({true,static_cast<FontId>(7),"",16},nullptr,0));
    CHECK_FALSE(TextRenderer::prepareFontState({true,FontId::Small,"assets/a.ttf",16},nullptr,0));
}

TEST_CASE("U11 font restore: bitmap and inactive selections have distinct preparations") {
    auto small=TextRenderer::prepareFontState({true,FontId::Small,"",16},nullptr,0);
    auto large=TextRenderer::prepareFontState({true,FontId::Large,"",32},nullptr,0);
    REQUIRE(small != nullptr);
    REQUIRE(large != nullptr);
    CHECK(small->description().font==FontId::Small);
    CHECK(large->description().font==FontId::Large);
    NullRenderDevice null;
    CHECK_FALSE(null.defaultFontState().active);
    auto inactive=null.prepareFontState({},nullptr,0);
    REQUIRE(inactive != nullptr);
    CHECK(null.applyFontState(std::move(inactive)));
    CHECK_FALSE(null.prepareFontState({true,FontId::Small,"",16},nullptr,0));
    null.clearFontState();
    CHECK_FALSE(null.captureFontState().active);
}

#if defined(_WIN32)
namespace {
class FontAssetScope final : public IAssetReader {
public:
    Caesura::AssetDirectoryResult listDirectory(const std::string&, size_t, size_t) override { return {}; }
    explicit FontAssetScope(IRenderDevice* device)
        : oldReader(BackendRegistry::instance().getAssetReader()), oldDevice(BackendRegistry::instance().getRenderDevice()) {
        std::ifstream input("assets/fonts/NotoSansCJKsc-Regular.otf",std::ios::binary);
        REQUIRE(input.good());
        bytes.assign(std::istreambuf_iterator<char>(input),{});
        BackendRegistry::instance().setAssetReader(this);
        BackendRegistry::instance().setRenderDevice(device);
    }
    ~FontAssetScope() override {
        BackendRegistry::instance().setAssetReader(oldReader);
        BackendRegistry::instance().setRenderDevice(oldDevice);
    }
    std::vector<uint8_t> readAsset(const std::string& path,size_t limit) override {
        ++reads;
        return path=="assets/prepared-only.otf" && bytes.size()<=limit ? bytes : std::vector<uint8_t>{};
    }
    std::vector<uint8_t> bytes;
    int reads=0;
    IAssetReader* oldReader;
    IRenderDevice* oldDevice;
};

std::vector<uint8_t> readFontTexture(bgfx::TextureHandle source, uint16_t width, uint16_t height) {
    const auto target=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::RGBA8,
        BGFX_TEXTURE_BLIT_DST|BGFX_TEXTURE_READ_BACK);
    REQUIRE(bgfx::isValid(target));
    std::vector<uint8_t> pixels(size_t(width)*height*4);
    bgfx::blit(250,target,0,0,source);
    uint32_t frame=bgfx::frame();
    const uint32_t ready=bgfx::readTexture(target,pixels.data());
    while (frame<ready) frame=bgfx::frame();
    bgfx::destroy(target);
    return pixels;
}
}

TEST_CASE("U11 font restore: GPU applies snapshots and preserves the actual font through recovery") {
    constexpr wchar_t env[]=L"CAESURA_FONT_RESTORE_CHILD";
    constexpr wchar_t name[]=L"U11 font restore: GPU applies snapshots and preserves the actual font through recovery";
    if (!CaesuraTest::isGpuChildProcess(env)) {
        CHECK(CaesuraTest::runGpuChildProcess(env,name)==ERROR_SUCCESS);
        return;
    }
    CaesuraTest::HiddenSdlWindow window(128,64);
    REQUIRE(window);
    BgfxRenderDevice device;
    REQUIRE(device.setPreferredBackend("dx11"));
    REQUIRE(device.init(window.nativeHandle(),128,64));
    {
        TextRenderer text;
        REQUIRE(text.init(&device));
        const auto small=readFontTexture(text.fontTexture(),256,48);
        text.setFont(FontId::Large);
        REQUIRE(text.captureFontState().font==FontId::Large);
        const auto large=readFontTexture(text.fontTexture(),512,96);
        bool scaled=true;
        for (size_t y=0;y<96;++y) for (size_t x=0;x<512;++x)
            scaled=scaled && large[(y*512+x)*4+3]==small[((y/2)*256+x/2)*4+3];
        CHECK(scaled);
        text.onDeviceLost();
        text.onDeviceRestored();
        CHECK(text.captureFontState().font==FontId::Large);
        CHECK(bgfx::isValid(text.fontTexture()));
        std::ifstream input("assets/fonts/NotoSansCJKsc-Regular.otf",std::ios::binary);
        REQUIRE(input.good());
        std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),{}};
        auto prepared=TextRenderer::prepareFontState({true,FontId::TTF,"assets/prepared-only.otf",28},bytes.data(),bytes.size());
        REQUIRE(prepared != nullptr);
        bytes.clear(); bytes.shrink_to_fit();
        REQUIRE(text.applyFontState(std::move(prepared)));
        const float height=text.lineHeight();
        text.onDeviceLost();
        text.onDeviceRestored();
        CHECK(text.captureFontState().font==FontId::TTF);
        CHECK(text.captureFontState().assetPath=="assets/prepared-only.otf");
        CHECK(text.lineHeight()==height);
        CHECK(bgfx::isValid(text.fontTexture()));
        text.clearFontState();
        CHECK_FALSE(text.captureFontState().active);
        CHECK_FALSE(bgfx::isValid(text.fontTexture()));
        text.renderText(10,"cleared",0,0,TextColor::White());
        text.renderRuby(10,"base","ruby",0,0,TextColor::White());
        text.onDeviceLost(); text.onDeviceRestored();
        CHECK_FALSE(text.captureFontState().active);
        text.shutdown();
    }
    device.setFont(1);
    REQUIRE(device.recoverDevice(window.nativeHandle(),128,64));
    CHECK(device.captureFontState().font==FontId::Large);
    std::ifstream input("assets/fonts/NotoSansCJKsc-Regular.otf",std::ios::binary);
    REQUIRE(input.good());
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),{}};
    auto prepared=device.prepareFontState({true,FontId::TTF,"assets/prepared-only.otf",24},bytes.data(),bytes.size());
    REQUIRE(prepared != nullptr);
    REQUIRE(device.applyFontState(std::move(prepared)));
    REQUIRE(device.recoverDevice(window.nativeHandle(),128,64));
    CHECK(device.captureFontState().font==FontId::TTF);
    CHECK(device.captureFontState().assetPath=="assets/prepared-only.otf");
    device.clearFontState();
    REQUIRE(device.recoverDevice(window.nativeHandle(),128,64));
    CHECK_FALSE(device.captureFontState().active);
    {
        FontAssetScope assets(&device);
        LuaManager vm;
        REQUIRE(vm.init());
        const auto run=[&](const char* code) {
            vm.resetInstructionBudget();
            const int result=luaL_dostring(vm.state(),code);
            const std::string error=result==LUA_OK ? "" : lua_tostring(vm.state(),-1);
            lua_settop(vm.state(),0);
            REQUIRE_MESSAGE(result==LUA_OK,error);
        };
        run(R"lua(
            assert(Render.text_set_font('assets/prepared-only.otf',24.75))
            assert(Restore.capture_font().size==24)
            KAG.set_font(1)
            local actual=Restore.capture_font()
            assert(actual.font==1 and actual.path=='' and actual.size==32)
            font_ticket=assert(Restore.prepare_font({version=1,active=true,
                font=2,path='assets/prepared-only.otf',size=28}))
            assert(Restore.capture_font().font==1)
        )lua");
        CHECK(assets.reads==2);
        assets.bytes.clear(); assets.bytes.shrink_to_fit();
        run(R"lua(
            assert(Restore.apply_font(font_ticket))
            assert(Restore.capture_font().font==2 and Restore.capture_font().size==28)
            Restore.discard_font(font_ticket); Restore.discard_font(font_ticket)
            assert(Restore.apply_font(font_ticket)==false)
            assert(Restore.capture_font().font==2)
            assert(Restore.clear_font())
            assert(Restore.capture_font().active==false)
            assert(Restore.prepare_font({version=1,active=true,font=2,path='assets/a\0b.ttf',size=24})==nil)
        )lua");
        CHECK(assets.reads==2);
    }
    device.shutdown();
}
#endif
