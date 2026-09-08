#include "doctest.h"
#include "entry/Engine.h"
#include "entry/EngineConfig.h"
#include "di/BackendRegistry.h"
#include "script/api/ILuaManager.h"
#include "storage/api/ISaveManager.h"
#include "TestPaths.h"
#include "EntryLifecycleBackends.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <map>
#include <stdexcept>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

using namespace Caesura;

namespace {

EngineConfig bindingConfig() {
    EngineConfig config;
    config.headless = true;
    return config;
}

// Real composition root, VM registrations, SaveManager and default provider.
// Only the save directory is isolated; no Lua/storage mock replaces the path.
struct SaveBindingFixture {
    TestPaths::ScopedTempDir directory{"save_binding"};
    Engine engine{bindingConfig()};
    ILuaManager* vm = nullptr;
    ISaveManager* saves = nullptr;
    lua_State* state = nullptr;

    SaveBindingFixture() {
        REQUIRE(engine.init());
        vm = BackendRegistry::instance().getLuaManager();
        saves = BackendRegistry::instance().getSaveManager();
        REQUIRE(vm != nullptr);
        REQUIRE(saves != nullptr);
        REQUIRE(saves->getSaveProvider() != nullptr);
        state = vm->state();
        REQUIRE(state != nullptr);
        saves->init(directory.string());
        saves->clearEncryptionKey();
    }

    void run(const std::string& source) {
        vm->resetInstructionBudget();
        const int top = lua_gettop(state);
        const int status = luaL_dostring(state, source.c_str());
        const std::string error = status != LUA_OK && lua_isstring(state, -1)
            ? lua_tostring(state, -1) : "Lua binding assertion failed";
        if (status == LUA_OK) CHECK(lua_gettop(state) == top);
        lua_settop(state, top);
        REQUIRE_MESSAGE(status == LUA_OK, error);
    }

    std::string bytes() const {
        std::ifstream input(directory.path() / "save_9.json", std::ios::binary);
        REQUIRE(input.good());
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    void seed() {
        run("assert(KAG.save_game(9, {marker='original'}, 'binding.ks', 7))");
    }

    void reject(const std::string& value) {
        const auto original = bytes();
        run("local value = " + value + R"lua(
            local ok, err = KAG.save_game(9, value, 'binding.ks', 8)
            assert(ok == false, 'unrepresentable state was accepted')
            assert(type(err) == 'string' and #err > 0, 'failure needs a reason')
            local state, meta = KAG.load_game(9)
            assert(state.marker == 'original' and meta.token_index == 7)
            assert(20 + 22 == 42)
        )lua");
        CHECK(bytes() == original);
    }

    void rejectDiskValue(const json& value) {
        REQUIRE(saves->save(9, value, "binding.ks", 7));
        const auto original = bytes();
        run(R"lua(
            local state, err = KAG.load_game(9)
            assert(state == nil, 'unsupported disk state was accepted')
            assert(type(err) == 'string' and #err > 0)
            assert(20 + 22 == 42)
        )lua");
        CHECK(bytes() == original);
        seed();
        run("assert(KAG.load_game(9).marker == 'original')");
    }
};

class FailingLuaAllocator {
public:
    FailingLuaAllocator(lua_State* state, size_t allowance) : state_(state), remaining_(allowance) {
        original_ = lua_getallocf(state_, &originalData_);
        lua_setallocf(state_, allocate, this);
    }
    ~FailingLuaAllocator() { lua_setallocf(state_, original_, originalData_); }
    size_t rejected = 0;
private:
    static void* allocate(void* context, void* memory, size_t oldSize, size_t newSize) {
        auto& self = *static_cast<FailingLuaAllocator*>(context);
        if (newSize != 0 && (memory == nullptr || newSize > oldSize)) {
            if (self.remaining_ == 0) { ++self.rejected; return nullptr; }
            --self.remaining_;
        }
        return self.original_(self.originalData_, memory, oldSize, newSize);
    }
    lua_State* state_;
    lua_Alloc original_ = nullptr;
    void* originalData_ = nullptr;
    size_t remaining_;
};

struct ThumbnailSaveFixture : SaveBindingFixture {
    ThumbnailSaveFixture() {
        // CMake syncs runtime Lua fixtures into the actual test CWD. Do not
        // infer the source tree from a particular build-directory depth.
        REQUIRE(std::filesystem::is_regular_file("scripts/kag/commands/save.lua"));
        REQUIRE(std::filesystem::is_regular_file("scripts/flow.lua"));
        run("package.path = 'scripts/?.lua;scripts/?/init.lua;' .. package.path");
    }
};

// Only the renderer completion boundary is controlled. The real Lua VM,
// SaveCommands, SaveBinding and SaveManager still perform each disk commit.
class ThumbnailRenderer final : public Test::RenderDevice {
public:
    explicit ThumbnailRenderer(Test::LifecycleProbe& probe) : Test::RenderDevice(probe) {}
    bool reject = false;
    bool throwOnTake = false;
    uint64_t nextId = 1;
    unsigned cancellations = 0;
    unsigned consumptions = 0;
    std::map<uint64_t, ScreenshotResult> pending;
    std::vector<ScreenshotOptions> requested;
    ScreenshotResult requestScreenshot(const ScreenshotOptions& options) override {
        requested.push_back(options);
        if (reject) return {{}, ScreenshotStatus::Failed, 0, 0, 0, {}, "unsupported"};
        ScreenshotResult result{{nextId++, 700}, ScreenshotStatus::Pending};
        result.width = options.width;
        result.height = options.height;
        pending.emplace(result.ticket.requestId, result);
        return result;
    }
    ScreenshotResult takeScreenshot(const ScreenshotTicket& ticket) override {
        if (throwOnTake) { throwOnTake = false; throw std::runtime_error("controlled readback failure"); }
        auto found = pending.find(ticket.requestId);
        if (ticket.generation != 700 || found == pending.end()) return {};
        if (found->second.status == ScreenshotStatus::Pending) return found->second;
        auto result = std::move(found->second);
        pending.erase(found);
        ++consumptions;
        return result;
    }
    bool cancelScreenshot(const ScreenshotTicket& ticket) override {
        auto found = pending.find(ticket.requestId);
        if (ticket.generation != 700 || found == pending.end()
            || found->second.status != ScreenshotStatus::Pending) return false;
        ++cancellations;
        found->second.status = ScreenshotStatus::Cancelled;
        found->second.error = "controlled-cancellation";
        return true;
    }
    void complete(uint64_t id, std::vector<uint8_t> png) {
        auto& result = pending.at(id);
        REQUIRE(result.status == ScreenshotStatus::Pending);
        result.status = ScreenshotStatus::Completed;
        result.frameId = id + 40;
        result.png = std::move(png);
    }
};

struct ThumbnailBoundary {
    Test::LifecycleProbe probe;
    ThumbnailRenderer renderer{probe};
    IRenderDevice* original = BackendRegistry::instance().getRenderDevice();
    ThumbnailBoundary() { BackendRegistry::instance().setRenderDevice(&renderer); }
    ~ThumbnailBoundary() { BackendRegistry::instance().setRenderDevice(original); }
};

void prepareThumbnailSave(SaveBindingFixture& fixture) {
    fixture.run(R"lua(
        u15_save = require('kag.commands.save')
        u15_ctx = {f={route='A',nested={value=1}},sf={},tf={},lf={},mp={},variables={},
            current_scene='tests/scripts/A.ks',token_index=7}
        u15_params = {slot=9,desc='A description'}
        u15_co = coroutine.create(function() u15_save.save(u15_ctx,u15_params) end)
        assert(coroutine.resume(u15_co))
        assert(coroutine.status(u15_co)=='suspended')
    )lua");
}

} // namespace

TEST_CASE("U11 SaveBinding: empty objects survive the real JSON roundtrip") {
    SaveBindingFixture fixture;
    fixture.run(R"lua(
        assert(KAG.save_game(9, {}, 'binding.ks', 1))
        local state = assert(KAG.load_game(9))
        assert(type(state) == 'table' and next(state) == nil)
        assert(KAG.save_game(9, {nested={}, frames={{}, {}}}, 'binding.ks', 2))
        state = assert(KAG.load_game(9))
        assert(type(state.nested) == 'table' and next(state.nested) == nil)
        assert(#state.frames == 2 and next(state.frames[1]) == nil)
        assert(type(state.frames[2]) == 'table' and next(state.frames[2]) == nil)
    )lua");
    CHECK(json::parse(fixture.bytes()).at("data").at("nested").is_object());
}

TEST_CASE("U11 SaveBinding: dense arrays preserve integer index order") {
    SaveBindingFixture fixture;
    fixture.run(R"lua(
        local values = {}
        for i=96,1,-1 do values[i] = {index=i, enabled=i%2==0} end
        assert(KAG.save_game(9, values, 'binding.ks', 3))
        local restored = assert(KAG.load_game(9))
        assert(#restored == 96)
        for i=1,96 do
            assert(restored[i].index == i and restored[i].enabled == (i%2==0))
        end
    )lua");
    const auto disk = json::parse(fixture.bytes()).at("data");
    REQUIRE(disk.is_array());
    for (size_t i = 0; i < disk.size(); ++i) CHECK(disk[i].at("index") == i + 1);
}

TEST_CASE("U11 SaveBinding: NUL and Unicode object keys and values retain their lengths") {
    SaveBindingFixture fixture;
    fixture.run(R"lua(
        local zero = string.char(0)
        local key = 'key' .. zero .. 'suffix'
        local value = 'left' .. zero .. 'right' .. '\u{4e2d}\u{1f600}'
        assert(KAG.save_game(9, {[key]=value, plain='ok'}, value, 4, value))
        local restored, meta = KAG.load_game(9)
        assert(restored)
        assert(restored[key] == value and #restored[key] == #value)
        assert(restored.key == nil and restored.plain == 'ok')
        assert(meta.scene == value, 'loaded scene metadata was truncated')
        local listed = KAG.list_saves()
        assert(#listed == 1 and listed[1].scene == value, 'listed scene metadata was truncated')
    )lua");
    const auto envelope = json::parse(fixture.bytes());
    CHECK(envelope.at("scene") == envelope.at("data").begin().value());
    CHECK(envelope.at("thumbnail") == envelope.at("scene"));
}

TEST_CASE("U11 SaveBinding: shared acyclic tables are copied as independent JSON values") {
    SaveBindingFixture fixture;
    fixture.run(R"lua(
        local shared = {answer=42}
        assert(KAG.save_game(9, {a=shared, b=shared}, 'binding.ks', 5))
        local restored = assert(KAG.load_game(9))
        assert(restored.a.answer == 42 and restored.b.answer == 42)
        restored.a.answer = 0
        assert(restored.b.answer == 42)
    )lua");
}

TEST_CASE("U11 SaveBinding: invalid table keys cannot overwrite an existing slot") {
    SaveBindingFixture fixture;
    fixture.seed();
    for (const char* value : {
             "{[2]='second', named='value'}", "{[1]='first', named='value'}",
             "{[2]='sparse'}", "{[0]='zero'}", "{[-1]='negative'}",
             "{[1.5]='fraction'}", "{[true]='boolean'}", "{[{}]='table'}"}) {
        CAPTURE(value);
        fixture.reject(value);
    }
}

TEST_CASE("U11 SaveBinding: unsupported values and cycles fail without damaging the VM or slot") {
    SaveBindingFixture fixture;
    fixture.seed();
    for (const char* value : {
             "{value=function() end}", "{value=coroutine.create(function() end)}",
             "{value=math.huge}", "{value=-math.huge}", "{value=0/0}",
             "(function() local t={} t.self=t return t end)()",
             "(function() local a,b={},{} a.b=b b.a=a return a end)()"}) {
        CAPTURE(value);
        fixture.reject(value);
    }
    // Full userdata is supplied by the host without replacing any binding.
    lua_newuserdatauv(fixture.state, 1, 0);
    lua_setglobal(fixture.state, "u11_test_userdata");
    fixture.reject("{value=u11_test_userdata}");
    lua_pushnil(fixture.state);
    lua_setglobal(fixture.state, "u11_test_userdata");
}

TEST_CASE("U11 SaveBinding: invalid UTF-8 fails cleanly before writing") {
    SaveBindingFixture fixture;
    fixture.seed();
    fixture.reject("{value=string.char(0xff)}");
    fixture.reject("{[string.char(0xc0, 0x80)]='invalid key'}");
    const auto original = fixture.bytes();
    fixture.run(R"lua(
        local invalid = 'prefix' .. string.char(0, 0xff)
        for _, args in ipairs({{invalid, ''}, {'binding.ks', invalid}}) do
            local ok, err = KAG.save_game(9, {marker='replacement'}, args[1], 8, args[2])
            assert(ok == false and type(err) == 'string', 'invalid metadata was accepted')
        end
    )lua");
    CHECK(fixture.bytes() == original);
}

TEST_CASE("U11 SaveBinding: the supported depth boundary roundtrips and excess depth is rejected") {
    SaveBindingFixture fixture;
    fixture.run(R"lua(
        local root, cursor = {}, nil
        cursor = root
        for i=1,64 do cursor.child={} cursor=cursor.child end
        cursor.leaf = 'depth-64'
        assert(KAG.save_game(9, root, 'binding.ks', 6))
        local restored = assert(KAG.load_game(9))
        for i=1,64 do restored=assert(restored.child) end
        assert(restored.leaf == 'depth-64')
    )lua");
    fixture.seed();
    fixture.reject(R"lua((function()
        local root, cursor = {}, nil
        cursor = root
        for i=1,65 do cursor.child={} cursor=cursor.child end
        return root
    end)())lua");
}

TEST_CASE("U11 SaveBinding: unrepresentable disk JSON is rejected with a healthy Lua stack") {
    SaveBindingFixture fixture;
    SUBCASE("scalar root cannot become a fabricated empty table") {
        fixture.rejectDiskValue(json("not a state table"));
    }
    SUBCASE("legacy object null keeps the established absent-key mapping") {
        // Historical bindings encoded empty state tables as object nulls.
        // The state codec supplies known-field defaults after native loading.
        REQUIRE(fixture.saves->save(9, json{{"sf", nullptr}, {"f", {{"route", "old"}}}},
                                   "binding.ks", 7));
        fixture.run(R"lua(
            local state = assert(KAG.load_game(9))
            assert(state.sf == nil and state.f.route == 'old')
        )lua");
    }
    SUBCASE("array null cannot become a hole") {
        fixture.rejectDiskValue(json::array({1, nullptr, 3}));
    }
    SUBCASE("unsigned integer must fit Lua integer") {
        fixture.rejectDiskValue(json{{"integer", std::numeric_limits<uint64_t>::max()}});
    }
    SUBCASE("deep input cannot consume an unrelated Lua stack value") {
        json nested = json::object();
        for (int i = 0; i < 65; ++i) nested = json{{"child", std::move(nested)}};
        fixture.rejectDiskValue(nested);
    }
}

TEST_CASE("U11 SaveBinding: Lua allocation failure during load and listing leaves the slot reusable") {
    SaveBindingFixture fixture;
    fixture.run(R"lua(
        local values = {}
        for i=1,32 do values[i]={index=i,text=string.rep(tostring(i),32)} end
        assert(KAG.save_game(9, {marker='original',values=values}, 'allocation.ks', 7))
    )lua");
    const auto original = fixture.bytes();
    for (const char* function : {"load_game", "list_saves"}) {
        size_t failures = 0;
        for (size_t allowance = 0; allowance < 40; ++allowance) {
            CAPTURE(function);
            CAPTURE(allowance);
            lua_gc(fixture.state, LUA_GCCOLLECT);
            REQUIRE(lua_checkstack(fixture.state, 16));
            lua_getglobal(fixture.state, "KAG");
            lua_getfield(fixture.state, -1, function);
            lua_remove(fixture.state, -2);
            const bool loading = std::string(function) == "load_game";
            if (loading) lua_pushinteger(fixture.state, 9);
            int result = LUA_OK;
            size_t rejected = 0;
            {
                FailingLuaAllocator allocator(fixture.state, allowance);
                result = lua_pcall(fixture.state, loading ? 1 : 0, 2, 0);
                rejected = allocator.rejected;
            }
            if (rejected != 0) ++failures;
            CHECK((result == LUA_ERRMEM || result == LUA_OK));
            if (result == LUA_OK) {
                CHECK((lua_istable(fixture.state, -2) || lua_isnil(fixture.state, -2)));
            }
            lua_settop(fixture.state, 0);
            CHECK(fixture.bytes() == original);
            fixture.run("local s=assert(KAG.load_game(9)); assert(s.marker=='original' and #s.values==32)");
        }
        CHECK(failures > 1);
    }
}

TEST_CASE("U15 SaveBinding: capture yields until its own result and freezes the actual slot envelope") {
    ThumbnailSaveFixture fixture;
    ThumbnailBoundary boundary;
    prepareThumbnailSave(fixture);
    CHECK_FALSE(fixture.saves->slotExists(9));
    REQUIRE(boundary.renderer.pending.size() == 1);
    REQUIRE(boundary.renderer.requested.size() == 1);
    CHECK(boundary.renderer.requested[0].width == 320);
    CHECK(boundary.renderer.requested[0].height == 180);
    fixture.run(R"lua(
        u15_ctx.f.route, u15_ctx.f.nested.value = 'B', 2
        u15_ctx.current_scene, u15_ctx.token_index, u15_params.slot = 'tests/scripts/B.ks',99,10
        assert(coroutine.resume(u15_co, 0.016, 'ignored resume value'))
        assert(coroutine.status(u15_co)=='suspended')
    )lua");
    CHECK_FALSE(fixture.saves->slotExists(9));
    // Distinct binary payloads stand in only at the readback result boundary.
    // Real PNG encoding/pixels are covered by the renderer integration suite.
    boundary.renderer.complete(1, {0, 255, 'A'});
    fixture.run(R"lua(
        assert(coroutine.resume(u15_co, 0.032, {extra=true}))
        assert(coroutine.status(u15_co)=='dead')
        assert(u15_ctx.tf.save_result=='ok' and u15_ctx.tf.thumbnail_result=='completed')
        assert(#u15_ctx.active_operations==0 and next(u15_ctx._pending_save_slots)==nil)
    )lua");
    const auto disk = json::parse(fixture.bytes());
    CHECK(disk.at("scene") == "tests/scripts/A.ks");
    CHECK(disk.at("token_index") == 7);
    CHECK(disk.at("data").at("f").at("route") == "A");
    CHECK(disk.at("data").at("f").at("nested").at("value") == 1);
    CHECK(disk.at("data").at("description") == "A description");
    CHECK(disk.at("thumbnail") == "AP9B");
    CHECK_FALSE(fixture.saves->slotExists(10));
    CHECK(boundary.renderer.pending.empty());
    CHECK(boundary.renderer.consumptions == 1);
    CHECK(boundary.renderer.cancellations == 0);
    CHECK(boundary.probe.advanceCalls == 0);
}

TEST_CASE("U15 SaveBinding: delayed captures cannot cross pair slots or frozen scenes") {
    ThumbnailSaveFixture fixture;
    ThumbnailBoundary boundary;
    prepareThumbnailSave(fixture);
    fixture.run(R"lua(
        u15_ctx.f.route, u15_ctx.current_scene = 'B','tests/scripts/B.ks'
        u15_b = coroutine.create(function() u15_save.save(u15_ctx,{slot=10}) end)
        assert(coroutine.resume(u15_b))
        assert(coroutine.status(u15_b)=='suspended')
    )lua");
    REQUIRE(boundary.renderer.pending.size() == 2);
    boundary.renderer.complete(2, {'B', 0, 255});
    fixture.run("assert(coroutine.resume(u15_b)); assert(coroutine.status(u15_b)=='dead')");
    SaveMeta b;
    CHECK(fixture.saves->load(10, &b).at("f").at("route") == "B");
    CHECK(b.sceneName == "tests/scripts/B.ks");
    CHECK(b.thumbnail == "QgD/");
    CHECK_FALSE(fixture.saves->slotExists(9));
    boundary.renderer.complete(1, {'A'});
    fixture.run("assert(coroutine.resume(u15_co)); assert(coroutine.status(u15_co)=='dead')");
    SaveMeta a;
    CHECK(fixture.saves->load(9, &a).at("f").at("route") == "A");
    CHECK(a.sceneName == "tests/scripts/A.ks");
    CHECK(a.thumbnail == "QQ==");
    CHECK(boundary.renderer.pending.empty());
    CHECK(boundary.renderer.consumptions == 2);
}

TEST_CASE("U15 SaveBinding: cancellation and close consume their terminal and prevent late saves") {
    ThumbnailSaveFixture fixture;
    ThumbnailBoundary boundary;
    prepareThumbnailSave(fixture);
    bool completedFirst = false;
    SUBCASE("explicit coroutine close") {
        fixture.run("assert(coroutine.close(u15_co))");
    }
    SUBCASE("Operation cancel without closing wakes as cancelled") {
        fixture.run(R"lua(
            require('kag.operation').cancel_all(u15_ctx)
            assert(coroutine.resume(u15_co))
            assert(coroutine.status(u15_co)=='dead' and u15_ctx.tf.save_error=='save-cancelled')
        )lua");
    }
    SUBCASE("completion wins but closing still discards the completed image") {
        completedFirst = true;
        boundary.renderer.complete(1, {'A'});
        fixture.run("assert(coroutine.close(u15_co))");
    }
    SUBCASE("completion wins but cancellation still prevents commit") {
        completedFirst = true;
        boundary.renderer.complete(1, {'A'});
        fixture.run(R"lua(
            require('kag.operation').cancel_all(u15_ctx)
            assert(coroutine.resume(u15_co))
            assert(u15_ctx.tf.save_error=='save-cancelled')
        )lua");
    }
    SUBCASE("renderer cancellation is not an optional-image failure") {
        REQUIRE(boundary.renderer.cancelScreenshot({1,700}));
        fixture.run("assert(coroutine.resume(u15_co)); assert(u15_ctx.tf.save_error=='save-cancelled')");
    }
    CHECK_FALSE(fixture.saves->slotExists(9));
    CHECK(boundary.renderer.pending.empty());
    CHECK(boundary.renderer.consumptions == 1);
    CHECK(boundary.renderer.cancellations == (completedFirst ? 0 : 1));
    fixture.run("collectgarbage('collect'); assert(#u15_ctx.active_operations==0)");
    CHECK(boundary.renderer.consumptions == 1);
    CHECK(boundary.probe.advanceCalls == 0);
}

TEST_CASE("U15 SaveBinding: optional failures and nonyieldable capture retire requests without pumping") {
    ThumbnailSaveFixture fixture;
    ThumbnailBoundary boundary;
    SUBCASE("nonyieldable owner returns explicit reason and leaves no pending request") {
        fixture.run(R"lua(
            local image,status,reason=KAG.capture_thumbnail()
            assert(image==nil and status=='unavailable' and reason=='not-yieldable')
            assert(require('kag.commands.save').save({f={},sf={},tf={},current_scene='direct.ks'}, {slot=9}))
        )lua");
        CHECK(boundary.renderer.cancellations == 2);
        CHECK(boundary.renderer.consumptions == 2);
        SaveMeta meta;
        REQUIRE(fixture.saves->load(9, &meta).is_object());
        CHECK(meta.thumbnail.empty());
    }
    SUBCASE("missing renderer and unsupported renderer remain explicit") {
        BackendRegistry::instance().setRenderDevice(nullptr);
        fixture.run(R"lua(
            local image,status,reason=KAG.capture_thumbnail()
            assert(image==nil and status=='unavailable' and reason=='renderer-unavailable')
        )lua");
        BackendRegistry::instance().setRenderDevice(&boundary.renderer);
        boundary.renderer.reject = true;
        fixture.run(R"lua(
            local image,status,reason=KAG.capture_thumbnail()
            assert(image==nil and status=='unavailable' and reason=='unsupported')
        )lua");
        CHECK(boundary.renderer.consumptions == 0);
    }
    SUBCASE("readback failure falls back to a single actual state-only save") {
        prepareThumbnailSave(fixture);
        auto& result = boundary.renderer.pending.at(1);
        result.status = ScreenshotStatus::Failed;
        result.error = "encode-failed";
        fixture.run(R"lua(
            assert(coroutine.resume(u15_co))
            assert(u15_ctx.tf.save_result=='ok' and u15_ctx.tf.thumbnail_result=='failed')
            assert(u15_ctx.tf.thumbnail_error=='encode-failed')
        )lua");
        CHECK(json::parse(fixture.bytes()).at("thumbnail") == "");
        CHECK(boundary.renderer.consumptions == 1);
    }
    SUBCASE("C++ boundary failure cannot escape Lua and cannot strand a request") {
        boundary.renderer.throwOnTake = true;
        fixture.run(R"lua(
            local image,status,reason=KAG.capture_thumbnail()
            assert(image==nil and status=='failed' and reason=='controlled readback failure')
        )lua");
        CHECK(boundary.renderer.cancellations == 1);
    }
    SUBCASE("explicit image bypasses automatic capture and preserves save payload") {
        fixture.run(R"lua(
            assert(require('kag.commands.save').save({f={},sf={},tf={},current_scene='explicit.ks'},
                {slot=9,thumbnail='explicit-base64'}))
        )lua");
        CHECK(boundary.renderer.requested.empty());
        CHECK(json::parse(fixture.bytes()).at("thumbnail") == "explicit-base64");
    }
    CHECK(boundary.renderer.pending.empty());
    CHECK(boundary.probe.advanceCalls == 0);
}

TEST_CASE("U15 SaveBinding: missing owner during a pending capture cancels instead of publishing") {
    ThumbnailSaveFixture fixture;
    ThumbnailBoundary boundary;
    prepareThumbnailSave(fixture);
    // The old renderer's teardown owns its records. The guard must re-resolve
    // Registry and never call the retired pointer when the owner disappears.
    boundary.renderer.pending.clear();
    BackendRegistry::instance().setRenderDevice(nullptr);
    fixture.run(R"lua(
        assert(coroutine.resume(u15_co))
        assert(coroutine.status(u15_co)=='dead' and u15_ctx.tf.save_error=='save-cancelled')
    )lua");
    CHECK_FALSE(fixture.saves->slotExists(9));
    CHECK(boundary.renderer.cancellations == 0);
    CHECK(boundary.renderer.consumptions == 0);
}

TEST_CASE("U15 SaveBinding: Lua allocation failures close admitted screenshot ownership") {
    ThumbnailSaveFixture fixture;
    ThumbnailBoundary boundary;
    fixture.run(R"lua(
        u15_co=coroutine.create(function() return KAG.capture_thumbnail() end)
        assert(coroutine.resume(u15_co))
        assert(coroutine.status(u15_co)=='suspended')
    )lua");
    REQUIRE(boundary.renderer.pending.size() == 1);
    boundary.renderer.complete(1, std::vector<uint8_t>(1024, 0xf1));
    lua_getglobal(fixture.state, "u15_co");
    lua_State* coroutine = lua_tothread(fixture.state, -1);
    REQUIRE(coroutine != nullptr);
    int count = 0;
    int status;
    {
        FailingLuaAllocator allocator(fixture.state, 0);
        status = lua_resume(coroutine, fixture.state, 0, &count);
    }
    CHECK(status == LUA_ERRMEM);
    lua_pop(fixture.state, 1);
    // Lua 5.4 errored coroutines retain tbc slots until their owner closes them.
    fixture.run("coroutine.close(u15_co); u15_co=nil; collectgarbage('collect')");
    CHECK(boundary.renderer.pending.empty());
    CHECK(boundary.renderer.consumptions == 1);
    CHECK(boundary.renderer.cancellations == 0);
    fixture.run("assert(KAG.save_game(9,{healthy=true},'healthy.ks',1))");
}

TEST_CASE("U15 SaveBinding: real runner stop reload and replacement close pending native captures") {
    ThumbnailSaveFixture fixture;
    ThumbnailBoundary boundary;
    fixture.run(R"lua(
        -- Only scene bytes are supplied here; tokenizer/compiler, runner,
        -- scheduler, SaveCommands, native capture and save bindings stay real.
        local flow = require('flow')
        local function scene(path, source)
            local tokens = require('tokenizer').parse(source)
            require('kag.compiler').compile(tokens)
            return {path=path,tokens=tokens,labels=tokens._compiled.labels}
        end
        flow.load_scene = function(path)
            return scene(path,path=='A.ks' and '[save slot=9][wait time=60000][end]'
                or '[wait time=60000][end]')
        end
        flow.reload_scene = function(path) return scene(path,'[wait time=60000][end]') end
        require('kag')
        u15_runner = require('kag_runner')
        u15_runner.set_resume_adapter({is_paused=function() return false end,
            resume=function(_,co,value) return coroutine.resume(co,value) end})
        assert(u15_runner.start('A.ks'))
        u15_owner = u15_runner.get_ctx()
        for i=1,8 do
            if u15_owner._pending_save_slots and u15_owner._pending_save_slots[9] then break end
            u15_runner.update(0)
        end
        assert(u15_owner._pending_save_slots and u15_owner._pending_save_slots[9])
        u15_old_co = u15_owner.co
        assert(coroutine.status(u15_old_co)=='suspended')
    )lua");
    REQUIRE(boundary.renderer.pending.size() == 1);
    SUBCASE("stop") { fixture.run("assert(u15_runner.stop())"); }
    SUBCASE("reload") { fixture.run("assert(u15_runner.reload_scene('A.ks'))"); }
    SUBCASE("replacement") { fixture.run("assert(u15_runner.start('B.ks',{replace=true}))"); }
    CHECK(boundary.renderer.pending.empty());
    CHECK(boundary.renderer.cancellations == 1);
    CHECK(boundary.renderer.consumptions == 1);
    CHECK_FALSE(fixture.saves->slotExists(9));
    fixture.run(R"lua(
        assert(coroutine.status(u15_old_co)=='dead')
        assert(not coroutine.resume(u15_old_co))
        assert(#u15_owner.active_operations==0)
        for i=1,3 do u15_runner.update(0) end
        assert(u15_runner.stop())
    )lua");
    CHECK_FALSE(fixture.saves->slotExists(9));
    CHECK(boundary.renderer.pending.empty());
}
