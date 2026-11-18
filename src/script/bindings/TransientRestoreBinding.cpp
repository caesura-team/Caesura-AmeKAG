#include "TransientRestoreBinding.h"
#include "../../di/BackendRegistry.h"
#include "../../live2d/api/IAnimationBackend.h"
#include "../../render/api/IParticleSystem.h"
#include "../../render/api/IRenderDevice.h"
#include "../../render/api/IVideoPlayer.h"
#include <cstdio>
#include <array>
#include <limits>
#include <stdexcept>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace Caesura {
namespace {
struct TransientCounts {
    lua_Integer videos = 0, particles = 0, emitters = 0, models = 0, postfx = 0;
};

int failure(lua_State* state, const char* message, bool boolean = false) {
    if (boolean) lua_pushboolean(state, false);
    else lua_pushnil(state);
    lua_pushstring(state, message);
    return 2;
}

// Reduce C++ exceptions to fixed bytes before callers report them to Lua.
// Each component gets an independent attempt; retain the first useful failure.
template<typename Action>
bool attempt(const char* component, char (&error)[256], const Action& action) {
    try {
        action();
        return true;
    } catch (const std::exception& cause) {
        if (!error[0]) std::snprintf(error, sizeof(error), "%s: %s", component, cause.what());
    } catch (...) {
        if (!error[0]) std::snprintf(error, sizeof(error), "%s failed", component);
    }
    return false;
}

int captureTransients(lua_State* state) {
    TransientCounts counts;
    char error[256] = {};
    const bool captured = attempt("Transient capture", error, [&] {
        auto& registry = BackendRegistry::instance();
        if (auto* video = registry.getVideoPlayer()) counts.videos = video->activeCount();
        if (auto* particles = registry.getParticleSystem()) {
            counts.particles = particles->aliveCount();
            counts.emitters = particles->activeEmitterCount();
        }
        if (auto* animation = registry.getAnimationBackend()) {
            const auto models = animation->loadedModelCount();
            if (models > static_cast<size_t>(std::numeric_limits<lua_Integer>::max()))
                throw std::runtime_error("Model count exceeds the Lua integer range");
            counts.models = static_cast<lua_Integer>(models);
        }
        if (auto* renderer = registry.getRenderDevice()) counts.postfx = renderer->isPostFxActive() ? 1 : 0;
        if (counts.videos < 0 || counts.particles < 0 || counts.emitters < 0)
            throw std::runtime_error("Backend reported a negative transient count");
    });
    if (!captured) return failure(state, error);
    // Only trivial counters and the fixed error buffer are live across Lua calls.
    lua_createtable(state, 0, 5);
    lua_pushinteger(state, counts.videos); lua_setfield(state, -2, "videos");
    lua_pushinteger(state, counts.particles); lua_setfield(state, -2, "particles");
    lua_pushinteger(state, counts.emitters); lua_setfield(state, -2, "emitters");
    lua_pushinteger(state, counts.models); lua_setfield(state, -2, "models");
    lua_pushinteger(state, counts.postfx); lua_setfield(state, -2, "postfx");
    return 1;
}

// SandboxQuota may itself call Lua. Run it below pcall so a quota Lua error
// cannot interrupt the remaining backend cleanup or particle reinitialization.
int releaseParticleQuota(lua_State* state) {
    char error[256] = {};
    if (attempt("Particle quota release", error, [] {
        auto& registry = BackendRegistry::instance();
        const int count = registry.count("particles_emitters");
        if (count < 0) throw std::runtime_error("Negative emitter quota");
        for (int i = 0; i < count; ++i) registry.release("particles_emitters");
    })) return 0;
    lua_pushstring(state, error);
    return lua_error(state);
}

void returnParticleQuota(lua_State* state, char (&error)[256]) {
    lua_pushcfunction(state, releaseParticleQuota);
    if (lua_pcall(state, 0, 0, 0) == LUA_OK) return;
    if (!error[0]) {
        const char* message = lua_type(state, -1) == LUA_TSTRING ? lua_tostring(state, -1) : nullptr;
        std::snprintf(error, sizeof(error), "%s", message && message[0] ? message : "Particle quota release failed");
    }
    lua_pop(state, 1);
}

void stopParticles(lua_State* state, char (&error)[256]) {
    IParticleSystem* particles = nullptr;
    bool initialized = false, cleared = false;
    if (!attempt("Particle cleanup", error, [&] {
        particles = BackendRegistry::instance().getParticleSystem();
        if (!particles) return;
        const int emitters = particles->activeEmitterCount(), alive = particles->aliveCount();
        if (emitters < 0 || alive < 0) throw std::runtime_error("Negative particle count");
        if (!emitters && !alive) return;
        initialized = particles->isInitialized();
        particles->shutdown();
        cleared = true;
    }) || !particles) return;
    // A prior quota failure can leave a residual count after particles were
    // already removed. Retry that release without restarting an idle backend.
    returnParticleQuota(state, error);
    if (cleared && initialized) attempt("Particle reinitialization", error, [&] {
        if (!particles->init()) throw std::runtime_error("Cannot reinitialize particle backend");
    });
}

int stopTransients(lua_State* state) {
    if (!lua_checkstack(state, 4)) return failure(state, "Cannot grow transient cleanup stack", true);
    std::array<int, 8> keptModels{};
    std::size_t count = 0;
    if (!lua_isnoneornil(state, 1)) {
        if (!lua_istable(state, 1) || lua_rawlen(state, 1) > keptModels.size())
            return failure(state, "Invalid model restore handles", true);
        count = lua_rawlen(state, 1);
        for (std::size_t i = 0; i < count; ++i) {
            lua_rawgeti(state, 1, static_cast<lua_Integer>(i + 1));
            const auto handle = lua_tointeger(state, -1);
            const bool valid = lua_isinteger(state, -1) && handle > 0 && handle <= std::numeric_limits<int>::max();
            lua_pop(state, 1);
            if (!valid) return failure(state, "Invalid model restore handle", true);
            keptModels[i] = static_cast<int>(handle);
        }
    }
    char error[256] = {};
    attempt("Video cleanup", error, [] {
        if (auto* video = BackendRegistry::instance().getVideoPlayer()) video->closeAll();
    });
    stopParticles(state, error);
    attempt("Model cleanup", error, [&] {
        auto* animation = BackendRegistry::instance().getAnimationBackend();
        if (animation && animation->loadedModelCount() > 0)
            animation->clearModels(std::span<const int>(keptModels.data(), count));
    });
    attempt("Postfx cleanup", error, [] {
        auto* renderer = BackendRegistry::instance().getRenderDevice();
        if (renderer && renderer->isPostFxActive()) renderer->clearPostFx();
    });
    if (error[0]) return failure(state, error, true);
    lua_pushboolean(state, true);
    return 1;
}
}

void registerTransientRestoreBinding(lua_State* state) {
    lua_getglobal(state, "Restore");
    static const luaL_Reg functions[] = {
        {"capture_transients", captureTransients}, {"stop_transients", stopTransients},
        {nullptr, nullptr}
    };
    luaL_setfuncs(state, functions, 0);
    lua_pop(state, 1);
}
}
