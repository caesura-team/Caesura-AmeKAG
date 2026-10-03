#include "doctest.h"
#include "entry/Engine.h"
#include "entry/EngineConfig.h"
#include "di/BackendRegistry.h"
#include "resource/api/IAssetReader.h"
#include "render/TextureManager.h"
#include "script/api/ILuaManager.h"
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

using namespace Caesura;

namespace {
EngineConfig restoreConfig() {
    EngineConfig config;
    config.headless = true;
    return config;
}

class RestoreReader final : public IAssetReader {
public:
    Caesura::AssetDirectoryResult listDirectory(const std::string&, size_t, size_t) override { return {}; }
    std::vector<uint8_t> readAsset(const std::string& path, size_t limit) override {
        ++reads;
        lastPath = path;
        if (fail) throw std::runtime_error("injected read failure");
        return bytes.size() <= limit ? bytes : std::vector<uint8_t>{};
    }
    // A real 1x1, 24-bit BMP, decoded by the Engine's CPU decoder.
    std::vector<uint8_t> bytes = {
        'B','M',58,0,0,0,0,0,0,0,54,0,0,0,40,0,0,0,
        1,0,0,0,1,0,0,0,1,0,24,0,0,0,0,0,4,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,2,1,0
    };
    std::string lastPath;
    int reads = 0;
    bool fail = false;
};

class RestoreTextures final : public TextureManager {
public:
    bool describeTexture(uint32_t id, TextureSourceInfo& output) const override {
        if (id == 90) {
            output = {TextureSourceKind::Asset, "assets/background.bmp", {}};
            return true;
        }
        if (id == 91) {
            output = {TextureSourceKind::Color, "", {1,2,3,4}};
            return true;
        }
        return false;
    }
    uint32_t loadTextureFromRGBA(const uint8_t* rgba, uint16_t width,
                                uint16_t height, const std::string& key) override {
        ++uploads;
        if (throwUpload) throw std::runtime_error("injected upload failure");
        if (fail) return 0;
        pixels.assign(rgba, rgba + size_t(width) * height * 4);
        lastPath = key;
        return nextId++;
    }
    std::vector<uint8_t> pixels;
    std::string lastPath;
    uint32_t nextId = 91;
    int uploads = 0;
    bool fail = false;
    bool throwUpload = false;
};

struct RestoreFixture {
    Engine engine{restoreConfig()};
    RestoreReader reader;
    RestoreTextures textures;
    IAssetReader* oldReader = nullptr;
    ITextureManager* oldTextures = nullptr;
    ILuaManager* vm = nullptr;

    RestoreFixture() {
        REQUIRE(engine.init());
        auto& registry = BackendRegistry::instance();
        oldReader = registry.getAssetReader();
        oldTextures = registry.getTextureManager();
        registry.setAssetReader(&reader);
        registry.setTextureManager(&textures);
        vm = registry.getLuaManager();
        REQUIRE(vm != nullptr);
    }
    ~RestoreFixture() {
        BackendRegistry::instance().setAssetReader(oldReader);
        BackendRegistry::instance().setTextureManager(oldTextures);
    }
    void run(const char* source) {
        vm->resetInstructionBudget();
        lua_State* state = vm->state();
        const int top = lua_gettop(state);
        const int status = luaL_dostring(state, source);
        const std::string error = status == LUA_OK ? "" : lua_tostring(state, -1);
        if (status == LUA_OK) CHECK(lua_gettop(state) == top);
        lua_settop(state, top);
        REQUIRE_MESSAGE(status == LUA_OK, error);
    }
};
}

TEST_CASE("U11 RestoreBinding: prepares immutable pixels without touching the GPU") {
    RestoreFixture fixture;
    fixture.run(R"lua(
        ticket = assert(Restore.prepare_image('assets/background.bmp'))
        local w,h = Restore.image_info(ticket)
        assert(w == 1 and h == 1 and type(ticket) == 'userdata')
    )lua");
    CHECK(fixture.reader.reads == 1);
    CHECK(fixture.textures.uploads == 0);
    fixture.reader.bytes.clear();
    fixture.run(R"lua(
        assert(Restore.materialize_image(ticket) == 91)
        local id, err = Restore.materialize_image(ticket)
        assert(id == nil and type(err) == 'string')
        Restore.discard_image(ticket)
        Restore.discard_image(ticket)
    )lua");
    CHECK(fixture.reader.reads == 1);
    CHECK(fixture.textures.uploads == 1);
    CHECK(fixture.textures.pixels == std::vector<uint8_t>{1,2,3,255});
    CHECK(fixture.textures.lastPath == "assets/background.bmp");
}

TEST_CASE("U11 RestoreBinding: invalid input and failed preparation never upload") {
    RestoreFixture fixture;
    fixture.run(R"lua(
        for _, path in ipairs({'', '../a.bmp', '/a.bmp', 'C:/a.bmp', 'a\\b.bmp', 'a\0b.bmp'}) do
            local ticket,err = Restore.prepare_image(path)
            assert(ticket == nil and type(err) == 'string', path)
        end
    )lua");
    CHECK(fixture.reader.reads == 0);
    fixture.reader.fail = true;
    fixture.run("assert(Restore.prepare_image('assets/a.bmp') == nil)");
    fixture.reader.fail = false;
    fixture.reader.bytes = {1,2,3};
    fixture.run("assert(Restore.prepare_image('assets/a.bmp') == nil)");
    CHECK(fixture.textures.uploads == 0);
}

TEST_CASE("U11 RestoreBinding: discard and failed materialization consume ownership") {
    RestoreFixture fixture;
    fixture.run(R"lua(
        local ticket = assert(Restore.prepare_color(1,2,3,4))
        Restore.discard_image(ticket)
        assert(Restore.materialize_image(ticket) == nil)
        assert(Restore.prepare_color(-1,2,3,4) == nil)
        assert(Restore.prepare_color(1.5,2,3,4) == nil)
        assert(Restore.prepare_color(1,2,3,256) == nil)
    )lua");
    CHECK(fixture.textures.uploads == 0);
    fixture.textures.fail = true;
    fixture.run(R"lua(
        ticket = assert(Restore.prepare_color(1,2,3,255))
        local id,err = Restore.materialize_image(ticket)
        assert(id == nil and type(err) == 'string')
    )lua");
    fixture.textures.fail = false;
    fixture.run("assert(Restore.materialize_image(ticket) == nil)");
    CHECK(fixture.textures.uploads == 1);
    fixture.textures.throwUpload = true;
    fixture.run(R"lua(
        ticket = assert(Restore.prepare_color(1,2,3,255))
        assert(Restore.materialize_image(ticket) == nil)
        assert(Restore.materialize_image(ticket) == nil)
    )lua");
    CHECK(fixture.textures.uploads == 2);
}

TEST_CASE("U11 RestoreBinding: exposes serializable origins and rejects unknown textures") {
    RestoreFixture fixture;
    fixture.run(R"lua(
        local source = assert(Restore.describe_texture(90))
        assert(source.kind == 'asset' and source.path == 'assets/background.bmp')
        source = assert(Restore.describe_texture(91))
        assert(source.kind == 'color' and source.r == 1 and source.g == 2
            and source.b == 3 and source.a == 4)
        for _, id in ipairs({0,-1,1.5,4294967296,100}) do
            local value,err = Restore.describe_texture(id)
            assert(value == nil and type(err) == 'string')
        end
    )lua");
    CHECK(fixture.reader.reads == 0);
    CHECK(fixture.textures.uploads == 0);
}
