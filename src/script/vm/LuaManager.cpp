 extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#include "../vm/LuaManager.h"
#include "../state/GameState.h"
#include "../bindings/KAGBinding.h"
#include "../bindings/EngineBinding.h"
#include "../bindings/RenderBinding.h"
#include "../bindings/DevCoreBinding.h"
#include "../bindings/DebugBinding.h"
// [R8-FIX] UnifiedBinding removed (round 40+1): the _CAESURA_BACKEND proxy is
// superseded by backend.lua (BackendFactory) with direct KAG/Render/DevCore calls.
#include "../bindings/VFXBinding.h"
#include "../bindings/MiniGameBinding.h"
#include "../bindings/SmaBinding.h"
#include "../bindings/SaveBinding.h"
#include "../bindings/RestoreBinding.h"
#include "../bindings/AudioRestoreBinding.h"
#include "../bindings/FontRestoreBinding.h"
#include "../bindings/TransientRestoreBinding.h"
#include "../bindings/SteamBinding.h"
#include "../bindings/AIBinding.h"
#include "../../di/BackendRegistry.h"
#include "../../di/api/ThreadAssert.h"
#include "../../debug/api/DebugLog.h"
#include <cstdio>
#include <limits>
#include <thread>
#include <chrono>

namespace Caesura {

namespace {
char kLuaManagerRegistryKey;
constexpr int kInstructionHookInterval = 10000;

// [galgame-flake-H1] On Windows a transient filesystem open failure (AV scan,
// I/O backpressure, saves/logs flush) can make luaL_dofile return LUA_ERRFILE.
// A 30+-file require chain in scripts/kag/init.lua then aborts the whole load.
// Retry the file once after a short nap; real script errors (syntax/runtime)
// return non-LUA_ERRFILE codes and are never retried.
constexpr int kLoadRetryDelayMs = 4;
} // namespace

// ===========================================================================
//  Track 3: Instruction-count hook for CPU budget enforcement
// ===========================================================================

void LuaManager::instructionHook(lua_State* L, lua_Debug* /*ar*/) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kLuaManagerRegistryKey);
    auto* manager = static_cast<LuaManager*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!manager) return;

    if (manager->m_instructionCount >
        std::numeric_limits<int>::max() - kInstructionHookInterval) {
        manager->m_instructionCount = std::numeric_limits<int>::max();
    } else {
        manager->m_instructionCount += kInstructionHookInterval;
    }

    if (manager->m_instructionCount > manager->m_instructionBudget) {
        manager->m_budgetExceeded = true;
        // Force a Lua error to unwind the stack
        luaL_error(L, "Sandbox: instruction budget exceeded (%d instructions)",
                   manager->m_instructionBudget);
    }
}

bool LuaManager::init() {
    if (m_initialized) return true;
    CAESURA_ASSERT_MAIN_THREAD();

    m_L = luaL_newstate();
    if (!m_L) {
        DEBUG_ERR(SubSys::Scripting, ErrCode::Script_LuaVMCreateFailed,
                  "[Lua] Failed to create Lua state.");
        return false;
    }

    luaL_openlibs(m_L);

    registerModules();

    lua_pushlightuserdata(m_L, this);
    lua_rawsetp(m_L, LUA_REGISTRYINDEX, &kLuaManagerRegistryKey);

    // Track 3: Instruction-count hook for CPU budget (every 10000 instructions)
    lua_sethook(m_L, instructionHook, LUA_MASKCOUNT, kInstructionHookInterval);

    m_initialized = true;
    printf("[Lua] VM initialized.\n");
    return true;
}


void LuaManager::lockdownScriptEnv() {
    // Load sandbox rules from Lua module (human-readable, AI-inspectable)
    // All dangerous globals, module restrictions, and I/O stubs defined there.
    if (luaL_dofile(m_L, "scripts/sandbox.lua") != LUA_OK) {
        DEBUG_ERR(SubSys::Scripting, ErrCode::Script_LoadFailed,
                  "[Lua] Failed to load sandbox.lua: %s",
                  lua_tostring(m_L, -1));
        lua_pop(m_L, 1);
    } else {
        // sandbox.lua returns the Sandbox module table; pop it to keep the stack clean
        lua_pop(m_L, 1);
        printf("[Lua] Script environment locked down (sandbox.lua).\n");
    }
}

LuaManager::~LuaManager() {
    shutdown();
}

void LuaManager::shutdown() {
    if (m_L) {
        if (!GameState::stopRunner(m_L)) {
            fprintf(stderr, "[Lua] Runner cleanup failed during VM shutdown.\n");
        }
        lua_close(m_L);
        m_L = nullptr;
    }
    m_initialized = false;
    printf("[Lua] VM shut down.\n");
}

void LuaManager::update(float /*deltaTime*/) {
    CAESURA_ASSERT_MAIN_THREAD();
    // Event-driven; no polling needed
}

void LuaManager::registerModules() {
    CAESURA_ASSERT_MAIN_THREAD();

    // -- Security: sandboxing moved to lockdownScriptEnv() (called after scripts load) --

    Caesura::engine_binding::registerEngineBindings(m_L);
    registerKAGBinding(m_L);
    registerRenderBinding(m_L);
    registerDevCoreBinding(m_L);
    registerDebugBinding(m_L);
    // registerUnifiedBackendBinding(m_L);  // deprecated, replaced by BackendFactory
    // [R12-FIX] SaveBinding must be registered AFTER KAGBinding (line ~103)
    // because it appends functions to the existing KAG global table.
    // Do not reorder these two calls.
    registerSaveBinding(m_L);
    registerRestoreBinding(m_L);
    registerAudioRestoreBinding(m_L);
    registerFontRestoreBinding(m_L);
    registerTransientRestoreBinding(m_L);
    registerVFXBinding(m_L);
    registerMiniGameBinding(m_L);
    registerSmaBinding(m_L);
    registerAIBinding(m_L);
    registerSteamBinding(m_L);   // P2: unified registration point

    printf("[Lua] Engine (backend selection) module registered.\n");
    // KAGBinding::registerKAGBinding logs its own API count (auto-derived).
    printf("[Lua] Render module registered (via BackendRegistry).\n");
    printf("[Lua] DevCore module registered (via BackendRegistry).\n");
    printf("[Lua] Debug module registered (8 APIs).\n");
}
bool LuaManager::loadScript(const char* path) {
    CAESURA_ASSERT_MAIN_THREAD();
    if (!m_L || !path) return false;
    printf("[Lua] Loading script: %s\n", path);

    const int status = luaL_dofile(m_L, path);
    if (status == LUA_OK) return true;

    // [galgame-flake-H1] LUA_ERRFILE means the file could not be opened (transient
    // on Windows), not a script-level error. Give it one short retry to absorb AV /
    // I/O backpressure. Non-ERRFILE codes are genuine syntax/runtime errors and are
    // never retried — retrying them would only double the failure.
    if (status == LUA_ERRFILE) {
        const char* firstErr = lua_tostring(m_L, -1);
        DEBUG_WARN(SubSys::Scripting, ErrCode::Script_LoadFailed,
                   "[Lua] Open failed for %s (LUA_ERRFILE) retrying: %s",
                   path, firstErr ? firstErr : "(no message)");
        lua_pop(m_L, 1); // clear the stale error message before retrying

        std::this_thread::sleep_for(std::chrono::milliseconds(kLoadRetryDelayMs));

        if (luaL_dofile(m_L, path) == LUA_OK) return true;
    }

    DEBUG_ERR(SubSys::Scripting, ErrCode::Script_LoadFailed,
              "[Lua] Error loading %s: %s", path, lua_tostring(m_L, -1));
    lua_pop(m_L, 1);
    return false;
}

void LuaManager::resumeKAGCoroutine() {
    CAESURA_ASSERT_MAIN_THREAD();
    // Reserved for future use
}
} // namespace Caesura
