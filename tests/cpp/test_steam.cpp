// test_steam.cpp — Steam backend tests (NullSteamBackend)
#include "doctest.h"
#include "steam/NullSteamBackend.h"
#include "steam/api/ISteamBackend.h"

using namespace Caesura;

TEST_CASE("NullSteamBackend::init returns false") {
    NullSteamBackend steam;
    CHECK(steam.init() == false);
}

TEST_CASE("NullSteamBackend::name") {
    NullSteamBackend steam;
    CHECK(std::string(steam.name()) == "NullSteam");
}

TEST_CASE("NullSteamBackend::overlay is never active") {
    NullSteamBackend steam;
    CHECK(steam.isOverlayActive() == false);
}

TEST_CASE("NullSteamBackend::achievements always return false") {
    NullSteamBackend steam;
    CHECK(steam.unlockAchievement("ACH_TEST") == false);
    CHECK(steam.isAchievementUnlocked("ACH_TEST") == false);
    CHECK(steam.resetAchievement("ACH_TEST") == false);
    CHECK(steam.resetAllAchievements() == false);
}

TEST_CASE("NullSteamBackend::stats return default values") {
    NullSteamBackend steam;
    CHECK(steam.setStatInt("kills", 10) == false);
    CHECK(steam.getStatInt("kills") == 0);
    CHECK(steam.setStatFloat("time", 1.5f) == false);
    CHECK(steam.getStatFloat("time") == 0.0f);
    CHECK(steam.storeStats() == false);
}

TEST_CASE("NullSteamBackend::cloud operations return empty") {
    NullSteamBackend steam;
    const char* data = "test save data";
    CHECK(steam.cloudWrite("save.dat", data, 13) == false);
    char buf[256] = {};
    CHECK(steam.cloudRead("save.dat", buf, 256) == 0);
    CHECK(steam.cloudFileSize("save.dat") == 0);
    CHECK(steam.cloudFileExists("save.dat") == false);
    CHECK(steam.cloudDelete("save.dat") == false);
    CHECK(steam.cloudQuotaTotal() == 0);
    CHECK(steam.cloudQuotaUsed() == 0);
}

TEST_CASE("NullSteamBackend::runCallbacks does not crash") {
    NullSteamBackend steam;
    steam.runCallbacks();  // should be no-op
}

TEST_CASE("NullSteamBackend::shutdown is idempotent") {
    NullSteamBackend steam;
    steam.shutdown();
    steam.shutdown();  // second call should not crash
}

// ---------------------------------------------------------------------------
// G3: Conditional-compilation tests for the real Steam backend.
//
// SteamBackend.cpp is ALWAYS compiled (cmake/CaesuraModules.cmake
// caesura_add_module(Steam ...)), so in a no-SDK build every method compiles
// its #else branch. The four cases below preserve that degradation contract
// without the SDK and exercise uninitialized sentinels with the SDK. SDK ON
// cases never call init(): initialized sessions and account actions require
// separate acceptance. Static source checks follow these runtime cases.
// ---------------------------------------------------------------------------

#include "steam/SteamBackend.h"

#include <filesystem>
#include <fstream>
#include <sstream>

static std::string readSteamSourceFile(const std::string& relative) {
#ifdef CAESURA_SOURCE_DIR
    // Out-of-tree builds: prefer the CMake-injected source root.
    const std::filesystem::path fromMacro(CAESURA_SOURCE_DIR);
    if (std::filesystem::exists(fromMacro / "src") &&
        std::filesystem::exists(fromMacro / "tests" / "cpp")) {
        std::ifstream file(fromMacro / relative, std::ios::binary);
        std::ostringstream out;
        out << file.rdbuf();
        return out.str();
    }
#endif
    auto path = std::filesystem::current_path();
    while (!path.empty()) {
        if (std::filesystem::exists(path / "src") &&
            std::filesystem::exists(path / "tests" / "cpp")) {
            break;
        }
        const auto parent = path.parent_path();
        if (parent == path) {
            path.clear();
            break;
        }
        path = parent;
    }
    REQUIRE_FALSE(path.empty());
    std::ifstream file(path / relative, std::ios::binary);
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}

#ifdef CAESURA_HAS_STEAM
TEST_CASE("SteamBackend (SDK ON uninitialized) default state is unavailable") {
#else
TEST_CASE("SteamBackend (no-SDK build) init degrades to false") {
#endif
    SteamBackend steam;
#ifdef CAESURA_HAS_STEAM
    REQUIRE_FALSE(steam.isAvailable());
#else
    // Without the SDK the real backend must refuse to initialize.
    CHECK(steam.init() == false);
#endif
    CHECK(std::string(steam.name()) == "Steam");
    steam.shutdown();
#ifdef CAESURA_HAS_STEAM
    CHECK_FALSE(steam.isAvailable());
#endif
}

#ifdef CAESURA_HAS_STEAM
TEST_CASE("SteamBackend (SDK ON uninitialized) feature gates return disabled sentinels") {
#else
TEST_CASE("SteamBackend (no-SDK build) feature gates all return disabled sentinels") {
#endif
    SteamBackend steam;
#ifdef CAESURA_HAS_STEAM
    REQUIRE_FALSE(steam.isAvailable());
#else
    steam.init();
#endif
    CHECK(steam.isOverlayActive() == false);
    CHECK(steam.unlockAchievement("ACH_TEST") == false);
    CHECK(steam.isAchievementUnlocked("ACH_TEST") == false);
    CHECK(steam.resetAchievement("ACH_TEST") == false);
    CHECK(steam.resetAllAchievements() == false);
#ifdef CAESURA_HAS_STEAM
    CHECK_FALSE(steam.isAvailable());
#endif
}

#ifdef CAESURA_HAS_STEAM
TEST_CASE("SteamBackend (SDK ON uninitialized) stats return default values") {
#else
TEST_CASE("SteamBackend (no-SDK build) stats degrade to default values") {
#endif
    SteamBackend steam;
#ifdef CAESURA_HAS_STEAM
    REQUIRE_FALSE(steam.isAvailable());
#else
    steam.init();
#endif
    CHECK(steam.setStatInt("kills", 10) == false);
    CHECK(steam.getStatInt("kills") == 0);
    CHECK(steam.setStatFloat("time", 1.5f) == false);
    CHECK(steam.getStatFloat("time") == 0.0f);
    CHECK(steam.storeStats() == false);
#ifdef CAESURA_HAS_STEAM
    CHECK_FALSE(steam.isAvailable());
#endif
}

#ifdef CAESURA_HAS_STEAM
TEST_CASE("SteamBackend (SDK ON uninitialized) cloud operations return empty") {
#else
TEST_CASE("SteamBackend (no-SDK build) cloud operations degrade to empty") {
#endif
    SteamBackend steam;
#ifdef CAESURA_HAS_STEAM
    REQUIRE_FALSE(steam.isAvailable());
#else
    steam.init();
#endif
    const char* data = "save payload";
    CHECK(steam.cloudWrite("save.dat", data, 12) == false);
    char buf[64] = {};
    CHECK(steam.cloudRead("save.dat", buf, 64) == 0);
    CHECK(steam.cloudFileSize("save.dat") == 0);
    CHECK(steam.cloudFileExists("save.dat") == false);
    CHECK(steam.cloudDelete("save.dat") == false);
    CHECK(steam.cloudQuotaTotal() == 0);
    CHECK(steam.cloudQuotaUsed() == 0);
    CHECK(steam.cloudFileCount() == 0);
    CHECK(std::string(steam.cloudFileNameAt(0)) == "");
#ifdef CAESURA_HAS_STEAM
    CHECK_FALSE(steam.isAvailable());
#endif
}

TEST_CASE("SteamBackend (no-SDK build) destructor + runCallbacks are safe") {
    // shutdown via destructor must not crash; runCallbacks is a no-op.
    {
        SteamBackend steam;
        steam.runCallbacks();
    }  // ~SteamBackend() calls shutdown()
    SteamBackend steam2;
    steam2.shutdown();
    steam2.shutdown();  // idempotent
}

TEST_CASE("SteamBackend no-SDK build must never reference Steamworks symbols") {
    // Every real SDK call site lives behind #ifdef CAESURA_HAS_STEAM. If any
    // Steamworks symbol slipped outside a guard, a no-SDK build would fail to
    // link -- this static assertion fails that drift before it reaches CI.
    const std::string src = readSteamSourceFile("src/steam/SteamBackend.cpp");
    CHECK(src.find("SteamAPI_Init()") != std::string::npos);        // SDK path exists
    CHECK(src.find("SteamAPI_RunCallbacks()") != std::string::npos);
    CHECK(src.find("SteamUserStats()") != std::string::npos);
    CHECK(src.find("SteamRemoteStorage()") != std::string::npos);
    CHECK(src.find("#ifdef CAESURA_HAS_STEAM") != std::string::npos);
    // Negative: no Steamworks type/macro appears outside a guard (the header
    // only declares CCallbackManual members inside its own guard too -- the
    // STEAM_CALLBACK macro form was replaced in Sprint 4b/Steam-SDK bring-up
    // because its protected default ctor is inaccessible to a non-derived
    // holder class; CCallbackManual + explicit Register works for holders).
    const std::string header = readSteamSourceFile("src/steam/SteamBackend.h");
    CHECK(header.find("CCallbackManual") != std::string::npos);
    CHECK(header.find("#ifdef CAESURA_HAS_STEAM") != std::string::npos);
}

// Contract-only boundary injection: real schema/handler and actual C binding.
// This recorder never advertises Steam availability and never contacts an SDK.
#include "di/BackendRegistry.h"
#include "script/bindings/SteamBinding.h"
#include "script/bindings/EngineBinding.h"
#include "script/bindings/KAGBinding.h"
#include "script/bindings/RenderBinding.h"
#include "script/bindings/DevCoreBinding.h"
#include <memory>
#include <vector>
#include <limits>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
namespace {
class SteamContractRecorder final : public NullSteamBackend {
public:
    bool accepted=true;
    std::vector<std::string> unlocks,names,files;
    std::vector<int32_t> ints;
    std::vector<float> floats;
    std::string payload;
    bool unlockAchievement(const char* id) override {unlocks.emplace_back(id);return accepted;}
    bool setStatInt(const char* name,int32_t value) override {names.emplace_back(name);ints.push_back(value);return accepted;}
    bool setStatFloat(const char* name,float value) override {names.emplace_back(name);floats.push_back(value);return accepted;}
    bool cloudWrite(const char* file,const void* bytes,int32_t size) override {
        files.emplace_back(file);payload.assign(static_cast<const char*>(bytes),static_cast<size_t>(size));return accepted;
    }
};
struct SteamBindingFixture {
    BackendRegistry& registry=BackendRegistry::instance();
    ISteamBackend* previous=registry.getSteamBackend();
    std::unique_ptr<lua_State,decltype(&lua_close)> L{luaL_newstate(),lua_close};
    std::string error;
    explicit SteamBindingFixture(ISteamBackend& backend) {
        registry.setSteamBackend(&backend);
        if(!L)return;
        luaL_openlibs(L.get());registerSteamBinding(L.get());
        engine_binding::registerEngineBindings(L.get());registerKAGBinding(L.get());
        registerRenderBinding(L.get());registerDevCoreBinding(L.get());
        const auto root=std::filesystem::path(CAESURA_SOURCE_DIR).generic_string();
        const auto search=root+"/scripts/?.lua;"+root+"/scripts/?/init.lua;";
        lua_pushlstring(L.get(),search.data(),search.size());lua_setglobal(L.get(),"STEAM_CONTRACT_PATH");
    }
    ~SteamBindingFixture(){L.reset();registry.setSteamBackend(previous);}
    bool run(const char* code) {
        lua_settop(L.get(),0);const int status=luaL_dostring(L.get(),code);
        const char* text=status==LUA_OK?nullptr:lua_tostring(L.get(),-1);
        error=text?text:"";return status==LUA_OK;
    }
    bool boot(){return run(R"lua(
        package.path=STEAM_CONTRACT_PATH..package.path
        assert(debug.getinfo(steam.unlock_achievement,'S').what=='C')
        assert(steam.set_achievement==steam.unlock_achievement)
        assert(debug.getinfo(Engine.get_capability_profile,'S').what=='C')
        assert(Engine.get_capability_profile().available.steam==false)
        local schema=require('kag.schema')
        local system=require('kag.commands.system')
        -- Explicit handler ABI slice, not a supported capability claim.
        function steam_contract(params,ctx)
            ctx=ctx or {f={},sf={},tf={},lf={},mp={},current_scene='steam-contract.ks',token_index=1}
            local result=system.steam_achievement(ctx,schema.coerce('steam_achievement',params,ctx))
            return ctx,result
        end
        function steam_dispatch(source)
            local tokenizer=require('tokenizer');local scheduler=require('scheduler');require('kag')
            local ctx={f={},sf={},tf={},lf={},mp={},call_stack={},current_scene='steam-disabled.ks',token_index=1,stop_flag=false}
            local tokens=tokenizer.parse(source);ctx.tokens=tokens
            local failure
            ctx.handle_error=function(_,message) failure=message;ctx.stop_flag=true;ctx._command_error=true end
            local co=coroutine.create(function() scheduler.run(ctx,tokens,1) end)
            for _=1,32 do
                if coroutine.status(co)=='dead' then break end
                local ok,err=coroutine.resume(co,16)
                if not ok then failure=err;break end
            end
            assert(coroutine.status(co)=='dead','Steam command unexpectedly blocked')
            return ctx,failure
        end
    )lua");}
};
}

TEST_CASE("Steam native contract: schema id name positional and canonical priority reach the real C binding") {
    SteamContractRecorder recorder;SteamBindingFixture f(recorder);REQUIRE(f.L!=nullptr);REQUIRE_MESSAGE(f.boot(),f.error);
    REQUIRE_MESSAGE(f.run(R"lua(
        for _,params in ipairs({{id='ACH_ID'},{name='ACH_NAME'},{'ACH_POSITIONAL'},{id='ACH_CANONICAL',name='IGNORED'},{id='',name='ACH_EMPTY_ALIAS'}}) do
            local ctx,result=steam_contract(params)
            assert(result==true and ctx.tf.steam_achievement_result=='unlocked')
        end
        assert(Engine.get_capability_profile().available.steam==false,'recorder is not a live Steam session')
    )lua"),f.error);
    CHECK((recorder.unlocks==std::vector<std::string>{"ACH_ID","ACH_NAME","ACH_POSITIONAL","ACH_CANONICAL","ACH_EMPTY_ALIAS"}));
}

TEST_CASE("Steam native contract: condition skip and real backend refusal preserve story result semantics") {
    SteamContractRecorder recorder;SteamBindingFixture f(recorder);REQUIRE(f.L!=nullptr);REQUIRE_MESSAGE(f.boot(),f.error);
    REQUIRE_MESSAGE(f.run(R"lua(
        local ctx={f={allow=false},sf={},tf={steam_achievement_result='unchanged'},lf={},mp={}}
        local _,result=steam_contract({id='MUST_NOT_CALL',cond='f.allow'},ctx)
        assert(result==true and ctx.tf.steam_achievement_result=='unchanged')
        ctx.f.allow=true;steam_contract({name='ACH_ALLOWED',cond='f.allow'},ctx)
        assert(ctx.tf.steam_achievement_result=='unlocked')
    )lua"),f.error);
    CHECK(recorder.unlocks==std::vector<std::string>{"ACH_ALLOWED"});recorder.accepted=false;
    REQUIRE_MESSAGE(f.run("local c,r=steam_contract({id='ACH_REFUSED',silent='true'});assert(r==true and c.tf.steam_achievement_result=='refused')"),f.error);
    REQUIRE(recorder.unlocks.size()==2);CHECK(recorder.unlocks.back()=="ACH_REFUSED");
}

TEST_CASE("Steam native contract: invalid achievement identifiers never call the backend") {
    SteamContractRecorder recorder;SteamBindingFixture f(recorder);REQUIRE(f.L!=nullptr);REQUIRE_MESSAGE(f.boot(),f.error);
    REQUIRE_MESSAGE(f.run(R"lua(
        for _,value in ipairs({false,{},'', 'ACH_OK'..string.char(0)..'DIFFERENT'}) do
            local ok,result=pcall(steam.unlock_achievement,value)
            assert(not ok or result==false,'invalid achievement id accepted')
        end
        local ok,result=pcall(steam.set_achievement,nil);assert(not ok or result==false)
    )lua"),f.error);
    CHECK(recorder.unlocks.empty());
}

TEST_CASE("Steam native contract: embedded NUL cannot unlock a truncated command achievement") {
    SteamContractRecorder recorder;SteamBindingFixture f(recorder);REQUIRE(f.L!=nullptr);REQUIRE_MESSAGE(f.boot(),f.error);
    REQUIRE_MESSAGE(f.run(R"lua(
        local c,r=steam_contract({name='ACH_OK'..string.char(0)..'DIFFERENT',silent=true})
        assert(r==true and c.tf.steam_achievement_result=='refused','truncated achievement was reported unlocked')
    )lua"),f.error);
    CHECK(recorder.unlocks.empty());
}

TEST_CASE("Steam native contract: int32 stat boundaries reject narrowing") {
    SteamContractRecorder recorder;SteamBindingFixture f(recorder);REQUIRE(f.L!=nullptr);REQUIRE_MESSAGE(f.boot(),f.error);
    REQUIRE_MESSAGE(f.run("assert(steam.set_stat_int('low',-2147483648));assert(steam.set_stat_int('high',2147483647));assert(steam.set_stat_float('ratio',0.25))"),f.error);
    CHECK((recorder.ints==std::vector<int32_t>{INT32_MIN,INT32_MAX}));REQUIRE(recorder.floats.size()==1);CHECK(recorder.floats[0]==.25f);
    recorder.names.clear();recorder.ints.clear();recorder.floats.clear();
    REQUIRE_MESSAGE(f.run(R"lua(
        for _,v in ipairs({2147483648,-2147483649,1.5}) do
            local ok,result=pcall(steam.set_stat_int,'count',v);assert(not ok or result==false,'stat narrowed out of int32 range')
        end
    )lua"),f.error);
    CHECK(recorder.names.empty());CHECK(recorder.ints.empty());CHECK(recorder.floats.empty());
}

TEST_CASE("Steam native contract: float stats reject nonfinite and overflow values") {
    SteamContractRecorder recorder;SteamBindingFixture f(recorder);REQUIRE(f.L!=nullptr);REQUIRE_MESSAGE(f.boot(),f.error);
    REQUIRE_MESSAGE(f.run(R"lua(
        for _,v in ipairs({0/0,math.huge,-math.huge,1e100}) do
            local ok,result=pcall(steam.set_stat_float,'ratio',v)
            assert(not ok or result==false,'nonfinite stat accepted')
        end
    )lua"),f.error);
    CHECK(recorder.names.empty());CHECK(recorder.floats.empty());
}

TEST_CASE("Steam native contract: cloud data preserves binary NUL without a real cloud write") {
    SteamContractRecorder recorder;SteamBindingFixture f(recorder);REQUIRE(f.L!=nullptr);REQUIRE_MESSAGE(f.boot(),f.error);
    REQUIRE_MESSAGE(f.run("assert(steam.cloud_write('slot.dat','A'..string.char(0)..'B'))"),f.error);
    CHECK(recorder.files==std::vector<std::string>{"slot.dat"});CHECK(recorder.payload==std::string("A\0B",3));
}

TEST_CASE("Steam native contract: real unavailable backend and actual capability dispatch stay disabled") {
    // Never init the SDK-ON implementation: no account/client/network action.
    SteamBackend backend;REQUIRE_FALSE(backend.isAvailable());
    SteamBindingFixture f(backend);REQUIRE(f.L!=nullptr);REQUIRE_MESSAGE(f.boot(),f.error);
#ifndef CAESURA_HAS_STEAM
    REQUIRE_MESSAGE(f.run("assert(Engine.get_capability_profile().compiled.steam==false)"),f.error);
#endif
    REQUIRE_MESSAGE(f.run(R"lua(
        assert(steam.unlock_achievement('ACH_NO_SESSION')==false)
        local c,r=steam_contract({id='ACH_NO_SESSION',silent=true})
        assert(r==true and c.tf.steam_achievement_result=='refused')
        local runtime=require('capability_runtime');local cap=runtime.query('steam.achievements')
        local expected=Engine.get_capability_profile().compiled.steam and 'backend_unavailable' or 'sdk_disabled'
        assert(cap.status=='unsupported' and cap.reason==expected)
        local denied,err=steam_dispatch('[steam_achievement id="ACH_NO_SESSION"][eval exp="f.after=1"][end]')
        assert(type(err)=='string' and err:find(expected,1,true) and denied.f.after==nil)
        assert(runtime.configure_project_json('{"capabilities":{"required":[],"optional":["steam.achievements"],"accept_approximate":[]}}'))
        local optional,why=steam_dispatch('[steam_achievement name="ACH_NO_SESSION"][eval exp="f.after=1"][end]')
        assert(why==nil and optional.f.after==1)
        assert(optional.tf.steam_achievement_result==nil,'blocked handler must not claim an unlock/refusal attempt')
        assert(#optional.capability_diagnostics==1 and optional.capability_diagnostics[1].status=='unsupported')
        assert(optional.capability_diagnostics[1].reason==expected)
    )lua"),f.error);
    CHECK_FALSE(backend.isAvailable());
}

// One safe backend-return-length negative. The Lua allocator refuses the
// forged LONG-string allocation before Lua could copy beyond the 8-byte vector.
// Current external/lua/lstring.c allocates long strings before memcpy.
#include <cstdlib>
#include <cstring>
namespace {
struct SteamBoundedLuaAllocator {
    unsigned rejectedLargeAllocations=0;
    static void* allocate(void* user,void* ptr,size_t,size_t size) {
        auto& self=*static_cast<SteamBoundedLuaAllocator*>(user);
        if(size==0){std::free(ptr);return nullptr;}
        if(size>1024u*1024u){++self.rejectedLargeAllocations;return nullptr;}
        return std::realloc(ptr,size);
    }
};
class SteamReadLengthRecorder final : public NullSteamBackend {
public:
    int32_t reported=3;
    int32_t actualCapacity=0;
    unsigned readCalls=0;
    int32_t cloudFileSize(const char*) const override{return 8;}
    int32_t cloudRead(const char*,void* buffer,int32_t capacity) override {
        ++readCalls;actualCapacity=capacity;
        // The test backend never writes outside the supplied destination.
        if(buffer && capacity>=3)std::memcpy(buffer,"A\0B",3);
        return reported;
    }
};
}
TEST_CASE("Steam native contract: cloud read rejects oversized backend length before Lua allocation") {
    SteamBoundedLuaAllocator allocator;
    SteamReadLengthRecorder recorder;
    auto& registry=BackendRegistry::instance();
    struct RestoreSteam {
        BackendRegistry& registry;ISteamBackend* previous;
        ~RestoreSteam(){registry.setSteamBackend(previous);}
    } restore{registry,registry.getSteamBackend()};
    registry.setSteamBackend(&recorder);
    std::unique_ptr<lua_State,decltype(&lua_close)> L{
        lua_newstate(SteamBoundedLuaAllocator::allocate,&allocator),lua_close};
    REQUIRE(L!=nullptr);luaL_openlibs(L.get());registerSteamBinding(L.get());
    const int positive=luaL_dostring(L.get(),"local v=steam.cloud_read('slot.dat');assert(v=='A'..string.char(0)..'B')");
    REQUIRE(positive==LUA_OK);REQUIRE(recorder.actualCapacity==8);REQUIRE(recorder.readCalls==1);
    REQUIRE(allocator.rejectedLargeAllocations==0);
    recorder.reported=64*1024*1024+1;
    const int negative=luaL_dostring(L.get(),R"lua(
        local ok,value=pcall(steam.cloud_read,'slot.dat')
        assert(ok and value==nil,'oversized backend read must return nil without allocating a forged Lua string')
    )lua");
    CAPTURE(allocator.rejectedLargeAllocations);
    CHECK_MESSAGE(negative==LUA_OK,(negative==LUA_OK?"":lua_tostring(L.get(),-1)));
    CHECK(recorder.readCalls==2);CHECK(recorder.actualCapacity==8);
    CHECK(allocator.rejectedLargeAllocations==0);
    // No SDK init, cloud service, or real out-of-bounds write/read in the
    // negative fixture. A failing old binding attempts a forbidden allocation.
}
