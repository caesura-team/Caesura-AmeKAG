// test_script_bindings.cpp - script bindings unit tests (R4.1)
#include "doctest.h"
#include <map>
#include "script/vm/LuaManager.h"
#include "script/bindings/KAGBinding.h"
#include "script/bindings/RenderBinding.h"
#include "script/bindings/VFXBinding.h"
#include "script/bindings/MiniGameBinding.h"
#include "script/bindings/DebugBinding.h"
#include "script/bindings/DevCoreBinding.h"
#include "script/bindings/SteamBinding.h"
#include "di/BackendRegistry.h"
#include "input/InputRouter.h"
#include "render/NullRenderDevice.h"
#include "platform/NullPlatformBackend.h"
#include "steam/NullSteamBackend.h"

#include <cstring>
#include <string>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

using namespace Caesura;

namespace {

class UnlockingSteamBackend final : public NullSteamBackend {
public:
    // This fixture supplies an available simulated achievement service.
    bool isAvailable() const override { return true; }
    bool unlockAchievement(const char* id) override {
        lastAchievement = id ? id : "";
        return true;
    }

    std::string lastAchievement;
};

class ScopedSteamRegistration {
public:
    explicit ScopedSteamRegistration(ISteamBackend* backend)
        : m_previous(BackendRegistry::instance().getSteamBackend()) {
        BackendRegistry::instance().setSteamBackend(backend);
    }

    ~ScopedSteamRegistration() {
        BackendRegistry::instance().setSteamBackend(m_previous);
    }

private:
    ISteamBackend* m_previous;
};

} // namespace

static LuaManager* initBindingLua() {
    auto* lm = new LuaManager();
    if (!lm->init()) { delete lm; return nullptr; }
    lua_State* L = lm->state();
    static NullRenderDevice render;
    static NullPlatformBackend platform;
    BackendRegistry::instance().setRenderDevice(&render);
    BackendRegistry::instance().setPlatformBackend(&platform);
    registerKAGBinding(L);
    registerRenderBinding(L);
    registerVFXBinding(L);
    registerMiniGameBinding(L);
    registerDebugBinding(L);
    registerDevCoreBinding(L);
    return lm;
}

TEST_CASE("Bindings: DevCore module registered as global") {
    auto* lm = initBindingLua();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    lua_getglobal(L, "DevCore");
    CHECK(lua_istable(L, -1));
    lua_pop(L, 1);
    delete lm;
}

TEST_CASE("Bindings: DevCore.log callable") {
    auto* lm = initBindingLua();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    lua_getglobal(L, "DevCore");
    lua_getfield(L, -1, "log");
    CHECK(lua_isfunction(L, -1));
    lua_pushstring(L, "test message");
    int ret = lua_pcall(L, 1, 0, 0);
    CHECK(ret == LUA_OK);
    lua_pop(L, 1);
    delete lm;
}

TEST_CASE("Bindings: DevCore input focus roundtrip and validation") {
    auto* lm = initBindingLua();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    InputRouter router;
    lua_pushlightuserdata(L, &router);
    lua_setfield(L, LUA_REGISTRYINDEX, "Caesura.InputRouter");

    const int focusScriptResult = luaL_dostring(L,
        "local ok = DevCore.set_input_focus('game')\n"
        "assert(ok == true)\n"
        "assert(DevCore.get_input_focus() == 'GAME')\n"
        "local invalid, reason = DevCore.set_input_focus('UI')\n"
        "assert(invalid == false and type(reason) == 'string')\n"
        "assert(DevCore.get_input_focus() == 'GAME')\n"
        "local wrong_type, type_reason = DevCore.set_input_focus({})\n"
        "assert(wrong_type == false and type(type_reason) == 'string')\n"
        "assert(DevCore.get_input_focus() == 'GAME')\n"
        "assert(DevCore.set_input_focus('KAG') == true)\n"
        "assert(DevCore.get_input_focus() == 'KAG')");
    REQUIRE(focusScriptResult == LUA_OK);
    CHECK(router.getFocus() == InputFocus::KAG);

    delete lm;
}

TEST_CASE("Bindings: VFX module registered") {
    auto* lm = initBindingLua();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    lua_getglobal(L, "VFX");
    CHECK(lua_istable(L, -1));
    lua_pop(L, 1);
    delete lm;
}

TEST_CASE("Bindings: VFX particles functions exist") {
    auto* lm = initBindingLua();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    lua_getglobal(L, "VFX");
    REQUIRE(lua_istable(L, -1));
    lua_getfield(L, -1, "particles_create_emitter");
    CHECK(lua_isfunction(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, -1, "particles_alive_count");
    CHECK(lua_isfunction(L, -1));
    lua_pop(L, 1);
    lua_pop(L, 1);
    delete lm;
}

TEST_CASE("Bindings: mini_game module registered as global") {
    auto* lm = initBindingLua();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    lua_getglobal(L, "mini_game");
    CHECK(lua_istable(L, -1));
    lua_pop(L, 1);
    delete lm;
}

TEST_CASE("Bindings: mini_game exposes all 20 documented APIs") {
    auto* lm = initBindingLua();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    lua_getglobal(L, "mini_game");
    REQUIRE(lua_istable(L, -1));
    const char* methods[] = {
        "spawn_cube", "spawn_sphere", "spawn_plane", "remove_object",
        "set_camera", "create_material", "set_material", "set_ambient",
        "set_directional", "add_point_light", "remove_light",
        "check_collision", "set_collision", "set_velocity", "set_gravity",
        "load_scene", "unload_scene", "enter", "leave", "is_active",
    };
    for (const char* m : methods) {
        lua_getfield(L, -1, m);
        CHECK_MESSAGE(lua_isfunction(L, -1), m);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    delete lm;
}

TEST_CASE("Bindings: mini_game methods are safe without a backend") {
    // No "Caesura.MiniGameBackend" registry entry in this test context:
    // every binding must return a safe value instead of crashing.
    auto* lm = initBindingLua();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    const char* script =
        "local mg = mini_game\n"
        "local h = mg.load_scene('missing.json')\n"
        "assert(type(h) == 'number')\n"
        "mg.enter(1)\n"
        "mg.leave()\n"
        "assert(mg.is_active() == false)\n"
        "local ok, err = mg.spawn_cube(0, 0, 0)\n"
        "assert(ok == false or type(ok) == 'number', tostring(err))\n"
        "local mat = mg.create_material(1, 0, 0)\n"
        "assert(mat == false or type(mat) == 'number')\n"
        "mg.set_camera(0, 0, 0, 0, 0, 0)\n"
        "mg.set_ambient(0.1, 0.1, 0.1)\n"
        "mg.set_directional(0, -1, 0)\n"
        "mg.set_gravity(1, true)\n"
        "mg.set_velocity(1, 1, 0, 0)\n"
        "mg.remove_object(1)\n"
        "return 'ok'\n";
    REQUIRE(luaL_dostring(L, script) == LUA_OK);
    lua_pop(L, 1);
    delete lm;
}

TEST_CASE("Bindings: Render module registered") {
    auto* lm = initBindingLua();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    lua_getglobal(L, "Render");
    CHECK(lua_istable(L, -1));
    lua_pop(L, 1);
    delete lm;
}

TEST_CASE("Bindings: KAG.play_bgm returns boolean") {
    auto* lm = initBindingLua();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    lua_getglobal(L, "KAG");
    lua_getfield(L, -1, "play_bgm");
    lua_pushstring(L, "nonexistent.wav");
    lua_pushnumber(L, 0.5);
    int ret = lua_pcall(L, 2, 1, 0);
    CHECK(ret == LUA_OK);
    CHECK(lua_isboolean(L, -1));
    CHECK_FALSE(lua_toboolean(L, -1)); // returns false (no audio backend)
    lua_pop(L, 2);
    delete lm;
}

TEST_CASE("Bindings: KAG.stop_bgm does not crash") {
    auto* lm = initBindingLua();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    lua_getglobal(L, "KAG");
    lua_getfield(L, -1, "stop_bgm");
    int ret = lua_pcall(L, 0, 0, 0);
    CHECK(ret == LUA_OK);
    lua_pop(L, 1);
    delete lm;
}

TEST_CASE("Bindings: Debug module functions exist") {
    auto* lm = initBindingLua();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    lua_getglobal(L, "Debug");
    REQUIRE(lua_istable(L, -1));
    lua_getfield(L, -1, "get_last_error");
    CHECK(lua_isfunction(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, -1, "log");
    CHECK(lua_isfunction(L, -1));
    lua_pop(L, 1);
    lua_pop(L, 1);
    delete lm;
}

TEST_CASE("Bindings: KAG global table must have expected APIs") {
    auto* lm = initBindingLua();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    lua_getglobal(L, "KAG");
    REQUIRE(lua_istable(L, -1));
    lua_getfield(L, -1, "play_bgm");
    CHECK(lua_isfunction(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, -1, "play_se");
    CHECK(lua_isfunction(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, -1, "render_text");
    CHECK(lua_isfunction(L, -1));
    lua_pop(L, 1);
    lua_pop(L, 1);
    delete lm;
}

TEST_CASE("Bindings: Steam module resolves backend through BackendRegistry") {
    LuaManager lua;
    REQUIRE(lua.init());

    UnlockingSteamBackend steam;
    ScopedSteamRegistration registration(&steam);
    registerSteamBinding(lua.state());

    lua_getglobal(lua.state(), "steam");
    REQUIRE(lua_istable(lua.state(), -1));
    lua_getfield(lua.state(), -1, "unlock_achievement");
    lua_pushstring(lua.state(), "FIRST_SCENE");
    REQUIRE(lua_pcall(lua.state(), 1, 1, 0) == LUA_OK);
    CHECK(lua_toboolean(lua.state(), -1));
    CHECK(steam.lastAchievement == "FIRST_SCENE");
    lua_pop(lua.state(), 2);
}

// ---- Steam cloud-save bindings (no SDK required) -------------------------

class RecordingCloudBackend final : public NullSteamBackend {
public:
    // This fixture supplies an available in-memory cloud service.
    bool isAvailable() const override { return true; }
    std::map<std::string, std::string> files;
    std::string lastWritten;
    std::string lastRead;
    std::string lastDeleted;

    bool cloudWrite(const char* fileName, const void* data, int32_t size) override {
        lastWritten = fileName ? fileName : "";
        files[lastWritten] = std::string(static_cast<const char*>(data),
                                         static_cast<size_t>(size));
        return true;
    }
    int32_t cloudRead(const char* fileName, void* buffer, int32_t maxSize) override {
        lastRead = fileName ? fileName : "";
        const auto it = files.find(lastRead);
        if (it == files.end() || !buffer || maxSize <= 0) return 0;
        const int32_t n = std::min<int32_t>(maxSize,
                                            static_cast<int32_t>(it->second.size()));
        std::memcpy(buffer, it->second.data(), static_cast<size_t>(n));
        return n;
    }
    int32_t cloudFileSize(const char* fileName) const override {
        const auto it = files.find(fileName ? fileName : "");
        return it == files.end() ? 0 : static_cast<int32_t>(it->second.size());
    }
    bool cloudFileExists(const char* fileName) const override {
        return files.count(fileName ? fileName : "") > 0;
    }
    bool cloudDelete(const char* fileName) override {
        lastDeleted = fileName ? fileName : "";
        files.erase(lastDeleted);
        return true;
    }
    int32_t cloudFileCount() const override { return static_cast<int32_t>(files.size()); }
    const char* cloudFileNameAt(int32_t index) const override {
        if (index < 0) return "";
        int32_t i = 0;
        for (const auto& [name, _] : files) {
            if (i++ == index) return name.c_str();
        }
        return "";
    }
};

TEST_CASE("Bindings: Steam cloud_save/load round trip through Lua") {
    LuaManager lua;
    REQUIRE(lua.init());

    RecordingCloudBackend steam;
    ScopedSteamRegistration registration(&steam);
    registerSteamBinding(lua.state());

    lua_State* L = lua.state();
    // cloud_write("slot_3", "hello cloud")
    lua_getglobal(L, "steam");
    REQUIRE(lua_istable(L, -1));
    lua_getfield(L, -1, "cloud_write");
    lua_pushstring(L, "slot_3");
    lua_pushstring(L, "hello cloud");
    REQUIRE(lua_pcall(L, 2, 1, 0) == LUA_OK);
    CHECK(lua_toboolean(L, -1));
    lua_pop(L, 1);
    CHECK(steam.files["slot_3"] == "hello cloud");

    // cloud_read("slot_3") -> "hello cloud"
    lua_getfield(L, -1, "cloud_read");
    lua_pushstring(L, "slot_3");
    REQUIRE(lua_pcall(L, 1, 1, 0) == LUA_OK);
    CHECK(std::string(lua_tostring(L, -1)) == "hello cloud");
    lua_pop(L, 1);

    // cloud_file_exists / cloud_file_size
    lua_getfield(L, -1, "cloud_file_exists");
    lua_pushstring(L, "slot_3");
    REQUIRE(lua_pcall(L, 1, 1, 0) == LUA_OK);
    CHECK(lua_toboolean(L, -1));
    lua_pop(L, 1);

    lua_getfield(L, -1, "cloud_file_size");
    lua_pushstring(L, "slot_3");
    REQUIRE(lua_pcall(L, 1, 1, 0) == LUA_OK);
    CHECK(lua_tointeger(L, -1) == 11);
    lua_pop(L, 1);

    // cloud_list() -> { "slot_3" }
    lua_getfield(L, -1, "cloud_list");
    REQUIRE(lua_pcall(L, 0, 1, 0) == LUA_OK);
    REQUIRE(lua_istable(L, -1));
    lua_geti(L, -1, 1);
    CHECK(std::string(lua_tostring(L, -1)) == "slot_3");
    lua_pop(L, 1);
    lua_pop(L, 1);

    // cloud_delete("slot_3")
    lua_getfield(L, -1, "cloud_delete");
    lua_pushstring(L, "slot_3");
    REQUIRE(lua_pcall(L, 1, 1, 0) == LUA_OK);
    CHECK(lua_toboolean(L, -1));
    lua_pop(L, 1);
    CHECK(steam.files.empty());

    // cloud_read of a missing file -> nil (graceful)
    lua_getfield(L, -1, "cloud_read");
    lua_pushstring(L, "nope");
    REQUIRE(lua_pcall(L, 1, 1, 0) == LUA_OK);
    CHECK(lua_isnil(L, -1));
    lua_pop(L, 1);

    lua_pop(L, 1);  // steam table
}

TEST_CASE("Bindings: Steam module registered without backend (Null fallback)") {
    LuaManager lua;
    REQUIRE(lua.init());
    // No backend in the registry: the table still exists and every call
    // returns a safe default (no nil-function error).
    registerSteamBinding(lua.state());
    lua_getglobal(lua.state(), "steam");
    REQUIRE(lua_istable(lua.state(), -1));
    lua_getfield(lua.state(), -1, "cloud_quota_total");
    REQUIRE(lua_pcall(lua.state(), 0, 1, 0) == LUA_OK);
    CHECK(lua_tointeger(lua.state(), -1) == 0);
    lua_pop(lua.state(), 2);
}
