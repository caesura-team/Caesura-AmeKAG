#include "doctest.h"
#include "script/vm/LuaManager.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <memory>
#include <stdexcept>

using namespace Caesura;

namespace {
int finalizeLifetimeWitness(lua_State* state) {
    auto* count = *static_cast<int**>(lua_touserdata(state, 1));
    ++*count;
    return 0;
}

void retainLifetimeWitness(lua_State* state, int& finalized) {
    REQUIRE(state != nullptr);
    auto** count = static_cast<int**>(lua_newuserdatauv(state, sizeof(int*), 0));
    *count = &finalized;
    lua_newtable(state);
    lua_pushcfunction(state, finalizeLifetimeWitness);
    lua_setfield(state, -2, "__gc");
    lua_setmetatable(state, -2);
    // Keep the object alive until lua_close, so a collection cannot make a
    // leaking manager appear to release its owned VM at destruction.
    lua_setfield(state, LUA_REGISTRYINDEX, "U2.LifetimeWitness");
    lua_gc(state, LUA_GCCOLLECT);
    CHECK(finalized == 0);
}
} // namespace

TEST_CASE("U2 Lua lifetime: scope exit closes the owned VM") {
    int finalized = 0;
    {
        LuaManager manager;
        REQUIRE(manager.init());
        retainLifetimeWitness(manager.state(), finalized);
    }
    CHECK(finalized == 1);
}

TEST_CASE("U2 Lua lifetime: interface owner destruction closes the owned VM") {
    int finalized = 0;
    {
        std::unique_ptr<ILuaManager> manager = std::make_unique<LuaManager>();
        REQUIRE(manager->init());
        retainLifetimeWitness(manager->state(), finalized);
    }
    CHECK(finalized == 1);
}

TEST_CASE("U2 Lua lifetime: exception unwinding closes the owned VM") {
    int finalized = 0;
    CHECK_THROWS_AS(([&] {
        LuaManager manager;
        REQUIRE(manager.init());
        retainLifetimeWitness(manager.state(), finalized);
        throw std::runtime_error("leave Lua owner scope");
    }()), std::runtime_error);
    CHECK(finalized == 1);
}

TEST_CASE("U2 Lua lifetime: explicit shutdown and reinitialization close each VM once") {
    int firstFinalized = 0;
    int secondFinalized = 0;
    {
        LuaManager manager;
        REQUIRE(manager.init());
        retainLifetimeWitness(manager.state(), firstFinalized);
        manager.shutdown();
        CHECK(manager.state() == nullptr);
        CHECK(firstFinalized == 1);
        manager.shutdown();
        CHECK(firstFinalized == 1);
        REQUIRE(manager.init());
        retainLifetimeWitness(manager.state(), secondFinalized);
        CHECK(firstFinalized == 1);
    }
    CHECK(firstFinalized == 1);
    CHECK(secondFinalized == 1);
}
