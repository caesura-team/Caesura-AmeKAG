#include "doctest.h"
#include "EntryLifecycleBackends.h"
#include "di/BackendRegistry.h"
#include "script/bindings/DevCoreBinding.h"
#include <filesystem>
#include <string>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

TEST_CASE("Native input contract: production input rectangles reach real DevCore integer decoder") {
    Caesura::Test::LifecycleProbe probe;
    Caesura::Test::PlatformBackend platform(probe);
    auto& registry=Caesura::BackendRegistry::instance();
    auto* prior=registry.getPlatformBackend();
    registry.setPlatformBackend(&platform);
    lua_State* L=luaL_newstate();
    struct Cleanup {
        Caesura::BackendRegistry& registry;
        Caesura::IPlatformBackend* prior;
        lua_State* L;
        ~Cleanup(){registry.setPlatformBackend(prior);if(L){Caesura::registerDevCoreBinding(L);lua_close(L);}}
    } cleanup{registry,prior,L};
    REQUIRE(L);
    luaL_openlibs(L);Caesura::registerDevCoreBinding(L);
    lua_getglobal(L,"DevCore");lua_getfield(L,-1,"set_text_input_rect");
    REQUIRE(lua_iscfunction(L,-1));lua_setglobal(L,"CONTRACT_NATIVE_IME_RECT");lua_pop(L,1);
    const auto script=(std::filesystem::path(CAESURA_SOURCE_DIR)/"tests/scripts/test_input_appearance.lua").generic_string();
    const int result=luaL_dofile(L,script.c_str());
    const std::string error=result==LUA_OK?"":lua_tostring(L,-1);
    CHECK_MESSAGE(result==LUA_OK,error);
}
