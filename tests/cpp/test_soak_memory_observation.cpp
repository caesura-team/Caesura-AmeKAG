#include "doctest.h"
#ifdef _WIN32
#include "../probes/SoakMemoryObservation.h"
#include <memory>
extern "C" {
#include <lauxlib.h>
}

namespace {
constexpr size_t blockBytes = 80ull * 1024 * 1024;
struct FinalizerState { unsigned released = 0; bool failed = false; };
struct NativeBlock { void* pointer; FinalizerState* state; };

int releaseNativeBlock(lua_State* state) {
    auto* block = static_cast<NativeBlock*>(lua_touserdata(state, 1));
    if (block && block->pointer) {
        if (VirtualFree(block->pointer, 0, MEM_RELEASE)) ++block->state->released;
        else block->state->failed = true;
        block->pointer = nullptr;
    }
    return 0;
}

uint64_t processPrivateBytes() {
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    REQUIRE(GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) != 0);
    return memory.PrivateUsage;
}

void* addNativeBlock(lua_State* state, FinalizerState& finalizers, bool retained) {
    lua_gc(state, LUA_GCSTOP, 0);
    auto* block = static_cast<NativeBlock*>(lua_newuserdatauv(state, sizeof(NativeBlock), 0));
    block->pointer = nullptr;
    block->state = &finalizers;
    if (luaL_newmetatable(state, "U27.NativeMemoryControl")) {
        lua_pushcfunction(state, releaseNativeBlock);
        lua_setfield(state, -2, "__gc");
    }
    lua_setmetatable(state, -2);
    block->pointer = VirtualAlloc(nullptr, blockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    REQUIRE(block->pointer != nullptr);
    auto* bytes = static_cast<volatile unsigned char*>(block->pointer);
    for (size_t offset = 0; offset < blockBytes; offset += 4096) bytes[offset] = 1;
    void* address = block->pointer;
    if (retained) lua_setglobal(state, "u27_retained_memory_control");
    else lua_pop(state, 1);
    return address;
}

bool regionIsFree(void* address) {
    MEMORY_BASIC_INFORMATION memory{};
    return VirtualQuery(address, &memory, sizeof(memory)) == sizeof(memory)
        && memory.State == MEM_FREE;
}
}

TEST_CASE("U27 observation: native Lua finalizers precede process memory counters") {
    FinalizerState finalizers;
    std::unique_ptr<lua_State, decltype(&lua_close)> state(luaL_newstate(), lua_close);
    REQUIRE(state != nullptr);
    auto* L = state.get();
    const auto baseline = Caesura::TestSupport::observeSoakMemory(L);
    void* transient = addNativeBlock(L, finalizers, false);
    const auto before = processPrivateBytes();
    REQUIRE(before >= baseline.privateBytes + blockBytes - 1024 * 1024);

    const auto observed = Caesura::TestSupport::observeSoakMemory(L);
    const auto after = processPrivateBytes();
    CHECK(finalizers.released == 1);
    CHECK_FALSE(finalizers.failed);
    CHECK(regionIsFree(transient));
    CHECK(before >= after + blockBytes - 1024 * 1024);
    CHECK(observed.privateBytes <= after + 4 * 1024 * 1024);

    const auto retainedBaseline = processPrivateBytes();
    void* retained = addNativeBlock(L, finalizers, true);
    const auto held = Caesura::TestSupport::observeSoakMemory(L);
    CHECK(finalizers.released == 1);
    CHECK_FALSE(regionIsFree(retained));
    // The original long-run growth budget still rejects a genuinely held block.
    CHECK(held.privateBytes > retainedBaseline + 64 * 1024 * 1024);
    CHECK(processPrivateBytes() > retainedBaseline + 64 * 1024 * 1024);

    lua_pushnil(L);
    lua_setglobal(L, "u27_retained_memory_control");
    Caesura::TestSupport::observeSoakMemory(L);
    CHECK(finalizers.released == 2);
    CHECK_FALSE(finalizers.failed);
    CHECK(regionIsFree(retained));
}
#endif
