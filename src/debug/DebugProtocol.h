// ===========================================================================
//  Caesura (AmeKAG) -- DebugProtocol.h
//  Phase 8.2: Lua debug hooks for breakpoints, stepping, and inspection.
//  Uses lua_sethook with LUA_MASKLINE for breakpoint detection.
// ===========================================================================

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

struct lua_State;
struct lua_Debug;

namespace Caesura {

class HotReload;

class DebugProtocol {
public:
    enum class Command {
        Continue,
        StepInto,
        StepOver,
        StepOut,
    };

    enum class RunState {
        Detached,
        Running,
        Paused,
        ResumePending,
    };

    using PauseId = std::uint64_t;
    using CommandSink = std::function<bool(PauseId, Command)>;
    static constexpr int NoResume = -1;
    static constexpr PauseId NoPause = 0;

    struct ResumeOutcome {
        int status = NoResume;
        int resultCount = 0;
        std::string error;
    };

    // The reload monitor must outlive this object. An attached Lua state must
    // remain alive until shutdown() or destruction restores its original hook.
    explicit DebugProtocol(HotReload& hotReload) noexcept;
    ~DebugProtocol();

    DebugProtocol(const DebugProtocol&) = delete;
    DebugProtocol& operator=(const DebugProtocol&) = delete;

    // Attach to the main Lua state after its safety hook has been installed and
    // before debuggable child coroutines are created. Existing coroutines keep
    // the hook configuration they inherited at creation time. init()/shutdown()
    // must run on the Lua owner thread while Lua is stopped.
    bool init(lua_State* L);
    // Returns false without touching Lua when called from a non-owner thread.
    // Destruction of an attached protocol on the wrong thread terminates.
    bool shutdown();

    // Breakpoints use UTF-8 source identifiers, as supplied by Lua/editor JSON.
    // This is a lexical operation: Lua's source marker is removed, separators
    // and path segments are normalized, and Windows-style paths are ASCII
    // case-folded. Relative identifiers remain relative for display.
    static std::string canonicalSourceId(const std::string& source);

    // Relative breakpoint paths are resolved against the working directory
    // captured by init(), so editor absolute paths and Lua relative chunk names
    // identify the same source without changing currentSource()'s display form.
    void setBreakpoint(const std::string& file, int line);
    void removeBreakpoint(const std::string& file, int line);
    void clearAllBreakpoints();
    bool hasBreakpoint(const std::string& file, int line) const;

    // The returned sink is safe to retain on a transport thread. It owns no
    // DebugProtocol or lua_State pointer. Commands must carry currentPauseId();
    // stale pause generations and commands after shutdown are rejected.
    CommandSink commandSink() const;

    // Drain transport commands on the Lua owner thread. A resume command only
    // changes the state to ResumePending; the host explicitly resumes below.
    void pumpCommands();
    bool stepOver();
    bool stepInto();
    bool stepOut();
    bool continue_();

    // Resume the coroutine stopped by the hook. Returns a Lua status code, or
    // NoResume if no command is pending. Must run on the Lua owner thread.
    int resumePausedCoroutine(int* resultCount = nullptr);

    // Production-host entry point. Unlike the compatibility method above, it
    // captures a Lua error before removing all results produced by this resume
    // from the coroutine stack. No Lua state escapes through the outcome.
    ResumeOutcome resumePausedCoroutineManaged();

    // Inspection
    std::string inspectLocal(int frameIndex, const std::string& name);
    std::string inspectGlobal(const std::string& name);
    std::string currentSource() const;
    int currentLine() const;
    PauseId currentPauseId() const;
    bool isDebugActive() const;
    RunState runState() const;
    std::uint64_t nonYieldableHitCount() const;

private:
    struct CommandMailbox;

    // Hook callback registered via lua_sethook. The active instance is resolved
    // from a private entry in the Lua registry shared by all VM coroutines.
    static void hookCallback(lua_State* L, lua_Debug* ar);
    bool onLineHook(lua_State* L, lua_Debug* ar);
    bool enqueueCommand(Command command);
    bool applyCommand(Command command);
    ResumeOutcome resumePausedCoroutineImpl(bool clearResults);
    bool isOwnerThreadLocked() const noexcept;

    // Format a Lua value at the given stack index
    static std::string formatValue(lua_State* L, int index);

    lua_State* m_L = nullptr;
    lua_State* m_pausedL = nullptr;
    HotReload& m_hotReload;
    bool m_initialized = false;
    std::thread::id m_ownerThread;
    std::shared_ptr<CommandMailbox> m_mailbox;
    std::string m_sourceRoot;

    // Breakpoint storage: "file:line" → set
    std::unordered_set<std::string> m_breakpoints;

    // Current debug state
    mutable std::mutex m_stateMutex;
    RunState m_runState = RunState::Detached;
    std::string m_currentSource;
    int m_currentLine = 0;
    int m_pauseDepth = 0;
    PauseId m_pauseId = NoPause;
    PauseId m_nextPauseId = 1;
    std::uint64_t m_nonYieldableHits = 0;

    // Stepping state
    enum class StepMode { None, Into, Over, Out };
    StepMode m_stepMode = StepMode::None;
    int m_stepDepth = 0;
    lua_State* m_stepThread = nullptr;
};

} // namespace Caesura
