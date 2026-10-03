// ===========================================================================
//  Caesura (AmeKAG) -- DebugProtocol.cpp
//  Lua debug hooks for non-blocking breakpoints, stepping, and inspection.
// ===========================================================================

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include "DebugProtocol.h"
#include "HotReload.h"
#include "DebugManager.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <new>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace Caesura {

namespace {

char kDebugHookContextRegistryKey;
constexpr std::uint32_t kDebugHookContextMagic = 0x43444247U;

struct DebugHookContext {
    std::uint32_t magic = kDebugHookContextMagic;
    DebugProtocol* activeProtocol = nullptr;
    lua_Hook previousHook = nullptr;
    int previousMask = 0;
    int previousCount = 0;
};

DebugHookContext* findHookContext(lua_State* L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kDebugHookContextRegistryKey);
    auto* context = static_cast<DebugHookContext*>(lua_touserdata(L, -1));
    if (!context || context->magic != kDebugHookContextMagic) {
        context = nullptr;
    }
    lua_pop(L, 1);
    return context;
}

DebugHookContext* createHookContext(lua_State* L) {
    void* storage = lua_newuserdatauv(L, sizeof(DebugHookContext), 1);
    auto* context = new (storage) DebugHookContext{};
    lua_pushnil(L);
    lua_setiuservalue(L, -2, 1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kDebugHookContextRegistryKey);
    return context;
}

bool pushHookContext(lua_State* L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kDebugHookContextRegistryKey);
    auto* context = static_cast<DebugHookContext*>(lua_touserdata(L, -1));
    if (!context || context->magic != kDebugHookContextMagic) {
        lua_pop(L, 1);
        return false;
    }
    return true;
}

void anchorPausedThread(lua_State* L) {
    if (!pushHookContext(L)) return;
    lua_pushthread(L);
    lua_setiuservalue(L, -2, 1);
    lua_pop(L, 1);
}

void clearPausedThreadAnchor(lua_State* L) {
    if (!L || !pushHookContext(L)) return;
    lua_pushnil(L);
    lua_setiuservalue(L, -2, 1);
    lua_pop(L, 1);
}

int maskForHookEvent(int event) noexcept {
    switch (event) {
        case LUA_HOOKCALL:
        case LUA_HOOKTAILCALL:
            return LUA_MASKCALL;
        case LUA_HOOKRET:
            return LUA_MASKRET;
        case LUA_HOOKLINE:
            return LUA_MASKLINE;
        case LUA_HOOKCOUNT:
            return LUA_MASKCOUNT;
        default:
            return 0;
    }
}

int stackDepth(lua_State* L) {
    int depth = 0;
    lua_Debug frame;
    while (lua_getstack(L, depth, &frame)) ++depth;
    return depth;
}

bool isAsciiAlpha(char character) noexcept {
    return (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z');
}

void asciiCaseFold(std::string& source) {
    for (char& character : source) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
}

std::string normalizeSource(std::string source) {
    if (!source.empty() && (source.front() == '@' || source.front() == '=')) {
        source.erase(source.begin());
    }
    for (char& character : source) {
        if (character == '\\') character = '/';
    }

    if (source.empty()) return {};

    std::string root;
    std::size_t cursor = 0;
    std::size_t protectedSegments = 0;
    bool absolute = false;
    bool windowsStyle = false;

    if (source.size() >= 2 && isAsciiAlpha(source[0]) && source[1] == ':') {
        root.assign(source, 0, 2);
        cursor = 2;
        windowsStyle = true;
        if (cursor < source.size() && source[cursor] == '/') {
            root += '/';
            absolute = true;
            while (cursor < source.size() && source[cursor] == '/') ++cursor;
        }
    } else if (source.size() >= 2 && source[0] == '/' && source[1] == '/') {
        root = "//";
        cursor = 2;
        absolute = true;
        windowsStyle = true;
        protectedSegments = 2; // UNC server and share cannot be escaped by '..'.
        while (cursor < source.size() && source[cursor] == '/') ++cursor;
    } else if (source.front() == '/') {
        root = "/";
        cursor = 1;
        absolute = true;
        while (cursor < source.size() && source[cursor] == '/') ++cursor;
    }

    std::vector<std::string> segments;
    while (cursor <= source.size()) {
        const std::size_t separator = source.find('/', cursor);
        const std::size_t end = separator == std::string::npos
                                    ? source.size()
                                    : separator;
        const std::string segment = source.substr(cursor, end - cursor);
        if (!segment.empty() && segment != ".") {
            if (segment == "..") {
                if (segments.size() > protectedSegments &&
                    segments.back() != "..") {
                    segments.pop_back();
                } else if (!absolute) {
                    segments.push_back(segment);
                }
            } else {
                segments.push_back(segment);
            }
        }
        if (separator == std::string::npos) break;
        cursor = separator + 1;
    }

    std::string result = root;
    for (const std::string& segment : segments) {
        const bool driveRelativeRoot = result.size() == 2 && result[1] == ':';
        if (!result.empty() && result.back() != '/' && !driveRelativeRoot) {
            result += '/';
        }
        result += segment;
    }
    if (result.empty()) result = ".";

#ifdef _WIN32
    windowsStyle = true;
#endif
    if (windowsStyle) asciiCaseFold(result);
    return result;
}

bool isRootedSource(const std::string& source) noexcept {
    return (!source.empty() && source.front() == '/') ||
           (source.size() >= 2 && isAsciiAlpha(source[0]) && source[1] == ':');
}

std::string sourceIdentity(const std::string& source,
                           const std::string& sourceRoot) {
    std::string canonical = normalizeSource(source);
    if (canonical.empty() || sourceRoot.empty() || isRootedSource(canonical)) {
        return canonical;
    }
    return normalizeSource(sourceRoot + '/' + canonical);
}

std::string breakpointKey(const std::string& file, int line,
                          const std::string& sourceRoot) {
    return sourceIdentity(file, sourceRoot) + ":" + std::to_string(line);
}

class LuaStackGuard {
public:
    explicit LuaStackGuard(lua_State* L) noexcept
        : m_L(L), m_top(lua_gettop(L)) {}

    ~LuaStackGuard() {
        lua_settop(m_L, m_top);
    }

    LuaStackGuard(const LuaStackGuard&) = delete;
    LuaStackGuard& operator=(const LuaStackGuard&) = delete;

private:
    lua_State* m_L;
    int m_top;
};

} // namespace

struct DebugProtocol::CommandMailbox {
    bool post(PauseId pauseId, Command command) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!open || pauseId == NoPause || pauseId != activePauseId ||
            !acceptingResume || !pending.empty()) {
            return false;
        }
        pending.push_back(command);
        return true;
    }

    void beginPause(PauseId pauseId) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!open) return;
        pending.clear();
        activePauseId = pauseId;
        acceptingResume = true;
    }

    std::vector<Command> drain() {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<Command> result(pending.begin(), pending.end());
        pending.clear();
        if (!result.empty()) acceptingResume = false;
        return result;
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex);
        open = false;
        acceptingResume = false;
        activePauseId = NoPause;
        pending.clear();
    }

    std::mutex mutex;
    std::deque<Command> pending;
    PauseId activePauseId = NoPause;
    bool open = true;
    bool acceptingResume = false;
};

DebugProtocol::DebugProtocol(HotReload& hotReload) noexcept
    : m_hotReload(hotReload) {}

DebugProtocol::~DebugProtocol() {
    if (!shutdown()) std::terminate();
}

bool DebugProtocol::init(lua_State* L) {
    if (!L) return false;

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_initialized) return m_L == L;
    }

    const lua_Hook currentHook = lua_gethook(L);
    const int currentMask = lua_gethookmask(L);
    const int currentCount = lua_gethookcount(L);
    DebugHookContext* context = findHookContext(L);

    if (!context) {
        if (currentHook == hookCallback) return false;
        context = createHookContext(L);
        context->previousHook = currentHook;
        context->previousMask = currentMask;
        context->previousCount = currentCount;
    } else {
        // Existing child coroutines can retain the dispatcher after shutdown.
        // Reuse its shared context only when the main state's base hook matches.
        if (context->activeProtocol || currentHook == hookCallback ||
            context->previousHook != currentHook ||
            context->previousMask != currentMask ||
            context->previousCount != currentCount) {
            return false;
        }
    }

    std::error_code sourceRootError;
    const std::filesystem::path workingDirectory =
        std::filesystem::current_path(sourceRootError);
    std::string sourceRoot;
    if (!sourceRootError) {
        // Lua/editor source identifiers are UTF-8. The native working directory
        // can contain characters outside the Windows ANSI code page.
        const auto utf8 = workingDirectory.generic_u8string();
        sourceRoot = normalizeSource(std::string(
            reinterpret_cast<const char*>(utf8.data()), utf8.size()));
    }

    auto mailbox = std::make_shared<CommandMailbox>();
    clearPausedThreadAnchor(L);

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_L = L;
        m_pausedL = nullptr;
        m_initialized = true;
        m_ownerThread = std::this_thread::get_id();
        m_mailbox = std::move(mailbox);
        m_sourceRoot = std::move(sourceRoot);
        m_runState = RunState::Running;
        m_currentSource.clear();
        m_currentLine = 0;
        m_pauseDepth = 0;
        m_pauseId = NoPause;
        m_nextPauseId = 1;
        m_nonYieldableHits = 0;
        m_stepMode = StepMode::None;
        m_stepDepth = 0;
        m_stepThread = nullptr;
    }

    context->activeProtocol = this;
    lua_sethook(L, hookCallback, context->previousMask | LUA_MASKLINE,
                context->previousCount);

    DEBUG_INFO(SubSys::Dbg, ErrCode::Ok,
               "DebugProtocol initialized -- non-blocking composite Lua hook registered.");
    return true;
}

bool DebugProtocol::shutdown() {
    std::shared_ptr<CommandMailbox> mailbox;
    lua_State* mainState = nullptr;
    bool initialized = false;
    bool wasDebugActive = false;

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_initialized && m_ownerThread != std::this_thread::get_id()) {
            return false;
        }
        mailbox = std::move(m_mailbox);
        mainState = m_L;
        initialized = m_initialized;
        wasDebugActive = m_runState == RunState::Paused ||
                         m_runState == RunState::ResumePending;
    }
    if (mailbox) mailbox->close();

    bool ownedHookContext = false;
    if (initialized && mainState) {
        DebugHookContext* context = findHookContext(mainState);
        if (context && context->activeProtocol == this) {
            ownedHookContext = true;
            // Child coroutines lazily restore this base hook the next time the
            // dispatcher runs; the shared registry context therefore outlives us.
            context->activeProtocol = nullptr;
            if (lua_gethook(mainState) == hookCallback) {
                lua_sethook(mainState, context->previousHook,
                            context->previousMask, context->previousCount);
            }
            clearPausedThreadAnchor(mainState);
        }
    }

    if (ownedHookContext && wasDebugActive &&
        m_hotReload.scriptState() == ScriptState::DEBUG_ACTIVE) {
        m_hotReload.setScriptState(ScriptState::IDLE);
    }

    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_L = nullptr;
    m_pausedL = nullptr;
    m_initialized = false;
    m_ownerThread = {};
    m_sourceRoot.clear();
    m_runState = RunState::Detached;
    m_currentSource.clear();
    m_currentLine = 0;
    m_pauseDepth = 0;
    m_pauseId = NoPause;
    m_nextPauseId = 1;
    m_stepMode = StepMode::None;
    m_stepDepth = 0;
    m_stepThread = nullptr;
    m_breakpoints.clear();
    return true;
}

// -- Breakpoint management ------------------------------------------------

std::string DebugProtocol::canonicalSourceId(const std::string& source) {
    return normalizeSource(source);
}

void DebugProtocol::setBreakpoint(const std::string& file, int line) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (!m_initialized) return;
    m_breakpoints.insert(breakpointKey(file, line, m_sourceRoot));
}

void DebugProtocol::removeBreakpoint(const std::string& file, int line) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (!m_initialized) return;
    m_breakpoints.erase(breakpointKey(file, line, m_sourceRoot));
}

void DebugProtocol::clearAllBreakpoints() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_breakpoints.clear();
}

bool DebugProtocol::hasBreakpoint(const std::string& file, int line) const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_breakpoints.count(breakpointKey(file, line, m_sourceRoot)) > 0;
}

// -- Command mailbox and execution control --------------------------------

DebugProtocol::CommandSink DebugProtocol::commandSink() const {
    std::shared_ptr<CommandMailbox> mailbox;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        mailbox = m_mailbox;
    }

    std::weak_ptr<CommandMailbox> weakMailbox = mailbox;
    return [weakMailbox](PauseId pauseId, Command command) {
        auto locked = weakMailbox.lock();
        return locked && locked->post(pauseId, command);
    };
}

bool DebugProtocol::enqueueCommand(Command command) {
    std::shared_ptr<CommandMailbox> mailbox;
    PauseId pauseId = NoPause;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        mailbox = m_mailbox;
        if (m_runState == RunState::Paused) pauseId = m_pauseId;
    }
    return mailbox && mailbox->post(pauseId, command);
}

bool DebugProtocol::stepOver() {
    return enqueueCommand(Command::StepOver);
}

bool DebugProtocol::stepInto() {
    return enqueueCommand(Command::StepInto);
}

bool DebugProtocol::stepOut() {
    return enqueueCommand(Command::StepOut);
}

bool DebugProtocol::continue_() {
    return enqueueCommand(Command::Continue);
}

void DebugProtocol::pumpCommands() {
    std::shared_ptr<CommandMailbox> mailbox;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (!isOwnerThreadLocked()) return;
        mailbox = m_mailbox;
    }
    if (!mailbox) return;

    for (Command command : mailbox->drain()) {
        if (applyCommand(command)) break;
    }
}

bool DebugProtocol::applyCommand(Command command) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (!isOwnerThreadLocked() || m_runState != RunState::Paused || !m_pausedL) {
        return false;
    }

    switch (command) {
        case Command::Continue:
            m_stepMode = StepMode::None;
            m_stepDepth = 0;
            m_stepThread = nullptr;
            break;
        case Command::StepInto:
            m_stepMode = StepMode::Into;
            m_stepDepth = m_pauseDepth;
            m_stepThread = m_pausedL;
            break;
        case Command::StepOver:
            m_stepMode = StepMode::Over;
            m_stepDepth = m_pauseDepth;
            m_stepThread = m_pausedL;
            break;
        case Command::StepOut:
            m_stepMode = StepMode::Out;
            m_stepDepth = m_pauseDepth;
            m_stepThread = m_pausedL;
            break;
    }

    m_runState = RunState::ResumePending;
    return true;
}

int DebugProtocol::resumePausedCoroutine(int* resultCount) {
    const ResumeOutcome outcome = resumePausedCoroutineImpl(false);
    if (resultCount) *resultCount = outcome.resultCount;
    return outcome.status;
}

DebugProtocol::ResumeOutcome DebugProtocol::resumePausedCoroutineManaged() {
    return resumePausedCoroutineImpl(true);
}

DebugProtocol::ResumeOutcome DebugProtocol::resumePausedCoroutineImpl(
    bool clearResults) {
    ResumeOutcome outcome;

    lua_State* coroutine = nullptr;
    lua_State* mainState = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (!isOwnerThreadLocked() || m_runState != RunState::ResumePending ||
            !m_pausedL || !m_L) {
            return outcome;
        }
        coroutine = m_pausedL;
        mainState = m_L;
        m_runState = RunState::Running;
    }

    if (m_hotReload.scriptState() == ScriptState::DEBUG_ACTIVE) {
        m_hotReload.setScriptState(ScriptState::IDLE);
    }

    outcome.status =
        lua_resume(coroutine, mainState, 0, &outcome.resultCount);

    if (clearResults) {
        if (outcome.status != LUA_OK && outcome.status != LUA_YIELD) {
            const char* error = lua_tostring(coroutine, -1);
            outcome.error =
                error ? error : "Lua resume failed with a non-string error";
        }

        const int stackTop = lua_gettop(coroutine);
        const int producedResults =
            outcome.resultCount < stackTop ? outcome.resultCount : stackTop;
        if (producedResults > 0) lua_pop(coroutine, producedResults);
    }

    bool clearAnchor = false;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_runState != RunState::Paused &&
            m_runState != RunState::ResumePending) {
            m_pausedL = nullptr;
            m_pauseDepth = 0;

            if (outcome.status != LUA_YIELD) {
                m_stepMode = StepMode::None;
                m_stepDepth = 0;
                m_stepThread = nullptr;
            }

            // A normal script yield can happen before a step completes. Keep
            // the anchor in that case so the step target remains valid.
            clearAnchor = m_stepMode == StepMode::None;
        }
    }
    if (clearAnchor) clearPausedThreadAnchor(mainState);

    return outcome;
}

bool DebugProtocol::isOwnerThreadLocked() const noexcept {
    return m_initialized && m_ownerThread == std::this_thread::get_id();
}

// -- Inspection -----------------------------------------------------------

std::string DebugProtocol::inspectLocal(int frameIndex, const std::string& name) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (!isOwnerThreadLocked()) return "<owner thread required>";

    lua_State* activeL = m_pausedL ? m_pausedL : m_L;
    if (!activeL) return "nil";
    LuaStackGuard stackGuard(activeL);

    lua_Debug frame;
    if (!lua_getstack(activeL, frameIndex, &frame)) return "<invalid frame>";

    for (int index = 1;; ++index) {
        const char* localName = lua_getlocal(activeL, &frame, index);
        if (!localName) break;
        if (!name.empty() && std::strcmp(localName, name.c_str()) == 0) {
            std::string result = formatValue(activeL, -1);
            lua_pop(activeL, 1);
            return result;
        }
        lua_pop(activeL, 1);
    }

    return "nil";
}

std::string DebugProtocol::inspectGlobal(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (!isOwnerThreadLocked()) return "<owner thread required>";

    lua_State* activeL = m_pausedL ? m_pausedL : m_L;
    if (!activeL || name.empty()) return "nil";
    LuaStackGuard stackGuard(activeL);

    // A normal global lookup can invoke _G.__index while m_stateMutex is held.
    // Read the registry's global table directly so inspection cannot execute Lua.
    lua_rawgeti(activeL, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
    lua_pushlstring(activeL, name.data(), name.size());
    lua_rawget(activeL, -2);
    std::string result = formatValue(activeL, -1);
    lua_pop(activeL, 2);
    return result;
}

std::string DebugProtocol::currentSource() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_currentSource;
}

int DebugProtocol::currentLine() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_currentLine;
}

DebugProtocol::PauseId DebugProtocol::currentPauseId() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (m_runState != RunState::Paused &&
        m_runState != RunState::ResumePending) {
        return NoPause;
    }
    return m_pauseId;
}

bool DebugProtocol::isDebugActive() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_runState == RunState::Paused ||
           m_runState == RunState::ResumePending;
}

DebugProtocol::RunState DebugProtocol::runState() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_runState;
}

std::uint64_t DebugProtocol::nonYieldableHitCount() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_nonYieldableHits;
}

// -- Hook callback --------------------------------------------------------

void DebugProtocol::hookCallback(lua_State* L, lua_Debug* ar) {
    DebugHookContext* context = findHookContext(L);
    if (!context) return;

    const lua_Hook previousHook = context->previousHook;
    const int previousMask = context->previousMask;
    const int eventMask = maskForHookEvent(ar->event);
    if (previousHook && previousHook != hookCallback &&
        (previousMask & eventMask) != 0) {
        previousHook(L, ar);
        if (lua_status(L) == LUA_YIELD) return;
    }

    DebugProtocol* activeProtocol = context->activeProtocol;
    if (!activeProtocol) {
        // Coroutines copy their hook when created. Restore their base hook on
        // first use after the protocol detaches to remove line-hook overhead.
        if (lua_gethook(L) == hookCallback) {
            lua_sethook(L, previousHook, previousMask, context->previousCount);
        }
        return;
    }

    if (ar->event == LUA_HOOKLINE && activeProtocol->onLineHook(L, ar)) {
        // Lua 5.4 permits a line hook to yield only with zero results and no
        // continuation. This must remain the final action in the dispatcher.
        lua_yield(L, 0);
        return;
    }
}

bool DebugProtocol::onLineHook(lua_State* L, lua_Debug* ar) {
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (!m_initialized || m_runState != RunState::Running) return false;
        // Fast path: no breakpoints and no step mode -- the common case for
        // a normally running game. Skip lua_getinfo/normalize/stackDepth
        // (string ops per executed line) entirely.
        if (m_breakpoints.empty() && m_stepMode == StepMode::None) return false;
    }

    if (m_hotReload.scriptState() == ScriptState::RELOADING) return false;

    lua_getinfo(L, "Sln", ar);
    const std::string source = normalizeSource(ar->source ? ar->source : "?");
    const int line = ar->currentline;
    const int depth = stackDepth(L);

    bool shouldBreak = false;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (!m_initialized || m_runState != RunState::Running) return false;

        shouldBreak =
            m_breakpoints.count(breakpointKey(source, line, m_sourceRoot)) > 0;
        if (!shouldBreak && m_stepMode != StepMode::None && m_stepThread == L) {
            switch (m_stepMode) {
                case StepMode::Into:
                    shouldBreak = true;
                    break;
                case StepMode::Over:
                    shouldBreak = depth <= m_stepDepth;
                    break;
                case StepMode::Out:
                    shouldBreak = depth < m_stepDepth;
                    break;
                case StepMode::None:
                    break;
            }
        }
    }

    if (!shouldBreak) return false;

    if (!lua_isyieldable(L)) {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_currentSource = source;
        m_currentLine = line;
        ++m_nonYieldableHits;
        std::fprintf(stderr,
                     "[Debug] Non-yieldable hit at %s:%d; execution continues.\n",
                     source.c_str(), line);
        return false;
    }

    // Publish Paused only after the registry owns the coroutine strongly and
    // the mailbox is ready to accept exactly one resume command.
    anchorPausedThread(L);
    std::shared_ptr<CommandMailbox> mailbox;
    PauseId pauseId = NoPause;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        mailbox = m_mailbox;
        pauseId = m_nextPauseId++;
        if (pauseId == NoPause) pauseId = m_nextPauseId++;
    }
    if (mailbox) mailbox->beginPause(pauseId);

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_currentSource = source;
        m_currentLine = line;
        m_pausedL = L;
        m_pauseDepth = depth;
        m_pauseId = pauseId;
        m_stepMode = StepMode::None;
        m_stepDepth = 0;
        m_stepThread = nullptr;
        m_runState = RunState::Paused;
    }

    m_hotReload.setScriptState(ScriptState::DEBUG_ACTIVE);
    std::fprintf(stderr, "[Debug] Paused at %s:%d\n", source.c_str(), line);
    return true;
}

// -- Value formatting -----------------------------------------------------

std::string DebugProtocol::formatValue(lua_State* L, int index) {
    const int type = lua_type(L, index);
    switch (type) {
        case LUA_TNIL:
            return "nil";
        case LUA_TBOOLEAN:
            return lua_toboolean(L, index) ? "true" : "false";
        case LUA_TNUMBER: {
            std::ostringstream out;
            out << lua_tonumber(L, index);
            return out.str();
        }
        case LUA_TSTRING:
            return std::string("\"") + lua_tostring(L, index) + "\"";
        case LUA_TTABLE: {
            std::ostringstream out;
            out << "table(" << lua_rawlen(L, index) << " entries)";
            return out.str();
        }
        case LUA_TFUNCTION:
            return "<function>";
        case LUA_TTHREAD:
            return "<thread>";
        case LUA_TUSERDATA:
            return "<userdata>";
        case LUA_TLIGHTUSERDATA:
            return "<lightuserdata>";
        default:
            return "<unknown>";
    }
}

} // namespace Caesura
