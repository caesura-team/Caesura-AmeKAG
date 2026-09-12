// test_demo_e2e.cpp - Demo end-to-end smoke test (S1.2)
#include "doctest.h"
#include "script/vm/LuaManager.h"
#include "script/bindings/KAGBinding.h"
#include "script/bindings/RenderBinding.h"
#include "script/bindings/DevCoreBinding.h"
#include "script/bindings/DebugBinding.h"
#include <fstream>
#include <sstream>
#include <memory>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

using namespace Caesura;

struct DemoLuaDeleter {
    void operator()(LuaManager* manager) const {
        manager->shutdown();
        delete manager;
    }
};

static std::unique_ptr<LuaManager, DemoLuaDeleter> initDemoLua() {
    std::unique_ptr<LuaManager, DemoLuaDeleter> lm(new LuaManager());
    if (!lm->init()) return nullptr;
    lua_State* L = lm->state();
    if (luaL_dostring(L,
        "package.path = 'scripts/?.lua;scripts/?/init.lua;' .. package.path") != LUA_OK) {
        return nullptr;
    }
    registerKAGBinding(L);
    registerRenderBinding(L);
    registerDevCoreBinding(L);
    registerDebugBinding(L);
    return lm;
}

static bool requireModule(lua_State* L, const char* name) {
    lua_getglobal(L, "require");
    lua_pushstring(L, name);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        lua_pop(L, 1);
        return false;
    }
    lua_pop(L, 1);
    return true;
}

static std::string readDemoScript() {
    std::ifstream f("scripts/demo_story.ks", std::ios::binary);
    if (!f.is_open()) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

TEST_CASE("Demo E2E: tokenizer and scheduler modules load") {
    auto lm = initDemoLua();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    CHECK(requireModule(L, "tokenizer"));
    CHECK(requireModule(L, "scheduler"));
}

// Execute the production tokenizer/compiler/scheduler with command stubs in a
// no-host VM. Observed dispatch payloads are the oracle, not native availability.
static void installDemoDriver(lua_State* L) {
    const char* code = R"lua(
        assert(package.loaded['capability_runtime'] == nil,
               'dispatch fixture must select its no-host boundary before capture')
        rawset(Engine, 'get_capability_profile', nil)
        local runtime = require('capability_runtime')
        assert(not runtime.has_host(), 'dispatch fixture has no native backend proof')
        assert(runtime.query('audio.play').proven == false)
        function runDemoWithStubs(script, maxFrames)
            local dispatched = {}
            package.loaded['kag'] = setmetatable({}, {
                __index = function(_, key)
                    return function(_, params)
                        dispatched[#dispatched + 1] = {cmd = key, params = params}
                    end
                end
            })
            local tokens = require('tokenizer').parse(script)
            assert(#tokens > 0, 'demo must contain executable tokens')
            local ctx = {
                f = {}, sf = {}, tf = {}, tokens = tokens, token_index = 1,
                call_stack = {}, layers = {}, backlog = {},
                active_operations = {}, stop_flag = false,
                variables = {}, characters = {}, unlockedCG = {},
                unlockedMusic = {}, seen_scenes = {}, waiting_input = false,
                macros = {}, load_tokens = function() return nil end,
            }
            local co = coroutine.create(function()
                require('scheduler').run(ctx, tokens, 1)
            end)
            local frames = 0
            while coroutine.status(co) ~= 'dead' and frames < maxFrames do
                frames = frames + 1
                local ok, err = coroutine.resume(co, 16)
                assert(ok, err)
            end
            assert(coroutine.status(co) == 'dead', 'demo did not finish')
            assert(ctx.error_command == nil, 'a demo command handler failed')
            return ctx, dispatched, tokens, frames
        end
    )lua";
    REQUIRE(luaL_dostring(L, code) == LUA_OK);
}

TEST_CASE("Demo E2E: scheduler dispatches 10 lines and stops at [end]") {
    auto lm = initDemoLua();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    installDemoDriver(L);

    std::string kag = "*start\n";
    for (int i = 1; i <= 10; ++i) {
        kag += "[ch name=\"T" + std::to_string(i) + "\" text=\"line " + std::to_string(i) + "\"]\n[p]\n";
    }
    kag += "[end]\n[ch text=\"unreachable after end\"]\n";

    const char* code = R"lua(
        local ctx, dispatched, tokens, frames = runDemoWithStubs(..., 50)
        assert(#tokens == 23, 'expected label, ten ch/p pairs, end and sentinel')
        assert(frames > 10, 'scheduler must resume through the token stream')
        assert(#dispatched == 20, 'all ten ch/p pairs must dispatch exactly once')
        for i = 1, 10 do
            local line = dispatched[2 * i - 1]
            assert(line.cmd == 'ch', 'dialogue dispatch order')
            assert(line.params.name == 'T' .. i, 'speaker payload')
            assert(line.params.text == 'line ' .. i, 'dialogue payload')
            assert(dispatched[2 * i].cmd == 'p', 'page-wait dispatch order')
        end
    )lua";
    REQUIRE(luaL_loadstring(L, code) == LUA_OK);
    lua_pushlstring(L, kag.data(), kag.size());
    const int r = lua_pcall(L, 1, 0, 0);
    INFO("Lua error: " << (r != LUA_OK && lua_tostring(L, -1) ? lua_tostring(L, -1) : "none"));
    CHECK(r == LUA_OK);
}

TEST_CASE("Demo E2E: demo_story.ks dispatches dialogue through EOF") {
    auto lm = initDemoLua();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    installDemoDriver(L);

    const std::string script = readDemoScript();
    INFO("Required fixture: scripts/demo_story.ks (test executable working directory)");
    REQUIRE(!script.empty());

    const char* code = R"lua(
        local ctx, dispatched = runDemoWithStubs(..., 8000)
        local found, dialogue = {}, {}
        for _, item in ipairs(dispatched) do
            found[item.cmd] = true
            if item.cmd == 'ch' then dialogue[#dialogue + 1] = item.params end
        end
        for _, cmd in ipairs({'font','pt','bg','wait','fg','position','layopt',
                              'p','text','ruby','reset','playbgm','playse',
                              'stopse','cl','l','er'}) do
            assert(found[cmd], 'demo command never dispatched: ' .. cmd)
        end
        assert(#dialogue > 10, 'demo dialogue must be executed')
        assert(dialogue[1].name == 'Narrator', 'opening speaker')
        assert(dialogue[1].text == 'Welcome to Caesura (AmeKAG).', 'opening dialogue')
        assert(dialogue[#dialogue].text == 'Thanks for trying Caesura (AmeKAG)!',
               'closing dialogue must execute')
        assert(dispatched[#dispatched].cmd == 'er', 'must reach final clear at EOF')
    )lua";
    REQUIRE(luaL_loadstring(L, code) == LUA_OK);
    lua_pushlstring(L, script.data(), script.size());
    const int r = lua_pcall(L, 1, 0, 0);
    INFO("Lua error: " << (r != LUA_OK && lua_tostring(L, -1) ? lua_tostring(L, -1) : "none"));
    CHECK(r == LUA_OK);
}

static std::string readFullPipelineDemo() {
    std::ifstream f("demo/full_pipeline_demo.ks", std::ios::binary);
    if (!f.is_open()) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

TEST_CASE("Demo E2E: full_pipeline_demo.ks parses the complete command surface") {
    auto lm = initDemoLua();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();
    REQUIRE(requireModule(L, "tokenizer"));

    const std::string script = readFullPipelineDemo();
    INFO("Required fixture: demo/full_pipeline_demo.ks (test executable working directory)");
    REQUIRE(!script.empty());

    const char* code =
        "local tokenizer = require('tokenizer'); "
        "local tokens = tokenizer.parse(...); "
        "assert(#tokens > 40, 'full pipeline should produce a rich token stream'); "
        "local names = {}; "
        "for _, t in ipairs(tokens) do "
        "  if type(t) == 'table' and t.cmd then names[t.cmd] = true "
        "  elseif type(t) == 'table' and t.type then names[t.type] = true end "
        "end; "
        "for _, cmd in ipairs({'bg','fg','cl','ch','p','ruby','er','playbgm','playse','playvoice','setbgmvolume','stopbgm','wait','if','jump','eval','iscript','button','endbutton','save','load','trans','quake','vfx','label','end'}) do "
        "  assert(names[cmd], 'missing command in full pipeline: ' .. cmd) "
        "end";
    REQUIRE(luaL_loadstring(L, code) == LUA_OK);
    lua_pushstring(L, script.c_str());
    const int r = lua_pcall(L, 1, 0, 0);
    if (r != LUA_OK) {
        MESSAGE("Lua error: " << (lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown"));
    }
    CHECK(r == LUA_OK);
}
TEST_CASE("Demo E2E: full_pipeline_demo.ks drives to [end] with stub handlers") {
    auto lm = initDemoLua();
    REQUIRE(lm != nullptr);
    lua_State* L = lm->state();

    const std::string script = readFullPipelineDemo();
    INFO("Required fixture: demo/full_pipeline_demo.ks (test executable working directory)");
    REQUIRE(!script.empty());

    installDemoDriver(L);
    const char* code = R"lua(
        local script = ...
        local ctx, dispatched = runDemoWithStubs(
            script .. '\n[ch text="unreachable after end"]\n', 8000)
        local found, dialogue = {}, {}
        for _, item in ipairs(dispatched) do
            found[item.cmd] = true
            if item.cmd == 'ch' then dialogue[item.params.text] = true end
        end
        for _, cmd in ipairs({'font','pt','bg','fg','cl','ch','p','ruby','er',
                              'playbgm','playse','playvoice','setbgmvolume','stopbgm',
                              'wait','button','endbutton','save','load','trans',
                              'quake','vfx'}) do
            assert(found[cmd], 'command never dispatched: ' .. cmd)
        end
        assert(ctx.tf.eval_result == 42, '[eval] must execute')
        assert(ctx.tf.demo_value == 42, '[iscript] must execute')
        assert(dialogue['The condition evaluated to true.'], 'true branch must execute')
        assert(not dialogue['The condition evaluated to false.'], 'false branch must be skipped')
        assert(dialogue['You chose the library route.'], 'route must execute')
        assert(dialogue['Thank you for playing.'], 'ending dialogue must execute')
        assert(not dialogue['unreachable after end'], '[end] must stop execution')
    )lua";
    REQUIRE(luaL_loadstring(L, code) == LUA_OK);
    lua_pushstring(L, script.c_str());
    const int r = lua_pcall(L, 1, 0, 0);
    if (r != LUA_OK) {
        MESSAGE("Lua error: " << (lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown"));
    }
    CHECK(r == LUA_OK);
}
