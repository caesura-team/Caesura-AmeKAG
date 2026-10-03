#include "doctest.h"
#include "di/BackendRegistry.h"
#include "render/api/IMeshRenderer.h"
#include "render/api/ITextureManager.h"
#include "script/bindings/SmaBinding.h"
#include <string>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#if defined(_WIN32)
#include "HiddenGpuContext.h"
#include "render/BgfxRenderDevice.h"
#include "render/TextureManager.h"
#include "script/bindings/RenderBinding.h"
#include <array>
#endif

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

namespace {
// Only the hardware boundary is replaced. The registered Lua C functions and
// table conversion are the production implementation, without a Lua proxy.
class MeshBoundaryRecorder final : public Caesura::IMeshRenderer {
public:
    bool isInitialized() const override { return true; }
    void setSkinMode(Caesura::SkinMode value) override { mode = value; }
    Caesura::SkinMode skinMode() const override { return mode; }
    Caesura::MeshHandle createMesh(const Caesura::SMAMesh& value) override {
        mesh = value; ++creates; return Caesura::MeshHandle{41};
    }
    void destroyMesh(Caesura::MeshHandle) override {}
    void updateMesh(Caesura::MeshHandle value,
                    const std::vector<Caesura::BonePose>& valuePoses) override {
        handle = value; poses = valuePoses; ++updates;
    }
    void drawMesh(uint16_t view, Caesura::MeshHandle meshHandle, uint32_t texture,
                  float x, float y, float scale, float opacity) override {
        ++draws;drawView=view;drawHandle=meshHandle;drawTexture=texture;
        drawX=x;drawY=y;drawScale=scale;drawOpacity=opacity;
        if(throwDraw)throw std::runtime_error("SMA GPU bone snapshot allocation failed");
    }
    size_t meshCount() const override { return creates; }
    Caesura::SkinMode mode = Caesura::SkinMode::Auto;
    Caesura::SMAMesh mesh;
    Caesura::MeshHandle handle;
    std::vector<Caesura::BonePose> poses;
    unsigned creates = 0, updates = 0, draws = 0;
    uint16_t drawView=0;Caesura::MeshHandle drawHandle{};uint32_t drawTexture=0;
    float drawX=0,drawY=0,drawScale=0,drawOpacity=0;
    bool throwDraw=false;
};

struct SmaNativeFixture {
    MeshBoundaryRecorder mesh;
    Caesura::IMeshRenderer* previous =
        Caesura::BackendRegistry::instance().getMeshRenderer();
    lua_State* state = luaL_newstate();
    SmaNativeFixture() {
        Caesura::BackendRegistry::instance().setMeshRenderer(&mesh);
        if (state) Caesura::registerSmaBinding(state);
    }
    ~SmaNativeFixture() {
        if (state) lua_close(state);
        Caesura::BackendRegistry::instance().setMeshRenderer(previous);
    }
    int call(const char* source, int returns = 0) {
        const int loaded = luaL_loadstring(state, source);
        return loaded == LUA_OK ? lua_pcall(state, 0, returns, 0) : loaded;
    }
    std::string errorText(int result) const {
        if (result == LUA_OK) return {};
        const char* message = lua_tostring(state, -1);
        return message ? std::string(message) : std::string("non-string Lua error");
    }
};
}

TEST_CASE("SMA native contract: named vertex fields reach the renderer") {
    SmaNativeFixture f;
    REQUIRE(f.state != nullptr);
    const int result = f.call(R"(
        return sma.create_mesh({
            {x=0.25,y=-0.5,u=0.125,v=0.75,bone0=2,w0=0.6,bone1=3,w1=0.4},
            {x=1,y=0,u=1,v=0,bone0=0,w0=1},
            {x=0,y=1,u=0,v=1,bone0=0,w0=1}
        }, {0,1,2})
    )", 1);
    const auto message = f.errorText(result);
    REQUIRE_MESSAGE(result == LUA_OK, message);
    CHECK(lua_tointeger(f.state, -1) == 41);
    REQUIRE(f.mesh.creates == 1);
    REQUIRE(f.mesh.mesh.vertices.size() == 3);
    const auto& v = f.mesh.mesh.vertices.front();
    CHECK(v.x == doctest::Approx(0.25));
    CHECK(v.y == doctest::Approx(-0.5));
    CHECK(v.u == doctest::Approx(0.125));
    CHECK(v.v == doctest::Approx(0.75));
    CHECK(v.bone0 == 2);
    CHECK(v.w0 == doctest::Approx(0.6));
    CHECK(v.bone1 == 3);
    CHECK(v.w1 == doctest::Approx(0.4));
    CHECK(f.mesh.mesh.vertices[1].bone1 == UINT16_MAX);
    CHECK(f.mesh.mesh.vertices[1].w1 == 0);
    REQUIRE(f.mesh.mesh.indices.size() == 3);
    CHECK(f.mesh.mesh.indices[0] == 0);
    CHECK(f.mesh.mesh.indices[1] == 1);
    CHECK(f.mesh.mesh.indices[2] == 2);
}

TEST_CASE("SMA native contract: named pose fields and defaults reach the renderer") {
    SmaNativeFixture f;
    REQUIRE(f.state != nullptr);
    const int result = f.call(
        "sma.update_mesh(41, {{rot=0.75,scale=1.5,ox=-0.25,oy=0.5},{}})");
    const auto message = f.errorText(result);
    REQUIRE_MESSAGE(result == LUA_OK, message);
    CHECK(f.mesh.updates == 1);
    CHECK(f.mesh.handle.id == 41);
    REQUIRE(f.mesh.poses.size() == 2);
    CHECK(f.mesh.poses[0].rot == doctest::Approx(0.75));
    CHECK(f.mesh.poses[0].scale == doctest::Approx(1.5));
    CHECK(f.mesh.poses[0].ox == doctest::Approx(-0.25));
    CHECK(f.mesh.poses[0].oy == doctest::Approx(0.5));
    CHECK(f.mesh.poses[1].rot == 0);
    CHECK(f.mesh.poses[1].scale == 1);
    CHECK(f.mesh.poses[1].ox == 0);
    CHECK(f.mesh.poses[1].oy == 0);
}

TEST_CASE("SMA native contract: named fields precede raw positional fallback") {
    SmaNativeFixture f;
    REQUIRE(f.state != nullptr);
    luaL_openlibs(f.state);
    const int result = f.call(R"lua(
        local trap={__index=function() error('record metamethod must not execute') end}
        local vertex=setmetatable({99,9,0.25,0.75,'bad',0.6,3,0.4,
            x=0,u=0.125,bone0=2},trap)
        assert(sma.create_mesh({vertex},{0,0,0})==41)
        sma.update_mesh(41,{setmetatable({9,'bad',-2,5,rot=0,scale=1.5,oy=0},trap),
            setmetatable({},trap)})
    )lua");
    REQUIRE_MESSAGE(result == LUA_OK, f.errorText(result));
    REQUIRE(f.mesh.creates == 1);
    REQUIRE(f.mesh.mesh.vertices.size() == 1);
    const auto& vertex = f.mesh.mesh.vertices[0];
    CHECK(vertex.x == 0);
    CHECK(vertex.y == 9);
    CHECK(vertex.u == doctest::Approx(0.125));
    CHECK(vertex.v == doctest::Approx(0.75));
    CHECK(vertex.bone0 == 2);
    CHECK(vertex.bone1 == 3);
    CHECK(vertex.w0 == doctest::Approx(0.6));
    CHECK(vertex.w1 == doctest::Approx(0.4));
    REQUIRE(f.mesh.updates == 1);
    REQUIRE(f.mesh.poses.size() == 2);
    CHECK(f.mesh.poses[0].rot == 0);
    CHECK(f.mesh.poses[0].scale == doctest::Approx(1.5));
    CHECK(f.mesh.poses[0].ox == -2);
    CHECK(f.mesh.poses[0].oy == 0);
    CHECK(f.mesh.poses[1].rot == 0);
    CHECK(f.mesh.poses[1].scale == 1);
    CHECK(f.mesh.poses[1].ox == 0);
    CHECK(f.mesh.poses[1].oy == 0);
    const char* invalidNamedMeshes[] = {
        "return sma.create_mesh({{1,2,x='bad'}},{0,0,0})",
        "return sma.create_mesh({{1,2,x=false}},{0,0,0})",
        "return sma.create_mesh({{1,2,x=0/0}},{0,0,0})",
        "return sma.create_mesh({{1,2,0,0,0,1,bone0=-1}},{0,0,0})",
        "return sma.create_mesh({{1,2,0,0,0,1,1,0,bone1=65536}},{0,0,0})"
    };
    for (const auto* script : invalidNamedMeshes) {
        CAPTURE(script);
        lua_settop(f.state, 0);
        const int rejected = f.call(script, 1);
        REQUIRE_MESSAGE(rejected == LUA_OK, f.errorText(rejected));
        CHECK(lua_tointeger(f.state, -1) == 0);
        CHECK(f.mesh.creates == 1);
    }
    const char* invalidNamedPoses[] = {
        "sma.update_mesh(41,{{0,1,0,0,rot='bad'}})",
        "sma.update_mesh(41,{{0,1,0,0,scale=1/0}})",
        "sma.update_mesh(41,{{0,1,0,0,oy=false}})"
    };
    for (const auto* script : invalidNamedPoses) {
        CAPTURE(script);
        lua_settop(f.state, 0);
        const int rejected = f.call(script);
        REQUIRE_MESSAGE(rejected == LUA_OK, f.errorText(rejected));
        CHECK(f.mesh.updates == 1);
    }
}

TEST_CASE("SMA native contract: malformed geometry never reaches the renderer") {
    SmaNativeFixture f;
    REQUIRE(f.state != nullptr);
    CHECK(f.call("return sma.create_mesh(nil, {})", 1) == LUA_OK);
    CHECK(lua_tointeger(f.state, -1) == 0);
    lua_settop(f.state, 0);
    CHECK(f.call("return sma.create_mesh({false}, {0,0,0})", 1) == LUA_OK);
    CHECK(lua_tointeger(f.state, -1) == 0);
    CHECK(f.mesh.creates == 0);
    lua_settop(f.state, 0);
    CHECK(f.call("sma.update_mesh(41, nil)") == LUA_OK);
    CHECK(f.mesh.updates == 0);
}

TEST_CASE("SMA native contract: out of range indices reject a named mesh") {
    SmaNativeFixture f;
    REQUIRE(f.state != nullptr);
    const int result = f.call("return sma.create_mesh({{x=0,y=0}}, {0,0,1})", 1);
    const auto message = f.errorText(result);
    REQUIRE_MESSAGE(result == LUA_OK, message);
    CHECK(lua_tointeger(f.state, -1) == 0);
    CHECK(f.mesh.creates == 0);
}

TEST_CASE("SMA native contract: malformed fields and indices have no backend effects") {
    SmaNativeFixture f;
    REQUIRE(f.state != nullptr);
    const char* rejectedMeshes[] = {
        "return sma.create_mesh({{x='bad'}}, {0,0,0})",
        "return sma.create_mesh({{x=0/0}}, {0,0,0})",
        "return sma.create_mesh({{x=1/0}}, {0,0,0})",
        "return sma.create_mesh({{bone0=-1}}, {0,0,0})",
        "return sma.create_mesh({{bone1=65536}}, {0,0,0})",
        "return sma.create_mesh({{}}, {0,0,0.5})",
        "return sma.create_mesh({{}}, {0,0,'0'})",
        "return sma.create_mesh({{}}, {0,0,false})"
    };
    for (const char* script : rejectedMeshes) {
        CAPTURE(script);
        lua_settop(f.state, 0);
        const int result = f.call(script, 1);
        const auto message = f.errorText(result);
        REQUIRE_MESSAGE(result == LUA_OK, message);
        CHECK(lua_tointeger(f.state, -1) == 0);
        CHECK(f.mesh.creates == 0);
    }
    const char* rejectedPoses[] = {
        "sma.update_mesh(41, {{rot='bad'}})",
        "sma.update_mesh(41, {{rot=0/0}})",
        "sma.update_mesh(41, {{scale=1/0}})",
        "sma.update_mesh(41, {false})"
    };
    for (const char* script : rejectedPoses) {
        CAPTURE(script);
        lua_settop(f.state, 0);
        const int result = f.call(script);
        const auto message = f.errorText(result);
        REQUIRE_MESSAGE(result == LUA_OK, message);
        CHECK(f.mesh.updates == 0);
    }
}

TEST_CASE("SMA texture contract: zero part texture inherits the actor texture") {
    SmaNativeFixture f;REQUIRE(f.state);luaL_openlibs(f.state);
    const auto path=std::filesystem::path(CAESURA_SOURCE_DIR).generic_string()+"/scripts/?.lua;";
    lua_pushlstring(f.state,path.data(),path.size());lua_setglobal(f.state,"SMA_MODULE_PATH");
    std::ifstream file(std::filesystem::path(CAESURA_SOURCE_DIR)/"demo/assets/sma/hero.json",std::ios::binary);
    REQUIRE(file.good());
    const std::string bytes((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
    REQUIRE_FALSE(bytes.empty());lua_pushlstring(f.state,bytes.data(),bytes.size());lua_setglobal(f.state,"HERO_BYTES");
    const int result=f.call(R"(
        package.path=SMA_MODULE_PATH..package.path
        local module=require('kag.sma')
        local asset=assert(module.load(HERO_BYTES,{validate=true}))
        assert(#asset.parts==5)
        for _,part in ipairs(asset.parts) do assert(part.tex==0,'fixture must exercise explicit zero') end
        local ctx={};local actor=module.spawn(ctx,'hero',asset,'idle',{texId=77})
        assert(actor and #actor.parts==5)
        for _,part in ipairs(actor.parts) do assert(part.texId==77,'zero texture must mean inherited actor texture') end
        module.despawn(ctx,'hero')
        asset.parts[1].tex=91
        actor=module.spawn(ctx,'hero',asset,'idle',{texId=77})
        assert(actor.parts[1].texId==91,'explicit positive part override must remain')
        for i=2,#actor.parts do assert(actor.parts[i].texId==77) end
        module.despawn(ctx,'hero')
    )");
    REQUIRE_MESSAGE(result==LUA_OK,f.errorText(result));
    CHECK(f.mesh.creates==10);
}

namespace {
// Logical texture ownership boundary only; never initializes/allocates a GPU.
class SmaExceptionTextures final : public Caesura::ITextureManager {
public:
    bool initialize() override{return true;}
    bool initialize(bool) override{return true;}
    void shutdown() override{}
    void setDevMode(bool) override{}
    uint32_t loadTexture(const std::string&) override{return 0;}
    uint32_t loadTextureFromMemory(const uint8_t*,uint32_t,const std::string&) override{return 0;}
    uint32_t loadTextureFromRGBA(const uint8_t*,uint16_t,uint16_t,const std::string&) override{return 0;}
    uint32_t createSolidTexture(uint8_t,uint8_t,uint8_t,uint8_t) override{return 77;}
    uint32_t getPlaceholderTexture() override{return 0;}
    void destroyTexture(uint32_t) override{}
    uint32_t getTextureHandle(uint32_t id) const override{return id==77?23:0;}
    void getTextureSizeById(uint32_t,uint16_t& w,uint16_t& h) const override{w=h=1;}
    bool isValid(uint32_t id) const override{return id==77;}
    bool describeTexture(uint32_t,Caesura::TextureSourceInfo&) const override{return false;}
    uint64_t totalTextureBytes() const override{return 0;}
    bool checkBudget(uint32_t,uint16_t,uint16_t) override{return true;}
    void trackTexture(uint32_t,uint64_t) override{}
    void untrackTexture(uint32_t) override{}
};
}
TEST_CASE("SMA native contract: real module positional geometry and animated poses reach native boundaries") {
    // Exercise the shipped producer, not a Lua replacement for sma.create_mesh.
    // Only mesh hardware and logical texture ownership are recorded here.
    SmaExceptionTextures textures;
    auto& registry = Caesura::BackendRegistry::instance();
    struct RestoreTextures {
        Caesura::BackendRegistry& registry;
        Caesura::ITextureManager* previous;
        ~RestoreTextures() { registry.setTextureManager(previous); }
    } restore{registry, registry.getTextureManager()};
    registry.setTextureManager(&textures);
    SmaNativeFixture f;
    REQUIRE(f.state != nullptr);
    luaL_openlibs(f.state);
    const auto path = std::filesystem::path(CAESURA_SOURCE_DIR).generic_string() + "/scripts/?.lua;";
    lua_pushlstring(f.state, path.data(), path.size());
    lua_setglobal(f.state, "SMA_MODULE_PATH");
    const int result = f.call(R"lua(
        package.path=SMA_MODULE_PATH..package.path
        for _,name in ipairs({'create_mesh','update_mesh','draw_mesh'}) do
            assert(debug.getinfo(sma[name],'S').what=='C', 'real native binding required')
        end
        local model=require('kag.sma')
        local asset=model.load([=[{
            "texture":"owned-logical-texture",
            "bones":[{"id":0,"parent":-1,"pivot":[0,0]},
                     {"id":1,"parent":-1,"pivot":[0,0]}],
            "mesh":{
                "positions":[[2,-3],[22,-3],[2,17]],
                "uvs":[[0.125,0.25],[0.875,0.25],[0.125,0.75]],
                "indices":[0,1,2],
                "weights":[{"bone":0,"w":0.6},{"bone":1,"w":0.4},
                           {"bone":0,"w":0.6},{"bone":1,"w":0.4},
                           {"bone":0,"w":0.6},{"bone":1,"w":0.4}]
            },
            "animations":{"move":{"tracks":[
                {"bone":0,"frames":[
                    {"t":0,"rot":0.2,"scale":1,"offset":[1,-2]},
                    {"t":1,"rot":0.6,"scale":1.5,"offset":[5,-6]}]},
                {"bone":1,"frames":[
                    {"t":0,"rot":-0.3,"scale":0.75,"offset":[-5,6]}]}
            ]}}
        }]=], {validate=true})
        local ctx={}
        local actor=model.spawn(ctx,'bridge',asset,'move',
            {view=1,texId=77,x=100,y=120,scale=2,opacity=0.5,loop=false})
        assert(actor and actor.handle==41)
        model.update(ctx,0.5)
        model.render(ctx)
        model.despawn(ctx,'bridge')
    )lua");
    REQUIRE_MESSAGE(result == LUA_OK, f.errorText(result));
    REQUIRE(f.mesh.creates == 1);
    REQUIRE(f.mesh.mesh.vertices.size() == 3);
    const float expected[][4] = {{2,-3,0.125f,0.25f}, {22,-3,0.875f,0.25f}, {2,17,0.125f,0.75f}};
    for (size_t i = 0; i < 3; ++i) {
        CAPTURE(i);
        const auto& v = f.mesh.mesh.vertices[i];
        CHECK(v.x == doctest::Approx(expected[i][0]));
        CHECK(v.y == doctest::Approx(expected[i][1]));
        CHECK(v.u == doctest::Approx(expected[i][2]));
        CHECK(v.v == doctest::Approx(expected[i][3]));
        CHECK(v.bone0 == 0);
        CHECK(v.w0 == doctest::Approx(0.6));
        CHECK(v.bone1 == 1);
        CHECK(v.w1 == doctest::Approx(0.4));
    }
    CHECK(f.mesh.mesh.indices == std::vector<uint16_t>{0,1,2});
    REQUIRE(f.mesh.updates == 1);
    CHECK(f.mesh.handle.id == 41);
    REQUIRE(f.mesh.poses.size() == 2);
    CHECK(f.mesh.poses[0].rot == doctest::Approx(0.4));
    CHECK(f.mesh.poses[0].scale == doctest::Approx(1.25));
    CHECK(f.mesh.poses[0].ox == doctest::Approx(3));
    CHECK(f.mesh.poses[0].oy == doctest::Approx(-4));
    CHECK(f.mesh.poses[1].rot == doctest::Approx(-0.3));
    CHECK(f.mesh.poses[1].scale == doctest::Approx(0.75));
    CHECK(f.mesh.poses[1].ox == doctest::Approx(-5));
    CHECK(f.mesh.poses[1].oy == doctest::Approx(6));
    REQUIRE(f.mesh.draws == 1);
    CHECK(f.mesh.drawHandle.id == 41);
    CHECK(f.mesh.drawTexture == 23); // Real C binding resolves logical ID 77.
    CHECK(f.mesh.drawView == 1); // Explicit MAIN isolates geometry from default-view behavior.
    CHECK(f.mesh.drawX == 100);
    CHECK(f.mesh.drawY == 120);
    CHECK(f.mesh.drawScale == 2);
    CHECK(f.mesh.drawOpacity == doctest::Approx(0.5)); // Argument transport, not GPU alpha proof.
}

TEST_CASE("SMA native contract: module and schema command default to MAIN and retain explicit views") {
    SmaExceptionTextures textures;
    auto& registry = Caesura::BackendRegistry::instance();
    struct RestoreTextures {
        Caesura::BackendRegistry& registry;
        Caesura::ITextureManager* previous;
        ~RestoreTextures() { registry.setTextureManager(previous); }
    } restore{registry, registry.getTextureManager()};
    registry.setTextureManager(&textures);
    struct Case { bool command; int suppliedView; uint16_t expectedView; };
    const Case cases[] = {{false,-1,1}, {false,0,0}, {false,7,7},
                          {true,-1,1}, {true,0,0}, {true,7,7}};
    for (const auto& item : cases) {
        CAPTURE(item.command);
        CAPTURE(item.suppliedView);
        SmaNativeFixture f;
        REQUIRE(f.state != nullptr);
        luaL_openlibs(f.state);
        const auto path = std::filesystem::path(CAESURA_SOURCE_DIR).generic_string() + "/scripts/?.lua;";
        lua_pushlstring(f.state, path.data(), path.size());
        lua_setglobal(f.state, "SMA_MODULE_PATH");
        lua_pushboolean(f.state, item.command);
        lua_setglobal(f.state, "SMA_USE_COMMAND");
        if (item.suppliedView < 0) lua_pushnil(f.state);
        else lua_pushinteger(f.state, item.suppliedView);
        lua_setglobal(f.state, "SMA_VIEW_OVERRIDE");
        const int result = f.call(R"lua(
            package.path=SMA_MODULE_PATH..package.path
            local model=require('kag.sma')
            local asset=model.load([=[{
                "bones":[{"id":0,"parent":-1,"pivot":[0,0]}],
                "mesh":{"positions":[[0,0],[20,0],[0,20]],"indices":[0,1,2]},
                "animations":{"idle":{"tracks":[{"bone":0,"frames":[{"t":0}]}]}}
            }]=],{validate=true})
            local ctx={}
            if SMA_USE_COMMAND then
                require('kag.commands.system') -- Install the real sma_play schema.
                model.register('view-fixture',asset)
                local params=require('kag.schema').coerce('sma_play',
                    {name='actor',asset='view-fixture',tex=77,view=SMA_VIEW_OVERRIDE},ctx)
                model.commands.sma_play(ctx,params)
            else
                model.spawn(ctx,'actor',asset,'idle',{texId=77,view=SMA_VIEW_OVERRIDE})
            end
            model.update(ctx,0)
            model.render(ctx)
            model.despawn(ctx,'actor')
        )lua");
        REQUIRE_MESSAGE(result == LUA_OK, f.errorText(result));
        REQUIRE(f.mesh.creates == 1);
        REQUIRE(f.mesh.draws == 1);
        CHECK(f.mesh.drawView == item.expectedView);
        CHECK(f.mesh.drawTexture == 23);
        CHECK(f.mesh.drawOpacity == 1);
    }
}

TEST_CASE("SMA native contract: positional fields retain numeric and index rejection") {
    SmaNativeFixture f;
    REQUIRE(f.state != nullptr);
    const char* rejectedMeshes[] = {
        "return sma.create_mesh({{'bad',0}}, {0,0,0})",
        "return sma.create_mesh({{0/0,0}}, {0,0,0})",
        "return sma.create_mesh({{0,1/0}}, {0,0,0})",
        "return sma.create_mesh({{0,0,1e100,0}}, {0,0,0})",
        "return sma.create_mesh({{0,0,0,false}}, {0,0,0})",
        "return sma.create_mesh({{0,0,0,0,-1,1}}, {0,0,0})",
        "return sma.create_mesh({{0,0,0,0,0.5,1}}, {0,0,0})",
        "return sma.create_mesh({{0,0,0,0,0,1,65536,0}}, {0,0,0})",
        "return sma.create_mesh({{0,0,0,0,0,0/0}}, {0,0,0})",
        "return sma.create_mesh({{0,0,0,0,0,1,1,1/0}}, {0,0,0})",
        "return sma.create_mesh({{2,3}}, {0,0,1})",
        "return sma.create_mesh({{2,3}}, {0,0,65536})",
        "return sma.create_mesh({{2,3}}, {0,0,0.5})",
        "return sma.create_mesh({{2,3}}, {0,0,'0'})"
    };
    for (const auto* script : rejectedMeshes) {
        CAPTURE(script);
        lua_settop(f.state, 0);
        const auto before = f.mesh.creates;
        const int result = f.call(script, 1);
        REQUIRE_MESSAGE(result == LUA_OK, f.errorText(result));
        CHECK(lua_tointeger(f.state, -1) == 0);
        CHECK(f.mesh.creates == before);
    }
    const char* rejectedPoses[] = {
        "sma.update_mesh(41, {{'bad',1,0,0}})",
        "sma.update_mesh(41, {{0/0,1,0,0}})",
        "sma.update_mesh(41, {{0,1/0,0,0}})",
        "sma.update_mesh(41, {{0,1,1e100,0}})",
        "sma.update_mesh(41, {{0,1,0,false}})"
    };
    for (const auto* script : rejectedPoses) {
        CAPTURE(script);
        lua_settop(f.state, 0);
        const auto before = f.mesh.updates;
        const int result = f.call(script);
        REQUIRE_MESSAGE(result == LUA_OK, f.errorText(result));
        CHECK(f.mesh.updates == before);
    }
}

TEST_CASE("SMA native contract: renderer draw exceptions remain Lua errors") {
    SmaExceptionTextures textures;
    auto& registry=Caesura::BackendRegistry::instance();
    struct RestoreTextures {
        Caesura::BackendRegistry& registry;Caesura::ITextureManager* previous;
        ~RestoreTextures(){registry.setTextureManager(previous);}
    } restore{registry,registry.getTextureManager()};
    registry.setTextureManager(&textures);
    SmaNativeFixture f;REQUIRE(f.state);
    // Positive call proves the real C function reaches the hardware boundary.
    REQUIRE(f.call("sma.draw_mesh(41,1,77,12,18,1,1)")==LUA_OK);
    REQUIRE(f.mesh.draws==1);REQUIRE(f.mesh.drawTexture==23);
    f.mesh.throwDraw=true;
    bool escaped=false;std::string escapedMessage;int status=-1;
    try {status=f.call("sma.draw_mesh(41,1,77,12,18,1,1)");}
    catch(const std::exception& error){escaped=true;escapedMessage=error.what();}
    catch(...){escaped=true;escapedMessage="non-standard C++ exception";}
    CAPTURE(escapedMessage);
    CHECK_FALSE_MESSAGE(escaped,"C++ renderer exception escaped actual lua_pcall");
    CHECK(f.mesh.draws==2);
    // An escaped exception may leave Lua's protection chain unwound; do not
    // resume that failed old state. The fixture closes it and root owns process
    // retirement. On the corrected path, assert real Lua error and recovery.
    if(!escaped) {
        CHECK(status==LUA_ERRRUN);
        CHECK_FALSE(f.errorText(status).empty());
        lua_settop(f.state,0);f.mesh.throwDraw=false;
        const int after=f.call("sma.draw_mesh(41,1,77,12,18,1,1)");
        CHECK_MESSAGE(after==LUA_OK,f.errorText(after));
        CHECK(f.mesh.draws==3);
    }
}
#if defined(_WIN32)
TEST_CASE("SMA texture contract: real TextureManager logical ID resolves before mesh draw") {
    constexpr wchar_t childEnv[]=L"CAESURA_SMA_TEXTURE_NAMESPACE_CHILD";
    constexpr wchar_t testName[]=L"SMA texture contract: real TextureManager logical ID resolves before mesh draw";
    if(!CaesuraTest::isGpuChildProcess(childEnv)) {
        CHECK(CaesuraTest::runGpuChildProcess(childEnv,testName)==ERROR_SUCCESS);return;
    }
    CaesuraTest::HiddenSdlWindow window(64,64);REQUIRE(window);
    Caesura::BgfxRenderDevice device;REQUIRE(device.setPreferredBackend("dx11"));REQUIRE(device.init(window.nativeHandle(),64,64));
    {
        // Hold distinct real GPU allocations outside TextureManager's logical
        // numbering. The checked inequality is a fixture prerequisite, never
        // a manufactured raw handle or an assertion inferred from its size.
        struct RawTextures {
            std::array<bgfx::TextureHandle,8> values;
            RawTextures(){for(auto& value:values)value=BGFX_INVALID_HANDLE;}
            ~RawTextures(){for(auto value:values)if(bgfx::isValid(value))bgfx::destroy(value);}
        } reserved;
        const uint32_t pixel=0xff00ffff;
        for(auto& raw:reserved.values) {
            raw=bgfx::createTexture2D(1,1,false,1,bgfx::TextureFormat::RGBA8,0,bgfx::copy(&pixel,sizeof(pixel)));
            REQUIRE(bgfx::isValid(raw));
        }
        Caesura::TextureManager textures;REQUIRE(textures.initialize());
        auto& registry=Caesura::BackendRegistry::instance();
        auto* previous=registry.getTextureManager();registry.setTextureManager(&textures);
        struct RestoreTextures {
            Caesura::BackendRegistry& registry;Caesura::ITextureManager* prior;Caesura::TextureManager& textures;
            ~RestoreTextures(){registry.setTextureManager(prior);textures.shutdown();}
        } restore{registry,previous,textures};
        SmaNativeFixture f;REQUIRE(f.state);Caesura::registerRenderBinding(f.state);
        int result=f.call("return Render.create_solid_texture(255,136,204,255)",1);
        REQUIRE_MESSAGE(result==LUA_OK,f.errorText(result));
        const auto logical=static_cast<uint32_t>(lua_tointeger(f.state,-1));lua_pop(f.state,1);
        REQUIRE(logical>0);REQUIRE(textures.isValid(logical));
        const auto raw=textures.getTextureHandle(logical);
        CAPTURE(logical);CAPTURE(raw);REQUIRE(raw!=logical);REQUIRE(raw!=UINT16_MAX);
        result=f.call("return sma.create_mesh({{0,0,0,0},{1,0,1,0},{0,1,0,1}}, {0,1,2})",1);
        REQUIRE_MESSAGE(result==LUA_OK,f.errorText(result));REQUIRE(lua_tointeger(f.state,-1)==41);lua_pop(f.state,1);
        lua_pushinteger(f.state,logical);lua_setglobal(f.state,"SMA_LOGICAL_TEXTURE");
        result=f.call("sma.draw_mesh(41,1,SMA_LOGICAL_TEXTURE,12,18,0.75,0.5)");
        REQUIRE_MESSAGE(result==LUA_OK,f.errorText(result));
        CHECK(f.mesh.draws==1);CHECK(f.mesh.drawTexture==raw);
        CHECK(f.mesh.drawView==1);CHECK(f.mesh.drawHandle.id==41);
        CHECK(f.mesh.drawX==12);CHECK(f.mesh.drawY==18);CHECK(f.mesh.drawScale==doctest::Approx(0.75));
        CHECK(f.mesh.drawOpacity==doctest::Approx(0.5));
        // No ownership exists for logical zero, an unknown ID, or a destroyed
        // logical ID; they must not fall through to raw GPU slot numbers.
        const auto draws=f.mesh.draws;
        REQUIRE(f.call("sma.draw_mesh(41,1,0,0,0,1,1);sma.draw_mesh(41,1,4294967295,0,0,1,1)")==LUA_OK);
        CHECK(f.mesh.draws==draws);
        textures.destroyTexture(logical);
        REQUIRE(f.call("sma.draw_mesh(41,1,SMA_LOGICAL_TEXTURE,0,0,1,1)")==LUA_OK);
        CHECK(f.mesh.draws==draws);
    }
    bgfx::frame();device.shutdown();
}
#endif
