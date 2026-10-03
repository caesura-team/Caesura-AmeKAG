#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#include <cstdint>
#include <stdexcept>
extern "C" {
#include <lua.h>
}

namespace Caesura::TestSupport {

struct SoakMemoryObservation {
    uint64_t privateBytes;
    uint64_t rssBytes;
    uint64_t osHandles;
    uint64_t luaBytes;
};

// Native resources owned by Lua userdata may be released by __gc. Finish that
// cleanup before reading either Lua bytes or process counters. These remain
// successive observations, not a claim of globally atomic process/GPU idle.
inline SoakMemoryObservation observeSoakMemory(lua_State* state) {
    if (!state) throw std::runtime_error("Missing VM observation");
    lua_gc(state, LUA_GCCOLLECT, 0);
    const uint64_t luaBytes = uint64_t(lua_gc(state, LUA_GCCOUNT, 0)) * 1024
        + uint64_t(lua_gc(state, LUA_GCCOUNTB, 0));
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    DWORD handles = 0;
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
        throw std::runtime_error("Memory observation failed");
    if (!GetProcessHandleCount(GetCurrentProcess(), &handles))
        throw std::runtime_error("Handle observation failed");
    return {uint64_t(memory.PrivateUsage), uint64_t(memory.WorkingSetSize), handles, luaBytes};
}

} // namespace Caesura::TestSupport
