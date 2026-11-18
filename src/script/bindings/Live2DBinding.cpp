#include "Live2DBinding.h"
#include "../../di/BackendRegistry.h"
#include "../../di/api/ThreadAssert.h"
#include "../../live2d/api/IAnimationBackend.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace Caesura {
namespace {
enum class Operation { Load, Show, Hide, Unload, Motion, Expression, Mouth, Voice };

int failure(lua_State* state, bool loading, const char* reason) {
    if (loading) lua_pushnil(state);
    else lua_pushboolean(state, false);
    lua_pushstring(state, reason);
    return 2;
}

bool text(lua_State* state, int index, const char*& value, size_t& size) {
    if (lua_type(state, index) != LUA_TSTRING) return false;
    value = lua_tolstring(state, index, &size);
    return size > 0 && std::memchr(value, '\0', size) == nullptr;
}

bool number(lua_State* state, int index, float& value, bool optional = false) {
    if (optional && lua_isnoneornil(state, index)) return true;
    if (lua_type(state, index) != LUA_TNUMBER) return false;
    const lua_Number input = lua_tonumber(state, index);
    if (!std::isfinite(input) || std::abs(input) > std::numeric_limits<float>::max()) return false;
    value = static_cast<float>(input);
    return true;
}

int invoke(lua_State* state, Operation operation) {
    CAESURA_ASSERT_MAIN_THREAD();
    const bool loading = operation == Operation::Load;
    const char* path = nullptr;
    const char* name = nullptr;
    size_t pathSize = 0, nameSize = 0;
    int handle = 0;
    float x = 0, y = 0, scale = 1, mouth = 0;
    float fadeIn = 0.5f, fadeOut = 0.5f, weight = 1.0f;
    bool enabled = false;
    if (loading) {
        if (!text(state, 1, path, pathSize) || !text(state, 2, name, nameSize))
            return failure(state, true, "Live2D load requires nonempty path and name without NUL");
    } else {
        int valid = 0;
        const lua_Integer input = lua_tointegerx(state, 1, &valid);
        if (lua_type(state, 1) != LUA_TNUMBER || !valid || input <= 0
            || input > std::numeric_limits<int>::max())
            return failure(state, false, "Invalid Live2D model handle");
        handle = static_cast<int>(input);
    }
    if (operation == Operation::Show && (!number(state, 2, x, true)
        || !number(state, 3, y, true) || !number(state, 4, scale, true) || scale <= 0))
        return failure(state, false, "Live2D show requires finite coordinates and positive scale");
    if (operation == Operation::Motion && (!text(state, 2, name, nameSize)
        || !number(state, 3, fadeIn, true) || !number(state, 4, fadeOut, true)
        || fadeIn < 0 || fadeIn > 10 || fadeOut < 0 || fadeOut > 10))
        return failure(state, false, "Live2D motion requires a name and fade durations within 0..10 seconds");
    if (operation == Operation::Expression && (!text(state, 2, name, nameSize)
        || !number(state, 3, weight, true) || weight < 0 || weight > 1))
        return failure(state, false, "Live2D expression requires a name and weight within 0..1");
    if (operation == Operation::Mouth && (!number(state, 2, mouth) || mouth < 0 || mouth > 1))
        return failure(state, false, "Live2D mouth value must be finite and within 0..1");
    if (operation == Operation::Voice) {
        if (lua_type(state, 2) != LUA_TBOOLEAN)
            return failure(state, false, "Live2D voice mode requires a boolean");
        enabled = lua_toboolean(state, 2) != 0;
    }
    // C++ temporaries and exceptions leave this scope before pushing to Lua.
    char error[256] = {};
    try {
        auto* animation = BackendRegistry::instance().getAnimationBackend();
        if (!animation || !animation->isCubismAvailable())
            throw std::runtime_error("Cubism animation backend is unavailable");
        if (!loading && !animation->isLoaded(handle))
            throw std::runtime_error("Live2D model handle is not loaded");
        switch (operation) {
        case Operation::Load:
            handle = animation->loadModel(std::string(path, pathSize), std::string(name, nameSize));
            if (handle <= 0) throw std::runtime_error("Live2D model load failed");
            break;
        case Operation::Show: animation->showModel(handle, x, y, scale); break;
        case Operation::Hide: animation->hideModel(handle); break;
        case Operation::Unload: animation->unloadModel(handle); break;
        case Operation::Motion:
            if (!animation->playMotion(handle, std::string(name, nameSize), fadeIn, fadeOut))
                throw std::runtime_error("Live2D motion could not start");
            break;
        case Operation::Expression:
            if (!animation->setExpression(handle, std::string(name, nameSize), weight))
                throw std::runtime_error("Live2D expression could not be applied");
            break;
        case Operation::Mouth:
            if (!animation->setVoiceLipSync(handle, false))
                throw std::runtime_error("Live2D model has no supported mouth parameter");
            animation->setParameter(handle, "ParamMouthOpenY", mouth);
            break;
        case Operation::Voice:
            if (!animation->setVoiceLipSync(handle, enabled))
                throw std::runtime_error("Live2D voice lip sync is unsupported for this model");
            break;
        }
    } catch (const std::exception& cause) {
        std::snprintf(error, sizeof(error), "%s", cause.what());
    } catch (...) {
        std::snprintf(error, sizeof(error), "Live2D backend operation failed");
    }
    if (error[0]) return failure(state, loading, error);
    if (loading) lua_pushinteger(state, handle);
    else lua_pushboolean(state, true);
    return 1;
}

int load(lua_State* state) { return invoke(state, Operation::Load); }
int show(lua_State* state) { return invoke(state, Operation::Show); }
int hide(lua_State* state) { return invoke(state, Operation::Hide); }
int unload(lua_State* state) { return invoke(state, Operation::Unload); }
int motion(lua_State* state) { return invoke(state, Operation::Motion); }
int expression(lua_State* state) { return invoke(state, Operation::Expression); }
int mouth(lua_State* state) { return invoke(state, Operation::Mouth); }
int voice(lua_State* state) { return invoke(state, Operation::Voice); }
}

void registerLive2DBinding(lua_State* state) {
    static const luaL_Reg methods[] = {
        {"load", load}, {"show", show}, {"hide", hide}, {"unload", unload},
        {"play_motion", motion}, {"set_expression", expression},
        {"set_mouth", mouth}, {"set_voice_lipsync", voice}, {nullptr, nullptr}
    };
    luaL_newlib(state, methods);
    lua_setglobal(state, "Live2D");
}
}
