#include "doctest.h"
#include "entry/Engine.h"
#include "entry/EngineConfig.h"
#include "audio/NullAudioBackend.h"
#include "di/BackendRegistry.h"
#include "script/vm/LuaManager.h"
#include "script/bindings/KAGBinding.h"
#include "script/bindings/RenderBinding.h"
#include "script/bindings/VFXBinding.h"
#include "script/bindings/DebugBinding.h"
#include "script/bindings/DevCoreBinding.h"
#include <thread>
#include <stdexcept>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

using namespace Caesura;

namespace {
class AudioCreationResult final : public NullAudioBackend {
public:
    // Availability is independent of the injected per-operation result.
    bool isPlaybackAvailable() const override { return true; }
    unsigned int playBGM(const std::string&, float) override { return result(); }
    unsigned int playVoice(const std::string&) override { return result(); }
    unsigned int playSE(const std::string&) override { return result(); }
    unsigned int playSE3D(const std::string&, float, float, float) override { return result(); }
    unsigned int result() {
        ++calls;
        if (failWithException) throw std::runtime_error("injected audio creation failure");
        return handle;
    }
    unsigned int handle = 0;
    int calls = 0;
    bool failWithException = false;
};
}

TEST_CASE("U11 KAG audio: creation results and exceptions reach Lua") {
    auto* audio = new AudioCreationResult;
    EngineConfig config;
    config.headless = true;
    config.audio = audio;
    Engine engine{std::move(config)};
    REQUIRE(engine.init());
    auto* vm = BackendRegistry::instance().getLuaManager();
    REQUIRE(vm != nullptr);
    lua_State* state = vm->state();
    const auto expectResult = [&](bool success) {
        vm->resetInstructionBudget();
        lua_pushboolean(state, success);
        lua_setglobal(state, "expected_audio_success");
        const int result = luaL_dostring(state, R"lua(
            assert(KAG.play_bgm('test.wav',0) == expected_audio_success, 'bgm result')
            assert(KAG.play_voice('test.wav') == expected_audio_success, 'voice result')
            assert(KAG.play_se('test.wav') == expected_audio_success, 'se result')
            assert(KAG.play_se_3d('test.wav',1,2,3) == expected_audio_success, '3d se result')
        )lua");
        const std::string message = result == LUA_OK ? "" : lua_tostring(state, -1);
        lua_settop(state, 0);
        CHECK_MESSAGE(result == LUA_OK, message);
    };
    expectResult(false);
    audio->handle = 123;
    expectResult(true);
    audio->failWithException = true;
    CHECK_NOTHROW(expectResult(false));
    CHECK(audio->calls == 12);
}

static LuaManager* initLuaWithBindings() {
    auto* lm = new LuaManager();
    if (!lm->init()) {
        delete lm;
        return nullptr;
    }
    lua_State* L = lm->state();
    registerKAGBinding(L);
    registerRenderBinding(L);
    registerVFXBinding(L);
    registerDebugBinding(L);
    registerDevCoreBinding(L);
    return lm;
}

TEST_CASE("KAG global table exists after registerKAGBinding") {
    auto* lm = initLuaWithBindings();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    lua_getglobal(L, "KAG");
    CHECK(lua_istable(L, -1) == 1);
    lua_pop(L, 1);
    delete lm;
}

TEST_CASE("Render.text_set_font binding exists") {
    auto* lm = initLuaWithBindings();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    lua_getglobal(L, "Render");
    lua_getfield(L, -1, "text_set_font");
    CHECK(lua_isfunction(L, -1) == 1);
    lua_pop(L, 2);
    delete lm;
}

TEST_CASE("Render.text_reset_state binding exists") {
    auto* lm = initLuaWithBindings();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    lua_getglobal(L, "Render");
    lua_getfield(L, -1, "text_reset_state");
    CHECK(lua_isfunction(L, -1) == 1);
    lua_pop(L, 2);
    delete lm;
}

TEST_CASE("KAG.play_bgm is a function") {
    auto* lm = initLuaWithBindings();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    lua_getglobal(L, "KAG");
    lua_getfield(L, -1, "play_bgm");
    CHECK(lua_isfunction(L, -1) == 1);
    lua_pop(L, 2);
    delete lm;
}


TEST_CASE("KAG.render_text is a function") {
    auto* lm = initLuaWithBindings();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    lua_getglobal(L, "KAG");
    lua_getfield(L, -1, "render_text");
    CHECK(lua_isfunction(L, -1) == 1);
    lua_pop(L, 2);
    delete lm;
}

TEST_CASE("Render global table exists") {
    auto* lm = initLuaWithBindings();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    lua_getglobal(L, "Render");
    CHECK(lua_istable(L, -1) == 1);
    lua_pop(L, 1);
    delete lm;
}

TEST_CASE("VFX global table exists") {
    auto* lm = initLuaWithBindings();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    lua_getglobal(L, "VFX");
    CHECK(lua_istable(L, -1) == 1);
    lua_pop(L, 1);
    delete lm;
}

TEST_CASE("Debug global table exists") {
    auto* lm = initLuaWithBindings();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    lua_getglobal(L, "Debug");
    CHECK(lua_istable(L, -1) == 1);
    lua_pop(L, 1);
    delete lm;
}

TEST_CASE("DevCore global table exists") {
    auto* lm = initLuaWithBindings();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    lua_getglobal(L, "DevCore");
    CHECK(lua_istable(L, -1) == 1);
    lua_pop(L, 1);
    delete lm;
}
