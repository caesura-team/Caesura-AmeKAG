#pragma once
#include "../api/ILuaManager.h"
struct lua_State;
struct lua_Debug;

namespace Caesura {

class LuaManager : public ILuaManager {
public:
    LuaManager(const LuaManager&) = delete;
    LuaManager& operator=(const LuaManager&) = delete;

    LuaManager() = default;
    ~LuaManager() override;

    lua_State* state() { return m_L; }

    // Lifecycle
    bool init();
    void shutdown();
    void update(float deltaTime);

    // Load and execute a Lua script file
    bool loadScript(const char* path);

    // Resume the KAG coroutine
    void resumeKAGCoroutine();

    // Apply sandbox restrictions (called from main.cpp after init)
    void lockdownScriptEnv();

    // CPU budget for AI-generated scripts (Track 3)
    void setInstructionBudget(int budget) { m_instructionBudget = budget; }
    int  getInstructionBudget() const    { return m_instructionBudget; }
    bool isInstructionBudgetExceeded() const { return m_budgetExceeded; }
    void resetInstructionBudget() { m_instructionCount = 0; m_budgetExceeded = false; }

private:
    static void instructionHook(lua_State* L, lua_Debug* ar);
    void registerModules();

    lua_State* m_L = nullptr;
    bool m_initialized = false;

    int  m_instructionCount = 0;
    // 20M covers one cold tokenize of a ~120KB scene through the pure-Lua
    // LPeg VM (~140 instr/byte, t59 audit); .ksc precompile stays the fast
    // path, and the per-window runaway guard keeps its purpose.
    int  m_instructionBudget = 20000000;
    bool m_budgetExceeded = false;
};

} // namespace Caesura
