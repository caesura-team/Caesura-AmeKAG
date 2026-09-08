#pragma once

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <cstddef>
#include <cstring>

namespace Caesura::detail {

// Explicit owner-thread bookkeeping. Records are trivially destructible so a
// host can forget them without touching a coroutine after the VM has closed.
struct ManagedCoroutine {
    lua_State* co = nullptr;
    int reference = LUA_NOREF;
};

struct CoroutineDiagnostic {
    int status = LUA_OK;
    char message[256] = {};
};

struct CoroutineStep {
    int status = LUA_OK;
    int results = 0;
    char error[256] = {};
    CoroutineDiagnostic closed;
};

inline void copyCoroutineMessage(char* destination, std::size_t capacity,
                                 const char* text, std::size_t size) noexcept {
    if (capacity == 0) return;
    const std::size_t used = size < capacity ? size : capacity - 1;
    if (used != 0) std::memcpy(destination, text, used);
    destination[used] = '\0';
}

inline void copyCoroutineError(lua_State* state, char* destination,
                               std::size_t capacity, const char* fallback) noexcept {
    // lua_tolstring can allocate when converting a number. Only inspect an
    // existing Lua string here; never invoke a user __tostring during cleanup.
    if (lua_type(state, -1) == LUA_TSTRING) {
        std::size_t size = 0;
        const char* text = lua_tolstring(state, -1, &size);
        copyCoroutineMessage(destination, capacity, text, size);
    } else {
        copyCoroutineMessage(destination, capacity, fallback, std::strlen(fallback));
    }
}

inline CoroutineDiagnostic closeManagedCoroutine(lua_State* owner,
                                                 ManagedCoroutine& run) noexcept {
    const ManagedCoroutine held = run;
    run = {};  // Disarm before __close can re-enter host cleanup.
    CoroutineDiagnostic result;
    if (!owner || !held.co || held.reference < 0) return result;

    // Lua 5.4 closes protectedly and reports errors instead of abandoning its
    // to-be-closed stack. Keep the registry reference until this has finished.
    result.status = lua_closethread(held.co, owner);
    if (result.status != LUA_OK) {
        copyCoroutineError(held.co, result.message, sizeof(result.message),
                           "non-string coroutine close error");
    }

    // The registry is shared by all threads. Use the just-reset coroutine's
    // spare stack, not an arbitrary owner stack. luaL_unref overwrites its
    // existing reference/freelist slots; no error formatting or other Lua API
    // may dereference this coroutine after its last strong reference is gone.
    luaL_unref(held.co, LUA_REGISTRYINDEX, held.reference);
    return result;
}

namespace managed_coroutine_detail {
struct CreateContext {
    const char* script;
    ManagedCoroutine run;
    CoroutineDiagnostic result;
};

inline int createProtected(lua_State* owner) {
    auto* context = static_cast<CreateContext*>(lua_touserdata(owner, 1));
    lua_State* co = lua_newthread(owner);
    context->result.status = luaL_loadstring(co, context->script);
    if (context->result.status != LUA_OK) {
        copyCoroutineError(co, context->result.message, sizeof(context->result.message),
                           "compile error");
        (void)lua_closethread(co, owner);
        lua_pop(owner, 1);
        return 0;
    }
    // Publish the POD record only after luaL_ref succeeds. An allocation error
    // before that point is caught by the owner's pcall; no host reference leaks.
    const int reference = luaL_ref(owner, LUA_REGISTRYINDEX);
    context->run = {co, reference};
    return 0;
}
} // namespace managed_coroutine_detail

inline CoroutineDiagnostic createManagedCoroutine(lua_State* owner, const char* script,
                                                  ManagedCoroutine& run) noexcept {
    CoroutineDiagnostic result;
    if (!owner || !script || run.co || run.reference != LUA_NOREF) {
        result.status = LUA_ERRRUN;
        constexpr char message[] = "invalid managed coroutine creation state";
        copyCoroutineMessage(result.message, sizeof(result.message), message, sizeof(message) - 1);
        return result;
    }
    const int top = lua_gettop(owner);
    if (!lua_checkstack(owner, 2)) {
        result.status = LUA_ERRMEM;
        constexpr char message[] = "cannot grow managed coroutine owner stack";
        copyCoroutineMessage(result.message, sizeof(result.message), message, sizeof(message) - 1);
        return result;
    }
    managed_coroutine_detail::CreateContext context{script, {}, {}};
    lua_pushcfunction(owner, managed_coroutine_detail::createProtected);
    lua_pushlightuserdata(owner, &context);
    const int status = lua_pcall(owner, 1, 0, 0);
    if (status != LUA_OK) {
        result.status = status;
        copyCoroutineError(owner, result.message, sizeof(result.message),
                           "managed coroutine creation failed");
    } else {
        run = context.run;
        result = context.result;
    }
    // Only our pcall error result can be above this saved owner stack top.
    lua_settop(owner, top);
    return result;
}

inline CoroutineStep resumeManagedCoroutine(lua_State* owner, ManagedCoroutine& run) noexcept {
    CoroutineStep result;
    if (!owner) {
        run = {};
        result.status = LUA_ERRRUN;
        return result;
    }
    if (!run.co || run.reference < 0) {
        result.status = LUA_ERRRUN;
        return result;
    }
    result.status = lua_resume(run.co, owner, 0, &result.results);
    if (result.status == LUA_YIELD) {
        // Consume exactly the yielded values. A C continuation may retain a
        // lua_toclose guard below them; yield(0) must leave that stack intact.
        if (result.results > 0) lua_pop(run.co, result.results);
        return result;
    }
    if (result.status != LUA_OK) {
        copyCoroutineError(run.co, result.error, sizeof(result.error),
                           "non-string coroutine error");
    }
    result.closed = closeManagedCoroutine(owner, run);
    return result;
}

} // namespace Caesura::detail
