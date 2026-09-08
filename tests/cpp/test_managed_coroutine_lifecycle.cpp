#include "doctest.h"
#include "script/vm/ManagedCoroutine.h"

extern "C" {
#include <lualib.h>
}

#include <cstring>
#include <deque>
#include <type_traits>

using namespace Caesura::detail;

namespace {
struct LuaVm {
    lua_State* state = luaL_newstate();
    LuaVm() { REQUIRE(state != nullptr); luaL_openlibs(state); }
    ~LuaVm() { if (state) lua_close(state); }
    void shutdown() { lua_close(state); state = nullptr; }
};

ManagedCoroutine create(lua_State* owner, const char* script) {
    ManagedCoroutine run;
    const auto result = createManagedCoroutine(owner, script, run);
    REQUIRE_MESSAGE(result.status == LUA_OK, result.message);
    REQUIRE(run.co != nullptr);
    REQUIRE(run.reference >= 0);
    return run;
}

lua_Integer integerGlobal(lua_State* owner, const char* name) {
    lua_getglobal(owner, name);
    const auto value = lua_tointeger(owner, -1);
    lua_pop(owner, 1);
    return value;
}

bool booleanGlobal(lua_State* owner, const char* name) {
    lua_getglobal(owner, name);
    const bool value = lua_toboolean(owner, -1) != 0;
    lua_pop(owner, 1);
    return value;
}

void released(lua_State* owner, int reference) {
    lua_rawgeti(owner, LUA_REGISTRYINDEX, reference);
    CHECK_FALSE(lua_isthread(owner, -1));
    lua_pop(owner, 1);
}

void retained(lua_State* owner, const ManagedCoroutine& run) {
    lua_rawgeti(owner, LUA_REGISTRYINDEX, run.reference);
    CHECK(lua_tothread(owner, -1) == run.co);
    lua_pop(owner, 1);
}

struct NativeGuard {
    int* closes;
    int resumes;
};

int closeNativeGuard(lua_State* state) {
    auto* guard = static_cast<NativeGuard*>(lua_touserdata(state, 1));
    if (guard && guard->closes) {
        ++*guard->closes;
        guard->closes = nullptr;
    }
    return 0;
}

int continueNativeGuard(lua_State* state, int, lua_KContext context) {
    auto* guard = static_cast<NativeGuard*>(lua_touserdata(state, static_cast<int>(context)));
    if (!guard || !guard->closes) return luaL_error(state, "native continuation lost its guard");
    if (++guard->resumes < 2) return lua_yieldk(state, 0, context, continueNativeGuard);
    return 0;
}

int beginNativeGuard(lua_State* state) {
    auto* closes = static_cast<int*>(lua_touserdata(state, lua_upvalueindex(1)));
    auto* guard = static_cast<NativeGuard*>(lua_newuserdatauv(state, sizeof(NativeGuard), 0));
    guard->closes = closes;
    guard->resumes = 0;
    const int index = lua_gettop(state);
    luaL_setmetatable(state, "U15.ManagedContinuationGuard");
    lua_toclose(state, index);
    return lua_yieldk(state, 0, index, continueNativeGuard);
}

void registerNativeGuard(lua_State* owner, int* closes) {
    luaL_newmetatable(owner, "U15.ManagedContinuationGuard");
    lua_pushcfunction(owner, closeNativeGuard);
    lua_setfield(owner, -2, "__close");
    lua_pop(owner, 1);
    lua_pushlightuserdata(owner, closes);
    lua_pushcclosure(owner, beginNativeGuard, 1);
    lua_setglobal(owner, "u15_native_guard");
}
} // namespace

static_assert(std::is_trivially_destructible_v<ManagedCoroutine>);
static_assert(std::is_trivially_destructible_v<CoroutineDiagnostic>);
static_assert(std::is_trivially_destructible_v<CoroutineStep>);

TEST_CASE("U15 managed coroutine: abort closes a yielded Lua scope before releasing its reference") {
    LuaVm vm;
    auto run = create(vm.state, R"lua(
        closed=0
        local guard <close> = setmetatable({}, {__close=function() closed=closed+1 end})
        coroutine.yield()
    )lua");
    const int reference = run.reference;
    REQUIRE(resumeManagedCoroutine(vm.state, run).status == LUA_YIELD);
    CHECK(integerGlobal(vm.state, "closed") == 0);
    retained(vm.state, run);
    CHECK(closeManagedCoroutine(vm.state, run).status == LUA_OK);
    CHECK(integerGlobal(vm.state, "closed") == 1);
    CHECK(run.co == nullptr);
    CHECK(run.reference == LUA_NOREF);
    released(vm.state, reference);
    CHECK(closeManagedCoroutine(vm.state, run).status == LUA_OK);
    CHECK(integerGlobal(vm.state, "closed") == 1);
}

TEST_CASE("U15 managed coroutine: yielded values are consumed and normal completion closes once") {
    LuaVm vm;
    auto run = create(vm.state, R"lua(
        closed=0
        local guard <close> = setmetatable({}, {__close=function() closed=closed+1 end})
        coroutine.yield('first', 2, true)
        return 42
    )lua");
    const int reference = run.reference;
    const auto yielded = resumeManagedCoroutine(vm.state, run);
    REQUIRE(yielded.status == LUA_YIELD);
    CHECK(yielded.results == 3);
    CHECK(lua_gettop(run.co) == 0);
    CHECK(integerGlobal(vm.state, "closed") == 0);
    const auto finished = resumeManagedCoroutine(vm.state, run);
    CHECK(finished.status == LUA_OK);
    CHECK(finished.results == 1);
    CHECK(finished.closed.status == LUA_OK);
    CHECK(integerGlobal(vm.state, "closed") == 1);
    CHECK(run.co == nullptr);
    released(vm.state, reference);
}

TEST_CASE("U15 managed coroutine: an error terminal still closes pending Lua scopes") {
    LuaVm vm;
    auto run = create(vm.state, R"lua(
        closed=0
        local guard <close> = setmetatable({}, {__close=function() closed=closed+1 end})
        coroutine.yield()
        error('managed-runtime-error')
    )lua");
    const int reference = run.reference;
    REQUIRE(resumeManagedCoroutine(vm.state, run).status == LUA_YIELD);
    const auto finished = resumeManagedCoroutine(vm.state, run);
    CHECK(finished.status == LUA_ERRRUN);
    CHECK(std::strstr(finished.error, "managed-runtime-error") != nullptr);
    CHECK(integerGlobal(vm.state, "closed") == 1);
    // Lua closes an errored coroutine with the original error status as well.
    CHECK(finished.closed.status == LUA_ERRRUN);
    CHECK(run.co == nullptr);
    released(vm.state, reference);
}

TEST_CASE("U15 managed coroutine: close errors still release references without error coercion") {
    LuaVm vm;
    const char* source = R"lua(
        closed=0
        local guard <close> = setmetatable({}, {__close=function()
            closed=closed+1; error('managed-close-error')
        end})
        coroutine.yield()
    )lua";
    bool nonString = false;
    SUBCASE("string error") {}
    SUBCASE("non-string error must not call __tostring") {
        nonString = true;
        source = R"lua(
            closed=0; tostring_called=false
            local guard <close> = setmetatable({}, {__close=function()
                closed=closed+1
                error(setmetatable({}, {__tostring=function()
                    tostring_called=true; error('diagnostic coercion must not run')
                end}))
            end})
            coroutine.yield()
        )lua";
    }
    auto run = create(vm.state, source);
    const int reference = run.reference;
    REQUIRE(resumeManagedCoroutine(vm.state, run).status == LUA_YIELD);
    const auto closed = closeManagedCoroutine(vm.state, run);
    CHECK(closed.status == LUA_ERRRUN);
    CHECK(integerGlobal(vm.state, "closed") == 1);
    CHECK(run.co == nullptr);
    released(vm.state, reference);
    if (nonString) {
        CHECK_FALSE(booleanGlobal(vm.state, "tostring_called"));
        CHECK(std::strstr(closed.message, "non-string") != nullptr);
    } else {
        CHECK(std::strstr(closed.message, "managed-close-error") != nullptr);
    }
    auto next = create(vm.state, "return 1");
    CHECK(resumeManagedCoroutine(vm.state, next).status == LUA_OK);
}

TEST_CASE("U15 managed coroutine: removing a middle run never reuses another active reference") {
    LuaVm vm;
    std::deque<ManagedCoroutine> runs;
    runs.push_back(create(vm.state, "coroutine.yield(); finished_a=true"));
    runs.push_back(create(vm.state, "finished_b=true"));
    runs.push_back(create(vm.state, "coroutine.yield(); finished_c=true"));
    REQUIRE(resumeManagedCoroutine(vm.state, runs[0]).status == LUA_YIELD);
    REQUIRE(resumeManagedCoroutine(vm.state, runs[2]).status == LUA_YIELD);
    const int releasedReference = runs[1].reference;
    REQUIRE(resumeManagedCoroutine(vm.state, runs[1]).status == LUA_OK);
    released(vm.state, releasedReference);
    runs.erase(runs.begin() + 1);
    auto next = create(vm.state, "coroutine.yield(); finished_d=true");
    CHECK(next.reference != runs[0].reference);
    CHECK(next.reference != runs[1].reference);
    runs.push_back(next);
    lua_gc(vm.state, LUA_GCCOLLECT);
    for (const auto& active : runs) retained(vm.state, active);
    CHECK(resumeManagedCoroutine(vm.state, runs[0]).status == LUA_OK);
    CHECK(resumeManagedCoroutine(vm.state, runs[1]).status == LUA_OK);
    CHECK(resumeManagedCoroutine(vm.state, runs[2]).status == LUA_YIELD);
    CHECK(resumeManagedCoroutine(vm.state, runs[2]).status == LUA_OK);
    CHECK(booleanGlobal(vm.state, "finished_a"));
    CHECK(booleanGlobal(vm.state, "finished_b"));
    CHECK(booleanGlobal(vm.state, "finished_c"));
    CHECK(booleanGlobal(vm.state, "finished_d"));
}

TEST_CASE("U15 managed coroutine: yield zero preserves a native continuation close guard") {
    LuaVm vm;
    int closes = 0;
    registerNativeGuard(vm.state, &closes);
    bool abort = false;
    SUBCASE("normal continuation return") {}
    SUBCASE("abort while suspended") { abort = true; }
    auto run = create(vm.state, "u15_native_guard()");
    const auto first = resumeManagedCoroutine(vm.state, run);
    REQUIRE(first.status == LUA_YIELD);
    CHECK(first.results == 0);
    CHECK(lua_gettop(run.co) == 1);
    CHECK(closes == 0);
    REQUIRE(resumeManagedCoroutine(vm.state, run).status == LUA_YIELD);
    CHECK(closes == 0);
    if (abort) CHECK(closeManagedCoroutine(vm.state, run).status == LUA_OK);
    else CHECK(resumeManagedCoroutine(vm.state, run).status == LUA_OK);
    CHECK(closes == 1);
    CHECK(run.co == nullptr);
}

TEST_CASE("U15 managed coroutine: compile failure and a closed VM leave safe host records") {
    LuaVm vm;
    ManagedCoroutine invalid;
    lua_pushinteger(vm.state, 91);
    const int originalTop = lua_gettop(vm.state);
    const auto compile = createManagedCoroutine(vm.state, "local =", invalid);
    CHECK(compile.status == LUA_ERRSYNTAX);
    CHECK(invalid.co == nullptr);
    CHECK(invalid.reference == LUA_NOREF);
    CHECK(lua_gettop(vm.state) == originalTop);
    CHECK(lua_tointeger(vm.state, -1) == 91);
    lua_pop(vm.state, 1);
    auto run = create(vm.state, "coroutine.yield()");
    REQUIRE(resumeManagedCoroutine(vm.state, run).status == LUA_YIELD);
    vm.shutdown();
    // The coroutine pointer is now stale. Null-owner cleanup must only forget
    // the trivially destructible host record, never call a Lua API on it.
    CHECK(closeManagedCoroutine(nullptr, run).status == LUA_OK);
    CHECK(run.co == nullptr);
    CHECK(run.reference == LUA_NOREF);
}
